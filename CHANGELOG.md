# Changelog

All notable changes to timps are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/), and the project aims to follow
semantic versioning.

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
