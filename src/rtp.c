#include <string.h>
#include "rtp.h"

void rtp_session_init(RtpSession *s, int pt, uint32_t ts_freq)
{
    static uint32_t ssrc_seed = 0x12345678;
    s->seq     = (uint16_t)(ssrc_seed & 0xffff);
    s->ssrc    = ssrc_seed ^ 0xdeadbeef;
    ssrc_seed += 0x9e3779b9;
    s->ts_freq = ts_freq;
    s->pt      = pt;
}

int rtp_build_header(uint8_t *buf, int pt, uint16_t seq,
                     uint32_t ts, uint32_t ssrc, int marker)
{
    buf[0]  = 0x80;                          /* V=2, P=0, X=0, CC=0 */
    buf[1]  = (uint8_t)((marker ? 0x80 : 0) | (pt & 0x7f));
    buf[2]  = (uint8_t)(seq >> 8);
    buf[3]  = (uint8_t)(seq & 0xff);
    buf[4]  = (uint8_t)(ts >> 24);
    buf[5]  = (uint8_t)(ts >> 16);
    buf[6]  = (uint8_t)(ts >> 8);
    buf[7]  = (uint8_t)(ts & 0xff);
    buf[8]  = (uint8_t)(ssrc >> 24);
    buf[9]  = (uint8_t)(ssrc >> 16);
    buf[10] = (uint8_t)(ssrc >> 8);
    buf[11] = (uint8_t)(ssrc & 0xff);
    return RTP_HDR_LEN;
}

/* -------------------------------------------------------------------------
 * Find the next Annex-B NAL start code.
 * Returns pointer to 00 00 01 or NULL.
 * -----------------------------------------------------------------------*/
static const uint8_t *find_start_code(const uint8_t *p, const uint8_t *end)
{
    for (; p + 3 <= end; p++) {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1) return p;
        if (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) return p;
    }
    return NULL;
}

/* Skip 3- or 4-byte start code, return pointer to first NALU byte */
static const uint8_t *skip_sc(const uint8_t *p)
{
    if (p[2] == 1) return p + 3;
    return p + 4;
}

/* -------------------------------------------------------------------------
 * H.264 sender (RFC 6184)
 * -----------------------------------------------------------------------*/
int rtp_send_h264(RtpSession *s, const uint8_t *data, int len,
                  uint32_t pts_ms,
                  rtp_write_fn write, int channel, void *ud)
{
    const uint8_t *end   = data + len;
    const uint8_t *p     = data;
    uint32_t       ts    = pts_ms * (s->ts_freq / 1000);
    uint8_t        pkt[RTP_HDR_LEN + RTP_MAX_PKT + 8];
    int            sent  = 0;

    /* Walk NAL units */
    while (p < end) {
        const uint8_t *sc = find_start_code(p, end);
        if (!sc) break;
        const uint8_t *nalu     = skip_sc(sc);
        const uint8_t *next_sc  = find_start_code(nalu, end);
        int             nalu_len = next_sc ? (int)(next_sc - nalu)
                                           : (int)(end     - nalu);
        if (nalu_len <= 0) { p = nalu; continue; }

        /* Drop AUD NALUs in RTP */
        uint8_t nal_type = nalu[0] & 0x1f;
        if (nal_type == 9) { p = nalu + nalu_len; continue; }

        if (nalu_len <= RTP_MAX_PKT) {
            /* Single NALU packet */
            int hlen = rtp_build_header(pkt, s->pt, s->seq++, ts,
                                        s->ssrc, 1);
            memcpy(pkt + hlen, nalu, nalu_len);
            write(channel, pkt, hlen + nalu_len, ud);
            sent++;
        } else {
            /* FU-A fragmentation */
            uint8_t fu_indicator = (nalu[0] & 0xe0) | 28;
            uint8_t fu_hdr_start = 0x80 | (nalu[0] & 0x1f); /* S bit */
            const uint8_t *frag = nalu + 1;
            int remaining       = nalu_len - 1;

            while (remaining > 0) {
                int frag_sz = remaining > RTP_MAX_PKT - 2
                            ? RTP_MAX_PKT - 2 : remaining;
                int last    = (frag_sz == remaining);
                int marker  = last;
                uint8_t fu_hdr = (fu_hdr_start & 0x1f)
                               | (last ? 0x40 : 0);   /* E bit */

                int hlen = rtp_build_header(pkt, s->pt, s->seq++, ts,
                                            s->ssrc, marker);
                pkt[hlen]   = fu_indicator;
                pkt[hlen+1] = fu_hdr;
                memcpy(pkt + hlen + 2, frag, frag_sz);
                write(channel, pkt, hlen + 2 + frag_sz, ud);

                frag         += frag_sz;
                remaining    -= frag_sz;
                fu_hdr_start &= ~0x80; /* clear S bit */
                sent++;
            }
        }
        p = nalu + nalu_len;
    }
    return sent;
}

