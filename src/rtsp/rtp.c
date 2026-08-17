#include "rtp.h"
#include "../codec/nal.h"
#include "../codec/aac.h"
#include "../util.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

/* B1: the packetization MTU is per-track at runtime now (t->mtu, from the
 * rtsp.mtu config key); RTP_MTU_MAX (rtp.h) only sizes the stack buffers. */

/* M6: fill out with kernel randomness (same /dev/urandom pattern as
 * auth_gen_token in auth.c); <0 = unavailable, caller falls back */
static int urand_bytes(void *out, size_t n)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < n) {
        ssize_t r = read(fd, (uint8_t *)out + off, n - off);
        if (r <= 0) { close(fd); return -1; }
        off += (size_t)r;
    }
    close(fd);
    return 0;
}

static uint32_t pts_to_ts(rtp_track *t, int64_t pts_us)
{
    /* RTP timestamps must be RELATIVE to the stream start, not the absolute
     * monotonic clock. pts_us is ms_now_us() (CLOCK_MONOTONIC = uptime), so the
     * old code made the 32-bit RTP ts encode uptime*clock_rate: video (90 kHz)
     * and audio (16 kHz) then diverged and wrapped at different periods (~13 h
     * vs ~74 h). Players saw a huge A/V offset and non-monotonic/invalid
     * timestamps, which made ffmpeg/go2rtc (Frigate) drop and reconnect every
     * few minutes. Anchor each track to its first pts so the values stay small
     * and correlated; the RTCP SR still maps them to wall-clock for A/V sync. */
    if (!t->have_pts0){ t->pts0 = pts_us; t->have_pts0 = 1; }
    int64_t rel = pts_us - t->pts0;
    if (rel < 0) rel = 0;
    /* L13 (deferred): `rel * clock_rate` is an int64 product; at the 90 kHz
     * video clock it overflows INT64_MAX after roughly INT64_MAX/90000 us of
     * continuous uptime without a process restart, i.e. ~2.8-3 years, at
     * which point this timestamp math goes wrong (the RTCP SR is immune:
     * it extrapolates from last_rtp_ts plus a small elapsed term). A correct fix needs the track to periodically
     * rebase pts0/ts_base rather than a one-line clamp here, so it's left
     * for a follow-up rather than patched in this pass. */
    return t->ts_base + (uint32_t)((rel * (int64_t)t->clock_rate) / 1000000);
}

void rtp_track_init(rtp_track *t, int pt, uint32_t clock_rate, int mtu,
                    const char *cname, rtp_out_fn out, void *ctx)
{
    memset(t, 0, sizeof(*t));
    t->payload_type = pt;
    t->clock_rate   = clock_rate;
    /* B1: runtime MTU from rtsp.mtu. config.c already clamps the key, but
     * clamp again here so no caller can ever size past the stack buffers. */
    if (mtu < RTP_MTU_MIN) mtu = RTP_MTU_MIN;
    if (mtu > RTP_MTU_MAX) mtu = RTP_MTU_MAX;
    t->mtu = mtu;
    /* B2: SDES CNAME; both tracks of a session get the same string */
    snprintf(t->cname, sizeof t->cname, "%s",
             (cname && cname[0]) ? cname : "timps");
    t->out = out; t->ctx = ctx;
    /* M6: SSRC/start-seq/ts_base from /dev/urandom, not rand() - rand() is
     * seeded time^pid (main.c), making off-path RTP injection/guessing
     * feasible; RFC 3550 wants an unpredictable SSRC anyway. The old weak
     * mix stays only as a fallback for a system without /dev/urandom. */
    struct { uint32_t ssrc, ts; uint16_t seq; } rnd;
    if (urand_bytes(&rnd, sizeof rnd) == 0) {
        t->ssrc    = rnd.ssrc;
        t->seq     = rnd.seq;
        t->ts_base = rnd.ts;
    } else {
        t->ssrc = ((uint32_t)rand()<<16) ^ (uint32_t)rand() ^ (uint32_t)time(NULL);
        t->seq  = (uint16_t)rand();
        t->ts_base = (uint32_t)rand();
    }
    t->last_sr_us = 0;
}

