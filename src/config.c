#include "config.h"
#include "log.h"
#include "motion_caps.h"   /* MOTION_MAX_CELLS/MOTION_CELL_LIMIT (grid clamp) */
#include "rotate_caps.h"   /* ROT_HAS_90/ROT_HAS_HW_I2D (rotation whitelist) */
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
#include <limits.h>  /* INT_MAX for open-ended lo-only clamps in the tables */

#define MOD "CONFIG"
ms_config g_cfg;
ms_config g_cfg_boot;            /* see config.h: immutable boot snapshot */
const char *g_cfg_path = NULL;   /* config file in use, set by config_load() */

/* Snapshot the config as the running encoder was started with. Called once at
 * startup after config_load()/config_sensor_finalize() and before the HAL/
 * network servers come up, i.e. before any /control thread can mutate g_cfg. */
void config_snapshot_boot(void) { g_cfg_boot = g_cfg; }

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
 *   - daynight.time_night_start,    (daynight.time_night_start /
 *     daynight.time_day_start        daynight.time_day_start)
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
/* rotation parser: accepts degrees (0/90/270, plus 180 on T40/T41) and the
 * legacy T31 rotTo90 enum (0/1/2 -> 0/90/270), then whitelists against this
 * SoC's caps. Anything unsupported coerces to 0 (with a warning) so the
 * streamer stays behaviour-neutral until an apply path exists.
 * 180 is platform-nuanced: on every classic-API SoC (T10/T20/T21/T23/T30/T31/
 * C100) it was only ever a GLOBAL ISP Hflip+Vflip - visually and mechanically
 * identical to (and made redundant by) the always-available image.hflip +
 * image.vflip, and falsely modelled as per-stream - so it was removed there and
 * still coerces to 0 below (use image.hflip+image.vflip instead). But on T40/T41
 * (ROT_HAS_HW_I2D) 180 is a genuine PER-CHANNEL hardware I2D rotate scoped to
 * just the requesting video channel - something the global image.hflip/vflip
 * CANNOT replicate there (they flip every channel at the sensor/ISP). So 180 is
 * a real, distinct capability on that platform and is accepted only when
 * ROT_HAS_HW_I2D is defined; everywhere else it lands on the "unsupported -> 0"
 * rail like an unsupported 90/270 request.
 * IMPORTANT: legacy 1/2 here must round-trip to the SAME rotTo90 value in
 * hal_ingenic.c's fs_create() (currently 90->1, 270->2, see the 2026-07-21
 * comment there) so that a prudynt.cfg/raptor-style `rotation=1|2` (which is
 * literally the raw libimp enum, not degrees - verified against prudynt-t's
 * Config.cpp) lands on the same physical direction it did in that config,
 * not the inverted one. */
static int prot(const char *val){
    int r = pint(val);
    if (r==1) r=90; else if (r==2) r=270;        /* legacy T31 rotTo90 0/1/2 */
    if (r!=0 && r!=90 && r!=180 && r!=270){ LOGW(MOD,"rotation %d invalid -> 0",r); return 0; }
#ifndef ROT_HAS_90
    if (r==90||r==270){ LOGW(MOD,"rotation %d unsupported on this SoC -> 0",r); return 0; }
#endif
#ifndef ROT_HAS_HW_I2D
    /* 180 only has a real per-channel path on T40/T41; elsewhere it is the
     * removed redundant global flip - coerce to 0 (use image.hflip+vflip). */
    if (r==180){ LOGW(MOD,"rotation 180 unsupported on this SoC -> 0 (use image.hflip+image.vflip)"); return 0; }
#endif
    return r;
}
static float pflt(const char *v){ return (float)strtod(v, NULL); }
/* F3: pflt with a sane range, same rationale as pint_cl() above. The !(x>=lo)
 * form also catches NaN (all comparisons with NaN are false), so "nan" from a
 * broken client lands on the lower rail instead of poisoning every float
 * comparison in the consumer. */
static float pflt_cl(const char *v, float lo, float hi)
{
    float x = pflt(v);
    if (!(x >= lo))     x = lo;
    else if (x > hi)    x = hi;
    return x;
}
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
    /* 1200 (WebRTC's choice) leaves room for WireGuard/OpenVPN/PPPoE/IPv6
     * tunnel overhead; raise to 1400 for LAN-only setups if desired */
    c->rtsp_mtu = 1200;
    c->http_enabled = 1; c->http_port = 8880; c->http_preview_chn = 1;
    c->http_adaptive_drop = 1;   /* hardware-verified (see httpd.c): the v1 hang
                                  * (missing IDR fallback on P-frame eviction) is
                                  * fixed and rate-limited; confirmed clean on a
                                  * healthy link (garage) and correctly freezing/
                                  * recovering under genuine packet loss (a
                                  * chronically weak-WiFi camera) */
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
    c->audio.spk_enabled=1; c->audio.spk_volume=80; c->audio.spk_gain=25;
    c->audio.backchannel=0; c->audio.backchannel_codec=0; c->audio.backchannel_rate=16000;
    c->audio.aec=0;                                        /* AEC opt-in, default off */

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
            o->font_size=(s==0)?32:12; o->color=0xFFFFFFFF; o->transparency=255;
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
    c->daynight.mode=DN_MODE_SENSOR;   /* sensor-driven detection is the default */
    c->daynight.time_night_start[0]=0; c->daynight.time_day_start[0]=0;
    c->daynight.sun_latitude=0.0f; c->daynight.sun_longitude=0.0f;
    c->daynight.sun_sunrise_offset_min=0; c->daynight.sun_sunset_offset_min=0;
    c->daynight.total_gain_day_threshold=300.0f;
    c->daynight.total_gain_night_threshold=3000.0f;
    c->daynight.threshold_low=25.0f; c->daynight.threshold_high=75.0f;
    c->daynight.hysteresis=0.1f;
    c->daynight.day_gain_pct=60; c->daynight.baseline_delay_s=30;
    c->daynight.boot_settle_s=5; c->daynight.boot_settle_max_s=120;
    c->daynight.boot_stable_pct=20; c->daynight.night_reconfirm_s=3600;
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

