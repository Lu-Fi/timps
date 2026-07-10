/* hal_ingenic.c - real Ingenic SoC backend (T10/T20/T21/T23/T30/T31/T40/T41/C100).
 * Depends only on the vendor libimp (+pthread). Video via IMP_ISP/FrameSource/
 * Encoder, audio via IMP_AI (+ own G.711 or IMP_AENC AAC), OSD via IMP_OSD,
 * motion via IMP_IVS. Compiled only with -DHAL_INGENIC against the SDK headers.
 *
 * The control flow mirrors the proven prudynt-t pipeline but is stripped down
 * to the essentials for minimal footprint. */
#include "hal.h"
#ifdef HAL_INGENIC
#include "../hub.h"
#include "../log.h"
#include "../util.h"
#include "../codec/nal.h"
#include "../codec/g711.h"
#include "imp_osd.h"
#include "imp_motion.h"

#include <imp/imp_system.h>
#include <imp/imp_isp.h>
#include <imp/imp_framesource.h>
#include <imp/imp_encoder.h>
#include <imp/imp_audio.h>
#include <imp/imp_osd.h>
#ifdef USE_FAAC
#include <faac.h>
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#define MOD "HAL_ING"

#if defined(PLATFORM_T31)||defined(PLATFORM_C100)||defined(PLATFORM_T40)||defined(PLATFORM_T41)
#define ENC_NEW_API 1
#endif

/* Debounce for on-demand StartRecvPic/StopRecvPic: with several clients that
 * connect/disconnect rapidly, toggling the encoder in quick succession can
 * destabilize the IMP driver (observed kernel crashes with multiple ffplay
 * clients). Stop only after the source has had no consumer for this long. */
#ifndef MS_IDLE_STOP_US
#define MS_IDLE_STOP_US 2000000   /* 2 s */
#endif
/* AU assembly buffer bounds (was a fixed 1 MB static per video thread; now
 * sized from the stream resolution to fit small-RAM SoCs like the T10) */
#ifndef MS_AU_BUF_MIN
#define MS_AU_BUF_MIN (128*1024)
#endif
#ifndef MS_AU_BUF_MAX
#define MS_AU_BUF_MAX (1024*1024)
#endif
/* JPEG assembly buffer (was a fixed 512 KB static) */
#ifndef MS_JPEG_BUF_MIN
#define MS_JPEG_BUF_MIN (96*1024)
#endif
#ifndef MS_JPEG_BUF_MAX
#define MS_JPEG_BUF_MAX (512*1024)
#endif
/* Audio input buffering. The Ingenic AI delivers a frame only once usrFrmDepth
 * frames are cached, so this depth IS the audio latency (depth x 40 ms). It was
 * 30 (~1.2 s) which made browser/RTSP audio audibly lag the video; keep it small
 * for low latency (must stay > 0 or the AI delivers no frames). */
#ifndef MS_AI_FRM_NUM
#define MS_AI_FRM_NUM   6
#endif
#ifndef MS_AI_FRM_DEPTH
#define MS_AI_FRM_DEPTH 2
#endif

static IMPSensorInfo    g_sensor;
static const ms_config *g_hcfg;

typedef struct { int chn, grp, codec, w, h; volatile int run, active, idr_req; pthread_t thr; } vchan;
static vchan g_v[MS_MAX_VSTREAM];
static int   g_nv;
static volatile int g_arun, g_aactive;
static pthread_t    g_athr;
static int          g_acodec = MS_AC_PCMU;   /* effective audio codec */
static int          g_asr    = 8000;         /* effective audio sample rate */
/* JPEG channels: [0] = dedicated jpeg.* channel (own framesource), further
 * entries = optional encoders piggybacked on a video stream's encoder group
 * (videoN.jpeg = true) which share that stream's framesource (no extra rmem) */
typedef struct {
    int          chn;        /* IMP encoder channel */
    int          src;        /* hub source id (HUB_JPEG_SRC / HUB_JPEG_SRC_N) */
    int          w, h, fps;
    int          snapshot;   /* periodic file snapshot (dedicated chn only) */
    volatile int run, active;
    pthread_t    thr;
} jchan;
static jchan g_j[1+MS_MAX_VSTREAM];
static int   g_nj;

