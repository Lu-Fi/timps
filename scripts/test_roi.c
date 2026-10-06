/* test_roi.c - host-only test for the encoder ROI / ISP FrontCrop validation
 * (src/hal/roi_geom.h) and, built per SoC/stack with -DRT_EXPECT_*, the tier
 * matrix of src/roi_caps.h.
 * Build/run: make test-roi */
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include "roi_caps.h"
#include "hal/roi_geom.h"

static int failures, checks;
#define CK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void t_layout(void)
{
    /* the vendor IMPEncoderROICfg / IMPISPFrontCrop layouts on MIPS o32 and
     * the host alike; hal_ingenic.c asserts the same against the headers */
    CK(offsetof(ms_roi_api, en) == 4 && offsetof(ms_roi_api, rel) == 5,
       "bool flags at 4/5");
    CK(offsetof(ms_roi_api, qp) == 8, "qp at 8");
    CK(offsetof(ms_roi_api, x0) == 12 && offsetof(ms_roi_api, y1) == 24, "rect 12..27");
    CK(sizeof(ms_roi_api) == 28, "ROI cfg 28 bytes, got %zu", sizeof(ms_roi_api));
    CK(offsetof(ms_fcrop_api, top) == 4 && offsetof(ms_fcrop_api, left) == 8,
       "top before left");
    CK(sizeof(ms_fcrop_api) == 20, "FrontCrop 20 bytes");
}

/* the p0/p1 trap: the rect carries corners, never a width */
static void t_corners(void)
{
    ms_roi_api a;
    CK(roi_geom(32, 16, 64, 32, 1920, 1080, &a) == GEOM_OK, "aligned rect is OK");
    CK(a.x0 == 32 && a.y0 == 16, "p0 = x,y");
    CK(a.x1 == 95 && a.y1 == 47, "p1 = x+w-1,y+h-1 (got %d,%d)", a.x1, a.y1);
    CK(a.x1 != 64 && a.y1 != 32, "p1 is not w,h");

    CK(roi_geom(100, 50, 200, 100, 1920, 1080, &a) == GEOM_ADJUSTED, "unaligned adjusts");
    CK(a.x0 == 96 && a.y0 == 48 && a.x1 == 303 && a.y1 == 159,
       "outward to macroblocks: %d,%d-%d,%d", a.x0, a.y0, a.x1, a.y1);

    CK(roi_geom(1900, 1000, 100, 200, 1920, 1080, &a) == GEOM_ADJUSTED, "edge clamps");
    CK(a.x1 == 1919 && a.y1 == 1087, "1080p: last MB row ends at 1087 (got %d)", a.y1);

    CK(roi_geom(-8, -8, 40, 40, 640, 360, &a) == GEOM_ADJUSTED, "negative start clamps");
    CK(a.x0 == 0 && a.y0 == 0 && a.x1 == 31 && a.y1 == 31, "0..31");

    CK(roi_geom(0, 0, 0, 16, 640, 360, &a) == GEOM_REJECT, "w=0 refused");
    CK(roi_geom(640, 0, 16, 16, 640, 360, &a) == GEOM_REJECT, "right of the picture");
    CK(roi_geom(-100, 0, 50, 16, 640, 360, &a) == GEOM_REJECT, "left of the picture");
    CK(roi_geom(0, 0, 16, 16, 0, 360, &a) == GEOM_REJECT, "no picture");
}

static void t_corners_sweep(void)
{
    static const int dims[][2] = { {100, 60}, {640, 360}, {1920, 1080}, {48, 16} };
    int bad = 0;
    for (size_t d = 0; d < sizeof dims / sizeof dims[0]; d++) {
        int fw = dims[d][0], fh = dims[d][1];
        int gw = (fw + 15) & ~15, gh = (fh + 15) & ~15;
        for (int x = -20; x < fw + 20; x += 3)
            for (int w = 0; w < fw + 40; w += 7) {
                int y = x % 37, h = w / 2 + 1;
                ms_roi_api a;
                int r = roi_geom(x, y, w, h, fw, fh, &a);
                int cx = x < 0 ? 0 : x, ex = x + w > fw ? fw : x + w;
                int cy = y < 0 ? 0 : y, ey = y + h > fh ? fh : y + h;
                if (r == GEOM_REJECT) {
                    if (w > 0 && cx < ex && cy < ey) bad++;
                    continue;
                }
                if (a.x0 % 16 || a.y0 % 16 || (a.x1 + 1) % 16 || (a.y1 + 1) % 16 ||
                    a.x0 > cx || a.y0 > cy || a.x1 < ex - 1 || a.y1 < ey - 1 ||
                    a.x1 > gw - 1 || a.y1 > gh - 1 || a.x0 < 0 || a.y0 < 0)
                    bad++;
            }
    }
    CK(bad == 0, "%d sweep cases out of the macroblock grid or not covering", bad);
}

