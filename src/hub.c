#include "hub.h"
#include "log.h"
#include <string.h>

#define MOD "HUB"
static hub_source g_src[HUB_NSRC];
static void (*g_idr_cb)(int) = NULL;
static void (*g_act_cb)(int,int) = NULL;

void hub_set_idr_cb(void (*cb)(int src)){ g_idr_cb = cb; }
void hub_request_idr(int src){ if (g_idr_cb) g_idr_cb(src); }
void hub_set_activity_cb(void (*cb)(int src, int active)){ g_act_cb = cb; }

/* Activity callback serialization. Notifications are LEVEL based (derived
 * from the current nsub at callback time), never edge based, so concurrent
 * subscribe/unsubscribe can never deliver a stale "stop" after a "start".
 * g_act_lock serializes callbacks; s->lock is NOT held while calling out
 * (the callback may call back into hub functions). */
static pthread_mutex_t g_act_lock = PTHREAD_MUTEX_INITIALIZER;

static void hub_notify_activity(int src)
{
    hub_source *s = hub_get(src);
    if (!s || !g_act_cb) return;
    pthread_mutex_lock(&g_act_lock);
    /* re-read nsub NOW: the last notification to run always carries the
     * current subscriber state, so a source with consumers is never stopped */
    int n;
    pthread_mutex_lock(&s->lock); n = s->nsub; pthread_mutex_unlock(&s->lock);
    g_act_cb(src, n > 0 ? 1 : 0);
    pthread_mutex_unlock(&g_act_lock);
}

int hub_active(int src)
{
    hub_source *s = hub_get(src); if(!s) return 0;
    int n; pthread_mutex_lock(&s->lock); n=s->nsub; pthread_mutex_unlock(&s->lock);
    return n>0;
}

int hub_subs(int src)
{
    hub_source *s = hub_get(src); if(!s) return 0;
    int n; pthread_mutex_lock(&s->lock); n=s->nsub; pthread_mutex_unlock(&s->lock);
    return n;
}

int hub_video_subs(void)
{
    int total=0;
    for (int i=0;i<MS_MAX_VSTREAM;i++){
        hub_source *s=hub_get(i); if(!s) continue;
        pthread_mutex_lock(&s->lock); total+=s->nsub; pthread_mutex_unlock(&s->lock);
    }
    return total;
}

static void (*g_control_cb)(const char*,const char*) = NULL;
void hub_set_control_cb(void (*cb)(const char *key, const char *val)){ g_control_cb = cb; }
void hub_control(const char *key, const char *val){ if (g_control_cb) g_control_cb(key, val); }

static void (*g_control_commit_cb)(void) = NULL;
void hub_set_control_commit_cb(void (*cb)(void)){ g_control_commit_cb = cb; }
void hub_control_commit(void){ if (g_control_commit_cb) g_control_commit_cb(); }

/* hub_publish() snapshots s->subs[] under s->lock and then pushes to those
 * queues AFTER releasing s->lock (see hub_publish). Since hub_unsubscribe()
 * callers destroy/free their fanqueue right after it returns, a push that
 * already snapshotted a queue must be allowed to finish before unsubscribe
 * for that queue returns - otherwise fanqueue_push() could run against a
 * queue whose mutex/cond has just been destroyed (use-after-free).
 * hub_publish() has exactly one producer thread per source (one video/jpeg/
 * audio thread per hub_source), so a plain per-source busy flag - not a
 * counter - is enough to track "a push for this source is in flight".
 * Kept as hub.c-local arrays (not fields on hub_source in hub.h) since this
 * is purely an internal publish/unsubscribe handshake; protected by the
 * same per-source s->lock as the rest of hub_source. */
static int             g_pushing[HUB_NSRC];
static pthread_cond_t  g_push_done[HUB_NSRC];

