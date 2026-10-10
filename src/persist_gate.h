#ifndef MS_PERSIST_GATE_H
#define MS_PERSIST_GATE_H
/* Rate limit for /control's config-file rewrites (control.c): a change after
 * a quiet window is written at once, changes inside the window are collected
 * and written together when it ends, so a dragged slider costs one rewrite
 * per window instead of one per POST (NOR/jffs2 erase cycles). Pure, caller
 * holds the lock; scripts/test_persist_gate.c. */

#ifndef PG_WINDOW_US
#define PG_WINDOW_US 3000000LL
#endif
#ifndef PG_RETRY_US
#define PG_RETRY_US 60000000LL      /* after a failed write (full/ro /etc) */
#endif

typedef struct {
    long long last_write;           /* us of the last write attempt, 0 = never */
    long long due;                  /* us the pending set is written at */
    int pending;
    int failing;                    /* the last write failed */
} pgate;

/* a POST changed keys: 1 = write now (with everything pending), 0 = deferred.
 * While writes fail every POST tries again, so its reply can say so. */
static inline int pg_post(pgate *g, long long now, int urgent)
{
    if (urgent || g->failing) return 1;
    if (!g->pending) {
        if (!g->last_write || now - g->last_write >= PG_WINDOW_US) return 1;
        g->due = g->last_write + PG_WINDOW_US;
        g->pending = 1;
    }
    return 0;
}

static inline void pg_wrote(pgate *g, long long now, int ok)
{
    g->last_write = now;
    g->failing = !ok;
    if (ok) { g->pending = 0; g->due = 0; }
    else    { g->pending = 1; g->due = now + PG_RETRY_US; }
}

static inline int pg_due(const pgate *g, long long now)
{
    return g->pending && now >= g->due;
}

#endif
