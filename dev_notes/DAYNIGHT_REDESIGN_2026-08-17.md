# Day/Night - Redesign Draft "Three Independent Paths"

**Date:** 2026-08-17 · **Revision 2** (following feedback: drop the brightness
fallback, boot click ok, **many cameras have no geodata - and geodata does not
help in dark/artificially lit rooms anyway**; question about learning,
boot-persistent values)
**Code state:** `c0192d1`, `src/daynight.c` = 2030 lines, `src/daynight_probe.h` = 732 lines
**Status:** **implemented** on 2026-08-17. The draft stands unchanged; what
changed during implementation relative to Revision 2 is covered in §12.
**Predecessor:** `dev_notes/DAYNIGHT_ALTERNATIVES_2026-08-07.md`,
`docs/wiki/Day-Night-Design-Notes.md`.

---

## 0. Why a redesign is permissible now

The design notes ruled *"No rewrite ... and a test harness first"* - **under
the condition** that no executable oracle exists. That condition is now met:
recorder (`c84fcef`), replay + virtual clock (`e06bf41`, `e0c629b`),
14 scenarios, property tests. Crucially, the scenarios assert **behavior,
not internals** - `expected_mode`, `max_wrong_mode_s`, `max_switches`,
`restart_equivalence_s`, `monotonicity`. These five assertions survive a
redesign unchanged; only `expect_log` and the `max_switches` baseline need
to be recast.

---

## 1. Diagnosis in one sentence

The current state has **one** path from night to day (the probe) and **nine
rules that ration exactly that path** - backoff, failure ratchet,
anchor override, passive-evidence skip, `probe_max_skip_s`, trend suspension,
brightening hold, oscillation freeze, verify deadlines. Each is individually
correct and was paid for by a real incident. But because they **all gate the
same thing**, their misbehavior multiplies instead of canceling out. That is
the 2026-08-14 camera: four hours of IR video in daylight, while a restart
decided correctly within ten seconds.

> **Design rule:** every economizing rule must be covered by a trigger that
> it does not itself influence. Rationing is allowed - but never on the
> single path. In Revision 2 this rule is **checkable at runtime** (§4.5),
> not just a design intention.

---

## 2. The physical facts

1. **The day pipeline does not lie.** IR-cut closed, LEDs off, color on: an
   honest measure of ambient light, in both directions.
2. **The night pipeline lies systematically.** IR-cut open + its own
   illuminator: the absolute level means nothing. A **change** does mean
   something.
3. **Gain alone is blind at the bright end** - and that is exactly where the
   most expensive incidents live. See §3; this is the most important change
   in Revision 2.
4. **Twilight is computable** - but only for cameras that can see the sky.
   For the rest of the fleet the calendar is worthless (your statement, and
   it is correct). The calendar is therefore an *accelerator*, never a
   carrier.

---

## 3. The measurement: exposure index instead of gain

**This is the substantive correction to Revision 1.** If many cameras have
no geodata, and the calendar has nothing to say indoors anyway, the *sensory*
path has to carry the fleet. The current state cannot do that - not because
of the logic, but because of the measurement:

`total_gain` has a hard floor at **256** (1.0x). Once the AE hits that
floor, any further brightening of the scene is **invisible**. Those are
exactly the two most stubborn cases in the incident register: cam-J sits at
256-268 under its own IR, cam-H chases its baseline up to 257-266. No
gain-based trigger can ever work there, no matter how cleverly it is
designed - and half of the rationing apparatus exists only to paper over
that blindness.

But the AE controls two quantities: gain **and** integration time. Once
gain hits the floor, it keeps shortening the exposure. Both fields are
already present in the `/proc/jz/isp/isp-m0` dump and are already parsed
today (`daynight.c:150-153`). So:

```
D = total_gain × (integration_time / max_integration_time)      /* higher = darker */
```

- **Dark scene:** `integration_time == max` → `D == total_gain`. **Identical
  to today.** All thresholds, all 14 corpus scenarios keep their meaning.
- **Bright scene, gain at the floor:** integration time drops → `D` falls
  well below 256, smoothly and with plenty of resolution.
- **No `max_integration_time` readable** (0/missing) → `D = total_gain`, a
  clean fallback to today's behavior.
