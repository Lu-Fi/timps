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

#endif
