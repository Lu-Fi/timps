/* fanqueue.h - bounded single-consumer queue of packet pointers.
 * Producer (HAL thread) never blocks: on overflow the oldest packet is
 * dropped. This guarantees a slow client can never stall the encoder. */
#ifndef MS_FANQUEUE_H
#define MS_FANQUEUE_H
#include "frame.h"
#include <pthread.h>

typedef struct {
    ms_pkt        **slots;
    int             cap;
    int             head, tail, count;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    int             closed;
    unsigned        dropped;
    int             dropped_key;   /* a video keyframe was dropped on overflow */
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

#endif
