/**
 * adc_capture.cpp - see adc_capture.h.
 */

#include "adc_capture.h"
#include "config.h"
#include "ssb_adc_filter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_continuous.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "soc/gpio_struct.h"   // GPIO.out_w1ts/w1tc - see adc_conv_done_cb()'s Fs jitter hunt toggle
#include <Arduino.h>

#define ADC_UNIT       ADC_UNIT_1
#define ADC_CHANNEL    ADC_CHANNEL_5   // GPIO6 on ESP32-S3 - Micr input

static adc_continuous_handle_t s_adc;

static volatile uint16_t s_adc_fifo[ADC_FIFO_SIZE];
static volatile uint32_t s_adc_fifo_head = 0;   // written only by adc_conv_done_cb (ISR)
static volatile uint32_t s_adc_fifo_tail = 0;   // written only by adc_capture_read_next_sample() (dsp_task)

// Two 4th-order float biquad cascades (ssb_biquad4_t - two cascaded
// 2nd-order stages each, see ssb_adc_filter.h), both always initialized -
// safe here since only ever called from dsp_task now, never from the ISR
// (which stays integer-only, see adc_conv_done_cb). Only one is actually
// used per sample, selected by s_adc_lpf_mode below; keeping both
// initialized means switching modes live never needs a re-init, only a
// state reset (see the mode-transition handling in
// adc_capture_set_lpf_mode()).
static ssb_biquad4_t s_adc_lpf_butterworth;
static ssb_biquad4_t s_adc_lpf_chebyshev;

// Live OFF/Butterworth/Chebyshev toggle for the ADC LPF, via serial 'f' -
// see serial_commands.cpp. Lets you compare filtered-vs-raw (or one filter
// family vs the other) on the same physical signal without a rebuild, to
// check whether an artifact is actually coming from the filter or from
// somewhere else entirely.
static volatile adc_lpf_mode_t s_adc_lpf_mode = ADC_LPF_MODE_OFF;

// Continuity diagnostics: is the ADC stream actually gap-free at
// ADC_CONT_SAMPLE_FREQ_HZ, or are frames being dropped? The biquad's
// frequency response assumes uniform sample spacing - a stream with
// silent gaps isn't really "80kHz" even if most samples are on time.
// All ISR-incremented, plain integers, read/printed from loop() only
// (via adc_capture_get_diag()).
static volatile uint32_t s_dbg_adc_samples_total = 0;
static volatile uint32_t s_dbg_adc_callback_count = 0;
static volatile uint32_t s_dbg_adc_pool_ovf_count = 0;

// Does dsp_task ever find fewer than ADC_SAMPLES_PER_TICK samples waiting
// in the FIFO? The FIFO's capacity can't create lookahead - it only
// protects against overflow if dsp_task briefly lags. Whether a tick ever
// comes up short depends on the (fixed, boot-order-determined) phase
// between burst arrival and tick timing, not on FIFO size.
static volatile uint32_t s_dbg_adc_fifo_starve_count = 0;
static volatile uint32_t s_dbg_adc_fifo_min_available = 0xFFFFFFFFu;
static volatile uint32_t s_dbg_adc_fifo_max_available = 0;
static volatile uint32_t s_dbg_adc_fifo_drop_count = 0;
static int64_t s_adc_start_us = 0;

