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
| `audio.codec` | enum | `aac` | `aac`\|`pcmu`\|`pcma`\|`opus`\|`none` (aliases `g711u`/`ulaw`→pcmu, `g711a`/`alaw`→pcma, `off`→none) | Restart-only | Audio codec. AAC needs `USE_FAAC`; `opus` needs `USE_STREAM_OPUS` (RTSP-only, RFC 7587 — see [Audio](Audio.md)/[Streaming Protocols](Streaming-Protocols.md)) and is only an accepted token on such a build. Falls back to AAC at parse time for any unrecognized token. |
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
| `audio.aec` | bool | 0 | 0/1 | **Live** (only if `USE_PLAY` or `USE_BACKCHANNEL` compiled in) | Opt-in Acoustic Echo Cancellation (`IMP_AI_EnableAec`) for the backchannel — subtracts the speaker output from the mic capture. Engages only once both AI capture and AO output are actually live, applied at the next AO open (same timing contract as `spk_volume`/`spk_gain`). Off by default since AEC quality/latency varies per SoC/mic/speaker pairing. |

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

> **Security — empty credentials expose the media (by design).** While both
> `http.user` and `rtsp.user` are empty (the shipped default), the media
> endpoints — the RTSP video/audio stream, `/snapshot.jpg`, `/stream.mjpeg`
> and `/stream.mp4` — are reachable by **anyone on the network with no
> authentication**. `/control` and `/events` are the exception: they stay
> **loopback-only** (non-local requests get `403`) even with empty
> credentials, so config/state is never exposed off-device without a token or
> configured credentials. Set `rtsp.user`/`rtsp.pass` and/or
> `http.user`/`http.pass` to require auth for the media too. See
> [HTTP /control API Reference § Empty credentials](HTTP-Control-API.md#empty-credentials-the-shipped-default--media-is-open-control-is-not).

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
| `osd.monitor_stream` | int | 0 | — | **Live** | Which stream's measured fps feeds the `{fps}` placeholder. Settable via `/control` (`{"osd":{"monitor_stream":...}}`); read directly off `g_cfg` on every OSD text refresh, so a POST applies on the next render, no restart needed. |
| `osd.font_path` | string | `/usr/share/fonts/default.ttf` | — | Restart-only | Default TTF font for text items without a per-item `font_path` override. Settable via `/control`, same restart-required class as `osd.enabled`. |
| `osd.vars_file` | string | `/tmp/timps_osd.vars` | — | Restart-only | Custom placeholder source file (see "Custom placeholders" below). Settable via `/control`, same restart-required class as `osd.enabled`. |
| `osd.supersample` | int | 2 | 1–4 | Restart-only | TTF rasterizer anti-aliasing quality (samples per axis per pixel); cost scales ~quadratically, 2 is visually close to 4 at typical OSD sizes for roughly a quarter of the CPU cost. Settable via `/control`, same restart-required class as `osd.enabled`. |
| `osd.hinting` | bool | 0 | 0/1 | Restart-only (only if `USE_OSD_HINTING` compiled in) | Opt-in lightweight geometric autohint for the TTF rasterizer. The rasterizer (`msttf.c`) does not execute the font's embedded TrueType hint bytecode (a real hint interpreter is real interpreter-writing work with a real correctness/security surface for an on-device, unsandboxed daemon); at small sizes (e.g. the substream OSD's default 12px) that shows up as visibly uneven stroke widths between glyphs. Enabling this snaps long, near-vertical/near-horizontal outline edges (typical letter stems/serifs) to the pixel grid before rasterizing, which measurably reduces that unevenness — it is a coarse heuristic, not real hinting, and does not preserve the font's authored hint intent. Off by default: existing installs render byte-for-byte identical OSD bitmaps unless this is explicitly enabled. Verified against the shipped UbuntuMono Regular (`/usr/share/fonts/default.ttf`); untested against other TTF files if a user swaps `osd.font_path`. Also gated at COMPILE time by `USE_OSD_HINTING` (`BR2_PACKAGE_TIMPS_OSD_HINTING` in the buildroot package, off by default — measured ~2.1KB smaller `.text` on T31/GCC 16.1.0/-Os when left off): on a build without it, setting this key is accepted but has no effect. Settable via `/control`, same restart-required class as `osd.enabled`. |

### Custom placeholders (show any value you want in the OSD)

