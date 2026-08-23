#pragma once

/**
 * envelope_output.h
 *
 * Everything that turns the computed envelope into an actual analog
 * signal: the LEDC/RC "PWM comparison" path (RSET modulation via a
 * BS170 gate) and the MCP4725 I2C DAC path (currently disconnected
 * hardware, kept for side-by-side comparison), plus the envelope-to-PWM-
 * duty range mapping knobs ('u'/'j'/'i'/'k').
 *
 * Why a DAC instead of PWM+RC filter: at a -70dBc spurious target, a
 * switched (PWM) envelope needs either an impractically high switching
 * frequency or a multi-pole filter to get there on filter math alone, and
 * in practice parasitic coupling of the switching edges tends to dominate
 * at that level regardless of filter order. A true DAC output has no
 * switching-frequency energy to suppress in the first place.
 *
 * PWM_COMPARISON_ENABLED (config.h) adds the old LEDC+RC path back in,
 * driven from the same envelope value as the DAC, purely so the two can
 * be scoped side by side against the same source signal. Set to 0 once
 * you're done comparing.
 */

#include <stdint.h>
#include "config.h"
#include "esp_attr.h"

// ---- MCP4725 DAC (RSET modulation output) ----
// NO LONGER CONNECTED (DAC hardware removed) - GPIO47/48 are the
// highest general-purpose pins on the S3: not strapping, not flash/
// PSRAM (even on Octal variants), not USB-JTAG, not shared with UART0/
// Serial. Note: if your exact module part number ends in "V" (e.g.
// N8R8V), these two run at 1.8V logic instead of 3.3V - check before
// this DAC (or anything else) actually gets wired back to them.
#define MCP4725_SDA_GPIO      47
#define MCP4725_SCL_GPIO      48
#define MCP4725_I2C_PORT      I2C_NUM_0
#define MCP4725_I2C_FREQ_HZ   400000        // fast mode - standard (100kHz) is too slow to fit the sample period
#define MCP4725_I2C_ADDR      0x61          // 0x60 with A0 tied low, 0x61 with A0 tied high
// Keep output codes off the 0/4095 rails - MCP4725 linearity degrades
// near the extremes (datasheet-recommended usable range).
#define DAC_CODE_MIN           100
#define DAC_CODE_MAX          4000

// ---- PWM comparison path: drives the same envelope value out via
// LEDC+RC as well as the DAC, so you can scope both side by side against
// the same source signal. Set PWM_COMPARISON_ENABLED (config.h) to 0 once
// you're done comparing - this was removed from the main design in favour
// of the DAC (imaging/spurious concerns discussed earlier), this is just
// for a direct side-by-side look.
#define RSET_MOD_LEDC_GPIO    2      // within the board's easy-access GPIO1-13 range; not otherwise used
#define RSET_MOD_LEDC_TIMER   LEDC_TIMER_0
#define RSET_MOD_LEDC_CH      LEDC_CHANNEL_0
#define RSET_MOD_LEDC_FREQ_HZ 78125  // max achievable at 10-bit res on 80MHz APB clock (see earlier discussion)
#define RSET_MOD_LEDC_RES     LEDC_TIMER_10_BIT

// Target max DAC update rate. The envelope only carries content up to
// ~3.5-4kHz, so ~10kHz comfortably clears Nyquist. Deliberately throttling
// down from "as fast as the I2C bus allows" (~14kHz back-to-back) reduces
// how often the I2C driver's ISR fires - which we've confirmed is the
// actual source of the cross-core timing jitter on dsp_task, not flash
// cache eviction. This trades unneeded DAC update margin for reduced
// disruption, at no audio-quality cost. Applied on the SENDING side
// (envelope_output_submit_dac_sample() skips xQueueOverwrite itself) -
// see the .cpp for why.
#define DAC_TARGET_UPDATE_RATE_HZ 10000u
#define DAC_WRITE_DECIMATION ((SAMPLE_RATE_HZ + DAC_TARGET_UPDATE_RATE_HZ - 1) / DAC_TARGET_UPDATE_RATE_HZ)  // round up