// adc_continuous's on_conv_done callback - ISR context, fires once per
// completed conversion frame. edata->conv_frame_buffer is a direct
// pointer into driver-owned memory (never free it) containing that
// frame's raw bytes. No adc_continuous_read() call here - the ESP-IDF
// docs are explicit that blocking driver calls don't belong in this
// callback; the pool still needs periodic draining, but that happens
// separately in adc_capture_service(), fully decoupled from this.
//
// Just pushes raw integers into the FIFO - no filtering happens here at
// all. adc_capture_read_next_sample() does the filtering, draining a
// fixed count per tick - see adc_capture.h for why this split is what
// actually solves both the noise and the timing issues.
static bool IRAM_ATTR adc_conv_done_cb(adc_continuous_handle_t handle,
                                        const adc_continuous_evt_data_t *edata,
                                        void *user_data)
{
#if TIMING_DEBUG_ENABLED
    // Fs jitter hunt - see config.h's TIMING_DEBUG_GPIO_ISR comment.
    // Scope this against TIMING_DEBUG_GPIO_ISR to test whether THIS ISR
    // (also Core 1, firing every ADC_CONT_FRAME_SAMPLES/
    // ADC_CONT_SAMPLE_FREQ_HZ) is the thing delaying gptimer's alarm ISR
    // from firing on time - i.e. ISR-to-ISR contention on Core 1.
    //
    // Falling edge here = true ISR entry instant, zero added latency -
    // this is the edge to trigger/measure jitter off. The matching
    // out_w1ts now sits at every return point below (see there) instead
    // of immediately after this line, so the LOW pulse width becomes
    // "however long this callback's own real work took" - free width for
    // a 100MHz scope to resolve, using time that was already being spent
    // here rather than adding any. Bonus: that width is itself a genuine
    // per-firing ISR-execution-time measurement.
    GPIO.out_w1tc = (1UL << TIMING_DEBUG_GPIO_ADC);
#endif

    uint32_t n = edata->size / SOC_ADC_DIGI_DATA_BYTES_PER_CONV;
    if (n == 0) {
#if TIMING_DEBUG_ENABLED
        GPIO.out_w1ts = (1UL << TIMING_DEBUG_GPIO_ADC);   // close the pulse on this early-return path too
#endif
        return false;
    }
    if (n > ADC_CONT_FRAME_SAMPLES) n = ADC_CONT_FRAME_SAMPLES;   // defensive - shouldn't happen

    s_dbg_adc_callback_count++;
    s_dbg_adc_samples_total += n;

    uint32_t head = s_adc_fifo_head;
    for (uint32_t i = 0; i < n; i++) {
        adc_digi_output_data_t *p = (adc_digi_output_data_t *)
            (edata->conv_frame_buffer + i * SOC_ADC_DIGI_DATA_BYTES_PER_CONV);

        uint32_t next_head = (head + 1) & ADC_FIFO_MASK;
        if (next_head == s_adc_fifo_tail) {
            // FIFO full - dsp_task has fallen behind by more than
            // ADC_FIFO_SIZE samples (shouldn't happen at 4x headroom
            // over one burst unless something else is starving it).
            // Drop rather than overwrite unread data or block in an ISR.
            // Distinct from s_dbg_adc_pool_ovf_count - that's the
            // DRIVER's own internal buffer overflowing before we even
            // see the data; this is OUR software FIFO, further downstream.
            //
            // Counts every remaining sample in THIS frame as dropped
            // (n - i), not just +1 for the break - an earlier version
            // incremented by 1 per callback-that-hit-full, which
            // massively undercounted true loss whenever a full 16-sample
            // frame arrived against an already-full FIFO (up to 16 lost,
            // only 1 counted).
            s_dbg_adc_fifo_drop_count += (n - i);
            break;
        }
        s_adc_fifo[head] = (uint16_t)p->type2.data;
        head = next_head;
    }
    s_adc_fifo_head = head;
#if TIMING_DEBUG_ENABLED
    GPIO.out_w1ts = (1UL << TIMING_DEBUG_GPIO_ADC);   // rising edge = this callback's real work is done
#endif
    return false;   // no higher-priority task needs waking from this event
}

// Fires if the underlying driver pool overflows - i.e. on_conv_done and/or
// the periodic drain in adc_capture_service() aren't keeping up, and
// conversion data is being lost. Integer-only, ISR-safe.
static bool IRAM_ATTR adc_pool_ovf_cb(adc_continuous_handle_t handle,
                                       const adc_continuous_evt_data_t *edata,
                                       void *user_data)
{
    s_dbg_adc_pool_ovf_count++;
    return false;
}

