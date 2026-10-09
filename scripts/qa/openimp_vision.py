#!/usr/bin/env python3
"""openimp_vision.py - image metrics for timps-qa-openimp.sh (FrontCrop / ROI / ISP).

Pure Python (no numpy needed; the QA host may not have it). Images are read as
PGM/PPM directly, anything else (JPEG) is converted by ffmpeg, else PIL.

Subcommands print ONE line of key=value pairs, last pair verdict=..., so the
shell can grep them:

  crop   --before B --after A --win x,y,w,h --sensor W,H [--alt x,y,w,h]
         [--expect crop|full]
  roi    --off F.. --on F.. --rect x,y,w,h --dir blur|sharp [--delta D] [--rc cbr|..]
  lift   --lo L --hi H --lo2 L2 [--min 8]   ISP key effect on mean luma
  region --img F [--align 16] [--frac 0.34]
  stats  F                       mean luma, saturation, spatial std
  noise  F F [F..]               temporal noise (frame-difference std)
  stable A B                     AE settle check between two grabs

Thresholds (also in the timps-qa-openimp.sh header):
  NCC_MIN     0.85  after-image vs the matching sub-window of the before-image
  NCC_MARGIN  0.10  that NCC must beat the whole-image and alt-window NCC by this
  ROI_BLUR    0.85  rel sharpness (in/out, ON vs OFF) at or below = blurred
  ROI_SHARP   1.05  rel sharpness at or above = sharpened
  With --delta the blur bar is 1 - 0.15 * min(|D|, 20) / 20. No measurable
  effect is WARN instead of NOEFFECT when |D| < ROI_SMALL_DELTA (16: the
  encoder clamps the QP table, T21 +13/-12), or for blur under --rc cbr when
  rel is within ROI_CBR_BAND (0.10) of the bar (CBR hands the saved bits back).
  lift: effect = hi - (lo + lo2) / 2 with lo2 measured after hi at the low
  value again; |lo2 - lo| > max(LIFT_MIN, |hi - lo| / 3) = the scene changed
  (day/night switch, IR probe) = INCONCLUSIVE; effect >= LIFT_MIN PASS,
  <= -LIFT_MIN FAIL, else WARN.
"""
import argparse
import subprocess
import sys

NCC_MIN = 0.85
NCC_MARGIN = 0.10
ROI_BLUR = 0.85
ROI_SHARP = 1.05
ROI_SMALL_DELTA = 16
ROI_CBR_BAND = 0.10
LIFT_MIN = 8.0
FLAT_STD = 4.0          # a before-image flatter than this cannot prove a crop
GRID_W = 64


class Img:
    __slots__ = ("w", "h", "px", "rgb")

    def __init__(self, w, h, px, rgb=None):
        self.w, self.h, self.px, self.rgb = w, h, px, rgb


def _parse_pnm(data):
    # P5/P6, maxval <= 255
    fields, pos = [], 2
    magic = data[:2]
    while len(fields) < 3:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while data[pos:pos + 1] not in (b"\n", b""):
                pos += 1
            continue
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1
    w, h, maxv = fields
    if maxv > 255:
        raise ValueError("16-bit PNM not supported")
    if magic == b"P5":
        return Img(w, h, bytearray(data[pos:pos + w * h]))
    if magic == b"P6":
        rgb = data[pos:pos + 3 * w * h]
        r, g, b = rgb[0::3], rgb[1::3], rgb[2::3]
        y = bytearray((77 * r[i] + 150 * g[i] + 29 * b[i]) >> 8 for i in range(w * h))
        return Img(w, h, y, rgb)
    raise ValueError("not a P5/P6 file")


def load(path):
    with open(path, "rb") as f:
        head = f.read(2)
    if head in (b"P5", b"P6"):
        with open(path, "rb") as f:
            return _parse_pnm(f.read())
    try:
        out = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-frames:v", "1",
                              "-f", "image2pipe", "-c:v", "ppm", "-"],
                             capture_output=True, timeout=30, check=True).stdout
        return _parse_pnm(out)
    except (OSError, subprocess.SubprocessError):
        pass
    from PIL import Image  # last resort
    im = Image.open(path).convert("RGB")
    w, h = im.size
    raw = im.tobytes()
    return _parse_pnm(b"P6\n%d %d\n255\n" % (w, h) + raw)


def save_pgm(img, path):
    with open(path, "wb") as f:
        f.write(b"P5\n%d %d\n255\n" % (img.w, img.h))
        f.write(bytes(img.px))


def crop(img, x, y, w, h):
    x, y = max(0, x), max(0, y)
    w, h = min(w, img.w - x), min(h, img.h - y)
    px = bytearray()
    for r in range(y, y + h):
        px += img.px[r * img.w + x: r * img.w + x + w]
    return Img(w, h, px)


