/* daynight.c - native automatic day/night detection. See daynight.h for the
 * four-path design and dev_notes/DAYNIGHT_REDESIGN_2026-08-17.md for why it
 * is shaped this way; docs/wiki/Day-Night-Design-Notes.md is the incident
 * record that produced it.
 *
 * The short version, because everything below is an instance of it: the day
 * pipeline reports ambient light honestly and the night pipeline does not, so
 * day->night is a plain measurement and night->day is a physical probe. All
 * of the scheduling in this file exists to bound how often that probe costs
 * an audible IR-cut click, and every rationing rule is gated on an
 * independent trigger still working (dn_c_sighted) - because the previous
 * design's nine rationing rules all gated the same single path, and their
 * failures multiplied instead of cancelling.
 *
 * Compiled only with -DUSE_DAYNIGHT; uses nothing but libc + pthread. */
#include "daynight.h"
#include "config.h"
#include "hal/hal.h"   /* hal_isp_total_gain(): ISP gain via the IMP API */
#include <stdio.h>     /* snprintf() - needed by the !USE_DAYNIGHT stub too */

#ifdef USE_DAYNIGHT
#include "hub.h"       /* hub_control(): re-assert running_mode into the ISP */
#include "events.h"    /* wake /events SSE subscribers on real changes */
#include "log.h"
#include "util.h"      /* ms_now_us(): monotonic clock for every deadline */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>    /* F-01: fork/execlp/dup2 instead of system() */
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

#define MOD "DAYNIGHT"

enum { DN_DAY = 0, DN_NIGHT = 1, DN_UNKNOWN = -1 };

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
/* Post-switch re-assert, defense-in-depth. The board hook chain (switch_cmd
 * -> 'color' script -> POST /control) is fire-and-forget, so a lost POST would
 * otherwise leave the decision and the ISP silently desynced until the next
 * transition. SetISPRunningMode is idempotent, so a couple of extra re-drives
 * cost nothing. */
#ifndef DN_REASSERT_MS
#define DN_REASSERT_MS 8000
#endif
#ifndef DN_REASSERT_COUNT
#define DN_REASSERT_COUNT 2
#endif
/* Post-switch readback: believe a switch only once the ISP itself reports
 * the new mode (dn_read sees the line every tick anyway). If it has not
 * after the whole assert chain (script + both re-asserts) had time, force
 * ONE real transition through the counter mode - re-asserting an already-
 * believed value is a no-op to a stuck ISP, only an edge acts (cam-wohn
 * 2026-08-21; same rule as the railed-boot cycle). One cycle, then give up
 * loudly: a second identical cycle has no new mechanism, and a permanently
 * stuck ISP must not turn this into a click generator. Cost trade-off in
 * CHANGELOG.md 2026-08-21. */
#ifndef DN_VERIFY_MS
#define DN_VERIFY_MS (DN_REASSERT_COUNT * DN_REASSERT_MS + 2000)
#endif
#ifndef DN_VERIFY_HOLD_MS
#define DN_VERIFY_HOLD_MS 5000
#endif
#define DN_VERIFY_MAX_CYCLES 1
/* Standing disagreement notice: cur vs the ISP readback OUTSIDE the verify
 * window. Report only, never enforce - the mismatch may be a deliberate
 * manual override (board script, /control), and forcing it back would
 * clobber exactly that. Debounced well past any switch transient; once per
 * episode, reset when they agree again. Resolution path for the operator:
 * a requested probe, which makes the automaton re-measure and decide. */
#ifndef DN_DESYNC_MS
#define DN_DESYNC_MS 20000
#endif
/* "has anything happened since the last probe that could mean day?" - the
 * question the heartbeat deferral turns on. Answered by the SUSTAINED minimum
 * of the smoothed exposure (see the tumbling window in the night branch)
 * against the proven night reference: a scene that has been at least this
 * much brighter, and stayed there for a full probe_confirm_s, has produced
 * new evidence and is worth a click.
 *
 * It has to be the sustained minimum and not a bare running one. That is the
 * one mechanism deliberately carried over from the pre-redesign machine,
 * because the corpus proves the naive version wrong in both directions: a
 * bare minimum of a noisy static scene (03-noisy-night runs +-25% AGC
 * jitter) descends without bound and defeats the deferral outright, while a
 * plain max/min RANGE test misses a genuine permanent step - 11-dim-lightson
 * moves the night pipeline 2000 -> 1750, a 12.5% shift that any range
 * threshold loose enough to survive the noise would call "flat". The
 * tumbling window separates the two properly: noise cannot hold a level, a
 * step can. */
#ifndef DN_MOVED_MARGIN
#define DN_MOVED_MARGIN 0.90f
#endif
/* ---- the trend trigger (path T) ---------------------------------------- *
 * Two further EMAs of the exposure index, on top of the tick smoother `s`:
 * a FAST one that follows the scene and a SLOW one that is the scene's
 * memory. Their ratio answers the one relative question the night pipeline
 * is entitled to answer - "has this got sustainedly brighter than it has
 * been" - which no absolute level can, because "day" spans a factor of 63
 * across this fleet at one instant.
 *
 * Why not reuse `s`: its constant is DN_ALPHA at the tick, i.e. ~7 s at the
 * 2 s default. That is a de-noiser, not a memory; divided by an hour-long
 * EMA it would carry the AGC jitter straight into the ratio.
 *
 * The numbers are measured, not guessed. Sweep over the fleet night of
 * 2026-08-17/18 (private/messungen/2026-08-18_daemmerung, 12 cameras, 181
 * camera-hours with the camera actually in night mode, sampled at 1/min -
 * the daemon's 2 s tick makes the fast side smoother still, so every
 * false-fire figure here is an UPPER bound):
 *
 *   tau fast/slow   bar    dawns found   false fires per camera-hour
 *   3 / 60 min      75 %   10 of 12      0.22
 *   3 / 60 min      70 %    9 of 12      0.15
 *   3 / 15 min      75 %    8 of 12      0.19  (and 30-70 min later)
 *   3 / 10 min      75 %    6 of 12      0.13
 *
 * 3/60 at 75 % finds the most dawns and finds them earliest, and it
 * reproduces the sweep that first chose it on the previous night (7 of 8
 * twilights, 0.13 false fires per camera-hour). The 15- and 10-minute rows
 * show what goes wrong on the way down: natural twilight is slow - a factor
 * of 2.2 over 67 minutes - so the slow side has to be a real memory and not
 * a rate measure, or it tracks the twilight instead of noticing it. The two
 * dawns nobody finds are a permanently dark outbuilding and a camera whose AE
 * never leaves its rail; both are heartbeat-carried by construction.
 *
 * Most of that 0.22 comes from two cameras that spent the whole daylight
 * period stuck in night mode - where a "false" fire is the correct one - so
 * it is an over-count in the same, safe direction.
 *
 * NO THIRD (medium, ~10 min) CONSTANT, though one was proposed after the
 * same night: an interior light switched on mid-dusk fell inside it (7845 ->
 * 5087, a factor of 1.54) and so registered against neither the jump bar
 * (which needs a factor of 2) nor the hour-long memory, which the dusk had
 * left far BELOW the current level - the ratio bottomed at 1.15, never
 * anywhere near firing. The observation is real; the proposed remedy is
 * refuted by the same measurement. On that event a 10-minute EMA bottoms at
 * 0.87, still short of the 75 % bar, and the ~88 % bar it would take sits
 * inside the +-25 % AGC noise band: measured, that doubles the false-fire
 * rate to 0.44 per camera-hour while STILL finding fewer dawns (9 of 12)
 * than 3/60 alone. A 1.54x change is what the heartbeat is for. */
#ifndef DN_TREND_FAST_MS
#define DN_TREND_FAST_MS   180000     /* tau 3 min  - follows the scene */
#endif
#ifndef DN_TREND_SLOW_MS
#define DN_TREND_SLOW_MS  3600000     /* tau 60 min - remembers it */
#endif
#ifndef DN_TREND_PCT
#define DN_TREND_PCT 75               /* fire below this much of the memory */
#endif
/* the [24.8] gain floor (1.0x). Only used to answer "can the trigger see?"
 * when the integration-time half of the metric is unavailable - see
 * dn_c_sighted(). */
#define DN_GAIN_FLOOR 256.0f
/* lower clamp on the integration-time ratio, so a bogus 0 from the scrape
 * cannot collapse the metric to zero. */
#define DN_RATIO_MIN (1.0f / 4096.0f)
/* wall clock is only trusted from 2020 on - a camera without NTP boots into
 * 1970 and must not act on a "calendar" derived from it. */
#define DN_CLOCK_SANE 1577836800L

/* ---- learning ---------------------------------------------------------- */
#define DN_LEARN_SLOTS 8         /* day excursions kept for the median */
#define DN_LEARN_MIN   3         /* ... before the median is used at all */
#define DN_LEARN_M     1.6f      /* headroom above the median, ~2/3 stop */
#define DN_LEARN_SAVE_MS 3600000 /* at most one state write per hour */
#define DN_LEARN_LOG_MS  86400000            /* the daily summary line */
#define DN_LEARN_LOG_FIRST_MS 60000          /* ... plus one shortly after start */
#define DN_UNREACHABLE 1.5f      /* "clear of the threshold" for the diagnostic */

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

typedef struct {
    float d;       /* THE exposure index (higher = darker); <0 = unknown */
    float gain;    /* total_gain, IMP [24.8] linear (256 = 1x); <0 = unknown */
    float ratio;   /* integration_time / max; <0 = the fields were unreadable */
    float bright;  /* 0..100 %, status readout only; <0 = unknown */
    /* How much room the AE still has, in log2 units (32 = one stop): the
     * unused gain below each ceiling, plus a stop's worth if the integration
     * time is not railed either. <0 = the maxima were not readable.
     *
     * This is what makes a ratio reading trustworthy. An AE with nothing left
     * cannot respond to the illuminator going off, so it returns r ~= 1 -
     * indistinguishable from daylight. Measured on a pitch-dark outbuilding:
     * r = 1.14, below ir_ratio_day, and only the reserve (1 unit, i.e. none)
     * separates that from a genuinely lit room. */
    int   headroom;
    int   isp;     /* mode the ISP itself reports: DN_DAY/DN_NIGHT/DN_UNKNOWN */
} dn_sample;

