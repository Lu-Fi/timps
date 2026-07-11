/* control.c - optional live control. The whole file is empty unless built with
 * -DUSE_CONTROL, so the feature adds nothing to a minimal build.
 *
 * Parses the nested JSON posted to /control (see control.h for the schema),
 * flattens every recognized setting to its config-file key ("image.brightness",
 * "audio.volume", "osd0.0.text", "video0.bitrate", ...) and for each one:
 *   1. updates the in-memory config (config_apply_kv on g_cfg),
 *   2. applies it live through hub_control() -> HAL,
 *   3. collects it for persistence and finally rewrites the changed keys in
 *      the config file (config_write_keys, atomic tmp+rename).
 * No JSON library: targeted scanning like the rest of timps. */
#ifdef USE_CONTROL
#include "control.h"
#include "config.h"
#include "daynight.h"
#include "hub.h"
#include "log.h"
#include "isp_caps.h"
#include "audio_caps.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define MOD "CTRL"

#define CTRL_MAX_CHG 48
typedef struct {
    char key[CTRL_MAX_CHG][32];
    char val[CTRL_MAX_CHG][160];
    int  n;
} ctrl_changes;

/* ---------- tiny range-based JSON scanning ---------- */

static const char *skip_ws(const char *p, const char *e)
{
    while (p<e && (*p==' '||*p=='\t'||*p=='\r'||*p=='\n')) p++;
    return p;
}

/* find "name" within [s,e) followed by ':'; returns pointer to the value
 * (first non-ws char after the colon) or NULL */
static const char *find_field(const char *s, const char *e, const char *name)
{
    char pat[40];
    int pl = snprintf(pat, sizeof pat, "\"%s\"", name);
    if (pl<=0 || pl>=(int)sizeof pat) return NULL;
    for (const char *p=s; p+pl<=e; p++){
        if (memcmp(p, pat, pl)) continue;
        const char *q = skip_ws(p+pl, e);
        if (q<e && *q==':') return skip_ws(q+1, e);
        /* "name" not followed by ':' (e.g. a string value): keep looking */
    }
    return NULL;
}

/* find the object value of "name" within [s,e): returns pointer just past its
 * '{' and sets *oend to the matching '}'. Brace matching skips string
 * literals so OSD texts containing '{placeholders}' cannot derail it. */
static const char *find_obj(const char *s, const char *e, const char *name,
                            const char **oend)
{
    const char *p = find_field(s, e, name);
    if (!p || p>=e || *p!='{') return NULL;
    int depth = 0;
    for (const char *q=p; q<e; q++){
        if (*q=='"'){                          /* skip string literal */
            for (q++; q<e && *q!='"'; q++)
                if (*q=='\\' && q+1<e) q++;
            continue;
        }
        if (*q=='{') depth++;
        else if (*q=='}'){
            if (--depth==0){ *oend=q; return p+1; }
        }
    }
    return NULL;
}

/* extract the scalar value of "name" within [s,e) into out (unquoted strings
 * get minimal unescaping). Returns 1 if found. Object values are rejected. */
static int get_val(const char *s, const char *e, const char *name,
                   char *out, size_t cap)
{
    const char *p = find_field(s, e, name);
    if (!p || p>=e || *p=='{' || *p=='[') return 0;
    size_t o = 0;
    if (*p=='"'){
        for (p++; p<e && *p!='"'; p++){
            char ch = *p;
            if (ch=='\\' && p+1<e){
                p++;
                ch = (*p=='n')?'\n':(*p=='t')?'\t':*p;
            }
            if (o+1<cap) out[o++]=ch;
        }
    } else {
        for (; p<e && *p!=',' && *p!='}' && *p!=']' &&
               *p!=' ' && *p!='\t' && *p!='\r' && *p!='\n'; p++)
            if (o+1<cap) out[o++]=*p;
    }
    out[o]=0;
    return 1;
}

/* ---------- unified apply: config + live + persist collection ---------- */

/* Make a value safe to store in the flat config file: no control chars (a
 * newline would inject a new config line on the next load) and no double quote
 * (would break the "key = \"value\"" quoting). Defense-in-depth even though
 * /control is only reachable from localhost or with valid credentials. */
