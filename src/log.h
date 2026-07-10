#ifndef TIMPS_LOG_H
#define TIMPS_LOG_H

#include <stdio.h>
#include <time.h>

/* Log levels */
#define LOG_LEVEL_ERROR  0
#define LOG_LEVEL_WARN   1
#define LOG_LEVEL_INFO   2
#define LOG_LEVEL_DEBUG  3

extern int g_log_level;

static inline const char *log_ts(void) {
    static char buf[24];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(buf, sizeof(buf), "%H:%M:%S", tm);
    return buf;
}

#define log_error(fmt, ...) do { \
    if (g_log_level >= LOG_LEVEL_ERROR) \
        fprintf(stderr, "[%s] ERR  " fmt "\n", log_ts(), ##__VA_ARGS__); \
} while (0)

#define log_warn(fmt, ...) do { \
    if (g_log_level >= LOG_LEVEL_WARN) \
        fprintf(stderr, "[%s] WARN " fmt "\n", log_ts(), ##__VA_ARGS__); \
} while (0)

#define log_info(fmt, ...) do { \
    if (g_log_level >= LOG_LEVEL_INFO) \
        fprintf(stdout, "[%s] INFO " fmt "\n", log_ts(), ##__VA_ARGS__); \
} while (0)

#define log_debug(fmt, ...) do { \
    if (g_log_level >= LOG_LEVEL_DEBUG) \
        fprintf(stdout, "[%s] DBG  " fmt "\n", log_ts(), ##__VA_ARGS__); \
} while (0)

#endif /* TIMPS_LOG_H */
