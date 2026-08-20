/* srt.h - optional SRT output: serves one video stream (+AAC audio) as
 * MPEG-TS over SRT, as a listener or a caller. Only built with USE_SRT. */
#ifndef MS_SRT_H
#define MS_SRT_H
#include "config.h"

void srt_start(const ms_config *cfg);   /* no-op unless USE_SRT + srt.enabled */
void srt_stop(void);

/* last sampled link stats (the periodic "stats:" log line feeds this), for
 * the /control status block. -1 / t_us==0 = never measured. */
typedef struct {
    int       caller;      /* 1 = caller mode */
    int       connected;   /* receivers currently being served */
    int64_t   t_us;        /* ms_now_us() of the sample, 0 = none yet */
    double    rtt_ms, bw_mbps, rate_mbps;
    long long sent, retrans, loss, drop;
} ms_srt_stats;
void srt_get_stats(ms_srt_stats *out);

#endif
