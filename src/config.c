#include "config.h"
#include "log.h"
#include "motion_caps.h"   /* MOTION_MAX_CELLS/MOTION_CELL_LIMIT (grid clamp) */
#include "rotate_caps.h"   /* ROT_HAS_90/ROT_HAS_180 (rotation whitelist) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>   /* fchmod on the mkstemp'd config tmp */
#include <errno.h>

#define MOD "CONFIG"
ms_config g_cfg;
const char *g_cfg_path = NULL;   /* config file in use, set by config_load() */

/* see config.h: guards runtime g_cfg string mutation vs concurrent readers.
 *
 * Lock-protected fields (written under this lock by config_apply_kv()'s
 * copystr(), via control.c's /control POST handling - every read of them,
 * direct struct access or through config_get_kv() below, must also happen
 * under the lock, or a reader can observe a torn/non-terminated string
 * mid-strncpy and run strlen() off the end of it):
 *   - osd.items[][].text            (osd<S>.<N>.text / legacy osd<N>.text)
 *   - video[].rtsp_path             (video<N>.rtsp_path)
 *   - sensor.model                  (sensor.model)
 *   - record.dir, record.name       (record.dir / record.name)
 *   - timelapse.dir, timelapse.name (timelapse.dir / timelapse.name)
 * Everything else in g_cfg is either an int/enum (aligned word reads, no
 * tearing) or only ever written at startup (config_load/
 * config_sensor_finalize, single-threaded before any other thread runs), so
 * it needs no lock.
 *
 * Recursive on purpose: config_get_kv() takes this lock itself around the
 * fields above so callers outside this file (OSD updater, recorder, RTSP
 * path match, GET /control, ...) get safe reads without having to know the
 * convention, but control.c's timps_apply_setting() ALSO wraps a whole
 * get-apply-get sequence in this same lock (for atomic change-detection) -
 * a plain, non-recursive mutex would self-deadlock in that nested case. */
static pthread_mutex_t g_str_lock;
static pthread_once_t  g_str_lock_once = PTHREAD_ONCE_INIT;
static void g_str_lock_init(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_str_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}
void config_str_lock(void)   { pthread_once(&g_str_lock_once, g_str_lock_init); pthread_mutex_lock(&g_str_lock); }
void config_str_unlock(void) { pthread_mutex_unlock(&g_str_lock); }

static void copystr(char *dst, const char *src, size_t n)
{
    strncpy(dst, src, n-1); dst[n-1]=0;
}

static int  pbool(const char *v){ return (!strcasecmp(v,"1")||!strcasecmp(v,"true")||!strcasecmp(v,"on")||!strcasecmp(v,"yes")); }
static int  pint(const char *v){ return (int)strtol(v, NULL, 0); }
/* M11: pint with a documented sane range. Values a broken client/script
 * persists via /control used to reach the HAL unchecked - a nonsense fps/
 * width/port makes HAL init fail, main exit and the respawn loop crash
 * forever. Clamping (as the motion and osd.supersample keys already do)
 * keeps a bad value from bricking the stream; the clamped value is what is
 * read back and persisted, so /control dedup stays idempotent. */
static int  pint_cl(const char *v, int lo, int hi)
{
    int x = pint(v);
    if (x < lo) x = lo;
    if (x > hi) x = hi;
    return x;
}
/* rotation parser: accepts degrees (0/90/180/270) and the legacy T31 rotTo90
 * enum (0/1/2 -> 0/90/270), then whitelists against this SoC's caps. Anything
 * unsupported coerces to 0 (with a warning) so the streamer stays behaviour-
 * neutral until an apply path exists. */
static int prot(const char *val){
    int r = pint(val);
    if (r==1) r=90; else if (r==2) r=270;        /* legacy T31 rotTo90 0/1/2 */
    if (r!=0 && r!=90 && r!=180 && r!=270){ LOGW(MOD,"rotation %d invalid -> 0",r); return 0; }
#ifndef ROT_HAS_90
    if (r==90||r==270){ LOGW(MOD,"rotation %d unsupported on this SoC -> 0",r); return 0; }
#endif
#ifndef ROT_HAS_180
    if (r==180){ LOGW(MOD,"rotation 180 unsupported -> 0"); return 0; }
#endif
    return r;
}
static float pflt(const char *v){ return (float)strtod(v, NULL); }
static uint32_t phex(const char *v){ return (uint32_t)strtoul(v, NULL, 0); }
static int  pvcodec(const char *v){ return (!strcasecmp(v,"h265")||!strcasecmp(v,"hevc")) ? MS_VC_H265 : MS_VC_H264; }
static int  pacodec(const char *v){
    if (!strcasecmp(v,"aac")) return MS_AC_AAC;
    if (!strcasecmp(v,"pcmu")||!strcasecmp(v,"g711u")||!strcasecmp(v,"ulaw")) return MS_AC_PCMU;
    if (!strcasecmp(v,"pcma")||!strcasecmp(v,"g711a")||!strcasecmp(v,"alaw")) return MS_AC_PCMA;
    if (!strcasecmp(v,"none")||!strcasecmp(v,"off")) return MS_AC_NONE;
    return MS_AC_AAC;
}
/* canonical config-file spelling of an audio codec (inverse of pacodec) */
static const char *acodec_name(int c){
    switch (c){
        case MS_AC_AAC:  return "aac";
        case MS_AC_PCMU: return "pcmu";
        case MS_AC_PCMA: return "pcma";
        default:         return "none";
    }
}
/* canonical config-file spelling of a video codec (inverse of pvcodec) */
static const char *vcodec_name(int c){
    return (c==MS_VC_H265) ? "h265" : "h264";
}
static int prc(const char *v){
    if(!strcasecmp(v,"cbr"))return MS_RC_CBR;
    if(!strcasecmp(v,"vbr"))return MS_RC_VBR;
    if(!strcasecmp(v,"fixqp"))return MS_RC_FIXQP;
    if(!strcasecmp(v,"smart"))return MS_RC_SMART;
    if(!strcasecmp(v,"capped_vbr"))return MS_RC_CAPPED_VBR;
    if(!strcasecmp(v,"capped_quality"))return MS_RC_CAPPED_QUALITY;
    return MS_RC_CBR;
}
/* canonical config-file spelling of a rate-control mode (inverse of prc) */
static const char *rc_name(int m){
    switch (m){
        case MS_RC_VBR:            return "vbr";
        case MS_RC_FIXQP:          return "fixqp";
        case MS_RC_SMART:          return "smart";
        case MS_RC_CAPPED_VBR:     return "capped_vbr";
        case MS_RC_CAPPED_QUALITY: return "capped_quality";
        default:                   return "cbr";
    }
}

