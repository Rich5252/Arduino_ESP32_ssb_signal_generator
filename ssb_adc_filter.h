// ssb_adc_filter.h
// 2nd-order Butterworth low-pass biquad for ADC noise reduction.
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

// Process one sample. Call once per raw ADC sample, from TASK context only.
// State is flushed of denormals internally, consistent with ssb_dsp.c's
// flush_denorm() handling elsewhere in the pipeline.
float ssb_biquad_process(ssb_biquad_t *f, float in);

// Reset filter state (e.g. if restarting capture) without recomputing coeffs.
void ssb_biquad_reset(ssb_biquad_t *f);

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