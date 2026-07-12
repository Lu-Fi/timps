#include "httpd.h"
#include "fmp4.h"
#include "../net.h"
#include "../hub.h"
#include "../log.h"
#include "../util.h"
#include "../codec/aac.h"
#include "../auth.h"
#include "../tls.h"
#ifdef USE_CONTROL
#include "../control.h"
#include "../daynight.h"
#include "../events.h"
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
    void            *tls_ctx;   /* ms_tls_ctx* when http.https (USE_TLS), else NULL */
};

typedef struct { int fd; const ms_config *cfg; int local; void *tls; void *tls_ctx; } hconn;

/* connection I/O that transparently uses TLS when this is an HTTPS connection
 * (c->tls set), otherwise the plain socket. Without USE_TLS these are exactly
 * the old net_sendall(c->fd,...) / recv(c->fd,...) calls. */
static int csend(hconn *c, const void *buf, int len)
{
#ifdef USE_TLS
    if (c->tls) return ms_tls_write((ms_tls_conn *)c->tls, buf, len);
#endif
    return net_sendall(c->fd, buf, len);
}
static int crecv(hconn *c, void *buf, int len, int flags)
{
#ifdef USE_TLS
    if (c->tls) {
        /* no cheap non-blocking peek over TLS: report "no data" (-1, like a
         * plain recv EAGAIN), NOT 0 - the streaming loops treat 0 as orderly
         * close and would drop live TLS clients on every idle poll */
        if (flags & MSG_DONTWAIT) return -1;
        return ms_tls_read((ms_tls_conn *)c->tls, buf, len);
    }
#endif
    return recv(c->fd, buf, len, flags);
}

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

/* like http_send but with extra response headers ("Name: v\r\n" lines),
 * used to attach the CORS set to /control responses */
static void http_send_ex(hconn *c, const char *status, const char *ctype,
                         const char *extra, const char *body, int bodylen)
{
    char hdr[1024];
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n%s"
        "Cache-Control: no-cache\r\nConnection: close\r\n\r\n",
        status, ctype, bodylen, extra ? extra : "");
    if (n >= (int)sizeof hdr) return;      /* never send a truncated header */
    csend(c, hdr, n);
    if (body && bodylen) csend(c, body, bodylen);
}

static void http_send(hconn *c, const char *status, const char *ctype,
                      const char *body, int bodylen)
{
    http_send_ex(c, status, ctype, "", body, bodylen);
}

/* CORS header for the media endpoints (/stream.mp4, /stream.mjpeg,
 * /snapshot.jpg): '*' is safe here because media auth never relies on
 * ambient browser credentials - access is localhost, the ?token= query
 * (which any origin the user gave the token to may use) or explicit Basic.
 * Sent unconditionally: harmless without an Origin, and it keeps the
 * fMP4/MJPEG/snapshot responses fetch()able cross-origin (the WebUI MSE
 * preview loads /stream.mp4 via fetch). */
#define MEDIA_CORS "Access-Control-Allow-Origin: *\r\n"

