/* log.h - tiny leveled logger, no dependencies */
#ifndef MS_LOG_H
#define MS_LOG_H

enum { LOG_ERROR=0, LOG_WARN=1, LOG_INFO=2, LOG_DEBUG=3 };

void log_set_level(int level);
/* enable/disable syslog output (on by default so `logread` shows timps) */
void log_set_syslog(int on);
/* Raise these modules to DEBUG without raising the global level. Names, not a
 * bitmask: a mask in a config file is a magic number that silently means
 * something else the day a module is added, and nothing warns. A comma list of
 * MOD strings ("DAYNIGHT,MAIN") is self-describing and a typo is detectable.
 * Empty or NULL clears it. Case-insensitive. */
void log_set_debug_modules(const char *csv);
void log_printf(int level, const char *module, const char *fmt, ...)
    __attribute__((format(printf,3,4)));

#define LOGE(mod, ...) log_printf(LOG_ERROR, mod, __VA_ARGS__)
#define LOGW(mod, ...) log_printf(LOG_WARN,  mod, __VA_ARGS__)
#define LOGI(mod, ...) log_printf(LOG_INFO,  mod, __VA_ARGS__)
#define LOGD(mod, ...) log_printf(LOG_DEBUG, mod, __VA_ARGS__)

#endif
