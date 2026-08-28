/* resample.c - see resample.h. */
#include "resample.h"
#include <string.h>

int ms_resample(const int16_t *in, int n, int src_rate, int out_rate,
                int16_t *out, int out_cap)
{
    if (n <= 0 || out_cap <= 0) return 0;
    if (src_rate == out_rate || src_rate <= 0){
        int c = n; if (c > out_cap) c = out_cap;
        memcpy(out, in, (size_t)c * sizeof(int16_t));
        return c;
    }
    double ratio = (double)out_rate / (double)src_rate;
    int cnt = (int)(n * ratio);
    if (cnt > out_cap) cnt = out_cap;
    /* Fixed-point source cursor: pos = i/ratio = i*src_rate/out_rate, walked
     * with one add per output sample instead of a floating-point divide per
     * sample (these SoCs have no FPU worth the name). Q32, not Q16: the step
     * is truncated, so its error accumulates over the call, and at Q16 a
     * 3x upsample of a full 16 K-sample buffer walks off by ~0.1 sample -
     * enough to show up against the reference. Q32 puts that walk-off at
     * 1e-5 samples, far below the interpolation's own 1-LSB rounding. */
    uint64_t step = (((uint64_t)(unsigned)src_rate) << 32) / (unsigned)out_rate;
    uint64_t pos = 0;
    for (int i = 0; i < cnt; i++, pos += step){
        int i0 = (int)(pos >> 32); if (i0 >= n) i0 = n - 1;
        int i1 = (i0 + 1 < n) ? i0 + 1 : i0;
        int32_t f = (int32_t)((pos >> 16) & 0xFFFFu);
        /* 64-bit product: the int16 difference times a Q16 fraction overflows
         * int32 at the extremes (65535*65535). One 32x32->64 multiply. */
        out[i] = (int16_t)(in[i0] + (int32_t)(((int64_t)(in[i1] - in[i0]) * f) >> 16));
    }
    return cnt;
}
