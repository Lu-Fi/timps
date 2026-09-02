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
#include "motion_caps.h"
#include "rtsp/backchannel.h"
#include "rtsp/speaker.h"
#include "rotate_caps.h"   /* ROT_HAS_90/ROT_HAS_HW_I2D (rotation caps + eff dims) */
#include "enc_caps.h"      /* ENC_LIVE_KEYS: videoN keys applied to the live encoder */
#include "hal/imp_motion.h"
#include "hal/imp_osd.h"
#include "hal/hal.h"       /* hal_enc_stats: read-only encoder telemetry */
#include "util.h"          /* RTSP_MAX_CLIENTS/HTTP_MAX_CLIENTS/EVENTS_MAX_CLIENTS_DEF */
#include "record.h"
#include "timelapse.h"
#include "srt.h"           /* ms_srt_stats for the srt.* status block */
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

/* Same compile-time constant main.c uses for `timpsd -v` and its startup log
 * line - passed in via -DMS_VERSION on the whole build's command line
 * (Makefile), so it is already defined here too on a normal build. The
 * fallback mirrors main.c's, only for a standalone/tooling compile of this
 * file without that flag. */
#ifndef MS_VERSION
#define MS_VERSION "0.1.0"
#endif

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
    /* strchr rejects any '/', so the only value left that could traverse is a
     * bare ".." - checked via THE shared component-semantics check
     * (ms_has_dotdot, util.h) instead of a divergent strstr, so every '..'
     * test in the tree means the same thing (L-2). */
    if (!name || !name[0] || strchr(name,'/') || ms_has_dotdot(name)) return 0;
    char p[300];
    if ((size_t)snprintf(p,sizeof p,"%s/%s",SOUNDS_DIR,name) >= sizeof p) return 0;
    struct stat st;
    if (stat(p,&st)!=0 || !S_ISREG(st.st_mode)) return 0;
    snprintf(out,cap,"%s",p);
    return 1;
}

/* Rendered caps.play.sounds body ("a.wav","b.opus" - no brackets), cached.
 * Building it means an opendir/readdir walk plus one stat() per candidate (up
 * to SOUNDS_LIST_MAX of them), and GET /control ran all of that on EVERY
 * request for a directory that is part of the firmware image and essentially
 * never changes at runtime. Rebuild only when SOUNDS_DIR's own mtime moves -
 * one stat() on the directory, not on its contents; a create/delete/rename in
 * there always bumps it. (Directory mtime has 1 s granularity, so a file
 * dropped in during the same second as a build can be missed until the next
 * change - a non-issue for a picker list, and the play POST re-validates the
 * chosen name against the real directory anyway.) Callers are concurrent
 * httpd worker threads, hence the mutex; it is held across the append so the
 * buffer cannot be realloc'd out from under a reader. */
static pthread_mutex_t g_sounds_lock = PTHREAD_MUTEX_INITIALIZER;
static ms_buf  g_sounds;
static time_t  g_sounds_mtime;
static int     g_sounds_valid;

/* caller holds g_sounds_lock */
static void sounds_cache_refresh(void)
{
    struct stat dst;
    time_t mt = (stat(SOUNDS_DIR,&dst)==0) ? dst.st_mtime : (time_t)0;
    if (g_sounds_valid && mt == g_sounds_mtime) return;
    g_sounds.len = 0; g_sounds.err = 0;
    g_sounds_mtime = mt; g_sounds_valid = 1;
    DIR *dh = opendir(SOUNDS_DIR);
    if (!dh) return;
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
        if (!first) ms_buf_put(&g_sounds, ",", 1);
        ms_buf_put(&g_sounds, "\"", 1);
        ms_buf_put(&g_sounds, nm, l);
        ms_buf_put(&g_sounds, "\"", 1);
        first = 0; emitted++;
    }
    closedir(dh);
    /* a partial list from an OOM would be invalid JSON (a dangling comma or
     * an unterminated string) - emit nothing rather than that, and drop the
     * valid flag so the next request retries instead of caching the gap for
     * as long as the directory happens not to change */
    if (g_sounds.err){ g_sounds.len = 0; g_sounds.err = 0; g_sounds_valid = 0; }
}
#endif

#define CTRL_MAX_CHG 48
typedef struct {
    char key[CTRL_MAX_CHG][40];  /* fits daynight.total_gain_night_threshold */
    char val[CTRL_MAX_CHG][160];
    int  n;
} ctrl_changes;

/* counters for the ctrl_result the caller gets back; see control.h */
static __thread int g_acc, g_chg, g_rej, g_nopersist;
/* the "applied" echo, built as JSON while the values are in hand. It records a
 * field whenever the CANONICAL stored value differs from what was POSTed - not
 * when the value CHANGED. The two are not the same, and the difference is
 * exactly the case the UI needs most: posting 999 to a field already clamped
 * to 255 stores nothing new (changed stays 0) but the caller still sent a value
 * it will never get, and a slider left showing 999 would be wrong. Building it
 * from the change list missed that; comparing posted vs effective does not. */
static __thread char g_echo[CTRL_ECHO_CAP];
static __thread int  g_echo_off, g_echo_full;
/* the "deferred" list: CHANGED video/sensor keys hub_control() could not
 * apply to the running pipeline (see control.h). Same builder pattern as the
 * echo; keys are table names, never user data, so no escaping is needed. */
static __thread int  g_deferred;
static __thread char g_defer[CTRL_DEFER_CAP];
static __thread int  g_defer_off, g_defer_full;

/* the "ignored" list: field names the request carried that this build does not
 * apply (see control.h). Unlike echo/defer these are USER data - a client can
 * post any name at all - so they go through ms_json_esc like the echo values,
 * never spliced raw. ign_full = 0 says the list is short (buffer full, or a
 * name too long to carry), the same contract as echo_full/defer_full. */
static __thread char g_ign[CTRL_IGN_CAP];
static __thread int  g_ign_off, g_ign_full;

static void ign_add(const char *key)
{
    if (!g_ign_full) return;                        /* already overflowed */
    char ekey[CTRL_IGN_NAME*2+8];
    ms_json_esc(key, ekey, sizeof ekey);
    int w = snprintf(g_ign + g_ign_off, sizeof g_ign - (size_t)g_ign_off,
                     "%s\"%s\"", g_ign_off ? "," : "", ekey);
    if (w < 0 || g_ign_off + w >= (int)sizeof g_ign) {
        g_ign[g_ign_off] = 0; g_ign_full = 0; return;
    }
    g_ign_off += w;
}