/* ------------------------------------------------------------------------
 * Descriptor-driven key tables (B2, review 2026-07-31).
 *
 * set_kv() and config_get_kv() used to be two hand-maintained ~230-key
 * strcmp cascades (~21 KB .text - the two largest functions in the whole
 * binary). Every key now has ONE table entry describing where it lives in
 * ms_config, how it parses/clamps and how it reads back, so setter and
 * getter cannot drift apart. Keys whose behaviour does not fit the generic
 * {name, offset, type, clamp} shape stay explicit code in set_kv() below:
 * motion.cols/rows (cross-axis ROI-budget clamp), the deprecated
 * motion.roi_* (parse + one-shot warning), general.syslog (side effect
 * only, not stored in g_cfg) and videoN.buffers (extra buffers_explicit
 * flag).
 *
 * Clamp provenance (values unchanged from the old cascades, see git history
 * for the full rationale): M11 (dims/fps/ports/bitrates/gop/qp/buffers),
 * F-03 (audio volume/gain/PGA/spk), F-04 (OSD logo dims/outline), H4
 * (font_size), L-1/L-2 (audio.bitrate 8..320), imp_isp.h domains (image.*
 * knobs 0..255, highlight/backlight 0..10, WB gains 0..65535), imp_audio.h
 * domains (ns 0..3, agc_target_dbfs 0..31, agc_compression_db 0..90),
 * F3 2026-07-31 (daynight.* numerics, see daynight_fields).
 */
enum {
    T_BOOL,     /* int:      pbool()                        <-> "%d"     */
    T_INT,      /* int:      pint(), clamped lo..hi if lo<hi <-> "%d"     */
    T_CHAN,     /* int:      video stream idx, bad -> 0      <-> "%d"     */
    T_HEX,      /* uint32_t: phex()                          <-> "0x%08X" */
    T_FLT,      /* float:    pflt(), clamped lo..hi if lo<hi <-> "%g"     */
    T_STR,      /* char[hi]: copystr()   <-> "%s" under config_str_lock   */
    T_VCODEC,   /* int: pvcodec()  <-> vcodec_name()                      */
    T_ACODEC,   /* int: pacodec()  <-> acodec_name()                      */
    T_RC,       /* int: prc()      <-> rc_name()                          */
    T_ROT,      /* int: prot() (degrees/legacy + SoC whitelist) <-> "%d"  */
    T_OSDTYPE,  /* int: "logo"/"text"          <-> same                   */
    T_DNMODE,   /* int: "sensor"/"time"/"sun"  <-> same                   */
    T_RECMODE,  /* int: "motion"/"continuous"/number <-> "%d"             */
    T_BCCODEC,  /* int: "pcmu"/"pcma"/"aac"/number   <-> "%d"             */
};

/* Set/persist-only key: config_get_kv() keeps reporting it unknown, exactly
 * like the old hand-written getter did, so /control change-detection falls
 * back to always applying it. Making one of these readable would newly let
 * the get-apply-get dedup SKIP an unchanged re-POST - a behaviour change -
 * so don't drop the flag without checking the /control consumers. */
#define F_NOGET 0x01

typedef struct {
    const char    *name;    /* canonical key name (after the section prefix) */
    const char    *alias;   /* optional legacy/alternate spelling, or NULL */
    unsigned short off;     /* byte offset from the section base struct */
    unsigned char  type;    /* T_* */
    unsigned char  flags;   /* F_* */
    int lo, hi;             /* T_INT/T_FLT: clamp when lo<hi; T_STR: buf size in hi */
} cfg_field;

/* entry helpers: F = generic field, FS = string field (size from the struct) */
#define F(nm,al,fld,ty,fl,LO,HI) \
    { nm, al, (unsigned short)offsetof(TT,fld), ty, fl, LO, HI }
#define FS(nm,al,fld,fl) \
    { nm, al, (unsigned short)offsetof(TT,fld), T_STR, fl, 0, \
      (int)sizeof ((TT*)0)->fld }

