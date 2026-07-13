/* daynight.h - native automatic day/night detection (compiled only with
 * -DUSE_DAYNIGHT, otherwise the whole feature vanishes from the build).
 *
 * A small pthread samples the Ingenic ISP exposure state
 * (daynight.isp_path, default /proc/jz/isp/isp-m0) every
 * daynight.interval_ms, derives a scene brightness in percent (same formula
 * as thingino's daynightd) and switches between day and night with
 * hysteresis and a minimum dwell time (daynight.transition_s). On a switch
 * it runs "<daynight.switch_cmd> day|night" (default: thingino's /sbin
 * daynight script, which drives ircut / IR LEDs / color -> ISP running_mode)
 * - timps does NOT touch image.running_mode itself, the color hook does.
 *
 * daynight.enabled can be flipped at runtime (config or /control): while
 * disabled the thread keeps sampling (so the status below stays live for the
 * WebUI) but forces nothing (manual mode); re-enabling restarts detection
 * from a clean state. Missing/unreadable ISP file (host sim, non-Ingenic)
 * just skips the cycle. */
#ifndef MS_DAYNIGHT_H
#define MS_DAYNIGHT_H

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

#endif
