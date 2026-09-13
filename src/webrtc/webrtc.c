/* webrtc.c - WHEP signalling + the ICE-lite/DTLS session (see webrtc.h). */
#ifdef USE_WEBRTC
#include "webrtc.h"
#include "stun.h"
#include "dtls.h"
#include "../log.h"
#include "../util.h"
#include "../auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define MOD "WEBRTC"

/* One browser tab is one session, and nothing here streams yet, so the cap is
 * about bounding threads/sockets, not about serving an audience. */
#ifndef WEBRTC_MAX_SESSIONS
#define WEBRTC_MAX_SESSIONS 4
#endif
/* ICE + DTLS on a LAN complete in tens of milliseconds; anything still
 * unfinished this late is a peer that went away mid-negotiation. */
#define WEBRTC_SETUP_US (30LL * 1000000)
/* A connected browser sends STUN consent checks every few seconds
 * (RFC 7675), so silence this long means the tab is gone. */
#define WEBRTC_IDLE_US  (30LL * 1000000)

typedef struct {
    volatile int       used;
    volatile int       run;
    char               id[33];
    char               lufrag[9];
    char               lpwd[33];
    char               expect_user[80];   /* "<lufrag>:<their-ufrag>" */
    int                fd;
    int                port;
    struct sockaddr_in peer;
    int                have_peer;
    ms_dtls           *dtls;
    pthread_t          thr;
} wrtc_session;

static wrtc_session   g_sess[WEBRTC_MAX_SESSIONS];
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static ms_dtls_ctx    *g_dtls_ctx;
static int             g_port_cfg;

int webrtc_available(void) { return g_dtls_ctx != NULL; }

/* ---------------- SDP helpers (line-anchored, CRLF or LF) ---------------- */

static const char *sdp_line(const char *sdp, const char *pre)
{
    size_t n = strlen(pre);
    const char *p = sdp;
    while (p && *p) {
        if (!strncmp(p, pre, n)) return p + n;
        const char *e = strchr(p, '\n');
        p = e ? e + 1 : NULL;
    }
    return NULL;
}

/* copy up to the next space/CR/LF */
static int sdp_tok(const char *p, char *out, int cap)
{
    int i = 0;
    while (p && p[i] && p[i] != ' ' && p[i] != '\r' && p[i] != '\n' &&
           i < cap - 1) { out[i] = p[i]; i++; }
    out[i] = 0;
    return i;
}

/* copy the rest of the line */
static int sdp_rest(const char *p, char *out, int cap)
{
    int i = 0;
    while (p && p[i] && p[i] != '\r' && p[i] != '\n' && i < cap - 1) {
        out[i] = p[i]; i++;
    }
    out[i] = 0;
    return i;
}

static int sdp_count(const char *sdp, const char *pre)
{
    size_t n = strlen(pre);
    int c = 0;
    const char *p = sdp;
    while (p && *p) {
        if (!strncmp(p, pre, n)) c++;
        const char *e = strchr(p, '\n');
        p = e ? e + 1 : NULL;
    }
    return c;
}

/* ---------------- session ---------------- */

static int same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static void sess_release(wrtc_session *s)
{
    if (s->dtls) { ms_dtls_free(s->dtls); s->dtls = NULL; }
    if (s->fd >= 0) { close(s->fd); s->fd = -1; }
    s->have_peer = 0;
    __sync_synchronize();
    s->used = 0;
}

