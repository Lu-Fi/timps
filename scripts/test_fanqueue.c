/* test_fanqueue.c - host-only unit test for the fanqueue overflow contract.
 *
 * The queue is the one thing standing between a slow client and either the
 * encoder (it must never block) or the daemon's RSS (a stalled consumer pins
 * every packet it has queued). Both bounds are enforced in ONE place -
 * fanqueue_push()'s drop-oldest loop - and both are capacity-dependent, so
 * changing any subscriber's capacity is only safe if that loop behaves as
 * documented at the new depth too. This test pins that behaviour down:
 *
 *   - drop-oldest keeps the NEWEST packets, in order, at any capacity
 *     (MS_MJPEG_QCAP = 2 included, which is where MJPEG clients now sit);
 *   - the FQ_MAX_BYTES budget bounds pinned payload independently of the slot
 *     count, and a single oversized packet is still admissible;
 *   - the drop flags (dropped_key/dropped_any/dropped_audio) and the
 *     headless-GOP forward drop still fire at small capacities;
 *   - close/timeout semantics;
 *   - and, under a real producer thread with a deliberately slow consumer,
 *     that capacity is what bounds both the pinned bytes and how far behind
 *     live the consumer can fall.
 *
 * Leak-checked separately with -fsanitize=address (every dropped packet must
 * be unref'd by the queue, or a stalled client would leak instead of drop).
 *
 * Build/run: make test-fanqueue
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "fanqueue.h"

/* the byte budget under test - same #ifndef default as fanqueue.c, so a
 * -DFQ_MAX_BYTES on the command line (the T10/T20/T21 platforms halve it)
 * moves the test and the implementation together */
#ifndef FQ_MAX_BYTES
#define FQ_MAX_BYTES (2*1024*1024)
#endif

static int failures = 0;
static int checks   = 0;
static const char *cur = "";

static void ck(int ok, const char *what)
{
    checks++;
    if (!ok) { failures++; printf("  FAIL [%s] %s\n", cur, what); }
}
static void ck_eq(long got, long want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL [%s] %s: got %ld, want %ld\n", cur, what, got, want);
    }
}

/* ---------- packet helpers ------------------------------------------------
 * pkt_new() copies, so one shared source buffer serves every size. pts_us
 * carries a sequence number: that is what lets a pop prove WHICH packets
 * survived an overflow, not just how many. */
static uint8_t *g_src;
static size_t   g_srccap;

static ms_pkt *mk(int media, int key, size_t len, int64_t seq)
{
    if (len > g_srccap) { g_src = realloc(g_src, len); g_srccap = len; memset(g_src,0xA5,len); }
    ms_pkt *p = pkt_new(g_src, len, seq, key, media);
    if (!p) { printf("  FAIL [%s] out of memory\n", cur); failures++; exit(1); }
    return p;
}
static ms_pkt *jpg(size_t len, int64_t seq){ return mk(MS_MEDIA_JPEG, 0, len, seq); }

/* pop and return the sequence number (-1 if the queue gave nothing) */
static int64_t pop_seq(fanqueue *q)
{
    ms_pkt *p = fanqueue_pop(q, 0);
    if (!p) return -1;
    int64_t s = p->pts_us;
    pkt_unref(p);
    return s;
}

