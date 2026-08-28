/* timelapse.c - native timelapse: every timelapse.interval_s the most recent
 * JPEG frame is written to <timelapse.dir>/<host>/timelapses/<strftime>.jpg
 * (mkdir -p, tmp file + rename). Mirrors record.c: a thread gated by
 * timelapse.enabled that idles unsubscribed while off.
 *
 * JPEG source and grab: shared with /snapshot.jpg?chn=N in mp4/httpd.c via
 * hub_pick_jpeg_src()/hub_grab_jpeg() (see hub.h/hub.c) - prefer the JPEG
 * encoder piggybacked on timelapse.channel, fall back to the dedicated
 * jpeg.* channel, then to any stream with a piggyback encoder. Shots
 * subscribe just-in-time like snapshot_jpg(): the pipeline is woken only
 * long enough to deliver one fresh frame per interval, then released so the
 * HAL idle-stop debounce shuts encoder + framesource back down - the old
 * always-subscribed loop kept them running 24/7 and discarded
 * interval_s*fps encodes per kept frame.
 * Retention: *.jpg older than keep_days are pruned (emptied directories are
 * removed), at most hourly - see TL_PRUNE_US.
 *
 * Only built with USE_TIMELAPSE (default on); without it the stubs at the
 * bottom keep the call sites (main.c/control.c) unconditional, like srt.c. */
#ifdef USE_TIMELAPSE
#include "timelapse.h"
#include "hub.h"
#include "frame.h"
#include "log.h"
#include "util.h"

#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>

#define MOD "TL"

static const ms_config *g_tc;
static ms_stopgate      g_gate;   /* P-02: stop-condvar (was a volatile run flag + slice-sleep) */
static pthread_t        g_thr;
static int              g_started;

static pthread_mutex_t  g_lock = PTHREAD_MUTEX_INITIALIZER;
/* status snapshot (under g_lock) */
static long long        g_count;
static time_t           g_last_t;
static char             g_lastfile[160];

/* ---- filesystem helpers ----
 * ms_path_unsafe / ms_free_mb / ms_mkdirs / ms_media_path live in util.c,
 * shared with record.c - the '..' check is a security check (L10/L-2); see
 * util.h for the chosen component semantics. */

/* delete *.jpg older than cutoff under base; rmdir prunes emptied dirs
 * (fails harmlessly on non-empty ones). Depth-bounded. */
