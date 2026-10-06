# Idle-Dark (`daynight.ir_idle_off`) - 2026-10

**Status:** stage 1 of 3 landed (plumbing, config/status, pure logic). **No
behaviour change yet:** nothing switches the illuminator. Design: scratch
document `dn-idle-ir-design.md` (recommendation "Idle-Dark", commit plan
C0..C9). C0 (`timps-irprobe` verbs `idle`/`state`, `daynight.controls.*`) is in
the firmware tree (`cfa79f9f4`, `scripts/test_irprobe.sh`).

The idea in one paragraph: at night, once nothing has needed a picture for
`daynight.ir_idle_linger_s`, switch only the IR illuminator off (IR-cut and ISP
stay in night). With the FrameSource keepalive the AE keeps measuring ambient
light ("noir") while idle, so the first consumer is answered from that reading
- LED on (night) or day (clearly bright noir) - without the 8 s probe.

Measured since the design (peer session, T23, OpenIMP/open-tx-isp, keepalive,
no consumer): ISP IRQs keep running and the AE keeps regulating, settling
~1-6 s after a light change; the first snapshot right after the dark was
overexposed, the next ~2 s later correct. That is design gate M-A, positive on
that SoC. `DN_IR_WAKE_SETTLE_MS` stays at the design's start value 3500.

## Idle-Dark stage 1 status

Commits (timps, local, not pushed):

| Commit | Design | What |
| --- | --- | --- |
| `04a0794` | C1 | `hub_image_subs()` (video + JPEG subscribers, no audio), `hal_image_pinned()` (motion FS pin, read lock-free; a configured `jpeg.snapshot_path`; sim: count from the file in `TIMPS_SIM_WATCH`), `ms_stopgate_kick()`/`ms_stopgate_wait_kick()`, `daynight_watch_kick()` from `act_trampoline()` for video/JPEG sources |
| `3d57364` | C3 | `src/dn_idle.h` (pure decision, injected clock) + `scripts/test_dn_idle.c`, `make test-dn-idle` |
| `d6ef9d9` | C2 | `daynight.ir_idle_off` (0..1, default 0) and `daynight.ir_idle_linger_s` (0..3600, default 60), both `F_CTRL`/live; status `daynight.ir_idle` in `GET /control` and `/events`; `caps.daynight`; WARN once without keepalive; docs, CHANGELOG, `timps.conf.example`, `test_config` cases |

What is wired and what is not:

* **Wired:** consumer level and kick reach the day/night thread every tick
  (`dn_idle_status_tick()`), which keeps the linger and publishes
  `{cfg, available, reason, dark, watched, linger_left_s}`. `/events` pushes a
  `daynight` event when `cfg`, `available` or `dark` change.
