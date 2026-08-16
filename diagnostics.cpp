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
