# Building

timps is a small, dependency-light C11 codebase built with a plain
top-level `Makefile` — no autotools/CMake/meson. Two build modes exist:

- **`make sim`** — a host (x86/whatever `cc` targets) build using the
  simulation HAL backend (`src/hal/hal_sim.c`), which feeds pre-recorded
  files instead of talking to real ISP/encoder hardware. No cross toolchain
  or vendor SDK headers needed. Produces `timpsd-sim`.
- **`make PLATFORM=... CROSS_COMPILE=... IMP_INC=...`** — a cross-compiled
  build against the real Ingenic `libimp` SDK for a MIPS target. Produces
  `timpsd`.

## Host simulation build

```sh
make sim                       # builds ./timpsd-sim with the host cc
./timpsd-sim -c timps.conf     # feeds files instead of the ISP
ffplay http://127.0.0.1:8880/stream.mp4
```

`hal_sim.c` reads `sim.video0` / `sim.video1` (raw H.264/H.265 Annex-B
files, looped), `sim.audio` (an ADTS AAC file), and `sim.jpeg` (a still
JPEG) and republishes them into the [hub](Architecture.md) at the
configured fps/sample rate, so the entire RTSP/HTTP/control/events stack
can be exercised without hardware. `hal_isp_total_gain()` /
`hal_isp_ae_luma()` always report "unavailable" in this backend, so
[day/night](Day-Night.md) falls back to its brightness-proc-scrape path
(which is also absent on a host, so the day/night thread simply idles).

`make sim` links `src/hal/hal_sim.c` in place of `src/hal/hal_ingenic.c`
and does not need `IMP_INC`/vendor libraries at all — this is the fastest
way to iterate on protocol/control/muxer code.

## Cross-compiling for a real camera

```sh
git clone --recurse-submodules https://github.com/Lu-Fi/timps
cd timps
make PLATFORM=T31 CROSS_COMPILE=mipsel-linux-
make strip PLATFORM=T31 CROSS_COMPILE=mipsel-linux-
```

