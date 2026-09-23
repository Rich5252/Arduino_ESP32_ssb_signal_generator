// ssb_adc_filter.h
// 2nd-order low-pass biquad(s) for ADC noise reduction - Butterworth
// (maximally flat) and Chebyshev Type I (steeper rolloff, at the cost of
// passband ripple), both built from the same RBJ-cookbook LPF formula with
// only Q changed - see ssb_adc_filter.c for the derivation.
//
// Two variants:
//  - ssb_biquad_t / ssb_biquad_process(): float, Direct Form II
//    Transposed. NOT SAFE IN ISR CONTEXT on Xtensa - float ops need the
//    FPU coprocessor, which has no register-save area for ISRs (trips a
//    CoprocessorException/Guru Meditation the moment it's used there;
//    confirmed the hard way). Fine to use from a FreeRTOS task.
//  - ssb_biquad_fixed_t / ssb_biquad_fixed_process(): Q15 fixed-point,
//    integer-only. Safe in ISR context - this is the one to call
//    directly from an adc_continuous on_conv_done() callback, which is
//    what lets the filter update every raw ADC sample (e.g. every
//    12.5us @ 80kHz) without adding per-tick draining cost to the
//    real-time task that reads its output.
//
// Both are initialized via the same RBJ-cookbook LPF formula (float math
// at init time only - that's fine, init happens from task context, e.g.
// inside setup()/init_adc(), never from an ISR).

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Float variant - task context only, NOT ISR-safe ----

typedef struct {
    float b0, b1, b2;   // feedforward coefficients (normalized, a0=1)
    float a1, a2;        // feedback coefficients (normalized, a0=1)
    float z1, z2;         // Direct Form II transposed state
} ssb_biquad_t;

// Initialize as a 2nd-order Butterworth (Q=0.707) low-pass, RBJ cookbook formula.
// fc_hz: cutoff frequency, fs_hz: sample rate the filter will run at
// (i.e. the raw ADC rate, e.g. 80000 - NOT the dsp_task rate).
void ssb_biquad_lpf_init(ssb_biquad_t *f, float fc_hz, float fs_hz);

// Initialize as a 2nd-order Chebyshev Type I low-pass with passband ripple
// ripple_db (e.g. 1.0f). Q is derived from the ripple spec instead of the
// fixed Butterworth Q=0.707 (closed form: epsilon=sqrt(10^(Rp/10)-1),
// v=0.5*asinh(1/epsilon), Q=sqrt(cosh(2v))/(2*sinh(v)) - cross-checked
// numerically against scipy.signal.cheb1ap's analog prototype poles to 5
// decimal places).
//
// fc_hz here means exactly what it means for ssb_biquad_lpf_init() above:
// the TRUE -3dB frequency - NOT the raw "ripple-band edge" a textbook
// Chebyshev spec (or the RBJ formula's own w0 parameter) would normally
// use. Those two ARE different frequencies for a Chebyshev filter (the
// ripple edge sits lower - they only coincide for Butterworth, which has
// no ripple to have an edge), so internally this bisects for the
// ripple-edge parameter that makes the ACTUAL -3dB point land exactly at
// fc_hz (see ssb_adc_filter.c for the numeric verification, and for why
// the naive "just reuse fc_hz directly as the RBJ w0 parameter" version
// was checked and found WRONG - it actually gives weaker attenuation than
// Butterworth through most of the near stopband, the opposite of the
// intent). The bisection is ~40 iterations of a closed-form magnitude
// evaluation, done once at init time only (task context, cheap) - never
// per-sample.
//
// Net effect of calling this with the SAME fc_hz as the Butterworth
// filter: both have their -3dB point at exactly fc_hz, and the Chebyshev
// filter is monotonically MORE attenuated above that - genuinely "same
// bandwidth, faster rolloff", not just a same-nominal-spec approximation.
void ssb_biquad_chebyshev_lpf_init(ssb_biquad_t *f, float fc_hz, float fs_hz, float ripple_db);

// Process one sample. Call once per raw ADC sample, from TASK context only.
// State is flushed of denormals internally, consistent with ssb_dsp.c's
// flush_denorm() handling elsewhere in the pipeline.
float ssb_biquad_process(ssb_biquad_t *f, float in);

// Reset filter state (e.g. if restarting capture) without recomputing coeffs.
void ssb_biquad_reset(ssb_biquad_t *f);

