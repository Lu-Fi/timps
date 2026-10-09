#ifndef MS_ROI_GEOM_H
#define MS_ROI_GEOM_H
/* Pure validation for encoder ROI and ISP FrontCrop: libimp clips neither, so
 * timps aligns, clamps and refuses before anything reaches the SDK. Covered
 * by scripts/test_roi.c (make test-roi). */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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
/* Family A (SetChnROI): -26 produced invalid H.264 on T20 (the jump back at
 * the region edge is mb_qp_delta +26, H.264 allows -26..+25; Intel VA-API
 * refused it, -25 decoded). */
#define ROI_QP_DELTA_MIN (-25)
#define ROI_QP_DELTA_MAX 25
/* Family B (SetChnRoiAttr, OpenIMP T31): the API takes -26..25 but the
 * library limits the effective delta to -25..25 (OpenIMP maintainer). */
#define ROI_B_DELTA_MIN  (-25)
#define ROI_B_DELTA_MAX  25
#define ROI_B_WINS       10
/* an absolute QP stays absolute only while every picture QP the stream can
 * have is within this distance; beyond it the edge delta is invalid H.264 */
#define ROI_ABS_WINDOW   25
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

static inline int roi_clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* The picture QP the encoder can use: lo..hi (FixQP: lo == hi == videoN.qp;
 * other modes: the RC window min_qp..max_qp), ref is what an absolute QP is
 * turned into a delta against. dmin/dmax: the family's delta range. */
typedef struct { int lo, hi, ref, dmin, dmax; } ms_roi_qpctx;

/* qp_mode + qp -> what is sent: *rel_out 0 absolute, 1 delta. An absolute QP
 * further than ROI_ABS_WINDOW from any possible picture QP becomes the delta
 * abs - ref, clamped. */
static inline int roi_qp_map(int rel, int qp, const ms_roi_qpctx *c,
                             int *rel_out, int *out)
{
    if (rel) {
        *rel_out = 1;
        *out = roi_clampi(qp, c->dmin, c->dmax);
        return *out != qp ? GEOM_ADJUSTED : GEOM_OK;
    }
    int a = roi_clampi(qp, ROI_QP_ABS_MIN, ROI_QP_ABS_MAX);
    if (a - c->lo <= ROI_ABS_WINDOW && c->hi - a <= ROI_ABS_WINDOW) {
        *rel_out = 0;
        *out = a;
        return a != qp ? GEOM_ADJUSTED : GEOM_OK;
    }
    *rel_out = 1;
    *out = roi_clampi(a - c->ref, c->dmin, c->dmax);
    return GEOM_ADJUSTED;
}

/* One SetChnROI argument. en=0 or a rejected rect yields a disable entry
 * (en=0, zero rect) for the same slot. */
static inline int roi_build(int idx, int en, int rel, int qp,
                            int x, int y, int w, int h, int fw, int fh,
                            const ms_roi_qpctx *qc, ms_roi_api *o)
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
    int r, q = roi_qp_map(rel, qp, qc, &r, &a.qp);
    a.rel = (bool)r;
    a.en = 1;
    *o = a;
    return (g == GEOM_ADJUSTED || q == GEOM_ADJUSTED) ? GEOM_ADJUSTED : GEOM_OK;
}

/* IMPEncoderRoiWin (OpenIMP T31, vendor T40/T41 1.2.6): rect in x/y/w/h
 * pixels; mode 0 delta, 1 fixed QP */
typedef struct {
    bool     enable;
    uint32_t x, y, w, h;
    int      mode;
    int8_t   qp;
} ms_roiwin_api;
typedef struct { ms_roiwin_api win[ROI_B_WINS]; } ms_roiattr_api;
enum { ROI_B_DELTA = 0, ROI_B_FIXED = 1 };

/* family B rect: the family-A macroblock corners turned into x/y/w/h, the end
 * clipped to the picture (libimp refuses x+w > width, e.g. the 1088th row) */
