/* record.c - local recording to SD as fragmented-MP4 segments, raptor-RMR
 * style. Subscribes to a video stream (+AAC audio) on the hub and muxes with
 * the SAME fmp4 muxer used for /stream.mp4, writing rotating segment files
 * under <record.dir>/<hostname>/records/<strftime name>.mp4.
 *
 * Modes:
 *   continuous  - record whenever enabled.
 *   motion      - record around motion: a small ring buffer provides pre-roll
 *                 (from the keyframe before the trigger), and recording
 *                 continues until post_roll_s after the last motion.
 * A manual override (/control record start/stop, the control-bar button) wins
 * over the config mode. Segments rotate every record.segment_s at a keyframe
 * boundary; before each new segment the oldest files are pruned until at least
 * record.min_free_mb is free. */
#include "record.h"
#include "hub.h"
#include "frame.h"
#include "fanqueue.h"
#include "mp4/fmp4.h"
#include "codec/aac.h"
#include "hal/imp_motion.h"
#include "log.h"
#include "util.h"

#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>

#define MOD "REC"
#define REC_QCAP   256
#define RING_CAP   256          /* pre-roll ring: recent packets */

static const ms_config *g_rc;
static volatile int     g_run;
static pthread_t        g_thr;
static int              g_started;

static pthread_mutex_t  g_lock = PTHREAD_MUTEX_INITIALIZER;
static int              g_manual = -1;   /* -1 auto, 0 forced off, 1 forced on */
/* status snapshot (under g_lock) */
static int              g_recording;
static long long        g_curbytes;
static char             g_curfile[160];

/* ---- pre-roll ring (motion mode) ---- */
static ms_pkt *r_buf[RING_CAP];
static int     r_head, r_count;

static void ring_clear(void)
{
    for (int i=0;i<r_count;i++) pkt_unref(r_buf[(r_head+i)%RING_CAP]);
    r_head=r_count=0;
}
static void ring_push(ms_pkt *p, int64_t pre_us)
{
    if (r_count==RING_CAP){ pkt_unref(r_buf[r_head]); r_head=(r_head+1)%RING_CAP; r_count--; }
    r_buf[(r_head+r_count)%RING_CAP]=pkt_ref(p); r_count++;
    /* trim by time (keep ~pre_us), correctness of the keyframe start is handled
     * at flush time */
    while (r_count>1){
        ms_pkt *f=r_buf[r_head];
        if (p->pts_us - f->pts_us <= pre_us) break;
        pkt_unref(f); r_head=(r_head+1)%RING_CAP; r_count--;
    }
}

/* ---- filesystem helpers ---- */

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

/* recursively find the oldest regular file under base (by mtime); returns 1 and
 * fills out on success */
