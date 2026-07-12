/* imp_motion.c - grid motion detection via the IMP_IVS move interface.
 *
 * Real implementation only for a target build (-DHAL_INGENIC) whose SDK has
 * the move API (MOTION_AVAILABLE, see ../motion_caps.h); everywhere else the
 * stub at the bottom keeps the daemon linking and reports the feature
 * unavailable (or, on the host sim, echoes the config so the WebUI can be
 * exercised).
 *
 * Grid layout: motion.cols x motion.rows cells split the monitor stream's
 * frame evenly (integer pixel boundaries: cell c spans [c*W/cols,
 * (c+1)*W/cols-1], so rounding is distributed and the last row/column
 * absorbs the remainder). Cell index = row*cols + col (row-major), the same
 * order IMP returns retRoi[] in and the same order the /control status
 * "active" array uses. cols*rows is clamped to the SDK's compile-time
 * IMP_IVS_MOVE_MAX_ROI_CNT (= MOTION_MAX_CELLS; 52 on most SDKs, 4 on the
 * old T10/T20 3.9.0 SDK).
 *
 * Sensitivity: the config keeps the UI range 0..255; IMP's move algorithm
 * takes 0..4 (normal camera; the panoramic 0..8 range is not used), so the
 * value is mapped linearly (v*4/255) and applied to EVERY cell (one global
 * sensitivity for now; per-cell later). */
#include "imp_motion.h"
#include "../motion_caps.h"
#include "../events.h"     /* wake /events SSE subscribers on grid changes */
#include "../log.h"
#include <string.h>

#define MOD "MOTION"

#if defined(HAL_INGENIC) && MOTION_AVAILABLE
#include <imp/imp_ivs.h>
#include <imp/imp_ivs_move.h>
#include <imp/imp_system.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>

static volatile int   g_run;
static pthread_t      g_thr;
static int            g_grp = 0, g_chn = 0;
static int            g_fs_chn = -1;         /* bound FrameSource channel */
static IMPIVSInterface *g_intf;
static const ms_config *g_hcfg;

/* live status snapshot, shared with motion_get_status() */
static pthread_mutex_t g_st_lock = PTHREAD_MUTEX_INITIALIZER;
static ms_motion_status g_st;
static int64_t g_last_event = -1;            /* CLOCK_MONOTONIC ms, -1 never */

/* IMP flags a cell in retRoi[] only on the frame(s) it moved and clears it on
 * the next processed frame. The /events SSE pusher and the /control poller read
 * g_st ASYNCHRONOUSLY (woken by events_notify, then they sample the current
 * state), so they almost always race the clear and observe all-zeros - "an
 * event arrives but every segment is 0". Latch each cell as active for a short
 * hold window after its last hit so any reader reliably catches the motion; the
 * client overlay's afterglow then just smooths the fade. The window is
 * configurable via motion.hold_ms (default 800, 0 = off); read once at start. */
#define MOTION_HOLD_MS_DEFAULT 800
static int     g_hold_ms = MOTION_HOLD_MS_DEFAULT;
static int64_t g_cell_hit[MOTION_STATUS_MAX]; /* last retRoi=1 per cell, mono ms */

static int64_t now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
                             return (int64_t)t.tv_sec*1000 + t.tv_nsec/1000000; }

/* effective grid geometry from the config: >=1x1 and cols*rows clamped to
 * MOTION_MAX_CELLS (rows gives way, mirroring the clamp in config.c) */
static void grid_geom(const ms_config *cfg, int *cols, int *rows)
{
    int c = cfg->motion.cols  > 0 ? cfg->motion.cols  : 1;
    int r = cfg->motion.rows  > 0 ? cfg->motion.rows  : 1;
    if (c > MOTION_MAX_CELLS) c = MOTION_MAX_CELLS;
    if (r > MOTION_MAX_CELLS) r = MOTION_MAX_CELLS;
    if (c * r > MOTION_MAX_CELLS) r = MOTION_MAX_CELLS / c;   /* >=1: c<=MAX */
    *cols = c; *rows = r;
}

