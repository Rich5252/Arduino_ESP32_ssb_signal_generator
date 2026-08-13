#pragma once

/**
 * serial_commands.h
 *
 * The single-character serial command handler (signal-source switches,
 * relative-delay/PWM-range/gdeq tuning knobs, EQ/compressor/gain, RF
 * output, diagnostics reset/mute, and preset loading) - moved out of
 * loop() as-is. See ssb_mic_test_commands.md for the full user-facing
 * reference of what each key does.
 */

// Drains Serial.available() and dispatches every recognized character,
// exactly like the original inline while(Serial.available()) block in
// loop(). Call once per loop() iteration.
void handle_serial_commands(void);
