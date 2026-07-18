/* daynight.c - native automatic day/night detection. See daynight.h.
 * The decision is GAIN-based like prudynt/raptor: total_gain in the IMP
 * [24.8] linear scale (256 = 1x) is compared against
 * total_gain_day/night_threshold (defaults 300/3000). Direction is INVERTED
 * vs brightness: high gain = dark scene = night. The wide day..night gap is
 * the hysteresis dead-zone. When no gain field is readable, the brightness
 * fallback keeps the daynightd port (formula + threshold_low/high +
 * averaging) so an existing tuning still translates 1:1. Compiled only with
 * -DUSE_DAYNIGHT; uses nothing but libc + pthread. */
#include "daynight.h"
#include "config.h"
#include "hal/hal.h"   /* hal_isp_total_gain(): ISP gain via the IMP API */

#ifdef USE_DAYNIGHT
#include "events.h"   /* wake /events SSE subscribers on real changes */
#include "log.h"
#include "util.h"     /* ms_now_us(): monotonic clock for dwell/baseline (M12) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <math.h>

#define MOD "DAYNIGHT"

enum { DN_DAY = 0, DN_NIGHT = 1, DN_UNKNOWN = -1 };

/* smoothing window, same as daynightd's BRIGHTNESS_SAMPLES */
#define DN_SAMPLES 10

static pthread_t    g_thr;
static volatile int g_stop;
static int          g_started;

/* latest measurement, shared with control.c via daynight_get_status() */
static pthread_mutex_t g_st_mu = PTHREAD_MUTEX_INITIALIZER;
static float g_st_brightness = -1.0f;      /* % or <0 = unknown */
static float g_st_gain       = -1.0f;      /* [24.8] linear or <0 = unknown */
static float g_st_luma       = -1.0f;      /* AE luma or <0 = unavailable */
static int   g_st_mode       = DN_UNKNOWN; /* mode as switched by the thread */

static void dn_status_update(float brightness, float total_gain, float ae_luma, int mode)
{
    /* last values that woke /events (only touched by the sampling thread) */
    static float nfy_b = -1000.0f, nfy_g = -1000.0f;
    static int   nfy_m = -1000;

    pthread_mutex_lock(&g_st_mu);
    g_st_brightness = brightness;
    g_st_gain       = total_gain;
    g_st_luma       = ae_luma;
    g_st_mode       = mode;
    pthread_mutex_unlock(&g_st_mu);

    /* wake /events subscribers only on a REAL change - brightness/gain
     * jitter every sample, so require a mode flip, >= 1% brightness or a
     * >= 5% gain move (same thresholds as the /events consumer dedup) */
    float db = brightness - nfy_b; if (db < 0) db = -db;
    float dg = total_gain - nfy_g; if (dg < 0) dg = -dg;
    if (mode != nfy_m || db >= 1.0f ||
        dg >= (nfy_g > 0.0f ? nfy_g * 0.05f : 8.0f)) {
        nfy_b = brightness; nfy_g = total_gain; nfy_m = mode;
        events_notify();
    }
}

/* Scene brightness 0..100% from the ISP proc file, or <0 if unavailable.
 * Port of daynightd's calculate_brightness_from_isp(): the integration-time
 * ratio (low integration = bright scene) damped by sensor analog gain
 * (/160 max) and ISP digital gain (/80 max); fallbacks: the ISP "Brightness"
 * setting, then the reported running mode.
 *
 * total_gain (out, may be NULL): the combined sensor+ISP gain in the IMP
 * [24.8] linear format (256 = 1x) - the same scale as
 * IMP_ISP_Tuning_GetTotalGain and the prudynt/raptor "total_gain" the WebUI
 * plots (photosensing thresholds: day 300, night 3000). The isp-m0 gain
 * fields are in the IMP log2 unit (0 = 1x, 32 = 2x, per the SetMaxAgain/
 * SetMaxDgain docs), so linear = 2^(units/32); analog + sensor digital +
 * ISP digital add up in log space. -1 when no gain field was found.
 * NOTE: sscanf on the exact-prefix format quietly skips the "MAX SENSOR
 * analog gain" style maximum lines that strstr also matches. */
