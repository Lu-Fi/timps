#include "frame.h"
#include <stdlib.h>
#include <string.h>

ms_pkt *pkt_new(const uint8_t *data, size_t len, int64_t pts_us, int keyframe, int media)
{
    ms_pkt *p = (ms_pkt*)malloc(sizeof(*p));
    if (!p) return NULL;
    p->data = (uint8_t*)malloc(len ? len : 1);
    if (!p->data) { free(p); return NULL; }
    if (len) memcpy(p->data, data, len);
    p->len = len;
    p->pts_us = pts_us;
    p->keyframe = keyframe;
    p->media = media;
    p->_ref = 1;
    return p;
}

ms_pkt *pkt_ref(ms_pkt *p)
{
    if (p) __sync_add_and_fetch(&p->_ref, 1);
    return p;
}

void pkt_unref(ms_pkt *p)
{
    if (!p) return;
    if (__sync_sub_and_fetch(&p->_ref, 1) == 0) {
        free(p->data);
        free(p);
    }
}