/* One sample of the ISP exposure state.
 *
 * The gain half comes from IMP_ISP_Tuning_GetTotalGain where the platform has
 * it (robust, and the same number prudynt/raptor and the WebUI plot), falling
 * back to the /proc dump's own gain fields. The integration-time half has no
 * IMP accessor at all, so the /proc scrape now runs on every decision tick
 * rather than only for the status readout - which is what the 2 s default
 * interval_ms pays for.
 *
 * The isp-m0 gain fields are in the IMP log2 unit (0 = 1x, 32 = 2x, per the
 * SetMaxAgain/SetMaxDgain docs), so linear = 2^(units/32) and the analog,
 * sensor-digital and ISP-digital parts add in log space.
 * NOTE: sscanf on the exact-prefix format quietly skips the "MAX SENSOR
 * analog gain" style maximum lines that strstr also matches. */
static void dn_read(const ms_daynight_cfg *dn, dn_sample *o)
{
    o->d = o->gain = o->ratio = o->bright = -1.0f;
    o->headroom = -1;
    o->isp = DN_UNKNOWN;

    { uint32_t hg; if (hal_isp_total_gain(&hg) == 0) o->gain = (float)hg; }

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
     * read. Falling back costs one failed fopen per tick on a camera where the
     * default is wrong, and nothing at all where it is right. */
    static const char *const ALT[] = { "/proc/jz/isp/isp_info", NULL };
    static const char *used;            /* single-caller: the detection thread */
    FILE *fp = fopen(dn->isp_path, "r");
    if (!fp) {
        for (int i = 0; ALT[i] && !fp; i++) {
            if (!strcmp(ALT[i], dn->isp_path)) continue;
            fp = fopen(ALT[i], "r");
            if (fp && used != ALT[i]) {
                used = ALT[i];
                LOGW(MOD, "%s is not readable, using %s instead - set "
                          "daynight.isp_path to silence this",
                     dn->isp_path, ALT[i]);
            }
        }
    } else if (used) {
        used = NULL;                    /* the configured path came back */
    }
    if (fp) {
        char line[256];
        int it = -1, mit = -1, ag = -1, dg = -1, idg = -1, cb = -1;
        int mag = -1, midg = -1;          /* the ceilings, for the reserve */
        char m[32] = {0};

        while (fgets(line, sizeof line, fp)) {
            if (strstr(line, "ISP Runing Mode :"))
                sscanf(line, "ISP Runing Mode : %31s", m);
            else if (strstr(line, "SENSOR Integration Time :"))
                sscanf(line, "SENSOR Integration Time : %d lines", &it);
            else if (strstr(line, "SENSOR Max Integration Time :"))
                sscanf(line, "SENSOR Max Integration Time : %d lines", &mit);
            else if (strstr(line, "MAX SENSOR analog gain :"))
                sscanf(line, "MAX SENSOR analog gain : %d", &mag);
            else if (strstr(line, "MAX ISP digital gain :"))
                sscanf(line, "MAX ISP digital gain : %d", &midg);
            else if (strstr(line, "SENSOR analog gain :"))
                sscanf(line, "SENSOR analog gain : %d", &ag);
            else if (strstr(line, "SENSOR digital gain :"))
                sscanf(line, "SENSOR digital gain : %d", &dg);
            else if (strstr(line, "ISP digital gain :"))
                sscanf(line, "ISP digital gain : %d", &idg);
            else if (strstr(line, "Brightness :"))
                sscanf(line, "Brightness : %d", &cb);
        }
        fclose(fp);

        if      (!strcmp(m, "Day"))   o->isp = DN_DAY;
        else if (!strcmp(m, "Night")) o->isp = DN_NIGHT;

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
        if (mit <= 0 && it > 0) {
            if (it > g_int_hwm) g_int_hwm = it;
            mit = g_int_hwm;
        }
        if (mag >= 0 && ag >= 0) {
            o->headroom = (mag - ag) + (midg >= 0 && idg >= 0 ? midg - idg : 0);
            if (o->headroom < 0) o->headroom = 0;
            if (it > 0 && mit > 0 && it < mit * 9 / 10) o->headroom += 32;
        }

        if (it >= 0 && mit > 0) {
            float r = (float)it / (float)mit;
            if (r > 1.0f) r = 1.0f;
            if (r < DN_RATIO_MIN) r = DN_RATIO_MIN;
            o->ratio = r;
        }

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
            if      (!strcmp(m, "Day"))   o->bright = 75.0f;
            else if (!strcmp(m, "Night")) o->bright = 25.0f;
        }
    }

    if (o->gain > 0.0f) {
        o->d = (o->ratio > 0.0f) ? o->gain * o->ratio : o->gain;
        if (!isfinite(o->d) || o->d <= 0.0f) o->d = -1.0f;
    }
}

/* EMA coefficient for a TIME CONSTANT rather than a per-tick step: alpha =
 * dt/tau, clamped at 1 so a tick longer than the constant simply adopts the
 * sample. Written this way so daynight.interval_ms can be retuned without
 * silently retuning the filter with it - the 3/60 minute pair below was swept
 * at one sample per minute and has to mean the same thing at the 2 s default.
 * Identical form to scripts/dn-trend-eval.py's alpha(), so the sweep and the
 * daemon filter the same signal the same way. */
static float dn_ema_alpha(int dt_ms, int tau_ms)
{
    if (tau_ms <= 0) return 1.0f;
    float a = (float)dt_ms / (float)tau_ms;
    return a > 1.0f ? 1.0f : a;
}

/* Whether the spontaneous-brightening trigger (path C) can actually see.
 *
 * This is the runtime form of the design's one construction rule: a rationing
 * rule may only be applied while the independent trigger it defers to is
 * working. The trigger needs the metric to have room BELOW the current night
 * reference. With the integration-time ratio available it always does (the
 * index runs orders of magnitude under the gain floor). Without it the index
 * degrades to bare gain, whose hard floor is 256 - so a scene resting near
 * that floor under its own illuminator (cam-J rests at 256-268, a
 * camera ~30 cm from a reflective object) can never produce the fall the
 * trigger needs, and the heartbeat must not be stretched on its behalf. */
static int dn_c_sighted(const dn_sample *sm, const ms_daynight_cfg *dn, float ref)
{
    if (sm->ratio > 0.0f) return 1;
    if (ref <= 0.0f)      return 1;       /* nothing anchored yet */
    return ref * (float)dn->probe_jump_pct / 100.0f > DN_GAIN_FLOOR;
}

/* A reading from a railed AE is a clip, not a level. It may prove night -
 * a meter pegged at the DARK end cannot happen in daylight - but it must
 * never be remembered: not as the reference, not as a learned cost. r from
 * two clipped readings is no information either (measured: r=1.00 on a
 * meter whose scene was worth 23x less). Unknown headroom counts as usable,
 * or platforms without ceiling fields could never anchor at all. */
static int dn_clipped(int headroom, const ms_daynight_cfg *dn)
{
    return headroom >= 0 && headroom < dn->ir_min_headroom;
}

/* Say so, once per session, when the ISP dump carries no gain ceilings.
 * Without them the AE reserve is unknown, dn_clipped() must count unknown
 * as usable, and nothing on this camera protects the reference or the
 * learned values from a railed meter - a silent absence the operator would
 * otherwise only meet as hr=-1 in a probe line. Sibling of dn_blind_check().
 * Three consecutive misses arm it, so one torn /proc read cannot. */
static void dn_ceiling_check(const dn_sample *sm, int *miss, int *warned)
{
    if (sm->headroom >= 0) { *miss = 0; return; }
    if (*warned || ++*miss < 3) return;
    *warned = 1;
    LOGW(MOD, "the ISP dump reports no gain ceilings (MAX SENSOR analog "
              "gain / MAX ISP digital gain), so the AE reserve is unknown "
              "here: a railed meter cannot be told from a dark scene, the "
              "railed-boot re-tune never fires, and the night reference and "
              "learned values are NOT protected against clipped readings. "
              "Ratio probes without a clear answer fall back to the audible "
              "probe on this camera");
}

/* Say so, once, when path C is structurally blind on this camera. Called
 * from BOTH places a reference is set - the delayed anchor after entering
 * night AND a probe that proved night - because which of the two runs is an
 * accident of how the camera got into night, and the operator-visible fact
 * ("this camera is carried by the heartbeat alone") is the same either way.
 * Missing the probe-proven path is exactly what the corpus's inverted-regime
 * scenario caught. */
static void dn_blind_check(const dn_sample *sm, const ms_daynight_cfg *dn,
                           float ref, int *warned)
{
    if (*warned || dn_c_sighted(sm, dn, ref)) return;
    *warned = 1;
    LOGW(MOD, "the night reference sits at the sensor's gain floor and no "
              "integration-time reading is available, so a brightening scene "
              "cannot be detected here - the heartbeat carries this camera "
              "alone. If this sensor does report SENSOR Integration Time, "
              "check daynight.isp_path");
}

/* ------------------------------------------------------------------ *
 * Driving the board                                                   */

/* run "<switch_cmd> day|night" (the thingino board script: ircut/light/color).
 * The mode change is committed even if the command fails so a missing script
 * warns once per switch instead of retrying every sample. */
/* The tail is machine-readable on purpose. Everything in it is already in
 * the surrounding prose, but the dashboards count these lines with a grep,
 * and a reworded sentence would empty them silently - the same failure the
 * exposure series had when it stopped without saying so. */
static void dn_switch(int mode, const char *why, const char *cmd,
                      float s, float ref, float bar)
{
    const char *arg = (mode == DN_NIGHT) ? "night" : "day";
    LOGI(MOD, "switching to %s (%s): %s %s [mode=%s exp=%.0f ref=%.0f bar=%.0f]",
         arg, why, cmd, arg, arg, (double)s, (double)ref, (double)bar);
    /* F-01: fork()+execlp() instead of system(). switch_cmd comes from the
     * config file; system() would let a value like "reboot; nc ..." inject
     * shell commands (as root). exec'ing it as a single program with the fixed
     * arg "day"/"night" removes the shell entirely - a malicious value just
     * fails to exec, it cannot inject. */
    pid_t pid = fork();
    if (pid < 0){ LOGW(MOD,"daynight: fork failed: %s", strerror(errno)); return; }
    if (pid == 0){
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0){ dup2(nul,1); dup2(nul,2); if (nul>2) close(nul); }
        execlp(cmd, cmd, arg, (char*)NULL);
        _exit(127);              /* exec failed (script missing / not a program) */
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    int rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    if (rc != 0)
        LOGW(MOD, "'%s %s' failed (rc=%d) - is the script installed?", cmd, arg, rc);
}


