# Day/Night

`src/daynight.c` implements native, automatic day/night switching,
replacing thingino's separate `daynightd` daemon. Built with
`USE_DAYNIGHT` (on by default — see [Building](Building.md)); without it,
stub functions report `enabled=0` and fall back to whatever
`image.running_mode` currently holds.

## What timps does — and does not — control directly

A single background thread (`dn_thread`) decides day vs. night and, on a
change, runs an external **board switch script** as
`<daynight.switch_cmd> day|night` (default script name `daynight`, via
`fork()`+`execlp()` — never `system()`, so a config value can't inject
shell commands). **timps does not flip the ISP mode itself from this
path** — the board script is expected to drive the physical IR-cut
filter and IR LEDs, and then call back into timps's own `/control` to set
`image.running_mode`, closing the loop:

```
dn_thread decides day/night
        │
        ▼
fork()+execlp(switch_cmd, "day"|"night")   (e.g. thingino's `daynight` script)
        │
        ▼
board script: flips IR-cut filter + IR LEDs, then...
        │
        ▼
POST /control {"image":{"running_mode":0|1}}   ("the color hook")
        │
        ▼
control.c: timps_apply_setting("image.running_mode", ...)
        │
        ▼
hal_ingenic.c: IMP_ISP_Tuning_SetISPRunningMode(...)
        │
        ▼
fs_kick_running_mode()   (see below)
```

This indirection keeps the whole board in sync: the same script that
already knows how to drive this specific camera's IR-cut hardware is
responsible for telling the ISP the new mode, rather than timps
duplicating board-specific logic. The mode change is committed
(`cur=target`) even if the script exits non-zero (logged as a warning) —
a missing script only warns once, it doesn't retry every sample.

## The one asymmetry everything follows from

The two optical paths are **not equally trustworthy**, and every rule below
is a consequence of that:

* **Day pipeline** — IR-cut closed, illuminator off, colour tuning. The
  exposure the ISP settles on is an honest measure of ambient light.
* **Night pipeline** — IR-cut open, illuminator on. The camera is partly
  measuring *its own light*. An absolute reading here means nothing; only a
  **change** in it means anything.

So day→night is a plain measurement, and night→day can only be answered by
physically switching to the day pipeline and looking — a **probe**, which
costs an audible IR-cut click. All of the scheduling in `daynight.c` exists
to bound how often that click happens without ever giving up the ability to
correct a wrong night.

> **Design note.** Between 2026-08-02 and 2026-08-16 this file accumulated
> nine separate rules rationing that one probe — backoff, failure ratchet,
> anchor override, evidence skip, `probe_max_skip_s`, trend suspension,
> brightening hold, oscillation breaker, verify deadlines. Each was correct
> and each was bought with a real incident, but because they all gated the
> *same single path* their failures multiplied instead of cancelling; the
> worst case was a camera rendering IR video in broad daylight for four
> hours while a service restart fixed it in ten seconds. The 2026-08-17
> redesign replaces them with independent paths (five today) and one rate limit.
> The full argument is in `dev_notes/DAYNIGHT_REDESIGN_2026-08-17.md`; the
> incident record it came from is in
> [Day/Night Design Notes](Day-Night-Design-Notes.md).

## The metric: exposure index, not gain

```
D = total_gain × (integration_time / max_integration_time)      higher = darker
```

`total_gain` alone has a hard floor at **256** (1.0×). Once the AE rails
there, further brightening of the scene is **invisible** — which is why a
camera resting at 256–268 under its own IR (mounted close to a reflective
object) could never have its brightening detected at all, no matter how the
probe was scheduled. The AE has a second control, and when the gain bottoms
out it keeps shortening the exposure instead, so the product carries the
signal across the whole range.

* Dark scene: integration railed at max → `D == total_gain`. Identical to the
  pre-redesign behaviour, and the reason `day_gain`/`night_gain` keep their
  historic calibration.
* Bright scene: `D` falls far below 256, continuously.
* `max_integration_time` unreadable → `D = total_gain`, i.e. clean degradation
  to the old behaviour.
* Normalising on `max` makes it frame-rate independent.