/* write the 12-byte RTP header */
static int rtp_hdr(uint8_t *p, rtp_track *t, int marker, uint32_t ts)
{
    p[0] = 0x80;
    p[1] = (uint8_t)((marker?0x80:0) | (t->payload_type & 0x7F));
    wr_be16(p+2, t->seq++);
    wr_be32(p+4, ts);
    wr_be32(p+8, t->ssrc);
    return 12;
}

/* L3: emit's <0 (sink says client is gone) is threaded up through every
 * packetizer so the rest of the access unit is abandoned instead of burning
 * CPU fragmenting/copying packets a dead socket will only reject again. */
static int emit(rtp_track *t, uint8_t *pkt, int len, uint32_t ts)
{
    t->pkt_count++;
    t->octet_count += (uint32_t)(len - 12);
    t->last_rtp_ts = ts;
    return t->out(t->ctx, pkt, len, 0);
}

/* ---- H264 (RFC 6184) ---- */
static int send_h264_nal(rtp_track *t, const uint8_t *nal, size_t n,
                         uint32_t ts, int last_in_au)
{
    uint8_t pkt[RTP_MTU_MAX + 32];
    size_t mtu = (size_t)t->mtu;
    if (n + 12 <= mtu) {
        int h = rtp_hdr(pkt, t, last_in_au, ts);
        memcpy(pkt+h, nal, n);
        return emit(t, pkt, h+(int)n, ts) < 0 ? -1 : 0;
    }
    /* FU-A */
    uint8_t nri = nal[0] & 0x60;
    uint8_t typ = nal[0] & 0x1F;
    const uint8_t *p = nal + 1;
    size_t left = n - 1;
    int first = 1;
    while (left > 0) {
        size_t chunk = left;
        if (chunk > mtu - 12 - 2) chunk = mtu - 12 - 2; /* -12 RTP hdr, -2 FU ind+hdr */
        int end = (chunk == left);
        int h = rtp_hdr(pkt, t, (end && last_in_au), ts);
        pkt[h]   = nri | 28;                          /* FU indicator */
        pkt[h+1] = (uint8_t)((first?0x80:0)|(end?0x40:0)|typ); /* FU header */
        memcpy(pkt+h+2, p, chunk);
        if (emit(t, pkt, h+2+(int)chunk, ts) < 0) return -1;
        p += chunk; left -= chunk; first = 0;
    }
    return 0;
}

int rtp_send_h264(rtp_track *t, const uint8_t *au, size_t len, int64_t pts_us)
{
    uint32_t ts = pts_to_ts(t, pts_us);
    /* one-NAL lookahead to know which is last (for the marker bit) without
     * a fixed-size NAL list - the old list[64] cap silently dropped any
     * NAL past the 64th (unreachable with the Ingenic encoder's single-
     * slice AUs today, but a real correctness bug for any AU that isn't). */
    nal_iter it; nal_unit u, pending; int have_pending = 0;
    nal_iter_init(&it, au, len);
    while (nal_iter_next(&it, &u)) {
        if (have_pending &&
            send_h264_nal(t, pending.data, pending.len, ts, 0) < 0) return -1;
        pending = u; have_pending = 1;
    }
    if (have_pending)
        return send_h264_nal(t, pending.data, pending.len, ts, 1);
    return 0;
}

