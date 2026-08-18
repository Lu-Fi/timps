#!/usr/bin/env python3
"""dn-trend-eval.py - would a two-EMA brightening detector fire, and how often?

The proposal it evaluates: instead of probing on a schedule, watch the exposure
index for a sustained BRIGHTENING and probe only then.

    fast  = EMA of D, short time constant   (follows the scene)
    slow  = EMA of D, long time constant    (the memory)
    fire when  fast / slow  <  1 - threshold

Both sides are smoothed, so the ratio is far quieter than either signal, and
the question it asks - "has this got sustainedly brighter?" - is a RELATIVE one
the night pipeline can actually answer, unlike "is it day?", which it cannot.
A false fire costs a few seconds of dimmer image, not an audible IR-cut click,
which is what makes a generous threshold affordable here and did not in any
previous design.

What this measures is the FALSE-FIRE floor: how often the detector goes off on
a scene where nothing is happening. That is the number that decides whether a
threshold is usable, and it is the one that cannot be reasoned about from first
principles because it depends on each sensor's AGC noise.

CAVEAT, and it runs in the safe direction: the sampler behind these CSVs runs
at one sample per minute, while the daemon samples every 2 s. Thirty times
denser sampling makes the fast EMA markedly smoother for the same time
constant, so every false-fire count here is an UPPER BOUND on what the daemon
would produce.

    ./scripts/dn-trend-eval.py dn-isp-*.csv
    ./scripts/dn-trend-eval.py --fast 3 --slow 15 dn-isp-*.csv
"""
import csv
import sys

FLOOR = 256.0


def load(path):
    """(minutes_from_start, D, night) per sample - mirrors dn_read()."""
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
                             r["mode"].lower().startswith("n")))
            except (ValueError, TypeError, KeyError):
                continue
    return rows


def alpha(tau_min, dt_min):
    """EMA coefficient for a time constant, at this sample spacing."""
    return 1.0 if tau_min <= 0 else min(1.0, dt_min / tau_min)


def evaluate(rows, tau_fast, tau_slow, thresholds):
    """-> {threshold: (fires, minutes_observed, worst_ratio)}"""
    out = {th: [0, 0.0, 1.0] for th in thresholds}
    fast = slow = None
    armed = {th: True for th in thresholds}      # edge-triggered, like the daemon
    prev_t = None
    for t, d, _night in rows:
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
        for th in thresholds:
            out[th][1] = t
            out[th][2] = min(out[th][2], ratio)
            if ratio < 1.0 - th:
                if armed[th]:
                    out[th][0] += 1
                    armed[th] = False
            elif ratio > 1.0 - th * 0.5:         # re-arm with hysteresis
                armed[th] = True
    return out


def main(paths, tau_fast, tau_slow):
    ths = [0.10, 0.20, 0.30, 0.40]
    print("two-EMA brightening detector: fast tau %g min, slow tau %g min"
          % (tau_fast, tau_slow))
    print("fires per hour on the observed (quiet) scenes - lower is better;"
          " these are upper bounds, see the caveat in --help\n")
    print("%-20s %6s %7s %8s  %s" % ("camera", "span", "mode", "quietest",
                                     "   ".join("%3d%%" % (t * 100) for t in ths)))
    print("-" * 78)
    totals = {th: [0, 0.0] for th in ths}
    for p in sorted(paths):
        rows = load(p)
        name = p.rsplit("/", 1)[-1].replace("dn-isp-", "").replace(".csv", "")
        if len(rows) < 5:
            print("%-20s too few samples" % name)
            continue
        res = evaluate(rows, tau_fast, tau_slow, ths)
        span = res[ths[0]][1] / 60.0
        night = sum(1 for r in rows if r[2]) / len(rows)
        cells = []
        for th in ths:
            fires, _, _ = res[th]
            totals[th][0] += fires
            totals[th][1] += span
            cells.append("%4.1f" % (fires / span if span else 0))
        print("%-20s %5.1fh %6s %8.2f  %s"
              % (name, span, "night" if night > 0.5 else "day",
                 res[ths[-1]][2], "   ".join(cells)))
    print("-" * 78)
    print("%-20s %5.1fh %6s %8s  %s"
          % ("FLEET fires/hour", sum(t[1] for t in totals.values()) / len(ths),
             "", "",
             "   ".join("%4.1f" % (totals[th][0] / totals[th][1] if totals[th][1]
                                   else 0) for th in ths)))


if __name__ == "__main__":
    args = sys.argv[1:]
    tf, ts = 3.0, 15.0
    while args and args[0].startswith("--"):
        if args[0] == "--fast":
            tf = float(args[1]); args = args[2:]
        elif args[0] == "--slow":
            ts = float(args[1]); args = args[2:]
        else:
            raise SystemExit(__doc__)
    if not args:
        raise SystemExit(__doc__)
    main(args, tf, ts)
