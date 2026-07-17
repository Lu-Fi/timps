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

void hub_init(void)
{
    for (int i=0;i<HUB_NSRC;i++){
        memset(&g_src[i], 0, sizeof(hub_source));
        pthread_mutex_init(&g_src[i].lock, NULL);
        g_pushing[i] = 0;
        pthread_cond_init(&g_push_done[i], NULL);
    }
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
    int rc=-1;
    pthread_mutex_lock(&s->lock);
    if (s->nsub < HUB_MAX_SUBS){ s->subs[s->nsub++]=q; rc=0; }
    pthread_mutex_unlock(&s->lock);
    if (rc==0){
        LOGD(MOD,"subscribe src=%d nsub=%d", src, s->nsub);
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
    pthread_mutex_unlock(&s->lock);
    LOGD(MOD,"unsubscribe src=%d nsub=%d", src, s->nsub);
    hub_notify_activity(src);          /* level based, not edge based */
}

void hub_publish(int src, const uint8_t *data, size_t len,
                 int64_t pts_us, int keyframe, int media)
{
    hub_source *s = hub_get(src); if(!s) return;

    /* Build the refcounted packet (malloc + full-frame memcpy, up to ~1MB for
     * an IDR) BEFORE taking s->lock. Holding the lock across the copy AND
     * the fan-out push stalls every other s->lock user (hub_active/
     * hub_get_vparam/hub_get_fps/hub_subscribe, incl. the producer threads
     * themselves, OSD, SSE stats) behind a single slow subscriber. */
    ms_pkt *p = pkt_new(data, len, pts_us, keyframe, media);
    if (!p) return;

    /* Under the lock: only update the cached vparam/fps state and snapshot
     * the current subscriber list into a small local array. No malloc/memcpy
     * and no queue pushes happen while s->lock is held. g_pushing[src] is
     * raised while a snapshot is "out" so a concurrent hub_unsubscribe()
     * knows to wait for our push loop below before letting its caller
     * destroy the fanqueue (see hub_unsubscribe / g_pushing comment). */
    fanqueue *subs_snap[HUB_MAX_SUBS];
    int nsub_snap;
    int pushing = 0;
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
    }
    nsub_snap = s->nsub;
    for (int i=0;i<nsub_snap;i++) subs_snap[i] = s->subs[i];
    if (nsub_snap > 0) { g_pushing[src] = 1; pushing = 1; }
    pthread_mutex_unlock(&s->lock);

    /* Push to the (snapshotted) subscriber queues after releasing s->lock.
     * Each push takes its own ref; the builder's own reference (from
     * pkt_new) is released once below - same net refcount as before. */
    for (int i=0;i<nsub_snap;i++)
        fanqueue_push(subs_snap[i], pkt_ref(p));
    pkt_unref(p);

    if (pushing) {
        pthread_mutex_lock(&s->lock);
        g_pushing[src] = 0;
        pthread_cond_broadcast(&g_push_done[src]);
        pthread_mutex_unlock(&s->lock);
    }
}
