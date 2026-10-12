/* daynight.c - native automatic day/night detection. The decision is
 * described in daynight.h and dev_notes/dn-redesign.md: the day pipeline
 * reports ambient light honestly and the night pipeline does not, so
 * day->night is a plain measurement and night->day is a look without the
 * illuminator (silent) confirmed by the day pipeline (one click).
 *
 * Compiled only with -DUSE_DAYNIGHT; uses nothing but libc + pthread. */
#include "daynight.h"
#include "config.h"
#include "hal/hal.h"   /* hal_isp_total_gain()/hal_isp_exposure(): ISP via IMP */
#include <stdio.h>     /* snprintf() - needed by the !USE_DAYNIGHT stub too */

#ifdef USE_DAYNIGHT
#include "hub.h"       /* consumers and the snapshot hold for Idle-Dark */
#include "events.h"    /* wake /events SSE subscribers on real changes */
#include "log.h"
#include "util.h"      /* ms_now_us(): monotonic clock for every deadline */
#include <string.h>
#include <strings.h>
#include <stdlib.h>    /* strtol(): the ISP /proc field parser */
#include <ctype.h>     /* isspace(): ditto */
#include <unistd.h>    /* F-01: vfork/execlp/dup2 instead of system() */
#include <sys/wait.h>
#include <signal.h>    /* SIGKILL: a board hook that wedges is not waited on forever */
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

#define MOD "DAYNIGHT"

enum { DN_DAY = 0, DN_NIGHT = 1, DN_UNKNOWN = -1 };

/* Idle-Dark timing and what the illuminator hook answers to "idle" */
#define DN_IR_WAKE_SETTLE_MS   3500   /* AE after "IR on": the snapshot hold */
#define DN_IR_WAKE_HOOK_MS     1000   /* budget for the "on" hook before ready_at is known */
#define DN_IR_IDLE_REASSERT_S   600   /* inside the hook's own 15 min failsafe */
enum { DNI_HOOK_OK = 0, DNI_HOOK_NONE, DNI_HOOK_OLD, DNI_HOOK_WHITE };

/* the lower of two published analog gain caps is the one the AE stops at */
static int dn_mag_min(int mag, int other)
{
    return (mag > 0 && other > 0 && other < mag) ? other : mag;
}

/* ---- tuning that is deliberately NOT config ---------------------------- *
 * Everything a camera actually needs to differ on is a config key. What is
 * left here is the filter shape, which is a property of how ISP AE loops
 * behave rather than of any particular installation. */
#ifndef DN_ALPHA
#define DN_ALPHA 0.30f           /* per-tick EMA step of the exposure smoother */
#endif
#ifndef DN_STABLE_N
#define DN_STABLE_N 4            /* consecutive on-EMA samples = AE settled */
#define DN_IR_MAX_FAILS 2        /* failed illuminator calls before retiring it */
#endif
#ifndef DN_STABLE_PCT
#define DN_STABLE_PCT 0.10f      /* ... within this fraction of the EMA */
#endif
#ifndef DN_STABLE_MAX_MS
#define DN_STABLE_MAX_MS 120000  /* give up waiting for a sensor that never settles */
#endif
/* believe a switch once the ISP shows it, after the board hook chain */
/* the board hook chain (switch_cmd -> color -> POST /control) is fire-and-
 * forget and some ISPs take the mode late or fall back: re-drive it twice */
#define DN_REASSERT_MS    8000
#define DN_REASSERT_COUNT 2
#define DN_CYCLE_HOLD_MS  5000   /* the other mode of a readback cycle, held */
#ifndef DN_VERIFY_MS
#define DN_VERIFY_MS 18000
#endif
/* lower clamp on the integration-time ratio, so a bogus 0 from the scrape
 * cannot collapse the metric to zero. */
#define DN_RATIO_MIN (1.0f / 4096.0f)
/* wall clock is only trusted from 2020 on - a camera without NTP boots into
 * 1970 and must not act on a "calendar" derived from it. */
#define DN_CLOCK_SANE 1577836800L


/* how often dn_read() re-probes the CONFIGURED isp_path while a fallback path
 * is the one actually working (~5 min at the 2 s default interval) */
#define DN_ISP_REPROBE_TICKS 150

/* ---- trace recorder ---------------------------------------------------- */
#ifndef DN_TRACE_EVERY
#define DN_TRACE_EVERY 5         /* one line per N samples (10 s at 2 s) */
#endif
#ifndef DN_TRACE_MAX_BYTES
#define DN_TRACE_MAX_BYTES (1<<20)   /* rotate once past 1 MB - tmpfs, see config.h */
#endif

static pthread_t   g_thr;
static ms_stopgate g_gate;
static int         g_started;

/* latest measurement, shared with control.c via daynight_get_status() */
static pthread_mutex_t g_st_mu = PTHREAD_MUTEX_INITIALIZER;
static float g_st_brightness = -1.0f;
static float g_st_gain       = -1.0f;
static float g_st_exposure   = -1.0f;
static float g_st_luma       = -1.0f;
static int   g_st_mode       = DN_UNKNOWN;
static float g_st_ref        = -1.0f;
static float g_st_bar        = -1.0f;
static int   g_st_desync     = -1;   /* -1 unknown, 0 in sync, 1 standing */

static void dn_status_update(float brightness, float gain, float exposure,
                             float luma, int mode, float ref, float bar,
                             int desync)
{
    /* last values that woke /events (only touched by the sampling thread) */
    static float nfy_b = -1000.0f, nfy_g = -1000.0f;
    static int   nfy_m = -1000, nfy_d = -1000;

    pthread_mutex_lock(&g_st_mu);
    g_st_brightness = brightness;
    g_st_gain       = gain;
    g_st_exposure   = exposure;
    g_st_luma       = luma;
    g_st_mode       = mode;
    g_st_ref        = ref;
    g_st_bar        = bar;
    g_st_desync     = desync;
    pthread_mutex_unlock(&g_st_mu);

    /* wake /events subscribers only on a REAL change - the readings jitter
     * every sample, so require a mode flip, a standing-desync edge, >= 1%
     * brightness or a >= 5% gain move (same thresholds as the /events
     * consumer dedup) */
    float db = brightness - nfy_b; if (db < 0) db = -db;
    float dg = gain - nfy_g;       if (dg < 0) dg = -dg;
    if (mode != nfy_m || desync != nfy_d || db >= 1.0f ||
        dg >= (nfy_g > 0.0f ? nfy_g * 0.05f : 8.0f)) {
        nfy_b = brightness; nfy_g = gain; nfy_m = mode; nfy_d = desync;
        events_notify();
    }
}

/* ------------------------------------------------------------------ *
 * Reading the ISP                                                     */

/* high-water mark of the observed integration time, for SDKs that do not
 * publish its maximum - see dn_read(). Touched only by the detection thread. */
static int g_int_hwm;
/* last integration-time ratio, held over a few unreadable ticks - see dn_read() */
#define DN_RATIO_HOLD_TICKS 5
static float g_ratio_last = -1.0f;
static int   g_ratio_miss;

typedef struct {
    float d;       /* THE exposure index (higher = darker); <0 = unknown */
    float gain;    /* total_gain, IMP [24.8] linear (256 = 1x); <0 = unknown */
    float ratio;   /* integration_time / max; <0 = the fields were unreadable */
    float bright;  /* 0..100 %, status readout only; <0 = unknown */
    /* How much room the AE still has, in log2 units (32 = one stop): the
     * unused gain below each ceiling, plus a stop's worth if the integration
     * time is not railed either - the latter only where the SDK publishes a
     * real maximum to be railed against (see dn_read). <0 = the maxima were
     * not readable.
     *
     * This is what makes a ratio reading trustworthy. An AE with nothing left
     * cannot respond to the illuminator going off, so it returns r ~= 1 -
     * indistinguishable from daylight. Measured on a pitch-dark outbuilding:
     * r = 1.14, below ir_ratio_day, and only the reserve (1 unit, i.e. none)
     * separates that from a genuinely lit room. */
    int   headroom;
    int   isp;     /* mode the ISP itself reports: DN_DAY/DN_NIGHT/DN_UNKNOWN */
    int   ag, mag, dg, idg;   /* the scrape's gain fields, log2 units; -1 = absent */

    /* ---- SHADOW READ, decides nothing (yet) ----------------------------
     * The same integration time and maximum the /proc scrape above supplies,
     * taken instead from IMP_ISP_Tuning_GetExpr via hal_isp_exposure(). This
     * exists to answer one question with real fleet data before anything is
     * switched over: does the SDK accessor agree with the scrape on the T31s,
     * and does it give the two T20 cellar cameras the REAL maximum that
     * g_int_hwm currently has to guess?
     *
     * Deliberately NOT fed into ->ratio or ->d. The scrape is what every
     * decision path, every tuned threshold and every recorded trace in the
     * fleet was calibrated against; swapping the measurement underneath them
     * on the strength of a header reading would invalidate all of it at once.
     * These ride along in the trace CSV (columns imp_*) so a real dusk and a
     * real dawn can be compared side by side first. <0 / 0 = unavailable
     * (T40/T41 have no GetExpr, sim, or the ISP is not up yet). */
    float ratio_imp;    /* it/it_max per the SDK, same scale as ->ratio */
    int   imp_it;       /* sensor lines */
    int   imp_it_max;   /* sensor lines - the number the T20 scrape lacks */
    int   imp_line_us;  /* microseconds per sensor line */
    int   imp_expr_us;  /* GetEVAttr's own exposure, for cross-checking */
} dn_sample;

/* One sample of the ISP exposure state.
 *
 * The gain half comes from IMP_ISP_Tuning_GetTotalGain where the platform has
 * it (robust, and the same number prudynt/raptor and the WebUI plot), falling
 * back to the /proc dump's own gain fields. The integration-time half comes
 * from the /proc scrape, which therefore runs on every decision tick rather
 * than only for the status readout - which is what the 2 s default interval_ms
 * pays for.
 *
 * That half DOES have an IMP accessor after all - IMP_ISP_Tuning_GetExpr, on
 * every classic-tuning SoC, maximum included (this comment used to say there
 * was none; corrected 2026-09-06 against the vendored headers). It is read
 * here as a shadow measurement and traced, but decides nothing yet: see the
 * imp_* members of dn_sample for why the cutover is a separate, evidenced
 * step rather than a drop-in.
 *
 * The isp-m0 gain fields are in the IMP log2 unit (0 = 1x, 32 = 2x, per the
 * SetMaxAgain/SetMaxDgain docs), so linear = 2^(units/32) and the analog,
 * sensor-digital and ISP-digital parts add in log space. */

