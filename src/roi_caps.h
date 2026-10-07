#ifndef MS_ROI_CAPS_H
#define MS_ROI_CAPS_H
/* Per-SoC tiers for encoder ROI (IMP_Encoder_SetChnROI, the T10..T23 "family
 * A" API) and ISP FrontCrop (IMP_ISP_Tuning_SetFrontCrop).
 *
 *   EFFECTIVE     device-tested: the call takes and the picture shows it
 *   EXPERIMENTAL  the call exists and returns 0; effect unverified or absent
 *   UNSUPPORTED   no symbol, a different API, or a known pipeline hazard
 *   SIM           host sim: everything accepted, nothing applied
 *
 * Both setters are bound weak in hal_ingenic.c, so a libimp without the
 * symbol downgrades the running tier to UNSUPPORTED (hal_roi_tier()).
 * Raising a SoC after its device test is a one-line change here; the matrix
 * is pinned by scripts/test_roi.c (make test-roi). */

#define CAP_TIER_UNSUPPORTED  0
#define CAP_TIER_EXPERIMENTAL 1
#define CAP_TIER_EFFECTIVE    2
#define CAP_TIER_SIM          3

static inline const char *cap_tier_name(int t)
{
    return t == CAP_TIER_EFFECTIVE ? "effective" :
           t == CAP_TIER_EXPERIMENTAL ? "experimental" :
           t == CAP_TIER_SIM ? "sim" : "unsupported";
}

#if defined(PLATFORM_T10)||defined(PLATFORM_T20)||defined(PLATFORM_T21)|| \
    defined(PLATFORM_T23)||defined(PLATFORM_T30)||defined(PLATFORM_T31)|| \
    defined(PLATFORM_T40)||defined(PLATFORM_T41)||defined(PLATFORM_C100)
#define ROI_PLATFORM_KNOWN 1
#endif

/* ROI, H.264 only. Device tests by the OpenIMP maintainer, not by timps.
 * T10 OpenIMP: QP51 -25 % bitrate, delta -10 x3.7.
 * T20 OpenIMP: QP51 block visible, QP15 clean.
 * T21 OpenIMP: effective by default; OPENIMP_T21_ROI=0 in libimp turns it off.
 * T10/T20/T21 vendor: programs the registers, never seen on a device.
 * T23: no effect, the vendor encoder only stores the region.
 * T31 OpenIMP and T40/T41 use SetChnRoiAttr (10 windows), not wired;
 * T30/C100 not investigated. */
#if !defined(ROI_PLATFORM_KNOWN)
#define ROI_TIER CAP_TIER_SIM
#elif defined(USE_OPENIMP) && \
      (defined(PLATFORM_T10)||defined(PLATFORM_T20)||defined(PLATFORM_T21))
#define ROI_TIER CAP_TIER_EFFECTIVE
#elif defined(PLATFORM_T10)||defined(PLATFORM_T20)||defined(PLATFORM_T21)
#define ROI_TIER CAP_TIER_EXPERIMENTAL
#else
#define ROI_TIER CAP_TIER_UNSUPPORTED
#endif

/* FrontCrop, ISP-wide, sensor pixels.
 * T10/T20/T21 OpenIMP: device-tested (T20 shows it on downscaled channels only).
 * T10/T20/T21 vendor: no such symbol.
 * T23 OpenIMP: needs open-tx-isp 3cf9bdac or newer (older drivers hung on a
 * crop change; windows below the ch0 size now fail with -EINVAL). The tier is
 * compile-time and cannot see the driver, so start-only. T23 vendor: untested.
 * T31: Get/Set round-trip only, effect not measured.
 * T40/T41: not in the SDK, and a live fs1 crop stops frames on T41.
 * T30/C100 not investigated. */
#if !defined(ROI_PLATFORM_KNOWN)
#define FCROP_TIER CAP_TIER_SIM
#elif defined(USE_OPENIMP) && \
      (defined(PLATFORM_T10)||defined(PLATFORM_T20)||defined(PLATFORM_T21))
#define FCROP_TIER CAP_TIER_EFFECTIVE
#elif (defined(USE_OPENIMP) && defined(PLATFORM_T23)) || defined(PLATFORM_T31)
#define FCROP_TIER CAP_TIER_EXPERIMENTAL
#else
#define FCROP_TIER CAP_TIER_UNSUPPORTED
#endif

/* log text for a configured but refused feature */
#if defined(PLATFORM_T23)
#define ROI_WHY "no effect on T23: the vendor encoder only stores the region"
#elif defined(PLATFORM_T31)
#define ROI_WHY "T31 ROI goes through SetChnRoiAttr (OpenIMP only), not wired yet"
#elif defined(PLATFORM_T40)||defined(PLATFORM_T41)
#define ROI_WHY "T40/T41 use the SetChnRoiAttr API, not wired"
#else
#define ROI_WHY "not investigated on this SoC"
#endif
#if defined(PLATFORM_T23)
#define FCROP_WHY "T23 FrontCrop needs OpenIMP (vendor build untested; older drivers hung on a crop change)"
#elif defined(PLATFORM_T40)||defined(PLATFORM_T41)
#define FCROP_WHY "the T40/T41 SDK has no FrontCrop"
#elif defined(PLATFORM_T10)||defined(PLATFORM_T20)||defined(PLATFORM_T21)
#define FCROP_WHY "FrontCrop is an OpenIMP extension on this SoC"
#else
#define FCROP_WHY "not investigated on this SoC"
#endif

/* caps.fcrop.live; T23 start-only: a live change on a running pipeline is
 * what hung older drivers, and the start apply runs before chn0 streams */
#if FCROP_TIER != CAP_TIER_UNSUPPORTED && !defined(PLATFORM_T23)
#define FCROP_LIVE 1
#else
#define FCROP_LIVE 0
#endif

#endif