/* ================= system / sensor / ISP ================= */
/* apply the image.* (ISP) tuning block */
static void apply_image_tuning(void)
{
    const ms_image_cfg *im = &g_hcfg->image;
#if !defined(NO_TUNINGS)
    IMP_ISP_Tuning_SetBrightness(im->brightness);
    IMP_ISP_Tuning_SetContrast(im->contrast);
    IMP_ISP_Tuning_SetSaturation(im->saturation);
    IMP_ISP_Tuning_SetSharpness(im->sharpness);
    IMP_ISP_Tuning_SetISPHflip((IMPISPTuningOpsMode)(im->hflip?1:0));
    IMP_ISP_Tuning_SetISPVflip((IMPISPTuningOpsMode)(im->vflip?1:0));
    IMP_ISP_Tuning_SetMaxAgain(im->max_again);
    IMP_ISP_Tuning_SetMaxDgain(im->max_dgain);
    IMP_ISP_Tuning_SetTemperStrength(im->temper_strength);
#if !defined(PLATFORM_T21)
    IMP_ISP_Tuning_SetSinterStrength(im->sinter_strength);
    IMP_ISP_Tuning_SetAeComp(im->ae_compensation);
#endif
    {
        IMPISPAntiflickerAttr fl = (IMPISPAntiflickerAttr)im->anti_flicker;
        IMP_ISP_Tuning_SetAntiFlickerAttr(fl);
    }
    {
        IMPISPWB wb; memset(&wb,0,sizeof wb);
        wb.mode=(enum isp_core_wb_mode)im->core_wb_mode;
        wb.rgain=im->wb_rgain; wb.bgain=im->wb_bgain;
        IMP_ISP_Tuning_SetWB(&wb);
    }
#if defined(PLATFORM_T23)||defined(PLATFORM_T31)||defined(PLATFORM_C100)
    IMP_ISP_Tuning_SetBcshHue(im->hue);
    IMP_ISP_Tuning_SetDPC_Strength(im->dpc_strength);
    { uint8_t d=(uint8_t)im->defog_strength; IMP_ISP_Tuning_SetDefog_Strength(&d); }
#endif
#if defined(PLATFORM_T21)||defined(PLATFORM_T23)||defined(PLATFORM_T31)||defined(PLATFORM_C100)
    IMP_ISP_Tuning_SetDRC_Strength(im->drc_strength);
#endif
#if defined(PLATFORM_T23)||defined(PLATFORM_T31)||defined(PLATFORM_C100)
    /* SetBacklightComp only exists in the T23/T31/C100 SDKs */
    if (im->backlight_compensation>0)
        IMP_ISP_Tuning_SetBacklightComp(im->backlight_compensation);
    else
#endif
    if (im->highlight_depress>0)
        IMP_ISP_Tuning_SetHiLightDepress(im->highlight_depress);
    LOGI(MOD,"image tuning applied (bri=%d con=%d sat=%d sharp=%d)",
         im->brightness,im->contrast,im->saturation,im->sharpness);
#endif
}

static int isp_init(void)
{
    int ret;
    memset(&g_sensor,0,sizeof g_sensor);
    /* bounded copies: sensor.model (64) is larger than name (32) / i2c.type (20) */
    snprintf(g_sensor.name, sizeof g_sensor.name, "%.*s",
             (int)sizeof(g_sensor.name)-1, g_hcfg->sensor.model);
    g_sensor.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
    snprintf(g_sensor.i2c.type, sizeof g_sensor.i2c.type, "%.*s",
             (int)sizeof(g_sensor.i2c.type)-1, g_hcfg->sensor.model);
    g_sensor.i2c.addr = g_hcfg->sensor.i2c_addr;

#if !(defined(PLATFORM_T40)||defined(PLATFORM_T41))
    IMP_OSD_SetPoolSize(g_hcfg->osd_pool_size * 1024);
#endif
    ret = IMP_ISP_Open();
    if (ret<0){ LOGE(MOD,"IMP_ISP_Open failed"); return -1; }

#if defined(PLATFORM_T40)||defined(PLATFORM_T41)
    IMP_ISP_AddSensor(IMPVI_MAIN, &g_sensor);
    IMP_ISP_EnableSensor(IMPVI_MAIN, &g_sensor);
#else
    IMP_ISP_AddSensor(&g_sensor);
    IMP_ISP_EnableSensor();
#endif
    if (IMP_System_Init()<0){ LOGE(MOD,"IMP_System_Init failed"); return -1; }
    IMP_ISP_EnableTuning();
    apply_image_tuning();
    IMP_ISP_Tuning_SetSensorFPS(g_hcfg->sensor.fps, 1);
#ifdef IMPISP_RUNNING_MODE_DAY
    IMP_ISP_Tuning_SetISPRunningMode(IMPISP_RUNNING_MODE_DAY);
#endif
    IMP_System_GetVersion(NULL);
    LOGI(MOD,"ISP up, sensor=%s fps=%d", g_hcfg->sensor.model, g_hcfg->sensor.fps);
    return 0;
}

/* ================= framesource ================= */
static int fs_create(int chn, const ms_vstream_cfg *v)
{
    IMPFSChnAttr a; memset(&a,0,sizeof a);
    a.pixFmt = PIX_FMT_NV12;
    a.outFrmRateNum = v->fps; a.outFrmRateDen = 1;
    a.nrVBs = v->buffers>0 ? v->buffers : 2;
    a.type  = FS_PHY_CHANNEL;
    int scale = (g_hcfg->sensor.width!=v->width)||(g_hcfg->sensor.height!=v->height);
    a.crop.enable = 0;
    a.crop.width  = g_hcfg->sensor.width;  a.crop.height = g_hcfg->sensor.height;
    a.scaler.enable = scale;
    a.scaler.outwidth = v->width; a.scaler.outheight = v->height;
    a.picWidth = v->width; a.picHeight = v->height;
#if defined(PLATFORM_T31) && !defined(KERNEL_VERSION_4)
    if (v->rotation!=0){
        /* swap output dims for 90/270 and request rotation */
        a.scaler.outwidth = v->height; a.scaler.outheight = v->width;
        a.picWidth = v->height; a.picHeight = v->width;
        IMP_FrameSource_SetChnRotate(chn, v->rotation, v->height, v->width);
    }
#endif
    if (IMP_FrameSource_CreateChn(chn,&a)<0){ LOGE(MOD,"FS_CreateChn %d",chn); return -1; }
    IMP_FrameSource_SetChnAttr(chn,&a);
    return 0;
}

