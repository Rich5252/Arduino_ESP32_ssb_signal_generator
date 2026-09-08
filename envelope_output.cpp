/**
 * envelope_output.cpp - see envelope_output.h.
 */

#include "envelope_output.h"
#include "driver/i2c.h"
#if PWM_COMPARISON_ENABLED
#include "driver/ledc.h"
#endif
#if SDM_COMPARISON_ENABLED
#include "driver/sdm.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <Arduino.h>
// 2026-09-08: only needed for the ENVELOPE_INTERP_USE_HW_FADE flag itself
// (gates the ledc_fade_func_install() call below) - this file otherwise has
// no dependency on envelope_interp's own state/API.
#include "envelope_interp.h"

static volatile float s_env_pwm_offset = 0.2f;   // was a hardcoded constant
static volatile float s_env_pwm_scale  = 0.9f;   // was a hardcoded constant

static QueueHandle_t s_envelope_queue;   // length 1, "latest value wins" (xQueueOverwrite)
static TaskHandle_t s_dac_task;
static volatile uint16_t s_dbg_dac_code = 0;

// MCP4725 "Fast Write Command" - 2 data bytes after the address, updates
// the DAC register immediately, does NOT touch EEPROM (EEPROM writes take
// 25-50ms - never do that on this path). Byte layout:
//   byte0 = 0 0 PD1 PD0 D11 D10 D9 D8   (PD1:PD0 = 00 -> normal operation)
//   byte1 = D7 D6 D5 D4 D3 D2 D1 D0
static void mcp4725_fast_write(uint16_t code12)
{
    uint8_t buf[2] = {
        (uint8_t)((code12 >> 8) & 0x0F),
        (uint8_t)(code12 & 0xFF),
    };
    esp_err_t err = i2c_master_write_to_device(MCP4725_I2C_PORT, MCP4725_I2C_ADDR, buf, sizeof(buf), pdMS_TO_TICKS(10));
    if (err != ESP_OK) {
        // Throttled - at 9600Hz we'd otherwise flood the log if the bus is
        // genuinely broken (wrong address, no pull-ups, no ACK, etc.)
        static uint32_t last_err_log_ms = 0;
        uint32_t now = millis();
        if (now - last_err_log_ms >= 1000) {
            last_err_log_ms = now;
            Serial.printf("MCP4725 write failed: %s (check address 0x%02X, pull-ups, wiring)\r\n",
                esp_err_to_name(err), MCP4725_I2C_ADDR);
        }
    }
}

static void dac_task(void *arg)
{
    float envelope = 0.0f;
    while (1) {
        // Blocks here - this task's whole job is to wait for a value and
        // write it out. Whatever this ~70us I2C write costs, it only
        // delays how fresh THIS task's own output is, never dsp_task.
        // Throttling now happens on the SENDING side (envelope_output_
        // submit_dac_sample()) - see DAC_WRITE_DECIMATION. Doing it here
        // via "continue" instead made things worse: xQueueOverwrite wakes
        // a blocked receiver on every call regardless of whether work is
        // skipped, so skipping the write but still re-blocking
        // immediately just meant MORE frequent cross-core wake events,
        // not fewer - the opposite of what we wanted.
        xQueueReceive(s_envelope_queue, &envelope, portMAX_DELAY);

        uint16_t code = (uint16_t)(DAC_CODE_MIN + envelope * (float)(DAC_CODE_MAX - DAC_CODE_MIN));
        mcp4725_fast_write(code);

        s_dbg_dac_code = code;
    }
}

