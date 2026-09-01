/* Minimal SHA-1 (RFC 3174).
 *
 * Scope note, repeated from sha1.h because it is the whole reason this file
 * is allowed to exist in a project that otherwise never hand-rolls crypto:
 * the ONLY caller is the WebSocket handshake, which must compute
 *
 *   base64(SHA1(Sec-WebSocket-Key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
 *
 * RFC 6455 section 1.3 picks SHA-1 there deliberately and explains that the
 * value "does not provide any security" - it exists so that a caching proxy
 * or a non-WebSocket server cannot accidentally produce a response that looks
 * like a successful upgrade. SHA-1's collision weakness is irrelevant to that
 * job, and no library alternative is reachable here (this daemon links only
 * libjct).
 *
 * Anything that IS a security boundary - the /ws access token - goes through
 * ws_token.c / sha256.c instead and never touches this code. The two are kept
 * in separate translation units so that "which hash is this?" is answerable
 * by looking at the #include list of the caller.
 */

#include <string.h>

#include "sha1.h"

static uint32_t rol32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_block(sha1_ctx *ctx, const unsigned char block[64]) {
  uint32_t w[80];
  uint32_t a, b, c, d, e;
  int i;

  /* RFC 3174 5.a: the message is big-endian regardless of host order, so
   * assemble the words by hand rather than casting - this daemon's target
   * (MIPS32, mipsel) is little-endian. */
  for (i = 0; i < 16; i++) {
    w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
           ((uint32_t)block[i * 4 + 2] << 8) | ((uint32_t)block[i * 4 + 3]);
  }
  for (i = 16; i < 80; i++)
    w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

  a = ctx->state[0];
  b = ctx->state[1];
  c = ctx->state[2];
  d = ctx->state[3];
  e = ctx->state[4];

  for (i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20) {
      f = (b & c) | ((~b) & d);
      k = 0x5A827999u;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1u;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDCu;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6u;
    }
    uint32_t tmp = rol32(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = rol32(b, 30);
    b = a;
    a = tmp;
  }

  ctx->state[0] += a;
  ctx->state[1] += b;
  ctx->state[2] += c;
  ctx->state[3] += d;
  ctx->state[4] += e;
}

void sha1_init(sha1_ctx *ctx) {
  ctx->state[0] = 0x67452301u;
  ctx->state[1] = 0xEFCDAB89u;
  ctx->state[2] = 0x98BADCFEu;
  ctx->state[3] = 0x10325476u;
  ctx->state[4] = 0xC3D2E1F0u;
  ctx->bitlen = 0;
  ctx->buf_len = 0;
}

void sha1_update(sha1_ctx *ctx, const unsigned char *data, size_t len) {
  size_t i = 0;

  ctx->bitlen += (uint64_t)len * 8u;

  /* top up a partial block first, then run whole blocks straight out of the
   * caller's buffer (avoids a memcpy per block for the common case where the
   * whole message arrives in one update) */
  if (ctx->buf_len > 0) {
    size_t need = 64 - ctx->buf_len;
    size_t take = (len < need) ? len : need;
    memcpy(ctx->buf + ctx->buf_len, data, take);
    ctx->buf_len += take;
    i += take;
    if (ctx->buf_len == 64) {
      sha1_block(ctx, ctx->buf);
      ctx->buf_len = 0;
    }
  }

  for (; i + 64 <= len; i += 64)
    sha1_block(ctx, data + i);

  if (i < len) {
    memcpy(ctx->buf, data + i, len - i);
    ctx->buf_len = len - i;
  }
}

void sha1_final(sha1_ctx *ctx, unsigned char digest[20]) {
  uint64_t bitlen = ctx->bitlen;
  unsigned char pad = 0x80;
  unsigned char lenbe[8];
  int i;

  /* RFC 3174 4: append 0x80, then zeros until the length is 56 mod 64, then
   * the 64-bit big-endian bit count. Reusing sha1_update() for the padding
   * would corrupt ctx->bitlen, so drive the buffer directly. */
  ctx->buf[ctx->buf_len++] = pad;
  if (ctx->buf_len > 56) {
    memset(ctx->buf + ctx->buf_len, 0, 64 - ctx->buf_len);
    sha1_block(ctx, ctx->buf);
    ctx->buf_len = 0;
  }
  memset(ctx->buf + ctx->buf_len, 0, 56 - ctx->buf_len);

  for (i = 0; i < 8; i++)
    lenbe[i] = (unsigned char)(bitlen >> (56 - 8 * i));
  memcpy(ctx->buf + 56, lenbe, 8);
  sha1_block(ctx, ctx->buf);

  for (i = 0; i < 5; i++) {
    digest[i * 4] = (unsigned char)(ctx->state[i] >> 24);
    digest[i * 4 + 1] = (unsigned char)(ctx->state[i] >> 16);
    digest[i * 4 + 2] = (unsigned char)(ctx->state[i] >> 8);
    digest[i * 4 + 3] = (unsigned char)(ctx->state[i]);
  }

  /* the context can hold a fragment of the (public) handshake key; wiping is
   * cheap and keeps the habit consistent with sha256_final() where it does
   * matter */
  memset(ctx, 0, sizeof(*ctx));
}
