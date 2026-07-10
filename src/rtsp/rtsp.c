#include "rtsp.h"
#include "rtp.h"
#include "../net.h"
#include "../hub.h"
#include "../log.h"
#include "../util.h"
#include "../codec/aac.h"
#include "../auth.h"
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

struct rtsp_server {
    const ms_config *cfg;
    int              lfd;
    pthread_t        thr;
    volatile int     run;
};

/* RTP output sink (UDP or TCP-interleaved) */
typedef struct {
    int                tcp;          /* 1 = interleaved on control fd */
    int                fd;           /* udp socket or control fd */
    struct sockaddr_in dst, dst_rtcp;
    int                chan_rtp, chan_rtcp;
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
        return net_sendall(s->fd, buf, 4 + len) < 0 ? -1 : len;
    } else {
        struct sockaddr_in *d = rtcp ? &s->dst_rtcp : &s->dst;
        return (int)sendto(s->fd, pkt, len, 0, (struct sockaddr*)d, sizeof(*d));
    }
}

typedef struct {
    int                 fd;
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
} session;

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
    for (int i=0;i<MS_MAX_VSTREAM;i++){
        if (!c->video[i].enabled) continue;
        const char *rp = c->video[i].rtsp_path;
        /* match "/ch0" possibly followed by /trackID or end */
        size_t l = strlen(rp);
        if (strncmp(path, rp, l)==0 && (path[l]==0||path[l]=='/'||path[l]=='?'))
            return i;
    }
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
    getsockname(s->fd, (struct sockaddr*)&local, &sl);
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &local.sin_addr, ip, sizeof ip);

    n += snprintf(body+n, sizeof(body)-n,
        "v=0\r\no=- 0 0 IN IP4 %s\r\ns=timps\r\nc=IN IP4 %s\r\nt=0 0\r\n",
        ip, ip);

    /* video */
    const ms_vstream_cfg *v = &c->video[vchn];
    int isH265 = (v->codec==MS_VC_H265);
    n += snprintf(body+n, sizeof(body)-n,
        "m=video 0 RTP/AVP %d\r\na=rtpmap:%d %s/90000\r\n"
        "a=control:trackID=0\r\n",
        VIDEO_PT, VIDEO_PT, isH265?"H265":"H264");
    vparam vp;
    if (hub_get_vparam(vchn, &vp) && vparam_ready(&vp)) {
        char fmtp[1600];
        vparam_sdp_fmtp(&vp, VIDEO_PT, fmtp, sizeof fmtp);
        n += snprintf(body+n, sizeof(body)-n, "%s", fmtp);
    }

    /* audio - use the codec the HAL actually produces (from the hub) */
    int acodec, asr, ach;
    if (hub_get_audio(&acodec, &asr, &ach) && acodec != MS_AC_NONE) {
        if (acodec==MS_AC_AAC) {
            uint8_t asc[2]; aac_asc(asr, ach, asc);
            n += snprintf(body+n, sizeof(body)-n,
                "m=audio 0 RTP/AVP %d\r\na=rtpmap:%d mpeg4-generic/%d/%d\r\n"
                "a=fmtp:%d streamtype=5;profile-level-id=1;mode=AAC-hbr;"
                "sizelength=13;indexlength=3;indexdeltalength=3;config=%02X%02X\r\n"
                "a=control:trackID=1\r\n",
                AUDIO_PT, AUDIO_PT, asr, ach, AUDIO_PT, asc[0], asc[1]);
        } else {
            int pt = (acodec==MS_AC_PCMA)?8:0;   /* static PTs */
            const char *nm = (acodec==MS_AC_PCMA)?"PCMA":"PCMU";
            n += snprintf(body+n, sizeof(body)-n,
                "m=audio 0 RTP/AVP %d\r\na=rtpmap:%d %s/8000\r\n"
                "a=control:trackID=1\r\n", pt, pt, nm);
        }
    }
    (void)sdpsz;
    snprintf(sdp, sdpsz, "%s", body);
}

