/* test_hub_pool.c - host-only unit test for the packet recycling pool
 * (src/frame.c: pkt_pool_get / pkt_unref / pkt_pool_trim).
 *
 * What this proves, and why it needs a harness rather than a soak:
 *
 * The pool's whole job is "which allocations does it NOT make". That is
 * invisible from the outside - a camera streams identically whether every IDR
 * costs a 300 KB malloc+free or none of them do - so the property is asserted
 * here directly by counting the malloc/free calls frame.c actually issues
 * (linker --wrap), rather than inferred from RSS or CPU on hardware.
 *
 * The three properties under test (R-01, dev_notes/FRAME_POOL_BIG_2026-09-05.md):
 *   1. a buffer over keep_cap is RETAINED on its last unref (one per pool) and
 *      handed back to the next over-keep_cap borrow with zero allocation,
 *   2. it is handed out ONLY to over-keep_cap borrows, so no published packet
 *      ever carries cap >> len (which the fanqueue/record byte budgets, which
 *      account ->len, could not see),
 *   3. pkt_pool_trim() gives it back, restoring the old idle ceiling.
 * Plus: the pre-existing freelist behaviour is unchanged, the refcount
 * contract still decides WHEN a buffer becomes reusable, and neither a
 * randomised fan-out nor a concurrent producer/consumer/trimmer leaks or
 * double-frees.
 *
 * Build/run: make test-hub-pool   (and: make test-hub-pool SAN=asan / SAN=tsan)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "frame.h"

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { failures++; \
        printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); \
        printf("\n"); } \
} while (0)

/* ---------- allocation accounting -----------------------------------------
 * --wrap only redirects calls from the objects in THIS link (the test itself
 * and src/frame.c), so libc's own internal allocations - printf buffers,
 * pthread bookkeeping - are not counted and cannot skew a comparison. The
 * test therefore allocates nothing of its own: every buffer below is either a
 * fixed array or comes from the pool under test. */
void *__real_malloc(size_t n);
void  __real_free(void *p);

static int g_nmalloc, g_nfree, g_nbig;   /* g_nbig: mallocs > BIG_THRESH */
#define BIG_THRESH (96*1024)

void *__wrap_malloc(size_t n)
{
    __sync_add_and_fetch(&g_nmalloc, 1);
    if (n > BIG_THRESH) __sync_add_and_fetch(&g_nbig, 1);
    return __real_malloc(n);
}
void __wrap_free(void *p)
{
    if (p) __sync_add_and_fetch(&g_nfree, 1);
    __real_free(p);
}

static int m0, f0, b0;
static void mark(void)   { m0 = g_nmalloc; f0 = g_nfree; b0 = g_nbig; }
static int  d_malloc(void){ return g_nmalloc - m0; }
static int  d_free(void)  { return g_nfree   - f0; }
static int  d_big(void)   { return g_nbig    - b0; }
static int  live(void)    { return g_nmalloc - g_nfree; }

/* Every buffer handed out must be writable to its full advertised capacity:
 * a cap that overstates the allocation is the one bookkeeping error that
 * would otherwise only surface as heap corruption on the camera. Under ASan
 * this turns into a hard report; unsanitised it is at least exercised. */
static void touch(ms_pkt *p)
{
    if (!p) return;
    memset(p->data, 0x5A, p->cap);
    p->len = p->cap > 64 ? 64 : p->cap;
}

/* Empty a pool completely so the global allocation count can be compared
 * against zero. The daemon never does this - pools are process-lifetime
 * statics by design (frame.h) - so it lives here, not in frame.c: borrow every
 * pooled buffer out, detach it from its pool, and let pkt_unref() really free
 * it. The loop ends on the first borrow that had to allocate, i.e. the first
 * one the pool could not serve. */
static void drain_pool(pkt_pool *pool)
{
    pkt_pool_trim(pool);
    for (;;) {
        mark();
        ms_pkt *p = pkt_pool_get(pool, 1);
        int fresh = d_malloc() > 0;
        if (!p) break;
        p->pool = NULL;
        pkt_unref(p);
        if (fresh) break;
    }
    pkt_pool_trim(pool);
}

#define KEEP  (96*1024)     /* the shipped HUB_POOL_KEEP_CAP */
#define SMALL 1000          /* a P-frame */
#define BIG   (300*1024)    /* an observed 1080p IDR */
#define HUGE  (500*1024)    /* a bigger one, forcing a grow */