#define TT ms_sensor_cfg
static const cfg_field sensor_fields[] = {
    FS("model",     0,             model, 0),
    F ("i2c_addr",  "i2c_address", i2c_addr, T_INT, 0, 0,0),
    F ("fps",       0,             fps,      T_INT, 0, 0,0),
    F ("width",     0,             width,    T_INT, 0, 0,0),
    F ("height",    0,             height,   T_INT, 0, 0,0),
};
#undef TT

#define TT ms_image_cfg
static const cfg_field image_fields[] = {
    F("brightness",             0, brightness,             T_INT, 0, 0,255),
    F("contrast",               0, contrast,               T_INT, 0, 0,255),
    F("saturation",             0, saturation,             T_INT, 0, 0,255),
    F("sharpness",              0, sharpness,              T_INT, 0, 0,255),
    F("hue",                    0, hue,                    T_INT, 0, 0,255),
    F("vflip",                  0, vflip,                  T_BOOL,0, 0,0),
    F("hflip",                  0, hflip,                  T_BOOL,0, 0,0),
    F("running_mode",           0, running_mode,           T_INT, 0, 0,0),
    F("anti_flicker",           0, anti_flicker,           T_INT, 0, 0,0),
    F("ae_compensation",        0, ae_compensation,        T_INT, 0, 0,255),
    F("max_again",              0, max_again,              T_INT, 0, 0,255),
    F("max_dgain",              0, max_dgain,              T_INT, 0, 0,255),
    F("sinter_strength",        0, sinter_strength,        T_INT, 0, 0,255),
    F("temper_strength",        0, temper_strength,        T_INT, 0, 0,255),
    F("dpc_strength",           0, dpc_strength,           T_INT, 0, 0,255),
    F("defog_strength",         0, defog_strength,         T_INT, 0, 0,255),
    F("drc_strength",           0, drc_strength,           T_INT, 0, 0,255),
    F("highlight_depress",      0, highlight_depress,      T_INT, 0, 0,10),
    F("backlight_compensation", 0, backlight_compensation, T_INT, 0, 0,10),
    F("core_wb_mode",           0, core_wb_mode,           T_INT, 0, 0,0),
    F("wb_rgain",               0, wb_rgain,               T_INT, 0, 0,65535),
    F("wb_bgain",               0, wb_bgain,               T_INT, 0, 0,65535),
};
#undef TT

#define TT ms_audio_cfg
static const cfg_field audio_fields[] = {
    F ("enabled",            0, enabled,            T_BOOL,   0, 0,0),
    F ("codec",              0, codec,              T_ACODEC, 0, 0,0),
    F ("samplerate",         0, samplerate,         T_INT,    0, 0,0),
    /* 1 = mono (native), 2 = simulated stereo (mono mic duplicated to L=R,
     * AAC only) - anything else would put a bogus channel count in the AAC
     * ASC / SDP / fMP4 stsd */
    F ("channels",           0, channels,           T_INT,    0, 1,2),
    F ("bitrate",            0, bitrate_kbps,       T_INT,    0, 8,320),
    F ("volume",             0, volume,             T_INT,    0, 0,100),
    F ("gain",               0, gain,               T_INT,    0, 0,31),
    F ("high_pass",          0, high_pass,          T_BOOL,   0, 0,0),
    F ("agc",                0, agc,                T_BOOL,   0, 0,0),
    F ("ns",                 0, ns,                 T_INT,    0, 0,3),
    F ("alc_gain",           0, alc_gain,           T_INT,    0, 0,7),
    F ("agc_target_dbfs",    0, agc_target_dbfs,    T_INT,    0, 0,31),
    F ("agc_compression_db", 0, agc_compression_db, T_INT,    0, 0,90),
    F ("mute",               0, mute,               T_BOOL,   0, 0,0),
    F ("force_stereo",       0, force_stereo,       T_BOOL,   0, 0,0),
    F ("spk_enabled",        0, spk_enabled,        T_BOOL,   0, 0,0),
    F ("spk_volume",         0, spk_volume,         T_INT,    0, 0,100),
    F ("spk_gain",           0, spk_gain,           T_INT,    0, 0,100),
    F ("backchannel",        0, backchannel,        T_BOOL,   0, 0,0),
    F ("backchannel_codec",  0, backchannel_codec,  T_BCCODEC,0, 0,0),
    F ("backchannel_rate",   0, backchannel_rate,   T_INT,    0, 8000,48000),
    F ("aec",                0, aec,                T_BOOL,   0, 0,0),
};
#undef TT

#define TT ms_jpeg_cfg
static const cfg_field jpeg_fields[] = {
    F ("enabled",       0, enabled, T_BOOL, 0, 0,0),
    F ("width",         0, width,   T_INT,  0, 64,4096),
    F ("height",        0, height,  T_INT,  0, 64,4096),
    F ("quality",       0, quality, T_INT,  0, 1,100),
    F ("fps",           0, fps,     T_INT,  0, 1,120),
    F ("imp_chn",       0, imp_chn, T_INT,  0, 0,0),
    FS("snapshot_path", 0, snapshot_path, 0),
};
#undef TT