static void defer_add(const char *key)
{
    g_deferred++;
    if (!g_defer_full) return;                      /* already overflowed */
    int w = snprintf(g_defer + g_defer_off, sizeof g_defer - (size_t)g_defer_off,
                     "%s\"%s\"", g_defer_off ? "," : "", key);
    if (w < 0 || g_defer_off + w >= (int)sizeof g_defer) {
        g_defer[g_defer_off] = 0; g_defer_full = 0; return;
    }
    g_defer_off += w;
}

/* keys from the caps.restart sections (video/sensor) - the only ones the
 * deferred grading covers; other sections have their own live/restart
 * contracts documented above and unchanged semantics. */
static int key_is_restart_section(const char *key)
{
    return (!strncmp(key,"video",5) && key[5]>='0' &&
            key[5]<'0'+MS_MAX_VSTREAM && key[6]=='.') ||
           !strncmp(key,"sensor.",7);
}

static void echo_add(const char *key, const char *eff)
{
    if (!g_echo_full) return;                       /* already overflowed */
    /* Must go through the SAME escaper the status document uses for every
     * string: an effective value read back from g_cfg can carry a quote or a
     * backslash if timps.conf was hand-edited, and emitting it raw would hand
     * the client malformed JSON - from the very reply whose job is to tell it
     * what is true. jesc also folds invalid UTF-8, which strict parsers reject.
     * Values are capped at 160 chars and escaping at most doubles them. */
    char ekey[96], eval[336];
    ms_json_esc(key, ekey, sizeof ekey);
    ms_json_esc(eff, eval, sizeof eval);
    int w = snprintf(g_echo + g_echo_off, sizeof g_echo - (size_t)g_echo_off,
                     "%s\"%s\":\"%s\"", g_echo_off ? "," : "", ekey, eval);
    if (w < 0 || g_echo_off + w >= (int)sizeof g_echo) {
        g_echo[g_echo_off] = 0; g_echo_full = 0; return;
    }
    g_echo_off += w;
}

/* ---------- tiny range-based JSON scanning ---------- */

static const char *skip_ws(const char *p, const char *e)
{
    while (p<e && (*p==' '||*p=='\t'||*p=='\r'||*p=='\n')) p++;
    return p;
}

/* find "name" within [s,e) followed by ':'; returns pointer to the value
 * (first non-ws char after the colon) or NULL.
 *
 * The scan steps from string literal to string literal and skips over the
 * CONTENTS of every one it does not match - the same discipline find_obj()
 * uses for its brace matching, and for the same reason. A raw memcmp sweep
 * matched the pattern anywhere in the range, so a POSTed string VALUE whose
 * bytes contained  "x":  (an OSD text, say) could bind a LATER field's lookup
 * to that substring instead of to the object's real "x" member. */
static const char *find_field(const char *s, const char *e, const char *name)
{
    char pat[40];
    int pl = snprintf(pat, sizeof pat, "\"%s\"", name);
    if (pl<=0 || pl>=(int)sizeof pat) return NULL;
    for (const char *p=s; p<e; p++){
        if (*p != '"') continue;
        if (e-p >= pl && !memcmp(p, pat, pl)){
            const char *q = skip_ws(p+pl, e);
            if (q<e && *q==':') return skip_ws(q+1, e);
            /* "name" not followed by ':' (e.g. a string value): keep looking */
        }
        for (p++; p<e && *p!='"'; p++)          /* past this literal's end */
            if (*p=='\\' && p+1<e) p++;
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
        /* Control characters stay folded: a newline would inject an extra line
         * into the flat key = value config file, and no amount of quoting helps
         * against that. Quote and backslash used to be REPLACED here, which was
         * itself silent data loss on every write. Both reasons for it are gone:
         * config.c's writer now quotes what needs quoting and the round trip is
         * proven, and all three JSON surfaces (GET /control, the POST reply's
         * "applied" echo, the /events stream) escape through ms_json_esc. */
        if (ch < 0x20) ch=' ';
        out[o++]=(char)ch;
    }
    out[o]=0;
}

