# timps — Troubleshooting Catalogue

**Audience: an AI assistant helping a thingino user whose camera misbehaves.**
Companion to `docs/ai/reference.md` (what timps *is*) and
`docs/ai/config-keys.md` (what every key *means*). This file is the
*failure* view: symptom → cause → how to confirm → fix.

## How to use this file

1. Start at **§0 Quick triage**: match the user's words to a section anchor.
2. If the user pasted a log line, go straight to **§12 Log-message
   dictionary** — it covers every `LOGE`/`LOGW` string in `src/`.
3. Always confirm build and version first: `GET /control` → `version`,
   `caps.*`, `*.available`. Most "it doesn't work" reports are
   *compiled out*, not misconfigured.
4. Quote key names exactly. A wrong key name sends the user on a dead end,
   and `POST /control` silently lists unknown names under `ignored`.
5. Entries marked **(unverified)** are inference, vendor documentation or a
   field measurement with no record in this repository — say so when you
   repeat them.
6. §15 is the closing note for the assistant; **§16 follows it** and catalogues
   the shipped scripts and CGIs.

Everything else here was read out of this repository at **v1.9.21
(2026-09-25)**; a statement marked **since v1.9.22 (unreleased)** is in the
source but in no tagged release yet. Source files are named; line numbers deliberately are not.

---

## 0. Quick triage

