/* timelapse.h - native timelapse: periodic JPEG snapshots from the hub's
 * JPEG source to <timelapse.dir>/<host>/timelapses/, pruned after keep_days. */
#ifndef MS_TIMELAPSE_H
#define MS_TIMELAPSE_H
#include "config.h"

/* Start / stop the timelapse thread. Safe to call when disabled (the thread
 * idles, unsubscribed, until timelapse.enabled). timelapse_stop() is
 * idempotent. */
void timelapse_start(const ms_config *cfg);
void timelapse_stop(void);

#ifdef USE_CONTROL
typedef struct {
    int       available;     /* built with timelapse support */
    int       enabled;       /* configured on (timelapse.enabled) */
    int       interval_s;    /* seconds between shots */
    long long count;         /* shots written since daemon start */
    long long last_t;        /* unix time of the last shot, 0 = never */
    long long free_mb;       /* free space on the timelapse directory */
    char      file[160];     /* last written file ("" = none yet) */
} ms_timelapse_status;

void timelapse_get_status(ms_timelapse_status *st);
#endif

#endif
