/**
 * ssb_mic_test.ino
 *
 * Arduino-IDE test bed for the SSB mic/DSP pipeline, refactored from the
 * ESP-IDF main.c worked example. AD9851/DDS code is stubbed out entirely
 * (behind AD9851_ATTACHED) - this sketch only exercises: mic ADC ->
 * ssb_dsp -> {freq_dev_hz, envelope} -> RSET-style PWM output, on its own
 * FreeRTOS task pinned to Core 0. Completely standalone - doesn't touch or
 * depend on TXlink at all yet.
 *
 * Drop this .ino into a sketch folder of the same name, next to
 * ssb_dsp.c / ssb_dsp.h (already added to your project per last session).
 *
 * WIRING NOTE: this targets ESP32-S3. ADC1 channel-to-GPIO mapping is
 * different from classic ESP32 - ADC_CHANNEL_6 here is GPIO7, NOT GPIO34
 * (the original ESP-IDF file's comment was written for classic ESP32).
 * Wire your electret preamp output to GPIO7, or change ADC_CHANNEL below
 * and update this comment to match.
 *
 * What to check once the preamp is wired up:
 *  - Serial monitor (115200): throttled envelope/freq-dev summary, printed
 *    from loop() on Core 1 so it can never perturb the Core 0 sample loop.
 *  - Scope on RSET_MOD_LEDC_GPIO: filtered PWM output should track your
 *    voice envelope in real time - this works with no DDS attached at all.
 *  - TWOTONE_TEST_MODE below bypasses the mic with a synthesized two-tone
 *    signal - a zero-hardware smoke test of the whole DSP chain, useful
 *    before the preamp is even wired up. Defaults to 0 (mic) since testing
 *    the real preamp is the point of this sketch.
 *
 * When the AD9851 board arrives: flip AD9851_ATTACHED to 1 and fill in
 * ad9851_init()/ad9851_set_frequency() calls (or swap in the templated
 * Arduino AD9851.h library instead, reviewed earlier - either works, the
 * DSP/task/timer structure here doesn't need to change either way).
 */

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gptimer.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

#include "ssb_dsp.h"

static const char *TAG = "ssb_mic_test";

// ---- Set to 1 once the AD9851 board is wired up and its driver calls
// below are filled in. Until then this runs mic->DSP->PWM standalone. ----
#define AD9851_ATTACHED 0

// ---- Two-tone test mode: bypass the mic ADC with a synthesized signal.
// Zero-hardware smoke test of the DSP chain. Flip to 1 if you want to
// sanity-check the chain before the preamp is wired up; 0 tests the
// real mic path, which is the point of this sketch. ----
#define TWOTONE_TEST_MODE   1
#define TWOTONE_F1_HZ        700.0f
#define TWOTONE_F2_HZ       1900.0f
#define TWOTONE_AMPLITUDE    0.45f   // keep below 0.5 so peaks don't clip when summed

// RSET-style PWM output - independent of the AD9851, gives you a real
// envelope waveform on a scope even before the DDS arrives. Adjust to a
// GPIO your board actually has free (avoid the octal PSRAM/flash pins on
// S3 modules that use them, typically in the 26-37 range - check your
// module's datasheet if unsure).
#define RSET_MOD_LEDC_GPIO    25
#define RSET_MOD_LEDC_TIMER   LEDC_TIMER_0
#define RSET_MOD_LEDC_CH      LEDC_CHANNEL_0
#define RSET_MOD_LEDC_FREQ_HZ 78125
#define RSET_MOD_LEDC_RES     LEDC_TIMER_10_BIT

#define ADC_UNIT       ADC_UNIT_1
#define ADC_CHANNEL    ADC_CHANNEL_6   // GPIO7 on ESP32-S3 (NOT GPIO34 - that's classic ESP32) - wire preamp output here

#define SAMPLE_RATE_HZ     9600u
#define HILBERT_TAPS       65
#define MAX_FREQ_DEV_HZ    2800.0f

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
static volatile ssb_sideband_t s_sideband = SSB_SIDEBAND_USB;

// Written by the DSP task, printed by loop() on Core 1 - keeps Serial
// (slow, not ISR/IRAM-safe) completely out of the time-critical path.
static volatile float s_dbg_envelope = 0.0f;
static volatile float s_dbg_freq_dev = 0.0f;

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

static void dsp_task(void *arg)
{
    // Simple DC-blocking single-pole high-pass state (mic path only)
    float dc_estimate = 0.0f;
    const float dc_alpha = 0.995f;

    while (1) {
        // Block until the timer ISR notifies us - this sets our sample rate.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

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
        // rigorously bounded - clamp before scaling to PWM duty.
        if (envelope > 1.0f) envelope = 1.0f;
        uint32_t max_duty = (1 << RSET_MOD_LEDC_RES) - 1;
        uint32_t duty = (uint32_t)(envelope * (float)max_duty);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, RSET_MOD_LEDC_CH, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, RSET_MOD_LEDC_CH);

        s_dbg_envelope = envelope;
        s_dbg_freq_dev = freq_dev_hz;
    }
}

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
    init_rset_mod_pwm();

    // Pin the DSP task to Core 0, high priority. Arduino's own loop() runs
    // on Core 1 by default - keeping them separate from the start avoids
    // the jitter risk we're designing around before this gets merged
    // into TXlink (whose loop() will stay on Core 1, untouched).
    xTaskCreatePinnedToCore(dsp_task, "ssb_dsp_task", 4096, NULL,
                             configMAX_PRIORITIES - 2, &s_dsp_task, 0);

    init_sample_timer();

    ESP_LOGI(TAG, "SSB mic test running: taps=%d fs=%uHz mode=%s ad9851=%s",
             HILBERT_TAPS, SAMPLE_RATE_HZ,
             TWOTONE_TEST_MODE ? "TWO-TONE TEST" : "mic",
             AD9851_ATTACHED ? "attached" : "not attached (stubbed)");
}

void loop()
{
    // Diagnostics only - throttled well below the sample rate so this
    // never competes with the DSP task for CPU time or the ADC/SPI bus.
    static uint32_t last_print_ms = 0;
    uint32_t now = millis();
    if (now - last_print_ms >= 200) {
        last_print_ms = now;
        Serial.printf("envelope=%.3f  freq_dev=%.1fHz\n",
                      s_dbg_envelope, s_dbg_freq_dev);
    }
    delay(10);
}
