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

/* Capping the AE's maximum integration time (image.ae_it_max_us).
 *
 * Two different SDK spellings, split by SoC generation (verified 2026-09-06
 * against the vendored headers):
 *   ISP_HAS_AE_IT_MAX   - IMP_ISP_Tuning_SetAe_IT_MAX(unsigned int), on
 *                         T23/T31/C100. The header documents no unit; measured
 *                         on cam-garage (T31X/sc4336p) it is SENSOR LINES, the
 *                         same unit GetExpr reports and reads back through it.
 *   ISP_HAS_AE_IT_RANGE - IMP_ISP_Tuning_SetIntegrationTime(IMPISPITAttr*), on
 *                         T10/T20/T21/T30 - the older SDK, which expresses the
 *                         same thing as a mode + integration_time +
 *                         max_integration_time triple.
 * Neither exists in the T40/T41 reworked tuning API, so the key is inert
 * there (and F_CAP-gated out of GET /control's caps list). */
#if defined(PLATFORM_T23)||defined(PLATFORM_T31)||defined(PLATFORM_C100)|| \
    !defined(ISP_PLATFORM_KNOWN)
#define ISP_HAS_AE_IT_MAX 1
#endif
#if defined(PLATFORM_T10)||defined(PLATFORM_T20)||defined(PLATFORM_T21)|| \
    defined(PLATFORM_T30)
#define ISP_HAS_AE_IT_RANGE 1
#endif

/* IMP_ISP_Tuning_GetExpr + IMP_ISP_Tuning_GetEVAttr (AE exposure readback).
 *
 * Declared, with an IDENTICAL IMPISPExpr union layout (verified 2026-09-06
 * against T20/3.12.0, T21/1.0.33, T23/1.3.0, T30/1.0.5, T31/1.1.6 and
 * C100/2.1.0), on every classic-tuning SoC; absent from the T40/T41 reworked
 * tuning API. GetExpr's g_attr publishes integration_time / _min / _max in
 * SENSOR LINES plus one_line_expr_in_us, so the exposure can be expressed in
 * real microseconds without knowing the sensor mode; GetEVAttr adds expr_us,
 * again and dgain.
 *
 * This is what daynight.c's g_int_hwm high-water-mark exists to guess: the
 * /proc dump on the T20s publishes "SENSOR Integration Time" but no maximum,
 * and dn_read() therefore estimates the maximum as the longest exposure it has
 * ever seen. GetExpr answers it directly. See dn_read()'s dual-read. */
#if !defined(ISP_NEW_TUNING_API)
#define ISP_HAS_EXPR 1
#endif

/* IMP_ISP_Tuning_GetSensorAttr (real sensor output resolution) - T23 T31 T32
 * T33 T40 T41 C100. Used to declare the framesource input resolution when the
 * sensor driver reports 0x0 to the framesource (e.g. sc2336). T40/T41 take an
 * extra IMPVI_NUM arg (ISP_NEW_TUNING_API); the others use the plain form.
 * Absent on T10/T20/T21/T30. */
#if defined(PLATFORM_T23)||defined(PLATFORM_T31)||defined(PLATFORM_T32)|| \
    defined(PLATFORM_T33)||defined(PLATFORM_T40)||defined(PLATFORM_T41)|| \
    defined(PLATFORM_C100)||!defined(ISP_PLATFORM_KNOWN)
#define ISP_HAS_SENSOR_ATTR 1
#endif

#endif /* MS_ISP_CAPS_H */
