#!/usr/bin/env python3
"""dn-trace2scenario.py - turn a MEASURED daynight trace into a replay scenario.

Why this exists. docs/wiki/Day-Night-Design-Notes.md section 6 opens with
"there is no trace to replay": every scenario in the corpus so far was
hand-reconstructed from syslog fragments, including the ones used to verify
the fixes. Since 2026-08-17 there are real traces from a real fleet with
operator-supplied ground truth (what was switched on, and when). This turns
those into corpus scenarios, so the machine is tested against what actually
happened rather than against someone's memory of it.

THE TWO-PIPELINE PROBLEM, and how this stays honest about it. A scenario needs
the gain of BOTH optical paths at every instant; a field trace only ever holds
the one the camera was in. So each emitted curve is labelled:

  measured  - taken from the trace while the camera was in that mode
  held      - the last measured value of that pipeline, carried forward
  inferred  - no measurement of that pipeline exists in the window at all;
              a value is chosen and the scenario says so in its description

A scenario whose day curve is entirely `inferred` is still useful (it pins the
night-side behaviour) but must not be read as proof about the day side. The
generated description states which parts are which, so a reader cannot mistake
one for the other.

Usage:
  ./scripts/dn-trace2scenario.py --trace private/.../traces/cam-X.csv \\
      --from "22:17:00" --to "22:23:00" --date 2026-08-17 \\
      --name 16-shed-light --incident "operator switched shed light 22:17:29-22:21:55" \\
      --expect-day "22:17:29..22:21:55" --scale 1 > scripts/dn-scenarios/16-shed-light.json
"""
import argparse, datetime as dt, json, sys

def load(path):
    lines = open(path).read().splitlines()
    clock = [l for l in lines if l.startswith("#clock")]
    if not clock:
        raise SystemExit("trace has no '#clock <epoch> <uptime>' header")
    ep, up = (int(x) for x in clock[0].split()[1:3])
    out = []
    for l in lines:
        if not l or not l[0].isdigit():
            continue
        f = l.split(",")
        try:
            g = float(f[2])
            if g > 0:
                out.append((ep - up + int(f[0]) / 1000.0, int(f[1]), g))
        except (ValueError, IndexError):
            continue
    return sorted(out)

def hhmm(day, s):
    h, m, *rest = (int(x) for x in s.split(":"))
    return dt.datetime(day.year, day.month, day.day, h, m, rest[0] if rest else 0).timestamp()

def curve(rows, want_night, t0, step):
    """-> ([[t, gain], ...], provenance) for one pipeline."""
    pts, last, prov = [], None, "inferred"
    for t, c, g in rows:
        if (c == 1) == want_night and c in (0, 1):
            pts.append([int(round(t - t0)), int(round(g))])
            last = g
            prov = "measured" if prov == "inferred" else prov
    if not pts:
        return None, "inferred"
    # thin to `step` seconds so the JSON stays readable
    thin, prev_t = [], -1e9
    for t, g in pts:
        if t - prev_t >= step or not thin:
            thin.append([t, g]); prev_t = t
    if thin[0][0] != 0:
        thin.insert(0, [0, thin[0][1]])
    return thin, ("measured" if len(pts) > 1 else "held")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trace", required=True)
    ap.add_argument("--date", required=True)
    ap.add_argument("--from", dest="t_from", required=True)
    ap.add_argument("--to", dest="t_to", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--incident", default="")
    ap.add_argument("--note", default="", help="operator ground truth, free text")
    ap.add_argument("--expect-day", default="", help='"HH:MM:SS..HH:MM:SS" windows that were genuinely day')
    ap.add_argument("--scale", type=int, default=1)
    ap.add_argument("--step", type=int, default=5)
    ap.add_argument("--day-gain", type=int, default=768)
    ap.add_argument("--night-gain", type=int, default=4096)
    ap.add_argument("--infer-day", type=int, default=None,
                    help="day-pipeline value to use when the trace has none")
    a = ap.parse_args()

    day = dt.datetime.strptime(a.date, "%Y-%m-%d")
    t0, t1 = hhmm(day, a.t_from), hhmm(day, a.t_to)
    rows = [r for r in load(a.trace) if t0 <= r[0] <= t1]
    if not rows:
        raise SystemExit("no samples in that window")

    night, np_ = curve(rows, True, t0, a.step)
    dayc,  dp   = curve(rows, False, t0, a.step)
    if night is None:
        raise SystemExit("no night-pipeline samples - not a usable scenario")
    if dayc is None:
        if a.infer_day is None:
            raise SystemExit("no day-pipeline samples in window; pass --infer-day N "
                             "and the scenario will be labelled 'inferred'")
        dayc, dp = [[0, a.infer_day], [int(t1 - t0), a.infer_day]], "inferred"

    dur = int(t1 - t0)
    initial = "night" if rows[0][1] == 1 else "day"
    expected = [[0, "night"]]
    for w in [x for x in a.expect_day.split(",") if x.strip()]:
        s, e = w.split("..")
        expected += [[int(hhmm(day, s.strip()) - t0), "any"],
                     [int(hhmm(day, s.strip()) - t0) + 30, "day"],
                     [int(hhmm(day, e.strip()) - t0), "any"],
                     [int(hhmm(day, e.strip()) - t0) + 30, "night"]]

    desc = ("MEASURED, not reconstructed. Source: %s, %s %s-%s local.\n\n%s\n\n"
            "Pipeline provenance - night curve: %s, day curve: %s. A curve marked "
            "'inferred' has no measurement behind it in this window and pins nothing "
            "about that pipeline; only the 'measured' side carries evidence."
            % (a.trace.split("/")[-1], a.date, a.t_from, a.t_to,
               a.note or a.incident, np_, dp))

    scn = {"name": a.name, "incident": a.incident, "description": desc,
           "scale": a.scale, "duration_s": dur, "initial_mode": initial,
           "config": {"daynight.day_gain": a.day_gain,
                      "daynight.night_gain": a.night_gain},
           "interp": "step", "noise_pct": 0,
           "night_gain": night, "day_gain": dayc,
           "expect": {"expected_mode": expected, "max_wrong_mode_s": 120,
                      "max_switches": 4, "monotonicity": "warn"}}
    json.dump(scn, sys.stdout, indent=2)
    print()

if __name__ == "__main__":
    main()