/* Drive the IR illuminator alone: "<irprobe_cmd> on|off".
 *
 * Deliberately a different command from switch_cmd, because on the hardware
 * these are different mechanisms with different costs. switch_cmd moves a
 * motor - audible, mechanical, once or twice a day. This writes a GPIO -
 * silent, unlimited, and the only thing it costs is a few seconds of dimmer
 * night image. Conflating them would throw away the entire reason the ratio
 * measurement is affordable.
 *
 * Same fork+execlp discipline as dn_switch: never a shell, so a hostile
 * config value fails to exec instead of injecting. Returns 0 when the command
 * ran and succeeded; anything else means the caller must fall back to the
 * audible probe rather than assume the illuminator moved. */
/* Set by /control, cleared by the automaton when it acts on it. One flag, no
 * queue: two requests a second apart mean one probe, which is what the caller
 * wants and what the illuminator can stand. */
static volatile int g_probe_req = 0;
/* The silent probe is armed by configuration but only kept armed by evidence.
 * A board that cannot switch its illuminator separately answers every attempt
 * with a failure, and each of those costs an audible IR-cut probe instead -
 * worse than never having tried. Two consecutive failures retire it for the
 * session; the design's fallback (path C plus the heartbeat) is what remains.
 * Not persisted: a restart re-tests, which is the cheap direction to be wrong in. */
static volatile int g_ir_unusable = 0;

int daynight_request_probe(void)
{
    if (!g_cfg.daynight.irprobe_cmd[0] || g_ir_unusable) return -1;
    g_probe_req = 1;
    return 0;
}

static int dn_irprobe(const char *cmd, int on)
{
    if (!cmd || !cmd[0]) return -1;
    const char *arg = on ? "on" : "off";
    pid_t pid = fork();
    if (pid < 0) { LOGW(MOD, "irprobe: fork failed: %s", strerror(errno)); return -1; }
    if (pid == 0) {
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) { dup2(nul,1); dup2(nul,2); if (nul>2) close(nul); }
        execlp(cmd, cmd, arg, (char*)NULL);
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    int rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    if (rc != 0)
        LOGW(MOD, "'%s %s' failed (rc=%d) - silent probe unavailable, "
                  "falling back to the IR-cut probe", cmd, arg, rc);
    return rc == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ *
 * The calendar. It NEVER decides in auto mode - it only tells the
 * heartbeat when looking is worth more than usual. In schedule mode it is
 * the whole decision.                                                  */

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
 * base (epoch seconds); gmtime_r is used ONLY to locate the UTC calendar day
 * of `now` (floored to UTC midnight - a few minutes of skew right at UTC
 * midnight is acceptable for light scheduling). Returns 0 with sr_out/ss_out
 * set on a normal day, +1 for polar day, -1 for polar night. */
static int dn_sun_times(float lat, float lon, time_t now,
                        time_t *sr_out, time_t *ss_out)
{
    struct tm g;
    gmtime_r(&now, &g);
    time_t midnight = now - (g.tm_hour * 3600 + g.tm_min * 60 + g.tm_sec);

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
    if (cosw < -1.0) return +1;        /* sun always up   -> permanent day   */
    if (cosw >  1.0) return -1;        /* sun always down -> permanent night */
    double w0 = acos(cosw) * R2D;
    double jrise = Jtransit - w0 / 360.0;
    double jset  = Jtransit + w0 / 360.0;
    if (sr_out) *sr_out = (time_t)((jrise - 2440587.5) * 86400.0 + 0.5);
    if (ss_out) *ss_out = (time_t)((jset  - 2440587.5) * 86400.0 + 0.5);
    return 0;
}

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
    if (dn->time_night_start[0] && dn->time_day_start[0] &&
        dn_hhmm_min(dn->time_night_start) >= 0 &&
        dn_hhmm_min(dn->time_day_start)   >= 0) return DN_CAL_TIME;
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
        sr += (time_t)dn->sun_sunrise_offset_min * 60;
        ss += (time_t)dn->sun_sunset_offset_min  * 60;
        return (wall >= sr && wall < ss) ? DN_DAY : DN_NIGHT;
    }
    default: return DN_UNKNOWN;
    }
}

/* Seconds until the calendar's next day edge, or -1 when there is no
 * calendar (or a polar day/night, where there is no edge to wait for). Used
 * only to pull the heartbeat IN, never to push it out. */
static int64_t dn_secs_to_dawn(const ms_daynight_cfg *dn, time_t wall)
{
    switch (dn_cal_kind(dn, wall)) {
    case DN_CAL_TIME: {
        int d = dn_hhmm_min(dn->time_day_start);
        struct tm lt; localtime_r(&wall, &lt);
        int cur = lt.tm_hour * 60 + lt.tm_min;
        int mins = d - cur;
        if (mins <= 0) mins += 24 * 60;
        return (int64_t)mins * 60 - lt.tm_sec;
    }
    case DN_CAL_SUN: {
        time_t sr, ss;
        if (dn_sun_times(dn->sun_latitude, dn->sun_longitude, wall, &sr, &ss) != 0)
            return -1;
        sr += (time_t)dn->sun_sunrise_offset_min * 60;
        if (sr <= wall) {              /* today's is past: take tomorrow's */
            if (dn_sun_times(dn->sun_latitude, dn->sun_longitude,
                             wall + 86400, &sr, &ss) != 0) return -1;
            sr += (time_t)dn->sun_sunrise_offset_min * 60;
        }
        return (int64_t)(sr - wall);
    }
    default: return -1;
    }
}

/* ------------------------------------------------------------------ *
 * Learning (daynight.learn, default off). See the `learn` comment in
 * config.h for the two safety rules; this is their implementation.    */

typedef struct {
    float   slot[DN_LEARN_SLOTS];  /* lowest exposure of the last N day excursions */
    int     n;                     /* slots filled */
    int     idx;                   /* next write */
    int     dirty;
    int64_t saved_ms;
} dn_learn;

static float dn_learn_median(const dn_learn *l)
{
    if (l->n <= 0) return -1.0f;
    float v[DN_LEARN_SLOTS];
    memcpy(v, l->slot, (size_t)l->n * sizeof v[0]);
    for (int i = 1; i < l->n; i++) {         /* insertion sort, n <= 8 */
        float k = v[i]; int j = i - 1;
        while (j >= 0 && v[j] > k) { v[j+1] = v[j]; j--; }
        v[j+1] = k;
    }
    return (l->n & 1) ? v[l->n / 2] : (v[l->n/2 - 1] + v[l->n/2]) * 0.5f;
}

static void dn_learn_add(dn_learn *l, float v)
{
    if (!(v > 0.0f) || !isfinite(v)) return;
    l->slot[l->idx] = v;
    l->idx = (l->idx + 1) % DN_LEARN_SLOTS;
    if (l->n < DN_LEARN_SLOTS) l->n++;
    l->dirty = 1;
}

/* The effective day threshold. L1: only ever RAISE the configured value - a
 * too-generous threshold produces a false day, which the honest day->night
 * measurement corrects within day_confirm_s, while a too-strict one makes day
 * unconfirmable, which is the failure with no bound. L2: clamp below
 * night_gain/2 so the two thresholds can never cross and oscillate. */
static float dn_day_bar(const ms_daynight_cfg *dn, const dn_learn *l)
{
    float bar = dn->day_gain;
    if (!dn->learn || l->n < DN_LEARN_MIN) return bar;
    float m = dn_learn_median(l);
    if (!(m > 0.0f)) return bar;
    float want = m * DN_LEARN_M;
    if (want > bar) bar = want;
    float cap = dn->night_gain * 0.5f;
    if (bar > cap)          bar = cap;
    if (bar < dn->day_gain) bar = dn->day_gain;
    return bar;
}

static void dn_learn_load(const char *path, dn_learn *l)
{
    memset(l, 0, sizeof *l);
    if (!path || !path[0]) return;
    FILE *f = fopen(path, "r");
    if (!f) return;                    /* absent is normal, not an error */
    char line[256];
    int ver = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "v ", 2)) ver = atoi(line + 2);
        else if (ver == 1 && !strncmp(line, "day_ref ", 8)) {
            const char *p = line + 8;
            while (l->n < DN_LEARN_SLOTS) {
                char *e; float v = strtof(p, &e);
                if (e == p) break;
                p = e;
                if (v > 0.0f && isfinite(v)) l->slot[l->n++] = v;
            }
            l->idx = l->n % DN_LEARN_SLOTS;
        }
    }
    fclose(f);
    /* A state file we do not understand is discarded silently rather than
     * half-applied: nothing in here is worth risking a camera over, and the
     * next day excursion refills it. */
    if (ver != 1) memset(l, 0, sizeof *l);
    else if (l->n)
        LOGI(MOD, "loaded %d learned day levels from %s (median %.0f)",
             l->n, path, (double)dn_learn_median(l));
}

static void dn_learn_save(const ms_daynight_cfg *dn, dn_learn *l, int64_t now_ms)
{
    if (!dn->learn || !dn->state_path[0] || !l->dirty) return;
    if (l->saved_ms && now_ms - l->saved_ms < DN_LEARN_SAVE_MS) return;
    l->saved_ms = now_ms;              /* also the retry clock on failure */

    char tmp[sizeof dn->state_path + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", dn->state_path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        LOGW(MOD, "cannot write %s: %s", tmp, strerror(errno));
        return;
    }
    fputs("v 1\nday_ref", f);
    for (int i = 0; i < l->n; i++) fprintf(f, " %.0f", (double)l->slot[i]);
    fputc('\n', f);
    int bad = ferror(f);
    if (fclose(f) != 0) bad = 1;
    if (bad) { LOGW(MOD, "cannot write %s: %s", tmp, strerror(errno));
               unlink(tmp); return; }
    if (rename(tmp, dn->state_path) != 0) {
        LOGW(MOD, "cannot replace %s: %s", dn->state_path, strerror(errno));
        unlink(tmp);
        return;
    }
    l->dirty = 0;
}

/* The once-a-day summary. Emitted whether or not learning is ENABLED - the
 * point is that the numbers can be read off a running camera before deciding
 * to switch it on. */
