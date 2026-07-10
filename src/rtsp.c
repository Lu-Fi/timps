/*
 * rtsp.c — RTSP/1.0 server implementation.
 *
 * Each client connection is handled in its own pthread.
 * Video and audio frames are forwarded to the client via encoder/audio sinks.
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
#include <time.h>

#include "rtsp.h"
#include "config.h"
#include "stream.h"
#include "encoder.h"
#include "audio.h"
#include "rtp.h"
#include "auth.h"
#include "log.h"

/* -------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

#define RTSP_BUF_SIZE   4096
#define MAX_CLIENTS     16

/* -------------------------------------------------------------------------
 * Per-client state
 * -----------------------------------------------------------------------*/

typedef enum {
    TRANSPORT_TCP,
    TRANSPORT_UDP,
} TransportType;

typedef struct {
    int            fd;
    char           peer[64];

    /* Parsed from SETUP */
    int            stream_idx;    /* 0 or 1 */
    int            has_audio;

    /* TCP interleaved channels */
    int            video_channel; /* RTP  channel */
    int            rtcp_channel;  /* RTCP channel */
    int            audio_channel;
    int            audio_rtcp_channel;

    /* UDP */
    TransportType  transport;
    int            udp_video_fd;
    int            udp_audio_fd;
    struct sockaddr_in udp_video_peer;
    struct sockaddr_in udp_audio_peer;

    /* RTP sessions */
    RtpSession     video_rtp;
    RtpSession     audio_rtp;

    /* Auth */
    char           nonce[33];

    /* Encoder/audio sinks registered for this client */
    EncoderSink    enc_sink;
    AudioSink      aud_sink;
    int            playing;

    pthread_mutex_t send_lock;
} RtspClient;

/* -------------------------------------------------------------------------
 * Server globals
 * -----------------------------------------------------------------------*/

static int  g_listen_fd  = -1;
static int  g_running    = 0;
static pthread_t g_listen_thread;

/* -------------------------------------------------------------------------
 * Helper: send all bytes
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

/* -------------------------------------------------------------------------
 * RTP write callback — interleaved TCP (§10.12)
 * -----------------------------------------------------------------------*/
static int rtp_tcp_write(int channel, const uint8_t *data, int len,
                         void *userdata)
{
    RtspClient *c = (RtspClient *)userdata;
    uint8_t    hdr[4];

    hdr[0] = '$';
    hdr[1] = (uint8_t)channel;
    hdr[2] = (uint8_t)(len >> 8);
    hdr[3] = (uint8_t)(len & 0xff);

    pthread_mutex_lock(&c->send_lock);
    int r = send_all(c->fd, hdr, 4) < 0 ? -1 :
            send_all(c->fd, data, len);
    pthread_mutex_unlock(&c->send_lock);
    return r;
}

/* -------------------------------------------------------------------------
 * RTP write callback — UDP
 * -----------------------------------------------------------------------*/
static int rtp_udp_write(int channel, const uint8_t *data, int len,
                         void *userdata)
{
    RtspClient *c = (RtspClient *)userdata;
    int fd;
    struct sockaddr_in *peer;

    if (channel == c->video_channel) {
        fd   = c->udp_video_fd;
        peer = &c->udp_video_peer;
    } else if (channel == c->audio_channel) {
        fd   = c->udp_audio_fd;
        peer = &c->udp_audio_peer;
    } else {
        return -1;
    }

    return (int)sendto(fd, data, len, 0,
                       (struct sockaddr *)peer, sizeof(*peer));
}

/* -------------------------------------------------------------------------
 * Video frame sink callback
 * -----------------------------------------------------------------------*/
static void video_sink_cb(const EncoderFrame *f, void *ud)
{
    RtspClient *c = (RtspClient *)ud;
    if (!c->playing) return;

    rtp_write_fn wfn = (c->transport == TRANSPORT_TCP)
                     ? rtp_tcp_write : rtp_udp_write;

    if (g_cfg.stream[c->stream_idx].codec == CODEC_H265)
        rtp_send_h265(&c->video_rtp, f->data, f->len,
                      (uint32_t)(f->pts & 0xffffffff), wfn,
                      c->video_channel, c);
    else
        rtp_send_h264(&c->video_rtp, f->data, f->len,
                      (uint32_t)(f->pts & 0xffffffff), wfn,
                      c->video_channel, c);
}

