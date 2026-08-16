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

## Decision sources (`daynight.mode`)

The ISP's exposure state is sampled every `daynight.interval_ms`
regardless of mode (so the WebUI's live gain/brightness readout stays
populated in every mode); only the *decision* differs:

| Mode | Decides by |
| --- | --- |
| `sensor` (default) | ISP total gain (primary) with a brightness-percentage fallback — the classic prudynt/raptor-style approach. |
| `time` | The local wall clock against a fixed `[time_night_start, time_day_start)` window, which may wrap past midnight. |
| `sun` | Today's real sunrise/sunset for `sun_latitude`/`sun_longitude`, each edge shiftable by an offset in minutes. |

`daynight.enabled=0` (manual mode) makes the thread keep sampling — so
status stays live — but forces nothing in **any** of the three modes.

### `sensor` mode: gain thresholds and the adaptive night baseline

Gain and brightness are **inversely** related: high gain means a dark
scene. The preferred metric is `hal_isp_total_gain()` (a direct IMP call,
IMP's `[24.8]` linear scale, `256 = 1×`); when unavailable (T40/T41, host
sim, or the ISP not yet initialized), a fallback parses
`daynight.isp_path` (default `/proc/jz/isp/isp-m0`) for integration-time
and gain fields and derives an equivalent brightness percentage, mirroring
thingino's `daynightd` semantics.

- From day: switch to night when gain **exceeds**
  `daynight.total_gain_night_threshold` (default 4096 = 16x the [24.8] gain floor).
- From night: switch to day when gain **drops below**
  `daynight.total_gain_day_threshold` (default 768 = 3x the gain floor), *or* below an
  **adaptive** baseline if one has been sampled: after
  `daynight.baseline_delay_s` (default 30) of being in night — enough
  time for IR LEDs to settle — the current gain is captured once as the
  night baseline, and the day trigger becomes
  `night_baseline × daynight.day_gain_pct ÷ 100` (default 60%) instead of
  the fixed threshold. This makes the night→day trigger robust to
  IR-illumination variance across different scenes rather than relying on
  one fixed number for every camera. `day_gain_pct=0` disables the
  adaptive baseline entirely.

  Three hardenings (added 2026-08-02 after two real stuck-in-night
  incidents in one evening — a basement whose single utility light only
  pushed gain to 65% of a cleanly-sampled baseline, and a kids' room
  whose baseline was sampled mid-lighting-transition; revised 2026-08-03
  after the first version turned AGC noise into overnight probe flapping,
  and again 2026-08-04 for probe economy — see the arming margin and
  failure ratchet on the brightening probe below, and the reconfirm
  backoff and passive-evidence skip further down):
  - the computed trigger is **floored** at `total_gain_day_threshold` —
    the adaptive bar can never be stricter than the calibrated
    "definitely day" level;
  - the baseline and the probe comparison run on a **night-only smoothed
    gain** (EMA), and the baseline **drifts slowly toward it in both
    directions** — an unrepresentative sample (taken mid-transition, or
    off a post-revert AE still settling) self-corrects within minutes,
    and gain noise centers instead of ratcheting the baseline to its
    envelope maximum;
  - a **sustained-brightening probe**: smoothed gain holding below the
    halfway point between `day_gain_pct`% and 100% of the baseline for
    `DN_BRIGHTEN_CONFIRM_MS` (30 s; a real light came on, but not enough
    to cross the strict bar) fires the same day-pipeline probe as the
    periodic reconfirm below. It arms only on a fresh above-bar→below-bar edge, so a failed
    probe cannot re-fire on unchanged darkness — the baseline must first
    re-converge and the scene newly brighten. For a few seconds after
    any probe, the day→night revert additionally waits for a *stable*
    day-pipeline reading, so the AE convergence ramp right after the
    pipeline switch cannot kill a legitimate probe (a genuinely dark
    room rails at max gain, which is stable at once, so its correct
    revert is barely delayed). Two further gates were added 2026-08-04
    after the pre-dawn probe volley (fleet logs all 11 cameras
    2026-08-03/04: on a slow dawn ramp the edge-arm alone still let the
    probe re-fire every 10–40 min all the way to sunrise, because each
    failed probe re-sampled a *lower* baseline and the continuously
    declining gain kept re-crossing the freshly-lowered bar — some holds
    starting a tangent 0.2% under it):
    - an **arming margin** (`DN_BRIGHTEN_MARGIN`, 0.97): the hold starts
      only when smoothed gain is clearly *below* the bar, never on a
      graze that a tick of noise put fractionally under it;
    - a **failure ratchet**: after a probe fails, the *next* brightening
      hold must additionally undercut `day_gain_pct`% of the gain level
      that just failed — i.e. a whole further trigger-worth of genuinely
      new brightening, not just re-crossing a drifted bar. A slow ramp
      therefore gets at most a couple of well-spaced probes across the
      night instead of a volley, while a real light-on step (a 20–35%
      gain drop) still clears the ratchet immediately. The ratchet is
      latched on a failed probe and cleared on any genuine transition.

  The baseline currently in effect and the resulting trigger are
  reported read-only as `night_baseline` / `day_trigger` in the
  `GET /control` `"daynight"` object (`-1` = none / not in night).
