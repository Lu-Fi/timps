/* talk_ws.c - see talk_ws.h. Compiled only when USE_BC_WS. */
#ifdef USE_BC_WS
#include "talk_ws.h"
#include "backchannel.h"
#include "../ws.h"
#include "../codec/g711.h"
#include "../log.h"
#include "../util.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#define MOD "talk"

/* ---- tunables ----------------------------------------------------------- */

/* How long ws_read_message() waits for a frame before handing control back so
 * the loop can do its periodic work. Also the liveness tick. */
#define TALK_POLL_MS 1000

/* Close a session that has sent NOTHING (not even a PONG) for this long.
 * ws.c stamps ws_conn.last_rx_ms on every complete frame of any kind, so a
 * browser that is merely silent-but-alive keeps answering our PINGs and never
 * trips this; a half-open TCP after a WiFi drop does. */
#define TALK_IDLE_MS 10000

/* Send a PING this often, so an idle-but-live client has something to answer
 * and TALK_IDLE_MS measures liveness rather than talkativeness. */
#define TALK_PING_MS 3000

/* --- the backlog guard ---
 *
 * This is the one thing a TCP transport does not get for free. RTP over UDP
 * self-regulates: a network stall DROPS packets, so the RTSP backchannel
 * recovers on its own. On a WebSocket nothing is ever dropped - a 2 s WiFi
 * stall buffers 2 s of audio, delivers it in a burst, and hal_ao_write()
 * (which blocks, by design, using the AO ring as the playback clock) then
 * plays every sample of it. Latency would step up by 2 s and never recover.
 *
 * So: track how far ahead of the wall clock the audio we have ACCEPTED
 * reaches. If accepting this frame would put us more than TALK_MAX_BACKLOG_US
 * ahead, drop it instead. Dropping without advancing the accumulator lets the
 * schedule slide back to real time on its own.
 *
 * TALK_RESYNC_US restarts the schedule when the client simply stopped talking
 * for a while (push-to-talk released, tab backgrounded), which must not be
 * mistaken for us being behind. */
#define TALK_MAX_BACKLOG_US (400 * 1000LL)
#define TALK_RESYNC_US (2000 * 1000LL)

/* Give up on the session after this many consecutive frames rejected by the
 * election (another talker - an ONVIF client or NVR - holds the speaker).
 * ~1 s at 20 ms frames. Closing beats decoding into a void for as long as the
 * visitor keeps talking. */
#define TALK_MAX_LOST 50

/* Sample rates a client may declare. Anything else is refused rather than
 * guessed at: speaker.c resamples whatever it is handed, so the only job here
 * is to reject a garbage or hostile value before it reaches ms_resample().
 * The list is deliberately generous at the top end because a browser
 * AudioContext may decline the rate the page requested and impose the
 * hardware one instead (notably iOS Safari, which commonly forces 48000). */
static const int TALK_RATES[] = {8000, 16000, 24000, 32000, 44100, 48000};

/* ---- request-head helpers ------------------------------------------------
 *
 * talk_ws.c does its own header scanning rather than calling httpd.c's
 * http_header(): that one is static, and de-static-ing it (plus exporting
 * hconn) is more churn than the ~20 lines below. ws.c's own ws_header() is
 * not usable either - it reads out of a ws_handshake that only
 * ws_handshake_read() fills, and that function cannot run here because
 * conn_thread has already consumed the head off the socket. */

/* Copy the value of header `name` (WITHOUT the colon) out of an HTTP request
 * head. Case-insensitive and anchored at a line start, so a header VALUE can
 * never impersonate a header name. `out` is always NUL-terminated. Returns 1
 * when found. */
static int hdr_get(const char *head, const char *name, char *out, int cap)
{
    size_t nlen = strlen(name);
    const char *p = strchr(head, '\n');   /* skip the request line */
    out[0] = 0;
    while (p) {
        p++;
        if (*p == '\r' || *p == '\n' || *p == 0) break;      /* end of head */
        if (!strncasecmp(p, name, nlen) && p[nlen] == ':') {
            const char *v = p + nlen + 1;
            while (*v == ' ' || *v == '\t') v++;
            int i = 0;
            while (v[i] && v[i] != '\r' && v[i] != '\n' && i < cap-1) {
                out[i] = v[i]; i++;
            }
            out[i] = 0;
            return 1;
        }
        p = strchr(p, '\n');
    }
    return 0;
}

/* Reduce an Origin or Host value to its bare host: strip any scheme, any
 * port, any path. "https://cam.lan:8080/x" and "cam.lan:8080" both -> "cam.lan". */
static void host_of(const char *in, char *out, int cap)
{
    const char *p = strstr(in, "://");
    p = p ? p + 3 : in;
    int i = 0;
    while (p[i] && p[i] != ':' && p[i] != '/' && i < cap-1) { out[i] = p[i]; i++; }
    out[i] = 0;
}

