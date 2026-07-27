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

#if defined(USE_BACKCHANNEL) || defined(USE_PLAY)
/* Speaker output (IMP_AO). The HAL is the sole owner of the AO device; speaker.c
 * (backchannel + play queue) drives these, opening lazily on first use and
 * closing when idle, mirroring the IMP_AI capture lifecycle. */

/* Bring up AO dev/chn 0 at (or near) want_rate. Returns the sample rate the AO
 * was actually programmed at (>0, may differ if want_rate was unsupported and a
 * fallback was used) or -1 on failure. Idempotent while open (returns the live
 * rate). */
int  hal_ao_open(int want_rate);

/* Send nsamp mono int16 samples (at the rate hal_ao_open returned). Blocks with
 * the AO's own buffering as backpressure. Returns 0 on success, <0 on error. */
int  hal_ao_write(const int16_t *pcm, int nsamp);

/* Tear down AO dev/chn 0. Idempotent (no-op if not open). drain=1: wait for
 * the ring buffer to actually finish playing out before disabling (use at
 * the natural end of a clip). drain=0: discard whatever is still queued and
 * disable immediately (use when preempting/stopping - e.g. backchannel must
 * take the speaker without waiting out a play tail). */
void hal_ao_close(int drain);

void hal_ao_set_vol(int vol);    /* IMP_AO_SetVol, clamped to the SDK range */
void hal_ao_set_gain(int gain);  /* IMP_AO_SetGain, clamped to the SDK range */
#endif

#endif
