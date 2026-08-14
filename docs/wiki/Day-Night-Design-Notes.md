# Day/Night — Design Notes

[Day/Night](Day-Night.md) documents what `src/daynight.c` *does*. This page
is about why it keeps needing fixing, and what to change about that. It is a
working document for whoever touches the file next, not user documentation.

## Why this page exists

Of the 17 `fix(daynight):` commits to date, **12 are the same behavioural
class**: the camera latches into the wrong mode and does not get out, or it
flips between modes. `53c21b4, c78dbcb, b3eec71, f8a7b21, 0f5fc80, bd21ce6,
fad4f40, a5dae07, b4a54f0, 43c2b16, 8c8dc1f, 14a1d61`, plus the 2026-08-14
cam-vorne dawn incident that produced this page.

Twelve fixes for one symptom class is a signal about the design, not about the
fixes. Each of those commits is individually well-reasoned and each closed a
real, reproduced failure. So the interesting question is not "was fix N
correct" (they were) but "why does fix N+1 keep arriving".

## 1. What the machine actually is

One thread, one 500 ms poll loop, one metric (`total_gain`, IMP `[24.8]`
linear, `256 = 1×`, **inverted**: high gain = dark). Everything else is state
carried between ticks. Current state, grouped by what it is for:

| Group | State |
| --- | --- |
| Decision | `cur`, `pending_target`/`pending_since_ms` (hysteresis candidate), `last_switch_ms` (dwell) |
| Measurement conditioning | `hist[]` (brightness ring), `settle_hist[]`/`settle_n` (AE-stability ring), `settle_floor_ms`/`settle_hard_ms`, `smooth_tg` (night-only EMA), `scraped_b`/`next_scrape_ms` |
| Night model | `night_baseline`, `baseline_logged`, `night_entered_ms` |
| Probe scheduling | `verify{mode,at_ms}`, `brighten_since_ms`, `brighten_armed`, `probe_day_ms`, `probe_verdict_at_ms`, `probe_backoff`, `probe_fail_smooth`, `last_phys_probe_ms`, `day_verify_ref`/`day_verify_ext` |
| Backstops | `osc_hist[]`/`osc_n`/`osc_freeze_until_ms`, `reassert_at_ms`/`reassert_left` |

Counting the *gates* — the independent mechanisms that can block, delay,
trigger or override a mode resolution — gives **fifteen**, not the eight one
might guess from the section headings:

1. boot/re-enable settle (floor + AE-stability extension)
2. dwell (`transition_s`)
3. pre-switch hysteresis (5 s)
4. adaptive baseline + symmetric EMA drift
5. sustained-brightening hold, with its arming *edge*
6. …and its arming *margin* (`DN_BRIGHTEN_MARGIN`)
7. failure ratchet (`probe_fail_smooth`)
8. exponential backoff on failed probes
9. passive-evidence skip gate
10. `probe_max_skip_s` outer bound on the skip gate
11. ratchet-anchor override of the skip gate (`14a1d61`)
12. dead-zone adoption + its verify deadline, both directions
13. probe three-outcome classification → ambiguous arms a verify (`8c8dc1f`)
14. post-probe AE-stability revert gate
15. oscillation-breaker freeze

(plus the post-switch re-assert series, which is actuation rather than
decision). Nine of the fifteen exist purely to decide *when to spend an
audible IR-cut click*. That ratio is the story of this file.

## 2. What the incident record actually says

The twelve incidents are not twelve unrelated bugs, and they are not one
undifferentiated mass either. They sort into five generators:

| Generator | Incidents | Status |
| --- | --- | --- |
| **A. Actuation / measurement plumbing** — the Set landed mid-ramp, the AE hadn't converged | `53c21b4`, `c78dbcb`, `b3eec71` (part) | **Closed.** No recurrence since 2026-08-02. |
| **B. A reachable state with no pending decision** | `bd21ce6` (UNKNOWN), `43c2b16` (adopted DAY), `8c8dc1f` (ambiguous probe landing) | **Closed**, and closed *checkably*: after `43c2b16`+`8c8dc1f` every reachable `(cur, verify)` pair has an armed deadline. |
| **C. A derived bar that goes unsatisfiable** | `f8a7b21`, `a5dae07`(1), 2026-08-14 | **Open.** |
| **D. Two guards validating each other** | `0f5fc80`, `14a1d61` | **Open.** |
| **E. The evidence model is missing a dimension** | `b4a54f0`, 2026-08-14 | **Open.** |

