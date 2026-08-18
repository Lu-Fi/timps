#!/usr/bin/env python3
"""dn-trend-eval.py - what the daemon's trend trigger finds, and what it costs.

This is the offline model of path T in src/daynight.c (the DN_TREND_* block).
It is not a proposal any more: the daemon keeps these two EMAs and fires this
comparison, so the numbers below are the parameter evidence for constants that
are actually shipped, and a change here without a change there is a bug in one
of the two.

    fast = EMA of D, tau DN_TREND_FAST_MS   (follows the scene)
    slow = EMA of D, tau DN_TREND_SLOW_MS   (the scene's memory)
    fire when  fast / slow  <  DN_TREND_PCT / 100

Both sides are smoothed, so the ratio is far quieter than either signal, and
the question it asks - "has this got sustainedly brighter than it has been?" -
is a RELATIVE one the night pipeline can actually answer, unlike "is it day?",
which it cannot: genuine daylight spans a factor of 63 across this fleet at one
instant. alpha() is the same dt/tau form as dn_ema_alpha() in the daemon, so
both filter the same signal the same way at any sample spacing.

TWO NUMBERS, NOT ONE. An earlier version of this tool reported only the
false-fire rate, which is half a verdict: a threshold of zero has a perfect
false-fire rate and finds nothing. So each run reports

  * DETECTIONS - did the trigger fire during the morning twilight window, and
    when. This is what the trigger is FOR; the design's whole reason for a
    60-minute slow side is that natural twilight is slow (a measured factor of
    2.2 over 67 minutes) and a shorter memory tracks it instead of noticing it.
  * FALSE FIRES per camera-hour, counted ONLY over samples where the camera
    was actually in night mode, because that is the only state in which the
    daemon arms the trigger. Counting a fire while the camera sits in day mode
    measures nothing the daemon would ever do.

Both counts are edge-triggered with the same hysteresis the daemon gets from
re-anchoring its EMAs after every verdict.

CAVEAT, and it runs in the safe direction: the sampler behind these CSVs runs
at one sample per minute, while the daemon samples every 2 s. Thirty times
denser sampling makes the fast EMA markedly smoother for the same time
constant, so every false-fire count here is an UPPER BOUND on what the daemon
would produce. A second over-count, same direction: a camera that is STUCK in
night through daylight registers its (correct) daytime fires as false ones.

MEASURED, fleet night of 2026-08-17/18, 12 cameras, 181 camera-hours in night
mode (private/messungen/2026-08-18_daemmerung/samples):

    tau fast/slow   bar    dawns found   false fires per camera-hour
    3 / 60 min      75 %   10 of 12      0.22      <- shipped
    3 / 60 min      70 %    9 of 12      0.15
    3 / 15 min      75 %    8 of 12      0.19  (and 30-70 min later)
    3 / 10 min      75 %    6 of 12      0.13
    3 / 10 min      88 %    9 of 12      0.44

The last row is the one that closes an open question. A third, MEDIUM (~10
min) constant was proposed after that night to catch the 21:03 bedroom light,
which fell inside the ongoing dusk (7845 -> 5087, a factor of 1.54) and so
showed up against neither the jump bar (which needs a factor of 2) nor the
60-minute memory the dusk had left far below the current level - the ratio
bottomed at 1.15. The observation is real and the remedy is refuted by the
same data: on that event a 10-minute EMA bottoms at 0.87, still short of 75 %,
and the ~88 % bar it would take sits inside the +-25 % AGC noise band, doubles
the false-fire rate, and STILL finds fewer dawns than 3/60 does alone. A
factor-1.54 event is what the heartbeat is for.

    ./scripts/dn-trend-eval.py private/messungen/*/samples/*.csv
    ./scripts/dn-trend-eval.py --fast 3 --slow 10 --dawn 05:30-08:10 *.csv
"""
import csv
import datetime
import sys

FLOOR = 256.0

# The daemon's shipped constants, so a bare run reports what is deployed.
TAU_FAST, TAU_SLOW = 3.0, 60.0          # minutes; DN_TREND_FAST/SLOW_MS
BAR = 0.75                              # DN_TREND_PCT / 100
# Thresholds swept side by side. The shipped bar is in the list on purpose -
# a sweep that cannot evaluate the value in the source is decoration.
BARS = [0.60, 0.70, 0.75, 0.80]
# Civil twilight to well past sunrise for the measured location (Grosshau,
# 50.72 N / 6.37 E, 2026-08-18: civil dawn 05:48, sunrise 06:24). A fire in
# here is the trigger doing its job; anywhere else in night it is a false one.
DAWN = (datetime.time(5, 30), datetime.time(8, 10))


def load(path):
    """(minutes_from_start, D, night, epoch) per sample - mirrors dn_read()."""
    rows, hwm, t0 = [], 0, None
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
                u = sum(int(r[k] or 0) for k in ("again", "dgain", "ispdgain")
                        if int(r[k] or 0) > 0)
                t = int(r["epoch"])
                t0 = t if t0 is None else t0
                rows.append(((t - t0) / 60.0,
                             FLOOR * 2.0 ** (u / 32.0) * min(1.0, it / mx),
                             r["mode"].lower().startswith("n"),
                             t))
            except (ValueError, TypeError, KeyError):
                continue
    return rows


