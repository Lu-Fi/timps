#ifndef TIMPS_AUTH_H
#define TIMPS_AUTH_H

#include <stdint.h>

/* ---------- MD5 ---------------------------------------------------------- */

typedef struct {
    uint32_t state[4];
    uint32_t count[2];
    uint8_t  buf[64];
} MD5_CTX;

void    MD5_Init(MD5_CTX *ctx);
void    MD5_Update(MD5_CTX *ctx, const void *data, size_t len);
void    MD5_Final(uint8_t digest[16], MD5_CTX *ctx);
/* Convenience: hash a string, write 32-char lowercase hex into out[33] */
void    md5_hex(const char *s, char out[33]);

/* ---------- HTTP Basic --------------------------------------------------- */
/*
 * Validate "Authorization: Basic <base64>" header value.
 * Returns 1 if credentials match user:pass, 0 otherwise.
 */
int http_basic_check(const char *header_value,
                     const char *user, const char *pass);

/* Build the WWW-Authenticate header value for Basic. */
void http_basic_challenge(char *buf, size_t len, const char *realm);

/* ---------- RTSP Digest -------------------------------------------------- */
/*
 * Generate a 32-char hex nonce (16 random bytes → hex).
 * out must be at least 33 bytes.
 */
void rtsp_digest_nonce(char out[33]);

/*
 * Verify RTSP Digest response.
 *   method   : RTSP method string ("DESCRIBE", "PLAY", …)
 *   uri      : request URI
 *   nonce    : the nonce we sent
 *   response : the hex response from the client (32 chars)
 *   user,pass: credentials to verify against
 * Returns 1 on match, 0 on mismatch.
 */
int rtsp_digest_verify(const char *method, const char *uri,
                       const char *nonce,  const char *response,
                       const char *user,   const char *pass);

/*
 * Build the WWW-Authenticate Digest challenge header value.
 * buf must be large enough (256 bytes is fine).
 */
void rtsp_digest_challenge(char *buf, size_t len,
                           const char *realm, const char *nonce);

#endif /* TIMPS_AUTH_H */
