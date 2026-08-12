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
#include <stdio.h>     /* snprintf() - needed by both the real impl and the !USE_DAYNIGHT stub */

#ifdef USE_DAYNIGHT
#include "hub.h"       /* hub_control(): re-assert running_mode into the ISP */
#include "events.h"   /* wake /events SSE subscribers on real changes */
#include "log.h"
#include "util.h"     /* ms_now_us(): monotonic clock for dwell/baseline (M12) */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>       /* F-01: fork/execlp/dup2 instead of system() */
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <math.h>

#define MOD "DAYNIGHT"

enum { DN_DAY = 0, DN_NIGHT = 1, DN_UNKNOWN = -1 };

/* smoothing window, same as daynightd's BRIGHTNESS_SAMPLES */
#define DN_SAMPLES 10

static pthread_t    g_thr;
static ms_stopgate g_gate;   /* P-02: stop-condvar (was volatile g_stop + slice-sleep) */
static int          g_started;

/* latest measurement, shared with control.c via daynight_get_status() */
static pthread_mutex_t g_st_mu = PTHREAD_MUTEX_INITIALIZER;
static float g_st_brightness = -1.0f;      /* % or <0 = unknown */
static float g_st_gain       = -1.0f;      /* [24.8] linear or <0 = unknown */
static float g_st_luma       = -1.0f;      /* AE luma or <0 = unavailable */
static int   g_st_mode       = DN_UNKNOWN; /* mode as switched by the thread */
static float g_st_baseline   = -1.0f;      /* night gain baseline or <0 = none */
static float g_st_daytrig    = -1.0f;      /* effective night->day trigger */

/* Effective night->day gain trigger for a given baseline: day_gain_pct% of
 * the baseline when one exists, floored at the fixed day threshold so the
 * adaptive bar can never be STRICTER than the calibrated "definitely day"
 * level (a too-low baseline otherwise yields a trigger no real light source
 * can reach - seen live 2026-08-02, see the hardening comment above
 * DN_BRIGHTEN_CONFIRM_MS). Without a baseline the fixed threshold applies.
 *
 * The floor only applies while it sits BELOW the baseline. Its premise -
 * "gain under total_gain_day_threshold through the night pipeline means
 * daylight" - is disproven the moment a STABLE night baseline itself
 * measures at/below that threshold: cam-wyze-pan (T20/jxf22, 2026-08-11)
 * rests at gain ~256-268 under its own IR in a genuinely dark room (extreme
 * IR return), while the default threshold is 300. Flooring the trigger at
 * 300 then puts it ABOVE the resting night gain, which is a perpetual
 * false "day" verdict: flip to day, find real darkness, flip back, re-trip
 * the oscillation breaker at every freeze expiry, forever. In that inverted
 * regime the trigger stays purely adaptive (day_gain_pct% of the measured
 * resting level), and the "too-low baseline" concern the floor was built
 * for is covered by the probe machinery (periodic reconfirm + sustained
 * brightening), which re-checks through the DAY pipeline - the only
 * trustworthy source in this regime anyway.
 *
 * NOTE (2026-08-12): in the adaptive regime (0 < day_gain_pct < 100) this
 * trigger no longer fires a direct night->day switch at all - night->day is
 * probe-mediated via the brightening hold (see the decision-path comment) -
 * so the value is informational there (status readout, baseline log lines)
 * and only switches directly for pct<=0 (legacy) and pct>=100. */
static float dn_day_trigger(const ms_daynight_cfg *dn, float baseline)
{
    float thr = dn->total_gain_day_threshold;
    if (baseline > 0.0f && dn->day_gain_pct > 0) {
        thr = baseline * (float)dn->day_gain_pct / 100.0f;
        if (thr < dn->total_gain_day_threshold &&
            (float)dn->total_gain_day_threshold < baseline)
            thr = dn->total_gain_day_threshold;
    }
    return thr;
}

