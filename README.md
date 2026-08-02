<div align="center">

# timps

**Tiny IMP Streamer** — a minimal, dependency-light RTSP / fragmented-MP4 / MJPEG
streamer for Ingenic SoC IP cameras.

Built straight on the vendor **libimp** — no live555, libconfig, libwebsockets
or libschrift. A lightweight alternative to prudynt / raptor for the
[thingino](https://github.com/themactep/thingino-firmware) ecosystem.

</div>

---

## Features

- **Pure C**, only `libimp` + pthread (optional `libfaac` for AAC) — no heavyweight dependencies
- **RTSP** — H.264 / H.265 video, AAC / G.711 audio
- **Browser preview** — fragmented-MP4 over MSE, plus JPEG snapshot & MJPEG
- **On-demand encoding** — a stream is only encoded while a client is watching (idle ≈ 0 % CPU)
- **Live control API** — `POST`/`GET /control`: change ISP image, audio, OSD, encoder/sensor & motion live and persist them; per-SoC capability reporting (`caps.*`)
- **Event push stream** — `GET /events` (SSE): subscribe to `motion` / `daynight` / `stats` instead of polling
- **Per-stream TrueType OSD** — independent overlay set per stream, placeholders (time, hostname, uptime, fps…), optional text outline/stroke
- **Grid motion detection** — configurable `IMP_IVS` ROI grid with live per-cell state for a preview overlay
- **Automatic day/night** — native ISP-brightness detection (replaces thingino's `daynightd`), or force by a fixed time window or real sunrise/sunset for a lat/long
- **Native speaker output** — timps owns `IMP_AO` directly: ONVIF audio backchannel (two-way audio) and a system-sound play queue (WAV/Opus/PCM/G.711), no `/bin/iac` dependency
- **Local recording** — continuous or motion-triggered fragmented-MP4 segments to SD, with pre/post-roll and free-space pruning
- **Privacy masks** — solid cover rectangles per stream, live-adjustable
- **Image rotation** — hardware 90/270 on T31/T40/T41, software 90/270 on T23, plus genuine per-channel 180° on T40/T41 (I2D); on other SoCs a 180° flip is `image.hflip`+`image.vflip`
- **Optional HTTPS/RTSPS (mbedTLS) and MPEG-TS/SRT output**
- **Authentication** — RTSP Digest / HTTP Basic (own MD5) + a `/control` token (per-boot + optional remote secret) with CORS
- **Logging** — leveled logger to stderr and syslog (visible in `logread`)
- **Tiny footprint** — small enough for a T10

**Supported SoCs:** T10 · T20 · T21 · T23 · T30 · T31 · T40 · T41 · C100

## Streams & endpoints

| URL | Description |
| --- | --- |
| `rtsp://<ip>:554/ch0` | main stream (video + audio) |
| `rtsp://<ip>:554/ch1` | sub stream |
| `http://<ip>:8880/` | browser preview (fMP4 via MSE, with audio) |
| `http://<ip>:8880/stream.mp4` | fragmented MP4 (ffplay / VLC) |
| `http://<ip>:8880/snapshot.jpg` | latest JPEG frame |
| `http://<ip>:8880/stream.mjpeg` | MJPEG (multipart) |
| `…?chn=N` | JPEG / MJPEG at the resolution of `videoN` (needs `videoN.jpeg = true`) |
| `http://<ip>:8880/events` | SSE push stream: `motion` / `daynight` / `stats` events (`USE_CONTROL` builds, see below) |

## Quick start (thingino)

`timps` ships as a thingino package. In your firmware tree:

```sh
make menuconfig      # Streamer Packages → Streamer → timps  (deselect prudynt/raptor)
make                 # build the firmware, flash as usual
```

On the camera, copy `timps.conf.example` to `/etc/timps.conf`, adjust the
`sensor.*` block for your board, and start `timpsd -c /etc/timps.conf`.

For rapid iteration against a live camera (SSH key installed):

```sh
scripts/deploy.sh --build     # rebuild the timps package, push timpsd + config, run it live
```

## Configuration

Everything is a flat `key = value` file — see [`scripts/camera.conf`](scripts/camera.conf)
/ `timps.conf.example`. Highlights:

```ini
sensor.model    = sc4336p
sensor.i2c_addr = 0x30
sensor.width    = 2560
sensor.height   = 1440

video0.codec = h264      # main:  1080p
video1.codec = h264      # sub:   360p
audio.codec  = aac       # needs timps built with libfaac; else pcmu (G.711u)

# OSD overlays: an independent set per video stream. Keys osd<S>.<N>.<field>
# (stream S, item N); legacy osd<N>.<field> keys still load and apply to
# every stream. x/y: 0 = centered, >0 = from left/top, <0 = from right/
# bottom. Optional text outline: outline = <px>, outline_color = 0xAARRGGBB.
# Placeholders:
# {hostname} {ip} {mac} {fps} {uptime} {net} {cpu} {mem} {clients} + strftime
osd0.0.text = %Y-%m-%d %H:%M:%S    # stream 0, item 0
osd1.0.text = %H:%M                # stream 1 has its own items

# rtsp.user / rtsp.pass  → enable auth (empty = open)
```

## Building from source

### Cross-compile for the camera

```sh
git clone --recurse-submodules https://github.com/Lu-Fi/timps
cd timps
make PLATFORM=T31 CROSS_COMPILE=mipsel-linux-
make strip PLATFORM=T31 CROSS_COMPILE=mipsel-linux-
```

`PLATFORM` ∈ `T10 T20 T21 T23 T30 T31 T40 T41 C100`. Result: a single `timpsd`
binary. Vendor libs are linked via `IMPLIBS` (default static
`-l:libimp.a -l:libalog.a -l:libsysutils.a`; add `libmuslshim` /
`libaudioProcess` to match your SDK).

### Optional build flags

| Flag | Effect |
| --- | --- |
| `USE_FAAC=1` | software AAC audio via `libfaac` (browser + RTSP sound) |
| `USE_CONTROL` | `/control` endpoint: live settings + persistence (see below). **On by default**; `USE_CONTROL=0` to leave it out |
| `USE_DAYNIGHT` | native automatic day/night detection (see below). **On by default**; `USE_DAYNIGHT=0` to leave it out |
| `USE_BACKCHANNEL` | ONVIF audio backchannel (client → speaker), native `IMP_AO`. Off by default |
| `USE_BC_AAC` | also accept AAC on the backchannel (needs `libhelix-aac`); G.711 always works without it |
| `USE_PLAY` | system-sound play queue (`/usr/sbin/play` protocol via a FIFO), native `IMP_AO`. Off by default |
| `USE_PLAY_OPUS` | also decode Ogg-Opus in the play queue (needs `opusfile`); WAV/PCM/G.711 always work without it |
| `USE_ROTATE` | image rotation (`videoN.rotation = 0\|90\|270`, plus `180` on T40/T41, see below). Off by default |
| `USE_SW_ROTATE` | software 90/270 rotation on SoCs without a hardware path (T23); needs `USE_ROTATE` |
| `USE_TLS` | HTTPS (`http.https`) + RTSPS (`rtsp.tls`) via mbedTLS. Auto-enabled when `libmbedtls` is linked |
| `USE_SRT` | MPEG-TS over SRT output (listener mode). Auto-enabled when `libsrt` is linked |

## Live control API

On by default (`USE_CONTROL=1`); build with `USE_CONTROL=0` to leave it out
entirely. `POST /control` takes a nested JSON blob; every recognized setting is
**applied live via IMP and written back to the config file**
(`/etc/timps.conf` — only the changed keys, comments/order preserved, atomic
tmp+rename). `GET /control` returns the current values as JSON.

`/control` access is granted to **any one** of: requests from localhost
(on-device web UI / bridge CGIs), a valid **token**, or HTTP **Basic**
credentials (when configured); otherwise `401`/`403`. Tokens are sent as an
`X-Timps-Token: <token>` header (preferred) or `?token=<token>` query
parameter (may show up in access logs; fine on a LAN). The same token also
unlocks `/events` and **viewing** the HTTP media endpoints — `/stream.mp4`,
`/stream.mjpeg`, `/snapshot.jpg` (incl. their `?chn=N` forms) accept
`?token=<token>` so an `<img>`/`<video src>` can use it — but never RTSP.
Two tokens are valid: a random **per-boot** token published to
`http.token_file` (default `/run/timps.token`, mode 0640, `""` disables) for
local privileged helpers like the thingino WebUI, and the optional persistent
`http.token` secret for remote automation — the configured secret is never
written to the token file. `/control` also answers CORS: an `OPTIONS`
preflight returns `204` and responses reflect the request's `Origin` (plus
`Vary: Origin`, allow-listing the `X-Timps-Token` header), so a browser page
served from another port (e.g. the WebUI on `:80`) can call `:8880/control`
directly. Reflecting the Origin is safe because a foreign origin cannot read
the token file or the token; no `Allow-Credentials` is ever sent.

```sh
curl -X POST http://127.0.0.1:8880/control -d '{
  "image": {"brightness":140,"contrast":128,"saturation":128,"sharpness":128,
            "hue":128,"hflip":0,"vflip":0,"running_mode":1},
  "audio": {"volume":90,"gain":30},
  "osd":   {"enabled":1},
  "osd0":  {"0":{"enabled":1,"text":"%Y-%m-%d %H:%M:%S","x":10,"y":10,
                 "font_size":32,"color":"0xFFFFFFFF",
                 "outline":1,"outline_color":"0xFF000000"},
            "3":{"enabled":0}},
  "osd1":  {"0":{"text":"sub cam"}},
  "video": {"0":{"bitrate":3500},"1":{"bitrate":600}}
}'
curl http://127.0.0.1:8880/control        # read back the current values
```

Schema overview (all fields optional, unknown keys ignored):

| Section | Keys | Live effect |
| --- | --- | --- |
| `image` | `brightness contrast saturation sharpness hue hflip vflip running_mode anti_flicker ae_compensation max_again max_dgain sinter_strength temper_strength dpc_strength defog_strength drc_strength highlight_depress backlight_compensation core_wb_mode wb_rgain wb_bgain` | immediate via the matching `IMP_ISP_Tuning_*` call; **per-SoC** — `caps.image` lists what this chip supports (e.g. `hue`/WDR/defog/WB only on some SoCs), unsupported keys still persist |
| `audio` (live) | `volume gain alc_gain high_pass agc agc_target_dbfs agc_compression_db ns mute spk_volume spk_gain` | immediate; `caps.audio` lists support (`alc_gain` only T21/T31/C100; `spk_volume`/`spk_gain` gated on an `IMP_AO` pipeline being built in). `mute` = live mic mute; speaker keys apply to whichever producer currently holds `IMP_AO` and persist as the default for the next one |
| `audio` (persist) | `codec samplerate bitrate channels backchannel backchannel_codec backchannel_rate spk_enabled force_stereo` | persisted only — applies on restart |
| `speaker` | `play` (filename), `stop` (`1`) | not persisted — a transient action enqueued on the play FIFO (`USE_PLAY`); `play` is validated against `/usr/share/sounds` (no `/` or `..`) |
| `osdS.N` (`osd0`/`osd1` objects, items 0–7) | `enabled text x y font_size color transparency outline outline_color` | immediate for items that had a region at startup; *enabling* an item that started disabled only persists (applies after restart). Every video stream has its **own independent item set** (`osd0` = stream 0, `osd1` = stream 1) |
| `osd.N` (legacy shared form, items 0–7) | same leaf keys | still parsed; the item is mirrored onto **every** stream (pre-per-stream behavior) |
| `osd` | `enabled` (master switch, global for all streams) | persisted only — the OSD groups are built once at startup |
| `video.N` | `enabled codec width height fps bitrate rc_mode gop max_gop profile qp min_qp max_qp rotation buffers rtsp_path` | persisted only — applies on restart (encoder/FrameSource are never reconfigured live) |
| `sensor` | `model i2c_addr fps width height` | persisted only — applies on restart (sensor is probed at ISP init) |
| `daynight` | `enabled mode time_night_start time_day_start sun_latitude sun_longitude sun_sunrise_offset_min sun_sunset_offset_min` | immediate — toggles/reconfigures the automatic day/night detection, including the `time`/`sun` override modes (see below) |
| `motion` | `enabled sensitivity cols rows monitor_stream` | immediate — the IMP_IVS grid is cleanly stopped and recreated (`cooldown_ms`/`on_motion` are config-file only) |
| `privacy.S.N` | `enabled x y w h color` | immediate — cover region created/shown/hidden/moved live (as long as OSD or a privacy region was on at startup) |
| `record` | `mode segment_s pre_roll_s post_roll_s min_free_mb dir` + `{"active":1\|0}` | `active` is an immediate manual start/stop override; the rest applies on the next recording cycle |

`GET /control` reports a `"caps"` object so a UI can present exactly what this
build/SoC supports: `caps.image` / `caps.audio` (live-capable leaf keys for this
chip — grey out the rest), `caps.osd` (accepted OSD item leaf keys),
`caps.motion` (`{available, max_cells}`), `caps.backchannel`/`caps.play`
(speaker features, see *Speaker* below), `caps.privacy`
(`{available, max_regions}`), `caps.record` and `caps.restart`
(`["video","sensor"]` — the persist-only sections, so clients can prompt for
a restart). The OSD
dump carries the global master switch as `"osd":{"enabled":..}` followed by
one full item set per stream (`"osd0"`, `"osd1"`), each item incl. its `type`
(`text`/`logo`) so UIs can tell text overlays from the logo.

The legacy flat form (`{"brightness":140,"running_mode":1}` /
`{"force_mode":"night"}`) still works and maps to `image.*`.

### Motion detection (grid)

Motion detection is a `motion.cols` × `motion.rows` **grid** of IMP_IVS
move-ROIs split evenly over the `motion.monitor_stream` frame; each cell
reports motion on its own. `cols*rows` is clamped to the SDK's compile-time
`IMP_IVS_MOVE_MAX_ROI_CNT` (52 on most SDKs, 4 on the old T10/T20 3.9.0 SDK
— see `src/motion_caps.h`), reported as
`"caps":{"motion":{"available":0|1,"max_cells":N}}`. `GET /control` exposes
the live state for UI overlays:

```json
"motion":{"available":1,"enabled":1,"cols":5,"rows":5,"max_cells":52,
          "sensitivity":128,"monitor_stream":0,
          "active":[0,0,1,0,...],"last_ms":3200}
```

`active` is per-cell 0/1, row-major (`index = row*cols + col`, length
`cols*rows`); `last_ms` is the time since the last motion event (-1 = never).
The UI sensitivity 0..255 maps to IMP's 0..4 range. When ANY cell trips,
`motion.on_motion` runs (rate-limited by `motion.cooldown_ms`). On SoCs/SDKs
without the IMP_IVS move API the feature reports `available:0` and all
motion calls are no-ops.

### Event push stream (`GET /events`)

Instead of polling `GET /control`, clients can **subscribe**: `GET /events`
is a long-lived `text/event-stream` (Server-Sent Events) that pushes JSON
state the moment it changes. Ships with `USE_CONTROL` (like `/control`);
`events.enabled = 0` turns the endpoint off at runtime. Event types:

| `event:` | pushed when | `data:` payload |
| --- | --- | --- |
| `motion` | the active-grid / enabled / geometry / sensitivity changed | the `/control` `"motion"` object (identical shape) |
| `daynight` | mode flipped, brightness moved ≥ 1 % or total gain ≥ 5 % | the `/control` `"daynight"` object (identical shape) |
| `stats` | every `events.stats_ms` (default 2000, `0` = off) | `{"uptime_s":N,"clients":N,"video":[{"chn":0,"subs":N,"fps":F},…]}` |

`?stream=motion,daynight,stats` selects the wanted types (default: all).
On connect the server sends `retry: 3000` (EventSource reconnect delay), a
`: connected` comment and the full current state once; afterwards each
connection deduplicates against what *it* last sent, so idle scenes cost
nothing but a `: ping` keepalive every ~12 s. Internally the producers
(IVS result thread, day/night sampler, `/control` writes) wake the
subscribers via a shared condition variable — push latency is the producer's
own sampling rate, no HTTP polling anywhere.

Access rules are exactly `/control`'s: localhost, a valid token, or Basic
credentials; `OPTIONS` preflight and CORS `Origin` reflection included.
Browsers use the query form `…/events?stream=motion&token=<tok>` because
`EventSource` **cannot send custom headers** — a query token can end up in
access logs, which is accepted on a LAN (the token only unlocks
`/control`/`/events`, never the streams; put TLS in front on the internet).
`events.max_clients` (default 8) caps concurrent subscriber connections
(each parks one HTTP thread) — beyond it `/events` answers `503`.

```sh
curl -N http://127.0.0.1:8880/events                  # everything
curl -N "http://127.0.0.1:8880/events?stream=motion&token=$(cat /run/timps.token)"
```

The thingino WebUI preview overlay subscribes to `?stream=motion` and falls
back to 4 Hz `/control` polling when `/events` is unavailable.

## Automatic day/night

Built in with `USE_DAYNIGHT=1` (default): a small thread samples the Ingenic
ISP exposure state (integration time + analog/ISP digital gain from
`/proc/jz/isp/isp-m0`), derives a scene brightness in percent — the exact
formula, thresholds and hysteresis of thingino's `daynightd` daemon — and on a
change runs the board switch script (`daynight day|night` on thingino, which
toggles the IR-cut filter, IR LEDs and the `color` hook; `color` then sets
timps's `image.running_mode` back through `/control`). timps never sets the
ISP mode directly, so the whole board stays in sync. On thingino the timps
package **disables the separate `daynightd` daemon** — timps is the detector.

Rules: in day mode it switches to night when the (10-sample smoothed)
brightness drops below `threshold_low`; in night mode to day above
`threshold_high`; the very first decision uses a band narrowed by
`(high-low)*hysteresis` on both sides; `transition_s` is the minimum dwell
between switches. If the ISP file is absent (host sim), the thread idles.

| Key | Default | Meaning |
| --- | --- | --- |
| `daynight.enabled` | `1` | auto detection on/off (runtime-toggleable via `/control`: `{"daynight":{"enabled":false}}` = manual mode) |
| `daynight.mode` | `sensor` | `sensor` (below) / `time` / `sun` — see **Override modes** |
| `daynight.threshold_low` | `25` | % — below this (in day) → night |
| `daynight.threshold_high` | `75` | % — above this (in night) → day |
| `daynight.hysteresis` | `0.1` | band factor for the initial decision |
| `daynight.interval_ms` | `500` | sample interval |
| `daynight.transition_s` | `5` | min seconds between switches |
| `daynight.switch_cmd` | `daynight` | run as `<cmd> day\|night` on a switch |
| `daynight.isp_path` | `/proc/jz/isp/isp-m0` | ISP exposure proc file |

### Override modes (`daynight.mode`)

The sensor isn't always the right signal — an IR-blocked lens, a scene
that's naturally dark/bright, or simply wanting the schedule to match a
person's routine instead of the camera's exposure. `daynight.mode` swaps
the decision source entirely; `sensor`/`total_gain`/brightness are still
sampled every cycle either way, so the WebUI's live gain/brightness readout
stays populated, and `daynight.enabled=0` (manual) suppresses forcing in
all three modes the same way.

| Mode | Forces a switch by |
| --- | --- |
| `sensor` (default) | ISP `total_gain`/brightness, as above |
| `time` | the local wall clock against a fixed `[time_night_start .. time_day_start]` window, wrapping past midnight |
| `sun` | today's real sunrise/sunset for `sun_latitude`/`sun_longitude`, each edge shiftable by an offset |

| Key | Default | Meaning |
| --- | --- | --- |
| `daynight.time_night_start` | `` | `"HH:MM"` — switch to night at this local time (`time` mode) |
| `daynight.time_day_start` | `` | `"HH:MM"` — switch to day at this local time (`time` mode) |
| `daynight.sun_latitude` / `sun_longitude` | `0` | decimal degrees (`sun` mode) |
| `daynight.sun_sunrise_offset_min` | `0` | minutes added to computed sunrise before switching to day (negative allowed) |
| `daynight.sun_sunset_offset_min` | `0` | minutes added to computed sunset before switching to night (negative allowed) |

```sh
curl -X POST http://127.0.0.1:8880/control -d '{
  "daynight": {"mode":"sun", "sun_latitude":52.52, "sun_longitude":13.40,
               "sun_sunset_offset_min":-30}
}'
curl http://127.0.0.1:8880/control | jq .daynight
# -> {"mode":"sun", ..., "sun_computed_sunrise":"05:17", "sun_computed_sunset":"21:07"}
```

`GET /control`'s `daynight` object always includes today's computed
`sun_computed_sunrise`/`sun_computed_sunset` (local `"HH:MM"`), so a UI can
sanity-check a configured lat/long before trusting it, even while another
mode is active. Polar latitudes where the sun genuinely doesn't rise/set
that day fall back to permanent day or permanent night for the `sun`
decision, rather than producing an invalid time.

## Speaker: backchannel + system-sound play

`src/rtsp/speaker.c` is the sole owner of `IMP_AO` (the camera speaker) and
arbitrates two independent producers — timps drives the hardware directly,
no external audio daemon (`/bin/iac`) needed:

- **ONVIF audio backchannel** (`USE_BACKCHANNEL=1`, off by default) — an
  RTSP client `SETUP`s track 2 and streams RTP audio (PCMU/PCMA, pure C; AAC
  too with `USE_BC_AAC=1` via `libhelix-aac`) that plays through the
  speaker. Backchannel always **preempts**: it takes the speaker mid-play
  if it has to, since it's the real-time conversational path.
- **System-sound play queue** (`USE_PLAY=1`, off by default) — a FIFO at
  `/run/timps/audio_out` accepts `PLAY url=<path> [vol=N] [gain=N]
  [rate=N] [format=wav|pcm|opus] [loop=N] [delay=ms]` / `STOP` lines, the
  same protocol prudynt/raptor's `/usr/sbin/play` wrapper speaks — so the
  WiFi captive-portal prompt, the post-upgrade chime and the Home Assistant
  ESPHome `media_player`/TTS integration all get a working speaker for
  free once this is built in. Decodes WAV, raw PCM16 and G.711 µ/A-law out
  of the box; add `USE_PLAY_OPUS=1` for Ogg-Opus (`opusfile`) too — that's
  what thingino's own `thingino-sounds` package ships.

| Key | Meaning |
| --- | --- |
| `audio.backchannel` | enable the backchannel (persist-only, restart-required) |
| `audio.backchannel_codec` | `pcmu`/`pcma`/`aac` |
| `audio.backchannel_rate` | 8000–48000 Hz |
| `audio.spk_volume` / `spk_gain` | speaker volume/gain — **live** via `/control`, applied to whichever producer currently holds `IMP_AO` and persisted as the default for the next one |

```sh
# live speaker volume/gain
curl -X POST http://127.0.0.1:8880/control -d '{"audio":{"spk_volume":80,"spk_gain":25}}'

# play a system sound (path validated against /usr/share/sounds - no / or ..)
curl -X POST http://127.0.0.1:8880/control -d '{"speaker":{"play":"chime_1.wav"}}'
curl -X POST http://127.0.0.1:8880/control -d '{"speaker":{"stop":1}}'
```

`GET /control` reports `caps.backchannel = {available}` and
`caps.play = {available, sounds:[...]}` — the latter enumerated live from
`/usr/share/sounds`, listing `.wav` unconditionally (the µ-law/PCM decoder
needs no library) and `.opus` only when `USE_PLAY_OPUS` is actually built,
so a UI never offers a file this exact build can't decode. This is what
drives the thingino WebUI's speaker volume slider and test-sound dropdown.

## Recording, privacy masks, rotation, TLS/SRT

### Local recording (to SD)

`record` section + `/control`: records one video stream (+ AAC audio) to
`<dir>/<hostname>/records/<strftime>.mp4` as fragmented MP4, reusing the
`/stream.mp4` muxer. `record.mode` is `continuous` or `motion` (pre-roll
ring from the keyframe before the trigger, plus `record.post_roll_s` after
the last motion); segments rotate every `record.segment_s` at a keyframe,
oldest files pruned to keep `record.min_free_mb` free. `GET /control`
reports a `record` status object (recording/channel/mode/bytes/free_mb/
file) and `caps.record`; `{"record":{"active":1|0}}` is a manual start/stop
override (the WebUI record button).

### Privacy masks

`privacy<S>.<N>.{enabled,x,y,w,h,color}` (up to `MS_MAX_PRIVACY` per
stream) — solid filled rectangles over sensitive areas, implemented as IMP
OSD cover regions. Applied **live** (create/show/hide/move without a
restart, as long as OSD or a privacy region was on at startup) and
persisted. `GET /control` dumps the `privacy` tree and advertises
`caps.privacy = {available, max_regions}`.

### Image rotation

`videoN.rotation = 0|90|270` (plus `180` on T40/T41) (`USE_ROTATE`, off by
default; restart-required). Hardware 90/270 needs T31/T40/T41 (FrameSource/I2D
rotate); software 90/270 (`USE_SW_ROTATE`) works on SoCs without a hardware path
(T23) at the cost of CPU (per-frame transpose) and H.264-only, software-OSD-only
encoding. **`rotation=180`** is a genuine per-channel capability **only on
T40/T41**, where the hardware I2D block flips just the one requesting stream. On
every other SoC `rotation=180` was removed (there it was only a *global* ISP H+V
flip that also flipped every sibling stream, redundant with `image.hflip`) and
is coerced to 0 — for a 180° flip there use `image.hflip=1` + `image.vflip=1`
(always available, independent of `USE_ROTATE`; note this is a global flip). See
`docs/rotation.md`.
Known limitation on T31: 90/270 can't carry a hardware OSD/privacy overlay
(libimp IPU-OSD stride bug) — see `docs/rotation.md`.

### HTTPS / RTSPS (mbedTLS) and SRT output

`USE_TLS=1` (auto-enabled when `libmbedtls` is linked) lets the HTTP server
run **HTTPS** (`http.https` + `http.tls_cert`/`http.tls_key`) and RTSP run
**RTSPS** (`rtsp.tls`/`rtsp.tls_port`); the same code path serves plain
HTTP/RTSP byte-for-byte when TLS is off. `USE_SRT=1` (auto-enabled when
`libsrt` is linked) adds MPEG-TS over SRT in listener mode
(`srt.enabled`/`port`/`channel`/`latency_ms`/`streamid`/`passphrase`,
played back with e.g. `ffplay srt://<ip>:9000`).

### Host simulation (no hardware)

```sh
make sim                       # builds timpsd-sim with the host cc
./timpsd-sim -c timps.conf     # feeds files instead of the ISP
ffplay http://127.0.0.1:8880/stream.mp4
```

### Ingenic headers (git submodule)

The IMP headers are **not** vendored in this repo — they come from
[gtxaspec/ingenic-headers](https://github.com/gtxaspec/ingenic-headers) as a git
submodule under `include/` (same pattern as prudynt-t). Clone with
`--recurse-submodules`, or run `git submodule update --init` afterwards. The
host-sim build does not need them.

## Security

Authentication is off while `rtsp.user` / `http.user` are empty. Once set, RTSP
requires **Digest** auth (`rtsp://user:pass@ip:554/ch0`) and HTTP requires
**Basic** auth. Local (loopback) requests skip auth so an on-device web UI always
works. HTTP Basic is base64, not encrypted — put a TLS reverse proxy in front for
internet exposure. `/control`, `/events` and the HTTP media endpoints
(`/stream.mp4`, `/stream.mjpeg`, `/snapshot.jpg`) additionally accept a token
(`http.token` / per-boot `http.token_file`, see *Live control API*); the token
grants viewing + `/control` + `/events`, still not RTSP. Tokens are cleartext
on the wire like Basic auth — the same TLS advice applies — and a `?token=` in
the URL can end up in proxy/access logs (fine on a LAN), so prefer the
`X-Timps-Token` header where the client can send headers.

## Project layout

```
src/
  main.c            startup, args, signals
  config.*          key=value parser
  frame.* fanqueue.*  ref-counted packets + bounded fan-out queue
  hub.*             one publisher (HAL) → many sinks, on-demand activation
  md5.* auth.*      MD5 + RTSP-Digest / HTTP-Basic auth
  codec/            NAL iteration, SPS/PPS→avcC/hvcC, ADTS/ASC, G.711
  rtsp/             RTP packetisation + RTSP server (own implementation)
  mp4/              fMP4 muxer + HTTP server (preview, JPEG, MJPEG)
  hal/              backend interface, Ingenic backend, host-sim backend,
                    IMP_OSD array, IMP_IVS motion, TrueType rasteriser
```

## License

MIT
