# Known limitation: talk/backchannel use can leave the mic noisier until restart

**Date:** 2026-09-08
**Camera:** Garage (`wuuk_y0510_t31x_sc4336p_ssv6158`, T31X/sc4336p)
**Status:** understood, not fixed - documenting so this doesn't get re-investigated from scratch

## Symptom

Live audio was clean right after tuning `audio.{gain,alc_gain,volume,high_pass}`
via `/control`. Pressing the WebUI's push-to-talk button (`#ms-talk` on
`preview.html`, not the control-bar mute button) and then releasing it left
the mic audibly noisier than before, and it stayed that way - `POST
{"audio":{"aec":0}}` did not fix it, only a full `timps` restart did.

## Root cause

`preview-talk.js`'s WebSocket talk session -> `talk_ws.c` -> `speaker.c`'s
`speaker_write_pcm()` -> `ao_ensure()` -> `hal_ao_open()`
(`src/hal/hal_ingenic.c:5178-5253`) calls `IMP_AI_EnableAec(0,0,0,0)` on the
**AI (mic) channel**, not just the AO/speaker side, whenever `audio.aec=1`.

`IMP_AI_EnableAec` is not narrow echo-cancellation - it instantiates the
vendor's full WebRTC audio-processing chain on the mic, configured entirely
by `/etc/webrtc_profile.ini` (shipped by `ingenic-sdk`, independent of any
timps setting):

```ini
[AGC] AGC_enable=true
      set_mode=kFixedDigital
      set_target_level_dbfs=4
      set_compression_gain_db=15
      enable_limiter=true
[HP]  HP_enable=true
[NS]  NS_enable=true
      set_level=kHigh
```

A fixed-digital AGC with 15 dB of compression gain pulls the noise floor up
in speech pauses; `kHigh` noise suppression adds the classic musical-
noise/gurgle artifact on top. Layered on our already-boosted gain stack
(`gain=31`, `alc_gain=1`, `volume=100`, roughly +25 dB over stock defaults),
the result is clearly audible.

`hal_ao_close()` calls `IMP_AI_DisableAec(0,0)` on session end, but per
timps' own comments on the neighboring HPF/AGC/NS code
(`hal_ingenic.c:4293-4302`), these vendor DSP modules run on their own
internal record thread inside `libaudioProcess.so` and are not safely
resettable while the AI channel stays open - disabling one does not
reliably restore the pre-engage state. Confirmed empirically: with
`audio.aec=0` persisted, a fresh talk session (logged, no `AEC enabled`
line) still left the mic noisy until `timps` was actually restarted.

## Current handling

- `high_pass` is now on by default (`src/config.c`, 2026-09-08) - a real
  improvement independent of this bug, unaffected by it.
- `timps.conf.example` and the WebUI's `config-audio.html` now correctly
  document that `high_pass`/`agc`/`ns` are restart-required (never live) and
  that `aec` persists immediately but only engages at the next AO open - and
  call out this noise-until-restart behavior explicitly.
- No code fix attempted: the actual defect (module state not resetting on
  `IMP_AI_DisableAec`) lives inside the closed vendor SDK
  (`libaudioProcess.so`), not in timps. The vendor's own header
  (`include/T31/1.1.6/en/imp/imp_audio.h:503-538`) already hints AEC quality
  is not great; not chasing this further unless it starts blocking a real
  use case.

## If this comes up again

- Workaround: restart `timps` after a talk/backchannel session if the mic
  sounds off. On any camera with `audio.backchannel=1` + a talk button
  exposed, expect this.
- If AEC needs to stay usable without the noise, the tuning knob is
  `/etc/webrtc_profile.ini` itself (`AGC_enable=false` and/or
  `NS_enable=false`, or dial back `set_compression_gain_db`/`set_level`) -
  untested, not yet tried on real hardware.
- Camera-specific gain tuning (`gain=31`/`alc_gain=1`/`volume=100`) is only
  applied to Garage so far, live via `/control`, not yet reflected in the
  repo's `user/wuuk_y0510_t31x_sc4336p_ssv6158/192.168.10.21/overlay/etc/timps.conf`
  - a future rebuild+reflash of Garage would revert it unless that overlay
  is updated too.