void config_defaults(ms_config *c)
{
    memset(c, 0, sizeof(*c));
    c->loglevel = LOG_INFO;
    c->imp_polling_timeout = 500;
    c->osd_pool_size = 1024;   /* max on T-series; holds small OSD regions */

    /* sensor.* start UNSET so config_sensor_finalize() can auto-detect them
     * from /proc/jz/sensor/sensor0/ (raptor/prudynt style); a config value or,
     * failing that, a safe fallback fills whatever the sensor registry lacks */
    c->sensor.model[0] = 0;
    c->sensor.i2c_addr = 0;
    c->sensor.fps = 0;
    c->sensor.width = 0;
    c->sensor.height = 0;

    /* ISP image defaults (128 = neutral, like the old streamer) */
    ms_image_cfg *im = &c->image;
    im->brightness=128; im->contrast=128; im->saturation=128; im->sharpness=128; im->hue=128;
    im->vflip=0; im->hflip=0; im->running_mode=0; im->anti_flicker=2; im->ae_compensation=128;
    im->max_again=160; im->max_dgain=80;
    im->sinter_strength=128; im->temper_strength=128; im->dpc_strength=128;
    im->defog_strength=128; im->drc_strength=128;
    im->highlight_depress=0; im->backlight_compensation=0;
    im->core_wb_mode=0; im->wb_rgain=0; im->wb_bgain=0;

    c->rtsp_enabled = 1; c->rtsp_port = 554; c->rtsp_user[0]=0; c->rtsp_pass[0]=0;
    c->http_enabled = 1; c->http_port = 8880; c->http_preview_chn = 1;
    c->http_user[0]=0; c->http_pass[0]=0;
    c->http_token[0]=0;
    copystr(c->http_token_file, "/run/timps.token", sizeof c->http_token_file);
    c->events_enabled=1; c->events_stats_ms=2000; c->events_max_clients=8;
    /* optional TLS (USE_TLS builds): off by default */
    c->http_https=0;
    /* reuse thingino's httpd cert (mbedtls-certgen writes it here); S95timps
     * generates it on first boot when https is enabled and it is missing */
    copystr(c->http_tls_cert,"/etc/ssl/certs/httpd.crt",128);
    copystr(c->http_tls_key,"/etc/ssl/private/httpd.key",128);
    c->rtsp_tls=0; c->rtsp_tls_port=322;
    /* optional SRT output (USE_SRT builds): off by default */
    c->srt.enabled=0; c->srt.port=9000; c->srt.channel=0; c->srt.latency_ms=120;
    c->srt.streamid[0]=0; c->srt.passphrase[0]=0;

    for (int i=0;i<MS_MAX_VSTREAM;i++){
        ms_vstream_cfg *v=&c->video[i];
        v->codec=MS_VC_H264; v->fps=25; v->rc_mode=MS_RC_CBR;
        v->gop=50; v->max_gop=60; v->profile=2; v->qp=35; v->min_qp=20; v->max_qp=45;
        v->rotation=0; v->buffers=2; v->imp_chn=i;
        /* piggyback JPEG encoder: on by default (snapshot.jpg/MJPEG preview
         * and the thingino WebUI thumbnail both expect it to just work).
         * channels 0..MS_MAX_VSTREAM-1 = video, MS_MAX_VSTREAM = dedicated
         * jpeg channel, so the piggyback encoders start after those. */
        v->jpeg_enabled=1; v->jpeg_quality=75; v->jpeg_fps=5;
        v->jpeg_chn=MS_MAX_VSTREAM+1+i;
    }
    c->video[0].enabled=1; c->video[0].width=1920; c->video[0].height=1080;
    c->video[0].bitrate_kbps=3000; copystr(c->video[0].rtsp_path,"/ch0",MS_MAX_STR);
    c->video[1].enabled=1; c->video[1].width=640; c->video[1].height=360;
    c->video[1].bitrate_kbps=512; copystr(c->video[1].rtsp_path,"/ch1",MS_MAX_STR);

    c->audio.enabled=1; c->audio.codec=MS_AC_AAC; c->audio.samplerate=16000;
    c->audio.channels=1; c->audio.bitrate_kbps=32;
    c->audio.volume=80; c->audio.gain=25;   /* audible defaults */
    c->audio.high_pass=0; c->audio.agc=0; c->audio.ns=0;
    c->audio.alc_gain=0;                                   /* PGA off */
    c->audio.agc_target_dbfs=10; c->audio.agc_compression_db=0;
    c->audio.mute=0;                                       /* mic live */
    c->audio.force_stereo=0;
    c->audio.spk_enabled=0; c->audio.spk_volume=80; c->audio.spk_gain=25;
    c->audio.backchannel=0; c->audio.backchannel_codec=0; c->audio.backchannel_rate=16000;

    c->jpeg.enabled=0; c->jpeg.width=640; c->jpeg.height=360;
    c->jpeg.quality=75; c->jpeg.fps=5; c->jpeg.imp_chn=2;
    c->jpeg.snapshot_path[0]=0;

    /* OSD: per-stream arrays of overlays; same sensible default layout on
     * every stream (time / hostname / uptime / logo) */
    c->osd.enabled=1; c->osd.monitor_stream=0; c->osd.supersample=2;
    copystr(c->osd.font_path,"/usr/share/fonts/default.ttf",128);
    copystr(c->osd.vars_file,"/tmp/timps_osd.vars",128);
    for (int s=0;s<MS_MAX_VSTREAM;s++){
        ms_osd_item *it=c->osd.items[s];
        for (int i=0;i<MS_MAX_OSD;i++){
            ms_osd_item *o=&it[i];
            o->enabled=0; o->type=MS_OSD_TEXT; o->x=10; o->y=10;
            /* font_size is absolute px (no per-stream auto-scale): main stream
             * 32, sub-streams a smaller default that still stays legible */
            o->font_size=(s==0)?32:24; o->color=0xFFFFFFFF; o->transparency=255;
            /* text outline ON by default (1px solid-black stroke) so overlays
             * stay readable on light backgrounds without extra config */
            o->outline=1; o->outline_color=0xFF000000;
        }
        /* default layout per stream. x/y: 0 = centered,
         * positive = from left/top, negative = from right/bottom. */
        it[0].enabled=1; it[0].x=10;  it[0].y=10;   /* top-left   */
        copystr(it[0].text,"%Y-%m-%d %H:%M:%S",128);
        it[1].enabled=1; it[1].x=0;   it[1].y=10;   /* top-center */
        copystr(it[1].text,"{hostname}",128);
        it[2].enabled=1; it[2].x=-10; it[2].y=10;   /* top-right  */
        copystr(it[2].text,"{uptime}",128);
        it[3].enabled=1; it[3].type=MS_OSD_LOGO;
        it[3].x=-10; it[3].y=-10;                   /* bottom-right */
        copystr(it[3].logo_path,"/usr/share/images/thingino_100x30.bgra",128);
        it[3].logo_w=100; it[3].logo_h=30;
    }

    /* privacy cover masks: all off by default; default fill = opaque black so
     * enabling a region without setting a color still masks the area */
    for (int s=0;s<MS_MAX_VSTREAM;s++)
        for (int i=0;i<MS_MAX_PRIVACY;i++)
            c->privacy[s][i].color=0xFF000000;

    c->motion.enabled=0; c->motion.monitor_stream=0; c->motion.sensitivity=128;
    c->motion.cooldown_ms=5000; c->motion.hold_ms=800; c->motion.skip_frames=5;
    /* detection grid default: 5x5 where the SDK's ROI budget allows it,
     * 2x2 on small-budget SDKs (T10/T20 3.9.0: MOTION_MAX_CELLS = 4) */
#if MOTION_MAX_CELLS >= 25
    c->motion.cols=5; c->motion.rows=5;
#elif MOTION_MAX_CELLS >= 4
    c->motion.cols=2; c->motion.rows=2;
#else
    c->motion.cols=1; c->motion.rows=1;
#endif

    /* local recording (fMP4 to SD): off by default; thingino path conventions
     * (SD at /mnt/mmcblk0p1, <host>/records/ tree, strftime segment names),
     * motion-triggered with a short pre/post roll like raptor's RMR */
    c->record.enabled=0; c->record.channel=0; c->record.mode=1;
    copystr(c->record.dir,"/mnt/mmcblk0p1",sizeof c->record.dir);
    copystr(c->record.name,"%Y%m%d/%H/%Y%m%dT%H%M%S",sizeof c->record.name);
    c->record.segment_s=60; c->record.pre_roll_s=3; c->record.post_roll_s=10;
    c->record.min_free_mb=200; c->record.audio=1;

    /* native timelapse (periodic JPEG snapshots): off by default; same path
     * conventions as the recorder (<dir>/<host>/timelapses/ tree) */
    c->timelapse.enabled=0; c->timelapse.channel=0;
    copystr(c->timelapse.dir,"/mnt/mmcblk0p1",sizeof c->timelapse.dir);
    copystr(c->timelapse.name,"%Y%m%d/%H/%Y%m%dT%H%M%S",sizeof c->timelapse.name);
    c->timelapse.interval_s=60; c->timelapse.keep_days=7;

    /* automatic day/night: gain thresholds mirror prudynt (day 300, night
     * 3000), the brightness fallback mirrors thingino's daynightd.json */
    c->daynight.enabled=1;
    c->daynight.total_gain_day_threshold=300.0f;
    c->daynight.total_gain_night_threshold=3000.0f;
    c->daynight.threshold_low=25.0f; c->daynight.threshold_high=75.0f;
    c->daynight.hysteresis=0.1f;
    c->daynight.day_gain_pct=60; c->daynight.baseline_delay_s=30;
    c->daynight.interval_ms=500; c->daynight.transition_s=5;
    copystr(c->daynight.switch_cmd,"daynight",sizeof c->daynight.switch_cmd);
    copystr(c->daynight.isp_path,"/proc/jz/isp/isp-m0",sizeof c->daynight.isp_path);

    c->sim_video0[0]=0; c->sim_video1[0]=0; c->sim_audio[0]=0;
}

/* Parse an OSD item key and return a pointer to its field name, or NULL.
 * Canonical per-stream form:  "osd<S>.<N>.<field>"  -> *stream=S, *item=N
 * Legacy shared form:         "osd<N>.<field>"      -> *stream=-1 (all
 * streams), *item=N. Kept so pre-per-stream configs still load. */
static const char *osd_key(const char *key, int *stream, int *item)
{
    if (strncmp(key,"osd",3) || key[3]<'0' || key[3]>'0'+MS_MAX_OSD-1 ||
        key[4]!='.')
        return NULL;
    int a = key[3]-'0';
    if (key[5]>='0' && key[5]<='0'+MS_MAX_OSD-1 && key[6]=='.'){
        /* per-stream: first digit is the stream index */
        if (a >= MS_MAX_VSTREAM) return NULL;
        *stream = a; *item = key[5]-'0';
        return key+7;
    }
    *stream = -1; *item = a;      /* legacy: item index, applies to all */
    return key+5;
}

