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
#include "esp_attr.h"

// Two-tone (700/1900Hz default) and single-tone (1000Hz default) sample
// generators - selected via 't'/'s', consumed by ssb_dsp_process_sample()
// exactly like a mic sample would be.
float IRAM_ATTR generate_twotone_sample(void);
float IRAM_ATTR generate_singletone_sample(void);

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