/* P-01: one recycling packet pool per hub source. The producer assembles each
 * access unit straight into a pooled buffer and hands it to hub_publish_take(),
 * so the old per-frame malloc + full-frame copy (pkt_new in hub_publish) is
 * gone on the converted paths. Pools are process-lifetime statics (never
 * freed): a slow subscriber can hold a packet long after the producer stopped,
 * and the last unref must always find a live pool to return the buffer to.
 *
 * Sizing (per source):
 *   HUB_POOL_MAX_FREE - idle buffers kept for reuse. The idle case needs just
 *     1 (the producer keeps publishing with 0 subs through the stop-debounce
 *     window, borrowing+returning the SAME buffer with zero churn). A few more
 *     absorb the small in-flight working set of a couple of responsive clients
 *     (build + a handful queued) without malloc churn; a genuinely slow client
 *     or the recorder pre-roll ring pins many buffers, but those exceed the
 *     pool and simply fall back to malloc/free - i.e. no worse than today.
 *   HUB_POOL_KEEP_CAP - buffers that ratcheted past this (a one-off large IDR)
 *     are freed on return rather than pinned idle, bounding worst-case idle
 *     pool memory to HUB_POOL_MAX_FREE * HUB_POOL_KEEP_CAP per source
 *     (~384 KB). High-frequency P-frames stay well under it and recycle for
 *     free; the ~1/GOP oversized IDR pays a realloc-grow + free, negligible. */
#ifndef HUB_POOL_MAX_FREE
#define HUB_POOL_MAX_FREE 4
#endif
#ifndef HUB_POOL_KEEP_CAP
#define HUB_POOL_KEEP_CAP (96*1024)
#endif
static pkt_pool        g_pool[HUB_NSRC];

void hub_init(void)
{
    for (int i=0;i<HUB_NSRC;i++){
        memset(&g_src[i], 0, sizeof(hub_source));
        pthread_mutex_init(&g_src[i].lock, NULL);
        g_pushing[i] = 0;
        pthread_cond_init(&g_push_done[i], NULL);
        pkt_pool_init(&g_pool[i], HUB_POOL_MAX_FREE, HUB_POOL_KEEP_CAP);
    }
}

ms_pkt *hub_pkt_get(int src, size_t cap)
{
    if (src < 0 || src >= HUB_NSRC) return NULL;
    return pkt_pool_get(&g_pool[src], cap);
}

hub_source *hub_get(int src)
{
    if (src<0 || src>=HUB_NSRC) return NULL;
    return &g_src[src];
}

void hub_set_video_params(int src, int vcodec, int w, int h, int fps)
{
    hub_source *s = hub_get(src); if(!s) return;
    pthread_mutex_lock(&s->lock);
    s->active=1; s->vcodec=vcodec; s->width=w; s->height=h; s->fps=fps;
    vparam_init(&s->vp, vcodec);
    s->vp_ready=0;
    pthread_mutex_unlock(&s->lock);
}

int hub_get_video_params(int src, int *vcodec, int *w, int *h, int *fps)
{
    hub_source *s = hub_get(src); if(!s) return 0;
    int act;
    pthread_mutex_lock(&s->lock);
    act = s->active;
    if (vcodec) *vcodec = s->vcodec;
    if (w)      *w      = s->width;
    if (h)      *h      = s->height;
    if (fps)    *fps    = s->fps;
    pthread_mutex_unlock(&s->lock);
    return act;
}

int hub_get_vparam(int src, vparam *out)
{
    hub_source *s = hub_get(src); if(!s) return 0;
    int ready;
    pthread_mutex_lock(&s->lock);
    *out = s->vp;
    ready = s->vp_ready;
    pthread_mutex_unlock(&s->lock);
    return ready;
}

double hub_get_fps(int src)
{
    hub_source *s = hub_get(src); if(!s) return 0.0;
    double fps;
    pthread_mutex_lock(&s->lock);
    fps = s->mfps;
    pthread_mutex_unlock(&s->lock);
    return fps;
}

double hub_get_bitrate(int src)
{
    hub_source *s = hub_get(src); if(!s) return 0.0;
    double kbps;
    pthread_mutex_lock(&s->lock);
    /* bwin only advances while the producer publishes; if the last window is
     * stale the stream is idle (on-demand encoder stopped) - report 0 rather
     * than a frozen last-seen rate. */
    kbps = (ms_now_us() - s->bwin < 2000000) ? s->mkbps : 0.0;
    pthread_mutex_unlock(&s->lock);
    return kbps;
}

