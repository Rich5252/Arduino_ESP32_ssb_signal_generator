#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "adc_capture.h"   // adc_lpf_mode_t
#include "ssb_dsp.h"        // SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ
#include "envelope_interp.h"  // envelope_interp_curve_t


// Runtime switch between test signals and live mic input, toggled from
// loop() via serial commands 't' (two-tone)/'s' (single-tone)/'m' (mic) -
// see there. Starts from TWOTONE_TEST_MODE's compile-time value so
// existing behavior is unchanged if you never send a command. dsp_task
// branches on this rather than #if - all code paths are always compiled
// in and the ADC always runs (see init_adc() call in setup(), no longer
// conditional) so switching between any of the three is instant with no
// re-init needed.
typedef enum {
    AUDIO_SRC_MIC = 0,
    AUDIO_SRC_TWOTONE = 1,
    AUDIO_SRC_SINGLETONE = 2,   // clean single tone - isolates the DSP/RF chain from mic-side
                                // confounds (preamp hum, mic nonlinearity, room noise) when
                                // characterizing basic phase-modulation cleanliness, which two-tone's
                                // intermodulation products make harder to read at a glance
    AUDIO_SRC_ENVSTEP = 3,      // slow envelope square wave, bypassing ssb_dsp_process_sample()
                                // entirely (no mic, no Hilbert FIR, freq_dev_hz held at 0) - isolates
                                // JUST the PWM->analog filter->RSET path's own step response, with a
                                // sharp, easy-to-scope-trigger edge, for measuring its group delay
                                // directly rather than eyeballing a subtle two-tone envelope feature
    AUDIO_SRC_FMTEST = 4,       // pure sinusoidal FREQUENCY modulation, ALSO bypassing
                                // ssb_dsp_process_sample() entirely - envelope held at a fixed
                                // constant, freq_dev_hz set directly to a clean single-frequency
                                // sine wave. Isolates the AD9851/SPI/delay-line chain completely
                                // from the Hilbert FIR/DSP math (the opposite isolation from
                                // ENVSTEP, which isolates the envelope/PWM/filter path instead).
                                // Expected result is a textbook FM sideband forest at
                                // fc +/- n*FM_TEST_MOD_HZ with Bessel-function J_n(beta) amplitudes,
                                // beta=FM_TEST_DEV_HZ/FM_TEST_MOD_HZ - any spur that DOESN'T fit
                                // that pattern implicates the AD9851 chain itself, not the DSP math
                                // that both this mode and ENVSTEP deliberately route around.
    AUDIO_SRC_AMTEST = 5,       // mirror image of FMTEST: pure sinusoidal AMPLITUDE modulation,
                                // freq_dev_hz held at exactly 0 (phase/carrier completely fixed,
                                // no FM at all). Isolates the RSET/PWM/analog-filter/transistor
                                // path with a clean, mathematically known AM signal - ideal linear
                                // AM should produce ONLY a single sideband pair at fc+/-AM_TEST_MOD_HZ.
                                // Any additional sidebands/harmonics beyond that pair implicates
                                // nonlinearity specifically in the RSET path (transistor, PWM
                                // quantization, filter); any FM-looking sidebands appearing despite
                                // freq_dev_hz never being nonzero would mean genuine AM-to-PM
                                // crosstalk somewhere physical, a distinct and worth-knowing finding.
    AUDIO_SRC_CHIRP = 6,        // logarithmic sine-chirp sweep (20Hz-20kHz by default, see config.h's
                                // CHIRP_* constants) direct to the envelope/PWM (RSET) output, plus a
                                // synced square-wave reference on CHIRP_REF_GPIO (pin39) - for
                                // characterizing the analog reconstruction filter's transfer function
                                // against an external ADC-based TF measurement rig. Bypasses
                                // ssb_dsp_process_sample/envelope_floor/gdeq/relative_delay/AD9851/
                                // normal diagnostics - more than just ssb_dsp_process_sample() the
                                // way ENVSTEP/FMTEST/AMTEST do - and runs on EVERY fast tick (64kHz),
                                // not just full ticks (16kHz), since a 20kHz chirp needs more than
                                // SAMPLE_RATE_HZ's own 8kHz Nyquist. The offset/scale ('u'/'j'/'i'/'k')
                                // or predistort ('D') DC mapping is deliberately NOT bypassed, though -
                                // unlike master gain (which only scales the swing around a fixed mean,
                                // same as AMTEST), those knobs move WHERE on the duty range the sweep
                                // is centered, which is what's needed to test the analog filter/BS170
                                // gate for duty-range-dependent nonlinearity - see the .ino's dsp_task
                                // chirp block. Appended at the END of this enum (value 6) rather than
                                // inserted near AMTEST above, so no existing settingsPresets[] entry
                                // (which reference these by name, not by value) is disturbed.
} audio_source_t;


