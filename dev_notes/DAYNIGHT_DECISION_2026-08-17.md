# Day/night - the decision after the field measurement

**Date:** 2026-08-17, evening · **Status:** decided, not yet implemented
**Supersedes:** the night-to-day half of `DAYNIGHT_REDESIGN_2026-08-17.md` (revision 2).
Everything else in that document - the exposure index, the day-to-night
direction, boot behaviour, the calendar as a pure scheduler - still stands.

---

## Why there is a second decision

The morning's design decided night-to-day on an **absolute threshold**
(`day_gain`) read off the day pipeline. A field measurement across twelve
cameras and one full night refuted that choice. Three measurements, each
sufficient on its own:

**1. The range of "day" cannot be captured fleet-wide.** In the same minute,
under the same overcast sky, in genuine daylight: index 11 to 709 across seven
cameras - a **factor of 63**. On a single camera the same value swung by a
**factor of 4.5** within one morning (3238 to 709).

**2. A real event fell straight through the threshold.** At 01:12:47 the light
came on in an unlit interior. The night index dropped from 10856 to 824 in **a
single sample**. With the light on, the day pipeline read **1024** - above the
default `day_gain` of 768. The morning's design would have said "still night"
and stayed in greyscale. The old daemon, with its *relative* baseline, got this
right. **The design would have been a regression here.**

**3. There is a measurand that does not have this problem.** Switch the IR
illuminator off briefly and measure the ratio, and it separates cleanly:

| | ratio r | |
|---|---|---|
| unlit interior, genuine night | **25.1x** | the IR is doing the work |
| location that should have been day | 1.41x | the IR contributes almost nothing |
| location that should have been day | 1.27x | likewise |
| day camera, control | 1.00x | |
| day camera, very bright | 0.96x | |

Between 1.41 and 25 lies **a factor of 18 of empty space**. A single
fleet-wide threshold classifies everything correctly - with no per-camera
value, no learning, no calibration. And the two cameras this test flagged as
"ought to be day" are exactly the same two that the night measurement had
independently shown to be stuck.

---

## The decision

> **Night-to-day is decided by the contribution of the camera's own IR
> illuminator, not by an absolute level.** The camera does not ask "how bright
> is it" - that varies by a factor of 63 - but "am *I* making this light, or is
> the room". That question is dimensionless and identical everywhere.

The interference that caused the problem becomes the measurement signal.

### The automaton

**Day to night** is unchanged: index above `night_gain` for `day_confirm_s`
means night. An honest measurement on the honest pipeline, an absolute
threshold - but an uncritical one. It sits at 4096, while everything
interesting happens around 1000.

**Night** keeps two EMAs of the index (tau 3 min and 60 min) and fires the
**silent probe** when any one of three conditions holds:

> *Read "What shipped - 2026-08-18" at the end before this table.* All three
> triggers exist in the code, but the Jump row measures against the proven
> `ref`, not against the slow EMA, and the Trend row is armed only where the
> probe is actually silent. Both differences are deliberate and are argued
> there.

| | Trigger | Catches | Confirmation |
|---|---|---|---|
| **Jump** | raw value < 50 % of the slow EMA | light switch, shutters, gate | 3 samples (~6 s) |
| **Trend** | fast/slow EMA < 75 % | dusk and dawn | `probe_confirm_s` (15 s) |
| **Heartbeat** | nothing for `heartbeat_s` | everything else | - |

**The silent probe:** IR LEDs off, wait `probe_settle_s` (8 s), measure
`D_dark`, LEDs back on. No filter click, no mechanism.

```
r = D_dark / D_illuminated

r >= 3.0                       -> night confirmed. Re-anchor the slow EMA.
                                  Zero clicks.
r <= 1.5 and the AE had headroom -> DAY. Switch the board. One click.
no headroom, railed at the dark end -> night confirmed (see below).
otherwise                      -> inconclusive: audible fallback probe,
                                  rate-limited by probe_min_gap_s.
```

### Being railed is an answer, not blindness

Two of the twelve cameras sit with their entire exposure automation at the
rail: integration at maximum, analog gain at maximum, ISP digital at maximum.
Switching the IR off changes nothing there, r stays 1.00 - and that looks like
"day".

The way out lies in the *direction* of the rail. These cameras are exhausted at
the **dark** end: the scene sits at or below the darkest point the sensor can
measure. **That is itself the proof of night.** A rail at the bright end cannot
occur in night mode.

Permanently dark locations therefore cost **zero audible clicks** - the
heartbeat confirms silently, the probe confirms silently. Over the measured
night the old daemon spent six clicks there for zero information.

And the loop closes by itself: if the gate opens or the light comes on at
cam-A, the index falls away from the rail - the jump trigger fires, *and
because the value has moved, the automation has headroom again*, so the ratio
can now answer. The blindness dissolves at exactly the moment it would matter.

