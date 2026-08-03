# Changelog

All notable changes to timps are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/), and the project aims to follow
semantic versioning.

## [Unreleased]

## [1.7.5] - 2026-08-03

### Fixed
- **Day/night thread could get permanently stuck at boot with zero self-
  healing.** From `DN_UNKNOWN`, the decision silently stays put while gain
  sits inside the day/night dead-zone (300..3000 default) — by original
  design. But a camera can boot with the ISP already in a *persisted* mode
  and a dead-zone reading (e.g. a restart in daylight with a stale night
  config), and both self-healing probes (periodic reconfirm, sustained
  brightening) are gated on `cur==DN_NIGHT` — which `DN_UNKNOWN` never
  satisfies. Result: a camera could render night video (or day, in the
  inverse case) indefinitely after a reboot, with a perfectly healthy
  thread producing zero log lines, only discovered live on a T31 that
  restarted at 09:23 in broad daylight and stayed dark for hours.
  Once the boot-settle window ends still undecided, the thread now adopts
  the persisted `image.running_mode` as its internal state (the ISP is
  already running it, so nothing switches) so the normal in-mode triggers
  and probes arm. Since an adopted night is a guess rather than a
  measurement, its first day-pipeline verify probe fires within 5 minutes
  (or sooner if `night_reconfirm_s` is set lower) — once, even when
  periodic reconfirm is disabled. Pre-existing gap, not a v1.7.4
  regression; v1.7.4 only happened to be the build running when a restart
  finally landed in the dead-zone. Verified in `timpsd-sim` replaying the
  exact incident (adopts, verifies, and reaches day in ~25s versus
  indefinitely stuck before) plus a one-shot-probe-with-reconfirm-disabled
  case and two clean (non-dead-zone) boots showing zero adoption noise.

## [1.7.4] - 2026-08-03

### Fixed
- **Day/night baseline drift ratcheting into an overnight flap loop.** The
  v1.7.3 hardening's upward-only EMA baseline drift tracked raw gain ticks;
  noisy night AGC ratcheted the baseline to its noise ceiling, causing
  night↔day to flap every few minutes to every hour, all night, on real
  cameras — worse than the bug it was meant to fix. Replaced with a
  night-only smoothed gain driving a slow, symmetric baseline drift, an
  edge-armed brightening probe that disarms after a failed attempt, and an
  8s post-probe AE-stability gate so a lit room's exposure-convergence
  transient can't kill a legitimate probe. Verified against the exact
  logged flap pattern in `timpsd-sim`: zero flaps over 3 minutes where
  v1.7.3 flapped every 1-2 minutes.

## [1.7.3] - 2026-08-02

