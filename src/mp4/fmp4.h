/* fmp4.h - fragmented MP4 muxer (ISO BMFF / CMAF-ish) for live streaming.
 * Produces an init segment (ftyp+moov) followed by moof+mdat fragments. */
#ifndef MS_FMP4_H
#define MS_FMP4_H
#include <stdint.h>
#include <sys/uio.h>           /* struct iovec, for fmp4_video_fragment_iov */
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

/* How many NALs of one access unit fmp4_video_fragment indexes on the stack.
 * A typical AU is 1-5 NALs (SPS/PPS/SEI + one slice); 32 covers heavily
 * multi-sliced encodes too, and an AU beyond that just takes the slower
 * re-scan path. 256 B of a 128 KB (MS_STACK_STREAM) thread stack. */
#define FMP4_NAL_IDX 32
/* head + (4-byte length prefix, NAL body) per indexed NAL = 65. Far below
 * IOV_MAX (1024 on Linux/uClibc-ng), so one sendmsg() always covers a whole
 * fragment and net_sendmsg_all() never has to split a frame. */
#define FMP4_IOV_MAX (1 + 2*FMP4_NAL_IDX)

/* Scatter/gather description of ONE video fragment, filled by
 * fmp4_video_fragment_iov(). Caller-owned, meant to live on the streaming
 * thread's stack and be refilled per frame (~650 B on MIPS32). */
typedef struct {
    struct iovec iov[FMP4_IOV_MAX];
    int          niov;                /* 0 = nothing to send for this AU */
    uint8_t      lp[FMP4_NAL_IDX][4]; /* AVCC length prefixes the iovecs point at */
} fmp4_frag_iov;

/* fmp4_video_fragment() without the full-AU copy: writes only moof + the
 * 8-byte mdat header into `head` (~120 B) and points the remaining iovecs
 * straight at the caller's access unit. The produced BYTE STREAM is identical
 * to fmp4_video_fragment()'s - this only changes how it is handed over.
 *
 * LIFETIME CONTRACT (same one rtp.h states for the RTSP sinks): the iovecs
 * alias `au` and `head`, so BOTH must stay alive and unmodified until the
 * gather-write has completed. In stream_mp4() that is the window between the
 * mux call and pkt_unref().
 *
 * The rare AU with more than FMP4_NAL_IDX NALs is muxed contiguously into
 * `head` by the normal path and handed back as a single iovec, so the caller
 * never needs a second code path. Returns 0 (fi->niov may be 0 for a
 * parameter-set-only AU) or -1 on a truncated/corrupt fragment. */
int  fmp4_video_fragment_iov(fmp4_mux *m, const uint8_t *au, size_t len,
                             int keyframe, int64_t pts_us,
                             ms_buf *head, fmp4_frag_iov *fi);

#endif