/* ================= encoder ================= */
static int enc_create(int chn, int grp, const ms_vstream_cfg *v)
{
    IMPEncoderChnAttr a; memset(&a,0,sizeof a);
#ifdef ENC_NEW_API
    IMPEncoderProfile prof = (v->codec==MS_VC_H265)
        ? IMP_ENC_PROFILE_HEVC_MAIN
        : (v->profile>=2?IMP_ENC_PROFILE_AVC_HIGH:
           v->profile==1?IMP_ENC_PROFILE_AVC_MAIN:IMP_ENC_PROFILE_AVC_BASELINE);
    IMPEncoderRcMode rc;
    switch (v->rc_mode){
        case MS_RC_VBR:            rc=IMP_ENC_RC_MODE_VBR; break;
        case MS_RC_FIXQP:          rc=IMP_ENC_RC_MODE_FIXQP; break;
        case MS_RC_CAPPED_VBR:     rc=IMP_ENC_RC_MODE_CAPPED_VBR; break;
        case MS_RC_SMART:
        case MS_RC_CAPPED_QUALITY: rc=IMP_ENC_RC_MODE_CAPPED_QUALITY; break;
        default:                   rc=IMP_ENC_RC_MODE_CBR; break;
    }
    IMP_Encoder_SetDefaultParam(&a, prof, rc,
        v->width, v->height, v->fps, 1, v->gop, 2, -1, v->bitrate_kbps);
#else
    /* older platforms: manual attribute setup (H264 only path shown) */
#if defined(PLATFORM_T10)||defined(PLATFORM_T20)
    a.encAttr.enType   = PT_H264;               /* no H.265 on T10/T20 */
#else
    a.encAttr.enType   = (v->codec==MS_VC_H265)?PT_H265:PT_H264;
#endif
    a.encAttr.bufSize  = 0;
    a.encAttr.profile  = v->profile;
    a.encAttr.picWidth = v->width;
    a.encAttr.picHeight= v->height;
    a.rcAttr.outFrmRate.frmRateNum = v->fps;
    a.rcAttr.outFrmRate.frmRateDen = 1;
    a.rcAttr.maxGop = v->gop;
    /* rc mode MUST be filled: an all-zero attrRcMode means FIXQP with qp=0
     * (broken stream). H264 CBR defaults, field names verified against the
     * vendored T20/T21/T30 headers. */
    a.rcAttr.attrRcMode.rcMode = ENC_RC_MODE_CBR;
    a.rcAttr.attrRcMode.attrH264Cbr.maxQp        = (v->max_qp>0)?(uint32_t)v->max_qp:45;
    a.rcAttr.attrRcMode.attrH264Cbr.minQp        = (v->min_qp>0)?(uint32_t)v->min_qp:15;
    a.rcAttr.attrRcMode.attrH264Cbr.outBitRate   = (uint32_t)v->bitrate_kbps;
    a.rcAttr.attrRcMode.attrH264Cbr.iBiasLvl     = 0;
    a.rcAttr.attrRcMode.attrH264Cbr.frmQPStep    = 3;
    a.rcAttr.attrRcMode.attrH264Cbr.gopQPStep    = 15;
    a.rcAttr.attrRcMode.attrH264Cbr.adaptiveMode = 0;
    a.rcAttr.attrRcMode.attrH264Cbr.gopRelation  = 0;
#endif
    if (IMP_Encoder_CreateChn(chn,&a)<0){ LOGE(MOD,"Encoder_CreateChn %d",chn); return -1; }
    IMP_Encoder_RegisterChn(grp, chn);
    return 0;
}

/* concatenated pack -> keyframe test */
static int au_is_key(int codec, const uint8_t *au, size_t len)
{
    nal_iter it; nal_unit u; nal_iter_init(&it,au,len);
    while (nal_iter_next(&it,&u)){
        if (u.len<1) continue;
        if (codec==MS_VC_H264){ if (h264_nal_type(u.data)==5) return 1; }
        else { int t=h265_nal_type(u.data); if (t>=16&&t<=23) return 1; }
    }
    return 0;
}