static void init_i2c_dac(void)
{
    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = MCP4725_SDA_GPIO;
    conf.scl_io_num = MCP4725_SCL_GPIO;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;   // belt-and-braces; use real
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;   // external ~4.7k pull-ups too
    conf.master.clk_speed = MCP4725_I2C_FREQ_HZ;
    i2c_param_config(MCP4725_I2C_PORT, &conf);
    i2c_driver_install(MCP4725_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
}

#if SDM_COMPARISON_ENABLED
static sdm_channel_handle_t s_sdm_chan = NULL;

// 2026-09-08: brings up the SDM channel on SDM_OUT_GPIO - see
// envelope_output.h's SDM section for the pin/rate/density reasoning.
// `sdm_config_t sdm_cfg = {}` zero-initializes everything not explicitly
// set below (in particular invert_out/io_loop_back/flags, whichever of
// those turns out to be this IDF version's actual field layout for them -
// not yet checked against the installed driver/sdm.h, same "CHECK THIS
// against your actual installed driver" caution as envelope_output_start_
// hw_fade()'s own comment) to their safe off/false defaults: no output
// inversion, no loopback debug mode.
static void init_sdm(void)
{
    sdm_config_t sdm_cfg = {};
    sdm_cfg.clk_src = SDM_CLK_SRC_DEFAULT;
    sdm_cfg.gpio_num = SDM_OUT_GPIO;
    sdm_cfg.sample_rate_hz = SDM_SAMPLE_RATE_HZ;

    esp_err_t err = sdm_new_channel(&sdm_cfg, &s_sdm_chan);
    if (err != ESP_OK) {
        Serial.printf("SDM channel init FAILED on GPIO%d: %s\r\n", SDM_OUT_GPIO, esp_err_to_name(err));
        s_sdm_chan = NULL;
        return;
    }
    err = sdm_channel_enable(s_sdm_chan);
    if (err != ESP_OK) {
        Serial.printf("SDM channel enable FAILED: %s\r\n", esp_err_to_name(err));
        return;
    }
    // Start at density 0 (~50% average -> mid-scale after filtering) as a
    // known, safe boot value, same "explicit known state at boot" idea as
    // the MCP4725 probe's own mid-scale write just below in
    // envelope_output_init().
    sdm_channel_set_pulse_density(s_sdm_chan, 0);
    Serial.printf("SDM channel OK on GPIO%d, sample_rate_hz=%u\r\n", SDM_OUT_GPIO, (unsigned)SDM_SAMPLE_RATE_HZ);
}
#endif

#if PWM_COMPARISON_ENABLED
static void init_rset_mod_pwm(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = RSET_MOD_LEDC_RES,
        .timer_num = RSET_MOD_LEDC_TIMER,
        .freq_hz = RSET_MOD_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_cfg);

    ledc_channel_config_t ch_cfg = {
        .gpio_num = RSET_MOD_LEDC_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = RSET_MOD_LEDC_CH,
        .timer_sel = RSET_MOD_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&ch_cfg);

#if ENVELOPE_INTERP_USE_HW_FADE
    // 2026-09-08: required once before ANY ledc_set_fade_*()/
    // ledc_fade_start() call - installs the LEDC driver's own fade ISR
    // service, which is what actually steps the duty register forward on
    // its own clock once envelope_output_start_hw_fade() (below) kicks a
    // fade off. Argument 0 = no ESP_INTR_FLAG_* fade-ISR allocation flags
    // needed here (default behaviour is fine - this ISR doesn't need to be
    // IRAM-resident/shared/etc. for our purposes). Only installed at all
    // when the flag is on, so the fade ISR isn't silently running unused
    // in the default (software ramp) build.
    ledc_fade_func_install(0);
#endif
}
#endif

void envelope_output_init(void)
{
    init_i2c_dac();
#if PWM_COMPARISON_ENABLED
    init_rset_mod_pwm();
#endif
#if SDM_COMPARISON_ENABLED
    init_sdm();
#endif
    // Explicit connectivity probe - writes mid-scale once so success/failure
    // is obvious in the log immediately at boot, rather than inferred later
    // from DAC behavior.
    {
        uint8_t probe_buf[2] = { 0x08, 0x00 };  // code 0x800 = mid-scale
        esp_err_t probe_err = i2c_master_write_to_device(MCP4725_I2C_PORT, MCP4725_I2C_ADDR,
            probe_buf, sizeof(probe_buf), pdMS_TO_TICKS(50));
        if (probe_err == ESP_OK) {
            Serial.printf("MCP4725 probe OK at address 0x%02X\r\n", MCP4725_I2C_ADDR);
        }
        else {
            Serial.printf("MCP4725 probe FAILED at address 0x%02X: %s - check wiring/pull-ups/address before proceeding\r\n",
                MCP4725_I2C_ADDR, esp_err_to_name(probe_err));
        }
    }

    s_envelope_queue = xQueueCreate(1, sizeof(float));

    // dac_task on Core 1 (with Arduino's own loop(), which is mostly idle
    // here) at low priority - keeps it fully off Core 0, no scheduling
    // interaction with dsp_task at all.
    //
    // Currently compiled out (dac_task_enabled). Note for whenever this
    // is revisited: the Fs-jitter hunt briefly tried moving dsp_task
    // itself onto Core 1 (see the .ino's "TRIED, REVERTED" note on its
    // xTaskCreatePinnedToCore() call) and found real hardware starved
    // Serial completely when dsp_task shared Core 1 with loop() - reverted,
    // dsp_task is back on Core 0. So this comment's premise (dsp_task is
    // on Core 0, dac_task on Core 1, no interaction) still holds today,
    // but if dsp_task's core ever changes again, re-check dac_task's
    // placement against it too rather than assuming this stays apart.
#if dac_task_enabled
    xTaskCreatePinnedToCore(dac_task, "ssb_dac_task", 3072, NULL,
                           tskIDLE_PRIORITY + 1, &s_dac_task, 1);
#endif
}

