/* srtp.c - SRTP/SRTCP for SRTP_AES128_CM_HMAC_SHA1_80 (see srtp.h). */
#ifdef USE_WEBRTC
#include "srtp.h"
#include "../sha1.h"

#include <string.h>

/* ---------------- AES-128 (FIPS-197, encryption only) ---------------- */

static const uint8_t SB[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };

static uint8_t xt(uint8_t a) { return (uint8_t)((a << 1) ^ ((a >> 7) * 0x1b)); }

static void aes128_expand(const uint8_t key[16], uint8_t rk[176])
{
    memcpy(rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4];
        memcpy(t, rk + i - 4, 4);
        if ((i & 15) == 0) {
            uint8_t tmp = t[0];
            t[0] = (uint8_t)(SB[t[1]] ^ rcon);
            t[1] = SB[t[2]];
            t[2] = SB[t[3]];
            t[3] = SB[tmp];
            rcon = xt(rcon);
        }
        for (int j = 0; j < 4; j++) rk[i + j] = (uint8_t)(rk[i - 16 + j] ^ t[j]);
    }
}

/* state is column-major: s[4*col + row], which is also the wire order */
static void aes128_encrypt(const uint8_t rk[176], const uint8_t in[16],
                           uint8_t out[16])
{
    uint8_t s[16], t;
    for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ rk[i]);
    for (int r = 1; r <= 10; r++) {
        for (int i = 0; i < 16; i++) s[i] = SB[s[i]];
        t = s[1];  s[1]  = s[5];  s[5]  = s[9];  s[9]  = s[13]; s[13] = t;
        t = s[2];  s[2]  = s[10]; s[10] = t;
        t = s[6];  s[6]  = s[14]; s[14] = t;
        t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7]  = s[3];   s[3]  = t;
        if (r != 10) {
            for (int c = 0; c < 16; c += 4) {
                uint8_t a0 = s[c], a1 = s[c+1], a2 = s[c+2], a3 = s[c+3];
                uint8_t x = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
                s[c]   = (uint8_t)(a0 ^ x ^ xt((uint8_t)(a0 ^ a1)));
                s[c+1] = (uint8_t)(a1 ^ x ^ xt((uint8_t)(a1 ^ a2)));
                s[c+2] = (uint8_t)(a2 ^ x ^ xt((uint8_t)(a2 ^ a3)));
                s[c+3] = (uint8_t)(a3 ^ x ^ xt((uint8_t)(a3 ^ a0)));
            }
        }
        for (int i = 0; i < 16; i++) s[i] ^= rk[r * 16 + i];
    }
    memcpy(out, s, 16);
}

/* AES counter mode over `buf`. `ctr` is the full 16-byte initial block; only
 * its low 16 bits ever advance, which is all SRTP's 2^16-block limit needs. */
static void aes_ctr(const uint8_t rk[176], uint8_t ctr[16],
                    uint8_t *buf, int len)
{
    uint8_t ks[16];
    int off = 0;
    while (off < len) {
        aes128_encrypt(rk, ctr, ks);
        int n = len - off;
        if (n > 16) n = 16;
        for (int i = 0; i < n; i++) buf[off + i] ^= ks[i];
        off += n;
        if (++ctr[15] == 0) ctr[14]++;
    }
}

/* ---------------- HMAC-SHA1 ---------------- */

/* Same construction as stun.c's, duplicated rather than shared: stun.c is
 * also the host test harness's only dependency (`make test-stun`), and
 * hoisting this into a common file would drag AES into that link. */
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
    for (int i = 0; i < 64; i++) pad[i] = (uint8_t)(k[i] ^ 0x36);
    sha1_init(&c);
    sha1_update(&c, pad, 64);
    if (alen) sha1_update(&c, a, (size_t)alen);
    if (blen) sha1_update(&c, b, (size_t)blen);
    sha1_final(&c, inner);
    for (int i = 0; i < 64; i++) pad[i] = (uint8_t)(k[i] ^ 0x5c);
    sha1_init(&c);
    sha1_update(&c, pad, 64);
    sha1_update(&c, inner, 20);
    sha1_final(&c, out);
}

