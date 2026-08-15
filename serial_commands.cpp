/**
 * serial_commands.cpp - see serial_commands.h.
 */

#include "serial_commands.h"
#include "config.h"
#include "dsp_state.h"
#include "adc_capture.h"
#include "envelope_gdeq.h"
#include "envelope_output.h"
#include "diagnostics.h"
#include "settings.h"
#include "ssb_dsp.h"
#include "test_signals.h"
#if AD9851_ATTACHED
#include "relative_delay.h"
#include "carrier_output.h"
#endif
#include <Arduino.h>

// Returns the C symbol name (e.g. "AUDIO_SRC_MIC") for a given
// audio_source_t, for the 'P' settings-dump command below - distinct from
// dsp_state.h's audio_source_name(), which returns a human-readable label
// ("mic", "TWO-TONE TEST", ...) that isn't valid C and can't be pasted
// into a PersistentSettings initializer.
static const char *audio_source_enum_name(audio_source_t src)
{
    switch (src) {
        case AUDIO_SRC_MIC:        return "AUDIO_SRC_MIC";
        case AUDIO_SRC_TWOTONE:    return "AUDIO_SRC_TWOTONE";
        case AUDIO_SRC_SINGLETONE: return "AUDIO_SRC_SINGLETONE";
        case AUDIO_SRC_ENVSTEP:    return "AUDIO_SRC_ENVSTEP";
        case AUDIO_SRC_FMTEST:     return "AUDIO_SRC_FMTEST";
        case AUDIO_SRC_AMTEST:     return "AUDIO_SRC_AMTEST";
        default:                   return "AUDIO_SRC_MIC";
    }
}

