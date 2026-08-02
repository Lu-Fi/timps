#include "rtsp.h"
#include "rtp.h"
#include "../net.h"
#include "../hub.h"
#include "../log.h"
#include "../util.h"
#include "../codec/aac.h"
#include "../auth.h"
#include "../tls.h"
#include "backchannel.h"
#if defined(USE_TLS) || defined(USE_BACKCHANNEL)
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
/* A3: RTSP session keepalive timeout, advertised as ";timeout=" in every
 * SETUP response's Session header (RFC 2326 12.37; 60 s is also the RFC
 * default when the parameter is absent, so this only makes explicit what
 * clients already assume - ffmpeg/live555/VLC read it and send keepalives
 * at timeout/2). Enforcement is UDP-transport only and deliberately lax at
 * 2x the advertised value: a well-behaved client pings every 30 s, so it
 * gets four missed keepalives of grace before the session is reaped. */
#ifndef RTSP_SESSION_TIMEOUT_S
#define RTSP_SESSION_TIMEOUT_S 60
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

/* P3: batch buffer for UDP RTP - collect the packets of one access unit and
 * hand them to the kernel in a single sendmmsg() instead of one sendto()
 * each. A 200 KB IDR is ~170 packets at the 1200-byte default MTU; on this
 * no-vDSO 3.10/MIPS platform each avoided syscall is ~2-5 us. Availability
 * verified against the actual toolchain: uClibc-ng exports sendmmsg (checked
 * in the buildroot sysroot libc.so) and kernel 3.10 has the syscall (since
 * 3.0); glibc/musl (sim builds) have it too. ENOSYS still falls back at
 * runtime, so an exotic kernel degrades to the old per-packet path. */
#define RTP_BATCH_N 16
typedef struct {
    int             n;
    struct mmsghdr  msgs[RTP_BATCH_N];
    struct iovec    iov[RTP_BATCH_N];
    uint8_t         buf[RTP_BATCH_N][RTP_MTU_MAX];
} rtp_batch;

/* RTP output sink (UDP or TCP-interleaved) */
typedef struct {
    int                tcp;          /* 1 = interleaved on control fd */
    int                fd;           /* udp RTP socket (or control fd, TCP) */
    int                fd_rtcp;      /* udp RTCP socket (UDP only, else unused) */
    struct sockaddr_in dst, dst_rtcp;
    int                chan_rtp, chan_rtcp;
    rtp_batch         *batch;        /* P3: UDP video only; NULL = send direct */
#ifdef USE_TLS
    void              *tls;          /* ms_tls_conn* when interleaved over RTSPS */
#endif
} rtp_sink;

/* flush the pending sendmmsg batch; 0 = ok (or nothing pending), <0 = error
 * (same contract as a failed sendto: caller stops the session) */
static int sink_flush(rtp_sink *s)
{
    rtp_batch *b = s->batch;
    if (!b || b->n == 0) return 0;
    int off = 0;
    while (off < b->n) {
        int r = (int)sendmmsg(s->fd, b->msgs + off, (unsigned)(b->n - off), 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == ENOSYS) {           /* kernel without sendmmsg */
                for (; off < b->n; off++)
                    if (sendto(s->fd, b->iov[off].iov_base, b->iov[off].iov_len,
                               0, (struct sockaddr*)&s->dst, sizeof s->dst) < 0)
                        { b->n = 0; return -1; }
                break;
            }
            b->n = 0;
            return -1;
        }
        off += r;
    }
    b->n = 0;
    return 0;
}

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
        if (!rtcp && s->batch) {
            /* P3: stage into the batch; the play loop flushes after every
             * access unit (and this flushes itself when a big IDR fills a
             * whole batch mid-AU) */
            rtp_batch *b = s->batch;
            if (len > (int)sizeof b->buf[0]) return -1;
            memcpy(b->buf[b->n], pkt, (size_t)len);
            struct iovec *iv = &b->iov[b->n];
            iv->iov_base = b->buf[b->n];
            iv->iov_len  = (size_t)len;
            struct mmsghdr *m = &b->msgs[b->n];
            memset(m, 0, sizeof *m);
            m->msg_hdr.msg_name    = &s->dst;
            m->msg_hdr.msg_namelen = sizeof s->dst;
            m->msg_hdr.msg_iov     = iv;
            m->msg_hdr.msg_iovlen  = 1;
            if (++b->n == RTP_BATCH_N && sink_flush(s) < 0) return -1;
            return len;
        }
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
    int                 next_ichan;    /* A7: next self-assigned interleaved channel */
    rtp_sink            vsink, asink;
    int                 v_udp[2], a_udp[2];   /* server rtp,rtcp fds */
    fanqueue            q;
    rtp_track           vtrack, atrack;
    int                 playing;
    int                 play_cseq;     /* CSeq of PLAY; 200 sent after subscribe */
#ifdef USE_BACKCHANNEL
    int                 have_bc;               /* client SETUP the backchannel (trackID=2) */
    int                 bc_udp[2];             /* server rtp,rtcp recv fds (UDP), -1 none */
    int                 bc_chan_rtp;           /* TCP-interleaved rtp channel id, -1 UDP */
#endif
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

/* A5: if the client sent a Session header it must name the session this
 * connection actually owns (RFC 2326 12.37) - anything else is 454. No
 * header at all is fine (pre-SETUP requests, and per-connection sessions
 * make it redundant anyway). The value may carry ";timeout=..." which some
 * clients echo back, so compare up to the first delimiter. */
