# Audio backchannel (two-way audio)

The **backchannel** lets an RTSP/ONVIF client send audio *to* the camera so it
plays through the camera's speaker ("talk-back"). It is an **optional,
compile-time gated** feature — like rotation, TLS or SRT — so cameras that don't
need it (or have no speaker wired) pay nothing.

## How it works

timps implements the ONVIF audio backchannel (Profile T). When a client sends
`Require: www.onvif.org/ver20/backchannel` in `DESCRIBE`, timps advertises an
extra `m=audio` media (`a=sendonly`, `trackID=2`) in the SDP. The client then
`SETUP`s that track and streams RTP *to* the camera. timps receives the RTP,
decodes it to PCM16, resamples it, and pipes it to **`/bin/iac -s`** — the client
of thingino's `ingenic-audiodaemon`.

That daemon owns `IMP_AO` (the speaker device). timps **never opens `IMP_AO`
itself**, exactly like prudynt and raptor. The benefit: no device-exclusivity
conflict with portal sounds, motion tones, etc., and the output path is
completely HAL-free — it works the same on **every** SoC, as long as
`ingenic-audiodaemon` is installed.

```
client mic --RTP--> timps RTSP --decode(PCM16)--> /bin/iac -s --> iad --> IMP_AO --> speaker
```

## Codecs

| Codec | Build flag        | Decoder            | Notes                                              |
|-------|-------------------|--------------------|----------------------------------------------------|
| PCMU  | `USE_BACKCHANNEL` | pure C (g711.c)    | default. 8 kHz, lowest latency, universal ONVIF    |
| PCMA  | `USE_BACKCHANNEL` | pure C (g711.c)    | 8 kHz A-law                                         |
| AAC   | `USE_BC_AAC`      | libhelix-aac       | better quality, higher latency, extra dependency   |

G.711 needs **no external library** (decode is a few lines of pure C). AAC pulls
in `libhelix-aac` — the same decoder prudynt uses — so it is behind its own
separate switch `USE_BC_AAC` (which implies `USE_BACKCHANNEL`).

Why offer both: G.711 is the interoperable, low-latency default for interactive
talk-back; AAC sounds fuller and avoids the 8→16 kHz upsampling, but adds
~64 ms/frame latency and a decoder dependency. See the codec discussion in the
project notes.

## Requirements per SoC

The backchannel itself is SoC-independent — the decode + pipe path is portable C.
The only hard requirements are on the **device**, not the chip:

1. **`ingenic-audiodaemon` present** (`/bin/iac`). timps reports
   `caps.backchannel.available = 0` and refuses `SETUP` (406) if it is missing.
2. **A speaker is physically wired** (DAC + amplifier). Many Ingenic camera
   boards have no speaker at all — then the feature is pointless regardless of
   software.

## Duplex / limitations

- **Full-duplex data path**: the outgoing mic stream keeps running while the
  backchannel plays (same as prudynt/raptor).
- **One talker at a time**: the first RTSP client to send audio owns the speaker
  for its session; other clients' backchannel frames are dropped until it
  releases (at PLAY end / TEARDOWN).
- **No AEC** (acoustic echo cancellation): if the speaker sits close to the mic,
  expect echo. prudynt/raptor also omit AEC. Use push-to-talk in the client, or
  accept it. This is a hardware/UX concern, not a code bug.
- **UDP source check**: backchannel RTP is only accepted from the RTSP peer's IP.

## Cost

- **Binary**: G.711-only path ≈ +10–20 KB (no new library). AAC adds
  ~+60–100 KB (libhelix-aac).
- **RAM**: a couple of scratch buffers (~48 KB static) plus the `iac` child
  process; the audio buffers themselves are tiny (16 kHz mono = 32 KB/s).
- **CPU**: negligible for G.711; AAC decode is light on these SoCs.

## Build

Standalone:

```
# G.711 only
make target PLATFORM=T31 USE_BACKCHANNEL=1
# with AAC (point at the helix headers/lib)
make target PLATFORM=T31 USE_BC_AAC=1 HELIX_INC=/path/to/helix/include HELIXLIB=-lhelix-aac
```

`build.sh` flags: `-backchannel`, `-bc-aac` (the latter needs `HELIX_INC`/
`HELIXLIB` in the environment).

In thingino, enable `BR2_PACKAGE_TIMPS_BACKCHANNEL` (and optionally
`BR2_PACKAGE_TIMPS_BC_AAC`, which pulls in `libhelix-aac`).

`BR2_PACKAGE_TIMPS_BACKCHANNEL` deliberately does **not** select
`ingenic-audiodaemon` — timps only runtime-execs `/bin/iac`, it never links the
daemon (prudynt/raptor do the same). Forcing it would drag in `libwebsockets`,
which fails to build on some uClibc toolchains. So for a working speaker you
must **also enable `BR2_PACKAGE_INGENIC_AUDIODAEMON` yourself** (or already have
`/bin/iac` on the device). Without it the backchannel just reports unavailable.

## Config

```
audio.backchannel        = 1        # master on/off (restart-required)
audio.backchannel_codec  = pcmu     # pcmu | pcma | aac
audio.backchannel_rate   = 16000    # speaker sample rate fed to iac (Hz)
```