/* ---- H265 (RFC 7798) ---- */
static int send_h265_nal(rtp_track *t, const uint8_t *nal, size_t n,
                         uint32_t ts, int last_in_au)
{
    uint8_t pkt[RTP_MTU_MAX + 32];
    size_t mtu = (size_t)t->mtu;
    if (n + 12 <= mtu) {
        int h = rtp_hdr(pkt, t, last_in_au, ts);
        memcpy(pkt+h, nal, n);
        return emit(t, pkt, h+(int)n, ts) < 0 ? -1 : 0;
    }
    /* FU (type 49) - 2-byte payload hdr + 1-byte FU header */
    uint8_t typ = (nal[0] >> 1) & 0x3F;
    uint8_t lid = ((nal[0]&1)<<5) | (nal[1]>>3);   /* layer id */
    uint8_t tid = nal[1] & 0x07;
    const uint8_t *p = nal + 2;
    size_t left = n - 2;
    int first = 1;
    while (left > 0) {
        size_t chunk = left;
        if (chunk > mtu - 12 - 3) chunk = mtu - 12 - 3; /* -12 RTP hdr, -3 FU hdr */
        int end = (chunk == left);
        int h = rtp_hdr(pkt, t, (end && last_in_au), ts);
        pkt[h]   = (uint8_t)((49<<1) | (lid>>5));
        pkt[h+1] = (uint8_t)((lid<<3) | tid);
        pkt[h+2] = (uint8_t)((first?0x80:0)|(end?0x40:0)|typ);
        memcpy(pkt+h+3, p, chunk);
        if (emit(t, pkt, h+3+(int)chunk, ts) < 0) return -1;
        p += chunk; left -= chunk; first = 0;
    }
    return 0;
}

int rtp_send_h265(rtp_track *t, const uint8_t *au, size_t len, int64_t pts_us)
{
    uint32_t ts = pts_to_ts(t, pts_us);
    /* one-NAL lookahead, see rtp_send_h264() for why (no fixed-size cap) */
    nal_iter it; nal_unit u, pending; int have_pending = 0;
    nal_iter_init(&it, au, len);
    while (nal_iter_next(&it, &u)) {
        if (have_pending &&
            send_h265_nal(t, pending.data, pending.len, ts, 0) < 0) return -1;
        pending = u; have_pending = 1;
    }
    if (have_pending)
        return send_h265_nal(t, pending.data, pending.len, ts, 1);
    return 0;
}

/* M-1: the sample-count-driven audio timestamp (audio_samples) only advances
 * when a frame is actually SENT, while rtp_maybe_sr() maps "now" to RTP time
 * via the wall clock. Any gap in published audio (audio.mute via /control, an
 * AI stall/watchdog retry, fanqueue overflow drops) therefore froze the media
 * timeline while the SR mapping kept advancing: after e.g. a 10 s mute, audio
 * resumed with contiguous timestamps the next SR declared to be 10 s in the
 * PAST, and SR-honoring receivers (ffmpeg/go2rtc/Frigate) shifted audio by
 * the accumulated gap - permanently, growing with every gap.
 *
 * Fix: before stamping a frame, compare its publish pts against the previous
 * one. If the delta exceeds 2 nominal frame durations (a real discontinuity,
 * not scheduling jitter), advance audio_samples by the missed time, rounded
 * to whole frames, so the media timeline jumps forward to match wall-clock.
 * For a continuous stream the delta is ~1 frame duration, the condition never
 * fires, and the counter advances exactly as before (jitter immunity kept).
 * frame_samples: samples per frame at clock_rate (G.711: len bytes == samples;
 * AAC-LC: 1024). */
static void audio_gap_resync(rtp_track *t, int64_t pts_us, uint32_t frame_samples)
{
    if (t->last_pts != 0 && frame_samples > 0 && t->clock_rate > 0) {
        int64_t frame_us = ((int64_t)frame_samples * 1000000) /
                           (int64_t)t->clock_rate;
        int64_t delta = pts_us - t->last_pts;
        if (frame_us > 0 && delta > 2 * frame_us) {
            /* expected_pts = last_pts + frame_us; missed whole frames =
             * round((pts_us - expected_pts) / frame_us), each worth
             * frame_samples samples ( == gap * clock_rate / 1e6 rounded
             * to whole frames). The current frame's own advance still
             * happens in the caller as usual. */
            int64_t missed = (delta - frame_us + frame_us / 2) / frame_us;
            if (missed > 0)
                t->audio_samples += (uint64_t)missed * frame_samples;
        }
    }
    t->last_pts = pts_us;
}