static void stream_mp4(hconn *c, int chn)
{
    const ms_config *cfg = c->cfg;
    if (chn<0 || chn>=MS_MAX_VSTREAM || !cfg->video[chn].enabled) chn = 0;

    fanqueue q;
    if (fanqueue_init(&q, MS_MP4_QCAP)) return;
    if (hub_subscribe(chn, &q) != 0) {           /* source full (>HUB_MAX_SUBS) */
        http_send_ex(c,"503 Service Unavailable","text/plain",MEDIA_CORS,"busy",4);
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
        MEDIA_CORS "\r\n";
    if (csend(c, rh, strlen(rh))<0) goto out;

    ms_buf seg;
    if (ms_buf_init(&seg, 4096)) goto out;
    fmp4_init_segment(&mux, &seg);
    if (csend(c, seg.data, seg.len)<0){ ms_buf_free(&seg); goto out; }
    ms_buf_free(&seg);
    LOGI(MOD,"mp4 client streaming chn=%d",chn);

    int got_key=0;
    /* blocking socket: net_sendall must never write a partial fragment */
    while (1) {
        ms_pkt *p = fanqueue_pop(&q, 200);
        if (!p) {
            char t[8]; int n=crecv(c,t,sizeof t,MSG_DONTWAIT);
            if (n==0) break;
            continue;
        }
        /* if the queue overflowed and dropped a keyframe, ask for a fresh IDR
         * so the client doesn't decode garbage until the next GOP */
        if (fanqueue_take_dropped_key(&q)) hub_request_idr(chn);
        ms_buf frag;
        if (ms_buf_init(&frag, p->len+256)){ pkt_unref(p); break; }  /* OOM */
        if (p->media==MS_MEDIA_VIDEO) {
            if (!got_key){ if(!p->keyframe){ pkt_unref(p); ms_buf_free(&frag); continue; } got_key=1; }
            fmp4_video_fragment(&mux, p->data, p->len, p->keyframe, p->pts_us, &frag);
        } else if (want_audio && got_key) {
            fmp4_audio_fragment(&mux, p->data, p->len, p->pts_us, &frag);
        }
        int rc = 0;
        if (frag.len) rc = csend(c, frag.data, frag.len);
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
    if (src < 0){ http_send_ex(c,"404 Not Found","text/plain",MEDIA_CORS,"no jpeg",7); return; }
    fanqueue q;
    if (fanqueue_init(&q, 4)) return;
    if (hub_subscribe(src, &q) != 0) {
        http_send_ex(c,"503 Service Unavailable","text/plain",MEDIA_CORS,"busy",4);
        fanqueue_free(&q);
        return;
    }
    ms_pkt *p = fanqueue_pop(&q, 3000);
    if (p) {
        char hdr[224];
        int n=snprintf(hdr,sizeof hdr,
            "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n"
            "Cache-Control: no-cache\r\nConnection: close\r\n" MEDIA_CORS "\r\n", p->len);
        if (csend(c,hdr,n)>=0) csend(c,p->data,(int)p->len);
        pkt_unref(p);
    } else {
        http_send_ex(c,"503 Unavailable","text/plain",MEDIA_CORS,"no frame",8);
    }
    hub_unsubscribe(src, &q);
    fanqueue_free(&q);
}

/* MJPEG multipart stream. 'bnd' overrides the multipart boundary (so it matches
 * what a web-UI CGI/proxy already announced, e.g. the thingino preview). */
static void stream_mjpeg(hconn *c, int src, const char *bnd)
{
    if (src < 0){ http_send_ex(c,"404 Not Found","text/plain",MEDIA_CORS,"no jpeg",7); return; }
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
        http_send_ex(c,"503 Service Unavailable","text/plain",MEDIA_CORS,"busy",4);
        fanqueue_free(&q);
        return;
    }
    char rh[288];
    int n=snprintf(rh,sizeof rh,
        "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=%s\r\n"
        "Cache-Control: no-cache\r\nConnection: close\r\n" MEDIA_CORS "\r\n", BND);
    if (csend(c,rh,n)<0){ hub_unsubscribe(src,&q); fanqueue_free(&q); return; }
    LOGI(MOD,"mjpeg client streaming");
    while (1) {
        ms_pkt *p = fanqueue_pop(&q, 500);
        if (!p) {
            char t[8]; int r=crecv(c,t,sizeof t,MSG_DONTWAIT);
            if (r==0) break;
            continue;
        }
        char part[160];
        int hn=snprintf(part,sizeof part,
            "\r\n--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n",
            BND, p->len);
        int rc = csend(c,part,hn);
        if (rc>=0) rc = csend(c,p->data,(int)p->len);
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

#ifdef USE_CONTROL
/* copy the value of request header 'name' (give it WITH the trailing ':')
 * into out; case-insensitive, matched at line starts only. Returns 1 if
 * found. The value is cut at CR/LF, so it can never inject headers when
 * reflected into a response. Only the /control+/events token/CORS helpers
 * below use this, hence the USE_CONTROL guard. */
static int http_header(const char *buf, const char *name, char *out, int cap)
{
    size_t nl = strlen(name);
    const char *p = buf;
    while (*p) {
        if (strncasecmp(p, name, nl)==0) {
            p += nl; while (*p==' ') p++;
            int i=0;
            while (*p && *p!='\r' && *p!='\n' && i<cap-1) out[i++]=*p++;
            out[i]=0;
            return 1;
        }
        const char *e=strchr(p,'\n'); if(!e)break; p=e+1;
    }
    return 0;
}

/* token auth for /control, /events and the HTTP media endpoints: the token
 * travels as "X-Timps-Token:" header or as ?token= in the query string
 * (header preferred - a query token can end up in proxy/access logs; <img>/
 * <video src>/EventSource can only use the query form). Valid tokens: the
 * random per-boot g_ctl_token (published only to local privileged readers
 * via http.token_file) and the optional persistent http.token secret.
 * Constant-time comparison. The token never unlocks RTSP. */
static int http_check_token(const ms_config *cfg, const char *buf, const char *path)
{
    char tok[128];
    if (!http_header(buf, "X-Timps-Token:", tok, sizeof tok)) {
        const char *q = strstr(path, "token=");
        if (!q) return 0;
        q += 6;
        int i=0;
        while (q[i] && q[i]!='&' && i<(int)sizeof(tok)-1){ tok[i]=q[i]; i++; }
        tok[i]=0;
    }
    if (!tok[0]) return 0;
    if (g_ctl_token[0]     && auth_token_eq(tok, g_ctl_token))     return 1;
    if (cfg->http_token[0] && auth_token_eq(tok, cfg->http_token)) return 1;
    return 0;
}

/* CORS header set for /control responses: reflect the request's Origin (a
 * cross-origin page cannot read the token file nor the token, so reflecting
 * does not hand control to a malicious origin; '*' is deliberately avoided
 * once a token is in play). No Allow-Credentials: auth is the token header
 * (or Basic typed into the request), never ambient cookies. */
static int http_cors(const char *buf, char *out, int cap)
{
    char origin[256];
    out[0]=0;
    if (!http_header(buf, "Origin:", origin, sizeof origin) || !origin[0])
        return 0;
    snprintf(out, (size_t)cap,
        "Access-Control-Allow-Origin: %s\r\n"
        "Vary: Origin\r\n"
        "Access-Control-Allow-Headers: X-Timps-Token, Content-Type\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Max-Age: 600\r\n", origin);
    return 1;
}

/* ---------------- GET /events: Server-Sent-Events push ----------------
 * Long-lived text/event-stream (same open-socket pattern as /stream.mjpeg)
 * that PUSHES JSON state instead of being polled. Event types:
 *   motion   - the /control "motion" status object (grid + active cells)
 *   daynight - the /control "daynight" status object (mode/brightness/gain)
 *   stats    - {"uptime_s","clients","video":[{"chn","subs","fps"},..]}
 * ?stream=motion,daynight,stats selects types (default: all). Auth/CORS are
 * the /control rules (conn_thread); note EventSource cannot send headers, so
 * browsers pass the token as ?token= (accepted by http_check_token). Each
 * connection blocks on the events condvar (events_wait, woken by
 * imp_motion.c/daynight.c/control.c) and re-sends an event only when its
 * payload differs from what IT last sent (per-connection dedup snapshots),
 * plus the full state once on connect; stats tick every events.stats_ms.
 * A ": ping" comment goes out when nothing happened for a while so dead
 * clients are detected promptly and proxies keep the stream open. */

#ifndef EVENTS_KEEPALIVE_MS
#define EVENTS_KEEPALIVE_MS 12000
#endif

static volatile int g_nsse;      /* current /events connections (sync builtins) */
static int64_t g_start_us;       /* daemon start, for the stats uptime */

/* one "event: <type>\ndata: <json>\n\n" frame; <0 = client gone (write
 * error). Oversized payloads are dropped, never truncated - a cut data line
 * would poison the whole stream for the parser. */
static int sse_emit(hconn *c, const char *type, const char *json, int64_t *last_write)
{
    char frame[1280];            /* fits a max-grid motion event + headroom */
    int n = snprintf(frame, sizeof frame, "event: %s\ndata: %s\n\n", type, json);
    if (n >= (int)sizeof frame){ LOGW(MOD,"sse %s event too large, dropped",type); return 0; }
    int rc = csend(c, frame, n);
    if (rc >= 0) *last_write = ms_now_us();
    return rc;
}

/* the periodic "stats" payload: what timps actually tracks - subscriber
 * counts and the measured per-stream fps (the OSD {clients}/{fps} sources);
 * per-stream bitrate is not measured anywhere, so it is not invented here */
static int stats_json(const ms_config *cfg, char *buf, size_t cap)
{
    size_t o = 0;
    #define APP(...) do { \
        int _n = snprintf(buf+o, o<cap?cap-o:0, __VA_ARGS__); \
        if (_n>0) o += (size_t)_n; \
    } while (0)
    APP("{\"uptime_s\":%lld,\"clients\":%d,\"video\":[",
        (long long)((ms_now_us()-g_start_us)/1000000), hub_video_subs());
    int first = 1;
    for (int i=0;i<MS_MAX_VSTREAM;i++){
        if (!cfg->video[i].enabled) continue;
        APP("%s{\"chn\":%d,\"subs\":%d,\"fps\":%.1f}",
            first?"":",", i, hub_subs(i), hub_get_fps(i));
        first = 0;
    }
    APP("]}");
    #undef APP
    return (int)o;
}

static void events_stream(hconn *c, const char *path, const char *cors)
{
    const ms_config *cfg = c->cfg;

    /* ?stream= filter: absent = all; present = only the listed types */
    int want_motion = 1, want_dn = 1, want_stats = 1;
    const char *f = strstr(path, "stream=");
    if (f){
        char fl[80]; int i = 0;
        for (f += 7; f[i] && f[i] != '&' && i < (int)sizeof fl - 1; i++) fl[i] = f[i];
        fl[i] = 0;
        want_motion = strstr(fl, "motion")   != NULL;
        want_dn     = strstr(fl, "daynight") != NULL;
        want_stats  = strstr(fl, "stats")    != NULL;
    }

    /* own cap below the global HTTP_MAX_CLIENTS: a flood of /events
     * connections must not exhaust the per-connection threads (each SSE
     * client parks one thread until it disconnects) */
    int max = cfg->events_max_clients > 0 ? cfg->events_max_clients : 8;
    if (__sync_add_and_fetch(&g_nsse, 1) > max){
        __sync_fetch_and_sub(&g_nsse, 1);
        LOGW(MOD,"sse client limit (%d) reached, rejecting", max);
        http_send_ex(c,"503 Service Unavailable","text/plain",cors,"busy",4);
        return;
    }

    char hdr[768];
    int hn = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n"
        "X-Accel-Buffering: no\r\n%s\r\n", cors);
    if (hn >= (int)sizeof hdr || csend(c, hdr, hn) < 0) goto out;
    {   /* preamble: EventSource reconnect delay + a first-byte comment */
        static const char pre[] = "retry: 3000\n\n: connected\n\n";
        if (csend(c, pre, (int)sizeof pre - 1) < 0) goto out;
    }
    LOGI(MOD,"sse client streaming (%d/%d)", g_nsse, max);

    {
    ms_motion_status lm;                          /* last-sent snapshots */
    int have_m = 0, have_d = 0;
    int ld_en = 0, ld_mode = 0;
    float ld_b = 0.0f, ld_g = 0.0f;
    int stats_ms = cfg->events_stats_ms;
    int64_t next_stats = (want_stats && stats_ms > 0) ? ms_now_us() : INT64_MAX;
    int64_t last_write = ms_now_us();
    unsigned gen = events_generation();
    char js[1024];                                /* fits max_cells active[] */

    memset(&lm, 0, sizeof lm);
    for (;;){
        int rc = 0;
        if (want_motion){
            ms_motion_status m; motion_get_status(&m);
            /* dedup: emit only when the payload would differ; last_ms just
             * counts up, so it is deliberately NOT compared */
            if (!have_m || m.available != lm.available ||
                m.enabled != lm.enabled || m.cols != lm.cols ||
                m.rows != lm.rows || m.cells != lm.cells ||
                m.sensitivity != lm.sensitivity ||
                memcmp(m.active, lm.active, sizeof m.active)){
                lm = m; have_m = 1;
                if (control_motion_json(js, sizeof js, &m) < (int)sizeof js)
                    rc = sse_emit(c, "motion", js, &last_write);
            }
        }
        if (rc >= 0 && want_dn){
            int en = 0, mode = 0; float b = -1.0f, g = -1.0f;
            daynight_get_status(&en, &mode, &b, &g);
            float db = b - ld_b; if (db < 0) db = -db;
            float dg = g - ld_g; if (dg < 0) dg = -dg;
            /* change thresholds match the producer filter in daynight.c */
            if (!have_d || en != ld_en || mode != ld_mode || db >= 1.0f ||
                dg >= (ld_g > 0.0f ? ld_g * 0.05f : 8.0f)){
                ld_en = en; ld_mode = mode; ld_b = b; ld_g = g; have_d = 1;
                if (control_daynight_json(js, sizeof js, en, mode, b, g) < (int)sizeof js)
                    rc = sse_emit(c, "daynight", js, &last_write);
            }
        }
        if (rc >= 0 && ms_now_us() >= next_stats){
            next_stats = ms_now_us() + (int64_t)stats_ms * 1000;
            if (stats_json(cfg, js, sizeof js) < (int)sizeof js)
                rc = sse_emit(c, "stats", js, &last_write);
        }
        if (rc < 0) break;                        /* client gone (EPIPE) */

        /* block until a producer notifies or the stats/keepalive tick is
         * due; the timeout also bounds how long a dead client can linger */
        int wait_ms = (int)((last_write + EVENTS_KEEPALIVE_MS*1000LL - ms_now_us())/1000);
        if (next_stats != INT64_MAX){
            int sms = (int)((next_stats - ms_now_us())/1000);
            if (sms < wait_ms) wait_ms = sms;
        }
        if (wait_ms < 25) wait_ms = 25;           /* coalesce notify bursts */
        gen = events_wait(gen, wait_ms);

        /* detect an orderly close even while nothing is being pushed */
        char t[8]; int r = crecv(c, t, sizeof t, MSG_DONTWAIT);
        if (r == 0) break;
        if (ms_now_us() - last_write >= EVENTS_KEEPALIVE_MS*1000LL){
            if (csend(c, ": ping\n\n", 8) < 0) break;
            last_write = ms_now_us();
        }
    }
    }
out:
    __sync_fetch_and_sub(&g_nsse, 1);
    LOGI(MOD,"sse client disconnected (%d left)", g_nsse);
}
#endif /* USE_CONTROL */

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
    http_send(c,"200 OK","text/html",html,n);
}

