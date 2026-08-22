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

/* ---- fixed fleet-wide constants (2026-08-22 config consolidation) ------
 * These eight used to be per-camera ms_daynight_cfg fields. The config
 * consolidation (dev_notes/DAYNIGHT_DECISION_2026-08-17.md) found that every
 * camera measured wanted the same value - the same situation DN_TREND_PCT
 * (daynight.c) was already hardcoded for, and the same fix: a config key
 * nobody ever needed to change is not a config key, it is a constant that
 * happens to be spelled like one. Defined here rather than in daynight.c
 * because control.c's status JSON and config.c's config-file grace-period
 * warning both need the same numbers - one header, so there is exactly one
 * place the eight values live, never two copies to drift apart.
 *
 * ir_ratio_night/ir_ratio_day: r = D(illuminator off) / D(illuminator on) is
 * dimensionless and needs no per-camera calibration - unlike any absolute
 * exposure level, which spans a factor of 63 across this fleet at one
 * instant. Re-derived 2026-08-19 from a full dusk-to-dawn campaign (twelve
 * cameras, 37-62 probe pairs each): the darkest genuine night with AE
 * headroom measured r=2.38, the dimmest confirmed-lit room r=1.50, and
 * anything in 1.8..2.2 produces the same verdicts across the whole campaign
 * - so both thresholds sit at the same round number, 2.0, deliberately
 * equal (the gap between them is where the ratio is genuinely undecided and
 * falls back to the audible probe).
 *
 * ir_min_headroom: minimum AE reserve, in log2 units (32 = one stop), for
 * the ratio above to mean anything. An AE with nothing left cannot respond
 * to the illuminator going off and returns r ~= 1 - indistinguishable from
 * daylight. Measured on a pitch-dark outbuilding: r = 1.14 with 1 unit of
 * reserve, which an 8-unit floor correctly calls a clip, not a level.
 *
 * ref_delay_s/boot_settle_s/transition_s: settle-time floors (IR LEDs and AE
 * convergence after a switch or boot) - every camera's AE settles on the
 * same order of seconds, so these were never observed to need per-camera
 * tuning either.
 *
 * probe_jump_pct/probe_settle_s: the probe economy's own trigger bar and AE
 * settle time - see the DN_TREND_PCT precedent comment in daynight.c for why
 * a swept, fleet-wide constant belongs here instead of in config. */
#define DN_PROBE_JUMP_PCT   50     /* probe when D falls below this % of the night reference */
#define DN_PROBE_SETTLE_S    8     /* AE settle before a probe verdict */
#define DN_REF_DELAY_S       30    /* wait after entering night before anchoring the reference */
#define DN_IR_RATIO_NIGHT  2.0f    /* r at or above this = night, no click */
#define DN_IR_RATIO_DAY    2.0f    /* r at or below this = day (if AE had room) */
#define DN_IR_MIN_HEADROOM   8     /* min AE reserve, log2 units, for the ratio to mean anything */
#define DN_BOOT_SETTLE_S     5     /* min wait before the first boot decision */
#define DN_TRANSITION_S      5     /* min dwell between mode switches */

#ifdef USE_DAYNIGHT
void daynight_start(void);
void daynight_stop(void);

/* Ask the automaton to run a probe on its next tick. Returns 0 if it was
 * armed, -1 if this build/camera cannot probe silently (no irprobe_cmd) - the
 * caller can then tell the user why nothing happened instead of guessing. */
int  daynight_request_probe(void);
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
 *   isp_desync  standing disagreement between the decided mode and the ISP's
 *               own readback ("ISP Runing Mode"), DEBOUNCED (the raw compare
 *               flickers during every ordinary switch): 1 = standing (also
 *               warned in the log, reported but never enforced - it may be a
 *               deliberate manual override; a requested probe resolves it),
 *               0 = in agreement, -1 = unknown (no readback / not decided)
 * NULL pointers are allowed for outputs the caller does not need. */
void daynight_get_status(int *enabled, int *mode,
                         float *brightness, float *total_gain, float *exposure,
                         float *ae_luma, float *night_ref, float *probe_bar,
                         int *isp_desync);

/* Today's computed sunrise/sunset for the configured daynight.sun_* location
 * and offsets, formatted as local "HH:MM" into sr_hhmm/ss_hhmm (either may be
 * NULL). Read-only feedback so the WebUI can sanity-check lat/long before
 * trusting it. Returns 1 on a normal day, 0 for polar day/night or without
 * USE_DAYNIGHT (strings then read "--:--"). */
int daynight_sun_status(char *sr_hhmm, char *ss_hhmm, size_t cap);

#endif