static void *video_thread(void *arg)
{
    vchan *vc = (vchan*)arg;
    /* AU buffer sized from the resolution (~0.5 byte/pixel is generous for an
     * IDR), bounded to [MS_AU_BUF_MIN, MS_AU_BUF_MAX]. Heap instead of a fixed
     * 1 MB __thread array so small-RAM SoCs are not penalized per stream. */
    size_t au_cap = (size_t)vc->w * (size_t)vc->h / 2;
    if (au_cap < MS_AU_BUF_MIN) au_cap = MS_AU_BUF_MIN;
    if (au_cap > MS_AU_BUF_MAX) au_cap = MS_AU_BUF_MAX;
    uint8_t *au = (uint8_t*)malloc(au_cap);
    if (!au){ LOGE(MOD,"chn%d: no memory for AU buffer (%zu)",vc->chn,au_cap); return NULL; }
    int receiving=0;
    int64_t idle_since=0;
    while (vc->run) {
        /* on-demand: encode while there are consumers. The hub subscriber
         * count is the truth source (level, not edge), so a racing stale
         * "inactive" flag can never stop a stream that still has clients. */
        int want = vc->active || hub_active(vc->chn);
        if (!want) {
            if (!receiving){ usleep(50000); continue; }
            /* debounce the stop: only shut the encoder down after a sustained
             * idle period; rapid client churn must not toggle Start/StopRecvPic */
            int64_t now = ms_now_us();
            if (idle_since==0) idle_since = now;
            if (now - idle_since >= MS_IDLE_STOP_US) {
                IMP_Encoder_StopRecvPic(vc->chn); receiving=0; idle_since=0;
                LOGI(MOD,"video chn%d idle",vc->chn);
                continue;
            }
            /* during the debounce window keep draining the encoder below so
             * the pipeline never backs up (publish is a no-op with 0 subs) */
        } else {
            idle_since = 0;
            if (!receiving){ IMP_Encoder_StartRecvPic(vc->chn); receiving=1;
                             vc->idr_req=0; IMP_Encoder_RequestIDR(vc->chn);
                             LOGI(MOD,"video chn%d streaming",vc->chn); }
        }
        /* honor IDR requests from RTSP/HTTP threads here (single-thread IMP) */
        if (vc->idr_req){ vc->idr_req=0; IMP_Encoder_RequestIDR(vc->chn); }
        if (IMP_Encoder_PollingStream(vc->chn, g_hcfg->imp_polling_timeout)!=0) continue;
        IMPEncoderStream st;
        if (IMP_Encoder_GetStream(vc->chn,&st,1)!=0) continue;
        size_t aulen=0;
        for (uint32_t i=0;i<st.packCount;i++){
#ifdef ENC_NEW_API
            const uint8_t *p=(const uint8_t*)(uintptr_t)st.virAddr + st.pack[i].offset;
#else
            const uint8_t *p=(const uint8_t*)(uintptr_t)st.pack[i].virAddr;
#endif
            size_t l=st.pack[i].length;
            if (l==0) continue;
            /* guarantee Annex-B: prepend a start code if the pack lacks one */
            int has_sc = (l>=3 && p[0]==0 && p[1]==0 &&
                          (p[2]==1 || (l>=4 && p[2]==0 && p[3]==1)));
            if (!has_sc && aulen+4<=au_cap){
                au[aulen++]=0; au[aulen++]=0; au[aulen++]=0; au[aulen++]=1;
            }
            if (aulen+l<=au_cap){ memcpy(au+aulen,p,l); aulen+=l; }
        }
        int key = au_is_key(vc->codec, au, aulen);
        hub_publish(vc->chn, au, aulen, ms_now_us(), key, MS_MEDIA_VIDEO);
        IMP_Encoder_ReleaseStream(vc->chn,&st);
    }
    if (receiving) IMP_Encoder_StopRecvPic(vc->chn);
    free(au);
    return NULL;
}

/* ================= JPEG / MJPEG ================= */
static void *jpeg_thread(void *arg)
{
    jchan *jc = (jchan*)arg;
    /* buffer sized from the JPEG resolution (~0.5 byte/pixel), bounded */
    size_t jcap = (size_t)jc->w * (size_t)jc->h / 2;
    if (jcap < MS_JPEG_BUF_MIN) jcap = MS_JPEG_BUF_MIN;
    if (jcap > MS_JPEG_BUF_MAX) jcap = MS_JPEG_BUF_MAX;
    uint8_t *jbuf = (uint8_t*)malloc(jcap);
    if (!jbuf){ LOGE(MOD,"no memory for JPEG buffer (%zu)",jcap); return NULL; }
    int receiving=0;
    int64_t next=0, idle_since=0;
    int64_t period = 1000000/(jc->fps>0?jc->fps:5);
    while (jc->run) {
        /* run when an MJPEG/snapshot client is connected, or when a periodic
         * file snapshot is configured. Stop is debounced like video (see
         * MS_IDLE_STOP_US) to avoid Start/StopRecvPic churn. */
        int jwant = jc->active || jc->snapshot || hub_active(jc->src);
        if (!jwant) {
            if (!receiving){ usleep(50000); continue; }
            int64_t nowi = ms_now_us();
            if (idle_since==0) idle_since = nowi;
            if (nowi - idle_since >= MS_IDLE_STOP_US) {
                IMP_Encoder_StopRecvPic(jc->chn); receiving=0; idle_since=0;
                continue;
            }
        } else idle_since = 0;
        if (!receiving){ IMP_Encoder_StartRecvPic(jc->chn); receiving=1; }
        int64_t now=ms_now_us();
        if (now<next){ usleep(next-now); }
        next=ms_now_us()+period;

        if (IMP_Encoder_PollingStream(jc->chn, g_hcfg->imp_polling_timeout)!=0) continue;
        IMPEncoderStream st;
        if (IMP_Encoder_GetStream(jc->chn,&st,1)!=0) continue;
        size_t jlen=0;
        for (uint32_t i=0;i<st.packCount;i++){
#ifdef ENC_NEW_API
            const uint8_t *p=(const uint8_t*)(uintptr_t)st.virAddr + st.pack[i].offset;
#else
            const uint8_t *p=(const uint8_t*)st.pack[i].virAddr;
#endif
            size_t l=st.pack[i].length;
            if (jlen+l<=jcap){ memcpy(jbuf+jlen,p,l); jlen+=l; }
        }
        if (jc->active || hub_active(jc->src))
            hub_publish(jc->src, jbuf, jlen, ms_now_us(), 1, MS_MEDIA_JPEG);
        if (jc->snapshot && g_hcfg->jpeg.snapshot_path[0]){
            char tmp[160]; snprintf(tmp,sizeof tmp,"%s.tmp",g_hcfg->jpeg.snapshot_path);
            FILE *f=fopen(tmp,"wb");
            if (f){ fwrite(jbuf,1,jlen,f); fclose(f); rename(tmp,g_hcfg->jpeg.snapshot_path); }
        }
        IMP_Encoder_ReleaseStream(jc->chn,&st);
    }
    if (receiving) IMP_Encoder_StopRecvPic(jc->chn);
    free(jbuf);
    return NULL;
}