#define TT ms_config
static const cfg_field rtsp_fields[] = {   /* fields live directly in ms_config */
    F ("enabled",  0,             rtsp_enabled,  T_BOOL, 0, 0,0),
    F ("port",     0,             rtsp_port,     T_INT,  0, 1,65535),
    F ("mtu",      0,             rtsp_mtu,      T_INT,  0, 548,1472),
    FS("user",     "username",    rtsp_user,     0),
    FS("pass",     "password",    rtsp_pass,     0),
    F ("tls",      "tls_enabled", rtsp_tls,      T_BOOL, 0, 0,0),
    F ("tls_port", 0,             rtsp_tls_port, T_INT,  0, 1,65535),
};
static const cfg_field http_fields[] = {
    F ("enabled",     0,          http_enabled,     T_BOOL, 0, 0,0),
    F ("port",        0,          http_port,        T_INT,  0, 1,65535),
    F ("preview_chn", 0,          http_preview_chn, T_INT,  0, 0,0),
    F ("adaptive_drop", 0,        http_adaptive_drop, T_BOOL, 0, 0,0),
    FS("user",        "username", http_user,        0),
    FS("pass",        "password", http_pass,        0),
    FS("token",       0,          http_token,       0),
    FS("token_file",  0,          http_token_file,  0),
    F ("https",       "tls",      http_https,       T_BOOL, 0, 0,0),
    FS("tls_cert",    "cert",     http_tls_cert,    0),
    FS("tls_key",     "key",      http_tls_key,     0),
};
/* /events SSE push stream (startup settings, like the http.token* keys
 * deliberately not settable via /control) */
static const cfg_field events_fields[] = {
    F("enabled",     0, events_enabled,     T_BOOL, 0, 0,0),
    F("stats_ms",    0, events_stats_ms,    T_INT,  0, 0,0),
    F("max_clients", 0, events_max_clients, T_INT,  0, 0,0),
};
static const cfg_field general_fields[] = {   /* + general.syslog in set_kv() */
    F("loglevel",            0, loglevel,            T_INT, 0, 0,0),
    F("imp_polling_timeout", 0, imp_polling_timeout, T_INT, 0, 0,0),
    F("osd_pool_size",       0, osd_pool_size,       T_INT, 0, 0,0),
};
static const cfg_field sim_fields[] = {
    FS("video0", 0, sim_video0, 0),
    FS("video1", 0, sim_video1, 0),
    FS("audio",  0, sim_audio,  0),
    FS("jpeg",   0, sim_jpeg,   0),
};
#undef TT

#define TT ms_srt_cfg
static const cfg_field srt_fields[] = {
    F ("enabled",    0,         enabled,    T_BOOL, 0, 0,0),
    F ("port",       0,         port,       T_INT,  0, 1,65535),
    F ("channel",    0,         channel,    T_INT,  0, 0,0),
    F ("latency_ms", "latency", latency_ms, T_INT,  0, 0,0),
    FS("streamid",   0,         streamid,   0),
    FS("passphrase", 0,         passphrase, 0),
};
#undef TT

#define TT ms_osd_cfg
static const cfg_field osd_fields[] = {
    F ("enabled",        0, enabled,        T_BOOL, 0, 0,0),
    F ("monitor_stream", 0, monitor_stream, T_INT,  0, 0,0),
    FS("font_path",      0, font_path,      0),
    FS("vars_file",      0, vars_file,      0),
    F ("supersample",    0, supersample,    T_INT,  0, 1,4),
};
#undef TT

#define TT ms_motion_cfg
/* motion.cols/rows SET goes through explicit code in set_kv() (cross-axis
 * MOTION_CELL_LIMIT clamp); their table entries below only serve the GET
 * side. The deprecated motion.roi_* keys are entirely outside the table
 * (parse + one-shot warning in set_kv(), never readable). */
static const cfg_field motion_fields[] = {
    F ("enabled",        0, enabled,        T_BOOL, 0, 0,0),
    F ("monitor_stream", 0, monitor_stream, T_CHAN, 0, 0,0),
    F ("sensitivity",    0, sensitivity,    T_INT,  0, 0,255),
    F ("cols",           0, cols,           T_INT,  0, 0,0),
    F ("rows",           0, rows,           T_INT,  0, 0,0),
    /* M3: floor cooldown_ms so the on_motion hook cannot be re-exec'd on every
     * IVS result. IVS emits a result about every skip_frames/fps seconds - as
     * fast as ~200 ms at the defaults (skip_frames=5) - and the fork+exec'd hook
     * is deliberately not tracked, so with a 0/negative cooldown a hook slower
     * than that interval piles up unboundedly. 250 ms caps re-fires to at most
     * ~4/s (just above the fastest detection cadence) while still allowing
     * legitimate sub-second fast-alert use; 0 (disabled/no floor) is no longer
     * accepted. */
    F ("cooldown_ms",    0, cooldown_ms,    T_INT,  0, 250,INT_MAX),
    F ("hold_ms",        0, hold_ms,        T_INT,  0, 0,INT_MAX),
    F ("skip_frames",    0, skip_frames,    T_INT,  0, 1,INT_MAX),
    FS("on_motion",      0, on_motion,      F_NOGET),
};
#undef TT