static void *sess_thread(void *arg)
{
    wrtc_session *s = (wrtc_session *)arg;
    uint8_t buf[2048];
    int64_t born = ms_now_us(), last_rx = born;
    int connected = 0;

    while (s->run) {
        struct pollfd p = { .fd = s->fd, .events = POLLIN, .revents = 0 };
        int pr = poll(&p, 1, (s->dtls && !connected) ? 20 : 250);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr > 0 && (p.revents & POLLIN)) {
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            int n = (int)recvfrom(s->fd, buf, sizeof buf, 0,
                                  (struct sockaddr *)&from, &fl);
            if (n > 0) {
                /* RFC 7983 demux on the first byte: 0-3 STUN, 20-63 DTLS,
                 * 128-191 RTP/RTCP (SRTP is a later milestone - those are
                 * dropped here rather than silently misrouted). */
                if (buf[0] <= 3) {
                    stun_req rq;
                    if (stun_parse_request(buf, n, s->lpwd, &rq) &&
                        !strcmp(rq.username, s->expect_user)) {
                        last_rx = ms_now_us();
                        uint8_t rsp[STUN_MAX_MSG];
                        int rn = stun_build_response(rsp, sizeof rsp, rq.txid,
                                                     &from, s->lpwd);
                        if (rn > 0)
                            sendto(s->fd, rsp, (size_t)rn, 0,
                                   (struct sockaddr *)&from, sizeof from);
                        /* Peer-reflexive learning (RFC 8445 7.3.1.3): the
                         * first authenticated source becomes the peer, and a
                         * nominated one replaces it as long as DTLS has not
                         * started - after that the transport is bound. */
                        if (!s->have_peer ||
                            (rq.use_candidate && !s->dtls &&
                             !same_addr(&from, &s->peer))) {
                            s->peer = from;
                            s->have_peer = 1;
                            char ip[INET_ADDRSTRLEN] = "?";
                            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
                            LOGI(MOD, "%s: ICE peer %s:%u%s", s->id, ip,
                                 ntohs(from.sin_port),
                                 rq.use_candidate ? " (nominated)" : "");
                        }
                    }
                } else if (buf[0] >= 20 && buf[0] <= 63) {
                    if (s->have_peer && same_addr(&from, &s->peer)) {
                        last_rx = ms_now_us();
                        if (!s->dtls)
                            s->dtls = ms_dtls_new(g_dtls_ctx, s->fd, &s->peer);
                        if (s->dtls && !connected)
                            ms_dtls_feed(s->dtls, buf, n);
                    }
                }
            }
        }
        if (s->dtls && !connected) {
            int r = ms_dtls_handshake(s->dtls);
            if (r == 0) {
                connected = 1;
                LOGI(MOD, "%s: DTLS connected (transport up; no media in this "
                          "build)", s->id);
            } else if (r < 0) {
                break;
            }
        }
        int64_t now = ms_now_us();
        if (!connected && now - born > WEBRTC_SETUP_US) {
            LOGW(MOD, "%s: no ICE/DTLS completion within %llds - closing",
                 s->id, (long long)(WEBRTC_SETUP_US / 1000000));
            break;
        }
        if (now - last_rx > WEBRTC_IDLE_US) {
            LOGI(MOD, "%s: idle, closing", s->id);
            break;
        }
    }
    sess_release(s);
    return NULL;
}

static int udp_bind(int port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int fl = fcntl(fd, F_GETFD, 0);
    if (fl >= 0) fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    return fd;
}

/* ---------------- WHEP ---------------- */

