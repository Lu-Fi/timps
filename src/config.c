#include "config.h"
#include "daynight.h"       /* DN_* fixed constants, for the config-file grace-
                             * period warning on the keys that hardcoded them */
#include "log.h"
#include "trace.h"         /* general.trace/general.trace_ms (side-effect keys) */
#include "motion_caps.h"   /* MOTION_MAX_CELLS/MOTION_CELL_LIMIT (grid clamp) */
#include "rotate_caps.h"   /* ROT_HAS_90/ROT_HAS_HW_I2D (rotation whitelist) */
#include "isp_caps.h"      /* ISP_HAS_* (image_fields[]'s F_CAP gating) */
#include "audio_caps.h"    /* AUDIO_HAS_* (audio_fields[]'s F_CAP gating) */
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
#include <stdatomic.h>  /* atomic_load/atomic_store for F_ATOMIC live-int fields */

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
 * A6: the live-mutable int/enum fields in g_cfg (the numeric part of any
 * section a /control POST can touch - image.*, audio.*, sensor.*, record.*,
 * timelapse.*, daynight.*, motion.*, ...) MUST ALSO be read under this lock (or
 * be _Atomic). Aligned 32-bit loads do not TEAR on this target, but a lock-free
 * read of a field the /control writer is concurrently mutating is still a C11
 * data race / UB - see the doctrine in config.h. A field that is only ever
 * written at startup (config_load/config_sensor_finalize, single-threaded
 * before any other thread runs) genuinely needs no lock; a live-mutable one
 * does. The one live int read lock-free on a hot path (audio.mute in the
 * per-frame audio worker) is _Atomic instead of taking the lock every frame.
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
/* video codec parser. Like prot() above, unsupported values are coerced HERE
 * (at parse/apply time) so the stored value - and therefore /control read-back -
 * reflects what the encoder will actually produce. T10/T20 have no H.265
 * encoder at all (hal_ingenic.c hardcodes PT_H264 there), so accepting h265
 * would leave v->codec=MS_VC_H265 while the HAL emits H.264; rtsp.c then keys
 * its isH265/rtp_send_h265/keyframe logic off the config value and streams the
 * H.264 bytes mislabelled as H.265, which breaks players. Coerce to h264 with a
 * warning instead of letting that mismatch surface downstream. */
