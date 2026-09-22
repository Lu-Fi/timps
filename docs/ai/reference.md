# timps — Reference for a thingino Community Support Assistant

**Audience: an AI assistant answering thingino user questions in Discord.**
This document describes `timps`, one of the streamer choices in thingino
firmware. Everything here is verified against the `main` branch of
<https://github.com/Lu-Fi/timps> and against `package/timps/` in a thingino
firmware tree.

Applies to timps v1.9.18 (source: `main`, 2026-09-15)

A statement marked **since v1.9.19 (unreleased)** is in the source but in no
tagged release yet — on a v1.9.18 camera describe the previous behaviour.

## Where to find what

| Question | Document |
| --- | --- |
| "What is the exact default / range / apply mode of key `X`?" | **`docs/ai/config-keys.md`** — every config key, verified key by key against `src/config.c`. It owns the key tables; this file only summarizes them. |
| "What does this log line mean?" / "Why is the camera doing X?" | **`docs/ai/troubleshooting.md`** — log dictionary, error/status codes, misconfiguration gallery, and a "what NOT to advise" list of plausible-but-wrong answers. |
| Feature background and design rationale | **`docs/wiki/`** — the human wiki (`Configuration-Reference.md`, `Day-Night.md`, `Audio.md`, `Motion-Detection.md`, `Rate-Control-*.md`, …). |
| Everything else — what timps is, how it is built and gated, how to answer a user | this file. |

If this file and `config-keys.md` disagree about a key, `config-keys.md` wins;
if either disagrees with the source, the source wins.

Rules for using this document:

- Prefer quoting exact key names, paths, endpoints and Kconfig symbols from
  here over paraphrasing. Wrong key names are the most common way to send a
  user on a dead end.
- Features in timps are **build-gated**. Before telling a user "do X", check
  whether X exists in their build. The camera answers that itself:
  `GET /control` → the `caps` object and the `*.available` flags. Always
  recommend that check instead of guessing from a version number.
- If a user's camera runs **prudynt-t** (the thingino default streamer), none
  of the config keys below apply. prudynt uses `/etc/prudynt.cfg`, a different
  format and a different control interface. Ask which streamer is installed
  (`ls /usr/bin/timpsd` or "does your WebUI show a timps version?") before
  giving timps-specific instructions.

---

## 1. What timps is

`timps` = "Tiny IMP Streamer". A single-binary (`/usr/bin/timpsd`), pure-C
RTSP / fragmented-MP4 / MJPEG streamer for Ingenic-SoC IP cameras, written
directly against the vendor `libimp` library. No live555, no libconfig, no
libwebsockets, no libschrift. Stripped binary is roughly **360 KB** (mipsel)
(unverified — depends on the enabled `USE_*` set; measure the actual build).

- Source: <https://github.com/Lu-Fi/timps>
- Docs (in-repo wiki, `docs/wiki/`): `Home.md`, `Architecture.md`,
  `Building.md`, `Configuration-Reference.md`, `HTTP-Control-API.md`,
  `Streaming-Protocols.md`, `Day-Night.md`, `Audio.md`, `Motion-Detection.md`,
  `Recording-Timelapse.md`, `Rate-Control-Bandwidth.md`,
  `Rate-Control-Parameters.md`, `Logging.md`, `Testing-QA.md`,
  `Platform-SDK-Support.md`. Also `docs/rotation.md` and
  `docs/sdk-feature-gaps.md`.
- Config file: `/etc/timps.conf`, plain `key = value`
- Init script: `/etc/init.d/S95timps` (start/stop/restart)
- Per-boot control token: `/run/timps.token`
- Default HTTP port **8880**, RTSP **554**, RTSPS **322** (when enabled)

### Why a user might choose timps over prudynt-t