/* ---------- 1: the freelist path is untouched ----------------------------- */
static void t_small_recycle(void)
{
    pkt_pool pool; pkt_pool_init(&pool, 4, KEEP);

    ms_pkt *a = pkt_pool_get(&pool, SMALL);
    CHECK(a != NULL, "first small borrow failed");
    if (!a) return;
    touch(a);
    uint8_t *data = a->data;
    CHECK(a->cap >= SMALL, "cap %zu < requested %d", a->cap, SMALL);
    CHECK(a->len == 64 || a->cap < 64, "len not settable");
    CHECK(a->_ref == 1, "borrow ref %d, want 1", a->_ref);
    CHECK(a->pool == &pool, "borrow not tagged with its pool");

    pkt_unref(a);
    mark();
    ms_pkt *b = pkt_pool_get(&pool, SMALL);
    CHECK(b == a, "small buffer not recycled (struct)");
    CHECK(b && b->data == data, "small buffer not recycled (data)");
    CHECK(d_malloc() == 0, "recycled small borrow still made %d malloc(s)", d_malloc());
    CHECK(b && b->len == 0, "recycled borrow must start at len 0");
    pkt_unref(b);

    drain_pool(&pool);
}

/* ---------- 2: the fix - a large buffer is retained and reused ------------- */
static void t_big_recycle(void)
{
    pkt_pool pool; pkt_pool_init(&pool, 4, KEEP);

    mark();
    ms_pkt *a = pkt_pool_get(&pool, BIG);
    CHECK(a != NULL, "first big borrow failed");
    if (!a) return;
    touch(a);
    uint8_t *data = a->data;
    CHECK(d_malloc() == 2, "first big borrow: %d mallocs, want 2 (struct+data)", d_malloc());
    CHECK(d_big() == 1, "first big borrow: %d large mallocs, want 1", d_big());

    mark();
    pkt_unref(a);
    CHECK(d_free() == 0, "last unref of a big buffer freed %d block(s) - "
                         "it must be retained (this is R-01)", d_free());

    mark();
    ms_pkt *b = pkt_pool_get(&pool, BIG);
    CHECK(b == a, "big buffer not recycled (struct)");
    CHECK(b && b->data == data, "big buffer not recycled (data)");
    CHECK(d_malloc() == 0, "recycled big borrow made %d malloc(s), want 0", d_malloc());
    if (b) touch(b);
    pkt_unref(b);

    /* and again, to show it is steady state and not a one-shot */
    mark();
    for (int i = 0; i < 20; i++) {
        ms_pkt *p = pkt_pool_get(&pool, BIG);
        CHECK(p == a, "big buffer lost on iteration %d", i);
        if (!p) break;
        touch(p);
        pkt_unref(p);
    }
    CHECK(d_malloc() == 0, "20 further big frames cost %d malloc(s), want 0", d_malloc());
    CHECK(d_free() == 0, "20 further big frames cost %d free(s), want 0", d_free());

    drain_pool(&pool);
}

