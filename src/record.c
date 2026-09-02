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
 * record.min_free_mb is free.
 *
 * Only built with USE_RECORD (default on); without it the stubs at the bottom
 * keep the call sites (main.c/control.c) unconditional, exactly like srt.c. */
#ifdef USE_RECORD
#include "record.h"
#include "hub.h"
#include "frame.h"
#include "fanqueue.h"
#include "mp4/fmp4.h"
#include "codec/aac.h"
#include "rotate_caps.h"   /* ms_vstream_eff_dims (post-rotation mux dims) */
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
#include <dirent.h>
#include <fcntl.h>

#define MOD "REC"
/* fanqueue capacity (packet pointers; retained packet payloads are the real
 * cost) - a stalled recorder consumer otherwise pins every queued ms_pkt's
 * full frame alive via refcount. At roughly 2K@4-6 Mbit/s, 256 slots could
 * pin several MB per stall; 128 halves that worst case while still giving a
 * slow SD card/segment-rotation stall plenty of headroom (M10). */
#ifndef REC_QCAP
#define REC_QCAP   128
#endif
#define RING_CAP   256          /* pre-roll ring: recent packets */
/* pre-roll ring is otherwise bounded only by count (RING_CAP) and time
 * (pre_roll_s) - neither bounds BYTES. At modest fps a long pre_roll_s and a
 * high bitrate can hold far fewer than RING_CAP packets while each one is
 * large, so total memory referenced (these are pkt_ref()'d, not copied, but
 * still real bytes kept alive) is otherwise unbounded. Hard safety backstop,
 * independent of config: even the most generous realistic pre_roll_s/bitrate
 * combination fits comfortably under this. -D-overridable like the other
 * small-RAM knobs (default unchanged, L17). */
#ifndef RING_MAX_BYTES
#define RING_MAX_BYTES (4*1024*1024)
#endif

static const ms_config *g_rc;
static ms_stopgate      g_gate;   /* P-02: stop-condvar (was a volatile run flag + slice-sleep) */
static pthread_t        g_thr;
static int              g_started;

static pthread_mutex_t  g_lock = PTHREAD_MUTEX_INITIALIZER;
static int              g_manual = -1;   /* -1 auto, 0 forced off, 1 forced on */
/* status snapshot (under g_lock) */
static int              g_recording;
static long long        g_curbytes;
static char             g_curfile[160];
/* write-error latch (under g_lock) for ms_record_status - see record.h */
static long long        g_werrs;
static long long        g_werr_us;
static char             g_werr[64];

static void note_werr(const char *op, int err)
{
    pthread_mutex_lock(&g_lock);
    g_werrs++;
    g_werr_us = ms_now_us();
    snprintf(g_werr, sizeof g_werr, "%s: %s", op, strerror(err));
    pthread_mutex_unlock(&g_lock);
}

/* ---- pre-roll ring (motion mode) ---- */
static ms_pkt *r_buf[RING_CAP];
static int     r_head, r_count;
static size_t  r_bytes;                /* sum of r_buf[*]->len currently held */

static void ring_clear(void)
{
    for (int i=0;i<r_count;i++) pkt_unref(r_buf[(r_head+i)%RING_CAP]);
    r_head=r_count=0; r_bytes=0;
}
static void ring_push(ms_pkt *p, int64_t pre_us)
{
    if (r_count==RING_CAP){
        r_bytes-=r_buf[r_head]->len;
        pkt_unref(r_buf[r_head]); r_head=(r_head+1)%RING_CAP; r_count--;
    }
    r_buf[(r_head+r_count)%RING_CAP]=pkt_ref(p); r_count++; r_bytes+=p->len;
    /* trim by time (keep ~pre_us), correctness of the keyframe start is handled
     * at flush time */
    while (r_count>1){
        ms_pkt *f=r_buf[r_head];
        if (p->pts_us - f->pts_us <= pre_us) break;
        r_bytes-=f->len; pkt_unref(f); r_head=(r_head+1)%RING_CAP; r_count--;
    }
    /* hard byte backstop, independent of the time trim above (see
     * RING_MAX_BYTES) */
    while (r_count>1 && r_bytes>RING_MAX_BYTES){
        ms_pkt *f=r_buf[r_head];
        r_bytes-=f->len; pkt_unref(f); r_head=(r_head+1)%RING_CAP; r_count--;
    }
}

/* ---- filesystem helpers ----
 * ms_path_unsafe / ms_free_mb / ms_mkdirs / ms_media_path live in util.c,
 * shared with timelapse.c - the '..' check is a security check (L10/L-2) and
 * used to exist as word-identical twins here and there; see util.h for the
 * chosen component semantics. */

/* how many prune victims one tree walk collects. Once the card sits at
 * min_free_mb - the steady state of continuous recording - EVERY seg_open()
 * prunes, and the old code re-walked the whole records tree (opendir/readdir/
 * lstat over thousands of segments) once per deleted file. That walk runs
 * synchronously inside seg_open() while encoded frames pile into the record
 * fanqueue, so on a full card it costs dropped frames / a re-requested IDR
 * that every RTSP+SRT viewer pays for too, not just the recording. One walk
 * now yields this many oldest files; a rotation frees at most one segment's
 * worth of space, so 32 covers a normal cycle many times over and the
 * re-walk below is the rare fallback (e.g. after an external bulk write to
 * the card). */
