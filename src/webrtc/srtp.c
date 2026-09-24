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

/* SB[x] scaled by the MixColumns column (2,1,1,3). One table instead of the
 * usual four: the other three are byte rotations of it. */
static const uint32_t TE[256] = {
0xc66363a5,0xf87c7c84,0xee777799,0xf67b7b8d,0xfff2f20d,0xd66b6bbd,
0xde6f6fb1,0x91c5c554,0x60303050,0x02010103,0xce6767a9,0x562b2b7d,
0xe7fefe19,0xb5d7d762,0x4dababe6,0xec76769a,0x8fcaca45,0x1f82829d,
0x89c9c940,0xfa7d7d87,0xeffafa15,0xb25959eb,0x8e4747c9,0xfbf0f00b,
0x41adadec,0xb3d4d467,0x5fa2a2fd,0x45afafea,0x239c9cbf,0x53a4a4f7,
0xe4727296,0x9bc0c05b,0x75b7b7c2,0xe1fdfd1c,0x3d9393ae,0x4c26266a,
0x6c36365a,0x7e3f3f41,0xf5f7f702,0x83cccc4f,0x6834345c,0x51a5a5f4,
0xd1e5e534,0xf9f1f108,0xe2717193,0xabd8d873,0x62313153,0x2a15153f,
0x0804040c,0x95c7c752,0x46232365,0x9dc3c35e,0x30181828,0x379696a1,
0x0a05050f,0x2f9a9ab5,0x0e070709,0x24121236,0x1b80809b,0xdfe2e23d,
0xcdebeb26,0x4e272769,0x7fb2b2cd,0xea75759f,0x1209091b,0x1d83839e,
0x582c2c74,0x341a1a2e,0x361b1b2d,0xdc6e6eb2,0xb45a5aee,0x5ba0a0fb,
0xa45252f6,0x763b3b4d,0xb7d6d661,0x7db3b3ce,0x5229297b,0xdde3e33e,
0x5e2f2f71,0x13848497,0xa65353f5,0xb9d1d168,0x00000000,0xc1eded2c,
0x40202060,0xe3fcfc1f,0x79b1b1c8,0xb65b5bed,0xd46a6abe,0x8dcbcb46,
0x67bebed9,0x7239394b,0x944a4ade,0x984c4cd4,0xb05858e8,0x85cfcf4a,
0xbbd0d06b,0xc5efef2a,0x4faaaae5,0xedfbfb16,0x864343c5,0x9a4d4dd7,
0x66333355,0x11858594,0x8a4545cf,0xe9f9f910,0x04020206,0xfe7f7f81,
0xa05050f0,0x783c3c44,0x259f9fba,0x4ba8a8e3,0xa25151f3,0x5da3a3fe,
0x804040c0,0x058f8f8a,0x3f9292ad,0x219d9dbc,0x70383848,0xf1f5f504,
0x63bcbcdf,0x77b6b6c1,0xafdada75,0x42212163,0x20101030,0xe5ffff1a,
0xfdf3f30e,0xbfd2d26d,0x81cdcd4c,0x180c0c14,0x26131335,0xc3ecec2f,
0xbe5f5fe1,0x359797a2,0x884444cc,0x2e171739,0x93c4c457,0x55a7a7f2,
0xfc7e7e82,0x7a3d3d47,0xc86464ac,0xba5d5de7,0x3219192b,0xe6737395,
0xc06060a0,0x19818198,0x9e4f4fd1,0xa3dcdc7f,0x44222266,0x542a2a7e,
0x3b9090ab,0x0b888883,0x8c4646ca,0xc7eeee29,0x6bb8b8d3,0x2814143c,
0xa7dede79,0xbc5e5ee2,0x160b0b1d,0xaddbdb76,0xdbe0e03b,0x64323256,
0x743a3a4e,0x140a0a1e,0x924949db,0x0c06060a,0x4824246c,0xb85c5ce4,
0x9fc2c25d,0xbdd3d36e,0x43acacef,0xc46262a6,0x399191a8,0x319595a4,
0xd3e4e437,0xf279798b,0xd5e7e732,0x8bc8c843,0x6e373759,0xda6d6db7,
0x018d8d8c,0xb1d5d564,0x9c4e4ed2,0x49a9a9e0,0xd86c6cb4,0xac5656fa,
0xf3f4f407,0xcfeaea25,0xca6565af,0xf47a7a8e,0x47aeaee9,0x10080818,
0x6fbabad5,0xf0787888,0x4a25256f,0x5c2e2e72,0x381c1c24,0x57a6a6f1,
0x73b4b4c7,0x97c6c651,0xcbe8e823,0xa1dddd7c,0xe874749c,0x3e1f1f21,
0x964b4bdd,0x61bdbddc,0x0d8b8b86,0x0f8a8a85,0xe0707090,0x7c3e3e42,
0x71b5b5c4,0xcc6666aa,0x904848d8,0x06030305,0xf7f6f601,0x1c0e0e12,
0xc26161a3,0x6a35355f,0xae5757f9,0x69b9b9d0,0x17868691,0x99c1c158,
0x3a1d1d27,0x279e9eb9,0xd9e1e138,0xebf8f813,0x2b9898b3,0x22111133,
0xd26969bb,0xa9d9d970,0x078e8e89,0x339494a7,0x2d9b9bb6,0x3c1e1e22,
0x15878792,0xc9e9e920,0x87cece49,0xaa5555ff,0x50282878,0xa5dfdf7a,
0x038c8c8f,0x59a1a1f8,0x09898980,0x1a0d0d17,0x65bfbfda,0xd7e6e631,
0x844242c6,0xd06868b8,0x824141c3,0x299999b0,0x5a2d2d77,0x1e0f0f11,
0x7bb0b0cb,0xa85454fc,0x6dbbbbd6,0x2c16163a,
};

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

