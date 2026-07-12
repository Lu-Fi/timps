/* timelapse.c - native timelapse: every timelapse.interval_s the most recent
 * JPEG frame is written to <timelapse.dir>/<host>/timelapses/<strftime>.jpg
 * (mkdir -p, tmp file + rename). Mirrors record.c: a thread gated by
 * timelapse.enabled that idles unsubscribed while off.
 *
 * JPEG source (same mapping as /snapshot.jpg?chn=N in mp4/httpd.c): prefer
 * the JPEG encoder piggybacked on timelapse.channel, fall back to the
 * dedicated jpeg.* channel, then to any stream with a piggyback encoder.
 * Subscribing keeps that JPEG encoder running (hub_active), so shots cost no
 * extra pipeline beyond the encoder that /snapshot.jpg would use anyway.
 * Retention: after each shot, *.jpg older than keep_days are pruned (emptied
 * directories are removed). */
#include "timelapse.h"
#include "hub.h"
#include "frame.h"
#include "fanqueue.h"
#include "log.h"
#include "util.h"

#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>

#define MOD "TL"
#define TL_QCAP 4

static const ms_config *g_tc;
static volatile int     g_run;
static pthread_t        g_thr;
static int              g_started;

static pthread_mutex_t  g_lock = PTHREAD_MUTEX_INITIALIZER;
/* status snapshot (under g_lock) */
static long long        g_count;
static time_t           g_last_t;
static char             g_lastfile[160];

/* ---- filesystem helpers (same shape as record.c) ---- */

static long long free_mb(const char *dir)
{
    struct statvfs vf;
    if (statvfs(dir,&vf)!=0) return -1;
    return (long long)((vf.f_bavail*(unsigned long long)vf.f_frsize)/(1024*1024));
}

/* create every parent directory of a file path (mkdir -p on dirname) */
static void mkdirs(const char *path)
{
    char tmp[512]; snprintf(tmp,sizeof tmp,"%s",path);
    char *slash=strrchr(tmp,'/'); if(!slash) return; *slash=0;
    for (char *p=tmp+1; *p; p++){
        if (*p=='/'){ *p=0; mkdir(tmp,0755); *p='/'; }
    }
    mkdir(tmp,0755);
}

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

static void prune(void)
{
    int days=g_tc->timelapse.keep_days;
    if (days<=0) return;
    /* timelapse.dir is runtime-mutable via /control: snapshot it under the
     * config string lock (never hold the lock across filesystem calls) */
    char dir[128];
    config_str_lock();
    snprintf(dir,sizeof dir,"%s",g_tc->timelapse.dir);
    config_str_unlock();
    char host[64]="camera"; gethostname(host,sizeof host);
    char base[208]; snprintf(base,sizeof base,"%s/%s/timelapses",dir,host);
    prune_old(base, time(NULL)-(time_t)days*86400, 0);
}

/* ---- JPEG source selection (mirror of httpd.c jpeg_src_from_path) ---- */

