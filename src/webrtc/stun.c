/* stun.c - ICE-lite responder side of STUN (see stun.h). */
#ifdef USE_WEBRTC
#include "stun.h"
#include "../sha1.h"

#include <string.h>

#define STUN_MAGIC   0x2112A442u
#define STUN_BINDING 0x0001
#define STUN_SUCCESS 0x0101

#define A_USERNAME      0x0006
#define A_INTEGRITY     0x0008
#define A_XOR_MAPPED    0x0020
#define A_USE_CANDIDATE 0x0025
#define A_FINGERPRINT   0x8028

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void     wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void     wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* Both integrity attributes are computed over the message as it WILL look
 * once the attribute itself is appended - i.e. over a header whose length
 * field counts the attribute that is not there yet. Rather than copy the
 * whole datagram to patch four bytes, every primitive below takes the patched
 * 20-byte header and the body separately. */

static uint32_t crc32_2(const uint8_t *a, int alen, const uint8_t *b, int blen)
{
    uint32_t c = 0xFFFFFFFFu;
    for (int pass = 0; pass < 2; pass++) {
        const uint8_t *p = pass ? b : a;
        int n = pass ? blen : alen;
        for (int i = 0; i < n; i++) {
            c ^= p[i];
            for (int k = 0; k < 8; k++)
                c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
        }
    }
    return c ^ 0xFFFFFFFFu;
}

/* HMAC-SHA1 (RFC 2104) over a+b, keyed with the short-term credential. sha1.c
 * is shared with the WebSocket accept key, which is why the HMAC construction
 * lives here instead: a USE_WEBRTC=0 build must not grow by a byte. */
static void hmac_sha1_2(const uint8_t *key, int klen,
                        const uint8_t *a, int alen,
                        const uint8_t *b, int blen, uint8_t out[20])
{
    uint8_t k[64], pad[64], inner[20];
    sha1_ctx c;
    memset(k, 0, sizeof k);
    if (klen > 64) {
        sha1_init(&c); sha1_update(&c, key, (size_t)klen); sha1_final(&c, k);
    } else if (klen > 0) {
        memcpy(k, key, (size_t)klen);
    }
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha1_init(&c);
    sha1_update(&c, pad, 64);
    if (alen) sha1_update(&c, a, (size_t)alen);
    if (blen) sha1_update(&c, b, (size_t)blen);
    sha1_final(&c, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha1_init(&c);
    sha1_update(&c, pad, 64);
    sha1_update(&c, inner, 20);
    sha1_final(&c, out);
}

int stun_is_stun(const uint8_t *p, int len)
{
    return len >= 20 && (p[0] & 0xC0) == 0 &&
           rd16(p + 2) + 20 <= len &&
           p[4] == 0x21 && p[5] == 0x12 && p[6] == 0xA4 && p[7] == 0x42;
}

int stun_parse_request(const uint8_t *p, int len, const char *pwd,
                       stun_req *out)
{
    if (!stun_is_stun(p, len) || rd16(p) != STUN_BINDING) return 0;
    int mlen = rd16(p + 2);
    if (mlen + 20 > len) return 0;
    len = mlen + 20;                       /* ignore any trailing bytes */

    memset(out, 0, sizeof *out);
    memcpy(out->txid, p + 8, 12);

    int integrity_ok = 0, fingerprint_seen = 0;
    int off = 20;
    while (off + 4 <= len) {
        int type = rd16(p + off), alen = rd16(p + off + 2);
        int val = off + 4;
        if (val + alen > len) return 0;
        switch (type) {
        case A_USERNAME:
            if (alen >= (int)sizeof out->username) return 0;
            memcpy(out->username, p + val, (size_t)alen);
            out->username[alen] = 0;
            break;
        case A_USE_CANDIDATE:
            out->use_candidate = 1;
            break;
        case A_INTEGRITY: {
            /* Everything after MESSAGE-INTEGRITY is outside its coverage, and
             * per RFC 5389 only FINGERPRINT may legally follow it. */
            if (alen != 20 || integrity_ok) return 0;
            uint8_t hdr[20], mac[20];
            memcpy(hdr, p, 20);
            wr16(hdr + 2, (uint16_t)(off - 20 + 24));
            hmac_sha1_2((const uint8_t *)pwd, (int)strlen(pwd),
                        hdr, 20, p + 20, off - 20, mac);
            if (memcmp(mac, p + val, 20) != 0) return 0;
            integrity_ok = 1;
            break;
        }
        case A_FINGERPRINT: {
            if (alen != 4) return 0;
            uint8_t hdr[20];
            memcpy(hdr, p, 20);
            wr16(hdr + 2, (uint16_t)(off - 20 + 8));
            uint32_t want = crc32_2(hdr, 20, p + 20, off - 20) ^ 0x5354554Eu;
            uint32_t got = ((uint32_t)p[val] << 24) | ((uint32_t)p[val+1] << 16) |
                           ((uint32_t)p[val+2] << 8) | p[val+3];
            if (want != got) return 0;
            fingerprint_seen = 1;
            break;
        }
        default: break;
        }
        off = val + alen;
        off = (off + 3) & ~3;              /* attributes are 4-byte aligned */
    }
    return integrity_ok && fingerprint_seen;
}

int stun_build_response(uint8_t *out, int cap, const uint8_t txid[12],
                        const struct sockaddr_in *peer, const char *pwd)
{
    if (cap < 20 + 12 + 24 + 8) return -1;
    wr16(out, STUN_SUCCESS);
    wr16(out + 2, 0);
    wr32(out + 4, STUN_MAGIC);
    memcpy(out + 8, txid, 12);
    int n = 20;

    /* XOR-MAPPED-ADDRESS: the peer's transport address obfuscated with the
     * magic cookie, which is what tells the browser its check reached us. */
    wr16(out + n, A_XOR_MAPPED); wr16(out + n + 2, 8);
    out[n + 4] = 0;
    out[n + 5] = 1;                                    /* IPv4 */
    wr16(out + n + 6, (uint16_t)(ntohs(peer->sin_port) ^ (STUN_MAGIC >> 16)));
    wr32(out + n + 8, ntohl(peer->sin_addr.s_addr) ^ STUN_MAGIC);
    n += 12;

    uint8_t hdr[20];
    memcpy(hdr, out, 20);
    wr16(hdr + 2, (uint16_t)(n - 20 + 24));
    wr16(out + n, A_INTEGRITY); wr16(out + n + 2, 20);
    hmac_sha1_2((const uint8_t *)pwd, (int)strlen(pwd),
                hdr, 20, out + 20, n - 20, out + n + 4);
    n += 24;

    memcpy(hdr, out, 20);
    wr16(hdr + 2, (uint16_t)(n - 20 + 8));
    wr16(out + n, A_FINGERPRINT); wr16(out + n + 2, 4);
    wr32(out + n + 4, crc32_2(hdr, 20, out + 20, n - 20) ^ 0x5354554Eu);
    n += 8;

    wr16(out + 2, (uint16_t)(n - 20));
    return n;
}

#endif /* USE_WEBRTC */
