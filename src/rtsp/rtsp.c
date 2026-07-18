#include "rtsp.h"
#include "rtp.h"
#include "../net.h"
#include "../hub.h"
#include "../log.h"
#include "../util.h"
#include "../codec/aac.h"
#include "../auth.h"
#include "../tls.h"
#ifdef USE_TLS
#include <fcntl.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>

#define MOD "RTSP"
#define VIDEO_PT 96
#define AUDIO_PT 97

/* global limit on concurrent RTSP clients (each costs a thread + fanqueue);
 * prevents unbounded memory/thread growth from many or slow clients */
#ifndef RTSP_MAX_CLIENTS
#define RTSP_MAX_CLIENTS 8
#endif
/* per-client fanqueue capacity (packet pointers) */
#ifndef MS_RTSP_QCAP
#define MS_RTSP_QCAP 64
#endif

static volatile int g_nclients;   /* current client count (sync builtins) */

/* M3: control fds of live accepted clients (stored as fd+1; 0 = free slot) so
 * rtsp_stop() can shutdown() them and unblock detached client threads parked
 * in recv()/TLS handshake/send before the TLS ctx is freed. The mutex orders
 * every stop-side shutdown() strictly before the owning thread's close(), so
 * a slot can never be shut down after its fd number was reused elsewhere. */
static pthread_mutex_t g_clients_mx = PTHREAD_MUTEX_INITIALIZER;
static int g_client_fd1[RTSP_MAX_CLIENTS];

static int client_fd_reg(int fd)
{
    int slot = -1;
    pthread_mutex_lock(&g_clients_mx);
    for (int i = 0; i < RTSP_MAX_CLIENTS; i++)
        if (!g_client_fd1[i]) { g_client_fd1[i] = fd + 1; slot = i; break; }
    pthread_mutex_unlock(&g_clients_mx);
    return slot;
}
static void client_fd_unreg(int slot)
{
    if (slot < 0) return;
    pthread_mutex_lock(&g_clients_mx);
    g_client_fd1[slot] = 0;
    pthread_mutex_unlock(&g_clients_mx);
}

struct rtsp_server {
    const ms_config *cfg;
    int              lfd;
    pthread_t        thr;
    volatile int     run;
#ifdef USE_TLS
    int              lfd_tls;   /* RTSPS listener, -1 = none */
    pthread_t        thr_tls;
    void            *tls_ctx;   /* ms_tls_ctx*, same cert/key as HTTPS */
#endif
};

/* RTP output sink (UDP or TCP-interleaved) */
typedef struct {
    int                tcp;          /* 1 = interleaved on control fd */
    int                fd;           /* udp RTP socket (or control fd, TCP) */
    int                fd_rtcp;      /* udp RTCP socket (UDP only, else unused) */
    struct sockaddr_in dst, dst_rtcp;
    int                chan_rtp, chan_rtcp;
#ifdef USE_TLS
    void              *tls;          /* ms_tls_conn* when interleaved over RTSPS */
#endif
} rtp_sink;

static int sink_send(void *ctx, const uint8_t *pkt, int len, int rtcp)
{
    rtp_sink *s = (rtp_sink*)ctx;
    if (s->tcp) {
        /* one write = one TCP segment: prepend the 4-byte interleave header to
         * the RTP packet in a single buffer (avoids a tiny header segment and
         * halves the syscalls, which dominated CPU with TCP_NODELAY) */
        uint8_t buf[4 + 1600];
        if (len > (int)sizeof(buf) - 4) return -1;
        buf[0] = '$';
        buf[1] = (uint8_t)(rtcp ? s->chan_rtcp : s->chan_rtp);
        buf[2] = (uint8_t)(len >> 8);
        buf[3] = (uint8_t)len;
        memcpy(buf + 4, pkt, len);
#ifdef USE_TLS
        /* interleaved packets ride the control connection: over RTSPS they
         * must go through TLS like every other byte on that connection */
        if (s->tls) return ms_tls_write((ms_tls_conn*)s->tls, buf, 4+len) < 0 ? -1 : len;
#endif
        return net_sendall(s->fd, buf, 4 + len) < 0 ? -1 : len;
    } else {
        /* UDP media stays plaintext even for RTSPS clients: RTSPS secures the
         * control channel (and interleaved-TCP media) only - no SRTP here.
         * RTCP must originate from the RTCP socket (server_port+1), not the
         * RTP one - some port-strict receivers drop a Sender Report whose
         * source port doesn't match the SETUP-negotiated server_port pair. */
        struct sockaddr_in *d = rtcp ? &s->dst_rtcp : &s->dst;
        int fd = rtcp ? s->fd_rtcp : s->fd;
        return (int)sendto(fd, pkt, len, 0, (struct sockaddr*)d, sizeof(*d));
    }
}