/* ---- AAC (RFC 3640 mpeg4-generic) ---- */
int rtp_send_aac(rtp_track *t, const uint8_t *frame, size_t len, int64_t pts_us)
{
    size_t plen; int off = aac_adts_strip(frame, len, &plen);
    const uint8_t *au = frame + off;
    if (plen == 0 || plen > 0x1FFF) return 0; /* AU-size field is 13 bits;
                                               * malformed frame, client fine */
    /* Sample-count-driven timestamp (see rtp_send_g711): AAC-LC is a fixed 1024
     * samples per AU, so advance by that instead of the jittery publish
     * wall-clock. pts0 is still anchored for the RTCP SR. */
    if (!t->have_pts0){ t->pts0 = pts_us; t->have_pts0 = 1; }
    audio_gap_resync(t, pts_us, 1024);          /* M-1: jump over real gaps */
    uint32_t ts = t->ts_base + (uint32_t)t->audio_samples;
    t->audio_samples += 1024;
    uint8_t pkt[RTP_MTU_MAX + 32];
    size_t mtu = (size_t)t->mtu;

    if (plen + 12 + 4 <= mtu) {
        /* common case: whole AU in one packet. AU-headers-length = 16 bits;
         * one AU header of 16 bits: 13 bits size + 3 bits index(0) */
        int h = rtp_hdr(pkt, t, 1, ts);
        wr_be16(pkt+h, 16);
        uint16_t auh = (uint16_t)((plen & 0x1FFF) << 3);
        wr_be16(pkt+h+2, auh);
        memcpy(pkt+h+4, au, plen);
        return emit(t, pkt, h+4+(int)plen, ts) < 0 ? -1 : 0;
    }

    /* RFC 3640 3.2.3.1 + 3.2.1.1 ("AU-size"): an AU exceeding the MTU (high
     * bitrate + a transient frame) is split across multiple RTP packets, one
     * fragment per packet. Per the AU-size field definition, EVERY packet
     * carrying a fragment - not just the first - has its own AU-header, and
     * that AU-header's size field always reports the size of the COMPLETE,
     * unfragmented AU (a receiver tells "whole AU" from "fragment" by
     * comparing that size to the actual AU-data-section length). Omitting
     * the AU-header on continuation fragments (as an earlier version of this
     * function did) is not RFC 3640 conformant and breaks payload parsing
     * for any receiver that isn't lenient about it. */
    size_t sent = 0;
    uint16_t auh = (uint16_t)((plen & 0x1FFF) << 3);   /* full AU size, AU-Index=0 */
    while (sent < plen) {
        size_t chunk = mtu - 12 - 4;
        if (chunk > plen - sent) chunk = plen - sent;
        int end = (sent + chunk >= plen);
        int h = rtp_hdr(pkt, t, end, ts);
        wr_be16(pkt+h, 16);
        wr_be16(pkt+h+2, auh);
        memcpy(pkt+h+4, au+sent, chunk);
        if (emit(t, pkt, h+4+(int)chunk, ts) < 0) return -1; /* client gone (L3) */
        sent += chunk;
    }
    return 0;
}

/* ---- G.711 (raw, fragment if needed) ---- */
int rtp_send_g711(rtp_track *t, const uint8_t *frame, size_t len, int64_t pts_us)
{
    /* G.711 is sample-exact (1 byte == 1 sample @ 8 kHz) and captured in hard
     * real time, so derive the RTP timestamp from a cumulative SAMPLE counter,
     * not the publish wall-clock (pts_to_ts). The audio thread stamps frames
     * with ms_now_us() at hub-publish time; scheduling jitter plus the 15 ms
     * catch-up pacing made those stamps advance unevenly while every packet
     * still carried a fixed 40 ms of samples -> overlapping / jumping audio
     * timeline -> player stutter + rebuffering. A sample counter advances at
     * exactly clock_rate/s, staying consistent with the wall-clock-based RTCP
     * SR to within capture-crystal ppm. pts0/have_pts0 are still latched:
     * rtp_maybe_sr() gates on have_pts0 ("has anything been sent yet"). */
    if (!t->have_pts0){ t->pts0 = pts_us; t->have_pts0 = 1; }
    audio_gap_resync(t, pts_us, (uint32_t)len); /* M-1: jump over real gaps */
    uint32_t ts = t->ts_base + (uint32_t)t->audio_samples;
    uint8_t pkt[RTP_MTU_MAX + 32];
    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        /* budget the 12 RTP header bytes too (the old RTP_MTU check didn't
         * - unreachable with 320-byte G.711 frames, wrong on principle) */
        if (chunk > (size_t)t->mtu - 12) chunk = (size_t)t->mtu - 12;
        /* RFC 3551 4.1: marker bit belongs at the start of a talkspurt
         * (after silence). This track has no silence suppression - it's
         * one continuous talkspurt for the whole session - so that's just
         * the very first packet ever sent on it, not every packet. */
        int h = rtp_hdr(pkt, t, t->pkt_count==0, (uint32_t)(ts + off));
        memcpy(pkt+h, frame+off, chunk);
        if (emit(t, pkt, h+(int)chunk, ts) < 0) return -1; /* client gone (L3) */
        off += chunk;
    }
    t->audio_samples += len;   /* mono 8-bit: bytes == samples */
    return 0;
}

