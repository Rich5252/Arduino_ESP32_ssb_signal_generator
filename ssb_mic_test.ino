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
 *             SAMPLE_RATE_HZ this comfortably fits within one sample
 *             period. If you ever raise the sample rate materially,
 *             revisit this - the I2C write doesn't scale down with a
 *             faster sample clock, and an SPI DAC (e.g. MCP4921) would be
 *             the fix at that point.
 *
 * Why a DAC instead of PWM+RC filter, MCP4725 pinout, ADC design history,
 * the group-delay-equalizer derivation, etc: see the module headers below
 * (envelope_output.h, adc_capture.h, envelope_gdeq.h, relative_delay.h) -
 * this file used to carry all of that inline, but at ~1900 lines with
 * everything in one place it was getting hard to navigate. It's now split
 * into one module per concern, each owning its own state behind a small
 * function-based interface (never raw `extern`-shared globals) so nothing
 * outside a module can get at its internals except through that
 * interface:
 *
 *   config.h           - shared compile-time constants (#defines only)
 *   settings.h          - audio_source_t, PersistentSettings, presets (unchanged)
 *   dsp_state.h/.cpp    - the ssb_dsp handle, sideband, audio source, gain cache
 *   adc_capture.h/.cpp  - adc_continuous DMA setup, ISR FIFO, biquad LPF
 *   test_signals.h/.cpp - two-tone/single-tone/ENVSTEP/FMTEST/AMTEST generators
 *   envelope_gdeq.h/.cpp- the envelope-path group-delay all-pass equalizer
 *   relative_delay.h/.cpp - phase/envelope fractional relative-timing line
 *   carrier_output.h/.cpp - AD9851 init + per-tick frequency update
 *   envelope_output.h/.cpp - PWM (RSET) + MCP4725 DAC + dac_task
 *   diagnostics.h/.cpp  - dsp_task timing stats + periodic status prints
 *   serial_commands.h/.cpp - the single-character serial command handler
 *
 * What's left here is genuinely top-level orchestration only: the sample
 * timer ISR, dsp_task's per-tick call sequence, setup(), and loop().
 * Nothing about the real-time behavior changed in this split - every
 * function that used to run inline in dsp_task is now a small IRAM_ATTR
 * function in its owning module, called in exactly the same order.
 *
 * NOT COMPILER-VERIFIED: this refactor was done by careful manual review
 * (no ESP32/Arduino toolchain available in the environment it was written
 * in) - run an Arduino IDE compile pass before flashing to hardware.
 *
 * Completely standalone - doesn't touch or depend on TXlink at all yet.
 * Drop this .ino into a sketch folder of the same name, next to all the
 * other .c/.cpp/.h files listed above.
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
 *  - Serial monitor (921600): throttled envelope/freq-dev/DAC-code
 *    summary, printed from loop() on Core 1 at low priority so it can
 *    never perturb either real-time task.
 *  - Scope on the MCP4725's VOUT pin: should track your voice envelope
 *    directly, no switching ripple to look for at all.
 *  - TWOTONE_TEST_MODE (config.h) bypasses the mic with a synthesized
 *    signal - a zero-hardware smoke test of the DSP chain.
 *
 * When the AD9851 board arrives: flip AD9851_ATTACHED to 1 (config.h) and
 * fill in ad9851_init()/ad9851_set_frequency() calls - the DSP/task/
 * timer/DAC structure here doesn't need to change either way.
 *
 * AUDIO_FX_ENABLED (config.h) turns on an optional pre-Hilbert
 * conditioning stage inside ssb_dsp itself (HPF + presence peak +
 * compressor, see ssb_dsp.h). Runs on plain mult/add per sample - no
 * measurable timing impact expected, but re-check the TIMING_DEBUG_GPIO
 * scope trace after enabling to confirm rather than assume.
 *
 * ADC: uses adc_continuous (DMA), not adc_oneshot - see adc_capture.h for
 * the full history of why.
 */

#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

 // Shared compile-time configuration - must come before any other project
 // header (gates #if blocks in several of them, e.g. PWM_COMPARISON_ENABLED
 // must be known before envelope_output.cpp includes "driver/ledc.h").
#include "config.h"

// Pre-defined settings tables (audio_source_t, PersistentSettings, presets)
#include "settings.h"

#include "ssb_dsp.h"

#include "dsp_state.h"
#include "adc_capture.h"
#include "test_signals.h"
#include "envelope_gdeq.h"
#if AD9851_ATTACHED
#include "relative_delay.h"
#include "carrier_output.h"
#endif
#include "envelope_output.h"
#include "diagnostics.h"
#include "serial_commands.h"

// (No TAG/ESP_LOG here - everything in this file uses Serial.printf so it's
// visible regardless of the IDE's Core Debug Level setting. ssb_dsp.c has
// its own separate TAG for its internal ESP_LOG calls.)

static TaskHandle_t s_dsp_task;

static bool IRAM_ATTR on_timer_alarm(gptimer_handle_t timer, const gptimer_alarm_event_data_t* edata, void* user_ctx)
{
    BaseType_t high_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_dsp_task, &high_task_woken);
    return high_task_woken == pdTRUE;
}