// each settings list has all the levers pre-defined for a particular test
typedef struct
{
    const char *name;

    audio_source_t audio_source;

    float relative_delay_samples;

    float env_pwm_offset;
    float env_pwm_scale;

    bool env_gdeq_enable;
    adc_lpf_mode_t adc_lpf_mode;   // was a plain bool "adc_lpf_bypass" - migrated to a 3-state
                                    // enum (off/Butterworth/Chebyshev) when the Chebyshev filter
                                    // option was added; every existing preset had bypass=true,
                                    // so this was a purely mechanical true->ADC_LPF_MODE_OFF
                                    // migration with zero behavior change to any preset below

    bool eq_enable;
    bool compressor_enable;

    float master_gain_db;

    bool ad9851_output_enable;

    // Added when these two per-preset levers were noticed missing from
    // PersistentSettings/the 'P' dump - both existed as live serial
    // toggles ('D', 'x'/'z') for a while before being wired in here.
    // Appended at the END of the struct (rather than inserted near their
    // conceptually-related fields above) so every existing preset's
    // POSITIONAL initializer list below still lines up unchanged - only
    // these two new trailing values needed adding to each.
    bool env_predistort_enable;   // 'D' - envelope pre-distortion LUT (envelope_predistort.h);
                                   // REPLACES env_pwm_offset/env_pwm_scale above while enabled
    float env_floor;              // 'x'/'z' - envelope-null floor (envelope_floor.h), 0.0=off

    // Added when this lever was introduced (freq_dev slew-rate limiter,
    // ssb_dsp.h) - appended at the END for the same reason env_predistort_enable/
    // env_floor were: every existing preset's POSITIONAL initializer list
    // below still lines up unchanged, only this one new trailing value
    // needed adding to each.
    float freq_dev_slew_limit_hz; // '{'/'}' - freq_dev slew-rate limit, Hz/sample (ssb_dsp.h);
                                   // SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ = off

    // Added when this lever was introduced (envelope output interpolation,
    // envelope_interp.h) - appended at the END for the same reason every
    // trailing field above was: every existing preset's POSITIONAL
    // initializer list below still lines up unchanged, only this one new
    // trailing value needed adding to each.
    bool envelope_interp_enable;  // 'I' - ENVELOPE_INTERP_FACTOR-x envelope output
                                   // interpolation (envelope_interp.h), off by default

    // Added when the interp curve became runtime-switchable (envelope_
    // interp.h v4.3, for a direct linear-vs-Catmull-Rom A/B) - appended at
    // the END for the same reason every trailing field above was. Only
    // affects rendered output while envelope_interp_enable is true.
    // CATMULL_ROM is deliberately the enum's 0 value (see envelope_interp.h)
    // so this is safe to leave off any preset literal that predates this
    // field - C's zero-fill of missing trailing initializers lands on
    // today's actual default, not v4's older one.
    envelope_interp_curve_t envelope_interp_curve;  // 'C' - envelope_interp.h

    // Added when this lever was introduced (envelope-path magnitude/
    // insertion-loss equalizer, envelope_ampeq.h) - appended at the END
    // for the same reason every trailing field above was: every existing
    // preset's POSITIONAL initializer list below still lines up
    // unchanged (C zero-fills this to false, matching its off-by-default
    // convention), only this one new trailing value needed adding to
    // presets that want it deliberately on.
    bool env_ampeq_enable;  // 'a' - envelope_ampeq.h, off by default

    // Added 2026-09-04 when ampeq's shelf 1 and shelf 2 were split into
    // independently-toggleable flags (envelope_ampeq.h - real hardware
    // showed shelf1-only and shelf1+shelf2 needed to be A/B-able without a
    // reflash). Appended at the END, same reasoning as env_ampeq_enable
    // above and every trailing field before it - existing presets'
    // positional initializers zero-fill this to false (shelf 2 off),
    // matching env_ampeq_enable's own off-by-default convention and this
    // project's current recommendation (shelf1-only, shelf2 off, pending
    // a gdeq refit - see group_delay_fit_notes.md's 2026-09-04 entries).
    bool env_ampeq_shelf2_enable;  // 'A' - envelope_ampeq.h, off by default

} PersistentSettings;


