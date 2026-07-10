#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>

static int g_level = LOG_INFO;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *lvl_str[] = { "ERR", "WRN", "INF", "DBG" };

void log_set_level(int level){ g_level = level; }

void log_printf(int level, const char *module, const char *fmt, ...)
{
    if (level > g_level) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    char tbuf[16];
    strftime(tbuf, sizeof tbuf, "%H:%M:%S", &tm);

    pthread_mutex_lock(&g_lock);
    fprintf(stderr, "%s.%03ld [%s] %-12s ", tbuf, ts.tv_nsec/1000000,
            lvl_str[level & 3], module ? module : "");
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    pthread_mutex_unlock(&g_lock);
}
