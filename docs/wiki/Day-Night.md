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
> redesign replaces them with four independent paths and one rate limit.
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

## Four paths, none of which can block another

| | Path | Signal | Covers | Cost |
| --- | --- | --- | --- | --- |
| **A** | day → night | day-pipeline `D` vs `night_gain`, held `day_confirm_s` | dusk, lights off | 1 click |
| **C** | spontaneous probe | *relative* fall of `D` below `probe_jump_pct`% of the night reference, held `probe_confirm_s` | **lights on**; indoor cameras; cameras with no location data — the path that carries most of a fleet | 2 clicks if it fails |
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

## Learning (`daynight.learn`, default off)

Each confirmed day records its lowest `D`; the median of the last 8 says how
bright this scene actually manages to get. With `learn=1` that median raises
the effective `day_gain` when the configured value turns out to be
unreachable — the failure that had three cameras stuck in night on
2026-08-16, each needing an SSH session to diagnose. Two rules keep it safe:

1. **It may only ever raise the threshold.** A too-generous `day_gain`
   produces a false day, which path A corrects within `day_confirm_s`. A
   too-strict one makes day unconfirmable, which is the failure with no
   bound.
2. **It is clamped below `night_gain/2`**, so the two thresholds cannot cross
   and start oscillating.

With `learn=0` the values are still collected and written to the log **once a
day**, so the numbers can be read off a running camera before deciding to
switch it on:

```
DAYNIGHT: learned: 6 day excursion(s) [812 790 845 1103 798 802], median 807,
          effective day_gain 768 vs configured 768 - not applied (daynight.learn=0)
```

`daynight.state_path` persists them across reboots when learning is on
(change-only, at most one write an hour, atomic rename — unlike `trace_path`
this is not a flash-wear concern). A state file that does not parse is
discarded silently.

Deliberately **not** persisted: the night reference (it is re-anchored in
30 s and a stale one would *disable* path C, violating rule 2) and the mode
(it is measured at boot, not believed).

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

## Live control and status

| Action | Effect |
| --- | --- |
| `POST /control {"daynight":{"enabled":false}}` | Switches to manual mode; the thread keeps sampling for status but stops forcing switches. |
| `POST /control {"daynight":{"mode":"schedule",...}}` | Switches decision source; validated against `auto`/`schedule` (and the legacy `sensor`/`time`/`sun` spellings) before being applied. |
| `POST /control {"image":{"running_mode":1}}` | Manually forces the ISP mode (what the board switch script itself calls). Re-posting the *same* value still re-drives the ISP (see [HTTP /control API Reference](HTTP-Control-API.md)) — necessary precisely because of the latch quirk above. |
| `GET /control` `"daynight"` object | Read-only status: `enabled`, `mode` (0 day/1 night), `brightness` (%), `total_gain` (IMP `[24.8]` linear), **`exposure`** (the index the decision actually runs on — plot this one when diagnosing), `ae_luma`, `night_baseline`/`day_trigger` (kept under their old key names, now carrying the proven night reference and the probe bar, `-1` outside night), the configured thresholds, and — always — today's computed `sun_computed_sunrise`/`sun_computed_sunset`. |
| `GET /events?stream=daynight` | Pushes the same status object whenever mode flips, brightness moves ≥1%, or gain moves ≥5% relative (or ≥8 absolute near zero) — see [HTTP /control API Reference](HTTP-Control-API.md#event-types). |

See [Configuration Reference](Configuration-Reference.md#daynight--automatic-daynight)
for the full key table (including which fields are settable via
`/control` vs. config-file-only).
