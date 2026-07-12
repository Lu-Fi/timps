/* config.h - minimal key=value config (replaces libconfig) */
#ifndef MS_CONFIG_H
#define MS_CONFIG_H

#include <stdint.h>
#include <stddef.h>   /* size_t */

#define MS_MAX_VSTREAM 2
#define MS_MAX_OSD     8
#define MS_MAX_PRIVACY 4
#define MS_MAX_STR     64

enum ms_vcodec { MS_VC_H264=0, MS_VC_H265=1 };
enum ms_acodec { MS_AC_NONE=0, MS_AC_AAC=1, MS_AC_PCMU=2, MS_AC_PCMA=3 };
enum ms_rcmode { MS_RC_CBR=0, MS_RC_VBR, MS_RC_FIXQP, MS_RC_SMART,
                 MS_RC_CAPPED_VBR, MS_RC_CAPPED_QUALITY };
enum ms_osd_type { MS_OSD_TEXT=0, MS_OSD_LOGO=1 };

typedef struct {
    int      enabled;
    int      codec;          /* enum ms_vcodec */
    int      width, height;
    int      fps;
    int      bitrate_kbps;
    int      rc_mode;        /* enum ms_rcmode */
    int      gop;
    int      max_gop;
    int      profile;        /* 0 baseline,1 main,2 high */
    int      qp;             /* fixed qp / init qp */
    int      min_qp, max_qp;
    int      rotation;       /* 0,90,270 (0/1/2) */
    int      buffers;        /* IMP nrVBs */
    char     rtsp_path[MS_MAX_STR];
    int      imp_chn;        /* encoder channel */
    /* optional extra JPEG encoder piggybacked on this stream: it is
     * registered into the SAME encoder group, so it shares the stream's
     * FrameSource (no additional rmem for video buffers) and produces
     * JPEGs at this stream's resolution. Default: off. */
    int      jpeg_enabled;
    int      jpeg_quality;   /* 1..100 */
    int      jpeg_fps;       /* max snapshot/MJPEG publish rate */
    int      jpeg_chn;       /* IMP encoder channel (must be unique) */
} ms_vstream_cfg;

typedef struct {
    int      enabled;
    int      codec;          /* enum ms_acodec */
    int      samplerate;
    int      channels;
    int      bitrate_kbps;   /* for aac */
    int      volume;         /* IMP_AI_SetVol level */
    int      gain;           /* IMP_AI_SetGain level */
    int      high_pass;      /* HPF on/off */
    int      agc;            /* automatic gain control on/off */
    int      ns;             /* noise suppression: 0 off, 1..3 = level */
    int      alc_gain;       /* IMP_AI_SetAlcGain PGA level 0..7 (T21/T31/C100) */
    int      agc_target_dbfs;    /* AGC TargetLevelDbfs 0..31 */
    int      agc_compression_db; /* AGC CompressionGaindB 0..90 */
    int      mute;           /* live mic mute: 1 = captured frames are dropped
                              * before the encoder/hub (no audio to any client);
                              * toggled at runtime via /control, no restart */
    /* persist-only (no runtime path in timps yet): kept so the WebUI audio
     * page can store them; capture is mono and there is no AO pipeline */
    int      force_stereo;
    int      spk_enabled;
    int      spk_volume;
    int      spk_gain;
} ms_audio_cfg;

typedef struct {
    char     model[MS_MAX_STR];
    int      i2c_addr;
    int      fps;
    int      width, height;
} ms_sensor_cfg;

/* JPEG / MJPEG snapshot stream (the old streamer's "stream2") */
typedef struct {
    int      enabled;
    int      width, height;
    int      quality;               /* 1..100 */
    int      fps;                   /* max MJPEG frame rate */
    int      imp_chn;               /* JPEG encoder channel */
    char     snapshot_path[128];    /* periodic file snapshot ("" = none) */
} ms_jpeg_cfg;

/* ISP image tuning (mirrors the old streamer's image.* block) */
typedef struct {
    int      brightness, contrast, saturation, sharpness, hue;
    int      vflip, hflip;
    int      running_mode;          /* 0 day, 1 night */
    int      anti_flicker;          /* 0 disable,1 50Hz,2 60Hz */
    int      ae_compensation;
    int      max_again, max_dgain;
    int      sinter_strength, temper_strength, dpc_strength;
    int      defog_strength, drc_strength;
    int      highlight_depress, backlight_compensation;
    int      core_wb_mode, wb_rgain, wb_bgain;
} ms_image_cfg;

/* one OSD overlay: text (with placeholders) or a BGRA logo */
typedef struct {
    int      enabled;
    int      type;                  /* enum ms_osd_type */
    char     text[128];             /* template: {ph} + strftime %.. */
    char     logo_path[128];        /* raw BGRA logo file */
    int      logo_w, logo_h;
    int      x, y;                  /* position; negative = from right/bottom edge */
    int      font_size;
    uint32_t color;                 /* 0xAARRGGBB */
    int      transparency;          /* 0..255 group alpha */
    int      outline;               /* text outline width in px (0 = off) */
    uint32_t outline_color;         /* 0xAARRGGBB outline/stroke color */
    char     font_path[128];        /* optional per-item TTF override */
} ms_osd_item;