#define TT ms_record_cfg
/* clamp notes: segment_s 0 keeps "no rotation" (documented); negative/absurd
 * rolls are rejected so a garbage value can't silently disable rotation.
 * The whole section is readable (M13): without get coverage every
 * record-page POST re-wrote /etc/timps.conf (flash wear). */
static const cfg_field record_fields[] = {
    F ("enabled",     0,           enabled,     T_BOOL,    0, 0,0),
    F ("channel",     0,           channel,     T_CHAN,    0, 0,0),
    F ("mode",        0,           mode,        T_RECMODE, 0, 0,0),
    FS("dir",         0,           dir,         0),
    FS("name",        0,           name,        0),
    F ("segment_s",   "segment",   segment_s,   T_INT,     0, 0,86400),
    F ("pre_roll_s",  "pre_roll",  pre_roll_s,  T_INT,     0, 0,60),
    F ("post_roll_s", "post_roll", post_roll_s, T_INT,     0, 0,300),
    F ("min_free_mb", 0,           min_free_mb, T_INT,     0, 0,1048576),
    F ("audio",       0,           audio,       T_BOOL,    0, 0,0),
};
#undef TT

#define TT ms_timelapse_cfg
static const cfg_field timelapse_fields[] = {
    F ("enabled",    0,          enabled,    T_BOOL, 0, 0,0),
    F ("channel",    0,          channel,    T_CHAN, 0, 0,0),
    FS("dir",        0,          dir,        0),
    FS("name",       0,          name,       0),
    F ("interval_s", "interval", interval_s, T_INT,  0, 1,INT_MAX),
    F ("keep_days",  0,          keep_days,  T_INT,  0, 0,INT_MAX),
};
#undef TT

#define TT ms_daynight_cfg
/* F3: the numeric keys used to reach the detection thread unclamped via
 * /control (pint/pflt raw). Nothing crashed (daynight.c guards interval_ms>0
 * and day_gain_pct>0), but a negative transition_s silently disabled the
 * dwell and a day_gain_pct>100 made the adaptive night->day trigger fire
 * inside gain jitter. Ranges: lat/lon geographic; sun offsets +-1 day;
 * gain thresholds in the IMP [24.8] linear scale (256=1x, defaults 300/3000,
 * cold-start transients ~20000 - 1e6 = ~3900x is far beyond any sensor);
 * threshold_low/high are brightness %; hysteresis is a 0..1 fraction of the
 * low..high band; day_gain_pct<=100 keeps the adaptive day trigger BELOW the
 * night baseline (0 = feature off, as before); interval_ms floor 100 keeps
 * the sampling loop from busy-spinning. */
static const cfg_field daynight_fields[] = {
    F ("enabled",                    0, enabled,                    T_BOOL,  0, 0,0),
    F ("mode",                       0, mode,                       T_DNMODE,0, 0,0),
    FS("time_night_start",           0, time_night_start,           0),
    FS("time_day_start",             0, time_day_start,             0),
    F ("sun_latitude",               0, sun_latitude,               T_FLT,   0, -90,90),
    F ("sun_longitude",              0, sun_longitude,              T_FLT,   0, -180,180),
    F ("sun_sunrise_offset_min",     0, sun_sunrise_offset_min,     T_INT,   0, -1440,1440),
    F ("sun_sunset_offset_min",      0, sun_sunset_offset_min,      T_INT,   0, -1440,1440),
    F ("total_gain_day_threshold",   0, total_gain_day_threshold,   T_FLT,   0, 1,1000000),
    F ("total_gain_night_threshold", 0, total_gain_night_threshold, T_FLT,   0, 1,1000000),
    F ("threshold_low",              0, threshold_low,              T_FLT,   0, 0,100),
    F ("threshold_high",             0, threshold_high,             T_FLT,   0, 0,100),
    F ("hysteresis",                 0, hysteresis,                 T_FLT,   0, 0,1),
    F ("day_gain_pct",               0, day_gain_pct,               T_INT,   0, 0,100),
    F ("baseline_delay_s",           0, baseline_delay_s,           T_INT,   0, 0,3600),
    F ("boot_settle_s",              0, boot_settle_s,              T_INT,   0, 0,600),
    F ("boot_settle_max_s",          0, boot_settle_max_s,          T_INT,   0, 0,3600),
    F ("boot_stable_pct",            0, boot_stable_pct,            T_INT,   0, 0,100),
    F ("night_reconfirm_s",          0, night_reconfirm_s,          T_INT,   0, 0,86400),
    F ("interval_ms",                0, interval_ms,                T_INT,   0, 100,60000),
    F ("transition_s",               0, transition_s,               T_INT,   0, 0,3600),
    FS("switch_cmd",                 0, switch_cmd,                 F_NOGET),
    FS("isp_path",                   0, isp_path,                   F_NOGET),
};
#undef TT

