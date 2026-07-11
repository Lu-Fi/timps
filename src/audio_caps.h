/* audio_caps.h - per-SoC audio (IMP_AI) capability matrix, the audio sibling
 * of isp_caps.h.
 *
 * One place decides which audio.* keys the running build can drive LIVE:
 *   - src/hal/hal_ingenic.c guards the IMP_AI_* calls with these macros
 *     (ai_apply_key()),
 *   - src/control.c derives the "caps":{"audio":[...]} list that GET /control
 *     reports so the WebUI can grey out unsupported controls.
 * Keep both consumers in sync with this header.
 *
 * Verified against the vendored SDK headers (include/<SoC>/<ver>/../imp_audio.h):
 *   IMP_AI_SetVol/SetGain, IMP_AI_EnableHpf/DisableHpf,
 *   IMP_AI_EnableAgc/DisableAgc (IMPAudioAgcConfig),
 *   IMP_AI_EnableNs/DisableNs            -> all SoCs (T10 T20 T21 T23 T30
 *                                           T31 T40 T41 C100)
 *   IMP_AI_SetAlcGain (analog PGA gain)  -> T21 T31 C100 only
 * (IMP_AO_SetVol/SetGain exist everywhere too, but timps has no audio output
 * pipeline, so speaker keys are persist-only - see control.c/hal_ingenic.c.)
 *
 * NOT here on purpose: codec / samplerate / bitrate / channels / enabled are
 * IMP_AI_SetPubAttr- or encoder-init-time attributes, never live calls; they
 * persist to the config and take effect on the next restart.
 *
 * A build without any PLATFORM_* macro (host sim) enables everything so the
 * WebUI can be exercised against timpsd-sim. */
#ifndef MS_AUDIO_CAPS_H
#define MS_AUDIO_CAPS_H

#if defined(PLATFORM_T10)||defined(PLATFORM_T20)||defined(PLATFORM_T21)|| \
    defined(PLATFORM_T23)||defined(PLATFORM_T30)||defined(PLATFORM_T31)|| \
    defined(PLATFORM_T40)||defined(PLATFORM_T41)||defined(PLATFORM_C100)
#define AUDIO_PLATFORM_KNOWN 1
#endif

/* IMP_AI_SetAlcGain (analog level control / PGA gain).
 * Note: a T10 build compiles against the T20 headers (see Makefile), which
 * do not declare it either, so T10 stays without ALC. */
#if defined(PLATFORM_T21)||defined(PLATFORM_T31)||defined(PLATFORM_C100)|| \
    !defined(AUDIO_PLATFORM_KNOWN)
#define AUDIO_HAS_ALC_GAIN 1
#endif

#endif /* MS_AUDIO_CAPS_H */