def resample(img, gw, gh):
    """Box-average to gw x gh (area mean; robust to scaler/JPEG differences)."""
    out = [0.0] * (gw * gh)
    xb = [(i * img.w) // gw for i in range(gw + 1)]
    yb = [(j * img.h) // gh for j in range(gh + 1)]
    for j in range(gh):
        y0, y1 = yb[j], max(yb[j + 1], yb[j] + 1)
        acc = [0] * gw
        for r in range(y0, min(y1, img.h)):
            row = img.px[r * img.w:(r + 1) * img.w]
            for i in range(gw):
                x0, x1 = xb[i], max(xb[i + 1], xb[i] + 1)
                acc[i] += sum(row[x0:x1])
        for i in range(gw):
            n = (max(xb[i + 1], xb[i] + 1) - xb[i]) * (min(y1, img.h) - y0)
            out[j * gw + i] = acc[i] / max(n, 1)
    return out


def _mean_std(v):
    n = len(v)
    m = sum(v) / n
    var = sum((a - m) * (a - m) for a in v) / n
    return m, var ** 0.5


def ncc(a, b):
    ma, sa = _mean_std(a)
    mb, sb = _mean_std(b)
    if sa < 1e-6 or sb < 1e-6:
        return 0.0
    return sum((x - ma) * (y - mb) for x, y in zip(a, b)) / (len(a) * sa * sb)


def lap_abs(img):
    """|4-neighbour Laplacian| per pixel (border = 0), as a flat list."""
    w, h, p = img.w, img.h, img.px
    out = [0] * (w * h)
    for y in range(1, h - 1):
        o = y * w
        up, mid, dn = p[o - w:o], p[o:o + w], p[o + w:o + 2 * w]
        row = [abs(4 * mid[x] - mid[x - 1] - mid[x + 1] - up[x] - dn[x]) for x in range(1, w - 1)]
        out[o + 1:o + w - 1] = row
    return out


def sharp_in_out(img, rect):
    """Laplacian variance (mean squared response) inside / outside rect."""
    x, y, w, h = rect
    lap = lap_abs(img)
    si = so = 0.0
    ni = no = 0
    for r in range(1, img.h - 1):
        row = lap[r * img.w + 1:(r + 1) * img.w - 1]
        sq = sum(v * v for v in row)
        if y <= r < y + h:
            a, b = max(x, 1) - 1, min(x + w, img.w - 1) - 1
            ins = sum(v * v for v in row[a:b])
            si += ins
            ni += max(b - a, 0)
            so += sq - ins
            no += len(row) - max(b - a, 0)
        else:
            so += sq
            no += len(row)
    return si / max(ni, 1), so / max(no, 1)


def median(v):
    s = sorted(v)
    n = len(s)
    return s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])


def rect_arg(s):
    v = [int(float(t)) for t in s.split(",")]
    if len(v) != 4:
        raise argparse.ArgumentTypeError("need x,y,w,h")
    return v


def pair_arg(s):
    v = [int(float(t)) for t in s.split(",")]
    if len(v) != 2:
        raise argparse.ArgumentTypeError("need W,H")
    return v


def emit(**kw):
    parts = []
    for k, v in kw.items():
        if isinstance(v, float):
            v = "%.3f" % v
        v = str(v)
        parts.append("%s=%s" % (k, '"%s"' % v if " " in v else v))
    print(" ".join(parts))


# --------------------------------------------------------------------- crop
def crop_check(before, after, win, sensor, alt=None, expect="crop"):
    """Does `after` show the sensor window `win` of `before` (full FOV)?"""
    sx, sy = before.w / sensor[0], before.h / sensor[1]
    x, y, w, h = win
    sub = crop(before, round(x * sx), round(y * sy), round(w * sx), round(h * sy))
    gw = GRID_W
    gh = max(8, round(gw * after.h / after.w))
    ga = resample(after, gw, gh)
    n_win = ncc(ga, resample(sub, gw, gh))
    n_full = ncc(ga, resample(before, gw, gh))
    n_alt = None
    if alt:
        ax, ay, aw, ah = alt
        n_alt = ncc(ga, resample(crop(before, round(ax * sx), round(ay * sy),
                                      round(aw * sx), round(ah * sy)), gw, gh))
    _, bstd = _mean_std(resample(before, gw, gh))
    res = dict(ncc_win=n_win, ncc_full=n_full)
    if n_alt is not None:
        res["ncc_alt"] = n_alt
    res["size_before"] = "%dx%d" % (before.w, before.h)
    res["size_after"] = "%dx%d" % (after.w, after.h)
    if (before.w, before.h) != (after.w, after.h):
        return res, "FAIL", "stream size changed by the crop"
    if bstd < FLAT_STD:
        return res, "INCONCLUSIVE", "scene too flat (std %.1f) to tell crops apart" % bstd
    if expect == "full":
        if n_full >= NCC_MIN and n_full >= n_win + NCC_MARGIN:
            return res, "PASS", "full picture kept (stream not zoomable)"
        return res, "FAIL", "stream changed although caps says it is not zoomable"
    others = [n_full] + ([n_alt] if n_alt is not None else [])
    if n_win >= NCC_MIN and all(n_win >= o + NCC_MARGIN for o in others):
        return res, "PASS", "after-image matches the requested window"
    if n_full >= n_win:
        return res, "FAIL", "after-image looks like the uncropped picture"
    if n_alt is not None and n_alt >= n_win:
        return res, "FAIL", "after-image matches the OTHER window"
    return res, "FAIL", "after-image does not match the window (ncc %.2f < %.2f or margin < %.2f)" % (
        n_win, NCC_MIN, NCC_MARGIN)