/* -------------------------------------------------------------------------
 * Audio frame sink callback
 * -----------------------------------------------------------------------*/
static void audio_sink_cb(const AudioFrame *f, void *ud)
{
    RtspClient *c = (RtspClient *)ud;
    if (!c->playing || !c->has_audio) return;

    rtp_write_fn wfn = (c->transport == TRANSPORT_TCP)
                     ? rtp_tcp_write : rtp_udp_write;

    switch (g_cfg.audio_codec) {
    case AUDIO_G711A:
    case AUDIO_G711U:
        rtp_send_g711(&c->audio_rtp, f->data, f->len,
                      (uint32_t)(f->pts & 0xffffffff), wfn,
                      c->audio_channel, c);
        break;
    case AUDIO_AAC:
        rtp_send_aac(&c->audio_rtp, f->data, f->len,
                     (uint32_t)(f->pts & 0xffffffff), wfn,
                     c->audio_channel, c);
        break;
    default:
        break;
    }
}

/* -------------------------------------------------------------------------
 * RTSP response helpers
 * -----------------------------------------------------------------------*/

static void rtsp_send(RtspClient *c, const char *msg)
{
    pthread_mutex_lock(&c->send_lock);
    send_all(c->fd, (const uint8_t *)msg, (int)strlen(msg));
    pthread_mutex_unlock(&c->send_lock);
}

static void rtsp_401(RtspClient *c, int cseq)
{
    char challenge[256], resp[512];
    rtsp_digest_nonce(c->nonce);
    rtsp_digest_challenge(challenge, sizeof(challenge), "timps", c->nonce);
    snprintf(resp, sizeof(resp),
             "RTSP/1.0 401 Unauthorized\r\n"
             "CSeq: %d\r\n"
             "WWW-Authenticate: %s\r\n"
             "\r\n",
             cseq, challenge);
    rtsp_send(c, resp);
}

/* -------------------------------------------------------------------------
 * SDP generation
 * -----------------------------------------------------------------------*/
static int build_sdp(int stream_idx, char *out, int outlen)
{
    StreamCfg *s    = &g_cfg.stream[stream_idx];
    const char *vpt = (s->codec == CODEC_H265) ? "H265" : "H264";
    int         vid_pt = 96;
    int         aud_pt = audio_rtp_pt();
    int         sr     = audio_sample_rate();
    char        audio_line[256] = "";

    if (g_cfg.audio_codec != AUDIO_NONE && aud_pt >= 0) {
        if (g_cfg.audio_codec == AUDIO_G711A || g_cfg.audio_codec == AUDIO_G711U) {
            const char *enc = (g_cfg.audio_codec == AUDIO_G711A) ? "PCMA" : "PCMU";
            snprintf(audio_line, sizeof(audio_line),
                     "m=audio 0 RTP/AVP %d\r\n"
                     "a=rtpmap:%d %s/%d\r\n"
                     "a=control:trackID=1\r\n",
                     aud_pt, aud_pt, enc, sr);
        } else {
            snprintf(audio_line, sizeof(audio_line),
                     "m=audio 0 RTP/AVP 97\r\n"
                     "a=rtpmap:97 mpeg4-generic/%d/1\r\n"
                     "a=fmtp:97 streamtype=5;profile-level-id=1;"
                     "mode=AAC-hbr;sizelength=13;indexlength=3;"
                     "indexdeltalength=3\r\n"
                     "a=control:trackID=1\r\n", sr);
        }
    }

    return snprintf(out, outlen,
        "v=0\r\n"
        "o=- 0 0 IN IP4 0.0.0.0\r\n"
        "s=timps stream%d\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "t=0 0\r\n"
        "a=control:*\r\n"
        "m=video 0 RTP/AVP %d\r\n"
        "a=rtpmap:%d %s/90000\r\n"
        "a=framerate:%d\r\n"
        "a=control:trackID=0\r\n"
        "%s",
        stream_idx, vid_pt, vid_pt, vpt, s->fps, audio_line);
}

/* -------------------------------------------------------------------------
 * Parse an RTSP message from a client
 * -----------------------------------------------------------------------*/

