/* dtls.c - DTLS 1.2 server for a WebRTC session (see dtls.h). */
#ifdef USE_WEBRTC
/* mbedtls_dtls_srtp_info's chosen profile is an MBEDTLS_PRIVATE field with no
 * public getter in 3.x, and mbedTLS's own ssl_server2.c reads it exactly this
 * way. The macro only renames the member (private_<name> otherwise), so the
 * struct layout this file sees stays identical to the library's. Must precede
 * every mbedTLS include. */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include "dtls.h"
#include "../log.h"
#include "../util.h"

#include <mbedtls/ssl.h>
#include <mbedtls/net_sockets.h>   /* MBEDTLS_ERR_NET_SEND_FAILED */
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>

#if MBEDTLS_VERSION_MAJOR < 3
#error "timps USE_WEBRTC needs mbedTLS 3.x (the 2.x DTLS-SRTP API differs)"
#endif
#if !defined(MBEDTLS_SSL_PROTO_DTLS)
#error "timps USE_WEBRTC needs an mbedTLS built with MBEDTLS_SSL_PROTO_DTLS"
#endif
#if !defined(MBEDTLS_SSL_DTLS_SRTP)
/* A browser's ClientHello always carries the use_srtp extension and its DTLS
 * stack aborts when the server answers without one, so a build without this
 * would negotiate nothing - fail here rather than on the camera. */
#error "timps USE_WEBRTC needs an mbedTLS built with MBEDTLS_SSL_DTLS_SRTP (buildroot: BR2_PACKAGE_MBEDTLS_DTLS_SRTP)"
#endif
#if !defined(MBEDTLS_THREADING_C)
#error "timps USE_WEBRTC needs an mbedTLS built with MBEDTLS_THREADING_C: one CTR_DRBG is shared across session threads"
#endif
#if !defined(MBEDTLS_SSL_KEYING_MATERIAL_EXPORT)
/* The SRTP session keys come out of the TLS exporter (RFC 5764 4.2); without
 * it the handshake would succeed and every media packet would be undecryptable
 * garbage to the peer. */
#error "timps USE_WEBRTC needs an mbedTLS built with MBEDTLS_SSL_KEYING_MATERIAL_EXPORT"
#endif

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>

#define MOD "WEBRTC"

struct ms_dtls_ctx {
    mbedtls_ssl_config       conf;
    mbedtls_x509_crt         cert;
    mbedtls_pk_context       key;
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
    char                     fp[100];   /* "AA:" x 32 */
};

struct ms_dtls {
    mbedtls_ssl_context ssl;
    int                 fd;
    struct sockaddr_in  peer;
    const uint8_t      *rx;      /* borrowed: valid only inside one drive step */
    int                 rxlen;
    /* DTLS retransmission timer (mbedtls_ssl_set_timer_cb). Rolled by hand
     * rather than using mbedtls_timing_* so this does not require an mbedTLS
     * built with MBEDTLS_TIMING_C. */
    int64_t             t_start;
    uint32_t            t_int_ms, t_fin_ms;
};

/* conf keeps the pointer, so this must outlive every session.
 * ONE profile, deliberately: srtp.c implements AES-128-CM with an 80-bit
 * HMAC-SHA1 tag and nothing else, so offering _32 as well would let a peer
 * negotiate a transform we cannot produce. Every browser offers _80. */
static const mbedtls_ssl_srtp_profile g_srtp_profiles[] = {
    MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80,
    MBEDTLS_TLS_SRTP_UNSET
};

static void timer_set(void *v, uint32_t int_ms, uint32_t fin_ms)
{
    ms_dtls *d = (ms_dtls *)v;
    d->t_int_ms = int_ms;
    d->t_fin_ms = fin_ms;
    if (fin_ms) d->t_start = ms_now_us();
}

static int timer_get(void *v)
{
    ms_dtls *d = (ms_dtls *)v;
    if (!d->t_fin_ms) return -1;                  /* cancelled */
    int64_t el = (ms_now_us() - d->t_start) / 1000;
    if (el >= (int64_t)d->t_fin_ms) return 2;
    if (el >= (int64_t)d->t_int_ms) return 1;
    return 0;
}

