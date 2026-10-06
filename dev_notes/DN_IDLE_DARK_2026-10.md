# Idle-Dark (`daynight.ir_idle_off`) - 2026-10

**Status:** all three stages landed (C0-C8): Idle-Dark acts when
`daynight.ir_idle_off = 1` (default 0); WebUI card and docs are in (stage 3).
The default stays off until the measurements at the end are done. Design:
scratch document `dn-idle-ir-design.md` (recommendation "Idle-Dark", commit
plan C0..C9). C0 is in the firmware tree: `cfa79f9f4` (every illuminator
`daynight.controls` enables) and `67fbc42e9` (verbs `idle`/`state`, exit 3/4),
tested by `scripts/test_irprobe.sh`.

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

## Idle-Dark stage 2 status

Commits (local, not pushed):

| Repo | Commit | Design | What |
| --- | --- | --- | --- |
| firmware `ciao` | `67fbc42e9` | C0 | `timps-irprobe idle` (off, no watchdog, voids older ones; exit 3 with white light in the set, 4 when a switch failed and was rolled back), `state` (prints on/off, unreadable = on) |
| timps | `c0e929f` | C0 test | `scripts/test_irprobe.sh`: 98 checks (sh + busybox sh) |
| timps | `d0fc082` | C4 | entry, wake, hand-back, re-assert, freshness check, kickable sleep |
| timps | `4ba51bd` | C5 | noir day rule (idle and at a wake), `state` check, `noir_cost` |
| timps | `5e6b619` | C6 | `hub_grab_jpeg()` warm-up hold + `make test-hub-hold` |
| timps | `ef3c1a8` | C7 | dn-replay keys/assertions, sim fixture kick, scenarios 39-54 |

### Behaviour per path

* **Entry** (`dni_step` → `DNI_LED_OFF`, reason `idle`): night, auto mode,
  booted, no probe/verify/enforce in flight, 5 s dwell, linger over, a fresh
  lit reading on this tick. timps arms the kick, runs `<irprobe_cmd> idle`
  and treats the next 8 s as a silent probe whose verdict does **not**
  relight: `r >= 2` night (the reference becomes the noir level, heartbeat
  credit), `r <= 2` with reserve and no `filter_cost`/`noir_cost` veto → day
  (one click, nobody watches), anything inconclusive stays night and dark
  (no audible escalation). Exit 1/2 of `idle` → `reason = "hook"`, 3 →
  `"white_light"`, other failures twice → `"hook"`; reset on re-enable or a
  0→1 edge of the key.
