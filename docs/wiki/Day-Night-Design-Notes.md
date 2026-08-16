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
6. …and its arming *margin* (`DN_BRIGHTEN_MARGIN`), against a *frozen* rest
   reference (`brighten_ref` / `DN_HOLD_REF_LEAD`)
7. failure ratchet (`probe_fail_smooth` × `DN_RATCHET_MARGIN`)
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
undifferentiated mass either. They sort into six generators (five, until the sixth named itself on 2026-08-16):

| Generator | Incidents | Status |
| --- | --- | --- |
| **A. Actuation / measurement plumbing** — the Set landed mid-ramp, the AE hadn't converged | `53c21b4`, `c78dbcb`, `b3eec71` (part) | **Closed.** No recurrence since 2026-08-02. |
| **B. A reachable state with no pending decision** | `bd21ce6` (UNKNOWN), `43c2b16` (adopted DAY), `8c8dc1f` (ambiguous probe landing) | **Closed**, and closed *checkably*: after `43c2b16`+`8c8dc1f` every reachable `(cur, verify)` pair has an armed deadline. |
| **C. A derived bar that goes unsatisfiable** | `f8a7b21`, `a5dae07`(1), 2026-08-14, kinder-links 2026-08-16 | **Open.** Self-reporting since `5423b79`, but kinder-links shows the guard only covers the *sensor's* range: a bar can clear the 256 floor and still sit under everything the scene ever reads. |
| **D. Two guards validating each other** | `0f5fc80`, `14a1d61` | **Open.** |
| **E. The evidence model is missing a dimension** | `b4a54f0`, 2026-08-14, kinder-links 2026-08-16 | **Half closed.** The unverified-day revert (`19dcd74`) and `probe_backoff` are trend-aware; the latter only genuinely so since 2026-08-16, when the suspension stopped being a snapshot predicate. The brightening hold's ratchet is still level-only. |
| **F. A property stated over one snapshot** | kinder-links 2026-08-16 | **Open**, and newly named. The defect is in the oracle, not the machine: purity bought counterfactual reach, not temporal claims. See §4F. |

