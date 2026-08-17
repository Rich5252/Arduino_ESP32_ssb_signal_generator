#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "adc_capture.h"   // adc_lpf_mode_t


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
static const PersistentSettings settingsPresets[10] =
{
    // Preset 0 - Normal microphone operation
    {
        "Micr",
        AUDIO_SRC_MIC,     // audio_source
        3.04f,               // relative_delay_samples (was 1.9f @ 10000Hz)
        0.32f,               // env_pwm_offset
        0.8f,               // env_pwm_scale
        true,               // env_gdeq_enable
        ADC_LPF_MODE_OFF,   // adc_lpf_mode (was adc_lpf_bypass=true)
        true,               // eq_enable
        true,               // compressor_enable
        -2.0f,               // master_gain_db
        true,               // ad9851_output_enable
        false,              // env_predistort_enable
        0.0f                // env_floor
    },

    // Preset 1 - Two-tone test
    {
        "TwoToneButwGD",
        AUDIO_SRC_TWOTONE,
        2.96f,               // relative_delay_samples (was 1.85f @ 10000Hz)
        0.08f,               // env_pwm_offset
        0.84f,               // env_pwm_scale
        true,                // env_gdeq_enable
        ADC_LPF_MODE_OFF,    // adc_lpf_mode (was adc_lpf_bypass=true)
        false,               // eq_enable
        false,               // compressor_enable
        +1.0f,               // master_gain_db
        true,               // ad9851_output_enable
        false,              // env_predistort_enable
        0.0f                // env_floor
    },

    // Preset 2 - Single-tone test
    {
        "TwoToneButwNoGD",
        AUDIO_SRC_TWOTONE,
        -0.96f,               // relative_delay_samples (was -0.6f @ 10000Hz)
        0.04f,               // env_pwm_offset
        0.82f,               // env_pwm_scale
        false,                // env_gdeq_enable
        ADC_LPF_MODE_OFF,    // adc_lpf_mode (was adc_lpf_bypass=true)
        false,               // eq_enable
        false,               // compressor_enable
        +1.0f,               // master_gain_db
        true,               // ad9851_output_enable
        false,              // env_predistort_enable
        0.0f                // env_floor
    },

    // Preset 3 - Envelope / PWM test
        { "BesselNoGD", AUDIO_SRC_TWOTONE, -0.96f, 0.00f, 0.90f, false, ADC_LPF_MODE_OFF, false, false, 2.0f, true, false, 0.0f },  // was -0.60f @ 10000Hz; adc_lpf_bypass=true

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

    // Preset 4 - Diagnostic / raw ADC
        { "AM-ButwGd", AUDIO_SRC_AMTEST, 2.96f, 0.08f, 0.84f, true, ADC_LPF_MODE_OFF, false, false, 1.0f, true, false, 0.0f },  // was 1.85f @ 10000Hz; adc_lpf_bypass=true

    // Preset 5 -
    { "TwoToneButwGD Env 1.6-2.9", AUDIO_SRC_TWOTONE, 2.48f, 0.40f, 0.42f, true, ADC_LPF_MODE_OFF, false, false, 1.0f, true, false, 0.0f },  // was 1.55f @ 10000Hz; adc_lpf_bypass=true

        // Preset 6 -
    { "TwoToneButwGD Env 1.6-2.9 DelayTuned", AUDIO_SRC_TWOTONE, 2.64f, 0.36f, 0.48f, true, ADC_LPF_MODE_OFF, false, false, 1.0f, true, false, 0.0f },  // was 1.65f @ 10000Hz; adc_lpf_bypass=true
        // Preset 7 -
    { "TwoToneButwGD Env 1.6-2.9", AUDIO_SRC_TWOTONE, 2.96f, 0.40f, 0.46f, true, ADC_LPF_MODE_OFF, false, false, 1.0f, true, false, 0.0f },  // was 1.85f @ 10000Hz; adc_lpf_bypass=true
        // Preset 8 -
    { "FM Env 2.2", AUDIO_SRC_FMTEST, 2.96f, 0.12f, 0.48f, true, ADC_LPF_MODE_OFF, false, false, 0.0f, true, false, 0.0f },  // was 1.85f @ 10000Hz; adc_lpf_bypass=true
        // Preset 9 -
    { "AM Env = 1.6 - 2.9", AUDIO_SRC_AMTEST, 3.04f, 0.28f, 0.70f, true, ADC_LPF_MODE_OFF, true, true, -4.0f, true, false, 0.0f }  // was 1.90f @ 10000Hz; adc_lpf_bypass=true
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
