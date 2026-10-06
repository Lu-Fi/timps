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

/* ROI, H.264 only.
 * T20 OpenIMP: QP51 block visible, QP15 clean (bitrate effect not measured).
 * T20 vendor, T10: programs the registers, never seen on a device.
 * T21: returns 0 without effect (vendor 1.0.33 never programs it).
 * T23: handed to the vendor i264e, untested.
 * T31 has no ROI call; T40/T41 use SetChnRoiAttr (10 windows, delta only),
 * a different API; T30/C100 not investigated. */
#if !defined(ROI_PLATFORM_KNOWN)
#define ROI_TIER CAP_TIER_SIM
#elif defined(PLATFORM_T20) && defined(USE_OPENIMP)
#define ROI_TIER CAP_TIER_EFFECTIVE
#elif defined(PLATFORM_T10)||defined(PLATFORM_T20)||defined(PLATFORM_T21)|| \
      defined(PLATFORM_T23)
#define ROI_TIER CAP_TIER_EXPERIMENTAL
#else
#define ROI_TIER CAP_TIER_UNSUPPORTED
#endif

/* FrontCrop, ISP-wide, sensor pixels.
 * T20/T21 OpenIMP: device-tested (T20 shows it on downscaled channels only).
 * T10 OpenIMP: untested. T10/T20/T21 vendor: no such symbol.
 * T31: Get/Set round-trip only, picture effect not measured.
 * T23: a crop change stops the pipeline until reboot (open OpenIMP blocker),
 * so no crop at all, not even at start. T40/T41: not in the SDK, and a live
 * fs1 crop stops frames on T41. T30/C100 not investigated. */
#if !defined(ROI_PLATFORM_KNOWN)
#define FCROP_TIER CAP_TIER_SIM
#elif defined(USE_OPENIMP) && (defined(PLATFORM_T20)||defined(PLATFORM_T21))
#define FCROP_TIER CAP_TIER_EFFECTIVE
#elif (defined(USE_OPENIMP) && defined(PLATFORM_T10)) || defined(PLATFORM_T31)
#define FCROP_TIER CAP_TIER_EXPERIMENTAL
#else
#define FCROP_TIER CAP_TIER_UNSUPPORTED
#endif

/* log text for a configured but refused feature */
#if defined(PLATFORM_T31)
#define ROI_WHY "the T31 libimp has no ROI call"
#elif defined(PLATFORM_T40)||defined(PLATFORM_T41)
#define ROI_WHY "T40/T41 use the SetChnRoiAttr API, not wired"
#else
#define ROI_WHY "not investigated on this SoC"
#endif
#if defined(PLATFORM_T23)
#define FCROP_WHY "a crop change stops the T23 pipeline until reboot (open blocker)"
#elif defined(PLATFORM_T40)||defined(PLATFORM_T41)
#define FCROP_WHY "the T40/T41 SDK has no FrontCrop"
#elif defined(PLATFORM_T10)||defined(PLATFORM_T20)||defined(PLATFORM_T21)
#define FCROP_WHY "FrontCrop is an OpenIMP extension on this SoC"
#else
#define FCROP_WHY "not investigated on this SoC"
#endif

/* 1 = /control applies it to the running ISP; 0 = start only (restart). */
#if FCROP_TIER != CAP_TIER_UNSUPPORTED
#define FCROP_LIVE 1
#else
#define FCROP_LIVE 0
#endif

#endif
