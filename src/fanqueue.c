#include "fanqueue.h"
#include <stdlib.h>
#include <errno.h>
#include <time.h>

int fanqueue_init(fanqueue *q, int cap)
{
    q->slots = (ms_pkt**)calloc(cap, sizeof(ms_pkt*));
    if (!q->slots) return -1;
    q->cap = cap; q->head=q->tail=q->count=0;
    q->closed = 0; q->dropped = 0; q->dropped_key = 0;
    pthread_mutex_init(&q->lock, NULL);
    /* condvar on CLOCK_MONOTONIC: a wall-clock step (NTP sync on boot) must
     * never stretch a consumer's pop timeout (like events.c does) */
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&q->cond, &a);
    pthread_condattr_destroy(&a);
    return 0;
}

void fanqueue_free(fanqueue *q)
{
    if (!q->slots) return;
    for (int i=0;i<q->count;i++)
        pkt_unref(q->slots[(q->head+i)%q->cap]);
    free(q->slots); q->slots=NULL;
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->cond);
}

int fanqueue_push(fanqueue *q, ms_pkt *p)
{
    int dropped = 0;
    pthread_mutex_lock(&q->lock);
    if (q->closed) { pthread_mutex_unlock(&q->lock); pkt_unref(p); return 0; }
    if (q->count == q->cap) {
        /* drop oldest */
        ms_pkt *old = q->slots[q->head];
        q->head = (q->head+1)%q->cap;
        q->count--;
        /* remember if a video keyframe was lost (flag read by the consumer,
         * which then requests a fresh IDR from its source) */
        if (old->media==MS_MEDIA_VIDEO && old->keyframe) q->dropped_key = 1;
        pkt_unref(old);
        q->dropped++;
        dropped = 1;

        /* Once a keyframe has been lost, any video packets still queued
         * belong to a now-headless GOP: undecodable until the next keyframe.
         * Rather than trickle those out to the consumer one overflow at a
         * time, drop forward through consecutive non-keyframe video packets
         * at the head right now, leaving any interleaved audio packets in
         * place (audio decodes independently of the video GOP). Stops as
         * soon as a keyframe (fresh or otherwise) reaches the head, so this
         * never discards packets belonging to an already-valid GOP.
         * dropped_key is cleared by the consumer via
         * fanqueue_take_dropped_key(); until then this is a no-op once the
         * head is back at a keyframe or non-video packet. */
        if (q->dropped_key) {
            while (q->count > 0) {
                ms_pkt *h = q->slots[q->head];
                if (h->media != MS_MEDIA_VIDEO || h->keyframe) break;
                q->head = (q->head+1)%q->cap;
                q->count--;
                pkt_unref(h);
                q->dropped++;
            }
        }
    }
    q->slots[q->tail] = p;
    q->tail = (q->tail+1)%q->cap;
    q->count++;
    pthread_mutex_unlock(&q->lock);
    pthread_cond_signal(&q->cond);
    return dropped;
}

ms_pkt *fanqueue_pop(fanqueue *q, int timeout_ms)
{
    pthread_mutex_lock(&q->lock);
    while (q->count==0 && !q->closed) {
        if (timeout_ms < 0) {
            pthread_cond_wait(&q->cond, &q->lock);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            ts.tv_sec  += timeout_ms/1000;
            ts.tv_nsec += (long)(timeout_ms%1000)*1000000L;
            if (ts.tv_nsec >= 1000000000L){ ts.tv_sec++; ts.tv_nsec-=1000000000L; }
            int r = pthread_cond_timedwait(&q->cond, &q->lock, &ts);
            if (r == ETIMEDOUT) break;
        }
    }
    ms_pkt *p = NULL;
    if (q->count>0) {
        p = q->slots[q->head];
        q->head=(q->head+1)%q->cap;
        q->count--;
    }
    pthread_mutex_unlock(&q->lock);
    return p;
}

int fanqueue_take_dropped_key(fanqueue *q)
{
    pthread_mutex_lock(&q->lock);
    int k = q->dropped_key;
    q->dropped_key = 0;
    pthread_mutex_unlock(&q->lock);
    return k;
}

void fanqueue_close(fanqueue *q)
{
    pthread_mutex_lock(&q->lock);
    q->closed = 1;
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->lock);
}