/* Prefix-anchored field match: the value part past `pfx`, or NULL.
 *
 * Every value line in the dump starts at column 0, and the sscanf formats
 * this replaces were anchored there too (a scanf literal only ever matches at
 * the current position) - so an indented line already yielded nothing even
 * though strstr() matched it. This therefore accepts exactly the same lines
 * as the old strstr()-scan-the-whole-line + sscanf()-re-match-the-prefix
 * pair, for one comparison instead of two scans, ~43k times a day forever.
 * The "MAX ..." prefixes must stay AHEAD of the plain ones they contain, as
 * they did before. */
#define DN_FIELD(line, pfx) \
    (strncmp((line), (pfx), sizeof(pfx) - 1) ? NULL : (line) + sizeof(pfx) - 1)

/* strtol with sscanf's rule: no number there -> leave the target alone, so a
 * field the dump omits or mangles stays at its -1 "absent" marker. */
static void dn_field_int(const char *v, int *out)
{
    char *e; long x = strtol(v, &e, 10);
    if (e != v) *out = (int)x;
}

/* the "%31s" of "ISP Runing Mode : %31s": first whitespace-delimited token */
static void dn_field_tok(const char *v, char *out, size_t osz)
{
    while (isspace((unsigned char)*v)) v++;
    size_t i = 0;
    while (v[i] && !isspace((unsigned char)v[i]) && i + 1 < osz){ out[i] = v[i]; i++; }
    out[i] = 0;
}

/* MAX SENSOR analog gain from the isp_info next to a configured isp-m0, -1
 * if there is none. The cap is static, so it is re-read only every
 * DN_ISP_REPROBE_TICKS calls, not on every tick. */
static int dn_info_mag(const char *isp_path)
{
    static int mag = -1;
    static unsigned age;
    const char *b = strrchr(isp_path, '/');
    b = b ? b + 1 : isp_path;
    if (strcmp(b, "isp-m0")) return -1;
    if (age++ % DN_ISP_REPROBE_TICKS) return mag;
    mag = -1;
    char p[160];
    int n = snprintf(p, sizeof p, "%.*sisp_info", (int)(b - isp_path), isp_path);
    if (n < 0 || n >= (int)sizeof p) return mag;
    FILE *f = fopen(p, "r");
    if (!f) return mag;
    char line[256];
    const char *v;
    while (fgets(line, sizeof line, f))
        if ((v = DN_FIELD(line, "MAX SENSOR analog gain :"))) {
            dn_field_int(v, &mag);
            break;
        }
    fclose(f);
    return mag;
}

static void dn_read(const ms_daynight_cfg *dn, dn_sample *o)
{
    o->d = o->gain = o->ratio = o->bright = -1.0f;
    o->headroom = -1;
    o->isp = DN_UNKNOWN;
    o->ag = o->mag = o->dg = o->idg = -1;
    o->ratio_imp = -1.0f;
    o->imp_it = o->imp_it_max = o->imp_line_us = o->imp_expr_us = 0;

    { uint32_t hg; if (hal_isp_total_gain(&hg) == 0) o->gain = (float)hg; }

    /* the shadow read (see dn_sample) - taken BEFORE the scrape so both halves
     * describe as nearly the same instant as two separate reads can */
    {
        hal_isp_expo ex;
        if (hal_isp_exposure(&ex) == 0) {
            o->imp_it      = (int)ex.it_lines;
            o->imp_it_max  = (int)ex.it_max_lines;
            o->imp_line_us = (int)ex.line_us;
            o->imp_expr_us = (int)ex.expr_us;
            if (ex.it_max_lines > 0) {
                float r = (float)ex.it_lines / (float)ex.it_max_lines;
                if (r > 1.0f) r = 1.0f;
                if (r < DN_RATIO_MIN) r = DN_RATIO_MIN;
                o->ratio_imp = r;
            }
        }
    }

    /* T41 open stack: /proc prints only the sensor maximum, which overstates
     * the reserve once a gain cap is set - a railed meter would pass for a
     * lit room. So only the SDK's capped ceiling counts. */
    int ae_hr = -1;
    {
        hal_isp_ae ae;
        if (hal_isp_ae_info(&ae) == 0) {
            float ev = log2f((float)ae.again_max / 1024.0f)
                     - (float)ae.total_log2_q16 / 65536.0f;
            ae_hr = ev > 0.0f ? (int)(32.0f * ev + 0.5f) : 0;
            /* the stop from integration time only while gain is still 1x:
             * once gain rises the AE sits at its own limit, which can be
             * below it_max (T55A: 1750 of 2092 lines at 6.9x) */
            if (ae.total_log2_q16 < 65536 / 32 && ae.it > 0 &&
                ae.it < ae.it_max * 9 / 10)
                ae_hr += 32;
        }
    }

    /* The configured path first, then the known alternatives. Measured on the
     * live fleet 2026-08-17, and it splits cleanly by SoC generation: all ten
     * T31/T23 cameras have /proc/jz/isp/isp-m0 (the default), and both T20
     * ones publish the same fields as /proc/jz/isp/isp_info instead, with no
     * isp-m0 at all. That is the older SDK's naming, not a per-camera quirk,
     * so the fallback is worth having permanently. It matters more than the
     * two-of-twelve count suggests: those two T20s are cam-K and
     * cam-J, the deliberately dark cellar cameras whose light is
     * switched on by hand - i.e. precisely the scene the spontaneous
     * brightening path exists for, and cam-J is the a5dae07 camera
     * this whole exposure-index change was motivated by. With only the default configured, the scrape on those two has
     * been failing silently for as long as it has existed: the gain came from
     * the IMP API and every field only the scrape can provide (integration
     * time, and before the redesign the brightness fallback) was simply never
     * read. */
    static const char *const ALT[] = { "/proc/jz/isp/isp_info", NULL };
    static const char *used;            /* single-caller: the detection thread */
    /* Once a fallback is known to work, open THAT first. Probing the
     * configured path every time cost a guaranteed ENOENT fopen on every tick
     * of every T20's life (isp-m0 does not exist in that SDK and never will) -
     * ~43k a day, forever. The configured path is still re-probed, both
     * whenever the cached one stops opening and once every
     * DN_ISP_REPROBE_TICKS regardless, so "the configured path came back" (or
     * was corrected via /control) is still picked up - just not 43k times a
     * day. Where the configured path is right this whole dance costs nothing,
     * exactly as before. */
    static unsigned since_reprobe;
    FILE *fp = NULL;
    int from_cfg = 0;
    if (used && ++since_reprobe < DN_ISP_REPROBE_TICKS)
        fp = fopen(used, "r");
    if (!fp) {
        since_reprobe = 0;
        fp = fopen(dn->isp_path, "r");
        if (fp) {
            from_cfg = 1;
            used = NULL;                /* the configured path came back */
        } else {
            for (int i = 0; ALT[i] && !fp; i++) {
                if (!strcmp(ALT[i], dn->isp_path)) continue;
                fp = fopen(ALT[i], "r");
                if (fp && used != ALT[i]) {
                    used = ALT[i];
                    /* INFO, not WARN: on T20 this is the normal path, and a
                     * WARN lands in last_errors and turns the WebUI health tile */
                    LOGI(MOD, "%s is not readable, using %s instead",
                         dn->isp_path, ALT[i]);
                }
            }
        }
    }
    if (fp) {
        char line[256];
        int it = -1, mit = -1, ag = -1, dg = -1, idg = -1, cb = -1;
        int mag = -1, midg = -1;          /* the ceilings, for the reserve */
        int tgq = -1;                     /* open-tx-isp T41: total gain, log2 in Q16 */
        char m[32] = {0};

        /* one bit per field; stop reading once the dump has supplied all of
         * them rather than scanning the rest of a several-KB register dump */
        unsigned got = 0;
        const char *v;

        while (fgets(line, sizeof line, fp)) {
            if      ((v = DN_FIELD(line, "ISP Runing Mode :")))
                { dn_field_tok(v, m, sizeof m);  got |= 1u<<0; }
            else if ((v = DN_FIELD(line, "SENSOR Integration Time :")))
                { dn_field_int(v, &it);          got |= 1u<<1; }
            else if ((v = DN_FIELD(line, "SENSOR Max Integration Time :")))
                { dn_field_int(v, &mit);         got |= 1u<<2; }
            else if ((v = DN_FIELD(line, "MAX SENSOR analog gain :")))
                { dn_field_int(v, &mag);         got |= 1u<<3; }
            else if ((v = DN_FIELD(line, "MAX ISP digital gain :")))
                { dn_field_int(v, &midg);        got |= 1u<<4; }
            else if ((v = DN_FIELD(line, "SENSOR analog gain :")))
                { dn_field_int(v, &ag);          got |= 1u<<5; }
            else if ((v = DN_FIELD(line, "SENSOR digital gain :")))
                { dn_field_int(v, &dg);          got |= 1u<<6; }
            else if ((v = DN_FIELD(line, "ISP digital gain :")))
                { dn_field_int(v, &idg);         got |= 1u<<7; }
            else if ((v = DN_FIELD(line, "Brightness :")))
                { dn_field_int(v, &cb);          got |= 1u<<8; }
            /* open-tx-isp on T41 prints its own, differently spelled dump:
             * "AeIntegrationTime : 1750", "AeMaxIntegrationTime : 2092",
             * "TotalGainDb : 390142" (log2 of the total gain in Q16; the
             * ExposureValue line is integration time x that gain) and a
             * lower-case "ISP Runing Mode : day" */
            else if ((v = DN_FIELD(line, "AeIntegrationTime :")))
                { dn_field_int(v, &it);          got |= 1u<<9; }
            else if ((v = DN_FIELD(line, "AeMaxIntegrationTime :")))
                { dn_field_int(v, &mit);         got |= 1u<<10; }
            else if ((v = DN_FIELD(line, "TotalGainDb :")))
                { dn_field_int(v, &tgq);         got |= 1u<<11; }
            else continue;
            if (got == 0x1ffu) break;
        }
        fclose(fp);

        if (from_cfg) {
            int im = dn_info_mag(dn->isp_path), lo = dn_mag_min(mag, im);
            static int said_mag;
            if (lo != mag && !said_mag) {
                said_mag = 1;
                LOGI(MOD, "%s publishes MAX SENSOR analog gain %d, isp_info %d: "
                          "using %d, the cap the AE stops at",
                     dn->isp_path, mag, im, lo);
            }
            mag = lo;
        }

        if      (!strcasecmp(m, "Day"))   o->isp = DN_DAY;
        else if (!strcasecmp(m, "Night")) o->isp = DN_NIGHT;
        o->ag = ag; o->mag = mag; o->dg = dg; o->idg = idg;

        if (o->gain < 0.0f && tgq >= 0) {
            o->gain = 256.0f * exp2f((float)tgq / 65536.0f);   /* [24.8], 256 = 1x */
            if (!isfinite(o->gain)) o->gain = -1.0f;
        }
        if (o->gain < 0.0f && (ag >= 0 || dg >= 0 || idg >= 0)) {
            float units = 0.0f;                 /* log2 gain, 32 units per stop */
            if (ag  > 0) units += (float)ag;
            if (dg  > 0) units += (float)dg;
            if (idg > 0) units += (float)idg;
            o->gain = 256.0f * exp2f(units / 32.0f);
            /* garbage/out-of-range gain regs can push exp2f to +inf, which
             * would later print as the literal "inf" - invalid JSON. */
            if (!isfinite(o->gain)) o->gain = -1.0f;
        }

        /* Some SDKs publish the integration time but not its maximum -
         * cam-J (T20/jxf22) is exactly that case, and it is the camera
         * that needs the index most. The maximum is a constant of the sensor
         * mode and frame rate, so the longest exposure ever observed is a
         * sound stand-in for it: a high-water mark, which can only be an
         * UNDER-estimate early on, and an under-estimate makes the ratio rail
         * at 1.0 and the index degrade to plain gain - the safe direction. One
         * genuinely dark stretch pins it at the true value and it stays there.
         * Reset with the thread, not kept across a re-enable, because the
         * sensor mode may have changed underneath us. */
        int mit_real = mit > 0;             /* the SDK published it itself */
        if (mit <= 0 && it > 0) {
            if (it > g_int_hwm) g_int_hwm = it;
            mit = g_int_hwm;
        }
        if (mag >= 0 && ag >= 0) {
            o->headroom = (mag - ag) + (midg >= 0 && idg >= 0 ? midg - idg : 0);
            if (o->headroom < 0) o->headroom = 0;
            /* A stop's worth if the AE could still answer by lengthening the
             * exposure - but only a REAL maximum can establish that. Against
             * the high-water mark above the test fires forever once a
             * transient has pinned the mark over the value the AE settles at,
             * and it reported 32 units of reserve for a meter railed on both
             * gains. The safe-direction argument above is about o->ratio, not
             * about this. Both T20s, no maximum published; every T31/T23
             * publishes one and reaches it when dark, so this is inert there.
             * Measured 2026-08-28, see TODO.md and scenario 29. */
            if (mit_real && it > 0 && it < mit * 9 / 10) o->headroom += 32;
        }

        if (it >= 0 && mit > 0) {
            float r = (float)it / (float)mit;
            if (r > 1.0f) r = 1.0f;
            if (r < DN_RATIO_MIN) r = DN_RATIO_MIN;
            o->ratio = r;
        }

        /* Cutover 2026-09-06: where the SDK never publishes a real max (both
         * T20 cellar cameras - see mit_real above), the high-water-mark ratio
         * just computed is a boot-time-blind estimate (rails at 1.0 for
         * minutes after every restart, confirmed live on cam-07:
         * ratio_imp=0.8350 vs ratio=1.0000 at t=21s, converging to within
         * 0.1% of each other by ~2.5 min). IMP_ISP_Tuning_GetExpr's own
         * it_max_lines is authoritative from sample 1, so prefer it whenever
         * the scrape's max is the estimate, not the SDK's. T31/T23 (mit_real
         * true) are untouched - cross-checked to agree with IMP there
         * already, nothing to gain by switching. */
        if (!mit_real && o->ratio_imp > 0.0f)
            o->ratio = o->ratio_imp;

        /* brightness %, the thingino daynightd formula - STATUS ONLY. It
         * stopped being a decision path in the 2026-08-17 redesign: it is a
         * second, cruder view of the same two fields the exposure index uses
         * properly, and keeping it meant every decision rule existed twice. */
        if (it >= 0 && mit > 0) {
            float b = (1.0f - (float)it / (float)mit) * 100.0f;
            if (ag  >= 0) b /= 1.0f + ag  / 160.0f;
            if (idg >  0) b /= 1.0f + idg /  80.0f;
            if (b < 0.0f)   b = 0.0f;
            if (b > 100.0f) b = 100.0f;
            o->bright = b;
        } else if (cb >= 0) {
            o->bright = ((float)cb / 255.0f) * 100.0f;
        } else if (m[0]) {
            if      (!strcasecmp(m, "Day"))   o->bright = 75.0f;
            else if (!strcasecmp(m, "Night")) o->bright = 25.0f;
        }
    }
    if (o->headroom < 0) o->headroom = ae_hr;

    /* One-shot, at the first sample where the SDK answers: say what the two
     * measurements make of the same instant. This is the line that tells you,
     * on a camera you have just flashed and without arming a trace, whether
     * the scrape has a real maximum or an estimated one and whether the SDK
     * agrees with it. Once per process - the trace CSV carries the series. */
    {
        static int said;
        if (!said && o->imp_it_max > 0) {
            said = 1;
            LOGI(MOD, "exposure sources: scrape r=%.4f (%s max), GetExpr r=%.4f "
                      "it=%d/%d line=%dus (%d/%dus), GetEVAttr %dus: %s",
                 (double)o->ratio, g_int_hwm > 0 ? "estimated" : "published",
                 (double)o->ratio_imp, o->imp_it, o->imp_it_max, o->imp_line_us,
                 o->imp_it * o->imp_line_us, o->imp_it_max * o->imp_line_us,
                 o->imp_expr_us,
                 g_int_hwm > 0 ? "IMP decides" : "shadow read only");
        }
    }

    /* A tick without the integration time would switch d to bare gain, a
     * different metric: one such sample in a look's average falsifies it.
     * Hold the last ratio across a short gap. */
    if (o->ratio > 0.0f) {
        g_ratio_last = o->ratio; g_ratio_miss = 0;
    } else if (g_ratio_last > 0.0f && ++g_ratio_miss <= DN_RATIO_HOLD_TICKS) {
        o->ratio = g_ratio_last;
    }

    if (o->gain > 0.0f) {
        o->d = (o->ratio > 0.0f) ? o->gain * o->ratio : o->gain;
        if (!isfinite(o->d) || o->d <= 0.0f) o->d = -1.0f;
    }
}


