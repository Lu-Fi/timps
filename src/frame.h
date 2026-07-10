/* frame.h - reference counted encoded packets (zero-copy fan-out) */
#ifndef MS_FRAME_H
#define MS_FRAME_H
#include <stdint.h>
#include <stddef.h>

enum ms_media { MS_MEDIA_VIDEO=0, MS_MEDIA_AUDIO=1, MS_MEDIA_JPEG=2 };

typedef struct ms_pkt {
    uint8_t     *data;      /* video: Annex-B access unit; audio: raw frame */
    size_t       len;
    int64_t      pts_us;    /* presentation time, microseconds */
    int          keyframe;  /* video IDR */
    int          media;     /* enum ms_media */
    int          _ref;
} ms_pkt;

ms_pkt *pkt_new(const uint8_t *data, size_t len, int64_t pts_us, int keyframe, int media);
ms_pkt *pkt_ref(ms_pkt *p);
void    pkt_unref(ms_pkt *p);

#endif
