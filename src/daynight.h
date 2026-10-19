/* daynight.h - native automatic day/night detection (compiled only with
 * -DUSE_DAYNIGHT, otherwise the whole feature vanishes from the build).
 *
 * A small pthread decides day vs night and on a change runs
 * "<daynight.switch_cmd> day|night" (default: thingino's /sbin daynight
 * script, which drives ircut / IR LEDs / color -> ISP running_mode).
 *
 * The decision (dev_notes/dn-redesign.md, user description there):
 *   day -> night  the exposure index above night_gain, or the AE at its
 *                 limit, held day_confirm_s (6 s when it is very dark).
 *   night         now and then the illuminator goes off for 8 s and the
 *                 camera "looks": every 45 min, at once when the night picture
 *                 gets jump_pct brighter or its colour moves color_pct, or on
 *                 a /control request.
 *   look -> check if the scene without the illuminator looks bright enough,
 *                 the day pipeline is asked once (a check, one click): below
 *                 day_gain it stays day, else back to night, and the next
 *                 check needs a darker look and waits 1, 2, 4 ... min.
 *   guarantee     no day-pipeline reading for heartbeat_max_s: check anyway.
 *   no LED        cameras without a separately switchable illuminator check
 *                 on a doubling of the night brightness and every heartbeat_s.
 *   boot          a check. Idle-Dark (ir_idle_off) is a look that lasts while
 *                 nobody watches.
 *
 * The metric is the EXPOSURE INDEX D = total_gain * integration_time /
 * max_integration_time (higher = darker). daynight.mode = schedule lets the
 * configured calendar decide instead. */
#ifndef MS_DAYNIGHT_H
#define MS_DAYNIGHT_H

#include <stddef.h>   /* size_t (daynight_sun_status) */

/* fixed timing, the same on every camera */
#define DN_PROBE_SETTLE_S    8     /* AE settle before a look or a check is read */
#define DN_REF_DELAY_S       30    /* settle after entering night before the night level counts */
#define DN_IR_MIN_HEADROOM   8     /* AE reserve (log2 units) below which a reading is a clip */
#define DN_BOOT_SETTLE_S     5     /* min wait before the boot check */

#ifdef USE_DAYNIGHT
void daynight_start(void);
void daynight_stop(void);
/* main loop, once a second: one error when the detection thread stopped ticking */
void daynight_check_alive(void);
#endif

/* Ask for a look on the next tick. Returns 0 if it was armed, -1 if this
 * build/camera cannot look silently (no irprobe_cmd, or no USE_DAYNIGHT at
 * all) - the caller can then tell the user why nothing happened. Always linkable, like the status readout
 * below: control.c's probe command must not need its own USE_DAYNIGHT guard. */
int  daynight_request_probe(void);

/* Test pattern: 1 = hold day (IR-cut in, illuminator off, engine paused),
 * 0 = release, the engine starts over with its boot check. */
void daynight_hold(int on);

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
 *   night_ref   the night level (illuminator on) at the last look; -1 when
 *               not in night or not settled yet
 *   probe_bar   the level the reading WITHOUT the illuminator must fall below
 *               for a look to ask for a check; -1 when not in night
 *   isp_desync  standing disagreement between the decided mode and the ISP's
 *               own readback ("ISP Runing Mode") outside the readback window:
 *               1 = standing (reported, never enforced - it may be a manual
 *               override), 0 = in agreement, -1 = unknown
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

/* A picture consumer arrived (hub activity callback, any video/JPEG source).
 * Non-blocking: a flag and, while Idle-Dark is armed, a gate kick; the
 * day/night thread reads the consumer level itself. Always linkable. */
void daynight_watch_kick(void);

/* Idle-Dark status (daynight.ir_idle_off) for GET /control and /events:
 * cfg the configured switch, available whether it may act (reason says why
 * not: "disabled", "keepalive", "no_illuminator", "white_light", "hook"),
 * dark the illuminator is off for idle, watched a consumer is present,
 * linger_left_s until an unwatched night may go dark. */
typedef struct {
    int cfg, available, dark, watched, linger_left_s;
    const char *reason;
} dn_ir_idle_status;
void daynight_ir_idle_status(dn_ir_idle_status *o);

/* colour trigger (daynight.color_pct): "on", "off" or "unavailable" (this
 * libimp gives no white-balance statistics); *dev the colour change since
 * the last look in %, -1 without a reference */
const char *daynight_color_state(float *dev);

#endif