static void timps_apply_setting(ctrl_changes *ch, const char *key, const char *raw)
{
    /* null/undefined stay refused, and NOT as a formality: some WebUI clients
     * poll settings and send a null for a field they do not know, so giving it
     * a meaning would turn a client bug into silent data loss on a field nobody
     * meant to touch. It cannot express intent, because it also arrives by
     * accident.
     *
     * The EMPTY STRING can. Nobody sends "" by accident - it only appears when
     * someone clears a text field on purpose, which until now did nothing at
     * all: the UI showed empty, the camera kept the old text. So "" is accepted
     * for string fields and means "clear this". For every other type it stays
     * refused, because there it is not an intent but an accident waiting to
     * happen - pint("") is 0, and an empty value on a numeric field would zero
     * a setting rather than clear it. */
    if (!raw || !strcmp(raw,"null") || !strcmp(raw,"undefined")){
        LOGD(MOD,"ignoring %s = '%s' (not a valid value)", key, raw?raw:"");
        g_rej++;
        return;
    }
    if (!raw[0] && !config_key_is_str(key)){
        LOGD(MOD,"ignoring empty %s (only string fields can be cleared)", key);
        g_rej++;
        return;
    }
    g_acc++;   /* recognised and about to be applied, changed or not */
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
    /* posted != effective -> the caller needs to know, changed or not */
    if (strcmp(out, val) != 0) echo_add(key, out);
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

    int live = hub_control(key, out);    /* live via the HAL (1) or persist-only (0) */
    if (!live && key_is_restart_section(key)) defer_add(key);
    /* echo to every other /events subscriber ("config" SSE event) so other
     * open WebUI tabs/clients reflect this change instead of only seeing it
     * on next poll. motion- and daynight-prefixed keys additionally still
     * drive their own richer status events from imp_motion.c/daynight.c -
     * this is just the raw settings echo, for everything else too. */
    events_config_push(key, out);
    g_chg++;
    if (ch->n < CTRL_MAX_CHG){
        snprintf(ch->key[ch->n], sizeof ch->key[0], "%s", key);
        snprintf(ch->val[ch->n], sizeof ch->val[0], "%s", out);
        ch->n++;
    } else {
        g_nopersist++;
        LOGW(MOD,"too many settings in one request, %s not persisted", key);
    }
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

/* The other direction: which names did the request carry that the walk above
 * did NOT consume? apply_ctrl_fields() looks each TABLE name up in the body, so
 * it is structurally blind to a name that is not in the table - which is why a
 * body mixing one good key with one typo answered 200 accepted:1 and dropped
 * the typo without a word. Only a request whose fields are ALL unknown was ever
 * visible (422 unknown_fields), i.e. the case a client is least likely to hit.
 *
 * So walk the body instead of the table, and report every member of THIS
 * object apply_ctrl_fields() would not have taken. The test is exactly dual to
 * its accept test (name in tbl[] AND F_CTRL), never a second hand-written name
 * list, so the two cannot drift: a field that stops being applied starts being
 * reported in the same edit. `extra` names the keys a section consumes OUTSIDE
 * the table (daynight.mode's token validation, the record/speaker/daynight
 * commands), space separated - without it those would be reported as ignored
 * while in fact being applied.
 *
 * Deliberately narrow, so that what it does say is always true:
 *   - members whose value is an OBJECT or ARRAY are skipped. get_val() refuses
 *     those too, so an object is never a settable field - it is a subsection
 *     (video's "0"/"1", an osd item) scanned through its own call, or one this
 *     build does not know, and naming a whole section as an ignored FIELD would
 *     be a category error.
 *   - only this object's own level is walked; nested objects are scanned by
 *     their own calls, and an out-of-range index ({"video":{"9":{...}}}) has no
 *     call and stays unreported.
 *   - the top-level legacy flat form is not scanned at all: there every
 *     unknown scalar would be reported under an "image." prefix it does not
 *     have. */
static void ign_note(const char *prefix, const char *nb, const char *ne,
                     const cfg_field *tbl, int n, const char *extra)
{
    size_t len = (size_t)(ne - nb);
    if (len == 0) return;
    if (len >= CTRL_IGN_NAME){    /* cannot be a field name, and cannot be
                                   * carried verbatim: say the list is short
                                   * rather than report a truncated name */
        g_ign_full = 0;
        return;
    }
    char name[CTRL_IGN_NAME];
    memcpy(name, nb, len); name[len] = 0;
    for (int i=0;i<n;i++)
        if ((tbl[i].flags & F_CTRL) && !strcmp(name, tbl[i].name)) return;
    for (const char *x = extra; x && *x; ){
        while (*x==' ') x++;
        size_t xl = strcspn(x, " ");
        if (xl == len && !memcmp(x, name, len)) return;
        x += xl;
    }
    char full[CTRL_IGN_NAME+40];
    snprintf(full, sizeof full, "%s.%s", prefix, name);
    ign_add(full);
}

static void ign_scan(const char *prefix, const char *s, const char *e,
                     const cfg_field *tbl, int n, const char *extra)
{
    int depth = 0;
    for (const char *p=s; p<e; p++){
        if (*p=='{' || *p=='[') { depth++; continue; }
        if (*p=='}' || *p==']') { if (depth>0) depth--; continue; }
        if (*p!='"') continue;
        const char *q = p+1;                     /* find the closing quote */
        while (q<e && *q!='"'){ if (*q=='\\' && q+1<e) q++; q++; }
        if (q>=e) return;                        /* unterminated - give up */
        const char *c = skip_ws(q+1, e);
        if (c<e && *c==':'){                     /* a NAME, not a string value */
            const char *v = skip_ws(c+1, e);
            if (depth==0 && v<e && *v!='{' && *v!='[')
                ign_note(prefix, p+1, q, tbl, n, extra);
        }
        p = q;
    }
}

int control_apply_json(const char *json, ctrl_result *res)
{
    if (res) { res->accepted = res->changed = res->rejected = 0; res->not_persisted = 0;
               res->deferred = 0; res->echo[0] = 0; res->echo_full = 1;
               res->defer[0] = 0; res->defer_full = 1;
               res->ign[0] = 0; res->ign_full = 1; }
    if (!json || !json[0]) return -1;
    /* Not a JSON object at all - the hand-rolled scanner would simply find
     * nothing and the old code answered 200 to it. Say so instead. */
    if (!strchr(json, '{')) return -1;
    g_acc = g_chg = g_rej = g_nopersist = 0;
    g_echo[0] = 0; g_echo_off = 0; g_echo_full = 1;
    g_deferred = 0; g_defer[0] = 0; g_defer_off = 0; g_defer_full = 1;
    g_ign[0] = 0; g_ign_off = 0; g_ign_full = 1;
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
    /* -2, not -1: -1 is the caller's "the body was not a JSON object", which
     * httpd.c now turns into a 400 that TELLS the client its request was
     * malformed. An allocation failure here is neither the client's fault nor
     * its problem to fix, and blaming it would send someone auditing a request
     * that was fine. Graded as 503 instead - the honest answer, and the same
     * one every other OOM path in httpd.c gives. */
    if (!ch) { LOGW(MOD,"control_apply_json: OOM"); pthread_mutex_unlock(&apply_mu); return -2; }
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
    /* only the nested form gets the unknown-field scan: in the legacy flat
     * form the "body" is the whole document, whose other members are sections,
     * not image fields (see ign_scan). */
    if (sb) ign_scan("image", sb, se, img_tbl, nimg, NULL);
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
        ign_scan("audio", sb, se, aud_tbl, naud, NULL);
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
        /* no field table at all here: play/stop are commands, and anything
         * else in this object is a name this build does not know. */
        ign_scan("speaker", sb, se, NULL, 0, "play stop");
    }
#endif

    {   /* general: only F_CTRL fields are reachable, which today means
         * debug_modules alone - loglevel stays file-only. Live matters here:
         * turning debugging on by restarting destroys the state one is trying
         * to observe. */
        const char *ge, *gb = find_obj(json, end, "general", &ge);
        if (gb) {
            int ngen; const cfg_field *gen_tbl = cfg_fields_general(&ngen);
            apply_ctrl_fields(ch, "general", gb, ge, gen_tbl, ngen);
            ign_scan("general", gb, ge, gen_tbl, ngen, NULL);
        }
    }

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
            /* the pre-2026-08-17 tokens stay accepted here for the same
             * reason the config parser accepts them: WebUI builds in the
             * field still POST them (see the T_DNMODE case in config.c). */
            if (!strcmp(v,"auto")||!strcmp(v,"schedule")||
                !strcmp(v,"sensor")||!strcmp(v,"time")||!strcmp(v,"sun"))
                timps_apply_setting(ch, "daynight.mode", v);
            else
                LOGW(MOD,"ignoring daynight.mode = '%s' (not auto/schedule)", v);
        }
        int ndn; const cfg_field *dn_tbl = cfg_fields_daynight(&ndn);
        {   /* {"daynight":{"probe":1}} -> run a silent IR probe on the next
             * tick. A COMMAND, not a setting, so it is counted here the same
             * way record.clip is - otherwise the grading would answer 422 to a
             * probe that was actually armed.
             *
             * -1 means this camera has no daynight.irprobe_cmd, i.e. it cannot
             * probe silently at all. That is a refusal the caller needs to see:
             * without it, "nothing happened" is indistinguishable from "the
             * probe ran and found nothing", and the operator goes looking for a
             * fault in the automaton instead of a missing config key. */
            if (get_val(sb, se, "probe", v, sizeof v) && atoi(v) != 0) {
                if (daynight_request_probe() == 0) g_acc++;
                else                               g_rej++;
            }
        }
        apply_ctrl_fields(ch, "daynight", sb, se, dn_tbl, ndn);
        /* "mode" is in the table but F_CTRL-less (validated by the token check
         * above, not the generic walker); "probe" is a command. */
        ign_scan("daynight", sb, se, dn_tbl, ndn, "mode probe");
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
        /* osd.* globals (enabled/monitor_stream/font_path/vars_file/
         * supersample/hinting): all restart-required, all F_CTRL in
         * osd_fields (config.c) - one generic walk covers what used to be
         * the hand-written "enabled" special-case plus OSD_GLOBAL_KEYS[].
         *
         * Walk the TOP LEVEL only, in segments between the nested item
         * objects. Descending would be wrong - osd_item has its own
         * "enabled", so an item's value would be read as the global - but
         * stopping at the first '{', as this did, silently dropped every
         * global that happened to come after an item. JSON object order
         * carries no meaning, so {"osd":{"0":{..},"enabled":false}} was
         * accepted with 200 and the "enabled" thrown away. Our own WebUI
         * emits the globals first; nobody else has to. */
        int nosd; const cfg_field *osd_tbl = cfg_fields_osd(&nosd);
        for (const char *seg = sb; seg < se; ) {
            const char *q = seg;
            while (q < se && *q != '{') {
                if (*q == '"') { for (q++; q < se && *q != '"'; q++) if (*q=='\\' && q+1<se) q++; }
                q++;
            }
            apply_ctrl_fields(ch, "osd", seg, q, osd_tbl, nosd);
            ign_scan("osd", seg, q, osd_tbl, nosd, NULL);
            if (q >= se) break;
            const char *nend = NULL;
            int d = 0;
            for (const char *r = q; r < se; r++) {
                if (*r == '"') { for (r++; r < se && *r != '"'; r++) if (*r=='\\' && r+1<se) r++; continue; }
                if (*r == '{') d++;
                else if (*r == '}' && --d == 0) { nend = r; break; }
            }
            if (!nend) break;                    /* unbalanced - stop here */
            seg = nend + 1;
        }
        int nitem; const cfg_field *item_tbl = cfg_fields_osd_item(&nitem);
        for (int i=0;i<MS_MAX_OSD;i++){
            char idx[4]; snprintf(idx,sizeof idx,"%d",i);
            const char *ie, *ib = find_obj(sb, se, idx, &ie);
            if (!ib) continue;
            char pre[8]; snprintf(pre,sizeof pre,"osd%d",i);
            apply_ctrl_fields(ch, pre, ib, ie, item_tbl, nitem);
            ign_scan(pre, ib, ie, item_tbl, nitem, NULL);
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
            ign_scan(pre, ib, ie, item_tbl, nitem, NULL);
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
            ign_scan(pre, ib, ie, vid_tbl, nvid, NULL);
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
                ign_scan(pre, nb, ne, priv_tbl, nprivf, NULL);
            }
        }
    }

    /* sensor: {"sensor":{"model":"gc2053","fps":25,...}} -> sensor.*
     * (persist-only, applied at the next ISP init) */
    sb = find_obj(json, end, "sensor", &se);
    if (sb){
        int nsen; const cfg_field *sen_tbl = cfg_fields_sensor(&nsen);
        apply_ctrl_fields(ch, "sensor", sb, se, sen_tbl, nsen);
        ign_scan("sensor", sb, se, sen_tbl, nsen, NULL);
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
        ign_scan("motion", sb, se, mot_tbl, nmot, NULL);
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
                /* A COMMAND, not a setting: it never goes through
                 * timps_apply_setting, so without this it left accepted at 0
                 * and the new grading answered 422 to a clip request that had
                 * actually been taken - which is exactly the false alarm the
                 * grading exists to prevent. Count it, and let its verdict
                 * (-1 = bad path or no recorder) show up as rejected, so a
                 * caller learns the request was refused without needing SSH. */
                if (record_clip(clip, secs) == 0) g_acc++;
                else                              g_rej++;
            }
        }
        int nrec; const cfg_field *rec_tbl = cfg_fields_record(&nrec);
        apply_ctrl_fields(ch, "record", sb, se, rec_tbl, nrec);
        /* active/clip/seconds are commands handled above, not table fields. */
        ign_scan("record", sb, se, rec_tbl, nrec, "active clip seconds");
    }

    /* timelapse: {"timelapse":{"enabled":..,"channel":..,"dir":..,"name":..,
     * "interval_s":..,"keep_days":..}} -> timelapse.*. All persist; the
     * running timelapse thread reads them live (no restart). */
    sb = find_obj(json, end, "timelapse", &se);
    if (sb){
        int ntl; const cfg_field *tl_tbl = cfg_fields_timelapse(&ntl);
        apply_ctrl_fields(ch, "timelapse", sb, se, tl_tbl, ntl);
        ign_scan("timelapse", sb, se, tl_tbl, ntl, NULL);
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
    if (res) {
        res->accepted = g_acc; res->changed = g_chg; res->rejected = g_rej;
        res->not_persisted = g_nopersist;
        res->deferred = g_deferred;
        snprintf(res->echo, sizeof res->echo, "%s", g_echo);
        res->echo_full = g_echo_full;
        snprintf(res->defer, sizeof res->defer, "%s", g_defer);
        res->defer_full = g_defer_full;
        snprintf(res->ign, sizeof res->ign, "%s", g_ign);
        res->ign_full = g_ign_full;
    }
    free(ch);
    pthread_mutex_unlock(&apply_mu);
    return 0;
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
/* moved to util.c as ms_json_esc(): the SSE emitter in mp4/httpd.c needs the
 * SAME escaper, and a second copy would be a second thing to get wrong. Kept as
 * a one-line forwarder so the 17 call sites below read unchanged. */
static void jesc(const char *s, char *out, size_t cap)
{
    ms_json_esc(s, out, cap);
}

/* GET /control's "caps":{"image":[...],"audio":[...]} advertisement used to
 * be two hand-written arrays here (IMG_CAPS/AUD_CAPS), re-listing image./
 * audio key names under a SECOND copy of isp_caps.h's/audio_caps.h's
 * #ifdefs. Both arrays are gone now: control_get_json() below walks
 * config.c's image_fields[]/audio_fields[] tables directly (via
 * cfg_fields_image()/cfg_fields_audio()) and emits every entry that carries
 * BOTH F_CTRL and F_CAP - see F_CAP's doc comment in config.h for exactly
 * which fields that is and why (image: pure ISP_HAS_* hardware gate; audio:
 * hardware/feature gate PLUS a deliberate live-vs-restart curation). */

/* Read-only day/night status object (shared /control + /events shape, see
 * control.h): "enabled" is the auto-detection flag (kept as the FIRST key:
 * the CGI bridges match "daynight":{"enabled":N), mode 0 day / 1 night,
 * brightness in %, total_gain in the IMP [24.8] linear scale (256 = 1x,
 * like GetTotalGain and the prudynt/raptor value the WebUI plots);
 * -1 = unknown. Measured by daynight.c; a stub answers unknowns when built
 * without USE_DAYNIGHT. The configured thresholds ride along (from g_cfg) so
 * the photosensing page can load and edit them.
 *
 * "exposure" (2026-08-17) is the value the decision actually runs on -
 * total_gain scaled by the AE's integration-time ratio, so it equals
 * total_gain in a dark scene and drops far below it in a bright one, where
 * gain alone rails at its floor. Plot that one if you are diagnosing a
 * day/night problem; total_gain stays for continuity with existing pages.
 *
 * "night_baseline"/"day_trigger" keep their key names for the same
 * continuity reason but now carry the redesign's two night-side numbers: the
 * PROVEN night reference and the level the exposure must fall below to ask
 * for a probe. Both are -1 outside night. The old keys for retired config
 * (day_gain_pct, night_reconfirm_s, ...) are gone - see
 * dev_notes/DAYNIGHT_REDESIGN_2026-08-17.md section 7.3. */
int control_daynight_json(char *buf, size_t cap, int enabled, int mode,
                          float brightness, float total_gain, float exposure,
                          float ae_luma, float night_ref, float probe_bar,
                          int isp_desync)
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
    if (!isfinite(total_gain)) total_gain = -1.0f;
    if (!isfinite(exposure))   exposure   = -1.0f;
    if (!isfinite(night_ref))  night_ref  = -1.0f;
    if (!isfinite(probe_bar))  probe_bar  = -1.0f;
    const char *dnmode = d->mode==DN_MODE_SCHEDULE ? "schedule" : "auto";
    /* computed sunrise/sunset feedback for the sun calendar (local HH:MM) so the
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
        "\"exposure\":%.0f,\"ae_luma\":%.0f,"
        "\"night_baseline\":%.0f,\"day_trigger\":%.0f,"
        /* standing decided-mode/ISP-readback disagreement, debounced;
         * -1 unknown, 0 in sync, 1 standing (see daynight.h) */
        "\"isp_desync\":%d,"
        /* the two thresholds under BOTH names: the new one, and the
         * pre-2026-08-17 one an existing photosensing page still binds to
         * (they are the same config field via the alias). */
        "\"day_gain\":%g,\"night_gain\":%g,"
        "\"total_gain_day_threshold\":%g,\"total_gain_night_threshold\":%g,"
        "\"day_confirm_s\":%d,"
        "\"probe_min_gap_s\":%d,\"probe_jump_pct\":%d,"
        "\"probe_confirm_s\":%d,\"probe_settle_s\":%d,\"ref_delay_s\":%d,"
        "\"heartbeat_s\":%d,\"heartbeat_max_s\":%d,"
        /* the silent IR probe's verdict thresholds. Kept in the status for
         * diagnostics even though the 2026-08-22 consolidation turned all
         * eight of these (probe_jump_pct..transition_s below) from F_CTRL
         * config into fixed DN_* constants (daynight.h) - a camera's
         * effective values are still worth being able to read, they are just
         * no longer settable per camera, so they come from the constants
         * rather than from *d now. */
        "\"ir_ratio_night\":%g,\"ir_ratio_day\":%g,\"ir_min_headroom\":%d,"
        "\"boot_probe\":%d,\"boot_settle_s\":%d,"
        "\"dn_mode\":\"%s\","
        "\"time_night_start\":\"%s\",\"time_day_start\":\"%s\","
        "\"sun_latitude\":%g,\"sun_longitude\":%g,"
        "\"sun_sunrise_offset_min\":%d,\"sun_sunset_offset_min\":%d,"
        "\"sun_computed_sunrise\":\"%s\",\"sun_computed_sunset\":\"%s\","
        "\"interval_ms\":%d,\"transition_s\":%d}",
        enabled, mode, (double)brightness, (double)total_gain,
        (double)exposure, (double)ae_luma,
        (double)night_ref, (double)probe_bar,
        isp_desync,
        (double)d->day_gain, (double)d->night_gain,
        (double)d->day_gain, (double)d->night_gain,
        d->day_confirm_s,
        d->probe_min_gap_s, DN_PROBE_JUMP_PCT,
        d->probe_confirm_s, DN_PROBE_SETTLE_S, DN_REF_DELAY_S,
        d->heartbeat_s, d->heartbeat_max_s,
        (double)DN_IR_RATIO_NIGHT, (double)DN_IR_RATIO_DAY, DN_IR_MIN_HEADROOM,
        d->boot_probe, DN_BOOT_SETTLE_S,
        dnmode,
        etns, etds,
        (double)d->sun_latitude, (double)d->sun_longitude,
        d->sun_sunrise_offset_min, d->sun_sunset_offset_min,
        sun_sr, sun_ss,
        d->interval_ms, DN_TRANSITION_S);
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
    /* Build identity (2026-08 fleet incident: fw_ota.sh's flash script logged
     * "Firmware flashed successfully" on multiple cameras whose /usr/bin/
     * timpsd binary demonstrably had NOT changed post-reboot - the flash
     * script's own success signal only proves a reboot was triggered, not
     * that the new binary is what came back up. MS_VERSION is git describe's
     * tag+commit+dirty-flag string, already compiled in and used for
     * `timpsd -v`/the startup log line (main.c) - this just exposes the same
     * compile-time constant here too, so a one-line `curl .../control | jget
     * version` (or scripts/timps-qa.sh's new check) catches exactly this
     * class of "reboot happened, binary didn't" drift without needing an SSH
     * MD5 comparison every time. "version" is a plain top-level key, first in
     * the document (ahead of "caps" - see the note on caps below) since it
     * is fixed, always-present metadata, not part of any capability list a
     * CGI bridge scans into. */
    APP("{\"version\":\"%s\",", MS_VERSION);
    /* caps FIRST (of the REST of the document): the CGI bridges scan for the
     * *last* occurrence of a key, which must be the value in the image
     * object below, not the caps name */
    APP("\"caps\":{\"image\":[");
    {
        int nf; const cfg_field *tbl = cfg_fields_image(&nf);
        int first = 1;
        for (int i=0;i<nf;i++)
            if ((tbl[i].flags & (F_CTRL|F_CAP)) == (F_CTRL|F_CAP)) {
                APP("%s\"%s\"", first?"":",", tbl[i].name);
                first = 0;
            }
    }
    APP("],\"audio\":[");
    {
        int nf; const cfg_field *tbl = cfg_fields_audio(&nf);
        int first = 1;
        for (int i=0;i<nf;i++)
            if ((tbl[i].flags & (F_CTRL|F_CAP)) == (F_CTRL|F_CAP)) {
                APP("%s\"%s\"", first?"":",", tbl[i].name);
                first = 0;
            }
    }
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
    /* videoN.* keys THIS build can apply to the running encoder (enc_caps.h)
     * - the per-key exception to the conservative "video" entry above. A
     * listed key can still fall back to restart at runtime (channel down,
     * classic H265); the POST reply's "deferred" grading is the per-request
     * truth, this list is what the UI may offer as live. Empty on the sim
     * and on builds without a HAL control path. */
    APP("\"video_live\":[");