### Fixed
- **Adaptive night→day threshold too strict for a real light source.** Two
  live incidents: a basement whose only light dropped gain to 65% of a
  cleanly-sampled night baseline (never crossing `day_gain_pct`'s 60% bar),
  and a room whose baseline was sampled mid-lighting-transition
  (unrepresentatively low). The trigger is now floored at
  `total_gain_day_threshold`, the baseline drifts toward observed gain
  instead of staying fixed, and a "sustained brightening" probe forces an
  early day-pipeline recheck instead of waiting up to `night_reconfirm_s`.
  (Superseded by the fix in 1.7.4 above once this introduced its own
  regression.)
- `night_baseline`/`day_trigger` (the adaptive values currently in effect)
  are now exposed read-only in `GET /control` and the `/events` SSE push.

## [1.7.2] - 2026-08-02

### Fixed
- `boot_settle_s`/`boot_settle_max_s`/`boot_stable_pct`/`night_reconfirm_s`
  (new in 1.7.1) were live-settable but never actually appeared in the
  `GET /control` status JSON — a separate hand-written serializer had its
  own hardcoded field list, unrelated to the settings path.

## [1.7.1] - 2026-08-02

### Fixed
- **False night-mode latch surviving a reflash into broad daylight.** A
  fixed 5s post-boot settle window was too short for a cold/freshly-
  reflashed sensor's AE to converge, so a transient gain spike could
  commit straight to night regardless of real daylight and never recover.
  `boot_settle_s`/`boot_settle_max_s`/`boot_stable_pct` now wait for
  several consecutive gain readings to actually stabilize before trusting
  the first decision, and a new `night_reconfirm_s` periodically forces a
  real day-pipeline probe so an already-latched false night self-heals
  instead of requiring a manual `/control` override.

## [1.7.0] - 2026-08-02

### Added
- **HTTP Digest authentication** (RFC 7616 `qop=auth` + legacy RFC 2069)
  alongside the existing Basic auth, for both the HTTP preview endpoints
  and RTSP.
- **Read-only encoder telemetry** via `IMP_Encoder_Query`: per-channel
  queue/buffer stats (`registered`, `left_pics`, `left_stream_bytes`,
  `left_stream_frames`, `cur_packs`, `work_done`) and, on T31,
  `ave_bitrate` from `IMP_Encoder_GetChnAveBitrate`, exposed as a new
  `"encoder"` object in `GET /control`.
- **Per-client adaptive fMP4 frame-dropping** on weak links: a slow
  `/stream.mp4` client freezes on its last frame and resumes cleanly at
  the next keyframe instead of stalling every subscriber, with drop stats
  (fps/kbps/resolution/drops) visible in status. Defaulted on once
  hardware-verified.
- **Live IVS motion sensitivity** via `IMP_IVS_SetParam` (no grid rebuild
  needed for a sensitivity-only change), and opt-in AEC
  (`IMP_AI_EnableAec`) for the backchannel.
- `USE_RECORD`/`USE_TIMELAPSE` compile-time flags to shrink SD-less builds;
  `{fpsN}`/`{bitrate}` OSD placeholders for a specific stream's measured
  throughput.

### Changed
- **`rotation=180` removed on classic-API SoCs** (T10/T20/T21/T23/T30/T31/
  C100), since it's mechanically identical to `image.hflip`+`image.vflip`
  there — then **restored specifically for T40/T41**, which have a genuine
  per-channel I2D-based 180° distinct from their (global) hflip/vflip
  registers. Final `caps.rotation`: T31 `[0,90,270]`, T40/T41
  `[0,90,180,270]`, no-rotation SoCs `[0]`.
- `sendmmsg`-batched UDP video RTP per access unit, table-driven
  `/control` key lookup (~17KB smaller `.text`), explicit per-thread-type
  stack sizes, and a just-in-time timelapse hub subscription (was held
  24/7) — all throughput/footprint work with no behavior change.

### Fixed
- **T31 FS-rotate / T23 SW-rotate crash safety.** A rotation request
  outside the vendor-safe envelope (64-aligned & ≤1280x704 & ≤15fps for
  T31; 16-aligned & ≤704x576 & ≤15fps for T23) used to silently fall back
  to an oversized/misaligned software path that then failed encoder
  bring-up and took the **entire multi-stream pipeline** down — reproduced
  live via a rotation the `/control` API had itself accepted and
  persisted. Both platforms now refuse an out-of-envelope rotation and
  bring that one stream up unrotated instead; a `SetChnRotate`/
  `YuvInit` failure is likewise isolated to the affected stream rather
  than aborting the whole daemon.
- **A batch of RTSP/RTP/RTCP/SDP conformance fixes** found across several
  review rounds: `SET_PARAMETER` answered as a keepalive (200, RFC 2326
  §10.9) instead of 405; idle TCP backchannel-only sessions reaped after
  the standard timeout (previously immortal, since they never trip the
  media-write timeout other TCP sessions rely on); `Content-Length`
  request bodies actually consumed so the byte stream stays framed;
  `rtsps://` scheme stripped so TLS clients resolve the right stream;
  unsupported `Require:` feature-tags answered 551; Digest `uri=`
  verified against the real request-target; orphaned UDP sessions reaped
  at 2x the advertised timeout; `CSeq` echoed on every error response;
  `HEAD` answers `GET`'s headers with no body (RFC 7231 §4.3.2); plus
  fixes for SDP truncation/`Content-Length` mismatches, `FD_CLOEXEC` on
  accepted sockets, and several `/control` JSON-encoding hardenings
  (control-char/UTF-8 handling, `\uXXXX` decoding, failing closed instead
  of shipping truncated JSON).
- **Motion detection**: IVS grid now uses pre-rotation frame dimensions
  with a transposed grid on the T23 SW-rotate path (was building the grid
  in the wrong orientation), sensitivity changes that map to the same IVS
  level are deduped, and `cooldown_ms` is floored and persisted correctly.
- **Day/night**: a queued ISP `running_mode` change is now actively
  latched (`fs_kick_running_mode`) instead of only taking effect on the
  next unrelated encoder event; a pre-switch hysteresis window (raptor-
  style) replaced blind reassertion; a transient reading during the AE
  settle window is now ignored instead of seeding a false decision.
- **Recording/timelapse**: `record.audio` toggles the hub subscription
  live; a dropped packet (not just a missed keyframe) now requests a
  rate-limited IDR; `gethostname()` results are NUL-terminated before use
  in path templates.
- Video/JPEG AU buffers now size from the actual frame instead of a fixed
  estimate (fixes both the 1.6.1 sub-stream stall class and an analogous
  JPEG/snapshot/MJPEG stall once a scene crosses a detail threshold — see
  below), audio speaker/backchannel gating and resampling edge cases, and
  a `/control` re-POST of an unchanged `image.running_mode` now still
  re-drives the ISP (some SoCs need the write even when the value didn't
  change).

## [1.6.4] - 2026-07-29

### Added
- **`{bitrate}` OSD text placeholder.** Reports the live measured
  throughput (kbit/s) of the monitored stream, mirroring the existing
  `{fps}` placeholder's mechanism and style. Shows `0` when the encoder
  has no active consumer rather than a frozen last-seen value.

### Fixed
- **JPEG snapshot/MJPEG/WebUI preview going permanently dark once a scene
  crosses a detail threshold.** `jpeg_thread()`'s buffer starts at a
  ~0.5 byte/pixel estimate, bounded to `[MS_JPEG_BUF_MIN, MS_JPEG_BUF_MAX]`
  — same class of bug as the `[1.6.1]` AU buffer fix, but unlike a video
  frame, nothing here shrinks a JPEG scene back down once it crosses the
  estimate (e.g. daylight bringing out more detail than a dawn/dusk
  scene), so every frame overflowed and got dropped forever from that
  point on: snapshots returned "no frame", MJPEG and the WebUI preview
  went dark, while day/night switching and RTSP video kept working fine
  (different codec/buffer entirely) — easy to mistake for a day/night bug
  from the WebUI. Now sums the pack lengths before assembly and grows the
  buffer (bounded by `MS_JPEG_BUF_MAX`) to fit the real frame, same as the
  AU buffer. Verified on real hardware (Galayou Y4, T23n): `/snapshot.jpg`
  went from HTTP 503 "no frame" with continuous buffer-overflow log spam
  to a valid ~450KB daytime JPEG, no overflow since.

## [1.6.3] - 2026-07-28

### Fixed
- **1-3s browser preview lag behind the physical camera (noticeable during
  PTZ).** Two independent contributors on the encoder→browser path:
  - The embedded MSE player JS (`src/mp4/httpd.c`) only corrected its
    live-edge position once it drifted more than 6s behind, jumping back
    to just 1s behind even then. With `autoplay`, the browser starts
    playback wherever it first had enough buffered data (typically 1-3s)
    and then plays at a flat 1x forever — that initial gap never shrunk on
    its own. Replaced the dead-zone jump with active drain:
    `playbackRate` now scales 1.0→1.3x with how far behind live the
    player is, settling at a steady-state ~0.5s behind live (kept as
    jitter margin), with a hard seek reserved for a large post-stall
    drift (>4s).
  - The HTTP/fMP4 listener never set `TCP_NODELAY` (the RTSP listener
    already did) — Nagle's algorithm held small fragments until the prior
    write was ACKed, adding up to ~200ms of pure transport latency per
    fragment, compounding across every video/audio fragment sent.
  GOP size/B-frames/rate-control and the encoder polling loop were
  reviewed and ruled out: no B-frames are used (no look-ahead latency),
  the poll timeout only bounds idle-wait and never delays an
  already-produced frame, and the fanqueue has no steady-state queuing
  delay. GOP interval affects only startup/post-drop recovery, not
  in-progress PTZ framing, so it was left unchanged (shrinking it further
  would trade bandwidth/quality for no benefit here). Verified on real
  hardware (Cinnado D1 T31L x2).

## [1.6.2] - 2026-07-28

### Fixed
- **Video encoder permanent stall when a framesource enable silently
  fails.** `fs_use()` never checked `IMP_FrameSource_EnableChn()`'s return
  value, and the enable only fires on the 0→1 user-count edge. If it fails
  once — or "succeeds" without actually arming the channel, a failure
  class this file already documents twice (the AI watchdog, and the T31
  `nrVBs` case) — `video_thread()`'s `StartRecvPic` still reports success
  and `PollingStream` spins at `rc=-1` forever: the encoder never produces
  another frame. With a client still subscribed, the idle-stop debounce
  never fires, so the framesource never gets a fresh enable attempt — a
  permanent stall recoverable only by restarting the daemon. Observed live
  on a T31L main channel (`nrVBs=1`, i.e. no buffer slack) after streaming
  correctly for hours; the sub-stream (independent framesource, ≥2
  buffers) kept working the whole time. Now mirrors the existing AI
  watchdog for video: after `MS_VIDEO_WATCHDOG_ITERS` (~5s) consecutive
  `PollingStream` misses, force a real Stop/Disable/Enable/Start cycle
  instead of spinning; `EnableChn` failures are now logged instead of
  silently swallowed. Verified on real hardware (Cinnado D1 T31L): full QA
  pass (77 PASS / 0 FAIL) after the fix, including 20/20 clean TCP and
  20/20 clean UDP reconnect cycles through the previously-fragile
  enable/disable edge.

## [1.6.1] - 2026-07-28

### Fixed
- **Sub-stream permanent stall on a large (e.g. complex-scene) IDR frame.**
  `video_thread()`'s AU assembly buffer starts at a ~0.5 byte/pixel
  estimate, clamped to `[MS_AU_BUF_MIN 128KB, MS_AU_BUF_MAX 1MB]`. For a
  small sub-stream (e.g. 640x360) that estimate sits at the 128KB floor,
  which a complex-scene IDR can exceed. The overflow handler used to drop
  the oversized frame and force a fresh IDR — but an IDR is the *largest*
  frame type, so the forced replacement overflowed too, forced another
  IDR, overflowed again: a permanent self-reinforcing stall that delivered
  zero decodable video on that stream from the first oversized frame
  onward (found as "only one stream works" — main stayed fine on its
  larger 1MB cap). Now sums the pack lengths before assembly and grows the
  buffer (bounded by `MS_AU_BUF_MAX`) to fit the real frame instead of
  truncating it; the `IMP_Encoder_RequestIDR()` call on the (now
  last-resort, >1MB-AU-or-failed-realloc-only) overflow path is dropped,
  since forcing an IDR there was the actual cause of the stall — a dropped
  frame already recovers via the existing
  `fanqueue_take_dropped_key`/`hub_request_idr` path on a real client.
  Verified on real hardware (Galayou Y4, T23n): the sub stream went from
  zero video across 20+ minutes and every reconnect to streaming
  correctly (640x360 h264@25+aac, IDR ~99KB) alongside the main stream,
  with zero overflow events since boot.

## [1.6.0] - 2026-07-27

### Added
- **Native speaker output (`IMP_AO`) — timps now owns the camera speaker
  directly, no more `/bin/iac`.** New `src/rtsp/speaker.c` is the sole
  `IMP_AO` owner and arbitrates two producers: the ONVIF **backchannel**
  (live RTP → PCM, always preempts) and a **system-sound play queue**
  driven by a FIFO at `/run/timps/audio_out` taking `PLAY url=<path>
  [vol= gain= rate= format= loop= delay=]` / `STOP` lines — the same
  protocol prudynt/raptor's `/usr/sbin/play` wrapper already speaks, so
  the WiFi captive-portal prompts, the post-upgrade chime and the ESPHome
  `media_player`/TTS integration all get a working speaker on a timps
  image for free. The play queue decodes Ogg-Opus (`opusfile`, gated on
  new `USE_PLAY_OPUS` like `USE_BC_AAC` gates the AAC backchannel), WAV,
  raw PCM16 and G.711 µ/A-law. New `USE_PLAY`/`USE_PLAY_OPUS` build flags
  (off by default; `USE_BACKCHANNEL` no longer needs `/bin/iac` present at
  all — `bc_available()` is always true once built in). New
  `hal_ao_open/write/close/set_vol/set_gain` in the HAL mirror the
  existing `IMP_AI` bring-up (rate-fallback loop, lazy open/close); a new
  `src/codec/resample.c` (extracted from `backchannel.c`) is shared by
  both producers.
- **Live speaker volume/gain + WebUI-driven system-sound play.**
  `audio.spk_volume`/`spk_gain` were parsed and persisted but never
  actually reached the hardware before; now every `IMP_AO` open applies
  them, and `POST /control {"audio":{"spk_volume":..,"spk_gain":..}}`
  writes through live (`caps.audio` gains the two keys, gated on an AO
  pipeline being compiled in). `GET /control` gains
  `caps.play={available,sounds:[...]}`, enumerated from
  `/usr/share/sounds` (`.wav` always listed since the µ-law/PCM decoder
  needs no library; `.opus` only when `USE_PLAY_OPUS` is actually built,
  so the list never offers a file this exact build can't decode).
  `POST {"speaker":{"play":"<file>"}}` / `{"stop":1}` enqueues on the FIFO
  after validating the name against that directory (rejects `/`, `..`,
  non-regular files) — this is what drives the thingino WebUI's
  test-sound dropdown and live speaker volume slider.
- **Day/night: time-window and sunrise/sunset override modes.** The
  native detector could previously only decide from the ISP sensor
  (`total_gain`/brightness). `daynight.mode` (`sensor`/`time`/`sun`, a
  string token) adds two sensor-independent modes: **`time`** forces by
  the local wall clock — a fixed `[time_night_start .. time_day_start]`
  `"HH:MM"` window, wrapping past midnight (e.g. night 20:00, day 06:30).
  **`sun`** forces by today's real sunrise/sunset for
  `sun_latitude`/`sun_longitude` via the standard low-precision sunrise
  equation (pure math, UTC epoch throughout), each edge shiftable by
  `sun_sunrise_offset_min`/`sun_sunset_offset_min` (negative allowed);
  polar day/night degenerate cases fall back to permanent day/night
  instead of NaN. `sensor` stays the default and its gain/brightness
  branch is untouched; `time`/`sun` reuse the existing switch + minimum-
  dwell machinery, and `daynight.enabled=0` (manual) still suppresses
  forcing in all three modes. `GET /control` exposes the new config
  fields plus today's computed sunrise/sunset (`sun_computed_sunrise`/
  `sunset`, local `"HH:MM"`) so a UI can sanity-check the configured
  lat/long before trusting it.
- **T31(L) `nrVBs` buffer-count override** (`video.buffers`, raptor-style):
  the T31 non-scaled-channel safety clamp (see Fixed below) now only
  applies to the *default* buffer count — an explicit `buffers=` in
  `timps.conf` is trusted as-is (with a warning, since a bad value fails
  silently down in the kernel/dmesg), letting a board/sensor combination
  that doesn't hit the constraint opt out without patching code.

### Fixed
- **Play-file tail no longer cut short on normal end-of-clip.** Two
  layered bugs, both in `hal_ao_close()`'s drain path: (1) it
  unconditionally discarded the AO ring buffer (`IMP_AO_ClearChnBuf`) on
  every close, including a clip finishing normally, not just on
  stop/preempt — a `drain` flag now distinguishes the two, discarding
  immediately only on stop/preempt/backchannel-takeover. (2) the drain
  path's fixed sleep (one `MS_AI_FRM_NUM`-period ring's worth, ~0.24 s)
  assumed that was the whole story, but the IMP AO keeps its own
  playback cache on top of that ring — the real residual is ~0.7 s, so
  the fixed sleep still closed the channel ~0.5 s early (e.g.
  "Configuration portal is down" stopped after "portal"). Now uses
  `IMP_AO_FlushChnBuf`, the SDK's "wait for the last segment to finish
  playing" primitive, which blocks until the whole cache has actually
  reached the DAC regardless of depth. Verified acoustically via RTSP
  mic loopback across clips from 0.6 s to 2.7 s, before/after audible
  span matched against each source clip's real content window.