typedef struct {
    int                 fd;
    int                 slot;          /* g_client_fd1[] index (M3), -1 none */
    struct sockaddr_in  peer;
    const ms_config    *cfg;
    char                session[16];
    char                nonce[36];      /* per-connection digest nonce */
    int                 authed;
    int                 vchn;          /* video source index, -1 none */
    int                 have_video, have_audio;
    /* transport */
    int                 tcp;
    rtp_sink            vsink, asink;
    int                 v_udp[2], a_udp[2];   /* server rtp,rtcp fds */
    fanqueue            q;
    rtp_track           vtrack, atrack;
    int                 playing;
    int                 play_cseq;     /* CSeq of PLAY; 200 sent after subscribe */
#ifdef USE_TLS
    void               *tls;           /* ms_tls_conn*, NULL = plain RTSP */
    void               *tls_ctx;       /* listener's ms_tls_ctx* (RTSPS), else NULL */
#endif
} session;

/* control-channel I/O: transparently TLS when this is an RTSPS connection
 * (s->tls set), otherwise the plain socket. Without USE_TLS these are exactly
 * the old net_sendall(s->fd,...) / recv(s->fd,...) calls. */
static int r_send(session *s, const void *buf, int len)
{
#ifdef USE_TLS
    if (s->tls) return ms_tls_write((ms_tls_conn*)s->tls, buf, len);
#endif
    return net_sendall(s->fd, buf, len);
}
static int r_recv(session *s, void *buf, int len, int nonblock)
{
#ifdef USE_TLS
    if (s->tls) {
        if (nonblock) {
            /* control poll during PLAY: toggle O_NONBLOCK so ms_tls_read
             * returns 0 (WANT_READ = no data now, retry) instead of blocking,
             * and map that to the plain-recv EAGAIN convention - callers must
             * never mistake it for an orderly peer-close */
            int fl = fcntl(s->fd, F_GETFL, 0);
            fcntl(s->fd, F_SETFL, fl | O_NONBLOCK);
            int n = ms_tls_read((ms_tls_conn*)s->tls, buf, len);
            fcntl(s->fd, F_SETFL, fl);
            if (n == 0) { errno = EAGAIN; return -1; }      /* no data yet */
            if (n < 0)  { errno = ECONNRESET; return -1; }  /* closed/error */
            return n;
        }
        return ms_tls_read((ms_tls_conn*)s->tls, buf, len); /* blocking fd */
    }
#endif
    return recv(s->fd, buf, len, nonblock ? MSG_DONTWAIT : 0);
}

/* ---- request parsing helpers ---- */
static const char *hdr_find(const char *req, const char *name)
{
    /* case-insensitive line search; returns pointer after "name:" */
    size_t nl = strlen(name);
    const char *p = req;
    while (*p) {
        if (strncasecmp(p, name, nl)==0 && p[nl]==':')
            { p+=nl+1; while(*p==' ')p++; return p; }
        const char *e = strchr(p, '\n');
        if (!e) break;
        p = e+1;
    }
    return NULL;
}
static int hdr_int(const char *req, const char *name, int def)
{
    const char *p = hdr_find(req, name);
    return p ? atoi(p) : def;
}

static int find_video_by_path(const ms_config *c, const char *path)
{
    /* videoN.rtsp_path is runtime-mutable via /control: match under the
     * config string lock (short strcmps, per-request only, not per-frame) */
    config_str_lock();
    for (int i=0;i<MS_MAX_VSTREAM;i++){
        if (!c->video[i].enabled) continue;
        const char *rp = c->video[i].rtsp_path;
        /* match "/ch0" possibly followed by /trackID or end */
        size_t l = strlen(rp);
        if (strncmp(path, rp, l)==0 && (path[l]==0||path[l]=='/'||path[l]=='?'))
            { config_str_unlock(); return i; }
    }
    config_str_unlock();
    /* default to first enabled */
    for (int i=0;i<MS_MAX_VSTREAM;i++) if (c->video[i].enabled) return i;
    return -1;
}

/* extract the request path from "METHOD rtsp://host:port/path... RTSP/1.0" */
static void extract_path(const char *req, char *out, int outsz)
{
    out[0]=0;
    const char *sp = strchr(req, ' ');
    if (!sp) return;
    const char *url = sp+1;
    const char *end = strchr(url, ' ');
    if (!end) return;
    char tmp[512]; int n = (int)(end-url);
    if (n >= (int)sizeof tmp) n = sizeof(tmp)-1;
    memcpy(tmp, url, n); tmp[n]=0;
    /* strip scheme://host[:port] */
    const char *p = tmp;
    if (!strncasecmp(p,"rtsp://",7)){ p+=7; const char *slash=strchr(p,'/'); p = slash?slash:""; }
    strncpy(out, p, outsz-1); out[outsz-1]=0;
}

