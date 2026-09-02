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
 *             "latest value wins" queue and moves straight on. (Briefly
 *             tried on Core 1 instead during the Fs jitter hunt - see
 *             setup()'s "TRIED, REVERTED" note - real hardware starved
 *             Serial entirely, reverted.)
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
#include "soc/gpio_struct.h"   // GPIO.out_w1ts/w1tc - see on_timer_alarm()'s Fs jitter hunt toggle

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
#include "envelope_predistort.h"
#include "envelope_floor.h"
#if AD9851_ATTACHED
#include "relative_delay.h"
#include "carrier_output.h"
#endif
#include "envelope_output.h"
#include "envelope_interp.h"
#include "diagnostics.h"
#include "serial_commands.h"

// (No TAG/ESP_LOG here - everything in this file uses Serial.printf so it's
// visible regardless of the IDE's Core Debug Level setting. ssb_dsp.c has
// its own separate TAG for its internal ESP_LOG calls.)

static TaskHandle_t s_dsp_task;

static bool IRAM_ATTR on_timer_alarm(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx)
{
#if TIMING_DEBUG_ENABLED
    // Fs jitter hunt - see config.h's TIMING_DEBUG_GPIO_ISR comment. Raw
    // register toggle (not digitalWrite) - real ISR context, this MUST
    // stay IRAM-safe with zero risk of calling into a non-IRAM-resident
    // function. GPIO.out_w1ts/w1tc is a direct memory-mapped register
    // write, no function call at all - the standard idiom for exactly
    // this.
    //
    // Falling edge here = true ISR entry instant, zero added latency -
    // trigger/measure jitter off THIS edge, not the rising one. The
    // matching out_w1ts sits AFTER vTaskNotifyGiveFromISR() below instead
    // of immediately after this line, so the LOW pulse width becomes
    // "however long the notify call itself took" - free width for a
    // 100MHz scope to resolve (using real work's own duration, not an
    // added delay), instead of two back-to-back register writes that are
    // over almost as soon as they start.
    GPIO.out_w1tc = (1UL << TIMING_DEBUG_GPIO_ISR);
#endif
    BaseType_t high_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_dsp_task, &high_task_woken);
#if TIMING_DEBUG_ENABLED
    GPIO.out_w1ts = (1UL << TIMING_DEBUG_GPIO_ISR);   // rising edge = notify call done, about to return
#endif
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
    // Simple DC-blocking single-pole high-pass state (mic path only).
    // dc_alpha is derived from DC_BLOCK_TIME_CONSTANT_S (config.h) rather
    // than a hardcoded per-sample constant, so the real-world cutoff stays
    // the same regardless of SAMPLE_RATE_HZ.
    float dc_estimate = 0.0f;
    const float dc_alpha = expf(-1.0f / (SAMPLE_RATE_HZ * DC_BLOCK_TIME_CONSTANT_S));

#if AD9851_ATTACHED
    // One-time throwaway SPI transfer, BEFORE the real-time loop below
    // starts - pays a one-time cost here instead of on the loop's first
    // live tick. Confirmed via [timing]: a fresh boot showed a single
    // ~264-265us spi_us spike (reproducible across independent reboots -
    // a deterministic cost, not a scheduling fluke) on exactly the first
    // dsp_task tick, causing exactly one overrun/late tick, never
    // recurring afterward. Best explanation: carrier_output_init() (in
    // setup(), which runs on a different core than dsp_task - see this
    // function's own IRAM_ATTR comment above) already calls
    // ad9851_set_frequency() once, but that only warms ITS core's
    // i-cache; this task's own IRAM_ATTR only covers this project's own
    // code, not the ESP-IDF spi_master driver internals underneath
    // spi_device_polling_transmit() - those still live in flash unless
    // CONFIG_SPI_MASTER_IN_IRAM is set, so Core 0 pays its own first-call
    // cache-fill cost regardless of what Core 1 already ran. Harmless in
    // practice (no one is transmitting through this before dsp_task's
    // loop even starts), but free to eliminate here rather than on a
    // live sample.
    carrier_output_set_freq_dev(0.0f);