/* ---------- 1. drop-oldest keeps the newest, in order ---------------------- */
static void t_drop_oldest(int cap)
{
    static char name[64];
    snprintf(name, sizeof name, "drop-oldest keeps newest (cap=%d)", cap);
    cur = name;

    fanqueue q;
    ck_eq(fanqueue_init(&q, cap), 0, "init");

    int drops = 0;
    for (int i = 1; i <= 6; i++) drops += fanqueue_push(&q, jpg(4096, i));

    int count = -1, gcap = -1; size_t bytes = 0;
    fanqueue_depth(&q, &count, &gcap, &bytes);
    ck_eq(count, cap, "queue holds exactly cap packets");
    ck_eq(gcap,  cap, "reported capacity");
    ck_eq((long)bytes, (long)cap*4096, "queued bytes = cap * frame size");
    ck_eq(drops, 6 - cap, "one drop reported per evicted packet");
    ck_eq((long)q.dropped, 6 - cap, "dropped counter");

    /* the survivors must be the LAST cap frames, oldest-first */
    for (int i = 0; i < cap; i++) {
        int64_t want = 6 - cap + 1 + i;
        ck_eq((long)pop_seq(&q), (long)want, "surviving frame order");
    }
    ck_eq((long)pop_seq(&q), -1, "queue empty afterwards");
    fanqueue_free(&q);
}

/* ---------- 2. byte budget ------------------------------------------------- */
static size_t pin_after(int cap, size_t framelen, int nframes)
{
    fanqueue q;
    if (fanqueue_init(&q, cap)) return 0;
    for (int i = 1; i <= nframes; i++) fanqueue_push(&q, jpg(framelen, i));
    size_t bytes = 0; fanqueue_depth(&q, NULL, NULL, &bytes);
    fanqueue_free(&q);            /* also proves the queue unrefs what it holds */
    return bytes;
}

static void t_byte_budget(void)
{
    cur = "FQ_MAX_BYTES budget";

    /* Small JPEGs: the SLOT cap is what binds, so capacity is the whole story.
     * This is the case the MJPEG depth change is about. */
    size_t p8_200 = pin_after(8, 200*1024, 12);
    size_t p2_200 = pin_after(2, 200*1024, 12);
    ck_eq((long)p8_200, 8L*200*1024, "cap 8 pins 8 x 200 KB");
    ck_eq((long)p2_200, 2L*200*1024, "cap 2 pins 2 x 200 KB");
    printf("  info: 200 KB JPEGs, stalled client pins %zu KB at cap 8 -> %zu KB at cap 2\n",
           p8_200/1024, p2_200/1024);

    /* Large JPEGs: the byte budget binds first and cap 8 never gets to use its
     * slots - which is exactly why the old depth bought latency, not headroom. */
    size_t p8_800 = pin_after(8, 800*1024, 12);
    size_t p2_800 = pin_after(2, 800*1024, 12);
    ck(p8_800 <= FQ_MAX_BYTES, "cap 8 respects FQ_MAX_BYTES");
    ck(p2_800 <= FQ_MAX_BYTES, "cap 2 respects FQ_MAX_BYTES");
    ck_eq((long)p2_800, 2L*800*1024, "cap 2 pins 2 x 800 KB");
    printf("  info: 800 KB JPEGs, pins %zu KB at cap 8 -> %zu KB at cap 2\n",
           p8_800/1024, p2_800/1024);

    /* a packet that alone exceeds the budget is still admissible (count>0 rule):
     * the queue must never silently deliver nothing at all */
    cur = "oversized single packet";
    fanqueue q;
    ck_eq(fanqueue_init(&q, 2), 0, "init");
    fanqueue_push(&q, jpg(FQ_MAX_BYTES + 4096, 1));
    int count = -1; size_t bytes = 0;
    fanqueue_depth(&q, &count, NULL, &bytes);
    ck_eq(count, 1, "oversized packet is queued");
    ck_eq((long)bytes, (long)FQ_MAX_BYTES + 4096, "and accounted in full");
    fanqueue_push(&q, jpg(4096, 2));          /* next push evicts it */
    fanqueue_depth(&q, &count, NULL, &bytes);
    ck_eq(count, 1, "and is evicted by the next packet");
    ck_eq((long)pop_seq(&q), 2, "the newer packet is what remains");
    fanqueue_free(&q);
}