#ifdef ENC_LIVE_KEYS
    {
        static const char *const lk[] = { ENC_LIVE_KEYS };
        for (size_t li=0; li<sizeof lk/sizeof lk[0]; li++)
            APP("%s\"%s\"", li?",":"", lk[li]);
    }
#endif
    APP("],");
    /* Concurrent-client ceilings. These are REFUSAL points - RTSP answers 453
     * and drops, HTTP and /events answer 503 "busy" - and until now a client
     * had no way at all to learn them short of opening connections until one
     * failed, which on a live camera means deliberately DoSing the thing you
     * are monitoring. Everything else a client needs here is either fixed by
     * the protocol or already inferable from this same document; these three
     * are not inferable from anything.
     *
     * rtsp/http are compile-time bounds (util.h, -D overridable per board -
     * the low-RAM boards in this fleet build with -DRTSP_MAX_CLIENTS=4), so
     * they genuinely differ between two cameras running the same version
     * string. events.max_clients is a config key instead, so it is read from
     * the live config with the same <=0 fallback httpd.c enforces.
     *
     * Deliberately NOT dumped here: MS_MAX_VSTREAM / MS_MAX_OSD / SRT's client
     * cap. The first two are already discoverable - this document dumps one
     * object per stream and per OSD item, so a client counts them - and the
     * SRT one belongs with the rest of srt.* below if it is ever needed. */
    APP("\"rtsp_max_clients\":%d,\"http_max_clients\":%d,"
        "\"events_max_clients\":%d,",
        RTSP_MAX_CLIENTS, HTTP_MAX_CLIENTS,
        c->events_max_clients > 0 ? c->events_max_clients
                                  : EVENTS_MAX_CLIENTS_DEF);
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
    /* audio backchannel: available only if the feature is compiled AND
     * audio.backchannel was enabled at boot (bc_available() - see there,
     * it's restart-only, not the live config value).
     *
     * "talk_ws" is the browser-microphone WebSocket transport for that same
     * backchannel (USE_BC_WS, endpoint /talk). It is a SECOND flag rather
     * than a second caps entry because it is not a separate feature - it is
     * another way into this one, and a camera can perfectly well have the
     * RTSP backchannel without it.
     *
     * Like "available" it folds compile-time and boot-time state into one
     * number, so the WebUI can hide its talk button on a single test instead
     * of guessing. It reports the EFFECTIVE mode, not the raw config value:
     *   0  httpd would not serve /talk at all right now
     *   1  served, TLS required   (audio.talk_ws=1 and this port is TLS)
     *   2  served, TLS optional   (audio.talk_ws=2; plain ws:// accepted)
     * audio.talk_ws=1 on a plaintext port therefore reports 0, because /talk
     * would 426 every request - the WebUI must not offer a button for it.
     * Nothing here says which scheme to dial: 1 always means wss://, and for
     * 2 the WebUI takes the scheme from /x/timps-token.cgi's "tls" field, the
     * same one it already uses for the media/control URLs. */
