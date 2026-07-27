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
    for (int i = 0; i < cnt; i++){
        double pos = i / ratio;
        int i0 = (int)pos; if (i0 >= n) i0 = n - 1;
        int i1 = (i0 + 1 < n) ? i0 + 1 : i0;
        double f = pos - i0;
        out[i] = (int16_t)(in[i0]*(1.0-f) + in[i1]*f);
    }
    return cnt;
}