void hub_set_audio_params(int acodec, int samplerate, int channels)
{
    hub_source *s = hub_get(HUB_AUDIO_SRC); if(!s) return;
    pthread_mutex_lock(&s->lock);
    s->active=1; s->acodec=acodec; s->samplerate=samplerate; s->channels=channels;
    pthread_mutex_unlock(&s->lock);
}

void hub_clear_audio_params(void)
{
    hub_source *s = hub_get(HUB_AUDIO_SRC); if(!s) return;
    pthread_mutex_lock(&s->lock);
    s->active=0;
    pthread_mutex_unlock(&s->lock);
}

int hub_get_audio(int *acodec, int *samplerate, int *channels)
{
    hub_source *s = hub_get(HUB_AUDIO_SRC); if(!s) return 0;
    int act;
    pthread_mutex_lock(&s->lock);
    act = s->active;
    if (acodec) *acodec = s->acodec;
    if (samplerate) *samplerate = s->samplerate;
    if (channels) *channels = s->channels;
    pthread_mutex_unlock(&s->lock);
    return act;
}

int hub_subscribe(int src, fanqueue *q)
{
    hub_source *s = hub_get(src); if(!s) return -1;
    int rc=-1, nsnap=0;
    pthread_mutex_lock(&s->lock);
    if (s->nsub < HUB_MAX_SUBS){ s->subs[s->nsub++]=q; rc=0; }
    nsnap = s->nsub;   /* L11: snapshot for logging - nsub can change again
                        * the moment the lock is released */
    pthread_mutex_unlock(&s->lock);
    if (rc==0){
        LOGD(MOD,"subscribe src=%d nsub=%d", src, nsnap);
        hub_notify_activity(src);      /* level based, not edge based */
    }
    return rc;
}

void hub_unsubscribe(int src, fanqueue *q)
{
    hub_source *s = hub_get(src); if(!s) return;
    pthread_mutex_lock(&s->lock);
    for (int i=0;i<s->nsub;i++){
        if (s->subs[i]==q){
            s->subs[i]=s->subs[--s->nsub];
            break;
        }
    }
    /* Wait out any hub_publish() push already in flight for this source: it
     * may have snapshotted subs[] (possibly including q) before we removed
     * q above, and our caller frees/destroys q right after we return. */
    while (g_pushing[src])
        pthread_cond_wait(&g_push_done[src], &s->lock);
    int nsnap = s->nsub;   /* L11: snapshot for logging (see hub_subscribe) */
    pthread_mutex_unlock(&s->lock);
    LOGD(MOD,"unsubscribe src=%d nsub=%d", src, nsnap);
    hub_notify_activity(src);          /* level based, not edge based */
}

/* Shared per-frame under-lock bookkeeping for both hub_publish() (borrowed
 * buffer, copies) and hub_publish_take() (pooled buffer, no copy). Updates the
 * cached vparam/fps/bitrate for VIDEO and snapshots the subscriber list, all
 * under s->lock. vparam_update() reads the AU buffer HERE, exactly as before,
 * at the same point in the sequence relative to the push - the take path passes
 * the same (data,len) it will hand to subscribers, and the producer's packet is
 * still fully owned at this point, so vparam sees identical bytes. Raises
 * g_pushing[src] while a subscriber snapshot is "out" so a concurrent
 * hub_unsubscribe() waits for the push loop before destroying a fanqueue.
 * Returns the snapshot count; sets *pushing (nonzero => caller must call
 * hub_finish_push()). */
