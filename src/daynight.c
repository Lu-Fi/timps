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
#include "hub.h"       /* hub_control(): re-assert running_mode into the ISP */
#include "events.h"   /* wake /events SSE subscribers on real changes */
#include "log.h"
#include "util.h"     /* ms_now_us(): monotonic clock for dwell/baseline (M12) */
#include <stdio.h>
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
static volatile int g_stop;
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
 * DN_BRIGHTEN_CONFIRM_MS). Without a baseline the fixed threshold applies. */
static float dn_day_trigger(float baseline)
{
    const ms_daynight_cfg *dn = &g_cfg.daynight;
    float thr = dn->total_gain_day_threshold;
    if (baseline > 0.0f && dn->day_gain_pct > 0) {
        thr = baseline * (float)dn->day_gain_pct / 100.0f;
        if (thr < dn->total_gain_day_threshold)
            thr = dn->total_gain_day_threshold;
    }
    return thr;
}

static void dn_status_update(float brightness, float total_gain, float ae_luma,
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
    g_st_daytrig    = (mode == DN_NIGHT) ? dn_day_trigger(baseline) : -1.0f;
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
static void dn_switch(int mode, const char *why)
{
    const char *arg = (mode == DN_NIGHT) ? "night" : "day";
    const char *cmd = g_cfg.daynight.switch_cmd;
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

/* sleep interval_ms in small slices so daynight_stop() joins promptly */
static void dn_sleep(int ms)
{
    while (ms > 0 && !g_stop) {
        int step = ms > 200 ? 200 : ms;
        usleep((useconds_t)step * 1000);
        ms -= step;
    }
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
#ifndef DN_BRIGHTEN_CONFIRM_MS
#define DN_BRIGHTEN_CONFIRM_MS 60000  /* sustained-brightening probe confirm */
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
 * normal in-mode triggers and probes arm. An adopted NIGHT is a guess, not a
 * measurement, so its first day-pipeline reconfirm probe fires early - after
 * min(night_reconfirm_s, DN_ADOPT_PROBE_S) - and once even when the periodic
 * reconfirm is disabled: a guess must be verified. */
#ifndef DN_ADOPT_PROBE_S
#define DN_ADOPT_PROBE_S 300     /* verify an adopted night within 5 minutes */
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
     * dn_ae_stable() and the DN_SETTLE_SAMPLES comment above. */
    int64_t settle_floor_ms = ms_now_us() / 1000 +
        (int64_t)g_cfg.daynight.boot_settle_s * 1000;
    int64_t settle_hard_ms  = ms_now_us() / 1000 +
        (int64_t)g_cfg.daynight.boot_settle_max_s * 1000;
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
    /* self-healing periodic reconfirm (see night_reconfirm_s doc comment in
     * config.h): when we're in night and this fires, we probe the day
     * pipeline for real instead of trusting night-path gain. 0 = none pending. */
    int64_t night_probe_at_ms = 0;
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
    /* baseline value last reported in a log line (rate-limits the drift
     * INFO log to meaningful moves, see the EMA drift below). */
    float   baseline_logged = -1.0f;
    /* P2: last /proc-scraped brightness (status readout only when the gain
     * API works) and when the next scrape is due. 0 = scrape on first tick. */
    float   scraped_b = -1.0f;
    int64_t next_scrape_ms = 0;

    for (int i = 0; i < DN_SAMPLES; i++) hist[i] = 50.0f;  /* neutral start */

    { int m = g_cfg.daynight.mode;
      const char *ms = m==DN_MODE_TIME ? "time" : m==DN_MODE_SUN ? "sun" : "sensor";
      /* the /control server is already up here: snapshot the runtime-mutable
       * time window strings under the config string lock (see config.c) */
      char tns[sizeof g_cfg.daynight.time_night_start];
      char tds[sizeof g_cfg.daynight.time_day_start];
      config_str_lock();
      snprintf(tns, sizeof tns, "%s", g_cfg.daynight.time_night_start);
      snprintf(tds, sizeof tds, "%s", g_cfg.daynight.time_day_start);
      config_str_unlock();
      LOGI(MOD, "detection thread started (mode=%s, time night=%s day=%s, "
                "sun lat=%g lon=%g off rise=%d set=%d min)",
           ms, tns[0]?tns:"-",
           tds[0]?tds:"-",
           (double)g_cfg.daynight.sun_latitude, (double)g_cfg.daynight.sun_longitude,
           g_cfg.daynight.sun_sunrise_offset_min, g_cfg.daynight.sun_sunset_offset_min); }
    LOGI(MOD, "detection thread started (gain day<%g night>%g, "
              "brightness fallback %.1f/%.1f hyst %.2f, "
              "interval %dms, dwell %ds, isp=%s, cmd=%s, "
              "boot settle %ds/%ds stable<%d%%, night reconfirm %ds)",
         (double)g_cfg.daynight.total_gain_day_threshold,
         (double)g_cfg.daynight.total_gain_night_threshold,
         g_cfg.daynight.threshold_low, g_cfg.daynight.threshold_high,
         g_cfg.daynight.hysteresis, g_cfg.daynight.interval_ms,
         g_cfg.daynight.transition_s, g_cfg.daynight.isp_path,
         g_cfg.daynight.switch_cmd,
         g_cfg.daynight.boot_settle_s, g_cfg.daynight.boot_settle_max_s,
         g_cfg.daynight.boot_stable_pct, g_cfg.daynight.night_reconfirm_s);

    while (!g_stop) {
        const ms_daynight_cfg *dn = &g_cfg.daynight;
        int interval = dn->interval_ms > 0 ? dn->interval_ms : 500;

        /* fire a pending post-switch running-mode re-assert once it comes due,
         * then re-arm the next one until the series is exhausted (see
         * DN_REASSERT_MS / DN_REASSERT_COUNT). Dropped if auto was turned off in
         * the meantime - the user may have taken manual control of the mode. */
        if (reassert_at_ms && ms_now_us() / 1000 >= reassert_at_ms) {
            if (dn->enabled) {
                /* apply the CURRENT desired mode: ing_control applies image.*
                 * from g_cfg, so read it here too (N3) - value, apply and log all
                 * agree, and a manual override during the window is honoured. */
                int rm = g_cfg.image.running_mode ? 1 : 0;
                hub_control("image.running_mode", rm ? "1" : "0");
                LOGI(MOD, "re-asserting running_mode=%d after switch (%d left)",
                     rm, reassert_left - 1);
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
            smooth_tg = -1.0f; probe_day_ms = 0;
            pending_target = DN_UNKNOWN; pending_since_ms = 0;
            dn_status_update(b, tg, luma, DN_UNKNOWN, night_baseline);
            dn_sleep(interval);
            continue;
        }
        if (!was_enabled) {             /* (re)enabled: detect from scratch */
            was_enabled = 1;
            cur = DN_UNKNOWN;           /* mode may have been set manually */
            night_baseline = -1.0f;
            night_entered_ms = 0;
            night_probe_at_ms = 0;
            brighten_since_ms = 0; brighten_armed = 0;
            smooth_tg = -1.0f; probe_day_ms = 0;
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
                LOGD(MOD, "%s not readable, detection idle", dn->isp_path);
                warned_noisp = 1;
            }
            dn_status_update(b, tg, luma, cur, night_baseline);
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
            /* night_probe_at_ms is armed by night entry (when reconfirm is
             * enabled) or by dead-zone adoption (one-shot, even when it is
             * not - see DN_ADOPT_PROBE_S); honour it whenever set. */
            if (night_probe_at_ms > 0 && now_ms >= night_probe_at_ms)
                probe_why = "periodic reconfirm probe";
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
                        brighten_since_ms = now_ms;
                        LOGI(MOD, "sustained brightening: gain %.0f below probe "
                                  "bar %.0f (baseline %.0f), confirming %ds",
                             (double)smooth_tg, (double)probe_bar,
                             (double)night_baseline,
                             DN_BRIGHTEN_CONFIRM_MS / 1000);
                    } else if (now_ms - brighten_since_ms >=
                                   (int64_t)DN_BRIGHTEN_CONFIRM_MS &&
                               now_ms - last_switch_ms >=
                                   (int64_t)dn->transition_s * 1000)
                        probe_why = "sustained brightening probe";
                }
            }
            if (probe_why) {
                LOGI(MOD, "night reconfirm (%s): probing day pipeline after "
                          "%llds dwell (gain %.0f, baseline %.0f)", probe_why,
                     (long long)((now_ms - night_entered_ms) / 1000),
                     (double)tg, (double)night_baseline);
                dn_switch(DN_DAY, probe_why);
                cur = DN_DAY;
                last_switch_ms  = now_ms;
                pending_target  = DN_UNKNOWN; pending_since_ms = 0;
                reassert_left   = DN_REASSERT_COUNT;
                reassert_at_ms  = now_ms + DN_REASSERT_MS;
                night_baseline  = -1.0f;
                night_entered_ms = 0;
                night_probe_at_ms = 0;
                brighten_since_ms = 0;
                brighten_armed  = 0;    /* re-arms above the bar next night */
                probe_day_ms    = now_ms; /* gate the revert on stability */
                dn_status_update(b, tg, luma, cur, night_baseline);
                dn_sleep(interval);
                continue;
            }
        }

        int  target = cur;
        char why[64];
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
             * time_night_start/time_day_start are runtime-mutable via /control:
             * snapshot them under the config string lock (see config.c
             * g_str_lock comment), or a read racing copystr() can see a
             * non-terminated buffer and sscanf() past the 6-byte field. */
            time_t now = time(NULL); struct tm lt; localtime_r(&now, &lt);
            char tns[sizeof dn->time_night_start], tds[sizeof dn->time_day_start];
            config_str_lock();
            snprintf(tns, sizeof tns, "%s", dn->time_night_start);
            snprintf(tds, sizeof tds, "%s", dn->time_day_start);
            config_str_unlock();
            target = dn_time_target(tns, tds, &lt);
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
            float day_thr = dn_day_trigger(night_baseline);
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
                cur = g_cfg.image.running_mode ? DN_NIGHT : DN_DAY;
                night_entered_ms = (cur == DN_NIGHT) ? now_ms : 0;
                if (cur == DN_NIGHT) {
                    int64_t probe_s = DN_ADOPT_PROBE_S;
                    if (dn->night_reconfirm_s > 0 &&
                        dn->night_reconfirm_s < probe_s)
                        probe_s = dn->night_reconfirm_s;
                    night_probe_at_ms = now_ms + probe_s * 1000;
                    LOGI(MOD, "reading %s inside the day/night dead-zone after "
                              "settle - adopting persisted mode night "
                              "(day-pipeline verify probe in %llds)",
                         why, (long long)probe_s);
                } else {
                    night_probe_at_ms = 0;
                    LOGI(MOD, "reading %s inside the day/night dead-zone after "
                              "settle - adopting persisted mode day", why);
                }
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
            if (cur == DN_UNKNOWN && settle_active) {
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
                dn_switch(target, why);
                cur = target;
                last_switch_ms  = now_ms;
                pending_target  = DN_UNKNOWN;   /* candidate consumed */
                pending_since_ms = 0;
                /* arm the small post-switch re-assert safety net (defense-in-
                 * depth, see DN_REASSERT_MS/COUNT). */
                reassert_left  = DN_REASSERT_COUNT;
                reassert_at_ms = now_ms + DN_REASSERT_MS;
                /* (re)arm the adaptive baseline: sample it a while after we
                 * enter night, once the IR LEDs have settled; clear on day.
                 * The smoothed gain restarts with the night session so a
                 * failed probe's railed day readings never leak in. */
                night_baseline = -1.0f;
                brighten_since_ms = 0;
                brighten_armed = 0;
                smooth_tg = -1.0f;
                probe_day_ms = 0;
                night_entered_ms = (target == DN_NIGHT) ? now_ms : 0;
                /* (re)arm the periodic reconfirm probe alongside it, see
                 * night_reconfirm_s doc comment in config.h */
                night_probe_at_ms = (target == DN_NIGHT && dn->night_reconfirm_s > 0)
                    ? now_ms + (int64_t)dn->night_reconfirm_s * 1000 : 0;
            }
        } else {
            /* reading is back in (or never left) the current regime: abandon any
             * half-confirmed candidate so a brief excursion never accumulates. */
            pending_target  = DN_UNKNOWN;
            pending_since_ms = 0;
        }

        /* sample the night gain baseline once, after the IR LEDs settle.
         * Taken from the SMOOTHED gain when available so a post-revert AE
         * still settling down cannot plant an inflated baseline. */
        if (cur == DN_NIGHT && night_baseline < 0.0f && tg >= 0.0f &&
            dn->baseline_delay_s > 0 && night_entered_ms > 0 &&
            ms_now_us() / 1000 - night_entered_ms >=      /* monotonic (M12) */
                (int64_t)dn->baseline_delay_s * 1000) {
            night_baseline = (smooth_tg > 0.0f) ? smooth_tg : tg;
            baseline_logged = night_baseline;
            LOGI(MOD, "night gain baseline = %.0f (day trigger < %d%% = %.0f)",
                 (double)night_baseline, dn->day_gain_pct,
                 (double)dn_day_trigger(night_baseline));
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
                     (double)dn_day_trigger(night_baseline));
            }
        }

        dn_status_update(b, tg, luma, cur, night_baseline);
        dn_sleep(interval);
    }
    LOGI(MOD, "detection thread stopped");
    return NULL;
}

void daynight_start(void)
{
    if (g_started) return;
    g_stop = 0;
    if (ms_thread_create(&g_thr, MS_STACK_UTIL, dn_thread, NULL) != 0) {
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
    if (m == DN_UNKNOWN)   /* manual mode / before the first auto switch */
        m = g_cfg.image.running_mode ? DN_NIGHT : DN_DAY;
    if (enabled)        *enabled        = g_cfg.daynight.enabled ? 1 : 0;
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
    const ms_daynight_cfg *dn = &g_cfg.daynight;
    time_t now = time(NULL), sr, ss;
    int r = dn_sun_times(dn->sun_latitude, dn->sun_longitude, now, &sr, &ss);
    if (r != 0) {                       /* polar day/night: no rise/set today */
        if (sr_hhmm && cap) snprintf(sr_hhmm, cap, "%s", r > 0 ? "--:--" : "--:--");
        if (ss_hhmm && cap) snprintf(ss_hhmm, cap, "%s", "--:--");
        return 0;
    }
    sr += (time_t)dn->sun_sunrise_offset_min * 60;
    ss += (time_t)dn->sun_sunset_offset_min  * 60;
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
    if (mode)           *mode           = g_cfg.image.running_mode ? 1 : 0;
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
