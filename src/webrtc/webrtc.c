/* webrtc.c - WHEP signalling + the ICE-lite/DTLS/SRTP session (see webrtc.h). */
#ifdef USE_WEBRTC
#include "webrtc.h"
#include "stun.h"
#include "dtls.h"
#include "srtp.h"
#include "../log.h"
#include "../util.h"
#include "../auth.h"
#include "../hub.h"
#include "../frame.h"
#include "../fanqueue.h"
#include "../codec/vparam.h"
#include "../rtsp/rtp.h"

#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

/* One browser tab is one session; the cap bounds threads, sockets and - now
 * that media flows - hub subscriptions. */
#ifndef WEBRTC_MAX_SESSIONS
#define WEBRTC_MAX_SESSIONS 4
#endif
/* ICE + DTLS on a LAN complete in tens of milliseconds; anything still
 * unfinished this late is a peer that went away mid-negotiation. */
#define WEBRTC_SETUP_US (30LL * 1000000)
/* A connected browser sends STUN consent checks every few seconds
 * (RFC 7675), so silence this long means the tab is gone. */
#define WEBRTC_IDLE_US  (30LL * 1000000)
/* The conventional WebRTC packet size, well under any path MTU once the
 * 10-byte SRTP tag, UDP and IP are added - and far enough below RTP_MTU_MAX
 * that the tag can never push a packet past what the sink buffer holds. */
#define WEBRTC_MTU      1200
#define WEBRTC_QCAP     16
/* How long the media loop parks on the fanqueue before looping round to the
 * socket again: the upper bound on reacting to a PLI or a consent check. */
#define WEBRTC_POP_MS   20
/* How long DELETE waits for the session thread to actually finish releasing
 * before answering, so a 200 means "gone", not "asked to go". Comfortably
 * above the loop's own longest blocking wait (250 ms pre-DTLS). */
#define WEBRTC_DELETE_WAIT_MS 600
/* m-sections accepted in one offer. A WHEP viewer offers video, or video +
 * audio; the slack is for an offer that also carries an m=application
 * (data channel), which is answered rejected rather than refused outright. */
#define WEBRTC_MAX_MSEC 4

typedef struct {
    volatile int       used;
    volatile int       run;
    unsigned           gen;               /* slot generation, for DELETE's wait */
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
    uint8_t            peer_fp[32];       /* the offer's a=fingerprint (sha-256) */
    /* media */
    int                chn;               /* hub video source */
    int                pt;                /* negotiated H264 payload type */
    uint32_t           ssrc;              /* promised in the answer's a=ssrc */
    int                have_audio;
    int                asrc;              /* HUB_AUDIO_SRC or HUB_AUDIO_SRC2 */
    int                apt;               /* negotiated G.711 payload type */
    uint32_t           assrc;
    srtp_session       srtp;
    rtp_track          vtrack, atrack;
    fanqueue           q;
    int                qinit, subbed, subbed_a;
    int64_t            last_idr_us;
} wrtc_session;

static wrtc_session   g_sess[WEBRTC_MAX_SESSIONS];
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static unsigned        g_gen_ctr;
static ms_dtls_ctx    *g_dtls_ctx;
static int             g_port_cfg;
static int             g_port_max;
static int             g_chn_cfg;

int webrtc_available(void) { return g_dtls_ctx != NULL; }

/* ---------------- SDP helpers (line-anchored, CRLF or LF) ---------------- */

/* Every lookup is bounded by [beg,end): an offer has several m-sections now,
 * and a session-level scan that ran into the next one would answer with
 * another media's mid, ufrag or rtpmap. `end` NULL means "to the NUL". */