/* Extract header value (returns pointer into msg, not null-terminated) */
static const char *get_header(const char *msg, const char *hdr,
                               char *val, int valsz)
{
    char search[128];
    snprintf(search, sizeof(search), "\r\n%s:", hdr);
    const char *p = strcasestr(msg, search);
    if (!p) {
        /* Also try at start of message */
        if (strncasecmp(msg, hdr, strlen(hdr)) == 0 && msg[strlen(hdr)] == ':')
            p = msg - 2; /* will be adjusted below */
        else return NULL;
    }
    p = strchr(p, ':') + 1;
    while (*p == ' ') p++;
    const char *end = strstr(p, "\r\n");
    if (!end) end = p + strlen(p);
    int len = (int)(end - p);
    if (len >= valsz) len = valsz - 1;
    memcpy(val, p, len);
    val[len] = '\0';
    return val;
}

static int get_cseq(const char *msg)
{
    char val[32] = "0";
    get_header(msg, "CSeq", val, sizeof(val));
    return atoi(val);
}

/* -------------------------------------------------------------------------
 * Check Digest auth in a message.  Returns 1 if OK, 0 if missing/bad.
 * -----------------------------------------------------------------------*/
static int check_auth(RtspClient *c, const char *msg, const char *method)
{
    char authval[512] = "";
    if (!get_header(msg, "Authorization", authval, sizeof(authval))) return 0;

    /* Parse Digest fields */
    char username[64]="", uri[256]="", nonce[64]="", response[64]="";

    /* Simple token parser */
    char *p = authval;
    while (*p) {
        char key[32]="", val[256]="";
        while (*p == ' ' || *p == ',') p++;
        char *eq = strchr(p, '=');
        if (!eq) break;
        int kl = (int)(eq - p);
        if (kl >= (int)sizeof(key)) break;
        memcpy(key, p, kl);
        key[kl] = '\0';
        p = eq + 1;
        if (*p == '"') {
            p++;
            char *close = strchr(p, '"');
            if (!close) break;
            int vl = (int)(close - p);
            if (vl >= (int)sizeof(val)) vl = sizeof(val)-1;
            memcpy(val, p, vl); val[vl] = '\0';
            p = close + 1;
        } else {
            char *end = p;
            while (*end && *end != ',' && *end != ' ') end++;
            int vl = (int)(end - p);
            if (vl >= (int)sizeof(val)) vl = sizeof(val)-1;
            memcpy(val, p, vl); val[vl] = '\0';
            p = end;
        }
        if      (!strcasecmp(key, "username")) snprintf(username, sizeof(username), "%s", val);
        else if (!strcasecmp(key, "uri"))      snprintf(uri,      sizeof(uri),      "%s", val);
        else if (!strcasecmp(key, "nonce"))    snprintf(nonce,    sizeof(nonce),    "%s", val);
        else if (!strcasecmp(key, "response")) snprintf(response, sizeof(response), "%s", val);
    }

    if (!strcmp(username, g_cfg.rtsp_user) &&
        rtsp_digest_verify(method, uri, c->nonce, response,
                           g_cfg.rtsp_user, g_cfg.rtsp_pass))
        return 1;

    return 0;
}

/* -------------------------------------------------------------------------
 * Handle one RTSP request
 * -----------------------------------------------------------------------*/