/* Origin policy.
 *
 * WebSocket is NOT covered by CORS - the http_cors() headers httpd.c attaches
 * to /talk do nothing to protect the upgrade itself, so this has to be
 * policed here. The credential is still the ?token=, which a cross-origin
 * page cannot read; this is defence in depth for the case where a token has
 * leaked into a URL, a referrer or a proxy log.
 *
 *   - No Origin header at all -> allowed. RFC 6455 4.1 requires browsers to
 *     send one, so its absence means a non-browser client, which has no
 *     ambient credentials to hijack and still had to present a valid token.
 *   - Origin present -> its host must equal the Host header's host, i.e. the
 *     page came from this camera. An explicitly empty or "null" Origin (a
 *     sandboxed iframe, a data: document) is refused: it is opaque, so it can
 *     never be matched against anything, and treating it as absent would let
 *     any sandboxed frame through.
 *
 * Deliberately no configurable allow-list yet (motors has motors.ws_origins);
 * the WebUI page is served from this same camera. */
static int origin_ok(const char *head)
{
    char origin[192], host[128], oh[128], hh[128];

    if (!hdr_get(head, "Origin", origin, sizeof origin))
        return 1;                                   /* absent: non-browser */
    if (!origin[0] || !strcasecmp(origin, "null"))
        return 0;                                   /* opaque: unmatchable */
    if (!hdr_get(head, "Host", host, sizeof host))
        return 0;                                   /* HTTP/1.1 requires Host */

    host_of(origin, oh, sizeof oh);
    host_of(host,   hh, sizeof hh);
    /* Ports are deliberately NOT compared: the WebUI page is served by uhttpd
     * on :443 while timps' own listener is on a different port by
     * construction, so requiring port equality would reject every legitimate
     * same-camera request. Host equality is the property that matters. */
    return oh[0] && hh[0] && !strcasecmp(oh, hh);
}

/* Read one query parameter out of a request target. Anchored on '?' or '&' so
 * "rate=" cannot be matched inside some other parameter's name or value.
 * Returns the value pointer (into path) or NULL. */
static const char *qparam(const char *path, const char *name)
{
    size_t nlen = strlen(name);
    const char *q = strchr(path, '?');
    while (q) {
        q++;
        if (!strncmp(q, name, nlen) && q[nlen] == '=')
            return q + nlen + 1;
        q = strchr(q, '&');
    }
    return NULL;
}

/* Declared capture rate, or 0 if the client asked for one we do not accept.
 * Absent means 8000: mu-law's native rate and what the reference page sends. */
static int talk_rate(const char *path)
{
    const char *v = qparam(path, "rate");
    if (!v) return 8000;
    int r = atoi(v);
    for (unsigned i = 0; i < sizeof TALK_RATES / sizeof TALK_RATES[0]; i++)
        if (r == TALK_RATES[i]) return r;
    return 0;
}

/* ---- the session -------------------------------------------------------- */

