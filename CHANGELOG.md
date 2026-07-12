# Changelog

All notable changes to timps are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/), and the project aims to follow
semantic versioning.

## [Unreleased]

### Changed
- **Default `http.port` moved 8080 → 8880.** Port 8080 clashed with the
  ONVIF daemon (`onvif_srvd`), which also listens there; whichever bound
  first won, so ONVIF could fail to start when timps grabbed 8080. timps now
  defaults to `8880`, leaving 8080 to ONVIF. The port is still configurable
  via `http.port`; the WebUI reads the live port from `/x/timps-token.cgi`,
  so browser pages follow automatically.

### Added
- **Optional HTTPS + SRT (compile-time gated).** New `USE_TLS` (mbedTLS) and
  `USE_SRT` (libsrt) build flags, auto-enabled by the buildroot package
  selection (`BR2_PACKAGE_MBEDTLS` / `BR2_PACKAGE_LIBSRT`) - if the lib isn't in
  the image nothing changes. `USE_TLS`: a small mbedTLS wrapper (`src/tls.c`)
  behind which the HTTP server can run **HTTPS** (`http.https` + `http.tls_cert`
  / `http.tls_key`); the httpd I/O now goes through a TLS-aware send/recv layer
  that is byte-for-byte the old plain path when `USE_TLS` is off. `USE_SRT`:
  MPEG-TS over SRT output in listener mode (`src/srt.c`, `srt.enabled`/`port`/
  `channel`/`latency_ms`/`streamid`/`passphrase`) served from the hub like the
  recorder. Config keys for RTSPS (`rtsp.tls`/`rtsp.tls_port`) are parsed and
  reserved. NOTE: the TLS and SRT code paths cannot be built in the x86 sim
  (no mbedTLS/libsrt) - the default build stays verified; the TLS/SRT paths and
  the hand-rolled TS muxer need on-device verification.
- **Local recording to SD** (`record` section + `/control` action): records one
  video stream (+AAC audio) to `<dir>/<hostname>/records/<strftime>.mp4` as
  fragmented MP4, reusing the `/stream.mp4` muxer (`src/record.c`). Modes:
  `continuous` or `motion` (pre-roll ring from the keyframe before the trigger +
  `post_roll_s` after the last motion). Segments rotate every `record.segment_s`
  at a keyframe; oldest files are pruned to keep `record.min_free_mb` free.
  `GET /control` reports a `record` status object (recording/channel/mode/bytes/
  free_mb/file) and `caps.record`; `{"record":{"active":1|0}}` is a manual
  start/stop override (the WebUI record button). thingino path defaults
  (`/mnt/mmcblk0p1`, `<host>/records/` tree). Verified end-to-end in the x86 sim
  (valid MP4 segments via ffprobe).
- **Privacy cover masks** (`privacy` section, `/control` + config): solid filled
  rectangles per video stream (`privacy<S>.<N>.{enabled,x,y,w,h,color}`, up to
  `MS_MAX_PRIVACY` per stream) that black out sensitive areas, implemented as IMP
  OSD cover regions in the per-stream OSD group. Applied LIVE (create/show/hide/
  move without a restart, as long as OSD or a privacy region was on at startup)
  and persisted. `GET /control` dumps the `privacy` tree and advertises
  `caps.privacy = {available, max_regions}`. Replaces the prudynt-era WebUI
  privacy page's dependency on the `json-prudynt.cgi` bridge. NOTE: the IMP cover
  region call in `imp_osd.c` uses the common SDK form and needs on-device
  verification against the exact `<imp/imp_osd.h>` coverData layout.
- **Token now also unlocks HTTP media viewing** (`USE_CONTROL` builds): the
  `/control` token (per-boot `http.token_file` + optional persistent
  `http.token`, same constant-time check) is accepted on `/stream.mp4`,
  `/stream.mjpeg` and `/snapshot.jpg` (incl. `?chn=N`) as `?token=` — the
  only form an `<img>`/`<video src>` can use — or `X-Timps-Token`. This lets
  the thingino WebUI previews load the streams DIRECTLY from the HTTP port
  (no on-device proxy CGIs) even with `http.user` set. Media access is now
  localhost ∨ token ∨ Basic ∨ open-when-no-user — the existing rules are
  unchanged, the token is a pure addition; it still never unlocks RTSP, and
  non-media paths (`/` player, bogus paths) are NOT unlocked by a token.
  The media endpoints also answer the CORS `OPTIONS` preflight now, and
  `/stream.mjpeg` + `/snapshot.jpg` responses carry
  `Access-Control-Allow-Origin: *` like `/stream.mp4` always did, so
  cross-origin `fetch()`es of all three work. Caveat as with `/events`: a
  query token can end up in access logs — accepted on a LAN.