/* create one JPEG encoder channel (not yet registered to a group) */
static int jpeg_enc_create(int chn, int w, int h, int quality)
{
    IMPEncoderChnAttr a; memset(&a,0,sizeof a);
#ifdef ENC_NEW_API
    /* exact JPEG params from prudynt: frmRate 24/1, gop 0, maxSameScene 0,
     * iInitialQP = quality, targetBitRate = 0. (Putting quality into the
     * bitrate slot with iInitialQP=-1 caused the div-by-zero SIGFPE.) */
    IMP_Encoder_SetDefaultParam(&a, IMP_ENC_PROFILE_JPEG, IMP_ENC_RC_MODE_FIXQP,
        w, h, 24, 1, 0, 0, quality, 0);
#else
    (void)quality;
    a.encAttr.enType=PT_JPEG;
    a.encAttr.picWidth=w; a.encAttr.picHeight=h;
#endif
    if (IMP_Encoder_CreateChn(chn,&a)!=0){ LOGE(MOD,"JPEG CreateChn %d failed",chn); return -1; }
    return 0;
}

static void jpeg_chan_start(int chn, int src, int w, int h, int fps, int snapshot)
{
    jchan *jc=&g_j[g_nj++];
    jc->chn=chn; jc->src=src; jc->w=w; jc->h=h;
    jc->fps=fps>0?fps:5; jc->snapshot=snapshot;
    jc->run=1; jc->active=0;
    pthread_create(&jc->thr,NULL,jpeg_thread,jc);
}

/* dedicated JPEG channel: own framesource + own encoder group (jpeg.*) */
static int jpeg_setup(const ms_config *cfg)
{
    int chn = cfg->jpeg.imp_chn;
    ms_vstream_cfg jv; memset(&jv,0,sizeof jv);
    jv.width=cfg->jpeg.width; jv.height=cfg->jpeg.height; jv.fps=cfg->jpeg.fps>0?cfg->jpeg.fps:5;
    jv.buffers=2; jv.codec=MS_VC_H264; /* framesource is codec-agnostic */
    if (fs_create(chn,&jv)!=0) return -1;
    if (jpeg_enc_create(chn, cfg->jpeg.width, cfg->jpeg.height, cfg->jpeg.quality)!=0) return -1;
    IMP_Encoder_CreateGroup(chn);
    IMP_Encoder_RegisterChn(chn, chn);
    IMPCell fs={DEV_ID_FS,chn,0}, enc={DEV_ID_ENC,chn,0};
    IMP_System_Bind(&fs,&enc);
    IMP_FrameSource_EnableChn(chn);
    jpeg_chan_start(chn, HUB_JPEG_SRC, cfg->jpeg.width, cfg->jpeg.height,
                    cfg->jpeg.fps, cfg->jpeg.snapshot_path[0]!=0);
    LOGI(MOD,"JPEG channel %d ready (%dx%d q%d)",chn,cfg->jpeg.width,cfg->jpeg.height,cfg->jpeg.quality);
    return 0;
}

/* optional JPEG encoder piggybacked on video stream vi: registered into the
 * SAME encoder group, so it shares the video framesource. Costs no extra rmem
 * (no new framesource buffers), only the encoder channel itself. */
static int jpeg_attach(const ms_config *cfg, int vi, int grp)
{
    const ms_vstream_cfg *v=&cfg->video[vi];
    int chn = v->jpeg_chn;
    int q   = (v->jpeg_quality>0 && v->jpeg_quality<=100) ? v->jpeg_quality : 75;
    if (jpeg_enc_create(chn, v->width, v->height, q)!=0) return -1;
    if (IMP_Encoder_RegisterChn(grp, chn)!=0){
        LOGE(MOD,"JPEG RegisterChn %d to group %d failed",chn,grp);
        IMP_Encoder_DestroyChn(chn);
        return -1;
    }
    jpeg_chan_start(chn, HUB_JPEG_SRC_N(vi), v->width, v->height, v->jpeg_fps, 0);
    LOGI(MOD,"JPEG-on-video%d: encoder chn %d in group %d (%dx%d q%d)",
         vi,chn,grp,v->width,v->height,q);
    return 0;
}

/* ================= audio ================= */
static int ai_rate_enum(int sr)
{
    switch (sr) {
        case 8000:  return AUDIO_SAMPLE_RATE_8000;
        case 16000: return AUDIO_SAMPLE_RATE_16000;
        default:    return AUDIO_SAMPLE_RATE_8000;
    }
}

