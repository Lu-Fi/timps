#!/usr/bin/env python3
"""dn-replay.py - day/night replay harness (design-notes section 6).

Drives a host-built timpsd-sim through synthetic or recorded gain traces and
asserts on the decisions it makes. Two modes, per the design notes' honest
statement of the two-pipeline problem (a faithful replay needs the gain of
BOTH pipelines at each instant, because which one gets read depends on the
decision the machine under test makes - and only one is ever observable in
the field):

  (a) REGRESSION replay on synthetic two-pipeline scenarios (the workhorse):
        ./scripts/dn-replay.py scripts/dn-scenarios/<name>.json
        ./scripts/dn-replay.py --all scripts/dn-scenarios
      The scenario declares a day-pipeline curve AND a night-pipeline curve;
      the driver serves whichever one the machine's active mode selects
      (fake isp-m0 scrape file + a switch_cmd stub that records every board
      drive). Fully faithful, because the counterfactual reading exists.

  (b) INCIDENT replay on a recorded field trace (daynight.trace_path CSV):
        ./scripts/dn-replay.py --incident /path/trace.csv [--config k=v ...]
      Faithful only up to the first point where the machine's decision
      diverges from the recorded one - the recorded gain past that point
      belongs to a pipeline the machine under test may no longer be in. So
      it STOPS and reports "diverged at T" rather than continuing on
      invented data. Limited, but it answers the question every daynight
      fix commit answered by hand: would this build have diverged from the
      recorded behaviour, and when?

      Fidelity scales with trace density. Even a sub-grace probe-timing
      wobble makes the replayed machine read the OTHER pipeline for a few
      seconds around a probe, and any verify anchor latched from such a
      reading is corrupted - observed concretely: a 5 s-cadence trace of
      the 09-dawn-ramp scenario shifted the replayed brightening probe 30
      virtual seconds early, anchoring its verify on a night-pipeline 354
      instead of the recorded day-pipeline 523 and flipping the extension
      decision. For sim-recorded traces, record densely:
          make sim SIM_CFLAGS="-DMS_CLOCK_SCALE=60 -DDN_TRACE_EVERY=1 \
                               -DDN_TRACE_MAX_BYTES=8000000"
      Field traces at the default 5 s cadence remain usable, but treat a
      divergence verdict within one probe pair of a recorded probe as
      timing noise, not as a behavioural finding.

Time compression: the sim binary must be built with the virtual clock -
    make sim SIM_CFLAGS="-DMS_CLOCK_SCALE=<scale>"
and the harness must be told the same factor (per-scenario "scale" key /
--scale). All scenario times are VIRTUAL seconds; see MS_CLOCK_SCALE in
src/util.h for why no DN_* constant needs compressing.

Scenario JSON (all times virtual seconds, all gains IMP [24.8] linear):
  name, incident, description   - bookkeeping (incident = commit/date)
  scale                         - MS_CLOCK_SCALE the binary was built with
  duration_s                    - run length
  initial_mode                  - "day"|"night" (persisted image.running_mode)
  config                        - {"daynight.<key>": value, ...} overrides
  day_gain / night_gain         - [[t, gain], ...] breakpoints per pipeline
  interp                        - "log" (default, geometric - gain ramps are
                                  exponential), "linear" or "step"
  noise_pct                     - deterministic +-% jitter on the served gain
  expect:
    mode_at                     - [[t, "day"|"night"], ...]
    expected_mode               - ground-truth timeline [[t, "day"|"night"|
                                  "any"], ...]; "any" = transition window,
                                  no wrong-mode accrual
    max_wrong_mode_s            - max CONTINUOUS wrong-mode run
    max_switches                - click budget: every dn_switch (probe fire,
                                  revert, genuine flip) is one audible board
                                  drive; this is a first-class requirement
    restart_equivalence_s       - bound T for the restart-equivalence oracle
                                  (null = skip); see check_restart_equiv()
    monotonicity                - "enforce"|"warn" (default)|"skip"; see
                                  check_monotonicity()
    expect_log / forbid_log     - regexes the sim log must / must not match

Exit code 0 iff every run scenario passed. Output is plain text, no ANSI.
"""
import argparse
import json
import math
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ---------------------------------------------------------------- gain plumbing