static void dn_learn_log(const ms_daynight_cfg *dn, const dn_learn *l)
{
    char buf[96];
    int o = 0;
    for (int i = 0; i < l->n && o < (int)sizeof buf - 8; i++)
        o += snprintf(buf + o, sizeof buf - (size_t)o, "%s%.0f",
                      i ? " " : "", (double)l->slot[i]);
    if (!l->n) snprintf(buf, sizeof buf, "-");

    float bar = dn_day_bar(dn, l);
    const char *state = !dn->learn        ? "not applied (daynight.learn=0)"
                      : l->n < DN_LEARN_MIN ? "not applied yet (needs 3 samples)"
                      : bar > dn->day_gain  ? "APPLIED"
                                            : "no change (configured value is looser)";
    LOGI(MOD, "learned: %d day excursion(s) [%s], median %.0f, "
              "effective day_gain %.0f vs configured %.0f - %s",
         l->n, buf, (double)dn_learn_median(l), (double)bar,
         (double)dn->day_gain, state);
}

/* The unreachable-threshold diagnostic.
 *
 * A SINGLE failed probe cannot tell "this room is dark" from "day_gain is set
 * below anything this scene can produce" - both look like a day-pipeline
 * reading above the bar, which is why the pre-redesign version of this warning
 * had to fire per probe and then repeat forever. Waiting for the daily
 * learning line is the opposite failure: a misconfigured camera would sit
 * silent for 24 h. So the evidence used is DN_DIAG_FAILS consecutive failed
 * probes whose best reading never once came close - which arrives within a few
 * probe intervals, cannot be produced by one unlucky measurement, and is
 * genuinely ambiguous only between the two causes the message names. Latched:
 * once per session, not once per probe. */
#define DN_DIAG_FAILS 3
static void dn_diag_threshold(const ms_daynight_cfg *dn, int fails,
                              float probe_best, int day_seen, int *warned)
{
    if (*warned || !dn->diagnose_thresholds || day_seen) return;
    if (fails < DN_DIAG_FAILS || !(probe_best > 0.0f)) return;
    if (probe_best <= dn->day_gain * DN_UNREACHABLE) return;
    *warned = 1;
    LOGW(MOD, "no probe has ever confirmed day (%d in a row found night): the "
              "best day-pipeline exposure seen was %.0f but daynight.day_gain "
              "is %.0f. Either this scene never gets bright, or the threshold "
              "is too strict - raise daynight.day_gain above %.0f (keeping "
              "night_gain well above that), or set daynight.learn=1 to have it "
              "raised automatically",
         fails, (double)probe_best, (double)dn->day_gain, (double)probe_best);
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
        /* trend_fast/trend_slow are APPENDED, never inserted: every existing
         * reader of this file indexes by column position, and the trend pair
         * is a diagnostic rather than something the older columns depend on.
         * -1 = not seeded (not in night, or a probe in flight). */
        if (ftello(f) == 0)
            fputs("t_mono_ms,cur,d,gain,ratio,smooth,ref,bar,"
                  "verdict_in_s,heartbeat_in_s,trend_fast,trend_slow\n", f);
    }
    int64_t v_in = verdict_at > 0 ? (verdict_at - now_ms) / 1000 : -1;
    int64_t h_in = hb_at      > 0 ? (hb_at      - now_ms) / 1000 : -1;
    fprintf(f, "%lld,%d,%.0f,%.0f,%.4f,%.0f,%.0f,%.0f,%lld,%lld,%.0f,%.0f\n",
            (long long)now_ms, cur, (double)sm->d, (double)sm->gain,
            (double)sm->ratio, (double)s, (double)ref, (double)bar,
            (long long)v_in, (long long)h_in,
            (double)ema_fast, (double)ema_slow);
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

/* ------------------------------------------------------------------ */

/* block until the interval elapses OR daynight_stop() requests stop */
static void dn_sleep(int ms) { ms_stopgate_wait(&g_gate, ms); }