- The gap between the two fixed thresholds (768..4096 by default) **is**
  the hysteresis dead-zone for this mode — no separate averaging is
  applied.
- **Dead-zone adoption** (added 2026-08-03 after a live incident): a
  fresh start whose reading sits *inside* the dead-zone used to stay
  undecided forever — silently keeping the stale persisted mode with no
  self-healing, because both reconfirm probes only run once a mode is
  established (a T31 restarted in broad daylight with a stale night
  config and a mid-band gain of 731 rendered night video indefinitely).
  Now, once the boot-settle window is over and the reading still cannot
  decide, the thread adopts the persisted `image.running_mode` as its
  mode (the ISP is already running it) and arms the normal triggers. An
  adopted *night* is a guess, so its first day-pipeline verify probe
  fires after `min(night_reconfirm_s, 300 s)` — and once even when the
  periodic reconfirm is disabled.
- **Verifying an unverified day** (symmetric side added 2026-08-12,
  "still brightening" extension added 2026-08-14). An adopted *day* — and,
  since the same release, a day a reconfirm probe *landed* on with a
  dead-zone reading — is verified without any IR-cut click: day-pipeline
  gain is the trustworthy metric already (it is what every night probe
  switches *to*), so the deadline is settled by simply re-reading it. It
  is **confirmed** when the reading would have decided day unaided from
  `DN_UNKNOWN`, and otherwise the guess falls back to night through the
  ordinary switch path — night being the recoverable side, since a wrong
  night self-corrects within one probe cycle and a wrong day does not.

  That revert judges a *level* at one instant, which cannot distinguish
  the scene it exists for (a camera booted after dark on a stale
  persisted day, rendering black) from a scene in free fall *through* the
  dead-zone toward daylight — and dawn is the second one. So the revert
  now waits for the improvement to **stop**: if the metric has moved at
  least `DN_DAY_VERIFY_FALL` (10%) better than the reading that armed the
  deadline, the deadline is re-armed for another interval and re-anchored
  on the new reading (`"day still unconfirmed but brightening: gain N is
  P% of the M that armed the deadline - extending Ts (#k)"`).

  This can only ever *delay* a revert — it never causes a switch and
  costs zero IR-cut clicks. A darkening scene cannot buy an extension
  (improvement is required), and a genuinely dark one rails above
  `total_gain_night_threshold`, which is tested first and reverts within
  the ordinary hysteresis whatever the deadline says. The anchor ratchets
  down on every extension, so gain noise cannot sustain a chain of them.
  And the rule is *self-terminating* rather than merely bounded: each
  extension moves the metric ≥10% closer to the day threshold, so from
  the top of the dead-zone at most ~22 are possible at the default
  thresholds before day is simply confirmed — `DN_DAY_VERIFY_EXT_MAX` is
  a seatbelt above that bound, not the operative limit.

  Live case it fixes (cam-vorne, T23/SC2336, 2026-08-14): four probes
  down the dawn ramp read day-pipeline gain 9024 → 4813 → 2425 → 708, the
  last two landing in the dead-zone and reverting five minutes later at
  1436 and 452 — each a 36–41% *fall* since the reading that armed the
  deadline. Because an unverified-day revert is also accounted as a
  failed probe, each one doubled the reconfirm backoff and latched the
  brightening ratchet lower, the second at 315 — putting its bar (189)
  *below the sensor's own night-pipeline gain floor* (~256 = 1×), where
  no reading can ever satisfy it. With the brightening path dead and the
  backoff at its ×4 cap, the next periodic reconfirm was 4 h out and the
  camera rendered IR-mode video in daylight from 06:20 until a manual
  restart at 08:07 — a restart that read *day* at once off the very same
  gain (257). See [Day-Night Design Notes](Day-Night-Design-Notes.md) for
  why that "a cold start would decide differently" property is the
  invariant this subsystem keeps violating, and what to do about it.

