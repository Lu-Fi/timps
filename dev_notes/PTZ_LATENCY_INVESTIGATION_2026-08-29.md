# PTZ latency investigation — 2026-08-29

Code-level follow-up to `dev_notes/PTZ_LATENCY_2026-08-29.md` (the black-box
measurement pass). Same hardware: cam-garage (Wuuk Y0510, T31X/sc4336p,
`192.168.10.21`), base build `v1.9.3-60-g2a19efd`, video0 1200 kbps @ 15 fps,
video1 384 kbps CBR gop=50 @ 15 fps, both H.264 High. All work below was done
with **Frigate disconnected and verified absent** (no established sessions on
554/8880 before starting — `ss`/`netstat` on the camera), which the measurement
doc flagged as an unchecked confound.

**Headline**: the camera itself is fast and uniform. Measured with a
decode-free client (raw RTSP/RTP parser + offline decode, see §3), the true
command-to-changed-frame-bytes-on-the-wire latency is **~150–210 ms for RTSP
main AND sub alike, under every consumer mix tested** — of which ~71 ms is SSH
dispatch + motor spin-up (parent doc's `motors -b` cross-check). Inside the
daemon the whole pipeline accounts for **~17 ms** (§2). Every large number and
every between-stream difference in the measurement doc — main 588 ms vs sub
1220 ms, the 1.1–1.3 s "alone" latency, the fMP4-consumer speed-up — is a
**client-side artifact of the measuring ffmpeg pipeline**, not camera latency.
The consumer-mix effect is real and 100% reproducible, but it toggles a
buffering pathology *in the ffmpeg client*, via stream content (§4).

Method/tooling: a `TIMPS_TRACE=1` debug build (the existing `USE_TRACE`
facility, `src/trace.h`) was run from `/tmp` on the camera (stock daemon
stopped; nothing flashed, `/etc/timps.conf` untouched — trace keys went into a
`/tmp/trace.conf` copy), extended with a temporary producer-side probe
(capture→encoder-output age per channel). All instrumentation and experiment
scaffolding was **reverted from the source tree after the investigation**; the
stock daemon was restored on the camera. Raw data + scripts:
`scratchpad/ptz2/` of this session (session-scoped, will be cleaned up;
everything needed to re-run is described here).

## 1. Experiment inventory

| exp | design | key result |
|---|---|---|
| 1 | RTSP main via ffmpeg; conditions interleaved round-robin ×8: `alone` / `+fMP4 sink` / `+equivalent dummy traffic` (camera-side `nc` feed, ~1.45 Mbit/s bursty at 15 Hz, **not** a hub consumer); parallel ping probe | alone med **1261 ms** (bimodal: 2/8 trials ~350), fmp4 med **351 ms** (8/8 fast), load med **1280 ms** (≈alone). RTT ~7–14 ms in *all* conditions → not network/radio; effect requires a *timps consumer*, not traffic |
| 3 | same, on the traced daemon, + `mainsub` condition (ffmpeg on ch0 + ch1); internal per-AU trace + producer probe | internals flat everywhere (§2); ffmpeg-alone 1.29–1.30 s, ffmpeg+fmp4 0.32–0.47 s, mainsub **main 1.28/1.31/1.28 vs sub 1.30/1.31/1.14 — paired equal**, no sub gap |
| 4 | decode-free raw RTSP client (hand-rolled DESCRIBE/SETUP/PLAY, TCP-interleaved, FU-A/STAP-A depacketizer); records per-AU **arrival time**, decodes the dump **offline**, maps changed-frame *index*→arrival time; conditions ×4 each | **alone ch0 202±14 ms, +fmp4 199±63 ms, mainsub ch0 171±50 / ch1 148±35 ms** — uniform, fast, sub ≤ main |
| 5 | ffmpeg client-flag matrix, single consumer (stock daemon): `base` (nobuffer/low_delay/probesize32k/analyzeduration0), `-threads 1`, `-reorder_queue_size 0`, `-max_delay 0`, `plain` (no flags, Frigate-like) | base 1298/1296, threads1 1315/1272, rq0 1292/1267, **md0 997/773** (partial), plain 1372/1368 — the artifact survives every standard low-latency knob |
| 6 | live `ffprobe -show_packets` (demux only, no decode), onset by packet-size jump | demux-level onset **173/166/176 ms** — matches the raw client; the ~1.1 s sits *after* the demuxer, in the client's decode/output stage |
| replay | both exp-4 ES dumps re-fed to the *identical* ffmpeg pipeline at paced 15 fps via pipe | both dumps decode with a ~4-frame standing delay — content alone doesn't reproduce it; the artifact needs the RTSP transport path |
| didr | daemon hacked (env-gated, temporary) to request a **second IDR** after a session's first video AU, reproducing the double-IDR session start that fMP4-attach causes | see §4 — clears the artifact for low-latency-flag clients (7/8), not for default-flag clients |

n per cell is small (2–8) but the effect sizes are ~900 ms against ~30–90 ms
trial-to-trial noise; ordering/warm-up confounds were controlled by strict
round-robin interleaving within each experiment.

## 2. Where the daemon actually spends time (measured from inside)

Traced build, per-AU instrumentation on live sessions (mask 0x1f,
threshold 1 ms → every AU logged), plus a temporary producer probe logging
`IMP_System_GetTimeStamp() − pack[0].timestamp` (both on libimp's own clock,
so the difference is base-free):

| stage | code | chn0 (main) | chn1 (sub) |
|---|---|---|---|
| FrameSource capture stamp → encoder bitstream available | `video_thread`, `src/hal/hal_ingenic.c` (PollingStream/GetStream loop) | avg **16.0–16.8 ms** (min ~15.3, max ~18.7) | avg **6.3–18.3 ms** (min ~4.0) |
| hub publish → consumer pop (`age`) | `src/hub.c` `hub_publish_take` → `src/fanqueue.c` (event-driven: `pthread_cond_signal` on empty→non-empty, `fanqueue.c:96-102`) | med **0.1 ms**, p95 1.0 ms (n=1450 AUs) | med **0.0 ms**, p95 0.3 ms (n=547) |
| pop → last byte to kernel (`send`), RTSP/TCP | `src/rtsp/rtsp.c:1283-1426` + batched `sendmsg` (`rtsp.c` sink_send / `net.c:net_sendmsg_all`) | med **0.3 ms**; transient max 173 ms on IDR-sized writes | med 0.1 ms, max 93 ms |
| fMP4 mux+send | `src/mp4/httpd.c:476-700` | age med 0.1 ms, send med 0.3 ms | — |
| consumer queue depth at pop | fanqueue `q=` | **0/64 essentially always** | 0/64 |

Total intra-daemon: **~17 ms** from capture timestamp to bytes handed to the
kernel. There is no queue standing anywhere; the fan-out is signal-driven with
zero backlog at 15 fps. Note the capture *stamp* is taken at FrameSource
dequeue, so sensor exposure+readout (≤1 frame interval, ≤67 ms at 15 fps) sits
in front of this and is fundamental cost, not code.

End-to-end raw-client budget (~150–210 ms) decomposes as: ~71 ms SSH+motor
start (parent doc) + physical motion becoming visible + ≤67 ms frame-boundary
quantization + ~17 ms capture→wire + ~5–10 ms network. Nothing here is
plausibly reducible except by raising fps (fleet-capped to 15 on purpose).

## 3. Q1/Q2 — per-stream breakdown, and why "sub is 700 ms slower" dissolved

The decode-free measurement (exp 4) shows **ch1 arrival latency ≤ ch0**
(148–186 ms vs 171–202 ms, paired). The measurement doc's 700 ms gap was two
client artifacts composed: in its 4-streams-concurrent design an fMP4 consumer
(chn0) was always attached, which put the *main*-stream ffmpeg client into its
fast state (§4) while the *sub* ffmpeg client stayed in the slow state. Re-run
paired with ffmpeg but **without** the fMP4 consumer (exp 3 `mainsub`), main
and sub are equally slow (1.28 s vs 1.30 s) — the gap is gone. The suspected
GOP/bitrate mechanism from the measurement doc is refuted: no encoder-side or
fan-out-side asymmetry exists at all (§2; the sub encoder emits *earlier* than
main, 6 vs 16 ms — smaller frames). The `videoN.buffers`/nrVBs and
`isp_ch0_pre_dequeue_time` angles were checked and are inert on this board
(pre_dequeue=0, both channels nrVBs=2, `fs_create`,
`src/hal/hal_ingenic.c:995-1022`).

fMP4 tracking main closely, and MJPEG's numbers, were already consistent in
the parent doc; the raw parser there (378 ms) vs ffmpeg (~500 ms) gap is the
same class of client-side overhead, MJPEG's being one ~200 ms frame interval
at its real 5 fps.

## 4. Q3 — the "fMP4 consumer speeds up RTSP main" effect

**Real, 8/8 reproducible under interleaved A/B/C (exp 1), and not what it
looked like.** Facts established:

- Equivalent-bandwidth dummy traffic does NOT reproduce it (load ≈ alone) and
  ping RTT is flat ~8 ms in all conditions → not WiFi power-save/radio wake,
  not network-layer. (The camera is on WiFi via ssv6158; this was explicitly
  the alternative to kill.)
- The daemon's internals are identical in fast and slow trials — age/send/
  queue-depth flat (§2). The extra ~900–1100 ms is not produced, queued, or
  delayed anywhere in timps.
- Demux-level packet arrival in the *same* client is fast (~170 ms, exp 6);
  decoded-frame output of the same session is ~1.3 s late → the standing
  delay lives between libavformat and the decoded-frame output of the ffmpeg
  CLI pipeline (ffmpeg 8.0.1, 16-core host). It is insensitive to `-threads 1`,
  `-reorder_queue_size 0`, `-fflags nobuffer`, `-flags low_delay`;
  `-max_delay 0` shaves ~300–500 ms but does not clear it. A default-flags
  client (≈Frigate's shape) shows the full ~1.37 s.
- **Trigger identified (empirical): the session's opening IDR pattern.** In
  every fmp4-condition session the client's stream begins with **two
  back-to-back IDRs** (~131 KB each, 0–30 ms apart) — the RTSP PLAY IDR
  request (`src/rtsp/rtsp.c:1204`) plus the fMP4 connection's own
  `hub_request_idr(chn)` (`src/mp4/httpd.c:385` at connect, `:441` after the
  audio warmup — the two land within 1–2 frame intervals of each other on the
  shared encoder). Every slow session begins with a single IDR. 3/3 fast
  sessions double-IDR, all slow sessions single-IDR in the exp-3 trace log.
- **Server-side reproduction**: a temporary, env-gated daemon hack requesting
  one extra IDR right after a session's first video AU goes out (making every
  join a double-IDR join) switches ffmpeg clients to the fast state:
  re-running the whole flag matrix against the didr daemon gave base 1316/456,
  `-threads 1` 450/459, `-reorder_queue_size 0` 524/408, `-max_delay 0`
  518/540 — 7 of 8 low-latency-flag sessions fast (vs 0 of 8 against the
  stock daemon), one `base` session still slow (the un-stick is a startup
  race, not a guarantee). **Default-flag clients stayed slow (1320/1385)** —
  their ~1.35 s appears to be ordinary default-buffering that no server-side
  content pattern clears.
- Replaying the recorded streams through the identical ffmpeg pipeline via a
  pipe does *not* reproduce the slow state (both dumps ~4-frame delay), so
  the pathology needs RTSP transport + content together. The exact libav
  mechanism (why a second IDR at start prevents ~19 frames of standing
  buffering downstream of the demuxer, ffmpeg 8.0.1) was **not** chased into
  libav internals — from timps' side the observable contract is characterized.

**Is there a timps fix here?** Nothing in timps is broken — a decode-free
client already gets ~170 ms unconditionally. Requesting a second IDR on every
RTSP join (the didr hack) demonstrably un-sticks ffmpeg-family clients and
costs one extra ~130 KB frame per join on the shared encoder — but it is a
content workaround for an incompletely-understood quirk of one client family
on one ffmpeg version, it is a race rather than a guarantee (1 of 8 sessions
stayed slow), and it does nothing for default-flag clients. Landing it
fleet-wide on that basis would be cargo-culting. It was reverted. If Frigate
(an ffmpeg consumer, with its own input-arg preset — neither "plain" nor our
"base" exactly) is confirmed to exhibit the same standing delay against our
cameras, this is the first thing to A/B — the hack is 8 lines in
`stream_loop()` (`src/rtsp/rtsp.c`, after the first video AU send:
`hub_request_idr(s->vchn)` once), and this doc plus the trace build reproduce
the whole measurement in an afternoon.

## 5. Q4 — why MJPEG runs at 5 fps, not 25

Read-from-code, confirmed by config: **5 fps is the configured default, not a
bug.** The piggyback JPEG channel that `/stream.mjpeg` serves defaults to
`jpeg_fps=5` (`src/config.c:283`); the throttle is `vc->jpeg_period = 1e6/jfps`
(`src/hal/hal_ingenic.c:2668-2669`), and on T31 (ENC_NEW_API) the HW JPEG
encoder channel itself is created at that rate, so it isn't even encoding
discarded frames (`jpeg_enc_create`, `src/hal/hal_ingenic.c:3049-3058`).
"Nominal 25" in the measurement doc was a misreading — nothing configures
MJPEG at 25; the *video* fps (15 here) is a different channel. `jpeg_fps` is a
config-file-only key (`F_NOGET`, no `F_CTRL` — `src/config.c:1024`), so
raising it means editing `/etc/timps.conf` (`videoN.jpeg_fps`) and restarting;
cost is VPU work + ~5×/s JPEG bytes per fps step. At 5 fps, MJPEG's ~500 ms
measured latency is ~200 ms quantization + client overhead on top of the same
~170 ms camera latency; fundamental to the cadence, not to the path.

## 6. What was fixed / not fixed

**Fixed: nothing** — that is the finding, not an omission. The daemon's
pipeline has no material latency to remove (~17 ms internal, event-driven
fan-out, zero standing queues). All investigation scaffolding (the
`MS_TR_ENC` producer probe in `trace.h`/`hal_ingenic.c`, the didr hack in
`rtsp.c`) was reverted; the camera runs the stock `v1.9.3-60-g2a19efd` binary
and config again (verified post-restore), and the firmware build tree was
rebuilt without `TIMPS_TRACE` so nothing debug-flavored feeds the paused
fleet build.

**Found but deliberately not "fixed":**
- The ffmpeg-client standing delay + double-IDR workaround (§4): client-side
  pathology, workaround reverted, documented for a Frigate-side follow-up.
- One 3.67 s fMP4 outlier in the parent doc's data: consistent with a
  transient WiFi/TCP stall (the same capture shows send-side max spikes of
  100–300 ms on IDR writes); not reproduced, not chased.
- `jpeg_fps=5`: a deliberate default; raising it is a per-camera config
  decision, not a code change.

## 7. Verdict — is further work worth it?

**On the camera side: no.** The pipeline is ~17 ms; the remaining ~150–200 ms
end-to-end is motor spin-up, optics/exposure, one frame interval of
quantization at 15 fps, and the network — fundamental cost at this frame
rate. The single biggest lever on perceived joystick latency would be raising
fps (67→40 ms/frame at 25 fps), which was deliberately capped fleet-wide.

**On the consumer side: possibly, and cheaply.** If PTZ-follow or
motion-latency through Frigate matters, verify whether Frigate's ffmpeg
exhibits the same post-demux standing delay against our cameras (its symptom:
detections/streams lag a stable ~1 s that vanishes while a second fMP4/HLS
consumer is attached to the same channel). If yes: the double-IDR join
un-sticks nobuffer-family clients (not default-flag ones), the usual
client-side low-latency options alone do NOT, and the winning combination in
this data was low-latency flags + a double-IDR join — where the fix lands
depends on which side Frigate's preset actually resembles. Anyone picking this up
should start from this doc's exp-4 raw client + the `TIMPS_TRACE=1` build
(`make CAMERA=... IP=... TIMPS_TRACE=1 rebuild-timps`, run from `/tmp` with a
config copy) rather than from ffmpeg-based black-box numbers, which this
investigation showed can be off by ~6× and invert stream-to-stream
comparisons.
