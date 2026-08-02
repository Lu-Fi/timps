#ifndef MS_ROTATE_CAPS_H
#define MS_ROTATE_CAPS_H
/* Per-SoC image-rotation capabilities. Each macro is switched on only in the
 * commit that implements its apply path; Batch 1 leaves the target apply
 * macros OFF (behaviour-neutral). The host sim advertises everything.
 *
 * The ENTIRE body is a compile-time opt-in via USE_ROTATE (Makefile knob /
 * BR2_PACKAGE_TIMPS_ROTATE, mirroring USE_CONTROL/USE_TLS/USE_FAAC). When
 * USE_ROTATE is undefined NO ROT_HAS_* macro is defined, so every
 * `#ifdef ROT_HAS_*` block in the code compiles out and config.c's prot()
 * coerces every 90/270 request to 0 - the feature costs zero bytes.
 *
 * NOTE: rotation=180 is NO LONGER a rotation value. On every classic-API SoC
 * it was only ever a global ISP Hflip+Vflip - identical to (and made redundant
 * by) the always-available image.hflip + image.vflip, but falsely modelled as
 * per-stream (setting it on one stream silently flipped all others). It was
 * removed; users wanting a 180 flip set image.hflip=1 + image.vflip=1. So
 * `rotation` now only ever means a real transpose (90/270) or 0/absent. */
#ifdef USE_ROTATE
#if defined(PLATFORM_T10)||defined(PLATFORM_T20)||defined(PLATFORM_T21)|| \
    defined(PLATFORM_T23)||defined(PLATFORM_T30)||defined(PLATFORM_T31)|| \
    defined(PLATFORM_T40)||defined(PLATFORM_T41)||defined(PLATFORM_C100)
#define ROT_PLATFORM_KNOWN 1
#endif

/* 90/270 apply paths - each enabled by the batch that implements it:
 *   ROT_HAS_HW_I2D    (T40/T41, Batch 3) - true HW I2D rotate
 *   ROT_HAS_FS_ROTATE (T31, Batch 4)     - libimp FrameSource software rotate
 *   ROT_HAS_SW_90     (T23 + -DMS_ENABLE_SW_ROTATE, Batch 5) - own NV12
 *     transpose + libimp's unbound YuvEncode API (hal_ingenic.c sw_rot path)
 * On the host sim (no PLATFORM_* -> !ROT_PLATFORM_KNOWN) enable 90 so the
 * plumbing and caps can be exercised. */
#if defined(PLATFORM_T40)||defined(PLATFORM_T41)
#define ROT_HAS_HW_I2D 1
#endif
#if defined(PLATFORM_T31)
#define ROT_HAS_FS_ROTATE 1
#endif
/* T23 (Batch 5): 90/270 in SOFTWARE (CPU transpose per frame, then the unbound
 * IMP_Encoder_Yuv* encode path). Deliberately OPT-IN via the USE_SW_ROTATE=1
 * Makefile knob (-DMS_ENABLE_SW_ROTATE): it costs real CPU on the single-core
 * T23 and loses hardware OSD/privacy on the rotated stream. Without the knob a
 * T23 build advertises no rotation values at all (ROT_HAS_90 undefined). */
#if defined(PLATFORM_T23) && defined(MS_ENABLE_SW_ROTATE)
#define ROT_HAS_SW_90 1
#endif
/* ROT_HAS_90 gates the config whitelist for 90/270 (config.c coerces 90/270->0
 * when it is undefined). Define it wherever a real 90/270 apply path exists
 * (HW I2D, FS rotate or the opt-in T23 SW rotate) or on the host sim. SoCs
 * without an apply path leave it undefined, so 90/270 still coerces to 0. */
#if defined(ROT_HAS_HW_I2D)||defined(ROT_HAS_FS_ROTATE)||defined(ROT_HAS_SW_90)||!defined(ROT_PLATFORM_KNOWN)
#define ROT_HAS_90 1
#endif
#endif /* USE_ROTATE */
#endif
