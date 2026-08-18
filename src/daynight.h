/* daynight.h - native automatic day/night detection (compiled only with
 * -DUSE_DAYNIGHT, otherwise the whole feature vanishes from the build).
 *
 * A small pthread decides day vs night and on a change runs
 * "<daynight.switch_cmd> day|night" (default: thingino's /sbin daynight
 * script, which drives ircut / IR LEDs / color -> ISP running_mode) - timps
 * does NOT touch image.running_mode itself, the color hook does.
 *
 * THE ONE FACT THE WHOLE DESIGN FOLLOWS FROM: the two optical paths are not
 * equally trustworthy. With the IR-cut closed and the illuminator off (the
 * DAY pipeline) the exposure the ISP settles on is an honest measure of
 * ambient light. With the IR-cut open and the illuminator on (the NIGHT
 * pipeline) the camera is measuring, in part, its own light - so an absolute
 * night reading means nothing, and only a CHANGE in it means anything. That
 * asymmetry produces five independent paths, none of which can block another:
 *
 *   A  day -> night   direct, from the honest day-pipeline reading. Needs no
 *                     history and no probe: D above daynight.night_gain for
 *                     day_confirm_s and the switch is made.
 *   C  night -> day   ONLY via a probe: physically switch to the day pipeline,
 *                     let the AE settle for probe_settle_s, then judge once
 *                     against day_gain and either stay or fall straight back.
 *                     A probe is asked for when the night reading drops below
 *                     probe_jump_pct% of the night reference and holds there -
 *                     i.e. when someone turns a light on. This is the path that
 *                     carries cameras with no usable location data, which is
 *                     most of them.
 *   T  night -> day   the same probe, asked for by a TREND instead of a step:
 *                     a 3-minute EMA of the index below 75% of a 60-minute one
 *                     means the scene is brighter than it remembers being,
 *                     which is what a dawn looks like and what a light switch
 *                     does not. Only armed where daynight.irprobe_cmd makes
 *                     the probe silent - see the DN_TREND_* block in
 *                     daynight.c for the measured reason.
 *   B  heartbeat      a probe every heartbeat_s regardless of any reading, or
 *                     heartbeat_max_s once the scene demonstrably stopped
 *                     moving. Sensor-independent, so it is the only bound on
 *                     how long a wrong night can last - deliberately a flat
 *                     interval, never a multiplying backoff. When a calendar
 *                     is configured it only pulls this in to dawn.
 *   D  boot           the persisted mode is a guess: if it says day we are in
 *                     the honest pipeline already and one reading settles it,
 *                     if it says night a single probe turns the guess into a
 *                     measurement (daynight.boot_probe).
 *
 * The metric is the EXPOSURE INDEX D = total_gain * integration_time /
 * max_integration_time (higher = darker), not bare gain - see the
 * ms_daynight_cfg comment in config.h for why, and
 * dev_notes/DAYNIGHT_REDESIGN_2026-08-17.md for the whole design.
 *
 * daynight.mode selects between that automaton (auto) and letting the
 * configured calendar decide outright with no sensor and no probes at all
 * (schedule). daynight.enabled can be flipped at runtime (config or
 * /control): while disabled the thread keeps sampling (so the status below
 * stays live for the WebUI) but forces nothing (manual mode) in both modes;
 * re-enabling restarts detection from a clean state. A missing/unreadable ISP
 * file (host sim, non-Ingenic) just skips the cycle in auto mode; schedule
 * mode needs no ISP. */
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
 *               unreadable / no sample was taken yet. Status readout only -
 *               it decides nothing.
 *   total_gain  total sensor+ISP gain in the IMP [24.8] linear format
 *               (256 = 1x, like IMP_ISP_Tuning_GetTotalGain and the
 *               prudynt/raptor "total_gain" the WebUI plots); -1 = unknown
 *   exposure    THE DECISION METRIC: total_gain scaled by the AE's
 *               integration-time ratio. Equal to total_gain in a dark scene
 *               (integration railed at max) and far below it in a bright one,
 *               which is the range bare gain does not have; -1 = unknown
 *   ae_luma     ISP AE average luminance (raptor's ae_luma), a secondary
 *               photosensing metric; -1 when the SoC/build has no GetAeLuma
 *   night_ref   the night reference the spontaneous-brightening trigger
 *               measures against - the exposure level at which night was last
 *               PROVEN, either by entering it from day or by a probe that
 *               found darkness. -1 = none anchored (not in night, or the AE
 *               has not settled since entering it)
 *   probe_bar   the level D must fall below to ask for a probe
 *               (probe_jump_pct% of night_ref); -1 when not in night
 * NULL pointers are allowed for outputs the caller does not need. */
void daynight_get_status(int *enabled, int *mode,
                         float *brightness, float *total_gain, float *exposure,
                         float *ae_luma, float *night_ref, float *probe_bar);

/* Today's computed sunrise/sunset for the configured daynight.sun_* location
 * and offsets, formatted as local "HH:MM" into sr_hhmm/ss_hhmm (either may be
 * NULL). Read-only feedback so the WebUI can sanity-check lat/long before
 * trusting it. Returns 1 on a normal day, 0 for polar day/night or without
 * USE_DAYNIGHT (strings then read "--:--"). */
int daynight_sun_status(char *sr_hhmm, char *ss_hhmm, size_t cap);

#endif
