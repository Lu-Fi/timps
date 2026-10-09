#!/usr/bin/env python3
"""Synthetic-image tests for openimp_vision.py (no camera, no numpy).

    python3 scripts/qa/test_openimp_vision.py        (or: -m unittest)

Proves the metrics detect a FrontCrop window, reject an uncropped / wrong-window
/ unrelated picture, see a blurred and a sharpened ROI, pick the textured box,
and (with ffmpeg + libx264) see a real encoder ROI QP offset.
"""
import contextlib
import io
import math
import os
import random
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import openimp_vision as ov  # noqa: E402

W, H = 640, 360


def value_noise(w, h, seed, scales=(64, 24, 8, 3), amps=(70, 40, 25, 12)):
    rnd = random.Random(seed)
    acc = [0.0] * (w * h)
    for sc, amp in zip(scales, amps):
        gw, gh = w // sc + 2, h // sc + 2
        g = [rnd.uniform(-1, 1) for _ in range(gw * gh)]
        for y in range(h):
            fy = y / sc
            y0 = int(fy)
            ty = fy - y0
            for x in range(w):
                fx = x / sc
                x0 = int(fx)
                tx = fx - x0
                a = g[y0 * gw + x0] * (1 - tx) + g[y0 * gw + x0 + 1] * tx
                b = g[(y0 + 1) * gw + x0] * (1 - tx) + g[(y0 + 1) * gw + x0 + 1] * tx
                acc[y * w + x] += amp * (a * (1 - ty) + b * ty)
    return acc


def to_img(vals, w, h, gain=1.0, off=0.0, noise=0.0, seed=1):
    rnd = random.Random(seed)
    px = bytearray(max(0, min(255, int(128 + off + gain * v + (rnd.gauss(0, noise) if noise else 0))))
                   for v in vals)
    return ov.Img(w, h, px)


def scale_bilinear(img, w, h):
    out = bytearray(w * h)
    for y in range(h):
        fy = (y + 0.5) * img.h / h - 0.5
        y0 = max(0, min(img.h - 2, int(math.floor(fy))))
        ty = min(1.0, max(0.0, fy - y0))
        for x in range(w):
            fx = (x + 0.5) * img.w / w - 0.5
            x0 = max(0, min(img.w - 2, int(math.floor(fx))))
            tx = min(1.0, max(0.0, fx - x0))
            p = img.px
            a = p[y0 * img.w + x0] * (1 - tx) + p[y0 * img.w + x0 + 1] * tx
            b = p[(y0 + 1) * img.w + x0] * (1 - tx) + p[(y0 + 1) * img.w + x0 + 1] * tx
            out[y * w + x] = int(a * (1 - ty) + b * ty + 0.5)
    return ov.Img(w, h, out)


def box_blur_rect(img, rect, k=3, passes=2):
    x0, y0, w, h = rect
    px = bytearray(img.px)
    for _ in range(passes):
        src = bytes(px)
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                s = n = 0
                for dy in range(-k, k + 1):
                    yy = min(img.h - 1, max(0, y + dy))
                    row = yy * img.w
                    for dx in range(-k, k + 1):
                        s += src[row + min(img.w - 1, max(0, x + dx))]
                        n += 1
                px[y * img.w + x] = s // n
    return ov.Img(img.w, img.h, px)


def sharpen_rect(img, rect, amount=1.0):
    blurred = box_blur_rect(img, rect, k=1, passes=1)
    px = bytearray(img.px)
    x0, y0, w, h = rect
    for y in range(y0, y0 + h):
        for x in range(x0, x0 + w):
            i = y * img.w + x
            px[i] = max(0, min(255, int(img.px[i] + amount * (img.px[i] - blurred.px[i]))))
    return ov.Img(img.w, img.h, px)


