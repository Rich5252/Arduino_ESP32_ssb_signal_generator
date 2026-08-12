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

/**
 * @brief Optional pre-Hilbert audio conditioning: two cascaded biquads
 *        (high-pass + presence peak) followed by a feed-forward envelope
 *        compressor, applied to the raw audio sample BEFORE it enters the
 *        Hilbert delay line. Runs entirely on adds/mults/one divide per
 *        sample - no log/exp/pow in the per-sample path - so it stays
 *        negligible against the DSP budget regardless of Fs.
 *
 *        Processing order is compressor -> EQ (compress-then-EQ), the
 *        more common choice for SSB voice punch: it avoids the presence
 *        boost itself tripping the compressor.
 *
 *        Leave `enable = false` (or zero-initialize this struct, e.g. via
 *        a designated initializer that omits it) to reproduce the exact
 *        prior behavior with zero added cost.
 */
typedef struct {
    bool enable;                ///< Master enable for this whole stage.
    float hpf_freq_hz;          ///< High-pass corner, e.g. 300.0f (rumble/proximity cut).
    float presence_freq_hz;     ///< Presence-peak center, e.g. 2200.0f.
    float presence_gain_db;     ///< Presence-peak gain in dB, e.g. 4.0f. 0.0f = no boost (filter still runs).
    float presence_q;           ///< Presence-peak Q, e.g. 1.0f.
    float comp_threshold;       ///< Compressor threshold, linear envelope units (post-HPF signal is
                                 ///< typically < ~1.0), e.g. 0.3f. Only levels above this are compressed.
    float comp_ratio;           ///< Compression ratio above threshold, e.g. 3.5f means 3.5:1.
    float comp_attack_ms;       ///< Envelope-follower attack time, e.g. 3.0f.
    float comp_release_ms;      ///< Envelope-follower release time, e.g. 120.0f.
} ssb_audio_fx_config_t;

typedef struct {
    uint32_t sample_rate_hz;   ///< Audio sample rate, e.g. 8000-19200 Hz. Must match your ADC/timer rate.
    int num_taps;              ///< Hilbert FIR length, ODD, e.g. 33 or 65. Longer = better opposite-sideband
                                ///< suppression and lower cutoff, at the cost of more group delay and CPU.
    float max_freq_dev_hz;     ///< Clamp on |frequency deviation| per sample, e.g. 3000.0f. Without this,
                                ///< phase noise near zero envelope crossings can produce huge spurious
                                ///< instantaneous frequency spikes (a well-known issue in this technique;
                                ///< QCX-SSB refers to this as "restricting" the phase changes).
    ssb_audio_fx_config_t audio_fx;  ///< Optional pre-Hilbert EQ/compression. See ssb_audio_fx_config_t.
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

/**
 * @brief Live-tweak the compressor's threshold/ratio without re-running
 *        ssb_dsp_init(). EQ shape and attack/release are fixed at init
 *        (they involve trig/exp, deliberately kept out of any per-sample
 *        or frequently-called path); threshold/ratio are cheap to change
 *        on the fly if you want a physical pot or serial command for it.
 *        No-op if audio_fx wasn't enabled at init.
 */
void ssb_dsp_set_compressor(ssb_dsp_handle_t handle, float threshold, float ratio);

/**
 * @brief Independently enable/disable the EQ (HPF + presence peak) and
 *        compressor stages at runtime - e.g. from a serial command, to
 *        A/B each one's contribution without recompiling. Both default
 *        to whatever ssb_audio_fx_config_t::enable was at init.
 *        No-op (and the getters return false) if audio_fx wasn't enabled
 *        at init - the underlying biquad/compressor state was never set
 *        up, so there's nothing to toggle. IRAM_ATTR: safe to call from
 *        the real-time path, though the intended use is occasional calls
 *        from a command handler, not per-sample.
 */
void IRAM_ATTR ssb_dsp_set_eq_enabled(ssb_dsp_handle_t handle, bool enable);
void IRAM_ATTR ssb_dsp_set_compressor_enabled(ssb_dsp_handle_t handle, bool enable);
bool ssb_dsp_get_eq_enabled(ssb_dsp_handle_t handle);
bool ssb_dsp_get_compressor_enabled(ssb_dsp_handle_t handle);

/**
 * @brief Master gain trim, in dB, applied after EQ/compressor (or
 *        directly to the raw sample if audio_fx wasn't enabled at init -
 *        this always works). Deliberately manual rather than automatic:
 *        the compressor's gain reduction is exactly computable from its
 *        threshold/ratio (see ssb_dsp_set_compressor - its makeup gain
 *        is applied automatically now), but EQ's effect on perceived
 *        level depends on the input spectrum, which isn't something
 *        this module can know - use this to trim it out by ear/scope
 *        instead of trusting a guessed number. IRAM_ATTR: safe to call
 *        from the real-time path, though intended for occasional calls
 *        (e.g. a serial '+'/'-' command), not per-sample.
 */
void IRAM_ATTR ssb_dsp_set_master_gain_db(ssb_dsp_handle_t handle, float gain_db);
float ssb_dsp_get_master_gain_db(ssb_dsp_handle_t handle);

/**
 * @brief Evidence for setting max_freq_dev_hz from real data instead of
 *        guessing a constant and re-measuring on real hardware. Confirmed
 *        on real hardware that too-tight a clamp doesn't just fail to
 *        protect against wild instantaneous-frequency spikes near
 *        envelope zero-crossings (its intended job) - it can also bias a
 *        two-tone signal's average output frequency via asymmetric
 *        clipping of otherwise-legitimate content, and directly hurt
 *        sideband suppression. max_unclamped_freq_dev_hz is the running
 *        high-water mark of the TRUE (pre-clamp) peak deviation the
 *        signal actually reaches; clip_count is how many samples the
 *        clamp has actually had to intervene on. Both since init or the
 *        last ssb_dsp_reset_freq_dev_stats() call.
 */
typedef struct {
    float max_unclamped_freq_dev_hz;
    uint32_t clip_count;
} ssb_dsp_freq_dev_stats_t;

void ssb_dsp_get_freq_dev_stats(ssb_dsp_handle_t handle, ssb_dsp_freq_dev_stats_t *out);
void ssb_dsp_reset_freq_dev_stats(ssb_dsp_handle_t handle);

/**
 * @brief Sub-phase timing breakdown of ssb_dsp_process_sample, each a
 *        running high-water mark in microseconds since ssb_dsp_init().
 *        Measured internally via esp_timer_get_time() - negligible
 *        overhead (a handful of reads/compares), safe to leave enabled
 *        permanently rather than only when chasing a specific problem.
 *        Use this to find out where time is actually going inside the
 *        DSP call instead of guessing - e.g. after removing the FIR
 *        loop's modulo only shaved ~2us off the total, so the real cost
 *        is evidently elsewhere.
 */
typedef struct {
    uint32_t max_audio_fx_us;  ///< Compressor + 2 biquads (0 if audio_fx wasn't enabled at init).
    uint32_t max_fir_us;       ///< Hilbert FIR convolution (num_taps multiply-adds).
    uint32_t max_atan2_us;     ///< atan2f() alone (instantaneous phase).
    uint32_t max_sqrt_us;      ///< sqrtf() alone (envelope magnitude).
} ssb_dsp_profile_t;

void ssb_dsp_get_profile(ssb_dsp_handle_t handle, ssb_dsp_profile_t *out);

#ifdef __cplusplus
}
#endif