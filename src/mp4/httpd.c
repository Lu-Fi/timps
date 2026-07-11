#include "httpd.h"
#include "fmp4.h"
#include "../net.h"
#include "../hub.h"
#include "../log.h"
#include "../util.h"
#include "../codec/aac.h"
#include "../auth.h"
#ifdef USE_CONTROL
#include "../control.h"
#endif
#include <stdio.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define MOD "HTTP"

/* global limit on concurrent HTTP connections (each costs a thread plus a
 * bounded fanqueue) so many/slow clients cannot exhaust memory (DoS) */
#ifndef HTTP_MAX_CLIENTS
#define HTTP_MAX_CLIENTS 8
#endif
/* fanqueue capacity for fMP4 streaming clients (pointers; retained packet
 * payloads are the real cost -> keep this bounded and modest) */
#ifndef MS_MP4_QCAP
#define MS_MP4_QCAP 64
#endif

static volatile int g_nconn;   /* current connection count (sync builtins) */

struct httpd {
    const ms_config *cfg;
    int              lfd;
    pthread_t        thr;
    volatile int     run;
};

typedef struct { int fd; const ms_config *cfg; int local; } hconn;

/* the player HTML is split so the correct MSE codec string (derived from the
 * live SPS) can be injected between HEAD and TAIL at request time */
static const char *PLAYER_HEAD =
"<!doctype html><html><head><meta charset=utf-8><title>timps</title>"
"<style>body{background:#111;color:#eee;font-family:sans-serif;text-align:center;margin:0}"
"#wrap{position:relative;display:inline-block;margin-top:1em}"
"video{max-width:96%;background:#000;display:block}"
/* embed mode (in an iframe): no chrome, video fills the frame */
"body.embed #wrap{margin:0;display:block}body.embed video{max-width:100%;width:100%}"
"</style></head>";
static const char *PLAYER_TAIL =
"if(window.MediaSource&&MediaSource.isTypeSupported(mime)){"
"const ms=new MediaSource();v.src=URL.createObjectURL(ms);"
"ms.addEventListener('sourceopen',async()=>{"
"let sb;try{sb=ms.addSourceBuffer(mime);}catch(e){v.src=src;return;}"
"sb.mode='sequence';let dead=false;const q=[];let busy=false;"
"const stop=()=>{dead=true;try{if(ms.readyState==='open')ms.endOfStream();}catch(e){}};"
/* drop buffered data older than ~10s behind playback so the SourceBuffer
 * never fills up during a long-running live stream */
"const evict=()=>{try{if(sb.buffered.length){const s=sb.buffered.start(0),e=v.currentTime-10;"
"if(e>s+4&&!sb.updating)sb.remove(s,e);}}catch(e){}};"
"const pump=()=>{if(dead||busy||sb.updating||ms.readyState!=='open'||!q.length)return;"
"busy=true;const c=q[0];try{sb.appendBuffer(c);q.shift();}"
"catch(e){busy=false;if(e.name==='QuotaExceededError')evict();else stop();}};"
"sb.addEventListener('updateend',()=>{busy=false;"
"if(v.buffered.length){if(v.currentTime<v.buffered.start(0))v.currentTime=v.buffered.start(0);"
/* keep close to the live edge; if we drift >6s behind, jump forward */
"const end=v.buffered.end(v.buffered.length-1);if(end-v.currentTime>6)v.currentTime=end-1;}"
"evict();pump();});"
"sb.addEventListener('error',stop);"
"const res=await fetch(src);const rd=res.body.getReader();"
"try{while(true){const{done,value}=await rd.read();if(done||dead)break;q.push(value);pump();}}"
"catch(e){}stop();"
"});}else{v.src=src;}"
"</script></body></html>";

static void http_send(int fd, const char *status, const char *ctype,
                      const char *body, int bodylen)
{
    char hdr[512];
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
        "Cache-Control: no-cache\r\nConnection: close\r\n\r\n",
        status, ctype, bodylen);
    net_sendall(fd, hdr, n);
    if (body && bodylen) net_sendall(fd, body, bodylen);
}

