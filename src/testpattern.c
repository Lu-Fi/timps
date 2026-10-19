#include "testpattern.h"
#include "hal/hal.h"
#include "hal/imp_motion.h"
#include "daynight.h"
#include "util.h"
#include "log.h"
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#define MOD "TESTPAT"
#define TP_TICK_MS  100
#define TP_GRACE_MS 1500   /* motion reported this long after an object ends still counts */

static pthread_mutex_t g_tp_mtx = PTHREAD_MUTEX_INITIALIZER;
static tp_status g_ev;
static int g_mode;
static volatile int g_run;
static pthread_t g_thr;
static int g_thr_on;
static uint64_t g_lat_sum;

static int64_t now_ms(void) { return ms_now_us() / 1000; }

static void *tp_eval_thread(void *a)
{
    (void)a;
    int prev_obj = 0, prev_any = 0, open = 0, hit = 0;
    int64_t t_obj = 0, t_end = -1000000;
    while (g_run) {
        hal_tp_state st;
        ms_motion_status ms;
        int ok = hal_tp_get(&st) == 0;
        motion_get_status(&ms);
        int64_t t = now_ms();
        int obj = ok && st.objects_active > 0;
        int any = ms.enabled && ms.any;
        pthread_mutex_lock(&g_tp_mtx);
        g_ev.motion = ms.enabled;
        if (obj && !prev_obj) {
            g_ev.objects++;
            open = 1; hit = 0; t_obj = t;
        } else if (!obj && prev_obj) {
            t_end = t;
        }
        if (any && !prev_any) {
            if ((obj || t - t_end < TP_GRACE_MS) && open && !hit) {
                unsigned lat = (unsigned)(t - t_obj);
                hit = 1;
                g_ev.hits++;
                g_lat_sum += lat;
                g_ev.lat_avg_ms = (unsigned)(g_lat_sum / g_ev.hits);
                if (lat > g_ev.lat_max_ms) g_ev.lat_max_ms = lat;
            } else if (!obj && t - t_end >= TP_GRACE_MS) {
                g_ev.false_alarms++;
            }
        }
        if (open && !obj && t - t_end >= TP_GRACE_MS) {
            if (!hit) g_ev.misses++;
            open = 0;
        }
        pthread_mutex_unlock(&g_tp_mtx);
        prev_obj = obj; prev_any = any;
        usleep(TP_TICK_MS * 1000);
    }
    return NULL;
}

static void eval_stop(void)
{
    if (!g_thr_on) return;
    g_run = 0;
    pthread_join(g_thr, NULL);
    g_thr_on = 0;
}

int tp_available(void) { return hal_tp_available(); }

int tp_set(int mode, const tp_params *p)
{
    if (mode < TP_OFF || mode > TP_MOTION || !hal_tp_available()) return -1;
    hal_tp_params hp = { -1, 0, -1, -1, -1 };
    if (p) {
        hp.obj_size = p->obj_size; hp.obj_contrast = p->obj_contrast;
        hp.obj_speed = p->obj_speed; hp.obj_cell_x = p->obj_cell_x; hp.obj_cell_y = p->obj_cell_y;
    }
    eval_stop();
    if (mode != TP_OFF) daynight_hold(1);
    if (hal_tp_set(mode, &hp) != 0) {
        LOGW(MOD, "test pattern mode %d refused by libimp", mode);
        if (g_mode == TP_OFF) daynight_hold(0);
        return -1;
    }
    if (mode == TP_OFF) daynight_hold(0);
    pthread_mutex_lock(&g_tp_mtx);
    memset(&g_ev, 0, sizeof g_ev);
    g_lat_sum = 0;
    g_mode = mode;
    pthread_mutex_unlock(&g_tp_mtx);
    if (mode == TP_MOTION) {
        g_run = 1;
        if (ms_thread_create(&g_thr, MS_STACK_UTIL, tp_eval_thread, NULL) == 0) g_thr_on = 1;
        else g_run = 0;
    }
    LOGI(MOD, "test pattern mode %d%s", mode, mode ? " (day/night held in day)" : "");
    return 0;
}

void tp_get_status(tp_status *o)
{
    pthread_mutex_lock(&g_tp_mtx);
    *o = g_ev;
    o->mode = g_mode;
    pthread_mutex_unlock(&g_tp_mtx);
    o->available = hal_tp_available();
    hal_tp_state st;
    if (o->available && hal_tp_get(&st) == 0) {
        o->mode = st.mode;
        o->frames = st.frames;
        o->objects_shown = st.objects_shown;
        o->objects_active = st.objects_active;
    }
}

void tp_stop(void)
{
    eval_stop();
    if (g_mode != TP_OFF && hal_tp_available()) hal_tp_set(TP_OFF, NULL);
    g_mode = TP_OFF;
    daynight_hold(0);
}