#endif

    // Fast-tick counter for envelope_interp's v4 design (see envelope_
    // interp.h): the sample gptimer itself now runs at ENVELOPE_INTERP_
    // FACTOR x SAMPLE_RATE_HZ, but the full DSP pipeline below still only
    // runs on 1 in ENVELOPE_INTERP_FACTOR of those wakes ("full" ticks,
    // still true SAMPLE_RATE_HZ) - the rest just walk envelope_interp's
    // linear ramp and return immediately. dsp_task-private, single-
    // threaded - see init_sample_timer() for the timer side of this.
    uint32_t fast_tick_count = 0;
    // Which ENVELOPE_INTERP_FACTOR-sized "group" of fast ticks the last
    // full tick belonged to (fast_tick_count / ENVELOPE_INTERP_FACTOR) -
    // see the is_full_tick check below for why this replaced a plain
    // "% ENVELOPE_INTERP_FACTOR == 0" check.
    uint32_t last_full_group = 0;

    while (1) {
        // Block until the timer ISR notifies us - this sets our fast tick
        // rate (ENVELOPE_INTERP_FACTOR x SAMPLE_RATE_HZ - see above).
        //
        // ulTaskNotifyTake(pdTRUE, ...) - xClearCountOnExit=pdTRUE - CLEARS
        // FreeRTOS's notification count to 0 on every take, but its RETURN
        // VALUE is how many times the ISR actually gave (i.e. how many
        // real fast-tick periods have elapsed) since our last take - if
        // dsp_task is ever even briefly late getting back here (a cache
        // stall, a moment of contention, anything), two or more real
        // 15.6us hardware periods can coalesce into a single wake. Bug
        // fix (found on real hardware: envelope timing was jittery, IMDs
        // shuffled, EVEN WITH 'I' fully disabled): the first cut of this
        // loop discarded that return value and did a bare fast_tick_
        // count++ per wake, which silently desyncs fast_tick_count from
        // the TRUE hardware period count the moment even one coalescing
        // event happens - after that, "full" ticks (where the real DSP
        // work and the real PWM write happen) start firing at the wrong,
        // irregular offsets relative to the true SAMPLE_RATE_HZ grid,
        // indefinitely, until another coalescing event randomly happens
        // to correct (or worsen) the drift. That's sample-clock jitter on
        // the DSP rate itself - explains both symptoms (irregular
        // envelope timing; IMDs moving both up and down, not uniformly,
        // which is what jitter does to a spurious floor). Fix: advance
        // fast_tick_count by the ACTUAL elapsed count, not by 1 - it then
        // always equals the true cumulative hardware period count since
        // boot, so long-run phase can never PERMANENTLY drift, no matter
        // how many wakes it took to get there.
        uint32_t elapsed_fast_ticks = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (elapsed_fast_ticks < 1) {
            elapsed_fast_ticks = 1;   // shouldn't happen (portMAX_DELAY blocks until >=1), defensive only
        }
        fast_tick_count += elapsed_fast_ticks;

        // Bug fix on top of the above (found on real hardware: envelope
        // still "moving around" even after the fix above landed): a bare
        // "fast_tick_count % ENVELOPE_INTERP_FACTOR == 0" check assumes a
        // coalescing event always lands EXACTLY on a multiple of
        // ENVELOPE_INTERP_FACTOR. It doesn't have to - if a full tick's
        // own DSP processing occasionally takes long enough that MORE
        // than ENVELOPE_INTERP_FACTOR real hardware periods elapse before
        // getting back here, fast_tick_count can jump straight PAST the
        // next full-tick boundary (e.g. 3 -> 8, skipping 4 entirely) - a
        // plain modulo check then stays false for several more ticks
        // until the count next happens to land exactly on a multiple,
        // meaning that period's real DSP sample (ADC/Hilbert/atan2/AD9851/
        // the lot) is skipped outright, not just an interpolation cosmetic
        // step - and the NEXT full tick is delayed by however many extra
        // sub-periods it takes to re-land on a clean multiple, compounding
        // the original overrun instead of just absorbing it. Comparing
        // which ENVELOPE_INTERP_FACTOR-sized GROUP fast_tick_count falls
        // into, rather than its exact remainder, fixes this: any update
        // that crosses one or more group boundaries is recognized as a
        // full tick immediately, on the very next wake, however far past
        // the exact boundary the coalesced count landed - so a bad
        // overrun still costs the one sample it made unrecoverable, but
        // never cascades into delaying subsequent ones too.
        uint32_t fast_tick_group = fast_tick_count / ENVELOPE_INTERP_FACTOR;
        bool is_full_tick = (fast_tick_group != last_full_group);
        if (is_full_tick) {
            last_full_group = fast_tick_group;
        }

        // Sine-chirp test mode ('w', AUDIO_SRC_CHIRP) - intercepted here,
        // BEFORE the is_full_tick check below, so it runs on EVERY fast
        // tick (the full ENVELOPE_INTERP_FACTOR x SAMPLE_RATE_HZ = 64kHz
        // rate), not just the 1-in-ENVELOPE_INTERP_FACTOR full ticks the
        // rest of this loop uses. A 20kHz chirp needs more than
        // SAMPLE_RATE_HZ's own 8kHz Nyquist, so this mode can't reuse the
        // normal 16kHz full-tick pipeline the way ENVSTEP/FMTEST/AMTEST
        // do - it bypasses ADC/ssb_dsp_process_sample/envelope_floor/gdeq/
        // relative_delay/AD9851/normal diagnostics, writing (almost)
        // straight to the PWM output and the reference GPIO instead. Raw
        // register writes for the reference pin (GPIO.out_w1ts/w1tc), not
        // digitalWrite - this runs at 64kHz, same real-time-sensitivity
        // reasoning as on_timer_alarm()'s own ISR-context register writes
        // (see config.h's TIMING_DEBUG_GPIO_ISR comment), even though this
        // call site is task, not ISR, context.
        //
        // "Almost" straight to the PWM output: the offset/scale (or
        // predistort) DC mapping is deliberately NOT skipped - see the
        // comment right before that call below for why, and 'u'/'j'/'i'/
        // 'k'/'D' in serial_commands.cpp for the knobs it wires in.
        if (dsp_state_get_audio_source() == AUDIO_SRC_CHIRP) {
            float chirp_envelope;
            bool ref_high;
            test_signals_generate_chirp(dsp_state_get_master_gain_linear(), &chirp_envelope, &ref_high);

            // Apply the SAME offset/scale (or predistort) DC mapping every
            // other source gets from the normal full-tick pipeline below -
            // deliberately NOT skipped here, unlike envelope_floor/gdeq
            // (see this block's own top comment for why those specific two
            // stay skipped). Master gain ('+'/'-', already passed into
            // test_signals_generate_chirp() above) only scales the SWING
            // around AM_TEST_DEPTH's fixed mean (same convention as
            // AMTEST) - it moves how HARD the filter is driven, not WHERE
            // on the duty range it's centered, so it can't reveal a
            // duty-range-dependent (DC-operating-point-dependent)
            // nonlinearity in the analog filter/BS170 gate stage. 'u'/'j'
            // (offset) and 'i'/'k' (scale) directly move that operating
            // point - exactly the knob needed to test the TF across
            // different parts of the duty range - so wiring them in here
            // is what actually answers that question, not more gain.
            if (envelope_predistort_get_enabled()) {
                chirp_envelope = envelope_predistort_process(chirp_envelope);
            } else {
                chirp_envelope = chirp_envelope * envelope_output_get_pwm_scale() + envelope_output_get_pwm_offset();
            }
            // Final safety clamp - same bounds/reasoning as the normal
            // pipeline's own clamp right before its envelope_output_write_
            // pwm() call: an offset/scale combination (or, at high master
            // gain, AMTEST's own "swing can push peaks past 1.0" case,
            // still possible pre-mapping above) can push this outside
            // [0,1] - flatten it here rather than wrapping the raw LEDC
            // duty register.
            if (chirp_envelope < 0.0f) chirp_envelope = 0.0f;
            if (chirp_envelope > 1.0f) chirp_envelope = 1.0f;
            envelope_output_write_pwm(chirp_envelope);
#if !CMD_DEBUG_PIN_ENABLED
            if (ref_high) {
                GPIO.out_w1ts = (1UL << CHIRP_REF_GPIO);
            } else {
                GPIO.out_w1tc = (1UL << CHIRP_REF_GPIO);
            }
#endif
            continue;
        }

        if (!is_full_tick) {
            // Cheap path: no ADC read, no DSP compute, no AD9851/DAC/
            // diagnostics work - just one more step of envelope_interp's
            // ramp (a no-op when interpolation is disabled). Everything
            // below this block is the ORIGINAL per-tick body, unchanged,
            // now only reached on the 1-in-ENVELOPE_INTERP_FACTOR "full"
            // ticks.
            envelope_interp_on_interp_tick();
            continue;
        }

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
        } else if (src == AUDIO_SRC_SINGLETONE) {
            sample = generate_singletone_sample();
        } else if (src == AUDIO_SRC_ENVSTEP) {
            sample = 0.0f;   // unused - ssb_dsp_process_sample() is bypassed entirely for this mode, see below
        } else if (src == AUDIO_SRC_FMTEST) {
            sample = 0.0f;   // unused - ssb_dsp_process_sample() is bypassed entirely for this mode too, see below
        } else if (src == AUDIO_SRC_AMTEST) {
            sample = 0.0f;   // unused - ssb_dsp_process_sample() is bypassed entirely for this mode too, see below
        } else {
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
        } else if (src == AUDIO_SRC_FMTEST) {
            test_signals_generate_fmtest(&freq_dev_hz, &envelope);
        } else if (src == AUDIO_SRC_AMTEST) {
            test_signals_generate_amtest(master_gain_linear, &envelope, &freq_dev_hz);
        } else {
            ssb_dsp_process_sample(dsp_state_get_ssb(), sample, dsp_state_get_sideband(), &freq_dev_hz, &envelope);
        }
        int64_t t_dsp_done_us = esp_timer_get_time();

        // Envelope-null floor - see envelope_floor.h (envelope-only clamp;
        // an earlier freq_dev_hz-freezing version was removed after
        // real-hardware testing showed it caused a hard phase
        // discontinuity - see that file for the postmortem). Must run on
        // the RAW envelope straight out of the block above, before
        // gdeq/predistort reshape it, so the clamp decision reflects the
        // actual signal. Off by default (floor 0.0), toggle via 'x'/'z'.
        envelope = envelope_floor_apply(envelope);

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
        // rigorously bounded - clamp before handing off either way.
        //
        // Two mutually exclusive ways to turn that [0,1] value into a PWM
        // duty: the original linear offset/scale mapping (runtime-tunable
        // via envelope_output.h's 'u'/'j'/'i'/'k' handlers - this is the
        // PWM duty range, a separate knob from master gain), or, if
        // enabled via 'D', envelope_predistort.h's measured lookup table -
        // see there for why this REPLACES the linear mapping rather than
        // stacking with it (the table's own domain already spans desired-
        // envelope-to-duty end to end). Off by default, same convention
        // as 'g'.
        if (envelope_predistort_get_enabled()) {
            envelope = envelope_predistort_process(envelope);
        } else {
            envelope = envelope * envelope_output_get_pwm_scale() + envelope_output_get_pwm_offset();
        }
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

        // t_start_us (captured at the very top of this tick, before any
        // DSP processing) is passed through so envelope_interp's ramp can
        // use it as its own t=0 reference - see envelope_interp.h's v4.1
        // note for why a timestamp taken at THIS call site instead
        // (after the full pipeline above has already run) would make the
        // ramp's timing wrong, not just imprecise.
        envelope_interp_on_full_tick(delayed_envelope, t_start_us);

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
        uint32_t adc_us   = (uint32_t)(t_adc_done_us   - t_start_us);
        uint32_t dsp_us   = (uint32_t)(t_dsp_done_us   - t_adc_done_us);
        uint32_t write_us = (uint32_t)(t_write_done_us - t_dsp_done_us);
        uint32_t busy_us  = (uint32_t)(t_write_done_us - t_start_us);
        diagnostics_record_phase_timings(adc_us, dsp_us, write_us, busy_us);