#define TT ms_vstream_cfg
/* videoN.buffers additionally sets buffers_explicit=1 in set_kv() */
static const cfg_field video_fields[] = {
    F ("enabled",      0,              enabled,      T_BOOL,  0,       0,0),
    F ("codec",        0,              codec,        T_VCODEC,0,       0,0),
    F ("width",        0,              width,        T_INT,   0,       64,4096),
    F ("height",       0,              height,       T_INT,   0,       64,4096),
    F ("fps",          0,              fps,          T_INT,   0,       1,120),
    F ("bitrate",      0,              bitrate_kbps, T_INT,   0,       16,50000),
    F ("rc_mode",      "mode",         rc_mode,      T_RC,    0,       0,0),
    F ("gop",          0,              gop,          T_INT,   0,       1,1000),
    F ("max_gop",      0,              max_gop,      T_INT,   0,       1,1000),
    F ("profile",      0,              profile,      T_INT,   0,       0,2),
    F ("qp",           0,              qp,           T_INT,   0,       1,51),
    F ("min_qp",       0,              min_qp,       T_INT,   0,       1,51),
    F ("max_qp",       0,              max_qp,       T_INT,   0,       1,51),
    F ("rotation",     0,              rotation,     T_ROT,   0,       0,0),
    F ("buffers",      0,              buffers,      T_INT,   0,       1,8),
    FS("rtsp_path",    0,              rtsp_path,    0),
    F ("imp_chn",      0,              imp_chn,      T_INT,   F_NOGET, 0,0),
    F ("jpeg",         "jpeg_enabled", jpeg_enabled, T_BOOL,  F_NOGET, 0,0),
    F ("jpeg_quality", 0,              jpeg_quality, T_INT,   F_NOGET, 1,100),
    F ("jpeg_fps",     0,              jpeg_fps,     T_INT,   F_NOGET, 1,120),
    F ("jpeg_chn",     0,              jpeg_chn,     T_INT,   F_NOGET, 0,0),
};
#undef TT

#define TT ms_osd_item
static const cfg_field osd_item_fields[] = {
    F ("enabled",       0,              enabled,       T_BOOL,   0,       0,0),
    F ("type",          0,              type,          T_OSDTYPE,0,       0,0),
    FS("text",          0,              text,          0),
    FS("logo",          "logo_path",    logo_path,     F_NOGET),
    F ("logo_w",        "logo_width",   logo_w,        T_INT,    F_NOGET, 0,4096),
    F ("logo_h",        "logo_height",  logo_h,        T_INT,    F_NOGET, 0,4096),
    F ("x",             0,              x,             T_INT,    0,       0,0),
    F ("y",             0,              y,             T_INT,    0,       0,0),
    /* H4: font_size feeds the OSD canvas allocation (msttf_render); clamped
     * at parse so a bad /control write can never request an absurd raster
     * (the rasterizer additionally hard-clamps its own pixel height) */
    F ("font_size",     0,              font_size,     T_INT,    0,       8,256),
    F ("color",         "font_color",   color,         T_HEX,    0,       0,0),
    /* imp_osd.c feeds this straight into the group attr's uint8_t fgAlhpa:
     * clamp so e.g. 300 doesn't wrap to 44 while the config echoes 300 */
    F ("transparency",  0,              transparency,  T_INT,    0,       0,255),
    F ("outline",       "stroke",       outline,       T_INT,    0,       0,64),
    F ("outline_color", "stroke_color", outline_color, T_HEX,    0,       0,0),
    FS("font_path",     0,              font_path,     F_NOGET),
};
#undef TT

#define TT ms_privacy_region
static const cfg_field privacy_fields[] = {
    F("enabled", 0,            enabled, T_BOOL, 0, 0,0),
    F("x",       0,            x,       T_INT,  0, 0,0),
    F("y",       0,            y,       T_INT,  0, 0,0),
    F("w",       "width",      w,       T_INT,  0, 0,0),
    F("h",       "height",     h,       T_INT,  0, 0,0),
    F("color",   "fill_color", color,   T_HEX,  0, 0,0),
};
#undef TT
#undef F
#undef FS

/* prefix -> {base struct, field table}. Linear scan: ~16 sections and ~10
 * fields/section on a path that runs on startup parse and /control POSTs
 * only - not hot. noget marks whole set/persist-only sections (the old
 * getter had no branch for them at all; same dedup rationale as F_NOGET). */
typedef struct {
    const char     *prefix;    /* includes the trailing dot */
    unsigned char   plen;
    unsigned char   noget;
    unsigned short  base_off;  /* offset of the section base in ms_config */
    const cfg_field *fields;
    unsigned char   nfields;
} cfg_section;

#define NF(t) (unsigned char)(sizeof t / sizeof t[0])
#define SEC(pfx, ng, boff, tbl) \
    { pfx, (unsigned char)(sizeof pfx - 1), ng, (unsigned short)(boff), tbl, NF(tbl) }