* **Plumbing only:** `daynight_watch_kick()` sets a flag; the gate kick fires
  only when `g_dni_armed` is set, which nothing does yet, and `dn_sleep()`
  still uses the plain `ms_stopgate_wait()`, which ignores kicks. `dni_step()`
  is not called by the automaton; `dark` is always 0. `g_dni_hook` stays
  `DNI_HOOK_OK` (C4 sets it from the `idle` verb's exit code).
* Decisions are therefore unchanged by construction; the thread only reads
  hub/HAL counters and computes a status.

Behaviour of `dni_step()` (what C4 will call), tested in `test_dn_idle.c`:

* Gate (`dni_gate()`): `ir_idle_off && enabled` (else `disabled`), keepalive
  (`keepalive`), hook state (`no_illuminator` when `irprobe_cmd` is empty or
  the illuminator was retired, `hook` for a hook without `idle`,
  `white_light`).
* Watched = consumers > 0 **or a kick since the last step** (a snapshot can
  come and go between two ticks); each watched step sets
  `watched_until = now + linger`.
* Entry (`DNI_LED_OFF`, reason `idle`): night, unwatched, linger over,
  `booted`, no probe/verify/enforce in flight, `DN_TRANSITION_S` dwell. At boot
  `watched_until` is 0, so an unwatched boot goes dark right after the dwell.
* While dark: re-assert `idle` at +8 s and +16 s (the board script lights in
  the background), then every `DN_IR_IDLE_REASSERT_S` (600 s).
* First consumer: `DNI_LED_ON` at once, `ready_at = now + 3500 ms`; reason
  `wake` or `wake (noir unsettled)` (dark < `DN_PROBE_SETTLE_S`, not
  `stable`, not `fresh`). `DNI_WAKE_DAY` instead only when the caller allows
  the noir day rule (`day_rule`, meant for C5), noir is valid, `s < day_gain`
  and the `noir_cost` veto is not active; the LED then stays off and the
  caller confirms with `state` and switches day, or lights it itself.
* Hand-back: gate closes while dark (POST `ir_idle_off=0`, `enabled=0`)
  → `DNI_LED_ON`; `dni_handback()` for stop; a day switch clears `dark`
  silently; `dni_note_switch()` after every `switch_cmd` (the script drives
  the LED), re-entry then after the dwell.
* Restart: state starts zeroed (not dark), so no blind LED action; the boot
  assertion of the mode relights, entry repeats after boot.

## Tests run for stage 1

* `make test-dn-idle`: 79 checks (linger expiry, linger 0, flapping 10 s / 120 s
  polls, kick between ticks, permanent pin, every gate reason, boot/dwell/busy,
  hand-backs, wake verdicts incl. unsettled/veto/frozen AE/twilight band,
  re-assert schedule, switch while idle, boot and restart, stop-gate kick
  semantics).
* All existing host tests, `make test-auth`, `scripts/test_irprobe.sh`,
  `scripts/test_s95timps_preflight.sh`: pass.
* dn-replay corpus (sim at scale 30 and 60, each scenario with its own scale):
  38/38 at `59c0e68` and 38/38 at `d6ef9d9`.
* Sim, by hand: POST of both keys (clamped 5→1, 99999→3600, persisted),
  `reason:"keepalive"` + one WARN without keepalive, with
  `general.fs_keepalive=1` `available:1`, `watched`/`linger_left_s` following
  `TIMPS_SIM_WATCH`, a `/events` push on each `cfg`/`available` change.
* Cross-builds warning-free; stripped size and `size` dec (text+data+bss)
  before → after:

| Flavour | stripped | dec |
| --- | --- | --- |
| T23 OpenIMP | 405988 → 406024 | 594782 → 597267 (+2485) |
| T41 OpenIMP | 339968 → 344116 | 510942 → 513483 (+2541) |
| T21 OpenIMP | 393628 → 393664 | 572511 → 575012 (+2501) |
| T21 vendor | 389508 → 389544 | 569242 → 571743 (+2501) |
| T31 vendor | 389552 → 393684 | 572058 → 574559 (+2501) |

  The stripped jumps of ~4 KiB are page alignment. On T23 the largest pieces
  are the status tick inlined into `dn_thread` (+704 B), the stop-gate helper
  (+344, `ms_stopgate_wait` -232), the status getter, `hub_image_subs`,
  `hal_image_pinned`, the kick, plus the JSON/WARN strings.

## Deviations from the design

* Commit order C1, C3, C2: the C2 status uses `dni_gate()` from C3.
* `hal_image_pinned()` reads the motion pin with an atomic load instead of
  under `g_motion_mtx`: that mutex is held across a whole IVS rebuild and
  would block the day/night thread. The pin's stores became atomic.
* `reason` `disabled` covers both `ir_idle_off = 0` and `daynight.enabled = 0`
  (`cfg` tells them apart).
* The status also carries `ir_idle_off`/`ir_idle_linger_s` at the top level
  of the `daynight` object, like the other settings the photosensing page
  binds to; `caps.daynight = {available, ir_idle}` is new.
* The `/events` scratch buffer grew from 1024 to 1280 bytes (the size of the
  SSE frame): the `daynight` object is ~0.9 KB now and ~1 KB worst case.
* The `DN_IR_*` constants live in `src/dn_idle.h` next to the logic.
* Re-assert after a `switch_cmd night` while idle: re-entry after the 5 s
  dwell, then +8/+16 s, instead of the design's +2/+8/+16 s.

## Open for stage 2 (C4-C7)

* **C4:** call `dni_step()` from the automaton; idle entry as a silent probe
  without relight through the `idle` verb (exit 1 → `DNI_HOOK_OLD`, 3 →
  `DNI_HOOK_WHITE`); wake: arm `g_dni_armed`, switch `dn_sleep()` to
  `ms_stopgate_wait_kick()` and run only the idle part (no sample) on a kick
  wake; LED on via `on`, reset `s/stable_n/ref`, trend reseed, heartbeat
  credit; hand-back on disable, schedule mode change, stop and POST; LED
  re-assert; the freshness input (gain or integration time moved since LED
  off); `dni_note_switch()` after every `switch_cmd`.
* **C5:** noir day rule while idle (`s < day_gain` for `probe_confirm_s`) and
  on wake (`day_rule = 1` in auto mode), `state` cross-check before a day
  verdict, `noir_cost` learning and veto, probes escalate to audible while
  dark.
* **C6:** `hub_grab_jpeg()` warm-up hold until `ready_at`
  (`daynight_image_ready_at_us()`), hub test; optional IDR at `ready_at`.
* **C7:** dn-replay keys (`consumers` → `TIMPS_SIM_WATCH`, `switch_drives_led`,
  irprobe stub `idle`/`state`, `ir_hook_rc`), assertions and scenarios 39-54.

Stage 3 (C8): WebUI photosensing switch/linger field/status chip
(`config-photosensing.html`/`.js` in the firmware tree; `timps.webui.json`
needs nothing), wiki Day-Night section, `feature-list-de.md`.
