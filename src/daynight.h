/* daynight.h - native automatic day/night detection (compiled only with
 * -DUSE_DAYNIGHT, otherwise the whole feature vanishes from the build).
 *
 * A small pthread decides day vs night and on a change runs
 * "<daynight.switch_cmd> day|night" (default: thingino's /sbin daynight
 * script, which drives ircut / IR LEDs / color -> ISP running_mode) - timps
 * does NOT touch image.running_mode itself, the color hook does. A minimum
 * dwell (daynight.transition_s) guards against flapping at a boundary.
 *
 * The decision source is selectable via daynight.mode:
 *   sensor (default) - samples the Ingenic ISP exposure state
 *       (daynight.isp_path, default /proc/jz/isp/isp-m0) every
 *       daynight.interval_ms and decides from ISP total_gain (prudynt/raptor
 *       scale) with a brightness fallback (thingino daynightd formula), using
 *       the gain/brightness thresholds + hysteresis.
 *   time   - forces day/night purely by the local wall clock: a fixed window
 *       [daynight.time_night_start .. daynight.time_day_start] ("HH:MM"),
 *       independent of the scene; the window may wrap past midnight.
 *   sun    - forces day/night by today's real sunrise/sunset computed from
 *       daynight.sun_latitude/sun_longitude (standard sunrise equation, pure
 *       math), each shifted by sun_sunrise/sunset_offset_min minutes; looks
 *       natural across seasons unlike a fixed clock time.
 * The ISP is still sampled in every mode so the WebUI live gain/brightness
 * readout stays populated; only the decision branch changes.
 *
 * daynight.enabled can be flipped at runtime (config or /control): while
 * disabled the thread keeps sampling (so the status below stays live for the
 * WebUI) but forces nothing (manual mode) in ALL modes; re-enabling restarts
 * detection from a clean state. Missing/unreadable ISP file (host sim,
 * non-Ingenic) just skips the sensor cycle (time/sun need no ISP). */
#ifndef MS_DAYNIGHT_H
#define MS_DAYNIGHT_H

#include <stddef.h>   /* size_t (daynight_sun_status) */

#ifdef USE_DAYNIGHT
void daynight_start(void);
void daynight_stop(void);
#endif

/* Latest day/night measurement for GET /control (always linkable: without
 * USE_DAYNIGHT a stub answers "unknown"). Values:
 *   enabled     0/1  auto detection on (always 0 without USE_DAYNIGHT)
 *   mode        0 day / 1 night: the mode last switched by the detection
 *               thread, falling back to image.running_mode (manual mode,
 *               before the first switch, or without USE_DAYNIGHT)
 *   brightness  scene brightness 0..100 %, or -1 when the ISP proc file is
 *               unreadable / no sample was taken yet
 *   total_gain  total sensor+ISP gain in the IMP [24.8] linear format
 *               (256 = 1x, like IMP_ISP_Tuning_GetTotalGain and the
 *               prudynt/raptor "total_gain" the WebUI plots), derived from
 *               the isp-m0 gain fields (log2 units, 32 = 2x); -1 = unknown
 *   ae_luma     ISP AE average luminance (raptor's ae_luma), a secondary
 *               photosensing metric; -1 when the SoC/build has no GetAeLuma
 * NULL pointers are allowed for outputs the caller does not need. */
void daynight_get_status(int *enabled, int *mode,
                         float *brightness, float *total_gain, float *ae_luma);

/* Today's computed sunrise/sunset for the configured daynight.sun_* location
 * and offsets, formatted as local "HH:MM" into sr_hhmm/ss_hhmm (either may be
 * NULL). Read-only feedback so the WebUI can sanity-check lat/long before
 * trusting the SUN mode. Returns 1 on a normal day, 0 for polar day/night or
 * without USE_DAYNIGHT (strings then read "--:--"). */
int daynight_sun_status(char *sr_hhmm, char *ss_hhmm, size_t cap);

#endif