void adc_capture_init(void)
{
#if TIMING_DEBUG_ENABLED
    // Fs jitter hunt - see config.h's TIMING_DEBUG_GPIO_ISR comment and
    // adc_conv_done_cb()'s own toggle below. Set up here, before the
    // driver starts (end of this function), so the pin is a valid OUTPUT
    // before the ISR could possibly fire.
    pinMode(TIMING_DEBUG_GPIO_ADC, OUTPUT);
    digitalWrite(TIMING_DEBUG_GPIO_ADC, LOW);
#endif

    adc_continuous_handle_cfg_t handle_cfg = {
        .max_store_buf_size = ADC_CONT_BUF_BYTES,
        .conv_frame_size = ADC_CONT_FRAME_BYTES,
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&handle_cfg, &s_adc));

    adc_digi_pattern_config_t adc_pattern[1] = {
        {
            .atten = ADC_ATTEN_DB_12,
            .channel = ADC_CHANNEL,
            .unit = ADC_UNIT,
            .bit_width = ADC_BITWIDTH_12,
        },
    };
    adc_continuous_config_t dig_cfg = {
        .pattern_num = 1,
        .adc_pattern = adc_pattern,
        .sample_freq_hz = ADC_CONT_SAMPLE_FREQ_HZ,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2,   // S3 result format
    };
    ESP_ERROR_CHECK(adc_continuous_config(s_adc, &dig_cfg));

    // Must happen before dsp_task can possibly start draining the FIFO -
    // adc_capture_read_next_sample() calls ssb_biquad4_process() on
    // whichever filter is selected every tick once mic mode is active.
    // Both are initialized unconditionally regardless of the current mode,
    // so switching modes live (via 'f') never needs a re-init. Fine to
    // init here (task context, at startup).
    ssb_biquad4_lpf_init(&s_adc_lpf_butterworth, ADC_LPF_CUTOFF_HZ, (float)ADC_CONT_SAMPLE_FREQ_HZ);
    ssb_biquad4_chebyshev_lpf_init(&s_adc_lpf_chebyshev, ADC_LPF_CUTOFF_HZ, (float)ADC_CONT_SAMPLE_FREQ_HZ,
                                    ADC_LPF_CHEBYSHEV_RIPPLE_DB);

    // Must register before starting - the driver returns ESP_ERR_INVALID_STATE
    // if you try to add a callback while already running.
    adc_continuous_evt_cbs_t cbs = {
        .on_conv_done = adc_conv_done_cb,
        .on_pool_ovf  = adc_pool_ovf_cb,
    };
    ESP_ERROR_CHECK(adc_continuous_register_event_callbacks(s_adc, &cbs, NULL));

    ESP_ERROR_CHECK(adc_continuous_start(s_adc));
    s_adc_start_us = esp_timer_get_time();

    Serial.printf("adc_continuous started: target=%uHz (gptimer tick=%uHz), frame=%u samples, LPF=%.0fHz\r\n",
                  ADC_CONT_SAMPLE_FREQ_HZ, SAMPLE_RATE_HZ, ADC_CONT_FRAME_SAMPLES, ADC_LPF_CUTOFF_HZ);

    // ---- One-time raw diagnostic dump, NOT on the real-time path ----
    // Prints the exact bytes adc_continuous_read() actually returns
    // (individual raw samples, NOT the running average adc_conv_done_cb
    // computes for real-time use - this dump predates the averaging and
    // is purely a structural sanity check of the byte layout), plus how
    // our code decodes them (channel/data via adc_digi_output_data_t)
    // and the resolved value of SOC_ADC_DIGI_DATA_BYTES_PER_CONV itself.
    // The "channel" field decoded from each sample SHOULD read back as
    // ADC_CHANNEL (5) consistently - if it doesn't, that's hard evidence
    // the struct layout or per-sample byte stride assumed in the read
    // loop doesn't match this specific ESP-IDF version, rather than
    // guessing at the fix blind. Serial.printf, not ESP_LOGI - guaranteed
    // visible regardless of the IDE's Core Debug Level setting.
    Serial.printf("ADC debug: SOC_ADC_DIGI_DATA_BYTES_PER_CONV = %d (expected sample stride in bytes)\r\n",
                  (int)SOC_ADC_DIGI_DATA_BYTES_PER_CONV);
    vTaskDelay(pdMS_TO_TICKS(50));   // let a few real conversions accumulate
    {
        uint8_t dbg_buf[64];
        uint32_t dbg_bytes = 0;
        esp_err_t derr = adc_continuous_read(s_adc, dbg_buf, sizeof(dbg_buf), &dbg_bytes, 100);
        Serial.printf("ADC debug: read returned err=%s bytes=%u\r\n", esp_err_to_name(derr), dbg_bytes);
        for (uint32_t off = 0; off + SOC_ADC_DIGI_DATA_BYTES_PER_CONV <= dbg_bytes;
             off += SOC_ADC_DIGI_DATA_BYTES_PER_CONV) {
            adc_digi_output_data_t *p = (adc_digi_output_data_t *)(dbg_buf + off);
            Serial.printf("  [%u] raw=%02X %02X %02X %02X  decoded: channel=%u data=%u\r\n",
                          off / SOC_ADC_DIGI_DATA_BYTES_PER_CONV,
                          dbg_buf[off], dbg_buf[off + 1],
                          (SOC_ADC_DIGI_DATA_BYTES_PER_CONV > 2 ? dbg_buf[off + 2] : 0),
                          (SOC_ADC_DIGI_DATA_BYTES_PER_CONV > 3 ? dbg_buf[off + 3] : 0),
                          p->type2.channel, p->type2.data);
        }
    }
}