static void *dn_thread(void *arg)
{
    (void)arg;
    /* ---- the entire state of the automaton. Seventeen scalars, no ring
     * buffers, and every one of them has a single meaning that can be stated
     * in one line - which is the point of the redesign as much as the line
     * count is. (Fourteen before the trend pair; the three it costs are the
     * only state the 2026-08-17 decision note added to this loop.) ---- */
    int     cur         = DN_UNKNOWN;  /* mode as switched by US */
    int64_t mode_since  = 0;           /* for transition_s */
    float   s           = -1.0f;       /* EMA of the exposure index */
    int     stable_n    = 0;           /* consecutive on-EMA samples */
    float   ref         = -1.0f;       /* PROVEN night level (see the header) */
    int64_t ref_due     = 0;           /* earliest anchor time after night entry */
    int64_t trig_since  = 0;           /* path C hold start */
    int64_t dark_since  = 0;           /* path A hold start */
    int64_t last_probe  = 0;           /* click budget */
    int64_t verdict_at  = 0;           /* probe in flight -> judge at */
    float   pre_probe   = -1.0f;       /* night level the in-flight probe left */
    int     pre_probe_hr= -1;          /* ... and the AE reserve it was read at */
    int64_t hb_at       = 0;           /* next heartbeat check */
    /* the silent probe: illuminator off, read, illuminator on. d_lit is the
     * reading taken immediately before it went off; ir_verdict_at is when the
     * dark reading is due. 0 = no silent probe in flight. */
    int64_t ir_verdict_at = 0;
    float   d_lit         = -1.0f;
    int     d_lit_hr      = -1;        /* AE reserve the lit reading was taken at */
    const char *ir_why    = NULL;
    float   sust_min    = -1.0f;       /* sustained brightest since the last probe */
    float   win_max     = -1.0f;       /* ... its tumbling window */
    int64_t win_at      = 0;
    /* path T, the trend pair (see DN_TREND_*). -1 = needs seeding, which is
     * also how they are re-anchored after every verdict. */
    float   ema_fast    = -1.0f;       /* tau DN_TREND_FAST_MS  - the scene */
    float   ema_slow    = -1.0f;       /* tau DN_TREND_SLOW_MS  - its memory */
    int64_t trend_since = 0;           /* path T hold start */
    /* ---- bookkeeping that is not part of the decision ---- */
    int     was_enabled = 0;
    int     warned_noisp = 0, warned_nocal = 0, hb_defer_logged = 0;
    int     blind_warned = 0;          /* path-C-is-blind notice, once */
    int     ref_wait_logged = 0;       /* railed-meter anchor deferral, once per arm */
    int     ceil_miss = 0, ceil_warned = 0;  /* dn_ceiling_check() latch */
    int64_t reassert_at = 0;
    int     reassert_left = 0;
    int64_t verify_at   = 0;           /* readback due for the last commanded mode */
    int     verify_cyc  = 0;           /* forced counter-cycles spent on it */
    int64_t enforce_at  = 0;           /* mid-cycle: when to switch back to cur */
    int64_t desync_since = 0;          /* standing cur/readback mismatch start */
    int     desync_warned = 0;         /* the notice, once per episode */
    int     booted      = 0;
    int64_t boot_at     = ms_now_us() / 1000;
    int     day_ok      = 0;           /* the current day is confirmed, not probed */
    float   day_min     = -1.0f;       /* lowest exposure of this day excursion */
    /* what the IR-cut filter costs in THIS scene, measured: the day pipeline
     * reading divided by the night reading that the ratio verdict left. The
     * silent probe answers "is the illuminator earning its keep"; the mode
     * decision needs "is there light enough to run day mode", and on a dark
     * interior those differ by this factor. -1 = not measured yet. */
    int     ir_fails    = 0;           /* consecutive illuminator-command failures */
    float   filter_cost = -1.0f;
    float   ir_night_at = -1.0f;       /* night level a pending ratio verdict left */
    float   probe_best  = -1.0f;       /* best day-pipeline reading ever, for the diagnostic */
    int     day_seen    = 0;           /* has day ever been confirmed? */
    int     probe_fails = 0;           /* consecutive probes that found night */
    int     diag_warned = 0;           /* the diagnostic is once per session */
    dn_learn lrn;
    int64_t learn_log_at = boot_at + DN_LEARN_LOG_FIRST_MS;

    g_int_hwm = 0;                     /* see dn_read(): sensor mode may differ */
    { ms_daynight_cfg dn0;
      config_str_lock();
      dn0 = g_cfg.daynight;
      config_str_unlock();
      dn_learn_load(dn0.state_path, &lrn);
      time_t wall = time(NULL);
      int ck = dn_cal_kind(&dn0, wall);
      LOGI(MOD, "detection thread started (mode=%s, day<%g night>%g, "
                "probe: gap %ds jump %d%% confirm %ds settle %ds, "
                "heartbeat %ds/%ds, calendar=%s, boot_probe=%d, learn=%d, "
                "interval %dms, dwell %ds, isp=%s, cmd=%s)",
           dn0.mode == DN_MODE_SCHEDULE ? "schedule" : "auto",
           (double)dn0.day_gain, (double)dn0.night_gain,
           dn0.probe_min_gap_s, dn0.probe_jump_pct, dn0.probe_confirm_s,
           dn0.probe_settle_s, dn0.heartbeat_s, dn0.heartbeat_max_s,
           ck == DN_CAL_TIME ? "time window" : ck == DN_CAL_SUN ? "sun" : "none",
           dn0.boot_probe, dn0.learn, dn0.interval_ms, dn0.transition_s,
           dn0.isp_path, dn0.switch_cmd);
      if (dn0.time_night_start[0] && dn0.time_day_start[0] &&
          (dn0.sun_latitude != 0.0f || dn0.sun_longitude != 0.0f))
          LOGW(MOD, "both a time window (%s..%s) and a location (%g/%g) are "
                    "configured - the time window wins and the sun settings "
                    "are ignored. Clear one of the two so the active schedule "
                    "is not a matter of precedence",
               dn0.time_night_start, dn0.time_day_start,
               (double)dn0.sun_latitude, (double)dn0.sun_longitude);
      if (dn0.mode == DN_MODE_SCHEDULE && ck == DN_CAL_NONE)
          LOGW(MOD, "mode=schedule needs a calendar: set daynight.time_night_"
                    "start/time_day_start, or daynight.sun_latitude/"
                    "sun_longitude, and make sure the clock is set"); }

    while (!ms_stopgate_stopped(&g_gate)) {
        /* M10 whole-struct snapshot (mirrors imp_osd.c refresh_text()): every
         * daynight tunable is runtime-mutable via /control, which applies
         * changes under the config string lock. Snapshot once per poll and
         * operate on the local copy, so one decision never mixes a freshly-set
         * threshold with a stale interval. */
        ms_daynight_cfg dncfg;
        int running_mode;
        config_str_lock();
        dncfg        = g_cfg.daynight;
        running_mode = g_cfg.image.running_mode;
        config_str_unlock();
        const ms_daynight_cfg *dn = &dncfg;
        int     interval = dn->interval_ms > 0 ? dn->interval_ms : 2000;
        int64_t now      = ms_now_us() / 1000;

        /* fire a pending post-switch running-mode re-assert once it comes due,
         * then re-arm until the series is exhausted (see DN_REASSERT_MS). */
        if (reassert_at && now >= reassert_at) {
            if (dn->enabled) {
                int rm = running_mode ? 1 : 0;
                hub_control("image.running_mode", rm ? "1" : "0");
                LOGD(MOD, "re-asserting running_mode=%d after switch (%d left)",
                     rm, reassert_left - 1);
                /* Divergence check: timps never writes running_mode - the
                 * board hook chain does, fire-and-forget, so a lost POST
                 * leaves the decision and the ISP silently desynced. By this
                 * point the ~1 s hook latency has long passed. Deliberately
                 * WARN-only: re-running switch_cmd here would clobber a
                 * legitimate manual override. */
                if ((cur == DN_NIGHT && !rm) || (cur == DN_DAY && rm))
                    LOGW(MOD, "running_mode=%d never followed the switch to %s - "
                              "board hook chain (switch_cmd -> color -> POST "
                              "/control) incomplete, or manual override",
                         rm, cur == DN_NIGHT ? "night" : "day");
            }
            reassert_at = (--reassert_left > 0) ? now + DN_REASSERT_MS : 0;
        }

        /* sample even in manual mode so GET /control always reports live
         * values (WebUI photosensing page + data collector). */
        dn_sample sm;
        dn_read(dn, &sm);
        float luma = -1.0f;
        { uint32_t al; if (hal_isp_ae_luma(&al) == 0) luma = (float)al; }

        if (!dn->enabled) {                 /* manual: measure, force nothing */
            was_enabled = 0;
            cur = DN_UNKNOWN;               /* mode may be set manually now */
            s = -1.0f; stable_n = 0; ref = -1.0f; ref_due = 0;
            trig_since = dark_since = verdict_at = hb_at = mode_since = 0;
            pre_probe = -1.0f; pre_probe_hr = -1;
            sust_min = win_max = -1.0f; win_at = 0;
            ema_fast = ema_slow = -1.0f; trend_since = 0;
            ir_verdict_at = 0; d_lit = -1.0f; d_lit_hr = -1;
            verify_at = enforce_at = 0; verify_cyc = 0;
            desync_since = 0; desync_warned = 0;
            day_ok = 0; day_min = -1.0f;
            dn_status_update(sm.bright, sm.gain, sm.d, luma, DN_UNKNOWN,
                             -1.0f, -1.0f, -1);
            dn_sleep(interval);
            continue;
        }
        if (!was_enabled) {                 /* (re)enabled: decide from scratch */
            was_enabled = 1;
            cur = DN_UNKNOWN;
            s = -1.0f; stable_n = 0; ref = -1.0f; ref_due = 0;
            trig_since = dark_since = verdict_at = hb_at = mode_since = 0;
            ir_verdict_at = 0; d_lit = -1.0f; d_lit_hr = -1;
            verify_at = enforce_at = 0; verify_cyc = 0;
            desync_since = 0; desync_warned = 0;
            last_probe = 0; pre_probe = -1.0f; pre_probe_hr = -1;
            ref_wait_logged = 0;
            sust_min = win_max = -1.0f; win_at = 0;
            ema_fast = ema_slow = -1.0f; trend_since = 0;
            day_ok = 0; day_min = -1.0f;
            filter_cost = ir_night_at = -1.0f;   /* re-measure from scratch */
            ir_fails = 0; g_ir_unusable = 0;     /* and re-test the illuminator */
            booted = 0; boot_at = now;
            warned_noisp = 0; hb_defer_logged = 0; blind_warned = 0;
            LOGI(MOD, "auto day/night enabled");
        }

        /* the smoother. Everything downstream reads `s`, never the raw tick:
         * one filter, applied once, instead of the previous design's several
         * partially-overlapping ones. */
        if (sm.d > 0.0f) {
            s = (s > 0.0f) ? s + (sm.d - s) * DN_ALPHA : sm.d;
            stable_n = (fabsf(sm.d - s) < s * DN_STABLE_PCT) ? stable_n + 1 : 0;
            /* the sustained minimum since the last probe (see
             * DN_MOVED_MARGIN): a tumbling probe_confirm_s window whose
             * MAXIMUM is what may enter the minimum, so a level only counts
             * once the scene held it for the whole window. O(1) state, and
             * the same debounce shape - and the same constant - that path C
             * uses for its own hold. */
            int64_t win_ms = (int64_t)dn->probe_confirm_s * 1000;
            if (win_max <= 0.0f) { win_max = s; win_at = now; }
            else if (s > win_max)  win_max = s;
            if (now - win_at >= win_ms) {
                if (sust_min <= 0.0f || win_max < sust_min) sust_min = win_max;
                win_max = s; win_at = now;
            }
            /* the trend pair. Fed the RAW sample, not `s`: two filters in
             * series would only add lag to a measurement whose whole content
             * is its own time constant.
             *
             * Only in night, and frozen while a silent probe is in flight.
             * Both restrictions are the same point - the pair compares the
             * scene against its own memory, and that is only a comparison
             * between like and like while the optics stay put. With the
             * illuminator off the camera is in a third optical state
             * entirely, and a day excursion's levels are not commensurable
             * with a night's at all. */
            /* ... and never fed a clip (see dn_clipped): a railed boot
             * seeds the memory at the rail, and the repair then reads as a
             * dawn - one wasted probe per boot, measured. */
            if (cur == DN_NIGHT && !ir_verdict_at && !enforce_at &&
                !dn_clipped(sm.headroom, dn)) {
                float af = dn_ema_alpha(interval, DN_TREND_FAST_MS);
                float as = dn_ema_alpha(interval, DN_TREND_SLOW_MS);
                ema_fast = (ema_fast > 0.0f) ? ema_fast + (sm.d - ema_fast) * af : sm.d;
                ema_slow = (ema_slow > 0.0f) ? ema_slow + (sm.d - ema_slow) * as : sm.d;
            }
        }

        float bar = dn_day_bar(dn, &lrn);   /* effective day threshold */
        int   target     = cur;             /* plain switch target */
        int   force      = 0;               /* bypass the dwell (probe revert) */
        int   want_probe = 0;
        int   no_silent  = 0;               /* this probe may not go the silent route */
        const char *probe_why = NULL;
        char  why[80];
        why[0] = 0;

        /* ---- ISP readback gate (see DN_VERIFY_MS) --------------------- */
        if (enforce_at && now >= enforce_at) {     /* cycle 2/2: back to cur */
            dn_switch(cur, "isp readback enforce", dn->switch_cmd, s, ref, bar);
            enforce_at = 0;
            if (verdict_at)         /* re-read the probe on settled optics */
                verdict_at = now + (int64_t)dn->probe_settle_s * 1000;
            verify_at = now + DN_VERIFY_MS;
            reassert_left = DN_REASSERT_COUNT;
            reassert_at   = now + DN_REASSERT_MS;
            s = -1.0f; stable_n = 0;
            sust_min = win_max = -1.0f; win_at = 0;
            ema_fast = ema_slow = -1.0f; trend_since = 0;
        }
        if (verify_at && sm.isp >= 0 && sm.isp == cur) {
            LOGD(MOD, "ISP confirmed the switch to %s%s",
                 cur == DN_NIGHT ? "night" : "day",
                 verify_cyc ? " (after a forced transition)" : "");
            verify_at = 0; verify_cyc = 0;
        } else if (verify_at && now >= verify_at && !ir_verdict_at) {
            verify_at = 0;
            if (sm.isp < 0) {
                LOGD(MOD, "ISP does not report its running mode - switch to "
                          "%s stays unverified",
                     cur == DN_NIGHT ? "night" : "day");
            } else if (verify_cyc < DN_VERIFY_MAX_CYCLES) {
                verify_cyc++;
                LOGW(MOD, "ISP still reports %s %d s after the switch to %s "
                          "(script and re-asserts all ran) - forcing one "
                          "transition through %s, the only thing a stuck "
                          "ISP acts on",
                     cur == DN_NIGHT ? "Day" : "Night",
                     (int)(DN_VERIFY_MS / 1000),
                     cur == DN_NIGHT ? "night" : "day",
                     cur == DN_NIGHT ? "day" : "night");
                dn_switch(cur == DN_NIGHT ? DN_DAY : DN_NIGHT, "isp readback",
                          dn->switch_cmd, s, ref, bar);
                enforce_at = now + DN_VERIFY_HOLD_MS;
                s = -1.0f; stable_n = 0;
            } else {
                LOGW(MOD, "ISP still reports %s after a forced transition - "
                          "giving up until the next mode change; the image "
                          "does not match the decided mode %s",
                     cur == DN_NIGHT ? "Day" : "Night",
                     cur == DN_NIGHT ? "night" : "day");
            }
        }

        /* ---- standing disagreement notice (see DN_DESYNC_MS) ---------- */
        {
            int agree = (booted && cur >= 0 && sm.isp >= 0)
                      ? (sm.isp == cur) : -1;
            if (agree == 0 && !verify_at && !enforce_at) {
                if (!desync_since) desync_since = now;
                if (!desync_warned && now - desync_since >= DN_DESYNC_MS) {
                    desync_warned = 1;
                    LOGW(MOD, "decided mode is %s but the ISP has been "
                              "rendering %s for %d s - not enforcing, this "
                              "may be a manual override. To re-measure and "
                              "resolve, request a probe: POST /control "
                              "{\"daynight\":{\"probe\":1}}",
                         cur == DN_NIGHT ? "night" : "day",
                         sm.isp == DN_NIGHT ? "Night" : "Day",
                         (int)((now - desync_since) / 1000));
                }
            } else {
                desync_since = 0;       /* gate active or no longer standing */
                if (desync_warned && agree == 1) {
                    desync_warned = 0;
                    LOGI(MOD, "decided mode and ISP agree again (%s)",
                         cur == DN_NIGHT ? "night" : "day");
                }
            }
        }

        do {
            /* mid-enforcement the optics are deliberately in the counter
             * mode and every reading is junk - hold all decisions. */
            if (enforce_at) break;
            /* ---------------- schedule mode: the calendar decides ------- */
            if (dn->mode == DN_MODE_SCHEDULE) {
                time_t wall = time(NULL);
                int t = dn_cal_target(dn, wall);
                if (t == DN_UNKNOWN) {
                    if (!warned_nocal) {
                        warned_nocal = 1;
                        LOGW(MOD, "mode=schedule but no usable calendar "
                                  "(set daynight.time_night_start/time_day_start "
                                  "or daynight.sun_latitude/sun_longitude, and "
                                  "make sure the clock is set) - forcing nothing");
                    }
                    break;
                }
                warned_nocal = 0;
                target = t;
                snprintf(why, sizeof why, "calendar");
                break;
            }

            /* ---------------- auto mode --------------------------------- */
            if (sm.d <= 0.0f) {             /* no ISP (sim/host/wedged) */
                if (!warned_noisp) {
                    warned_noisp = 1;
                    LOGW(MOD, "%s not readable, detection idle", dn->isp_path);
                }
                break;
            }
            warned_noisp = 0;
            dn_ceiling_check(&sm, &ceil_miss, &ceil_warned);

            /* (D) boot: the persisted mode is a guess about a scene nobody
             * measured. Turn it into a measurement once, at t=0 - which is
             * the restart-equivalence the design notes named as the property
             * every stuck-mode incident violated, made literal instead of
             * derived. */
            if (!booted) {
                if (now - boot_at < (int64_t)dn->boot_settle_s * 1000 ||
                    (stable_n < DN_STABLE_N &&
                     now - boot_at < DN_STABLE_MAX_MS))
                    break;                  /* AE has not converged yet */
                booted = 1;
                /* mode_since stays 0 deliberately: the dwell exists to space
                 * SWITCHES apart, and there has not been one yet - leaving it
                 * armed here would block the boot probe by transition_s. */
                /* Push image.running_mode into the ISP once, the same way a
                 * switch does. NOT switch_cmd: that moves the IR-cut filter,
                 * and the measurement says the board was already right - the
                 * illuminator was on, the ISP reported Night, and only its
                 * tuning was wrong. Re-asserting the running mode is what
                 * repaired it; a filter movement per boot would be a real
                 * mechanical cost the evidence does not ask for. */
                /* Assert ONCE, here, and do not arm the repeat countdown.
                 * Arming it was wrong in a way that only showed on a camera:
                 * the boot probe switches within seconds, and the pending
                 * re-asserts then kept pushing the PERSISTED value over the
                 * decision that had just been made - a living room sat in
                 * night mode in daylight because the switch to day was
                 * overwritten twice, eight seconds apart. A switch arms its
                 * own re-assert with its own value; this one only has to say
                 * the mode out loud once. */
                hub_control("image.running_mode", running_mode ? "1" : "0");
                LOGI(MOD, "boot: asserted running_mode=%d into the ISP - "
                          "nothing had told it this session", running_mode ? 1 : 0);
                verify_at = now + DN_VERIFY_MS; verify_cyc = 0;
                if (running_mode) {         /* persisted NIGHT */
                    cur = DN_NIGHT;
                    ref = -1.0f;
                    ref_due = now + (int64_t)dn->ref_delay_s * 1000;
                    ref_wait_logged = 0;
                    sust_min = win_max = -1.0f; win_at = 0;
                    ema_fast = ema_slow = -1.0f; trend_since = 0;
                    if (sm.headroom == 0) {
                        /* AE hard-railed since boot: the reading is a clip
                         * and a silent probe would divide two clips (r=1.00,
                         * measured). The re-assert above is a no-op here -
                         * the value never changed - and only a real
                         * transition re-tunes the AE (T23 boot defect). So
                         * spend the one filter cycle, even with boot_probe=0:
                         * this is a broken meter, not an unverified mode.
                         * Trade-off in CHANGELOG.md 2026-08-21. */
                        want_probe = 1; probe_why = "boot pegged";
                        no_silent = 1;
                        LOGI(MOD, "boot: AE railed with 0 units of reserve - "
                                  "the ISP came up mistuned and re-asserting "
                                  "running_mode cannot fix that; cycling "
                                  "through day once to re-tune it");
                    } else if (dn->boot_probe) {
                        want_probe = 1; probe_why = "boot verify";
                    } else {
                        hb_at = now + 1;    /* the first heartbeat does the job */
                        LOGI(MOD, "boot: adopting persisted night, "
                                  "boot_probe=0 so the heartbeat verifies it");
                    }
                } else {                    /* persisted DAY - honest pipeline */
                    cur = DN_DAY;
                    if (s < bar) {
                        day_ok = 1; day_seen = 1; day_min = s;
                        LOGI(MOD, "boot: day confirmed by exposure %.0f (< %.0f)",
                             (double)s, (double)bar);
                    } else {
                        target = DN_NIGHT; force = 1;
                        snprintf(why, sizeof why, "boot: exposure %.0f", (double)s);
                    }
                }
                break;
            }

            /* (0) THE SILENT PROBE'S VERDICT. The illuminator has been off
             * for probe_settle_s; this reading is the scene without our own
             * light in it. Comparing the two answers the question an absolute
             * threshold cannot: not "how bright is it" - which varies by a
             * factor of 63 across this fleet at one instant - but "am I making
             * this light, or is the room". */
            if (ir_verdict_at && now >= ir_verdict_at) {
                float d_dark = s;
                int   room   = sm.headroom;
                ir_verdict_at = 0;
                dn_irprobe(dn->irprobe_cmd, 1);      /* light first, judge after */
                s = -1.0f; stable_n = 0;             /* the reading changes back */
                float r = (d_lit > 0.0f && d_dark > 0.0f) ? d_dark / d_lit : -1.0f;
                d_lit = -1.0f;
                if (r <= 0.0f) {
                    LOGW(MOD, "silent probe gave no usable reading - falling "
                              "back to the IR-cut probe");
                    want_probe = 1; probe_why = ir_why ? ir_why : "probe";
                    /* escalations may not re-enter the silent path: d_lit is
                     * cleared, so a second silent probe reads r=-1 and the
                     * loop never reaches the audible judge it asked for. */
                    no_silent = 1;
                } else if (dn_clipped(d_lit_hr, dn)) {
                    /* the LIT reading was railed, so r divides two clips and
                     * says nothing (see dn_clipped). Railed-dark in night
                     * mode is still night evidence: stay, and let the next
                     * honest reading decide anything worth remembering. */
                    LOGD(MOD, "silent probe (%s): r=%.2f but the lit reading "
                              "had only %d units of AE reserve - a ratio of "
                              "two clips, staying night",
                         ir_why ? ir_why : "?", (double)r, d_lit_hr);
                    trig_since = 0;
                    hb_at = now + (int64_t)dn->heartbeat_s * 1000;
                    sust_min = win_max = -1.0f; win_at = 0;
                    ema_fast = ema_slow = -1.0f; trend_since = 0;
                } else if (r >= dn->ir_ratio_night) {
                    /* the illuminator was doing the work: night, and it cost
                     * no click at all. */
                    LOGD(MOD, "silent probe (%s): r=%.2f (%.0f without IR vs "
                              "%.0f with) - the illuminator carries this "
                              "scene, staying night",
                         ir_why ? ir_why : "?", (double)r, (double)d_dark,
                         (double)(d_dark / r));
                    /* The reference only ever ratcheted UPWARD, on proof
                     * from a day probe that came back dark. A silent probe
                     * answering "night" at a level below the bar is the same
                     * kind of proof pointing the other way: the reference
                     * predicted a brightening worth spending a look on, the
                     * look said night, so the reference is describing a scene
                     * that no longer exists. Without this there is no way
                     * down at all. Measured: a camera whose ISP came up wrong
                     * anchored at 131072, then read 6070 once it recovered -
                     * seventeen probes in a row, one every fourteen seconds,
                     * each eight seconds with the illuminator off, forever. */
                    {
                        float lit = d_dark / r;
                        float bar_c = ref * (float)dn->probe_jump_pct / 100.0f;
                        if (ref > 0.0f && lit > 0.0f && lit < bar_c) {
                            ref = lit;
                            LOGI(MOD, "night reference lowered to %.0f, proven "
                                      "by the silent probe (bar %.0f)",
                                 (double)ref,
                                 (double)(ref * (float)dn->probe_jump_pct
                                          / 100.0f));
                        }
                    }
                    trig_since = 0;
                    hb_at = now + (int64_t)dn->heartbeat_s * 1000;
                    sust_min = win_max = -1.0f; win_at = 0;
                    /* re-anchor the trend pair on a verdict, exactly as the
                     * decision note specifies. A probe that has just been
                     * answered must not be asked the same question again by
                     * the same evidence: reseeding puts the ratio back at
                     * 1.0, so path T has to earn a fresh 25 % of relative
                     * move before it fires next. Across a real twilight
                     * (factor 2.2 over 67 minutes) that is a handful of
                     * silent probes, which is what they are cheap for. */
                    ema_fast = ema_slow = -1.0f; trend_since = 0;
                } else if (room < 0) {
                    /* reserve unknown: r ~= 1 from a lit room and from a
                     * railed meter look identical, and only the reserve
                     * separates them (see the headroom note in dn_sample).
                     * Falling into the pegged branch below would call this
                     * "night" forever; the day pipeline is the one honest
                     * judge left. */
                    LOGD(MOD, "silent probe (%s): r=%.2f but the AE reserve "
                              "is unknown - cannot tell a lit room from a "
                              "railed meter, asking the day pipeline",
                         ir_why ? ir_why : "?", (double)r);
                    want_probe = 1; probe_why = ir_why ? ir_why : "probe";
                    no_silent = 1;          /* see the r<=0 branch */
                } else if (r <= dn->ir_ratio_day && room >= dn->ir_min_headroom) {
                    /* the room supplies the light - but that alone does not
                     * make day mode usable, because switching also closes the
                     * IR-cut filter. Once its cost has been measured here,
                     * project the day reading and refuse a verdict that is
                     * already known to bounce straight back off night_gain.
                     * Without this the two rules chase each other all night. */
                    float lit = d_dark / r;
                    if (filter_cost > 0.0f &&
                        lit * filter_cost > dn->night_gain) {
                        LOGD(MOD, "silent probe (%s): r=%.2f - the room "
                                  "supplies the light, but day mode would "
                                  "read ~%.0f (%.2fx the night level) against "
                                  "night_gain %.0f, staying night",
                             ir_why ? ir_why : "?", (double)r,
                             (double)(lit * filter_cost), (double)filter_cost,
                             (double)dn->night_gain);
                        /* this too is a night verdict below the bar, so the
                         * same lowering rule as the r>=ir_ratio_night branch
                         * applies - without it the jump trigger re-fires
                         * every probe_confirm_s forever (measured on a shed
                         * camera: one 8 s dimming every 26 s, all morning). */
                        {
                            float bar_c = ref * (float)dn->probe_jump_pct
                                          / 100.0f;
                            if (ref > 0.0f && lit > 0.0f && lit < bar_c) {
                                ref = lit;
                                LOGI(MOD, "night reference lowered to %.0f, "
                                          "proven by the silent probe (bar "
                                          "%.0f)", (double)ref,
                                     (double)(ref * (float)dn->probe_jump_pct
                                              / 100.0f));
                            }
                        }
                        trig_since = 0;
                        hb_at = now + (int64_t)dn->heartbeat_s * 1000;
                        sust_min = win_max = -1.0f; win_at = 0;
                        ema_fast = ema_slow = -1.0f; trend_since = 0;
                    } else {
                        LOGD(MOD, "silent probe (%s): r=%.2f with %d units of "
                                  "AE reserve - the room supplies the light, "
                                  "not the illuminator", ir_why ? ir_why : "?",
                             (double)r, room);
                        ir_night_at = lit;
                        target = DN_DAY;
                        snprintf(why, sizeof why, "IR ratio %.2f", (double)r);
                    }
                } else if (room < dn->ir_min_headroom) {
                    /* r came back near 1, but the AE had nothing left to move
                     * with, so it could not have answered. Pegged at the DARK
                     * end is itself proof of night - a bright-end ceiling
                     * cannot occur in night mode. Measured: a pitch-dark
                     * outbuilding returns r=1.14 with 1 unit of reserve. */
                    LOGD(MOD, "silent probe (%s): r=%.2f but only %d units of "
                              "AE reserve - the meter is pegged at the dark "
                              "end, which is itself night", 
                         ir_why ? ir_why : "?", (double)r, room);
                    trig_since = 0;
                    hb_at = now + (int64_t)dn->heartbeat_s * 1000;
                    sust_min = win_max = -1.0f; win_at = 0;
                    ema_fast = ema_slow = -1.0f; trend_since = 0;
                } else {
                    /* between the two thresholds: the ratio genuinely could
                     * not decide. Spend the click and let the day pipeline
                     * judge. */
                    LOGD(MOD, "silent probe (%s): r=%.2f is between %.2f and "
                              "%.2f - inconclusive, asking the day pipeline",
                         ir_why ? ir_why : "?", (double)r,
                         (double)dn->ir_ratio_day, (double)dn->ir_ratio_night);
                    want_probe = 1; probe_why = ir_why ? ir_why : "probe";
                    no_silent = 1;          /* see the r<=0 branch */
                }
                /* The measurement itself, in columns, once per probe at the
                 * point where every branch has decided. This is what the
                 * 2026-08-17 campaign collected by hand - a reading with the
                 * illuminator and one without - except it runs wherever timps
                 * runs, instead of needing a script to be installed and
                 * surviving only as long as nobody removes it.
                 *
                 * A line of its own rather than a tail on each verdict: the
                 * five branches word themselves differently, and five places
                 * to keep in step is how a parser ends up knowing four of
                 * them. -1 means not measured, as everywhere else here. */
                {   /* the trigger names are prose ("boot verify"); a
                     * key=value line must not carry a space in a value. */
                    char whyb[24];
                    snprintf(whyb, sizeof whyb, "%s", ir_why ? ir_why : "?");
                    for (char *w = whyb; *w; w++) if (*w == ' ') *w = '_';
                LOGD(MOD, "probe: r=%.2f lit=%.0f dark=%.0f hr=%d "
                          "verdict=%s mode=%s ref=%.0f why=%s lit_hr=%d",
                     (double)r,
                     (r > 0.0f) ? (double)(d_dark / r) : -1.0,
                     (double)d_dark, room,
                     (target == DN_DAY)   ? "day"
                     : want_probe         ? "escalate"
                     : "night",
                     (cur == DN_NIGHT) ? "night" : "day",
                     (double)ref, whyb, d_lit_hr);
                }
                ir_why = NULL;
                d_lit_hr = -1;
                break;
            }

            /* (1) an IR-CUT probe is in flight: exactly ONE verdict, binary.
             * There is no third "ambiguous" outcome to schedule a follow-up
             * for, which is what the whole dn_verify/dead-zone apparatus used
             * to be. */
            if (verdict_at && now >= verdict_at) {
                if (sm.isp >= 0 && sm.isp != cur && (verify_at || enforce_at))
                    break;   /* wrong optics - the readback gate is on it */
                verdict_at = 0;
                if ((probe_best <= 0.0f || s < probe_best) &&
                    !dn_clipped(sm.headroom, dn)) probe_best = s;
                if (s < bar) {
                    day_ok = 1; day_seen = 1; day_min = s;
                    probe_fails = 0;
                    pre_probe = -1.0f;      /* it stuck: not a revert any more */
                    pre_probe_hr = -1;
                    hb_defer_logged = 0;
                    LOGI(MOD, "probe confirmed day: exposure %.0f (< %.0f)",
                         (double)s, (double)bar);
                } else {
                    target = DN_NIGHT; force = 1;
                    probe_fails++;
                    snprintf(why, sizeof why, "probe found night, exposure %.0f",
                             (double)s);
                    dn_diag_threshold(dn, probe_fails, probe_best, day_seen,
                                      &diag_warned);
                }
                break;
            }

            /* (2) DAY: honest measurement, no history, no probe. */
            if (cur == DN_DAY) {
                /* while a probe's verdict is still pending the AE is mid
                 * ramp on the new pipeline and this branch must not pre-empt
                 * it: the outcome would be the same (night) but it would
                 * arrive through the ordinary path, losing the pre-probe
                 * level the reference is anchored from. */
                if (verdict_at) break;
                if (day_ok && (day_min <= 0.0f || s < day_min)) day_min = s;
                if (s > dn->night_gain) { if (!dark_since) dark_since = now; }
                else                    dark_since = 0;
                if (dark_since &&
                    now - dark_since >= (int64_t)dn->day_confirm_s * 1000) {
                    /* a ratio verdict undone by the absolute rule is the one
                     * measurement this scene ever offers of what day mode
                     * actually reads. Taking it here is what makes the next
                     * verdict cheaper than this one was.
                     *
                     * The factor may be BELOW 1: measured on a T20 whose
                     * illuminator contributes nothing (r = 1.00), the day
                     * pipeline read 6166 against a night level of 8171. That
                     * is not nonsense, it is a different relationship between
                     * the two pipelines - and rejecting it as such is what
                     * left that camera unable to learn and free to flap. */
                    if (ir_night_at > 0.0f && s > 0.0f &&
                        !dn_clipped(sm.headroom, dn)) {
                        /* not learned from a railed meter: a clipped day
                         * reading understates the cost and re-opens the flap
                         * this factor exists to close. */
                        filter_cost = s / ir_night_at;
                        LOGI(MOD, "day mode reads %.2fx the night level here "
                                  "(%.0f vs %.0f) - a ratio verdict now needs "
                                  "the night reading below %.0f",
                             (double)filter_cost, (double)s,
                             (double)ir_night_at,
                             (double)(dn->night_gain / filter_cost));
                    }
                    ir_night_at = -1.0f;
                    target = DN_NIGHT;
                    snprintf(why, sizeof why, "exposure %.0f > %.0f",
                             (double)s, (double)dn->night_gain);
                }
                break;
            }

            /* (3) NIGHT. */
            if (cur != DN_NIGHT) break;

            /* (3a) anchor the reference once the AE has settled after IR-on.
             * Nothing downstream is load-bearing on this single sample the
             * way the old night_baseline was: a reference that lands too high
             * costs one self-correcting probe, one that lands too low costs
             * the spontaneous trigger until the next heartbeat re-anchors it. */
            if (ref < 0.0f && ref_due && now >= ref_due &&
                (stable_n >= DN_STABLE_N || now >= ref_due + DN_STABLE_MAX_MS)) {
                if (dn_clipped(sm.headroom, dn)) {
                    /* a clip must not become the long-term reference (see
                     * dn_clipped); keep waiting for an honest sample. The
                     * absolute thresholds and the heartbeat carry the
                     * automaton meanwhile. */
                    if (!ref_wait_logged) {
                        ref_wait_logged = 1;
                        LOGI(MOD, "night reference deferred: only %d units of "
                                  "AE reserve, so %.0f is a clip, not a level "
                                  "- waiting for the meter to come off the "
                                  "rail", sm.headroom, (double)s);
                    }
                } else {
                    ref = s;
                    LOGI(MOD, "night reference %.0f (probe bar %.0f)",
                         (double)ref,
                         (double)(ref * (float)dn->probe_jump_pct / 100.0f));
                    dn_blind_check(&sm, dn, ref, &blind_warned);
                }
            }
            int sighted = dn_c_sighted(&sm, dn, ref);

            /* (3b) path C - spontaneous brightening, edge-triggered against a
             * reference that only ever moves on PROOF. */
            if (ref > 0.0f && s < ref * (float)dn->probe_jump_pct / 100.0f) {
                if (!trig_since) trig_since = now;
            } else trig_since = 0;
            if (trig_since &&
                now - trig_since >= (int64_t)dn->probe_confirm_s * 1000) {
                want_probe = 1; probe_why = "brightening";
            }

            /* (3b2) path T - the TREND. Path C asks "is this much brighter
             * than the level night was last PROVEN at", and a dawn never
             * answers yes: the bar is a factor of 2 and natural twilight
             * takes 67 minutes to manage 2.2 - measured on cam-C's
             * dawn (corpus scenario 20), 106 minutes to reach the bar, all
             * of it spent rendering IR video in daylight. This asks the
             * other question -
             * "is the scene brighter than it remembers being" - which is
             * exactly what a dawn does answer, and it is why the fleet's
             * mornings were previously found by the heartbeat or not at all.
             *
             * Armed ONLY where the silent probe exists. That is not a
             * portability detail, it is the affordability argument the
             * threshold rests on: 0.22 false fires per camera-hour is a few
             * seconds of dimmer image each when the illuminator can be
             * switched alone, and about 2.6 extra MOTOR movements per
             * 12-hour night when it cannot - which would be worse than the
             * 2 clicks a day this design exists to reach. A board without
             * separately switchable LEDs therefore keeps jump plus
             * heartbeat, i.e. exactly its pre-existing behaviour. */
            if (dn->irprobe_cmd[0] && !g_ir_unusable &&
                ema_fast > 0.0f && ema_slow > 0.0f &&
                ema_fast < ema_slow * (float)DN_TREND_PCT / 100.0f) {
                if (!trend_since) trend_since = now;
            } else trend_since = 0;
            /* Operator request from /control. Checked before the trend so a
             * manual probe is not swallowed by a trigger that would have
             * fired anyway - the point of asking is to get an answer NOW. */
            if (g_probe_req && !want_probe) {
                g_probe_req = 0;
                want_probe = 1; probe_why = "requested";
            }
            if (trend_since && !want_probe &&
                now - trend_since >= (int64_t)dn->probe_confirm_s * 1000) {
                want_probe = 1; probe_why = "trend";
            }

            /* (3c) path B - the heartbeat, the ONLY bound on a wrong night.
             * A scene that has not moved measurably since the last probe has
             * produced no evidence worth spending a click on, so the check is
             * deferred - but only while path C can actually see, and never
             * past heartbeat_max_s since the last probe. That hard bound is
             * why this is not a backoff. */
            if (!hb_at) hb_at = now + (int64_t)dn->heartbeat_s * 1000;
            if (now >= hb_at) {
                int flat = !(ref > 0.0f && sust_min > 0.0f &&
                             sust_min < ref * DN_MOVED_MARGIN);
                int64_t since = last_probe ? now - last_probe : INT64_MAX;
                if (flat && sighted &&
                    since < (int64_t)dn->heartbeat_max_s * 1000) {
                    hb_at = now + (int64_t)dn->heartbeat_s * 1000;
                    /* INFO, not DEBUG: fires once per episode and is the
                     * one line that answers "why did no heartbeat probe
                     * run all day" */
                    if (!hb_defer_logged++)
                        LOGI(MOD, "heartbeat due but nothing has happened "
                                  "since the last probe (sustained brightest "
                                  "%.0f, would need %.0f) - deferring, forced "
                                  "at %ds",
                             (double)sust_min,
                             (double)(ref * DN_MOVED_MARGIN),
                             dn->heartbeat_max_s);
                } else if (!want_probe) {
                    want_probe = 1; probe_why = "heartbeat";
                }
            }
        } while (0);

        /* ---------------- commit ------------------------------------- */
        /* THE ESCALATION. Looking is not one thing but two, with costs an
         * order of magnitude apart, so the cheap one goes first and the
         * expensive one only runs when the cheap one could not answer.
         *
         *   silent  - illuminator off for probe_settle_s. A GPIO write and a
         *             few seconds of dimmer image. No motor, no click, no
         *             wear. Answers "am I making this light" directly.
         *   audible - the IR-cut probe below. Moves a motor, and on a camera
         *             in a bedroom that is the thing people actually complain
         *             about.
         *
         * The previous design had only the audible one, which is why nine
         * separate rules existed to ration it. Making the common case free is
         * what let all nine go. */
        if (want_probe && !no_silent && cur == DN_NIGHT && !ir_verdict_at &&
            dn->irprobe_cmd[0] && !g_ir_unusable &&
            (!mode_since ||
             now - mode_since >= (int64_t)dn->transition_s * 1000)) {
            if (dn_irprobe(dn->irprobe_cmd, 0) == 0) {
                d_lit = s;
                d_lit_hr = sm.headroom;
                ir_why = probe_why;
                ir_verdict_at = now + (int64_t)dn->probe_settle_s * 1000;
                LOGD(MOD, "silent probe (%s): illuminator off, reading in %ds "
                          "(lit level %.0f)", probe_why ? probe_why : "?",
                     dn->probe_settle_s, (double)s);
                s = -1.0f; stable_n = 0;   /* the scene is about to change */
                want_probe = 0;
                probe_why = NULL;
                /* and nothing else happens this tick: the audible path below
                 * must not fire while a silent verdict is pending, or the
                 * cheap measurement is paid for and then thrown away. */
                ir_fails = 0;
                goto tail;
            }
            /* dn_irprobe() failing has already logged why; want_probe stays
             * set and the audible path below takes over this tick. */
            if (++ir_fails >= DN_IR_MAX_FAILS) {
                g_ir_unusable = 1;
                LOGW(MOD, "'%s' failed %d times - retiring the silent probe "
                          "for this session; the trend trigger goes with it, "
                          "leaving the jump trigger and the heartbeat",
                     dn->irprobe_cmd, ir_fails);
            }
        }
        if (want_probe && cur == DN_NIGHT && !ir_verdict_at &&
            (!last_probe ||
             now - last_probe >= (int64_t)dn->probe_min_gap_s * 1000) &&
            (!mode_since ||
             now - mode_since >= (int64_t)dn->transition_s * 1000)) {
            /* THE probe. It is the only route from night to day, and
             * probe_min_gap_s above is the only thing rationing it - so the
             * worst-case audible click rate is a property of the config
             * rather than of interacting heuristics. */
            LOGD(MOD, "probe (%s): exposure %.0f vs night reference %.0f, "
                      "verdict in %ds", probe_why, (double)s, (double)ref,
                 dn->probe_settle_s);
            pre_probe = (s > 0.0f) ? s : ref;
            pre_probe_hr = sm.headroom;
            dn_switch(DN_DAY, probe_why, dn->switch_cmd, s, ref, bar);
            cur = DN_DAY; mode_since = now; last_probe = now;
            verdict_at = now + (int64_t)dn->probe_settle_s * 1000;
            ir_verdict_at = 0; d_lit = -1.0f; ir_why = NULL;
            trig_since = dark_since = 0; hb_at = 0;
            s = -1.0f; stable_n = 0;
            sust_min = win_max = -1.0f; win_at = 0;
            ema_fast = ema_slow = -1.0f; trend_since = 0;
            day_ok = 0; day_min = -1.0f;
            hb_defer_logged = 0;
            reassert_left = DN_REASSERT_COUNT;
            reassert_at   = now + DN_REASSERT_MS;
            verify_at = now + DN_VERIFY_MS; verify_cyc = 0; enforce_at = 0;
        } else if (target != cur && target != DN_UNKNOWN &&
                   (force || !mode_since ||
                    now - mode_since >= (int64_t)dn->transition_s * 1000)) {
            int reverted = (cur == DN_DAY && target == DN_NIGHT && pre_probe > 0.0f);
            dn_switch(target, why[0] ? why : "?", dn->switch_cmd, s, ref, bar);
            cur = target; mode_since = now;
            s = -1.0f; stable_n = 0;
            trig_since = dark_since = verdict_at = 0;
            ir_verdict_at = 0; d_lit = -1.0f; ir_why = NULL;
            sust_min = win_max = -1.0f; win_at = 0;
            ema_fast = ema_slow = -1.0f; trend_since = 0;
            reassert_left = DN_REASSERT_COUNT;
            reassert_at   = now + DN_REASSERT_MS;
            verify_at = now + DN_VERIFY_MS; verify_cyc = 0; enforce_at = 0;
            hb_defer_logged = 0;
            if (target == DN_NIGHT) {
                if (reverted && dn_clipped(pre_probe_hr, dn)) {
                    /* the probe proved night, but the level it left was a
                     * clip (see dn_clipped) - a verdict, not a reference.
                     * Anchor from the next honest sample instead. */
                    ref = -1.0f;
                    ref_due = now + (int64_t)dn->ref_delay_s * 1000;
                    ref_wait_logged = 0;
                    LOGI(MOD, "probe found night, but the pre-probe level "
                              "%.0f had only %d units of AE reserve - the "
                              "reference stays unset until the meter can "
                              "answer", (double)pre_probe, pre_probe_hr);
                } else if (reverted) {
                    /* A probe that found darkness is a PROOF of night at the
                     * level measured immediately before it. That is the whole
                     * ratchet: one assignment, here and nowhere else. It also
                     * makes a repeat of the same stimulus (headlights, a dip
                     * in an overcast dawn) unable to re-fire, because the bar
                     * is now derived from the level that did fire. */
                    ref = pre_probe; ref_due = 0;
                    LOGI(MOD, "night reference %.0f, proven by the probe "
                              "(bar %.0f)", (double)ref,
                         (double)(ref * (float)dn->probe_jump_pct / 100.0f));
                    dn_blind_check(&sm, dn, ref, &blind_warned);
                } else {
                    ref = -1.0f;
                    ref_due = now + (int64_t)dn->ref_delay_s * 1000;
                    ref_wait_logged = 0;
                    /* a confirmed day that has now ended is a learning
                     * sample: this is how bright this scene actually gets. */
                    if (day_ok && day_min > 0.0f) dn_learn_add(&lrn, day_min);
                }
                day_ok = 0; day_min = -1.0f;
                hb_at = now + (int64_t)dn->heartbeat_s * 1000;
                { int64_t dawn = dn_secs_to_dawn(dn, time(NULL));
                  if (dawn >= 0 && now + dawn * 1000 < hb_at)
                      hb_at = now + dawn * 1000; }
            } else {
                ref = -1.0f; ref_due = 0; hb_at = 0;
                /* entering day: only the calendar's own verdict counts as a
                 * confirmation here - an auto-mode day is confirmed by the
                 * probe verdict instead, which sets these itself. */
                day_ok  = (dn->mode == DN_MODE_SCHEDULE);
                day_min = day_ok ? s : -1.0f;
            }
            pre_probe = -1.0f; pre_probe_hr = -1;
        }

    tail:
        if (now >= learn_log_at) {
            learn_log_at = now + DN_LEARN_LOG_MS;
            dn_learn_log(dn, &lrn);
        }
        dn_learn_save(dn, &lrn, now);

        float st_ref = (cur == DN_NIGHT) ? ref : -1.0f;
        float st_bar = (cur == DN_NIGHT && ref > 0.0f)
                     ? ref * (float)dn->probe_jump_pct / 100.0f : -1.0f;
        /* the DEBOUNCED state, not the raw per-tick comparison - raw would
         * flicker to 1 during every ordinary switch transient */
        int st_desync = (booted && cur >= 0 && sm.isp >= 0)
                      ? (desync_warned ? 1 : 0) : -1;
        dn_trace(dn, now, cur, &sm, s, st_ref, st_bar, verdict_at, hb_at,
                 ema_fast, ema_slow);
        dn_status_update(sm.bright, sm.gain, sm.d, luma, cur, st_ref, st_bar,
                         st_desync);
        dn_sleep(interval);
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

#endif /* USE_DAYNIGHT */
