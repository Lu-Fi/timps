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

void hub_init(void)
{
    for (int i=0;i<HUB_NSRC;i++){
        memset(&g_src[i], 0, sizeof(hub_source));
        pthread_mutex_init(&g_src[i].lock, NULL);
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
    return s->mfps;
}

void hub_set_audio_params(int acodec, int samplerate, int channels)
{
    hub_source *s = hub_get(HUB_AUDIO_SRC); if(!s) return;
    pthread_mutex_lock(&s->lock);
    s->active=1; s->acodec=acodec; s->samplerate=samplerate; s->channels=channels;
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
    pthread_mutex_unlock(&s->lock);
    LOGD(MOD,"unsubscribe src=%d nsub=%d", src, s->nsub);
    hub_notify_activity(src);          /* level based, not edge based */
}

void hub_publish(int src, const uint8_t *data, size_t len,
                 int64_t pts_us, int keyframe, int media)
{
    hub_source *s = hub_get(src); if(!s) return;
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
    if (s->nsub == 0) { pthread_mutex_unlock(&s->lock); return; }
    ms_pkt *p = pkt_new(data, len, pts_us, keyframe, media);
    if (!p){ pthread_mutex_unlock(&s->lock); return; }
    for (int i=0;i<s->nsub;i++)
        fanqueue_push(s->subs[i], pkt_ref(p));
    pthread_mutex_unlock(&s->lock);
    pkt_unref(p);
}