// Envelope-to-PWM-duty range mapping - separate knob from master gain.
// Master gain (ssb_dsp_set_master_gain_db) scales the WHOLE signal chain
// (phase deviation and envelope together, inside ssb_dsp); this only
// remaps envelope's own [0,1] output into a duty-cycle range before it
// drives the BS170 gate via PWM. Controls where the envelope's quiet-to-
// loud excursion actually sits relative to the Vgs sweet spot already
// characterized on real hardware (~2.3V +/-0.75V) - a mismatch here is a
// plausible independent contributor to the observed nonlinearity,
// separate from overall drive level. Runtime-tunable via 'u'/'j'
// (offset) and 'i'/'k' (scale/span) so it can be swept empirically
// rather than needing a reflash per trial.
#define ENV_PWM_STEP 0.02f   // 2% duty per keypress

// Creates the envelope hand-off queue, brings up the I2C DAC driver and
// (if PWM_COMPARISON_ENABLED) the LEDC PWM channel, runs the MCP4725
// connectivity probe write, and (if dac_task_enabled) starts dac_task on
// Core 1. Call once from setup().
void envelope_output_init(void);

// Drives the PWM comparison output from the (already relative-delayed)
// envelope value. No-op if PWM_COMPARISON_ENABLED is 0, ALSO a no-op
// while duty-override mode is active (see below) - dsp_task calls this
// unconditionally every full tick, same as always; the no-op happens
// internally so dsp_task itself needed no changes for this feature.
void IRAM_ATTR envelope_output_write_pwm(float delayed_envelope);

// ---- Direct duty override ('d' + '>'/'<'/'N'/'B', serial_commands.cpp) ----
// For characterizing the RSET/PWM/filter/AD9851 chain directly against a
// KNOWN, exact commanded duty count, bypassing master gain, envelope,
// offset/scale, AND the predistort LUT entirely - see
// envelope_predistort.h's REVISION 3 notes for why this exists (the
// gate-voltage-inference chain that REVISION 2/3 relied on to back out
// duty from a DC voltage reading is no longer needed at all once duty can
// just be commanded and read back directly). The carrier/phase (AD9851)
// path is completely unaffected - whatever audio source is selected
// keeps running normally; 's' (single-tone, phase held rock-steady) is
// the natural choice while sweeping this.
//
// Writes an EXACT raw PWM duty count [0, (1<<RSET_MOD_LEDC_RES)-1]
// straight to the RSET LEDC channel - no float conversion, no offset/
// scale, nothing else in between. Called only from serial_commands.cpp
// (loop()/Core 1, not dsp_task/Core 0 - same non-ISR context
// envelope_output_write_pwm() itself is always called from, so this
// needs no new ISR-safety consideration).
void IRAM_ATTR envelope_output_write_duty_raw(uint32_t duty);

// The highest valid raw duty count, i.e. (1<<RSET_MOD_LEDC_RES)-1 - a
// getter rather than making callers reach for RSET_MOD_LEDC_RES
// themselves, since that's an ledc_timer_bit_t enum from driver/ledc.h,
// which only THIS file's .cpp includes; serial_commands.cpp (the only
// other caller) has no reason to need that header itself.
uint32_t envelope_output_get_max_duty(void);

// While enabled, envelope_output_write_pwm() above becomes a no-op every
// tick - the LEDC hardware just continues outputting whatever duty
// envelope_output_write_duty_raw() last wrote (LEDC holds its duty
// register between explicit updates, so dsp_task doesn't need to keep
// re-writing it - and by skipping the write entirely rather than trying
// to race it, dsp_task's normal envelope pipeline can never fight the
// override). 'd' toggles this; entering it resets the working duty value
// to 0 - see serial_commands.cpp.
bool envelope_output_duty_override_get_enabled(void);
void envelope_output_duty_override_set_enabled(bool enable);

// Hands the (un-delayed) envelope value to dac_task via the 1-deep
// "latest value wins" queue, decimated to DAC_TARGET_UPDATE_RATE_HZ. Call
// once per dsp_task tick, after the PWM/AD9851 writes.
void IRAM_ATTR envelope_output_submit_dac_sample(float envelope);

uint16_t envelope_output_get_last_dac_code(void);

float envelope_output_get_pwm_offset(void);
float envelope_output_get_pwm_scale(void);

// 'u'/'j'/'i'/'k' handlers - clamp to [0,1] internally, same bounds the
// original inline handlers used.
void envelope_output_raise_pwm_offset(void);
void envelope_output_lower_pwm_offset(void);
void envelope_output_widen_pwm_scale(void);
void envelope_output_narrow_pwm_scale(void);

// Sets offset/scale directly (used by preset loading) - no clamping,
// matching the original preset loader's direct assignment (presets are
// trusted to contain sane values, same as before).
void envelope_output_set_pwm_offset(float offset);
void envelope_output_set_pwm_scale(float scale);
