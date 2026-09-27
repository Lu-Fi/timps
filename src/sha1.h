#ifndef MOTORS_SHA1_H
#define MOTORS_SHA1_H

#include <stddef.h>
#include <stdint.h>

/* Minimal SHA-1 (RFC 3174). Used for the WebSocket handshake's
 * Sec-WebSocket-Accept (RFC 6455 section 1.3) AND, as HMAC-SHA1, for STUN
 * MESSAGE-INTEGRITY and the SRTP/SRTCP auth tags (webrtc/stun.c, srtp.c) -
 * the latter two ARE a security boundary. HMAC-SHA1 is sound there (it does
 * not rely on collision resistance), so do not drop or swap this file without
 * those users in mind. Never use bare SHA-1 where collision resistance
 * matters (passwords, signatures, token storage). */

typedef struct {
  uint32_t state[5];
  uint64_t bitlen;
  unsigned char buf[64];
  size_t buf_len;
} sha1_ctx;

void sha1_init(sha1_ctx *ctx);
void sha1_update(sha1_ctx *ctx, const unsigned char *data, size_t len);
void sha1_final(sha1_ctx *ctx, unsigned char digest[20]);

#endif /* MOTORS_SHA1_H */
