#include "frame.h"
#include <stdlib.h>
#include <string.h>

ms_pkt *pkt_new(const uint8_t *data, size_t len, int64_t pts_us, int keyframe, int media)
{
    ms_pkt *p = (ms_pkt*)malloc(sizeof(*p));
    if (!p) return NULL;
    p->data = (uint8_t*)malloc(len ? len : 1);
    if (!p->data) { free(p); return NULL; }
    if (len) memcpy(p->data, data, len);
    p->len = len;
    p->cap = len ? len : 1;
    p->pts_us = pts_us;
    p->enq_us = 0;           /* stamped unconditionally by hub_publish*() */
    p->keyframe = keyframe;
    p->media = media;
    p->_ref = 1;
    p->pool = NULL;      /* not pooled: pkt_unref() free()s as before */
    p->pnext = NULL;
    return p;
}

ms_pkt *pkt_ref(ms_pkt *p)
{
    if (p) __sync_add_and_fetch(&p->_ref, 1);
    return p;
}

void pkt_unref(ms_pkt *p)
{
    if (!p) return;
    if (__sync_sub_and_fetch(&p->_ref, 1) == 0) {
        pkt_pool *pool = p->pool;
        if (pool) {
            /* P-01: the last reference is gone, so no subscriber can still
             * touch this buffer - hand it back to the source pool for reuse
             * (no free()). Two destinations, split at keep_cap:
             *   <= keep_cap: the freelist, bounded by max_free, as before.
             *   >  keep_cap: the single ->big slot (R-01), so the recurring
             *     large frames recycle. A SECOND oversized buffer is freed
             *     rather than pinned - that is what keeps the idle ceiling at
             *     max_free*keep_cap + one big buffer instead of scaling with
             *     however many large frames happen to be in flight. */
            int pooled = 0;
            pthread_mutex_lock(&pool->lock);
            if (p->cap > pool->keep_cap) {
                if (!pool->big) { p->pnext = NULL; pool->big = p; pooled = 1; }
            } else if (pool->nfree < pool->max_free) {
                p->pnext = pool->freelist;
                pool->freelist = p;
                pool->nfree++;
                pooled = 1;
            }
            pthread_mutex_unlock(&pool->lock);
            if (pooled) return;   /* keep p->data allocated for the next frame */
        }
        free(p->data);
        free(p);
    }
}

void pkt_pool_init(pkt_pool *pool, int max_free, size_t keep_cap)
{
    pthread_mutex_init(&pool->lock, NULL);
    pool->freelist = NULL;
    pool->nfree    = 0;
    pool->max_free = max_free;
    pool->keep_cap = keep_cap;
    pool->big      = NULL;
}

void pkt_pool_trim(pkt_pool *pool)
{
    ms_pkt *b;
    pthread_mutex_lock(&pool->lock);
    b = pool->big;
    pool->big = NULL;
    pthread_mutex_unlock(&pool->lock);
    /* free outside the lock: the producer's next borrow must not wait on it */
    if (b) { free(b->data); free(b); }
}

ms_pkt *pkt_pool_get(pkt_pool *pool, size_t cap)
{
    ms_pkt *p = NULL;
    if (cap == 0) cap = 1;
    /* Size-matched dispatch (R-01). A request over keep_cap takes the ->big
     * slot and NEVER the freelist: raiding a warm small buffer for a large
     * frame would free its data anyway, so it only costs the next P-frame its
     * buffer. A request at or under keep_cap takes the freelist and NEVER
     * ->big: handing an IDR-sized buffer to a 2 KB P-frame is precisely the
     * cap >> len packet the fanqueue/record byte budgets cannot account for.
     * Either miss falls through to a fresh malloc, exactly as an empty
     * freelist always has. */
    pthread_mutex_lock(&pool->lock);
    if (cap > pool->keep_cap) {
        if (pool->big) { p = pool->big; pool->big = NULL; }
    } else if (pool->freelist) {
        p = pool->freelist;
        pool->freelist = p->pnext;
        pool->nfree--;
    }
    pthread_mutex_unlock(&pool->lock);
    if (!p) {
        p = (ms_pkt*)malloc(sizeof(*p));
        if (!p) return NULL;
        p->data = NULL;
        p->cap  = 0;
    }
    if (cap > p->cap) {
        /* free+malloc, not realloc: the borrow starts at len==0, so the old
         * contents are the previous frame's - bytes the caller overwrites and
         * nobody reads. realloc() would copy them anyway on every grow, and
         * it also has to work in place or move THIS block, while malloc is
         * free to serve a better-fitting chunk. */
        free(p->data);
        p->data = (uint8_t*)malloc(cap);
        if (!p->data) { free(p); return NULL; }   /* OOM: drop frame */
        p->cap  = cap;
    }
    p->len      = 0;
    p->pts_us   = 0;
    p->enq_us   = 0;        /* stamped unconditionally by hub_publish*() */
    p->keyframe = 0;
    p->media    = 0;
    p->_ref     = 1;
    p->pool     = pool;
    p->pnext    = NULL;
    return p;
}