def gain_to_units(gain):
    """linear [24.8] gain -> isp-m0 log2 units (32/stop), clamped at the
    physical floor (256 linear = units 0). The clamp IS the sensor's gain
    floor: the synthetic driver can therefore never serve a reading the
    hardware could not produce, which is what makes unsatisfiable-bar
    scenarios honest."""
    if gain <= 256.0:
        return 0
    return round(32.0 * math.log2(gain / 256.0))


def units_to_gain(units):
    return 256.0 * (2.0 ** (units / 32.0))


def interp_gain(points, t, mode):
    """piecewise interpolation over [[t, gain], ...] breakpoints."""
    if t <= points[0][0]:
        return points[0][1]
    if t >= points[-1][0]:
        return points[-1][1]
    for i in range(1, len(points)):
        t0, g0 = points[i - 1]
        t1, g1 = points[i]
        if t <= t1:
            if mode == "step" or t1 == t0:
                return g0
            f = (t - t0) / (t1 - t0)
            if mode == "linear":
                return g0 + (g1 - g0) * f
            # log (default): geometric - light ramps are exponential
            return g0 * ((g1 / g0) ** f)
    return points[-1][1]


def noise_factor(t, pct):
    """deterministic pseudo-noise, +-pct% peak; period ~7-17 s so the EMA
    smoother sees realistic AGC hunting, reproducible run to run."""
    if not pct:
        return 1.0
    n = 0.6 * math.sin(2 * math.pi * t / 17.0) + \
        0.4 * math.sin(2 * math.pi * t / 7.3)
    return 1.0 + (pct / 100.0) * n

# ---------------------------------------------------------------- sim lifecycle

class SimRun:
    """One timpsd-sim process in a private rundir with the fake isp-m0 file,
    the switch_cmd stub and the trace recorder wired up."""

    def __init__(self, binary, scale, initial_mode, config, keep=False):
        # short prefix: daynight.switch_cmd is a 64-byte field on the target
        self.dir = tempfile.mkdtemp(prefix="dnrp-")
        self.keep = keep
        self.scale = scale
        self.initial_mode = initial_mode          # "day"|"night"
        self.isp = os.path.join(self.dir, "isp-m0")
        self.switch_log = os.path.join(self.dir, "switch.log")
        self.mode_file = os.path.join(self.dir, "mode.txt")
        self.trace = os.path.join(self.dir, "trace.csv")
        self.log = os.path.join(self.dir, "sim.log")
        stub = os.path.join(self.dir, "switch.sh")
        with open(stub, "w") as f:
            f.write("#!/bin/sh\n"
                    "# board day/night script stub: record every physical\n"
                    "# drive (one line = one audible IR-cut click pair)\n"
                    "echo \"$(date +%s.%N) $1\" >> " + self.switch_log + "\n"
                    "echo \"$1\" > " + self.mode_file + "\n")
        os.chmod(stub, 0o755)
        base = {
            "rtsp.enabled": 0, "http.enabled": 0, "audio.enabled": 0,
            "video0.enabled": 0, "video1.enabled": 0, "jpeg.enabled": 0,
            "record.enabled": 0, "timelapse.enabled": 0, "motion.enabled": 0,
            "daynight.enabled": 1,
            "daynight.boot_settle_s": 2,
            "daynight.isp_path": self.isp,
            "daynight.switch_cmd": stub,
            "daynight.trace_path": self.trace,
            "image.running_mode": 1 if initial_mode == "night" else 0,
        }
        base.update(config or {})
        self.conf = os.path.join(self.dir, "run.conf")
        with open(self.conf, "w") as f:
            for k, v in base.items():
                f.write("%s = %s\n" % (k, v))
        self.binary = binary
        self.proc = None
        self.t0 = None

    def serve(self, gain, isp_mode):
        """write the fake isp-m0 scrape file atomically (rename: the scraper
        sees old or new, never a torn file)."""
        u = gain_to_units(gain)
        tmp = self.isp + ".tmp"
        with open(tmp, "w") as f:
            f.write("ISP Runing Mode : %s\n" % ("Night" if isp_mode == "night"
                                                else "Day"))
            f.write("SENSOR analog gain : %d\n" % u)
        os.replace(tmp, self.isp)

    def start(self, first_gain):
        self.serve(first_gain, self.initial_mode)
        logf = open(self.log, "w")
        self.proc = subprocess.Popen([self.binary, "-c", self.conf],
                                     stdout=logf, stderr=subprocess.STDOUT,
                                     cwd=self.dir)
        self.t0 = time.monotonic()

    def vnow(self):
        return (time.monotonic() - self.t0) * self.scale

    def cur_mode(self):
        try:
            with open(self.mode_file) as f:
                m = f.read().strip()
            return m if m in ("day", "night") else self.initial_mode
        except OSError:
            return self.initial_mode

    def stop(self):
        if self.proc:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
            self.proc = None

    def switches(self):
        """[(t_virtual, mode), ...] - every physical board drive."""
        out = []
        try:
            with open(self.switch_log) as f:
                for line in f:
                    parts = line.split()
                    if len(parts) == 2:
                        out.append((self._real_to_virtual(float(parts[0])),
                                    parts[1]))
        except OSError:
            pass
        return out

    def _real_to_virtual(self, epoch):
        # switch.log carries wall-clock epoch; anchor on our own spawn stamp
        if not hasattr(self, "_epoch0"):
            self._epoch0 = None
        if self._epoch0 is None:
            self._epoch0 = time.time() - (time.monotonic() - self.t0)
        return (epoch - self._epoch0) * self.scale

    def cleanup(self):
        if self.keep:
            print("  rundir kept: %s" % self.dir)
        else:
            shutil.rmtree(self.dir, ignore_errors=True)