* **Boot**: an unwatched boot whose silent measurement proves night calls
  `idle` instead of `on` (that also voids the probe's 60 s watchdog) and is
  the entry: LED off from the boot verdict.
* **While dark**: re-assert `idle` at +8/+16 s and every 600 s. Silent probes
  are impossible (they would divide noir by noir): heartbeat and operator
  probes go audible, with the noir level never used as the revert reference.
  With C5, paths C/T are quiet while the noir reading is fresh.
* **Wake** (kick from `act_trampoline` → gate kick → idle pass without a
  sample, or the level at a tick): `on` at once, `hub_set_image_ready_at(now +
  3500 ms)` (the trampoline already set a provisional `+4500 ms` inside
  `hub_subscribe`), an in-flight entry verdict is dropped, `s`/`stable_n`/
  `ref`/trend/sustained-min reset, reference re-anchored after 30 s, heartbeat
  credit when the noir reading was valid and dark (`> night_gain` or railed).
* **Hand-back**: gate closes while dark (POST `ir_idle_off = 0`,
  `daynight.enabled = 0`, `mode = schedule` - new reason `schedule`, keepalive
  gone, hook unusable) → `on`; stop → `on`; a failing re-assert → `on`; every
  `switch_cmd` clears `dark` (`dni_note_switch` in `dn_switch`).
* **Freshness**: the raw gain/exposure at LED-off is remembered; the AE counts
  as running once either changes. An entry verdict with an unmoved AE (and an
  unclipped lit reading) is refused: WARN once, LED on, no entry until the
  next switch. The noir day rule and the wake's day verdict need it too.
* **Noir day rule** (C5): fresh noir `< day_gain` held `probe_confirm_s`, not
  vetoed (`noir_cost > 0 && s * noir_cost > night_gain`), and `state` says
  off → `switch day ("idle ambient …")`. `state` on → INFO, re-assert,
  re-measure. Path A undoing such a day within `day_confirm_s + 18 s` learns
  `noir_cost = day / noir`. At a wake (`day_rule = 1`): valid noir below
  `day_gain`, not vetoed, `state` off → switch to day in an immediate full
  tick, LED stays off; otherwise LED on. A wake-day the next tick did not
  switch lights the LED.
* **Hold** (C6): `hub_grab_jpeg()` reads the ready instant after its
  subscribe and discards JPEGs until then (deadline extended by the hold,
  capped at `HUB_IMAGE_HOLD_MAX_MS` = 5000). RTSP/WebRTC/MJPEG/fMP4/DESCRIBE
  are never held. No IDR at `ready_at` (not trivial: the thread ticks every
  2 s).

### Tests run for stage 2

* `make test-dn-idle` 90 checks, `make test-hub-hold` 14, every other host
  test (`test-config`, `-fs-ref`, `-fs-rot`, `-daynight-read`,
  `-daynight-sun`, `-libimp-check`, `-clients`, `-fmp4`, `-fanqueue`,
  `-hub-pool`, `-hub-idr`, `-timelapse-prune`, `-record-ring`, `-stun`,
  `-srtp`, `-auth`), `scripts/test_irprobe.sh` 98,
  `scripts/test_s95timps_preflight.sh` 16: pass.
* dn-replay corpus, sims at scale 30 and 60, each scenario with its own:
  **54/54** (38 old + 16 new). The 38 old ones make the same switches and
  silent probes as at stage 1 (compared per scenario, timing masked).
* Mutation checks: wake without `on` → 7 scenarios fail; no freshness check →
  47 fails (false day at t=39 s, 4 clicks); no kick (tick-only wake) → 6
  fail on `led_on_within_s`; hold ignored → `test-hub-hold` 4 checks fail.
* Sim by hand (`http.enabled` in a scenario, POST on loopback):
  `ir_idle_off` 1→0 relights at once (`reason disabled`), 0→1 goes dark
  again, `mode = schedule` relights (`reason schedule`), `enabled = 0`
  relights.

* Cross-builds from `513f830` against `887054b`, warning-free; stripped size
  and `size` dec (text+data+bss), binaries + md5 in
  `scratch-builds/idle-ir-s2/`:

| Flavour | stripped | dec |
| --- | --- | --- |
| T23 OpenIMP | 406024 → 414228 | 597267 → 605077 (+7810) |
| T41 OpenIMP | 344116 → 352320 | 513483 → 521373 (+7890) |
| T21 OpenIMP | 393664 → 401868 | 575028 → 582806 (+7778) |
| T21 vendor | 389544 → 397748 | 571743 → 579537 (+7794) |
| T31 vendor | 393684 → 401888 | 574559 → 582353 (+7794) |

  More than the design's 2-4 KB: `dn_thread` +5.0 KB on T23 (entry verdict,
  noir rule, idle pass with its inputs and logs inlined), `dn_irhook` 468 B
  (stdout capture), `dn_idle_hook` 308, hub hold ~0.4 KB, the rest strings.

### Scenarios 39-54

39 night first consumer + external LED (state check), 40 unwatched dawn with
a halogen false day (`noir_cost` learnt, then a real dawn, consumer in day),
41 twilight band consumer + wake-day, 42/43 snapshot polling 10 s / 120 s,
44 pinned consumer, 45 unwatched boot (base 31), 46 wake 2-4 s after LED off,
47 frozen AE, 48 nothing switchable, 49 old hook, 50 white light, 51
consumer through dusk, 52 SIGKILL restart with the LED off, 53 linger
boundary, 54 keepalive off. Harness: `consumers` (`TIMPS_SIM_WATCH`; the sim
fixture kicks like a subscribe), `switch_drives_led`
(+`switch_led_delay_s`), `ir_hook_rc`, `led_external`, `restart_at`;
assertions `ir_state_at`, `max_led_toggles`, `led_on_within_s`,
`max_led_on_unwatched_s` (on `led.log`, written by both stubs).

### Deviations from the design (stage 2)

* Probes in the dark escalate to audible already in C4 (design: C5): without
  it C4 alone would run silent probes on noir/noir (r = 1, false day).
* The entry verdict never escalates to the audible probe (design: unclear or
  unknown reserve → audible, rationed): every re-entry after a switch would
  repeat it, i.e. a click pair every `probe_min_gap_s` on cameras without
  ceilings. Undecided stays night and dark; the noir rule and the heartbeat
  decide later.
* C5 also quiets paths C/T while the noir reading is fresh (not in the
  design): otherwise every unwatched dawn costs audible probe pairs before
  the noir rule fires, which the design's unwatched-dawn case (exactly one switch)
  excludes.