static void dn_status_update(const ms_daynight_cfg *dn,
                             float brightness, float total_gain, float ae_luma,
                             int mode, float baseline)
{
    /* last values that woke /events (only touched by the sampling thread) */
    static float nfy_b = -1000.0f, nfy_g = -1000.0f;
    static int   nfy_m = -1000;

    pthread_mutex_lock(&g_st_mu);
    g_st_brightness = brightness;
    g_st_gain       = total_gain;
    g_st_luma       = ae_luma;
    g_st_mode       = mode;
    g_st_baseline   = baseline;
    g_st_daytrig    = (mode == DN_NIGHT) ? dn_day_trigger(dn, baseline) : -1.0f;
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
        /* garbage/out-of-range gain regs (sscanf from /proc/jz/isp) can push
         * exp2f to +inf, which would later print as the literal "inf" -
         * invalid JSON. Rail non-finite results to the -1.0f "unknown"
         * sentinel, same class of guard pflt_cl() applies to config floats. */
        if (!isfinite(*total_gain)) *total_gain = -1.0f;
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
static void dn_switch(int mode, const char *why, const char *cmd)
{
    const char *arg = (mode == DN_NIGHT) ? "night" : "day";
    LOGI(MOD, "switching to %s (%s): %s %s", arg, why, cmd, arg);
    /* F-01: run "<switch_cmd> day|night" via fork()+execlp() instead of
     * system(). switch_cmd comes from the config file; system() would let a
     * value like "reboot; nc ..." inject shell commands (as root). exec'ing it
     * as a single program with the fixed arg "day"/"night" removes the shell
     * entirely - a malicious value just fails to exec, it can't inject. */
    pid_t pid = fork();
    if (pid < 0){ LOGW(MOD,"daynight: fork failed: %s", strerror(errno)); return; }
    if (pid == 0){
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0){ dup2(nul,1); dup2(nul,2); if (nul>2) close(nul); }
        execlp(cmd, cmd, arg, (char*)NULL);
        _exit(127);              /* exec failed (e.g. script missing / not a program) */
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    int rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    if (rc != 0)
        LOGW(MOD, "'%s %s' failed (rc=%d) - is the script installed?", cmd, arg, rc);
}

/* ------------------------------------------------------------------ *
 * Non-sensor decision modes (DN_MODE_TIME / DN_MODE_SUN). Both return
 * DN_DAY / DN_NIGHT / DN_UNKNOWN and feed the SAME switch machinery as the
 * sensor mode (dwell guard included). enabled=0 short-circuits before any of
 * this, so manual mode still forces nothing regardless of mode.             */

/* parse "HH:MM" -> minutes since local midnight [0..1439], or -1 if unset/bad */
static int dn_hhmm_min(const char *s)
{
    if (!s || !s[0]) return -1;
    int h = -1, m = -1;
    if (sscanf(s, "%d:%d", &h, &m) != 2) return -1;
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
}

/* DN_MODE_TIME: force by the local wall clock. Night runs from hhmm_night
 * until hhmm_day; the window may wrap past midnight (night 20:00, day 06:30 =>
 * night start > day start). Both edges must be set, else DN_UNKNOWN. */
static int dn_time_target(const char *hhmm_night, const char *hhmm_day,
                          const struct tm *now)
{
    int n = dn_hhmm_min(hhmm_night);
    int d = dn_hhmm_min(hhmm_day);
    if (n < 0 || d < 0 || n == d) return DN_UNKNOWN;
    int cur = now->tm_hour * 60 + now->tm_min;
    int is_night;
    if (n > d)                          /* wraps midnight: [n..24) U [0..d) */
        is_night = (cur >= n || cur < d);
    else                                /* same day window: [n..d)          */
        is_night = (cur >= n && cur < d);
    return is_night ? DN_NIGHT : DN_DAY;
}

/* Today's sunrise/sunset for lat/lon as epoch time_t (UTC), via the standard
 * low-precision "sunrise equation" (NOAA/Meeus). Everything stays in one time
 * base (epoch seconds); gmtime_r is used ONLY to locate the UTC calendar day
 * of `now` (floored to UTC midnight - a few minutes of skew right at UTC
 * midnight is acceptable for light scheduling). Returns 0 with sr_out/ss_out set on
 * a normal day, +1 for polar day (sun never sets), -1 for polar night (sun
 * never rises) - degenerate cases fall back sanely instead of NaN. */
static int dn_sun_times(float lat, float lon, time_t now,
                        time_t *sr_out, time_t *ss_out)
{
    struct tm g;
    gmtime_r(&now, &g);
    /* floor to UTC midnight without timegm(): subtract the UTC time-of-day */
    time_t midnight = now - (g.tm_hour * 3600 + g.tm_min * 60 + g.tm_sec);

    const double D2R = M_PI / 180.0, R2D = 180.0 / M_PI;
    /* Julian date at that UTC midnight (JD at Unix epoch = 2440587.5) */
    double jd_mid = 2440587.5 + (double)midnight / 86400.0;
    /* integer day count since J2000, corrected toward the day's solar transit */
    double n = floor(jd_mid - 2451545.0 + 0.0008 + 0.5);
    double Jstar = n - lon / 360.0;                    /* lon east-positive */
    double M  = fmod(357.5291 + 0.98560028 * Jstar, 360.0); if (M < 0) M += 360;
    double Mr = M * D2R;
    double C  = 1.9148 * sin(Mr) + 0.0200 * sin(2 * Mr) + 0.0003 * sin(3 * Mr);
    double lambda = fmod(M + C + 180.0 + 102.9372, 360.0); if (lambda < 0) lambda += 360;
    double lr = lambda * D2R;
    double Jtransit = 2451545.0 + Jstar + 0.0053 * sin(Mr) - 0.0069 * sin(2 * lr);
    double decl = asin(sin(lr) * sin(23.44 * D2R));    /* solar declination */
    double latr = lat * D2R;
    /* hour angle at the standard -0.833 deg sunrise/sunset altitude */
    double cosw = (sin(-0.833 * D2R) - sin(latr) * sin(decl)) /
                  (cos(latr) * cos(decl));
    if (cosw < -1.0) return +1;        /* sun always up  -> permanent day   */
    if (cosw >  1.0) return -1;        /* sun always down -> permanent night */
    double w0 = acos(cosw) * R2D;      /* degrees */
    double jrise = Jtransit - w0 / 360.0;
    double jset  = Jtransit + w0 / 360.0;
    if (sr_out) *sr_out = (time_t)((jrise - 2440587.5) * 86400.0 + 0.5);
    if (ss_out) *ss_out = (time_t)((jset  - 2440587.5) * 86400.0 + 0.5);
    return 0;
}

/* DN_MODE_SUN: day between (sunrise+off) and (sunset+off), else night. */
static int dn_sun_target(const ms_daynight_cfg *dn, time_t now)
{
    time_t sr, ss;
    int r = dn_sun_times(dn->sun_latitude, dn->sun_longitude, now, &sr, &ss);
    if (r > 0) return DN_DAY;          /* polar day   */
    if (r < 0) return DN_NIGHT;        /* polar night */
    sr += (time_t)dn->sun_sunrise_offset_min * 60;
    ss += (time_t)dn->sun_sunset_offset_min  * 60;
    return (now >= sr && now < ss) ? DN_DAY : DN_NIGHT;
}

/* P-02: block until the interval elapses OR daynight_stop() requests stop -
 * one wakeup per interval instead of 200 ms slices, same prompt-join latency
 * (the stop broadcast wakes the wait immediately). */
static void dn_sleep(int ms)
{
    ms_stopgate_wait(&g_gate, ms);
}

/* Grace period after the thread (or a re-enable) starts sampling, before the
 * very first day/night decision is trusted. IMP_ISP's AE hasn't converged
 * yet at cold start, so total_gain can read a wild transient (observed:
 * 15000-20000, easily over any reasonable night threshold) for the first
 * fraction of a second - and since the first-ever switch bypasses the normal
 * transition_s dwell (see below), that single bad sample used to commit
 * straight to night on every boot/reboot regardless of actual light.
 *
 * A flat wall-clock delay (daynight.boot_settle_s, floor only) turned out to
 * be insufficient after a firmware reflash: reproduced twice on real T31s
 * (bright room, ~18:00) where the very first switch fired ~90s after thread
 * start, well past the old fixed 5s window - the AE simply took far longer
 * to converge on a freshly-reflashed sensor than on a warm reboot. So the
 * settle wait is now ALSO gated on gain actually being stable: once
 * boot_settle_s has elapsed, keep waiting (up to boot_settle_max_s as a hard
 * cap) until DN_SETTLE_SAMPLES consecutive readings fall within
 * boot_stable_pct% of their average - see dn_ae_stable(). boot_stable_pct=0
 * reverts to the old flat-floor-only behaviour. */
#define DN_SETTLE_SAMPLES 6   /* consecutive readings required to call AE settled */

/* true once `n` (capped at DN_SETTLE_SAMPLES) samples in `hist` (ring buffer,
 * only the last DN_SETTLE_SAMPLES entries matter) all fall within pct% of
 * their average - i.e. the AE loop has stopped moving total_gain around. */
static int dn_ae_stable(const float *hist, int n, int pct)
{
    if (n < DN_SETTLE_SAMPLES || pct <= 0) return 0;
    float mn = hist[0], mx = hist[0], avg = 0.0f;
    for (int i = 0; i < DN_SETTLE_SAMPLES; i++) {
        if (hist[i] < mn) mn = hist[i];
        if (hist[i] > mx) mx = hist[i];
        avg += hist[i];
    }
    avg /= DN_SETTLE_SAMPLES;
    if (avg <= 0.0f) return 0;
    return (mx - mn) <= avg * (float)pct / 100.0f;
}

/* Pre-switch hysteresis (raptor ric_poll_exposure style): the PRIMARY fix for
 * the stuck-mode class of bug. WHY: image.running_mode is pushed into the ISP by
 * the board 'color' script at the instant we switch. If we switch on the very
 * first reading that crosses the threshold, that Set lands while the ISP is still
 * mid its own gain-based day/night transition (the AE gain is ramping fast right
 * at the crossover), and IMP_ISP_Tuning_SetISPRunningMode can be silently dropped
 * (returns 0, config + GET /control + isp-m0 all keep claiming the requested
 * mode, yet the ISP stays in the wrong colour pipeline for hours - grey scene in
 * daylight, or IR-magenta at night). Fix it at the DECISION point: require the
 * target to hold for DN_HYSTERESIS_MS of continuous polling before switching, so
 * by the time we issue the Set the gain has already been stable in the new regime
 * for several seconds and the fast part of the ramp is over. Applied uniformly to
 * both directions and to the day_gain_pct adaptive-baseline night->day trigger
 * (it operates on target!=cur, so it covers every trigger path). Composes with -
 * does not replace - the boot/re-enable settle wait (ignore the cold-start
 * transient right after thread start/re-enable, see dn_ae_stable() below) and
 * transition_s (minimum dwell between switches):
 * cold-start-settle gates seeding the candidate, dwell gates the switch, and the
 * hysteresis confirms the reading is stable, all three ANDed before dn_switch. */
#ifndef DN_HYSTERESIS_MS
#define DN_HYSTERESIS_MS  5000   /* target must hold this long before switching */
#endif

/* Post-switch re-assert, now DEFENSE-IN-DEPTH only (reduced). The pre-switch
 * hysteresis above should keep the Set from landing mid-ramp in the first place;
 * this is a small idempotent safety net kept because the stuck state could not be
 * reproduced on the bench (so hysteresis alone is not yet PROVEN sufficient on
 * this ISP) and the exact timing of the ISP's internal mode flip vs our delayed
 * Set is not fully known. A couple of extra re-drives after the switch cost
 * nothing (SetISPRunningMode is idempotent; hub_control re-applies to the HAL
 * only, no config write). Remove once real dusk/dawn transitions confirm the
 * hysteresis alone holds. Each re-assert reads the CURRENT desired mode from
 * g_cfg (ing_control applies image.* from g_cfg, not the passed value) so apply,
 * value and log agree and a manual override during the window wins. */
#ifndef DN_REASSERT_MS
#define DN_REASSERT_MS    8000   /* interval between re-asserts */
#endif
#ifndef DN_REASSERT_COUNT
#define DN_REASSERT_COUNT 2      /* small safety net -> ~16 s post-switch */
#endif

/* P2 (optimization review 2026-07-31): how often the /proc ISP dump is
 * scraped when the gain comes from the IMP API. dn_brightness() (fopen +
 * ~7 strstr/sscanf per line over the whole isp-m0 dump, incl. kernel-side
 * ISP register reads) used to run EVERY tick, yet whenever
 * hal_isp_total_gain() works (the normal case outside T40/T41) its gain
 * result was immediately overwritten and only the brightness % survived -
 * and that feeds nothing but the /control//events status readout, never the
 * decision. So: poll the cheap gain API every interval_ms as before (the
 * DECISION cadence and the settle/hysteresis/dwell mechanism are untouched
 * by this), and throttle the scrape to a status refresh. When the gain API
 * is unavailable (host sim, T40/T41, ISP down) the scrape IS the decision
 * input (gain, or the averaged-brightness fallback) and keeps running every
 * tick exactly as before. */
#ifndef DN_SCRAPE_MS
#define DN_SCRAPE_MS 5000        /* status-only brightness refresh interval */
#endif

/* Adaptive-baseline hardening (2026-08-02, after two real stuck-in-night
 * incidents in one evening - a basement and a kids' room, both with the room
 * legitimately lit but the camera stuck in mono):
 *
 *  1. A night baseline sampled during a lighting transition is unrepresent-
 *     atively LOW, making day_gain_pct% of it stricter than the room's real
 *     light can ever reach. Fix: while night lasts the baseline drifts UP
 *     toward any higher gain via a small per-tick EMA step - a bad low
 *     sample self-corrects within a minute of true darkness, while a brief
 *     upward transient (headlights leaving, lens shadow) barely moves it.
 *     Downward drift is deliberately NOT done: falling gain is exactly what
 *     the day trigger itself must detect.
 *
 *  2. Even a CLEANLY sampled baseline can defeat a legitimate light source:
 *     a single utility bulb dropped one room's gain to a rock-stable 65% of
 *     baseline - genuinely, visibly brighter - but never below the 60% bar,
 *     so the camera stayed night indefinitely. The honest resolution is the
 *     one night_reconfirm_s already uses: night-pipeline gain is a poor
 *     proxy for "is it day", so PROBE the day pipeline and let its own
 *     calibrated thresholds decide (a wrong probe self-reverts via the
 *     night threshold). When gain holds below the halfway point between
 *     day_gain_pct and 100% of baseline for DN_BRIGHTEN_CONFIRM_MS, fire
 *     that probe early instead of waiting up to night_reconfirm_s. */
/* 60000 -> 30000 (2026-08-12, "switching feels sluggish"): with the direct
 * adaptive night->day switch removed (b4a54f0), this hold IS the entire
 * night->day latency for the everyday "a light came on" event - the smoothed
 * gain crosses the bar within ~2-11 s of the step (DN_SMOOTH_ALPHA=0.1 at a
 * 500 ms tick), so the confirm window was ~85% of the wait. The hold does two
 * jobs and only one of them needs 60 s worth of anything:
 *  - reject transients (don't clunk the IR-cut for passing headlights). Cost
 *    of halving: a brightening lasting 30..60 s now buys one probe pair where
 *    it used to buy none. That is BOUNDED, not open-ended - probe_fail_smooth
 *    latches on the failed probe and the ratchet then demands another full
 *    day_gain_pct%-worth of NEW brightening, so it stays at most ONE pair per
 *    night entry exactly as at 60 s.
 *  - let smooth_tg converge to the dip floor before the ratchet latches on it
 *    (see the dawn analysis at the night->day decision). This is the load-
 *    bearing one, and it is satisfied with room to spare: the EMA residual is
 *    0.9^N, so 30 s (60 ticks) is 99.8% converged vs 99.9998% at 60 s. Traced
 *    against the cam-wyze-pan dawn numbers the ratchet would latch at 838
 *    instead of ~820 - a 2% difference in a bar that sits at 60% of it.
 * Below ~20 s that second job starts to erode (98.5% at 40 ticks), so 30 s is
 * the floor this reasoning supports, not an arbitrary halving. */
#ifndef DN_BRIGHTEN_CONFIRM_MS
#define DN_BRIGHTEN_CONFIRM_MS 30000  /* sustained-brightening probe confirm */
#endif
/* REGRESSION FIX (overnight logs, 4 cameras, v1.7.3): the first version of
 * this hardening drifted the baseline UPWARD toward RAW gain ticks. With a
 * noisy night gain (IR AGC hunting) that ratchets the baseline to the noise
 * envelope's MAXIMUM; ordinary troughs then sit below the probe bar, fire a
 * probe, the (correctly) failed probe reverts, the post-revert baseline is
 * resampled off a still-settling elevated AE reading, and the settled gain
 * again reads "brighter than baseline" - a self-sustaining flap loop every
 * few minutes all night. Three changes break the loop:
 *  - the baseline drifts toward a night-only SMOOTHED gain, symmetric in
 *    both directions (a noise trough/peak cannot ratchet anything, and an
 *    elevated post-revert sample corrects back DOWN);
 *  - the brightening probe only arms on a fresh above-bar -> below-bar
 *    edge of the smoothed gain: after a failed probe the system is below
 *    the bar and DISARMED until the baseline re-converges (bar falls under
 *    the current gain), so a failed probe buys minutes of guaranteed quiet
 *    and only genuine NEW brightening can re-fire;
 *  - for DN_PROBE_SETTLE_MS after a probe switch the day->night revert
 *    candidate is only seeded from STABLE readings (dn_ae_stable): a railed
 *    dark-room reading is stable at once (revert proceeds), but a probe
 *    landing in a lit room can no longer be killed by the AE convergence
 *    ramp right after the pipeline switch. */
#ifndef DN_BASELINE_ALPHA
#define DN_BASELINE_ALPHA 0.002f /* per-tick baseline step toward smoothed gain */
#endif
#ifndef DN_SMOOTH_ALPHA
#define DN_SMOOTH_ALPHA 0.1f     /* per-tick EMA step of the night gain smoother */
#endif
#ifndef DN_PROBE_SETTLE_MS
#define DN_PROBE_SETTLE_MS 8000  /* post-probe: revert only on stable readings */
#endif

/* Dead-zone adoption (live incident 2026-08-03, T31 restart in daylight):
 * from DN_UNKNOWN the sensor decision stays put while gain sits inside the
 * day..night dead-zone (300..3000 default) - by design, and silent. But a
 * boot lands in UNKNOWN with the ISP already running the PERSISTED mode, so
 * "stay put" really means "keep the stale mode with NO self-healing": both
 * reconfirm probes are gated on cur==DN_NIGHT, which UNKNOWN never satisfies.
 * A camera rebooted at 09:23 in broad daylight with a stale night config and
 * a mid-band gain (731) sat rendering night video indefinitely - thread
 * healthy, zero log lines, night_baseline never sampled. Fix: once the boot
 * settle window is over and the reading still cannot decide, ADOPT the
 * persisted running_mode as cur (the ISP is in that mode anyway) so the
 * normal in-mode triggers and probes arm. An adopted mode is a guess, not a
 * measurement, so it gets a scheduled verification EITHER WAY - after
 * min(night_reconfirm_s, DN_ADOPT_PROBE_S), and once even when the periodic
 * reconfirm is disabled: a guess must be verified. See dn_verify below for
 * what "verify" means in each direction. */
#ifndef DN_ADOPT_PROBE_S
#define DN_ADOPT_PROBE_S 300     /* verify an adopted mode within 5 minutes */
#endif

/* Deadline bookkeeping for verifying a GUESSED mode (2026-08-12). Until now
 * the "an unverified decision must be re-checked after a bounded delay" idea
 * existed for night only (a lone night_probe_at_ms deadline), so an adopted
 * DAY was never re-checked at all - and since every other self-healing path in
 * this file is gated on cur==DN_NIGHT, that left one hole with no way out: a
 * camera powered off in daylight, left until after dark, then booted, reads a
 * dead-zone gain, adopts the stale persisted DAY and sits in the day pipeline
 * (IR-cut closed, illuminator off) rendering a black scene indefinitely. The
 * only day-side check is the fixed tg > total_gain_night_threshold crossing,
 * which a dark-but-not-railed scene need never reach. This struct holds the
 * deadline for BOTH directions:
 *   mode  = the mode the deadline belongs to (DN_UNKNOWN = idle). It is only
 *           ever consulted while cur still equals it, so a mode change voids a
 *           stale deadline even before the switch path re-arms or clears it.
 *   at_ms = monotonic ms when the verification comes due (0 = nothing armed).
 *
 * What "verify" MEANS differs per direction, deliberately - the two pipelines
 * are not equally trustworthy, which is the whole reason the probe machinery
 * above exists:
 *   NIGHT: night/IR-pipeline gain is a poor proxy for "is it day" (IR-cut open
 *          means visible+NIR, and the illuminator self-lights the scene), so
 *          verification must physically PROBE the day pipeline and let its
 *          calibrated thresholds decide. That is the existing, incident-tested
 *          mechanism further down - unchanged, it merely reads its deadline
 *          from here now.
 *   DAY:   day-pipeline gain IS the trustworthy metric - it is precisely what
 *          every night probe switches TO in order to obtain an honest reading -
 *          so no physical probe is needed or wanted here: just re-read it at
 *          the deadline. The guess is CONFIRMED when the reading would have
 *          decided day on its own from DN_UNKNOWN (gain below
 *          total_gain_day_threshold, or in the brightness fallback above the
 *          narrowed threshold_high band); the deadline is then dropped with no
 *          switch and no IR-cut click. If the reading is still ambiguous, the
 *          guess failed to confirm and night is targeted through the ORDINARY
 *          switch path below (dwell + hysteresis + oscillation breaker all
 *          apply, nothing special-cased), where the self-healing probes then
 *          re-check it properly. Night is the safe side of that coin: a wrong
 *          night self-corrects within one probe cycle, a wrong day is exactly
 *          the state that has no way back. */
typedef struct {
    int     mode;     /* DN_DAY / DN_NIGHT this deadline verifies, else DN_UNKNOWN */
    int64_t at_ms;    /* monotonic ms when it comes due (0 = nothing armed) */
} dn_verify;

static void dn_verify_arm(dn_verify *v, int mode, int64_t at_ms)
{
    v->mode  = mode;
    v->at_ms = at_ms;
}

static void dn_verify_clear(dn_verify *v)
{
    v->mode  = DN_UNKNOWN;
    v->at_ms = 0;
}

/* true once the deadline armed for `mode` has come due while still in `mode` */
static int dn_verify_due(const dn_verify *v, int mode, int64_t now_ms)
{
    return v->mode == mode && v->at_ms > 0 && now_ms >= v->at_ms;
}

/* bounded delay before an ADOPTED (guessed) mode must have proved itself: the
 * earlier of the configured reconfirm interval and DN_ADOPT_PROBE_S, and
 * DN_ADOPT_PROBE_S flat when the periodic reconfirm is disabled entirely.
 * Shared by both directions so a guess costs the same wait either way - no new
 * tunable, the same timing the adopted-night probe has always used. */
static int64_t dn_adopt_verify_s(const ms_daynight_cfg *dn)
{
    int64_t s = DN_ADOPT_PROBE_S;
    if (dn->night_reconfirm_s > 0 && (int64_t)dn->night_reconfirm_s < s)
        s = dn->night_reconfirm_s;
    return s;
}

/* Probe economy (fleet logs 2026-08-03/04, all 11 cameras): every reconfirm
 * probe is USER-VISIBLE - the board script clunks the IR-cut, kills the IR
 * LEDs and the stream shows ~7-9 s of dark colour video before the revert.
 * The hourly periodic probe alone produced 8-12 such flips per camera per
 * night ("periodische Tag/Nacht-Umschaltungen"), and the sustained-
 * brightening probe added 2-6 more on slow-ramp scenes (pre-dawn: gain
 * declines continuously, each failed probe resamples a LOWER baseline, the
 * ramp re-crosses the new bar, repeat every 10-40 min - with tangent starts
 * like 4898 vs bar 4906). Three measures, all pure probe-scheduling logic:
 *  - exponential backoff: a probe that FAILS (reverts to night within
 *    DN_PROBE_FAIL_WINDOW_MS) doubles the periodic interval, x1 -> x2 -> x4
 *    (DN_PROBE_BACKOFF_MAX), bounded by max(night_reconfirm_s,
 *    DN_PROBE_BACKOFF_CAP_S); any genuine transition or a probe that STICKS
 *    resets it. Self-healing keeps its first-hour probe; confirmed darkness
 *    stops clunking hourly.
 *  - arming margin: the brightening hold only starts once the smoothed gain
 *    is below DN_BRIGHTEN_MARGIN of the bar, never on a tangent graze.
 *  - failure ratchet: after a failed probe the next brightening probe also
 *    requires smooth gain below day_gain_pct% of the level that just failed
 *    - i.e. another full trigger-worth of NEW brightening. A slow ramp gets
 *    at most a couple of well-spaced probes across the whole night; a real
 *    light-on step still passes immediately. */
#ifndef DN_PROBE_FAIL_WINDOW_MS
#define DN_PROBE_FAIL_WINDOW_MS 30000 /* revert within this = failed probe */
#endif
#ifndef DN_PROBE_BACKOFF_MAX
#define DN_PROBE_BACKOFF_MAX 4        /* interval multiplier cap (1h->2h->4h) */
#endif
#ifndef DN_PROBE_BACKOFF_CAP_S
#define DN_PROBE_BACKOFF_CAP_S 14400  /* absolute backoff ceiling (4 h) */
#endif
#ifndef DN_BRIGHTEN_MARGIN
#define DN_BRIGHTEN_MARGIN 0.97f      /* hold arms only clearly below the bar */
#endif

/* Passive-evidence gate for the PERIODIC reconfirm probe (cam-wyze, closet in
 * constant darkness, 2026-08-04: "das klacken der IR blende nervt ... nachts
 * andauernd"). The backoff above cut the FREQUENCY of the periodic probe, but
 * every time it fires it still physically drives the board day/night script -
 * the IR-cut filter is a mechanical relay whose move is AUDIBLE. On a camera
 * that sits in genuinely unchanging darkness for hours or days, a blind
 * scheduled probe accomplishes nothing but that clunk: the log evidence that
 * night proved it (2026-08-04, cam-wyze: 1 h probe fired, day-pipeline gain
 * 1002, reverted in 9 s, exactly as designed - and pointless, the passive
 * night gain had been 988->1002 all interval, nowhere near the day bar).
 *
 * So the periodic probe now only physically fires when there is passive reason
 * to suspect the state changed - the SAME signal the sustained-brightening
 * probe already uses: is the smoothed night gain clearly below the probe bar
 * (DN_BRIGHTEN_MARGIN of night_baseline * (100+day_gain_pct)/200)? If the gain
 * is still solidly deep in night territory, SKIP the physical switch this
 * cycle - no dn_switch, no IR-cut click - and silently re-arm on the same
 * backoff schedule. This is not a weakening of self-healing: a FALSE night
 * latch (actually daytime behind an engaged IR pipeline) reads LOW gain, which
 * is exactly the evidence that fires the probe; only a genuinely-dark scene,
 * where a probe could only fail anyway, is skipped.
 *
 * The safety net that survives a permanently-flat gain (a truly stuck reading
 * that evidence alone can never clear) is daynight.probe_max_skip_s: once this
 * long has passed since the last ACTUAL physical probe, fire regardless of
 * gain - "trust nothing, double-check". Default 12h => at most ~2 physical
 * clicks/day under permanent darkness (vs up to 6/day at the 4h backoff cap
 * before this), while the very first probe after each night entry still
 * always fires (skip only ever applies to a follow-up probe, so the
 * stuck-forever class stays covered within the first interval as before).
 * Configurable (2026-08-05, config.h doc comment) - deliberately floored well
 * above zero in config.c's validation table; this is a safety net, not a
 * feature meant to be switched off. */

/* Oscillation breaker (feedback-loop backstop, added 2026-08-04). A camera
 * mounted very close (~30 cm) to a reflective object hits a PHYSICAL feedback
 * loop that none of the probe-economy logic above can see, because it happens
 * on the PRIMARY threshold crossings, not on a probe: night -> IR LED on ->
 * the LED reflects intensely off the close object -> AGC gain reads very low
 * ("bright") -> genuine night->day crossing (tg < day trigger) -> IR LED off +
 * colour pipeline -> but it is actually still dark -> gain rails back up ->
 * genuine day->night crossing (tg > night threshold) -> IR LED on again ->
 * repeat, flipping every few seconds indefinitely (each flip clunks the
 * IR-cut). This is deliberately a GENERAL backstop for ANY fast day/night
 * oscillation, not IR-specific detection: count GENUINE (non-probe) mode flips
 * in a rolling window and, if DN_OSC_FLIPS of them land within
 * DN_OSC_WINDOW_MS, declare an oscillation and FREEZE in the last-decided mode
 * for DN_OSC_FREEZE_MS, suppressing both switches AND probes so the loop
 * cannot continue during the cooldown.
 *
 * CRITICAL: probe-driven flips are NOT counted. A reconfirm/brightening probe
 * that switches to day and then reverts on failure is a normal, INTENTIONAL
 * 2-flip event under the probe-economy design above (at most one such pair per
 * backoff-scheduled probe, i.e. once per hour+ normally); it must never look
 * like oscillation. Only the genuine main-switch threshold crossings feed the
 * counter (the probe fire and the probe-failure revert both bypass it), so a
 * probe cycle contributes ZERO to the count and can never trip the breaker.
 * With DN_OSC_FLIPS=3 even two such probe cycles inside one window stay under
 * the bar; a real IR-reflection loop, whose crossings are all genuine, trips
 * on the third flip (~15 s in at the 5 s hysteresis). */
#ifndef DN_OSC_WINDOW_MS
#define DN_OSC_WINDOW_MS 60000   /* rolling window for the genuine-flip count */
#endif
#ifndef DN_OSC_FLIPS
#define DN_OSC_FLIPS 3           /* genuine flips within the window = oscillation */
#endif
#ifndef DN_OSC_FREEZE_MS
#define DN_OSC_FREEZE_MS 600000  /* hold the last mode this long after detection */
#endif

static void *dn_thread(void *arg)
{
    (void)arg;
    int    cur = DN_UNKNOWN;            /* mode as switched by US */
    int    was_enabled = 0;
    int    warned_noisp = 0;
    float  hist[DN_SAMPLES];
    int    hidx = 0;
    int64_t last_switch_ms = 0;
    /* boot/re-enable settle: floor + gain-stability extension, see
     * dn_ae_stable() and the DN_SETTLE_SAMPLES comment above. These live ints
     * are runtime-mutable via /control, so read them under the config lock like
     * every other tunable snapshot below (the values are anyway recomputed from
     * the per-iteration snapshot in the (re)enable block before first use). */
    config_str_lock();
    int boot_settle_s0     = g_cfg.daynight.boot_settle_s;
    int boot_settle_max_s0 = g_cfg.daynight.boot_settle_max_s;
    config_str_unlock();
    int64_t settle_floor_ms = ms_now_us() / 1000 + (int64_t)boot_settle_s0 * 1000;
    int64_t settle_hard_ms  = ms_now_us() / 1000 + (int64_t)boot_settle_max_s0 * 1000;
    float   settle_hist[DN_SETTLE_SAMPLES];
    int     settle_n = 0;
    /* pre-switch hysteresis (see DN_HYSTERESIS_MS): the candidate target we are
     * currently confirming and when it first appeared. DN_UNKNOWN = no candidate. */
    int     pending_target  = DN_UNKNOWN;
    int64_t pending_since_ms = 0;
    /* pending post-switch running-mode re-asserts (see DN_REASSERT_MS). The next
     * one fires at reassert_at_ms (0 = none pending); reassert_left counts down. */
    int64_t reassert_at_ms = 0;
    int     reassert_left  = 0;
    /* adaptive night baseline (raptor-style): gain sampled once, baseline_delay_s
     * after entering night (IR LEDs settled); night->day then triggers relative
     * to it. -1 = not sampled yet. */
    float   night_baseline = -1.0f;
    int64_t night_entered_ms = 0;
    /* pending verification deadline for the mode we are currently in (see
     * dn_verify): in NIGHT it schedules the self-healing day-pipeline probe
     * (periodic reconfirm - see night_reconfirm_s in config.h - or the
     * one-shot check of an adopted night); in DAY it schedules the gain
     * re-read that must confirm an adopted day. Idle = nothing pending. */
    dn_verify verify = { DN_UNKNOWN, 0 };
    /* sustained-brightening probe (see DN_BRIGHTEN_CONFIRM_MS): when gain
     * first held below the probe bar while in night. 0 = not holding. */
    int64_t brighten_since_ms = 0;
    /* the probe only arms on a fresh above-bar -> below-bar edge of the
     * smoothed gain (see the DN_BASELINE_ALPHA regression comment). */
    int     brighten_armed = 0;
    /* night-only smoothed gain (DN_SMOOTH_ALPHA EMA): drives the baseline
     * drift and the brightening comparison so AGC noise cannot ratchet
     * either. Reset on every night entry so day/probe readings (railed max
     * gain in a dark IR-less room) never pollute it. -1 = no sample yet. */
    float   smooth_tg = -1.0f;
    /* when the last probe switched to day (0 = none recent): gates the
     * day->night revert on stable readings for DN_PROBE_SETTLE_MS. */
    int64_t probe_day_ms = 0;
    /* one-shot deadline at which a probe's OWN outcome is classified (see the
     * three-outcome comment at the probe fire): the day-pipeline reading has
     * settled by then, so it either confirmed day, reverted, or landed
     * ambiguous - only the last arms a verify. 0 = nothing outstanding. */
    int64_t probe_verdict_at_ms = 0;
    /* probe economy (see DN_PROBE_FAIL_WINDOW_MS): periodic-interval
     * multiplier, doubled on every failed probe up to DN_PROBE_BACKOFF_MAX,
     * reset to 1 by any genuine transition or a probe that sticks. */
    int     probe_backoff = 1;
    /* smoothed night gain at the moment the last FAILED probe fired; a new
     * brightening probe must undercut day_gain_pct% of this (failure
     * ratchet). -1 = no failed probe outstanding. */
    float   probe_fail_smooth = -1.0f;
    /* monotonic ms of the last time a probe ACTUALLY drove the board (physical
     * IR-cut click), periodic or brightening. Feeds the probe_max_skip_s
     * outer bound so evidence-skipped periodic probes cannot silently disable
     * self-healing forever. 0 = none this session (first probe always fires). */
    int64_t last_phys_probe_ms = 0;
    /* oscillation breaker (see DN_OSC_WINDOW_MS): monotonic timestamps of the
     * last DN_OSC_FLIPS GENUINE (non-probe) mode flips (ring, only the last
     * DN_OSC_FLIPS matter), and the deadline until which switches and probes
     * are frozen after a detected feedback loop (0 = not frozen). */
    int64_t osc_hist[DN_OSC_FLIPS];
    int     osc_n = 0;
    int64_t osc_freeze_until_ms = 0;
    /* baseline value last reported in a log line (rate-limits the drift
     * INFO log to meaningful moves, see the EMA drift below). */
    float   baseline_logged = -1.0f;
    /* P2: last /proc-scraped brightness (status readout only when the gain
     * API works) and when the next scrape is due. 0 = scrape on first tick. */
    float   scraped_b = -1.0f;
    int64_t next_scrape_ms = 0;

    for (int i = 0; i < DN_SAMPLES; i++) hist[i] = 50.0f;  /* neutral start */

    /* the /control server is already up here: snapshot the whole runtime-
     * mutable daynight config once under the config string lock (see config.c)
     * and log from the local copy, so none of these tunables (numeric or the
     * time-window strings) are read lock-free - same M10 whole-item snapshot
     * pattern the loop below and imp_osd.c's refresh_text() use. */
    { ms_daynight_cfg dn0;
      config_str_lock();
      dn0 = g_cfg.daynight;
      config_str_unlock();
      const char *ms = dn0.mode==DN_MODE_TIME ? "time"
                     : dn0.mode==DN_MODE_SUN  ? "sun" : "sensor";
      LOGI(MOD, "detection thread started (mode=%s, time night=%s day=%s, "
                "sun lat=%g lon=%g off rise=%d set=%d min)",
           ms, dn0.time_night_start[0]?dn0.time_night_start:"-",
           dn0.time_day_start[0]?dn0.time_day_start:"-",
           (double)dn0.sun_latitude, (double)dn0.sun_longitude,
           dn0.sun_sunrise_offset_min, dn0.sun_sunset_offset_min);
      LOGI(MOD, "detection thread started (gain day<%g night>%g, "
                "brightness fallback %.1f/%.1f hyst %.2f, "
                "interval %dms, dwell %ds, isp=%s, cmd=%s, "
                "boot settle %ds/%ds stable<%d%%, night reconfirm %ds)",
           (double)dn0.total_gain_day_threshold,
           (double)dn0.total_gain_night_threshold,
           dn0.threshold_low, dn0.threshold_high,
           dn0.hysteresis, dn0.interval_ms,
           dn0.transition_s, dn0.isp_path, dn0.switch_cmd,
           dn0.boot_settle_s, dn0.boot_settle_max_s,
           dn0.boot_stable_pct, dn0.night_reconfirm_s); }

    while (!ms_stopgate_stopped(&g_gate)) {
        /* M10 whole-struct snapshot (mirrors imp_osd.c refresh_text()): every
         * daynight tunable is runtime-mutable via /control, which applies
         * changes under the config string lock (see config.c). Snapshot the
         * whole struct plus the live image.running_mode once per poll, then
         * operate on the local copy for the rest of the iteration - so one
         * decision never mixes a freshly-set threshold with a stale interval,
         * and none of these ints/enums/strings are read lock-free mid-update
         * (the same C11 data-race class as the audio.mute fix). One lock+copy
         * per ~500ms poll is negligible. */
        ms_daynight_cfg dncfg;
        int running_mode;
        config_str_lock();
        dncfg        = g_cfg.daynight;
        running_mode = g_cfg.image.running_mode;
        config_str_unlock();
        const ms_daynight_cfg *dn = &dncfg;
        int interval = dn->interval_ms > 0 ? dn->interval_ms : 500;

        /* fire a pending post-switch running-mode re-assert once it comes due,
         * then re-arm the next one until the series is exhausted (see
         * DN_REASSERT_MS / DN_REASSERT_COUNT). Dropped if auto was turned off in
         * the meantime - the user may have taken manual control of the mode. */
        if (reassert_at_ms && ms_now_us() / 1000 >= reassert_at_ms) {
            if (dn->enabled) {
                /* apply the CURRENT desired mode: ing_control applies image.*
                 * from g_cfg, so read it here too (N3) - value, apply and log all
                 * agree, and a manual override during the window is honoured
                 * (running_mode is this iteration's under-lock snapshot). */
                int rm = running_mode ? 1 : 0;
                hub_control("image.running_mode", rm ? "1" : "0");
                LOGI(MOD, "re-asserting running_mode=%d after switch (%d left)",
                     rm, reassert_left - 1);
                /* Divergence check: timps itself never writes running_mode -
                 * the board hook chain does (switch_cmd -> 'color' script ->
                 * POST /control, see daynight.h). That chain is fire-and-
                 * forget ('color ... &' in the board script, curl output
                 * discarded), so a lost/failed POST leaves the decision and
                 * the ISP silently desynced until the next transition. By
                 * this first re-assert (DN_REASSERT_MS after the switch) the
                 * ~1s hook latency measured on real boards has long passed:
                 * a still-mismatched running_mode means the hook chain did
                 * not complete (script missing, control disabled in
                 * thingino.json, curl failed) OR the user manually overrode
                 * the mode - either way it deserves a visible line, not
                 * silence. Deliberately WARN-only: re-running switch_cmd
                 * here would clobber a legitimate manual override (which the
                 * re-assert above intentionally honours). */
                if ((cur == DN_NIGHT && !rm) || (cur == DN_DAY && rm))
                    LOGW(MOD, "running_mode=%d never followed the switch to "
                              "%s - board hook chain (switch_cmd -> color -> "
                              "POST /control) incomplete, or manual override",
                         rm, cur == DN_NIGHT ? "night" : "day");
            }
            if (--reassert_left > 0)
                reassert_at_ms = ms_now_us() / 1000 + DN_REASSERT_MS;
            else
                reassert_at_ms = 0;
        }

        /* sample even in manual mode so GET /control always reports the live
         * brightness/total_gain (WebUI gain display + data collector).
         * total_gain: prefer the ISP's own IMP_ISP_Tuning_GetTotalGain (robust,
         * like prudynt/raptor) and fall back to the /proc/isp-m0 scrape only
         * when the API is unavailable (host sim, T40/T41, ISP down). Brightness
         * still comes from the scrape (no direct luma API used yet), but that
         * scrape is throttled to DN_SCRAPE_MS while the gain API works (P2) -
         * the gain the DECISION uses is still sampled every tick either way. */
        float tg = -1.0f;
        int   api_gain = 0;
        { uint32_t hg;
          if (hal_isp_total_gain(&hg) == 0) { tg = (float)hg; api_gain = 1; } }
        float b = scraped_b;
        if (!api_gain || ms_now_us() / 1000 >= next_scrape_ms) {
            float stg = -1.0f;
            b = scraped_b = dn_brightness(dn->isp_path, &stg);
            if (!api_gain) tg = stg;
            next_scrape_ms = ms_now_us() / 1000 + DN_SCRAPE_MS;
        }
        float luma = -1.0f;
        { uint32_t al; if (hal_isp_ae_luma(&al) == 0) luma = (float)al; }

        if (!dn->enabled) {             /* manual mode: measure, force nothing */
            was_enabled = 0;
            cur = DN_UNKNOWN;           /* mode may be forced manually now */
            night_baseline = -1.0f;
            brighten_since_ms = 0; brighten_armed = 0;
            smooth_tg = -1.0f; probe_day_ms = 0; probe_verdict_at_ms = 0;
            pending_target = DN_UNKNOWN; pending_since_ms = 0;
            dn_status_update(dn, b, tg, luma, DN_UNKNOWN, night_baseline);
            dn_sleep(interval);
            continue;
        }
        if (!was_enabled) {             /* (re)enabled: detect from scratch */
            was_enabled = 1;
            cur = DN_UNKNOWN;           /* mode may have been set manually */
            night_baseline = -1.0f;
            night_entered_ms = 0;
            dn_verify_clear(&verify);
            brighten_since_ms = 0; brighten_armed = 0;
            smooth_tg = -1.0f; probe_day_ms = 0; probe_verdict_at_ms = 0;
            probe_backoff = 1; probe_fail_smooth = -1.0f;
            last_phys_probe_ms = 0;
            osc_n = 0; osc_freeze_until_ms = 0;
            pending_target = DN_UNKNOWN; pending_since_ms = 0;
            settle_floor_ms = ms_now_us() / 1000 + (int64_t)dn->boot_settle_s * 1000;
            settle_hard_ms  = ms_now_us() / 1000 + (int64_t)dn->boot_settle_max_s * 1000;
            settle_n = 0;
            for (int i = 0; i < DN_SAMPLES; i++) hist[i] = 50.0f;
            LOGI(MOD, "auto day/night enabled");
        }

        if (dn->mode == DN_MODE_SENSOR && b < 0.0f && tg < 0.0f) {
                                        /* no ISP (sim/host): skip silently.
                                         * time/sun modes don't need the ISP,
                                         * so only the sensor path idles here. */
            if (!warned_noisp) {
                /* one-shot, so this is not spam - but it used to be LOGD
                 * (invisible in production): a permanently wedged ISP leaves
                 * detection idling forever with no self-heal (visible only
                 * indirectly via /control's brightness/total_gain=-1), so
                 * this deserves to actually reach the log at default level. */
                LOGW(MOD, "%s not readable, detection idle", dn->isp_path);
                warned_noisp = 1;
            }
            dn_status_update(dn, b, tg, luma, cur, night_baseline);
            dn_sleep(interval);
            continue;
        }
        warned_noisp = 0;

        /* night-only smoothed gain: the baseline drift and the brightening
         * probe read this instead of raw ticks (see the DN_BASELINE_ALPHA
         * regression comment). Only night readings feed it - it is reset on
         * every night entry so the railed day-pipeline gain of a failed
         * probe can never leak in. */
        if (dn->mode == DN_MODE_SENSOR && cur == DN_NIGHT && tg >= 0.0f)
            smooth_tg = (smooth_tg > 0.0f)
                ? smooth_tg + (tg - smooth_tg) * DN_SMOOTH_ALPHA : tg;

        /* self-healing reconfirm probes: gain sampled through the night/IR
         * pipeline is not a reliable proxy for "is it actually day" (IR-cut
         * changes the optical path and the ISP night tuning table can target
         * a different AE point - see night_reconfirm_s doc comment in
         * config.h), so force a real probe switch to day and let the normal
         * hysteresis re-decide from a true day-pipeline reading instead of
         * trusting a possibly-stuck night-path one. Two triggers share the
         * probe:
         *  - periodic: after night_reconfirm_s of continuous night dwell;
         *  - sustained brightening: gain held below the halfway point
         *    between day_gain_pct% and 100% of the baseline for
         *    DN_BRIGHTEN_CONFIRM_MS (a real light came on but not enough to
         *    cross the strict adaptive bar - see the hardening comment above
         *    DN_BRIGHTEN_CONFIRM_MS), gated on the transition_s dwell so a
         *    probe that reverts cannot flap. */
        if (cur == DN_NIGHT && dn->mode == DN_MODE_SENSOR) {
            int64_t now_ms = ms_now_us() / 1000;
            const char *probe_why = NULL;
            /* the night verification deadline is armed by night entry (when
             * reconfirm is enabled) or by dead-zone adoption (one-shot, even
             * when it is not - see DN_ADOPT_PROBE_S); honour it whenever set. */
            if (dn_verify_due(&verify, DN_NIGHT, now_ms)) {
                /* passive-evidence gate (see probe_max_skip_s in config.h): only spend
                 * an audible IR-cut click when the passive night gain gives a
                 * reason to suspect the state changed. If we have a baseline +
                 * smoothed gain and the gain is still solidly deep in night
                 * (>= DN_BRIGHTEN_MARGIN of the same probe bar the brightening
                 * hold uses), there is nothing to resolve - skip the physical
                 * probe and silently re-arm on the same backoff schedule.
                 * Fire anyway when we cannot judge (no baseline yet - e.g. the
                 * dead-zone adoption one-shot), on the first probe of the
                 * session (last_phys_probe_ms==0), or once probe_max_skip_s
                 * has elapsed since the last actual physical probe (the
                 * trust-nothing safety net for a permanently-flat reading). */
                float probe_bar = (night_baseline > 0.0f)
                    ? night_baseline * (100.0f + (float)dn->day_gain_pct) / 200.0f
                    : 0.0f;
                int can_judge = (night_baseline > 0.0f && smooth_tg > 0.0f);
                int solidly_night = can_judge &&
                    smooth_tg >= probe_bar * DN_BRIGHTEN_MARGIN;
                int outer_bound_due = (last_phys_probe_ms == 0) ||
                    (now_ms - last_phys_probe_ms >=
                     (int64_t)dn->probe_max_skip_s * 1000);
                if (solidly_night && !outer_bound_due) {
                    int64_t iv  = (int64_t)dn->night_reconfirm_s * probe_backoff;
                    int64_t cap = dn->night_reconfirm_s > DN_PROBE_BACKOFF_CAP_S
                                ? (int64_t)dn->night_reconfirm_s
                                : (int64_t)DN_PROBE_BACKOFF_CAP_S;
                    if (iv > cap) iv = cap;
                    dn_verify_arm(&verify, DN_NIGHT, now_ms + iv * 1000);
                    LOGI(MOD, "periodic reconfirm due but gain %.0f still deep "
                              "in night (bar %.0f, baseline %.0f) - skipping "
                              "IR-cut probe, re-arm in %llds (%llds since last "
                              "physical probe, force at %ds)",
                         (double)smooth_tg, (double)probe_bar,
                         (double)night_baseline, (long long)iv,
                         last_phys_probe_ms > 0 ?
                             (long long)((now_ms - last_phys_probe_ms) / 1000)
                             : 0LL, dn->probe_max_skip_s);
                } else {
                    probe_why = "periodic reconfirm probe";
                }
            }
            if (!probe_why && night_baseline > 0.0f && smooth_tg > 0.0f &&
                dn->day_gain_pct > 0 && dn->day_gain_pct < 100) {
                float probe_bar = night_baseline *
                    (100.0f + (float)dn->day_gain_pct) / 200.0f;
                if (smooth_tg >= probe_bar) {
                    /* above the bar: (re)arm - only a fresh above->below
                     * edge may start a hold. After a failed probe the scene
                     * sits below the bar DISARMED until the baseline drift
                     * converges and the bar drops under the current gain,
                     * so identical darkness can never re-fire. */
                    brighten_armed = 1;
                    brighten_since_ms = 0;
                } else if (brighten_armed) {
                    if (!brighten_since_ms) {
                        /* hold-start gates (see the probe-economy comment):
                         *  - margin: clearly below the bar, never a tangent
                         *    graze (fleet logs: holds starting 0.2% under);
                         *  - failure ratchet: after a failed probe, require
                         *    another full trigger-worth of NEW brightening
                         *    below the level that already failed, or a slow
                         *    ramp re-fires every time it re-crosses the
                         *    freshly resampled baseline's bar. */
                        if (smooth_tg < probe_bar * DN_BRIGHTEN_MARGIN &&
                            (probe_fail_smooth <= 0.0f ||
                             smooth_tg < probe_fail_smooth *
                                         (float)dn->day_gain_pct / 100.0f)) {
                            brighten_since_ms = now_ms;
                            LOGI(MOD, "sustained brightening: gain %.0f below probe "
                                      "bar %.0f (baseline %.0f), confirming %ds",
                                 (double)smooth_tg, (double)probe_bar,
                                 (double)night_baseline,
                                 DN_BRIGHTEN_CONFIRM_MS / 1000);
                        }
                    } else if (now_ms - brighten_since_ms >=
                                   (int64_t)DN_BRIGHTEN_CONFIRM_MS &&
                               now_ms - last_switch_ms >=
                                   (int64_t)dn->transition_s * 1000)
                        probe_why = "sustained brightening probe";
                }
            }
            /* Pre-baseline day-trigger PROBE: before the night baseline has
             * been planted (adaptive mode only), a gain reading under the
             * static day threshold must not full-switch to day - through the
             * night/IR pipeline it is ambiguous between "lights came on" and
             * "strong IR return in darkness" (cam-wyze-pan rests at ~256
             * under IR vs the 300 threshold; the resulting instant full
             * switches re-tripped the oscillation breaker forever). Fire the
             * PROBE machinery instead: a genuine lights-on sticks in day
             * within seconds; darkness reverts cheaply (probe pairs are
             * excluded from the breaker) and arms the probe_fail_smooth
             * ratchet, which blocks an identical re-fire on the next night
             * entry - the loop terminates after one probe pair, and the
             * baseline then plants at the true resting level. The direct
             * switch is suppressed in the decision path below for the WHOLE
             * adaptive regime, not just this pre-baseline window - the
             * post-baseline crossing has the same night-pipeline ambiguity
             * (dawn through the open IR-cut, cam-wyze-pan 2026-08-12; see
             * the comment at the night->day decision below). */
            if (!probe_why && night_baseline < 0.0f && dn->day_gain_pct > 0 &&
                smooth_tg > 0.0f &&
                smooth_tg < (float)dn->total_gain_day_threshold &&
                now_ms - last_switch_ms >= (int64_t)dn->transition_s * 1000 &&
                (probe_fail_smooth <= 0.0f ||
                 smooth_tg < probe_fail_smooth *
                             (float)dn->day_gain_pct / 100.0f))
                probe_why = "pre-baseline day-trigger probe";
            /* oscillation cooldown (see DN_OSC_WINDOW_MS): while frozen,
             * suppress probes too - a probe would flip the very mode the
             * freeze is holding and restart the loop. */
            if (probe_why && osc_freeze_until_ms && now_ms < osc_freeze_until_ms) {
                LOGD(MOD, "oscillation freeze: suppressing %s (%llds left)",
                     probe_why,
                     (long long)((osc_freeze_until_ms - now_ms) / 1000));
                probe_why = NULL;
            }
            if (probe_why) {
                LOGI(MOD, "night reconfirm (%s): probing day pipeline after "
                          "%llds dwell (gain %.0f, baseline %.0f)", probe_why,
                     (long long)((now_ms - night_entered_ms) / 1000),
                     (double)tg, (double)night_baseline);
                dn_switch(DN_DAY, probe_why, dn->switch_cmd);
                cur = DN_DAY;
                last_switch_ms  = now_ms;
                pending_target  = DN_UNKNOWN; pending_since_ms = 0;
                reassert_left   = DN_REASSERT_COUNT;
                reassert_at_ms  = now_ms + DN_REASSERT_MS;
                night_baseline  = -1.0f;
                night_entered_ms = 0;
                /* the probe supersedes the night deadline that scheduled it;
                 * its OWN outcome is judged by probe_verdict_at_ms below. */
                dn_verify_clear(&verify);
                brighten_since_ms = 0;
                brighten_armed  = 0;    /* re-arms above the bar next night */
                probe_day_ms    = now_ms; /* gate the revert on stability */
                /* A probe has THREE possible outcomes, not two. It either
                 * confirms day (day-pipeline gain below the day threshold -
                 * it sticks, and nothing further is scheduled) or reverts
                 * (gain above the night threshold - the ordinary crossing
                 * below). The third is a day-pipeline reading that lands in
                 * the dead-zone BETWEEN them: it neither confirms nor
                 * reverts, so before this the camera simply stayed in the
                 * probed day with nothing pending - the same stuck class the
                 * dead-zone adoption verify exists for, reached through a
                 * probe instead of a boot. Judge that outcome once, after the
                 * AE has settled on the new pipeline (DN_PROBE_SETTLE_MS,
                 * ~2x the observed post-probe convergence ramp), and only
                 * then arm a verify for the ambiguous case. */
                probe_verdict_at_ms = now_ms + DN_PROBE_SETTLE_MS;
                last_phys_probe_ms = now_ms; /* outer-bound clock, see probe_max_skip_s in config.h */
                dn_status_update(dn, b, tg, luma, cur, night_baseline);
                dn_sleep(interval);
                continue;
            }
        }

        int  target = cur;
        char why[64];
        /* set when an UNVERIFIED day (adopted from the persisted mode, or
         * landed on by a probe with an ambiguous outcome) failed to confirm
         * itself by its deadline - see dn_verify. Drives the switch REASON
         * string and the failed-probe accounting; the switch itself goes
         * through the ordinary path like any other target. */
        int  day_unverified = 0;
        /* keep the stability ring fed in sensor mode whenever gain is
         * readable: the boot settle (cur==UNKNOWN) and the post-probe
         * revert gate below both read it. Cheap: one ring write per tick. */
        if (dn->mode == DN_MODE_SENSOR && tg >= 0.0f) {
            settle_hist[settle_n % DN_SETTLE_SAMPLES] = tg;
            settle_n++;
        }
        if (dn->mode == DN_MODE_TIME) {
            /* fixed local-clock window (localtime_r: this IS the user's wall
             * clock). Reuses the same switch/dwell machinery below.
             * time_night_start/time_day_start are runtime-mutable via /control,
             * but dncfg already holds a consistent copy taken under the config
             * string lock at the top of the loop (see the M10 snapshot comment),
             * so they are safe to read directly here - no torn/non-terminated
             * buffer, no need for a second lock. */
            time_t now = time(NULL); struct tm lt; localtime_r(&now, &lt);
            target = dn_time_target(dn->time_night_start, dn->time_day_start, &lt);
            snprintf(why, sizeof why, "clock %02d:%02d", lt.tm_hour, lt.tm_min);
        } else if (dn->mode == DN_MODE_SUN) {
            /* today's real sunrise/sunset (+offsets) for the configured location */
            time_t now = time(NULL);
            target = dn_sun_target(dn, now);
            snprintf(why, sizeof why, "sun schedule");
        } else if (tg >= 0.0f) {
            /* PRIMARY: total_gain, prudynt/raptor semantics. INVERTED vs
             * brightness: high gain = dark = night. The day..night threshold
             * gap (300..3000 by default) is the hysteresis dead-zone, so no
             * averaging is needed; from UNKNOWN a mid-gap start stays put
             * until the gain leaves the dead-zone. */
            /* night->day: relative to the adaptive night baseline when we have
             * one (day when gain < day_gain_pct% of it, floored at the fixed
             * day threshold - see dn_day_trigger()), else the fixed day
             * threshold. day->night stays the fixed night threshold. */
            float day_thr = dn_day_trigger(dn, night_baseline);
            if (cur == DN_DAY) {
                int64_t dnow_ms = ms_now_us() / 1000;
                /* classify a probe's own outcome, exactly once (see the
                 * three-outcome comment at the probe fire). A reading that
                 * confirms day or reverts needs nothing from us - only the
                 * ambiguous dead-zone landing gets a verify deadline, on the
                 * same bounded delay a dead-zone adoption uses. */
                if (probe_verdict_at_ms && dnow_ms >= probe_verdict_at_ms) {
                    probe_verdict_at_ms = 0;
                    if (tg >= dn->total_gain_day_threshold &&
                        tg <= dn->total_gain_night_threshold) {
                        int64_t verify_s = dn_adopt_verify_s(dn);
                        dn_verify_arm(&verify, DN_DAY,
                                      dnow_ms + verify_s * 1000);
                        LOGI(MOD, "probe landed on an ambiguous day-pipeline "
                                  "gain %.0f (dead-zone %.0f..%.0f): neither "
                                  "confirms day nor reverts - gain must "
                                  "confirm it within %llds",
                             (double)tg,
                             (double)dn->total_gain_day_threshold,
                             (double)dn->total_gain_night_threshold,
                             (long long)verify_s);
                    }
                }
                if (tg > dn->total_gain_night_threshold) target = DN_NIGHT;
                else if (dn_verify_due(&verify, DN_DAY, dnow_ms)) {
                    /* an ADOPTED day had to prove itself by now (see
                     * dn_verify). "Proved" = this day-pipeline reading would
                     * have decided day on its own from DN_UNKNOWN, i.e. it
                     * left the dead-zone that forced the guess in the first
                     * place. Note night_baseline is always -1 while in day
                     * (cleared on every switch and probe), so day_thr equals
                     * the fixed threshold here - spelled out for clarity. */
                    if (tg < dn->total_gain_day_threshold) {
                        dn_verify_clear(&verify);
                        LOGI(MOD, "day confirmed by day-pipeline gain "
                                  "%.0f (< %.0f)", (double)tg,
                             (double)dn->total_gain_day_threshold);
                    } else {
                        /* still ambiguous after the bounded wait: the guess is
                         * unverified, so fall to the recoverable side. The
                         * deadline stays armed so the normal hysteresis can
                         * accumulate (a reading that dips below the day
                         * threshold meanwhile drops the target and confirms
                         * the guess instead); the switch clears it. */
                        day_unverified = 1;
                        target = DN_NIGHT;
                        LOGD(MOD, "day never confirmed: gain %.0f still "
                                  "inside the dead-zone", (double)tg);
                    }
                }
            } else if (cur == DN_NIGHT) {
                /* Adaptive mode (0 < day_gain_pct < 100): night->day is
                 * PROBE-MEDIATED ONLY - no direct switch off night-pipeline
                 * gain, before OR after the baseline plants. Night-pipeline
                 * gain under the day trigger is structurally ambiguous: with
                 * the IR-cut open the sensor sees visible+NIR, so a light
                 * level that reads "day" through the night pipeline can read
                 * solidly "night" through the closed-IR-cut day pipeline.
                 * Live proof (cam-wyze-pan, T20/jxf22 basement, dawn
                 * 2026-08-12): resting night gain a stable 10856 for hours,
                 * dawn dips it to ~820 (< trigger 6514) -> the old direct
                 * switch fired after just the 5s hysteresis, preempting the
                 * sustained-brightening hold 4s into its 60s confirm; the
                 * day pipeline then read >= 8192 (dark), reverted - and
                 * because BOTH flips were genuine (probe_day_ms unset) the
                 * revert took the genuine branch below, RESETTING the
                 * failure ratchet/backoff instead of latching them. The
                 * identical pair repeated on every dawn fluctuation (5
                 * audible IR-cut pairs in one morning), spaced too far
                 * apart (>60s) for the oscillation breaker. Routing the
                 * crossing through the brightening probe fixes both halves:
                 * the hold lets smooth_tg converge to the dip floor, so
                 * a failed probe latches probe_fail_smooth at that floor
                 * and the ratchet (< day_gain_pct% of it) makes the same
                 * dip unrepeatable - at most ONE probe pair per dawn, zero
                 * if a dip is shorter than DN_BRIGHTEN_CONFIRM_MS; a
                 * genuine lights-on/dawn probe simply sticks in day. Cost:
                 * adaptive night->day latency is now that confirm instead
                 * of 5s - the same
                 * price the pre-baseline window (a5dae07) and the marginal-
                 * band brightening already pay, and the day pipeline is the
                 * only trustworthy judge here anyway.
                 * day_gain_pct=0 keeps the plain legacy threshold; the
                 * config cap is 100, where the brightening probe is gated
                 * off (see the pct<100 arm above), so that edge keeps the
                 * old post-baseline direct switch rather than losing its
                 * night->day path entirely. */
                if (tg < day_thr &&
                    (dn->day_gain_pct <= 0 ||
                     (dn->day_gain_pct >= 100 && night_baseline > 0.0f)))
                    target = DN_DAY;
            } else {
                if      (tg > dn->total_gain_night_threshold) target = DN_NIGHT;
                else if (tg < dn->total_gain_day_threshold)   target = DN_DAY;
            }
            if (day_unverified)
                snprintf(why, sizeof why, "unverified day, gain %.0f",
                         (double)tg);
            else
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
                int64_t dnow_ms = ms_now_us() / 1000;
                /* same one-shot probe-outcome classification as the gain
                 * path, against this branch's own day/night criteria. */
                if (probe_verdict_at_ms && dnow_ms >= probe_verdict_at_ms) {
                    probe_verdict_at_ms = 0;
                    if (avg <= dn->threshold_high - hyst_range &&
                        avg >= dn->threshold_low) {
                        int64_t verify_s = dn_adopt_verify_s(dn);
                        dn_verify_arm(&verify, DN_DAY,
                                      dnow_ms + verify_s * 1000);
                        LOGI(MOD, "probe landed on an ambiguous brightness "
                                  "%.1f%%: neither confirms day nor reverts - "
                                  "must confirm within %llds",
                             (double)avg, (long long)verify_s);
                    }
                }
                if (avg < dn->threshold_low)  target = DN_NIGHT;
                else if (dn_verify_due(&verify, DN_DAY, dnow_ms)) {
                    /* same adopted-day verification as the gain path, against
                     * the criterion this branch would have used to decide day
                     * unaided from DN_UNKNOWN (the narrowed band below). */
                    if (avg > dn->threshold_high - hyst_range) {
                        dn_verify_clear(&verify);
                        LOGI(MOD, "day confirmed by brightness %.1f%% "
                                  "(> %.1f%%)", (double)avg,
                             (double)(dn->threshold_high - hyst_range));
                    } else {
                        day_unverified = 1;
                        target = DN_NIGHT;
                        LOGD(MOD, "day never confirmed: brightness %.1f%% "
                                  "still inside the dead-zone", (double)avg);
                    }
                }
            } else if (cur == DN_NIGHT) {
                if (avg > dn->threshold_high) target = DN_DAY;
            } else {
                if      (avg < dn->threshold_low  + hyst_range) target = DN_NIGHT;
                else if (avg > dn->threshold_high - hyst_range) target = DN_DAY;
            }
            if (day_unverified)
                snprintf(why, sizeof why,
                         "unverified day, brightness %.1f%%", (double)avg);
            else
                snprintf(why, sizeof why, "avg brightness %.1f%%", (double)avg);
        }

        /* dead-zone adoption (see DN_ADOPT_PROBE_S): still undecided after
         * the boot settle window -> adopt the persisted mode so the in-mode
         * triggers and self-healing probes arm instead of idling forever. */
        if (cur == DN_UNKNOWN && target == DN_UNKNOWN &&
            dn->mode == DN_MODE_SENSOR) {
            int64_t now_ms = ms_now_us() / 1000;
            int settled = now_ms >= settle_floor_ms &&
                (dn->boot_stable_pct <= 0 || now_ms >= settle_hard_ms ||
                 dn_ae_stable(settle_hist, settle_n, dn->boot_stable_pct));
            if (settled) {
                cur = running_mode ? DN_NIGHT : DN_DAY;
                night_entered_ms = (cur == DN_NIGHT) ? now_ms : 0;
                /* SYMMETRIC verification (see dn_verify): whichever mode the
                 * persisted value hands us is a guess about a scene we could
                 * not read, and the persisted value carries no indication of
                 * how long ago it was true - a camera powered off in daylight
                 * and booted after dark adopts DAY from a stale write. Arm the
                 * same bounded deadline either way; only the evidence it is
                 * later settled with differs (night: a physical day-pipeline
                 * probe; day: a plain re-read of the already-trustworthy
                 * day-pipeline gain, no IR-cut click). */
                int64_t verify_s = dn_adopt_verify_s(dn);
                dn_verify_arm(&verify, cur, now_ms + verify_s * 1000);
                LOGI(MOD, "reading %s inside the day/night dead-zone after "
                          "settle - adopting persisted mode %s (%s in %llds)",
                     why, cur == DN_NIGHT ? "night" : "day",
                     cur == DN_NIGHT ? "day-pipeline verify probe"
                                     : "gain must confirm it",
                     (long long)verify_s);
            }
        }

        if (target != cur && target != DN_UNKNOWN) {
            /* CLOCK_MONOTONIC, not time(NULL): an NTP step after boot (typical on
             * cameras) would make wall-clock deltas negative (switching stuck for
             * the step size) or jump straight past the timers (M12). */
            int64_t now_ms = ms_now_us() / 1000;

            /* pre-switch hysteresis: track how long THIS candidate target has
             * held continuously; a change of candidate restarts the clock. */
            if (target != pending_target) {
                pending_target  = target;
                pending_since_ms = now_ms;
            }

            /* three guards, ANDed, each solving a distinct problem:
             *  - cold-start settle: ignore the AE transient right after thread
             *    start/re-enable, and do NOT let it seed the hysteresis candidate;
             *  - dwell: minimum time between switches (transition_s);
             *  - hysteresis: the candidate must have held DN_HYSTERESIS_MS so the
             *    Set is not issued mid AE ramp.
             * The settle guard is floor-only outside sensor mode (or when
             * boot_stable_pct disables the stability wait); in sensor mode it
             * also holds past the floor, up to settle_hard_ms, until
             * dn_ae_stable() sees the gain stop moving (see the
             * DN_SETTLE_SAMPLES comment above dn_ae_stable()). */
            int settle_active = (now_ms < settle_floor_ms) ||
                (dn->mode == DN_MODE_SENSOR && dn->boot_stable_pct > 0 &&
                 now_ms < settle_hard_ms &&
                 !dn_ae_stable(settle_hist, settle_n, dn->boot_stable_pct));
            if (osc_freeze_until_ms && now_ms < osc_freeze_until_ms) {
                /* oscillation breaker cooldown (see DN_OSC_WINDOW_MS): hold the
                 * last-decided mode, ignore threshold crossings until it lifts
                 * so a feedback loop cannot keep clunking the IR-cut. */
                LOGD(MOD, "oscillation freeze: holding %s, ignoring %s "
                          "(%llds left)", cur == DN_NIGHT ? "night" : "day",
                     why, (long long)((osc_freeze_until_ms - now_ms) / 1000));
                pending_target  = DN_UNKNOWN;
                pending_since_ms = 0;
            } else if (cur == DN_UNKNOWN && settle_active) {
                LOGD(MOD, "AE settling, ignoring transient reading");
                pending_target  = DN_UNKNOWN;   /* don't confirm from a transient */
                pending_since_ms = 0;
            } else if (cur == DN_DAY && target == DN_NIGHT && probe_day_ms &&
                       now_ms - probe_day_ms < (int64_t)DN_PROBE_SETTLE_MS &&
                       dn->mode == DN_MODE_SENSOR &&
                       !dn_ae_stable(settle_hist, settle_n,
                                     dn->boot_stable_pct > 0 ? dn->boot_stable_pct : 20)) {
                /* right after a probe switched to day, the first readings are
                 * the AE converging on the new pipeline. A genuinely dark
                 * room rails at max gain - STABLE at once, so its (correct)
                 * revert proceeds - but a lit room's convergence ramp must
                 * not seed the night candidate and kill a legitimate probe
                 * (see the DN_BASELINE_ALPHA regression comment). */
                LOGD(MOD, "post-probe AE settling, ignoring transient reading");
                pending_target  = DN_UNKNOWN;
                pending_since_ms = 0;
            } else if (cur != DN_UNKNOWN &&
                now_ms - last_switch_ms < (int64_t)dn->transition_s * 1000) {
                LOGD(MOD, "transition delay not met, waiting");
            } else if (now_ms - pending_since_ms < (int64_t)DN_HYSTERESIS_MS) {
                LOGD(MOD, "%s candidate, confirming (%lld/%d ms)", why,
                     (long long)(now_ms - pending_since_ms), DN_HYSTERESIS_MS);
            } else {
                dn_switch(target, why, dn->switch_cmd);
                cur = target;
                last_switch_ms  = now_ms;
                pending_target  = DN_UNKNOWN;   /* candidate consumed */
                pending_since_ms = 0;
                /* arm the small post-switch re-assert safety net (defense-in-
                 * depth, see DN_REASSERT_MS/COUNT). */
                reassert_left  = DN_REASSERT_COUNT;
                reassert_at_ms = now_ms + DN_REASSERT_MS;
                /* probe economy accounting (see DN_PROBE_FAIL_WINDOW_MS),
                 * BEFORE the state resets below - it reads probe_day_ms and
                 * the pre-probe smoothed gain. A revert to night shortly
                 * after a probe = the probe FAILED (found genuine darkness):
                 * double the periodic interval and latch the failure ratchet
                 * for the brightening probe. Every other switch is a genuine
                 * transition: reset both. */
                /* An unconfirmed probe-day counts as a FAILED probe too, even
                 * though its revert lands long after the fail window: the
                 * probe was fired to find day and did not, so its cost must
                 * ratchet exactly like a fast revert. Accounting it as a
                 * genuine transition instead would reset probe_fail_smooth
                 * and the backoff, letting the identical marginal scene
                 * re-fire the same probe every few minutes - a slow version
                 * of the very flap loop the ratchet exists to stop, and one
                 * spaced too far apart for the oscillation breaker to catch.
                 * smooth_tg still holds the pre-probe night level here (it is
                 * only updated while cur==DN_NIGHT), so the ratchet latches at
                 * the same value either path. */
                if (target == DN_NIGHT && probe_day_ms &&
                    (now_ms - probe_day_ms < (int64_t)DN_PROBE_FAIL_WINDOW_MS ||
                     day_unverified)) {
                    probe_fail_smooth = smooth_tg;   /* pre-probe night level */
                    if (probe_backoff < DN_PROBE_BACKOFF_MAX)
                        probe_backoff *= 2;
                    LOGI(MOD, "probe %s (backoff x%d, brighten ratchet < %.0f)",
                         day_unverified ? "never confirmed day - back to night"
                                        : "confirmed genuine night",
                         probe_backoff, (double)(probe_fail_smooth > 0.0f ?
                             probe_fail_smooth * (float)dn->day_gain_pct / 100.0f
                             : -1.0f));
                } else {
                    probe_backoff = 1;
                    probe_fail_smooth = -1.0f;
                    /* oscillation breaker (see DN_OSC_WINDOW_MS): this is a
                     * GENUINE (non-probe) threshold-crossing flip - the kind an
                     * IR-reflection feedback loop produces. Probe fire/revert
                     * flips take the if-branch above and are deliberately never
                     * counted, so a normal reconfirm/brightening probe cycle
                     * cannot trip this. Record it; if DN_OSC_FLIPS genuine flips
                     * fall within DN_OSC_WINDOW_MS, freeze the last-decided mode
                     * for DN_OSC_FREEZE_MS. */
                    osc_hist[osc_n % DN_OSC_FLIPS] = now_ms;
                    osc_n++;
                    if (osc_n >= DN_OSC_FLIPS) {
                        int64_t oldest = osc_hist[0];
                        for (int i = 1; i < DN_OSC_FLIPS; i++)
                            if (osc_hist[i] < oldest) oldest = osc_hist[i];
                        if (now_ms - oldest <= (int64_t)DN_OSC_WINDOW_MS) {
                            osc_freeze_until_ms = now_ms + DN_OSC_FREEZE_MS;
                            osc_n = 0;   /* start counting fresh after cooldown */
                            LOGW(MOD, "possible IR-reflection feedback loop "
                                      "detected (%d day/night flips in %llds) - "
                                      "camera may be mounted too close to a "
                                      "reflective object; freezing in %s mode "
                                      "for %ds", DN_OSC_FLIPS,
                                 (long long)((now_ms - oldest) / 1000),
                                 target == DN_NIGHT ? "night" : "day",
                                 DN_OSC_FREEZE_MS / 1000);
                        }
                    }
                }
                /* (re)arm the adaptive baseline: sample it a while after we
                 * enter night, once the IR LEDs have settled; clear on day.
                 * The smoothed gain restarts with the night session so a
                 * failed probe's railed day readings never leak in. */
                night_baseline = -1.0f;
                brighten_since_ms = 0;
                brighten_armed = 0;
                smooth_tg = -1.0f;
                probe_day_ms = 0;
                probe_verdict_at_ms = 0;  /* a new switch supersedes it */
                night_entered_ms = (target == DN_NIGHT) ? now_ms : 0;
                /* (re)arm the periodic reconfirm probe alongside it (see
                 * night_reconfirm_s doc comment in config.h), stretched by
                 * the failure backoff and bounded by
                 * max(night_reconfirm_s, DN_PROBE_BACKOFF_CAP_S). */
                if (target == DN_NIGHT && dn->night_reconfirm_s > 0) {
                    int64_t iv  = (int64_t)dn->night_reconfirm_s * probe_backoff;
                    int64_t cap = dn->night_reconfirm_s > DN_PROBE_BACKOFF_CAP_S
                                ? (int64_t)dn->night_reconfirm_s
                                : (int64_t)DN_PROBE_BACKOFF_CAP_S;
                    if (iv > cap) iv = cap;
                    dn_verify_arm(&verify, DN_NIGHT, now_ms + iv * 1000);
                } else
                    /* a real switch is a measurement, not a guess: it also
                     * retires any pending adopted-mode verification. */
                    dn_verify_clear(&verify);
            }
        } else {
            /* reading is back in (or never left) the current regime: abandon any
             * half-confirmed candidate so a brief excursion never accumulates. */
            pending_target  = DN_UNKNOWN;
            pending_since_ms = 0;
        }

        /* sample the night gain baseline once, after the IR LEDs settle.
         * Taken from the SMOOTHED gain when available so a post-revert AE
         * still settling down cannot plant an inflated baseline.
         *
         * ALSO gated on AE stability (same dn_ae_stable ring the boot settle
         * uses), bounded by baseline_delay_s + boot_settle_max_s: a fixed
         * 30 s delay alone plants the baseline MID-DESCENT on cameras whose
         * AE takes much longer to converge after IR-on. Observed on
         * cam-wyze-pan (T20/jxf22, dark room, strong IR return) 2026-08-11:
         * night entry at railed gain, AE still collapsing toward its true
         * IR-lit resting level (~800) when the 30 s sample planted baselines
         * of 10856/5148 - the derived day trigger (60% = 6514/3090) then sat
         * far ABOVE the resting gain, so finishing the descent read as
         * "brightening", fired a full adaptive night->day switch, the day
         * pipeline found genuine darkness, flipped back, re-planted another
         * mid-descent baseline 30 s later, and the pair repeated until the
         * oscillation breaker froze the camera - every ~25 min, forever.
         * The symmetric drift below cannot save this: its ~4 min time
         * constant loses the race to a descent that crosses the trigger
         * seconds after the plant. Waiting for a stable reading plants the
         * baseline at the settled level instead, the trigger lands BELOW it,
         * and the loop terminates after at most one flip-pair.
         * boot_stable_pct=0 (stability checks disabled) or the
         * boot_settle_max_s cap preserve the old fixed-delay behaviour. */
        if (cur == DN_NIGHT && night_baseline < 0.0f && tg >= 0.0f &&
            dn->baseline_delay_s > 0 && night_entered_ms > 0) {
            int64_t since_ms = ms_now_us() / 1000 - night_entered_ms; /* monotonic (M12) */
            if (since_ms >= (int64_t)dn->baseline_delay_s * 1000 &&
                (dn->boot_stable_pct <= 0 ||
                 dn_ae_stable(settle_hist, settle_n, dn->boot_stable_pct) ||
                 since_ms >= ((int64_t)dn->baseline_delay_s +
                              (int64_t)dn->boot_settle_max_s) * 1000)) {
                night_baseline = (smooth_tg > 0.0f) ? smooth_tg : tg;
                baseline_logged = night_baseline;
                LOGI(MOD, "night gain baseline = %.0f (day trigger < %d%% = %.0f, "
                          "sampled %llds after night entry)",
                     (double)night_baseline, dn->day_gain_pct,
                     (double)dn_day_trigger(dn, night_baseline),
                     (long long)(since_ms / 1000));
            }
        } else if (cur == DN_NIGHT && night_baseline > 0.0f && smooth_tg > 0.0f) {
            /* slow SYMMETRIC drift toward the smoothed gain: an unrepresent-
             * ative baseline (sampled mid lighting-transition, or off a
             * post-revert AE still settling) self-corrects in a few minutes
             * in EITHER direction, and AGC noise centers instead of
             * ratcheting (the v1.7.3 upward-only drift chased noise peaks -
             * see the DN_BASELINE_ALPHA regression comment). The drift is
             * much slower than DN_BRIGHTEN_CONFIRM_MS, so a genuine sudden
             * brightening still opens a wide baseline-vs-gain gap before
             * the baseline can follow it down. Log at INFO on a meaningful
             * (>=25%) move either way. */
            night_baseline += (smooth_tg - night_baseline) * DN_BASELINE_ALPHA;
            if (baseline_logged > 0.0f &&
                (night_baseline >= baseline_logged * 1.25f ||
                 night_baseline <= baseline_logged * 0.75f)) {
                baseline_logged = night_baseline;
                LOGI(MOD, "night gain baseline drifted to %.0f "
                          "(day trigger < %d%% = %.0f)",
                     (double)night_baseline, dn->day_gain_pct,
                     (double)dn_day_trigger(dn, night_baseline));
            }
        }

        dn_status_update(dn, b, tg, luma, cur, night_baseline);
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
                         float *brightness, float *total_gain, float *ae_luma,
                         float *night_baseline, float *day_trigger)
{
    pthread_mutex_lock(&g_st_mu);
    float b  = g_st_brightness;
    float tg = g_st_gain;
    float lu = g_st_luma;
    int   m  = g_st_mode;
    float nb = g_st_baseline;
    float dt = g_st_daytrig;
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
    if (enabled)        *enabled        = dn_enabled ? 1 : 0;
    if (mode)           *mode           = m;
    if (brightness)     *brightness     = b;
    if (total_gain)     *total_gain     = tg;
    if (ae_luma)        *ae_luma        = lu;
    if (night_baseline) *night_baseline = nb;
    if (day_trigger)    *day_trigger    = dt;
}

/* see daynight.h: today's computed sunrise/sunset for the configured
 * lat/long+offsets, as local "HH:MM" strings (for the WebUI SUN readout). */
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
        if (sr_hhmm && cap) snprintf(sr_hhmm, cap, "%s", r > 0 ? "--:--" : "--:--");
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
 * measurement -> brightness/gain unknown, mode from the persisted/live ISP
 * running mode */
void daynight_get_status(int *enabled, int *mode,
                         float *brightness, float *total_gain, float *ae_luma,
                         float *night_baseline, float *day_trigger)
{
    if (enabled)        *enabled        = 0;
    if (mode){          /* F-03: live-mutable, read under the config string lock */
        config_str_lock();
        int rm = g_cfg.image.running_mode;
        config_str_unlock();
        *mode = rm ? 1 : 0;
    }
    if (brightness)     *brightness     = -1.0f;
    if (total_gain)     *total_gain     = -1.0f;
    if (ae_luma)        *ae_luma        = -1.0f;
    if (night_baseline) *night_baseline = -1.0f;
    if (day_trigger)    *day_trigger    = -1.0f;
}

int daynight_sun_status(char *sr_hhmm, char *ss_hhmm, size_t cap)
{
    if (sr_hhmm && cap) snprintf(sr_hhmm, cap, "%s", "--:--");
    if (ss_hhmm && cap) snprintf(ss_hhmm, cap, "%s", "--:--");
    return 0;
}

#endif /* USE_DAYNIGHT */
