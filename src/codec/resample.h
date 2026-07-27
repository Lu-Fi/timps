/* resample.h - shared linear resampler for the speaker output path.
 * Used by both audio producers that feed IMP_AO (backchannel + play queue),
 * extracted from the original in-file resampler in backchannel.c. */
#ifndef MS_RESAMPLE_H
#define MS_RESAMPLE_H
#include <stdint.h>

/* Linear-interpolate mono int16 `in` (n samples at src_rate) to out_rate,
 * writing at most out_cap samples into out[]. Returns samples written.
 * src_rate <= 0 or == out_rate is a straight copy (clamped to out_cap). */
int ms_resample(const int16_t *in, int n, int src_rate, int out_rate,
                int16_t *out, int out_cap);

#endif
