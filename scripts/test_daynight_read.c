/* test_daynight_read.c - host-only unit test for daynight.c's dn_read() on
 * the open-tx-isp T41 dump and the GetAeExprInfo AE reserve (opened for test
 * via -DDN_READ_TEST, HAL stubbed below).
 *
 * Built and run by `make test-daynight-read`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "config.h"
#include "hal/hal.h"

extern int dn_read_test(const ms_daynight_cfg *dn, float *gain, float *ratio,
                        float *d, int *headroom, int *clipped);

static int g_fail;
static int g_ae_rc = -1;
static hal_isp_ae g_ae;

int hal_isp_total_gain(uint32_t *gain) { (void)gain; return -1; }
int hal_isp_exposure(hal_isp_expo *out) { (void)out; return -1; }
int hal_isp_ae_info(hal_isp_ae *out)
{
    if (g_ae_rc == 0) *out = g_ae;
    return g_ae_rc;
}
void log_printf(int level, const char *module, const char *fmt, ...)
{
    (void)level; (void)module; (void)fmt;
}

/* the open-tx-isp T41 dump as read on the Vanhua T55A (gc5603), night */
static const char T41_DUMP[] =
    "ISP Runing Mode : night\n"
    "********* Ae Expr Info *********\n"
    "AeIntegrationTime : 1750\n"
    "AeAGain : 7104\n"
    "AeMinIntegrationTime : 1\n"
    "AeMaxIntegrationTime : 2092\n"
    "AeMaxAGain : 63443\n"
    "TotalGainDb : 183134\n"
    "ExposureValue : 12140\n";

static const char CLASSIC_DUMP[] =
    "ISP Runing Mode : Day\n"
    "SENSOR analog gain : 100\n"
    "MAX SENSOR analog gain : 320\n"
    "MAX ISP digital gain : 64\n"
    "ISP digital gain : 0\n"
    "SENSOR Integration Time : 500 lines\n"
    "SENSOR Max Integration Time : 1000 lines\n";

/* the open stack's isp-m0 on the Wyze Pan v1 (T20/jxf22), dark: the sensor
 * maximum 158 while the AE stops at 128, which isp_info beside it publishes */
static const char PAN_M0[] =
    "SENSOR Integration Time : 1967 lines\n"
    "SENSOR Max Integration Time : 2246 lines\n"
    "ISP Runing Mode : Day\n"
    "SENSOR analog gain : 128\n"
    "MAX SENSOR analog gain : 158\n"
    "SENSOR digital gain : 0\n"
    "MAX SENSOR digital gain : 0\n"
    "ISP digital gain : 0\n"
    "MAX ISP digital gain : 0\n";
static const char PAN_INFO[] =
    "SENSOR analog gain : 128\n"
    "MAX SENSOR analog gain : 128\n"
    "MAX SENSOR digital gain : 0\n"
    "MAX ISP digital gain : 32\n";

static void put(const char *path, const char *txt)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    fputs(txt, f);
    fclose(f);
}

static void ae(int rc, unsigned it, unsigned it_max, unsigned again,
               unsigned again_max, unsigned tg)
{
    g_ae_rc = rc;
    memset(&g_ae, 0, sizeof g_ae);
    g_ae.it = it; g_ae.it_min = 1; g_ae.it_max = it_max;
    g_ae.again = again; g_ae.again_max = again_max;
    g_ae.total_log2_q16 = tg;
}

static void expect(const char *what, const ms_daynight_cfg *dn,
                   int want_hr, int want_clip)
{
    float gain, ratio, d;
    int hr, clip;
    dn_read_test(dn, &gain, &ratio, &d, &hr, &clip);
    if (hr != want_hr || clip != want_clip) {
        fprintf(stderr, "FAIL %s: headroom %d clipped %d, want %d / %d\n",
                what, hr, clip, want_hr, want_clip);
        g_fail = 1;
    } else {
        printf("ok   %s: headroom %d clipped %d\n", what, hr, clip);
    }
}