// ---- 4th-order variant: two cascaded 2nd-order stages ----
//
// A cascade of two independently-bilinear-transformed 2nd-order sections
// is mathematically exact for a true 4th-order digital filter (the
// bilinear transform's z<->s substitution is applied to a rational
// function that's already factored into 2nd-order sections - factoring
// commutes with substitution, so this isn't an approximation of a "real"
// 4th-order design, it IS one), same technique used throughout audio DSP
// to build higher-order filters out of RBJ-style biquad stages.
typedef struct {
    ssb_biquad_t stage1, stage2;
} ssb_biquad4_t;

void ssb_biquad4_reset(ssb_biquad4_t *f);

// Process one sample through both cascaded stages in order. Same
// task-context-only restriction as ssb_biquad_process() (float ops).
float ssb_biquad4_process(ssb_biquad4_t *f, float in);

// 4th-order Butterworth: both stages share the SAME fc_hz (true -3dB
// point, same convention as ssb_biquad_lpf_init()) but different Q -
// the standard per-stage Butterworth pole-angle Qs for N=4:
// Q1=1/(2*cos(pi/8))=0.541196 (the more heavily damped stage), Q2=1/(2*
// cos(3*pi/8))=1.306563 (the more resonant stage; cascaded, their peaking
// cancels the other's rolloff-shoulder to give the maximally-flat result).
// Cascading two RBJ biquads at the same fc with these two Qs reproduces
// the true 4th-order Butterworth exactly - no bisection/correction needed
// (verified numerically: composite -3dB lands within <1e-6 of fc_hz as-is).
void ssb_biquad4_lpf_init(ssb_biquad4_t *f, float fc_hz, float fs_hz);

// 4th-order Chebyshev Type I, ripple_db passband ripple. UNLIKE the
// Butterworth cascade above, the two stages are NOT at the same
// frequency - a Chebyshev prototype's poles sit on an ellipse, not a
// circle, so each stage gets its own center frequency AND its own Q,
// both derived from the ripple spec (see ssb_adc_filter.c for the
// pole-pair math). fc_hz has the same meaning as everywhere else in this
// file: the TRUE -3dB frequency of the composite (cascaded) response,
// solved via the same one-time bisection technique
// ssb_biquad_chebyshev_lpf_init() uses (~40 iterations at init time,
// applied as a single shared frequency-scale factor across both stages
// so their relative spacing - set by the ripple/order math - stays
// correct while the whole pair slides to hit the target -3dB point).
void ssb_biquad4_chebyshev_lpf_init(ssb_biquad4_t *f, float fc_hz, float fs_hz, float ripple_db);

// ---- 6th-order variant: three cascaded 2nd-order stages ----
//
// 2026-09-23: added alongside the 8th-order variant below when the mic's
// ADC anti-alias stopband (currently 4th order, ~3-8kHz region) was found
// to be the limiting factor on out-of-band rejection once a better
// electret capsule is fitted. Same "cascade of independently-transformed
// biquads is mathematically exact, not an approximation" reasoning as
// ssb_biquad4_t - just one more stage. Offered as the smaller of the two
// steps (roughly +50% CPU over 4th order vs. 8th order's +100%) since a
// live CPU-headroom check on this same day found real margin much
// tighter than when the 2nd->4th jump was made (see
// moving_forward_notes.md's 2026-09-23 entry) - worth having as a
// fallback if 8th order's extra cost doesn't fit.
typedef struct {
    ssb_biquad_t stage1, stage2, stage3;
} ssb_biquad6_t;

void ssb_biquad6_reset(ssb_biquad6_t *f);
float ssb_biquad6_process(ssb_biquad6_t *f, float in);

// 6th-order Butterworth: three stages, same shared fc_hz, standard
// per-stage pole-angle Qs for N=6 (theta_k=(2k-1)*pi/12, Q_k=1/(2*cos
// (theta_k))): Q1=1/(2*cos(pi/12))=0.517638, Q2=1/(2*cos(pi/4))=0.707107
// (the middle stage lands exactly on the familiar 2nd-order-Butterworth Q
// by coincidence of the angle math, not a special case), Q3=1/(2*cos(5*
// pi/12))=1.931852. Verified the same way as the N=4 case: composite -3dB
// lands within <1e-6 of fc_hz as-is, no bisection needed.
void ssb_biquad6_lpf_init(ssb_biquad6_t *f, float fc_hz, float fs_hz);