def alpha(tau_min, dt_min):
    """EMA coefficient for a time constant, at this sample spacing.

    Identical to dn_ema_alpha() in src/daynight.c: alpha = dt/tau, clamped at
    1. Keep the two in step - the whole point of this tool is that it filters
    what the daemon filters."""
    return 1.0 if tau_min <= 0 else min(1.0, dt_min / tau_min)


def in_dawn(epoch, dawn):
    t = datetime.datetime.fromtimestamp(epoch).time()
    return dawn[0] <= t <= dawn[1]


def evaluate(rows, tau_fast, tau_slow, bars, dawn):
    """-> {bar: (detections, false_fires, night_hours, quietest_ratio)}

    The EMAs run over every sample - they are a model of the scene, and the
    daemon's own pair is likewise fed whenever it is in night - but a FIRE is
    only counted while the camera is in night mode, because that is the only
    state in which the daemon arms path T."""
    out = {b: [0, 0, 0.0, 1.0] for b in bars}
    fast = slow = None
    armed = {b: True for b in bars}      # edge-triggered, like the daemon
    prev_t = None
    for t, d, night, epoch in rows:
        if fast is None:
            fast = slow = d
            prev_t = t
            continue
        dt = max(1e-6, t - prev_t)
        prev_t = t
        fast += (d - fast) * alpha(tau_fast, dt)
        slow += (d - slow) * alpha(tau_slow, dt)
        if slow <= 0:
            continue
        ratio = fast / slow
        for b in bars:
            if night:
                out[b][2] += dt / 60.0
                out[b][3] = min(out[b][3], ratio)
            if ratio >= b + (1.0 - b) * 0.5:       # re-arm with hysteresis
                armed[b] = True
            elif ratio < b and night and armed[b]:
                armed[b] = False
                if in_dawn(epoch, dawn):
                    out[b][0] += 1
                else:
                    out[b][1] += 1
    return out


def first_fire(rows, tau_fast, tau_slow, bar, dawn):
    """clock time of the first dawn-window fire, or None."""
    fast = slow = None
    prev_t = None
    armed = True
    for t, d, night, epoch in rows:
        if fast is None:
            fast = slow = d
            prev_t = t
            continue
        dt = max(1e-6, t - prev_t)
        prev_t = t
        fast += (d - fast) * alpha(tau_fast, dt)
        slow += (d - slow) * alpha(tau_slow, dt)
        if slow <= 0:
            continue
        ratio = fast / slow
        if ratio >= bar + (1.0 - bar) * 0.5:
            armed = True
        elif ratio < bar and night and armed:
            armed = False
            if in_dawn(epoch, dawn):
                return datetime.datetime.fromtimestamp(epoch)
    return None


def main(paths, tau_fast, tau_slow, dawn):
    print("trend trigger (path T): fast tau %g min, slow tau %g min"
          % (tau_fast, tau_slow))
    print("dawn window %s-%s; false fires counted only while the camera is in "
          "NIGHT mode\n" % (dawn[0].strftime("%H:%M"), dawn[1].strftime("%H:%M")))
    head = "   ".join("%3d%%" % (b * 100) for b in BARS)
    print("%-20s %6s %8s %8s  %s" % ("camera", "night", "quietest",
                                     "dawn@%d%%" % (BAR * 100), head))
    print("-" * 82)
    seen = {b: [0, 0, 0.0] for b in BARS}     # detections, false, night hours
    cams = 0
    for p in sorted(paths):
        rows = load(p)
        name = p.rsplit("/", 1)[-1].replace("dn-isp-", "").replace(".csv", "")
        if len(rows) < 5:
            print("%-20s too few samples" % name)
            continue
        cams += 1
        res = evaluate(rows, tau_fast, tau_slow, BARS, dawn)
        hit = first_fire(rows, tau_fast, tau_slow, BAR, dawn)
        cells = []
        for b in BARS:
            det, false, hours, _ = res[b]
            seen[b][0] += 1 if det else 0
            seen[b][1] += false
            seen[b][2] += hours
            cells.append("%4.2f" % (false / hours if hours else 0))
        print("%-20s %5.1fh %8.2f %8s  %s"
              % (name, res[BAR][2], res[BAR][3],
                 hit.strftime("%H:%M") if hit else "-", "   ".join(cells)))
    print("-" * 82)
    print("%-20s %5.1fh %8s %8s  %s"
          % ("FLEET false/cam-h", seen[BARS[0]][2], "", "",
             "   ".join("%4.2f" % (seen[b][1] / seen[b][2] if seen[b][2] else 0)
                        for b in BARS)))
    print("%-20s %5s  %8s %8s  %s"
          % ("dawns found", "", "", "",
             "   ".join("%2d/%-2d" % (seen[b][0], cams) for b in BARS)))


def parse_dawn(s):
    a, _, b = s.partition("-")
    return (datetime.datetime.strptime(a, "%H:%M").time(),
            datetime.datetime.strptime(b, "%H:%M").time())


if __name__ == "__main__":
    args = sys.argv[1:]
    tf, ts, dw = TAU_FAST, TAU_SLOW, DAWN
    while args and args[0].startswith("--"):
        if args[0] == "--fast":
            tf = float(args[1]); args = args[2:]
        elif args[0] == "--slow":
            ts = float(args[1]); args = args[2:]
        elif args[0] == "--dawn":
            dw = parse_dawn(args[1]); args = args[2:]
        else:
            raise SystemExit(__doc__)
    if not args:
        raise SystemExit(__doc__)
    main(args, tf, ts, dw)
