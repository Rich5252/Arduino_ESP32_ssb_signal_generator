/**
 * diagnostics.cpp - see diagnostics.h.
 */

#include "diagnostics.h"
#include "config.h"
#include "dsp_state.h"
#include "adc_capture.h"
#include "envelope_gdeq.h"
#include "envelope_output.h"
#include "ssb_dsp.h"
#include "carrier_output.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_freertos_hooks.h"   // esp_register_freertos_idle_hook_for_cpu() - see core1_idle_hook() below
#include <Arduino.h>

// Written by dsp_task, printed by diagnostics_service() on Core 1 at low
// priority - keeps Serial (slow) completely out of the real-time task.
static volatile float s_dbg_envelope = 0.0f;
static volatile float s_dbg_freq_dev = 0.0f;
#if AD9851_ATTACHED
static volatile float s_dbg_delayed_freq_dev = 0.0f;   // post-delay-line value, actually used
static volatile uint32_t s_dbg_tx_freq = 0;             // the exact integer Hz value sent to
                                                          // ad9851_set_frequency() - ground truth
#endif

static volatile uint32_t s_dbg_max_busy_us = 0;
static volatile uint32_t s_dbg_overrun_count = 0;
static const uint32_t k_sample_period_us = SSB_SAMPLE_PERIOD_US;

// WAKE-UP jitter - distinct from s_dbg_max_busy_us above, which only
// measures how long dsp_task's OWN work takes once it resumes. This
// measures the actual observed gap between successive ulTaskNotifyTake()
// returns - i.e. was dsp_task woken up ON TIME, regardless of how fast
// its own processing was. A task can have comfortable busy_us margin
// every single tick and STILL be intermittently woken late (preempted,
// scheduling delay, etc.) - that wouldn't show up in busy_us at all, but
// would still starve anything timing-sensitive that assumes a strictly
// periodic tick, like the ADC FIFO drain rate.
static volatile uint32_t s_dbg_max_tick_gap_us = 0;      // worst observed inter-tick gap
static volatile uint32_t s_dbg_late_tick_count = 0;      // ticks where the gap exceeded 1.5x nominal

// Phase breakdown of the same total: which part of dsp_task's work is
// actually costing the most.
static volatile uint32_t s_dbg_max_adc_us = 0;
static volatile uint32_t s_dbg_max_dsp_us = 0;
static volatile uint32_t s_dbg_max_write_us = 0;

static int64_t s_dsp_tick_start_us = 0;         // captured once at gptimer_start(), and again on reset
static volatile uint32_t s_dbg_dsp_tick_count = 0;  // incremented once per dsp_task tick, unconditionally

static volatile bool s_diag_muted = false;

// Core 1 headroom - see the Fs jitter hunt's crosscore-wake finding: the
// ~1us->6us stretch on gptimer's notify-from-ISR call only happens when
// dsp_task was genuinely blocked, and is the cost of the crosscore IPI
// needed to wake a Core-0-pinned task from a Core-1 ISR. The structural
// fix (co-locate the ISR and the task on one core) already crashed once
// moving the TIMER onto Core 0, which had zero spare CPU. This
// measurement is what cleared the mirror option - moving dsp_task itself
// onto Core 1 - as worth trying instead of guessing blind: once delay(10)
// was correctly attributed (see diagnostics_record_core1_loop_timings()'s
// header comment), Core 1 (hosting loop(), Serial/USB CDC,
// adc_capture_service(), and the ADC's own on_conv_done ISR) turned out
// to be sitting ~98-99% idle, not the ~3% the uncorrected idle-hook
// reading suggested - comfortable headroom by the CPU-time math for
// dsp_task's own ~45-75% duty cycle. TRIED anyway, REVERTED: real
// hardware starved Serial completely (output AND commands, not just
// delayed) despite that headroom - so CPU-time budget alone isn't the
// whole story for whatever makes this core-sharing arrangement fail; see
// the .ino's "TRIED, REVERTED" note on dsp_task's xTaskCreatePinnedToCore()
// call. dsp_task is back on Core 0; this idle/breakdown reading is still
// the right one to watch if that mirror option gets revisited once the
// actual starvation mechanism is understood.
//
// esp_register_freertos_idle_hook_for_cpu() calls core1_idle_hook() every
// time IDLE1 actually gets scheduled - i.e. only when Core 1 genuinely
// has nothing else ready to run (idle priority is the lowest there is,
// so nothing here can preempt real work). Delta-sum-with-threshold: the
// gap between two consecutive calls is either IDLE1's own tight-loop
// overhead (small, uninterrupted - genuine idle time, count it) or
// something else ran on Core 1 in between (large gap - NOT idle, must be
// excluded rather than mis-counted as spare budget). The threshold just
// needs to sit comfortably above the hook's own call-to-call overhead
// (expected well under 1us) and comfortably below any real task/ISR
// activity worth caring about.
//
// Both statics are touched ONLY from Core 1 (the hook itself, and the
// [core1] print/reset below, both run on Core 1 - the hook can never
// preempt the print since idle is the lowest priority) - no lock needed,
// same single-core-ownership reasoning as the ADC FIFO's head/tail split.
#define CORE1_IDLE_GAP_THRESHOLD_US 10
static uint64_t s_core1_idle_us_accum = 0;
static int64_t  s_core1_idle_last_call_us = 0;
static bool     s_core1_idle_hook_registered = false;