static void set_video(ms_vstream_cfg *v, const char *k, const char *val)
{
    /* M11: numeric ranges clamped on parse (conservative bounds well past
     * anything a T-series SoC supports, so no legitimate config is altered):
     * width/height 64..4096 px, fps 1..120, bitrate 16..50000 kbps,
     * gop/max_gop 1..1000 frames, profile 0..2, qp 1..51 (H.264/H.265 QP
     * range), buffers 1..8 VBs. */
    if (!strcmp(k,"enabled")) v->enabled=pbool(val);
    else if (!strcmp(k,"codec")) v->codec=pvcodec(val);
    else if (!strcmp(k,"width")) v->width=pint_cl(val,64,4096);
    else if (!strcmp(k,"height")) v->height=pint_cl(val,64,4096);
    else if (!strcmp(k,"fps")) v->fps=pint_cl(val,1,120);
    else if (!strcmp(k,"bitrate")) v->bitrate_kbps=pint_cl(val,16,50000);
    else if (!strcmp(k,"rc_mode")||!strcmp(k,"mode")) v->rc_mode=prc(val);
    else if (!strcmp(k,"gop")) v->gop=pint_cl(val,1,1000);
    else if (!strcmp(k,"max_gop")) v->max_gop=pint_cl(val,1,1000);
    else if (!strcmp(k,"profile")) v->profile=pint_cl(val,0,2);
    else if (!strcmp(k,"qp")) v->qp=pint_cl(val,1,51);
    else if (!strcmp(k,"min_qp")) v->min_qp=pint_cl(val,1,51);
    else if (!strcmp(k,"max_qp")) v->max_qp=pint_cl(val,1,51);
    else if (!strcmp(k,"rotation")) v->rotation=prot(val);
    else if (!strcmp(k,"buffers")) v->buffers=pint_cl(val,1,8);
    else if (!strcmp(k,"rtsp_path")) copystr(v->rtsp_path,val,MS_MAX_STR);
    else if (!strcmp(k,"imp_chn")) v->imp_chn=pint(val);
    else if (!strcmp(k,"jpeg")||!strcmp(k,"jpeg_enabled")) v->jpeg_enabled=pbool(val);
    else if (!strcmp(k,"jpeg_quality")) v->jpeg_quality=pint_cl(val,1,100);
    else if (!strcmp(k,"jpeg_fps")) v->jpeg_fps=pint_cl(val,1,120);
    else if (!strcmp(k,"jpeg_chn")) v->jpeg_chn=pint(val);
    else LOGW(MOD,"unknown video key %s", k);
}

static void set_osd_item(ms_osd_item *o, const char *k, const char *val)
{
    if (!strcmp(k,"enabled")) o->enabled=pbool(val);
    else if (!strcmp(k,"type")) o->type=(!strcasecmp(val,"logo"))?MS_OSD_LOGO:MS_OSD_TEXT;
    else if (!strcmp(k,"text")) copystr(o->text,val,128);
    else if (!strcmp(k,"logo")||!strcmp(k,"logo_path")) copystr(o->logo_path,val,128);
    else if (!strcmp(k,"logo_w")||!strcmp(k,"logo_width")) o->logo_w=pint(val);
    else if (!strcmp(k,"logo_h")||!strcmp(k,"logo_height")) o->logo_h=pint(val);
    else if (!strcmp(k,"x")) o->x=pint(val);
    else if (!strcmp(k,"y")) o->y=pint(val);
    /* H4: font_size feeds the OSD canvas allocation (msttf_render); clamp at
     * parse so a bad /control write can never request an absurd raster. The
     * rasterizer additionally hard-clamps its own pixel height (8..512). */
    else if (!strcmp(k,"font_size")) o->font_size=pint_cl(val,8,256);
    else if (!strcmp(k,"color")||!strcmp(k,"font_color")) o->color=phex(val);
    /* imp_osd.c feeds this straight into the group attr's uint8_t fgAlhpa:
     * clamp so e.g. 300 doesn't wrap to 44 while the config echoes 300 */
    else if (!strcmp(k,"transparency")) o->transparency=pint_cl(val,0,255);
    else if (!strcmp(k,"outline")||!strcmp(k,"stroke")) o->outline=pint(val);
    else if (!strcmp(k,"outline_color")||!strcmp(k,"stroke_color")) o->outline_color=phex(val);
    else if (!strcmp(k,"font_path")) copystr(o->font_path,val,128);
    else LOGW(MOD,"unknown osd item key %s", k);
}

/* Parse a privacy region key "privacy<S>.<N>.<field>" -> *stream=S, *item=N,
 * returns the field name, or NULL. */
static const char *privacy_key(const char *key, int *stream, int *item)
{
    if (strncmp(key,"privacy",7) || key[7]<'0' || key[7]>'0'+MS_MAX_VSTREAM-1 ||
        key[8]!='.' || key[9]<'0' || key[9]>'0'+MS_MAX_PRIVACY-1 || key[10]!='.')
        return NULL;
    *stream = key[7]-'0'; *item = key[9]-'0';
    return key+11;
}

static void set_privacy_region(ms_privacy_region *p, const char *k, const char *val)
{
    if (!strcmp(k,"enabled")) p->enabled=pbool(val);
    else if (!strcmp(k,"x")) p->x=pint(val);
    else if (!strcmp(k,"y")) p->y=pint(val);
    else if (!strcmp(k,"w")||!strcmp(k,"width")) p->w=pint(val);
    else if (!strcmp(k,"h")||!strcmp(k,"height")) p->h=pint(val);
    else if (!strcmp(k,"color")||!strcmp(k,"fill_color")) p->color=phex(val);
    else LOGW(MOD,"unknown privacy key %s", k);
}