static void prune_old(const char *base, time_t cutoff, int depth)
{
    if (depth>8) return;
    DIR *d=opendir(base); if(!d) return;
    struct dirent *e;
    while ((e=readdir(d))){
        if (!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")) continue;
        char p[336]; snprintf(p,sizeof p,"%s/%s",base,e->d_name);
        struct stat s; if (lstat(p,&s)!=0) continue;
        if (S_ISDIR(s.st_mode)){
            prune_old(p,cutoff,depth+1);
            rmdir(p);
        } else if (S_ISREG(s.st_mode)){
            size_t l=strlen(p);
            if (l>4 && !strcmp(p+l-4,".jpg") && s.st_mtime<cutoff){
                if (unlink(p)==0) LOGI(MOD,"pruned %s",p);
            }
        }
    }
    closedir(d);
}

/* how often prune_old() is actually allowed to walk the tree. prune() is
 * called after EVERY successful shot, but the retention it enforces has
 * DAY granularity - at a 10 s interval and a multi-day keep_days that walked
 * and lstat'd every kept JPEG six times a minute, forever, to move a cutoff
 * that had not moved. Hourly is still ~24x finer than the cutoff steps, and
 * leaves the card idle between shots (the whole point of the just-in-time
 * grab below). */
#define TL_PRUNE_US (3600*1000000LL)

/* keep_days comes from the caller's under-lock timelapse snapshot (F-02). */
static void prune(int keep_days)
{
    int days=keep_days;
    if (days<=0) return;
    /* only tl_thread calls prune(), so the guard needs no locking. Starts at 0
     * so the first shot after a start/restart prunes immediately - that is the
     * one time the backlog can be arbitrarily old (the daemon was down). */
    static int64_t next_walk_us;
    int64_t now=ms_now_us();
    if (next_walk_us && now<next_walk_us) return;
    next_walk_us = now + TL_PRUNE_US;
    /* timelapse.dir is runtime-mutable via /control: snapshot it under the
     * config string lock (never hold the lock across filesystem calls) */
    char dir[128];
    config_str_lock();
    snprintf(dir,sizeof dir,"%s",g_tc->timelapse.dir);
    config_str_unlock();
    if (ms_path_unsafe(dir,NULL)) return;  /* never prune outside the tree (L10) */
    char host[64]; ms_hostname(host,sizeof host);   /* F4 handling lives in ms_hostname */
    char base[208]; snprintf(base,sizeof base,"%s/%s/timelapses",dir,host);
    prune_old(base, time(NULL)-(time_t)days*86400, 0);
}

/* ---- shot writer ---- */

static int shot_write(const ms_pkt *p)
{
    /* timelapse.dir/name are runtime-mutable via /control: snapshot them
     * under the config string lock before strftime/path building */
    char dir[128], name[96];
    config_str_lock();
    snprintf(dir,sizeof dir,"%s",g_tc->timelapse.dir);
    snprintf(name,sizeof name,"%s",g_tc->timelapse.name);
    config_str_unlock();
    if (ms_path_unsafe(dir,name)){
        LOGE(MOD,"unsafe timelapse.dir/name ('..' or absolute name), skipping shot");
        return -1;
    }
    char path[512], tmp[520];
    time_t t = ms_media_path(path,sizeof path,dir,"timelapses",name,".jpg");
    snprintf(tmp,sizeof tmp,"%s.tmp",path);
    ms_mkdirs(path);
    FILE *f=fopen(tmp,"wb");
    if (!f){ LOGE(MOD,"open %s: %s",tmp,strerror(errno)); return -1; }
    /* short write = SD yanked / disk full: drop the shot, keep the loop alive */
    int werr = (fwrite(p->data,1,p->len,f) != p->len);
    if (fclose(f)!=0) werr=1;
    if (werr || rename(tmp,path)!=0){
        LOGE(MOD,"write %s: %s",path,strerror(errno));
        unlink(tmp);
        return -1;
    }
    pthread_mutex_lock(&g_lock);
    g_count++; g_last_t=t;
    snprintf(g_lastfile,sizeof g_lastfile,"%.*s",(int)sizeof(g_lastfile)-1,path);
    pthread_mutex_unlock(&g_lock);
    LOGI(MOD,"snapshot -> %s (%zu bytes)",path,p->len);
    return 0;
}

/* ---- thread ---- */

/* Just-in-time frame grab: timps encodes nothing while idle, so subscribing
 * to the JPEG hub source is what WAKES that encoder. The actual two-phase
 * cold-wake grab (including the piggyback parent-video wake on a cold
 * start) is shared with mp4/httpd.c's snapshot_jpg() as hub_grab_jpeg() -
 * see hub.h/hub.c for the full mechanism. All subscriptions it takes are
 * dropped before it returns, so the HAL idle-stop debounce (MS_IDLE_STOP_US,
 * 2 s) shuts the pipeline back down between shots (encoder/framesource/ISP
 * load between shots = 0, instead of interval_s*fps discarded encodes per
 * kept frame; intervals below ~5 s stay within the debounce window, so short
 * intervals do not churn start/stop). Freshness is inherent: the fanqueue
 * only ever receives frames published AFTER the subscribe, and every JPEG
 * is standalone - no keyframe wait needed. */

/* re-try delay after a frameless grab (encoder cold-start hiccup); a failed
 * WRITE (SD yanked/full) still waits the full interval like it always did */
#define TL_RETRY_US (5*1000000LL)

static void *tl_thread(void *arg)
{
    (void)arg;
    int64_t next_us=0;
    int cur_src=-1;                 /* last announced source, -1 = idle */

    while (!ms_stopgate_stopped(&g_gate)){
        /* read the live config every pass so channel/interval changes from
         * /control take effect without a daemon restart (like record.c).
         * F-02: snapshot the whole timelapse section once per pass under the
         * config string lock (daynight.c pattern) and use the local copy, so the
         * /control writer never races these int reads (dir is still snapshotted
         * at its own point of use in prune()). */
        ms_timelapse_cfg tl;
        config_str_lock();
        tl = g_tc->timelapse;
        config_str_unlock();
        int chn=tl.channel; if (chn<0||chn>=MS_MAX_VSTREAM) chn=0;
        int src = tl.enabled ? hub_pick_jpeg_src(g_tc,chn,0) : -1;

        if (src<0){
            if (cur_src>=0){ LOGI(MOD,"timelapse idle"); cur_src=-1; }
            next_us=0;
            /* P-02: disabled - one stop-aware wait instead of a 300 ms poll.
             * The 1 s cadence only bounds how fast a /control ENABLE is picked
             * up; shutdown is immediate (stop wakes the wait). */
            if (ms_stopgate_wait(&g_gate, 1000)) break;
            continue;
        }
        if (src!=cur_src){
            LOGI(MOD,"timelapse active (src=%d interval=%ds, just-in-time)",
                 src,tl.interval_s);
            cur_src=src;
        }

        int64_t now=ms_now_us();
        if (!next_us) next_us=now;   /* first shot right after enable */
        if (now < next_us){
            /* P-02: wait until the next shot (or stop), capped at 1 s so a live
             * interval/channel/disable change from /control is still picked up
             * within ~1 s on the next pass. Stop wakes the wait immediately.
             * One wakeup/s while waiting instead of 300 ms slices. */
            int64_t left=next_us-now;
            int wait_ms = left > 1000000 ? 1000 : (int)(left/1000);
            if (wait_ms < 1) wait_ms = 1;
            if (ms_stopgate_wait(&g_gate, wait_ms)) break;
            continue;
        }

        int iv=tl.interval_s; if (iv<1) iv=1;
        int64_t retry = TL_RETRY_US < (int64_t)iv*1000000 ? TL_RETRY_US
                                                          : (int64_t)iv*1000000;
        ms_pkt *p=hub_grab_jpeg(src,HUB_JPEG_GRAB_WAIT_MS,NULL,NULL,NULL);
        if (p){
            int ok = (shot_write(p)==0);
            pkt_unref(p);
            if (ok) prune(tl.keep_days);
            next_us = now + (int64_t)iv*1000000;
        } else {
            LOGW(MOD,"no frame from src=%d within %d ms - retrying in %ds",
                 src, 2*HUB_JPEG_GRAB_WAIT_MS, (int)(retry/1000000));
            next_us = now + retry;
        }
    }
    return NULL;
}

/* ---- public ---- */

void timelapse_start(const ms_config *cfg)
{
    if (g_started) return;
    g_tc=cfg; ms_stopgate_init(&g_gate); g_started=1;
    if (ms_thread_create(&g_thr,MS_STACK_UTIL,tl_thread,NULL)!=0){ g_started=0; ms_stopgate_stop(&g_gate); LOGE(MOD,"thread"); return; }
    LOGI(MOD,"timelapse ready (%s, dir=%s interval=%ds)",
         cfg->timelapse.enabled?"enabled":"idle", cfg->timelapse.dir,
         cfg->timelapse.interval_s);
}

void timelapse_stop(void)
{
    if (!g_started) return;
    ms_stopgate_stop(&g_gate); pthread_join(g_thr,NULL); g_started=0;
}

#ifdef USE_CONTROL
void timelapse_get_status(ms_timelapse_status *st)
{
    if (!st) return;
    memset(st,0,sizeof *st);
    st->available=1;
    st->free_mb=-1;
    if (g_tc){
        /* F-02/F-03: timelapse.dir (string) AND enabled/interval_s (ints) are
         * runtime-mutable via /control - snapshot them together under the config
         * string lock; statfs happens outside it. */
        char dir[128];
        config_str_lock();
        st->enabled=g_tc->timelapse.enabled;
        st->interval_s=g_tc->timelapse.interval_s;
        snprintf(dir,sizeof dir,"%s",g_tc->timelapse.dir);
        config_str_unlock();
        st->free_mb=ms_free_mb(dir);
    }
    pthread_mutex_lock(&g_lock);
    st->count=g_count; st->last_t=(long long)g_last_t;
    snprintf(st->file,sizeof st->file,"%s",g_lastfile);
    pthread_mutex_unlock(&g_lock);
}
#endif

#else /* !USE_TIMELAPSE */
#include "timelapse.h"
#include <string.h>
void timelapse_start(const ms_config *cfg) { (void)cfg; }
void timelapse_stop(void) {}
#ifdef USE_CONTROL
/* available=0 tells /control (and the WebUI timelapse page) the feature is
 * not compiled in. */
void timelapse_get_status(ms_timelapse_status *st) { memset(st, 0, sizeof *st); st->free_mb = -1; }
#endif
#endif /* USE_TIMELAPSE */
