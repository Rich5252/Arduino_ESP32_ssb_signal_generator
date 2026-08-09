// ssb_adc_filter.c
#include "ssb_adc_filter.h"
#include <math.h>

// Same denormal-flush threshold used in ssb_dsp.c - keep consistent
// across the codebase rather than duplicating a different constant.
static inline float flush_denorm(float x)
{
    return (x > -1e-15f && x < 1e-15f) ? 0.0f : x;
}

void ssb_biquad_lpf_init(ssb_biquad_t *f, float fc_hz, float fs_hz)
{
    float w0    = 2.0f * (float)M_PI * fc_hz / fs_hz;
    float cosw0 = cosf(w0);
    float sinw0 = sinf(w0);
    float Q     = 0.70710678f;   // Butterworth: maximally flat passband
    float alpha = sinw0 / (2.0f * Q);

    float a0 =  1.0f + alpha;
    float b0 = (1.0f - cosw0) / 2.0f;
    float b1 =  1.0f - cosw0;
    float b2 = (1.0f - cosw0) / 2.0f;
    float a1 = -2.0f * cosw0;
    float a2 =  1.0f - alpha;

    // Normalize by a0 so process() doesn't need a division per sample.
    f->b0 = b0 / a0;
    f->b1 = b1 / a0;
    f->b2 = b2 / a0;
    f->a1 = a1 / a0;
    f->a2 = a2 / a0;

    ssb_biquad_reset(f);
}

void ssb_biquad_reset(ssb_biquad_t *f)
{
    f->z1 = 0.0f;
    f->z2 = 0.0f;
}

// Direct Form II Transposed - good numerical behaviour, only 2 state vars.
float ssb_biquad_process(ssb_biquad_t *f, float in)
{
    float out = f->b0 * in + f->z1;
    f->z1 = f->b1 * in - f->a1 * out + f->z2;
    f->z2 = f->b2 * in - f->a2 * out;

    f->z1 = flush_denorm(f->z1);
    f->z2 = flush_denorm(f->z2);

    return out;
}