- **T31(L) `nrVBs=1` clamp scoped to `PLATFORM_T31` only.** The non-
  scaled-channel buffer-count safety clamp in `fs_create()` (shared
  across every SoC family) fired for any chip's channel requesting >1
  buffer at native sensor resolution, but the kernel constraint requiring
  it was only ever observed on T31(L) — T10/T20/T21/T23/T30/T40/T41/C100
  now keep their untouched 2-buffer default.

## [1.5.0] - 2026-07-26

### Changed
- **Default `http.port` moved 8080 → 8880.** Port 8080 clashed with the
  ONVIF daemon (`onvif_srvd`), which also listens there; whichever bound
  first won, so ONVIF could fail to start when timps grabbed 8080. timps now
  defaults to `8880`, leaving 8080 to ONVIF. The port is still configurable
  via `http.port`; the WebUI reads the live port from `/x/timps-token.cgi`,
  so browser pages follow automatically.
- **Sub-stream OSD default `font_size` 24 → 12 px.** Better fit on typical
  sub-stream resolutions; still an absolute px value, not auto-scaled — see
  `osd1.*` in `timps.conf.example`.

### Added
- **Optional image rotation** (`USE_ROTATE`/`USE_SW_ROTATE` build flags,
  `videoN.rotation` config key: `0|90|180|270`). 180° works on every SoC;
  hardware 90/270 on T31/T40/T41; software 90/270 (CPU transpose + SW
  JPEG/OSD) on T23. Restart-required; downstream (encoder, RTSP SDP,
  fMP4/MP4, OSD, snapshots) all use the post-rotation dimensions via one
  helper. Off by default, ~0.2 KB when disabled. Known limitation: on T31,
  90/270 can't carry a hardware OSD/privacy overlay (libimp IPU-OSD stride
  bug) — see `docs/rotation.md`.