static void t_qp(void)
{
    int q;
    CK(roi_qp(0, 51, &q) == GEOM_OK && q == 51, "abs 51");
    CK(roi_qp(0, 60, &q) == GEOM_ADJUSTED && q == 51, "abs 60 -> 51");
    CK(roi_qp(0, -1, &q) == GEOM_ADJUSTED && q == 0, "abs -1 -> 0");
    CK(roi_qp(1, -5, &q) == GEOM_OK && q == -5, "delta -5");
    CK(roi_qp(1, -40, &q) == GEOM_ADJUSTED && q == -26, "delta -40 -> -26");
    CK(roi_qp(1, 30, &q) == GEOM_ADJUSTED && q == 25, "delta 30 -> 25");
}

static void t_build(void)
{
    ms_roi_api a;
    memset(&a, 0x55, sizeof a);
    CK(roi_build(3, 0, 1, -5, 0, 0, 64, 64, 640, 360, &a) == GEOM_OK, "disabled");
    CK(a.idx == 3 && !a.en && a.x1 == 0 && a.qp == 0, "disable entry for slot 3");
    CK(roi_build(1, 1, 1, -60, 8, 8, 32, 32, 640, 360, &a) == GEOM_ADJUSTED, "clamped build");
    CK(a.en && a.rel && a.qp == -26 && a.x0 == 0 && a.x1 == 47, "qp + rect clamped");
    CK(roi_build(2, 1, 0, 20, 700, 0, 32, 32, 640, 360, &a) == GEOM_REJECT, "outside");
    CK(!a.en && a.idx == 2, "rejected region becomes a disable");
}

static void t_state(void)
{
    CK(roi_stream_state(1, 0, 0) == ROI_ST_OK, "ok");
    CK(roi_stream_state(0, 0, 0) == ROI_ST_OFF, "stream off");
    CK(roi_stream_state(1, 1, 0) == ROI_ST_H265, "h265 refused");
    CK(roi_stream_state(1, 0, 90) == ROI_ST_ROTATED, "rotated refused");
    CK(!strcmp(roi_state_name(ROI_ST_ROTATED), "rotated"), "name");
}

/* FrontCrop: x->left, y->top (the API order is top,left) */
static void t_fcrop(void)
{
    ms_fcrop_api c;
    CK(fcrop_fit(100, 50, 640, 360, 1920, 1080, &c) == GEOM_OK, "plain window");
    CK(c.en && c.left == 100 && c.top == 50, "left=x top=y (got left %u top %u)", c.left, c.top);
    CK(c.width == 640 && c.height == 360, "size");
    CK(fcrop_fit(101, 51, 641, 361, 1920, 1080, &c) == GEOM_ADJUSTED, "odd adjusts");
    CK(c.left == 100 && c.top == 50 && c.width == 640 && c.height == 360, "evened");
    CK(fcrop_fit(1800, 900, 640, 360, 1920, 1080, &c) == GEOM_ADJUSTED, "moved inside");
    CK(c.left == 1280 && c.top == 720, "moved, not shrunk");
    CK(fcrop_fit(-10, 0, 4000, 4000, 1920, 1080, &c) == GEOM_ADJUSTED, "oversize");
    CK(c.left == 0 && c.top == 0 && c.width == 1920 && c.height == 1080, "full sensor");
    CK(fcrop_fit(0, 0, 63, 200, 1920, 1080, &c) == GEOM_REJECT, "below 64 refused");
    CK(fcrop_fit(0, 0, 640, 360, 0, 0, &c) == GEOM_REJECT, "no sensor size");
    int bad = 0;
    for (int x = -50; x < 2000; x += 37)
        for (int w = 64; w < 2100; w += 41) {
            int h = w * 9 / 16 + 64, y = x / 2;
            if (fcrop_fit(x, y, w, h, 1920, 1080, &c) == GEOM_REJECT) { bad++; continue; }
            if ((c.left | c.top | c.width | c.height) & 1 || c.width < FCROP_MIN ||
                c.height < FCROP_MIN || c.left + c.width > 1920 || c.top + c.height > 1080)
                bad++;
        }
    CK(bad == 0, "%d fcrop sweep cases odd, too small or outside", bad);
    fcrop_off(1921, 1080, &c);
    CK(!c.en && c.width == 1920 && c.height == 1080 && !c.left && !c.top, "off = full even window");
}

int main(void)
{
    t_layout();
    t_corners();
    t_corners_sweep();
    t_qp();
    t_build();
    t_state();
    t_fcrop();
#ifdef RT_EXPECT_ROI
    CK(ROI_TIER == RT_EXPECT_ROI, "ROI_TIER %s, want %s",
       cap_tier_name(ROI_TIER), cap_tier_name(RT_EXPECT_ROI));
    CK(FCROP_TIER == RT_EXPECT_FCROP, "FCROP_TIER %s, want %s",
       cap_tier_name(FCROP_TIER), cap_tier_name(RT_EXPECT_FCROP));
    CK(FCROP_LIVE == (RT_EXPECT_FCROP != CAP_TIER_UNSUPPORTED), "FCROP_LIVE %d", FCROP_LIVE);
    if (failures) printf("test_roi %-7s FAILED\n", RT_LABEL);
#else
    printf("test_roi: %d/%d checks passed\n", checks - failures, checks);
#endif
    return failures ? 1 : 0;
}