static int bio_send(void *v, const unsigned char *buf, size_t len)
{
    ms_dtls *d = (ms_dtls *)v;
    ssize_t r;
    do {
        r = sendto(d->fd, buf, len, 0,
                   (struct sockaddr *)&d->peer, sizeof d->peer);
    } while (r < 0 && errno == EINTR);
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return (int)r;
}

static int bio_recv(void *v, unsigned char *buf, size_t len)
{
    ms_dtls *d = (ms_dtls *)v;
    if (!d->rx) return MBEDTLS_ERR_SSL_WANT_READ;
    int n = d->rxlen;
    if ((size_t)n > len) {
        d->rx = NULL;                              /* record we cannot hold */
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    memcpy(buf, d->rx, (size_t)n);
    d->rx = NULL;
    return n;
}

ms_dtls_ctx *ms_dtls_ctx_new(const char *cert_file, const char *key_file)
{
    ms_dtls_ctx *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_x509_crt_init(&c->cert);
    mbedtls_pk_init(&c->key);
    mbedtls_entropy_init(&c->entropy);
    mbedtls_ctr_drbg_init(&c->drbg);

    const char *pers = "timps-dtls";
    if (mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->entropy,
                              (const unsigned char *)pers, strlen(pers)) != 0) {
        LOGE(MOD, "ctr_drbg seed failed"); goto fail;
    }
    if (mbedtls_x509_crt_parse_file(&c->cert, cert_file) != 0) {
        LOGE(MOD, "cannot parse cert %s", cert_file); goto fail;
    }
    if (mbedtls_pk_parse_keyfile(&c->key, key_file, NULL,
                                 mbedtls_ctr_drbg_random, &c->drbg) != 0) {
        LOGE(MOD, "cannot parse key %s", key_file); goto fail;
    }
    if (mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_SERVER,
                                    MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        LOGE(MOD, "dtls config defaults failed"); goto fail;
    }
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);
    if (mbedtls_ssl_conf_own_cert(&c->conf, &c->cert, &c->key) != 0) {
        LOGE(MOD, "cert/key pair rejected (%s / %s)", cert_file, key_file);
        goto fail;
    }
    /* The browser is the DTLS client (we answer a=setup:passive) and offers
     * the use_srtp extension unconditionally; without a profile in common it
     * aborts the handshake. ms_dtls_export_srtp() then pulls the SRTP session
     * keys out of the completed handshake. */
    if (mbedtls_ssl_conf_dtls_srtp_protection_profiles(&c->conf,
                                                       g_srtp_profiles) != 0) {
        LOGE(MOD, "dtls-srtp profile list rejected"); goto fail;
    }
    /* SECURITY, and a real coupling between two layers: HelloVerifyRequest
     * exists to stop a spoofed source address from making a server do
     * handshake work. Here the ICE layer above has already done that job -
     * webrtc.c hands a datagram to DTLS only from a peer address that sent a
     * STUN Binding Request carrying a valid HMAC-SHA1 over our ice-pwd. Any
     * future change that feeds this from an unvalidated address MUST restore
     * cookies here.
     * This call is NOT optional dressing: mbedtls_ssl_config_defaults()
     * installs dummy cookie callbacks that always fail, so a DTLS server that
     * never calls this cannot complete a handshake at all. */
#if defined(MBEDTLS_SSL_DTLS_HELLO_VERIFY)
    mbedtls_ssl_conf_dtls_cookies(&c->conf, NULL, NULL, NULL);
#endif
    /* No client certificate: WebRTC identifies the peer by matching its
     * a=fingerprint against the self-signed cert it presents, which is a job
     * for the media layer, not for a CA chain we do not have. */
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE);

    unsigned char h[32];
    if (mbedtls_sha256(c->cert.raw.p, c->cert.raw.len, h, 0) != 0) {
        LOGE(MOD, "certificate fingerprint failed"); goto fail;
    }
    static const char hx[] = "0123456789ABCDEF";
    for (int i = 0; i < 32; i++) {
        c->fp[3*i]   = hx[h[i] >> 4];
        c->fp[3*i+1] = hx[h[i] & 15];
        c->fp[3*i+2] = ':';
    }
    c->fp[95] = 0;
    return c;
