/*
 * auth.c — MD5, HTTP Basic, RTSP Digest authentication
 *
 * MD5 implementation is a clean-room RFC 1321 implementation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "auth.h"

/* =========================================================================
 * MD5 (RFC 1321)
 * ========================================================================= */

#define S11 7
#define S12 12
#define S13 17
#define S14 22
#define S21 5
#define S22 9
#define S23 14
#define S24 20
#define S31 4
#define S32 11
#define S33 16
#define S34 23
#define S41 6
#define S42 10
#define S43 15
#define S44 21

static const uint8_t PADDING[64] = { 0x80 };

#define F(x,y,z)  (((x)&(y)) | ((~x)&(z)))
#define G(x,y,z)  (((x)&(z)) | ((y)&(~z)))
#define H(x,y,z)  ((x)^(y)^(z))
#define I(x,y,z)  ((y)^((x)|(~z)))

#define ROTATE(x,n)  (((x)<<(n)) | ((x)>>(32-(n))))

#define FF(a,b,c,d,x,s,ac) { \
    (a) += F(b,c,d) + (x) + (uint32_t)(ac); \
    (a)  = ROTATE(a,s); \
    (a) += (b); }
#define GG(a,b,c,d,x,s,ac) { \
    (a) += G(b,c,d) + (x) + (uint32_t)(ac); \
    (a)  = ROTATE(a,s); \
    (a) += (b); }
#define HH(a,b,c,d,x,s,ac) { \
    (a) += H(b,c,d) + (x) + (uint32_t)(ac); \
    (a)  = ROTATE(a,s); \
    (a) += (b); }
#define II(a,b,c,d,x,s,ac) { \
    (a) += I(b,c,d) + (x) + (uint32_t)(ac); \
    (a)  = ROTATE(a,s); \
    (a) += (b); }

static void md5_encode(uint8_t *out, const uint32_t *in, size_t len)
{
    size_t i, j;
    for (i = 0, j = 0; j < len; i++, j += 4) {
        out[j]   = (uint8_t)(in[i]);
        out[j+1] = (uint8_t)(in[i] >> 8);
        out[j+2] = (uint8_t)(in[i] >> 16);
        out[j+3] = (uint8_t)(in[i] >> 24);
    }
}

static void md5_decode(uint32_t *out, const uint8_t *in, size_t len)
{
    size_t i, j;
    for (i = 0, j = 0; j < len; i++, j += 4)
        out[i] = ((uint32_t)in[j])        | ((uint32_t)in[j+1]<<8)
               | ((uint32_t)in[j+2]<<16)  | ((uint32_t)in[j+3]<<24);
}