Headroom test: `(max_analog - analog) + (max_digital - digital) >= 8` log2
units, or integration time below 90 % of maximum.

---

## Why these numbers

Every one is measured, none guessed.

**r thresholds 1.5 / 3.0 - SUPERSEDED 2026-08-19, both now 2.0.** The original
pair came from single instants spanning 1.41..25.1, an apparent factor of 18 of
empty space. The full dusk-to-dawn campaign this document asked for (twelve
cameras, 2026-08-18/19, 37-62 probe pairs each) shows a much tighter night, and
both values were wrong in the same direction:

| | r | |
|---|---|---|
| darkest genuine night, with AE headroom | **2.38** | cam-C, an unlit outbuilding |
| a DIMMED bedroom light | **1.50** | cam-D, confirmed against the house's own switch log |

3.0 was too high: cam-C and cam-E sit at 2.33..2.38 all night with headroom to
spare, so the rail rule cannot rescue them - they would have landed in
"inconclusive" and fallen through to the AUDIBLE probe on every heartbeat, i.e.
exactly the clicking this design exists to remove. 1.5 was too tight: the first
sample of that dimmed light read 1.65 and would have been missed, delaying a lit
room by one probe interval.

Both are now 2.0 and deliberately EQUAL - the inconclusive band between them
produced nothing but audible probes on this fleet, and one boundary separates
1.50 from 2.38 with about 30 % of margin either side.

**The threshold is not the load-bearing part.** Anything in 1.8..2.2 returns the
same verdicts across the whole campaign. What does the work is the confirmation:
every single sample below 2.0 during the core night was an isolated outlier, and
the only run of consecutive ones was the real light. Four apparent outliers at
06:20-06:40 were dawn on cameras that switched to day moments later.

One apparent cost was WITHDRAWN on closer inspection, and the way it fell apart
is worth keeping: cam-A produced five consecutive sub-threshold samples at
20:05-20:25 with headroom 123..142, which read as a dusk transition the probe
would act on. It is not. That camera is the designated test camera and was being
worked on all evening. The give-away is in the data rather than in the log: its
headroom sits at 1 - fully railed - at 20:00, jumps to 142, and is back at 1 by
20:30, where it stays for the rest of the night. Dusk is a slow ramp; an
exposure automaton cannot go from railed to five stops of reserve and back
inside twenty-five minutes on daylight alone.

So the fleet-night contains exactly ONE confirmed trigger, and it is the real
light. The lesson is about the corpus, not the design: samples from a camera
that is being worked on are not measurements, and the test camera needs
excluding by default rather than by noticing afterwards.

**Jump at 50 %** - the indoor light produced 0.076 in one sample. AGC noise sits
at +/-25 %, a single sample at 0.5 is beyond 2 sigma, and three samples of
confirmation take care of the rest.

**Trend 75 % at tau 3/60 min** - parameter sweep over the real night: 7 of 8
twilights detected, **0.13 false triggers per camera per hour**. My first
choice (tau 15 min) detected only 1 of 8 - natural twilight is slow (a factor
of 2.2 over 67 minutes), so the slow EMA has to be a real memory and not a rate
measure.

**Settling 8 s** - measured: the day pipeline is stable from the first sample
after the switch (1024, 1024, 1024, 1116, 1092, 1046). An outlier occurs only
at the instant of switching; 8 s is comfortably past it.

**Heartbeat 30 min** instead of 4-12 h - it is silent now, so it may be
frequent. The upper bound on a wrong night mode drops from twelve hours to half
an hour.

---

## What the morning design loses

`ref` and its ratchet (the slow EMA carries that, and every probe re-anchors
it) · the staggered window minimum · `dn_c_sighted` (replaced by the headroom
test, which is a measurement rather than a heuristic) · `probe_jump_pct` · the
heartbeat deferral · the upward learning of `day_gain`.

`day_gain` stays - but only as the verdict threshold of the **audible fallback
probe**, i.e. for the case where the ratio cannot answer. It is no longer a
critical value. `daynight.learn` stays as an option that can be switched off,
with its daily log line, but loses its urgency.

State: roughly **eleven scalars**, no ring buffers.

---

## Expected behaviour

| Case | before (measured) | expected |
|---|---|---|
| ordinary camera, one day | 5-26 clicks | **2** (dawn and dusk) |
| permanently dark location | 2-4 clicks | **0** |
| light switched on in a dark room | 31 s to colour | **< 15 s** |
| twilight | spread -184 min ... "never" | one probe, then the switch |
| wrong night mode, upper bound | 12 h | **30 min** |

---

## Open points

1. **The cost of the silent probe is unmeasured.** It darkens the picture for
   about 10 s. Whether motion detection sees that as an event I do not know -
   it is the one remaining unknown, and it determines whether `heartbeat_s` can
   stay at 30 minutes.
