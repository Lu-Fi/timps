#include "rtp.h"
#include "../codec/nal.h"
#include "../codec/aac.h"
#include "../util.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#define RTP_MTU 1400

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
     * which point this timestamp math (and rtp_maybe_sr()'s identical
     * computation) goes wrong. A correct fix needs the track to periodically
     * rebase pts0/ts_base rather than a one-line clamp here, so it's left
     * for a follow-up rather than patched in this pass. */
    return t->ts_base + (uint32_t)((rel * (int64_t)t->clock_rate) / 1000000);
}

void rtp_track_init(rtp_track *t, int pt, uint32_t clock_rate,
                    rtp_out_fn out, void *ctx)
{
    memset(t, 0, sizeof(*t));
    t->payload_type = pt;
    t->clock_rate   = clock_rate;
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
    uint8_t pkt[RTP_MTU + 32];
    if (n + 12 <= RTP_MTU) {
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
        if (chunk > RTP_MTU - 12 - 2) chunk = RTP_MTU - 12 - 2; /* -12 RTP hdr, -2 FU ind+hdr */
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
    uint8_t pkt[RTP_MTU + 32];
    if (n + 12 <= RTP_MTU) {
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
        if (chunk > RTP_MTU - 12 - 3) chunk = RTP_MTU - 12 - 3; /* -12 RTP hdr, -3 FU hdr */
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
    uint8_t pkt[RTP_MTU + 32];

    if (plen + 12 + 4 <= RTP_MTU) {
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
        size_t chunk = RTP_MTU - 12 - 4;
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
     * SR to within capture-crystal ppm. pts0 is still anchored so
     * rtp_maybe_sr() has its NTP<->RTP reference. */
    if (!t->have_pts0){ t->pts0 = pts_us; t->have_pts0 = 1; }
    audio_gap_resync(t, pts_us, (uint32_t)len); /* M-1: jump over real gaps */
    uint32_t ts = t->ts_base + (uint32_t)t->audio_samples;
    uint8_t pkt[RTP_MTU + 32];
    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > RTP_MTU) chunk = RTP_MTU;
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

/* ---- RTCP Sender Report ---- */
int rtp_maybe_sr(rtp_track *t, int64_t now_us)
{
    /* pts_to_ts() anchors pts0/ts_base on the FIRST packet sent - before
     * that there's no valid RTP-time <-> wall-clock mapping to report.
     * Skip without touching last_sr_us, so the first real SR still goes
     * out promptly once packets start flowing instead of waiting up to
     * another full second because this call "used up" the 1s gate below. */
    if (!t->have_pts0) return 0;
    if (t->last_sr_us && now_us - t->last_sr_us < 1000000) return 0;
    t->last_sr_us = now_us;
    /* NTP from realtime clock */
    struct timespec rt; clock_gettime(CLOCK_REALTIME, &rt);
    uint64_t ntp = ((uint64_t)(rt.tv_sec + 2208988800ULL) << 32) |
                   (uint32_t)((double)rt.tv_nsec * 4.294967296);
    /* RTP timestamp for "now", using the SAME pts0/ts_base anchor pts_to_ts()
     * uses for sample timestamps - not t->last_rtp_ts (the last *sent
     * packet*'s ts, which is up to one frame/AAC-frame stale, and
     * unboundedly stale if the track stalls). ffmpeg/go2rtc/Frigate-style
     * NVRs derive A/V sync from exactly this NTP<->RTP pair, so a stale
     * pairing here makes lip-sync jitter every SR interval. now_us is
     * CLOCK_MONOTONIC (ms_now_us(), see rtsp.c's caller), the same clock
     * pts_us values are on, so this stays consistent with pts_to_ts(). */
    int64_t rel = now_us - t->pts0;
    if (rel < 0) rel = 0;
    uint32_t rtp_ts_now = t->ts_base +
        (uint32_t)((rel * (int64_t)t->clock_rate) / 1000000);
    uint8_t sr[28];
    sr[0]=0x80; sr[1]=200; wr_be16(sr+2, 6);
    wr_be32(sr+4, t->ssrc);
    wr_be32(sr+8,  (uint32_t)(ntp>>32));
    wr_be32(sr+12, (uint32_t)ntp);
    wr_be32(sr+16, rtp_ts_now);
    wr_be32(sr+20, t->pkt_count);
    wr_be32(sr+24, t->octet_count);
    /* H-1: over TCP-interleaved a failed/timed-out send can leave a torn
     * '$'-framed packet; report it so the caller stops the session. */
    return t->out(t->ctx, sr, sizeof sr, 1) < 0 ? -1 : 0;
}