Grounded differences (do not oversell — these are the ones verifiable from
timps's own source/docs):

| Aspect | timps |
| --- | --- |
| Config format | flat `key = value` in `/etc/timps.conf` (prudynt uses a libconfig-style `/etc/prudynt.cfg`) |
| Control API | `POST`/`GET /control` JSON over plain HTTP + `GET /events` SSE push |
| Dependencies | libimp + pthread only; AAC/TLS/SRT/Opus are optional links |
| Day/night | built in (`USE_DAYNIGHT`), replaces thingino's separate `daynightd` |
| Recording | built-in fMP4 segment recording to SD + native JPEG timelapse |
| WebRTC | optional WHEP endpoint (`/webrtc/whep`), LAN/VPN only |
| SRT | optional MPEG-TS over SRT, listener or caller |
| Speaker | owns `IMP_AO` natively — no `/bin/iac` / ingenic-audiodaemon needed |
| Per-stream OSD | independent overlay set per video stream, own TrueType rasterizer |

Do **not** claim timps is faster/better than prudynt in general terms; stick to
feature presence and the config/API differences above.

### Supported SoCs

**T10, T20, T21, T23, T30, T31, T40, T41, C100.**

The Buildroot package has one hard exclusion: `depends on !BR2_SOC_FAMILY =
"a1"` (no Ingenic A1). Everything else is a capability difference, not an
exclusion:

- Encoder API split: "new" (`IMP_Encoder_SetDefaultParam`) on **T31, C100,
  T40, T41**; "classic" (manual `IMPEncoderChnAttr`) on **T10, T20, T21, T23,
  T30**. This is why `videoN.bitrate` means different things (see §7).
- ISP tuning API split: `ISP_NEW_TUNING_API` only on **T40/T41** — several
  `image.*` knobs do not exist there (highlight depress, sinter/temper NR,
  max again/dgain, manual WB, ISP hflip/vflip).
- `audio.alc_gain` (analog PGA) exists only on **T21, T31, C100**.
- Rotation: hardware I2D on **T40/T41**, FrameSource rotate on **T31**,
  software on **T23** (opt-in), none on **T10/T20/T21/T30/C100**.
- A host build with no `PLATFORM_*` macro (`make sim`) reports every
  capability, so the WebUI can be exercised without hardware.

---

## 2. Enabling and building timps

timps is a Buildroot package, **`package/timps`**, in the thingino firmware
tree.

Select it in `make menuconfig`: **Streamer Packages → Streamer → timps**
(Kconfig symbol `BR2_PACKAGE_THINGINO_STREAMER_TIMPS`; the streamer is a
`choice`, so selecting timps deselects prudynt/raptor/strero). That enables
`BR2_PACKAGE_TIMPS`. Then `make` and flash as usual.

### Kconfig options → `USE_*` build flags

`package/timps/timps.mk` translates each Kconfig bool into a `USE_*` make
variable. **If a feature is compiled out, no config key can turn it on.**

| Kconfig symbol | Build flag | Default | Gates |
| --- | --- | --- | --- |
| `BR2_PACKAGE_TIMPS_FAAC` | `USE_FAAC` | y | AAC audio encode via libfaac. Without it `audio.codec = aac` is unavailable; use `pcmu`/`pcma`. |
| `BR2_PACKAGE_TIMPS_CONTROL` | `USE_CONTROL` | y | `/control` and `/events` endpoints, the token machinery, live settings + persistence (~15 KB). |
| `BR2_PACKAGE_TIMPS_DAYNIGHT` | `USE_DAYNIGHT` | y | The automatic day/night thread. Off → status reports `enabled=0` and `image.running_mode` is whatever was last set. |
| `BR2_PACKAGE_TIMPS_RECORD` | `USE_RECORD` | y | SD recording + `/control` clip capture (~11 KB). Off → `caps.record.available=0`. |
| `BR2_PACKAGE_TIMPS_TIMELAPSE` | `USE_TIMELAPSE` | y | Periodic JPEG timelapse (~4 KB). Off → `caps.timelapse.available=0`. |
| `BR2_PACKAGE_TIMPS_TLS` | `USE_TLS` | y | HTTPS (`http.https`) + RTSPS (`rtsp.tls`) via mbedTLS. Selects `BR2_PACKAGE_MBEDTLS`. Off → `tls.available=0` in `GET /control` and TLS keys are inert. |
| `BR2_PACKAGE_TIMPS_WEBRTC` | `USE_WEBRTC` | y | `POST /webrtc/whep`. **Depends on `TIMPS_TLS` and `TIMPS_CONTROL`**, selects `BR2_PACKAGE_MBEDTLS_DTLS_SRTP`. Off → `caps.webrtc` key is **absent** from `GET /control`. |
| `BR2_PACKAGE_TIMPS_SRT` | `USE_SRT` | n | MPEG-TS over SRT (libsrt). Off → `srt.available=0`. |
| `BR2_PACKAGE_TIMPS_STREAM_OPUS` | `USE_STREAM_OPUS` | n | `audio.codec = opus` for RTSP/RTP (links bare libopus, not opusfile). |
| `BR2_PACKAGE_TIMPS_BACKCHANNEL` | `USE_BACKCHANNEL` | n | ONVIF audio backchannel (RTSP client → speaker), native `IMP_AO`. |
| `BR2_PACKAGE_TIMPS_BC_AAC` | `USE_BC_AAC` | n | AAC decode on the backchannel (libhelix-aac). Depends on BACKCHANNEL. G.711 always works without it. |
| `BR2_PACKAGE_TIMPS_BC_WS` | `USE_BC_WS` | **y on TLS builds**, else n | Browser-microphone WebSocket at `/talk` (~8 KB). Depends on BACKCHANNEL + CONTROL. |
| `BR2_PACKAGE_TIMPS_PLAY` | `USE_PLAY` | n | System-sound play queue + `/usr/sbin/play` (FIFO at `/run/timps/audio_out`). WAV/PCM16/G.711. |
| `BR2_PACKAGE_TIMPS_PLAY_OPUS` | `USE_PLAY_OPUS` | n | Ogg-Opus decode in the play queue (opusfile). Needed for thingino's stock `.opus` sounds. |
| `BR2_PACKAGE_TIMPS_ROTATE` | `USE_ROTATE` | n | `videoN.rotation`. Off → rotation values are coerced to 0 with a warning, and `caps.rotation` is absent. |
| `BR2_PACKAGE_TIMPS_SW_ROTATE` | `USE_SW_ROTATE` | n | Software 90/270 on T23. Depends on ROTATE. |
| `BR2_PACKAGE_TIMPS_OSD_HINTING` | `USE_OSD_HINTING` | n | Compiles the OSD geometric autohinting pass (~2 KB). |
| `BR2_PACKAGE_TIMPS_DIAG_TOOLS` | — | n | Installs `timps-selftest`, `timps-logcat-ship`, `timps-dn-isp-log`. |
| `BR2_PACKAGE_TIMPS_DROP_LIBSTDCPP` | — | n | Target-finalize hook that removes `libstdc++.so.6` when nothing links it (flash saving on small rootfs). |

There is also an **"Audio backchannel preset"** choice bundling the
interdependent audio knobs:

- `..._AUDIO_PRESET_MANUAL` (default) — set the individual options yourself.
- `..._AUDIO_PRESET_FULL` — BACKCHANNEL + BC_AAC + PLAY + PLAY_OPUS +
  thingino-sounds in Opus. ~395 KB of extra libraries; fits T31-family flash,
  too large for a ~5 MB T20 rootfs.
- `..._AUDIO_PRESET_MINIMAL` — BACKCHANNEL + PLAY with G.711 sounds only, no
  extra codec libraries. Confirmed on a T20 with a ~5 MB rootfs.

### Standalone cross-compile (no firmware tree)

```sh
git clone --recurse-submodules https://github.com/Lu-Fi/timps
cd timps
make PLATFORM=T31 CROSS_COMPILE=mipsel-linux-
make strip PLATFORM=T31 CROSS_COMPILE=mipsel-linux-
```

`PLATFORM ∈ {T10 T20 T21 T23 T30 T31 T40 T41 C100}`. IMP headers come from the
`gtxaspec/ingenic-headers` git submodule under `include/` — a clone without
`--recurse-submodules` will fail to build (fix: `git submodule update --init`).

Host simulation without hardware: `make sim` → `./timpsd-sim -c timps.conf`,
fed by `sim.video0` / `sim.video1` / `sim.audio` / `sim.jpeg` file paths.

**Important caveat to relay:** a standalone `build.sh`/`make` binary and the
firmware package binary are usually **not compiled with the same feature set**
(the firmware package links mbedTLS, a plain `make` does not). The version
string cannot tell them apart. Use `GET /control`'s `tls.available`,
`srt.available` and the `caps.*` flags.

---

## 3. The config file

**Path: `/etc/timps.conf`. Format: flat `key = value`, `#` comments,
sections are just key prefixes.** No nesting, no JSON, no libconfig.
`timps.conf.example` in the repo is the fully commented reference.

Changes made through `POST /control` are written back into this file (only the
changed keys, atomic tmp+rename). Hand edits need a restart:
`/etc/init.d/S95timps restart`.

The thingino package ships a **trimmed** `/etc/timps.conf` (about 46 active
keys) — most keys are absent and the compiled-in defaults apply. Notably it
ships `rtsp.user`/`rtsp.pass` and `http.user`/`http.pass` all set to
**`thingino`/`thingino`**, `motion.on_motion = /usr/sbin/timps-motion`, and
`general.debug_modules = daynight`.

### Section map

| Prefix | Governs |
| --- | --- |
| `general.` | Log level, per-module debug, syslog, IMP polling timeout, OSD pool size |
| `sensor.` | Sensor model / I²C address / fps / width / height (all optional — auto-detected from `/proc/jz/sensor/sensor0/`) |
| `image.` | ISP tuning: brightness, contrast, NR, WB, flips, day/night running mode |
| `video0.` / `video1.` | Per-stream encoder: codec, geometry, fps, bitrate, rate control, GOP, QP, rotation, RTSP path, piggyback JPEG |
| `jpeg.` | The dedicated JPEG/MJPEG encoder channel (`/snapshot.jpg`, `/stream.mjpeg`) |
| `osd.` and `osd<S>.<N>.` | Global OSD settings, and per-stream overlay items |
| `privacy<S>.<N>.` | Privacy cover rectangles per stream |
| `audio.` | Mic capture, codecs, DSP, speaker, backchannel, `/talk` |
| `rtsp.` | RTSP server: enable, port, MTU, credentials, RTSPS |
| `http.` | HTTP server: enable, port, preview channel, credentials, token, TLS cert paths, `http.https` |
| `webrtc.` | WHEP endpoint gating and UDP media ports |
| `srt.` | SRT output mode, port, latency, streamid, passphrase |
| `events.` | `/events` SSE: enable, stats period, client cap |
| `motion.` | IVS motion grid, sensitivity, hook |
| `record.` | Local SD recording |
| `timelapse.` | Periodic JPEG timelapse |
| `daynight.` | Automatic day/night detection |
| `sim.` | Host-simulation file inputs (ignored on device) |

### Key reference with real defaults

A **summary** only. `docs/ai/config-keys.md` is the exhaustive, code-verified
table (default, range, apply mode, and what each key actually does) and is the
one to quote when a user asks about a specific key.

**`general.`**
```
general.loglevel            = 2      # 0 err, 1 warn, 2 info, 3 debug
general.debug_modules       =        # comma list, max 8; raises those modules to DEBUG
general.syslog              = 1      # also log to syslog (visible in `logread`)
general.imp_polling_timeout = 500    # ms
general.osd_pool_size       = 1024   # KB
```
Module tags for `debug_modules`: `MAIN CONFIG CTRL DAYNIGHT HAL_ING HTTP HUB
MOTION OSD REC RTSP SRT TL HAL_SIM TLS AAC bc spk TRACE`. This key is live over
`/control` — no restart needed, which matters because raising the global level
destroys the 64 KB syslog ring you were trying to read.

**`sensor.`** — `model`, `i2c_addr`, `fps`, `width`, `height`. All optional;
omitted values are read from the kernel sensor registry. A configured value
always wins. T40/T41 and the host sim have no registry, so set them there.

**`image.`** (all 0..255 unless noted, 128 = neutral; all live-applicable)
`brightness`, `contrast`, `saturation`, `sharpness`, `hue`, `hflip`, `vflip`,
`anti_flicker` (0 off / 1 = 50 Hz / 2 = 60 Hz), `running_mode` (0 day, 1 night),
`ae_compensation`, `max_again` (160), `max_dgain` (80), `sinter_strength`
(spatial NR), `temper_strength` (temporal NR), `dpc_strength`,
`defog_strength`, `drc_strength`, `highlight_depress` (0), `backlight_compensation`
(0), `core_wb_mode` (0 = auto), `wb_rgain`, `wb_bgain`, `ae_it_max_us`
(opt-in cap on AE integration time).
Which of these the SoC really supports is listed in `caps.image`. Unsupported
values still persist; the HAL skips them.

**`video0.` / `video1.`**
```
videoN.enabled   = 1
videoN.codec     = h264            # h264 | h265
videoN.width/height/fps
videoN.bitrate   = 3000 / 512      # kbps (meaning differs per SoC, see §7)
videoN.rc_mode   = cbr             # cbr|vbr|fixqp|smart|capped_vbr|capped_quality
videoN.gop       = 50
videoN.profile   = 2               # 0 baseline, 1 main, 2 high
videoN.qp        = 35              # fixqp only
videoN.min_qp    = 20
videoN.max_qp    = 45
videoN.quality_lvl / change_pos / i_bias_lvl / fluc_lvl   # classic SoCs only
videoN.rotation  = 0               # needs USE_ROTATE
videoN.buffers   = 2
videoN.rtsp_path = /ch0 | /ch1
videoN.imp_chn   = 0 | 1
videoN.jpeg      = true            # piggyback JPEG encoder at this resolution
videoN.jpeg_quality / jpeg_fps / jpeg_chn
```

**`jpeg.`** — `enabled` (**compiled default 0**; the shipped example file sets
it to 1), `width` 640, `height` 360, `quality` 75, `fps` 5, `imp_chn` 2,
`snapshot_path` (optional periodic file dump). With the dedicated channel off,
`/snapshot.jpg` and `/stream.mjpeg` without `?chn=` fall back to the first video
stream that has `videoN.jpeg = true` (which defaults to on), so they still work
— just at that stream's resolution rather than `jpeg.width`×`jpeg.height`.

**`rtsp.`** — `enabled` 1, `port` 554, `mtu` 1200 (548–1472), `user`, `pass`,
`tls` 0, `tls_port` 322.

**`http.`** — `enabled` 1, `port` 8880, `preview_chn` 1, `adaptive_drop` 1,
`user`, `pass`, `token` (empty), `token_file` `/run/timps.token`,
`https` 0 (tri-state), `tls_cert` `/etc/ssl/certs/timps.crt`, `tls_key`
`/etc/ssl/private/timps.key`.

**`webrtc.`** — `enabled` (**default 2 on `USE_WEBRTC` builds**), `port` 0,
`port_max` 0, `channel` 0.

**`srt.`** — `enabled`, `port` 9000, `mode` (`listener` default | `caller`),
`host` (caller), `channel` 0, `latency_ms` 120, `streamid`, `passphrase`.

**`events.`** — `enabled` 1, `stats_ms` 2000 (0 = no stats events),
`max_clients` 8. Startup-only, deliberately not settable via `/control`.

**`audio.`** — `enabled` 1, `codec` `aac` (aac|pcmu|pcma|opus|none),
`codec2` (`pcmu` default on `USE_WEBRTC` builds, else off), `samplerate` 16000,
`channels` 1, `bitrate` 32, `volume` 80, `gain` 25, `alc_gain` 0,
`high_pass` **1**, `agc` 0, `agc_target_dbfs` 10, `agc_compression_db` 0,
`ns` 0, `mute` 0, `aec` 0, `talk_ws` 0, `force_stereo` 0,
`spk_enabled` 1, `spk_volume` 80, `spk_gain` 25, `backchannel` 0,
`backchannel_codec` `pcmu`, `backchannel_rate` 16000.

**`osd.`** — `enabled` 1, `monitor_stream` 0, `font_path`
`/usr/share/fonts/default.ttf` (empty = built-in bitmap font), `vars_file`
`/tmp/timps_osd.vars`, `supersample` 2 (1–4), `hinting` 1. Items:
`osd<S>.<N>.{enabled,type,text,x,y,font_size,color,transparency,
outline,outline_color,logo,logo_w,logo_h,font_path}`. `type` is `text` or
`logo`. Position convention: `0` = centered on that axis, positive = from
left/top, negative = from right/bottom. Text placeholders: strftime tokens plus
`{hostname} {ip} {mac} {fps} {bitrate} {uptime}`, stream-scoped `{fpsN}` /
`{bitrateN}`, and any `{name}` written into `osd.vars_file` as `name = value`.
`font_size` is absolute pixels and is **not** auto-scaled per stream.
Legacy `osd<N>.<field>` keys still load and apply to every stream.

**`privacy<S>.<N>.`** — `enabled`, `x`, `y`, `w`, `h`, `color` (0xAARRGGBB).
Max 4 regions per stream (`caps.privacy.max_regions`).

**`motion.`** — `enabled` 0, `monitor_stream` 0, `sensitivity` 128 (0–255,
mapped to the SDK's 0–4), `cols` 5, `rows` 5, `cooldown_ms` 5000, `hold_ms` 800,
`skip_frames` 5, `on_motion` (path to an executable, run without a shell and
without arguments).

**`record.`** — `enabled` 0, `channel` 0, `mode` (`continuous` | `motion`,
default motion), `dir` `/mnt/mmcblk0p1`, `name`
`%Y%m%d/%H/%Y%m%dT%H%M%S`, `segment_s` 60, `pre_roll_s` 3, `post_roll_s` 10,
`min_free_mb` 200, `audio` 1. Files land in `<dir>/<hostname>/records/`.

**`timelapse.`** — `enabled` 0, `channel` 0, `dir` `/mnt/mmcblk0p1`, `name`
same strftime pattern, `interval_s` 60, `keep_days` 7. Files land in
`<dir>/<hostname>/timelapses/`.

**`daynight.`** — `enabled` 1, `mode` `auto` (|`schedule`), `day_gain` 768,
`night_gain` 4096, `day_confirm_s` 30, `probe_min_gap_s` 600,
`probe_confirm_s` 15, `heartbeat_s` 14400, `heartbeat_max_s` 43200,
`boot_probe` 1, `interval_ms` 2000, `switch_cmd` `daynight`,
`irprobe_cmd` `timps-irprobe`, `isp_path` `/proc/jz/isp/isp-m0`,
`diagnose_thresholds` 0, `history_s` 0 (max 172800), `trace_path` (tmpfs only),
plus the optional calendar: `time_night_start` / `time_day_start` (HH:MM),
`sun_latitude` / `sun_longitude`, `sun_sunrise_offset_min` /
`sun_sunset_offset_min`.

**Keys that are file-only (never settable over HTTP)** — say so if a user
reports a POST being ignored (the common ones; `config-keys.md` marks every
key): `general.loglevel`, `general.imp_polling_timeout`,
`general.osd_pool_size`, `motion.cooldown_ms`,
`motion.on_motion`, `daynight.switch_cmd`, `daynight.isp_path`,
`daynight.irprobe_cmd`, `daynight.trace_path`, `events.*`, `rtsp.*`, `http.*`,
`srt.*`, `webrtc.*`, `jpeg.*`. By contrast `osd.supersample` and `osd.hinting`
**are** POST-able on current `main`, but are restart-required (read once at OSD
setup), so a POST that "does nothing" there is expected until a restart.

**Removed / now-fixed constants** (a user's old config may still contain them;
they parse but are ignored, with one warning if the value differs):
`daynight.probe_jump_pct`, `probe_settle_s`, `ref_delay_s`, `ir_ratio_night`,
`ir_ratio_day`, `ir_min_headroom`, `boot_settle_s`, `transition_s`, and the
removed `daynight.learn` / `daynight.state_path`.

**Aliases that still work**: `videoN.mode` → `rc_mode`, `record.segment` →
`segment_s`, `osd0.0.stroke` → `outline`,
`daynight.total_gain_day_threshold` → `day_gain`,
`daynight.total_gain_night_threshold` → `night_gain`, and the legacy flat
`/control` form `{"force_mode":"night"|"day"}` → `image.running_mode`.

---

## 4. Stream endpoints — and which to recommend

| Endpoint | Notes |
| --- | --- |
| `rtsp://<ip>:554/ch0` | Main stream, video + audio. Path is `video0.rtsp_path`. |
| `rtsp://<ip>:554/ch1` | Sub stream (`video1.rtsp_path`). |
| `rtsps://<ip>:322/chN` | Only when `USE_TLS` and `rtsp.tls = 1`. |
| `http://<ip>:8880/` | Browser preview page (fMP4 over MSE). |
| `http://<ip>:8880/stream.mp4` | Fragmented MP4 — works in ffplay/VLC and browsers. |
| `http://<ip>:8880/snapshot.jpg` | Latest JPEG frame. |
| `http://<ip>:8880/stream.mjpeg` (alias `/mjpeg`) | MJPEG multipart. |
| `…?chn=N` | Selects a video stream. On `/snapshot.jpg` and `/stream.mjpeg` it is **strict** — it picks that stream's piggyback JPEG encoder and 404s if `videoN.jpeg` is off, with no fallback. On `/stream.mp4` and the player page `/` it falls back to `http.preview_chn`, then to `0` if that stream is boot-disabled. |
| `…?embed` | Player page only: any path containing the substring `embed` renders without the heading, for an `<iframe>`. |
| `/stream.mjpeg?boundary=X` | Overrides the multipart boundary (to match what a proxy already announced). Sanitised to `[A-Za-z0-9_-]`, other bytes become `_`; default and empty-result fallback is `msmjpeg`. |
| `http://<ip>:8880/control` | JSON control API (`USE_CONTROL`). |
| `http://<ip>:8880/events` | SSE push stream (`USE_CONTROL`). |
| `wss://<ip>:8880/talk` | Browser mic → camera speaker (`USE_BC_WS`). |
| `http://<ip>:8880/webrtc/whep` | `POST` SDP offer; `DELETE /webrtc/whep/<id>` (`USE_WEBRTC`). |
| `srt://<ip>:9000` | MPEG-TS over SRT listener (`USE_SRT`, `srt.enabled = 1`). |

**Which transport to recommend:**

- **NVR / Frigate / Home Assistant / long-term reliability** → RTSP.
- **A browser tab, any browser, tolerant of a second or two of latency** →
  `/stream.mp4` (fMP4/MSE). This is the WebUI's default player.
- **Low latency in a browser, on the LAN or a VPN** → WebRTC/WHEP — but only
  Chromium-family browsers are a safe recommendation (see §8).
- **A still image for automation** → `/snapshot.jpg` (on-demand, cheap).
- **Contribution over an unreliable/NAT'd link to a remote receiver** → SRT in
  `caller` mode.

**RTSP path matching is forgiving, not strict.** `find_video_by_path()`
(`src/rtsp/rtsp.c`) tries each **boot-enabled** stream's `rtsp_path` as a
prefix, and if none matches **falls back to the first boot-enabled stream**.
So `rtsp://<ip>/anything` serves `ch0`, and `rtsp://<ip>/ch1` on a camera with
`video1.enabled = 0` also serves `ch0` — silently, at ch0's resolution.
`404 Not Found` on DESCRIBE/SETUP means **no video stream is enabled at all**,
not "that path is wrong". A user who insists they are watching the substream
and sees main-stream dimensions has usually hit this.

**On-demand encoding:** every stream is only captured and encoded while at
least one client is attached. There is nothing to configure. Two consequences
worth telling users: idle CPU is near zero, and **a live `encoder.<n>.rc`
readback will look frozen on a camera nobody is watching** — attach a client
for a couple of seconds before trusting it.

**`HEAD` for health checks.** Three different behaviours, and the difference
matters if a monitoring system polls the camera:

| Path | `HEAD` behaviour |
| --- | --- |
| `/snapshot.jpg` | **Grabs a real frame** and returns its true `Content-Length`, body suppressed. This *does* wake the JPEG pipeline — it is a full snapshot minus the bytes. |
| `/stream.mp4`, `/stream.mjpeg` | Headers only (including the MJPEG boundary). **No encoder wake-up**, no subscription. |
| `/events` | Headers only, **no SSE body and no client slot consumed** — it does not count against `events.max_clients`. |
| `/control` | Same as `GET`: the JSON is built and its `Content-Length` reported, then the body is suppressed. |

So a `HEAD /stream.mp4` liveness probe is free, while a `HEAD /snapshot.jpg`
probe costs a frame every time.

---

## 5. Authentication and access control

One gate covers `/control`, `/events`, and the HTTP media endpoints
(`/stream.mp4`, `/stream.mjpeg`, `/snapshot.jpg`, `?chn=N` forms). A request
passes if **any** of these holds:

1. **Loopback** — peer is `127.0.0.0/8`. This is how an on-device WebUI always
   works without a password.
2. **A token** — the random **per-boot token** written to `http.token_file`
   (`/run/timps.token`, mode 0640), or the optional persistent secret
   `http.token` (never written to the token file). Send it as
   **`X-Timps-Token: <token>`** (preferred) or **`?token=<token>`** (the only
   form `<img>`, `<video src>` and `EventSource` can use). **A token never
   unlocks RTSP.**
3. **HTTP Basic or Digest** — `http.user`/`http.pass`, falling back to
   `rtsp.user`/`rtsp.pass`. The 401 challenge offers Digest first, then Basic.

**The token list is exactly these paths**, and nothing else:
`/stream.mp4`, `/snapshot.jpg`, `/stream.mjpeg`, `/mjpeg`, `/control`,
`/events`, `/talk` (`USE_BC_WS`) and `/webrtc/…` (`USE_WEBRTC`). **The player
page `/` and `/index.html` are deliberately not on it.** With `http.user` set,
`http://<cam>:8880/?token=…` answers **401** while
`http://<cam>:8880/stream.mp4?token=…` works — the page is reachable only from
loopback or with Basic/Digest. A WebUI that embeds the player in an
`<iframe>` therefore needs real credentials, not a token; embedding
`/stream.mp4?token=…` in a `<video>` does not.

**CORS and preflight.** The same path list gets `Access-Control-Allow-Origin`
reflection and answers `OPTIONS` with `204` **before any auth** (a preflight
carries no credentials by design). Two details worth knowing:

- **Private Network Access.** When a browser sends
  `Access-Control-Request-Private-Network: true` (Chrome's PNA, for a page
  hosted off the LAN reaching a LAN device), the preflight echoes
  `Access-Control-Allow-Private-Network: true`. It is echoed **only when
  asked**, never volunteered. It grants nothing: the real request still has to
  pass the token/Basic/Digest/loopback gate.
- The WHEP `201 Created` adds
  `Access-Control-Expose-Headers: Location`, so the browser can actually read
  the session id it has to `DELETE` later.

**RTSP accepts Basic *and* Digest.** `rtsp_check_auth()` tries
`auth_rtsp_digest()` and then `auth_http_basic()`, and the `401` offers both
challenges — so `rtsp://user:pass@<ip>/ch0` works with Basic-only clients and
embedded NVRs, not just Digest-capable ones. Digest is additionally bound to
the nonce issued on **that connection**, so a sniffed Digest header cannot be
replayed; a sniffed Basic header can, which is the reason to prefer RTSPS on
an untrusted segment. Auth is enabled by setting `rtsp.user`/`rtsp.pass`.

### The empty-credentials case (important for security questions)

While **both** `http.user` and `rtsp.user` are empty:

- The **media** endpoints and the RTSP stream are reachable by anyone on the
  network with **no authentication**. This is deliberate ("an unconfigured
  camera streams on the LAN out of the box").
- `/control` and `/events` are **not**: a non-loopback request is refused with
  `403` when no credentials are configured. So config state cannot be read or
  changed off-device without credentials or a token.

The thingino package ships `thingino`/`thingino` for both, so most fielded
cameras are not in the empty case. Advise users to change these.

CORS: media endpoints send `Access-Control-Allow-Origin: *`; `/control` and
`/events` reflect the request `Origin` (with `Vary: Origin`, allow-listing the
`X-Timps-Token` header, no credentials). `OPTIONS` preflight is answered `204`
before auth runs.

Useful shell snippet for a user on the camera or a LAN host:

```sh
T=$(ssh root@<cam> cat /run/timps.token)
curl -s -H "X-Timps-Token: $T" http://<cam>:8880/control
```

---

## 6. The `/control` API

### `GET /control`

Returns the full in-memory config plus read-only status as one JSON document
(~8 KB). Top-level keys:

`version`, `caps`, `image`, `audio`, `sensor`, `video`, `osd`, `osd0`, `osd1`,
`privacy`, `daynight`, `motion`, `encoder`, `queue_drops`, `record`,
`timelapse`, `srt`, `tls`, `last_errors`.

- `version` is the compiled-in git-describe string of the running binary. This
  is the reliable way to check "did the new firmware actually come up", which a
  flash tool's success message does not prove.
- `last_errors` holds the last WARN/ERROR per module — useful when the syslog
  ring has already recycled.

#### Read-only status leaves

Everything above is echoed config *except* these. None of them is settable;
several answer a support question no other endpoint can. (`GET /control` can
answer **`500 control json too large`** if the document overruns its 22 528-byte
cap — see the POST limits below.)

| Object | Leaves | Notes |
| --- | --- | --- |
| `encoder.<N>` | `registered`, `left_pics`, `left_stream_bytes`, `left_stream_frames`, `cur_packs`, `work_done`, `au_drops`, and `ave_bitrate` **on T31 only** | Straight from `IMP_Encoder_Query`. A channel whose query fails (stream disabled, SW-rotate path, host sim) is **omitted entirely** rather than reported as zeros. **`au_drops` = producer-side drops** (oversized AU, pool OOM in the encode thread) — the only exact count, since the log throttles those to every 20th event. Distinct from the top-level **`queue_drops[]`, which counts consumer-queue evictions.** |
| `encoder.<N>.rc` | `rc_mode`, then whichever of `bitrate`, `max_bitrate`, `qp`, `min_qp`, `max_qp`, `i_bias_lvl`, `change_pos`, `quality_lvl`, `static_time`, `frm_qp_step`, `gop_qp_step`, `adaptive_mode`, `gop_relation`, `fluc_lvl`, `ip_delta`, `pb_delta`, `max_psnr`, `rc_options`, `max_picture_size` the current mode and SDK actually carry | What the encoder **holds right now**, read back from `IMP_Encoder_GetChnAttrRcMode` — deliberately separate from the configured `videoN.*` block so written and held values can be compared. Fields the mode/API does not carry are omitted, not zeroed. On new-API SoCs `bitrate`/`max_bitrate` are raw SDK values (**unit unverified**). Frozen on a camera with no client attached (see §4). |
| `srt` | `enabled`, `port`, `channel`, `mode` (`listener`/`caller`), `connected`, `stats_age_s`, `rtt_ms`, `bw_mbps`, `rate_mbps`, `retrans`, `loss`, `drop` | **`stats_age_s = -1` means no receiver has been connected long enough for a sample** — the other numbers are then meaningless, not zero-valued facts. |
| `timelapse` | `count`, `last_t`, `free_mb`, `last_file` | |
| `record` | `recording`, `write_errors`, `last_error`, `last_error_age_s`, `motion_gate_enabled`, `manual_off`, `bytes`, `free_mb`, `file` | `last_error_age_s = -1` = no error recorded. `manual_off` is the manual stop latch (§6 commands). |
| `motion` | `active[]`, `last_ms`, `max_cells`, plus the echoed grid config | **`active[]` is row-major**: index = `row * cols + col`, length `cols * rows`. **`last_ms = -1` means "never"**, not "just now". `max_cells` is the SDK budget, the same number as `caps.motion.max_cells`. |
| `daynight` | `mode` (**int** 0 = day / 1 = night) *and* `dn_mode` (**string**), `brightness` (%), `total_gain`, `exposure`, `ae_luma`, `night_baseline`, `day_trigger`, `sun_computed_sunrise`, `sun_computed_sunset`, … | `total_gain` is the IMP `[24.8]` linear scale (**256 = 1.0×**). **`exposure` is the value the decision actually runs on** — `total_gain` scaled by the AE integration-time ratio, so it equals `total_gain` in a dark scene and drops far below it in a bright one. Quote `exposure`, not `total_gain`, when answering a day/night question; `total_gain` is kept for continuity with existing pages. `night_baseline`/`day_trigger` are `-1` outside night. `sun_computed_*` read `"--:--"` where the sun does not rise or set (polar latitudes). The object also echoes the **frozen internal constants** (`ir_ratio_night`, `ir_ratio_day`, `probe_settle_s`, `ref_delay_s`, `ir_min_headroom`, `boot_settle_s`, `transition_s`, `probe_jump_pct`) — readable, **not settable**. |
| `last_errors` | `{"<MOD>": {"level","age_s","count","msg"}}` | One entry per module that has logged, with the most recent message and how many times it repeated. Useful once the 64 KB syslog ring has recycled. |
| `osd<S>.<N>.type` | `"text"` or `"logo"` | Read back as a **word**, unlike `record.mode` and `audio.backchannel_codec`, which read back as numbers. |

### The `caps` object — capability announcement

**This is the mechanism that tells a client what this specific build on this
specific SoC supports.** Always steer a user (or a UI) to branch on `caps`,
never on a version number, because the same version string can be two
differently-compiled binaries.

| `caps` field | Meaning |
| --- | --- |
| `caps.image[]` | The `image.*` keys this SoC actually supports |
| `caps.audio[]` | The `audio.*` keys this build/SoC supports |
| `caps.osd[]` | The OSD item fields that apply **live** (`text,x,y,font_size,color,transparency,outline,outline_color`) |
| `caps.restart[]` | Sections whose keys are persist-only: `["video","sensor","osd.enabled"]` |
| `caps.video_live[]` | The `videoN.*` keys this build can push to a running encoder |
| `caps.rtsp_max_clients` / `http_max_clients` / `events_max_clients` | Concurrent-client ceilings (refusal points: RTSP, HTTP and events all answer `503`) |
| `caps.motion` | `{available, max_cells}` — `available` = build has the IMP_IVS move API |
| `caps.privacy` | `{available, max_regions}` — `available` reflects whether an OSD group actually exists in the running pipeline |
| `caps.rotation[]` | Applicable rotation values, e.g. `[0,90,180,270]`. **Key absent entirely** when `USE_ROTATE=0` |
| `caps.record` / `caps.timelapse` | `{available: 0|1}` |
| `caps.backchannel` | `{available, talk_ws}` — `talk_ws` is the *effective* mode: 0 not served, 1 served TLS-required, 2 served plaintext-ok |
| `caps.play` | `{available, sounds:[...]}` — enumerates playable files |
| `caps.webrtc` | `{available, enabled}` — same 0/1/2 meaning as `talk_ws`. **Key absent entirely** when `USE_WEBRTC=0` |

Note the deliberate convention: a key that is **absent** (`caps.rotation`,
`caps.webrtc`) means "this binary has no such endpoint/feature at all", which
`{"available":0}` could not distinguish from "compiled in but switched off".

`GET /control` also carries `tls:{available,https,rtsps,rtsps_port}` and
`srt:{available,...}` as top-level objects with the same "was this binary built
with it" semantics.

### Scoped GET sub-endpoints

| Query | Returns |
| --- | --- |
| `?fields=1` | Inventory of every POST-able config field, grouped by section. Generated from the same tables the POST walks, so it cannot drift. |
| `?stats=1` | Small object: per-stream `gop`/`profile`/`rc_mode` + `IMP_Encoder_Query` backlog. Exists so a stats card does not poll the whole document. |
| `?dn_history=1[&last=N\|&since=S][&max=N]` | Day/night decision series from the in-RAM ring sized by `daynight.history_s`. Rows are arrays `[t, gain, exposure, luma, bright%, mode]`; ≤600 rows/response, cursor paging via `next`/`head`/`oldest`/`lapped`. |

### `POST /control`

Nested JSON, sections matching the config prefixes. Every recognized key is
applied to the in-memory config, live-applied via the HAL where a path exists,
pushed to other `/events` subscribers as a `config` event, and persisted back
to `/etc/timps.conf` in one batched atomic write.

```sh
curl -X POST http://127.0.0.1:8880/control -d '{
  "image": {"brightness":140,"running_mode":1},
  "audio": {"volume":90,"gain":30},
  "video": {"0":{"bitrate":3500}},
  "motion": {"enabled":1,"sensitivity":140,"cols":5,"rows":5},
  "daynight": {"day_gain":1200}
}'
```

Non-setting **commands** that also go through POST:

- `{"record":{"active":1|0}}` — manual start/stop override
- `{"record":{"clip":"/tmp/x.mp4","seconds":6}}` — one-shot fMP4 clip (this is
  what send2/Telegram motion videos use). Constraints, all in `record_clip()`:
  the path **must start with `/tmp/`** and must contain no `..` component;
  `seconds <= 0` becomes **6** and `seconds > 30` is clamped to **30**; only
  one clip at a time (a second request is dropped with `clip busy, skipped`).
  **The POST blocks for roughly `seconds`** — it runs the capture on the HTTP
  worker thread — so a client timeout below that looks like a failure on a clip
  that was actually taken. A refused clip counts as `rejected`, so a body
  carrying nothing else answers **409**, not 200.
- `{"daynight":{"probe":1}}` — arm one silent IR probe on the next tick;
  **rejected** (not silently ignored) if the camera has no `daynight.irprobe_cmd`
- `{"speaker":{"play":"chime_1.wav"}}` / `{"speaker":{"stop":1}}` — play queue
  (`USE_PLAY`), file validated against the sounds directory

Response body (same shape whatever the status):

```json
{"ok":true,"accepted":2,"changed":1,"rejected":0,"not_persisted":0,
 "deferred":0,"deferred_keys":[],"ignored":[],
 "applied":{"image.brightness":"255"}}
```

- `accepted` — known fields applied, including no-op rewrites and clamped
  writes, plus commands carried out.
- `changed` — the subset that differed and was persisted.
- `rejected` — known keys with refused **values**, plus failed commands.
- `not_persisted` — applied live but not written, because the request changed
  more keys than the 48-slot persist list holds. Tell the user to split the
  request; those values are gone after a reboot.
- `deferred` / `deferred_keys` — `videoN.*`/`sensor.*` keys persisted but not
  applied to the running pipeline this request.
- `ignored` — key names this build did not apply (typo, wrong section, gated
  out, or no write path). Fully prefixed, e.g. `"video1.quality_level"`.
- `applied` — echo of the **effective** value after clamping.
- `truncated` / `deferred_truncated` / `ignored_truncated` — present and `true`
  when the matching list overran its buffer (`applied` 512 B, `deferred_keys`
  1024 B, `ignored` 512 B). **The counts stay exact; only the lists are
  short.** When one of these flags appears, do not reason from the list —
  re-read the state with `GET /control`.

**Request size limits.** The request line, headers **and** body must fit one
**4096-byte** connection buffer, so the usable JSON is 4096 minus whatever the
headers consumed — roughly **3.5 KB** in practice. A larger declared
`Content-Length` is refused with **`413 Payload Too Large` / `body too large`**
rather than silently truncated and half-applied. A full OSD or privacy batch
can reach this; split it. A request using **`Transfer-Encoding`** instead of
`Content-Length` gets **`411 Length Required`** — this catches some proxies and
Python `requests` called with a generator body. In the other direction,
`GET /control` answers **`500` / `control json too large`** if the response
overruns its 22 528-byte cap.

Status codes and their `reason` discriminators:

| Status | `reason` | Meaning | Advice to the user |
| --- | --- | --- | --- |
| 200 | — | At least one known field applied (partial success is still 200) | Check `rejected` and `ignored` |
| 400 | `not_json` | Body was not a JSON object | Client bug |
| 422 | `unknown_fields` | Parsed, but **no key this build knows** | Check spelling **and** whether the feature is compiled in (`caps`) — retrying identically will never work |
| 409 | `values_rejected` | Keys were all known, all values refused | Key names were right, fix the values |
| 411 | — | `Transfer-Encoding` instead of `Content-Length`, or a missing/zero length | Send a fixed-length body |
| 413 | — | Headers + body exceed the 4096-byte buffer | Split the request |
| 503 | `oom` | Allocation failure | Retry; not a client error |

### `GET /events` (SSE)

```sh
curl -N "http://<cam>:8880/events?stream=motion,daynight,stats&token=$T"
```

Event types: `motion`, `daynight`, `stats`, `config`. `?stream=` selects a
subset (default: all four). `motion` and `daynight` emit their full current
state once on connect. The stream opens with the preamble
`retry: 3000` followed by the comment `: connected`, and a `: ping` comment
arrives whenever nothing else has gone out for ~12 s.
`events.enabled = 0` → the endpoint answers `404`; above `events.max_clients`
→ `503 busy`. Browsers must use `?token=` because `EventSource` cannot set
headers.

**`stats` is change-gated, not periodic.** `events.stats_ms` is the *sampling*
period; an event is emitted only when `stats_changed()` says something moved:
`clients`, the channel count, or per channel `chn`, `subs`, `width`, `height`,
`codec`, `drop_frames`, `drop_bytes` (all **exact** comparisons), `fps` by
**≥ 0.1**, or `kbps` by **≥ 5 %** (floor 8 kbps when the previous value was 0).
**On an idle camera you therefore get one `stats` on connect and then nothing
but `: ping`** — that is not a stalled stream, and a client must not treat the
gap as a liveness failure. Payload:

```json
{"uptime_s":1234,"clients":2,
 "video":[{"chn":0,"subs":1,"fps":25.0,"kbps":2980,"width":1920,"height":1080,
           "codec":"h264","drop_frames":0,"drop_bytes":0}]}
```

**`config` reports another client's `/control` writes**, one event per key:

```json
{"key":"image.brightness","value":"140"}
```

`value` is **always a JSON string**, even for numbers and booleans — parse
accordingly. The key/value table holds **24 slots**; if a bulk save evicted a
key this client had not seen yet, it first receives
`{"resync":true}`, which means *the incremental stream lapsed — re-read
`GET /control`* rather than assuming the missed key is unchanged.

---

## 7. Rate control and bitrate (a frequent confusion)

`videoN.bitrate` means **different things per SoC generation**, deliberately:

- **Classic (T10–T30, incl. T23):** written into `maxBitRate`/`outBitRate` —
  under vbr/smart it is a **hard ceiling** the controller works below.
- **New API (T31/C100/T40/T41):** passed as `uTargetBitRate` — a **target**;
  the real cap `uMaxBitRate` stays at the SDK default.

So a T23-calibrated "ceiling" intuition does not transfer to a T31.

Measured on a T23: the classic controller is **quality-seeking** — it encodes
the best quality `min_qp` allows and the bitrate follows. `min_qp` is the
operative lever (sweeping 20/30/38 under vbr gave 1743/1181/235 kbit/s);
`quality_lvl` moves the operating point gently (2→7 ≈ −27 %); `change_pos`
measurably did nothing. Treat per-SoC behavior as measured-on-T23, not
guaranteed everywhere.

Classic-SoC-only keys (warn once if set on T31+): `quality_lvl` (0–7; the SDK
header suggests a hard lower bound `minBitRate = bitrate × quality[lvl]`, but
the project's own T23 measurements never found that floor binding at any
setting measured — what the key does do is shift the whole operating point
uniformly, 2→7 ≈ −27 %), `change_pos` (50–100), `i_bias_lvl` (−3…3, also CBR),
`fluc_lvl` (H.265 only).

Live-applicable rate-control keys by SoC (the rest need a restart):

| SoC | Live keys |
| --- | --- |
| T10–T30 (incl. T23) | `rc_mode`, `bitrate`, `qp`, `min_qp`, `max_qp`, `quality_lvl`, `change_pos`, `i_bias_lvl` (H.264 streams only) |
| T31 / C100 | `bitrate`, `min_qp`, `max_qp`, `i_bias_lvl` |
| T40 | `bitrate`, `min_qp`, `max_qp` |
| T41 | `bitrate`, `min_qp`, `max_qp` |

`qp` (fixqp) is **not** live on any new-API SoC (T31/C100/T40/T41) — deliberately:
measured on a T31X (2026-08-22) the boot path spans 6.4× across qp 25 vs 42
while the live path does not move the bitstream at all. Setting `qp` there
needs a restart.

Verify against `caps.video_live`, and read back `encoder.<n>.rc` in
`GET /control` — that object is what the encoder actually holds, as opposed to
what was configured. **It only refreshes while the channel is encoding**, so
attach a client first.

---

## 8. WebRTC / WHEP specifics

Answer template for "how do I get WebRTC on my camera":

1. **Build:** `BR2_PACKAGE_TIMPS_WEBRTC` (defaults **y** in thingino). It
   requires `BR2_PACKAGE_TIMPS_TLS` and `BR2_PACKAGE_TIMPS_CONTROL`, and
   selects `BR2_PACKAGE_MBEDTLS_DTLS_SRTP`. Turning it on in an existing
   output directory needs an mbedTLS dirclean/rebuild, because it changes the
   mbedTLS build.
2. **Runtime:** `webrtc.enabled` — `0` off, `1` on but the signalling POST must
   arrive over https, `2` on and a plaintext POST is accepted. **Default is 2**
   on `USE_WEBRTC` builds.
3. **Check from outside:** `GET /control` → `caps.webrtc`. Absent = not built.
   `{"available":1,"enabled":2}` = ready.
4. **Audio:** WHEP carries G.711 only. On the usual `audio.codec = aac` camera,
   `audio.codec2 = pcmu` (the `USE_WEBRTC` default) runs a second independent
   G.711u encode so WHEP gets audio while RTSP/fMP4/recording keep AAC. Without
   it, the answer contains `m=audio 0` — a video-only session.
5. **Certificate:** the DTLS identity is the `http.tls_cert` / `http.tls_key`
   pair. `S95timps` makes sure a usable pair exists before `timpsd` starts.

Known limits to state plainly:

- **LAN or VPN only** — one host candidate, no STUN/TURN, no NAT traversal,
  no IPv6, no trickle ICE.
- No Opus, no AAC, no H.265 over WebRTC, and no transcoding.
- No NACK/retransmission, no FEC, no congestion control (only PLI/FIR).
- **Firefox will not play it at the default `videoN.profile = 2` (High).**
  Firefox offers Baseline only and re-checks the answer's `profile-level-id`,
  so it refuses the answer outright at `setRemoteDescription()` ("Answer had no
  codecs in common with offer"). Fix: set `videoN.profile = 0` on the
  `webrtc.channel` stream. Chrome/Chromium/Edge accept any profile (they
  initialize the decoder from the in-band SPS) and are the tested target.
  Safari is untested.
- **Max 4 concurrent sessions**; a 5th offer gets `503`. An incomplete session
  is reclaimed after 30 s.
- **A fixed `webrtc.port` is the bottom of a range**, not one port: each
  session binds its own UDP socket from `webrtc.port` up to `webrtc.port_max`
  (default `webrtc.port + 3`). `webrtc.port = 0` = one ephemeral port per
  session.
- Access rules are `/control`'s (localhost / token / Basic-Digest).
- Status codes the endpoint gives: `404` = not built, `503` =
  `webrtc.enabled=0` or all slots taken, `426` = plaintext POST on a
  TLS-capable port with `webrtc.enabled=1`, `405` = wrong method.

---

## 9. TLS / HTTPS specifics

`http.https` is a **tri-state**, and the meaning of `1` changed in v1.9.11 —
this is a common source of confusion:

- `0` — plaintext only (default).
- `1` — **both schemes on the same port**. Each connection is classified by its
  first byte (`0x16` = TLS handshake record; an HTTP method starts with a
  letter), so `http://<ip>:8880/` and `https://<ip>:8880/` both work. This
  exists because the preview stream is fetched by a WebUI page whose scheme is
  decided by a *different* server (uhttpd on :80/:443) — page on http + stream
  on https fails (a self-signed cert cannot be click-through-trusted for a
  subresource fetch; Safari just says "Load failed"), and the reverse is blocked
  as mixed content.
- `2` — TLS only; a plaintext request gets `426 Upgrade Required`. This is what
  `1` used to mean.

Either on-value **fails closed**: if the cert/key cannot be loaded, the listener
is not bound at all rather than silently downgraded to plaintext. `rtsp.tls` is
a plain boolean (RTSPS on `rtsp.tls_port`, default 322).

`S95timps` handles certificates before `timpsd` starts:
1. an operator-supplied pair at `http.tls_cert`/`http.tls_key` is never
   second-guessed;
2. otherwise, on an image with the WebUI, it **symlinks** them to uhttpd's own
   `/etc/ssl/certs/uhttpd.crt` + `/etc/ssl/private/uhttpd.key` so `:443` and
   `:8880` present the *identical* certificate (browsers track self-signed trust
   per host **and port**);
3. otherwise it generates a self-signed pair.

On an image without the WebUI, the user must visit `https://<ip>:8880/` once to
accept the certificate.

TLS session resumption (RFC 5077 tickets) is always on; the ticket key rotates
every 12 h.

**"HTTPS does nothing / my TLS keys are ignored"** → check `tls.available` in
`GET /control`. `{"available":0}` means this binary has no mbedTLS
(`USE_TLS=0`) and every TLS key is inert however it is set. The only other
symptom is a log line `RTSPS requested but built without USE_TLS`.

---

## 10. Day/night — the concepts a support answer needs

timps replaces thingino's separate `daynightd` (the package disables
`S97daynightd` when `USE_DAYNIGHT` is on).

**What timps does and does not drive.** A thread decides day vs night; on a
change it runs `<daynight.switch_cmd> day|night` (default script name
`daynight`) via `fork()+execlp()` — never `system()`. **The board script** flips
the physical IR-cut filter and IR LEDs, then calls back with
`POST /control {"image":{"running_mode":0|1}}`. So a camera that switches IR
hardware but stays colour-tinted usually has a board-script problem, not a
timps one — and vice versa.

**The metric is the exposure index, not raw gain:**

```
D = total_gain × (integration_time / max_integration_time)      higher = darker
```

`total_gain` alone floors at **256 (= 1.0×)**; once the AE rails there, further
brightening is invisible. The AE keeps shortening exposure instead, so the
product carries signal across the whole range. In a dark scene the integration
time is railed and `D == total_gain`, which is why the thresholds keep their
historic calibration.

**The two thresholds** — both evaluated on the **day pipeline** only:

- `daynight.day_gain` (default **768** = 3×) — index below this = day.
  "Day is confirmed when the day pipeline can hold the scene at ≤ 3× gain."
- `daynight.night_gain` (default **4096** = 16×) — index above this (while in
  day) = night. "Colour is hopeless."

Old names `total_gain_day_threshold` / `total_gain_night_threshold` still work.

**The asymmetry** (explain this when a user asks why it "keeps clicking" or
"won't go back to colour"): in day (IR-cut closed, illuminator off) the reading
is an honest measure of ambient light, so day→night is a plain measurement. In
night the camera is partly measuring **its own IR**, so night→day can only be
decided by a **probe** — switching to the day pipeline, letting the AE settle,
and judging once. A probe costs an audible IR-cut click, and
`daynight.probe_min_gap_s` (600 s) is what rations it.

**The silent probe:** where the board can toggle its IR illuminator
independently, `daynight.irprobe_cmd` (default `timps-irprobe`, called as
`<cmd> on|off`) lets timps ask the cheap question first — illuminator off for a
few seconds, compare. The ratio is dimensionless so one pair of thresholds fits
every camera. Clearing `irprobe_cmd` disables both the silent probe and the
trend path, leaving only the jump trigger and the heartbeat (i.e. more audible
clicks).

**The heartbeat** is the only bound on how long a wrong "night" can last:
`heartbeat_s` = 14400 (4 h) while the scene is moving, `heartbeat_max_s` =
43200 (12 h) once it demonstrably is not.

**Tuning guidance for a user stuck in night mode during the day:** the
historical default 300 (1.17×) asserted "day only when the day pipeline needs
essentially no gain" — outdoor daylight. Indoors a normally lit room needs 2–3×,
so the comparison could never come true. The default was raised to 768 (3×),
which clears marginal indoor cases; genuinely dim rooms (one measured camera's
best day-pipeline reading was 9.79×) still need an explicit per-camera
`daynight.day_gain` raise. Steps to give a user:

1. `curl .../control` and read the `daynight` object: `total_gain`, `exposure`,
   `night_baseline`, `day_trigger`, `day_gain`, `night_gain`, `isp_desync`.
2. Set `daynight.diagnose_thresholds = 1` — after three consecutive failed
   probes it warns once per daemon session that no probe has ever confirmed
   day, and names the value to raise.
3. Set `daynight.history_s = 14400` (or use the tuning page's "collect in
   background" switch) and read `GET /control?dn_history=1&last=600`, or use
   the WebUI's `tool-sensor-data` page, to see where the index actually sits.
4. Raise `daynight.day_gain` toward (but well below) `night_gain` until the day
   reading clears it.
5. `general.debug_modules = daynight` (live over `/control`, no restart) gives
   per-probe lines like `probe: r=1.05 lit=3350 dark=3520 hr=199 verdict=day`.

`daynight.mode = schedule` hands the decision to the calendar outright (no
sensor, no probes) — a reasonable fallback for a camera whose optics defeat the
measurement. `daynight.enabled = 0` is full manual (`image.running_mode` or the
WebUI toggle).

`isp_desync = 1` in the status means the decided mode and the ISP readback have
disagreed persistently — usually a board-script/`running_mode` loop that is not
closing.

---

## 11. Troubleshooting playbook

### First three questions for any timps report

1. **Which streamer?** `ls -l /usr/bin/timpsd`; if absent, this is prudynt/raptor.
2. **Which build?** `curl -s http://<cam>:8880/control` (from the camera:
   `curl -s http://127.0.0.1:8880/control`) and read `version`, `caps`,
   `tls.available`, `srt.available`.
3. **What does the log say?** `logread | grep -i timps` (works because
   `general.syslog = 1`; the init script backgrounds `timpsd`, so its stderr is
   discarded). Also read `last_errors` in `GET /control` — the 64 KB syslog ring
   recycles within hours.

### "Feature X isn't working"

Distinguish **compiled out** from **misconfigured**:

| Symptom | Likely cause | Check |
| --- | --- | --- |
| `POST /control` returns `422 unknown_fields` | The key does not exist in this build | `GET /control?fields=1`, and the relevant `caps.*` |
| Key accepted (`200`) but nothing happens | Restart-required key, or live path unavailable | `caps.restart`, `caps.video_live`, the POST reply's `deferred_keys` |
| `caps.webrtc` / `caps.rotation` missing entirely | `USE_WEBRTC` / `USE_ROTATE` = 0 | Rebuild with the Kconfig option |
| `record.available = 0` | `USE_RECORD` = 0 | Rebuild with `BR2_PACKAGE_TIMPS_RECORD` |
| `tls.available = 0` but `http.https = 1` in the config | Binary has no mbedTLS | Rebuild with `BR2_PACKAGE_TIMPS_TLS` |
| `/talk` answers 426 | `audio.talk_ws = 1` on a plaintext port | Either enable `http.https`, or set `audio.talk_ws = 2` (understanding that mic audio and the token then cross the network in clear) |
| `/webrtc/whep` answers 404 | Not built | See §8 |
| `/webrtc/whep` answers 503 | `webrtc.enabled = 0`, or all 4 session slots busy | — |
| `/events` answers 404 | `events.enabled = 0` or `USE_CONTROL=0` | — |
| Setting is accepted then reverts after reboot | `not_persisted > 0` in the POST reply (more than 48 changed keys), or a file-only key | Split the request; hand-edit `/etc/timps.conf` for file-only keys |

### Streaming problems

- **No video at all, but the daemon runs** — remember on-demand encoding: the
  encoder starts when a client attaches. Check `videoN.enabled`, then
  `encoder.<n>` in `GET /control` (a failing/absent channel is omitted from
  that object rather than reported as zeros).
- **A browser preview stalls/freezes for one viewer** — `http.adaptive_drop`
  (default 1) is working as designed: that client alone freezes on its last
  frame and resumes at the next keyframe, instead of being fed a headless GOP.
- **`queue_drops` climbing** — some consumer is behind and the shared encoder is
  being asked for extra IDRs on its behalf, which spikes bitrate for everyone.
  **since v1.9.19 (unreleased):** those recovery IDRs are rate-limited per
  *stream* (1 s) rather than per consumer, a coalesced one is cancelled by the
  keyframe that satisfies it (so one drop burst costs one IDR, not two), and
  the log names the culprit once a minute —
  `chn=0 rec: 12 queue overflows in the last 60s (consumer too slow, IDR
  re-requested)`, with the kind being `rec`/`rtsp`/`mp4`/`webrtc`/`srt`. On
  v1.9.18 the only trace was the counter itself. No config key either way.
- **A recording has a gap, then a second of smeared picture** — up to v1.9.18
  the recorder muxed the headless GOP after a queue overflow.
  **since v1.9.19 (unreleased)** it freezes and resumes at the next keyframe,
  so the gap is slightly longer and clean.
- **"The camera won't do more than 30 fps"** — **since v1.9.19 (unreleased)** an
  *autodetected* `sensor.fps` is capped at 30; set `sensor.fps` explicitly to
  go higher, and read the `sensor fps: requested N, driver holds n/d` line to
  see what the driver actually took. `videoN.fps` is the stream rate only.
- **RTSP over a VPN drops or fragments** — lower `rtsp.mtu` (default 1200 is
  already the VPN-safe value; 1400 is the LAN-only optimization).
- **RTSP refuses a client with `503 Service Unavailable`** —
  `caps.rtsp_max_clients` reached (a compile-time bound, `-D`-overridable per
  board; low-RAM boards often build with 4). `src/rtsp/rtsp.c` sends no other
  refusal code for this; `453` does not appear in it at all.
- **MJPEG/snapshot at a stream's resolution 404s** — needs `videoN.jpeg = true`
  and a unique `videoN.jpeg_chn`.

### Audio problems

- **No audio in the browser preview** — `audio.codec` must be `aac` for fMP4;
  AAC needs `USE_FAAC`. G.711 does not play in the MSE preview.
- **No audio in a WHEP session** — see §8 item 4 (`audio.codec2 = pcmu`).
- **`high_pass`/`agc`/`ns`/`aec` POST succeeds but nothing changes** — these are
  **not live** despite being POST-able; they take effect at the next AI bring-up
  (restart) or next AO open. libimp would race its own record thread otherwise.
- **Mic sounds noisy after using talk-back** — known: `IMP_AI_EnableAec` brings
  up the vendor's full WebRTC audio-processing chain on the mic (configured by
  `/etc/webrtc_profile.ini`, not by timps's settings) and has been observed to
  leave the mic audibly noisier until `timpsd` is restarted.
- **Speaker silent** — `audio.spk_enabled` is the master gate; on a build
  without `USE_PLAY`/`USE_BACKCHANNEL` there is no AO pipeline at all. Note the
  camera must physically have a speaker wired.
- **`getUserMedia()` refused on the talk page** — the browser requires a secure
  context. Either serve the page over https, or use the browser's own
  insecure-origin override; `audio.talk_ws = 2` only tells *timps* to accept
  plain `ws://`, it cannot lift the browser's rule.

### Motion problems

- `motion.enabled = 0` is the default — it must be turned on.
- `cols × rows` is clamped to the SDK ROI budget (`caps.motion.max_cells`,
  usually 52).
- `motion.on_motion` is **config-file only** (run without a shell, no
  arguments); `motion.cooldown_ms` likewise. They are deliberately not
  POST-able.
- `motion.stalled = 1` in the status means IVS stopped delivering results and a
  recovery cycle ran.
- For snappier detection lower `motion.skip_frames` (2–3); for cheaper, raise it.
- The grid overlay missing single-frame motion → raise `motion.hold_ms`.

### Recording problems

- `{"record":{"active":1}}` returning `200` is **not** a promise that recording
  started. If `record.min_free_mb` cannot be reached even by deleting every
  existing recording, the recorder refuses to open a segment: `recording` stays
  `0`, `write_errors` increments, and `last_error` reads
  `min_free_mb=<N> unreachable (max <M>MB)`. That is the deliberate behaviour —
  better than deleting everything and still missing the target.
- `mode = motion` also requires `motion.enabled = 1`; the status distinguishes
  this via `motion_gate_enabled`.
- `manual_off` in the status is a manual-stop latch that overrides the config.

### Rotation problems

- `USE_ROTATE` is **off by default**. Without it, any non-zero
  `videoN.rotation` is coerced to 0 with a warning.
- `rotation = 180` is a real per-channel capability **only on T40/T41**.
  Everywhere else use `image.hflip = 1` + `image.vflip = 1`.
- `90`/`270` do not exist on T10/T20/T21/T30/C100 at all.
- T23 90/270 needs `USE_SW_ROTATE`: CPU transpose + software H.264 encode,
  H.264 only, software text OSD only (no logo/privacy), substream-class
  resolution, and JPEG needs the rotated width (= source height) to be a
  multiple of 32 — e.g. 1280×704 → 704×1280, not 1280×720.
- Direction is not uniform: on T23/T40/T41, 90 = clockwise; on T31, 90 =
  counter-clockwise (kept numerically aligned with prudynt-t/raptor).
- On T31, hardware OSD/privacy currently do not work on 90/270 when the stream
  is downscaled from the sensor.
- Rotation is restart-required; the advertised dimensions swap
  (`eff_width`/`eff_height` in `GET /control`).

### Diagnostics to ask a user for

```sh
# from a LAN host, with the token
T=$(ssh root@<cam> cat /run/timps.token)
curl -s -H "X-Timps-Token: $T" http://<cam>:8880/control          # full state
curl -s -H "X-Timps-Token: $T" 'http://<cam>:8880/control?fields=1'
# on the camera
logread | tail -n 200
/etc/init.d/S95timps restart
```

Live per-module debug without a restart (and without drowning the syslog ring):

```sh
curl -s -X POST -H "X-Timps-Token: $T" http://<cam>:8880/control \
     -d '{"general":{"debug_modules":"daynight"}}'
```

There is also a QA harness in the repo:
`scripts/timps-qa.sh --cam <ip> [--profile quick|standard|load|soak|drift]`.

---

## 12. Accuracy notes for the assistant

- **`webrtc.enabled` default is `2`** on `USE_WEBRTC` builds (set in
  `src/config.c`). Some older help text in the Buildroot `Config.in` still says
  `1` or `0` — the source is authoritative.
- **`jpeg.enabled` compiled default is `0`**, although `timps.conf.example`
  shows it as `1`. Per-stream piggyback JPEG (`videoN.jpeg`) defaults to on.
- **`osd.hinting`**: the runtime default is `1`. The Buildroot `Config.in` help
  text still says "default 0 (off) either way" — that text is stale. Either way
  it does nothing unless `BR2_PACKAGE_TIMPS_OSD_HINTING` compiled the pass in.
  It and `osd.supersample` are POST-able but restart-required.
- **The token header is `X-Timps-Token`** (or `?token=` in a URL). No other
  spelling works — `X-Auth-Token` in particular is not recognized.
- **The speaker is native `IMP_AO`** — timps owns the audio-output device and
  needs no `/bin/iac` (ingenic-audiodaemon). The `src/config.h` comment on
  `audio.backchannel_rate` still calls it the rate "fed to iac"; that wording
  is stale.
- When unsure whether a feature exists on a user's camera, say so and ask for
  `GET /control` output rather than guessing. The `caps` object is designed
  exactly for that question.
- **Out of scope for this document (do not discuss):** any dual-sensor
  support. It exists only as an unshipped, hardware-unverified prototype on a
  development branch and is not part of what a user can run or be supported on.