/* A reading from a railed AE is a clip, not a level. It may prove night -
 * a meter pegged at the DARK end cannot happen in daylight - but it must
 * never be remembered as the reference. r from two clipped readings is no
 * information either (measured: r=1.00 on a meter whose scene was worth 23x
 * less). Unknown headroom counts as usable, or platforms without ceiling
 * fields could never anchor at all. */
static int dn_clipped(int headroom)
{
    return headroom >= 0 && headroom < DN_IR_MIN_HEADROOM;
}

#ifdef DN_READ_TEST
/* test-only opening for dn_read() - see scripts/test_daynight_read.c */
int dn_read_test(const ms_daynight_cfg *dn, float *gain, float *ratio,
                 float *d, int *headroom, int *clipped)
{
    dn_sample o;
    dn_read(dn, &o);
    *gain = o.gain; *ratio = o.ratio; *d = o.d; *headroom = o.headroom;
    *clipped = dn_clipped(o.headroom);
    return o.isp;
}
#endif


/* ------------------------------------------------------------------ *
 * Driving the board                                                   */

/* How long a board hook may run before it is presumed wedged. switch_cmd
 * drives a motor through a script chain, irprobe_cmd writes a GPIO - both are
 * sub-second in practice, so these are generous ceilings for "it is never
 * coming back", not budgets anything normal spends. */
#ifndef DN_CMD_TIMEOUT_MS
#define DN_CMD_TIMEOUT_MS 10000      /* switch_cmd */
#endif
#ifndef DN_IRPROBE_TIMEOUT_MS
#define DN_IRPROBE_TIMEOUT_MS 5000   /* irprobe_cmd */
#endif
/* Grace given to a hook when shutdown is already requested: long enough for a
 * healthy script to finish the switch it started, short enough that a wedged
 * one cannot hold daynight_stop()'s pthread_join(). */
#ifndef DN_CMD_STOP_MS
#define DN_CMD_STOP_MS 1000
#endif
/* ... and to one started after the stop request (the illuminator hand-back).
 * Not shorter: the board hook spawns several processes and a killed "on" also
 * disarms its own watchdog. Worst chain after stop: one hook in flight
 * (STOP+KILL 1.5 s) + one hand-back (LATE+KILL 1.5 s) = 3 s < the 4 s alarm;
 * dn_switch() starts nothing once stopping. */
#ifndef DN_CMD_LATE_MS
#define DN_CMD_LATE_MS 1000
#endif
#define DN_CMD_POLL_MS 20            /* waitpid(WNOHANG) cadence */
#define DN_CMD_KILL_MS 500           /* reap window after SIGKILL */

/* Reap a board-hook child, bounded. A blocking waitpid() here is a hang the
 * whole feature rides on: the hook runs ON the detection thread, so a script
 * that never returns - stuck I2C, an NFS-mounted script, a shell waiting on a
 * pipe nobody writes - freezes day/night switching AND daemon shutdown, since
 * daynight_stop() joins this same thread. Poll instead; wait on the stop gate
 * so a shutdown is noticed within one slice rather than at the deadline, and
 * SIGKILL a child that outstays its welcome.
 *
 * Returns the child's exit status, or -1 if it died on a signal, had to be
 * killed, or could not be reaped. A child stuck in uninterruptible sleep
 * survives SIGKILL, so the reap after it is bounded too: we give up and leave
 * a zombie rather than trade this hang for a shorter one. */