int main(void)
{
    char path[] = "/tmp/timps-dnread-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return 2; }
    close(fd);

    ms_daynight_cfg dn;
    memset(&dn, 0, sizeof dn);

    /* first, so the first isp-m0 read loads the cached isp_info cap */
    {
        char dir[] = "/tmp/timps-dnread-pan-XXXXXX", m0[96], info[96];
        if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
        snprintf(m0, sizeof m0, "%s/isp-m0", dir);
        snprintf(info, sizeof info, "%s/isp_info", dir);
        put(m0, PAN_M0);
        put(info, PAN_INFO);
        snprintf(dn.isp_path, sizeof dn.isp_path, "%s", m0);
        /* (128-128) + (0-0) + 32 for IT 1967 < 90 % of 2246 */
        expect("pan isp-m0 158 + isp_info 128", &dn, 32, 0);
        unlink(info);
        expect("pan isp_info removed: cached", &dn, 32, 0);
        float g, r, d; int hr, cl;
        for (int i = 0; i < 200; i++) dn_read_test(&dn, &g, &r, &d, &hr, &cl);
        expect("pan isp_info absent after refresh", &dn, 30 + 32, 0);
        put(info, "MAX SENSOR analog gain : 160\n");
        for (int i = 0; i < 200; i++) dn_read_test(&dn, &g, &r, &d, &hr, &cl);
        expect("pan isp_info 160 does not raise 158", &dn, 30 + 32, 0);
        unlink(info); unlink(m0); rmdir(dir);
    }

    snprintf(dn.isp_path, sizeof dn.isp_path, "%s", path);

    /* parse: it/mit from the Ae* lines, gain from TotalGainDb */
    put(path, T41_DUMP);
    ae(-1, 0, 0, 0, 0, 0);
    {
        float gain, ratio, d;
        int hr, clip;
        int isp = dn_read_test(&dn, &gain, &ratio, &d, &hr, &clip);
        if (isp != 1 || fabsf(gain - 1776.0f) > 1.0f ||
            fabsf(ratio - 1750.0f / 2092.0f) > 1e-4f || fabsf(d - 1486.0f) > 1.0f) {
            fprintf(stderr, "FAIL t41 parse: isp %d gain %.1f ratio %.4f d %.1f "
                            "(want 1 / 1776 / 0.8365 / 1486)\n",
                    isp, (double)gain, (double)ratio, (double)d);
            g_fail = 1;
        } else {
            printf("ok   t41 parse: gain %.0f ratio %.4f d %.0f\n",
                   (double)gain, (double)ratio, (double)d);
        }
    }

    /* /proc alone: its AeMaxAGain is uncapped, so the reserve stays unknown */
    expect("t41 /proc only (GetAeExprInfo fails)", &dn, -1, 0);

    /* capped at 16x: 4 - 2.794 EV = 1.21 stops; gain above 1x, so no
     * integration-time stop although it is 84 % of its max */
    ae(0, 1750, 2092, 7104, 16384, 183134);
    expect("t41 cap 16x, 6.9x applied", &dn, 39, 0);

    /* railed at the cap */
    ae(0, 2092, 2092, 7104, 7104, 183134);
    expect("t41 at the cap", &dn, 0, 1);

    /* within a quarter stop of the cap still counts as clipped */
    ae(0, 2092, 2092, 7104, 8192, 183134);
    expect("t41 0.21 stop below the cap", &dn, 7, 1);

    /* no cap configured: the SDK reports the sensor maximum, which is then
     * the AE's real limit */
    ae(0, 1750, 2092, 7104, 63443, 183134);
    expect("t41 no cap (sensor max)", &dn, 101, 0);

    /* bright: gain at 1x and integration well short of its max */
    ae(0, 500, 2092, 1024, 16384, 0);
    expect("t41 bright, 1x gain", &dn, 160, 0);

    /* the call fails again (ISP restarting): back to unknown */
    ae(-1, 0, 0, 0, 0, 0);
    expect("t41 call fails", &dn, -1, 0);

    /* classic dump: its own ceiling fields decide, the SDK value is ignored */
    put(path, CLASSIC_DUMP);
    ae(0, 2092, 2092, 7104, 7104, 183134);
    expect("classic dump wins over GetAeExprInfo", &dn, 220 + 64 + 32, 0);

    unlink(path);
    if (g_fail) { fprintf(stderr, "test_daynight_read: FAILED\n"); return 1; }
    printf("test_daynight_read: all passed\n");
    return 0;
}