static const cfg_section g_sections[] = {
    SEC("sensor.",    0, offsetof(ms_config,sensor),    sensor_fields),
    SEC("image.",     0, offsetof(ms_config,image),     image_fields),
    SEC("audio.",     0, offsetof(ms_config,audio),     audio_fields),
    SEC("jpeg.",      1, offsetof(ms_config,jpeg),      jpeg_fields),
    SEC("rtsp.",      1, 0,                             rtsp_fields),
    SEC("srt.",       1, offsetof(ms_config,srt),       srt_fields),
    SEC("http.",      1, 0,                             http_fields),
    SEC("events.",    1, 0,                             events_fields),
    SEC("osd.",       0, offsetof(ms_config,osd),       osd_fields),
    SEC("motion.",    0, offsetof(ms_config,motion),    motion_fields),
    SEC("record.",    0, offsetof(ms_config,record),    record_fields),
    SEC("timelapse.", 0, offsetof(ms_config,timelapse), timelapse_fields),
    SEC("daynight.",  0, offsetof(ms_config,daynight),  daynight_fields),
    SEC("general.",   1, 0,                             general_fields),
    SEC("sim.",       1, 0,                             sim_fields),
};
#undef SEC

static const cfg_field *field_find(const cfg_field *t, int n, const char *k)
{
    for (int i=0;i<n;i++)
        if (!strcmp(k,t[i].name) || (t[i].alias && !strcmp(k,t[i].alias)))
            return &t[i];
    return NULL;
}

static const cfg_section *section_find(const char *key, const char **field)
{
    for (size_t i=0;i<sizeof g_sections/sizeof g_sections[0];i++)
        if (!strncmp(key, g_sections[i].prefix, g_sections[i].plen)){
            *field = key + g_sections[i].plen;
            return &g_sections[i];
        }
    return NULL;
}

static void field_set(void *base, const cfg_field *f, const char *val)
{
    void *p = (char*)base + f->off;
    switch (f->type){
    case T_BOOL:   *(int*)p = pbool(val); break;
    case T_INT:    *(int*)p = (f->lo < f->hi) ? pint_cl(val,f->lo,f->hi)
                                              : pint(val); break;
    case T_CHAN: { int v = pint(val);
                   *(int*)p = (v<0 || v>=MS_MAX_VSTREAM) ? 0 : v; break; }
    case T_HEX:    *(uint32_t*)p = phex(val); break;
    case T_FLT:    *(float*)p = (f->lo < f->hi) ? pflt_cl(val,(float)f->lo,(float)f->hi)
                                                : pflt(val); break;
    case T_STR:    copystr((char*)p, val, (size_t)f->hi); break;
    case T_VCODEC: *(int*)p = pvcodec(val); break;
    case T_ACODEC: *(int*)p = pacodec(val); break;
    case T_RC:     *(int*)p = prc(val); break;
    case T_ROT:    *(int*)p = prot(val); break;
    case T_OSDTYPE:*(int*)p = (!strcasecmp(val,"logo")) ? MS_OSD_LOGO
                                                        : MS_OSD_TEXT; break;
    case T_DNMODE:
        if      (!strcmp(val,"sensor")) *(int*)p = DN_MODE_SENSOR;
        else if (!strcmp(val,"time"))   *(int*)p = DN_MODE_TIME;
        else if (!strcmp(val,"sun"))    *(int*)p = DN_MODE_SUN;
        else { LOGW(MOD,"daynight.mode: unknown '%s', keeping sensor",val);
               *(int*)p = DN_MODE_SENSOR; }
        break;
    case T_RECMODE:
        *(int*)p = (!strcasecmp(val,"motion"))     ? 1 :
                   (!strcasecmp(val,"continuous")) ? 0 : pint(val);
        break;
    case T_BCCODEC:
        if      (!strcasecmp(val,"pcmu")) *(int*)p = 0;
        else if (!strcasecmp(val,"pcma")) *(int*)p = 1;
        else if (!strcasecmp(val,"aac"))  *(int*)p = 2;
        else *(int*)p = pint_cl(val,0,2);
        break;
    }
}

/* read one field back as its normalized string; 0 = not readable (F_NOGET) */
static int field_get(const void *base, const cfg_field *f, char *out, size_t cap)
{
    if (f->flags & F_NOGET) return 0;
    const void *p = (const char*)base + f->off;
    switch (f->type){
    case T_BOOL: case T_INT: case T_CHAN: case T_ROT:
    case T_RECMODE: case T_BCCODEC:
        snprintf(out,cap,"%d",*(const int*)p); break;
    case T_HEX:    snprintf(out,cap,"0x%08X",*(const uint32_t*)p); break;
    case T_FLT:    snprintf(out,cap,"%g",(double)*(const float*)p); break;
    case T_STR:
        /* several strings are runtime-mutable via /control (see the
         * g_str_lock comment at the top); taking the recursive lock
         * uniformly for EVERY string read is cheap and keeps the mutable
         * set covered by construction */
        config_str_lock();
        snprintf(out,cap,"%s",(const char*)p);
        config_str_unlock();
        break;
    case T_VCODEC: snprintf(out,cap,"%s",vcodec_name(*(const int*)p)); break;
    case T_ACODEC: snprintf(out,cap,"%s",acodec_name(*(const int*)p)); break;
    case T_RC:     snprintf(out,cap,"%s",rc_name(*(const int*)p)); break;
    case T_OSDTYPE:snprintf(out,cap,"%s",
                       (*(const int*)p==MS_OSD_LOGO)?"logo":"text"); break;
    case T_DNMODE: { int m = *(const int*)p;
        snprintf(out,cap,"%s",
                 m==DN_MODE_TIME?"time":m==DN_MODE_SUN?"sun":"sensor"); break; }
    }
    return 1;
}

