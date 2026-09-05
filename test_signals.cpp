/**
 * test_signals.cpp - see test_signals.h.
 */

#include <math.h>
#include "test_signals.h"
#include "config.h"
#include "envelope_interp.h"   // ENVELOPE_INTERP_FACTOR - the chirp's fast-tick rate

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
// Kept at a consistent 200Hz spacing throughout (except the last entry,
// see below) so each band probes a clean, comparable single point on the
// delay-vs-frequency curve - a wider-spaced pair averages alignment
// across its whole span instead of reading one point, which is exactly
// why the very first sweep (using the old 700/1900Hz default, 1200Hz
// spacing) came back as a clear outlier (2.00 samples) against the other
// four bands' tight 1.50-1.55 sample cluster. That wide pair is kept as
// the LAST entry for reference/comparison rather than mixed into the main
// ordered-by-frequency sweep. Cycling order is low to high center
// frequency: 400/800/1600/2600/3600Hz, then the wide legacy pair.
static const twotone_band_t TWOTONE_BAND_PRESETS[] = {
    { "300/500 (low)",           300.0f,  500.0f },
    { "700/900",                 700.0f,  900.0f },
    { "1500/1700 (mid)",        1500.0f, 1700.0f },
    { "2500/2700 (mid-high)",   2500.0f, 2700.0f },
    { "3500/3700 (high)",       3500.0f, 3700.0f },
    { "700/1900 (wide, legacy default)", TWOTONE_F1_HZ, TWOTONE_F2_HZ },
};
#define TWOTONE_BAND_COUNT (sizeof(TWOTONE_BAND_PRESETS) / sizeof(TWOTONE_BAND_PRESETS[0]))
// Starts on the last (wide legacy) entry so the first 'T' press after boot
// wraps around to index 0 (300/500Hz, the low end) - i.e. the first press
// begins the ordered sweep rather than re-landing on the boot default.
static int s_band_index = TWOTONE_BAND_COUNT - 1;

float test_signals_get_twotone_f1_hz(void) { return s_tone1_hz; }
float test_signals_get_twotone_f2_hz(void) { return s_tone2_hz; }

const char* test_signals_next_twotone_band(void)
{
    s_band_index = (s_band_index + 1) % TWOTONE_BAND_COUNT;
    s_tone1_hz = TWOTONE_BAND_PRESETS[s_band_index].f1_hz;
    s_tone2_hz = TWOTONE_BAND_PRESETS[s_band_index].f2_hz;
    return TWOTONE_BAND_PRESETS[s_band_index].name;
}

// ---- 2026-09-04: runtime-adjustable tone2/tone1 amplitude ratio ('R') ----
// See test_signals.h for why this exists. 2026-09-04, later same day:
// range narrowed to a symmetric +/-3dB (was 0 to -20dB, tone2-down-only) -
// real-hardware testing with the original range already confirmed the
// amplitude-mismatch hypothesis, and separately clarified that 'eq's own
// presence peak BOOSTS tone2 (the upper tone) rather than attenuating it
// - the down-only version could only test the opposite direction from
// what 'eq' actually does. Now spans both directions. To keep every step
// directly comparable in overall drive level, and to make a boost
// direction safe, tone1+tone2's combined constructive-interference peak
// is held CONSTANT at today's existing 2*TWOTONE_AMPLITUDE (0.9) for
// EVERY ratio - only the SPLIT between the two tones changes (e.g. equal
// splits 0.45/0.45; +3dB splits ~0.373/0.527, tone2 louder; -3dB splits
// ~0.527/0.373, tone2 quieter) - rather than a fixed tone1 plus a
// multiplier on tone2, which is what let the old down-only version cycle
// safely but would have pushed the peak past 1.0 in the boost direction.
typedef struct { const char *name; float ratio_db; } tone_ratio_t;
static const tone_ratio_t TONE_RATIO_PRESETS[] = {
    { "-3dB (tone2 quieter)",                             -3.0f },
    { "-2dB",                                              -2.0f },
    { "-1dB",                                              -1.0f },
    { "equal (0dB, today's default)",                       0.0f },
    { "+1dB",                                               1.0f },
    { "+2dB",                                               2.0f },
    { "+3dB (tone2 louder - matches eq's own direction)",   3.0f },
};
#define TONE_RATIO_COUNT (sizeof(TONE_RATIO_PRESETS) / sizeof(TONE_RATIO_PRESETS[0]))
#define TONE_RATIO_EQUAL_INDEX 3   // must stay pointed at the 0dB entry above

static int s_tone_ratio_index = TONE_RATIO_EQUAL_INDEX;
// Both initialized directly to TWOTONE_AMPLITUDE (rather than derived from
// the table via a startup call) so boot behavior is bit-for-bit identical
// to before 'R' existed until 'R' is actually pressed - this exactly
// equals what the 0dB preset's own formula produces anyway
// (peak_budget/(1+1) = 2*TWOTONE_AMPLITUDE/2 = TWOTONE_AMPLITUDE), so
// there's no discontinuity the first time 'R' IS pressed either.
static volatile float s_tone1_amplitude = TWOTONE_AMPLITUDE;
static volatile float s_tone2_amplitude = TWOTONE_AMPLITUDE;

// Ratio (tone2/tone1, linear) currently in effect - derived from the two
// amplitudes actually in use rather than cached separately, so it can
// never drift out of sync with what generate_twotone_sample() is doing.
float test_signals_get_tone2_gain(void) { return s_tone2_amplitude / s_tone1_amplitude; }