// -----------------------------------------------------------------------------
// Pre-defined settings
// -----------------------------------------------------------------------------

// relative_delay_samples below was tuned by ear/scope at SAMPLE_RATE_HZ=10000
// and is stored as a raw sample count, not a time - so when SAMPLE_RATE_HZ
// was raised to 16000, every value here was rescaled by 16000/10000=1.6 to
// preserve the same REAL-TIME delay (e.g. 1.9 samples @ 10000Hz = 190us ->
// 3.04 samples @ 16000Hz, still 190us). This is only a first-order
// approximation, for two reasons: (1) it's a straight proportional scale of
// a by-ear/scope-tuned value, not a re-measurement; (2) more importantly,
// envelope_gdeq's own contributed delay is NOT the same fraction of a
// sample at both rates - the 16000Hz-fitted ENV_GDEQ_A1/A2 (see
// envelope_gdeq.h) add ~163us of mean delay vs. the 10000Hz fit's ~265us,
// a real ~102us (~1.6 samples @ 16000Hz) reduction independent of this
// rescale. So for the env_gdeq_enable=true presets specifically, expect the
// true re-tuned optimum to land LOWER (less positive) than the naive x1.6
// value below by roughly that amount - a starting hint for '['/']'
// re-tuning, not a substitute for it. Presets with env_gdeq_enable=false
// aren't affected by that second factor.
//
// 2026-09-01: baseline merged from the user's own live-tuned settings.h -
// every preset below reflects real bench tuning (see group_delay_fit_notes.md's
// 2026-09-01 entries for the IMD/delay-sweep work behind presets 0/5/6
// specifically), not this file's earlier by-ear starting points. Only
// structural addition on top of the user's file: envelope_interp_curve
// (the trailing field above) - every preset explicitly set to
// ENVELOPE_INTERP_CURVE_CATMULL_ROM since none of them have been re-tuned
// against the linear curve yet, so behavior is unchanged from the merged
// baseline until 'C' is used to test that.
static const PersistentSettings settingsPresets[10] =
{
    // Preset 0 - Normal microphone operation

    {"Micr latest 16kFs",    AUDIO_SRC_MIC, 2.48f, 0.36f, 0.48f, true, ADC_LPF_MODE_CHEBYSHEV, true, true, 33.0f, true, false, 0.00f,
    SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ, true, ENVELOPE_INTERP_CURVE_CATMULL_ROM },
    // relative_delay_samples
    // env_pwm_offset
    // env_pwm_scale
    // env_gdeq_enable
    // adc_lpf_mode
    // eq_enable
    // compressor_enable
    // master_gain_db
    // ad9851_output_enable
    // env_predistort_enable
    // env_floor
    // freq_dev_slew_limit_hz
    // envelope_interp_enable
    // envelope_interp_curve

// Preset 1 - Two-tone test
{
"TwoTone Base",
AUDIO_SRC_TWOTONE, 0.00f, 0.20f, 0.90f, false, ADC_LPF_MODE_OFF, false, false, -2.0f, true, false, 0.00f,
SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ, false, ENVELOPE_INTERP_CURVE_CATMULL_ROM },

// Preset 2 - Single-tone test
 { "Shelf2 Baseline pre grp adj#3", AUDIO_SRC_TWOTONE, 2.00f, 0.20f, 0.90f, true, ADC_LPF_MODE_OFF, false, false, -1.4f, true, true, 0.00f, SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ, false, ENVELOPE_INTERP_CURVE_CATMULL_ROM, true, true },

// Preset 3 - Envelope / PWM test
    { "BesselNoGD", AUDIO_SRC_TWOTONE, -0.96f, 0.00f, 0.90f, false, ADC_LPF_MODE_OFF, false, false, 2.0f, true, false, 0.0f, SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ, false, ENVELOPE_INTERP_CURVE_CATMULL_ROM },  // was -0.60f @ 10000Hz; adc_lpf_bypass=true

    // relative_delay_samples
   // env_pwm_offset
   // env_pwm_scale
    // env_gdeq_enable
    // adc_lpf_mode
   // eq_enable
   // compressor_enable
   // master_gain_db
    // ad9851_output_enable
   // env_predistort_enable
    // env_floor
    // freq_dev_slew_limit_hz
    // envelope_interp_enable
    // envelope_interp_curve

// Preset 4 - Diagnostic / raw ADC
{ "AM-ButwGd", AUDIO_SRC_AMTEST, 2.96f, 0.08f, 0.84f, true, ADC_LPF_MODE_OFF, false, false, 1.0f, true, false, 0.0f, SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ, false, ENVELOPE_INTERP_CURVE_CATMULL_ROM },  // was 1.85f @ 10000Hz; adc_lpf_bypass=true

// Preset 5 -
{ "V4 Microphone tuned", AUDIO_SRC_MIC, 3.48f, 0.36f, 0.48f, true, ADC_LPF_MODE_CHEBYSHEV, true, true, 21.3f, true, true, 0.00f, SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ, true, ENVELOPE_INTERP_CURVE_CATMULL_ROM },

// Preset 6 -
{ "V4 Two tone tuned", AUDIO_SRC_TWOTONE, 2.15f, 0.20f, 0.90f, false, ADC_LPF_MODE_OFF, false, false, 0.6f, true, true, 0.00f, SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ, true, ENVELOPE_INTERP_CURVE_CATMULL_ROM },
// Preset 7 -
{ "TwoToneButwGD Env 1.6-2.9", AUDIO_SRC_TWOTONE, 2.96f, 0.40f, 0.46f, true, ADC_LPF_MODE_OFF, false, false, 1.0f, true, false, 0.0f, SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ, false, ENVELOPE_INTERP_CURVE_CATMULL_ROM },  // was 1.85f @ 10000Hz; adc_lpf_bypass=true
// Preset 8 -
{ "FM Env 2.2", AUDIO_SRC_FMTEST, 2.96f, 0.12f, 0.48f, true, ADC_LPF_MODE_OFF, false, false, 0.0f, true, false, 0.0f, SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ, false, ENVELOPE_INTERP_CURVE_CATMULL_ROM },  // was 1.85f @ 10000Hz; adc_lpf_bypass=true
// Preset 9 -
{ "AM Env = 1.6 - 2.9", AUDIO_SRC_AMTEST, 3.04f, 0.28f, 0.70f, true, ADC_LPF_MODE_OFF, true, true, -4.0f, true, false, 0.0f, SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ, false, ENVELOPE_INTERP_CURVE_CATMULL_ROM }  // was 1.90f @ 10000Hz; adc_lpf_bypass=true
};

// If this array's size ever changes, ssb_mic_test.ino's serial handler
// (the `c >= '0' && c <= '9'` preset-select block in loop(), and the
// boot-banner preset listing in setup()) needs its range updated to
// match - this catches a silent mismatch at compile time instead of
// leaking a stale range (missing the new last preset, or indexing past
// the array's end) into a build that otherwise looks fine.
static_assert(sizeof(settingsPresets) / sizeof(settingsPresets[0]) == 10,
              "settingsPresets size changed - update the '0'-'9' range "
              "in ssb_mic_test.ino's loop()/setup()");
