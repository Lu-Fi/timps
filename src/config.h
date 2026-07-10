#ifndef TIMPS_CONFIG_H
#define TIMPS_CONFIG_H

#include <stdint.h>

/* Supported video codecs */
typedef enum {
    CODEC_H264 = 0,
    CODEC_H265 = 1,
} VideoCodec;

/* Supported audio codecs */
typedef enum {
    AUDIO_NONE = 0,
    AUDIO_G711A = 1,  /* G.711 A-law  (PCMA) */
    AUDIO_G711U = 2,  /* G.711 µ-law  (PCMU) */
    AUDIO_AAC   = 3,
} AudioCodec;

/* Per-stream configuration (two streams: main + sub) */
typedef struct {
    int         enabled;
    int         width;
    int         height;
    int         fps;
    int         gop;        /* I-frame interval in frames */
    int         bitrate;    /* kbps */
    VideoCodec  codec;
} StreamCfg;

typedef struct {
    char        sensor[64];     /* sensor driver name */
    char        sensor_bin[128];/* ISP binary path */

    /* Video streams */
    StreamCfg   stream[2];      /* 0=main, 1=sub */

    /* Audio */
    AudioCodec  audio_codec;
    int         audio_sample_rate; /* Hz: 8000 / 16000 */
    int         audio_volume;      /* 0-100 */

    /* RTSP */
    int         rtsp_port;
    char        rtsp_user[64];
    char        rtsp_pass[64];
    int         rtsp_auth;      /* 0=none, 1=digest */

    /* HTTP */
    int         http_port;
    char        http_user[64];
    char        http_pass[64];
    int         http_auth;      /* 0=none, 1=basic */

    /* OSD */
    int         osd_enabled;
    char        osd_font[256];  /* path to .ttf font */
    int         osd_font_size;
    char        osd_label[256]; /* label template */
    int         osd_x;          /* position */
    int         osd_y;

    /* Motion detection */
    int         motion_enabled;
    int         motion_sensitivity; /* 0-9 */
    char        motion_script[256]; /* script to run on motion */

    /* Misc */
    int         log_level;      /* 0=error..3=debug */
    int         daemonize;
} Config;

/* Global configuration instance */
extern Config g_cfg;

/* Load configuration from file. Returns 0 on success. */
int config_load(const char *path);

/* Apply built-in defaults */
void config_defaults(void);

/* Print active configuration */
void config_dump(void);

#endif /* TIMPS_CONFIG_H */