static int dn_reap(pid_t pid, const char *cmd, const char *arg, int timeout_ms)
{
    int64_t deadline = ms_now_us() / 1000 + timeout_ms;
    int killed = 0, shortened = 0;
    int stop_ms = ms_stopgate_stopped(&g_gate) ? DN_CMD_LATE_MS : DN_CMD_STOP_MS;
    for (;;){
        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        if (r < 0 && errno != EINTR){
            LOGW(MOD, "waitpid('%s %s') failed: %s", cmd, arg, strerror(errno));
            return -1;
        }
        int64_t now = ms_now_us() / 1000;
        int stopping = ms_stopgate_stopped(&g_gate);
        if (stopping && !shortened && !killed){
            shortened = 1;
            if (deadline > now + stop_ms) deadline = now + stop_ms;
        }
        if (now >= deadline){
            if (killed){
                LOGE(MOD, "'%s %s' (pid %d) survived SIGKILL - abandoned, "
                          "hook stuck in the kernel",
                     cmd, arg, (int)pid);
                return -1;
            }
            LOGW(MOD, "'%s %s' (pid %d) did not finish in time - killing it. "
                      "A board hook must not block the detection thread",
                 cmd, arg, (int)pid);
            kill(pid, SIGKILL);
            killed = 1;
            deadline = now + DN_CMD_KILL_MS;
        }
        /* the gate returns immediately once stop is requested, so poll on a
         * plain sleep from that point on rather than spinning */
        if (killed || stopping){
            struct timespec ts = { 0, DN_CMD_POLL_MS * 1000000L };
            nanosleep(&ts, NULL);
        } else {
            ms_stopgate_wait(&g_gate, DN_CMD_POLL_MS);
        }
    }
}

/* run "<switch_cmd> day|night" (the thingino board script: ircut/light/color).
 * The mode change is committed even if the command fails so a missing script
 * warns once per switch instead of retrying every sample. */
/* highest fd the child closes: every open one (RLIMIT_NOFILE may exceed
 * 1024) plus a margin for other threads opening more before the vfork. Read
 * in the parent, opendir() is no call for a vfork child. */
static int dn_fd_top(void)
{
    int top = 1024;
    DIR *d = opendir("/proc/self/fd");
    if (!d) return top;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        int fd = atoi(de->d_name);
        if (fd + 64 > top) top = fd + 64;
    }
    closedir(d);
    return top;
}

/* "<cmd> <arg>" with stdout/stderr on /dev/null. vfork, not fork: copying
 * the whole daemon can ENOMEM on a small board (NEU-01b in imp_motion.c).
 * Its own noinline frame, so the child cannot clobber the caller's locals. */
static pid_t __attribute__((noinline)) dn_vfork_exec(const char *cmd, const char *arg,
                                                      int outfd, int top)
{
    pid_t pid = vfork();
    if (pid == 0){
        if (outfd >= 0) dup2(outfd, 1);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0){ if (outfd < 0) dup2(nul,1); dup2(nul,2); if (nul>2) close(nul); }
        /* The vendor/open libimp keeps its device nodes (/dev/isp-m0, the
         * frame channels) open without O_CLOEXEC, and a board hook may leave a
         * background child behind (timps-irprobe's 60 s watchdog sleeper). An
         * inherited fd keeps the kernel module "busy" after timps has stopped
         * (rmmod tx_isp_*: busy). This child has its own fd table, so closing
         * everything above stderr costs the daemon nothing. */
        for (int fd = 3; fd < top; fd++) close(fd);
        execlp(cmd, cmd, arg, (char*)NULL);
        _exit(127);              /* exec failed (script missing / not a program) */
    }
    return pid;
}

static pid_t dn_spawn(const char *cmd, const char *arg, int outfd)
{
    return dn_vfork_exec(cmd, arg, outfd, dn_fd_top());
}

/* The tail is machine-readable on purpose. Everything in it is already in
 * the surrounding prose, but the dashboards count these lines with a grep,
 * and a reworded sentence would empty them silently - the same failure the
 * exposure series had when it stopped without saying so. */
static void dn_switch(int mode, const char *why, const char *cmd,
                      float s, float ref, float bar)
{
    const char *arg = (mode == DN_NIGHT) ? "night" : "day";
    if (ms_stopgate_stopped(&g_gate)) {     /* shutdown budget: no new motor drive */
        LOGD(MOD, "stopping - not switching to %s (%s)", arg, why);
        return;
    }
    LOGI(MOD, "switching to %s (%s): %s %s [mode=%s exp=%.0f ref=%.0f bar=%.0f]",
         arg, why, cmd, arg, arg, (double)s, (double)ref, (double)bar);
    /* F-01: vfork()+execlp() instead of system(). switch_cmd comes from the
     * config file; system() would let a value like "reboot; nc ..." inject
     * shell commands (as root). exec'ing it as a single program with the fixed
     * arg "day"/"night" removes the shell entirely - a malicious value just
     * fails to exec, it cannot inject. */
    pid_t pid = dn_spawn(cmd, arg, -1);
    if (pid < 0){ LOGW(MOD,"daynight: vfork failed: %s", strerror(errno)); return; }
    int rc = dn_reap(pid, cmd, arg, DN_CMD_TIMEOUT_MS);
    if (rc != 0)
        LOGW(MOD, "'%s %s' failed (rc=%d) - is the script installed?", cmd, arg, rc);
}


/* Drive the IR illuminator alone: "<irprobe_cmd> on|off".
 *
 * Deliberately a different command from switch_cmd, because on the hardware
 * these are different mechanisms with different costs. switch_cmd moves a
 * motor - audible, mechanical, once or twice a day. This writes a GPIO -
 * silent, unlimited, and the only thing it costs is a few seconds of dimmer
 * night image. Conflating them would make every look a click.
 *
 * Same vfork+execlp discipline as dn_switch: never a shell, so a hostile
 * config value fails to exec instead of injecting. Returns 0 when the command
 * ran and succeeded; anything else means the illuminator did not move. */
/* Set by /control, cleared when the night branch acts on it: one look. */
static volatile int g_probe_req = 0;
/* A board that cannot switch its illuminator separately fails every look;
 * DN_IR_MAX_FAILS in a row turn looks off for the session (a restart
 * re-tests) and the camera checks like one without a switchable LED. */
static volatile int g_ir_unusable = 0;

int daynight_request_probe(void)
{
    if (!g_cfg.daynight.irprobe_cmd[0] || g_ir_unusable) return -1;
    g_probe_req = 1;
    return 0;
}

/* Idle-Dark consumer kick. Called from the hub's activity callback under its
 * lock, so it only sets a flag and signals; the gate is kicked only while the
 * thread has armed it (it waits kickable then), so a kick never shortens an
 * ordinary sampling tick. */
static int g_watch_kick;
static int g_dni_armed;
/* what the illuminator hook answered to "idle" (DNI_HOOK_*) */
static int g_dni_hook = DNI_HOOK_OK;

/* Idle-Dark status, under g_st_mu */
static dn_ir_idle_status g_st_idle = { 0, 0, 0, 0, 0, "disabled" };

void daynight_watch_kick(void)
{
    __atomic_store_n(&g_watch_kick, 1, __ATOMIC_RELAXED);
    if (__atomic_load_n(&g_dni_armed, __ATOMIC_ACQUIRE)) {
        /* provisional, before the subscriber's first pop; the thread sets
         * the real one once the hook has run */
        hub_set_image_ready_at(ms_now_us() +
            (int64_t)(DN_IR_WAKE_SETTLE_MS + DN_IR_WAKE_HOOK_MS) * 1000);
        ms_stopgate_kick(&g_gate);
    }
}

/* "<irprobe_cmd> <verb>": its exit status, -1 if it could not run or was
 * killed. `out` (optional) gets the start of its stdout. */
static int dn_irhook(const char *cmd, const char *verb, char *out, size_t outsz)
{
    if (!cmd || !cmd[0]) return -1;
    int pfd[2] = { -1, -1 };
    if (out) {
        out[0] = 0;
        if (pipe(pfd) == 0) {
            fcntl(pfd[0], F_SETFD, FD_CLOEXEC);
            fcntl(pfd[1], F_SETFD, FD_CLOEXEC);
        } else pfd[0] = pfd[1] = -1;
    }
    pid_t pid = dn_spawn(cmd, verb, pfd[1]);
    if (pfd[1] >= 0) close(pfd[1]);
    int rc = -1;
    if (pid < 0) LOGW(MOD, "irprobe: vfork failed: %s", strerror(errno));
    else rc = dn_reap(pid, cmd, verb, DN_IRPROBE_TIMEOUT_MS);
    if (out && outsz > 1 && pfd[0] >= 0) {
        /* non-blocking: a background child of the hook may hold the pipe */
        fcntl(pfd[0], F_SETFL, O_NONBLOCK);
        ssize_t n = read(pfd[0], out, outsz - 1);
        out[n > 0 ? n : 0] = 0;
        close(pfd[0]);
    }
    return rc;
}

static int dn_irprobe(const char *cmd, int on)
{
    if (!cmd || !cmd[0]) return -1;
    const char *arg = on ? "on" : "off";
    int rc = dn_irhook(cmd, arg, NULL, 0);
    if (rc != 0)
        LOGW(MOD, "'%s %s' failed (rc=%d)", cmd, arg, rc);
    return rc == 0 ? 0 : -1;
}

/* Switch the illuminator back on after a look. A failure here leaves
 * the LEDs off for the rest of the night with nothing else to retry it, so one
 * more try (not more: a hung hook costs DN_IRPROBE_TIMEOUT_MS each). */
static void dn_light_on(const char *cmd, int tries)
{
    if (ms_stopgate_stopped(&g_gate)) tries = 1;
    for (int i = 0; i < tries; i++)
        if (dn_irprobe(cmd, 1) == 0) return;
    LOGW(MOD, "illuminator could not be switched back on after a look "
              "(%d %s) - it may stay off until the next switch",
         tries, tries == 1 ? "try" : "tries");
}


/* "<irprobe_cmd> idle": 0 when the illuminator is off. An old hook answers
 * an unknown verb with 1 (exec'd `light` usage) or 2, a white light in the
 * set with 3; those end Idle-Dark until re-enabled. Anything else is a
 * failure, DN_IR_MAX_FAILS in a row end it too. */