# ---------------------------------------------------------------- mode timeline

def mode_timeline(initial_mode, switches, end_t):
    """[(t_from, t_to, mode), ...] covering [0, end_t]. Before the first
    dn_switch the ISP renders the PERSISTED mode (that is what adoption
    encodes), so the initial segment is initial_mode."""
    segs = []
    cur, t = initial_mode, 0.0
    for st, sm in switches:
        st = max(st, t)
        if st > t:
            segs.append((t, st, cur))
        cur, t = sm, st
    if end_t > t:
        segs.append((t, end_t, cur))
    return segs


def mode_at(segs, t):
    for a, b, m in segs:
        if a <= t < b:
            return m
    return segs[-1][2] if segs else None


def expected_at(timeline, t):
    cur = timeline[0][1]
    for tt, m in timeline:
        if t >= tt:
            cur = m
        else:
            break
    return cur

# ---------------------------------------------------------------- checks

class Report:
    def __init__(self, name):
        self.name = name
        self.lines = []
        self.failed = False

    def check(self, ok, label, detail, warn_only=False):
        if ok:
            self.lines.append("  PASS  %-24s %s" % (label, detail))
        elif warn_only:
            self.lines.append("  WARN  %-24s %s" % (label, detail))
        else:
            self.lines.append("  FAIL  %-24s %s" % (label, detail))
            self.failed = True

    def note(self, label, detail):
        self.lines.append("  info  %-24s %s" % (label, detail))

    def emit(self):
        print("scenario %s" % self.name)
        for line in self.lines:
            print(line)
        print("RESULT: %s %s" % ("FAIL" if self.failed else "PASS", self.name))
        return not self.failed


def wrong_mode_runs(segs, expected, samples):
    """max continuous wrong-mode run and total, sampled on the driver grid.
    'any' expected windows accrue nothing."""
    max_run = total = run = 0.0
    prev_t = None
    for t in samples:
        exp = expected_at(expected, t)
        m = mode_at(segs, t)
        dt = 0.0 if prev_t is None else t - prev_t
        prev_t = t
        if exp != "any" and m is not None and m != exp:
            run += dt
            total += dt
            max_run = max(max_run, run)
        else:
            run = 0.0
    return max_run, total


