/**
 * DAC version 1
 * ssb_mic_test.ino
 *
 * Arduino-IDE test bed for the SSB mic/DSP pipeline, refactored from the
 * ESP-IDF main.c worked example. AD9851/DDS code is stubbed out entirely
 * (behind AD9851_ATTACHED) - this sketch exercises: mic ADC -> ssb_dsp ->
 * {freq_dev_hz, envelope} -> MCP4725 DAC output (RSET modulation), on two
 * FreeRTOS tasks:
 *
 *   dsp_task  (Core 0, high priority) - timer-notified, does the ADC read,
 *             DSP, and (when attached) the AD9851 SPI write. Never blocks
 *             on the DAC - it drops the computed envelope into a 1-deep
 *             "latest value wins" queue and moves straight on.
 *   dac_task  (Core 1, lower priority) - blocks waiting for a new envelope
 *             value, then does the MCP4725 I2C fast-write (~70us at
 *             400kHz - far too slow to share a task with the phase-
 *             critical AD9851 update without risking it). At the current
 *             SAMPLE_RATE_HZ (9600, ~104us/sample) this comfortably fits
 *             within one sample period. If you ever raise the sample rate
 *             materially, revisit this - the I2C write doesn't scale down
 *             with a faster sample clock, and an SPI DAC (e.g. MCP4921)
 *             would be the fix at that point.
 *
 * Why a DAC instead of PWM+RC filter: at a -70dBc spurious target, a
 * switched (PWM) envelope needs either an impractically high switching
 * frequency or a multi-pole filter to get there on filter math alone, and
 * in practice parasitic coupling of the switching edges tends to dominate
 * at that level regardless of filter order. A true DAC output has no
 * switching-frequency energy to suppress in the first place.
 *
 * PWM_COMPARISON_ENABLED (below) adds the old LEDC+RC path back in,
 * driven from the same envelope value as the DAC, purely so the two can
 * be scoped side by side against the same source signal. Set to 0 once
 * you're done comparing.
 *
 * Completely standalone - doesn't touch or depend on TXlink at all yet.
 * Drop this .ino into a sketch folder of the same name, next to
 * ssb_dsp.c / ssb_dsp.h (already added to your project per last session).
 *
 * WIRING:
 *  - Mic preamp -> GPIO6 (ADC1 channel 5 on ESP32-S3).
 *  - MCP4725 SDA -> MCP4725_SDA_GPIO, SCL -> MCP4725_SCL_GPIO. Needs
 *    external ~4.7k pull-ups on both lines for reliable 400kHz operation -
 *    the ESP32's internal pull-ups are too weak on their own.
 *  - MCP4725 I2C address 0x61 (A0 pin tied high).
 *  - MCP4725 VOUT feeds your RSET modulation circuit.
 *  - PWM comparison output (RSET_MOD_LEDC_GPIO) -> RC filter -> scope,
 *    for side-by-side comparison against the DAC output only.
 *
 * What to check once the preamp and DAC are wired up:
 *  - Serial monitor (115200): throttled envelope/freq-dev/DAC-code
 *    summary, printed from loop() on Core 1 at low priority so it can
 *    never perturb either real-time task.
 *  - Scope on the MCP4725's VOUT pin: should track your voice envelope
 *    directly, no switching ripple to look for at all.
 *  - TWOTONE_TEST_MODE below bypasses the mic with a synthesized signal -
 *    a zero-hardware smoke test of the DSP chain.
 *
 * When the AD9851 board arrives: flip AD9851_ATTACHED to 1 and fill in
 * ad9851_init()/ad9851_set_frequency() calls - the DSP/task/timer/DAC
 * structure here doesn't need to change either way.
 */

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gptimer.h"
#include "driver/i2c.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_attr.h"
#include "esp_log.h"

// Defined here (before includes that depend on it) rather than down with
// the other PWM defines below - #if needs this to already be known.
#define PWM_COMPARISON_ENABLED 1
#if PWM_COMPARISON_ENABLED
#include "driver/ledc.h"
#endif

#include "ssb_dsp.h"

static const char *TAG = "ssb_mic_test";

// ---- Set to 1 once the AD9851 board is wired up and its driver calls
// below are filled in. Until then this runs mic->DSP->DAC standalone. ----
#define AD9851_ATTACHED 0

// ---- Two-tone test mode: bypass the mic ADC with a synthesized signal.
// Zero-hardware smoke test of the DSP chain. ----
#define TWOTONE_TEST_MODE   1
#define TWOTONE_F1_HZ        700.0f
#define TWOTONE_F2_HZ       1900.0f
#define TWOTONE_AMPLITUDE    0.45f   // keep below 0.5 so peaks don't clip when summed

