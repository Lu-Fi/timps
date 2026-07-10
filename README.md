# timps

**Tiny IMP Streamer** — a minimal, dependency-light RTSP / fragmented-MP4 / MJPEG
streamer for Ingenic SoC IP cameras. Built straight on the vendor **libimp**.

## Features

- **Pure C**, only `libimp` + pthread (optional `libfaac` for AAC) — no heavyweight dependencies
- **RTSP**: H.264 / H.265 video, AAC / G.711 audio (RFC 2326, RFC 6184, RFC 7798)
- **Browser preview** via fragmented-MP4 (MSE), plus JPEG snapshot & MJPEG
- **On-demand encoding** — encoders only run while a client is watching
- **TrueType OSD** with placeholders (time, hostname, uptime, fps, bitrate, viewers…)
- **Motion detection** (IMP_IVS)
- **Auth**: RTSP Digest MD5 / HTTP Basic
- **Tiny footprint** — runs on T10 / T20 / T21 / T23 / T30 / T31 / T40 / T41 / C100

## Supported SoCs

| Family | SoCs |
|--------|------|
| T10/T20/T21 | Xiaomi Dafang, Wyze Cam, Wansview, … |
| T23 | |
| T30/T31 | Atom Cam, WyzeV3, Ingenic eval boards |
| T40/T41 | Ingenic eval boards |
| C100 | |

## Building

### Prerequisites

You need:
1. A MIPS cross-compiler — e.g. `mips-linux-uclibc-gnu-gcc` from the Ingenic SDK,
   or `mips-linux-gnu-gcc` with `-muclibc`.
2. The Ingenic SDK headers and libraries for your SoC.

Layout expected by the Makefile:

```
sdk/
├── include/
│   └── imp/
│       ├── imp_common.h
│       ├── imp_system.h
│       ├── imp_isp.h
│       ├── imp_framesource.h
│       ├── imp_encoder.h
│       ├── imp_audio.h
│       ├── imp_osd.h
│       └── imp_ivs.h
│           imp_ivs_move.h
└── lib/
    └── uclibc/
        ├── libimp.so
        └── libalog.so
```

You can obtain SDK headers from
[gtxaspec/ingenic-headers](https://github.com/gtxaspec/ingenic-headers) (MIT-licensed
English translations).

### Compile

```bash
# T20/T30/T31 (big-endian uClibc)
make SDK=/path/to/sdk CROSS_COMPILE=mips-linux-uclibc-gnu-

# T31 with GNU toolchain
make SDK=/path/to/sdk CROSS_COMPILE=mips-linux-gnu- CFLAGS_EXTRA=-muclibc

# T41 (little-endian)
make SDK=/path/to/sdk CROSS_COMPILE=mipsel-linux-

# Strip the binary
make strip
```

Deploy:

```bash
scp timps root@camera:/usr/sbin/
scp timps.conf.example root@camera:/etc/timps.conf
```

## Configuration

Copy `timps.conf.example` to `/etc/timps.conf` and adjust:

```ini
sensor          = gc2053          # sensor driver name
sensor_bin      = /etc/sensor/gc2053.bin

stream0.codec   = h264            # h264 or h265
stream0.width   = 1920
stream0.height  = 1080
stream0.fps     = 25
stream0.bitrate = 2048            # kbps

rtsp_port       = 554
rtsp_user       = admin
rtsp_pass       = admin           # change this!
rtsp_auth       = 1               # 1 = Digest MD5

http_port       = 8080
http_auth       = 1               # 1 = Basic

osd_enabled     = 1
osd_label       = %Y-%m-%d %H:%M:%S  %{host}

motion_enabled  = 0
motion_script   = /etc/timps/motion.sh
```

See `timps.conf.example` for all options.

## Usage

```
timps [-c config] [-d] [-v] [-h]

  -c <file>   Configuration file (default: /etc/timps.conf)
  -d          Daemonize
  -v          Increase log verbosity (repeat for more: -vv = debug)
  -h          Help
```

```bash
timps -c /etc/timps.conf
```

## Stream URLs

| Protocol | URL |
|----------|-----|
| RTSP (main) | `rtsp://<host>:554/` or `/stream0` |
| RTSP (sub)  | `rtsp://<host>:554/stream1` |
| fMP4 (MSE)  | `http://<host>:8080/stream` |
| fMP4 sub    | `http://<host>:8080/stream1` |
| MJPEG       | `http://<host>:8080/mjpeg` |
| Snapshot    | `http://<host>:8080/snapshot.jpg` |
| Info page   | `http://<host>:8080/` |

### Browser preview (MSE)

```html
<script>
const ms  = new MediaSource();
const vid = document.querySelector('video');
vid.src   = URL.createObjectURL(ms);

ms.addEventListener('sourceopen', () => {
  const sb = ms.addSourceBuffer('video/mp4; codecs="avc1.42E01E"');
  fetch('/stream').then(r => {
    const reader = r.body.getReader();
    function pump() {
      reader.read().then(({done, value}) => {
        if (done) return;
        sb.appendBuffer(value);
        pump();
      });
    }
    pump();
  });
});
</script>
```

### VLC / ffplay

```bash
ffplay ******192.168.1.100/stream0
vlc    ******192.168.1.100/stream1
```

## OSD Placeholders

| Placeholder  | Description |
|-------------|-------------|
| `%Y-%m-%d`  | Date (strftime) |
| `%H:%M:%S`  | Time (strftime) |
| `%{host}`   | Hostname |
| `%{uptime}` | Uptime `h:mm:ss` |
| `%{viewers}`| Active viewer count |
| `%{fps}`    | Encoder FPS |
| `%{bitrate}`| Encoder bitrate (kbps) |

## Motion Detection

When `motion_enabled = 1`, `IMP_IVS` MoveDetect is used. On trigger:

- A log entry is written (`INFO motion detected`)
- The optional `motion_script` is executed via `system()` (asynchronously)

Example script `/etc/timps/motion.sh`:

```sh
#!/bin/sh
curl -s "http://home-server/notify?src=camera&event=motion" &
```

## Architecture

```
Sensor → ISP → FrameSource[0] ─→ OSD[0] ─→ Encoder[0] ─┐
                                                          ├── RTSP server ─→ clients
               FrameSource[1] ─→ IVS[0] ─→ Encoder[1] ─┘   HTTP server ─→ browsers
                                    │
                                    └── motion_script
Audio device ──────────────────────────────────────────────────────────────→ RTSP audio
```

Each encoder starts on first viewer and stops on last disconnect
(**on-demand encoding**).

## SoC compatibility notes

| SoC | Old/New API | Endian | Toolchain |
|-----|-------------|--------|-----------|
| T10/T20/T21/T23 | Old (`IMPEncoderCHNAttr`) | BE | mips-linux-uclibc-gnu- |
| T30/T31 | New (`IMPEncoderChnAttr` + `SetDefaultParam`) | BE | mips-linux-uclibc-gnu- |
| T40/T41 | New (+ `IMPVI_MAIN`) | BE | mips-linux-uclibc-gnu- |
| C100 | New | BE | mips-linux-uclibc-gnu- |

The code uses the new T31+ API by default. For T20/T21/T23 you will need to
use the old `IMPEncoderCHNAttr` struct — SDK headers take care of this when
you compile with the appropriate SDK.

## License

MIT — see [LICENSE](LICENSE).
