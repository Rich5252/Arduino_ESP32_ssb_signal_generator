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

// ---- Fixed-point (Q15) variant - integer only, safe to call from ISR ----

static inline int32_t round_to_q15(float x)
{
    float scaled = x * (float)(1 << SSB_BIQUAD_FIXED_SHIFT);
    return (int32_t)(scaled >= 0.0f ? (scaled + 0.5f) : (scaled - 0.5f));
}

void ssb_biquad_fixed_lpf_init(ssb_biquad_fixed_t *f, float fc_hz, float fs_hz)
{
    // Same RBJ-cookbook derivation as ssb_biquad_lpf_init() - float math
    // here is fine, this runs once from task context (e.g. init_adc()),
    // never from the ISR that calls ssb_biquad_fixed_process().
    float w0    = 2.0f * (float)M_PI * fc_hz / fs_hz;
    float cosw0 = cosf(w0);
    float sinw0 = sinf(w0);
    float Q     = 0.70710678f;
    float alpha = sinw0 / (2.0f * Q);

    float a0 =  1.0f + alpha;
    float b0 = (1.0f - cosw0) / 2.0f;
    float b1 =  1.0f - cosw0;
    float b2 = (1.0f - cosw0) / 2.0f;
    float a1 = -2.0f * cosw0;
    float a2 =  1.0f - alpha;

    f->b0 = round_to_q15(b0 / a0);
    f->b1 = round_to_q15(b1 / a0);
    f->b2 = round_to_q15(b2 / a0);
    f->a1 = round_to_q15(a1 / a0);   // can approach -2*32768 near very low cutoffs - int32 has ample headroom
    f->a2 = round_to_q15(a2 / a0);

    ssb_biquad_fixed_reset(f);
}

void ssb_biquad_fixed_reset(ssb_biquad_fixed_t *f)
{
    f->z1 = 0;
    f->z2 = 0;
}

// Direct Form II Transposed, integer arithmetic only - no denormal concern
// (that's a float-specific issue), no FPU coprocessor touched, so this is
// legal to call directly from ISR context, unlike ssb_biquad_process().
//
// IMPORTANT: the feedback terms (a1, a2) use out_scaled - the FULL Q15
// precision result - not the truncated-to-integer 'out' below. a1 here
// sits close to -2 (near-unity-gain feedback); feeding a coarsely
// quantized value back through that is the textbook cause of fixed-point
// IIR limit-cycle noise - a small self-sustaining oscillation generated
// by the quantization step itself. Learned this one the hard way too:
// an earlier version truncated before feeding back and reintroduced
// noise on the sweep test despite the filter coefficients being
// identical. Truncation only happens once, right at the end, for the
// value handed to the caller - it never re-enters the recursion.
int32_t ssb_biquad_fixed_process(ssb_biquad_fixed_t *f, int32_t in)
{
    int64_t out_scaled = (int64_t)f->b0 * in + f->z1;   // full Q15 precision

    f->z1 = (int64_t)f->b1 * in
          - (((int64_t)f->a1 * out_scaled) >> SSB_BIQUAD_FIXED_SHIFT)
          + f->z2;
    f->z2 = (int64_t)f->b2 * in
          - (((int64_t)f->a2 * out_scaled) >> SSB_BIQUAD_FIXED_SHIFT);

    return (int32_t)(out_scaled >> SSB_BIQUAD_FIXED_SHIFT);   // truncate ONLY here, for the caller
}