static int hub_prepare_locked(hub_source *s, int src,
                              const uint8_t *data, size_t len,
                              int keyframe, int media,
                              fanqueue **subs_snap, int *pushing)
{
    int nsub_snap;
    *pushing = 0;
    pthread_mutex_lock(&s->lock);
    if (media == MS_MEDIA_VIDEO) {
        if (keyframe || !s->vp_ready) {
            if (vparam_update(&s->vp, data, len)) s->vp_ready = 1;
        }
        int64_t now = ms_now_us();
        if (s->fwin == 0) s->fwin = now;
        s->fcount++;
        if (now - s->fwin >= 1000000) {
            s->mfps = s->fcount * 1000000.0 / (now - s->fwin);
            s->fcount = 0; s->fwin = now;
        }
        if (s->bwin == 0) s->bwin = now;
        s->bcount += len;
        if (now - s->bwin >= 1000000) {
            /* bytes -> kbit/s: bytes*8/1000 over (now-bwin)/1e6 seconds */
            s->mkbps = s->bcount * 8000.0 / (now - s->bwin);
            s->bcount = 0; s->bwin = now;
        }
    }
    nsub_snap = s->nsub;
    for (int i=0;i<nsub_snap;i++) subs_snap[i] = s->subs[i];
    if (nsub_snap > 0) { g_pushing[src] = 1; *pushing = 1; }
    pthread_mutex_unlock(&s->lock);
    return nsub_snap;
}

/* Clear the in-flight-push flag and wake any hub_unsubscribe() waiting on it. */
static void hub_finish_push(int src)
{
    hub_source *s = hub_get(src); if(!s) return;
    pthread_mutex_lock(&s->lock);
    g_pushing[src] = 0;
    pthread_cond_broadcast(&g_push_done[src]);
    pthread_mutex_unlock(&s->lock);
}

void hub_publish(int src, const uint8_t *data, size_t len,
                 int64_t pts_us, int keyframe, int media)
{
    hub_source *s = hub_get(src); if(!s) return;

    fanqueue *subs_snap[HUB_MAX_SUBS];
    int pushing = 0;
    int nsub_snap = hub_prepare_locked(s, src, data, len, keyframe, media,
                                       subs_snap, &pushing);

    if (nsub_snap == 0) return;      /* nobody listening: skip the malloc + copy */

    /* The big pkt_new (malloc + up to ~1 MB memcpy) is done AFTER releasing the
     * lock. Prefer hub_publish_take() on the hot video/JPEG paths to avoid this
     * copy entirely; this borrowed-buffer path stays for the audio producers
     * (tiny frames) and any caller that cannot assemble into a pooled buffer. */
    ms_pkt *p = pkt_new(data, len, pts_us, keyframe, media);
    if (p) {
        /* push after releasing s->lock; each push takes its own ref, the
         * builder's own reference is released once below. */
        for (int i=0;i<nsub_snap;i++)
            fanqueue_push(subs_snap[i], pkt_ref(p));
        pkt_unref(p);
    }

    if (pushing) hub_finish_push(src);   /* clear even if pkt_new failed */
}

void hub_publish_take(int src, ms_pkt *p,
                      int64_t pts_us, int keyframe, int media)
{
    hub_source *s = hub_get(src);
    if (!s || !p) { pkt_unref(p); return; }   /* pkt_unref(NULL) is a no-op */

    p->pts_us   = pts_us;
    p->keyframe = keyframe;
    p->media    = media;

    fanqueue *subs_snap[HUB_MAX_SUBS];
    int pushing = 0;
    int nsub_snap = hub_prepare_locked(s, src, p->data, p->len, keyframe, media,
                                       subs_snap, &pushing);

    if (nsub_snap == 0) {
        /* 0 subscribers: consume the producer's reference. This returns the
         * buffer straight to the source pool (no copy, no free) - invariant (a):
         * publishing through the idle-stop window stays a borrow+return of the
         * same buffer, never a per-frame malloc/free. */
        pkt_unref(p);
        return;
    }

    /* Zero-copy fan-out: each push takes its own ref; the producer's own
     * reference is released once below (its buffer returns to the pool only
     * once the last subscriber has drained it). */
    for (int i=0;i<nsub_snap;i++)
        fanqueue_push(subs_snap[i], pkt_ref(p));
    pkt_unref(p);

    if (pushing) hub_finish_push(src);
}
