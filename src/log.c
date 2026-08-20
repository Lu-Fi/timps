/* log.c - tiny leveled logger. Writes to stderr (visible when timpsd runs in
 * the foreground / the host sim) and, by default, to the system log so the
 * messages show up in `logread` on the device (the init script backgrounds
 * timpsd, so its stderr is otherwise discarded). */

/* syslog.h defines LOG_ERR/LOG_WARNING/LOG_INFO/LOG_DEBUG as macros that
 * collide with our own log.h level enum (LOG_INFO/LOG_DEBUG). Capture the
 * syslog priorities we need first, then drop the clashing macros before
 * pulling in log.h so our enum is the only definition. The syslog()/openlog()
 * prototypes stay available. */
#include <syslog.h>
static const int SYS_PRI[4] = { LOG_ERR, LOG_WARNING, LOG_INFO, LOG_DEBUG };
#undef LOG_ERR
#undef LOG_WARNING
#undef LOG_INFO
#undef LOG_DEBUG

#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>        /* strcasecmp */
#include <time.h>
#include <pthread.h>

static int g_level = LOG_INFO;
static int g_syslog = 1;          /* on by default -> logread shows timps */
static int g_syslog_open = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *lvl_str[] = { "ERR", "WRN", "INF", "DBG" };

/* Per-module DEBUG. The device's syslog ring is 64 KB and recycles in minutes
 * under load, so raising the global level to see one subsystem loses the very
 * lines it was raised for. */
#define LOG_DBGMOD_MAX 8
#define LOG_DBGMOD_LEN 16
static char g_dbgmod[LOG_DBGMOD_MAX][LOG_DBGMOD_LEN];
static int  g_dbgmod_n = 0;

/* Read without the lock: the list only changes on an explicit config apply,
 * and the worst case is that one line is judged against a half-written entry.
 * Locking the hot path of every log call to protect a diagnostic switch would
 * be the wrong trade. */
static int mod_is_debug(const char *module)
{
    if (!module || g_dbgmod_n <= 0) return 0;
    for (int i = 0; i < g_dbgmod_n; i++)
        if (!strcasecmp(g_dbgmod[i], module)) return 1;
    return 0;
}

void log_set_debug_modules(const char *csv)
{
    pthread_mutex_lock(&g_lock);
    g_dbgmod_n = 0;
    for (const char *p = csv; p && *p && g_dbgmod_n < LOG_DBGMOD_MAX; ) {
        while (*p == ',' || *p == ' ') p++;
        const char *e = p;
        while (*e && *e != ',') e++;
        size_t n = (size_t)(e - p);
        while (n && (p[n-1] == ' ')) n--;
        if (n && n < LOG_DBGMOD_LEN) {
            memcpy(g_dbgmod[g_dbgmod_n], p, n);
            g_dbgmod[g_dbgmod_n][n] = 0;
            g_dbgmod_n++;
        }
        p = *e ? e + 1 : e;
    }
    pthread_mutex_unlock(&g_lock);
}

void log_set_level(int level){ g_level = level; }

void log_set_syslog(int on){ g_syslog = on; }

void log_printf(int level, const char *module, const char *fmt, ...)
{
    if (level > g_level && !(level == LOG_DEBUG && mod_is_debug(module)))
        return;

    char msg[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    char tbuf[16];
    strftime(tbuf, sizeof tbuf, "%H:%M:%S", &tm);

    pthread_mutex_lock(&g_lock);
    fprintf(stderr, "%s.%03ld [%s] %-12s %s\n", tbuf, ts.tv_nsec/1000000,
            lvl_str[level & 3], module ? module : "", msg);
    if (g_syslog){
        if (!g_syslog_open){ openlog("timpsd", LOG_PID, LOG_DAEMON); g_syslog_open = 1; }
        syslog(SYS_PRI[level & 3], "[%s] %s", module ? module : "", msg);
    }
    pthread_mutex_unlock(&g_lock);
}