// IRAM_ATTR - keeps this task's code in internal RAM rather than flash,
// so it's immune to cache-line stalls caused by Core 1 activity (dac_task's
// I2C driver work) touching flash. Confirmed by disabling dac_task and
// seeing timing clean up - this is the structural fix rather than just
// working around it by leaving dac_task off. Every module function called
// from here is itself IRAM_ATTR for the same reason - see each module's
// header.
static void IRAM_ATTR dsp_task(void* arg)
{
    // Simple DC-blocking single-pole high-pass state (mic path only)
    float dc_estimate = 0.0f;
    const float dc_alpha = 0.995f;

    while (1) {
        // Block until the timer ISR notifies us - this sets our sample rate.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

#if TIMING_DEBUG_ENABLED
        digitalWrite(TIMING_DEBUG_GPIO, HIGH);
#endif
        int64_t t_start_us = esp_timer_get_time();
        diagnostics_record_tick_start(t_start_us);

        // Read once and reuse for both branches below (generation and
        // isolation-test dispatch) - the original .ino re-read the
        // volatile audio-source selector separately at each of the two
        // if/else chains, which could in principle see two different
        // values if a source switch landed mid-tick (loop() on Core 1
        // changing it concurrently with dsp_task on Core 0 reading it).
        // Reading it once here removes that theoretical split-brain
        // window; deliberate, called out here since everything else in
        // this refactor is a verbatim move.
        audio_source_t src = dsp_state_get_audio_source();

        float sample;
        if (src == AUDIO_SRC_TWOTONE) {
            sample = generate_twotone_sample();
        }
        else if (src == AUDIO_SRC_SINGLETONE) {
            sample = generate_singletone_sample();
        }
        else if (src == AUDIO_SRC_ENVSTEP) {
            sample = 0.0f;   // unused - ssb_dsp_process_sample() is bypassed entirely for this mode, see below
        }
        else if (src == AUDIO_SRC_FMTEST) {
            sample = 0.0f;   // unused - ssb_dsp_process_sample() is bypassed entirely for this mode too, see below
        }
        else if (src == AUDIO_SRC_AMTEST) {
            sample = 0.0f;   // unused - ssb_dsp_process_sample() is bypassed entirely for this mode too, see below
        }
        else {
            // Pops the next batch of raw samples from the ADC FIFO,
            // filters them (or passes through raw if bypassed), and
            // returns the latest value in ADC-code units - see
            // adc_capture.h for the full FIFO/filtering design.
            float filtered = adc_capture_read_next_sample();
            int raw = (int)filtered;
            // Normalize 12-bit ADC (0-4095) to roughly [-1, 1] with DC removal.
            sample = (float)raw / 2048.0f - 1.0f;
            dc_estimate = dc_alpha * dc_estimate + (1.0f - dc_alpha) * sample;
            sample -= dc_estimate;
        }
        int64_t t_adc_done_us = esp_timer_get_time();

        float freq_dev_hz = 0.0f;
        float envelope = 0.0f;
        float master_gain_linear = dsp_state_get_master_gain_linear();
        if (src == AUDIO_SRC_ENVSTEP) {
            envelope = test_signals_generate_envstep(master_gain_linear);
        }
        else if (src == AUDIO_SRC_FMTEST) {
            test_signals_generate_fmtest(&freq_dev_hz, &envelope);
        }
        else if (src == AUDIO_SRC_AMTEST) {
            test_signals_generate_amtest(master_gain_linear, &envelope, &freq_dev_hz);
        }
        else {
            ssb_dsp_process_sample(dsp_state_get_ssb(), sample, dsp_state_get_sideband(), &freq_dev_hz, &envelope);
        }
        int64_t t_dsp_done_us = esp_timer_get_time();

        // Envelope-path group-delay equalizer - see envelope_gdeq.h for
        // the coefficients/rationale. Applied here, unconditionally
        // across every audio source that reaches this point (mic/two-
        // tone/single-tone via ssb_dsp_process_sample above, or
        // ENVSTEP/AMTEST's own direct envelope assignment) - one
        // insertion point covers all of them identically, including the
        // isolation test modes, deliberately (see envelope_gdeq.h for
        // why that's useful rather than a shortcut). Off by default,
        // toggle via 'g'.
        envelope = envelope_gdeq_process(envelope);

        // envelope is roughly [0,1] for typical mic levels but not
        // rigorously bounded - clamp before handing off. Offset/scale are
        // runtime-tunable (see envelope_output.h's 'u'/'j'/'i'/'k'
        // handlers) - this is the PWM duty range, a separate knob from
        // master gain.
        envelope = envelope * envelope_output_get_pwm_scale() + envelope_output_get_pwm_offset();
        if (envelope < 0.0f) envelope = 0.0f;
        if (envelope > 1.0f) envelope = 1.0f;

        // PWM write goes FIRST now, before the AD9851 SPI transfer -
        // deliberately, not incidentally. ledc_set_duty()+ledc_update_duty()
        // is a near-instant register write, while ad9851_set_frequency()
        // takes a measured ~20-54us (the SPI clock time itself, see the
        // [timing] write_us figures via diagnostics_service()). Doing the
        // AD9851 write first (the original order, long before this
        // module split) meant the envelope's own register write didn't
        // happen until that whole SPI transfer had finished - adding
        // tens of microseconds of PURELY SOFTWARE-caused lag on top of
        // whatever PWM's own update-boundary timing and the analog
        // reconstruction filter's group delay already add downstream.
        // This reorder removes one real, measurable contributor to that
        // gap for free - it doesn't eliminate PWM's own inherent delay
        // or the analog filter's group delay, both of which still exist
        // after this register write completes.
        //
        // Both delay-line rings are always written/read together, BEFORE
        // either output is driven - relative_delay's current value
        // (runtime-tunable via '['/']', signed) decides whether the
        // freq_dev or the envelope side actually gets held back; the
        // other one reads its own just-written (undelayed) value. See
        // relative_delay.h for why this needs to be bipolar and
        // fractional.
        float delayed_envelope = envelope;
        float delayed_freq_dev_hz = freq_dev_hz;
#if AD9851_ATTACHED
        relative_delay_apply(freq_dev_hz, envelope, &delayed_freq_dev_hz, &delayed_envelope);
#endif

        envelope_output_write_pwm(delayed_envelope);

#if AD9851_ATTACHED
        uint32_t tx_freq = carrier_output_set_freq_dev(delayed_freq_dev_hz);

        // Unlike the envelope/freq_dev diagnostics below (which are the
        // PRE-delay values from ssb_dsp_process_sample and have always
        // been blind to whatever the delay line does), these are what's
        // ACTUALLY sent to the chip - the only way to directly verify
        // from firmware whether changing the relative delay ever alters
        // the computed frequency itself (it shouldn't - a pure sample
        // delay can't change frequency content - vs. just when a given
        // value gets sent).
        diagnostics_set_tx_info(delayed_freq_dev_hz, tx_freq);
#endif

        // Non-blocking, always succeeds - overwrites whatever was there.
        // dac_task will pick up the latest value whenever it next runs;
        // this call never waits on the I2C bus. Kept after both real
        // outputs above - this path only drives the (currently
        // disconnected) MCP4725, not RSET, so its own latency doesn't
        // affect the timing analysis above at all. Uses the UN-delayed
        // envelope, same as the original - only the PWM (RSET) output
        // above is relative-delayed.
        envelope_output_submit_dac_sample(envelope);

        diagnostics_set_envelope_freqdev(envelope, freq_dev_hz);

        // Diagnostics: plain volatile writes inside diagnostics.cpp, no
        // Serial/printf here - this stays cheap enough to leave enabled
        // permanently rather than only turning it on when chasing a
        // specific problem.
        int64_t t_write_done_us = esp_timer_get_time();
        uint32_t adc_us = (uint32_t)(t_adc_done_us - t_start_us);
        uint32_t dsp_us = (uint32_t)(t_dsp_done_us - t_adc_done_us);
        uint32_t write_us = (uint32_t)(t_write_done_us - t_dsp_done_us);
        uint32_t busy_us = (uint32_t)(t_write_done_us - t_start_us);
        diagnostics_record_phase_timings(adc_us, dsp_us, write_us, busy_us);

#if TIMING_DEBUG_ENABLED
        digitalWrite(TIMING_DEBUG_GPIO, LOW);
#endif
    }
}

