/* fmp4.h - fragmented MP4 muxer (ISO BMFF / CMAF-ish) for live streaming.
 * Produces an init segment (ftyp+moov) followed by moof+mdat fragments. */
#ifndef MS_FMP4_H
#define MS_FMP4_H
#include <stdint.h>
#include "../util.h"
#include "../codec/vparam.h"

typedef struct {
    int       has_video, has_audio;
    int       vcodec;              /* MS_VC_H264 / MS_VC_H265 */
    int       width, height, fps;
    uint32_t  v_timescale;         /* 90000 */
    vparam    vp;                  /* SPS/PPS/VPS for avcC/hvcC */

    uint32_t  a_timescale;         /* audio samplerate */
    int       a_channels;
    uint8_t   asc[2];

    uint32_t  seq;                 /* fragment sequence */
    uint64_t  v_dts, a_dts;        /* baseMediaDecodeTime accumulators */
    int       vp_ready;

    /* A/V sync: both tracks are anchored to one shared capture-time zero
     * point; tfdt is derived from the real capture PTS so audio and video
     * cannot drift apart. -1 = not yet set. */
    int64_t   base_pts_us;         /* shared zero point (first sample of either track) */
    int64_t   v_last_pts_us;       /* last valid video PTS, -1 = none */
    int64_t   a_last_pts_us;       /* last valid audio PTS, -1 = none */
} fmp4_mux;

void fmp4_init(fmp4_mux *m);
/* build ftyp+moov init segment; requires vp_ready if has_video. */
int  fmp4_init_segment(fmp4_mux *m, ms_buf *out);
/* append one video access unit as a moof+mdat fragment.
 * pts_us: capture timestamp (monotonic us); <=0 = unknown -> nominal timing */
int  fmp4_video_fragment(fmp4_mux *m, const uint8_t *au, size_t len,
                         int keyframe, int64_t pts_us, ms_buf *out);
/* append one audio frame (raw AAC, ADTS stripped internally) */
int  fmp4_audio_fragment(fmp4_mux *m, const uint8_t *frame, size_t len,
                         int64_t pts_us, ms_buf *out);

#endif
