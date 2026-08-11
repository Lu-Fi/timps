/* trace.h - opt-in send-pipeline instrumentation (DEVELOPER TOOL).
 *
 * WHY THIS EXISTS
 * ---------------
 * A client-side capture can tell us that a stream fell behind real time (an
 * ffmpeg recording whose media duration lags the wall-clock span, or a multi-
 * second inter-frame gap), but it cannot tell us WHERE the time went. The three
 * candidates all look identical from outside the camera:
 *
 *   (a) the encoder/HAL never produced the frame in time  (producer stall)
 *   (b) the frame existed but this client's thread was busy elsewhere
 *       (still blocked writing the PREVIOUS frame, i.e. socket backpressure)
 *   (c) the write() / send() for this frame itself blocked (link stall,
 *       receiver window closed, WiFi retransmit storm)
 *
 * This module gives every media-sending loop four numbers per access unit that
 * separate exactly those cases:
 *
 *   gap  = t_pop(this AU)  - t_done(previous AU)
 *          how long this sender thread had NOTHING to send. Large gap with a
 *          small age  => (a): the producer starved us.
 *   age  = t_pop           - t_enq (hub publish time, ms_pkt.enq_us)
 *          how long the AU sat between "encoder produced it" and "we popped
 *          it". Large age with a small gap => (b): we were behind, the queue
 *          had backlog waiting while we were stuck on an earlier frame.
 *   send = t_done          - t_pop
 *          packetize + mux + write, i.e. everything this thread did for the AU.
 *   wr   = summed time actually spent INSIDE the socket write calls for this
 *          AU (MS_TR_WR). wr ~= send  => (c): the kernel/link blocked us.
 *          send - wr ("cpu") is packetization/mux/memcpy/mutex time; if THAT
 *          is what is large, the stall is inside timps (lock contention, an
 *          allocation, a HAL call), not on the wire.
 *
 * COST / WHY IT IS SAFE TO LEAVE COMPILED IN
 * ------------------------------------------
 * Off (the default, g_trace_mask == 0) every hook is `if (mask & bit)` against
 * a hot global int - no syscalls, no allocation, no locks, so the timing being
 * observed is not perturbed. There is deliberately NO per-frame heap use at
 * all, on or off: the whole per-connection state is one ms_trace_ctx embedded
 * in the caller's existing session/connection struct (~120 bytes), which
 * matters on a 64 MB T20/T23.
 *
 * With MS_TR_AU on, the added cost is 2 clock_gettime() per access unit (~50/s
 * at 25 fps video + audio). With MS_TR_WR on, 2 more per socket write - which
 * for a fragmented 200 KB IDR is ~170 writes, i.e. ~340 syscalls, roughly 1 ms
 * on a no-vDSO MIPS target. That is three orders of magnitude below the
 * multi-second stalls this is built to catch, but it is why MS_TR_WR is a
 * separate bit you can leave off.
 *
 * Output is threshold-gated: a line is emitted only for an access unit whose
 * gap, age or send exceeded general.trace_ms (default 250 ms), so a healthy
 * stream produces nothing. MS_TR_SUM adds one periodic per-connection summary
 * so "nothing happened" is also positive evidence rather than silence.
 *
 * TURNING IT ON: it is NOT reachable from the WebUI or the /control API, and
 * NOT a menuconfig/Kconfig option. It IS gated behind a plain Makefile build
 * flag, USE_TRACE (default 0), because this is a developer tool that must not
 * ship in production camera images - see the USE_TRACE comment in Makefile.
 * With USE_TRACE=0 (the default for every real build), trace.c is not even
 * compiled: every function below becomes a static-false inline stub the
 * compiler dead-code-eliminates at -Os, ms_trace_ctx shrinks to an empty
 * placeholder, and general.trace/general.trace_ms in timps.conf are accepted
 * but are a no-op (same pattern as USE_OSD_HINTING's osd.hinting key).
 *
 * With a USE_TRACE=1 build, add to /etc/timps.conf and restart timpsd:
 *
 *     general.trace    = 15     # bitmask, see below; 0 = off (default)
 *     general.trace_ms = 250    # report threshold in ms
 *
 * general.trace/general.trace_ms are handled as side-effecting keys in
 * config.c's set_kv() (exactly like general.syslog): they are in no cfg_field
 * table, so they carry no F_CTRL flag, cannot be POSTed to /control, are not
 * echoed by GET /control, and are not in the /control?fields=1 inventory.
 */
#ifndef MS_TRACE_H
#define MS_TRACE_H

#include <stdint.h>
#include <stddef.h>

enum {
    MS_TR_AU  = 1<<0,   /* per-AU gap/age/send timing (2 clock reads per AU) */
    MS_TR_WR  = 1<<1,   /* time each socket write (adds 2 clock reads/write) */
    MS_TR_Q   = 1<<2,   /* sample this client's fanqueue backlog at pop time */
    MS_TR_SUM = 1<<3,   /* periodic per-connection summary line */
    MS_TR_ALL = 0xF,
};

#ifndef USE_TRACE

/* USE_TRACE=0 (default): true no-op build, no trace.c, nothing to link.
 * Every call site in hub.c/rtsp.c/httpd.c/config.c compiles unchanged; the
 * `if (ms_trace_on(...))` guards fold to `if (0)` and the compiler removes
 * the dead bodies at -Os, so this costs nothing - not even a global-int
 * compare - in a production image. */
