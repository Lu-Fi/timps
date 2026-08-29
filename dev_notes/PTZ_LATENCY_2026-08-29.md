# PTZ joystick-to-motion latency — 2026-08-29

Raw measurement pass on cam-garage (Wuuk Y0510, T31, `192.168.10.21`,
build `v1.9.3-60-g2a19efd`), commissioned to find out how long it takes
from issuing a pan command to the movement actually being visible in each
of timps' stream types. This document records only what was **measured**;
no code has been read or changed yet. It is the starting brief for the
follow-up code investigation (see "Next step" at the bottom).

**Caveat on this data**: at measurement time, Frigate (an external NVR)
may still have been holding its own RTSP session open against this camera
in parallel with the four sessions described below — the fleet is in
normal production use and this run did not explicitly check for or
exclude that. Section "Consumer-mix effect" below shows RTSP latency is
*sensitive to what else is connected*, so a silent fifth consumer is a
real confound on the absolute numbers, not just a footnote. The user is
disconnecting Frigate from cam-garage before the follow-up investigation
specifically to remove this variable — any future measurement should
verify session count first (e.g. via `/control` or on-device connection
state) rather than assume a clean baseline.

## Method

- **Trigger (t0)**: `motors -d h -x <target> -y 0` issued over an
  already-open SSH ControlMaster channel to the camera. `motors -x/-y`
  alone does nothing — it requires the `-d` direction verb. t0 is the
  local wall-clock timestamp immediately before the command line is
  written to the (already-open) channel; SSH exec + on-device dispatch
  (~10-30 ms) is therefore included in every figure below, not subtracted.
- **Move**: alternated between xpos 2630 and 3030 (both endpoints richly
  textured in-frame, confirmed via snapshot first) so consecutive trials
  don't compound position drift.
- **Detection (t1)**: all four streams (RTSP main ch0, RTSP sub ch1, HTTP
  fMP4, HTTP MJPEG) were held open and recorded **simultaneously** during
  the same physical move — a paired design, so motor start-jitter cancels
  out of the *between-stream* comparison even though it's present in each
  stream's own absolute numbers. Each stream's frames were diffed against
  its own pre-move baseline; onset was unambiguous (diff jumps from ~0.5
  to 10-40 in a single frame) with steady frame arrival and no stalls
  before t0.
- **n = 24** per stream (2 independent batches of 12), plus a separate
  cross-check and two separate small consumer-mix probes (n=6 each,
  below).
- Raw data, capture/analysis scripts, and snapshots:
  `/tmp/claude-1000/-mnt-NVMe-git-timps/e0226b58-f2f9-4e5c-a5cd-963bb8e48c44/scratchpad/ptz/`
  (session-scoped scratch dir — copy anything worth keeping before it's
  cleaned up).

## Results — all four streams open concurrently

| stream | n | mean | median | min | max | sd | fps |
|---|---|---|---|---|---|---|---|
| RTSP main (ch0) | 24 | 588 ms | 541 ms | 386 ms | 1049 ms | 171 ms | 15 |
| RTSP sub (ch1) | 24 | 1220 ms | 1279 ms | 799 ms | 1604 ms | 175 ms | 15 |
| HTTP fMP4 | 24 | 690 ms | 548 ms | 360 ms | 3668 ms | 647 ms | 15 |
| HTTP MJPEG | 24 | 501 ms | 501 ms | 368 ms | 639 ms | 81 ms | 5 (actual, not the nominal 25) |

fMP4 excluding one 3.67 s outlier trial: mean 561 ms, median 520 ms, max
873 ms, sd 133 ms. That one outlier is itself worth explaining, not just
excluding — see open questions.

## `motors -b` cross-check (independent, non-video signal)

`motors -b` (on-device "is the motor currently moving" flag) flips 0→1 at
a mean of 71 ms after t0 (range 28-144 ms, n=24). This sits ~440 ms
*before* the fastest video onset across the four streams, which is
consistent with a real sensor→ISP→encode→network→decode pipeline
contributing the rest — the two measurement methods agree on order of
magnitude and don't contradict each other.

## Systematic differences between streams

