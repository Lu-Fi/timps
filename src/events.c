/* events.c - tiny notify hub for the /events SSE push stream. See events.h.
 * Compiled to a no-op stub without -DUSE_CONTROL (the /events endpoint lives
 * under USE_CONTROL like /control, but the producers call events_notify()
 * unconditionally, so the stub keeps every build permutation linking). */
#include "events.h"

#ifdef USE_CONTROL
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"

#define MOD "events"

static pthread_mutex_t g_mu   = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t  g_once = PTHREAD_ONCE_INIT;
static pthread_cond_t  g_cv;
static unsigned        g_gen;

/* condvar on CLOCK_MONOTONIC so a wall-clock jump (NTP sync on boot) cannot
 * stall the wait - it doubles as the SSE keepalive/stats timer */
static void cv_init(void)
{
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&g_cv, &a);
    pthread_condattr_destroy(&a);
}

void events_notify(void)
{
    pthread_once(&g_once, cv_init);
    pthread_mutex_lock(&g_mu);
    g_gen++;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

unsigned events_generation(void)
{
    pthread_mutex_lock(&g_mu);
    unsigned g = g_gen;
    pthread_mutex_unlock(&g_mu);
    return g;
}

/* motion snapshot ring: every changed grid is queued so no transition can
 * coalesce away between two subscriber samples (the old resample-the-level
 * design lost paired transitions to the memcmp dedup). g_mq_seq counts every
 * snapshot ever pushed; slot = seq % cap. Subscribers keep a private cursor
 * (their next seq to read) - no registration, nothing to leak on disconnect.
 * A consumer more than EV_MQ_CAP behind is lapped: pop skips it forward to
 * the oldest retained snapshot (drop-oldest, producer never blocks). */
#define EV_MQ_CAP 32
static ms_motion_status g_mq[EV_MQ_CAP];
static unsigned         g_mq_seq;

void events_motion_push(const ms_motion_status *st)
{
    pthread_once(&g_once, cv_init);
    pthread_mutex_lock(&g_mu);
    g_mq[g_mq_seq % EV_MQ_CAP] = *st;
    g_mq_seq++;
    g_gen++;                             /* implies events_notify() */
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

unsigned events_motion_cursor(void)
{
    pthread_mutex_lock(&g_mu);
    unsigned s = g_mq_seq;
    pthread_mutex_unlock(&g_mu);
    return s;
}

int events_motion_pop(unsigned *cursor, ms_motion_status *out)
{
    int got = 0;
    pthread_mutex_lock(&g_mu);
    if (g_mq_seq - *cursor > EV_MQ_CAP)  /* lapped: skip to oldest retained */
        *cursor = g_mq_seq - EV_MQ_CAP;
    if (*cursor != g_mq_seq){
        *out = g_mq[*cursor % EV_MQ_CAP];
        (*cursor)++;
        got = 1;
    }
    pthread_mutex_unlock(&g_mu);
    return got;
}

/* config table: settings are LEVEL state (not edges like motion), so a
 * fixed table keyed by name - pushing an already-tracked key overwrites it
 * in place, coalescing bursts (a dragged slider keeps exactly one slot).
 * g_cfg_seq is a global counter, bumped on every push; each slot remembers
 * the seq it was last written at so subscribers can ask "anything with
 * seq > mine?" without a ring. Only when a genuinely new key needs a slot
 * and all EV_CFG_SLOTS are held by other still-live keys does the oldest
 * get evicted - g_cfg_evict_seq then marks the newest evicted seq so a
 * subscriber that hadn't caught up to it yet knows it lost a key and must
 * resync (one GET /control) instead of silently missing that update.
 * Sized at 24 (~4.9KB BSS, 204B/slot) so the most common bulk save - the
 * Image Quality page, ~22 keys in one POST (see control.c IMG[]) - fits in
 * one push without forcing every other open tab through the resync/refetch
 * fallback path; 8 slots made that the common case instead of the rare
 * one. */
#define EV_CFG_SLOTS 24
typedef struct {
    char     key[40];
    char     val[160];
    unsigned seq;      /* 0 = slot never used */
} ev_cfg_slot;
static ev_cfg_slot g_cfgtab[EV_CFG_SLOTS];
static unsigned    g_cfg_seq;
static unsigned    g_cfg_evict_seq;

void events_config_push(const char *key, const char *val)
{
    pthread_once(&g_once, cv_init);
    pthread_mutex_lock(&g_mu);
    int slot = -1, empty = -1, oldest = -1;
    for (int i = 0; i < EV_CFG_SLOTS; i++) {
        if (g_cfgtab[i].seq && !strcmp(g_cfgtab[i].key, key)) { slot = i; break; }
        if (!g_cfgtab[i].seq && empty < 0) empty = i;
        if (g_cfgtab[i].seq && (oldest < 0 || g_cfgtab[i].seq < g_cfgtab[oldest].seq)) oldest = i;
    }
    if (slot < 0) {
        slot = (empty >= 0) ? empty : oldest;
        /* evicting a DIFFERENT, still-live key: mark it lost */
        if (slot == oldest && g_cfgtab[oldest].seq && strcmp(g_cfgtab[oldest].key, key) &&
            g_cfgtab[oldest].seq > g_cfg_evict_seq)
            g_cfg_evict_seq = g_cfgtab[oldest].seq;
    }
    snprintf(g_cfgtab[slot].key, sizeof g_cfgtab[slot].key, "%s", key);
    snprintf(g_cfgtab[slot].val, sizeof g_cfgtab[slot].val, "%s", val);
    g_cfgtab[slot].seq = ++g_cfg_seq;
    g_gen++;                             /* implies events_notify() */
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

unsigned events_config_cursor(void)
{
    pthread_mutex_lock(&g_mu);
    unsigned s = g_cfg_seq;
    pthread_mutex_unlock(&g_mu);
    return s;
}

int events_config_resync(unsigned *cursor)
{
    int need = 0;
    pthread_mutex_lock(&g_mu);
    if (*cursor < g_cfg_evict_seq) { *cursor = g_cfg_evict_seq; need = 1; }
    pthread_mutex_unlock(&g_mu);
    return need;
}

int events_config_pop(unsigned *cursor, char *key, int keycap, char *val, int valcap)
{
    int got = 0, best = -1;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < EV_CFG_SLOTS; i++)
        if (g_cfgtab[i].seq > *cursor && (best < 0 || g_cfgtab[i].seq < g_cfgtab[best].seq))
            best = i;
    if (best >= 0) {
        snprintf(key, keycap, "%s", g_cfgtab[best].key);
        snprintf(val, valcap, "%s", g_cfgtab[best].val);
        *cursor = g_cfgtab[best].seq;
        got = 1;
    }
    pthread_mutex_unlock(&g_mu);
    return got;
}

/* daynight decision history ring - see events.h. Heap, not BSS: the 48 h
 * ceiling is 270 KiB and most cameras never open the tuning graph, so the
 * allocation follows daynight.history_s instead of always standing at the
 * maximum. That is safe to resize under load precisely because every access
 * (push, stat, range) happens inside g_mu - a reader can never be holding a
 * pointer into the old buffer while the producer frees it. */
static pthread_mutex_t g_dnh_mu = PTHREAD_MUTEX_INITIALIZER;
static ms_dn_sample   *g_dnh;
static unsigned        g_dnh_cap;
static unsigned        g_dnh_seq;    /* every sample ever pushed */

static uint32_t mono_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)ts.tv_sec;
}