static void gen_sdp(session *s, const ms_config *c, int vchn, char *sdp, int sdpsz)
{
    char body[2048]; int n=0;
    struct sockaddr_in local; socklen_t sl=sizeof local;
    char ip[INET_ADDRSTRLEN];
    /* L12: getsockname() can fail (e.g. fd race on a fast disconnect); its
     * return was previously ignored, which could feed an uninitialized
     * `local` into inet_ntop() and emit a garbage IP in the SDP o=/c= lines.
     * Fall back to a safe, well-defined value instead. */
    if (getsockname(s->fd, (struct sockaddr*)&local, &sl) == 0)
        inet_ntop(AF_INET, &local.sin_addr, ip, sizeof ip);
    else
        strcpy(ip, "0.0.0.0");

    /* M2: guard every accumulation step so `sizeof(body)-n` (size_t) can
     * never underflow if an earlier snprintf() reported it would have
     * written past the buffer (n > sizeof(body)). Mirrors the n>=0/n<sizeof
     * guard already used for the RTP-Info header in stream_loop(). */
    if (n>=0 && n<(int)sizeof(body))
        n += snprintf(body+n, sizeof(body)-n,
            "v=0\r\no=- 0 0 IN IP4 %s\r\ns=timps\r\nc=IN IP4 %s\r\nt=0 0\r\n",
            ip, ip);

    /* video */
    const ms_vstream_cfg *v = &c->video[vchn];
    int isH265 = (v->codec==MS_VC_H265);
    if (n>=0 && n<(int)sizeof(body))
        n += snprintf(body+n, sizeof(body)-n,
            "m=video 0 RTP/AVP %d\r\na=rtpmap:%d %s/90000\r\n"
            "a=control:trackID=0\r\n",
            VIDEO_PT, VIDEO_PT, isH265?"H265":"H264");
    vparam vp;
    if (hub_get_vparam(vchn, &vp) && vparam_ready(&vp)) {
        char fmtp[1600];
        vparam_sdp_fmtp(&vp, VIDEO_PT, fmtp, sizeof fmtp);
        if (n>=0 && n<(int)sizeof(body))
            n += snprintf(body+n, sizeof(body)-n, "%s", fmtp);
    }

    /* audio - use the codec the HAL actually produces (from the hub) */
    int acodec, asr, ach;
    if (hub_get_audio(&acodec, &asr, &ach) && acodec != MS_AC_NONE) {
        if (acodec==MS_AC_AAC) {
            uint8_t asc[2]; aac_asc(asr, ach, asc);
            if (n>=0 && n<(int)sizeof(body))
                n += snprintf(body+n, sizeof(body)-n,
                    "m=audio 0 RTP/AVP %d\r\na=rtpmap:%d mpeg4-generic/%d/%d\r\n"
                    "a=fmtp:%d streamtype=5;profile-level-id=1;mode=AAC-hbr;"
                    "sizelength=13;indexlength=3;indexdeltalength=3;config=%02X%02X\r\n"
                    "a=control:trackID=1\r\n",
                    AUDIO_PT, AUDIO_PT, asr, ach, AUDIO_PT, asc[0], asc[1]);
        } else {
            int pt = (acodec==MS_AC_PCMA)?8:0;   /* static PTs */
            const char *nm = (acodec==MS_AC_PCMA)?"PCMA":"PCMU";
            if (n>=0 && n<(int)sizeof(body))
                n += snprintf(body+n, sizeof(body)-n,
                    "m=audio 0 RTP/AVP %d\r\na=rtpmap:%d %s/8000\r\n"
                    "a=control:trackID=1\r\n", pt, pt, nm);
        }
    }
    (void)sdpsz;
    snprintf(sdp, sdpsz, "%s", body);
}

static void send_resp(session *s, int cseq, const char *extra, const char *body)
{
    char hdr[3072];
    int bl = body ? (int)strlen(body) : 0;
    int n = snprintf(hdr, sizeof hdr,
        "RTSP/1.0 200 OK\r\nCSeq: %d\r\n%s", cseq, extra?extra:"");
    /* L2: snprintf returns the WOULD-BE length; clamp so hdr+n below never
     * points past the buffer and r_send's length matches what's in it */
    if (n < 0) return;
    if (n >= (int)sizeof hdr) n = (int)sizeof hdr - 1;
    int m;
    if (body)
        m = snprintf(hdr+n, sizeof(hdr)-n,
            "Content-Type: application/sdp\r\nContent-Length: %d\r\n\r\n%s", bl, body);
    else
        m = snprintf(hdr+n, sizeof(hdr)-n, "\r\n");
    if (m < 0) return;
    n += m;
    if (n >= (int)sizeof hdr) n = (int)sizeof hdr - 1;
    r_send(s, hdr, n);
}

/* copy the Authorization header value (up to CRLF) into out */
static void get_auth_hdr(const char *req, char *out, int outsz)
{
    out[0]=0;
    const char *v = hdr_find(req, "Authorization");
    if (!v) return;
    const char *e = v; while(*e && *e!='\r' && *e!='\n') e++;
    int n=(int)(e-v); if(n>=outsz)n=outsz-1;
    memcpy(out,v,n); out[n]=0;
}

/* returns 1 if request is authenticated (or auth not required) */
static int rtsp_check_auth(session *s, char *req)
{
    if (!s->cfg->rtsp_user[0]) return 1;          /* auth disabled */
    if (s->authed) return 1;                       /* already validated */
    char method[16]={0}; sscanf(req,"%15s",method);
    char av[512]; get_auth_hdr(req, av, sizeof av);
    if (av[0]) {
        if (auth_rtsp_digest(method, av, s->cfg->rtsp_user, s->cfg->rtsp_pass, s->nonce) ||
            auth_http_basic(av, s->cfg->rtsp_user, s->cfg->rtsp_pass)) {
            s->authed = 1; return 1;
        }
    }
    return 0;
}

