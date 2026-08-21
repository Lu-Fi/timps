#include "httpd.h"
#include "fmp4.h"
#include "../net.h"
#include "../hub.h"
#include "../log.h"
#include "../util.h"
#include "../rotate_caps.h"  /* ms_vstream_eff_dims (post-rotation mux dims) */
#include "../codec/aac.h"
#include "../auth.h"
#include "../tls.h"
#include "../trace.h"
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
#include <poll.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define MOD "HTTP"

/* HTTP_MAX_CLIENTS (the global limit on concurrent HTTP connections) now lives
 * in util.h: GET /control advertises it as caps.http_max_clients, and one
 * definition shared by the enforcer and the advertiser cannot drift apart. */
/* fanqueue capacity for fMP4 streaming clients (pointers; retained packet
 * payloads are the real cost -> keep this bounded and modest) */
#ifndef MS_MP4_QCAP
#define MS_MP4_QCAP 64
#endif
/* per-client adaptive-drop high-water mark (http.adaptive_drop): the fanqueue
 * is this client's private buffer, kept near-empty by a healthy consumer.
 * Crossing 3/4 of capacity means ~1.5-2s of video (at 25fps over 64 slots)
 * has piled up unsent - a genuinely struggling link, not a momentary IDR
 * burst (a keyframe plus a few P-frames is <10 packets). -D overridable like
 * MS_MP4_QCAP itself. */
#ifndef MS_MP4_DROP_HIWAT
#define MS_MP4_DROP_HIWAT (MS_MP4_QCAP*3/4)
#endif
/* H-2 hardening: disconnect detection in the fMP4/MJPEG streaming loops is
 * data-driven (crecv() is only even polled once fanqueue_pop times out with
 * no packet), so an encoder-stall (hub source pinned but the HAL stopped
 * publishing) plus a client that vanished without an orderly TCP close (power
 * loss, NAT drop) or is on TLS (crecv(MSG_DONTWAIT) can never observe an
 * orderly close there, see crecv() above) left the loop spinning at the
 * fanqueue_pop timeout cadence forever - healthy-looking thread, zero log
 * output, connection slot pinned until restart. A subscribed hub source is
 * expected to always publish (that is the entire on-demand design's
 * invariant - see hub.c), so a long enough run of zero packets is itself
 * proof of a stall independent of what crecv() can observe; bound the leak
 * by giving up once no packet has arrived for this long. Generous relative to
 * any real configured frame rate (even 1 fps clears it 60x over) so it never
 * fires on a healthy, merely slow stream. */
/* An fMP4 presentation whose moov declares an audio trak stalls in MSE as soon
 * as that trak stops advancing: the browser intersects the buffered ranges of
 * all active tracks, so silent audio freezes the VIDEO. The warmup below only
 * settles this at connect time - audio.mute via /control mutes MID-connection,
 * and the HAL then keeps draining the AI without publishing, so the source
 * stays "active" and nothing else notices. Drop such a client instead: on
 * reconnect the warmup sees no AAC and serves a correct video-only moov.
 * 5 s is well above ordinary AAC jitter (~64 ms/frame) and below the point
 * where a frozen picture reads as a dead stream. */
#ifndef MS_MP4_AUDIO_GAP_US
#define MS_MP4_AUDIO_GAP_US (5LL*1000000)
#endif
#ifndef MS_STREAM_STALL_US
#define MS_STREAM_STALL_US (60LL*1000000)
#endif
/* Shutdown drain window (httpd_stop): how long teardown waits for the detached
 * per-connection threads to return. -D overridable like the caps above, purely
 * so a shutdown-latency measurement can widen it and SEE how long the threads
 * actually take instead of only learning that they blew a fixed deadline. */
#ifndef MS_HTTP_DRAIN_MS
#define MS_HTTP_DRAIN_MS 500
#endif

static volatile int g_nconn;   /* current connection count (sync builtins) */
/* M-1: every live connection, so httpd_stop() can END its thread instead of
 * merely waiting for it - a streaming loop parked in fanqueue_pop() has no
 * other way to learn that the daemon is going away. See ms_client_reg in
 * util.h; rtsp.c registers its clients in the same one. */
static ms_client_slot g_clients[HTTP_MAX_CLIENTS];
static ms_client_reg  g_clientreg = MS_CREG_INIT(g_clients);
/* adaptive-drop visibility (http.adaptive_drop): frames a client-side fanqueue
 * discarded while frozen waiting for a keyframe (see the dropping state in
 * mp4_stream below). Per-channel, summed across every mp4 client on that
 * channel - not per-connection, since a single struggling client's drops are
 * the interesting signal regardless of how many other clients are healthy. */
/* plain int, not int64: MIPS32 has no native 64-bit atomic instruction, so
 * __sync_fetch_and_add on a wider type would need libatomic (not linked) */
static volatile unsigned g_drop_frames[MS_MAX_VSTREAM];
static volatile unsigned g_drop_bytes[MS_MAX_VSTREAM];

struct httpd {
    const ms_config *cfg;
    int              lfd;
    pthread_t        thr;
    volatile int     run;
    void            *tls_ctx;   /* ms_tls_ctx* when http.https (USE_TLS), else NULL */
};

/* tr: opt-in send trace (trace.h). NULL for every non-streaming request; only
 * stream_mp4() points it at its own stack-local ctx, so csend()'s hook costs a
 * NULL test on all the short-lived endpoints. */
typedef struct { int fd; const ms_config *cfg; int local; int head; void *tls; void *tls_ctx;
                 ms_trace_ctx *tr;
                 int slot;   /* g_clientreg index, -1 = unregistered */ } hconn;

/* connection I/O that transparently uses TLS when this is an HTTPS connection
 * (c->tls set), otherwise the plain socket. Without USE_TLS these are exactly
 * the old net_sendall(c->fd,...) / recv(c->fd,...) calls. */