static void set_kv(ms_config *c, const char *key, const char *val)
{
    int osi, oii;
    const char *ok = osd_key(key, &osi, &oii);
    if (ok){
        if (osi>=0) set_osd_item(&c->osd.items[osi][oii], ok, val);
        else        /* legacy osdN.*: mirror onto every stream's item N */
            for (int s=0;s<MS_MAX_VSTREAM;s++)
                set_osd_item(&c->osd.items[s][oii], ok, val);
        return;
    }

    int psi, pii;
    const char *pk = privacy_key(key, &psi, &pii);
    if (pk){ set_privacy_region(&c->privacy[psi][pii], pk, val); return; }

    if (!strncmp(key,"video0.",7)){ set_video(&c->video[0], key+7, val); return; }
    if (!strncmp(key,"video1.",7)){ set_video(&c->video[1], key+7, val); return; }

    if (!strncmp(key,"sensor.",7)){
        const char *k=key+7;
        if(!strcmp(k,"model"))copystr(c->sensor.model,val,MS_MAX_STR);
        else if(!strcmp(k,"i2c_addr")||!strcmp(k,"i2c_address"))c->sensor.i2c_addr=pint(val);
        else if(!strcmp(k,"fps"))c->sensor.fps=pint(val);
        else if(!strcmp(k,"width"))c->sensor.width=pint(val);
        else if(!strcmp(k,"height"))c->sensor.height=pint(val);
        else LOGW(MOD,"unknown key %s",key);
        return;
    }
    if (!strncmp(key,"image.",6)){
        /* continuous ISP knobs clamped at parse to the IMP consumer's domain
         * (hal_ingenic.c casts them to unsigned char / uint32_t / uint16_t):
         * without the clamp an out-of-range value wraps at the cast while the
         * config keeps echoing the raw number - applied != persisted.
         * Ranges from the T31/T23 imp_isp.h docs: 0..255 for the uchar knobs
         * and the strength/gain fields, 0..10 for highlight_depress/
         * backlight_compensation ([0-10], 0 = off), 0..65535 for the uint16_t
         * manual WB gains. Enums/bools (running_mode/anti_flicker/
         * core_wb_mode/hflip/vflip) are validated by their consumers. */
        const char *k=key+6; ms_image_cfg *m=&c->image;
        if(!strcmp(k,"brightness"))m->brightness=pint_cl(val,0,255);
        else if(!strcmp(k,"contrast"))m->contrast=pint_cl(val,0,255);
        else if(!strcmp(k,"saturation"))m->saturation=pint_cl(val,0,255);
        else if(!strcmp(k,"sharpness"))m->sharpness=pint_cl(val,0,255);
        else if(!strcmp(k,"hue"))m->hue=pint_cl(val,0,255);
        else if(!strcmp(k,"vflip"))m->vflip=pbool(val);
        else if(!strcmp(k,"hflip"))m->hflip=pbool(val);
        else if(!strcmp(k,"running_mode"))m->running_mode=pint(val);
        else if(!strcmp(k,"anti_flicker"))m->anti_flicker=pint(val);
        else if(!strcmp(k,"ae_compensation"))m->ae_compensation=pint_cl(val,0,255);
        else if(!strcmp(k,"max_again"))m->max_again=pint_cl(val,0,255);
        else if(!strcmp(k,"max_dgain"))m->max_dgain=pint_cl(val,0,255);
        else if(!strcmp(k,"sinter_strength"))m->sinter_strength=pint_cl(val,0,255);
        else if(!strcmp(k,"temper_strength"))m->temper_strength=pint_cl(val,0,255);
        else if(!strcmp(k,"dpc_strength"))m->dpc_strength=pint_cl(val,0,255);
        else if(!strcmp(k,"defog_strength"))m->defog_strength=pint_cl(val,0,255);
        else if(!strcmp(k,"drc_strength"))m->drc_strength=pint_cl(val,0,255);
        else if(!strcmp(k,"highlight_depress"))m->highlight_depress=pint_cl(val,0,10);
        else if(!strcmp(k,"backlight_compensation"))m->backlight_compensation=pint_cl(val,0,10);
        else if(!strcmp(k,"core_wb_mode"))m->core_wb_mode=pint(val);
        else if(!strcmp(k,"wb_rgain"))m->wb_rgain=pint_cl(val,0,65535);
        else if(!strcmp(k,"wb_bgain"))m->wb_bgain=pint_cl(val,0,65535);
        else LOGW(MOD,"unknown key %s",key);
        return;
    }
    if (!strncmp(key,"audio.",6)){
        const char *k=key+6;
        if(!strcmp(k,"enabled"))c->audio.enabled=pbool(val);
        else if(!strcmp(k,"codec"))c->audio.codec=pacodec(val);
        else if(!strcmp(k,"samplerate"))c->audio.samplerate=pint(val);
        /* 1 = mono (native), 2 = simulated stereo (mono mic duplicated to
         * L=R, AAC only) - anything else would put a bogus channel count in
         * the AAC ASC / SDP / fMP4 stsd */
        else if(!strcmp(k,"channels"))c->audio.channels=pint_cl(val,1,2);
        else if(!strcmp(k,"bitrate"))c->audio.bitrate_kbps=pint_cl(val,8,320);
        else if(!strcmp(k,"volume"))c->audio.volume=pint(val);
        else if(!strcmp(k,"gain"))c->audio.gain=pint(val);
        else if(!strcmp(k,"high_pass"))c->audio.high_pass=pbool(val);
        else if(!strcmp(k,"agc"))c->audio.agc=pbool(val);
        /* clamped to the IMP domains (imp_audio.h): IMP_AI_EnableNs mode
         * [0-3], IMPAudioAgcConfig TargetLevelDbfs [0-31] /
         * CompressionGaindB [0-90] - out-of-range used to be silently
         * rejected by libimp while the config kept the raw value */
        else if(!strcmp(k,"ns"))c->audio.ns=pint_cl(val,0,3);
        else if(!strcmp(k,"alc_gain"))c->audio.alc_gain=pint(val);
        else if(!strcmp(k,"agc_target_dbfs"))c->audio.agc_target_dbfs=pint_cl(val,0,31);
        else if(!strcmp(k,"agc_compression_db"))c->audio.agc_compression_db=pint_cl(val,0,90);
        else if(!strcmp(k,"mute"))c->audio.mute=pbool(val);
        else if(!strcmp(k,"force_stereo"))c->audio.force_stereo=pbool(val);
        else if(!strcmp(k,"spk_enabled"))c->audio.spk_enabled=pbool(val);
        else if(!strcmp(k,"spk_volume"))c->audio.spk_volume=pint(val);
        else if(!strcmp(k,"spk_gain"))c->audio.spk_gain=pint(val);
        else if(!strcmp(k,"backchannel"))c->audio.backchannel=pbool(val);
        else if(!strcmp(k,"backchannel_codec")){
            if(!strcasecmp(val,"pcmu"))c->audio.backchannel_codec=0;
            else if(!strcasecmp(val,"pcma"))c->audio.backchannel_codec=1;
            else if(!strcasecmp(val,"aac"))c->audio.backchannel_codec=2;
            else c->audio.backchannel_codec=pint_cl(val,0,2);
        }
        else if(!strcmp(k,"backchannel_rate"))c->audio.backchannel_rate=pint_cl(val,8000,48000);
        else LOGW(MOD,"unknown key %s",key);
        return;
    }
    if (!strncmp(key,"jpeg.",5)){
        const char *k=key+5;
        if(!strcmp(k,"enabled"))c->jpeg.enabled=pbool(val);
        else if(!strcmp(k,"width"))c->jpeg.width=pint_cl(val,64,4096);   /* M11 */
        else if(!strcmp(k,"height"))c->jpeg.height=pint_cl(val,64,4096);
        else if(!strcmp(k,"quality"))c->jpeg.quality=pint_cl(val,1,100);
        else if(!strcmp(k,"fps"))c->jpeg.fps=pint_cl(val,1,120);
        else if(!strcmp(k,"imp_chn"))c->jpeg.imp_chn=pint(val);
        else if(!strcmp(k,"snapshot_path"))copystr(c->jpeg.snapshot_path,val,128);
        else LOGW(MOD,"unknown key %s",key);
        return;
    }
    if (!strncmp(key,"rtsp.",5)){
        const char *k=key+5;
        if(!strcmp(k,"enabled"))c->rtsp_enabled=pbool(val);
        else if(!strcmp(k,"port"))c->rtsp_port=pint_cl(val,1,65535);   /* M11 */
        else if(!strcmp(k,"user")||!strcmp(k,"username"))copystr(c->rtsp_user,val,MS_MAX_STR);
        else if(!strcmp(k,"pass")||!strcmp(k,"password"))copystr(c->rtsp_pass,val,MS_MAX_STR);
        else if(!strcmp(k,"tls")||!strcmp(k,"tls_enabled"))c->rtsp_tls=pbool(val);
        else if(!strcmp(k,"tls_port"))c->rtsp_tls_port=pint_cl(val,1,65535);
        else LOGW(MOD,"unknown key %s",key);
        return;
    }
    if (!strncmp(key,"srt.",4)){
        const char *k=key+4; ms_srt_cfg *s=&c->srt;
        if(!strcmp(k,"enabled"))s->enabled=pbool(val);
        else if(!strcmp(k,"port"))s->port=pint_cl(val,1,65535);   /* M11 */
        else if(!strcmp(k,"channel"))s->channel=pint(val);
        else if(!strcmp(k,"latency_ms")||!strcmp(k,"latency"))s->latency_ms=pint(val);
        else if(!strcmp(k,"streamid"))copystr(s->streamid,val,sizeof s->streamid);
        else if(!strcmp(k,"passphrase"))copystr(s->passphrase,val,sizeof s->passphrase);
        else LOGW(MOD,"unknown key %s",key);
        return;
    }
    if (!strncmp(key,"http.",5)){
        const char *k=key+5;
        if(!strcmp(k,"enabled"))c->http_enabled=pbool(val);
        else if(!strcmp(k,"port"))c->http_port=pint_cl(val,1,65535);   /* M11 */
        else if(!strcmp(k,"preview_chn"))c->http_preview_chn=pint(val);
        else if(!strcmp(k,"user")||!strcmp(k,"username"))copystr(c->http_user,val,MS_MAX_STR);
        else if(!strcmp(k,"pass")||!strcmp(k,"password"))copystr(c->http_pass,val,MS_MAX_STR);
        else if(!strcmp(k,"token"))copystr(c->http_token,val,MS_MAX_STR);
        else if(!strcmp(k,"token_file"))copystr(c->http_token_file,val,sizeof c->http_token_file);
        else if(!strcmp(k,"https")||!strcmp(k,"tls"))c->http_https=pbool(val);
        else if(!strcmp(k,"tls_cert")||!strcmp(k,"cert"))copystr(c->http_tls_cert,val,128);
        else if(!strcmp(k,"tls_key")||!strcmp(k,"key"))copystr(c->http_tls_key,val,128);
        else LOGW(MOD,"unknown key %s",key);
        return;
    }
    if (!strncmp(key,"events.",7)){
        /* /events SSE push stream (startup settings, like the http.token*
         * keys deliberately not settable via /control) */
        const char *k=key+7;
        if(!strcmp(k,"enabled"))c->events_enabled=pbool(val);
        else if(!strcmp(k,"stats_ms"))c->events_stats_ms=pint(val);
        else if(!strcmp(k,"max_clients"))c->events_max_clients=pint(val);
        else LOGW(MOD,"unknown key %s",key);
        return;
    }
    if (!strncmp(key,"osd.",4)){
        const char *k=key+4;
        if(!strcmp(k,"enabled"))c->osd.enabled=pbool(val);
        else if(!strcmp(k,"monitor_stream"))c->osd.monitor_stream=pint(val);
        else if(!strcmp(k,"font_path"))copystr(c->osd.font_path,val,128);
        else if(!strcmp(k,"vars_file"))copystr(c->osd.vars_file,val,128);
        else if(!strcmp(k,"supersample")){
            int v2=pint(val); if(v2<1)v2=1; if(v2>4)v2=4;
            c->osd.supersample=v2;
        }
        else LOGW(MOD,"unknown key %s",key);
        return;
    }
    if (!strncmp(key,"motion.",7)){
        const char *k=key+7;
        if(!strcmp(k,"enabled"))c->motion.enabled=pbool(val);
        else if(!strcmp(k,"monitor_stream")){
            int v2=pint(val);
            if (v2<0||v2>=MS_MAX_VSTREAM) v2=0;
            c->motion.monitor_stream=v2;
        }
        else if(!strcmp(k,"sensitivity")){
            int v2=pint(val);
            c->motion.sensitivity = v2<0 ? 0 : v2>255 ? 255 : v2;
        }
        else if(!strcmp(k,"cols")||!strcmp(k,"rows")){
            /* grid geometry: >=1 per axis and cols*rows clamped to the SDK's
             * ROI budget (MOTION_CELL_LIMIT). The value BEING SET is clamped
             * against the current other axis (never the other way around),
             * so re-applying the same pair is idempotent - the /control
             * dedup then skips repeated posts instead of rewriting flash. */
            int v2=pint(val);
            if (v2<1) v2=1;
            if (v2>MOTION_CELL_LIMIT) v2=MOTION_CELL_LIMIT;
            if (c->motion.cols<1) c->motion.cols=1;
            if (c->motion.rows<1) c->motion.rows=1;
            if (k[0]=='c'){
                int other = c->motion.rows;
                if (v2 > MOTION_CELL_LIMIT/other) v2 = MOTION_CELL_LIMIT/other;
                c->motion.cols = v2;
            } else {
                int other = c->motion.cols;
                if (v2 > MOTION_CELL_LIMIT/other) v2 = MOTION_CELL_LIMIT/other;
                c->motion.rows = v2;
            }
        }
        else if(!strcmp(k,"roi_x")||!strcmp(k,"roi_y")||
                !strcmp(k,"roi_w")||!strcmp(k,"roi_h")){
            /* B6: legacy single-ROI keys - still parsed/persisted for compat
             * but NOTHING consumes them anymore (the cell grid replaced them).
             * Warn once when a non-default (non-zero) value is set so a user
             * isn't silently losing an ROI restriction they think is active. */
            int v2=pint(val);
            if      (k[4]=='x') c->motion.roi_x=v2;
            else if (k[4]=='y') c->motion.roi_y=v2;
            else if (k[4]=='w') c->motion.roi_w=v2;
            else                c->motion.roi_h=v2;
            if (v2!=0){
                static int roi_warned;
                if (!roi_warned){
                    roi_warned=1;
                    LOGW(MOD,"motion.roi_* is deprecated and IGNORED - use the "
                             "motion grid (motion.cols/rows + cells) instead");
                }
            }
        }
        else if(!strcmp(k,"cooldown_ms"))c->motion.cooldown_ms=pint(val);
        else if(!strcmp(k,"hold_ms")){ int v2=pint(val); c->motion.hold_ms = v2<0?0:v2; }
        else if(!strcmp(k,"skip_frames")){ int v2=pint(val); c->motion.skip_frames = v2<1?1:v2; }
        else if(!strcmp(k,"on_motion"))copystr(c->motion.on_motion,val,128);
        else LOGW(MOD,"unknown key %s",key);
        return;
    }
    if (!strncmp(key,"record.",7)){
        const char *k=key+7; ms_record_cfg *rc=&c->record;
        if(!strcmp(k,"enabled"))rc->enabled=pbool(val);
        else if(!strcmp(k,"channel")){
            int v2=pint(val); rc->channel=(v2<0||v2>=MS_MAX_VSTREAM)?0:v2;
        }
        else if(!strcmp(k,"mode"))
            rc->mode=(!strcasecmp(val,"motion"))?1:(!strcasecmp(val,"continuous"))?0:pint(val);
        else if(!strcmp(k,"dir"))copystr(rc->dir,val,sizeof rc->dir);
        else if(!strcmp(k,"name"))copystr(rc->name,val,sizeof rc->name);
        /* clamp: 0 keeps "no rotation" (documented), negative/absurd is rejected
         * so a garbage value can't silently disable rotation or set wild rolls */
        else if(!strcmp(k,"segment_s")||!strcmp(k,"segment"))rc->segment_s=pint_cl(val,0,86400);
        else if(!strcmp(k,"pre_roll_s")||!strcmp(k,"pre_roll"))rc->pre_roll_s=pint_cl(val,0,60);
        else if(!strcmp(k,"post_roll_s")||!strcmp(k,"post_roll"))rc->post_roll_s=pint_cl(val,0,300);
        else if(!strcmp(k,"min_free_mb"))rc->min_free_mb=pint_cl(val,0,1048576);
        else if(!strcmp(k,"audio"))rc->audio=pbool(val);
        else LOGW(MOD,"unknown key %s",key);
        return;
    }
    if (!strncmp(key,"timelapse.",10)){
        const char *k=key+10; ms_timelapse_cfg *tc=&c->timelapse;
        if(!strcmp(k,"enabled"))tc->enabled=pbool(val);
        else if(!strcmp(k,"channel")){
            int v2=pint(val); tc->channel=(v2<0||v2>=MS_MAX_VSTREAM)?0:v2;
        }
        else if(!strcmp(k,"dir"))copystr(tc->dir,val,sizeof tc->dir);
        else if(!strcmp(k,"name"))copystr(tc->name,val,sizeof tc->name);
        else if(!strcmp(k,"interval_s")||!strcmp(k,"interval")){
            int v2=pint(val); tc->interval_s=v2<1?1:v2;
        }
        else if(!strcmp(k,"keep_days")){ int v2=pint(val); tc->keep_days=v2<0?0:v2; }
        else LOGW(MOD,"unknown key %s",key);
        return;
    }
    if (!strncmp(key,"daynight.",9)){
        const char *k=key+9;
        if(!strcmp(k,"enabled"))c->daynight.enabled=pbool(val);
        else if(!strcmp(k,"total_gain_day_threshold"))c->daynight.total_gain_day_threshold=pflt(val);
        else if(!strcmp(k,"total_gain_night_threshold"))c->daynight.total_gain_night_threshold=pflt(val);
        else if(!strcmp(k,"threshold_low"))c->daynight.threshold_low=pflt(val);
        else if(!strcmp(k,"threshold_high"))c->daynight.threshold_high=pflt(val);
        else if(!strcmp(k,"hysteresis"))c->daynight.hysteresis=pflt(val);
        else if(!strcmp(k,"day_gain_pct"))c->daynight.day_gain_pct=pint(val);
        else if(!strcmp(k,"baseline_delay_s"))c->daynight.baseline_delay_s=pint(val);
        else if(!strcmp(k,"interval_ms"))c->daynight.interval_ms=pint(val);
        else if(!strcmp(k,"transition_s"))c->daynight.transition_s=pint(val);
        else if(!strcmp(k,"switch_cmd"))copystr(c->daynight.switch_cmd,val,sizeof c->daynight.switch_cmd);
        else if(!strcmp(k,"isp_path"))copystr(c->daynight.isp_path,val,sizeof c->daynight.isp_path);
        else LOGW(MOD,"unknown key %s",key);
        return;
    }
    if (!strncmp(key,"general.",8)){
        const char *k=key+8;
        if(!strcmp(k,"loglevel"))c->loglevel=pint(val);
        else if(!strcmp(k,"imp_polling_timeout"))c->imp_polling_timeout=pint(val);
        else if(!strcmp(k,"osd_pool_size"))c->osd_pool_size=pint(val);
        else if(!strcmp(k,"syslog"))log_set_syslog(pbool(val));  /* to logread; default on */
        else LOGW(MOD,"unknown key %s",key);
        return;
    }
    if (!strncmp(key,"sim.",4)){
        const char *k=key+4;
        if(!strcmp(k,"video0"))copystr(c->sim_video0,val,sizeof c->sim_video0);
        else if(!strcmp(k,"video1"))copystr(c->sim_video1,val,sizeof c->sim_video1);
        else if(!strcmp(k,"audio"))copystr(c->sim_audio,val,sizeof c->sim_audio);
        else if(!strcmp(k,"jpeg"))copystr(c->sim_jpeg,val,sizeof c->sim_jpeg);
        else LOGW(MOD,"unknown key %s",key);
        return;
    }
    LOGW(MOD,"unknown key %s", key);
}