static void md5_transform(uint32_t state[4], const uint8_t block[64])
{
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], x[16];
    md5_decode(x, block, 64);

    FF(a,b,c,d, x[ 0], S11, 0xd76aa478UL);
    FF(d,a,b,c, x[ 1], S12, 0xe8c7b756UL);
    FF(c,d,a,b, x[ 2], S13, 0x242070dbUL);
    FF(b,c,d,a, x[ 3], S14, 0xc1bdceeeUL);
    FF(a,b,c,d, x[ 4], S11, 0xf57c0fafUL);
    FF(d,a,b,c, x[ 5], S12, 0x4787c62aUL);
    FF(c,d,a,b, x[ 6], S13, 0xa8304613UL);
    FF(b,c,d,a, x[ 7], S14, 0xfd469501UL);
    FF(a,b,c,d, x[ 8], S11, 0x698098d8UL);
    FF(d,a,b,c, x[ 9], S12, 0x8b44f7afUL);
    FF(c,d,a,b, x[10], S13, 0xffff5bb1UL);
    FF(b,c,d,a, x[11], S14, 0x895cd7beUL);
    FF(a,b,c,d, x[12], S11, 0x6b901122UL);
    FF(d,a,b,c, x[13], S12, 0xfd987193UL);
    FF(c,d,a,b, x[14], S13, 0xa679438eUL);
    FF(b,c,d,a, x[15], S14, 0x49b40821UL);

    GG(a,b,c,d, x[ 1], S21, 0xf61e2562UL);
    GG(d,a,b,c, x[ 6], S22, 0xc040b340UL);
    GG(c,d,a,b, x[11], S23, 0x265e5a51UL);
    GG(b,c,d,a, x[ 0], S24, 0xe9b6c7aaUL);
    GG(a,b,c,d, x[ 5], S21, 0xd62f105dUL);
    GG(d,a,b,c, x[10], S22, 0x02441453UL);
    GG(c,d,a,b, x[15], S23, 0xd8a1e681UL);
    GG(b,c,d,a, x[ 4], S24, 0xe7d3fbc8UL);
    GG(a,b,c,d, x[ 9], S21, 0x21e1cde6UL);
    GG(d,a,b,c, x[14], S22, 0xc33707d6UL);
    GG(c,d,a,b, x[ 3], S23, 0xf4d50d87UL);
    GG(b,c,d,a, x[ 8], S24, 0x455a14edUL);
    GG(a,b,c,d, x[13], S21, 0xa9e3e905UL);
    GG(d,a,b,c, x[ 2], S22, 0xfcefa3f8UL);
    GG(c,d,a,b, x[ 7], S23, 0x676f02d9UL);
    GG(b,c,d,a, x[12], S24, 0x8d2a4c8aUL);

    HH(a,b,c,d, x[ 5], S31, 0xfffa3942UL);
    HH(d,a,b,c, x[ 8], S32, 0x8771f681UL);
    HH(c,d,a,b, x[11], S33, 0x6d9d6122UL);
    HH(b,c,d,a, x[14], S34, 0xfde5380cUL);
    HH(a,b,c,d, x[ 1], S31, 0xa4beea44UL);
    HH(d,a,b,c, x[ 4], S32, 0x4bdecfa9UL);
    HH(c,d,a,b, x[ 7], S33, 0xf6bb4b60UL);
    HH(b,c,d,a, x[10], S34, 0xbebfbc70UL);
    HH(a,b,c,d, x[13], S31, 0x289b7ec6UL);
    HH(d,a,b,c, x[ 0], S32, 0xeaa127faUL);
    HH(c,d,a,b, x[ 3], S33, 0xd4ef3085UL);
    HH(b,c,d,a, x[ 6], S34, 0x04881d05UL);
    HH(a,b,c,d, x[ 9], S31, 0xd9d4d039UL);
    HH(d,a,b,c, x[12], S32, 0xe6db99e5UL);
    HH(c,d,a,b, x[15], S33, 0x1fa27cf8UL);
    HH(b,c,d,a, x[ 2], S34, 0xc4ac5665UL);

    II(a,b,c,d, x[ 0], S41, 0xf4292244UL);
    II(d,a,b,c, x[ 7], S42, 0x432aff97UL);
    II(c,d,a,b, x[14], S43, 0xab9423a7UL);
    II(b,c,d,a, x[ 5], S44, 0xfc93a039UL);
    II(a,b,c,d, x[12], S41, 0x655b59c3UL);
    II(d,a,b,c, x[ 3], S42, 0x8f0ccc92UL);
    II(c,d,a,b, x[10], S43, 0xffeff47dUL);
    II(b,c,d,a, x[ 1], S44, 0x85845dd1UL);
    II(a,b,c,d, x[ 8], S41, 0x6fa87e4fUL);
    II(d,a,b,c, x[15], S42, 0xfe2ce6e0UL);
    II(c,d,a,b, x[ 6], S43, 0xa3014314UL);
    II(b,c,d,a, x[13], S44, 0x4e0811a1UL);
    II(a,b,c,d, x[ 4], S41, 0xf7537e82UL);
    II(d,a,b,c, x[11], S42, 0xbd3af235UL);
    II(c,d,a,b, x[ 2], S43, 0x2ad7d2bbUL);
    II(b,c,d,a, x[ 9], S44, 0xeb86d391UL);

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    memset(x, 0, sizeof(x));
}

void MD5_Init(MD5_CTX *ctx)
{
    ctx->count[0] = ctx->count[1] = 0;
    ctx->state[0] = 0x67452301UL;
    ctx->state[1] = 0xefcdab89UL;
    ctx->state[2] = 0x98badcfeUL;
    ctx->state[3] = 0x10325476UL;
}

void MD5_Update(MD5_CTX *ctx, const void *data, size_t len)
{
    const uint8_t *input = (const uint8_t *)data;
    size_t i, idx, partLen;

    idx = (size_t)((ctx->count[0] >> 3) & 0x3f);
    if ((ctx->count[0] += (uint32_t)(len << 3)) < (uint32_t)(len << 3))
        ctx->count[1]++;
    ctx->count[1] += (uint32_t)(len >> 29);

    partLen = 64 - idx;
    if (len >= partLen) {
        memcpy(&ctx->buf[idx], input, partLen);
        md5_transform(ctx->state, ctx->buf);
        for (i = partLen; i + 63 < len; i += 64)
            md5_transform(ctx->state, &input[i]);
        idx = 0;
    } else {
        i = 0;
    }
    memcpy(&ctx->buf[idx], &input[i], len - i);
}