#ifdef USE_STREAM_OPUS
/* ---- Opus (RFC 7587) ---- */
int rtp_send_opus(rtp_track *t, const uint8_t *frame, size_t len, int64_t pts_us)
{
    if (len == 0) return 0;   /* nothing to send; client fine */
    /* RFC 7587 sec 4: the entire compressed Opus packet is the RTP payload,
     * carried VERBATIM - no payload header, no framing (unlike AAC's AU header
     * or H264's FU-A). One published packet == one Opus frame.
     *
     * TIMESTAMP CLOCK (the critical, easy-to-get-wrong bit): RFC 7587 mandates
     * the RTP timestamp clock rate for Opus is ALWAYS 48000 Hz - it is what SDP
     * signals (a=rtpmap opus/48000/2) and what this track was init'd with
     * (clock_rate == 48000) - REGARDLESS of the encoder's internal sample rate
     * (this HAL captures/encodes at 16 kHz by default). The HAL encodes one
     * native 40 ms AI capture frame per published packet, so every packet
     * advances the media timeline by exactly 40 ms == 1920 ticks at the 48 kHz
     * clock. Advancing by the raw PCM sample count instead (640 @ 16 kHz) would
     * run the timeline at 1/3 speed and desync/slow every client. Frame
     * duration is a constant 40 ms whatever rate the AI falls back to (the AI
     * numPerFrm is always sr*40/1000), so 1920 is correct at 8/16/48 kHz alike.
     *
     * Sample-count-driven timestamping (see rtp_send_g711's comment for why):
     * derive ts from a cumulative tick counter that only advances on a real
     * send, immune to publish wall-clock jitter, with audio_gap_resync() jumping
     * it forward over genuine gaps (mute/stall/drop) so the RTCP SR mapping
     * stays aligned. pts0/have_pts0 are still latched: rtp_maybe_sr() gates
     * on have_pts0 ("has anything been sent yet"). */
    enum { OPUS_TS_PER_FRAME = 1920 };   /* 40 ms * 48000 Hz / 1000 */
    if (!t->have_pts0){ t->pts0 = pts_us; t->have_pts0 = 1; }
    audio_gap_resync(t, pts_us, OPUS_TS_PER_FRAME);   /* M-1: jump over real gaps */
    uint32_t ts = t->ts_base + (uint32_t)t->audio_samples;
    t->audio_samples += OPUS_TS_PER_FRAME;

    /* One Opus frame at any sane VOIP bitrate is a few hundred bytes, far under
     * the MTU. Opus has no in-band fragmentation, so if a pathological rtsp.mtu
     * could not hold even one frame we drop it rather than emit a split payload
     * no receiver could reassemble - the client stays valid, just misses audio. */
    if (len + 12 > (size_t)t->mtu) return 0;
    uint8_t pkt[RTP_MTU_MAX + 32];
    /* RFC 3551 4.1 / 7587: marker bit at the start of a talkspurt. No silence
     * suppression here (one continuous talkspurt), so that's the first packet. */
    int h = rtp_hdr(pkt, t, t->pkt_count==0, ts);
    memcpy(pkt+h, frame, len);
    return emit(t, pkt, h+(int)len, ts) < 0 ? -1 : 0;   /* client gone (L3) */
}
#endif /* USE_STREAM_OPUS */

