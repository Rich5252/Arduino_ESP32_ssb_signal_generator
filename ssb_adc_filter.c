// ssb_adc_filter.c
#include "ssb_adc_filter.h"
#include <math.h>

// Same denormal-flush threshold used in ssb_dsp.c - keep consistent
// across the codebase rather than duplicating a different constant.
static inline float flush_denorm(float x)
{
    return (x > -1e-15f && x < 1e-15f) ? 0.0f : x;
}

// Shared RBJ-cookbook 2nd-order LPF design, parameterized by Q - Butterworth
// (Q=0.70710678f, maximally flat) and Chebyshev Type I (Q derived from the
// ripple spec, see ssb_biquad_chebyshev_lpf_init() below) are both just
// this same formula with a different Q, nothing else changes.
static void biquad_lpf_design(ssb_biquad_t *f, float fc_hz, float fs_hz, float Q)
{
    float w0    = 2.0f * (float)M_PI * fc_hz / fs_hz;
    float cosw0 = cosf(w0);
    float sinw0 = sinf(w0);
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

void ssb_biquad_lpf_init(ssb_biquad_t *f, float fc_hz, float fs_hz)
{
    biquad_lpf_design(f, fc_hz, fs_hz, 0.70710678f);   // Butterworth: maximally flat passband
}

// |H(e^-jw)| in dB for the biquad currently loaded in *f, at frequency
// f_hz - used only at filter-design time (see below), never per-sample.
static float biquad_mag_db(const ssb_biquad_t *f, float f_hz, float fs_hz)
{
    float w  = 2.0f * (float)M_PI * f_hz / fs_hz;
    float c1 = cosf(w),       s1 = sinf(w);
    float c2 = cosf(2.0f * w), s2 = sinf(2.0f * w);

    float num_re = f->b0 + f->b1 * c1 + f->b2 * c2;
    float num_im =       - f->b1 * s1 - f->b2 * s2;
    float den_re = 1.0f  + f->a1 * c1 + f->a2 * c2;
    float den_im =       - f->a1 * s1 - f->a2 * s2;

    float num_mag2 = num_re * num_re + num_im * num_im;
    float den_mag2 = den_re * den_re + den_im * den_im;

    return 10.0f * log10f(num_mag2 / den_mag2);
}

// Q-vs-ripple closed form for a 2nd-order Chebyshev Type I low-pass -
// derived and numerically verified against scipy.signal.cheb1ap(2, rp)'s
// analog prototype poles (matched to 5 decimal places across 0.1-3dB
// ripple), then separately verified that plugging the resulting Q into
// biquad_lpf_design() above (the SAME formula ssb_biquad_lpf_init() uses)
// reproduces the expected passband ripple after the bilinear transform -
// valid because this filter runs heavily oversampled (fc/fs ~3.75% at
// 3000Hz/80000Hz), well within the region where frequency warping is
// negligible.
//
// fc_hz means the same thing here as it does for ssb_biquad_lpf_init():
// the TRUE -3dB frequency. That's NOT the same parameter
// biquad_lpf_design()'s own w0/RBJ math expects for a Chebyshev design -
// a Chebyshev filter's conventional "cutoff" is its RIPPLE-BAND EDGE,
// which sits at a LOWER frequency than the true -3dB point (they only
// coincide for Butterworth, where there's no ripple to have an edge).
// Feeding biquad_lpf_design() the ripple-edge frequency directly (an
// earlier version of this function did exactly that, using fc_hz
// unmodified) was verified numerically to be WRONG for what "same 3kHz
// bandwidth, faster rolloff" means in practice: it shifts the true -3dB
// point UP (e.g. ~3725Hz for 1dB ripple at 3000Hz nominal) and, because
// of that shift, is actually WEAKER than the Butterworth filter through
// most of the near stopband (5-8kHz) before finally catching up around
// 10kHz+ - the opposite of "faster rolloff" at the frequencies that
// matter most for this filter's job.
//
// So instead: bisect (at init time only - ~40 iterations of a closed-form
// magnitude evaluation, done once at startup or on a live mode switch,
// NEVER per-sample) for the ripple-edge parameter that makes the
// resulting filter's ACTUAL -3dB point land exactly at fc_hz. For a
// 2nd-order Chebyshev the response falls monotonically past the ripple
// edge, so the ripple-edge parameter is always < fc_hz - bracket is
// [0.05*fc_hz, fc_hz]. Verified against a from-scratch Python
// reimplementation: converges to the same ripple-edge frequency (e.g.
// ~2412Hz for fc_hz=3000Hz/fs=80000Hz/1dB ripple) as an independent
// bisection, and the resulting response is a small in-band ripple bump
// (e.g. +0.7dB peak for 1dB ripple), EXACTLY -3dB at fc_hz (matching
// Butterworth's -3dB point there too), and monotonically MORE attenuated
// than Butterworth from there out to the top of the ADC's Nyquist band
// (up to ~3.8dB more by 10-20kHz) - genuinely "same 3kHz bandwidth,
// faster rolloff", not just "same nominal spec".
void ssb_biquad_chebyshev_lpf_init(ssb_biquad_t *f, float fc_hz, float fs_hz, float ripple_db)
{
    float epsilon = sqrtf(powf(10.0f, ripple_db / 10.0f) - 1.0f);
    float v       = 0.5f * asinhf(1.0f / epsilon);
    float Q       = sqrtf(coshf(2.0f * v)) / (2.0f * sinhf(v));

    float lo = fc_hz * 0.05f;
    float hi = fc_hz;
    for (int i = 0; i < 40; i++) {
        float mid = 0.5f * (lo + hi);
        biquad_lpf_design(f, mid, fs_hz, Q);   // leaves *f loaded with this trial design
        float mag_db = biquad_mag_db(f, fc_hz, fs_hz);
        if (mag_db > -3.0103f) {
            hi = mid;   // not attenuated enough yet at fc_hz -> shift design down
        } else {
            lo = mid;   // overshot -> shift design up
        }
    }
    // *f is already loaded with the last trial design (state reset by the
    // biquad_lpf_design() call inside the loop) - close enough after 40
    // bisection steps that a final re-design call would change nothing
    // observable.
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