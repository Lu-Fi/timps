# Audio

timps handles audio in three separate but related capacities: **capture**
(microphone → RTSP/HTTP/SRT/recording), the **ONVIF-style two-way
backchannel** (RTSP client → camera speaker), and a **system-sound play
queue** (local WAV/PCM/G.711/Opus files → camera speaker). The backchannel
and play queue share one speaker owner module, `src/rtsp/speaker.c`, which
drives the Ingenic `IMP_AO` device **natively** — there is no dependency
on an external `/bin/iac`/`play` helper process. (`docs/backchannel.md` at
the repository root predates this native-`IMP_AO` design and still
describes the old `/bin/iac`-based architecture; treat this wiki page as
current and that file as historical.)

## Capture

Configured under `audio.*` (see
[Configuration Reference](Configuration-Reference.md#audio--capture-encode-speaker-defaults)).

| Codec | Library | Notes |
| --- | --- | --- |
| **AAC** | `libfaac` (`USE_FAAC`) | Default codec. Needed for audio on the HTTP fMP4 preview and SRT (neither can carry G.711 — see [Streaming Protocols](Streaming-Protocols.md)). `src/codec/aac.c` supplies the ADTS/AudioSpecificConfig bookkeeping (sample-rate table lookup, 2-byte ASC construction, ADTS-header stripping) that both the RTSP SDP builder and the SRT TS mux use — the actual encode call lives in the HAL. |
| **G.711 (µ-law/A-law)** | none — `src/codec/g711.c` is pure C (canonical Sun/CCITT expand/compress tables) | Always available, no library dependency. RTSP-only for capture playback; cannot be muxed into fMP4 or SRT's TS mux. |
| **Opus** | bare `libopus` **encoder** (`USE_STREAM_OPUS`, ~337 KB) — **not** `opusfile`/`libogg` | Compile-time optional (off by default). Select with `audio.codec=opus`; unrecognized on a build without `USE_STREAM_OPUS`. The mic is encoded at its capture rate (16 kHz default) in `OPUS_APPLICATION_VOIP` mode as one Opus frame per 40 ms AI capture frame; `audio.bitrate_kbps` sets the encoder bitrate (default 32). RTSP/RTP only (RFC 7587), exactly like G.711 — **not** muxable into fMP4 or SRT. The RTP track is always advertised as `opus/48000/2` regardless of the real 16 kHz/mono encoding; the actual mono layout is signalled out-of-band via `sprop-stereo=0` and `rtp_send_opus()` timestamps against the mandatory 48 kHz clock. If `opus_encoder_create` fails at stream start, timps falls back to PCMU. Entirely separate from the play-queue `USE_PLAY_OPUS` decode feature below. |

`audio.channels=2` / `audio.force_stereo=1` produce "simulated stereo": the
mono mic signal duplicated to L=R, AAC only — not a genuine stereo
capture path (the hardware is mono).

Live-applicable capture knobs (`audio.volume`, `gain`, `alc_gain`,
`mute`) go straight to `IMP_AI_Set*` calls; `high_pass`/`agc`/`ns`/
`agc_target_dbfs`/`agc_compression_db` are restart-only **by design**, not
merely unimplemented: libimp runs those on its own vendor audio-processing
thread and frees state without external locking, so a live toggle would
race that thread (a real use-after-free crash class this restart-only
rule specifically avoids). `audio.mute` is the one exception that *is*
live everywhere — it's a plain publish-gate inside timps (captured
frames are dropped before reaching the encoder/hub), not an SDK call, so
it works identically on every platform.

## The speaker: sole `IMP_AO` owner (`src/rtsp/speaker.c`)

`speaker.c` is the single point of contact with the physical speaker
device, shared by two independent producers that never talk to `IMP_AO`
directly: the backchannel and the play queue. It owns:

- **Lazy AO lifecycle** — opens `IMP_AO` on first use (`hal_ao_open`,
  applying `audio.spk_volume`/`spk_gain` as the initial values) and closes
  it when idle. `audio.spk_enabled` is the master gate: if `0`, the AO
  device is never opened at all, silencing both producers through this
  one check.
- **Ownership arbitration** — exactly one producer (a backchannel RTSP
  session, or the play queue) holds the AO at a time. **The backchannel
  always preempts**: if the play queue currently holds the speaker, it is
  signaled to yield immediately (discarding, not draining, whatever it
  was playing) so the real-time conversational path never waits behind a
  system sound. The play queue, conversely, never preempts anything — it
  yields to an active backchannel and waits for it to release before
  resuming.
- **Live volume/gain** — `speaker_set_volume`/`speaker_set_gain` apply
  immediately to whichever producer currently holds `IMP_AO`, and persist
  as the default for whichever producer opens it next. This is what
  `audio.spk_volume`/`spk_gain`'s "Live" classification in the
  [Configuration Reference](Configuration-Reference.md) actually means.
- **Shared resampler** (`src/codec/resample.c`) — a simple mono int16
  linear-interpolation resampler used by both producers to convert their
  native rate (backchannel's negotiated RTP rate, or a play file's native
  rate) to whatever rate `IMP_AO` was actually opened at. Its output
  buffer is sized to fit the *actual* required output length rather than
  a fixed cap, specifically because the resampler silently truncates if
  its output buffer is too small — undersizing it here previously
  dropped audio whenever the output rate was much higher than the source
  rate (e.g. an 8kHz µ-law backchannel resampled up to a 48kHz-configured
  AO).

## ONVIF audio backchannel (`src/rtsp/backchannel.c`, `USE_BACKCHANNEL`)

Two-way audio: an RTSP client `SETUP`s a second track (`trackID=2`) and
streams RTP audio *to* the camera, which decodes it and plays it through
the speaker — the standard ONVIF Profile T backchannel mechanism. Off by
default; enabling it is restart-only (`audio.backchannel`, see
[Configuration Reference](Configuration-Reference.md#audio--capture-encode-speaker-defaults)).

- **Codecs**: G.711 PCMU/PCMA always (pure C, `g711.c`); AAC additionally
  with `USE_BC_AAC` (needs `libhelix-aac`).
- **Echo cancellation** (`audio.aec`, opt-in, off by default, **live** —
  see [Configuration Reference](Configuration-Reference.md#audio--capture-encode-speaker-defaults)):
  engages `IMP_AI_EnableAec` to subtract the speaker's own output from
  the mic capture, so the far end doesn't hear itself echoed back through
  the camera's mic during a two-way conversation. It only engages once
  both the mic capture (`IMP_AI`) and the speaker output (`IMP_AO`) are
  actually live — a play-only session with no active capture skips it —
  and is applied the next time the AO device is opened, the same timing
  contract as `spk_volume`/`spk_gain`. Off by default because AEC quality
  and added latency vary considerably by SoC/microphone/speaker pairing.
- **RTSP wiring** (see [Streaming Protocols](Streaming-Protocols.md)): the
  track is only advertised in the SDP if the client's `Require:` header
  names `www.onvif.org/ver20/backchannel` **and** the feature was
  actually configured at boot (`bc_available()` — a boot-time flag, not
  the live config value, precisely because `audio.backchannel` is
  restart-only: a live `/control` toggle must not advertise a backchannel
  whose codec/rate parameters haven't actually taken effect yet). Both
  TCP-interleaved and UDP transport are supported for the reverse audio
  path; incoming UDP datagrams are filtered to the RTSP peer's own source
  IP as a basic anti-injection measure.
- **Session election**: the *first* RTSP session to actually feed an RTP
  packet becomes the exclusive backchannel decode owner; every other
  concurrent session's packets are dropped until that owner releases (at
  `TEARDOWN` or connection loss) — this is a separate, RTP-decode-level
  arbitration layered on top of speaker.c's AO-device arbitration.
- **RTP handling**: validates RTP version/header length, strips CSRC/
  extension headers and any padding, and — importantly — only accepts
  packets whose RTP payload type matches the negotiated codec, which also
  incidentally filters out any RTCP arriving on the same logical
  transport.
- **AAC decode safety** (`USE_BC_AAC`, via libhelix-aac): the RFC 3640
  AU-header section is parsed to find each access unit's boundaries
  within a packet; because `AACDecode()` can write a full worst-case
  block (2 channels × 1024 samples) into the output buffer before the
  code can clamp the running total, the loop guards on having room for a
  *full* worst-case block remaining, not just "some room" — closing off a
  buffer-overrun path a network-controlled packet with many AU headers
  could otherwise trigger.

```sh
# ffmpeg-based two-way test tooling: scripts/bc-send.py, scripts/bc-talk.py
```

## Browser push-to-talk (`/talk`, `USE_BC_WS`)

A browser cannot speak RTSP/RTP, so the ONVIF backchannel above is
unreachable from a web page. `USE_BC_WS` (`BR2_PACKAGE_TIMPS_BC_WS`, off by
default) adds an RFC 6455 WebSocket at `/talk` that feeds the *same* speaker
path and the same single-talker arbitration: the client sends 20 ms
**binary** frames of G.711 mu-law, timps decodes them and hands the PCM to
`bc_feed_pcm()`. `USE_BC_WS` implies `USE_BACKCHANNEL` + `USE_CONTROL` (the
`?token=` machinery lives there — a `WebSocket` constructor cannot set
request headers). `scripts/talk-ws.py` speaks the protocol without a
browser.

`USE_TLS` is **recommended but optional**: `ws.c` terminates nothing itself,
so the plain-`ws://` code path links without mbedTLS at all. Whether the
port actually requires TLS is a *runtime* choice, `audio.talk_ws`
(restart-required, default `0`; see
[Configuration Reference](Configuration-Reference.md#audio--capture-encode-speaker-defaults)):

| `audio.talk_ws` | `/talk` |
| --- | --- |
| `0` | not served |
| `1` | served over TLS only — a plaintext port answers `426`. The package default on TLS builds; unsatisfiable (and warned about at startup) on a build without `USE_TLS` |
| `2` | served over `wss://` when the port is TLS, **and** over plain `ws://` when it is not |

`2` is a deliberate hand edit, never preset: the microphone audio and its
token then cross the network in the clear. `GET /control` reports the
resolved answer as `caps.backchannel.talk_ws` (`0`/`1`/`2`, where `1` on a
plaintext port resolves to `0`), so the WebUI needs no second test.

### Granting the browser a secure context for a plain-`http://` camera

`talk_ws=2` only removes the *server's* TLS requirement. The *browser*
separately refuses `getUserMedia()` — and, unrelated to audio, the thingino
WebUI's WebCodecs real-time preview mode, gated on `window.VideoDecoder` —
on a plain `http://` origin unless something grants that origin a **secure
context**. No browser treats private/LAN IP ranges as trustworthy by
default (verified against Chromium/Firefox/WebKit source and MDN — the
secure-context algorithm is otherwise consistent across all three: https,
`wss:`, `127.0.0.0/8`, `::1`, `localhost`, `file:`, nothing else), so a
`http://192.168.x.x` camera is an insecure context everywhere without one
of the overrides below.

**Chrome / Edge / Brave** — `chrome://flags/#unsafely-treat-insecure-origin-as-secure`
(`edge://flags/...`, `brave://flags/...` — same flag, compiled into
Chromium):

1. Open the flag URL above.
2. In the **Insecure origins treated as secure** text box, enter the
   camera's origin **including any non-default port**, e.g.
   `http://192.168.10.21` (port 80) or `http://192.168.10.21:8080`. Scheme
   required, no trailing slash. Multiple origins: comma-separated;
   wildcard host patterns are also accepted.
3. Set the dropdown next to it to **Enabled**.
4. Relaunch the browser when prompted.

This flips `window.isSecureContext` to `true` for the listed origin(s) —
confirmed in the Chromium source (`net/base/is_potentially_trustworthy.cc`,
the allowlist feeds the same `IsOriginPotentiallyTrustworthy()` check that
computes `isSecureContext`), so it unlocks **every** secure-context API,
`getUserMedia()` and `VideoDecoder` alike, not just audio. The setting is
stored browser-wide (in the installation's `Local State` file, not per
profile) and persists across restarts — no command-line flag needed on
each launch, and no per-tab or per-session repetition.

**Firefox** has two different overrides depending on what you need:

- `dom.securecontext.allowlist` in `about:config` — a comma-separated list
  of **hostnames** (no scheme, no port — `192.168.10.21`, not
  `http://192.168.10.21` or `192.168.10.21:8080`). This is Firefox's
  equivalent of the Chromium flag: matching hosts get `isSecureContext ===
  true` (verified in `nsGlobalWindowOuter::ComputeIsSecureContext` →
  `nsMixedContentBlocker::IsPotentiallyTrustworthyOrigin`), unlocking both
  `getUserMedia()` **and** `VideoDecoder` — this is the one to use if you
  also want the WebCodecs preview to work.
- `media.devices.insecure.enabled` **and**
  `media.getusermedia.insecure.enabled`, both `true` in `about:config` —
  an older, narrower pair (both required together; the first re-exposes
  `navigator.mediaDevices` on `http:`, the second then permits
  `getUserMedia()` itself). Two real differences from the option above:
  it's **global** (every `http://` site, not just the camera's origin —
  there's no host list), and it only bypasses the `mediaDevices`/gUM gate
  specifically — it does **not** flip `isSecureContext`, so `VideoDecoder`
  stays unavailable regardless. Prefer `dom.securecontext.allowlist`
  unless you specifically want the broader, audio-only override.

**Safari** has a narrower, WebRTC-specific override, not a general
secure-context one: **Develop → WebRTC → Allow Media Capture on Insecure
Sites** (macOS; on iOS, reachable via remote Web Inspector from a Mac,
Device Settings pane). This permits `getUserMedia()` on `http://` pages, so
push-to-talk (`talk_ws=2`) works. There is no Safari equivalent of the
Chromium/Firefox host-allowlist — `isSecureContext` itself never becomes
true for a plain `http://` origin — so the WebCodecs preview stays
unavailable on Safari regardless of this toggle.

None of the above is needed at all against `http://localhost` (or a tunnel
that makes the camera appear there, e.g. `ssh -L 8880:127.0.0.1:8880
root@camera`): browsers treat `localhost` as a secure context unconditionally
(Firefox only since version 84; check your version if relying on this).

## System-sound play queue (`USE_PLAY`)

A FIFO at `/run/timps/audio_out` that speaks the **same line protocol**
prudynt/raptor's `/usr/sbin/play` wrapper does, so anything already built
against that convention — the thingino WiFi captive-portal prompt, the
post-upgrade chime, a Home Assistant ESPHome `media_player`/TTS
integration — gets a working speaker for free once this feature is
compiled in.

```
PLAY url=<path> [format=wav|pcm|opus] [vol=N] [gain=N] [rate=N] [loop=N] [delay=N]
STOP
```

- `format=` is auto-detected from the file's leading bytes if omitted
  (`OggS` → Opus, `RIFF` → WAV, else raw PCM16).
- `vol=`/`gain=` default to "leave the AO at its current setting" (`-1`).
- `rate=` is only meaningful for raw PCM (WAV carries its own rate in the
  header; Opus is always 48000 via `opusfile`).
- `loop=` is clamped to 1–32 repetitions; `delay=` (ms between loops) to
  0–5000.
- The queue holds exactly **one pending slot** — a new `PLAY` (or `STOP`)
  always immediately preempts whatever was queued or playing; this is a
  latest-wins slot, not an accumulating playlist (an `append=` parameter
  is accepted for wire-protocol compatibility but silently ignored).

**Decoders built in**: WAV/RIFF (PCM, and A-law/µ-law payloads inside a
WAV container), raw PCM16 mono, and — with `USE_PLAY_OPUS` — Ogg-Opus via
`opusfile`. All are decoded in fixed-size blocks (4096 mono frames) and
downmixed to mono if the source is stereo.

`POST /control {"speaker":{"play":"<file>"}}` (see
[HTTP /control API Reference](HTTP-Control-API.md)) resolves the given
filename against `/usr/share/sounds` — rejecting any `/` or `..` — and
enqueues it on the FIFO; `{"speaker":{"stop":1}}` stops playback. Neither
is persisted to the config file; both are transient actions.

`GET /control`'s `caps.play.sounds` array live-enumerates
`/usr/share/sounds` (capped at 96 entries to keep the JSON response
bounded) so a WebUI test-sound picker only ever offers files this exact
build can actually decode: `.wav`/`.ulaw` unconditionally, `.opus` only
when `USE_PLAY_OPUS` was actually compiled in.

## Config keys

See [Configuration Reference](Configuration-Reference.md#audio--capture-encode-speaker-defaults)
for the full `audio.*` table (capture, backchannel, speaker defaults) —
`audio.backchannel`/`backchannel_codec`/`backchannel_rate` for the
backchannel, `audio.spk_enabled`/`spk_volume`/`spk_gain` for the shared
speaker defaults. There is no dedicated `play.*` section — the play
queue's behavior is entirely protocol-driven (the FIFO commands above)
plus the `USE_PLAY`/`USE_PLAY_OPUS` build flags.
