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
 * @brief Diagnostic-only: direct firmware-side measurement of the mean
 *        per-sample phase delta (dphi, radians) computed by wrap_pi() in
 *        ssb_dsp_process_sample() - BEFORE slew-limiting or the
 *        max_freq_dev_hz clamp, i.e. the true output of the atan2/
 *        null-crossing phase math itself.
 *
 *        dphi_sum/dphi_sample_count is a PLAIN unweighted average, scaled
 *        to Hz (dphi_sum/dphi_sample_count * sample_rate_hz / 2*pi).
 *        CONFIRMED AGAINST REAL HARDWARE NOT to be what an SDR reads as
 *        the carrier's average frequency offset - real two-tone tests
 *        measured near their correct frequency (deviations of Hz, not the
 *        100s of Hz this plain average showed for various tone-pair
 *        sweeps). Kept only as a mechanistic diagnostic (see near_null_*
 *        below) - use env2_dphi_sum/env2_sum instead for anything meant
 *        to predict/match a real spectrum measurement.
 *
 *        env2_dphi_sum/env2_sum give the ENVELOPE^2-WEIGHTED average
 *        instead: (env2_dphi_sum/env2_sum) * sample_rate_hz / 2*pi. This
 *        IS the physically meaningful quantity - for an analytic signal
 *        A(t)e^{jphi(t)}, the power spectrum's centroid equals the
 *        energy-weighted (A(t)^2-weighted) average instantaneous
 *        frequency, a standard identity, not the plain time-average. The
 *        near-null samples that dominate dphi_sum's plain average sit
 *        exactly where envelope (and so envelope^2) is smallest, so this
 *        weighting suppresses almost all of their contribution - matching
 *        why the plain average overstated the real effect so badly.
 *
 *        near_null_dphi_sum/near_null_sample_count restrict the PLAIN
 *        (unweighted) sum to samples where envelope < null_bias_threshold
 *        (see ssb_dsp_set_null_bias_threshold() below) - i.e. samples at
 *        or near a two-tone destructive-interference null. Comparing this
 *        subset's contribution to dphi_sum's overall total is what
 *        localizes the mechanism (confirms it's concentrated at null
 *        crossings) - it does NOT predict on-air impact by itself; for
 *        that, use env2_dphi_sum/env2_sum above.
 *
 *        All four sums reset together, along with
 *        max_unclamped_freq_dev_hz/clip_count, via
 *        ssb_dsp_reset_freq_dev_stats() - same call the 'r' serial command
 *        already makes, so no new wiring needed to start a clean
 *        measurement window.
 */
typedef struct {
    float dphi_sum;                    ///< Sum of dphi (radians) over every sample since last reset.
    uint32_t dphi_sample_count;
    float near_null_dphi_sum;          ///< Same sum, restricted to envelope < null_bias_threshold samples.
    uint32_t near_null_sample_count;
    float env2_dphi_sum;               ///< Sum of envelope^2 * dphi - the physically meaningful,
                                        ///< energy-weighted numerator. Divide by env2_sum for the mean.
    float env2_sum;                    ///< Sum of envelope^2 - the energy-weighted average's denominator.
} ssb_dsp_null_bias_stats_t;

void ssb_dsp_get_null_bias_stats(ssb_dsp_handle_t handle, ssb_dsp_null_bias_stats_t *out);

/**
 * @brief Envelope threshold (roughly [0,1] units, same convention as
 *        out_envelope) below which a sample counts as "near a null" for
 *        ssb_dsp_get_null_bias_stats()'s near_null_* fields. Default
 *        0.05f (~5% of full scale) at init - IRAM_ATTR/volatile-backed
 *        for the same reason as freq_dev_slew_limit_hz: occasional writes
 *        from a command handler, read every sample in the real-time path.
 *        Tune live if near_null_sample_count comes back 0 (threshold too
 *        tight for this signal's actual peak envelope - lower gain or a
 *        two-tone test won't reach a clean I=Q=0 at every discrete sample)
 *        or implausibly large (threshold catching ordinary low-envelope
 *        content, not just genuine nulls).
 */
void IRAM_ATTR ssb_dsp_set_null_bias_threshold(ssb_dsp_handle_t handle, float threshold);
float ssb_dsp_get_null_bias_threshold(ssb_dsp_handle_t handle);