/* OSD config. Each video stream has its OWN independent item set
 * (items[stream][item]); the master switch, monitor stream and the font/vars
 * paths stay global.
 * Config keys:
 *   canonical (per stream): osd<S>.<N>.<field>   e.g. osd0.0.text (stream 0,
 *     item 0), osd1.3.enabled (stream 1, item 3)
 *   legacy (pre-per-stream): osd<N>.<field>      e.g. osd0.text - still
 *     parsed for backward compatibility and applied to item N on EVERY
 *     stream (old configs keep drawing the same overlays on both streams) */
typedef struct {
    int         enabled;            /* master switch (global, restart) */
    int         monitor_stream;     /* stream whose fps feeds the {fps} var */
    char        font_path[128];     /* default TTF for text items */
    char        vars_file[128];     /* custom placeholder source (e.g. /tmp/..) */
    ms_osd_item items[MS_MAX_VSTREAM][MS_MAX_OSD];  /* per-stream item sets */
} ms_osd_cfg;

/* privacy mask: a solid filled rectangle drawn over the video (IMP OSD cover
 * region) to black out a sensitive area. Each video stream has its own set of
 * up to MS_MAX_PRIVACY regions. Applied LIVE via /control (created/shown/moved
 * at runtime, like OSD items) - no restart required.
 * Config keys (per stream): privacy<S>.<N>.<field>, e.g. privacy0.0.enabled,
 * privacy1.2.w. Fields: enabled, x, y, w, h, color (0xAARRGGBB fill). */
typedef struct {
    int      enabled;
    int      x, y, w, h;            /* rect in the stream's frame, pixels */
    uint32_t color;                 /* 0xAARRGGBB fill color */
} ms_privacy_region;

/* native automatic day/night detection (thread compiled with -DUSE_DAYNIGHT;
 * keys are always parsed so a config with daynight.* loads warning-free).
 * Semantics/defaults match thingino's daynightd. */
typedef struct {
    int      enabled;            /* 0 = manual mode (thread idles) */
    float    threshold_low;      /* %: below this (in day) -> night */
    float    threshold_high;     /* %: above this (in night) -> day */
    float    hysteresis;         /* factor for the unknown-state band */
    int      interval_ms;        /* sample interval */
    int      transition_s;       /* min dwell between switches */
    char     switch_cmd[64];     /* board script, run as "<cmd> day|night" */
    char     isp_path[128];      /* ISP exposure proc file */
} ms_daynight_cfg;

typedef struct {
    int      enabled;            /* IMP_IVS motion detection */
    int      monitor_stream;
    int      sensitivity;        /* 0..255 (mapped to IMP's 0..4 in the HAL) */
    int      cols, rows;         /* detection GRID over the monitor stream's
                                  * frame; cols*rows is clamped to the SDK's
                                  * IMP_IVS_MOVE_MAX_ROI_CNT (motion_caps.h:
                                  * 52 on most SDKs, 4 on T10/T20 3.9.0) */
    int      roi_x, roi_y, roi_w, roi_h;  /* legacy single-ROI keys: still
                                  * parsed (old configs load warning-free)
                                  * but unused since the grid replaced them */
    int      cooldown_ms;        /* min gap between motion events */
    int      hold_ms;            /* keep a cell "active" this long after its last
                                  * retRoi hit so async /events + /control readers
                                  * reliably observe single-frame motion instead
                                  * of racing the clear back to 0 (0 = no hold) */
    int      skip_frames;        /* IMP_IVS_MoveParam.skipFrameCnt: analyse every
                                  * Nth frame. Higher = cheaper but more latency;
                                  * lower = snappier but more CPU (>=1, default 5) */
    char     on_motion[128];     /* command/script to run on motion ("" = none).
                                  * Config-file only, NOT settable via /control
                                  * (it is executed through system()). */
} ms_motion_cfg;

/* local recording to SD (fragmented MP4 segments, like raptor's RMR). Reuses
 * the fMP4 muxer; motion-triggered or continuous. */
typedef struct {
    int      enabled;            /* master enable (also gates on-boot start) */
    int      channel;            /* video stream to record (0..MS_MAX_VSTREAM-1) */
    int      mode;               /* 0 = continuous, 1 = motion-triggered */
    char     dir[128];           /* SD base dir, e.g. /mnt/mmcblk0p1 */
    char     name[96];           /* strftime path template (under <dir>/<host>/records/) */
    int      segment_s;          /* max segment length (seconds), 0 = single file */
    int      pre_roll_s;         /* motion: seconds of buffered video kept before the trigger */
    int      post_roll_s;        /* motion: keep recording this long after the last motion */
    int      min_free_mb;        /* delete oldest segments until at least this much is free */
    int      audio;              /* 1 = mux audio into the recording when available */
} ms_record_cfg;

