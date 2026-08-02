# Configuration Reference

timps reads a single flat `key = value` file (default `/etc/timps.conf`,
overridable with `-c <path>`) — see `timps.conf.example` /
`scripts/camera.conf` for worked examples. There is no INI-style `[section]`
syntax; the "section" is simply the dotted prefix of the key (`image.`,
`audio.`, `video0.`, ...).

This page documents **every** key recognized by `src/config.c`, grouped by
section, with type, default, valid range, and — importantly — whether the
key can be changed **live** via `POST /control` (see
[HTTP /control API Reference](HTTP-Control-API.md)) or is **persist-only**
(the value is stored/echoed but only takes effect the next time `timpsd`
restarts).

## How to read the "Live?" column

| Value | Meaning |
| --- | --- |
| **Live** | A `/control` POST applies the value to the running pipeline immediately (subject to per-SoC hardware support — see [Platform & SDK Support](Platform-SDK-Support.md)) and persists it to the config file. |
| **Live (next cycle)** | Not applied via a direct HAL call, but the owning thread (recorder, timelapse, day/night) re-reads the live in-memory config on every loop iteration, so the change takes effect within a few hundred ms to a few seconds without a restart. |
| **Restart-only** | The key is only read once at startup (or at ISP/encoder init). A `/control` POST for one of these still updates the in-memory config and persists it to the file (when the section accepts POSTs at all — see below), but the *running* daemon keeps behaving as it did at boot; the new value takes effect on the next `timpsd` restart. |
| **File-only** | The key has **no `/control` POST path at all** — it can only be changed by editing the config file (or via an offline tool that calls `config_write_keys()`) and restarting. Some of these are still included in `GET /control`'s read-only dump; others are not readable back at all (noted individually). |

This classification is derived directly from `src/control.c`'s
`control_apply_json()` (which sections/keys it accepts) and
`src/config.c`'s descriptor tables (which keys `GET /control`/change-detection
can read back) — not guessed from names or comments.

Two important caveats:

- **`image.*` live-apply is per-SoC**: every `image.*` key is *accepted*
  live regardless of platform, but the HAL only issues the underlying
  `IMP_ISP_Tuning_*` call when that platform's SDK actually exposes it
  (see the per-key notes below and `caps.image` in `GET /control`). On an
  unsupported platform the value is stored/echoed but has no visible
  effect.
- **`videoN.rtsp_path` is a documented exception**: it is POSTed through
  the same code path as the other (persist-only) `video.N.*` keys and
  `GET /control`'s `caps.restart` array lists the whole `video` section as
  restart-required — but `config.h` explicitly carves it out: RTSP
  `DESCRIBE` matches the request path against the **live** config on every
  request, not the boot snapshot, so a `/control` edit to `rtsp_path`
  actually changes which URL serves that stream immediately, with no
  restart. Every other `video.N.*` key (codec/dimensions/fps/bitrate/...)
  genuinely needs a restart because the encoder/FrameSource pipeline is
  never reconfigured live.

---

## `sensor.*` — camera sensor

Restart-only; the sensor is probed and configured once at ISP init
(`config_sensor_finalize()`). Not reachable via `/control` at all (no
`"sensor"` handling in `control_apply_json` beyond being listed for
completeness — actually it **is** accepted for persistence, see below).
Note: `config_sensor_finalize()` auto-detects unset fields from
`/proc/jz/sensor/sensor0/*` and a config value that disagrees with the
loaded kernel sensor driver is **overridden** (with a warning) rather than
trusted, because a mismatching name/i2c address would crash the ISP driver.

| Key | Type | Default | Range | Live? | Description |
| --- | --- | --- | --- | --- | --- |
| `sensor.model` | string | *(unset → auto-detected, fallback `gc2053`)* | — | Restart-only | Sensor driver name; auto-filled from `/proc/jz/sensor/sensor0/name` if present. |
| `sensor.i2c_addr` (alias `i2c_address`) | int | *(unset → auto-detected, fallback `0x37`)* | — | Restart-only | Sensor I2C address. |
| `sensor.fps` | int | *(unset → auto/video0.fps, fallback 25)* | — | Restart-only | Sensor capture frame rate. |
| `sensor.width` | int | *(unset → auto/video0.width, fallback 1920)* | — | Restart-only | Sensor native width. |
| `sensor.height` | int | *(unset → auto/video0.height, fallback 1080)* | — | Restart-only | Sensor native height. |

`sensor.*` **is** accepted by `POST /control` (`{"sensor":{...}}`) for
persistence — it is listed in `caps.restart` alongside `video` — but has
no live-apply path; the value is written to the config file and takes
effect at the next ISP init.

## `image.*` — ISP tuning