static void set_kv(ms_config *c, const char *key, const char *val)
{
    int osi, oii;
    const char *ok = osd_key(key, &osi, &oii);
    if (ok){
        const cfg_field *f = field_find(osd_item_fields, NF(osd_item_fields), ok);
        if (!f){ LOGW(MOD,"unknown osd item key %s", ok); return; }
        if (osi>=0) field_set(&c->osd.items[osi][oii], f, val);
        else        /* legacy osdN.*: mirror onto every stream's item N */
            for (int s=0;s<MS_MAX_VSTREAM;s++)
                field_set(&c->osd.items[s][oii], f, val);
        return;
    }

    int psi, pii;
    const char *pk = privacy_key(key, &psi, &pii);
    if (pk){
        const cfg_field *f = field_find(privacy_fields, NF(privacy_fields), pk);
        if (f) field_set(&c->privacy[psi][pii], f, val);
        else   LOGW(MOD,"unknown privacy key %s", pk);
        return;
    }

    if (!strncmp(key,"video0.",7) || !strncmp(key,"video1.",7)){
        ms_vstream_cfg *v = &c->video[key[5]-'0'];
        const cfg_field *f = field_find(video_fields, NF(video_fields), key+7);
        if (!f){ LOGW(MOD,"unknown video key %s", key+7); return; }
        field_set(v, f, val);
        /* buffers given explicitly: HAL safety clamps (e.g. T31 non-scaled
         * channel) trust it as-is instead of overriding */
        if (f->off == offsetof(ms_vstream_cfg,buffers)) v->buffers_explicit = 1;
        return;
    }

    /* per-key logic that doesn't fit the generic table */
    if (!strncmp(key,"motion.",7)){
        const char *k = key+7;
        if (!strcmp(k,"cols")||!strcmp(k,"rows")){
            /* grid geometry: >=1 per axis and cols*rows clamped to the SDK's
             * ROI budget (MOTION_CELL_LIMIT). The value BEING SET is clamped
             * against the current other axis (never the other way around),
             * so re-applying the same pair is idempotent - the /control
             * dedup then skips repeated posts instead of rewriting flash. */
            int v2 = pint(val);
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
            return;
        }
        if (!strcmp(k,"roi_x")||!strcmp(k,"roi_y")||
            !strcmp(k,"roi_w")||!strcmp(k,"roi_h")){
            /* B6: legacy single-ROI keys - still parsed/persisted for compat
             * but NOTHING consumes them anymore (the cell grid replaced them).
             * Warn once when a non-default (non-zero) value is set so a user
             * isn't silently losing an ROI restriction they think is active. */
            int v2 = pint(val);
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
            return;
        }
        /* every other motion.* key: generic table below */
    }
    if (!strcmp(key,"general.syslog")){          /* to logread; default on */
        log_set_syslog(pbool(val));
        return;
    }

    const char *k;
    const cfg_section *s = section_find(key, &k);
    if (s){
        const cfg_field *f = field_find(s->fields, s->nfields, k);
        if (f) field_set((char*)c + s->base_off, f, val);
        else   LOGW(MOD,"unknown key %s", key);
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
 * form set_kv() stores. Coverage comes from the same descriptor tables set_kv()
 * writes through; keys/sections the old hand-written getter did not cover stay
 * unreadable via F_NOGET / section noget (change-detection then falls back to
 * always applying them). Returns 0 for anything unknown. */
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
        const cfg_field *f = field_find(osd_item_fields, NF(osd_item_fields), ok);
        return f ? field_get(&c->osd.items[osi][oii], f, out, cap) : 0;
    }

    int psi, pii;
    const char *pk = privacy_key(key, &psi, &pii);
    if (pk){
        const cfg_field *f = field_find(privacy_fields, NF(privacy_fields), pk);
        return f ? field_get(&c->privacy[psi][pii], f, out, cap) : 0;
    }

    if (!strncmp(key,"video0.",7) || !strncmp(key,"video1.",7)){
        const cfg_field *f = field_find(video_fields, NF(video_fields), key+7);
        return f ? field_get(&c->video[key[5]-'0'], f, out, cap) : 0;
    }

    const char *k;
    const cfg_section *s = section_find(key, &k);
    if (s && !s->noget){
        const cfg_field *f = field_find(s->fields, s->nfields, k);
        if (f) return field_get((const char*)c + s->base_off, f, out, cap);
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