void talk_ws_serve(int fd, void *tls, const char *head, int head_len,
                   const char *path)
{
    ws_io   io;
    ws_conn wsc;
    char    key[64], val[128];
    int     rate;

    if (!head || head_len <= 0) return;

    io.fd = fd;
    io.tls = tls;

    /* --- validate the upgrade request, out of the head conn_thread read --- *
     *
     * RFC 6455 section 4.1: the handshake MUST be a GET. httpd.c's dispatch
     * matches on the path alone, so check it here - a POST carrying the right
     * Sec-* headers is not something a browser form can produce, but nothing
     * upstream of this point rejects one either. */
    if (strncmp(head, "GET ", 4) != 0) {
        ws_handshake_reject(&io, 405, "Method Not Allowed", "GET required");
        return;
    }
    rate = talk_rate(path);
    if (!rate) {
        LOGW(MOD, "refused: unsupported rate= in %s", path);
        ws_handshake_reject(&io, 400, "Bad Request", "bad rate");
        return;
    }
    if (!hdr_get(head, "Sec-WebSocket-Key", key, sizeof key) || !key[0]) {
        ws_handshake_reject(&io, 400, "Bad Request", "not a websocket upgrade");
        return;
    }
    if (!hdr_get(head, "Upgrade", val, sizeof val) ||
        strcasecmp(val, "websocket") != 0) {
        ws_handshake_reject(&io, 400, "Bad Request", "not a websocket upgrade");
        return;
    }
    /* "Connection: keep-alive, Upgrade" is legal and common, so this is a
     * token search rather than an equality test (RFC 6455 section 4.1). */
    if (!hdr_get(head, "Connection", val, sizeof val) ||
        strcasestr(val, "upgrade") == NULL) {
        ws_handshake_reject(&io, 400, "Bad Request", "not a websocket upgrade");
        return;
    }
    if (!hdr_get(head, "Sec-WebSocket-Version", val, sizeof val) ||
        atoi(val) != 13) {
        ws_handshake_reject(&io, 426, "Upgrade Required", "websocket version 13 required");
        return;
    }
    if (!origin_ok(head)) {
        LOGW(MOD, "refused: cross-origin upgrade");
        ws_handshake_reject(&io, 403, "Forbidden", "bad origin");
        return;
    }

    /* --- 101 --- *
     *
     * ws_handshake_accept() reads only hs->key, but ws_handshake is a ~3.8 KB
     * struct (it carries the raw head for ws_header()'s benefit, which this
     * daemon does not use). Scoped to this block so the compiler is free to
     * overlap its stack slot with the message loop's buffers below rather
     * than carrying it for the whole session. */
    {
        ws_handshake hs;
        memset(&hs, 0, sizeof hs);
        snprintf(hs.key, sizeof hs.key, "%s", key);
        if (ws_handshake_accept(&io, &hs, NULL) != WS_OK) {
            LOGD(MOD, "handshake write failed");
            return;
        }
    }

    ws_conn_init(&wsc, &io);
    LOGI(MOD, "talk session open (%d Hz mu-law)", rate);

    /* Tell the page what we settled on. Cheap, and it turns "no audio" into a
     * diagnosable state on the browser side. */
    {
        char hello[64];
        snprintf(hello, sizeof hello, "{\"ok\":1,\"codec\":\"pcmu\",\"rate\":%d}", rate);
        ws_send_text(&io, hello);
    }

    /* --- the message loop --- */
    {
        unsigned char payload[WS_MAX_PAYLOAD];
        int16_t       pcm[WS_MAX_PAYLOAD];
        int64_t       t0_us = 0;      /* wall clock the schedule is anchored at */
        int64_t       acc_us = 0;     /* audio duration accepted since t0_us */
        int64_t       last_ping = 0;
        long long     nlost = 0;      /* consecutive election refusals */
        unsigned long nframes = 0, ndropped = 0;

        for (;;) {
            size_t plen = 0;
            int    op = 0;
            int    rc = ws_read_message(&wsc, &op, payload, sizeof payload,
                                        &plen, TALK_POLL_MS);

            if (rc == WS_AGAIN) {
                if (ws_conn_idle_ms(&wsc) > TALK_IDLE_MS) {
                    LOGI(MOD, "talk session idle >%d ms - closing", TALK_IDLE_MS);
                    ws_send_close(&io, WS_CLOSE_GOING_AWAY, "idle");
                    break;
                }
                int64_t now_ms = ws_now_ms();
                if (now_ms - last_ping > TALK_PING_MS) {
                    last_ping = now_ms;
                    if (ws_send_ping(&io) != WS_OK) break;
                }
                continue;
            }
            if (rc != WS_OK) break;      /* CLOSED / EPROTO / ETOOBIG / EIO */

            /* Audio only. A text frame here is a client bug, not an attack,
             * but there is no text command in this protocol to mistake it
             * for, so say so plainly rather than ignoring it. */
            if (op != WS_OP_BIN) {
                ws_send_close(&io, WS_CLOSE_UNSUPPORTED, "binary frames only");
                break;
            }
            if (plen == 0) continue;

            /* --- backlog guard (see TALK_MAX_BACKLOG_US) --- */
            {
                int64_t now = ms_now_us();
                int64_t frame_us = (int64_t)plen * 1000000LL / rate;
                if (t0_us == 0) { t0_us = now; acc_us = 0; }
                /* client went quiet: re-anchor instead of reading the gap as
                 * "we are behind" */
                if (now - (t0_us + acc_us) > TALK_RESYNC_US) {
                    t0_us = now; acc_us = 0;
                }
                if ((t0_us + acc_us + frame_us) - now > TALK_MAX_BACKLOG_US) {
                    ndropped++;          /* drop; do NOT advance acc_us */
                    continue;
                }
                acc_us += frame_us;
            }

            /* one mu-law byte per sample; plen <= WS_MAX_PAYLOAD == |pcm| */
            g711_ulaw_decode(payload, plen, pcm);

            if (bc_feed_pcm(&wsc, pcm, (int)plen, rate)) {
                nframes++;
                nlost = 0;
            } else if (++nlost >= TALK_MAX_LOST) {
                LOGW(MOD, "another talker holds the speaker - closing");
                ws_send_close(&io, WS_CLOSE_POLICY, "speaker busy");
                break;
            }
        }

        LOGI(MOD, "talk session closed (%lu frames, %lu dropped)",
             nframes, ndropped);
    }

    /* Every exit path lands here. bc_release() clears the election (and any
     * AAC decoder state) if we held it, and calls speaker_release(), which
     * does ao_drop(0) - discarding whatever is still queued rather than
     * waiting out a tail. That discard is what bounds accumulated latency to
     * a single push-to-talk press. */
    bc_release(&wsc);
}

#endif /* USE_BC_WS */
