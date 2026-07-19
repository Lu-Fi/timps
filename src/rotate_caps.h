#ifndef MS_ROTATE_CAPS_H
#define MS_ROTATE_CAPS_H
/* Per-SoC image-rotation capabilities. Each macro is switched on only in the
 * commit that implements its apply path; Batch 1 leaves the target apply
 * macros OFF (behaviour-neutral). The host sim advertises everything. */
#if defined(PLATFORM_T10)||defined(PLATFORM_T20)||defined(PLATFORM_T21)|| \
    defined(PLATFORM_T23)||defined(PLATFORM_T30)||defined(PLATFORM_T31)|| \
    defined(PLATFORM_T40)||defined(PLATFORM_T41)||defined(PLATFORM_C100)
#define ROT_PLATFORM_KNOWN 1
#endif

/* 180 = ISP Hflip+Vflip (or per-channel I2D on T40/T41): all camera SoCs.
 * Enabled now because 180 needs no dim swap and Batch 2 provides the apply;
 * to stay behaviour-neutral in Batch 1 the config whitelist still coerces
 * unsupported values, and no apply path references these yet. */
#define ROT_HAS_180 1

/* 90/270 apply paths - all OFF until their batch:
 *   ROT_HAS_HW_I2D    (T40/T41, Batch 3)
 *   ROT_HAS_FS_ROTATE (T31, Batch 4)
 *   ROT_HAS_SW_90     (T23 + -DMS_ENABLE_SW_ROTATE, Batch 5)
 * On the host sim (no PLATFORM_* -> !ROT_PLATFORM_KNOWN) enable 90 so the
 * plumbing and caps can be exercised. */
#ifndef ROT_PLATFORM_KNOWN
#define ROT_HAS_90 1
#endif
#endif