#define PRUNE_BATCH 32

typedef struct { time_t mt; char path[336]; } prune_cand;

/* insertion into a bounded array kept sorted oldest-first: with PRUNE_BATCH
 * small this beats a heap in both code size and constant factor, and the
 * common case (a file NEWER than every candidate held) is a single compare. */
static void cand_add(prune_cand *c, int *n, const char *path, time_t mt)
{
    if (*n==PRUNE_BATCH && mt>=c[PRUNE_BATCH-1].mt) return;
    int i = (*n<PRUNE_BATCH) ? (*n)++ : PRUNE_BATCH-1;
    for (; i>0 && c[i-1].mt>mt; i--) c[i]=c[i-1];
    c[i].mt=mt; snprintf(c[i].path,sizeof c[i].path,"%s",path);
}

/* recursively collect the PRUNE_BATCH oldest regular files under base (by
 * mtime), and accumulate the total size of every regular file seen into
 * *bytes. lstat (not stat) so a symlink is never followed out of the records
 * tree, and depth-bounded like timelapse.c's prune_old (L8).
 *
 * The total is carried out of this walk rather than measured by a second one:
 * the reachability check in prune_free() needs the tree's size, and every
 * entry is already being lstat'd here. A separate sizing walk doubled the
 * opendir/readdir/lstat cost of every segment rotation in the steady state
 * where the card sits at min_free_mb and each rotation prunes. */
