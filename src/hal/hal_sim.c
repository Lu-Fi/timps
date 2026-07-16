/* hal_sim.c - simulation backend for host (x86) testing.
 * Feeds Annex-B video, ADTS AAC and a JPEG file into the hub, paced in real
 * time, so the RTSP/HTTP/MJPEG paths can be validated without hardware.
 * Honors on-demand activation: a source only emits while it has consumers. */
#include "hal.h"
#include "../hub.h"
#include "../config.h"
#include "../log.h"
#include "../util.h"
#include "../codec/nal.h"
#include "../codec/aac.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#define MOD "HAL_SIM"

typedef struct {
    int          src, chn, fps, codec;
    char         path[256];
    pthread_t    thr;
    volatile int run, active;
} sim_vid;

typedef struct {
    int          samplerate;
    char         path[256];
    pthread_t    thr;
    volatile int run, active;
} sim_aud;

typedef struct {
    int          fps;
    char         path[256];
    pthread_t    thr;
    volatile int run, active;
} sim_jpeg;

static sim_vid  g_vid[MS_MAX_VSTREAM];
static sim_aud  g_aud;
static sim_jpeg g_jpg;
static int      g_nvid;
static int64_t  g_epoch;

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f=fopen(path,"rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t *b=malloc(n); if(!b){fclose(f);return NULL;}
    if (fread(b,1,n,f)!=(size_t)n){ free(b); fclose(f); return NULL; }
    fclose(f); *len=n; return b;
}

static int is_vcl(int codec, const uint8_t *nal)
{
    if (codec==MS_VC_H264){ int t=h264_nal_type(nal); return t>=1&&t<=5; }
    int t=h265_nal_type(nal); return t<=31;
}
static int is_key(int codec, const uint8_t *nal)
{
    if (codec==MS_VC_H264) return h264_nal_type(nal)==5;
    int t=h265_nal_type(nal); return t>=16&&t<=23;
}

static void wait_active(volatile int *active, volatile int *run)
{
    while (*run && !*active) usleep(50000);
}

#define SIM_AU_CAP (512*1024)

static void *vid_thread(void *arg)
{
    sim_vid *v=(sim_vid*)arg;
    size_t flen; uint8_t *file=read_file(v->path,&flen);
    if (!file){ LOGE(MOD,"cannot open sim video %s",v->path); return NULL; }
    LOGI(MOD,"sim video chn%d %s (%zu bytes) %dfps (on-demand)",v->chn,v->path,flen,v->fps);
    int64_t step=1000000/(v->fps?v->fps:25);

    /* host-only test code, but a 512KB array on a pthread stack is still
     * needless risk (default pthread stacks can be smaller than the main
     * thread's, and this was re-"declared" - i.e. its space claimed - on
     * every outer while-iteration). Heap-allocate once instead, matching
     * the heap-buffer pattern already used for the real per-stream AU/JPEG
     * buffers in hal_ingenic.c. */
    uint8_t *au = (uint8_t*)malloc(SIM_AU_CAP);
    if (!au){ LOGE(MOD,"no memory for sim AU buffer"); free(file); return NULL; }

    while (v->run) {
        wait_active(&v->active,&v->run);
        if (!v->run) break;
        int64_t next=ms_now_us();
        nal_iter it; nal_unit u; nal_iter_init(&it,file,flen);
        size_t aulen=0; int have_vcl=0, key=0;
        while (v->run && v->active && nal_iter_next(&it,&u)) {
            int vcl = is_vcl(v->codec,u.data);
            if (vcl && have_vcl) {
                int64_t now=ms_now_us();
                if (next>now){ usleep(next-now); now=ms_now_us(); }
                hub_publish(v->src,au,aulen,now-g_epoch,key,MS_MEDIA_VIDEO);
                next+=step; aulen=0; have_vcl=0; key=0;
            }
            if (aulen+4+u.len < SIM_AU_CAP){
                au[aulen++]=0;au[aulen++]=0;au[aulen++]=0;au[aulen++]=1;
                memcpy(au+aulen,u.data,u.len); aulen+=u.len;
            }
            if (vcl){ have_vcl=1; if(is_key(v->codec,u.data)) key=1; }
        }
    }
    free(au);
    free(file);
    return NULL;
}