Both fields come from the `daynight.isp_path` scrape, so that scrape now runs
on every decision tick — which is what the 2 s default `interval_ms` pays for
(the gain half still prefers `IMP_ISP_Tuning_GetTotalGain` where available).

## Five paths, none of which can block another

| | Path | Signal | Covers | Cost |
| --- | --- | --- | --- | --- |
| **A** | day → night | day-pipeline `D` vs `night_gain`, held `day_confirm_s` | dusk, lights off | 1 click |
| **C** | spontaneous probe | *relative* fall of `D` below `probe_jump_pct`% of the night reference, held `probe_confirm_s` | **lights on**; indoor cameras; cameras with no location data — the path that carries most of a fleet | 2 clicks if it fails |
| **T** | trend probe | a 3-minute EMA of `D` below **75%** of a 60-minute one, held `probe_confirm_s` | **dawn** — a ramp too slow for path C's step bar (measured: a real twilight needs 106 minutes to reach it) | 0 clicks; only armed when `daynight.irprobe_cmd` makes the probe silent |
| **B** | heartbeat probe | wall clock only, no sensor | a reference anchored too low, a scene the index cannot read | 2 clicks |
| **D** | boot | one measurement at `t=0` | the persisted mode is a guess | 1 click per boot |

The construction rule, and the thing the previous design lacked:

> **A rationing rule may only be applied while the independent trigger it
> defers to is actually working.**

That is not an aspiration but a runtime predicate (`dn_c_sighted()`): if the
integration-time fields are unavailable *and* the night reference sits at the
gain floor, path C is structurally blind, so the heartbeat is not allowed to
stretch itself on path C's behalf.

## The night reference

One number, and it is a **proof**, not a filter state:

> `ref` = the exposure level at which night was last *proven* — either by
> entering night from day, or by a probe that went and found darkness.

A probe that fails is simultaneously a proof of night *and* a contemporaneous
night-pipeline reading, which is why one variable does the job the old design
spread across `night_baseline` (drifting), `smooth_tg`, `probe_fail_smooth`,
`min_smooth_since_probe` and `brighten_ref`. It never drifts. It moves in
exactly two places, and the failed-probe assignment is the entire "ratchet":
after a failed probe the bar is derived from the level that *did* fire, so a
repeat of the same stimulus (headlights, a dip in an overcast dawn) cannot
re-fire it.

Nothing is load-bearing on any single sample the way `night_baseline` was:

* reference anchored **too high** → one probe fires, finds night, and
  re-anchors at the true resting level. Permanently correct after one
  self-correcting click pair. (The `a5dae07` incident was this exact
  situation looping forever every 25 minutes.)
* reference anchored **too low** → path C goes quiet until the heartbeat
  re-anchors it. Bounded by `heartbeat_max_s`.

It can also be lowered directly, with no probe needed: a silent probe
verdict of "night" — the illuminator carries the scene (`r >=
ir_ratio_night`), or day mode is measured-to-bounce (the filter-cost
projection; that branch forgot the rule until 2026-08-21 and cam-schuppen
re-fired every 26 s against a stale bar, scenario
`26-projection-verdict-no-way-down`) — at a level *below* the current
probe bar (`ref * probe_jump_pct / 100`) is proof in the other direction —
the reference predicted a brightening worth looking at, and the look found
darkness, so the reference described a scene that no longer exists. Without
this a reference anchored too high while the ISP was misbehaving had no way
down at all: measured on a camera whose ISP came up wrong and anchored at
131072, then read 6070 once it recovered — the jump trigger fired every
fourteen seconds, the silent probe correctly answered "the illuminator
carries this scene," and nothing moved. Seventeen probes in a row, which
reads from outside as an IR lamp that keeps switching itself off. Logged as
`night reference lowered to <level>, proven by the silent probe (bar
<level>)`. Corpus scenario `23-stale-reference-no-way-down`.

