# WebUI live preview: getting closer to real-time — ideas (2026-08-29)

Follow-up to `dev_notes/PTZ_LATENCY_INVESTIGATION_2026-08-29.md`. That work
established the baseline this document builds on: the camera side delivers a
changed frame's bytes to a client socket in **~170 ms** (daemon-internal share
~17 ms), so everything above that in the WebUI preview is client-side policy.

## Where the preview's latency actually is today

The WebUI preview is a native MSE/fMP4 player
(`package/thingino-webui`-overlaid `preview.html` from
`thingino-firmware-LuFi/package/timps/files/www/`, a port of the daemon's own
embedded player in `src/mp4/httpd.c` `PLAYER_TAIL`): it `fetch()`es
`/stream.mp4?chn=N` (default `http.preview_chn=1`, the 640x360 sub stream)
and appends each chunk to a SourceBuffer. Crucially it then **deliberately
plays behind the live edge** — a user-selectable jitter margin of 0.5 / 1.5
(default) / 3 s, enforced by playbackRate-nudging, because zero cushion
turned every WiFi hiccup into a visible freeze. So today's glass-to-glass is
roughly:

    ~0.17 s (camera+network, measured)  +  0.5–3 s margin (policy, default 1.5)
    + browser MSE/video pipeline (~0.1 s)  ≈  0.8–3.2 s, typically ~1.7 s

The daemon needs nothing: it already muxes one `moof`+`mdat` per access unit
(frame-granular, no server-side batching), and the ffmpeg-family standing
delay from the PTZ investigation is libav-specific — browsers don't have it.
The problem to solve is purely **the client's buffering model**: MSE +
`<video>` gives smoothness by construction and makes true low latency
structurally awkward.

## Idea 1 — WebCodecs player on the existing `/stream.mp4` (recommended; prototyped and verified)

Replace (when available) the MSE pipeline with: `fetch(/stream.mp4)` → tiny
JS box parser (we control the muxer, so the layout is fixed and trivially
parseable: skip `ftyp`, pull `avcC` out of `moov`, read the `tfhd` track id
of each `moof`, feed each video `mdat` — already AVCC length-prefixed — to a
`VideoDecoder` configured `{codec, description: avcC, optimizeForLatency:
true}`) → draw each `VideoFrame` to a canvas on arrival. No SourceBuffer, no
playback clock, no cushion: the newest frame is simply shown when it arrives.

**Prototyped 2026-08-29 against cam-garage** (Chromium, sub stream chn=1,
`avc1.640033`, ~90-line player core): side-by-side with the current MSE logic
on the same camera, the WebCodecs path decoded/rendered 1124/1124 frames with
**arrival→render avg 1.0 ms, max 14.6 ms, decode queue ≤3, standing buffer
~0**, while the MSE side sat seconds behind draining toward its 1.5 s target.
Expected glass-to-glass ≈ the measured 170 ms camera+network floor + one
frame of quantization ≈ **0.2–0.3 s**, i.e. ~1.4 s better than today's
default and still ~0.4 s better than the "Low latency" MSE setting.

Costs / tradeoffs, honestly:

- **Client-only change.** Zero daemon work; same endpoint, same token/auth,
  same single-consumer load on the camera. The change lives entirely in
  `package/timps/files/www/preview.html` (~150 lines incl. fallback glue).
- **Compatibility**: H.264 `VideoDecoder` is in Chrome/Edge 94+, Safari
  16.4+, Firefox 130+ — good in 2026, but not universal, so **feature-detect
  and keep the existing MSE player as the fallback** (it stays the code path
  for old browsers). H.265 streams should always fall back (WebCodecs HEVC
  support is platform-dependent patchwork).
- **No cushion means jitter is visible**: a 0.5 s WiFi stall becomes a 0.5 s
  freeze then a jump to live (frames arriving during the stall can be
  dropped — for a *preview* that chases "now", that's the correct behavior;
  it's what MJPEG viewers have always done). Expose it as a "Real-time"
  option in the existing latency selector rather than silently replacing
  "Stable".
- **Audio**: this path decodes video only. The preview autoplays muted
  anyway; simplest contract is "unmute switches to the MSE player (with its
  margin)", with a later option of a full `AudioDecoder`+`AudioWorklet` path
  if anyone actually wants low-latency audio.
- **CPU**: hardware decode still applies (WebCodecs uses the same decoders);
  canvas drawing adds a little compositor work at 640x360@15 — negligible on
  anything that runs the WebUI. `requestVideoFrameCallback`-style pacing is
  unnecessary; draw-on-output is the point.