- **Fps-independent**, because it is normalized to `max`.

What this fixes directly, without a single scheduling rule:

| before | with exposure index |
|---|---|
| `DN_GAIN_FLOOR`, `dn_bar_reachable()`, `dn_hold_gate()`, "bar below the floor, path dead" | gone - the floor now sits orders of magnitude lower |
| cam-J / cam-H: brightening structurally invisible | visible, path C works |
| "day can never be confirmed", `day_gain` 300 vs. scene 260 (three cameras 2026-08-16) | a lit room now reads e.g. `D ≈ 20` instead of 260 - a huge margin |

Cost: the `/proc` scrape has to move back into the decision tick (today
throttled to `DN_SCRAPE_MS`=5 s because gain came from the IMP API). The
scrape costs one `fopen` + ~7 `strstr`/`sscanf` calls per line. Countermeasure:
the decision does not need 500 ms - **`interval_ms` for the scrape at 2 s**,
which is a quarter of the current state's load before the P2 optimization,
and still 7x faster than any confirmation duration in the automaton.

---

## 4. The automaton

### 4.1 Four paths that do not block one another

| # | path | signal | covers | cost |
|---|------|--------|----------|--------|
| **A** | day→night direct | day-pipeline `D` (honest) | twilight, light off | 1 click |
| **C** | spontaneous jump | *relative* change of `D` at night | **light on**, interior rooms, cameras without geodata - **the fleet's carrier path** | 2 clicks on failure |
| **B** | heartbeat | clock only, sensor-blind | anchored reference too low, scene unreadable | 2 clicks |
| **D** | boot probe | one-off measurement at t=0 | every persisted mode is a guess | 1 click per boot |

Swapped relative to Revision 1: **C carries, B is the safety net.** The
calendar (sunrise/sunset) is now only an optional modifier of B (§4.4).

### 4.2 State - complete

```c
int     mode;         /* DAY | NIGHT - the only authoritative bit            */
int64_t mode_since;   /* transition_s dwell                                  */
float   s;            /* EMA of D; -1 = needs seeding                        */
int     stable_n;     /* consecutive samples with |D-s| < 10% - AE settled   */
float   ref;          /* PROVEN night level; -1 = none yet                   */
int64_t ref_due;      /* when ref is allowed to be anchored                  */
int64_t trig_since;   /* since when s < ref*jump (0 = not triggered)         */
int64_t dark_since;   /* since when s > night_gain, DAY only                 */
int64_t last_probe;   /* click budget                                        */
int64_t verdict_at;   /* running probe: verdict time (0 = none)              */
float   pre_probe;    /* night level immediately before the running probe    */
int64_t hb_at;        /* next heartbeat                                      */
float   sust_min;     /* held minimum of s since the last probe              */
float   win_max;      /* ... its staggered window (see §12.2)                */
int64_t win_at;
```

15 scalars, **no ring buffers**. `ref` is not filter state but a **proof**:
"this is how bright it was the last time it was *proven* to be night." The
current state carries five approximations of the same quantity side by side
(`night_baseline` drifting, `smooth_tg`, `probe_fail_smooth`,
`min_smooth_since_probe`, `brighten_ref`). They collapse into one, because a
failed probe is simultaneously a *night proof* and a concurrent night-pipeline
measurement.

### 4.3 Rules - complete