static void sanitize_val(const char *in, char *out, size_t cap)
{
    size_t o=0;
    for (; *in && o+1<cap; in++){
        unsigned char ch=(unsigned char)*in;
        if (ch < 0x20) ch=' ';
        else if (ch=='"') ch='\'';
        out[o++]=(char)ch;
    }
    out[o]=0;
}

static void timps_apply_setting(ctrl_changes *ch, const char *key, const char *raw)
{
    /* Reject values that are never valid so a stray JSON null/undefined (some
     * WebUI clients poll settings and send a null when a field is unknown) is
     * not stored as "null" and parsed to 0. */
    if (!raw || !raw[0] || !strcmp(raw,"null") || !strcmp(raw,"undefined")){
        LOGD(MOD,"ignoring %s = '%s' (not a valid value)", key, raw?raw:"");
        return;
    }
    char val[160]; sanitize_val(raw, val, sizeof val);

    /* Change detection: apply to the in-memory config, then compare the
     * normalized before/after value. If nothing actually changed, skip the
     * live HAL call and the config-file write. This stops clients that re-post
     * the same value every couple of seconds from hammering the ISP and, worse,
     * rewriting /etc/timps.conf on flash over and over. */
    char before[96], after[96];
    int known = config_get_kv(&g_cfg, key, before, sizeof before);
    config_apply_kv(&g_cfg, key, val);
    if (known){
        config_get_kv(&g_cfg, key, after, sizeof after);
        if (!strcmp(before, after)){
            LOGD(MOD,"unchanged %s = %s (skipped)", key, val);
            return;
        }
    }

    hub_control(key, val);               /* live via the HAL */
    if (ch->n < CTRL_MAX_CHG){
        snprintf(ch->key[ch->n], sizeof ch->key[0], "%s", key);
        snprintf(ch->val[ch->n], sizeof ch->val[0], "%s", val);
        ch->n++;
    } else LOGW(MOD,"too many settings in one request, %s not persisted", key);
    LOGI(MOD,"set %s = %s", key, val);
}

static void apply_section(ctrl_changes *ch, const char *prefix,
                          const char *s, const char *e,
                          const char *const *keys, int nkeys)
{
    char v[160], full[32];
    for (int i=0;i<nkeys;i++){
        if (!get_val(s, e, keys[i], v, sizeof v)) continue;
        snprintf(full, sizeof full, "%s.%s", prefix, keys[i]);
        timps_apply_setting(ch, full, v);
    }
}

