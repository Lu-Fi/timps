/* imp_motion.h - Ingenic IMP_IVS motion detection (SoC feature) */
#ifndef MS_IMP_MOTION_H
#define MS_IMP_MOTION_H
#include "../config.h"
int  imp_motion_start(const ms_config *cfg);
void imp_motion_stop(void);
#endif