static volatile bool s_duty_override_enabled = false;

void IRAM_ATTR envelope_output_write_pwm(float delayed_envelope)
{
#if PWM_COMPARISON_ENABLED
    if (s_duty_override_enabled) {
        // Direct duty-set command (envelope_output_write_duty_raw(), via
        // serial_commands.cpp's 'd'/'>'/'<'/'N'/'B') owns the LEDC duty
        // register right now - see envelope_output.h's header comment.
        return;
    }
    uint32_t max_duty = (1u << RSET_MOD_LEDC_RES) - 1u;
    uint32_t duty = (uint32_t)(delayed_envelope * (float)max_duty);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, RSET_MOD_LEDC_CH, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, RSET_MOD_LEDC_CH);
#else
    (void)delayed_envelope;
#endif
}

// 2026-09-08: see envelope_output.h's own comment on this function for the
// overall design (why `steps` is a plain parameter, the approximate/not-
// yet-bench-verified rounding). Implementation notes specific to THIS body:
//
// ledc_set_fade_with_step()'s signature, per the ESP-IDF driver/ledc.h this
// was written against:
//   esp_err_t ledc_set_fade_with_step(ledc_mode_t speed_mode,
//       ledc_channel_t channel, uint32_t target_duty, uint32_t scale,
//       uint32_t cycle_num)
// - CHECK THIS against your actual installed driver/ledc.h before trusting
// this compiles/behaves as written; a signature mismatch here would be a
// clean compile error (easy to fix), but a semantic mismatch (e.g. if some
// IDF version's `scale` means something other than "duty counts per step")
// would silently mis-shape the ramp instead - not verified on this bench.
//
// `scale` is a per-step DUTY COUNT (not a step count) and `cycle_num` is
// how many LEDC PWM periods each step holds before advancing - so to land
// on approximately `steps` hardware steps, back `scale` out as the total
// current-to-target duty distance divided by `steps` (floor via integer
// division - see envelope_output.h for why this makes the real step count
// only approximately `steps`, not exact). cycle_num=1 (advance every PWM
// period) is what actually spreads the fade across `steps` full
// RSET_MOD_LEDC_FREQ_HZ periods - the entire point of retuning
// RSET_MOD_LEDC_FREQ_HZ to be commensurate with the tick rate (see that
// #define's own comment): ENVELOPE_INTERP_FACTOR periods at 64kHz line up
// with one gptimer tick interval, which is exactly how envelope_interp.cpp
// calls this (steps == ENVELOPE_INTERP_FACTOR).
void IRAM_ATTR envelope_output_start_hw_fade(float target_envelope, uint32_t steps)
{
#if PWM_COMPARISON_ENABLED
    if (s_duty_override_enabled) {
        // Same early-return as envelope_output_write_pwm() above - direct
        // duty-set command owns the LEDC duty register right now.
        return;
    }
    if (steps < 1) {
        steps = 1;   // defensive - avoid a divide-by-zero below; callers
                      // are expected to always pass ENVELOPE_INTERP_FACTOR (>=1)
    }

    uint32_t max_duty = (1u << RSET_MOD_LEDC_RES) - 1u;
    uint32_t target_duty = (uint32_t)(target_envelope * (float)max_duty);
    if (target_duty > max_duty) {
        target_duty = max_duty;
    }

    uint32_t current_duty = ledc_get_duty(LEDC_LOW_SPEED_MODE, RSET_MOD_LEDC_CH);
    uint32_t distance = (target_duty > current_duty) ? (target_duty - current_duty)
                                                       : (current_duty - target_duty);
    uint32_t scale = distance / steps;
    if (scale < 1) {
        // Distance smaller than `steps` (or zero) - still take at least
        // one real hardware step of size 1 rather than passing scale=0,
        // which ledc_set_fade_with_step() would likely reject/no-op.
        scale = 1;
    }

    ledc_set_fade_with_step(LEDC_LOW_SPEED_MODE, RSET_MOD_LEDC_CH, target_duty, scale, 1);
    ledc_fade_start(LEDC_LOW_SPEED_MODE, RSET_MOD_LEDC_CH, LEDC_FADE_NO_WAIT);
#else
    (void)target_envelope;
    (void)steps;
#endif
}