/* public: apply one key=value pair (same keys as the config file). Used by the
 * config loader and by the optional /control endpoint for live changes. */
void config_apply_kv(ms_config *c, const char *key, const char *val)
{
    set_kv(c, key, val);
}

/* public: read back a key's current value as a normalized string, matching the
 * form set_kv() stores. Only the keys the /control endpoint can change are
 * covered; anything else returns 0 (change-detection then falls back to always
 * applying). Keep in sync with the image/audio/video/sensor/osd branches of
 * set_kv(). */
int config_get_kv(const ms_config *c, const char *key, char *out, size_t cap)
{
    if (!out || cap==0) return 0;
    out[0]=0;

    int osi, oii;
    const char *ok = osd_key(key, &osi, &oii);
    if (ok){
        /* legacy osdN.* keys write to EVERY stream, so they only read back
         * while all streams still agree on the value - once the sets have
         * diverged the key reports unknown and a legacy write always
         * applies (no false dedup-skip). */
        if (osi<0){
            const ms_osd_item *a=&c->osd.items[0][oii];
            /* .text is runtime-mutable (see g_str_lock comment above); the
             * strcmp() below must not race a concurrent copystr() into it */
            config_str_lock();
            int agree = 1;
            for (int s=1;s<MS_MAX_VSTREAM;s++){
                const ms_osd_item *b=&c->osd.items[s][oii];
                if (a->enabled!=b->enabled || a->type!=b->type ||
                    a->x!=b->x || a->y!=b->y || a->font_size!=b->font_size ||
                    a->color!=b->color || a->transparency!=b->transparency ||
                    a->outline!=b->outline ||
                    a->outline_color!=b->outline_color ||
                    strcmp(a->text,b->text)){
                    agree = 0;
                    break;
                }
            }
            config_str_unlock();
            if (!agree) return 0;
            osi = 0;
        }
        const ms_osd_item *o=&c->osd.items[osi][oii]; const char *k=ok;
        if(!strcmp(k,"enabled")) snprintf(out,cap,"%d",o->enabled);
        else if(!strcmp(k,"type")) snprintf(out,cap,"%s",o->type==MS_OSD_LOGO?"logo":"text");
        else if(!strcmp(k,"text")){
            config_str_lock();
            snprintf(out,cap,"%s",o->text);
            config_str_unlock();
        }
        else if(!strcmp(k,"x")) snprintf(out,cap,"%d",o->x);
        else if(!strcmp(k,"y")) snprintf(out,cap,"%d",o->y);
        else if(!strcmp(k,"font_size")) snprintf(out,cap,"%d",o->font_size);
        else if(!strcmp(k,"color")||!strcmp(k,"font_color")) snprintf(out,cap,"0x%08X",o->color);
        else if(!strcmp(k,"transparency")) snprintf(out,cap,"%d",o->transparency);
        else if(!strcmp(k,"outline")||!strcmp(k,"stroke")) snprintf(out,cap,"%d",o->outline);
        else if(!strcmp(k,"outline_color")||!strcmp(k,"stroke_color")) snprintf(out,cap,"0x%08X",o->outline_color);
        else return 0;
        return 1;
    }
    if (!strncmp(key,"osd.",4)){
        const char *k=key+4;
        if(!strcmp(k,"enabled")) snprintf(out,cap,"%d",c->osd.enabled);
        else if(!strcmp(k,"monitor_stream")) snprintf(out,cap,"%d",c->osd.monitor_stream);
        else if(!strcmp(k,"font_path")) snprintf(out,cap,"%s",c->osd.font_path);
        else if(!strcmp(k,"vars_file")) snprintf(out,cap,"%s",c->osd.vars_file);
        else if(!strcmp(k,"supersample")) snprintf(out,cap,"%d",c->osd.supersample);
        else return 0;
        return 1;
    }
    {   /* privacy<S>.<N>.<field> */
        int psi, pii;
        const char *pk = privacy_key(key, &psi, &pii);
        if (pk){
            const ms_privacy_region *p=&c->privacy[psi][pii];
            if(!strcmp(pk,"enabled")) snprintf(out,cap,"%d",p->enabled);
            else if(!strcmp(pk,"x")) snprintf(out,cap,"%d",p->x);
            else if(!strcmp(pk,"y")) snprintf(out,cap,"%d",p->y);
            else if(!strcmp(pk,"w")||!strcmp(pk,"width")) snprintf(out,cap,"%d",p->w);
            else if(!strcmp(pk,"h")||!strcmp(pk,"height")) snprintf(out,cap,"%d",p->h);
            else if(!strcmp(pk,"color")||!strcmp(pk,"fill_color")) snprintf(out,cap,"0x%08X",p->color);
            else return 0;
            return 1;
        }
    }
    if (!strncmp(key,"image.",6)){
        const ms_image_cfg *m=&c->image; const char *k=key+6;
        if(!strcmp(k,"brightness")) snprintf(out,cap,"%d",m->brightness);
        else if(!strcmp(k,"contrast")) snprintf(out,cap,"%d",m->contrast);
        else if(!strcmp(k,"saturation")) snprintf(out,cap,"%d",m->saturation);
        else if(!strcmp(k,"sharpness")) snprintf(out,cap,"%d",m->sharpness);
        else if(!strcmp(k,"hue")) snprintf(out,cap,"%d",m->hue);
        else if(!strcmp(k,"hflip")) snprintf(out,cap,"%d",m->hflip);
        else if(!strcmp(k,"vflip")) snprintf(out,cap,"%d",m->vflip);
        else if(!strcmp(k,"running_mode")) snprintf(out,cap,"%d",m->running_mode);
        else if(!strcmp(k,"anti_flicker")) snprintf(out,cap,"%d",m->anti_flicker);
        else if(!strcmp(k,"ae_compensation")) snprintf(out,cap,"%d",m->ae_compensation);
        else if(!strcmp(k,"max_again")) snprintf(out,cap,"%d",m->max_again);
        else if(!strcmp(k,"max_dgain")) snprintf(out,cap,"%d",m->max_dgain);
        else if(!strcmp(k,"sinter_strength")) snprintf(out,cap,"%d",m->sinter_strength);
        else if(!strcmp(k,"temper_strength")) snprintf(out,cap,"%d",m->temper_strength);
        else if(!strcmp(k,"dpc_strength")) snprintf(out,cap,"%d",m->dpc_strength);
        else if(!strcmp(k,"defog_strength")) snprintf(out,cap,"%d",m->defog_strength);
        else if(!strcmp(k,"drc_strength")) snprintf(out,cap,"%d",m->drc_strength);
        else if(!strcmp(k,"highlight_depress")) snprintf(out,cap,"%d",m->highlight_depress);
        else if(!strcmp(k,"backlight_compensation")) snprintf(out,cap,"%d",m->backlight_compensation);
        else if(!strcmp(k,"core_wb_mode")) snprintf(out,cap,"%d",m->core_wb_mode);
        else if(!strcmp(k,"wb_rgain")) snprintf(out,cap,"%d",m->wb_rgain);
        else if(!strcmp(k,"wb_bgain")) snprintf(out,cap,"%d",m->wb_bgain);
        else return 0;
        return 1;
    }
    if (!strncmp(key,"audio.",6)){
        const char *k=key+6; const ms_audio_cfg *a=&c->audio;
        if(!strcmp(k,"volume")) snprintf(out,cap,"%d",a->volume);
        else if(!strcmp(k,"gain")) snprintf(out,cap,"%d",a->gain);
        else if(!strcmp(k,"alc_gain")) snprintf(out,cap,"%d",a->alc_gain);
        else if(!strcmp(k,"high_pass")) snprintf(out,cap,"%d",a->high_pass);
        else if(!strcmp(k,"agc")) snprintf(out,cap,"%d",a->agc);
        else if(!strcmp(k,"ns")) snprintf(out,cap,"%d",a->ns);
        else if(!strcmp(k,"agc_target_dbfs")) snprintf(out,cap,"%d",a->agc_target_dbfs);
        else if(!strcmp(k,"agc_compression_db")) snprintf(out,cap,"%d",a->agc_compression_db);
        else if(!strcmp(k,"mute")) snprintf(out,cap,"%d",a->mute);
        else if(!strcmp(k,"enabled")) snprintf(out,cap,"%d",a->enabled);
        else if(!strcmp(k,"codec")) snprintf(out,cap,"%s",acodec_name(a->codec));
        else if(!strcmp(k,"samplerate")) snprintf(out,cap,"%d",a->samplerate);
        else if(!strcmp(k,"channels")) snprintf(out,cap,"%d",a->channels);
        else if(!strcmp(k,"bitrate")) snprintf(out,cap,"%d",a->bitrate_kbps);
        else if(!strcmp(k,"force_stereo")) snprintf(out,cap,"%d",a->force_stereo);
        else if(!strcmp(k,"spk_enabled")) snprintf(out,cap,"%d",a->spk_enabled);
        else if(!strcmp(k,"spk_volume")) snprintf(out,cap,"%d",a->spk_volume);
        else if(!strcmp(k,"spk_gain")) snprintf(out,cap,"%d",a->spk_gain);
        else if(!strcmp(k,"backchannel")) snprintf(out,cap,"%d",a->backchannel);
        else if(!strcmp(k,"backchannel_codec")) snprintf(out,cap,"%d",a->backchannel_codec);
        else if(!strcmp(k,"backchannel_rate")) snprintf(out,cap,"%d",a->backchannel_rate);
        else return 0;
        return 1;
    }
    if (!strncmp(key,"video0.",7) || !strncmp(key,"video1.",7)){
        const ms_vstream_cfg *v = (key[5]=='0') ? &c->video[0] : &c->video[1];
        const char *k = key+7;
        if(!strcmp(k,"enabled")) snprintf(out,cap,"%d",v->enabled);
        else if(!strcmp(k,"codec")) snprintf(out,cap,"%s",vcodec_name(v->codec));
        else if(!strcmp(k,"width")) snprintf(out,cap,"%d",v->width);
        else if(!strcmp(k,"height")) snprintf(out,cap,"%d",v->height);
        else if(!strcmp(k,"fps")) snprintf(out,cap,"%d",v->fps);
        else if(!strcmp(k,"bitrate")) snprintf(out,cap,"%d",v->bitrate_kbps);
        else if(!strcmp(k,"rc_mode")||!strcmp(k,"mode")) snprintf(out,cap,"%s",rc_name(v->rc_mode));
        else if(!strcmp(k,"gop")) snprintf(out,cap,"%d",v->gop);
        else if(!strcmp(k,"max_gop")) snprintf(out,cap,"%d",v->max_gop);
        else if(!strcmp(k,"profile")) snprintf(out,cap,"%d",v->profile);
        else if(!strcmp(k,"qp")) snprintf(out,cap,"%d",v->qp);
        else if(!strcmp(k,"min_qp")) snprintf(out,cap,"%d",v->min_qp);
        else if(!strcmp(k,"max_qp")) snprintf(out,cap,"%d",v->max_qp);
        else if(!strcmp(k,"rotation")) snprintf(out,cap,"%d",v->rotation);
        else if(!strcmp(k,"buffers")) snprintf(out,cap,"%d",v->buffers);
        else if(!strcmp(k,"rtsp_path")){
            config_str_lock();
            snprintf(out,cap,"%s",v->rtsp_path);
            config_str_unlock();
        }
        else return 0;
        return 1;
    }
    if (!strncmp(key,"sensor.",7)){
        const ms_sensor_cfg *s=&c->sensor; const char *k=key+7;
        if(!strcmp(k,"model")){
            config_str_lock();
            snprintf(out,cap,"%s",s->model);
            config_str_unlock();
        }
        else if(!strcmp(k,"i2c_addr")||!strcmp(k,"i2c_address")) snprintf(out,cap,"%d",s->i2c_addr);
        else if(!strcmp(k,"fps")) snprintf(out,cap,"%d",s->fps);
        else if(!strcmp(k,"width")) snprintf(out,cap,"%d",s->width);
        else if(!strcmp(k,"height")) snprintf(out,cap,"%d",s->height);
        else return 0;
        return 1;
    }
    if (!strncmp(key,"daynight.",9)){
        const ms_daynight_cfg *d=&c->daynight; const char *k=key+9;
        if(!strcmp(k,"enabled")) snprintf(out,cap,"%d",d->enabled);
        else if(!strcmp(k,"total_gain_day_threshold")) snprintf(out,cap,"%g",(double)d->total_gain_day_threshold);
        else if(!strcmp(k,"total_gain_night_threshold")) snprintf(out,cap,"%g",(double)d->total_gain_night_threshold);
        else if(!strcmp(k,"threshold_low")) snprintf(out,cap,"%g",(double)d->threshold_low);
        else if(!strcmp(k,"threshold_high")) snprintf(out,cap,"%g",(double)d->threshold_high);
        else if(!strcmp(k,"hysteresis")) snprintf(out,cap,"%g",(double)d->hysteresis);
        else if(!strcmp(k,"day_gain_pct")) snprintf(out,cap,"%d",d->day_gain_pct);
        else if(!strcmp(k,"baseline_delay_s")) snprintf(out,cap,"%d",d->baseline_delay_s);
        else if(!strcmp(k,"interval_ms")) snprintf(out,cap,"%d",d->interval_ms);
        else if(!strcmp(k,"transition_s")) snprintf(out,cap,"%d",d->transition_s);
        else return 0;
        return 1;
    }
    if (!strncmp(key,"motion.",7)){
        const ms_motion_cfg *m=&c->motion; const char *k=key+7;
        if(!strcmp(k,"enabled")) snprintf(out,cap,"%d",m->enabled);
        else if(!strcmp(k,"monitor_stream")) snprintf(out,cap,"%d",m->monitor_stream);
        else if(!strcmp(k,"sensitivity")) snprintf(out,cap,"%d",m->sensitivity);
        else if(!strcmp(k,"cols")) snprintf(out,cap,"%d",m->cols);
        else if(!strcmp(k,"rows")) snprintf(out,cap,"%d",m->rows);
        else if(!strcmp(k,"cooldown_ms")) snprintf(out,cap,"%d",m->cooldown_ms);
        else if(!strcmp(k,"hold_ms")) snprintf(out,cap,"%d",m->hold_ms);
        else if(!strcmp(k,"skip_frames")) snprintf(out,cap,"%d",m->skip_frames);
        else return 0;
        return 1;
    }
    if (!strncmp(key,"record.",7)){
        /* M13: control.c sets record.* via /control, but without this branch
         * change-detection reported every record key "unknown" and each
         * record-page POST rewrote /etc/timps.conf (flash wear). Mirrors the
         * record.* branch of set_kv(); mode reads back numerically (0/1),
         * which the get-apply-get dedup compares against itself. */
        const ms_record_cfg *r=&c->record; const char *k=key+7;
        if(!strcmp(k,"enabled")) snprintf(out,cap,"%d",r->enabled);
        else if(!strcmp(k,"channel")) snprintf(out,cap,"%d",r->channel);
        else if(!strcmp(k,"mode")) snprintf(out,cap,"%d",r->mode);
        else if(!strcmp(k,"dir")){
            config_str_lock();
            snprintf(out,cap,"%s",r->dir);
            config_str_unlock();
        }
        else if(!strcmp(k,"name")){
            config_str_lock();
            snprintf(out,cap,"%s",r->name);
            config_str_unlock();
        }
        else if(!strcmp(k,"segment_s")||!strcmp(k,"segment")) snprintf(out,cap,"%d",r->segment_s);
        else if(!strcmp(k,"pre_roll_s")||!strcmp(k,"pre_roll")) snprintf(out,cap,"%d",r->pre_roll_s);
        else if(!strcmp(k,"post_roll_s")||!strcmp(k,"post_roll")) snprintf(out,cap,"%d",r->post_roll_s);
        else if(!strcmp(k,"min_free_mb")) snprintf(out,cap,"%d",r->min_free_mb);
        else if(!strcmp(k,"audio")) snprintf(out,cap,"%d",r->audio);
        else return 0;
        return 1;
    }
    if (!strncmp(key,"timelapse.",10)){
        const ms_timelapse_cfg *t=&c->timelapse; const char *k=key+10;
        if(!strcmp(k,"enabled")) snprintf(out,cap,"%d",t->enabled);
        else if(!strcmp(k,"channel")) snprintf(out,cap,"%d",t->channel);
        else if(!strcmp(k,"dir")){
            config_str_lock();
            snprintf(out,cap,"%s",t->dir);
            config_str_unlock();
        }
        else if(!strcmp(k,"name")){
            config_str_lock();
            snprintf(out,cap,"%s",t->name);
            config_str_unlock();
        }
        else if(!strcmp(k,"interval_s")) snprintf(out,cap,"%d",t->interval_s);
        else if(!strcmp(k,"keep_days")) snprintf(out,cap,"%d",t->keep_days);
        else return 0;
        return 1;
    }
    return 0;
}

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) *e-- = 0;
    return s;
}

