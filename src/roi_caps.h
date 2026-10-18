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
 * Family A, IMP_Encoder_SetChnROI (8 slots, p0/p1 corners):
 * T10 OpenIMP: QP51 -25 % bitrate, delta -10 x3.7.
 * T20 OpenIMP: QP51 block visible, QP15 clean.
 * T21 OpenIMP: effective by default; OPENIMP_T21_ROI=0 in libimp turns it off;
 * the T21 encoder saturates deltas at about -12..+13.
 * T10/T20/T21 vendor: programs the registers, never seen on a device.
 * T23 OpenIMP: effective once the libimp exports OpenIMP_Cap_T23HelixRoi
 * (OpenIMP c271c07, v2026.10.11-rc1); older ones drop the regions silently,
 * roi_rt_tier() then reports unsupported. OPENIMP_T23_ROI=0 turns it off.
 * T23 vendor: no effect, the encoder only stores the region.
 * Family B, IMP_Encoder_SetChnRoiAttr (10 windows, x/y/w/h):
 * T31 OpenIMP: effective (AVPU QP table, device-tested by the OpenIMP
 * maintainer); OPENIMP_T31_ROI=0 in libimp turns it off.
 * T31 vendor: the 1.1.x libimp has no ROI symbol.
 * T40/T41 OpenIMP: device-tested by the OpenIMP maintainer once the libimp
 * exports OpenIMP_Cap_EncRoi (bit 0); T41 only under CBR/VBR (bit 1), FixQP
 * shows no effect. Deltas only: the SDK declares absolute QP unsupported.
 * OPENIMP_T40_ROI=0 / OPENIMP_T41_ROI=0 in libimp turn it off.
 * T40/T41 vendor: deltas -26..25 only, never device-tested.
 * T30/C100 not investigated. */
#if (defined(PLATFORM_T31)||defined(PLATFORM_T40)||defined(PLATFORM_T41)) && defined(USE_OPENIMP)
#define ROI_FAMILY_B 1
#endif
#if (defined(PLATFORM_T40)||defined(PLATFORM_T41)) && defined(USE_OPENIMP)
#define ROI_B_DELTA_ONLY 1
#endif
#if !defined(ROI_PLATFORM_KNOWN)
#define ROI_TIER CAP_TIER_SIM
#elif defined(USE_OPENIMP) && \
      (defined(PLATFORM_T10)||defined(PLATFORM_T20)||defined(PLATFORM_T21)|| \
       defined(PLATFORM_T23)||defined(PLATFORM_T31))
#define ROI_TIER CAP_TIER_EFFECTIVE
#elif defined(PLATFORM_T10)||defined(PLATFORM_T20)||defined(PLATFORM_T21)|| \
      (defined(USE_OPENIMP) && (defined(PLATFORM_T40)||defined(PLATFORM_T41)))
#define ROI_TIER CAP_TIER_EXPERIMENTAL
#else
#define ROI_TIER CAP_TIER_UNSUPPORTED
#endif

/* caps.roi: the delta range sent, and the range the encoder honours */
#ifdef ROI_FAMILY_B
#define ROI_API_NAME   "roiattr"
#define ROI_SLOTS      ROI_B_WINS
#define ROI_DMIN       ROI_B_DELTA_MIN
#define ROI_DMAX       ROI_B_DELTA_MAX
#else
#define ROI_API_NAME   "chnroi"
#define ROI_SLOTS      ROI_HW_SLOTS
#define ROI_DMIN       ROI_QP_DELTA_MIN
#define ROI_DMAX       ROI_QP_DELTA_MAX
#endif
#if (defined(PLATFORM_T21) || defined(PLATFORM_T23)) && defined(USE_OPENIMP)
#define ROI_DEFF_MIN   (-12)
#define ROI_DEFF_MAX   13
#else
#define ROI_DEFF_MIN   ROI_DMIN
#define ROI_DEFF_MAX   ROI_DMAX
#endif