#ifdef USE_BACKCHANNEL
#ifdef USE_BC_WS
    {
        int tws = c->audio.talk_ws;
        if (tws < 0 || tws > 2 || !bc_available()) tws = 0;
#ifdef USE_TLS
        if (tws == 1 && !c->http_https) tws = 0;   /* strict mode, no TLS */
#else
        if (tws == 1) tws = 0;                     /* strict mode, TLS-less build */
#endif
        APP("\"backchannel\":{\"available\":%d,\"talk_ws\":%d},",
            bc_available(), tws);
    }
#else
    APP("\"backchannel\":{\"available\":%d,\"talk_ws\":0},", bc_available());
#endif
#else
    APP("\"backchannel\":{\"available\":0,\"talk_ws\":0},");
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
    pthread_mutex_lock(&g_sounds_lock);
    sounds_cache_refresh();
    APP("%.*s", (int)g_sounds.len, g_sounds.data ? (const char*)g_sounds.data : "");
    pthread_mutex_unlock(&g_sounds_lock);
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
    /* video streams (persist-first; the ENC_LIVE_KEYS rc subset applies
     * live, see caps.video_live above). codec/rc_mode go through
     * config_get_kv for the canonical config-file spelling. */
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
            "\"min_qp\":%d,\"max_qp\":%d,\"quality_lvl\":%d,"
            "\"change_pos\":%d,\"i_bias_lvl\":%d,\"fluc_lvl\":%d,"
            "\"rotation\":%d,\"buffers\":%d,"
            "\"rtsp_path\":\"%s\"}",
            i?",":"", i, vs->enabled, cod, vs->width, vs->height, ew, eh, vs->fps,
            vs->bitrate_kbps, rc, vs->gop, vs->max_gop, vs->profile,
            vs->qp, vs->min_qp, vs->max_qp, vs->quality_lvl,
            vs->change_pos, vs->i_bias_lvl, vs->fluc_lvl, vs->rotation, vs->buffers, rp);