```
tick (every interval_ms = 2000):

  D = read_exposure()                    # §3; <0 = skip tick
  s = (s < 0) ? D : s + (D - s) * ALPHA
  stable_n = |D - s| < 0.1*s ? stable_n+1 : 0
  flat_lo = min(flat_lo, s); flat_hi = max(flat_hi, s)

  # -- (1) evaluate the running probe: EXACTLY ONE verdict, binary --------
  if verdict_at && now >= verdict_at:
      verdict_at = 0
      if s < day_gain_eff:               # day confirmed (§5: eff = learned)
          learn_day(s)
      else:                              # probe found night = PROOF
          switch(NIGHT)                  # bypasses transition_s: retreat is free
          ref = pre_probe                # ratchet: exactly here, nowhere else
      continue

  # -- (2) DAY: honest measurement, no history, no probe ------------------
  if mode == DAY:
      learn_day(s)                       # running minimum of the day excursion
      dark_since = (s > night_gain) ? (dark_since ?: now) : 0
      if dark_since && now-dark_since >= day_confirm_s && dwell_ok():
          switch(NIGHT); ref = -1; ref_due = now + ref_delay_s

  # -- (3) NIGHT ------------------------------------------------------------
  else:
      # (3a) anchor the reference once the AE has settled after IR turn-on
      if ref < 0 && now >= ref_due && (stable_n >= STABLE_N || now >= ref_due + ref_max_s):
          ref = s

      # (3b) path C - spontaneous jump, edge-triggered
      jump = (ref > 0 && s < ref * jump_ratio)
      trig_since = jump ? (trig_since ?: now) : 0
      want = trig_since && now - trig_since >= probe_confirm_s

      # (3c) path B - heartbeat
      if now >= hb_at: want = 1

      # (3d) click budget: the ONLY rationing rule
      if want && now - last_probe >= probe_min_gap_s && dwell_ok():
          pre_probe = s
          switch(DAY); last_probe = now
          verdict_at = now + probe_settle_s
          trig_since = 0
          flat_lo = flat_hi = s
          hb_at = next_heartbeat(now)
```

`switch()` additionally sets `s = -1; stable_n = 0; mode_since = now` and
arms the existing re-assert net (`DN_REASSERT_*`, unchanged).

### 4.4 `next_heartbeat()` - and where the calendar fits in

```
flat = (flat_hi / flat_lo) < 1.15        # scene unchanged since the last probe
iv   = flat ? heartbeat_max_s : heartbeat_s          # 12h : 4h
if !c_sighted():  iv = min(iv, heartbeat_s)          # §4.5 - saving forbidden
if calendar available and says NIGHT and c_sighted():
      iv = min(iv, time until sunrise + sunrise_offset_min)
return now + iv
```

Three properties:

- **You lose nothing structural without geodata.** The heartbeat is the
  baseline load; the calendar can only make it *better* (an appointment
  exactly at sunrise instead of "sometime in the next 4h"). The carrier path
  is C.
- **The flatness rule replaces the entire skip-gate apparatus.** "The scene
  has moved by less than 15% since the last probe" is an honest statement
  about evidence - two floats, no derived bar, no `probe_max_skip_s` outer
  bound. The dark, windowless interior: 2 click pairs per day.
- **No multiplicative backoff.** The ceiling for "wrong mode" is
  `heartbeat_max_s` = 12h, constant and unconditional. The current state had
  `night_reconfirm_s × 4` + skip, and therefore effectively none.

### 4.5 `c_sighted()` - the design rule as a runtime predicate

Economizing rules may only kick in when the *independent* path C can
actually see:

```
c_sighted() = (integration-time fields readable)        # D still has downward resolution
           || (ref > DN_GAIN_FLOOR * 1.5)               # gain still has headroom
```

If C is blind, the heartbeat falls back to the fast tick and the calendar
is not allowed to suppress anything. That makes "every economizing rule is
covered" no longer an intention in the author's head, but a condition in the
code - and checkable in the harness. That check is exactly what the current
state's nine rules were missing.

### 4.6 Boot

The persisted `image.running_mode` is a **guess**. Instead of verifying
it, it gets measured:

- persisted **day** → we are already on the honest pipeline: render the
  verdict after `boot_settle_s` (+ `stable_n`). **Zero clicks.**
- persisted **night** → a one-off boot probe, without a heartbeat or budget
  check. **One click per boot** (confirmed by you).

That makes the design notes' *restart equivalence* hold **literally** at
t=0. This drops, with nothing to replace it: dead-zone adoption, `dn_verify`
on both sides, `day_verify_ref`/`_ext`, `DN_DAY_VERIFY_FALL/EXT_MAX`, the
three-outcome classification, and `probe_verdict_at_ms` - roughly 400 lines
for about 15.

---

## 5. Learning and persistence

> *"Could the mechanism learn values? And remember them across a boot,
> too?"*

Yes - but this is exactly where every single incident originated:
accumulated history overriding the present. Hence two hard rules before
anything gets learned:

> **L1 - Direction rule.** A learned value may only shift a threshold in
> the direction whose failure self-corrects. A `day_gain` that is too
> generous produces a false day → path A corrects it within `day_confirm_s`.
> A `day_gain` that is too strict makes day *unconfirmable* → that is the
> unbounded class. So: **only learn upward, never downward.**
>
> **L2 - Ceiling rule.** No persisted value may delay a correction beyond
> `heartbeat_max_s`. Persistence is allowed to make things faster or quieter,
> never slower.

### 5.1 What gets learned

**`day_ref` - how bright does it even get here?**
Per day excursion, the minimum of `D` (this is today's `day_best_tg`, just
persistent). Eight slots as a ring, and the **median** is used - robust
against a single outlier (a flashlight into the lens) without any decay
math.

```
day_gain_eff = clamp( max(day_gain, median(day_ref) * DAY_LEARN_M),   /* L1: only upward */
                      day_gain,  night_gain / 2 )                     /* collision forbidden */
```

This is the actual win: it handles the *"day can never be confirmed"*
class (three cameras on 2026-08-16) **automatically**, instead of diagnosing
it and waiting for a human with SSH access. If `day_gain_eff` runs up against
the `night_gain/2` clamp, the existing `diagnose_thresholds` warning still
fires - at that point the configuration genuinely *is* wrong.

**~~`quiet_tier`~~ - dropped during implementation.** The idea was to
carry the slow heartbeat cadence across a boot. It buys nothing: with
`boot_probe=1` the camera measures immediately on every boot anyway and
replans the heartbeat from the result. And with `boot_probe=0` a persisted
"slow" state would actually be the *less safe* variant - starting at the
fast cadence after a boot is exactly right. Persistent state that buys
nothing is exactly the kind of surface area this rework is meant to remove.

### 5.2 What is explicitly not persisted

- **`ref`** - the night proof. It re-anchors within 30 s, but after a
  boot it could be arbitrarily stale, and a `ref` that is too low
  *disables* path C. Violates L2. Savings: 30 s. Risk: the `a5dae07` class -
  back in through the back door. **No.**
- **`mode`** - is not *believed*, it is measured on every boot (§4.6).
- **Backoff/ratchet state** - no longer exists.

### 5.3 How it gets written

A small text file, `daynight.state_path` (empty = learning off, plain
config behavior):

```
v1
day_ref 812 790 845 1103 798 802 830 795
quiet 1
```

- Written **only on change** and **at most once per hour**, atomically via
  `rename()`. Order of magnitude: a handful of writes per day, ~60 bytes.
  That is uncritical for camera NAND - unlike the trace, which is why the
  trace belongs on tmpfs.
- On read: version line does not match / unparsable / values outside
  plausible bounds → **ignore silently** and start as if there were no file.
  A corrupt learning file must never disable a camera.
- Checkable in the harness: a new scenario switch "with a pre-seeded state
  file", plus the assertion that `restart_equivalence_s` **with** a state
  file is no worse than without one. That is the direct test for L2.

### 5.4 Ordering - decision

The draft recommended shipping stage 1 without learning (debugging two
new things at once is the pattern that produced twelve incidents).
**Instead it was decided to: ship it, but make it switchable via
`daynight.learn` and OFF by default** - which gives the same protection,
because the shipped state is identical to "without learning", while still
making the numbers visible immediately.

Concretely: collection and **logging once a day always happens**;
application and persistence only with `learn=1`. That lets you read off a
running camera what learning *would* do before turning it on:

```
DAYNIGHT: learned: 6 day excursion(s) [812 790 845 1103 798 802], median 807,
          effective day_gain 768 vs configured 768 - not applied (daynight.learn=0)
```

---

## 6. The real incidents, played through

