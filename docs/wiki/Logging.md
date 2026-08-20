# Logging

timps writes to stderr and, with `general.syslog = 1`, to syslog. Two knobs
decide what you see: a global level, and a per-module debug switch.

## The two knobs

```
general.loglevel      = 2          # 0=err 1=warn 2=info 3=debug
general.debug_modules =            # comma-separated module names, case-insensitive
```

The filter is one line (`src/log.c`, `log_printf`):

```c
if (level > g_level && !mod_is_debug(module)) return;
```

A message passes if its level is at or below `loglevel`, **or** if its module
is named in `debug_modules` — a listed module is raised to DEBUG outright, so
*all* of its levels pass regardless of the global level. So `debug_modules`
opens one module's full output without making every other module verbose —
which is the point: `loglevel = 3` on a camera that also runs the ISP layer
produces far more than anyone wants to read. (Until 2026-08-20 only the
listed module's DEBUG lines passed the filter, so `loglevel = 1` plus
`debug_modules = daynight` showed DAYNIGHT's DEBUG lines while hiding its
INFO lines — nobody expected that.)

`debug_modules` takes at most 8 names — a 9th name, or one too long to be a
module tag, is rejected with a WARN rather than silently dropped — and is
live-applicable over `POST /control` — no restart, no config file edit:

```
curl -s -X POST http://<cam>:8880/control -H "X-Auth-Token: $T" \
     -d '{"general":{"debug_modules":"daynight"}}'
```

## What each level is for

| level | contains | example |
|---|---|---|
| 0 `err` | the subsystem cannot do its job and stops trying: a socket that cannot bind, an encoder channel that never comes up | `bind/listen on 9000 failed` |
| 1 `warn` | something is wrong or degraded, and a human may need to act | `IMP_AI_EnableAec failed - continuing without echo cancellation` |
| 2 `info` | **events**: state changed, a decision was taken, a subsystem came up | `switching to day (probe confirmed)`, `ISP up, sensor=sc4336p fps=30` |
| 3 `debug` | **measurements**: per-tick values, per-probe detail, per-frame notes | `probe: r=1.05 lit=3350 dark=3520 hr=199 verdict=day` |

The dividing line between 2 and 3 is *event versus measurement*. An end user
wants to know that the camera switched to day mode and why; they do not want
the exposure index of every probe that decided not to switch. Level 2 is the
shipped default and is meant to stay readable on a camera that runs for
months.

## Modules

Names as they appear in the log and as `debug_modules` accepts them.

| module | source | err | warn | info | debug | what its debug output adds |
|---|---|---:|---:|---:|---:|---|
| `DAYNIGHT` | `daynight.c` | 0 | 21 | 15 | 9 | every probe: ratio verdict and its branch, the structured `probe: r=… lit=… dark=… hr=… verdict=…` line, exposure vs night reference, illuminator-off readings |
| `HAL_ING` | `hal/hal_ingenic.c` | 44 | 57 | 39 | 10 | encoder/framesource internals, polling and teardown detail |
| `CTRL` | `control.c` | 0 | 4 | 3 | 5 | request/field handling on `/control` |
| `OSD` | `hal/imp_osd.c` | 4 | 8 | 6 | 3 | overlay placement and region updates |
| `RTSP` | `rtsp/rtsp.c` | 4 | 7 | 4 | 2 | per-drop queue-overflow detail, dropped P-frames, IDR re-requests (the *first* keyframe drop of a session is a WARN) |
| `HTTP` | `mp4/httpd.c` | 2 | 11 | 7 | 2 | same for the fMP4/MJPEG side, including adaptive freeze (first keyframe drop per client is a WARN) |
| `HUB` | `hub.c` | 0 | 0 | 0 | 2 | fan-out subscribe/unsubscribe — this module logs *nothing* below debug; drops are counted in `/control` `queue_drops` and warned about by the consumers (RTSP/HTTP) |
| `REC` | `record.c` | 9 | 6 | 6 | 2 | segment and writer detail |
| `TLS` | `tls.c` | 5 | 1 | 1 | 1 | handshakes that are ordinary peer noise (EOF, close_notify, reset); the interesting ones are WARN |
| `CONFIG` | `config.c` | 0 | 23 | 3 | 0 | — |
| `SRT` | `srt.c` | 7 | 9 | 3 | 1 | the 10-second key=value `stats:` line (RTT, loss, retransmits, send rate) while a receiver is connected |
| `spk` | `rtsp/speaker.c` | 0 | 8 | 7 | 0 | — |
| `MAIN` | `main.c` | 4 | 4 | 7 | 0 | — |
| `MOTION` | `hal/imp_motion.c` | 9 | 6 | 5 | 0 | — |
| `TL` | `timelapse.c` | 4 | 1 | 5 | 0 | — |
| `HAL_SIM` | `hal/hal_sim.c` | 4 | 2 | 4 | 0 | — |
| `TRACE` | `trace.c` | 0 | 4 | 0 | 0 | — (deliberately WARN-only: its output is gated by its own `general.trace*` switch) |
| `bc` | `rtsp/backchannel.c` | 0 | 1 | 3 | 0 | — |
| `AAC` | `codec/aac.c` | 0 | 1 | 0 | 0 | — |
| `LOG` | `log.c` | 0 | 2 | 0 | 0 | — (the logger itself: rejected `debug_modules` names) |

Names are matched case-insensitively, so `daynight` and `DAYNIGHT` are the
same; they are written above exactly as they appear in the log. Note `CTRL`
and `REC` — not `CONTROL` or `RECORD`. A name too long to be a module tag, or
a 9th name, is rejected with a WARN. A *well-formed* name that matches
nothing (a typo of the right length) is still accepted silently and does
nothing — it looks identical to a module that simply has no debug output.

Modules with no debug lines are listed so that `debug_modules=motion` visibly
does nothing rather than looking broken.

## Collecting day/night data

The day/night measurement series lives at DEBUG. A camera that feeds a central
collector therefore needs

```
general.debug_modules = daynight
```

in its `/etc/timps.conf`. Without it the automaton still works and still logs
its decisions at INFO — you get the switches and the reference changes, but
not the per-probe numbers the analysis scripts read.

This is deliberate, but the point is *readability*, not bandwidth: at the
default cadence (`probe_min_gap_s = 600`, `heartbeat_s = 14400`) the demoted
lines amount to a few dozen a day, and a camera that sets `debug_modules`
ships exactly as many bytes as before the change. On a collector, the bulk of
the day/night-related traffic is elsewhere entirely — `crond`'s once-a-minute
job lines and, where `dnlog.isp_syslog` is on, the per-minute `dnisp` CSV row
from `timps-dn-isp-log` — neither of which this change touches. What the
change buys is a level 2 that stays readable on installations that never
asked for per-probe measurements.

The same convention applies to SRT's 10-second `stats:` line: it is a
measurement and lives at DEBUG — a dashboard that wants the series sets
`debug_modules = srt` (or `daynight,srt`).

The replay harness (`scripts/dn-replay.py`) sets `debug_modules=daynight` for
every scenario: several `expect_log` patterns match lines that are now DEBUG,
and the `forbid_log` checks would otherwise pass vacuously against lines the
filter had already removed.
