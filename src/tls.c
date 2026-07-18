/* tls.c - mbedTLS server wrapper (see tls.h). Only compiled with USE_TLS. */
#ifdef USE_TLS
#include "tls.h"
#include "log.h"

#include <mbedtls/ssl.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/version.h>

#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>

#define MOD "TLS"

struct ms_tls_ctx {
    mbedtls_ssl_config       conf;
    mbedtls_x509_crt         cert;
    mbedtls_pk_context       key;
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
};

struct ms_tls_conn {
    mbedtls_ssl_context ssl;
    mbedtls_net_context net;
};

ms_tls_ctx *ms_tls_ctx_new(const char *cert_file, const char *key_file)
{
    ms_tls_ctx *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_x509_crt_init(&c->cert);
    mbedtls_pk_init(&c->key);
    mbedtls_entropy_init(&c->entropy);
    mbedtls_ctr_drbg_init(&c->drbg);

    const char *pers = "timps-tls";
    if (mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->entropy,
                              (const unsigned char *)pers, strlen(pers)) != 0) {
        LOGE(MOD, "ctr_drbg seed failed"); goto fail;
    }
    if (mbedtls_x509_crt_parse_file(&c->cert, cert_file) != 0) {
        LOGE(MOD, "cannot parse cert %s", cert_file); goto fail;
    }
#if MBEDTLS_VERSION_MAJOR >= 3
    if (mbedtls_pk_parse_keyfile(&c->key, key_file, NULL,
                                 mbedtls_ctr_drbg_random, &c->drbg) != 0)
#else
    if (mbedtls_pk_parse_keyfile(&c->key, key_file, NULL) != 0)
#endif
    {
        LOGE(MOD, "cannot parse key %s", key_file); goto fail;
    }
    if (mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_SERVER,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) goto fail;
#if MBEDTLS_VERSION_MAJOR < 3
    /* M2: mbedTLS 2.x's PRESET_DEFAULT still negotiates TLS 1.0/1.1 - require
     * TLS 1.2 (3.x already defaults to >=1.2 and drops this API in 3.2+) */
    mbedtls_ssl_conf_min_version(&c->conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);
#endif
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);
    if (mbedtls_ssl_conf_own_cert(&c->conf, &c->cert, &c->key) != 0) goto fail;
    LOGI(MOD, "TLS server context ready (cert %s)", cert_file);
    return c;
fail:
    ms_tls_ctx_free(c);
    return NULL;
}

void ms_tls_ctx_free(ms_tls_ctx *c)
{
    if (!c) return;
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_x509_crt_free(&c->cert);
    mbedtls_pk_free(&c->key);
    mbedtls_ctr_drbg_free(&c->drbg);
    mbedtls_entropy_free(&c->entropy);
    free(c);
}

ms_tls_conn *ms_tls_accept(ms_tls_ctx *ctx, int fd)
{
    ms_tls_conn *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    mbedtls_ssl_init(&c->ssl);
    mbedtls_net_init(&c->net);
    c->net.fd = fd;
    if (mbedtls_ssl_setup(&c->ssl, &ctx->conf) != 0) goto fail;
    mbedtls_ssl_set_bio(&c->ssl, &c->net, mbedtls_net_send, mbedtls_net_recv, NULL);
    /* M1: bound the handshake. A client that connects and never speaks used
     * to park this thread in recv() inside mbedtls_ssl_handshake() forever.
     * On a blocking fd with SO_RCVTIMEO, a timed-out read surfaces as
     * MBEDTLS_ERR_NET_RECV_FAILED (not WANT_READ), so the loop below exits.
     * Callers (rtsp/httpd accept paths) set the same-magnitude timeout via
     * net_set_timeouts() already; this keeps the guarantee local to tls.c. */
    {
        struct timeval tv = { 30, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }
    int r;
    while ((r = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
            LOGD(MOD, "handshake failed (-0x%x)", -r);
            goto fail;
        }
    }
    return c;
fail:
    mbedtls_ssl_free(&c->ssl);
    free(c);
    return NULL;
}

int ms_tls_read(ms_tls_conn *c, void *buf, int len)
{
    int r = mbedtls_ssl_read(&c->ssl, (unsigned char *)buf, (size_t)len);
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
        return 0;                          /* no data yet */
    if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return -1;
    return r;                              /* >0 bytes, <0 error/closed */
}

int ms_tls_write(ms_tls_conn *c, const void *buf, int len)
{
    int off = 0;
    while (off < len) {
        int r = mbedtls_ssl_write(&c->ssl, (const unsigned char *)buf + off,
                                  (size_t)(len - off));
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (r <= 0) return -1;
        off += r;
    }
    return off;
}

int ms_tls_pending(ms_tls_conn *c)
{
    /* bytes already decrypted into the TLS layer's buffer: invisible to
     * poll() on the raw fd, so poll-based loops must check this first (L7) */
    return (int)mbedtls_ssl_get_bytes_avail(&c->ssl);
}

void ms_tls_close(ms_tls_conn *c)
{
    if (!c) return;
    mbedtls_ssl_close_notify(&c->ssl);
    mbedtls_ssl_free(&c->ssl);
    free(c);
}

#endif /* USE_TLS */