static int dn_idle_hook(const char *cmd, int *fails)
{
    int rc = dn_irhook(cmd, "idle", NULL, 0);
    if (rc == 0) { *fails = 0; return 0; }
    int was = g_dni_hook;
    if (rc == 3)
        g_dni_hook = DNI_HOOK_WHITE;
    else if (rc == 1 || rc == 2 || ++*fails >= DN_IR_MAX_FAILS)
        g_dni_hook = DNI_HOOK_OLD;
    if (g_dni_hook == DNI_HOOK_WHITE && was != DNI_HOOK_WHITE)
        LOGW(MOD, "'%s idle' refused (white light is among the illuminators "
                  "daynight.controls switches) - Idle-Dark stays off", cmd);
    else if (g_dni_hook == DNI_HOOK_OLD && was != DNI_HOOK_OLD)
        LOGW(MOD, "'%s idle' failed (rc=%d) - the hook has no 'idle' verb "
                  "(update timps-irprobe) or nothing to switch; Idle-Dark "
                  "stays off", cmd, rc);
    else
        LOGW(MOD, "'%s idle' failed (rc=%d)", cmd, rc);
    return -1;
}


/* ------------------------------------------------------------------ *
 * The calendar: the whole decision in schedule mode, unused in auto. */

/* parse "HH:MM" -> minutes since local midnight [0..1439], or -1 if unset/bad */
static int dn_hhmm_min(const char *s)
{
    if (!s || !s[0]) return -1;
    int h = -1, m = -1;
    if (sscanf(s, "%d:%d", &h, &m) != 2) return -1;
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
}

/* fixed local-clock window: night from hhmm_night until hhmm_day; the window
 * may wrap past midnight (night 20:00, day 06:30 => night start > day start). */
static int dn_time_target(const char *hhmm_night, const char *hhmm_day,
                          const struct tm *now)
{
    int n = dn_hhmm_min(hhmm_night);
    int d = dn_hhmm_min(hhmm_day);
    if (n < 0 || d < 0 || n == d) return DN_UNKNOWN;
    int cur = now->tm_hour * 60 + now->tm_min;
    int is_night = (n > d) ? (cur >= n || cur < d)   /* wraps midnight */
                           : (cur >= n && cur < d);
    return is_night ? DN_NIGHT : DN_DAY;
}

/* Today's sunrise/sunset for lat/lon as epoch time_t (UTC), via the standard
 * low-precision "sunrise equation" (NOAA/Meeus). Everything stays in one time
 * base (epoch seconds). The calendar day is chosen by LOCAL SOLAR time, not
 * UTC: lon*240 s (= lon/15 h) shifts `now` into local solar time before
 * flooring to a day, because solar midnight is by definition the middle of
 * the night and can never land next to a sunrise/sunset. Keying on UTC
 * midnight instead (gmtime_r + floor, the previous approach) rolled the
 * calendar to TOMORROW at UTC midnight, which is 17:00 PDT / 16:00 PST in
 * Vancouver - so every evening the schedule calendar adopted tomorrow's
 * sunset (a near-identical time, off by the ~2 min/day drift) with tomorrow's
 * sunRISE still hours away, and dn_cal_target's "wall >= sr" failed
 * immediately: an instant, exact-to-the-second switch to night at UTC
 * midnight instead of at the real sunset (2026-09-26, reported for
 * Vancouver). Returns 0 with sr_out/ss_out set on a normal day, +1 for polar
 * day, -1 for polar night. */
static int dn_sun_times(float lat, float lon, time_t now,
                        time_t *sr_out, time_t *ss_out)
{
    time_t day = (now + (time_t)(lon * 240.0f)) / 86400;
    time_t midnight = day * 86400;

    /* Memoized per local-solar day: the answer is constant for the whole day
     * by construction (it is derived from `midnight`, nothing else
     * time-varying), yet mode=schedule asked for it on every 2 s tick - ~10
     * double-precision libm calls (sin/cos/asin/acos/fmod) on a soft-float
     * SoC, ~43k times a day, all returning the same two instants. Keyed on
     * lat/lon as well, so a coordinate change via /control takes effect on
     * the next tick; the sunrise/sunset OFFSETS are applied by the callers
     * AFTER this returns and so need no invalidation. Two slots because
     * dn_cal_target() looks at yesterday, today and tomorrow, which fewer
     * slots would thrash. Locked: daynight_sun_status() calls this from the
     * control thread too. */
    static struct { time_t day, sr, ss; float lat, lon; int r, valid; } memo[3];
    static pthread_mutex_t memo_mu = PTHREAD_MUTEX_INITIALIZER;
    int slot = (int)(((day % 3) + 3) % 3);
    pthread_mutex_lock(&memo_mu);
    if (memo[slot].valid && memo[slot].day == midnight &&
        memo[slot].lat == lat && memo[slot].lon == lon) {
        if (sr_out) *sr_out = memo[slot].sr;
        if (ss_out) *ss_out = memo[slot].ss;
        int mr = memo[slot].r;
        pthread_mutex_unlock(&memo_mu);
        return mr;
    }

    const double D2R = M_PI / 180.0, R2D = 180.0 / M_PI;
    double jd_mid = 2440587.5 + (double)midnight / 86400.0;
    double n = floor(jd_mid - 2451545.0 + 0.0008 + 0.5);
    double Jstar = n - lon / 360.0;                    /* lon east-positive */
    double M  = fmod(357.5291 + 0.98560028 * Jstar, 360.0); if (M < 0) M += 360;
    double Mr = M * D2R;
    double C  = 1.9148 * sin(Mr) + 0.0200 * sin(2 * Mr) + 0.0003 * sin(3 * Mr);
    double lambda = fmod(M + C + 180.0 + 102.9372, 360.0); if (lambda < 0) lambda += 360;
    double lr = lambda * D2R;
    double Jtransit = 2451545.0 + Jstar + 0.0053 * sin(Mr) - 0.0069 * sin(2 * lr);
    double decl = asin(sin(lr) * sin(23.44 * D2R));
    double latr = lat * D2R;
    double cosw = (sin(-0.833 * D2R) - sin(latr) * sin(decl)) /
                  (cos(latr) * cos(decl));
    int r = 0;
    time_t sr = 0, ss = 0;
    if      (cosw < -1.0) r = +1;      /* sun always up   -> permanent day   */
    else if (cosw >  1.0) r = -1;      /* sun always down -> permanent night */
    else {
        double w0 = acos(cosw) * R2D;
        double jrise = Jtransit - w0 / 360.0;
        double jset  = Jtransit + w0 / 360.0;
        sr = (time_t)((jrise - 2440587.5) * 86400.0 + 0.5);
        ss = (time_t)((jset  - 2440587.5) * 86400.0 + 0.5);
    }
    memo[slot].day = midnight; memo[slot].lat = lat; memo[slot].lon = lon;
    memo[slot].sr  = sr;       memo[slot].ss  = ss;  memo[slot].r   = r;
    memo[slot].valid = 1;
    pthread_mutex_unlock(&memo_mu);
    if (sr_out) *sr_out = sr;
    if (ss_out) *ss_out = ss;
    return r;
}

#ifdef DN_SUN_TEST
/* test-only opening for dn_sun_times() - see scripts/test_daynight_sun.c */
int dn_sun_times_test(float lat, float lon, time_t now, time_t *sr, time_t *ss)
{
    return dn_sun_times(lat, lon, now, sr, ss);
}
#endif

/* Is a calendar configured at all, and which one?
 *
 * An explicit time window wins over a location; a location counts as set when
 * either coordinate is non-zero (0,0 is the Atlantic and is what an
 * unconfigured camera reports). A clock that has not been set yet disqualifies
 * both - a camera without NTP boots into 1970 and must not act on a
 * "calendar" derived from that.
 *
 * The precedence matters most in the case this exists for: mode=schedule is
 * the LAST RESORT for a camera whose exposure reading is unusable (a broken
 * or absent ISP path, a sensor whose AE tells you nothing), and someone
 * reaching for a last resort should not have to guess which of two configured
 * schedules is running. So it is never silent - the startup banner names the
 * source, and configuring both earns a warning (see the caller). */
enum { DN_CAL_NONE = 0, DN_CAL_TIME = 1, DN_CAL_SUN = 2 };
static int dn_cal_kind(const ms_daynight_cfg *dn, time_t wall)
{
    if (wall < DN_CLOCK_SANE) return DN_CAL_NONE;
    /* exactly the condition dn_time_target() can answer: both valid, and
     * distinct (equal edges make it return DN_UNKNOWN) */
    int n = dn->time_night_start[0] ? dn_hhmm_min(dn->time_night_start) : -1;
    int d = dn->time_day_start[0]   ? dn_hhmm_min(dn->time_day_start)   : -1;
    if (n >= 0 && d >= 0 && n != d) return DN_CAL_TIME;
    if (dn->sun_latitude != 0.0f || dn->sun_longitude != 0.0f) return DN_CAL_SUN;
    return DN_CAL_NONE;
}

/* DN_DAY / DN_NIGHT per the configured calendar, or DN_UNKNOWN if there is none */
static int dn_cal_target(const ms_daynight_cfg *dn, time_t wall)
{
    switch (dn_cal_kind(dn, wall)) {
    case DN_CAL_TIME: {
        struct tm lt; localtime_r(&wall, &lt);
        return dn_time_target(dn->time_night_start, dn->time_day_start, &lt);
    }
    case DN_CAL_SUN: {
        time_t sr, ss;
        int r = dn_sun_times(dn->sun_latitude, dn->sun_longitude, wall, &sr, &ss);
        if (r > 0) return DN_DAY;      /* polar day   */
        if (r < 0) return DN_NIGHT;    /* polar night */
        /* an offset (up to +-1 day) can push an edge across solar midnight,
         * so the neighbouring days' shifted windows count too - otherwise the
         * decision clipped them while daynight_sun_status() showed them */
        for (int k = -1; k <= 1; k++) {
            if (dn_sun_times(dn->sun_latitude, dn->sun_longitude,
                             wall + (time_t)k * 86400, &sr, &ss) != 0) continue;
            sr += (time_t)dn->sun_sunrise_offset_min * 60;
            ss += (time_t)dn->sun_sunset_offset_min  * 60;
            if (wall >= sr && wall < ss) return DN_DAY;
        }
        return DN_NIGHT;
    }
    default: return DN_UNKNOWN;
    }
}