static void collect_oldest(const char *base, prune_cand *c, int *n, int depth,
                           long long *bytes)
{
    if (depth>8) return;
    DIR *d=opendir(base); if(!d) return;
    struct dirent *e;
    while ((e=readdir(d))){
        if (!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")) continue;
        char p[336]; snprintf(p,sizeof p,"%s/%s",base,e->d_name);
        struct stat s; if (lstat(p,&s)!=0) continue;
        if (S_ISDIR(s.st_mode))       collect_oldest(p,c,n,depth+1,bytes);
        else if (S_ISREG(s.st_mode)){ cand_add(c,n,p,s.st_mtime); *bytes+=s.st_size; }
    }
    closedir(d);
}

/* delete oldest segment files until at least min_free_mb is available.
 * min_free_mb comes from the caller's under-lock record snapshot (F-02), not a
 * lock-free g_rc->record read. Returns 0 if the caller may proceed to record
 * (already had enough free space, or pruning reached the target), -1 if
 * min_free_mb cannot be reached even by deleting every existing recording -
 * the caller must not record rather than empty the archive chasing a target
 * it can never hit. */
static int prune_free(int min_free_mb)
{
    if (min_free_mb<=0) return 0;
    /* record.dir is runtime-mutable via /control: snapshot it under the
     * config string lock (never hold the lock across statfs/unlink) */
    char dir[128];
    config_str_lock();
    snprintf(dir,sizeof dir,"%s",g_rc->record.dir);
    config_str_unlock();
    if (ms_path_unsafe(dir,NULL)) return -1;   /* never prune outside the tree (L10) */

    long long fm=ms_free_mb(dir);
    if (fm<0) return 0;              /* statvfs failed; behave as before (no-op) */
    if (fm>=min_free_mb) return 0;   /* already satisfied, nothing to prune */

    char base[200]; char host[64];
    ms_hostname(host,sizeof host);           /* F4 handling lives in ms_hostname */
    snprintf(base,sizeof base,"%s/%s/records",dir,host);

    static int warned;

    /* ~11 KB kept off the record thread's stack: only rec_thread ever reaches
     * prune_free (via seg_open), so a function-local static needs no locking -
     * same reasoning as seg_write's persistent fragment buffer. */
    static prune_cand cand[PRUNE_BATCH];
    for (int walk=0; walk<32; walk++){
        fm=ms_free_mb(dir);
        if (fm<0 || fm>=min_free_mb) return 0;
        int n=0; long long tree_bytes=0;
        collect_oldest(base,cand,&n,0,&tree_bytes);

        /* Reachability check BEFORE deleting anything: if freeing every byte
         * of every existing recording still would not reach min_free_mb, no
         * amount of pruning gets there - the old code kept deleting anyway,
         * one PRUNE_BATCH walk at a time, until the archive was empty and it
         * STILL hadn't reached an unreachable target. Refuse instead, once per
         * bad-value edge so it does not spam every segment rotation.
         * First walk only: later walks have already deleted files, so their
         * (smaller) tree total would refuse mid-prune. */
        if (walk==0){
            long long max_recoverable = fm + tree_bytes/(1024*1024);
            if (max_recoverable < min_free_mb){
                if (!warned){
                    LOGW(MOD,"record.min_free_mb=%d unreachable (only %lldMB free, "
                             "%lldMB even if every existing recording were deleted) - "
                             "refusing to record rather than empty the archive",
                         min_free_mb,fm,max_recoverable);
                    warned=1;
                }
                /* Surface the refusal the same way a write failure would
                 * (write_errors/last_error in record_get_status()) - otherwise a
                 * client that just asked for record.active=1 sees the request
                 * accepted, "recording" never turns true, and nothing explains why. */
                pthread_mutex_lock(&g_lock);
                g_werrs++;
                g_werr_us = ms_now_us();
                snprintf(g_werr,sizeof g_werr,"min_free_mb=%d unreachable (max %lldMB)",
                         min_free_mb,max_recoverable);
                pthread_mutex_unlock(&g_lock);
                return -1;
            }
            warned=0;
        }

        if (!n) return 0;                    /* nothing left to prune */
        for (int i=0;i<n;i++){
            if (unlink(cand[i].path)!=0) return 0;
            LOGI(MOD,"pruned %s (free %lld MB < %d)",cand[i].path,fm,min_free_mb);
            fm=ms_free_mb(dir);
            if (fm<0 || fm>=min_free_mb) return 0;
        }
    }
    return 0;
}

/* ---- segment writer ---- */

static FILE     *w_fp;
static fmp4_mux  w_mux;
static int       w_got_key;
static int64_t   w_start_us;
static int       w_chn;
static int64_t   w_sync_us;    /* last fflush+fsync (M7 periodic durability) */

/* how often the open segment is flushed+fsync'd to media; without this a
 * power cut loses up to segment_s of recording from the page cache (M7) */
#ifndef REC_SYNC_US
#define REC_SYNC_US (5*1000000LL)
#endif

static void seg_close(void);   /* fwd: seg_write closes on a write error */

static void status_set(int rec, long long bytes, const char *file)
{
    pthread_mutex_lock(&g_lock);
    g_recording=rec; g_curbytes=bytes;
    snprintf(g_curfile,sizeof g_curfile,"%.*s",(int)sizeof(g_curfile)-1,file?file:"");
    pthread_mutex_unlock(&g_lock);
}

static int seg_open(int chn, const ms_record_cfg *rc)
{
    if (prune_free(rc->min_free_mb)!=0) return -1;   /* target unreachable; see prune_free() */
    /* record.dir/name are runtime-mutable via /control: snapshot them under
     * the config string lock before strftime/path building */
    char dir[128], name[96];
    config_str_lock();
    snprintf(dir,sizeof dir,"%s",g_rc->record.dir);
    snprintf(name,sizeof name,"%s",g_rc->record.name);
    config_str_unlock();
    if (ms_path_unsafe(dir,name)){
        LOGE(MOD,"unsafe record.dir/name ('..' or absolute name), not recording");
        return -1;
    }
    /* O_EXCL, not fopen("wb"): the segment name is only as unique as the
     * strftime pattern's own granularity (default: one second). A stop/start
     * inside the same window - motion re-triggering right after post_roll
     * expiry is the realistic one - or a backwards clock step onto an
     * already-recorded timestamp would otherwise silently truncate a COMPLETE
     * earlier segment. Failing instead would be wrong too: a clock step back
     * over an hour of existing files would stall recording for that whole
     * hour (the caller just retries seg_open on the next packet). So uniquify
     * with a -N suffix - nothing already on the card is ever destroyed, and
     * the retry path isn't entered at all. Same rigor as the rest of this
     * function: every failure path below unlinks `path`, which now always
     * names the file actually opened. */
    char stem[400], path[512];
    ms_media_path(stem,sizeof stem,dir,"records",name,"");
    snprintf(path,sizeof path,"%s.mp4",stem);
    ms_mkdirs(path);
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0644);
    for (int i=1; fd<0 && errno==EEXIST && i<1000; i++){
        snprintf(path,sizeof path,"%s-%d.mp4",stem,i);
        fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0644);
        if (fd>=0) LOGW(MOD,"segment name collision, wrote %s instead",path);
    }
    if (fd<0){ LOGE(MOD,"open %s: %s",path,strerror(errno)); return -1; }
    w_fp=fdopen(fd,"wb");
    if (!w_fp){
        LOGE(MOD,"fdopen %s: %s",path,strerror(errno));
        close(fd); unlink(path);         /* don't leave an empty stub (L6) */
        return -1;
    }

    fmp4_init(&w_mux);
    w_mux.has_video=1;
    /* codec/geometry/fps are restart-only: mux the segment as the RUNNING
     * encoder actually produces it (boot snapshot), not a live-edited g_cfg -
     * a mismatch yields a container that misdescribes its own samples. config.h */
    w_mux.vcodec=g_cfg_boot.video[chn].codec;
    /* ACTUAL running dims (hub_get_video_params reflects any T23 SW-rotate /
     * T31 FS-rotate safe-envelope refusal - see hub.h); fall back to the raw
     * boot-config computation only if the HAL hasn't populated the hub yet
     * (shouldn't happen here in practice - recording starts well after HAL
     * start - but zero/garbage dims would corrupt the mp4 track anyway). */
    int ew, eh;
    if (!hub_get_video_params(chn, NULL, &ew, &eh, NULL))
        ms_vstream_eff_dims(&g_cfg_boot.video[chn], &ew, &eh);
    w_mux.width =ew;
    w_mux.height=eh;
    w_mux.fps   =g_cfg_boot.video[chn].fps;
    vparam vp;
    if (hub_get_vparam(chn,&vp) && vparam_ready(&vp)){ w_mux.vp=vp; w_mux.vp_ready=1; }
    int ac=MS_AC_NONE,asr=0,ach=0;
    if (rc->audio && hub_get_audio(&ac,&asr,&ach) && ac==MS_AC_AAC){
        w_mux.has_audio=1; w_mux.a_timescale=asr; w_mux.a_channels=ach;
        aac_asc(asr,ach,w_mux.asc);
    }
    ms_buf seg;
    if (ms_buf_init(&seg,4096)){ fclose(w_fp); w_fp=NULL; unlink(path); return -1; }
    /* fails when the track isn't warmed up yet (no vparam from a keyframe
     * through the hub, e.g. right at cold start / a live channel switch) or
     * on OOM mid-build. Writing anyway would produce a moov-less segment
     * file that no player can open - bail and let the writer loop above
     * retry seg_open() on the next packet instead. */
    if (fmp4_init_segment(&w_mux,&seg)!=0){
        ms_buf_free(&seg); fclose(w_fp); w_fp=NULL;
        unlink(path);
        return -1;
    }
    size_t n=seg.len?fwrite(seg.data,1,seg.len,w_fp):0;
    if (seg.len && n!=seg.len){
        LOGE(MOD,"write %s: %s",path,strerror(errno));
        ms_buf_free(&seg); fclose(w_fp); w_fp=NULL;
        unlink(path);                    /* don't leave a moov-less stub (L6) */
        return -1;
    }
    ms_buf_free(&seg);
    w_got_key=0; w_start_us=ms_now_us(); w_chn=chn; w_sync_us=w_start_us;
    status_set(1,(long long)n,path);
    LOGI(MOD,"recording -> %s",path);
    return 0;
}

