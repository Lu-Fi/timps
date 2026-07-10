/* imp_motion.c - motion detection via IMP_IVS move interface.
 * Compiled only for the target (-DHAL_INGENIC). Modelled on prudynt/Motion. */
#include "imp_motion.h"
#ifdef HAL_INGENIC
#include "../log.h"
#include <imp/imp_ivs.h>
#include <imp/imp_ivs_move.h>
#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>

#define MOD "MOTION"
static volatile int   g_run;
static pthread_t      g_thr;
static int            g_grp = 0, g_chn = 0;
static const ms_config *g_hcfg;

static int64_t now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
                             return (int64_t)t.tv_sec*1000 + t.tv_nsec/1000000; }

static void *motion_thread(void *arg)
{
    (void)arg;
    IMP_IVS_MoveOutput *result;
    int64_t last_event = 0;
    while (g_run) {
        if (IMP_IVS_PollingResult(g_chn, 1000) < 0) continue;
        if (IMP_IVS_GetResult(g_chn, (void**)&result) < 0) continue;
        int detected = 0;
        for (int i=0;i<IMP_IVS_MOVE_MAX_ROI_CNT;i++)
            if (result->retRoi[i]) { detected = 1; break; }
        if (detected) {
            int64_t t = now_ms();
            if (t - last_event >= g_hcfg->motion.cooldown_ms) {
                last_event = t;
                LOGI(MOD,"motion detected");
                if (g_hcfg->motion.on_motion[0]) {
                    /* SECURITY: on_motion is passed to system() and therefore
                     * runs through /bin/sh with the daemon's privileges. The
                     * value comes ONLY from the local config file (no network
                     * input) - keep it that way. Anyone who can edit the
                     * config can already run arbitrary commands, but never
                     * feed user/remote-controlled strings into this field. */
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
    int w = cfg->video[mon].width, h = cfg->video[mon].height;

    if (IMP_IVS_CreateGroup(g_grp)<0){ LOGE(MOD,"CreateGroup failed"); return -1; }

    IMP_IVS_MoveParam mp; memset(&mp,0,sizeof mp);
    /* IMP sensitivity is 0..4; map 0..255 config -> 0..4 */
    mp.sense[0] = cfg->motion.sensitivity * 4 / 255;
    mp.skipFrameCnt = 5;
    mp.frameInfo.width  = w;
    mp.frameInfo.height = h;
    int rx=cfg->motion.roi_x, ry=cfg->motion.roi_y;
    int rw=cfg->motion.roi_w?cfg->motion.roi_w:w, rh=cfg->motion.roi_h?cfg->motion.roi_h:h;
    mp.roiRect[0].p0.x = rx; mp.roiRect[0].p0.y = ry;
    mp.roiRect[0].p1.x = rx+rw-1; mp.roiRect[0].p1.y = ry+rh-1;
    mp.roiRectCnt = 1;

    IMPIVSInterface *intf = IMP_IVS_CreateMoveInterface(&mp);
    if (IMP_IVS_CreateChn(g_chn, intf)<0){ LOGE(MOD,"CreateChn failed"); return -1; }
    if (IMP_IVS_RegisterChn(g_grp, g_chn)<0){ LOGE(MOD,"RegisterChn failed"); return -1; }
    if (IMP_IVS_StartRecvPic(g_chn)<0){ LOGE(MOD,"StartRecvPic failed"); return -1; }

    g_run = 1;
    pthread_create(&g_thr, NULL, motion_thread, NULL);
    LOGI(MOD,"motion detection started (%dx%d sense=%d)", w, h, mp.sense[0]);
    return 0;
}

void imp_motion_stop(void)
{
    if (!g_run) return;
    g_run = 0;
    pthread_join(g_thr, NULL);
    IMP_IVS_StopRecvPic(g_chn);
    IMP_IVS_UnRegisterChn(g_chn);
    IMP_IVS_DestroyChn(g_chn);
    IMP_IVS_DestroyGroup(g_grp);
}
#else
int  imp_motion_start(const ms_config *cfg){ (void)cfg; return 0; }
void imp_motion_stop(void){}
#endif