static int session_matches(session *s, const char *req)
{
    const char *v = hdr_find(req, "Session");
    if (!v) return 1;
    size_t l = strlen(s->session);
    if (l && !strncmp(v, s->session, l) &&
        (v[l]==0 || v[l]=='\r' || v[l]=='\n' || v[l]==';' || v[l]==' '))
        return 1;
    return 0;
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

/* A6: the absolute request URL as the client sent it, for Content-Base */
static void extract_url(const char *req, char *out, int outsz)
{
    out[0]=0;
    const char *sp = strchr(req, ' ');
    if (!sp) return;
    const char *url = sp+1;
    const char *end = strchr(url, ' ');
    if (!end) return;
    int n = (int)(end-url);
    if (n >= outsz) n = outsz-1;
    memcpy(out, url, n); out[n]=0;
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
    /* strip scheme://host[:port] - accept both rtsp:// and the TLS listener's
     * rtsps:// (RFC 7826 C.1), else a DESCRIBE over port 322 keeps the whole
     * "rtsps://host:port/chN" URL as its "path", find_video_by_path() finds no
     * prefix match and silently falls back to the first stream (wrong sub-/
     * mainstream). host:port skipping is identical - only the scheme differs. */
    const char *p = tmp;
    if (!strncasecmp(p,"rtsps://",8)){ p+=8; const char *slash=strchr(p,'/'); p = slash?slash:""; }
    else if (!strncasecmp(p,"rtsp://",7)){ p+=7; const char *slash=strchr(p,'/'); p = slash?slash:""; }
    strncpy(out, p, outsz-1); out[outsz-1]=0;
}

static void gen_sdp(session *s, const ms_config *c, int vchn, char *sdp, int sdpsz, int want_bc)
{
    (void)want_bc;
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

    /* C2: RFC 4566 5.2 wants a globally unique (sess-id, sess-version)
     * pair, NTP-timestamp format recommended - not the fixed "0 0" this
     * used to emit. One process-wide boot timestamp is enough: timps never
     * re-announces an SDP, so the version only needs to be valid, not
     * incrementing. Static init is thread-safe here in practice and the
     * worst race outcome is two clients seeing the same (valid) id. */
    static long long sdp_sid;
    if (!sdp_sid) sdp_sid = (long long)time(NULL) + 2208988800LL;

    /* M2: guard every accumulation step so `sizeof(body)-n` (size_t) can
     * never underflow if an earlier snprintf() reported it would have
     * written past the buffer (n > sizeof(body)). Mirrors the n>=0/n<sizeof
     * guard already used for the RTP-Info header in stream_loop().
     * C3: a=range:npt=now- marks the stream live/unbounded (RFC 2326 A.3);
     * without it some players assume a seekable VOD range. */
    if (n>=0 && n<(int)sizeof(body))
        n += snprintf(body+n, sizeof(body)-n,
            "v=0\r\no=- %lld %lld IN IP4 %s\r\ns=timps\r\nc=IN IP4 %s\r\n"
            "t=0 0\r\na=range:npt=now-\r\n"
            "a=control:*\r\n",   /* A6: aggregate control = Content-Base */
            sdp_sid, sdp_sid, ip, ip);

    /* video. C3: advertise what the daemon already knows from its own
     * config - b=AS (kbps, RFC 4566 5.8), a=framerate (RFC 4566 6),
     * a=framesize (3GPP TS 26.234; the attribute older ONVIF NVR auto-
     * configurators read) - so clients get bitrate/fps/geometry without
     * having to parse the SPS. */
    const ms_vstream_cfg *v = &c->video[vchn];
    int isH265 = (v->codec==MS_VC_H265);
    int vw, vh; ms_vstream_eff_dims(v, &vw, &vh);
    if (n>=0 && n<(int)sizeof(body))
        n += snprintf(body+n, sizeof(body)-n,
            "m=video 0 RTP/AVP %d\r\nb=AS:%d\r\n"
            "a=rtpmap:%d %s/90000\r\n"
            "a=framerate:%d\r\na=framesize:%d %d-%d\r\n"
            "a=control:trackID=0\r\n",
            VIDEO_PT, v->bitrate_kbps,
            VIDEO_PT, isH265?"H265":"H264",
            v->fps, VIDEO_PT, vw, vh);
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
            int akbps = c->audio.bitrate_kbps > 0 ? c->audio.bitrate_kbps : 32;
            if (n>=0 && n<(int)sizeof(body))
                n += snprintf(body+n, sizeof(body)-n,
                    "m=audio 0 RTP/AVP %d\r\nb=AS:%d\r\n"
                    "a=rtpmap:%d mpeg4-generic/%d/%d\r\n"
                    "a=fmtp:%d streamtype=5;profile-level-id=1;mode=AAC-hbr;"
                    "sizelength=13;indexlength=3;indexdeltalength=3;config=%02X%02X\r\n"
                    "a=control:trackID=1\r\n",
                    AUDIO_PT, akbps, AUDIO_PT, asr, ach, AUDIO_PT, asc[0], asc[1]);
        } else {
            int pt = (acodec==MS_AC_PCMA)?8:0;   /* static PTs */
            const char *nm = (acodec==MS_AC_PCMA)?"PCMA":"PCMU";
            if (n>=0 && n<(int)sizeof(body))
                n += snprintf(body+n, sizeof(body)-n,
                    "m=audio 0 RTP/AVP %d\r\nb=AS:64\r\n"   /* G.711 is 64 kbps */
                    "a=rtpmap:%d %s/8000\r\n"
                    "a=control:trackID=1\r\n", pt, pt, nm);
        }
    }
