#!/usr/bin/env python3
"""compare.py - acceptance figures per engine from run.py/others.py JSON.

  compare.py name=run.json [name=run.json ...] [others=others.json]

Dark time (steady night), switches after boot in a dusk + dawn, light-on and
light-off latency (first time the mode matches a changed truth and holds
60 s), longest wrong run, pendulum events, pairs over their bounds."""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run as R  # noqa: E402

LOOK_S = 8
HOLD = 60


def mode_at(sw, init, t):
    m = init
    for ts, mm in sw:
        if ts <= t:
            m = mm
    return m


def latencies(sw, init, truth, dur):
    """[(kind, seconds)] for every truth change to day or night"""
    out = []
    pts = [(t, m) for t, m in truth if m != "any"]
    for (t0, m0), (t1, m1) in zip(pts, pts[1:]):
        if m1 == m0 or t1 < R.BOOT_ANY + 1:
            continue
        lat = None
        for t in range(t1, dur - HOLD):
            if all(mode_at(sw, init, x) == m1 for x in range(t, t + HOLD, 5)):
                lat = t - t1
                break
        out.append((m1, lat if lat is not None else dur - t1))
    return out


def main():
    fams = {f["name"]: f for f in json.load(open(os.path.join(R.HERE, "families.json")))["families"]}
    profs = {}
    for f in os.listdir(os.path.join(R.HERE, "profiles")):
        if f.endswith(".json"):
            p = json.load(open(os.path.join(R.HERE, "profiles", f)))
            profs[p["name"]] = p
    engines = {}
    for a in sys.argv[1:]:
        name, path = a.split("=", 1)
        d = json.load(open(path))
        if name == "others":
            for k, v in d.items():
                pair, eng = k.split("|")
                engines.setdefault(eng, {})[pair] = v
        else:
            engines[name] = d
    rows = []
    for eng, res in engines.items():
        on, off, dark, wrong, pend, bad, dd = [], [], [], 0, 0, 0, []
        day_sw = {}
        for pair, r in res.items():
            fname, pname = pair.rsplit("-", 1)
            fam = fams.get(fname)
            if not fam:
                continue
            scn = R.build(fam, pname, profs[pname], 768.0, 4096.0)
            truth = scn["expect"]["expected_mode"]
            init = fam.get("initial_mode", "night")
            sw = [tuple(x) for x in r.get("switch_t", [])]
            bad += not r["ok"]
            pend += r.get("pendulum", 0)
            wrong = max(wrong, r.get("wrong", 0))
            if fname in ("lamp_jump", "foreign_ir_lamp", "check_then_light", "lamp_nojump",
                         "eufy_dark_bulb", "lamp_again"):
                for kind, lat in latencies(sw, init, truth, fam["duration_s"]):
                    (on if kind == "day" else off).append((lat, pair))
            if fname in ("dusk", "dawn"):
                lat = latencies(sw, init, truth, fam["duration_s"])
                if lat:
                    dd.append((fname, pname, lat[-1][1]))
                day_sw.setdefault(pname, 0)
                day_sw[pname] += sum(1 for t, _ in sw if t > R.BOOT_ANY + 30)
            if fname == "steady_night":
                looks = r.get("dark_s", r.get("looks", 0) * LOOK_S)
                dark.append(100.0 * looks / fam["duration_s"])
        on_jump = [x for x in on if not x[1].startswith(("lamp_nojump", "eufy_dark_bulb", "lamp_again"))]
        rows.append((eng, bad, len(res), dark, day_sw, on_jump, on, off, dd, wrong, pend))
    for eng, bad, n, dark, day_sw, on_jump, on, off, dd, wrong, pend in rows:
        print("== %s: %d/%d pairs within bounds" % (eng, n - bad, n))
        print("  dark time steady night %%: %s" % ", ".join("%.2f" % x for x in dark))
        print("  switches in dusk+dawn after boot: %s" % day_sw)
        print("  light on with a jump -> day s: max %s  %s" % (
            max((x[0] for x in on_jump), default="-"),
            ", ".join("%s=%s" % (p, l) for l, p in on_jump)))
        print("  light on (all) -> day s: max %s" % max((x[0] for x in on), default="-"))
        print("  light off -> night s: max %s  %s" % (
            max((x[0] for x in off), default="-"),
            ", ".join("%s=%s" % (p, l) for l, p in off)))
        print("  dusk/dawn s after crossing: %s" % ", ".join("%s-%s=%s" % x for x in dd))
        print("  longest wrong run s: %s; pendulum events: %d" % (wrong, pend))


if __name__ == "__main__":
    main()