**Gain read through the night/IR pipeline is not the same metric as
gain read through the day pipeline.** IR-cut engagement changes the
optical path, and the ISP's night tuning table can target a different AE
point than the day table — reproduced twice on real T31s (bright room,
right after a firmware reflash+reboot): the camera latched into night on
an AE-convergence transient (see below), sampled its adaptive baseline
from that same bogus reading, and then sat there indefinitely because
live night-path gain (600–976) never dropped below the resulting
day-trigger (387) even though the room was genuinely daylight the whole
time — forcing `image.running_mode=0` manually made gain immediately
settle at a sane 365–420. Because of this, no amount of night-path
threshold tuning can recover from a bad initial trigger; see "Self-healing
periodic reconfirm" below for how timps guards against it.

If neither gain nor brightness can be read at all (proc file missing —
host sim, or a non-Ingenic environment), `sensor`-mode's decision cycle is
simply skipped that sample (status still updates); `time`/`sun` modes are
unaffected since they don't need the ISP.

### Override modes (`time` / `sun`)

Useful when the sensor's own exposure reading isn't the right signal — an
IR-blocked lens, a scene that's naturally dark or bright regardless of
time of day, or simply wanting the schedule to match a person's routine
rather than the camera's AE convergence.

- **`time`**: `daynight.time_night_start`/`time_day_start` are local
  `"HH:MM"` strings; the night window may wrap past midnight (e.g. night
  starts 20:00, day starts 06:30). Either edge left empty/invalid means
  no forcing.
- **`sun`**: a pure-math, no-external-library sunrise/sunset calculation
  (a standard low-precision NOAA/Meeus-style formula) for
  `daynight.sun_latitude`/`sun_longitude`, each edge shiftable by
  `sun_sunrise_offset_min`/`sun_sunset_offset_min` (minutes, may be
  negative). Polar latitudes where the sun genuinely doesn't rise or set
  that day resolve to permanent day or permanent night rather than
  propagating an invalid time; `GET /control`'s computed
  `sun_computed_sunrise`/`sunset` report `"--:--"` in that case so a
  WebUI can sanity-check a configured lat/long before trusting it.

```sh
curl -X POST http://127.0.0.1:8880/control -d '{
  "daynight": {"mode":"sun", "sun_latitude":52.52, "sun_longitude":13.40,
               "sun_sunset_offset_min":-30}
}'
curl http://127.0.0.1:8880/control | jq .daynight
```

## Anti-flapping guards

Three gates, all timed off `CLOCK_MONOTONIC` (deliberately not wall-clock
time, since an NTP step shortly after boot — typical for these cameras —
could otherwise make a wall-clock delta negative or skip past a timer),
must all pass before a switch is actually issued:

1. **Cold-start settle** (`daynight.boot_settle_s`, default 5s floor, from
   thread start/re-enable): ignores the AE convergence transient (observed
   gain spikes of 15000–20000 before AE settles) so every boot doesn't
   immediately commit to "night" regardless of actual light. A flat delay
   turned out to be insufficient after a firmware reflash — reproduced
   twice where AE genuinely took ~90s to converge, well past the old fixed
   5s window — so in `sensor` mode the wait also extends **past** the
   floor, up to `daynight.boot_settle_max_s` (default 120s) as a hard cap,
   until `daynight.boot_stable_pct`% (default 20%) of consecutive readings
   agree gain has actually stopped moving. `boot_stable_pct=0` reverts to
   the flat floor-only wait.
2. **Dwell** (`daynight.transition_s`, default 5s): minimum time since the
   last switch before another one is allowed.