static float dn_brightness(const char *path, float *total_gain)
{
    if (total_gain) *total_gain = -1.0f;
    FILE *fp = fopen(path, "r");
    if (!fp) return -1.0f;

    char line[256];
    int  integration_time = -1, max_integration_time = -1;
    int  analog_gain = -1, digital_gain = -1, isp_digital_gain = -1;
    int  cur_brightness = -1;
    char mode[32] = {0};

    while (fgets(line, sizeof line, fp)) {
        if (strstr(line, "ISP Runing Mode :"))
            sscanf(line, "ISP Runing Mode : %31s", mode);
        else if (strstr(line, "SENSOR Integration Time :"))
            sscanf(line, "SENSOR Integration Time : %d lines", &integration_time);
        else if (strstr(line, "SENSOR Max Integration Time :"))
            sscanf(line, "SENSOR Max Integration Time : %d lines", &max_integration_time);
        else if (strstr(line, "SENSOR analog gain :"))
            sscanf(line, "SENSOR analog gain : %d", &analog_gain);
        else if (strstr(line, "SENSOR digital gain :"))
            sscanf(line, "SENSOR digital gain : %d", &digital_gain);
        else if (strstr(line, "ISP digital gain :"))
            sscanf(line, "ISP digital gain : %d", &isp_digital_gain);
        else if (strstr(line, "Brightness :"))
            sscanf(line, "Brightness : %d", &cur_brightness);
    }
    fclose(fp);

    if (total_gain &&
        (analog_gain >= 0 || digital_gain >= 0 || isp_digital_gain >= 0)) {
        float units = 0.0f;                 /* log2 gain, 32 units per stop */
        if (analog_gain      > 0) units += (float)analog_gain;
        if (digital_gain     > 0) units += (float)digital_gain;
        if (isp_digital_gain > 0) units += (float)isp_digital_gain;
        *total_gain = 256.0f * exp2f(units / 32.0f);   /* -> [24.8] linear */
    }

    float b = -1.0f;
    if (integration_time >= 0 && max_integration_time > 0) {
        float exposure_ratio = (float)integration_time / (float)max_integration_time;
        b = (1.0f - exposure_ratio) * 100.0f;          /* low integration = bright */
        if (analog_gain >= 0)      b /= 1.0f + analog_gain / 160.0f;
        if (isp_digital_gain > 0)  b /= 1.0f + isp_digital_gain / 80.0f;
        if (b < 0.0f)   b = 0.0f;
        if (b > 100.0f) b = 100.0f;
    } else if (cur_brightness >= 0) {
        b = ((float)cur_brightness / 255.0f) * 100.0f;
    } else if (mode[0]) {
        if      (!strcmp(mode, "Day"))   b = 75.0f;
        else if (!strcmp(mode, "Night")) b = 25.0f;
    }
    return b;
}

/* run "<switch_cmd> day|night" (the thingino board script: ircut/light/color).
 * The mode change is committed even if the command fails so a missing script
 * warns once per switch instead of retrying every sample. */
static void dn_switch(int mode, const char *why)
{
    const char *arg = (mode == DN_NIGHT) ? "night" : "day";
    char cmd[192];
    snprintf(cmd, sizeof cmd, "%s %s >/dev/null 2>&1",
             g_cfg.daynight.switch_cmd, arg);
    LOGI(MOD, "switching to %s (%s): %s %s",
         arg, why, g_cfg.daynight.switch_cmd, arg);
    int rc = system(cmd);
    if (rc != 0)
        LOGW(MOD, "'%s %s' failed (rc=%d) - is the script installed?",
             g_cfg.daynight.switch_cmd, arg, rc);
}

/* sleep interval_ms in small slices so daynight_stop() joins promptly */
static void dn_sleep(int ms)
{
    while (ms > 0 && !g_stop) {
        int step = ms > 200 ? 200 : ms;
        usleep((useconds_t)step * 1000);
        ms -= step;
    }
}

