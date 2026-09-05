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
    size_t          keep_cap;   /* small/large split point: a buffer whose data
                                 * grew beyond this never enters the freelist,
                                 * so a transient large IDR can't pin
                                 * max_free*IDR-size idle */
    /* R-01 (REVIEW_2026-08-07 P-01, left open pending a soak measurement):
     * ONE over-keep_cap buffer is retained here instead of being freed, so the
     * recurring large frames - every IDR, and every 1080p JPEG, all of which
     * are above keep_cap by construction - recycle instead of paying a
     * malloc+free of 100-800 KB each. Deliberately a single slot and not a
     * bigger keep_cap: raising keep_cap would let all max_free freelist entries
     * ratchet to IDR size (4x the idle footprint the 96 KB ceiling was chosen
     * to defend on 32 MB SoCs), and it is handed out ONLY for requests that are
     * themselves over keep_cap (see pkt_pool_get). That second rule is what
     * keeps cap ~= len on every published packet: fanqueue's FQ_MAX_BYTES /
     * record.c's RING_MAX_BYTES budgets account ->len, so a small frame carried
     * in an IDR-sized buffer would be memory the backstops cannot see - the
     * exact hazard hal_ingenic.c's sw-rotate publish path is documented to
     * avoid. pkt_pool_trim() releases the slot when a producer goes idle. */
    ms_pkt         *big;
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
/* Release the retained over-keep_cap buffer (see ->big), returning the pool to
 * the max_free*keep_cap idle ceiling. For producers that have just stopped
 * their encoder: an idle source must not sit on an IDR-sized buffer. Safe to
 * call at any time and any number of times - it only ever frees a buffer that
 * is already idle in the pool, never one still referenced by a subscriber. */
void    pkt_pool_trim(pkt_pool *p);

#endif
