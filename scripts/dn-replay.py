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
src/util.h for why no DN_* constant needs compressing. The binary's actual
scale is verified against the scenario's at startup (SimRun.check_clock) and a
mismatch is fatal - a binary that ignores the flag runs in real time and
passes everything vacuously, which reads exactly like "this scenario does not
discriminate". Read check_clock()'s comment before bisecting a historical fix.

Scenario JSON (all times virtual seconds, all gains IMP [24.8] linear):
  name, incident, description   - bookkeeping (incident = commit/date)
  scale                         - MS_CLOCK_SCALE the binary was built with
  duration_s                    - run length
  initial_mode                  - "day"|"night" (persisted image.running_mode)
  config                        - {"daynight.<key>": value, ...} overrides
  day_gain / night_gain         - [[t, gain], ...] breakpoints per pipeline
  no_ceilings                   - OPTIONAL bool: omit the MAX gain lines from
                                  the fake isp-m0 dump, so the daemon's AE
                                  reserve stays unknown (hr=-1). Models the
                                  dump variant without ceiling fields, where
                                  every clip protection is structurally
                                  absent - the case dn_ceiling_check() warns
                                  about.
  isp_sticky                    - OPTIONAL {"stuck": "day"|"night"}: the ISP
                                  refuses to leave the stuck mode. switch_cmd
                                  still runs (the click is still recorded),
                                  but the served "ISP Runing Mode" - and the
                                  served pipeline - stay stuck until one
                                  switch TO the stuck mode arrives; after
                                  that edge the ISP follows normally. The
                                  harness form of the latch defect class
                                  (hal_ingenic.c): re-asserting a believed
                                  value is a no-op, only a real transition
                                  acts - cam-wohn 2026-08-21. With isp_sticky
                                  the mode_at / expected_mode assertions
                                  judge the RENDERED timeline (what was
                                  served), not the switch log, because with a
                                  stuck ISP the two are exactly what differ.
  isp_override                  - OPTIONAL [[t, "day"|"night"|null], ...]:
                                  from t on, the served "ISP Runing Mode"
                                  (and pipeline) is FORCED to the given mode
                                  regardless of switch_cmd; null releases the
                                  force. Models an operator driving the board
                                  script by hand while the daemon stands on
                                  its own decision - the cam-wohn-ofen
                                  reverse-desync of 2026-08-21. Overrides win
                                  over isp_sticky while active; like sticky,
                                  the mode assertions judge the rendered
                                  timeline.
  night_gain_noir               - OPTIONAL [[t, gain], ...]: the NIGHT pipeline
                                  with the IR illuminator switched OFF. This is
                                  the third optical state the 2026-08-17
                                  decision turns on - the daemon briefly kills
                                  the illuminator and compares, so a scenario
                                  that does not supply this curve cannot
                                  exercise the ratio path at all - and so the
                                  harness does not enable it there: without
                                  this curve daynight.irprobe_cmd is left
                                  unset and the scenario tests the audible
                                  IR-cut path it was written for. Supplying
                                  the curve is what opts a scenario in.
  day_int_ratio / night_int_ratio
                                - OPTIONAL [[t, ratio], ...] integration-time
                                  ratio (integration/max, 0..1) per pipeline.
                                  Omitted = the isp-m0 dump carries no
                                  integration-time fields at all, which is
                                  what the pre-2026-08-17 harness produced and
                                  what makes the exposure index degrade to
                                  bare gain - so every scenario written before
                                  the redesign keeps feeding exactly what it
                                  always fed. Supply it to exercise the range
                                  the index has BELOW the 256 gain floor, i.e.
                                  the IR-saturated scenes bare gain cannot
                                  see. The decision metric is gain * ratio.
  interp                        - "log" (default, geometric - gain ramps are
                                  exponential), "linear" or "step"
  noise_pct                     - stochastic +-% AGC jitter (2 sigma), seeded
  noise_seed                    - PRNG seed (default 0); vary to re-roll a run
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

# Ceilings of the synthetic sensor, in isp-m0 log2 units (32 = one stop).
# 320 units is ten stops of analog range, comfortably more than any scene in
# the corpus needs, so a scenario only runs out of reserve when its own curve
# is written to rail - which is exactly how the real pegged cameras behave.
MAX_AGAIN, MAX_IDGAIN = 320, 64


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


