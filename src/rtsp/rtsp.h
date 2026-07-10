/* rtsp.h - minimal RTSP 1.0 server (no live555) */
#ifndef MS_RTSP_H
#define MS_RTSP_H
#include "../config.h"

typedef struct rtsp_server rtsp_server;

rtsp_server *rtsp_start(const ms_config *cfg);
void         rtsp_stop(rtsp_server *s);

#endif