static void seg_write(ms_pkt *p)
{
    if (!w_fp) return;
    /* persistent per-recorder fragment buffer (M1): reset to len=0/err=0
     * each packet instead of ms_buf_init()/ms_buf_free() per packet. Only
     * rec_thread ever calls seg_write, so a function-local static is safe
     * without locking; it grows once to the steady-state fragment size via
     * ms_buf_reserve and is then reused for the life of the process. */
    static ms_buf frag;
    ms_buf_reset(&frag, 256*1024);   /* reuse, shrink an outlier IDR buffer back */
    int frag_ok=1;
    if (p->media==MS_MEDIA_VIDEO){
        if (!w_got_key){ if (!p->keyframe){ return; } w_got_key=1; }
        frag_ok = fmp4_video_fragment(&w_mux,p->data,p->len,p->keyframe,p->pts_us,&frag)==0;
    } else if (p->media==MS_MEDIA_AUDIO && w_mux.has_audio && w_got_key){
        frag_ok = fmp4_audio_fragment(&w_mux,p->data,p->len,p->pts_us,&frag)==0;
    }
    /* a failed fragment (OOM mid-build) can hold partial, non-box-tree bytes
     * - writing those to the file would corrupt the rest of this segment,
     * same reasoning as the /stream.mp4 path in mp4/httpd.c. Drop just this
     * packet instead; the next successful fragment keeps the file going. */
    if (!frag_ok){
        LOGW(MOD,"dropped a corrupt %s fragment while recording (OOM?)",
             p->media==MS_MEDIA_VIDEO?"video":"audio");
        return;
    }
    if (frag.len){
        size_t wn=fwrite(frag.data,1,frag.len,w_fp);
        if (wn!=frag.len){
            /* SD yanked / disk full: stop the segment so status stops claiming
             * 'recording'. The writer loop reopens (fopen then fails -> retries
             * per packet) instead of silently looking healthy. */
            int e=errno;   /* before LOGE, which may clobber errno */
            LOGE(MOD,"segment write failed (%s), closing",strerror(e));
            note_werr("write", e);
            seg_close(); return;
        }
        pthread_mutex_lock(&g_lock); g_curbytes+=frag.len; pthread_mutex_unlock(&g_lock);
        /* periodic durability (M7) without stalling the record thread (M-3):
         * fflush hands libc's buffer to the kernel (a real short-write error
         * still surfaces here and closes the segment), then kick ASYNC writeback
         * with sync_file_range() instead of a blocking fsync() - a slow SD card
         * can no longer park this thread for seconds and drop frames. The
         * durable fsync happens at seg_close(). */
        int64_t now=ms_now_us();
        if (now - w_sync_us >= REC_SYNC_US){
            if (fflush(w_fp)!=0){
                int e=errno;
                LOGE(MOD,"segment flush failed (%s), closing",strerror(e));
                note_werr("flush", e);
                seg_close(); return;
            }
            sync_file_range(fileno(w_fp), 0, 0, SYNC_FILE_RANGE_WRITE);
            w_sync_us=now;
        }
    }
}

static void seg_close(void)
{
    if (!w_fp) return;
    FILE *fp=w_fp; w_fp=NULL;
    /* fclose/fsync results matter (M7): buffered data is only handed to the
     * kernel at fclose, and only reaches media after fsync - ignoring both
     * silently truncated the tail of the segment on error/power cut. */
    int err=0;
    if (fflush(fp)!=0) err=errno;
    else if (fsync(fileno(fp))!=0) err=errno;
    if (fclose(fp)!=0 && !err) err=errno;
    if (err){
        LOGE(MOD,"segment close/sync failed: %s (tail may be truncated)",strerror(err));
        note_werr("close/sync", err);
    }
    LOGI(MOD,"segment closed (%lld bytes)",g_curbytes);
    status_set(0,0,"");
}