That table is the substance of my disagreement with the "these mechanisms now
interact combinatorially, producing edge cases faster than patches retire
them" reading. Two of the five generators genuinely *did* close, and B closed
because someone stated a closure criterion ("every guess must have a
deadline") rather than patching one more path. The remaining three are not
combinatorial noise — they are three nameable, recurring mistakes, each with a
statable rule that would prevent it. That is a much more tractable situation
than "combinatorial explosion", and a much worse one than "naturally
converging".

## 3. The invariant nobody wrote down

Every stuck-mode incident in the record shares one signature that nobody has
remarked on:

> **A service restart fixes it.**

`cam-wohn` 2026-08-03. Schuppen 2026-08-13 — *"only a manual service restart
(which replants the baseline from scratch) recovered it"*. cam-vorne
2026-08-14 — restarted at 08:07 and switched to day within ten seconds, off
gain **257**, the very same reading it had been calling night for four hours.

That is not a coincidence and it is not a workaround. It is the diagnosis. A
restart discards all accumulated history and re-decides from present evidence
alone, and it lands on the right answer *because* the evidence was never
ambiguous — only the history was. So the property this subsystem is supposed
to have, and has never stated, is:

> **Restart-equivalence.** If a cold start on the current evidence would decide
> mode M *unambiguously*, the running machine must converge to M within a
> bounded time T.

Two refinements make it honest rather than glib:

- **"Unambiguously"** matters: inside the dead-zone a cold start cannot decide
  either — it adopts and schedules a verification. So the invariant only binds
  where the evidence is decisive. That qualifier is already encoded in exactly
  one place in the code (the adopted-day confirm test: *"the reading would
  have decided day on its own from `DN_UNKNOWN`"`*). Generalising it is the
  cheap part.
- **T is not zero.** History legitimately matters here: it is what keeps the
  IR-cut from clunking every hour in a dark closet. So the invariant must be
  bounded-time, not instantaneous. `T = night_reconfirm_s` is the natural
  bound — one reconfirm interval, which is precisely the guarantee
  `night_reconfirm_s` was introduced (`b3eec71`) to provide, and which
  `probe_backoff` × the skip gate has been quietly eroding ever since.

Stated that way, **every one of the twelve incidents is a violation of the
same invariant**, and the reason patches haven't converged is that each one
closed a *path* to the violation rather than the violation. There are many
paths.

This is also the most valuable thing on this page, because it is *executable*:
given a gain trace, you can evaluate the cold-start verdict at every sample,
compare it to what the running machine believed, and flag any decisive
divergence lasting longer than T. That is a test oracle, and it is the thing
this subsystem has never had.

## 4. The three open generators, and the rule that closes each

### C. Bars derived from measurements, with no awareness of the measurement's range

`total_gain` has a hard floor at **256** (`1.0×` — units 0) and a practical
ceiling at the sensor's max analog+digital gain. Several bars are computed as
a *fraction of a previously measured gain*, with nothing checking the result
lands inside that range:

- `f8a7b21`: `day_gain_pct`% of a baseline sampled mid-transition → a trigger
  no real light source could reach.
- `a5dae07`(1): flooring the trigger at `total_gain_day_threshold` when the
  resting baseline is *below* that threshold → a permanent false "day".
- 2026-08-14: the failure ratchet demanded `60%` of `315` = **189**, below the
  floor of 256. Unsatisfiable, so the entire sustained-brightening path was
  silently dead — and nothing said so.

Note the shape: the ratchet is a *geometric* sequence. Each dawn probe latches
`probe_fail_smooth` at roughly the previous one's level, and each failure
demands another `day_gain_pct`% below that. On any camera whose dawn ramp
carries the night-pipeline gain toward the floor, the ratchet **will**
eventually demand a gain below the floor. That is not an edge case; it is the
terminal state of the recursion, reached on cam-vorne after four probes.

> **Rule: any bar derived from a measurement must be checked against the
> measurement's physical range, and an unsatisfiable bar must be logged.**

Cheapest useful action in the whole file: a `dn_bar_reachable()` helper plus a
`LOGW` at the point a bar is latched. On 2026-08-14 that would have printed
*"brighten ratchet 189 is below the gain floor 256 — brightening path
disabled"* at 06:20:29, four hours before anyone noticed by eye.

### D. Two guards validating each other

Both instances have an identical structure: mechanism **A** derives its
reference from a value that mechanism **B** is concurrently moving, so A and B
confirm each other instead of either being the other's escape hatch.

- `0f5fc80`: the baseline drifted toward raw gain, and the brightening bar was
  derived from the baseline. Noise ratcheted both together → overnight flap.
- `14a1d61`: the baseline drifts toward `smooth_tg`, and the skip gate's
  "solidly night" bar is derived from the baseline — so over hours the bar
  chased the (day-level) gain down and "still deep in night" stayed true.

The fix in both cases was the same idea, found twice independently: anchor the
test on a value **frozen at the moment of an actual measurement**
(`probe_fail_smooth`) rather than one that drifts.

> **Rule: an "is there evidence of change?" test must be anchored on a frozen
> measurement, never on a value that any other mechanism updates.**

Worth an audit pass: `night_baseline` currently feeds the brightening bar, the
skip gate and `dn_day_trigger()`, and drifts under all three.

### E. The evidence model is missing a dimension

Every test in the file is `metric <op> threshold` — a **level**, sampled at an
instant. But the signal that distinguishes the two scenes the dead-zone
conflates is the **derivative**:

- a static dim scene (the one the unverified-day revert exists for) sits still;
- a scene mid-dawn is in free fall *through* the dead-zone.

`b4a54f0` is the same gap seen from the other side: night-pipeline gain has no
usable level semantics at all, so the fix was to stop deciding on it. The
2026-08-14 fix (`19dcd74`) adds the file's first derivative-aware rule — revert
an unverified day only once the improvement has *stopped*.

Two more places are still level-only and demonstrably shouldn't be:

- **`probe_backoff`.** Doubling the reconfirm interval in the middle of a
  monotone descent is exactly backwards. On cam-vorne the backoff hit its ×4
  cap at 05:58 *while gain was falling through three orders of magnitude*, and
  that cap is what turned a bad revert into a four-hour outage.
- **The brightening hold**, whose ratchet asks "how far below the last failure"
  when at dawn the answer that matters is "still falling, and for how long".

> **Rule: any test that must distinguish "dim" from "getting brighter" needs a
> reference point in time, not just a threshold.**

## 5. Verdict on a redesign

**No rewrite. A targeted restructure, and a test harness first.**

The switching machinery — settle, dwell, hysteresis, re-assert, the board hook
chain — is correct and expensively earned; nothing in the incident record
since 2026-08-02 touches it. The defects are all in *probe scheduling* and
*evidence modelling*. A rewrite would relitigate twelve incidents' worth of
hard-won detail with no test net underneath it, which is a reliable recipe for
incidents 13 through 18.

What is actually missing is not a better structure. It is **an executable
oracle**. You cannot safely refactor this file without one; with one, you can
refactor it incrementally and cheaply.

### The destination: `dn_next_probe(evidence)`

Collapsing `verify`, `brighten_*`, `probe_backoff`, `probe_fail_smooth`,
`last_phys_probe_ms`, the skip gate and the anchor override into one **pure**
function over a small typed evidence struct is the right end state. The win is
not fewer lines — it is that a pure function is directly unit-testable and,
more importantly, **property**-testable:

> **Monotonicity.** For evidence `e1`, `e2` identical except that `e2` is
> strictly brighter: `dn_next_probe(e2) <= dn_next_probe(e1)`.
> Brighter evidence must never buy a *later* correction.

This single property is violated by `0f5fc80`, by `14a1d61`, and by the
2026-08-14 incident — where an *ambiguous* (dead-zone, i.e. brighter) revert
produced the same 4 h schedule as a *confirmed-night* revert. Three of the
open incidents, one assertion. That is what makes the collapse worth doing.

But it is a bad *first* step, because there is currently no way to show the
collapse is behaviour-preserving.

## 6. The replay harness

### Step 0 — there is no trace to replay

Checked on cam-vorne 2026-08-14: no gain recorder exists anywhere on the
camera, nothing but syslog lines. Every past *"verified against timpsd-sim"*
was therefore a **hand-reconstructed scenario**, not a replay of recorded
data — including the verification of the 2026-08-14 fix itself. That is the
binding constraint, and it is why the harness idea has to start with a
recorder rather than a replayer.

Sketch: a `daynight.trace_path` key appending one line per N samples —
`t_mono, cur, tg, luma, baseline, smooth_tg, day_trigger, probe_fail_smooth,
verify_mode, verify_in_s, backoff`. Roughly 30 lines of code. Write to
**tmpfs** with a size cap, not to flash (these are camera-grade eMMC/NAND and
a 5 s cadence is ~900 KB/day); persist or upload only on demand. The `/events`
SSE stream already carries most of this and a LAN-side collector is the other
viable shape.

### Step 1 — the two-pipeline problem, stated honestly

A faithful replay needs the gain for **both** pipelines at each timestamp,
because which one gets read depends on the decision the machine under test
makes — and a patch that changes the decision immediately needs the
counterfactual reading. In the field only one pipeline is observable at a
time. **True replay from field data alone is therefore impossible**, and the
harness should say so rather than pretend:

- **(a) Regression replay** on *synthetic* two-pipeline traces — fully
  faithful, the workhorse. This is what the 2026-08-14 fix was verified
  against: a fake `isp-m0` scrape file plus a `switch_cmd` stub that records
  the active mode, with the driver serving the matching pipeline's gain.
- **(b) Incident replay** on a field trace — faithful only up to the first
  point where the machine's decision diverges from the recorded one, and it
  must then **stop and report "diverged at T"** rather than continue on
  invented data. Limited, but it answers the exact question every one of these
  twelve commits tried to answer by hand: *would this patch have diverged from
  the recorded bad behaviour, and when?*

### Step 2 — time compression

A dawn incident spans hours. `ms_now_us()` is a single `static inline` in
`src/util.h` — one chokepoint — so an opt-in `MS_CLOCK_SCALE` for sim builds
is straightforward. The `DN_*` compile-time constants must scale with it,
which needs a one-line `SIM_CFLAGS` hook on the `sim` Makefile target (the
2026-08-14 verification worked around this by compressing the *config*
timings 15× and leaving the constants alone, which is adequate for a targeted
check and not for a corpus).

### Step 3 — assertions, not eyeballs

Each scenario declares expectations: mode at time T, **maximum wrong-mode
duration**, maximum board switches (the click budget is a first-class
requirement here, not a nicety), plus the two global properties from above —
restart-equivalence and probe-time monotonicity.

### Step 4 — the corpus

One scenario per historical incident. The numbers are all in the commit
messages already, which is the one genuinely good side-effect of this repo's
verbose-commit convention: `53c21b4` (boot transient 15000–20000),
`f8a7b21` (65 % of baseline), `0f5fc80` (post-revert elevated resample),
`bd21ce6` (boot at 731 in the dead-zone), `a5dae07` (resting 256–268 vs
threshold 300; baseline planted mid-descent at 10856/5148), `b4a54f0` (10856
resting, dawn dip to 820, day pipeline 8192), `43c2b16` (booted after dark on
a stale day), `14a1d61` (fail at 284, baseline chases to 257–266), and
2026-08-14 (9024 → 4813 → 2425 → 708, reverts at 1436 and 452, ratchet 189 vs
floor 256).

## 7. What not to do

- **Do not add more escalation timers.** A "dead-zone revert retry ladder" —
  shortening the reconfirm interval after an ambiguous revert and doubling it
  per consecutive one — was written and considered for the 2026-08-14 incident
  and **rejected**: it recovers *from* the wrong revert instead of preventing
  it, and it does so by re-introducing a bounded version of the pre-dawn probe
  volley that `fad4f40` was written to remove — several audible IR-cut pairs
  through every dawn that lands in the dead-zone, on every camera. The fix
  that shipped prevents the revert and costs *fewer* clicks than the status
  quo. Prefer that direction: this subsystem already has nine gates whose only
  job is rationing clicks, and adding a tenth that spends them is moving
  backwards.
- **Do not rewrite before the harness exists.**
- **Do not treat `probe_max_skip_s` as the answer to anything.** It is a 12 h
  backstop. Twice now (`14a1d61`, 2026-08-14) an incident has been
  characterised as "well within `probe_max_skip_s`", which is true and
  irrelevant — a camera in the wrong mode for four hours is already a failure.

## 8. Suggested order

1. ~~Derivative-aware unverified-day revert~~ — done, `19dcd74`.
2. **`dn_bar_reachable()` + a warn on an unsatisfiable bar.** Cheapest item
   here; makes generator C self-reporting instead of silent.
3. **Trace recorder** (tmpfs, capped). Unblocks everything below.
4. **Replay harness + scenario corpus**, with the restart-equivalence and
   monotonicity assertions.
5. **`dn_next_probe()` collapse**, validated against the corpus.
6. **Revisit `probe_backoff`'s trend-blindness** (generator E) — with the
   corpus in place this becomes a safe change rather than a brave one.

Steps 2 and 3 are each an evening's work and are worth doing before the next
incident, not after it.
