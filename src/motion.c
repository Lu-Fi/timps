#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <imp/imp_ivs.h>
#include <imp/imp_ivs_move.h>
#include <imp/imp_common.h>
#include "motion.h"
#include "config.h"
#include "log.h"

#define IVS_GRP  0
#define IVS_CHN  0

static volatile int g_motion_flag = 0;
static volatile int g_running     = 0;
static pthread_t    g_thread;
static IMPIVSInterface *g_ivs_iface = NULL;

static void *motion_thread(void *arg)
{
    IMPIVSMoveOutput *output;
    (void)arg;

    log_debug("motion thread started");
    while (g_running) {
        int ret = IMP_IVS_PollingResult(IVS_CHN, 1000);
        if (ret < 0) { log_warn("IMP_IVS_PollingResult: %d", ret); continue; }
        if (ret == 0) continue;

        ret = IMP_IVS_GetResult(IVS_CHN, (void **)&output);
        if (ret) continue;

        for (int i = 0; i < output->retRoi.roiCount; i++) {
            if (output->retRoi.roiList[i]) {
                log_info("motion detected (roi %d)", i);
                g_motion_flag = 1;
                if (g_cfg.motion_script[0]) {
                    /* Run user script asynchronously */
                    char cmd[300];
                    snprintf(cmd, sizeof(cmd), "%s &", g_cfg.motion_script);
                    if (system(cmd) < 0)
                        log_warn("motion script exec failed");
                }
                break;
            }
        }
        IMP_IVS_ReleaseResult(IVS_CHN, output);
    }
    log_debug("motion thread exiting");
    return NULL;
}

int motion_init(void)
{
    if (!g_cfg.motion_enabled) return 0;

    IMPIVSMoveParam param;
    memset(&param, 0, sizeof(param));
    param.skipFrameCnt      = 5;
    param.frameInfo.width   = g_cfg.stream[0].width;
    param.frameInfo.height  = g_cfg.stream[0].height;
    param.roiRectCnt        = 1;
    param.roiRect[0].p0.x   = 0;
    param.roiRect[0].p0.y   = 0;
    param.roiRect[0].p1.x   = g_cfg.stream[0].width;
    param.roiRect[0].p1.y   = g_cfg.stream[0].height;
    param.sense[0]          = g_cfg.motion_sensitivity;

    g_ivs_iface = IMP_IVS_CreateMoveInterface(&param);
    if (!g_ivs_iface) {
        log_error("IMP_IVS_CreateMoveInterface failed");
        return -1;
    }

    IMP_IVS_CreateGroup(IVS_GRP);
    IMP_IVS_CreateChn(IVS_CHN, g_ivs_iface);
    IMP_IVS_RegisterChn(IVS_GRP, IVS_CHN);
    IMP_IVS_StartRecvPic(IVS_CHN);

    g_running = 1;
    if (pthread_create(&g_thread, NULL, motion_thread, NULL)) {
        log_error("motion thread create failed");
        g_running = 0;
        IMP_IVS_StopRecvPic(IVS_CHN);
        IMP_IVS_UnRegisterChn(IVS_CHN);
        IMP_IVS_DestroyChn(IVS_CHN);
        IMP_IVS_DestroyGroup(IVS_GRP);
        return -1;
    }

    log_info("motion detection started (sensitivity=%d)", g_cfg.motion_sensitivity);
    return 0;
}

void motion_exit(void)
{
    if (!g_cfg.motion_enabled || !g_running) return;
    g_running = 0;
    pthread_join(g_thread, NULL);
    IMP_IVS_StopRecvPic(IVS_CHN);
    IMP_IVS_UnRegisterChn(IVS_CHN);
    IMP_IVS_DestroyChn(IVS_CHN);
    IMP_IVS_DestroyGroup(IVS_GRP);
    log_info("motion detection stopped");
}

int motion_triggered(void)
{
    int v = g_motion_flag;
    g_motion_flag = 0;
    return v;
}
