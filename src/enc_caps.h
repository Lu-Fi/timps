/* enc_caps.h - per-SoC LIVE encoder rate-control capability.
 *
 * One place decides which videoN.* keys the running build can apply to a
 * LIVE encoder channel (no daemon restart), the isp_caps.h pattern:
 *   - src/hal/hal_ingenic.c gates the actual runtime IMP calls on it
 *     (rc_live_apply() in ing_control's video branch),
 *   - src/control.c derives the "caps":{"video_live":[...]} list GET
 *     /control reports, so the WebUI can label live vs restart per key.
 * Keep both consumers in sync with this header.
 *
 * Derived from the vendored SDK headers (verified 2026-08-21):
 *   classic (T10..T30, incl. T23): IMP_Encoder_SetChnAttrRcMode takes the
 *     whole rc union, so every rc key can be re-applied in one call.
 *     H264-only per the header - an H265 stream on T21/T30 stays
 *     restart-bound (runtime-graded, not expressible here).
 *   T31/C100: SetChnBitRate + SetChnQpBounds + SetChnQpIPDelta +
 *     SetChnAttrRcMode (the last used only for the fixqp initial QP).
 *   T40: as T31/C100 but no SetChnQpIPDelta (i_bias_lvl unsupported).
 *   T41: only SetChnBitRate and SetChnQpBounds - no rc-mode setter at all.
 * quality_lvl/change_pos/fluc_lvl have no new-API equivalent anywhere.
 *
 * The list is a static PLATFORM capability: a listed key can still fall
 * back to restart (channel not running, classic H265, IMP call rejected) -
 * the per-request truth is the "deferred" grading in the POST reply.
 * No ENC_LIVE_KEYS at all (host sim): nothing applies live. */
#ifndef MS_ENC_CAPS_H
#define MS_ENC_CAPS_H

#ifdef HAL_INGENIC
#if defined(PLATFORM_T31)||defined(PLATFORM_C100)||defined(PLATFORM_T40)|| \
    defined(PLATFORM_T41)
#define ENC_RC_API_NEW 1
#if defined(PLATFORM_T41)
#define ENC_LIVE_KEYS "bitrate", "min_qp", "max_qp"
#elif defined(PLATFORM_T40)
#define ENC_LIVE_KEYS "bitrate", "min_qp", "max_qp", "qp"
#else /* T31/C100 */
#define ENC_LIVE_KEYS "bitrate", "min_qp", "max_qp", "qp", "i_bias_lvl"
#endif
#else /* classic API: full union re-fill, H264 channels */
#define ENC_LIVE_KEYS "rc_mode", "bitrate", "qp", "min_qp", "max_qp", \
                      "quality_lvl", "change_pos", "i_bias_lvl"
#endif
#endif /* HAL_INGENIC */

#endif
