/*
 * http.c — HTTP/1.1 server for fMP4/MJPEG/snapshot streaming.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <imp/imp_framesource.h>

#include "http.h"
#include "config.h"
#include "stream.h"
#include "encoder.h"
#include "fmp4.h"
#include "auth.h"
#include "log.h"

/* -------------------------------------------------------------------------
 * Types
 * -----------------------------------------------------------------------*/

typedef enum {
    REQ_UNKNOWN,
    REQ_ROOT,
    REQ_SNAPSHOT,
    REQ_MJPEG,
    REQ_STREAM0,
    REQ_STREAM1,
} ReqType;

typedef struct {
    int     fd;
    char    peer[64];
} HttpClient;

static int      g_listen_fd = -1;
static int      g_running   = 0;
static pthread_t g_listen_thread;

/* -------------------------------------------------------------------------
 * Helpers
 * -----------------------------------------------------------------------*/

static int send_all(int fd, const uint8_t *buf, int len)
{
    int sent = 0;
    while (sent < len) {
        int n = (int)write(fd, buf + sent, len - sent);
        if (n <= 0) return -1;
        sent += n;
    }
    return sent;
}

static int sendstr(int fd, const char *s)
{
    return send_all(fd, (const uint8_t *)s, (int)strlen(s));
}

/* -------------------------------------------------------------------------
 * Auth
 * -----------------------------------------------------------------------*/

static int check_basic_auth(const char *request)
{
    char auth[256] = "";
    const char *p = strcasestr(request, "\r\nAuthorization:");
    if (!p) return 0;
    p = strchr(p, ':') + 1;
    while (*p == ' ') p++;
    const char *end = strstr(p, "\r\n");
    if (!end) end = p + strlen(p);
    int len = (int)(end - p);
    if (len >= (int)sizeof(auth)) len = (int)sizeof(auth) - 1;
    memcpy(auth, p, len);
    auth[len] = '\0';
    return http_basic_check(auth, g_cfg.http_user, g_cfg.http_pass);
}

static void send_401(int fd)
{
    char challenge[128], resp[256];
    http_basic_challenge(challenge, sizeof(challenge), "timps");
    snprintf(resp, sizeof(resp),
             "HTTP/1.1 401 Unauthorized\r\n"
             "WWW-Authenticate: %s\r\n"
             "Content-Length: 0\r\n"
             "Connection: close\r\n"
             "\r\n",
             challenge);
    sendstr(fd, resp);
}

/* -------------------------------------------------------------------------
 * JPEG snapshot via FrameSource direct access
 * -----------------------------------------------------------------------*/

typedef struct {
    uint8_t *data;
    int      len;
    int      done;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
} SnapshotCtx;

static void snapshot_sink_cb(const EncoderFrame *f, void *ud)
{
    SnapshotCtx *ctx = (SnapshotCtx *)ud;
    pthread_mutex_lock(&ctx->lock);
    if (!ctx->done && f->key) {
        ctx->data = malloc(f->len);
        if (ctx->data) {
            memcpy(ctx->data, f->data, f->len);
            ctx->len  = f->len;
        }
        ctx->done = 1;
        pthread_cond_signal(&ctx->cond);
    }
    pthread_mutex_unlock(&ctx->lock);
}

