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
 * disabled the thread idles (manual mode); re-enabling restarts detection
 * from a clean state. Missing/unreadable ISP file (host sim, non-Ingenic)
 * just skips the cycle. */
#ifndef MS_DAYNIGHT_H
#define MS_DAYNIGHT_H

#ifdef USE_DAYNIGHT
void daynight_start(void);
void daynight_stop(void);
#endif

#endif
