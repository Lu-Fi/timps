#include "testpattern.h"
#include "hal/hal.h"
#include "hal/imp_motion.h"
#include "daynight.h"
#include "util.h"
#include "log.h"
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#define MOD "TESTPAT"
#define TP_TICK_MS  100
#define TP_GRACE_MS 1500   /* motion reported this long after an object ends still counts */
#define TP_SCRIPT   "/tmp/timps-tp-events"

/* default motion test: one moving 32 px object every 6 s, each in another
 * cell of the 8 x 6 grid (motion v2 learns a spot that repeats), away from
 * the frame counter (top left) and the centred logo */
static const unsigned char k_cells[][2] = {
    {1,1},{6,1},{1,4},{6,4},{2,1},{5,1},{2,4},{5,4},
    {1,2},{6,2},{1,3},{6,3},{0,5},{7,5},{7,1},{0,3},
};
#define TP_NOBJ ((int)(sizeof k_cells / sizeof k_cells[0]))
static int g_scripted;

static pthread_mutex_t g_tp_mtx = PTHREAD_MUTEX_INITIALIZER;
static tp_status g_ev;
static int g_mode;
static volatile int g_run;
static pthread_t g_thr;
static int g_thr_on;
static uint64_t g_lat_sum;

static int64_t now_ms(void) { return ms_now_us() / 1000; }

/* motion grid cells overlapping cell (cx,cy) of the pattern's 8 x 6 grid */
static void obj_cells(const ms_motion_status *ms, int cx, int cy, unsigned char *m)
{
    memset(m, 0, MOTION_STATUS_MAX);
    for (int r = 0; r < ms->rows; r++)
        for (int c = 0; c < ms->cols; c++) {
            int i = r * ms->cols + c;
            if (i >= MOTION_STATUS_MAX) return;
            /* overlap of [c/cols,(c+1)/cols) with [cx/8,(cx+1)/8) etc., cross-multiplied */
            int ox = c * 8 < (cx + 1) * ms->cols && cx * ms->cols < (c + 1) * 8;
            int oy = r * 6 < (cy + 1) * ms->rows && cy * ms->rows < (r + 1) * 6;
            m[i] = ox && oy;
        }
}

/* An object counts as detected when one of the motion cells over its grid
 * cell turns active while it is shown (or up to TP_GRACE_MS after). Cells
 * that stay active (frame counter, OSD clock) never make an edge, so they
 * neither hit nor raise false alarms. A rising cell elsewhere while no
 * object is up is a false alarm. Scripted objects fall back to any cell. */
static int tp_write_script(void)
{
    FILE *f = fopen(TP_SCRIPT, "w");
    if (!f) return -1;
    for (int i = 0; i < TP_NOBJ; i++)
        fprintf(f, "t=%d cell=%d,%d size=32 contrast=40 dur=3 speed=40\n",
                6 + 6 * i, k_cells[i][0], k_cells[i][1]);
    return fclose(f) == 0 ? 0 : -1;
}

static void *tp_eval_thread(void *a)
{
    (void)a;
    unsigned char prev[MOTION_STATUS_MAX], om[MOTION_STATUS_MAX];
    memset(prev, 0, sizeof prev);
    int prev_obj = 0, open = 0, hit = 0, k = -1, cx = 0, cy = 0;
    int64_t t_obj = 0, t_end = -1000000;
    while (g_run) {
        hal_tp_state st;
        ms_motion_status ms;
        int ok = hal_tp_get(&st) == 0;
        motion_get_status(&ms);
        int64_t t = now_ms();
        int obj = ok && st.objects_active > 0;
        int n = ms.cells < MOTION_STATUS_MAX ? ms.cells : MOTION_STATUS_MAX;
        if (obj && !prev_obj) {
            k++;
            if (g_scripted && k < TP_NOBJ) { cx = k_cells[k][0]; cy = k_cells[k][1]; }
            else { cx = st.obj_cell_x; cy = st.obj_cell_y; }
        }
        if (ok && (g_scripted ? k < TP_NOBJ : !st.scripted)) obj_cells(&ms, cx, cy, om);
        else memset(om, 1, sizeof om);
        int rise_in = 0, rise_out = 0;
        if (ms.enabled)
            for (int i = 0; i < n; i++)
                if (ms.active[i] && !prev[i]) {
                    if (om[i]) rise_in = 1; else rise_out = 1;
                }
        pthread_mutex_lock(&g_tp_mtx);
        g_ev.motion = ms.enabled;
        if (obj && !prev_obj) {
            g_ev.objects++;
            open = 1; hit = 0; t_obj = t;
        } else if (!obj && prev_obj) {
            t_end = t;
        }
        int live = obj || t - t_end < TP_GRACE_MS;
        if (rise_in && open && !hit && live) {
            unsigned lat = (unsigned)(t - t_obj);
            hit = 1;
            g_ev.hits++;
            g_lat_sum += lat;
            g_ev.lat_avg_ms = (unsigned)(g_lat_sum / g_ev.hits);
            if (lat > g_ev.lat_max_ms) g_ev.lat_max_ms = lat;
        } else if ((rise_out || rise_in) && !live) {
            g_ev.false_alarms++;
        }
        if (open && !live) {
            if (!hit) g_ev.misses++;
            open = 0;
        }
        pthread_mutex_unlock(&g_tp_mtx);
        prev_obj = obj;
        for (int i = 0; i < n; i++) prev[i] = ms.enabled && ms.active[i];
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
    hal_tp_params hp = { -1, 0, -1, -1, -1, NULL };
    if (p) {
        hp.obj_size = p->obj_size; hp.obj_contrast = p->obj_contrast;
        hp.obj_speed = p->obj_speed; hp.obj_cell_x = p->obj_cell_x; hp.obj_cell_y = p->obj_cell_y;
    }
    int scripted = mode == TP_MOTION && hp.obj_size <= 0 && hp.obj_cell_x < 0 && hp.obj_cell_y < 0 &&
                   tp_write_script() == 0;
    if (scripted) hp.script = TP_SCRIPT;
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
    g_scripted = scripted;
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