static inline int roi_geom_b(int x, int y, int w, int h, int fw, int fh,
                             ms_roiwin_api *o)
{
    ms_roi_api a;
    int g = roi_geom(x, y, w, h, fw, fh, &a);
    if (g == GEOM_REJECT) return g;
    int ex = a.x1 + 1 > fw ? fw : a.x1 + 1, ey = a.y1 + 1 > fh ? fh : a.y1 + 1;
    o->x = (uint32_t)a.x0; o->y = (uint32_t)a.y0;
    o->w = (uint32_t)(ex - a.x0); o->h = (uint32_t)(ey - a.y0);
    return g;
}

static inline int roi_build_b(int en, int rel, int qp, int x, int y, int w, int h,
                              int fw, int fh, const ms_roi_qpctx *qc, ms_roiwin_api *o)
{
    ms_roiwin_api b;
    memset(&b, 0, sizeof b);
    if (!en) { *o = b; return GEOM_OK; }
    int g = roi_geom_b(x, y, w, h, fw, fh, &b);
    if (g == GEOM_REJECT) { memset(&b, 0, sizeof b); *o = b; return g; }
    int r, q8, q = roi_qp_map(rel, qp, qc, &r, &q8);
    b.mode = r ? ROI_B_DELTA : ROI_B_FIXED;
    b.qp = (int8_t)q8;
    b.enable = 1;
    *o = b;
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

/* T31/T23 (MSCA): the driver takes a window only if, per axis,
 * win * (100 + p) >= out * 100 for every channel output; p is the
 * open-tx-isp fcrop_upscale_pct (absent: 0, no upscaling). */
static inline void fcrop_min_msca(int out_w, int out_h, int p, int *mw, int *mh)
{
    if (p < 0) p = 0;
    long w = ((long)out_w * 100 + 100 + p - 1) / (100 + p);
    long h = ((long)out_h * 100 + 100 + p - 1) / (100 + p);
    w = (w + 1) & ~1L; h = (h + 1) & ~1L;
    *mw = w < FCROP_MIN ? FCROP_MIN : (int)w;
    *mh = h < FCROP_MIN ? FCROP_MIN : (int)h;
}

/* zoom against the full sensor, x100, capped by the smallest window */
static inline int fcrop_zoom_x100(int sw, int sh, int mw, int mh)
{
    if (mw <= 0 || mh <= 0 || sw <= 0 || sh <= 0) return 100;
    long zx = (long)sw * 100 / mw, zy = (long)sh * 100 / mh;
    long z = zx < zy ? zx : zy;
    return z < 100 ? 100 : (int)z;
}

/* T10/T20/T21: only a downscaled stream shows the window, and only a window
 * at least its own size (no upscaling) */
static inline int fcrop_ds_zooms(int ww, int wh, int vw, int vh, int sw, int sh)
{
    if (vw >= sw && vh >= sh) return 0;
    return ww >= vw && wh >= vh;
}

/* T10/T20/T21 refused a window: 1 when a stream could show it but none of
 * those runs (an idle stream's channel counts as unable), so the next
 * stream start applies it; 0 = a real refusal. */
static inline int fcrop_ds_waits(int ww, int wh, int n, const int *vw, const int *vh,
                                 const int *on, int sw, int sh)
{
    int fit = 0;
    for (int i = 0; i < n; i++) {
        if (!fcrop_ds_zooms(ww, wh, vw[i], vh[i], sw, sh)) continue;
        if (on[i]) return 0;
        fit = 1;
    }
    return fit;
}

/* the disable call: full sensor window, en=0 */
static inline void fcrop_off(int sw, int sh, ms_fcrop_api *o)
{
    o->en = 0; o->top = 0; o->left = 0;
    o->width = (unsigned)(sw > 0 ? sw : 0) & ~1u;
    o->height = (unsigned)(sh > 0 ? sh : 0) & ~1u;
}

#endif
