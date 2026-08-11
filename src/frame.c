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
             * (no free()) unless the pool is already full or the buffer
             * ratcheted past keep_cap (a one-off large IDR), in which case
             * free it to bound idle pool memory to max_free*keep_cap/source. */
            int pooled = 0;
            pthread_mutex_lock(&pool->lock);
            if (pool->nfree < pool->max_free && p->cap <= pool->keep_cap) {
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
}

ms_pkt *pkt_pool_get(pkt_pool *pool, size_t cap)
{
    ms_pkt *p = NULL;
    pthread_mutex_lock(&pool->lock);
    if (pool->freelist) {
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
    if (cap == 0) cap = 1;
    if (cap > p->cap) {
        uint8_t *nd = (uint8_t*)realloc(p->data, cap);
        if (!nd) { free(p->data); free(p); return NULL; }  /* OOM: drop frame */
        p->data = nd;
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