#if TIMING_DEBUG_ENABLED
        digitalWrite(TIMING_DEBUG_GPIO, LOW);
#endif
    }
}

// Fires at ENVELOPE_INTERP_FACTOR x SAMPLE_RATE_HZ now (envelope_interp.h's
// v4 design - see dsp_task's own fast-tick-counter comment for the other
// half of this). resolution_hz is scaled up by the same factor alarm_count
// is scaled down by, so alarm_count comes out to EXACTLY the same value
// (2000000/SAMPLE_RATE_HZ) it always was - this is still the identical
// "N ticks of a M Hz clock" relationship this timer has always used, just
// counting a faster clock the same number of ticks, not a new formula.
static void init_sample_timer(void)
{
    gptimer_handle_t timer = NULL;
    gptimer_config_t timer_cfg = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 2000000UL * ENVELOPE_INTERP_FACTOR,
        // Fs jitter hunt: intr_priority=3 (top of the commonly-usable
        // range for a plain C interrupt handler on the S3, since level 4+
        // requires special assembly-level handling this driver doesn't
        // use). Previously left unset (0 = "IDF picks whatever's
        // available"), which real hardware scope evidence pointed to as
        // the actual jitter mechanism: pin5 (this timer's alarm ISR
        // entry) occasionally had its ~15.625us nominal period stretch to
        // ~19us, and those stretches correlated tightly with pin13 (the
        // ADC continuous driver's own on_conv_done ISR, also Core 1)
        // landing within a few us of pin5's edge - consistent with the
        // two same-core ISRs occasionally colliding and one having to
        // wait for the other, since same-priority interrupts on Xtensa
        // don't nest/preempt each other. Xtensa DOES let a HIGHER-priority
        // interrupt preempt a lower one mid-ISR, so this explicitly raises
        // this timer's priority above the ADC driver's (still whatever
        // default/unset priority IDF gives it - no public API found to
        // lower it explicitly), letting gptimer's alarm cut in on an
        // in-progress ADC ISR instead of queuing behind it.
        //
        // CONFIRMED WIN on real hardware: compiled clean (this project's
        // IDF version does have this field, at this value), and pin5's
        // period tightened from occasional ~19us excursions down to
        // +/-0.5us around the 15.625us nominal - roughly a 7x reduction
        // in the period jitter that started this whole investigation.
        // Next to check: whether this also brings [timing]'s own
        // wakeup-jitter max_gap_us down off its previous 75-80us range
        // (nominal 62), and whether the audible/measured noise
        // improves - pin5's period is upstream evidence, not the final
        // verification.
        .intr_priority = 3,
    };
    gptimer_new_timer(&timer_cfg, &timer);

    gptimer_event_callbacks_t cbs = {
        .on_alarm = on_timer_alarm,
    };
    gptimer_register_event_callbacks(timer, &cbs, NULL);

    gptimer_alarm_config_t alarm_cfg = {
        .alarm_count = 2000000UL / SAMPLE_RATE_HZ,
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

    // Fs jitter hunt - see config.h's TIMING_DEBUG_GPIO_ISR comment.
    // GPIO_ADC's own pinMode/init lives in adc_capture_init() instead
    // (adc_capture.cpp) since that's the module that owns the ISR that
    // toggles it - kept together rather than split across files.
    pinMode(TIMING_DEBUG_GPIO_ISR, OUTPUT);
    digitalWrite(TIMING_DEBUG_GPIO_ISR, LOW);

    // New serial-activity-correlation pin - see config.h's
    // TIMING_DEBUG_GPIO_CMD comment.
    pinMode(TIMING_DEBUG_GPIO_CMD, OUTPUT);
    digitalWrite(TIMING_DEBUG_GPIO_CMD, LOW);
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
    envelope_interp_init();

    // dsp_task on Core 0, high priority - the phase-critical path.
    //
    // TRIED, REVERTED: pinned to Core 1 instead (matching gptimer's ISR
    // core, to eliminate the crosscore-IPI wake cost identified by the Fs
    // jitter hunt - see config.h's TIMING_DEBUG_GPIO_ADC comment and
    // diagnostics.cpp's Core-1-headroom comment for the reasoning that led
    // here, and envelope_interp.h's v4 notes for the v3 precedent this was
    // known to risk). Real hardware result: Serial output AND command
    // handling both went completely dead - not degraded, DEAD - meaning
    // loopTask wasn't just delayed, it got starved of Core 1 entirely once
    // dsp_task was also there. The actual RF/audio output stayed NORMAL
    // throughout (confirmed - it was the scope trace on the TIMING_DEBUG_*
    // pins that looked "messy", not the signal itself), so dsp_task's own
    // real-time work was NOT visibly disrupted - this looks like a clean,
    // one-sided starvation of loopTask specifically, not a general Core-1
    // breakdown. Still a harder failure than the pure CPU-budget math
    // predicted, though (dsp_task's own measured ~45-75% duty cycle should
    // have left tens of us of genuinely idle time per tick for loopTask to
    // run in) - exactly the v3 lesson repeating despite 'I' (envelope
    // interp, the specific thing v3's warning was about) being OFF, which
    // the CPU-budget reasoning didn't anticipate. Root cause NOT
    // understood yet - reverted rather than guessing at a fix blind.
    // Whatever mechanism actually starves loopTask here (scheduling
    // artifact specific to same-core ISR+task colocation? something about
    // how portYIELD_FROM_ISR interacts with a same-core notify vs a
    // crosscore one? some other task's behavior changing as a side
    // effect?) needs to be understood - ideally via a Guru Meditation/
    // hang diagnosis or a scaled-down repro - before trying this
    // direction again. The messy TIMING_DEBUG_* scope trace itself is a
    // live, not-yet-followed-up lead worth revisiting too - which pin,
    // and what "messy" actually looked like (irregular width vs period vs
    // missing edges), could point straight at the mechanism.
    xTaskCreatePinnedToCore(dsp_task, "ssb_dsp_task", 4096, NULL,
                             configMAX_PRIORITIES - 2, &s_dsp_task, 0);

    // TRIED, REVERTED: moved this call into dsp_task()'s own body (Core 0)
    // instead of here, on the theory that gptimer's alarm ISR living on
    // Core 1 (setup()/loop()'s default core) while dsp_task lives on Core
    // 0 was costing a cross-core IPI hop on every tick, explaining a
    // [timing] wakeup-jitter figure that didn't track busy_us. Real
    // hardware disagreed hard: regular reboots, and every [timing] number
    // got WORSE, not better (max_busy_us 44->62us i.e. right at the 62us
    // period edge, ADC actual sps collapsed to ~4.75k against an 80k
    // target, pool_ovf_total went from 0 to nonzero, dsp long-window rate
    // came in -2.6% off nominal instead of ~0.000%) - all consistent with
    // the board crash-looping rather than running steady-state slower.
    // Reverted back to calling it from here (unchanged from before that
    // experiment) pending an actual Guru Meditation / panic backtrace to
    // explain WHY moving it broke things, rather than guessing again
    // blind. See ssb_mic_test_commands.md / chat history around this date
    // for the full wakeup-jitter investigation this was chasing.
    init_sample_timer();
    diagnostics_init();

    Serial.printf("SSB mic test running: taps=%d fs=%uHz mode=%s ad9851=%s dac=MCP4725@0x%02X pwm_compare=%s\r\n",
             HILBERT_TAPS, SAMPLE_RATE_HZ,
             audio_source_name(dsp_state_get_audio_source()),
             AD9851_ATTACHED ? "attached" : "not attached (stubbed)",
             MCP4725_I2C_ADDR,
             PWM_COMPARISON_ENABLED ? "on" : "off");
    Serial.println("Send 't' for two-tone test signal, 's' for single-tone test signal, 'm' for live mic input, 'p' for envelope step test, 'y' for FM isolation test, 'h' for AM isolation test, 'f' to cycle the ADC LPF off/Butterworth/Chebyshev, 'r' to reset diagnostics, 'v' to mute periodic diagnostics, 'n' to cycle the null_bias diagnostic's envelope threshold (see '[dsp] null_bias' line).");
    Serial.printf("Send 'w' for the sine-chirp test mode (%.0fHz-%.0fHz log sweep over %.1fs, %.0fms mute/"
                  "sync marker at each restart, square-wave reference on pin%d) - characterizes the "
                  "envelope/PWM (RSET) analog filter's transfer function against an external ADC-based "
                  "measurement rig. 'u'/'j'/'i'/'k'/'D' still move the sweep's DC operating point on the "
                  "duty range (use these to test the filter across the range, NOT master gain, which "
                  "only scales swing depth around a fixed mean).\r\n",
                  CHIRP_F0_HZ, CHIRP_F1_HZ, CHIRP_SWEEP_SEC, CHIRP_MUTE_SEC * 1000.0f, CHIRP_REF_GPIO);
    Serial.printf("Send 'T' to step the two-tone pair through a spread of bands (currently f1=%.0fHz f2=%.0fHz) - "
                  "for mapping envelope/phase delay mismatch vs. frequency without a recompile per band.\r\n",
                  test_signals_get_twotone_f1_hz(), test_signals_get_twotone_f2_hz());
    Serial.printf("Send 'e' to toggle EQ (currently %s), 'c' to toggle compressor (currently %s), "
                  "'+'/'-' for master gain (currently %+.2fdB, %.1fdB/step), "
                  "'.'/',' for fine master gain (%.1fdB/step).\r\n",
                  ssb_dsp_get_eq_enabled(dsp_state_get_ssb()) ? "ON" : "off",
                  ssb_dsp_get_compressor_enabled(dsp_state_get_ssb()) ? "ON" : "off",
                  ssb_dsp_get_master_gain_db(dsp_state_get_ssb()), MASTER_GAIN_STEP_DB,
                  MASTER_GAIN_FINE_STEP_DB);
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
    Serial.printf("Send 'D' to toggle envelope pre-distortion (currently %s) - measured-curve lookup "
                  "table that REPLACES the 'u'/'j'/'i'/'k' linear offset/scale mapping while on, "
                  "not yet validated beyond the measurement itself.\r\n",
                  envelope_predistort_get_enabled() ? "ON" : "off");
    Serial.printf("Send 'I' to toggle %dx envelope output interpolation (currently %s) - smooths the "
                  "PWM duty staircase between DSP ticks to push its zero-order-hold spectral image "
                  "out past the analog filter's stopband (per QMX's own amplitude-interpolation "
                  "trick), not yet validated on real hardware.\r\n",
                  ENVELOPE_INTERP_FACTOR, envelope_interp_get_enabled() ? "ON" : "off");
    Serial.printf("Send 'C' to switch the 'I' interpolation curve (currently %s) - cycles "
                  "Catmull-Rom (smooth, but rounds off a two-tone null's sharp fold) -> LINEAR "
                  "(opposite tradeoff, straight-line ramp) -> HOLD (v4.4: no ramp at all, just "
                  "the same full-tick value written 4x at 64kHz - isolates the faster write RATE "
                  "from any ramp SHAPE) -> back to Catmull-Rom. Only affects output while 'I' is ON.\r\n",
                  envelope_interp_get_curve() == ENVELOPE_INTERP_CURVE_LINEAR ? "LINEAR"
                      : envelope_interp_get_curve() == ENVELOPE_INTERP_CURVE_HOLD ? "HOLD"
                      : "Catmull-Rom");
    Serial.printf("Send 'x'/'z' to raise/lower the envelope-null floor (currently %.2f) - smoothly "
                  "compresses envelope's [0,1] range into [floor,1], to keep two-tone nulls out "
                  "of the predistort LUT's steepest region; 0.00 = off.\r\n",
                  envelope_floor_get());
    {
        float slew = ssb_dsp_get_freq_dev_slew_limit_hz(dsp_state_get_ssb());
        if (slew >= SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ) {
            Serial.println("Send '{'/'}' to tighten/loosen the freq_dev slew-rate limit (currently off) - "
                          "smooths only the sharp per-sample frequency swing at a two-tone envelope "
                          "null (ordinary content never approaches it), leaving normal audio untouched.");
        } else {
            Serial.printf("Send '{'/'}' to tighten/loosen the freq_dev slew-rate limit (currently "
                          "%.0fHz/sample) - smooths only the sharp per-sample frequency swing at a "
                          "two-tone envelope null (ordinary content never approaches it), leaving "
                          "normal audio untouched.\r\n", slew);
        }
    }
    // Presets (settings.h) load every lever above in one command - handy
    // once a preset is dialed in, no need to remember/retype the whole
    // sequence of individual knob commands every boot. Loop bound taken
    // from the array itself (not hardcoded) so this banner can't silently
    // drift out of sync with settingsPresets again the way it did when
    // the array grew from 5 to 10 entries without this loop being
    // updated - see settings.h's static_assert for the compile-time half
    // of that same guard.
    const int preset_count = sizeof(settingsPresets) / sizeof(settingsPresets[0]);
    Serial.printf("Send '0'-'%d' to load a preset: ", preset_count - 1);
    for (int i = 0; i < preset_count; i++) {
        Serial.printf("%d=%s%s", i, settingsPresets[i].name, i < preset_count - 1 ? ", " : "\r\n");
    }
    Serial.println("Send 'P' to print the current settings as a single pasteable preset line for settings.h.");
}