Core of the prototype (kept here since the scratchpad is session-scoped —
box scanning helpers omitted, they're ~20 lines):

```js
// moov -> avcC -> configure
const d = avcCBytes;   // from moov/trak/mdia/minf/stbl/stsd/avc1/avcC
dec = new VideoDecoder({ output: f => { ctx.drawImage(f,0,0); f.close(); },
                         error: e => fallbackToMSE() });
dec.configure({ codec: "avc1." + hex(d[1]) + hex(d[2]) + hex(d[3]),
                description: d, optimizeForLatency: true });
// per fragment: moof/traf/tfhd track_ID == video ? feed following mdat:
dec.decode(new EncodedVideoChunk({ type: hasIdrNal ? "key" : "delta",
                                   timestamp: t += 66666, data: mdatPayload }));
```

## Idea 2 — cheap improvements inside the current MSE player (compose with 1, benefit the fallback)

- **Adaptive jitter margin** instead of a fixed default: the player already
  sees chunk arrival times; track delivery jitter (e.g. p95 inter-chunk gap
  over the last 30 s) and set `targetBehindS = clamp(p95 + 0.2, 0.3, 3)`.
  Healthy LAN sessions would settle around 0.4–0.6 s instead of 1.5 s with
  no user action, weak-WiFi sessions would keep their cushion. ~20 lines of
  JS, no protocol change, directly reuses the stall data that motivated the
  1.5 s default.
- **PTZ-aware live-edge snap**: while the joystick overlay
  (`preview-motors.js`) is being actively used, temporarily drop the margin
  (or hard-seek to `bufferedEnd − 0.3`) and restore it a few seconds after
  the last move. The times you most care about latency are exactly the times
  you're driving the camera; a post-move re-buffer is invisible. Small,
  contained, no risk to the idle-viewing experience.

## Idea 3 — MJPEG as the universal floor, not the main path

`/stream.mjpeg` already exists and an `<img>` shows a JPEG the moment it
arrives (≈ one frame + network of latency, no decoder pipeline at all). Its
real rate is the deliberate `videoN.jpeg_fps=5` default (config-file-only
key). Raising it to 10–15 for a camera where someone wants a snappier
no-WebCodecs preview is a one-line `/etc/timps.conf` edit at the cost of VPU
work and ~2–4 Mbit/s for 640x360 — fine as a per-camera choice or an
"ancient browser" fallback, wrong as the default (no audio, worse
quality-per-bit, more radio airtime on WiFi cameras).

## Idea 4 — WebRTC: assessed, not recommended for this use case

WebRTC would land in the same ~0.2–0.3 s class as Idea 1 — on a LAN, its
extra machinery buys almost nothing beyond what a WebSocket-free WebCodecs
player already achieves, and it costs by far the most: ICE/DTLS/SRTP + SDP
signaling inside (or beside) a C daemon that runs on 64 MB cameras, plus
keying/cert handling. Its two genuine advantages over Idea 1 are (a)
**mixed-content immunity** — an HTTPS WebUI today cannot load the plain-HTTP
port-8880 stream at all (known limitation noted in `preview.html`), while
WebRTC media is exempt — and (b) NAT traversal + congestion-controlled
adaptive delivery for **remote** viewing. If off-LAN live view ever becomes
a requirement, revisit WebRTC (probably as a thingino-level component, not
inside timpsd). For the mixed-content problem alone, the proportionate fixes
are `http.https=1` (TLS build) or reverse-proxying `/stream.mp4` through the
WebUI's own origin — not a WebRTC stack.

## Ideas considered and rejected

- **LL-HLS / DASH-LL**: 1–3 s by design at best; strictly worse than the
  current MSE player for this purpose, plus segmenter complexity. No.
- **WebSocket/WebTransport frame push**: the daemon-side rework buys nothing
  — chunked-HTTP `fetch()` streaming already delivers each AU to JS the
  moment it's sent (measured: arrival matches the raw client), and a WS
  endpoint would be new C code for identical latency. WebTransport
  additionally needs HTTP/3+QUIC on the camera. No.
- **jsmpeg-style MPEG1-over-WS**: obsolete; requires software MPEG1 encode
  the SoC doesn't do. No.
- **Raising the true floor** (~0.17 s): that's motor+optics+frame interval
  at 15 fps + network; only more fps (fleet-capped deliberately, see
  camera-room notes) meaningfully lowers it. Out of scope for the preview.

## Suggested order

1. Idea 1 behind feature detection, as a new "Real-time" entry in the
   existing latency selector (fallback: current MSE). One file, testable on
   cam-garage in an afternoon; the prototype's parser/decoder core is above.
2. Idea 2's PTZ-aware snap (tiny, immediate perceived win even for MSE
   fallback users), then the adaptive margin.
3. Leave MJPEG and WebRTC as documented options for the specific situations
   they fit (per-camera compat floor; future remote viewing).