void MD5_Final(uint8_t digest[16], MD5_CTX *ctx)
{
    uint8_t bits[8];
    size_t  idx, padLen;

    md5_encode(bits, ctx->count, 8);
    idx    = (size_t)((ctx->count[0] >> 3) & 0x3f);
    padLen = (idx < 56) ? (56 - idx) : (120 - idx);
    MD5_Update(ctx, PADDING, padLen);
    MD5_Update(ctx, bits, 8);
    md5_encode(digest, ctx->state, 16);
    memset(ctx, 0, sizeof(*ctx));
}

void md5_hex(const char *s, char out[33])
{
    MD5_CTX  ctx;
    uint8_t  digest[16];
    int      i;

    MD5_Init(&ctx);
    MD5_Update(&ctx, s, strlen(s));
    MD5_Final(digest, &ctx);
    for (i = 0; i < 16; i++)
        snprintf(&out[i*2], 3, "%02x", digest[i]);
    out[32] = '\0';
}

/* =========================================================================
 * Base64 decode (for HTTP Basic)
 * ========================================================================= */

static const char b64tbl[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64val(char c)
{
    const char *p = strchr(b64tbl, c);
    return p ? (int)(p - b64tbl) : -1;
}

static size_t base64_decode(const char *src, char *dst, size_t dstlen)
{
    size_t out = 0;
    int    v;

    while (*src && out + 2 < dstlen) {
        int a = b64val(src[0]), b = b64val(src[1]);
        if (a < 0 || b < 0) break;
        dst[out++] = (char)((a << 2) | (b >> 4));
        if (src[2] == '=' || out >= dstlen) break;
        v = b64val(src[2]);
        if (v < 0) break;
        dst[out++] = (char)(((b & 0xf) << 4) | (v >> 2));
        if (src[3] == '=' || out >= dstlen) break;
        v = b64val(src[3]);
        if (v < 0) break;
        dst[out++] = (char)(((b64val(src[2]) & 0x3) << 6) | v);
        src += 4;
    }
    if (out < dstlen) dst[out] = '\0';
    return out;
}

/* =========================================================================
 * HTTP Basic
 * ========================================================================= */

int http_basic_check(const char *header_value,
                     const char *user, const char *pass)
{
    char decoded[256];
    char expected[256];
    const char *b64;

    /* header_value is "Basic <base64>" */
    b64 = header_value;
    if (strncasecmp(b64, "Basic ", 6) == 0) b64 += 6;
    while (*b64 == ' ') b64++;

    base64_decode(b64, decoded, sizeof(decoded));

    snprintf(expected, sizeof(expected), "%s:%s", user, pass);
    return strcmp(decoded, expected) == 0 ? 1 : 0;
}

void http_basic_challenge(char *buf, size_t len, const char *realm)
{
    snprintf(buf, len, "Basic realm=\"%s\"", realm);
}

/* =========================================================================
 * RTSP Digest (RFC 2617 subset)
 * ========================================================================= */

void rtsp_digest_nonce(char out[33])
{
    MD5_CTX  ctx;
    uint8_t  digest[16];
    char     tmp[64];
    int      i;

    /* Use time + random bytes as nonce seed */
    snprintf(tmp, sizeof(tmp), "%ld%d", (long)time(NULL), rand());
    MD5_Init(&ctx);
    MD5_Update(&ctx, tmp, strlen(tmp));
    MD5_Final(digest, &ctx);
    for (i = 0; i < 16; i++)
        snprintf(&out[i*2], 3, "%02x", digest[i]);
    out[32] = '\0';
}

int rtsp_digest_verify(const char *method, const char *uri,
                       const char *nonce,  const char *response,
                       const char *user,   const char *pass)
{
    char  ha1_str[256], ha2_str[256], resp_str[512];
    char  ha1[33],      ha2[33],      expected[33];

    /* HA1 = MD5(user:realm:password) — we use realm="timps" */
    snprintf(ha1_str, sizeof(ha1_str), "%s:timps:%s", user, pass);
    md5_hex(ha1_str, ha1);

    /* HA2 = MD5(method:uri) */
    snprintf(ha2_str, sizeof(ha2_str), "%s:%s", method, uri);
    md5_hex(ha2_str, ha2);

    /* response = MD5(HA1:nonce:HA2) */
    snprintf(resp_str, sizeof(resp_str), "%s:%s:%s", ha1, nonce, ha2);
    md5_hex(resp_str, expected);

    return (strncasecmp(response, expected, 32) == 0) ? 1 : 0;
}

void rtsp_digest_challenge(char *buf, size_t len,
                           const char *realm, const char *nonce)
{
    snprintf(buf, len,
             "Digest realm=\"%s\", nonce=\"%s\", algorithm=MD5",
             realm, nonce);
}