- **Optional ONVIF audio backchannel** (`USE_BACKCHANNEL`/`USE_BC_AAC` build
  flags, `audio.backchannel`/`backchannel_codec`/`backchannel_rate` config
  keys, `caps.backchannel.available`). Implements ONVIF Profile T two-way
  audio: an RTSP client streams RTP audio (PCMU/PCMA pure-C, or AAC via
  libhelix-aac) to the camera; timps decodes + resamples it to PCM16 and
  pipes it to `/bin/iac -s` (thingino's `ingenic-audiodaemon`) — timps itself
  never opens `IMP_AO`, so it works identically on every SoC as long as the
  audiodaemon is installed. See `docs/backchannel.md`.
- **Optional HTTPS + SRT (compile-time gated).** New `USE_TLS` (mbedTLS) and
  `USE_SRT` (libsrt) build flags, auto-enabled by the buildroot package
  selection (`BR2_PACKAGE_MBEDTLS` / `BR2_PACKAGE_LIBSRT`) - if the lib isn't in
  the image nothing changes. `USE_TLS`: a small mbedTLS wrapper (`src/tls.c`)
  behind which the HTTP server can run **HTTPS** (`http.https` + `http.tls_cert`
  / `http.tls_key`); the httpd I/O now goes through a TLS-aware send/recv layer
  that is byte-for-byte the old plain path when `USE_TLS` is off. `USE_SRT`:
  MPEG-TS over SRT output in listener mode (`src/srt.c`, `srt.enabled`/`port`/
  `channel`/`latency_ms`/`streamid`/`passphrase`) served from the hub like the
  recorder. Config keys for RTSPS (`rtsp.tls`/`rtsp.tls_port`) are parsed and
  reserved. NOTE: the TLS and SRT code paths cannot be built in the x86 sim
  (no mbedTLS/libsrt) - the default build stays verified; the TLS/SRT paths and
  the hand-rolled TS muxer need on-device verification.
- **Local recording to SD** (`record` section + `/control` action): records one
  video stream (+AAC audio) to `<dir>/<hostname>/records/<strftime>.mp4` as
  fragmented MP4, reusing the `/stream.mp4` muxer (`src/record.c`). Modes:
  `continuous` or `motion` (pre-roll ring from the keyframe before the trigger +
  `post_roll_s` after the last motion). Segments rotate every `record.segment_s`
  at a keyframe; oldest files are pruned to keep `record.min_free_mb` free.
  `GET /control` reports a `record` status object (recording/channel/mode/bytes/
  free_mb/file) and `caps.record`; `{"record":{"active":1|0}}` is a manual
  start/stop override (the WebUI record button). thingino path defaults
  (`/mnt/mmcblk0p1`, `<host>/records/` tree). Verified end-to-end in the x86 sim
  (valid MP4 segments via ffprobe).
- **Privacy cover masks** (`privacy` section, `/control` + config): solid filled
  rectangles per video stream (`privacy<S>.<N>.{enabled,x,y,w,h,color}`, up to
  `MS_MAX_PRIVACY` per stream) that black out sensitive areas, implemented as IMP
  OSD cover regions in the per-stream OSD group. Applied LIVE (create/show/hide/
  move without a restart, as long as OSD or a privacy region was on at startup)
  and persisted. `GET /control` dumps the `privacy` tree and advertises
  `caps.privacy = {available, max_regions}`. Replaces the prudynt-era WebUI
  privacy page's dependency on the `json-prudynt.cgi` bridge. NOTE: the IMP cover
  region call in `imp_osd.c` uses the common SDK form and needs on-device
  verification against the exact `<imp/imp_osd.h>` coverData layout.
- **Token now also unlocks HTTP media viewing** (`USE_CONTROL` builds): the
  `/control` token (per-boot `http.token_file` + optional persistent
  `http.token`, same constant-time check) is accepted on `/stream.mp4`,
  `/stream.mjpeg` and `/snapshot.jpg` (incl. `?chn=N`) as `?token=` — the
  only form an `<img>`/`<video src>` can use — or `X-Timps-Token`. This lets
  the thingino WebUI previews load the streams DIRECTLY from the HTTP port
  (no on-device proxy CGIs) even with `http.user` set. Media access is now
  localhost ∨ token ∨ Basic ∨ open-when-no-user — the existing rules are
  unchanged, the token is a pure addition; it still never unlocks RTSP, and
  non-media paths (`/` player, bogus paths) are NOT unlocked by a token.
  The media endpoints also answer the CORS `OPTIONS` preflight now, and
  `/stream.mjpeg` + `/snapshot.jpg` responses carry
  `Access-Control-Allow-Origin: *` like `/stream.mp4` always did, so
  cross-origin `fetch()`es of all three work. Caveat as with `/events`: a
  query token can end up in access logs — accepted on a LAN.
- **`GET /events` SSE push stream** (`USE_CONTROL` builds): a long-lived
  `text/event-stream` that PUSHES JSON state instead of being polled —
  `event: motion` (the `/control` motion object, emitted when the active
  grid/enabled/geometry/sensitivity changed), `event: daynight` (the
  `/control` daynight object, on a mode flip or ≥1 % brightness / ≥5 % gain
  move) and a periodic `event: stats`
  (`{"uptime_s","clients","video":[{"chn","subs","fps"},…]}`, every
  `events.stats_ms`). `?stream=motion,daynight,stats` filters the types
  (default all). Same auth as `/control` (localhost / token / Basic, CORS +
  OPTIONS preflight); the token is also accepted as `?token=` because
  EventSource cannot send headers. On connect: `retry: 3000`, a
  `: connected` comment and the full current state once; afterwards
  per-connection dedup (last-sent snapshot per event type) plus a `: ping`
  keepalive (~12 s) that doubles as dead-client detection. New tiny notify
  hub `src/events.c/.h` (generation counter + `CLOCK_MONOTONIC` condvar):
  `events_notify()` is called from the IVS result thread (grid changed,
  start/stop), the day/night sampler (real changes only) and `/control`
  writes to `motion.*`/`daynight.*`/`image.running_mode`; it is a no-op stub
  without `USE_CONTROL`, so every build permutation still links. Config:
  `events.enabled` (default 1), `events.stats_ms` (default 2000, 0 = off),
  `events.max_clients` (default 8; beyond → `503`, so an /events flood
  cannot exhaust the HTTP connection threads). The status-object JSON is
  built by shared helpers (`control_motion_json`/`control_daynight_json`),
  so `/control` and `/events` emit identical shapes by construction. The
  thingino WebUI preview overlay now subscribes to `?stream=motion` (with a
  4 Hz `/control` polling fallback) instead of polling.
- **Grid motion detection (IMP_IVS)**: the single detection ROI became a
  configurable `motion.cols` × `motion.rows` GRID of IMP_IVS move-ROIs laid
  evenly over the `motion.monitor_stream` frame (integer pixel split, the last
  row/column absorbs rounding; cell index row-major = `row*cols+col`).
  `cols*rows` is clamped to the SDK's compile-time `IMP_IVS_MOVE_MAX_ROI_CNT`,
  taken from the `imp_ivs_move.h` being built against via the new
  `motion_caps.h` (`MOTION_AVAILABLE`/`MOTION_MAX_CELLS`): 52 on most SDKs,
  **4** on the old T10/T20 3.9.0 SDK (grid defaults 5×5, 2×2 on 4-cell SDKs).
  The UI sensitivity 0..255 maps to IMP's 0..4 normal-camera range (one global
  value for all cells for now). SDKs without the move API compile a no-op stub
  and report the feature unavailable. The IVS group is now explicitly bound to
  the monitor stream's FrameSource (FS→IVS, unbound on stop) and the move
  interface is released via `IMP_IVS_DestroyMoveInterface` (both were missing).