Since 2026-08-21 the reference additionally refuses **clips**: a reading
taken while the AE is railed (reserve known and `< ir_min_headroom`) is a
clip, not a level — it may prove night, but it may not be remembered. The
post-entry anchor waits for an honest sample (`night reference deferred: …`),
and a failed probe whose pre-probe level was a clip leaves the reference
unset instead of ratcheting the clip in. Until a trustworthy reading arrives
the automaton runs on the configured absolute thresholds and the heartbeat
alone, which changes nothing about its steady-state probe cadence. The same
rule keeps clips out of `filter_cost`, the threshold diagnostic and the
trend memory. Measured: a T23 whose ISP boots railed anchored 131072 where
the scene was worth a twentieth of that, and the probe bar of 65536 then sat
above anything a real brightening could reach. Corpus scenario
`24-pegged-boot-poisoned-reference`.

## The trend (path T)

`ref` answers a **step**; it cannot answer a **ramp**. The bar sits at
`probe_jump_pct` (50%) of a proven night level, and natural twilight takes an
hour or more to get there — measured on `cam-C`, the dawn of
2026-08-18 fell 11839 → 5312 over two hours (a factor of 2.23) and did not
reach the bar until 07:31, 67 minutes after sunrise. Meanwhile the camera
renders IR video in daylight.

So night also keeps two EMAs of the index and compares them with each other:

```
fast = EMA, tau 3 min       (follows the scene)
slow = EMA, tau 60 min      (remembers it)
probe when  fast / slow < 75%,  held probe_confirm_s
```

Nothing absolute is consulted — the question is "is this scene brighter than
it remembers being", which is the one relative question the night pipeline can
answer honestly. Both EMAs are **re-anchored after every verdict**, so an
answered question cannot immediately re-ask itself, and both are frozen while
the illuminator is off (that is a third optical state, not a darker version of
this one).

Measured over 12 cameras and 181 camera-hours of night
(`scripts/dn-trend-eval.py`): **10 of 12 dawns found, 0.22 false fires per
camera-hour** at these constants. Shorter memories fire *later* and find
*fewer*, because they track the twilight instead of noticing it.

> **Only armed when `daynight.irprobe_cmd` is set.** That rate is affordable
> as silent probes — a few seconds of dimmer image each — and not as audible
> ones: it would be roughly 2.6 extra motor movements per 12-hour night. A
> board that cannot switch its LEDs separately keeps path C plus the
> heartbeat.

The constants are compile-time (`DN_TREND_FAST_MS`, `DN_TREND_SLOW_MS`,
`DN_TREND_PCT` in `src/daynight.c`), not config keys: they are dimensionless
and fleet-wide by construction, which is the same property that made the IR
ratio worth having in the first place.

## The silent probe

Where the board can drive its IR illuminator without moving the IR-cut filter
(`daynight.irprobe_cmd`), every request for a verdict tries this first:

```
turn the illuminator OFF
wait probe_settle_s for the AE
r = index(illuminator off) / index(illuminator on)
turn the illuminator back ON
```

`r` is a ratio, so it is dimensionless and the same pair of thresholds fits
every camera in a fleet. No absolute level does: genuine daylight readings
span a factor of 63 across twelve cameras at one instant.

| verdict | condition | costs |
|---|---|---|
| night | *lit* reserve `< ir_min_headroom` | nothing — `r` divides two clips and says nothing; railed-dark is still night evidence |
| night | `r >= ir_ratio_night` | nothing — the illuminator was carrying the scene |
| night | reserve `< ir_min_headroom` | nothing — the meter is pegged at the *dark* end, which is itself proof |
| escalate | reserve **unknown** (no ceiling fields in the ISP dump) | one audible probe — a lit room and a railed meter look identical without the reserve, so the day pipeline judges (2026-08-21; previously this fell into the "pegged" row and answered night forever) |
| day | `r <= ir_ratio_day`, reserve sufficient, **and the filter-cost gate below** | one switch |
| escalate | anything in between | one audible probe |

The headroom test is not a refinement. An AE with nothing left cannot respond
to the illuminator going off, so it returns `r ≈ 1` — indistinguishable from
daylight. Measured on a pitch-dark scene: `r = 1.14`, below `ir_ratio_day`.
Only the reserve separates that from a genuinely lit room.