#ifdef DN_SUN_TEST
int dn_cal_target_test(const ms_daynight_cfg *dn, time_t wall)
{
    return dn_cal_target(dn, wall);
}

#endif


/* Decimated feed into the /events history ring (events.h), which is what the
 * WebUI tuning graph pages through. Deliberately NOT dn_trace: that one is a
 * write-only CSV on a rotating file with a different column set. */
static void dn_hist_tick(const dn_sample *sm, float luma, int mode, int retain_s)
{
    static int64_t next;
    int64_t now = ms_now_us() / 1000;
    if (now < next) return;
    next = now + (int64_t)DN_HIST_PERIOD_S * 1000;

    ms_dn_sample h;
    h.t        = 0;                       /* stamped by the push */
    h.gain     = isfinite(sm->gain) ? sm->gain : -1.0f;
    h.exposure = isfinite(sm->d)    ? sm->d    : -1.0f;
    h.luma     = (luma >= 0.0f)
               ? (int16_t)(luma > 32767.0f ? 32767.0f : luma + 0.5f) : -1;
    h.bright   = (sm->bright >= 0.0f)
               ? (int8_t)(sm->bright > 100.0f ? 100.0f : sm->bright + 0.5f) : -1;
    h.mode     = (int8_t)mode;
    events_dn_hist_push(&h, retain_s);
}

/* ------------------------------------------------------------------ *
 * Trace recorder (daynight.trace_path, opt-in). The replay harness's
 * input; see docs/wiki/Day-Night-Design-Notes.md section 6.            */
static void dn_trace(const ms_daynight_cfg *dn, int64_t now_ms, int cur,
                     const dn_sample *sm, float s, float ref, float bar,
                     int64_t verdict_at, int64_t hb_at,
                     float ema_fast, float ema_slow)
{
    static FILE *f = NULL;
    static char  path[sizeof dn->trace_path];
    /* the fail-closed latch. It has to be its own flag rather than "f is NULL
     * while path is set", because ROTATION also leaves f NULL with path set
     * and must reopen on the next sample. */
    static int   open_failed = 0;
    static unsigned every = 0;

    if (!dn->trace_path[0]) {                       /* off (the default) */
        if (f) { fclose(f); f = NULL; }
        path[0] = 0; open_failed = 0;
        return;
    }
    if (strcmp(path, dn->trace_path) != 0) {        /* live path change */
        if (f) { fclose(f); f = NULL; }
        snprintf(path, sizeof path, "%s", dn->trace_path);
        open_failed = 0;
    }
    if (every++ % DN_TRACE_EVERY) return;
    if (open_failed) return;
    if (!f) {
        /* a trace is a diagnostic for tmpfs, never flash (camera eMMC/NAND
         * wear): warn once per (re)open when the path does not look like a
         * RAM filesystem, but still honour it. */
        if (strncmp(dn->trace_path, "/tmp/", 5) &&
            strncmp(dn->trace_path, "/run/", 5) &&
            strncmp(dn->trace_path, "/dev/shm/", 9))
            LOGW(MOD, "trace_path %s is not under /tmp, /run or /dev/shm - "
                      "tracing to flash wears it out", dn->trace_path);
        f = fopen(dn->trace_path, "a");
        if (!f) {
            LOGW(MOD, "cannot open trace_path %s: %s - tracing disabled until "
                      "the path changes", dn->trace_path, strerror(errno));
            open_failed = 1;
            return;
        }
        /* readers index by column position: the layout of the earlier
         * engine stays, ref/bar are the night level and the look bar,
         * verdict/heartbeat the next verdict and look, trend_* are -1 */
        if (ftello(f) == 0)
            fputs("t_mono_ms,cur,d,gain,ratio,smooth,ref,bar,"
                  "verdict_in_s,heartbeat_in_s,trend_fast,trend_slow,"
                  /* the IMP_ISP_Tuning_GetExpr shadow read - APPENDED, for the
                   * same reason trend_fast/trend_slow were: every existing
                   * reader indexes by column position. ratio_imp is directly
                   * comparable with the `ratio` column above; they are the same
                   * quantity from two different sources. */
                  "ratio_imp,imp_it,imp_it_max,imp_line_us,imp_expr_us\n", f);
    }
    int64_t v_in = verdict_at > 0 ? (verdict_at - now_ms) / 1000 : -1;
    int64_t h_in = hb_at      > 0 ? (hb_at      - now_ms) / 1000 : -1;
    fprintf(f, "%lld,%d,%.0f,%.0f,%.4f,%.0f,%.0f,%.0f,%lld,%lld,%.0f,%.0f,"
               "%.4f,%d,%d,%d,%d\n",
            (long long)now_ms, cur, (double)sm->d, (double)sm->gain,
            (double)sm->ratio, (double)s, (double)ref, (double)bar,
            (long long)v_in, (long long)h_in,
            (double)ema_fast, (double)ema_slow,
            (double)sm->ratio_imp, sm->imp_it, sm->imp_it_max,
            sm->imp_line_us, sm->imp_expr_us);
    fflush(f);   /* tmpfs, a fraction of a line per second - a crash must not
                  * eat the tail */
    if (ftello(f) > (off_t)DN_TRACE_MAX_BYTES) {
        fclose(f); f = NULL;
        char old[sizeof path + 2];
        snprintf(old, sizeof old, "%s.1", path);
        if (rename(path, old) != 0)
            LOGW(MOD, "trace rotate %s -> %s failed: %s", path, old,
                 strerror(errno));
        /* next decimated sample reopens fresh (and rewrites the header) */
    }
}


static char g_why[80];

/* ------------------------------------------------------------------ *
 * The decision (dev_notes/dn-redesign.md). Day: the day pipeline decides.
 * Night: look without the illuminator now and then; when that looks like
 * day, ask the day pipeline once (a check). */

enum { S_BOOT = -1, S_DAY = 0, S_NIGHT = 1, S_LOOK = 2, S_CHECK = 3 };

#define SD_HOLD_MS    30000     /* an idle day reading must hold this long */
#define SD_JUMP_HOLD_MS 20000   /* a jump must hold this long (12 s headlights do not) */
#define SD_DEEP_MS    6000      /* a deep dark day reading switches after this */
#define SD_JUMP_CHECK 0.50f     /* jump bar where the answer costs a click (no LED) */
#define SD_B_REL      0.85f     /* after a railed check: look again below this share */
#define SD_FAR        4.0f      /* a check this many times night_gain counts as railed */
#define SD_LOCK_MS    60000     /* lockout after the first failed check, doubling */
#define SD_PAIR_MS    60000     /* a look and the check it asked for */
#define SD_RISE_MS    60000     /* a darker scene held this long is the new reference */
#define SD_RISE       1.10f     /* ... starting this far above it (noise stays below) */
#define SD_B_MIN      0.50f     /* a light that halves the dark reading may ask again */
#define SD_LOOK_S     2700      /* 8 s every 45 min = 0.30 % of the night */

/* the AE cannot brighten the image any further */
static int sd_exhausted(const dn_sample *m)
{
    if (dn_clipped(m->headroom)) return 1;
    return m->mag > 0 && m->ag * 100 >= m->mag * 98 && m->dg <= 0 &&
           m->idg <= 0 && m->ratio >= 0.8f;
}

static int sd_watched(void)
{
    return hub_image_subs() + hal_image_pinned() > 0;
}

static void sd_arm(int on)
{
    __atomic_store_n(&g_dni_armed, on, __ATOMIC_RELEASE);
}

static void sd_idle_status(const ms_daynight_cfg *dn, int irp, int idle,
                           int64_t quiet, int64_t now)
{
    static int warned;
    int ka = hal_fs_keepalive();
    const char *why = (!dn->ir_idle_off || !dn->enabled) ? "disabled"
                    : dn->mode == DN_MODE_SCHEDULE ? "schedule"
                    : !ka ? "keepalive"
                    : !irp ? "no_illuminator"
                    : g_dni_hook == DNI_HOOK_WHITE ? "white_light"
                    : g_dni_hook == DNI_HOOK_OLD ? "hook" : "";
    if (dn->ir_idle_off && !ka && !warned++)
        LOGW(MOD, "daynight.ir_idle_off=1 needs the framesource keepalive "
                  "(general.fs_keepalive)");
    int64_t left = (quiet && !idle) ? dn->ir_idle_linger_s * 1000LL - (now - quiet) : 0;
    dn_ir_idle_status st = { dn->ir_idle_off ? 1 : 0, !why[0], idle, sd_watched(),
                             left > 0 ? (int)((left + 999) / 1000) : 0, why };
    pthread_mutex_lock(&g_st_mu);
    int changed = st.available != g_st_idle.available || st.dark != g_st_idle.dark ||
                  st.reason != g_st_idle.reason;
    g_st_idle = st;
    pthread_mutex_unlock(&g_st_mu);
    if (changed) events_notify();
}