static const char *sdp_line_n(const char *beg, const char *end, const char *pre)
{
    size_t n = strlen(pre);
    const char *p = beg;
    while (p && *p && (!end || p < end)) {
        if ((!end || p + (ptrdiff_t)n <= end) && !strncmp(p, pre, n)) return p + n;
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

typedef struct {
    const char *beg, *end;      /* the m= line through the last of its attrs */
    const char *fmts;           /* first payload type in the m= line */
    char        kind[12];       /* "video" / "audio" / "application" / ... */
    char        proto[48];      /* "UDP/TLS/RTP/SAVPF" */
    char        mid[32];
    int         accept;         /* answered with a real port, not 0 */
} sdp_msec;

typedef struct {
    const char *sdp;
    const char *sess_end;       /* start of the first m= line */
    sdp_msec    m[WEBRTC_MAX_MSEC];
    int         nm;
} sdp_offer;

/* An attribute of one m-section, falling back to the session level - which is
 * where Firefox puts a=fingerprint and where either browser may put
 * a=ice-ufrag/a=ice-pwd (RFC 8859). */
static const char *sdp_attr(const sdp_offer *o, const sdp_msec *m,
                            const char *pre)
{
    const char *v = sdp_line_n(m->beg, m->end, pre);
    return v ? v : sdp_line_n(o->sdp, o->sess_end, pre);
}

/* Split the offer into its m-sections. Returns the count, or -1 if there are
 * none or more than WEBRTC_MAX_MSEC (an answer must mirror every one of them,
 * so an offer we cannot mirror is refused rather than half-answered). */
static int sdp_parse(sdp_offer *o, const char *sdp)
{
    memset(o, 0, sizeof *o);
    o->sdp = sdp;
    const char *p = sdp;
    while (p && *p) {
        if (!strncmp(p, "m=", 2)) {
            if (!o->sess_end) o->sess_end = p;
            if (o->nm >= WEBRTC_MAX_MSEC) return -1;
            sdp_msec *m = &o->m[o->nm++];
            m->beg = p;
            /* Every step stays inside THIS line: a truncated "m=video" with no
             * port would otherwise let the plain skip-to-space walk on into
             * the next line and read its words as this section's proto. */
            const char *q = p + 2;
            q += sdp_tok(q, m->kind, sizeof m->kind);
            while (*q == ' ') q++;
            while (*q && *q != ' ' && *q != '\r' && *q != '\n') q++;  /* port */
            while (*q == ' ') q++;
            q += sdp_tok(q, m->proto, sizeof m->proto);
            while (*q == ' ') q++;
            m->fmts = q;
            if (o->nm > 1) o->m[o->nm - 2].end = p;
        }
        const char *e = strchr(p, '\n');
        p = e ? e + 1 : NULL;
    }
    if (!o->nm) return -1;
    o->m[o->nm - 1].end = sdp + strlen(sdp);
    return o->nm;
}

/* Is `mid` one of the mids the offer's a=group:BUNDLE line lists? Two media
 * over one ICE transport is only legal if the offerer asked for it. */
static int bundle_has(const sdp_offer *o, const char *mid)
{
    const char *g = sdp_line_n(o->sdp, o->sess_end, "a=group:BUNDLE");
    if (!g) return 0;
    char line[256], tok[32];
    sdp_rest(g, line, sizeof line);
    for (const char *p = line; *p; ) {
        while (*p == ' ') p++;
        if (!*p) break;
        p += sdp_tok(p, tok, sizeof tok);
        if (!strcmp(tok, mid)) return 1;
    }
    return 0;
}

/* An RTP payload type is 7 bits of decimal (RFC 3550 5.1). atoi() alone would
 * turn a non-numeric format token - "webrtc-datachannel", or a hostile
 * "a=rtpmap:abc PCMU/8000" - into payload type 0, which happens to be PCMU's
 * static assignment. Returns -1 for anything that is not a plain 0..127. */
static int pt_num(const char *s)
{
    if (!s[0]) return -1;
    for (int i = 0; s[i]; i++) if (s[i] < '0' || s[i] > '9') return -1;
    int v = atoi(s);
    return (v >= 0 && v <= 127) ? v : -1;
}

/* value of `key=` inside the a=fmtp line of `pt` within one m-section, or "" */
static void fmtp_param(const sdp_msec *ms, const char *pt, const char *key,
                       char *out, int cap)
{
    char want[32], line[512];
    out[0] = 0;
    snprintf(want, sizeof want, "a=fmtp:%s ", pt);
    const char *f = sdp_line_n(ms->beg, ms->end, want);
    if (!f || sdp_rest(f, line, sizeof line) <= 0) return;
    size_t kl = strlen(key);
    for (const char *p = line; p; ) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            int i = 0;
            p += kl + 1;
            while (p[i] && p[i] != ';' && i < cap - 1) { out[i] = p[i]; i++; }
            out[i] = 0;
            return;
        }
        p = strchr(p, ';');
        if (p) p++;
    }
}

/* Pick the offered payload type to actually send H264 on.
 *
 * Milestone 1 echoed the offer's FIRST payload type, which on Chrome is VP8 -
 * harmless while nothing was sent, fatal now. A browser offers H264 several
 * times over, once per profile-level-id/packetization-mode combination, and
 * only the entry whose profile matches what the encoder is really producing
 * will decode. So: require packetization-mode=1 (FU-A, the only thing
 * rtp_send_h264() emits), then prefer an exact profile-level-id match against
 * the live SPS, then a matching profile_idc, then anything.
 * Returns the payload type, or -1 when the offer carries no usable H264. */
static int pick_h264_pt(const sdp_msec *ms, const vparam *vp, char *pt_out,
                        int pt_cap)
{
    char ours[8];
    snprintf(ours, sizeof ours, "%02X%02X%02X", vp->sps[1], vp->sps[2],
             vp->sps[3]);
    int best = -1, best_score = 0;
    char pt[8];
    const char *m = ms->fmts;
    while (*m) {
        while (*m == ' ') m++;
        if (!*m || *m == '\r' || *m == '\n') break;
        m += sdp_tok(m, pt, sizeof pt);
        int ptn = pt_num(pt);
        if (ptn < 0) continue;
        char want[32], rm[64];
        snprintf(want, sizeof want, "a=rtpmap:%s ", pt);
        const char *r = sdp_line_n(ms->beg, ms->end, want);
        if (!r || sdp_rest(r, rm, sizeof rm) <= 0) continue;
        if (strncasecmp(rm, "H264/90000", 10)) continue;
        char mode[8], plid[16];
        fmtp_param(ms, pt, "packetization-mode", mode, sizeof mode);
        if (atoi(mode) != 1) continue;
        fmtp_param(ms, pt, "profile-level-id", plid, sizeof plid);
        int score = 1;
        if (strlen(plid) == 6) {
            if (!strncasecmp(plid, ours, 4)) score = 3;      /* profile+iop */
            else if (!strncasecmp(plid, ours, 2)) score = 2; /* profile_idc */
        }
        if (score > best_score) {
            best_score = score;
            best = ptn;
            snprintf(pt_out, (size_t)pt_cap, "%s", pt);
        }
        if (best_score == 3) break;
    }
    /* Normal with Chrome, which offers no High-profile H264 at all: it still
     * plays, because a receiver initialises its decoder from the in-band SPS
     * rather than from the SDP. Firefox, which really is baseline-only, will
     * not - hence the line. */
    if (best >= 0 && best_score < 2)
        LOGI(MOD, "no offered H264 matches profile %s - answering pt %s with "
                  "ours", ours, pt_out);
    return best;
}