/* ---- RTCP (SR / SDES / BYE) ---- */

/* write a 28-byte Sender Report at p, return its length */
static int rtcp_wr_sr(rtp_track *t, int64_t now_us, uint8_t *p)
{
    /* NTP from realtime clock */
    struct timespec rt; clock_gettime(CLOCK_REALTIME, &rt);
    uint64_t ntp = ((uint64_t)(rt.tv_sec + 2208988800ULL) << 32) |
                   (uint32_t)((double)rt.tv_nsec * 4.294967296);
    /* RTP timestamp for "now": extrapolate from the MEDIA timeline's last
     * true media<->wall correspondence - last_rtp_ts (the media timestamp of
     * the last sent packet, sample-exact for audio, pts-derived for video)
     * paired with sr_ref_mono_us (that packet's hub publish stamp), advanced by the monotonic time elapsed since. Both
     * subtractions stay within a single clock each.
     *
     * The previous form (`now_us - t->pts0`) assumed pts_us values live on
     * ms_now_us()'s clock. They do NOT: hal_ingenic publishes pts_sanitize()
     * output (which can lead/lag the monotonic clock by seconds while the
     * sanitizer slews - perpetual on sensors whose real fps != configured
     * fps, e.g. cam-L's 25.42 vs 25), and hal_sim publishes g_epoch-
     * relative values (hours off host uptime). The SR's RTP timestamp then
     * contradicted the media packets' timestamps by exactly that offset;
     * ffmpeg's RTCP NTP-sync path (active whenever audio+video are both
     * SETUP) rebased the stream timeline once the SRs arrived and
     * invalidated already-queued video AUs to NOPTS. With audio muxed
     * alongside that is survivable, but a video-only (-an) matroska -c copy
     * client dies on it ("Can't write packet with unknown timestamp") when
     * a NOPTS AU lands after the first cluster opened - QA 13b's false
     * "isolation not holding": both healthy clients aborted in ~1s, 4/4
     * deterministic on cam-L, whose perpetually-slewing sanitizer made
     * the offset seconds-large. Regression shipped in v1.8.5 (365162d): the
     * stale-pairing fix was right to re-sample NTP fresh but moved the RTP
     * side of the pair onto the wrong clock.
     *
     * Extrapolating along the wall clock is correct across send stalls too:
     * the media/capture clock keeps ticking while frames queue, which is
     * exactly what the elapsed monotonic term models. */
    int64_t rel = t->sr_ref_mono_us ? (now_us - t->sr_ref_mono_us) : 0;
    if (rel < 0) rel = 0;
    uint32_t rtp_ts_now = t->last_rtp_ts +
        (uint32_t)((rel * (int64_t)t->clock_rate) / 1000000);
    p[0]=0x80; p[1]=200; wr_be16(p+2, 6);
    wr_be32(p+4, t->ssrc);
    wr_be32(p+8,  (uint32_t)(ntp>>32));
    wr_be32(p+12, (uint32_t)ntp);
    wr_be32(p+16, rtp_ts_now);
    wr_be32(p+20, t->pkt_count);
    wr_be32(p+24, t->octet_count);
    return 28;
}

/* write an SDES packet (one chunk: SSRC + CNAME item, RFC 3550 6.5) at p,
 * return its length (a multiple of 4; END octet + zero padding included) */
static int rtcp_wr_sdes(rtp_track *t, uint8_t *p)
{
    int cl = (int)strlen(t->cname);
    int content = 4 + 2 + cl + 1;          /* SSRC + item hdr + text + END */
    int pad = (4 - (content & 3)) & 3;     /* chunk pads to 32-bit boundary */
    int len = 4 + content + pad;
    p[0]=0x81; p[1]=202; wr_be16(p+2, (uint16_t)(len/4 - 1));
    wr_be32(p+4, t->ssrc);
    p[8] = 1;                              /* item type CNAME */
    p[9] = (uint8_t)cl;
    memcpy(p+10, t->cname, (size_t)cl);
    memset(p+10+cl, 0, (size_t)(1+pad));   /* END + padding */
    return len;
}

