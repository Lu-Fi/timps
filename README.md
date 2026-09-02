<div align="center">

# timps

**Tiny IMP Streamer** — a minimal, dependency-light RTSP / fragmented-MP4 / MJPEG
streamer for Ingenic SoC IP cameras.

Built straight on the vendor **libimp** — no live555, libconfig, libwebsockets
or libschrift. A lightweight alternative to prudynt / raptor for the
[thingino](https://github.com/themactep/thingino-firmware) ecosystem.

</div>

---

## Documentation

This README is a quick-start entry point. For the full reference —
architecture, every config key, the complete `/control` API, per-SoC
capability matrix, testing — see the **[wiki](docs/wiki/Home.md)**:

| Page | Covers |
| --- | --- |
| [Architecture](docs/wiki/Architecture.md) | Process/thread model, HAL abstraction, hub pub/sub, data flow |
| [Building](docs/wiki/Building.md) | `make sim`, cross-compiling, every `USE_*` flag, thingino packaging |
| [Configuration Reference](docs/wiki/Configuration-Reference.md) | Every `timps.conf` key, defaults, live-vs-restart-only |
| [HTTP /control API](docs/wiki/HTTP-Control-API.md) | `/control` JSON API, auth, `caps` object, `/events` SSE |
| [Streaming Protocols](docs/wiki/Streaming-Protocols.md) | RTSP, fMP4, MJPEG, snapshot, SRT |
| [Motion Detection](docs/wiki/Motion-Detection.md) | The IVS grid model, sensitivity, the `on_motion` hook |
| [Recording & Timelapse](docs/wiki/Recording-Timelapse.md) | SD recording (pre/post-roll, segments) and periodic JPEG timelapse |
| [Day/Night](docs/wiki/Day-Night.md) | Automatic day/night switching and decision modes |
| [Audio](docs/wiki/Audio.md) | Capture codecs, two-way backchannel, system-sound play queue |
| [Platform & SDK Support](docs/wiki/Platform-SDK-Support.md) | Per-SoC capability matrix |
| [Testing / QA](docs/wiki/Testing-QA.md) | `scripts/timps-qa.sh` |

Other docs worth knowing about: [`docs/rotation.md`](docs/rotation.md) (full
rotation deep-dive), [`docs/sdk-feature-gaps.md`](docs/sdk-feature-gaps.md),
[`docs/backchannel.md`](docs/backchannel.md),
[`docs/camera-fleet.md`](docs/camera-fleet.md).

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
- **Local recording & timelapse** — continuous or motion-triggered fragmented-MP4 segments to SD (pre/post-roll, free-space pruning), plus periodic JPEG timelapse capture
- **Privacy masks** — solid cover rectangles per stream, live-adjustable
- **Image rotation** — hardware 90/270 on T31/T40/T41, software 90/270 on T23, plus genuine per-channel 180° on T40/T41 (I2D); on other SoCs a 180° flip is `image.hflip`+`image.vflip`
- **Optional HTTPS/RTSPS (mbedTLS) and MPEG-TS/SRT output**
- **Authentication** — RTSP Digest, HTTP Digest/Basic (own MD5) + a `/control` token (per-boot + optional remote secret) with CORS
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
| `http://<ip>:8880/events` | SSE push stream: `motion` / `daynight` / `stats` events (`USE_CONTROL` builds — see [HTTP /control API](docs/wiki/HTTP-Control-API.md)) |
| `wss://<ip>:8880/talk` | WebSocket audio backchannel: browser microphone → camera speaker (`USE_BC_WS` builds, TLS only — see [Talk](#talk-browser-microphone--camera-speaker-talk)) |

See [Streaming Protocols](docs/wiki/Streaming-Protocols.md) for transport
details, codec negotiation and client-compatibility notes.

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
/ `timps.conf.example`, or the full [Configuration Reference](docs/wiki/Configuration-Reference.md)
for every key, its default and whether it applies live or needs a restart.
Highlights:

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
# {hostname} {ip} {mac} {fps} {fpsN} {bitrate} {bitrateN} {uptime} {net} {cpu}
# {mem} {clients} + strftime
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
`libaudioProcess` to match your SDK). See [Building](docs/wiki/Building.md)
for the thingino-firmware packaging integration and per-platform header
paths.

### Optional build flags

| Flag | Effect |
| --- | --- |
| `USE_FAAC=1` | software AAC audio via `libfaac` (browser + RTSP sound) |
| `USE_CONTROL` | `/control` endpoint: live settings + persistence. **On by default**; `USE_CONTROL=0` to leave it out |
| `USE_DAYNIGHT` | native automatic day/night detection. **On by default**; `USE_DAYNIGHT=0` to leave it out |
| `USE_RECORD` | local SD recording (fMP4 segments + `/control` clips). **On by default**; `USE_RECORD=0` to leave it out (saves ~11 KB) |
| `USE_TIMELAPSE` | native timelapse (periodic JPEG shots to SD). **On by default**; `USE_TIMELAPSE=0` to leave it out (saves ~4 KB) |
| `USE_BACKCHANNEL` | ONVIF audio backchannel (client → speaker), native `IMP_AO`. Off by default |
| `USE_BC_AAC` | also accept AAC on the backchannel (needs `libhelix-aac`); G.711 always works without it |
| `USE_BC_WS` | also serve the backchannel to a browser over a WebSocket (`/talk`, G.711 mu-law). Off by default; implies `USE_BACKCHANNEL`+`USE_CONTROL`, requires `USE_TLS` |
| `USE_PLAY` | system-sound play queue (`/usr/sbin/play` protocol via a FIFO), native `IMP_AO`. Off by default |
| `USE_PLAY_OPUS` | also decode Ogg-Opus in the play queue (needs `opusfile`); WAV/PCM/G.711 always work without it |
| `USE_ROTATE` | image rotation (`videoN.rotation = 0\|90\|270`, plus `180` on T40/T41). Off by default |
| `USE_SW_ROTATE` | software 90/270 rotation on SoCs without a hardware path (T23); needs `USE_ROTATE` |
| `USE_TLS` | HTTPS (`http.https`) + RTSPS (`rtsp.tls`) via mbedTLS. Auto-enabled when `libmbedtls` is linked |
| `USE_SRT` | MPEG-TS over SRT output (listener mode). Auto-enabled when `libsrt` is linked |

Full details, defaults and rationale for each flag: [Building](docs/wiki/Building.md).

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

## Live control API

On by default (`USE_CONTROL=1`; build with `USE_CONTROL=0` to leave it out
entirely). `POST /control` takes a nested JSON blob covering image tuning,
audio, OSD, motion, day/night, privacy masks, recording and more — every
recognized setting is applied live via IMP where possible and written back to
the config file (`/etc/timps.conf`, only the changed keys, atomic tmp+rename).
`GET /control` returns the current values plus a `caps` object describing
exactly what this build/SoC supports, so a UI can grey out the rest.

```sh
curl -X POST http://127.0.0.1:8880/control -d '{
  "image": {"brightness":140,"contrast":128,"hflip":0,"vflip":0},
  "audio": {"volume":90,"gain":30},
  "video": {"0":{"bitrate":3500}}
}'
curl http://127.0.0.1:8880/control        # read back the current values
```

`/control` access is granted to requests from localhost, a valid **token**
(`X-Timps-Token` header or `?token=`), or HTTP **Basic/Digest** credentials;
the same rules gate `GET /events` and viewing the HTTP media endpoints. Full
schema, the `caps` object, authentication details and the `/events` SSE
stream: **[HTTP /control API](docs/wiki/HTTP-Control-API.md)**.

### Motion detection

A `motion.cols` × `motion.rows` grid of IMP_IVS move-ROIs over the
`motion.monitor_stream` frame, each cell reporting motion independently for a
live preview overlay; `motion.on_motion` runs (rate-limited) when any cell
trips. See [Motion Detection](docs/wiki/Motion-Detection.md) for the grid
model, sensitivity mapping and per-SDK cell limits.

### Automatic day/night

A thread samples the Ingenic ISP exposure state, replacing thingino's separate
`daynightd` daemon. The two directions are not symmetric, because the two
optical paths are not equally trustworthy: with the IR-cut closed the exposure
is an honest measure of ambient light, so day→night is a plain measurement,
while in night the camera is partly measuring its own illuminator, so
night→day is only ever decided by physically probing the day pipeline. A
calendar (fixed window or real sunrise/sunset for a lat/long) is optional and
only schedules those probes — it never decides, which is what keeps basements
and artificially lit rooms working. `daynight.mode=schedule` hands the
decision to the calendar outright. See [Day/Night](docs/wiki/Day-Night.md).

### Speaker: backchannel + system-sound play

`src/rtsp/speaker.c` is the sole owner of `IMP_AO` and arbitrates two
independent producers — no external audio daemon (`/bin/iac`) needed: an
ONVIF-style audio **backchannel** (`USE_BACKCHANNEL`, RTSP client → speaker,
PCMU/PCMA or AAC) and a system-sound **play queue** (`USE_PLAY`, the same FIFO
protocol prudynt/raptor's `/usr/sbin/play` wrapper speaks — WAV, raw PCM16,
G.711 and optionally Ogg-Opus). See [Audio](docs/wiki/Audio.md) for the full
protocol and capability details.

### Talk: browser microphone → camera speaker (`/talk`)

A browser cannot speak RTSP/RTP, so the backchannel above is unreachable from
a web page. `USE_BC_WS` (`BR2_PACKAGE_TIMPS_BC_WS`, off by default) adds
`wss://<ip>:8880/talk`, an RFC 6455 WebSocket that feeds the *same* speaker
path and the same single-talker arbitration: the client sends 20 ms **binary**
frames of G.711 mu-law, timps decodes them and hands the PCM to
`bc_feed_pcm()`. No new library — the framing rides the connection the HTTP
server has already TLS-terminated.

A client opens `/talk?token=<token>&rate=<hz>` and starts sending. `rate` is
the capture rate it actually got (`8000` — mu-law's native rate — if omitted;
`16000`/`24000`/`32000`/`44100`/`48000` are also accepted, because a browser
`AudioContext` may impose the hardware rate instead of the requested one;
anything else is refused with 400). timps resamples to the AO rate. The server
replies with one hello text frame, `{"ok":1,"codec":"pcmu","rate":N}`, then
only ever sends PING/CLOSE. It closes with 1008 if another talker (an ONVIF
client or NVR) holds the speaker, and with 1001 after 10 s of silence — it
PINGs every 3 s, so a merely quiet client keeps the session up. Frames that
would run the accepted audio more than 400 ms ahead of real time are dropped
rather than queued, since TCP will not drop them for you and the delay would
never come back. `scripts/talk-ws.py` speaks all of this without a browser.

Access is the same gate as `/events`: localhost, a valid **token**, or
Basic/Digest credentials — as `?token=`, because a `WebSocket` constructor
cannot set request headers. A WebSocket upgrade is not covered by CORS, so
`/talk` additionally requires a present `Origin` to have the same host as
`Host` (an absent one is a non-browser client, which still had to present a
token; `null` is refused).

`USE_BC_WS` implies `USE_BACKCHANNEL` + `USE_CONTROL` (the token machinery
lives there). `USE_TLS` is **recommended but optional** — `ws.c` terminates
nothing itself, so the plain-`ws://` path links without mbedTLS. Enable it per
camera with `audio.talk_ws` (restart-required, default 0):

| `audio.talk_ws` | `/talk` |
| --- | --- |
| `0` | not served |
| `1` | served over TLS only — a plaintext port answers 426. The package default on TLS builds; unsatisfiable (and warned about at startup) without TLS |
| `2` | served over `wss://` when the port is TLS, **and** over plain `ws://` when it is not |

`2` is a deliberate hand edit, never preset: the microphone audio and its
token then cross the network in the clear. The real constraint it works around
is the browser, not the camera — `getUserMedia()` is refused outside a secure
context, so `2` is only useful to someone who has granted the origin one
(`chrome://flags/#unsafely-treat-insecure-origin-as-secure`, a trusted tunnel,
localhost). A microphone-to-speaker path stays opt-in at build time and again
at run time. `GET /control` reports the resolved answer as
`caps.backchannel.talk_ws` (`0`/`1`/`2`, where `1` with a plaintext port
resolves to `0`), so the WebUI needs no second test.

### Recording, timelapse & privacy masks

`record.*` + `/control` records one video stream (+ AAC audio) to fragmented
MP4 on SD, continuous or motion-triggered, with pre/post-roll and free-space
pruning; `timelapse.*` independently captures periodic JPEG shots. Privacy
masks (`privacy<S>.<N>.*`) overlay solid cover rectangles per stream, live
by default (as long as OSD or a privacy region was on at startup). Details:
[Recording & Timelapse](docs/wiki/Recording-Timelapse.md).

### Image rotation

`videoN.rotation = 0|90|270` (plus `180` on T40/T41), `USE_ROTATE` (off by
default, restart-required). Hardware 90/270 on T31/T40/T41; software 90/270
(`USE_SW_ROTATE`) on T23 at the cost of CPU and H.264-only encoding.
`rotation=180` is a genuine per-channel capability **only on T40/T41** (I2D);
on every other SoC it's redundant with a global ISP flip and is coerced to 0
there — use `image.hflip=1` + `image.vflip=1` instead. Full per-SoC matrix,
direction conventions and constraints: [`docs/rotation.md`](docs/rotation.md)
and [Platform & SDK Support](docs/wiki/Platform-SDK-Support.md).

### HTTPS / RTSPS (mbedTLS) and SRT output

`USE_TLS=1` (auto-enabled when `libmbedtls` is linked) lets the HTTP server
run **HTTPS** and RTSP run **RTSPS**, the same code path serving plain
HTTP/RTSP byte-for-byte when TLS is off. `USE_SRT=1` (auto-enabled when
`libsrt` is linked) adds MPEG-TS over SRT in listener mode, played back with
e.g. `ffplay srt://<ip>:9000`. See [Streaming Protocols](docs/wiki/Streaming-Protocols.md).

## Security

Authentication is off while `rtsp.user` / `http.user` are empty. Once set,
RTSP requires **Digest** auth and HTTP accepts either **Digest** or **Basic**
(client's choice; the 401 offers Digest first). Local (loopback) requests
skip auth so an on-device web UI always works. HTTP Basic is base64, not
encrypted — put a TLS reverse proxy in front for internet exposure, or build
with `USE_TLS`. `/control`, `/events` and the HTTP media endpoints
additionally accept a token (`http.token` / per-boot `http.token_file`); the
token grants viewing + `/control` + `/events`, still not RTSP.

## Project layout

```
src/
  main.c            startup, args, signals
  config.*          key=value parser
  frame.* fanqueue.*  ref-counted packets + bounded fan-out queue
  hub.*             one publisher (HAL) → many sinks, on-demand activation
  md5.* auth.*      MD5 + RTSP-Digest / HTTP-Digest/Basic auth
  codec/            NAL iteration, SPS/PPS→avcC/hvcC, ADTS/ASC, G.711
  rtsp/             RTP packetisation + RTSP server (own implementation)
  mp4/              fMP4 muxer + HTTP server (preview, JPEG, MJPEG)
  hal/              backend interface, Ingenic backend, host-sim backend,
                    IMP_OSD array, IMP_IVS motion, TrueType rasteriser
```

See [Architecture](docs/wiki/Architecture.md) for the full process/thread
model and data flow.

## License

MIT
