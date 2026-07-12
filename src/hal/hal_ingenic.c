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
#include "../isp_caps.h"
#include "../audio_caps.h"
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
#include <time.h>

#define MOD "HAL_ING"

#if defined(PLATFORM_T31)||defined(PLATFORM_C100)||defined(PLATFORM_T40)||defined(PLATFORM_T41)
#define ENC_NEW_API 1
#else
/* classic encoder headers (T10..T30) spell the channel attr type with a
 * capitalized CHN; alias it so enc_create() reads the same on every SoC */
typedef IMPEncoderCHNAttr IMPEncoderChnAttr;
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

typedef struct {
    int chn, grp, codec, w, h;
    volatile int run, active, idr_req;
    pthread_t thr;
} vchan;
static vchan g_v[MS_MAX_VSTREAM];
static int   g_nv;
static volatile int g_arun, g_aactive;
static pthread_t    g_athr;
static int          g_acodec = MS_AC_PCMU;   /* effective audio codec */
static int          g_asr    = 8000;         /* effective audio sample rate */
static IMPAudioIOAttr g_aio;                 /* attr accepted by IMP_AI_SetPubAttr */
static volatile int   g_ai_up = 0;           /* AI dev 0/chn 0 enabled (audio_thread) */
/* JPEG channels: [0] = dedicated jpeg.* channel (own framesource), further
 * entries = optional encoders piggybacked on a video stream's encoder group
 * (videoN.jpeg = true) which share that stream's framesource (no extra rmem) */
typedef struct {
    int          chn;        /* IMP encoder channel */
    int          fs_chn;     /* framesource feeding it (own or the video's) */
    int          src;        /* hub source id (HUB_JPEG_SRC / HUB_JPEG_SRC_N) */
    int          w, h, fps;
    int          snapshot;   /* periodic file snapshot (dedicated chn only) */
    volatile int run, active;
    pthread_t    thr;
} jchan;
static jchan g_j[1+MS_MAX_VSTREAM];
static int   g_nj;

/* ---- idle blocking + on-demand framesource ----
 * Idle producer threads block on g_act_cond instead of usleep-polling.
 * ing_set_active() broadcasts on every activation; the 1 s timeout is only a
 * safety net against lost wakeups and bounds the shutdown join latency. */
static pthread_mutex_t g_act_mtx  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_act_cond = PTHREAD_COND_INITIALIZER;
static void act_wake(void)
{
    pthread_mutex_lock(&g_act_mtx);
    pthread_cond_broadcast(&g_act_cond);
    pthread_mutex_unlock(&g_act_mtx);
}
static void act_wait(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1;
    pthread_mutex_lock(&g_act_mtx);
    pthread_cond_timedwait(&g_act_cond, &g_act_mtx, &ts);
    pthread_mutex_unlock(&g_act_mtx);
}

/* A FrameSource channel is enabled only while someone consumes its frames.
 * An enabled FS keeps the whole bound pipeline (FS -> OSD -> encoder group)
 * pumping frames at sensor fps inside libimp's worker threads even when the
 * encoder channel has StopRecvPic'd - that was ~19 % idle CPU with zero
 * clients. Refcounted because a video stream and its piggybacked JPEG
 * encoder share one FS (and motion detection pins the monitored stream). */
#define MS_FS_MAXCHN 8
static pthread_mutex_t g_fs_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_fs_users[MS_FS_MAXCHN];
static void fs_use(int chn)
{
    if (chn < 0 || chn >= MS_FS_MAXCHN) return;
    pthread_mutex_lock(&g_fs_mtx);
    if (g_fs_users[chn]++ == 0) {
        IMP_FrameSource_EnableChn(chn);
        LOGI(MOD,"framesource %d enabled", chn);
    }
    pthread_mutex_unlock(&g_fs_mtx);
}
static void fs_unuse(int chn)
{
    if (chn < 0 || chn >= MS_FS_MAXCHN) return;
    pthread_mutex_lock(&g_fs_mtx);
    if (g_fs_users[chn] > 0 && --g_fs_users[chn] == 0) {
        IMP_FrameSource_DisableChn(chn);
        LOGI(MOD,"framesource %d disabled (idle)", chn);
    }
    pthread_mutex_unlock(&g_fs_mtx);
}