/* Write the buffered pre-roll, starting at the OLDEST keyframe still in the
 * ring - a decodable start as far back as pre_roll_s allows. Scanning newest
 * first instead would pick the most recent keyframe and cap every pre-roll at
 * roughly one GOP no matter what pre_roll_s says, which is not what the ring
 * is trimmed to hold (ring_push) nor what the cap warning in rec_thread
 * measures. */
static void flush_ring(void)
{
    if (r_count<=0) return;
    int start=-1;
    for (int i=0;i<r_count;i++){
        ms_pkt *p=r_buf[(r_head+i)%RING_CAP];
        if (p->media==MS_MEDIA_VIDEO && p->keyframe){ start=i; break; }
    }
    if (start<0) return;                 /* no keyframe buffered -> live start */
    for (int i=start;i<r_count;i++) seg_write(r_buf[(r_head+i)%RING_CAP]);
}

/* ---- decision ---- */

/* rc is the caller's per-pass under-lock record snapshot (F-02). */
static int motion_recent(const ms_record_cfg *rc)
{
#ifdef USE_CONTROL
    ms_motion_status st; motion_get_status(&st);
    if (!st.available || !st.enabled) return 0;
    return st.last_ms>=0 && st.last_ms < (long long)rc->post_roll_s*1000;
#else
    (void)rc;
    return 0;
#endif
}

static int want_run(const ms_record_cfg *rc)
{
    int man; pthread_mutex_lock(&g_lock); man=g_manual; pthread_mutex_unlock(&g_lock);
    if (man==1) return 1;
    if (man==0) return 0;
    return rc->enabled;
}
static int want_write(const ms_record_cfg *rc)
{
    int man; pthread_mutex_lock(&g_lock); man=g_manual; pthread_mutex_unlock(&g_lock);
    if (man==0) return 0;
    if (man==1) return 1;
    if (!rc->enabled) return 0;
    if (rc->mode==0) return 1;                     /* continuous */
    return motion_recent(rc);                       /* motion */
}

/* ---- thread ---- */

