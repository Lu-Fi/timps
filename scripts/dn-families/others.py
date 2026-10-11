#!/usr/bin/env python3
"""others.py - the same families and profiles through models of two other
day/night engines, for comparison with run.py's results:

  daynightd  thingino-firmware package/thingino-daynightd/files/daynightd.c
  raptor     hon-revived raptor ric/ric_daynight.c, trigger=luma

  scripts/dn-families/others.py [--json OUT]

Both are MODELS, simplified where the profiles carry no data:
- daynightd reads ev_log2 of whatever pipeline is active; modelled as
  ev = 823.5 * exposure index, so its 50 % day threshold sits on day_gain
  (768) and its 25 % night threshold at 1366. 1 s samples, night count 6,
  day count 4, 5 s transition delay, anti-flap 30 samples, no IR-off probe.
- raptor reads ae_luma and total_gain; ae_luma is modelled as 60 (AE on
  target) unless the reading is railed (profile flag) or above 4 x night_gain,
  then 5. total_gain = exposure index. 1 s polls, hysteresis 5, night_luma 20,
  day_gain_pct 25, probe_gain_pct 90, probe_holdoff 60 s, probe_recheck 600 s,
  day verification with a 30 s lockout doubling to 300 s.
- The AE settles instantly in both models (the real ones need seconds)."""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run as R  # noqa: E402

DAY_GAIN, NIGHT_GAIN = 768.0, 4096.0


def series(fam, prof):
    pts = R.expand(fam["timelines"][fam["_p"]])
    out = []
    for t, item in pts:
        d, l, k, railed = R.reading(prof, item)
        out.append((t, d, l, k, railed))
    return out


def at(ser, t):
    cur = ser[0]
    for p in ser:
        if p[0] <= t:
            cur = p
        else:
            break
    return cur


def luma_of(v, railed):
    return 5 if (railed or v > 4 * NIGHT_GAIN) else 60


def run_daynightd(ser, dur, init):
    c = 632456.0 / DAY_GAIN
    night_thr, day_thr = 1124683, 632456
    mode, nc, dc, flap, last_tr = None, 0, 0, 0, -999
    sw, modes = [], []
    for t in range(dur):
        _, d, l, k, railed = at(ser, t)
        if mode is None:
            mode = init
        sig = c * (d if mode == "day" else l)
        target = mode
        if mode == "day":
            if sig > night_thr:
                nc += 1
                if nc >= 6:
                    target = "night"
            elif nc > 0:
                nc -= 1
        else:
            if sig < day_thr:
                dc += 1
                if dc >= 4:
                    target = "day"
            elif dc > 0:
                dc -= 1
        if target != mode:
            if t - last_tr < 5:
                nc = dc = 0
            elif flap > 0:
                flap -= 1
            else:
                mode = target
                sw.append((t, mode))
                last_tr, nc, dc, flap = t, 0, 0, 30
        elif flap > 0:
            flap -= 1
        modes.append(mode)
    return sw, modes, 0