static uint32_t ror(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }

static uint32_t sub_word(uint32_t w)
{
    return ((uint32_t)SB[w >> 24] << 24) | ((uint32_t)SB[(w >> 16) & 0xFF] << 16) |
           ((uint32_t)SB[(w >> 8) & 0xFF] << 8) | SB[w & 0xFF];
}

static void aes128_expand(const uint8_t key[16], uint32_t rk[44])
{
    uint32_t rcon = 0x01000000;
    for (int i = 0; i < 4; i++) rk[i] = rd32(key + 4 * i);
    for (int i = 4; i < 44; i++) {
        uint32_t t = rk[i - 1];
        if ((i & 3) == 0) {
            t = sub_word((t << 8) | (t >> 24)) ^ rcon;
            rcon = (rcon & 0x80000000) ? ((rcon << 1) ^ 0x1B000000) : (rcon << 1);
        }
        rk[i] = rk[i - 4] ^ t;
    }
}

static void aes128_encrypt(const uint32_t rk[44], const uint8_t in[16],
                           uint8_t out[16])
{
    uint32_t s0 = rd32(in) ^ rk[0],     s1 = rd32(in + 4) ^ rk[1];
    uint32_t s2 = rd32(in + 8) ^ rk[2], s3 = rd32(in + 12) ^ rk[3];
    uint32_t t0, t1, t2, t3;
    for (int r = 1; r < 10; r++) {
        const uint32_t *k = rk + 4 * r;
        t0 = TE[s0 >> 24] ^ ror(TE[(s1 >> 16) & 0xFF], 8) ^
             ror(TE[(s2 >> 8) & 0xFF], 16) ^ ror(TE[s3 & 0xFF], 24) ^ k[0];
        t1 = TE[s1 >> 24] ^ ror(TE[(s2 >> 16) & 0xFF], 8) ^
             ror(TE[(s3 >> 8) & 0xFF], 16) ^ ror(TE[s0 & 0xFF], 24) ^ k[1];
        t2 = TE[s2 >> 24] ^ ror(TE[(s3 >> 16) & 0xFF], 8) ^
             ror(TE[(s0 >> 8) & 0xFF], 16) ^ ror(TE[s1 & 0xFF], 24) ^ k[2];
        t3 = TE[s3 >> 24] ^ ror(TE[(s0 >> 16) & 0xFF], 8) ^
             ror(TE[(s1 >> 8) & 0xFF], 16) ^ ror(TE[s2 & 0xFF], 24) ^ k[3];
        s0 = t0; s1 = t1; s2 = t2; s3 = t3;
    }
    wr32(out,      (((uint32_t)SB[s0 >> 24] << 24) | ((uint32_t)SB[(s1 >> 16) & 0xFF] << 16) |
                    ((uint32_t)SB[(s2 >> 8) & 0xFF] << 8) | SB[s3 & 0xFF]) ^ rk[40]);
    wr32(out + 4,  (((uint32_t)SB[s1 >> 24] << 24) | ((uint32_t)SB[(s2 >> 16) & 0xFF] << 16) |
                    ((uint32_t)SB[(s3 >> 8) & 0xFF] << 8) | SB[s0 & 0xFF]) ^ rk[41]);
    wr32(out + 8,  (((uint32_t)SB[s2 >> 24] << 24) | ((uint32_t)SB[(s3 >> 16) & 0xFF] << 16) |
                    ((uint32_t)SB[(s0 >> 8) & 0xFF] << 8) | SB[s1 & 0xFF]) ^ rk[42]);
    wr32(out + 12, (((uint32_t)SB[s3 >> 24] << 24) | ((uint32_t)SB[(s0 >> 16) & 0xFF] << 16) |
                    ((uint32_t)SB[(s1 >> 8) & 0xFF] << 8) | SB[s2 & 0xFF]) ^ rk[43]);
}