class Base(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.vals = value_noise(W, H, seed=7)
        cls.before = to_img(cls.vals, W, H)
        cls.other = to_img(value_noise(W, H, seed=99), W, H)


class TestCrop(Base):
    CENTER = [160, 90, 320, 180]
    TOPLEFT = [0, 0, 320, 180]

    def zoom(self, win, gain=1.0, off=0.0, noise=0.0):
        sub = ov.crop(self.before, *win)
        z = scale_bilinear(sub, W, H)
        # AE gain/offset change + sensor noise after the crop
        return to_img([p - 128 for p in z.px], W, H, gain, off, noise, seed=3)

    def check(self, after, win, alt=None, expect="crop", sensor=(W, H), before=None):
        return ov.crop_check(before or self.before, after, win, list(sensor), alt, expect)

    def test_center_window_detected(self):
        res, v, _ = self.check(self.zoom(self.CENTER, 1.3, -10, 4), self.CENTER, self.TOPLEFT)
        self.assertEqual(v, "PASS", res)
        self.assertGreater(res["ncc_win"], 0.9)

    def test_topleft_window_detected(self):
        res, v, _ = self.check(self.zoom(self.TOPLEFT, 0.8, 15, 4), self.TOPLEFT, self.CENTER)
        self.assertEqual(v, "PASS", res)

    def test_sensor_larger_than_stream(self):
        # stream 640x360 from a 1920x1080 sensor: window in sensor pixels
        win_s = [c * 3 for c in self.CENTER]
        res, v, _ = self.check(self.zoom(self.CENTER), win_s, sensor=(1920, 1080))
        self.assertEqual(v, "PASS", res)

    def test_uncropped_rejected(self):
        after = to_img([p - 128 for p in self.before.px], W, H, 1.1, 5, 4, seed=5)
        res, v, why = self.check(after, self.CENTER, self.TOPLEFT)
        self.assertEqual(v, "FAIL", res)
        self.assertIn("uncropped", why)

    def test_wrong_window_rejected(self):
        res, v, _ = self.check(self.zoom(self.TOPLEFT), self.CENTER, self.TOPLEFT)
        self.assertEqual(v, "FAIL", res)

    def test_unrelated_rejected(self):
        res, v, _ = self.check(self.other, self.CENTER, self.TOPLEFT)
        self.assertEqual(v, "FAIL", res)
        self.assertLess(res["ncc_win"], ov.NCC_MIN)

    def test_size_change_fails(self):
        small = scale_bilinear(self.before, 320, 180)
        _, v, why = self.check(small, self.CENTER)
        self.assertEqual(v, "FAIL")
        self.assertIn("size", why)

    def test_flat_scene_inconclusive(self):
        flat = ov.Img(W, H, bytearray([120]) * (W * H))
        _, v, _ = self.check(flat, self.CENTER, before=flat)
        self.assertEqual(v, "INCONCLUSIVE")

    def test_not_zoomable_stream_keeps_full_picture(self):
        after = to_img([p - 128 for p in self.before.px], W, H, 1.0, 0, 3, seed=9)
        res, v, _ = self.check(after, self.CENTER, expect="full")
        self.assertEqual(v, "PASS", res)
        _, v2, _ = self.check(self.zoom(self.CENTER), self.CENTER, expect="full")
        self.assertEqual(v2, "FAIL")

    def test_jpeg_roundtrip(self):
        if not shutil.which("ffmpeg"):
            self.skipTest("ffmpeg not on PATH")
        with tempfile.TemporaryDirectory() as d:
            paths = {}
            for name, img in (("b", self.before), ("a", self.zoom(self.CENTER, 1.2, 0, 3))):
                pgm = os.path.join(d, name + ".pgm")
                ov.save_pgm(img, pgm)
                jpg = os.path.join(d, name + ".jpg")
                subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", pgm, "-q:v", "8", jpg], check=True)
                paths[name] = jpg
            res, v, _ = ov.crop_check(ov.load(paths["b"]), ov.load(paths["a"]),
                                      self.CENTER, [W, H], self.TOPLEFT)
            self.assertEqual(v, "PASS", res)


class TestRoi(Base):
    RECT = [224, 112, 208, 128]

    def frames(self, fn=None, n=3):
        out = []
        for s in range(n):
            f = to_img(self.vals, W, H, noise=1.5, seed=20 + s)
            out.append(fn(f) if fn else f)
        return out

    def test_blur_detected(self):
        res, v, _ = ov.roi_check(self.frames(), self.frames(lambda f: box_blur_rect(f, self.RECT)),
                                 self.RECT, "blur")
        self.assertEqual(v, "PASS", res)
        self.assertLess(res["rel"], 0.5)

    def test_sharpen_detected(self):
        res, v, _ = ov.roi_check(self.frames(), self.frames(lambda f: sharpen_rect(f, self.RECT)),
                                 self.RECT, "sharp")
        self.assertEqual(v, "PASS", res)

    def test_no_change_is_noeffect(self):
        for d in ("blur", "sharp"):
            res, v, _ = ov.roi_check(self.frames(), self.frames(n=3), self.RECT, d)
            self.assertEqual(v, "NOEFFECT", (d, res))
            self.assertAlmostEqual(res["rel"], 1.0, delta=0.05)

    def test_wrong_direction_fails(self):
        blurred = self.frames(lambda f: box_blur_rect(f, self.RECT))
        _, v, _ = ov.roi_check(self.frames(), blurred, self.RECT, "sharp")
        self.assertEqual(v, "FAIL")

    def test_global_blur_is_not_roi(self):
        whole = [0, 0, W, H]
        res, v, _ = ov.roi_check(self.frames(), self.frames(lambda f: box_blur_rect(f, whole, 1, 1)),
                                 self.RECT, "blur")
        self.assertNotEqual(v, "PASS", res)

    def weak(self):
        def mix(f):
            g = box_blur_rect(f, self.RECT)
            return ov.Img(f.w, f.h, [round(0.95 * x + 0.05 * y) for x, y in zip(f.px, g.px)])
        return self.frames(mix)

    def test_weak_blur_graded_by_delta_and_rc(self):
        off, on = self.frames(), self.weak()
        res, v, _ = ov.roi_check(off, on, self.RECT, "blur", 20, "vbr")
        self.assertTrue(0.85 < res["rel"] < 0.95, res)
        self.assertEqual(v, "NOEFFECT")
        self.assertEqual(ov.roi_check(off, on, self.RECT, "blur", 20, "cbr")[1], "WARN")
        self.assertEqual(ov.roi_check(off, on, self.RECT, "blur", 13, "cbr")[1], "WARN")
        self.assertIn(ov.roi_check(off, on, self.RECT, "blur", 13, "vbr")[1], ("PASS", "NOEFFECT"))
        self.assertEqual(ov.roi_check(off, on, self.RECT, "blur")[1], "NOEFFECT")

    def test_small_delta_no_effect_is_warn(self):
        off = self.frames()
        self.assertEqual(ov.roi_check(off, self.frames(n=3), self.RECT, "blur", 13)[1], "WARN")
        self.assertEqual(ov.roi_check(off, self.frames(n=3), self.RECT, "sharp", -12)[1], "WARN")
        blurred = self.frames(lambda f: box_blur_rect(f, self.RECT))
        self.assertEqual(ov.roi_check(off, blurred, self.RECT, "sharp", -12)[1], "FAIL")
        self.assertEqual(ov.roi_check(off, blurred, self.RECT, "blur", 13)[1], "PASS")

    def test_small_delta_graded_under_fixqp_vbr(self):
        off, same = self.frames(), self.frames(n=3)
        for rc in ("fixqp", "vbr"):
            self.assertEqual(ov.roi_check(off, same, self.RECT, "blur", 13, rc)[1], "NOEFFECT")
            self.assertEqual(ov.roi_check(off, same, self.RECT, "sharp", -12, rc)[1], "NOEFFECT")
        self.assertEqual(ov.roi_check(off, same, self.RECT, "blur", 13, "cbr")[1], "WARN")
        sharper = self.frames(lambda f: sharpen_rect(f, self.RECT))
        self.assertEqual(ov.roi_check(off, sharper, self.RECT, "blur", 13, "cbr")[1], "FAIL")

    def test_blur_bar(self):
        self.assertAlmostEqual(ov.blur_bar(None), ov.ROI_BLUR)
        self.assertAlmostEqual(ov.blur_bar(20), 0.85)
        self.assertAlmostEqual(ov.blur_bar(25), 0.85)
        self.assertAlmostEqual(ov.blur_bar(10), 0.925)

    def test_region_picker_finds_texture(self):
        # textured only in the lower-right area, flat elsewhere
        vals = [v if (x >= 352 and y >= 176) else 0.0
                for y in range(H) for x, v in zip(range(W), self.vals[y * W:(y + 1) * W])]
        img = to_img(vals, W, H)
        (x, y, w, h), share = ov.pick_region(img, 16, 0.34)
        self.assertTrue(x % 16 == 0 and y % 16 == 0 and w % 16 == 0 and h % 16 == 0)
        self.assertGreaterEqual(x, 336)
        self.assertGreaterEqual(y, 160)
        self.assertGreater(share, 0.3)

    def test_real_x264_roi(self):
        """libx264 honours ffmpeg's addroi: the same metric must see it."""
        if not shutil.which("ffmpeg"):
            self.skipTest("ffmpeg not on PATH")
        enc = subprocess.run(["ffmpeg", "-hide_banner", "-encoders"], capture_output=True, text=True).stdout
        if "libx264" not in enc:
            self.skipTest("ffmpeg without libx264")
        x, y, w, h = self.RECT
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "src.pgm")
            ov.save_pgm(to_img(self.vals, W, H, noise=2.0, seed=4), src)

            def enc_frames(qoff, tag):
                vf = "format=yuv420p"
                if qoff is not None:
                    vf += ",addroi=x=%d:y=%d:w=%d:h=%d:qoffset=%s" % (x, y, w, h, qoff)
                h264 = os.path.join(d, tag + ".h264")
                subprocess.run(["ffmpeg", "-v", "error", "-y", "-loop", "1", "-i", src, "-t", "1",
                                "-r", "5", "-vf", vf, "-c:v", "libx264", "-g", "1", "-crf", "30",
                                "-f", "h264", h264], check=True)
                subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", h264, "-frames:v", "3",
                                os.path.join(d, tag + "_%d.pgm")], check=True)
                return [ov.load(os.path.join(d, "%s_%d.pgm" % (tag, i))) for i in (1, 2, 3)]

            off = enc_frames(None, "off")
            res, v, _ = ov.roi_check(off, enc_frames("0.4", "blur"), self.RECT, "blur")
            self.assertEqual(v, "PASS", res)
            res, v, _ = ov.roi_check(off, enc_frames("-0.4", "sharp"), self.RECT, "sharp")
            self.assertEqual(v, "PASS", res)