/* optional SRT output (USE_SRT builds only, libsrt): serves one video stream
 * (+audio) as MPEG-TS over SRT in listener mode. */
typedef struct {
    int      enabled;
    int      port;                  /* SRT listener port (default 9000) */
    int      channel;               /* video stream to serve */
    int      latency_ms;            /* SRT receive/peer latency */
    char     streamid[64];          /* optional required STREAMID */
    char     passphrase[64];        /* optional AES passphrase ("" = none) */
} ms_srt_cfg;

typedef struct {
    /* general */
    int            loglevel;
    int            imp_polling_timeout;
    int            osd_pool_size;

    ms_sensor_cfg  sensor;
    ms_image_cfg   image;

    /* rtsp */
    int            rtsp_enabled;
    int            rtsp_port;
    char           rtsp_user[MS_MAX_STR];
    char           rtsp_pass[MS_MAX_STR];

    /* http fmp4 preview */
    int            http_enabled;
    int            http_port;
    int            http_preview_chn;   /* which video stream to expose */
    char           http_user[MS_MAX_STR];  /* empty = fall back to rtsp creds */
    char           http_pass[MS_MAX_STR];
    /* /control token auth (startup/security settings, NOT settable via
     * /control): http_token = optional persistent remote secret (also
     * accepted as a valid token, for automation); http_token_file = where
     * the random per-boot token is published for local privileged readers
     * ("" = don't write). The configured secret is NEVER written there. */
    char           http_token[MS_MAX_STR];
    char           http_token_file[128];
    /* GET /events SSE push stream (USE_CONTROL builds only; startup
     * settings, deliberately NOT settable via /control) */
    int            events_enabled;      /* 0 = endpoint answers 404 */
    int            events_stats_ms;     /* "stats" event period, 0 = none */
    int            events_max_clients;  /* concurrent /events conns -> 503 */
    /* optional TLS (USE_TLS builds only): HTTPS for the http port + RTSPS for a
     * second RTSP port. Plain HTTP/RTSP still run as before. */
    int            http_https;          /* 1 = serve the http port over TLS */
    char           http_tls_cert[128];  /* PEM cert file */
    char           http_tls_key[128];   /* PEM private key file */
    int            rtsp_tls;            /* 1 = also run an RTSPS (TLS) listener */
    int            rtsp_tls_port;       /* RTSPS port (default 322) */

    ms_vstream_cfg video[MS_MAX_VSTREAM];
    ms_audio_cfg   audio;
    ms_jpeg_cfg    jpeg;
    ms_osd_cfg     osd;
    ms_privacy_region privacy[MS_MAX_VSTREAM][MS_MAX_PRIVACY]; /* cover masks */
    ms_motion_cfg  motion;
    ms_record_cfg  record;
    ms_srt_cfg     srt;
    ms_daynight_cfg daynight;

    /* sim backend (x86 testing) */
    char           sim_video0[256];
    char           sim_video1[256];
    char           sim_audio[256];
    char           sim_jpeg[256];
} ms_config;

extern ms_config g_cfg;
extern const char *g_cfg_path;   /* config file in use (set by config_load) */

void config_defaults(ms_config *c);
int  config_load(ms_config *c, const char *path); /* 0 ok, <0 file err (defaults kept) */
/* apply a single key=value (same keys as the config file) to c */
void config_apply_kv(ms_config *c, const char *key, const char *val);
/* read the current value of a key back as a normalized string (the same form
 * config_apply_kv would store). Covers the keys the /control endpoint touches
 * (image.*, audio.*, videoN.*, sensor.*, osdS.N.*, legacy osdN.*, osd.*).
 * Returns 1 if the key is known (out filled), 0 otherwise. Used for change-
 * detection. (audio/video/sensor coverage includes the persist-only restart
 * keys. A legacy osdN.* key reads back only while all streams agree on the
 * value - otherwise it reports unknown so a legacy write always applies.) */
int  config_get_kv(const ms_config *c, const char *key, char *out, size_t cap);
/* replace/append "key = value" lines in the config file (atomic, keeps
 * comments/order). Returns 0 on success. */
int  config_write_keys(const char *path, const char *const *keys,
                       const char *const *vals, int n);
/* Serializes /control's runtime writes of g_cfg STRINGS (copystr inside
 * config_apply_kv) against concurrent readers (OSD updater, recorder, RTSP
 * path match, GET /control). Leaf lock: hold it only for a short copy/compare,
 * never across HAL/status/blocking calls. Ints are not covered (aligned word
 * reads, no tearing to worry about). */
void config_str_lock(void);
void config_str_unlock(void);

#endif