/* ================= motion detection lifecycle ================= */
/* Bring the IVS motion grid in sync with the current config. ANY runtime
 * change (enable/disable, cols/rows, sensitivity, monitor_stream) goes
 * through a clean stop + recreate: the move parameters (grid ROIs, sense)
 * are create-time attributes of the IVS interface, so live geometry or
 * sensitivity changes rebuild the channel. While motion runs, the monitored
 * stream's FrameSource is pinned (fs_use) so the idle logic never turns off
 * the frames the IVS group feeds on. Serialized: called from ing_start/
 * ing_stop (main thread) and from /control connection threads. */
static pthread_mutex_t g_motion_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_motion_pin = -1;    /* pinned FS channel while motion runs */
static void motion_sync(const ms_config *cfg)
{
    pthread_mutex_lock(&g_motion_mtx);
    if (g_motion_pin >= 0) {                     /* running -> stop first */
        imp_motion_stop();
        fs_unuse(g_motion_pin);
        g_motion_pin = -1;
    }
    if (cfg->motion.enabled) {
        int mon = cfg->motion.monitor_stream;
        if (mon < 0 || mon >= MS_MAX_VSTREAM || !cfg->video[mon].enabled)
            mon = 0;
        if (imp_motion_start(cfg) == 0) {
            /* IVS needs the monitored stream's frames independent of
             * clients: pin that framesource until motion stops */
            g_motion_pin = cfg->video[mon].imp_chn;
            fs_use(g_motion_pin);
        }
    }
    pthread_mutex_unlock(&g_motion_mtx);
}

/* ================= system / sensor / ISP ================= */
/* Apply one image.* (ISP tuning) key from the current config (g_hcfg->image).
 * Returns 1 when the key is wired on this PLATFORM's IMP SDK, 0 when the SoC
 * cannot do it (the value is still parsed/persisted by the config layer).
 * The per-SoC guards come from ../isp_caps.h - keep them in sync with the
 * caps.image list control.c reports. Callers serialize ISP access (g_isp_lock
 * for live control; init is single-threaded). */