const char* test_signals_next_tone_ratio(void)
{
    s_tone_ratio_index = (s_tone_ratio_index + 1) % TONE_RATIO_COUNT;
    float ratio_db = TONE_RATIO_PRESETS[s_tone_ratio_index].ratio_db;
    float r = powf(10.0f, ratio_db / 20.0f);            // tone2/tone1, linear
    const float peak_budget = 2.0f * TWOTONE_AMPLITUDE;  // constant across every ratio
    float a1 = peak_budget / (1.0f + r);
    s_tone1_amplitude = a1;
    s_tone2_amplitude = r * a1;
    return TONE_RATIO_PRESETS[s_tone_ratio_index].name;
}

float IRAM_ATTR generate_twotone_sample(void)
{
    const float two_pi = 2.0f * (float)M_PI;
    float sample = s_tone1_amplitude * sinf(s_tone1_phase) +
                   s_tone2_amplitude * sinf(s_tone2_phase);
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

// ---- Sine-chirp test mode ('w', AUDIO_SRC_CHIRP) - see test_signals.h. ----
//
// File-scope (not function-local) statics, unlike every generator above -
// this one needs an explicit reset (test_signals_chirp_reset()) callable
// from OUTSIDE this function (serial_commands.cpp's 'w' handler), so a
// fresh entry into chirp mode always starts a clean sweep from t=0 rather
// than resuming mid-sweep from a previous session.
static float s_chirp_phase = 0.0f;        // envelope sine's own phase accumulator, radians
static float s_chirp_elapsed_s = 0.0f;    // time within the current CHIRP_MUTE_SEC+CHIRP_SWEEP_SEC cycle

void test_signals_chirp_reset(void)
{
    s_chirp_phase = 0.0f;
    s_chirp_elapsed_s = 0.0f;
}

void IRAM_ATTR test_signals_generate_chirp(float master_gain_linear, float *out_envelope, bool *out_ref_high)
{
    // Runs at the FULL fast-tick rate (ENVELOPE_INTERP_FACTOR x
    // SAMPLE_RATE_HZ = 64kHz, not the normal 16kHz full-tick rate) - see
    // the .ino's dsp_task for the early-intercept call site that makes
    // that true. Computed fresh here every call rather than cached, since
    // it's cheap (one integer multiply) next to the sinf/powf below.
    const float fs_fast = (float)SAMPLE_RATE_HZ * (float)ENVELOPE_INTERP_FACTOR;
    const float two_pi = 2.0f * (float)M_PI;

    if (s_chirp_elapsed_s < CHIRP_MUTE_SEC) {
        // Brief silence at the start of every cycle - a clean, easy-to-
        // trigger-on marker for the external measurement rig to detect
        // "sweep restarting here" without needing any other sync signal.
        // Reference square wave forced low too, so BOTH channels the rig
        // reads show the same unambiguous marker.
        *out_envelope = 0.0f;
        *out_ref_high = false;
        // Phase deliberately NOT advanced during mute - the sweep always
        // begins its first post-mute sample at exactly phase=0, so every
        // repeat of the sweep is bit-for-bit identical, same reasoning as
        // why the null-bias investigation's test tones repeat identically
        // (see null_bias_investigation.md) - here that's a feature, not a
        // confound, since a repeatable stimulus is exactly what a transfer-
        // function measurement wants.
    } else {
        float t_sweep = s_chirp_elapsed_s - CHIRP_MUTE_SEC;   // 0 at sweep start
        if (t_sweep > CHIRP_SWEEP_SEC) t_sweep = CHIRP_SWEEP_SEC;   // clamp the last fractional tick before wrap

        // Logarithmic (exponential) sweep: f(t) = f0 * (f1/f0)^(t/T) -
        // instantaneous frequency, integrated into a phase accumulator
        // per-sample rather than using the sweep's closed-form phase
        // integral, since the per-sample instantaneous-frequency approach
        // is simpler to get right and cheap enough at this rate (one powf
        // per fast tick, ~64k/sec - negligible next to the DSP budget the
        // full 16kHz pipeline already spends per tick).
        float f_inst = CHIRP_F0_HZ * powf(CHIRP_F1_HZ / CHIRP_F0_HZ, t_sweep / CHIRP_SWEEP_SEC);

        s_chirp_phase += two_pi * f_inst / fs_fast;
        if (s_chirp_phase > two_pi) s_chirp_phase -= two_pi;

        // Same AM_TEST_DEPTH convention test_signals_generate_amtest() uses:
        // fixed mean (carrier amplitude) so '+'/'-' master gain scales only
        // the swing, not the baseline - keeps depth and overall level as
        // separate, independently-readable knobs on a scope/analyzer.
        float s = sinf(s_chirp_phase);
        *out_envelope = AM_TEST_DEPTH + AM_TEST_DEPTH * master_gain_linear * s;
        *out_ref_high = (s >= 0.0f);
    }

    s_chirp_elapsed_s += 1.0f / fs_fast;
    if (s_chirp_elapsed_s >= CHIRP_MUTE_SEC + CHIRP_SWEEP_SEC) {
        s_chirp_elapsed_s = 0.0f;
        s_chirp_phase = 0.0f;   // resync phase too, so every repeat sweep is identical (see above)
    }
}
