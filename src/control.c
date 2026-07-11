/* control.c - optional live control. The whole file is empty unless built with
 * -DUSE_CONTROL, so the feature adds nothing to a minimal build.
 *
 * Parses the nested JSON posted to /control (see control.h for the schema),
 * flattens every recognized setting to its config-file key ("image.brightness",
 * "audio.volume", "osd0.text", "video0.bitrate", ...) and for each one:
 *   1. updates the in-memory config (config_apply_kv on g_cfg),
 *   2. applies it live through hub_control() -> HAL,
 *   3. collects it for persistence and finally rewrites the changed keys in
 *      the config file (config_write_keys, atomic tmp+rename).
 * No JSON library: targeted scanning like the rest of timps. */
#ifdef USE_CONTROL
#include "control.h"
#include "config.h"
#include "hub.h"
#include "log.h"
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

    static const char *const IMG[] = {
        "brightness","contrast","saturation","sharpness","hue",
        "hflip","vflip","running_mode"
    };
    static const char *const AUD[] = { "volume","gain" };
    static const char *const OSD[] = {
        "enabled","text","x","y","font_size","color","transparency"
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
    if (sb) apply_section(&ch, "audio", sb, se, AUD, (int)(sizeof AUD/sizeof AUD[0]));

    /* daynight: {"daynight":{"enabled":true|false}} toggles the native
     * automatic day/night detection (config-only key: the detection thread
     * polls g_cfg, the HAL ignores it). Parsed even in a USE_DAYNIGHT=0
     * build, where it just persists. */
    sb = find_obj(json, end, "daynight", &se);
    if (sb && get_val(sb, se, "enabled", v, sizeof v))
        timps_apply_setting(&ch, "daynight.enabled",
                            (!strcmp(v,"true")||!strcmp(v,"1")) ? "1" :
                            (!strcmp(v,"false")||!strcmp(v,"0")) ? "0" : v);

    /* osd: {"osd":{"0":{...},...,"7":{...}}} -> osdN.* */
    sb = find_obj(json, end, "osd", &se);
    if (sb){
        for (int i=0;i<MS_MAX_OSD;i++){
            char idx[4]; snprintf(idx,sizeof idx,"%d",i);
            const char *ie, *ib = find_obj(sb, se, idx, &ie);
            if (!ib) continue;
            char pre[8]; snprintf(pre,sizeof pre,"osd%d",i);
            apply_section(&ch, pre, ib, ie, OSD, (int)(sizeof OSD/sizeof OSD[0]));
        }
    }

    /* video: {"video":{"0":{"bitrate":3500},"1":{...}}} -> videoN.bitrate */
    sb = find_obj(json, end, "video", &se);
    if (sb){
        for (int i=0;i<MS_MAX_VSTREAM;i++){
            char idx[4]; snprintf(idx,sizeof idx,"%d",i);
            const char *ie, *ib = find_obj(sb, se, idx, &ie);
            if (!ib) continue;
            if (get_val(ib, ie, "bitrate", v, sizeof v)){
                char key[20]; snprintf(key,sizeof key,"video%d.bitrate",i);
                timps_apply_setting(&ch, key, v);
            }
        }
    }

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

int control_get_json(char *buf, size_t cap)
{
    const ms_config *c = &g_cfg;
    size_t o = 0;
    #define APP(...) do { \
        int _n = snprintf(buf+o, o<cap?cap-o:0, __VA_ARGS__); \
        if (_n>0) o += (size_t)_n; \
    } while (0)
    APP("{\"image\":{\"brightness\":%d,\"contrast\":%d,\"saturation\":%d,"
        "\"sharpness\":%d,\"hue\":%d,\"hflip\":%d,\"vflip\":%d,\"running_mode\":%d},",
        c->image.brightness,c->image.contrast,c->image.saturation,
        c->image.sharpness,c->image.hue,c->image.hflip,c->image.vflip,
        c->image.running_mode);
    APP("\"audio\":{\"volume\":%d,\"gain\":%d},", c->audio.volume, c->audio.gain);
    APP("\"video\":{");
    for (int i=0;i<MS_MAX_VSTREAM;i++)
        APP("%s\"%d\":{\"bitrate\":%d}", i?",":"", i, c->video[i].bitrate_kbps);
    APP("},\"osd\":{");
    for (int i=0;i<MS_MAX_OSD;i++){
        const ms_osd_item *it=&c->osd.items[i];
        char t[256]; jesc(it->text, t, sizeof t);
        APP("%s\"%d\":{\"enabled\":%d,\"text\":\"%s\",\"x\":%d,\"y\":%d,"
            "\"font_size\":%d,\"color\":\"0x%08X\",\"transparency\":%d}",
            i?",":"", i, it->enabled, t, it->x, it->y,
            it->font_size, it->color, it->transparency);
    }
    APP("},\"daynight\":{\"enabled\":%d}}", c->daynight.enabled);
    #undef APP
    if (o >= cap){ buf[cap-1]=0; return (int)cap-1; }   /* truncated */
    return (int)o;
}
#endif /* USE_CONTROL */
