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
    float comp_attack_ms;       ///< Envelope-follower attack time, e.g. 3.0f.
    float comp_release_ms;      ///< Envelope-follower release time, e.g. 120.0f.
    // 2026-09-25: comp_threshold/comp_ratio REMOVED from this config struct -
    // see ssb_dsp_set_compressor_level()'s doc comment below for the full
    // replacement design. Short version: the compressor's threshold is now
    // a fixed internal constant (SSB_DSP_COMP_THRESHOLD in ssb_dsp.c, 0.30f)
    // shared by every calibrated level, not independently configurable -
    // the whole point of the per-level {ratio, makeup} table is that it was
    // measured against ONE specific threshold on real voice data, and would
    // silently go stale (wrong achieved dB, or worse, the safety-margin
    // analysis behind it) if threshold could drift out from under it
    // without the table being recalibrated to match. Compression amount is
    // now selected via ssb_dsp_set_compressor_level() instead of a free
    // threshold/ratio pair.
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
 * @brief 2026-09-25: the switchable-mode compressor (SSB_DSP_COMP_MODE_RATIO
 *        / PEAK_NORMALIZE / LIMIT_ONLY, added and removed again the same
 *        week) is GONE - see moving_forward_notes.md's 2026-09-25 entry for
 *        the full reasoning. Short version: the user's own real mic-to-ADC
 *        recording (G4AHN.wav) showed that ALL of that mode-switching and
 *        curve-shape tuning was calibrated against an unrealistically loud
 *        signal - the real recording's peak barely reached 42% of full
 *        scale, so the compressor's threshold (0.1-0.3 depending which
 *        config you look at) almost never engaged at all. The actual fix
 *        needed a stage upstream of the compressor, not another compressor
 *        mode - see ssb_dsp_set_mic_gain_db() below.
 *
 *        Replacing it: ONE compressor algorithm (a 3-segment piecewise-
 *        linear approximation of a dB-domain/log soft-knee curve, per the
 *        user's explicit request), with a small number of discrete,
 *        integer-dB "compression level" presets instead of free-running
 *        threshold/ratio - see ssb_dsp_set_compressor_level() below.
 */

/**
 * @brief 2026-09-11: NaN/Inf canary for this module's three IIR-style
 *        persistent states (eq_hpf/eq_presence biquad feedback, the
 *        compressor's envelope follower) - see moving_forward_notes.md's
 *        matching entry for the full motivation. Unlike freq_dev_hz/phase
 *        (recomputed fresh from atan2(Q,I) every sample - a bad tick can't
 *        outlive itself), these carry state forward every tick; a NaN/Inf
 *        value here would persist indefinitely once introduced, and
 *        nothing in this file's flush_denorm() calls would catch it (only
 *        near-zero subnormals are). Found and fixed one real "resumes from
 *        stale state" bug in ssb_dsp_set_eq_enabled() while adding this
 *        (see that function) - this canary is the live/ongoing check for
 *        whether that state is EVER actually non-finite, complementing the
 *        code-level fix rather than replacing the need for it.
 *        *_finite reads true (healthy) even while the corresponding stage
 *        is disabled via ssb_dsp_set_eq_enabled()/set_compressor_enabled()
 *        - a disabled stage's state is simply frozen, not being fed
 *        anything, so "still finite" is the correct steady-state answer,
 *        not a false positive.
 */
typedef struct {
    bool eq_hpf_finite;
    bool eq_presence_finite;
    bool compressor_env_finite;
} ssb_dsp_iir_canary_t;

void ssb_dsp_get_iir_canary(ssb_dsp_handle_t handle, ssb_dsp_iir_canary_t *out);

/**
 * @brief Master gain trim, in dB, applied after EQ/compressor (or
 *        directly to the raw sample if audio_fx wasn't enabled at init -
 *        this always works). Deliberately manual rather than automatic:
 *        the compressor's own makeup gain is applied automatically (as of
 *        2026-09-24, tracked dynamically from the signal's own observed
 *        peak level rather than a fixed assumption that it peaks at
 *        0dBFS - see ssb_dsp.c's compressor_t/compressor_process()
 *        comments), but EQ's effect on perceived level depends on the
 *        input spectrum, which isn't something this module can know -
 *        use this to trim it out by ear/scope instead of trusting a
 *        guessed number. IRAM_ATTR: safe to call from the real-time path,
 *        though intended for occasional calls (e.g. a serial '+'/'-'
 *        command), not per-sample.
 */
void IRAM_ATTR ssb_dsp_set_master_gain_db(ssb_dsp_handle_t handle, float gain_db);
float ssb_dsp_get_master_gain_db(ssb_dsp_handle_t handle);

/**
 * @brief 2026-09-25: Mic Gain - a manual trim applied to the raw sample
 *        BEFORE compressor/EQ (mirrors master_gain_db's exact pattern -
 *        human-friendly dB in, precomputed linear multiply in the hot
 *        path - except this one runs at the START of ssb_dsp_process_sample()
 *        instead of the end).
 *
 *        This is the control this project was missing, identified by
 *        researching the FLEX-6600/SmartSDR gain-staging architecture (Mic
 *        Gain -> TX EQ -> Speech Processor -> RF/ALC, Mic Gain manual and
 *        NOT automatic) and then CONFIRMED against the user's own real
 *        mic-to-ADC recording: master_gain_db is applied AFTER the
 *        compressor/EQ, so it was already functioning as "RF Power," not
 *        "Mic Gain" - there was no control at all for the signal's level
 *        going INTO the compressor. Every threshold/ratio number tuned
 *        earlier in this project's life was tuned against whatever level
 *        happened to arrive from the ADC (a fixed analog gain stage,
 *        varying with mic-to-mouth distance and capsule sensitivity) - a
 *        genuinely different level every session, with no way to correct
 *        it before the compressor saw it.
 *
 *        Deliberately manual, same reasoning as master_gain_db: there's no
 *        way for this module to know the mic capsule's sensitivity or the
 *        operator's mic-to-mouth distance, so it can't self-normalize
 *        correctly - set it by ear/scope (or watch the envelope/profile
 *        diagnostics already exposed via the 'V' command) so that ordinary
 *        speech peaks land close to the level the compressor's calibration
 *        assumes (see ssb_dsp_set_compressor_level() below - the per-level
 *        table was derived assuming a peak-normalized input, NOT an
 *        arbitrary raw ADC level). 0.0dB (unity) at init - existing
 *        behavior is unchanged until this is deliberately dialed in.
 *
 *        Applies unconditionally whenever ssb_dsp_process_sample() runs
 *        (real mic input, and also TWOTONE/singletone/chirp test signals,
 *        which already go through this same function) - same scope as the
 *        existing compressor/EQ stage, not gated by audio_fx_configured
 *        (this is useful even with EQ/compressor both disabled, e.g. to
 *        trim a hot ADC input before it reaches the Hilbert transform at
 *        all).
 */
void IRAM_ATTR ssb_dsp_set_mic_gain_db(ssb_dsp_handle_t handle, float gain_db);
float ssb_dsp_get_mic_gain_db(ssb_dsp_handle_t handle);

/**
 * @brief 2026-09-25: Mic Squelch - a genuine "silence it" gate, requested
 *        after ssb_dsp_set_mic_gain_db()'s finer-grained sibling,
 *        envelope_floor.h, turned out (as flagged when that change shipped)
 *        to be structurally incapable of this: envelope_floor RAISES the
 *        bottom of the envelope range and can never reach zero by
 *        construction (see its own header) - it was never going to fix
 *        "spiky/crackly output at low mic input" no matter how finely its
 *        step size was tuned, only reduce how far into a bad predistort
 *        region near-silence could drive. This is the actual gate: below
 *        threshold, transmitted RF power (the envelope) is forced toward
 *        true zero; above it, audio passes through unaffected.
 *
 *        2026-09-26 CORRECTION - WHERE the gate is APPLIED changed, based
 *        on real-hardware feedback. The first cut (below, and still
 *        accurate for everything except the exact application point)
 *        multiplied audio_sample itself, before the Hilbert transform.
 *        On real hardware this was reported as: "the squelch drops to
 *        absolute zero input to dsp that results in no USB noise but only
 *        a small carrier tone" - i.e. LESS pleasant than the noise it
 *        replaced, not more. Root cause: forcing audio_sample to an exact,
 *        bit-constant 0.0 fills the Hilbert delay line with zeros, so
 *        I=Q=0; atan2(0,0) then returns a fixed, degenerate phase value
 *        every sample (confirmed both analytically and with a standalone
 *        test harness - see moving_forward_notes.md's 2026-09-26 entry),
 *        collapsing computed frequency deviation to exactly 0. Any real
 *        analog RF leakage at that "envelope=0" operating point (a common
 *        EER/polar-PA reality from finite switching-PA off-isolation, not
 *        something firmware can necessarily eliminate) then presents as a
 *        discrete, easily audible CW tone rather than as quiet noise.
 *
 *        FIX (per explicit instruction: "keep the hilbert fed with noise
 *        but reduce env to zero") - the gate/gain state machine below is
 *        unchanged, but it no longer touches audio_sample at all.
 *        audio_sample flows into the compressor/EQ/Hilbert path exactly as
 *        it would with squelch disabled, so the Hilbert transform (and the
 *        phase it derives via atan2(Q,I)) always sees the real,
 *        unattenuated signal and never freezes. The resulting smoothed
 *        gain is instead multiplied into the ENVELOPE - computed
 *        downstream, post-Hilbert, as sqrt(I^2+Q^2) - in
 *        ssb_dsp_process_sample(), immediately after that computation.
 *        This is sound specifically because phase is mathematically
 *        scale-invariant to any uniform positive gain
 *        (atan2(k*Q, k*I) == atan2(Q, I) for all k > 0), while envelope
 *        scales exactly as k*sqrt(I^2+Q^2) - so gating envelope alone
 *        gives true RF-power silence (a real, exact zero - not just "very
 *        quiet") without perturbing phase/frequency at all. Net effect:
 *        when the gate is closed, the carrier keeps dithering off genuine
 *        mic noise (turning any residual RF leakage into innocuous
 *        incoherent noise, as it would be with squelch off) instead of
 *        locking to a discrete, audible tone. See squelch_update()'s doc
 *        comment in ssb_dsp.c for the code-level detail. NOT YET
 *        bench-validated against real hardware - the original design's
 *        gate/hysteresis/level-detector behavior WAS confirmed to "work
 *        well" on real hardware (see moving_forward_notes.md), but that
 *        was before this application-point correction.
 *
 *        PLACEMENT - the level DETECTOR taps the sample deliberately
 *        BEFORE the compressor/EQ (right after mic gain, at the very top
 *        of ssb_dsp_process_sample()), not downstream on the demodulated
 *        envelope like envelope_floor/envelope_alc/envelope_softlimit are
 *        - this is about where the gate LOOKS, not (as of the correction
 *        above) where it ACTS. This matters mechanically, not just
 *        stylistically: the compressor's makeup_gain multiplies EVERY
 *        sample unconditionally, including ones below its own threshold
 *        (see compressor_process() in ssb_dsp.c - `gain=1.0` below
 *        threshold, but `x * gain * makeup_gain` still applies makeup
 *        regardless) - so quiet residual noise gets the same fixed dB
 *        boost real speech does. A detector reading downstream of the
 *        compressor would be looking at a signal that's already been
 *        amplified up toward the same operating range as quiet real
 *        speech, making the two hard to tell apart. Reading it here gives
 *        the detector the noise at its true, un-boosted level, before
 *        anything downstream gets a chance to amplify it.
 *
 *        ALGORITHM - three stages, not a single instantaneous compare,
 *        specifically to avoid two failure modes: (1) a single noisy
 *        sample or brief click falsely tripping the gate open, and (2)
 *        the gate itself introducing a new discontinuity/click at every
 *        open or close transition (the exact class of problem
 *        envelope_floor.cpp's own header documents two EARLIER, reverted
 *        designs hitting - a phase freeze and a hard envelope clamp, both
 *        removed after real-hardware testing showed genuine new
 *        distortion). Same "gain-computer + smoothed asymmetric
 *        attack/release" topology envelope_alc.h already uses safely for
 *        a related reason:
 *          a) LEVEL DETECTOR - a one-pole follower on fabsf(audio_sample)
 *             (post mic-gain, pre-compressor), its own attack/release
 *             time constants chosen specifically to reject brief spikes:
 *             at a 5ms attack time constant, one isolated single-sample
 *             (62.5us @ 16kHz) impulse moves the detector by only
 *             ~1-exp(-Ts/tau) =~ 1.25% of the impulse's own amplitude -
 *             an isolated "crackle" click would need to be roughly 80x a
 *             real signal's amplitude to trip this detector by itself,
 *             while genuine sustained speech still registers within a
 *             handful of milliseconds.
 *          b) HYSTERESIS (Schmitt trigger) - opens above `threshold`,
 *             closes only once the detector drops BELOW threshold *
 *             SSB_DSP_SQUELCH_HYSTERESIS_RATIO (a lower, separate close
 *             point) - standard noise-gate practice, prevents rapid
 *             open/close "chatter" for a signal hovering right at a
 *             single threshold value (which, unaddressed, would itself
 *             sound like the exact crackle this feature exists to fix).
 *          c) GAIN RAMP - the resulting open/closed decision drives a
 *             SEPARATE one-pole smoothed gain (fast attack so genuine
 *             speech onsets aren't clipped, slow release so brief
 *             in-word dips don't cause audible chatter). As of the
 *             2026-09-26 correction above, this gain multiplies the
 *             computed ENVELOPE (post-Hilbert), NOT audio_sample - but it
 *             is still continuous by construction, so (unlike the
 *             reverted hard-clamp envelope_floor design) there's no
 *             derivative discontinuity introduced at the threshold
 *             itself, now in the envelope rather than in the sample
 *             stream.
 *
 *        Threshold is in the same full-scale-referenced linear units as
 *        SSB_DSP_COMP_THRESHOLD (0.30) and the ~0.85 mic-gain calibration
 *        target - NOT dB, NOT normalized to compressor_level. Range is
 *        deliberately capped well below the compressor's own 0.30
 *        threshold (SSB_DSP_SQUELCH_THRESHOLD_MAX below) so a
 *        misconfigured squelch can't eat into legitimate quiet speech
 *        that the compressor itself would still treat as normal signal.
 *
 *        Unconditional, independent of audio_fx_configured/comp_enable -
 *        same "always available" reasoning as mic_gain_db/master_gain_db
 *        (this is a mic-input-quality fix, useful whether or not the
 *        compressor is even in use). OFF by default (threshold irrelevant
 *        until enabled) - existing behavior unchanged until deliberately
 *        opted into, same convention as every other toggle in this file.
 *
 *        NOT YET BENCH-VALIDATED - first cut, built from the documented
 *        failure modes of three EARLIER related designs (envelope_floor's
 *        two reverted attempts, envelope_alc's proven-safe topology) but
 *        not itself measured against the actual reported crackle yet. The
 *        specific attack/release/hysteresis numbers are reasoned starting
 *        points, not fitted/measured optima - same epistemic status this
 *        project gives every other "first cut" feature (see
 *        envelope_alc.h's own status section for the pattern this
 *        follows).
 */
#define SSB_DSP_SQUELCH_THRESHOLD_MIN   0.0f
#define SSB_DSP_SQUELCH_THRESHOLD_MAX   0.10f   // well below SSB_DSP_COMP_THRESHOLD (0.30) -
                                                  // see doc comment above for why

void IRAM_ATTR ssb_dsp_set_squelch_enabled(ssb_dsp_handle_t handle, bool enable);
bool ssb_dsp_get_squelch_enabled(ssb_dsp_handle_t handle);
void IRAM_ATTR ssb_dsp_set_squelch_threshold(ssb_dsp_handle_t handle, float threshold);
float ssb_dsp_get_squelch_threshold(ssb_dsp_handle_t handle);

/**
 * @brief 2026-09-25: Compression Level - replaces free-running
 *        threshold/ratio (and the whole switchable-mode idea above) with a
 *        small set of discrete, integer-dB presets: 0 (SSB_DSP_COMP_LEVEL_MIN,
 *        a plain limiter, no boost) through 10 (SSB_DSP_COMP_LEVEL_MAX,
 *        the most aggressive preset shipped). Values outside that range are
 *        clamped, not rejected.
 *
 *        WHAT EACH LEVEL MEANS: dB of measured RMS/power gain on the
 *        user's OWN real voice (G4AHN.wav), assuming mic gain (see
 *        ssb_dsp_set_mic_gain_db() above) has already been set so the
 *        input peaks around the same level (0.85, roughly -1.4dBFS) the
 *        calibration itself was normalized to. "Level 6" was chosen,
 *        measured, and verified to deliver close to +6.0dB of real RMS
 *        gain on that actual recording - not a guessed number, and not an
 *        abstract knob position. Levels 7-9 continue the same +1dB-per-step
 *        pattern (+7, +8, +9dB nominal). Level 10 is NOT +10dB: at this
 *        threshold (0.30, shared/fixed across all levels) and on this
 *        recording's actual statistics, +10dB and even +9dB of RMS gain are
 *        PHYSICALLY UNREACHABLE by any ratio, however extreme - as
 *        ratio->infinity the curve degenerates to a hard limiter fixed at
 *        the threshold, and even that true hard-clip asymptote only
 *        achieves ~+8.5dB measured RMS gain on G4AHN.wav (confirmed
 *        2026-09-25: an initial calibration attempt targeting literal
 *        +9dB/+10dB just saturated the search's ratio ceiling and produced
 *        two IDENTICAL, mislabeled entries - see moving_forward_notes.md's
 *        2026-09-25 entry for the full investigation). Level 10 is
 *        therefore calibrated to a genuinely achievable +8.35dB instead,
 *        distinct from level 9's +8.1dB and level 8's +7.6dB - the last
 *        three steps compress together more tightly than the +1dB/step
 *        pattern below them because they're approaching that hard physical
 *        ceiling, not because of a calibration shortcut.
 *
 *        HISTORICAL NOTE: an earlier revision of this same calibration
 *        pass found a project listening test judged +10dB (as it existed
 *        under the OLD free-running scheme, not this table) to already
 *        audibly hurt intelligibility, and shipped only levels 0-6 as a
 *        result. This 2026-09-25 extension to 0-10 was requested
 *        explicitly by the user with awareness that plain "level 10" no
 *        longer means a clean +10dB - it means "the most aggressive
 *        setting this threshold/recording combination can actually
 *        deliver, ~+8.5dB, sounding more clipped/distorted than levels
 *        7-8 for only marginal extra loudness." No new listening test has
 *        been run to re-confirm intelligibility at levels 7-10; treat them
 *        as numerically verified but NOT YET subjectively validated.
 *
 *        If mic gain is set differently than 0.85 peak, or the input
 *        material has different statistics than G4AHN.wav (different
 *        voice, different mic, music, noise), the ACTUAL achieved dB will
 *        differ from the label - the label is a calibrated estimate for
 *        this one real recording, not a live measurement or a guarantee.
 *
 *        LEVEL 0 IS SPECIAL-CASED, not just "the bottom of the same curve
 *        family evaluated at 0dB": searching for a ratio that hits exactly
 *        0dB of restored-peak RMS gain naturally converges to ratio=1 (no
 *        compression at all, verified numerically during calibration) -
 *        that's a bypass, not "a simple audio side limiter" as requested.
 *        Level 0 instead fixes makeup_gain at EXACTLY 1.0 (no restoration,
 *        no boost, ever) paired with a moderately firm ratio chosen to
 *        genuinely catch real overshoot - measured on the real recording,
 *        this comes out to a very slight (a fraction of a dB) RMS
 *        REDUCTION, which is the correct, expected behavior for a limiter:
 *        it only ever takes away, never adds.
 *
 *        HOW THE CURVE/TABLE WORKS (the implementation, not just the
 *        knob): a fixed, shared threshold (SSB_DSP_COMP_THRESHOLD in
 *        ssb_dsp.c, 0.30f - NOT independently configurable, see
 *        ssb_audio_fx_config_t's comment) and four fixed, shared breakpoint
 *        x-locations feed a 3-segment piecewise-linear approximation of a
 *        dB-domain (log) soft-knee curve, per the user's explicit request
 *        ("replace the original with a 3 segment log curve"). Each level's
 *        {ratio, makeup_gain} pair is a CONSTANT baked in at build time
 *        from an offline Python calibration against G4AHN.wav (bisecting
 *        ratio to hit each target dB, computing makeup once as the exact
 *        peak-restoring value at that ratio, then verifying the result on
 *        the real recording) - see moving_forward_notes.md's 2026-09-25
 *        entry for the full table and methodology. Calling this function
 *        only recomputes the 4 breakpoint y-values from that level's ratio
 *        (log10f/powf - fine here, this is a rare, human-triggered event,
 *        never called per-sample) and copies in the fixed makeup_gain -
 *        it does NOT touch env or re-run any search at runtime.
 *
 *        WHY A FIXED, PRE-CALIBRATED CONSTANT rather than the
 *        peak-tracking dynamic makeup gain the 2026-09-24 RATIO/
 *        PEAK_NORMALIZE modes used: that scheme's peak_env follower has a
 *        real, measured lag - a fast attack (matching env's own attack, by
 *        design) still can't react to a transient inside its own attack
 *        window, and during calibration on the REAL recording this showed
 *        up as genuine full-scale overshoot (measured peaks over 1.0,
 *        1.79 at the most aggressive tested level) that a "worst-case
 *        steady-state envelope" safety analysis alone did NOT predict. A
 *        fixed makeup constant doesn't remove that mechanism (the
 *        envelope-based GAIN decision still has the same attack lag - see
 *        compressor_process() in ssb_dsp.c), but it does remove one
 *        moving part's worth of surprise, and the real fix for the
 *        transient-overshoot mechanism is the hard output clamp
 *        ssb_dsp_process_sample() now applies after this whole stage (see
 *        that function) - a genuine safety net, confirmed necessary by
 *        this exact measurement, not a theoretical nicety.
 *
 *        NOT YET BENCH-VALIDATED on real hardware - this whole design is
 *        built and numerically verified against ONE offline recording.
 */
#define SSB_DSP_COMP_LEVEL_MIN  0
#define SSB_DSP_COMP_LEVEL_MAX  10

void IRAM_ATTR ssb_dsp_set_compressor_level(ssb_dsp_handle_t handle, int level_db);
int ssb_dsp_get_compressor_level(ssb_dsp_handle_t handle);

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
float IRAM_ATTR ssb_dsp_get_null_bias_threshold(ssb_dsp_handle_t handle);

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

/**
 * @brief Generic second-order (biquad) high-shelf filter - RBJ Audio EQ
 *        Cookbook formula, S=1 (the standard "no bump/dip in the
 *        transition band" shelf slope). NOT specific to SSB modulation -
 *        exposed as a reusable primitive for the SAME kind of job
 *        ssb_allpass1_t does (equalizing an external ANALOG filter's
 *        measured response), except this one corrects MAGNITUDE instead
 *        of phase/delay. Unlike ssb_allpass1_t, this is NOT unity-gain -
 *
 *        NOTE: named ssb_shelf_biquad_t, NOT ssb_biquad_t - ssb_adc_filter.h
 *        already defines a DIFFERENT ssb_biquad_t (Direct-Form-II-
 *        Transposed, ADC low-pass use) that would otherwise collide at
 *        link time (both .c files are compiled into the same sketch) -
 *        confirmed the hard way via a "multiple definition" linker error
 *        when this was first added under the ssb_biquad_t name.
 *        that's the whole point: it approaches 0dB below its corner and a
 *        fixed plateau gain (positive or negative) well above it, with a
 *        smooth transition in between. See the caller (e.g.
 *        ssb_mic_test.ino's ENV_AMPEQ_SHELF_FREQ_HZ/GAIN_DB) for a worked
 *        example - a deliberately CAPPED/PARTIAL correction for the
 *        envelope path's analog reconstruction filter's high-frequency
 *        insertion loss, not a full inverse response (see
 *        group_delay_fit_notes.md's 2026-09-03 insertion-loss entry for
 *        why a full inverse was judged too risky to try first).
 *
 *        Reuses the same normalized-coefficient Direct-Form-I structure
 *        (and the same one-time-division-at-init tradeoff) as the private
 *        peaking-EQ biquad in ssb_dsp.c's own pre-Hilbert audio_fx chain -
 *        this is a separate, public instance of that same well-known RBJ
 *        cookbook math, not a refactor of the private one (kept private
 *        to avoid touching working, already-shipped code for an unrelated
 *        feature).
 */
typedef struct {
    float b0, b1, b2, a1, a2;   ///< Normalized Direct-Form-I coefficients (a0 already divided out).
    float x1, x2, y1, y2;       ///< Direct-Form-I state: previous two inputs/outputs.
} ssb_shelf_biquad_t;

/**
 * @brief Set up a high-shelf biquad and zero its state. fc/fs pick the
 *        shelf's corner (roughly where the response crosses half the
 *        plateau gain, in dB), gain_db is the plateau gain reached well
 *        above fc (positive = boost, negative = cut, 0.0f = flat/no-op).
 *        Cheap - fine to call from task context at init, not intended to
 *        be called from the per-sample path.
 */
void ssb_shelf_biquad_set_highshelf(ssb_shelf_biquad_t *f, float fc, float fs, float gain_db);

/**
 * @brief Zero the filter's state without touching its coefficients - same
 *        reset-on-re-enable reasoning as ssb_allpass1_reset().
 */
void ssb_shelf_biquad_reset(ssb_shelf_biquad_t *f);

/**
 * @brief Process one sample. Standard Direct-Form-I biquad:
 *        y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2, then shift the delay
 *        lines. IRAM_ATTR/denormal-flushed the same way as
 *        ssb_allpass1_process() - safe to call every tick from a
 *        real-time task (not ISR-safe: float).
 */
float IRAM_ATTR ssb_shelf_biquad_process(ssb_shelf_biquad_t *f, float x);

#ifdef __cplusplus
}
#endif