static void *audio_thread(void *arg)
{
    (void)arg;
    int dev=0, chnid=0;
    int use_aac = (g_acodec==MS_AC_AAC);

    /* --- configure the audio input FIRST, with a samplerate fallback ---
     * The AI frame size is decoupled from the AAC encoder: the SoC only accepts
     * "natural" frame sizes (a 40 ms frame here), so 16 kHz/1024 was rejected.
     * We capture 40 ms frames and re-block them into faac's 1024-sample units. */
    int want_sr[2] = { g_asr, 8000 };
    int nsr = (g_asr==8000) ? 1 : 2;
    int ai_ok = 0;
    IMPAudioIOAttr aio;
    for (int r=0; r<nsr && !ai_ok; r++){
        int sr = want_sr[r];
        memset(&aio,0,sizeof aio);
        aio.samplerate = ai_rate_enum(sr);
        aio.bitwidth   = AUDIO_BIT_WIDTH_16;
        aio.soundmode  = AUDIO_SOUND_MODE_MONO;
        aio.frmNum     = MS_AI_FRM_NUM;
        aio.numPerFrm  = sr*40/1000;          /* 40 ms: 320@8k, 640@16k */
        aio.chnCnt     = 1;
        if (IMP_AI_SetPubAttr(dev,&aio)==0){ g_asr=sr; ai_ok=1; break; }
        LOGW(MOD,"IMP_AI_SetPubAttr %dHz failed%s", sr, (r+1<nsr)?" -> trying 8000":"");
    }
    if (!ai_ok){ LOGE(MOD,"audio input unavailable"); return NULL; }

    IMP_AI_Enable(dev);
    /* REQUIRED on T-series: without a channel frame depth the AI delivers no
     * frames (PollingFrame then returns empty -> silent audio) */
    IMPAudioIChnParam chnp; memset(&chnp,0,sizeof chnp);
    chnp.usrFrmDepth = MS_AI_FRM_DEPTH;   /* low depth = low audio latency */
    IMP_AI_SetChnParam(dev,chnid,&chnp);
    IMP_AI_EnableChn(dev,chnid);
    IMP_AI_SetVol(dev,chnid, g_hcfg->audio.volume);
    IMP_AI_SetGain(dev,chnid, g_hcfg->audio.gain);

    /* --- now open faac at the samplerate the AI actually accepted --- */
#ifdef USE_FAAC
    faacEncHandle faac = NULL;
    unsigned long faac_in = 1024, faac_max = 8192;
    if (use_aac) {
        faac = faacEncOpen(g_asr, 1, &faac_in, &faac_max);
        if (faac) {
            faacEncConfigurationPtr fc = faacEncGetCurrentConfiguration(faac);
            fc->inputFormat   = FAAC_INPUT_16BIT;
            fc->outputFormat  = 0;             /* raw AAC (no ADTS) */
            fc->mpegVersion   = MPEG4;
            fc->aacObjectType = LOW;
            fc->useTns = 0; fc->useLfe = 0;
            if (g_hcfg->audio.bitrate_kbps>0) fc->bitRate = g_hcfg->audio.bitrate_kbps*1000;
            faacEncSetConfiguration(faac, fc);
            LOGI(MOD,"faac AAC encoder: %dHz in=%lu max=%lu",g_asr,faac_in,faac_max);
        } else {
            LOGW(MOD,"faacEncOpen failed -> PCMU");
            use_aac=0; g_acodec=MS_AC_PCMU;
        }
    }
#else
    use_aac = 0;   /* built without USE_FAAC: no software AAC */
    if (g_acodec==MS_AC_AAC) g_acodec=MS_AC_PCMU;
#endif

    /* the codec/rate the HAL actually produces must be what SDP/ASC advertise */
    if (!use_aac && g_acodec==MS_AC_PCMU && g_asr!=8000) g_asr=8000;
    hub_set_audio_params(g_acodec, g_asr, 1);

    LOGI(MOD,"audio in: %dHz %s vol=%d gain=%d numPerFrm=%d", g_asr,
         use_aac?"AAC":(g_acodec==MS_AC_PCMA?"PCMA":"PCMU"),
         g_hcfg->audio.volume, g_hcfg->audio.gain, aio.numPerFrm);

#ifdef USE_FAAC
    /* re-blocking buffer: accumulate 40 ms AI frames, feed faac_in-sized units */
    int16_t   acc[4096];
    size_t    acc_n = 0;
    int dbg_logged = 0;
#endif

    while (g_arun) {
        if (!g_aactive){ usleep(50000); continue; }   /* on-demand */
        int64_t a_t0 = ms_now_us();
        /* sleep on the no-frame path so a non-blocking/failing PollingFrame
         * can never spin the CPU (audio input may be idle on some boards) */
        if (IMP_AI_PollingFrame(dev,chnid, g_hcfg->imp_polling_timeout)!=0){ usleep(10000); continue; }
        IMPAudioFrame frm;
        if (IMP_AI_GetFrame(dev,chnid,&frm,1)!=0) continue;
        const int16_t *pcm=(const int16_t*)frm.virAddr;
        size_t samples=frm.len/2;
        if (use_aac) {
#ifdef USE_FAAC
            /* append into the accumulator, then drain in faac_in blocks */
            for (size_t off=0; off<samples; ){
                size_t take = samples-off;
                if (acc_n+take > sizeof(acc)/sizeof(acc[0])) take = sizeof(acc)/sizeof(acc[0]) - acc_n;
                memcpy(acc+acc_n, pcm+off, take*sizeof(int16_t));
                acc_n += take; off += take;
                while (acc_n >= faac_in){
                    static __thread uint8_t aac[8192];
                    int n = faacEncEncode(faac, (int32_t*)(void*)acc, (unsigned)faac_in, aac, sizeof aac);
                    if (n<0) LOGW(MOD,"faacEncEncode returned %d",n);
                    if (n>0){
                        if (!dbg_logged){ LOGI(MOD,"AAC encoder producing (%d bytes/frame)",n); dbg_logged=1; }
                        hub_publish(HUB_AUDIO_SRC, aac, (size_t)n, ms_now_us(), 0, MS_MEDIA_AUDIO);
                    }
                    acc_n -= faac_in;
                    memmove(acc, acc+faac_in, acc_n*sizeof(int16_t));
                }
            }
#endif
        } else {
            uint8_t enc[2048];
            if (samples>sizeof enc) samples=sizeof enc;
            if (g_acodec==MS_AC_PCMA) g711_alaw_encode(pcm,samples,enc);
            else                      g711_ulaw_encode(pcm,samples,enc);
            hub_publish(HUB_AUDIO_SRC,enc,samples,ms_now_us(),0,MS_MEDIA_AUDIO);
        }
        IMP_AI_ReleaseFrame(dev,chnid,&frm);
        /* adaptive pacing: if IMP_AI didn't actually block (spin), throttle to
         * ~real time; if it blocked (~20 ms), this adds nothing */
        int64_t a_dt = ms_now_us() - a_t0;
        if (a_dt < 15000) usleep(15000 - a_dt);
    }
#ifdef USE_FAAC
    if (faac) faacEncClose(faac);
#endif
    IMP_AI_DisableChn(dev,chnid);
    IMP_AI_Disable(dev);
    return NULL;
}