static void send_resp(int fd, int cseq, const char *extra, const char *body)
{
    char hdr[3072];
    int bl = body ? (int)strlen(body) : 0;
    int n = snprintf(hdr, sizeof hdr,
        "RTSP/1.0 200 OK\r\nCSeq: %d\r\n%s", cseq, extra?extra:"");
    if (body)
        n += snprintf(hdr+n, sizeof(hdr)-n,
            "Content-Type: application/sdp\r\nContent-Length: %d\r\n\r\n%s", bl, body);
    else
        n += snprintf(hdr+n, sizeof(hdr)-n, "\r\n");
    net_sendall(fd, hdr, n);
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
        if (auth_rtsp_digest(method, av, s->cfg->rtsp_user, s->cfg->rtsp_pass) ||
            auth_http_basic(av, s->cfg->rtsp_user, s->cfg->rtsp_pass)) {
            s->authed = 1; return 1;
        }
    }
    return 0;
}

static void rtsp_send_401(session *s, int cseq)
{
    if (!s->nonce[0]) auth_make_nonce(s->nonce);
    char hdr[512];
    int n=snprintf(hdr,sizeof hdr,
        "RTSP/1.0 401 Unauthorized\r\nCSeq: %d\r\n"
        "WWW-Authenticate: Digest realm=\"%s\", nonce=\"%s\"\r\n"
        "WWW-Authenticate: Basic realm=\"%s\"\r\n\r\n",
        cseq, AUTH_REALM, s->nonce, AUTH_REALM);
    net_sendall(s->fd, hdr, n);
}

/* returns 0 to keep connection, <0 to close, 1 = start playing */
static int handle_request(session *s, char *req)
{
    int cseq = hdr_int(req, "CSeq", 0);
    char path[256]; extract_path(req, path, sizeof path);

    if (!strncmp(req, "OPTIONS", 7)) {
        send_resp(s->fd, cseq,
            "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n", NULL);
        return 0;
    }
    /* every method except OPTIONS requires authentication */
    if (!rtsp_check_auth(s, req)) { rtsp_send_401(s, cseq); return 0; }
    if (!strncmp(req, "DESCRIBE", 8)) {
        int vchn = find_video_by_path(s->cfg, path);
        if (vchn < 0) { net_sendall(s->fd,"RTSP/1.0 404 Not Found\r\n\r\n",26); return -1; }
        s->vchn = vchn;
        /* ensure params exist quickly */
        hub_request_idr(vchn);
        for (int i=0;i<50;i++){ vparam vp; if(hub_get_vparam(vchn,&vp)&&vparam_ready(&vp))break; usleep(10000);}
        char sdp[2600]; gen_sdp(s, s->cfg, vchn, sdp, sizeof sdp);
        /* players use the request URL as Content-Base */
        send_resp(s->fd, cseq, "", sdp);
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
            net_sendall(s->fd, e404, (int)strlen(e404));
            return -1;
        }
        if (!s->session[0]) snprintf(s->session,sizeof s->session,"%08X",(unsigned)rand());

        char extra[256];
        if (tr && strstr(tr,"TCP")) {
            /* interleaved */
            int rc=0, cc=1;
            const char *il = strstr(tr,"interleaved=");
            if (il) sscanf(il+12,"%d-%d",&rc,&cc);
            rtp_sink *snk = is_audio ? &s->asink : &s->vsink;
            snk->tcp=1; snk->fd=s->fd; snk->chan_rtp=rc; snk->chan_rtcp=cc;
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
            int *udp = is_audio ? s->a_udp : s->v_udp;
            /* pick a free even/odd port pair from a wide range with retries.
             * The old tiny random window (6000 + chn*4 + rand()&0x3E) collided
             * as soon as a few clients streamed concurrently -> bind failed. */
            int base = 0, bound = -1;
            for (int t = 0; t < 64 && bound < 0; t++) {
                base = 6000 + ((rand() % 8192) & ~1);       /* even, 6000..14190 */
                bound = net_bind_udp_pair(&udp[0], &udp[1], base);
            }
            if (bound < 0){
                net_sendall(s->fd,"RTSP/1.0 500 Internal\r\n\r\n",25); return -1; }
            rtp_sink *snk = is_audio ? &s->asink : &s->vsink;
            snk->tcp=0; snk->fd=udp[0];
            snk->dst=s->peer; snk->dst.sin_port=htons((uint16_t)cp);
            snk->dst_rtcp=s->peer; snk->dst_rtcp.sin_port=htons((uint16_t)cp2);
            if (is_audio) s->have_audio=1; else s->have_video=1;
            snprintf(extra,sizeof extra,
                "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d\r\nSession: %s\r\n",
                cp,cp2, base,base+1, s->session);
        }
        send_resp(s->fd, cseq, extra, NULL);
        return 0;
    }
    if (!strncmp(req, "PLAY", 4)) {
        /* PLAY without a successful SETUP (or without a video source) */
        if ((!s->have_video && !s->have_audio) ||
            (s->have_video && s->vchn < 0)) {
            const char *e = "RTSP/1.0 455 Method Not Valid in This State\r\n\r\n";
            net_sendall(s->fd, e, (int)strlen(e));
            return -1;
        }
        /* the 200 OK is sent from stream_loop once hub_subscribe succeeded */
        s->play_cseq = cseq;
        return 1;
    }
    if (!strncmp(req, "GET_PARAMETER", 13)) {
        char extra[64]; snprintf(extra,sizeof extra,"Session: %s\r\n",s->session);
        send_resp(s->fd, cseq, extra, NULL);
        return 0;
    }
    if (!strncmp(req, "TEARDOWN", 8)) {
        send_resp(s->fd, cseq, "", NULL);
        return -1;
    }
    net_sendall(s->fd,"RTSP/1.0 405 Method Not Allowed\r\n\r\n",35);
    return 0;
}

