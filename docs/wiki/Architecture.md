# Architecture

## Overview

timps is a single multi-threaded process (`timpsd`). One **HAL backend**
owns the hardware (or its simulation) and publishes encoded access units
into a small in-process **hub**; independent protocol servers (RTSP, HTTP,
SRT) subscribe to the hub to fan frames out to clients. Encoding only runs
while at least one subscriber is attached to a given source — this
on-demand model is central to the design and shows up throughout the
codebase as `hub_active()`/`fs_use()`/`fs_unuse()` refcounting.

```
        ┌───────────┐      ┌────────────────────┐      ┌───────────────┐
sensor →│  ISP/AE/AWB│ →   │ FrameSource (fs_use)│ →   │  Encoder(s)    │
        └───────────┘      └────────────────────┘      │ (H.264/H.265/  │
                                    │                    │  JPEG piggy-  │
                                    ▼                    │  back)         │
                              ┌───────────┐              └───────┬───────┘
                              │  OSD/IVS  │                      │
                              │ (overlay, │                      ▼
                              │  motion)  │              ┌───────────────┐
                              └───────────┘              │  hub_publish  │
                                                          │ (hub.c)       │
                                                          └───────┬───────┘
                                                                  │ fan-out
                        ┌─────────────────────────────────────────┼──────────────────────┐
                        ▼                     ▼                   ▼                      ▼
                 ┌─────────────┐      ┌──────────────┐    ┌─────────────┐        ┌─────────────┐
                 │ RTSP server │      │ HTTP server  │    │ SRT (MPEG-TS)│        │ record.c /   │
                 │ (rtsp.c)    │      │ (httpd.c):   │    │ (srt.c)      │        │ timelapse.c  │
                 │ RTP/RTCP    │      │ fMP4/MJPEG/  │    │              │        │ (SD storage) │
                 └─────────────┘      │ snapshot/    │    └─────────────┘        └─────────────┘
                                      │ control/     │
                                      │ events       │
                                      └──────────────┘
```

Audio capture flows through the same hub (a dedicated audio source index)
to RTSP/HTTP/SRT/record consumers; the [Day/Night](Day-Night.md) thread and
[Motion Detection](Motion-Detection.md) sit alongside the video pipeline,
reading ISP state and IVS grid results respectively, and drive back into
the HAL and `/control` via small callback interfaces rather than by
touching hub internals directly.

## Startup sequence (`src/main.c`)

1. Parse `-c <config>` / `-v` / `-h`.
2. `config_load()` — parse the flat key=value file into `g_cfg` (see
   [Configuration Reference](Configuration-Reference.md)).
3. `config_sensor_finalize()` — auto-detect unset `sensor.*` fields from
   `/proc/jz/sensor/sensor0/*` (real hardware only).