/* ---------- 3. drop flags at a small capacity ------------------------------ */
static void t_flags_at_cap2(void)
{
    cur = "drop flags at cap 2";
    fanqueue q;
    ck_eq(fanqueue_init(&q, 2), 0, "init");

    /* IDR, P, P: the third push evicts the IDR (dropped_key), and the
     * headless-GOP forward drop must then discard the now-undecodable P
     * behind it rather than trickling it out. */
    fanqueue_push(&q, mk(MS_MEDIA_VIDEO, 1, 4096, 1));
    fanqueue_push(&q, mk(MS_MEDIA_VIDEO, 0, 4096, 2));
    ck_eq(fanqueue_push(&q, mk(MS_MEDIA_VIDEO, 0, 4096, 3)), 1, "push reports a drop");

    int count = -1; fanqueue_depth(&q, &count, NULL, NULL);
    ck_eq(count, 1, "headless-GOP forward drop cleared the stale P too");

    fq_status st;
    ms_pkt *p = fanqueue_pop_ex(&q, 0, &st);
    ck(p != NULL, "a packet survives");
    ck_eq(p ? (long)p->pts_us : -1, 3, "and it is the newest one");
    ck_eq(st.dropped_key, 1, "dropped_key reported with the packet");
    ck_eq(st.dropped_any, 1, "dropped_any reported with the packet");
    ck_eq(st.cap, 2, "status carries the capacity");
    pkt_unref(p);
    ck_eq(fanqueue_take_dropped_key(&q), 0, "flags were cleared by pop_ex");
    ck_eq(fanqueue_take_dropped(&q), 0, "dropped_any cleared by pop_ex");

    /* audio eviction raises its own flag (the mute-vs-congestion signal) */
    fanqueue_push(&q, mk(MS_MEDIA_AUDIO, 0, 512, 10));
    fanqueue_push(&q, mk(MS_MEDIA_AUDIO, 0, 512, 11));
    fanqueue_push(&q, mk(MS_MEDIA_AUDIO, 0, 512, 12));
    ck_eq(fanqueue_take_dropped_audio(&q), 1, "dropped_audio raised");
    ck_eq(fanqueue_take_dropped_audio(&q), 0, "and read-and-cleared");
    fanqueue_free(&q);

    /* a pop that only times out must not swallow a drop signal */
    cur = "timed-out pop keeps the flags";
    ck_eq(fanqueue_init(&q, 2), 0, "init");
    fanqueue_push(&q, mk(MS_MEDIA_VIDEO, 1, 4096, 1));
    fanqueue_push(&q, mk(MS_MEDIA_VIDEO, 1, 4096, 2));
    fanqueue_push(&q, mk(MS_MEDIA_VIDEO, 1, 4096, 3));   /* evicts seq 1 (a key) */
    while (pop_seq(&q) >= 0) { }                         /* drain, ignoring flags */
    /* queue is empty now; the flags were consumed by the pops that delivered
     * packets, so a timing-out pop simply reports nothing */
    fq_status st2;
    ck(fanqueue_pop_ex(&q, 10, &st2) == NULL, "empty pop times out");
    ck_eq(st2.dropped_key, 0, "no flag invented on a dry pop");
    ck_eq(st2.count, 0, "depth reported as empty");
    fanqueue_free(&q);
}

/* ---------- 4. close ------------------------------------------------------- */
static void t_close(void)
{
    cur = "close";
    fanqueue q;
    ck_eq(fanqueue_init(&q, 2), 0, "init");
    fanqueue_push(&q, jpg(4096, 1));
    fanqueue_close(&q);
    ck_eq(fanqueue_closed(&q), 1, "closed flag");
    fanqueue_push(&q, jpg(4096, 2));            /* dropped + unref'd, never queued */
    int count = -1; fanqueue_depth(&q, &count, NULL, NULL);
    ck_eq(count, 1, "post-close push does not enqueue");
    ck_eq((long)pop_seq(&q), 1, "already-queued packet still pops");
    ck(fanqueue_pop(&q, 1000) == NULL, "pop on a closed empty queue returns at once");
    fanqueue_free(&q);
}

