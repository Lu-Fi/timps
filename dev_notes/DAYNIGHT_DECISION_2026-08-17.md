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

**r thresholds 1.5 / 3.0** - measured 25.1 against 1.41/1.27 against 1.00/0.96.
The gap is a factor of 18; 3.0 leaves a factor of 2 of room on either side.

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