- **Live motion control + status**: `motion.enabled`/`sensitivity`/`cols`/
  `rows`/`monitor_stream` are settable via `/control` and applied LIVE — the
  HAL cleanly stops and recreates the IVS channel (move params are create-time
  attributes). `cooldown_ms`/`on_motion` stay config-file only (`on_motion`
  runs through `system()`). `GET /control` gained `caps.motion`
  (`available`, `max_cells`) and a read-only `motion` status object:
  `{"available","enabled","cols","rows","max_cells","sensitivity",
  "monitor_stream","active":[0/1,... row-major, length cols*rows],
  "last_ms"}` (`last_ms` = ms since the last motion event, -1 = never). The
  thingino WebUI polls it directly on `:8880` with the `/control` token to
  draw a live grid overlay on the preview.

- **Token auth for `/control`**: the endpoint now allows any one of localhost
  (unchanged), a valid token, or HTTP Basic (unchanged). Tokens travel as an
  `X-Timps-Token:` header (preferred) or `?token=` query parameter, are
  compared in constant time and only unlock `/control` — never the streams.
  A random 128-bit per-boot token is generated from `/dev/urandom` and
  published to `http.token_file` (default `/run/timps.token`, mode 0640,
  `""` disables) so a local privileged helper (the thingino WebUI) can hand it
  to its authenticated browser session; an optional persistent `http.token`
  secret is also accepted for remote automation and is never written to the
  token file.