/* caller holds g_dnh_mu */
static void dnh_resize(unsigned newcap)
{
    if (newcap == g_dnh_cap) return;
    if (!newcap) {
        free(g_dnh); g_dnh = NULL; g_dnh_cap = 0;
        LOGI(MOD, "daynight history off");
        return;
    }
    ms_dn_sample *nb = (ms_dn_sample *)calloc(newcap, sizeof *nb);
    if (!nb) {
        static int warned;       /* retried every sample - say so only once */
        if (!warned++)
            LOGW(MOD, "daynight history: cannot allocate %u samples, keeping %u",
                 newcap, g_dnh_cap);
        return;
    }
    /* carry the newest samples over rather than dropping the series on the
     * floor every time the retention is nudged; g_dnh_seq stays monotonic so
     * client cursors survive the re-layout */
    unsigned have = g_dnh_seq < g_dnh_cap ? g_dnh_seq : g_dnh_cap;
    unsigned keep = have < newcap ? have : newcap;
    for (unsigned i = 0; i < keep; i++) {
        unsigned s = g_dnh_seq - keep + i;
        nb[s % newcap] = g_dnh[s % g_dnh_cap];
    }
    free(g_dnh);
    g_dnh = nb; g_dnh_cap = newcap;
    LOGI(MOD, "daynight history %u samples (%u s, %u B)",
         newcap, newcap * DN_HIST_PERIOD_S,
         (unsigned)(newcap * sizeof *nb));
}