3. **Pre-switch hysteresis** (~5s, hardcoded, distinct from the config
   `hysteresis` float used only by the brightness fallback): a candidate
   target must hold *continuously* for this long before the mode is
   actually applied. This is the primary fix for a "stuck mode" failure
   class: if `SetISPRunningMode` is issued while the ISP's own AE gain is
   still mid-ramp exactly at the day/night crossover, the driver can
   silently drop it — the API call reports success, and the config/proc
   file both claim the new mode, but the ISP pipeline stays in the wrong
   color mode (grey in daylight, or IR-tinted magenta at night) for
   hours. Waiting out a stable candidate before committing avoids issuing
   the change during that unstable window in the first place.

As defense in depth, after a successful switch the mode is **re-asserted**
twice more (roughly 8s apart, over a ~16s window), each time re-reading
the *current* `image.running_mode` (so a manual override during that
window is respected) — kept because the stuck-mode failure couldn't be
reliably reproduced on a bench, so the pre-switch hysteresis alone isn't
*proven* sufficient on its own.

## Self-healing periodic reconfirm

The three anti-flap guards above stop a *bad reading* from causing a
switch, but they can't fix a switch that already happened for the wrong
reason — and as the sensor-mode section above describes, once
`sensor` mode is latched into night on a bogus trigger, night-path gain
readings don't reliably cross back over any threshold, adaptive or fixed,
because that gain isn't measuring the same thing day-path gain measures.

So, only in `sensor` mode, after `daynight.night_reconfirm_s` (default
3600s = 1h, `0` disables) of *continuous* night dwell, timps forces a real
probe: it runs the day switch exactly as if a normal night→day decision
had fired (same `dn_switch()`, same post-switch re-assert, baseline reset),
then lets the **normal** day→night hysteresis re-evaluate from a true
day-pipeline reading on the next ticks. If the scene is genuinely still
dark, gain will cross back above `total_gain_night_threshold` and the
usual dwell+hysteresis window (~10–15s) flips it right back to night; if
it isn't, the camera simply stays in day — which is exactly what manually
forcing `image.running_mode=0` does today, just automatic. Each probe is
logged distinctly (`"switching to day (periodic reconfirm probe): ..."`)
so it's not confused with a real dusk/dawn transition.

The default interval (1h) trades a few extra IR-cut relay cycles per night
(roughly one per hour of darkness) for bounding how long a false latch can
persist — worst case, a stuck-in-night camera self-corrects within one
`night_reconfirm_s` window instead of staying wrong until someone notices
and intervenes manually.

**Exponential backoff on failed probes (added 2026-08-04).** Every probe
switch is *user-visible* — the board script clunks the IR-cut relay, kills
the IR LEDs, and the stream shows ~7–9s of dark colour video before the
revert. On a genuinely dark, unchanging night the hourly probe therefore
produced 8–12 of these flips per camera per night (fleet logs all 11
cameras 2026-08-03/04, reported as "periodische Tag/Nacht-Umschaltungen")
while learning nothing new each time. So a probe that *fails* — reverts to
night within 30s (`DN_PROBE_FAIL_WINDOW_MS`) of switching — now doubles the
periodic interval for the next one: ×1→×2→×4 (`DN_PROBE_BACKOFF_MAX`),
bounded by `max(night_reconfirm_s, DN_PROBE_BACKOFF_CAP_S=4h)`. A camera
that is really dark all night thus still gets its first-hour self-healing
probe but then backs off to a handful of probes instead of one every hour;
any genuine transition (or a probe that sticks in day) resets the
multiplier to 1. The failed-probe log line reads
`"probe confirmed genuine night (backoff x2, brighten ratchet < N)"`.

*Trend suspension (added 2026-08-15).* The backoff rests on a premise — "the
darkness this probe just measured is confirmed" — that it used to hold onto
regardless of what the scene did afterwards. It is now suspended for as long
as the measurement contradicts it: while the smoothed gain sits below 97%
(`DN_BRIGHTEN_MARGIN`) of `probe_fail_smooth` — the gain frozen at the instant
a physical probe last found genuine night — the multiplier is ignored and the
deadline falls back to one plain `night_reconfirm_s` after it was armed. It
can only ever pull a deadline *in*, never past it and never earlier than a
plain reconfirm interval, so it cannot buy an extra probe; and it costs zero
extra clicks in the case the backoff exists for, because an unchanging dark
scene sits at or above its own `probe_fail_smooth` and the test is simply
false. Logged once per suspension (on the edge, not per tick):
`"gain N is below 97% of the last failed probe's level M — the scene is
measurably brighter than the darkness the x4 backoff was granted for,
suspending it (reconfirm due 900s after arming, not 3600s)"`. Before this, a
dawn ramp could hold the ×4 cap the whole way down — the mechanism behind the
four-hour cam-vorne window of 2026-08-14.