void control_apply_json(const char *json)
{
    if (!json || !json[0]) return;
    const char *end = json + strlen(json);
    ctrl_changes ch; ch.n = 0;
    char v[160];

    /* every numeric image.* key (accepted regardless of SoC support: the HAL
     * skips what the platform cannot do, the value still persists) */
    static const char *const IMG[] = {
        "brightness","contrast","saturation","sharpness","hue",
        "hflip","vflip","running_mode","anti_flicker","ae_compensation",
        "max_again","max_dgain","sinter_strength","temper_strength",
        "dpc_strength","defog_strength","drc_strength","highlight_depress",
        "backlight_compensation","core_wb_mode","wb_rgain","wb_bgain"
    };
    /* audio.* live keys (accepted regardless of SoC support, like IMG: the
     * HAL skips what the platform cannot do, the value still persists).
     * "mute" is the live mic mute: the HAL audio thread stops publishing
     * captured frames while it is set (no IMP call needed). */
    static const char *const AUD_LIVE[] = {
        "volume","gain","alc_gain","high_pass","agc",
        "agc_target_dbfs","agc_compression_db","ns","mute"
    };
    /* audio.* persist-only keys: SetPubAttr/encoder-init attributes (plus
     * speaker/stereo keys without a runtime path). They go through the same
     * timps_apply_setting (config + persist); the HAL audio branch only logs
     * them as "applies on restart" instead of touching the running input. */
    static const char *const AUD_REST[] = {
        "enabled","codec","samplerate","channels","bitrate",
        "force_stereo","spk_enabled","spk_volume","spk_gain"
    };
    static const char *const OSD[] = {
        "enabled","text","x","y","font_size","color","transparency",
        "outline","outline_color"
    };
    /* videoN.* encoder/stream keys: ALL persist-only (restart-required).
     * Encoder and FrameSource attributes cannot be changed on the running
     * pipeline, so the HAL video branch only logs them; they persist to the
     * config file and take effect when the timps daemon restarts. */
    static const char *const VID_REST[] = {
        "enabled","codec","width","height","fps","bitrate","rc_mode",
        "gop","max_gop","profile","qp","min_qp","max_qp","rotation",
        "buffers","rtsp_path"
    };
    /* sensor.* keys: persist-only like VID_REST (the sensor is probed and
     * configured once at ISP init). */
    static const char *const SENSOR[] = {
        "model","i2c_addr","fps","width","height"
    };

    /* image: nested "image":{...} preferred; the legacy flat keys of the old
     * /control (top-level brightness/... ) keep working via a whole-body scan */
    const char *se, *sb = find_obj(json, end, "image", &se);
    apply_section(&ch, "image", sb?sb:json, sb?se:end,
                  IMG, (int)(sizeof IMG/sizeof IMG[0]));
    /* legacy day/night: {"force_mode":"night"|"day"} */
    if (get_val(json, end, "force_mode", v, sizeof v)){
        if      (!strcmp(v,"night")) timps_apply_setting(&ch,"image.running_mode","1");
        else if (!strcmp(v,"day"))   timps_apply_setting(&ch,"image.running_mode","0");
    }

    sb = find_obj(json, end, "audio", &se);
    if (sb){
        apply_section(&ch, "audio", sb, se, AUD_LIVE, (int)(sizeof AUD_LIVE/sizeof AUD_LIVE[0]));
        apply_section(&ch, "audio", sb, se, AUD_REST, (int)(sizeof AUD_REST/sizeof AUD_REST[0]));
    }

    /* daynight: {"daynight":{"enabled":true|false}} toggles the native
     * automatic day/night detection (config-only key: the detection thread
     * polls g_cfg, the HAL ignores it). Parsed even in a USE_DAYNIGHT=0
     * build, where it just persists. */
    sb = find_obj(json, end, "daynight", &se);
    if (sb && get_val(sb, se, "enabled", v, sizeof v))
        timps_apply_setting(&ch, "daynight.enabled",
                            (!strcmp(v,"true")||!strcmp(v,"1")) ? "1" :
                            (!strcmp(v,"false")||!strcmp(v,"0")) ? "0" : v);

    /* osd, legacy shared form: {"osd":{"enabled":true,"0":{...},..,"7":{...}}}
     * -> osd.enabled + legacy osdN.* keys (each item is applied to EVERY
     * stream, the pre-per-stream behavior). The master switch is only looked
     * for in the span BEFORE the first nested item object so an item's
     * "enabled" is never mistaken for it (the WebUI bridge emits the osd-level
     * keys first). It is config-only: imp_osd_setup runs once at startup, so
     * it takes effect on restart. */
    sb = find_obj(json, end, "osd", &se);
    if (sb){
        const char *fe = sb;
        while (fe<se && *fe!='{') fe++;
        if (get_val(sb, fe, "enabled", v, sizeof v))
            timps_apply_setting(&ch, "osd.enabled",
                                (!strcmp(v,"true")||!strcmp(v,"1")) ? "1" :
                                (!strcmp(v,"false")||!strcmp(v,"0")) ? "0" : v);
        for (int i=0;i<MS_MAX_OSD;i++){
            char idx[4]; snprintf(idx,sizeof idx,"%d",i);
            const char *ie, *ib = find_obj(sb, se, idx, &ie);
            if (!ib) continue;
            char pre[8]; snprintf(pre,sizeof pre,"osd%d",i);
            apply_section(&ch, pre, ib, ie, OSD, (int)(sizeof OSD/sizeof OSD[0]));
        }
    }

    /* osd, per-stream form: {"osd0":{"0":{...},..},"osd1":{...}} -> canonical
     * osdS.N.* keys; each video stream carries its own independent item set,
     * applied LIVE via imp_osd_apply(stream,item). */
    for (int s=0;s<MS_MAX_VSTREAM;s++){
        char sec[8]; snprintf(sec,sizeof sec,"osd%d",s);
        sb = find_obj(json, end, sec, &se);
        if (!sb) continue;
        for (int i=0;i<MS_MAX_OSD;i++){
            char idx[4]; snprintf(idx,sizeof idx,"%d",i);
            const char *ie, *ib = find_obj(sb, se, idx, &ie);
            if (!ib) continue;
            char pre[12]; snprintf(pre,sizeof pre,"osd%d.%d",s,i);
            apply_section(&ch, pre, ib, ie, OSD, (int)(sizeof OSD/sizeof OSD[0]));
        }
    }

    /* video: {"video":{"0":{"bitrate":3500,"codec":"h264",...},"1":{...}}}
     * -> videoN.* (persist-only: the HAL does not reconfigure the running
     * encoder; changes apply on the next restart) */
    sb = find_obj(json, end, "video", &se);
    if (sb){
        for (int i=0;i<MS_MAX_VSTREAM;i++){
            char idx[4]; snprintf(idx,sizeof idx,"%d",i);
            const char *ie, *ib = find_obj(sb, se, idx, &ie);
            if (!ib) continue;
            char pre[8]; snprintf(pre,sizeof pre,"video%d",i);
            apply_section(&ch, pre, ib, ie, VID_REST, (int)(sizeof VID_REST/sizeof VID_REST[0]));
        }
    }

    /* sensor: {"sensor":{"model":"gc2053","fps":25,...}} -> sensor.*
     * (persist-only, applied at the next ISP init) */
    sb = find_obj(json, end, "sensor", &se);
    if (sb)
        apply_section(&ch, "sensor", sb, se, SENSOR, (int)(sizeof SENSOR/sizeof SENSOR[0]));

    /* persist all changed keys back into the config file */
    if (ch.n > 0 && g_cfg_path && g_cfg_path[0]){
        const char *keys[CTRL_MAX_CHG], *vals[CTRL_MAX_CHG];
        for (int i=0;i<ch.n;i++){ keys[i]=ch.key[i]; vals[i]=ch.val[i]; }
        config_write_keys(g_cfg_path, keys, vals, ch.n);
    }
}