// ---- MCP4725 DAC (RSET modulation output) ----
#define MCP4725_SDA_GPIO      13
#define MCP4725_SCL_GPIO      12
#define MCP4725_I2C_PORT      I2C_NUM_0
#define MCP4725_I2C_FREQ_HZ   400000        // fast mode - standard (100kHz) is too slow to fit the sample period
#define MCP4725_I2C_ADDR      0x61          // 0x60 with A0 tied low, 0x61 with A0 tied high
// Keep output codes off the 0/4095 rails - MCP4725 linearity degrades
// near the extremes (datasheet-recommended usable range).
#define DAC_CODE_MIN           100
#define DAC_CODE_MAX          4000

// ---- PWM comparison path: drives the same envelope value out via
// LEDC+RC as well as the DAC, so you can scope both side by side against
// the same source signal. Set PWM_COMPARISON_ENABLED (above, near the
// includes) to 0 once you're done comparing - this was removed from the
// main design in favour of the DAC (imaging/spurious concerns discussed
// earlier), this is just for a direct side-by-side look.
#define RSET_MOD_LEDC_GPIO    2      // within the board's easy-access GPIO1-13 range; not otherwise used
#define RSET_MOD_LEDC_TIMER   LEDC_TIMER_0
#define RSET_MOD_LEDC_CH      LEDC_CHANNEL_0
#define RSET_MOD_LEDC_FREQ_HZ 78125  // max achievable at 10-bit res on 80MHz APB clock (see earlier discussion)
#define RSET_MOD_LEDC_RES     LEDC_TIMER_10_BIT

#define ADC_UNIT       ADC_UNIT_1
#define ADC_CHANNEL    ADC_CHANNEL_5   // GPIO6 on ESP32-S3 - Micr input

// ---- Timing debug pin: toggled high at the start of dsp_task's real work
// and low at the end, so a scope on this pin directly measures the actual
// loop iteration time on real hardware - much more trustworthy than
// estimating it. Set to 0 to remove once you've got your measurement.
#define TIMING_DEBUG_ENABLED 1
#define TIMING_DEBUG_GPIO     4   // within the board's easy-access GPIO1-13 range; not otherwise used

#define SAMPLE_RATE_HZ     20000u
#define HILBERT_TAPS       65
#define MAX_FREQ_DEV_HZ    2800.0f

// Target max DAC update rate. The envelope only carries content up to
// ~3.5-4kHz, so ~10kHz comfortably clears Nyquist. Deliberately throttling
// down from "as fast as the I2C bus allows" (~14kHz back-to-back) reduces
// how often the I2C driver's ISR fires - which we've confirmed is the
// actual source of the cross-core timing jitter on dsp_task, not flash
// cache eviction. This trades unneeded DAC update margin for reduced
// disruption, at no audio-quality cost. Applied on the SENDING side
// (dsp_task skips xQueueOverwrite itself) - see dsp_task for why.
#define DAC_TARGET_UPDATE_RATE_HZ 10000u
#define DAC_WRITE_DECIMATION ((SAMPLE_RATE_HZ + DAC_TARGET_UPDATE_RATE_HZ - 1) / DAC_TARGET_UPDATE_RATE_HZ)  // round up

#if AD9851_ATTACHED
#include "ad9851.h"
#define AD9851_PIN_DATA   23
#define AD9851_PIN_WCLK   18
#define AD9851_PIN_FQUD   19
#define AD9851_PIN_RESET  21
#define REF_CLK_HZ        30000000u
#define CARRIER_HZ        14200000u
static ad9851_handle_t s_ad9851;
static volatile uint32_t s_carrier_hz = CARRIER_HZ;
#endif

static ssb_dsp_handle_t s_ssb;
static adc_oneshot_unit_handle_t s_adc;
static TaskHandle_t s_dsp_task;
static TaskHandle_t s_dac_task;
static QueueHandle_t s_envelope_queue;   // length 1, "latest value wins" (xQueueOverwrite)
static volatile ssb_sideband_t s_sideband = SSB_SIDEBAND_USB;

// Written by dsp_task/dac_task, printed by loop() on Core 1 at low
// priority - keeps Serial (slow) completely out of both real-time tasks.
static volatile float s_dbg_envelope = 0.0f;
static volatile float s_dbg_freq_dev = 0.0f;
static volatile uint16_t s_dbg_dac_code = 0;

static bool IRAM_ATTR on_timer_alarm(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx)
{
    BaseType_t high_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_dsp_task, &high_task_woken);
    return high_task_woken == pdTRUE;
}

#if TWOTONE_TEST_MODE
static float s_tone1_phase = 0.0f;
static float s_tone2_phase = 0.0f;