static void *dn_thread(void *arg)
{
    (void)arg;
    int     st = S_BOOT, cur = DN_UNKNOWN, enabled_was = 0, ir_fails = 0;
    int     stable = 0, cycled = 0, idle = 0, idle_fails = 0, desync = 0;
    float   s = -1.0f, A = -1.0f, L = -1.0f, N = -1.0f;
    float   B = -1.0f;              /* <0 unset, 0 learn at the next look, >0 bar */
    int64_t boot_at = ms_now_us() / 1000, due = 0, hold = 0, look_at = 0;
    int64_t day_at = 0, mode_at = 0, N_at = 0, verify_at = 0, quiet = 0, idle_at = 0;
    int64_t reassert_at = 0, cycle_at = 0, rise = 0;
    int     reassert_left = 0;
    int64_t lock_until = 0, lock_ms = SD_LOCK_MS;  /* failed checks: 1, 2, 4 ... min */
    const char *why = "boot";

    while (!ms_stopgate_stopped(&g_gate)) {
        ms_daynight_cfg c;
        int rm;
        config_str_lock();
        c = g_cfg.daynight; rm = g_cfg.image.running_mode;
        config_str_unlock();
        if (c.day_gain > c.night_gain) {
            static int said;
            if (!said++)
                LOGW(MOD, "daynight.day_gain %.0f is above night_gain %.0f - swapped",
                     (double)c.day_gain, (double)c.night_gain);
            float t = c.day_gain; c.day_gain = c.night_gain; c.night_gain = t;
        }
        const ms_daynight_cfg *dn = &c;
        int64_t now = ms_now_us() / 1000;
        int64_t look_ms = (int64_t)(dn->heartbeat_s < SD_LOOK_S ? dn->heartbeat_s : SD_LOOK_S) * 1000;
        int irp = dn->irprobe_cmd[0] && !g_ir_unusable;
        int locked = now < lock_until;
        dn_sample sm;
        dn_read(dn, &sm);
        float luma = -1.0f;
        { uint32_t al; if (hal_isp_ae_luma(&al) == 0) luma = (float)al; }

        if (!dn->enabled || dn->mode == DN_MODE_SCHEDULE) {
            if (st == S_LOOK) dn_light_on(dn->irprobe_cmd, 2);
            enabled_was = 0; st = S_BOOT; idle = 0; g_probe_req = 0; sd_arm(0);
            if (dn->enabled) {
                int t = dn_cal_target(dn, time(NULL));
                static int said;
                if (t == DN_UNKNOWN && !said++)
                    LOGW(MOD, "mode=schedule needs time_night_start/time_day_start "
                              "or sun_latitude/sun_longitude, and a set clock");
                if (t != DN_UNKNOWN && t != cur)
                    dn_switch(t, "calendar", dn->switch_cmd, -1, -1, -1);
                if (t != DN_UNKNOWN) cur = t;
            } else cur = DN_UNKNOWN;
            dn_status_update(sm.bright, sm.gain, sm.d, luma, cur, -1, -1, -1);
            goto wait;
        }
        if (!enabled_was) {
            enabled_was = 1; st = S_BOOT; cur = DN_UNKNOWN; s = A = B = N = -1.0f;
            boot_at = now; hold = verify_at = lock_until = 0; lock_ms = SD_LOCK_MS;
            reassert_at = cycle_at = 0; reassert_left = 0;
            desync = cycled = ir_fails = 0; g_ir_unusable = 0;
            LOGI(MOD, "auto day/night enabled (simple engine)");
        }
        if (sm.d > 0.0f) {
            s = (s > 0.0f) ? s + (sm.d - s) * DN_ALPHA : sm.d;
            stable = (fabsf(sm.d - s) < s * DN_STABLE_PCT) ? stable + 1 : 0;
        }
        if (cur == DN_DAY) day_at = now;

        if (reassert_at && now >= reassert_at) {
            hub_control("image.running_mode", rm ? "1" : "0");
            LOGD(MOD, "re-asserting running_mode=%d after switch (%d left)",
                 rm ? 1 : 0, reassert_left - 1);
            if ((cur == DN_NIGHT) != (rm != 0))
                LOGW(MOD, "running_mode=%d never followed the switch to %s",
                     rm ? 1 : 0, cur == DN_NIGHT ? "night" : "day");
            reassert_at = --reassert_left > 0 ? now + DN_REASSERT_MS : 0;
        }

        /* believe a switch once the ISP shows it, else one real edge through
         * the other mode, held 5 s; readings meanwhile are of the wrong optics */
        if (cycle_at) {
            if (now < cycle_at) goto out;
            dn_switch(cur, "isp readback", dn->switch_cmd, s, A, B);
            cycle_at = 0; s = -1.0f; stable = 0;
            verify_at = now + DN_VERIFY_MS;
            reassert_left = DN_REASSERT_COUNT; reassert_at = now + DN_REASSERT_MS;
        }
        if (verify_at && now >= verify_at && st != S_LOOK) {
            verify_at = 0;
            if (sm.isp >= 0 && sm.isp != cur && !cycled) {
                cycled = 1;
                LOGW(MOD, "ISP did not follow the switch to %s - one cycle",
                     cur == DN_NIGHT ? "night" : "day");
                dn_switch(!cur, "isp readback", dn->switch_cmd, s, A, B);
                cycle_at = now + DN_CYCLE_HOLD_MS;
                goto out;
            }
        }
        desync = (!verify_at && sm.isp >= 0 && cur >= 0 && sm.isp != cur);
        if (cycled && sm.isp == cur) cycled = 0;

        if (sm.d <= 0.0f) {
            static int said;
            if (!said++)
                LOGW(MOD, "no exposure reading from %s - detection idle", dn->isp_path);
            if (st == S_BOOT && now - boot_at >= DN_STABLE_MAX_MS) {
                cur = st = rm ? DN_NIGHT : DN_DAY;
                LOGW(MOD, "boot: no exposure reading - asserting the persisted %s",
                     cur == DN_NIGHT ? "night" : "day");
                dn_switch(cur, "boot fallback", dn->switch_cmd, -1, -1, -1);
                mode_at = now;
            }
            goto out;
        }

        switch (st) {
        case S_BOOT:
            if (now - boot_at < DN_BOOT_SETTLE_S * 1000 ||
                (stable < DN_STABLE_N && now - boot_at < DN_STABLE_MAX_MS))
                break;
            why = "boot"; L = -1.0f;
            goto check;
        case S_DAY:
            /* the raw reading starts the hold, so the smoother's lag does not add to it */
            if (sm.d > dn->night_gain || s > dn->night_gain ||
                sd_exhausted(&sm)) {
                if (!hold) hold = now;
                int deep = SD_DEEP_MS && now - mode_at >= DN_PROBE_SETTLE_S * 1000 &&
                           (s >= 2.0f * dn->night_gain || sd_exhausted(&sm));
                if (now - hold >= (deep ? SD_DEEP_MS : (int64_t)dn->day_confirm_s * 1000)) {
                    snprintf(g_why, sizeof g_why, "exposure %.0f%s", (double)s,
                             s > dn->night_gain ? "" : ", AE exhausted");
                    L = -1.0f;
                    goto night;
                }
            } else hold = 0;
            break;
        case S_NIGHT: {
            if (cycled) break;
            if (A < 0.0f && now - mode_at >= DN_REF_DELAY_S * 1000 &&
                stable >= DN_STABLE_N && !sd_exhausted(&sm))
                A = s;
            int jump = 0;
            float jb = irp ? 1.0f - (float)dn->jump_pct / 100.0f : SD_JUMP_CHECK;
            if (A > 0.0f && s > 0.0f && s < A * jb) {
                if (!hold) hold = now;
                jump = now - hold >= SD_JUMP_HOLD_MS;
            } else hold = 0;
            if (jump) B = -1.0f;            /* new light: the bar was for the old one */
            /* the light A (and B) were taken under is gone: else a lamp
             * would have to beat that light, and B would still judge for it */
            if (A > 0.0f && s > A * SD_RISE) {
                if (!rise) rise = now;
            } else if (s < A) rise = 0;
            if (rise && now - rise >= SD_RISE_MS) {
                LOGD(MOD, "reference follows the darker scene: %.0f -> %.0f",
                     (double)A, (double)s);
                A = s; rise = 0;
                if (B > 0.0f) B = 0.0f;
            }
            if (sd_watched()) quiet = 0;
            else if (!quiet) quiet = now;
            if (irp && dn->ir_idle_off && hal_fs_keepalive() && quiet &&
                now - quiet >= (int64_t)dn->ir_idle_linger_s * 1000 &&
                g_dni_hook == DNI_HOOK_OK && !g_probe_req &&
                dn_idle_hook(dn->irprobe_cmd, &idle_fails) == 0) {
                __atomic_store_n(&g_watch_kick, 0, __ATOMIC_RELAXED);
                sd_arm(1);
                idle = 1; why = "idle"; L = s; idle_at = now; hold = rise = 0;
                st = S_LOOK; due = now + DN_PROBE_SETTLE_S * 1000; s = -1.0f; stable = 0;
                break;
            }
            /* a jump waits out a lockout with its old level kept, so it
             * looks the moment the lockout ends */
            if (irp && ((jump && !locked) || now >= look_at || g_probe_req)) {
                why = jump ? "jump" : g_probe_req ? "requested" : "timer";
                g_probe_req = 0;
                goto look;
            }
            if (!irp && jump && !locked) { why = "jump"; L = s; goto check; }
            if (now - (day_at ? day_at : boot_at) >=
                    (int64_t)(irp ? dn->heartbeat_max_s : dn->heartbeat_s) * 1000) {
                why = irp ? "guarantee" : "timer"; L = s;
                goto check;
            }
            break;
        }
        case S_LOOK: {
            if (idle) {
                if (sd_watched() || __atomic_exchange_n(&g_watch_kick, 0, __ATOMIC_RELAXED)) {
                    dn_light_on(dn->irprobe_cmd, 2);
                    hub_set_image_ready_at(ms_now_us() + (int64_t)DN_IR_WAKE_SETTLE_MS * 1000);
                    idle = 0; quiet = 0; sd_arm(0);
                    st = S_NIGHT; A = L; s = -1.0f; stable = 0; look_at = now + look_ms;
                    break;
                }
                if (now - idle_at >= DN_IR_IDLE_REASSERT_S * 1000LL) {
                    dn_idle_hook(dn->irprobe_cmd, &idle_fails);
                    idle_at = now;
                }
            }
            if (now < due || s <= 0.0f) break;
            N = s; N_at = now;
            int ok = !dn_clipped(sm.headroom);
            if (ok && B == 0.0f) B = N * SD_B_REL;
            /* new light may be read darker without the LED than by day */
            float bar = B > 0.0f ? B : dn->day_gain *
                        (why[0] == 'j' ? 2.0f : 1.0f);
            int ask = ok && N < bar;
            if (idle) {
                if (!ask) { hold = 0; break; }
                if (!hold) hold = now;
                if (now - hold < SD_HOLD_MS || locked) break;
                idle = 0; sd_arm(0);
                goto check;
            }
            dn_light_on(dn->irprobe_cmd, 2);
            s = -1.0f; stable = 0;
            LOGD(MOD, "look (%s): lit %.0f dark %.0f bar %.0f -> %s", why,
                 (double)L, (double)N, (double)bar, ask ? "check" : "night");
            if (ask && !locked)
                goto check;
            st = S_NIGHT; hold = 0;
            if (!(ask && why[0] == 'j')) A = L;
            look_at = ask ? lock_until : now + look_ms;
            break;
        }
        case S_CHECK:
            if (now < due) break;
            if (sm.isp >= 0 && sm.isp != DN_DAY && now < due + DN_VERIFY_MS) break;
            if (s < dn->day_gain) {
                st = S_DAY; B = -1.0f; hold = 0; mode_at = now;
                lock_until = 0; lock_ms = SD_LOCK_MS;
                LOGI(MOD, "check (%s): day, exposure %.0f < %.0f", why,
                     (double)s, (double)dn->day_gain);
                break;
            }
            /* where the look predicts day for this scene; a check at or far
             * past the AE limit says nothing about that (Eufy: 40960 without
             * sd_exhausted), then just ask for a darker look */
            if (N > 0.0f && now - N_at < SD_PAIR_MS) {
                B = N * dn->day_gain / s;
                if (B < N * SD_B_REL &&
                    (sd_exhausted(&sm) || s > SD_FAR * dn->night_gain))
                    B = N * SD_B_REL;
                if (B < N * SD_B_MIN) B = N * SD_B_MIN;
            } else B = 0.0f;
            if (why[0] != 'b') { lock_until = now + lock_ms; lock_ms *= 2; }
            LOGI(MOD, "check (%s): night, exposure %.0f", why, (double)s);
            snprintf(g_why, sizeof g_why, "check found night, exposure %.0f", (double)s);
            goto night;
        }
        goto out;

    look:
        L = s; rise = 0;
        if (dn_irprobe(dn->irprobe_cmd, 0) == 0) {
            ir_fails = 0;
            st = S_LOOK; due = now + DN_PROBE_SETTLE_S * 1000; s = -1.0f; stable = 0;
            goto out;
        }
        dn_light_on(dn->irprobe_cmd, 1);
        if (++ir_fails >= DN_IR_MAX_FAILS) g_ir_unusable = 1;
        st = S_NIGHT; look_at = now + look_ms;
        goto out;
    check:
        dn_switch(DN_DAY, why, dn->switch_cmd, s, A, B);
        cur = DN_DAY; st = S_CHECK; mode_at = now; cycled = 0;
        due = now + DN_PROBE_SETTLE_S * 1000; s = -1.0f; stable = 0;
        verify_at = now + DN_VERIFY_MS;
        /* not at boot: running_mode is still the persisted guess */
        reassert_left = why[0] == 'b' ? 0 : DN_REASSERT_COUNT;
        reassert_at = why[0] == 'b' ? 0 : now + DN_REASSERT_MS;
        goto out;
    night:
        dn_switch(DN_NIGHT, g_why, dn->switch_cmd, s, A, B);
        cur = st = DN_NIGHT; mode_at = now; hold = rise = 0; s = -1.0f; stable = 0; cycled = 0;
        /* a check without a fresh look learns B from the same scene, soon */
        A = L; look_at = now + (B == 0.0f ? DN_REF_DELAY_S * 1000LL : look_ms);
        verify_at = now + DN_VERIFY_MS;
        reassert_left = DN_REASSERT_COUNT; reassert_at = now + DN_REASSERT_MS;

    out:
        if (st != S_NIGHT && st != S_LOOK) g_probe_req = 0;
        dn_trace(dn, now, cur, &sm, s, A, B > 0.0f ? B : dn->day_gain, due, look_at, -1.0f, -1.0f);
        dn_status_update(sm.bright, sm.gain, sm.d, luma, cur, A,
                         B > 0.0f ? B : dn->day_gain, desync);
    wait:
        sd_idle_status(dn, irp, idle, quiet, now);
        dn_hist_tick(&sm, luma, cur, dn->history_s);
        if (idle) ms_stopgate_wait_kick(&g_gate, dn->interval_ms > 0 ? dn->interval_ms : 2000);
        else ms_stopgate_wait(&g_gate, dn->interval_ms > 0 ? dn->interval_ms : 2000);
    }
    if (st == S_LOOK) {
        char cmd[sizeof g_cfg.daynight.irprobe_cmd];
        config_str_lock();
        memcpy(cmd, g_cfg.daynight.irprobe_cmd, sizeof cmd);
        config_str_unlock();
        dn_light_on(cmd, 1);
    }
    LOGI(MOD, "detection thread stopped");
    return NULL;
}