/* ---------- GET /control: dump the current (in-memory) values ---------- */

/* JSON-escape a string into out (bounded) */
static void jesc(const char *s, char *out, size_t cap)
{
    size_t o=0;
    for (; *s && o+2<cap; s++){
        if (*s=='"' || *s=='\\'){ out[o++]='\\'; out[o++]=*s; }
        else if ((unsigned char)*s < 0x20) out[o++]=' ';
        else out[o++]=*s;
    }
    out[o]=0;
}

/* image.* keys the HAL actually wires on this build's PLATFORM (guards from
 * isp_caps.h, same matrix hal_ingenic.c uses). The WebUI reads this from
 * GET /control ("caps") to grey out unsupported controls. */
static const char *const IMG_CAPS[] = {
    "brightness","contrast","saturation","sharpness",
#ifdef ISP_HAS_HUE
    "hue",
#endif
    "hflip","vflip","running_mode","anti_flicker",
#ifdef ISP_HAS_AECOMP
    "ae_compensation",
#endif
#ifdef ISP_HAS_GAINS
    "max_again","max_dgain",
#endif
#ifdef ISP_HAS_NR
    "sinter_strength","temper_strength",
#endif
#ifdef ISP_HAS_DPC
    "dpc_strength",
#endif
#ifdef ISP_HAS_DEFOG
    "defog_strength",
#endif
#ifdef ISP_HAS_DRC
    "drc_strength",
#endif
#ifdef ISP_HAS_HILIGHT
    "highlight_depress",
#endif
#ifdef ISP_HAS_BACKLIGHT
    "backlight_compensation",
#endif
#ifdef ISP_HAS_WB
    "core_wb_mode","wb_rgain","wb_bgain",
#endif
};

/* audio.* keys the HAL can apply LIVE on this build's PLATFORM (guards from
 * audio_caps.h, same matrix ai_apply_key() in hal_ingenic.c uses). Persist-
 * only keys (codec/samplerate/bitrate/channels/enabled/force_stereo/spk_*)
 * are deliberately NOT listed: the WebUI bridge treats everything outside
 * this list as save+restart. */
static const char *const AUD_CAPS[] = {
    "volume","gain",
#ifdef AUDIO_HAS_ALC_GAIN
    "alc_gain",
#endif
    "high_pass","agc","agc_target_dbfs","agc_compression_db","ns",
    "mute",   /* live mic mute: publish gate in the HAL, works on every SoC */
};