static inline float generate_twotone_sample(void)
{
    const float two_pi = 2.0f * (float)M_PI;
    float sample = TWOTONE_AMPLITUDE * sinf(s_tone1_phase) +
                   TWOTONE_AMPLITUDE * sinf(s_tone2_phase);
    s_tone1_phase += two_pi * TWOTONE_F1_HZ / (float)SAMPLE_RATE_HZ;
    s_tone2_phase += two_pi * TWOTONE_F2_HZ / (float)SAMPLE_RATE_HZ;
    if (s_tone1_phase > two_pi) s_tone1_phase -= two_pi;
    if (s_tone2_phase > two_pi) s_tone2_phase -= two_pi;
    return sample;
}
#endif

// IRAM_ATTR - keeps this task's code in internal RAM rather than flash,
// so it's immune to cache-line stalls caused by Core 1 activity (dac_task's
// I2C driver work) touching flash. Confirmed by disabling dac_task and
// seeing timing clean up - this is the structural fix rather than just
// working around it by leaving dac_task off.
static void IRAM_ATTR dsp_task(void* arg)
{
    // Simple DC-blocking single-pole high-pass state (mic path only)
    float dc_estimate = 0.0f;
    const float dc_alpha = 0.995f;
    uint32_t dac_skip_count = 0;

    while (1) {
        // Block until the timer ISR notifies us - this sets our sample rate.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

#if TIMING_DEBUG_ENABLED
        digitalWrite(TIMING_DEBUG_GPIO, HIGH);
#endif

        float sample;
#if TWOTONE_TEST_MODE
        sample = generate_twotone_sample();
#else
        int raw = 0;
        adc_oneshot_read(s_adc, ADC_CHANNEL, &raw);
        // Normalize 12-bit ADC (0-4095) to roughly [-1, 1] with DC removal.
        sample = (float)raw / 2048.0f - 1.0f;
        dc_estimate = dc_alpha * dc_estimate + (1.0f - dc_alpha) * sample;
        sample -= dc_estimate;
#endif

        float freq_dev_hz = 0.0f;
        float envelope = 0.0f;
        ssb_dsp_process_sample(s_ssb, sample, s_sideband, &freq_dev_hz, &envelope);

#if AD9851_ATTACHED
        uint32_t tx_freq = s_carrier_hz + (int32_t)freq_dev_hz;
        ad9851_set_frequency(s_ad9851, tx_freq);
#endif

        // envelope is roughly [0,1] for typical mic levels but not
        // rigorously bounded - clamp before handing off.
        envelope = envelope * 0.9 + 0.2;
        if (envelope < 0.2f) envelope = 0.2f;
        if (envelope > 1.0f) envelope = 1.0f;

        // Non-blocking, always succeeds - overwrites whatever was there.
        // dac_task will pick up the latest value whenever it next runs;
        // this call never waits on the I2C bus.
        //
        // Throttled to DAC_TARGET_UPDATE_RATE_HZ: xQueueOverwrite wakes
        // dac_task's blocked receiver on every call, so calling it every
        // sample means waking the other core at the full DSP rate even
        // when most of those wakes would do nothing but immediately
        // re-block. Skipping the call itself (not just the write on the
        // receiving end) genuinely reduces cross-core wake frequency,
        // which is what we've confirmed actually causes the jitter.
        dac_skip_count++;
        if (dac_skip_count >= DAC_WRITE_DECIMATION) {
            dac_skip_count = 0;
            xQueueOverwrite(s_envelope_queue, &envelope);
        }

#if PWM_COMPARISON_ENABLED
        // Same envelope value, driven out via LEDC - this write is a
        // near-instant register write (unlike the DAC's I2C transaction),
        // so it's safe to do directly here in dsp_task without decoupling.
        {
            uint32_t max_duty = (1u << RSET_MOD_LEDC_RES) - 1u;
            uint32_t duty = (uint32_t)(envelope * (float)max_duty);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, RSET_MOD_LEDC_CH, duty);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, RSET_MOD_LEDC_CH);
        }
#endif

        s_dbg_envelope = envelope;
        s_dbg_freq_dev = freq_dev_hz;

#if TIMING_DEBUG_ENABLED
        digitalWrite(TIMING_DEBUG_GPIO, LOW);
#endif    
    }
}

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
            ESP_LOGW(TAG, "MCP4725 write failed: %s (check address 0x%02X, pull-ups, wiring)",
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
        // Throttling now happens on the SENDING side (dsp_task) - see
        // DAC_WRITE_DECIMATION there. Doing it here via "continue" instead
        // made things worse: xQueueOverwrite wakes a blocked receiver on
        // every call regardless of whether work is skipped, so skipping
        // the write but still re-blocking immediately just meant MORE
        // frequent cross-core wake events, not fewer - the opposite of
        // what we wanted.
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

static void init_adc(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT,
    };
    adc_oneshot_new_unit(&unit_cfg, &s_adc);

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    adc_oneshot_config_channel(s_adc, ADC_CHANNEL, &chan_cfg);
}

