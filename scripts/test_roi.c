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

static const ms_roi_qpctx QA_FIX35 = { 35, 35, 35, ROI_QP_DELTA_MIN, ROI_QP_DELTA_MAX };

static void t_qp(void)
{
    int q, r;
    CK(roi_qp_map(0, 51, &QA_FIX35, &r, &q) == GEOM_OK && !r && q == 51, "abs 51 near 35");
    CK(roi_qp_map(0, 60, &QA_FIX35, &r, &q) == GEOM_ADJUSTED && !r && q == 51, "abs 60 -> 51");
    CK(roi_qp_map(1, -5, &QA_FIX35, &r, &q) == GEOM_OK && r && q == -5, "delta -5");
    CK(roi_qp_map(1, -40, &QA_FIX35, &r, &q) == GEOM_ADJUSTED && q == -25, "delta -40 -> -25");
    CK(roi_qp_map(1, -26, &QA_FIX35, &r, &q) == GEOM_ADJUSTED && q == -25,
       "family A delta -26 -> -25 (invalid H.264 on T20)");
    CK(roi_qp_map(1, 30, &QA_FIX35, &r, &q) == GEOM_ADJUSTED && q == 25, "delta 30 -> 25");
}

/* absolute QP: kept only while |abs - picture QP| <= 25 for every picture QP */
static void t_qp_map(void)
{
    int q, r;
    CK(roi_qp_map(0, 10, &QA_FIX35, &r, &q) == GEOM_OK && !r && q == 10, "35-10 = 25 stays abs");
    CK(roi_qp_map(0, 9, &QA_FIX35, &r, &q) == GEOM_ADJUSTED && r && q == -25, "35-9 = 26 -> delta -25");
    CK(roi_qp_map(0, 0, &QA_FIX35, &r, &q) == GEOM_ADJUSTED && r && q == -25, "abs 0 -> -25");
    ms_roi_qpctx lo = { 20, 20, 20, ROI_QP_DELTA_MIN, ROI_QP_DELTA_MAX };
    CK(roi_qp_map(0, 46, &lo, &r, &q) == GEOM_ADJUSTED && r && q == 25, "20+26 -> delta 25");
    CK(roi_qp_map(0, 45, &lo, &r, &q) == GEOM_OK && !r && q == 45, "20+25 stays abs");
    /* CBR window 15..45: abs 15 can sit 30 below the picture */
    ms_roi_qpctx cbr = { 15, 45, 30, ROI_QP_DELTA_MIN, ROI_QP_DELTA_MAX };
    CK(roi_qp_map(0, 15, &cbr, &r, &q) == GEOM_ADJUSTED && r && q == -15, "cbr abs 15 -> -15");
    CK(roi_qp_map(0, 30, &cbr, &r, &q) == GEOM_OK && !r && q == 30, "cbr abs 30 stays");
    CK(roi_qp_map(0, 20, &cbr, &r, &q) == GEOM_OK && !r, "cbr 20: 45-20 = 25 stays");
    CK(roi_qp_map(0, 19, &cbr, &r, &q) == GEOM_ADJUSTED && r && q == -11, "cbr 19 -> -11");
    ms_roi_qpctx b = { 51, 51, 51, ROI_B_DELTA_MIN, ROI_B_DELTA_MAX };
    CK(roi_qp_map(1, -25, &b, &r, &q) == GEOM_OK && q == -25, "family B keeps -25");
    CK(roi_qp_map(1, -26, &b, &r, &q) == GEOM_ADJUSTED && q == -25, "family B -26 -> -25");
    CK(roi_qp_map(0, 0, &b, &r, &q) == GEOM_ADJUSTED && r && q == -25, "B abs 0 at 51 -> -25");
    CK(roi_qp_map(1, 26, &b, &r, &q) == GEOM_ADJUSTED && q == 25, "family B 26 -> 25");
    /* every output is a legal value of what it claims to be */
    int bad = 0;
    for (int lo2 = 1; lo2 <= 51; lo2 += 5)
        for (int hi2 = lo2; hi2 <= 51; hi2 += 7)
            for (int qp = -60; qp <= 60; qp++)
                for (int m = 0; m < 2; m++) {
                    ms_roi_qpctx c = { lo2, hi2, (lo2 + hi2) / 2, ROI_QP_DELTA_MIN, ROI_QP_DELTA_MAX };
                    roi_qp_map(m, qp, &c, &r, &q);
                    if (r ? (q < ROI_QP_DELTA_MIN || q > ROI_QP_DELTA_MAX)
                          : (q < 0 || q > 51 || q - lo2 > 25 || hi2 - q > 25))
                        bad++;
                    if (m && !r) bad++;
                }
    CK(bad == 0, "%d qp map cases out of range", bad);
}