static void *conn_thread(void *arg)
{
    hconn *c = (hconn*)arg;
#ifdef USE_TLS
    /* HTTPS: run the TLS handshake before any request I/O. From here on all
     * reads/writes go through crecv/csend, which use c->tls transparently. */
    if (c->tls_ctx) {
        c->tls = ms_tls_accept((ms_tls_ctx *)c->tls_ctx, c->fd);
        if (!c->tls) { close(c->fd); free(c); __sync_fetch_and_sub(&g_nconn, 1); return NULL; }
    }
#endif
#ifdef USE_CONTROL
    char buf[4096];               /* room for a full nested /control JSON body */
#else
    char buf[1024];
#endif
    int n = crecv(c, buf, sizeof(buf)-1, 0);
    if (n>0) {
        buf[n]=0;
        char method[8], path[256];
        if (sscanf(buf,"%7s %255s",method,path)==2) {
            /* /control + /events + media extras: CORS reflection + token
             * auth. tok_ok grants access to these paths ONLY (it is never
             * computed for others); everything else keeps the localhost/
             * Basic rules. /events and the media endpoints accept ?token=
             * because EventSource and <img>/<video src> cannot send custom
             * headers. */
            char cors[512]; cors[0]=0;
            int tok_ok = 0;
#ifdef USE_CONTROL
            /* media endpoints: the /control token also unlocks VIEWING here
             * (never RTSP), so the thingino WebUI preview <img>/players can
             * load straight from this port without on-device proxy CGIs.
             * localhost and the open-when-no-user rule stay as they are. */
            int media = !strncmp(path,"/stream.mp4",11)   ||
                        !strncmp(path,"/snapshot.jpg",13) ||
                        !strncmp(path,"/stream.mjpeg",13);
            if (media || !strncmp(path,"/control",8) || !strncmp(path,"/events",7)) {
                http_cors(buf, cors, sizeof cors);
                if (!strcmp(method,"OPTIONS")) {
                    /* CORS preflight: answered before any auth - a preflight
                     * carries no credentials by design. 204, no body. */
                    char r[768];
                    int rn = snprintf(r, sizeof r,
                        "HTTP/1.1 204 No Content\r\n%s"
                        "Content-Length: 0\r\nConnection: close\r\n\r\n", cors);
                    csend(c, r, rn);
                    goto done;
                }
                tok_ok = http_check_token(c->cfg, buf, path);
            }
#endif
            /* global gate: localhost, a valid token (tok_ok is only ever
             * set for /control, /events and the media endpoints), or Basic */
            if (!c->local && !tok_ok && !http_check_auth(c->cfg, buf)) {
                char r[768];
                int rn = snprintf(r, sizeof r,
                    "HTTP/1.1 401 Unauthorized\r\n"
                    "WWW-Authenticate: Basic realm=\"" AUTH_REALM "\"\r\n%s"
                    "Content-Length: 12\r\nConnection: close\r\n\r\nUnauthorized",
                    cors);
                csend(c, r, rn);
                goto done;
            }
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
                /* live settings - allowed for: localhost (on-device bridge
                 * CGIs), a valid token (tok_ok, checked above), or configured
                 * credentials (Basic already enforced by the global gate) */
                const char *user = c->cfg->http_user[0] ? c->cfg->http_user
                                                        : c->cfg->rtsp_user;
                if (!c->local && !tok_ok && !user[0])
                    http_send_ex(c,"403 Forbidden","text/plain",cors,"local only",10);
                else if (!strcmp(method,"GET")) {
                    /* worst case: caps + full image/audio/sensor blocks +
                     * 2 full video stream blocks + 2 per-stream OSD sets
                     * (2 x 8 items) with long texts + the motion status
                     * (up to MOTION_MAX_CELLS "active" entries) */
                    char js[18432];
                    int jn = control_get_json(js, sizeof js);
                    http_send_ex(c,"200 OK","application/json",cors,js,jn);
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
                            int r = crecv(c, buf+n, sizeof(buf)-1-n, 0);
                            if (r <= 0) break;
                            n += r; have += r; buf[n] = 0;
                        }
                    }
                    control_apply_json(body ? body : "");
                    http_send_ex(c,"200 OK","application/json",cors,"{\"ok\":true}",11);
                }
            }
            else if (!strncmp(path,"/events",7)) {
                /* SSE push stream - same access rules as /control:
                 * localhost, a valid token (tok_ok; EventSource passes it
                 * as ?token=), or configured credentials (Basic already
                 * enforced by the global gate) */
                const char *user = c->cfg->http_user[0] ? c->cfg->http_user
                                                        : c->cfg->rtsp_user;
                if (!c->cfg->events_enabled)
                    http_send_ex(c,"404 Not Found","text/plain",cors,"disabled",8);
                else if (!c->local && !tok_ok && !user[0])
                    http_send_ex(c,"403 Forbidden","text/plain",cors,"local only",10);
                else
                    events_stream(c, path, cors);
            }
