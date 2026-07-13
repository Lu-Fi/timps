/* hal.h - hardware abstraction. A backend captures + encodes and publishes
 * encoded access units into the hub. Backends: ingenic (real SoC) / sim (x86). */
#ifndef MS_HAL_H
#define MS_HAL_H
#include "../config.h"

typedef struct {
    const char *name;
    int  (*init)(const ms_config *cfg);   /* open ISP/sensor/system */
    int  (*start)(const ms_config *cfg);  /* create channels (idle until demanded) */
    void (*request_idr)(int src);         /* force keyframe on a video source */
    void (*set_active)(int src, int on);  /* on-demand: start/stop a source's encode */
    void (*stop)(void);                   /* stop + teardown */
} hal_backend;

/* returns the backend selected at compile time */
const hal_backend *hal_get(void);

/* ISP total gain for day/night detection: the ISP's own overall gain
 * (IMP [24.8] linear, 256 = 1x - the value prudynt/raptor plot), read straight
 * from the IMP API instead of scraping /proc/jz/isp/isp-m0. Returns 0 and fills
 * *gain on success, <0 when unavailable (sim, T40/T41 new-tuning API, or the
 * ISP is not initialised) - daynight then falls back to the proc scrape. */
int hal_isp_total_gain(uint32_t *gain);

/* ISP AE average luminance (raptor's ae_luma) - a secondary day/night metric.
 * IMP_ISP_Tuning_GetAeLuma exists on T21/T23/T31/C100 only; returns 0 and fills
 * *luma on success, <0 when unavailable (other SoCs, sim, ISP down). */
int hal_isp_ae_luma(uint32_t *luma);

#endif