static void handle_request(RtspClient *c, char *msg)
{
    char method[16]="", uri[256]="", version[16]="";
    int  cseq;

    sscanf(msg, "%15s %255s %15s", method, uri, version);
    cseq = get_cseq(msg);

    log_debug("RTSP [%s] %s %s (CSeq %d)", c->peer, method, uri, cseq);

    /* ---- OPTIONS ---- */
    if (!strcmp(method, "OPTIONS")) {
        char resp[256];
        snprintf(resp, sizeof(resp),
                 "RTSP/1.0 200 OK\r\n"
                 "CSeq: %d\r\n"
                 "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN\r\n"
                 "\r\n", cseq);
        rtsp_send(c, resp);
        return;
    }

    /* ---- Auth check (except OPTIONS) ---- */
    if (g_cfg.rtsp_auth && strcmp(method, "OPTIONS")) {
        if (!check_auth(c, msg, method)) {
            rtsp_401(c, cseq);
            return;
        }
    }

    /* ---- Determine stream index from URI ---- */
    c->stream_idx = 0;
    if (strstr(uri, "stream1")) c->stream_idx = 1;

    /* ---- DESCRIBE ---- */
    if (!strcmp(method, "DESCRIBE")) {
        char sdp[1024];
        int  sdplen = build_sdp(c->stream_idx, sdp, sizeof(sdp));
        char resp[2048];
        snprintf(resp, sizeof(resp),
                 "RTSP/1.0 200 OK\r\n"
                 "CSeq: %d\r\n"
                 "Content-Base: %s/\r\n"
                 "Content-Type: application/sdp\r\n"
                 "Content-Length: %d\r\n"
                 "\r\n"
                 "%s",
                 cseq, uri, sdplen, sdp);
        rtsp_send(c, resp);
        return;
    }

    /* ---- SETUP ---- */
    if (!strcmp(method, "SETUP")) {
        /* Determine if this is the video or audio track */
        int is_audio = (strstr(uri, "trackID=1") != NULL);
        char transport_hdr[256] = "";
        get_header(msg, "Transport", transport_hdr, sizeof(transport_hdr));

        if (strstr(transport_hdr, "RTP/AVP/TCP") ||
            strstr(transport_hdr, "interleaved")) {
            /* TCP interleaved */
            c->transport = TRANSPORT_TCP;
            int ch_base = is_audio ? 2 : 0;
            char resp[512];
            snprintf(resp, sizeof(resp),
                     "RTSP/1.0 200 OK\r\n"
                     "CSeq: %d\r\n"
                     "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\n"
                     "Session: %08x\r\n"
                     "\r\n",
                     cseq, ch_base, ch_base + 1,
                     (unsigned)((uintptr_t)c & 0xffffffff));
            if (is_audio) {
                c->audio_channel      = ch_base;
                c->audio_rtcp_channel = ch_base + 1;
                c->has_audio = 1;
            } else {
                c->video_channel = ch_base;
                c->rtcp_channel  = ch_base + 1;
            }
            rtsp_send(c, resp);
        } else {
            /* UDP unicast */
            c->transport = TRANSPORT_UDP;
            /* Parse client_port= from Transport header */
            int cport_rtp = 0, cport_rtcp = 0;
            char *cp = strstr(transport_hdr, "client_port=");
            if (cp) sscanf(cp + 12, "%d-%d", &cport_rtp, &cport_rtcp);

            /* Create a UDP send socket */
            /* c->peer may have port appended; strip it */
            char peer_ip[64] = "";
            snprintf(peer_ip, sizeof(peer_ip), "%s", c->peer);
            char *colon = strrchr(peer_ip, ':');
            if (colon) *colon = '\0';

            int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
            struct sockaddr_in peer_addr;
            memset(&peer_addr, 0, sizeof(peer_addr));
            peer_addr.sin_family      = AF_INET;
            peer_addr.sin_port        = htons(cport_rtp);
            inet_aton(peer_ip, &peer_addr.sin_addr);

            /* Bind to any local port */
            struct sockaddr_in local = {0};
            local.sin_family = AF_INET;
            bind(udp_fd, (struct sockaddr *)&local, sizeof(local));

            socklen_t llen = sizeof(local);
            getsockname(udp_fd, (struct sockaddr *)&local, &llen);
            int sport = ntohs(local.sin_port);

            if (is_audio) {
                c->has_audio       = 1;
                c->udp_audio_fd    = udp_fd;
                c->udp_audio_peer  = peer_addr;
                c->audio_channel   = 2; /* logical channel id for cb */
            } else {
                c->udp_video_fd    = udp_fd;
                c->udp_video_peer  = peer_addr;
                c->video_channel   = 0;
            }

            char resp[512];
            snprintf(resp, sizeof(resp),
                     "RTSP/1.0 200 OK\r\n"
                     "CSeq: %d\r\n"
                     "Transport: RTP/AVP/UDP;unicast;client_port=%d-%d;"
                     "server_port=%d-%d\r\n"
                     "Session: %08x\r\n"
                     "\r\n",
                     cseq, cport_rtp, cport_rtcp,
                     sport, sport + 1,
                     (unsigned)((uintptr_t)c & 0xffffffff));
            rtsp_send(c, resp);
        }
        return;
    }

    /* ---- PLAY ---- */
    if (!strcmp(method, "PLAY")) {
        char resp[256];
        snprintf(resp, sizeof(resp),
                 "RTSP/1.0 200 OK\r\n"
                 "CSeq: %d\r\n"
                 "Session: %08x\r\n"
                 "Range: npt=0.000-\r\n"
                 "\r\n",
                 cseq, (unsigned)((uintptr_t)c & 0xffffffff));
        rtsp_send(c, resp);

        if (!c->playing) {
            /* Register sinks and start encoder */
            stream_acquire(c->stream_idx);

            VideoCodec vc = g_cfg.stream[c->stream_idx].codec;
            rtp_session_init(&c->video_rtp,
                             96,      /* PT */
                             90000);

            c->enc_sink.cb       = video_sink_cb;
            c->enc_sink.userdata = c;
            stream_add_video_sink(c->stream_idx, &c->enc_sink);
            encoder_request_idr(c->stream_idx);
            (void)vc;

            if (c->has_audio && g_cfg.audio_codec != AUDIO_NONE) {
                int apt = audio_rtp_pt();
                rtp_session_init(&c->audio_rtp, apt,
                                 (uint32_t)audio_sample_rate());
                stream_audio_acquire();
                c->aud_sink.cb       = audio_sink_cb;
                c->aud_sink.userdata = c;
                stream_add_audio_sink(&c->aud_sink);
            }

            c->playing = 1;
            log_info("RTSP PLAY  [%s] stream%d", c->peer, c->stream_idx);
        }
        return;
    }

    /* ---- PAUSE ---- */
    if (!strcmp(method, "PAUSE")) {
        c->playing = 0;
        char resp[256];
        snprintf(resp, sizeof(resp),
                 "RTSP/1.0 200 OK\r\n"
                 "CSeq: %d\r\n"
                 "\r\n", cseq);
        rtsp_send(c, resp);
        return;
    }

    /* ---- TEARDOWN ---- */
    if (!strcmp(method, "TEARDOWN")) {
        char resp[256];
        snprintf(resp, sizeof(resp),
                 "RTSP/1.0 200 OK\r\n"
                 "CSeq: %d\r\n"
                 "\r\n", cseq);
        rtsp_send(c, resp);
        /* Signal the client thread to exit */
        c->playing = 0;
        return;
    }

    /* Unknown method */
    char resp[128];
    snprintf(resp, sizeof(resp),
             "RTSP/1.0 501 Not Implemented\r\nCSeq: %d\r\n\r\n", cseq);
    rtsp_send(c, resp);
}