static int  pvcodec(const char *v){
    int c = (!strcasecmp(v,"h265")||!strcasecmp(v,"hevc")) ? MS_VC_H265 : MS_VC_H264;
/* T23 belongs here too: its vendored SDK marks every H.265 rc struct as
 * unsupported (see T23 1.3.0 imp_encoder.h, and hal_ingenic.c says
 * so as well), so IMP_Encoder_CreateChn fails, ing_start() returns -1 and
 * main() exits - one config line takes the WHOLE daemon down instead of
 * costing one stream. Coerce and warn, exactly as T10/T20 already do. */
#if defined(PLATFORM_T10)||defined(PLATFORM_T20)||defined(PLATFORM_T23)
    if (c==MS_VC_H265){ LOGW(MOD,"codec h265 unsupported on this SoC -> h264"); c=MS_VC_H264; }
#endif
    return c;
}
static int  pacodec(const char *v){
    if (!strcasecmp(v,"aac")) return MS_AC_AAC;
    if (!strcasecmp(v,"pcmu")||!strcasecmp(v,"g711u")||!strcasecmp(v,"ulaw")) return MS_AC_PCMU;
    if (!strcasecmp(v,"pcma")||!strcasecmp(v,"g711a")||!strcasecmp(v,"alaw")) return MS_AC_PCMA;
#ifdef USE_STREAM_OPUS
    /* Opus as an RTSP/RTP streaming codec (RFC 7587). Only accepted on builds
     * compiled with USE_STREAM_OPUS; otherwise "opus" is an unrecognized codec
     * and falls through to the default (MS_AC_AAC) below, exactly like any other
     * unknown string, so no opus code path is ever reached on a build without
     * the feature. Independent of USE_PLAY_OPUS (local .opus file playback). */
    if (!strcasecmp(v,"opus")) return MS_AC_OPUS;
#endif
    if (!strcasecmp(v,"none")||!strcasecmp(v,"off")) return MS_AC_NONE;
    return MS_AC_AAC;
}
/* canonical config-file spelling of an audio codec (inverse of pacodec) */
static const char *acodec_name(int c){
    switch (c){
        case MS_AC_AAC:  return "aac";
        case MS_AC_PCMU: return "pcmu";
        case MS_AC_PCMA: return "pcma";
#ifdef USE_STREAM_OPUS
        case MS_AC_OPUS: return "opus";
#endif
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
    c->debug_modules[0] = 0;
    c->imp_polling_timeout = 500;
    c->osd_pool_size = 1024;   /* max on T-series; holds small OSD regions */

    /* sensor.* start UNSET so config_sensor_finalize() can auto-detect them
     * from /proc/jz/sensor/<X>/ (raptor/prudynt style); a config value or,
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
                                  * healthy link (cam-A) and correctly freezing/
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
    copystr(c->srt.mode,"listener",sizeof c->srt.mode);
    c->srt.host[0]=0; c->srt.streamid[0]=0; c->srt.passphrase[0]=0;

    for (int i=0;i<MS_MAX_VSTREAM;i++){
        ms_vstream_cfg *v=&c->video[i];
        v->codec=MS_VC_H264; v->fps=25; v->rc_mode=MS_RC_CBR;
        v->gop=50; v->max_gop=60; v->profile=2; v->qp=35; v->min_qp=20; v->max_qp=45;
        /* the values the classic rc fills used as literals before these became
         * config keys, so an unset config keeps the previous encoder behaviour */
        v->quality_lvl=2; v->change_pos=80; v->i_bias_lvl=0; v->fluc_lvl=0;
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
    c->osd.enabled=1; c->osd.monitor_stream=0; c->osd.supersample=2; c->osd.hinting=1;
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

    /* automatic day/night, see the ms_daynight_cfg doc comment in config.h
     * and dev_notes/DAYNIGHT_REDESIGN_2026-08-17.md */
    c->daynight.enabled=1;
    c->daynight.mode=DN_MODE_AUTO;   /* sensor + probes; calendar optional */
    c->daynight.time_night_start[0]=0; c->daynight.time_day_start[0]=0;
    c->daynight.sun_latitude=0.0f; c->daynight.sun_longitude=0.0f;
    c->daynight.sun_sunrise_offset_min=0; c->daynight.sun_sunset_offset_min=0;
    /* 300 -> 768 and 3000 -> 4096 (2026-08-16). Both are now expressed as
     * multiples of the [24.8] gain floor (256 = 1.0x), which is the scale the
     * decision actually lives on, instead of round decimals nobody could
     * justify: day is confirmed when the DAY pipeline can hold the scene at
     * <= 3x gain, night is entered when it needs > 16x.
     *
     * 300 was 1.17x - it asserted "day only when the day pipeline needs
     * essentially no gain", i.e. outdoor daylight. Indoors that is simply
     * wrong: a normally-lit room in daytime needs 2-3x, so the comparison
     * `tg < total_gain_day_threshold` could never come true, every probe
     * reverted, and the camera sat in night mode in daylight. Five of twelve
     * fleet cameras needed a manual override on 2026-08-16, and the two
     * diagnosed in detail measured 531 (2.07x) and 2505 (9.79x) as the BEST
     * their day pipelines managed.
     *
     * 3x is deliberately modest and does not pretend to fix the extreme end.
     * It clears the marginal cases - cam-E at 2.07x, cam-D at
     * ~2.7x - which are the ones a default can reasonably be expected to
     * cover, and leaves genuinely dim rooms like cam-F (9.79x) as
     * explicit per-camera overrides, which is honest: no single default fits
     * a 9.79x room and a sunlit hallway. Picking the fleet's lowest observed
     * override (800) would be fitting to five samples; 3x is a statable
     * premise about indoor light that happens to cover them.
     *
     * The night threshold moves with it to keep the hysteresis band sane:
     * 768..4096 is 5.3x, against 300..3000's 10x. Narrower, but still over
     * two stops of dead-zone, and the band is not free - every 1x of it is
     * a range where the machine holds its current mode on a reading it
     * cannot classify. 16x is a defensible "colour is hopeless" point.
     * Per-camera tuning remains the answer for any specific room; enable
     * daynight.diagnose_thresholds to be told what to set it to.
     *
     * These two numbers survive the 2026-08-17 redesign unchanged in value
     * AND in meaning: the exposure index equals total_gain whenever the AE
     * has the integration time railed at max, which is exactly the dark end
     * where both thresholds were calibrated. What changes is that a BRIGHT
     * scene now reads far below 256 instead of sticking at it, so 768 has a
     * great deal more margin under it than it used to. */
    c->daynight.day_gain=768.0f;
    c->daynight.night_gain=4096.0f;
    c->daynight.day_confirm_s=30;
    /* Probe economy. 600 s between probes bounds the audible cost at one
     * click pair per 10 minutes under every combination of triggers; 15 s of
     * confirm rejects headlights and passing cars while keeping "light on ->
     * colour" at roughly 25 s end to end (the other half of that number,
     * probe_jump_pct's 50% and probe_settle_s's 8 s, are now the fixed
     * DN_PROBE_JUMP_PCT/DN_PROBE_SETTLE_S constants in daynight.h - see there
     * for why: the 2026-08-22 consolidation found every camera measured
     * wanted the same values, same as ref_delay_s below and the silent
     * probe's three fields further down). */
    c->daynight.probe_min_gap_s=600;
    c->daynight.probe_confirm_s=15;
    /* Silent probe OFF by default: it needs a board command that can drive the
     * illuminator on its own, and a wrong one would leave the scene dark. Set
     * daynight.irprobe_cmd to enable it.
     *
     * ir_ratio_night/ir_ratio_day/ir_min_headroom used to have their own
     * defaults here (2.0/2.0/8, re-derived 2026-08-19 from a twelve-camera
     * dusk-to-dawn campaign). All three are now the fixed
     * DN_IR_RATIO_NIGHT/DN_IR_RATIO_DAY/DN_IR_MIN_HEADROOM constants in
     * daynight.h, which carries the full campaign writeup. */
    /* Default ON since 2026-08-19. It was empty because "a wrong command would
     * leave the scene dark", and that risk is real - the daemon restores the
     * illuminator after the probe but has no failsafe if it dies inside the 8 s
     * dark window. The default now points at a helper that arms a detached
     * 60 s watchdog before switching off, so the light comes back even then,
     * and that exits non-zero on a board with no switchable illuminator, which
     * timps logs and skips.
     *
     * Leaving it off was costing more than it saved: three cameras sat in night
     * mode through an entire afternoon because night->day fell back to the
     * absolute threshold, which cannot separate a dark room from night - the
     * measured spread of genuine daylight across this fleet is a factor of 63.
     * The IR ratio settles it in one silent probe. Set to empty to disable. */
    snprintf(c->daynight.irprobe_cmd, sizeof c->daynight.irprobe_cmd,
             "timps-irprobe");
    /* The heartbeat is the only bound on a wrong night, so it is a flat
     * interval, never a multiplier: 4 h while the scene is moving, 12 h once
     * it demonstrably is not. A dark closet therefore costs two click pairs
     * a day and a camera that sees anything at all costs six - both without
     * any evidence-gating that could erode the guarantee. */
    c->daynight.heartbeat_s=14400;
    c->daynight.heartbeat_max_s=43200;
    /* one probe per boot buys a MEASUREMENT instead of a stale opinion. 0
     * adopts the persisted mode without measuring - but boot asserts the
     * mode it ends up with on the board either way, which is the half a
     * dark-time reboot needs (2026-08-22 IR/ircut desync). */
    c->daynight.boot_probe=1;
    /* boot_settle_s used to default to 5 here; now the fixed DN_BOOT_SETTLE_S
     * constant in daynight.h. */
    /* 2000 ms, not the old 500: the exposure index needs the /proc scrape on
     * every decision tick (the integration time has no IMP API), and no
     * confirmation window in the automaton is shorter than 8 s. This is still
     * well under half the scrape rate the pre-2026-07-31 code ran at. */
    c->daynight.interval_ms=2000;
    /* transition_s (default 5) and ref_delay_s (default 30) used to be set
     * here too; both are now the fixed DN_TRANSITION_S/DN_REF_DELAY_S
     * constants in daynight.h. daynight.learn (default 0) and its
     * state_path (default /etc/timps-daynight.state) are gone outright, not
     * hardcoded - see the note above switch_cmd in config.h for why. */
    copystr(c->daynight.switch_cmd,"daynight",sizeof c->daynight.switch_cmd);
    copystr(c->daynight.isp_path,"/proc/jz/isp/isp-m0",sizeof c->daynight.isp_path);
    c->daynight.trace_path[0]=0;   /* trace recorder off by default */
    c->daynight.diagnose_thresholds=0; /* threshold warns off by default */

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

/* F_NOGET/F_ATOMIC/F_CTRL and the cfg_field struct itself now live in
 * config.h (moved there so control.c's /control POST handling can walk these
 * same tables via the cfg_fields_*() accessors near the bottom of this file,
 * instead of hand-listing field names a second time - see the F_CTRL doc
 * comment in config.h for the security-allowlist rationale). F_NOGET's "don't
 * drop without checking /control" and F_ATOMIC's "T_BOOL/T_INT only,
 * genuinely lock-free hot-path reads only" rules still apply exactly as
 * before; nothing about their behavior changed. */

/* entry helpers: F = generic field, FS = string field (size from the struct) */
#define F(nm,al,fld,ty,fl,LO,HI) \
    { nm, al, (unsigned short)offsetof(TT,fld), ty, fl, LO, HI }
#define FS(nm,al,fld,fl) \
    { nm, al, (unsigned short)offsetof(TT,fld), T_STR, fl, 0, \
      (int)sizeof ((TT*)0)->fld }

#define TT ms_sensor_cfg
/* every sensor.* key is F_CTRL (POST-able), matching the old SENSOR[] array
 * in control.c verbatim: persist-only (applied at the next ISP init), like
 * videoN.*. */
static const cfg_field sensor_fields[] = {
    FS("model",     0,             model, F_CTRL),
    /* F-01: sensor.* is POSTable via /control and persists to timps.conf, so a
     * garbage numeric (e.g. width=70000, i2c_addr=-5) would survive a reboot and
     * feed bad values into the ISP init -> respawn crash-loop (the registry
     * override at config_finalize only guards sensor.model, not the numerics).
     * Clamp like videoN.*; lo=0 keeps 0 meaning "auto". */
    F ("i2c_addr",  "i2c_address", i2c_addr, T_INT, F_CTRL, 0,0x7F),
    F ("fps",       0,             fps,      T_INT, F_CTRL, 0,120),
    F ("width",     0,             width,    T_INT, F_CTRL, 0,8192),
    F ("height",    0,             height,   T_INT, F_CTRL, 0,8192),
};
#undef TT

#define TT ms_image_cfg
/* every image.* key is F_CTRL (POST-able): accepted regardless of SoC
 * support, the HAL just skips what the platform cannot do at apply time.
 * F_CAP mirrors isp_caps.h's ISP_HAS_* matrix - the SAME conditions
 * hal_ingenic.c's isp_apply_image() guards its IMP_ISP_Tuning_Set* calls
 * with - so control.c's GET /control caps builder can walk this one table
 * instead of re-listing every name under a second copy of these #ifdefs (the
 * old hand-written IMG_CAPS[] array in control.c). Base fields with no
 * platform gate (present on every SoC per isp_caps.h's header comment) get
 * F_CAP unconditionally. */
#ifdef ISP_HAS_HUE
#define CAP_HUE F_CAP
#else
#define CAP_HUE 0
#endif
#ifdef ISP_HAS_AECOMP
#define CAP_AECOMP F_CAP
#else
#define CAP_AECOMP 0
#endif
#ifdef ISP_HAS_GAINS
#define CAP_GAINS F_CAP
#else
#define CAP_GAINS 0
#endif
#ifdef ISP_HAS_NR
#define CAP_NR F_CAP
#else
#define CAP_NR 0
#endif
#ifdef ISP_HAS_DPC
#define CAP_DPC F_CAP
#else
#define CAP_DPC 0
#endif
#ifdef ISP_HAS_DEFOG
#define CAP_DEFOG F_CAP
#else
#define CAP_DEFOG 0
#endif
#ifdef ISP_HAS_DRC
#define CAP_DRC F_CAP
#else
#define CAP_DRC 0
#endif
#ifdef ISP_HAS_HILIGHT
#define CAP_HILIGHT F_CAP
#else
#define CAP_HILIGHT 0
#endif
#ifdef ISP_HAS_BACKLIGHT
#define CAP_BACKLIGHT F_CAP
#else
#define CAP_BACKLIGHT 0
#endif
#ifdef ISP_HAS_WB
#define CAP_WB F_CAP
#else
#define CAP_WB 0
#endif
static const cfg_field image_fields[] = {
    F("brightness",             0, brightness,             T_INT, F_CTRL|F_CAP,          0,255),
    F("contrast",               0, contrast,               T_INT, F_CTRL|F_CAP,          0,255),
    F("saturation",             0, saturation,             T_INT, F_CTRL|F_CAP,          0,255),
    F("sharpness",              0, sharpness,              T_INT, F_CTRL|F_CAP,          0,255),
    F("hue",                    0, hue,                    T_INT, F_CTRL|CAP_HUE,        0,255),
    F("vflip",                  0, vflip,                  T_BOOL,F_CTRL|F_CAP,          0,0),
    F("hflip",                  0, hflip,                  T_BOOL,F_CTRL|F_CAP,          0,0),
    F("running_mode",           0, running_mode,           T_INT, F_CTRL|F_CAP,          0,1),   /* F-09 */
    F("anti_flicker",           0, anti_flicker,           T_INT, F_CTRL|F_CAP,          0,2),   /* F-09 */
    F("ae_compensation",        0, ae_compensation,        T_INT, F_CTRL|CAP_AECOMP,     0,255),
    F("max_again",              0, max_again,              T_INT, F_CTRL|CAP_GAINS,      0,255),
    F("max_dgain",              0, max_dgain,              T_INT, F_CTRL|CAP_GAINS,      0,255),
    F("sinter_strength",        0, sinter_strength,        T_INT, F_CTRL|CAP_NR,         0,255),
    F("temper_strength",        0, temper_strength,        T_INT, F_CTRL|CAP_NR,         0,255),
    F("dpc_strength",           0, dpc_strength,           T_INT, F_CTRL|CAP_DPC,        0,255),
    F("defog_strength",         0, defog_strength,         T_INT, F_CTRL|CAP_DEFOG,      0,255),
    F("drc_strength",           0, drc_strength,           T_INT, F_CTRL|CAP_DRC,        0,255),
    F("highlight_depress",      0, highlight_depress,      T_INT, F_CTRL|CAP_HILIGHT,    0,10),
    F("backlight_compensation", 0, backlight_compensation, T_INT, F_CTRL|CAP_BACKLIGHT,  0,10),
    F("core_wb_mode",           0, core_wb_mode,           T_INT, F_CTRL|CAP_WB,         0,1),   /* F-09 */
    F("wb_rgain",               0, wb_rgain,               T_INT, F_CTRL|CAP_WB,         0,65535),
    F("wb_bgain",               0, wb_bgain,               T_INT, F_CTRL|CAP_WB,         0,65535),
};
#undef CAP_HUE
#undef CAP_AECOMP
#undef CAP_GAINS
#undef CAP_NR
#undef CAP_DPC
#undef CAP_DEFOG
#undef CAP_DRC
#undef CAP_HILIGHT
#undef CAP_BACKLIGHT
#undef CAP_WB
#undef TT

#define TT ms_audio_cfg
/* every audio.* key is F_CTRL (POST-able) - this table is the union of the
 * old hand-written AUD_LIVE[] (applied to the running AI/AO immediately:
 * volume/gain/alc_gain/mute/spk_volume/spk_gain/aec) and AUD_REST[] (persist-
 * only, SetPubAttr/encoder-init attributes that apply at the next restart).
 * That live-vs-restart distinction is a HAL-side concern (see hub.c's audio
 * branch), not a POST-reachability one, so both classes carry the same flag
 * here; control.c's generic walker no longer needs to know which is which.
 *
 * F_CAP is a DIFFERENT, narrower axis: it marks the subset the old
 * hand-written AUD_CAPS[] array in control.c advertised via GET /control's
 * "caps":{"audio":[...]}. Unlike image_fields[], this is NOT just a hardware
 * gate - codec/samplerate/bitrate/channels/enabled/force_stereo/spk_enabled/
 * backchannel(_codec/_rate)/high_pass/agc/ns/agc_target_dbfs/agc_compression_db are all
 * fully F_CTRL POST-able and persist correctly, but stay WITHOUT F_CAP on
 * every platform: they are restart-required or persist-only (see hub.c's
 * audio branch / ai_apply_key() in hal_ingenic.c), so the WebUI is meant to
 * show them as "applies on restart", never as a live control. Only
 * volume/gain/mute are always F_CAP; alc_gain/spk_volume/spk_gain/aec are
 * additionally hardware/feature gated below, matching AUD_CAPS[]'s old
 * #ifdefs exactly. */
#ifdef AUDIO_HAS_ALC_GAIN
#define CAP_ALC F_CAP
#else
#define CAP_ALC 0
#endif
#if defined(USE_PLAY) || defined(USE_BACKCHANNEL)
#define CAP_SPK F_CAP
#else
#define CAP_SPK 0
#endif
static const cfg_field audio_fields[] = {
    F ("enabled",            0, enabled,            T_BOOL,   F_CTRL, 0,0),
    F ("codec",              0, codec,              T_ACODEC, F_CTRL, 0,0),
    F ("samplerate",         0, samplerate,         T_INT,    F_CTRL, 8000,96000),   /* F-09 */
    /* 1 = mono (native), 2 = simulated stereo (mono mic duplicated to L=R,
     * AAC only) - anything else would put a bogus channel count in the AAC
     * ASC / SDP / fMP4 stsd */
    F ("channels",           0, channels,           T_INT,    F_CTRL, 1,2),
    F ("bitrate",            0, bitrate_kbps,       T_INT,    F_CTRL, 8,320),
    F ("volume",             0, volume,             T_INT,    F_CTRL|F_CAP, 0,100),
    F ("gain",               0, gain,               T_INT,    F_CTRL|F_CAP, 0,31),
    F ("high_pass",          0, high_pass,          T_BOOL,   F_CTRL, 0,0),
    F ("agc",                0, agc,                T_BOOL,   F_CTRL, 0,0),
    F ("ns",                 0, ns,                 T_INT,    F_CTRL, 0,3),
    F ("alc_gain",           0, alc_gain,           T_INT,    F_CTRL|CAP_ALC, 0,7),
    F ("agc_target_dbfs",    0, agc_target_dbfs,    T_INT,    F_CTRL, 0,31),
    F ("agc_compression_db", 0, agc_compression_db, T_INT,    F_CTRL, 0,90),
    F ("mute",               0, mute,               T_BOOL,   F_ATOMIC|F_CTRL|F_CAP, 0,0),
    F ("force_stereo",       0, force_stereo,       T_BOOL,   F_CTRL, 0,0),
    F ("spk_enabled",        0, spk_enabled,        T_BOOL,   F_CTRL, 0,0),
    F ("spk_volume",         0, spk_volume,         T_INT,    F_CTRL|CAP_SPK, 0,100),
    F ("spk_gain",           0, spk_gain,           T_INT,    F_CTRL|CAP_SPK, 0,100),
    F ("backchannel",        0, backchannel,        T_BOOL,   F_CTRL, 0,0),
    F ("backchannel_codec",  0, backchannel_codec,  T_BCCODEC,F_CTRL, 0,0),
    F ("backchannel_rate",   0, backchannel_rate,   T_INT,    F_CTRL, 8000,48000),
    F ("aec",                0, aec,                T_BOOL,   F_CTRL|CAP_SPK, 0,0),
};
#undef CAP_ALC
#undef CAP_SPK
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
    FS("debug_modules",      0, debug_modules,      F_CTRL),
    /* 0 would make IMP_*_PollingStream return immediately: video_thread spins,
     * the watchdog burns its MS_VIDEO_WATCHDOG_ITERS misses in under a
     * millisecond and escalates to raise(SIGTERM) - one config line and the
     * daemon exits seconds after the first client. Floor it well above that. */
    F("imp_polling_timeout", 0, imp_polling_timeout, T_INT, 0, 10,10000),
    /* 1024 is the T-series maximum (see the default above); anything larger is
     * rejected by libimp anyway, and 0 leaves the OSD pool unusable. */
    F("osd_pool_size",       0, osd_pool_size,       T_INT, 0, 1,1024),
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
    /* "listener"/"caller"; validated in srt.c (bad value -> listener) so this
     * table stays a plain string and config.c needs no new T_ type */
    FS("mode",       0,         mode,       0),
    FS("host",       0,         host,       0),
    FS("streamid",   0,         streamid,   0),
    FS("passphrase", 0,         passphrase, 0),
};
#undef TT

#define TT ms_osd_cfg
/* every osd.* global is F_CTRL (POST-able), matching the old hand-written
 * osd.enabled special-case + OSD_GLOBAL_KEYS[] in control.c combined - all
 * restart-required (imp_osd_setup() builds the OSD groups once at startup). */
static const cfg_field osd_fields[] = {
    F ("enabled",        0, enabled,        T_BOOL, F_CTRL, 0,0),
    F ("monitor_stream", 0, monitor_stream, T_INT,  F_CTRL, 0,0),
    FS("font_path",      0, font_path,      F_CTRL),
    FS("vars_file",      0, vars_file,      F_CTRL),
    F ("supersample",    0, supersample,    T_INT,  F_CTRL, 1,4),
    /* opt-in geometric autohint, default off: see the ms_osd_cfg.hinting
     * comment in config.h and msttf_set_hinting() for what this does and why
     * it's not a real TrueType hint-bytecode interpreter. Same File-only/
     * restart-only handling as supersample: read once by imp_osd_setup(). */
    F ("hinting",        0, hinting,        T_BOOL, F_CTRL, 0,0),
};
#undef TT

#define TT ms_motion_cfg
/* motion.cols/rows SET goes through explicit code in set_kv() (cross-axis
 * MOTION_CELL_LIMIT clamp); their table entries below only serve the GET
 * side. The deprecated motion.roi_* keys are entirely outside the table
 * (parse + one-shot warning in set_kv(), never readable). */
/* enabled/monitor_stream/sensitivity/cols/rows/hold_ms/skip_frames are F_CTRL
 * (POST-able), matching the old hand-written motion.enabled special-case +
 * MOTION_KEYS[] + MOTION_RESTART_KEYS[] in control.c combined.
 * cooldown_ms and on_motion deliberately have NO F_CTRL - this is the
 * security boundary the audit called out: on_motion is run via
 * fork()+execlp() (no shell) and cooldown_ms is the floor that bounds how
 * often it re-fires, so both stay config-file-only, never settable over
 * HTTP. (Do not add F_CTRL to either without re-reading the M3 comment
 * below and control.c's motion section comment.) */
static const cfg_field motion_fields[] = {
    F ("enabled",        0, enabled,        T_BOOL, F_CTRL, 0,0),
    F ("monitor_stream", 0, monitor_stream, T_CHAN, F_CTRL, 0,0),
    F ("sensitivity",    0, sensitivity,    T_INT,  F_CTRL, 0,255),
    F ("cols",           0, cols,           T_INT,  F_CTRL, 0,0),
    F ("rows",           0, rows,           T_INT,  F_CTRL, 0,0),
    /* M3: floor cooldown_ms so the on_motion hook cannot be re-exec'd on every
     * IVS result. IVS emits a result about every skip_frames/fps seconds - as
     * fast as ~200 ms at the defaults (skip_frames=5) - and the fork+exec'd hook
     * is deliberately not tracked, so with a 0/negative cooldown a hook slower
     * than that interval piles up unboundedly. 250 ms caps re-fires to at most
     * ~4/s (just above the fastest detection cadence) while still allowing
     * legitimate sub-second fast-alert use; 0 (disabled/no floor) is no longer
     * accepted. NOT F_CTRL - see the security-boundary comment above. */
    F ("cooldown_ms",    0, cooldown_ms,    T_INT,  0,      250,INT_MAX),
    F ("hold_ms",        0, hold_ms,        T_INT,  F_CTRL, 0,INT_MAX),
    F ("skip_frames",    0, skip_frames,    T_INT,  F_CTRL, 1,INT_MAX),
    /* NOT F_CTRL - see the security-boundary comment above. */
    FS("on_motion",      0, on_motion,      F_NOGET),
};
#undef TT

#define TT ms_record_cfg
/* clamp notes: segment_s 0 keeps "no rotation" (documented); negative/absurd
 * rolls are rejected so a garbage value can't silently disable rotation.
 * The whole section is readable (M13): without get coverage every
 * record-page POST re-wrote /etc/timps.conf (flash wear). */
/* every record.* key is F_CTRL (POST-able), matching the old REC_KEYS[]
 * array in control.c verbatim - the running recorder reads them live, no
 * restart needed. record.active/record.clip are separate transient actions
 * (not table fields at all), handled by hand-written code in control.c. */
static const cfg_field record_fields[] = {
    F ("enabled",     0,           enabled,     T_BOOL,    F_CTRL, 0,0),
    F ("channel",     0,           channel,     T_CHAN,    F_CTRL, 0,0),
    F ("mode",        0,           mode,        T_RECMODE, F_CTRL, 0,0),
    FS("dir",         0,           dir,         F_CTRL),
    FS("name",        0,           name,        F_CTRL),
    F ("segment_s",   "segment",   segment_s,   T_INT,     F_CTRL, 0,86400),
    F ("pre_roll_s",  "pre_roll",  pre_roll_s,  T_INT,     F_CTRL, 0,60),
    /* min 1, not 0: motion_recent() (record.c) gates motion-triggered
     * recording on `last_ms < post_roll_s*1000` - at 0 that's never true even
     * for the triggering event itself, so record.mode=1 would silently
     * record nothing, ever, with enabled:true and zero warning (Finding 1). */
    F ("post_roll_s", "post_roll", post_roll_s, T_INT,     F_CTRL, 1,300),
    F ("min_free_mb", 0,           min_free_mb, T_INT,     F_CTRL, 0,1048576),
    F ("audio",       0,           audio,       T_BOOL,    F_CTRL, 0,0),
};
#undef TT

#define TT ms_timelapse_cfg
/* every timelapse.* key is F_CTRL (POST-able), matching the old TL_KEYS[]
 * array in control.c verbatim - the running timelapse thread reads them
 * live, no restart needed. */
static const cfg_field timelapse_fields[] = {
    F ("enabled",    0,          enabled,    T_BOOL, F_CTRL, 0,0),
    F ("channel",    0,          channel,    T_CHAN, F_CTRL, 0,0),
    FS("dir",        0,          dir,        F_CTRL),
    FS("name",       0,          name,       F_CTRL),
    F ("interval_s", "interval", interval_s, T_INT,  F_CTRL, 1,INT_MAX),
    F ("keep_days",  0,          keep_days,  T_INT,  F_CTRL, 0,INT_MAX),
};
#undef TT

#define TT ms_daynight_cfg
/* F3: the numeric keys used to reach the detection thread unclamped via
 * /control (pint/pflt raw). Ranges: lat/lon geographic; sun offsets +-1 day;
 * both thresholds in the IMP [24.8] linear scale (256=1x, defaults 768/4096,
 * cold-start transients ~20000 - 1e6 = ~3900x is far beyond any sensor);
 * probe_min_gap_s is floored at 60 s - this is the ONLY bound on the audible
 * click rate, so it is a safety net rather than something to switch off;
 * heartbeat_s is floored at 300 s and heartbeat_max_s cannot be set below it
 * in practice (the automaton takes the smaller of the two when the scene is
 * moving); interval_ms floor 100 keeps the sampling loop from busy-spinning.
 * (probe_jump_pct and transition_s used to be clamped here too; both are now
 * the fixed DN_PROBE_JUMP_PCT/DN_TRANSITION_S constants in daynight.h and no
 * longer take a runtime value at all.) */
/* Every daynight.* key is F_CTRL (POST-able) EXCEPT:
 *  - mode: POST-able too, but NOT via the generic walker - control.c keeps
 *    hand-written validation for it (reject an unrecognized token outright
 *    with a warning, instead of config_apply_kv's own coerce-to-sensor-and-
 *    persist behaviour) rather than the generic per-field apply, so it is
 *    deliberately left without F_CTRL: the flag means "the generic walker
 *    may apply this", not "reachable via POST at all".
 *  - switch_cmd/isp_path: genuinely NOT settable via /control (exec'd
 *    command / scraped proc path) - the security boundary the audit called
 *    out, already marked F_NOGET, now also deliberately left without F_CTRL. */
static const cfg_field daynight_fields[] = {
    F ("enabled",                    0, enabled,                    T_BOOL,  F_CTRL, 0,0),
    /* NOT F_CTRL - see the comment above (hand-validated in control.c). */
    F ("mode",                       0, mode,                       T_DNMODE,0,      0,0),
    FS("time_night_start",           0, time_night_start,           F_CTRL),
    FS("time_day_start",             0, time_day_start,             F_CTRL),
    F ("sun_latitude",               0, sun_latitude,               T_FLT,   F_CTRL, -90,90),
    F ("sun_longitude",              0, sun_longitude,              T_FLT,   F_CTRL, -180,180),
    F ("sun_sunrise_offset_min",     0, sun_sunrise_offset_min,     T_INT,   F_CTRL, -1440,1440),
    F ("sun_sunset_offset_min",      0, sun_sunset_offset_min,      T_INT,   F_CTRL, -1440,1440),
    /* the two thresholds keep their pre-2026-08-17 names as aliases: the
     * units and the calibration are unchanged, only the metric got range at
     * the bright end, so an existing per-camera tuning stays valid. */
    F ("day_gain",   "total_gain_day_threshold",   day_gain,   T_FLT, F_CTRL, 1,1000000),
    F ("night_gain", "total_gain_night_threshold", night_gain, T_FLT, F_CTRL, 1,1000000),
    F ("day_confirm_s",              0, day_confirm_s,              T_INT,   F_CTRL, 1,3600),
    F ("probe_min_gap_s",            0, probe_min_gap_s,            T_INT,   F_CTRL, 60,86400),
    F ("probe_confirm_s",            0, probe_confirm_s,            T_INT,   F_CTRL, 1,3600),
    F ("heartbeat_s",                0, heartbeat_s,                T_INT,   F_CTRL, 300,604800),
    F ("heartbeat_max_s",            0, heartbeat_max_s,            T_INT,   F_CTRL, 300,604800),
    F ("boot_probe",                 0, boot_probe,                 T_INT,   F_CTRL, 0,1),
    F ("interval_ms",                0, interval_ms,                T_INT,   F_CTRL, 100,60000),
    F ("diagnose_thresholds",        0, diagnose_thresholds,        T_INT,   F_CTRL, 0,1),
    /* NOT F_CTRL - see the comment above (security boundary): a path the
     * daemon writes to as root must never be POSTable (arbitrary-file-write
     * primitive) - file-only. */
    FS("switch_cmd",                 0, switch_cmd,                 F_NOGET),
    FS("isp_path",                   0, isp_path,                   F_NOGET),
    FS("trace_path",                 0, trace_path,                 F_NOGET),
    /* exec'd as "<cmd> on|off" - same security boundary as switch_cmd */
    FS("irprobe_cmd",                0, irprobe_cmd,                F_NOGET),
};
#undef TT

#define TT ms_vstream_cfg
/* videoN.buffers additionally sets buffers_explicit=1 in set_kv() */
/* enabled..rtsp_path (16 keys) are F_CTRL (POST-able), matching the old
 * VID_REST[] array in control.c verbatim - all persist-only/restart-
 * required (the HAL does not reconfigure the running encoder).
 * imp_chn/jpeg/jpeg_quality/jpeg_fps/jpeg_chn deliberately have NO F_CTRL:
 * VID_REST never listed them either - they are internal encoder-channel
 * wiring (imp_chn: the IMP channel index; jpeg*: the piggyback JPEG
 * encoder's own attributes), not user-facing stream settings, and were
 * already F_NOGET. Preserve this exclusion; do not add F_CTRL here without a
 * deliberate decision to expose channel wiring over HTTP. */
static const cfg_field video_fields[] = {
    F ("enabled",      0,              enabled,      T_BOOL,  F_CTRL,  0,0),
    F ("codec",        0,              codec,        T_VCODEC,F_CTRL,  0,0),
    F ("width",        0,              width,        T_INT,   F_CTRL,  64,4096),
    F ("height",       0,              height,       T_INT,   F_CTRL,  64,4096),
    F ("fps",          0,              fps,          T_INT,   F_CTRL,  1,120),
    F ("bitrate",      0,              bitrate_kbps, T_INT,   F_CTRL,  16,50000),
    F ("rc_mode",      "mode",         rc_mode,      T_RC,    F_CTRL,  0,0),
    F ("gop",          0,              gop,          T_INT,   F_CTRL,  1,1000),
    F ("max_gop",      0,              max_gop,      T_INT,   F_CTRL,  1,1000),   /* F-04: RESERVED/no-effect - GOP comes from videoN.gop; kept for compat only */
    F ("profile",      0,              profile,      T_INT,   F_CTRL,  0,2),
    F ("qp",           0,              qp,           T_INT,   F_CTRL,  1,51),     /* only consumed when videoN.rc_mode=fixqp (enc_create's iInitialQP); no effect under CBR/VBR/etc, which rate-control instead */
    F ("min_qp",       0,              min_qp,       T_INT,   F_CTRL,  1,51),
    F ("max_qp",       0,              max_qp,       T_INT,   F_CTRL,  1,51),
    /* classic-SoC rc knobs; ranges are the imp_encoder.h domains. No effect on
     * the ENC_NEW_API SoCs, which warn once instead of failing silently. */
    F ("quality_lvl",  0,              quality_lvl,  T_INT,   F_CTRL,  0,7),
    F ("change_pos",   0,              change_pos,   T_INT,   F_CTRL,  50,100),
    F ("i_bias_lvl",   0,              i_bias_lvl,   T_INT,   F_CTRL,  -3,3),
    F ("fluc_lvl",     0,              fluc_lvl,     T_INT,   F_CTRL,  0,4),      /* H265 only; H264 rc structs have no flucLvl */
    F ("rotation",     0,              rotation,     T_ROT,   F_CTRL,  0,0),
    F ("buffers",      0,              buffers,      T_INT,   F_CTRL,  1,8),
    FS("rtsp_path",    0,              rtsp_path,    F_CTRL),
    /* libimp's own bound is chn<9 (GetStream_Impl); above MS_FS_MAXCHN fs_use()
     * returns silently, so an out-of-range channel gives no video and no clear
     * diagnostic. Config-file only (F_NOGET, no F_CTRL) - a footgun, not an
     * attack surface, but a one-line one to close. */
    F ("imp_chn",      0,              imp_chn,      T_INT,   F_NOGET, 0,8),
    F ("jpeg",         "jpeg_enabled", jpeg_enabled, T_BOOL,  F_NOGET, 0,0),
    F ("jpeg_quality", 0,              jpeg_quality, T_INT,   F_NOGET, 1,100),
    F ("jpeg_fps",     0,              jpeg_fps,     T_INT,   F_NOGET, 1,120),
    F ("jpeg_chn",     0,              jpeg_chn,     T_INT,   F_NOGET, 0,8),
};
#undef TT

#define TT ms_osd_item
/* enabled/type/text/x/y/font_size/color/transparency/outline/outline_color
 * are F_CTRL (POST-able), matching the old OSD[] array in control.c
 * verbatim. logo/logo_w/logo_h/font_path deliberately have NO F_CTRL - they
 * were never in OSD[] either (the per-item logo image and its per-item TTF
 * override are configured file-only, not exposed to the item editor); all
 * four were already F_NOGET. Preserve this exclusion. */
static const cfg_field osd_item_fields[] = {
    F ("enabled",       0,              enabled,       T_BOOL,   F_CTRL,  0,0),
    F ("type",          0,              type,          T_OSDTYPE,F_CTRL,  0,0),
    FS("text",          0,              text,          F_CTRL),
    FS("logo",          "logo_path",    logo_path,     F_NOGET),
    F ("logo_w",        "logo_width",   logo_w,        T_INT,    F_NOGET, 0,4096),
    F ("logo_h",        "logo_height",  logo_h,        T_INT,    F_NOGET, 0,4096),
    F ("x",             0,              x,             T_INT,    F_CTRL,  0,0),
    F ("y",             0,              y,             T_INT,    F_CTRL,  0,0),
    /* H4: font_size feeds the OSD canvas allocation (msttf_render); clamped
     * at parse so a bad /control write can never request an absurd raster
     * (the rasterizer additionally hard-clamps its own pixel height) */
    F ("font_size",     0,              font_size,     T_INT,    F_CTRL,  8,256),
    F ("color",         "font_color",   color,         T_HEX,    F_CTRL,  0,0),
    /* imp_osd.c feeds this straight into the group attr's uint8_t fgAlhpa:
     * clamp so e.g. 300 doesn't wrap to 44 while the config echoes 300 */
    F ("transparency",  0,              transparency,  T_INT,    F_CTRL,  0,255),
    F ("outline",       "stroke",       outline,       T_INT,    F_CTRL,  0,64),
    F ("outline_color", "stroke_color", outline_color, T_HEX,    F_CTRL,  0,0),
    FS("font_path",     0,              font_path,     F_NOGET),
};
#undef TT

/* every privacy region key is F_CTRL (POST-able), matching the old
 * PRIV_KEYS[] array in control.c verbatim - applied LIVE (creates/shows/
 * moves the IMP OSD cover region) and persisted. */
#define TT ms_privacy_region
static const cfg_field privacy_fields[] = {
    F("enabled", 0,            enabled, T_BOOL, F_CTRL, 0,0),
    F("x",       0,            x,       T_INT,  F_CTRL, 0,0),
    F("y",       0,            y,       T_INT,  F_CTRL, 0,0),
    F("w",       "width",      w,       T_INT,  F_CTRL, 0,0),
    F("h",       "height",     h,       T_INT,  F_CTRL, 0,0),
    F("color",   "fill_color", color,   T_HEX,  F_CTRL, 0,0),
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
    /* readable since debug_modules became POST-able: an unreadable field
     * defeats the change detection, and every POST would then rewrite
     * /etc/timps.conf with an fsync - the flash wear that detection exists to
     * prevent. loglevel and the polling/pool numbers read back harmlessly. */
    SEC("general.",   0, 0,                             general_fields),
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

/* Canonical spelling of a key, or a copy of the input when it is not a known
 * field. Used by config_write_keys(): the file may hold a pre-rename alias
 * (a fleet that has been upgraded in place usually does), and matching only
 * the exact string left that line untouched and APPENDED the canonical one.
 * The result was right - the later line wins on load - but the stale line
 * stayed forever, and a hand edit to it did nothing, which is a good way to
 * lose an afternoon. */
static void key_canonical(const char *key, char *out, size_t cap);

static const cfg_section *section_find(const char *key, const char **field)
{
    for (size_t i=0;i<sizeof g_sections/sizeof g_sections[0];i++)
        if (!strncmp(key, g_sections[i].prefix, g_sections[i].plen)){
            *field = key + g_sections[i].plen;
            return &g_sections[i];
        }
    return NULL;
}

static void key_canonical(const char *key, char *out, size_t cap)
{
    const char *fname = NULL;
    const cfg_section *sec = section_find(key, &fname);
    if (sec){
        const cfg_field *f = field_find(sec->fields, sec->nfields, fname);
        if (f){ snprintf(out, cap, "%s%s", sec->prefix, f->name); return; }
    }
    snprintf(out, cap, "%s", key);
}

static void field_set(void *base, const cfg_field *f, const char *val)
{
    void *p = (char*)base + f->off;
    if (f->flags & F_ATOMIC){
        /* _Atomic int field (T_BOOL/T_INT): store atomically so the lock-free
         * cross-thread reader's atomic load is properly synchronized. */
        int v = (f->type==T_BOOL) ? pbool(val)
              : (f->lo < f->hi)   ? pint_cl(val,f->lo,f->hi)
                                  : pint(val);
        atomic_store((_Atomic int*)p, v);
        return;
    }
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
        /* "sensor"/"time"/"sun" are the pre-2026-08-17 tokens, still accepted
         * so an existing config keeps working: the sensor mode IS the new
         * auto mode, and time/sun both mean "let the calendar decide" - which
         * of the two calendars applies is now derived from whether a time
         * window or a lat/lon is configured, so the distinction no longer
         * needs to be spelled out here. */
        if      (!strcmp(val,"auto"))     *(int*)p = DN_MODE_AUTO;
        else if (!strcmp(val,"schedule")) *(int*)p = DN_MODE_SCHEDULE;
        else if (!strcmp(val,"sensor"))   *(int*)p = DN_MODE_AUTO;
        else if (!strcmp(val,"time") || !strcmp(val,"sun"))
                                          *(int*)p = DN_MODE_SCHEDULE;
        else { LOGW(MOD,"daynight.mode: unknown '%s', keeping auto",val);
               *(int*)p = DN_MODE_AUTO; }
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
    if (f->flags & F_ATOMIC){        /* _Atomic int (T_BOOL/T_INT): atomic load */
        snprintf(out,cap,"%d",atomic_load((const _Atomic int*)p));
        return 1;
    }
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
                 m==DN_MODE_SCHEDULE?"schedule":"auto"); break; }
    }
    return 1;
}