That table is the substance of my disagreement with the "these mechanisms now
interact combinatorially, producing edge cases faster than patches retire
them" reading. Two of the six generators genuinely *did* close, and B closed
because someone stated a closure criterion ("every guess must have a
deadline") rather than patching one more path. The remaining four are not
combinatorial noise — they are four nameable, recurring mistakes, each with a
statable rule that would prevent it. That is a much more tractable situation
than "combinatorial explosion", and a much worse one than "naturally
converging".

One honest correction to the optimism above, from 2026-08-16: F was found not
by another incident of an old kind but by *the fix for E producing a new
instance of E*, and it took a live camera and a person flipping a light switch
to notice. The generators are tractable; the oracle for them is still behind
the code.

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
disabled"* at 06:20:29, four hours before anyone noticed by eye. Done in
`5423b79`.

**Amendment, kinder-links 2026-08-16 — the rule as written is too weak.** A
bar can clear the sensor's floor by a wide margin and still be unreachable,
because the range that matters is not the sensor's, it is *the scene's*. On
that camera the ratchet latched at the room's ordinary resting night level
(1653), demanded 60 % of it (992 — analog gain 62), and the room lives between
analog 85 and 100 all night. 992 is nowhere near the 256 floor, so
`dn_bar_reachable()` had nothing to say; the sustained-brightening path was
nevertheless dead for the whole night, and turning the room light on moved
nothing at all. The diagnosis is not that the guard failed — it is that the
bar was derived with the wrong constant, and that a guard keyed to the
hardware could never have caught it:

> **Rule (extended): a bar derived from a measurement must be checked against
> the range the SCENE can reach, not only the range the sensor can represent.
> If the fraction that defines the bar was borrowed from a test answering a
> different question, that is the bug — fix the derivation, not the guard.**

Which is exactly what `DN_RATCHET_MARGIN` does: the ratchet stopped borrowing
`day_gain_pct` (a *pipeline* discriminator, 0.74 stops, correct for the IR-cut
transitions it was written for) and got its own constant sized for the
question it actually asks — "is this new evidence, distinguishable from the
evidence that already failed?" — at one quarter stop. Incidentally this also
pushes the geometric recursion above further from the floor: the same
2026-08-14 latch now derives 265 rather than 189.

### D. Two guards validating each other

Both instances have an identical structure: mechanism **A** derives its
reference from a value that mechanism **B** is concurrently moving, so A and B
confirm each other instead of either being the other's escape hatch.

- `0f5fc80`: the baseline drifted toward raw gain, and the brightening bar was
  derived from the baseline. Noise ratcheted both together → overnight flap.
- `14a1d61`: the baseline drifts toward `smooth_tg`, and the skip gate's
  "solidly night" bar is derived from the baseline — so over hours the bar
  chased the (day-level) gain down and "still deep in night" stayed true.
- kinder-links 2026-08-16: the same chase in the **third** consumer, the
  sustained-brightening hold, and the fastest-acting instance of it by a wide
  margin. The hold's bar is `(100+pct)/200` of the baseline, so it sits 22.4 %
  away; `DN_BASELINE_ALPHA` closes 22.4 % in about **25 s**, against a
  `DN_BRIGHTEN_CONFIRM_MS` of **30 s**. The debounce loses that race by
  construction. Traced tick by tick on the corpus 10 replay: a 23 % light-on
  left the margin test 2.2 % short at the instant the step completed and
  falling from there, so it never opened on any tick — and 25 s later the bar
  had dropped *through* `smooth_tg`, taking the `>= bar` branch, which re-arms
  and clears the hold. A permanently brighter room therefore reads as "nothing
  has changed" from then on. Not merely slow: **erased**.

The fix in all three cases was the same idea, found three times independently:
anchor the test on a value **frozen at the moment of an actual measurement**
(`probe_fail_smooth`, or for the hold a snapshot of the baseline taken while
the scene was still at rest) rather than one that drifts.

> **Rule: an "is there evidence of change?" test must be anchored on a frozen
> measurement, never on a value that any other mechanism updates.**
>
> **Corollary (2026-08-16): where a bar is derived from a drifting value, the
> drift rate and the confirm window are not independent knobs. If the drift
> can close the bar's own gap faster than the confirm can elapse, the test is
> not "strict", it is unreachable — the same failure mode as an unsatisfiable
> bar (generator C), arriving through time rather than through range. State
> the ratio when either constant is touched.**

The audit that item asked for is now two-thirds done: `night_baseline` fed the
brightening bar, the skip gate and `dn_day_trigger()` and drifted under all
three. The skip gate got its frozen anchor in `14a1d61`, the hold got
`brighten_ref` on 2026-08-16, and `dn_day_trigger()` is informational in the
adaptive regime (night→day is probe-mediated since `b4a54f0`), so it is the
one consumer where the drift is harmless. Note also **where** the hold's
snapshot has to be taken: freezing it when the scene crosses the *bar* is far
too late, because the bar is 22.4 % away and the baseline has been chasing for
tens of seconds by then — measured, that late freeze captured 2353 where the
true pre-event rest level was 2397, and 1.8 % of contamination was itself
enough to keep the margin shut. It freezes at the 3 % `DN_BRIGHTEN_MARGIN`
band instead: the reference stops tracking the moment the scene is *measurably*
brighter, not once it is *decisively* brighter.

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

- **`probe_backoff`.** ~~Doubling the reconfirm interval in the middle of a
  monotone descent is exactly backwards.~~ **Closed** by the trend suspension
  in `dn_next_probe()` (`dn_trend_falling()`): while the gain sits below
  `DN_BRIGHTEN_MARGIN` of the frozen `probe_fail_smooth`, the multiplier is
  suspended and the deadline falls back to one plain `night_reconfirm_s`
  after arming. Note the reference point it needed was already in the file and
  already frozen — generator D's rule supplied it. On cam-vorne the backoff
  hit its ×4 cap at 05:58 *while gain was falling through three orders of
  magnitude*, and that cap is what turned a bad revert into a four-hour
  outage; corpus scenario 08 now measures the difference (recovery at 1276 s
  instead of 3976 s) and fails without it.

  **Closed properly on 2026-08-16, not before.** As first written the
  suspension read the *instantaneous* `smooth_tg`, which means it was still a
  level test sampled at an instant — generator E's own defect, reintroduced
  inside generator E's own fix. It therefore un-fired as readily as it fired.
  On kinder-links it suspended an ×4 backoff at 23:21:08 (gain 1599 against a
  1653 anchor), pulling the reconfirm from 03:12 in to 00:12; the room then
  dimmed on its own, the predicate went false, and the four hours came
  straight back — long before the deadline it had pulled in. The evidence had
  been *measured* and then discarded. It now reads `min_smooth_since_probe`,
  the lowest smoothed gain since the anchor was frozen, so "the scene has been
  measurably brighter than confirmed night" is a fact about the interval
  rather than about this tick. The skip gate's ratchet-anchor override reads
  the same value for the same reason: a deadline is a single instant, so
  testing the instantaneous gain there throws away every brightening that
  happened between deadlines.
- **The brightening hold**, whose ratchet asks "how far below the last failure"
  when at dawn the answer that matters is "still falling, and for how long".
  `DN_RATCHET_MARGIN` (2026-08-16) fixed the *scale* of that question, not its
  level-only-ness; the hold is still the one place in the file that asks it
  purely as a threshold.

> **Rule: any test that must distinguish "dim" from "getting brighter" needs a
> reference point in time, not just a threshold.**

#### F. A property stated over one snapshot cannot see a defect that lives between snapshots

Worth naming separately, because it is a defect in the *oracle*, not in the
machine, and it is the reason the 2026-08-16 bug survived a week of work aimed
squarely at it. `dn_next_probe()` is pure over one `dn_evidence`, and both
properties `tests/dn-probe-props.c` asserted were statements about a single
evaluation: monotonicity compares two evidence values, the reconfirm bound
reads one. The trend suspension satisfied both, on every one of ~10⁶ points,
while still handing four hours back the moment a room dimmed again — because
across a *sequence* of evaluations, brighter evidence had no durable effect.
Purity bought counterfactual reachability, which is real and was the point;
it did not by itself buy temporal claims.

The corpus caught nothing either, and for a related reason: the nine scenarios
all replay *monotone* transitions (dawn ramps, boots, a single dip). None of
them contained a brightening that came and then went, which is precisely the
trajectory that distinguishes a latched predicate from a sampled one. Scenario
10 exists to be that trajectory.

> **Rule: for every piece of state the schedule carries between ticks, state a
> property over the SEQUENCE, not only over the snapshot — at minimum "evidence
> once measured is never worth less later". And when adding a scenario, ask
> what its gain curve does *non*-monotonically; a corpus of monotone traces
> cannot falsify a memory bug.**

`assert_dip_monotone()` is that property for `min_smooth_since_probe`: for two
evidence values with identical current readings, the one that records a
brighter past minimum must never be granted a later probe.

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
2. ~~**`dn_bar_reachable()` + a warn on an unsatisfiable bar.**~~ — done,
   `5423b79`. Generator C is self-reporting instead of silent.
3. ~~**Trace recorder** (tmpfs, capped).~~ — done, `c84fcef`
   (`daynight.trace_path`, opt-in).
4. ~~**Replay harness + scenario corpus**, with the restart-equivalence and
   monotonicity assertions.~~ — done, `e06bf41` (virtual clock) + `e0c629b`
   (`scripts/dn-replay.py`, nine scenarios).
5. ~~**`dn_next_probe()` collapse**, validated against the corpus.~~ — done,
   `src/daynight_probe.h` + `tests/dn-probe-props.c`.
6. ~~**Revisit `probe_backoff`'s trend-blindness** (generator E).~~ — done,
   `dn_trend_falling()`; see the generator-E section above.
7. ~~**Latch the trend suspension on a measurement, not a predicate**
   (generators E + F).~~ — done, 2026-08-16: `min_smooth_since_probe` in
   `dn_evidence`, read by both `dn_trend_falling()` and the skip gate's
   ratchet-anchor override, plus `assert_dip_monotone()` as the sequence
   property that would have caught it. Corpus scenario 10.
8. ~~**Give the failure ratchet its own margin** (generator C, extended).~~ —
   done, 2026-08-16: `DN_RATCHET_MARGIN` (one quarter stop) instead of
   borrowing `day_gain_pct`. Pinned by the `kinder-links B1` cases in the
   property test — both that a 21 % brightening can now start a hold, and that
   an identical re-dip still cannot.
9. ~~**Stop the brightening hold's bar being derived from the drifting
   baseline** (generator D, third consumer).~~ — done, 2026-08-16:
   `brighten_ref`, released again by `DN_HOLD_REF_LEAD`. This is the one that
   makes the fast path actually fire for the everyday event — corpus 10 goes
   from 489 s to 35 s — and the one that most needed its second-order effect
   caught, since the first version of it walked `fad4f40`'s probe volley down
   corpus 09's dawn ramp.

### What the corpus is worth, and how to keep it worth that

The corpus is only evidence if a scenario **fails against a build without the
fix it is named for**. Four of the twelve assert a *decision* rule rather than
a new log line, and all four were checked that way (08 against the anchor
override and against the trend suspension, 09 against `19dcd74`'s extension
*and* — newly — as the negative control for `DN_HOLD_REF_LEAD`, 10 against
both the latched suspension and the frozen hold reference, 11 as the
sensitivity floor via `forbid_log`) — each fails on behavioural assertions,
not merely on `expect_log`.

Scenario 09 earned a second job on 2026-08-16 and it is worth naming, because
it is the only reason a fix that looked finished was not shipped broken: the
first version of the frozen hold reference passed 10 and 11 and the whole
property test, and 09 failed it instantly on the **click budget** — 14 board
switches against 9, six sustained-brightening pairs walking down one dawn
ramp. The mode assertions all still passed. A corpus that only checked "did it
end up in the right mode" would have shipped it. `max_switches` is not a
nicety. Scenario 10 is the sharpest of the three: pre-fix it
never leaves night at all inside the run (wrong-mode 3100 s, `mode@3500` and
`mode@5900` both wrong), and `probe_max_skip_s` is deliberately left at its
43200 default so the backstop cannot quietly rescue it.

Scenario 10 also carries the generator-F lesson in its shape. Its gain curve
is **non-monotone on purpose** — a brightening that comes and then goes,
before the deadline it pulled in. Every one of the first nine replays a
monotone transition, which is why none of them could falsify a schedule that
forgets. When adding a scenario, ask what its curve does non-monotonically
before asking what it asserts.

Build a negative control by **reverting the fix's hunk in a copy of the
current tree**, never by building the historical commit. Trees older than
`e06bf41` have no `MS_CLOCK_SCALE` hook: they silently ignore `SIM_CFLAGS`,
run in real time while the harness drives them 60× faster, reach none of their
own deadlines, and come back green on everything — a false negative that reads
exactly like "this scenario does not discriminate". `dn-replay.py` now refuses
such a binary outright (`SimRun.check_clock`), but the cheaper habit is not to
build one.

Next candidates, in the same spirit: the brightening hold's ratchet is the
last level-only test generator E names — `DN_RATCHET_MARGIN` corrected its
scale but it is still a threshold, not a trend — and `night_baseline` still
feeds three consumers while drifting under all three (the generator-D audit).
kinder-links put a sharper edge on that last one: on the night in question the
hold was blocked not by the ratchet but by the *baseline-relative* arming
margin, because the baseline had already drifted down to chase the very
brightening the hold was meant to notice. That is `14a1d61` again, in the one
consumer the anchor override does not cover.

Also worth a pass now that generator F is named: audit the remaining
between-tick state in `dn_thread` the same way. `brighten_since_ms`,
`day_verify_ref`, `brighten_ref` and `probe_backoff` itself are each carried
across ticks and each has only snapshot properties asserted about it.

And one specific piece of unfinished business from the third fix: the hold now
distinguishes a step from a ramp by how far its frozen reference has led the
baseline, which is a *proxy* for the derivative. The direct test — "is the
gain still falling?" — is the open generator-E item two bullets up, and it
would replace `DN_HOLD_REF_LEAD` with the thing that constant is standing in
for. Do that when the hold is next opened, not before: the proxy is measured,
bounded on both sides and has a negative control, which is more than the
subsystem's last four constants started with.
