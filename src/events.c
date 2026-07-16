/* events.c - tiny notify hub for the /events SSE push stream. See events.h.
 * Compiled to a no-op stub without -DUSE_CONTROL (the /events endpoint lives
 * under USE_CONTROL like /control, but the producers call events_notify()
 * unconditionally, so the stub keeps every build permutation linking). */
#include "events.h"

#ifdef USE_CONTROL
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

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

#endif
