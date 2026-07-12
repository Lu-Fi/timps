/* tls.h - tiny mbedTLS server wrapper shared by HTTPS (httpd) and RTSPS (rtsp).
 * Compiled only when USE_TLS is defined (BR2_PACKAGE_MBEDTLS selected); without
 * it timps is plain HTTP/RTSP and this header is empty. */
#ifndef MS_TLS_H
#define MS_TLS_H
#ifdef USE_TLS

typedef struct ms_tls_ctx  ms_tls_ctx;   /* server context: cert + key + RNG */
typedef struct ms_tls_conn ms_tls_conn;  /* one accepted TLS connection */

/* Build a server context from a PEM cert + key file. NULL on failure. */
ms_tls_ctx  *ms_tls_ctx_new(const char *cert_file, const char *key_file);
void         ms_tls_ctx_free(ms_tls_ctx *ctx);

/* Wrap an accepted socket fd and run the handshake. NULL on failure (the caller
 * still owns/closes fd). */
ms_tls_conn *ms_tls_accept(ms_tls_ctx *ctx, int fd);

/* Blocking-ish read: >0 bytes, 0 = no data right now (retry), <0 = closed/error.
 * Write: writes all len bytes; returns len, or <0 on error. */
int          ms_tls_read(ms_tls_conn *c, void *buf, int len);
int          ms_tls_write(ms_tls_conn *c, const void *buf, int len);
void         ms_tls_close(ms_tls_conn *c);

#endif /* USE_TLS */
#endif