/* Resolve a key to its field DESCRIPTOR only - no target, no write. Covers the
 * four table-driven branches of set_kv() below; the special cases it also has
 * (motion grid, retired daynight keys, general.trace/syslog) are every one of
 * them numeric or side-effecting, so a NULL here is the right answer for them.
 *
 * Yes, this repeats set_kv's dispatch prefix. Folding the two together would
 * mean restructuring 165 lines that interleave lookup with index arithmetic and
 * per-key special handling, and the payoff would be small: this function is
 * read-only, and if the two ever DIVERGE the failure is fail-safe. An unknown
 * key answers "not a string", which makes config_key_is_str() say no, which
 * makes control.c reject an empty value it might otherwise have accepted. The
 * cost of drift is an over-strict refusal, never a wrong write. */
static const cfg_field *field_for_key(const char *key)
{
    int osi, oii;
    const char *ok = osd_key(key, &osi, &oii);
    if (ok) return field_find(osd_item_fields, NF(osd_item_fields), ok);

    int psi, pii;
    const char *pk = privacy_key(key, &psi, &pii);
    if (pk) return field_find(privacy_fields, NF(privacy_fields), pk);

    if (!strncmp(key,"video0.",7) || !strncmp(key,"video1.",7))
        return field_find(video_fields, NF(video_fields), key+7);

    const char *k;
    const cfg_section *sec = section_find(key, &k);
    if (sec) return field_find(sec->fields, sec->nfields, k);
    return NULL;
}