static void stream_mp4(hconn *c, int chn)
{
    const ms_config *cfg = c->cfg;
    if (chn<0 || chn>=MS_MAX_VSTREAM || !cfg->video[chn].enabled) chn = 0;

    fanqueue q;
    if (fanqueue_init(&q, MS_MP4_QCAP)) return;
    if (hub_subscribe(chn, &q) != 0) {           /* source full (>HUB_MAX_SUBS) */
        http_send(c->fd,"503 Service Unavailable","text/plain","busy",4);
        fanqueue_free(&q);
        return;
    }
    /* fMP4 can only carry AAC; use it only if the HAL actually produces AAC */
    int acodec=MS_AC_NONE, asr=0, ach=0;
    hub_get_audio(&acodec, &asr, &ach);
    int can_audio = (acodec==MS_AC_AAC);
    if (can_audio && hub_subscribe(HUB_AUDIO_SRC, &q) != 0)
        can_audio = 0;                           /* degrade to video-only */
    hub_request_idr(chn);

    /* wait for parameter sets */
    fmp4_mux mux; fmp4_init(&mux);
    mux.has_video = 1;
    mux.vcodec = cfg->video[chn].codec;
    mux.width  = cfg->video[chn].width;
    mux.height = cfg->video[chn].height;
    mux.fps    = cfg->video[chn].fps;
    int ok=0;
    for (int i=0;i<200;i++){
        vparam vp;
        if (hub_get_vparam(chn,&vp) && vparam_ready(&vp)){ mux.vp=vp; mux.vp_ready=1; ok=1; break; }
        usleep(10000);
    }
    if (!ok){ LOGW(MOD,"no video params, abort mp4"); goto out; }

    /* Only declare an audio track if AAC frames are actually flowing. A track
     * that is announced in moov but never fed makes browsers stall the whole
     * presentation (video freezes waiting for audio). Warm up briefly and
     * commit to audio only once we've seen a real AAC packet; discard whatever
     * we pop here (we re-request an IDR afterwards). */
    int want_audio = 0;
    if (can_audio) {
        for (int i=0;i<80 && !want_audio;i++){          /* up to ~800 ms */
            ms_pkt *p = fanqueue_pop(&q, 10);
            if (!p) continue;
            if (p->media==MS_MEDIA_AUDIO) want_audio = 1;
            pkt_unref(p);
        }
        if (!want_audio) LOGW(MOD,"no AAC within warmup -> video-only mp4");
    }
    if (want_audio) {
        mux.has_audio = 1;
        mux.a_timescale = asr;
        mux.a_channels  = ach;
        aac_asc(asr, ach, mux.asc);
    } else if (can_audio) {
        hub_unsubscribe(HUB_AUDIO_SRC, &q);             /* stop pulling audio */
        can_audio = 0;
    }
    hub_request_idr(chn);                               /* fresh keyframe after warmup */

    /* HTTP response headers (streamed body, no length) */
    const char *rh =
        "HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\n"
        "Cache-Control: no-cache\r\nConnection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n\r\n";
    if (net_sendall(c->fd, rh, strlen(rh))<0) goto out;

    ms_buf seg; ms_buf_init(&seg, 4096);
    fmp4_init_segment(&mux, &seg);
    if (net_sendall(c->fd, seg.data, seg.len)<0){ ms_buf_free(&seg); goto out; }
    ms_buf_free(&seg);
    LOGI(MOD,"mp4 client streaming chn=%d",chn);

    int got_key=0;
    /* blocking socket: net_sendall must never write a partial fragment */
    while (1) {
        ms_pkt *p = fanqueue_pop(&q, 200);
        if (!p) {
            char t[8]; int n=recv(c->fd,t,sizeof t,MSG_DONTWAIT);
            if (n==0) break;
            continue;
        }
        /* if the queue overflowed and dropped a keyframe, ask for a fresh IDR
         * so the client doesn't decode garbage until the next GOP */
        if (fanqueue_take_dropped_key(&q)) hub_request_idr(chn);
        ms_buf frag; ms_buf_init(&frag, p->len+256);
        if (p->media==MS_MEDIA_VIDEO) {
            if (!got_key){ if(!p->keyframe){ pkt_unref(p); ms_buf_free(&frag); continue; } got_key=1; }
            fmp4_video_fragment(&mux, p->data, p->len, p->keyframe, p->pts_us, &frag);
        } else if (want_audio && got_key) {
            fmp4_audio_fragment(&mux, p->data, p->len, p->pts_us, &frag);
        }
        int rc = 0;
        if (frag.len) rc = net_sendall(c->fd, frag.data, frag.len);
        ms_buf_free(&frag);
        pkt_unref(p);
        if (rc<0) break;
    }
out:
    hub_unsubscribe(chn, &q);
    if (can_audio) hub_unsubscribe(HUB_AUDIO_SRC, &q);
    fanqueue_free(&q);
}