void daynight_start(void)
{
    if (g_started) return;
    ms_stopgate_init(&g_gate);
    if (ms_thread_create(&g_thr, MS_STACK_UTIL, dn_thread, NULL) != 0) {
        LOGW(MOD, "cannot start detection thread");
        return;
    }
    g_started = 1;
}

void daynight_stop(void)
{
    if (!g_started) return;
    __atomic_store_n(&g_dni_armed, 0, __ATOMIC_RELEASE);
    ms_stopgate_stop(&g_gate);
    pthread_join(g_thr, NULL);
    g_started = 0;
}

/* see daynight.h: latest measurement for GET /control */
void daynight_get_status(int *enabled, int *mode,
                         float *brightness, float *total_gain, float *exposure,
                         float *ae_luma, float *night_ref, float *probe_bar,
                         int *isp_desync)
{
    pthread_mutex_lock(&g_st_mu);
    float b  = g_st_brightness;
    float tg = g_st_gain;
    float ex = g_st_exposure;
    float lu = g_st_luma;
    int   m  = g_st_mode;
    float rf = g_st_ref;
    float pb = g_st_bar;
    int   ds = g_st_desync;
    pthread_mutex_unlock(&g_st_mu);
    /* F-03: running_mode/daynight.enabled are live-mutable via /control -
     * snapshot under the config string lock instead of reading lock-free. */
    int running_mode, dn_enabled;
    config_str_lock();
    running_mode = g_cfg.image.running_mode;
    dn_enabled   = g_cfg.daynight.enabled;
    config_str_unlock();
    if (m == DN_UNKNOWN)   /* manual mode / before the first auto switch */
        m = running_mode ? DN_NIGHT : DN_DAY;
    if (enabled)    *enabled    = dn_enabled ? 1 : 0;
    if (mode)       *mode       = m;
    if (brightness) *brightness = b;
    if (total_gain) *total_gain = tg;
    if (exposure)   *exposure   = ex;
    if (ae_luma)    *ae_luma    = lu;
    if (night_ref)  *night_ref  = rf;
    if (probe_bar)  *probe_bar  = pb;
    if (isp_desync) *isp_desync = ds;
}

void daynight_ir_idle_status(dn_ir_idle_status *o)
{
    pthread_mutex_lock(&g_st_mu);
    *o = g_st_idle;
    pthread_mutex_unlock(&g_st_mu);
    config_str_lock();
    o->cfg = g_cfg.daynight.ir_idle_off ? 1 : 0;
    config_str_unlock();
}

/* see daynight.h: today's computed sunrise/sunset for the configured
 * lat/long+offsets, as local "HH:MM" strings (for the WebUI readout). */
int daynight_sun_status(char *sr_hhmm, char *ss_hhmm, size_t cap)
{
    /* F-03: sun_latitude/longitude are floats (can tear on MIPS if the compiler
     * splits the load) and the offsets are live-mutable via /control - snapshot
     * all four under the config string lock instead of reading lock-free. */
    float lat, lon; int sr_off, ss_off;
    config_str_lock();
    lat    = g_cfg.daynight.sun_latitude;
    lon    = g_cfg.daynight.sun_longitude;
    sr_off = g_cfg.daynight.sun_sunrise_offset_min;
    ss_off = g_cfg.daynight.sun_sunset_offset_min;
    config_str_unlock();
    time_t now = time(NULL), sr, ss;
    int r = dn_sun_times(lat, lon, now, &sr, &ss);
    if (r != 0) {                       /* polar day/night: no rise/set today */
        if (sr_hhmm && cap) snprintf(sr_hhmm, cap, "%s", "--:--");
        if (ss_hhmm && cap) snprintf(ss_hhmm, cap, "%s", "--:--");
        return 0;
    }
    sr += (time_t)sr_off * 60;
    ss += (time_t)ss_off * 60;
    struct tm lt;
    if (sr_hhmm && cap) { localtime_r(&sr, &lt); snprintf(sr_hhmm, cap, "%02d:%02d", lt.tm_hour, lt.tm_min); }
    if (ss_hhmm && cap) { localtime_r(&ss, &lt); snprintf(ss_hhmm, cap, "%02d:%02d", lt.tm_hour, lt.tm_min); }
    return 1;
}

#else /* !USE_DAYNIGHT */

/* stub so control.c always links: no detection thread -> auto is off, no
 * measurement -> everything unknown, mode from the persisted/live ISP mode */
void daynight_get_status(int *enabled, int *mode,
                         float *brightness, float *total_gain, float *exposure,
                         float *ae_luma, float *night_ref, float *probe_bar,
                         int *isp_desync)
{
    if (enabled) *enabled = 0;
    if (mode){          /* F-03: live-mutable, read under the config string lock */
        config_str_lock();
        int rm = g_cfg.image.running_mode;
        config_str_unlock();
        *mode = rm ? 1 : 0;
    }
    if (brightness) *brightness = -1.0f;
    if (total_gain) *total_gain = -1.0f;
    if (exposure)   *exposure   = -1.0f;
    if (ae_luma)    *ae_luma    = -1.0f;
    if (night_ref)  *night_ref  = -1.0f;
    if (probe_bar)  *probe_bar  = -1.0f;
    if (isp_desync) *isp_desync = -1;
}

int daynight_sun_status(char *sr_hhmm, char *ss_hhmm, size_t cap)
{
    if (sr_hhmm && cap) snprintf(sr_hhmm, cap, "%s", "--:--");
    if (ss_hhmm && cap) snprintf(ss_hhmm, cap, "%s", "--:--");
    return 0;
}

/* no detection thread to arm a probe on */
int daynight_request_probe(void) { return -1; }

void daynight_watch_kick(void) {}

void daynight_ir_idle_status(dn_ir_idle_status *o)
{
    config_str_lock();
    int cfg = g_cfg.daynight.ir_idle_off ? 1 : 0;
    config_str_unlock();
    *o = (dn_ir_idle_status){ cfg, 0, 0, 0, 0, "disabled" };
}

#endif /* USE_DAYNIGHT */
