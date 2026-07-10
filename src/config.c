#include "config.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>

#define MOD "CONFIG"
ms_config g_cfg;

static void copystr(char *dst, const char *src, size_t n)
{
    strncpy(dst, src, n-1); dst[n-1]=0;
}

static int  pbool(const char *v){ return (!strcasecmp(v,"1")||!strcasecmp(v,"true")||!strcasecmp(v,"on")||!strcasecmp(v,"yes")); }
static int  pint(const char *v){ return (int)strtol(v, NULL, 0); }
static uint32_t phex(const char *v){ return (uint32_t)strtoul(v, NULL, 0); }
static int  pvcodec(const char *v){ return (!strcasecmp(v,"h265")||!strcasecmp(v,"hevc")) ? MS_VC_H265 : MS_VC_H264; }
static int  pacodec(const char *v){
    if (!strcasecmp(v,"aac")) return MS_AC_AAC;
    if (!strcasecmp(v,"pcmu")||!strcasecmp(v,"g711u")||!strcasecmp(v,"ulaw")) return MS_AC_PCMU;
    if (!strcasecmp(v,"pcma")||!strcasecmp(v,"g711a")||!strcasecmp(v,"alaw")) return MS_AC_PCMA;
    if (!strcasecmp(v,"none")||!strcasecmp(v,"off")) return MS_AC_NONE;
    return MS_AC_AAC;
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

void config_defaults(ms_config *c)
{
    memset(c, 0, sizeof(*c));
    c->loglevel = LOG_INFO;
    c->imp_polling_timeout = 500;
    c->osd_pool_size = 1024;   /* max on T-series; holds small OSD regions */

    copystr(c->sensor.model, "gc2053", MS_MAX_STR);
    c->sensor.i2c_addr = 0x37;
    c->sensor.fps = 25;
    c->sensor.width = 1920;
    c->sensor.height = 1080;

    /* ISP image defaults (128 = neutral, like the old streamer) */
    ms_image_cfg *im = &c->image;
    im->brightness=128; im->contrast=128; im->saturation=128; im->sharpness=128; im->hue=128;
    im->vflip=0; im->hflip=0; im->running_mode=0; im->anti_flicker=2; im->ae_compensation=128;
    im->max_again=160; im->max_dgain=80;
    im->sinter_strength=128; im->temper_strength=128; im->dpc_strength=128;
    im->defog_strength=128; im->drc_strength=128;
    im->highlight_depress=0; im->backlight_compensation=0;
    im->core_wb_mode=0; im->wb_rgain=0; im->wb_bgain=0;

    c->rtsp_enabled = 1; c->rtsp_port = 8554; c->rtsp_user[0]=0; c->rtsp_pass[0]=0;
    c->http_enabled = 1; c->http_port = 8080; c->http_preview_chn = 1;
    c->http_user[0]=0; c->http_pass[0]=0;

    for (int i=0;i<MS_MAX_VSTREAM;i++){
        ms_vstream_cfg *v=&c->video[i];
        v->codec=MS_VC_H264; v->fps=25; v->rc_mode=MS_RC_CBR;
        v->gop=50; v->max_gop=60; v->profile=2; v->qp=35; v->min_qp=20; v->max_qp=45;
        v->rotation=0; v->buffers=2; v->imp_chn=i;
        /* piggyback JPEG encoder: off by default (no extra memory/CPU).
         * channels 0..MS_MAX_VSTREAM-1 = video, MS_MAX_VSTREAM = dedicated
         * jpeg channel, so the piggyback encoders start after those. */
        v->jpeg_enabled=0; v->jpeg_quality=75; v->jpeg_fps=5;
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

    c->jpeg.enabled=0; c->jpeg.width=640; c->jpeg.height=360;
    c->jpeg.quality=75; c->jpeg.fps=5; c->jpeg.imp_chn=2;
    c->jpeg.snapshot_path[0]=0;

    /* OSD: array of overlays; one default timestamp item enabled */
    c->osd.enabled=1; c->osd.monitor_stream=0;
    copystr(c->osd.font_path,"/usr/share/fonts/DejaVuSansMono.ttf",128);
    copystr(c->osd.vars_file,"/tmp/timps_osd.vars",128);
    for (int i=0;i<MS_MAX_OSD;i++){
        ms_osd_item *o=&c->osd.items[i];
        o->enabled=0; o->type=MS_OSD_TEXT; o->x=10; o->y=10;
        o->font_size=32; o->color=0xFFFFFFFF; o->transparency=255;
    }
    /* default layout, drawn on every stream. x/y: 0 = centered,
     * positive = from left/top, negative = from right/bottom. */
    c->osd.items[0].enabled=1; c->osd.items[0].x=10;  c->osd.items[0].y=10;   /* top-left   */
    copystr(c->osd.items[0].text,"%Y-%m-%d %H:%M:%S",128);
    c->osd.items[1].enabled=1; c->osd.items[1].x=0;   c->osd.items[1].y=10;   /* top-center */
    copystr(c->osd.items[1].text,"{hostname}",128);
    c->osd.items[2].enabled=1; c->osd.items[2].x=-10; c->osd.items[2].y=10;   /* top-right  */
    copystr(c->osd.items[2].text,"{uptime}",128);
    c->osd.items[3].enabled=1; c->osd.items[3].type=MS_OSD_LOGO;
    c->osd.items[3].x=-10; c->osd.items[3].y=-10;                             /* bottom-right */
    copystr(c->osd.items[3].logo_path,"/usr/share/images/thingino_100x30.bgra",128);
    c->osd.items[3].logo_w=100; c->osd.items[3].logo_h=30;

    c->motion.enabled=0; c->motion.monitor_stream=0; c->motion.sensitivity=128;
    c->motion.cooldown_ms=5000;

    c->sim_video0[0]=0; c->sim_video1[0]=0; c->sim_audio[0]=0;
}

/* returns osd item index for keys "osd0.".."osd7.", else -1 */
static int osd_index(const char *key)
{
    if (strncmp(key,"osd",3)==0 && key[3]>='0' && key[3]<='7' && key[4]=='.')
        return key[3]-'0';
    return -1;
}

static void set_video(ms_vstream_cfg *v, const char *k, const char *val)
{
    if (!strcmp(k,"enabled")) v->enabled=pbool(val);
    else if (!strcmp(k,"codec")) v->codec=pvcodec(val);
    else if (!strcmp(k,"width")) v->width=pint(val);
    else if (!strcmp(k,"height")) v->height=pint(val);
    else if (!strcmp(k,"fps")) v->fps=pint(val);
    else if (!strcmp(k,"bitrate")) v->bitrate_kbps=pint(val);
    else if (!strcmp(k,"rc_mode")||!strcmp(k,"mode")) v->rc_mode=prc(val);
    else if (!strcmp(k,"gop")) v->gop=pint(val);
    else if (!strcmp(k,"max_gop")) v->max_gop=pint(val);
    else if (!strcmp(k,"profile")) v->profile=pint(val);
    else if (!strcmp(k,"qp")) v->qp=pint(val);
    else if (!strcmp(k,"min_qp")) v->min_qp=pint(val);
    else if (!strcmp(k,"max_qp")) v->max_qp=pint(val);
    else if (!strcmp(k,"rotation")) v->rotation=pint(val);
    else if (!strcmp(k,"buffers")) v->buffers=pint(val);
    else if (!strcmp(k,"rtsp_path")) copystr(v->rtsp_path,val,MS_MAX_STR);
    else if (!strcmp(k,"imp_chn")) v->imp_chn=pint(val);
    else if (!strcmp(k,"jpeg")||!strcmp(k,"jpeg_enabled")) v->jpeg_enabled=pbool(val);
    else if (!strcmp(k,"jpeg_quality")) v->jpeg_quality=pint(val);
    else if (!strcmp(k,"jpeg_fps")) v->jpeg_fps=pint(val);
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
    else if (!strcmp(k,"font_size")) o->font_size=pint(val);
    else if (!strcmp(k,"color")||!strcmp(k,"font_color")) o->color=phex(val);
    else if (!strcmp(k,"transparency")) o->transparency=pint(val);
    else if (!strcmp(k,"font_path")) copystr(o->font_path,val,128);
    else LOGW(MOD,"unknown osd item key %s", k);
}

static void set_kv(ms_config *c, const char *key, const char *val)
{
    int oi = osd_index(key);
    if (oi>=0){ set_osd_item(&c->osd.items[oi], key+5, val); return; }

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
        const char *k=key+6; ms_image_cfg *m=&c->image;
        if(!strcmp(k,"brightness"))m->brightness=pint(val);
        else if(!strcmp(k,"contrast"))m->contrast=pint(val);
        else if(!strcmp(k,"saturation"))m->saturation=pint(val);
        else if(!strcmp(k,"sharpness"))m->sharpness=pint(val);
        else if(!strcmp(k,"hue"))m->hue=pint(val);
        else if(!strcmp(k,"vflip"))m->vflip=pbool(val);
        else if(!strcmp(k,"hflip"))m->hflip=pbool(val);
        else if(!strcmp(k,"running_mode"))m->running_mode=pint(val);
        else if(!strcmp(k,"anti_flicker"))m->anti_flicker=pint(val);
        else if(!strcmp(k,"ae_compensation"))m->ae_compensation=pint(val);
        else if(!strcmp(k,"max_again"))m->max_again=pint(val);
        else if(!strcmp(k,"max_dgain"))m->max_dgain=pint(val);
        else if(!strcmp(k,"sinter_strength"))m->sinter_strength=pint(val);
        else if(!strcmp(k,"temper_strength"))m->temper_strength=pint(val);
        else if(!strcmp(k,"dpc_strength"))m->dpc_strength=pint(val);
        else if(!strcmp(k,"defog_strength"))m->defog_strength=pint(val);
        else if(!strcmp(k,"drc_strength"))m->drc_strength=pint(val);
        else if(!strcmp(k,"highlight_depress"))m->highlight_depress=pint(val);
        else if(!strcmp(k,"backlight_compensation"))m->backlight_compensation=pint(val);
        else if(!strcmp(k,"core_wb_mode"))m->core_wb_mode=pint(val);
        else if(!strcmp(k,"wb_rgain"))m->wb_rgain=pint(val);
        else if(!strcmp(k,"wb_bgain"))m->wb_bgain=pint(val);
        else LOGW(MOD,"unknown key %s",key);
        return;
    }
    if (!strncmp(key,"audio.",6)){
        const char *k=key+6;
        if(!strcmp(k,"enabled"))c->audio.enabled=pbool(val);
        else if(!strcmp(k,"codec"))c->audio.codec=pacodec(val);
        else if(!strcmp(k,"samplerate"))c->audio.samplerate=pint(val);
        else if(!strcmp(k,"channels"))c->audio.channels=pint(val);
        else if(!strcmp(k,"bitrate"))c->audio.bitrate_kbps=pint(val);
        else if(!strcmp(k,"volume"))c->audio.volume=pint(val);
        else if(!strcmp(k,"gain"))c->audio.gain=pint(val);
        else if(!strcmp(k,"high_pass"))c->audio.high_pass=pbool(val);
        else if(!strcmp(k,"agc"))c->audio.agc=pbool(val);
        else if(!strcmp(k,"ns"))c->audio.ns=pint(val);
        else LOGW(MOD,"unknown key %s",key);
        return;
    }
    if (!strncmp(key,"jpeg.",5)){
        const char *k=key+5;
        if(!strcmp(k,"enabled"))c->jpeg.enabled=pbool(val);
        else if(!strcmp(k,"width"))c->jpeg.width=pint(val);
        else if(!strcmp(k,"height"))c->jpeg.height=pint(val);
        else if(!strcmp(k,"quality"))c->jpeg.quality=pint(val);
        else if(!strcmp(k,"fps"))c->jpeg.fps=pint(val);
        else if(!strcmp(k,"imp_chn"))c->jpeg.imp_chn=pint(val);
        else if(!strcmp(k,"snapshot_path"))copystr(c->jpeg.snapshot_path,val,128);
        else LOGW(MOD,"unknown key %s",key);
        return;
    }
    if (!strncmp(key,"rtsp.",5)){
        const char *k=key+5;
        if(!strcmp(k,"enabled"))c->rtsp_enabled=pbool(val);
        else if(!strcmp(k,"port"))c->rtsp_port=pint(val);
        else if(!strcmp(k,"user")||!strcmp(k,"username"))copystr(c->rtsp_user,val,MS_MAX_STR);
        else if(!strcmp(k,"pass")||!strcmp(k,"password"))copystr(c->rtsp_pass,val,MS_MAX_STR);
        else LOGW(MOD,"unknown key %s",key);
        return;
    }
    if (!strncmp(key,"http.",5)){
        const char *k=key+5;
        if(!strcmp(k,"enabled"))c->http_enabled=pbool(val);
        else if(!strcmp(k,"port"))c->http_port=pint(val);
        else if(!strcmp(k,"preview_chn"))c->http_preview_chn=pint(val);
        else if(!strcmp(k,"user")||!strcmp(k,"username"))copystr(c->http_user,val,MS_MAX_STR);
        else if(!strcmp(k,"pass")||!strcmp(k,"password"))copystr(c->http_pass,val,MS_MAX_STR);
        else LOGW(MOD,"unknown key %s",key);
        return;
    }
    if (!strncmp(key,"osd.",4)){
        const char *k=key+4;
        if(!strcmp(k,"enabled"))c->osd.enabled=pbool(val);
        else if(!strcmp(k,"monitor_stream"))c->osd.monitor_stream=pint(val);
        else if(!strcmp(k,"font_path"))copystr(c->osd.font_path,val,128);
        else if(!strcmp(k,"vars_file"))copystr(c->osd.vars_file,val,128);
        else LOGW(MOD,"unknown key %s",key);
        return;
    }
    if (!strncmp(key,"motion.",7)){
        const char *k=key+7;
        if(!strcmp(k,"enabled"))c->motion.enabled=pbool(val);
        else if(!strcmp(k,"monitor_stream"))c->motion.monitor_stream=pint(val);
        else if(!strcmp(k,"sensitivity"))c->motion.sensitivity=pint(val);
        else if(!strcmp(k,"roi_x"))c->motion.roi_x=pint(val);
        else if(!strcmp(k,"roi_y"))c->motion.roi_y=pint(val);
        else if(!strcmp(k,"roi_w"))c->motion.roi_w=pint(val);
        else if(!strcmp(k,"roi_h"))c->motion.roi_h=pint(val);
        else if(!strcmp(k,"cooldown_ms"))c->motion.cooldown_ms=pint(val);
        else if(!strcmp(k,"on_motion"))copystr(c->motion.on_motion,val,128);
        else LOGW(MOD,"unknown key %s",key);
        return;
    }
    if (!strncmp(key,"general.",8)){
        const char *k=key+8;
        if(!strcmp(k,"loglevel"))c->loglevel=pint(val);
        else if(!strcmp(k,"imp_polling_timeout"))c->imp_polling_timeout=pint(val);
        else if(!strcmp(k,"osd_pool_size"))c->osd_pool_size=pint(val);
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

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) *e-- = 0;
    return s;
}

int config_load(ms_config *c, const char *path)
{
    config_defaults(c);
    FILE *f = fopen(path, "r");
    if (!f) { LOGW(MOD,"config %s not found, using defaults", path); return -1; }
    char line[512];
    int n=0;
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s=='#' || *s==';') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(s);
        char *val = trim(eq+1);
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
