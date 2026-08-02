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
  `daynight.total_gain_night_threshold` (default 3000).
- From night: switch to day when gain **drops below**
  `daynight.total_gain_day_threshold` (default 300), *or* below an
  **adaptive** baseline if one has been sampled: after
  `daynight.baseline_delay_s` (default 30) of being in night — enough
  time for IR LEDs to settle — the current gain is captured once as the
  night baseline, and the day trigger becomes
  `night_baseline × daynight.day_gain_pct ÷ 100` (default 60%) instead of
  the fixed threshold. This makes the night→day trigger robust to
  IR-illumination variance across different scenes rather than relying on
  one fixed number for every camera. `day_gain_pct=0` disables the
  adaptive baseline entirely.
- The gap between the two fixed thresholds (300..3000 by default) **is**
  the hysteresis dead-zone for this mode — no separate averaging is
  applied.

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

1. **Cold-start settle** (~5s from thread start/re-enable): ignores the
   AE convergence transient (observed gain spikes of 15000–20000 before
   AE settles) so every boot doesn't immediately commit to "night"
   regardless of actual light.
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
| `GET /control` `"daynight"` object | Read-only status: `enabled`, `mode` (0 day/1 night), `brightness` (%), `total_gain` (IMP `[24.8]` linear), `ae_luma`, the configured thresholds, and — always, regardless of active mode — today's computed `sun_computed_sunrise`/`sun_computed_sunset`. |
| `GET /events?stream=daynight` | Pushes the same status object whenever mode flips, brightness moves ≥1%, or gain moves ≥5% relative (or ≥8 absolute near zero) — see [HTTP /control API Reference](HTTP-Control-API.md#event-types). |

See [Configuration Reference](Configuration-Reference.md#daynight--automatic-daynight)
for the full key table (including which fields are settable via
`/control` vs. config-file-only).