static void rtsp_send_401(session *s, int cseq)
{
    /* a fresh nonce on every challenge (not just the first): a client
     * retrying with a stale/forged Authorization header now gets a new
     * nonce to authenticate against instead of s->nonce staying valid
     * (and replayable) for the rest of the TCP connection's lifetime. */
    auth_make_nonce(s->nonce);
    char hdr[512];
    int n=snprintf(hdr,sizeof hdr,
        "RTSP/1.0 401 Unauthorized\r\nCSeq: %d\r\n"
        "WWW-Authenticate: Digest realm=\"%s\", nonce=\"%s\"\r\n"
        "WWW-Authenticate: Basic realm=\"%s\"\r\n\r\n",
        cseq, AUTH_REALM, s->nonce, AUTH_REALM);
    r_send(s, hdr, n);
}

/* returns 0 to keep connection, <0 to close, 1 = start playing */
static int handle_request(session *s, char *req)
{
    int cseq = hdr_int(req, "CSeq", 0);
    char path[256]; extract_path(req, path, sizeof path);

    if (!strncmp(req, "OPTIONS", 7)) {
        send_resp(s, cseq,
            "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n", NULL);
        return 0;
    }
    /* every method except OPTIONS requires authentication */
    if (!rtsp_check_auth(s, req)) { rtsp_send_401(s, cseq); return 0; }
    if (!strncmp(req, "DESCRIBE", 8)) {
        int vchn = find_video_by_path(s->cfg, path);
        if (vchn < 0) { r_send(s,"RTSP/1.0 404 Not Found\r\n\r\n",26); return -1; }
        s->vchn = vchn;
        /* ensure params exist quickly */
        hub_request_idr(vchn);
        for (int i=0;i<50;i++){ vparam vp; if(hub_get_vparam(vchn,&vp)&&vparam_ready(&vp))break; usleep(10000);}
        char sdp[2600]; gen_sdp(s, s->cfg, vchn, sdp, sizeof sdp);
        /* players use the request URL as Content-Base */
        send_resp(s, cseq, "", sdp);
        return 0;
    }
    if (!strncmp(req, "SETUP", 5)) {
        const char *tr = hdr_find(req, "Transport");
        int is_audio = strstr(path,"trackID=1") != NULL;
        if (s->vchn < 0) s->vchn = find_video_by_path(s->cfg, path);
        /* no valid video source -> refuse; otherwise vchn==-1 would index
         * c->video[-1] (OOB) later in stream_loop */
        if (s->vchn < 0) {
            const char *e404 = "RTSP/1.0 404 Not Found\r\n\r\n";
            r_send(s, e404, (int)strlen(e404));
            return -1;
        }
        if (!s->session[0]) {
            /* M6: session id from auth_gen_token()'s /dev/urandom generator,
             * not rand() - rand() is seeded time^pid (main.c) and guessable */
            char tok[33]; auth_gen_token(tok);
            snprintf(s->session, sizeof s->session, "%.8s", tok);
        }

        char extra[256];
        if (tr && strstr(tr,"TCP")) {
            /* interleaved */
            int rc=0, cc=1;
            const char *il = strstr(tr,"interleaved=");
            if (il) sscanf(il+12,"%d-%d",&rc,&cc);
            if (rc<0||rc>255) rc=0;          /* N4: reject out-of-range channels */
            if (cc<0||cc>255) cc = rc<255 ? rc+1 : 0;
            rtp_sink *snk = is_audio ? &s->asink : &s->vsink;
            snk->tcp=1; snk->fd=s->fd; snk->chan_rtp=rc; snk->chan_rtcp=cc;
#ifdef USE_TLS
            snk->tls=s->tls;   /* interleaved media rides the (TLS?) control conn */
#endif
            s->tcp=1;
            if (is_audio) s->have_audio=1; else s->have_video=1;
            snprintf(extra,sizeof extra,
                "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\nSession: %s\r\n",
                rc,cc,s->session);
        } else {
            /* UDP */
            int cp=0, cp2=0;
            const char *cpp = tr?strstr(tr,"client_port="):NULL;
            if (cpp) sscanf(cpp+12,"%d-%d",&cp,&cp2);
            if (cp<0||cp>65535) cp=0;        /* N4: reject out-of-range ports */
            if (cp2<0||cp2>65535) cp2=0;
            int *udp = is_audio ? s->a_udp : s->v_udp;
            /* H1: a repeated SETUP for the same track (re-SETUP, or an
             * unauthenticated client just hammering SETUP before ever
             * PLAYing) used to overwrite udp[0]/udp[1] via
             * net_bind_udp_pair() below without closing the pair already
             * bound for this track, leaking 2 fds per repeat until the
             * whole process ran out of sockets for every client. Close any
             * previously bound pair for this track first; net_bind_udp_pair()
             * only ever writes udp[0]/udp[1] on success, so leaving them at
             * -1 here is safe even if the rebind below fails. */
            if (udp[0] >= 0) { close(udp[0]); udp[0] = -1; }
            if (udp[1] >= 0) { close(udp[1]); udp[1] = -1; }
            /* pick a free even/odd port pair from a wide range with retries.
             * The old tiny random window (6000 + chn*4 + rand()&0x3E) collided
             * as soon as a few clients streamed concurrently -> bind failed. */
            int base = 0, bound = -1;
            for (int t = 0; t < 64 && bound < 0; t++) {
                base = 6000 + ((rand() % 8192) & ~1);       /* even, 6000..14190 */
                bound = net_bind_udp_pair(&udp[0], &udp[1], base);
            }
            if (bound < 0){
                r_send(s,"RTSP/1.0 500 Internal\r\n\r\n",25); return -1; }
            rtp_sink *snk = is_audio ? &s->asink : &s->vsink;
            snk->tcp=0; snk->fd=udp[0]; snk->fd_rtcp=udp[1];
            snk->dst=s->peer; snk->dst.sin_port=htons((uint16_t)cp);
            snk->dst_rtcp=s->peer; snk->dst_rtcp.sin_port=htons((uint16_t)cp2);
            if (is_audio) s->have_audio=1; else s->have_video=1;
            snprintf(extra,sizeof extra,
                "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d\r\nSession: %s\r\n",
                cp,cp2, base,base+1, s->session);
        }
        send_resp(s, cseq, extra, NULL);
        return 0;
    }
    if (!strncmp(req, "PLAY", 4)) {
        /* PLAY without a successful SETUP (or without a video source) */
        if ((!s->have_video && !s->have_audio) ||
            (s->have_video && s->vchn < 0)) {
            const char *e = "RTSP/1.0 455 Method Not Valid in This State\r\n\r\n";
            r_send(s, e, (int)strlen(e));
            return -1;
        }
        /* the 200 OK is sent from stream_loop once hub_subscribe succeeded */
        s->play_cseq = cseq;
        return 1;
    }
    if (!strncmp(req, "GET_PARAMETER", 13)) {
        char extra[64]; snprintf(extra,sizeof extra,"Session: %s\r\n",s->session);
        send_resp(s, cseq, extra, NULL);
        return 0;
    }
    if (!strncmp(req, "TEARDOWN", 8)) {
        send_resp(s, cseq, "", NULL);
        return -1;
    }
    r_send(s,"RTSP/1.0 405 Method Not Allowed\r\n\r\n",35);
    return 0;
}