static int isp_apply_image(const char *k)
{
#if defined(NO_TUNINGS)
    (void)k;
    return 1;
#else
    const ms_image_cfg *im = &g_hcfg->image;
#ifdef ISP_NEW_TUNING_API           /* T40/T41: IMPVI_NUM + pointer args */
    if (!strcmp(k,"brightness")){ unsigned char u=(unsigned char)im->brightness;
        IMP_ISP_Tuning_SetBrightness(IMPVI_MAIN,&u); return 1; }
    if (!strcmp(k,"contrast")){ unsigned char u=(unsigned char)im->contrast;
        IMP_ISP_Tuning_SetContrast(IMPVI_MAIN,&u); return 1; }
    if (!strcmp(k,"saturation")){ unsigned char u=(unsigned char)im->saturation;
        IMP_ISP_Tuning_SetSaturation(IMPVI_MAIN,&u); return 1; }
    if (!strcmp(k,"sharpness")){ unsigned char u=(unsigned char)im->sharpness;
        IMP_ISP_Tuning_SetSharpness(IMPVI_MAIN,&u); return 1; }
    if (!strcmp(k,"hue")){ unsigned char u=(unsigned char)im->hue;
        IMP_ISP_Tuning_SetBcshHue(IMPVI_MAIN,&u); return 1; }
    if (!strcmp(k,"hflip") || !strcmp(k,"vflip")){
        IMPISPHVFLIP m = im->hflip
            ? (im->vflip ? IMPISP_FLIP_HV_MODE : IMPISP_FLIP_H_MODE)
            : (im->vflip ? IMPISP_FLIP_V_MODE  : IMPISP_FLIP_NORMAL_MODE);
#if defined(PLATFORM_T41)
        /* T41 wraps the mode in IMPISPHVFLIPAttr (sensor + per-channel ISP);
         * flip at the sensor, leave the ISP channels at NORMAL */
        IMPISPHVFLIPAttr fa; memset(&fa,0,sizeof fa);
        fa.sensor_mode = m;
        IMP_ISP_Tuning_SetHVFLIP(IMPVI_MAIN,&fa);
#else
        IMP_ISP_Tuning_SetHVFLIP(IMPVI_MAIN,&m);
#endif
        return 1;
    }
    if (!strcmp(k,"running_mode")){
        IMPISPRunningMode m = im->running_mode ? IMPISP_RUNNING_MODE_NIGHT
                                               : IMPISP_RUNNING_MODE_DAY;
        IMP_ISP_Tuning_SetISPRunningMode(IMPVI_MAIN,&m); return 1;
    }
    if (!strcmp(k,"anti_flicker")){ /* 0 off, 1 = 50 Hz, 2 = 60 Hz */
        IMPISPAntiflickerAttr fl; memset(&fl,0,sizeof fl);
        fl.mode = im->anti_flicker ? IMPISP_ANTIFLICKER_NORMAL_MODE
                                   : IMPISP_ANTIFLICKER_DISABLE_MODE;
        fl.freq = (im->anti_flicker==2) ? 60 : 50;
        IMP_ISP_Tuning_SetAntiFlickerAttr(IMPVI_MAIN,&fl); return 1;
    }
#else                               /* classic API (T10..T31, C100) */
    if (!strcmp(k,"brightness")){ IMP_ISP_Tuning_SetBrightness((unsigned char)im->brightness); return 1; }
    if (!strcmp(k,"contrast")){   IMP_ISP_Tuning_SetContrast((unsigned char)im->contrast);     return 1; }
    if (!strcmp(k,"saturation")){ IMP_ISP_Tuning_SetSaturation((unsigned char)im->saturation); return 1; }
    if (!strcmp(k,"sharpness")){  IMP_ISP_Tuning_SetSharpness((unsigned char)im->sharpness);   return 1; }
    if (!strcmp(k,"hue")){
#ifdef ISP_HAS_HUE
        IMP_ISP_Tuning_SetBcshHue((unsigned char)im->hue); return 1;
#else
        return 0;
#endif
    }
    if (!strcmp(k,"hflip")){ IMP_ISP_Tuning_SetISPHflip((IMPISPTuningOpsMode)(im->hflip?1:0)); return 1; }
    if (!strcmp(k,"vflip")){ IMP_ISP_Tuning_SetISPVflip((IMPISPTuningOpsMode)(im->vflip?1:0)); return 1; }
    if (!strcmp(k,"running_mode")){
        IMP_ISP_Tuning_SetISPRunningMode(im->running_mode ? IMPISP_RUNNING_MODE_NIGHT
                                                          : IMPISP_RUNNING_MODE_DAY);
        return 1;
    }
    if (!strcmp(k,"anti_flicker")){ /* enum: 0 off, 1 = 50 Hz, 2 = 60 Hz */
        IMP_ISP_Tuning_SetAntiFlickerAttr((IMPISPAntiflickerAttr)im->anti_flicker);
        return 1;
    }
    if (!strcmp(k,"ae_compensation")){
#ifdef ISP_HAS_AECOMP
        IMP_ISP_Tuning_SetAeComp(im->ae_compensation); return 1;
#else
        return 0;
#endif
    }
    if (!strcmp(k,"max_again")){ IMP_ISP_Tuning_SetMaxAgain((uint32_t)im->max_again); return 1; }
    if (!strcmp(k,"max_dgain")){ IMP_ISP_Tuning_SetMaxDgain((uint32_t)im->max_dgain); return 1; }
    if (!strcmp(k,"sinter_strength")){ IMP_ISP_Tuning_SetSinterStrength((uint32_t)im->sinter_strength); return 1; }
    if (!strcmp(k,"temper_strength")){ IMP_ISP_Tuning_SetTemperStrength((uint32_t)im->temper_strength); return 1; }
    if (!strcmp(k,"dpc_strength")){
#ifdef ISP_HAS_DPC
        IMP_ISP_Tuning_SetDPC_Strength((unsigned int)im->dpc_strength); return 1;
#else
        return 0;
#endif
    }
    if (!strcmp(k,"defog_strength")){
#ifdef ISP_HAS_DEFOG
        uint8_t d=(uint8_t)im->defog_strength;
        IMP_ISP_Tuning_SetDefog_Strength(&d); return 1;
#else
        return 0;
#endif
    }
    if (!strcmp(k,"drc_strength")){
#ifdef ISP_HAS_DRC
        IMP_ISP_Tuning_SetDRC_Strength((unsigned int)im->drc_strength); return 1;
#else
        return 0;
#endif
    }
    if (!strcmp(k,"highlight_depress")){ /* 0 disables */
        IMP_ISP_Tuning_SetHiLightDepress((uint32_t)im->highlight_depress); return 1;
    }
    if (!strcmp(k,"backlight_compensation")){
#ifdef ISP_HAS_BACKLIGHT
        IMP_ISP_Tuning_SetBacklightComp((uint32_t)im->backlight_compensation); return 1;
#else
        return 0;
#endif
    }
    /* white balance: mode + gains are one IMPISPWB, applied on any of them */
    if (!strcmp(k,"core_wb_mode")||!strcmp(k,"wb_rgain")||!strcmp(k,"wb_bgain")){
        IMPISPWB wb; memset(&wb,0,sizeof wb);
        wb.mode=(enum isp_core_wb_mode)im->core_wb_mode;
        wb.rgain=(uint16_t)im->wb_rgain; wb.bgain=(uint16_t)im->wb_bgain;
        IMP_ISP_Tuning_SetWB(&wb); return 1;
    }
#endif /* ISP_NEW_TUNING_API */
    return 0;
#endif /* NO_TUNINGS */
}