/* JPEG source selection for /snapshot.jpg and /stream.mjpeg:
 *   no ?chn=  -> dedicated jpeg.* channel (fallback: first videoN.jpeg)
 *   ?chn=N    -> JPEG encoder piggybacked on video stream N (videoN.jpeg=true)
 * returns a hub source id or -1 if nothing suitable is enabled */
static int jpeg_src_from_path(const char *path, const ms_config *cfg)
{
    const char *q = strstr(path, "chn=");
    if (q) {
        int n = atoi(q+4);
        if (n>=0 && n<MS_MAX_VSTREAM && cfg->video[n].enabled && cfg->video[n].jpeg_enabled)
            return HUB_JPEG_SRC_N(n);
        return -1;
    }
    if (cfg->jpeg.enabled) return HUB_JPEG_SRC;
    for (int i=0;i<MS_MAX_VSTREAM;i++)
        if (cfg->video[i].enabled && cfg->video[i].jpeg_enabled)
            return HUB_JPEG_SRC_N(i);
    return -1;
}

/* single latest JPEG snapshot */
static void snapshot_jpg(hconn *c, int src)
{
    if (src < 0){ http_send(c->fd,"404 Not Found","text/plain","no jpeg",7); return; }
    fanqueue q;
    if (fanqueue_init(&q, 4)) return;
    if (hub_subscribe(src, &q) != 0) {
        http_send(c->fd,"503 Service Unavailable","text/plain","busy",4);
        fanqueue_free(&q);
        return;
    }
    ms_pkt *p = fanqueue_pop(&q, 3000);
    if (p) {
        char hdr[160];
        int n=snprintf(hdr,sizeof hdr,
            "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n"
            "Cache-Control: no-cache\r\nConnection: close\r\n\r\n", p->len);
        if (net_sendall(c->fd,hdr,n)>=0) net_sendall(c->fd,p->data,(int)p->len);
        pkt_unref(p);
    } else {
        http_send(c->fd,"503 Unavailable","text/plain","no frame",8);
    }
    hub_unsubscribe(src, &q);
    fanqueue_free(&q);
}

/* MJPEG multipart stream. 'bnd' overrides the multipart boundary (so it matches
 * what a web-UI CGI/proxy already announced, e.g. the thingino preview). */
static void stream_mjpeg(hconn *c, int src, const char *bnd)
{
    if (src < 0){ http_send(c->fd,"404 Not Found","text/plain","no jpeg",7); return; }
    char BND[64];
    if (bnd && bnd[0]){
        int i=0; for (; bnd[i] && bnd[i]!='&' && i<(int)sizeof(BND)-1; i++){
            char ch=bnd[i];
            BND[i] = ((ch>='A'&&ch<='Z')||(ch>='a'&&ch<='z')||(ch>='0'&&ch<='9')||ch=='-'||ch=='_') ? ch : '_';
        }
        BND[i]=0;
        if (!BND[0]) snprintf(BND,sizeof BND,"msmjpeg");
    } else snprintf(BND,sizeof BND,"msmjpeg");

    /* subscribe BEFORE sending headers so a full source can answer 503 */
    fanqueue q;
    if (fanqueue_init(&q,8)) return;
    if (hub_subscribe(src, &q) != 0) {   /* too many subscribers */
        http_send(c->fd,"503 Service Unavailable","text/plain","busy",4);
        fanqueue_free(&q);
        return;
    }
    char rh[256];
    int n=snprintf(rh,sizeof rh,
        "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=%s\r\n"
        "Cache-Control: no-cache\r\nConnection: close\r\n\r\n", BND);
    if (net_sendall(c->fd,rh,n)<0){ hub_unsubscribe(src,&q); fanqueue_free(&q); return; }
    LOGI(MOD,"mjpeg client streaming");
    while (1) {
        ms_pkt *p = fanqueue_pop(&q, 500);
        if (!p) {
            char t[8]; int r=recv(c->fd,t,sizeof t,MSG_DONTWAIT);
            if (r==0) break;
            continue;
        }
        char part[160];
        int hn=snprintf(part,sizeof part,
            "\r\n--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n",
            BND, p->len);
        int rc = net_sendall(c->fd,part,hn);
        if (rc>=0) rc = net_sendall(c->fd,p->data,(int)p->len);
        pkt_unref(p);
        if (rc<0) break;
    }
    hub_unsubscribe(src, &q);
    fanqueue_free(&q);
}