static void stream_loop(session *s)
{
    const ms_config *c = s->cfg;
    /* guard against an invalid video channel (would read c->video[-1]) */
    if (s->have_video &&
        (s->vchn < 0 || s->vchn >= MS_MAX_VSTREAM || !c->video[s->vchn].enabled)) {
        const char *e404 = "RTSP/1.0 404 Not Found\r\n\r\n";
        r_send(s, e404, (int)strlen(e404));
        return;
    }
    int vc = s->have_video ? c->video[s->vchn].codec : MS_VC_H264;
    int ac = MS_AC_AAC, asr = c->audio.samplerate, ach = c->audio.channels;
    hub_get_audio(&ac, &asr, &ach);      /* actual audio codec from the HAL */

    int sub_v = 0, sub_a = 0;
    if (s->have_video) {
        rtp_track_init(&s->vtrack, VIDEO_PT, 90000, sink_send, &s->vsink);
        if (hub_subscribe(s->vchn, &s->q) != 0) goto full;
        sub_v = 1;
        hub_request_idr(s->vchn);
    }
    if (s->have_audio) {
        int apt = (ac==MS_AC_AAC)?AUDIO_PT : (ac==MS_AC_PCMA?8:0);
        int arate = (ac==MS_AC_AAC)?asr:8000;
        rtp_track_init(&s->atrack, apt, arate, sink_send, &s->asink);
        if (hub_subscribe(HUB_AUDIO_SRC, &s->q) != 0) goto full;
        sub_a = 1;
    }

    /* subscriptions succeeded -> confirm PLAY now */
    {
        char extra[320];
        int n = snprintf(extra,sizeof extra,
                         "Session: %s\r\nRange: npt=0.000-\r\n",s->session);
        /* RFC 2326 12.33: RTP-Info maps the (random) initial seq/rtptime a
         * client will see to the stream start, so it can detect sequence
         * gaps and compute presentation time correctly before the first
         * RTCP SR arrives. Both values are exact even though no packet has
         * been sent yet: seq is rtp_hdr()'s current t->seq (it post-
         * increments on use, so this IS the first packet's seq), and
         * rtptime is exactly t->ts_base (pts_to_ts() anchors rel=0 on its
         * very first call). VLC/ffplay tolerate its absence; some
         * ONVIF/live555-derived NVR stacks wait for it or mis-seed their
         * jitter buffer without it. */
        if ((sub_v || sub_a) && n>0 && n<(int)sizeof extra) {
            n += snprintf(extra+n, sizeof(extra)-n, "RTP-Info: ");
            if (sub_v && n>0 && n<(int)sizeof extra)
                n += snprintf(extra+n, sizeof(extra)-n,
                              "url=trackID=0;seq=%u;rtptime=%u",
                              (unsigned)s->vtrack.seq, (unsigned)s->vtrack.ts_base);
            if (sub_v && sub_a && n>0 && n<(int)sizeof extra)
                n += snprintf(extra+n, sizeof(extra)-n, ",");
            if (sub_a && n>0 && n<(int)sizeof extra)
                n += snprintf(extra+n, sizeof(extra)-n,
                              "url=trackID=1;seq=%u;rtptime=%u",
                              (unsigned)s->atrack.seq, (unsigned)s->atrack.ts_base);
            if (n>0 && n<(int)sizeof extra) snprintf(extra+n, sizeof(extra)-n, "\r\n");
        }
        send_resp(s, s->play_cseq, extra, NULL);
    }

    /* keep the socket blocking so TCP-interleaved writes are never partial
     * (we poll for control input with MSG_DONTWAIT instead) */
    char ctl[2048]; int ctlhave = 0;
    int got_key = 0;   /* start clients on a keyframe for clean decode */
    LOGI(MOD,"PLAY session=%s vchn=%d %s v=%d a=%d", s->session, s->vchn,
         s->tcp?"TCP":"UDP", s->have_video, s->have_audio);

    while (s->playing) {
        /* if the queue overflowed and dropped a keyframe, request a fresh IDR
         * so the client doesn't decode garbage until the next GOP */
        if (sub_v && fanqueue_take_dropped_key(&s->q)) hub_request_idr(s->vchn);
        ms_pkt *p = fanqueue_pop(&s->q, 100);
        if (p) {
            int sendrc = 0;
            if (p->media==MS_MEDIA_VIDEO && s->have_video) {
                if (!got_key) {
                    if (!p->keyframe) { pkt_unref(p); goto after_pkt; }
                    got_key = 1;
                }
                if (vc==MS_VC_H265) sendrc = rtp_send_h265(&s->vtrack,p->data,p->len,p->pts_us);
                else                sendrc = rtp_send_h264(&s->vtrack,p->data,p->len,p->pts_us);
            } else if (p->media==MS_MEDIA_AUDIO && s->have_audio) {
                if (ac==MS_AC_AAC) sendrc = rtp_send_aac(&s->atrack,p->data,p->len,p->pts_us);
                else               sendrc = rtp_send_g711(&s->atrack,p->data,p->len,p->pts_us);
            }
            pkt_unref(p);
            /* H-1: a failed send (SO_SNDTIMEO expired after 15s of zero
             * progress, or client gone) may have left a PARTIAL '$'-framed
             * interleaved packet (or torn TLS write) on the wire. One more
             * byte would permanently desync the framing for a client that
             * later drains its window - and looping forever on a stalled
             * client would pin this slot (defeats the DoS timeout). Stop
             * the play loop now; teardown below closes the fd. */
            if (sendrc < 0) { s->playing = 0; break; }
        }
    after_pkt:;
        int64_t now = ms_now_us();
        if ((s->have_video && rtp_maybe_sr(&s->vtrack, now) < 0) ||
            (s->have_audio && rtp_maybe_sr(&s->atrack, now) < 0)) {
            s->playing = 0; break;      /* H-1: torn RTCP frame, see above */
        }

        /* poll control socket for TEARDOWN/keepalive/close; over TLS r_recv
         * maps "no data yet" to -1/EAGAIN, so n==0 only ever means a plain
         * socket's orderly close. ctl/ctlhave persist across polls (unlike
         * a single-shot per-recv buffer) so a request split across TCP
         * segments is correctly reassembled, and a client-sent interleaved
         * '$' RTCP frame is skipped by its own declared length instead of
         * discarding the whole recv() chunk - which used to also silently
         * eat a TEARDOWN (or any other request) concatenated right after
         * it in the same read. */
        int n = r_recv(s, ctl+ctlhave, (int)sizeof(ctl)-1-ctlhave, 1);
        if (n==0) break;                         /* peer closed */
        if (n>0) {
            ctlhave += n; ctl[ctlhave]=0;
            int close_conn = 0, progressed;
            do {
                progressed = 0;
                if (ctlhave >= 4 && ctl[0]=='$') {
                    int flen = ((unsigned char)ctl[2]<<8)|(unsigned char)ctl[3];
                    int total = 4 + flen;
                    if (total > (int)sizeof(ctl)-1) { close_conn = 1; break; } /* bogus length */
                    if (ctlhave < total) break;              /* wait for the rest */
                    memmove(ctl, ctl+total, (size_t)(ctlhave-total+1));
                    ctlhave -= total; progressed = 1;
                    continue;
                }
                if (ctlhave > 0 && ctl[0]!='$') {
                    char *end = strstr(ctl, "\r\n\r\n");
                    if (!end) break;                          /* incomplete request, wait */
                    size_t reqlen = (size_t)(end-ctl) + 4;
                    if (!strncmp(ctl,"TEARDOWN",8)) { close_conn = 1; break; }
                    if (!strncmp(ctl,"GET_PARAMETER",13)||!strncmp(ctl,"OPTIONS",7)) {
                        int cseq=hdr_int(ctl,"CSeq",0);
                        char e[64]; snprintf(e,sizeof e,"Session: %s\r\n",s->session);
                        send_resp(s,cseq,e,NULL);
                    }
                    memmove(ctl, ctl+reqlen, ctlhave-reqlen+1);
                    ctlhave -= (int)reqlen; progressed = 1;
                }
            } while (progressed);
            if (close_conn) break;
            /* buffer full with no complete frame/request drained: either a
             * bogus/oversized interleaved length or a request line longer
             * than we're willing to buffer - drop the connection rather
             * than spin forever unable to make progress */
            if (ctlhave >= (int)sizeof(ctl)-1) break;
        } else if (errno!=EAGAIN && errno!=EWOULDBLOCK) {
            break;
        }
    }

    if (sub_v) hub_unsubscribe(s->vchn, &s->q);
    if (sub_a) hub_unsubscribe(HUB_AUDIO_SRC, &s->q);
    return;

full:
    /* source subscriber table full (> HUB_MAX_SUBS consumers) */
    if (sub_v) hub_unsubscribe(s->vchn, &s->q);
    {
        const char *e503 = "RTSP/1.0 503 Service Unavailable\r\n\r\n";
        r_send(s, e503, (int)strlen(e503));
    }
    LOGW(MOD,"subscribe failed (source full), closing session=%s", s->session);
}