#ifdef USE_CONTROL
/* Apply a live setting from the web UI (via /control -> hub_control). ISP tuning
 * calls are runtime-safe but we serialize them under a mutex. Optional feature,
 * compiled only with -DUSE_CONTROL. */
static pthread_mutex_t g_isp_lock = PTHREAD_MUTEX_INITIALIZER;
static void ing_control(const char *key, int val)
{
    pthread_mutex_lock(&g_isp_lock);
    if (!strcmp(key,"running_mode")){
#ifdef IMPISP_RUNNING_MODE_DAY
        IMP_ISP_Tuning_SetISPRunningMode(val ? IMPISP_RUNNING_MODE_NIGHT
                                             : IMPISP_RUNNING_MODE_DAY);
#endif
    }
    else if (!strcmp(key,"brightness")) IMP_ISP_Tuning_SetBrightness(val);
    else if (!strcmp(key,"contrast"))   IMP_ISP_Tuning_SetContrast(val);
    else if (!strcmp(key,"saturation")) IMP_ISP_Tuning_SetSaturation(val);
    else if (!strcmp(key,"sharpness"))  IMP_ISP_Tuning_SetSharpness(val);
    else if (!strcmp(key,"hflip"))      IMP_ISP_Tuning_SetISPHflip((IMPISPTuningOpsMode)(val?1:0));
    else if (!strcmp(key,"vflip"))      IMP_ISP_Tuning_SetISPVflip((IMPISPTuningOpsMode)(val?1:0));
    pthread_mutex_unlock(&g_isp_lock);
    LOGI(MOD,"control %s=%d", key, val);
}
#endif

/* ================= HAL entry points ================= */
static int ing_init(const ms_config *cfg)
{
    g_hcfg=cfg;
    int r = isp_init();
#ifdef USE_CONTROL
    if (r==0) hub_set_control_cb(ing_control);
#endif
    return r;
}