/* returns 1 if the request carries valid Basic credentials (or auth disabled) */
static int http_check_auth(const ms_config *cfg, const char *buf)
{
    const char *user = cfg->http_user[0] ? cfg->http_user : cfg->rtsp_user;
    const char *pass = cfg->http_user[0] ? cfg->http_pass : cfg->rtsp_pass;
    if (!user[0]) return 1;                        /* auth disabled */
    /* find Authorization header (case-insensitive) */
    const char *p = buf;
    while (*p) {
        if (strncasecmp(p,"Authorization:",14)==0) {
            p+=14; while(*p==' ')p++;
            char line[512]; int i=0;
            while (*p && *p!='\r' && *p!='\n' && i<(int)sizeof(line)-1) line[i++]=*p++;
            line[i]=0;
            return auth_http_basic(line, user, pass);
        }
        const char *e=strchr(p,'\n'); if(!e)break; p=e+1;
    }
    return 0;
}

/* extract ?chn=N from the request path; returns def if absent/invalid */
static int path_chn(const char *path, const ms_config *cfg, int def)
{
    const char *q = strstr(path, "chn=");
    if (q){ int n=atoi(q+4); if(n>=0 && n<MS_MAX_VSTREAM && cfg->video[n].enabled) return n; }
    return def;
}

/* serve the preview page with an MSE codec string derived from the live SPS */
static void serve_player(hconn *c, const char *path)
{
    const ms_config *cfg = c->cfg;
    int chn = path_chn(path, cfg, cfg->http_preview_chn);
    if (chn<0||chn>=MS_MAX_VSTREAM||!cfg->video[chn].enabled) chn=0;

    char vcodec[24] = "avc1.640028";               /* High@4.0 fallback */
    hub_request_idr(chn);
    for (int i=0;i<100;i++){
        vparam vp;
        if (hub_get_vparam(chn,&vp) && vparam_ready(&vp)){
            if (vp.codec==MS_VC_H264 && vp.sps_len>=4)
                snprintf(vcodec,sizeof vcodec,"avc1.%02X%02X%02X",vp.sps[1],vp.sps[2],vp.sps[3]);
            else if (vp.codec==MS_VC_H265)
                snprintf(vcodec,sizeof vcodec,"hvc1.1.6.L123.B0");
            break;
        }
        usleep(10000);
    }
    int acodec=MS_AC_NONE; hub_get_audio(&acodec,NULL,NULL);
    const char *aud = (acodec==MS_AC_AAC) ? ", mp4a.40.2" : "";

    /* embed mode: bare video to drop into an iframe (e.g. the thingino web UI).
     * No heading/chrome - the host page provides the surrounding UI. */
    int embed = (strstr(path,"embed") != NULL);

    char html[4096];
    int n = snprintf(html, sizeof html,
        "%s<body class=\"%s\">%s"
        "<div id=wrap><video id=v autoplay muted controls playsinline></video></div>"
        "<script>const v=document.getElementById('v');"
        "const mime='video/mp4; codecs=\"%s%s\"';const src='/stream.mp4?chn=%d';%s",
        PLAYER_HEAD,
        embed?"embed":"",
        embed?"":"<h3>timps preview</h3>",
        vcodec, aud, chn, PLAYER_TAIL);
    if (n>=(int)sizeof html) n=sizeof(html)-1;
    http_send(c->fd,"200 OK","text/html",html,n);
}

