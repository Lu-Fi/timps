/* daynight.c - native automatic day/night detection. See daynight.h.
 * Brightness formula and hysteresis/threshold semantics are a straight port
 * of thingino's daynightd (package/thingino-daynightd/files/daynightd.c) so
 * an existing daynightd tuning translates 1:1. Compiled only with
 * -DUSE_DAYNIGHT; uses nothing but libc + pthread. */
#ifdef USE_DAYNIGHT
#include "daynight.h"
#include "config.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#define MOD "DAYNIGHT"

enum { DN_DAY = 0, DN_NIGHT = 1, DN_UNKNOWN = -1 };

/* smoothing window, same as daynightd's BRIGHTNESS_SAMPLES */
#define DN_SAMPLES 10

static pthread_t    g_thr;
static volatile int g_stop;
static int          g_started;

/* Scene brightness 0..100% from the ISP proc file, or <0 if unavailable.
 * Port of daynightd's calculate_brightness_from_isp(): the integration-time
 * ratio (low integration = bright scene) damped by sensor analog gain
 * (/160 max) and ISP digital gain (/80 max); fallbacks: the ISP "Brightness"
 * setting, then the reported running mode. */
static float dn_brightness(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return -1.0f;

    char line[256];
    int  integration_time = -1, max_integration_time = -1;
    int  analog_gain = -1, isp_digital_gain = -1, cur_brightness = -1;
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
        else if (strstr(line, "ISP digital gain :"))
            sscanf(line, "ISP digital gain : %d", &isp_digital_gain);
        else if (strstr(line, "Brightness :"))
            sscanf(line, "Brightness : %d", &cur_brightness);
    }
    fclose(fp);

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
static void dn_switch(int mode, float avg)
{
    const char *arg = (mode == DN_NIGHT) ? "night" : "day";
    char cmd[192];
    snprintf(cmd, sizeof cmd, "%s %s >/dev/null 2>&1",
             g_cfg.daynight.switch_cmd, arg);
    LOGI(MOD, "switching to %s (avg brightness %.1f%%): %s %s",
         arg, avg, g_cfg.daynight.switch_cmd, arg);
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

    for (int i = 0; i < DN_SAMPLES; i++) hist[i] = 50.0f;  /* neutral start */

    LOGI(MOD, "detection thread started (thresholds %.1f/%.1f, hyst %.2f, "
              "interval %dms, dwell %ds, isp=%s, cmd=%s)",
         g_cfg.daynight.threshold_low, g_cfg.daynight.threshold_high,
         g_cfg.daynight.hysteresis, g_cfg.daynight.interval_ms,
         g_cfg.daynight.transition_s, g_cfg.daynight.isp_path,
         g_cfg.daynight.switch_cmd);

    while (!g_stop) {
        const ms_daynight_cfg *dn = &g_cfg.daynight;
        int interval = dn->interval_ms > 0 ? dn->interval_ms : 500;

        if (!dn->enabled) {             /* manual mode: idle, force nothing */
            was_enabled = 0;
            dn_sleep(interval);
            continue;
        }
        if (!was_enabled) {             /* (re)enabled: detect from scratch */
            was_enabled = 1;
            cur = DN_UNKNOWN;           /* mode may have been set manually */
            for (int i = 0; i < DN_SAMPLES; i++) hist[i] = 50.0f;
            LOGI(MOD, "auto day/night enabled");
        }

        float b = dn_brightness(dn->isp_path);
        if (b < 0.0f) {                 /* no ISP (sim/host): skip silently */
            if (!warned_noisp) {
                LOGD(MOD, "%s not readable, detection idle", dn->isp_path);
                warned_noisp = 1;
            }
            dn_sleep(interval);
            continue;
        }
        warned_noisp = 0;

        hist[hidx] = b;
        hidx = (hidx + 1) % DN_SAMPLES;
        float avg = 0.0f;
        for (int i = 0; i < DN_SAMPLES; i++) avg += hist[i];
        avg /= DN_SAMPLES;

        /* daynightd's hysteresis semantics: inside day/night the plain
         * thresholds apply (leave day only below low, leave night only above
         * high); from UNKNOWN the band is narrowed by
         * (high-low)*hysteresis on both sides so a mid-range start stays put */
        float hyst_range = (dn->threshold_high - dn->threshold_low) * dn->hysteresis;
        int target = cur;
        if (cur == DN_DAY) {
            if (avg < dn->threshold_low)  target = DN_NIGHT;
        } else if (cur == DN_NIGHT) {
            if (avg > dn->threshold_high) target = DN_DAY;
        } else {
            if      (avg < dn->threshold_low  + hyst_range) target = DN_NIGHT;
            else if (avg > dn->threshold_high - hyst_range) target = DN_DAY;
        }

        if (target != cur && target != DN_UNKNOWN) {
            /* minimum dwell between switches (first switch is immediate) */
            int64_t now_ms = (int64_t)time(NULL) * 1000;
            if (cur != DN_UNKNOWN &&
                now_ms - last_switch_ms < (int64_t)dn->transition_s * 1000) {
                LOGD(MOD, "transition delay not met, waiting");
            } else {
                dn_switch(target, avg);
                cur = target;
                last_switch_ms = now_ms;
            }
        }

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
#endif /* USE_DAYNIGHT */