static void init_sample_timer(void)
{
    gptimer_handle_t timer = NULL;
    gptimer_config_t timer_cfg = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 2000000, // 1MHz tick = 1us resolution
    };
    gptimer_new_timer(&timer_cfg, &timer);

    gptimer_event_callbacks_t cbs = {
        .on_alarm = on_timer_alarm,
    };
    gptimer_register_event_callbacks(timer, &cbs, NULL);

    gptimer_alarm_config_t alarm_cfg = {
        .alarm_count = 2000000 / SAMPLE_RATE_HZ,
        .reload_count = 0,
    };
    alarm_cfg.flags.auto_reload_on_alarm = true;  // nested dotted designators aren't valid C++
    gptimer_set_alarm_action(timer, &alarm_cfg);

    gptimer_enable(timer);
    gptimer_start(timer);
}

void setup()
{
    Serial.begin(921600);
    delay(1000);  // give USB CDC time to enumerate before we print - 200ms
    // wasn't enough on this board, confirmed empirically

// Direct confirmation of actual CPU clock - cheap, definitive, and
// worth checking given max_busy_us has been running ~3x higher than
// expected. 240 = full speed; if this prints 80 or 160, the board is
// NOT at max clock (check Arduino IDE: Tools > CPU Frequency) and
// that alone would explain a roughly-3x-too-slow measurement.
    Serial.printf("CPU ticks/us = %u (240 = full speed 240MHz)\r\n", esp_rom_get_cpu_ticks_per_us());

#if TIMING_DEBUG_ENABLED
    pinMode(TIMING_DEBUG_GPIO, OUTPUT);
    digitalWrite(TIMING_DEBUG_GPIO, LOW);
#endif

#if AD9851_ATTACHED
    carrier_output_init();
#endif

    ssb_dsp_config_t dsp_cfg = {
        .sample_rate_hz = SAMPLE_RATE_HZ,
        .num_taps = HILBERT_TAPS,
        .max_freq_dev_hz = MAX_FREQ_DEV_HZ,
        // Pre-Hilbert EQ (HPF + presence peak) and feed-forward compressor.
        // Runs on plain mults/adds per sample (no log/exp/pow in the hot
        // path - only at init), so this is negligible against the DSP
        // budget. Set .enable = false to go back to raw mic passthrough.
        .audio_fx = {
            .enable = AUDIO_FX_ENABLED,
            .hpf_freq_hz = 300.0f,
            .presence_freq_hz = 2200.0f,
            .presence_gain_db = 2.0f,
            .presence_q = 1.0f,
            .comp_threshold = 0.1f,
            .comp_ratio = 3.5f,
            .comp_attack_ms = 3.0f,
            .comp_release_ms = 120.0f,
        },
    };
    ESP_ERROR_CHECK(dsp_state_init(&dsp_cfg));

    // Testing session defaults - set explicitly here rather than baked
    // into ssb_dsp's own init defaults, which stay general-purpose (both
    // stages default to audio_fx.enable's value, gain to unity). Both
    // audio_fx stages off and a -2dB starting gain, per the current test
    // protocol - isolates the phase/envelope timing question from EQ/
    // compressor's own contribution to distortion.
    ssb_dsp_set_eq_enabled(dsp_state_get_ssb(), false);
    ssb_dsp_set_compressor_enabled(dsp_state_get_ssb(), false);
    dsp_state_set_master_gain_db(-2.0f);

    // Envelope group-delay equalizer coefficients - see envelope_gdeq.h.
    // Initialized (states zeroed) regardless of its enabled default, so
    // enabling it later via 'g' doesn't need a separate init path.
    envelope_gdeq_init();

    // Always started now, regardless of the initial audio source - needed
    // so the mic path is live and ready the moment a 't'/'s'/'m' serial
    // command switches source at runtime.
    adc_capture_init();
    envelope_output_init();

    // dsp_task on Core 0, high priority - the phase-critical path.
    xTaskCreatePinnedToCore(dsp_task, "ssb_dsp_task", 4096, NULL,
        configMAX_PRIORITIES - 2, &s_dsp_task, 0);

    init_sample_timer();
    diagnostics_init();

    Serial.printf("SSB mic test running: taps=%d fs=%uHz mode=%s ad9851=%s dac=MCP4725@0x%02X pwm_compare=%s\r\n",
        HILBERT_TAPS, SAMPLE_RATE_HZ,
        audio_source_name(dsp_state_get_audio_source()),
        AD9851_ATTACHED ? "attached" : "not attached (stubbed)",
        MCP4725_I2C_ADDR,
        PWM_COMPARISON_ENABLED ? "on" : "off");
    Serial.println("Send 't' for two-tone test signal, 's' for single-tone test signal, 'm' for live mic input, 'p' for envelope step test, 'y' for FM isolation test, 'h' for AM isolation test, 'f' to toggle the ADC LPF on/off, 'r' to reset diagnostics, 'v' to mute periodic diagnostics.");
    Serial.printf("Send 'e' to toggle EQ (currently %s), 'c' to toggle compressor (currently %s), "
        "'+'/'-' for master gain (currently %+.1fdB, %.1fdB/step).\r\n",
        ssb_dsp_get_eq_enabled(dsp_state_get_ssb()) ? "ON" : "off",
        ssb_dsp_get_compressor_enabled(dsp_state_get_ssb()) ? "ON" : "off",
        ssb_dsp_get_master_gain_db(dsp_state_get_ssb()), MASTER_GAIN_STEP_DB);
#if AD9851_ATTACHED
    Serial.printf("Send 'o' to toggle AD9851 RF output on/off (currently %s), "
        "'['/']' for relative phase/envelope delay (currently %+.2f samples, ~%+.0fus, "
        "%.2f/step - positive delays phase, negative delays envelope).\r\n",
        carrier_output_get_rf_enabled() ? "ON" : "off",
        relative_delay_get_samples(), relative_delay_get_samples() * 1000000.0f / SAMPLE_RATE_HZ,
        DELAY_STEP_SAMPLES);
#endif
    Serial.printf("Send 'u'/'j' for PWM duty range offset, 'i'/'k' for span "
        "(currently %.0f%%-%.0f%%, offset=%.2f scale=%.2f, %.0f%%/step).\r\n",
        envelope_output_get_pwm_offset() * 100.0f,
        (envelope_output_get_pwm_offset() + envelope_output_get_pwm_scale() > 1.0f
            ? 1.0f : envelope_output_get_pwm_offset() + envelope_output_get_pwm_scale()) * 100.0f,
        envelope_output_get_pwm_offset(), envelope_output_get_pwm_scale(), ENV_PWM_STEP * 100.0f);
    Serial.printf("Send 'g' to toggle the envelope group-delay equalizer (currently %s) - "
        "fitted against the original Sallen-Key filter's real LTspice response, "
        "not yet validated on hardware; re-tune '['/']' from scratch after enabling.\r\n",
        envelope_gdeq_get_enabled() ? "ON" : "off");
    // Presets (settings.h) load every lever above in one command - handy
    // once a preset is dialed in, no need to remember/retype the whole
    // sequence of individual knob commands every boot.
    Serial.print("Send '0'-'4' to load a preset: ");
    for (int i = 0; i < 5; i++) {
        Serial.printf("%d=%s%s", i, settingsPresets[i].name, i < 4 ? ", " : "\r\n");
    }
    Serial.println("Send 'P' to print the current settings as a single pasteable preset line for settings.h.");
}

void loop()
{
    handle_serial_commands();

    // Keep adc_continuous's internal pool from filling up - see
    // adc_capture.h. Low priority, not time-critical - fine to do here
    // alongside the other loop() work.
    adc_capture_service();

    // Throttled status/timing/adc diagnostic prints - see diagnostics.h.
    // This task is lower priority than both real-time tasks, so it never
    // competes with either for CPU time or bus access.
    diagnostics_service();

    delay(10);
}