/* -------------------------------------------------------------------------
 * Client thread
 * -----------------------------------------------------------------------*/
static void client_cleanup(RtspClient *c)
{
    if (c->playing) {
        stream_remove_video_sink(c->stream_idx, &c->enc_sink);
        stream_release(c->stream_idx);
        if (c->has_audio && g_cfg.audio_codec != AUDIO_NONE) {
            stream_remove_audio_sink(&c->aud_sink);
            stream_audio_release();
        }
        log_info("RTSP CLOSE [%s] stream%d", c->peer, c->stream_idx);
    }
    if (c->udp_video_fd > 0) close(c->udp_video_fd);
    if (c->udp_audio_fd > 0) close(c->udp_audio_fd);
    if (c->fd > 0)            close(c->fd);
    pthread_mutex_destroy(&c->send_lock);
    free(c);
}

static void *client_thread(void *arg)
{
    RtspClient *c = (RtspClient *)arg;
    char buf[RTSP_BUF_SIZE];
    int  buflen = 0;

    log_info("RTSP CONNECT [%s]", c->peer);

    while (1) {
        /* Read data into buf */
        int n = (int)read(c->fd, buf + buflen, sizeof(buf) - buflen - 1);
        if (n <= 0) break;
        buflen += n;
        buf[buflen] = '\0';

        /* Process all complete RTSP messages in the buffer */
        while (1) {
            /* Handle interleaved RTP data ($...) from client (RTCP) */
            if (buf[0] == '$' && buflen >= 4) {
                int pktlen = ((uint8_t)buf[2] << 8) | (uint8_t)buf[3];
                int total  = 4 + pktlen;
                if (buflen < total) break;
                /* Silently discard RTCP from client */
                memmove(buf, buf + total, buflen - total);
                buflen -= total;
                continue;
            }

            /* Find end of RTSP message (\r\n\r\n) */
            char *end = strstr(buf, "\r\n\r\n");
            if (!end) break;

            int msglen = (int)(end - buf) + 4;

            /* Check for body (Content-Length) */
            char clval[32] = "0";
            get_header(buf, "Content-Length", clval, sizeof(clval));
            int cl = atoi(clval);
            if (buflen < msglen + cl) break;

            /* Null-terminate for parsing */
            buf[msglen + cl] = '\0';

            handle_request(c, buf);

            /* Check if TEARDOWN or connection closed */
            if (!c->playing &&
                (strstr(buf, "TEARDOWN") || strstr(buf, "PLAY") == NULL)) {
                /* After TEARDOWN we still drain, client will close */
                if (strstr(buf, "TEARDOWN")) {
                    memmove(buf, buf + msglen + cl, buflen - msglen - cl);
                    buflen -= msglen + cl;
                    goto done;
                }
            }

            memmove(buf, buf + msglen + cl, buflen - msglen - cl);
            buflen -= msglen + cl;
        }
    }

done:
    client_cleanup(c);
    return NULL;
}