static void *dn_thread(void *arg)
{
    (void)arg;
    int    cur = DN_UNKNOWN;            /* mode as switched by US */
    int    was_enabled = 0;
    int    warned_noisp = 0;
    float  hist[DN_SAMPLES];
    int    hidx = 0;
    int64_t last_switch_ms = 0;
    /* adaptive night baseline (raptor-style): gain sampled once, baseline_delay_s
     * after entering night (IR LEDs settled); night->day then triggers relative
     * to it. -1 = not sampled yet. */
    float   night_baseline = -1.0f;
    int64_t night_entered_ms = 0;

    for (int i = 0; i < DN_SAMPLES; i++) hist[i] = 50.0f;  /* neutral start */

    LOGI(MOD, "detection thread started (gain day<%g night>%g, "
              "brightness fallback %.1f/%.1f hyst %.2f, "
              "interval %dms, dwell %ds, isp=%s, cmd=%s)",
         (double)g_cfg.daynight.total_gain_day_threshold,
         (double)g_cfg.daynight.total_gain_night_threshold,
         g_cfg.daynight.threshold_low, g_cfg.daynight.threshold_high,
         g_cfg.daynight.hysteresis, g_cfg.daynight.interval_ms,
         g_cfg.daynight.transition_s, g_cfg.daynight.isp_path,
         g_cfg.daynight.switch_cmd);

    while (!g_stop) {
        const ms_daynight_cfg *dn = &g_cfg.daynight;
        int interval = dn->interval_ms > 0 ? dn->interval_ms : 500;

        /* sample even in manual mode so GET /control always reports the live
         * brightness/total_gain (WebUI gain display + data collector).
         * total_gain: prefer the ISP's own IMP_ISP_Tuning_GetTotalGain (robust,
         * like prudynt/raptor) and fall back to the /proc/isp-m0 scrape only
         * when the API is unavailable (host sim, T40/T41, ISP down). Brightness
         * still comes from the scrape (no direct luma API used yet). */
        float tg = -1.0f;
        float b  = dn_brightness(dn->isp_path, &tg);
        { uint32_t hg; if (hal_isp_total_gain(&hg) == 0) tg = (float)hg; }
        float luma = -1.0f;
        { uint32_t al; if (hal_isp_ae_luma(&al) == 0) luma = (float)al; }

        if (!dn->enabled) {             /* manual mode: measure, force nothing */
            was_enabled = 0;
            cur = DN_UNKNOWN;           /* mode may be forced manually now */
            night_baseline = -1.0f;
            dn_status_update(b, tg, luma, DN_UNKNOWN);
            dn_sleep(interval);
            continue;
        }
        if (!was_enabled) {             /* (re)enabled: detect from scratch */
            was_enabled = 1;
            cur = DN_UNKNOWN;           /* mode may have been set manually */
            night_baseline = -1.0f;
            for (int i = 0; i < DN_SAMPLES; i++) hist[i] = 50.0f;
            LOGI(MOD, "auto day/night enabled");
        }

        if (b < 0.0f && tg < 0.0f) {    /* no ISP (sim/host): skip silently */
            if (!warned_noisp) {
                LOGD(MOD, "%s not readable, detection idle", dn->isp_path);
                warned_noisp = 1;
            }
            dn_status_update(b, tg, luma, cur);
            dn_sleep(interval);
            continue;
        }
        warned_noisp = 0;

        int  target = cur;
        char why[64];
        if (tg >= 0.0f) {
            /* PRIMARY: total_gain, prudynt/raptor semantics. INVERTED vs
             * brightness: high gain = dark = night. The day..night threshold
             * gap (300..3000 by default) is the hysteresis dead-zone, so no
             * averaging is needed; from UNKNOWN a mid-gap start stays put
             * until the gain leaves the dead-zone. */
            /* night->day: relative to the adaptive night baseline when we have
             * one (day when gain < day_gain_pct% of it), else the fixed day
             * threshold. day->night stays the fixed night threshold. */
            float day_thr = dn->total_gain_day_threshold;
            if (night_baseline > 0.0f && dn->day_gain_pct > 0)
                day_thr = night_baseline * (float)dn->day_gain_pct / 100.0f;
            if (cur == DN_DAY) {
                if (tg > dn->total_gain_night_threshold) target = DN_NIGHT;
            } else if (cur == DN_NIGHT) {
                if (tg < day_thr)                        target = DN_DAY;
            } else {
                if      (tg > dn->total_gain_night_threshold) target = DN_NIGHT;
                else if (tg < dn->total_gain_day_threshold)   target = DN_DAY;
            }
            snprintf(why, sizeof why, "total_gain %.0f", (double)tg);
        } else {
            /* FALLBACK (no gain field readable): averaged brightness with
             * daynightd's hysteresis semantics: inside day/night the plain
             * thresholds apply (leave day only below low, leave night only
             * above high); from UNKNOWN the band is narrowed by
             * (high-low)*hysteresis on both sides so a mid-range start
             * stays put */
            hist[hidx] = b;
            hidx = (hidx + 1) % DN_SAMPLES;
            float avg = 0.0f;
            for (int i = 0; i < DN_SAMPLES; i++) avg += hist[i];
            avg /= DN_SAMPLES;

            float hyst_range = (dn->threshold_high - dn->threshold_low) * dn->hysteresis;
            if (cur == DN_DAY) {
                if (avg < dn->threshold_low)  target = DN_NIGHT;
            } else if (cur == DN_NIGHT) {
                if (avg > dn->threshold_high) target = DN_DAY;
            } else {
                if      (avg < dn->threshold_low  + hyst_range) target = DN_NIGHT;
                else if (avg > dn->threshold_high - hyst_range) target = DN_DAY;
            }
            snprintf(why, sizeof why, "avg brightness %.1f%%", (double)avg);
        }

        if (target != cur && target != DN_UNKNOWN) {
            /* minimum dwell between switches (first switch is immediate).
             * CLOCK_MONOTONIC, not time(NULL): an NTP step after boot (typical
             * on cameras) would make wall-clock deltas negative (switching
             * stuck for the step size) or jump straight past the dwell (M12) */
            int64_t now_ms = ms_now_us() / 1000;
            if (cur != DN_UNKNOWN &&
                now_ms - last_switch_ms < (int64_t)dn->transition_s * 1000) {
                LOGD(MOD, "transition delay not met, waiting");
            } else {
                dn_switch(target, why);
                cur = target;
                last_switch_ms = now_ms;
                /* (re)arm the adaptive baseline: sample it a while after we
                 * enter night, once the IR LEDs have settled; clear on day */
                night_baseline = -1.0f;
                night_entered_ms = (target == DN_NIGHT) ? now_ms : 0;
            }
        }

        /* sample the night gain baseline once, after the IR LEDs settle */
        if (cur == DN_NIGHT && night_baseline < 0.0f && tg >= 0.0f &&
            dn->baseline_delay_s > 0 && night_entered_ms > 0 &&
            ms_now_us() / 1000 - night_entered_ms >=      /* monotonic (M12) */
                (int64_t)dn->baseline_delay_s * 1000) {
            night_baseline = tg;
            LOGI(MOD, "night gain baseline = %.0f (day trigger < %d%% = %.0f)",
                 (double)night_baseline, dn->day_gain_pct,
                 (double)(night_baseline * (float)dn->day_gain_pct / 100.0f));
        }

        dn_status_update(b, tg, luma, cur);
        dn_sleep(interval);
    }
    LOGI(MOD, "detection thread stopped");
    return NULL;
}

