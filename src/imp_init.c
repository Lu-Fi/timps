#include <stdio.h>
#include <string.h>
#include <imp/imp_common.h>
#include <imp/imp_system.h>
#include <imp/imp_framesource.h>
#include <imp/imp_isp.h>
#include "imp_init.h"
#include "config.h"
#include "log.h"

#define FS_GRP  0

/* FrameSource channel index matches stream index */
#define FS_CHN(n) (n)

/*
 * Correct SDK initialisation sequence (all SoC generations):
 *   IMP_ISP_Open → IMP_ISP_AddSensor → IMP_ISP_EnableSensor
 *   → IMP_System_Init → IMP_ISP_EnableTuning
 *   → IMP_FrameSource_CreateChn (for each enabled stream)
 *   → IMP_System_Bind (FS → ENC per stream)
 *   → IMP_FrameSource_EnableChn
 */
int imp_init(void)
{
    int           ret;
    IMPSensorInfo sensor_info;
    IMPFSChnAttr  fs_attr;
    IMPCell       src_cell, dst_cell;
    int           sensor_added = 0;
    int           system_inited = 0;

    /* ------------------------------------------------------------------ */
    /* 1. ISP open                                                          */
    /* ------------------------------------------------------------------ */
    ret = IMP_ISP_Open();
    if (ret) {
        log_error("IMP_ISP_Open failed: %d", ret);
        return ret;
    }

    /* ------------------------------------------------------------------ */
    /* 2. Add sensor                                                        */
    /* ------------------------------------------------------------------ */
    memset(&sensor_info, 0, sizeof(sensor_info));
    strncpy(sensor_info.name, g_cfg.sensor, sizeof(sensor_info.name) - 1);
    sensor_info.cbus_type          = TX_SENSOR_CONTROL_INTERFACE_I2C;
    sensor_info.i2c.i2c_adapter_id = 0;
    sensor_info.i2c.addr           = 0x37;
    /* driver name == sensor name */
    strncpy(sensor_info.i2c.type, g_cfg.sensor,
            sizeof(sensor_info.i2c.type) - 1);

    ret = IMP_ISP_AddSensor(&sensor_info);
    if (ret) {
        log_error("IMP_ISP_AddSensor failed: %d", ret);
        goto err_isp;
    }
    sensor_added = 1;

    /* Optional ISP bin path */
    if (g_cfg.sensor_bin[0])
        IMP_ISP_SetDefaultBinPath(g_cfg.sensor_bin);

    /* ------------------------------------------------------------------ */
    /* 3. Enable sensor                                                     */
    /* ------------------------------------------------------------------ */
    ret = IMP_ISP_EnableSensor();
    if (ret) {
        log_error("IMP_ISP_EnableSensor failed: %d", ret);
        goto err_sensor;
    }

    /* ------------------------------------------------------------------ */
    /* 4. System init (must come after EnableSensor)                       */
    /* ------------------------------------------------------------------ */
    ret = IMP_System_Init();
    if (ret) {
        log_error("IMP_System_Init failed: %d", ret);
        goto err_sensor;
    }
    system_inited = 1;

    /* ISP tuning (non-fatal) */
    if (IMP_ISP_EnableTuning())
        log_warn("IMP_ISP_EnableTuning failed (non-fatal)");

    /* ------------------------------------------------------------------ */
    /* 5. FrameSource channels                                              */
    /* ------------------------------------------------------------------ */
    for (int i = 0; i < 2; i++) {
        StreamCfg *s = &g_cfg.stream[i];
        if (!s->enabled) continue;

        memset(&fs_attr, 0, sizeof(fs_attr));
        fs_attr.pixFmt        = PIX_FMT_NV12;
        fs_attr.outFrmRateNum = s->fps;
        fs_attr.outFrmRateDen = 1;
        fs_attr.nrVBs         = 2;
        fs_attr.type          = FS_PHY_CHANNEL;
        fs_attr.picWidth      = s->width;
        fs_attr.picHeight     = s->height;
        /* Sub-channel uses scaler to downscale from the main frame */
        fs_attr.scaler.enable = (i == 1) ? 1 : 0;

        ret = IMP_FrameSource_CreateChn(FS_CHN(i), &fs_attr);
        if (ret) {
            log_error("IMP_FrameSource_CreateChn(%d) failed: %d", i, ret);
            goto err_fs;
        }

        ret = IMP_FrameSource_SetChnAttr(FS_CHN(i), &fs_attr);
        if (ret) {
            log_error("IMP_FrameSource_SetChnAttr(%d) failed: %d", i, ret);
            IMP_FrameSource_DestroyChn(FS_CHN(i));
            goto err_fs;
        }

        /* Bind FrameSource channel → Encoder group */
        src_cell.deviceID = DEV_ID_FS;
        src_cell.groupID  = FS_GRP;
        src_cell.outputID = FS_CHN(i);

        dst_cell.deviceID = DEV_ID_ENC;
        dst_cell.groupID  = i;
        dst_cell.outputID = 0;

        ret = IMP_System_Bind(&src_cell, &dst_cell);
        if (ret) {
            log_error("IMP_System_Bind(FS%d→ENC%d) failed: %d", i, i, ret);
            IMP_FrameSource_DestroyChn(FS_CHN(i));
            goto err_fs;
        }

        ret = IMP_FrameSource_EnableChn(FS_CHN(i));
        if (ret) {
            log_error("IMP_FrameSource_EnableChn(%d) failed: %d", i, ret);
            IMP_System_UnBind(&src_cell, &dst_cell);
            IMP_FrameSource_DestroyChn(FS_CHN(i));
            goto err_fs;
        }

        log_info("FrameSource[%d]: %dx%d@%dfps", i, s->width, s->height, s->fps);
    }

    return 0;

err_fs:
    /* Disable and destroy any FS channels already set up */
    for (int i = 1; i >= 0; i--) {
        if (!g_cfg.stream[i].enabled) continue;
        IMP_FrameSource_DisableChn(FS_CHN(i));
        src_cell.deviceID = DEV_ID_FS;  src_cell.groupID = FS_GRP;
        src_cell.outputID = FS_CHN(i);
        dst_cell.deviceID = DEV_ID_ENC; dst_cell.groupID = i;
        dst_cell.outputID = 0;
        IMP_System_UnBind(&src_cell, &dst_cell);
        IMP_FrameSource_DestroyChn(FS_CHN(i));
    }
    if (system_inited) IMP_System_Exit();
err_sensor:
    IMP_ISP_DisableSensor();
    if (sensor_added) IMP_ISP_DelSensor(&sensor_info);
err_isp:
    IMP_ISP_Close();
    return ret;
}

void imp_exit(void)
{
    IMPCell src_cell, dst_cell;

    for (int i = 1; i >= 0; i--) {
        if (!g_cfg.stream[i].enabled) continue;

        IMP_FrameSource_DisableChn(FS_CHN(i));

        src_cell.deviceID = DEV_ID_FS;
        src_cell.groupID  = FS_GRP;
        src_cell.outputID = FS_CHN(i);
        dst_cell.deviceID = DEV_ID_ENC;
        dst_cell.groupID  = i;
        dst_cell.outputID = 0;
        IMP_System_UnBind(&src_cell, &dst_cell);

        IMP_FrameSource_DestroyChn(FS_CHN(i));
    }

    IMP_ISP_DisableTuning();
    IMP_System_Exit();
    IMP_ISP_DisableSensor();
    IMP_ISP_Close();
}