static void init_sample_timer(void)
{
    gptimer_handle_t timer = NULL;
    gptimer_config_t timer_cfg = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000, // 1MHz tick = 1us resolution
    };
    gptimer_new_timer(&timer_cfg, &timer);

    gptimer_event_callbacks_t cbs = {
        .on_alarm = on_timer_alarm,
    };
    gptimer_register_event_callbacks(timer, &cbs, NULL);

    gptimer_alarm_config_t alarm_cfg = {
        .alarm_count = 1000000 / SAMPLE_RATE_HZ,
        .reload_count = 0,
    };
    alarm_cfg.flags.auto_reload_on_alarm = true;  // nested dotted designators aren't valid C++
    gptimer_set_alarm_action(timer, &alarm_cfg);

    gptimer_enable(timer);
    gptimer_start(timer);
}

void setup()
{
    Serial.begin(115200);
    delay(200);   // give USB CDC a moment to enumerate before we print

#if TIMING_DEBUG_ENABLED
    pinMode(TIMING_DEBUG_GPIO, OUTPUT);
    digitalWrite(TIMING_DEBUG_GPIO, LOW);
#endif

#if AD9851_ATTACHED
    ad9851_config_t ad_cfg = {
        .spi_host = SPI2_HOST,
        .pin_data = AD9851_PIN_DATA,
        .pin_wclk = AD9851_PIN_WCLK,
        .pin_fqud = AD9851_PIN_FQUD,
        .pin_reset = AD9851_PIN_RESET,
        .ref_clk_hz = REF_CLK_HZ,
        .use_6x_multiplier = true,
        .spi_clock_hz = 2000000,
    };
    ESP_ERROR_CHECK(ad9851_init(&ad_cfg, &s_ad9851));
    ad9851_set_frequency(s_ad9851, s_carrier_hz);
#endif

    ssb_dsp_config_t dsp_cfg = {
        .sample_rate_hz = SAMPLE_RATE_HZ,
        .num_taps = HILBERT_TAPS,
        .max_freq_dev_hz = MAX_FREQ_DEV_HZ,
    };
    ESP_ERROR_CHECK(ssb_dsp_init(&dsp_cfg, &s_ssb));

    init_adc();
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
            ESP_LOGI(TAG, "MCP4725 probe OK at address 0x%02X", MCP4725_I2C_ADDR);
        }
        else {
            ESP_LOGE(TAG, "MCP4725 probe FAILED at address 0x%02X: %s - check wiring/pull-ups/address before proceeding",
                MCP4725_I2C_ADDR, esp_err_to_name(probe_err));
        }
    }


    s_envelope_queue = xQueueCreate(1, sizeof(float));

    // dac_task on Core 1 (with Arduino's own loop(), which is mostly idle
    // here) at low priority - keeps it fully off Core 0, no scheduling
    // interaction with dsp_task at all.
    //xTaskCreatePinnedToCore(dac_task, "ssb_dac_task", 3072, NULL,
    //                       tskIDLE_PRIORITY + 1, &s_dac_task, 1);

    // dsp_task on Core 0, high priority - the phase-critical path.
    xTaskCreatePinnedToCore(dsp_task, "ssb_dsp_task", 4096, NULL,
                             configMAX_PRIORITIES - 2, &s_dsp_task, 0);

    init_sample_timer();

    ESP_LOGI(TAG, "SSB mic test running: taps=%d fs=%uHz mode=%s ad9851=%s dac=MCP4725@0x%02X pwm_compare=%s",
             HILBERT_TAPS, SAMPLE_RATE_HZ,
             TWOTONE_TEST_MODE ? "TWO-TONE TEST" : "mic",
             AD9851_ATTACHED ? "attached" : "not attached (stubbed)",
             MCP4725_I2C_ADDR,
             PWM_COMPARISON_ENABLED ? "on" : "off");
}

void loop()
{
    // Diagnostics only - throttled well below the sample rate, and this
    // task is lower priority than both real-time tasks, so it never
    // competes with either for CPU time or bus access.
    static uint32_t last_print_ms = 0;
    uint32_t now = millis();
    if (now - last_print_ms >= 200) {
        last_print_ms = now;
        Serial.printf("envelope=,%.3f  ,freq_dev=,%.1f,Hz  dac_code=,%u\r\n",
                      s_dbg_envelope, s_dbg_freq_dev, s_dbg_dac_code);
    }
    delay(10);
}
