/* rtp.h - RTP packetization for H264/H265/AAC/G711 (no library) */
#ifndef MS_RTP_H
#define MS_RTP_H
#include <stdint.h>
#include <stddef.h>

/* output sink: send one RTP (or RTCP) packet. return <0 to signal the track
 * is dead (client gone / socket error). */
typedef int (*rtp_out_fn)(void *ctx, const uint8_t *pkt, int len, int rtcp);

typedef struct {
    int        payload_type;
    uint32_t   ssrc;
    uint16_t   seq;
    uint32_t   clock_rate;
    uint32_t   ts_base;        /* random start offset */
    int64_t    pts0;           /* first pts_us on this track (relative base) */
    int        have_pts0;
    uint64_t   audio_samples;  /* cumulative samples sent on an audio track; the
                                * RTP timestamp is ts_base + this, so it's exact
                                * and immune to publish-time wall-clock jitter */
    int64_t    last_pts;       /* last publish pts_us seen on this audio track
                                * (0 = none yet); used to detect real gaps
                                * (mute/stall/drop) and jump audio_samples
                                * forward so the media timeline stays aligned
                                * with the wall-clock RTCP SR mapping (M-1) */
    /* RTCP SR bookkeeping */
    uint32_t   pkt_count;
    uint32_t   octet_count;
    int64_t    last_sr_us;
    uint32_t   last_rtp_ts;
    rtp_out_fn out;
    void      *ctx;
} rtp_track;

void rtp_track_init(rtp_track *t, int pt, uint32_t clock_rate,
                    rtp_out_fn out, void *ctx);

/* pts_us: presentation time in microseconds (shared A/V timeline).
 * Return 0 on success, <0 if the sink reported a send failure (client gone /
 * timed-out partial write). On <0 over TCP-interleaved transport the framing
 * may be torn mid-packet, so the caller MUST stop sending on that connection
 * (H-1): any further '$'-framed byte would permanently desync the stream. */
int rtp_send_h264(rtp_track *t, const uint8_t *au, size_t len, int64_t pts_us);
int rtp_send_h265(rtp_track *t, const uint8_t *au, size_t len, int64_t pts_us);
int rtp_send_aac (rtp_track *t, const uint8_t *frame, size_t len, int64_t pts_us);
int rtp_send_g711(rtp_track *t, const uint8_t *frame, size_t len, int64_t pts_us);

/* emit an RTCP Sender Report if >= ~1s since the last one.
 * Returns 0 on success or when no SR was due, <0 on send failure (same
 * stop-sending contract as rtp_send_*). */
int rtp_maybe_sr(rtp_track *t, int64_t now_us);

#endif