- `PLATFORM` selects one of `T10 T20 T21 T23 T30 T31 T40 T41 C100`.
- `CROSS_COMPILE` is the toolchain prefix (default `mipsel-linux-`).
- `IMP_INC` points at the vendored Ingenic IMP SDK headers; it defaults to
  a version pinned per platform under `./include/<SoC>/<version>/<lang>`
  (a git submodule sourced from
  [gtxaspec/ingenic-headers](https://github.com/gtxaspec/ingenic-headers)):

  | Platform | Default `IMP_INC` |
  | --- | --- |
  | T31 | `include/T31/1.1.6/en` |
  | C100 | `include/C100/2.1.0/en` |
  | T21 | `include/T21/1.0.33/zh` |
  | T23 | `include/T23/1.3.0/en` (must be ≥1.1.2 — the 1.1.0 header is missing the trailing `fcrop` member of `IMPFSChnAttr`; with libimp 1.3.0 that mismatch makes the framesource silently deliver no frames. `fs_create()` has a compile-time tripwire against this.) |
  | T30 | `include/T30/1.0.5/zh` |
  | T40 | `include/T40/1.2.0/zh` |
  | T41 | `include/T41/1.2.0/zh` |
  | T20, T10 | `include/T20/3.12.0/zh` (T10 builds against the T20 headers) |

- `IMP_LIB` optionally adds a `-L` search directory for `libimp.a`/`.so`.
- `SYSROOT` optionally passes `--sysroot` to the cross toolchain.
- `IMPLIBS` (default `-l:libimp.a -l:libalog.a -l:libsysutils.a`) controls
  how the vendor libraries are linked — static archives by default for a
  single self-contained drop-in binary; adjust to match your SDK (e.g. add
  `libmuslshim`/`libaudioProcess`).
- The result is a single `timpsd` binary. `make strip` strips debug symbols
  after building.

Small-RAM targets (e.g. T10, 64 MB) can tune buffer sizes via extra
`CFLAGS`, e.g.:

```sh
make PLATFORM=T10 CFLAGS="-DMS_AU_BUF_MAX=524288 -DMS_JPEG_BUF_MAX=262144 \
  -DMS_RTSP_QCAP=32 -DMS_MP4_QCAP=32 -DRTSP_MAX_CLIENTS=4 -DHTTP_MAX_CLIENTS=4"
```

(AU/JPEG buffers already auto-scale with the configured resolution; these
overrides lower the ceiling further for very constrained boards.)

## Build hardening

`HARDEN=1` (default) adds `-fstack-protector-strong` (+ `-D_FORTIFY_SOURCE=2`
unless `FORTIFY=0`) plus linker RELRO/`noexecstack`. Set `HARDEN=0` for a
bare build if the target toolchain lacks `libssp`/fortified libc wrappers
(`build.sh`, the scripted MIPS cross-build driver, carries its own
libc-aware copy of this logic — see `docs/M14-build-hardening.md` for the
full rationale).

## Feature flags (`USE_*`)

Every optional feature is gated by a `USE_*` Make variable, each turning
into a `-DUSE_*` (or a differently-named macro, noted below) preprocessor
define. A feature that is off compiles its code out entirely — the
resulting binary is byte-identical to a build that never had the feature.

| Flag | Default | Effect |
| --- | --- | --- |
| `USE_CONTROL` | **1 (on)** | Builds `src/control.c` + `src/events.c`: the `/control` live-settings API and the `/events` SSE push stream. See [HTTP /control API Reference](HTTP-Control-API.md). `USE_CONTROL=0` removes it entirely (saves ~15 KB, reduces attack surface). |
| `USE_DAYNIGHT` | **1 (on)** | Builds `src/daynight.c`: the native automatic day/night detection thread. See [Day/Night](Day-Night.md). |
| `USE_RECORD` | **1 (on)** | Builds `src/record.c`: local SD recording (continuous/motion-triggered fMP4 segments) + the `/control` clip/record endpoints. `USE_RECORD=0` saves ~11 KB. |
| `USE_TIMELAPSE` | **1 (on)** | Builds `src/timelapse.c`: periodic JPEG capture to SD. `USE_TIMELAPSE=0` saves ~4 KB. |
| `USE_FAAC` | 0 (off) | Software AAC audio encoding via `libfaac` (`FAACLIB`, default static `-l:libfaac.a`). Needed for AAC on RTSP/browser preview; without it `audio.codec` should be `pcmu`/`pcma`. |
| `USE_TLS` | 0 (off) | HTTPS (`http.https`) + RTSPS (`rtsp.tls`, port 322 by default) via mbedTLS (`src/tls.c`). Needs `-lmbedtls`/`-lmbedx509`/`-lmbedcrypto` available; auto-enabled by the thingino package build when `libmbedtls` is linked. |
| `USE_SRT` | 0 (off) | MPEG-TS over SRT output in listener mode (`src/srt.c`), needs `libsrt`. Because SRT links C++ (`libstdc++`), the final link step switches from `gcc` to `g++` (`LINK_DRV`) when this is on. |
| `USE_ROTATE` | 0 (off) | Image rotation (`videoN.rotation = 0\|90\|270`, plus `180` on T40/T41). Off by default = zero `ROT_HAS_*` macros defined, so all rotation code compiles out. See [Platform & SDK Support](Platform-SDK-Support.md) and `docs/rotation.md`. |
| `USE_SW_ROTATE` | 0 (off) | Opt-in **software** 90/270 rotation on T23 only (CPU NV12 transpose + unbound `IMP_Encoder_YuvEncode`; no hardware OSD/privacy on the rotated stream). Implies `USE_ROTATE=1`. Defines `-DMS_ENABLE_SW_ROTATE` (not `USE_SW_ROTATE` itself) in the actual compile. |
| `USE_BACKCHANNEL` | 0 (off) | ONVIF-style two-way audio backchannel (client → camera speaker) via native `IMP_AO`. Pure-C G.711 decode built in; see [Audio](Audio.md). |
| `USE_BC_AAC` | 0 (off) | Also accept AAC on the backchannel (`libhelix-aac`, `HELIXLIB`/`HELIX_INC`). Implies `USE_BACKCHANNEL=1`. |
| `USE_PLAY` | 0 (off) | System-sound play queue: a FIFO at `/run/timps/audio_out` accepting the same `PLAY`/`STOP` protocol prudynt/raptor's `/usr/sbin/play` speaks, driving `IMP_AO` natively. Decodes WAV, raw PCM16, G.711 out of the box. |
| `USE_PLAY_OPUS` | 0 (off) | Also decode Ogg-Opus in the play queue (`opusfile`, `OPUSLIB`/`OPUS_INC`). Implies `USE_PLAY=1`. |

Any combination of `USE_BACKCHANNEL`/`USE_PLAY` pulls in `src/rtsp/speaker.c`
+ `src/codec/resample.c` (the shared `IMP_AO` owner + resampler) — this is
handled automatically by the Makefile's `USE_AUDIO_OUT` derived variable.

### Build recipe internals worth knowing

- `BASE` lists the platform-independent sources shared by both `target`
  and `sim`; `TARGET_SRC`/`SIM_SRC` add the HAL-specific and
  `USE_CONTROL`-only sources (`control.c`, `events.c`, `daynight.c`) — note
  the sim build **also** links `control.c`/`events.c`/`daynight.c`
  unconditionally (so `/control` works against `timpsd-sim` too) and
  `imp_motion.c` (motion detection is exercised in sim as well, via the
  `MOTION_AVAILABLE` host-sim fallback in `motion_caps.h`).
- The real target build compiles then links in two steps so vendor static
  libs (`IMPLIBS`) can be linked last; the object files are removed after a
  successful link (`@rm -f $(TARGET_OBJS)`).
- `VERSION` is baked into the binary (`timps -v` / the startup log line) via
  `git describe --tags --always --dirty`; the buildroot package overrides
  it with `VERSION=$(TIMPS_VERSION)`.

### Self-test target

```sh
make test-auth
```

Builds `timpsd-sim` with `USE_CONTROL=1`, starts it on unprivileged ports,
runs `scripts/test_auth.sh` against it (RTSP fully exercised; HTTP
negative tests are skipped over loopback since the HTTP server trusts
`127.0.0.0/8` — point it at a real LAN IP to exercise those), then stops
it and propagates the exit code.

## Building as part of thingino-firmware

A full camera firmware image builds timps through a buildroot package in
the sibling `thingino-firmware` (or `thingino-firmware-LuFi`) tree, at
`package/timps/`:

```sh
make menuconfig      # Streamer Packages → Streamer → timps  (deselect prudynt/raptor)
make                 # build the firmware, flash as usual
```

`package/timps/timps.mk` invokes this project's `Makefile` with
`PLATFORM=<SOC_FAMILY upper>` and the cross toolchain, translating each
`BR2_PACKAGE_TIMPS_*` Kconfig boolean into the matching `USE_*` flag, e.g.:

| Kconfig option | `USE_*` flag | Default |
| --- | --- | --- |
| `BR2_PACKAGE_TIMPS_FAAC` | `USE_FAAC` | y |
| `BR2_PACKAGE_TIMPS_CONTROL` | `USE_CONTROL` | y |
| `BR2_PACKAGE_TIMPS_DAYNIGHT` | `USE_DAYNIGHT` | y |
| `BR2_PACKAGE_TIMPS_RECORD` | `USE_RECORD` | y |
| `BR2_PACKAGE_TIMPS_TIMELAPSE` | `USE_TIMELAPSE` | y |
| `BR2_PACKAGE_TIMPS_TLS` | `USE_TLS` | y |
| `BR2_PACKAGE_TIMPS_SRT` | `USE_SRT` | n |
| `BR2_PACKAGE_TIMPS_ROTATE` | `USE_ROTATE` | n |
| `BR2_PACKAGE_TIMPS_SW_ROTATE` (depends on `ROTATE`) | `USE_SW_ROTATE` | n |
| `BR2_PACKAGE_TIMPS_BACKCHANNEL` | `USE_BACKCHANNEL` | n |
| `BR2_PACKAGE_TIMPS_BC_AAC` (depends on `BACKCHANNEL`) | `USE_BC_AAC` | n |
| `BR2_PACKAGE_TIMPS_PLAY` | `USE_PLAY` | n |
| `BR2_PACKAGE_TIMPS_PLAY_OPUS` (depends on `PLAY`) | `USE_PLAY_OPUS` | n |

The package also exposes an **"Audio backchannel preset"** choice that
bundles the interdependent audio options for convenience:
`MANUAL` (default, set the above individually), `FULL` (backchannel + AAC
+ Opus play queue, confirmed to fit T31-family flash, too large for a
~5 MB T20 image), and `MINIMAL` (backchannel + G.711 play queue only, no
extra codec libraries — confirmed on a 5 MB-rootfs T20 board).

Beyond the Makefile translation, the package's finalize hooks handle
cross-package integration: swapping in timps-native WebUI bridge CGIs and
a fMP4/MSE preview page when `BR2_PACKAGE_THINGINO_WEBUI` and
`BR2_PACKAGE_TIMPS_CONTROL` are both enabled, disabling thingino's
standalone `S97daynightd` init script when `USE_DAYNIGHT` is on (timps is
the detector), and installing timps's own `S96onvif_discovery` (which
sources ONVIF credentials from `/etc/timps.conf`).

The package ships a default `/etc/timps.conf` (`package/timps/files/timps.conf`)
with `rtsp.user`/`rtsp.pass` and `http.user`/`http.pass` both set to
`thingino`/`thingino` — the same default-credential convention prudynt-t
uses on thingino; leaving `rtsp.user` empty makes RTSP open (no auth). See
[Configuration Reference](Configuration-Reference.md) and
[HTTP /control API Reference](HTTP-Control-API.md) for the full
authentication model.

This wiki's focus is the timps daemon itself; consult
`package/timps/timps.mk`/`Config.in` in the firmware repo directly for the
authoritative, up-to-date buildroot integration.