int control_get_json(char *buf, size_t cap)
{
    const ms_config *c = &g_cfg;
    size_t o = 0;
    #define APP(...) do { \
        int _n = snprintf(buf+o, o<cap?cap-o:0, __VA_ARGS__); \
        if (_n>0) o += (size_t)_n; \
    } while (0)
    /* caps FIRST: the CGI bridges scan for the *last* occurrence of a key,
     * which must be the value in the image object below, not the caps name */
    APP("{\"caps\":{\"image\":[");
    for (size_t i=0;i<sizeof IMG_CAPS/sizeof IMG_CAPS[0];i++)
        APP("%s\"%s\"", i?",":"", IMG_CAPS[i]);
    APP("],\"audio\":[");
    for (size_t i=0;i<sizeof AUD_CAPS/sizeof AUD_CAPS[0];i++)
        APP("%s\"%s\"", i?",":"", AUD_CAPS[i]);
    /* osd item leaf keys /control accepts (per-stream osdS.N.* and legacy
     * osdN.*; the master switch "osd.enabled" is restart-only). Every video
     * stream has its own independent item set, dumped as "osd0"/"osd1" below;
     * outline/outline_color are the per-item text stroke. The WebUI bridge
     * maps the prudynt OSD tree onto these. */
    APP("],\"osd\":[\"enabled\",\"text\",\"x\",\"y\","
        "\"font_size\",\"color\",\"transparency\",\"outline\",\"outline_color\"");
    /* restart-required sections: every key under these objects is persist-
     * only (config + restart, never applied to the running pipeline). The
     * WebUI bridge reads this to flag such changes as "restart_required". */
    APP("],\"restart\":[\"video\",\"sensor\"]},");
    APP("\"image\":{\"brightness\":%d,\"contrast\":%d,\"saturation\":%d,"
        "\"sharpness\":%d,\"hue\":%d,\"hflip\":%d,\"vflip\":%d,\"running_mode\":%d,",
        c->image.brightness,c->image.contrast,c->image.saturation,
        c->image.sharpness,c->image.hue,c->image.hflip,c->image.vflip,
        c->image.running_mode);
    APP("\"anti_flicker\":%d,\"ae_compensation\":%d,\"max_again\":%d,"
        "\"max_dgain\":%d,\"sinter_strength\":%d,\"temper_strength\":%d,"
        "\"dpc_strength\":%d,\"defog_strength\":%d,\"drc_strength\":%d,"
        "\"highlight_depress\":%d,\"backlight_compensation\":%d,"
        "\"core_wb_mode\":%d,\"wb_rgain\":%d,\"wb_bgain\":%d},",
        c->image.anti_flicker,c->image.ae_compensation,c->image.max_again,
        c->image.max_dgain,c->image.sinter_strength,c->image.temper_strength,
        c->image.dpc_strength,c->image.defog_strength,c->image.drc_strength,
        c->image.highlight_depress,c->image.backlight_compensation,
        c->image.core_wb_mode,c->image.wb_rgain,c->image.wb_bgain);
    {   /* full audio state: live keys + the persist-only (restart) keys */
        char cod[16]="none";
        config_get_kv(c, "audio.codec", cod, sizeof cod);
        APP("\"audio\":{\"volume\":%d,\"gain\":%d,\"alc_gain\":%d,"
            "\"high_pass\":%d,\"agc\":%d,\"agc_target_dbfs\":%d,"
            "\"agc_compression_db\":%d,\"ns\":%d,\"mute\":%d,",
            c->audio.volume, c->audio.gain, c->audio.alc_gain,
            c->audio.high_pass, c->audio.agc, c->audio.agc_target_dbfs,
            c->audio.agc_compression_db, c->audio.ns, c->audio.mute);
        APP("\"enabled\":%d,\"codec\":\"%s\",\"samplerate\":%d,"
            "\"channels\":%d,\"bitrate\":%d,\"force_stereo\":%d,"
            "\"spk_enabled\":%d,\"spk_volume\":%d,\"spk_gain\":%d},",
            c->audio.enabled, cod, c->audio.samplerate,
            c->audio.channels, c->audio.bitrate_kbps, c->audio.force_stereo,
            c->audio.spk_enabled, c->audio.spk_volume, c->audio.spk_gain);
    }
    {   /* sensor (all persist-only / restart-required) */
        char sm[136]; jesc(c->sensor.model, sm, sizeof sm);
        APP("\"sensor\":{\"model\":\"%s\",\"i2c_addr\":%d,\"fps\":%d,"
            "\"width\":%d,\"height\":%d},",
            sm, c->sensor.i2c_addr, c->sensor.fps,
            c->sensor.width, c->sensor.height);
    }
    /* video streams (all persist-only / restart-required). codec/rc_mode go
     * through config_get_kv for the canonical config-file spelling. */
    APP("\"video\":{");
    for (int i=0;i<MS_MAX_VSTREAM;i++){
        const ms_vstream_cfg *vs=&c->video[i];
        char key[20], cod[12]="h264", rc[20]="cbr", rp[136];
        snprintf(key,sizeof key,"video%d.codec",i);
        config_get_kv(c, key, cod, sizeof cod);
        snprintf(key,sizeof key,"video%d.rc_mode",i);
        config_get_kv(c, key, rc, sizeof rc);
        jesc(vs->rtsp_path, rp, sizeof rp);
        APP("%s\"%d\":{\"enabled\":%d,\"codec\":\"%s\",\"width\":%d,"
            "\"height\":%d,\"fps\":%d,\"bitrate\":%d,\"rc_mode\":\"%s\","
            "\"gop\":%d,\"max_gop\":%d,\"profile\":%d,\"qp\":%d,"
            "\"min_qp\":%d,\"max_qp\":%d,\"rotation\":%d,\"buffers\":%d,"
            "\"rtsp_path\":\"%s\"}",
            i?",":"", i, vs->enabled, cod, vs->width, vs->height, vs->fps,
            vs->bitrate_kbps, rc, vs->gop, vs->max_gop, vs->profile,
            vs->qp, vs->min_qp, vs->max_qp, vs->rotation, vs->buffers, rp);
    }
    /* osd: master switch as its own tiny object (kept directly after "video"
     * - the CGI bridge scopes its video scan up to the "osd" marker), then
     * one independent item set per video stream as "osd0"/"osd1", incl. the
     * item type so the bridge can tell text overlays from the logo */
    APP("},\"osd\":{\"enabled\":%d}", c->osd.enabled);
    for (int s=0;s<MS_MAX_VSTREAM;s++){
        APP(",\"osd%d\":{", s);
        for (int i=0;i<MS_MAX_OSD;i++){
            const ms_osd_item *it=&c->osd.items[s][i];
            char t[256]; jesc(it->text, t, sizeof t);
            APP("%s\"%d\":{\"enabled\":%d,\"type\":\"%s\",\"text\":\"%s\","
                "\"x\":%d,\"y\":%d,\"font_size\":%d,\"color\":\"0x%08X\","
                "\"transparency\":%d,\"outline\":%d,\"outline_color\":\"0x%08X\"}",
                i?",":"", i, it->enabled,
                it->type==MS_OSD_LOGO?"logo":"text", t, it->x, it->y,
                it->font_size, it->color, it->transparency,
                it->outline, it->outline_color);
        }
        APP("}");
    }
    {   /* read-only day/night status (never persisted): "enabled" is the
         * auto-detection flag (kept as the FIRST key: the CGI bridges match
         * "daynight":{"enabled":N), mode 0 day / 1 night, brightness in %,
         * total_gain in the IMP [24.8] linear scale (256 = 1x, like
         * GetTotalGain and the prudynt/raptor value the WebUI plots);
         * -1 = unknown. Measured by daynight.c; a stub answers unknowns
         * when built without USE_DAYNIGHT. */
        int dn_en = 0, dn_mode = 0;
        float dn_b = -1.0f, dn_tg = -1.0f;
        daynight_get_status(&dn_en, &dn_mode, &dn_b, &dn_tg);
        APP(",\"daynight\":{\"enabled\":%d,\"mode\":%d,\"brightness\":%.1f,"
            "\"total_gain\":%.0f}}",
            dn_en, dn_mode, (double)dn_b, (double)dn_tg);
    }
    #undef APP
    if (o >= cap){ buf[cap-1]=0; return (int)cap-1; }   /* truncated */
    return (int)o;
}
#endif /* USE_CONTROL */