void daynight_start(void)
{
    if (g_started) return;
    g_stop = 0;
    if (pthread_create(&g_thr, NULL, dn_thread, NULL) != 0) {
        LOGW(MOD, "cannot start detection thread");
        return;
    }
    g_started = 1;
}

void daynight_stop(void)
{
    if (!g_started) return;
    g_stop = 1;
    pthread_join(g_thr, NULL);
    g_started = 0;
}

/* see daynight.h: latest measurement for GET /control */
void daynight_get_status(int *enabled, int *mode,
                         float *brightness, float *total_gain, float *ae_luma)
{
    pthread_mutex_lock(&g_st_mu);
    float b  = g_st_brightness;
    float tg = g_st_gain;
    float lu = g_st_luma;
    int   m  = g_st_mode;
    pthread_mutex_unlock(&g_st_mu);
    if (m == DN_UNKNOWN)   /* manual mode / before the first auto switch */
        m = g_cfg.image.running_mode ? DN_NIGHT : DN_DAY;
    if (enabled)    *enabled    = g_cfg.daynight.enabled ? 1 : 0;
    if (mode)       *mode       = m;
    if (brightness) *brightness = b;
    if (total_gain) *total_gain = tg;
    if (ae_luma)    *ae_luma    = lu;
}

#else /* !USE_DAYNIGHT */

/* stub so control.c always links: no detection thread -> auto is off, no
 * measurement -> brightness/gain unknown, mode from the persisted/live ISP
 * running mode */
void daynight_get_status(int *enabled, int *mode,
                         float *brightness, float *total_gain, float *ae_luma)
{
    if (enabled)    *enabled    = 0;
    if (mode)       *mode       = g_cfg.image.running_mode ? 1 : 0;
    if (brightness) *brightness = -1.0f;
    if (total_gain) *total_gain = -1.0f;
    if (ae_luma)    *ae_luma    = -1.0f;
}

#endif /* USE_DAYNIGHT */
