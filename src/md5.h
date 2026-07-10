/* md5.h - compact MD5 (public-domain style) for RTSP Digest auth */
#ifndef MS_MD5_H
#define MS_MD5_H
#include <stdint.h>
#include <stddef.h>

typedef struct { uint32_t a,b,c,d; uint64_t len; uint8_t buf[64]; size_t n; } md5_ctx;
void md5_init(md5_ctx *c);
void md5_update(md5_ctx *c, const void *data, size_t len);
void md5_final(md5_ctx *c, uint8_t out[16]);
/* convenience: MD5 of a string -> 32-char lowercase hex (33 bytes incl NUL) */
void md5_hex(const char *s, char out[33]);

#endif
