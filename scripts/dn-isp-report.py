#!/usr/bin/env python3
"""dn-isp-report.py - analyse the CSVs collected by dn-isp-probe.sh.

Two jobs, one of which needs you to say when it was actually light.

1. WITHOUT --truth: does the AE still shorten the integration time once its
   gain rails at the [24.8] floor (256)?  That is the assumption the exposure
   index D = gain * int/max rests on. If it holds, the index has range exactly
   where bare gain is blind - the IR-saturated night regime of the a5dae07
   incident. If it does not, D degrades to plain gain on that camera, the
   daemon says so in a log line, and the heartbeat carries it. Both are
   supported; this is how to tell them apart.

2. WITH --truth: score the running build against reality, and derive per-camera
   thresholds from measured data instead of a fleet-wide guess.

   Why the labels are needed at all: a threshold has to sit ABOVE the dimmest
   genuine daylight and BELOW the brightest genuine night, and neither bound is
   observable without knowing which is which. Measured on this fleet on an
   overcast 2026-08-17, genuine daylight spanned a factor of 63 ACROSS cameras
   at one instant, and a factor of 4.5 on one camera across a single morning.
   No single fleet-wide number survives that, and the value must be chosen from
   the DIM end - a threshold that works in sun fails under cloud, never the
   other way round.

   The truth file is one line per camera, or "*" for all, local times:

       # camera        light-from   light-until
       *               06:35        20:40
       cam-A           07:10        20:05      # sees the sky late

   Lines starting with # are ignored; a per-camera line overrides "*".

Usage:
    ./scripts/dn-isp-report.py dn-isp-*.csv
    ./scripts/dn-isp-report.py --truth truth.txt dn-isp-*.csv
"""
import csv
import datetime as dt
import sys

FLOOR = 256.0          # IMP [24.8] gain floor, 1.0x
NEAR_FLOOR = 1.05      # "at the floor" tolerance
DAY_HEADROOM = 1.25    # day_gain above the dimmest observed daylight
NIGHT_MARGIN = 0.80    # night_gain below the brightest observed night


def gain_linear(again, dgain, ispdgain):
    """isp-m0 / isp_info log2 units (32 per stop, 0 = 1.0x) -> [24.8] linear."""
    units = sum(u for u in (again, dgain, ispdgain) if u and u > 0)
    return FLOOR * 2.0 ** (units / 32.0)


def load(path):
    """Mirrors dn_read() in src/daynight.c, including its high-water-mark
    stand-in for a missing SENSOR Max Integration Time - the T20 SDK does not
    publish one, and those are exactly the two cameras worth measuring. The
    analysis has to model the daemon, not an idealised version of it."""
    rows, hwm = [], 0
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            try:
                it = int(r["int"])
                mx = int(r["max_int"] or 0)
                if mx <= 0 and it > 0:
                    hwm = max(hwm, it)
                    mx = hwm
                if mx <= 0:
                    continue
                g = gain_linear(int(r["again"] or 0), int(r["dgain"] or 0),
                                int(r["ispdgain"] or 0))
                ratio = min(1.0, it / mx)
                rows.append({"t": int(r["epoch"]),
                             "night": r["mode"].lower().startswith("n"),
                             "ratio": ratio, "gain": g, "d": g * ratio})
            except (ValueError, TypeError, KeyError):
                continue
    return rows


def parse_truth(path):
    """-> {camera_or_'*': (from_minutes, until_minutes)} in local time."""
    out = {}
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) != 3:
                raise SystemExit("bad truth line (want 'name HH:MM HH:MM'): %s"
                                 % line)
            name, a, b = parts
            try:
                out[name] = tuple(int(x.split(":")[0]) * 60 + int(x.split(":")[1])
                                  for x in (a, b))
            except (ValueError, IndexError):
                raise SystemExit("bad time in truth line: %s" % line)
    return out


def is_light(epoch, window):
    """window may wrap past midnight, like the daemon's own time calendar."""
    lt = dt.datetime.fromtimestamp(epoch)
    m = lt.hour * 60 + lt.minute
    a, b = window
    return (a <= m < b) if a <= b else (m >= a or m < b)


def hhmm(epoch):
    return dt.datetime.fromtimestamp(epoch).strftime("%H:%M")