**Passive-evidence skip (added 2026-08-04).** Backoff cut how *often* the
periodic probe fires, but each firing still drives the physical IR-cut relay —
an audible click. On a camera in genuinely unchanging darkness (cam-wyze,
closet, reported as "das klacken der IR blende nervt … nachts andauernd") that
click accomplishes nothing: the passive night gain never moved, so a probe can
only confirm what the gain already showed. So a due periodic probe is now
**skipped entirely** — no `dn_switch`, no click — when there is no passive
reason to suspect the state changed: if a night baseline and smoothed gain are
available and the smoothed gain is still `≥ DN_BRIGHTEN_MARGIN` of the same
probe bar the sustained-brightening hold uses (i.e. solidly deep in night,
nowhere near the day trigger), the probe silently re-arms on its backoff
schedule instead of firing. The skip log line reads `"periodic reconfirm due
but gain N still deep in night (bar M, baseline B) - skipping IR-cut probe,
re-arm in Ts (… since last physical probe, force at Us)"`.

This does *not* weaken self-healing. A *false* night latch — actually daytime
behind an engaged IR pipeline — reads *low* gain, which is precisely the
evidence that makes the probe fire; only a genuinely-dark scene, where the
probe could do nothing but fail, is skipped. Two guarantees keep the safety net
intact: the **first** probe after each night entry always fires
(`last_phys_probe_ms == 0`), so a stuck-forever mode is still caught within the
first interval as before; and a **`daynight.probe_max_skip_s` outer bound**
(default 12h) forces a physical probe regardless of gain once that long has
passed since the last *actual* physical probe — the trust-nothing double-check
for a permanently-flat reading that gain evidence alone can never clear. Net
effect under permanent darkness: at most ~2 physical clicks per day, versus up
to 6/day at the 4h backoff cap before this.

Made configurable 2026-08-05 (was a compile-time-only `DN_PROBE_MAX_SKIP_S`
constant): settable live via `/control`, range 3600–604800s. Deliberately
floored at 1h — a POST below the floor clamps to it rather than accepting it —
because this is the safety net for the whole passive-evidence-skip mechanism
above, not a knob meant to switch it off; raise it if even the reduced click
rate is still too frequent for a particular camera.

**Ratchet anchor (added 2026-08-13, live incident — Schuppen T31/SC2336).**
The "false night latch reads low gain, so the skip gate would fire" argument
above has one hole: `night_baseline` is not a fixed reference, it drifts
toward the smoothed night gain every tick (see baseline drift above) —
*including* the very gain the skip gate is judging. Given hours of continuous
(wrongly-classified) night dwell, the baseline fully converges to whatever the
current gain reads, even a genuinely day-level one, dragging the probe bar
down in lockstep and keeping "solidly deep in night" true forever. Live case:
a probe failed at gain 284 (latching the brighten ratchet), and over the next
2.5h the baseline chased the actual — by then clearly daytime — gain down to
257–266; the skip gate kept re-arming the whole time because bar and gain
drifted down together, and only a manual service restart (which replants the
baseline from scratch) recovered it before `probe_max_skip_s` would eventually
have forced a probe anyway.

The fix reuses `probe_fail_smooth` — the smoothed gain snapshotted at the
*moment* a probe last failed — as a second, drift-immune anchor whenever the
brighten ratchet is outstanding: the skip is now also revoked once gain has
moved measurably (`DN_BRIGHTEN_MARGIN`) brighter than that frozen snapshot,
regardless of what the baseline has since drifted to. A camera that is
genuinely flat/unchanging after a failed probe (the closet case this gate was
built for) still sits at or above its own `probe_fail_smooth` and keeps
skipping exactly as before; only real further brightening past the
last-checked point loses the skip. This does not touch the brightening-hold's
own ratchet check (still baseline-relative, unchanged) — only the periodic
reconfirm's decision to skip a *due* probe. The log line
`"ratchet anchor overrides baseline evidence: gain N has drifted below M% of
the last failed probe's level P (baseline-relative bar was B) - forcing
periodic reconfirm"` marks the moment this anchor is what forces the probe.