static void *aud_thread(void *arg)
{
    sim_aud *a=(sim_aud*)arg;
    size_t flen; uint8_t *file=read_file(a->path,&flen);
    if (!file){ LOGE(MOD,"cannot open sim audio %s",a->path); return NULL; }
    LOGI(MOD,"sim audio %s (%zu bytes) %dHz (on-demand)",a->path,flen,a->samplerate);
    int64_t step=1000000*1024/(a->samplerate?a->samplerate:16000);
    while (a->run) {
        wait_active(&a->active,&a->run);
        if (!a->run) break;
        int64_t next=ms_now_us(); size_t off=0;
        while (a->run && a->active && off+7<=flen) {
            if (file[off]!=0xFF||(file[off+1]&0xF0)!=0xF0){ off++; continue; }
            int fl=((file[off+3]&3)<<11)|(file[off+4]<<3)|(file[off+5]>>5);
            if (fl<7||off+fl>flen) break;
            int64_t now=ms_now_us();
            if (next>now){ usleep(next-now); now=ms_now_us(); }
            /* live mic mute (audio.mute via /control): keep pacing but do
             * not publish - mirrors the gate in hal_ingenic's audio_thread */
            if (!g_cfg.audio.mute)
                hub_publish(HUB_AUDIO_SRC,file+off,fl,now-g_epoch,0,MS_MEDIA_AUDIO);
            off+=fl; next+=step;
        }
    }
    free(file);
    return NULL;
}

static void *jpg_thread(void *arg)
{
    sim_jpeg *j=(sim_jpeg*)arg;
    size_t flen; uint8_t *file=read_file(j->path,&flen);
    if (!file){ LOGE(MOD,"cannot open sim jpeg %s",j->path); return NULL; }
    LOGI(MOD,"sim jpeg %s (%zu bytes) %dfps (on-demand)",j->path,flen,j->fps);
    int64_t step=1000000/(j->fps?j->fps:5);
    while (j->run) {
        wait_active(&j->active,&j->run);
        if (!j->run) break;
        hub_publish(HUB_JPEG_SRC,file,flen,ms_now_us()-g_epoch,1,MS_MEDIA_JPEG);
        usleep(step);
    }
    free(file);
    return NULL;
}

static int sim_init(const ms_config *cfg){ (void)cfg; return 0; }

static int sim_start(const ms_config *cfg)
{
    g_nvid=0; g_epoch=ms_now_us();
    for (int i=0;i<MS_MAX_VSTREAM;i++){
        if (!cfg->video[i].enabled) continue;
        const char *path = (i==0)?cfg->sim_video0:cfg->sim_video1;
        if (!path[0]) { LOGW(MOD,"sim.video%d not set, skipping",i); continue; }
        sim_vid *v=&g_vid[g_nvid++];
        v->src=i; v->chn=i; v->fps=cfg->video[i].fps; v->codec=cfg->video[i].codec;
        strncpy(v->path,path,sizeof v->path-1);
        hub_set_video_params(i,cfg->video[i].codec,cfg->video[i].width,
                             cfg->video[i].height,cfg->video[i].fps);
        v->run=1; v->active=0;
        pthread_create(&v->thr,NULL,vid_thread,v);
    }
    if (cfg->audio.enabled && cfg->sim_audio[0]) {
        hub_set_audio_params(cfg->audio.codec,cfg->audio.samplerate,cfg->audio.channels);
        strncpy(g_aud.path,cfg->sim_audio,sizeof g_aud.path-1);
        g_aud.samplerate=cfg->audio.samplerate; g_aud.run=1; g_aud.active=0;
        pthread_create(&g_aud.thr,NULL,aud_thread,&g_aud);
    }
    if (cfg->jpeg.enabled && cfg->sim_jpeg[0]) {
        strncpy(g_jpg.path,cfg->sim_jpeg,sizeof g_jpg.path-1);
        g_jpg.fps=cfg->jpeg.fps; g_jpg.run=1; g_jpg.active=0;
        pthread_create(&g_jpg.thr,NULL,jpg_thread,&g_jpg);
    }
    return 0;
}

static void sim_request_idr(int src){ (void)src; }

static void sim_set_active(int src, int on)
{
    LOGI(MOD,"source %d -> %s", src, on?"ACTIVE":"idle");
    if (src==HUB_AUDIO_SRC){ g_aud.active=on; return; }
    if (src==HUB_JPEG_SRC){ g_jpg.active=on; return; }
    for (int i=0;i<g_nvid;i++) if (g_vid[i].src==src){ g_vid[i].active=on; return; }
}

static void sim_stop(void)
{
    for (int i=0;i<g_nvid;i++){ g_vid[i].run=0; pthread_join(g_vid[i].thr,NULL); }
    if (g_aud.run){ g_aud.run=0; pthread_join(g_aud.thr,NULL); }
    if (g_jpg.run){ g_jpg.run=0; pthread_join(g_jpg.thr,NULL); }
}

static const hal_backend g_sim = {
    .name="sim", .init=sim_init, .start=sim_start,
    .request_idr=sim_request_idr, .set_active=sim_set_active, .stop=sim_stop
};

const hal_backend *hal_get(void){ return &g_sim; }

/* no ISP on the host: daynight uses its proc-scrape / brightness fallback */
int hal_isp_total_gain(uint32_t *gain){ (void)gain; return -1; }
int hal_isp_ae_luma(uint32_t *luma){ (void)luma; return -1; }