/* ---------- 3: size-matched dispatch, both directions ---------------------- */
static void t_size_matched(void)
{
    pkt_pool pool; pkt_pool_init(&pool, 4, KEEP);

    /* park one big buffer in the pool, freelist empty */
    ms_pkt *big = pkt_pool_get(&pool, BIG);
    if (!big) { CHECK(0, "big borrow failed"); return; }
    touch(big);
    uint8_t *bigdata = big->data;
    pkt_unref(big);

    /* a P-frame must NOT be handed the IDR-sized buffer: that packet would be
     * published with cap 300 KB and len ~1 KB, memory no byte budget can see */
    ms_pkt *s = pkt_pool_get(&pool, SMALL);
    CHECK(s != NULL, "small borrow failed");
    if (!s) return;
    CHECK(s != big, "a small borrow was handed the retained big buffer");
    CHECK(s->cap <= KEEP, "small borrow got cap %zu > keep_cap %d", s->cap, KEEP);
    CHECK(s->cap == SMALL, "small borrow cap %zu, want exactly %d", s->cap, SMALL);
    touch(s);
    pkt_unref(s);

    /* ...and the big buffer is still there for the next IDR */
    mark();
    ms_pkt *b2 = pkt_pool_get(&pool, BIG);
    CHECK(b2 == big, "the retained big buffer was consumed by a small borrow");
    CHECK(b2 && b2->data == bigdata, "big buffer data replaced");
    CHECK(d_malloc() == 0, "big borrow after a small one made %d malloc(s)", d_malloc());
    if (b2) { touch(b2); pkt_unref(b2); }

    /* the converse: a big borrow must not raid the small freelist */
    pkt_pool_trim(&pool);
    ms_pkt *s1 = pkt_pool_get(&pool, SMALL);
    ms_pkt *s2 = pkt_pool_get(&pool, SMALL);
    if (!s1 || !s2) { CHECK(0, "small borrows failed"); return; }
    touch(s1); touch(s2);
    uint8_t *d1 = s1->data, *d2 = s2->data;
    pkt_unref(s1); pkt_unref(s2);          /* freelist now holds 2 */

    ms_pkt *b3 = pkt_pool_get(&pool, BIG); /* must malloc fresh, not raid */
    CHECK(b3 != s1 && b3 != s2, "a big borrow took a freelist buffer");
    if (b3) { touch(b3); pkt_unref(b3); }

    mark();
    ms_pkt *r2 = pkt_pool_get(&pool, SMALL);
    ms_pkt *r1 = pkt_pool_get(&pool, SMALL);
    CHECK(d_malloc() == 0, "the small freelist was disturbed by a big borrow "
                           "(%d malloc(s) on 2 small borrows)", d_malloc());
    CHECK((r1 && r1->data == d1) || (r1 && r1->data == d2), "small buffer 1 lost");
    CHECK((r2 && r2->data == d1) || (r2 && r2->data == d2), "small buffer 2 lost");
    pkt_unref(r1); pkt_unref(r2);

    drain_pool(&pool);
}

/* ---------- 4: exactly ONE oversized buffer is retained -------------------- */
static void t_one_big_only(void)
{
    pkt_pool pool; pkt_pool_init(&pool, 4, KEEP);

    ms_pkt *a = pkt_pool_get(&pool, BIG);
    ms_pkt *b = pkt_pool_get(&pool, BIG);
    ms_pkt *c = pkt_pool_get(&pool, BIG);
    if (!a || !b || !c) { CHECK(0, "big borrows failed"); return; }
    touch(a); touch(b); touch(c);

    mark();
    pkt_unref(a);                       /* -> retained */
    CHECK(d_free() == 0, "first big return freed %d block(s), want 0", d_free());
    mark();
    pkt_unref(b);                       /* -> slot taken, must be freed */
    CHECK(d_free() == 2, "second big return freed %d block(s), want 2 "
                         "(struct+data); the pool must pin only one", d_free());
    mark();
    pkt_unref(c);                       /* -> likewise */
    CHECK(d_free() == 2, "third big return freed %d block(s), want 2", d_free());

    drain_pool(&pool);
}

/* ---------- 5: trim ------------------------------------------------------- */
static void t_trim(void)
{
    pkt_pool pool; pkt_pool_init(&pool, 4, KEEP);

    /* trim on a fresh pool is a no-op */
    mark();
    pkt_pool_trim(&pool);
    CHECK(d_free() == 0, "trim on an empty pool freed %d block(s)", d_free());

    ms_pkt *a = pkt_pool_get(&pool, BIG);
    if (!a) { CHECK(0, "big borrow failed"); return; }
    touch(a);
    pkt_unref(a);

    mark();
    pkt_pool_trim(&pool);
    CHECK(d_free() == 2, "trim freed %d block(s), want 2 (struct+data)", d_free());

    /* idempotent */
    mark();
    pkt_pool_trim(&pool);
    pkt_pool_trim(&pool);
    CHECK(d_free() == 0, "repeated trim freed %d block(s), want 0", d_free());

    /* after a trim the next big frame allocates fresh - i.e. the idle ceiling
     * really did drop back to the freelist-only figure */
    mark();
    ms_pkt *b = pkt_pool_get(&pool, BIG);
    CHECK(d_malloc() == 2, "big borrow after trim made %d malloc(s), want 2", d_malloc());
    if (b) { touch(b); pkt_unref(b); }

    /* trim must leave the small freelist alone: it only targets the one
     * oversized buffer, so idle behaviour for P-frames is exactly as before */
    ms_pkt *s = pkt_pool_get(&pool, SMALL);
    if (!s) { CHECK(0, "small borrow failed"); return; }
    touch(s);
    uint8_t *sd = s->data;
    pkt_unref(s);
    pkt_pool_trim(&pool);
    mark();
    ms_pkt *s2 = pkt_pool_get(&pool, SMALL);
    CHECK(s2 && s2->data == sd, "trim discarded a freelist buffer");
    CHECK(d_malloc() == 0, "trim cost the freelist %d malloc(s)", d_malloc());
    pkt_unref(s2);

    drain_pool(&pool);
}