- **CORS on `/control`**: `OPTIONS` preflight (204, answered before auth — a
  preflight carries no credentials) and reflection of the request's `Origin`
  (+ `Vary: Origin`, `Access-Control-Allow-Headers: X-Timps-Token,
  Content-Type`, methods, max-age) on `/control` responses, so a browser page
  on another port (WebUI on `:80`) can call `:8880/control` directly with the
  token. `Access-Control-Allow-Credentials` is deliberately never sent.

### Fixed
- Target builds now pass `-I$(IMP_INC)/imp` too: the T10/T20 3.12.0 IVS
  headers include `<imp_ivs.h>` without the `imp/` prefix and did not resolve
  with `-I$(IMP_INC)` alone.
- **Command injection hardening**: `daynight.switch_cmd` (day/night switch
  script) and `motion.on_motion` (motion-trigger script) now run via
  `fork()`+`execlp()` instead of `system()` — no shell, so a value containing
  shell metacharacters just fails to exec instead of running as injected
  commands. Both keys were already config-file-only (never settable via
  `/control`), but this closes the gap for anyone with config-file write
  access. See `dev_notes/SECURITY_AUDIT_2026-07-23.md`.
- **Value clamping**: `audio.gain` now clamps to the IMP-documented mic PGA
  range (0..31, was 0..100); `audio.volume`/`alc_gain`/`spk_volume`/
  `spk_gain` and OSD `logo_w`/`logo_h`/`outline` are clamped against their
  real IMP/rendering limits so out-of-range `/control` values can't wrap or
  blow up an allocation.
