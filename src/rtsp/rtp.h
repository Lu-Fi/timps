/* rtp.h - RTP packetization for H264/H265/AAC/G711 (no library) */
#ifndef MS_RTP_H
#define MS_RTP_H
#include <stdint.h>
#include <stddef.h>

/* Output sink: send one RTP (or RTCP) packet. Return <0 to signal the track is
 * dead (client gone / socket error).
 *
 * The packet is handed over in TWO pieces, which is what lets the sink give the
 * kernel a scatter/gather write instead of memcpy'ing the media payload into a
 * contiguous buffer first (the payload is by far the bigger half):
 *   hdr[0..hlen)  protocol headers only - the 12-byte RTP header plus any
 *                 payload-format header (H264 FU indicator+header, H265 FU
 *                 payload+FU header, AAC AU-headers-length + AU header). At
 *                 most RTP_HDR_MAX bytes. For an RTCP packet (rtcp!=0) this is
 *                 the WHOLE compound packet and pay/plen are NULL/0.
 *   pay[0..plen)  the media payload, pointing DIRECTLY INTO the encoded access
 *                 unit the caller was given - never a copy. May be 0-length.
 *
 * ---- LIFETIME CONTRACT (read before changing anything here) ----------------
 * The two halves have DIFFERENT lifetimes, and that asymmetry is the whole
 * point of the split signature:
 *
 *   hdr  lives in a small buffer on the PACKETIZER's stack that is REUSED for
 *        the very next packet of the same access unit. It is valid ONLY for
 *        the duration of this call. A sink that defers the actual send (the
 *        UDP sendmmsg batch in rtsp.c) MUST copy it - hence RTP_HDR_MAX.
 *
 *   pay  points into the refcounted ms_pkt (frame.h) that the consumer thread
 *        popped from its fanqueue. That reference is held by the consumer for
 *        the whole access unit and released only AFTER it has flushed every
 *        sink (rtsp.c stream_loop's flush barrier just above its pkt_unref).
 *        So a sink MAY park this pointer in a pending batch until its next
 *        flush - and MUST NOT hold it past that flush.
 *
 * Why the payload pointer is safe at all, i.e. what a future change must not
 * break (each link verified, not assumed):
 *   - fanqueue_pop() REMOVES the packet from the queue under the queue lock and
 *     transfers the queue's reference to the popping thread (fanqueue.c) - it
 *     does not unref. So the consumer owns a reference outright.
 *   - the fanqueue's drop-oldest overflow eviction only ever unrefs packets
 *     still IN slots[] (fanqueue.c fanqueue_push). A packet already popped is
 *     unreachable from there, so a producer overrun CANNOT free a buffer a
 *     sender is transmitting, however far behind that sender falls.
 *   - fanqueue_close() (shutdown, via ms_creg_wake_all in util.c) only sets a
 *     flag and broadcasts; it frees no packet. fanqueue_free() unrefs only the
 *     still-queued remainder and runs on the consumer's OWN thread after its
 *     loop returned (rtsp.c client_thread), never concurrently with a send.
 *   - the buffer returns to its source pool (or is free()d) strictly on the
 *     LAST pkt_unref, via an atomic decrement (frame.c pkt_unref), so the
 *     producer cannot recycle and overwrite it while this reference is held.
 *   - every subscriber gets its own pkt_ref() (hub.c hub_publish /
 *     hub_publish_take), so a second subscriber's progress is irrelevant.
 * Whoever moves the consumer's pkt_unref() earlier - above the sink flush, or
 * into the send loop - breaks all of this and turns it into a use-after-free
 * that shows up as sporadically corrupted video, not as a crash. */
typedef int (*rtp_out_fn)(void *ctx, const uint8_t *hdr, int hlen,
                          const uint8_t *pay, int plen, int rtcp);

/* hard upper bound for one RTP packet (header + payload): 1500 Ethernet MTU
 * minus 20 B IP and 8 B UDP. The runtime rtsp.mtu config value is clamped to
 * this; all packet buffers are sized off it (B1). */
#define RTP_MTU_MAX 1472
/* below this, FU fragmentation overhead explodes and nothing legitimate
 * needs it (576 = IPv4 minimum reassembly size minus headers, rounded) */
#define RTP_MTU_MIN 548
/* Upper bound on the `hlen` a sink can ever be handed (see rtp_out_fn): the
 * 12-byte RTP header plus the largest payload-format header any packetizer in
 * rtp.c prepends - AAC's 2-byte AU-headers-length + 2-byte AU header (RFC 3640)
 * is the biggest at 4; H265 FU is 3, H264 FU-A is 2, G711/Opus are 0. A sink
 * that stages headers (rtsp.c's sendmmsg batch) sizes its slots off this, so
 * any new packetizer that needs more must raise it here. */
#define RTP_HDR_MAX 16

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
    int64_t    sr_ref_mono_us; /* CLOCK_MONOTONIC stamp paired with
                                * last_rtp_ts - the media<->wall anchor the
                                * SR extrapolates from (see rtcp_wr_sr()).
                                * 0 = no anchor yet (SRs are gated on
                                * have_pts0, which implies a send anyway) */
    rtp_out_fn out;
    void      *ctx;
} rtp_track;

/* Record the monotonic wall time corresponding to the media packet just sent
 * on this track (the packet's hub publish stamp, p->enq_us). Pairs with
 * t->last_rtp_ts to give the RTCP SR a media-timestamp <-> wall-clock
 * correspondence on consistent clocks: mono_us is on ms_now_us()'s clock,
 * while pts_us values may live on a different epoch (pts_sanitize output,
 * sim's g_epoch-relative clock), so the SR must never mix them. Call after
 * each successful rtp_send_*(). */
static inline void rtp_sr_anchor(rtp_track *t, int64_t mono_us)
{
    t->sr_ref_mono_us = mono_us;
}

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