/* Same idea for audio, with one codec instead of a profile ladder: find the
 * payload type the offer maps to the G.711 flavour this daemon actually
 * encodes. Returns -1 when the offer carries none - the answer then rejects
 * the audio m-section rather than negotiating a codec nothing is sent on. */
static int pick_g711_pt(const sdp_msec *ms, int acodec)
{
    const char *want_rtpmap = (acodec == MS_AC_PCMA) ? "PCMA/8000" : "PCMU/8000";
    int static_pt = (acodec == MS_AC_PCMA) ? 8 : 0;
    char pt[8];
    const char *m = ms->fmts;
    while (*m) {
        while (*m == ' ') m++;
        if (!*m || *m == '\r' || *m == '\n') break;
        m += sdp_tok(m, pt, sizeof pt);
        int ptn = pt_num(pt);
        if (ptn < 0) continue;
        char want[32], rm[64];
        snprintf(want, sizeof want, "a=rtpmap:%s ", pt);
        const char *r = sdp_line_n(ms->beg, ms->end, want);
        if (r && sdp_rest(r, rm, sizeof rm) > 0) {
            if (!strncasecmp(rm, want_rtpmap, 9)) return ptn;
            continue;
        }
        /* No rtpmap: RFC 3551's static assignment still stands (0 = PCMU,
         * 8 = PCMA), and it is what a minimal non-browser WHEP client sends. */
        if (ptn == static_pt) return static_pt;
    }
    return -1;
}

/* "AA:BB:..." (RFC 8122) -> 32 raw bytes. Returns 0 on success. Rejects
 * anything but sha-256: it is what every browser offers, and quietly
 * accepting an algorithm we then compare with SHA-256 would pass every
 * certificate. */