/* public: is this key a string field? control.c needs it to tell "clear this
 * text" from "zero this number" - an empty value is meaningful for the first
 * and silently destructive for the second (pint("") is 0). */
int config_key_is_str(const char *key)
{
    const cfg_field *f = field_for_key(key);
    return f && f->type == T_STR;
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
        /* F-04: videoN.max_gop is parsed/clamped/persisted/echoed for compat
         * but NOTHING in any HAL consumes it - the encoder's keyframe
         * interval comes from videoN.gop (rcAttr.maxGop / gopAttr.uGopLength
         * = v->gop). videoN.qp IS consumed by enc_create() (iInitialQP /
         * attrH264FixQp.qp / attrH265FixQp.qp), but only when
         * videoN.rc_mode=fixqp; under CBR/VBR/etc it has no HAL consumer
         * since those modes derive QP from rate control instead. Warn once
         * on a non-zero videoN.max_gop so a user isn't silently losing a
         * setting they think is active, same as motion.roi_*. */
        if (!strcmp(key+7,"max_gop") && pint(val)!=0){
            static int vqp_warned;
            if (!vqp_warned){
                vqp_warned=1;
                LOGW(MOD,"videoN.max_gop is reserved and IGNORED - "
                         "the keyframe interval comes from videoN.gop");
            }
        }
        return;
    }

