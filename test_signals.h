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
// plus a synced square-wave reference bit for CHIRP_REF_GPIO (pin13) - for
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
