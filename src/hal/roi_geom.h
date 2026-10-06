#ifndef MS_ROI_GEOM_H
#define MS_ROI_GEOM_H
/* Pure validation for encoder ROI and ISP FrontCrop: libimp clips neither, so
 * timps aligns, clamps and refuses before anything reaches the SDK. Covered
 * by scripts/test_roi.c (make test-roi). */
#include <stdbool.h>
#include <stdint.h>

/* IMPEncoderROICfg as libimp reads it. The rect is IMPRect = two INCLUSIVE
 * corners p0/p1, not x/y/w/h (OpenIMP names the same four ints x,y,width,
 * height but reads them as corners). */
typedef struct {
    uint32_t idx;            /* 0..7 */
    bool     en;
    bool     rel;            /* 0 absolute QP, 1 delta */
    int      qp;
    int      x0, y0, x1, y1;
} ms_roi_api;

/* IMPISPFrontCrop: top comes BEFORE left */
typedef struct {
    bool     en;
    unsigned top, left, width, height;
} ms_fcrop_api;

#define ROI_ALIGN        16      /* corners are truncated to macroblocks */
#define ROI_HW_SLOTS     8
#define ROI_QP_ABS_MIN   0
#define ROI_QP_ABS_MAX   51
/* delta behaviour at the limits is unverified; this range also fits the
 * T40/T41 RoiAttr API (-26..25) */
#define ROI_QP_DELTA_MIN (-26)
#define ROI_QP_DELTA_MAX 25
#define FCROP_MIN        64      /* OpenIMP T23 refuses smaller; max zoom undocumented */

enum { GEOM_REJECT = -1, GEOM_OK = 0, GEOM_ADJUSTED = 1 };

/* x/y/w/h in the encoded picture (fw x fh) -> macroblock-aligned inclusive
 * corners. Rounded outwards so the request stays covered; p1 is the last
 * pixel of the last macroblock, which stays inside the macroblock grid
 * whether libimp treats p1 as inclusive or exclusive (at worst one MB short). */
static inline int roi_geom(int x, int y, int w, int h, int fw, int fh, ms_roi_api *o)
{
    if (w <= 0 || h <= 0 || fw <= 0 || fh <= 0) return GEOM_REJECT;
    long ex = (long)x + w, ey = (long)y + h;
    long cx = x < 0 ? 0 : x, cy = y < 0 ? 0 : y;
    if (ex > fw) ex = fw;
    if (ey > fh) ey = fh;
    if (cx >= ex || cy >= ey) return GEOM_REJECT;
    o->x0 = (int)(cx & ~(long)(ROI_ALIGN - 1));
    o->y0 = (int)(cy & ~(long)(ROI_ALIGN - 1));
    o->x1 = (int)(((ex + ROI_ALIGN - 1) & ~(long)(ROI_ALIGN - 1)) - 1);
    o->y1 = (int)(((ey + ROI_ALIGN - 1) & ~(long)(ROI_ALIGN - 1)) - 1);
    return (o->x0 != x || o->y0 != y || o->x1 != (long)x + w - 1 ||
            o->y1 != (long)y + h - 1) ? GEOM_ADJUSTED : GEOM_OK;
}

static inline int roi_qp(int rel, int qp, int *out)
{
    int lo = rel ? ROI_QP_DELTA_MIN : ROI_QP_ABS_MIN;
    int hi = rel ? ROI_QP_DELTA_MAX : ROI_QP_ABS_MAX;
    *out = qp < lo ? lo : qp > hi ? hi : qp;
    return *out != qp ? GEOM_ADJUSTED : GEOM_OK;
}

/* One SetChnROI argument. en=0 or a rejected rect yields a disable entry
 * (en=0, zero rect) for the same slot. */
static inline int roi_build(int idx, int en, int rel, int qp,
                            int x, int y, int w, int h, int fw, int fh,
                            ms_roi_api *o)
{
    ms_roi_api a = {0};
    a.idx = (uint32_t)idx;
    a.rel = rel ? 1 : 0;
    if (!en) { *o = a; return GEOM_OK; }
    int g = roi_geom(x, y, w, h, fw, fh, &a);
    if (g == GEOM_REJECT) {
        a.x0 = a.y0 = a.x1 = a.y1 = 0;
        *o = a;
        return GEOM_REJECT;
    }
    int q = roi_qp(rel, qp, &a.qp);
    a.en = 1;
    *o = a;
    return (g == GEOM_ADJUSTED || q == GEOM_ADJUSTED) ? GEOM_ADJUSTED : GEOM_OK;
}

/* per-stream ROI state as GET /control reports it */
enum { ROI_ST_OK = 0, ROI_ST_OFF, ROI_ST_H265, ROI_ST_ROTATED, ROI_ST_UNSUPPORTED };

static inline const char *roi_state_name(int s)
{
    static const char *const n[] = { "ok", "off", "h265", "rotated", "unsupported" };
    return (s >= 0 && s <= ROI_ST_UNSUPPORTED) ? n[s] : "unsupported";
}

/* Rotated streams are refused: the encoder input is the rotated frame and
 * how SetChnROI addresses it is unverified on every SoC that has ROI. */
static inline int roi_stream_state(int enabled, int h265, int rotation)
{
    if (!enabled) return ROI_ST_OFF;
    if (h265) return ROI_ST_H265;
    if (rotation) return ROI_ST_ROTATED;
    return ROI_ST_OK;
}

/* x/y/w/h in sensor pixels (sw x sh) -> FrontCrop window: even values,
 * moved (not shrunk) inside the sensor, at least FCROP_MIN on each side. */
static inline int fcrop_fit(int x, int y, int w, int h, int sw, int sh,
                            ms_fcrop_api *o)
{
    if (sw < FCROP_MIN || sh < FCROP_MIN || w < FCROP_MIN || h < FCROP_MIN)
        return GEOM_REJECT;
    int x0 = x, y0 = y, w0 = w, h0 = h;
    if (w > sw) w = sw;
    if (h > sh) h = sh;
    w &= ~1; h &= ~1;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > sw - w) x = sw - w;
    if (y > sh - h) y = sh - h;
    x &= ~1; y &= ~1;
    o->en = 1;
    o->left = (unsigned)x; o->top = (unsigned)y;
    o->width = (unsigned)w; o->height = (unsigned)h;
    return (x != x0 || y != y0 || w != w0 || h != h0) ? GEOM_ADJUSTED : GEOM_OK;
}

/* the disable call: full sensor window, en=0 */
static inline void fcrop_off(int sw, int sh, ms_fcrop_api *o)
{
    o->en = 0; o->top = 0; o->left = 0;
    o->width = (unsigned)(sw > 0 ? sw : 0) & ~1u;
    o->height = (unsigned)(sh > 0 ? sh : 0) & ~1u;
}

#endif