static void *client_thread(void *arg)
{
    session *s = (session*)arg;
    net_set_nodelay(s->fd);
#ifdef USE_TLS
    /* RTSPS: run the TLS handshake before any request I/O. From here on all
     * control and interleaved-TCP I/O uses r_send/r_recv (s->tls aware). */
    if (s->tls_ctx) {
        s->tls = ms_tls_accept((ms_tls_ctx*)s->tls_ctx, s->fd);
        if (!s->tls) goto done;
    }
#endif
    char buf[4096];
    int have=0, playing=0;

    /* control phase: read requests until PLAY */
    while (!playing) {
        int n = r_recv(s, buf+have, (int)sizeof(buf)-1-have, 0);
        if (n<=0) goto done;
        have += n; buf[have]=0;
        char *end;
        while ((end = strstr(buf, "\r\n\r\n")) != NULL) {
            size_t reqlen = (size_t)(end - buf) + 4;
            char req[4096];
            memcpy(req, buf, reqlen); req[reqlen]=0;
            int r = handle_request(s, req);
            memmove(buf, buf+reqlen, have-reqlen+1);
            have -= reqlen;
            if (r < 0) goto done;
            if (r == 1) { playing=1; break; }
        }
    }

    s->playing = 1;
    if (fanqueue_init(&s->q, MS_RTSP_QCAP)==0) {
        stream_loop(s);
        fanqueue_free(&s->q);
    }

done:
    LOGI(MOD,"client disconnect session=%s", s->session[0]?s->session:"-");
#ifdef USE_TLS
    if (s->tls) ms_tls_close((ms_tls_conn*)s->tls);
#endif
    /* M3: unregister BEFORE close() - once closed, the fd number can be
     * reused, and rtsp_stop() must never shutdown() a reused fd */
    client_fd_unreg(s->slot);
    close(s->fd);
    /* L15: fd 0 is a valid bound socket; only -1 means "not bound". */
    if (s->v_udp[0]>=0) close(s->v_udp[0]);
    if (s->v_udp[1]>=0) close(s->v_udp[1]);
    if (s->a_udp[0]>=0) close(s->a_udp[0]);
    if (s->a_udp[1]>=0) close(s->a_udp[1]);
    free(s);
    __sync_fetch_and_sub(&g_nclients, 1);
    return NULL;
}

