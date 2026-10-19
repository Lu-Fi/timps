#ifndef MS_TESTPATTERN_H
#define MS_TESTPATTERN_H
#include <stdint.h>

/* OpenIMP test pattern (OpenIMP_Cap_TestPattern): 0 off, 1 static pattern,
 * 2 moving square + frame counter, 3 motion test with scripted objects.
 * While a pattern runs, day/night is held in day mode. In mode 3 every
 * object appearance is matched against the motion detection. */
enum { TP_OFF = 0, TP_PATTERN = 1, TP_MOVING = 2, TP_MOTION = 3 };

typedef struct {
    int obj_size;        /* mode 3: px of a 640 px wide frame, 0 = default */
    int obj_contrast;    /* -200..200, 0 = default */
    int obj_speed;       /* px/s, -1 = default */
    int obj_cell_x, obj_cell_y;  /* -1 = default */
} tp_params;

typedef struct {
    int      available;
    int      mode;
    uint64_t frames;
    uint64_t objects_shown;
    unsigned objects_active;
    /* mode 3 scoring since the last switch */
    int      motion;             /* motion detection running */
    unsigned objects, hits, misses, false_alarms;
    unsigned lat_avg_ms, lat_max_ms;
} tp_status;

int  tp_available(void);
/* 0 ok, -1 unavailable or refused */
int  tp_set(int mode, const tp_params *p);
void tp_get_status(tp_status *o);
void tp_stop(void);

#endif
