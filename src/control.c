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
#include "events.h"
#include "hub.h"
#include "log.h"
#include "isp_caps.h"
#include "audio_caps.h"
#include "motion_caps.h"
#include "rtsp/backchannel.h"
#include "rtsp/speaker.h"
#include "rotate_caps.h"   /* ROT_HAS_90/ROT_HAS_HW_I2D (rotation caps + eff dims) */
#include "hal/imp_motion.h"
#include "hal/imp_osd.h"
#include "hal/hal.h"       /* hal_enc_stats: read-only encoder telemetry */
#include "record.h"
#include "timelapse.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>   /* serialize concurrent control_apply_json POSTs (A1) */
#ifdef USE_PLAY
#include <dirent.h>
#include <sys/stat.h>
#endif

#define MOD "CTRL"

#ifdef USE_PLAY
#define SOUNDS_DIR "/usr/share/sounds"
/* Hard cap on how many sound files caps.play.sounds will enumerate into the
 * GET /control JSON. This is a truncation-prevention measure, NOT an arbitrary
 * restriction: control_get_json() builds the whole document in one fixed heap
 * buffer (CONTROL_JSON_CAP in httpd.c), and the sounds list is the only
 * unbounded contributor - this project supports large media-player-style sound
 * libraries (ESPHome media_player), so a directory with hundreds of files could
 * otherwise overflow the buffer and make the API return truncated, invalid JSON.
 * Every other caps.* list is inherently bounded (fixed arrays / SDK limits); we
 * bound this one explicitly. A client needing the full library can read the
 * directory another way (it is a plain filesystem path) - listing it here is a
 * convenience for the WebUI test-sound picker, which only needs a workable set. */
#define SOUNDS_LIST_MAX 96

/* Resolve a test-sound name to its full path under SOUNDS_DIR, rejecting path
 * traversal and anything that is not a regular file actually present there
 * (the same set the caps.play "sounds" list reports). Returns 1 on success. */
static int sound_path(const char *name, char *out, size_t cap)
{
    if (!name || !name[0] || strchr(name,'/') || strstr(name,"..")) return 0;
    char p[300];
    if ((size_t)snprintf(p,sizeof p,"%s/%s",SOUNDS_DIR,name) >= sizeof p) return 0;
    struct stat st;
    if (stat(p,&st)!=0 || !S_ISREG(st.st_mode)) return 0;
    snprintf(out,cap,"%s",p);
    return 1;
}
#endif