// Threshold-free cross-check on the idle% accounting above: a plain
// count of every hook call, no gap filtering at all. CORE1_IDLE_GAP_
// THRESHOLD_US was a guess, not calibrated against this actual hardware/
// IDF build - if idle% ever comes back suspiciously low (can't be
// explained by the other measured categories), a call rate that's also
// very low corroborates "Core 1 really is that busy"; a call rate that's
// still substantial while idle_us reads low would instead point at the
// threshold itself silently discarding real idle gaps that are just
// wider than 10us (e.g. if IDLE1's own per-iteration housekeeping on
// this IDF version costs more than assumed).
static uint32_t s_core1_idle_hook_calls = 0;

// Core 1 BREAKDOWN - "where does that ~96-97% busy time actually go".
// Sums (not high-water marks - see diagnostics_record_core1_loop_timings()'s
// own header comment for why), one per loop() sub-call, plus idle above.
// Same single-core-ownership reasoning as the idle accumulator - only
// ever touched from Core 1 (loop()'s own context), no lock needed.
static uint64_t s_core1_busy_cmd_us     = 0;   // handle_serial_commands()
static uint64_t s_core1_busy_adc_svc_us = 0;   // adc_capture_service()
static uint64_t s_core1_busy_diag_us    = 0;   // diagnostics_service() itself (mostly the
                                                // throttled [timing]/[adc]/[dsp] printf block)
static uint64_t s_core1_busy_delay_us   = 0;   // loop()'s own delay(10) - see
                                                // diagnostics_record_core1_loop_timings()'s
                                                // header comment for why this one bucket
                                                // turned out to explain the whole "other"
                                                // mystery

// esp_freertos_idle_cb_t is bool(*)(void), not void(*)(void) - confirmed
// on real hardware (this project's exact esp32s3-libs build rejected the
// void signature outright, -fpermissive error). Return value isn't ours
// to interpret here - this hook is just accumulating a measurement, not
// influencing idle-task behavior (light sleep, WDT feeding, etc., which
// are handled elsewhere) - true is the safe, do-nothing-special choice.
static bool IRAM_ATTR core1_idle_hook(void)
{
    s_core1_idle_hook_calls++;   // unconditional - the threshold-free cross-check, see its own comment
    int64_t now = esp_timer_get_time();
    if (s_core1_idle_last_call_us != 0) {
        int64_t gap = now - s_core1_idle_last_call_us;
        if (gap > 0 && gap <= CORE1_IDLE_GAP_THRESHOLD_US) {
            s_core1_idle_us_accum += (uint64_t)gap;
        }
    }
    s_core1_idle_last_call_us = now;
    return true;
}

void IRAM_ATTR diagnostics_record_tick_start(int64_t t_start_us)
{
    s_dbg_dsp_tick_count++;   // unconditional - counts real elapsed ticks regardless of mode

    // Measured first thing each tick, so it reflects the true wake-up-to-
    // wake-up gap rather than anything downstream.
    static int64_t s_last_tick_start_us = 0;
    if (s_last_tick_start_us != 0) {
        uint32_t gap_us = (uint32_t)(t_start_us - s_last_tick_start_us);
        if (gap_us > s_dbg_max_tick_gap_us) s_dbg_max_tick_gap_us = gap_us;
        if (gap_us > (k_sample_period_us + k_sample_period_us / 2)) s_dbg_late_tick_count++;
    }
    s_last_tick_start_us = t_start_us;
}