static void t_build(void)
{
    ms_roi_api a;
    memset(&a, 0x55, sizeof a);
    CK(roi_build(3, 0, 1, -5, 0, 0, 64, 64, 640, 360, &QA_FIX35, &a) == GEOM_OK, "disabled");
    CK(a.idx == 3 && !a.en && a.x1 == 0 && a.qp == 0, "disable entry for slot 3");
    CK(roi_build(1, 1, 1, -60, 8, 8, 32, 32, 640, 360, &QA_FIX35, &a) == GEOM_ADJUSTED,
       "clamped build");
    CK(a.en && a.rel && a.qp == -25 && a.x0 == 0 && a.x1 == 47, "qp + rect clamped");
    CK(roi_build(2, 1, 0, 20, 700, 0, 32, 32, 640, 360, &QA_FIX35, &a) == GEOM_REJECT, "outside");
    CK(!a.en && a.idx == 2, "rejected region becomes a disable");
    CK(roi_build(0, 1, 0, 5, 0, 0, 32, 32, 640, 360, &QA_FIX35, &a) == GEOM_ADJUSTED &&
       a.rel && a.qp == -25, "abs 5 under FixQP 35 goes out as delta -25");
}

static void t_layout_b(void)
{
    /* IMPEncoderRoiWin of OpenIMP origin/claude/t31-roi = vendor T41 1.2.6 */
    CK(offsetof(ms_roiwin_api, x) == 4 && offsetof(ms_roiwin_api, h) == 16, "rect 4..19");
    CK(offsetof(ms_roiwin_api, mode) == 20 && offsetof(ms_roiwin_api, qp) == 24, "mode 20, qp 24");
    CK(sizeof(ms_roiwin_api) == 28, "RoiWin 28 bytes, got %zu", sizeof(ms_roiwin_api));
    CK(sizeof(ms_roiattr_api) == 280, "RoiAttr 280 bytes");
}

/* x,y,w,h for family B, never past the picture */
static void t_geom_b(void)
{
    ms_roiwin_api b;
    CK(roi_geom_b(32, 16, 64, 32, 1920, 1080, &b) == GEOM_OK, "aligned");
    CK(b.x == 32 && b.y == 16 && b.w == 64 && b.h == 32, "x,y,w,h not corners (%u,%u %ux%u)",
       b.x, b.y, b.w, b.h);
    CK(roi_geom_b(100, 50, 200, 100, 1920, 1080, &b) == GEOM_ADJUSTED, "unaligned");
    CK(b.x == 96 && b.y == 48 && b.w == 208 && b.h == 112, "outward (%u,%u %ux%u)", b.x, b.y, b.w, b.h);
    CK(roi_geom_b(1900, 1000, 100, 200, 1920, 1080, &b) == GEOM_ADJUSTED, "edge");
    CK(b.x + b.w == 1920 && b.y + b.h == 1080 && b.y == 992, "ends at 1080, not 1088 (%u+%u)",
       b.y, b.h);
    CK(roi_geom_b(640, 0, 16, 16, 640, 360, &b) == GEOM_REJECT, "outside");
    int bad = 0;
    for (int x = -20; x < 700; x += 7)
        for (int w = 1; w < 700; w += 13) {
            int y = x % 41, h = w / 3 + 1;
            if (roi_geom_b(x, y, w, h, 640, 360, &b) == GEOM_REJECT) continue;
            int ex = x + w > 640 ? 640 : x + w, ey = y + h > 360 ? 360 : y + h;
            if (b.x % 16 || b.y % 16 || !b.w || !b.h || b.x + b.w > 640 || b.y + b.h > 360 ||
                (int)b.x > (x < 0 ? 0 : x) || (int)(b.x + b.w) < ex || (int)(b.y + b.h) < ey)
                bad++;
        }
    CK(bad == 0, "%d family B rects outside the picture or not covering", bad);
}

