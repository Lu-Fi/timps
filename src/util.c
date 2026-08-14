#include "util.h"
#include <stdlib.h>
#include <limits.h>
#include <errno.h>

int ms_thread_create(pthread_t *t, size_t stack, void *(*fn)(void *), void *arg)
{
    pthread_attr_t a;
    if (pthread_attr_init(&a) != 0)
        return pthread_create(t, NULL, fn, arg);
#ifdef PTHREAD_STACK_MIN
    if (stack < (size_t)PTHREAD_STACK_MIN) stack = (size_t)PTHREAD_STACK_MIN;
#endif
    /* a rejected size (EINVAL) leaves the attr at its default - still valid */
    (void)pthread_attr_setstacksize(&a, stack);
    int r = pthread_create(t, &a, fn, arg);
    pthread_attr_destroy(&a);
    if (r != 0)
        r = pthread_create(t, NULL, fn, arg);   /* belt and braces */
    return r;
}

int ms_buf_init(ms_buf *b, size_t cap)
{
    if (cap < 64) cap = 64;
    b->data = (uint8_t*)malloc(cap);
    b->len = 0;
    b->cap = b->data ? cap : 0;
    b->err = b->data ? 0 : 1;
    return b->data ? 0 : -1;
}

int ms_buf_reserve(ms_buf *b, size_t extra)
{
    if (b->len + extra <= b->cap) return 0;
    size_t nc = b->cap ? b->cap : 64;
    while (nc < b->len + extra) nc *= 2;
    uint8_t *nd = (uint8_t*)realloc(b->data, nc);
    if (!nd) { b->err = 1; return -1; }
    b->data = nd; b->cap = nc;
    return 0;
}

int ms_buf_put(ms_buf *b, const void *src, size_t n)
{
    if (ms_buf_reserve(b, n)) return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

int ms_buf_u8(ms_buf *b, uint8_t v){ return ms_buf_put(b, &v, 1); }
int ms_buf_be16(ms_buf *b, uint16_t v){ uint8_t t[2]; wr_be16(t,v); return ms_buf_put(b,t,2); }
int ms_buf_be32(ms_buf *b, uint32_t v){ uint8_t t[4]; wr_be32(t,v); return ms_buf_put(b,t,4); }

void ms_buf_free(ms_buf *b){ free(b->data); b->data=NULL; b->len=b->cap=0; }

void ms_buf_reset(ms_buf *b, size_t soft)
{
    b->len = 0; b->err = 0;
    if (soft && b->cap > soft) {
        uint8_t *nd = (uint8_t*)realloc(b->data, soft);
        if (nd) { b->data = nd; b->cap = soft; }   /* shrink failure is harmless */
    }
}

int ms_base64(char *dst, const uint8_t *src, int n)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i=0, o=0;
    while (i+3 <= n){
        uint32_t v = (src[i]<<16)|(src[i+1]<<8)|src[i+2];
        dst[o++]=t[(v>>18)&63]; dst[o++]=t[(v>>12)&63];
        dst[o++]=t[(v>>6)&63];  dst[o++]=t[v&63];
        i+=3;
    }
    int rem = n-i;
    if (rem==1){
        uint32_t v=src[i]<<16;
        dst[o++]=t[(v>>18)&63]; dst[o++]=t[(v>>12)&63]; dst[o++]='='; dst[o++]='=';
    } else if (rem==2){
        uint32_t v=(src[i]<<16)|(src[i+1]<<8);
        dst[o++]=t[(v>>18)&63]; dst[o++]=t[(v>>12)&63]; dst[o++]=t[(v>>6)&63]; dst[o++]='=';
    }
    dst[o]=0;
    return o;
}


/* ---- stop gate (P-02): see util.h ---------------------------------------- */
void ms_stopgate_init(ms_stopgate *g)
{
    pthread_mutex_init(&g->lock, NULL);
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&g->cond, &a);
    pthread_condattr_destroy(&a);
    g->stop = 0;
}

int ms_stopgate_wait(ms_stopgate *g, int ms)
{
    /* MS_CLOCK_SCALE (sim builds only, see ms_now_us() in util.h): callers
     * pass VIRTUAL milliseconds; divide down to real ones so periodic
     * threads keep their configured virtual cadence under a compressed
     * clock. Floor at 1 real ms so an extreme scale degrades to a fast
     * tick, never a busy spin. */
#ifdef MS_CLOCK_SCALE
    ms /= (int)(MS_CLOCK_SCALE);
    if (ms < 1) ms = 1;
#endif
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec  += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L){ ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&g->lock);
    /* Predicate-before-wait AND predicate-in-loop: a stop set before we blocked
     * is caught here (we never enter the wait); a stop set while blocked is
     * delivered by the broadcast in ms_stopgate_stop(). ETIMEDOUT (interval
     * elapsed) or a spurious wake just re-tests the predicate. */
    while (!g->stop) {
        int r = pthread_cond_timedwait(&g->cond, &g->lock, &ts);
        if (r == ETIMEDOUT) break;
    }
    int stopped = g->stop;
    pthread_mutex_unlock(&g->lock);
    return stopped;
}

void ms_stopgate_stop(ms_stopgate *g)
{
    pthread_mutex_lock(&g->lock);
    g->stop = 1;
    pthread_cond_broadcast(&g->cond);
    pthread_mutex_unlock(&g->lock);
}

int ms_stopgate_stopped(ms_stopgate *g)
{
    pthread_mutex_lock(&g->lock);
    int stopped = g->stop;
    pthread_mutex_unlock(&g->lock);
    return stopped;
}