**All keys live-apply**, gated per-SoC (see `caps.image` in `GET /control`
and the ISP-capability column below; the underlying macros live in
`src/isp_caps.h`). A value the platform can't apply is still stored and
echoed back — it just has no visible effect until/unless a firmware update
adds support. `image.running_mode` is additionally special: re-posting the
*same* value still re-drives the ISP (skipping the flash write) because it
is treated as a hardware-sync command, not just stored state — see
[Day/Night](Day-Night.md) for why, including the `fs_kick_running_mode()`
latch-kick this triggers.

| Key | Type | Default | Range | Live? | ISP capability gate | Description |
| --- | --- | --- | --- | --- | --- | --- |
| `image.brightness` | int | 128 | 0–255 | Live | all SoCs | ISP brightness. |
| `image.contrast` | int | 128 | 0–255 | Live | all SoCs | ISP contrast. |
| `image.saturation` | int | 128 | 0–255 | Live | all SoCs | ISP saturation. |
| `image.sharpness` | int | 128 | 0–255 | Live | all SoCs | ISP sharpness. |
| `image.hue` | int | 128 | 0–255 | Live | `ISP_HAS_HUE`: T23/T31/T40/T41/C100 | Hue (`SetBcshHue`). |
| `image.hflip` | bool | 0 | 0/1 | Live | all SoCs (via `SetHVFLIP` on T40/T41) | Horizontal flip. Global (all streams). Use with `vflip` for a 180° flip on SoCs without a rotation path. |
| `image.vflip` | bool | 0 | 0/1 | Live | all SoCs | Vertical flip. Global (all streams). |
| `image.running_mode` | int | 0 | 0 = day, 1 = night | Live | all SoCs | ISP day/night running mode. Normally driven by the [Day/Night](Day-Night.md) thread's switch script, not set directly by a user. |
| `image.anti_flicker` | int | 2 | 0 = off, 1 = 50 Hz, 2 = 60 Hz | Live | all SoCs | Anti-flicker (banding) compensation. |
| `image.ae_compensation` | int | 128 | 0–255 | Live | `ISP_HAS_AECOMP`: all except T21, T40/T41 | AE (auto-exposure) compensation/EV bias. |
| `image.max_again` | int | 160 | 0–255 | Live | `ISP_HAS_GAINS`: all except T40/T41 | Max analog gain ceiling. |
| `image.max_dgain` | int | 80 | 0–255 | Live | `ISP_HAS_GAINS` | Max digital gain ceiling. |
| `image.sinter_strength` | int | 128 | 0–255 | Live | `ISP_HAS_NR`: all except T40/T41 | Spatial noise reduction strength. |
| `image.temper_strength` | int | 128 | 0–255 | Live | `ISP_HAS_NR` | Temporal noise reduction strength. |
| `image.dpc_strength` | int | 128 | 0–255 | Live | `ISP_HAS_DPC`: T23/T31/C100 | Defective pixel correction strength. |
| `image.defog_strength` | int | 128 | 0–255 | Live | `ISP_HAS_DEFOG`: T23/T31/C100 | Defog strength. |
| `image.drc_strength` | int | 128 | 0–255 | Live | `ISP_HAS_DRC` (WDR): T21/T23/T31/C100 | Dynamic range compression (WDR) strength. |
| `image.highlight_depress` | int | 0 | 0–10 | Live | `ISP_HAS_HILIGHT`: all except T40/T41 | Highlight suppression. |
| `image.backlight_compensation` | int | 0 | 0–10 | Live | `ISP_HAS_BACKLIGHT`: T23/T31/C100 | Backlight compensation. |
| `image.core_wb_mode` | int | 0 | SDK enum | Live | `ISP_HAS_WB`: all except T40/T41 | White balance mode. |
| `image.wb_rgain` | int | 0 | 0–65535 | Live | `ISP_HAS_WB` | Manual WB red gain (used when `core_wb_mode` selects manual). |
| `image.wb_bgain` | int | 0 | 0–65535 | Live | `ISP_HAS_WB` | Manual WB blue gain. |

## `audio.*` — capture, encode, speaker defaults

Split between live-applicable knobs (real-time DSP/gain calls) and
persist-only attribute keys (codec/format/pipeline-init settings that
`IMP_AI_SetPubAttr`/encoder init need at bring-up time and cannot change
on a running capture channel).

