/* events.h - change notification for the /events SSE push stream (httpd.c).
 *
 * Producers (imp_motion.c grid results, daynight.c measurements, control.c
 * settings writes) call events_notify() whenever observable state changed;
 * every /events connection blocked in events_wait() wakes up, re-reads the
 * status sources and pushes only what actually differs from what it last
 * sent. One global generation counter + condvar for the LEVEL sources
 * (daynight, stats, settings) - no allocation, a missed increment cannot
 * lose data there because subscribers re-read the CURRENT state.
 *
 * MOTION is different: two grid transitions that land while a subscriber is
 * off the condvar (writing to a slow socket, in crecv, scheduled out)
 * collapse into one wakeup, the resampled level equals the last-sent one and
 * the dedup drops BOTH - a real motion burst emits nothing. So grid changes
 * additionally go through a small bounded snapshot ring: the producer pushes
 * a copy of every changed grid (events_motion_push, drops the oldest when a
 * very slow client laps it), each subscriber drains it with a private cursor
 * (events_motion_pop) and emits EVERY snapshot - lossless for rising edges,
 * never blocking the producer.
 *
 * events_notify()/events_motion_push() are safe to call from ANY build
 * permutation: without -DUSE_CONTROL they are no-op stubs (there is no
 * /events endpoint to wake). */
#ifndef MS_EVENTS_H
#define MS_EVENTS_H
#include <stdint.h>
#include "hal/imp_motion.h"    /* ms_motion_status (queued snapshot type) */

/* ---- daynight decision history -----------------------------------------
 * A second, much longer ring than the motion one: the WebUI tuning graph
 * (tool-sensor-data) needs a SERIES over hours, and it cannot collect one
 * itself - the WebUI is plain HTTP on a LAN IP, so navigator.serviceWorker is
 * undefined and a hidden or closed tab collects nothing. So the daemon keeps
 * the series and the page just pages through it with a cursor.
 *
 * Same cursor discipline as the motion ring: the seq counts every sample ever
 * pushed and is never reset, so a client cursor stays meaningful across a
 * resize; a client further behind than the ring retains is told it lapped and
 * refetches from the oldest sample instead of silently getting a hole. */
#define DN_HIST_PERIOD_S 10      /* decimation: one retained sample per N s */

typedef struct {
    uint32_t t;         /* CLOCK_MONOTONIC seconds at push (filled by push) */
    float    gain;      /* total_gain, IMP [24.8] linear; -1 unknown */
    float    exposure;  /* the exposure index the decision runs on; -1 unknown */
    int16_t  luma;      /* ae_luma 0..255, rounded; -1 unknown */
    int8_t   bright;    /* brightness 0..100 %, rounded; -1 unknown */
    int8_t   mode;      /* 0 day, 1 night, -1 unknown */
} ms_dn_sample;         /* 16 B; 48 h at DN_HIST_PERIOD_S = 270 KiB */

/* producer (daynight.c, one call per DN_HIST_PERIOD_S): append a sample.
 * retain_s (daynight.history_s, already clamped to 0..48 h by config.c) rides
 * along so the ring follows a live reconfiguration without events.c having to
 * reach into g_cfg; 0 frees the ring. Never blocks, never fails loudly - a
 * failed allocation just leaves the previous ring in place. */
void events_dn_hist_push(ms_dn_sample *s, int retain_s);

/* producers: wake all /events subscribers ("some observable state changed") */
void events_notify(void);

/* producer (imp_motion.c): enqueue a changed grid snapshot AND wake all
 * subscribers (implies events_notify; O(cells) copy, never blocks) */
void events_motion_push(const ms_motion_status *st);

/* producer (control.c): record a changed setting for the /events "config"
 * push AND wake all subscribers (implies events_notify).
 *
 * Unlike motion, a setting is LEVEL state, not an edge: while a subscriber
 * is off the condvar, three updates to the same key coalesce into one -
 * that is exactly right (a dragged slider need not replay every intermediate
 * value, only the latest). So this is a small fixed table keyed by name,
 * not a ring: pushing an already-present key overwrites it in place. Only
 * when a truly new, distinct key needs a slot and the table is full does an
 * still-undelivered *different* key get evicted - subscribers behind that
 * point are told to resync (one GET /control) instead of silently losing
 * that key's update. */
void events_config_push(const char *key, const char *val);

#ifdef USE_CONTROL
/* subscribers (httpd.c /events connections): read the current generation,
 * then block until it moves past last_gen or timeout_ms elapsed (whichever
 * first - the timeout doubles as the SSE keepalive/stats tick). Returns the
 * current generation to pass into the next wait. */
unsigned events_generation(void);
unsigned events_wait(unsigned last_gen, int timeout_ms);

/* motion snapshot ring, per-subscriber cursor: seed the cursor with
 * events_motion_cursor() on connect (only FUTURE snapshots are delivered;
 * the initial level comes from motion_get_status), then drain with
 * events_motion_pop until it returns 0. A lapped cursor silently skips to
 * the oldest retained snapshot (bounded memory, drop-oldest overflow). */
unsigned events_motion_cursor(void);
int events_motion_pop(unsigned *cursor, ms_motion_status *out);

/* config table, per-subscriber cursor: seed with events_config_cursor() on
 * connect (only FUTURE changes are delivered - the initial values come from
 * the client's own GET /control on page load), then each wakeup first call
 * events_config_resync() (returns 1 exactly once if this subscriber lapped
 * an eviction - emit one "please refetch /control" event), then drain
 * events_config_pop() until it returns 0. */
unsigned events_config_cursor(void);
int events_config_resync(unsigned *cursor);
int events_config_pop(unsigned *cursor, char *key, int keycap, char *val, int valcap);

/* daynight history readers (control.c, serving GET /control?dn_history=1).
 *
 * events_dn_hist_stat() reports the window: *head is the seq the next push
 * will use (i.e. one past the newest sample), *oldest the lowest seq still
 * retained, *cap the ring's current capacity in samples and *t_now the same
 * monotonic second the samples are stamped with, so a client can convert a
 * sample's t into wall time against its own clock and stay correct across an
 * NTP step. Any pointer may be NULL.
 *
 * events_dn_hist_range() copies up to max samples starting at *from into out,
 * advances *from past them and returns the count. A *from below the retained
 * window is snapped up to the oldest retained sample and *lapped (may be
 * NULL) is set to 1 - the client then knows its series has a hole and should
 * refetch rather than splice. */
void events_dn_hist_stat(unsigned *head, unsigned *oldest, int *cap,
                         uint32_t *t_now);
int  events_dn_hist_range(unsigned *from, int max, ms_dn_sample *out,
                          int *lapped);
#endif

#endif