/* SoCs whose ROI tier depends on a libimp capability marker */
#define ROI_GATE_NONE 0
#define ROI_GATE_T23  1
#define ROI_GATE_CAP  2   /* T40/T41: OpenIMP_Cap_EncRoi bit 0 */
#if defined(USE_OPENIMP) && defined(PLATFORM_T23)
#define ROI_GATE ROI_GATE_T23
#elif defined(USE_OPENIMP) && (defined(PLATFORM_T40)||defined(PLATFORM_T41))
#define ROI_GATE ROI_GATE_CAP
#else
#define ROI_GATE ROI_GATE_NONE
#endif

static inline int roi_rt_tier(int tier, int gate, int cap)
{
    return gate != ROI_GATE_NONE && !cap ? CAP_TIER_UNSUPPORTED : tier;
}

/* FrontCrop, ISP-wide, sensor pixels.
 * T10/T20/T21 OpenIMP: device-tested (T20 shows it on downscaled channels only).
 * T10/T20/T21 vendor: no such symbol.
 * T10 OpenIMP: windows below 640x360 fail (-1, errno 122), outside the
 * sensor -1/EINVAL. T10/T20 show the window on downscaled (sub) streams only.
 * T23/T31 OpenIMP: the table is the ceiling, fcrop_rt_tier() lowers it when
 * the driver lacks fcrop_upscale_pct (open-tx-isp T23 164225e5/25b1d8c7, T31
 * 27dc8be4, device-tested with b15ef235). Without it T23 hung on a live crop
 * change: experimental and start-only (needs 3cf9bdac+), T31 experimental.
 * Every stream must fit the window or be scaled down (below: -EINVAL).
 * T23 vendor: untested. T31 vendor: experimental.
 * T40/T41: no FrontCrop in the SDK, and a live fs1 crop stops frames on T41.
 * T41 OpenIMP: IMP_ISP_Tuning_SetAutoZoom instead (per FS channel, sensor
 * pixels), once the libimp exports OpenIMP_Cap_AutoZoom. Live on outputs up
 * to FCROP_AZ_LIVE_W x FCROP_AZ_LIVE_H; the driver defers a larger one to the
 * channel's next enable, so its encode thread restarts the framesource with
 * the encoder stopped (device-tested on T41 ch0, also under fs_keepalive).
 * The start window is set before the first enable. No upscaling.
 * T40 OpenIMP: the driver refuses AutoZoom (-EOPNOTSUPP).
 * T30/C100 not investigated. */
#if defined(PLATFORM_T41) && defined(USE_OPENIMP)
#define FCROP_AUTOZOOM 1
#define FCROP_AZ_LIVE_W 768
#define FCROP_AZ_LIVE_H 432
#endif
#if !defined(ROI_PLATFORM_KNOWN)
#define FCROP_TIER CAP_TIER_SIM
#elif defined(USE_OPENIMP) && \
      (defined(PLATFORM_T10)||defined(PLATFORM_T20)||defined(PLATFORM_T21)|| \
       defined(PLATFORM_T23)||defined(PLATFORM_T31))
#define FCROP_TIER CAP_TIER_EFFECTIVE
#elif defined(PLATFORM_T31) || defined(FCROP_AUTOZOOM)
#define FCROP_TIER CAP_TIER_EXPERIMENTAL
#else
#define FCROP_TIER CAP_TIER_UNSUPPORTED
#endif

/* how the driver decides which stream shows the window (caps.fcrop) */
#if defined(PLATFORM_T31)
#define FCROP_MSCA_PARAM "/sys/module/tx_isp_t31/parameters/fcrop_upscale_pct"
#elif defined(PLATFORM_T23)
#define FCROP_MSCA_PARAM "/sys/module/tx_isp_t23/parameters/fcrop_upscale_pct"
#elif defined(PLATFORM_T10)||defined(PLATFORM_T20)||defined(PLATFORM_T21)
#define FCROP_DS_ONLY 1
#endif
/* image.fcrop_upscale_pct ceiling, device-tested; T31 at 150 hard-hangs the MSCA */
#if defined(USE_OPENIMP) && (defined(PLATFORM_T23) || defined(PLATFORM_T31))
#define FCROP_UPSCALE_MAX 100
#endif
/* the ISP crops in unflipped sensor coordinates on these axes and flips
 * afterwards, so image.fcrop_* (picture coordinates) are mirrored there:
 * bit0 x for image.hflip, bit1 y for image.vflip. Device-proven with
 * hflip+vflip (T31 on the Garage, sc4336p); the vendor stacks untested or
 * not needed. */