#else
        /* rotation compiled out: omit eff_width/eff_height and the rotation
         * value is always 0 (prot() coerced it), so eff == raw dims anyway. */
        APP("%s\"%d\":{\"enabled\":%d,\"codec\":\"%s\",\"width\":%d,"
            "\"height\":%d,"
            "\"fps\":%d,\"bitrate\":%d,\"rc_mode\":\"%s\","
            "\"gop\":%d,\"max_gop\":%d,\"profile\":%d,\"qp\":%d,"
            "\"min_qp\":%d,\"max_qp\":%d,\"quality_lvl\":%d,"
            "\"change_pos\":%d,\"i_bias_lvl\":%d,\"fluc_lvl\":%d,"
            "\"rotation\":%d,\"buffers\":%d,"
            "\"rtsp_path\":\"%s\"}",
            i?",":"", i, vs->enabled, cod, vs->width, vs->height, vs->fps,
            vs->bitrate_kbps, rc, vs->gop, vs->max_gop, vs->profile,
            vs->qp, vs->min_qp, vs->max_qp, vs->quality_lvl,
            vs->change_pos, vs->i_bias_lvl, vs->fluc_lvl, vs->rotation, vs->buffers, rp);
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
        int dn_en = 0, dn_mode = 0, dn_ds = -1;
        float dn_b = -1.0f, dn_tg = -1.0f, dn_ex = -1.0f, dn_lu = -1.0f;
        float dn_rf = -1.0f, dn_pb = -1.0f;
        daynight_get_status(&dn_en, &dn_mode, &dn_b, &dn_tg, &dn_ex, &dn_lu,
                            &dn_rf, &dn_pb, &dn_ds);
        APP(",\"daynight\":");
        int _dn = control_daynight_json(o<cap?buf+o:buf, o<cap?cap-o:0,
                                        dn_en, dn_mode, dn_b, dn_tg, dn_ex,
                                        dn_lu, dn_rf, dn_pb, dn_ds);
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
            /* au_drops: cumulative producer-side frame drops (oversized AU /
             * pool OOM in the HAL encode thread). The log throttles these to
             * every 20th event; this is the only exact count. Distinct from
             * queue_drops below, which counts consumer-queue evictions. */
            APP("%s\"%d\":{\"registered\":%u,\"left_pics\":%u,"
                "\"left_stream_bytes\":%u,\"left_stream_frames\":%u,"
                "\"cur_packs\":%u,\"work_done\":%u,\"au_drops\":%u",
                nemit?",":"", i, es.registered, es.left_pics,
                es.left_stream_bytes, es.left_stream_frames,
                es.cur_packs, es.work_done, es.au_drops);
            if (es.ave_bitrate >= 0.0)
                APP(",\"ave_bitrate\":%.1f", es.ave_bitrate);
            {   /* "rc": what the encoder ACTUALLY holds right now
                 * (IMP_Encoder_GetChnAttrRcMode readback, hal.h) -
                 * deliberately separate from the CONFIGURED videoN.* block
                 * above, so written and held values can be compared. Keys
                 * reuse the videoN.* names where they mean the same thing;
                 * fields the current mode/API does not carry are omitted.
                 * On the new-API SoCs, bitrate/max_bitrate are the raw SDK
                 * values (unit unverified) plus the four attrs timps never
                 * writes (ip_delta/pb_delta/rc_options/max_picture_size/
                 * max_psnr) - readable here for the first time. */
                hal_enc_rc rc;
                if (hal_enc_rc_read(c->video[i].imp_chn, &rc) == 0){
                    APP(",\"rc\":{\"rc_mode\":\"%s\"", rc.mode);
                    if (rc.bitrate       >= 0) APP(",\"bitrate\":%lld", rc.bitrate);
                    if (rc.max_bitrate   >= 0) APP(",\"max_bitrate\":%lld", rc.max_bitrate);
                    #define RCF(name,fld) \
                        if (rc.fld != HAL_RC_UNSET) APP(",\"" name "\":%d", rc.fld)
                    RCF("qp",            qp);
                    RCF("min_qp",        min_qp);
                    RCF("max_qp",        max_qp);
                    RCF("i_bias_lvl",    i_bias_lvl);
                    RCF("change_pos",    change_pos);
                    RCF("quality_lvl",   quality_lvl);
                    RCF("static_time",   static_time);
                    RCF("frm_qp_step",   frm_qp_step);
                    RCF("gop_qp_step",   gop_qp_step);
                    RCF("adaptive_mode", adaptive_mode);
                    RCF("gop_relation",  gop_relation);
                    RCF("fluc_lvl",      fluc_lvl);
                    RCF("ip_delta",      ip_delta);
                    RCF("pb_delta",      pb_delta);
                    RCF("max_psnr",      max_psnr);
                    #undef RCF
                    if (rc.rc_options       >= 0) APP(",\"rc_options\":%lld", rc.rc_options);
                    if (rc.max_picture_size >= 0) APP(",\"max_picture_size\":%lld", rc.max_picture_size);
                    APP("}");
                }
            }
            APP("}");
            nemit++;
        }
        APP("}");
    }
    {   /* cumulative fanqueue overflow-heal events per video stream, summed
         * over all consumers (RTSP/fMP4/record) - hub_note_drop(). Nonzero
         * and climbing = some client is behind and the shared encoder is
         * being asked for extra IDRs on its behalf (bitrate spikes for
         * everyone), which the log otherwise never shows. */
        APP(",\"queue_drops\":[");
        for (int i=0;i<MS_MAX_VSTREAM;i++)
            APP("%s%u", i?",":"", hub_get_drops(i));
        APP("]");
    }
    {   /* local recording: live status + the persisted config keys, so the
         * WebUI record page can read the current settings back (dir/name/
         * segment/roll/min_free/audio); enabled/channel/mode already mirror
         * the config via record_get_status */
        ms_record_status rst; record_get_status(&rst);
        char jf[200]; jesc(rst.file, jf, sizeof jf);
        char je[200]; jesc(rst.last_error, je, sizeof je);
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
            "\"manual_off\":%d,"
            /* write-path health (record.h): a full/dying SD card otherwise
             * announces itself once, in a log ring that recycles in hours */
            "\"write_errors\":%lld,\"last_error_age_s\":%lld,\"last_error\":\"%s\"}",
            rst.available, rst.enabled, rst.recording, rst.channel, rst.mode,
            (long long)rst.bytes, (long long)rst.free_mb, jf,
            jd, jn, c->record.segment_s, c->record.pre_roll_s,
            c->record.post_roll_s, c->record.min_free_mb, c->record.audio,
            rst.motion_gate_available, rst.motion_gate_enabled, rst.manual_off,
            rst.write_errors,
            rst.last_error_us ? (long long)((ms_now_us()-rst.last_error_us)/1000000) : -1LL,
            je);
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
    /* SRT output (USE_SRT builds only): a minimal read-only status block, not
     * a "caps" entry, matching the record/timelapse pattern above rather than
     * the caps.record/caps.timelapse availability-only style - srt.* is
     * persist-only config (no F_CTRL fields at all, see cfg_fields note),
     * but a test harness still needs a way to tell whether THIS camera has
     * SRT compiled in, enabled, and which port to dial without needing SSH.
     * Added alongside scripts/timps-qa.sh's new SRT coverage (section 4b),
     * which was previously zero despite srt.c/srt_fields existing - there was
     * no way to discover srt.enabled/port from the outside at all before this. */