- **`GET /events` SSE push stream** (`USE_CONTROL` builds): a long-lived
  `text/event-stream` that PUSHES JSON state instead of being polled —
  `event: motion` (the `/control` motion object, emitted when the active
  grid/enabled/geometry/sensitivity changed), `event: daynight` (the
  `/control` daynight object, on a mode flip or ≥1 % brightness / ≥5 % gain
  move) and a periodic `event: stats`
  (`{"uptime_s","clients","video":[{"chn","subs","fps"},…]}`, every
  `events.stats_ms`). `?stream=motion,daynight,stats` filters the types
  (default all). Same auth as `/control` (localhost / token / Basic, CORS +
  OPTIONS preflight); the token is also accepted as `?token=` because
  EventSource cannot send headers. On connect: `retry: 3000`, a
  `: connected` comment and the full current state once; afterwards
  per-connection dedup (last-sent snapshot per event type) plus a `: ping`
  keepalive (~12 s) that doubles as dead-client detection. New tiny notify
  hub `src/events.c/.h` (generation counter + `CLOCK_MONOTONIC` condvar):
  `events_notify()` is called from the IVS result thread (grid changed,
  start/stop), the day/night sampler (real changes only) and `/control`
  writes to `motion.*`/`daynight.*`/`image.running_mode`; it is a no-op stub
  without `USE_CONTROL`, so every build permutation still links. Config:
  `events.enabled` (default 1), `events.stats_ms` (default 2000, 0 = off),
  `events.max_clients` (default 8; beyond → `503`, so an /events flood
  cannot exhaust the HTTP connection threads). The status-object JSON is
  built by shared helpers (`control_motion_json`/`control_daynight_json`),
  so `/control` and `/events` emit identical shapes by construction. The
  thingino WebUI preview overlay now subscribes to `?stream=motion` (with a
  4 Hz `/control` polling fallback) instead of polling.
- **Grid motion detection (IMP_IVS)**: the single detection ROI became a
  configurable `motion.cols` × `motion.rows` GRID of IMP_IVS move-ROIs laid
  evenly over the `motion.monitor_stream` frame (integer pixel split, the last
  row/column absorbs rounding; cell index row-major = `row*cols+col`).
  `cols*rows` is clamped to the SDK's compile-time `IMP_IVS_MOVE_MAX_ROI_CNT`,
  taken from the `imp_ivs_move.h` being built against via the new
  `motion_caps.h` (`MOTION_AVAILABLE`/`MOTION_MAX_CELLS`): 52 on most SDKs,
  **4** on the old T10/T20 3.9.0 SDK (grid defaults 5×5, 2×2 on 4-cell SDKs).
  The UI sensitivity 0..255 maps to IMP's 0..4 normal-camera range (one global
  value for all cells for now). SDKs without the move API compile a no-op stub
  and report the feature unavailable. The IVS group is now explicitly bound to
  the monitor stream's FrameSource (FS→IVS, unbound on stop) and the move
  interface is released via `IMP_IVS_DestroyMoveInterface` (both were missing).
- **Live motion control + status**: `motion.enabled`/`sensitivity`/`cols`/
  `rows`/`monitor_stream` are settable via `/control` and applied LIVE — the
  HAL cleanly stops and recreates the IVS channel (move params are create-time
  attributes). `cooldown_ms`/`on_motion` stay config-file only (`on_motion`
  runs through `system()`). `GET /control` gained `caps.motion`
  (`available`, `max_cells`) and a read-only `motion` status object:
  `{"available","enabled","cols","rows","max_cells","sensitivity",
  "monitor_stream","active":[0/1,... row-major, length cols*rows],
  "last_ms"}` (`last_ms` = ms since the last motion event, -1 = never). The
  thingino WebUI polls it directly on `:8880` with the `/control` token to
  draw a live grid overlay on the preview.

