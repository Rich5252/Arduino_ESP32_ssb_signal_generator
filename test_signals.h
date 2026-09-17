#pragma once

/**
 * test_signals.h
 *
 * Synthesized test signals: the always-available TWOTONE/SINGLETONE audio
 * sources (used as sample-domain input to ssb_dsp_process_sample(), same
 * as a mic sample would be), and the three isolation-test generators
 * (ENVSTEP/FMTEST/AMTEST) that bypass ssb_dsp_process_sample() entirely
 * and hand dsp_task pre-made {freq_dev_hz, envelope} values directly - see
 * settings.h's audio_source_t for what each mode is for.
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_attr.h"

// Two-tone (700/1900Hz default) and single-tone (1000Hz default) sample
// generators - selected via 't'/'s', consumed by ssb_dsp_process_sample()
// exactly like a mic sample would be.
float IRAM_ATTR generate_twotone_sample(void);
float IRAM_ATTR generate_singletone_sample(void);

// Runtime-adjustable two-tone frequency pair, for sweeping the pair across
// different parts of the audio band without a recompile/reflash per band -
// see TWOTONE_BAND_PRESETS in test_signals.cpp and the 'T' serial command.
// Starts at TWOTONE_F1_HZ/F2_HZ (config.h, 700/1900Hz) regardless of
// where that pair sits in TWOTONE_BAND_PRESETS, so 't' behaves exactly
// as before if 'T' is never sent.
float test_signals_get_twotone_f1_hz(void);
float test_signals_get_twotone_f2_hz(void);

// Advances to the next entry in TWOTONE_BAND_PRESETS (wrapping around),
// applies it to generate_twotone_sample()'s frequencies, and returns its
// display name for the caller (serial_commands.cpp's 'T' handler) to
// print. Intended workflow: 'T' to pick a band, '['/']' to re-tune
// relative delay for that band, capture a spectrum, repeat - building up
// an empirical delay-vs-frequency curve across the band without any lab
// equipment beyond the RF spectrum analyzer already in use (see the
// group-delay-equalizer refit discussion this was added for).
const char* test_signals_next_twotone_band(void);

// Runtime-adjustable amplitude ratio between the two tones - tone2 (the
// upper of the pair) scaled relative to tone1. Added 2026-09-04 to test
// directly whether it's specifically the AMPLITUDE MISMATCH between the
// two tones - not 'eq's HPF/presence side effects, not tone timing - that
// keeps the envelope from reaching a true zero at a destructive-
// interference null and thereby avoids freq_dev's sign-flip discontinuity
// there (see ssb_mic_test_commands.md's "Is 'eq's IMD benefit the
// highpass or the presence boost?" section, 2026-09-03 "resolved:
// amplitude" entry, which this follows up on). Independent of 'e'/eq and
// of 'a'/'A'/'g' - combine or compare freely. Starts at equal amplitude
// (today's unchanged default) so 'R' must be pressed at least once to
// introduce any mismatch.
//
// 2026-09-04, later same day: confirmed on the bench and range narrowed
// to a symmetric +/-3dB (was 0 to -20dB, tone2-down-only) - see
// TONE_RATIO_PRESETS in test_signals.cpp for the exact 1dB-step cycle in
// both directions. Both directions are covered because 'eq's own presence
// peak turned out to BOOST tone2 (not attenuate it) - the combined
// tone1+tone2 constructive-interference peak is held constant across
// every ratio (see test_signals.cpp), so boosting tone2 carries no more
// clipping risk than attenuating it did.
float test_signals_get_tone2_gain(void);
const char* test_signals_next_tone_ratio(void);

// Two-tone null-uncertainty dither ('Q', 2026-09-11) - see test_signals.cpp
// for the full rationale (null_bias_investigation.md's "Root mechanism
// identified" section). In short: these test tones are exact phase-
// accumulator multiples of SAMPLE_RATE_HZ, so every destructive-
// interference null in a run recurs at an IDENTICAL alignment to the
// sample grid - whatever tiny dphi-resolution bias one null produces,
// every null produces identically, so it accumulates coherently in
// null_bias/null_bias2 instead of averaging out the way real voice's
// randomly-timed nulls apparently do. This is a DIFFERENT, narrower
// target than the "theoretically infinite phase bandwidth at the origin"
// problem group_delay_fit_notes.md researched (2026-09-03) - that one has
// no dithering precedent in the EER/polar literature and needs an
// upstream I/Q trajectory fix instead; THIS is the two-tone TEST's own
// measurement-repeatability artifact, where "break an exact coincidental
// periodicity with a little dither" is a well-precedented DSP technique
// (same idea as ADC dither breaking limit cycles/idle tones).
//
// Off by default (TWOTONE_DITHER_ENABLED, config.h) so plain 't'/'T'/'R'
// behavior is bit-for-bit unchanged unless 'Q' is explicitly pressed.
// When on, adds a small (+/-TWOTONE_DITHER_MAX_HZ), continuously and
// slowly varying frequency offset to tone2 ONLY - tone1 is left exactly
// at its nominal frequency as an undithered reference, so the pair's
// spacing still reads close to nominal at a glance. The offset is large
// enough, over a several-second integration window, to walk every null's
// position relative to the sample grid all the way around, but far too
// small/slow to show up as its own resolvable line on a spectrum analyzer
// at normal two-tone dwell times.
//
// UNTESTED ON REAL HARDWARE. Validation plan: compare `[dsp] null_bias2`
// (weighted_bias) behavior across several 'r' resets on the SAME preset,
// with 'Q' off vs. on - looking for the biased constant to turn into a
// value that scatters/trends toward zero given a long enough window.
// Important caveat from null_bias_investigation.md's 2026-09-09 update:
// weighted_bias itself was found to drift for TENS OF SECONDS TO MINUTES
// after a reset even with absolutely nothing changed - so this comparison
// needs a long, logged dwell (or better, the full time series), not a
// quick point-read a second or two after 'r', or you'll be comparing
// noise to noise rather than seeing what 'Q' actually does.
//
// 2026-09-17 update: a SEPARATE investigation (the whole-second warble
// hunt, see null_bias_investigation.md) independently proved that this
// same exact-phase-accumulator tone generation forces the ENTIRE
// downstream ssb_dsp_process_sample() output (freq_dev, envelope,
// everything) to be exactly, bit-for-bit periodic at exactly 1.000s
// (16000 samples) for ANY two-tone frequency pair - confirmed both by an
// independent from-scratch Python/float32 simulation of the real
// algorithm and by real-hardware 'F' captures (two independent captures
// agreeing to 40us, then a single continuous 2.0s capture splitting into
// two zero-diff halves). This is the SAME underlying cause described
// above (exact phase-accumulator tone generation -> exact recurrence),
// just observed at a coarser (whole-second) timescale instead of the
// finer (per-null) timescale this dither was originally built for. A
// follow-up simulation (sim_dither_test.py, scratchpad-only, not
// committed to this repo) confirmed the EXISTING dither parameters below
// - completely unchanged, no retuning needed - already destroy this
// whole-second exact repetition: cycle-to-cycle max freq_dev difference
// jumps from 0.0Hz (undithered) to ~14677Hz (dithered), with roughly
// 1732/16000 samples changing by more than 50Hz between consecutive
// 1-second cycles.
//
// 2026-09-17, same day, real-hardware test: enabling 'Q' was tried on
// the bench and produced a SUBJECTIVELY NOISIER result, not a cleaner
// one - the opposite of what the periodicity-breaking simulation above
// would suggest. Working theory (not yet confirmed): dither only
// decorrelates WHEN/how often a given near-null glitch recurs - it does
// nothing to reduce the glitches' own magnitude (still the same
// ~5000-9000Hz single-sample freq_dev spikes documented elsewhere in
// null_bias_investigation.md). Before, those spikes landed at the same
// point every 1.000s cycle, so they were heard as one discrete, coherent
// periodic buzz; with dither on, the same total spike energy is spread
// essentially at random across the whole run, which likely reads to the
// ear as broadband "noisier" content rather than as an improvement, even
// though the coherent periodic tone is indeed gone. If that theory holds,
// dither (whether applied here to tone2's frequency, or - per the
// 2026-09-17 "should the fast trig itself be randomized" discussion - to
// the fast_atan2/fast_sqrt computation) is very unlikely to be the right
// fix for the underlying problem, because it only redistributes the
// error rather than reducing it. The existing freq_dev slew-rate limiter
// (ssb_dsp_set_freq_dev_slew_limit_hz(), '{'/'}', off by default) directly
// caps the SIZE of each sample-to-sample freq_dev jump instead, and its
// own doc comment already notes its starting value is "comfortably below
// a null event's ~8000Hz/sample, so it actually engages only where
// intended" - i.e. it was apparently built with exactly this glitch
// class in mind. See null_bias_investigation.md's 2026-09-17 entry for
// the open question of whether the slew limiter (not dither) is the
// right next thing to test. NOT resolved as of this writing - treat the
// dither mechanism described above as informative about how the test
// tones behave, not as a recommended fix.
bool test_signals_get_twotone_dither_enabled(void);
void test_signals_set_twotone_dither_enabled(bool enable);

// Legacy two-tone phase generator toggle ('O', 2026-09-17) - lets the OLD
// accumulate-and-subtract phase generator (`phase += two_pi*f/Fs; if
// (phase > two_pi) phase -= two_pi;`) be switched back in for direct A/B
// comparison against the exact-recompute-from-sample-index generator that
// replaced it earlier the same day (see generate_twotone_sample()'s own
// 2026-09-17 comment, and null_bias_investigation.md's matching entry, for
// the full story: the old accumulator never resets its own float32
// rounding error, giving both tones a small but real, steadily GROWING
// frequency error - confirmed by simulation at roughly +/-2e-4Hz over an
// 8M-tick/500s run - which the exact recompute eliminates entirely).
//
// Added not because the old scheme is considered better (it isn't - the
// new one is strictly more correct), but because the user specifically
// wants to be able to revert to the "pure but jumping" tone behavior on
// demand as a sanity-check reference point, without a separate reflash,
// given how much investigation was already anchored to that older
// behavior. Off by default (matches the exact-recompute code that shipped
// today) so plain 't'/'T'/'R'/'Q' behavior is completely unchanged unless
// 'O' is explicitly pressed.
//
// When ON: tone1 always uses the old accumulator; tone2 uses it too
// whenever dither ('Q') is off (dither's own tone2 path already used the
// accumulator both before and after the NCO fix - see
// generate_twotone_sample() - so 'O' has no effect on tone2 while 'Q' is
// on). Toggling 'O' mid-run can cause one small, one-time phase
// discontinuity in whichever tone(s) switch generators, on the same
// tick the switch happens - same accepted-as-negligible convention as
// 'Q's own on/off transition and a 'T' band change already carry.
bool test_signals_get_twotone_legacy_phase_enabled(void);
void test_signals_set_twotone_legacy_phase_enabled(bool enable);

// Envelope step test ('p') - slow square wave direct to the envelope
// output, carrier held fixed, bypassing ssb_dsp_process_sample()
// entirely. master_gain_linear is dsp_state_get_master_gain_linear() -
// passed in rather than read directly to keep this module decoupled from
// dsp_state (this mode bypasses ssb_dsp itself, where master gain
// normally applies, so without this explicit scaling '+'/'-' would be
// inert here).
float IRAM_ATTR test_signals_generate_envstep(float master_gain_linear);

// FM isolation test ('y') - pure sinusoidal frequency modulation, envelope
// held at a fixed full-scale constant, also bypassing
// ssb_dsp_process_sample() entirely.
void IRAM_ATTR test_signals_generate_fmtest(float *out_freq_dev_hz, float *out_envelope);

// AM isolation test ('h') - pure sinusoidal amplitude modulation,
// freq_dev_hz held at exactly 0, also bypassing ssb_dsp_process_sample()
// entirely. master_gain_linear scales the SWING only (see the .cpp for
// why the mean/carrier-amplitude term deliberately does not scale with
// gain).
void IRAM_ATTR test_signals_generate_amtest(float master_gain_linear, float *out_envelope, float *out_freq_dev_hz);

// Sine-chirp test mode ('w', AUDIO_SRC_CHIRP) - logarithmic sweep from
// CHIRP_F0_HZ to CHIRP_F1_HZ (config.h) direct to the envelope output,
// plus a synced square-wave reference bit for CHIRP_REF_GPIO (pin39) - for
// characterizing the analog envelope/PWM reconstruction filter's transfer
// function against an external ADC-based measurement rig. Unlike
// ENVSTEP/FMTEST/AMTEST above, this is called on EVERY fast tick (the full
// ENVELOPE_INTERP_FACTOR x SAMPLE_RATE_HZ rate), not just full ticks - see
// the .ino's dsp_task for the early-intercept call site. master_gain_linear
// scales the swing only, matching AMTEST's own gain convention.
// out_ref_high is the instantaneous sign of the chirp's own sine (true
// while the sine is >= 0), forced false during the CHIRP_MUTE_SEC silence
// at each sweep restart - the measurement rig can use it both as a phase
// reference and as a restart/sync marker.
void IRAM_ATTR test_signals_generate_chirp(float master_gain_linear, float *out_envelope, bool *out_ref_high);

// Zeroes the chirp's phase/elapsed-time state so a fresh 'w' entry always
// starts a clean sweep from t=0 (mute period first) rather than resuming
// wherever a PREVIOUS chirp session left off - same reset-on-entry
// convention as envelope_gdeq_set_enabled()/envelope_interp_set_enabled()
// use for their own off->on transitions. Call once when switching INTO
// AUDIO_SRC_CHIRP (see serial_commands.cpp's 'w' handler).
void test_signals_chirp_reset(void);