float IRAM_ATTR adc_capture_read_next_sample(void)
{
    static float s_last_filtered_adc = 2048.0f;
    adc_lpf_mode_t mode = s_adc_lpf_mode;

    uint32_t tail = s_adc_fifo_tail;
    uint32_t head = s_adc_fifo_head;   // snapshot - ISR may still be advancing it, fine for a single consumer
    uint32_t available = (head - tail) & ADC_FIFO_MASK;

    if (available < s_dbg_adc_fifo_min_available) s_dbg_adc_fifo_min_available = available;
    if (available > s_dbg_adc_fifo_max_available) s_dbg_adc_fifo_max_available = available;
    if (available < ADC_SAMPLES_PER_TICK) s_dbg_adc_fifo_starve_count++;

    // Nominal N/tick whenever that many are genuinely available - NOT
    // "everything available up to the cap" (an earlier version did that,
    // which greedily drained the FIFO to near-zero the instant any burst
    // landed, then starved for the next several ticks until the next one
    // arrived - reintroducing the exact zero-order-hold staircase problem
    // this whole FIFO design was meant to fix).
    //
    // The "+1" branch handles the small persistent surplus (true_ratio
    // measured ~4.032 at the old 9600Hz rate, not exactly the nominal
    // ratio - see diagnostics.cpp's long-window [dsp] print) by
    // opportunistically taking one extra sample whenever one happens to
    // already be waiting, continuously bleeding off the surplus in the
    // smallest possible increment. Without this, a fixed cap-of-32
    // catch-up threshold (see ADC_SAMPLES_PER_TICK_MAX) still works, but
    // a slow surplus takes many tens of ms to accumulate enough to
    // trigger it - producing periodic gulps that show up as regular
    // visible/audible spikes. Soaking up 1 extra sample at a time instead
    // means the correction is spread continuously rather than
    // concentrated into periodic jolts.
    //
    // ADC_SAMPLES_PER_TICK_MAX is still checked first and kept as a
    // genuine-backlog fallback (startup, mode switch) - that scenario
    // needs to recover fast, not trickle back 1 sample at a time.
    uint32_t to_pop;
    if (available > ADC_SAMPLES_PER_TICK_MAX) {
        to_pop = ADC_SAMPLES_PER_TICK_MAX;              // genuine backlog - catch up fast
    } else if (available >= ADC_SAMPLES_PER_TICK + 1) {
        to_pop = ADC_SAMPLES_PER_TICK + 1;              // small surplus present - bleed off 1
    } else if (available >= ADC_SAMPLES_PER_TICK) {
        to_pop = ADC_SAMPLES_PER_TICK;                  // normal case - steady nominal pace
    } else {
        to_pop = available;                              // genuinely starved - take what's there
    }
    for (uint32_t i = 0; i < to_pop; i++) {
        float raw = (float)s_adc_fifo[tail];
        switch (mode) {
            case ADC_LPF_MODE_BUTTERWORTH:
                s_last_filtered_adc = ssb_biquad4_process(&s_adc_lpf_butterworth, raw);
                break;
            case ADC_LPF_MODE_CHEBYSHEV:
                s_last_filtered_adc = ssb_biquad4_process(&s_adc_lpf_chebyshev, raw);
                break;
            case ADC_LPF_MODE_OFF:
            default:
                s_last_filtered_adc = raw;
                break;
        }
        tail = (tail + 1) & ADC_FIFO_MASK;
    }
    s_adc_fifo_tail = tail;

    return s_last_filtered_adc;
}