fail:
    ms_dtls_ctx_free(c);
    return NULL;
}

void ms_dtls_ctx_free(ms_dtls_ctx *c)
{
    if (!c) return;
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_x509_crt_free(&c->cert);
    mbedtls_pk_free(&c->key);
    mbedtls_ctr_drbg_free(&c->drbg);
    mbedtls_entropy_free(&c->entropy);
    free(c);
}

const char *ms_dtls_fingerprint(const ms_dtls_ctx *c) { return c->fp; }

ms_dtls *ms_dtls_new(ms_dtls_ctx *ctx, int fd, const struct sockaddr_in *peer)
{
    ms_dtls *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->fd = fd;
    d->peer = *peer;
    mbedtls_ssl_init(&d->ssl);
    if (mbedtls_ssl_setup(&d->ssl, &ctx->conf) != 0) {
        mbedtls_ssl_free(&d->ssl);
        free(d);
        return NULL;
    }
    mbedtls_ssl_set_bio(&d->ssl, d, bio_send, bio_recv, NULL);
    mbedtls_ssl_set_timer_cb(&d->ssl, d, timer_set, timer_get);
    return d;
}

void ms_dtls_free(ms_dtls *d)
{
    if (!d) return;
    mbedtls_ssl_free(&d->ssl);
    free(d);
}

void ms_dtls_feed(ms_dtls *d, const uint8_t *p, int len)
{
    d->rx = p;
    d->rxlen = len;
}

int ms_dtls_handshake(ms_dtls *d)
{
    int r = mbedtls_ssl_handshake(&d->ssl);
    d->rx = NULL;
    if (r == 0) return 0;
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
        return 1;
    LOGW(MOD, "dtls handshake failed (-0x%x)", -r);
    return -1;
}

int ms_dtls_export_srtp(ms_dtls *d, uint8_t *out, int len)
{
    mbedtls_dtls_srtp_info info;
    mbedtls_ssl_get_dtls_srtp_negotiation_result(&d->ssl, &info);
    if (info.chosen_dtls_srtp_profile != MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80) {
        LOGW(MOD, "peer negotiated no usable SRTP profile (%u)",
             (unsigned)info.chosen_dtls_srtp_profile);
        return -1;
    }
    /* An MKI would have to be carried in every packet srtp.c builds, and no
     * browser asks for one - refuse rather than emit packets the peer would
     * parse one field short. */
    if (info.mki_len) {
        LOGW(MOD, "peer requested an SRTP MKI (%u bytes) - unsupported",
             (unsigned)info.mki_len);
        return -1;
    }
    /* mbedTLS 3.6 bug, found the hard way (SIGSEGV inside
     * mbedtls_ssl_export_keying_material, invalid read at 0x141):
     * mbedtls_ssl_handshake_wrapup() DEFERS handshake_wrapup_free_hs_transform()
     * for DTLS so the last flight can still be retransmitted, which leaves
     * ssl->transform NULL and the finished transform parked in
     * transform_negotiate. The TLS 1.2 exporter reads its randbytes straight
     * off ssl->transform without checking - so on DTLS, i.e. on every WebRTC
     * handshake, it dereferences NULL. Lend it the negotiated transform for
     * the duration of the call; nothing else touches this context, the
     * session thread owns it alone and the pointer is put back immediately. */
    mbedtls_ssl_transform *saved = d->ssl.transform;
    if (!saved) {
        if (!d->ssl.transform_negotiate) {
            LOGW(MOD, "no transform after handshake - cannot export srtp keys");
            return -1;
        }
        d->ssl.transform = d->ssl.transform_negotiate;
    }
    static const char lbl[] = "EXTRACTOR-dtls_srtp";
    int r = mbedtls_ssl_export_keying_material(&d->ssl, out, (size_t)len,
                                               lbl, sizeof lbl - 1, NULL, 0, 0);
    d->ssl.transform = saved;
    if (r != 0) {
        LOGW(MOD, "srtp keying material export failed (-0x%x)", -r);
        return -1;
    }
    return 0;
}

#endif /* USE_WEBRTC */