| The user says… | Go to |
| --- | --- |
| "No video at all", stream URL just hangs | [§2 Video/encoder](#2-video-and-encoder), [§1 Startup](#1-startup-and-config) |
| "Daemon isn't running / camera keeps rebooting" | [§1 Startup](#1-startup-and-config) |
| "It just died / exited on its own", a pasted `/run/timps.crash` | [§1.9 Exit codes and crash artefacts](#19-startup-exit-codes-and-crash-artefacts) |
| "Black image" / "green image" / "frozen frame" | [§2 Video/encoder](#2-video-and-encoder), [§3 Image/ISP](#3-image--isp--day-night) |
| "Purple / pink / IR-tinted picture" | [§3 Image/ISP](#3-image--isp--day-night) |
| "Horizontal bands / flicker under lights" | [§3 Image/ISP](#3-image--isp--day-night) |
| "Image is upside down / sideways / rotation ignored" | [§2 Video/encoder](#2-video-and-encoder) |
| "Choppy, stuttering, freezes then jumps" | [§2 Video/encoder](#2-video-and-encoder), [§4 Streaming](#4-streaming-per-protocol) |
| "Bitrate is way higher than I set" | [§2 Video/encoder](#2-video-and-encoder) |
| "RTSP won't connect" / VLC error | [§4.1 RTSP](#41-rtsp) |
| "Browser preview black / stalls" | [§4.2 HTTP](#42-http-fmp4-mjpeg-snapshot-sse) |
| "WebRTC stays black / never connects" | [§4.3 WebRTC/WHEP](#43-webrtc--whep) |
| "SRT player gets nothing" | [§4.4 SRT](#44-srt) |
| "No audio" (anywhere) | [§6 Audio](#6-audio-and-backchannel) |
| "Talk-back / two-way audio doesn't work" | [§6 Audio](#6-audio-and-backchannel) |
| "Mic got noisy after I used talk" | [§6 Audio](#6-audio-and-backchannel) |
| "No motion events" / "too many motion events" | [§7 Motion](#7-motion) |
| "No recordings" / "recordings have gaps" | [§5 Recording](#5-recording-timelapse-storage) |
| "SD card / NFS share fills up" | [§5 Recording](#5-recording-timelapse-storage) |
| "Day/night keeps flapping" | [§3 Image/ISP](#3-image--isp--day-night) |
| "Stays in night mode all day" / "never goes to night" | [§3 Image/ISP](#3-image--isp--day-night) |
| "CPU pegged / camera hot / load high" | [§2 Video/encoder](#2-video-and-encoder) |
| "Settings don't stick after reboot" | [§1 Startup](#1-startup-and-config) |
| "Can't log in" / 401 / 403 | [§8 Auth/network/TLS](#8-auth-network-tls) |
| "Browser warns about the certificate" | [§8 Auth/network/TLS](#8-auth-network-tls) |
| "https:// doesn't load but http:// does" (or vice versa) | [§8 Auth/network/TLS](#8-auth-network-tls) |
| "WebUI page is missing / buttons do nothing" | [§9 WebUI and plugins](#9-webui-and-plugins) |
| "PTZ / pan-tilt doesn't move" | [§10 PTZ interplay](#10-ptz-interplay-thingino-motors) |
| "Upgraded and now X broke" | [§11 Build/flash/OTA](#11-build-flash-ota) |
| "Flash full / OTA fails / no space" | [§11 Build/flash/OTA](#11-build-flash-ota) |
| "It worked on prudynt" | `docs/ai/reference.md` §1 — different daemon, different config file |
| "The snapshot button / heartbeat / restart button is broken" | [§16 Shipped scripts and CGIs](#16-shipped-scripts-cgis-and-helpers), [§9 WebUI](#9-webui-and-plugins) |
| "What does `/usr/sbin/<x>` do?" / a shipped script misbehaves | [§16 Shipped scripts and CGIs](#16-shipped-scripts-cgis-and-helpers) |
| A pasted log line | [§12 Log dictionary](#12-log-message-dictionary) |
| A config file that "looks right" | [§13 Misconfiguration gallery](#13-misconfiguration-gallery) |

---

## 1. Startup and config

### 1.1 The daemon does not come up at all

**Symptom:** no stream on any port, `GET /control` refused, WebUI shows
nothing.

Confirm, in this order:

```sh
ls -l /usr/bin/timpsd        # wrong streamer? prudynt/raptor has no timpsd
pidof timpsd
logread | grep -i timps | tail -n 100
```

`timpsd` writes to syslog because `general.syslog` defaults to on; the init
script backgrounds the process, so **stderr is discarded** — `logread` is the
only log. `src/main.c` parses three arguments — `-c <config>`, `-v` (raise the
level to DEBUG) and `-h` (print `timps <version>` plus a one-line usage and
exit 0). **Anything else is silently ignored**, so a typo'd flag looks like it
worked. The default config path is `/etc/timps.conf`.

| Log line (`src/main.c`) | Cause | Fix |
| --- | --- | --- |
| `another timpsd instance already holds <path> - refusing to start` | Single-instance lock held. A second instance re-initialising the ISP would corrupt the first one's pipeline, so it refuses. Exit code **1**. | Find and stop the other `timpsd`. Usually a hand-started one plus the init script. |
| `no HAL backend available` | Binary built without a usable HAL (neither the Ingenic backend nor the sim). | Build problem, not config — rebuild the package. |
| `HAL init failed - retrying in <N>s` | The **init** stage failed: `IMP_ISP_Open`, `IMP_ISP_AddSensor`, `IMP_ISP_EnableSensor` or `IMP_System_Init`, all inside `isp_init()`. **Retries forever** with exponential backoff 5 → 60 s, and these failures **do not count** toward the start-failure budget. | See §1.2. |
| `HAL start failed (<n>/10) - unwinding and retrying in <N>s` | The **start** stage failed: framesource/encoder/OSD/bind bring-up in `ing_start()`. This is the only path bounded at `MS_STARTUP_MAX_START_FAILS` = **10**, after which the one-shot reboot fires. | See §1.2. |

**The distinction matters for triage.** A wrong or missing sensor fails in
`isp_init()`, so it produces `HAL init failed - retrying` forever and
**never reaches the reboot escalation**. A camera that reboots itself once has
a *start*-stage problem (rmem, encoder, framesource), not a sensor problem. The
`video pipeline bring-up failed - tearing down partial state` line comes only
from `ing_start()`'s fail label, and is the marker that this attempt was one of
the ten.

### 1.2 The one-shot recovery reboot

After 10 consecutive `start()` failures (or a teardown that had to be
abandoned), `src/main.c` escalates to **one** real `reboot()` and no more.
This is the 2026-08-22 T31 fleet precedent: process-level retries never
cleared the board's stuck rmem carve-out, a real reboot cleared it every time.

The one-shot-ness lives in a marker file, `/etc/timps-startup-reboot.flag`
(`MS_STARTUP_REBOOT_MARKER`). Three log outcomes:

- `… - escalating to ONE reboot before giving up permanently` — normal
  escalation. The camera reboots. Expect it back in a minute.
- `… - AGAIN, after the one-shot recovery reboot already tried for this…
  (needs manual intervention, not another reboot)` — the reboot did not help.
  **Do not tell the user to keep power-cycling.** This is a board/hardware or
  SDK-state problem; collect `dmesg` and the full `logread`.
- `…, but the one-shot reboot marker … cannot be written (…) - … giving up
  permanently WITHOUT the escalation reboot. Fix the rootfs (/etc full, or
  mounted read-only?)` — the rootfs is full or read-only. Fix that first;
  until then timps deliberately stays down rather than boot-loop.

Related: `could not clear the startup-reboot marker … Remove it by hand`
means a **stale** marker is sitting there and the *next*, unrelated incident
will skip its recovery reboot. `rm /etc/timps-startup-reboot.flag`.

A camera that "reboots every few minutes" is therefore **not** timps's
escalation path (that fires once), and it cannot be a timps respawn loop
either, because **nothing respawns `timpsd`** (§2.2). Look elsewhere — a
kernel watchdog, a PMIC, or the power supply.

The underlying condition this escalation exists for is worth knowing: after
a restart, **the previous instance's rmem carve-out (22 MB on T31 boards) is
not always released** by the time the new instance runs. `S95timps`'s wait
loop only proves the PID is gone; the kernel-side ISP/encoder release lags.
`IMP_System_Init`'s small allocations squeak through while the encoder's big
contiguous allocation in `start()` still fails. Before v1.9.3 a single
`start()` failure exited the process permanently — 5 of 12 cameras stayed
down after one fleet restart on 2026-08-22 and needed manual reboots.
**Upgrade to ≥ 1.9.3.**

One operational scar to recognise: **`S95timps stop` during a bring-up
backoff sleep kills `timpsd` with no log line at all.** Not because the
handlers are missing — `on_signal` is installed *before* the retry loop, so
`SIGINT`/`SIGTERM` is caught and just clears `g_run`. The silence is that the
loop's own `if (!g_run) return 1;` exits with **no log line at all**: no
`shutting down`, no `teardown complete`. It looks exactly like a silent crash;
the tell is the absence of a crash record in `/run/timps.crash` (§1.9).

### 1.3 Config file handling — what is and is not true

Verified in `src/config.c`:

- **A missing config file is not fatal.** `config load` logs
  `config <path> not found, using defaults` and runs on built-in defaults.
  timps does **not** create the file at first boot; `config_write_keys()`
  creates it the first time something is persisted.
- **Rewrites preserve whole-line comments — but not inline ones.**
  `config_write_keys()` copies the existing file line by line, replaces the
  lines whose key it owns, appends the rest, and writes atomically (`mkstemp` +
  `fsync` + `rename` + directory `fsync`). Full-line comments, blank lines,
  ordering and unknown lines survive. **An inline `# comment` on a rewritten
  key's own line is lost**, because `write_kv_line()` regenerates that line as
  `key = value`. So "the daemon will eat your comments" is wrong *in general*
  and right *for the trailing comment on a key you then change from the WebUI*.
  Matches `docs/ai/config-keys.md` §1.5.
- **Duplicate lines for the same key:** the *first* occurrence is replaced,
  later duplicates are **dropped** on the next rewrite. Before a rewrite, the
  loader applies lines in file order, so the **last** one wins at load time.
  A file with two different values for one key therefore changes meaning the
  first time that key is persisted. Remove duplicates.
- **Aliases are canonicalised on write.** A pre-rename alias line (e.g.
  `rtsp.username`, `daynight.total_gain_day_threshold`) is *replaced* by its
  canonical spelling rather than left behind alongside a new line.
- **Long lines are dropped, not truncated:** `config: line longer than 510
  chars skipped (starts "…")`.
- **Unbalanced quotes are kept verbatim, quotes included:**
  `config: <key> has an opening quote but no closing one`.
- **Inline comments are stripped** from unquoted values. `osd0.text = Kamera
  #2` persists as `Kamera` — the writer quotes values containing
  `space # " ' ;` for exactly this reason, but a *hand-edited* unquoted value
  loses everything from the `#`. Quote it.
- **Unknown keys log `unknown key <name>` and are dropped.** They are not
  stored, not echoed, and do not survive a rewrite.

### 1.4 "The setting doesn't stick"

Four distinct causes, distinguishable from the `POST /control` reply body:

| Reply field | Meaning | What to do |
| --- | --- | --- |
| `"ignored":["…"]` | The build does not know that field name (typo, or feature compiled out). Counts are unaffected — a request mixing one good key with one typo still answers `200`. | Check the spelling against `GET /control?fields=1`. |
| `"deferred":N,"deferred_keys":[…]` | Changed fields that were **persisted but did not reach the running pipeline**: `video*`/`sensor.*`, and since v1.9.20 the restart-only `audio.*`/`osd.*` keys (`caps.restart`). | Restart `timpsd`. |
| `"not_persisted":N` | The value is live in memory but was **not written to the file**. | Split the request; see below. |
| `"rejected":N` | A value was refused outright (e.g. `speaker play` with a bad path). | Fix the value. |

`not_persisted` in practice: `control.c` batches at most `CTRL_MAX_CHG` =
**48** changed keys per request. Past that the tail stays live-but-unsaved and
`too many settings in one request, <key> not persisted` appears in the log.

`config_write_keys()` has a second, higher cap of **64** keys per call, whose
warning is `config_write_keys: <n> keys in one call, only the first 64 are
persisted`. **That line is unreachable from `/control`** — 48 < 64 — so if a
user pastes it, the write came from some other caller, not from a POST. Do not
tell them to split their request.

The other way a change silently does not persist:
`read error on <path> … ABORTING the config rewrite … this flash needs
attention` — a bad flash block. The *old* file is intact and the change is
live only. Take this seriously: the flash is failing.

### 1.5 Values that are clamped, coerced or ignored rather than rejected

This is the single most common source of "it accepted my value and did
something else". `src/config.c` **clamps numeric keys into range** and
**coerces unsupported enums**, then reads back and persists the *clamped*
value — so `GET /control` shows what the encoder will really do, not what
the user typed. It does not reject.

Consequences worth stating to a user:

- `videoN.fps = 0` becomes **1** (range 1..120). `videoN.bitrate = 0`
  becomes **16** kbps (range 16..50000).
- `videoN.width/height` clamp to 64..4096, `sensor.width/height` to 0..8192.
- `image.*` knobs clamp to the `imp_isp.h` domains (mostly 0..255;
  `highlight_depress`/`backlight_compensation` 0..10; WB gains 0..65535).
- `motion.cols`/`motion.rows` clamp so that `cols × rows ≤ MOTION_CELL_LIMIT`
  (`src/motion_caps.h`; 52 on most SDKs, **4** on the old T10/T20 3.9.0 SDK).
  The axis *being set* is clamped against the current other axis.
- `daynight.mode` with an unknown word logs
  `daynight.mode: unknown '<x>', keeping auto` and stays on `auto`.
- **since v1.9.19:** an *autodetected* `sensor.fps` is capped at
  **30** and says so (`sensor.fps: driver max_fps=<N>, auto capped to 30 (set
  sensor.fps to override)`). **since v1.9.20** the cap rises to
  the fastest enabled `videoN.fps` when that is higher. This is the one clamp
  that does **not** apply to a configured value — write `sensor.fps`
  explicitly and it is passed through. §1.10
- Rotation and codec are coerced at parse time — see §2.
- `"nan"` in a float key lands on the lower rail rather than poisoning every
  comparison downstream (`pflt_cl()`).

**The one exception where clamping is loudly called out** is transport
security. Fields flagged `F_SECVAL` (`http.https`, `rtsp.tls`) log:

> `http.https = 'strict' is not a valid value and was read as 0 - that is
> PLAINTEXT, no TLS. Valid: 0, 1 or 2`

Because `pbool()` turns any unrecognised word into `0`, a typo like `ture`,
`enabled` or `strict` silently disables TLS. Always have the user read this
key back from `GET /control`.

### 1.6 Keys that exist, persist, echo back — and do nothing

`src/config.c` warns once per session for each of these:

| Key | Log line | Reality |
| --- | --- | --- |
| `videoN.max_gop` | `videoN.max_gop is reserved and IGNORED - the keyframe interval comes from videoN.gop` | Use `videoN.gop`. |
| `motion.roi_x/y/w/h` | `motion.roi_* is deprecated and IGNORED - use the motion grid (motion.cols/rows + cells) instead` | Use the cell grid. |
| `osd.hinting` on a build without `USE_OSD_HINTING` | `osd.hinting is stored but has no effect in this build` | Rebuild with `BR2_PACKAGE_TIMPS_OSD_HINTING`, or accept the default rendering. |
| `videoN.qp` under any `rc_mode` other than `fixqp` | *(no warning)* | Only consumed as `iInitialQP` when `rc_mode = fixqp`. |
| `videoN.quality_lvl` / `change_pos` / `fluc_lvl` on a new-API SoC | `videoN.quality_lvl/change_pos/fluc_lvl: no equivalent field in this SoC's encoder API - values ignored` | Classic-SoC knobs only. `fluc_lvl` is additionally H.265-only. |
| `videoN.i_bias_lvl` where the SDK lacks `SetChnQpIPDelta` | `videoN.i_bias_lvl: this SoC's SDK has no IMP_Encoder_SetChnQpIPDelta - value ignored` | Nothing to do. |

### 1.7 daynight keys retired by the 2026-08-17 / 2026-08-22 redesigns

Two different warnings, two different meanings.

**Retired entirely** — `<key> is obsolete and IGNORED - <why>`:
`daynight.day_gain_pct`, `baseline_delay_s`, `boot_settle_max_s`,
`boot_stable_pct`, `night_reconfirm_s`, `probe_max_skip_s`,
`threshold_low`, `threshold_high`, `hysteresis`, `learn`, `state_path`.
The mechanisms behind them (adaptive baseline, probe backoff, the brightness
fallback, the learning subsystem) no longer exist. Delete the lines. The
replacements are `daynight.day_gain` / `daynight.night_gain` and
`daynight.heartbeat_s` / `heartbeat_max_s`.

**Frozen into internal constants** — `daynight.<key>=<v> is now a fixed
internal constant (<c>) and can no longer be tuned per camera`:
`probe_jump_pct`, `probe_settle_s`, `ref_delay_s`, `ir_min_headroom`,
`boot_settle_s`, `transition_s` (ints) and `ir_ratio_night`, `ir_ratio_day`
(floats). These warn **only when the configured value differs from the
constant**, so a camera that had tuned one away from the default is the one
whose behaviour changes. Tell such users their per-camera tuning is gone and
the fleet-wide value applies.

Both sets kept their **gain thresholds** as aliases:
`daynight.total_gain_day_threshold` → `daynight.day_gain`,
`total_gain_night_threshold` → `night_gain`. Existing tuning still applies.

### 1.8 Sensor identity mismatches

`config_sensor_finalize()` (`src/config.c`) reads the kernel sensor registry
under `/proc/jz/sensor/` (flat, or under a `sensor0/` subdirectory — both
layouts exist) and treats the **loaded driver as authoritative** for the
sensor name and I2C address:

- `config sensor.model 'X' != loaded driver 'Y' - using 'Y' (the config value
  would crash the ISP)` — a mismatching name makes the sensor kernel module
  work from a zero attribute table (`pclk`/`line_time` = 0) and divide by
  zero, i.e. **SIGFPE in the kernel**. The override is a safety net; fix the
  config line anyway.
- `config sensor.i2c 0x.. != loaded driver 0x.. - using 0x..` — same idea.

Resolution and fps stay config-first (the registry often reports 0). When
nothing supplies them: fall back to `video0`'s values, then 1920×1080 @ 25,
model `gc2053`, I²C `0x37`. A camera reporting `sensor: gc2053 … 1920x1080
@25fps` on hardware that is not a GC2053 means `/proc/jz/sensor/` was
unreadable — the sensor `.ko` did not load.

### 1.9 Startup, exit codes and crash artefacts

Nothing supervises `timpsd` (§2.2), so *how* it died is the whole diagnosis.
All of this is `src/main.c`.

**Exit 0** — every one of these looks identical to `start-stop-daemon`:

- `-h` on the command line (prints version + usage).
- A normal shutdown. The marker is the last line, `teardown complete -
  exiting`, after `shutting down`.
- `hard_exit()`: the 3 s shutdown guillotine (`MS_SHUTDOWN_ALARM_S`) fired, or
  a **second** `SIGINT`/`SIGTERM` arrived while the first was being handled. It
  `_exit(0)`s wherever teardown happens to be — an unfinished recording segment
  loses its `moov`. It writes `timpsd: shutdown alarm fired - hard exit` to
  **stderr**, which the init script discards, so in `logread` the only evidence
  is the **absence of `teardown complete - exiting`** after `shutting down`.
- The video watchdog giving up (§2.2). It does `raise(SIGTERM)`, i.e. it goes
  through the ordinary teardown and returns 0 — so "the camera stopped serving
  and the daemon exited cleanly" is the expected shape, not a contradiction.

**Exit 1:**

- The singleton `flock` is held by another `timpsd`.
- `SIGINT`/`SIGTERM` during a bring-up backoff sleep — **no log line at all**
  (§1.2).
- The non-reboot branches of `startup_give_up()`: the reboot was already spent
  for this incident, the marker file cannot be written, or `reboot()` itself
  failed. Each logs its own LOGE first (§12.1).
- A HAL teardown that had to be abandoned while the operator was already
  stopping the daemon.

**Death by signal.** `SIGSEGV`, `SIGBUS`, `SIGFPE` and `SIGABRT` are caught
only to record the fault, then the default disposition is restored and the
signal is **re-raised** (so the shell/parent sees a signal death, not an exit
code; `_exit(128 + sig)` is an unreachable fallback). The handler writes to
stderr **and** to **`/run/timps.crash`**, which is the file worth asking for:

- the signal name, `si_addr` and the faulting `pc`;
- the whole of `/proc/self/maps`;
- one classification line, `fault address is in: …`, naming
  **`libimp.so`** (Ingenic video/ISP/encoder), **`libaudioProcess.so`**
  (Ingenic audio), `timpsd` itself, another mapping, or
  "not inside any mapped region (wild/corrupted pointer)".

That one line decides whether the bug is ours or the vendor's. An alternate
signal stack is installed, so a stack-overflow `SIGSEGV` is still captured;
`sigaltstack failed` in the log means it is not.

**Signals that are *not* handled:**

- **`SIGHUP` has no handler.** The default action terminates the process — no
  teardown, no log line, no crash file. `S95timps` has no `reload` for exactly
  this reason; do not suggest `kill -HUP`.
- `SIGPIPE` is ignored (a dead client must not kill the daemon).
- `SIGALRM` is used for **two different deadlines**: the 3 s shutdown
  guillotine above, and a 20 s **bring-up** teardown deadline
  (`MS_STARTUP_STOP_ALARM_S`). The second does not kill the process — it
  abandons the wedged `g_hal->stop()` via `siglongjmp` and writes
  `timpsd: HAL teardown did not return - abandoning it` to **stderr only**.
  Caveat: if the vendor call is stuck in an uninterruptible kernel ioctl,
  neither deadline can fire.

**Runtime files worth naming when asking a user to look:**

| Path | What it is |
| --- | --- |
| `/run/timps.lock` | The singleton `flock` (mode 0644). Held for the daemon's lifetime; a failed `flock` is the "another timpsd instance already holds …" refusal. If it cannot even be **opened**, timps logs `proceeding without the single-instance guard` and runs anyway. |
| `/run/timps.crash` | The fatal-signal record described above (mode 0644, truncated on each crash). |
| `/run/timps.token` | The per-boot `/control` token (`http.token_file`, mode 0640). |
| `/run/timps/audio_out` | The play FIFO (mode 0660, `USE_PLAY`) — see §6.5. |
| `/var/run/timpsd.pid` | Written by `start-stop-daemon -m`, **not** by timpsd, and never removed by it. |
| `/etc/timps-startup-reboot.flag` | The one-shot recovery-reboot marker (§1.2) — on `/etc` on purpose, so it survives the reboot. |
| `/run/timps-motion.lock` | `timps-motion`'s `flock` against overlapping motion events (package-side, not the daemon). |
| `/run/timps-irprobe.<type>` | `timps-irprobe`'s nonce stamp for its 60 s re-light watchdog, one per illuminator type (`ir940`/`ir850`/`ir`). |
| `/run/motors-active` | Set by thingino-motors; `timps-motion` returns silently while it exists. |

**Non-fatal start-up conditions**, all of which leave a running daemon:

| Condition | What happens |
| --- | --- |
| Config file missing or unreadable | `config <path> not found, using defaults`. Built-in defaults, not fatal, and the file is not created until something is persisted. |
| RTSP port in use | `cannot bind rtsp port <n>`, `rtsp_start()` returns NULL, everything else runs. |
| HTTP port in use, or TLS fail-closed | `httpd_start()` returns NULL. **`webrtc_start()` is skipped whenever `httpd` is NULL**, so a bind failure on the HTTP port silently takes WHEP down with it. |
| RTSPS cert/bind failure | Degrades to plain RTSP (not fail-closed, unlike HTTP). §4.1 |
| Sensor not found / ISP init fails | Retries forever, no reboot escalation (§1.1). |
| Audio bring-up fails | `audio input unavailable`, video continues. §6.1 |
| A second instance | `another timpsd instance already holds …` + exit 1. |

### 1.10 `sensor.fps` — what auto gives you, and what the driver holds

**All of this section is since v1.9.19.** On v1.9.18 and earlier
the sensor rate was set and never checked, and nothing was logged at all.

Three different numbers get confused here:

| Number | Where it comes from | Where to read it |
| --- | --- | --- |
| The **requested** sensor rate | `sensor.fps`, or the driver's `max_fps` when unset | `GET /control` → `sensor.fps` |
| The rate the **driver holds** | `IMP_ISP_Tuning_GetSensorFPS()` after the set call | the log line below — **not** exposed over `/control` |
| The **stream** rate | `videoN.fps`, per encoder channel | `GET /control` → `video.<N>.fps` |

One line per bring-up, from `isp_set_sensor_fps()` in
`src/hal/hal_ingenic.c`:

> `sensor fps: requested 25, driver holds 25/1, set rc=0`

- INFO when the set call returned `0` **and** the readback matches.
- **WARN** when the return code is non-zero, or the driver holds a different
  rate (`driver holds 25/1 (25.00)` against a requested 30 means the driver
  refused). The number the driver holds is the truth; `GET /control` keeps
  echoing what was requested.
- **WARN** `readback unavailable (rc=…)` when the driver's
  `IMP_ISP_Tuning_GetSensorFPS` fails or returns a zero denominator. The set
  call may still have worked; this line says only that it could not be
  verified. (Up to the fix in v1.9.19 this was also the permanent state on
  T10, whose SDK was wrongly assumed not to declare the getter.)

Pitfalls:

- **Auto is capped.** Leaving `sensor.fps` unset on a sensor whose driver
  advertises more caps it at 30 and logs
  `sensor.fps: driver max_fps=40, auto capped to 30`. The GC2053 is the
  motivating case: it reports `max_fps = 40` on a mode its clock runs at 30.
  To ask for more, write the value explicitly. On v1.9.19 this also capped a
  stream configured above 30 (e.g. `video0.fps = 60` on a 60 fps sensor) to
  a 30 fps sensor; **since v1.9.20** the cap is the fastest
  enabled `videoN.fps` when that is higher, and the log says which registry
  key (`max_fps` or `fps`) the value came from.
- **`videoN.fps` is the stream rate only.** It is passed to the framesource,
  never to the sensor driver, so raising it does not raise capture. §13 #18
- **`sensor.fps` is restart-only** (like every `sensor.*` key): a POST persists
  it and the POST reply lists it under `deferred`. It reaches the ISP at the
  next bring-up.
- A WARN here that persists across restarts is a driver/SoC limit, not a config
  error — at `sensor.fps = 30` on a T31 GC2053 also check
  `image.anti_flicker` (the driver publishes a 25 fps line model at probe, so
  neither 50 nor 60 Hz lands on a true mains period). §3.1

---

## 2. Video and encoder

### 2.1 On-demand encoding — the #1 false alarm

timps starts the framesource/encoder **when a client attaches** and stops it
`MS_IDLE_STOP_US` = **2 s** after the last one leaves
(`src/hal/hal_ingenic.c`). So:

- `framesource N disabled (idle)` and `video chnN idle` in the log are
  **normal**, not a fault.
- `GET /control` omits a channel from the `encoder.<n>` object when it is not
  running. An *absent* channel is not the same as a broken one.
- "No video" reported from a `GET /control` snapshot alone is meaningless.
  Have the user actually open the stream, then look.

### 2.2 No video, or video that stops

| Log (`src/hal/hal_ingenic.c`) | Meaning | Action |
| --- | --- | --- |
| `chnN: PollingStream idle (rc=…, miss#N) - encoder emits no frames` | The encoder produced nothing for a full watchdog window (`MS_VIDEO_WATCHDOG_ITERS` = 10). | Occasional single misses self-heal. Repeats mean the ISP/encoder is wedged. |
| `chnN: encoder dead after N consecutive misses - forcing a framesource disable/enable cycle to recover (recovery attempt n/5)` | Automatic recovery, bounded at `MS_VIDEO_WATCHDOG_MAX_RECOVERIES` = **5**. | Watch whether it recovers. |
| `chnN: N consecutive forced-recovery cycles never produced a frame - encoder/ISP is not coming back on its own; exiting (camera needs a manual/scheduled restart)` | Gave up; the **process exits**. It does so by `raise(SIGTERM)`, i.e. through the ordinary shutdown path — so it **exits 0** with `shutting down` … `teardown complete - exiting`, and the pidfile `start-stop-daemon -m` wrote is **left behind**. A stale `/var/run/timpsd.pid` after a watchdog give-up is therefore expected, and `S95timps status` will report a pid that no longer exists. | **Nothing restarts it.** See the warning below. If it recurs, this is an SDK/board problem — collect `dmesg`. |

> **There is no watchdog and no respawn for `timpsd`.** `/etc/inittab` has no
> respawn line, `S95timps` is start/stop only, and `/dev/watchdog` covers
> kernel hangs, not a dead userspace process. **If `timpsd` exits for any
> reason it stays dead until a human or a cron job notices.** A user who says
> "the camera went dark and came back only after I rebooted" has usually hit
> exactly this, not a hardware fault.
| `jpeg chnN: … giving up on this channel (MJPEG/snapshot output disabled until restart)` | Same watchdog on the JPEG channel, but it only disables **JPEG**; video keeps running. | Restart `timpsd` to get snapshots back. |
| `chnN: GetStream failed after PollingStream OK` | Transient SDK hiccup. | Ignore unless constant. |
| `video pipeline bring-up failed - tearing down partial state` | `ing_start()` failed; `main()`'s retry loop takes over (§1.1). | Read the LOGE immediately above it — that one names the real failure. |

Bring-up LOGEs that precede it, in pipeline order:
`IMP_ISP_Open failed` → `IMP_ISP_AddSensor failed (sensor=…)` →
`IMP_ISP_EnableSensor failed (sensor=…)` → `IMP_System_Init failed after N
tries` → `FS_CreateChn N` / `FS_SetChnAttr N failed` →
`Encoder_CreateGroup N failed` / `Encoder_CreateChn N` /
`Encoder_RegisterChn N to group M failed` → `Bind fsN->encM failed` /
`Bind fsN->osdM failed` / `Bind osdN->encM failed` →
`video chnN thread create failed`.

`IMP_System_Init busy, retry n/5 in 1s (ISP still releasing?)` is the
signature of a **previous instance's rmem not yet released** — the classic
"restarted too fast" case. It usually clears by itself.

`<phase>: N IMP call(s) failed (first: …) - the next start may fail ISP init`
is a teardown that did not fully clean up. Expect the *next* start to be
rockier; a reboot is the reliable cure.

### 2.3 Dropped frames / the stream never becomes decodable

| Log | Meaning |
| --- | --- |
| `chnN: AU exceeds max buffer (need=…, max=…, packCount=…) - dropping frame (N dropped so far)` | One access unit was bigger than the assembly buffer. The buffer is sized from the stream resolution between `MS_AU_BUF_MIN` (128 KB) and `MS_AU_BUF_MAX` (1 MB). |
| `chnN: AU assembly overflow (cap=…, need=…)` / `chnN: no memory for AU packet` | Same family; the latter is genuine OOM. |
| `chnN: JPEG exceeds max buffer` / `JPEG assembly overflow` / `no memory for JPEG packet` | Same for the JPEG channel (`MS_JPEG_BUF_MIN` 96 KB … `MS_JPEG_BUF_MAX` 1 MB). |
| `jpeg chnN: empty stream (packCount=…) - dropping frame` | The SDK returned a zero-pack stream. |

**The livelock to recognise.** When the dropped AU is a *keyframe*, the
downstream heal path asks for another IDR, which is just as large, which is
dropped again — P-frames flow and no client ever gets a decodable picture.
timps warns about this at bring-up, before it can happen:

> `encoder chnN: <B> kbps at <F> fps implies ~<K> KB keyframes, near or above
> the 1024 KB AU buffer - keyframes will be dropped and clients may never get
> a decodable stream; lower videoN.bitrate or raise MS_AU_BUF_MAX`

The trigger, read straight off `enc_create()`, is
`bitrate_kbps × 1000 / fps > MS_AU_BUF_MAX / 10 × 8` = **838 856**
(80 % of the 1 MB buffer). Rearranged: the warning fires when

> `videoN.bitrate` **> ~839 × `videoN.fps`** kbps

— about **21 Mbps at 25 fps**, 12.6 Mbps at 15 fps. 25000 kbps at 25 fps trips
it; 4096 kbps at 25 fps is nowhere near it. (An older edition of this file said
`fps × 6500`, which is ~8× too strict and would have you lowering a perfectly
healthy 4 Mbps stream.) Lowering the bitrate is still the right answer when it
*does* fire — a 1 MB keyframe is not a setting anyone on these boards wants.

#### Consumer-side drops: reading `queue_drops`

`queue_drops[<n>]` in `GET /control` counts **consumer-queue evictions** on
video stream `n`: a subscriber (RTSP session, fMP4 client, WebRTC session, SRT,
the recorder) fell behind and its own `fanqueue` dropped the oldest packets.
It is *not* an encoder-side counter — that is `encoder.<n>.au_drops` — and it
says nothing about which consumer was slow. **since v1.9.20** the
hub counts at the push that evicts: one count per published packet that had
to evict from a consumer's queue, audio included, for every consumer alike. On
v1.9.19 each consumer counted its own drops when it next popped a packet - so
a client wedged in `send()`, which never pops again, was **never counted**
(the classic "a viewer is stuck but `queue_drops` stays 0"), and RTSP/SRT
counted only video evictions, WebRTC only those outside its PLI window. Expect
higher numbers from v1.9.20 for the same situation; compare rates, not
absolute values across versions.

**since v1.9.19** the log says which one, at the shipped log
level, at most once per 60 s per (consumer kind, stream):

> `chn=0 rec: 12 queue overflows in the last 60s (consumer too slow)`

(on v1.9.19 the line ended `(consumer too slow, IDR re-requested)`)

The kinds are `rec`, `rtsp`, `mp4`, `webrtc`, `srt` (`HUB_DROP_*` in
`src/hub.h`), the module tag is `HUB`, and the counter behind the line is the
same `queue_drops`. The window is emitted by the producer, so an open one is
also flushed when its stream loses its last subscriber — otherwise an
on-demand stream that idle-stops would hold the line back until the next
viewer attached. Before v1.9.19 each consumer WARNed once per session on its
*first keyframe* drop and logged everything else at DEBUG, so a camera dropping
steadily showed a silent log and only a moving counter — the usual reason a
user reports "the number goes up and nothing is in the log".

What `queue_drops` means, unchanged in v1.9.19:

- **A rising number is a consumer/link problem, not an encoder problem.** Find
  the kind in the summary line, then the client: a weak WiFi viewer, an NVR
  pulling more than the link carries, an NFS `record.dir` that stalled.
- **It never resets** except on restart (`g_qdrops[]` in `src/hub.c` is only
  ever incremented), so compare two reads a minute apart rather than reacting
  to an absolute value.
- **Recovery requests are never lost.** **since v1.9.20** RTSP,
  SRT and WebRTC hand every video eviction to the hub instead of dropping
  requests inside their own 1 s window; on v1.9.19 a P-frame lost within a
  second of the previous request stayed unhealed until the next natural
  keyframe (2–3 s at `gop=50`).
- **There is no config key for any of this.** The recovery interval
  (`HUB_IDR_RECOVERY_MIN_US`, 1 s) and the summary interval
  (`HUB_DROP_REPORT_US`, 60 s) are compile-time constants in `src/hub.h`, and
  deliberately so. `http.adaptive_drop` is the only related runtime knob, and
  it governs the fMP4 path only.

### 2.4 Bitrate is much higher than configured

Three separate mechanisms, all real:

1. **`rc_mode` semantics.** `cbr` is the only mode that treats
   `videoN.bitrate` as a ceiling to hold. `vbr`, `smart`, `capped_vbr`,
   `capped_quality` treat it as a target/cap with quality-driven excursions,
   and `fixqp` ignores it entirely (the rate is whatever the fixed QP
   produces). A user who sets `rc_mode = fixqp` and then complains the
   bitrate ignores their setting has found the documented behaviour.
2. **Per-SoC mode fallbacks**, logged at bring-up:
   `rc_mode capped_vbr/capped_quality has no classic-SoC equivalent -> using
   vbr` and `rc_mode smart has no new-API equivalent -> using
   capped_quality`. The mode actually in force is the one in
   `GET /control?stats=1`.
3. **IDR storms.** Every consumer whose queue overflows asks the shared
   encoder for a fresh keyframe, and that keyframe goes to **all** clients.
   One slow viewer therefore raises the bitrate for everyone. Look at
   `queue_drops` in `GET /control` and at
   `… send queue overflowed, dropping frames (client/network too slow)`.
   **since v1.9.19:** recovery IDRs are rate-limited **per video
   stream** (`hub_request_idr_recovery()`, 1 s) instead of per consumer, so
   *N* slow consumers can no longer cost *N* IDRs/s on the one shared encoder.
   A request that loses the race is coalesced, not dropped — it is issued by
   the next published frame once the interval has passed, so a frozen consumer
   always gets its keyframe. A keyframe published before that interval elapses
   **cancels** the coalesced request (it is what the consumers were waiting
   for), so a single drop burst costs one forced IDR, not two. Requests a
   client needs to **start** decoding
   (subscribe, RTSP `DESCRIBE`/`PLAY`, a fresh fMP4 `GET`, the WebRTC answer)
   still go out immediately. A single slow consumer heals exactly as fast as
   on v1.9.18; what changes is the multi-consumer case, which is where the
   storm was.

Also: `videoN.min_qp (a) > max_qp (b) - programming b..a` — timps swaps them
rather than refusing. And `Encoder_SetChnQpBounds N (a..b) failed - using SDK
default range` means the SoC refused the bounds outright.

### 2.5 Live changes that "don't take"

`hal_ingenic.c` applies rate control live where the SDK allows it and
otherwise defers to restart, logging which happened:

- `control <key> applied to the running encoder (takes effect at the next
  IDR/GOP)` — applied, but you will not see it until the next keyframe.
- `SetChnAttrRcMode chnN failed - value applies on restart`,
  `SetChnBitRate chnN failed …`, `SetChnQpBounds chnN failed …`,
  `SetChnQpIPDelta chnN failed …`, `SetChnAttrRcMode chnN (fixqp qp) failed …`
  — the live path was refused; the value is persisted and applies on restart.
- `live rc change on an H265 stream: the classic SDK's SetChnAttrRcMode is
  H264-only - applies on restart` — **H.265 rate control is restart-only on
  classic SoCs.** Expected, not a bug.
- `<key> persisted, applies on restart` — the generic deferred case, and what
  puts a key in the POST reply's `deferred_keys`.

Resolution, codec, GOP, profile, rotation, buffers and every `sensor.*` key
are restart-required. Restart: `/etc/init.d/S95timps restart`.

### 2.6 Rotation

`USE_ROTATE` is **off by default** (`src/rotate_caps.h`). Without it, no
`ROT_HAS_*` macro is defined and `config.c`'s `prot()` coerces every
non-zero `videoN.rotation` to 0, logging
`rotation <d> unsupported on this SoC -> 0`.

Capability matrix (`src/rotate_caps.h`):

| SoC | 90 / 270 | 180 |
| --- | --- | --- |
| T40, T41 | `ROT_HAS_HW_I2D` — true hardware I2D rotate | **Yes**, genuine per-channel |
| T31 | `ROT_HAS_FS_ROTATE` — libimp FrameSource rotate | No |
| T23 | `ROT_HAS_SW_90`, **only** with `USE_SW_ROTATE` / `-DMS_ENABLE_SW_ROTATE` | No |
| T10, T20, T21, T30, C100 | **No** — coerced to 0 | No |

On every SoC except T40/T41, `rotation = 180` logs
`rotation 180 unsupported on this SoC -> 0 (use image.hflip+image.vflip)`.
That is the correct advice: there, 180 was only ever a *global* ISP
Hflip+Vflip falsely modelled as per-stream. `image.hflip = 1` +
`image.vflip = 1` does exactly the same thing, on every channel, everywhere.

**Legacy values:** `rotation = 1` and `2` are accepted as the raw libimp
`rotTo90` enum (1 → 90, 2 → 270), so a `prudynt.cfg`-style `rotation=1`
lands on the same physical direction it did there.

**Direction is not uniform.** On T23/T40/T41, 90 = clockwise; on T31, 90 =
counter-clockwise (kept numerically aligned with prudynt-t/raptor).

**T31 FrameSource rotate rejections** — the stream comes up **unrotated**
and says so:

- `videoN: refusing FS-rotate WxH not 64-aligned (T31 FS-rotate wants
  64-alignment) - stream will run UNROTATED` (`MS_FS_ROT_ALIGN` = 64)
- `videoN: refusing FS-rotate WxH@F exceeds vendor FS-rotate cap (<=1280x704,
  <=15fps; past it libimp software-rotates and the oversized geometry fails
  Encoder bring-up) - stream will run UNROTATED`
  (`MS_FS_ROT_MAX_PIXELS` = 1280×704, `MS_FS_ROT_MAX_FPS` = 15)
- `videoN: FS-rotate enable (SetChnRotate) failed - disabling rotation for
  this stream, bringing it up UNROTATED`
- `FS_SetI2dAttr N failed (rotation may stay inactive)`

**T23 software rotate** (`MS_SW_ROT_MAX_PIXELS` = 704×576,
`MS_SW_ROT_MAX_FPS` = 15, `MS_SW_ROT_WIDTH_ALIGN` = 16,
`MS_SW_ROT_MIN_WIDTH` = 256):

- `videoN: SW rotate at WxH@Ffps is CPU-HEAVY on this SoC - strongly consider
  a substream-class setting (<=704x576, <=15fps)` — a warning, it still runs.
- `videoN: refusing SW rotate at WxH@Ffps (exceeds safe envelope …) - stream
  will run UNROTATED` — past the envelope, refused.
- `videoN: SW rotate post-rotation width W unusable for the encoder (needs
  picWidth%16==0 and >=256; a 90/270 rotate makes encoder width = source
  height H) … use a source HEIGHT that is a multiple of 16 and >=256, e.g.
  704x576 -> 576x704` — the classic trap: **the source height becomes the
  encoder width**. Note the check order in `sw_rot_start()`: the **envelope**
  test (`ew × eh > 704×576`, or `fps > 15`) runs *first*, so on T23 a 1280-wide
  stream is refused before the alignment test is ever reached — and 1280×704 is
  refused too (901 120 px ≫ 405 504). A working T23 rotate is a
  substream-class source whose **height** is a multiple of 16 and ≥ 256, e.g.
  **704×576 → 576×704** or 640×512 → 512×640. (640×360 fails the alignment
  test: 360 % 16 ≠ 0.)
- `videoN.jpeg: rotated WxH not 32/8-aligned for InputJpege (need
  width%32==0, height%8==0; make source height a multiple of 32, e.g. 704) -
  JPEG disabled on this stream` — JPEG needs a *stricter* alignment than the
  encoder.
- `sw-rot chnN: VbmAlloc N failed (rmem exhausted?)` — out of reserved video
  memory. Lower the resolution or free a stream.
- `sw-rot streamN: OSD item N is a logo - not composited on the SW-rotate
  path (text only)` and `sw-rot streamN: software OSD active (… no privacy
  covers)` — **on the T23 software-rotate path there is no logo overlay and
  no privacy mask at all**, only software text, and its coordinates are in
  *rotated* frame space.
- `videoN.jpeg: standalone JPEG on SW-rotate stream (… jpeg_quality has NO
  effect on this path - the SoC's InputJpege ignores it)`.

**Rotation is restart-required** and the advertised dimensions swap
(`eff_width`/`eff_height` in `GET /control`).

### 2.7 OSD and privacy masks

Hard limits (`src/config.h`): **`MS_MAX_VSTREAM` = 2** video streams,
**`MS_MAX_OSD` = 8** overlay items per stream, **`MS_MAX_PRIVACY` = 4**
privacy regions, `MS_MAX_STR` = 64 bytes per string field.

| Log (`src/hal/imp_osd.c`) | Meaning / fix |
| --- | --- |
| `osd stream N item M: rendered WxH exceeds usable WxH - skipped (reduce font_size/text length)` | The rendered text does not fit. Shorten it or lower `osd<S>.<N>.font_size`. |
| `logo <path> (WxH) exceeds usable WxH - skipped` | Logo bigger than the frame. |
| `logo <path> not loaded` | File missing/unreadable/not a supported bitmap. |
| `stream N rotated D: hardware OSD/privacy limited to the top P px of the WxH frame (libimp picHeight range-check); lower overlays are clamped up. Use a square stream, 180, or ch1 for full coverage.` | **T31 rotated streams: OSD/privacy only work in a top band.** This is a libimp range-check, not something timps can widen. |
| `osd stream N item M: CreateRgn failed (region pool exhausted?)` / `osd stream N privacy M: CreateRgn failed` | The SDK's region pool ran out. Reduce the number of overlays/masks. |
| `IMP_OSD_SetPoolSize(N KB) failed - OSD overlays may not composite` | The OSD memory pool could not be sized at ISP init. |
| `IMP_OSD_RegisterRgn(rgnN,grpM) failed - this overlay will stay invisible` | The region exists but is not attached. |
| `IMP_OSD_SetGrpRgnAttr(rgnN,grpM) failed - overlay may render with wrong alpha or not at all` | Usually a transparency/alpha problem. |
| `osd updater thread create failed - overlays will not update` | Static OSD only from here on. |
| `osd item N: no region on stream M (disabled at startup) - enable persisted, applies on restart` | An OSD **item** disabled at boot has no region, so enabling it live cannot work. Restart. |
| `privacy N: no region on stream M (OSD+privacy off at startup) - persisted, applies on restart` | **Not** the same rule — see below. |

**The two are different, and conflating them sends users on a needless
restart.** `imp_osd_setup()` creates a region for each OSD item **only if that
item was enabled at boot**, but it **pre-creates all four privacy regions**
(hidden when disabled) as soon as the stream has an OSD group at all. So:

- **OSD items:** you genuinely cannot turn one on at runtime if it was off when
  `timpsd` started. The POST succeeds and persists; the picture changes at the
  next restart.
- **Privacy masks:** they *can* be toggled, moved and resized live. The warning
  fires only when the stream has **no OSD group in the first place** — i.e.
  `osd.enabled = 0` **and** no privacy region was enabled at boot — or when
  `CreateRgn` failed. `caps.privacy.available` reports `0` in exactly that
  case. The fix is to enable one privacy region (or OSD) in the file and
  restart **once**; after that every mask is live.

**Key spelling:** the canonical config key is `osd<S>.<N>.<field>`, dots
throughout — `osd0.0.enabled`, `osd1.3.font_size` (`osd_key()` in
`src/config.c`). There is no `osd0_0` form. Over `POST /control` the nesting is
`{"osd0":{"0":{"enabled":1}}}`; `{"osd":{"0":{…}}}` is the legacy shared form
that writes the item onto **every** stream.

---

## 3. Image / ISP / day-night

### 3.1 Flicker and banding under artificial light

`image.anti_flicker` means **0 = off, 1 = 50 Hz, 2 = 60 Hz**
(`src/hal/hal_ingenic.c`). The built-in default in `src/config.c` is **2**,
i.e. 60 Hz.

**So a camera in a 50 Hz country (most of the world outside the Americas)
ships with the wrong value and shows rolling horizontal bands under LED or
fluorescent light.** Fix: `image.anti_flicker = 1`. This is live-applicable
via `POST /control {"image":{"anti_flicker":1}}`.

If banding persists at the correct setting, the light source itself is
PWM-dimmed at a frequency the AE cannot lock to — no config fixes that.

### 3.2 Purple / pink / magenta / IR-tinted picture

Two distinct causes; separate them before advising.

**(a) `image.running_mode` forced while the board optics disagree.** In
timps's model (see `docs/wiki/Day-Night.md`) `image.running_mode` is *only*
the ISP colour pipeline (0 = day/colour, 1 = night/mono). The IR-cut filter
and the illuminator are moved by the **board hook**, `daynight.switch_cmd`.
If something sets `running_mode` directly — a script, a WebUI slider, a
POST — without running the hook, the ISP renders the day pipeline while the
IR-cut filter is still out and the IR LEDs are on. The result is the classic
purple/magenta cast. **Do not advise "just set `image.running_mode = 0` to
get colour back".** Let the day/night machine do it, or drive
`daynight.switch_cmd` too.

Related warning: `running_mode=<d> never followed the switch to <mode> -
board hook chain (switch_cmd -> color -> POST /control) incomplete, or manual
override`.

**(b) Wrong Bayer/flip handling at ISP probe time.** On T21 (and relatives)
the ISP samples the sensor's mbus code at probe, not at `sensor_init`, so a
flip-compensated code has to be set in `sensor_probe()`. A camera that is
mirrored *and* mis-coloured is a sensor-driver problem, not a timps config
problem. `/proc/jz/isp/isp-m0` shows the cached pattern. **(unverified in
this repo — it is a kernel/sensor-driver issue outside `src/`.)**

### 3.3 Day/night never switches, or flaps

First: `daynight.enabled` must be **1**, and `GET /control`'s `daynight`
object tells you what the machine currently believes.

The decision inputs come from an ISP dump file, `daynight.isp_path`. If it
is unreadable you get `<path> is not readable, using <other> instead - set
daynight.isp_path to silence this` or, worse, `<path> not readable,
detection idle` — **detection does nothing at all**.

| Warning (`src/daynight.c`) | What it really means | Fix |
| --- | --- | --- |
| `no probe has ever confirmed day (<N> in a row found night): the best day-pipeline exposure seen was <X> but daynight.day_gain is <Y>. Either this scene never gets bright, or the threshold is too strict - raise daynight.day_gain above <X>` | **The single most useful line in the whole day/night system.** It hands you the number to use. | Set `daynight.day_gain` just above the reported best exposure, keeping `night_gain` well above that. |
| `daynight.day_gain (<a>) is above night_gain (<b>) - the thresholds are swapped; using day<<b> night><a>` | The two thresholds are the wrong way round. timps swaps them and carries on. | `day_gain` must be the **lower** number. |
| `the ISP dump reports no gain ceilings (MAX SENSOR analog gain / MAX ISP digital gain), so the AE reserve is unknown here: a railed meter cannot be told from a dark scene …` | This sensor/ISP build does not expose gain ceilings. Several safety mechanisms are disabled: railed-boot re-tune, night-reference clipping protection, and ratio probes fall back to the audible IR-cut probe. | Nothing to configure. Expect a *clicking* IR-cut probe instead of a silent one, and accept coarser behaviour. |
| `the night reference sits at the sensor's gain floor and no integration-time reading is available, so a brightening scene cannot be detected here - the heartbeat carries this camera alone. If this sensor does report SENSOR Integration Time, check daynight.isp_path` | Night→day can only be found by the periodic heartbeat, not by the trend. | Check `daynight.isp_path`; otherwise tune `daynight.heartbeat_s` / `heartbeat_max_s` down for faster (but more disruptive) recovery. |
| `probe found night, but the pre-probe level <X> had only <N> units of AE reserve - the reference stays unset until the meter can answer` **(LOGI, not a warning** — it only shows at `general.loglevel = 2` or with `debug_modules = daynight`, and it is not a fault report) | AE is railed; the reading is a clip, not a level. | Usually self-resolving. Persistent → the scene is genuinely at the sensor's limit. |
| `boot: no usable exposure reading <N>s after start-up (<path>) - the boot measurement cannot run, falling back to the persisted <mode> and asserting it on the board once` | No measurement at boot. | Check `daynight.isp_path`. |
| `silent probe gave no usable reading - falling back to the IR-cut probe` | The silent (ratio) probe was inconclusive once. | Normal occasionally. |
| `'<cmd> on' failed (rc=…) - silent probe unavailable, falling back to the IR-cut probe` | `daynight.irprobe_cmd` is missing or failing. | Install the script, or accept audible probes. |
| `'<cmd>' failed <N> times - retiring the silent probe for this session; the trend trigger goes with it, leaving the jump trigger and the heartbeat` | The silent probe is permanently disabled until restart. | Fix `daynight.irprobe_cmd`. |
| `'<cmd> <arg>' failed (rc=…) - is the script installed?` | `daynight.switch_cmd` (or the probe command) is not installed/executable. | Install it. **Without a working `switch_cmd` the IR-cut filter never moves**, which looks exactly like "day/night doesn't work". |
| `'<cmd> <arg>' (pid N) did not finish in time - killing it. A board hook must not block the detection thread` | The hook is slow. | Make the script return promptly. |
| `'<cmd> <arg>' (pid N) survived SIGKILL - abandoning it. The board hook is stuck in the kernel (I2C/GPIO driver or a dead mount); day/night continues without it` | A kernel-level wedge (I²C, GPIO, or a hung NFS mount inside the script). | Investigate the driver/mount. Day/night keeps running blind. |
| `daynight: fork failed: …` / `irprobe: fork failed: …` | Out of memory. | See §5.5 / reduce resolution. |
| `cannot start detection thread` | Thread creation failed at start-up — day/night is off for this run. | Restart; if it recurs, memory pressure. |

### 3.4 The ISP disagrees with the decided mode

Three escalating warnings, in order:

1. `ISP still reports <mode> <N> s after the switch to <other> (script and
   re-asserts all ran) - forcing one transition through <x>, the only thing a
   stuck ISP acts on` — timps forces a transition. Usually cures it.
2. `ISP still reports <mode> after a forced transition - giving up until the
   next mode change; the image does not match the decided mode <other>` —
   the ISP latch is stuck. A restart is the reliable cure.
3. `decided mode is <a> but the ISP has been rendering <b> for <N> s - not
   enforcing, this may be a manual override. To re-measure and resolve,
   request a probe: POST /control {"daynight":{"probe":1}}` — timps suspects
   a human did this on purpose and backs off. That POST is the documented
   way out.

`SetISPRunningMode(<mode>) failed (rc=…)` from `HAL_ING` means the SDK
refused the mode change outright — seen on some T41/GC5603 bring-ups.

### 3.5 Schedule mode

- `mode=schedule needs a calendar: set daynight.time_night_start/
  time_day_start, or daynight.sun_latitude/sun_longitude, and make sure the
  clock is set` — and the second half matters: **a camera without NTP has no
  usable clock**, so schedule mode cannot work before time sync.
- `mode=schedule but no usable calendar … - forcing nothing` — same, seen at
  run time.
- `both a time window (<a>..<b>) and a location (<lat>/<lon>) are configured
  - the time window wins and the sun settings are ignored. Clear one of the
  two so the active schedule is not a matter of precedence` — configure
  **either** `time_night_start`/`time_day_start` **or**
  `sun_latitude`/`sun_longitude`, never both.

`daynight.mode` is deliberately **not** POST-able through the generic table;
`control.c` validates it by hand and logs
`ignoring daynight.mode = '<x>' (not auto/schedule)` for anything else.

### 3.6 Day/night tracing

`daynight.trace_path` is a **config-file-only** key (`F_NOGET` — not
POST-able, not echoed). Two guards:

- `trace_path <p> is not under /tmp, /run or /dev/shm - tracing to flash
  wears it out` — always trace to tmpfs.
- `cannot open trace_path <p>: … - tracing disabled until the path changes`.

For a live look without a restart, prefer:

```sh
curl -s -X POST -H "X-Timps-Token: $T" http://<cam>:8880/control \
     -d '{"general":{"debug_modules":"daynight"}}'
```

and `daynight.diagnose_thresholds = 1`, which makes the machine report an
unreachable `day_gain` instead of silently never switching.

### 3.7 AE integration-time cap (`image.ae_it_max_us`)

Three failure logs, all from `HAL_ING`:

- `image.ae_it_max_us: GetExpr gave no line/max reference (…) - cannot
  convert microseconds to sensor lines, cap not applied` — the sensor does
  not report a line time. The key cannot work on this camera.
- `image.ae_it_max_us=<n>: SDK rejected the cap (<lines>, rc=…) - AE maximum
  unchanged`.
- `image.ae_it_max_us=<n>: <k> writes on a live, delivering pipeline and the
  AE maximum is still <lines> lines - this sensor/ISP is not honouring the
  cap; retrying slowly` — the sensor ignores it. Do not keep raising the
  value; it will not help.

`image.ae_it_max_us=<n> … is above the sensor mode's own maximum … - nothing
to cap` (LOGI) means the value is larger than what the sensor can do anyway.
Range is 0..1000000 µs; 0 = off.

### 3.8 Image knobs that silently do nothing on this SoC

Every `image.*` key is accepted and persisted on every SoC; the HAL skips
what the platform cannot do. Whether a key *exists* here is announced by
`GET /control`'s `caps` object, built from the same `ISP_HAS_*` matrix
(`src/isp_caps.h`) that `hal_ingenic.c` guards its calls with. Keys that are
platform-gated: `hue`, `ae_compensation`, `max_again`/`max_dgain`,
`sinter_strength`/`temper_strength`, `dpc_strength`, `defog_strength`,
`drc_strength`, `highlight_depress`, `backlight_compensation`,
`core_wb_mode`/`wb_rgain`/`wb_bgain`, `ae_it_max_us`.

**So "I set `image.defog_strength` and nothing happened" is answered by
`caps`, not by trying harder.**

`IMP_ISP_EnableTuning failed - image tuning unavailable` means **no**
`image.*` key will do anything this run.

---

## 4. Streaming per protocol

Shared facts:

- **Every listener binds `INADDR_ANY`.** `src/net.c`'s `net_listen_tcp()`
  takes no address; there is **no `http.bind` / `rtsp.bind` key anywhere in
  timps.** Do not advise one. Restrict access with the firewall.
- **Per-source subscriber cap `HUB_MAX_SUBS` = 16** (`src/hub.h`). Past it,
  `hub_subscribe()` fails and the protocol layer answers "busy".
- Socket timeouts are `net_set_timeouts(fd, 30, 15)`: 30 s receive, **15 s
  send**. A client that stops reading is dropped after 15 s with
  `… send failed after <N>s (no TCP progress for 15s, SO_SNDTIMEO - peer
  stopped reading) - dropping client; the write is torn mid-frame` — the peer
  logs a truncated tail, which is expected, not corruption.

### 4.1 RTSP

`src/rtsp/rtsp.c`. Response codes a user can actually hit:

| Code | Cause | What the user should do |
| --- | --- | --- |
| `401 Unauthorized` (Digest + Basic challenge, realm `timps`) | `rtsp.user` is set. A fresh nonce is minted on every challenge; RTSP Digest additionally binds the nonce to **this connection**, so a replayed header fails. | Supply credentials. VLC prompts. |
| `404 Not Found` on DESCRIBE/SETUP | **No video stream is boot-enabled at all.** A path that merely does not match is *not* a 404 — see the note below. **The connection is closed.** | Check `videoN.enabled`. |
| `406 Not Acceptable` | `SETUP trackID=2` (ONVIF backchannel) when the backchannel is not available. | `audio.backchannel = 1` **and restart** (see §6.4). |
| `454 Session Not Found` | The `Session:` header does not match this connection's session. | Usually a client bug or a stale TEARDOWN. The running stream is *not* torn down. |
| `455 Method Not Valid in This State` | `PLAY` before any `SETUP`. Connection closed. | Client bug. |
| `461 Unsupported Transport` | No `Transport:` header, `client_port=0`, an unparseable port, or a multicast-only request. Connection kept. | timps supports RTP/AVP UDP unicast and RTP/AVP/TCP interleaved. **Multicast is not supported.** VLC falls back to TCP by itself. |
| `405 Method Not Allowed` with `Allow: OPTIONS, GET_PARAMETER, TEARDOWN` | `PAUSE` or a mid-session `DESCRIBE` while playing. Media keeps running. | **timps does not implement PAUSE.** The player's pause button gets a clean 405 instead of hanging. |
| `500 Internal Server Error` | The server could not bind a UDP port pair after 64 tries in 6000–14190. Connection closed. | Something else holds the whole range, or the box is out of sockets. Use TCP interleaved. |
| `503 Service Unavailable` with `Retry-After: 1` on DESCRIBE | No SPS/PPS after a 2 s warm-up — the encoder has not produced parameter sets yet. | Retry. Persistent → §2.2. |
| `503 Service Unavailable` on PLAY | `hub_subscribe()` failed: the source is at `HUB_MAX_SUBS` = 16. Log: `subscribe failed (source full), closing session=…` | Close other viewers. |
| Raw `RTSP/1.0 503 Service Unavailable` with **no CSeq**, socket closed immediately | `RTSP_MAX_CLIENTS` = **8** concurrent clients reached. Log: `client limit (8) reached, rejecting`. | Close viewers, or rebuild with `-DRTSP_MAX_CLIENTS=N`. |
| `551 Option not supported` | A `Require:` tag other than `backchannel`. | Client bug. |

> **At the RTSP client limit the server answers a bare
> `RTSP/1.0 503 Service Unavailable`** and closes the socket. Some older
> documentation and source comments claimed `453 (Not Enough Bandwidth)`;
> that was never what `src/rtsp/rtsp.c` sends.

**A wrong RTSP path does not 404 — it silently serves ch0.**
`find_video_by_path()` matches each **boot-enabled** stream's `rtsp_path` as a
prefix and, failing that, **falls back to the first boot-enabled stream**. So
`rtsp://<cam>/typo` plays the main stream, and `rtsp://<cam>/ch1` on a camera
with `video1.enabled = 0` also plays the main stream — at main-stream
resolution and bitrate. A user reporting "my substream URL gives me the full
1080p feed" has found this, not a resolution bug. Confirm with the SDP's
`a=framesize`/dimensions or `GET /control`'s `eff_width`/`eff_height`.

**SDP facts worth knowing** when a client behaves oddly:

- Track numbering is fixed: **`trackID=0` video, `trackID=1` audio,
  `trackID=2` the ONVIF backchannel** (`a=sendonly`, only when the backchannel
  was enabled at **boot**).
- The audio track is `mpeg4-generic` for AAC (with the RFC 3640 `config=`
  parameter), `opus/48000/2` on a `USE_STREAM_OPUS` build, or the **static**
  payload types **0 (PCMU) / 8 (PCMA)** for G.711.
- `a=range:npt=now-` marks the stream live and unbounded (RFC 2326 A.3), and
  `a=control:*` is the aggregate control URL.
- Responses carry an **explicit `Content-Base`** (the request URL,
  `/`-terminated), so a client resolves `a=control:trackID=N` against that
  rather than guessing a base. A client that ignores `Content-Base` and uses
  the full `rtsp[s]://host:port/chN` URL as the path is the classic way to end
  up on the wrong stream via the fallback above.
- UDP unicast server ports are an even/odd pair picked at random in
  **6000..14190** (even base), retried up to 64 times; exhaustion is the
  `500 Internal Server Error` row above.

**Connection dropped with no response at all** means one of: a request body
whose `Content-Length` would overflow the 4096-byte buffer; an incomplete
request pending longer than `RTSP_REQ_TIMEOUT_US` = **10 s** (log: `control
request incomplete after 10s, closing`); a bogus interleaved frame length; a
TLS handshake failure on the RTSPS port.

**Session reaping:** `RTSP_SESSION_TIMEOUT_S` = 60 is advertised as
`;timeout=60`; sessions are reaped at **2× = 120 s** of idleness, and only
for UDP transports. Log: `session=… idle >120s (client gone without
TEARDOWN), reaping`. A TCP-interleaved client that vanishes is noticed by
the failing send instead.

**Choppy RTSP / drops over a VPN or WiFi:**
`session=… chn=N: send queue overflowed, dropping frames (client/network too
slow) - details at DEBUG` (`MS_RTSP_QCAP` = 64). Lower `videoN.bitrate`,
prefer TCP interleaved, and check `rtsp.mtu` — the default **1200** is
already the VPN-safe value; 1400 is a LAN-only optimisation and will
fragment over most tunnels. Range is 548..1472.

**RTSPS (`rtsp.tls = 1`, `rtsp.tls_port`)** is a **second port**, and unlike
HTTPS it is **not fail-closed**: if the TLS context or the bind fails, plain
RTSP keeps serving and you get `RTSPS requested but TLS context failed -
plain RTSP only`, `cannot bind rtsps port <n>`, or `RTSPS requested but built
without USE_TLS`. Never assume RTSPS is active just because `rtsp.tls = 1`.

`cannot bind rtsp port <n>` on the main port means **no RTSP at all** —
`rtsp_start()` returns NULL. Almost always a port already in use (another
streamer still running).

`send_resp: response too large (hdr=…, body=…, cap=…), dropping` is
practically only reachable with an enormous SDP; report it as a bug.

### 4.2 HTTP: fMP4, MJPEG, snapshot, SSE

`src/mp4/httpd.c`. Ports and schemes are covered in §8.

| Response | Cause |
| --- | --- |
| `503 Service Unavailable` / `busy`, with `Access-Control-Allow-Origin: *` | `HTTP_MAX_CLIENTS` = **16** concurrent connections. Log: `connection limit (16) reached, rejecting client`. **`preview.html` alone holds 3+ slots per open tab** (the media stream plus two SSE subscriptions), so ~5 tabs is the real ceiling. |
| Connection closed with no bytes at all | Same cap, but on a TLS port where the peer already sent a ClientHello (a plaintext 503 into a TLS handshake looks like a TLS fault); or the 5 s header deadline expired; or the request line was malformed. |
| `404 Not Found` / `no jpeg` on `/snapshot.jpg`, `/stream.mjpeg`, `/mjpeg` | No JPEG source. An explicit `?chn=N` is **strict** — if that stream's `videoN.jpeg` is off you get 404, with no fallback to the other stream. |
| `503 Service Unavailable` / `busy` on a media path | The source is at `HUB_MAX_SUBS` = 16, or a snapshot grab collided with another. |
| `503 Unavailable` / `no frame` on `/snapshot.jpg` (note the non-standard reason phrase) | Subscribed fine, but no JPEG arrived within `HUB_JPEG_GRAB_WAIT_MS` = 1500 ms (worst case ~3 s). The JPEG channel is stalled — see §2.2. |
| `404 Not Found` / `not found` | Unknown path. This one is sent **without** CORS headers, so a cross-origin fetch reports a CORS error rather than a 404. Don't be misled. |
| `500 Internal Server Error` / `player page too large` | The generated player HTML exceeded its 4096-byte buffer. |
| `503 Service Unavailable` / `busy` on `/events` | SSE cap reached: `events.max_clients` if > 0, otherwise `EVENTS_MAX_CLIENTS_DEF` = **8**. Log: `sse client limit (<n>) reached, rejecting`. |
| `404 Not Found` / `disabled` on `/events` | `events.enabled = 0`. |
| `403 Forbidden` / `local only` | See §8.2. |

**The browser preview goes black or freezes for one viewer.** Look for these,
in order:

1. `mp4 chn=N: send queue overflowed, dropping frames (client/network too
   slow) - details at DEBUG` — `MS_MP4_QCAP` = 64, drop high-water 48. With
   `http.adaptive_drop = 1` (the **default**) that one client freezes on its
   last frame and resumes at the next keyframe, instead of being fed a
   headless GOP. **This is the design, not a bug.** Setting
   `http.adaptive_drop = 0` restores the older behaviour where the queue
   overflow triggers an IDR request — which raises the bitrate for *every*
   viewer.
2. `mp4 chn=N: no packets for <N>s - encoder stall, dropping this client` —
   `MS_STREAM_STALL_US` = **60 s** with nothing from the encoder. The client
   is dropped so it reconnects. See §2.2.
3. `mp4 chn=N: no audio for <N>s but video still flowing (muted mid-stream?)
   - dropping this client so it reconnects video-only` —
   `MS_MP4_AUDIO_GAP_US` = **5 s**. The `moov` already declared an audio
   track, so the player would stall waiting for it. Usually someone set
   `audio.mute = 1` mid-stream. Expected behaviour: one reconnect, then
   video-only.
4. `no video params, abort mp4` — no SPS/PPS within a 2 s wait. The
   connection is closed **with no HTTP response at all**, so the browser
   reports a network error, not a status code.
5. `no AAC within warmup -> video-only mp4` — audio was configured but the
   AAC encoder produced nothing in time; the fragment is built without an
   audio track. See §6.
6. `dropped a corrupt video/audio fragment (OOM?)` — memory pressure.

**MJPEG specifics:** `MS_MJPEG_QCAP` = **2**, deliberately much smaller than
the fMP4 queue — every JPEG is independent, so a backlog only adds latency.
`mjpeg: no frames for <N>s - encoder stall, dropping this client` is the
same 60 s stall rule.

**SSE specifics:** `sse <name> event too large, dropped` means one event
overflowed its buffer. Keepalive comments arrive every 12 s; a client that
sees nothing for longer has really lost the connection.

At shutdown, `N connection thread(s) still live after a 500 ms drain -
leaking tls_ctx/h rather than risking a use-after-free on process exit` is
a deliberate, safe choice during teardown, not a leak to report.

### 4.3 WebRTC / WHEP

`src/webrtc/`, endpoint `/webrtc/whep`. Limits: `WEBRTC_MAX_SESSIONS` = **4**
(one UDP port per slot, `webrtc.port` … `webrtc.port_max`), setup deadline
30 s, idle timeout 30 s, MTU 1200, offer body cap `WEBRTC_BODY_MAX` = 16384.

| Response | Cause | Fix |
| --- | --- | --- |
| `404 Not Found` on the path itself (generic `not found`) | Built without `USE_WEBRTC`. | Rebuild with `BR2_PACKAGE_TIMPS_WEBRTC`. |
| `503` / `webrtc.enabled=0` | Compiled in, turned off. | `webrtc.enabled = 1` **and restart** (`webrtc.*` are start-up settings). |
| `404` / `disabled` | `webrtc_available()` is false: the **DTLS context could not be built** from `http.tls_cert` / `http.tls_key`. Boot LOGE: `webrtc.enabled=1 but no usable DTLS certificate (<cert> / <key>) - /webrtc/whep stays disabled`. | WebRTC needs a certificate *even on a plaintext port* — DTLS-SRTP is not optional. Generate one. |
| `426 Upgrade Required` / `tls required` | A plaintext POST on a port where TLS **is** configured and `webrtc.enabled < 2`. | Use `https://`, or set `webrtc.enabled = 2` to accept a plaintext POST deliberately. |
| `403 Forbidden` / `local only` | §8.2. | |
| `405 Method Not Allowed` / `POST or DELETE` | Wrong method. | |
| `404` / `not found` | `POST /webrtc/whep/<something>` — POST must address the bare endpoint. | |
| `404` / `no such session` | `DELETE` for an unknown id. | |
| `411` / `413` / `400` / `503`, body `bad offer` | Missing `Content-Length`, a `Transfer-Encoding` header, an offer over 16 KB, a short read within 5 s, or OOM. | |
| `400 Bad Request` / `unsupported offer` | See the list below. | |
| `503` / `busy` | No SPS/PPS after a 2 s warm-up; **all 4 session slots in use**; `cannot bind udp port <a>-<b>`; thread creation failed. | Close other viewers, or widen `webrtc.port`…`webrtc.port_max`. |
| `500` / `answer failed` | The SDP answer did not fit 8192 bytes, or the fmtp could not be built. | Report as a bug. |
| `201 Created` | Success. The `Location:` header carries the session id to DELETE later. | |

`400 unsupported offer` in detail — each has its own LOGW:

- `video<N> is not H264 - WebRTC carries H264 only` → **set the WebRTC
  channel's `videoN.codec = h264`.** H.265 will never work over WHEP here.
- `offer carries no H264 with packetization-mode=1`.
- `offer has no usable sha-256 a=fingerprint`.
- `offer wants a=setup:<x> - we can only be passive` (timps is ICE-lite and
  DTLS-passive; the offer must be `actpass` or `active`).
- No `video` m-section, or a plain `RTP/AVP` (non-`SAVP`) offer, or a missing
  `a=ice-ufrag:` / `a=ice-pwd:`.

**Session established but the picture stays black:**

- `<sid>: no ICE/DTLS completion within <N>s - closing` — the browser's ICE
  never reached us. Almost always a network path problem: timps is
  **ICE-lite with a single host candidate**, no STUN, no TURN, no
  mDNS-candidate resolution. **It only works on a directly reachable LAN
  path.** Do not advise "add a STUN server" — there is nowhere to configure
  one.
- `<sid>: peer certificate does not match the offer's a=fingerprint -
  dropping` — a MITM, or a browser that renegotiated.
- `<sid>: DTLS up but media setup failed`.
- `dtls handshake failed (-0x…)`, `peer presented no DTLS certificate`,
  `peer negotiated no usable SRTP profile (…)`, `peer requested an SRTP MKI
  (… bytes) - unsupported`, `no transform after handshake - cannot export
  srtp keys`, `srtp keying material export failed (-0x…)` — DTLS-SRTP
  negotiation failures, all in `src/webrtc/dtls.c`.

**Firefox never plays the stream; Chrome does.** **Still open.** The answer's
`profile-level-id` comes from the live SPS, and `videoN.profile` defaults to
**2 (High)**. Chrome offers no High-profile H.264 in its own SDP but accepts
the answer anyway and decodes from the in-band SPS; **Firefox is
baseline-only in its SDP and refuses.** Chrome/Chromium is the tested target.
A user who must use Firefox can try `videoN.profile = 0` on the WebRTC
channel (restart required) — **(that workaround is inference, not something
measured in this repo)**.

**No audio in a WHEP session:** WebRTC here carries **G.711 only**. Set
`audio.codec2 = pcmu` so the second encode exists. Note
`audio.codec2: <N>Hz capture cannot feed 8kHz G.711 - off` — the capture rate
must be resamplable to 8 kHz. LOGI `audio m-section answered with port 0
(hub codec …, no matching G.711 payload type in one BUNDLE)` is the "no
audio negotiated" case.

`<sid>: DELETE marked the session but it has not finished yet` — DELETE
returns 200 after waiting up to 600 ms. Harmless.

`N session(s) still running at stop - leaving the DTLS context allocated` —
shutdown-time, deliberate, like the HTTP one.

### 4.4 SRT

`src/srt.c`. Limits: `SRT_MAX_CLIENTS` = **8**.

**Client URL form** (listener mode, the default):

```sh
ffplay 'srt://<ip>:9000?streamid=<id>'
ffmpeg -passphrase '<10-79 chars>' -i 'srt://<ip>:9000?streamid=<id>' -c copy out.ts
```

`streamid` is only needed when `srt.streamid` is set, and then it must match
exactly. Note that ffmpeg takes the passphrase as its **own option**, not as a
URL parameter. The listener fans **one shared encoded stream** out to every
accepted caller — extra viewers cost bandwidth, not encoder work, and they all
see the same GOP boundaries. Live link quality is in `GET /control`'s `srt`
object (`connected`, `rtt_ms`, `bw_mbps`, `rate_mbps`, `retrans`, `loss`,
`drop`); **`stats_age_s = -1` means no sample has been taken yet**, so the
other numbers mean nothing at that point.

| Symptom | Cause | Fix |
| --- | --- | --- |
| Client's handshake is rejected | `srt.streamid` is configured and the caller's streamid is missing or different. Log: `rejecting caller with missing/wrong streamid`. ffmpeg says "Connection setup failure". | Set the same `streamid=` in the client URL. |
| SRT never starts, daemon otherwise fine | `SRTO_PASSPHRASE rejected (need 10-79 chars): … - refusing to run unencrypted` — the passphrase is too short or too long. **timps refuses rather than falling back to unencrypted.** | Use a 10–79 character `srt.passphrase`. |
| `srt.mode=caller but srt.host is empty - SRT disabled` | Caller mode without a destination. | Set `srt.host`. |
| `unknown srt.mode '<x>' - using listener` | Typo. Valid: `listener`, `caller`. | |
| `bind/listen on <port> failed: …` | Port in use. Only SRT dies; the rest of the daemon runs. | |
| `client limit (8) reached, rejecting` | Enforced **after** `srt_accept()`, so the client sees an accepted-then-closed connection with no message. | |
| `connection to <host>:<port> lost - reconnecting (quiet retries, backoff up to <N>s)` / `connect to … failed: … - retrying` | Caller mode, destination unreachable. Retries are quiet by design — the log does **not** spam. | Fix the destination. |
| `chn=N: no packets for <N>s - encoder stall, dropping this client` | Same 60 s stall rule as HTTP. | §2.2. |
| **No audio in the SRT stream** | `audio.enabled=1 but codec is <g711u/g711a/…> - SRT/MPEG-TS carries AAC only, stream is video-only` | Set `audio.codec = aac` (needs `USE_FAAC`). |
| Audio configured as AAC, still silent | `AAC rate <N> Hz has no ADTS index, SRT audio disabled` | Use a standard AAC rate (8000/11025/12000/16000/22050/24000/32000/44100/48000…). |

---

## 5. Recording, timelapse, storage

### 5.1 `{"record":{"active":1}}` returned 200 but nothing is recorded

**`200 OK` is not a promise that recording started.** Check
`GET /control`'s `record` object: `recording`, `write_errors`, `last_error`,
`motion_gate_enabled`, `manual_off`.

| Cause | Evidence | Fix |
| --- | --- | --- |
| `record.min_free_mb` is unreachable | `record.min_free_mb=<N> unreachable (only <F>MB free, <M>MB even if every existing recording were deleted) - refusing to record rather than empty the archive`, and `last_error` says the same. | Lower `record.min_free_mb`, or use a bigger volume. **This is deliberate:** the old behaviour deleted the whole archive and still missed the target. |
| `record.mode = motion` but motion is off, **and no manual latch is set** | `motion_gate_enabled` in the status, `manual_off` absent. This is the config-driven path (`record.enabled = 1`), not the `{"record":{"active":1}}` one — a manual `active:1` latch bypasses mode and motion entirely (see below). | `motion.enabled = 1` as well. |
| A manual stop latch is set | `manual_off` in the status. It **overrides the config**. | `POST /control {"record":{"active":1}}` or clear the latch. |
| Unsafe path | `unsafe record.dir/name ('..' or absolute name), not recording` (LOGE) | `record.name` must be **relative** and must not contain `..`. It is a `strftime` pattern under `<record.dir>/<host>/records/`. |
| The directory is not writable | `open <path>: <errno>` / `fdopen <path>: <errno>` | Mount it, check permissions. |

`pruned <file> (free <N> MB < <M>)` (LOGI) is the **normal** retention
behaviour — oldest recordings are deleted to keep `min_free_mb` free.

**How the three record switches actually combine** (`want_write()` in
`src/record.c`), because the precedence surprises people:

1. A manual **stop** latch (`{"record":{"active":0}}`) wins over everything —
   nothing is recorded, `manual_off` is set.
2. A manual **start** latch (`{"record":{"active":1}}`) wins over everything
   else — it records **continuously**, ignoring `record.enabled`,
   `record.mode` and `motion.enabled`.
3. Only with no latch (`active` omitted, or a negative value, which returns to
   config mode) do `record.enabled`, then `record.mode`, then
   `motion_recent()` decide.

### 5.2 Recordings exist but are broken or have gaps

| Log (`src/record.c`) | Meaning |
| --- | --- |
| `write <path>: <errno>` / `segment write failed (<e>), closing` | The volume went away mid-segment (SD card ejected, NFS server gone, disk full). The segment is closed. |
| `segment flush failed (<e>), closing` | Same class. |
| `segment close/sync failed: <e> (tail may be truncated)` | The final `fsync` failed — **the last seconds of that file may be unreadable.** |
| `segment name collision, wrote <other> instead` | Two segments resolved to the same name. Make `record.name` more specific (it is a `strftime` pattern — include `%S`). |
| `dropped a corrupt video/audio fragment while recording (OOM?)` | Memory pressure; that fragment is missing from the file. |
| `clip busy, skipped <name>` | A `record.clip` request arrived while another clip was still being written. |
| `clip: no frames for <name>` | The clip window contained no frames — the encoder was idle (see §2.1: on-demand encoding). |
| `record.pre_roll_s=<N> cannot be held: the pre-roll ring caps at ~<X>s for ch<n> (<B> kbps, <F> fps, <P> packets / <M> MB max) - actual pre-roll is shorter` | The pre-roll ring is bounded by packet count and bytes, not by seconds. A high bitrate buys fewer seconds. | 
| `chn=<n>: record queue overflowed, dropping frames (storage/consumer too slow) - details at DEBUG` | **since v1.9.19.** The recorder's own queue evicted packets — the storage could not keep up. Once per subscription; the per-60 s `HUB` summary (§2.3) counts the rest. |

**A gap in a recording is now a clean cut, not decoder residue
(since v1.9.19).** When the recorder's queue overflows, every
packet still queued up to the next keyframe references access units that are
not in the file. Up to and including v1.9.18 they were muxed anyway
(`w_got_key` is per *segment*, so it did not re-arm), and the recording
carried up to a full GOP of visible residue after the hole — measured on a
T31X with a blackholed NFS `record.dir`: ~1.2 s of OSD ghosting after a 28.4 s
gap. Since v1.9.19 the segment freezes on the drop and resumes at the next
keyframe, the same shape `http.adaptive_drop` already gave fMP4 clients.
Consequences worth stating:

- The **hole gets slightly longer** (it now ends at a keyframe, up to one GOP
  later) and what follows it is clean. This is the intended trade.
- Audio freezes with the video, so both tracks resume together.
- Segment rotation is unaffected — it can only fire on a keyframe, which is
  the packet that also ends the freeze.
- In `motion` mode a drop while buffering pre-roll **clears the ring**, so that
  event's clip starts at the trigger rather than writing across the hole.
- An eviction that hit only **audio** does none of this: the video GOP is
  intact, so freezing would throw away good video for nothing. It still counts
  as an overflow (`queue_drops`, the WARN, the summary).
- There is no key to turn this off, and a frozen recorder keeps asking for an
  IDR until it gets one.

**Network shares (NFS/CIFS) as `record.dir`** deserve a warning: every
failure above appears as an ordinary `write`/`fsync` error, and a hung mount
will block the recorder thread. Note also that a hung mount inside a
`daynight.switch_cmd` script produces the
`survived SIGKILL - abandoning it` warning in §3.3. Prefer a local SD card;
if a share must be used, mount it `soft` with a timeout. **(The specific NFS
behaviour is inference — the code sees only generic I/O errors.)**

### 5.3 Recordings have no audio

`record.audio=1 but audio codec is <g711u/g711a/unknown/none|disabled> -
recordings are video-only; AAC (build with USE_FAAC=1) required`.
fMP4 carries AAC. Set `audio.codec = aac`; if that degrades to PCMU, the
build lacks `USE_FAAC` (see §6.1).

### 5.4 Timelapse

`src/timelapse.c`:

- `unsafe timelapse.dir/name ('..' or absolute name), skipping shot` — same
  path rule as recording.
- `open <path>: <e>` / `write <path>: <e>` — storage problem.
- `no frame from src=<n> within <N> ms - retrying in <N>s` — **the JPEG
  source produced nothing.** Timelapse is "just-in-time": it subscribes only
  when a shot is due. Check that `videoN.jpeg = true` (or `jpeg.enabled`) and
  that the JPEG channel is healthy (§2.2).
- `thread` (LOGE) — thread creation failed at start-up; timelapse is off.
- `timelapse.interval_s` is 1..INT_MAX; `keep_days` 0 = keep forever.

### 5.5 Out-of-memory symptoms

These are the lines that mean *the board is out of RAM*, not that a feature
is misconfigured:

`no memory for AU packet`, `no memory for JPEG packet`,
`sw-rot chnN: no memory for YuvEncode out buf`,
`videoN.jpeg: no memory for SW-rotate JPEG buf`,
`sw-rot chnN: VbmAlloc <N> failed (rmem exhausted?)`,
`control_apply_json: OOM`, `daynight history: cannot allocate <N> samples,
keeping <M>`, `daynight: fork failed`, `irprobe: fork failed`,
`on_motion: cannot launch '<cmd>': <e> - motion event dropped (the hook
itself is fine; the board is out of memory)`, `dropped a corrupt … fragment
(OOM?)`.

Levers, in order of effect: lower `videoN.width`/`height`, lower
`videoN.buffers` (1..8), disable the second stream, disable JPEG on a
stream, drop `daynight.history_s`, turn off software rotation.

---

## 6. Audio and backchannel

### 6.1 No audio anywhere

Work down this list:

1. `audio.enabled` must be 1 (default 1) and `audio.mute` 0.
2. `audio input unavailable` (LOGE) or `no audio frames received - disabling
   audio input` (LOGE) — the SoC AI never delivered. The mic is not wired, or
   the AI could not be opened: look for `IMP_AI_Enable failed`,
   `IMP_AI_SetChnParam failed`, `IMP_AI_EnableChn failed`.
3. `IMP_AI_SetPubAttr <N>Hz failed -> falling back` /
   `audio.samplerate <N>Hz unsupported by AI -> falling back` — the capture
   layer accepts **only 8000 and 16000 Hz** (`ai_rate_enum()` in
   `src/hal/hal_ingenic.c`), even though `audio.samplerate` clamps to
   8000..96000. The fallback order is configured → 16000 (AAC/Opus only) →
   8000. **Setting `audio.samplerate = 48000` does not give you 48 kHz
   audio.**
4. G.711 is **pinned to 8 kHz** regardless of `audio.samplerate` — otherwise
   the SDP would advertise 8 kHz while the AI ran at 16 kHz, giving
   double-speed playback.

### 6.2 Audio in one protocol but not another

This is by far the most common audio question. The matrix:

| Transport | Carries | Consequence |
| --- | --- | --- |
| RTSP / RTP | AAC, G.711 µ-law/A-law, Opus (only on `USE_STREAM_OPUS`) | Most flexible. |
| HTTP fMP4 (`/stream.mp4`, the browser preview) | **AAC only** | `audio.codec = pcmu` gives a silent preview. |
| SRT / MPEG-TS | **AAC only** | `audio.enabled=1 but codec is <x> - SRT/MPEG-TS carries AAC only, stream is video-only`. |
| WebRTC / WHEP | **G.711 only** | Needs `audio.codec2 = pcmu`. |
| Recording (fMP4) | **AAC only** | §5.3. |

So a camera set to `audio.codec = aac` + `audio.codec2 = pcmu` has audio
everywhere. A camera on G.711 alone has audio only in RTSP and WebRTC.

**AAC requires `USE_FAAC`, which is `0` by default in the upstream
`Makefile`** (the thingino package Kconfig `BR2_PACKAGE_TIMPS_FAAC` defaults
**y**, so firmware builds normally have it). Without it:
`no AAC encoder (build with USE_FAAC) -> using PCMU (G.711u)`. Also possible
at runtime: `faac_encoder_open failed (<e>) -> PCMU` and then
`faac fallback: reconfiguring AI <N>Hz -> 8000 for G.711`, and if even that
fails, `AI re-init at 8000 failed`.

Opus: `opus_encoder_create failed (<e>) -> PCMU`. Note that on a build
without `USE_STREAM_OPUS`, `audio.codec = opus` is **not** an error — it is
an unknown word and falls through to the default, **AAC**.

`unsupported AAC samplerate <N> Hz, using 16k index fallback` (`src/codec/
aac.c`) means the ADTS/ASC sample-rate index could not be derived; the
stream is tagged 16 kHz.

### 6.3 Stereo

`audio.channels = 2` / `audio.force_stereo = 1` is **simulated stereo** — the
mono mic duplicated to L=R. It requires AAC:
`audio.channels=2/force_stereo requires AAC - G.711 has no standard stereo,
staying mono`. That check also fires when an AAC request already degraded to
PCMU.

### 6.4 Talk-back (`/talk`) and the ONVIF backchannel

**`audio.backchannel` is read at boot only.** `bc_available()` reflects what
was configured when `timpsd` started, so **turning it on via `/control` does
not make `/talk` work** — `/talk` keeps answering `404 disabled` until a
restart. Same for the RTSP `trackID=2` backchannel, which answers
`406 Not Acceptable`.

`/talk` rejections (`src/mp4/httpd.c` then `src/rtsp/talk_ws.c`):

| Response | Cause | Fix |
| --- | --- | --- |
| `404` / `disabled` | `audio.talk_ws = 0`, or the backchannel was not configured at boot. | `audio.backchannel = 1`, `audio.talk_ws = 1`, restart. |
| `426 Upgrade Required` / `tls required` | `audio.talk_ws = 1` on a plaintext connection. | Serve over `https://`, or set `audio.talk_ws = 2` to accept plain `ws://` deliberately. |
| `403` / `local only` | §8.2. | |
| `405` / `GET required`, `400` / `not a websocket upgrade`, `426` / `websocket version 13 required` | Not a valid WebSocket handshake. | Client/proxy problem. |
| `400` / `bad rate` | `?rate=` must be one of 8000, 16000, 24000, 32000, 44100, 48000. Log: `refused: unsupported rate= in <path>`. | |
| `403` / `bad origin` | Log: `refused: cross-origin upgrade`. The `Origin` host must match the `Host` host (**ports are deliberately not compared**, because the WebUI is served by uhttpd on a different port). An `Origin` of `null` (sandboxed iframe) is refused; **no** `Origin` at all is allowed. | Serve the talk page from the camera's own hostname/IP. |
| WebSocket close **1008** `speaker busy` | Another talker holds the speaker. Log: `another talker holds the speaker - closing`. The owner is stolen only after 10 s of silence (`backchannel owner idle >10s - releasing speaker for queued playback`). | Wait, or close the other session. |
| WebSocket close **1001** `idle` | 10 s with no frames. | |
| WebSocket close **1003** `binary frames only` / **1009** | Client sent the wrong frame type, or a payload over 1024 bytes. | |

**`getUserMedia()` is refused by the browser on the talk page.** This is a
browser rule (secure context), not a timps rule. `audio.talk_ws = 2` only
tells *timps* to accept `ws://`; it cannot lift the browser's requirement.
Serve the page over HTTPS, or use the browser's insecure-origin override.

`AACInitDecoder failed` (`MOD bc`) means the AAC backchannel decoder could
not start — `USE_BC_AAC` builds only.

### 6.5 Speaker and local playback

- `audio.spk_enabled` is the master gate (default 1). On a build without
  `USE_PLAY` / `USE_BACKCHANNEL` there is **no AO pipeline at all**, and
  `spk_volume` / `spk_gain` / `aec` lose their `caps` entry.
- `audio output (speaker) unavailable` (LOGE) preceded by
  `IMP_AO_SetPubAttr <N>Hz failed -> falling back`, `IMP_AO_Enable failed`,
  `IMP_AO_EnableChn failed` — the board may simply have no speaker wired.
  Playback rates the AO accepts: 8000, 16000, 24000, 32000, 44100, 48000,
  96000.
- `IMP_AO_SendFrame failed` — a transient write failure.
- `speaker play: rejected '<path>'` (`MOD CTRL`) — the filename failed the
  path check. `play: cannot open <path>: <e>`, `play: bad WAV header in
  <path>`, `play: <path> is Opus but USE_PLAY_OPUS not built`,
  `play: op_open_file(<path>) failed (<n>)`.
- `mkfifo <path> failed: <e> - play queue disabled` /
  `open <path> failed: <e> - play queue disabled` /
  `cannot start play-FIFO thread` — the `/run/timps/audio_out` FIFO could not
  be created. `/run` full or read-only.
- **The FIFO protocol**, if `/usr/sbin/play` is missing or you need to test the
  daemon side directly: it is one line per command, mode 0660.

  ```sh
  echo 'PLAY url=/usr/share/sounds/chime_1.wav vol=80 gain=20 loop=2' > /run/timps/audio_out
  echo 'STOP' > /run/timps/audio_out
  ```

  Recognised tokens: `url=` (**absolute path**, required), `format=`, `vol=`,
  `gain=`, `rate=`, `loop=` (clamped 1..32), `delay=` (ms between loops).
  `append=` is accepted and **ignored** — the queue is a single latest-wins
  slot, not a playlist, so a new `PLAY` aborts the one in progress. The
  backchannel preempts either way.
- The **backchannel always preempts** the play queue for the speaker.

### 6.6 Microphone sounds noisy after using talk-back

Known and expected. `IMP_AI_EnableAec` brings up the vendor's full WebRTC
audio-processing chain on the mic — configured by `/etc/webrtc_profile.ini`,
**not** by any timps key — and it has been observed to leave the mic audibly
noisier until `timpsd` is restarted. `audio.aec` defaults to **0**. If a user
does not need echo cancellation, leave it off.

`IMP_AI_EnableAec failed - continuing without echo cancellation` means the
vendor DSP could not be engaged; on size-reduced images `libaudioProcess.so`
may have been stubbed out, which silently costs `aec`, `ns`, `agc` and
`high_pass`.

### 6.7 `high_pass` / `agc` / `ns` accepted but nothing changes

These four (`audio.high_pass`, `audio.agc`, `audio.agc_target_dbfs`,
`audio.agc_compression_db`, `audio.ns`) are **deliberately not live** even
though they are POST-able. libimp processes them on its own record thread and
frees them unlocked, so a live toggle would race the vendor thread. They
apply at the next AI bring-up, i.e. **at restart**.

Live audio keys are only: `volume`, `gain`, `alc_gain`, `mute`, and (on the
speaker side) `spk_volume`, `spk_gain`.

`audio.alc_gain unsupported on this platform (ignored)` — `IMP_AI_SetAlcGain`
exists on **T21, T31, C100** only. (A T10 build compiles against the T20
headers, which do not declare it.)

---

## 7. Motion

### 7.1 No motion events at all

1. `motion.enabled` defaults to **0**. It has to be turned on.
2. `IMP_IVS move detection not available in this build - motion detection
   disabled` — `MOTION_AVAILABLE` is 0 because the SDK has no
   `<imp/imp_ivs_move.h>`. `caps.motion.available` reports 0 and the WebUI
   greys the feature out. Nothing to configure.
3. `all <N> cells privacy-masked - motion not started` — **every** grid cell
   sits under a privacy region. Shrink the masks or the grid.
4. Bring-up LOGEs: `CreateGroup failed`, `CreateMoveInterface failed`,
   `CreateChn failed`, `RegisterChn failed`, `Bind FS->IVS failed`,
   `StartRecvPic failed`, `motion thread create failed`.
5. Check `motion.monitor_stream` points at an **enabled** stream. It is a
   `T_CHAN` field: anything outside 0..1 silently becomes **0**.

On success you get `motion detection started (WxH grid CxR=N cells,
sense=<s>/4, max <m>, hold=<t>ms)`.

### 7.2 Motion stalls

- `no IVS move results for over <N>ms - cycling the move channel to recover`
  (LOGE) then either `IVS move results resumed after a stall` (LOGI) or
  `IVS StartRecvPic failed during stall-recovery cycle` (LOGE).
- `motion.stalled = 1` in the status means a recovery cycle ran.
- `IMP_IVS_GetParam failed - falling back to full IVS rebuild` /
  `IMP_IVS_SetParam failed - falling back to full IVS rebuild` — a live
  sensitivity change could not be applied in place; a rebuild was done
  instead. Cosmetic.

### 7.3 Too many or too few events

- `motion.sensitivity` is 0..255 in the config but is mapped to the SDK's
  **0..4** (`sensitivity * 4 / 255`). So 0–63 → 0, 64–127 → 1, 128–191 → 2,
  192–254 → 3, 255 → 4. **Small changes around, say, 130 do nothing.** Move
  in steps of ~64.
- `motion.skip_frames` (≥ 1, default 5 when unset) trades latency for CPU.
  Lower for snappier detection, raise for cheaper.
- `motion.hold_ms` is how long a detected state is held. Raise it if the grid
  overlay misses single-frame motion.
- `motion.cols × motion.rows` is clamped to `MOTION_CELL_LIMIT` (52 on
  essentially every current SDK). The axis **being set** is clamped against
  the current other axis, so set them in the right order or set both.
  Defaults are 5×5.

### 7.4 The `on_motion` hook

`motion.on_motion` and `motion.cooldown_ms` are **config-file only**, by
design — they are a security boundary and carry no `F_CTRL` flag, so a POST
containing them is not applied. They **do** appear in the reply's `ignored`
array (`ign_note()` reports any member of a walked section that lacks
`F_CTRL`), so `{"motion":{"on_motion":"/x"}}` answers `200` with
`"ignored":["motion.on_motion"]` — the request is not silently swallowed.
Hand-edit `/etc/timps.conf` and restart.

The hook is run **without a shell and with no arguments**. So
`motion.on_motion = /usr/sbin/timps-motion --foo` will not work; put the
arguments inside the script.

**Since v1.9.10 it must be an absolute path.** The launcher moved from
`fork()` + `execlp()` to `posix_spawn()` (which was the fix for motion hooks
failing with `ENOMEM`, or getting `timpsd` OOM-killed, on low-RAM boards —
the double `fork()` copied ~97 MB of address space twice per event).
`posix_spawn` uses `execve`, so **there is no `PATH` search**: a bare command
name that used to work now silently fails. The wiki still describes
`execlp()` — that documentation is stale.

| Log (`src/hal/imp_motion.c`) | Meaning |
| --- | --- |
| `on_motion '<cmd>' cannot be executed - is the script installed and executable?` | Missing, not `+x`, or a bad interpreter line. |
| `on_motion '<cmd>' killed by signal <N> (out of memory?)` | The hook was killed. |
| `on_motion: <N> '<cmd>' hooks still running - skipping this event; the hook outlives motion.cooldown_ms` | **All `MOTION_HOOK_MAX` = 8 concurrent hook slots are busy** — `hooks_slot()` returned −1. The message's own wording blames `cooldown_ms`, and a hook slower than the cooldown is the usual way to get there, but the condition is the slot table, not the cooldown: 8 hooks must be in flight at once. Warned once per spell. **Raise `motion.cooldown_ms` or make the script faster** — events are being dropped. `cooldown_ms` has a floor of 250 ms. |
| `on_motion: cannot launch '<cmd>': <e> - motion event dropped (the hook itself is fine; the board is out of memory)` | `posix_spawn()` itself failed (not the exec). §5.5. |

Note the thingino package installs `/usr/sbin/timps-motion`
**unconditionally**, precisely because the shipped `timps.conf` points
`motion.on_motion` at it — otherwise every motion event would `posix_spawn()` a
missing script and log the "cannot be executed" warning above. (The comment in
`package/timps/timps.mk` still says `system()`; that text predates the
`posix_spawn` change and is stale.)

### 7.5 `motion.roi_*` does nothing

`motion.roi_* is deprecated and IGNORED - use the motion grid
(motion.cols/rows + cells) instead`. The single-ROI keys were replaced by the
cell grid. See §1.6.

---

## 8. Auth, network, TLS

### 8.1 What authentication actually exists

Verified in `src/auth.c`, `src/auth.h` and the gate in `src/mp4/httpd.c`:

- **HTTP Basic** and **HTTP Digest** (RFC 7616 `qop="auth"` plus legacy
  RFC 2069), realm `timps`, MD5.
- **RTSP Digest**, additionally bound to the nonce issued on *that*
  connection, so a sniffed `Authorization` header cannot be replayed.
- **A token** for `/control`, `/events`, the media paths, `/talk` and
  `/webrtc/whep`: header `X-Timps-Token:` or `?token=` in the query string
  (the query form exists because `EventSource`, `<img>`, `<video src>` and
  the `WebSocket` constructor cannot set headers). Two values are accepted:
  the per-boot 128-bit random token published to `http.token_file`
  (default **`/run/timps.token`**, mode 0640) and the optional persistent
  `http.token`. The token **never** unlocks RTSP.
- **Loopback bypass:** any peer in **127.0.0.0/8** skips authentication
  entirely. Hard-coded, not configurable — this is how the on-device WebUI
  CGIs reach the daemon.

**Things that do not exist. Never advise them:**

- No session cookie, no `Set-Cookie`, no `Secure`/`HttpOnly`/`SameSite`
  handling, no login endpoint, no logout.
- No CSRF token or header.
- **No login rate limiting, no lockout, no 429.** `auth_fail_note()` is
  *logging only*: from the 3rd failure, at most one line per minute,
  forgotten after 10 minutes of quiet. The line reads `<N> failed login
  attempts on <listener> since the last report (last from <peer>)`. A user who
  sees it is being scanned; it does not mean anything was blocked.
- **No `auth_bypass` key.** The equivalents are the loopback rule and the
  empty-credentials case below.
- **No `http.bind` / `rtsp.bind` key.** Everything binds all interfaces.

### 8.2 "403 Forbidden — local only"

`/control`, `/events`, `/talk` and `/webrtc/whep` answer `403 Forbidden` with
the body `local only` when **all three** are true: the peer is not loopback,
no valid token was presented, and **no credentials are configured**
(`http.user`, falling back to `rtsp.user`, is empty).

So: a camera with no username set is not "wide open on /control" — it is
**closed to the network** for those four endpoints and reachable only from
the camera itself or with the token. Conversely, the *media* endpoints
(`/stream.mp4`, `/snapshot.jpg`, `/stream.mjpeg`, the player page) **are**
fully public in that configuration, because `http_check_auth()` returns
success immediately when no user is configured.

> **That empty-credentials case is *not* what a thingino camera ships with.**
> `package/timps/files/timps.conf` sets `rtsp.user = thingino`,
> `rtsp.pass = thingino`, `http.user = thingino`, `http.pass = thingino`. So on
> a stock image:
> * `/control`, `/events`, `/talk`, `/webrtc/whep` answer **401**, not 403 —
>   credentials *are* configured, they are just the fleet-wide defaults;
> * the media endpoints are **not** public — they demand Basic/Digest too;
> * and **every camera on the network has the same password**. That is the
>   pitfall worth raising unprompted whenever a camera is reachable from
>   anything but a trusted VLAN. Change `http.user`/`http.pass` and
>   `rtsp.user`/`rtsp.pass` in `/etc/timps.conf` (they are file-only, §8.2's
>   last paragraph) and restart.
>
> A camera that really does answer `403 local only` has had its credentials
> **cleared** — someone edited the file, or it is running a config that
> predates the shipped template.

The two ways to fix a 403 from a LAN host:

```sh
T=$(ssh root@<cam> cat /run/timps.token)
curl -s -H "X-Timps-Token: $T" http://<cam>:8880/control
```

or set `http.user` + `http.pass` (or `rtsp.user` + `rtsp.pass`) and use
Basic/Digest. Note the password source follows the same fallback: if
`http.user` is empty, **both** user and password come from `rtsp.*`.

`http.user`, `http.pass`, `http.token`, `http.token_file`, `rtsp.user` and
`rtsp.pass` are **not** `F_CTRL` — they cannot be changed through
`POST /control`. Edit `/etc/timps.conf` and restart.

### 8.3 "401 Unauthorized" that keeps coming back

The 401 carries both a Digest and a Basic challenge. If the client loops:

- `stale=true` in the `WWW-Authenticate` header means the digest was
  cryptographically **correct** but the nonce was expired, evicted or
  replayed. The nonce table holds **32** entries with a **300 s** TTL, and
  under `qop=auth` the `nc` counter must strictly increase. A client that
  reuses an `nc` gets a fresh challenge. Normal clients retry silently.
- No `stale=true` means the credentials are actually wrong.
- RTSP mints a **fresh nonce on every challenge**, so a client that caches
  one will fail; VLC handles this correctly.

### 8.4 HTTPS vs HTTP on the same port — the tri-state

`http.https` is **not a boolean** (v1.9.11 changed the meaning of `1`):

| Value | Behaviour |
| --- | --- |
| `0` | Plaintext only. No TLS context, no sniffing. A **TLS client** gets its ClientHello parsed as a request line, which fails — the socket is closed with **no reply at all** ("empty reply from server"). |
| `1` | **Both schemes on the one port**, chosen per connection by peeking the first byte (`0x16` ⇒ TLS). Boot log: `HTTP and HTTPS both served on port <n> (http.https=1, scheme detected per connection)`. |
| `2` | TLS only. A plaintext request gets `426 Upgrade Required` with `Upgrade: TLS/1.2, HTTP/1.1` and the body `https required`. Boot log: `HTTPS only on port <n> (http.https=2, plaintext refused)`. |

Two failure modes to recognise instantly:

- **The port refuses every connection (ECONNREFUSED).** `http.https > 0` but
  the TLS context could not be built:
  `http.https=<n> but the TLS context could not be built from cert <c> / key
  <k> … REFUSING to serve port <p> at all rather than silently downgrading it
  to plaintext and leaking credentials.` **The listener is never bound.** Fix
  or regenerate the cert/key. This is fail-closed on purpose — unlike RTSPS,
  which degrades to plain RTSP.
  The message also offers `http.https = 0`. Treat that as **an exposure, not a
  fix**: it turns the port back to plaintext, so every Basic/Digest
  `Authorization` header and every `?token=` on the media and `/control` URLs
  crosses the network in the clear — and `?token=` in a query string is exactly
  the value that ends up in proxy logs. It also disables
  `webrtc.enabled = 1` (the WHEP signalling POST then `426`s) and
  `audio.talk_ws = 1` (`/talk` `426`s), both of which require TLS on that port.
  Reach for it only on an isolated segment, and say out loud that it is a
  downgrade.
- **The port serves plaintext although `http.https` is set.**
  `http.https=<n> but this build has no USE_TLS - port <p> serves PLAIN HTTP
  and any password or token sent to it goes over the network in the clear.`
  Rebuild with `BR2_PACKAGE_TIMPS_TLS`.

And the silent one: a typo in the value. `pbool()` reads anything it does not
recognise as `0`, so `http.https = ture` is plaintext. This is why
`F_SECVAL` exists — see §1.5 for the warning it prints (added in **v1.9.12**;
before that there was no warning at all).

> **Upgrade hazard.** `http.https = 1` **changed meaning in v1.9.11**. It used
> to mean *TLS only*; it now means *both schemes*. There is no warning and no
> migration. An operator who hand-configured `1` (or `true`/`on`/`yes`) for
> strict TLS is, after the upgrade, **also accepting plaintext on that port**
> — every Basic/Digest header and `?token=` in the clear. **They must change
> the value to `2`.**

**Which shipped scripts survive `http.https = 2`.** Several on-camera helpers
parse `http.https` themselves to decide how to reach the daemon over loopback,
and not all of them know the tri-state. Checked against
`package/timps/files/`:

| Handles `2` correctly | Breaks on `2` |
| --- | --- |
| `timps-selftest.sh` (`case 1 \| 2 \| true \| yes \| on`) | `/usr/sbin/color` — `case` has no `2` arm, falls back to `http://`, every `running_mode` POST gets a **426** and logs `POST … failed - color mode NOT set (timpsd down?)`. **This breaks day/night.** |
| `www/x/timps-imp.cgi` (same `case`) | `www/x/ch0.jpg` (and its `ch1`/`dl0`/`dl1`/`onvif/image*.cgi` aliases) — no `2` arm; every snapshot answers `502 snapshot unavailable`. |
| | `www/x/timps-heartbeat.sh` — no `2` arm; the WebUI heartbeat goes blank. |
| | `send2common` — `copy_photo` hardcodes `http://127.0.0.1:8880`. |
| | `timps-motion` — reads `http.port` but hardcodes `http://`, so both the snapshot and the `record.clip` POST fail. |

So on `http.https = 2` expect: no day/night switching, no WebUI snapshots, no
heartbeat, and motion notifications without media. **`http.https = 1` avoids
all of it** (both schemes on the one port), which is why the build ships `1`.
Earlier editions of this file had this table backwards — `timps-selftest.sh`
and `timps-imp.cgi` are the two that *do* handle `2`.

Connections that just close with nothing: the 5 s header deadline expired
(`MS_HTTP_SNIFF_MS` = 5000 ms for the first byte), or the request line was
malformed. Note the request line is parsed with `%7s %255s` — **a URI over
255 characters is truncated and then 404s.**

### 8.5 Certificate warnings in the browser

`src/tls.c` boot-time failures:

| LOGE / LOGW | Meaning |
| --- | --- |
| `cannot parse cert <path>` / `cannot parse key <path>` | Wrong file, wrong format, or empty. |
| `cert/key pair rejected (<c> / <k>)` | The key does not belong to the certificate. |
| `ctr_drbg seed failed` | No entropy source. |
| `ssl config defaults failed` | mbedTLS build problem. |
| `session ticket setup failed - no resumption on this listener` | Harmless; handshakes are just slower. |

Handshake-time, and this is the one users paste:

> `handshake failed: peer <ip> sent a fatal alert (-0x…, <N> more suppressed)
> - the CLIENT rejected our certificate (self-signed, or the name/IP it
> connected to is not in the cert SAN); our TLS config is not at fault`

Read it literally. The camera is fine; the **browser** refused the
certificate. Causes, in order of frequency:

1. Self-signed certificate, never trusted. A browser tracks trust for a
   self-signed certificate **per origin — scheme + host + port**, so trusting
   the WebUI on :443 does **not** trust timps on :8880.
2. The user connected by IP but the certificate only has a DNS SAN (or vice
   versa).
3. A `fetch()`/XHR failure gives **no click-through prompt at all** in some
   browsers (Safari on iOS especially), so the preview just fails silently
   while the page itself loads. The thingino init script therefore symlinks
   timps's cert/key at uhttpd's, so both ports present the *same* identity.

`handshake failed with peer <ip> (-0x…, <N> more suppressed) - rejected/
expired cert or TLS config problem?` is the generic twin.

The "`<N> more suppressed`" counter exists because a scanner would otherwise
flood the 64 KB syslog ring.

### 8.6 Port collisions and bind failures

| Log | Consequence |
| --- | --- |
| `cannot bind http port <n>` | `httpd_start()` returns NULL — **no HTTP at all**, and therefore no `/control`, no WebUI bridge. |
| `cannot bind rtsp port <n>` | No RTSP at all. |
| `cannot bind rtsps port <n>` | RTSPS only; plain RTSP keeps working. |
| `bind/listen on <n> failed` (SRT) | Only SRT dies. |
| `cannot bind udp port <a>-<b>` (WebRTC) | That WHEP session answers 503 `busy`. |

The usual cause is another streamer still running. Check `pidof prudynt` /
`pidof raptor`, and see §11.2.

---

## 9. WebUI and plugins

The WebUI is not part of timps. It is `thingino-webui`, and timps only
contributes a plugin manifest plus a set of pages, CGIs and JS. The
assembly chain is:

`package/timps/files/timps.webui.json` → `/var/www/a/plugins/timps.webui.json`
→ `assemble_plugins.py` at **global target-finalize** → `/var/www/a/plugins.js`
→ `navigation.js` reads `window.thinginoUIConfig.plugins`.

### 9.1 A menu entry 404s

`assemble_plugins.py`'s `validate_manifests()` **never touches the
filesystem** — the `"pages"` and `"cgi"` lists are cross-manifest validation
only. A page listed in the manifest but not actually installed produces **no
build error** and a nav entry that 404s at runtime. Check that the `.html`
really exists under `/var/www/`.

Real instance: `var/www/x/restart-prudynt.cgi` carries prudynt's *name* but
is timps's own replacement file, and an unconditional `rm -f` of "stock
prudynt files" deleted it — the WebUI's **Restart button 404'd on every image
from 2026-08-16 until 2026-08-24**. A quick check on a live camera:
`curl .../x/restart-prudynt.cgi` should return **401 auth-required**, not
404.

### 9.2 A page is blank, or shows the wrong (stock) content

This is the **per-package-directories merge** problem, and it is the single
most important firmware-level gotcha in the package.

With `BR2_PER_PACKAGE_DIRECTORIES=y`, every package installs into its own
`per-package/<pkg>/target/` copy, and target-finalize merges them with
`rsync --files-from=-` fed by the package list reversed with `tac`. rsync
keeps the **first** occurrence of a path, so for a colliding file the
**alphabetically last package wins**. `timps` only beats packages sorting
before it. Nothing fails; the per-package tree is correct and only the merged
image is wrong.

Practical rules:

- `rm` inside `$(TARGET_DIR)` is undone by target-finalize.
- timps re-applies its five forked WebUI files
  (`preview.html`, `tool-sensor-data.html`, `a/tool-sensor-data.js`,
  `x/json-heartbeat.cgi`, `x/json-heartbeat-slow.cgi`) from a
  target-finalize hook precisely because of this.
- **Never `scp` a raw `package/*/files/www/*.html` onto a camera.** Those
  files are un-assembled (~300 lines); `assemble_plugins.py` only runs at
  target-finalize and is what injects the plugin script tags (PTZ, motion,
  talk, …) that turn them into the ~2000-line built pages. Rebuild the
  package, run target-finalize, and copy from
  `$(O)/target/var/www/` instead.

### 9.3 A menu entry appears twice, or in the wrong place

- `assemble_plugins.py` prints `WARNING: Label '<x>' not found for 'after:',
  appending` when an anchor label was renamed upstream. The entry silently
  moves to the end of its section.
- Duplicated entries are deliberate: timps installs
  `/config-photosensing.html` and `/config-privacy.html` but does **not**
  claim them in its manifest, because `thingino-daynightd` and
  `thingino-privacy-lite` already claim those paths and a second claim would
  be a **hard build failure** (`PluginError`, exit 1). The accepted cost is a
  possible double entry on a combined image.
- Two plugins claiming the same **CGI** is only a stderr warning, never
  fatal.

### 9.4 The preview page shows a PTZ joystick on a camera with no motors

Stock `S48webui-config` reports `device.motors=true` whenever
`/etc/thingino.json` merely *has* a `motors` key — and the common config
ships a disabled placeholder on every board. The timps package installs its
own `S48webui-config` to fix that. If a non-PTZ camera shows the joystick,
that fix did not land (see §9.2).

### 9.5 Buttons do nothing / the page cannot talk to the daemon

The WebUI reaches timps through `/control`. Check, in order:

1. `curl http://127.0.0.1:8880/control` **on the camera** — loopback bypasses
   auth, so this must work.
2. From a browser on the LAN it needs the token. `x/timps-token.cgi` is what
   supplies it to the page; if that CGI is missing (§9.2) every control call
   fails.
3. The preview page and timps are on **different ports**, so CORS is in play.
   timps sends `Access-Control-Allow-Origin` on every 2xx/4xx answer and on
   the `OPTIONS` preflight — but **not** on the generic `404 not found` and
   (before the fix) not on the early connection-cap rejection. A "CORS error"
   in the console can therefore really be a 404 or a 503.

---

## 10. PTZ interplay (thingino-motors)

**timps does not drive motors.** There is no PTZ code in `src/`. Pan/tilt
comes from the separate `thingino-motors` package (`/usr/bin/motors`, the
`S59motor` init script and a kernel module), and the WebUI joystick talks to
that, not to timps. So a "PTZ not working" report is almost never a timps
problem — but timps is involved in three ways.

**1. The joystick appears on a camera without motors.** See §9.4.

**2. "The camera moves but I see it late."** Measured on a Wuuk Y0510 / T31
(`dev_notes/PTZ_LATENCY_2026-08-29.md`), all four streams open
simultaneously, n = 24:

| Stream | mean | median | min | max |
| --- | --- | --- | --- | --- |
| RTSP main (ch0) | 588 ms | 541 ms | 386 ms | 1049 ms |
| RTSP sub (ch1) | 1220 ms | 1279 ms | 799 ms | 1604 ms |
| HTTP fMP4 | 690 ms | 548 ms | 360 ms | 3668 ms |
| HTTP MJPEG | 501 ms | 501 ms | 368 ms | 639 ms |

`motors -b` (the on-device "motor is moving" flag) flips at a mean of 71 ms,
so most of the rest is the sensor → ISP → encode → network → decode
pipeline. **The sub-stream is consistently ~700 ms slower than the main
stream** (B-frames ruled out; deeper encoder buffering at the low bitrate is
the working hypothesis, *not verified*). If a user complains about joystick
lag while watching a substream preview, moving them to the main stream or to
MJPEG is the cheap fix. Note the caveat in that document: an external NVR
(Frigate) may have been holding a fifth session open during the measurement,
and RTSP latency is sensitive to the consumer mix.

**3. Command-line gotcha:** `motors -x <n> -y <n>` alone **does nothing** —
it needs the `-d` direction verb, e.g. `motors -d h -x 2630 -y 0`.

**4. Do not restart the motor module while the camera is live.** A
`modprobe -r` / reload of the motor module on a running camera has been
observed to produce a kernel Oops. Use a full `reboot` for motor
configuration changes rather than `S59motor restart`.

**5. `invert_x` / `invert_y` double-toggle.** A suspected upstream bug in
`S59motor` where the two settings toggle each other; the report is drafted
but **not yet confirmed on hardware** — treat as **(unverified)**.

---

## 11. Build, flash, OTA

All paths here are in the thingino firmware tree, not in the timps repo.

### 11.1 "Feature X does not exist on my camera"

Distinguish **compiled out** from **turned off** before anything else:

```sh
curl -s http://127.0.0.1:8880/control | head -c 2000    # caps, *.available
curl -s 'http://127.0.0.1:8880/control?fields=1'        # every POST-able key name
```

Kconfig symbols in `package/timps/Config.in` and their `USE_*` flags, with
the **package defaults** (which differ from the bare `Makefile` defaults):

| Kconfig | Default | `USE_*` | What disappears at 0 |
| --- | --- | --- | --- |
| `BR2_PACKAGE_TIMPS_FAAC` | y | `USE_FAAC` | AAC → no audio in the browser preview, in SRT, or in recordings |
| `…_CONTROL` | y | `USE_CONTROL` | `/control` and the `?token=` auth path |
| `…_DAYNIGHT` | y | `USE_DAYNIGHT` | the day/night thread |
| `…_RECORD` | y | `USE_RECORD` | recording (`caps.record.available = 0`), ~11 KB |
| `…_TIMELAPSE` | y | `USE_TIMELAPSE` | timelapse, ~4 KB |
| `…_TLS` | y | `USE_TLS` | HTTPS + RTSPS |
| `…_WEBRTC` | y (needs TLS + CONTROL) | `USE_WEBRTC` | `/webrtc/whep` |
| `…_SRT` | **n** | `USE_SRT` | SRT output |
| `…_ROTATE` | **n** | `USE_ROTATE` | **all** rotation; `caps.rotation` is omitted entirely |
| `…_SW_ROTATE` | **n** (needs ROTATE) | `USE_SW_ROTATE` | T23 software 90/270 |
| `…_OSD_HINTING` | **n** | `USE_OSD_HINTING` | OSD autohinting (~2.1 KB) |
| `…_BACKCHANNEL` | **n** | `USE_BACKCHANNEL` | ONVIF backchannel, and the AO pipeline |
| `…_BC_AAC` | **n** | `USE_BC_AAC` | AAC backchannel decode |
| `…_BC_WS` | y **if** `…_TLS` | `USE_BC_WS` | `/talk` |
| `…_PLAY` | **n** | `USE_PLAY` | the `/usr/sbin/play` queue |
| `…_PLAY_OPUS` | **n** | `USE_PLAY_OPUS` | Opus playback |
| `…_STREAM_OPUS` | **n** | `USE_STREAM_OPUS` | Opus as an RTSP codec |
| `…_DIAG_TOOLS` | **n** | — | `/usr/bin/timps-{selftest,logcat-ship,dn-isp-log}` |

`USE_TRACE` has **no Kconfig symbol on purpose** — it is
`make … TIMPS_TRACE=1 rebuild-timps` only, and the daemon announces it
loudly: `send-pipeline tracing ENABLED (mask=0x… ) threshold=<N>ms - this is
a debug build option, leave it off in normal operation`.

### 11.2 Two streamers, or the wrong streamer

The streamer is a Kconfig **choice** in
`package/thingino-streamer/Config.in` (`_NONE`, `_PRUDYNT`, `_RAPTOR`,
`_STRERO`, `_TIMPS`), so **two streamers cannot be enabled at once** — the
old "conflicts with prudynt-t" caveat is gone. The real hazards are leftovers:

- **`thingino-daynightd` survives a switch to timps.** It is selected only by
  the prudynt choice and has no `depends on` of its own, so a stale `=y`
  survives `olddefconfig`. Two day/night engines then fight over the IR-cut
  filter. `timps.mk` installs a `TIMPS_DISABLE_DAYNIGHTD` finalize hook that
  globs `rm -f /etc/init.d/*daynightd /etc/init.d/*dusk2dawn` — note it had
  to become a glob after a 2.0.0 rename (`S97daynightd` → `S10daynightd`)
  silently broke the old literal name.
- **The inverse:** on a clean timps image `/usr/sbin/{daynight,ircut,light}`
  are not built at all — they live in `thingino-daynightd/files/`. The
  recorded incident (Garage, T31/SC4336P, 2026-08-12) is exactly the
  purple-image case from §3.2: **every `switch_cmd` invocation failed with
  `rc=127`, the IR-cut filter was never removed, total gain climbed from
  3500 to 22000+ over minutes, and the image went purple/IR-tinted.**
  `timps.mk` now installs those three scripts from the daynightd package's
  own `files/`. If a user's log shows `'<cmd> <arg>' failed (rc=…) - is the
  script installed?`, check `ls -l /usr/sbin/daynight /usr/sbin/ircut`.
- A stale `S96onvif_discovery` from another streamer makes ONVIF credentials
  come back empty, because the raptor/prudynt copy builds `/etc/onvif.json`
  from `raptorctl`. timps ships a replacement that reads `/etc/timps.conf`
  instead — but **it is not installed unconditionally**. It comes from a
  target-finalize hook (`TIMPS_INSTALL_ONVIF_DISCOVERY`) that **probes the
  target** and installs only if `usr/sbin/wsd_simple_server`,
  `var/www/onvif/onvif.cgi` or an existing `S96onvif_discovery` is there.
  Deliberately *not* gated on `BR2_PACKAGE_THINGINO_ONVIF` (which is often
  "not set" on images that do have ONVIF), but on a genuinely ONVIF-free image
  nothing is shipped at all.
  At **run** time the script also exits early: with `/run/portal_mode` present,
  or when the default gateway does not answer a 1-second ping, it prints
  `Disabled` and **exits 1**. So "ONVIF discovery says Disabled" on a camera
  that is still bringing up WiFi is normal, not a misconfiguration.
- **Building from a bare worktree without `THINGINO_USER_DIR`** silently
  regenerates `.config` from the board defconfig — which for most boards
  defaults to **PRUDYNT**, not timps.

### 11.3 A change to `.config` disappeared

Three mechanisms, all in the firmware `Makefile`:

1. `force-config` starts with `rm -rvf $(OUTPUT_DIR)/.config`. **Manual edits
   are deleted, not merged.**
2. `rebuild-%` depends on `force-config`, so **`make rebuild-timps`
   regenerates `.config` first** — the most common way people lose a
   hand-edit.
3. `check-config` regenerates whenever any config input file is newer than
   `.config`. A manual edit updates `.config`'s mtime and therefore
   *suppresses* regeneration — until the next time an input file is touched,
   at which point the edit vanishes with no warning.

**Per-camera settings belong in `user/<camera>/local.fragment`** (or
`user/common/local.fragment`, or `user/<camera>/<IP>/local.fragment` when
`IP=` is set). They are concatenated onto `.config` in that order, before
`olddefconfig`. Note the fragment loop uses plain `cat`, so **template
variables such as `$(SOC_FAMILY)` are NOT substituted in a user
`local.fragment`** (unlike the `# FRAG:` fragments and the camera defconfig).
`$(OUTPUT_DIR)/.config_original` is the pre-`olddefconfig` snapshot — use it
to see what `olddefconfig` dropped.

Kconfig can also drop things silently: a **recursive dependency** (a `select`
that loops back into a `choice` default) makes kconfig ignore the involved
defaults *without an error*, and in one reproduced case `olddefconfig` even
flipped `BR2_PACKAGE_THINGINO_UHTTPD_TLS_MBEDTLS` to "not set". This is why
`TIMPS_WEBRTC` and `TIMPS_BC_WS` use `depends on` rather than re-`select`ing
mbedTLS.

### 11.4 "I rebuilt but the camera still runs the old binary"

- Use `make CAMERA=… IP=… rebuild-timps`. `<pkg>-dirclean <pkg>` alone is
  **not** enough: `rebuild-<pkg>` also does `<pkg>-reinstall` and
  `target-finalize`, and those two were the actual gap.
- Do **not** `rm -rf` the output directory first — a source-only change needs
  only `rebuild-timps` on the *existing* output dir.
- `images/rootfs.squashfs` is **not** rebuilt when only a package changed
  (the Makefile's `ROOTFS_BIN: KERNEL_BIN` dependency does not track package
  changes). `rm -f images/rootfs.squashfs` before rebuilding to force it.
- **`GET /control`'s `version` can lie.** `timps.mk` computes `VERSION=` by
  running `git describe` against `TIMPS_OVERRIDE_SRCDIR` at parse time
  (Buildroot's rsync strips `.git` from the copy). If the override dir is not
  a git checkout, the binary reports the **pinned tag**, not your working
  tree. There is a recorded 2026-08 incident where `fw_ota.sh` reported
  "flashed successfully" on cameras whose `/usr/bin/timpsd` had demonstrably
  not changed.
- A targeted `rebuild-timps` / rootfs-squashfs shortcut **skips
  target-finalize**, which silently reverts live overlay edits to
  `thingino.json` / `timps.conf`. Verify `$(O)/target/etc/*`, not just the
  package binary.

### 11.5 Flash, partitions and OTA

- **The rootfs partition is not a fixed size.** It is cut to fit the image at
  U-Boot build time (`ROOTFS_SIZE_ALIGNED = round_up(size, 64 KiB)`), and the
  data partition gets whatever is left. A camera's "5120k rootfs" is the
  imprint of whatever image was on it the last time a **full** flash laid the
  partitions down — **not** a hardware limit. Before optimising an image for
  size, check what created the partition it has to fit.
- **The pack-time size checks are `printf` only. None of them is fatal** —
  the build exits 0 with an oversize image. Worse, `ROOTFS PARTITION
  OVERFLOW` is arithmetically **unreachable**, because the partition size is
  *defined* as the aligned binary size. The checks that can actually fire are
  `DATA PARTITION OVERFLOW`, `DATA PARTITION TOO SMALL FOR JFFS2` and
  `OVERSIZE`.
- **Always flash with `make ota`, never `ota-rootfs`, after a from-scratch
  build.** `ota-rootfs` refuses an oversized rootfs ("bigger than /dev/mtd4")
  only *after* it has already freed overlay space and remounted `/`
  read-only, and it writes the **data partition with no size check at all**.
  Measured 2026-08-20: a 10158080-byte `data.jffs2` was pushed at a camera
  whose data partition is 9280k — 655360 bytes too large. A
  `-m rootfs` after a from-scratch build has overflowed the data partition
  and broken SSH badly enough to need a physical power-cycle.
- **Flashing the rootfs replaces `/etc/timps.conf`.** Any switch set by hand
  on the camera is gone. `/etc/timps.conf` *is* in `overlay/etc/cfg-backup.list`,
  but the backup is only taken when `-B` is passed — `ota-upgrade` does,
  `ota-rootfs` / `ota-full` / `ota-kernel` / `ota-uboot` do **not**. The
  backup must also fit **one 64 KB erase block**.
- **squashfs rounds to 4096-byte blocks.** Below 4096 bytes the packed image
  cannot see a change at all — 39.4 KB of uncompressed JS removed bought
  exactly 4096 bytes. Do not size cleanups off `du`.
- `libstdc++.so.6.0.35` is 2130 KB, the single largest file. The
  `BR2_PACKAGE_TIMPS_DROP_LIBSTDCPP` option removes it only when a
  `readelf -d` sweep finds no user — but **that option is nested inside the
  WebUI+CONTROL block in `timps.mk`, so on an image without the WebUI (or
  with `TIMPS_CONTROL=n`) it silently does nothing.**
- **`libgcc_s.so.1` must stay.** uClibc `dlopen()`s it for `pthread_cancel`
  stack unwinding and there is no `DT_NEEDED` entry, so a link-time
  dependency scan proves nothing.
- On a 5 MB rootfs, **SRT and the audio filters are mutually exclusive**. With
  `libaudioProcess.so` stubbed out, `IMP_AI_EnableAec` fails, timps logs one
  WARN and carries on — a silent loss of `aec`, `ns`, `agc` and `high_pass`.

### 11.6 The init script

`/etc/init.d/S95timps` (runlevel 95 — after uhttpd's `S02ssl` and
`S48webui-config`, before `S96onvif_discovery`):

- It starts `timpsd` with `start-stop-daemon -S -b -m -p /var/run/timpsd.pid
  -x /usr/bin/timpsd -- -c /etc/timps.conf`. `timpsd` itself never forks; the
  `-b` is what backgrounds it. **There is no log redirection** — syslog is the
  only log.
- It provisions TLS certificates *before* starting the daemon, preferring
  symlinks to uhttpd's `/etc/ssl/certs/uhttpd.crt` + key so that both ports
  present the **same identity** (see §8.5), and falling back to
  `generate-timps-tls-certs.sh`. If the `ln -sf` fails it removes **both**
  paths (a half-made link would leave timpsd with a cert and no key) and logs
  `TLS: could not link …` / `TLS cert generation failed: …` through
  `logger -t timps` — i.e. under the tag **`timps`, not `timpsd`**, so a
  `logread | grep timpsd` misses them.
- **Regenerating the certificate: check what you are deleting first.** On a
  WebUI image `/etc/ssl/certs/timps.crt` and `/etc/ssl/private/timps.key` are
  normally **symlinks onto uhttpd's pair**. Deleting the *links* is safe —
  `ensure_tls_certs()` re-creates them on the next start. Deleting the
  *targets* (`uhttpd.crt` / `uhttpd.key`) takes **the WebUI** down with timps
  until uhttpd's own `S02ssl` regenerates them, which happens at boot, not
  immediately. `ls -l` before `rm`.
- **It only provisions a certificate when one of `http.https`, `rtsp.tls` or
  `webrtc.enabled` is *uncommented* in `/etc/timps.conf`.** On an image built
  with `TIMPS_TLS=y` + `TIMPS_WEBRTC=y` but **without**
  `BR2_PACKAGE_THINGINO_UHTTPD_TLS=y`, none of those lines is uncommented by
  the build-time `sed`, so no certificate is generated — and WHEP is compiled
  in, runtime-enabled, and dead with
  `webrtc.enabled=1 but no usable DTLS certificate`. **(Read from the
  sources; not confirmed end-to-end on a built image.)**
- **`wait_stop()` is not always "about 5 seconds".** It loops 50 times over
  `usleep 100000 2>/dev/null || sleep 1`. Where busybox `usleep` exists that is
  ~5 s; where it does not, the fallback is `sleep 1` × 50 = **50 s**, and a
  `S95timps stop` that appears to hang for the better part of a minute is this,
  not a wedged daemon.
- **`restart` has a verified defect:** `stop` deletes the pidfile
  unconditionally, even when its wait loop timed out and printed
  `timpsd still stopping...`. The `restart` target's
  second `wait_stop` then returns immediately because the pidfile is already
  gone — so a slow IMP/rmem teardown is followed straight away by a `start`
  on top of a still-exiting instance, which is exactly the ISP-init failure
  the wait was written to prevent. The singleton `flock` is then the only
  guard, and it refuses the **new** process
  (`another timpsd instance already holds …`, exit 1), leaving the camera dark
  with an orphaned, still-exiting `timpsd` and **no pidfile**. If a restart
  leaves the camera dark, stop, wait, and start as two separate commands, or
  reboot.
- `/etc/timps.conf` is installed at **build time**; there is no
  `timps.conf.example` and no first-boot seeding, despite what
  `package/timps/README.md` still says.

### 11.7 Upgrade targets — "upgrade to ≥ vX"

Latest release in `CHANGELOG.md` is **1.9.21 (2026-09-25)**. Anything this
file marks "since v1.9.22 (unreleased)" is only in `[Unreleased]` — quote that
phrasing, not a version number, until the tag exists. Two caveats before
quoting a released version at a user:

- `CHANGELOG.md` **jumps from `[1.2.0]` to `[1.5.0]`**. The 1.3.x/1.4.x fixes
  are documented only in `dev_notes/RELEASE_v1.3.4.md`, `RELEASE_v1.3.5.md`,
  `RELEASE_v1.4.4.md`, `RELEASE_v1.4.5.md`, `RELEASE_v1.4.6.md`.
- There is **no `1.9.4` and no `1.9.7` heading**. Never name those as targets.

| The complaint | Upgrade to ≥ |
| --- | --- |
| RTSP/HTTP advertise a **permanently silent audio track** | **1.3.5** |
| **Crash when toggling `audio.agc` / `ns` / `high_pass`** live | **1.4.5** (1.4.4 only mitigated it) |
| ONVIF fails to start / port clash on 8080 | **1.5.0** (default `http.port` moved to **8880**) |
| A played sound file is **cut short at the end** | **1.6.0** |
| **Sub-stream delivers zero video forever** after one large IDR | **1.6.1** |
| **Video stops after hours**, only a restart fixes it | **1.6.2** |
| **Browser preview lags 1–3 s**, worst during PTZ | **1.6.3** |
| **`/snapshot.jpg` returns "no frame"**, MJPEG/preview go dark | **1.6.4** |
| A rotation outside the safe envelope **took the whole pipeline down** | **1.7.0** |
| **False night latch after a reflash into daylight** | **1.7.1** |
| **Overnight day/night flap loop** | **1.7.4** |
| **Stuck in night (or day) after a reboot, zero log output** | **1.7.5** |
| **Motion silently stalls forever**; immortal RTSP sessions; HTTP/SRT spin on an encoder stall; `record.post_roll_s=0` recorded nothing | **1.7.6** |
| **"The IR-cut clicking at night is annoying"** (8–12 flips/night); IR-reflection flip loop | **1.7.7**, properly **1.9.1** |
| **`rc_mode=fixqp` crashed the streamer**; `min_qp`/`max_qp` had no effect on T31/C100/T40/T41; `vbr`/`smart` silently ran as CBR on classic SoCs; `codec=h265` on T10/T20 streamed H.264 **mislabelled as H.265**; `jpeg.quality` had no effect on classic SoCs | **1.8.1** |
| **Double-instance ISP collision** → zero video + endless watchdog recovery; snapshot "no frame" on a refused rotation; recordings/SDP/`/control` advertising **swapped W/H** | **1.8.0** |
| mpv "No video PTS!"; seconds of **A/V skew on a fresh `/stream.mp4`**; preview stayed dead after a backgrounded tab | **1.8.2** |
| **hflip/vflip not taking effect at boot** on T23 | **1.8.3** |
| **hflip/vflip/running_mode silently reverting hours after boot** | **1.8.4** |
| Timeline jumps every 15–25 s (mpv, ffmpeg — RTCP SR mapping). *"mpv self-pauses" is **(unverified)**: the 1.8.5 entry documents the timeline jump only.* | **1.8.5** |
| **IR video in broad daylight for hours**; **`videoN.gop` ran at DOUBLE the configured value on T31/C100/T40/T41**; rotated stream ran at double fps; fMP4 corrupted after a queue eviction | **1.9.0** |
| Slow-loris shapes in the **TLS handshake and the WebSocket frame read** (`tls.c`, `ws.c`) — a timeout a successful read renewed rather than spent. *An "RTSP slowloris" fix at 1.9.0 is **(unverified)**; the CHANGELOG records this class under 1.9.8 and not for RTSP.* | **1.9.8** |
| Stuck in night on a board with **no gain ceilings**; flapping 8–12× an evening on a dim room; **`libstdc++.so.6` (2.1 MB) on every SRT build** | **1.9.1** |
| **Half an hour of black-and-white in daylight while `/control` said "day"** | **1.9.2** |
| **Camera stays down after a restart** on memory-constrained T31s; **rebooted after dark → IR LEDs off all night** | **1.9.3** |
| **iOS Safari "Load failed"** after enabling HTTPS; **fMP4 permanently N s out of sync after a WiFi stall**; a hung `switch_cmd` froze day/night *and* shutdown; TLS warnings blamed the camera's own config | **1.9.5** |
| Camera whose dark rest state *is* an AE clip gets permanently stuck | **1.9.6** |
| **`record.min_free_mb` too high emptied the whole archive**; photosensing sliders silently did nothing; an abandoned probe left the illuminator off; **`record.pre_roll_s` capped at ~one GOP**; UDP backchannel accepted datagrams from any port on the peer's IP | **1.9.8** |
| `teardown: 2 IMP call(s) failed` on **100 % of clean shutdowns**; a keepalive client blocked every other talker forever; connection-cap 503 gave an opaque CORS error | **1.9.9** |
| **Motion hook `ENOMEM` / `timpsd` OOM-killed** on low-RAM boards; sensitivity slider snapped back on every refresh; **T41: no OSD overlay was ever drawn** | **1.9.10** |
| **Preview unusable when the page and the stream are on different schemes** | **1.9.11** (read the hazard in §8.4) |
| A killed tab / WiFi drop **busy-spun a core** on `/talk` (a TLS transport EOF without `close_notify` came back as `EAGAIN` in `ws.c`); **a typo in `http.https`/`rtsp.tls` silently downgraded the transport with no log**. *The specific "pinned the core for 3–10 s" figure is **(unverified)** — the CHANGELOG records the busy-spin, not a duration.* | **1.9.12** |
| WebRTC at all | **1.9.13**, and really **1.9.14** — before that every POST got a 426 and every session was video-only |

Two upgrade side effects to warn about:

- **≥ 1.9.0 halves the keyframe interval on T31/C100/T40/T41.** The GOP bug
  meant those SoCs ran at *double* the configured interval. Cameras now emit
  twice as many keyframes; a user relying on the old behaviour should **raise
  `videoN.gop`**.
- **`audio.gain`'s default churned:** 31 → **15** in 1.9.10 → back to **25**
  in 1.9.14. A volume change across upgrades is usually this.

---

## 12. Log-message dictionary

Every `LOGE` and `LOGW` string in `src/` is listed here, grouped by the
module tag that appears in the log line, plus the few `LOGI` lines users
actually paste (level **I**). Format placeholders (`%d`, `%s`, …) are quoted as
they appear in the source, so a pasted line can be matched by its fixed prefix.

Reading a timps log line: `<level> <MOD> <message>`. Get the log with
`logread | grep -i timps`; the daemon is backgrounded by the init script so
**stderr is discarded** and syslog is the only sink (`general.syslog`
defaults to on). `GET /control`'s `last_errors` holds a recent slice, which
matters because the 64 KB syslog ring recycles within hours.

To raise detail for one subsystem without drowning the ring:

```sh
curl -s -X POST -H "X-Timps-Token: $T" http://<cam>:8880/control \
     -d '{"general":{"debug_modules":"daynight"}}'
```

### 12.1 `MAIN` (`src/main.c`)

| Message pattern | Level | Meaning | Action |
| --- | --- | --- | --- |
| `sigaltstack failed (%s) - a stack-overflow fault won't be caught` | W | The alternate signal stack could not be installed. | Cosmetic unless the daemon later dies without a backtrace. |
| `cannot open %s (%s) - proceeding without the single-instance guard` | W | The lock file could not be opened; two instances are now possible. | Check `/run` is writable. |
| `another timpsd instance already holds %s - refusing to start (a second instance re-initializing the ISP would corrupt the running instance's video pipeline)` | E | Single-instance lock held. A second instance re-initialising the ISP would corrupt the first's pipeline. | Stop the other `timpsd`. §1.1 |
| `%s - AGAIN, after the one-shot recovery reboot already tried for this. A real reboot does not fix this board's problem either, so giving up for real (needs manual intervention, not another reboot)` | E | Bring-up failed again *after* the recovery reboot. | Manual intervention. **Do not keep power-cycling.** §1.2 |
| `%s, but the one-shot reboot marker %s cannot be written (%s) - without it every boot would take this same escalation path and reboot again, i.e. a silent reboot loop, so giving up permanently WITHOUT the escalation reboot. Fix the rootfs (/etc full, or mounted read-only?) to re-enable the one-shot recovery reboot` | E | `/etc` is full or read-only, so no recovery reboot is attempted (a boot loop would be worse). | Fix the rootfs. §1.2 |
| `%s - escalating to ONE reboot before giving up permanently (2026-08-22 T31 precedent: retries alone did not clear whatever the board was waiting on, only a real reboot did)` | E | The camera is about to reboot itself, once. | Wait a minute. §1.2 |
| `reboot() itself failed (%s) - giving up instead` | E | No permission, or a sandboxed init. | Reboot manually. |
| `cannot write token file %s` / `short write on token file %s` | W | `http.token_file` (default `/run/timps.token`) could not be written. | `/run` full or read-only; token auth from the WebUI will fail. §8.1 |
| `no HAL backend available` | E | Built without a usable HAL. | Rebuild. |
| `could not clear the startup-reboot marker %s (%s) - it is stale now, and while it sits there a future unrelated start failure will skip its one-shot recovery reboot. Remove it by hand` | W | A **stale** marker will make the next, unrelated failure skip its recovery reboot. | `rm /etc/timps-startup-reboot.flag`. |
| `HAL start failed (%d/%d) - unwinding and retrying in %ds` | E | Encoder/framesource bring-up failed. Budget is 10. | §1.1, §1.2 |
| `HAL init failed - retrying in %ds` | E | ISP/system init failed. Retries forever, backoff 5→60 s. | §1.1 |

### 12.2 `CONFIG` (`src/config.c`)

| Message pattern | Level | Meaning | Action |
| --- | --- | --- | --- |
| `rotation %d invalid -> 0` | W | Not 0/90/180/270 (or the legacy 1/2). | Use a valid value. §2.6 |
| `rotation %d unsupported on this SoC -> 0` | W | No 90/270 path here, or `USE_ROTATE` is off. | §2.6 |
| `rotation 180 unsupported on this SoC -> 0 (use image.hflip+image.vflip)` | W | 180 is real only on T40/T41. | `image.hflip = 1` + `image.vflip = 1`. |
| `codec h265 unsupported on this SoC -> h264` | W | T10/T20/T23 have no H.265 encoder. Coerced at parse time so the read-back matches what is emitted. | Use `h264`. |
| `audio.codec2 '%s' unsupported (pcmu or off) -> off` | W | `audio.codec2` takes `pcmu` or `off` only — deliberately not defaulting to AAC. | `audio.codec2 = pcmu`. |
| `%s%s = '%s' is not a valid value and was read as %d - that is PLAINTEXT, no TLS. Valid: %s` | W | A typo in `http.https` or `rtsp.tls`. **TLS is off.** | Fix the value (0/1/2). §1.5, §8.4 |
| `daynight.mode: unknown '%s', keeping auto` | W | Valid: `auto`, `schedule` (plus the legacy `sensor`/`time`/`sun`). | |
| `unknown osd item key %s` / `unknown privacy key %s` / `unknown video key %s` / `unknown key %s` | W | The key does not exist in this build. **It is dropped.** | Check `GET /control?fields=1`. |
| `videoN.max_gop is reserved and IGNORED - the keyframe interval comes from videoN.gop` | W | Compat-only key. | Use `videoN.gop`. §1.6 |
| `osd.hinting is stored but has no effect in this build (compiled without OSD text autohinting)` | W | Built without `USE_OSD_HINTING`. | §1.6 |
| `motion.roi_* is deprecated and IGNORED - use the motion grid` | W | Replaced by the cell grid. | §7.5 |
| `%s is obsolete and IGNORED - %s` | W | A day/night key retired by the 2026-08-17 / 2026-08-22 redesigns. The message names the replacement. | Delete the line. §1.7 |
| `daynight.%s=%s is now a fixed internal constant (%d) and can no longer be tuned per camera - the configured value is being ignored` | W | An int key frozen fleet-wide. Only warns when the configured value differs. | §1.7 |
| `daynight.%s=%s is now a fixed internal constant (%g) and can no longer be tuned per camera - the configured value is being ignored` | W | Same, for `ir_ratio_night` / `ir_ratio_day`. | §1.7 |
| `sensor.fps: driver max_fps=%ld, auto capped to %d (set sensor.fps to override)` | I | **since v1.9.19.** `sensor.fps` was left unset and the driver advertised more than the cap (GC2053 reports 40 on a 30 fps mode). The cap is 30, or since v1.9.20 the fastest enabled `videoN.fps` if higher; from v1.9.20 the message reads `sensor.fps: driver %s=%ld, auto capped to %d (set sensor.fps to override)`, `%s` being `max_fps` or `fps`. The cap applies to the autodetected value only. | Nothing, unless more is really wanted — then set `sensor.fps` explicitly. §1.10 |
| `config %s not found, using defaults` | W | No config file. Not fatal. | §1.3 |
| `config: line longer than %zu chars skipped (starts \"%.40s...\")` | W | A line over ~510 characters is **dropped whole**. | Shorten it. |
| `config: %s has an opening quote but no closing one - keeping the value verbatim, quotes included` | W | Unbalanced quote. | Fix the quoting. |
| `config sensor.model '%s' != loaded driver '%s' - using '%s' (the config value would crash the ISP)` | W | The kernel sensor driver wins. A mismatch would divide by zero in the kernel. | Fix `sensor.model`. §1.8 |
| `config sensor.i2c 0x%02x != loaded driver 0x%02lx - using 0x%02lx` | W | Same for the I²C address. | §1.8 |
| `config_write_keys: %d keys in one call, only the first %d are persisted (%s onwards stays live but unsaved)` | W | Over 64 keys in one persist call. | Split the request. §1.4 |
| `cannot create tmp for %s: %s` / `fdopen tmp failed` | W | The atomic-rewrite temp file could not be created. | Filesystem full or read-only. |
| `read error on %s (%s) - ABORTING the config rewrite so the truncated copy is not committed over it. The setting is live but NOT persisted; this flash needs attention` | E | **A bad flash block.** The old file is intact; the change is live only. | Take it seriously — the flash is failing. |
| `fsync %s failed: %s` / `rename %s -> %s failed` / `fsync dir %s failed: %s` | W | Durability steps of the atomic rewrite failed. A power cut can lose the change. | Check the filesystem. |

### 12.3 `CTRL` (`src/control.c`)

| Message pattern | Level | Meaning | Action |
| --- | --- | --- | --- |
| `too many settings in one request, %s not persisted` | W | More than `CTRL_MAX_CHG` = 48 **changed** keys in one POST; the tail is live but unsaved. | Split the request. §1.4 |
| `control_apply_json: OOM` | W | The ~9.6 KB change buffer could not be allocated; the POST answers 503 `oom`. | §5.5 |
| `speaker play: rejected '%s'` | W | The filename failed the path check. | Use a plain path under the sounds directory. |
| `ignoring daynight.mode = '%s' (not auto/schedule)` | W | `daynight.mode` is hand-validated, not table-driven. | Use `auto` or `schedule`. |

### 12.4 `HAL_ING` (`src/hal/hal_ingenic.c`) — bring-up and teardown

| Message pattern | Level | Meaning | Action |
| --- | --- | --- | --- |
| `IMP_OSD_SetPoolSize(%d KB) failed - OSD overlays may not composite` | W | The OSD memory pool could not be sized. | On T41 this used to mean no overlay was ever drawn — upgrade to ≥ 1.9.10. |
| `IMP_ISP_Open failed` | E | The ISP device could not be opened. | Another instance? §1.1 |
| `IMP_ISP_AddSensor failed (sensor=%s)` | E | The named sensor is not the loaded driver. | §1.8 |
| `IMP_ISP_EnableSensor failed (sensor=%s)` | E | Sensor power-up failed. | Check the sensor `.ko` and wiring. |
| `IMP_System_Init failed after %d tries` | E | The vendor system layer never came up. | Usually rmem still held by a previous instance. §1.2 |
| `IMP_System_Init busy, retry %d/5 in 1s (ISP still releasing?)` | W | Previous instance's rmem not yet released. | Normally clears itself. |
| `IMP_ISP_EnableTuning failed - image tuning unavailable` | W | **No `image.*` key will do anything this run.** | Restart. §3.8 |
| `sensor fps: requested %d, driver holds %u/%u, set rc=%d` | I | **since v1.9.19.** The sensor driver accepted the rate and reads it back unchanged. One line per bring-up. | Nothing. §1.10 |
| `sensor fps: requested %d, driver holds %u/%u (%.2f), set rc=%d` | W | **since v1.9.19.** The set call failed (`rc != 0`) or the driver holds a **different** rate than was asked for. The rate the driver holds is the real one; `GET /control` keeps echoing the request. | Set `sensor.fps` to what the driver will hold, or accept it. Not a config error if it persists. §1.10 |
| `sensor fps: requested %d, set rc=%d, readback unavailable (rc=%d)` | W | **since v1.9.19.** `IMP_ISP_Tuning_GetSensorFPS` failed or reported a zero denominator (the getter is called on every supported SoC). The set call may well have worked; only the verification is missing. | Driver-specific; not a config error. §1.10 |
| `cannot read isp_ch0_pre_dequeue_time - assuming the pre-dequeue one-buffer schedule is active on framechan0` | W | T31 only: the module parameter could not be read, so `nrVBs` is forced to 1. | Cosmetic. |
| `chn0: explicit buffers=%d but isp_ch0_pre_dequeue_time=%d forces a one-buffer schedule on framechan0 - EnableChn will fail (dmesg: 'one buffer schedule') unless pre-dequeue is disabled at the driver` | W | T31 with `isp_ch0_pre_dequeue_time > 0` **and** an explicit `videoN.buffers`. | **Delete the `videoN.buffers` line** (see §13). The real fps lever is removing `BR2_ISP_CH0_PRE_DEQUEUE_TIME` from the board defconfig — measured 13.5 → 24.9 fps. |
| `framesource %d: EnableChn failed%s (retry)` | E | The framesource channel would not enable. | See the line above; also §2.2. |
| `framesource %d: %d reference(s) still held at teardown` | W | Subscriber accounting leak at shutdown. | Report as a bug. |
| `FS_CreateChn %d` / `FS_SetChnAttr %d failed` | E | Framesource geometry rejected. | Check `videoN.width`/`height` against the sensor. |
| `FS_SetI2dAttr %d failed (rotation may stay inactive)` | W | T40/T41 belt-and-braces rotation call. | §2.6 |
| `Encoder_CreateGroup %d failed` / `Encoder_CreateChn %d` / `Encoder_RegisterChn %d to group %d failed` | E | Encoder bring-up failed. | Check resolution, codec and `rc_mode` against the SoC. |
| `IMP_Encoder_SetDefaultParam(chn%d) failed - the attr struct below is only partly filled; CreateChn will likely reject it` | W | Precedes a `CreateChn` failure. | Same. |
| `Bind fs%d->osd%d failed` / `Bind osd%d->enc%d failed` / `Bind fs%d->enc%d failed` / `JPEG Bind fs%d->enc%d failed` | E | Pipeline binding failed. | §2.2 |
| `video chn%d thread create failed` / `audio thread create failed` / `jpeg chn%d thread create failed (channel kept for teardown)` / `sw-rot chn%d thread create failed` | E | Out of memory or thread limits. | §5.5 |
| `video pipeline bring-up failed - tearing down partial state` | E | The summary line; the real cause is the LOGE above it. | §2.2 |
| `%s: %d IMP call(s) failed (first: %s) - the next start may fail ISP init` | W | Teardown did not fully clean up. | Expect a rocky next start; a reboot cures it. |
| `dedicated JPEG channel unavailable` | W | The standalone JPEG channel could not be created. | Use `videoN.jpeg` instead of `jpeg.enabled`. |
| `JPEG CreateChn %d failed` / `JPEG CreateGroup %d failed` / `JPEG RegisterChn %d failed` / `JPEG RegisterChn %d to group %d failed` | E | JPEG channel bring-up failed. | Check `jpeg.width`/`height`/`imp_chn`. |

### 12.5 `HAL_ING` — encoder, rate control, frames

| Message pattern | Level | Meaning | Action |
| --- | --- | --- | --- |
| `videoN.min_qp (%d) > max_qp (%d) - programming %d..%d` | W | Swapped. timps orders them rather than letting the SDK ignore both. | Fix the config. |
| `rc_mode capped_vbr/capped_quality has no classic-SoC equivalent -> using vbr` | W | Classic SoCs (T10–T30) have no capped modes. | §2.4 |
| `rc_mode smart has no new-API equivalent -> using capped_quality` | W | T31/C100/T40/T41 have no `smart`. | §2.4 |
| `encoder chn%d: %d kbps at %d fps implies ~%lu KB keyframes, near or above the %d KB AU buffer - keyframes will be dropped and clients may never get a decodable stream; lower videoN.bitrate or raise MS_AU_BUF_MAX` | W | **The livelock warning.** | Lower `videoN.bitrate`. §2.3 |
| `videoN.quality_lvl/change_pos/fluc_lvl: no equivalent field in this SoC's encoder API - values ignored` | W | Classic-SoC knobs on a new-API SoC. | §1.6 |
| `videoN.i_bias_lvl: this SoC's SDK has no IMP_Encoder_SetChnQpIPDelta - value ignored` | W | T40/T41. | §1.6 |
| `Encoder_SetChnQpBounds %d (%d..%d) failed - using SDK default range` | W | The SoC refused the QP bounds. | |
| `Encoder_SetChnQpIPDelta %d (%d) failed - using SDK default` | W | Same for the I/P QP delta. | |
| `chn%d: StartRecvPic failed (attempt %d)` / `jpeg chn%d: StartRecvPic failed (attempt %d)` | E | The encoder would not start receiving. | §2.2 |
| `chn%d: PollingStream idle (rc=%d, miss#%d) - encoder emits no frames` | W | Watchdog window with no output. | §2.2 |
| `chn%d: encoder dead after %d consecutive misses - forcing a framesource disable/enable cycle to recover (recovery attempt %d/%d)` | E | Automatic recovery, max 5. | §2.2 |
| `chn%d: %d consecutive forced-recovery cycles never produced a frame - encoder/ISP is not coming back on its own; exiting` | E | **The process exits and nothing restarts it.** | §2.2 |
| `jpeg chn%d: PollingStream idle (miss#%d) - encoder emits no frames` | W | Same on the JPEG channel. | |
| `jpeg chn%d: encoder dead after %d consecutive misses - forcing a framesource disable/enable cycle to recover (recovery attempt %d/%d)` | E | JPEG recovery. | |
| `jpeg chn%d: %d consecutive forced-recovery cycles never produced a frame - giving up on this channel (MJPEG/snapshot output disabled until restart)` | E | **JPEG only** is disabled; video survives. | Restart to get snapshots back. |
| `chn%d: GetStream failed after PollingStream OK` / `jpeg chn%d: GetStream failed after PollingStream OK` | W | Transient SDK hiccup. | Ignore unless constant. |
| `chn%d: AU exceeds max buffer (need=%zu, max=%d, packCount=%u) - dropping frame (%u dropped so far)` | W | An access unit over `MS_AU_BUF_MAX` (1 MB). Deliberately **no** IDR is forced — that was the historical permanent-stall bug. | §2.3. On T23 at 1080p this is expected at stock defaults; the proven lever is raising `video0.min_qp`. |
| `chn%d: no memory for AU packet (need=%zu) - dropping frame` / `chn%d: AU assembly overflow (cap=%zu, need=%zu, packCount=%u) - dropping frame` | W | OOM / assembly overflow. | §5.5 |
| `chn%d: JPEG exceeds max buffer (need=%zu, max=%d) - dropping frame` / `chn%d: no memory for JPEG packet (need=%zu) - dropping frame` / `chn%d: JPEG assembly overflow (cap=%zu, need=%zu) - dropping frame` | W | Same for JPEG (`MS_JPEG_BUF_MAX` 1 MB). | |
| `jpeg chn%d: empty stream (packCount=%u) - dropping frame (%d)` | W | The SDK returned a zero-pack stream. | |
| `snapshot %s: %s` | W | Writing the periodic snapshot file failed. | Check `jpeg.snapshot_path`. |
| `jpeg.quality=%d not applied: custom quantization tables are known to degrade JPEG quality on T10 (JPEG left at SDK default)` | W | T10 special case, mirroring prudynt-t. | Nothing to do. |
| `IMP_Encoder_SetJpegeQl(chn%d,q%d) failed (JPEG left at default)` | W | The quantization table was rejected. | |
| `live rc change on an H265 stream: the classic SDK's SetChnAttrRcMode is H264-only - applies on restart` | W | **H.265 rate control is restart-only on classic SoCs.** | Restart. §2.5 |
| `SetChnAttrRcMode chn%d failed - value applies on restart` | W | Live rc refused. | §2.5 |
| `SetChnBitRate chn%d failed - value applies on restart` | W | Live bitrate refused. | §2.5 |
| `SetChnQpBounds chn%d failed - value applies on restart` | W | Live QP bounds refused. | §2.5 |
| `SetChnQpIPDelta chn%d failed - value applies on restart` | W | Live I/P delta refused. | §2.5 |
| `SetChnAttrRcMode chn%d (fixqp qp) failed - value applies on restart` | W | Live fixed-QP refused. | §2.5 |

> **Encoder telemetry needs a client.** `encoder.<n>.rc` reflects the
> encoder's *live* rate-control state, which only refreshes when the channel
> actually encodes. On a camera nobody is watching, a live `min_qp`/`max_qp`/
> `bitrate` POST reads back frozen **forever** and looks like a failed apply.
> Measured on a T41LQ: 2 s of RTSP and the readback followed immediately,
> every time. **Always attach a client before trusting an encoder readback.**

### 12.6 `HAL_ING` — rotation (software path)

| Message pattern | Level | Meaning | Action |
| --- | --- | --- | --- |
| `video%d: refusing FS-rotate %dx%d not %d-aligned (T31 FS-rotate wants %d-alignment) - stream will run UNROTATED` | W | T31 needs 64-aligned geometry. | §2.6 |
| `video%d: refusing FS-rotate %dx%d@%d exceeds vendor FS-rotate cap (<=1280x704, <=15fps; …) - stream will run UNROTATED` | W | Past the vendor cap. | §2.6 |
| `video%d: FS-rotate enable (SetChnRotate) failed - disabling rotation for this stream, bringing it up UNROTATED` | W | The SDK call failed. | §2.6 |
| `video%d: SW rotate at %dx%d@%dfps is CPU-HEAVY on this SoC - strongly consider a substream-class setting (<=704x576, <=15fps)` | W | T23 software rotate; it still runs. Costs ~6 MB and roughly one CPU core. | Use a substream-class size. |
| `video%d: refusing SW rotate at %dx%d@%dfps (exceeds safe envelope <=704x576 <=15fps) - stream will run UNROTATED` | W | Past the envelope. | §2.6 |
| `video%d: SW rotate post-rotation width %d unusable for the encoder (needs picWidth%%16==0 and >=256; a 90/270 rotate makes encoder width = source height %d) - refusing rotation, stream will run UNROTATED (use a source HEIGHT that is a multiple of 16 and >=256, e.g. 704x576 -> 576x704)` | W | **The source height becomes the encoder width.** | Use 1280×704, not 1280×720. §2.6 |
| `video%d: SW rotate needs even dims (got %dx%d)` | E | Odd width or height. | |
| `sw-rot chn%d: IMP_Encoder_YuvInit %dx%d failed` | E | The unbound encoder would not initialise. | |
| `video%d: SW rotate init failed - disabling rotation for this stream, bringing it up UNROTATED` | W | Summary of the above. | |
| `sw-rot chn%d: VbmAlloc %u failed (rmem exhausted?)` | E | Out of reserved video memory. | §5.5 |
| `sw-rot chn%d: VbmV2P failed` | E | Virtual→physical mapping failed. | |
| `sw-rot chn%d: no memory for YuvEncode out buf (%u)` | E | OOM. | §5.5 |
| `sw-rot chn%d: SetFrameDepth failed` | W | Frame depth could not be set. | |
| `sw-rot chn%d: GetFrame delivered nothing (miss#%d)` | W | No frame from the framesource. | |
| `sw-rot chn%d: YuvEncode failed (miss#%d)` | W | The software encode failed. | |
| `sw-rot chn%d: JPEG (%d) exceeds buf (%u) - dropped` | W | JPEG over the (deliberately oversized) buffer. | |
| `sw-rot stream %d: OSD item %d is a logo - not composited on the SW-rotate path (text only)` | W | **No logos on the T23 SW-rotate path.** Also no privacy masks. | Move the logo to the unrotated stream. |
| `sw-rot stream %d item %d: rendered %dx%d exceeds frame %dx%d - skipped` | W | Text too big for the rotated frame; coordinates are in **rotated** space. | |
| `video%d.jpeg: rotated %dx%d not 32/8-aligned for InputJpege (need width%%32==0, height%%8==0; make source height a multiple of 32, e.g. 704) - JPEG disabled on this stream` | W | JPEG needs stricter alignment than the encoder. | §2.6 |
| `video%d.jpeg: no memory for SW-rotate JPEG buf (%u) - JPEG disabled on this stream` | W | OOM. | §5.5 |

### 12.7 `HAL_ING` — image / AE

| Message pattern | Level | Meaning | Action |
| --- | --- | --- | --- |
| `SetISPRunningMode(%s) failed (rc=%d)` | W | The SDK refused the day/night colour-pipeline change. Seen on some T41/GC5603 bring-ups. | §3.4 |
| `image.ae_it_max_us: GetExpr gave no line/max reference (%s) - cannot convert microseconds to sensor lines, cap not applied` | W | This sensor reports no line time; the key cannot work here. | §3.7 |
| `image.ae_it_max_us=%d: SDK rejected the cap (%lu lines, rc=%d) - AE maximum unchanged` | W | Rejected. | §3.7 |
| `image.ae_it_max_us=%d: %d writes on a live, delivering pipeline and the AE maximum is still %lu lines - this sensor/ISP is not honouring the cap; retrying slowly` | W | The sensor ignores it. | Stop raising the value. §3.7 |

### 12.8 `HAL_ING` — audio

| Message pattern | Level | Meaning | Action |
| --- | --- | --- | --- |
| `audio.samplerate %dHz unsupported by AI%s -> falling back` | W | Capture accepts **8000 and 16000 only**. | §6.1 |
| `IMP_AI_SetPubAttr %dHz failed%s -> falling back` | W | The AI rejected the rate. | §6.1 |
| `audio input unavailable` | E | No mic pipeline this run. | §6.1 |
| `IMP_AI_Enable failed` / `IMP_AI_SetChnParam failed` / `IMP_AI_EnableChn failed` | E | AI bring-up failed. | §6.1 |
| `no audio frames received - disabling audio input` | E | The AI reported success but never delivered. Audio is no longer advertised. | §6.1 |
| `audio.alc_gain unsupported on this platform (ignored)` | W | `IMP_AI_SetAlcGain` exists on T21/T31/C100 only. | §6.7 |
| `no AAC encoder (build with USE_FAAC) -> using PCMU (G.711u)` | W | No AAC in this build. | §6.2 |
| `faac_encoder_open failed (%s) -> PCMU` | W | The AAC encoder would not open. | §6.2 |
| `faac fallback: reconfiguring AI %dHz -> 8000 for G.711` | W | The follow-on capture reconfiguration. | |
| `AI re-init at 8000 failed` | E | Even the fallback failed — no audio. | |
| `opus_encoder_create failed (%s) -> PCMU` | W | Opus could not start. | |
| `faac_encoder_encode: %s` / `opus_encode: %s` | W | A per-frame encode error. | Constant repetition means a broken encoder. |
| `audio.codec2: %dHz capture cannot feed 8kHz G.711 - off` | W | The capture rate cannot be resampled to 8 kHz; **WebRTC gets no audio**. | Use `audio.samplerate = 16000`. §4.3 |
| `audio.channels=2/force_stereo requires AAC - G.711 has no standard stereo, staying mono` | W | Simulated stereo needs AAC. | §6.3 |
| `IMP_AO_SetPubAttr %dHz failed%s -> falling back` / `IMP_AO_Enable failed` / `IMP_AO_EnableChn failed` | W | Speaker bring-up steps failed. | §6.5 |
| `audio output (speaker) unavailable` | E | No AO pipeline. The board may have no speaker. | §6.5 |
| `IMP_AI_EnableAec failed - continuing without echo cancellation` | W | The vendor DSP could not be engaged — often because `libaudioProcess.so` was stubbed out to save flash, which silently also costs `ns`, `agc` and `high_pass`. | §6.6 |
| `IMP_AO_SendFrame failed` | W | A transient speaker write failure. | |

### 12.9 `OSD` (`src/hal/imp_osd.c`)

| Message pattern | Level | Meaning | Action |
| --- | --- | --- | --- |
| `osd stream %d item %d: rendered %dx%d exceeds usable %dx%d%s - skipped (reduce font_size/text length)` | W | Text does not fit. | Shorten it or lower `font_size` (8..128). |
| `logo %s (%dx%d) exceeds usable %dx%d%s - skipped` | W | Logo too big. | |
| `logo %s (%dx%d) not loaded` | W | Missing/unreadable/unsupported file. | |
| `stream %d rotated %d: hardware OSD/privacy limited to the top %d px of the %dx%d frame (libimp picHeight range-check); lower overlays are clamped up. Use a square stream, 180, or ch1 for full coverage.` | W | **T31 rotated streams: OSD/privacy only in a top band.** A libimp range-check timps cannot widen. | Square rotated stream (≤ 704×704), or put the OSD on `ch1`. |
| `CreateGroup %d failed` | E | The OSD group could not be created — no overlays on that stream. | |
| `osd stream %d item %d: CreateRgn failed (region pool exhausted?)` / `osd stream %d privacy %d: CreateRgn failed (region pool exhausted?)` | E | The SDK region pool ran out. | Fewer overlays/masks (max 8 + 4 per stream). |
| `IMP_OSD_RegisterRgn(rgn%d,grp%d) failed - this overlay will stay invisible` | W | The region exists but is not attached. | |
| `IMP_OSD_SetGrpRgnAttr(rgn%d,grp%d) failed - overlay may render with wrong alpha or not at all` | W | Usually alpha/transparency. | |
| `osd updater thread create failed - overlays will not update` | E | Static OSD only from now on. | Restart. |
| `osd item %d: no region on stream %d (disabled at startup) - enable persisted, applies on restart` | W | **An item disabled at boot has no region, so it cannot be enabled live.** | Restart. §2.7 |
| `privacy %d: no region on stream %d (OSD+privacy off at startup) - persisted, applies on restart` | W | **Not** the OSD-item rule. All four privacy regions are pre-created (hidden when disabled) whenever the stream has an OSD group, so masks are normally live. This fires only when the stream has **no group at all** — `osd.enabled = 0` *and* no privacy region enabled at boot — or when `CreateRgn` failed. | Enable OSD or one privacy region in the file and restart **once**; after that every mask is live. §2.7 |

### 12.10 `MOTION` (`src/hal/imp_motion.c`)

| Message pattern | Level | Meaning | Action |
| --- | --- | --- | --- |
| `IMP_IVS move detection not available in this build - motion detection disabled` | W | The SDK has no move API. | Nothing to configure. §7.1 |
| `all %d cells privacy-masked - motion not started` | W | Every grid cell is under a privacy region. | Shrink the masks or the grid. |
| `CreateGroup failed` / `CreateMoveInterface failed` / `CreateChn failed` / `RegisterChn failed` / `Bind FS->IVS failed` / `StartRecvPic failed` | E | IVS bring-up failed. | §7.1 |
| `motion thread create failed` | E | Out of memory. | §5.5 |
| `no IVS move results for over %dms - cycling the move channel to recover` | E | A stall; `motion.stalled` goes to 1. | §7.2 |
| `IVS StartRecvPic failed during stall-recovery cycle` | E | Recovery failed. | Restart. |
| `IMP_IVS_GetParam failed - falling back to full IVS rebuild` / `IMP_IVS_SetParam failed - falling back to full IVS rebuild` | W | A live sensitivity change could not be applied in place. | Cosmetic. |
| `on_motion '%s' cannot be executed - is the script installed and executable?` | W | Missing, not `+x`, bad interpreter — **or not an absolute path** (since 1.9.10). | §7.4 |
| `on_motion '%s' killed by signal %d (out of memory?)` | W | The hook was killed. | §5.5 |
| `on_motion: %d '%s' hooks still running - skipping this event; the hook outlives motion.cooldown_ms` | W | **Events are being dropped** because all `MOTION_HOOK_MAX` = 8 concurrent hook slots are in use — not merely because one run exceeded the cooldown. | Raise `motion.cooldown_ms` (floor 250 ms) or speed up the script. §7.4 |
| `on_motion: cannot launch '%s': %s - motion event dropped (the hook itself is fine; the board is out of memory)` | W | OOM. | §5.5 |

### 12.11 `DAYNIGHT` (`src/daynight.c`)

Every one of these is discussed in §3.3–§3.6; the table is the index.

| Message pattern | Level | Meaning |
| --- | --- | --- |
| `%s is not readable, using %s instead - set daynight.isp_path to silence this` | W | The configured ISP dump path is wrong; a fallback is in use. |
| `%s not readable, detection idle` | W | **Detection does nothing at all.** Fix `daynight.isp_path`. |
| `the ISP dump reports no gain ceilings (MAX SENSOR analog gain / MAX ISP digital gain), so the AE reserve is unknown here: a railed meter cannot be told from a dark scene, the railed-boot re-tune never fires, and the night reference is NOT protected against clipped readings. Ratio probes without a clear answer fall back to the audible probe on this camera` | W | Several safety mechanisms are disabled; probes fall back to the audible IR-cut probe. |
| `the night reference sits at the sensor's gain floor and no integration-time reading is available…` | W | Night→day can only come from the heartbeat. |
| `no probe has ever confirmed day (%d in a row found night): the best day-pipeline exposure seen was %.0f but daynight.day_gain is %.0f…` | W | **The line that hands you the number.** Raise `daynight.day_gain` above the reported value. |
| `daynight.day_gain (%.0f) is above night_gain (%.0f) - the thresholds are swapped; using day<%.0f night>%.0f` | W | `day_gain` must be the lower number. |
| `running_mode=%d never followed the switch to %s - board hook chain (switch_cmd -> color -> POST /control) incomplete, or manual override` | W | The board hook chain is broken. Purple-image territory. §3.2 |
| `ISP still reports %s %d s after the switch to %s (script and re-asserts all ran) - forcing one transition through %s, the only thing a stuck ISP acts on` | W | Forced transition; usually cures it. |
| `ISP still reports %s after a forced transition - giving up until the next mode change; the image does not match the decided mode %s` | W | The ISP latch is stuck. Restart. |
| `decided mode is %s but the ISP has been rendering %s for %d s - not enforcing, this may be a manual override. To re-measure and resolve, request a probe: POST /control {"daynight":{"probe":1}}` | W | timps suspects a deliberate override. That POST is the way out. |
| `boot: no usable exposure reading %ds after start-up (%s) - the boot measurement cannot run, falling back to the persisted %s and asserting it on the board once` | W | No measurement at boot. |
| `probe found night, but the pre-probe level %.0f had only %d units of AE reserve - the reference stays unset until the meter can answer` | **I** | AE railed; the reading is a clip. Listed here because users paste it, but it is a **LOGI** — informational, not a fault. |
| `silent probe gave no usable reading - falling back to the IR-cut probe` | W | One inconclusive silent probe. Normal occasionally. |
| `'%s %s' failed (rc=%d) - is the script installed?` | W | `daynight.switch_cmd` missing. **Without it the IR-cut filter never moves.** §11.2 |
| `'%s %s' failed (rc=%d) - silent probe unavailable, falling back to the IR-cut probe` | W | `daynight.irprobe_cmd` missing/failing. |
| `'%s' failed %d times - retiring the silent probe for this session; the trend trigger goes with it, leaving the jump trigger and the heartbeat` | W | Silent probing is off until restart. |
| `'%s %s' (pid %d) did not finish in time - killing it. A board hook must not block the detection thread` | W | The hook is too slow. |
| `'%s %s' (pid %d) survived SIGKILL - abandoning it. The board hook is stuck in the kernel (I2C/GPIO driver or a dead mount); day/night continues without it` | E | A kernel-level wedge. |
| `waitpid('%s %s') failed: %s` | W | Child reaping failed. |
| `daynight: fork failed: %s` / `irprobe: fork failed: %s` | W | Out of memory. §5.5 |
| `both a time window (%s..%s) and a location (%g/%g) are configured - the time window wins and the sun settings are ignored` | W | Configure one or the other. §3.5 |
| `mode=schedule needs a calendar: set daynight.time_night_start/time_day_start, or daynight.sun_latitude/sun_longitude, and make sure the clock is set` | W | Including the clock — no NTP, no schedule. §3.5 |
| `mode=schedule but no usable calendar (set daynight.time_night_start/time_day_start or daynight.sun_latitude/sun_longitude, and make sure the clock is set) - forcing nothing` | W | Same, at run time. |
| `trace_path %s is not under /tmp, /run or /dev/shm - tracing to flash wears it out` | W | Trace to tmpfs. §3.6 |
| `cannot open trace_path %s: %s - tracing disabled until the path changes` | W | Bad path. |
| `trace rotate %s -> %s failed: %s` | W | Trace file rotation failed. |
| `cannot start detection thread` | W | Day/night is off for this run. |

### 12.12 `RTSP` (`src/rtsp/rtsp.c`)

| Message pattern | Level | Meaning | Action |
| --- | --- | --- | --- |
| `cannot bind rtsp port %d` | E | **No RTSP at all.** | Port in use — another streamer? §8.6 |
| `RTSPS requested but TLS context failed - plain RTSP only` | E | RTSPS is silently not active. | Fix the cert/key. |
| `cannot bind rtsps port %d` | E | Same; plain RTSP keeps serving. | |
| `RTSPS requested but built without USE_TLS` | W | Rebuild with `BR2_PACKAGE_TIMPS_TLS`. | |
| `client limit (%d) reached, rejecting` | W | `RTSP_MAX_CLIENTS` = 8. The client sees a bare `503`. | §4.1 |
| `subscribe failed (source full), closing session=%s` | W | `HUB_MAX_SUBS` = 16 on that source. | Close viewers. |
| `session=%s idle >%ds (client gone without TEARDOWN), reaping` | W | Normal cleanup after 120 s, UDP transports only. | Ignore. |
| `control request incomplete after %llds, closing` | W | 10 s without a complete request. | Client or proxy problem. |
| `session=%s: send failed after %llds (%s) - dropping client; the write is torn mid-frame/mid-interleave` | W | The client stopped reading; `SO_SNDTIMEO` 15 s. The peer logs a truncated tail — expected. | Network. |
| `session=%s chn=%d: send queue overflowed, dropping frames (client/network too slow) - details at DEBUG` | W | `MS_RTSP_QCAP` = 64. | Lower `videoN.bitrate`, use TCP. §4.1 |
| `send_resp: response too large (hdr=%d body=%d cap=%d), dropping` | E | Practically unreachable. | Report as a bug. |
| `%d client thread(s) still live after a %lld ms drain - proceeding to teardown` | W | Shutdown-time; deliberate. | Ignore. |

### 12.13 `HTTP` (`src/mp4/httpd.c`)

| Message pattern | Level | Meaning | Action |
| --- | --- | --- | --- |
| `cannot bind http port %d` | E | **No HTTP, no `/control`, no WebUI bridge.** | Port in use. §8.6 |
| `http.https=%d but the TLS context could not be built from cert %s / key %s (see the TLS error above) - REFUSING to serve port %d at all rather than silently downgrading it to plaintext and leaking credentials. Fix or regenerate the cert/key pair, or set http.https=0 to accept plain HTTP deliberately` | E | **Fail-closed: the listener is never bound**, so every client gets a connection refusal. | Fix the cert/key, or `http.https = 0`. §8.4 |
| `http.https=%d but this build has no USE_TLS - port %d serves PLAIN HTTP and any password or token sent to it goes over the network in the clear` | E | The one deliberate asymmetry — the operator cannot fix it without a different binary, so it serves rather than dies. | Rebuild with TLS. |
| `audio.talk_ws=1 requires TLS on the http port, but port %d is plaintext%s - /talk stays disabled. Set audio.talk_ws=2 to accept plain ws:// deliberately (the browser then needs a secure-context override to reach its microphone at all)` | W | `/talk` will answer 426. | §6.4 |
| `connection limit (%d) reached, rejecting client` | W | `HTTP_MAX_CLIENTS` = 16. Each preview tab holds 3+. | §4.2 |
| `sse client limit (%d) reached, rejecting` | W | `events.max_clients`, default 8. | |
| `sse %s event too large, dropped` | W | One event overflowed its buffer. | |
| `no video params, abort mp4` | W | No SPS/PPS within 2 s. **The connection is closed with no HTTP response**, so the browser reports a network error. | §2.2 |
| `no AAC within warmup -> video-only mp4` | W | Audio was configured but produced nothing in time. | §6 |
| `mp4 chn=%d: no packets for %llds - encoder stall, dropping this client` | W | 60 s with nothing from the encoder. | §2.2 |
| `mp4 chn=%d: no audio for %llds but video still flowing (muted mid-stream?) - dropping this client so it reconnects video-only` | W | 5 s audio gap after the `moov` declared an audio track. Usually `audio.mute = 1`. | One reconnect, then video-only. |
| `mp4 chn=%d: send queue overflowed, dropping frames (client/network too slow) - details at DEBUG` | W | `MS_MP4_QCAP` = 64. With `http.adaptive_drop = 1` this client alone freezes to the next keyframe. | §4.2 |
| `mjpeg: no frames for %llds - encoder stall, dropping this client` | W | Same 60 s rule. `MS_MJPEG_QCAP` is only 2 — deliberately. | |
| `dropped a corrupt %s fragment (OOM?)` | W | Memory pressure. | §5.5 |
| `%s=%d: send failed after %llds (%s) - dropping client; the write is torn mid-frame, so the peer logs a truncated tail` | W | `SO_SNDTIMEO` 15 s. **Not a data-path bug** — every byte sent before the cut was valid. ffmpeg reports `Stream ends prematurely` / `Invalid NAL unit size`. | Network. |
| `%d connection thread(s) still live after a %lld ms drain - leaking tls_ctx/h rather than risking a use-after-free on process exit` | W | Shutdown-time; deliberate. | Ignore. |

### 12.14 `REC` (`src/record.c`) and `TL` (`src/timelapse.c`)

| Message pattern | Level | Meaning | Action |
| --- | --- | --- | --- |
| `record.min_free_mb=%d unreachable (only %lldMB free, %lldMB even if every existing recording were deleted) - refusing to record rather than empty the archive` | W | Deliberate refusal (since 1.9.8; before that it emptied the archive). | Lower `record.min_free_mb`. §5.1 |
| `unsafe record.dir/name ('..' or absolute name), not recording` | E | `record.name` must be relative and contain no `..`. | §5.1 |
| `open %s: %s` / `fdopen %s: %s` | E | The segment file could not be created. | Mount/permissions. |
| `segment name collision, wrote %s instead` | W | Two segments resolved to the same name. | Add `%S` to `record.name`. |
| `write %s: %s` / `segment write failed (%s), closing` / `segment flush failed (%s), closing` | E | The volume went away or filled. | §5.2 |
| `segment close/sync failed: %s (tail may be truncated)` | E | **The last seconds of that file may be unreadable.** | §5.2 |
| `dropped a corrupt %s fragment while recording (OOM?)` | W | Memory pressure; that fragment is missing. | §5.5 |
| `chn=%d: record queue overflowed, dropping frames (storage/consumer too slow) - details at DEBUG` | W | **since v1.9.19.** Once per subscription: the recorder's own queue evicted packets, so the segment freezes and resumes at the next keyframe. Sustained drops are summarised by `HUB` once per 60 s (§12.17). | Faster storage, a lower `record.channel` bitrate, or a local card instead of a network share. §5.2 |
| `record.pre_roll_s=%d cannot be held: the pre-roll ring caps at ~%.0fs for ch%d (%d kbps, %d fps, %d packets / %d MB max) - actual pre-roll is shorter` | W | The ring is bounded by packets and bytes, not seconds. The byte cap is 4 MB — at a high `pre_roll_s` that is 11 % of a 37 MB board. | Lower `record.pre_roll_s`. |
| `record.audio=1 but audio codec is %s - recordings are video-only; AAC (build with USE_FAAC=1) required` | W | fMP4 carries AAC. | `audio.codec = aac`. §5.3 |
| `thread` | E | The recorder (or timelapse) thread could not start. | §5.5 |
| `clip busy, skipped %s` | W | A clip was already being written. | |
| `clip open %s: %s` / `clip write failed (%s), dropping` / `clip close %s: %s` | W/E | Clip I/O failed. | Storage. |
| `clip: no frames for %s` | W | The window contained no frames — the encoder was idle. | §2.1 |
| `unsafe timelapse.dir/name ('..' or absolute name), skipping shot` | E | Same path rule. | §5.4 |
| `open %s: %s` / `write %s: %s` (TL) | E | Storage problem. | §5.4 |
| `no frame from src=%d within %d ms - retrying in %ds` | W | **The JPEG source produced nothing.** | Enable `videoN.jpeg`; check the JPEG channel. §5.4 |

### 12.15 `TLS` (`src/tls.c`) and `WEBRTC` (`src/webrtc/`)

| Message pattern | Level | Meaning | Action |
| --- | --- | --- | --- |
| `ctr_drbg seed failed` | E | No entropy source (also in `dtls.c`). | |
| `cannot parse cert %s` / `cannot parse key %s` | E | Wrong file/format/empty (also in `dtls.c`). | Regenerate. |
| `ssl config defaults failed` / `dtls config defaults failed` | E | mbedTLS build problem. | |
| `cert/key pair rejected (%s / %s)` | E | The key does not belong to the certificate (also in `dtls.c`). | Regenerate both together. |
| `session ticket setup failed - no resumption on this listener` | W | Harmless; handshakes are just slower. | |
| `handshake failed: peer %s sent a fatal alert (-0x%x, %u more suppressed) - the CLIENT rejected our certificate (self-signed, or the name/IP it connected to is not in the cert SAN); our TLS config is not at fault` | W | **Read it literally: the browser refused the cert.** | §8.5 |
| `handshake failed with peer %s (-0x%x, %u more suppressed) - rejected/expired cert or TLS config problem? (mbedtls strerror)` | W | The generic twin. | §8.5 |
| `dtls-srtp profile list rejected` / `certificate fingerprint failed` | E | DTLS context could not be built → `/webrtc/whep` answers 404 `disabled`. | |
| `webrtc.enabled=1 but no usable DTLS certificate (%s / %s) - /webrtc/whep stays disabled` | E | **WebRTC needs a certificate even on a plaintext port.** | Generate one. §4.3, §11.6 |
| `dtls handshake failed (-0x%x)` | W | DTLS negotiation failed. | §4.3 |
| `peer presented no DTLS certificate` | W | The browser sent none. | |
| `peer negotiated no usable SRTP profile (%u)` | W | No common SRTP suite. | |
| `peer requested an SRTP MKI (%u bytes) - unsupported` | W | MKI is not implemented. | |
| `no transform after handshake - cannot export srtp keys` / `srtp keying material export failed (-0x%x)` | W | Key export failed. | |
| `%s: peer certificate does not match the offer's a=fingerprint - dropping` | W | MITM, or a renegotiating browser. | |
| `%s: DTLS up but media setup failed` | W | Media path setup failed after a good handshake. | |
| `%s: no ICE/DTLS completion within %llds - closing` | W | ICE never completed. timps is **ICE-lite, one host candidate, no STUN/TURN** — it only works on a directly reachable LAN path. | §4.3 |
| `offer has no usable sha-256 a=fingerprint` | W | Malformed offer → 400. | |
| `offer wants a=setup:%s - we can only be passive` | W | The offer must be `actpass` or `active`. | |
| `video%d is not H264 - WebRTC carries H264 only` | W | **Set that channel's `videoN.codec = h264`.** | §4.3 |
| `no SPS/PPS for video%d - cannot answer` | W | Parameter sets not ready → 503. | Retry. |
| `offer carries no H264 with packetization-mode=1` | W | Browser/offer limitation → 400. | |
| `cannot bind udp port %d-%d: %s` | E | The media port range is exhausted or in use → 503 `busy`. | Widen `webrtc.port`…`webrtc.port_max`. |
| `cannot start session thread` | E | OOM → 503. | §5.5 |
| `%s: DELETE marked the session but it has not finished yet` | W | DELETE returns 200 after waiting up to 600 ms. | Harmless. |
| `%d session(s) still running at stop - leaving the DTLS context allocated` | W | Shutdown-time; deliberate. | Ignore. |

### 12.16 `SRT` (`src/srt.c`)

| Message pattern | Level | Meaning | Action |
| --- | --- | --- | --- |
| `audio.enabled=1 but codec is %s - SRT/MPEG-TS carries AAC only, stream is video-only` | W | | `audio.codec = aac`. §4.4 |
| `AAC rate %d Hz has no ADTS index, SRT audio disabled` | W | Non-standard AAC rate. | Use a standard rate. |
| `chn=%d: no packets for %llds - encoder stall, dropping this client` | W | 60 s stall rule. | §2.2 |
| `SRTO_PASSPHRASE rejected (need 10-79 chars): %s - refusing to run unencrypted` | E | **timps refuses rather than falling back to plaintext.** | Use a 10–79 character passphrase. |
| `rejecting caller with missing/wrong streamid` | W | `srt.streamid` mismatch. | Set `streamid=` in the client URL. |
| `srt_startup failed` / `create_socket` / `create_socket: %s` | E | libsrt could not initialise. | |
| `bind/listen on %d failed: %s` | E | Port in use. Only SRT dies. | |
| `client limit (%d) reached, rejecting` | W | `SRT_MAX_CLIENTS` = 8, enforced after accept — the client sees an accepted-then-closed connection. | |
| `connection to %s:%d lost - reconnecting (quiet retries, backoff up to %ds)` | W | Caller mode. Retries are quiet by design. | Fix the destination. |
| `connect to %s:%d failed: %s - retrying (quiet retries, backoff up to %ds)` | W | Same. | |
| `unknown srt.mode '%s' - using listener` | W | Valid: `listener`, `caller`. | |
| `srt.mode=caller but srt.host is empty - SRT disabled` | E | | Set `srt.host`. |
| `%d client thread(s) still in libsrt after the drain - proceeding to srt_cleanup()` | W | Shutdown-time. | Ignore. |

### 12.17 Smaller modules

| Module | Message pattern | Level | Meaning |
| --- | --- | --- | --- |
| `HUB` | `chn=%d %s: %u queue overflow%s in the last %llds (consumer too slow)` | W | **since v1.9.19** (until v1.9.20 it ended `(consumer too slow, IDR re-requested)`). The only line `HUB` emits above DEBUG. `%s` is the consumer kind — `rec`, `rtsp`, `mp4`, `webrtc` or `srt` — and the line is rate-limited to one per 60 s per (kind, stream). Same events as `queue_drops` in `GET /control`. §2.3 |
| `AAC` | `unsupported AAC samplerate %d Hz, using 16k index fallback` | W | The ASC/ADTS index could not be derived; the stream is tagged 16 kHz. Use a standard rate. |
| `bc` | `AACInitDecoder failed` | W | The AAC backchannel decoder could not start (`USE_BC_AAC` builds). |
| `talk` | `refused: unsupported rate= in %s` | W | `?rate=` must be 8000/16000/24000/32000/44100/48000. iOS Safari commonly forces 48000. |
| `talk` | `refused: cross-origin upgrade` | W | The `Origin` host must match the `Host` host (ports are not compared). `Origin: null` is refused. §6.4 |
| `talk` | `another talker holds the speaker - closing` | W | Speaker contention; close code 1008. The owner is stolen only after 10 s of silence. |
| `spk` | `play: cannot open %s: %s` / `play: bad WAV header in %s` / `play: op_open_file(%s) failed (%d)` | W | The sound file is missing or not a supported format. |
| `spk` | `play: %s is Opus but USE_PLAY_OPUS not built` | W | Rebuild with `BR2_PACKAGE_TIMPS_PLAY_OPUS`, or use WAV. |
| `spk` | `backchannel owner idle >%llds - releasing speaker for queued playback` | W | Normal reclaim after 10 s. |
| `spk` | `mkfifo %s failed: %s - play queue disabled` / `open %s failed: %s - play queue disabled` / `cannot start play-FIFO thread` | W | `/run` is full or read-only. |
| `events` | `daynight history: cannot allocate %u samples, keeping %u` | W | `daynight.history_s` is too large for available RAM (cap 48 h). Lower it. |
| `HAL_SIM` | `cannot open sim video %s` / `cannot open sim audio %s` / `cannot open sim jpeg %s` | E | Host-simulator builds only. |
| `HAL_SIM` | `no memory for sim AU buffer` | E | Simulator OOM. |
| `HAL_SIM` | `sim.video%d not set, skipping` / `video%d.jpeg set but sim.jpeg missing` | W | Simulator config incomplete. |
| `LOG` | `debug_modules: %d name(s) over %d chars ignored` / `debug_modules: at most %d names, %d ignored` | W | `general.debug_modules` takes a bounded list of short names. |
| `TRACE` | `send-pipeline tracing ENABLED (mask=0x%x: %s%s%s%s) threshold=%dms - this is a debug build option, leave it off in normal operation` | W | A `TIMPS_TRACE=1` build with `general.trace` set. **Not for production images.** |
| `TRACE` | `%s/%s chn=%d: trace attached` | W | A trace probe attached to a consumer. |
| `TRACE` | `%s/%s chn=%d %s%s %zuB gap=%lld.%dms age=%lld.%dms send=%lld.%dms (wr=%lld.%dms/%lldw cpu=%lld.%dms)%s` | W | Per-AU trace line. Developer output. |
| `TRACE` | `%s/%s chn=%d %llds: au=%u slow=%u tx=%lluKiB max gap=%lld.%dms age=%lld.%dms send=%lld.%dms write=%lld.%dms (writes=%lld tot=%lld.%dms)%s` | W | Periodic trace summary. |

---

## 13. Misconfiguration gallery

Real `key = value` lines that are accepted, look right, and do the wrong
thing. Each entry: **what was written → what actually happens → what to
write instead.**

### Image and ISP

| # | Wrong | What happens | Correct |
| --- | --- | --- | --- |
| 1 | `image.anti_flicker = 2` on 50 Hz mains | 2 means **60 Hz**. Rolling horizontal bands under LED/fluorescent light. This is the **built-in default**, so every camera outside 60 Hz countries ships wrong. | `image.anti_flicker = 1` |
| 2 | `image.running_mode = 0` posted by hand to "get colour back" at night | The ISP renders the day pipeline while the IR-cut filter is still out and the IR LEDs are on → **purple/magenta image**. | Let day/night do it, or drive `daynight.switch_cmd` as well. |
| 3 | `image.brightness = 255` | A blown-out white picture. Usually left behind by an interrupted QA run rather than typed. | `image.brightness = 128` |
| 4 | `image.wb_rgain = 32767` / `image.wb_bgain = 32767` | A magenta image. Also a classic interrupted-QA leftover; unity is **1024** **(unverified — an IMP-documentation fact; `src/` only clamps these to 0..65535 and passes them through)**. | `1024`, or `image.core_wb_mode = 0` for auto. |
| 5 | `image.defog_strength = 200` on a T30 | Accepted, persisted, echoed back — and **ignored**, because the SoC has no defog. | Check `caps.image` first. |

### Video and encoder

| # | Wrong | What happens | Correct |
| --- | --- | --- | --- |
| 6 | `video0.rotation = 180` on anything but T40/T41 | Coerced to 0 with a warning. | `image.hflip = 1` + `image.vflip = 1` |
| 7 | `video0.rotation = 1` (meaning "true") | `1` is the legacy libimp enum for **90°**. | `0` to disable. |
| 8 | `video0.rotation = 90` at `1280x720` on a T23 SW-rotate build | Refused by the **pixel envelope** (`ew × eh > 704×576`), which is checked *before* the alignment rule — stream runs **unrotated**. (720 *is* a multiple of 16; alignment is not the problem, and `1280x704` is refused for the same envelope reason.) | A substream-class source whose **height** is a multiple of 16 and ≥ 256: `704x576` → `576x704`. |
| 9 | `video0.rotation = 90` with `1920x1080@25` on T31 | Exceeds the vendor FS-rotate cap (≤1280×704, ≤15 fps) → **unrotated**. | Rotate the substream, or drop to 1280×704 @15. |
| 10 | `video0.codec = h265` on T10/T20/T23 | Coerced to `h264` — deliberately, because before v1.8.1 it streamed H.264 **mislabelled as H.265** and broke players. | `h264` |
| 11 | `video0.bitrate = 25000` at `fps = 25` | ~1 MB keyframes against a 1 MB AU buffer → **every keyframe dropped, no client ever decodes**. The warning fires above **~839 × fps kbps**, i.e. ~21 Mbps at 25 fps. | Anything under that; 3000–4096 kbps at 25 fps is the fleet norm. |
| 12 | `video0.rc_mode = fixqp` plus a carefully chosen `bitrate` | `fixqp` **ignores** `bitrate` entirely; the rate is whatever the QP produces. | `cbr` if the number matters. |
| 13 | `video0.max_gop = 60` | Reserved and **ignored**; warns once. | `video0.gop = 60` |
| 14 | `video0.min_qp = 40` to "improve quality" | A raised QP **floor** is a bitrate **ceiling**. Reported on T31: 20→40 cut delivered bitrate 229 → **25** kbps **(unverified — no measurement record in this repo backs those two numbers; the direction of the effect is not in doubt)**. | Leave at the default 20 unless you want that. |
| 15 | `video0.qp = 30` under `rc_mode = cbr` | Only consumed under `fixqp`. No effect, no warning. | Use the rate-control knobs. |
| 16 | `video0.buffers = 3` on a T31 with `isp_ch0_pre_dequeue_time > 0` | `EnableChn` fails → **channel 0 dead**. And writing the default `2` back does **not** fix it: `buffers_explicit` is set by the key's *presence*, which suppresses the HAL's safety clamp. | **Delete the `video0.buffers` line entirely.** |
| 17 | `video0.fps = 0` | Clamped to **1**. One frame per second, not "unlimited". | A real number, 1..120. |
| 18 | `video0.fps = 30` with `sensor.fps = 15` | `fs_create()` passes `outFrmRateNum = video0.fps` to IMP unconditionally, and `GET /control` echoes **30** — but the framesource can only decimate, never interpolate, so you actually get 15 **(unverified — the hardware behaviour is inference; nothing in `src/` detects or reports the shortfall)**. | Match the sensor, or raise `sensor.fps` (restart-only). Since v1.9.19 the `sensor fps: requested …, driver holds …` line says what the sensor took; note the **auto** value is capped at 30, so above that `sensor.fps` has to be explicit. §1.10 |
| 19 | `video0.quality_lvl = 7` on a T31 | Classic-SoC knob; **no equivalent field** in the new-API encoder. Warned once, ignored. | On T23 it is a real lever: `quality_lvl` 2→7 moved the mean 1709 → 1243 kbit/s, **−27 %** (`dev_notes/T23_RATECONTROL_INVESTIGATION_2026-08-21.md`). The −41 % figure sometimes quoted is `cbr` → `vbr@7`, i.e. a rc-mode change as well. |
| 20 | `sensor.model = gc2053` on a board running sc4336p | Overridden by the kernel registry with a warning — the configured value would make the ISP divide by zero **in the kernel**. | Match the loaded driver, or leave `sensor.model` unset. |

### Audio

| # | Wrong | What happens | Correct |
| --- | --- | --- | --- |
| 21 | `audio.samplerate = 48000` | Capture accepts **8000 and 16000 only**; falls back with a warning. | `16000` |
| 22 | `audio.codec = pcmu` and then "why is the browser preview silent?" | fMP4 carries **AAC only**. | `audio.codec = aac` |
| 23 | `audio.codec = aac` and then "why does WebRTC have no sound?" | WHEP carries **G.711 only**. | Add `audio.codec2 = pcmu` |
| 24 | `audio.codec = opus` on a build without `USE_STREAM_OPUS` | Not an error — an unknown word, which falls through to the default **AAC**. Silently not Opus. | Check `GET /control` read-back. |
| 25 | `audio.channels = 2` with `audio.codec = pcmu` | Stays mono; G.711 has no standard stereo. (And "stereo" here is a duplicated mono mic anyway.) | `audio.codec = aac` |
| 26 | `audio.talk_ws = 1` on a port with `http.https = 0` | At request time `/talk` answers **`426 Upgrade Required` / `tls required`**. The word "disabled" comes from the *boot* WARN (`… - /talk stays disabled`), so do not expect a 404. | Enable TLS, or `audio.talk_ws = 2` and accept plain `ws://`. |
| 27 | `POST /control {"audio":{"backchannel":1}}` | Accepted and persisted — and `/talk` keeps answering **404** because `bc_available()` is a boot-time fact. | Restart `timpsd`. |
| 28 | `audio.aec = 1` on a camera with push-to-talk | `IMP_AI_EnableAec` brings up the vendor's full WebRTC audio chain on the **mic** (AGC `kFixedDigital` +15 dB, NS `kHigh`, configured by `/etc/webrtc_profile.ini`, not by any timps key); the mic stays audibly noisier until a restart, and `aec = 0` does not undo it. Root-caused and documented in `dev_notes/AEC_NOISE_AFTER_TALK_2026-09-08.md` — understood, not fixed. | Leave `audio.aec = 0` unless echo is a real problem. |
| 29 | `audio.high_pass = 1` posted live, expecting an immediate change | POST-able but **not live** — libimp would race its own record thread. | Restart. |

### Recording, motion, day/night

| # | Wrong | What happens | Correct |
| --- | --- | --- | --- |
| 30 | `record.min_free_mb = 4096` on a 2 GB card | Since 1.9.8: **refuses to record** and says so in `last_error`. Before 1.9.8 it deleted the entire archive and still missed the target. | Well under the card size, e.g. `200`. |
| 31 | `record.name = /mnt/sd/%Y%m%d.mp4` | Absolute path → `unsafe record.dir/name … not recording`. `record.name` is relative to `record.dir`. | `record.name = %Y%m%d/%H/%Y%m%dT%H%M%S` |
| 32 | `record.mode = motion` with `motion.enabled = 0` | Nothing is recorded **while no manual latch is set**. A `POST {"record":{"active":1}}` does *not* hit this: the manual start latch is checked first in `want_write()` and wins over mode and motion, so the camera then records **continuously** — which is its own surprise if the user expected motion clips. | `motion.enabled = 1` for motion-gated recording; clear the latch (`active` omitted or `<0`) to return to config mode. |
| 33 | `record.audio = 1` with `audio.codec = pcmu` | Recordings are **video-only**; fMP4 carries AAC. | `audio.codec = aac` |
| 34 | `record.pre_roll_s = 60` at 6 Mbps | The ring is bounded by packets and bytes, not seconds, and it holds the **smaller** of the two: `RING_MAX_BYTES × 8 / bitrate` = **~5.6 s** here, against `RING_CAP` (256) / fps ≈ 10 s at 25 fps. The byte cap binds above ~3.3 Mbps; the 256-packet cap only binds below it. 4 MB is also 11 % of a 37 MB board. | `3` (the default) is what the fleet uses. |
| 35 | `motion.cols = 10` and `motion.rows = 10` | `cols × rows` is clamped to `MOTION_CELL_LIMIT` (52). You get far fewer cells than you asked for. | `7 × 7 = 49`, or the default `5 × 5`. |
| 36 | `motion.on_motion = timps-motion` | Since v1.9.10 the launcher is `posix_spawn`/`execve` — **no PATH search**, so the exec fails. Not silent: the child `_exit(127)`s and every event logs `on_motion 'timps-motion' cannot be executed - is the script installed and executable?`. | `/usr/sbin/timps-motion` |
| 37 | `motion.cooldown_ms = 100` | Floored to **250**, so the hook cannot be re-exec'd on every frame. | `1000` or more if the hook is slow. |
| 38 | `motion.sensitivity = 130` changed to `140` | Both map to SDK level **2** (`sensitivity × 4 / 255`). No observable change. | Move in steps of ~64. |
| 39 | `daynight.day_gain = 5000` with `daynight.night_gain = 3000` | Swapped. timps warns and swaps them back. | `day_gain` is the **lower** number. |
| 40 | `daynight.night_gain = 7000` on a camera whose **day** pipeline rails at 6166 | Day mode's exit is `s > night_gain \|\| dn_clipped(headroom)`. The first half the day meter can never physically reach; the second — the AE-clip exit — is present *unless* the ISP dump reports no gain ceilings, in which case `dn_clipped()` treats unknown headroom as usable and that exit disappears too → **stuck in day, railed dark, for hours**. (Reported on a T20 in a windowless basement: 88 minutes; **the duration is unverified in this repo**.) | Do **not** raise `night_gain` above what the day pipeline can read. Revert to `4096`. |
| 41 | `daynight.day_gain = 300` indoors | 300 ≈ 1.17× the gain floor (the scale is `256` = 1.0×), i.e. "day only when the day pipeline needs essentially no gain" — unreachable in a lit room, where **2–3× is typical (unverified — a fleet observation, not a figure derived from `src/`)**. Camera never leaves night. | `768` (the current default), or take the number from the `no probe has ever confirmed day` warning. |
| 42 | Both `daynight.time_night_start` **and** `daynight.sun_latitude` set | The **time window wins** and the sun settings are silently ignored. | Configure exactly one. |
| 43 | `daynight.mode = schedule` on a camera with no NTP | No usable clock → nothing switches, ever. | Fix time sync first, or use `auto`. |
| 44 | `daynight.trace_path = /etc/dn-trace.log` | Warns: tracing to flash wears it out. | `/tmp/...`, `/run/...` or `/dev/shm/...` |
| 45 | `daynight.probe_jump_pct = 40` | Frozen into an internal constant by the 2026-08-22 consolidation. Warns only because the value differs from the constant. | Delete the line. |

### OSD, network and transport

| # | Wrong | What happens | Correct |
| --- | --- | --- | --- |
| 46 | `osd0_0.enabled = 0` in the file, then `POST {"osd0_0":{"enabled":1}}` | **Two faults.** (a) `osd0_0` is not a key at all — `osd_key()` wants dots (`osd0.0.enabled`), so the file line is an unknown key that is logged and dropped, and the POST section name matches nothing. (b) Even spelled right, an OSD **item** disabled at boot has no IMP region, so the POST persists and changes nothing visible. (Privacy masks are different — §2.7.) | `osd0.0.enabled = 1` in the file + restart; over HTTP, `{"osd0":{"0":{"enabled":1}}}`. |
| 47 | `osd0.0.font_size = 200` | Clamped to **128** (the ceiling was lowered from 256 because a 255-character item at 256 px could transiently allocate ~14 MB). | ≤ 128 |
| 48 | `osd0.0.text = Kamera #2` hand-edited, unquoted | The unquoted `#` opens an inline comment. The value is correct **until the next reboot**, then it is `Kamera`. | `osd0.0.text = "Kamera #2"` |
| 49 | `http.https = true` intending strict TLS | Since v1.9.11, `1` means **both schemes**; plaintext is accepted on the same port. | `http.https = 2` |
| 50 | `http.https = strict` (or `ture`, or `enabled`) | Parsed as **0** — plaintext. Warned since v1.9.12; before that, silent. | `0`, `1` or `2` |
| 51 | `http.port = 8080` | Clashes with `onvif_srvd`, which also listens there; whichever binds first wins, so ONVIF may fail to start. The default moved 8080 → 8880 in **v1.5.0** for exactly this reason (`CHANGELOG.md`). | `http.port = 8880` |
| 52 | `rtsp.mtu = 1400` for a camera reached over a VPN | Fragmentation and drops. 1200 is already the VPN-safe value; 1400 is a LAN-only optimisation. | `rtsp.mtu = 1200` |
| 53 | `rtsp.tls = 1` and assuming RTSPS is now active | Unlike HTTPS, RTSPS is **not fail-closed**: a broken cert or a failed bind leaves plain RTSP serving, with only a log line. | Check the boot log for `RTSPS requested but …`. |
| 54 | `srt.passphrase = secret` (6 characters) | libsrt requires **10–79**; timps refuses to run SRT at all rather than run it unencrypted. | A 10–79 character passphrase. |
| 55 | `srt.mode = caller` with `srt.host` unset | `srt.mode=caller but srt.host is empty - SRT disabled`. | Set `srt.host`. |
| 56 | `webrtc.enabled = 1` on a port where TLS is configured, reached over `http://` | Every WHEP POST gets **426**. (`1` was **never** a released default: 1.9.13 shipped `0`, and 1.9.14 went `0` → `1` → **`2`** within the one release. The compiled default today is `2`.) | `webrtc.enabled = 2`, or use `https://`. |
| 57 | `video0.codec = h265` with `webrtc.channel = 0` | WHEP answers **400** — WebRTC here carries H.264 only. | `h264` on the WebRTC channel. |
| 58 | `jpeg.enabled = 0` and `video0.jpeg = false` | `/snapshot.jpg` and `/stream.mjpeg` return **404 `no jpeg`**, and timelapse never gets a frame. | `video0.jpeg = true` with a unique `video0.jpeg_chn`. |
| 59 | `events.max_clients = 0` meaning "unlimited" | `<= 0` falls back to **8**, it does not disable the cap. | A real number. |
| 60 | `general.trace = 1` left on a production camera | Floods the 64 KB syslog ring with per-AU lines and announces itself as a debug build option. | `0` |
| 61 | The same key written twice in `timps.conf` | At load the **last** line wins; at the next rewrite the **first** is replaced and later duplicates are **deleted**. The meaning changes the first time that key is persisted. | Keep one line per key. |
| 62 | No `http.user` and no `rtsp.user`, then `curl` `/control` from a LAN host | **403 `local only`**, and the media endpoints are meanwhile fully public. **But this is not the shipped state** — `package/timps/files/timps.conf` sets `thingino`/`thingino` for both pairs, so a stock camera answers **401** and its media is closed. A 403 means the credentials were cleared by hand. | Use `X-Timps-Token`, or set credentials. |
| 63 | Leaving the shipped `http.user`/`http.pass` and `rtsp.user`/`rtsp.pass` at `thingino`/`thingino` | Every camera on the network shares one password, on RTSP **and** HTTP. Nothing warns about it; `GET /control` does not echo credentials. | Change all four in `/etc/timps.conf` (file-only) and restart. |

---

## 14. What NOT to advise

Plausible-sounding answers that are wrong in this code base. Each was checked
against the source named.

**Configuration**

1. **"The daemon will overwrite your config and eat your comments."** Mostly
   false. `config_write_keys()` (`src/config.c`) copies the file line by line,
   replaces only the lines whose key it owns, appends the rest, and commits
   atomically. **Full-line** comments, ordering and unknown lines survive. The
   one real loss: an **inline** `# comment` sitting on a key's own line is
   discarded when that key is rewritten, because `write_kv_line()` regenerates
   the whole line. Put notes on their own line.
2. **"There is a `/etc/timps.conf.example` that gets seeded on first boot."**
   There is not. `package/timps/timps.mk` installs `/etc/timps.conf`
   directly at build time. `package/timps/README.md` still says otherwise —
   that text is stale. (The repo's own `timps.conf.example` is a
   documentation file in the source tree, not something shipped.)
3. **"Out-of-range values are rejected, so if it was accepted it is valid."**
   Almost nothing is rejected. Numeric keys are **clamped** and enums are
   **coerced**, then the clamped value is persisted and echoed. `fps = 0`
   becomes 1; `codec = h265` on a T23 becomes `h264`.
4. **"`osd.hinting` and `osd.supersample` are config-file-only."** They are
   not. Both carry `F_CTRL` and are POST-able through `/control`; they are
   *restart-required*, which is a different thing. The genuinely file-only set
   is best stated as a **rule**, not a list, because a partial list keeps
   drifting. `POST /control` reaches only these sections:
   `image`, `audio`, `general`, `daynight`, `osd`/`osd<S>`, `video`,
   `privacy`, `sensor`, `motion`, `record`, `timelapse`. Therefore:
   - **Whole sections it never walks — every key in them is file-only:**
     `http.*`, `rtsp.*`, `jpeg.*`, `events.*`, `webrtc.*`, `srt.*`, `sim.*`.
     (That covers all credentials, tokens, ports and TLS paths.)
   - **Individual keys without `F_CTRL` inside a walked section:**
     `general.loglevel`, `general.imp_polling_timeout`,
     `general.osd_pool_size` (only `general.debug_modules` is POST-able);
     `video<N>.imp_chn` / `jpeg` / `jpeg_quality` / `jpeg_fps` / `jpeg_chn`;
     `osd<S>.<N>.logo` / `logo_w` / `logo_h` / `font_path`;
     `motion.on_motion`, `motion.cooldown_ms`;
     `daynight.switch_cmd`, `daynight.isp_path`, `daynight.irprobe_cmd`,
     `daynight.trace_path`.
   - **No table entry at all** (side effects in `set_kv()`), hence file-only:
     `general.syslog`, `general.trace`, `general.trace_ms`.
   - The exception in the other direction is `daynight.mode`: no `F_CTRL`, but
     hand-validated in `control.c`, so a POST does reach it.

   Keys in the second group are **named back** in the reply's `ignored` array,
   so a POST carrying one is not silently swallowed. Same rule in
   `docs/ai/config-keys.md` §1.3.
5. **"Set `videoN.max_gop` to control the keyframe interval."** It is
   reserved and ignored. `videoN.gop` is the key.
6. **"Use `motion.roi_x/y/w/h` to restrict the detection area."** Deprecated
   and ignored since the cell grid replaced them.

**Rotation and image**

7. **"Just set `videoN.rotation = 180` to turn the picture around."** Only on
   T40/T41. Everywhere else it is coerced to 0; use `image.hflip = 1` +
   `image.vflip = 1`.
8. **"90 is clockwise."** On T23/T40/T41 yes; **on T31, 90 is
   counter-clockwise** (kept numerically aligned with prudynt-t/raptor).
9. **"Rotation is just a config change, it applies live."** It is
   restart-required, and the advertised dimensions swap.
10. **"Force `image.running_mode` to fix a wrong-looking night image."**
    This is the documented cause of the purple/IR-tinted picture: it moves
    the ISP colour pipeline without moving the IR-cut filter or the
    illuminator. Drive the board hook instead.
11. **"Raise `daynight.night_gain` so it switches to night sooner."** On
    several SoCs the day and night pipelines rail at *different* ceilings. A
    `night_gain` above what the **day** meter can physically read removes day
    mode's only exit — measured as 88 minutes stuck in a railed-dark day
    mode on a T20.

**Streaming**

12. **"RTSP answers 453 (Not Enough Bandwidth) when the client limit is
    reached."** It answers a bare **503 Service Unavailable**, with no CSeq,
    and closes the socket. Some older documentation and source comments said
    `453`; that was never the behaviour.
13. **"Use multicast RTSP."** Not supported. A multicast-only `Transport:`
    gets `461 Unsupported Transport`.
14. **"Pause the RTSP stream."** `PAUSE` is not implemented; it answers
    `405` with `Allow: OPTIONS, GET_PARAMETER, TEARDOWN` and the media keeps
    running. That is deliberate — better than hanging.
15. **"Add a STUN or TURN server so WebRTC can traverse NAT."** There is
    nowhere to configure one. timps is **ICE-lite with a single host
    candidate**; WHEP only works on a directly reachable path.
16. **"WebRTC works in any modern browser."** **Firefox does not work** and
    that is still open — the answer carries a High-profile `profile-level-id`
    from the live SPS, which Firefox's baseline-only SDP refuses. Chrome and
    Chromium are the tested targets.
17. **"Bind the HTTP server to localhost only with `http.bind`."** There is
    no such key. `net_listen_tcp()` always binds `INADDR_ANY`. Use a
    firewall.
18. **"`http.adaptive_drop = 0` will fix the freezing preview."** It does the
    opposite for everyone else: instead of one slow client freezing on its
    last frame, the queue overflow triggers an IDR request on the **shared**
    encoder, spiking the bitrate for every viewer. The default `1` is the
    hardware-verified behaviour.
19. **"A `queue overflowed` or `AU exceeds max buffer` line means the camera
    is broken."** On a T23 at 1080p with stock defaults an occasional
    `AU exceeds max buffer` is expected — the classic rate controller is
    quality-seeking, so a complex-scene IDR legitimately reaches ~1 MB.
    Recovery is the next scheduled IDR. The lever is `video0.min_qp`.

**Auth and TLS**

20. **"Log out / clear the session cookie / send the CSRF token."** timps has
    **no cookies, no sessions and no CSRF token**. Authentication is Basic,
    Digest, the `X-Timps-Token` header (or `?token=`), or the hard-coded
    127.0.0.0/8 bypass.
21. **"You are locked out after too many failed logins — wait it out."**
    There is **no rate limiting, no lockout and no 429**. `auth_fail_note()`
    only writes a log line. A `<N> failed login attempts` warning means
    someone is scanning, not that anything was blocked.
22. **"Set `auth_bypass`."** No such key exists.
23. **"The handshake warning means your certificate or TLS settings are
    broken."** For the `peer … sent a fatal alert` variant the message says
    the opposite in so many words: the **client** rejected our certificate.
    The usual cause is a self-signed cert whose SAN carries only the `.local`
    name while the user connects by IP.
24. **"Trust the certificate once in the WebUI and the preview will work."**
    Browsers track self-signed trust **per origin — scheme + host + port**,
    and a `fetch()`/XHR failure offers no click-through at all in Safari.
    That is why the init script symlinks timps's cert/key to uhttpd's.
25. **"Get a Let's Encrypt certificate for the camera."** Structurally
    impossible here: public CAs cannot issue for private IPs or `.local`
    names, HTTP-01 needs per-camera public exposure, and on a T31L rootfs
    with ~12 KB headroom even uacme does not fit. A private fleet CA is the
    documented alternative.
26. **"Enable the HTTP→HTTPS redirect."** `BR2_PACKAGE_THINGINO_UHTTPD_HTTP_REDIRECT`
    is a **dead Kconfig symbol**, and `S60uhttpd`'s "redirects to HTTPS"
    startup line is aspirational text — port 80 verifiably answers `200`.

**Operations**

27. **"If `timpsd` dies something will restart it."** **Nothing will.**
    `/etc/inittab` has no respawn line and `S95timps` is start/stop only.
28. **"Keep power-cycling until it comes up."** After the one-shot recovery
    reboot has been spent, timps says so explicitly and stays down. That
    message means the board needs looking at, not another reboot.
29. **"`S95timps restart` is safe."** Its wait-for-stop guard is defeated by
    its own unconditional pidfile delete, so a slow IMP/rmem teardown is
    followed immediately by a start on top of a still-exiting instance —
    exactly the ISP-init failure the guard exists to prevent. Worse, plain
    **`S95timps stop` can leave an orphaned, still-exiting `timpsd` with its
    pidfile already deleted**: `status` then reports "not running" while the
    ISP is still held, and the next `start` is refused by the singleton lock.
    Check `pidof timpsd`, not the pidfile. Prefer stop, wait, start — or
    reboot.
30. **"`scp` the page from `package/timps/files/www/` onto the camera."**
    Those files are **un-assembled** (~300 lines). `assemble_plugins.py` runs
    only at target-finalize and is what injects the plugin script tags. Copy
    from `$(O)/target/var/www/` instead.
31. **"`rm` the stale file in `$(TARGET_DIR)` and rebuild."** Target-finalize
    re-applies the per-package trees over it.
32. **"`make <pkg>-dirclean <pkg>` rebuilds the package properly."** It does
    not update `target/`. Use `make CAMERA=… IP=… rebuild-timps`, which adds
    `-reinstall` and `target-finalize`.
33. **"Edit `.config` to change a build option."** `force-config` deletes it,
    and `rebuild-%` depends on `force-config`. Use
    `user/<camera>/local.fragment`.
34. **"Flash with `ota-rootfs`, it is faster."** It writes the data partition
    with **no size check at all** and refuses an oversized rootfs only after
    remounting `/` read-only. After a from-scratch build, use full `make ota`.
35. **"The rootfs partition is N KB, that is the hardware limit."** The
    rootfs partition is **cut to fit the image at U-Boot build time**. The
    number on a given camera is the imprint of whatever image last fully
    flashed it.
36. **"The build failed loudly if the image was too big."** The pack-time
    size checks are `printf` only — the build exits 0 with an oversize
    image — and `ROOTFS PARTITION OVERFLOW` is arithmetically unreachable.
37. **"`GET /control`'s `version` tells you what is running."** It can lie:
    the version is computed from `git describe` against the override source
    directory at parse time, so a non-git override dir makes the binary
    report the pinned tag. There is a recorded incident where the flash tool
    reported success on cameras whose binary had not changed.
38. **"Remove `libgcc_s.so.1`, nothing links it."** uClibc `dlopen()`s it for
    `pthread_cancel` stack unwinding with no `DT_NEEDED` entry. Removing it
    aborts the daemon right after audio init.
39. **"Trust the encoder read-back after a live rate-control POST."** On a
    camera with no client attached, `encoder.<n>.rc` stays frozen forever
    because the channel is not encoding. Attach a client for a couple of
    seconds first.
40. **"`{"record":{"active":1}}` returned 200, so it is recording."** It is
    not a promise. Read `recording`, `write_errors`, `last_error`,
    `motion_gate_enabled` and `manual_off`.

---

## 15. Closing note for the assistant

This code base has one signature failure class, and the project says so
itself: **"accepted, persisted, faithfully echoed back — and silently
ignored."** It has recurred at least ten times. When a user says *"I set X
and nothing happened"*, the first hypothesis should not be that they typed it
wrong. Check, in this order:

1. Is the feature **compiled in**? (`caps`, `*.available`)
2. Is the key **known to this build**? (`GET /control?fields=1`, and the POST
   reply's `ignored` list)
3. Is it **live or restart-required**? (`caps.restart`, `caps.video_live`,
   the POST reply's `deferred_keys`)
4. Is it **supported on this SoC**? (`caps.image`, `caps.rotation`,
   `caps.motion`)
5. Was it **clamped or coerced**? (the POST reply's `applied` echo)
6. Only then: is the value wrong?

And one corollary from the project's own QA notes: *a test that configures a
value the daemon ignores does not fail — it quietly stops testing.* The same
is true of a support answer.

---

## 16. Shipped scripts, CGIs and helpers

Everything the thingino package puts on the camera besides `timpsd` itself
(`package/timps/files/`). Most "timps is broken" reports that are not about
`timpsd` are about one of these. Install rules are in
`docs/ai/config-keys.md` §22.2 and §22.10.

### 16.1 Init scripts and daemonside helpers

| File | What it does | Trigger | On failure | Pitfalls |
| --- | --- | --- | --- | --- |
| `/etc/init.d/S95timps` | `start \| stop \| restart \| status`. Provisions TLS certs, then `start-stop-daemon -S -b -m`. | Boot (runlevel 95), or by hand. | `stop` prints `timpsd still stopping...` and deletes the pidfile anyway. | `wait_stop` is ~5 s **or 50 s** (§11.6); `restart` can start on a still-exiting instance; no `reload`. |
| `/etc/init.d/S96onvif_discovery` | Builds `/etc/onvif.json` from `/etc/timps.conf` (+ a live `GET /control` fallback for auto-detected values) and starts `wsd_simple_server` per interface. | Boot, after S95timps. | Prints `Disabled` and **exits 1** in portal mode or when the default gateway does not ping. | **Probe-gated install** — absent on an ONVIF-free image. Advertises a `snapurl` only for channels whose `videoN.jpeg` is on. |
| `/etc/init.d/S48webui-config` | Fixes stock `device.motors=true` reporting so a non-PTZ camera does not get a joystick. | Boot. | — | Installed only with `BR2_PACKAGE_THINGINO_WEBUI`; can be lost to the per-package merge (§9.2). |
| `/usr/bin/generate-timps-tls-certs.sh` | Self-signed cert/key generator. | `ensure_tls_certs()` step 5. | `S95timps` logs `TLS cert generation failed: <out>` via `logger -t timps`. | Installed only with `BR2_PACKAGE_TIMPS_TLS`; without it `ensure_tls_certs` silently gives up. |
| `/usr/libexec/agent/adapter.sh` (`agent-adapter`) | thingino-agent backend: reads file-only keys from `/etc/timps.conf` and live ones from `GET /control`. | thingino-agent. | — | Installed **unconditionally**, overwriting thingino-agent's null fallback. |

### 16.2 Day/night and motion

| File | What it does | Trigger | On failure | Pitfalls |
| --- | --- | --- | --- | --- |
| `/usr/sbin/color` | `POST {"image":{"running_mode":0\|1}}` over loopback; mirrors the mode into `/tmp/colormode.txt`. | `/usr/sbin/daynight`, i.e. `daynight.switch_cmd`. | Logs `POST <url> failed - color mode NOT set (timpsd down?)` and returns 1; the mode file is **not** written, so `color status` stays honest. | **No `case` arm for `http.https = 2`** → falls back to `http://`, gets a 426, day/night stops switching (§8.4). |
| `/usr/bin/timps-irprobe` | `light ir940\|ir850\|ir on\|off`, first pin that exists. | `daynight.irprobe_cmd`, the silent probe. | Exits 1 when no switchable illuminator exists; timps then retires the silent probe and falls back to the audible IR-cut probe. | `off` arms a **detached 60 s watchdog** that re-lights the illuminator, nonce-checked through `/run/timps-irprobe.<type>` so a newer `on`/`off` invalidates it. Installed **unconditionally**, not under `DIAG_TOOLS`. |
| `/usr/sbin/timps-motion` | Motion → send2 bridge: snapshot over HTTP + `POST {"record":{"clip":…}}`, then runs the enabled `send2*` services. | `motion.on_motion`. | **Always `exit 0`** — timps never sees a failure. Logs `snapshot fetch failed …` / `clip capture failed …` **once** via `logger -t timps-motion`, gated by `/tmp/.timps-motion-nophoto` / `-noclip` (so once per boot until it recovers). | Returns immediately and **silently** while `/run/motors-active` exists (no log at all) — a PTZ move suppresses motion notifications. `flock -n /run/timps-motion.lock` drops overlapping events. Hardcodes `http://`, so `http.https = 2` breaks it. Installed unconditionally; with `TIMPS_CONTROL=n` the clip POST can never work. |
| `/usr/sbin/{daynight,ircut,light}`, `/etc/init.d/S06ircut` | thingino's board hardware drivers, installed **from the daynightd package**. | `daynight.switch_cmd` chain. | `'<cmd> <arg>' failed (rc=127)` in timps's log. | On an image where these are missing the IR-cut filter never moves — the recorded purple-image incident (§11.2). |

### 16.3 send2 / notifications

| File | What it does | Trigger | On failure | Pitfalls |
| --- | --- | --- | --- | --- |
| `/usr/sbin/send2common` | Shared helpers for the `send2*` services; `copy_photo` fetches a snapshot. | The `send2*` tools. | Silent — the notification goes out without media. | **Hardcodes `http://127.0.0.1:8880`** in `copy_photo`, so a non-default `http.port` *or* `http.https = 2` loses the photo. Installed only with WEBUI + `BR2_THINGINO_DEV_IPCAM` + `TIMPS_CONTROL`. |
| `/usr/sbin/telegram-cam-register` | Registers the camera (id from `soc -s`, else a MAC) under `thingino/cam` for the Telegram bot. | Setup / the Telegram service. | Honours `/etc/telegram-cam-registration.disabled`. | Same install guard as `send2common`. |

### 16.4 Audio

| File | What it does | Trigger | On failure | Pitfalls |
| --- | --- | --- | --- | --- |
| `/usr/sbin/play` | Enqueues `PLAY`/`STOP` onto timps's FIFO `/run/timps/audio_out`. `play stop` clears the queue. | WiFi-portal prompts, the post-upgrade chime, ESPHome/HA media_player. | `set -eu`, so a bad option or a missing FIFO exits non-zero; the caller usually ignores it. | Installed only with `BR2_PACKAGE_TIMPS_PLAY`; without it every one of those integrations is a silent no-op. The **backchannel always preempts** the play queue (§6.5). |

### 16.5 WebUI CGIs (`/var/www/x/`)

All of these `. /var/www/x/auth.sh` and `require_auth` first — they are
reachable by **any authenticated WebUI session**.

| File | What it does | On failure | Pitfalls |
| --- | --- | --- | --- |
| `x/ch0.jpg` (also `ch1`, `dl0`, `dl1`, and the `onvif/image*.cgi` symlinks) | Proxies `/snapshot.jpg?chn=N` over loopback; channel and `Content-Disposition` derive from the invoked name, `?chn=0\|1` overrides. | `502 Bad Gateway` with the body `snapshot unavailable` — deliberately, so an NVR sees a failure rather than a bodyless 200. | Reads `http.port`/`http.https` from the conf, but **no `2` arm** → always 502 on TLS-only (§8.4). |
| `x/timps-heartbeat.sh` + `x/json-heartbeat.cgi`, `x/json-heartbeat-slow.cgi` | The WebUI's live status poll; both CGIs are re-applied by a finalize hook (§9.2). | Empty response → the WebUI status panel goes blank. | Same missing `2` arm as `ch0.jpg`. |
| `x/json-recordings.cgi` | `GET` lists segments under `<record.dir>/<host>/records`; `?file=` streams one; **`?del=` deletes one.** | — | **Any authenticated WebUI session can delete recordings** through `?del=`. Path traversal is guarded (`safe()` + `within_base()`), deletion authority is not. |
| `x/json-timelapse.cgi` | `GET` indexes shot **folders**; `?seq=` lists one folder's frames; `?file=` serves one JPEG. | — | Indexes folders, not frames, on purpose — a 7-day tree at 10 s holds ~60k JPEGs. |
| `x/restart-prudynt.cgi` (also installed as `restart-timps.cgi`) | Backgrounds `/etc/init.d/S95timps restart`. | **Always answers `{"status":"ok"}`** before the restart even runs — the JSON says nothing about whether the daemon came back. | Inherits every `restart` hazard in §11.6. The name is prudynt's because the WebUI calls that URL; an unconditional "remove stock prudynt files" once deleted it (§9.1). |
| `x/timps-imp.cgi` | Generic `/control` POST bridge for WebUI pages. | — | One of the two scripts that **does** handle `http.https = 2`. |
| `x/timps-token.cgi` | Hands the per-boot `/run/timps.token` to the page, plus `port`, `tls` and a `scheme` of `http`/`https`/**`both`**. | When the token file is unreadable it returns `{"error":"no token available", …}` **with HTTP 200** — JS that only checks the status code sees success and then sends `X-Timps-Token: undefined`. | Only the per-boot token is ever exposed; a configured `http.token` never reaches the file. On `scheme: "both"` the page must follow its own protocol (mixed content / self-signed interstitial). |

### 16.6 Motors UI (`/var/www/motors-ui/`)

Installed only when `BR2_PACKAGE_THINGINO_MOTORS` **and**
`BR2_PACKAGE_THINGINO_STREAMER_TIMPS` are both set; `json-motor-token.cgi`
additionally needs `BR2_PACKAGE_THINGINO_MOTORS_WS`.

> **`json-motor.cgi`, `json-motor-params.cgi` and `json-motors-config.cgi` all
> `eval` a string built from request data.** `json-motor.cgi` does
> `eval $(echo "$QUERY_STRING" | sed "s/&/;/g")` — unquoted, so the query
> string is executed as shell. The other two `eval` `KEY="value"` assignments
> that `awk` builds from the POST body, which a `"` or a `$(…)` escapes. All
> three are behind `require_auth`, so this is a **privilege surface for any
> authenticated WebUI user**, not an unauthenticated one — but it is worth
> saying plainly rather than describing these as ordinary CGIs.

### 16.7 Diagnostics (`BR2_PACKAGE_TIMPS_DIAG_TOOLS`, default **n**)

| File | What it does | Pitfalls |
| --- | --- | --- |
| `/usr/bin/timps-selftest` (`timps-selftest.sh`) | End-to-end probe of the live daemon. | **It mutates the camera**: it toggles privacy regions 0 and 3 and starts/stops recording. A run killed part-way **leaves a privacy mask on the picture**. Handles `http.https = 2` correctly. Its `RESULT:` line is ANSI-coloured, so `grep -q "^RESULT:"` never matches on WARN/FAIL. |
| `/usr/bin/timps-dn-isp-log` | Periodic day/night + ISP sample logger (`dnlog.isp_syslog`). | **No cron entry is shipped** — nothing calls it. |
| `/usr/bin/timps-logcat-ship` | Ships the syslog ring somewhere. | Same: no cron entry, nothing calls it. |