static int find_oldest(const char *base, char *out, size_t cap, time_t *oldest)
{
    DIR *d=opendir(base); if(!d) return 0;
    int found=0; struct dirent *e;
    while ((e=readdir(d))){
        if (!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")) continue;
        char p[336]; snprintf(p,sizeof p,"%s/%s",base,e->d_name);
        struct stat s; if (stat(p,&s)!=0) continue;
        if (S_ISDIR(s.st_mode)){
            if (find_oldest(p,out,cap,oldest)) found=1;
        } else if (S_ISREG(s.st_mode)){
            if (*oldest==0 || s.st_mtime<*oldest){ *oldest=s.st_mtime; snprintf(out,cap,"%s",p); found=1; }
        }
    }
    closedir(d);
    return found;
}

/* delete oldest segment files until at least min_free_mb is available */
static void prune_free(void)
{
    if (g_rc->record.min_free_mb<=0) return;
    char base[200]; char host[64]="camera"; gethostname(host,sizeof host);
    snprintf(base,sizeof base,"%s/%s/records",g_rc->record.dir,host);
    for (int guard=0; guard<10000; guard++){
        long long fm=free_mb(g_rc->record.dir);
        if (fm<0 || fm>=g_rc->record.min_free_mb) return;
        char victim[336]=""; time_t oldest=0;
        if (!find_oldest(base,victim,sizeof victim,&oldest) || !victim[0]) return;
        if (unlink(victim)!=0) return;
        LOGI(MOD,"pruned %s (free %lld MB < %d)",victim,fm,g_rc->record.min_free_mb);
    }
}

/* ---- segment writer ---- */

static FILE     *w_fp;
static fmp4_mux  w_mux;
static int       w_got_key;
static int64_t   w_start_us;
static int       w_chn;

static void seg_close(void);   /* fwd: seg_write closes on a write error */

static void status_set(int rec, long long bytes, const char *file)
{
    pthread_mutex_lock(&g_lock);
    g_recording=rec; g_curbytes=bytes;
    snprintf(g_curfile,sizeof g_curfile,"%.*s",(int)sizeof(g_curfile)-1,file?file:"");
    pthread_mutex_unlock(&g_lock);
}

static int seg_open(int chn)
{
    prune_free();
    char rel[160]; time_t t=time(NULL); struct tm tmv; localtime_r(&t,&tmv);
    if (strftime(rel,sizeof rel,g_rc->record.name,&tmv)==0)
        snprintf(rel,sizeof rel,"%ld",(long)t);
    char host[64]="camera"; gethostname(host,sizeof host);
    char path[512];
    snprintf(path,sizeof path,"%s/%s/records/%s.mp4",g_rc->record.dir,host,rel);
    mkdirs(path);
    w_fp=fopen(path,"wb");
    if (!w_fp){ LOGE(MOD,"open %s: %s",path,strerror(errno)); return -1; }

    fmp4_init(&w_mux);
    w_mux.has_video=1;
    w_mux.vcodec=g_rc->video[chn].codec;
    w_mux.width =g_rc->video[chn].width;
    w_mux.height=g_rc->video[chn].height;
    w_mux.fps   =g_rc->video[chn].fps;
    vparam vp;
    if (hub_get_vparam(chn,&vp) && vparam_ready(&vp)){ w_mux.vp=vp; w_mux.vp_ready=1; }
    int ac=MS_AC_NONE,asr=0,ach=0;
    if (g_rc->record.audio && hub_get_audio(&ac,&asr,&ach) && ac==MS_AC_AAC){
        w_mux.has_audio=1; w_mux.a_timescale=asr; w_mux.a_channels=ach;
        aac_asc(asr,ach,w_mux.asc);
    }
    ms_buf seg;
    if (ms_buf_init(&seg,4096)){ fclose(w_fp); w_fp=NULL; return -1; }
    fmp4_init_segment(&w_mux,&seg);
    size_t n=seg.len?fwrite(seg.data,1,seg.len,w_fp):0;
    if (seg.len && n!=seg.len){
        LOGE(MOD,"write %s: %s",path,strerror(errno));
        ms_buf_free(&seg); fclose(w_fp); w_fp=NULL; return -1;
    }
    ms_buf_free(&seg);
    w_got_key=0; w_start_us=ms_now_us(); w_chn=chn;
    status_set(1,(long long)n,path);
    LOGI(MOD,"recording -> %s",path);
    return 0;
}

static void seg_write(ms_pkt *p)
{
    if (!w_fp) return;
    ms_buf frag;
    if (ms_buf_init(&frag,p->len+256)) return;   /* OOM: drop this packet */
    if (p->media==MS_MEDIA_VIDEO){
        if (!w_got_key){ if (!p->keyframe){ ms_buf_free(&frag); return; } w_got_key=1; }
        fmp4_video_fragment(&w_mux,p->data,p->len,p->keyframe,p->pts_us,&frag);
    } else if (p->media==MS_MEDIA_AUDIO && w_mux.has_audio && w_got_key){
        fmp4_audio_fragment(&w_mux,p->data,p->len,p->pts_us,&frag);
    }
    if (frag.len){
        size_t wn=fwrite(frag.data,1,frag.len,w_fp);
        if (wn!=frag.len){
            /* SD yanked / disk full: stop the segment so status stops claiming
             * 'recording'. The writer loop reopens (fopen then fails -> retries
             * per packet) instead of silently looking healthy. */
            LOGE(MOD,"segment write failed (%s), closing",strerror(errno));
            ms_buf_free(&frag); seg_close(); return;
        }
        pthread_mutex_lock(&g_lock); g_curbytes+=frag.len; pthread_mutex_unlock(&g_lock);
    }
    ms_buf_free(&frag);
}

static void seg_close(void)
{
    if (!w_fp) return;
    fclose(w_fp); w_fp=NULL;
    LOGI(MOD,"segment closed (%lld bytes)",g_curbytes);
    status_set(0,0,"");
}

/* write the buffered pre-roll starting at the last keyframe within the window */
static void flush_ring(void)
{
    if (r_count<=0) return;
    int start=-1;
    for (int i=r_count-1;i>=0;i--){
        ms_pkt *p=r_buf[(r_head+i)%RING_CAP];
        if (p->media==MS_MEDIA_VIDEO && p->keyframe){ start=i; break; }
    }
    if (start<0) return;                 /* no keyframe buffered -> live start */
    for (int i=start;i<r_count;i++) seg_write(r_buf[(r_head+i)%RING_CAP]);
}

/* ---- decision ---- */

static int motion_recent(void)
{
#ifdef USE_CONTROL
    ms_motion_status st; motion_get_status(&st);
    if (!st.available || !st.enabled) return 0;
    return st.last_ms>=0 && st.last_ms < (long long)g_rc->record.post_roll_s*1000;
#else
    return 0;
#endif
}

static int want_run(void)
{
    int man; pthread_mutex_lock(&g_lock); man=g_manual; pthread_mutex_unlock(&g_lock);
    if (man==1) return 1;
    if (man==0) return 0;
    return g_rc->record.enabled;
}
static int want_write(void)
{
    int man; pthread_mutex_lock(&g_lock); man=g_manual; pthread_mutex_unlock(&g_lock);
    if (man==0) return 0;
    if (man==1) return 1;
    if (!g_rc->record.enabled) return 0;
    if (g_rc->record.mode==0) return 1;          /* continuous */
    return motion_recent();                        /* motion */
}

/* ---- thread ---- */

static void *rec_thread(void *arg)
{
    (void)arg;
    fanqueue q; int subscribed=0, chn=g_rc->record.channel, sub_audio=0;
    if (chn<0||chn>=MS_MAX_VSTREAM) chn=0;
    int64_t pre_us=(int64_t)g_rc->record.pre_roll_s*1000000;
    int motion_mode=(g_rc->record.mode==1);

    while (g_run){
        if (!want_run()){
            if (w_fp) seg_close();
            if (subscribed){
                hub_unsubscribe(chn,&q); if (sub_audio) hub_unsubscribe(HUB_AUDIO_SRC,&q);
                fanqueue_free(&q); subscribed=0; sub_audio=0;
            }
            ring_clear();
            usleep(300000);
            continue;
        }
        if (!subscribed){
            if (fanqueue_init(&q,REC_QCAP)){ usleep(300000); continue; }
            if (hub_subscribe(chn,&q)!=0){ fanqueue_free(&q); usleep(500000); continue; }
            int ac=MS_AC_NONE,asr=0,ach=0;
            if (g_rc->record.audio && hub_get_audio(&ac,&asr,&ach) && ac==MS_AC_AAC)
                sub_audio = (hub_subscribe(HUB_AUDIO_SRC,&q)==0);
            hub_request_idr(chn); subscribed=1;
        }

        ms_pkt *p=fanqueue_pop(&q,200);
        int writing=want_write();
        if (!p){ if (w_fp && !writing) seg_close(); continue; }
        if (fanqueue_take_dropped_key(&q)) hub_request_idr(chn);

        if (writing){
            if (!w_fp){
                if (seg_open(chn)==0 && motion_mode) flush_ring();
                ring_clear();
            }
            if (w_fp){
                /* rotate at a keyframe once the segment is long enough */
                if (g_rc->record.segment_s>0 && p->media==MS_MEDIA_VIDEO && p->keyframe &&
                    ms_now_us()-w_start_us >= (int64_t)g_rc->record.segment_s*1000000){
                    seg_close();
                    if (seg_open(chn)!=0){ pkt_unref(p); continue; }
                }
                seg_write(p);
            }
        } else {
            if (w_fp) seg_close();
            if (motion_mode) ring_push(p,pre_us);   /* buffer for pre-roll */
        }
        pkt_unref(p);
    }

    if (w_fp) seg_close();
    if (subscribed){
        hub_unsubscribe(chn,&q); if (sub_audio) hub_unsubscribe(HUB_AUDIO_SRC,&q);
        fanqueue_free(&q);
    }
    ring_clear();
    return NULL;
}

/* ---- public ---- */

void record_start(const ms_config *cfg)
{
    if (g_started) return;
    g_rc=cfg; g_run=1; g_started=1;
    if (pthread_create(&g_thr,NULL,rec_thread,NULL)!=0){ g_started=0; g_run=0; LOGE(MOD,"thread"); return; }
    LOGI(MOD,"recorder ready (mode=%s dir=%s)",
         cfg->record.mode==1?"motion":"continuous", cfg->record.dir);
}

void record_stop(void)
{
    if (!g_started) return;
    g_run=0; pthread_join(g_thr,NULL); g_started=0;
}

#ifdef USE_CONTROL
void record_get_status(ms_record_status *st)
{
    if (!st) return;
    memset(st,0,sizeof *st);
    st->available=1;
    st->enabled=g_rc?g_rc->record.enabled:0;
    st->channel=g_rc?g_rc->record.channel:0;
    st->mode=g_rc?g_rc->record.mode:0;
    st->free_mb=g_rc?free_mb(g_rc->record.dir):-1;
    pthread_mutex_lock(&g_lock);
    st->recording=g_recording; st->bytes=g_curbytes;
    snprintf(st->file,sizeof st->file,"%s",g_curfile);
    pthread_mutex_unlock(&g_lock);
}

int record_set_active(int on)
{
    pthread_mutex_lock(&g_lock);
    g_manual = on<0 ? -1 : on?1:0;
    pthread_mutex_unlock(&g_lock);
    LOGI(MOD,"manual override -> %s", on<0?"auto":on?"on":"off");
    return 0;
}
#endif