typedef struct { char _unused; } ms_trace_ctx;

static inline int ms_trace_on(int bits){ (void)bits; return 0; }
static inline void ms_trace_set(int mask){ (void)mask; }
static inline void ms_trace_set_threshold_ms(int ms){ (void)ms; }
static inline void ms_trace_open(ms_trace_ctx *t, const char *tag, const char *who, int chn)
{ (void)t; (void)tag; (void)who; (void)chn; }
static inline void ms_trace_au_begin(ms_trace_ctx *t){ (void)t; }
static inline int64_t ms_trace_wr_begin(void){ return 0; }
static inline void ms_trace_wr_end(ms_trace_ctx *t, int64_t t0, int bytes)
{ (void)t; (void)t0; (void)bytes; }
static inline void ms_trace_au_end(ms_trace_ctx *t, int media, int keyframe, size_t len,
                     int64_t enq_us, int64_t t_pop, int64_t t_done, int qcount, int qcap)
{ (void)t; (void)media; (void)keyframe; (void)len; (void)enq_us; (void)t_pop; (void)t_done;
  (void)qcount; (void)qcap; }
static inline void ms_trace_window(ms_trace_ctx *t, int64_t now){ (void)t; (void)now; }

#else /* USE_TRACE=1: the real thing, implemented in trace.c */

#include "util.h"      /* ms_now_us */

/* written once at config-load time, before any streaming thread exists; read
 * lock-free from every sender thread afterwards */
extern volatile int g_trace_mask;
extern volatile int g_trace_thresh_us;

static inline int ms_trace_on(int bits){ return (g_trace_mask & bits) != 0; }

void ms_trace_set(int mask);
void ms_trace_set_threshold_ms(int ms);

/* One per streaming connection, embedded in the caller's session struct.
 * Zero-initialise (or just call ms_trace_open) before use. */
typedef struct {
    const char *tag;          /* "rtsp" / "mp4" - a static string, not copied */
    char        who[24];      /* session id / peer, for the log line */
    int         chn;
    int64_t     t_prev;       /* t_done of the previous serviced AU */

    /* connection-lifetime write accounting, fed by ms_trace_wr_end() from
     * inside the sink. Cumulative so that writes made OUTSIDE an AU window
     * (RTCP compounds, HTTP headers, the fMP4 init segment) are still
     * accounted for - they show up in the summary and in mx_wr even though
     * they belong to no access unit. */
    int64_t     wr_us;        /* total time inside socket writes */
    int64_t     wr_n;         /* number of socket writes */
    uint64_t    tx_bytes;     /* bytes handed to those writes */

    /* snapshot of the two counters above taken at ms_trace_au_begin(), so the
     * per-AU figures are a difference rather than a reset (nothing is lost) */
    int64_t     au_wr_us0, au_wr_n0;

    /* rolling window (MS_TR_SUM) */
    int64_t     win_us;       /* window start */
    uint32_t    n_au, n_slow;
    int64_t     mx_gap, mx_age, mx_send, mx_wr;
    int         mx_q, q_cap;
} ms_trace_ctx;

/* who may be NULL. Safe (and cheap) to call even when tracing is off. */
void ms_trace_open(ms_trace_ctx *t, const char *tag, const char *who, int chn);

/* Call right after the AU was popped off this client's fanqueue. */
static inline void ms_trace_au_begin(ms_trace_ctx *t)
{
    t->au_wr_us0 = t->wr_us;
    t->au_wr_n0  = t->wr_n;
}

/* Bracket one socket write. wr_begin() returns 0 when MS_TR_WR is off, and
 * wr_end() is then a single compare - no clock read on either side. */
static inline int64_t ms_trace_wr_begin(void)
{
    return ms_trace_on(MS_TR_WR) ? ms_now_us() : 0;
}
static inline void ms_trace_wr_end(ms_trace_ctx *t, int64_t t0, int bytes)
{
    /* t0 first: it is 0 whenever MS_TR_WR is off, so the whole hook collapses
     * to one compare on the hot path even for a connection that HAS a ctx. */
    if (!t0 || !t) return;
    if (bytes > 0) t->tx_bytes += (uint64_t)bytes;
    int64_t d = ms_now_us() - t0;
    t->wr_us += d;
    t->wr_n++;
    if (d > t->mx_wr) t->mx_wr = d;
}

/* Call once the AU has been fully handed to the socket. enq_us is the
 * ms_pkt.enq_us hub-publish stamp (0 = unknown -> age is reported as 0).
 * qcount/qcap: this client's fanqueue backlog sampled at pop time, or -1/-1
 * when MS_TR_Q is off. Emits a line if any stage exceeded the threshold. */
void ms_trace_au_end(ms_trace_ctx *t, int media, int keyframe, size_t len,
                     int64_t enq_us, int64_t t_pop, int64_t t_done,
                     int qcount, int qcap);

/* Periodic summary (MS_TR_SUM). Pass a `now` the caller already has; emits at
 * most one line per MS_TRACE_WIN_US and resets the window maxima. */
void ms_trace_window(ms_trace_ctx *t, int64_t now);

#endif /* USE_TRACE */

#endif /* MS_TRACE_H */