void envelope_output_sync_ledc_timer_now(void)
{
#if PWM_COMPARISON_ENABLED
    // See envelope_output.h's own comment on this function for why this
    // matters - resets the LEDC timer's internal counter to a known phase
    // so its autonomous fade-step clock and the sample gptimer's alarm
    // grid share a common reference point instead of an arbitrary power-
    // on-to-power-on offset.
    ledc_timer_rst(LEDC_LOW_SPEED_MODE, RSET_MOD_LEDC_TIMER);
#endif
}

// 2026-09-08: see envelope_output.h's own comment on this function for the
// call-site/timing reasoning (called once per real dsp_task tick,
// independent of 'I'/HW_FADE/curve state), including the 2026-09-08 "later"
// note on the stage/commit-from-ISR variant this reverted FROM - real
// hardware came back worse, not better, with that split (see the header
// comment and group_delay_fit_notes.md's matching entry for the two
// suspected mechanisms).
//
// 2026-09-08, later still: gained the SAME duty-override early-return
// envelope_output_write_pwm()/start_hw_fade() already had - see
// envelope_output_write_duty_raw()'s own comment below for why. 'd' now
// drives whichever comparison path is actually compiled in (PWM or SDM,
// they're mutually exclusive - see the SDM_OUT_GPIO #error above), so this
// path needs to defer to it too, exactly like PWM's write function does.
void IRAM_ATTR envelope_output_write_sdm(float envelope)
{
#if SDM_COMPARISON_ENABLED
    if (s_duty_override_enabled) {
        // Direct override command ('d' + '>'/'<'/'N'/'B', now density-
        // aware - see envelope_output_write_duty_raw()) owns the SDM
        // channel right now.
        return;
    }
    if (s_sdm_chan == NULL) {
        // Either init_sdm() failed (see its own error log at boot) or
        // SDM_COMPARISON_ENABLED was flipped on without a successful
        // channel bring-up - fail silent/no-op rather than dereferencing
        // a null handle.
        return;
    }
    if (envelope < 0.0f) {
        envelope = 0.0f;
    } else if (envelope > 1.0f) {
        envelope = 1.0f;
    }
    int32_t density = (int32_t)(-(float)SDM_DENSITY_CLAMP + envelope * (2.0f * (float)SDM_DENSITY_CLAMP));
    if (density < -128) {
        density = -128;
    } else if (density > 127) {
        density = 127;
    }
    sdm_channel_set_pulse_density(s_sdm_chan, (int8_t)density);
#else
    (void)envelope;
#endif
}