| Key | Type | Default | Range | Live? | Description |
| --- | --- | --- | --- | --- | --- |
| `audio.volume` | int | 80 | 0–100 | **Live** | Mic capture volume (`IMP_AI_SetVol`). |
| `audio.gain` | int | 25 | 0–31 | **Live** | Mic capture gain (`IMP_AI_SetGain`). |
| `audio.alc_gain` | int | 0 | 0–7 | **Live** on T21/T31/C100 (`AUDIO_HAS_ALC_GAIN`); accepted/persisted elsewhere with no effect | Analog PGA gain level (`IMP_AI_SetAlcGain`). |
| `audio.mute` | bool | 0 | 0/1 | **Live** | Live mic mute: captured frames are dropped before the encoder/hub (works on every SoC — it's a publish-gate in timps, not an SDK call). |
| `audio.spk_volume` | int | 80 | 0–100 | **Live** (only if `USE_PLAY` or `USE_BACKCHANNEL` compiled in) | Speaker (`IMP_AO`) output volume — applies to whichever producer (backchannel/play queue) currently holds the AO device, and persists as the default for the next one. |
| `audio.spk_gain` | int | 25 | 0–100 | **Live** (same gate as `spk_volume`) | Speaker output gain. |
| `audio.enabled` | bool | 1 | 0/1 | Restart-only | Master audio capture enable. |
| `audio.codec` | enum | `aac` | `aac`\|`pcmu`\|`pcma`\|`none` (aliases `g711u`/`ulaw`→pcmu, `g711a`/`alaw`→pcma, `off`→none) | Restart-only | Audio codec. AAC needs `USE_FAAC`; falls back to AAC at parse time for any unrecognized token. |
| `audio.samplerate` | int | 16000 | unclamped | Restart-only | Capture sample rate in Hz. G.711 is pinned to 8000 Hz regardless of this value (see [Audio](Audio.md)). |
| `audio.channels` | int | 1 | 1–2 | Restart-only | 1 = mono (native). 2 = "simulated stereo" — the mono mic duplicated to L=R, AAC only. |
| `audio.bitrate` | int | 32 | 8–320 (kbps) | Restart-only | AAC encode bitrate. |
| `audio.high_pass` | bool | 0 | 0/1 | Restart-only | High-pass filter. **Not live**: libimp runs HPF/AGC/NS on its own record thread and frees state unlocked — a live toggle would race the vendor thread (use-after-free risk), so this is restart-required by design, not merely unimplemented. |
| `audio.agc` | bool | 0 | 0/1 | Restart-only | Automatic gain control on/off (same race-avoidance rationale as `high_pass`). |
| `audio.agc_target_dbfs` | int | 10 | 0–31 | Restart-only | AGC target level (dBFS). |
| `audio.agc_compression_db` | int | 0 | 0–90 | Restart-only | AGC compression gain (dB). |
| `audio.ns` | int | 0 | 0–3 | Restart-only | Noise suppression level (0 = off). |
| `audio.force_stereo` | bool | 0 | 0/1 | Restart-only | Alias/companion to `channels=2`. |
| `audio.spk_enabled` | bool | 1 | 0/1 | Restart-only | Master speaker-output enable. |
| `audio.backchannel` | bool | 0 | 0/1 | Restart-only | Enable the ONVIF two-way audio backchannel (`USE_BACKCHANNEL` build). |
| `audio.backchannel_codec` | enum | `pcmu` (0) | `pcmu`(0)\|`pcma`(1)\|`aac`(2) | Restart-only | Advertised/accepted backchannel codec. |
| `audio.backchannel_rate` | int | 16000 | 8000–48000 | Restart-only | Speaker sample rate fed by the backchannel decoder. |

See [Audio](Audio.md) for the backchannel/play-queue feature details.

## `jpeg.*` — dedicated JPEG/MJPEG channel

Entirely **file-only**: there is no `"jpeg"` object in the `/control` POST
schema, so these keys can only be changed by editing the config file and
restarting. (Note this is distinct from the per-stream *piggyback* JPEG
encoder configured under `video.N.jpeg*`, below — see
[Streaming Protocols](Streaming-Protocols.md) for how the two JPEG sources
relate.)

| Key | Type | Default | Range | Live? | Description |
| --- | --- | --- | --- | --- | --- |
| `jpeg.enabled` | bool | 0 | 0/1 | File-only | Enable the dedicated JPEG encoder channel (independent FrameSource). |
| `jpeg.width` | int | 640 | 64–4096 | File-only | JPEG channel width. |
| `jpeg.height` | int | 360 | 64–4096 | File-only | JPEG channel height. |
| `jpeg.quality` | int | 75 | 1–100 | File-only | JPEG quality. |
| `jpeg.fps` | int | 5 | 1–120 | File-only | Max snapshot/MJPEG publish rate for this channel. |
| `jpeg.imp_chn` | int | 2 | — | File-only | IMP encoder channel number (internal). |
| `jpeg.snapshot_path` | string | `""` | — | File-only | Optional path to periodically write the latest JPEG to disk (`""` = disabled). |

## `rtsp.*` — RTSP server

File-only (no `"rtsp"` object accepted by `/control`). See
[Streaming Protocols](Streaming-Protocols.md) for the RTSP server's
runtime behavior.

| Key | Type | Default | Range | Live? | Description |
| --- | --- | --- | --- | --- | --- |
| `rtsp.enabled` | bool | 1 | 0/1 | File-only | Enable the RTSP server. |
| `rtsp.port` | int | 554 | 1–65535 | File-only | RTSP listener port. |
| `rtsp.mtu` | int | 1200 | 548–1472 | File-only | Max RTP packet size (header+payload) for UDP transport; 1200 leaves headroom for WireGuard/OpenVPN/PPPoE/IPv6 tunnel overhead — raise to ~1400 on LAN-only setups. |
| `rtsp.user` (alias `username`) | string | `""` | — | File-only | RTSP username. Empty = RTSP auth disabled. |
| `rtsp.pass` (alias `password`) | string | `""` | — | File-only | RTSP password. |
| `rtsp.tls` (alias `tls_enabled`) | bool | 0 | 0/1 | File-only | Also run an RTSPS (TLS) listener (`USE_TLS` builds). |
| `rtsp.tls_port` | int | 322 | 1–65535 | File-only | RTSPS port (RFC 7826 Annex C well-known port). |

## `srt.*` — SRT output

File-only. `USE_SRT` builds only (see [Streaming Protocols](Streaming-Protocols.md)).

| Key | Type | Default | Range | Live? | Description |
| --- | --- | --- | --- | --- | --- |
| `srt.enabled` | bool | 0 | 0/1 | File-only | Enable the SRT listener. |
| `srt.port` | int | 9000 | 1–65535 | File-only | SRT listener port. |
| `srt.channel` | int | 0 | — | File-only | Video stream served over SRT. |
| `srt.latency_ms` (alias `latency`) | int | 120 | — | File-only | SRT latency (ms). |
| `srt.streamid` | string | `""` | — | File-only | Required `STREAMID` for a connecting client (`""` = not enforced). |
| `srt.passphrase` | string | `""` | — | File-only | AES passphrase for SRT encryption (10–79 chars if set; the listener refuses to start on an invalid passphrase rather than run unencrypted). |

## `http.*` — HTTP server (fMP4/MJPEG/snapshot/control/events)

File-only. See [HTTP /control API Reference](HTTP-Control-API.md) and
[Streaming Protocols](Streaming-Protocols.md).

| Key | Type | Default | Range | Live? | Description |
| --- | --- | --- | --- | --- | --- |
| `http.enabled` | bool | 1 | 0/1 | File-only | Enable the HTTP server. |
| `http.port` | int | 8880 | 1–65535 | File-only | HTTP listener port. |
| `http.preview_chn` | int | 1 | — | File-only | Default video stream for the built-in preview page. |
| `http.adaptive_drop` | bool | 1 | 0/1 | File-only | Per-client adaptive frame dropping on `/stream.mp4` when a slow client's own queue backs up (freeze-and-resume-at-keyframe instead of forwarding a corrupt headless GOP). |
| `http.user` (alias `username`) | string | `""` | — | File-only | HTTP Basic/Digest username. Falls back to `rtsp.user` if empty. |
| `http.pass` (alias `password`) | string | `""` | — | File-only | HTTP password. Falls back to `rtsp.pass` if empty. |
| `http.token` | string | `""` | — | File-only | Optional persistent remote secret token for `/control`/`/events`/media endpoints (never written to the token file). |
| `http.token_file` | string | `/run/timps.token` | — | File-only | Where the random per-boot token is published (mode 0640); `""` disables publishing. |
| `http.https` (alias `tls`) | bool | 0 | 0/1 | File-only | Serve HTTP over TLS (`USE_TLS` builds). |
| `http.tls_cert` (alias `cert`) | string | `/etc/ssl/certs/httpd.crt` | — | File-only | PEM certificate (shared with RTSPS). |
| `http.tls_key` (alias `key`) | string | `/etc/ssl/private/httpd.key` | — | File-only | PEM private key (shared with RTSPS). |

## `events.*` — Server-Sent-Events push stream

File-only startup settings (`USE_CONTROL` builds).

| Key | Type | Default | Range | Live? | Description |
| --- | --- | --- | --- | --- | --- |
| `events.enabled` | bool | 1 | 0/1 | File-only | `0` makes `/events` answer 404. |
| `events.stats_ms` | int | 2000 | — | File-only | Period of the `stats` SSE event; `0` disables it. |
| `events.max_clients` | int | 8 | — | File-only | Max concurrent `/events` connections; beyond this, `/events` answers 503. |

## `osd.*` — on-screen display (global settings)

The master switch and the shared OSD infrastructure settings. Per-item
overlay fields are documented separately below (`osd<S>.<N>.*`).

| Key | Type | Default | Range | Live? | Description |
| --- | --- | --- | --- | --- | --- |
| `osd.enabled` | bool | 1 | 0/1 | Restart-only | Master OSD on/off switch, global across all streams. Settable via `/control` (`{"osd":{"enabled":...}}`) and persists, but the OSD groups are only ever built once at startup (`imp_osd_setup`), so the effect needs a restart. |
| `osd.monitor_stream` | int | 0 | — | File-only | Which stream's measured fps feeds the `{fps}` placeholder. |
| `osd.font_path` | string | `/usr/share/fonts/default.ttf` | — | File-only | Default TTF font for text items without a per-item `font_path` override. |
| `osd.vars_file` | string | `/tmp/timps_osd.vars` | — | File-only | Custom placeholder source file. |
| `osd.supersample` | int | 2 | 1–4 | File-only | TTF rasterizer anti-aliasing quality (samples per axis per pixel); cost scales ~quadratically, 2 is visually close to 4 at typical OSD sizes for roughly a quarter of the CPU cost. |

## `osd<S>.<N>.*` — per-stream OSD overlay items

Canonical form: `osd<S>.<N>.<field>` where `S` is the video stream index
(0/1) and `N` is the item slot (0–7, `MS_MAX_OSD`). A legacy shared form
`osd<N>.<field>` (no stream index) is still parsed for backward
compatibility and mirrors the value onto **every** stream's item `N`.
Default layout: item 0 = timestamp (top-left), item 1 = `{hostname}`
(top-center), item 2 = `{uptime}` (top-right), item 3 = thingino logo
(bottom-right); items 4–7 are unused placeholders (disabled).

| Key | Type | Default | Range | Live? | Description |
| --- | --- | --- | --- | --- | --- |
| `enabled` | bool | item 0–3: `1`, items 4–7: `0` | 0/1 | **Restart-only** | Enabling an item that started disabled has no IMP region to attach to — only takes effect on restart. Disabling a running item also only persists (no live hide). |
| `type` | enum | `text` (item 3: `logo`) | `text`\|`logo` | File-only | Overlay type. Not settable via `/control` at all. |
| `text` | string(128) | per-item (see layout above) | — | **Live** (if the item had a region at startup) | Text template: literal text, `{placeholder}` tokens (`{hostname} {ip} {mac} {fps} {uptime} {net} {cpu} {mem} {clients}`), and `strftime()` codes. |
| `logo` (alias `logo_path`) | string(128) | `/usr/share/images/thingino_100x30.bgra` (item 3) | — | File-only | Raw BGRA logo file path. |
| `logo_w` (alias `logo_width`) | int | 100 (item 3) | 0–4096 | File-only | Logo width in px. |
| `logo_h` (alias `logo_height`) | int | 30 (item 3) | 0–4096 | File-only | Logo height in px. |
| `x` | int | per-item | — | **Live** | X position: `0` = centered, `>0` = pixels from the left edge, `<0` = pixels from the right edge. |
| `y` | int | per-item | — | **Live** | Y position: same convention, top/bottom. |
| `font_size` | int | 32 (stream 0) / 12 (other streams) | 8–256 | **Live** | Absolute pixel font size (no per-stream auto-scaling). |
| `color` (alias `font_color`) | hex (0xAARRGGBB) | `0xFFFFFFFF` | — | **Live** | Text fill color. |
| `transparency` | int | 255 | 0–255 | **Live** | Group alpha. |
| `outline` (alias `stroke`) | int | 1 | 0–64 | **Live** | Text outline/stroke width in px (`0` = off). On by default so overlays stay legible on light backgrounds. |
| `outline_color` (alias `stroke_color`) | hex (0xAARRGGBB) | `0xFF000000` | — | **Live** | Outline/stroke color. |
| `font_path` | string(128) | `""` (inherits `osd.font_path`) | — | File-only | Optional per-item TTF override. |

## `privacy<S>.<N>.*` — privacy cover masks

`S` = video stream index (0/1), `N` = region slot (0–3, `MS_MAX_PRIVACY`).
Implemented as IMP OSD cover regions; live-appliable as long as an OSD
group exists on that stream (i.e. OSD or a privacy region was enabled at
boot — `GET /control`'s `caps.privacy.available` reflects this).

| Key | Type | Default | Range | Live? | Description |
| --- | --- | --- | --- | --- | --- |
| `enabled` | bool | 0 | 0/1 | **Live**\* | Show/hide this cover region. |
| `x` | int | 0 | — | **Live**\* | Rectangle X origin, pixels, in the stream's frame. |
| `y` | int | 0 | — | **Live**\* | Rectangle Y origin. |
| `w` (alias `width`) | int | 0 | — | **Live**\* | Rectangle width. |
| `h` (alias `height`) | int | 0 | — | **Live**\* | Rectangle height. |
| `color` (alias `fill_color`) | hex (0xAARRGGBB) | `0xFF000000` (opaque black) | — | **Live**\* | Fill color. |

\* Live only if an OSD group already exists for that stream at boot;
otherwise the value persists but has no visible effect until restart.

## `motion.*` — motion detection

See [Motion Detection](Motion-Detection.md) for the full grid model. Only
`enabled`/`sensitivity`/`cols`/`rows`/`monitor_stream` are POST-able; the
rest are config-file-only (the running IVS thread does read them, but
there is no live-update path, so a change needs a restart to take
effect).

| Key | Type | Default | Range | Live? | Description |
| --- | --- | --- | --- | --- | --- |
| `motion.enabled` | bool | 0 | 0/1 | **Live** | Enable/disable IVS motion detection (stops/recreates the whole IVS grid). |
| `motion.monitor_stream` | int (channel) | 0 | valid stream index | **Live** | Which video stream's frame the grid is computed over. |
| `motion.sensitivity` | int | 128 | 0–255 | **Live** | UI-facing sensitivity; mapped to the SDK's native 0–4 range (`v*4/255`) applied uniformly to every cell. A change that maps to the same SDK level as before is treated as unchanged (no IVS rebuild). |
| `motion.cols` | int | 5 (or 2/1 on SDKs with a smaller ROI budget) | ≥1, `cols*rows` clamped to `MOTION_CELL_LIMIT` | **Live** | Grid columns. Setting one axis clamps against the *current* value of the other, never the reverse, so re-applying the same pair is idempotent. |
| `motion.rows` | int | 5 (or 2/1) | ≥1, same clamp | **Live** | Grid rows. |
| `motion.cooldown_ms` | int | 5000 | 250–`INT_MAX` (floor enforced) | File-only | Minimum gap between `on_motion` hook executions. |
| `motion.hold_ms` | int | 800 | 0–`INT_MAX` | File-only | How long a cell reports "active" after its last hit, so an async `/events`/`/control` reader reliably observes single-frame motion instead of racing IVS's own immediate clear. `0` = no hold. |
| `motion.skip_frames` | int | 5 | 1–`INT_MAX` | File-only | `IMP_IVS_MoveParam.skipFrameCnt` — analyze every Nth frame. Higher = cheaper/higher latency. |
| `motion.on_motion` | string(128) | `""` | — | File-only, **not GET-readable either** | Program to `fork()`+`execlp()` on motion (no shell, no arguments). `""` = disabled. Receives `MOTION_COLS`/`MOTION_ROWS`/`MOTION_CELLS`/`MOTION_TIME` via the environment — see [Motion Detection](Motion-Detection.md). |
| `motion.roi_x`/`roi_y`/`roi_w`/`roi_h` | int | 0 | — | File-only, **deprecated** | Legacy single-ROI keys, still parsed for old configs but **ignored** — the grid replaced them. Setting a non-zero value logs a one-time warning. |

## `record.*` — local SD recording

See [Recording & Timelapse](Recording-Timelapse.md). All settable keys are
re-read live by the recorder thread on every loop pass, so changes apply
within its ~300 ms poll cycle without a restart.

| Key | Type | Default | Range | Live? | Description |
| --- | --- | --- | --- | --- | --- |
| `record.enabled` | bool | 0 | 0/1 | **Live (next cycle)** | Master enable; also gates on-boot start. |
| `record.channel` | int (channel) | 0 | valid stream index | **Live (next cycle)** | Video stream to record. |
| `record.mode` | enum | `1` (motion) | `continuous`(0)\|`motion`(1)\|raw number | **Live (next cycle)** | Continuous vs. motion-triggered recording. |
| `record.dir` | string(128) | `/mnt/mmcblk0p1` | — | **Live (next cycle)** | SD base directory. |
| `record.name` | string(96) | `%Y%m%d/%H/%Y%m%dT%H%M%S` | `strftime` template | **Live (next cycle)** | Path template under `<dir>/<hostname>/records/`. |
| `record.segment_s` (alias `segment`) | int | 60 | 0–86400 | **Live (next cycle)** | Max segment length in seconds; rotation only happens at a video keyframe. `0` = single file, no rotation. |
| `record.pre_roll_s` (alias `pre_roll`) | int | 3 | 0–60 | **Live (next cycle)** | Motion mode: seconds of buffered video kept before the trigger (ring buffer). |
| `record.post_roll_s` (alias `post_roll`) | int | 10 | 0–300 | **Live (next cycle)** | Motion mode: keep recording this long after the last motion event. |
| `record.min_free_mb` | int | 200 | 0–1048576 | **Live (next cycle)** | Prune oldest segments until at least this much free space remains. |
| `record.audio` | bool | 1 | 0/1 | **Live (next cycle)** | Mux AAC audio into the recording when available (G.711 cannot be muxed into fMP4). |

`{"record":{"active":1|0}}` (not a config key) is a transient manual
start/stop override, and `{"record":{"clip":"<path>","seconds":N}}`
triggers an independent on-demand fMP4 clip capture — neither is
persisted to the config file.

## `timelapse.*` — periodic JPEG capture

See [Recording & Timelapse](Recording-Timelapse.md). Like `record.*`, the
running thread re-reads these live.

| Key | Type | Default | Range | Live? | Description |
| --- | --- | --- | --- | --- | --- |
| `timelapse.enabled` | bool | 0 | 0/1 | **Live (next cycle)** | Master enable; also gates on-boot start. |
| `timelapse.channel` | int (channel) | 0 | valid stream index | **Live (next cycle)** | Video stream whose JPEG is captured. |
| `timelapse.dir` | string(128) | `/mnt/mmcblk0p1` | — | **Live (next cycle)** | Base directory (SD, NFS, any writable path). |
| `timelapse.name` | string(96) | `%Y%m%d/%H/%Y%m%dT%H%M%S` | `strftime` template | **Live (next cycle)** | Path template under `<dir>/<hostname>/timelapses/`. |
| `timelapse.interval_s` (alias `interval`) | int | 60 | ≥1 | **Live (next cycle)** | Seconds between shots. |
| `timelapse.keep_days` | int | 7 | ≥0 | **Live (next cycle)** | Delete shots older than this; `0` = keep forever. |

## `daynight.*` — automatic day/night

See [Day/Night](Day-Night.md) for the full decision logic, the three
`mode` values, and the ISP running-mode latch fix. Note `switch_cmd` and
`isp_path` are **not GET-readable** even though they are settable via the
config file.

| Key | Type | Default | Range | Live? | Description |
| --- | --- | --- | --- | --- | --- |
| `daynight.enabled` | bool | 1 | 0/1 | **Live** | Auto-detection on/off; `0` = manual (thread still samples but forces nothing). |
| `daynight.mode` | enum | `sensor` | `sensor`\|`time`\|`sun` | **Live** | Decision source — see [Day/Night](Day-Night.md#override-modes). |
| `daynight.time_night_start` | string(6) | `""` | `"HH:MM"` | **Live** | `time` mode: local time to switch to night. |
| `daynight.time_day_start` | string(6) | `""` | `"HH:MM"` | **Live** | `time` mode: local time to switch to day. |
| `daynight.sun_latitude` | float | 0.0 | -90–90 | **Live** | `sun` mode: latitude, degrees. |
| `daynight.sun_longitude` | float | 0.0 | -180–180 | **Live** | `sun` mode: longitude, degrees. |
| `daynight.sun_sunrise_offset_min` | int | 0 | -1440–1440 | **Live** | Minutes added to computed sunrise before switching to day. |
| `daynight.sun_sunset_offset_min` | int | 0 | -1440–1440 | **Live** | Minutes added to computed sunset before switching to night. |
| `daynight.total_gain_day_threshold` | float | 300 | 1–1,000,000 | **Live** | `sensor` mode: in night, switch to day when ISP total gain drops below this. |
| `daynight.total_gain_night_threshold` | float | 3000 | 1–1,000,000 | **Live** | `sensor` mode: in day, switch to night when gain rises above this. |
| `daynight.day_gain_pct` | int | 60 | 0–100 | **Live** | Adaptive night→day trigger as a percentage of the sampled night baseline gain (`0` disables, falls back to the fixed threshold). |
| `daynight.baseline_delay_s` | int | 30 | 0–3600 | **Live** | Seconds into night before the adaptive baseline gain is sampled (lets IR LEDs settle). |
| `daynight.threshold_low` | float | 25.0 | 0–100 (%) | File-only | Brightness-fallback: below this in day → night (used only when no gain field is readable). |
| `daynight.threshold_high` | float | 75.0 | 0–100 (%) | File-only | Brightness-fallback: above this in night → day. |
| `daynight.hysteresis` | float | 0.1 | 0–1 | File-only | Fraction of the low–high band used for the very first (unknown-state) brightness-fallback decision. |
| `daynight.interval_ms` | int | 500 | 100–60000 | File-only | Sample interval. |
| `daynight.transition_s` | int | 5 | 0–3600 | File-only | Minimum dwell time between switches. |
| `daynight.switch_cmd` | string(64) | `daynight` | — | File-only, not GET-readable | Board script run as `<cmd> day\|night` on a switch. |
| `daynight.isp_path` | string(128) | `/proc/jz/isp/isp-m0` | — | File-only, not GET-readable | ISP exposure proc file for the brightness-fallback scrape. |

## `general.*` — daemon-wide settings

File-only; no `/control` POST path.

| Key | Type | Default | Range | Live? | Description |
| --- | --- | --- | --- | --- | --- |
| `general.loglevel` | int | `LOG_INFO` (2) | SDK log-level enum | File-only | Startup log verbosity (`-v` on the command line also forces debug). |
| `general.imp_polling_timeout` | int | 500 | — | File-only | `IMP_Encoder_PollingStream` timeout, ms. |
| `general.osd_pool_size` | int | 1024 | — | File-only | Max OSD pool size (bytes) reserved for small OSD regions. |
| `general.syslog` | bool | on | 0/1 | File-only | Also mirror log output to syslog (`logread`). A side-effecting key: applied immediately at config-load time via `log_set_syslog()`, but not stored as a field, so it cannot be echoed back by any getter. |

## `sim.*` — host simulation backend

Only meaningful for `make sim` builds (`src/hal/hal_sim.c`); ignored by a
real-hardware (`HAL_INGENIC`) build. File-only.

| Key | Type | Default | Range | Live? | Description |
| --- | --- | --- | --- | --- | --- |
| `sim.video0` | string(256) | `""` | file path | File-only | Annex-B H.264/H.265 file looped as the video0 source. |
| `sim.video1` | string(256) | `""` | file path | File-only | Annex-B file looped as the video1 source. |
| `sim.audio` | string(256) | `""` | file path | File-only | ADTS AAC file looped as the audio source. |
| `sim.jpeg` | string(256) | `""` | file path | File-only | Still JPEG file republished as the JPEG source(s). |

## `video<N>.*` — per-stream encoder settings

`N` is the video stream index (`0` = main, `1` = sub; `MS_MAX_VSTREAM=2`
streams total). **Every key here is persist-only / restart-required**
(the encoder and FrameSource are never reconfigured on the running
pipeline) **except `rtsp_path`**, which is honestly live — see the caveat
at the top of this page. `GET /control` groups this whole section under
`caps.restart: ["video", "sensor", "osd.enabled"]`.

| Key | Type | Default (video0 / video1) | Range | Live? | Description |
| --- | --- | --- | --- | --- | --- |
| `video<N>.enabled` | bool | 1 / 1 | 0/1 | Restart-only | Enable this stream. |
| `video<N>.codec` | enum | `h264` / `h264` | `h264`\|`h265` (aliases `hevc`) | Restart-only | Video codec. H.265 only where the platform's encoder API supports it — see [Platform & SDK Support](Platform-SDK-Support.md). |
| `video<N>.width` | int | 1920 / 640 | 64–4096 | Restart-only | Encode width (pre-rotation). |
| `video<N>.height` | int | 1080 / 360 | 64–4096 | Restart-only | Encode height (pre-rotation). |
| `video<N>.fps` | int | 25 / 25 | 1–120 | Restart-only | Encode frame rate. |
| `video<N>.bitrate` | int | 3000 / 512 | 16–50000 (kbps) | Restart-only | Target bitrate. |
| `video<N>.rc_mode` (alias `mode`) | enum | `cbr` / `cbr` | `cbr`\|`vbr`\|`fixqp`\|`smart`\|`capped_vbr`\|`capped_quality` | Restart-only | Rate-control mode. |
| `video<N>.gop` | int | 50 / 50 | 1–1000 | Restart-only | GOP length (I-frame interval). |
| `video<N>.max_gop` | int | 60 / 60 | 1–1000 | Restart-only | Max GOP length. |
| `video<N>.profile` | int | 2 / 2 | 0 (baseline) – 2 (high) | Restart-only | H.264/H.265 encode profile. |
| `video<N>.qp` | int | 35 / 35 | 1–51 | Restart-only | Fixed/initial QP. |
| `video<N>.min_qp` | int | 20 / 20 | 1–51 | Restart-only | Minimum QP. |
| `video<N>.max_qp` | int | 45 / 45 | 1–51 | Restart-only | Maximum QP. |
| `video<N>.rotation` | int | 0 / 0 | `0`\|`90`\|`270` (+`180` on T40/T41 only); legacy `1`/`2` accepted as raw `rotTo90` values | Restart-only, `USE_ROTATE` builds only | Image rotation — see [Platform & SDK Support](Platform-SDK-Support.md) and `docs/rotation.md`. Unsupported values on a given SoC are coerced to `0` with a warning. |
| `video<N>.buffers` | int | 2 / 2 | 1–8 | Restart-only | IMP video buffer count (`nrVBs`). Explicitly setting this makes the HAL trust it as-is instead of applying its own safety clamp. |
| `video<N>.rtsp_path` | string(64) | `/ch0` / `/ch1` | — | **Live** (documented exception, see above) | RTSP mount path for this stream. |
| `video<N>.jpeg` (alias `jpeg_enabled`) | bool | 1 / 1 | 0/1 | File-only | Enable a piggyback JPEG encoder sharing this stream's FrameSource (used by `/snapshot.jpg`/`/stream.mjpeg?chn=N` and [timelapse](Recording-Timelapse.md)). |
| `video<N>.jpeg_quality` | int | 75 / 75 | 1–100 | File-only | Piggyback JPEG quality. |
| `video<N>.jpeg_fps` | int | 5 / 5 | 1–120 | File-only | Piggyback JPEG max publish rate. |
| `video<N>.jpeg_chn` | int | `MS_MAX_VSTREAM+1+N` | — | File-only | IMP encoder channel number for the piggyback JPEG encoder (internal). |
| `video<N>.imp_chn` | int | `N` | — | File-only, internal | IMP encoder channel number for the video stream itself. |

---

For the live-apply mechanics (how a POST maps onto these keys, JSON
shapes, and the `caps` object a client can use to discover this
classification programmatically) see
[HTTP /control API Reference](HTTP-Control-API.md).