void adc_capture_service(void)
{
    // Keep adc_continuous's internal pool from filling up. The
    // on_conv_done callback (see adc_conv_done_cb) already captures every
    // frame's newest sample for dsp_task's use as it arrives - this call's
    // only job is freeing up the underlying pool so it doesn't overflow
    // (on_pool_ovf), so its contents are simply discarded. Low priority,
    // not time-critical. Runs unconditionally - the ADC is always active
    // (see adc_capture_init(), called unconditionally from setup())
    // regardless of the current audio source, so this pool needs draining
    // either way.
    uint8_t drain_buf[256];
    uint32_t drain_bytes = 0;
    while (adc_continuous_read(s_adc, drain_buf, sizeof(drain_buf), &drain_bytes, 0) == ESP_OK
           && drain_bytes > 0) {
        // discarded
    }
}

void adc_capture_set_lpf_mode(adc_lpf_mode_t mode)
{
    adc_lpf_mode_t prev = s_adc_lpf_mode;

    // Only one filter's state actually advances per sample (see the
    // switch in adc_capture_read_next_sample()) - the OTHER filter's z1/z2
    // sit frozen at whatever they were the last time IT was active, which
    // could be a long time ago (or never, if this is its first use since
    // boot - though init already zeroed it then). Switching TO a filtered
    // mode from a DIFFERENT mode resets that filter's state first, so the
    // first sample after switching doesn't get fed a stale/discontinuous
    // z1/z2 - same reset-on-transition reasoning as
    // envelope_gdeq_set_enabled()'s off->on reset. A no-op "switch" (mode
    // unchanged, e.g. reapplying the same preset) does NOT reset, so a
    // filter already running continues running continuously rather than
    // glitching every time.
    if (mode != prev) {
        if (mode == ADC_LPF_MODE_BUTTERWORTH) {
            ssb_biquad4_reset(&s_adc_lpf_butterworth);
        } else if (mode == ADC_LPF_MODE_CHEBYSHEV) {
            ssb_biquad4_reset(&s_adc_lpf_chebyshev);
        }
        // Switching TO off needs no reset - raw passthrough has no state.
    }

    s_adc_lpf_mode = mode;
}

adc_lpf_mode_t adc_capture_get_lpf_mode(void)
{
    return s_adc_lpf_mode;
}

const char *adc_capture_lpf_mode_name(adc_lpf_mode_t mode)
{
    switch (mode) {
        case ADC_LPF_MODE_BUTTERWORTH: return "Butterworth";
        case ADC_LPF_MODE_CHEBYSHEV:   return "Chebyshev";
        case ADC_LPF_MODE_OFF:
        default:                      return "off";
    }
}

void adc_capture_reset_diag(void)
{
    s_dbg_adc_samples_total = 0;
    s_dbg_adc_callback_count = 0;
    s_dbg_adc_pool_ovf_count = 0;
    s_dbg_adc_fifo_starve_count = 0;
    s_dbg_adc_fifo_min_available = 0xFFFFFFFFu;
    s_dbg_adc_fifo_max_available = 0;
    s_dbg_adc_fifo_drop_count = 0;
    s_adc_start_us = esp_timer_get_time();   // restarts the long-window average from now
}

void adc_capture_get_diag(adc_capture_diag_t *out)
{
    out->samples_total = s_dbg_adc_samples_total;
    out->callback_count = s_dbg_adc_callback_count;
    out->pool_ovf_count = s_dbg_adc_pool_ovf_count;
    out->fifo_starve_count = s_dbg_adc_fifo_starve_count;
    out->fifo_min_available = s_dbg_adc_fifo_min_available;
    out->fifo_max_available = s_dbg_adc_fifo_max_available;
    out->fifo_drop_count = s_dbg_adc_fifo_drop_count;
}

int64_t adc_capture_get_start_us(void)
{
    return s_adc_start_us;
}