class TestMisc(Base):
    def test_stats_and_stable(self):
        a = to_img(self.vals, W, H, noise=1.0, seed=1)
        b = to_img(self.vals, W, H, noise=1.0, seed=2)
        self.assertEqual(ov.stable(a, b)[2], 1)
        c = to_img(self.vals, W, H, off=30)
        self.assertEqual(ov.stable(a, c)[2], 0)
        m, s, sat = ov.stats(a)
        self.assertAlmostEqual(m, 128, delta=10)
        self.assertIsNone(sat)

    def test_ppm_saturation(self):
        gray = b"P6\n2 1\n255\n" + bytes([100, 100, 100, 50, 50, 50])
        col = b"P6\n2 1\n255\n" + bytes([200, 50, 50, 50, 200, 50])
        self.assertEqual(ov.stats(ov._parse_pnm(gray))[2], 0)
        self.assertGreater(ov.stats(ov._parse_pnm(col))[2], 100)

    def test_temporal_noise(self):
        quiet = [to_img(self.vals, W, H, noise=1.0, seed=s) for s in range(3)]
        loud = [to_img(self.vals, W, H, noise=8.0, seed=s) for s in range(3)]
        self.assertLess(ov.tnoise(quiet) * 3, ov.tnoise(loud))

    def test_lift(self):
        self.assertEqual(ov.lift_grade(17.5, 218.8, 18.0)[1], "PASS")
        self.assertEqual(ov.lift_grade(69.4, 20.7, 70.1)[1], "FAIL")
        self.assertEqual(ov.lift_grade(60.0, 63.0, 59.0)[1], "WARN")
        # cam-garage 2026-10-09: the daynight boot probe switched the IR off mid-test
        res, v, _ = ov.lift_grade(103.6, 10.3, 6.0)
        self.assertEqual(v, "INCONCLUSIVE", res)
        self.assertEqual(ov.lift_grade(20.0, 200.0, 90.0)[1], "INCONCLUSIVE")
        self.assertEqual(ov.lift_grade(20.0, 200.0, 40.0)[1], "PASS")
        self.assertEqual(ov.lift_grade(17.5, 218.8, 18.0, 216.0)[1], "PASS")
        # a light on during the first high grab only
        self.assertEqual(ov.lift_grade(20.0, 60.0, 21.0, 22.0)[1], "INCONCLUSIVE")
        self.assertEqual(ov.lift_grade(60.0, 63.0, 59.0, 62.0)[1], "WARN")

    def test_cli_lift(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            ov.main(["lift", "--lo", "100", "--hi", "10", "--lo2", "5"])
        self.assertIn("verdict=INCONCLUSIVE", buf.getvalue())
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            ov.main(["lift", "--lo", "20", "--hi", "200", "--lo2", "21", "--hi2", "198"])
        self.assertIn("verdict=PASS", buf.getvalue())

    def test_cli_error_line(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = ov.main(["stats", "/nonexistent.pgm"])
        self.assertEqual(rc, 2)
        self.assertIn("verdict=ERROR", buf.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=1)