**Oscillation breaker (added 2026-08-04).** The backoff/margin/ratchet above
are all about *probe* economy — they cannot see a loop that happens on the
PRIMARY threshold crossings themselves. A camera mounted very close (~30 cm)
to a reflective object hits exactly such a loop: in night the IR LED turns on,
reflects intensely off the close object, the AGC gain reads *very low* (looks
"bright"), so a genuine night→day crossing fires; the IR LED then switches off
and the colour pipeline takes over, but the scene is actually still dark, so
the gain rails straight back up and a genuine day→night crossing fires, turning
the IR LED on again — and round it goes, clunking the IR-cut every few seconds
indefinitely. This is a general safety net for *any* fast day/night
oscillation, not IR-specific detection: the thread counts **genuine
(non-probe) mode flips** in a rolling `DN_OSC_WINDOW_MS` (60s) window, and once
`DN_OSC_FLIPS` (3) of them fall inside it, it logs a single warning —

```
possible IR-reflection feedback loop detected (3 day/night flips in 14s) -
camera may be mounted too close to a reflective object; freezing in night
mode for 600s
```

— and **freezes** the last-decided mode for `DN_OSC_FREEZE_MS` (10 min),
suppressing both threshold switches *and* probes so the loop cannot continue.
After the cooldown it resumes normally and, if the physical condition still
persists, simply re-detects and re-freezes (at most a couple of flips per
cooldown instead of one every few seconds). The right permanent fix is to
re-aim or shield the camera, or set a fixed mode / time-or-sun schedule; the
breaker just stops the fleet-visible flapping in the meantime.

Crucially, a reconfirm or brightening **probe** that switches to day and then
reverts on failure is a normal, intentional *2-flip* event under the
probe-economy design above (at most one such pair per backoff-scheduled probe),
and its flips are **deliberately never counted** — only the genuine main-switch
threshold crossings feed the oscillation counter. A probe cycle therefore
contributes zero to the count and can never, by itself, trip the breaker;
boot-time dead-zone adoption and its one-shot verify probe are likewise
probe-driven and excluded. `DN_OSC_WINDOW_MS` / `DN_OSC_FLIPS` /
`DN_OSC_FREEZE_MS` are compile-time `#ifndef`-overridable constants, like the
other `DN_*` tunables; there is no runtime config key.

## The ISP running-mode latch quirk and `fs_kick_running_mode()`

Even with all of the above working correctly, one more hardware quirk
remained and was root-caused on a real deployed camera (Galayou Y4,
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

## Live control and status

| Action | Effect |
| --- | --- |
| `POST /control {"daynight":{"enabled":false}}` | Switches to manual mode; the thread keeps sampling for status but stops forcing switches. |
| `POST /control {"daynight":{"mode":"time",...}}` | Switches decision source; validated against `sensor`/`time`/`sun` before being applied. |
| `POST /control {"image":{"running_mode":1}}` | Manually forces the ISP mode (what the board switch script itself calls). Re-posting the *same* value still re-drives the ISP (see [HTTP /control API Reference](HTTP-Control-API.md)) — necessary precisely because of the latch quirk above. |
| `GET /control` `"daynight"` object | Read-only status: `enabled`, `mode` (0 day/1 night), `brightness` (%), `total_gain` (IMP `[24.8]` linear), `ae_luma`, `night_baseline`/`day_trigger` (the adaptive baseline and effective night→day trigger in effect, `-1` = none), the configured thresholds, and — always, regardless of active mode — today's computed `sun_computed_sunrise`/`sun_computed_sunset`. |
| `GET /events?stream=daynight` | Pushes the same status object whenever mode flips, brightness moves ≥1%, or gain moves ≥5% relative (or ≥8 absolute near zero) — see [HTTP /control API Reference](HTTP-Control-API.md#event-types). |

See [Configuration Reference](Configuration-Reference.md#daynight--automatic-daynight)
for the full key table (including which fields are settable via
`/control` vs. config-file-only).
