/* events.h - change notification for the /events SSE push stream (httpd.c).
 *
 * Producers (imp_motion.c grid results, daynight.c measurements, control.c
 * settings writes) call events_notify() whenever observable state changed;
 * every /events connection blocked in events_wait() wakes up, re-reads the
 * status sources and pushes only what actually differs from what it last
 * sent. One global generation counter + condvar - no queues, no allocation,
 * and a missed increment can never lose data (subscribers always re-read the
 * CURRENT state; the counter only says "something changed").
 *
 * events_notify() is safe to call from ANY build permutation: without
 * -DUSE_CONTROL it is a no-op stub (there is no /events endpoint to wake). */
#ifndef MS_EVENTS_H
#define MS_EVENTS_H

/* producers: wake all /events subscribers ("some observable state changed") */
void events_notify(void);

#ifdef USE_CONTROL
/* subscribers (httpd.c /events connections): read the current generation,
 * then block until it moves past last_gen or timeout_ms elapsed (whichever
 * first - the timeout doubles as the SSE keepalive/stats tick). Returns the
 * current generation to pass into the next wait. */
unsigned events_generation(void);
unsigned events_wait(unsigned last_gen, int timeout_ms);
#endif

#endif