* The entry's ratio verdict also honours `noir_cost` (its dark reading IS a
  noir reading); one false day then teaches both rules.
* Idle-Dark only in auto mode; `mode = schedule` hands back (new reason
  `schedule`). The design allowed LED-only idle in schedule mode; the
  schedule branch abandons every silent probe each tick, so it would need a
  second entry path for a mode without measurement.
* `idle` exit 1 counts as "hook" (old hook: the exec'd `light` rejects the
  verb with 1; a board without lights answers 1 as well), not
  `no_illuminator`; `no_illuminator` stays for an empty `irprobe_cmd` or a
  retired illuminator.
* No IDR request at `ready_at` (optional in the design).
* Wake/entry/re-assert are LOGD; WARN only for unusable hook, frozen AE and
  missing keepalive.

### Risks

* Untested on hardware: AE settle after "IR on", hook latency, dusk/dawn
  behaviour of noir vs the day optics, dual-IR boards.
* A frozen AE is detected only at entry; one that stops later during a long
  idle keeps an old noir reading (the day rule then never fires, the
  heartbeat still does).
* At 850 nm the LED going out shows that nobody watches or records.
* Heartbeat probes in the dark are audible (4-12 h).
* Snapshot latency +3.5 s (max 5 s) after a wake; HA's default timeout is
  10 s.

### Open for stage 3 (C8) and later

* WebUI (`config-photosensing.html`/`.js` in the firmware tree): switch
  `daynight.ir_idle_off`, field `daynight.ir_idle_linger_s` (0..3600), status
  chip from `daynight.ir_idle` (`dark` → "IR off (idle)", `available = 0` →
  "unavailable: <reason>" with `disabled`, `schedule`, `keepalive`,
  `no_illuminator`, `white_light`, `hook`), hide the switch when
  `caps.daynight.ir_idle = 0`, warning text about the 850 nm tell. Wiki
  Day-Night section, `feature-list-de.md`.
* On-camera measurements before the default could ever change: dusk and dawn
  pairs (noir vs day optics, how often `noir_cost` has to learn), AE settle
  after "IR on" with and without consumer (sets `DN_IR_WAKE_SETTLE_MS`), hook
  latency of `timps-irprobe on/idle/state`, first-frame luma after a wake
  (hold length, IDR use), dual-IR boards (both LEDs off with `idle`), real
  poll cadences of HA/agent-adapter (linger default), LED power.

## Idle-Dark stage 3 status

Commits (local, not pushed):

| Repo | Commit | Design | What |
| --- | --- | --- | --- |
| firmware `ciao` | `e4411e7ff` | C8 | Day / Night page: card "IR light when idle" (switch `daynight.ir_idle_off`, field `daynight.ir_idle_linger_s` 0..3600, status chip, 850 nm note), `WEBUI-NOTES.md` |
| timps | `1500d4a` | C8 | `scripts/test_webui_idle.js`; wiki Day-Night section "Idle-Dark", Configuration-Reference rows, `feature-list-de.md`, `reference.md`/`troubleshooting.md` WebUI notes, CHANGELOG, this section |

### WebUI