/* -------------------------------------------------------------------------
 * H.265 sender (RFC 7798)
 * -----------------------------------------------------------------------*/
int rtp_send_h265(RtpSession *s, const uint8_t *data, int len,
                  uint32_t pts_ms,
                  rtp_write_fn write, int channel, void *ud)
{
    const uint8_t *end   = data + len;
    const uint8_t *p     = data;
    uint32_t       ts    = pts_ms * (s->ts_freq / 1000);
    uint8_t        pkt[RTP_HDR_LEN + RTP_MAX_PKT + 8];
    int            sent  = 0;

    while (p < end) {
        const uint8_t *sc = find_start_code(p, end);
        if (!sc) break;
        const uint8_t *nalu    = skip_sc(sc);
        const uint8_t *next_sc = find_start_code(nalu, end);
        int nalu_len = next_sc ? (int)(next_sc - nalu)
                               : (int)(end     - nalu);
        if (nalu_len <= 0 || nalu_len < 2) { p = nalu; continue; }

        uint8_t nal_type = (nalu[0] >> 1) & 0x3f;
        /* Skip AUD (35) */
        if (nal_type == 35) { p = nalu + nalu_len; continue; }

        if (nalu_len <= RTP_MAX_PKT) {
            int hlen = rtp_build_header(pkt, s->pt, s->seq++, ts,
                                        s->ssrc, 1);
            memcpy(pkt + hlen, nalu, nalu_len);
            write(channel, pkt, hlen + nalu_len, ud);
            sent++;
        } else {
            /* FU fragmentation (RFC 7798 §4.4.3) */
            uint8_t fu_type   = 49;  /* FU NAL unit type */
            /* HEVC NAL header of the FU container */
            uint8_t nal_hdr0  = (fu_type << 1) & 0x7e;
            uint8_t nal_hdr1  = 1;  /* layer_id=0, tid=1 */
            uint8_t start_bit = 0x80;

            const uint8_t *frag = nalu + 2;
            int remaining       = nalu_len - 2;

            while (remaining > 0) {
                int frag_sz = remaining > RTP_MAX_PKT - 3
                            ? RTP_MAX_PKT - 3 : remaining;
                int last    = (frag_sz == remaining);
                uint8_t fu_hdr = (nal_type & 0x3f)
                               | start_bit
                               | (last ? 0x40 : 0);

                int hlen = rtp_build_header(pkt, s->pt, s->seq++, ts,
                                            s->ssrc, last);
                pkt[hlen]   = nal_hdr0;
                pkt[hlen+1] = nal_hdr1;
                pkt[hlen+2] = fu_hdr;
                memcpy(pkt + hlen + 3, frag, frag_sz);
                write(channel, pkt, hlen + 3 + frag_sz, ud);

                frag      += frag_sz;
                remaining -= frag_sz;
                start_bit  = 0;
                sent++;
            }
        }
        p = nalu + nalu_len;
    }
    return sent;
}

/* -------------------------------------------------------------------------
 * G.711 sender (RFC 3551)
 * -----------------------------------------------------------------------*/
int rtp_send_g711(RtpSession *s, const uint8_t *data, int len,
                  uint32_t ts,
                  rtp_write_fn write, int channel, void *ud)
{
    uint8_t pkt[RTP_HDR_LEN + 320 + 4];
    int hlen = rtp_build_header(pkt, s->pt, s->seq++, ts,
                                s->ssrc, 1);
    if (len > 320) len = 320;
    memcpy(pkt + hlen, data, len);
    return write(channel, pkt, hlen + len, ud);
}

/* -------------------------------------------------------------------------
 * AAC sender (RFC 3640, AAC-hbr mode)
 * -----------------------------------------------------------------------*/
int rtp_send_aac(RtpSession *s, const uint8_t *data, int len,
                 uint32_t ts,
                 rtp_write_fn write, int channel, void *ud)
{
    uint8_t pkt[RTP_HDR_LEN + 4 + 1400];

    /* AU-headers-length (2 bytes) + one AU-header (16 bits = size 13bits + idx 3bits) */
    int hlen = rtp_build_header(pkt, s->pt, s->seq++, ts,
                                s->ssrc, 1);
    uint16_t au_size    = (uint16_t)len;
    uint16_t au_headers_len = 16; /* 1 AU-header of 16 bits */

    pkt[hlen+0] = (uint8_t)(au_headers_len >> 8);
    pkt[hlen+1] = (uint8_t)(au_headers_len & 0xff);
    pkt[hlen+2] = (uint8_t)(au_size >> 5);
    pkt[hlen+3] = (uint8_t)((au_size & 0x1f) << 3);   /* AU-Index = 0 */

    if (len > 1400) len = 1400;
    memcpy(pkt + hlen + 4, data, len);
    return write(channel, pkt, hlen + 4 + len, ud);
}