/* apply the whole image.* (ISP) tuning block (boot + config reload).
 * "core_wb_mode" stands in for the whole WB triple (one SetWB call). */
static void apply_image_tuning(void)
{
#if !defined(NO_TUNINGS)
    static const char *const keys[] = {
        "brightness","contrast","saturation","sharpness","hue",
        "hflip","vflip","running_mode","anti_flicker","ae_compensation",
        "max_again","max_dgain","sinter_strength","temper_strength",
        "dpc_strength","defog_strength","drc_strength","highlight_depress",
        "backlight_compensation","core_wb_mode"
    };
    for (size_t i=0;i<sizeof keys/sizeof keys[0];i++)
        if (!isp_apply_image(keys[i]))
            LOGD(MOD,"image.%s unsupported on this platform (skipped)",keys[i]);
    const ms_image_cfg *im = &g_hcfg->image;
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
    /* on a fast restart the previous instance's IMP/rmem may not be released
     * yet - retry a few times before giving up instead of exiting the daemon */
    {
        int si_tries = 0;
        while (IMP_System_Init() < 0) {
            if (++si_tries >= 5) { LOGE(MOD,"IMP_System_Init failed after %d tries", si_tries); return -1; }
            LOGW(MOD,"IMP_System_Init busy, retry %d/5 in 1s (ISP still releasing?)", si_tries);
            usleep(1000000);
        }
    }
    IMP_ISP_EnableTuning();
    apply_image_tuning();   /* full image.* block incl. running_mode */
#if defined(PLATFORM_T41)
    { IMPISPSensorFps fps={ .num=(uint32_t)g_hcfg->sensor.fps, .den=1 };
      IMP_ISP_Tuning_SetSensorFPS(IMPVI_MAIN,&fps); }
#elif defined(ISP_NEW_TUNING_API)   /* T40 */
    { uint32_t fn=(uint32_t)g_hcfg->sensor.fps, fd=1;
      IMP_ISP_Tuning_SetSensorFPS(IMPVI_MAIN,&fn,&fd); }
#else
    IMP_ISP_Tuning_SetSensorFPS(g_hcfg->sensor.fps, 1);
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
            /* fully idle: block until ing_set_active() wakes us (1 s safety
             * timeout). No polling, no frame flow - the framesource is off. */
            if (!receiving){ act_wait(); continue; }
            /* debounce the stop: only shut the encoder down after a sustained
             * idle period; rapid client churn must not toggle Start/StopRecvPic */
            int64_t now = ms_now_us();
            if (idle_since==0) idle_since = now;
            if (now - idle_since >= MS_IDLE_STOP_US) {
                IMP_Encoder_StopRecvPic(vc->chn);
                fs_unuse(vc->chn);            /* stop the frame flow entirely */
                receiving=0; idle_since=0;
                LOGI(MOD,"video chn%d idle",vc->chn);
                continue;
            }
            /* during the debounce window keep draining the encoder below so
             * the pipeline never backs up (publish is a no-op with 0 subs) */
        } else {
            idle_since = 0;
            if (!receiving){ fs_use(vc->chn);
                             IMP_Encoder_StartRecvPic(vc->chn); receiving=1;
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
    if (receiving){ IMP_Encoder_StopRecvPic(vc->chn); fs_unuse(vc->chn); }
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
            /* fully idle: block until reactivated (see act_wait) */
            if (!receiving){ act_wait(); continue; }
            int64_t nowi = ms_now_us();
            if (idle_since==0) idle_since = nowi;
            if (nowi - idle_since >= MS_IDLE_STOP_US) {
                IMP_Encoder_StopRecvPic(jc->chn);
                fs_unuse(jc->fs_chn);         /* stop the frame flow entirely */
                receiving=0; idle_since=0;
                continue;
            }
        } else idle_since = 0;
        if (!receiving){ fs_use(jc->fs_chn);
                         IMP_Encoder_StartRecvPic(jc->chn); receiving=1; }
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
            const uint8_t *p=(const uint8_t*)(uintptr_t)st.pack[i].virAddr;
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
    if (receiving){ IMP_Encoder_StopRecvPic(jc->chn); fs_unuse(jc->fs_chn); }
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

static void jpeg_chan_start(int chn, int fs_chn, int src, int w, int h,
                            int fps, int snapshot)
{
    jchan *jc=&g_j[g_nj++];
    jc->chn=chn; jc->fs_chn=fs_chn; jc->src=src; jc->w=w; jc->h=h;
    jc->fps=fps>0?fps:5; jc->snapshot=snapshot;
    jc->run=1; jc->active=0;
    if (pthread_create(&jc->thr,NULL,jpeg_thread,jc)!=0){
        jc->run=0; g_nj--;           /* drop the slot: stop() must not join */
        LOGE(MOD,"jpeg chn%d thread create failed",chn);
    }
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
    /* framesource is enabled on demand by jpeg_thread (fs_use/fs_unuse) */
    jpeg_chan_start(chn, chn, HUB_JPEG_SRC, cfg->jpeg.width, cfg->jpeg.height,
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
    jpeg_chan_start(chn, grp, HUB_JPEG_SRC_N(vi), v->width, v->height, v->jpeg_fps, 0);
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

/* Apply one live audio.* key from the current config (g_hcfg->audio) to the
 * running audio input (dev 0 / chn 0, opened by audio_thread). Returns 1 when
 * the key is wired on this PLATFORM's IMP SDK, 0 when the SoC cannot do it
 * (the value is still parsed/persisted by the config layer). The per-SoC
 * guards come from ../audio_caps.h - keep them in sync with the caps.audio
 * list control.c reports. The HPF/AGC/NS hooks are runtime toggles in the
 * libimp capture path (they take the SetPubAttr attr, g_aio). Callers
 * serialize via g_isp_lock (boot-apply runs before live control can race).
 * NOT here: codec/samplerate/bitrate/channels/enabled (+ force_stereo and
 * the spk_* speaker keys - timps has no AO pipeline): those are init-time
 * attributes, persist-only, applied on the next restart. */
static int ai_apply_key(const char *k)
{
    const ms_audio_cfg *a = &g_hcfg->audio;
    if (!strcmp(k,"volume")){ IMP_AI_SetVol(0, 0, a->volume); return 1; }
    if (!strcmp(k,"gain"))  { IMP_AI_SetGain(0, 0, a->gain);  return 1; }
    if (!strcmp(k,"alc_gain")){
#ifdef AUDIO_HAS_ALC_GAIN
        IMP_AI_SetAlcGain(0, 0, a->alc_gain); return 1;
#else
        return 0;
#endif
    }
    if (!strcmp(k,"high_pass")){
        if (a->high_pass) IMP_AI_EnableHpf(&g_aio);
        else              IMP_AI_DisableHpf();
        return 1;
    }
    /* the two agc_* values parameterize the AGC -> re-enable with new config */
    if (!strcmp(k,"agc")||!strcmp(k,"agc_target_dbfs")||!strcmp(k,"agc_compression_db")){
        if (a->agc){
            IMPAudioAgcConfig agc;
            agc.TargetLevelDbfs   = a->agc_target_dbfs;
            agc.CompressionGaindB = a->agc_compression_db;
            IMP_AI_EnableAgc(&g_aio, agc);
        } else IMP_AI_DisableAgc();
        return 1;
    }
    if (!strcmp(k,"ns")){                    /* 0 = off, 1..3 = level */
        if (a->ns > 0) IMP_AI_EnableNs(&g_aio, a->ns);
        else           IMP_AI_DisableNs();
        return 1;
    }
    if (!strcmp(k,"mute"))                   /* live mic mute: no IMP call - */
        return 1;                            /* audio_thread gates the publish
                                              * on g_hcfg->audio.mute per frame */
    return 0;
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
    /* boot-apply the live audio set from the config (same ai_apply_key path
     * the /control endpoint uses at runtime); the off-state features are
     * simply not enabled instead of calling the IMP_AI_Disable* hooks.
     * g_ai_up is raised only afterwards: until then ing_control() leaves the
     * AI alone (the value lands in g_cfg first, so it is picked up here). */
    g_aio = aio;
    ai_apply_key("volume");
    ai_apply_key("gain");
    if (g_hcfg->audio.alc_gain > 0 && !ai_apply_key("alc_gain"))
        LOGW(MOD,"audio.alc_gain unsupported on this platform (ignored)");
    if (g_hcfg->audio.high_pass) ai_apply_key("high_pass");
    if (g_hcfg->audio.agc)       ai_apply_key("agc");
    if (g_hcfg->audio.ns > 0)    ai_apply_key("ns");
    g_ai_up = 1;

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
        if (!g_aactive){ act_wait(); continue; }   /* on-demand: block idle */
        int64_t a_t0 = ms_now_us();
        /* sleep on the no-frame path so a non-blocking/failing PollingFrame
         * can never spin the CPU (audio input may be idle on some boards) */
        if (IMP_AI_PollingFrame(dev,chnid, g_hcfg->imp_polling_timeout)!=0){ usleep(10000); continue; }
        IMPAudioFrame frm;
        if (IMP_AI_GetFrame(dev,chnid,&frm,1)!=0) continue;
        /* live mic mute (audio.mute via /control): keep draining the AI so
         * nothing backs up, but never feed the encoder/hub - RTSP and MP4
         * clients simply receive no audio frames until unmuted */
        if (g_hcfg->audio.mute){
            IMP_AI_ReleaseFrame(dev,chnid,&frm);
            int64_t m_dt = ms_now_us() - a_t0;
            if (m_dt < 15000) usleep(15000 - m_dt);
            continue;
        }
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
    g_ai_up = 0;
    IMP_AI_DisableChn(dev,chnid);
    IMP_AI_Disable(dev);
    return NULL;
}

#ifdef USE_CONTROL
/* Apply a live setting from /control (via hub_control). Keys are the config
 * file keys: image.*, audio.* (live: volume/gain/alc_gain/high_pass/agc/
 * agc_target_dbfs/agc_compression_db/ns), osdS.N.* (per-stream) and legacy
 * osdN.* (all streams). The value arrives as a string; numbers are parsed
 * here. ISP/AI calls are runtime-safe but serialized under a mutex; OSD goes
 * through imp_osd_apply(stream,item). ALL videoN.*
 * and sensor.* keys plus the attribute-level audio keys (enabled/codec/
 * samplerate/bitrate/channels/force_stereo/spk_*) are NOT applied live
 * (persisted only, take effect on restart). Compiled only with
 * -DUSE_CONTROL. */
static pthread_mutex_t g_isp_lock = PTHREAD_MUTEX_INITIALIZER;
static void ing_control(const char *key, const char *val)
{
    int v = (int)strtol(val, NULL, 0);

    if (!strncmp(key,"image.",6)){
        /* the control layer already stored the value in g_cfg (config_apply_kv
         * runs before hub_control), so the HAL applies from the config */
        const char *k = key+6;
        pthread_mutex_lock(&g_isp_lock);
        int ok = isp_apply_image(k);
        pthread_mutex_unlock(&g_isp_lock);
        if (ok) LOGI(MOD,"control %s=%d", key, v);
        else    LOGD(MOD,"image.%s unsupported on this platform (persisted only)", k);
        return;
    }

    if (!strncmp(key,"audio.",6)){
        const char *k = key+6;
        /* persist-only keys: encoder/SetPubAttr-level attributes (plus the
         * speaker/stereo keys timps has no runtime path for). The control
         * layer already stored them in g_cfg and persists them to the config
         * file; they take effect when timps is restarted. */
        if (!strcmp(k,"enabled")   || !strcmp(k,"codec")    ||
            !strcmp(k,"samplerate")|| !strcmp(k,"bitrate")  ||
            !strcmp(k,"channels")  || !strcmp(k,"force_stereo") ||
            !strncmp(k,"spk_",4)){
            LOGI(MOD,"%s persisted, applies on restart", key);
            return;
        }
        if (!g_ai_up){                         /* audio input not running */
            LOGD(MOD,"%s persisted (audio input not running)", key);
            return;
        }
        pthread_mutex_lock(&g_isp_lock);       /* dev 0 / chn 0 as in audio_thread */
        int ok = ai_apply_key(k);
        pthread_mutex_unlock(&g_isp_lock);
        if (ok) LOGI(MOD,"control %s=%d", key, v);
        else    LOGD(MOD,"audio.%s unsupported on this platform (persisted only)", k);
        return;
    }

    /* videoN.* / sensor.*: encoder, FrameSource and sensor settings are
     * config-only - never applied live (a live change would need a stream-
     * killing channel/ISP reconfig). The control layer already stored and
     * persisted the value; it takes effect on the next daemon restart. */
    if ((!strncmp(key,"video",5) && key[5]>='0' && key[5]<'0'+MS_MAX_VSTREAM
         && key[6]=='.') || !strncmp(key,"sensor.",7)){
        LOGI(MOD,"%s persisted, applies on restart", key);
        return;
    }

    /* daynight.*: config-only (the detection thread polls g_cfg), no HAL
     * action - the actual ISP mode change comes in as image.running_mode
     * via the board's color script. */
    if (!strncmp(key,"daynight.",9)) return;

    /* motion.*: enabled/cols/rows/sensitivity/monitor_stream are applied
     * LIVE by cleanly stopping and recreating the IVS grid (move params are
     * create-time attributes - see motion_sync). cooldown_ms/on_motion are
     * config-only: the polling thread reads them from g_cfg per event. */
    if (!strncmp(key,"motion.",7)){
        const char *k = key+7;
        if (!strcmp(k,"enabled") || !strcmp(k,"cols") || !strcmp(k,"rows") ||
            !strcmp(k,"sensitivity") || !strcmp(k,"monitor_stream")){
            motion_sync(g_hcfg);
            LOGI(MOD,"control %s applied (IVS grid re-synced)", key);
        }
        return;
    }

    /* osdS.N.* (per-stream) / legacy osdN.* (all streams): config (g_cfg) is
     * already updated -> re-apply the whole item on the right stream(s) */
    if (!strncmp(key,"osd",3) && key[3]>='0' && key[3]<'0'+MS_MAX_OSD && key[4]=='.'){
        if (key[5]>='0' && key[5]<'0'+MS_MAX_OSD && key[6]=='.' &&
            key[3]<'0'+MS_MAX_VSTREAM)
            imp_osd_apply(key[3]-'0', key[5]-'0');   /* osdS.N.* */
        else
            imp_osd_apply(-1, key[3]-'0');           /* legacy osdN.* */
        return;
    }

    /* privacy<S>.<N>.* cover masks: config (g_cfg) already updated -> re-apply
     * the region LIVE (create/show/hide/move) on that stream */
    if (!strncmp(key,"privacy",7) && key[7]>='0' && key[7]<'0'+MS_MAX_VSTREAM &&
        key[8]=='.' && key[9]>='0' && key[9]<'0'+MS_MAX_PRIVACY && key[10]=='.'){
        imp_osd_privacy_apply(key[7]-'0', key[9]-'0');
        return;
    }

    /* osd.* (master switch/global font/vars file): config-only - the OSD
     * groups are built once in imp_osd_setup at startup, so these take
     * effect on the next daemon restart */
    if (!strncmp(key,"osd.",4)){
        LOGI(MOD,"%s persisted, applies on restart", key);
        return;
    }
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
        /* NOT enabled here: the framesource runs on demand (fs_use/fs_unuse
         * from the consumer threads) so an idle timps pumps no frames at all */

        hub_set_video_params(i, v->codec, v->width, v->height, v->fps);
        g_v[g_nv].chn=chn; g_v[g_nv].grp=grp; g_v[g_nv].codec=v->codec;
        g_v[g_nv].w=v->width; g_v[g_nv].h=v->height; g_v[g_nv].run=1;
        /* count the slot only with a live thread: ing_stop must never join a
         * pthread_t that was never created */
        if (pthread_create(&g_v[g_nv].thr,NULL,video_thread,&g_v[g_nv])==0) g_nv++;
        else { g_v[g_nv].run=0; LOGE(MOD,"video chn%d thread create failed",chn); }
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
        if (pthread_create(&g_athr,NULL,audio_thread,NULL)!=0){
            g_arun=0;                /* had_audio stays 0 -> no join in stop */
            LOGE(MOD,"audio thread create failed");
        }
    }

    if (cfg->jpeg.enabled) jpeg_setup(cfg);
    motion_sync(cfg);      /* start the IVS motion grid if motion.enabled */
    return 0;
}

static void ing_set_active(int src, int on)
{
    if (src==HUB_AUDIO_SRC){ g_aactive=on; }
    else if (src>=HUB_JPEG_SRC && src<HUB_JPEG_SRC+HUB_NJPEG){
        for (int i=0;i<g_nj;i++) if (g_j[i].src==src){ g_j[i].active=on; break; }
    } else {
        for (int i=0;i<g_nv;i++) if (g_v[i].chn==src){ g_v[i].active=on; break; }
    }
    if (on) act_wake();   /* unblock idle producer threads immediately */
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
    /* stop the IVS motion grid (uses the pinned channel recorded at start,
     * so a runtime monitor_stream change can never unpin the wrong FS) */
    pthread_mutex_lock(&g_motion_mtx);
    if (g_motion_pin >= 0){
        imp_motion_stop();
        fs_unuse(g_motion_pin);
        g_motion_pin = -1;
    }
    pthread_mutex_unlock(&g_motion_mtx);
    /* raise all stop flags first, then wake the idle-blocked threads once so
     * every join returns promptly (act_wait also times out after 1 s) */
    int had_audio = g_arun;
    for (int i=0;i<g_nv;i++) g_v[i].run=0;
    for (int i=0;i<g_nj;i++) g_j[i].run=0;
    g_arun=0;
    act_wake();
    for (int i=0;i<g_nv;i++) pthread_join(g_v[i].thr,NULL);
    if (had_audio) pthread_join(g_athr,NULL);
    for (int i=0;i<g_nj;i++) pthread_join(g_j[i].thr,NULL);
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