4. `config_snapshot_boot()` — copies `g_cfg` into an immutable
   `g_cfg_boot` **before** any network thread (and therefore any
   `/control` POST) can run. This boot snapshot exists because several
   config sections (`video.N.*` encoder/geometry/codec, `sensor.*`) are
   restart-only: a later live edit must still describe/mux/record the
   stream the encoder was **actually started with**, not the newly-edited
   (but not-yet-applied) values. Every consumer that packetizes an RTSP
   SDP, muxes fMP4, records to SD, or builds an SRT PMT reads dimensions/
   codec/fps from `g_cfg_boot`, not the live `g_cfg`. The one documented
   exception is `video.N.rtsp_path`, which is read live because an RTSP
   `DESCRIBE` re-matches the request path on every request — see
   [Configuration Reference](Configuration-Reference.md#videon--per-stream-encoder-settings).
5. Seed `rand()` from `/dev/urandom` (non-secret UDP-port-pick users only —
   security-sensitive randomness, like RTSP nonces, goes straight to
   `/dev/urandom` in `auth.c`).
6. `hal_get()` — selects the compile-time HAL backend (`hal_ingenic` or
   `hal_sim`).
7. `USE_CONTROL` builds: generate the per-boot `/control` token
   (`auth_gen_token`) and, if `http.token_file` is set, publish it.
8. Install `SIGINT`/`SIGTERM` handlers *before* HAL init — a signal during
   a slow ISP bring-up now just clears the run flag and lets the normal
   teardown path run afterwards, with a 3-second watchdog `alarm()` that
   force-`_exit()`s if a second signal or a wedged vendor call prevents a
   clean shutdown.
9. `hub_init()` + register the HAL's IDR-request and activity callbacks
   with the hub.
10. `g_hal->init(&g_cfg)` then `g_hal->start(&g_cfg)` — brings up the
    ISP/sensor and creates (but does not yet *enable*) every configured
    stream's FrameSource/encoder/OSD/motion pipeline.
11. Optional audio-output bring-up (`USE_BACKCHANNEL`/`USE_PLAY`):
    `bc_configure()`, `speaker_configure()`, `speaker_start()`.
12. Start the protocol servers: `rtsp_start()`, `httpd_start()`,
    `daynight_start()` (`USE_DAYNIGHT`), `record_start()`, `timelapse_start()`,
    `srt_start()`.
13. `while (g_run) sleep(1);` — the main thread does nothing but wait;
    all real work happens on the threads spawned above.
14. On shutdown: stop protocol servers and the day/night/record/timelapse
    threads, stop the speaker, then `g_hal->stop()` tears down the HAL
    (IVS → threads → IMP objects → sensor → ISP, in dependency order).

## The HAL abstraction (`src/hal/hal.h`)

```c
typedef struct {
    const char *name;
    int  (*init)(const ms_config *cfg);
    int  (*start)(const ms_config *cfg);
    void (*request_idr)(int src);
    void (*set_active)(int src, int on);
    void (*stop)(void);
} hal_backend;
```

Two backends implement this interface, selected at compile time:

- **`src/hal/hal_ingenic.c`** (`-DHAL_INGENIC`) — the real hardware
  backend, talking to the vendor `libimp` SDK: `IMP_ISP`/sensor,
  `IMP_FrameSource`, `IMP_Encoder`, `IMP_OSD`, `IMP_IVS`, `IMP_AI`/`IMP_AO`.
  ~2800 lines; the single largest file in the project.
- **`src/hal/hal_sim.c`** — the host-simulation backend used by
  `make sim`. Feeds pre-recorded files instead of live hardware: loops a
  raw Annex-B H.264/H.265 file (`sim.video0`/`sim.video1`) NAL-by-NAL,
  paced to the configured fps; loops an ADTS AAC file (`sim.audio`),
  paced to 1024 samples/hop; and republishes a static JPEG
  (`sim.jpeg`) at `jpeg.fps`. It mirrors the same on-demand
  activation contract as the real HAL (each producer thread blocks until
  it has subscribers) and the same JPEG-source layout (dedicated +
  per-stream piggyback), so protocol/control code paths are exercised
  identically to a real camera. `hal_isp_total_gain()`/`hal_isp_ae_luma()`
  always report "unavailable," so [day/night](Day-Night.md) falls back to
  its proc-scrape path, which is also absent on a host and simply idles.

Two extra HAL entry points exist outside the `hal_backend` struct for
day/night and speaker support:

- `hal_isp_total_gain()` / `hal_isp_ae_luma()` — read the ISP's own gain/AE
  luminance registers directly (preferred over scraping
  `/proc/jz/isp/isp-m0`); return `<0` when unavailable (sim, T40/T41's new
  tuning API, or ISP not initialized).
- `hal_ao_open/write/close/set_vol/set_gain` (`USE_BACKCHANNEL`/`USE_PLAY`
  builds) — the speaker (`IMP_AO`) primitives, exclusively owned and
  arbitrated by `src/rtsp/speaker.c` (see [Audio](Audio.md)).

### On-demand FrameSource activation (`hal_ingenic.c`)

A framesource channel is only actually `IMP_FrameSource_EnableChn`'d on
the 0→1 edge of a per-channel refcount (`fs_use()`/`fs_unuse()`), and
`DisableChn`'d only on the 1→0 edge. Leaving a channel permanently enabled
keeps the whole bound pipeline pumping frames at sensor fps in libimp's
own worker threads even with zero subscribers — measured at roughly 19%
idle CPU with no clients — so idle-stopping it is the main lever behind
timps's near-zero idle CPU claim. The refcount is what lets a video
stream, its piggybacked JPEG encoder, and motion detection all safely
share one FrameSource: whichever of them needs frames bumps the ref, and
the channel only truly stops once none of them do.

Per-stream `video_thread()` loops implement the on-demand start/stop
policy: `want = active_flag || hub_active(chn)`; when idle they block on a
condition variable (`act_wait()`) instead of polling, woken immediately by
`hub`'s activity callback the moment a subscriber (or the `set_active`
API) appears. A debounce (`MS_IDLE_STOP_US`, ~2s) delays the actual
Stop/Disable so a client that reconnects within that window doesn't pay
the full pipeline teardown/rebuild cost. A watchdog (10 consecutive
`PollingStream` misses) forces a Stop/Disable/Enable/Start recovery cycle
if the encoder appears to have wedged.