def run_raptor(ser, dur, init):
    mode = init
    cooldown, base, lock, lock_next = 3, 0.0, 0, 0
    verify, probe, probe_left, holdoff, dip_run = False, False, 0, 0, 0
    recheck, nc, dc, prev, agree, extend = 0, 0, 0, 0.0, 0, 2
    sw, modes, dark = [], [], 0
    for t in range(dur):
        _, d, l, k, railed = at(ser, t)
        if mode == "day":
            gain, luma = d, luma_of(d, railed)
        elif probe:
            gain, luma = k, luma_of(k, False)
            dark += 1
        else:
            gain, luma = l, 60
        if cooldown > 0:
            cooldown -= 1
            if cooldown == 0 and mode == "day" and verify:
                verify = False
                if luma < 20:
                    hold = lock_next or 30
                    lock, lock_next = hold, min(hold * 2, 300)
                    mode = "night"; sw.append((t, mode)); cooldown = 3; base = 0
                    agree, extend = 0, 2
                else:
                    lock_next = 0
            if cooldown == 0 and mode == "night" and base == 0:
                within = prev > 0 and abs(gain - prev) <= prev / 10
                agree = agree + 1 if within else 0
                if agree < 2 and extend > 0:
                    extend -= 1; prev = gain; cooldown = 1
                else:
                    base = gain; recheck = 600
            prev = gain
            modes.append(mode)
            continue
        want_night = luma < 20 or gain > 80000
        want_day = False
        if mode == "night" and base > 0:
            if not probe and gain > base:
                base = gain
            want_day = gain < base * 0.25
        if mode == "night" and probe and luma >= 20:
            want_day = True
        if mode == "night" and base > 0 and lock == 0:
            if not probe:
                due = False
                if recheck > 0:
                    recheck -= 1
                    due = recheck == 0
                if holdoff > 0 and not due:
                    holdoff -= 1
                elif not due and gain >= base * 0.9:
                    dip_run = 0
                else:
                    dip_run += 1
                    if due or dip_run >= 3:
                        dip_run, probe, probe_left = 0, True, 3 + 5 + 3
            elif probe_left > 0:
                probe_left -= 1
                if probe_left > 5 + 2:
                    want_day = want_night = False
                elif probe_left == 0:
                    probe, holdoff, cooldown, recheck = False, 60, 3, 600
        if lock > 0:
            lock -= 1
            want_day = False
        if mode == "day":
            nc = nc + 1 if want_night else 0
            if nc >= 5:
                mode = "night"; sw.append((t, mode)); nc = 0; cooldown = 3; base = 0
                probe, agree, extend = False, 0, 2
        else:
            dc = dc + 1 if want_day else 0
            if dc >= 5:
                mode = "day"; sw.append((t, mode)); dc = 0; cooldown = 3
                verify, probe = True, False
        modes.append(mode)
    return sw, modes, dark


def wrong_longest(modes, truth):
    tl = sorted(truth)
    longest = cur = 0
    j = 0
    for t, m in enumerate(modes):
        while j + 1 < len(tl) and tl[j + 1][0] <= t:
            j += 1
        want = tl[j][1]
        if want != "any" and want != m:
            cur += 1
            longest = max(longest, cur)
        else:
            cur = 0
    return longest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json")
    a = ap.parse_args()
    fams = json.load(open(os.path.join(R.HERE, "families.json")))["families"]
    profs = {}
    for f in os.listdir(os.path.join(R.HERE, "profiles")):
        if f.endswith(".json"):
            p = json.load(open(os.path.join(R.HERE, "profiles", f)))
            profs[p["name"]] = p
    res = {}
    for fam in fams:
        if fam["name"] in ("idle", "isp_stuck"):
            continue                       # no counterpart in either model
        for pname in fam["timelines"]:
            fam["_p"] = pname
            prof = profs[pname]
            scn = R.build(fam, pname, prof, DAY_GAIN, NIGHT_GAIN)
            truth = scn["expect"]["expected_mode"]
            ser = series(fam, prof)
            led = fam.get("led", True)
            for eng, fn in (("daynightd", run_daynightd), ("raptor", run_raptor)):
                sw, modes, dark = fn(ser, fam["duration_s"], fam.get("initial_mode", "night"))
                if eng == "raptor" and not led:
                    dark = 0
                pairs, pend = R.analyse(sw, truth)
                wr = wrong_longest(modes, truth)
                b = fam["bounds"]
                ok = wr <= b["max_wrong_mode_s"] and len(sw) <= b["max_switches"] and not pend
                res["%s-%s|%s" % (fam["name"], pname, eng)] = {
                    "ok": ok, "switches": len(sw), "pairs": pairs, "pendulum": pend,
                    "switch_t": sw, "init": fam.get("initial_mode", "night"),
                    "wrong": wr, "dark_s": dark, "duration": fam["duration_s"]}
    for eng in ("daynightd", "raptor"):
        print("== %s" % eng)
        print("| pair | result | switches | failed checks | pendulum | dark s | longest wrong s |")
        print("| --- | --- | --- | --- | --- | --- | --- |")
        bad = 0
        for k, r in res.items():
            if not k.endswith("|" + eng):
                continue
            bad += not r["ok"]
            print("| %s | %s | %d | %d | %d | %d | %d |" % (
                k.split("|")[0], "PASS" if r["ok"] else "FAIL", r["switches"],
                r["pairs"], r["pendulum"], r["dark_s"], r["wrong"]))
        print("%s: %d pairs, %d failed" % (eng, sum(k.endswith("|" + eng) for k in res), bad))
    if a.json:
        json.dump(res, open(a.json, "w"), indent=1)


if __name__ == "__main__":
    main()