def noise_factor(t, pct, seed=0):
    """STOCHASTIC AGC noise, +-pct% (1 sigma ~ pct/2), seeded so a run is still
    reproducible.

    This was a bounded sum of two sinusoids until 2026-08-16, and that choice
    silently disarmed the corpus against a whole class of defect. A deterministic
    periodic signal has a fixed, finite set of extremes: the running minimum of
    anything derived from it converges after one period and then stops moving.
    So any bug whose behaviour depends on an ORDER STATISTIC over a long window
    - "the lowest value seen since X" - cannot be exhibited at all, no matter how
    long the scenario runs or how high noise_pct goes. min_smooth_since_probe
    shipped with exactly that bug (35af4c9): on real hardware its running minimum
    descends without bound in the number of samples, defeating the reconfirm
    backoff completely on a static scene, and the corpus reported zero click
    regressions across all eleven scenarios while it did so.

    Two properties matter for a replacement and both are deliberate:
      - it must be genuinely stochastic (fresh draws, unbounded tails) so
        window-dependent statistics can drift the way they do in the field;
      - it must be reproducible, or a corpus failure cannot be bisected. Hence a
        seeded PRNG keyed on the sample index rather than random.random().

    The AR(1) term gives it the short autocorrelation real AGC hunting has (an
    ISP does not redraw its gain independently every 500 ms), which is what makes
    the EMA smoother see something realistic rather than white noise it can
    trivially average away."""
    if not pct:
        return 1.0
    # deterministic per-(seed, tick) draws: splitmix64-style avalanche
    idx = int(round(t * 2.0))            # the 500 ms sample grid
    def draw(k):
        x = (idx * 0x9E3779B97F4A7C15 + k * 0xBF58476D1CE4E5B9
             + seed * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
        x ^= x >> 30; x = (x * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
        x ^= x >> 27; x = (x * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
        x ^= x >> 31
        return x / float(1 << 64)        # uniform [0,1)
    # Box-Muller -> gaussian, sigma = pct/2 so +-pct is about 2 sigma
    u1 = max(draw(1), 1e-12)
    u2 = draw(2)
    g = math.sqrt(-2.0 * math.log(u1)) * math.cos(2.0 * math.pi * u2)
    # AR(1) with the previous grid point, rho ~ 0.6: short-memory hunting
    u1p = max(draw(3), 1e-12)
    gp = math.sqrt(-2.0 * math.log(u1p)) * math.cos(2.0 * math.pi * draw(4))
    g = 0.6 * gp + 0.8 * g
    return 1.0 + (pct / 100.0) * 0.5 * g

# ---------------------------------------------------------------- sim lifecycle

class SimRun:
    """One timpsd-sim process in a private rundir with the fake isp-m0 file,
    the switch_cmd stub and the trace recorder wired up."""

    def __init__(self, binary, scale, initial_mode, config, keep=False,
                 model_illuminator=False, no_ceilings=False):
        self.no_ceilings = no_ceilings
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
        # The illuminator stub. Deliberately a SEPARATE script from switch.sh:
        # on real hardware these are different mechanisms - the IR-cut is a
        # motor and audible, the illuminator is a GPIO write and silent - and
        # conflating them in the harness would hide exactly the cost difference
        # the redesign turns on. Every call is logged with its argument, so a
        # scenario can assert "n silent probes, m audible clicks" separately.
        self.ir_log = os.path.join(self.dir, "irprobe.log")
        self.ir_file = os.path.join(self.dir, "ir.txt")
        irstub = os.path.join(self.dir, "irprobe.sh")
        with open(irstub, "w") as f:
            f.write("#!/bin/sh\n"
                    "echo \"$(date +%s.%N) $1\" >> " + self.ir_log + "\n"
                    "echo \"$1\" > " + self.ir_file + "\n")
        os.chmod(irstub, 0o755)
        with open(self.ir_file, "w") as f:
            f.write("on\n")
        base = {
            "rtsp.enabled": 0, "http.enabled": 0, "audio.enabled": 0,
            "video0.enabled": 0, "video1.enabled": 0, "jpeg.enabled": 0,
            "record.enabled": 0, "timelapse.enabled": 0, "motion.enabled": 0,
            "daynight.enabled": 1,
            # The per-probe diagnostics (ratio verdicts, the structured probe
            # line, the exposure-vs-reference note) are LOGD: an end user has
            # no use for a line per tick. Scenario expect_log patterns match
            # several of them, so the harness turns the module on explicitly
            # rather than raising the global level and pulling in every other
            # module's debug output.
            "general.debug_modules": "daynight",
            # No boot_settle_s here: it became the fixed DN_BOOT_SETTLE_S=5 on
            # 2026-08-22 and can no longer be overridden. The line that used to
            # set 2 kept being parsed, kept being ignored, and only added a
            # config WRN to every scenario's sim.log.
            "daynight.isp_path": self.isp,
            "daynight.switch_cmd": stub,
            "daynight.trace_path": self.trace,
            "image.running_mode": 1 if initial_mode == "night" else 0,
        }
        # The illuminator command is only handed to the daemon when the
        # scenario actually models what happens when the light goes off.
        # Enabling it otherwise is not a neutral default: the driver would
        # keep serving the ordinary night curve, the ratio would come back
        # exactly 1.0, and - with AE reserve available - that reads as "the
        # room supplies the light", so every legacy scenario would switch to
        # day on its first probe. A scenario without a night_gain_noir curve
        # is not modelling a pegged camera, it is modelling nothing at all,
        # and must therefore exercise the audible path it was written for.
        # Setting it is not enough: since the compiled default became a real
        # command, an unset key inherits it, and a legacy scenario would arm
        # path T with nothing to feed it. Clear it explicitly.
        base["daynight.irprobe_cmd"] = irstub if model_illuminator else ""
        base.update(config or {})
        self.conf = os.path.join(self.dir, "run.conf")
        with open(self.conf, "w") as f:
            for k, v in base.items():
                f.write("%s = %s\n" % (k, v))
        self.binary = binary
        self.proc = None
        self.t0 = None

    def serve(self, gain, isp_mode, ratio=None):
        """write the fake isp-m0 scrape file atomically (rename: the scraper
        sees old or new, never a torn file).

        ratio=None deliberately omits the integration-time fields entirely,
        which is what this harness always did: the exposure index then equals
        total_gain and every pre-2026-08-17 scenario keeps its meaning. A
        ratio drives the other half of the metric - MAX_INT is a round 1000
        "lines" so the served ratio is exact to a part in a thousand."""
        u = gain_to_units(gain)
        tmp = self.isp + ".tmp"
        with open(tmp, "w") as f:
            f.write("ISP Runing Mode : %s\n" % ("Night" if isp_mode == "night"
                                                else "Day"))
            f.write("SENSOR analog gain : %d\n" % u)
            # The ceilings. Without them the daemon cannot compute how much
            # room the AE still has, and the ratio verdict has no way to tell
            # "the room supplies the light" from "my meter is pegged and
            # cannot answer" - the two look identical (r ~= 1). A synthetic
            # camera therefore has to declare its limits like a real one -
            # unless the scenario is ABOUT a camera that does not
            # (no_ceilings, scenario 25).
            if not self.no_ceilings:
                f.write("MAX SENSOR analog gain : %d\n" % MAX_AGAIN)
                f.write("MAX ISP digital gain : %d\n" % MAX_IDGAIN)
            if ratio is not None:
                mx = 1000
                it = max(1, min(mx, int(round(ratio * mx))))
                f.write("SENSOR Integration Time : %d lines\n" % it)
                f.write("SENSOR Max Integration Time : %d lines\n" % mx)
        os.replace(tmp, self.isp)

    def start(self, first_gain, first_ratio=None):
        self.serve(first_gain, self.initial_mode, first_ratio)
        logf = open(self.log, "w")
        self.proc = subprocess.Popen([self.binary, "-c", self.conf],
                                     stdout=logf, stderr=subprocess.STDOUT,
                                     cwd=self.dir)
        self.t0 = time.monotonic()
        self.check_clock()

    def check_clock(self):
        """Verify the binary actually applies the virtual clock we are driving
        it at. THIS CHECK IS LOAD-BEARING, not hygiene. A binary built without
        the -DMS_CLOCK_SCALE hook (any tree older than e06bf41, which is every
        pre-fix tree a negative test would want to build) silently ignores
        SIM_CFLAGS and runs in REAL time, while this driver keeps feeding the
        scenario `scale` times faster than the machine experiences it. The
        machine then never reaches a single one of its own deadlines - the
        heartbeat, the probe verdict, the confirmation windows (and on a
        pre-2026-08-17 tree, night_reconfirm_s and the backoff) - so the
        incident cascade the scenario exists to reproduce cannot occur, and the run
        comes back GREEN on every behavioural assertion. That false negative
        is indistinguishable by eye from "this scenario does not discriminate",
        and it has already been mistaken for exactly that once. The tell was
        visible but subtle (first switch at t=451s instead of t=8s: 7.5 real
        seconds of boot settle multiplied by a scale the binary never
        applied), so we do not rely on anyone spotting it - the sim announces
        its scale in its startup banner (see MS_CLOCK_SCALE in src/main.c) and
        a mismatch is a hard error."""
        want = "MS_CLOCK_SCALE=%d" % self.scale
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            try:
                with open(self.log) as f:
                    text = f.read()
            except OSError:
                text = ""
            if want in text:
                return
            if "starting (backend=" in text and "MS_CLOCK_SCALE" not in text:
                raise SystemExit(
                    "sim binary %s runs on the REAL clock - it was built "
                    "without -DMS_CLOCK_SCALE, so SIM_CFLAGS was silently "
                    "dropped (no such hook before e06bf41). Driving it at "
                    "scale %d would replay the scenario %dx faster than the "
                    "machine experiences it: every deadline becomes "
                    "unreachable, the incident never reproduces, and the run "
                    "comes back falsely green. Rebuild with:\n"
                    "  make sim SIM_CFLAGS=\"-DMS_CLOCK_SCALE=%d\"\n"
                    "To bisect a historical fix, revert the fix's hunk in a "
                    "copy of the CURRENT tree instead of building the old "
                    "tree - that keeps the clock/trace contract the harness "
                    "depends on while isolating the behaviour under test."
                    % (self.binary, self.scale, self.scale, self.scale))
            m = re.search(r"MS_CLOCK_SCALE=(\d+)", text)
            if m:
                raise SystemExit(
                    "sim binary %s was built with MS_CLOCK_SCALE=%s but the "
                    "scenario asks for %d - rebuild, or pass --scale %s"
                    % (self.binary, m.group(1), self.scale, m.group(1)))
            time.sleep(0.05)
        raise SystemExit("sim binary %s produced no startup banner within 5s"
                         % self.binary)

    def vnow(self):
        return (time.monotonic() - self.t0) * self.scale

    def ir_on(self):
        """Is the illuminator currently lit, per the stub's own record?"""
        try:
            with open(self.ir_file) as f:
                return f.read().strip() != "off"
        except OSError:
            return True

    def ir_probes(self):
        """[(t_virtual, 'off'|'on')] - every silent illuminator drive."""
        out = []
        try:
            with open(self.ir_log) as f:
                for line in f:
                    ts, _, arg = line.strip().partition(" ")
                    out.append((self._real_to_virtual(float(ts)), arg))
        except (OSError, ValueError):
            pass
        return out

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
        self.aborted = False    # harness precondition failed: no verdict at all
        self.checks = 0

    def check(self, ok, label, detail, warn_only=False):
        self.checks += 1
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
        if self.aborted or not self.checks:
            # a precondition failed: there is no verdict to report, and
            # printing a bare "RESULT: PASS" with zero checks under it would
            # be actively misleading. `aborted` covers the virtual-clock
            # handshake, which raises SystemExit; the zero-check test covers
            # everything else that can throw out of the run - emit() is called
            # from a finally, so a sim binary that could not even be spawned
            # used to report a green scenario with no assertions in it.
            print("scenario %s: ABORTED, no assertions ran" % self.name)
            return False
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


def check_restart_equiv(rep, day_at, segs, samples, day_thr, night_thr, T):
    """Restart-equivalence oracle (design-notes section 3): if a cold start
    on the present evidence would decide mode M unambiguously, the running
    machine must converge to M within a bounded time T.

    Rewritten 2026-08-17 to ask the question DIRECTLY. Since the redesign a
    cold start is a defined, mechanical act - boot into the day pipeline
    (persisted day: read it; persisted night: probe it) and compare the
    DAY-pipeline exposure against day_gain/night_gain - so the oracle can
    evaluate exactly that counterfactual at every instant from the
    scenario's own day-pipeline curve, which the harness has and the field
    never does. That removes the previous version's ratchet-margin fudge,
    which existed only to stop the oracle flagging inverted-regime cameras
    where the NIGHT-pipeline reading it was forced to use was misleading.
    The day-pipeline curve is not misleading, so no fudge is needed:

      cold-start verdict at t = day  if day_curve(t) < day_thr
                                night if day_curve(t) > night_thr
                                (in between: not unambiguous, accrues nothing)

    day_at: callable t -> the day-pipeline exposure at t."""
    run_start = None
    worst = (0.0, None)
    for t in samples:
        g = day_at(t)
        verdict = ("day" if g < day_thr else
                   "night" if g > night_thr else None)
        if verdict and verdict != mode_at(segs, t):
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
    a LATER correction.

    Since the redesign there is exactly ONE schedule to observe - the
    heartbeat deadline - instead of the previous verify/backoff/ratchet
    tangle, so this reads (smoothed exposure, seconds until the next
    heartbeat) at every fresh re-arm and asserts the pairs are ordered. The
    old pure-function property test that used to be the authority here
    (tests/dn-probe-props.c over dn_next_probe) is gone with the function it
    tested: the schedule is now three predicates over four scalars, and the
    thing actually worth asserting is the end-to-end behaviour this observes.
    It is therefore promoted from a sanity observer to the check, though the
    default stays "warn" because a trajectory can only visit the evidence its
    own decisions lead it to."""
    if policy == "skip":
        rep.note("monotonicity", "skipped by scenario")
        return
    try:
        rows = []
        with open(trace_path) as f:
            next(f)
            for line in f:
                p = line.strip().split(",")
                if len(p) >= 10:          # see load_trace(): columns append
                    rows.append(p)
    except (OSError, StopIteration):
        rep.note("monotonicity", "no trace (binary without recorder?) - skipped")
        return
    events = []     # (dwell_id, evidence, granted delay s)
    dwell = 0
    prev_cur, prev_in = None, None
    for p in rows:
        cur, smooth, hb_in = int(p[1]), float(p[5]), int(p[9])
        if cur != prev_cur:
            dwell += 1
            prev_in = None
        prev_cur = cur
        if cur == 1 and hb_in >= 0 and smooth > 0:
            if prev_in is None or hb_in > prev_in + 2:   # fresh (re-)arm
                events.append((dwell, smooth, hb_in))
            prev_in = hb_in
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
    nseed = scn.get("noise_seed", 0)
    exp = scn.get("expect", {})
    sim = SimRun(binary, scale, scn["initial_mode"], scn.get("config"),
                 keep=keep,
                 model_illuminator=bool(scn.get("night_gain_noir")),
                 no_ceilings=bool(scn.get("no_ceilings")))
    def ratio_at(mode, t):
        c = scn.get("night_int_ratio" if mode == "night" else "day_int_ratio")
        return None if not c else max(0.0, min(1.0, interp_gain(c, t, "linear")))

    sticky = scn.get("isp_sticky")           # {"stuck": "day"|"night"}
    override = scn.get("isp_override")       # [[t, "day"|"night"|null], ...]

    def override_at(t):
        v = None
        for tt, mm in (override or []):
            if t >= tt:
                v = mm
        return v

    def isp_mode_now():
        """The mode the ISP actually renders. Without isp_sticky that is
        simply the last switch_cmd drive. With it, switches AWAY from the
        stuck mode are dropped until one switch TO the stuck mode has been
        seen - only a transition acts on a stuck ISP."""
        ov = override_at(sim.vnow())
        if ov:
            return ov                        # forced from outside
        if not sticky:
            return sim.cur_mode()
        stuck = sticky["stuck"]
        mode, released = sim.initial_mode, False
        for _, mm in sim.switches():
            if released:
                mode = mm
            elif mm == stuck:
                released, mode = True, stuck
            else:
                mode = stuck                 # dropped: the ISP holds
        return mode

    fed = []
    try:
        m0 = scn["initial_mode"]
        g0 = interp_gain(scn["night_gain" if m0 == "night" else "day_gain"],
                         0, interp)
        sim.start(g0, ratio_at(m0, 0))
        step = max(0.25 / scale, 0.002)      # 250 virtual ms per driver step
        while True:
            t = sim.vnow()
            if t >= dur:
                break
            m = isp_mode_now()
            # THREE optical states, not two. The illuminator is independent of
            # the IR-cut filter, so a camera in night mode with the IR switched
            # off is a third thing entirely - and it is the state the whole
            # ratio decision reads. Serving the ordinary night curve there
            # would make every probe return r == 1 and quietly render the
            # redesign untestable.
            ir = sim.ir_on()
            if m == "night" and not ir and scn.get("night_gain_noir"):
                curve = scn["night_gain_noir"]
            elif m == "night":
                curve = scn["night_gain"]
            else:
                curve = scn["day_gain"]
            g = interp_gain(curve, t, interp) * noise_factor(t, noise, nseed)
            g = max(g, 256.0)                # sensor floor, see gain_to_units
            r = ratio_at(m, t)
            sim.serve(g, m, r)
            # what the machine will actually decide on: the exposure index
            served = units_to_gain(gain_to_units(g))
            fed.append((t, served * (r if r is not None else 1.0), m))
            time.sleep(step)
        sim.stop()

        switches = sim.switches()
        irdrives = [p for p in sim.ir_probes() if p[1] == "off"]
        if sticky or override:
            # judge what the ISP RENDERED (the fed history), not what was
            # commanded - a stuck ISP is precisely where the two differ
            segs, start, curm = [], 0.0, scn["initial_mode"]
            for t, _, mm in fed:
                if mm != curm:
                    segs.append((start, t, curm))
                    start, curm = t, mm
            segs.append((start, dur, curm))
        else:
            segs = mode_timeline(scn["initial_mode"], switches, dur)
        rep.note("run", "%.0f virtual s at scale %d, %d board switches, "
                 "%d silent IR probes" % (dur, scale, len(switches), len(irdrives)))
        for st, sm in switches:
            rep.note("switch", "t=%.0fs -> %s" % (st, sm))

        for t, want in exp.get("mode_at", []):
            got = mode_at(segs, t)
            rep.check(got == want, "mode@%ds" % t,
                      "want %s, got %s" % (want, got))
        if "max_ir_probes" in exp:
            # Separate budget from max_switches on purpose: a silent probe
            # costs a few seconds of dimmer image, an audible switch costs a
            # motor movement. Conflating them would hide the entire point of
            # the redesign, which is trading many of the latter for a few of
            # the former.
            rep.check(len(irdrives) <= exp["max_ir_probes"], "ir-probe-budget",
                      "%d silent probes (budget %d)"
                      % (len(irdrives), exp["max_ir_probes"]))
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
            day_thr = float(cfg.get("daynight.day_gain",
                            cfg.get("daynight.total_gain_day_threshold", 768)))
            night_thr = float(cfg.get("daynight.night_gain",
                              cfg.get("daynight.total_gain_night_threshold",
                                      4096)))

            def day_at(t):
                g = interp_gain(scn["day_gain"], t, interp)
                r = ratio_at("day", t)
                return g * (r if r is not None else 1.0)

            check_restart_equiv(rep, day_at, segs, [f[0] for f in fed],
                                day_thr, night_thr, T)
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
    except SystemExit:
        rep.aborted = True
        raise
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
            # ">=", not "==": the recorder APPENDS columns as the automaton
            # grows a diagnostic (trend_fast/trend_slow, 2026-08-18). An
            # exact-width test silently turns every newer trace into "empty
            # trace" - or, in check_monotonicity, into a vacuous pass.
            if len(p) >= 10:
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