- **Token auth for `/control`**: the endpoint now allows any one of localhost
  (unchanged), a valid token, or HTTP Basic (unchanged). Tokens travel as an
  `X-Timps-Token:` header (preferred) or `?token=` query parameter, are
  compared in constant time and only unlock `/control` — never the streams.
  A random 128-bit per-boot token is generated from `/dev/urandom` and
  published to `http.token_file` (default `/run/timps.token`, mode 0640,
  `""` disables) so a local privileged helper (the thingino WebUI) can hand it
  to its authenticated browser session; an optional persistent `http.token`
  secret is also accepted for remote automation and is never written to the
  token file.
- **CORS on `/control`**: `OPTIONS` preflight (204, answered before auth — a
  preflight carries no credentials) and reflection of the request's `Origin`
  (+ `Vary: Origin`, `Access-Control-Allow-Headers: X-Timps-Token,
  Content-Type`, methods, max-age) on `/control` responses, so a browser page
  on another port (WebUI on `:80`) can call `:8880/control` directly with the
  token. `Access-Control-Allow-Credentials` is deliberately never sent.

### Fixed
- Target builds now pass `-I$(IMP_INC)/imp` too: the T10/T20 3.12.0 IVS
  headers include `<imp_ivs.h>` without the `imp/` prefix and did not resolve
  with `-I$(IMP_INC)` alone.

## [1.2.0] - 2026-07-11

### Added
- **Full ISP image control via `/control`**: the `image` section now covers the
  complete tuning set — brightness, contrast, saturation, sharpness, hue, h/v
  flip, running_mode, anti-flicker, AE compensation, max analog/digital gain,
  sinter & temper (noise), DPC, defog, DRC (WDR), highlight-depress (tone),
  backlight compensation and white balance (mode + R/B gain) — applied live via
  the matching `IMP_ISP_Tuning_*` call. A compile-time per-SoC capability matrix
  (`isp_caps.h`, T10–T41 + C100) is reported as `caps.image` so a UI can grey
  out what a given SoC cannot do; unsupported keys still persist.
- **Full audio control via `/control`**: live mic volume, gain, ALC gain,
  high-pass filter, AGC (+ target level / compression), noise-suppression, and a
  **live mic mute** (`audio.mute` — captured frames are dropped before the
  encoder/hub, no restart). Capability matrix in `audio_caps.h` → `caps.audio`.
  Codec / sample-rate / bitrate / channels persist and apply on restart.
  Speaker & forced-stereo have no IMP-AO path and are reported unsupported.
- **Full encoder & sensor control** (persist + restart): `video.N` accepts the
  whole per-stream key set (codec, width, height, fps, bitrate, rc_mode, gop,
  max_gop, profile, qp, min/max_qp, rotation, buffers, enabled, rtsp_path) and a
  new `sensor` section (model, i2c_addr, fps, width, height). These never touch
  the running pipeline; `GET /control` flags them in `caps.restart` and dumps
  the current values so a UI can populate.
- **Per-stream OSD**: every video stream has its own independent overlay set
  (`osd.items[stream][item]`). Canonical keys `osd<S>.<N>.<field>` (e.g.
  `osd0.0.text`, `osd1.2.x`); legacy `osd<N>.<field>` keys still load and mirror
  onto every stream. `/control` accepts `"osd0"/"osd1"` objects (live via
  `imp_osd_apply(stream,item)`) and still the shared legacy `"osd"` object.
- **OSD text outline/stroke**: new per-item `outline` (width px, 0 = off,
  default) and `outline_color` (`0xAARRGGBB`, default black). The TTF and
  embedded-bitmap rasterizers dilate the glyph coverage and blend the stroke
  under the fill; the region grows by the outline width. `caps.osd` lists the
  new leaves.