/**
 * @brief Optional per-sample SLEW-RATE limit on freq_dev_hz - distinct
 *        from max_freq_dev_hz above, which limits the VALUE. This limits
 *        how much freq_dev is allowed to CHANGE from one sample to the
 *        next.
 *
 *        Motivation, confirmed numerically (a from-scratch double-
 *        precision reimplementation of this exact algorithm run
 *        side-by-side against the real fast-math code on synthetic
 *        two-tone input): away from any envelope null, real two-tone
 *        content never asks freq_dev to move faster than roughly
 *        60Hz/sample, even on busy high-center-frequency bands. A
 *        genuine destructive-interference null asks for ~8000Hz/sample
 *        in a single step - a 100x+ gap with nothing in between. That
 *        single-sample swing is a real, correctly-computed requirement
 *        (representing the true instantaneous phase reversal at the
 *        null), but a fast transient in frequency is inherently wideband
 *        in the spectrum - this is a way to trade a little reconstruction
 *        fidelity right at the null for a lot less spectral splatter,
 *        without touching ordinary content at all.
 *
 *        Deliberately NOT a freeze - see envelope_floor.h's NOTE 1
 *        postmortem for why that was wrong (built up a "phase debt" that
 *        snapped back as a hard discontinuity). This keeps moving every
 *        sample, just capped in how fast: the AD9851 simply integrates
 *        whatever frequency word it's handed each sample, so a slightly
 *        slower-than-ideal ramp through the null IS the actual applied
 *        modulation, not a deferred correction owed to it later. Also NOT
 *        a hard value clamp - see NOTE 2's postmortem (derivative
 *        discontinuity at every crossing). A slew limiter's output is
 *        continuous by construction, so there's no kink introduced at
 *        any threshold.
 *
 *        Applied in ssb_dsp_process_sample() AFTER max_unclamped_freq_dev_hz
 *        tracks the true (pre-limit) peak, BEFORE the existing
 *        max_freq_dev_hz magnitude clamp - so that diagnostic still
 *        reflects the real, unlimited signal, and the safety clamp still
 *        has final say regardless of this setting.
 *
 *        Off (unlimited) by default/at init, same convention as
 *        'g'/'D'/'x'/'z' - existing tuning isn't disturbed until this is
 *        deliberately dialed in. "Off" is a literal large sentinel value
 *        (SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ) rather than a special-cased
 *        flag - any limit at or above 2x a sane max_freq_dev_hz can never
 *        actually engage anyway (freq_dev itself is bounded to
 *        +/-max_freq_dev_hz), so the sentinel is just "comfortably above
 *        that", not magic. ssb_dsp_raise_freq_dev_slew_limit()/lower()
 *        snap sensibly at both the off end and a practical minimum - see
 *        ssb_dsp.c.
 */
#define SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ  1.0e6f

void IRAM_ATTR ssb_dsp_set_freq_dev_slew_limit_hz(ssb_dsp_handle_t handle, float limit_hz);
float ssb_dsp_get_freq_dev_slew_limit_hz(ssb_dsp_handle_t handle);
void ssb_dsp_raise_freq_dev_slew_limit(ssb_dsp_handle_t handle);  // '}' - loosen (snaps to fully off
                                                                    // past the practical ceiling)
void ssb_dsp_lower_freq_dev_slew_limit(ssb_dsp_handle_t handle);  // '{' - tighten (snaps to a sane
                                                                    // starting point when coming from off)

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

/**
 * @brief Generic first-order digital all-pass section - unity gain at
 *        every frequency, pure phase/group-delay shaping. NOT specific to
 *        SSB modulation - exposed as a reusable primitive for equalizing
 *        an external ANALOG filter's group-delay dispersion (e.g. the
 *        envelope path's PWM -> RC reconstruction filter -> RSET stage),
 *        by cascading two or more sections whose coefficients are fitted
 *        numerically against that specific filter's measured (e.g.
 *        LTspice AC sweep) response. See the caller (e.g.
 *        ssb_mic_test.ino's ENV_GDEQ_A1/A2) for a worked example and the
 *        fitting method.
 *
 *        Transfer function: H(z) = (a + z^-1) / (1 + a*z^-1), |a| < 1 for
 *        stability. Group delay in samples:
 *          tau(w) = (1 - a^2) / (1 + 2*a*cos(w) + a^2)
 *        - flat at exactly 1 sample when a=0 (pure unit delay); rises
 *        with frequency for a>0, falls with frequency for a<0. A single
 *        section can only ever produce a MONOTONIC delay-vs-frequency
 *        curve - cascading two sections of opposite-sign a is what lets
 *        the combined curve have a local min/max in the middle of the
 *        band, needed to cancel a non-monotonic analog filter response
 *        (e.g. a Sallen-Key whose group delay peaks somewhere in-band
 *        and falls off on both sides of that peak).
 *
 *        IMPORTANT: because an all-pass filter can only ADD delay, never
 *        subtract it, using this to flatten a delay curve pushes the
 *        signal's OVERALL (mean) delay up, not just its dispersion. Any
 *        other signal path this one needs to stay time-aligned with
 *        (e.g. the phase/frequency path feeding the same PA) will need
 *        its own relative-delay compensation retuned to match - a
 *        one-time re-tune, not a per-sample concern.
 */
typedef struct {
    float a;           ///< All-pass coefficient, |a| < 1.
    float x1, y1;       ///< Direct-Form-I state: previous input/output.
} ssb_allpass1_t;

/**
 * @brief Set the coefficient and zero the filter's state. Cheap - fine to
 *        call from task context whenever coefficients change (e.g. once
 *        at init), not intended to be called from the per-sample path.
 */
void ssb_allpass1_init(ssb_allpass1_t *f, float a);

/**
 * @brief Zero the filter's state without touching its coefficient - e.g.
 *        when re-enabling after being bypassed, to avoid feeding a stale
 *        x1/y1 pair into the next sample (same reasoning as
 *        ssb_dsp_set_compressor_enabled()'s env reset on re-enable).
 */
void ssb_allpass1_reset(ssb_allpass1_t *f);

/**
 * @brief Process one sample. One-multiply Direct-Form-I realization:
 *        y = x1 + a*(x - y1); x1 = x; y1 = y. IRAM_ATTR/denormal-flushed
 *        the same way as the rest of this file's per-sample path - safe
 *        to call every tick from a real-time task (not ISR-safe: float).
 */
float IRAM_ATTR ssb_allpass1_process(ssb_allpass1_t *f, float x);

#ifdef __cplusplus
}
#endif