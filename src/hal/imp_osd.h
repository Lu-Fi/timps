/* imp_osd.h - Ingenic IMP_OSD text/logo overlays, one group per video stream */
#ifndef MS_IMP_OSD_H
#define MS_IMP_OSD_H
#include "../config.h"

/* Create an OSD group for video stream 'stream_idx' at the given output size,
 * with all enabled items of THAT stream's set (osd.items[stream_idx][..])
 * placed by their x/y. Returns the OSD group number to bind into the pipeline
 * (fs -> osd -> enc), or -1 if OSD is disabled / on failure. */
int  imp_osd_setup(const ms_config *cfg, int stream_idx, int width, int height);

/* Start the shared 1 Hz text updater (call once, after all streams set up). */
void imp_osd_start_updater(void);

/* Tear down all OSD groups and stop the updater. */
void imp_osd_stop(void);

#ifdef USE_CONTROL
/* Live control: re-apply cfg->osd.items[stream][item] (already updated in
 * g_cfg) on that stream - enable/disable, position, text, color, size,
 * transparency, outline, logo reload. stream < 0 = all streams (legacy shared
 * osdN.* keys). Items that were disabled at startup have no region and can
 * only be enabled after a restart. Thread-safe vs. the updater. */
void imp_osd_apply(int stream, int item);
#endif

#endif
