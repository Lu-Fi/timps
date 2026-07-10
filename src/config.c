#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "config.h"
#include "log.h"

Config g_cfg;

void config_defaults(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));

    strncpy(g_cfg.sensor, "gc2053", sizeof(g_cfg.sensor) - 1);
    strncpy(g_cfg.sensor_bin, "/etc/sensor/gc2053.bin",
            sizeof(g_cfg.sensor_bin) - 1);

    /* Main stream: 1080p30 H.264 2Mbps */
    g_cfg.stream[0].enabled = 1;
    g_cfg.stream[0].width   = 1920;
    g_cfg.stream[0].height  = 1080;
    g_cfg.stream[0].fps     = 25;
    g_cfg.stream[0].gop     = 50;
    g_cfg.stream[0].bitrate = 2048;
    g_cfg.stream[0].codec   = CODEC_H264;

    /* Sub stream: 640×360 H.264 512kbps */
    g_cfg.stream[1].enabled = 1;
    g_cfg.stream[1].width   = 640;
    g_cfg.stream[1].height  = 360;
    g_cfg.stream[1].fps     = 15;
    g_cfg.stream[1].gop     = 30;
    g_cfg.stream[1].bitrate = 512;
    g_cfg.stream[1].codec   = CODEC_H264;

    g_cfg.audio_codec       = AUDIO_G711A;
    g_cfg.audio_sample_rate = 8000;
    g_cfg.audio_volume      = 70;

    g_cfg.rtsp_port  = 554;
    g_cfg.rtsp_auth  = 1;
    strncpy(g_cfg.rtsp_user, "admin",  sizeof(g_cfg.rtsp_user) - 1);
    strncpy(g_cfg.rtsp_pass, "admin",  sizeof(g_cfg.rtsp_pass) - 1);

    g_cfg.http_port  = 8080;
    g_cfg.http_auth  = 1;
    strncpy(g_cfg.http_user, "admin",  sizeof(g_cfg.http_user) - 1);
    strncpy(g_cfg.http_pass, "admin",  sizeof(g_cfg.http_pass) - 1);

    g_cfg.osd_enabled   = 1;
    g_cfg.osd_font_size = 24;
    strncpy(g_cfg.osd_font,  "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            sizeof(g_cfg.osd_font) - 1);
    strncpy(g_cfg.osd_label, "%Y-%m-%d %H:%M:%S",
            sizeof(g_cfg.osd_label) - 1);
    g_cfg.osd_x = 10;
    g_cfg.osd_y = 10;

    g_cfg.motion_enabled     = 0;
    g_cfg.motion_sensitivity = 5;

    g_cfg.log_level  = LOG_LEVEL_INFO;
    g_cfg.daemonize  = 0;
}

/* Trim leading/trailing whitespace in-place, return pointer */
static char *trim(char *s)
{
    char *e;
    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) e--;
    e[1] = '\0';
    return s;
}