int webrtc_whep(const char *offer, const char *local_ip,
                char *ans, int anscap, char *sid, int sidcap)
{
    if (!g_dtls_ctx) return 404;
    if (!offer || strncmp(offer, "v=0", 3)) return 400;

    /* One bundled video m-section is all this milestone answers. Answering
     * fewer m-lines than the offer carries is not legal SDP, so say no
     * instead of emitting something the browser will reject obscurely. */
    if (sdp_count(offer, "m=") != 1 || !sdp_line(offer, "m=video ")) return 400;

    char rufrag[64] = "", mid[32] = "", proto[64] = "", pt[8] = "", rtpmap[64] = "";
    const char *p;
    if ((p = sdp_line(offer, "a=ice-ufrag:")) == NULL) return 400;
    sdp_tok(p, rufrag, sizeof rufrag);
    if (!rufrag[0]) return 400;
    if ((p = sdp_line(offer, "a=mid:")) != NULL) sdp_tok(p, mid, sizeof mid);
    if (!mid[0]) strcpy(mid, "0");

    /* m=video <port> <proto> <pt> ... - echo the proto and answer with the
     * offer's first payload type (and its rtpmap, if any) so the answer is
     * codec-agnostic; nothing is sent on it either way. */
    p = sdp_line(offer, "m=video ");
    while (*p && *p != ' ') p++;               /* past the port */
    while (*p == ' ') p++;
    p += sdp_tok(p, proto, sizeof proto);
    while (*p == ' ') p++;
    sdp_tok(p, pt, sizeof pt);
    if (!proto[0] || !pt[0]) return 400;
    char rtpmap_line[96] = "";
    {
        char want[24];
        snprintf(want, sizeof want, "a=rtpmap:%s ", pt);
        const char *r = sdp_line(offer, want);
        if (r && sdp_rest(r, rtpmap, sizeof rtpmap) > 0)
            snprintf(rtpmap_line, sizeof rtpmap_line, "a=rtpmap:%s %s\r\n",
                     pt, rtpmap);
    }

    pthread_mutex_lock(&g_mx);
    wrtc_session *s = NULL;
    for (int i = 0; i < WEBRTC_MAX_SESSIONS; i++)
        if (!g_sess[i].used) { s = &g_sess[i]; break; }
    if (!s) { pthread_mutex_unlock(&g_mx); return 503; }
    memset(s, 0, sizeof *s);
    s->used = 1;
    s->fd = -1;
    pthread_mutex_unlock(&g_mx);

    /* ICE credentials: auth_gen_token() is the /dev/urandom-backed generator
     * the control token already uses. 8 hex chars of ufrag and 32 of pwd sit
     * well inside RFC 8445's 4/22-character minimums.
     * Assembled in locals and copied in: building expect_user straight from
     * s->lufrag has source and destination inside the same object, which
     * -Wrestrict flags. */
    char tok[33], lufrag[9];
    auth_gen_token(s->id);
    auth_gen_token(tok);
    memcpy(lufrag, tok, 8);
    lufrag[8] = 0;
    auth_gen_token(s->lpwd);
    memcpy(s->lufrag, lufrag, sizeof lufrag);
    snprintf(s->expect_user, sizeof s->expect_user, "%s:%s", lufrag, rufrag);

    s->fd = udp_bind(g_port_cfg);
    if (s->fd < 0) {
        LOGE(MOD, "cannot bind udp port %d: %s", g_port_cfg, strerror(errno));
        sess_release(s);
        return 503;
    }
    {
        struct sockaddr_in a;
        socklen_t al = sizeof a;
        if (getsockname(s->fd, (struct sockaddr *)&a, &al) != 0) {
            sess_release(s);
            return 503;
        }
        s->port = ntohs(a.sin_port);
    }

    /* o= sess-id must be a NUMERIC string (RFC 4566 5.2) - the session's hex
     * id is fine for the Location header but not here. */
    static unsigned long long sdp_sid;
    if (!sdp_sid) sdp_sid = (unsigned long long)time(NULL) + 2208988800ULL;

    int n = snprintf(ans, (size_t)anscap,
        "v=0\r\n"
        "o=- %llu 1 IN IP4 %s\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "a=ice-lite\r\n"
        "a=group:BUNDLE %s\r\n"
        "a=msid-semantic: WMS\r\n"
        "m=video %d %s %s\r\n"
        "c=IN IP4 %s\r\n"
        "a=mid:%s\r\n"
        "a=rtcp-mux\r\n"
        "a=ice-ufrag:%s\r\n"
        "a=ice-pwd:%s\r\n"
        "a=fingerprint:sha-256 %s\r\n"
        "a=setup:passive\r\n"
        "a=inactive\r\n"
        "%s"
        "a=candidate:1 1 udp 2130706431 %s %d typ host\r\n"
        "a=end-of-candidates\r\n",
        sdp_sid, local_ip, mid, s->port, proto, pt, local_ip, mid,
        s->lufrag, s->lpwd, ms_dtls_fingerprint(g_dtls_ctx),
        rtpmap_line, local_ip, s->port);
    if (n >= anscap) { sess_release(s); return 500; }
    snprintf(sid, (size_t)sidcap, "%s", s->id);

    s->run = 1;
    /* MS_STACK_CONN, not MS_STACK_UTIL: this thread runs an mbedTLS
     * handshake, the same reason httpd.c's connection threads use it. */
    if (ms_thread_create(&s->thr, MS_STACK_CONN, sess_thread, s) != 0) {
        LOGE(MOD, "cannot start session thread");
        sess_release(s);
        return 503;
    }
    pthread_detach(s->thr);
    LOGI(MOD, "%s: answered WHEP offer, ICE-lite candidate %s:%d",
         s->id, local_ip, s->port);
    return 201;
}

void webrtc_start(const ms_config *cfg)
{
    if (!cfg->webrtc_enabled) return;
    g_port_cfg = cfg->webrtc_port;
    g_dtls_ctx = ms_dtls_ctx_new(cfg->http_tls_cert, cfg->http_tls_key);
    if (!g_dtls_ctx) {
        LOGE(MOD, "webrtc.enabled=1 but no usable DTLS certificate (%s / %s) "
                  "- /webrtc/whep stays disabled",
             cfg->http_tls_cert, cfg->http_tls_key);
        return;
    }
    LOGI(MOD, "WHEP endpoint /webrtc/whep ready (ICE-lite + DTLS, no media "
              "yet); fingerprint %s", ms_dtls_fingerprint(g_dtls_ctx));
}

void webrtc_stop(void)
{
    if (!g_dtls_ctx) return;
    for (int i = 0; i < WEBRTC_MAX_SESSIONS; i++) g_sess[i].run = 0;
    for (int w = 0; w < 50; w++) {
        int live = 0;
        for (int i = 0; i < WEBRTC_MAX_SESSIONS; i++) live += g_sess[i].used;
        if (!live) break;
        usleep(10000);
    }
    ms_dtls_ctx_free(g_dtls_ctx);
    g_dtls_ctx = NULL;
}

#endif /* USE_WEBRTC */