Nothing here requires touching timpsd — which is the satisfying conclusion of
the PTZ investigation: the daemon already delivers frames about as fast as
the sensor produces them; the preview's realtime-ness is a browser-side
policy decision, and WebCodecs finally makes the low-latency policy cheap to
implement.

## Second, independent verification (2026-08-29, same day)

After this document was written, Idea 1 was verified a second time from
scratch, independently of the agent that wrote it above, by hand-building a
standalone test page against cam-garage rather than trusting the write-up
alone. This surfaced two real bugs in box parsing that are worth recording
so nobody reintroduces them during the real integration:

- **`tkhd` track_ID offset was wrong** in the first draft of the test
  parser. `tkhd` payload layout: `version(1)+flags(3)`, then
  `creation_time`+`modification_time` (8 bytes each if version==1, 4 bytes
  each if version==0), then `track_ID(4)`. Correct offsets into the payload:
  **version 1 → byte 20, version 0 → byte 12.** The first draft had these
  swapped *and* wrong, which doesn't error - it silently reads garbage,
  `moof`'s `tfhd` track_ID then never matches, and zero frames ever reach
  the decoder with no error anywhere. This is the most likely
  silent-integration-bug spot; test it explicitly (assert the parsed video
  track_ID against a known-good value from `ffprobe -show_entries
  stream=index` or similar).
- **`avcC` is nested two levels deeper than it looks.** `stsd` layout:
  `version+flags+entry_count`, then the first sample entry
  (`[size(4)][type(4)='avc1'][78 fixed VisualSampleEntry bytes][child boxes
  incl. avcC]`). You must skip both the `avc1` entry's own 8-byte box header
  *and* the 78 fixed bytes before scanning for `avcC` as a child box - scanning
  from the start of the `avc1` entry (an easy mistake, since it "looks like"
  a plain box list) finds the `avc1` entry's own header, skips over the
  whole entry as one unmatched box using its size, and never enters it.

**Auth mechanism, confirmed against the real endpoint (not assumed)**: a
custom `Authorization:` header on a cross-origin `fetch()` to the daemon's
port 8880 triggers a CORS preflight that the daemon's response does not
satisfy (`Access-Control-Allow-Headers` doesn't list `authorization`) -
`fetch()` fails outright, every time, from any origin other than the
daemon's own port. The `?token=` query-string form (reading the per-boot
token from `/run/timps.token` on-device, same value `/x/timps-token.cgi`
serves to the WebUI) is not just preferred, it is the **only** form that
works for a cross-origin/standalone test client. **The real integration
lives inside `preview.html` itself and already uses this exact `?token=`
mechanism for its existing MSE fetch - so this is a non-issue for the real
integration, but it is the reason a quick standalone reproduction (e.g. for
a future regression check) must not "simplify" to an Authorization header.**

With both bugs fixed and using the `?token=` form: repeated fresh-tab runs
against cam-garage decoded 145-185 frames per run with **arrival-to-render
averaging 0.6-1.7 ms** and decode queue depth 0 throughout - consistent with
the original prototype's numbers (1.0 ms avg, 14.6 ms max) and confirming
the approach is real, not a one-off measurement artifact.

## Decision: build it

Given the second verification, the user asked for Idea 1 to be implemented
as a real feature (not left as a proposal), with these constraints agreed:

- **Additive, not a replacement.** It ships as a new "Real-time" entry in
  `preview.html`'s existing latency selector (currently 0.5 s / 1.5 s /
  3 s), alongside the current MSE player - not instead of it.
- **Feature-detected and codec-gated**: only offered when
  `window.VideoDecoder` exists and the stream is H.264 (never H.265 - patchy
  platform support for that codec in WebCodecs). Old browsers / H.265
  cameras simply never see the option, rather than seeing it and having it
  fail.
- **Honest about the audio gap**: this path is video-only. What happens if
  the user unmutes while on "Real-time" is a real UX decision, not a detail
  - left to the implementer's judgment, documented with reasoning, not
    silently swallowed.
- Reuses the existing `?token=` auth flow and stream selector (`chn=0/1`)
  rather than duplicating them - it should read as a mode of the same
  preview, not a bolted-on second page.
- Built and verified on cam-garage specifically (fast live-patch to
  `/var/www/preview.html` via SSH for iteration, then a real
  package-rebuild + reflash to prove the build pipeline actually produces
  what was tested - a live-patch alone was explicitly called out as
  insufficient, since it can drift from what a real build produces).
- Not committed/pushed automatically - lands as a reviewable working-tree
  diff in `thingino-firmware-LuFi` (a *different* repo from timps' own C
  source; this feature needed zero changes to timpsd itself).

Status: implementation in progress at the time this section was written.
Whoever picks this up next should check whether an implementation report
has been added below this line, or check `git status`/`git diff` in
`thingino-firmware-LuFi` for `package/timps/files/www/preview.html` to see
the actual current state.
