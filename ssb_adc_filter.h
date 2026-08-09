// ssb_adc_filter.h
// 2nd-order Butterworth low-pass biquad, intended to replace the N=16
// boxcar average currently applied to raw ADC samples in the
// adc_continuous on_conv_done() callback.
//
// Rationale: noise floor measured flat/broadband on spectrum analyser,
// so oversampling + LPF is the right approach in principle - but a
// boxcar has poor stopband rejection (leaky sinc sidelobes, ~-13dB
// best case) and only updates once per DMA frame (200us @ N=16,
// 80kHz), creating a zero-order-hold staircase in dsp_task's reads.
// This filter runs per raw sample (every 12.5us @ 80kHz) with a real
// -12dB/octave rolloff above cutoff.
//
// Usage:
//   static ssb_biquad_t s_adc_lpf;
//   ssb_biquad_lpf_init(&s_adc_lpf, 3000.0f, 80000.0f);   // once, at init
//   ...
//   // inside on_conv_done(), for each raw sample in the frame:
//   float filtered = ssb_biquad_process(&s_adc_lpf, (float)raw_adc_value);
//   s_last_adc_raw = filtered;   // volatile, read by dsp_task

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float b0, b1, b2;   // feedforward coefficients (normalized, a0=1)
    float a1, a2;        // feedback coefficients (normalized, a0=1)
    float z1, z2;         // Direct Form II transposed state
} ssb_biquad_t;

// Initialize as a 2nd-order Butterworth (Q=0.707) low-pass, RBJ cookbook formula.
// fc_hz: cutoff frequency, fs_hz: sample rate the filter will run at
// (i.e. the raw ADC rate, e.g. 80000 - NOT the dsp_task rate).
void ssb_biquad_lpf_init(ssb_biquad_t *f, float fc_hz, float fs_hz);

// Process one sample. Call once per raw ADC sample, in ISR/callback context.
// State is flushed of denormals internally, consistent with ssb_dsp.c's
// flush_denorm() handling elsewhere in the pipeline.
float ssb_biquad_process(ssb_biquad_t *f, float in);

// Reset filter state (e.g. if restarting capture) without recomputing coeffs.
void ssb_biquad_reset(ssb_biquad_t *f);

#ifdef __cplusplus
}
#endif