def check_restart_equiv(rep, segs, fed, day_thr, night_thr, T):
    """Restart-equivalence oracle (design-notes section 3): where the
    evidence is DECISIVE, a decisive divergence between the cold-start
    verdict and the machine's mode must not persist longer than T.

    The "unambiguously" qualifier, made concrete:
     - machine=day, evidence > night_thr: always counts - the day pipeline
       is the trustworthy metric and a wrong day has no other way back.
     - machine=night, evidence < day_thr: counts only while the evidence is
       FRESH - i.e. not already proven ambiguous by a failed physical probe
       at (or above) this level. A failed probe at smoothed level L
       establishes that night-pipeline readings >= 0.97*L mean darkness
       (the ratchet-anchor rule, 14a1d61); only a reading brighter than
       that is new evidence a cold start could act on that the running
       machine has not already tested. Without this qualifier the oracle
       would flag the inverted-regime cameras (a5dae07: resting night gain
       256-268 under IR vs day threshold 300) where the cold-start verdict
       is decisively WRONG and the machine's history is right.
    fed: [(t, gain, machine_mode_at_t)] from the driver."""
    run_start = None
    worst = (0.0, None)
    fail_L = None            # last failed-probe evidence level
    probe_pre_gain = None    # NIGHT-pipeline gain just before a probe fired
    last_night_gain = None
    last_mode = None
    for t, g, m in fed:
        if last_mode is not None and m != last_mode:
            if m == "day":
                # the level the machine's own ratchet latches is the
                # PRE-probe night-pipeline reading, not the day-railed
                # value the pipeline reports after the switch
                probe_pre_gain = last_night_gain
            elif m == "night":
                # a switch back to night after a probe = the probe failed
                # (fast revert or unconfirmed day - both latch the ratchet)
                if probe_pre_gain is not None:
                    fail_L = probe_pre_gain
                probe_pre_gain = None
        last_mode = m
        if m == "night":
            last_night_gain = g
        verdict = None
        if g > night_thr:
            verdict = "night"
        elif g < day_thr:
            verdict = "day"
        diverged = False
        if verdict and verdict != m:
            if m == "day":
                diverged = True
            elif m == "night" and verdict == "day":
                diverged = fail_L is None or g < 0.97 * fail_L
        if diverged:
            if run_start is None:
                run_start = t
            if t - run_start > worst[0]:
                worst = (t - run_start, run_start)
        else:
            run_start = None
    ok = worst[0] <= T
    rep.check(ok, "restart-equivalence",
              "longest decisive divergence %.0fs (bound %.0fs)%s"
              % (worst[0], T,
                 "" if worst[1] is None else ", from t=%.0fs" % worst[1]))


def check_monotonicity(rep, trace_path, policy):
    """Probe-time monotonicity (design-notes section 5): within one
    continuous night dwell, strictly brighter evidence must never be granted
    a LATER correction. Read the trace's verify re-arm events (verify_in_s
    jumping up while cur==night) and compare (evidence, granted delay)
    pairs. LIMITATION, stated honestly: only the armed verify deadline is
    visible in the trace - the brightening hold's implicit schedule is not -
    so this observes the periodic/adopt path only. The property becomes
    fully checkable once dn_next_probe() exists (next session's work), which
    is also why the default policy is "warn": probe_backoff's trend-
    blindness (generator E, known open) violates this by design today."""
    if policy == "skip":
        rep.note("monotonicity", "skipped by scenario")
        return
    try:
        rows = []
        with open(trace_path) as f:
            next(f)
            for line in f:
                p = line.strip().split(",")
                if len(p) == 11:
                    rows.append(p)
    except (OSError, StopIteration):
        rep.note("monotonicity", "no trace (binary without recorder?) - skipped")
        return
    events = []     # (dwell_id, evidence smooth_tg, granted delay s)
    dwell = 0
    prev_cur, prev_in = None, None
    for p in rows:
        cur, smooth, vmode, vin = int(p[1]), float(p[5]), int(p[8]), int(p[9])
        if cur != prev_cur:
            dwell += 1
            prev_in = None
        prev_cur = cur
        if cur == 1 and vmode == 1 and vin >= 0 and smooth > 0:
            if prev_in is None or vin > prev_in + 2:   # fresh (re-)arm
                events.append((dwell, smooth, vin))
            prev_in = vin
        else:
            prev_in = None
    viol = []
    for i in range(len(events)):
        for j in range(i + 1, len(events)):
            if events[i][0] != events[j][0]:
                continue
            _, e1, d1 = events[i]
            _, e2, d2 = events[j]
            if e2 < 0.98 * e1 and d2 > d1 * 1.05 + 10:
                viol.append("evidence %.0f->%.0f but delay %ds->%ds"
                            % (e1, e2, d1, d2))
    rep.check(not viol, "probe-monotonicity",
              "%d re-arm events, %d violations%s"
              % (len(events), len(viol),
                 (": " + viol[0]) if viol else ""),
              warn_only=(policy != "enforce"))

# ---------------------------------------------------------------- regression