static void *motion_thread(void *arg)
{
    (void)arg;
    IMP_IVS_MoveOutput *result;
    int64_t last_fire = 0;
    while (g_run) {
        if (IMP_IVS_PollingResult(g_chn, 1000) < 0) continue;
        if (IMP_IVS_GetResult(g_chn, (void**)&result) < 0) continue;
        int cells = g_st.cells;              /* fixed while the thread runs */
        int detected = 0, changed = 0, any_held = 0;
        int64_t nowm = now_ms();
        pthread_mutex_lock(&g_st_lock);
        for (int i=0;i<cells;i++){
            /* raw = motion on THIS frame; drives the on_motion trigger and the
             * last-event clock. held = latched view (raw or within HOLD_MS of
             * the last hit) - that is what readers see, so single-frame motion
             * is never lost to the async sampling race. */
            unsigned char raw = result->retRoi[i] ? 1 : 0;
            if (raw) { g_cell_hit[i] = nowm; detected = 1; }
            /* raw = this frame; extend it for g_hold_ms (0 = pure passthrough) */
            unsigned char held = raw;
            if (!held && g_hold_ms > 0 && g_cell_hit[i] > 0 &&
                nowm - g_cell_hit[i] < g_hold_ms) held = 1;
            if (held) any_held = 1;
            if (held != g_st.active[i]){ g_st.active[i] = held; changed = 1; }
        }
        g_st.any = any_held;
        if (detected) g_last_event = nowm;
        pthread_mutex_unlock(&g_st_lock);
        if (changed) events_notify();        /* push the new grid to /events */
        if (detected) {
            int64_t t = now_ms();
            if (t - last_fire >= g_hcfg->motion.cooldown_ms) {
                last_fire = t;
                LOGI(MOD,"motion detected");
                if (g_hcfg->motion.on_motion[0]) {
                    /* SECURITY: on_motion is passed to system() and therefore
                     * runs through /bin/sh with the daemon's privileges. The
                     * value comes ONLY from the local config file (it is
                     * deliberately NOT settable via /control) - keep it that
                     * way. Anyone who can edit the config can already run
                     * arbitrary commands, but never feed user/remote-
                     * controlled strings into this field. */
                    char cmd[160];
                    snprintf(cmd,sizeof cmd,"%s &", g_hcfg->motion.on_motion);
                    if (system(cmd)!=0) LOGW(MOD,"on_motion cmd failed");
                }
            }
        }
        IMP_IVS_ReleaseResult(g_chn, (void*)result);
    }
    return NULL;
}

int imp_motion_start(const ms_config *cfg)
{
    g_hcfg = cfg;
    int mon = cfg->motion.monitor_stream;
    if (mon < 0 || mon >= MS_MAX_VSTREAM || !cfg->video[mon].enabled) mon = 0;
    int w = cfg->video[mon].width, h = cfg->video[mon].height;
    int cols, rows;
    grid_geom(cfg, &cols, &rows);
    int cells = cols * rows;

    /* IMP sensitivity is 0..4 (normal camera); map the 0..255 config value */
    int sense = cfg->motion.sensitivity * 4 / 255;
    if (sense < 0) sense = 0;
    if (sense > 4) sense = 4;

    if (IMP_IVS_CreateGroup(g_grp)<0){ LOGE(MOD,"CreateGroup failed"); return -1; }

    IMP_IVS_MoveParam mp; memset(&mp,0,sizeof mp);
    mp.skipFrameCnt = cfg->motion.skip_frames >= 1 ? cfg->motion.skip_frames : 5;
    mp.frameInfo.width  = w;
    mp.frameInfo.height = h;
    for (int r=0;r<rows;r++){
        for (int c=0;c<cols;c++){
            int i = r*cols + c;
            mp.sense[i] = sense;
            mp.roiRect[i].p0.x = c*w/cols;
            mp.roiRect[i].p0.y = r*h/rows;
            mp.roiRect[i].p1.x = (c+1)*w/cols - 1;
            mp.roiRect[i].p1.y = (r+1)*h/rows - 1;
        }
    }
    mp.roiRectCnt = cells;

    g_intf = IMP_IVS_CreateMoveInterface(&mp);
    if (!g_intf){ LOGE(MOD,"CreateMoveInterface failed"); goto err_grp; }
    if (IMP_IVS_CreateChn(g_chn, g_intf)<0){ LOGE(MOD,"CreateChn failed"); goto err_intf; }
    if (IMP_IVS_RegisterChn(g_grp, g_chn)<0){ LOGE(MOD,"RegisterChn failed"); goto err_chn; }

    /* feed the IVS group from the monitored stream's FrameSource (the same
     * FS output also drives OSD/encoder - IMP binds broadcast per output).
     * The caller (hal_ingenic.c) pins that FS via fs_use() so the idle
     * logic never disables it while motion runs. */
    {
        IMPCell fs  = { DEV_ID_FS,  cfg->video[mon].imp_chn, 0 };
        IMPCell ivs = { DEV_ID_IVS, g_grp, 0 };
        if (IMP_System_Bind(&fs, &ivs)<0){ LOGE(MOD,"Bind FS->IVS failed"); goto err_reg; }
        g_fs_chn = cfg->video[mon].imp_chn;
    }
    if (IMP_IVS_StartRecvPic(g_chn)<0){ LOGE(MOD,"StartRecvPic failed"); goto err_bind; }

    g_hold_ms = cfg->motion.hold_ms >= 0 ? cfg->motion.hold_ms : MOTION_HOLD_MS_DEFAULT;

    pthread_mutex_lock(&g_st_lock);
    memset(&g_st, 0, sizeof g_st);
    memset(g_cell_hit, 0, sizeof g_cell_hit);
    g_st.available = 1; g_st.enabled = 1;
    g_st.cols = cols; g_st.rows = rows; g_st.cells = cells;
    g_st.sensitivity = cfg->motion.sensitivity;
    pthread_mutex_unlock(&g_st_lock);

    g_run = 1;
    if (pthread_create(&g_thr, NULL, motion_thread, NULL) != 0){
        /* no thread -> full rollback; g_run stays 0 so stop() never joins */
        LOGE(MOD,"motion thread create failed");
        g_run = 0;
        pthread_mutex_lock(&g_st_lock);
        g_st.enabled = 0;
        pthread_mutex_unlock(&g_st_lock);
        IMP_IVS_StopRecvPic(g_chn);
        goto err_bind;
    }
    events_notify();                         /* status flipped to enabled */
    LOGI(MOD,"motion detection started (%dx%d grid %dx%d=%d cells, sense=%d/4, max %d, hold=%dms)",
         w, h, cols, rows, cells, sense, MOTION_MAX_CELLS, g_hold_ms);
    return 0;

err_bind:
    { IMPCell fs={DEV_ID_FS,g_fs_chn,0}, ivs={DEV_ID_IVS,g_grp,0};
      IMP_System_UnBind(&fs,&ivs); g_fs_chn=-1; }
err_reg:
    IMP_IVS_UnRegisterChn(g_chn);
err_chn:
    IMP_IVS_DestroyChn(g_chn);
err_intf:
    IMP_IVS_DestroyMoveInterface(g_intf); g_intf=NULL;
err_grp:
    IMP_IVS_DestroyGroup(g_grp);
    return -1;
}

