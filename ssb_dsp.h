#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_attr.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Digital polar-modulation SSB generator (Weaver/Hilbert-based),
 *        following the architecture used by QCX-SSB / uSDX (Guido PE1NNZ),
 *        adapted here as an original ESP-IDF implementation.
 *
 * Technique summary:
 *  - Audio is treated as the real part of an analytic signal. A Hilbert
 *    transform FIR filter produces the imaginary (quadrature) part.
 *  - Instantaneous phase = atan2(Q, I). The *change* in phase from one
 *    sample to the next is a frequency deviation: dphi/dt <-> delta-f.
 *  - Applying that frequency deviation to the carrier (by continuously
 *    retuning a DDS/synthesizer) phase-modulates the carrier in a way that
 *    reproduces one sideband and cancels the other - no analog quadrature
 *    mixers or crystal filter needed.
 *  - Instantaneous amplitude = sqrt(I^2 + Q^2). Applying that as the PA's
 *    supply-voltage envelope (EER / polar modulation) restores AM
 *    information that pure phase modulation of a class-C/D/E PA would
 *    otherwise discard.
 *
 * This module only computes the two output streams (frequency deviation in
 * Hz, and normalized envelope 0.0-1.0). It does not touch hardware directly:
 * the caller is expected to add the deviation to a carrier frequency and
 * push it to a synthesizer (e.g. ad9851_set_frequency()), and drive a PWM
 * channel from the envelope for PA supply modulation.
 */

typedef enum {
    SSB_SIDEBAND_USB = 0,
    SSB_SIDEBAND_LSB = 1,
} ssb_sideband_t;

typedef struct {
    uint32_t sample_rate_hz;   ///< Audio sample rate, e.g. 8000-19200 Hz. Must match your ADC/timer rate.
    int num_taps;              ///< Hilbert FIR length, ODD, e.g. 33 or 65. Longer = better opposite-sideband
                                ///< suppression and lower cutoff, at the cost of more group delay and CPU.
    float max_freq_dev_hz;     ///< Clamp on |frequency deviation| per sample, e.g. 3000.0f. Without this,
                                ///< phase noise near zero envelope crossings can produce huge spurious
                                ///< instantaneous frequency spikes (a well-known issue in this technique;
                                ///< QCX-SSB refers to this as "restricting" the phase changes).
} ssb_dsp_config_t;

typedef struct ssb_dsp_s *ssb_dsp_handle_t;

/**
 * @brief Allocate and initialize a SSB DSP instance: generates windowed
 *        Hilbert FIR coefficients and delay-line buffers.
 */
esp_err_t ssb_dsp_init(const ssb_dsp_config_t *cfg, ssb_dsp_handle_t *out_handle);

/**
 * @brief Feed one new audio sample and get the resulting frequency deviation
 *        (add this to your carrier frequency before calling
 *        ad9851_set_frequency()) and normalized envelope (0.0-1.0, use to
 *        scale your PA-supply PWM duty cycle).
 *
 * @param handle        Instance from ssb_dsp_init()
 * @param audio_sample   Latest audio sample, normalized to roughly [-1.0, 1.0]
 * @param sideband       USB or LSB (flips the sign of the frequency deviation)
 * @param out_freq_dev_hz  Output: signed frequency deviation in Hz
 * @param out_envelope     Output: envelope magnitude, roughly [0.0, 1.0] for
 *                          full-scale input; caller may need their own gain
 *                          scaling depending on input signal levels.
 */
void IRAM_ATTR ssb_dsp_process_sample(ssb_dsp_handle_t handle,
                             float audio_sample,
                             ssb_sideband_t sideband,
                             float *out_freq_dev_hz,
                             float *out_envelope);

/**
 * @brief Free all buffers associated with a SSB DSP instance.
 */
void ssb_dsp_deinit(ssb_dsp_handle_t handle);

/**
 * @brief Group delay of the Hilbert FIR, in samples ((num_taps-1)/2).
 *        Informational only - useful if you want to align this audio path
 *        against another (e.g. a sidetone or receive-side monitor).
 */
int ssb_dsp_group_delay_samples(ssb_dsp_handle_t handle);

#ifdef __cplusplus
}
#endif