#ifdef USE_SRT
    {   /* + the last srt.c stats tick so QA can see link health (RTT, loss,
         * retransmits) without SSH-ing for the log. -1 / stats_age_s=-1 =
         * no receiver has been connected long enough for a sample yet. */
        ms_srt_stats ss; srt_get_stats(&ss);
        APP(",\"srt\":{\"available\":1,\"enabled\":%d,\"port\":%d,\"channel\":%d,"
            "\"mode\":\"%s\",\"connected\":%d,\"stats_age_s\":%lld,"
            "\"rtt_ms\":%.1f,\"bw_mbps\":%.2f,\"rate_mbps\":%.2f,"
            "\"retrans\":%lld,\"loss\":%lld,\"drop\":%lld}",
            c->srt.enabled, c->srt.port, c->srt.channel,
            ss.caller ? "caller" : "listener", ss.connected,
            ss.t_us ? (long long)((ms_now_us() - ss.t_us) / 1000000) : -1LL,
            ss.rtt_ms, ss.bw_mbps, ss.rate_mbps,
            ss.retrans, ss.loss, ss.drop);
    }
#else
    APP(",\"srt\":{\"available\":0}");
#endif
    /* TLS (USE_TLS builds only), same shape and same reasoning as srt.* above:
     * an "available" flag that answers "was this BINARY built with the
     * feature", plus - only when it was - the runtime settings needed to
     * actually dial it.
     *
     * This is load-bearing on this fleet, not decoration. The two builds that
     * exist are not compiled alike: the thingino firmware package links
     * mbedTLS (USE_TLS=1), the standalone build.sh binary does not, and the
     * `sim` target never has. So "does this camera speak HTTPS/RTSPS" cannot
     * be answered from "version" - two cameras report the same git-describe
     * string and differ. Without this the only symptom of the mismatch was
     * rtsp.c's startup warning "RTSPS requested but built without USE_TLS"
     * going to the log, where no HTTP client ever sees it, and a connect that
     * simply never succeeds.
     *
     * The unavailable branch reports nothing but the flag - deliberately, and
     * for the same reason srt does: http.https / rtsp.tls may well be 1 in the
     * config file (that is exactly what produces the warning above), but they
     * are INERT in this binary, no listener was ever opened, and echoing them
     * would invite a client to dial a port nothing is bound to. available:0
     * means "ignore any TLS config you may have seen elsewhere". */