#ifdef USE_BACKCHANNEL
    /* ONVIF audio backchannel (client -> speaker). Only advertised when the
     * client asked for it via `Require: www.onvif.org/ver20/backchannel` and
     * the backchannel was enabled at boot. a=sendonly + own trackID mirror
     * prudynt/live555. bc_available() reflects the boot-time state (not the
     * live audio.backchannel value): the key is restart-only, so a /control
     * enable must not advertise a pipeline still running on default codec/rate
     * until restart. */
    if (want_bc && bc_available()){
        int pt = bc_payload_type(), clk = bc_clock_rate();
        char fmtp[192]; fmtp[0]=0;
        if (pt==97){   /* AAC (mpeg4-generic): config= is a required RFC3640 param */
            uint8_t asc[2]; aac_asc(clk, 1, asc);
            snprintf(fmtp,sizeof fmtp,
                "a=fmtp:%d streamtype=5;profile-level-id=1;mode=AAC-hbr;"
                "sizelength=13;indexlength=3;indexdeltalength=3;config=%02X%02X\r\n",
                pt, asc[0], asc[1]);
        }
        if (n>=0 && n<(int)sizeof(body))
            n += snprintf(body+n, sizeof(body)-n,
                "m=audio 0 RTP/AVP %d\r\nc=IN IP4 0.0.0.0\r\nb=AS:%d\r\n"
                "a=rtpmap:%d %s/%d/1\r\n%s"
                "a=control:trackID=2\r\na=sendonly\r\n",
                pt, (pt==97?clk/667:64), pt, bc_rtpmap_name(), clk, fmtp);
    }
#endif
    (void)sdpsz;
    snprintf(sdp, sdpsz, "%s", body);
}

static void send_resp(session *s, int cseq, const char *extra, const char *body)
{
    /* hdr must hold: status line + extra headers + Content-Type/Length +
     * CRLFCRLF + the body. The largest body is the DESCRIBE SDP (char
     * sdp[2600] in handle_request); with the Content-Base extra (cb[560])
     * the worst case is ~3.3 KB, so 3072 could truncate. Size well past
     * that. Clamping alone is not enough: Content-Length is computed from
     * the FULL body length, so a truncated body write would advertise more
     * bytes than are sent and the client hangs. The explicit guard below
     * therefore drops such a response loudly instead. */
    char hdr[4096];
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
    /* hard bounds check: m is the would-be length; m >= remaining means the
     * body (and thus the message) was truncated while Content-Length already
     * promised the full bl. Never send a length-mismatched response - fail
     * loudly so an oversized SDP surfaces instead of silently reappearing at
     * a new, larger threshold. */
    if (m >= (int)(sizeof(hdr) - n)) {
        LOGE(MOD, "send_resp: response too large (hdr=%d body=%d cap=%d), dropping",
             n, bl, (int)sizeof hdr);
        return;
    }
    n += m;
    r_send(s, hdr, n);
}

/* A1: error twin of send_resp() - every response, including 4xx/5xx, must
 * echo the client's CSeq (RFC 2326 12.17). live555-derived stacks match
 * responses to requests via CSeq and time out instead of failing cleanly
 * when it's missing. extra (optional) carries additional headers, each
 * CRLF-terminated, e.g. "Allow: ...\r\n". */
