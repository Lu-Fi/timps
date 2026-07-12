/* motion_caps.h - per-SoC/SDK IMP_IVS motion-detection capability, the motion
 * sibling of isp_caps.h/audio_caps.h.
 *
 * One place decides whether this build can do IMP_IVS "move" detection and
 * how many grid cells it supports:
 *   MOTION_AVAILABLE  1 when the IMP_IVS move API is usable for this build,
 *                     0 otherwise (imp_motion.c becomes a no-op stub and
 *                     GET /control reports "available":0 so the WebUI greys
 *                     the feature out).
 *   MOTION_MAX_CELLS  the maximum number of grid cells = the compile-time
 *                     IMP_IVS_MOVE_MAX_ROI_CNT of the SDK header being built
 *                     against. This VARIES per SDK: 52 on most (T10/T20
 *                     3.12.0, T21, T23, T30, T31, T40, T41, C100), but only
 *                     4 on the old T10/T20 3.9.0 SDK - it is taken from
 *                     <imp/imp_ivs_move.h> and NEVER hardcoded here.
 *
 * Detection: __has_include on <imp/imp_ivs_move.h> against the build's
 * -I$(IMP_INC) include path, so the value always matches the exact SDK
 * version the binary links against. A host build without SDK headers (make
 * sim) simulates the common 52-cell move API so the WebUI can be exercised
 * against timpsd-sim, like isp_caps/audio_caps enable everything there.
 * Consumers: src/hal/imp_motion.c (grid build + status), src/config.c
 * (motion.cols/rows defaults + clamping), src/control.c (caps.motion +
 * motion status JSON). */
#ifndef MS_MOTION_CAPS_H
#define MS_MOTION_CAPS_H

#if defined(PLATFORM_T10)||defined(PLATFORM_T20)||defined(PLATFORM_T21)|| \
    defined(PLATFORM_T23)||defined(PLATFORM_T30)||defined(PLATFORM_T31)|| \
    defined(PLATFORM_T40)||defined(PLATFORM_T41)||defined(PLATFORM_C100)
#define MOTION_PLATFORM_KNOWN 1
#endif

#if defined(__has_include)
# if __has_include(<imp/imp_ivs_move.h>)
#  define MOTION_HAVE_IVS_HDR 1
# endif
#elif defined(HAL_INGENIC)
/* ancient compiler without __has_include: a target build always has the SDK
 * include path, so trust it (every vendored SDK set ships imp_ivs_move.h) */
# define MOTION_HAVE_IVS_HDR 1
#endif

#if defined(MOTION_HAVE_IVS_HDR)
# include <imp/imp_ivs_move.h>
# define MOTION_AVAILABLE 1
# define MOTION_MAX_CELLS IMP_IVS_MOVE_MAX_ROI_CNT
#elif !defined(MOTION_PLATFORM_KNOWN) && !defined(HAL_INGENIC)
/* host sim without SDK headers: simulate the common move API (52 cells) so
 * the WebUI motion page/overlay can be tested against timpsd-sim */
# define MOTION_AVAILABLE 1
# define MOTION_MAX_CELLS 52
#else
/* real target whose SDK has no IMP_IVS move support: whole feature off */
# define MOTION_AVAILABLE 0
# define MOTION_MAX_CELLS 0
#endif

/* clamp limit for motion.cols/rows (>=1 even when the feature is absent, so
 * the config layer never divides by zero) */
#if MOTION_MAX_CELLS > 0
# define MOTION_CELL_LIMIT MOTION_MAX_CELLS
#else
# define MOTION_CELL_LIMIT 1
#endif

#endif /* MS_MOTION_CAPS_H */