static int parse_fingerprint(const char *line, uint8_t out[32])
{
    char alg[16];
    int n = sdp_tok(line, alg, sizeof alg);
    if (n <= 0 || strcasecmp(alg, "sha-256")) return -1;
    const char *p = line + n;
    while (*p == ' ') p++;
    int i = 0;
    while (i < 32) {
        int hi = -1, lo = -1;
        for (int k = 0; k < 2; k++) {
            char c = *p++;
            int v;
            if      (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else return -1;
            if (k) lo = v; else hi = v;
        }
        out[i++] = (uint8_t)((hi << 4) | lo);
        if (i == 32) break;
        if (*p++ != ':') return -1;
    }
    /* exactly 32 bytes, nothing trailing but end-of-line */
    return (*p == 0 || *p == '\r' || *p == '\n') ? 0 : -1;
}

/* ---------------- bounded answer builder ---------------- */

typedef struct { char *p; int cap, len, ovf; } sdpbuf;

static void sb_add(sdpbuf *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void sb_add(sdpbuf *b, const char *fmt, ...)
{
    if (b->ovf) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->p + b->len, (size_t)(b->cap - b->len), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= b->cap - b->len) { b->ovf = 1; return; }
    b->len += n;
}

/* ---------------- session ---------------- */

static int same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static void sess_release(wrtc_session *s)
{
    if (s->subbed)   { hub_unsubscribe(s->chn, &s->q); s->subbed = 0; }
    if (s->subbed_a) { hub_unsubscribe(s->asrc, &s->q); s->subbed_a = 0; }
    if (s->qinit)  { fanqueue_free(&s->q); s->qinit = 0; }
    if (s->dtls) { ms_dtls_free(s->dtls); s->dtls = NULL; }
    if (s->fd >= 0) { close(s->fd); s->fd = -1; }
    s->have_peer = 0;
    memset(&s->srtp, 0, sizeof s->srtp);
    __sync_synchronize();
    s->used = 0;
}

/* rtp_out_fn. The hdr/pay split that lets rtsp.c hand the kernel a
 * scatter/gather write is of no use here: SRTP authenticates and encrypts one
 * contiguous buffer, so both halves are copied into this stack packet first.
 * That copy is the deliberate cost of the encrypted transport, and it is why
 * this sink does no rtp_batch staging either - there is nothing left to keep
 * zero-copy. */
static int wrtc_rtp_out(void *ctx, const uint8_t *hdr, int hlen,
                        const uint8_t *pay, int plen, int rtcp)
{
    wrtc_session *s = (wrtc_session *)ctx;
    uint8_t buf[RTP_MTU_MAX + SRTP_MAX_OVERHEAD];
    if (hlen < 0 || plen < 0 ||
        hlen + plen > (int)sizeof buf - SRTP_MAX_OVERHEAD) return -1;
    memcpy(buf, hdr, (size_t)hlen);
    if (plen) memcpy(buf + hlen, pay, (size_t)plen);
    int n = rtcp ? srtp_protect_rtcp(&s->srtp, buf, hlen + plen, (int)sizeof buf)
                 : srtp_protect_rtp (&s->srtp, buf, hlen + plen, (int)sizeof buf);
    if (n < 0) return -1;
    ssize_t r;
    do {
        r = sendto(s->fd, buf, (size_t)n, 0,
                   (struct sockaddr *)&s->peer, sizeof s->peer);
    } while (r < 0 && errno == EINTR);
    /* A UDP send error (ENOBUFS, a transient EHOSTUNREACH) abandons the rest
     * of this access unit but never the session - a WebRTC peer that is really
     * gone is caught by the consent-check idle timeout instead. */
    return r < 0 ? -1 : 0;
}

static void handle_rtcp(wrtc_session *s, const uint8_t *p, int len)
{
    int off = 0;
    while (off + 4 <= len) {
        int l = ((((int)p[off + 2] << 8) | p[off + 3]) + 1) * 4;
        if (l < 4 || off + l > len) break;
        int pt = p[off + 1], fmt = p[off] & 0x1F;
        /* PSFB (RFC 4585): FMT 1 = PLI, FMT 4 = FIR (RFC 5104 4.3.1). Both
         * mean "I need a fresh keyframe". Rate-limited like rtsp.c's
         * overflow-heal path: the IDR request hits the shared encoder, so one
         * unhappy browser must not spike the bitrate for every subscriber. */
        if (pt == 206 && (fmt == 1 || fmt == 4)) {
            int64_t now = ms_now_us();
            if (now - s->last_idr_us > 1000000) {
                s->last_idr_us = now;
                LOGD(MOD, "%s: %s - requesting IDR", s->id,
                     fmt == 1 ? "PLI" : "FIR");
                hub_request_idr_recovery(s->chn);
            }
        }
        off += l;
    }
}

static int sess_start_media(wrtc_session *s)
{
    /* RFC 8827 6.5: the certificate the peer just proved possession of must be
     * the one the offer announced. Without this the DTLS handshake only proves
     * that SOMETHING holding our ice-pwd is on the other end. */
    uint8_t fp[32];
    if (ms_dtls_peer_fingerprint(s->dtls, fp) != 0) return -1;
    if (memcmp(fp, s->peer_fp, sizeof fp) != 0) {
        LOGW(MOD, "%s: peer certificate does not match the offer's "
                  "a=fingerprint - dropping", s->id);
        return -1;
    }

    uint8_t km[SRTP_KEYING_LEN];
    if (ms_dtls_export_srtp(s->dtls, km, sizeof km) != 0) return -1;
    /* We answered a=setup:passive, so we are the DTLS server and protect with
     * the server half of the exported keying material. */
    int r = srtp_init(&s->srtp, km, sizeof km, 1);
    memset(km, 0, sizeof km);
    if (r != 0) return -1;

    rtp_track_init(&s->vtrack, s->pt, 90000, WEBRTC_MTU, "timps",
                   wrtc_rtp_out, s);
    s->vtrack.ssrc = s->ssrc;      /* the a=ssrc the answer already promised */
    if (s->have_audio) {
        rtp_track_init(&s->atrack, s->apt, 8000, WEBRTC_MTU, "timps",
                       wrtc_rtp_out, s);
        s->atrack.ssrc = s->assrc;
    }

    if (fanqueue_init(&s->q, WEBRTC_QCAP) != 0) return -1;
    s->qinit = 1;
    hub_count_drops(&s->q, s->chn, HUB_DROP_WEBRTC);
    if (hub_subscribe(s->chn, &s->q) != 0) return -1;
    s->subbed = 1;
    /* One queue for both sources, exactly as an RTSP session does it: packets
     * carry their own media tag, so the ordering the hub published in is the
     * ordering that goes on the wire. */
    if (s->have_audio) {
        if (hub_subscribe(s->asrc, &s->q) != 0) return -1;
        s->subbed_a = 1;
    }
    /* Same reason RTSP's PLAY does it: without a keyframe now the tab stays
     * black until the next scheduled IDR. */
    hub_request_idr(s->chn);
    s->last_idr_us = ms_now_us();
    return 0;
}

static void *sess_thread(void *arg)
{
    wrtc_session *s = (wrtc_session *)arg;
    uint8_t buf[2048];
    int64_t born = ms_now_us(), last_rx = born;
    int connected = 0, got_key = 0;

    while (s->run) {
        struct pollfd p = { .fd = s->fd, .events = POLLIN, .revents = 0 };
        /* Once media flows the blocking wait moves to the fanqueue below, so
         * the socket is only drained, not waited on. */
        int pr = poll(&p, 1, connected ? 0 : ((s->dtls) ? 20 : 250));
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr > 0 && (p.revents & POLLIN)) {
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            int n = (int)recvfrom(s->fd, buf, sizeof buf, 0,
                                  (struct sockaddr *)&from, &fl);
            if (n > 0) {
                /* RFC 7983 demux on the first byte: 0-3 STUN, 20-63 DTLS,
                 * 128-191 RTP/RTCP. */
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
                    /* SECURITY INVARIANT (see the cookie note in dtls.c): DTLS
                     * is only ever fed from an address that already passed
                     * stun_parse_request()'s MESSAGE-INTEGRITY check above,
                     * which is what makes HelloVerifyRequest redundant here.
                     * Do not relax this condition. */
                    if (s->have_peer && same_addr(&from, &s->peer)) {
                        last_rx = ms_now_us();
                        if (!s->dtls)
                            s->dtls = ms_dtls_new(g_dtls_ctx, s->fd, &s->peer);
                        if (s->dtls && !connected)
                            ms_dtls_feed(s->dtls, buf, n);
                    }
                } else if (buf[0] >= 128 && buf[0] <= 191) {
                    /* rtcp-mux (RFC 5761 4): the second byte separates RTCP
                     * packet types from RTP payload types. A recvonly browser
                     * sends only RTCP, so anything else is dropped. */
                    if (connected && n >= 2 && buf[1] >= 192 && buf[1] <= 223 &&
                        s->have_peer && same_addr(&from, &s->peer)) {
                        int pl = srtp_unprotect_rtcp(&s->srtp, buf, n);
                        if (pl > 0) {
                            last_rx = ms_now_us();
                            handle_rtcp(s, buf, pl);
                        }
                    }
                }
            }
        }
        if (s->dtls && !connected) {
            int r = ms_dtls_handshake(s->dtls);
            if (r == 0) {
                if (sess_start_media(s) != 0) {
                    LOGW(MOD, "%s: DTLS up but media setup failed", s->id);
                    break;
                }
                connected = 1;
                LOGI(MOD, "%s: DTLS+SRTP up, streaming video%d pt=%d ssrc=%u%s",
                     s->id, s->chn, s->pt, s->ssrc,
                     s->have_audio ? " + G.711 audio" : "");
            } else if (r < 0) {
                break;
            }
        }
        int64_t now = ms_now_us();
        if (connected) {
            fq_status qs;
            ms_pkt *pk = fanqueue_pop_ex(&s->q, WEBRTC_POP_MS, &qs);
            if (qs.closed) { pkt_unref(pk); break; }
            now = ms_now_us();
            /* only a video eviction breaks the GOP. Not gated on last_idr_us:
             * that throttles PLI/FIR, and a drop inside its window would go
             * unhealed - the hub rate-limits recovery per stream and
             * coalesces instead. */
            if (qs.dropped_video) hub_request_idr_recovery(s->chn);
            if (pk) {
                if (pk->media == MS_MEDIA_VIDEO) {
                    if (!got_key && pk->keyframe) got_key = 1;
                    if (got_key &&
                        rtp_send_h264(&s->vtrack, pk->data, pk->len,
                                      pk->pts_us) >= 0)
                        rtp_sr_anchor(&s->vtrack,
                                      pk->enq_us > 0 ? pk->enq_us : now);
                } else if (pk->media == MS_MEDIA_AUDIO && s->have_audio) {
                    if (rtp_send_g711(&s->atrack, pk->data, pk->len,
                                      pk->pts_us) >= 0)
                        rtp_sr_anchor(&s->atrack,
                                      pk->enq_us > 0 ? pk->enq_us : now);
                }
                /* Nothing defers a send here (no batching), so the packet
                 * reference can go back as soon as rtp_send_* returns. */
                pkt_unref(pk);
            }
            rtp_maybe_sr(&s->vtrack, now);
            if (s->have_audio) rtp_maybe_sr(&s->atrack, now);
        }
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
    if (s->subbed) {
        int64_t bnow = ms_now_us();
        rtp_send_bye(&s->vtrack, bnow);
        if (s->subbed_a) rtp_send_bye(&s->atrack, bnow);
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

/* Every session owns its own socket, so a single fixed webrtc.port would serve
 * exactly one viewer - the next bind() gets EADDRINUSE. Walk the configured
 * range instead (by default webrtc.port .. webrtc.port + cap - 1, so the range
 * is as wide as the session table). Port 0 stays one ephemeral bind: the OS
 * hands out a free port every time. */
static int udp_bind_session(void)
{
    if (!g_port_cfg) return udp_bind(0);
    for (int p = g_port_cfg; p <= g_port_max; p++) {
        int fd = udp_bind(p);
        if (fd >= 0) return fd;
    }
    return -1;
}

/* ---------------- WHEP ---------------- */

int webrtc_whep(const char *offer, const char *local_ip, int req_chn,
                char *ans, int anscap, char *sid, int sidcap)
{
    if (!g_dtls_ctx) return 404;
    if (!offer || strncmp(offer, "v=0", 3)) return 400;

    sdp_offer off;
    if (sdp_parse(&off, offer) < 0) return 400;

    /* The one m-section that must be answerable: video. */
    sdp_msec *vm = NULL;
    for (int i = 0; i < off.nm; i++)
        if (!strcmp(off.m[i].kind, "video")) { vm = &off.m[i]; break; }
    if (!vm) return 400;
    /* "UDP/TLS/RTP/SAVPF" and friends. A plain RTP/AVP offer would be echoed
     * back and then wait forever for a DTLS handshake that is not coming. */
    if (!strstr(vm->proto, "SAVP")) return 400;

    char rufrag[64] = "";
    const char *p;
    if ((p = sdp_attr(&off, vm, "a=ice-ufrag:")) == NULL) return 400;
    sdp_tok(p, rufrag, sizeof rufrag);
    if (!rufrag[0]) return 400;
    /* We never send a Binding Request (ICE-lite), so the remote pwd is never
     * used - but an offer without one is malformed, and answering it would
     * leave the browser's checks unanswerable for a reason we chose not to
     * notice. */
    if (!sdp_attr(&off, vm, "a=ice-pwd:")) return 400;

    uint8_t peer_fp[32];
    if ((p = sdp_attr(&off, vm, "a=fingerprint:")) == NULL ||
        parse_fingerprint(p, peer_fp) != 0) {
        LOGW(MOD, "offer has no usable sha-256 a=fingerprint");
        return 400;
    }
    /* We answer a=setup:passive, so the offerer has to be willing to be the
     * DTLS client. A passive offerer would leave both ends waiting. */
    {
        char setup[16] = "";
        if ((p = sdp_attr(&off, vm, "a=setup:")) != NULL)
            sdp_tok(p, setup, sizeof setup);
        if (setup[0] && strcmp(setup, "actpass") && strcmp(setup, "active")) {
            LOGW(MOD, "offer wants a=setup:%s - we can only be passive", setup);
            return 400;
        }
    }

    for (int i = 0; i < off.nm; i++)
        if (!off.m[i].mid[0]) {
            const char *mp = sdp_line_n(off.m[i].beg, off.m[i].end, "a=mid:");
            if (mp) sdp_tok(mp, off.m[i].mid, sizeof off.m[i].mid);
            if (!off.m[i].mid[0]) snprintf(off.m[i].mid, sizeof off.m[i].mid,
                                           "%d", i);
        }

    int chn = g_chn_cfg;
    if (req_chn >= 0) {
        /* an explicit ?chn= for a stream that is not running is the client's
         * mistake, not a warm-up to wait out */
        if (req_chn >= MS_MAX_VSTREAM ||
            !hub_get_video_params(req_chn, NULL, NULL, NULL, NULL)) {
            LOGW(MOD, "offer asks for video%d, which is not running", req_chn);
            return 400;
        }
        chn = req_chn;
    }
    {
        int vc = -1;
        if (hub_get_video_params(chn, &vc, NULL, NULL, NULL) && vc != MS_VC_H264) {
            LOGW(MOD, "video%d is not H264 - WebRTC carries H264 only", chn);
            return 400;
        }
    }

    /* Same warm-up RTSP's DESCRIBE does: encoding is on-demand, so on a fresh
     * boot the SPS the answer's profile-level-id and sprop-parameter-sets come
     * from does not exist until something subscribes. */
    vparam vp;
    int vready = hub_get_vparam(chn, &vp) && vparam_ready(&vp);
    if (!vready) {
        fanqueue wq;
        int winit = fanqueue_init(&wq, 4) == 0;
        int wsub = winit && hub_subscribe(chn, &wq) == 0;
        hub_request_idr(chn);
        for (int i = 0; i < 200 && !vready; i++) {
            if (hub_get_vparam(chn, &vp) && vparam_ready(&vp)) { vready = 1; break; }
            usleep(10000);
        }
        if (wsub)  hub_unsubscribe(chn, &wq);
        if (winit) fanqueue_free(&wq);
    }
    if (!vready) {
        LOGW(MOD, "no SPS/PPS for video%d - cannot answer", chn);
        return 503;
    }

    char ptstr[8] = "";
    int pt = pick_h264_pt(vm, &vp, ptstr, sizeof ptstr);
    if (pt < 0) {
        LOGW(MOD, "offer carries no H264 with packetization-mode=1");
        return 400;
    }
    vm->accept = 1;

    /* Audio: only if the daemon really encodes G.711 AND the offer carries it
     * AND the offerer put both mids in one BUNDLE group (this endpoint has a
     * single ICE transport, so two separate transports cannot be answered).
     * Anything else leaves the audio m-section answered with port 0 rather
     * than negotiating a codec that would never carry a packet. */
    sdp_msec *am = NULL;
    int apt = -1, acodec = MS_AC_NONE, arate = 0, ach = 0;
    int asrc = HUB_AUDIO_SRC;
    for (int i = 0; i < off.nm; i++)
        if (!strcmp(off.m[i].kind, "audio")) { am = &off.m[i]; break; }
    if (am) {
        /* Primary source first (unchanged behaviour when audio.codec is
         * already G.711); otherwise the optional audio.codec2 side-encode,
         * which exists precisely because AAC cannot ride this transport. */
        if (!hub_get_audio(&acodec, &arate, &ach)) acodec = MS_AC_NONE;
        if (acodec != MS_AC_PCMU && acodec != MS_AC_PCMA) {
            if (hub_get_audio2(&acodec, &arate, &ach)) asrc = HUB_AUDIO_SRC2;
            else acodec = MS_AC_NONE;
        }
        if ((acodec == MS_AC_PCMU || acodec == MS_AC_PCMA) &&
            strstr(am->proto, "SAVP") &&
            bundle_has(&off, vm->mid) && bundle_has(&off, am->mid))
            apt = pick_g711_pt(am, acodec);
        if (apt >= 0) am->accept = 1;
        else LOGI(MOD, "audio m-section answered with port 0 (hub codec %d, "
                       "no matching G.711 payload type in one BUNDLE)", acodec);
    }

    pthread_mutex_lock(&g_mx);
    wrtc_session *s = NULL;
    for (int i = 0; i < WEBRTC_MAX_SESSIONS; i++)
        if (!g_sess[i].used) { s = &g_sess[i]; break; }
    if (!s) { pthread_mutex_unlock(&g_mx); return 503; }
    memset(s, 0, sizeof *s);
    s->gen  = ++g_gen_ctr;
    s->used = 1;
    s->fd = -1;
    pthread_mutex_unlock(&g_mx);

    s->chn = chn;
    s->pt  = pt;
    memcpy(s->peer_fp, peer_fp, sizeof s->peer_fp);
    if (am && am->accept) { s->have_audio = 1; s->apt = apt; s->asrc = asrc; }

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

    /* The SSRC has to be known here, not in rtp_track_init(): the answer
     * announces it in a=ssrc so the browser can bind the incoming stream to
     * this transceiver without waiting to infer it. */
    auth_gen_token(tok);
    tok[8] = 0;
    s->ssrc = (uint32_t)strtoul(tok, NULL, 16);
    if (!s->ssrc) s->ssrc = 1;
    if (s->have_audio) {
        /* Distinct from the video SSRC: it is what separates the two streams
         * inside the one bundled transport, and it keys their SRTP rollover
         * counters apart (srtp.h). */
        auth_gen_token(tok);
        tok[8] = 0;
        s->assrc = (uint32_t)strtoul(tok, NULL, 16);
        if (!s->assrc || s->assrc == s->ssrc) s->assrc = s->ssrc ^ 0x5A5A5A5Au;
        if (!s->assrc) s->assrc = 2;
    }

    s->fd = udp_bind_session();
    if (s->fd < 0) {
        LOGE(MOD, "cannot bind udp port %d-%d: %s", g_port_cfg,
             g_port_cfg ? g_port_max : 0, strerror(errno));
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

    char fmtp[1024];
    int fn = vparam_sdp_fmtp(&vp, pt, fmtp, sizeof fmtp);
    /* snprintf semantics: a too-small buffer returns what it WOULD have
     * written, so a bare >0 test used to accept a silently cut sprop line. */
    if (fn <= 0 || fn >= (int)sizeof fmtp) { sess_release(s); return 500; }

    /* o= sess-id must be a NUMERIC string (RFC 4566 5.2) - the session's hex
     * id is fine for the Location header but not here. */
    static unsigned long long sdp_sid;
    if (!sdp_sid) sdp_sid = (unsigned long long)time(NULL) + 2208988800ULL;

    sdpbuf b = { ans, anscap, 0, 0 };
    sb_add(&b,
        "v=0\r\n"
        "o=- %llu 1 IN IP4 %s\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "a=ice-lite\r\n",
        sdp_sid, local_ip);
    /* Only the mids actually answered may stay in the group (RFC 8843 7.3.2):
     * a rejected m-line listed in BUNDLE is a malformed answer. */
    sb_add(&b, "a=group:BUNDLE");
    for (int i = 0; i < off.nm; i++)
        if (off.m[i].accept) sb_add(&b, " %s", off.m[i].mid);
    sb_add(&b, "\r\na=msid-semantic: WMS timps\r\n");

    for (int i = 0; i < off.nm; i++) {
        sdp_msec *m = &off.m[i];
        if (!m->accept) {
            /* RFC 3264 6: a rejected stream keeps its place and its format
             * list, and gets port 0. Echo the offer's first format so the
             * m-line stays syntactically valid. */
            char f1[32] = "0";        /* "webrtc-datachannel" is 18 */
            sdp_tok(m->fmts, f1, sizeof f1);
            sb_add(&b, "m=%s 0 %s %s\r\nc=IN IP4 %s\r\na=mid:%s\r\n"
                       "a=inactive\r\n",
                   m->kind, m->proto, f1[0] ? f1 : "0", local_ip, m->mid);
            continue;
        }
        int is_video = (m == vm);
        sb_add(&b,
            "m=%s %d %s %d\r\n"
            "c=IN IP4 %s\r\n"
            "a=mid:%s\r\n"
            "a=rtcp-mux\r\n"
            "a=ice-ufrag:%s\r\n"
            "a=ice-pwd:%s\r\n"
            "a=fingerprint:sha-256 %s\r\n"
            "a=setup:passive\r\n"
            "a=sendonly\r\n",
            m->kind, s->port, m->proto, is_video ? pt : apt, local_ip, m->mid,
            s->lufrag, s->lpwd, ms_dtls_fingerprint(g_dtls_ctx));
        if (is_video) {
            sb_add(&b,
                "a=rtpmap:%d H264/90000\r\n"
                /* PLI and FIR only: both just ask for a keyframe, which the
                 * encoder can give. Plain "nack" would promise retransmission
                 * this does not implement, so it is deliberately not offered. */
                "a=rtcp-fb:%d nack pli\r\n"
                "a=rtcp-fb:%d ccm fir\r\n"
                "%s"
                "a=msid:timps timps-video\r\n"
                "a=ssrc:%u cname:timps\r\n"
                "a=ssrc:%u msid:timps timps-video\r\n",
                pt, pt, pt, fmtp, s->ssrc, s->ssrc);
        } else {
            sb_add(&b,
                /* 40 ms is what the AI capture actually delivers per frame
                 * (numPerFrm = rate*40/1000 in hal_ingenic.c), so that is what
                 * one G.711 RTP packet carries. */
                "a=rtpmap:%d %s/8000\r\n"
                "a=ptime:40\r\n"
                "a=msid:timps timps-audio\r\n"
                "a=ssrc:%u cname:timps\r\n"
                "a=ssrc:%u msid:timps timps-audio\r\n",
                apt, acodec == MS_AC_PCMA ? "PCMA" : "PCMU",
                s->assrc, s->assrc);
        }
        sb_add(&b,
            "a=candidate:1 1 udp 2130706431 %s %d typ host\r\n"
            "a=end-of-candidates\r\n", local_ip, s->port);
    }
    if (b.ovf) { sess_release(s); return 500; }
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
    LOGI(MOD, "%s: answered WHEP offer, ICE-lite candidate %s:%d, H264 pt=%d%s%s",
         s->id, local_ip, s->port, pt,
         s->have_audio ? (acodec == MS_AC_PCMA ? ", PCMA" : ", PCMU") : "",
         s->have_audio && s->asrc == HUB_AUDIO_SRC2 ? " (codec2)" : "");
    return 201;
}

int webrtc_delete(const char *id)
{
    if (!g_dtls_ctx || !id || !*id) return 404;
    /* The id is 128 bits from auth_gen_token(); nothing else about the URL is
     * trusted, and the route itself already ran the /control auth rules. */
    wrtc_session *s = NULL;
    unsigned gen = 0;
    pthread_mutex_lock(&g_mx);
    for (int i = 0; i < WEBRTC_MAX_SESSIONS; i++)
        if (g_sess[i].used && !strcmp(g_sess[i].id, id)) {
            s = &g_sess[i];
            gen = s->gen;
            s->run = 0;          /* the session thread releases everything */
            break;
        }
    pthread_mutex_unlock(&g_mx);
    if (!s) return 404;

    /* Answer only once the slot is actually free (or has been handed to a new
     * session, which means ours finished), so a 200 is not a promise. */
    for (int w = 0; w < WEBRTC_DELETE_WAIT_MS / 10; w++) {
        pthread_mutex_lock(&g_mx);
        int gone = !s->used || s->gen != gen;
        pthread_mutex_unlock(&g_mx);
        if (gone) { LOGI(MOD, "%s: closed by DELETE", id); return 200; }
        usleep(10000);
    }
    /* Marked, not yet reaped: the thread is in a bounded wait and will free
     * the slot on its own. Still a success from the client's point of view. */
    LOGW(MOD, "%s: DELETE marked the session but it has not finished yet", id);
    return 200;
}

void webrtc_start(const ms_config *cfg)
{
    if (!cfg->webrtc_enabled) return;
    g_port_cfg = cfg->webrtc_port;
    g_port_max = cfg->webrtc_port_max;
    if (g_port_cfg) {
        if (g_port_max < g_port_cfg)
            g_port_max = g_port_cfg + WEBRTC_MAX_SESSIONS - 1;
        if (g_port_max > 65535) g_port_max = 65535;
    }
    g_chn_cfg  = cfg->webrtc_channel;
    g_dtls_ctx = ms_dtls_ctx_new(cfg->http_tls_cert, cfg->http_tls_key);
    if (!g_dtls_ctx) {
        LOGE(MOD, "webrtc.enabled=1 but no usable DTLS certificate (%s / %s) "
                  "- /webrtc/whep stays disabled",
             cfg->http_tls_cert, cfg->http_tls_key);
        return;
    }
    LOGI(MOD, "WHEP endpoint /webrtc/whep ready (ICE-lite + DTLS-SRTP, H264 "
              "video%d); fingerprint %s", g_chn_cfg,
         ms_dtls_fingerprint(g_dtls_ctx));
    if (g_port_cfg)
        LOGI(MOD, "media udp %d-%d, one port per session (%d slots)",
             g_port_cfg, g_port_max, WEBRTC_MAX_SESSIONS);
}

void webrtc_stop(void)
{
    if (!g_dtls_ctx) return;
    for (int i = 0; i < WEBRTC_MAX_SESSIONS; i++) g_sess[i].run = 0;
    int live = 0;
    /* 1 s: a session thread's longest blocking wait is the 250 ms pre-DTLS
     * poll, and main()'s shutdown guillotine still has to cover the vendor
     * teardown after this. */
    for (int w = 0; w < 100; w++) {
        live = 0;
        for (int i = 0; i < WEBRTC_MAX_SESSIONS; i++) live += g_sess[i].used;
        if (!live) break;
        usleep(10000);
    }
    if (live) {
        /* A session thread still holds this config (and its ssl contexts point
         * into it). Leaking one struct at shutdown is the lesser evil. */
        LOGW(MOD, "%d session(s) still running at stop - leaving the DTLS "
                  "context allocated", live);
        g_dtls_ctx = NULL;
        return;
    }
    ms_dtls_ctx_free(g_dtls_ctx);
    g_dtls_ctx = NULL;
}

#endif /* USE_WEBRTC */