static int jpeg_src(int chn)
{
    const ms_config *c=g_tc;
    if (chn>=0 && chn<MS_MAX_VSTREAM &&
        c->video[chn].enabled && c->video[chn].jpeg_enabled)
        return HUB_JPEG_SRC_N(chn);
    if (c->jpeg.enabled) return HUB_JPEG_SRC;
    for (int i=0;i<MS_MAX_VSTREAM;i++)
        if (c->video[i].enabled && c->video[i].jpeg_enabled)
            return HUB_JPEG_SRC_N(i);
    return -1;
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
    char rel[160]; time_t t=time(NULL); struct tm tmv; localtime_r(&t,&tmv);
    if (strftime(rel,sizeof rel,name,&tmv)==0)
        snprintf(rel,sizeof rel,"%ld",(long)t);
    char host[64]="camera"; gethostname(host,sizeof host);
    char path[512], tmp[520];
    snprintf(path,sizeof path,"%s/%s/timelapses/%s.jpg",dir,host,rel);
    snprintf(tmp,sizeof tmp,"%s.tmp",path);
    mkdirs(path);
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

static void *tl_thread(void *arg)
{
    (void)arg;
    fanqueue q; int subscribed=0, sub_src=-1;
    ms_pkt *latest=NULL;
    int64_t next_us=0;

    while (g_run){
        /* read the live config every pass so channel/interval changes from
         * /control take effect without a daemon restart (like record.c) */
        int chn=g_tc->timelapse.channel; if (chn<0||chn>=MS_MAX_VSTREAM) chn=0;
        int src = g_tc->timelapse.enabled ? jpeg_src(chn) : -1;

        if (src<0){
            if (subscribed){
                hub_unsubscribe(sub_src,&q); fanqueue_free(&q);
                subscribed=0; sub_src=-1;
            }
            if (latest){ pkt_unref(latest); latest=NULL; }
            next_us=0;
            usleep(300000);
            continue;
        }
        /* source switched live -> drop the old subscription, re-open below */
        if (subscribed && src!=sub_src){
            hub_unsubscribe(sub_src,&q); fanqueue_free(&q);
            subscribed=0; sub_src=-1;
            if (latest){ pkt_unref(latest); latest=NULL; }
        }
        if (!subscribed){
            if (fanqueue_init(&q,TL_QCAP)){ usleep(300000); continue; }
            if (hub_subscribe(src,&q)!=0){ fanqueue_free(&q); usleep(500000); continue; }
            subscribed=1; sub_src=src;
            LOGI(MOD,"timelapse active (src=%d interval=%ds)",src,g_tc->timelapse.interval_s);
        }

        ms_pkt *p=fanqueue_pop(&q,200);
        if (p){
            if (p->media==MS_MEDIA_JPEG){ if (latest) pkt_unref(latest); latest=p; }
            else pkt_unref(p);
        }

        int iv=g_tc->timelapse.interval_s; if (iv<1) iv=1;
        int64_t now=ms_now_us();
        if (!next_us) next_us=now;      /* first shot as soon as a frame is in */
        if (latest && now>=next_us){
            if (shot_write(latest)==0) prune();
            next_us = now + (int64_t)iv*1000000;
        }
    }

    if (subscribed){ hub_unsubscribe(sub_src,&q); fanqueue_free(&q); }
    if (latest) pkt_unref(latest);
    return NULL;
}

/* ---- public ---- */

void timelapse_start(const ms_config *cfg)
{
    if (g_started) return;
    g_tc=cfg; g_run=1; g_started=1;
    if (pthread_create(&g_thr,NULL,tl_thread,NULL)!=0){ g_started=0; g_run=0; LOGE(MOD,"thread"); return; }
    LOGI(MOD,"timelapse ready (%s, dir=%s interval=%ds)",
         cfg->timelapse.enabled?"enabled":"idle", cfg->timelapse.dir,
         cfg->timelapse.interval_s);
}

void timelapse_stop(void)
{
    if (!g_started) return;
    g_run=0; pthread_join(g_thr,NULL); g_started=0;
}

#ifdef USE_CONTROL
void timelapse_get_status(ms_timelapse_status *st)
{
    if (!st) return;
    memset(st,0,sizeof *st);
    st->available=1;
    st->enabled=g_tc?g_tc->timelapse.enabled:0;
    st->interval_s=g_tc?g_tc->timelapse.interval_s:0;
    st->free_mb=-1;
    if (g_tc){
        /* timelapse.dir is runtime-mutable via /control: snapshot it under
         * the config string lock, statfs outside it */
        char dir[128];
        config_str_lock();
        snprintf(dir,sizeof dir,"%s",g_tc->timelapse.dir);
        config_str_unlock();
        st->free_mb=free_mb(dir);
    }
    pthread_mutex_lock(&g_lock);
    st->count=g_count; st->last_t=(long long)g_last_t;
    snprintf(st->file,sizeof st->file,"%s",g_lastfile);
    pthread_mutex_unlock(&g_lock);
}
#endif
