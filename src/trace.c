/* trace.c - opt-in send-pipeline instrumentation. See trace.h for the model,
 * the cost analysis and how to turn it on. Only compiled at all when the
 * Makefile is invoked with USE_TRACE=1 (default 0, so production camera
 * builds never link this in - see the USE_TRACE comment in Makefile); when
 * compiled, still off by default at runtime, g_trace_mask == 0 makes every
 * hook in the media paths a single global-int test. */

#include "trace.h"
#include "log.h"
#include "frame.h"      /* MS_MEDIA_* */
#include <stdio.h>
#include <string.h>

#define MOD "TRACE"

/* summary window (MS_TR_SUM) */
#ifndef MS_TRACE_WIN_US
#define MS_TRACE_WIN_US (10LL*1000000)
#endif

volatile int g_trace_mask      = 0;
volatile int g_trace_thresh_us = 250000;   /* general.trace_ms default */

void ms_trace_set(int mask)
{
    g_trace_mask = mask & MS_TR_ALL;
    if (g_trace_mask)
        LOGW(MOD,"send-pipeline tracing ENABLED (mask=0x%x: %s%s%s%s) "
                 "threshold=%dms - this is a debug build option, "
                 "leave it off in normal operation",
             g_trace_mask,
             (g_trace_mask & MS_TR_AU) ? "au ":"",
             (g_trace_mask & MS_TR_WR) ? "write ":"",
             (g_trace_mask & MS_TR_Q)  ? "qdepth ":"",
             (g_trace_mask & MS_TR_SUM)? "summary":"",
             g_trace_thresh_us/1000);
}

void ms_trace_set_threshold_ms(int ms)
{
    if (ms < 1)     ms = 1;         /* 0 would log literally every AU */
    if (ms > 60000) ms = 60000;
    g_trace_thresh_us = ms * 1000;
}

void ms_trace_open(ms_trace_ctx *t, const char *tag, const char *who, int chn)
{
    memset(t, 0, sizeof *t);
    t->tag = tag ? tag : "?";
    snprintf(t->who, sizeof t->who, "%s", (who && who[0]) ? who : "-");
    t->chn = chn;
    t->mx_q = -1;
    t->q_cap = -1;
    if (!g_trace_mask) return;
    t->t_prev = ms_now_us();
    t->win_us = t->t_prev;
    LOGW(MOD,"%s/%s chn=%d: trace attached", t->tag, t->who, t->chn);
}

/* microseconds -> "<ms>.<tenths>" for a compact, still sub-ms-readable log.
 * TRUNCATES rather than rounds on purpose: rounding the fractional part would
 * carry (1999us -> "1.10ms"), and every caller feeds a non-negative value. */
#define TMS(x) (long long)((x)/1000), (int)(((x)%1000)/100)

void ms_trace_au_end(ms_trace_ctx *t, int media, int keyframe, size_t len,
                     int64_t enq_us, int64_t t_pop, int64_t t_done,
                     int qcount, int qcap)
{
    if (!ms_trace_on(MS_TR_AU) || !t_pop) return;

    int64_t gap  = t->t_prev ? t_pop - t->t_prev : 0;
    int64_t age  = (enq_us > 0 && t_pop > enq_us) ? t_pop - enq_us : 0;
    int64_t send = t_done - t_pop;
    int64_t wr   = t->wr_us - t->au_wr_us0;      /* time inside write() for this AU */
    int64_t wrn  = t->wr_n  - t->au_wr_n0;
    int64_t cpu  = send - wr;                    /* packetize/mux/memcpy/locks */
    if (cpu < 0) cpu = 0;                        /* MS_TR_WR off, or clock skew */

    t->t_prev = t_done;
    t->n_au++;
    if (gap  > t->mx_gap)  t->mx_gap  = gap;
    if (age  > t->mx_age)  t->mx_age  = age;
    if (send > t->mx_send) t->mx_send = send;
    if (qcount > t->mx_q)  t->mx_q    = qcount;
    if (qcap >= 0)         t->q_cap   = qcap;

    if (gap < g_trace_thresh_us && age < g_trace_thresh_us &&
        send < g_trace_thresh_us)
        return;                                  /* healthy AU: stay silent */
    t->n_slow++;

    char q[24] = "";
    if (qcount >= 0) snprintf(q, sizeof q, " q=%d/%d", qcount, qcap);

    /* One line per slow AU. Read it as: which of gap/age/send is the big one.
     *   gap  big, age small  -> producer (encoder/HAL) starved this thread
     *   age  big, gap small  -> we were behind; backlog waited on us
     *   send big, wr ~= send -> the socket write itself blocked (link/peer)
     *   send big, wr small   -> time went inside timps (mux/copy/lock), not
     *                           on the wire */
    LOGW(MOD,"%s/%s chn=%d %s%s %zuB gap=%lld.%dms age=%lld.%dms "
             "send=%lld.%dms (wr=%lld.%dms/%lldw cpu=%lld.%dms)%s",
         t->tag, t->who, t->chn,
         media==MS_MEDIA_VIDEO ? "V" : (media==MS_MEDIA_AUDIO ? "A" : "J"),
         keyframe ? "*" : "", len,
         TMS(gap), TMS(age), TMS(send), TMS(wr), (long long)wrn, TMS(cpu), q);
}

void ms_trace_window(ms_trace_ctx *t, int64_t now)
{
    if (!ms_trace_on(MS_TR_SUM) || !t->win_us) return;
    if (now - t->win_us < MS_TRACE_WIN_US) return;

    /* an all-zero window on an idle (nobody-watching) connection is noise */
    if (t->n_au) {
        char q[24] = "";
        if (t->mx_q >= 0) snprintf(q, sizeof q, " maxq=%d/%d", t->mx_q, t->q_cap);
        LOGW(MOD,"%s/%s chn=%d %llds: au=%u slow=%u tx=%lluKiB "
                 "max gap=%lld.%dms age=%lld.%dms send=%lld.%dms "
                 "write=%lld.%dms (writes=%lld tot=%lld.%dms)%s",
             t->tag, t->who, t->chn,
             (long long)((now - t->win_us)/1000000),
             t->n_au, t->n_slow, (unsigned long long)(t->tx_bytes/1024),
             TMS(t->mx_gap), TMS(t->mx_age), TMS(t->mx_send), TMS(t->mx_wr),
             (long long)t->wr_n, TMS(t->wr_us), q);
    }
    t->win_us = now;
    t->n_au = t->n_slow = 0;
    t->mx_gap = t->mx_age = t->mx_send = t->mx_wr = 0;
    t->mx_q = (t->mx_q >= 0) ? 0 : -1;
}