int rtp_maybe_sr(rtp_track *t, int64_t now_us)
{
    /* pts_to_ts() anchors pts0/ts_base on the FIRST packet sent - before
     * that there's no valid RTP-time <-> wall-clock mapping to report.
     * Skip without touching last_sr_us, so the first real SR still goes
     * out promptly once packets start flowing instead of waiting up to
     * another full second because this call "used up" the 1s gate below. */
    if (!t->have_pts0) return 0;
    if (t->last_sr_us && now_us - t->last_sr_us < 1000000) return 0;
    /* now_us is the caller's once-per-loop-iteration snapshot (rtsp.c's
     * stream_loop, P-03: one ms_now_us() per iteration to avoid a syscall
     * per media frame). That's fine for the cheap "is an SR due?" gate above,
     * but it is STALE by however long this iteration blocked between the
     * snapshot and here - the AU send can stall for seconds under TCP
     * backpressure on a large IDR (SO_SNDTIMEO is 15s), so now_us can lag
     * wall-clock by that much. Regression from adcd1dd (P-03): before it, now
     * was read right here, so the SR's RTP-timestamp math and rtcp_wr_sr()'s
     * fresh CLOCK_REALTIME NTP read were sampled back-to-back; after it they
     * drifted apart by the stall, producing an SR whose NTP<->RTP pairing is
     * off by the per-iteration send delay. Receivers that reconstruct
     * wall-clock PTS from SR data (ffmpeg/mpv) turn that inconsistent pairing
     * into visible backward/forward playback jumps - a universal burst on every
     * fresh connection plus recurring mid-stream jumps every ~15-25s, worst on
     * cameras with big/frequent IDRs (noisy night scenes). Fix: now that an SR
     * is actually going out, re-sample the monotonic clock ONCE, right here,
     * so it is paired back-to-back with rtcp_wr_sr()'s NTP read again. This
     * costs exactly one extra clock_gettime on the rare (~1/s/track) SR path,
     * NOT per frame, so P-03's per-frame syscall saving is preserved. Advance
     * last_sr_us by this fresh value too, so the next "due" gate measures from
     * the real write time, not the stale snapshot. */
    now_us = ms_now_us();
    t->last_sr_us = now_us;
    /* B2: RFC 3550 6.1 - every RTCP packet is a compound that includes an
     * SDES with CNAME. The bare 28-byte SR was tolerated by ffmpeg/VLC/
     * gstreamer but flagged by strict RTCP stacks (ONVIF conformance,
     * Genetec-class VMS). One buffer, one send: a compound must travel in
     * a single datagram / interleaved frame. */
    uint8_t buf[28 + 4+4+2+RTP_CNAME_MAX+1+3];
    int n = rtcp_wr_sr(t, now_us, buf);
    n += rtcp_wr_sdes(t, buf + n);
    /* H-1: over TCP-interleaved a failed/timed-out send can leave a torn
     * '$'-framed packet; report it so the caller stops the session. */
    return t->out(t->ctx, buf, n, 1) < 0 ? -1 : 0;
}

int rtp_send_bye(rtp_track *t, int64_t now_us)
{
    /* B3: RFC 3550 6.3.7 - a participant that leaves sends BYE. Compound
     * order per 6.1: SR (or an empty RR when nothing was ever sent, so
     * there is no valid NTP<->RTP mapping) + SDES + BYE. */
    uint8_t buf[28 + 4+4+2+RTP_CNAME_MAX+1+3 + 8];
    int n;
    if (t->have_pts0) {
        n = rtcp_wr_sr(t, now_us, buf);
    } else {
        buf[0]=0x80; buf[1]=201; wr_be16(buf+2, 1);   /* RR, RC=0 */
        wr_be32(buf+4, t->ssrc);
        n = 8;
    }
    n += rtcp_wr_sdes(t, buf + n);
    buf[n]=0x81; buf[n+1]=203; wr_be16(buf+n+2, 1);   /* BYE, SC=1 */
    wr_be32(buf+n+4, t->ssrc);
    n += 8;
    return t->out(t->ctx, buf, n, 1) < 0 ? -1 : 0;
}