| Incident | Current behavior | New draft |
|---|---|---|
| `53c21b4` boot transient 15000-20000 | first measurement switches to night | verdict only at `stable_n` - the transient never reaches a verdict |
| `bd21ce6` boot 09:23, gain 731 in the dead zone | adopts stale night, gets stuck, no log | persisted night → boot probe → honest measurement → day. **1 click** |
| `43c2b16` booted into stale day after dark | black image until the verify deadline | persisted day → verdict from an honest measurement → night. **1 click** |
| `a5dae07` baseline mid-descent at 10856, resting level 800 | trigger 6514 above the resting level → flip pair every ~25 min, forever | `ref`=10856 too high → jump fires **once** → probe finds night → `ref := 800` (proof) → permanently correct. One self-healing click pair instead of an infinite loop |
| `b4a54f0` twilight dip 10856→820, day pipeline 8192 | direct trigger overtakes the hold, 5 click pairs in one morning | night→day is **exclusively** probe-mediated; `probe_min_gap_s` caps it at ≤1 pair/10 min; after the failure `ref` ratchets to 820 → the same dip does not fire again |
| `0f5fc80` noisy night, post-revert resample | flap loop all night | `ref` is **only** ever set by proof, never drifts |
| `14a1d61` baseline chases 257-266 under the floor | brightening path dead, backoff ×4 | **exposure index (§3):** there is now resolution below 256 - the path is never dead in the first place |
| `a5dae07`/cam-J resting level 256-268 under its own IR | brightening structurally invisible, inverted regime as a special case | ditto - the "inverted regime" special case drops with nothing to replace it |
| 2026-08-14 ramp 9024→708, 4h wrong mode | ratchet 189 under the floor + backoff ×4 | heartbeat fires independent of `ref` and any measurement; ceiling 12h unconditional, ≈ sunrise with the calendar |
| 2026-08-16, 3 cameras: `day_gain` unreachable | every probe reverts, night forever, diagnosis only via SSH | §3 enlarges the margin by orders of magnitude; §5 automatically pulls `day_gain_eff` up as needed |
| IR reflection (~30 cm to the object) | genuine threshold flips → oscillation breaker needed | night→day **cannot** be triggered by a threshold. **The oscillation breaker is dropped with nothing to replace it** |
| **turning on a light, dark room** | 30 s hold + ratchet bar + backoff gate - often not at all | `D` drops drastically → `probe_confirm_s` (15 s) → probe → verdict (8 s). **Color after ~25 s**, no special case |

Headlights/a passing car: 2-5 s does not reach `probe_confirm_s`; a
repeated, identical event does not fire again after the first failure,
because `ref` then sits at the trigger level.

---

## 7. What goes away, what stays

### 7.1 Dropped with nothing to replace it

Backoff (`probe_backoff`, `DN_PROBE_BACKOFF_MAX/CAP_S`) · failure ratchet
(`probe_fail_smooth`, `DN_RATCHET_MARGIN`) · anchor override
(`min_smooth_since_probe`, `min_win_*`) · trend suspension (`dn_trend_falling`) ·
passive-evidence skip + `probe_max_skip_s` + `last_phys_probe_ms` ·
brightening hold (`brighten_*`, `DN_BRIGHTEN_*`, `DN_HOLD_REF_LEAD`) · adaptive
baseline (`day_gain_pct`, drift, `dn_day_trigger`) · `DN_GAIN_FLOOR`,
`dn_bar_reachable()`, `dn_hold_gate()` (moot due to §3) · dead-zone adoption ·
`dn_verify` on both sides · the "still-brightening" extension · oscillation
breaker (`DN_OSC_*`) · **brightness fallback as a decision path** (confirmed;
`dn_brightness()` stays only for the web UI display) · the entire
`dn_probe_plan`/`dn_evidence` layer (`src/daynight_probe.h`, 732 lines) - the
plan is now three `if`s.

### 7.2 Carried over unchanged

`dn_switch()` with `fork()`+`execlp()` (F-01) · re-assert net including
the divergence warning · `transition_s` dwell · `dn_sun_times()` (now a
modifier of the heartbeat) · `dn_time_target()` · stopgate/thread lifecycle ·
`dn_status_update()` + `/events` dedup · trace recorder (columns simplified,
`D` and `int/max` added) · `diagnose_thresholds` · `dn_brightness()` for the
status display.

### 7.3 Config migration

**Stays:** `enabled`, `interval_ms` (default 500 → **2000**), `transition_s`,
`switch_cmd`, `isp_path`, `trace_path`, `boot_settle_s`, `sun_*`, `time_*`,
`diagnose_thresholds`.

**Renamed, semantics now unambiguous** (both absolute, both apply only on
the honest pipeline, both in `D` units, which are identical to today's gain
units in the dark): `total_gain_day_threshold` → `day_gain`,
`total_gain_night_threshold` → `night_gain`. The gap between them is pure
hysteresis and **not a dead zone** - there is no longer an undecided
outcome.

