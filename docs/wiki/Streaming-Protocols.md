# Streaming Protocols

timps serves the same underlying [hub](Architecture.md#the-hub-srchubc--srchubh)
sources over five different wire protocols. This page covers each one's
transport, codecs, authentication, and the client-compatibility details
the code goes out of its way to get right.

## Quick reference

| Protocol | Default port | Transport | Video codec | Audio codec | Auth | TLS |
| --- | --- | --- | --- | --- | --- | --- |
| RTSP | 554 (RTSPS 322) | TCP-interleaved or UDP unicast | H.264 always; H.265 if `videoN.codec=h265` | AAC (RFC 3640), G.711 PCMU/PCMA (RFC 3551), or Opus (RFC 7587, `USE_STREAM_OPUS`) | RTSP Digest (no `qop`) or Basic | via `USE_TLS` + `rtsp.tls`, shares the HTTPS cert/key |
| HTTP fMP4 | 8880 (`/stream.mp4`) | Chunked HTTP, `video/mp4`, MSE-oriented | H.264 always; H.265 if the stream's codec is H.265 | **AAC only** — the muxer has no G.711 support | localhost bypass, token, HTTP Digest (`qop=auth`) or Basic | via `USE_TLS` + `http.https` |
| MJPEG | 8880 (`/stream.mjpeg`, `/mjpeg`) | `multipart/x-mixed-replace` | MJPEG (piggyback or dedicated `jpeg.*` channel) | n/a | same as HTTP | same as HTTP |
| Snapshot | 8880 (`/snapshot.jpg`) | single JPEG response | MJPEG | n/a | same as HTTP | same as HTTP |
| SRT | 9000 | MPEG-TS over SRT, listener (default) or caller (`srt.mode`) | H.264/H.265 (PID `0x100`) | AAC only (PID `0x101`; video-only fallback for G.711) | `srt.streamid` (access) + `srt.passphrase` (AES) | SRT's own encryption |

## RTSP (`src/rtsp/rtsp.c` + `src/rtsp/rtp.c`)

- **Port**: `rtsp.port` (default **554**); RTSPS on `rtsp.tls_port`
  (default **322**, RFC 7826 Annex C's well-known port) when built with
  `USE_TLS` and `rtsp.tls=1` — sharing the **same certificate/key** as
  HTTPS (`http.tls_cert`/`http.tls_key`).
- **Methods**: `OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER`.
  Anything else mid-session (e.g. `PAUSE`/`SET_PARAMETER`) gets a clean
  `405` with an `Allow:` header rather than being silently dropped.
- **Path convention**: `videoN.rtsp_path` (default `/ch0` for the main
  stream, `/ch1` for the sub stream) is matched as a path prefix followed
  by `/`, `?`, or end-of-string; an unmatched path falls back to the
  first boot-enabled stream. This is the one config key that is honestly
  **live** — see [Configuration Reference](Configuration-Reference.md).
- **Transport**: both **TCP-interleaved** (`Transport: RTP/AVP/TCP;
  interleaved=N-N+1`, with self-assigned sequential channel numbers per
  SETUP so video/audio/backchannel tracks never collide) and **UDP
  unicast** (`RTP/AVP;unicast;client_port=...;server_port=...`, server
  ports picked randomly from 6000–14190, even, with retry-on-collision).
  A `SETUP` offering neither gets `461 Unsupported Transport`.
- **Session timeout**: 60s (`RTSP_SESSION_TIMEOUT_S`), advertised via the
  SETUP response's `Session: ...;timeout=60` (clients like ffmpeg/live555/VLC
  send keepalives at half that). UDP-transport sessions are additionally
  reaped after 2× timeout of control-channel/RTCP silence; TCP-interleaved
  sessions rely on the socket's own send timeout instead.
- **RTP payload types**: fixed `96` (video), `97` (audio).
- **Performance**: UDP video RTP packets are batched via `sendmmsg()`
  (16 packets per call) instead of one `sendto()` per packet, with a
  per-packet fallback if the kernel lacks `sendmmsg`.

### SDP (DESCRIBE)

`gen_sdp()` builds a session description with several deliberate
RFC-compliance choices, all backed by code comments citing real client
breakage they fix:

- `o=` uses a real session id (not a hardcoded `"0 0"`) per RFC 4566 §5.2.
- `a=range:npt=now-` marks the stream as live/unbounded (RFC 2326 Annex
  A.3) so players don't assume a seekable VOD range.
- `a=control:*` plus per-track `a=control:trackID=0` (video) /
  `trackID=1` (audio) / `trackID=2` (backchannel, if applicable).
- The video m-line always includes an `a=fmtp:` line
  (`sprop-parameter-sets`/`profile-level-id`) — the code comment is
  blunt about why: *"never ship an SDP without the a=fmtp line...
  hardware-decoder NVRs and some mobile SDKs init their decoder strictly
  from the SDP and stay black"* without it. `DESCRIBE` briefly subscribes
  to the encoder (up to 2s) to force an IDR/SPS-PPS capture before
  answering, falling back to `503` + `Retry-After: 1` rather than sending
  a degraded SDP.
- The audio m-line is `mpeg4-generic/<rate>/<channels>` (AAC, RFC 3640
  `fmtp`: `streamtype=5;mode=AAC-hbr;config=<ASC hex>`) or G.711 PCMU
  (payload type 0) / PCMA (payload type 8) at 8kHz.
- `Content-Base:` is explicitly set to the request URL so a strict
  parser's relative `a=control:trackID=N` resolves correctly.

### RTP packetization (`rtp.c`)

- **H.264** (RFC 6184) / **H.265** (RFC 7798): single-NAL-per-packet when
  it fits the MTU (`rtsp.mtu`, default 1200), otherwise FU-A (H.264) or
  the equivalent H.265 fragmentation unit, with the marker bit set
  correctly on the last NAL of an access unit via one-NAL lookahead
  (an earlier fixed-size 64-NAL list silently dropped NALs beyond that
  cap).
- **AAC** (RFC 3640 `mpeg4-generic`): a 4-byte AU-header (16-bit
  headers-length + a 13-bit size/3-bit index-delta pair) is prepended,
  and — a specific RFC-conformance fix — the header reports the
  **complete** AU size on **every** fragment of a split AU, not just the
  first.
- **G.711**: sample-exact fragmentation; the marker bit is set only on
  the very first packet of the session (RFC 3551 §4.1 talkspurt
  semantics — there's no silence suppression here, so it never re-fires).
- **Timestamping**: each track's RTP timestamp is anchored to its own
  first PTS rather than absolute uptime, avoiding divergent wrap periods
  between the 90kHz video clock and 16/48kHz audio clock (an earlier
  design made some clients drop and reconnect every few minutes). Audio
  timestamps are sample-count-driven with a gap-resync mechanism so a
  real discontinuity (mute, capture stall, queue overflow) doesn't leave
  a permanently growing A/V offset.
- **RTCP**: a compound **SR+SDES** packet (never a bare SR) at most
  once/second, per RFC 3550 §6.1 — a bare 28-byte SR was tolerated by
  ffmpeg/VLC/gstreamer but flagged by stricter stacks (ONVIF conformance
  suites, Genetec-class VMS). `TEARDOWN` sends SR-or-RR + SDES + BYE
  (§6.3.7). SSRC and initial sequence/timestamp are seeded from
  `/dev/urandom`, not `rand()`, so off-path RTP injection/guessing isn't
  feasible.

### Authentication

Disabled while `rtsp.user` is empty. Otherwise every method except
`OPTIONS` requires auth: **RTSP Digest** or **HTTP Basic** in the
`Authorization:` header. A `401` challenges with *both*
`WWW-Authenticate: Digest` and `...Basic`, with a fresh nonce minted per
connection (RTSP has no `qop` here, unlike the HTTP side). The digest
check additionally verifies the client's `uri=` against the actual
request-line target (anti cross-URI replay) and that the nonce is one
this server actually issued this session (anti offline replay — a
sniffed Authorization header is otherwise fully reproducible and
replayable forever against any connection).

### ONVIF audio backchannel (trackID=2)

Advertised only if the client's `Require:` header includes
`www.onvif.org/ver20/backchannel` and the feature was actually configured
at boot (`USE_BACKCHANNEL`, `audio.backchannel=1`) — any other
unsupported `Require:` feature tag gets a clean `551 Option not
supported` (RFC 2326 §12.32) rather than being silently ignored. See
[Audio](Audio.md) for the full backchannel pipeline.

## HTTP fMP4 preview (`src/mp4/httpd.c` + `src/mp4/fmp4.c`)

- **Port**: `http.port` (default **8880**); HTTPS when built with
  `USE_TLS` and `http.https=1`.
- **Routes**: `/` and `/?embed` (an HTML page with an embedded
  MediaSource-Extensions `<video>` player — the MSE codec string is
  derived live from the actual SPS/HEVC profile-tier-level, not
  hardcoded), `/stream.mp4?chn=N` (the raw fMP4 byte stream, also what
  the player page fetches), `/snapshot.jpg?chn=N`, `/stream.mjpeg` /
  `/mjpeg` (`?chn=N`, `?boundary=`), plus `/control` and `/events`
  (`USE_CONTROL`). Anything else is `404`.
- **`?chn=N`** selects a stream by index, validated against the **boot**
  config (`g_cfg_boot.video[n].enabled`) since `enabled` is restart-only;
  an invalid/disabled index falls back to the caller's default
  (`http.preview_chn`).
- **fMP4 muxing**: a standard CMAF-style stream — one `ftyp`+`moov` init
  segment (brands `isom`/`iso5`/`dash`/`mp41`), then one `moof`+`mdat`
  fragment per access unit. Video sample entry is `avc1` or `hvc1`
  depending on the stream's actual codec; codec-config boxes
  (`avcC`/`hvcC`) come from the cached SPS/PPS/VPS. Audio, when present,
  is `mp4a`/`esds` (AAC-LC only — **G.711 audio cannot be muxed into
  fMP4 at all**, so an audio track is only declared when the hub reports
  AAC).
- **Continuous timeline**: the `tfdt` (fragment decode time) is a running
  accumulator of *emitted* durations rather than being derived per
  fragment from absolute PTS — a deliberate MSE-compatibility fix, since
  deriving it from absolute PTS left sub-frame gaps under normal capture
  jitter that stalled browser playback after the very first frame.
- **Adaptive frame drop** (`http.adaptive_drop`, default on): when one
  client's own fanqueue backs up (a weak link that can't keep up), that
  client freezes on its last good frame and resumes cleanly at the next
  keyframe instead of being fed a corrupted headless GOP — purely a
  per-client delivery decision that never touches the shared encoder or
  any other subscriber; a resulting IDR request is rate-limited to once/sec
  so one weak client can't spike the bitrate for everyone else.
- **MSE player details**: the built-in player handles iOS's
  `ManagedMediaSource` vs. desktop `MediaSource`, nudges playback rate to
  stay ~1.5s behind live (hard-seeking on >5s drift), and evicts
  SourceBuffer data more than 10s behind the current playback position.
- **HEAD**: answered with the same headers a `GET` would send and no
  body, per RFC 7231 §4.3.2, across every route.

### Authentication (HTTP)

`http_check_auth()` — every request except loopback (127.0.0.0/8) or a
token-authorized `/control`/`/events`/media path needs credentials.
Supports **HTTP Digest** (RFC 7616 `qop=auth`, plus legacy RFC 2069
no-qop) and **Basic**, falling back to `rtsp.user`/`rtsp.pass` if
`http.user` is unset. Unlike RTSP (one nonce per connection), HTTP is
one-connection-per-request, so nonces are tracked in a small global ring
(32 entries, 5-minute TTL); `qop=auth` clients must present a
strictly-increasing nonce-count (`nc`) per nonce, which is what lets a
legitimate repeat client (e.g. an NVR's periodic snapshot poller) reuse a
nonce without it being treated as a replay.

## MJPEG (`/stream.mjpeg`, `/mjpeg`)

`multipart/x-mixed-replace` body, one JPEG frame per part, sourced from
either a video stream's piggyback JPEG encoder (`videoN.jpeg=true`) or
the dedicated `jpeg.*` channel, selected the same way `?chn=N` selects a
video stream. Same auth/CORS/token rules as the rest of the HTTP surface.

## Snapshot (`/snapshot.jpg`)

A single JPEG response from the same source selection logic as MJPEG.
`?chn=N` gets the resolution of that stream's piggyback JPEG (needs
`videoN.jpeg=true`) instead of the dedicated `jpeg.*` channel's fixed
resolution.

## SRT (`src/srt.c`)

Compiled only under `USE_SRT` (needs `libsrt`). `make sim USE_SRT=1`
exercises both modes against host libsrt/ffmpeg, but the hand-rolled TS
mux still wants on-device verification with `ffplay`/VLC — TS
bit-twiddling is easy to get subtly wrong.

- **Mode**: `srt.mode` selects between the two, both serving one
  configured video channel (`srt.channel`, validated against the
  boot-enabled streams):
  - **`listener`** (default) — `srt_bind`+`srt_listen` (backlog 4) on
    `srt.port` (default **9000**) and one streaming thread per accepted
    client.
  - **`caller`** — timps dials `srt.host`:`srt.port` itself and streams
    over that one connection, for a camera behind NAT or on an
    unreliable link. It reconnects forever with a doubling 1→30 s
    backoff, and logs an outage **once** (not once per attempt); a
    session shorter than 5 s does not earn the fast backoff back, so a
    flapping receiver cannot cycle at 1 s. `srt.mode=caller` with an
    empty `srt.host` disables SRT with an error rather than falling back
    to a listener; any other unrecognized `srt.mode` value warns and
    uses `listener`.
- **Muxing**: a hand-rolled MPEG-TS mux — video on PID `0x100`, AAC audio
  on PID `0x101` (ADTS-wrapped for `stream_type=0x0F`), PMT on PID
  `0x1000`, PCR carried on the video PID, PAT/PMT resent roughly every
  second. TS packets are batched 7-at-a-time (1316 bytes, the
  conventional TS-over-UDP/SRT payload size) and flushed at the end of
  every access unit to bound added latency.
- **Audio limitation**: if `audio.enabled` but the configured codec isn't
  AAC (i.e. G.711 or Opus), SRT streams **video-only** with a one-time
  warning — the TS mux here only knows how to carry AAC.
- **Access control**: `srt.streamid`, if set, is enforced at accept time
  in listener mode — a connecting client must present the matching
  `STREAMID` or is rejected (this was previously checked nowhere at
  all); in caller mode the same key is instead the `STREAMID` timps
  *presents* to the receiver. `srt.passphrase`
  (10–79 chars, libsrt's AES-based encryption) is validated when the
  socket is set up; if libsrt rejects it, **SRT gives up** — the listener
  never binds, the caller stops dialling — rather than silently running
  unencrypted.

Playing back a listener-mode camera:

```sh
ffplay srt://<ip>:9000
```

## Codec support summary

| Codec | Where |
| --- | --- |
| **H.264** | Always available; the only option on platforms without a newer HEVC-capable encoder generation. |
| **H.265 (HEVC)** | Available where the SoC/SDK's encoder supports it (`videoN.codec=h265`) — see [Platform & SDK Support](Platform-SDK-Support.md). Carried over RTSP, HTTP fMP4, and SRT; **not** over MJPEG/snapshot (those are always JPEG). |
| **AAC** | Needs `USE_FAAC` (software encode via libfaac). The only audio codec HTTP fMP4 and SRT can carry. |
| **G.711 (PCMU/PCMA)** | Pure-C, always available, no library dependency. RTSP-only for playback purposes — cannot be muxed into fMP4 or the SRT TS mux. |
| **Opus** | Compile-time optional (`USE_STREAM_OPUS`, bare `libopus` encoder ~337 KB, off by default). Select with `audio.codec=opus`. RTSP/RTP only (RFC 7587), like G.711 — cannot be muxed into fMP4 or the SRT TS mux. Encoded at the capture rate (16 kHz default) but RTP-advertised as `opus/48000/2` per the RFC. See [Audio](Audio.md). |

See [Platform & SDK Support](Platform-SDK-Support.md) for which SoCs get
which encoder/ISP capabilities, and `docs/rotation.md` for how rotation
interacts with stream geometry (post-rotation dimensions are what every
protocol above actually advertises/muxes).

## Known limitations

### RTP/RTCP timestamp overflow after ~2.8–3 years of continuous uptime (deferred, "L13")

- **What.** The RTP timestamp math computes `rel * clock_rate` as an
  `int64` product, where `rel` is the microseconds elapsed since the
  track's first PTS. At the 90 kHz video clock this product overflows
  `INT64_MAX` once `rel` reaches roughly `INT64_MAX / 90000` µs.
- **When.** After about **2.8–3 years of continuous process uptime**
  without a restart. A restart re-anchors the track (`pts0`/`ts_base` are
  re-seeded), so the counter resets — in practice this is only reachable
  by a camera that streams for years without the daemon ever restarting.
- **Effect.** Past that point the video RTP timestamps (and the identical
  computation in the RTCP sender report) go wrong, which would desync A/V
  and could make strict clients drop/reconnect.
- **Why deferred.** A correct fix needs the track to periodically
  **rebase** `pts0`/`ts_base` rather than a one-line clamp, so it was
  intentionally left for a follow-up rather than patched piecemeal.
- **Where the code is.** `src/rtsp/rtp.c` — `pts_to_ts()` (the
  `L13 (deferred)` comment) and the matching math in the RTCP SR path
  (`rtcp_wr_sr()` / `rtp_maybe_sr()`).