void imp_motion_stop(void)
{
    if (!g_run) return;
    g_run = 0;
    pthread_join(g_thr, NULL);
    IMP_IVS_StopRecvPic(g_chn);
    if (g_fs_chn >= 0){
        IMPCell fs={DEV_ID_FS,g_fs_chn,0}, ivs={DEV_ID_IVS,g_grp,0};
        IMP_System_UnBind(&fs,&ivs);
        g_fs_chn = -1;
    }
    IMP_IVS_UnRegisterChn(g_chn);
    IMP_IVS_DestroyChn(g_chn);
    if (g_intf){ IMP_IVS_DestroyMoveInterface(g_intf); g_intf=NULL; }
    IMP_IVS_DestroyGroup(g_grp);
    pthread_mutex_lock(&g_st_lock);
    g_st.enabled = 0; g_st.any = 0;
    memset(g_st.active, 0, sizeof g_st.active);
    pthread_mutex_unlock(&g_st_lock);
    events_notify();                         /* status flipped to disabled */
    LOGI(MOD,"motion detection stopped");
}

void motion_get_status(ms_motion_status *st)
{
    pthread_mutex_lock(&g_st_lock);
    *st = g_st;
    st->last_ms = (g_last_event < 0) ? -1 : now_ms() - g_last_event;
    pthread_mutex_unlock(&g_st_lock);
    st->available = 1;
}

#else /* !HAL_INGENIC || !MOTION_AVAILABLE: no-op + status stub */

int imp_motion_start(const ms_config *cfg)
{
    (void)cfg;
#if !MOTION_AVAILABLE
    LOGW(MOD,"IMP_IVS move detection not available in this build - motion detection disabled");
#endif
    return -1;                       /* never "running": caller pins nothing */
}
void imp_motion_stop(void){}

void motion_get_status(ms_motion_status *st)
{
    memset(st, 0, sizeof *st);
    st->available = MOTION_AVAILABLE;
    st->last_ms = -1;
#if MOTION_AVAILABLE
    /* host sim: echo the configured grid (all cells idle) so the WebUI page
     * and preview overlay can be exercised against timpsd-sim */
    int c = g_cfg.motion.cols > 0 ? g_cfg.motion.cols : 1;
    int r = g_cfg.motion.rows > 0 ? g_cfg.motion.rows : 1;
    if (c > MOTION_MAX_CELLS) c = MOTION_MAX_CELLS;
    if (r > MOTION_MAX_CELLS) r = MOTION_MAX_CELLS;
    if (c * r > MOTION_MAX_CELLS) r = MOTION_MAX_CELLS / c;
    st->enabled = g_cfg.motion.enabled;
    st->cols = c; st->rows = r;
    st->cells = (c * r <= MOTION_STATUS_MAX) ? c * r : MOTION_STATUS_MAX;
    st->sensitivity = g_cfg.motion.sensitivity;
#endif
}
#endif