2. **Boards without separately switchable IR LEDs** fall back entirely to the
   morning design (audible probe, absolute threshold). On this fleet that
   affects no camera - all twelve can do it - but the code has to be able to.
3. **The ratio across twilight** has not yet been measured over a full
   transition, only at single instants. The running night measurement will
   supply it.
4. **Foreign IR in frame breaks the ratio's premise.** Added 2026-08-18 after
   inspecting night snapshots from `cam-H`: the blown-out highlight
   in its picture is the **IR ring of the second camera in the same room**,
   pointing roughly into the lens. The probe measures `D(own IR off) /
   `D(own IR on)` and thereby assumes the camera's own illuminator is the
   dominant IR source. Here it is not, and two distinct faults follow:

   - **Scene contribution.** Whatever the neighbour contributes to the actual
     illumination does not disappear during the probe. It pushes `r` towards
     1.0, i.e. towards a false "day" verdict in the middle of the night.
   - **Poisoned metering.** The LED ring is clipped white in frame. The AE
     partly regulates on that spot instead of on the room and stops down,
     lowering `D` independently of how dark the room really is. This corrupts
     *both* halves of the ratio and the absolute day-to-night threshold as
     well.

   Measured `r` on this camera swings between **3.0 and 32.7 within forty
   minutes**. Steady flooding would pin `r` near 1.0; a swing of that width is
   the signature of an exposure loop oscillating between two metering states.
   The metering fault therefore appears to dominate the illumination fault -
   but neither is under the daemon's control.

   **Mutual excitation.** If both cameras run this design, they drive each
   other: camera A probes, camera B's scene darkens, B's jump trigger fires, B
   probes, A darkens. Two automata sustaining each other with nothing having
   changed in the room. This is not hypothetical - both cameras have been
   running the measurement crons unsynchronised since 2026-08-17.

   **Consequences for the threshold work:** `cam-H` is unusable as a
   calibration input for `ir_ratio_night` / `ir_ratio_day`. Its values must be
   excluded from the fit, not averaged in.

   **Candidate mitigations**, cheapest first:
   - *Physical:* re-aim or rotate one of the two cameras so the other's LED
     ring leaves the frame. Costs a minute, fixes both faults at the source,
     and needs no code. Strongly preferred.
   - *Per-camera opt-out* (`daynight.ir_probe=0`): fall back to the audible
     probe and the absolute threshold on affected cameras. Honest, small, and
     on this fleet it affects exactly one pair.
   - *Coordination between cameras* - rejected. Requires inter-camera
     signalling for a problem that a screwdriver solves.

   A region-restricted measurement excluding the bright spot is not available:
   the index comes from the ISP's global AE, not from a per-region computation.

---

## What shipped - 2026-08-18

Written after implementing the trend trigger, and the place to look first when
this note and `src/daynight.c` disagree.

The **silent probe and its ratio verdict** shipped with `77b67df` on
2026-08-17 (`irprobe_cmd`, `ir_ratio_night`/`ir_ratio_day`, the headroom test,
"being railed is an answer"). The **trend trigger did not**, and for a day the
automaton above was two thirds implemented while this document described all
three of it. That is now closed, with three deliberate differences from the
text above.

### 1. The trend trigger, as specified - and re-measured

`fast` (tau 3 min) and `slow` (tau 60 min) EMAs of the exposure index, fired
when `fast/slow < 75 %`, held for `probe_confirm_s`. The constants are the ones
this note chose. They were re-swept on the *following* night's data
(`private/messungen/2026-08-18_daemmerung`, 12 cameras, **181 camera-hours
actually in night mode**) before being written into the source, and they
replicate:

| tau fast/slow | bar | dawns found | false fires per camera-hour |
|---|---|---|---|
| 3 / 60 min | **75 %** | **10 of 12** | **0.22** |
| 3 / 60 min | 70 % | 9 of 12 | 0.15 |
| 3 / 15 min | 75 % | 8 of 12 | 0.19 (and 30-70 min later) |
| 3 / 10 min | 75 % | 6 of 12 | 0.13 |

The first sweep's 7-of-8 and 0.13/camera-hour were a different night and a
different subset; the shape is the same one, and the two cameras nobody finds
are the permanently dark garage and a camera whose AE never leaves its rail -
both heartbeat-carried by construction. `scripts/dn-trend-eval.py` produces
this table and now reports **both** halves of the verdict: an earlier version
reported only false fires, which is half a verdict, since a threshold of zero
has a perfect false-fire rate and finds nothing.

Acceptance test: corpus scenario **20-dawn-trend-schuppen**, built on the
measured dawn of `cam-C` (11839 -> 5312 over two hours, a factor of
2.23 - this note's "natural twilight is slow", in one measurement). The jump
bar is not reached until 07:31; the trend fires at 06:45. On the recorded
night the real camera reached neither: its mode column reads Night continuously
from 02:52 past the end of the window. Replayed, the pre-change build spends
**2570 s in the wrong mode** and the post-change build **0**, for one audible
click either way.

### 2. The trigger is armed only where the probe is silent

Not in the text above, and it follows from the text above. The affordability
argument for a generous threshold is *"a false fire costs a few seconds of
dimmer image, not an audible click"*. Where `daynight.irprobe_cmd` is unset
that sentence is false: 0.22 false fires per camera-hour becomes ~2.6 **motor
movements** per 12-hour night, against the 2 clicks a day this design exists
to reach. So a board without separately switchable LEDs keeps jump plus
heartbeat - which is open point 2 arriving as code rather than as a promise,
and which is also why the fifteen pre-decision corpus scenarios are unaffected
by this change: none of them supplies a `night_gain_noir` curve, so none of
them gets an illuminator.

### 3. `ref` and its ratchet STAY - the slow EMA does not replace them

This note said the slow EMA carries `ref`, and that the ratchet and the
staggered window minimum could go. They did not, and should not:

- `ref` moves **only on proof** - entering night, or a probe that found
  darkness. That is precisely what makes `0f5fc80` (a reference drifting into
  a night-long flap loop) and `b4a54f0` (the same dawn dip re-firing five
  times) structurally impossible. A 60-minute EMA drifts by construction; it
  is a memory, not a proof, and swapping one for the other would reopen a
  class of incident that cost twelve diagnoses to close.
- The two answer different questions and neither subsumes the other. `ref`
  and the jump bar catch a **step** the memory would smooth away over its own
  time constant; `fast/slow` catches a **ramp** that never reaches the step
  bar - measured, the shed's dawn needs 106 minutes to get there.
- The staggered window minimum is load-bearing for the heartbeat deferral for
  the reasons already recorded in `DAYNIGHT_REDESIGN_2026-08-17.md` §12.2, and
  the trend pair does not answer that question at all.

Cost of keeping both: three scalars (two EMAs and a hold timestamp), 14 -> 17.
The EMAs are re-anchored on every verdict, exactly as this note specifies for
the slow one, so an answered question cannot immediately re-ask itself.

### 4. No third, medium time constant - the observation is real, the remedy is not

The measurement record for that night
(`private/messungen/2026-08-17_18/ground-truth.md`, point 2) concludes that a
third, medium (~10 min) constant is needed, because the 21:03 bedroom light
fell inside the ongoing dusk (7845 -> 5087, a factor of 1.54) and registered
against neither trigger. **The observation is correct and the remedy is
refuted by the same data.** Replayed on that camera's own samples:

- against the 60-minute memory the ratio bottoms at **1.15** - the dusk had
  left the memory far *below* the current level, so a brightening event during
  a darkening trend cannot show up as one. Confirmed exactly as recorded.
- against a **10-minute** memory it bottoms at **0.87** - still short of the
  75 % bar. The third constant does not catch the event that motivated it.
- the ~88 % bar it *would* take sits inside the +-25 % AGC noise band this
  note already measured. At 3/10 and 88 %: **0.44 false fires per camera-hour,
  double**, and still only 9 of 12 dawns - fewer than 3/60 alone.

A factor-1.54 event is what the heartbeat is for, and the heartbeat bounds it
at 30 minutes. Closed, not deferred.

### 5. The trend constants are not config

`DN_TREND_FAST_MS` / `DN_TREND_SLOW_MS` / `DN_TREND_PCT` live in
`src/daynight.c` beside `DN_ALPHA` and `DN_MOVED_MARGIN`, not in
`ms_daynight_cfg`. They are dimensionless and fleet-wide by the same argument
that made the ratio worth having: the whole point is that they do not need a
per-camera value. `day_gain`/`night_gain` are config because they vary by a
factor of 63 across the fleet; these do not vary at all. If they ever need to
be tunable at runtime, that is a `daynight.trend_pct` field in `config.c`'s
table **and** a matching key in `control_daynight_json()`, and the second half
is the one that must not be forgotten - a POST-settable key the GET does not
report back is worse than no key.

### Still not done

Nothing above touches open points 1 (the motion-detection cost of the silent
probe) or 4 (foreign IR in frame). Point 4 in particular now has a second
edge: path T fires on a *relative* move, and `cam-H`'s measured `r`
swing of 3.0 to 32.7 within forty minutes is exactly the kind of exposure-loop
oscillation that produces relative moves out of nothing. On that camera the
mitigation is still the screwdriver, or clearing `daynight.irprobe_cmd` (the
per-camera opt-out of open point 4, spelled `ir_probe=0` there) - which, note,
now also disarms path T, since the two are gated on the same setting.