static int csend(hconn *c, const void *buf, int len)
{
    /* trace.h: the fMP4 body is ONE TCP connection carrying muxed A/V, so this
     * single write is where a shared-pipe stall shows up. Bracketing it is what
     * separates "the link blocked" from "we were slow building the fragment". */
    int64_t t_wr = ms_trace_wr_begin();
    int rc;
#ifdef USE_TLS
    if (c->tls) {
        rc = ms_tls_write((ms_tls_conn *)c->tls, buf, len);
    } else
#endif
    {
        rc = net_sendall(c->fd, buf, len);
    }
    ms_trace_wr_end(c->tr, t_wr, rc >= 0 ? len : 0);
    return rc;
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
"#msg{position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);"
"color:#f88;font-size:.85em;max-width:90%;display:none}"
"</style></head>";
static const char *PLAYER_TAIL =
"const msg=document.getElementById('msg');"
"const showMsg=(t)=>{if(msg){msg.textContent=t;msg.style.display='block';}};"
"const hideMsg=()=>{if(msg)msg.style.display='none';};"
/* iOS Safari never exposes window.MediaSource; since iOS 17.1 it exposes
 * window.ManagedMediaSource instead (requires disableRemotePlayback, set on
 * the <video> tag below). Desktop browsers keep using plain MediaSource. */
"const MS=window.ManagedMediaSource||window.MediaSource;"
"if(!(MS&&MS.isTypeSupported(mime))){showMsg('Live preview needs a browser with Media Source support.');v.src=src;}"
"else{"
/* Fix 2: the whole MSE pipeline lives in connect() so it can be rebuilt.
 * A backgrounded browser tab is throttled and stops draining TCP; the server
 * then drops the idle socket (15s SO_SNDTIMEO in accept_thread), which ended
 * the fetch reader and, previously, permanently killed the <video> with no
 * recovery. Now: one connection at a time (cur), auto-reconnect on any stream
 * end/error while visible, and rebuild on return-to-foreground. */
"let cur=null,timer=0;"
/* Tear one connection fully down (idempotent via c.dead): abort its fetch so
 * the server socket closes at once instead of riding out the send timeout, end
 * its MediaSource, and revoke ITS object URL (never a newer connection's). */
"const teardown=(c)=>{if(!c||c.dead)return;c.dead=true;"
"try{c.ac.abort();}catch(e){}"
"try{if(c.ms.readyState==='open')c.ms.endOfStream();}catch(e){}"
"try{URL.revokeObjectURL(c.url);}catch(e){}};"
/* At most one pending reconnect; never while hidden or while a live conn exists */
"const retry=()=>{if(timer||document.hidden||(cur&&!cur.dead))return;"
"timer=setTimeout(()=>{timer=0;connect();},1000);};"
"function connect(){if(timer){clearTimeout(timer);timer=0;}"
"if(cur&&!cur.dead)return;"                     /* guard: never two live conns / fetch loops */
"const conn={dead:false,ac:new AbortController(),ms:new MS()};cur=conn;"
"const ms=conn.ms;conn.url=URL.createObjectURL(ms);v.src=conn.url;"
"const fail=()=>{teardown(conn);retry();};"
"ms.addEventListener('sourceopen',async()=>{if(conn.dead)return;"
"try{ms.duration=Infinity;}catch(e){}"          /* Safari live-MSE hint */
"let sb;try{sb=ms.addSourceBuffer(mime);}"
"catch(e){showMsg('Preview: codec not supported by this browser.');teardown(conn);return;}"
"sb.mode='sequence';const q=[];let busy=false;"
/* drop buffered data older than ~10s behind playback so the SourceBuffer
 * never fills up during a long-running live stream */
"const evict=()=>{try{if(sb.buffered.length){const s=sb.buffered.start(0),e=v.currentTime-10;"
"if(e>s+4&&!sb.updating)sb.remove(s,e);}}catch(e){}};"
"const pump=()=>{if(conn.dead||busy||sb.updating||ms.readyState!=='open'||!q.length)return;"
"busy=true;const c=q[0];try{sb.appendBuffer(c);q.shift();}"
"catch(e){busy=false;if(e.name==='QuotaExceededError')evict();else fail();}};"
"sb.addEventListener('updateend',()=>{busy=false;"
"if(v.buffered.length){if(v.currentTime<v.buffered.start(0))v.currentTime=v.buffered.start(0);"
/* Stay near the live edge so a PTZ move shows with minimal delay. A MediaSource
 * <video> otherwise plays wherever autoplay happened to start (often 1-3s of
 * accumulated buffer) and never catches up unless it falls seconds behind.
 * Hard-seek only on a big drift (post-stall); otherwise nudge playbackRate to
 * gently drain the buffer down to ~1.5s behind live (a larger jitter margin
 * than the previous 0.5s - measured WiFi-loss stalls on some cameras run up
 * to ~1.8s, and 0.5s of cushion wasn't enough to absorb them without a
 * visible freeze), scaling the rate with how far behind we are so a large
 * startup buffer drains in a few seconds and steady state settles at 1x. */
"const end=v.buffered.end(v.buffered.length-1),behind=end-v.currentTime;"
"if(behind>5)v.currentTime=end-1.5;"
"else v.playbackRate=behind>1.5?Math.min(1.3,1+(behind-1.5)*0.5):1;}"
"evict();pump();});"
"sb.addEventListener('error',fail);"
"try{const res=await fetch(src,{signal:conn.ac.signal});const rd=res.body.getReader();hideMsg();"
"while(true){const{done,value}=await rd.read();if(done||conn.dead)break;q.push(value);pump();}}"
"catch(e){}fail();"                             /* stream ended/errored/aborted -> reconnect */
"});}"
/* close cleanly when hidden (frees the server socket, blocks reconnect while
 * hidden), rebuild when shown again; connect() self-guards against duplicates */
"document.addEventListener('visibilitychange',()=>{"
"if(document.hidden)teardown(cur);else connect();});"
"connect();}"
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
    /* HEAD (RFC 7231 4.3.2): same headers a GET would send - including the
     * Content-Length of the body a GET would have returned - but no body */
    if (body && bodylen && !c->head) csend(c, body, bodylen);
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

/* HTTP response headers for /stream.mp4 (streamed body, no length) */
static const char MP4_RESP_HDR[] =
    "HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\n"
    "Cache-Control: no-cache\r\nConnection: close\r\n"
    MEDIA_CORS "\r\n";

static void stream_mp4(hconn *c, int chn)
{
    const ms_config *cfg = c->cfg;
    /* enabled/codec/geometry are restart-only: read the boot snapshot so a live
     * /control edit cannot mux this fMP4 stream with a codec/dimensions the
     * running encoder is not producing. See config.h. */
    if (chn<0 || chn>=MS_MAX_VSTREAM || !g_cfg_boot.video[chn].enabled) chn = 0;

    /* HEAD: the headers a GET would send, no body - and no encoder
     * pipeline wake-up for a mere probe */
    if (c->head) { csend(c, MP4_RESP_HDR, (int)sizeof MP4_RESP_HDR - 1); return; }

    /* trace.h: per-connection send trace. Lives on this thread's stack for the
     * whole streaming request and is unhooked before returning, so csend() on
     * any later request of this connection sees tr==NULL again. Armed before
     * the very first csend() so the response headers and the fMP4 init segment
     * (the biggest single write of the session) are accounted for too. */
    ms_trace_ctx trc;
    char trwho[16]; snprintf(trwho, sizeof trwho, "fd%d", c->fd);
    ms_trace_open(&trc, "mp4", trwho, chn);
    c->tr = &trc;

    fanqueue q;
    if (fanqueue_init(&q, MS_MP4_QCAP)) { c->tr = NULL; return; }
    if (hub_subscribe(chn, &q) != 0) {           /* source full (>HUB_MAX_SUBS) */
        http_send_ex(c,"503 Service Unavailable","text/plain",MEDIA_CORS,"busy",4);
        fanqueue_free(&q);
        c->tr = NULL;
        return;
    }
    /* M-1: publish the queue this connection is about to block on, from here
     * to the fanqueue_free() below - EVERY wait in this function is a wait on
     * it, including the two setup loops. Closing it is the wake-up that works
     * unconditionally: shutting the socket down only ends this loop by the
     * detour of crecv() returning 0, which needs a pop to time out first
     * (measured 201 ms vs 30 ms) and, over TLS, never happens at all - crecv()
     * cannot report an orderly close there. The socket shutdown is still worth
     * having, but for the OTHER parking spot: a csend() blocked on a client
     * that stopped reading, which no queue close can reach. */
    ms_creg_set_queue(&g_clientreg, c->slot, &q);
    /* fMP4 can only carry AAC; use it only if the HAL actually produces AAC */
    int acodec=MS_AC_NONE, asr=0, ach=0;
    /* the return value is the source's "active" flag: hub_clear_audio_params()
     * clears it when the HAL failed to bring the capture channel up, but
     * LEAVES acodec at AAC. Ignoring it meant every new connection still paid
     * the full ~800 ms warmup below (and logged a warning) on a camera whose
     * audio can never arrive - measured 0.772 s to first header byte. */
    int aactive = hub_get_audio(&acodec, &asr, &ach);
    int can_audio = (aactive && acodec==MS_AC_AAC);
    if (can_audio && hub_subscribe(HUB_AUDIO_SRC, &q) != 0)
        can_audio = 0;                           /* degrade to video-only */
    hub_request_idr(chn);

    /* wait for parameter sets */
    fmp4_mux mux; fmp4_init(&mux);
    mux.has_video = 1;
    mux.vcodec = g_cfg_boot.video[chn].codec;      /* restart-only: see config.h */
    /* ACTUAL running dims (hub_get_video_params reflects any T23 SW-rotate /
     * T31 FS-rotate safe-envelope refusal - see hub.h); fall back to the raw
     * boot-config computation only if the HAL hasn't populated the hub yet. */
    int ew, eh;
    if (!hub_get_video_params(chn, NULL, &ew, &eh, NULL))
        ms_vstream_eff_dims(&g_cfg_boot.video[chn], &ew, &eh);
    mux.width  = ew;
    mux.height = eh;
    mux.fps    = g_cfg_boot.video[chn].fps;
    int ok=0;
    /* the fanqueue_closed() test is what makes this 2 s wait interruptible:
     * without it a client that connected just as the daemon started shutting
     * down would sit here sleeping past the whole teardown (M-1). */
    for (int i=0;i<200 && !fanqueue_closed(&q);i++){
        vparam vp;
        if (hub_get_vparam(chn,&vp) && vparam_ready(&vp)){ mux.vp=vp; mux.vp_ready=1; ok=1; break; }
        usleep(10000);
    }
    if (!ok){
        if (fanqueue_closed(&q)) goto out;          /* shutting down, not a fault */
        LOGW(MOD,"no video params, abort mp4"); goto out;
    }

    /* Only declare an audio track if AAC frames are actually flowing. A track
     * that is announced in moov but never fed makes browsers stall the whole
     * presentation (video freezes waiting for audio). Warm up briefly and
     * commit to audio only once we've seen a real AAC packet; discard whatever
     * we pop here (we re-request an IDR afterwards). */
    int want_audio = 0;
    if (can_audio) {
        /* fanqueue_closed(): same reason as the parameter wait above - a pop on
         * a closed queue returns NULL at once, so without the test this would
         * spin its 80 iterations instead of leaving. */
        for (int i=0;i<80 && !want_audio && !fanqueue_closed(&q);i++){  /* up to ~800 ms */
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

    if (csend(c, MP4_RESP_HDR, (int)sizeof MP4_RESP_HDR - 1)<0) goto out;

    ms_buf seg;
    if (ms_buf_init(&seg, 4096)) goto out;
    /* fmp4_init_segment fails when the video track isn't warmed up yet or on
     * OOM mid-build (ms_buf sets seg.err, box_close()/box-size patches then
     * skip themselves, leaving seg with some bytes but not a valid box
     * tree) - either way sending it would hand the client a corrupt moov,
     * so bail instead of csend()-ing whatever partial bytes accumulated. */
    if (fmp4_init_segment(&mux, &seg) != 0){ ms_buf_free(&seg); goto out; }
    if (csend(c, seg.data, seg.len)<0){ ms_buf_free(&seg); goto out; }
    ms_buf_free(&seg);
    LOGI(MOD,"mp4 client streaming chn=%d",chn);

    int got_key=0;
    int64_t pre_key_probe_us = 0;   /* see the got_key==0 branch below (H-1) */
    int adaptive = cfg->http_adaptive_drop;
    int dropping = 0;      /* adaptive: skipping this client's frames until a keyframe */
    int64_t drop_idr_us = 0;   /* rate-limit the safety IDR request while dropping */
    int drop_warned = 0;   /* one WARN per client; per-drop detail stays LOGD */
    /* persistent per-connection fragment buffer (M1): reset to len=0/err=0
     * each frame instead of ms_buf_init()/ms_buf_free() per packet - avoids
     * a malloc+free (plus the full-AU copy that was already unavoidable) on
     * every single video/audio frame of every connected client. Grows once
     * to this stream's steady-state fragment size via ms_buf_reserve and is
     * then reused for the life of the connection; freed once when the loop
     * below exits. */
    ms_buf frag;
    if (ms_buf_init(&frag, 4096)) goto out;        /* OOM */
    int64_t last_pkt_us = ms_now_us();   /* H-2: encoder-stall bound, see above */
    int64_t last_audio_us = last_pkt_us; /* see MS_MP4_AUDIO_GAP_US */
    /* blocking socket: net_sendall must never write a partial fragment */
    while (1) {
        /* M-1: teardown closed our queue - leave NOW, before popping whatever
         * is still buffered. Sending it out could block up to SO_SNDTIMEO
         * (15 s) on a client that stopped reading, which is exactly the wedge
         * the drain in httpd_stop() cannot survive. */
        if (fanqueue_closed(&q)) break;
        ms_pkt *p = fanqueue_pop(&q, 200);
        if (!p) {
            char t[8]; int n=crecv(c,t,sizeof t,MSG_DONTWAIT);
            if (n==0) break;
            int64_t idle_now = ms_now_us();
            /* trace.h: idle tick for the periodic summary - reuses the stall
             * check's clock read, so no extra syscall on this path */
            ms_trace_window(&trc, idle_now);
            if (idle_now - last_pkt_us > MS_STREAM_STALL_US) {
                LOGW(MOD,"mp4 chn=%d: no packets for %llds - encoder stall, "
                         "dropping this client", chn,
                     (long long)(MS_STREAM_STALL_US/1000000));
                break;
            }
            continue;
        }
        last_pkt_us = ms_now_us();
        /* A declared-but-unfed audio trak freezes the whole presentation in
         * MSE (see MS_MP4_AUDIO_GAP_US). Only meaningful once we committed to
         * audio in the moov; video-only clients never reach the else branch. */
        if (p->media == MS_MEDIA_AUDIO) {
            last_audio_us = last_pkt_us;
        } else if (mux.has_audio &&
                   last_pkt_us - last_audio_us > MS_MP4_AUDIO_GAP_US) {
            LOGW(MOD,"mp4 chn=%d: no audio for %llds but video still flowing "
                     "(muted mid-stream?) - dropping this client so it "
                     "reconnects video-only", chn,
                 (long long)(MS_MP4_AUDIO_GAP_US/1000000));
            pkt_unref(p);
            break;
        }
        /* trace.h: last_pkt_us IS the pop instant, so t_pop costs nothing extra
         * on this path - only the t_done read after the send does. */
        int64_t t_pop = ms_trace_on(MS_TR_AU) ? last_pkt_us : 0;
        int tr_q = -1, tr_qcap = -1;
        if (t_pop) {
            ms_trace_au_begin(&trc);
            if (ms_trace_on(MS_TR_Q)) fanqueue_depth(&q, &tr_q, &tr_qcap, NULL);
        }
        /* self-guarded on MS_TR_SUM, so summaries work with MS_TR_AU off too */
        ms_trace_window(&trc, last_pkt_us);
        int lost_key = fanqueue_take_dropped_key(&q);
        /* WARN once per client: sustained overflow was otherwise invisible
         * below DEBUG - only the /control counters moved */
        if (lost_key && !drop_warned++)
            LOGW(MOD,"mp4 chn=%d: send queue overflowed, dropping frames "
                     "(client/network too slow) - details at DEBUG", chn);
        /* ANY eviction breaks GOP integrity for this client, not just a lost
         * keyframe: dropped mid-GOP P-frames leave every later P-frame of that
         * GOP referencing AUs the decoder never got (same defect rtsp.c heals
         * via its fanqueue_take_dropped() branch). This client's queue can
         * evict without ever tripping the two old triggers: the FQ_MAX_BYTES
         * byte budget binds below MS_MP4_DROP_HIWAT slots during a bitrate
         * spike (2 MB / 48 slots = ~43 KB/frame, an ordinary motion burst at
         * 1080p), and a hole shorter than one GOP need not contain a keyframe.
         * Observed on cam-L (T23/atbm6062 weak WiFi) QA 2026-08-11:
         * ~1.4-1.6 s eviction holes whose delivery resumed on mid-GOP
         * P-frames - silent corruption the adaptive path was built to
         * prevent. */
        int lost_any = fanqueue_take_dropped(&q);
        if (adaptive) {
            /* Per-client adaptive frame-dropping. This client's fanqueue is
             * its own private buffer; if it backs up (a weak link that can't
             * keep up) or the producer already had to evict a keyframe to make
             * room, the queued GOP tail is (or is about to become) a headless,
             * undecodable H.264 GOP. Naively forwarding those P-frames would
             * make the decoder build on references it never got - visible
             * corruption/drift for up to a full GOP (2s). Instead we FREEZE
             * this client on its last good frame and drop forward through the
             * backlog until the next NATURAL keyframe, then resume cleanly at
             * that fresh, self-contained GOP boundary. Draining pops (no mux,
             * no send) also let this client catch back up to the live edge.
             * Crucially we never call hub_request_idr() here: an IDR request
             * is global to the shared encoder and would spike the bitrate for
             * every other subscriber (Frigate, recording, healthy viewers)
             * just because one link is weak. Worst case this client gets a
             * keyframe-only slideshow - honest degradation, never corruption. */
            if (lost_key || lost_any) { hub_note_drop(chn); dropping = 1; }
            if (!dropping) {
                int cnt = 0; fanqueue_depth(&q, &cnt, NULL, NULL);
                if (cnt >= MS_MP4_DROP_HIWAT) dropping = 1;
            }
            if (dropping) {
                if (p->media == MS_MEDIA_VIDEO && p->keyframe) {
                    dropping = 0;                /* clean boundary: resume here */
                } else {
                    /* Freeze on the last good frame and drain the backlog until
                     * the next keyframe. 'dropping' MUST NOT be able to starve
                     * the client: on a slow SoC under sustained overflow the
                     * fanqueue can evict natural keyframes before this consumer
                     * pops them, so a keyframe might otherwise never arrive at
                     * the head and the stream would hang forever (the v1 bug).
                     * Guarantee progress by asking the encoder for a fresh IDR,
                     * rate-limited to once per second so a chronically weak
                     * client cannot spam global IDR requests and spike the
                     * shared encoder for every other subscriber. Recovery is
                     * then bounded to ~1s + one IDR latency, never unbounded. */
                    int64_t now = ms_now_us();
                    if (now - drop_idr_us > 1000000) {
                        LOGD(MOD,"mp4 chn=%d: overflow backlog - freezing "
                                 "client, IDR re-requested", chn);
                        hub_request_idr(chn);
                        drop_idr_us = now;
                    }
                    __sync_fetch_and_add(&g_drop_frames[chn], 1u);
                    __sync_fetch_and_add(&g_drop_bytes[chn], (unsigned)p->len);
                    pkt_unref(p);
                    continue;
                }
            }
        } else if (lost_key || lost_any) {
            /* legacy (adaptive_drop off): the queue overflowed - ask the
             * encoder for a fresh IDR so the client doesn't decode garbage
             * until the next natural GOP. Rate-limited like rtsp.c's
             * equivalent branch: a non-key drop can repeat every push while a
             * client stays behind, and the IDR request is global to the
             * shared encoder. */
            hub_note_drop(chn);
            int64_t now = ms_now_us();
            if (lost_key || now - drop_idr_us > 1000000) {
                LOGD(MOD,"mp4 chn=%d: overflow dropped %s - IDR re-requested",
                     chn, lost_key ? "a keyframe" : "P-frame(s)");
                hub_request_idr(chn);
                drop_idr_us = now;
            }
        }
        ms_buf_reset(&frag, 256*1024);   /* reuse, shrink an outlier IDR buffer back */
        int frag_ok = 1;
        if (p->media==MS_MEDIA_VIDEO) {
            if (!got_key){
                if(!p->keyframe){
                    /* H-1: packets keep arriving here, so fanqueue_pop above
                     * never times out and the disconnect probe at the top of
                     * the loop is unreachable - and the encoder was only
                     * ever asked for a keyframe once, before this loop
                     * started. If it never delivers one (HAL wedge), this
                     * used to discard forever: healthy-looking thread, zero
                     * log output, no exit. Retry the IDR request and probe
                     * for a stale/disconnected client on the same ~1s
                     * cadence the adaptive-drop path below already uses. */
                    int64_t now = ms_now_us();
                    if (now - pre_key_probe_us > 1000000) {
                        pre_key_probe_us = now;
                        hub_request_idr(chn);
                        char t[8];
                        if (crecv(c, t, sizeof t, MSG_DONTWAIT) == 0){
                            pkt_unref(p);
                            break;
                        }
                    }
                    pkt_unref(p); continue;
                }
                got_key=1;
            }
            frag_ok = fmp4_video_fragment(&mux, p->data, p->len, p->keyframe, p->pts_us, &frag) == 0;
        } else if (want_audio && got_key) {
            frag_ok = fmp4_audio_fragment(&mux, p->data, p->len, p->pts_us, &frag) == 0;
        }
        /* a failed fragment (OOM mid-build) can still have partial bytes in
         * frag - never send those, they're not a valid moof/mdat and would
         * desync the client's SourceBuffer for the rest of the session. If
         * we dropped a keyframe's fragment this way, ask for a fresh IDR so
         * the stream can resync as soon as memory pressure clears. */
        int rc = 0;
        if (!frag_ok) {
            LOGW(MOD,"dropped a corrupt %s fragment (OOM?)",
                 p->media==MS_MEDIA_VIDEO?"video":"audio");
            if (p->media==MS_MEDIA_VIDEO && p->keyframe) hub_request_idr(chn);
        } else if (frag.len) {
            rc = csend(c, frag.data, frag.len);
        }
        /* trace.h: read what the line needs before the packet can be recycled.
         * Note `send - wr` here is the fMP4 mux cost (fmp4_*_fragment builds
         * moof+mdat, which copies the whole AU) - the fMP4 analogue of the RTSP
         * packetizer, and the thing to look at when a slow AU shows a small wr. */
        if (t_pop)
            ms_trace_au_end(&trc, p->media, p->keyframe, p->len, p->enq_us,
                            t_pop, ms_now_us(), tr_q, tr_qcap);
        pkt_unref(p);
        if (rc<0) break;
    }
    ms_buf_free(&frag);
out:
    hub_unsubscribe(chn, &q);
    if (can_audio) hub_unsubscribe(HUB_AUDIO_SRC, &q);
    /* withdraw before the queue dies: after this returns no stop-side
     * fanqueue_close() can reach it any more (see ms_client_reg in util.h) */
    ms_creg_set_queue(&g_clientreg, c->slot, NULL);
    fanqueue_free(&q);
    c->tr = NULL;           /* trc dies with this frame; unhook before return */
}

/* JPEG source selection for /snapshot.jpg and /stream.mjpeg:
 *   no ?chn=  -> dedicated jpeg.* channel (fallback: first videoN.jpeg)
 *   ?chn=N    -> ONLY the JPEG encoder piggybacked on video stream N
 *                (videoN.jpeg=true) - no fallback, an explicit request for
 *                a channel that isn't a usable source is a 404, not a
 *                silent substitution.
 * Parses the query string, then hands off to hub_pick_jpeg_src() (shared
 * with timelapse.c's channel-selection - see hub.c) for the actual
 * priority/fallback logic. Returns a hub source id or -1 if nothing
 * suitable is enabled. */
static int jpeg_src_from_path(const char *path, const ms_config *cfg)
{
    const char *q = strstr(path, "chn=");
    int chn = q ? atoi(q+4) : -1;
    return hub_pick_jpeg_src(cfg, chn, q != NULL);
}

/* single latest JPEG snapshot.
 *
 * On-demand: timps encodes nothing while idle, so subscribing to the JPEG
 * hub source is what WAKES that encoder (hub_notify_activity -> HAL
 * jpeg_thread: fs_use + StartRecvPic). The cold-wake, two-phase-wait grab
 * itself (piggyback parent-video wake included) is shared with timelapse.c
 * as hub_grab_jpeg() - see hub.h/hub.c for the full mechanism. This function
 * only owns the HTTP side: the 404/503 responses and writing the JPEG bytes
 * out as a response. Worst-case wait is unchanged: 2 x HUB_JPEG_GRAB_WAIT_MS
 * (3 s at the default), never hangs the connection. */
/* publish the grab's queue into this connection's registry slot, so
 * ms_creg_wake_all() can close it on shutdown (see hub_grab_hook in hub.h) */
static void snap_qhook(struct fanqueue *q, void *ctx)
{
    ms_creg_set_queue(&g_clientreg, ((hconn *)ctx)->slot, q);
}

static void snapshot_jpg(hconn *c, int src)
{
    if (src < 0){ http_send_ex(c,"404 Not Found","text/plain",MEDIA_CORS,"no jpeg",7); return; }
    int busy = 0;
    ms_pkt *p = hub_grab_jpeg(src, HUB_JPEG_GRAB_WAIT_MS, &busy, snap_qhook, c);
    if (p) {
        char hdr[224];
        int n=snprintf(hdr,sizeof hdr,
            "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n"
            "Cache-Control: no-cache\r\nConnection: close\r\n" MEDIA_CORS "\r\n", p->len);
        /* never send a truncated header (n >= sizeof hdr means snprintf's
         * would-be length overran the buffer) - same guard as http_send_ex.
         * HEAD gets the true Content-Length of the grabbed frame, no body. */
        if (n < (int)sizeof hdr && csend(c,hdr,n)>=0 && !c->head)
            csend(c,p->data,(int)p->len);
        pkt_unref(p);
    } else if (busy) {
        http_send_ex(c,"503 Service Unavailable","text/plain",MEDIA_CORS,"busy",4);
    } else {
        http_send_ex(c,"503 Unavailable","text/plain",MEDIA_CORS,"no frame",8);
    }
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

    char rh[288];
    int n=snprintf(rh,sizeof rh,
        "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=%s\r\n"
        "Cache-Control: no-cache\r\nConnection: close\r\n" MEDIA_CORS "\r\n", BND);
    /* HEAD: same headers a GET would send (incl. the boundary), no body,
     * no encoder wake-up */
    if (c->head){ csend(c,rh,n); return; }

    /* subscribe BEFORE sending headers so a full source can answer 503 */
    fanqueue q;
    if (fanqueue_init(&q,8)) return;
    if (hub_subscribe(src, &q) != 0) {   /* too many subscribers */
        http_send_ex(c,"503 Service Unavailable","text/plain",MEDIA_CORS,"busy",4);
        fanqueue_free(&q);
        return;
    }
    if (csend(c,rh,n)<0){ hub_unsubscribe(src,&q); fanqueue_free(&q); return; }
    /* The boundary delimiter is what tells a client that the PREVIOUS part is
     * complete. Emitting it as the PREFIX of the next part (the old
     * "\r\n--BND\r\nContent-Type: ..." in one write) means frame N is only
     * declared finished once frame N+1 exists - a client that scans for the
     * delimiter instead of trusting Content-Length therefore shows every frame
     * one full frame period late (~200 ms at the default 5 fps). Send the
     * delimiter immediately AFTER each body instead, and open the body with one
     * delimiter here. Same bytes on the wire, same RFC 2046 framing (delimiter
     * line, part headers, blank line, body) - only the instant of the write
     * moves, from "when the next frame arrives" to "now".
     * dlm carries the leading CRLF that terminates the preceding body; the
     * opening delimiter is the same string without it, hence dlm+2. */
    char dlm[80];
    int dn = snprintf(dlm, sizeof dlm, "\r\n--%s\r\n", BND);
    if (dn >= (int)sizeof dlm ||                      /* cannot happen: BND<=63 */
        csend(c, dlm + 2, dn - 2) < 0) {
        hub_unsubscribe(src,&q); fanqueue_free(&q); return;
    }
    LOGI(MOD,"mjpeg client streaming");
    /* M-1: from here on this thread only ever waits on the queue, so publish
     * it - see stream_mp4 above. The csend()s before this point are covered by
     * the fd shutdown instead. */
    ms_creg_set_queue(&g_clientreg, c->slot, &q);
    int64_t last_pkt_us = ms_now_us();   /* H-2: encoder-stall bound, see above */
    while (1) {
        if (fanqueue_closed(&q)) break;             /* M-1: teardown, see stream_mp4 */
        ms_pkt *p = fanqueue_pop(&q, 500);
        if (!p) {
            char t[8]; int r=crecv(c,t,sizeof t,MSG_DONTWAIT);
            if (r==0) break;
            if (ms_now_us() - last_pkt_us > MS_STREAM_STALL_US) {
                LOGW(MOD,"mjpeg: no frames for %llds - encoder stall, "
                         "dropping this client",
                     (long long)(MS_STREAM_STALL_US/1000000));
                break;
            }
            continue;
        }
        last_pkt_us = ms_now_us();
        char part[160];
        int hn=snprintf(part,sizeof part,
            "Content-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n",
            p->len);
        /* a truncated multipart header would desync the boundary framing for
         * the rest of the stream - never send it, tear the stream down like
         * any other write failure (n >= sizeof part = snprintf overran) */
        int rc = (hn >= (int)sizeof part) ? -1 : csend(c,part,hn);
        if (rc>=0) rc = csend(c,p->data,(int)p->len);
        /* close this part and open the next one NOW, not when the next frame
         * shows up - that is the whole point (see the comment above dlm). */
        if (rc>=0) rc = csend(c,dlm,dn);
        pkt_unref(p);
        if (rc<0) break;
    }
    hub_unsubscribe(src, &q);
    ms_creg_set_queue(&g_clientreg, c->slot, NULL);
    fanqueue_free(&q);
}

/* --- Digest-auth nonce table --------------------------------------------
 * HTTP here is one-connection-per-request ("Connection: close"), so a
 * digest nonce cannot live in per-connection state the way the RTSP
 * session's s->nonce does: the 401 that issues the nonce and the
 * authenticated retry arrive on different connections. Track recently
 * issued nonces in a small global ring instead (bounded memory; overflow
 * just evicts the oldest outstanding challenge, whose client then gets a
 * fresh 401 with stale=true and retries silently). Replay protection:
 *   - qop=auth clients (RFC 7616/2617): nc must strictly increase per
 *     nonce, so a sniffed (nonce,nc,response) triple is rejected on
 *     replay while a legitimate client keeps reusing the nonce with
 *     nc++ (NVR snapshot pollers) until the TTL expires;
 *   - legacy RFC 2069 clients (no qop): the nonce is one-shot -
 *     consumed by its first successful use, replays always fail.
 * A stateless signed-timestamp nonce was rejected: replay REJECTION
 * inherently needs per-nonce state (nc high-water / consumed flag); a
 * pure MAC'd timestamp would accept a sniffed response for the whole
 * validity window. Nonce values themselves come from auth_make_nonce()
 * (/dev/urandom-backed, 128 bit). */
#ifndef HTTP_NONCE_MAX
#define HTTP_NONCE_MAX 32
#endif
#define HTTP_NONCE_TTL_US (300*1000000LL)          /* 5 min challenge life */

static struct hnonce { char n[33]; int64_t born; uint64_t nc_hi; }
                       g_nonces[HTTP_NONCE_MAX];
static int             g_nonce_next;
static pthread_mutex_t g_nonce_mx = PTHREAD_MUTEX_INITIALIZER;

/* mint + remember a fresh challenge nonce for a 401 */
static void http_new_nonce(char out[33])
{
    auth_make_nonce(out);
    pthread_mutex_lock(&g_nonce_mx);
    struct hnonce *e = &g_nonces[g_nonce_next];
    g_nonce_next = (g_nonce_next + 1) % HTTP_NONCE_MAX;
    snprintf(e->n, sizeof e->n, "%s", out);
    e->born  = ms_now_us();
    e->nc_hi = 0;
    pthread_mutex_unlock(&g_nonce_mx);
}

/* nonce/nc freshness check for an already crypto-valid Digest response;
 * consumes (no-qop) or advances (qop=auth) the entry under the lock.
 * strcmp on the nonce is fine timing-wise: nonces are public values,
 * sent in clear in every 401. */
static int http_nonce_check(const char *nonce, const char *nc)
{
    int64_t now = ms_now_us();
    int ok = 0;
    pthread_mutex_lock(&g_nonce_mx);
    for (int i = 0; i < HTTP_NONCE_MAX; i++) {
        struct hnonce *e = &g_nonces[i];
        if (!e->n[0]) continue;
        if (now - e->born > HTTP_NONCE_TTL_US) { e->n[0] = 0; continue; }
        if (strcmp(e->n, nonce) != 0) continue;
        if (nc && nc[0]) {                         /* qop=auth: nc must grow */
            char *end = NULL;
            uint64_t v = strtoull(nc, &end, 16);
            if (v > 0 && end && *end == 0 && v > e->nc_hi) { e->nc_hi = v; ok = 1; }
        } else {                                   /* RFC 2069: one-shot */
            e->n[0] = 0;
            ok = 1;
        }
        break;
    }
    pthread_mutex_unlock(&g_nonce_mx);
    return ok;
}

/* returns 1 if the request carries valid Basic or Digest credentials (or
 * auth disabled). When a Digest response is cryptographically valid for
 * the configured credentials but its nonce is not one we recently issued
 * (expired/evicted/replayed), *digest_stale is set so the 401 advertises
 * stale=true and the client re-handshakes without re-prompting the user
 * (per RFC 7616 3.3 stale MUST only be sent when the creds were right). */
static int http_check_auth(const ms_config *cfg, const char *buf,
                           const char *method, const char *path,
                           int *digest_stale)
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
            if (strncasecmp(line,"Digest",6)==0) {
                char cn[64], nc[16];
                /* path is the request line's Request-URI (2nd token, %255s):
                 * the client's digest uri MUST match it or the response is a
                 * replay for a different URI - reject like any auth failure */
                if (!auth_http_digest(method, path, line, user, pass,
                                      cn, sizeof cn, nc, sizeof nc))
                    return 0;                      /* wrong user/pass/uri/format */
                if (http_nonce_check(cn, nc)) return 1;
                if (digest_stale) *digest_stale = 1;
                return 0;
            }
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
 *   stats    - {"uptime_s","clients","video":[{"chn","subs","fps","kbps",
 *              "width","height","codec","drop_frames","drop_bytes"},..]}
 * ?stream=motion,daynight,stats selects types (default: all). Auth/CORS are
 * the /control rules (conn_thread); note EventSource cannot send headers, so
 * browsers pass the token as ?token= (accepted by http_check_token). Each
 * connection blocks on the events condvar (events_wait, woken by
 * imp_motion.c/daynight.c/control.c). Motion is QUEUE-driven: each
 * connection drains the events.c snapshot ring with a private cursor and
 * emits every queued grid change (lossless; a lapped slow client just loses
 * the oldest), with a resample+dedup pass only as initial-state/resync.
 * daynight/stats stay level-sampled with per-connection dedup; the full
 * state goes out once on connect and stats tick every events.stats_ms.
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
 * counts, the measured per-stream fps/bitrate (the OSD {fps}/{bitrate}
 * sources, see hub_get_fps/hub_get_bitrate), and cumulative adaptive-drop
 * counters (0/0 unless http.adaptive_drop=1 and a client's link is actually
 * struggling - see g_drop_frames/g_drop_bytes). */
static int stats_json(const ms_config *cfg, char *buf, size_t cap)
{
    size_t o = 0;
    #define APP(...) do { \
        int _n = snprintf(o<cap?buf+o:buf, o<cap?cap-o:0, __VA_ARGS__); \
        if (_n>0) o += (size_t)_n; \
    } while (0)
    APP("{\"uptime_s\":%lld,\"clients\":%d,\"video\":[",
        (long long)((ms_now_us()-g_start_us)/1000000), hub_video_subs());
    int first = 1;
    for (int i=0;i<MS_MAX_VSTREAM;i++){
        /* report the RUNNING stream (boot snapshot) so enabled/geometry/codec
         * stay consistent with the measured fps/kbps beside them, not a live-
         * edited g_cfg the encoder has not picked up. See config.h. */
        if (!g_cfg_boot.video[i].enabled) continue;
        /* ACTUAL running dims - same fix as stream_mp4() above: prefer the
         * hub's post-refusal effective dims, raw computation only as a
         * pre-hub-population fallback. */
        int w, h;
        if (!hub_get_video_params(i, NULL, &w, &h, NULL))
            ms_vstream_eff_dims(&g_cfg_boot.video[i], &w, &h);
        APP("%s{\"chn\":%d,\"subs\":%d,\"fps\":%.1f,\"kbps\":%.0f,"
            "\"width\":%d,\"height\":%d,\"codec\":\"%s\","
            "\"drop_frames\":%u,\"drop_bytes\":%u}",
            first?"":",", i, hub_subs(i), hub_get_fps(i), hub_get_bitrate(i),
            w, h, g_cfg_boot.video[i].codec==MS_VC_H265?"h265":"h264",
            g_drop_frames[i], g_drop_bytes[i]);
        first = 0;
    }
    APP("]}");
    #undef APP
    return (int)o;
}

static void events_stream(hconn *c, const char *path, const char *cors)
{
    const ms_config *cfg = c->cfg;

    /* HEAD: headers only, no SSE body, no client-slot consumed */
    if (c->head) {
        char hh[768];
        int hhn = snprintf(hh, sizeof hh,
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
            "Cache-Control: no-store\r\nConnection: close\r\n"
            "X-Accel-Buffering: no\r\n%s\r\n", cors);
        if (hhn < (int)sizeof hh) csend(c, hh, hhn);
        return;
    }

    /* ?stream= filter: absent = all; present = only the listed types */
    int want_motion = 1, want_dn = 1, want_stats = 1, want_config = 1;
    const char *f = strstr(path, "stream=");
    if (f){
        char fl[80]; int i = 0;
        for (f += 7; f[i] && f[i] != '&' && i < (int)sizeof fl - 1; i++) fl[i] = f[i];
        fl[i] = 0;
        want_motion = strstr(fl, "motion")   != NULL;
        want_dn     = strstr(fl, "daynight") != NULL;
        want_stats  = strstr(fl, "stats")    != NULL;
        want_config = strstr(fl, "config")   != NULL;
    }

    /* own cap below the global HTTP_MAX_CLIENTS: a flood of /events
     * connections must not exhaust the per-connection threads (each SSE
     * client parks one thread until it disconnects) */
    /* the <=0 fallback is shared with control.c (util.h) so the number
     * caps.events_max_clients advertises is the number enforced here */
    int max = cfg->events_max_clients > 0 ? cfg->events_max_clients
                                          : EVENTS_MAX_CLIENTS_DEF;
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
    int ld_en = 0, ld_mode = 0, ld_ds = -2;
    float ld_b = 0.0f, ld_g = 0.0f;
    int stats_ms = cfg->events_stats_ms;
    int64_t next_stats = (want_stats && stats_ms > 0) ? ms_now_us() : INT64_MAX;
    int64_t last_write = ms_now_us();
    unsigned gen = events_generation();
    unsigned mq_cur = events_motion_cursor();     /* private snapshot cursor */
    unsigned cfg_cur = events_config_cursor();    /* private config-table cursor */
    char js[1024];                                /* fits max_cells active[] */

    memset(&lm, 0, sizeof lm);
    for (;;){
        int rc = 0;
        if (want_motion){
            ms_motion_status m;
            /* drain the snapshot ring first: the producer queued EVERY grid
             * change, so none can coalesce away between two wakeups (the old
             * resample+dedup here dropped paired transitions) */
            while (rc >= 0 && events_motion_pop(&mq_cur, &m)){
                lm = m; have_m = 1;
                if (control_motion_json(js, sizeof js, &m) < (int)sizeof js)
                    rc = sse_emit(c, "motion", js, &last_write);
            }
            /* level resync: the initial full state on connect, plus changes
             * that never enter the ring (config edits via /control, the host
             * sim stub). Dedup vs the last-sent snapshot; last_ms just counts
             * up, so it is deliberately NOT compared. */
            if (rc >= 0){
                motion_get_status(&m);
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
        }
        if (rc >= 0 && want_dn){
            int en = 0, mode = 0, ds = -1;
            float b = -1.0f, g = -1.0f, lu = -1.0f;
            float ex = -1.0f, nb = -1.0f, dt = -1.0f;
            daynight_get_status(&en, &mode, &b, &g, &ex, &lu, &nb, &dt, &ds);
            float db = b - ld_b; if (db < 0) db = -db;
            float dg = g - ld_g; if (dg < 0) dg = -dg;
            /* change thresholds match the producer filter in daynight.c */
            if (!have_d || en != ld_en || mode != ld_mode || ds != ld_ds ||
                db >= 1.0f ||
                dg >= (ld_g > 0.0f ? ld_g * 0.05f : 8.0f)){
                ld_en = en; ld_mode = mode; ld_b = b; ld_g = g; ld_ds = ds;
                have_d = 1;
                if (control_daynight_json(js, sizeof js, en, mode, b, g, ex,
                                          lu, nb, dt, ds) < (int)sizeof js)
                    rc = sse_emit(c, "daynight", js, &last_write);
            }
        }
        if (rc >= 0 && ms_now_us() >= next_stats){
            next_stats = ms_now_us() + (int64_t)stats_ms * 1000;
            if (stats_json(cfg, js, sizeof js) < (int)sizeof js)
                rc = sse_emit(c, "stats", js, &last_write);
        }
        if (rc >= 0 && want_config){
            /* settings changed elsewhere (another client's /control POST):
             * a lapped eviction first (rare - many distinct keys changed
             * while this client was off the condvar), then every key this
             * client hasn't seen the latest value of yet. Values are
             * already sanitize_val()-cleaned in control.c (no raw quotes/
             * control bytes), safe to splice into the JSON string as-is. */
            if (events_config_resync(&cfg_cur))
                rc = sse_emit(c, "config", "{\"resync\":true}", &last_write);
            char ck[40], cv[160];
            while (rc >= 0 && events_config_pop(&cfg_cur, ck, sizeof ck, cv, sizeof cv)){
                /* Escape like every other JSON surface: the value here is the
                 * CANONICAL stored one, so a quote or backslash that reached
                 * timps.conf by hand (POSTed values are stripped of both) would
                 * otherwise emit a broken event and desync the client's parser
                 * mid-stream. Same escaper as GET /control and the POST reply. */
                char eck[96], ecv[336];
                ms_json_esc(ck, eck, sizeof eck);
                ms_json_esc(cv, ecv, sizeof ecv);
                int jl = snprintf(js, sizeof js,
                    "{\"key\":\"%s\",\"value\":\"%s\"}", eck, ecv);
                if (jl < (int)sizeof js)
                    rc = sse_emit(c, "config", js, &last_write);
            }
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
    /* enabled is restart-only -> boot snapshot (see config.h) */
    if (q){ int n=atoi(q+4); if(n>=0 && n<MS_MAX_VSTREAM && g_cfg_boot.video[n].enabled) return n; }
    return def;
}

/* serve the preview page with an MSE codec string derived from the live SPS */
static void serve_player(hconn *c, const char *path)
{
    const ms_config *cfg = c->cfg;
    int chn = path_chn(path, cfg, cfg->http_preview_chn);
    if (chn<0||chn>=MS_MAX_VSTREAM||!g_cfg_boot.video[chn].enabled) chn=0;  /* restart-only */

    char vcodec[48] = "avc1.640028";               /* High@4.0 fallback */
    hub_request_idr(chn);
    for (int i=0;i<100;i++){
        vparam vp;
        if (hub_get_vparam(chn,&vp) && vparam_ready(&vp)){
            if (vp.codec==MS_VC_H264 && vp.sps_len>=4)
                snprintf(vcodec,sizeof vcodec,"avc1.%02X%02X%02X",vp.sps[1],vp.sps[2],vp.sps[3]);
            else if (vp.codec==MS_VC_H265) {
                /* derive from the real SPS profile_tier_level; keep the old
                 * guessed string only if the SPS isn't parseable yet */
                if (vparam_hevc_codecs(&vp, vcodec, sizeof vcodec) < 0)
                    snprintf(vcodec,sizeof vcodec,"hvc1.1.6.L123.B0");
            }
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
        "<div id=wrap><video id=v autoplay muted controls playsinline disableremoteplayback>"
        "</video><div id=msg></div></div>"
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
    /* M-1: registered before the first blocking I/O of this connection (the
     * TLS handshake already is one), so httpd_stop() can reach it from here
     * on. Full registry -> -1 -> every ms_creg_* call below is a no-op. */
    c->slot = ms_creg_add(&g_clientreg, c->fd);
#ifdef USE_TLS
    /* HTTPS: run the TLS handshake before any request I/O. From here on all
     * reads/writes go through crecv/csend, which use c->tls transparently. */
    if (c->tls_ctx) {
        c->tls = ms_tls_accept((ms_tls_ctx *)c->tls_ctx, c->fd);
        if (!c->tls) { ms_creg_del(&g_clientreg, c->slot);   /* before close(): fd reuse */
                       close(c->fd); free(c); __sync_fetch_and_sub(&g_nconn, 1); return NULL; }
    }
#endif
#ifdef USE_CONTROL
    char buf[4096];               /* room for a full nested /control JSON body */
#else
    char buf[1024];
#endif
    /* Accumulate until the header block is complete (CRLFCRLF), the buffer
     * fills, or a bounded deadline elapses. A single recv() can return a
     * partial request line/headers under TCP segmentation, TLS record
     * boundaries, or slow clients/proxies, which broke auth/path parsing
     * below (RTSP's client_thread already loops like this). The deadline
     * is needed here because these sockets carry no SO_RCVTIMEO: without
     * it, a client that trickles bytes in would park this thread forever
     * instead of just getting whatever arrived in the first read. */
    int n = 0;
    int64_t hdr_deadline = ms_now_us() + 5*1000000LL;
    for (;;) {
        int64_t left_us = hdr_deadline - ms_now_us();
        if (left_us <= 0) break;
#ifdef USE_TLS
        /* L7: bytes already decrypted into the TLS layer's buffer are
         * invisible to poll() on the raw fd - skip the wait when pending */
        if (!(c->tls && ms_tls_pending((ms_tls_conn *)c->tls) > 0))
#endif
        {
            struct pollfd pfd; pfd.fd = c->fd; pfd.events = POLLIN; pfd.revents = 0;
            int pr = poll(&pfd, 1, (int)(left_us/1000)+1);
            if (pr <= 0) break;                /* timeout or poll error */
        }
        int r = crecv(c, buf+n, sizeof(buf)-1-n, 0);
        if (r <= 0) break;
        n += r;
        buf[n] = 0;
        if (strstr(buf,"\r\n\r\n") || n >= (int)sizeof(buf)-1) break;
    }
    if (n>0) {
        buf[n]=0;
        char method[8], path[256];
        if (sscanf(buf,"%7s %255s",method,path)==2) {
            /* HEAD = GET semantics with the body suppressed everywhere
             * (http_send_ex + the per-handler checks below) */
            c->head = (strcmp(method,"HEAD")==0);
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
            /* NOTE: every alias the dispatcher below accepts must be listed
             * here, or that alias silently loses BOTH the ?token= unlock and
             * the CORS headers - "/mjpeg" did, so a valid token got a 401 on
             * it while "/stream.mjpeg" worked (docs promise identical rules). */
            int media = !strncmp(path,"/stream.mp4",11)   ||
                        !strncmp(path,"/snapshot.jpg",13) ||
                        !strncmp(path,"/stream.mjpeg",13) ||
                        !strncmp(path,"/mjpeg",6);
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
             * set for /control, /events and the media endpoints), Digest
             * or Basic. The 401 offers Digest first (with a fresh tracked
             * nonce + qop="auth") and Basic second, so digest-capable
             * clients upgrade while plain Basic pollers keep working. */
            int stale = 0;
            if (!c->local && !tok_ok &&
                !http_check_auth(c->cfg, buf, method, path, &stale)) {
                /* only real credential failures feed the rate-limited
                 * brute-force report: a header-less request is the normal
                 * digest first round-trip, a stale nonce a normal retry */
                if (!stale && strcasestr(buf, "\nAuthorization:")) {
                    struct sockaddr_in pa; socklen_t pl = sizeof pa;
                    char ip[INET_ADDRSTRLEN] = "?";
                    if (getpeername(c->fd, (struct sockaddr*)&pa, &pl) == 0)
                        inet_ntop(AF_INET, &pa.sin_addr, ip, sizeof ip);
                    auth_fail_note(MOD, c->tls ? "https" : "http", ip);
                }
                char nonce[33];
                http_new_nonce(nonce);
                char r[1024];
                int rn = snprintf(r, sizeof r,
                    "HTTP/1.1 401 Unauthorized\r\n"
                    "WWW-Authenticate: Digest realm=\"" AUTH_REALM "\", "
                        "nonce=\"%s\", qop=\"auth\"%s\r\n"
                    "WWW-Authenticate: Basic realm=\"" AUTH_REALM "\"\r\n%s"
                    "Content-Length: 12\r\nConnection: close\r\n\r\n%s",
                    nonce, stale ? ", stale=true" : "", cors,
                    c->head ? "" : "Unauthorized");
                if (rn < (int)sizeof r) csend(c, r, rn);
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
                else if (!strcmp(method,"GET") || c->head) {
                    /* HEAD previously fell into the POST branch below and
                     * ran control_apply_json("") - GET semantics instead */
                    /* GET /control?fields=1: the F_CTRL field-name inventory
                     * (see control_fields_json's doc comment in control.h) -
                     * a small, separate document from the main status dump
                     * below, so it gets its own (much smaller) heap buffer
                     * rather than sharing CONTROL_JSON_CAP. Simple substring
                     * match on the query string, same convention as chn=/
                     * token= elsewhere in this file - no full query parser
                     * in this codebase. */
                    if (strstr(path, "fields=1")) {
                        #define CONTROL_FIELDS_CAP 8192
                        char *fj = (char *)malloc(CONTROL_FIELDS_CAP);
                        if (fj) {
                            int fn = control_fields_json(fj, CONTROL_FIELDS_CAP);
                            if (fn < 0) {
                                http_send_ex(c,"500 Internal Server Error","text/plain",
                                             cors,"fields json too large",21);
                            } else {
                                http_send_ex(c,"200 OK","application/json",cors,fj,fn);
                            }
                            free(fj);
                        } else {
                            http_send_ex(c,"503 Service Unavailable","text/plain",cors,"oom",3);
                        }
                        #undef CONTROL_FIELDS_CAP
                        goto control_get_done;
                    }
                    /* worst case: caps + full image/audio/sensor blocks +
                     * 2 full video stream blocks + 2 per-stream OSD sets
                     * (2 x 8 items) with long texts + the motion status
                     * (up to MOTION_MAX_CELLS "active" entries). Heap, not
                     * stack: this buffer would otherwise sit in every
                     * conn_thread's frame, which is tight on small embedded
                     * libc thread-stack defaults.
                     * +4K headroom for "last_errors" (16 modules x ~190 B
                     * worst case, log.c) + queue_drops/record error fields. */
                    #define CONTROL_JSON_CAP 22528
                    char *js = (char *)malloc(CONTROL_JSON_CAP);
                    if (js) {
                        int jn = control_get_json(js, CONTROL_JSON_CAP);
                        if (jn < 0) {
                            /* buffer overflow: js holds a truncated, invalid-
                             * JSON prefix. Never ship that as 200 - tell the
                             * client generation failed so it can retry/alert
                             * rather than choke on corrupt data silently. */
                            http_send_ex(c,"500 Internal Server Error","text/plain",
                                         cors,"control json too large",22);
                        } else {
                            http_send_ex(c,"200 OK","application/json",cors,js,jn);
                        }
                        free(js);
                    } else {
                        http_send_ex(c,"503 Service Unavailable","text/plain",cors,"oom",3);
                    }
                    #undef CONTROL_JSON_CAP
                    control_get_done: ;
                } else {
                    char *body = strstr(buf,"\r\n\r\n");
                    if (body) {
                        body += 4;
                        /* body may arrive split: finish per Content-Length */
                        int have = n - (int)(body - buf), clen = 0;
                        const char *cl = strcasestr(buf,"Content-Length:");
                        if (cl) clen = atoi(cl+15);
                        /* how many body bytes this fixed buffer can hold
                         * alongside the headers already consumed. A clen
                         * bigger than that used to get silently clamped and
                         * the truncated prefix applied as if it were the
                         * whole JSON - some keys take effect, others in the
                         * dropped tail just vanish, yet the client still
                         * gets 200 OK. Reject loudly instead. */
                        int cap = (int)sizeof(buf) - 1 - (int)(body - buf);
                        /* a negative Content-Length (e.g. "-1") used to slip
                         * past this guard entirely (clen > cap is false for
                         * any negative clen against a positive cap) */
                        if (clen < 0 || clen > cap) {
                            http_send_ex(c,"413 Payload Too Large","text/plain",cors,
                                        "body too large",14);
                            goto done;
                        }
                        /* H2: same bounded-deadline poll as the header
                         * phase above - without it a client announcing a
                         * Content-Length but never sending the body parked
                         * this thread in recv() indefinitely */
                        int64_t body_deadline = ms_now_us() + 5*1000000LL;
                        while (have < clen && n < (int)sizeof(buf)-1) {
                            int64_t left_us = body_deadline - ms_now_us();
                            if (left_us <= 0) break;
#ifdef USE_TLS
                            /* L7: see the header loop */
                            if (!(c->tls && ms_tls_pending((ms_tls_conn *)c->tls) > 0))
#endif
                            {
                                struct pollfd pfd;
                                pfd.fd = c->fd; pfd.events = POLLIN; pfd.revents = 0;
                                if (poll(&pfd, 1, (int)(left_us/1000)+1) <= 0) break;
                            }
                            int r = crecv(c, buf+n, sizeof(buf)-1-n, 0);
                            if (r <= 0) break;
                            n += r; have += r; buf[n] = 0;
                        }
                    }
                    /* The old code answered {"ok":true} 200 to everything -
                     * garbage, truncated JSON, unknown keys and real writes
                     * alike - so no client could tell a typo from a write, and
                     * no test could fail. Report what actually happened:
                     *   400 - the body was not a JSON object at all
                     *   422 - it parsed, but carried no field this build knows
                     *         (typo, wrong section, or a key gated out of this
                     *         build); nothing was applied
                     *   409 - it parsed and every field in it WAS known, but
                     *         every one of them was refused (bad value, or a
                     *         command that failed); nothing was applied
                     *   200 - at least one known field was applied
                     *   503 - the daemon could not allocate to service it
                     *
                     * 422 used to cover BOTH of the middle two, and those are
                     * opposite instructions to the client. "no field this build
                     * knows" says: your key names are wrong for THIS binary -
                     * check spelling, or check whether the feature is compiled
                     * in at all (the fleet's two builds differ - see the tls
                     * block in GET /control). "every field was refused" says:
                     * your key names were right and the daemon understood you,
                     * the VALUES are the problem - re-send with valid ones. A
                     * client that retried the first case unchanged would loop
                     * forever; a client that went hunting for a missing build
                     * feature in the second would be chasing nothing. Same
                     * status code, so neither could be automated.
                     *
                     * WHICH meaning kept 422 was decided by the installed base,
                     * not by taste. The 409 case is the newer one (its counter,
                     * cr.rejected, only started being reachable when values
                     * began being refused outright), while "unknown field ->
                     * 422" is what is already asserted on deployed cameras -
                     * thingino's timps-selftest.sh probes an unknown key and
                     * FAILS the camera on anything but 422, and the WebUI's
                     * timps-api.js prints its "no setting in this request is
                     * known to this timps build" message on a 422 whose
                     * rejected==0. Moving THAT to a new code would have turned
                     * every fielded selftest red for a purely cosmetic gain.
                     * Moving the value-refusal case instead costs only the
                     * rejected>0 branch of that same WebUI message, which
                     * degrades to the generic "timps /control HTTP 409" line
                     * until the WebUI is updated - a worse sentence, not a
                     * broken client. 409 Conflict: a 4xx (client must change
                     * something), not retryable as-is, and not already spoken
                     * by this endpoint.
                     *
                     * "reason" is the fix for the next client, so it never has
                     * to infer any of this from the status line. Emitted only
                     * on the error answers: a 200 body stays byte-for-byte what
                     * it was, and this is the hot path a dragged slider posts on.
                     *
                     * "accepted" counts applied fields INCLUDING no-op rewrites,
                     * so re-posting the current value is a success, not a
                     * silent failure. Clamped writes count as accepted too -
                     * clamping is the documented contract, not an error. */
                    ctrl_result cr;
                    int prc = control_apply_json(body ? body : "", &cr);
                    const char *st, *reason;
                    if (prc == -2)                              { st = "503 Service Unavailable"; reason = "oom"; }
                    else if (prc != 0)                          { st = "400 Bad Request";         reason = "not_json"; }
                    else if (cr.accepted == 0 && cr.rejected>0) { st = "409 Conflict";            reason = "values_rejected"; }
                    else if (cr.accepted == 0)                  { st = "422 Unprocessable Content"; reason = "unknown_fields"; }
                    else                                        { st = "200 OK";                  reason = NULL; }
                    char rb[CTRL_ECHO_CAP + 224];
                    int rn = snprintf(rb, sizeof rb,
                        "{\"ok\":%s,\"accepted\":%d,\"changed\":%d,"
                        "\"rejected\":%d,\"not_persisted\":%d,"
                        "\"applied\":{%s}%s%s%s%s}",
                        (prc==0 && cr.accepted>0) ? "true" : "false",
                        cr.accepted, cr.changed, cr.rejected, cr.not_persisted,
                        prc==0 ? cr.echo : "",
                        (prc==0 && !cr.echo_full) ? ",\"truncated\":true" : "",
                        reason ? ",\"reason\":\"" : "", reason ? reason : "",
                        reason ? "\"" : "");
                    if (rn >= (int)sizeof rb) rn = (int)sizeof rb - 1;
                    http_send_ex(c,st,"application/json",cors,rb,rn);
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
    /* M-1: unregister BEFORE close() - once closed, this fd number can be
     * handed to another accept(), and httpd_stop() must never shutdown() a
     * reused fd (same rule as rtsp.c's registry). */
    ms_creg_del(&g_clientreg, c->slot);
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
        int fd = net_accept_cloexec(h->lfd,(struct sockaddr*)&peer,&pl);
        if (fd<0){ if(h->run) usleep(50000); continue; }
        /* H2: bounded I/O - a silent client (or one that stops reading) must
         * time out in recv()/send() instead of pinning this slot's thread
         * forever; streaming clients read continuously and never trip these */
        net_set_timeouts(fd, 30, 15);
        /* live media is latency-sensitive: without TCP_NODELAY, Nagle holds a
         * small fMP4 fragment until the previous one is ACKed, which combined
         * with client delayed-ACK adds tens to ~200 ms per fragment. The RTSP
         * path already sets this (rtsp.c); each fragment is one send() here so
         * the syscall-batching tradeoff that made RTSP interleave carefully
         * does not apply. */
        net_set_nodelay(fd);
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
        c->fd=fd; c->cfg=h->cfg; c->slot=-1;   /* real slot assigned in conn_thread */
        /* loopback (127.0.0.0/8) clients skip auth: the local web UI must always
         * be able to reach the streamer, external clients still need the
         * password. This replaces prudynt's "web UI auth key". */
        c->local = ((ntohl(peer.sin_addr.s_addr) & 0xFF000000u) == 0x7F000000u);
        c->tls_ctx = h->tls_ctx;   /* NULL unless http.https (USE_TLS) */
        __sync_fetch_and_add(&g_nconn, 1);
        pthread_t t;
        if (ms_thread_create(&t,MS_STACK_CONN,conn_thread,c)==0) pthread_detach(t);
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
    ms_thread_create(&h->thr,MS_STACK_UTIL,accept_thread,h);
    return h;
}

void httpd_stop(httpd *h)
{
    if (!h) return;
    h->run=0;
    shutdown(h->lfd, SHUT_RDWR);   /* close() alone does not wake accept() */
    close(h->lfd);
    pthread_join(h->thr,NULL);
    /* Finding 3: the per-connection threads are pthread_detach'd, so the join
     * above only reaped the accept thread - an in-flight /control POST handler
     * can still be inside isp_apply_image / imp_osd_apply / motion_sync (direct
     * IMP calls) when main() next runs g_hal->stop() and destroys those
     * channels. Give the detached conn_threads a bounded window (~500 ms, same
     * shape as the RTSP 1s drain) to return; g_nconn hits 0 once the last one
     * does. Previously ONLY USE_TLS builds waited here, and only to protect the
     * tls_ctx free - the same wait is needed regardless of TLS to keep handlers
     * off the HAL, so it now runs unconditionally.
     *
     * M-1: the window alone was never enough, and the long-lived media stream
     * this comment used to wave at as an acceptable casualty was in fact the
     * common case. Such a loop waits on PACKETS, not on its socket: h->run is
     * invisible to it, nothing in a shutdown makes a frame stop arriving, and
     * over TLS crecv() cannot even report an orderly close. Measured: three
     * HTTP stream threads still live 20 s into teardown - i.e. still reading
     * the TLS context freed just below. So END them first and only then wait:
     * ms_creg_wake_all() closes each connection's stream queue (fanqueue_pop
     * returns at once and fanqueue_closed() tells the loop to leave) and shuts
     * its fd down (for a thread parked in send()/recv() instead - up to
     * SO_SNDTIMEO, 15 s, on a client that stopped reading). The drain then has
     * something that will actually finish inside its window. main()'s hard-exit
     * alarm stays the ultimate backstop, but is no longer what this path
     * depends on. */
    ms_creg_wake_all(&g_clientreg);
    int64_t drain0 = ms_now_us();
    for (int i = 0; i < (MS_HTTP_DRAIN_MS+9)/10 && g_nconn > 0; i++) usleep(10000);
    /* Say whether the drain actually drained. Without this the failure mode is
     * invisible: teardown continues either way, and "still live" is exactly the
     * state in which the tls_ctx free below is a use-after-free. */
    if (g_nconn > 0)
        LOGW(MOD,"%d connection thread(s) still live after a %lld ms drain - "
                 "proceeding to teardown", g_nconn,
             (long long)((ms_now_us()-drain0)/1000));
    else
        LOGI(MOD,"all connection threads gone after %lld ms",
             (long long)((ms_now_us()-drain0)/1000));
#ifdef USE_TLS
    if (h->tls_ctx) {
        /* the drain above already let any TLS handshake/read/write conn_thread
         * settle, so the ctx none of them still reference is safe to free. */
        ms_tls_ctx_free((ms_tls_ctx *)h->tls_ctx);
    }
#endif
    free(h);
}