void events_dn_hist_push(ms_dn_sample *s, int retain_s)
{
    unsigned want = (retain_s > 0)
                  ? (unsigned)retain_s / DN_HIST_PERIOD_S : 0;
    if (retain_s > 0 && !want) want = 1;
    pthread_mutex_lock(&g_dnh_mu);
    dnh_resize(want);
    if (g_dnh_cap) {
        s->t = mono_sec();
        g_dnh[g_dnh_seq % g_dnh_cap] = *s;
        g_dnh_seq++;
    }
    pthread_mutex_unlock(&g_dnh_mu);
}

void events_dn_hist_stat(unsigned *head, unsigned *oldest, int *cap,
                         uint32_t *t_now)
{
    pthread_mutex_lock(&g_dnh_mu);
    if (head)   *head   = g_dnh_seq;
    if (oldest) *oldest = g_dnh_seq > g_dnh_cap ? g_dnh_seq - g_dnh_cap : 0;
    if (cap)    *cap    = (int)g_dnh_cap;
    pthread_mutex_unlock(&g_dnh_mu);
    if (t_now) *t_now = mono_sec();
}

int events_dn_hist_range(unsigned *from, int max, ms_dn_sample *out, int *lapped)
{
    int n = 0;
    if (lapped) *lapped = 0;
    pthread_mutex_lock(&g_dnh_mu);
    if (g_dnh_cap) {
        unsigned oldest = g_dnh_seq > g_dnh_cap ? g_dnh_seq - g_dnh_cap : 0;
        /* signed compares, so a cursor kept across a daemon restart (seq back
         * at 0, client's still in the thousands) reads as "ahead" and resyncs
         * instead of wrapping into "billions behind" */
        if ((int)(*from - oldest) < 0 || (int)(*from - g_dnh_seq) > 0) {
            *from = oldest;
            if (lapped) *lapped = 1;
        }
        while (n < max && *from != g_dnh_seq)
            out[n++] = g_dnh[(*from)++ % g_dnh_cap];
    }
    pthread_mutex_unlock(&g_dnh_mu);
    return n;
}

unsigned events_wait(unsigned last_gen, int timeout_ms)
{
    pthread_once(&g_once, cv_init);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L){ ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&g_mu);
    while (g_gen == last_gen &&
           pthread_cond_timedwait(&g_cv, &g_mu, &ts) == 0)
        ;
    unsigned g = g_gen;
    pthread_mutex_unlock(&g_mu);
    return g;
}

#else /* !USE_CONTROL: producers still link, notification goes nowhere */

void events_notify(void) {}
void events_motion_push(const ms_motion_status *st) { (void)st; }
void events_config_push(const char *key, const char *val) { (void)key; (void)val; }
void events_dn_hist_push(ms_dn_sample *s, int retain_s) { (void)s; (void)retain_s; }

#endif