/* ---------- 6: growing and shrinking the retained buffer ------------------- */
static void t_grow_shrink(void)
{
    pkt_pool pool; pkt_pool_init(&pool, 4, KEEP);

    ms_pkt *a = pkt_pool_get(&pool, BIG);
    if (!a) { CHECK(0, "big borrow failed"); return; }
    touch(a); pkt_unref(a);

    /* a larger frame reuses the struct and regrows the data exactly once */
    mark();
    ms_pkt *b = pkt_pool_get(&pool, HUGE);
    CHECK(b == a, "grow lost the retained struct");
    CHECK(b && b->cap == HUGE, "grow gave cap %zu, want %d", b ? b->cap : 0, HUGE);
    CHECK(d_malloc() == 1 && d_free() == 1,
          "grow cost %d malloc / %d free, want 1 / 1", d_malloc(), d_free());
    if (b) { touch(b); pkt_unref(b); }

    /* a smaller frame afterwards reuses it as-is: cap never shrinks, so the
     * steady state after the largest IDR seen is zero allocation per frame */
    mark();
    ms_pkt *c = pkt_pool_get(&pool, BIG);
    CHECK(c == a, "shrink lost the retained struct");
    CHECK(c && c->cap == HUGE, "cap shrank to %zu", c ? c->cap : 0);
    CHECK(d_malloc() == 0 && d_free() == 0,
          "smaller borrow cost %d malloc / %d free, want 0 / 0", d_malloc(), d_free());
    if (c) { touch(c); pkt_unref(c); }

    drain_pool(&pool);
}

/* ---------- 7: the freelist bound is still max_free ------------------------ */
static void t_freelist_bound(void)
{
    pkt_pool pool; pkt_pool_init(&pool, 4, KEEP);

    ms_pkt *p[6];
    for (int i = 0; i < 6; i++) {
        p[i] = pkt_pool_get(&pool, SMALL);
        if (!p[i]) { CHECK(0, "small borrow %d failed", i); return; }
        touch(p[i]);
    }
    mark();
    for (int i = 0; i < 6; i++) pkt_unref(p[i]);
    CHECK(d_free() == 4, "returning 6 smalls to a max_free=4 pool freed %d "
                         "block(s), want 4 (2 packets x struct+data)", d_free());

    mark();
    for (int i = 0; i < 4; i++) { ms_pkt *q = pkt_pool_get(&pool, SMALL); touch(q); p[i] = q; }
    CHECK(d_malloc() == 0, "4 borrows from a full freelist made %d malloc(s)", d_malloc());
    mark();
    ms_pkt *extra = pkt_pool_get(&pool, SMALL);
    CHECK(d_malloc() == 2, "the 5th borrow made %d malloc(s), want 2", d_malloc());
    if (extra) { touch(extra); pkt_unref(extra); }
    for (int i = 0; i < 4; i++) pkt_unref(p[i]);

    drain_pool(&pool);
}

/* ---------- 8: the refcount contract still gates reuse --------------------- */
static void t_refcount_gate(void)
{
    pkt_pool pool; pkt_pool_init(&pool, 4, KEEP);

    ms_pkt *a = pkt_pool_get(&pool, BIG);
    if (!a) { CHECK(0, "big borrow failed"); return; }
    touch(a);
    /* three subscribers take a ref, the producer drops its own */
    pkt_ref(a); pkt_ref(a); pkt_ref(a);
    pkt_unref(a);
    CHECK(a->_ref == 3, "ref count %d after fan-out to 3, want 3", a->_ref);

    pkt_unref(a); pkt_unref(a);
    CHECK(a->_ref == 1, "ref count %d, want 1", a->_ref);
    /* still referenced: a borrow now must NOT hand out this buffer */
    ms_pkt *other = pkt_pool_get(&pool, BIG);
    CHECK(other != a, "a still-referenced buffer was handed out again");
    if (other) { touch(other); pkt_unref(other); }

    pkt_unref(a);              /* last subscriber: now it may be retained */
    drain_pool(&pool);
}

