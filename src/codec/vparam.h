/* vparam.h - H264/H265 parameter set extraction + MP4/SDP config building */
#ifndef MS_VPARAM_H
#define MS_VPARAM_H
#include <stdint.h>
#include "../util.h"

typedef struct {
    int      codec;        /* MS_VC_H264 / MS_VC_H265 */
    uint8_t  vps[160]; int vps_len;   /* h265 only */
    uint8_t  sps[160]; int sps_len;
    uint8_t  pps[80];  int pps_len;
} vparam;

void vparam_init(vparam *v, int codec);
/* scan an Annex-B access unit and cache parameter sets found in it.
 * returns 1 if the set is now complete (sps+pps [+vps]). */
int  vparam_update(vparam *v, const uint8_t *au, size_t len);
int  vparam_ready(const vparam *v);

/* Build AVCDecoderConfigurationRecord (H264) or HEVCDecoderConfigurationRecord
 * (H265) into out (appended). */
int  vparam_mp4_config(const vparam *v, ms_buf *out);

/* Build the SDP a=fmtp payload string (without "a=fmtp:<pt> " prefix). */
int  vparam_sdp_fmtp(const vparam *v, int payload_type, char *dst, int dstsz);

/* Build the ISO/IEC 14496-15 MSE/RFC6381 codecs string ("hvc1.<profile>.
 * <compat>.<tier+level>.<constraints>") from the live SPS profile_tier_level,
 * so MediaSource.isTypeSupported() sees the stream's real profile/tier/level
 * instead of a guess. Returns -1 (dst untouched) if the SPS isn't parseable
 * yet (e.g. too short) - caller should keep its own fallback string. */
int  vparam_hevc_codecs(const vparam *v, char *dst, int dstsz);

#endif