// 6th-order Chebyshev Type I - same ellipse-pole-pair derivation and
// same one-time bisection technique as ssb_biquad4_chebyshev_lpf_init(),
// just with three (theta_k=(2k-1)*pi/12, k=1,2,3) pole pairs instead of
// two. Numerically checked at fs=64000Hz/fc=3000Hz/1dB ripple: true -3dB
// lands at fc_hz as designed, peak in-band ripple stays ~0.99dB (same as
// the 4th-order design at the same ripple_db - going to a higher order at
// a FIXED ripple spec does NOT multiply the ripple the way naively
// cascading an existing filter with itself would; see this file's own
// header comment and moving_forward_notes.md's 2026-09-23 entry for why
// that distinction matters), while stopband rejection improves
// substantially (~-76.9dB at 8kHz vs the 4th-order design's ~-48.4dB).
void ssb_biquad6_chebyshev_lpf_init(ssb_biquad6_t *f, float fc_hz, float fs_hz, float ripple_db);

// ---- 8th-order variant: four cascaded 2nd-order stages ----
//
// 2026-09-23: same motivation as ssb_biquad6_t above - the larger of the
// two steps, roughly double the CPU of the 4th-order filter (four biquad
// evaluations per raw ADC sample instead of two), same relationship the
// existing 2nd->4th jump already established and validated ("4th order
// roughly DOUBLES the dB rejection... for negligible extra CPU" per
// adc_capture.h) - repeating that same doubling pattern one more time,
// with a genuinely separately-designed higher-order filter rather than
// running the existing 4th-order filter through itself twice (which would
// instead roughly DOUBLE the ripple in dB terms, shift the effective
// -3dB point, and double group delay - see moving_forward_notes.md's
// 2026-09-23 entry for the full comparison).
typedef struct {
    ssb_biquad_t stage1, stage2, stage3, stage4;
} ssb_biquad8_t;

void ssb_biquad8_reset(ssb_biquad8_t *f);
float ssb_biquad8_process(ssb_biquad8_t *f, float in);

// 8th-order Butterworth: four stages, same shared fc_hz, standard
// per-stage pole-angle Qs for N=8 (theta_k=(2k-1)*pi/16, Q_k=1/(2*cos
// (theta_k))): Q1=0.509796, Q2=0.601345, Q3=0.899976, Q4=2.562915.
// Same "cascading at the same fc_hz with these Qs is exact" guarantee as
// N=4/N=6 - verified numerically (<1e-6 at fc_hz).
void ssb_biquad8_lpf_init(ssb_biquad8_t *f, float fc_hz, float fs_hz);

// 8th-order Chebyshev Type I - four (theta_k=(2k-1)*pi/16, k=1..4)
// ellipse pole pairs, same bisection technique as the 4th/6th-order
// versions. Numerically checked at fs=64000Hz/fc=3000Hz/1dB ripple: true
// -3dB at fc_hz, peak in-band ripple still ~0.99dB (order-independent at
// a fixed ripple spec, same point as the 6th-order comment above),
// stopband rejection ~-105.8dB at 8kHz vs the 4th-order design's
// ~-48.4dB - more than double, since Chebyshev's stopband slope (unlike
// Butterworth's) doesn't scale purely linearly with order from a fixed
// ripple-edge baseline.
void ssb_biquad8_chebyshev_lpf_init(ssb_biquad8_t *f, float fc_hz, float fs_hz, float ripple_db);

// ---- Fixed-point (Q15) variant - ISR-safe, integer only ----

#define SSB_BIQUAD_FIXED_SHIFT   15   // Q15: coefficients scaled by 1<<15

typedef struct {
    int32_t b0, b1, b2;   // feedforward coefficients, Q15
    int32_t a1, a2;        // feedback coefficients, Q15
    int64_t z1, z2;         // Direct Form II transposed state, Q15-scaled
                             // (int64 headroom - cheap on Xtensa, no coprocessor
                             // needed, and removes any need to reason about
                             // overflow margins for a1 which can approach -2).
} ssb_biquad_fixed_t;

// Same LPF design as ssb_biquad_lpf_init(), quantized to Q15 integers.
// Called from task context (float math at init time is fine here - it's
// the per-sample PROCESS call that must stay ISR-legal, not init).
void ssb_biquad_fixed_lpf_init(ssb_biquad_fixed_t *f, float fc_hz, float fs_hz);

// Process one sample. Integer-only - safe to call directly from an ISR
// (e.g. adc_continuous's on_conv_done callback). in/return value are in
// the same integer units as the raw ADC reading (e.g. 0-4095 for 12-bit).
int32_t ssb_biquad_fixed_process(ssb_biquad_fixed_t *f, int32_t in);

void ssb_biquad_fixed_reset(ssb_biquad_fixed_t *f);

#ifdef __cplusplus
}
#endif