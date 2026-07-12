/* imp_motion.h - Ingenic IMP_IVS grid motion detection (SoC feature).
 *
 * The detection area is an N x M GRID of IMP_IVS move-ROIs laid evenly over
 * the motion.monitor_stream frame (motion.cols x motion.rows cells, clamped
 * to the SDK's MOTION_MAX_CELLS - see ../motion_caps.h). The polling thread
 * keeps a live per-cell snapshot that motion_get_status() hands to the
 * /control status JSON, and fires motion.on_motion (with cooldown) when ANY
 * cell trips. On builds without the IMP_IVS move API (MOTION_AVAILABLE 0)
 * everything is a no-op and the status reports available:0. */
#ifndef MS_IMP_MOTION_H
#define MS_IMP_MOTION_H
#include "../config.h"
#include <stdint.h>

/* fixed status-array size, >= any SDK's IMP_IVS_MOVE_MAX_ROI_CNT (52 max
 * across the vendored SDKs) so the struct layout never depends on the SDK */
#define MOTION_STATUS_MAX 64

typedef struct {
    int      available;    /* 1 = IMP_IVS move usable in this build */
    int      enabled;      /* 1 = detection currently running */
    int      cols, rows;   /* grid geometry actually in use */
    int      cells;        /* cols*rows (= length of active[]) */
    int      sensitivity;  /* 0..255 UI sensitivity in use */
    int      any;          /* any cell active in the latest result */
    int64_t  last_ms;      /* ms since the last motion event, -1 = never */
    unsigned char active[MOTION_STATUS_MAX]; /* per-cell 0/1, row-major
                                              * (index = row*cols + col) */
} ms_motion_status;

int  imp_motion_start(const ms_config *cfg);   /* 0 ok, <0 not started */
void imp_motion_stop(void);
/* thread-safe snapshot of the latest detection state (always linkable:
 * without the move API a stub answers available:0 / empty grid) */
void motion_get_status(ms_motion_status *st);
#endif