/* ---------- 9: pkt_new packets are never pooled ---------------------------- */
static void t_pkt_new_unpooled(void)
{
    static const uint8_t body[300*1024];
    ms_pkt *p = pkt_new(body, sizeof body, 1, 1, 0);
    if (!p) { CHECK(0, "pkt_new failed"); return; }
    CHECK(p->pool == NULL, "pkt_new packet carries a pool");
    mark();
    pkt_unref(p);
    CHECK(d_free() == 2, "pkt_new packet freed %d block(s) on last unref, want 2 "
                         "- an unpooled packet must still free exactly as before",
          d_free());
}

/* ---------- 10: randomised fan-out ---------------------------------------
 * Single-threaded but shaped like the real thing: a producer borrowing frames
 * whose sizes mix P-frames, IDRs and JPEGs, fanning each out to a random
 * number of consumer queues, consumers draining at their own pace, and trims
 * landing at arbitrary points. The invariant is that after a full drain every
 * block is accounted for. */
static unsigned rnd_s = 12345;
static unsigned rnd(unsigned n){ rnd_s = rnd_s*1103515245u + 12345u; return (rnd_s>>16) % n; }

static void t_random_fanout(void)
{
    pkt_pool pool; pkt_pool_init(&pool, 4, KEEP);
    ms_pkt *q[4][32];
    int qn[4] = {0,0,0,0};
    int drops = 0;

    for (int frame = 0; frame < 20000; frame++) {
        size_t cap;
        switch (rnd(10)) {
            case 0:  cap = BIG + rnd(200*1024); break;   /* IDR */
            case 1:  cap = 700*1024 + rnd(100*1024); break; /* 1080p JPEG */
            case 2:  cap = KEEP + 1; break;              /* just over the split */
            case 3:  cap = KEEP; break;                  /* exactly at it */
            case 4:  cap = 0; break;                     /* degenerate */
            default: cap = 500 + rnd(20000); break;      /* P-frame */
        }
        ms_pkt *p = pkt_pool_get(&pool, cap);
        if (!p) { drops++; continue; }
        CHECK(p->_ref == 1, "borrow ref %d", p->_ref);
        CHECK(p->len == 0, "borrow len %zu", p->len);
        CHECK(p->cap >= (cap ? cap : 1), "borrow cap %zu < %zu", p->cap, cap);
        memset(p->data, (int)(frame & 0xff), p->cap);
        p->len = cap;

        int nsub = (int)rnd(5);              /* 0..4 subscribers */
        for (int i = 0; i < nsub; i++) {
            if (qn[i] < 32) q[i][qn[i]++] = pkt_ref(p);
        }
        pkt_unref(p);                        /* producer drops its own ref */

        /* consumers drain a random prefix */
        for (int i = 0; i < 4; i++) {
            int take = (int)rnd(3);
            while (take-- > 0 && qn[i] > 0) {
                ms_pkt *h = q[i][0];
                memmove(q[i], q[i]+1, (size_t)(--qn[i]) * sizeof q[i][0]);
                pkt_unref(h);
            }
        }
        if (rnd(200) == 0) pkt_pool_trim(&pool);
    }
    for (int i = 0; i < 4; i++)
        while (qn[i] > 0) pkt_unref(q[i][--qn[i]]);

    CHECK(drops == 0, "%d borrow(s) failed under the random fan-out", drops);
    drain_pool(&pool);
}

/* ---------- 11: concurrent producer / consumers / trimmer ------------------
 * The real ownership pattern: ONE producer thread borrows and publishes, other
 * threads hold the last reference and return the buffer from their own thread,
 * and the trim lands from the producer's thread while packets are in flight.
 * Run this under ASan and TSan - that is where a lost buffer, a double free or
 * an unprotected freelist link would show. */
#define CC_SLOTS 64
static struct {
    pthread_mutex_t lock;
    pthread_cond_t  cv;
    ms_pkt         *slot[CC_SLOTS];
    int             n, head, done;
} cq[2];
static pkt_pool cc_pool;

