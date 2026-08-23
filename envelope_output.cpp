/**
 * envelope_output.cpp - see envelope_output.h.
 */

#include "envelope_output.h"
#include "driver/i2c.h"
#if PWM_COMPARISON_ENABLED
#include "driver/ledc.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <Arduino.h>

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
}
#endif

void envelope_output_init(void)
{
    init_i2c_dac();
#if PWM_COMPARISON_ENABLED
    init_rset_mod_pwm();
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

void IRAM_ATTR envelope_output_write_duty_raw(uint32_t duty)
{
#if PWM_COMPARISON_ENABLED
    uint32_t max_duty = (1u << RSET_MOD_LEDC_RES) - 1u;
    if (duty > max_duty) {
        duty = max_duty;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, RSET_MOD_LEDC_CH, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, RSET_MOD_LEDC_CH);
#else
    (void)duty;
#endif
}

uint32_t envelope_output_get_max_duty(void)
{
    return (1u << RSET_MOD_LEDC_RES) - 1u;
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
