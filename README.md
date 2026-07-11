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
- **On-demand encoding** — a stream is only encoded while a client is watching
- **TrueType OSD** — with placeholders: time, hostname, uptime, fps, bitrate, viewers…
- **Motion detection** — via `IMP_IVS`
- **Authentication** — RTSP Digest / HTTP Basic (own MD5)
- **Tiny footprint** — small enough for a T10

**Supported SoCs:** T10 · T20 · T21 · T23 · T30 · T31 · T40 · T41 · C100

## Streams & endpoints

| URL | Description |
| --- | --- |
| `rtsp://<ip>:554/ch0` | main stream (video + audio) |
| `rtsp://<ip>:554/ch1` | sub stream |
| `http://<ip>:8080/` | browser preview (fMP4 via MSE, with audio) |
| `http://<ip>:8080/stream.mp4` | fragmented MP4 (ffplay / VLC) |
| `http://<ip>:8080/snapshot.jpg` | latest JPEG frame |
| `http://<ip>:8080/stream.mjpeg` | MJPEG (multipart) |
| `…?chn=N` | JPEG / MJPEG at the resolution of `videoN` (needs `videoN.jpeg = true`) |

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

# OSD overlays (drawn on every stream). x/y: 0 = centered,
# >0 = from left/top, <0 = from right/bottom. Placeholders:
# {hostname} {ip} {mac} {fps} {uptime} {net} {cpu} {mem} {clients} + strftime
osd0.text = %Y-%m-%d %H:%M:%S

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

## Live control API

On by default (`USE_CONTROL=1`); build with `USE_CONTROL=0` to leave it out
entirely. `POST /control` takes a nested JSON blob; every recognized setting is
**applied live via IMP and written back to the config file**
(`/etc/timps.conf` — only the changed keys, comments/order preserved, atomic
tmp+rename). `GET /control` returns the current values as JSON.

Requests from localhost bypass auth (for an on-device web UI); remote access
is only allowed when HTTP/RTSP credentials are configured (Basic auth),
otherwise `403`.

```sh
curl -X POST http://127.0.0.1:8080/control -d '{
  "image": {"brightness":140,"contrast":128,"saturation":128,"sharpness":128,
            "hue":128,"hflip":0,"vflip":0,"running_mode":1},
  "audio": {"volume":90,"gain":30},
  "osd":   {"0":{"enabled":1,"text":"%Y-%m-%d %H:%M:%S","x":10,"y":10,
                 "font_size":32,"color":"0xFFFFFFFF"},
            "3":{"enabled":0}},
  "video": {"0":{"bitrate":3500},"1":{"bitrate":600}}
}'
curl http://127.0.0.1:8080/control        # read back the current values
```

Schema overview (all fields optional, unknown keys ignored):

| Section | Keys | Live effect |
| --- | --- | --- |
| `image` | `brightness contrast saturation sharpness hue hflip vflip running_mode` | immediate (`hue` only on T23/T31/C100) |
| `audio` | `volume gain` | immediate |
| `osd.N` (0–7) | `enabled text x y font_size color transparency` | immediate for items that had a region at startup; *enabling* an item that started disabled only persists (applies after restart) |
| `video.N` | `bitrate` (kbps) | persisted only — applies on restart (no live encoder reconfig) |
| `daynight` | `enabled` | immediate — toggles the automatic day/night detection (see below) |

The legacy flat form (`{"brightness":140,"running_mode":1}` /
`{"force_mode":"night"}`) still works and maps to `image.*`.

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
| `daynight.threshold_low` | `25` | % — below this (in day) → night |
| `daynight.threshold_high` | `75` | % — above this (in night) → day |
| `daynight.hysteresis` | `0.1` | band factor for the initial decision |
| `daynight.interval_ms` | `500` | sample interval |
| `daynight.transition_s` | `5` | min seconds between switches |
| `daynight.switch_cmd` | `daynight` | run as `<cmd> day\|night` on a switch |
| `daynight.isp_path` | `/proc/jz/isp/isp-m0` | ISP exposure proc file |

### Host simulation (no hardware)

```sh
make sim                       # builds timpsd-sim with the host cc
./timpsd-sim -c timps.conf     # feeds files instead of the ISP
ffplay http://127.0.0.1:8080/stream.mp4
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
internet exposure.

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