#define CTRL_MAX_CHG 48
typedef struct {
    char key[CTRL_MAX_CHG][40];  /* fits daynight.total_gain_night_threshold */
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

static int hexdig(char c)
{
    if (c>='0'&&c<='9') return c-'0';
    if (c>='a'&&c<='f') return c-'a'+10;
    if (c>='A'&&c<='F') return c-'A'+10;
    return -1;
}

/* parse exactly 4 hex digits at [h,e); returns 0..0xFFFF, or -1 on short
 * input / non-hex (so a malformed \uXX... can't read out of bounds). */
static long hex4(const char *h, const char *e)
{
    long v = 0;
    for (int i=0;i<4;i++){
        if (h+i>=e) return -1;
        int d = hexdig(h[i]);
        if (d<0) return -1;
        v = (v<<4)|d;
    }
    return v;
}

/* Append code point cp to out as UTF-8, starting at index o, keeping one byte
 * reserved for the terminating NUL. Writes nothing (returns 0) if it wouldn't
 * fit. Returns the number of bytes written (1..4). This daemon's strings are
 * UTF-8 throughout, so decoded \uXXXX escapes are re-encoded here as UTF-8. */
static size_t utf8_enc(unsigned cp, char *out, size_t o, size_t cap)
{
    unsigned char b[4]; size_t n;
    if      (cp < 0x80)   { b[0]=(unsigned char)cp; n=1; }
    else if (cp < 0x800)  { b[0]=0xC0|(cp>>6);  b[1]=0x80|(cp&0x3F); n=2; }
    else if (cp < 0x10000){ b[0]=0xE0|(cp>>12); b[1]=0x80|((cp>>6)&0x3F);
                            b[2]=0x80|(cp&0x3F); n=3; }
    else                  { b[0]=0xF0|(cp>>18); b[1]=0x80|((cp>>12)&0x3F);
                            b[2]=0x80|((cp>>6)&0x3F); b[3]=0x80|(cp&0x3F); n=4; }
    if (o + n + 1 > cap) return 0;   /* +1 reserves the NUL out[o]=0 below */
    for (size_t i=0;i<n;i++) out[o+i]=(char)b[i];
    return n;
}

/* extract the scalar value of "name" within [s,e) into out (quoted strings get
 * full RFC 8259 escape decoding: \" \\ \/ \b \f \n \r \t and \uXXXX incl.
 * UTF-16 surrogate pairs, re-encoded as UTF-8). Returns 1 if found. Object
 * values are rejected. */
static int get_val(const char *s, const char *e, const char *name,
                   char *out, size_t cap)
{
    const char *p = find_field(s, e, name);
    if (!p || p>=e || *p=='{' || *p=='[') return 0;
    size_t o = 0;
    if (*p=='"'){
        for (p++; p<e && *p!='"'; p++){
            unsigned cp;
            if (*p=='\\' && p+1<e){
                p++;
                switch (*p){
                case 'n': cp='\n'; break;
                case 't': cp='\t'; break;
                case 'r': cp='\r'; break;
                case 'b': cp='\b'; break;
                case 'f': cp='\f'; break;
                case '/': cp='/';  break;   /* correct by design, not by accident */
                case '"': cp='"';  break;
                case '\\':cp='\\'; break;
                case 'u': {
                    long u = hex4(p+1, e);
                    if (u < 0){ cp=0xFFFD; break; }   /* not 4 hex -> U+FFFD */
                    p += 4;                            /* consumed the 4 digits */
                    if (u>=0xD800 && u<=0xDBFF){       /* high surrogate */
                        long lo = (p+2<e && p[1]=='\\' && p[2]=='u')
                                    ? hex4(p+3, e) : -1;
                        if (lo>=0xDC00 && lo<=0xDFFF){
                            cp = 0x10000u + (unsigned)(((u-0xD800)<<10)
                                                       + (lo-0xDC00));
                            p += 6;                    /* consumed \uXXXX low */
                        } else cp = 0xFFFD;            /* unpaired high */
                    } else if (u>=0xDC00 && u<=0xDFFF){
                        cp = 0xFFFD;                   /* lone low surrogate */
                    } else cp = (unsigned)u;
                    break;
                }
                default: cp=(unsigned char)*p; break;  /* unknown -> literal */
                }
            } else cp = (unsigned char)*p;
            o += utf8_enc(cp, out, o, cap);
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
 * /control is only reachable from localhost or with valid credentials.
 *
 * Also no backslash: this sanitized value is spliced unescaped straight into
 * a JSON string for the /events "config" push (httpd.c events_stream), and a
 * raw backslash there produces an invalid (or, worse, a maliciously
 * reinterpreted) escape sequence - a lone '\"' at the end would even escape
 * the closing quote. Replacing it here keeps both consumers (config file,
 * JSON event) safe without a second escaping pass. */
static void sanitize_val(const char *in, char *out, size_t cap)
{
    size_t o=0;
    for (; *in && o+1<cap; in++){
        unsigned char ch=(unsigned char)*in;
        if (ch < 0x20) ch=' ';
        else if (ch=='"') ch='\'';
        else if (ch=='\\') ch='/';
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
    /* sized to match val[]/the general 160-char value cap (config_get_kv's
     * snprintf(out,cap,...) truncates to fit) - a 96-byte cap here let two
     * long values (e.g. OSD text, up to ~127 chars) that only differ past
     * byte 95 compare as "unchanged", silently skipping hub_control()/the
     * /events push/the config-file persist for a real change. */
    char before[160], after[160];
    config_str_lock();     /* g_cfg strings are read by other threads */
    int known = config_get_kv(&g_cfg, key, before, sizeof before);
    config_apply_kv(&g_cfg, key, val);
    /* Read the CANONICAL stored value back - the clamped/normalized form,
     * exactly as GET /control (also config_get_kv) reports it. Downstream we
     * feed THIS, not the raw pre-clamp POST string, to the HAL, the /events
     * echo and the config-file persist, so all three agree with the daemon's
     * in-memory g_cfg instead of leaving an out-of-range value in the file /
     * SSE that only self-corrects on the next POST (and survives a reboot,
     * since load re-clamps in memory but never rewrites the file). Read
     * UNCONDITIONALLY, not gated on `known`: a legacy osdN.* write to a key
     * whose per-stream item sets had diverged reads back unknown BEFORE the
     * write (config_get_kv reports the legacy key only while all streams
     * agree) but re-converges every stream, so the AFTER read now succeeds and
     * carries the clamped value - gating on `known` would miss exactly that
     * clamped case. `canon` is the AFTER-read success; use `after` only when it
     * is set, else fall back to `val` (F_NOGET fields / noget sections have no
     * readable canonical form - none of those keys are settable through this
     * funnel today, so that fallback is never a clamped value in practice). */
    int canon = config_get_kv(&g_cfg, key, after, sizeof after);
    config_str_unlock();
    const char *out = canon ? after : val;   /* canonical value for consumers */
    if (known && !strcmp(before, after)){
        /* image.running_mode is a hardware-SYNC command to the ISP, not just a
         * stored value: the ISP's actual day/night state can drift from our
         * config model. A SetISPRunningMode issued at the dusk day->night
         * crossover (while AE is still ramping) is accepted but does not always
         * latch, leaving the sensor in colour mode under IR (magenta cast) even
         * though our config - and the board day/night script's "color off",
         * which re-POSTs the SAME running_mode=1 - already say night. Plain
         * change-detection then swallowed that re-POST, so re-asserting the mode
         * (manually or from the script) was impossible: the only way to recover
         * was a full toggle. Re-drive the ISP on a no-change running_mode POST so
         * a repeat call actually re-asserts the mode, but skip the flash persist
         * (nothing changed) so a client re-posting it every few seconds still
         * cannot hammer the config file. Every other key keeps the plain skip. */
        if (!strcmp(key,"image.running_mode")){
            hub_control(key, out);       /* re-assert to the ISP, no re-persist */
            LOGD(MOD,"re-applied %s = %s to HAL (unchanged, not persisted)", key, out);
        } else
            LOGD(MOD,"unchanged %s = %s (skipped)", key, out);
        return;
    }

    /* L1: motion.sensitivity is quantized to the SDK's 0..4 range (v*4/255 in
     * imp_motion.c) before it reaches IVS, so a raw change that maps to the SAME
     * level (e.g. 128->129) has zero effect on detection. The change-detection
     * above only skips when the RAW values match; compare the MAPPED values too
     * and, when they match, skip the expensive IVS grid rebuild (deferred via
     * M2's g_motion_resync_pending - never flagged because hub_control() is
     * skipped, so hub_control_commit() finds nothing to do) and the flash
     * persist. Keep the mapping in sync with imp_motion.c. */
    if (known && !strcmp(key,"motion.sensitivity") &&
        atoi(before)*4/255 == atoi(after)*4/255){
        LOGD(MOD,"unchanged %s = %s (same effective sensitivity level, skipped)", key, val);
        return;
    }

    hub_control(key, out);               /* live via the HAL */
    /* echo to every other /events subscriber ("config" SSE event) so other
     * open WebUI tabs/clients reflect this change instead of only seeing it
     * on next poll. motion- and daynight-prefixed keys additionally still
     * drive their own richer status events from imp_motion.c/daynight.c -
     * this is just the raw settings echo, for everything else too. */
    events_config_push(key, out);
    if (ch->n < CTRL_MAX_CHG){
        snprintf(ch->key[ch->n], sizeof ch->key[0], "%s", key);
        snprintf(ch->val[ch->n], sizeof ch->val[0], "%s", out);
        ch->n++;
    } else LOGW(MOD,"too many settings in one request, %s not persisted", key);
    LOGI(MOD,"set %s = %s", key, out);
}

/* Generic per-section field-table walker: replaces the 11 hand-written
 * per-section name arrays (IMG/AUD_LIVE/AUD_REST/OSD/VID_REST/SENSOR/
 * DN_KEYS/MOTION_KEYS/MOTION_RESTART_KEYS/REC_KEYS/TL_KEYS/PRIV_KEYS) that
 * used to re-list ~100 field names already present in config.c's cfg_field
 * tables - the split between them and config.c's tables was the confirmed
 * root cause of several fields silently landing in config.c but never being
 * wired into a control.c array (fixed in the 55ead66 commit's nine fields +
 * osd item "type"). tbl[]/n now come straight from config.c's
 * cfg_fields_*() accessors, so a field can no longer exist in one table but
 * not the other.
 *
 * Only entries with F_CTRL set are ever applied - this is still a mandatory-
 * per-field SECURITY ALLOWLIST, not a walk-everything default: motion.
 * on_motion/cooldown_ms, daynight.switch_cmd/isp_path, every rtsp.* / http.*
 * credential/token and the videoN.imp_chn / jpeg* / jpeg_chn channel-wiring
 * fields all intentionally have no F_CTRL in config.c and so are silently
 * skipped here even if present in the POSTed JSON - see the F_CTRL doc
 * comment in config.h. Field names are matched using tbl[i].name only
 * (never .alias): the JSON body has only ever recognized the canonical
 * spellings the old arrays hard-coded (e.g. "rc_mode", not its "mode"
 * config-file alias), and this preserves that exactly. */
static void apply_ctrl_fields(ctrl_changes *ch, const char *prefix,
                              const char *s, const char *e,
                              const cfg_field *tbl, int n)
{
    char v[160], full[40];
    for (int i=0;i<n;i++){
        if (!(tbl[i].flags & F_CTRL)) continue;
        if (!get_val(s, e, tbl[i].name, v, sizeof v)) continue;
        snprintf(full, sizeof full, "%s.%s", prefix, tbl[i].name);
        timps_apply_setting(ch, full, v);
    }
}

void control_apply_json(const char *json)
{
    if (!json || !json[0]) return;
    /* A1 (concurrent-POST class): one HTTP worker thread per connection calls
     * this (httpd.c), so two simultaneous POSTs would otherwise interleave their
     * apply-to-g_cfg + hub_control + persist sequences and leave a partially
     * applied config. Serialize the whole apply-and-notify body with a single
     * mutex so one POST completes before the next starts. This is orthogonal to
     * config_str_lock (which guards individual writes against background
     * READERS); this guards apply-vs-apply. hub_control()/hub_control_commit()
     * still run after each field's config_str_unlock() as before. */
    static pthread_mutex_t apply_mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&apply_mu);
    const char *end = json + strlen(json);
    /* heap, not stack: 48*(40+160) = 9.6 KB is the largest single frame in
     * the daemon's hottest request path (a dragged slider posts often) and
     * this sits under buf[] in conn_thread too - same reasoning as the
     * /control GET json[] buffer. */
    ctrl_changes *ch = (ctrl_changes *)malloc(sizeof *ch);
    if (!ch) { LOGW(MOD,"control_apply_json: OOM"); pthread_mutex_unlock(&apply_mu); return; }
    ch->n = 0;
    char v[160];

    /* image: nested "image":{...} preferred; the legacy flat keys of the old
     * /control (top-level brightness/... ) keep working via a whole-body scan.
     * image_fields (config.c) covers every numeric image.* key, accepted
     * regardless of SoC support: the HAL skips what the platform cannot do,
     * the value still persists. */
    int nimg; const cfg_field *img_tbl = cfg_fields_image(&nimg);
    const char *se, *sb = find_obj(json, end, "image", &se);
    apply_ctrl_fields(ch, "image", sb?sb:json, sb?se:end, img_tbl, nimg);
    /* legacy day/night: {"force_mode":"night"|"day"} */
    if (get_val(json, end, "force_mode", v, sizeof v)){
        if      (!strcmp(v,"night")) timps_apply_setting(ch,"image.running_mode","1");
        else if (!strcmp(v,"day"))   timps_apply_setting(ch,"image.running_mode","0");
    }

    /* audio: audio_fields (config.c) covers both the "live" keys (volume/
     * gain/alc_gain/mute/spk_volume/spk_gain/aec - applied immediately to
     * the running AI/AO, see hub.c's audio branch) and the persist-only
     * SetPubAttr/encoder-init keys (enabled/codec/samplerate/channels/
     * bitrate/high_pass/agc/agc_target_dbfs/agc_compression_db/ns/
     * force_stereo/spk_enabled/backchannel*), applied on the next restart -
     * that live-vs-restart split is a HAL-side concern, not a POST-
     * reachability one, so one table walk covers both. */
    sb = find_obj(json, end, "audio", &se);
    if (sb){
        int naud; const cfg_field *aud_tbl = cfg_fields_audio(&naud);
        apply_ctrl_fields(ch, "audio", sb, se, aud_tbl, naud);
    }

#ifdef USE_PLAY
    /* speaker: {"speaker":{"play":"<file>"}} plays a system sound (validated
     * against SOUNDS_DIR), {"speaker":{"stop":1}} stops it. Not persisted - a
     * transient action that just enqueues on the play FIFO speaker.c reads. */
    sb = find_obj(json, end, "speaker", &se);
    if (sb){
        /* only stop on a TRUTHY "stop" - {"stop":false|0|null} must not stop,
         * and (else-if below) must not shadow a "play" sent in the same object.
         * Same truthiness idiom used for the boolean fields elsewhere here. */
        int do_stop = get_val(sb, se, "stop", v, sizeof v) &&
                      (!strcmp(v,"true")||!strcmp(v,"1"));
        if (do_stop){
            speaker_play_line("STOP");
            LOGI(MOD,"speaker stop");
        } else if (get_val(sb, se, "play", v, sizeof v)){
            char full[320], line[400];
            if (sound_path(v, full, sizeof full)){
                snprintf(line, sizeof line, "PLAY url=%s", full);
                speaker_play_line(line);
                LOGI(MOD,"speaker play %s", full);
            } else LOGW(MOD,"speaker play: rejected '%s'", v);
        }
    }
#endif

    /* daynight: {"daynight":{"enabled":..,"total_gain_day_threshold":..,
     * "total_gain_night_threshold":..}} - the native automatic day/night
     * detection switch + its gain thresholds (config-only keys: the detection
     * thread polls g_cfg, the HAL ignores them). Parsed even in a
     * USE_DAYNIGHT=0 build, where they just persist. */
    sb = find_obj(json, end, "daynight", &se);
    if (sb){
        /* mode: string token, validated here so garbage never corrupts state
         * (config_apply_kv would coerce an unknown token to sensor, but reject
         * it up front and log rather than silently persisting nonsense). This
         * is why daynight_fields' "mode" entry (config.c) has no F_CTRL: the
         * generic walker below must not also touch it. Every other
         * daynight.* key (enabled, the time_* / sun_* strings and numerics,
         * the gain thresholds, ...) rides the generic walker now. */
        if (get_val(sb, se, "mode", v, sizeof v)){
            if (!strcmp(v,"sensor")||!strcmp(v,"time")||!strcmp(v,"sun"))
                timps_apply_setting(ch, "daynight.mode", v);
            else
                LOGW(MOD,"ignoring daynight.mode = '%s' (not sensor/time/sun)", v);
        }
        int ndn; const cfg_field *dn_tbl = cfg_fields_daynight(&ndn);
        apply_ctrl_fields(ch, "daynight", sb, se, dn_tbl, ndn);
    }

    /* osd, legacy shared form: {"osd":{"enabled":true,"0":{...},..,"7":{...}}}
     * -> osd.enabled + legacy osdN.* keys (each item is applied to EVERY
     * stream, the pre-per-stream behavior). The master switch (and the other
     * osd.* globals below) are only looked for in the span BEFORE the first
     * nested item object so an item's own keys (e.g. an item's "enabled") are
     * never mistaken for them (the WebUI bridge emits the osd-level keys
     * first). All of them are config-only: imp_osd_setup builds the OSD
     * groups once at startup, so they take effect on restart. */
    sb = find_obj(json, end, "osd", &se);
    if (sb){
        const char *fe = sb;
        while (fe<se && *fe!='{') fe++;
        /* osd.* globals (enabled/monitor_stream/font_path/vars_file/
         * supersample/hinting): all restart-required, all F_CTRL in
         * osd_fields (config.c) - one generic walk covers what used to be
         * the hand-written "enabled" special-case plus OSD_GLOBAL_KEYS[]. */
        int nosd; const cfg_field *osd_tbl = cfg_fields_osd(&nosd);
        apply_ctrl_fields(ch, "osd", sb, fe, osd_tbl, nosd);
        int nitem; const cfg_field *item_tbl = cfg_fields_osd_item(&nitem);
        for (int i=0;i<MS_MAX_OSD;i++){
            char idx[4]; snprintf(idx,sizeof idx,"%d",i);
            const char *ie, *ib = find_obj(sb, se, idx, &ie);
            if (!ib) continue;
            char pre[8]; snprintf(pre,sizeof pre,"osd%d",i);
            apply_ctrl_fields(ch, pre, ib, ie, item_tbl, nitem);
        }
    }

    /* osd, per-stream form: {"osd0":{"0":{...},..},"osd1":{...}} -> canonical
     * osdS.N.* keys; each video stream carries its own independent item set,
     * applied LIVE via imp_osd_apply(stream,item). */
    for (int s=0;s<MS_MAX_VSTREAM;s++){
        char sec[8]; snprintf(sec,sizeof sec,"osd%d",s);
        sb = find_obj(json, end, sec, &se);
        if (!sb) continue;
        int nitem; const cfg_field *item_tbl = cfg_fields_osd_item(&nitem);
        for (int i=0;i<MS_MAX_OSD;i++){
            char idx[4]; snprintf(idx,sizeof idx,"%d",i);
            const char *ie, *ib = find_obj(sb, se, idx, &ie);
            if (!ib) continue;
            char pre[12]; snprintf(pre,sizeof pre,"osd%d.%d",s,i);
            apply_ctrl_fields(ch, pre, ib, ie, item_tbl, nitem);
        }
    }

    /* video: {"video":{"0":{"bitrate":3500,"codec":"h264",...},"1":{...}}}
     * -> videoN.* (persist-only: the HAL does not reconfigure the running
     * encoder; changes apply on the next restart) */
    sb = find_obj(json, end, "video", &se);
    if (sb){
        int nvid; const cfg_field *vid_tbl = cfg_fields_video(&nvid);
        for (int i=0;i<MS_MAX_VSTREAM;i++){
            char idx[4]; snprintf(idx,sizeof idx,"%d",i);
            const char *ie, *ib = find_obj(sb, se, idx, &ie);
            if (!ib) continue;
            char pre[8]; snprintf(pre,sizeof pre,"video%d",i);
            apply_ctrl_fields(ch, pre, ib, ie, vid_tbl, nvid);
        }
    }

    /* privacy: {"privacy":{"<s>":{"<n>":{enabled,x,y,w,h,color}}}} -> the
     * privacy<S>.<N>.* cover-mask keys. Applied LIVE (the HAL creates/shows/
     * moves the IMP OSD cover region on that stream) and persisted. */
    sb = find_obj(json, end, "privacy", &se);
    if (sb){
        int nprivf; const cfg_field *priv_tbl = cfg_fields_privacy(&nprivf);
        for (int s=0;s<MS_MAX_VSTREAM;s++){
            char sidx[4]; snprintf(sidx,sizeof sidx,"%d",s);
            const char *sse, *ssb = find_obj(sb, se, sidx, &sse);
            if (!ssb) continue;
            for (int n=0;n<MS_MAX_PRIVACY;n++){
                char nidx[4]; snprintf(nidx,sizeof nidx,"%d",n);
                const char *ne, *nb = find_obj(ssb, sse, nidx, &ne);
                if (!nb) continue;
                char pre[16]; snprintf(pre,sizeof pre,"privacy%d.%d",s,n);
                apply_ctrl_fields(ch, pre, nb, ne, priv_tbl, nprivf);
            }
        }
    }

    /* sensor: {"sensor":{"model":"gc2053","fps":25,...}} -> sensor.*
     * (persist-only, applied at the next ISP init) */
    sb = find_obj(json, end, "sensor", &se);
    if (sb){
        int nsen; const cfg_field *sen_tbl = cfg_fields_sensor(&nsen);
        apply_ctrl_fields(ch, "sensor", sb, se, sen_tbl, nsen);
    }

    /* motion: {"motion":{"enabled":..,"sensitivity":..,"cols":..,"rows":..,
     * "monitor_stream":..,"hold_ms":..,"skip_frames":..}} -> motion.*. Most
     * are applied LIVE: the HAL stops and recreates the IVS grid on any of
     * enabled/sensitivity/cols/rows/monitor_stream (move params are create-
     * time attributes); hold_ms/skip_frames only feed the grid at the next
     * such rebuild or a restart. cols/rows are clamped to the SDK's cell
     * budget by the config layer (caps.motion.max_cells).
     * motion.cooldown_ms/on_motion are deliberately NOT settable over HTTP
     * (no F_CTRL in motion_fields, config.c): on_motion is run via
     * fork()+execlp() (no shell) and stays config-file-only, and cooldown_ms
     * is the floor that bounds how often it re-fires - see the security-
     * boundary comment on motion_fields in config.c before ever adding
     * F_CTRL to either. */
    sb = find_obj(json, end, "motion", &se);
    if (sb){
        int nmot; const cfg_field *mot_tbl = cfg_fields_motion(&nmot);
        apply_ctrl_fields(ch, "motion", sb, se, mot_tbl, nmot);
    }

    /* record: {"record":{"active":1|0}} = manual start/stop override (the
     * control-bar record button); active omitted or <0 returns to config mode.
     * record.* config keys persist and the running recorder reads them live. */
    sb = find_obj(json, end, "record", &se);
    if (sb){
        if (get_val(sb, se, "active", v, sizeof v))
            record_set_active((!strcmp(v,"true")||!strcmp(v,"1")) ? 1 :
                              (!strcmp(v,"false")||!strcmp(v,"0")) ? 0 : -1);
        {   /* {"record":{"clip":"/tmp/x.mp4","seconds":6}} -> capture an
             * on-demand fMP4 clip (blocks ~seconds); used by send2 video. */
            char clip[160];
            if (get_val(sb, se, "clip", clip, sizeof clip)){
                int secs = get_val(sb, se, "seconds", v, sizeof v) ? atoi(v) : 6;
                record_clip(clip, secs);
            }
        }
        int nrec; const cfg_field *rec_tbl = cfg_fields_record(&nrec);
        apply_ctrl_fields(ch, "record", sb, se, rec_tbl, nrec);
    }

    /* timelapse: {"timelapse":{"enabled":..,"channel":..,"dir":..,"name":..,
     * "interval_s":..,"keep_days":..}} -> timelapse.*. All persist; the
     * running timelapse thread reads them live (no restart). */
    sb = find_obj(json, end, "timelapse", &se);
    if (sb){
        int ntl; const cfg_field *tl_tbl = cfg_fields_timelapse(&ntl);
        apply_ctrl_fields(ch, "timelapse", sb, se, tl_tbl, ntl);
    }

    /* M2: flush any HAL apply that was deferred/batched across the keys of this
     * request (e.g. the IVS motion-grid rebuild) exactly once, now that every
     * key has been applied to g_cfg. No-op when nothing motion/privacy-related
     * changed. */
    hub_control_commit();

    /* persist all changed keys back into the config file */
    if (ch->n > 0 && g_cfg_path && g_cfg_path[0]){
        const char *keys[CTRL_MAX_CHG], *vals[CTRL_MAX_CHG];
        for (int i=0;i<ch->n;i++){ keys[i]=ch->key[i]; vals[i]=ch->val[i]; }
        config_write_keys(g_cfg_path, keys, vals, ch->n);
    }
    free(ch);
    pthread_mutex_unlock(&apply_mu);
}

/* ---------- GET /control: dump the current (in-memory) values ---------- */

/* JSON-escape a string into out (bounded) */
/* Escape a UTF-8 string for embedding in a JSON string value: backslash the "
 * and \, drop control chars (< 0x20), and VALIDATE multi-byte UTF-8. A byte
 * >= 0x80 that does not begin a well-formed, minimally-encoded, non-surrogate
 * sequence (<= U+10FFFF) is replaced with U+FFFD (EF BF BD) and one byte is
 * skipped. Raw passthrough of >= 0x80 bytes previously let a config field that
 * had been hand-edited in Latin-1/other non-UTF-8 (POSTed values are already
 * UTF-8) produce output that isn't valid UTF-8, which strict parsers reject
 * (Python json raises UnicodeDecodeError, browsers mangle it). Every write is
 * bounds-checked so the escape/replacement expansion can't overflow out[cap]. */
static void jesc(const char *s, char *out, size_t cap)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t o=0;
    if (!cap) return;
    while (*p){
        unsigned char c = *p;
        if (c=='"' || c=='\\'){
            if (o+2 >= cap) break;
            out[o++]='\\'; out[o++]=(char)c; p++;
        } else if (c < 0x20){
            if (o+1 >= cap) break;
            out[o++]=' '; p++;
        } else if (c < 0x80){
            if (o+1 >= cap) break;
            out[o++]=(char)c; p++;
        } else {
            /* lead byte of a multi-byte sequence: decode + validate */
            int n; unsigned cp=0;
            if      ((c & 0xE0)==0xC0){ n=2; cp=c&0x1F; }
            else if ((c & 0xF0)==0xE0){ n=3; cp=c&0x0F; }
            else if ((c & 0xF8)==0xF0){ n=4; cp=c&0x07; }
            else n=0;                       /* stray continuation / 0xF8+ */
            int ok = n>0;
            for (int i=1; ok && i<n; i++){  /* p is NUL-terminated: a short
                                             * sequence hits \0 here and fails,
                                             * never reading past the string */
                if ((p[i] & 0xC0) != 0x80) ok=0;
                else cp = (cp<<6) | (p[i]&0x3F);
            }
            if (ok){
                if      (n==2 && cp<0x80)    ok=0;   /* overlong */
                else if (n==3 && cp<0x800)   ok=0;
                else if (n==4 && cp<0x10000) ok=0;
                if (cp>=0xD800 && cp<=0xDFFF) ok=0; /* surrogate */
                if (cp>0x10FFFF)              ok=0;
            }
            if (ok){
                if (o+(size_t)n >= cap) break;
                for (int i=0;i<n;i++) out[o++]=(char)p[i];
                p += n;
            } else {
                if (o+3 >= cap) break;              /* emit U+FFFD, skip 1 byte */
                out[o++]=(char)0xEF; out[o++]=(char)0xBF; out[o++]=(char)0xBD;
                p++;
            }
        }
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
#if defined(USE_PLAY) || defined(USE_BACKCHANNEL)
    /* speaker AO volume/gain: live only when an audio-output pipeline (play
     * queue or backchannel) is compiled in - it owns the IMP_AO device. */
    "spk_volume","spk_gain",
    /* AEC: applied at the next AO open (same category as spk_*), needs the AO
     * pipeline compiled in. */
    "aec",
#endif
    /* NOTE: high_pass/agc/agc_target_dbfs/agc_compression_db/ns are NOT here:
     * they are restart-required (libimp runs them on its own record thread and
     * frees them unlocked, so a live toggle races the vendor thread -> UAF).
     * The WebUI shows them as "applies on restart" like codec/samplerate. */
    "mute",   /* live mic mute: publish gate in the HAL, works on every SoC */
};

/* Read-only day/night status object (shared /control + /events shape, see
 * control.h): "enabled" is the auto-detection flag (kept as the FIRST key:
 * the CGI bridges match "daynight":{"enabled":N), mode 0 day / 1 night,
 * brightness in %, total_gain in the IMP [24.8] linear scale (256 = 1x,
 * like GetTotalGain and the prudynt/raptor value the WebUI plots);
 * -1 = unknown. Measured by daynight.c; a stub answers unknowns when built
 * without USE_DAYNIGHT. The configured gain thresholds ride along (from
 * g_cfg) so the photosensing page can load and edit them. */
int control_daynight_json(char *buf, size_t cap, int enabled, int mode,
                          float brightness, float total_gain, float ae_luma,
                          float night_baseline, float day_trigger)
{
    /* F-03: snapshot the whole daynight section under the config string lock
     * (the daynight.c thread pattern) and format from the local copy, so this
     * GET/SSE path never races the /control writer on the numeric/string fields
     * (recursive lock, so a get-apply-get sequence that already holds it is
     * still safe). */
    ms_daynight_cfg dcfg;
    config_str_lock();
    dcfg = g_cfg.daynight;
    config_str_unlock();
    const ms_daynight_cfg *d = &dcfg;
    /* defence in depth: these runtime-computed gains are printed with %.0f,
     * which emits the literal "inf"/"nan" (invalid JSON) for a non-finite
     * value. daynight.c already rails its own computation, but guard here too
     * since these values arrive from several call sites. -1.0f = "unknown". */
    if (!isfinite(total_gain))     total_gain     = -1.0f;
    if (!isfinite(night_baseline)) night_baseline = -1.0f;
    if (!isfinite(day_trigger))    day_trigger    = -1.0f;
    const char *dnmode = d->mode==DN_MODE_TIME ? "time" :
                         d->mode==DN_MODE_SUN  ? "sun"  : "sensor";
    /* computed sunrise/sunset feedback for the SUN mode (local HH:MM) so the
     * WebUI can show today's real switch times and the user can sanity-check
     * lat/long before trusting the math. "--:--" for polar day/night. */
    char sun_sr[8]="--:--", sun_ss[8]="--:--";
    daynight_sun_status(sun_sr, sun_ss, sizeof sun_sr);
    /* time_night_start/time_day_start already come from the under-lock snapshot
     * (dcfg) above, so no extra copy/locking is needed - jesc straight from it.
     * These are config-file-only HH:MM strings (unvalidated), so route them
     * through jesc like every other string field - a hand-edited config with a
     * stray " or \ would otherwise splice raw into the JSON. Sized for jesc's
     * worst-case expansion (up to 3 bytes out per input byte). */
    char etns[sizeof d->time_night_start * 3], etds[sizeof d->time_day_start * 3];
    jesc(d->time_night_start, etns, sizeof etns);
    jesc(d->time_day_start, etds, sizeof etds);
    return snprintf(buf, cap,
        "{\"enabled\":%d,\"mode\":%d,\"brightness\":%.1f,\"total_gain\":%.0f,"
        "\"ae_luma\":%.0f,"
        "\"night_baseline\":%.0f,\"day_trigger\":%.0f,"
        "\"total_gain_day_threshold\":%g,"
        "\"total_gain_night_threshold\":%g,\"day_gain_pct\":%d,"
        "\"baseline_delay_s\":%d,"
        "\"boot_settle_s\":%d,\"boot_settle_max_s\":%d,"
        "\"boot_stable_pct\":%d,\"night_reconfirm_s\":%d,"
        "\"probe_max_skip_s\":%d,"
        "\"dn_mode\":\"%s\","
        "\"time_night_start\":\"%s\",\"time_day_start\":\"%s\","
        "\"sun_latitude\":%g,\"sun_longitude\":%g,"
        "\"sun_sunrise_offset_min\":%d,\"sun_sunset_offset_min\":%d,"
        "\"sun_computed_sunrise\":\"%s\",\"sun_computed_sunset\":\"%s\","
        "\"threshold_low\":%g,\"threshold_high\":%g,\"hysteresis\":%g,"
        "\"interval_ms\":%d,\"transition_s\":%d}",
        enabled, mode, (double)brightness, (double)total_gain, (double)ae_luma,
        (double)night_baseline, (double)day_trigger,
        (double)d->total_gain_day_threshold,
        (double)d->total_gain_night_threshold,
        d->day_gain_pct, d->baseline_delay_s,
        d->boot_settle_s, d->boot_settle_max_s,
        d->boot_stable_pct, d->night_reconfirm_s,
        d->probe_max_skip_s,
        dnmode,
        etns, etds,
        (double)d->sun_latitude, (double)d->sun_longitude,
        d->sun_sunrise_offset_min, d->sun_sunset_offset_min,
        sun_sr, sun_ss,
        (double)d->threshold_low, (double)d->threshold_high,
        (double)d->hysteresis, d->interval_ms, d->transition_s);
}

/* Read-only motion status object (shared /control + /events shape, see
 * control.h). Never persisted from here; the settable motion.* keys go
 * through the "motion" POST section. "available" is kept as the FIRST key
 * so the CGI bridges can match the object by "motion":{"available". Fields:
 *   available 0/1  build has the IMP_IVS move API (caps.motion too)
 *   enabled   0/1  detection currently running
 *   cols/rows      grid geometry in use (active[] is row-major,
 *                  index = row*cols+col, length = cols*rows)
 *   max_cells      SDK budget (= caps.motion.max_cells, convenience)
 *   sensitivity    0..255 UI value in use
 *   monitor_stream stream whose FrameSource feeds the IVS grid
 *   stalled        1 = enabled but IVS has delivered no result for a while;
 *                  a recovery cycle ran (or is running) - see imp_motion.c
 *   active         per-cell 0/1 from the latest IVS result (empty
 *                  when unavailable or not running)
 *   last_ms        ms since the last motion event, -1 = never
 *   hold_ms/skip_frames  configured values (g_cfg, not st: baked into the
 *                  IVS grid/hold logic only at the next create/resync, see
 *                  imp_motion.c) - read-only feedback for the settings page,
 *                  settable via the "motion" POST section like monitor_stream */
int control_motion_json(char *buf, size_t cap, const ms_motion_status *st)
{
    size_t o = 0;
    #define APP(...) do { \
        int _n = snprintf(o<cap?buf+o:buf, o<cap?cap-o:0, __VA_ARGS__); \
        if (_n>0) o += (size_t)_n; \
    } while (0)
    /* F-03: motion.monitor_stream is live-mutable via /control - read it under
     * the config string lock rather than lock-free. hold_ms/skip_frames ride
     * along under the same lock (also /control-mutable now). */
    config_str_lock();
    int monitor_stream = g_cfg.motion.monitor_stream;
    int hold_ms = g_cfg.motion.hold_ms;
    int skip_frames = g_cfg.motion.skip_frames;
    config_str_unlock();
    APP("{\"available\":%d,\"enabled\":%d,\"cols\":%d,"
        "\"rows\":%d,\"max_cells\":%d,\"sensitivity\":%d,"
        "\"monitor_stream\":%d,\"hold_ms\":%d,\"skip_frames\":%d,"
        "\"stalled\":%d,\"active\":[",
        st->available, st->enabled, st->cols, st->rows,
        MOTION_MAX_CELLS, st->sensitivity, monitor_stream,
        hold_ms, skip_frames,
        st->stalled);
    int mcells = st->cells;
    if (mcells > MOTION_STATUS_MAX) mcells = MOTION_STATUS_MAX;
    for (int i=0;i<mcells;i++) APP("%s%d", i?",":"", st->active[i]);
    APP("],\"last_ms\":%lld}", (long long)st->last_ms);
    #undef APP
    return (int)o;
}

int control_get_json(char *buf, size_t cap)
{
    const ms_config *c = &g_cfg;
    size_t o = 0;
    #define APP(...) do { \
        int _n = snprintf(o<cap?buf+o:buf, o<cap?cap-o:0, __VA_ARGS__); \
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
    /* osd item leaf keys /control accepts AND applies live (per-stream
     * osdS.N.* and legacy osdN.*; the master switch "osd.enabled" is
     * restart-only). Every video stream has its own independent item set,
     * dumped as "osd0"/"osd1" below; outline/outline_color are the per-item
     * text stroke. The WebUI bridge maps the prudynt OSD tree onto these.
     * B2: per-item "enabled" is deliberately NOT in this live list: an item
     * that was disabled at boot has no IMP region (imp_osd_setup only builds
     * regions for boot-enabled items), so enabling it live is a silent no-op
     * until restart - the bridge must treat enable toggles as save+restart
     * like every key outside this list. */
    APP("],\"osd\":[\"text\",\"x\",\"y\","
        "\"font_size\",\"color\",\"transparency\",\"outline\",\"outline_color\"");
    /* restart-required sections: every key under these objects is persist-
     * only (config + restart, never applied to the running pipeline). The
     * WebUI bridge reads this to flag such changes as "restart_required".
     * "osd.enabled" (the master switch) rides along explicitly: it lives in
     * the osd.* section whose other keys are live, but itself only takes
     * effect on restart (groups are built once in imp_osd_setup). */
    APP("],\"restart\":[\"video\",\"sensor\",\"osd.enabled\"],");
    /* motion capability: available = this build has the IMP_IVS move API,
     * max_cells = the SDK's compile-time IMP_IVS_MOVE_MAX_ROI_CNT (the WebUI
     * limits the grid selectors so cols*rows never exceeds it) */
    APP("\"motion\":{\"available\":%d,\"max_cells\":%d},",
        MOTION_AVAILABLE, MOTION_MAX_CELLS);
    /* privacy cover masks: B3 - "available" is no longer hardcoded 1 but
     * reflects whether an OSD group actually exists in the running pipeline
     * (imp_osd_setup builds one per stream only when OSD or a privacy region
     * was enabled at boot; sim builds never have one). Region handles are
     * pre-created with the group, so when a group exists masks really can be
     * enabled/moved live; without one a /control write would persist but
     * silently change nothing until restart - the WebUI now knows. */
    {
        int pav = 0;
#ifdef HAL_INGENIC   /* sim has no imp_osd backend (not even the stub linked) */
        for (int s = 0; s < MS_MAX_VSTREAM; s++)
            if (imp_osd_group_active(s)) pav = 1;
#endif
        APP("\"privacy\":{\"available\":%d,\"max_regions\":%d},",
            pav, MS_MAX_PRIVACY);
    }
    /* image rotation: the set of values this SoC's build can actually apply
     * (0 always; 90/270 need a real dim-swapping transpose apply path). 180 is
     * only a rotation value on T40/T41 (ROT_HAS_HW_I2D), where it is a genuine
     * per-channel hardware I2D flip; on every other SoC 180 was a redundant
     * global ISP Hflip+Vflip and was removed (use image.hflip + image.vflip
     * there). The WebUI greys out whatever isn't listed. Only reported when
     * USE_ROTATE compiled the feature in; otherwise the key is omitted (the
     * WebUI hides the control). Fragments are ordered so the array is ascending
     * ([0,90,180,270] on T40/T41, [0,90,270] where only 90/270 exist, [0]
     * where nothing does). */
#ifdef USE_ROTATE
    APP("\"rotation\":[0"
#ifdef ROT_HAS_90
        ",90"
#endif
#ifdef ROT_HAS_HW_I2D
        ",180"
#endif
#ifdef ROT_HAS_90
        ",270"
#endif
        "],");
#endif /* USE_ROTATE */
#ifdef USE_RECORD
    APP("\"record\":{\"available\":1},");
#else
    APP("\"record\":{\"available\":0},");
#endif
    /* audio backchannel: available only if the feature is compiled AND the
     * ingenic-audiodaemon client (/bin/iac) is present on the device */
#ifdef USE_BACKCHANNEL
    APP("\"backchannel\":{\"available\":%d},", bc_available());
#else
    APP("\"backchannel\":{\"available\":0},");
#endif
    /* play queue: available = the play-FIFO feature is compiled in. "sounds"
     * enumerates the .opus and .wav files under SOUNDS_DIR so the WebUI
     * test-sound control can offer them (built like every other caps.* list;
     * the play POST re-validates the chosen name against this same
     * directory). .opus only decodes when USE_PLAY_OPUS is also compiled in
     * (thingino-sounds' format choice picks one or the other per board, but
     * nothing stops a leftover file from the other format sitting on disk
     * from a prior build - list both extensions rather than assume). */
#ifdef USE_PLAY
    APP("\"play\":{\"available\":1,\"sounds\":[");
    {
        DIR *dh = opendir(SOUNDS_DIR);
        if (dh){
            struct dirent *de; int first = 1; int emitted = 0;
            while ((de = readdir(dh))){
                /* stop well before the JSON buffer can overflow - see the
                 * SOUNDS_LIST_MAX comment above */
                if (emitted >= SOUNDS_LIST_MAX) break;
                const char *nm = de->d_name;
                size_t l = strlen(nm);
                /* .wav: any RIFF/WAVE this build can decode (PCM/A-law/mu-law -
                 * a real user-dropped file, e.g. for the ESPHome media_player
                 * integration). .ulaw: thingino-sounds' own G.711 mu-law
                 * asset format (still a WAV container internally, wav_open()
                 * sniffs the RIFF header regardless of extension - the
                 * distinct extension is just so it isn't mistaken for a
                 * generic playable-anywhere .wav on disk). */
                int is_wav  = l >= 5 && !strcmp(nm+l-4,".wav");
                int is_ulaw = l >= 6 && !strcmp(nm+l-5,".ulaw");
#ifdef USE_PLAY_OPUS
                int is_opus = l >= 6 && !strcmp(nm+l-5,".opus");
#else
                int is_opus = 0;   /* can't decode it - don't offer a sound that always fails */
#endif
                if (!is_opus && !is_wav && !is_ulaw) continue;
                /* skip names that can't be spliced raw into a JSON string: a
                 * quote/backslash breaks the string, and any control byte
                 * (< 0x20 - a literal newline in a filename is legal on Linux)
                 * is invalid inside a JSON string per RFC 8259. Skipping keeps
                 * this list emitted-raw (like the fixed caps.* lists) rather
                 * than introducing an escaping pass just for filenames. */
                int bad = 0;
                for (const char *q = nm; *q; q++)
                    if (*q=='"' || *q=='\\' || (unsigned char)*q < 0x20){ bad = 1; break; }
                if (bad) continue;
                char pp[300]; struct stat st;
                snprintf(pp,sizeof pp,"%s/%s",SOUNDS_DIR,nm);
                if (stat(pp,&st)!=0 || !S_ISREG(st.st_mode)) continue;
                APP("%s\"%s\"", first?"":",", nm);
                first = 0; emitted++;
            }
            closedir(dh);
        }
    }
    APP("]},");
#else
    APP("\"play\":{\"available\":0},");
#endif
#ifdef USE_TIMELAPSE
    APP("\"timelapse\":{\"available\":1}},");
#else
    APP("\"timelapse\":{\"available\":0}},");
#endif
    /* F-03: image.* and audio.* hold live-mutable ints (running_mode,
     * anti_flicker, volume, gain, ns, mute, ...) that the /control writer
     * mutates under the config string lock - snapshot both sections here rather
     * than reading each field lock-free. (mute is _Atomic; copying the struct
     * copies its current value, consistent with the rest.) One-time GET/SSE
     * read, so a plain struct copy under the lock is the idiomatic fix. */
    ms_image_cfg img;
    ms_audio_cfg aud;
    config_str_lock();
    img = c->image;
    aud = c->audio;
    config_str_unlock();
    APP("\"image\":{\"brightness\":%d,\"contrast\":%d,\"saturation\":%d,"
        "\"sharpness\":%d,\"hue\":%d,\"hflip\":%d,\"vflip\":%d,\"running_mode\":%d,",
        img.brightness,img.contrast,img.saturation,
        img.sharpness,img.hue,img.hflip,img.vflip,
        img.running_mode);
    APP("\"anti_flicker\":%d,\"ae_compensation\":%d,\"max_again\":%d,"
        "\"max_dgain\":%d,\"sinter_strength\":%d,\"temper_strength\":%d,"
        "\"dpc_strength\":%d,\"defog_strength\":%d,\"drc_strength\":%d,"
        "\"highlight_depress\":%d,\"backlight_compensation\":%d,"
        "\"core_wb_mode\":%d,\"wb_rgain\":%d,\"wb_bgain\":%d},",
        img.anti_flicker,img.ae_compensation,img.max_again,
        img.max_dgain,img.sinter_strength,img.temper_strength,
        img.dpc_strength,img.defog_strength,img.drc_strength,
        img.highlight_depress,img.backlight_compensation,
        img.core_wb_mode,img.wb_rgain,img.wb_bgain);
    {   /* full audio state: live keys + the persist-only (restart) keys */
        char cod[16]="none";
        config_get_kv(c, "audio.codec", cod, sizeof cod);   /* restart-only spelling */
        APP("\"audio\":{\"volume\":%d,\"gain\":%d,\"alc_gain\":%d,"
            "\"high_pass\":%d,\"agc\":%d,\"agc_target_dbfs\":%d,"
            "\"agc_compression_db\":%d,\"ns\":%d,\"mute\":%d,",
            aud.volume, aud.gain, aud.alc_gain,
            aud.high_pass, aud.agc, aud.agc_target_dbfs,
            aud.agc_compression_db, aud.ns, aud.mute);
        APP("\"enabled\":%d,\"codec\":\"%s\",\"samplerate\":%d,"
            "\"channels\":%d,\"bitrate\":%d,\"force_stereo\":%d,"
            "\"spk_enabled\":%d,\"spk_volume\":%d,\"spk_gain\":%d,"
            "\"backchannel\":%d,\"backchannel_codec\":%d,\"backchannel_rate\":%d,"
            "\"aec\":%d},",
            aud.enabled, cod, aud.samplerate,
            aud.channels, aud.bitrate_kbps, aud.force_stereo,
            aud.spk_enabled, aud.spk_volume, aud.spk_gain,
            aud.backchannel, aud.backchannel_codec, aud.backchannel_rate,
            aud.aec);
    }
    {   /* sensor (all persist-only / restart-required, but POST-able) */
        char sm[136];
        /* F-03: model is runtime-mutable AND i2c_addr/fps/width/height are
         * POST-able (F-01) - snapshot the string and the numerics together. */
        int s_i2c, s_fps, s_w, s_h;
        config_str_lock();
        jesc(c->sensor.model, sm, sizeof sm);
        s_i2c = c->sensor.i2c_addr; s_fps = c->sensor.fps;
        s_w = c->sensor.width; s_h = c->sensor.height;
        config_str_unlock();
        APP("\"sensor\":{\"model\":\"%s\",\"i2c_addr\":%d,\"fps\":%d,"
            "\"width\":%d,\"height\":%d},",
            sm, s_i2c, s_fps, s_w, s_h);
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
        config_str_lock();     /* rtsp_path is runtime-mutable via POST */
        jesc(vs->rtsp_path, rp, sizeof rp);
        config_str_unlock();
#ifdef USE_ROTATE
        /* ACTUAL running dims - but only when vs (the live/persist-only
         * config, possibly POSTed-but-not-yet-restarted) still matches what
         * actually booted: hub_get_video_params() reflects any T23 SW-rotate
         * / T31 FS-rotate safe-envelope refusal for the BOOT config (see
         * hub.h and the identical fix in record.c/mp4/httpd.c/rtsp.c), so it
         * is only meaningful for the geometry that is REALLY running. A
         * rotation/width/height just POSTed but not yet restarted (this
         * whole block is persist-only, see above) describes a config the HAL
         * hasn't attempted yet - there is no refusal decision to report, so
         * fall back to the raw config-level swap preview for that case. This
         * also keeps scripts/timps-qa.sh's --test-rotation contract intact:
         * it checks that eff_width/eff_height swap for a PENDING, not-yet-
         * restarted rotation POST. */
        int ew, eh;
        const ms_vstream_cfg *bv = &g_cfg_boot.video[i];
        int is_booted = (vs->rotation==bv->rotation && vs->width==bv->width &&
                          vs->height==bv->height);
        if (!is_booted || !hub_get_video_params(i, NULL, &ew, &eh, NULL))
            ms_vstream_eff_dims(vs, &ew, &eh);   /* pending preview / hub not up yet */
        APP("%s\"%d\":{\"enabled\":%d,\"codec\":\"%s\",\"width\":%d,"
            "\"height\":%d,\"eff_width\":%d,\"eff_height\":%d,"
            "\"fps\":%d,\"bitrate\":%d,\"rc_mode\":\"%s\","
            "\"gop\":%d,\"max_gop\":%d,\"profile\":%d,\"qp\":%d,"
            "\"min_qp\":%d,\"max_qp\":%d,\"rotation\":%d,\"buffers\":%d,"
            "\"rtsp_path\":\"%s\"}",
            i?",":"", i, vs->enabled, cod, vs->width, vs->height, ew, eh, vs->fps,
            vs->bitrate_kbps, rc, vs->gop, vs->max_gop, vs->profile,
            vs->qp, vs->min_qp, vs->max_qp, vs->rotation, vs->buffers, rp);
#else
        /* rotation compiled out: omit eff_width/eff_height and the rotation
         * value is always 0 (prot() coerced it), so eff == raw dims anyway. */
        APP("%s\"%d\":{\"enabled\":%d,\"codec\":\"%s\",\"width\":%d,"
            "\"height\":%d,"
            "\"fps\":%d,\"bitrate\":%d,\"rc_mode\":\"%s\","
            "\"gop\":%d,\"max_gop\":%d,\"profile\":%d,\"qp\":%d,"
            "\"min_qp\":%d,\"max_qp\":%d,\"rotation\":%d,\"buffers\":%d,"
            "\"rtsp_path\":\"%s\"}",
            i?",":"", i, vs->enabled, cod, vs->width, vs->height, vs->fps,
            vs->bitrate_kbps, rc, vs->gop, vs->max_gop, vs->profile,
            vs->qp, vs->min_qp, vs->max_qp, vs->rotation, vs->buffers, rp);
#endif /* USE_ROTATE */
    }
    /* osd: master switch + the other osd.* globals (monitor_stream/font_path/
     * vars_file/supersample/hinting - all restart-only, same as "enabled",
     * and all POST-able via the "osd" section handler above) as their own
     * tiny object (kept directly after "video" - the CGI bridge scopes its
     * video scan up to the "osd" marker), then one independent item set per
     * video stream as "osd0"/"osd1", incl. the item type so the bridge can
     * tell text overlays from the logo */
    {
        char ofp[sizeof c->osd.font_path * 3], ovf[sizeof c->osd.vars_file * 3];
        config_str_lock();     /* osd.font_path/vars_file are runtime-mutable via POST */
        jesc(c->osd.font_path, ofp, sizeof ofp);
        jesc(c->osd.vars_file, ovf, sizeof ovf);
        config_str_unlock();
        APP("},\"osd\":{\"enabled\":%d,\"monitor_stream\":%d,\"font_path\":\"%s\","
            "\"vars_file\":\"%s\",\"supersample\":%d,\"hinting\":%d}",
            c->osd.enabled, c->osd.monitor_stream, ofp, ovf,
            c->osd.supersample, c->osd.hinting);
    }
    for (int s=0;s<MS_MAX_VSTREAM;s++){
        APP(",\"osd%d\":{", s);
        for (int i=0;i<MS_MAX_OSD;i++){
            const ms_osd_item *it=&c->osd.items[s][i];
            char t[256];
            config_str_lock();     /* osd text is runtime-mutable via POST */
            jesc(it->text, t, sizeof t);
            config_str_unlock();
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
    /* privacy cover masks per stream: privacy.<s>.<n>.{enabled,x,y,w,h,color} */
    APP(",\"privacy\":{");
    for (int s=0;s<MS_MAX_VSTREAM;s++){
        APP("%s\"%d\":{", s?",":"", s);
        for (int n=0;n<MS_MAX_PRIVACY;n++){
            const ms_privacy_region *p=&c->privacy[s][n];
            APP("%s\"%d\":{\"enabled\":%d,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
                "\"color\":\"0x%08X\"}",
                n?",":"", n, p->enabled, p->x, p->y, p->w, p->h, p->color);
        }
        APP("}");
    }
    APP("}");
    {   /* read-only day/night status (never persisted); shape/docs in
         * control_daynight_json above (shared with the /events push) */
        int dn_en = 0, dn_mode = 0;
        float dn_b = -1.0f, dn_tg = -1.0f, dn_lu = -1.0f;
        float dn_nb = -1.0f, dn_dt = -1.0f;
        daynight_get_status(&dn_en, &dn_mode, &dn_b, &dn_tg, &dn_lu,
                            &dn_nb, &dn_dt);
        APP(",\"daynight\":");
        int _dn = control_daynight_json(o<cap?buf+o:buf, o<cap?cap-o:0,
                                        dn_en, dn_mode, dn_b, dn_tg, dn_lu,
                                        dn_nb, dn_dt);
        if (_dn>0) o += (size_t)_dn;
    }
    {   /* read-only motion status; shape/docs in control_motion_json above
         * (shared with the /events push) */
        ms_motion_status mst;
        motion_get_status(&mst);
        APP(",\"motion\":");
        int _mn = control_motion_json(o<cap?buf+o:buf, o<cap?cap-o:0, &mst);
        if (_mn>0) o += (size_t)_mn;
    }
    {   /* Item-2: read-only per-channel encoder telemetry (IMP_Encoder_Query).
         * Only channels whose query succeeds are emitted - a failed/absent
         * channel (disabled stream, SW-rotate path, host sim) is OMITTED rather
         * than reported as zeros. ave_bitrate appears only on T31 once frames
         * have flowed. nemit drives the inter-channel comma. */
        APP(",\"encoder\":{");
        int nemit = 0;
        for (int i=0;i<MS_MAX_VSTREAM;i++){
            hal_enc_stat es;
            if (hal_enc_stats(c->video[i].imp_chn, &es) != 0) continue;
            APP("%s\"%d\":{\"registered\":%u,\"left_pics\":%u,"
                "\"left_stream_bytes\":%u,\"left_stream_frames\":%u,"
                "\"cur_packs\":%u,\"work_done\":%u",
                nemit?",":"", i, es.registered, es.left_pics,
                es.left_stream_bytes, es.left_stream_frames,
                es.cur_packs, es.work_done);
            if (es.ave_bitrate >= 0.0)
                APP(",\"ave_bitrate\":%.1f", es.ave_bitrate);
            APP("}");
            nemit++;
        }
        APP("}");
    }
    {   /* local recording: live status + the persisted config keys, so the
         * WebUI record page can read the current settings back (dir/name/
         * segment/roll/min_free/audio); enabled/channel/mode already mirror
         * the config via record_get_status */
        ms_record_status rst; record_get_status(&rst);
        char jf[200]; jesc(rst.file, jf, sizeof jf);
        char jd[200], jn[200];
        config_str_lock();     /* record.dir/name are runtime-mutable via POST */
        jesc(c->record.dir, jd, sizeof jd);
        jesc(c->record.name, jn, sizeof jn);
        config_str_unlock();
        APP(",\"record\":{\"available\":%d,\"enabled\":%d,\"recording\":%d,"
            "\"channel\":%d,\"mode\":%d,\"bytes\":%lld,\"free_mb\":%lld,\"file\":\"%s\","
            "\"dir\":\"%s\",\"name\":\"%s\",\"segment_s\":%d,\"pre_roll_s\":%d,"
            "\"post_roll_s\":%d,\"min_free_mb\":%d,\"audio\":%d,"
            /* Finding 1/2: distinguishes "mode=1, no motion lately" (both 1,
             * recording still legitimately 0) from "mode=1 but motion isn't
             * running" (motion_gate_enabled 0 - recording is structurally
             * inert, not just quiet) - and surfaces a manual-off latch that
             * used to have zero status visibility. */
            "\"motion_gate_available\":%d,\"motion_gate_enabled\":%d,"
            "\"manual_off\":%d}",
            rst.available, rst.enabled, rst.recording, rst.channel, rst.mode,
            (long long)rst.bytes, (long long)rst.free_mb, jf,
            jd, jn, c->record.segment_s, c->record.pre_roll_s,
            c->record.post_roll_s, c->record.min_free_mb, c->record.audio,
            rst.motion_gate_available, rst.motion_gate_enabled, rst.manual_off);
    }
    {   /* native timelapse: live status + the persisted config keys, so the
         * WebUI timelapse page can read the settings back (dir/name/channel/
         * interval_s/keep_days) */
        ms_timelapse_status tst; timelapse_get_status(&tst);
        char jf[200]; jesc(tst.file, jf, sizeof jf);
        char jd[200], jn[200];
        config_str_lock();  /* timelapse.dir/name are runtime-mutable via POST */
        jesc(c->timelapse.dir, jd, sizeof jd);
        jesc(c->timelapse.name, jn, sizeof jn);
        config_str_unlock();
        APP(",\"timelapse\":{\"available\":%d,\"enabled\":%d,\"channel\":%d,"
            "\"interval_s\":%d,\"keep_days\":%d,\"count\":%lld,\"last_t\":%lld,"
            "\"free_mb\":%lld,\"last_file\":\"%s\",\"dir\":\"%s\",\"name\":\"%s\"}",
            tst.available, tst.enabled, c->timelapse.channel,
            c->timelapse.interval_s, c->timelapse.keep_days,
            (long long)tst.count, (long long)tst.last_t,
            (long long)tst.free_mb, jf, jd, jn);
    }
    APP("}");
    #undef APP
    /* o is the length snprintf *would* have produced (it keeps counting past
     * cap). If it reached/exceeded cap the document was cut off and is NOT valid
     * JSON - signal that with a negative return so the caller can send an HTTP
     * error instead of shipping a truncated body as 200 OK. */
    if (o >= cap){ if (cap) buf[cap-1]=0; return -1; }   /* truncated */
    return (int)o;
}
#endif /* USE_CONTROL */