# ---------------------------------------------------------------------- roi
def blur_bar(delta):
    if delta is None:
        return ROI_BLUR
    return 1.0 - (1.0 - ROI_BLUR) * min(abs(delta), 20) / 20.0


def roi_check(offs, ons, rect, direction, delta=None, rc=None):
    oi = [sharp_in_out(f, rect) for f in offs]
    ni = [sharp_in_out(f, rect) for f in ons]
    off_in, off_out = median([a for a, _ in oi]), median([b for _, b in oi])
    on_in, on_out = median([a for a, _ in ni]), median([b for _, b in ni])
    r_in = on_in / off_in if off_in > 0 else 0.0
    r_out = on_out / off_out if off_out > 0 else 0.0
    rel = r_in / r_out if r_out > 0 else 0.0
    res = dict(off_in=off_in, on_in=on_in, off_out=off_out, on_out=on_out,
               ratio_in=r_in, ratio_out=r_out, rel=rel)
    if off_in < 1.0:
        return res, "INCONCLUSIVE", "region has no texture"
    small = delta is not None and abs(delta) < ROI_SMALL_DELTA
    if direction == "blur":
        bar = blur_bar(delta)
        if rel <= bar and r_in < 1.0:
            return res, "PASS", "region blurred (rel %.2f <= %.2f)" % (rel, bar)
        if rel >= ROI_SHARP:
            return res, "FAIL", "region got SHARPER with a positive delta (rel %.2f)" % rel
        why = "no measurable blur (rel %.2f > %.2f)" % (rel, bar)
        if small:
            return res, "WARN", why + " - effective delta %+d too small to grade" % delta
        if rc == "cbr" and rel <= bar + ROI_CBR_BAND and r_in < 1.0:
            return res, "WARN", why + " - weak blur under CBR"
        return res, "NOEFFECT", why
    if rel >= ROI_SHARP and r_in > 1.0:
        return res, "PASS", "region sharpened (rel %.2f >= %.2f)" % (rel, ROI_SHARP)
    if rel <= ROI_BLUR:
        return res, "FAIL", "region got BLURRIER with a negative delta (rel %.2f)" % rel
    why = "no measurable sharpening (rel %.2f < %.2f)" % (rel, ROI_SHARP)
    if small:
        return res, "WARN", why + " - effective delta %+d too small to grade" % delta
    return res, "NOEFFECT", why


def lift_grade(lo, hi, lo2, min_d=LIFT_MIN):
    eff = hi - (lo + lo2) / 2.0
    drift = abs(lo2 - lo)
    res = dict(effect=eff, drift=drift)
    if drift > max(min_d, abs(hi - lo) / 3.0):
        return res, "INCONCLUSIVE", "scene changed during the test (low value %.1f, then %.1f)" % (lo, lo2)
    if eff >= min_d:
        return res, "PASS", "mean luma %.1f -> %.1f -> %.1f (%+.1f >= %g)" % (lo, hi, lo2, eff, min_d)
    if eff <= -min_d:
        return res, "FAIL", "mean luma FELL %.1f -> %.1f -> %.1f (%+.1f)" % (lo, hi, lo2, eff)
    return res, "WARN", "no measurable luma change %.1f -> %.1f -> %.1f" % (lo, hi, lo2)


