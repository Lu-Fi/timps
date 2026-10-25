#!/usr/bin/env python3
"""run.py - behaviour families x camera profiles through scripts/dn-replay.py.

  scripts/dn-families/run.py --bin timpsd-dnsim [--jobs 8] [--only REGEX]

The binary must be a sim built with -DMS_CLOCK_SCALE=60. Every (family,
profile) pair becomes one regression scenario; only behaviour is judged:
the mode against the truth derived from the scene's day-pipeline value,
the longest wrong-mode run, the number of board switches and, for Idle-Dark,
the illuminator timeline. Exit code 0 iff every pair passed."""
import argparse
import concurrent.futures as cf
import json
import math
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPLAY = os.path.join(os.path.dirname(HERE), "dn-replay.py")
SCALE = 60
STEP = 60          # daylight ramps are sampled every STEP virtual seconds
BOOT_ANY = 90      # boot settles and measures: no truth yet


def curve_at(curve, lvl):
    """[day, lit, dark] at daylight level lvl, log-interpolated."""
    pts = sorted(curve)
    if lvl <= pts[0][0]:
        return pts[0][1:]
    for a, b in zip(pts, pts[1:]):
        if lvl <= b[0]:
            f = (lvl - a[0]) / (b[0] - a[0])
            return [math.exp(math.log(x) + f * (math.log(y) - math.log(x)))
                    for x, y in zip(a[1:], b[1:])]
    return pts[-1][1:]


def reading(prof, item):
    """(day, lit, dark, railed) for a timeline item."""
    rail = prof.get("curve_rail", float("inf"))
    if isinstance(item, dict):
        d, l, k = curve_at(prof["curve"], item["lvl"])
        return d, l, k, d >= rail
    sc = prof["scenes"][item]
    if "lvl" in sc:
        d, l, k = curve_at(prof["curve"], sc["lvl"])
        return d, l, k, d >= rail
    return sc["day"], sc["lit"], sc["dark"], bool(sc.get("railed"))


def tint(prof, item):
    """[rg %, bg %] the lamp of a scene adds to the lit night colour"""
    sc = prof["scenes"].get(item, {}) if isinstance(item, str) else {}
    if "tint" in sc:
        return sc["tint"]
    k = sc.get("lamp", 0)
    return [k * x for x in prof["color"]["shift"]]


def expand(tl):
    """timeline -> [(t, item)] with daylight ramps sampled every STEP s."""
    out = []
    for (t0, a), (t1, b) in zip(tl, tl[1:]):
        out.append((t0, a))
        if isinstance(a, dict) and isinstance(b, dict) and a["lvl"] != b["lvl"]:
            t = t0 + STEP
            while t < t1:
                f = (t - t0) / (t1 - t0)
                out.append((t, {"lvl": a["lvl"] + f * (b["lvl"] - a["lvl"])}))
                t += STEP
    out.append(tl[-1])
    return out


def to_pipe(prof, v, pipe):
    """(gain, it_ratio) the harness serves for exposure index v."""
    m = prof["map"]
    if m["kind"] == "bare":
        return max(256.0, v), None
    if m["kind"] == "t20":
        r = m["it_base"]
        g = v / r
        if g < 256:
            return 256.0, v / 256.0
        if g > m["cap"]:
            r = min(m["it_cap"], v / m["cap"])
            return v / r, r
        return g, r
    r = 0.5 if pipe == "day" else 0.25
    g = v / r
    if g < 256:
        return 256.0, v / 256.0
    return g, r