**New (8):** `probe_min_gap_s` (600) · `probe_jump_pct` (50) ·
`probe_confirm_s` (15) · `probe_settle_s` (8) · `day_confirm_s` (30) ·
`heartbeat_s` (14400) · `heartbeat_max_s` (43200) · `state_path` (empty = no
learning). Optional `boot_probe` (`auto|always|never`).

**Removed (9):** `day_gain_pct`, `baseline_delay_s`, `boot_settle_max_s`,
`boot_stable_pct`, `night_reconfirm_s`, `probe_max_skip_s`, `threshold_low`,
`threshold_high`, `hysteresis` → parse with a migration warning.

**`daynight.mode`:** `auto` (default, A+B+C+D) · `schedule` (calendar
only, zero probes, zero sensor - replaces today's `time`/`sun`).
A separate `sensor` mode is no longer needed: without geodata, `auto` *is*
the sensor mode.

---

## 8. Size and memory - measured, before and after

**On the target architecture** (`mipsel-linux-gcc -Os`, the same flags as
`make target`), both states measured identically:

| | before | after |
|---|---|---|
| `.text` (incl. rodata) | 22,636 | 18,748 |
| `.bss` | 256 | 256 |
| **saved** | | **3,888 B (17%)** |

For comparison, the same comparison on x86-64 (`gcc -Os`), where I
originally measured:

| | before | after |
|---|---|---|
| `.bss` | 360 | 360 |
| **`size` total** (text incl. rodata) | **19,768** | **16,503** |
| source lines | 2030 + 732 (header) | 1290, header goes away |

**Saved: 3.9 KB on MIPS (17%) - not the 12 KB estimated in the draft.**
The estimate was wrong, and systematically so: most of `.text` is not
decision logic but **log and format call sites plus the sun math**. Both
scale with the number of *messages*, or are fixed cost - not with the
complexity of the algorithm. Anyone who wants to save more here has to cut
log lines, and those are exactly what carried every one of the twelve
incident diagnoses. The trade is not worth it.

**For RAM it is like in the alternatives enumeration: nothing to gain.**
360 B BSS before and after (the same status statics). The thread locals drop
from ~30 scalars plus three ring buffers (10+6+3 floats) to 15 scalars with
no buffers - together under 200 bytes on the stack. **Anyone who justifies
this rework with memory is justifying it wrong.** The win is the state space
and the bounded click budget; the 3.9 KB is a side effect.

CPU: the scrape returns to the decision tick (§3), but at 2 s instead of
500 ms - net less load than before the P2 optimization, and the IMP gain API
is now only needed for the gain component and the status display.

---

## 9. Risks and open points

1. **The exposure index is the one unproven assumption in the draft.** It
   assumes that the AE on the night path actually shortens the integration
   time once gain hits the floor. Physically it has to - but some ISPs hold
   exposure fixed at an anti-flicker value and only adjust gain; in that
   case `D` degrades to today's behavior (no harm, but no gain either).
   **To be measured before implementation**: on an IR-saturated camera
   (cam-J), log `isp-m0` over a night and check whether `SENSOR Integration
   Time` moves while gain sticks at 256. That is a one-line cron job and
   decides whether §3 holds up.
2. **Night→day latency increases** from 5 s (today's direct trigger) to
   `probe_confirm_s + probe_settle_s` ≈ 25 s. Deliberate: the direct trigger
   is exactly what caused `b4a54f0`.
3. **One click per boot** with persisted night (confirmed by you);
   `boot_probe=never` remains as a valve.
4. **The corpus needs an integration-time channel.** The 14 existing
   scenarios only feed gain - with `max_integration_time` missing, `D = gain`
   holds and they keep running unchanged. New scenarios for §3 need a second
   curve in the scenario JSON and in the `isp-m0` writer of `dn-replay.py`.
5. **Rebaseline `max_switches` per scenario.** Expectation: fewer almost
   everywhere, markedly fewer for `05-inverted-regime` and `09-dawn-ramp`.
   Where it is higher - look closely.
6. **Learning only in stage 2** (§5.4).

---

## 10. Validation plan

1. **Before:** run the corpus against the current state, record
   `max_switches` and `max_wrong_mode_s` per scenario as the baseline.
2. Take the measurement from §9.1 on a real IR-saturated camera.
3. Implement the new automaton, keep the old one behind
   `-DDAYNIGHT_LEGACY`, until 3-5 are green.
4. **All 14 scenarios** must pass `expected_mode`, `max_wrong_mode_s`,
   `restart_equivalence_s`, and `monotonicity`; recast `expect_log`.
5. New scenarios that the current state could not express: *light turned on
   in a dark room* (latency + click budget), *windowless room over 48h*
   (click budget), *boot at night into a lit room*, *camera with no valid
   clock and no geodata*, *IR-saturated scene with an integration-time
   signal*.
6. Recast the property test: monotonicity now holds over
   `next_probe_time(ref, s, hb)` - noticeably simpler to state than over
   `dn_evidence`.
7. Field: 2 cameras for a week with `trace_path` - count clicks/day and hold
   it against today's number.
8. Stage 2 (learning) only after that, with the L2 assertion from §5.3.

---

## 11. Stage 3 - optional

- **Shadow probe (A9).** If the board can switch the IR LEDs separately
  from the mechanical IR-cut (`switch_cmd probe`), a silent LED-off test
  replaces the audible click. **Nothing** changes in the automaton - only
  `switch(DAY)` in (3d) and the verdict get cheaper.
- **LDR/ALS (A7).** A pipeline-independent sensor makes path C absolute
  instead of relative and makes path B unnecessary. Board-dependent, no HAL
  access today. The design is prepared for it: a second source for
  `read_exposure()` plus permission to switch directly.

---

## 12. What implementation changed

The automaton from §4 stands as designed. Five points played out
differently, and **the corpus found** three of them - which is the actual
evidence that the rework was permissible at all:

1. **The size estimate was wrong** (§8): 3.9 KB instead of the estimated
   12 KB. Reason: `.text` consists mostly of log/format call sites and sun
   math, not decision logic.

2. **The flatness test had to bring back the staggered minimum logic.**
   What was designed was a simple range test (`max/min < 1.15`). The corpus
   disproves it in *both* directions: `03-noisy-night` runs ±25% AGC noise
   (a bare running minimum sinks unboundedly in that and kills the
   deferral); `11-dim-lightson` shifts the night pipeline by 12.5%
   permanently (any range threshold that survives the noise calls that
   "flat"). The fix is the **staggered window maximum** from the old
   design: a level only counts once the scene has held it for a full
   `probe_confirm_s`. Noise cannot hold a level; a genuine step can. Costs
   one more scalar (15 instead of 14) and is the only mechanism deliberately
   carried over from the predecessor.

3. **Learning shipped instead of being deferred** (§5.4) - switchable,
   off by default, daily log always on.

4. **`quiet_tier` dropped** (§5.1).

5. **The property test is gone; the corpus replaced it.**
   `tests/dn-probe-props.c` tested `dn_next_probe()` - a pure function over a
   large evidence structure whose value lay precisely in its complexity. The
   new schedule is three predicates over four scalars; a property test over
   that would check nothing that isn't already legible. In exchange, the
   monotonicity assertion was promoted from "cheap observer" to the actual
   check, and now reads the heartbeat deadline out of the trace.

6. **The diagnostic warning needed a new evidence basis.** What was
   designed was "once a day from the learning line." The corpus shows
   immediately why that does not work: scenario 13 runs for 2400 virtual
   seconds; the daily line comes after 60 s (when there is not yet any
   evidence), and then not again for 24h. A misconfigured camera would have
   stayed silent for a full day - worse than before. Now: **three
   consecutive failed probes** whose best day-pipeline value never came
   close to the threshold. Arrives within a few probe intervals, cannot be
   produced by a single unlucky measurement, and is rate-limited once per
   session instead of per probe.

Additionally, the **restart-equivalence oracle** gained precision: it now
asks the question directly, by evaluating the scenario's day-pipeline curve
as a counterfactual ("what would a cold start have decided at this
moment?"). The old oracle had to work with the night pipeline and needed a
ratchet-margin fudge factor for that, to avoid misreporting inverted scenes.
The fudge factor is gone, with nothing to replace it.