#ifdef USE_TLS
    APP(",\"tls\":{\"available\":1,\"https\":%d,\"rtsps\":%d,\"rtsps_port\":%d}",
        c->http_https, c->rtsp_tls, c->rtsp_tls_port);
#else
    APP(",\"tls\":{\"available\":0}");
#endif
    {   /* held last WARN/ERROR per module (log.c): the 64 KB syslog ring
         * recycles within hours, so the one line that explains a dead
         * recording / frozen stream / desynced day-night is usually gone by
         * the time anyone looks. This keeps it reachable without SSH. */
        APP(",\"last_errors\":");
        int _le = log_last_errors_json(o<cap?buf+o:buf, o<cap?cap-o:0);
        if (_le>0) o += (size_t)_le;
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

/* ---------- GET /control?fields=1: F_CTRL field-name inventory ---------- */
/* See the doc comment in control.h for the full rationale (Finding #1 of the
 * 2026-08 QA-coverage audit). This walks the exact same cfg_fields_*()
 * accessors apply_ctrl_fields() uses above - it is intentionally NOT a new
 * hand-written list of field names, since re-listing them a second time here
 * would recreate the identical drift bug this endpoint exists to catch. */
int control_fields_json(char *buf, size_t cap)
{
    size_t o = 0;
    #define APP(...) do { \
        int _n = snprintf(o<cap?buf+o:buf, o<cap?cap-o:0, __VA_ARGS__); \
        if (_n>0) o += (size_t)_n; \
    } while (0)
    #define SECFIELDS(jsonname, accessor) do { \
        int _n; const cfg_field *_t = (accessor)(&_n); \
        APP("\"" jsonname "\":["); \
        int _first = 1; \
        for (int _i=0;_i<_n;_i++) \
            if (_t[_i].flags & F_CTRL){ \
                APP("%s\"%s\"", _first?"":",", _t[_i].name); \
                _first = 0; \
            } \
        APP("],"); \
    } while (0)
    APP("{");
    SECFIELDS("image",     cfg_fields_image);
    SECFIELDS("audio",     cfg_fields_audio);
    SECFIELDS("sensor",    cfg_fields_sensor);
    SECFIELDS("osd",       cfg_fields_osd);
    SECFIELDS("osd_item",  cfg_fields_osd_item);
    SECFIELDS("motion",    cfg_fields_motion);
    SECFIELDS("record",    cfg_fields_record);
    SECFIELDS("timelapse", cfg_fields_timelapse);
    SECFIELDS("daynight",  cfg_fields_daynight);
    SECFIELDS("video",     cfg_fields_video);
    SECFIELDS("privacy",   cfg_fields_privacy);
    SECFIELDS("general",   cfg_fields_general);
    #undef SECFIELDS
    /* drop the trailing comma left by the last SECFIELDS() - only when the
     * document wasn't already truncated (o<=cap), same guard style as the
     * rest of this file's APP-based builders */
    if (o>0 && o<=cap && buf[o-1]==',') o--;
    APP("}");
    #undef APP
    if (o >= cap){ if (cap) buf[cap-1]=0; return -1; }   /* truncated */
    return (int)o;
}
#endif /* USE_CONTROL */