def build(fam, pname, prof, day_gain, night_gain):
    pts = expand(fam["timelines"][pname])
    dg, dr, ng, nr, ko, truth, awb = [], [], [], [], [], [], []
    col = prof.get("color")
    for t, item in pts:
        if col:
            d_rg, d_bg = tint(prof, item)
            awb.append([t, col["base"][0] * (1 + d_rg / 100.0),
                        col["base"][1] * (1 + d_bg / 100.0)])
        d, l, k, railed = reading(prof, item)
        g, r = to_pipe(prof, d, "day")
        dg.append([t, g]); dr.append([t, r])
        g, r = to_pipe(prof, l, "night")
        ng.append([t, g]); nr.append([t, r])
        ko.append([t, k / r if r else max(256.0, k)])
        m = "day" if d < day_gain else "night" if (d > night_gain or railed) else "any"
        truth.append([max(t, BOOT_ANY), m])
    truth = [[0, "any"]] + [x for i, x in enumerate(truth)
                            if i + 1 == len(truth) or truth[i + 1][0] != x[0]]
    b = fam["bounds"]
    exp = {"expected_mode": truth, "max_wrong_mode_s": b["max_wrong_mode_s"],
           "max_switches": b["max_switches"], "monotonicity": "skip"}
    for k in ("max_led_on_unwatched_s", "led_on_within_s", "max_ir_probes",
              "forbid_log", "expect_log_count"):
        if k in b:
            exp[k] = b[k]
    scn = {"name": "%s-%s" % (fam["name"], pname), "scale": SCALE,
           "duration_s": fam["duration_s"],
           "initial_mode": fam.get("initial_mode", "night"),
           "config": dict(fam.get("config", {})), "interp": "step",
           "noise_pct": 0, "day_gain": dg, "night_gain": ng, "expect": exp}
    scn["config"].setdefault("daynight.day_gain", day_gain)
    scn["config"].setdefault("daynight.night_gain", night_gain)
    if fam.get("led", True):
        scn["night_gain_noir"] = ko
    if any(r is not None for _, r in dr):
        scn["day_int_ratio"] = dr
        scn["night_int_ratio"] = nr
    if col:
        scn["awb_lit"] = awb
        scn["awb_noise_pct"] = col.get("noise_pct", 0)
    if "consumers" in fam:
        scn["consumers"] = fam["consumers"]
    if "isp_sticky" in fam:
        scn["isp_sticky"] = fam["isp_sticky"]
    if "day_lag_s" in fam:
        scn["day_lag_s"] = fam["day_lag_s"]
    scn["bounds_extra"] = {k: b[k] for k in ("max_on_s", "max_off_s") if k in b}
    scn.update(prof.get("harness", {}))
    return scn


PAIR_S = 30        # day then night within this: a failed check
PENDULUM_S = 120   # two real switches closer than this without a light change


def truth_changes(truth):
    return [t for (t, m), (_, p) in zip(truth[1:], truth) if m != p]


def analyse(switches, truth):
    """failed-check pairs and pendulum events in [(t, 'day'|'night')]"""
    pairs, real, i = 0, [], 0
    while i < len(switches):
        t, m = switches[i]
        if (m == "day" and i + 1 < len(switches) and switches[i + 1][1] == "night"
                and switches[i + 1][0] - t <= PAIR_S):
            pairs += 1
            i += 2
            continue
        real.append((t, m))
        i += 1
    ch = truth_changes(truth)
    pend = 0
    for (t1, _), (t2, _) in zip(real, real[1:]):
        if t2 - t1 < PENDULUM_S and not any(t1 - 60 <= c <= t2 for c in ch):
            pend += 1
    return pairs, pend


HOLD_S = 60        # a mode counts as reached once it holds this long


def mode_at(sw, init, t):
    m = init
    for ts, mm in sw:
        if ts <= t:
            m = mm
    return m


def latencies(sw, init, truth, dur):
    """[(mode, seconds)] for every truth change to day or night after boot"""
    out = []
    pts = [(t, m) for t, m in truth if m != "any"]
    for (t0, m0), (t1, m1) in zip(pts, pts[1:]):
        if m1 == m0 or t1 < BOOT_ANY + 1:
            continue
        lat = dur - t1
        for t in range(t1, dur - HOLD_S):
            if all(mode_at(sw, init, x) == m1 for x in range(t, t + HOLD_S, 5)):
                lat = t - t1
                break
        out.append((m1, lat))
    return out