def pick_region(img, align=16, frac=0.34):
    """Aligned box of ~frac x frac of the picture with the most gradient energy."""
    bw = align
    cols, rows = img.w // bw, img.h // bw
    lap = lap_abs(img)
    cell = [[0.0] * cols for _ in range(rows)]
    for r in range(rows * bw):
        row = lap[r * img.w:(r * img.w) + cols * bw]
        cr = cell[r // bw]
        for c in range(cols):
            cr[c] += sum(row[c * bw:(c + 1) * bw])
    rw = max(2, round(cols * frac))
    rh = max(2, round(rows * frac))
    best, bx, by = -1.0, 0, 0
    # keep one cell away from the border: the Laplacian there is 0
    for y in range(1, max(2, rows - rh)):
        for x in range(1, max(2, cols - rw)):
            e = sum(sum(cell[y + j][x:x + rw]) for j in range(rh))
            if e > best:
                best, bx, by = e, x, y
    total = sum(sum(r) for r in cell)
    share = best / total if total > 0 else 0.0
    return [bx * bw, by * bw, rw * bw, rh * bw], share


def stats(img):
    m, s = _mean_std(img.px)
    sat = None
    if img.rgb is not None:
        r, g, b = img.rgb[0::3], img.rgb[1::3], img.rgb[2::3]
        sat = sum(max(a, c, d) - min(a, c, d) for a, c, d in zip(r, g, b)) / len(r)
    return m, s, sat


def tnoise(frames):
    vals = []
    for a, b in zip(frames, frames[1:]):
        d = [x - y for x, y in zip(a.px, b.px)]
        vals.append(_mean_std(d)[1] / 2 ** 0.5)
    return median(vals)


def stable(a, b):
    ga, gb = resample(a, GRID_W, 36), resample(b, GRID_W, 36)
    mad = sum(abs(x - y) for x, y in zip(ga, gb)) / len(ga)
    dm = abs(sum(ga) - sum(gb)) / len(ga)
    return mad, dm, int(mad < 3.0 and dm < 2.0)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sp = ap.add_subparsers(dest="cmd", required=True)
    p = sp.add_parser("crop")
    p.add_argument("--before", required=True)
    p.add_argument("--after", required=True)
    p.add_argument("--win", type=rect_arg, required=True)
    p.add_argument("--sensor", type=pair_arg, required=True)
    p.add_argument("--alt", type=rect_arg)
    p.add_argument("--expect", choices=("crop", "full"), default="crop")
    p = sp.add_parser("roi")
    p.add_argument("--off", nargs="+", required=True)
    p.add_argument("--on", nargs="+", required=True)
    p.add_argument("--rect", type=rect_arg, required=True)
    p.add_argument("--dir", choices=("blur", "sharp"), required=True)
    p.add_argument("--delta", type=int)
    p.add_argument("--rc")
    p = sp.add_parser("lift")
    p.add_argument("--lo", type=float, required=True)
    p.add_argument("--hi", type=float, required=True)
    p.add_argument("--lo2", type=float, required=True)
    p.add_argument("--min", type=float, default=LIFT_MIN)
    p = sp.add_parser("region")
    p.add_argument("--img", required=True)
    p.add_argument("--align", type=int, default=16)
    p.add_argument("--frac", type=float, default=0.34)
    p = sp.add_parser("stats")
    p.add_argument("img")
    p = sp.add_parser("noise")
    p.add_argument("imgs", nargs="+")
    p = sp.add_parser("stable")
    p.add_argument("a")
    p.add_argument("b")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "crop":
            res, v, why = crop_check(load(a.before), load(a.after), a.win, a.sensor,
                                     a.alt, a.expect)
            emit(**res, verdict=v, reason=why)
        elif a.cmd == "roi":
            res, v, why = roi_check([load(f) for f in a.off], [load(f) for f in a.on],
                                    a.rect, a.dir, a.delta, a.rc)
            emit(**res, verdict=v, reason=why)
        elif a.cmd == "lift":
            res, v, why = lift_grade(a.lo, a.hi, a.lo2, a.min)
            emit(**res, verdict=v, reason=why)
        elif a.cmd == "region":
            img = load(a.img)
            r, share = pick_region(img, a.align, a.frac)
            emit(x=r[0], y=r[1], w=r[2], h=r[3], share=share, size="%dx%d" % (img.w, img.h))
        elif a.cmd == "stats":
            m, s, sat = stats(load(a.img))
            emit(mean=m, std=s, sat=-1.0 if sat is None else sat)
        elif a.cmd == "noise":
            emit(tnoise=tnoise([load(f) for f in a.imgs]))
        elif a.cmd == "stable":
            mad, dm, ok = stable(load(a.a), load(a.b))
            emit(mad=mad, dmean=dm, stable=ok)
    except (OSError, ValueError, ImportError, subprocess.SubprocessError) as e:
        emit(verdict="ERROR", reason=str(e).replace("\n", " ")[:200])
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