static int ct_eq(const uint8_t *a, const uint8_t *b, int n)
{
    uint8_t d = 0;
    for (int i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

/* ---------------- key derivation ---------------- */

void srtp_kdf(const uint8_t master_key[16], const uint8_t master_salt[14],
              uint8_t label, uint8_t *out, int outlen)
{
    uint8_t rk[176], x[16], blk[16], o[16];
    aes128_expand(master_key, rk);
    /* x = (key_id XOR master_salt) * 2^16, key_id = label || (index DIV kdr).
     * kdr is 0 here (the profile's default), so the index term is 0 and only
     * the label byte - right-aligned into the 14-byte salt - survives. */
    memcpy(x, master_salt, 14);
    x[14] = x[15] = 0;
    x[7] = (uint8_t)(x[7] ^ label);
    for (int n = 0, ctr = 0; n < outlen; ctr++) {
        memcpy(blk, x, 16);
        blk[14] = (uint8_t)(ctr >> 8);
        blk[15] = (uint8_t)ctr;
        aes128_encrypt(rk, blk, o);
        int c = outlen - n;
        if (c > 16) c = 16;
        memcpy(out + n, o, (size_t)c);
        n += c;
    }
}

static void derive_dir(srtp_dir *d, const uint8_t mk[16], const uint8_t ms[14])
{
    uint8_t k[16];
    srtp_kdf(mk, ms, 0x00, k, 16);           aes128_expand(k, d->rtp.rk);
    srtp_kdf(mk, ms, 0x01, d->rtp.auth, 20);
    srtp_kdf(mk, ms, 0x02, d->rtp.salt, 14);
    srtp_kdf(mk, ms, 0x03, k, 16);           aes128_expand(k, d->rtcp.rk);
    srtp_kdf(mk, ms, 0x04, d->rtcp.auth, 20);
    srtp_kdf(mk, ms, 0x05, d->rtcp.salt, 14);
    memset(k, 0, sizeof k);
}

int srtp_init(srtp_session *s, const uint8_t *km, int km_len, int we_are_server)
{
    if (!s || !km || km_len < SRTP_KEYING_LEN) return -1;
    /* RFC 5764 4.2 layout: client key, server key, client salt, server salt */
    const uint8_t *ck = km, *sk = km + 16, *cs = km + 32, *ss = km + 46;
    memset(s, 0, sizeof *s);
    if (we_are_server) { derive_dir(&s->out, sk, ss); derive_dir(&s->in, ck, cs); }
    else               { derive_dir(&s->out, ck, cs); derive_dir(&s->in, sk, ss); }
    s->ready = 1;
    return 0;
}

/* ---------------- per-packet IV (RFC 3711 4.1.1) ----------------
 * IV = (salt * 2^16) XOR (SSRC * 2^64) XOR (index * 2^16), i.e. the salt in
 * bytes 0..13, the SSRC XORed over bytes 4..7 and the 48-bit packet index
 * over bytes 8..13; the low 16 bits are the block counter. */
static void iv_build(uint8_t iv[16], const uint8_t salt[14],
                     uint32_t ssrc, uint32_t idx_hi, uint32_t idx_lo)
{
    memcpy(iv, salt, 14);
    iv[14] = iv[15] = 0;
    iv[4] ^= (uint8_t)(ssrc >> 24); iv[5] ^= (uint8_t)(ssrc >> 16);
    iv[6] ^= (uint8_t)(ssrc >> 8);  iv[7] ^= (uint8_t)ssrc;
    iv[8]  ^= (uint8_t)(idx_hi >> 8); iv[9] ^= (uint8_t)idx_hi;
    iv[10] ^= (uint8_t)(idx_lo >> 24); iv[11] ^= (uint8_t)(idx_lo >> 16);
    iv[12] ^= (uint8_t)(idx_lo >> 8);  iv[13] ^= (uint8_t)idx_lo;
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | p[3];
}
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* ---------------- SRTP ---------------- */

int srtp_protect_rtp(srtp_session *s, uint8_t *p, int len, int cap)
{
    if (!s->ready || len < 12 || cap < len + SRTP_TAG_LEN) return -1;
    int hlen = 12 + 4 * (p[0] & 0x0F);
    if (p[0] & 0x10) {                          /* RFC 3550 5.3.1 extension */
        if (len < hlen + 4) return -1;
        hlen += 4 + 4 * (((int)p[hlen + 2] << 8) | p[hlen + 3]);
    }
    if (hlen > len) return -1;

    uint32_t ssrc = rd32(p + 8);
    srtp_stream *st = NULL;
    for (int i = 0; i < SRTP_MAX_STREAMS; i++) {
        if (s->out_rtp[i].used && s->out_rtp[i].ssrc == ssrc) { st = &s->out_rtp[i]; break; }
        if (!s->out_rtp[i].used && !st) st = &s->out_rtp[i];   /* first free */
    }
    /* Table full and no match: refusing the packet is the only safe answer -
     * borrowing another SSRC's ROC would build an IV the peer cannot derive. */
    if (!st) return -1;
    if (!st->used) { st->used = 1; st->ssrc = ssrc; }

    uint16_t seq = (uint16_t)((p[2] << 8) | p[3]);
    /* Our own sequence numbers are emitted strictly increasing, so a seq that
     * went backwards is a wrap, not reordering (RFC 3711 3.3.1). */
    if (st->have_seq && seq < st->last_seq) st->roc++;
    st->last_seq = seq;
    st->have_seq = 1;

    uint8_t iv[16];
    iv_build(iv, s->out.rtp.salt, ssrc,
             st->roc >> 16, (st->roc << 16) | seq);
    aes_ctr(s->out.rtp.rk, iv, p + hlen, len - hlen);

    uint8_t roc_be[4], tag[20];
    wr32(roc_be, st->roc);
    hmac_sha1_2(s->out.rtp.auth, 20, p, len, roc_be, 4, tag);
    memcpy(p + len, tag, SRTP_TAG_LEN);
    return len + SRTP_TAG_LEN;
}

/* ---------------- SRTCP (RFC 3711 3.4) ----------------
 * The first 8 bytes (header + sender SSRC) stay in the clear; everything
 * after them is encrypted, then the E-flag + 31-bit index word is appended
 * and the tag covers all of it. */

int srtp_protect_rtcp(srtp_session *s, uint8_t *p, int len, int cap)
{
    if (!s->ready || len < 8 || cap < len + 4 + SRTP_TAG_LEN) return -1;
    uint32_t idx = (++s->rtcp_index) & 0x7FFFFFFFu;
    uint8_t iv[16];
    iv_build(iv, s->out.rtcp.salt, rd32(p + 4), 0, idx);
    aes_ctr(s->out.rtcp.rk, iv, p + 8, len - 8);
    wr32(p + len, 0x80000000u | idx);
    uint8_t tag[20];
    hmac_sha1_2(s->out.rtcp.auth, 20, p, len + 4, NULL, 0, tag);
    memcpy(p + len + 4, tag, SRTP_TAG_LEN);
    return len + 4 + SRTP_TAG_LEN;
}

int srtp_unprotect_rtcp(srtp_session *s, uint8_t *p, int len)
{
    if (!s->ready || len < 8 + 4 + SRTP_TAG_LEN) return -1;
    int n = len - SRTP_TAG_LEN;                 /* authenticated portion */
    uint8_t tag[20];
    hmac_sha1_2(s->in.rtcp.auth, 20, p, n, NULL, 0, tag);
    if (!ct_eq(tag, p + n, SRTP_TAG_LEN)) return -1;

    uint32_t w = rd32(p + n - 4);
    uint32_t idx = w & 0x7FFFFFFFu;
    /* Replay gate. Strictly increasing rather than a sliding window: the only
     * thing we act on is PLI/FIR, a reordered one is harmless to lose, and a
     * replayed one would otherwise let a passive attacker drive the encoder's
     * IDR rate. */
    if (s->in_rtcp_index && idx <= s->in_rtcp_index) return -1;
    s->in_rtcp_index = idx;

    int plen = n - 4;
    if (w & 0x80000000u) {
        uint8_t iv[16];
        iv_build(iv, s->in.rtcp.salt, rd32(p + 4), 0, idx);
        aes_ctr(s->in.rtcp.rk, iv, p + 8, plen - 8);
    }
    return plen;
}

#endif /* USE_WEBRTC */