A camera whose ISP dump has no ceiling fields (`MAX SENSOR analog gain` /
`MAX ISP digital gain`) has no reserve at all — and therefore none of the
clip protection either. That is announced once per session as a WARN
("the ISP dump reports no gain ceilings …"), not left to be inferred from
`hr=-1` in a probe line. Both fleet T20s do publish the ceilings (jxf23:
32 units ISP digital, jxf22: 45 — same SoC, different sensors, so a
hard-coded value would be wrong on one of them); the warning exists for the
dump variant nobody has met yet.

Both ratio thresholds ship at `2.0`, so with the defaults the "escalate" row
never fires. Setting `ir_ratio_day` below `ir_ratio_night` opens a deliberate
dead zone, trading an occasional audible click for not deciding on thin
evidence.

### When the illuminator command doesn't work

A board that cannot switch its IR LEDs separately from the filter answers
every attempt to run `irprobe_cmd` with a failure. Falling back to the
audible probe on each of those tries would be worse than never having
tried it — a motor movement paid for a command that was never going to
succeed. **Two consecutive failures retire the silent probe for the
session**, and the trend trigger (path T, which only exists to make silent
probing cheap) retires with it, leaving the jump trigger (path C) and the
heartbeat — the fleet's pre-existing behaviour on a board without a working
`irprobe_cmd`. Not persisted: a restart re-tests it, which is the cheap
direction to be wrong in. Logged once, at the second failure: `'<cmd>'
failed N times - retiring the silent probe for this session; the trend
trigger goes with it, leaving the jump trigger and the heartbeat`.

`POST /control {"daynight":{"probe":1}}` asks for one silent probe on the
next tick — useful to confirm a camera can see daylight without waiting for
the jump trigger, the trend or the heartbeat. It is rejected (not silently
swallowed) when there is no `irprobe_cmd` configured or the probe has
retired itself as above; see [HTTP /control API Reference](HTTP-Control-API.md).

### What switching to day also costs

A low ratio answers *is the illuminator earning its keep*. It does **not**
answer *is there enough light to run day mode* — because switching to day also
closes the IR-cut filter, and in a dim interior that filter alone can cost a
factor of three.

Left unguarded the two rules chase each other. Measured on a bedroom-class
scene with one dimmed lamp: the probe reports `r = 1.27` and switches to day;
the day pipeline, filter now closed, reads 11480 against a `night_gain` of
4096; path A sends it straight back; the illuminator returns and the ratio
reports 1.27 again. Eight round trips in one evening, each an audible click.

So the automaton measures `filter_cost` — the day reading over the night
reading the verdict left, 3.27 in that scene — the first time a ratio
verdict is undone that way, and from then on requires

```
current night reading × filter_cost < night_gain
```

before it will act on a ratio at all. One exploratory switch is unavoidable:
nothing can know the day-pipeline reading without trying it once. Every later
pass refuses in the log (at `LOGD`, like every per-probe line — see
[Logging](Logging.md)) and costs nothing:

```
silent probe (trend): r=1.27 - the room supplies the light, but day mode
would read ~11500 (3.27x the night level) against night_gain 4096, staying
night
```

**`filter_cost` may be below 1.** The first version of this guard only
learned when the day reading came back *higher* than the night one, on the
assumption that closing the IR-cut filter can only cost light. A T20 whose
illuminator contributes nothing measurable disproved that: the silent probe
returned `r = 1.00` with ample AE reserve, the day pipeline read 6166
against a night level of 8171 — lower, not higher — and 6166 was still
above `night_gain`, so the guard never armed and the camera flapped 11
times in a day. What `filter_cost` captures is not "what the filter costs"
but what day mode actually reads in this scene, and that is meaningful in
both directions; the log line where it's first measured says so:

```
day mode reads 0.75x the night level here (6166 vs 8171) - a ratio verdict
now needs the night reading below 5428
```

At real dawn the night reading falls, the product drops below `night_gain` on
its own, and the switch happens — no special case, no calendar. `filter_cost`
is measured per scene rather than configured, and is deliberately not
persisted: a quantity that moves with the scene is cheaper to re-measure once
per restart than to keep correct in flash. Corpus scenarios
`21-ir-ratio-flap-cam-sz` (factor above 1) and `22-ir-ratio-flap-t20` (factor
below 1) hold this to two switches where the unguarded automaton spent
twelve.

## The probe