def run_one(binary, path, keep):
    cmd = [sys.executable, REPLAY, "--bin", binary, path]
    if keep:
        cmd.append("--keep")
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True)
    t = p.stdout
    sw = re.search(r"(\d+) board switches, (\d+) silent IR probes", t)
    wr = re.search(r"wrong-mode\s+longest run (\d+)s", t)
    fails = re.findall(r"^\s+FAIL\s+(\S+)\s+(.*)$", t, re.M)
    switches = [(int(a), b) for a, b in re.findall(r"switch\s+t=(\d+)s -> (day|night)", t)]
    truth = json.load(open(path))["expect"]["expected_mode"]
    pairs, pend = analyse(switches, truth)
    scn = json.load(open(path))
    lat = latencies(switches, scn["initial_mode"], truth, scn["duration_s"])
    bnd = scn.get("bounds_extra", {})
    on = max((l for m, l in lat if m == "day"), default=0)
    off = max((l for m, l in lat if m == "night"), default=0)
    if "max_on_s" in bnd and on > bnd["max_on_s"]:
        fails.append(("light-on", "%d s to day (bound %d s)" % (on, bnd["max_on_s"])))
    if "max_off_s" in bnd and off > bnd["max_off_s"]:
        fails.append(("light-off", "%d s to night (bound %d s)" % (off, bnd["max_off_s"])))
    if pend:
        fails.append(("pendulum", "%d switch pairs closer than %d s without a light change"
                      % (pend, PENDULUM_S)))
    lat_bad = any(k in ("light-on", "light-off") for k, _ in fails)
    return {"ok": p.returncode == 0 and not pend and not lat_bad, "on_s": on, "off_s": off, "switches": int(sw.group(1)) if sw else -1,
            "pairs": pairs, "pendulum": pend, "switch_t": switches,
            "looks": int(sw.group(2)) if sw else -1,
            "wrong": int(wr.group(1)) if wr else -1,
            "fails": ["%s %s" % f for f in fails], "out": t}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bin", required=True)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument("--only", default="")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--json", help="write the results here")
    a = ap.parse_args()
    a.bin = os.path.abspath(a.bin)
    fams = json.load(open(os.path.join(HERE, "families.json")))["families"]
    profs = {}
    for f in os.listdir(os.path.join(HERE, "profiles")):
        if f.endswith(".json"):
            p = json.load(open(os.path.join(HERE, "profiles", f)))
            profs[p["name"]] = p
    tmp = tempfile.mkdtemp(prefix="dnfam-")
    jobs = []
    for fam in fams:
        for pname in fam["timelines"]:
            name = "%s-%s" % (fam["name"], pname)
            if a.only and not re.search(a.only, name):
                continue
            cfg = fam.get("config", {})
            scn = build(fam, pname, profs[pname],
                        float(cfg.get("daynight.day_gain", 768.0)),
                        float(cfg.get("daynight.night_gain", 4096.0)))
            path = os.path.join(tmp, name + ".json")
            json.dump(scn, open(path, "w"))
            jobs.append((name, path))
    res = {}
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        futs = {ex.submit(run_one, a.bin, p, a.keep): n for n, p in jobs}
        for f in cf.as_completed(futs):
            res[futs[f]] = f.result()
    bad = 0
    print("| pair | result | switches | failed checks | looks | longest wrong s | failed asserts |")
    print("| --- | --- | --- | --- | --- | --- | --- |")
    for n, _ in jobs:
        r = res[n]
        bad += not r["ok"]
        print("| %s | %s | %d | %d | %d | %d | %s |" % (
            n, "PASS" if r["ok"] else "FAIL", r["switches"], r["pairs"], r["looks"],
            r["wrong"], "; ".join(x[:80] for x in r["fails"])))
    print("%d pairs, %d failed" % (len(jobs), bad))
    if a.json:
        json.dump({n: {k: v for k, v in r.items() if k != "out"}
                   for n, r in res.items()}, open(a.json, "w"), indent=1)
    for n, p in jobs:
        if not a.keep:
            os.unlink(p)
    if not a.keep:
        os.rmdir(tmp)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