def run_regression(scn, binary, keep=False, scale_override=None):
    rep = Report(scn["name"])
    scale = scale_override or scn.get("scale", 30)
    dur = scn["duration_s"]
    interp = scn.get("interp", "log")
    noise = scn.get("noise_pct", 0)
    exp = scn.get("expect", {})
    sim = SimRun(binary, scale, scn["initial_mode"], scn.get("config"),
                 keep=keep)
    fed = []
    try:
        g0 = interp_gain(scn["night_gain" if scn["initial_mode"] == "night"
                             else "day_gain"], 0, interp)
        sim.start(g0)
        step = max(0.25 / scale, 0.002)      # 250 virtual ms per driver step
        while True:
            t = sim.vnow()
            if t >= dur:
                break
            m = sim.cur_mode()
            curve = scn["night_gain"] if m == "night" else scn["day_gain"]
            g = interp_gain(curve, t, interp) * noise_factor(t, noise)
            g = max(g, 256.0)                # sensor floor, see gain_to_units
            sim.serve(g, m)
            fed.append((t, units_to_gain(gain_to_units(g)), m))
            time.sleep(step)
        sim.stop()

        switches = sim.switches()
        segs = mode_timeline(scn["initial_mode"], switches, dur)
        rep.note("run", "%.0f virtual s at scale %d, %d board switches"
                 % (dur, scale, len(switches)))
        for st, sm in switches:
            rep.note("switch", "t=%.0fs -> %s" % (st, sm))

        for t, want in exp.get("mode_at", []):
            got = mode_at(segs, t)
            rep.check(got == want, "mode@%ds" % t,
                      "want %s, got %s" % (want, got))
        if "max_switches" in exp:
            rep.check(len(switches) <= exp["max_switches"], "click-budget",
                      "%d switches (budget %d)"
                      % (len(switches), exp["max_switches"]))
        if "expected_mode" in exp and "max_wrong_mode_s" in exp:
            samples = [f[0] for f in fed]
            mx, tot = wrong_mode_runs(segs, exp["expected_mode"], samples)
            rep.check(mx <= exp["max_wrong_mode_s"], "wrong-mode",
                      "longest run %.0fs, total %.0fs (bound %.0fs)"
                      % (mx, tot, exp["max_wrong_mode_s"]))
        T = exp.get("restart_equivalence_s")
        if T:
            cfg = scn.get("config", {})
            day_thr = float(cfg.get("daynight.total_gain_day_threshold", 300))
            night_thr = float(cfg.get("daynight.total_gain_night_threshold",
                                      3000))
            check_restart_equiv(rep, segs, fed, day_thr, night_thr, T)
        check_monotonicity(rep, sim.trace,
                           exp.get("monotonicity", "warn"))
        logtext = ""
        try:
            with open(sim.log) as f:
                logtext = f.read()
        except OSError:
            pass
        for rx in exp.get("expect_log", []):
            rep.check(re.search(rx, logtext) is not None, "expect-log",
                      repr(rx))
        for rx in exp.get("forbid_log", []):
            rep.check(re.search(rx, logtext) is None, "forbid-log",
                      repr(rx))
    finally:
        sim.stop()
        ok = rep.emit()
        sim.cleanup()
    return ok

# ---------------------------------------------------------------- incident

def load_trace(path):
    """recorded daynight.trace_path CSV -> [(t_s from first row, cur, tg)]"""
    rows = []
    with open(path) as f:
        header = f.readline()
        if not header.startswith("t_mono_ms"):
            raise SystemExit("not a daynight trace: %s" % path)
        for line in f:
            p = line.strip().split(",")
            if len(p) == 11:
                rows.append((int(p[0]), int(p[1]), float(p[2])))
    if not rows:
        raise SystemExit("empty trace: %s" % path)
    t0 = rows[0][0]
    return [((t - t0) / 1000.0, cur, tg) for t, cur, tg in rows]