```
switch to the day pipeline
wait probe_settle_s for the AE
judge ONCE against day_gain:  below → stay (day confirmed)
                              else  → revert immediately, ref := pre-probe level
```

There is no third, "ambiguous" outcome — which is what the entire
dead-zone/`dn_verify`/deadline-extension machinery used to exist for. The
revert bypasses `transition_s`: retreating from a wrong probe is free.

`probe_min_gap_s` is the **only** rule rationing probes, so the worst-case
click rate is a property of the configuration rather than of interacting
heuristics. This is also why there is no longer an oscillation breaker: an
IR-reflection feedback loop needs a *threshold crossing* to flip night→day,
and no threshold can do that any more.

## The heartbeat

The only bound on how long a wrong night can last, and deliberately a flat
interval rather than a multiplying backoff — a backoff is precisely how the
previous design turned a bounded guarantee (`night_reconfirm_s`) into an
unbounded one (×4, plus a skip gate, plus a 12 h outer bound).

* scene moving since the last probe → `heartbeat_s` (4 h)
* scene flat (smoothed range within 15%) **and** path C sighted →
  deferred, but never past `heartbeat_max_s` (12 h) since the last probe
* a configured calendar can only pull the deadline **in**, to the next
  sunrise — never push it out

A dark closet therefore costs two click pairs a day, and a camera that sees
anything at all costs six. Note what the calendar is *not* doing: it never
decides. That is what keeps basements, windowless rooms and artificially lit
spaces working, and it is why a camera with no location data loses only
sharpness of scheduling, not correctness.

## Boot

The persisted `image.running_mode` is a guess about a scene nobody measured.

* persisted **day** → we are already in the honest pipeline; one settled
  reading decides. **Zero clicks.**
* persisted **night** → one probe turns the guess into a measurement
  (`daynight.boot_probe=1`, the default). **One click per boot.**

This makes *restart-equivalence* — the property the design notes identified
as the one every stuck-mode incident violated ("a service restart fixes it")
— literal at `t=0` rather than something to be derived. It is also what
replaced dead-zone adoption, the symmetric verify deadlines and the
still-brightening extension.

Boot also pushes the persisted `image.running_mode` into the ISP directly
(`hub_control()`), once — the only path other than a switch that ever tells
the hardware anything, since adopting a persisted mode otherwise only set the
automaton's own state. `switch_cmd` is deliberately **not** run for this:
the board was already right (illuminator on, ISP already reporting Night),
only its tuning was wrong, and a filter movement per boot would be a real
mechanical cost the evidence doesn't ask for.

**Exception — the railed boot** (2026-08-21). When the persisted mode is
night and the AE comes up with **0 units of reserve**, the push above is a
no-op — the ISP already holds that value — and measured on a T23 the meter
still read its rail 25 minutes later. Every reading in that state is a clip
and even the silent probe is blind (`r` of two clips is exactly `1.00`).
What demonstrably re-tunes the AE is a real transition: `day` and back read
a factor 23 lower within 12 s of each leg. Boot therefore fires the audible
probe directly (`why=boot pegged`, skipping the silent path), even with
`boot_probe=0`: if the scene is day the probe confirms it and the movement
was owed anyway; if it is night the revert re-tunes the ISP and the
reference anchors from the first honest reading. One click per *railed*
boot is the cheaper side of that trade; a healthy boot is unaffected. Unlike a switch, this push does
**not** arm the repeating post-switch re-assert (below) — arming it here
raced the boot probe, which can decide within seconds: the pending re-assert
then overwrote that fresh decision with the stale persisted value a second
and third time, eight seconds apart, and a living room stayed in night mode
through daylight because of it.

## Learning (removed 2026-08-22)

An earlier version of this automaton recorded each confirmed day's lowest
`D` and, with `daynight.learn=1`, let the median of the last 8 raise the
effective `day_gain` when the configured value turned out to be unreachable
— the failure that had three cameras stuck in night on 2026-08-16. In
practice its own safety clamp (never raise the threshold past `night_gain/2`)
could not raise it far enough for the cameras that actually needed it, so the
config surface (`daynight.learn`, `daynight.state_path`) was removed rather
than kept as dead weight. `daynight.diagnose_thresholds` covers the same
failure mode today: it names the value to raise `day_gain` above instead of
trying to raise it automatically.