/* shared accept loop for the plain and (USE_TLS) RTSPS listeners; the TLS
 * handshake itself runs in client_thread so a slow client cannot stall it */
static void accept_loop(rtsp_server *sv, int lfd, int port, void *tls_ctx)
{
    LOGI(MOD,"listening on port %d%s", port, tls_ctx?" (RTSPS)":"");
    while (sv->run) {
        struct sockaddr_in peer; socklen_t pl=sizeof peer;
        int cfd = accept(lfd, (struct sockaddr*)&peer, &pl);
        if (cfd<0){ if(sv->run) usleep(50000); continue; }
        /* H1: bounded control I/O - a client that connects and goes silent
         * (or stops reading) must time out instead of pinning this slot's
         * thread forever in recv()/TLS-handshake/send. Streaming clients
         * read/write continuously and never trip these. */
        net_set_timeouts(cfd, 30, 15);
        /* global client cap: each client costs a thread + bounded queue.
         * L1: reserve the slot atomically (add-then-check) - the old plain
         * read of g_nclients let two racing accepts both pass the cap. */
        if (__sync_add_and_fetch(&g_nclients, 1) > RTSP_MAX_CLIENTS) {
            __sync_fetch_and_sub(&g_nclients, 1);
            const char *e503 = "RTSP/1.0 503 Service Unavailable\r\n\r\n";
            net_sendall(cfd, e503, (int)strlen(e503));
            close(cfd);
            LOGW(MOD,"client limit (%d) reached, rejecting", RTSP_MAX_CLIENTS);
            continue;
        }
        session *s = (session*)calloc(1,sizeof(session));
        if (!s){ close(cfd); __sync_fetch_and_sub(&g_nclients, 1); continue; }
        s->fd=cfd; s->peer=peer; s->cfg=sv->cfg; s->vchn=-1;
        s->slot = client_fd_reg(cfd);   /* M3: visible to rtsp_stop() */
        /* L15: fds are 0 (calloc), not "unbound", after this - a bound fd
         * can legitimately be 0 (stdin closed at startup) or overlap with
         * PID/fd reuse, so unbound MUST be a value no real fd ever has. */
        s->v_udp[0]=s->v_udp[1]=s->a_udp[0]=s->a_udp[1]=-1;
#ifdef USE_TLS
        s->tls_ctx = tls_ctx;   /* non-NULL on the RTSPS listener */
#endif
        pthread_t t;
        if (pthread_create(&t,NULL,client_thread,s)==0) pthread_detach(t);
        else { client_fd_unreg(s->slot); close(cfd); free(s);
               __sync_fetch_and_sub(&g_nclients, 1); }
    }
}

