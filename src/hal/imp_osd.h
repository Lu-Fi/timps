/* imp_osd.h - Ingenic IMP_OSD text/logo overlays, one group per video stream */
#ifndef MS_IMP_OSD_H
#define MS_IMP_OSD_H
#include "../config.h"

/* Create an OSD group for video stream 'stream_idx' at the given output size,
 * with all enabled osd.items placed by their x/y. Returns the OSD group
 * number to bind into the pipeline (fs -> osd -> enc), or -1 if OSD is
 * disabled / on failure. */
int  imp_osd_setup(const ms_config *cfg, int stream_idx, int width, int height);

/* Start the shared 1 Hz text updater (call once, after all streams set up). */
void imp_osd_start_updater(void);

/* Tear down all OSD groups and stop the updater. */
void imp_osd_stop(void);

#endif
