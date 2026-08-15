/**
 * test_signals.cpp - see test_signals.h.
 */

#include <math.h>
#include "test_signals.h"
#include "config.h"

// Always compiled in now (not gated on TWOTONE_TEST_MODE) since dsp_task
// branches on the runtime audio-source selector and can switch to this at
// any time via the 't' serial command.
static float s_tone1_phase = 0.0f;
static float s_tone2_phase = 0.0f;

// Runtime-adjustable pair (see test_signals.h) - starts at the
// config.h defaults so behavior is unchanged until 'T' is sent. volatile
// for the same reason relative_delay.cpp's s_relative_delay_samples is:
// written occasionally from the serial-command context, read every
// sample from generate_twotone_sample()'s hot path.
static volatile float s_tone1_hz = TWOTONE_F1_HZ;
static volatile float s_tone2_hz = TWOTONE_F2_HZ;

typedef struct { const char *name; float f1_hz; float f2_hz; } twotone_band_t;

// Spread across roughly the same ~100-4300Hz band envelope_gdeq was
// originally fit against (see envelope_gdeq.h) - a single tone-pair only
// tells you whether delay is right AT that pair's spacing; only a sweep
// across several bands reveals the SHAPE of a frequency-dependent
// envelope/phase delay mismatch, which a bulk relative-delay shift alone
// can never fully correct (it can only slide the curve, not reshape it -
// see the group-delay-equalizer refit discussion this was added for).
// Index 1 (700/1900Hz) is the default, matching TWOTONE_F1_HZ/F2_HZ.
static const twotone_band_t TWOTONE_BAND_PRESETS[] = {
    { "300/500 (low)",        300.0f,  500.0f },
    { "700/1900 (default)",   TWOTONE_F1_HZ, TWOTONE_F2_HZ },
    { "1500/1700 (mid)",      1500.0f, 1700.0f },
    { "2500/2700 (mid-high)", 2500.0f, 2700.0f },
    { "3500/3700 (high)",     3500.0f, 3700.0f },
};
#define TWOTONE_BAND_COUNT (sizeof(TWOTONE_BAND_PRESETS) / sizeof(TWOTONE_BAND_PRESETS[0]))
static int s_band_index = 1;   // starts on the 700/1900Hz entry above

float test_signals_get_twotone_f1_hz(void) { return s_tone1_hz; }
float test_signals_get_twotone_f2_hz(void) { return s_tone2_hz; }

const char* test_signals_next_twotone_band(void)
{
    s_band_index = (s_band_index + 1) % TWOTONE_BAND_COUNT;
    s_tone1_hz = TWOTONE_BAND_PRESETS[s_band_index].f1_hz;
    s_tone2_hz = TWOTONE_BAND_PRESETS[s_band_index].f2_hz;
    return TWOTONE_BAND_PRESETS[s_band_index].name;
}

float IRAM_ATTR generate_twotone_sample(void)
{
    const float two_pi = 2.0f * (float)M_PI;
    float sample = TWOTONE_AMPLITUDE * sinf(s_tone1_phase) +
                   TWOTONE_AMPLITUDE * sinf(s_tone2_phase);
    s_tone1_phase += two_pi * s_tone1_hz / (float)SAMPLE_RATE_HZ;
    s_tone2_phase += two_pi * s_tone2_hz / (float)SAMPLE_RATE_HZ;
    if (s_tone1_phase > two_pi) s_tone1_phase -= two_pi;
    if (s_tone2_phase > two_pi) s_tone2_phase -= two_pi;
    return sample;
}

// Single, clean tone - isolates the DSP/RF chain (Hilbert, phase/freq
// modulation, AD9851 output) from mic-side confounds like preamp hum or
// room noise when characterizing basic sideband suppression/splatter -
// easier to read on a spectrum analyser than two-tone's own IMD products
// when THAT'S not what you're trying to measure. Switch to via 's'.
static float s_singletone_phase = 0.0f;

float IRAM_ATTR generate_singletone_sample(void)
{
    const float two_pi = 2.0f * (float)M_PI;
    float sample = SINGLETONE_AMPLITUDE * sinf(s_singletone_phase);
    s_singletone_phase += two_pi * SINGLETONE_HZ / (float)SAMPLE_RATE_HZ;
    if (s_singletone_phase > two_pi) s_singletone_phase -= two_pi;
    return sample;
}