/* cut an inline "# comment" from a value. The '#' must be preceded by
 * whitespace (so a '#' that is part of the value survives) and the value must
 * not be quoted (quote it to keep a literal "# ..."). Without this a line like
 * "key = true   # note" parsed the value as "true   # note" -> pbool() false
 * and pacodec()/paths broke; numeric keys survived by luck (strtol stops at
 * the space). */
static void strip_inline_comment(char *val)
{
    if (*val=='"' || *val=='\'') return;          /* quoted: keep literal */
    for (char *p=val; *p; p++)
        if (*p=='#' && (p==val || p[-1]==' ' || p[-1]=='\t')) { *p=0; break; }
}

int config_load(ms_config *c, const char *path)
{
    config_defaults(c);
    g_cfg_path = path;               /* remember for config_write_keys() */
    FILE *f = fopen(path, "r");
    if (!f) { LOGW(MOD,"config %s not found, using defaults", path); return -1; }
    char line[512];
    int n=0;
    while (fgets(line, sizeof line, f)) {
        /* L9: a physical line longer than the buffer used to be silently
         * chopped into two logical lines (truncated value + a garbage "key").
         * Detect the missing trailing '\n', drop the whole line and warn. */
        size_t ll = strlen(line);
        if (ll+1 == sizeof line && line[ll-1] != '\n'){
            int ch = fgetc(f);
            if (ch != EOF){        /* genuinely longer than the buffer: drop rest + warn */
                while (ch!=EOF && ch!='\n') ch=fgetc(f);
                LOGW(MOD,"config: line longer than %zu chars skipped (starts \"%.40s...\")",
                     sizeof line - 2, line);
                continue;
            }
            /* L-1: EOF right after a full buffer = a legit final line with no
             * trailing '\n' (exactly sizeof-1 chars) - parse it, don't skip. */
        }
        char *s = trim(line);
        if (!*s || *s=='#' || *s==';') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(s);
        char *val = trim(eq+1);
        strip_inline_comment(val);
        val = trim(val);                 /* drop the space left before the '#' */
        size_t vl = strlen(val);
        if (vl>=2 && ((val[0]=='"'&&val[vl-1]=='"')||(val[0]=='\''&&val[vl-1]=='\''))) {
            val[vl-1]=0; val++;
        }
        set_kv(c, key, val);
        n++;
    }
    fclose(f);
    log_set_level(c->loglevel);
    LOGI(MOD,"loaded %d settings from %s", n, path);
    return 0;
}