def run_incident(path, binary, config, scale, grace, keep=False,
                 initial_mode=None):
    """Mode (b): feed the recorded single-pipeline gain and stop at the first
    sustained decision divergence. The recorded gain past that point belongs
    to a pipeline the machine under test may not be in - continuing would be
    replaying invented data, so we do not."""
    rep = Report("incident:%s" % os.path.basename(path))
    rows = load_trace(path)
    if initial_mode is None:
        first = next((c for _, c, _ in rows if c in (0, 1)), None)
        if first is None:
            raise SystemExit("trace has no decided mode; pass --initial-mode")
        initial_mode = "night" if first == 1 else "day"
    dur = rows[-1][0]
    sim = SimRun(binary, scale, initial_mode, config, keep=keep)
    diverged = None
    try:
        sim.start(rows[0][2])
        step = max(0.25 / scale, 0.002)
        i = 0
        mismatch_since = None
        while True:
            t = sim.vnow()
            if t >= dur:
                break
            while i + 1 < len(rows) and rows[i + 1][0] <= t:
                i += 1
            rec_t, rec_cur, rec_tg = rows[i]
            m = sim.cur_mode()
            sim.serve(max(rec_tg, 256.0) if rec_tg > 0 else 256.0, m)
            rec_mode = ("night" if rec_cur == 1 else
                        "day" if rec_cur == 0 else None)
            if rec_mode and m != rec_mode:
                if mismatch_since is None:
                    mismatch_since = t
                elif t - mismatch_since > grace:
                    diverged = (mismatch_since, rec_mode, m)
                    break
            else:
                mismatch_since = None
            time.sleep(step)
        sim.stop()
        if diverged:
            t, rec_m, got_m = diverged
            print("scenario %s" % rep.name)
            print("  DIVERGED at t=%.0fs (virtual): recorded=%s, "
                  "machine-under-test=%s" % (t, rec_m, got_m))
            print("  replay stopped there - the recorded gain past this "
                  "point belongs to the recorded machine's pipeline choice, "
                  "not this build's.")
            print("RESULT: DIVERGED %s" % rep.name)
        else:
            print("scenario %s" % rep.name)
            print("  replay complete: %d samples, %.0f virtual s, no "
                  "sustained divergence (grace %.0fs)"
                  % (len(rows), dur, grace))
            print("RESULT: PASS %s" % rep.name)
    finally:
        sim.stop()
        sim.cleanup()
    return diverged is None

# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("scenario", nargs="?", help="scenario JSON file")
    ap.add_argument("--all", metavar="DIR",
                    help="run every *.json scenario in DIR")
    ap.add_argument("--incident", metavar="TRACE",
                    help="incident replay of a recorded trace CSV")
    ap.add_argument("--bin", default=os.path.join(REPO, "timpsd-sim"),
                    help="sim binary (default: repo timpsd-sim)")
    ap.add_argument("--scale", type=int, default=None,
                    help="MS_CLOCK_SCALE the binary was built with "
                         "(overrides the scenario's value)")
    ap.add_argument("--config", action="append", default=[], metavar="K=V",
                    help="config override (incident mode: thresholds etc "
                         "must match the recording camera)")
    ap.add_argument("--grace", type=float, default=30.0,
                    help="incident mode: virtual seconds of mode mismatch "
                         "tolerated (hysteresis/dwell lag, probe-timing "
                         "wobble) before declaring divergence")
    ap.add_argument("--initial-mode", choices=["day", "night"], default=None)
    ap.add_argument("--keep", action="store_true", help="keep rundirs")
    args = ap.parse_args()

    if not os.path.isfile(args.bin) or not os.access(args.bin, os.X_OK):
        raise SystemExit("sim binary not found/executable: %s "
                         "(build: make sim SIM_CFLAGS=-DMS_CLOCK_SCALE=N)"
                         % args.bin)
    cfg = {}
    for kv in args.config:
        k, _, v = kv.partition("=")
        cfg[k] = v

    if args.incident:
        ok = run_incident(args.incident, args.bin, cfg, args.scale or 30,
                          args.grace, keep=args.keep,
                          initial_mode=args.initial_mode)
        sys.exit(0 if ok else 1)

    files = []
    if args.all:
        files = sorted(os.path.join(args.all, f)
                       for f in os.listdir(args.all) if f.endswith(".json"))
    elif args.scenario:
        files = [args.scenario]
    else:
        ap.error("give a scenario file, --all DIR or --incident TRACE")

    passed = failed = 0
    for path in files:
        with open(path) as f:
            scn = json.load(f)
        scn["config"] = {**scn.get("config", {}), **cfg}
        if run_regression(scn, args.bin, keep=args.keep,
                          scale_override=args.scale):
            passed += 1
        else:
            failed += 1
        print()
    print("corpus: %d passed, %d failed" % (passed, failed))
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