void IRAM_ATTR diagnostics_record_phase_timings(uint32_t adc_us, uint32_t dsp_us,
                                                 uint32_t write_us, uint32_t busy_us)
{
    if (adc_us   > s_dbg_max_adc_us)   s_dbg_max_adc_us   = adc_us;
    if (dsp_us   > s_dbg_max_dsp_us)   s_dbg_max_dsp_us   = dsp_us;
    if (write_us > s_dbg_max_write_us) s_dbg_max_write_us = write_us;
    if (busy_us  > s_dbg_max_busy_us)  s_dbg_max_busy_us  = busy_us;
    if (busy_us  > k_sample_period_us) s_dbg_overrun_count++;
}

void diagnostics_record_core1_loop_timings(uint32_t cmd_us, uint32_t adc_svc_us,
                                            uint32_t diag_us, uint32_t delay_us)
{
    s_core1_busy_cmd_us     += cmd_us;
    s_core1_busy_adc_svc_us += adc_svc_us;
    s_core1_busy_diag_us    += diag_us;
    s_core1_busy_delay_us   += delay_us;
}

void IRAM_ATTR diagnostics_set_envelope_freqdev(float envelope, float freq_dev_hz)
{
    s_dbg_envelope = envelope;
    s_dbg_freq_dev = freq_dev_hz;
}

void IRAM_ATTR diagnostics_set_tx_info(float delayed_freq_dev_hz, uint32_t tx_freq)
{
#if AD9851_ATTACHED
    s_dbg_delayed_freq_dev = delayed_freq_dev_hz;
    s_dbg_tx_freq = tx_freq;
#else
    (void)delayed_freq_dev_hz;
    (void)tx_freq;
#endif
}

// Owned by the [adc] 1-second rate print below; needs to survive a
// diagnostics_reset() so the very next print after a reset doesn't
// compute a bogus huge delta against stale pre-reset values.
static uint32_t s_last_samples_total = 0;
static uint32_t s_last_callback_count = 0;
static uint32_t s_last_rate_print_ms = 0;

void diagnostics_reset(void)
{
    s_dbg_max_busy_us = 0;
    s_dbg_overrun_count = 0;
    s_dbg_max_adc_us = 0;
    s_dbg_max_dsp_us = 0;
    s_dbg_max_write_us = 0;
    s_dbg_max_tick_gap_us = 0;
    s_dbg_late_tick_count = 0;
    s_dbg_dsp_tick_count = 0;
    s_dsp_tick_start_us = esp_timer_get_time();
    s_last_samples_total = 0;
    s_last_callback_count = 0;
    s_last_rate_print_ms = millis();
    s_core1_idle_us_accum = 0;
    s_core1_idle_last_call_us = 0;
    s_core1_idle_hook_calls = 0;
    s_core1_busy_cmd_us = 0;
    s_core1_busy_adc_svc_us = 0;
    s_core1_busy_diag_us = 0;
    s_core1_busy_delay_us = 0;
}

bool diagnostics_get_muted(void)
{
    return s_diag_muted;
}

void diagnostics_toggle_muted(void)
{
    s_diag_muted = !s_diag_muted;
    // Deliberately printed regardless of the new mute state - this
    // confirmation itself needs to always be visible, or muting silently
    // would just create a different confusing problem ("did that command
    // even register?").
    Serial.printf("-> periodic [timing]/[adc]/[dsp] diagnostics %s\r\n",
                  s_diag_muted ? "MUTED (command confirmations only)" : "resumed");
}

void diagnostics_init(void)
{
    s_dsp_tick_start_us = esp_timer_get_time();

    // Core 1 headroom measurement - see core1_idle_hook()'s own comment
    // above. cpuid=1 is passed explicitly to the registration call, so
    // it doesn't matter which core calls this function itself (setup()
    // runs on Core 1 anyway, but that's incidental here).
    esp_err_t err = esp_register_freertos_idle_hook_for_cpu(core1_idle_hook, 1);
    if (err == ESP_OK) {
        s_core1_idle_hook_registered = true;
    } else {
        // Non-fatal - just means the [core1] idle% line below will
        // always read 0% instead of a real measurement. Printed once
        // here rather than failing silently, since a 0% reading could
        // otherwise be mistaken for "genuinely no spare CPU" instead of
        // "hook never got registered".
        Serial.printf("WARNING: Core 1 idle hook registration failed (err=%d) - "
                      "[core1] idle%% will read as 0, not a real measurement\r\n", (int)err);
    }
}