// 2026-09-08: generalized from PWM-only to dual-purpose - see this
// function's own comment in envelope_output.h for the full reasoning.
// `duty` is really "the raw override INDEX," 0..envelope_output_get_max_
// duty() - under PWM_COMPARISON_ENABLED it's a literal LEDC duty count as
// it always was; under SDM_COMPARISON_ENABLED it's linearly remapped onto
// signed density [-SDM_DENSITY_CLAMP,+SDM_DENSITY_CLAMP], index 0 ->
// -SDM_DENSITY_CLAMP, index max -> +SDM_DENSITY_CLAMP - the exact same
// affine mapping envelope_output_write_sdm() uses for envelope=0/1, so a
// swept index means the same thing to both output paths. This is what
// lets 'd'/'>'/'<'/'N'/'B'/'E' (serial_commands.cpp) - and any existing
// automated sweep harness already driving those same keys - work
// unchanged regardless of which comparison path is compiled in; only one
// of PWM_COMPARISON_ENABLED/SDM_COMPARISON_ENABLED can be 1 at a time (see
// envelope_output.h's build-time #error), so there's no runtime ambiguity
// about which peripheral this actually writes to.
void IRAM_ATTR envelope_output_write_duty_raw(uint32_t duty)
{
#if PWM_COMPARISON_ENABLED
    uint32_t max_duty = (1u << RSET_MOD_LEDC_RES) - 1u;
    if (duty > max_duty) {
        duty = max_duty;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, RSET_MOD_LEDC_CH, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, RSET_MOD_LEDC_CH);
#elif SDM_COMPARISON_ENABLED
    if (s_sdm_chan == NULL) {
        return;
    }
    uint32_t max_index = 2u * (uint32_t)SDM_DENSITY_CLAMP;
    if (duty > max_index) {
        duty = max_index;
    }
    int32_t density = (int32_t)duty - (int32_t)SDM_DENSITY_CLAMP;
    sdm_channel_set_pulse_density(s_sdm_chan, (int8_t)density);
#else
    (void)duty;
#endif
}

// 2026-09-08: generalized alongside envelope_output_write_duty_raw() above -
// see that function's comment. Returns the max valid override INDEX for
// whichever comparison path is compiled in (LEDC duty count under PWM,
// 2*SDM_DENSITY_CLAMP under SDM), 0 if neither is enabled.
uint32_t envelope_output_get_max_duty(void)
{
#if PWM_COMPARISON_ENABLED
    return (1u << RSET_MOD_LEDC_RES) - 1u;
#elif SDM_COMPARISON_ENABLED
    return 2u * (uint32_t)SDM_DENSITY_CLAMP;
#else
    return 0;
#endif
}

bool envelope_output_duty_override_get_enabled(void)
{
    return s_duty_override_enabled;
}

void envelope_output_duty_override_set_enabled(bool enable)
{
    s_duty_override_enabled = enable;
}

void IRAM_ATTR envelope_output_submit_dac_sample(float envelope)
{
    // Non-blocking, always succeeds - overwrites whatever was there.
    // dac_task will pick up the latest value whenever it next runs; this
    // call never waits on the I2C bus.
    //
    // Throttled to DAC_TARGET_UPDATE_RATE_HZ: xQueueOverwrite wakes
    // dac_task's blocked receiver on every call, so calling it every
    // sample means waking the other core at the full DSP rate even when
    // most of those wakes would do nothing but immediately re-block.
    // Skipping the call itself (not just the write on the receiving end)
    // genuinely reduces cross-core wake frequency, which is what we've
    // confirmed actually causes the jitter. dac_skip_count persists
    // across calls the same way it did as a dsp_task-local variable in
    // the original .ino (that variable was declared once outside
    // dsp_task's infinite while(1) loop and never reset, so a file-static
    // counter here is exactly equivalent).
    static uint32_t dac_skip_count = 0;
    dac_skip_count++;
    if (dac_skip_count >= DAC_WRITE_DECIMATION) {
        dac_skip_count = 0;
        xQueueOverwrite(s_envelope_queue, &envelope);
    }
}

uint16_t envelope_output_get_last_dac_code(void)
{
    return s_dbg_dac_code;
}

float envelope_output_get_pwm_offset(void)
{
    return s_env_pwm_offset;
}

float envelope_output_get_pwm_scale(void)
{
    return s_env_pwm_scale;
}

void envelope_output_raise_pwm_offset(void)
{
    if (s_env_pwm_offset < 1.0f) s_env_pwm_offset += ENV_PWM_STEP;
}

void envelope_output_lower_pwm_offset(void)
{
    if (s_env_pwm_offset > 0.0f) s_env_pwm_offset -= ENV_PWM_STEP;
}

void envelope_output_widen_pwm_scale(void)
{
    if (s_env_pwm_scale < 1.0f) s_env_pwm_scale += ENV_PWM_STEP;
}

void envelope_output_narrow_pwm_scale(void)
{
    if (s_env_pwm_scale > 0.0f) s_env_pwm_scale -= ENV_PWM_STEP;
}

void envelope_output_set_pwm_offset(float offset)
{
    s_env_pwm_offset = offset;
}

void envelope_output_set_pwm_scale(float scale)
{
    s_env_pwm_scale = scale;
}