static void serve_snapshot(int fd, int stream_idx)
{
    SnapshotCtx ctx;
    struct timespec ts;

    memset(&ctx, 0, sizeof(ctx));
    pthread_mutex_init(&ctx.lock, &(pthread_mutexattr_t){0});
    pthread_cond_init(&ctx.cond, NULL);

    EncoderSink sink = { snapshot_sink_cb, &ctx };
    stream_acquire(stream_idx);
    stream_add_video_sink(stream_idx, &sink);
    encoder_request_idr(stream_idx);

    /* Wait up to 3 seconds for a key frame */
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 3;
    pthread_mutex_lock(&ctx.lock);
    while (!ctx.done)
        if (pthread_cond_timedwait(&ctx.cond, &ctx.lock, &ts)) break;
    pthread_mutex_unlock(&ctx.lock);

    stream_remove_video_sink(stream_idx, &sink);
    stream_release(stream_idx);

    if (ctx.data) {
        /* The encoder produces Annex-B H.264/H.265 frames, not JPEG.
         * A true JPEG snapshot would require a separate JPEG encoder channel.
         * Here we send the raw I-frame as octet-stream with a note header. */
        char hdr[256];
        snprintf(hdr, sizeof(hdr),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: application/octet-stream\r\n"
                 "Content-Disposition: attachment; filename=snapshot.h264\r\n"
                 "Content-Length: %d\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 ctx.len);
        sendstr(fd, hdr);
        send_all(fd, ctx.data, ctx.len);
        free(ctx.data);
    } else {
        sendstr(fd,
                "HTTP/1.1 503 Service Unavailable\r\n"
                "Content-Length: 0\r\nConnection: close\r\n\r\n");
    }

    pthread_cond_destroy(&ctx.cond);
    pthread_mutex_destroy(&ctx.lock);
}

/* -------------------------------------------------------------------------
 * MJPEG stream
 * -----------------------------------------------------------------------*/

typedef struct {
    int fd;
    int running;
    pthread_mutex_t lock;
} MjpegCtx;

static void mjpeg_sink_cb(const EncoderFrame *f, void *ud)
{
    MjpegCtx *ctx = (MjpegCtx *)ud;
    if (!ctx->running || !f->key) return;

    /* We use raw H.264 I-frames wrapped in MJPEG boundary.
     * (Same caveat as snapshot: a real JPEG encoder channel is needed
     * for true MJPEG; this streams I-frames for demonstration.) */
    char boundary[256];
    snprintf(boundary, sizeof(boundary),
             "--frame\r\n"
             "Content-Type: application/octet-stream\r\n"
             "Content-Length: %d\r\n"
             "\r\n",
             f->len);

    pthread_mutex_lock(&ctx->lock);
    if (sendstr(ctx->fd, boundary) < 0 ||
        send_all(ctx->fd, f->data, f->len) < 0 ||
        sendstr(ctx->fd, "\r\n") < 0)
        ctx->running = 0;
    pthread_mutex_unlock(&ctx->lock);
}

static void serve_mjpeg(int fd, int stream_idx)
{
    sendstr(fd,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: multipart/x-mixed-replace;boundary=frame\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n"
            "\r\n");

    MjpegCtx ctx;
    ctx.fd      = fd;
    ctx.running = 1;
    pthread_mutex_init(&ctx.lock, NULL);

    EncoderSink sink = { mjpeg_sink_cb, &ctx };
    stream_acquire(stream_idx);
    stream_add_video_sink(stream_idx, &sink);
    encoder_request_idr(stream_idx);

    /* Block until client disconnects */
    while (ctx.running) {
        char tmp[64];
        int n = (int)read(fd, tmp, sizeof(tmp));
        if (n <= 0) break;
    }

    stream_remove_video_sink(stream_idx, &sink);
    stream_release(stream_idx);
    pthread_mutex_destroy(&ctx.lock);
}

/* -------------------------------------------------------------------------
 * Fragmented-MP4 stream
 * -----------------------------------------------------------------------*/

typedef struct {
    int fd;
    int running;
    pthread_mutex_t lock;
    int init_sent;
    Fmp4Ctx *fmp4;
} Fmp4StreamCtx;

static int fmp4_write_cb(const uint8_t *data, int len, void *ud)
{
    Fmp4StreamCtx *ctx = (Fmp4StreamCtx *)ud;
    int r = send_all(ctx->fd, data, len);
    if (r < 0) ctx->running = 0;
    return r;
}

static void fmp4_enc_cb(const EncoderFrame *f, void *ud)
{
    Fmp4StreamCtx *ctx = (Fmp4StreamCtx *)ud;
    if (!ctx->running) return;

    pthread_mutex_lock(&ctx->lock);
    if (!ctx->running) { pthread_mutex_unlock(&ctx->lock); return; }

    if (!ctx->init_sent) {
        if (f->key) {
            /* Write moov init segment from this IDR frame */
            if (fmp4_write_init(ctx->fmp4, f->data, f->len) == 0)
                ctx->init_sent = 1;
            else
                ctx->running = 0;
        }
        pthread_mutex_unlock(&ctx->lock);
        return;
    }

    if (fmp4_write_video_frame(ctx->fmp4, f->data, f->len,
                               f->key, f->pts) < 0)
        ctx->running = 0;

    pthread_mutex_unlock(&ctx->lock);
}

static void serve_fmp4(int fd, int stream_idx)
{
    StreamCfg *s = &g_cfg.stream[stream_idx];

    sendstr(fd,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: video/mp4\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n");

    Fmp4StreamCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.fd      = fd;
    ctx.running = 1;
    pthread_mutex_init(&ctx.lock, NULL);
    ctx.fmp4 = fmp4_create(fmp4_write_cb, &ctx,
                            (s->codec == CODEC_H265),
                            s->width, s->height, s->fps);
    if (!ctx.fmp4) {
        sendstr(fd, "HTTP/1.1 500 Internal Server Error\r\n\r\n");
        return;
    }

    EncoderSink sink = { fmp4_enc_cb, &ctx };
    stream_acquire(stream_idx);
    stream_add_video_sink(stream_idx, &sink);
    encoder_request_idr(stream_idx);

    /* Block until client disconnects or error */
    while (ctx.running) {
        char tmp[64];
        int n = (int)read(fd, tmp, sizeof(tmp));
        if (n <= 0) break;
    }

    ctx.running = 0;
    stream_remove_video_sink(stream_idx, &sink);
    stream_release(stream_idx);
    fmp4_destroy(ctx.fmp4);
    pthread_mutex_destroy(&ctx.lock);
}

/* -------------------------------------------------------------------------
 * Root info page
 * -----------------------------------------------------------------------*/

static void serve_root(int fd)
{
    char body[1024];
    snprintf(body, sizeof(body),
             "<!DOCTYPE html><html><head><title>timps</title></head><body>\n"
             "<h1>timps — Tiny IMP Streamer</h1>\n"
             "<ul>\n"
             "<li><a href='/stream'>Video stream 0 (fMP4/MSE)</a></li>\n"
             "<li><a href='/stream1'>Video stream 1 (fMP4/MSE)</a></li>\n"
             "<li><a href='/snapshot.jpg'>Snapshot</a></li>\n"
             "<li><a href='/mjpeg'>MJPEG</a></li>\n"
             "</ul>\n"
             "<p>RTSP: rtsp://&lt;host&gt;:%d/stream0 | /stream1</p>\n"
             "</body></html>\n",
             g_cfg.rtsp_port);

    char hdr[256];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: text/html\r\n"
             "Content-Length: %d\r\n"
             "Connection: close\r\n"
             "\r\n",
             (int)strlen(body));
    sendstr(fd, hdr);
    sendstr(fd, body);
}

/* -------------------------------------------------------------------------
 * Client thread
 * -----------------------------------------------------------------------*/

static void *http_client_thread(void *arg)
{
    HttpClient *c = (HttpClient *)arg;
    char buf[4096];
    int  buflen = 0;

    log_debug("HTTP CONNECT [%s]", c->peer);

    /* Read HTTP request (stop after blank line) */
    while (buflen < (int)sizeof(buf) - 1) {
        int n = (int)read(c->fd, buf + buflen, sizeof(buf) - buflen - 1);
        if (n <= 0) goto done;
        buflen += n;
        buf[buflen] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }

    /* Auth check */
    if (g_cfg.http_auth && !check_basic_auth(buf)) {
        send_401(c->fd);
        goto done;
    }

    /* Parse method and path */
    char method[16]="", path[256]="";
    sscanf(buf, "%15s %255s", method, path);

    if (strcmp(method, "GET")) {
        sendstr(c->fd,
                "HTTP/1.1 405 Method Not Allowed\r\n"
                "Allow: GET\r\nContent-Length: 0\r\n\r\n");
        goto done;
    }

    /* Route */
    if (!strcmp(path, "/") || !strcmp(path, "/index.html"))
        serve_root(c->fd);
    else if (!strcmp(path, "/snapshot.jpg") || !strcmp(path, "/snapshot"))
        serve_snapshot(c->fd, 0);
    else if (!strcmp(path, "/mjpeg") || !strcmp(path, "/video.mjpg"))
        serve_mjpeg(c->fd, 0);
    else if (!strcmp(path, "/stream") || !strcmp(path, "/stream0") ||
             !strcmp(path, "/video")  || !strcmp(path, "/stream.mp4"))
        serve_fmp4(c->fd, 0);
    else if (!strcmp(path, "/stream1") || !strcmp(path, "/stream1.mp4"))
        serve_fmp4(c->fd, 1);
    else {
        sendstr(c->fd,
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Length: 0\r\nConnection: close\r\n\r\n");
    }

done:
    close(c->fd);
    free(c);
    return NULL;
}

/* -------------------------------------------------------------------------
 * Listen thread
 * -----------------------------------------------------------------------*/

static void *http_listen_thread(void *arg)
{
    (void)arg;
    log_info("HTTP server listening on port %d", g_cfg.http_port);

    while (g_running) {
        struct sockaddr_in peer_addr;
        socklen_t addrlen = sizeof(peer_addr);

        int fd = accept(g_listen_fd,
                        (struct sockaddr *)&peer_addr, &addrlen);
        if (fd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            if (!g_running) break;
            log_warn("HTTP accept: %s", strerror(errno));
            continue;
        }

        int val = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));

        HttpClient *c = calloc(1, sizeof(*c));
        if (!c) { close(fd); continue; }
        c->fd = fd;
        snprintf(c->peer, sizeof(c->peer), "%s:%d",
                 inet_ntoa(peer_addr.sin_addr),
                 ntohs(peer_addr.sin_port));

        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &attr, http_client_thread, c)) {
            log_warn("HTTP: client thread create failed");
            close(fd);
            free(c);
        }
        pthread_attr_destroy(&attr);
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Public API
 * -----------------------------------------------------------------------*/

int http_init(void)
{
    struct sockaddr_in addr;
    int val = 1;

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        log_error("HTTP socket: %s", strerror(errno));
        return -1;
    }

    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(g_cfg.http_port);

    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_error("HTTP bind: %s", strerror(errno));
        close(g_listen_fd);
        return -1;
    }

    if (listen(g_listen_fd, 8) < 0) {
        log_error("HTTP listen: %s", strerror(errno));
        close(g_listen_fd);
        return -1;
    }

    g_running = 1;
    if (pthread_create(&g_listen_thread, NULL, http_listen_thread, NULL)) {
        log_error("HTTP listen thread: %s", strerror(errno));
        close(g_listen_fd);
        return -1;
    }

    return 0;
}

void http_exit(void)
{
    g_running = 0;
    if (g_listen_fd >= 0) {
        shutdown(g_listen_fd, SHUT_RDWR);
        close(g_listen_fd);
        g_listen_fd = -1;
    }
    pthread_join(g_listen_thread, NULL);
}