static void t_build_b(void)
{
    ms_roiwin_api b;
    ms_roi_qpctx c = { 35, 35, 35, ROI_B_DELTA_MIN, ROI_B_DELTA_MAX };
    memset(&b, 0x55, sizeof b);
    CK(roi_build_b(0, 1, -5, 0, 0, 64, 64, 640, 360, &c, &b) == GEOM_OK && !b.enable && !b.w,
       "disabled window is all zero");
    CK(roi_build_b(1, 1, -10, 0, 0, 64, 64, 640, 360, &c, &b) == GEOM_OK && b.enable &&
       b.mode == ROI_B_DELTA && b.qp == -10, "delta -10");
    CK(roi_build_b(1, 0, 20, 0, 0, 64, 64, 640, 360, &c, &b) == GEOM_OK &&
       b.mode == ROI_B_FIXED && b.qp == 20, "abs 20 near 35 stays FIXED_QP");
    CK(roi_build_b(1, 0, 5, 0, 0, 64, 64, 640, 360, &c, &b) == GEOM_ADJUSTED &&
       b.mode == ROI_B_DELTA && b.qp == -25, "abs 5 -> delta -25");
    CK(roi_build_b(1, 1, -5, 9999, 9999, 64, 64, 640, 360, &c, &b) == GEOM_REJECT && !b.enable,
       "outside -> disabled");
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

/* T31/T23 minimum window (open-tx-isp fcrop_upscale_pct), T10/T20/T21 rule */
static void t_fcrop_limits(void)
{
    int w, h;
    fcrop_min_msca(1920, 1080, 0, &w, &h);
    CK(w == 1920 && h == 1080, "p=0: no upscaling");
    fcrop_min_msca(1920, 1080, -1, &w, &h);
    CK(w == 1920 && h == 1080, "unreadable p counts as 0");
    fcrop_min_msca(1920, 1080, 100, &w, &h);
    CK(w == 960 && h == 540, "p=100: 960x540 (got %dx%d)", w, h);
    CK(fcrop_zoom_x100(2560, 1440, w, h) == 266, "Garage 2.67x -> 266");
    fcrop_min_msca(1920, 1080, 75, &w, &h);
    CK(w == 1098 && h == 618, "p=75: ceil 1097.1 -> 1098, 617.1 -> 618 (got %dx%d)", w, h);
    CK((long)w * 175 >= 1920L * 100 && (long)h * 175 >= 1080L * 100, "p=75 passes the driver rule");
    fcrop_min_msca(100, 60, 100, &w, &h);
    CK(w == 64 && h == 64, "never below FCROP_MIN");
    CK(fcrop_zoom_x100(1920, 1080, 1920, 1080) == 100, "no zoom");
    int bad = 0;
    for (int ow = 64; ow <= 2560; ow += 37)
        for (int p = 0; p <= 150; p += 7) {
            fcrop_min_msca(ow, ow / 2 + 1, p, &w, &h);
            if ((long)w * (100 + p) < (long)ow * 100 || (w & 1) || (h & 1) ||
                (long)h * (100 + p) < (long)(ow / 2 + 1) * 100 ||
                (w - 2 >= FCROP_MIN && (long)(w - 2) * (100 + p) >= (long)ow * 100))
                bad++;
        }
    CK(bad == 0, "%d minimum windows wrong or not the smallest even one", bad);
    CK(fcrop_ds_zooms(1000, 562, 640, 360, 1920, 1080), "sub stream zooms");
    CK(!fcrop_ds_zooms(1000, 562, 1280, 720, 1920, 1080), "window smaller than the stream");
    CK(!fcrop_ds_zooms(1920, 1080, 1280, 720, 1280, 720), "stream at sensor size never zooms");
}

int main(void)
{
    t_layout();
    t_corners();
    t_corners_sweep();
    t_qp();
    t_qp_map();
    t_build();
    t_layout_b();
    t_geom_b();
    t_build_b();
    t_state();
    t_fcrop();
    t_fcrop_limits();
#ifdef RT_EXPECT_ROI
    CK(ROI_TIER == RT_EXPECT_ROI, "ROI_TIER %s, want %s",
       cap_tier_name(ROI_TIER), cap_tier_name(RT_EXPECT_ROI));
    CK(FCROP_TIER == RT_EXPECT_FCROP, "FCROP_TIER %s, want %s",
       cap_tier_name(FCROP_TIER), cap_tier_name(RT_EXPECT_FCROP));
    CK(FCROP_LIVE == RT_EXPECT_FLIVE, "FCROP_LIVE %d, want %d", FCROP_LIVE, RT_EXPECT_FLIVE);
    if (failures) printf("test_roi %-7s FAILED\n", RT_LABEL);
#else
    printf("test_roi: %d/%d checks passed\n", checks - failures, checks);
#endif
    return failures ? 1 : 0;
}
