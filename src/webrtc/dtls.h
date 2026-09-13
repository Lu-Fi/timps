/* dtls.h - DTLS 1.2 server over the ONE UDP socket a WebRTC session owns.
 *
 * Separate from tls.c on purpose: that context is
 * MBEDTLS_SSL_TRANSPORT_STREAM and owns the socket through mbedtls_net_*,
 * while here STUN, DTLS and (later) SRTP share a single datagram socket and
 * are demuxed by the caller (RFC 7983), so the BIO is fed packet by packet.
 * Only compiled with USE_WEBRTC.
 */
#ifndef MS_DTLS_H
#define MS_DTLS_H
#ifdef USE_WEBRTC

#include <stdint.h>
#include <netinet/in.h>

typedef struct ms_dtls_ctx ms_dtls_ctx;   /* shared: cert, key, DRBG, config */
typedef struct ms_dtls     ms_dtls;       /* one handshake/session */

/* Parses the same PEM pair the HTTPS/RTSPS listener uses. NULL on failure
 * (the caller then refuses to offer WebRTC at all). */
ms_dtls_ctx *ms_dtls_ctx_new(const char *cert_file, const char *key_file);
void         ms_dtls_ctx_free(ms_dtls_ctx *c);
/* "AA:BB:.." upper-case hex of SHA-256 over the certificate DER, i.e. exactly
 * what goes into the SDP answer's a=fingerprint line. */
const char  *ms_dtls_fingerprint(const ms_dtls_ctx *c);

ms_dtls *ms_dtls_new(ms_dtls_ctx *ctx, int fd, const struct sockaddr_in *peer);
void     ms_dtls_free(ms_dtls *d);
/* Hand over one received datagram that the RFC 7983 demux classified as DTLS.
 * At most one packet is held at a time; a second before the first is consumed
 * is dropped, which DTLS retransmission covers. */
void     ms_dtls_feed(ms_dtls *d, const uint8_t *p, int len);
/* Drive the handshake: 1 = still in progress, 0 = completed, -1 = failed. */
int      ms_dtls_handshake(ms_dtls *d);
/* RFC 5764 4.2 keying material for the negotiated DTLS-SRTP profile, i.e. the
 * TLS exporter under the label "EXTRACTOR-dtls_srtp" with no context. Valid
 * only after ms_dtls_handshake() returned 0; `len` must be exactly what the
 * profile needs (60 for SRTP_AES128_CM_HMAC_SHA1_80). Returns 0 on success,
 * and <0 if the export failed or the peer did not settle on that profile. */
int      ms_dtls_export_srtp(ms_dtls *d, uint8_t *out, int len);

#endif /* USE_WEBRTC */
#endif