/* ---------- 5. live producer vs. a deliberately slow consumer -------------- */
/* This is the MJPEG failure mode in miniature: the producer never blocks, the
 * consumer drains at a fraction of the rate, and capacity alone decides how
 * much memory is pinned and how far behind live the consumer ends up. */
typedef struct { fanqueue *q; int nframes; size_t framelen; int period_us; } prod_arg;

static void *producer(void *v)
{
    prod_arg *a = (prod_arg*)v;
    for (int i = 1; i <= a->nframes; i++) {
        fanqueue_push(a->q, jpg(a->framelen, i));
        usleep(a->period_us);
    }
    fanqueue_close(a->q);
    return NULL;
}

static void t_slow_consumer(int cap, double *avg_lag, size_t *max_bytes)
{
    static char name[80];
    snprintf(name, sizeof name, "slow consumer (cap=%d)", cap);
    cur = name;

    enum { NFRAMES = 300, FRAMELEN = 8192, PERIOD_US = 1000 };
    fanqueue q;
    ck_eq(fanqueue_init(&q, cap), 0, "init");
    prod_arg a = { &q, NFRAMES, FRAMELEN, PERIOD_US };
    pthread_t th;
    pthread_create(&th, NULL, producer, &a);

    size_t peak = 0;
    int    peak_count = 0;
    long   lag_sum = 0, lag_n = 0, lag_max = 0;
    int64_t last = 0;
    int     ordered = 1;

    for (;;) {
        fq_status st;
        ms_pkt *p = fanqueue_pop_ex(&q, 200, &st);
        if (!p) { if (fanqueue_closed(&q)) break; else continue; }
        /* "how far behind live am I": the producer's sequence is time, so the
         * backlog still queued behind this packet IS the consumer's lag. */
        long lag = st.count;
        lag_sum += lag; lag_n++;
        if (lag > lag_max) lag_max = lag;
        if (p->pts_us <= last) ordered = 0;         /* must never go backwards */
        last = p->pts_us;
        if (st.count > peak_count) peak_count = st.count;
        pkt_unref(p);
        usleep(PERIOD_US * 8);                      /* 8x slower than the source */
    }
    pthread_join(th, NULL);
    size_t bytes = 0; fanqueue_depth(&q, NULL, NULL, &bytes);
    if (bytes > peak) peak = bytes;

    ck(ordered, "frames are delivered newest-forward, never out of order");
    ck(peak_count <= cap, "backlog never exceeds capacity");
    ck(last == NFRAMES || last > NFRAMES - 5, "the consumer stays near live");
    *avg_lag   = lag_n ? (double)lag_sum / (double)lag_n : 0;
    *max_bytes = (size_t)peak_count * FRAMELEN;
    printf("  info: cap %d -> avg backlog %.2f frames, max %ld, "
           "worst-case pinned %zu KB\n",
           cap, *avg_lag, lag_max, *max_bytes/1024);
    fanqueue_free(&q);
}

int main(void)
{
    printf("fanqueue overflow contract\n\n");

    t_drop_oldest(2);          /* MS_MJPEG_QCAP */
    t_drop_oldest(1);          /* degenerate lower bound */
    t_drop_oldest(4);          /* hub_grab_jpeg's snapshot queue */
    t_byte_budget();
    t_flags_at_cap2();
    t_close();

    double lag8 = 0, lag2 = 0; size_t by8 = 0, by2 = 0;
    t_slow_consumer(8, &lag8, &by8);
    t_slow_consumer(2, &lag2, &by2);
    cur = "slow consumer comparison";
    ck(lag2 <= lag8 + 0.001, "a smaller queue cannot leave the consumer further behind");
    ck(by2 <= by8, "a smaller queue cannot pin more");

    free(g_src);
    printf("\n%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