- `hal_get()`'s return value is now NULL-checked before its first use at
  startup (previously dereferenced once, in the startup log line, before the
  existing check further down).

## [1.2.0] - 2026-07-11

### Added
- **Full ISP image control via `/control`**: the `image` section now covers the
  complete tuning set — brightness, contrast, saturation, sharpness, hue, h/v
  flip, running_mode, anti-flicker, AE compensation, max analog/digital gain,
  sinter & temper (noise), DPC, defog, DRC (WDR), highlight-depress (tone),
  backlight compensation and white balance (mode + R/B gain) — applied live via
  the matching `IMP_ISP_Tuning_*` call. A compile-time per-SoC capability matrix
  (`isp_caps.h`, T10–T41 + C100) is reported as `caps.image` so a UI can grey
  out what a given SoC cannot do; unsupported keys still persist.
- **Full audio control via `/control`**: live mic volume, gain, ALC gain,
  high-pass filter, AGC (+ target level / compression), noise-suppression, and a
  **live mic mute** (`audio.mute` — captured frames are dropped before the
  encoder/hub, no restart). Capability matrix in `audio_caps.h` → `caps.audio`.
  Codec / sample-rate / bitrate / channels persist and apply on restart.
  Speaker & forced-stereo have no IMP-AO path and are reported unsupported.
- **Full encoder & sensor control** (persist + restart): `video.N` accepts the
  whole per-stream key set (codec, width, height, fps, bitrate, rc_mode, gop,
  max_gop, profile, qp, min/max_qp, rotation, buffers, enabled, rtsp_path) and a
  new `sensor` section (model, i2c_addr, fps, width, height). These never touch
  the running pipeline; `GET /control` flags them in `caps.restart` and dumps
  the current values so a UI can populate.