/* read one trimmed line from a file into out; returns 0 on non-empty success */
static int read_proc_line(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int ok = fgets(out, (int)cap, f) != NULL;
    fclose(f);
    if (!ok) return -1;
    char *nl = strchr(out, '\n'); if (nl) *nl = 0;
    size_t l = strlen(out);
    while (l && (out[l-1]=='\r' || out[l-1]==' ' || out[l-1]=='\t')) out[--l]=0;
    return out[0] ? 0 : -1;
}
/* read /proc/jz/sensor/sensor0/<key> as a number (base 0 = 0x.. hex or decimal);
 * <0 on missing/unparseable */
static long read_sensor_proc(const char *key, int base)
{
    char path[80], buf[64];
    snprintf(path, sizeof path, "/proc/jz/sensor/sensor0/%s", key);
    if (read_proc_line(path, buf, sizeof buf) != 0) return -1;
    return strtol(buf, NULL, base);
}

/* raptor/prudynt-style sensor auto-detect: fill any sensor.* field left unset
 * (empty/0 - i.e. not given in the config) from the Ingenic kernel sensor
 * registry /proc/jz/sensor/sensor0/, which the board's sensor .ko populates
 * with name/i2c_addr/width/height/max_fps after probing the chip. Config values
 * always win; whatever is still unset gets a safe fallback. On the host sim and
 * on T40/T41 (no /proc/jz/sensor) the reads fail, so only the fallback applies.
 * Call once after config_load(), before the HAL is started. */