Any `{name}` token in an OSD `text` template that isn't one of the built-in
placeholders (`hostname`, `ip`, `mac`, `fps`, `fpsN`, `uptime`, `net`/`tx`,
`cpu`, `mem`, `clients`) is looked up in `osd.vars_file` instead - a plain
`key = value` text file, one entry per line. This lets an external script
drive OSD content with no timps code changes at all: temperature readings,
a doorbell state, whatever.

1. Pick a name and use it in the OSD text, e.g. `osd0.4.text = Temp: {room_temp}C`.
2. From any script/cron job with filesystem access, write matching lines to
   the configured `vars_file` (default `/tmp/timps_osd.vars`):
   ```
   room_temp = 21.5
   doorbell = idle
   ```
3. timps re-reads the file on every OSD refresh tick (~1x/s) - no signal or
   restart needed. There's no locking, so writing in-place (e.g. `echo ... >
   file`, which truncates then writes) has a small window where a concurrent
   read can land mid-write: either an empty file (blank placeholder for that
   tick) or a partially-written value line (a truncated-but-not-garbage value
   for that tick, e.g. `21` instead of `21.5`) - never a crash or memory
   issue, since reads are line-bounded, just a possible one-tick display
   glitch that self-corrects on the next read. Avoid it entirely with an
   atomic replace: write the new content to a temp file in the same
   directory, then `rename()`/`mv` it onto the target path - `rename()` is
   atomic on Linux, so timps always sees either the complete old file or the
   complete new one, never a partial write.

Limits: each resolved value is capped at 127 bytes, and the whole expanded
`text` string (all placeholders combined, before `strftime()` substitution)
at 511 bytes - long enough for typical single-line status text, not a
replacement for a full custom rendering pipeline. Unmatched names (typo, or
the vars_file doesn't exist yet) resolve to an empty string rather than an
error, so a missing file just shows blank text for that placeholder.

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
| `type` | enum | `text` (item 3: `logo`) | `text`\|`logo` | **Restart-only** | Overlay type. Settable via `/control` and persists, but `imp_osd_apply()`'s live re-render dispatch is fixed at region-creation time (`rg->is_text`), so switching an existing item between text and logo needs a restart to actually change what's drawn (same restart-required reasoning as `enabled`). |
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

See [Motion Detection](Motion-Detection.md) for the full grid model.
`enabled`/`sensitivity`/`cols`/`rows`/`monitor_stream` are POST-able and
**Live**; `hold_ms`/`skip_frames` are also POST-able (persist + echo)
but **Restart-only** — they feed the IVS grid/hold logic only at
create/resync time, so a POST takes effect at the next
enabled/cols/rows/monitor_stream-triggered resync or daemon restart, not
immediately. `cooldown_ms`/`on_motion` stay config-file-only by design
(see below).

| Key | Type | Default | Range | Live? | Description |
| --- | --- | --- | --- | --- | --- |
| `motion.enabled` | bool | 0 | 0/1 | **Live** | Enable/disable IVS motion detection (stops/recreates the whole IVS grid). |
| `motion.monitor_stream` | int (channel) | 0 | valid stream index | **Live** | Which video stream's frame the grid is computed over. |
| `motion.sensitivity` | int | 128 | 0–255 | **Live** | UI-facing sensitivity; mapped to the SDK's native 0–4 range (`v*4/255`) applied uniformly to every cell. A change that maps to the same SDK level as before is treated as unchanged (no IVS rebuild). |
| `motion.cols` | int | 5 (or 2/1 on SDKs with a smaller ROI budget) | ≥1, `cols*rows` clamped to `MOTION_CELL_LIMIT` | **Live** | Grid columns. Setting one axis clamps against the *current* value of the other, never the reverse, so re-applying the same pair is idempotent. |
| `motion.rows` | int | 5 (or 2/1) | ≥1, same clamp | **Live** | Grid rows. |
| `motion.cooldown_ms` | int | 5000 | 250–`INT_MAX` (floor enforced) | File-only | Minimum gap between `on_motion` hook executions. |
| `motion.hold_ms` | int | 800 | 0–`INT_MAX` | Restart-only | How long a cell reports "active" after its last hit, so an async `/events`/`/control` reader reliably observes single-frame motion instead of racing IVS's own immediate clear. `0` = no hold. Settable via `/control`; persists and applies at the next grid create/resync (or restart), not immediately. |
| `motion.skip_frames` | int | 5 | 1–`INT_MAX` | Restart-only | `IMP_IVS_MoveParam.skipFrameCnt` — analyze every Nth frame. Higher = cheaper/higher latency. Settable via `/control`; persists and applies at the next grid create/resync (or restart), not immediately. |
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
| `record.post_roll_s` (alias `post_roll`) | int | 10 | 1–300 | **Live (next cycle)** | Motion mode: keep recording this long after the last motion event. |
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

See [Day/Night](Day-Night.md) for the full decision logic. The short version:
the metric is the **exposure index** (`total_gain × integration_time /
max_integration_time`, higher = darker — equal to `total_gain` in a dark scene
and far below it in a bright one); day→night is a direct measurement on the
honest day pipeline, night→day only ever happens via a **probe**; and the
calendar, when configured, schedules probes rather than deciding anything.

`switch_cmd`, `isp_path`, `trace_path` and `state_path` are **not
GET-readable** even though they are settable via the config file — each names
a path or program the daemon acts on as root.

**Renamed 2026-08-17:** `total_gain_day_threshold` → `day_gain` and
`total_gain_night_threshold` → `night_gain`. The old names still work as
aliases and the units and calibration are unchanged.
**Removed 2026-08-17** (parsed, ignored, warned about once):
`day_gain_pct`, `baseline_delay_s`, `boot_settle_max_s`, `boot_stable_pct`,
`night_reconfirm_s`, `probe_max_skip_s`, `threshold_low`, `threshold_high`,
`hysteresis`.

| Key | Type | Default | Range | Live? | Description |
| --- | --- | --- | --- | --- | --- |
| `daynight.enabled` | bool | 1 | 0/1 | **Live** | Auto-detection on/off; `0` = manual (thread still samples but forces nothing). |
| `daynight.mode` | enum | `auto` | `auto`\|`schedule` | **Live** | `auto` = the sensor automaton (calendar optional). `schedule` = the calendar decides outright, no sensor and no probes. The pre-2026-08-17 tokens still parse: `sensor`→`auto`, `time`/`sun`→`schedule`. |
| `daynight.time_night_start` | string(6) | `""` | `"HH:MM"` | **Live** | Calendar: local time night begins. Set together with `time_day_start`; takes precedence over the sun calendar. |
| `daynight.time_day_start` | string(6) | `""` | `"HH:MM"` | **Live** | Calendar: local time day begins. |
| `daynight.sun_latitude` | float | 0.0 | -90–90 | **Live** | Sun calendar: latitude. Used when no time window is set and either coordinate is non-zero. |
| `daynight.sun_longitude` | float | 0.0 | -180–180 | **Live** | Sun calendar: longitude. |
| `daynight.sun_sunrise_offset_min` | int | 0 | -1440–1440 | **Live** | Minutes added to computed sunrise. |
| `daynight.sun_sunset_offset_min` | int | 0 | -1440–1440 | **Live** | Minutes added to computed sunset. |
| `daynight.day_gain` | float | 768 | 1–1,000,000 | **Live** | Exposure index below which the **day pipeline** confirms day. 768 = 3× gain: "day is confirmed when the day pipeline can hold the scene at ≤3×". Alias: `total_gain_day_threshold`. |
| `daynight.night_gain` | float | 4096 | 1–1,000,000 | **Live** | Exposure index above which day ends. 4096 = 16×, "colour is hopeless". Alias: `total_gain_night_threshold`. |
| `daynight.day_confirm_s` | int | 30 | 1–3600 | **Live** | How long the index must stay above `night_gain` before day→night. |
| `daynight.probe_min_gap_s` | int | 600 | 60–86400 | **Live** | Minimum seconds between probes. **This is the only thing rationing the audible IR-cut click**, so the worst-case click rate is a property of the config rather than of interacting heuristics. |
| `daynight.probe_jump_pct` | int | 50 | 1–99 | **Live** | Probe when the index falls below this percentage of the *night reference* (the level at which night was last proven). 50 = one full stop of new brightening. |
| `daynight.probe_confirm_s` | int | 15 | 1–3600 | **Live** | …and stays there this long. Long enough to reject headlights, short enough that "light on → colour" is about `probe_confirm_s + probe_settle_s`. |
| `daynight.probe_settle_s` | int | 8 | 1–600 | **Live** | AE settling on the day pipeline before the probe verdict is taken. One verdict, binary: below `day_gain` it sticks, otherwise it reverts immediately (bypassing `transition_s`). |
| `daynight.ref_delay_s` | int | 30 | 0–3600 | **Live** | Wait after entering night before anchoring the night reference (IR LEDs settling). A floor — the anchor also waits for the reading to stop moving. |
| `daynight.heartbeat_s` | int | 14400 (4h) | 300–604800 | **Live** | Probe interval while the scene is moving, independent of any reading. Deliberately flat, never doubling: this is the only bound on how long a wrong night can last. |
| `daynight.heartbeat_max_s` | int | 43200 (12h) | 300–604800 | **Live** | Interval once the scene has demonstrably not moved since the last probe (nothing new to spend a click on) — and the hard ceiling on the deferral. Only applied while the spontaneous trigger can actually see. |
| `daynight.boot_probe` | int | 1 | 0/1 | **Live** | `1` = one probe at boot when the persisted mode is night, turning a guess into a measurement. `0` = leave it to the first heartbeat. A persisted *day* never costs a click either way. |
| `daynight.boot_settle_s` | int | 5 | 0–600 | **Live** | Minimum wait after thread start/re-enable before the first decision (also gated on the reading having settled). |
| `daynight.learn` | int | 0 | 0/1 | **Live** | `1` = let the median of the last 8 confirmed-day minima raise `day_gain` when the configured value is unreachable for this scene, and persist it to `state_path`. It can only ever *raise* the threshold and is clamped below `night_gain/2`. With `0` the numbers are still collected and logged once a day. |
| `daynight.interval_ms` | int | 2000 | 100–60000 | **Live** | Sample interval. 2 s, not the pre-2026-08-17 500 ms: the exposure index needs the `/proc` scrape every tick and no confirmation window is shorter than 8 s. |
| `daynight.transition_s` | int | 5 | 0–3600 | **Live** | Minimum dwell between switches. A failed probe's revert bypasses it. |
| `daynight.diagnose_thresholds` | int | 0 | 0/1 | **Live** | Warn once a day when no probe has ever confirmed day and the best day-pipeline reading is still clear of `day_gain`. |
| `daynight.switch_cmd` | string(64) | `daynight` | — | File-only, not GET-readable | Board script run as `<cmd> day\|night` on a switch. |
| `daynight.isp_path` | string(128) | `/proc/jz/isp/isp-m0` | — | File-only, not GET-readable | ISP exposure proc file (gain **and** integration time). |
| `daynight.trace_path` | string(128) | `""` | — | File-only, not GET-readable | Opt-in decision-trace CSV for the replay harness. **tmpfs only**; 1 MB cap, rotated once to `<path>.1`. |
| `daynight.state_path` | string(128) | `/etc/timps-daynight.state` | — | File-only, not GET-readable | Where `learn=1` persists the learned day levels. Written only on change and at most hourly. |

## `general.*` — daemon-wide settings

File-only; no `/control` POST path.

| Key | Type | Default | Range | Live? | Description |
| --- | --- | --- | --- | --- | --- |
| `general.loglevel` | int | `LOG_INFO` (2) | SDK log-level enum | File-only | Startup log verbosity (`-v` on the command line also forces debug). |
| `general.imp_polling_timeout` | int | 500 | — | File-only | `IMP_Encoder_PollingStream` timeout, ms. |
| `general.osd_pool_size` | int | 1024 | — | File-only | Max OSD pool size (KB) reserved for small OSD regions. |
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
| `video<N>.max_gop` | int | 60 / 60 | 1–1000 | Restart-only | **RESERVED / no effect** — parsed, clamped and persisted for compatibility but consumed by no HAL; the encoder's keyframe interval comes from `video<N>.gop`. Setting a non-zero value logs a one-shot warning. |
| `video<N>.profile` | int | 2 / 2 | 0 (baseline) – 2 (high) | Restart-only | H.264/H.265 encode profile. |
| `video<N>.qp` | int | 35 / 35 | 1–51 | Restart-only | **RESERVED / no effect** — parsed, clamped and persisted for compatibility but consumed by no HAL (no init/fixed-QP wiring; the pre-T31 attribute path is CBR-only). Use `video<N>.min_qp`/`max_qp` + `video<N>.rc_mode` for quality control. Setting a non-zero value logs a one-shot warning. |
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