- **Per-stream OSD**: every video stream has its own independent overlay set
  (`osd.items[stream][item]`). Canonical keys `osd<S>.<N>.<field>` (e.g.
  `osd0.0.text`, `osd1.2.x`); legacy `osd<N>.<field>` keys still load and mirror
  onto every stream. `/control` accepts `"osd0"/"osd1"` objects (live via
  `imp_osd_apply(stream,item)`) and still the shared legacy `"osd"` object.
- **OSD text outline/stroke**: new per-item `outline` (width px, 0 = off,
  default) and `outline_color` (`0xAARRGGBB`, default black). The TTF and
  embedded-bitmap rasterizers dilate the glyph coverage and blend the stroke
  under the fill; the region grows by the outline width. `caps.osd` lists the
  new leaves.
- **Day/night measurement exposed** (`daynight_get_status()`): the detection
  thread derives the **total gain** from the isp-m0 gain fields (IMP log2 units)
  converted to the `GetTotalGain` [24.8] linear scale (256 = 1×, matching what
  prudynt/raptor report), keeps sampling in manual mode, and `GET /control`
  reports `daynight: {enabled, mode, brightness%, total_gain}` (−1 = unknown; a
  stub answers unknowns without `USE_DAYNIGHT`).
- **System log output**: timps now also logs to syslog (tag `timpsd`) so
  messages appear in `logread` (the init script backgrounds timpsd, so its
  stderr is otherwise discarded). On by default; disable with
  `general.syslog = false`.

### Changed / Fixed
- **Idle CPU** (~19 % → ~0 with no clients): on-demand now stops the
  `IMP_FrameSource` channel (not just the encoder) once a stream has no
  subscribers — an enabled FrameSource kept capturing/piping frames through the
  FS→OSD→encoder groups in the libimp worker threads. Producer threads now block
  on a condition variable instead of a poll loop, and the OSD updater only
  renders while a stream has viewers. Reactivation is immediate; the monitored
  FrameSource is pinned while motion detection is enabled.
- **`GET /control` capabilities** now report `caps.{image,audio,osd,restart}` so
  UIs can present exactly what this build/SoC supports.

## [1.1.0] - 2026-07-11

### Added
- **Live control API** (`POST`/`GET /control`, compile flag `USE_CONTROL`, on by
  default). A nested JSON blob changes settings live *and* persists the changed
  keys back to the config file (atomic tmp+rename, comments/order preserved).
  Supported: `image` (brightness, contrast, saturation, sharpness, hue, hflip,
  vflip, running_mode), `audio` (volume, gain), `osd.N` overlays, `video.N`
  bitrate (persisted only — applies on restart). The legacy flat form and
  `{"force_mode":"day"|"night"}` still work. Requests from localhost bypass
  auth; remote access requires configured HTTP/RTSP credentials.
- **Native automatic day/night** (compile flag `USE_DAYNIGHT`, on by default). A
  background thread reads ISP brightness from `/proc/jz/isp/isp-m0` and applies
  threshold + hysteresis + transition-delay logic (ported from thingino's
  `daynightd`), switching via the board's `daynight day|night` script (IR-cut /
  IR-LEDs / colour). Runtime toggle through `/control`
  (`{"daynight":{"enabled":true|false}}`). New `daynight.*` config keys
  (`enabled`, `threshold_low`, `threshold_high`, `hysteresis`, `interval_ms`,
  `transition_s`, `switch_cmd`, `isp_path`).
- **Live OSD apply** (`imp_osd_apply`): OSD overlay changes made through
  `/control` are re-rendered on the running streams.
- `config_get_kv()` — read a config value back as a normalized string
  (used for change detection).

### Fixed / Hardened
- **`/control` change detection**: a value that does not actually change is no
  longer re-applied to the ISP nor rewritten to the config file. This stops
  clients that poll and re-post the same value every few seconds from hammering
  the ISP and, worse, rewriting the config on flash over and over.
- **`/control` input validation**: invalid values (`null`, `undefined`, empty)
  are rejected instead of being stored and parsed to `0`.
- **Config-injection defense**: persisted values are stripped of control
  characters and double quotes before being written to the flat config file.

### Build
- `USE_DAYNIGHT` added to the Makefile (target and host-sim recipes); both
  `USE_CONTROL` and `USE_DAYNIGHT` default on and can be disabled independently
  (`USE_CONTROL=0` / `USE_DAYNIGHT=0`), compiling the feature out entirely.

## [1.0.0]

### Added
- Initial import: Tiny IMP Streamer — pure-C RTSP + fragmented-MP4 + JPEG/MJPEG
  streamer for Ingenic SoC cameras, built straight on the vendor `libimp`
  (no live555 / libconfig / libwebsockets / libschrift). On-demand encoding,
  TrueType OSD, motion detection, RTSP-Digest / HTTP-Basic auth. Ingenic
  headers via the `ingenic-headers` submodule.

[1.2.0]: https://github.com/Lu-Fi/timps/compare/v1.1.0...v1.2.0
[1.1.0]: https://github.com/Lu-Fi/timps/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/Lu-Fi/timps/releases/tag/v1.0.0
