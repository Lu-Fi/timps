/* record.h - local recording to SD as fragmented-MP4 segments (raptor-RMR
 * style, reusing the fmp4 muxer). Continuous or motion-triggered. */
#ifndef MS_RECORD_H
#define MS_RECORD_H
#include "config.h"

/* Start / stop the recorder thread. Safe to call when disabled (the thread
 * idles until recording is wanted). record_stop() is idempotent. */
void record_start(const ms_config *cfg);
void record_stop(void);

#ifdef USE_CONTROL
typedef struct {
    int       available;     /* built with recording support */
    int       enabled;       /* configured on (record.enabled) */
    int       recording;     /* a segment is being written right now */
    int       channel;       /* which video stream is being recorded */
    int       mode;          /* 0 = continuous, 1 = motion-triggered */
    long long bytes;         /* bytes written to the current segment */
    long long free_mb;       /* free space on the record directory */
    char      file[160];     /* current segment path ("" = idle) */
} ms_record_status;

void record_get_status(ms_record_status *st);
/* manual override from /control: on=1 forces recording, on=0 forces it off,
 * on<0 returns to config-driven (auto). Returns 0. */
int  record_set_active(int on);
/* on-demand clip: capture ~seconds of the record channel to `path` (must be
 * under /tmp/) as an fMP4 file, forward from the next keyframe. Blocks until
 * done. Returns 0 on success. Used by send2 for Telegram/motion video clips. */
int  record_clip(const char *path, int seconds);
#endif

#endif