## Anti-flapping guards

Three, all unchanged from the pre-redesign machinery, which the design notes
correctly identified as the part that was working:

* **`transition_s`** — minimum dwell between switches. A failed probe's
  revert bypasses it.
* **Confirmation windows** — `day_confirm_s` for path A, `probe_confirm_s`
  for path C. Also keeps the `SetISPRunningMode` call from landing mid AE
  ramp, which is the stuck-mode class the latch section below covers.
* **Post-switch re-assert** — `DN_REASSERT_MS`/`DN_REASSERT_COUNT`, plus a
  warning when `running_mode` never follows a switch (a lost POST from the
  fire-and-forget board hook chain, or a manual override).

## Regression corpus

`scripts/dn-replay.py --all scripts/dn-scenarios` drives a host-built
`timpsd-sim` through synthetic two-pipeline traces on a virtual clock. Fifteen
scenarios, one per historical incident plus the two the redesign added, each
asserting a mode timeline, a **click budget**, a maximum continuous wrong-mode
run, restart-equivalence and probe-time monotonicity.

Two of them exist as a pair and should stay that way:

* **09-dawn-ramp** feeds no integration-time channel, so the exposure index
  degrades to bare gain and its night curve floors at 260 — a sensor pinned at
  the gain floor. Path C is structurally exhausted exactly as dawn completes,
  and recovery costs one `heartbeat_s`. That is the honest bound of the
  degraded path.
* **15-dawn-ramp-exposure-index** is the same dawn *with* the integration-time
  channel. The scene keeps producing evidence, path C fires on its own, and
  day is confirmed within minutes — with no extra rule, timer or state.

The difference between the two is the entire argument for changing the metric,
written as an assertion rather than a claim.

## The ISP running-mode latch quirk and `fs_kick_running_mode()`

Even with all of the above working correctly, one more hardware quirk
remained and was root-caused on a real deployed camera (cam-L Y4,
T23/sc2336) on 2026-08-01: **the ISP core only processes a queued
`SetISPRunningMode` change while FrameSource channel 0 is actively
delivering frames.**

With chn0 idle-stopped — the normal state when nobody is watching the
main stream, per timps's [on-demand encoding](Architecture.md#on-demand-framesource-activation-hal_ingenicc)
design — `IMP_ISP_Tuning_SetISPRunningMode()` still returns success, but
the driver silently leaves the change queued indefinitely. This happens
**even while a different, always-scaled substream channel (chn1) keeps
streaming the whole time**: on the camera where this was found, the dusk
day→night switch (plus both automatic re-asserts) all reported success,
`/proc/jz/isp/isp-m0` kept reporting "Running Mode: Day," and the
always-on substream showed a visibly magenta, IR-lit image all evening.

The fix, in `src/hal/hal_ingenic.c`:

```c
#define FS0_MODE_KICK_US 500000
static void fs_kick_running_mode(void)
{
    if (!g_hcfg || !g_hcfg->video[0].enabled) return;
    fs_use(0);
    usleep(FS0_MODE_KICK_US);
    fs_unuse(0);
}
```

After every `running_mode` apply, the HAL briefly forces FrameSource
channel 0 active — via the same refcounted `fs_use()`/`fs_unuse()` pair
that on-demand streaming already uses — for 500ms (roughly 12 frames at
25fps, generous margin over the 1–2 frames empirically needed to latch
the mode). If chn0 is already streaming (a client is watching the main
stream), this is a cheap no-op ref bump; if chn0 was idle, it's a real
but transient wake that guarantees the queued mode change actually
applies before the channel is allowed to idle-stop again.

This means the return code of `SetISPRunningMode()` is now also checked
and logged (`LOGW` on failure) rather than ignored outright, and a
diagnostic (debug-level only) read-back via `GetISPRunningMode` is
logged, with the caveat that this readback is a libimp *userspace* value
(there is no kernel read path on T23) and so isn't by itself proof the
pipeline actually latched — it's `fs_kick_running_mode()` that provides
the actual fix.

## Machine-readable log lines