void handle_serial_commands(void)
{
    // Runtime source switch: 't' -> two-tone, 's' -> single-tone, 'm' ->
    // live mic, 'p'/'y'/'h' -> isolation tests. dsp_task reads the audio
    // source fresh every tick, so this takes effect on the very next
    // sample - no glitch/restart needed. Unrecognized bytes (e.g. line
    // endings from some serial monitors) are silently ignored rather than
    // echoed as an error.
    while (Serial.available()) {
        char c = Serial.read();
        audio_source_t src = dsp_state_get_audio_source();

        if (c == 't' && src != AUDIO_SRC_TWOTONE) {
            dsp_state_set_audio_source(AUDIO_SRC_TWOTONE);
            Serial.println("-> two-tone test signal");
        } else if (c == 's' && src != AUDIO_SRC_SINGLETONE) {
            dsp_state_set_audio_source(AUDIO_SRC_SINGLETONE);
            Serial.printf("-> single-tone test signal (%.0fHz)\r\n", SINGLETONE_HZ);
        } else if (c == 'm' && src != AUDIO_SRC_MIC) {
            dsp_state_set_audio_source(AUDIO_SRC_MIC);
            Serial.println("-> live mic input");
        } else if (c == 'p' && src != AUDIO_SRC_ENVSTEP) {
            dsp_state_set_audio_source(AUDIO_SRC_ENVSTEP);
            Serial.printf("-> envelope step test (%.1fHz square wave, carrier fixed - "
                          "measure the RSET node's rise/settling time against this edge)\r\n", ENVSTEP_HZ);
        } else if (c == 'y' && src != AUDIO_SRC_FMTEST) {
            dsp_state_set_audio_source(AUDIO_SRC_FMTEST);
            Serial.printf("-> FM isolation test (%.0fHz sine mod, %.0fHz peak deviation, beta=%.2f - "
                          "expect FM sidebands at fc+/-n*%.0fHz, no envelope content - "
                          "isolates AD9851/SPI chain from Hilbert/DSP math)\r\n",
                          FM_TEST_MOD_HZ, FM_TEST_DEV_HZ, FM_TEST_DEV_HZ/FM_TEST_MOD_HZ, FM_TEST_MOD_HZ);
        } else if (c == 'h' && src != AUDIO_SRC_AMTEST) {
            dsp_state_set_audio_source(AUDIO_SRC_AMTEST);
            Serial.printf("-> AM isolation test (%.0fHz sine mod, %.0f%% depth, carrier fixed - "
                          "expect ONLY fc+/-%.0fHz sideband pair, no FM content - "
                          "isolates RSET/PWM/filter path from AD9851/DSP)\r\n",
                          AM_TEST_MOD_HZ, AM_TEST_DEPTH * 200.0f, AM_TEST_MOD_HZ);
        } else if (c == 'T') {
            // Steps the two-tone pair through TWOTONE_BAND_PRESETS
            // (test_signals.cpp) - lets you sweep the pair across the
            // audio band without a recompile, to empirically map how much
            // relative-delay retuning each band needs (see the
            // group-delay-equalizer refit discussion). Switches into
            // two-tone mode too if not already there, so 'T' alone is
            // enough to start a sweep.
            bool was_twotone = (src == AUDIO_SRC_TWOTONE);
            const char *band_name = test_signals_next_twotone_band();
            if (!was_twotone) {
                dsp_state_set_audio_source(AUDIO_SRC_TWOTONE);
            }
            Serial.printf("-> two-tone band: %s (f1=%.0fHz f2=%.0fHz)%s - "
                          "re-tune relative delay ('['/']') for this band before capturing\r\n",
                          band_name, test_signals_get_twotone_f1_hz(), test_signals_get_twotone_f2_hz(),
                          was_twotone ? "" : ", two-tone mode enabled");
#if AD9851_ATTACHED
        } else if (c == ']') {
            relative_delay_increase();
            float d = relative_delay_get_samples();
            Serial.printf("-> relative delay %+.2f samples (~%+.0fus) - %s\r\n",
                          d, d * 1000000.0f / SAMPLE_RATE_HZ,
                          d > 0.0f ? "phase held back" :
                          d < 0.0f ? "envelope held back" : "aligned");
        } else if (c == '[') {
            relative_delay_decrease();
            float d = relative_delay_get_samples();
            Serial.printf("-> relative delay %+.2f samples (~%+.0fus) - %s\r\n",
                          d, d * 1000000.0f / SAMPLE_RATE_HZ,
                          d > 0.0f ? "phase held back" :
                          d < 0.0f ? "envelope held back" : "aligned");
#endif
        } else if (c == 'u') {
            envelope_output_raise_pwm_offset();
            float off = envelope_output_get_pwm_offset(), scale = envelope_output_get_pwm_scale();
            Serial.printf("-> PWM duty range %.0f%%-%.0f%% (offset=%.2f, scale=%.2f)\r\n",
                          off * 100.0f, (off + scale > 1.0f ? 1.0f : off + scale) * 100.0f, off, scale);
        } else if (c == 'j') {
            envelope_output_lower_pwm_offset();
            float off = envelope_output_get_pwm_offset(), scale = envelope_output_get_pwm_scale();
            Serial.printf("-> PWM duty range %.0f%%-%.0f%% (offset=%.2f, scale=%.2f)\r\n",
                          off * 100.0f, (off + scale > 1.0f ? 1.0f : off + scale) * 100.0f, off, scale);
        } else if (c == 'i') {
            envelope_output_widen_pwm_scale();
            float off = envelope_output_get_pwm_offset(), scale = envelope_output_get_pwm_scale();
            Serial.printf("-> PWM duty range %.0f%%-%.0f%% (offset=%.2f, scale=%.2f)\r\n",
                          off * 100.0f, (off + scale > 1.0f ? 1.0f : off + scale) * 100.0f, off, scale);
        } else if (c == 'k') {
            envelope_output_narrow_pwm_scale();
            float off = envelope_output_get_pwm_offset(), scale = envelope_output_get_pwm_scale();
            Serial.printf("-> PWM duty range %.0f%%-%.0f%% (offset=%.2f, scale=%.2f)\r\n",
                          off * 100.0f, (off + scale > 1.0f ? 1.0f : off + scale) * 100.0f, off, scale);
        } else if (c == 'g') {
            bool now_on = !envelope_gdeq_get_enabled();
            envelope_gdeq_set_enabled(now_on);   // internally resets state on an off->on transition
            Serial.printf("-> envelope group-delay equalizer %s%s\r\n", now_on ? "ON" : "off",
                          now_on ? " - re-tune relative delay ('['/']') from scratch, "
                                   "theoretical starting point ~+2.65 samples (see envelope_gdeq.h)" : "");
        } else if (c == 'f') {
            bool bypass = !adc_capture_get_lpf_bypass();
            adc_capture_set_lpf_bypass(bypass);
            Serial.printf("-> ADC LPF %s\r\n", bypass ? "BYPASSED (raw)" : "active");
        } else if (c == 'v') {
            diagnostics_toggle_muted();
        } else if (c == 'r') {
            // Resets every diagnostic counter/watermark for a clean
            // measurement window, without needing a full reflash. Useful
            // after switching modes (e.g. 't' then 'm') so counters like
            // drop_total/starve_ticks_total reflect only what happens
            // AFTER the reset, not history carried over from a different
            // mode or an earlier test run in the same boot. Touches every
            // module that keeps its own diagnostic counters, same as the
            // original inline handler touched every counter directly.
            diagnostics_reset();
            adc_capture_reset_diag();
            ssb_dsp_reset_freq_dev_stats(dsp_state_get_ssb());
            Serial.println("-> diagnostics reset, clean window starting now");
        } else if (c == 'e') {
            bool now_on = !ssb_dsp_get_eq_enabled(dsp_state_get_ssb());
            ssb_dsp_set_eq_enabled(dsp_state_get_ssb(), now_on);
            Serial.printf("-> EQ (HPF+presence) %s\r\n", now_on ? "ON" : "off");
        } else if (c == 'c') {
            bool now_on = !ssb_dsp_get_compressor_enabled(dsp_state_get_ssb());
            ssb_dsp_set_compressor_enabled(dsp_state_get_ssb(), now_on);
            Serial.printf("-> compressor %s\r\n", now_on ? "ON" : "off");
        } else if (c == '+') {
            float new_gain = ssb_dsp_get_master_gain_db(dsp_state_get_ssb()) + MASTER_GAIN_STEP_DB;
            dsp_state_set_master_gain_db(new_gain);
            Serial.printf("-> master gain %+.1f dB\r\n", new_gain);
        } else if (c == '-') {
            float new_gain = ssb_dsp_get_master_gain_db(dsp_state_get_ssb()) - MASTER_GAIN_STEP_DB;
            dsp_state_set_master_gain_db(new_gain);
            Serial.printf("-> master gain %+.1f dB\r\n", new_gain);
#if AD9851_ATTACHED
        } else if (c == 'o') {
            bool now_on = !carrier_output_get_rf_enabled();
            carrier_output_set_rf_enabled(now_on);
            Serial.printf("-> AD9851 RF output %s\r\n", now_on ? "ON" : "off (powered down)");
#endif
        } else if (c == 'P') {
            // Prints every current lever as a single comma-separated line,
            // in exactly PersistentSettings's field order (name,
            // audio_source, relative_delay_samples, env_pwm_offset,
            // env_pwm_scale, env_gdeq_enable, adc_lpf_bypass, eq_enable,
            // compressor_enable, master_gain_db, ad9851_output_enable) -
            // wrapped in braces with a trailing comma so the whole line
            // can be pasted directly into settingsPresets[] in settings.h
            // as a new preset entry. Rename "Live" (and add a numbered
            // comment above it, matching the existing presets' style)
            // after pasting - and remember settings.h's static_assert
            // ties the array size to the '0'-'4' range in this file, so
            // adding a 6th preset needs that range widened too (see the
            // static_assert's own comment in settings.h).
#if AD9851_ATTACHED
            float rel_delay = relative_delay_get_samples();
            bool rf_enabled = carrier_output_get_rf_enabled();
#else
            // No relative-delay line or AD9851 output to read without
            // AD9851_ATTACHED - placeholders matching every existing
            // preset's own default (0 delay wasn't any preset's default,
            // but it's the only sane placeholder absent a real value;
            // true for ad9851_output_enable matches all 5 current presets).
            float rel_delay = 0.0f;
            bool rf_enabled = true;
#endif
            Serial.println("-> settings line (paste into settingsPresets[] in settings.h, then rename \"Live\"):");
            Serial.printf("    { \"Live\", %s, %.2ff, %.2ff, %.2ff, %s, %s, %s, %s, %.1ff, %s },\r\n",
                          audio_source_enum_name(dsp_state_get_audio_source()),
                          rel_delay,
                          envelope_output_get_pwm_offset(),
                          envelope_output_get_pwm_scale(),
                          envelope_gdeq_get_enabled() ? "true" : "false",
                          adc_capture_get_lpf_bypass() ? "true" : "false",
                          ssb_dsp_get_eq_enabled(dsp_state_get_ssb()) ? "true" : "false",
                          ssb_dsp_get_compressor_enabled(dsp_state_get_ssb()) ? "true" : "false",
                          ssb_dsp_get_master_gain_db(dsp_state_get_ssb()),
                          rf_enabled ? "true" : "false");
        } else if (c >= '0' && c <= '4') {
            int preset = c - '0';
            const PersistentSettings& p = settingsPresets[preset];

            dsp_state_set_audio_source(p.audio_source);
#if AD9851_ATTACHED
            // relative_delay only exists under AD9851_ATTACHED (see
            // relative_delay.h) - guarded the same way here, so this
            // still compiles standalone (mic->DSP->DAC, no AD9851 board
            // yet) per the AD9851_ATTACHED toggle documented in config.h,
            // not just in the currently-built configuration.
            relative_delay_set_samples(p.relative_delay_samples);
#endif
            envelope_output_set_pwm_offset(p.env_pwm_offset);
            envelope_output_set_pwm_scale(p.env_pwm_scale);
            // Same reset-on-enable reasoning as the 'g' handler - now
            // shared via envelope_gdeq_set_enabled() itself, so it only
            // has to be correct in one place: only reset state on an
            // off->on transition, not on every preset load (all 5 presets
            // currently have gdeq on, so switching between them keeps the
            // filter running continuously, not jumping every time).
            envelope_gdeq_set_enabled(p.env_gdeq_enable);
            adc_capture_set_lpf_bypass(p.adc_lpf_bypass);

            ssb_dsp_set_eq_enabled(dsp_state_get_ssb(), p.eq_enable);
            ssb_dsp_set_compressor_enabled(dsp_state_get_ssb(), p.compressor_enable);
            dsp_state_set_master_gain_db(p.master_gain_db);

#if AD9851_ATTACHED
            carrier_output_set_rf_enabled(p.ad9851_output_enable);
#endif

            Serial.printf("-> preset %d: %s\r\n", preset, p.name);
        }
    }
}