static void send_err(session *s, int cseq, int code, const char *reason,
                     const char *extra)
{
    char hdr[512];
    int n = snprintf(hdr, sizeof hdr,
        "RTSP/1.0 %d %s\r\nCSeq: %d\r\n%s\r\n",
        code, reason, cseq, extra?extra:"");
    if (n < 0) return;
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
    /* the RTSP request-line target the client also puts in its digest uri=
     * (the full "rtsp[s]://host/path" URL): the digest uri MUST match it or
     * the response is a replay captured for a different URI */
    char rurl[512]; extract_url(req, rurl, sizeof rurl);
    if (av[0]) {
        if (auth_rtsp_digest(method, rurl, av, s->cfg->rtsp_user, s->cfg->rtsp_pass, s->nonce) ||
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
/* RFC 2326 12.32: every feature-tag a client lists in Require: is mandatory -
 * if we don't support one we MUST answer 551 Option not supported and echo the
 * unsupported tag(s) in an Unsupported: header, without running the method.
 * The only tag this daemon ever supports is the ONVIF backchannel, and only in
 * a USE_BACKCHANNEL build with backchannel actually enabled in the running
 * config (same condition the DESCRIBE handler uses to gate the a=sendonly
 * m-line). Parses the comma-separated tag list; returns 1 and fills `unsup`
 * (comma-separated) with the tags we can't honour, or 0 when Require: is absent
 * (the overwhelmingly common case, zero work) or every tag is supported. */
static int require_unsupported(session *s, const char *req,
                               char *unsup, size_t unsupsz)
{
    (void)s;
    const char *rq = hdr_find(req, "Require");
    if (!rq) { unsup[0]=0; return 0; }        /* no Require: -> nothing to do */
    int bc_ok = 0;
#ifdef USE_BACKCHANNEL
    bc_ok = bc_available();       /* boot-time state; audio.backchannel is restart-only */
#endif
    /* isolate the header value (up to end-of-line) so we can split it */
    char list[256]; size_t li=0;
    while (rq[li] && rq[li]!='\r' && rq[li]!='\n' && li < sizeof(list)-1)
        { list[li]=rq[li]; li++; }
    list[li]=0;
    size_t ol=0; unsup[0]=0;
    char *p = list;
    while (*p) {
        char *comma = strchr(p, ',');
        char *tok = p;
        if (comma) { *comma = 0; p = comma+1; } else p += strlen(p);
        while (*tok==' '||*tok=='\t') tok++;          /* trim leading ws */
        char *e = tok + strlen(tok);
        while (e>tok && (e[-1]==' '||e[-1]=='\t')) *--e = 0; /* trim trailing ws */
        if (!*tok) continue;
        if (bc_ok && strstr(tok, "backchannel")) continue; /* supported */
        /* unsupported: append comma-separated to the Unsupported: list */
        size_t tl = strlen(tok);
        if (ol + (ol?2:0) + tl < unsupsz) {
            if (ol) { unsup[ol++]=','; unsup[ol++]=' '; }
            memcpy(unsup+ol, tok, tl); ol+=tl; unsup[ol]=0;
        }
    }
    return ol>0;
}

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
    /* A5: a stale/garbage Session id must not silently act on the one
     * session this connection owns (RFC 2326 12.37) */
    if (!session_matches(s, req)) {
        send_err(s, cseq, 454, "Session Not Found", NULL);
        return 0;
    }
    /* RFC 2326 12.32: reject any Require: feature-tag we can't honour with 551
     * + Unsupported before running the method (OPTIONS is answered above -
     * capability discovery must always succeed). No Require: header is the
     * common case and costs one hdr_find. */
    {
        char unsup[256];
        if (require_unsupported(s, req, unsup, sizeof unsup)) {
            char e[300]; snprintf(e, sizeof e, "Unsupported: %s\r\n", unsup);
            send_err(s, cseq, 551, "Option not supported", e);
            return 0;
        }
    }
    if (!strncmp(req, "DESCRIBE", 8)) {
        int vchn = find_video_by_path(s->cfg, path);
        if (vchn < 0) { send_err(s, cseq, 404, "Not Found", NULL); return -1; }
        s->vchn = vchn;
        /* C1: never ship an SDP without the a=fmtp line (sprop-parameter-
         * sets/profile-level-id). The old code waited 500 ms and then sent
         * whatever it had - formally valid per RFC 6184 8.1, but hardware-
         * decoder NVRs and some mobile SDKs init their decoder strictly
         * from the SDP and stay black. Worse, waiting alone cannot help on
         * a fresh boot: encoding is on-demand, so with no subscriber the
         * encoder never runs and SPS/PPS never appear. Briefly subscribe
         * (same wake-up PLAY would do moments later) so the encoder spins
         * up and the hub captures the parameter sets from the first IDR,
         * then release it again - the activity callback keeps the encoder
         * running only if someone else is still consuming. Only if that
         * fails within 2 s (encoder wedged) answer 503 + Retry-After
         * instead of a degraded SDP the client would cache all session. */
        hub_request_idr(vchn);
        int vready = 0;
        { vparam vp; vready = hub_get_vparam(vchn,&vp) && vparam_ready(&vp); }
        if (!vready) {
            fanqueue wq; int winit = fanqueue_init(&wq, 4) == 0;
            int wsub = winit && hub_subscribe(vchn, &wq) == 0;
            for (int i=0;i<200 && !vready;i++){
                vparam vp;
                if (hub_get_vparam(vchn,&vp) && vparam_ready(&vp)) { vready=1; break; }
                usleep(10000);
            }
            if (wsub)  hub_unsubscribe(vchn, &wq);
            if (winit) fanqueue_free(&wq);
        }
        if (!vready) {
            send_err(s, cseq, 503, "Service Unavailable", "Retry-After: 1\r\n");
            return 0;
        }
        int want_bc = 0;
#ifdef USE_BACKCHANNEL
        /* A Require: backchannel here was already validated as supported by the
         * global 551 check above (an unsupported one never reaches this code);
         * we only need to know whether the client asked for it, to gate the
         * a=sendonly backchannel m-line in the SDP. */
        { const char *rq = hdr_find(req, "Require"); want_bc = rq && strstr(rq,"backchannel"); }
#endif
        char sdp[2600]; gen_sdp(s, s->cfg, vchn, sdp, sizeof sdp, want_bc);
        /* A6: explicit Content-Base (request URL, '/'-terminated per RFC
         * 2326 C.1.1) so strict parsers resolve the relative
         * a=control:trackID=N against it instead of guessing a base from
         * the request URL themselves */
        char url[512]; extract_url(req, url, sizeof url);
        char cb[560]; cb[0]=0;
        size_t ul = strlen(url);
        if (ul)
            snprintf(cb, sizeof cb, "Content-Base: %s%s\r\n",
                     url, url[ul-1]=='/' ? "" : "/");
        send_resp(s, cseq, cb, sdp);
        return 0;
    }
    if (!strncmp(req, "SETUP", 5)) {
        const char *tr = hdr_find(req, "Transport");
        int is_audio = strstr(path,"trackID=1") != NULL;
        if (s->vchn < 0) s->vchn = find_video_by_path(s->cfg, path);
        /* no valid video source -> refuse; otherwise vchn==-1 would index
         * c->video[-1] (OOB) later in stream_loop */
        if (s->vchn < 0) {
            send_err(s, cseq, 404, "Not Found", NULL);
            return -1;
        }
        if (!s->session[0]) {
            /* M6: session id from auth_gen_token()'s /dev/urandom generator,
             * not rand() - rand() is seeded time^pid (main.c) and guessable */
            char tok[33]; auth_gen_token(tok);
            snprintf(s->session, sizeof s->session, "%.8s", tok);
        }

#ifdef USE_BACKCHANNEL
        if (strstr(path,"trackID=2")){        /* ONVIF audio backchannel (we receive) */
            if (!bc_available()){    /* boot-time state; audio.backchannel is restart-only */
                /* refuse just this track, keep the connection (video/audio may
                 * already be SETUP on it) */
                send_err(s, cseq, 406, "Not Acceptable", NULL); return 0;
            }
            char bextra[256];
            if (tr && strstr(tr,"TCP")){
                int rc=-1, cc=-1; const char *il=strstr(tr,"interleaved=");
                if (il) sscanf(il+12,"%d-%d",&rc,&cc);
                /* A7: same self-assignment as the media tracks - the old 0-1
                 * default collided with an interleaved video track */
                if (rc<0||rc>255) { rc = s->next_ichan & 255; cc = -1; }
                if (cc<0||cc>255) cc = rc<255 ? rc+1 : 0;
                if (cc+1 > s->next_ichan) s->next_ichan = cc+1;
                s->bc_chan_rtp = rc; s->tcp=1; s->have_bc=1;
                snprintf(bextra,sizeof bextra,
                    "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\nSession: %s;timeout=%d\r\n",
                    rc,cc,s->session,RTSP_SESSION_TIMEOUT_S);
            } else if (tr && strstr(tr,"client_port=")) {
                int cp=0,cp2=0; const char *cpp=strstr(tr,"client_port=");
                sscanf(cpp+12,"%d-%d",&cp,&cp2);
                if (cp<0||cp>65535) cp=0; if (cp2<0||cp2>65535) cp2=0;
                if (s->bc_udp[0]>=0){ close(s->bc_udp[0]); s->bc_udp[0]=-1; }
                if (s->bc_udp[1]>=0){ close(s->bc_udp[1]); s->bc_udp[1]=-1; }
                int base=0, bound=-1;
                for (int t=0;t<64 && bound<0;t++){
                    base = 6000 + ((rand()%8192)&~1);
                    bound = net_bind_udp_pair(&s->bc_udp[0], &s->bc_udp[1], base);
                }
                if (bound<0){ send_err(s, cseq, 500, "Internal Server Error", NULL); return -1; }
                int fl=fcntl(s->bc_udp[0],F_GETFL,0);   /* poll without blocking PLAY */
                if (fl>=0) fcntl(s->bc_udp[0],F_SETFL,fl|O_NONBLOCK);
                s->have_bc=1;
                snprintf(bextra,sizeof bextra,
                    "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d\r\nSession: %s;timeout=%d\r\n",
                    cp,cp2, base,base+1, s->session,RTSP_SESSION_TIMEOUT_S);
            } else {
                /* A4: neither interleaved nor unicast+client_port */
                send_err(s, cseq, 461, "Unsupported Transport", NULL);
                return 0;
            }
            send_resp(s, cseq, bextra, NULL);
            return 0;
        }
#endif
        char extra[256];
        if (tr && strstr(tr,"TCP")) {
            /* interleaved */
            int rc=-1, cc=-1;
            const char *il = strstr(tr,"interleaved=");
            if (il) sscanf(il+12,"%d-%d",&rc,&cc);
            /* A7: no (or bogus) interleaved= from the client -> assign the
             * next free channel pair ourselves (0-1, then 2-3, ...); the old
             * fixed 0-1 default collided video and audio on one channel */
            if (rc<0||rc>255) { rc = s->next_ichan & 255; cc = -1; }
            if (cc<0||cc>255) cc = rc<255 ? rc+1 : 0;
            if (cc+1 > s->next_ichan) s->next_ichan = cc+1;
            rtp_sink *snk = is_audio ? &s->asink : &s->vsink;
            snk->tcp=1; snk->fd=s->fd; snk->chan_rtp=rc; snk->chan_rtcp=cc;
#ifdef USE_TLS
            snk->tls=s->tls;   /* interleaved media rides the (TLS?) control conn */
#endif
            s->tcp=1;
            if (is_audio) s->have_audio=1; else s->have_video=1;
            snprintf(extra,sizeof extra,
                "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\nSession: %s;timeout=%d\r\n",
                rc,cc,s->session,RTSP_SESSION_TIMEOUT_S);
        } else if (tr && strstr(tr,"client_port=")) {
            /* UDP unicast */
            int cp=0, cp2=0;
            const char *cpp = strstr(tr,"client_port=");
            sscanf(cpp+12,"%d-%d",&cp,&cp2);
            if (cp<0||cp>65535) cp=0;        /* N4: reject out-of-range ports */
            if (cp2<0||cp2>65535) cp2=0;
            /* A4: a client_port we can't send to (0 or unparseable) is an
             * unusable transport - 461 now beats a "successful" SETUP that
             * silently streams RTP to port 0 (black picture, no error) */
            if (cp==0) {
                send_err(s, cseq, 461, "Unsupported Transport", NULL);
                return 0;
            }
            if (cp2==0) cp2 = cp<65535 ? cp+1 : 0;   /* lone port: infer RTCP */
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
                send_err(s, cseq, 500, "Internal Server Error", NULL); return -1; }
            rtp_sink *snk = is_audio ? &s->asink : &s->vsink;
            snk->tcp=0; snk->fd=udp[0]; snk->fd_rtcp=udp[1];
            snk->dst=s->peer; snk->dst.sin_port=htons((uint16_t)cp);
            snk->dst_rtcp=s->peer; snk->dst_rtcp.sin_port=htons((uint16_t)cp2);
            if (is_audio) s->have_audio=1; else s->have_video=1;
            snprintf(extra,sizeof extra,
                "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d\r\nSession: %s;timeout=%d\r\n",
                cp,cp2, base,base+1, s->session,RTSP_SESSION_TIMEOUT_S);
        } else {
            /* A4: neither interleaved-TCP nor unicast-UDP with a client_port
             * (missing Transport header, multicast-only offer, garbage).
             * 461 lets a Milestone/Axis-style client fall back to unicast
             * instead of "succeeding" into a black picture. Keep the
             * connection - a retry with a supported transport may follow. */
            send_err(s, cseq, 461, "Unsupported Transport", NULL);
            return 0;
        }
        send_resp(s, cseq, extra, NULL);
        return 0;
    }
    if (!strncmp(req, "PLAY", 4)) {
        /* PLAY without a successful SETUP (or without a video source) */
        int any_track = s->have_video || s->have_audio;
#ifdef USE_BACKCHANNEL
        any_track = any_track || s->have_bc;
#endif
        if ((!any_track) ||
            (s->have_video && s->vchn < 0)) {
            send_err(s, cseq, 455, "Method Not Valid in This State", NULL);
            return -1;
        }
        /* the 200 OK is sent from stream_loop once hub_subscribe succeeded */
        s->play_cseq = cseq;
        return 1;
    }
    if (!strncmp(req, "GET_PARAMETER", 13)) {
        /* A8: before SETUP there is no session - omit the header instead of
         * sending "Session: " with an empty value (formally legal, looks
         * like a bug to log readers and strict parsers alike) */
        char extra[64]; extra[0]=0;
        if (s->session[0])
            snprintf(extra,sizeof extra,"Session: %s\r\n",s->session);
        send_resp(s, cseq, extra, NULL);
        return 0;
    }
    if (!strncmp(req, "TEARDOWN", 8)) {
        send_resp(s, cseq, "", NULL);
        return -1;
    }
    /* RFC 2326: 405 responses must carry Allow with the methods we accept */
    send_err(s, cseq, 405, "Method Not Allowed",
        "Allow: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n");
    return 0;
}

static void stream_loop(session *s)
{
    const ms_config *c = s->cfg;
    /* guard against an invalid video channel (would read c->video[-1]) */
    if (s->have_video &&
        (s->vchn < 0 || s->vchn >= MS_MAX_VSTREAM || !c->video[s->vchn].enabled)) {
        send_err(s, s->play_cseq, 404, "Not Found", NULL);
        return;
    }
    int vc = s->have_video ? c->video[s->vchn].codec : MS_VC_H264;
    int ac = MS_AC_AAC, asr = c->audio.samplerate, ach = c->audio.channels;
    hub_get_audio(&ac, &asr, &ach);      /* actual audio codec from the HAL */

    /* B2: one CNAME for both tracks (RFC 3550 6.5.1 - receivers correlate
     * the session's A/V pair by it): timps@<local-ip>, falling back to the
     * RTSP session id if the socket is already gone. */
    char cname[RTP_CNAME_MAX+1];
    {
        struct sockaddr_in loc; socklen_t sl = sizeof loc;
        char ip[INET_ADDRSTRLEN];
        if (getsockname(s->fd, (struct sockaddr*)&loc, &sl) == 0 &&
            inet_ntop(AF_INET, &loc.sin_addr, ip, sizeof ip))
            snprintf(cname, sizeof cname, "timps@%s", ip);
        else
            snprintf(cname, sizeof cname, "timps@%s", s->session);
    }

    int sub_v = 0, sub_a = 0;
    if (s->have_video) {
        rtp_track_init(&s->vtrack, VIDEO_PT, 90000, c->rtsp_mtu, cname,
                       sink_send, &s->vsink);
        /* P3: batch UDP video packets into sendmmsg; audio stays direct
         * (one packet per frame - nothing to batch). Allocation failure
         * just keeps the per-packet path. */
        if (!s->vsink.tcp)
            s->vsink.batch = (rtp_batch*)calloc(1, sizeof(rtp_batch));
        if (hub_subscribe(s->vchn, &s->q) != 0) goto full;
        sub_v = 1;
        hub_request_idr(s->vchn);
    }
    if (s->have_audio) {
        int apt = (ac==MS_AC_AAC)?AUDIO_PT : (ac==MS_AC_PCMA?8:0);
        int arate = (ac==MS_AC_AAC)?asr:8000;
        rtp_track_init(&s->atrack, apt, arate, c->rtsp_mtu, cname,
                       sink_send, &s->asink);
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
    int64_t drop_idr_us = 0;   /* rate-limit the safety IDR request on any drop */
    int pop_ms = 100;
    /* A3: last proof-of-life from the client. UDP-transport sessions are
     * otherwise undetectably dead: sendto() on an unconnected UDP socket
     * never fails (no ICMP errors delivered), so a client that vanished
     * without TEARDOWN (crash, power loss, network partition) would stream
     * into the void forever and pin one of RTSP_MAX_CLIENTS slots until a
     * daemon restart. Any control-channel bytes (requests, interleaved '$'
     * frames), received backchannel RTP, or an RTCP packet from the peer
     * counts as activity. TCP-interleaved sessions are exempt: a dead TCP
     * peer already trips SO_SNDTIMEO (15 s) on the media writes, while an
     * alive-but-silent one (ffmpeg over TCP sends nothing after PLAY) must
     * NOT be reaped for never sending keepalives. */
    int64_t last_act_us = ms_now_us();
#ifdef USE_BACKCHANNEL
    /* poll faster when receiving a backchannel so speaker audio isn't delayed
     * by up to a full 100ms media-idle tick (~one G.711 packet interval) */
    if (s->have_bc) pop_ms = 20;
#endif
    LOGI(MOD,"PLAY session=%s vchn=%d %s v=%d a=%d", s->session, s->vchn,
         s->tcp?"TCP":"UDP", s->have_video, s->have_audio);

    while (s->playing) {
        /* if the queue overflowed and dropped a keyframe, request a fresh IDR
         * so the client doesn't decode garbage until the next GOP */
        if (sub_v && fanqueue_take_dropped_key(&s->q)) {
            hub_request_idr(s->vchn);
            drop_idr_us = ms_now_us();
        }
        /* a dropped P-frame is silent (no keyframe lost) but still breaks the
         * rest of the GOP for this client: subsequent P-frames reference an AU
         * the decoder never got. Observed as a subject (a cat walking) flicker/
         * vanish mid-motion in a Frigate recording when a weak-WiFi RTSP/TCP
         * session backs up and hits the fanqueue byte cap. Self-heal by asking
         * for a fresh IDR too, but RATE-LIMITED to once/sec (mirrors httpd.c's
         * adaptive-drop): the IDR request is global to the shared encoder, so a
         * chronically slow client must not spike the bitrate for every other
         * subscriber. The keyframe-drop path above resets the timer, so it
         * won't double-fire. */
        else if (sub_v && fanqueue_take_dropped(&s->q)) {
            int64_t now = ms_now_us();
            if (now - drop_idr_us > 1000000) {
                hub_request_idr(s->vchn);
                drop_idr_us = now;
            }
        }
        ms_pkt *p = fanqueue_pop(&s->q, pop_ms);
        if (p) {
            int sendrc = 0;
            if (p->media==MS_MEDIA_VIDEO && s->have_video) {
                if (!got_key) {
                    if (!p->keyframe) { pkt_unref(p); goto after_pkt; }
                    got_key = 1;
                }
                if (vc==MS_VC_H265) sendrc = rtp_send_h265(&s->vtrack,p->data,p->len,p->pts_us);
                else                sendrc = rtp_send_h264(&s->vtrack,p->data,p->len,p->pts_us);
                /* P3: one access unit done - push the whole batch out in a
                 * single sendmmsg (no-op on TCP / unbatched sinks) */
                if (sendrc >= 0 && sink_flush(&s->vsink) < 0) sendrc = -1;
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
#ifdef USE_BACKCHANNEL
        /* drain any received backchannel RTP (UDP). Only accept from the RTSP
         * peer's address so a stranger can't inject audio into the speaker. */
        if (s->have_bc && s->bc_udp[0] >= 0){
            uint8_t bpkt[1600];
            for (int k=0;k<16;k++){
                struct sockaddr_in from; socklen_t fl=sizeof from;
                ssize_t bn = recvfrom(s->bc_udp[0], bpkt, sizeof bpkt, MSG_DONTWAIT,
                                      (struct sockaddr*)&from, &fl);
                if (bn <= 0) break;
                if (from.sin_addr.s_addr != s->peer.sin_addr.s_addr) continue;
                last_act_us = ms_now_us();   /* A3: talk-only UDP sessions */
                bc_feed_rtp(s, bpkt, (int)bn);
            }
        }
#endif
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
            last_act_us = now;                   /* A3: client is alive */
            ctlhave += n; ctl[ctlhave]=0;
            int close_conn = 0, progressed;
            do {
                progressed = 0;
                if (ctlhave >= 4 && ctl[0]=='$') {
                    int flen = ((unsigned char)ctl[2]<<8)|(unsigned char)ctl[3];
                    int total = 4 + flen;
                    if (total > (int)sizeof(ctl)-1) { close_conn = 1; break; } /* bogus length */
                    if (ctlhave < total) break;              /* wait for the rest */
#ifdef USE_BACKCHANNEL
                    /* interleaved backchannel RTP arrives on the control conn */
                    if (s->have_bc && s->bc_chan_rtp>=0 &&
                        (unsigned char)ctl[1]==(unsigned char)s->bc_chan_rtp)
                        bc_feed_rtp(s, (uint8_t*)ctl+4, flen);
#endif
                    memmove(ctl, ctl+total, (size_t)(ctlhave-total+1));
                    ctlhave -= total; progressed = 1;
                    continue;
                }
                if (ctlhave > 0 && ctl[0]!='$') {
                    char *end = strstr(ctl, "\r\n\r\n");
                    if (!end) break;                          /* incomplete headers, wait */
                    size_t hdrlen = (size_t)(end-ctl) + 4;
                    /* also consume any Content-Length entity body (e.g. a
                     * SET_PARAMETER text body) - leftover body bytes would
                     * otherwise be read as the next request's method line and
                     * desync every following request on this connection. Bound
                     * the length parse to the header block so a pipelined next
                     * request's Content-Length can't be picked up here. */
                    int clen; { char save=ctl[hdrlen]; ctl[hdrlen]=0;
                                clen=hdr_int(ctl,"Content-Length",0); ctl[hdrlen]=save; }
                    if (clen < 0) clen = 0;                   /* malformed -> no body */
                    size_t reqlen = hdrlen + (size_t)clen;
                    if (reqlen > (size_t)sizeof(ctl)-1) { close_conn=1; break; } /* body too big */
                    if ((size_t)ctlhave < reqlen) break;      /* body not fully arrived, wait */
                    if (!session_matches(s, ctl)) {
                        /* A5: a request naming some OTHER session must not act
                         * on this one - notably a mismatched TEARDOWN must not
                         * tear the running stream down (RFC 2326 12.37) */
                        send_err(s, hdr_int(ctl,"CSeq",0), 454,
                                 "Session Not Found", NULL);
                    } else
                    if (!strncmp(ctl,"TEARDOWN",8)) {
                        /* B3: clean teardown - tell RTCP-aware receivers the
                         * sender is leaving (RFC 3550 6.3.7, compound
                         * SR|RR+SDES+BYE), then answer before closing - RTSP
                         * is strictly request/response, and live555-derived
                         * clients wait for the 200 instead of treating the
                         * close as one. Best-effort either way. */
                        int64_t bnow = ms_now_us();
                        if (s->have_video) rtp_send_bye(&s->vtrack, bnow);
                        if (s->have_audio) rtp_send_bye(&s->atrack, bnow);
                        char e[64]; snprintf(e,sizeof e,"Session: %s\r\n",s->session);
                        send_resp(s, hdr_int(ctl,"CSeq",0), e, NULL);
                        close_conn = 1; break;
                    }
                    else
                    if (!strncmp(ctl,"GET_PARAMETER",13)||!strncmp(ctl,"OPTIONS",7)) {
                        int cseq=hdr_int(ctl,"CSeq",0);
                        char e[64]; snprintf(e,sizeof e,"Session: %s\r\n",s->session);
                        send_resp(s,cseq,e,NULL);
                    } else {
                        /* A2: anything else (PAUSE, SET_PARAMETER, mid-session
                         * DESCRIBE, ...) used to be dropped without ANY
                         * response - RTSP is strictly request/response, so
                         * VLC's pause button hung until its timeout and
                         * SET_PARAMETER-keepalive NVRs declared the session
                         * dead. Answer 405 listing what IS supported here;
                         * the media session keeps running. */
                        int cseq=hdr_int(ctl,"CSeq",0);
                        char e[128]; snprintf(e,sizeof e,
                            "Allow: OPTIONS, GET_PARAMETER, TEARDOWN\r\n"
                            "Session: %s\r\n", s->session);
                        send_err(s, cseq, 405, "Method Not Allowed", e);
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

        /* A3: UDP-transport liveness. Drain the server RTCP sockets - the
         * kernel otherwise just fills and drops (B3), and a receiver report
         * from the peer is the natural "still listening" signal for clients
         * that keep RTCP running but skip RTSP keepalives. Contents are not
         * parsed; a datagram from the peer's address is proof enough. */
        if (!s->tcp) {
            for (int t = 0; t < 2; t++) {
                int rfd = t ? s->a_udp[1] : s->v_udp[1];
                if (rfd < 0) continue;
                uint8_t rr[512];
                struct sockaddr_in from; socklen_t fl = sizeof from;
                while (recvfrom(rfd, rr, sizeof rr, MSG_DONTWAIT,
                                (struct sockaddr*)&from, &fl) > 0) {
                    if (from.sin_addr.s_addr == s->peer.sin_addr.s_addr)
                        last_act_us = now;
                    fl = sizeof from;
                }
            }
            if (now - last_act_us >
                (int64_t)RTSP_SESSION_TIMEOUT_S * 2 * 1000000) {
                LOGW(MOD,"session=%s idle >%ds (UDP client gone without "
                     "TEARDOWN), reaping", s->session, RTSP_SESSION_TIMEOUT_S*2);
                break;
            }
        }
    }

    if (sub_v) hub_unsubscribe(s->vchn, &s->q);
    if (sub_a) hub_unsubscribe(HUB_AUDIO_SRC, &s->q);
    free(s->vsink.batch); s->vsink.batch = NULL;   /* P3 */
    return;

full:
    /* source subscriber table full (> HUB_MAX_SUBS consumers) */
    if (sub_v) hub_unsubscribe(s->vchn, &s->q);
    free(s->vsink.batch); s->vsink.batch = NULL;   /* P3 */
    send_err(s, s->play_cseq, 503, "Service Unavailable", NULL);
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
            size_t hdrlen = (size_t)(end - buf) + 4;
            /* A request may carry an entity body (RFC 2326 allows a body on
             * e.g. SET_PARAMETER, which some NVR stacks send unconditionally).
             * Consume exactly Content-Length body bytes too - otherwise the
             * leftover body is mis-parsed as the next request's method line and
             * every subsequent request on this connection desyncs. Parse the
             * length from the header block only (temporarily NUL-terminating at
             * its end) so a pipelined next request's Content-Length can't leak
             * into this one. No Content-Length -> zero body (bodyless requests
             * are completely unaffected). */
            int clen; { char save=buf[hdrlen]; buf[hdrlen]=0;
                        clen=hdr_int(buf,"Content-Length",0); buf[hdrlen]=save; }
            if (clen < 0) clen = 0;                 /* malformed -> no body */
            size_t reqlen = hdrlen + (size_t)clen;
            if (reqlen > sizeof(buf)-1) goto done;  /* body too large to buffer */
            if ((size_t)have < reqlen) break;       /* body not fully arrived yet */
            char req[4096];
            memcpy(req, buf, hdrlen); req[hdrlen]=0; /* hand off headers only */
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
#ifdef USE_BACKCHANNEL
    bc_release(s);
    if (s->bc_udp[0]>=0) close(s->bc_udp[0]);
    if (s->bc_udp[1]>=0) close(s->bc_udp[1]);
#endif
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
        int cfd = net_accept_cloexec(lfd, (struct sockaddr*)&peer, &pl);
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
#ifdef USE_BACKCHANNEL
        s->bc_udp[0]=s->bc_udp[1]=-1; s->bc_chan_rtp=-1;
#endif
#ifdef USE_TLS
        s->tls_ctx = tls_ctx;   /* non-NULL on the RTSPS listener */
#endif
        pthread_t t;
        if (ms_thread_create(&t,MS_STACK_CONN,client_thread,s)==0) pthread_detach(t);
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
    ms_thread_create(&s->thr, MS_STACK_UTIL, accept_thread, s);
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
            else if (ms_thread_create(&s->thr_tls, MS_STACK_UTIL, accept_tls_thread, s) != 0){
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