def integration_verdict(rows):
    """Job 1: does the exposure move while the gain is pinned at the floor?"""
    night = [r for r in rows if r["night"]]
    railed = [r for r in night if r["gain"] <= FLOOR * NEAR_FLOOR]
    if not railed:
        deepest = min((r["gain"] for r in night), default=None)
        print("   night gain never reached the floor%s - this camera never "
              "entered the blind regime, so it cannot answer the question"
              % ("" if deepest is None else " (lowest %.0f)" % deepest))
        return
    lo = min(r["ratio"] for r in railed)
    hi = max(r["ratio"] for r in railed)
    print("   %d night samples with gain AT THE FLOOR (%.0f):"
          % (len(railed), FLOOR))
    print("     integration ratio %.4f .. %.4f (%.1fx)   index %.1f .. %.1f "
          "<- bare gain reads %.0f for every one"
          % (lo, hi, hi / lo if lo else float("inf"),
             FLOOR * lo, FLOOR * hi, FLOOR))
    if hi / lo >= 1.5:
        print("     VERDICT: the AE does shorten the exposure at the gain "
              "floor - the index has range where gain is blind. The "
              "redesign's assumption holds here.")
    else:
        print("     VERDICT: the exposure barely moves here (<1.5x). The index "
              "adds nothing over gain on this camera and the heartbeat "
              "carries it. Supported, but note it.")


def truth_verdict(rows, window):
    """Job 2: score the running build, then derive thresholds from labels."""
    for r in rows:
        r["light"] = is_light(r["t"], window)

    # -- what the running build got wrong, in wall-clock minutes -----------
    wrong = [r for r in rows if r["light"] == r["night"]]
    if wrong:
        runs, start, prev = [], None, None
        for r in rows:
            bad = r["light"] == r["night"]
            if bad and start is None:
                start = r["t"]
            elif not bad and start is not None:
                runs.append((start, prev))
                start = None
            prev = r["t"]
        if start is not None:
            runs.append((start, rows[-1]["t"]))
        worst = max(runs, key=lambda s: s[1] - s[0])
        print("   WRONG MODE for %d of %d samples (%.0f%%); longest stretch "
              "%.0f min, %s..%s"
              % (len(wrong), len(rows), 100.0 * len(wrong) / len(rows),
                 (worst[1] - worst[0]) / 60.0, hhmm(worst[0]), hhmm(worst[1])))
    else:
        print("   mode matched the ground truth for all %d samples" % len(rows))

    # -- the two bounds a threshold pair has to fit between -----------------
    # Only DAY-pipeline samples say anything about the thresholds: night-pipeline
    # readings are taken through the illuminator and are not comparable.
    daylight = [r["d"] for r in rows if r["light"] and not r["night"]]
    darkday = [r["d"] for r in rows if not r["light"] and not r["night"]]

    if not daylight:
        print("   no day-pipeline samples during daylight - this camera never "
              "left night while it was light, so its day threshold cannot be "
              "derived from this run (that IS the finding)")
        return
    dim = max(daylight)                     # the dimmest genuine daylight
    print("   daylight through the day pipeline: %.0f .. %.0f  (dimmest %.0f)"
          % (min(daylight), dim, dim))
    day_gain = dim * DAY_HEADROOM

    if darkday:
        bright_night = min(darkday)         # the brightest genuine night
        print("   darkness through the day pipeline: %.0f .. %.0f  "
              "(brightest %.0f)" % (bright_night, max(darkday), bright_night))
        night_gain = bright_night * NIGHT_MARGIN
        if day_gain >= night_gain:
            print("   >> NO THRESHOLD PAIR FITS: the dimmest daylight (%.0f) is "
                  "not separated from the brightest darkness (%.0f). Absolute "
                  "thresholds cannot work on this camera - use "
                  "daynight.mode = schedule." % (dim, bright_night))
            return
    else:
        # No day-pipeline reading of a dark scene: keep the historic band.
        night_gain = day_gain * 5.3
        print("   no day-pipeline reading of a DARK scene in this run - "
              "either it did not span the dark hours, or the camera was "
              "(correctly) in night for all of them. night_gain below is "
              "therefore DERIVED from the historic 5.3x band, not measured; "
              "a run covering one dusk pins it properly.")
    print("   >> daynight.day_gain   = %.0f" % day_gain)
    print("   >> daynight.night_gain = %.0f" % night_gain)


def report(path, truth):
    rows = load(path)
    name = path.rsplit("/", 1)[-1].replace("dn-isp-", "").replace(".csv", "")
    if not rows:
        print("%-20s no usable samples (no integration-time fields?)" % name)
        return
    span_h = (rows[-1]["t"] - rows[0]["t"]) / 3600.0
    night = sum(1 for r in rows if r["night"])
    print("%-20s %5.1f h, %d samples (%d night / %d day), %s..%s"
          % (name, span_h, len(rows), night, len(rows) - night,
             hhmm(rows[0]["t"]), hhmm(rows[-1]["t"])))
    integration_verdict(rows)
    if truth is not None:
        window = truth.get(name, truth.get("*"))
        if window is None:
            print("   no truth window for this camera and no '*' default")
        else:
            truth_verdict(rows, window)


if __name__ == "__main__":
    args = sys.argv[1:]
    truth = None
    if args and args[0] == "--truth":
        if len(args) < 2:
            raise SystemExit("--truth needs a file")
        truth = parse_truth(args[1])
        args = args[2:]
    if not args:
        raise SystemExit(__doc__)
    for p in args:
        report(p, truth)
        print()
