/* log.h - tiny leveled logger, no dependencies */
#ifndef MS_LOG_H
#define MS_LOG_H

enum { LOG_ERROR=0, LOG_WARN=1, LOG_INFO=2, LOG_DEBUG=3 };

void log_set_level(int level);
/* enable/disable syslog output (on by default so `logread` shows timps) */
void log_set_syslog(int on);
void log_printf(int level, const char *module, const char *fmt, ...)
    __attribute__((format(printf,3,4)));

#define LOGE(mod, ...) log_printf(LOG_ERROR, mod, __VA_ARGS__)
#define LOGW(mod, ...) log_printf(LOG_WARN,  mod, __VA_ARGS__)
#define LOGI(mod, ...) log_printf(LOG_INFO,  mod, __VA_ARGS__)
#define LOGD(mod, ...) log_printf(LOG_DEBUG, mod, __VA_ARGS__)

#endif
