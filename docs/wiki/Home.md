# timps wiki

**timps** ("Tiny IMP Streamer") is a lightweight, dependency-light RTSP /
fragmented-MP4 / MJPEG streaming daemon for Ingenic T-series SoC IP cameras.
It is built directly on the vendor `libimp` SDK — no live555, libconfig,
libwebsockets or libschrift — and is designed as a drop-in alternative to
[prudynt-t](https://github.com/themactep/prudynt-t) / raptor within the
[thingino](https://github.com/themactep/thingino-firmware) firmware
ecosystem.

This wiki is a curated, evergreen reference for developers and operators. It
is distinct from `docs/*.md` at the repository root, which are historical
investigation notes, build-hardening logs and one-off code audits — those are
linked from here where they contain relevant deep-dive detail, but this wiki
is the place to start.

> Every page in this wiki is written against, and verified against, the
> actual source in `src/` on `main`.

## What timps does

- **RTSP** (port 554, TCP interleaved or UDP transport; RTSPS on port 322
  when built with `USE_TLS`) — H.264 always, H.265 where the SoC/SDK
  supports it, AAC or G.711 (µ-law/A-law) audio.
- **Browser preview over fragmented MP4** (`/stream.mp4`, `/`) — MSE-compatible
  fMP4 muxing for `ffplay`/VLC and `<video>`/MSE in a browser, with audio.
- **MJPEG** (`/stream.mjpeg`) and **JPEG snapshot** (`/snapshot.jpg`).
- **SRT output** (`USE_SRT`) — MPEG-TS over SRT in listener mode, e.g. for
  `ffplay srt://<ip>:9000`.
- **On-demand encoding** — a stream's encoder only runs while at least one
  client is subscribed; idle streams cost ~0% CPU.
- **Live control API** (`/control`, `USE_CONTROL`) — read and change ISP
  image tuning, audio, OSD, motion and more without a restart, with changes
  persisted back to the config file.
- **Event push** (`/events`, Server-Sent Events) — subscribe to motion,
  day/night and stats updates instead of polling.
- **Grid-based motion detection** built on the Ingenic IVS ("Intelligent
  Video System") hardware move-detection API.
- **Automatic day/night switching**, replacing thingino's separate
  `daynightd` daemon: an `auto` sensor automaton (four independent
  day/probe/heartbeat/boot paths) with an optional calendar, or a
  calendar-only `schedule` mode. See [Day/Night](Day-Night.md).
- **Local recording** (continuous or motion-triggered fMP4 segments to SD)
  and **timelapse** (periodic JPEG capture).
- **Native speaker output** (`IMP_AO`) for an ONVIF-style two-way audio
  backchannel and a system-sound play queue — no external `/bin/iac`/`play`
  helper needed.
- **Privacy masks**, **per-stream TrueType OSD overlays**, and **image
  rotation** (hardware 90/270/180 on some SoCs, software 90/270 on others).
- **Authentication**: RTSP Digest, HTTP Basic, and a token mechanism for the
  HTTP media/API endpoints, with a localhost bypass for on-device UIs.

## Supported platforms

T10 · T20 · T21 · T23 · T30 · T31 · T40 · T41 · C100 (Ingenic SoC families).
See [Platform & SDK Support](Platform-SDK-Support.md) for the capability
matrix (two encoder-API generations, per-SoC rotation tiers, ISP tuning
availability) and links to the deeper `docs/sdk-feature-gaps.md` and
`docs/rotation.md` investigations.

## Page index

| Page | Covers |
| --- | --- |
| [Architecture](Architecture.md) | Process/thread model, the HAL abstraction, the hub pub/sub mechanism, fan-out queues, and the end-to-end data flow from sensor to client |
| [Building](Building.md) | `make sim`, cross-compiling for a real camera, every `USE_*` build flag, and the thingino-firmware packaging integration |
| [Configuration Reference](Configuration-Reference.md) | Every `timps.conf` key, grouped by section, with type/default/range and live-vs-restart-only classification |
| [HTTP /control API Reference](HTTP-Control-API.md) | The `/control` GET/POST JSON API, authentication, the `caps` capability object, and the `/events` SSE stream |
| [Streaming Protocols](Streaming-Protocols.md) | RTSP, HTTP fMP4, MJPEG, snapshot and SRT — ports, transports, codecs, and client-compatibility notes |
| [Motion Detection](Motion-Detection.md) | The IVS grid model, sensitivity mapping, the `on_motion` hook, and the T23 software-rotation coordinate caveat |
| [Recording & Timelapse](Recording-Timelapse.md) | Continuous/motion-triggered SD recording with pre/post-roll, segment rotation, and periodic JPEG timelapse capture |
| [Rate Control and Bandwidth: T23 vs T31](Rate-Control-Bandwidth.md) | Why the classic and new-generation encoder rate controllers use such different bandwidth for the same settings — a rate target vs. scene-content-adaptive quality |
| [Rate Control Parameters](Rate-Control-Parameters.md) | Every rate-control field (`bitrate`, `rc_mode`, `quality_lvl`, `change_pos`, `i_bias_lvl`, ...): range, per-SoC support, and cost — measured vs. header-derived |
| [Day/Night](Day-Night.md) | Automatic day/night switching, decision modes, and the ISP running-mode latch fix |
| [Day/Night Design Notes](Day-Night-Design-Notes.md) | Historical: why the day/night subsystem kept needing fixing — the incident record, the restart-equivalence invariant, and the replay-harness plan that made the 2026-08-17 redesign possible |
| [Audio](Audio.md) | Capture codecs, the ONVIF-style two-way backchannel, and the system-sound play queue |
| [Platform & SDK Support](Platform-SDK-Support.md) | Per-SoC capability summary; links to `docs/sdk-feature-gaps.md` and `docs/rotation.md` |
| [Testing / QA](Testing-QA.md) | `scripts/timps-qa.sh` — what it checks and how to run it against a real camera |
| [Logging](Logging.md) | Log levels, the per-module `debug_modules` switch, the module-tag table, and the machine-readable `switching to` / `probe:` / `stats:` lines |

## Quick links

- Top-level `README.md` — condensed feature tour and quick-start.
- `docs/rotation.md` — full rotation deep-dive (direction conventions,
  per-SoC constraints, OSD-on-rotated-stream limitations).
- `docs/sdk-feature-gaps.md` — an independent audit of every `IMP_*` SDK
  call available per platform vs. what timps currently uses, with a
  prioritized list of unused capabilities.
- `docs/backchannel.md` — implementation notes for the audio backchannel.
- `docs/camera-fleet.md` — a real-world fleet inventory (SoC/sensor mix per
  deployed camera).