/* -------------------------------------------------------------------------
 * Listen thread
 * -----------------------------------------------------------------------*/
static void *listen_thread(void *arg)
{
    (void)arg;
    log_info("RTSP server listening on port %d", g_cfg.rtsp_port);

    while (g_running) {
        struct sockaddr_in peer_addr;
        socklen_t          addrlen = sizeof(peer_addr);

        int fd = accept(g_listen_fd, (struct sockaddr *)&peer_addr, &addrlen);
        if (fd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            if (!g_running) break;
            log_warn("RTSP accept: %s", strerror(errno));
            continue;
        }

        /* Enable TCP keepalive and no-delay */
        int val = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));
        setsockopt(fd, SOL_SOCKET,  SO_KEEPALIVE, &val, sizeof(val));

        RtspClient *c = calloc(1, sizeof(*c));
        if (!c) { close(fd); continue; }
        c->fd              = fd;
        c->video_channel   = 0;
        c->rtcp_channel    = 1;
        c->audio_channel   = 2;
        c->audio_rtcp_channel = 3;
        c->udp_video_fd    = -1;
        c->udp_audio_fd    = -1;
        pthread_mutex_init(&c->send_lock, NULL);
        snprintf(c->peer, sizeof(c->peer), "%s:%d",
                 inet_ntoa(peer_addr.sin_addr),
                 ntohs(peer_addr.sin_port));

        /* Generate initial nonce */
        rtsp_digest_nonce(c->nonce);

        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &attr, client_thread, c)) {
            log_warn("RTSP: client thread create failed");
            client_cleanup(c);
        }
        pthread_attr_destroy(&attr);
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Public API
 * -----------------------------------------------------------------------*/

int rtsp_init(void)
{
    struct sockaddr_in addr;
    int                val = 1;

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        log_error("RTSP socket: %s", strerror(errno));
        return -1;
    }

    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(g_cfg.rtsp_port);

    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_error("RTSP bind: %s", strerror(errno));
        close(g_listen_fd);
        return -1;
    }

    if (listen(g_listen_fd, 8) < 0) {
        log_error("RTSP listen: %s", strerror(errno));
        close(g_listen_fd);
        return -1;
    }

    g_running = 1;
    if (pthread_create(&g_listen_thread, NULL, listen_thread, NULL)) {
        log_error("RTSP listen thread: %s", strerror(errno));
        close(g_listen_fd);
        return -1;
    }

    return 0;
}

void rtsp_exit(void)
{
    g_running = 0;
    if (g_listen_fd >= 0) {
        shutdown(g_listen_fd, SHUT_RDWR);
        close(g_listen_fd);
        g_listen_fd = -1;
    }
    pthread_join(g_listen_thread, NULL);
}