static void *conn_thread(void *arg)
{
    hconn *c = (hconn*)arg;
#ifdef USE_CONTROL
    char buf[4096];               /* room for a full nested /control JSON body */
#else
    char buf[1024];
#endif
    int n = recv(c->fd, buf, sizeof(buf)-1, 0);
    if (n>0) {
        buf[n]=0;
        char method[8], path[256];
        if (!c->local && !http_check_auth(c->cfg, buf)) {
            const char *r="HTTP/1.1 401 Unauthorized\r\n"
                "WWW-Authenticate: Basic realm=\"" AUTH_REALM "\"\r\n"
                "Content-Length: 12\r\nConnection: close\r\n\r\nUnauthorized";
            net_sendall(c->fd, r, (int)strlen(r));
            close(c->fd); free(c);
            __sync_fetch_and_sub(&g_nconn, 1);
            return NULL;
        }
        if (sscanf(buf,"%7s %255s",method,path)==2) {
            if (!strcmp(path,"/") || !strncmp(path,"/?",2) || !strncmp(path,"/index.html",11))
                serve_player(c, path);
            else if (!strncmp(path,"/stream.mp4",11))
                stream_mp4(c, path_chn(path, c->cfg, c->cfg->http_preview_chn));
            else if (!strncmp(path,"/snapshot.jpg",13))
                snapshot_jpg(c, jpeg_src_from_path(path, c->cfg));
            else if (!strncmp(path,"/stream.mjpeg",13)||!strncmp(path,"/mjpeg",6)){
                const char *b=strstr(path,"boundary="); if(b) b+=9;
                stream_mjpeg(c, jpeg_src_from_path(path, c->cfg), b);
            }
#ifdef USE_CONTROL
            else if (!strncmp(path,"/control",8)) {
                /* live settings: localhost always; remote only when credentials
                 * are configured (then Basic auth was already enforced above) */
                const char *user = c->cfg->http_user[0] ? c->cfg->http_user
                                                        : c->cfg->rtsp_user;
                if (!c->local && !user[0])
                    http_send(c->fd,"403 Forbidden","text/plain","local only",10);
                else if (!strcmp(method,"GET")) {
                    char js[4096];   /* worst case: 8 OSD items with long texts */
                    int jn = control_get_json(js, sizeof js);
                    http_send(c->fd,"200 OK","application/json",js,jn);
                } else {
                    char *body = strstr(buf,"\r\n\r\n");
                    if (body) {
                        body += 4;
                        /* body may arrive split: finish per Content-Length */
                        int have = n - (int)(body - buf), clen = 0;
                        const char *cl = strcasestr(buf,"Content-Length:");
                        if (cl) clen = atoi(cl+15);
                        if (clen > (int)sizeof(buf)) clen = (int)sizeof(buf);
                        while (have < clen && n < (int)sizeof(buf)-1) {
                            int r = recv(c->fd, buf+n, sizeof(buf)-1-n, 0);
                            if (r <= 0) break;
                            n += r; have += r; buf[n] = 0;
                        }
                    }
                    control_apply_json(body ? body : "");
                    http_send(c->fd,"200 OK","application/json","{\"ok\":true}",11);
                }
            }
#endif
            else
                http_send(c->fd,"404 Not Found","text/plain","not found",9);
        }
    }
    close(c->fd);
    free(c);
    __sync_fetch_and_sub(&g_nconn, 1);
    return NULL;
}

static void *accept_thread(void *arg)
{
    httpd *h = (httpd*)arg;
    LOGI(MOD,"listening on port %d", h->cfg->http_port);
    while (h->run) {
        struct sockaddr_in peer; socklen_t pl=sizeof peer;
        int fd = accept(h->lfd,(struct sockaddr*)&peer,&pl);
        if (fd<0){ if(h->run) usleep(50000); continue; }
        /* global connection cap: each client costs a thread + queue */
        if (g_nconn >= HTTP_MAX_CLIENTS) {
            const char *r="HTTP/1.1 503 Service Unavailable\r\n"
                          "Content-Length: 4\r\nConnection: close\r\n\r\nbusy";
            net_sendall(fd, r, (int)strlen(r));
            close(fd);
            LOGW(MOD,"connection limit (%d) reached, rejecting client",HTTP_MAX_CLIENTS);
            continue;
        }
        hconn *c = (hconn*)calloc(1,sizeof(hconn));
        if (!c){ close(fd); continue; }
        c->fd=fd; c->cfg=h->cfg;
        /* loopback (127.0.0.0/8) clients skip auth: the local web UI must always
         * be able to reach the streamer, external clients still need the
         * password. This replaces prudynt's "web UI auth key". */
        c->local = ((ntohl(peer.sin_addr.s_addr) & 0xFF000000u) == 0x7F000000u);
        __sync_fetch_and_add(&g_nconn, 1);
        pthread_t t;
        if (pthread_create(&t,NULL,conn_thread,c)==0) pthread_detach(t);
        else { close(fd); free(c); __sync_fetch_and_sub(&g_nconn, 1); }
    }
    return NULL;
}

httpd *httpd_start(const ms_config *cfg)
{
    httpd *h = (httpd*)calloc(1,sizeof(*h));
    h->cfg=cfg;
    h->lfd=net_listen_tcp(cfg->http_port, 8);
    if (h->lfd<0){ LOGE(MOD,"cannot bind http port %d",cfg->http_port); free(h); return NULL; }
    h->run=1;
    pthread_create(&h->thr,NULL,accept_thread,h);
    return h;
}

void httpd_stop(httpd *h)
{
    if (!h) return;
    h->run=0; close(h->lfd);
    pthread_join(h->thr,NULL);
    free(h);
}
