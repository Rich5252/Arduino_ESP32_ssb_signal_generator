#pragma once

/**
 * dsp_state.h
 *
 * Owns the core ssb_dsp handle and the small set of "which mode are we in,
 * what's the current gain" state that used to be a handful of static
 * globals near the top of ssb_mic_test.ino (s_ssb, s_sideband,
 * s_audio_source, s_master_gain_linear_cache, audio_source_name()).
 * Everything else (module-private state) lives in its own module now - see
 * adc_capture.h, test_signals.h, envelope_gdeq.h, relative_delay.h,
 * carrier_output.h, envelope_output.h, diagnostics.h.
 */

#include "ssb_dsp.h"
#include "settings.h"

// Creates the ssb_dsp instance from the given config and stores the handle
// here for every other module to read via dsp_state_get_ssb(). Call once
// from setup(), after filling in an ssb_dsp_config_t exactly like before.
esp_err_t dsp_state_init(const ssb_dsp_config_t *cfg);

ssb_dsp_handle_t IRAM_ATTR dsp_state_get_ssb(void);

ssb_sideband_t IRAM_ATTR dsp_state_get_sideband(void);

audio_source_t IRAM_ATTR dsp_state_get_audio_source(void);
void dsp_state_set_audio_source(audio_source_t src);

// Sets master gain on the ssb_dsp handle AND updates the cached linear
// value in one call - keeps the two in sync the same way every '+'/'-'
// and preset-load call site in the original .ino did by hand.
void dsp_state_set_master_gain_db(float db);
float IRAM_ATTR dsp_state_get_master_gain_linear(void);

// Takes plain int, not audio_source_t - preserved from the original .ino,
// where this mattered because Arduino auto-generates function prototypes
// and inserts them at the very top of the translation unit, before any of
// the .ino's own typedefs were visible; a custom enum type in the
// signature broke that auto-generated prototype. That specific constraint
// no longer applies now that this function lives in its own .cpp (Arduino
// doesn't auto-prototype non-.ino files), but every call site already
// passes an audio_source_t value or an int-compared source variable, so
// this is left as int to keep the change purely mechanical.
const char *audio_source_name(int src);