* Lives in `package/timps/files/www/config-photosensing.html` and
  `a/config-photosensing.js` (right column of the Settings tab, below "On
  switch, toggle"). That page is timps-only: `timps.mk` installs it over
  thingino-daynightd's, prudynt/raptor images keep daynightd's page, which is
  untouched. No new libraries, no plugin manifest change (the page script is
  already listed in the page itself; `assemble_plugins.py` only adds the
  plugin script tags at target-finalize).
* Visibility: hidden unless the status has `ir_idle` (older timps: the two
  keys are then not POSTed either, it would reject them). `caps.daynight
  .ir_idle = 0` hides it too, **except while the key is on**: then the chip
  says "not available: no keepalive" and the switch can turn it off. This
  deviates from "hide when caps = 0" on purpose.
* Chip per state (pure `idleView()`): key off → "off"; `available = 0` →
  "not available: <short reason>" plus a line with the fix (`disabled`,
  `schedule`, `keepalive`, `no_illuminator`, `white_light`, `hook`, unknown
  reasons passed through); `dark` → "IR off (idle)"; day → "ready (day)";
  `watched` → "IR on · watched"; linger running → "IR on · off in N s"
  (counted down locally from the last `linger_left_s`, since `/events` only
  pushes on `available`/`dark`/`reason` changes); linger over → "IR on ·
  going dark".
* The page's own MJPEG preview (ch1) runs even on the Settings tab and is a
  picture consumer: with the page open the chip reads "watched". Stopping the
  preview on the Settings tab would fix that but touches `timps-preview.js`
  (watchdog/retry), left for later.

### Tests run for stage 3

* `node scripts/test_webui_idle.js <fw>/package/timps/files/www/a/config-photosensing.js
  [control.json ...]`: 46 checks (every reason, hide rules incl. caps 0 + key
  on, dark/day/watched/linger/age countdown, string and boolean values, the
  page's ids and the 0..3600 range); mutation checks (hide rule, age, dark)
  fail as expected. Fed with sim `GET /control` documents (keepalive 1/0,
  empty `irprobe_cmd`, `TIMPS_SIM_WATCH` 0/1): chips as expected.
* Sim POST with the page's payload shape: 10 accepted, linger 9999 clamped to
  3600 and reported in `applied`, persisted.
* `node --check`, HTML tag balance/duplicate ids (python `html.parser`);
  a static render of the card in four states at 390 px (headless Firefox,
  page CSS + Bootstrap 5.3.8): no overflow, the chip wraps inside the header.
* Not verifiable here: the real page against a camera (load, save, live
  `/events` updates, slider buttons, preview interplay).
* Every host test (`test-config`, `-dn-idle` 90, `-hub-hold` 14, `-roi`,
  `-daynight-read`, `-daynight-sun`, `-fs-ref`, `-fs-rot`, `-libimp-check`,
  `-clients`, `-fanqueue`, `-hub-pool`, `-hub-idr`, `-timelapse-prune`,
  `-record-ring`, `-stun`, `-srtp`, `-auth`) and `scripts/test_irprobe.sh` on
  the `ciao` hook (98): pass; `make sim` warning-free. No timps source
  changed in stage 3, so no binary size delta.
* Firmware ships the hook: `timps.mk` installs `files/timps-irprobe` to
  `/usr/bin/timps-irprobe`; `ciao` has `cfa79f9f4` and `67fbc42e9` (verbs
  `idle`/`state`), `daynight.irprobe_cmd` defaults to `timps-irprobe`.

## Before the default may be switched on

Each on at least one T23 and one T41 (OpenIMP, keepalive auto), plus one
dual-IR board; results into this note:

1. **M-A per SoC, long idle**: AE keeps regulating without a consumer for
   hours (not just minutes); the freshness WARN never fires on a working
   stack, and fires on a stack without keepalive forced on.
2. **AE settle after "IR on"** with and without a consumer, light to dark
   scenes: time to a usable frame. Sets `DN_IR_WAKE_SETTLE_MS` (3500) and
   whether `HUB_IMAGE_HOLD_MAX_MS` (5000) is enough.
3. **Hook latency** of `timps-irprobe on`, `idle` and `state` on the target
   (fork + `jct` + `light`), cold and warm.
4. **First-frame luma after a wake** for RTSP, WebRTC, fMP4, MJPEG and
   `/snapshot.jpg`: how many seconds dark/overexposed; whether an IDR at
   `ready_at` is worth adding.
5. **Dusk and dawn pairs**, several nights, idle all night: noir vs day-optics
   readings over the twilight, how often the noir day rule fires early and
   `noir_cost` has to learn, number of clicks per night (target: dusk 1,
   dawn 1, at most one learnt false day per camera).
6. **Dual-IR boards** (850 + 940 nm): `idle` turns both off, `state` reads
   both, rollback (exit 4) leaves both on.
7. **Real poll cadences** of Home Assistant, the agent-adapter and NVRs that
   poll snapshots: confirm or change the 60 s linger default; HA's 10 s
   timeout vs the hold.
8. **Heartbeat in the dark**: audible probe count per night with the default
   `heartbeat_s`/`heartbeat_max_s`; acceptable in bedrooms?
9. **External LED drivers**: `daynight night` from cron/WebUI while idle
   (re-assert, `state` check before every day verdict) - no stuck-on LED,
   no false day.
10. **Restart/crash with the LED off**: SIGKILL and `S95timps restart` relight
    within the boot assertion.
11. **LED power** saved per night (current draw on/off) - the benefit side.
12. **WebUI on a camera**: card states live, save/reload, phone width; with
    the preview stopped the chip reaches "IR off (idle)".
13. **Fleet share**: how many cameras are idle at night at all (no Frigate,
    recorder or motion) - decides whether a default change matters.

Even then the default should only change for 940 nm / indoor profiles; at
850 nm the tell stays a user decision.
