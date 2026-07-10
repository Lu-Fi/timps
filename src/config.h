/* config.h - minimal key=value config (replaces libconfig) */
#ifndef MS_CONFIG_H
#define MS_CONFIG_H

#include <stdint.h>

#define MS_MAX_VSTREAM 2
#define MS_MAX_OSD     8
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
    int      ns;             /* noise suppression 0..3 */
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
    char     font_path[128];        /* optional per-item TTF override */
} ms_osd_item;

typedef struct {
    int         enabled;            /* master switch */
    int         monitor_stream;     /* video stream the OSD is drawn on */
    char        font_path[128];     /* default TTF for text items */
    char        vars_file[128];     /* custom placeholder source (e.g. /tmp/..) */
    ms_osd_item items[MS_MAX_OSD];
} ms_osd_cfg;

typedef struct {
    int      enabled;            /* IMP_IVS motion detection */
    int      monitor_stream;
    int      sensitivity;        /* 0..255 */
    int      roi_x, roi_y, roi_w, roi_h;  /* 0 => full frame */
    int      cooldown_ms;        /* min gap between motion events */
    char     on_motion[128];     /* command/script to run on motion ("" = none) */
} ms_motion_cfg;

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

    ms_vstream_cfg video[MS_MAX_VSTREAM];
    ms_audio_cfg   audio;
    ms_jpeg_cfg    jpeg;
    ms_osd_cfg     osd;
    ms_motion_cfg  motion;

    /* sim backend (x86 testing) */
    char           sim_video0[256];
    char           sim_video1[256];
    char           sim_audio[256];
    char           sim_jpeg[256];
} ms_config;

extern ms_config g_cfg;

void config_defaults(ms_config *c);
int  config_load(ms_config *c, const char *path); /* 0 ok, <0 file err (defaults kept) */

#endif
