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

/* Unlocked read on the hot path (locking every log call for a diagnostic
 * switch would be the wrong trade). The writer clears the published count
 * before rewriting the names and republishes it (release) only after they
 * are complete, so a scan starting mid-update sees the empty list rather
 * than torn names; a scan already in flight judges its one line against
 * the old list. Same __atomic style as hub.c's drop counters. */
static int mod_is_debug(const char *module)
{
    int n = __atomic_load_n(&g_dbgmod_n, __ATOMIC_ACQUIRE);
    if (!module || n <= 0) return 0;
    for (int i = 0; i < n; i++)
        if (!strcasecmp(g_dbgmod[i], module)) return 1;
    return 0;
}

void log_set_debug_modules(const char *csv)
{
    /* Parse into locals first: the rejection warnings at the end must run
     * after g_lock is released - log_printf() takes g_lock itself, so
     * warning from inside the locked region would deadlock. */
    char names[LOG_DBGMOD_MAX][LOG_DBGMOD_LEN];
    int k = 0, too_long = 0, too_many = 0;
    for (const char *p = csv; p && *p; ) {
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != ',') e++;
        size_t n = (size_t)(e - p);
        while (n && (p[n-1] == ' ' || p[n-1] == '\t')) n--;
        if (n >= LOG_DBGMOD_LEN)           too_long++;
        else if (n && k >= LOG_DBGMOD_MAX) too_many++;
        else if (n) {
            memcpy(names[k], p, n);
            names[k][n] = 0;
            k++;
        }
        p = *e ? e + 1 : e;
    }

    pthread_mutex_lock(&g_lock);
    /* clear-then-fill-then-publish: see mod_is_debug() */
    __atomic_store_n(&g_dbgmod_n, 0, __ATOMIC_RELEASE);
    if (k) memcpy(g_dbgmod, names, (size_t)k * LOG_DBGMOD_LEN);
    __atomic_store_n(&g_dbgmod_n, k, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_lock);

    /* A silently dropped name makes a typo'd list indistinguishable from a
     * working one - the failure the names-not-bitmask design (log.h) exists
     * to catch. */
    if (too_long)
        LOGW("LOG", "debug_modules: %d name(s) over %d chars ignored",
             too_long, LOG_DBGMOD_LEN - 1);
    if (too_many)
        LOGW("LOG", "debug_modules: at most %d names, %d ignored",
             LOG_DBGMOD_MAX, too_many);
}

void log_set_level(int level){ g_level = level; }

void log_set_syslog(int on){ g_syslog = on; }

/* ---- last-error registry (log.h) ---- */
#define LERR_SLOTS 16
#define LERR_MSG   120
static struct lerr {
    char     mod[16];
    char     msg[LERR_MSG];
    long     t;          /* unix time of the latest capture */
    unsigned count;      /* WARN+ lines from this module since start; 0 = free */
    int      level;
} g_lerr[LERR_SLOTS];

static void lerr_note(int level, const char *module, const char *msg, long t)
{
    /* caller holds g_lock. Slot per module; table full -> evict the oldest. */
    struct lerr *s = NULL, *fr = NULL, *old = NULL;
    for (int i = 0; i < LERR_SLOTS; i++){
        struct lerr *e = &g_lerr[i];
        if (!e->count){ if (!fr) fr = e; continue; }
        if (!strcmp(e->mod, module)){ s = e; break; }
        if (!old || e->t < old->t) old = e;
    }
    if (!s && !(s = fr)) s = old;
    if (strcmp(s->mod, module)){
        snprintf(s->mod, sizeof s->mod, "%s", module);
        s->count = 0;
    }
    s->count++;
    s->level = level;
    s->t = t;
    /* sanitize while copying so the JSON accessor can embed it verbatim */
    int j = 0;
    for (const char *p = msg; *p && j < LERR_MSG-1; p++){
        unsigned char ch = (unsigned char)*p;
        s->msg[j++] = ch=='"' ? '\'' : ch=='\\' ? '/' : ch < 0x20 ? ' ' : (char)ch;
    }
    s->msg[j] = 0;
}

int log_last_errors_json(char *buf, size_t cap)
{
    size_t o = 0;
    #define APP(...) do { \
        int _n = snprintf(o<cap?buf+o:buf, o<cap?cap-o:0, __VA_ARGS__); \
        if (_n>0) o += (size_t)_n; \
    } while (0)
    long now = (long)time(NULL);
    APP("{");
    pthread_mutex_lock(&g_lock);
    int first = 1;
    for (int i = 0; i < LERR_SLOTS; i++){
        const struct lerr *e = &g_lerr[i];
        if (!e->count) continue;
        APP("%s\"%s\":{\"level\":\"%s\",\"age_s\":%ld,\"count\":%u,\"msg\":\"%s\"}",
            first?"":",", e->mod, lvl_str[e->level & 3],
            now - e->t, e->count, e->msg);
        first = 0;
    }
    pthread_mutex_unlock(&g_lock);
    APP("}");
    #undef APP
    return (int)o;
}

void log_printf(int level, const char *module, const char *fmt, ...)
{
    /* a module in debug_modules is raised to DEBUG outright - ALL its
     * levels pass, not only LOG_DEBUG (log.h promises "raise to DEBUG";
     * DEBUG-visible-but-INFO-hidden at loglevel<2 surprised everyone) */
    if (level > g_level && !mod_is_debug(module))
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
    if (level <= LOG_WARN)
        lerr_note(level, module ? module : "", msg, (long)ts.tv_sec);
    fprintf(stderr, "%s.%03ld [%s] %-12s %s\n", tbuf, ts.tv_nsec/1000000,
            lvl_str[level & 3], module ? module : "", msg);
    if (g_syslog){
        if (!g_syslog_open){ openlog("timpsd", LOG_PID, LOG_DAEMON); g_syslog_open = 1; }
        syslog(SYS_PRI[level & 3], "[%s] %s", module ? module : "", msg);
    }
    pthread_mutex_unlock(&g_lock);
}