#ifndef USE_OSD_HINTING
    /* The key parses, clamps, persists and echoes on every build, but the
     * rasterizer's hinting pass is compiled out here (msttf_set_hinting() is
     * an empty stub). Without this the setting is accepted, reported back as
     * set, and changes nothing about the picture - and the only clue is that
     * the text looks the same as before. Once per session, like max_gop. */
    if (!strcmp(key,"osd.hinting") && pint(val)!=0){
        static int hint_warned;
        if (!hint_warned){
            hint_warned=1;
            LOGW(MOD,"osd.hinting is stored but has no effect in this build "
                     "(compiled without OSD text autohinting)");
        }
    }
#endif

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
    /* daynight keys retired by the 2026-08-17 redesign, plus `learn`/
     * `state_path` retired by the 2026-08-22 consolidation. They are parsed
     * and IGNORED with one warning each rather than dropped into the generic
     * "unknown key" line, because every one of them names a mechanism that no
     * longer exists (adaptive baseline, probe backoff, the brightness
     * fallback, the learning subsystem) and a bare "unknown key" would read
     * like a typo. The two gain thresholds are NOT here - they kept their old
     * names as aliases, so an existing tuning still applies. See
     * dev_notes/DAYNIGHT_REDESIGN_2026-08-17.md section 7.3 for the full
     * mapping. */
    if (!strncmp(key,"daynight.",9)){
        static const struct { const char *k, *why; } gone[] = {
          {"day_gain_pct",     "the adaptive night baseline is gone - night->day "
                               "is probe-mediated now, and the probe trigger is a "
                               "fixed internal constant"},
          {"baseline_delay_s", "the reference-anchor delay it renamed to is now a "
                               "fixed internal constant (DN_REF_DELAY_S)"},
          {"boot_settle_max_s","the settle wait is bounded internally now"},
          {"boot_stable_pct",  "the settle wait is gated on the reading having "
                               "stopped moving, with no tunable"},
          {"night_reconfirm_s","replaced by daynight.heartbeat_s / heartbeat_max_s"},
          {"probe_max_skip_s", "the probe skip it bounded no longer exists"},
          {"threshold_low",    "the brightness fallback is no longer a decision "
                               "path - use daynight.day_gain / night_gain"},
          {"threshold_high",   "the brightness fallback is no longer a decision "
                               "path - use daynight.day_gain / night_gain"},
          {"hysteresis",       "the brightness fallback is no longer a decision path"},
          {"learn",            "the learning subsystem is gone - diagnose_thresholds "
                               "already reports an unreachable day_gain without "
                               "touching it automatically"},
          {"state_path",       "was only used by daynight.learn, which is gone"},
        };
        for (size_t i=0;i<sizeof gone/sizeof gone[0];i++)
            if (!strcmp(key+9, gone[i].k)){
                LOGW(MOD,"%s is obsolete and IGNORED - %s", key, gone[i].why);
                return;
            }
        /* Eight more keys were turned from per-camera config into fixed
         * internal constants by the same consolidation (see the DN_* block in
         * daynight.h) - a different situation from the retirements above,
         * because the fixed value equals the field's old default: a config
         * that never touched one of these is unaffected, but a camera that
         * had explicitly tuned it away from the default would silently start
         * getting the fleet-wide value instead, with no warning at all if
         * this stayed silent like `gone[]` above. So each is parsed and
         * compared against the constant it became, and only warns when they
         * differ - matching the default is a no-op, not worth a warning. */
        static const struct { const char *k; int fixed; } gone_i[] = {
          {"probe_jump_pct",  DN_PROBE_JUMP_PCT},
          {"probe_settle_s",  DN_PROBE_SETTLE_S},
          {"ref_delay_s",     DN_REF_DELAY_S},
          {"ir_min_headroom", DN_IR_MIN_HEADROOM},
          {"boot_settle_s",   DN_BOOT_SETTLE_S},
          {"transition_s",    DN_TRANSITION_S},
        };
        for (size_t i=0;i<sizeof gone_i/sizeof gone_i[0];i++)
            if (!strcmp(key+9, gone_i[i].k)){
                int v = pint(val);
                if (v != gone_i[i].fixed)
                    LOGW(MOD,"daynight.%s=%s is now a fixed internal constant "
                             "(%d) and can no longer be tuned per camera - the "
                             "configured value is being ignored",
                         gone_i[i].k, val, gone_i[i].fixed);
                return;
            }
        static const struct { const char *k; float fixed; } gone_f[] = {
          {"ir_ratio_night", DN_IR_RATIO_NIGHT},
          {"ir_ratio_day",   DN_IR_RATIO_DAY},
        };
        for (size_t i=0;i<sizeof gone_f/sizeof gone_f[0];i++)
            if (!strcmp(key+9, gone_f[i].k)){
                float v = pflt(val);
                if (v != gone_f[i].fixed)
                    LOGW(MOD,"daynight.%s=%s is now a fixed internal constant "
                             "(%g) and can no longer be tuned per camera - the "
                             "configured value is being ignored",
                         gone_f[i].k, val, (double)gone_f[i].fixed);
                return;
            }
    }
    if (!strcmp(key,"general.syslog")){          /* to logread; default on */
        log_set_syslog(pbool(val));
        return;
    }
    /* Send-pipeline tracing (see src/trace.h). DELIBERATELY handled here as a
     * side-effecting key rather than as a cfg_field table entry, exactly like
     * general.syslog above: staying out of general_fields[] means it carries no
     * F_CTRL flag, so control.c's table walker never applies it from a POST,
     * config_get_kv() cannot echo it, and it is absent from the
     * /control?fields=1 inventory the WebUI builds its forms from. It is also
     * not a build-time option, so it never appears in menuconfig. This is a
     * developer probe for a specific incident, not a user-facing feature:
     * the only way to arm it is editing /etc/timps.conf by hand and restarting
     * timpsd. config_write_keys() preserves lines it does not own, so a
     * /control persist will not delete it while an investigation is running.
     *
     * Both keys take effect in file order; the threshold banner ms_trace_set()
     * prints therefore shows the default when general.trace_ms is listed after
     * general.trace. That is cosmetic - the value actually used per AU is read
     * live from g_trace_thresh_us. */
    if (!strcmp(key,"general.trace")){
        ms_trace_set(pint(val));
        return;
    }
    if (!strcmp(key,"general.trace_ms")){
        ms_trace_set_threshold_ms(pint(val));
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
    /* The logger keeps its own copy, so storing the value is not applying it.
     * Without this a POST would be accepted, echoed back correctly and do
     * nothing until the next restart - a switch that reports success and has
     * no effect is worse than one that is missing. */
    if (!strcmp(key, "general.debug_modules") || !strcmp(key, "debug_modules"))
        log_set_debug_modules(c->debug_modules);
    else if (!strcmp(key, "general.loglevel") || !strcmp(key, "loglevel"))
        log_set_level(c->loglevel);
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

/* Unquote a value that starts with ' or ", and cut an inline comment that
 * follows the CLOSING quote. strip_inline_comment() above bows out of quoted
 * values entirely (so a '#' INSIDE the quotes survives, which is the whole
 * point of quoting), which used to leave    key = "/mnt/sd" # a note    stored
 * verbatim as    "/mnt/sd" # a note    - quotes and comment both - because the
 * unquote step only looked at the first and last character of the raw value
 * and the last one was 'e', not '"'. Silently, and only visible after the next
 * reload.
 *
 * The closing quote is searched from the RIGHT, not the left: write_kv_line()
 * below deliberately does NOT escape a quote inside a value and documents that
 * the loader strips one leading and one trailing quote, so    "say "hi""
 * must keep reading back as    say "hi"   . Taking the first inner quote as
 * the closing one would silently truncate every such value on the next load -
 * trading this bug for a worse one. A quote qualifies as the closing quote
 * only if the rest of the value is blank or an inline "# comment"; the
 * rightmost one that qualifies wins.
 *
 * Returns the unquoted value, or val unchanged when there is no closing quote
 * at all (an unbalanced value, which is a config typo worth warning about -
 * the caller does that). */
static char *unquote_value(char *val, int *unterminated)
{
    *unterminated = 0;
    char q = *val;
    if (q!='"' && q!='\'') return val;
    for (char *p = val + strlen(val); --p > val; ){
        if (*p != q) continue;
        const char *t = p + 1;
        while (*t==' ' || *t=='\t') t++;
        if (*t && *t!='#') continue;              /* not the closing quote */
        *p = 0;                                   /* drop it + everything after */
        return val + 1;
    }
    *unterminated = 1;
    return val;
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
        int unterminated = 0;
        val = unquote_value(val, &unterminated);
        if (unterminated)
            LOGW(MOD,"config: %s has an opening quote but no closing one - "
                     "keeping the value verbatim, quotes included", key);
        set_kv(c, key, val);
        n++;
    }
    fclose(f);
    log_set_level(c->loglevel);
    log_set_debug_modules(c->debug_modules);
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
/* Resolve where under /proc/jz/sensor/ the kernel sensor registry's <key>
 * files actually live. Two layouts have been observed:
 *   - flat:   /proc/jz/sensor/<key>            (T31/sc4336p on a newer
 *             kernel/SDK - confirmed live: cat /proc/jz/sensor/name ->
 *             "sc4336p". A same-named /proc/jz/sensor/sc4336p/ subdirectory
 *             also exists there with an identical copy of every <key> file,
 *             but it is NOT the one raptor/prudynt-style tooling reads.)
 *   - nested: /proc/jz/sensor/sensor0/<key>    (a fixed "sensor0" alias,
 *             the layout every board seen before this one used)
 * Probe "name" in each location once (the registry is populated at driver
 * probe time and does not change for the life of the process), cache
 * whichever prefix answers first, and reuse it. *available is 0 when
 * neither exists (host sim, T40/T41) so callers' path reads just fail as
 * before and the existing config/fallback path takes over; the returned
 * prefix is then meaningless and must not be used. */
static const char *sensor_proc_prefix(int *available)
{
    static char prefix[16] = "";
    static int  avail = -1;   /* -1 = not yet probed */
    if (avail == -1) {
        struct stat st;
        if (stat("/proc/jz/sensor/sensor0/name", &st) == 0) {
            copystr(prefix, "sensor0/", sizeof prefix);
            avail = 1;
        } else if (stat("/proc/jz/sensor/name", &st) == 0) {
            prefix[0] = 0;   /* flat layout: no subdirectory component */
            avail = 1;
        } else {
            avail = 0;
        }
    }
    *available = avail;
    return prefix;
}

/* read /proc/jz/sensor/<prefix><key> as a number (base 0 = 0x.. hex or
 * decimal); <0 on missing/unparseable */
static long read_sensor_proc(const char *key, int base)
{
    int avail;
    const char *prefix = sensor_proc_prefix(&avail);
    if (!avail) return -1;
    char path[80], buf[64];
    snprintf(path, sizeof path, "/proc/jz/sensor/%s%s", prefix, key);
    if (read_proc_line(path, buf, sizeof buf) != 0) return -1;
    return strtol(buf, NULL, base);
}

/* raptor/prudynt-style sensor auto-detect: fill any sensor.* field left unset
 * (empty/0 - i.e. not given in the config) from the Ingenic kernel sensor
 * registry under /proc/jz/sensor/ (see sensor_proc_prefix() above for the
 * flat-vs-"sensor0" layout it resolves), which the board's sensor .ko
 * populates with name/i2c_addr/width/height/max_fps after probing the chip.
 * Config values always win; whatever is still unset gets a safe fallback.
 * On the host sim and on T40/T41 (no /proc/jz/sensor) the reads fail, so
 * only the fallback applies. Call once after config_load(), before the HAL
 * is started. */
void config_sensor_finalize(ms_config *c)
{
    /* The loaded kernel sensor driver (/proc/jz/sensor/) is authoritative
     * for the sensor NAME and I2C address: IMP_ISP_AddSensor must be told the
     * sensor that is actually loaded. A mismatching name makes the ISP/sensor
     * kernel module work from a zero attr table (pclk/line_time == 0) and divide
     * by zero -> SIGFPE in the kernel. So the registry overrides a stale config
     * value here (resolution/fps stay config-first below - the registry often
     * reports 0 for them). */
    { char name[MS_MAX_STR];
      int avail; const char *prefix = sensor_proc_prefix(&avail);
      char name_path[80];
      snprintf(name_path, sizeof name_path, "/proc/jz/sensor/%sname", prefix);
      if (avail && read_proc_line(name_path, name, sizeof name) == 0 && name[0]){
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

/* Write one "key = value" line, quoting whenever the loader would otherwise
 * eat part of the value.
 *
 * config_load() + strip_inline_comment() above do three things to a value, and
 * each one silently DESTROYS characters unless the writer quotes:
 *
 *   1. an unquoted '#' at the start of the value or after a space/tab opens an
 *      inline comment and the rest of the line is dropped. An OSD text of
 *      "Kamera #2" written bare comes back as "Kamera" - the value is live and
 *      correct until then, so the loss only ever surfaces after a reboot, on
 *      every camera, at once. strip_inline_comment() deliberately exempts
 *      quoted values; that exemption is the escape hatch used here.
 *   2. trim() eats leading and trailing whitespace, so "  pad  " comes back as
 *      "pad", and an empty value leaves nothing after the '=' at all.
 *   3. when the value starts with a quote character (' or "), the loader
 *      strips it together with the matching closing quote (the rightmost one
 *      followed by nothing but blanks or an inline comment). A value that
 *      legitimately IS 'Kamera' or "Kamera" therefore loses its own quotes on
 *      reload - and because the shortened form is stable, it does so
 *      invisibly from the second load on.
 *
 * So the value is quoted unless it is a bare token that no loader rule can
 * touch. The characters that force quoting, and the rule each one answers:
 *      ' ' (and any folded whitespace)  rules 1 + 2
 *      '#'                              rule 1
 *      '"'  '\''                        rule 3
 *      ';'                              a value starting with ';' would read
 *                                       as a comment line if it ever ended up
 *                                       at line start (e.g. a lost key name)
 * plus the empty value, for rule 2. When in doubt we quote: a redundant pair
 * of quotes is invisible after the next load, a missing pair is data loss.
 *
 * The '"' is our quoting character and is NOT escaped - it does not need to
 * be. The loader strips exactly ONE leading and ONE trailing character, and
 * only when the two match, so    key = "say "hi""    reads back as    say "hi"
 * The writer therefore no longer has to substitute '"' with '\'' (it used to,
 * which was itself a small, permanent data loss on every save).
 *
 * Control chars stay folded to a space unconditionally: a raw '\n' inside a
 * value would inject an entire extra config line on the next load, and no
 * quoting saves us from that - the flat one-line-per-key format has no
 * multi-line form. Folding them first also means the only whitespace that can
 * still be in v[] is a plain ' ', which is why the scan below tests for that
 * byte instead of isspace() (locale-proof, and it cannot misfire on a UTF-8
 * continuation byte). */
static void write_kv_line(FILE *f, const char *k, const char *vin)
{
    char v[256]; size_t o=0;
    for (const char *p=vin; *p && o+1<sizeof v; p++){
        unsigned char ch=(unsigned char)*p;
        v[o++] = (ch<0x20) ? ' ' : (char)ch;
    }
    v[o]=0;

    int quote = (o==0);
    for (size_t i=0; !quote && i<o; i++){
        char ch = v[i];
        if (ch==' ' || ch=='#' || ch=='"' || ch=='\'' || ch==';') quote = 1;
    }
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
                        if (strcmp(k, keys[i])){
                            /* not an exact hit - try the canonical form, so a
                             * pre-rename alias line is REPLACED rather than
                             * left behind with the new key appended below. */
                            char ck[80], ci[80];
                            key_canonical(k, ck, sizeof ck);
                            key_canonical(keys[i], ci, sizeof ci);
                            if (strcmp(ck, ci)) continue;
                        }
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
        /* fgets() returning NULL means EOF *or* a read error, and the two are
         * indistinguishable without ferror(). A bad flash block partway
         * through the old config would end the copy loop early, the tmp file
         * would look perfectly well-formed, and the rename() below would
         * commit it - silently deleting every setting after the unreadable
         * block, on the next /control POST, with the daemon still running the
         * old values so nobody notices until the next restart. There is no
         * safe way to "finish" the copy here, so abort the whole rewrite: the
         * original file is still intact, and the caller's change is simply
         * not persisted (it stays live in g_cfg). */
        if (ferror(in)){
            LOGE(MOD,"read error on %s (%s) - ABORTING the config rewrite so "
                     "the truncated copy is not committed over it. The setting "
                     "is live but NOT persisted; this flash needs attention",
                 path, strerror(errno));
            fclose(in);
            fclose(out);
            remove(tmp);
            goto unlock;
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

/* ------------------------------------------------------------------------
 * cfg_fields_*(): hand control.c's generic /control POST walker a read-only
 * view of the section tables above (+ field count) instead of it hand-
 * listing field names a second time. See the F_CTRL doc comment in config.h -
 * only entries with F_CTRL set in the returned table are POST-eligible; the
 * caller must never walk a table's fields unconditionally. */
const cfg_field *cfg_fields_image(int *n)     { *n = NF(image_fields);     return image_fields; }
const cfg_field *cfg_fields_audio(int *n)     { *n = NF(audio_fields);     return audio_fields; }
const cfg_field *cfg_fields_sensor(int *n)    { *n = NF(sensor_fields);    return sensor_fields; }
const cfg_field *cfg_fields_osd(int *n)       { *n = NF(osd_fields);       return osd_fields; }
const cfg_field *cfg_fields_osd_item(int *n)  { *n = NF(osd_item_fields);  return osd_item_fields; }
const cfg_field *cfg_fields_motion(int *n)    { *n = NF(motion_fields);    return motion_fields; }
const cfg_field *cfg_fields_record(int *n)    { *n = NF(record_fields);    return record_fields; }
const cfg_field *cfg_fields_timelapse(int *n) { *n = NF(timelapse_fields); return timelapse_fields; }
const cfg_field *cfg_fields_daynight(int *n)  { *n = NF(daynight_fields);  return daynight_fields; }
const cfg_field *cfg_fields_general(int *n)   { *n = NF(general_fields);   return general_fields; }
const cfg_field *cfg_fields_video(int *n)     { *n = NF(video_fields);     return video_fields; }
const cfg_field *cfg_fields_privacy(int *n)   { *n = NF(privacy_fields);   return privacy_fields; }
