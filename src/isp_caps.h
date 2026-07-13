/* isp_caps.h - per-SoC ISP tuning capability matrix.
 *
 * One place decides which image.* keys the running build can actually drive:
 *   - src/hal/hal_ingenic.c guards the IMP_ISP_Tuning_* calls with these
 *     macros (isp_apply_image()),
 *   - src/control.c derives the "caps":{"image":[...]} list that GET /control
 *     reports so the WebUI can grey out unsupported controls.
 * Keep both consumers in sync with this header.
 *
 * Verified against the vendored SDK headers (include/<SoC>/<ver>/../imp_isp.h):
 *   SetBrightness/Contrast/Saturation/Sharpness, SetAntiFlickerAttr,
 *   SetISPRunningMode, SetSensorFPS      -> all SoCs
 *   SetBcshHue (hue)                     -> T23 T31 T40 T41 C100
 *   SetBacklightComp/SetDefog_Strength/
 *   SetDPC_Strength                      -> T23 T31 C100
 *   SetDRC_Strength (WDR)                -> T21 T23 T31 C100
 *   SetHiLightDepress, SetSinter/Temper,
 *   SetMaxAgain/SetMaxDgain, SetWB,
 *   SetISPHflip/SetISPVflip              -> T10 T20 T21 T23 T30 T31 C100
 *   SetAeComp                            -> T10 T20 T23 T30 T31 C100 (not T21)
 *   SetHVFLIP                            -> flip path on T40/T41
 * A build without any PLATFORM_* macro (host sim) enables everything so the
 * WebUI can be exercised against timpsd-sim. */
#ifndef MS_ISP_CAPS_H
#define MS_ISP_CAPS_H

#if defined(PLATFORM_T10)||defined(PLATFORM_T20)||defined(PLATFORM_T21)|| \
    defined(PLATFORM_T23)||defined(PLATFORM_T30)||defined(PLATFORM_T31)|| \
    defined(PLATFORM_T40)||defined(PLATFORM_T41)||defined(PLATFORM_C100)
#define ISP_PLATFORM_KNOWN 1
#endif

/* T40/T41: reworked tuning API (IMPVI_NUM + pointer arguments) with a
 * reduced tuning feature set; flips go through SetHVFLIP. */
#if defined(PLATFORM_T40)||defined(PLATFORM_T41)
#define ISP_NEW_TUNING_API 1
#endif

/* IMP_ISP_Tuning_SetBcshHue */
#if defined(PLATFORM_T23)||defined(PLATFORM_T31)||defined(PLATFORM_T40)|| \
    defined(PLATFORM_T41)||defined(PLATFORM_C100)||!defined(ISP_PLATFORM_KNOWN)
#define ISP_HAS_HUE 1
#endif

/* IMP_ISP_Tuning_SetBacklightComp / SetDefog_Strength / SetDPC_Strength */
#if defined(PLATFORM_T23)||defined(PLATFORM_T31)||defined(PLATFORM_C100)|| \
    !defined(ISP_PLATFORM_KNOWN)
#define ISP_HAS_BACKLIGHT 1
#define ISP_HAS_DEFOG 1
#define ISP_HAS_DPC 1
#endif

/* IMP_ISP_Tuning_SetDRC_Strength (wide dynamic range) */
#if defined(PLATFORM_T21)||defined(PLATFORM_T23)||defined(PLATFORM_T31)|| \
    defined(PLATFORM_C100)||!defined(ISP_PLATFORM_KNOWN)
#define ISP_HAS_DRC 1
#endif

/* classic-API-only tunings, absent from the T40/T41 SDK:
 * SetHiLightDepress, SetSinterStrength/SetTemperStrength,
 * SetMaxAgain/SetMaxDgain, SetWB, SetISPHflip/SetISPVflip */
#if !defined(ISP_NEW_TUNING_API)
#define ISP_HAS_HILIGHT 1
#define ISP_HAS_NR 1
#define ISP_HAS_GAINS 1
#define ISP_HAS_WB 1
#endif

/* IMP_ISP_Tuning_SetAeComp (missing from the T21 SDK as well) */
#if !defined(ISP_NEW_TUNING_API) && !defined(PLATFORM_T21)
#define ISP_HAS_AECOMP 1
#endif

/* IMP_ISP_Tuning_GetAeLuma (AE average luminance) - T21 T23 T31 C100 only;
 * used by daynight as a secondary photosensing metric (raptor's ae_luma) */
#if defined(PLATFORM_T21)||defined(PLATFORM_T23)||defined(PLATFORM_T31)|| \
    defined(PLATFORM_C100)||!defined(ISP_PLATFORM_KNOWN)
#define ISP_HAS_AELUMA 1
#endif

#endif /* MS_ISP_CAPS_H */