static void stream_loop(session *s)
{
    const ms_config *c = s->cfg;
    /* guard against an invalid video channel (would read c->video[-1]) */
    if (s->have_video &&
        (s->vchn < 0 || s->vchn >= MS_MAX_VSTREAM || !c->video[s->vchn].enabled)) {
        const char *e404 = "RTSP/1.0 404 Not Found\r\n\r\n";
        net_sendall(s->fd, e404, (int)strlen(e404));
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
        char extra[128];
        snprintf(extra,sizeof extra,"Session: %s\r\nRange: npt=0.000-\r\n",s->session);
        send_resp(s->fd, s->play_cseq, extra, NULL);
    }

    /* keep the socket blocking so TCP-interleaved writes are never partial
     * (we poll for control input with MSG_DONTWAIT instead) */
    char ctl[1024];
    int got_key = 0;   /* start clients on a keyframe for clean decode */
    LOGI(MOD,"PLAY session=%s vchn=%d %s v=%d a=%d", s->session, s->vchn,
         s->tcp?"TCP":"UDP", s->have_video, s->have_audio);

    while (s->playing) {
        /* if the queue overflowed and dropped a keyframe, request a fresh IDR
         * so the client doesn't decode garbage until the next GOP */
        if (sub_v && fanqueue_take_dropped_key(&s->q)) hub_request_idr(s->vchn);
        ms_pkt *p = fanqueue_pop(&s->q, 100);
        if (p) {
            if (p->media==MS_MEDIA_VIDEO && s->have_video) {
                if (!got_key) {
                    if (!p->keyframe) { pkt_unref(p); goto after_pkt; }
                    got_key = 1;
                }
                if (vc==MS_VC_H265) rtp_send_h265(&s->vtrack,p->data,p->len,p->pts_us);
                else                rtp_send_h264(&s->vtrack,p->data,p->len,p->pts_us);
            } else if (p->media==MS_MEDIA_AUDIO && s->have_audio) {
                if (ac==MS_AC_AAC) rtp_send_aac(&s->atrack,p->data,p->len,p->pts_us);
                else               rtp_send_g711(&s->atrack,p->data,p->len,p->pts_us);
            }
            pkt_unref(p);
        }
    after_pkt:;
        int64_t now = ms_now_us();
        if (s->have_video) rtp_maybe_sr(&s->vtrack, now);
        if (s->have_audio) rtp_maybe_sr(&s->atrack, now);

        /* poll control socket for TEARDOWN/keepalive/close */
        int n = recv(s->fd, ctl, sizeof(ctl)-1, MSG_DONTWAIT);
        if (n==0) break;                         /* peer closed */
        if (n>0) {
            ctl[n]=0;
            if (ctl[0]=='$') { /* interleaved RTCP from client - ignore */ }
            else if (!strncmp(ctl,"TEARDOWN",8)) { break; }
            else if (!strncmp(ctl,"GET_PARAMETER",13)||!strncmp(ctl,"OPTIONS",7)) {
                int cseq=hdr_int(ctl,"CSeq",0);
                char e[64]; snprintf(e,sizeof e,"Session: %s\r\n",s->session);
                send_resp(s->fd,cseq,e,NULL);
            }
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
        net_sendall(s->fd, e503, (int)strlen(e503));
    }
    LOGW(MOD,"subscribe failed (source full), closing session=%s", s->session);
}

static void *client_thread(void *arg)
{
    session *s = (session*)arg;
    net_set_nodelay(s->fd);
    char buf[4096];
    int have=0, playing=0;

    /* control phase: read requests until PLAY */
    while (!playing) {
        int n = recv(s->fd, buf+have, sizeof(buf)-1-have, 0);
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
    close(s->fd);
    if (s->v_udp[0]>0) close(s->v_udp[0]);
    if (s->v_udp[1]>0) close(s->v_udp[1]);
    if (s->a_udp[0]>0) close(s->a_udp[0]);
    if (s->a_udp[1]>0) close(s->a_udp[1]);
    free(s);
    __sync_fetch_and_sub(&g_nclients, 1);
    return NULL;
}

static void *accept_thread(void *arg)
{
    rtsp_server *sv = (rtsp_server*)arg;
    LOGI(MOD,"listening on port %d", sv->cfg->rtsp_port);
    while (sv->run) {
        struct sockaddr_in peer; socklen_t pl=sizeof peer;
        int cfd = accept(sv->lfd, (struct sockaddr*)&peer, &pl);
        if (cfd<0){ if(sv->run) usleep(50000); continue; }
        /* global client cap: each client costs a thread + bounded queue */
        if (g_nclients >= RTSP_MAX_CLIENTS) {
            const char *e503 = "RTSP/1.0 503 Service Unavailable\r\n\r\n";
            net_sendall(cfd, e503, (int)strlen(e503));
            close(cfd);
            LOGW(MOD,"client limit (%d) reached, rejecting", RTSP_MAX_CLIENTS);
            continue;
        }
        session *s = (session*)calloc(1,sizeof(session));
        if (!s){ close(cfd); continue; }
        s->fd=cfd; s->peer=peer; s->cfg=sv->cfg; s->vchn=-1;
        __sync_fetch_and_add(&g_nclients, 1);
        pthread_t t;
        if (pthread_create(&t,NULL,client_thread,s)==0) pthread_detach(t);
        else { close(cfd); free(s); __sync_fetch_and_sub(&g_nclients, 1); }
    }
    return NULL;
}

rtsp_server *rtsp_start(const ms_config *cfg)
{
    rtsp_server *s = (rtsp_server*)calloc(1,sizeof(*s));
    s->cfg = cfg;
    s->lfd = net_listen_tcp(cfg->rtsp_port, 8);
    if (s->lfd < 0){ LOGE(MOD,"cannot bind rtsp port %d",cfg->rtsp_port); free(s); return NULL; }
    s->run = 1;
    pthread_create(&s->thr, NULL, accept_thread, s);
    return s;
}

void rtsp_stop(rtsp_server *s)
{
    if (!s) return;
    s->run = 0;
    close(s->lfd);
    pthread_join(s->thr, NULL);
    free(s);
}
