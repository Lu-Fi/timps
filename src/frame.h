/* frame.h - reference counted encoded packets (zero-copy fan-out) */
#ifndef MS_FRAME_H
#define MS_FRAME_H
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

enum ms_media { MS_MEDIA_VIDEO=0, MS_MEDIA_AUDIO=1, MS_MEDIA_JPEG=2 };

struct pkt_pool;

typedef struct ms_pkt {
    uint8_t     *data;      /* video: Annex-B access unit; audio: raw frame */
    size_t       len;       /* payload length in use */
    size_t       cap;       /* allocated capacity of data (>= len) */
    int64_t      pts_us;    /* presentation time, microseconds */
    /* CLOCK_MONOTONIC (ms_now_us) instant this packet was handed to the hub,
     * i.e. "the encoder produced it". ONLY stamped while the opt-in send-
     * pipeline trace is enabled (see trace.h); 0 otherwise, so the normal
     * build does not pay a clock_gettime per published frame. Consumers use
     * it purely for the `age` figure in a trace line - nothing functional
     * reads it, so a 0 here is always safe. Deliberately NOT pts_us: pts_us
     * is the sanitized/slewed CAPTURE timestamp, which is exactly the thing
     * a stall investigation must not have to trust. */
    int64_t      enq_us;
    int          keyframe;  /* video IDR */
    int          media;     /* enum ms_media */
    int          _ref;
    /* P-01: when non-NULL, the LAST pkt_unref() returns this (still-allocated)
     * buffer to the pool for reuse instead of free()ing it, so the hot publish
     * path does no per-frame malloc/free (and no second full-frame copy).
     * NULL for pkt_new()-built packets, which free() exactly as before -
     * every existing sink (fanqueue, record ring, rtsp/srt/httpd) is unchanged. */
    struct pkt_pool *pool;
    struct ms_pkt   *pnext; /* pool freelist link (valid only while idle in pool) */
} ms_pkt;

/* P-01: small fixed-size per-source recycling pool. Producers assemble the
 * access unit DIRECTLY into a pooled buffer (pkt_pool_get) and hand it over
 * via hub_publish_take(), eliminating the second full-frame copy pkt_new()
 * used to make. Thread-safe: the single producer borrows on its thread while
 * the last subscriber returns (pkt_unref) on another - both under ->lock. */
typedef struct pkt_pool {
    pthread_mutex_t lock;
    ms_pkt         *freelist;
    int             nfree;
    int             max_free;   /* cap on idle buffers retained */
    size_t          keep_cap;   /* a buffer whose data grew beyond this is
                                 * freed on return, not pooled, so a transient
                                 * large IDR can't pin max_free*IDR-size idle */
} pkt_pool;

/* Copy constructor (unchanged contract): mallocs + copies the buffer. Still
 * used by the audio publish paths and anywhere a borrowed buffer is captured
 * by copy. pool==NULL, cap==len. */
ms_pkt *pkt_new(const uint8_t *data, size_t len, int64_t pts_us, int keyframe, int media);
ms_pkt *pkt_ref(ms_pkt *p);
void    pkt_unref(ms_pkt *p);

/* Pool lifecycle. A pool MUST outlive every packet it ever handed out (a slow
 * subscriber can hold a ref long after the producer stopped), so pools are
 * process-lifetime statics owned by the hub - never freed. */
void    pkt_pool_init(pkt_pool *p, int max_free, size_t keep_cap);
/* Borrow a writable packet whose data capacity is >= cap (grown if needed).
 * Returns with _ref==1, len==0, pool set; NULL on OOM. The caller fills
 * data[0..len) and sets ->len, then publishes with hub_publish_take(). */
ms_pkt *pkt_pool_get(pkt_pool *p, size_t cap);

#endif