static void print_status_line(void)
{
#if AD9851_ATTACHED
    Serial.printf("envelope=,%.3f  ,freq_dev=,%.1f,Hz  dac_code=,%u  ,delayed=,%.1f,Hz  tx_freq=,%u,Hz\r\n",
                  s_dbg_envelope, s_dbg_freq_dev, envelope_output_get_last_dac_code(),
                  s_dbg_delayed_freq_dev, s_dbg_tx_freq);
#else
    Serial.printf("envelope=,%.3f  ,freq_dev=,%.1f,Hz  dac_code=,%u\r\n",
                  s_dbg_envelope, s_dbg_freq_dev, envelope_output_get_last_dac_code());
#endif
}

static void print_timing_and_adc_block(uint32_t now)
{
    // mode= goes through audio_source_name() - correctly identifies
    // ENVSTEP/FMTEST/AMTEST, not just TWOTONE/SINGLETONE/mic (see
    // dsp_state.cpp's audio_source_name()). gdeq= tells you whether the
    // group-delay equalizer was on during this measurement window.
    Serial.printf("[timing] mode=%s gdeq=%s max_busy_us=%u (adc=%u dsp=%u write=%u) period_us=%u overruns=%u\r\n",
                  audio_source_name(dsp_state_get_audio_source()),
                  envelope_gdeq_get_enabled() ? "ON" : "off",
                  s_dbg_max_busy_us, s_dbg_max_adc_us, s_dbg_max_dsp_us, s_dbg_max_write_us,
                  k_sample_period_us, s_dbg_overrun_count);
    Serial.printf("[timing]   wakeup jitter: max_gap_us=%u (nominal=%u) late_ticks_total=%u\r\n",
                  s_dbg_max_tick_gap_us, k_sample_period_us, s_dbg_late_tick_count);

    // Sub-phase breakdown of dsp_us itself, from ssb_dsp's internal
    // profiling - lets us see which part of the DSP call (audio_fx, the
    // Hilbert FIR, or atan2f/sqrtf) is actually costing time, rather than
    // guessing again.
    ssb_dsp_profile_t prof;
    ssb_dsp_get_profile(dsp_state_get_ssb(), &prof);
    Serial.printf("[timing]   dsp breakdown: audio_fx=%u fir=%u atan2=%u sqrt=%u\r\n",
                  prof.max_audio_fx_us, prof.max_fir_us, prof.max_atan2_us, prof.max_sqrt_us);

#if AD9851_ATTACHED
    // Splits the [timing] line's write_us (dominated by the AD9851 SPI
    // write) into CPU-side prep (FTW math + bit-reversal loop) vs. the
    // spi_device_polling_transmit() call itself - see ad9851_profile_t
    // (AD9851.h) and the bus-acquire-once change in ad9851_init() this
    // is meant to validate the effect of.
    ad9851_profile_t ad_prof;
    carrier_output_get_profile(&ad_prof);
    Serial.printf("[timing]   ad9851 breakdown: prep_us=%u spi_us=%u (prep+spi=%u vs. write_us=%u "
                  "above - gap is remaining driver/call overhead)\r\n",
                  ad_prof.max_prep_us, ad_prof.max_spi_us,
                  ad_prof.max_prep_us + ad_prof.max_spi_us, s_dbg_max_write_us);
#endif

    // Evidence for setting MAX_FREQ_DEV_HZ from real data instead of
    // guessing again - max_unclamped is the TRUE peak deviation the
    // signal actually reaches (before any clamping), clip_count is how
    // many samples the clamp has actually had to intervene on.
    {
        ssb_dsp_freq_dev_stats_t fd_stats;
        ssb_dsp_get_freq_dev_stats(dsp_state_get_ssb(), &fd_stats);
        Serial.printf("[dsp]   freq_dev: max_unclamped=%.0fHz (limit=%.0fHz) clip_count=%u\r\n",
                      fd_stats.max_unclamped_freq_dev_hz, MAX_FREQ_DEV_HZ, fd_stats.clip_count);
    }

    // ADC continuity check: actual samples/callbacks seen in this ~1s
    // window vs. what ADC_CONT_SAMPLE_FREQ_HZ implies, plus any pool
    // overflow events. If "actual" comes in noticeably below "expected"
    // (or pool_ovf is nonzero), the stream has real gaps - the filter's
    // uniform-sample-spacing assumption is being violated, which would
    // explain artifacts no amount of filter debugging could fix.
    adc_capture_diag_t adc_diag;
    adc_capture_get_diag(&adc_diag);

    uint32_t elapsed_ms = now - s_last_rate_print_ms;
    if (elapsed_ms > 0) {
        uint32_t actual_sps   = (uint32_t)((uint64_t)(adc_diag.samples_total - s_last_samples_total) * 1000 / elapsed_ms);
        uint32_t expected_cbs = (uint32_t)((uint64_t)ADC_CONT_SAMPLE_FREQ_HZ * elapsed_ms
                                            / 1000 / ADC_CONT_FRAME_SAMPLES);
        Serial.printf("[adc] actual=%u sps (expected=%u) callbacks=%u (expected~%u) pool_ovf_total=%u\r\n",
                      actual_sps, ADC_CONT_SAMPLE_FREQ_HZ,
                      adc_diag.callback_count - s_last_callback_count, expected_cbs,
                      adc_diag.pool_ovf_count);

        // Core 1 headroom over this SAME ~1s window - see core1_idle_hook()'s
        // own comment. Snapshot-then-reset rather than a running total: a
        // per-window reading is more useful here than a cumulative-since-
        // boot average would be, since it stays responsive to whatever's
        // currently happening on Core 1 (a Serial burst, a mode switch)
        // instead of smoothing it away over the long run. Safe to read/
        // reset without a lock - see the statics' own comment above for
        // why (idle priority can never preempt this loop()-context code).
        if (!s_core1_idle_hook_registered) {
            Serial.printf("[core1] idle hook not registered - no measurement available\r\n");
        } else {
            double idle_pct = (double)s_core1_idle_us_accum * 100.0 / ((double)elapsed_ms * 1000.0);
            uint32_t hook_calls_per_sec = (uint32_t)((uint64_t)s_core1_idle_hook_calls * 1000 / elapsed_ms);
            Serial.printf("[core1] idle(hook)=%.1f%% over %ums (Core-1 idle hook reading - kept as a "
                          "cross-check, but see [core1] busy breakdown's delay=%% below for the trustworthy "
                          "number: this hook-based figure reads suspiciously low because a genuinely "
                          "blocked task's idle-hook calls land ~1 OS tick apart, not microseconds apart, "
                          "so CORE1_IDLE_GAP_THRESHOLD_US=10 excludes nearly all of a real long idle "
                          "block instead of counting it)\r\n",
                          idle_pct, elapsed_ms);
            Serial.printf("[core1]   idle hook calls/s=%u (threshold-free cross-check on idle(hook)%% "
                          "above - see CORE1_IDLE_GAP_THRESHOLD_US's comment)\r\n",
                          hook_calls_per_sec);
            s_core1_idle_us_accum = 0;
            s_core1_idle_hook_calls = 0;

            // Breakdown of the window - "where does Core 1's time actually
            // go". cmd/adc_svc/diag/delay are the four loop() sub-calls we
            // can measure directly (see diagnostics_record_
            // core1_loop_timings()'s header comment - delay(10) used to go
            // unmeasured and its time landed entirely in "other", which is
            // what made "other" look like ~96% of Core 1 before this
            // bucket was added). "other" here is deliberately NOT reduced
            // by idle(hook)% above - that figure covers the same physical
            // time as delay% but via the unreliable hook/threshold method,
            // so subtracting both would double-count and mask real
            // "other" cost under the clamp. What's left in "other" after
            // cmd/adc_svc/diag/delay is genuinely unaccounted for: ISR
            // time (chiefly the ADC's on_conv_done, which fires
            // continuously regardless of mode) plus USB CDC driver
            // overhead, neither of which loop() ever sees directly to
            // time itself.
            double window_us   = (double)elapsed_ms * 1000.0;
            double cmd_pct     = (double)s_core1_busy_cmd_us     * 100.0 / window_us;
            double adc_svc_pct = (double)s_core1_busy_adc_svc_us * 100.0 / window_us;
            double diag_pct    = (double)s_core1_busy_diag_us    * 100.0 / window_us;
            double delay_pct   = (double)s_core1_busy_delay_us   * 100.0 / window_us;
            double other_pct   = 100.0 - cmd_pct - adc_svc_pct - diag_pct - delay_pct;
            if (other_pct < 0.0) other_pct = 0.0;   // clamp - rounding/overlap across independently-measured windows, not a real negative cost
            Serial.printf("[core1]   busy breakdown: cmd=%.1f%% adc_svc=%.1f%% diag=%.1f%% delay=%.1f%% "
                          "other(ADC ISR + USB CDC + ...)=%.1f%%\r\n",
                          cmd_pct, adc_svc_pct, diag_pct, delay_pct, other_pct);
            s_core1_busy_cmd_us = 0;
            s_core1_busy_adc_svc_us = 0;
            s_core1_busy_diag_us = 0;
            s_core1_busy_delay_us = 0;
        }

        // Long-window average - much lower noise than the 1s figure
        // above, since averaging error shrinks with window length. This
        // is what to trust for pinning down the TRUE achieved ADC rate
        // vs. the requested ADC_CONT_SAMPLE_FREQ_HZ.
        int64_t elapsed_since_start_us = esp_timer_get_time() - adc_capture_get_start_us();
        if (elapsed_since_start_us > 0) {
            double long_avg_sps = (double)adc_diag.samples_total * 1000000.0 / (double)elapsed_since_start_us;
            double error_pct = (long_avg_sps - (double)ADC_CONT_SAMPLE_FREQ_HZ)
                                * 100.0 / (double)ADC_CONT_SAMPLE_FREQ_HZ;
            Serial.printf("[adc]   long-window avg=%.2f sps over %.1fs (%.3f%% vs nominal %uHz)\r\n",
                          long_avg_sps, elapsed_since_start_us / 1000000.0,
                          error_pct, ADC_CONT_SAMPLE_FREQ_HZ);

            // Same measurement for dsp_task's own tick rate (gptimer) -
            // only ever measured the ADC side precisely before. Chronic
            // FIFO starvation despite the ADC running fast (not slow)
            // only makes sense if THIS clock is also running fast, by
            // more than the ADC's own error.
            int64_t dsp_elapsed_us = esp_timer_get_time() - s_dsp_tick_start_us;
            if (dsp_elapsed_us > 0) {
                double long_avg_tps = (double)s_dbg_dsp_tick_count * 1000000.0 / (double)dsp_elapsed_us;
                double tick_error_pct = (long_avg_tps - (double)SAMPLE_RATE_HZ)
                                         * 100.0 / (double)SAMPLE_RATE_HZ;
                // The number that actually matters for the FIFO: the TRUE
                // ratio of the two measured rates, vs. the nominal
                // ADC_SAMPLES_PER_TICK the drain logic assumes. If this
                // deviates meaningfully from that nominal value, that -
                // not either clock's error in isolation - is the real
                // cause of sustained starvation or backlog.
                double true_ratio = long_avg_sps / long_avg_tps;
                Serial.printf("[dsp]   long-window avg=%.2f ticks/s (%.3f%% vs nominal %uHz) true_ratio=%.4f (nominal=%u)\r\n",
                              long_avg_tps, tick_error_pct, SAMPLE_RATE_HZ,
                              true_ratio, ADC_SAMPLES_PER_TICK);
            }
        }
        Serial.printf("[adc]   fifo: available now min=%u max=%u (want>=%u,<%u) starve_ticks_total=%u drop_total=%u\r\n",
                      adc_diag.fifo_min_available, adc_diag.fifo_max_available,
                      ADC_SAMPLES_PER_TICK, ADC_FIFO_SIZE,
                      adc_diag.fifo_starve_count, adc_diag.fifo_drop_count);
    }
    s_last_samples_total  = adc_diag.samples_total;
    s_last_callback_count = adc_diag.callback_count;
    s_last_rate_print_ms  = now;
}

void diagnostics_service(void)
{
    static uint32_t last_print_ms = 0;
    uint32_t now = millis();
    if (!s_diag_muted && now - last_print_ms >= 45) {
        last_print_ms = now;
        print_status_line();
    }

    static uint32_t last_timing_print_ms = 0;
    if (!s_diag_muted && now - last_timing_print_ms >= 1000) {
        last_timing_print_ms = now;
        print_timing_and_adc_block(now);
    }
}
