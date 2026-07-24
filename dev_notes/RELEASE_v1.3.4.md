# v1.3.4

21 commits of security hardening, protocol-correctness fixes, and a new live
config-sync feature, none of which had shipped in a tagged release since
v1.3.3 (2026-07-14).

## Security / correctness (critical)

- **fmp4: propagate `ms_buf` allocation failures instead of emitting a
  corrupt fragment.** A failed buffer grow was silently ignored, risking a
  heap out-of-bounds write on the next box write.
- **RTSP digest auth now validates the nonce and refreshes it per
  challenge**, and session IDs / nonces use a secure random source instead
  of a predictable one.
- **msttf: bounds-check the TTF parser** against corrupt or malicious font
  data (table directory, glyph offsets, contour/point counts).
- **SRT: fix a double-ADTS risk**, validate the channel, batch TS packets
  into fewer syscalls.
- **hal/ingenic: drop truncated video/JPEG frames** instead of publishing
  corrupt data.

## Protocol correctness

- **vparam:** fix truncated parameter sets, add the missing avcC High-profile
  extension bytes, fix hvcC `array_completeness`.
- **RTP/RTCP:** fix several packetization bugs - FU fragmentation MTU
  accounting, G.711 marker bit semantics, H.264/H.265 NAL-per-AU cap, RTCP SR
  timestamp mapping.
- **RTSP:** add `RTP-Info` to the `PLAY` response, correctly reassemble the
  interleaved control stream, send RTCP from its own socket instead of the
  RTP one.
- **`/control` POST bodies over the size limit are now rejected**, not
  silently truncated.
- **Config file writes are now fsync'd** (file + directory entry) for
  durability across a power loss.

## New

- **Live config-sync:** setting changes now push to any open WebUI tab or
  client via the `/events` `config` SSE stream, instead of only applying on
  next reload.
- **OSD anti-aliasing quality is now configurable** (`osd.supersample`,
  default 2 - was hardcoded to 4, same visual result at lower CPU cost for
  typical OSD text sizes).
- **Live preview now works on iOS Safari** (uses `ManagedMediaSource` where
  `MediaSource` isn't available).
- Per-stream JPEG enabled by default.

## Reliability

- Pre-roll recording ring is now capped by bytes, not just count/time.
- HTTP request parsing accumulates headers instead of trusting a single
  `recv()`.
- The `/control` JSON response buffer moved off the stack to the heap, as
  did the hal_sim per-thread AU test buffer.
- `hub_get_fps()` now locks its read of `mfps`.

**Full diff**: https://github.com/Lu-Fi/timps/compare/v1.3.3...v1.3.4