static void *accept_thread(void *arg)
{
    rtsp_server *sv = (rtsp_server*)arg;
    accept_loop(sv, sv->lfd, sv->cfg->rtsp_port, NULL);
    return NULL;
}
#ifdef USE_TLS
static void *accept_tls_thread(void *arg)
{
    rtsp_server *sv = (rtsp_server*)arg;
    accept_loop(sv, sv->lfd_tls, sv->cfg->rtsp_tls_port, sv->tls_ctx);
    return NULL;
}
#endif

rtsp_server *rtsp_start(const ms_config *cfg)
{
    rtsp_server *s = (rtsp_server*)calloc(1,sizeof(*s));
    if (!s) return NULL;
    s->cfg = cfg;
    s->lfd = net_listen_tcp(cfg->rtsp_port, 8);
    if (s->lfd < 0){ LOGE(MOD,"cannot bind rtsp port %d",cfg->rtsp_port); free(s); return NULL; }
    s->run = 1;
    pthread_create(&s->thr, NULL, accept_thread, s);
#ifdef USE_TLS
    s->lfd_tls = -1;
    if (cfg->rtsp_tls) {
        /* RTSPS shares the HTTPS cert/key */
        s->tls_ctx = ms_tls_ctx_new(cfg->http_tls_cert, cfg->http_tls_key);
        if (!s->tls_ctx)
            LOGE(MOD,"RTSPS requested but TLS context failed - plain RTSP only");
        else {
            s->lfd_tls = net_listen_tcp(cfg->rtsp_tls_port, 8);
            if (s->lfd_tls < 0)
                LOGE(MOD,"cannot bind rtsps port %d",cfg->rtsp_tls_port);
            else if (pthread_create(&s->thr_tls, NULL, accept_tls_thread, s) != 0){
                close(s->lfd_tls); s->lfd_tls = -1;
            }
        }
    }
#else
    if (cfg->rtsp_tls) LOGW(MOD,"RTSPS requested but built without USE_TLS");
#endif
    return s;
}

void rtsp_stop(rtsp_server *s)
{
    if (!s) return;
    s->run = 0;
    shutdown(s->lfd, SHUT_RDWR);   /* close() alone does not wake accept() */
    close(s->lfd);
    pthread_join(s->thr, NULL);
#ifdef USE_TLS
    if (s->lfd_tls >= 0) {
        shutdown(s->lfd_tls, SHUT_RDWR);
        close(s->lfd_tls);
        pthread_join(s->thr_tls, NULL);
    }
#endif
    /* M3: both accept loops are joined (no new clients can register). Wake
     * detached client threads parked in recv()/TLS handshake/send by
     * shutting their control fds down - shutdown() only, the owning thread
     * still does the close(); the registry mutex guarantees we never touch
     * an fd number after its thread closed (and the kernel reused) it.
     * With the H1/M1 socket timeouts this is belt-and-suspenders, but it
     * makes the bounded drain below actually effective at shutdown time. */
    pthread_mutex_lock(&g_clients_mx);
    for (int i = 0; i < RTSP_MAX_CLIENTS; i++)
        if (g_client_fd1[i]) shutdown(g_client_fd1[i] - 1, SHUT_RDWR);
    pthread_mutex_unlock(&g_clients_mx);
    for (int i = 0; i < 100 && g_nclients > 0; i++) usleep(10000);
#ifdef USE_TLS
    if (s->tls_ctx) {
        /* detached client threads referenced conf/cert/drbg from this ctx;
         * only free it after the drain above (use-after-free at shutdown) */
        ms_tls_ctx_free((ms_tls_ctx*)s->tls_ctx);
    }
#endif
    free(s);
}