- **RTSP sub is consistently ~700 ms slower than RTSP main** (paired
  per-trial median, so this isolates the stream-type effect from
  motor-start jitter). Ruled out: B-frame reordering
  (`has_b_frames=0` confirmed on both channels via ffprobe). Suspected but
  **not verified**: the sub-stream's lower bitrate (384 kbps CBR) and/or a
  GOP size around 50 could mean deeper encoder-side buffering before a
  frame is considered "final" and flushed — this is a hypothesis, not a
  finding; the code investigation should confirm or refute it directly
  rather than trust this guess.
- **fMP4 tracks RTSP main closely** (paired median difference −6 ms) —
  expected, since they're the same underlying encoded channel (ch0),
  differing only in transport/muxing.
- **MJPEG's real frame rate is 5 fps, not the nominal 25** — worth
  checking why (config vs. runtime throttling vs. CPU/encoder
  contention) as a separate, smaller side question.
- A raw-socket MJPEG parser (bypassing ffmpeg's own decode pipeline) gave
  **378 ms** for the same kind of measurement, vs. ~500 ms through ffmpeg
  — meaning ffmpeg itself was adding roughly one full frame interval of
  measurement artifact on every stream in the main table above. **The
  absolute numbers in the results table likely all have a
  measurement-tool overhead of "about one frame interval" baked in on top
  of timps' real latency** — the *relative* comparisons between streams
  should still hold since the same tool was used consistently, but don't
  take e.g. "588 ms on RTSP main" as pure timps-side latency without
  first subtracting a comparable client-side decode/analysis constant.

## Consumer-mix effect (small probes, n=6 each — treat as a lead, not a settled result)

RTSP main (ch0) latency was also measured under different combinations of
*other* concurrently-connected consumers on the same camera:

| condition | mean |
|---|---|
| ch0 alone | ~1.1-1.2 s (two batches: 1204 ms, 1121 ms) |
| ch0 + MJPEG consumer | 1074 ms |
| ch0 + sub-stream consumer | 967 ms |
| ch0 + fMP4 consumer | **430 ms** |

**An HTTP fMP4 client on the same channel cut RTSP main's own PTZ latency
by roughly 700 ms**, in a probe designed only to check for a consumer-mix
effect at all, not to explain one. This is the single most interesting —
and least understood — number in this whole pass. Candidate directions
for the code investigation (genuinely candidates, not conclusions):

- Something in timps' fan-out/drain path (hub.c, fanqueue.c, or the
  rtsp.c / httpd.c consumer loops) that behaves differently when an HTTP
  consumer is attached vs. when it isn't — e.g. a wake/poll cadence, a
  buffering threshold, or a scheduling side-effect where a second
  consumer's read pattern changes how promptly the *first* consumer's
  queue gets drained.
- Could equally be an artifact of the small n (6) and the alone-condition
  being measured first/last in a way that correlates with a warm-up or
  thermal/CPU-load state — this needs a real, controlled A/B before
  trusting the 700 ms figure as real and not noise. n=6 is not enough to
  rule out chance for an effect this size, even though it's suggestive.

## Open questions for the code investigation

1. Where does each stream type's latency actually get spent? Break down
   sensor/ISP → encode → hub fan-out → per-protocol muxing/packaging →
   network write, per stream type, ideally with real timestamped debug
   instrumentation (behind a compile-time or runtime flag, not left on by
   default) rather than more black-box measurement from outside.
2. Is the RTSP-sub vs RTSP-main ~700 ms gap really the GOP/bitrate
   difference, or something else in how the sub-stream's encoder or its
   consumer loop is configured/scheduled?
3. Is the "fMP4 consumer speeds up RTSP main" effect real and
   reproducible under a proper controlled test, and if so, what in the
   code causes it? If real, does it point at something suboptimal in the
   *normal* (no fMP4 consumer) path that could be fixed to get that
   speed-up unconditionally?
4. Why is MJPEG's actual frame rate 5, not the nominal/configured 25?
5. Given all of this, what — if anything — is actually reducible without
   trading away image quality, CPU budget, or reliability? Not every
   millisecond found here is necessarily fixable or worth fixing; the
   investigation should say plainly which latency sources look like real
   headroom vs. which look like fundamental cost (sensor readout time,
   ISP pipeline depth, etc.).

## Next step

A Fable-model agent will investigate this in the actual timps source
(not just this document) on cam-garage, adding real debug/timing
instrumentation where needed to pin down where each stream's latency is
actually spent, and assess what (if anything) can be safely reduced.
That work — and its findings — belongs in a separate, later dev_notes
entry once it's done; this document is the measurement baseline it starts
from, not the investigation itself.