void config_sensor_finalize(ms_config *c)
{
    /* The loaded kernel sensor driver (/proc/jz/sensor/sensor0) is authoritative
     * for the sensor NAME and I2C address: IMP_ISP_AddSensor must be told the
     * sensor that is actually loaded. A mismatching name makes the ISP/sensor
     * kernel module work from a zero attr table (pclk/line_time == 0) and divide
     * by zero -> SIGFPE in the kernel. So the registry overrides a stale config
     * value here (resolution/fps stay config-first below - the registry often
     * reports 0 for them). */
    { char name[MS_MAX_STR];
      if (read_proc_line("/proc/jz/sensor/sensor0/name", name, sizeof name) == 0 && name[0]){
          if (c->sensor.model[0] && strcasecmp(c->sensor.model, name) != 0)
              LOGW(MOD,"config sensor.model '%s' != loaded driver '%s' - using '%s' "
                       "(the config value would crash the ISP)", c->sensor.model, name, name);
          copystr(c->sensor.model, name, MS_MAX_STR);
      }
    }
    { long v=read_sensor_proc("i2c_addr",0);
      if(v>0){
          if(c->sensor.i2c_addr && c->sensor.i2c_addr!=(int)v)
              LOGW(MOD,"config sensor.i2c 0x%02x != loaded driver 0x%02lx - using 0x%02lx",
                   c->sensor.i2c_addr,v,v);
          c->sensor.i2c_addr=(int)v;
      }
    }
    if (c->sensor.width   == 0){ long v=read_sensor_proc("width",10);    if(v>0) c->sensor.width  =(int)v; }
    if (c->sensor.height  == 0){ long v=read_sensor_proc("height",10);   if(v>0) c->sensor.height =(int)v; }
    if (c->sensor.fps     == 0){ long v=read_sensor_proc("max_fps",10);
                                 if(v<=0) v=read_sensor_proc("fps",10);  if(v>0) c->sensor.fps    =(int)v; }

    /* safe fallbacks when neither the config nor the sensor registry had it */
    if (!c->sensor.model[0]) copystr(c->sensor.model, "gc2053", MS_MAX_STR);
    if (c->sensor.i2c_addr == 0) c->sensor.i2c_addr = 0x37;
    /* Resolution: like raptor, when neither config nor the sensor registry
     * report it (some drivers, e.g. sc2336, expose no width/height in /proc),
     * derive the sensor resolution from the main stream (video0) so a 2K/4MP
     * camera whose driver reports 0 still crops/scales correctly; final safety
     * net is 1080p. */
    if (c->sensor.width   == 0)  c->sensor.width  = c->video[0].width  > 0 ? c->video[0].width  : 1920;
    if (c->sensor.height  == 0)  c->sensor.height = c->video[0].height > 0 ? c->video[0].height : 1080;
    if (c->sensor.fps     == 0)  c->sensor.fps    = c->video[0].fps    > 0 ? c->video[0].fps    : 25;

    LOGI(MOD, "sensor: %s i2c=0x%02x %dx%d @%dfps", c->sensor.model,
         c->sensor.i2c_addr, c->sensor.width, c->sensor.height, c->sensor.fps);
}

/* write one "key = value" line, quoting values that would not survive the
 * loader's whitespace trimming */
static void write_kv_line(FILE *f, const char *k, const char *vin)
{
    /* defensively strip anything that could break the flat key=value file:
     * control chars (a newline would inject a new config line) and the double
     * quote used for our own quoting */
    char v[256]; size_t o=0;
    for (const char *p=vin; *p && o+1<sizeof v; p++){
        unsigned char ch=(unsigned char)*p;
        v[o++] = (ch<0x20) ? ' ' : (ch=='"' ? '\'' : (char)ch);
    }
    v[o]=0;
    size_t l = o;
    int quote = (l==0) || isspace((unsigned char)v[0]) ||
                (l>0 && isspace((unsigned char)v[l-1])) || v[0]=='#';
    if (!quote && strchr(v,' ')) quote = 1;      /* keep multi-word values intact */
    if (quote) fprintf(f, "%s = \"%s\"\n", k, v);
    else       fprintf(f, "%s = %s\n", k, v);
}

/* Persist n key/value pairs into the config file: existing "key = ..." lines
 * are replaced in place (comments, order and unknown lines are preserved),
 * missing keys are appended at the end, later duplicates of a replaced key are
 * dropped. The file is written atomically (tmp file + rename). Returns 0 on
 * success. A missing source file is fine (it is created). */
int config_write_keys(const char *path, const char *const *keys,
                      const char *const *vals, int n)
{
    if (!path || !path[0] || n<=0) return -1;
    unsigned char done[64];
    if (n > (int)sizeof done) n = (int)sizeof done;
    memset(done, 0, sizeof done);

    /* Serialize writers. /control POSTs run in detached per-connection threads
     * with no lock around this file, so two rapid changes (e.g. AGC on then
     * off) used to run here concurrently on the same inode: one truncated /
     * renamed the tmp while the other was mid-write, leaving the live config
     * ending mid-line, onto which the next append glued the following key
     * ("audio.volume = 100audio.agc = 1") and the duplicate that followed. */
    static pthread_mutex_t wlock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&wlock);
    int rc = -1;

    /* Unique tmp in the same directory (mkstemp), so no two writers can ever
     * share the tmp inode; must stay on the same filesystem for rename(). */
    char tmp[300];
    if (snprintf(tmp, sizeof tmp, "%s.tmpXXXXXX", path) >= (int)sizeof tmp)
        goto unlock;
    int tfd = mkstemp(tmp);
    if (tfd < 0){ LOGW(MOD,"cannot create tmp for %s: %s", path, strerror(errno)); goto unlock; }
    fchmod(tfd, 0644);                       /* mkstemp makes it 0600; match a normal conf */
    FILE *out = fdopen(tfd, "w");
    if (!out){ LOGW(MOD,"fdopen tmp failed"); close(tfd); unlink(tmp); goto unlock; }

    int last_nl = 1;                         /* does the copied body end in '\n'? */
    FILE *in = fopen(path, "r");
    if (in){
        char line[512];
        while (fgets(line, sizeof line, in)){
            int handled = 0;
            char cpy[512];
            snprintf(cpy, sizeof cpy, "%s", line);
            char *s = trim(cpy);
            if (*s && *s!='#' && *s!=';'){
                char *eq = strchr(s, '=');
                if (eq){
                    *eq = 0;
                    char *k = trim(s);
                    for (int i=0;i<n;i++){
                        if (strcmp(k, keys[i])) continue;
                        if (!done[i]){ write_kv_line(out, keys[i], vals[i]); done[i]=1; }
                        /* else: duplicate line of an already replaced key -> drop */
                        handled = 1;
                        break;
                    }
                }
            }
            if (handled) { last_nl = 1; }     /* write_kv_line always ends in '\n' */
            else { fputs(line, out); size_t ll=strlen(line); last_nl = (ll==0)||line[ll-1]=='\n'; }
        }
        fclose(in);
    }
    /* Never let an appended key glue onto an unterminated last line. This also
     * self-heals a file an older (racy) build already left mid-line: the merged
     * text is split off cleanly on the next save that rewrites its lead key. */
    if (!last_nl) fputc('\n', out);
    for (int i=0;i<n;i++)
        if (!done[i]) write_kv_line(out, keys[i], vals[i]);

    if (fflush(out)!=0 || ferror(out)){ fclose(out); remove(tmp); goto unlock; }
    /* fflush() only moves data from libc's buffer into the OS page cache -
     * on jffs2/ubifs (this file's usual home) that is not durable yet. A
     * power cut right after "success" here (this is called on nearly every
     * /control POST) can leave the config file empty/zero-length. */
    if (fsync(fileno(out))!=0)
        LOGW(MOD,"fsync %s failed: %s", tmp, strerror(errno));
    fclose(out);
    if (rename(tmp, path)!=0){ LOGW(MOD,"rename %s -> %s failed", tmp, path); remove(tmp); goto unlock; }
    /* the rename()'s directory-entry update needs its own durability flush
     * too - otherwise a power cut right after a successful rename() can
     * still leave the directory pointing at the old (or no) file even
     * though the new file's data already landed. Best-effort: if this
     * fails there is nothing more constructive to do than log it. */
    {
        char dir[280]; snprintf(dir, sizeof dir, "%s", path);
        char *slash = strrchr(dir, '/');
        if (slash) *slash = 0; else snprintf(dir, sizeof dir, ".");
        int dfd = open(dir, O_RDONLY);
        if (dfd >= 0){
            if (fsync(dfd)!=0) LOGW(MOD,"fsync dir %s failed: %s", dir, strerror(errno));
            close(dfd);
        }
    }
    LOGI(MOD,"persisted %d setting(s) to %s", n, path);
    rc = 0;
unlock:
    pthread_mutex_unlock(&wlock);
    return rc;
}
