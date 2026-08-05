/* rtp.h - RTP packetization for H264/H265/AAC/G711 (no library) */
#ifndef MS_RTP_H
#define MS_RTP_H
#include <stdint.h>
#include <stddef.h>

/* output sink: send one RTP (or RTCP) packet. return <0 to signal the track
 * is dead (client gone / socket error). */
typedef int (*rtp_out_fn)(void *ctx, const uint8_t *pkt, int len, int rtcp);

/* hard upper bound for one RTP packet (header + payload): 1500 Ethernet MTU
 * minus 20 B IP and 8 B UDP. The runtime rtsp.mtu config value is clamped to
 * this; all packet buffers are sized off it (B1). */
#define RTP_MTU_MAX 1472
/* below this, FU fragmentation overhead explodes and nothing legitimate
 * needs it (576 = IPv4 minimum reassembly size minus headers, rounded) */
#define RTP_MTU_MIN 548

/* longest RTCP compound packet this module emits: SR (28) + SDES with a
 * CNAME of up to RTP_CNAME_MAX chars (4 hdr + 4 ssrc + 2 item hdr + text +
 * 1 END + 3 pad) + BYE (8) */
#define RTP_CNAME_MAX 39

typedef struct {
    int        payload_type;
    uint32_t   ssrc;
    uint16_t   seq;
    uint32_t   clock_rate;
    int        mtu;            /* max RTP packet size (hdr+payload) actually
                                * used when packetizing; from rtsp.mtu (B1),
                                * clamped to [RTP_MTU_MIN, RTP_MTU_MAX] */
    char       cname[RTP_CNAME_MAX+1]; /* RTCP SDES CNAME (RFC 3550 6.5.1);
                                * both tracks of a session share one value so
                                * receivers can correlate the A/V pair */
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

/* mtu: max RTP packet size (rtsp.mtu, clamped internally); cname: SDES CNAME
 * for this session's RTCP (NULL/empty falls back to "timps"). */
void rtp_track_init(rtp_track *t, int pt, uint32_t clock_rate, int mtu,
                    const char *cname, rtp_out_fn out, void *ctx);

/* pts_us: presentation time in microseconds (shared A/V timeline).
 * Return 0 on success, <0 if the sink reported a send failure (client gone /
 * timed-out partial write). On <0 over TCP-interleaved transport the framing
 * may be torn mid-packet, so the caller MUST stop sending on that connection
 * (H-1): any further '$'-framed byte would permanently desync the stream. */
int rtp_send_h264(rtp_track *t, const uint8_t *au, size_t len, int64_t pts_us);
int rtp_send_h265(rtp_track *t, const uint8_t *au, size_t len, int64_t pts_us);
int rtp_send_aac (rtp_track *t, const uint8_t *frame, size_t len, int64_t pts_us);
int rtp_send_g711(rtp_track *t, const uint8_t *frame, size_t len, int64_t pts_us);
#ifdef USE_STREAM_OPUS
/* Opus (RFC 7587): the encoded Opus packet is carried verbatim as the RTP
 * payload (no payload header). The RTP timestamp clock is ALWAYS 48000 Hz
 * regardless of the encoder's internal sample rate, so the track's clock_rate
 * MUST be 48000. See rtp_send_opus() for the fixed-clock timestamp math. */
int rtp_send_opus(rtp_track *t, const uint8_t *frame, size_t len, int64_t pts_us);
#endif

/* emit an RTCP Sender Report if >= ~1s since the last one. Sent as a
 * compound packet SR+SDES(CNAME) per RFC 3550 6.1 (B2).
 * Returns 0 on success or when no SR was due, <0 on send failure (same
 * stop-sending contract as rtp_send_*). */
int rtp_maybe_sr(rtp_track *t, int64_t now_us);

/* RTCP BYE for a clean TEARDOWN (RFC 3550 6.3.7), sent as a compound
 * SR|RR + SDES + BYE. Best-effort: the session is closing either way, so
 * the return value only reports the sink's verdict. */
int rtp_send_bye(rtp_track *t, int64_t now_us);

#endif