float IRAM_ATTR test_signals_generate_envstep(float master_gain_linear)
{
    // Direct square wave, bypassing ssb_dsp_process_sample() entirely -
    // no Hilbert FIR, no atan2/sqrt, no mic - isolates JUST the
    // PWM->analog filter->RSET path's own step response for measuring its
    // group delay directly on a scope, rather than trying to read it off
    // a subtle two-tone envelope feature. freq_dev_hz stays 0 (handled by
    // the caller not touching it) - carrier held constant, no phase
    // modulation while this mode is active. Amplitude scaled by master
    // gain so '+'/'-' has an effect here too, same as it does on real
    // signal paths.
    static float s_envstep_phase = 0.0f;
    float envelope = (s_envstep_phase < 0.5f) ? 0.0f : master_gain_linear;
    s_envstep_phase += ENVSTEP_HZ / (float)SAMPLE_RATE_HZ;
    if (s_envstep_phase >= 1.0f) s_envstep_phase -= 1.0f;
    return envelope;
}

void IRAM_ATTR test_signals_generate_fmtest(float *out_freq_dev_hz, float *out_envelope)
{
    // Direct sinusoidal frequency modulation, ALSO bypassing
    // ssb_dsp_process_sample() entirely - the mirror-image isolation test
    // to ENVSTEP: this exercises JUST the AD9851/SPI/delay-line chain
    // with a clean, mathematically known FM signal, with no Hilbert
    // FIR/atan2/sqrt involved at all. envelope held fixed (constant
    // drive, no AM) so only the phase/frequency path is under test.
    // Expected result is a textbook FM sideband forest at
    // fc +/- n*FM_TEST_MOD_HZ - see settings.h's audio_source_t comment
    // for the diagnostic logic.
    static float s_fmtest_phase = 0.0f;
    const float two_pi = 2.0f * (float)M_PI;
    *out_freq_dev_hz = FM_TEST_DEV_HZ * sinf(s_fmtest_phase);
    s_fmtest_phase += two_pi * FM_TEST_MOD_HZ / (float)SAMPLE_RATE_HZ;
    if (s_fmtest_phase > two_pi) s_fmtest_phase -= two_pi;
    *out_envelope = 1.0f;   // fixed, full-scale - no AM content, phase path only
}

void IRAM_ATTR test_signals_generate_amtest(float master_gain_linear, float *out_envelope, float *out_freq_dev_hz)
{
    // Direct sinusoidal amplitude modulation, ALSO bypassing
    // ssb_dsp_process_sample() entirely - mirror image of FMTEST:
    // exercises JUST the RSET/PWM/analog-filter/transistor path with a
    // clean, mathematically known AM signal. freq_dev_hz stays exactly 0
    // - phase/carrier held completely fixed, no FM at all. Ideal linear
    // AM should produce only a single sideband pair at
    // fc +/- AM_TEST_MOD_HZ - see settings.h's audio_source_t comment for
    // the diagnostic logic. Envelope still goes through the SAME PWM
    // offset/scale mapping as real operation (applied by dsp_task after
    // this returns), so results are directly comparable to real testing.
    // Mean (carrier amplitude, i.e. AM_TEST_DEPTH itself) stays FIXED
    // regardless of gain - only the SWING around that mean (modulation
    // depth) scales with master gain. Without this split, gain would move
    // both together, muddying "is this testing depth or overall level" -
    // '+'/'-' maps cleanly onto depth alone, and the PWM offset knob
    // remains the control for carrier amplitude, matching how those two
    // knobs are conceptually separate in the real signal chain. Above 0dB
    // the swing can still push peaks past 1.0, which the PWM clamp
    // downstream then flattens - a deliberate way to probe the RSET
    // path's saturation behavior, not a bug.
    static float s_amtest_phase = 0.0f;
    const float two_pi = 2.0f * (float)M_PI;
    float swing = AM_TEST_DEPTH * master_gain_linear;
    *out_envelope = AM_TEST_DEPTH + swing * sinf(s_amtest_phase);
    s_amtest_phase += two_pi * AM_TEST_MOD_HZ / (float)SAMPLE_RATE_HZ;
    if (s_amtest_phase > two_pi) s_amtest_phase -= two_pi;
    *out_freq_dev_hz = 0.0f;   // carrier held completely fixed - AM content only
}