- **Day/night measurement exposed** (`daynight_get_status()`): the detection
  thread derives the **total gain** from the isp-m0 gain fields (IMP log2 units)
  converted to the `GetTotalGain` [24.8] linear scale (256 = 1×, matching what
  prudynt/raptor report), keeps sampling in manual mode, and `GET /control`
  reports `daynight: {enabled, mode, brightness%, total_gain}` (−1 = unknown; a
  stub answers unknowns without `USE_DAYNIGHT`).
- **System log output**: timps now also logs to syslog (tag `timpsd`) so
  messages appear in `logread` (the init script backgrounds timpsd, so its
  stderr is otherwise discarded). On by default; disable with
  `general.syslog = false`.

### Changed / Fixed
- **Idle CPU** (~19 % → ~0 with no clients): on-demand now stops the
  `IMP_FrameSource` channel (not just the encoder) once a stream has no
  subscribers — an enabled FrameSource kept capturing/piping frames through the
  FS→OSD→encoder groups in the libimp worker threads. Producer threads now block
  on a condition variable instead of a poll loop, and the OSD updater only
  renders while a stream has viewers. Reactivation is immediate; the monitored
  FrameSource is pinned while motion detection is enabled.
- **`GET /control` capabilities** now report `caps.{image,audio,osd,restart}` so
  UIs can present exactly what this build/SoC supports.

## [1.1.0] - 2026-07-11

### Added
- **Live control API** (`POST`/`GET /control`, compile flag `USE_CONTROL`, on by
  default). A nested JSON blob changes settings live *and* persists the changed
  keys back to the config file (atomic tmp+rename, comments/order preserved).
  Supported: `image` (brightness, contrast, saturation, sharpness, hue, hflip,
  vflip, running_mode), `audio` (volume, gain), `osd.N` overlays, `video.N`
  bitrate (persisted only — applies on restart). The legacy flat form and
  `{"force_mode":"day"|"night"}` still work. Requests from localhost bypass
  auth; remote access requires configured HTTP/RTSP credentials.
- **Native automatic day/night** (compile flag `USE_DAYNIGHT`, on by default). A
  background thread reads ISP brightness from `/proc/jz/isp/isp-m0` and applies
  threshold + hysteresis + transition-delay logic (ported from thingino's
  `daynightd`), switching via the board's `daynight day|night` script (IR-cut /
  IR-LEDs / colour). Runtime toggle through `/control`
  (`{"daynight":{"enabled":true|false}}`). New `daynight.*` config keys
  (`enabled`, `threshold_low`, `threshold_high`, `hysteresis`, `interval_ms`,
  `transition_s`, `switch_cmd`, `isp_path`).
- **Live OSD apply** (`imp_osd_apply`): OSD overlay changes made through
  `/control` are re-rendered on the running streams.
- `config_get_kv()` — read a config value back as a normalized string
  (used for change detection).

### Fixed / Hardened
- **`/control` change detection**: a value that does not actually change is no
  longer re-applied to the ISP nor rewritten to the config file. This stops
  clients that poll and re-post the same value every few seconds from hammering
  the ISP and, worse, rewriting the config on flash over and over.
- **`/control` input validation**: invalid values (`null`, `undefined`, empty)
  are rejected instead of being stored and parsed to `0`.
- **Config-injection defense**: persisted values are stripped of control
  characters and double quotes before being written to the flat config file.

### Build
- `USE_DAYNIGHT` added to the Makefile (target and host-sim recipes); both
  `USE_CONTROL` and `USE_DAYNIGHT` default on and can be disabled independently
  (`USE_CONTROL=0` / `USE_DAYNIGHT=0`), compiling the feature out entirely.

## [1.0.0]

### Added
- Initial import: Tiny IMP Streamer — pure-C RTSP + fragmented-MP4 + JPEG/MJPEG
  streamer for Ingenic SoC cameras, built straight on the vendor `libimp`
  (no live555 / libconfig / libwebsockets / libschrift). On-demand encoding,
  TrueType OSD, motion detection, RTSP-Digest / HTTP-Basic auth. Ingenic
  headers via the `ingenic-headers` submodule.

[1.2.0]: https://github.com/Lu-Fi/timps/compare/v1.1.0...v1.2.0
[1.1.0]: https://github.com/Lu-Fi/timps/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/Lu-Fi/timps/releases/tag/v1.0.0
