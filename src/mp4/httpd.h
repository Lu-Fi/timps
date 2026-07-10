/* httpd.h - tiny HTTP server streaming fragmented MP4 for browser preview */
#ifndef MS_HTTPD_H
#define MS_HTTPD_H
#include "../config.h"

typedef struct httpd httpd;
httpd *httpd_start(const ms_config *cfg);
void   httpd_stop(httpd *h);

#endif