static void *cc_consumer(void *arg)
{
    int id = (int)(long)arg;
    for (;;) {
        ms_pkt *p = NULL;
        pthread_mutex_lock(&cq[id].lock);
        while (cq[id].n == 0 && !cq[id].done)
            pthread_cond_wait(&cq[id].cv, &cq[id].lock);
        if (cq[id].n > 0) {
            p = cq[id].slot[cq[id].head];
            cq[id].head = (cq[id].head + 1) % CC_SLOTS;
            cq[id].n--;
        } else { pthread_mutex_unlock(&cq[id].lock); break; }
        pthread_mutex_unlock(&cq[id].lock);
        /* read the payload the way a sink does, AFTER the producer let go */
        volatile uint8_t sink = 0;
        for (size_t i = 0; i < p->len; i += 4096) sink ^= p->data[i];
        (void)sink;
        pkt_unref(p);
    }
    return NULL;
}

static void t_concurrent(void)
{
    pkt_pool_init(&cc_pool, 4, KEEP);
    for (int i = 0; i < 2; i++) {
        pthread_mutex_init(&cq[i].lock, NULL);
        pthread_cond_init(&cq[i].cv, NULL);
        cq[i].n = cq[i].head = cq[i].done = 0;
    }
    pthread_t th[2];
    for (int i = 0; i < 2; i++) pthread_create(&th[i], NULL, cc_consumer, (void*)(long)i);

    int dropped = 0;
    for (int frame = 0; frame < 60000; frame++) {
        size_t cap = (frame % 25 == 0) ? BIG + rnd(64*1024) : 1000 + rnd(8000);
        ms_pkt *p = pkt_pool_get(&cc_pool, cap);
        if (!p) { dropped++; continue; }
        memset(p->data, 0x11, p->cap);
        p->len = cap;
        for (int i = 0; i < 2; i++) {
            pthread_mutex_lock(&cq[i].lock);
            if (cq[i].n < CC_SLOTS) {
                cq[i].slot[(cq[i].head + cq[i].n) % CC_SLOTS] = pkt_ref(p);
                cq[i].n++;
                pthread_cond_signal(&cq[i].cv);
            }
            pthread_mutex_unlock(&cq[i].lock);
        }
        pkt_unref(p);
        /* the idle-stop trim, firing while packets are still in flight */
        if (frame % 997 == 0) pkt_pool_trim(&cc_pool);
    }
    for (int i = 0; i < 2; i++) {
        pthread_mutex_lock(&cq[i].lock);
        cq[i].done = 1;
        pthread_cond_broadcast(&cq[i].cv);
        pthread_mutex_unlock(&cq[i].lock);
    }
    for (int i = 0; i < 2; i++) pthread_join(th[i], NULL);
    for (int i = 0; i < 2; i++) {
        while (cq[i].n > 0) {
            ms_pkt *p = cq[i].slot[cq[i].head];
            cq[i].head = (cq[i].head + 1) % CC_SLOTS;
            cq[i].n--;
            pkt_unref(p);
        }
    }
    CHECK(dropped == 0, "%d borrow(s) failed under concurrent load", dropped);
    drain_pool(&cc_pool);
}

/* -------------------------------------------------------------------------- */
/* Every case drains its own pool before returning, so the process-wide
 * malloc/free balance must be back to zero at the end of each one: a retained
 * buffer that trim missed, a double free, or a packet the pool silently
 * dropped all show up here rather than only under a sanitiser. */
static void run(const char *name, void (*fn)(void))
{
    int before = failures;
    printf("%-42s", name);
    fflush(stdout);
    fn();
    CHECK(live() == 0, "%s left %d allocation(s) live", name, live());
    printf("%s\n", failures == before ? "ok" : "FAILED");
}

int main(void)
{
    printf("pkt_pool unit test (keep_cap=%d, max_free=4)\n\n", KEEP);
    run("freelist recycle (unchanged)",          t_small_recycle);
    run("oversized buffer recycles (R-01)",      t_big_recycle);
    run("size-matched dispatch",                 t_size_matched);
    run("at most one oversized buffer retained", t_one_big_only);
    run("pkt_pool_trim",                         t_trim);
    run("retained buffer grow/reuse",            t_grow_shrink);
    run("freelist bound is still max_free",      t_freelist_bound);
    run("refcount gates reuse",                  t_refcount_gate);
    run("pkt_new packets stay unpooled",         t_pkt_new_unpooled);
    run("randomised fan-out, 20k frames",        t_random_fanout);
    run("concurrent producer/consumers/trim",    t_concurrent);

    printf("\n%d/%d checks passed", checks - failures, checks);
    printf("   (%d malloc, %d free, %d live)\n", g_nmalloc, g_nfree, live());
    return failures ? 1 : 0;
}
