#include "util.h"
#include "fanqueue.h"
#include <stdlib.h>
#include <sys/socket.h>
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

/* ---- live client registry (M-1/M-3): see util.h for the whole rationale --- */
int ms_creg_add(ms_client_reg *r, int fd)
{
    int slot = -1;
    pthread_mutex_lock(&r->lock);
    for (int i = 0; i < r->n; i++)
        if (!r->s[i].fd1) {
            r->s[i].fd1 = fd + 1;
            r->s[i].q   = NULL;   /* a recycled slot must not inherit a queue */
            slot = i;
            break;
        }
    pthread_mutex_unlock(&r->lock);
    return slot;
}

void ms_creg_set_queue(ms_client_reg *r, int slot, fanqueue *q)
{
    if (slot < 0 || slot >= r->n) return;
    pthread_mutex_lock(&r->lock);
    r->s[slot].q = q;
    pthread_mutex_unlock(&r->lock);
}

void ms_creg_del(ms_client_reg *r, int slot)
{
    if (slot < 0 || slot >= r->n) return;
    pthread_mutex_lock(&r->lock);
    r->s[slot].q   = NULL;
    r->s[slot].fd1 = 0;
    pthread_mutex_unlock(&r->lock);
}

void ms_creg_wake_all(ms_client_reg *r)
{
    pthread_mutex_lock(&r->lock);
    for (int i = 0; i < r->n; i++) {
        /* queue first: a thread that the shutdown() below wakes out of a
         * send()/recv() then finds the queue already closed and leaves on its
         * next loop test, instead of going back to sleep for one more pop
         * timeout. Closing costs nothing if it was already closed. */
        if (r->s[i].q)   fanqueue_close(r->s[i].q);
        /* shutdown(), never close(): the owning thread still owns this fd and
         * will close it itself. SHUT_RDWR so a blocked send() fails too, not
         * just a blocked recv(). */
        if (r->s[i].fd1) shutdown(r->s[i].fd1 - 1, SHUT_RDWR);
    }
    pthread_mutex_unlock(&r->lock);
}

void ms_json_esc(const char *s, char *out, size_t cap)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t o=0;
    if (!cap) return;
    while (*p){
        unsigned char c = *p;
        if (c=='"' || c=='\\'){
            if (o+2 >= cap) break;
            out[o++]='\\'; out[o++]=(char)c; p++;
        } else if (c < 0x20){
            if (o+1 >= cap) break;
            out[o++]=' '; p++;
        } else if (c < 0x80){
            if (o+1 >= cap) break;
            out[o++]=(char)c; p++;
        } else {
            /* lead byte of a multi-byte sequence: decode + validate */
            int n; unsigned cp=0;
            if      ((c & 0xE0)==0xC0){ n=2; cp=c&0x1F; }
            else if ((c & 0xF0)==0xE0){ n=3; cp=c&0x0F; }
            else if ((c & 0xF8)==0xF0){ n=4; cp=c&0x07; }
            else n=0;                       /* stray continuation / 0xF8+ */
            int ok = n>0;
            for (int i=1; ok && i<n; i++){  /* p is NUL-terminated: a short
                                             * sequence hits \0 here and fails,
                                             * never reading past the string */
                if ((p[i] & 0xC0) != 0x80) ok=0;
                else cp = (cp<<6) | (p[i]&0x3F);
            }
            if (ok){
                if      (n==2 && cp<0x80)    ok=0;   /* overlong */
                else if (n==3 && cp<0x800)   ok=0;
                else if (n==4 && cp<0x10000) ok=0;
                if (cp>=0xD800 && cp<=0xDFFF) ok=0; /* surrogate */
                if (cp>0x10FFFF)              ok=0;
            }
            if (ok){
                if (o+(size_t)n >= cap) break;
                for (int i=0;i<n;i++) out[o++]=(char)p[i];
                p += n;
            } else {
                if (o+3 >= cap) break;              /* emit U+FFFD, skip 1 byte */
                out[o++]=(char)0xEF; out[o++]=(char)0xBF; out[o++]=(char)0xBD;
                p++;
            }
        }
    }
    out[o]=0;
}

/* ---- shared media-tree filesystem helpers --------------------------------
 * moved here from word-identical twins in record.c/timelapse.c; the WHY of
 * each check (L10, L-2, F4) is on the declarations in util.h. */

#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

int ms_has_dotdot(const char *s)
{
    if (!s) return 0;
    if (!strcmp(s, "..") || !strncmp(s, "../", 3) || strstr(s, "/../")) return 1;
    size_t n = strlen(s);
    return (n >= 3 && !strcmp(s + n - 3, "/.."));
}

int ms_path_unsafe(const char *dir, const char *name)
{
    if (ms_has_dotdot(dir)) return 1;
    if (name && (ms_has_dotdot(name) || name[0] == '/')) return 1;
    return 0;
}

long long ms_free_mb(const char *dir)
{
    struct statvfs vf;
    if (statvfs(dir, &vf) != 0) return -1;
    return (long long)((vf.f_bavail * (unsigned long long)vf.f_frsize) / (1024*1024));
}

void ms_mkdirs(const char *path)
{
    char tmp[512]; snprintf(tmp, sizeof tmp, "%s", path);
    char *slash = strrchr(tmp, '/'); if (!slash) return; *slash = 0;
    for (char *p = tmp+1; *p; p++){
        if (*p == '/'){ *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

void ms_hostname(char *out, size_t cap)
{
    if (!cap) return;
    if (gethostname(out, cap) != 0) snprintf(out, cap, "camera");
    out[cap-1] = 0;   /* F4: an overlong hostname may come back unterminated */
}

time_t ms_media_path(char *out, size_t cap, const char *dir, const char *sub,
                     const char *name, const char *ext)
{
    time_t t = time(NULL); struct tm tmv; localtime_r(&t, &tmv);
    char rel[160];
    if (strftime(rel, sizeof rel, name, &tmv) == 0)
        snprintf(rel, sizeof rel, "%ld", (long)t);
    char host[64];
    ms_hostname(host, sizeof host);
    snprintf(out, cap, "%s/%s/%s/%s%s", dir, host, sub, rel, ext);
    return t;
}