static int ing_start(const ms_config *cfg)
{
    g_nv=0;
    for (int i=0;i<MS_MAX_VSTREAM;i++){
        if (!cfg->video[i].enabled) continue;
        const ms_vstream_cfg *v=&cfg->video[i];
        int chn=v->imp_chn, grp=v->imp_chn;
        if (fs_create(chn,v)!=0) return -1;
        if (enc_create(chn,grp,v)!=0) return -1;
        IMP_Encoder_CreateGroup(grp);
        /* optional per-stream JPEG encoder in the same group (videoN.jpeg) */
        if (v->jpeg_enabled) jpeg_attach(cfg, i, grp);

        /* pipeline: FrameSource -> [OSD] -> Encoder. Every stream gets its
         * own OSD group so overlays appear on all streams. */
        IMPCell fs  = { DEV_ID_FS,  chn, 0 };
        IMPCell enc = { DEV_ID_ENC, grp, 0 };
        int og = imp_osd_setup(cfg, i, v->width, v->height);
        if (og >= 0) {
            IMPCell osd = { DEV_ID_OSD, og, 0 };
            IMP_System_Bind(&fs,&osd);
            IMP_System_Bind(&osd,&enc);
        } else {
            IMP_System_Bind(&fs,&enc);
        }
        IMP_FrameSource_EnableChn(chn);

        hub_set_video_params(i, v->codec, v->width, v->height, v->fps);
        g_v[g_nv].chn=chn; g_v[g_nv].grp=grp; g_v[g_nv].codec=v->codec;
        g_v[g_nv].w=v->width; g_v[g_nv].h=v->height; g_v[g_nv].run=1;
        pthread_create(&g_v[g_nv].thr,NULL,video_thread,&g_v[g_nv]);
        g_nv++;
    }

    imp_osd_start_updater();   /* one thread refreshes OSD on all streams */

    if (cfg->audio.enabled && cfg->audio.codec!=MS_AC_NONE){
        /* pick the codec the SoC can actually encode. IMP_AENC on the
         * T-series only does G.711/G.726 - there is no hardware AAC, so an
         * AAC request transparently degrades to PCMU (G.711u @ 8 kHz). */
        g_acodec = cfg->audio.codec;
#if !defined(USE_FAAC) && !defined(IMP_AUDIO_ENC_TYPE_AAC)
        if (g_acodec==MS_AC_AAC){
            LOGW(MOD,"no AAC encoder (build with USE_FAAC) -> using PCMU (G.711u)");
            g_acodec=MS_AC_PCMU;
        }
#endif
        g_asr = (g_acodec==MS_AC_AAC) ? cfg->audio.samplerate : 8000; /* G.711 = 8 kHz */
        hub_set_audio_params(g_acodec, g_asr, cfg->audio.channels);
        g_arun=1;
        pthread_create(&g_athr,NULL,audio_thread,NULL);
    }

    if (cfg->jpeg.enabled) jpeg_setup(cfg);
    if (cfg->motion.enabled) imp_motion_start(cfg);
    return 0;
}

static void ing_set_active(int src, int on)
{
    if (src==HUB_AUDIO_SRC){ g_aactive=on; return; }
    if (src>=HUB_JPEG_SRC && src<HUB_JPEG_SRC+HUB_NJPEG){
        for (int i=0;i<g_nj;i++) if (g_j[i].src==src){ g_j[i].active=on; return; }
        return;
    }
    for (int i=0;i<g_nv;i++) if (g_v[i].chn==src){ g_v[i].active=on; return; }
}

static void ing_request_idr(int src)
{
    /* IMP_Encoder is not safe to touch from foreign threads. Just flag the
     * request; the owning video_thread issues the actual IMP_Encoder_RequestIDR
     * so each encoder channel is only ever accessed from its own thread. */
    for (int i=0;i<g_nv;i++) if (g_v[i].chn==src) g_v[i].idr_req=1;
}

static void ing_stop(void)
{
    if (g_hcfg->motion.enabled) imp_motion_stop();
    for (int i=0;i<g_nv;i++){ g_v[i].run=0; pthread_join(g_v[i].thr,NULL); }
    if (g_arun){ g_arun=0; pthread_join(g_athr,NULL); }
    for (int i=0;i<g_nj;i++){ g_j[i].run=0; pthread_join(g_j[i].thr,NULL); }
    for (int i=0;i<g_nj;i++){
        jchan *jc=&g_j[i];
        if (jc->src==HUB_JPEG_SRC){
            /* dedicated channel: own framesource + own group */
            IMPCell fs={DEV_ID_FS,jc->chn,0}, enc={DEV_ID_ENC,jc->chn,0};
            IMP_FrameSource_DisableChn(jc->chn);
            IMP_System_UnBind(&fs,&enc);
            IMP_Encoder_UnRegisterChn(jc->chn);
            IMP_Encoder_DestroyChn(jc->chn);
            IMP_Encoder_DestroyGroup(jc->chn);
            IMP_FrameSource_DestroyChn(jc->chn);
        } else {
            /* piggyback: only the encoder channel; framesource and group
             * belong to the video stream and are torn down below */
            IMP_Encoder_UnRegisterChn(jc->chn);
            IMP_Encoder_DestroyChn(jc->chn);
        }
    }
    g_nj=0;
    if (g_hcfg->osd.enabled) imp_osd_stop();

    for (int i=0;i<g_nv;i++){
        int chn=g_v[i].chn, grp=g_v[i].grp;
        IMPCell fs={DEV_ID_FS,chn,0}, enc={DEV_ID_ENC,grp,0};
        IMP_FrameSource_DisableChn(chn);
        IMP_System_UnBind(&fs,&enc);
        IMP_Encoder_UnRegisterChn(chn);
        IMP_Encoder_DestroyChn(chn);
        IMP_Encoder_DestroyGroup(grp);
        IMP_FrameSource_DestroyChn(chn);
    }
    IMP_System_Exit();
#if defined(PLATFORM_T40)||defined(PLATFORM_T41)
    IMP_ISP_DisableSensor(IMPVI_MAIN);
    IMP_ISP_DelSensor(IMPVI_MAIN,&g_sensor);
#else
    IMP_ISP_DisableSensor();
    IMP_ISP_DelSensor(&g_sensor);
#endif
    IMP_ISP_DisableTuning();
    IMP_ISP_Close();
}

static const hal_backend g_ingenic = {
    .name="ingenic", .init=ing_init, .start=ing_start,
    .request_idr=ing_request_idr, .set_active=ing_set_active, .stop=ing_stop
};
const hal_backend *hal_get(void){ return &g_ingenic; }
#endif /* HAL_INGENIC */