static void apply_kv(const char *key, const char *val)
{
    /* sensor */
    if (!strcmp(key, "sensor"))
        strncpy(g_cfg.sensor, val, sizeof(g_cfg.sensor) - 1);
    else if (!strcmp(key, "sensor_bin"))
        strncpy(g_cfg.sensor_bin, val, sizeof(g_cfg.sensor_bin) - 1);

    /* stream0 */
    else if (!strcmp(key, "stream0.enabled"))
        g_cfg.stream[0].enabled = atoi(val);
    else if (!strcmp(key, "stream0.width"))
        g_cfg.stream[0].width   = atoi(val);
    else if (!strcmp(key, "stream0.height"))
        g_cfg.stream[0].height  = atoi(val);
    else if (!strcmp(key, "stream0.fps"))
        g_cfg.stream[0].fps     = atoi(val);
    else if (!strcmp(key, "stream0.gop"))
        g_cfg.stream[0].gop     = atoi(val);
    else if (!strcmp(key, "stream0.bitrate"))
        g_cfg.stream[0].bitrate = atoi(val);
    else if (!strcmp(key, "stream0.codec"))
        g_cfg.stream[0].codec = !strcmp(val, "h265") ? CODEC_H265 : CODEC_H264;

    /* stream1 */
    else if (!strcmp(key, "stream1.enabled"))
        g_cfg.stream[1].enabled = atoi(val);
    else if (!strcmp(key, "stream1.width"))
        g_cfg.stream[1].width   = atoi(val);
    else if (!strcmp(key, "stream1.height"))
        g_cfg.stream[1].height  = atoi(val);
    else if (!strcmp(key, "stream1.fps"))
        g_cfg.stream[1].fps     = atoi(val);
    else if (!strcmp(key, "stream1.gop"))
        g_cfg.stream[1].gop     = atoi(val);
    else if (!strcmp(key, "stream1.bitrate"))
        g_cfg.stream[1].bitrate = atoi(val);
    else if (!strcmp(key, "stream1.codec"))
        g_cfg.stream[1].codec = !strcmp(val, "h265") ? CODEC_H265 : CODEC_H264;

    /* audio */
    else if (!strcmp(key, "audio_codec")) {
        if      (!strcmp(val, "g711a")) g_cfg.audio_codec = AUDIO_G711A;
        else if (!strcmp(val, "g711u")) g_cfg.audio_codec = AUDIO_G711U;
        else if (!strcmp(val, "aac"))   g_cfg.audio_codec = AUDIO_AAC;
        else                            g_cfg.audio_codec = AUDIO_NONE;
    }
    else if (!strcmp(key, "audio_sample_rate"))
        g_cfg.audio_sample_rate = atoi(val);
    else if (!strcmp(key, "audio_volume"))
        g_cfg.audio_volume = atoi(val);

    /* RTSP */
    else if (!strcmp(key, "rtsp_port"))
        g_cfg.rtsp_port = atoi(val);
    else if (!strcmp(key, "rtsp_user"))
        strncpy(g_cfg.rtsp_user, val, sizeof(g_cfg.rtsp_user) - 1);
    else if (!strcmp(key, "rtsp_pass"))
        strncpy(g_cfg.rtsp_pass, val, sizeof(g_cfg.rtsp_pass) - 1);
    else if (!strcmp(key, "rtsp_auth"))
        g_cfg.rtsp_auth = atoi(val);

    /* HTTP */
    else if (!strcmp(key, "http_port"))
        g_cfg.http_port = atoi(val);
    else if (!strcmp(key, "http_user"))
        strncpy(g_cfg.http_user, val, sizeof(g_cfg.http_user) - 1);
    else if (!strcmp(key, "http_pass"))
        strncpy(g_cfg.http_pass, val, sizeof(g_cfg.http_pass) - 1);
    else if (!strcmp(key, "http_auth"))
        g_cfg.http_auth = atoi(val);

    /* OSD */
    else if (!strcmp(key, "osd_enabled"))
        g_cfg.osd_enabled = atoi(val);
    else if (!strcmp(key, "osd_font"))
        strncpy(g_cfg.osd_font, val, sizeof(g_cfg.osd_font) - 1);
    else if (!strcmp(key, "osd_font_size"))
        g_cfg.osd_font_size = atoi(val);
    else if (!strcmp(key, "osd_label"))
        strncpy(g_cfg.osd_label, val, sizeof(g_cfg.osd_label) - 1);
    else if (!strcmp(key, "osd_x"))
        g_cfg.osd_x = atoi(val);
    else if (!strcmp(key, "osd_y"))
        g_cfg.osd_y = atoi(val);

    /* Motion */
    else if (!strcmp(key, "motion_enabled"))
        g_cfg.motion_enabled = atoi(val);
    else if (!strcmp(key, "motion_sensitivity"))
        g_cfg.motion_sensitivity = atoi(val);
    else if (!strcmp(key, "motion_script"))
        strncpy(g_cfg.motion_script, val, sizeof(g_cfg.motion_script) - 1);

    /* Misc */
    else if (!strcmp(key, "log_level"))
        g_cfg.log_level = atoi(val);
    else if (!strcmp(key, "daemonize"))
        g_cfg.daemonize = atoi(val);

    else
        log_warn("config: unknown key '%s'", key);
}

int config_load(const char *path)
{
    FILE *f;
    char  line[512];
    int   lineno = 0;

    f = fopen(path, "r");
    if (!f) {
        log_error("config: cannot open '%s'", path);
        return -1;
    }

    while (fgets(line, sizeof(line), f)) {
        char *p, *eq, *key, *val;
        lineno++;
        p = trim(line);
        if (*p == '\0' || *p == '#' || *p == ';') continue;

        eq = strchr(p, '=');
        if (!eq) {
            log_warn("config:%d: no '=' in line '%s'", lineno, p);
            continue;
        }
        *eq = '\0';
        key = trim(p);
        val = trim(eq + 1);

        /* Strip inline comments */
        char *comment = strchr(val, '#');
        if (comment) { *comment = '\0'; val = trim(val); }

        apply_kv(key, val);
    }

    fclose(f);
    return 0;
}

void config_dump(void)
{
    const char *codec_name[] = {"h264", "h265"};
    const char *audio_name[] = {"none", "g711a", "g711u", "aac"};

    log_info("=== timps configuration ===");
    log_info("sensor          : %s  (%s)", g_cfg.sensor, g_cfg.sensor_bin);
    for (int i = 0; i < 2; i++) {
        StreamCfg *s = &g_cfg.stream[i];
        if (!s->enabled) { log_info("stream%d         : disabled", i); continue; }
        log_info("stream%d         : %dx%d @ %dfps  %s %dkbps  gop=%d",
                 i, s->width, s->height, s->fps,
                 codec_name[s->codec], s->bitrate, s->gop);
    }
    log_info("audio           : %s  %dHz  vol=%d%%",
             audio_name[g_cfg.audio_codec],
             g_cfg.audio_sample_rate, g_cfg.audio_volume);
    log_info("RTSP            : port=%d  auth=%d  user=%s",
             g_cfg.rtsp_port, g_cfg.rtsp_auth, g_cfg.rtsp_user);
    log_info("HTTP            : port=%d  auth=%d  user=%s",
             g_cfg.http_port, g_cfg.http_auth, g_cfg.http_user);
    log_info("OSD             : %s  font=%s:%d  label='%s'",
             g_cfg.osd_enabled ? "on" : "off",
             g_cfg.osd_font, g_cfg.osd_font_size, g_cfg.osd_label);
    log_info("motion          : %s  sensitivity=%d",
             g_cfg.motion_enabled ? "on" : "off", g_cfg.motion_sensitivity);
}