/* AES counter mode over `buf`. `ctr` is the full 16-byte initial block; only
 * its low 16 bits ever advance, which is all SRTP's 2^16-block limit needs. */
static void aes_ctr(const uint32_t rk[44], uint8_t ctr[16],
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
 * hoisting this into a common file would drag AES into that link. The key is
 * fixed per session, so its ipad/opad blocks are absorbed once here and every
 * packet starts from a copy. */
static void hmac_sha1_key(srtp_keys *k, const uint8_t key[20])
{
    uint8_t pad[64];
    memset(pad, 0x36, sizeof pad);
    for (int i = 0; i < 20; i++) pad[i] ^= key[i];
    sha1_init(&k->hmac_in);
    sha1_update(&k->hmac_in, pad, 64);
    memset(pad, 0x5c, sizeof pad);
    for (int i = 0; i < 20; i++) pad[i] ^= key[i];
    sha1_init(&k->hmac_out);
    sha1_update(&k->hmac_out, pad, 64);
    memset(pad, 0, sizeof pad);
}

static void hmac_sha1_2(const srtp_keys *k,
                        const uint8_t *a, int alen,
                        const uint8_t *b, int blen, uint8_t out[20])
{
    uint8_t inner[20];
    sha1_ctx c = k->hmac_in;
    if (alen) sha1_update(&c, a, (size_t)alen);
    if (blen) sha1_update(&c, b, (size_t)blen);
    sha1_final(&c, inner);
    c = k->hmac_out;
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
    uint32_t rk[44];
    uint8_t x[16], blk[16], o[16];
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
    uint8_t k[16], a[20];
    srtp_kdf(mk, ms, 0x00, k, 16);           aes128_expand(k, d->rtp.rk);
    srtp_kdf(mk, ms, 0x01, a, 20);           hmac_sha1_key(&d->rtp, a);
    srtp_kdf(mk, ms, 0x02, d->rtp.salt, 14);
    srtp_kdf(mk, ms, 0x03, k, 16);           aes128_expand(k, d->rtcp.rk);
    srtp_kdf(mk, ms, 0x04, a, 20);           hmac_sha1_key(&d->rtcp, a);
    srtp_kdf(mk, ms, 0x05, d->rtcp.salt, 14);
    memset(k, 0, sizeof k);
    memset(a, 0, sizeof a);
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
    hmac_sha1_2(&s->out.rtp, p, len, roc_be, 4, tag);
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
    hmac_sha1_2(&s->out.rtcp, p, len + 4, NULL, 0, tag);
    memcpy(p + len + 4, tag, SRTP_TAG_LEN);
    return len + 4 + SRTP_TAG_LEN;
}

int srtp_unprotect_rtcp(srtp_session *s, uint8_t *p, int len)
{
    if (!s->ready || len < 8 + 4 + SRTP_TAG_LEN) return -1;
    int n = len - SRTP_TAG_LEN;                 /* authenticated portion */
    uint8_t tag[20];
    hmac_sha1_2(&s->in.rtcp, p, n, NULL, 0, tag);
    if (!ct_eq(tag, p + n, SRTP_TAG_LEN)) return -1;

    uint32_t w = rd32(p + n - 4);
    uint32_t idx = w & 0x7FFFFFFFu;
    uint32_t ssrc = rd32(p + 4);
    /* Replay gate, per sender SSRC (srtp.h). Strictly increasing rather than a
     * sliding window: the only thing we act on is PLI/FIR, a reordered one is
     * harmless to lose, and a replayed one would otherwise let a passive
     * attacker drive the encoder's IDR rate.
     * The tag was verified above, so only an authenticated peer can ever put an
     * SSRC in this table. */
    srtp_rtcp_src *src = NULL;
    for (int i = 0; i < SRTP_MAX_RTCP_SOURCES; i++) {
        if (s->in_rtcp[i].used && s->in_rtcp[i].ssrc == ssrc) { src = &s->in_rtcp[i]; break; }
        if (!s->in_rtcp[i].used && !src) src = &s->in_rtcp[i];   /* first free */
    }
    if (!src) return -1;
    if (src->used && idx <= src->index) return -1;
    src->used  = 1;
    src->ssrc  = ssrc;
    src->index = idx;

    int plen = n - 4;
    if (w & 0x80000000u) {
        uint8_t iv[16];
        iv_build(iv, s->in.rtcp.salt, ssrc, 0, idx);
        aes_ctr(s->in.rtcp.rk, iv, p + 8, plen - 8);
    }
    return plen;
}

#endif /* USE_WEBRTC */