Two lines are deliberately fixed-format, columnar `key=value` text: fleet
dashboards grep for them, and a reworded sentence would empty their count
silently, the same failure the 2026-08-17 hand-run exposure campaign had
when its script quietly stopped being called. Both use `-1` for "not
measured", the same convention as the `GET /control` status fields, and both
avoid spaces inside a value.

**Every switch**, at `LOGI` on the same line as the human-readable reason:

```
switching to night (heartbeat): daynight night [mode=night exp=6182 ref=-1 bar=768]
```

The `[...]` tail: `mode` is the mode being switched *to* (`day`/`night`);
`exp` is the exposure index at the moment of the switch; `ref` is the night
reference at that moment (`-1` when not currently in night, e.g. a
day→night switch); `bar` is the day threshold (`day_gain`) — the same value
on every switch line regardless of direction, since it is read once per tick
rather than chosen per branch.

**Every silent probe**, once per verdict, at `LOGD`, at the point where all
branches have decided:

```
probe: r=1.05 lit=3350 dark=3520 hr=199 verdict=day mode=night ref=12098 why=brightening
```

`r` is the IR ratio (`dark / lit`, `-1` if the probe got no usable reading);
`lit`/`dark` are the two raw readings compared — with the illuminator on and
off — the same pair the hand-run campaign used to collect by script (`-1`
alongside `r=-1`); `hr` is the AE headroom in log2 units at the dark
reading; `verdict` is `day`, `night`, or `escalate` (inconclusive — the
audible probe was asked for instead); `mode` is the automaton's mode at the
time of the probe; `ref` is the night reference (`-1` outside night); `why`
is the trigger that asked for this probe (`jump`, `trend`, `heartbeat`,
`boot verify`, `requested`, …) with spaces replaced by underscores, since a
`key=value` value can't carry one; `lit_hr` (appended 2026-08-21) is the AE
headroom at the *lit* reading, the number that decides whether `r` compared
two measurements or two clips.

Since 2026-08-20 this line is `LOGD` — a measurement, not an event (see
[Logging](Logging.md)). A collector that counts or plots it needs

```
general.debug_modules = daynight
```

in `/etc/timps.conf`; without it the line simply stops, and a counting
dashboard reads "no probes" where it should read "not logged". The
`switching to` line above stays at `LOGI` unconditionally.

## Live control and status

| Action | Effect |
| --- | --- |
| `POST /control {"daynight":{"enabled":false}}` | Switches to manual mode; the thread keeps sampling for status but stops forcing switches. |
| `POST /control {"daynight":{"mode":"schedule",...}}` | Switches decision source; validated against `auto`/`schedule` (and the legacy `sensor`/`time`/`sun` spellings) before being applied. |
| `POST /control {"image":{"running_mode":1}}` | Manually forces the ISP mode (what the board switch script itself calls). Re-posting the *same* value still re-drives the ISP (see [HTTP /control API Reference](HTTP-Control-API.md)) — necessary precisely because of the latch quirk above. |
| `POST /control {"daynight":{"probe":1}}` | Arms one silent IR probe for the next tick, without waiting for the jump trigger, the trend or the heartbeat. A command, not a setting — counted in `accepted`/`rejected` like `record.clip`. Rejected when there is no `daynight.irprobe_cmd` configured or the silent probe has retired itself for the session (see "When the illuminator command doesn't work" above). |
| `GET /control` `"daynight"` object | Read-only status: `enabled`, `mode` (0 day/1 night), `brightness` (%), `total_gain` (IMP `[24.8]` linear), **`exposure`** (the index the decision actually runs on — plot this one when diagnosing), `ae_luma`, `night_baseline`/`day_trigger` (kept under their old key names, now carrying the proven night reference and the probe bar, `-1` outside night), the configured thresholds, and — always — today's computed `sun_computed_sunrise`/`sun_computed_sunset`. |
| `GET /events?stream=daynight` | Pushes the same status object whenever mode flips, brightness moves ≥1%, or gain moves ≥5% relative (or ≥8 absolute near zero) — see [HTTP /control API Reference](HTTP-Control-API.md#event-types). |

See [Configuration Reference](Configuration-Reference.md#daynight--automatic-daynight)
for the full key table (including which fields are settable via
`/control` vs. config-file-only).
