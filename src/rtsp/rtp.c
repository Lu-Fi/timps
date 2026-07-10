#include "rtp.h"
#include "../codec/nal.h"
#include "../codec/aac.h"
#include "../util.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define RTP_MTU 1400

static uint32_t pts_to_ts(rtp_track *t, int64_t pts_us)
{
    return t->ts_base + (uint32_t)((pts_us * (int64_t)t->clock_rate) / 1000000);
}

void rtp_track_init(rtp_track *t, int pt, uint32_t clock_rate,
                    rtp_out_fn out, void *ctx)
{
    memset(t, 0, sizeof(*t));
    t->payload_type = pt;
    t->clock_rate   = clock_rate;
    t->out = out; t->ctx = ctx;
    t->ssrc = ((uint32_t)rand()<<16) ^ (uint32_t)rand() ^ (uint32_t)time(NULL);
    t->seq  = (uint16_t)rand();
    t->ts_base = (uint32_t)rand();
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

static int emit(rtp_track *t, uint8_t *pkt, int len, uint32_t ts)
{
    t->pkt_count++;
    t->octet_count += (uint32_t)(len - 12);
    t->last_rtp_ts = ts;
    return t->out(t->ctx, pkt, len, 0);
}

/* ---- H264 (RFC 6184) ---- */
static void send_h264_nal(rtp_track *t, const uint8_t *nal, size_t n,
                          uint32_t ts, int last_in_au)
{
    uint8_t pkt[RTP_MTU + 32];
    if (n + 12 <= RTP_MTU) {
        int h = rtp_hdr(pkt, t, last_in_au, ts);
        memcpy(pkt+h, nal, n);
        emit(t, pkt, h+(int)n, ts);
        return;
    }
    /* FU-A */
    uint8_t nri = nal[0] & 0x60;
    uint8_t typ = nal[0] & 0x1F;
    const uint8_t *p = nal + 1;
    size_t left = n - 1;
    int first = 1;
    while (left > 0) {
        size_t chunk = left;
        if (chunk > RTP_MTU - 2) chunk = RTP_MTU - 2;
        int end = (chunk == left);
        int h = rtp_hdr(pkt, t, (end && last_in_au), ts);
        pkt[h]   = nri | 28;                          /* FU indicator */
        pkt[h+1] = (uint8_t)((first?0x80:0)|(end?0x40:0)|typ); /* FU header */
        memcpy(pkt+h+2, p, chunk);
        emit(t, pkt, h+2+(int)chunk, ts);
        p += chunk; left -= chunk; first = 0;
    }
}

void rtp_send_h264(rtp_track *t, const uint8_t *au, size_t len, int64_t pts_us)
{
    uint32_t ts = pts_to_ts(t, pts_us);
    /* collect NAL list to know which is last (for marker bit) */
    nal_iter it; nal_unit u;
    nal_iter_init(&it, au, len);
    nal_unit list[64]; int cnt=0;
    while (cnt<64 && nal_iter_next(&it, &u)) list[cnt++]=u;
    for (int i=0;i<cnt;i++)
        send_h264_nal(t, list[i].data, list[i].len, ts, i==cnt-1);
}

/* ---- H265 (RFC 7798) ---- */
static void send_h265_nal(rtp_track *t, const uint8_t *nal, size_t n,
                          uint32_t ts, int last_in_au)
{
    uint8_t pkt[RTP_MTU + 32];
    if (n + 12 <= RTP_MTU) {
        int h = rtp_hdr(pkt, t, last_in_au, ts);
        memcpy(pkt+h, nal, n);
        emit(t, pkt, h+(int)n, ts);
        return;
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
        if (chunk > RTP_MTU - 3) chunk = RTP_MTU - 3;
        int end = (chunk == left);
        int h = rtp_hdr(pkt, t, (end && last_in_au), ts);
        pkt[h]   = (uint8_t)((49<<1) | (lid>>5));
        pkt[h+1] = (uint8_t)((lid<<3) | tid);
        pkt[h+2] = (uint8_t)((first?0x80:0)|(end?0x40:0)|typ);
        memcpy(pkt+h+3, p, chunk);
        emit(t, pkt, h+3+(int)chunk, ts);
        p += chunk; left -= chunk; first = 0;
    }
}

void rtp_send_h265(rtp_track *t, const uint8_t *au, size_t len, int64_t pts_us)
{
    uint32_t ts = pts_to_ts(t, pts_us);
    nal_iter it; nal_unit u;
    nal_iter_init(&it, au, len);
    nal_unit list[64]; int cnt=0;
    while (cnt<64 && nal_iter_next(&it, &u)) list[cnt++]=u;
    for (int i=0;i<cnt;i++)
        send_h265_nal(t, list[i].data, list[i].len, ts, i==cnt-1);
}

/* ---- AAC (RFC 3640 mpeg4-generic, single AU per packet) ---- */
void rtp_send_aac(rtp_track *t, const uint8_t *frame, size_t len, int64_t pts_us)
{
    size_t plen; int off = aac_adts_strip(frame, len, &plen);
    const uint8_t *au = frame + off;
    uint32_t ts = pts_to_ts(t, pts_us);
    uint8_t pkt[RTP_MTU + 32];
    if (plen + 12 + 4 > RTP_MTU) plen = RTP_MTU - 16; /* clamp (rare) */
    int h = rtp_hdr(pkt, t, 1, ts);
    /* AU-headers-length = 16 bits; one AU header of 16 bits:
     * 13 bits size + 3 bits index(0) */
    wr_be16(pkt+h, 16);
    uint16_t auh = (uint16_t)((plen & 0x1FFF) << 3);
    wr_be16(pkt+h+2, auh);
    memcpy(pkt+h+4, au, plen);
    emit(t, pkt, h+4+(int)plen, ts);
}

/* ---- G.711 (raw, fragment if needed) ---- */
void rtp_send_g711(rtp_track *t, const uint8_t *frame, size_t len, int64_t pts_us)
{
    uint32_t ts = pts_to_ts(t, pts_us);
    uint8_t pkt[RTP_MTU + 32];
    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > RTP_MTU) chunk = RTP_MTU;
        int h = rtp_hdr(pkt, t, 1, (uint32_t)(ts + off));
        memcpy(pkt+h, frame+off, chunk);
        emit(t, pkt, h+(int)chunk, ts);
        off += chunk;
    }
}

/* ---- RTCP Sender Report ---- */
void rtp_maybe_sr(rtp_track *t, int64_t now_us)
{
    if (t->last_sr_us && now_us - t->last_sr_us < 1000000) return;
    t->last_sr_us = now_us;
    /* NTP from realtime clock */
    struct timespec rt; clock_gettime(CLOCK_REALTIME, &rt);
    uint64_t ntp = ((uint64_t)(rt.tv_sec + 2208988800ULL) << 32) |
                   (uint32_t)((double)rt.tv_nsec * 4.294967296);
    uint8_t sr[28];
    sr[0]=0x80; sr[1]=200; wr_be16(sr+2, 6);
    wr_be32(sr+4, t->ssrc);
    wr_be32(sr+8,  (uint32_t)(ntp>>32));
    wr_be32(sr+12, (uint32_t)ntp);
    wr_be32(sr+16, t->last_rtp_ts);
    wr_be32(sr+20, t->pkt_count);
    wr_be32(sr+24, t->octet_count);
    t->out(t->ctx, sr, sizeof sr, 1);
}
