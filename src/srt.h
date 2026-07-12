/* srt.h - optional SRT output: serves one video stream (+AAC audio) as
 * MPEG-TS over SRT (listener mode). Only built with USE_SRT (libsrt). */
#ifndef MS_SRT_H
#define MS_SRT_H
#include "config.h"

void srt_start(const ms_config *cfg);   /* no-op unless USE_SRT + srt.enabled */
void srt_stop(void);

#endif
