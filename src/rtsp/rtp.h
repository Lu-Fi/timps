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

/* pts_us: presentation time in microseconds (shared A/V timeline) */
void rtp_send_h264(rtp_track *t, const uint8_t *au, size_t len, int64_t pts_us);
void rtp_send_h265(rtp_track *t, const uint8_t *au, size_t len, int64_t pts_us);
void rtp_send_aac (rtp_track *t, const uint8_t *frame, size_t len, int64_t pts_us);
void rtp_send_g711(rtp_track *t, const uint8_t *frame, size_t len, int64_t pts_us);

/* emit an RTCP Sender Report if >= ~1s since the last one */
void rtp_maybe_sr(rtp_track *t, int64_t now_us);

#endif
