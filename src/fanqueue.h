/* fanqueue.h - bounded single-consumer queue of packet pointers.
 * Producer (HAL thread) never blocks: on overflow (slot cap or byte budget,
 * FQ_MAX_BYTES) the oldest packet is dropped. This guarantees a slow client
 * can never stall the encoder nor pin unbounded payload bytes. */
#ifndef MS_FANQUEUE_H
#define MS_FANQUEUE_H
#include "frame.h"
#include <pthread.h>

typedef struct {
    ms_pkt        **slots;
    int             cap;
    int             head, tail, count;
    size_t          bytes;         /* sum of slots[*]->len currently queued
                                    * (byte budget, see FQ_MAX_BYTES) */
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    int             closed;
    unsigned        dropped;
    int             dropped_key;   /* a video keyframe was dropped on overflow */
    int             dropped_any;   /* ANY packet was dropped on overflow (incl.
                                    * P-frames), read-and-clear separately from
                                    * dropped_key so consumers can self-heal a
                                    * mid-GOP P-frame gap too */
} fanqueue;

int   fanqueue_init(fanqueue *q, int cap);
void  fanqueue_free(fanqueue *q);
/* push: takes ownership of a ref (does not add its own). returns 1 if a
 * packet was dropped to make room. */
int   fanqueue_push(fanqueue *q, ms_pkt *p);
/* pop: blocks until a packet is available or queue closed. returns NULL on
 * close. Caller must pkt_unref() the result. */
ms_pkt *fanqueue_pop(fanqueue *q, int timeout_ms);
void  fanqueue_close(fanqueue *q);
/* read-and-clear the dropped-keyframe flag. The consumer (which knows its
 * hub source) should call hub_request_idr() when this returns nonzero, so
 * clients don't decode garbage until the next natural GOP boundary. */
int   fanqueue_take_dropped_key(fanqueue *q);
/* read-and-clear the "any packet dropped" flag (keyframe OR P-frame). Mirrors
 * fanqueue_take_dropped_key() but fires on any overflow eviction. A dropped
 * P-frame silently corrupts the rest of the GOP for a frame-by-frame consumer
 * just like a dropped keyframe does, but leaves no keyframe to trip
 * dropped_key - so a consumer that decodes/displays every frame (RTSP) should
 * also request an IDR here, RATE-LIMITED, since IDR requests are global to the
 * shared encoder and a chronically slow client must not spike the bitrate for
 * every other subscriber. */
int   fanqueue_take_dropped(fanqueue *q);
/* snapshot the current backlog under the lock: queued slot count, capacity
 * and queued payload bytes (any of the out-pointers may be NULL). Lets a
 * consumer detect its OWN sustained backlog (this client can't keep up) and
 * react per-client, without touching the producer or other subscribers. */
void  fanqueue_depth(fanqueue *q, int *count, int *cap, size_t *bytes);

#endif