#if defined(USE_OPENIMP) && (defined(PLATFORM_T21) || defined(PLATFORM_T23) || defined(PLATFORM_T31))
#define FCROP_FLIP_MIRROR 3
#elif defined(USE_OPENIMP) && (defined(PLATFORM_T10) || defined(PLATFORM_T20))
#define FCROP_FLIP_MIRROR 2
#else
#define FCROP_FLIP_MIRROR 0
#endif
#if defined(PLATFORM_T10)
#define FCROP_SOC_MIN_W 640
#define FCROP_SOC_MIN_H 360
#endif

/* log text for a configured but refused feature */
#if defined(PLATFORM_T23) && defined(USE_OPENIMP)
#define ROI_WHY "T23 ROI needs OpenIMP v2026.10.11-rc1+ (OpenIMP_Cap_T23HelixRoi), OPENIMP_T23_ROI unset"
#elif defined(PLATFORM_T23)
#define ROI_WHY "no effect on T23: the vendor encoder only stores the region"
#elif defined(PLATFORM_T31)
#define ROI_WHY "T31 ROI needs OpenIMP (SetChnRoiAttr); the vendor libimp has no ROI"
#elif defined(PLATFORM_T40)||defined(PLATFORM_T41)
#define ROI_WHY "T40/T41 ROI needs OpenIMP with OpenIMP_Cap_EncRoi (SetChnRoiAttr)"
#else
#define ROI_WHY "not investigated on this SoC"
#endif
#if defined(PLATFORM_T23)
#define FCROP_WHY "T23 FrontCrop needs OpenIMP (vendor build untested; older drivers hung on a crop change)"
#elif defined(PLATFORM_T41)
#define FCROP_WHY "T41 crop needs OpenIMP AutoZoom (OpenIMP_Cap_AutoZoom)"
#elif defined(PLATFORM_T40)
#define FCROP_WHY "the T40 SDK has no FrontCrop and the T40 driver no AutoZoom"
#elif defined(PLATFORM_T10)||defined(PLATFORM_T20)||defined(PLATFORM_T21)
#define FCROP_WHY "FrontCrop is an OpenIMP extension on this SoC"
#else
#define FCROP_WHY "not investigated on this SoC"
#endif

/* compile-time ceiling of caps.fcrop.live */
#if FCROP_TIER != CAP_TIER_UNSUPPORTED
#define FCROP_LIVE 1
#else
#define FCROP_LIVE 0
#endif

/* SoCs whose FrontCrop tier depends on the driver's fcrop_upscale_pct */
#define FCROP_GATE_NONE 0
#define FCROP_GATE_T23  1
#define FCROP_GATE_T31  2
#define FCROP_GATE_AZ   3   /* T41: OpenIMP_Cap_AutoZoom */
#if defined(FCROP_AUTOZOOM)
#define FCROP_GATE FCROP_GATE_AZ
#elif defined(USE_OPENIMP) && defined(PLATFORM_T23)
#define FCROP_GATE FCROP_GATE_T23
#elif defined(USE_OPENIMP) && defined(PLATFORM_T31)
#define FCROP_GATE FCROP_GATE_T31
#else
#define FCROP_GATE FCROP_GATE_NONE
#endif

/* T23 start-only without the parameter: a live change on a running pipeline
 * is what hung older drivers, and the start apply runs before chn0 streams.
 * AZ: param is the libimp capability marker, without it nothing works. */
static inline int fcrop_rt_tier(int tier, int gate, int param, int *live)
{
    *live = tier != CAP_TIER_UNSUPPORTED;
    if (gate == FCROP_GATE_AZ && !param) { *live = 0; return CAP_TIER_UNSUPPORTED; }
    if (gate == FCROP_GATE_NONE || param || !*live) return tier;
    if (gate == FCROP_GATE_T23) *live = 0;
    return CAP_TIER_EXPERIMENTAL;
}

#endif