#endif
            else
                http_send(c,"404 Not Found","text/plain","not found",9);
        }
    }
done:
#ifdef USE_TLS
    if (c->tls) ms_tls_close((ms_tls_conn *)c->tls);
#endif
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
        c->tls_ctx = h->tls_ctx;   /* NULL unless http.https (USE_TLS) */
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
    if (!h) return NULL;
    h->cfg=cfg;
#ifdef USE_CONTROL
    g_start_us = ms_now_us();          /* uptime base for /events stats */
#endif
    h->lfd=net_listen_tcp(cfg->http_port, 8);
    if (h->lfd<0){ LOGE(MOD,"cannot bind http port %d",cfg->http_port); free(h); return NULL; }
#ifdef USE_TLS
    if (cfg->http_https) {
        h->tls_ctx = ms_tls_ctx_new(cfg->http_tls_cert, cfg->http_tls_key);
        if (!h->tls_ctx)
            LOGE(MOD,"HTTPS requested but TLS context failed - serving plain HTTP");
        else
            LOGI(MOD,"HTTPS enabled on port %d", cfg->http_port);
    }
#endif
    h->run=1;
    pthread_create(&h->thr,NULL,accept_thread,h);
    return h;
}

void httpd_stop(httpd *h)
{
    if (!h) return;
    h->run=0;
    shutdown(h->lfd, SHUT_RDWR);   /* close() alone does not wake accept() */
    close(h->lfd);
    pthread_join(h->thr,NULL);
#ifdef USE_TLS
    if (h->tls_ctx) ms_tls_ctx_free((ms_tls_ctx *)h->tls_ctx);
#endif
    free(h);
}
