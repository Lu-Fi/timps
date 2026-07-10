#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <imp/imp_osd.h>
#include <imp/imp_common.h>
#include "osd.h"
#include "config.h"
#include "stream.h"
#include "log.h"

#define OSD_REGION_W  480
#define OSD_REGION_H   32

static IMPOSDRgnHandle g_handles[2] = {-1, -1};
static time_t g_start_time;

/* Expand the OSD label template into dst (size dstlen). */
static void expand_label(char *dst, size_t dstlen)
{
    char hostname[64] = "timps";
    gethostname(hostname, sizeof(hostname) - 1);

    time_t now    = time(NULL);
    struct tm *tm = localtime(&now);

    /* First pass: strftime */
    char tmp[256];
    strftime(tmp, sizeof(tmp), g_cfg.osd_label, tm);

    /* Second pass: custom placeholders */
    char *p    = tmp;
    char *end  = dst + dstlen - 1;
    char *out  = dst;

    while (*p && out < end) {
        if (p[0] == '%' && p[1] == '{') {
            char *close = strchr(p + 2, '}');
            if (close) {
                char ph[32] = {0};
                int phlen = (int)(close - (p + 2));
                if (phlen < (int)sizeof(ph)) {
                    memcpy(ph, p + 2, phlen);
                    char val[64] = "";
                    if (!strcmp(ph, "host"))
                        snprintf(val, sizeof(val), "%s", hostname);
                    else if (!strcmp(ph, "uptime")) {
                        long up = (long)(now - g_start_time);
                        snprintf(val, sizeof(val), "%ld:%02ld:%02ld",
                                 up/3600, (up%3600)/60, up%60);
                    } else if (!strcmp(ph, "viewers"))
                        snprintf(val, sizeof(val), "%d",
                                 stream_viewer_count(0) + stream_viewer_count(1));
                    else if (!strcmp(ph, "fps"))
                        snprintf(val, sizeof(val), "%d", g_cfg.stream[0].fps);
                    else if (!strcmp(ph, "bitrate"))
                        snprintf(val, sizeof(val), "%d", g_cfg.stream[0].bitrate);
                    else
                        snprintf(val, sizeof(val), "???");

                    int vl = (int)strlen(val);
                    if (out + vl < end) {
                        memcpy(out, val, vl);
                        out += vl;
                    }
                    p = close + 1;
                    continue;
                }
            }
        }
        *out++ = *p++;
    }
    *out = '\0';
}

int osd_init(void)
{
    if (!g_cfg.osd_enabled) return 0;

    g_start_time = time(NULL);

    for (int i = 0; i < 2; i++) {
        if (!g_cfg.stream[i].enabled) continue;

        /* Create OSD group (same index as encoder group) */
        int ret = IMP_OSD_CreateGroup(i);
        if (ret) { log_warn("IMP_OSD_CreateGroup(%d): %d", i, ret); continue; }

        /* Create a region */
        IMPOSDRgnAttr attr;
        memset(&attr, 0, sizeof(attr));
        attr.type               = OSD_REG_PIC;
        attr.rect.p0.x          = g_cfg.osd_x;
        attr.rect.p0.y          = g_cfg.osd_y;
        attr.rect.p1.x          = g_cfg.osd_x + OSD_REGION_W;
        attr.rect.p1.y          = g_cfg.osd_y + OSD_REGION_H;
        attr.fmt                = PIX_FMT_BGRA;
        attr.data.picData.virAddr = calloc(OSD_REGION_W * OSD_REGION_H * 4, 1);
        attr.data.picData.phyAddr = 0;

        g_handles[i] = IMP_OSD_CreateRgn(&attr);
        if (g_handles[i] < 0) {
            log_warn("IMP_OSD_CreateRgn(%d) failed: %d", i, g_handles[i]);
            continue;
        }

        IMPOSDGrpRgnAttr grp;
        memset(&grp, 0, sizeof(grp));
        grp.show         = 1;
        grp.layer        = 3;
        grp.scalex       = 1;
        grp.scaley       = 1;
        grp.gAlphaEn     = 1;
        grp.fgAlhpa      = 0xff;
        grp.bgAlhpa      = 0x00;

        IMP_OSD_RegisterRgn(g_handles[i], i, &grp);
        IMP_OSD_Start(i);

        log_info("OSD group %d created", i);
    }
    return 0;
}

void osd_update(void)
{
    if (!g_cfg.osd_enabled) return;

    char label[256];
    expand_label(label, sizeof(label));

    /* We would use a TTF renderer (e.g. FreeType) here to rasterise label
     * into the BGRA bitmap.  Since FreeType is an optional dep, we write
     * only a stub that logs the label.  Integrators can replace this with
     * an actual rasteriser. */
    log_debug("OSD: %s", label);

    for (int i = 0; i < 2; i++) {
        if (g_handles[i] < 0) continue;
        /* In a full implementation, render 'label' with FreeType into the
         * region bitmap and call IMP_OSD_SetRgnAttr() to update. */
    }
}

void osd_exit(void)
{
    for (int i = 0; i < 2; i++) {
        if (g_handles[i] < 0) continue;
        IMP_OSD_Stop(i);
        IMP_OSD_UnRegisterRgn(g_handles[i], i);
        IMP_OSD_DestroyRgn(g_handles[i]);
        IMP_OSD_DestroyGroup(i);
        g_handles[i] = -1;
    }
}