void loop()
{
    // Core 1 breakdown - "what is Core 1 actually doing" (see the
    // [core1] idle% finding: both cores are essentially saturated, so
    // this answers where that time goes). Each call bracketed with
    // esp_timer_get_time(), summed into diagnostics.cpp's own
    // accumulators via one combined recorder call at the end - see
    // diagnostics_record_core1_loop_timings()'s header comment.
    int64_t t_cmd0 = esp_timer_get_time();
#if TIMING_DEBUG_ENABLED && CMD_DEBUG_PIN_ENABLED
    // See config.h's TIMING_DEBUG_GPIO_CMD comment - marks the exact
    // window handle_serial_commands() is running, to scope alongside
    // pin5 and test the "serial activity delays/disrupts gptimer's
    // alarm ISR" theory directly, rather than relying on manual timing.
    // Gated on CMD_DEBUG_PIN_ENABLED (config.h) - off by default now that
    // this same physical pin (13) is the chirp test mode's square-wave
    // reference output (CHIRP_REF_GPIO); the two must never toggle it at
    // once. Flip CMD_DEBUG_PIN_ENABLED back to 1 to re-enable this marker.
    digitalWrite(TIMING_DEBUG_GPIO_CMD, HIGH);
#endif
    handle_serial_commands();
#if TIMING_DEBUG_ENABLED && CMD_DEBUG_PIN_ENABLED
    digitalWrite(TIMING_DEBUG_GPIO_CMD, LOW);
#endif
    int64_t t_cmd1 = esp_timer_get_time();

    // Keep adc_continuous's internal pool from filling up - see
    // adc_capture.h. Low priority, not time-critical - fine to do here
    // alongside the other loop() work.
    adc_capture_service();
    int64_t t_adcsvc1 = esp_timer_get_time();

    // Throttled status/timing/adc diagnostic prints - see diagnostics.h.
    // This task is lower priority than both real-time tasks, so it never
    // competes with either for CPU time or bus access.
    diagnostics_service();
    int64_t t_diag1 = esp_timer_get_time();

    // delay(10) below used to go completely unmeasured - it's loop()'s
    // ONLY remaining code after the three calls above, so at a 10ms
    // request it should be by far the biggest chunk of every iteration
    // (idle/blocked, nothing else needs Core 1). It was landing entirely
    // in the old "other" bucket instead - see diagnostics_record_
    // core1_loop_timings()'s header comment for what that turned out to
    // mean for the [core1] idle hook reading too.
    int64_t t_delay0 = t_diag1;
    delay(10);
    int64_t t_delay1 = esp_timer_get_time();

    diagnostics_record_core1_loop_timings((uint32_t)(t_cmd1 - t_cmd0),
                                           (uint32_t)(t_adcsvc1 - t_cmd1),
                                           (uint32_t)(t_diag1 - t_adcsvc1),
                                           (uint32_t)(t_delay1 - t_delay0));
}