static void *rec_thread(void *arg)
{
    (void)arg;
    fanqueue q; int subscribed=0, sub_audio=0, sub_chn=-1;
    int64_t drop_idr_us=0;   /* rate-limit the safety IDR request on any drop */

    while (!ms_stopgate_stopped(&g_gate)){
        /* read the live config every pass so channel / mode / pre-roll changes
         * from /control take effect WITHOUT a daemon restart (the thread used to
         * freeze these at start, so picking "Substream" or switching mode in the
         * WebUI silently kept the boot-time values).
         * F-02: snapshot the whole record section ONCE per pass under the config
         * string lock (the daynight.c pattern) and work from the local copy, so
         * the /control writer never races these int/enum reads (the strings are
         * still re-snapshotted at their own point of use in seg_open/prune_free).
         * The lock is held only for the struct copy, never across HAL/IO. */
        ms_record_cfg rc;
        config_str_lock();
        rc = g_rc->record;
        config_str_unlock();
        int chn=rc.channel; if (chn<0||chn>=MS_MAX_VSTREAM) chn=0;
        int motion_mode=(rc.mode==1);
        int64_t pre_us=(int64_t)rc.pre_roll_s*1000000;

        /* config.c clamps record.pre_roll_s to 0..60s, but the pre-roll ring is
         * hard-capped at RING_CAP packets AND RING_MAX_BYTES bytes (a deliberate
         * embedded memory budget, NOT resized here). At a real encode bitrate
         * the byte cap is the binding limit - e.g. ~11s at 3 Mbit/s - so a large
         * configured pre_roll_s silently under-delivers. Warn once (per process)
         * when the configured value cannot actually be held at THIS stream's
         * bitrate/fps, so an operator gets a clear signal instead of silent
         * truncation. Derivation: byte cap seconds = RING_MAX_BYTES*8 / bitrate;
         * packet cap seconds = RING_CAP / fps; the ring holds the smaller. */
        if (motion_mode && rc.pre_roll_s>0){
            static int preroll_warned;
            if (!preroll_warned){
                /* bitrate/fps are restart-only (VID_REST): read the boot snapshot
                 * so the warning reflects what the encoder actually produces. */
                int kbps=g_cfg_boot.video[chn].bitrate_kbps, fps=g_cfg_boot.video[chn].fps;
                double cap_s=1e9;                       /* unknown bitrate -> no cap */
                if (kbps>0) cap_s=(double)RING_MAX_BYTES*8.0/((double)kbps*1000.0);
                if (fps>0){ double ps=(double)RING_CAP/(double)fps; if (ps<cap_s) cap_s=ps; }
                if ((double)rc.pre_roll_s > cap_s+0.5){
                    preroll_warned=1;
                    LOGW(MOD,"record.pre_roll_s=%d cannot be held: the pre-roll "
                             "ring caps at ~%.0fs for ch%d (%d kbps, %d fps, "
                             "%d packets / %d MB max) - actual pre-roll is shorter",
                         rc.pre_roll_s, cap_s, chn, kbps, fps,
                         RING_CAP, RING_MAX_BYTES/(1024*1024));
                }
            }
        }

        if (!want_run(&rc)){
            if (w_fp) seg_close();
            if (subscribed){
                hub_unsubscribe(sub_chn,&q); if (sub_audio) hub_unsubscribe(HUB_AUDIO_SRC,&q);
                fanqueue_free(&q); subscribed=0; sub_audio=0; sub_chn=-1;
            }
            ring_clear();
            /* P-02: idle (recording disabled) - one stop-aware wait instead
             * of a 300 ms poll. 1 s poll cadence (was ~3.3 wakeups/s) only
             * bounds how fast a /control ENABLE (or manual trigger) is noticed
             * from the fully-idle state, which has no pre-roll to lose anyway;
             * shutdown is still immediate (stop wakes the wait at once). */
            if (ms_stopgate_wait(&g_gate, 1000)) break;
            continue;
        }
        /* channel switched live -> drop the old subscription, re-open below */
        if (subscribed && chn!=sub_chn){
            if (w_fp) seg_close();
            hub_unsubscribe(sub_chn,&q); if (sub_audio) hub_unsubscribe(HUB_AUDIO_SRC,&q);
            fanqueue_free(&q); subscribed=0; sub_audio=0; sub_chn=-1;
            ring_clear();
        }
        if (!subscribed){
            if (fanqueue_init(&q,REC_QCAP)){ if (ms_stopgate_wait(&g_gate,300)) break; continue; }
            if (hub_subscribe(chn,&q)!=0){ fanqueue_free(&q); if (ms_stopgate_wait(&g_gate,500)) break; continue; }
            int ac=MS_AC_NONE,asr=0,ach=0;
            int have_a = hub_get_audio(&ac,&asr,&ach);
            if (rc.audio && have_a && ac==MS_AC_AAC)
                sub_audio = (hub_subscribe(HUB_AUDIO_SRC,&q)==0);
            else if (rc.audio){
                /* B5: the fMP4 muxer only carries AAC. With G.711 or audio off
                 * (USE_FAAC=0 falls back to G.711) recordings come out video-
                 * only - say so once instead of silently dropping the track. */
                static int aac_warned;
                if (!aac_warned){
                    aac_warned=1;
                    LOGW(MOD,"record.audio=1 but audio codec is %s - recordings "
                             "are video-only; AAC (build with USE_FAAC=1) required",
                         ac==MS_AC_PCMU?"g711u":ac==MS_AC_PCMA?"g711a":
                         have_a?"unknown":"none/disabled");
                }
            }
            hub_request_idr(chn); subscribed=1; sub_chn=chn;
        }

        /* record.audio is re-read live like channel/mode/pre_roll above:
         * seg_open() already re-reads it per segment and declares (or omits)
         * the AAC track accordingly, but the audio hub subscription was only
         * decided once in the !subscribed block. A live 0->1 toggle therefore
         * left new segments declaring an AAC track that received no samples
         * (broken/empty audio track); a 1->0 toggle leaked the subscription.
         * Reconcile the subscription with the live flag every pass so both
         * transitions take effect without a re-subscribe. The sub_audio guard
         * makes repeated toggling idempotent (no double-subscribe / no leak);
         * the AAC gate matches seg_open() so a declared track is always the
         * one we are actually subscribed to. */
        if (subscribed){
            int want_audio = rc.audio;
            if (want_audio && !sub_audio){
                int ac=MS_AC_NONE,asr=0,ach=0;
                if (hub_get_audio(&ac,&asr,&ach) && ac==MS_AC_AAC)
                    sub_audio = (hub_subscribe(HUB_AUDIO_SRC,&q)==0);
            } else if (!want_audio && sub_audio){
                hub_unsubscribe(HUB_AUDIO_SRC,&q); sub_audio=0;
            }
        }

        ms_pkt *p=fanqueue_pop(&q,200);
        int writing=want_write(&rc);
        if (!p){ if (w_fp && !writing) seg_close(); continue; }
        /* a dropped keyframe corrupts the GOP outright; a dropped P-frame is
         * silent but breaks it just the same for anyone decoding this
         * recording later. Self-heal both, P-frame drops rate-limited to
         * once/sec since the IDR request is global to the shared encoder
         * (see src/rtsp/rtsp.c for the same pattern and its rationale). */
        if (fanqueue_take_dropped_key(&q)) {
            hub_note_drop(chn);   /* /control "queue_drops" */
            LOGD(MOD,"chn=%d: overflow dropped a keyframe - IDR re-requested",chn);
            hub_request_idr(chn);
            drop_idr_us = ms_now_us();
        } else if (fanqueue_take_dropped(&q)) {
            hub_note_drop(chn);
            int64_t now = ms_now_us();
            if (now - drop_idr_us > 1000000) {
                LOGD(MOD,"chn=%d: overflow dropped P-frame(s) - IDR re-requested",chn);
                hub_request_idr(chn);
                drop_idr_us = now;
            }
        }

        if (writing){
            if (!w_fp){
                /* only drop the pre-roll once recording actually started; a
                 * transient seg_open failure keeps the ring for the retry */
                if (seg_open(chn,&rc)==0){ if (motion_mode) flush_ring(); ring_clear(); }
            }
            if (w_fp){
                /* rotate at a keyframe once the segment is long enough */
                if (rc.segment_s>0 && p->media==MS_MEDIA_VIDEO && p->keyframe &&
                    ms_now_us()-w_start_us >= (int64_t)rc.segment_s*1000000){
                    seg_close();
                    if (seg_open(chn,&rc)!=0){ pkt_unref(p); continue; }
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
        hub_unsubscribe(sub_chn,&q); if (sub_audio) hub_unsubscribe(HUB_AUDIO_SRC,&q);
        fanqueue_free(&q);
    }
    ring_clear();
    return NULL;
}

/* ---- public ---- */

void record_start(const ms_config *cfg)
{
    if (g_started) return;
    g_rc=cfg; ms_stopgate_init(&g_gate); g_started=1;
    if (ms_thread_create(&g_thr,MS_STACK_STREAM,rec_thread,NULL)!=0){ g_started=0; ms_stopgate_stop(&g_gate); LOGE(MOD,"thread"); return; }
    LOGI(MOD,"recorder ready (mode=%s dir=%s)",
         cfg->record.mode==1?"motion":"continuous", cfg->record.dir);
}

void record_stop(void)
{
    if (!g_started) return;
    ms_stopgate_stop(&g_gate); pthread_join(g_thr,NULL); g_started=0;
}

#ifdef USE_CONTROL
void record_get_status(ms_record_status *st)
{
    if (!st) return;
    memset(st,0,sizeof *st);
    st->available=1;
    st->free_mb=-1;
    if (g_rc){
        /* F-02/F-03: record.dir (string) AND the enabled/channel/mode ints are
         * runtime-mutable via /control - snapshot them together under the config
         * string lock; statfs happens outside it. */
        char dir[128];
        config_str_lock();
        st->enabled=g_rc->record.enabled;
        st->channel=g_rc->record.channel;
        st->mode=g_rc->record.mode;
        snprintf(dir,sizeof dir,"%s",g_rc->record.dir);
        config_str_unlock();
        st->free_mb=ms_free_mb(dir);
    }
    pthread_mutex_lock(&g_lock);
    st->recording=g_recording; st->bytes=g_curbytes;
    st->manual_off = (g_manual==0);
    snprintf(st->file,sizeof st->file,"%s",g_curfile);
    st->write_errors=g_werrs; st->last_error_us=g_werr_us;
    snprintf(st->last_error,sizeof st->last_error,"%s",g_werr);
    pthread_mutex_unlock(&g_lock);
    if (st->mode==1){
        ms_motion_status mst; motion_get_status(&mst);
        st->motion_gate_available = mst.available;
        st->motion_gate_enabled = mst.enabled;
    }
}

int record_set_active(int on)
{
    pthread_mutex_lock(&g_lock);
    g_manual = on<0 ? -1 : on?1:0;
    pthread_mutex_unlock(&g_lock);
    LOGI(MOD,"manual override -> %s", on<0?"auto":on?"on":"off");
    return 0;
}

/* On-demand clip for send2 (Telegram/motion video). Self-contained: its own hub
 * subscription + fmp4 muxer, independent of the rotating SD recorder above, so
 * it works whether or not record.enabled is set (subscribing wakes the encoder
 * on demand). Captures forward from the next keyframe for `seconds` into a
 * finalized-enough fMP4 (same format the SD segments use). Blocks. Path is
 * restricted to /tmp/ (send2 uses mktemp there) so a /control caller can't
 * overwrite arbitrary files. */
int record_clip(const char *path, int seconds)
{
    /* /tmp/ only AND no ".." component: strncmp alone lets "/tmp/../etc/x"
     * through, which ms_mkdirs()+open() would then overwrite. This used to be
     * strstr(path,"..") - a SUBSTRING test that silently diverged from the
     * component semantics this comment already promised. ms_has_dotdot blocks
     * every traversal the substring test blocked (only a whole ".." component
     * ever walks upward), and now all '..' checks share one meaning. */
    if (!path || strncmp(path,"/tmp/",5)!=0 || ms_has_dotdot(path) || !g_rc) return -1;
    if (seconds<=0) seconds=6;
    if (seconds>30) seconds=30;
    /* F-02: snapshot the live record ints (channel/audio) under the lock; this
     * runs on the /control HTTP thread, racing the /control writer otherwise. */
    int rc_channel, rc_audio;
    config_str_lock();
    rc_channel=g_rc->record.channel;
    rc_audio=g_rc->record.audio;
    config_str_unlock();
    int chn=rc_channel; if (chn<0||chn>=MS_MAX_VSTREAM) chn=0;

    /* one clip at a time: capture runs on the /control HTTP thread and pins an
     * encoder + fanqueue + a tmpfs (RAM) file for its whole duration. Without a
     * cap, rapid motion events park every HTTP worker and stack parallel clips
     * in RAM. Extra requests are dropped (return -1). */
    static pthread_mutex_t clip_lock = PTHREAD_MUTEX_INITIALIZER;
    if (pthread_mutex_trylock(&clip_lock)!=0){ LOGW(MOD,"clip busy, skipped %s",path); return -1; }

    fanqueue q; int have_q=0, sub_v=0, sub_audio=0;
    ms_buf frag;  int have_frag=0;
    fmp4_mux mux; FILE *fp=NULL; int rc=-1;
    int ac=MS_AC_NONE,asr=0,ach=0;
    int64_t deadline=0, giveup=ms_now_us()+(int64_t)(seconds+5)*1000000;
    int64_t drop_idr_us=0;   /* rate-limit the safety IDR request on any drop */

    if (fanqueue_init(&q,REC_QCAP)) goto out; have_q=1;
    if (hub_subscribe(chn,&q)!=0) goto out; sub_v=1;
    if (rc_audio && hub_get_audio(&ac,&asr,&ach) && ac==MS_AC_AAC)
        sub_audio=(hub_subscribe(HUB_AUDIO_SRC,&q)==0);
    hub_request_idr(chn);
    if (ms_buf_init(&frag,4096)) goto out; have_frag=1;

    while (!ms_stopgate_stopped(&g_gate)){
        int64_t now=ms_now_us();
        if (!fp && now>=giveup) break;           /* no start point ever arrived */
        ms_pkt *p=fanqueue_pop(&q,200);
        if (!p){ if (fp && now>=deadline) break; continue; }
        /* see the matching comment in rec_thread() above / src/rtsp/rtsp.c:
         * self-heal a dropped P-frame too, rate-limited (global IDR request). */
        if (fanqueue_take_dropped_key(&q)) {
            hub_request_idr(chn);
            drop_idr_us = now;
        } else if (fanqueue_take_dropped(&q)) {
            if (now - drop_idr_us > 1000000) {
                hub_request_idr(chn);
                drop_idr_us = now;
            }
        }

        if (!fp){
            /* open on the first keyframe with a ready parameter set */
            if (!(p->media==MS_MEDIA_VIDEO && p->keyframe)){ pkt_unref(p); continue; }
            vparam vp;
            if (!(hub_get_vparam(chn,&vp) && vparam_ready(&vp))){ pkt_unref(p); continue; }
            ms_mkdirs(path);
            /* O_EXCL|O_NOFOLLOW: never follow a pre-planted symlink or clobber an
             * existing file (send2 hands us a fresh mktemp -u name). */
            int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
            if (fd<0){ LOGW(MOD,"clip open %s: %s",path,strerror(errno)); pkt_unref(p); break; }
            fp=fdopen(fd,"wb");
            if (!fp){ close(fd); unlink(path); pkt_unref(p); break; }
            fmp4_init(&mux);
            /* restart-only codec/geometry/fps -> boot snapshot (see config.h) */
            mux.has_video=1; mux.vcodec=g_cfg_boot.video[chn].codec;
            /* ACTUAL running dims - see the seg_open() call site above for why
             * hub_get_video_params() takes priority over the raw computation. */
            int ew, eh;
            if (!hub_get_video_params(chn, NULL, &ew, &eh, NULL))
                ms_vstream_eff_dims(&g_cfg_boot.video[chn], &ew, &eh);
            mux.width=ew; mux.height=eh;
            mux.fps=g_cfg_boot.video[chn].fps;
            mux.vp=vp; mux.vp_ready=1;
            if (sub_audio){ mux.has_audio=1; mux.a_timescale=asr; mux.a_channels=ach;
                            aac_asc(asr,ach,mux.asc); }
            frag.len=0; frag.err=0;
            if (fmp4_init_segment(&mux,&frag)!=0 ||
                (frag.len && fwrite(frag.data,1,frag.len,fp)!=frag.len)){
                fclose(fp); fp=NULL; unlink(path); pkt_unref(p); break;
            }
            deadline=now+(int64_t)seconds*1000000;
        }

        frag.len=0; frag.err=0;
        int wrote=0;
        if (p->media==MS_MEDIA_VIDEO)
            wrote=(fmp4_video_fragment(&mux,p->data,p->len,p->keyframe,p->pts_us,&frag)==0);
        else if (p->media==MS_MEDIA_AUDIO && mux.has_audio)
            wrote=(fmp4_audio_fragment(&mux,p->data,p->len,p->pts_us,&frag)==0);
        if (wrote && frag.len && fwrite(frag.data,1,frag.len,fp)!=frag.len){
            LOGE(MOD,"clip write failed (%s), dropping",strerror(errno));
            fclose(fp); fp=NULL; unlink(path); pkt_unref(p); break;
        }
        pkt_unref(p);
        if (now>=deadline) break;
    }

    if (fp){
        /* fclose flushes the last buffered fragment - if THAT write fails the
         * clip is truncated and must not be reported as success (L5) */
        if (fclose(fp)!=0){
            LOGE(MOD,"clip close %s: %s",path,strerror(errno));
            unlink(path);
        } else { rc=0; LOGI(MOD,"clip -> %s (%ds)",path,seconds); }
    }
    else LOGW(MOD,"clip: no frames for %s",path);

out:
    if (have_frag) ms_buf_free(&frag);
    if (sub_v) hub_unsubscribe(chn,&q);
    if (sub_audio) hub_unsubscribe(HUB_AUDIO_SRC,&q);
    if (have_q) fanqueue_free(&q);
    pthread_mutex_unlock(&clip_lock);
    return rc;
}
#endif

#else /* !USE_RECORD */
#include "record.h"
#include <string.h>
void record_start(const ms_config *cfg) { (void)cfg; }
void record_stop(void) {}
#ifdef USE_CONTROL
/* available=0 tells /control (and the WebUI record page) the feature is not
 * compiled in; the manual override and clip capture report failure. */
void record_get_status(ms_record_status *st) { memset(st, 0, sizeof *st); st->free_mb = -1; }
int  record_set_active(int on) { (void)on; return -1; }
int  record_clip(const char *path, int seconds) { (void)path; (void)seconds; return -1; }
#endif
#endif /* USE_RECORD */
