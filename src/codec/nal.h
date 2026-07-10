/* nal.h - Annex-B NAL unit iteration (no allocation) */
#ifndef MS_NAL_H
#define MS_NAL_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    const uint8_t *au;
    size_t         au_len;
    size_t         pos;
} nal_iter;

typedef struct {
    const uint8_t *data;   /* points at NAL, start code stripped */
    size_t         len;
} nal_unit;

void nal_iter_init(nal_iter *it, const uint8_t *au, size_t len);
/* returns 1 and fills *out for each NAL; 0 when done */
int  nal_iter_next(nal_iter *it, nal_unit *out);

/* codec-specific type extraction */
static inline int h264_nal_type(const uint8_t *n){ return n[0] & 0x1F; }
static inline int h265_nal_type(const uint8_t *n){ return (n[0] >> 1) & 0x3F; }

#endif