## The hub (`src/hub.c` / `src/hub.h`)

The hub is a small, fixed-size pub/sub registry: one `hub_source` per
video stream, one shared audio source (`HUB_AUDIO_SRC`), and a set of
JPEG sources (a dedicated `jpeg.*` channel plus one optional piggyback
per video stream). It has exactly one producer thread per source (the
HAL's video/audio/JPEG threads) and up to `HUB_MAX_SUBS` (16) subscriber
queues per source.

- **`hub_publish(src, data, len, pts_us, keyframe, media)`** — called by
  the HAL for every encoded access unit. Under the source's lock it
  updates cached SPS/PPS/VPS parameter sets (`vparam`) and measured
  fps/bitrate, then **snapshots the subscriber list**. The actual
  refcounted packet allocation (`pkt_new`, a malloc + full-frame copy up
  to ~1MB) happens *after* releasing the lock, and — critically — is
  **skipped entirely when there are zero subscribers**, so a source can
  keep "publishing" through the idle-stop debounce window at effectively
  no cost.
- **`hub_subscribe`/`hub_unsubscribe`** — callers supply their own
  [fanqueue](#fan-out-queues-fanqueuec); the hub tracks a per-source
  busy flag (`g_pushing[]`) so an in-flight `hub_publish()` push that
  already snapshotted a queue is guaranteed to finish before
  `hub_unsubscribe()` returns — otherwise a caller that immediately frees
  its fanqueue after unsubscribing could race a use-after-free.
- **Activity callback** — `hub_set_activity_cb()` lets the HAL learn when
  a source's subscriber count crosses 0↔1. Notifications are
  **level-based, not edge-based** (recomputed from the current `nsub` at
  the moment the callback actually runs, serialized by a dedicated lock),
  so a fast subscribe-then-unsubscribe race can never deliver a stale
  "stop" after a "start."
- **Control plumbing** — `hub_control(key, val)` is a thin pass-through
  the `/control` layer uses to hand a parsed `section.key`/raw-value pair
  to whatever HAL callback is registered (`hub_set_control_cb`), and
  `hub_control_commit()` lets the HAL batch an expensive rebuild (e.g. the
  motion-detection IVS grid) so it only runs once per `/control` request
  even if several of that request's keys would each trigger it.

## Fan-out queues (`src/fanqueue.c` / `src/fanqueue.h`)

Each subscriber (an RTSP session's video/audio sink, an HTTP `/stream.mp4`
client, the recorder, ...) owns one `fanqueue`: a bounded ring buffer of
refcounted packet pointers with **two independent overflow limits** — a
slot-count cap and a total-queued-bytes cap (`FQ_MAX_BYTES`, 2 MB by
default). The producer (`fanqueue_push`) never blocks: on overflow it
drops from the **head** (oldest first), and once a dropped packet was a
video keyframe it keeps dropping forward through the now-headless GOP
(non-keyframe video packets), leaving interleaved audio packets alone,
until a fresh keyframe reaches the head. This guarantees a slow client can
never stall the shared encoder and never pins unbounded memory, while
self-healing as soon as the stream naturally reaches its next keyframe.

Read-and-clear flags, reported by `fanqueue_pop_ex()` together with the
packet they were raised behind, let a consumer detect and react to loss:

- `fq_status.dropped_key` — a keyframe was evicted; the consumer
  should request a fresh IDR from the hub source.
- `fq_status.dropped_any` — *any* packet was evicted (including a
  P-frame, which silently corrupts the rest of that GOP for a
  frame-by-frame consumer just like a lost keyframe, but leaves no
  keyframe to trip the first flag).
- `fq_status.dropped_video` (**since v1.9.19**) — the eviction
  hit a *video* packet, key or not. This is the flag the heal paths act on:
  consumers that decode every frame (RTSP, SRT, WebRTC) request an IDR here,
  rate-limited, since an IDR request is a shared, global cost across every
  other subscriber of that source, and the recorder/fMP4 freeze on it until
  the next keyframe. `dropped_any` alone could not say that — an evicted
  *audio* packet raises it too, and discarding a GOP of decodable video over
  one lost audio frame helps nobody. An audio-only eviction still counts as
  an overflow (`queue_drops`, the summary WARN) and still feeds the fMP4
  mute-vs-congestion check.

**Since v1.9.19** that rate limit is no longer each consumer's
own. A consumer healing an overflow calls `hub_request_idr_recovery(src)`
instead of `hub_request_idr(src)`, and the hub enforces
`HUB_IDR_RECOVERY_MIN_US` (1 s) **per video stream** across every consumer, so
*N* slow consumers can no longer cost *N* IDRs/s on the one shared encoder — a
forced IDR at CBR is the largest frame there is, so it lengthened every other
consumer's queue and provoked the next round of drops. A request that loses the
race is *coalesced*, not dropped: it is remembered and issued by the next
published frame once the interval has passed, so a consumer frozen until its
next keyframe always gets one even if it never asks again — unless a video
keyframe is published first, which cancels the coalesced request, because that
keyframe is what the consumers were waiting for and a second forced IDR would
land in queues that had just recovered. Requests a client
needs to **start** decoding (subscribe, RTSP `DESCRIBE`/`PLAY`, a fresh fMP4
`GET`, the WebRTC answer) keep using `hub_request_idr()` and are never delayed.

**Since v1.9.20 (unreleased)** RTSP, SRT and WebRTC no longer keep a 1 s gate
of their own in front of that call: a gate there discarded any request inside
its window instead of letting the hub coalesce it, so a P-frame lost within a
second of the previous request stayed unhealed until the next natural
keyframe. While a request is pending, calling again costs nothing (no clock
read, no lock), which is what lets a frozen consumer ask per packet.

Evictions are counted by the hub itself, in its fan-out push (**since
v1.9.20, unreleased**): a consumer registers its queue with
`hub_count_drops(q, src, kind)` before subscribing, and every published packet
that has to evict from that queue counts once against video stream `src`,
audio included. Until then each consumer reported its own drops when it next
popped a packet, so a peer wedged in `send()` - which never pops again - was
never counted at all, and RTSP/SRT/WebRTC counted only video evictions. The
`kind` (`HUB_DROP_REC`/`RTSP`/`MP4`/`WEBRTC`/`SRT`) drives a WARN summary of at
most one line per 60 s per (kind, stream); an open window is also flushed when
its stream loses its last subscriber, so an on-demand stream that idle-stops
does not hold the line back until the next viewer. The counter behind it is
`queue_drops` in `GET /control`. A queue never registered (a JPEG grab's helper
queue, which overflows by design) is not counted. There is no config key for
either interval.

`fanqueue_depth()` lets a consumer inspect its own backlog (used by
`http.adaptive_drop` in the HTTP fMP4 path to freeze-and-resume-at-keyframe
a single slow client without touching any other subscriber or the shared
encoder — see [Streaming Protocols](Streaming-Protocols.md)).

## Reference-counted packets (`src/frame.c` / `src/frame.h`)

The unit the hub and every fanqueue move around is `ms_pkt` — a small
refcounted struct (`data`/`len`/`pts_us`/`keyframe`/`media`/`_ref`). One
`pkt_new()` allocation and copy happens per access unit in
`hub_publish()`, then a `pkt_ref()` is taken for each subscriber's queue
push, so N subscribers share one payload copy instead of N. `enum
ms_media` distinguishes `MS_MEDIA_VIDEO` / `MS_MEDIA_AUDIO` /
`MS_MEDIA_JPEG`.

## Protocol servers

Each protocol server is an independent module that subscribes to the hub
and translates packets into its own wire format — none of them know about
each other or reach into the HAL directly:

| Server | File(s) | Subscribes to | Notes |
| --- | --- | --- | --- |
| RTSP | `src/rtsp/rtsp.c` + `rtp.c` | video/audio sources per session | own from-scratch RTSP/RTP/RTCP implementation, no live555 |
| HTTP | `src/mp4/httpd.c` + `mp4/fmp4.c` | video/audio/JPEG sources | fMP4 preview, MJPEG, snapshot, plus (`USE_CONTROL`) `/control` and `/events` |
| SRT | `src/srt.c` | one video (+audio) source | hand-rolled MPEG-TS mux, `USE_SRT` builds |
| Recorder | `src/record.c` | one video (+audio) source | reuses the fMP4 muxer, writes segments to SD |
| Timelapse | `src/timelapse.c` | a JPEG source | just-in-time capture, releases the source between shots |

See [Streaming Protocols](Streaming-Protocols.md),
[HTTP /control API Reference](HTTP-Control-API.md), and
[Recording & Timelapse](Recording-Timelapse.md) for the details of each.

## Configuration and control-plane concurrency

`g_cfg` (the live, mutable config) is read from many threads
concurrently (protocol servers, the OSD updater, the recorder, the
day/night thread) and written from `/control` POST handling. Plain
`int`/`enum`/`float` fields are read/written as aligned words with no
tearing concern; a handful of **string** fields that are genuinely
runtime-mutable (OSD text, `video.N.rtsp_path`, `sensor.model`,
`record.dir`/`name`, `timelapse.dir`/`name`, `daynight.time_*_start`) are
guarded by a dedicated **recursive** mutex (`config_str_lock`/
`config_str_unlock` in `config.c`) so a reader never observes a
torn/non-NUL-terminated string mid-write. It's recursive specifically
because `/control`'s change-detection (`timps_apply_setting()`) wraps a
whole get-apply-get sequence in the same lock.

Persisting a `/control` change back to `/etc/timps.conf` goes through
`config_write_keys()`, which rewrites only the changed lines in place
(preserving comments/order/unknown lines), appends genuinely new keys, and
commits atomically via a unique `mkstemp()` temp file + `fsync()` +
`rename()` + directory `fsync()` — chosen specifically because these
camera SD/flash filesystems (jffs2/ubifs) are not guaranteed durable until
`fsync`'d, and `/control` POSTs can arrive in rapid bursts (a dragged
slider), so a global write-lock also serializes concurrent writers to
avoid two racing POSTs corrupting the file mid-line.
