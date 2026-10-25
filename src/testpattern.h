#ifndef MS_TESTPATTERN_H
#define MS_TESTPATTERN_H
#include <stdint.h>

/* OpenIMP test pattern (OpenIMP_Cap_TestPattern): 0 off, 1 static pattern,
 * 2 moving square + frame counter, 3 motion test with scripted objects,
 * 4 the sensor's own pattern through the whole ISP (AE/AWB hold options),
 * 5 a still pattern written into the middle of the ISP (after sharpening;
 * OpenIMP_TestPattern_Supported(5), T23 so far).
 * While a pattern runs, day/night is held in day mode. In mode 3 every
 * object appearance is matched against the motion detection. */
enum { TP_OFF = 0, TP_PATTERN = 1, TP_MOVING = 2, TP_MOTION = 3, TP_SENSOR = 4, TP_ISP_MID = 5 };

typedef struct {
    int obj_size;        /* mode 3: px of a 640 px wide frame, 0 = default */
    int obj_contrast;    /* -200..200, 0 = default */
    int obj_speed;       /* px/s, -1 = default */
    int obj_cell_x, obj_cell_y;  /* -1 = default */
    int ae_hold, awb_hold;       /* mode 4, -1 = default */
} tp_params;

typedef struct {
    int      available;
    int      sensor;             /* mode 4 possible */
    int      isp_mid;            /* mode 5 possible */
    char     sensor_name[32];
    const char *sensor_desc;
    int      ae_hold, awb_hold;
    int      mode;
    int      pending;            /* mode 4 waits for day mode to settle */
    int      refused;            /* last mode the libimp refused, 0 = none */
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
