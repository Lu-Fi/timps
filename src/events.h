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
#include "hal/imp_motion.h"    /* ms_motion_status (queued snapshot type) */

/* producers: wake all /events subscribers ("some observable state changed") */
void events_notify(void);

/* producer (imp_motion.c): enqueue a changed grid snapshot AND wake all
 * subscribers (implies events_notify; O(cells) copy, never blocks) */
void events_motion_push(const ms_motion_status *st);

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
#endif

#endif
