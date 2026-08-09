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
 *
 * AUDIO_FX_ENABLED (below) turns on an optional pre-Hilbert conditioning
 * stage inside ssb_dsp itself (HPF + presence peak + compressor, see
 * ssb_dsp.h). Runs on plain mult/add per sample - no measurable timing
 * impact expected, but re-check the TIMING_DEBUG_GPIO scope trace after
 * enabling to confirm rather than assume.
 *
 * ADC: uses adc_continuous (DMA), not adc_oneshot. Measured overhead of
 * adc_oneshot_read() at this sample rate (~147us/call) made it unusable
 * for a 50us period - adc_oneshot is documented as intended for
 * occasional reads, not audio-rate polling. adc_continuous free-runs the
 * ADC into a DMA buffer on its own clock (ADC_CONT_SAMPLE_FREQ_HZ,
 * slightly above SAMPLE_RATE_HZ); dsp_task's gptimer-driven cadence is
 * UNCHANGED - it still fires at exactly SAMPLE_RATE_HZ, one sample per
 * tick, which is what keeps AD9851/PWM updates correctly paced. Each
 * tick just pops whatever's newest out of the DMA buffer (cheap) instead
 * of triggering a fresh blocking conversion (expensive). See dsp_task
 * and init_adc() for the details.
 */

#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gptimer.h"
#include "driver/i2c.h"
#include "esp_adc/adc_continuous.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

// Defined here (before includes that depend on it) rather than down with
// the other PWM defines below - #if needs this to already be known.
#define PWM_COMPARISON_ENABLED 1
#if PWM_COMPARISON_ENABLED
#include "driver/ledc.h"
#endif

#define dac_task_enabled 0

#include "ssb_dsp.h"
#include "ssb_adc_filter.h"

// (No TAG/ESP_LOG here - everything in this file uses Serial.printf so it's
// visible regardless of the IDE's Core Debug Level setting. ssb_dsp.c has
// its own separate TAG for its internal ESP_LOG calls.)

// ---- Set to 1 once the AD9851 board is wired up and its driver calls
// below are filled in. Until then this runs mic->DSP->DAC standalone. ----
#define AD9851_ATTACHED 0

// ---- Two-tone test mode: bypass the mic ADC with a synthesized signal.
// Zero-hardware smoke test of the DSP chain. ----
#define TWOTONE_TEST_MODE   1
#define TWOTONE_F1_HZ        700.0f
#define TWOTONE_F2_HZ       1900.0f
#define TWOTONE_AMPLITUDE    0.45f   // keep below 0.5 so peaks don't clip when summed

// ---- Pre-Hilbert audio conditioning (HPF + presence EQ + compressor) ----
// See ssb_dsp.h's ssb_audio_fx_config_t for the individual parameters,
// set below in dsp_cfg.audio_fx. Leave off with TWOTONE_TEST_MODE if you
// want to look at the raw DSP chain's spurious performance without any
// conditioning in the signal path.
#define AUDIO_FX_ENABLED 0

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

// ---- adc_continuous (DMA) config, replacing adc_oneshot ----
// adc_oneshot's per-call overhead (~147us measured) is documented as
// unsuitable for audio-rate polling - it's built for occasional reads.
//
// HISTORY (kept for context, since this took a few iterations to land on):
// v1 (polling adc_continuous_read() from dsp_task each tick) hit two
// separate problems depending on frame size: a large frame (64 samples
// @ ~21kHz, ~3ms to fill) meant most ticks found nothing ready and reused
// a stale sample for milliseconds at a time (measured as "low and
// non-monotonic, with inversions"). Shrinking the frame to reduce
// staleness (4 samples @ 80kHz) instead made adc_continuous_read() itself
// land at nearly the SAME rate as dsp_task's own tick, and the read call
// jumped to ~50-53us regardless of further frame/rate tuning - evidence
// that ~50us is close to the real cost of a genuinely successful
// (non-stale) read on this build, not something more tuning would fix.
//
// v2: register adc_continuous's on_conv_done EVENT CALLBACK instead of
// polling. The callback runs in ISR context right when a frame completes
// and hands us a direct pointer to the frame data
// (adc_continuous_evt_data_t::conv_frame_buffer) - no adc_continuous_read()
// call needed there at all (the ESP-IDF docs are explicit that read()
// isn't ISR-safe to call from within the callback anyway). The callback
// just pushes raw integers into s_adc_fifo now (see the AVERAGING note
// below and s_adc_fifo); dsp_task drains a fixed count per tick and does
// the actual filtering. No driver call and no blocking in the hot
// ISR/task path either way.
//
// The underlying pool still needs periodic draining via
// adc_continuous_read() to avoid on_pool_ovf - see loop(), where this
// happens at low priority on Core 1, fully decoupled from dsp_task.
//
// AVERAGING (superseded, kept for context): frame=1 (the original version
// of this section) meant the callback only ever kept the single freshest
// raw ADC reading - real oversampling was happening (ADC running faster
// than the tick rate) but none of it was being used to reduce noise,
// since every sample but the last was simply discarded. A later version
// averaged across the whole frame (N=16 boxcar, ~200us window,
// sqrt(16)=4x noise reduction) instead of just taking the tail sample.
//
// SWAPPED FOR A REAL FILTER - four iterations to land on the current
// design. Spectrum-analyser check of the raw ADC floor (mic grounded)
// showed it's flat/broadband, so oversampling+LPF is the right approach
// in principle - but a boxcar's stopband is leaky (sinc sidelobes only
// ~-13dB down at best) and it only updated once per frame (200us @
// N=16), leaving a zero-order-hold staircase between updates:
//   1. Float biquad (ssb_adc_filter.h) directly in adc_conv_done_cb -
//      crashed immediately (Guru Meditation, CoprocessorException):
//      Xtensa ISRs have no FPU register-save area, float math isn't
//      legal there at all.
//   2. Float biquad moved to dsp_task, fed via a ring buffer from the
//      ISR (integer-only) that dsp_task fully DRAINED every tick -
//      worked once ADC_CONT_FRAME_SAMPLES dropped to 4 (matching the
//      ISR firing rate to the tick rate), but that also meant the ISR
//      fired every tick, and draining+filtering in dsp_task every tick
//      added real measured cost (~20-26us) against the ~50us budget.
//   3. Q15 fixed-point biquad run directly back in the ISR, to avoid
//      that per-tick task cost - timing didn't improve and a genuine
//      limit-cycle bug turned up (fixed, feedback terms were using the
//      truncated output instead of full Q15 precision) but STILL didn't
//      fix the noise on a sweep test. Turned out frame size was never
//      really the issue for the filter itself (it processes every
//      sample regardless of N) - only ever exposing the LAST sample of
//      each burst is what tied output cadence to frame size.
//   4. Current: split the two concerns apart properly. adc_conv_done_cb
//      goes back to pure integer copying (any N is fine now, no ISR
//      arithmetic cost at all) into s_adc_fifo; dsp_task pops a FIXED
//      count per tick (ADC_SAMPLES_PER_TICK, the true Fs_adc/Fs_dsp
//      ratio) and filters each one in order, in task context (float is
//      safe there). The FIFO absorbs the burstiness of "16 samples
//      arrive together every 4th tick" while dsp_task drains a steady 4
//      every tick - so every tick gets a genuinely fresh, individually
//      filtered sample, without needing the ISR to fire that often.
//      ADC_CONT_FRAME_SAMPLES can go back up for ISR efficiency; it no
//      longer has any bearing on filter correctness or output cadence.
// ADC_LPF_CUTOFF_HZ (below) is the actual tuning knob for the noise/
// bandwidth tradeoff now; not yet verified by ear or against real
// spurious data - flagged for the same "listen to it, don't assume"
// treatment the old N value got.
#define ADC_CONT_SAMPLE_FREQ_HZ   80000u   // within ESP32-S3's continuous-mode range
#define ADC_CONT_FRAME_SAMPLES    16    // DMA chunk size only now - see AVERAGING note above.
                                         // If adc_continuous_new_handle() errors on this, the
                                         // driver enforces a different frame-size constraint -
                                         // report the exact error and we'll adjust.
#define ADC_CONT_FRAME_BYTES      (ADC_CONT_FRAME_SAMPLES * SOC_ADC_DIGI_DATA_BYTES_PER_CONV)
// 4x the original size - 4096 bytes (1024 samples, ~12.7ms headroom @
// ~80.6kHz actual) turned out to be smaller than loop() could stall for
// on a slow-baud Serial.printf() burst, causing real on_pool_ovf events
// (see Serial.begin() and the [adc] pool_ovf_total diagnostic). Raising
// baud rate is the primary fix; this is cheap additional margin on top.
#define ADC_CONT_BUF_BYTES        16384  // ~4096 samples, ~50ms headroom @ 80kHz

// 2nd-order Butterworth LPF applied per raw sample in adc_conv_done_cb,
// replacing the old N=16 boxcar average - see the AVERAGING comment
// above. Sits at the top of the voice band on purpose (same reasoning
// the old 5kHz boxcar corner used): filtering broadband ADC noise
// without eating wanted audio. Real tuning knob now - not yet verified
// by ear or spectrum analyser with the AD9851 in the loop.
#define ADC_LPF_CUTOFF_HZ         3000.0f

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
static adc_continuous_handle_t s_adc;

// Raw ADC samples cross the ISR->task boundary through this FIFO as
// plain integers - the ISR does NO filtering at all now, just copies.
// This decouples two things that were previously tangled together:
//   - ADC_CONT_FRAME_SAMPLES (how many samples the DMA/ISR handles per
//     interrupt) - can now be whatever's efficient for the driver/ISR
//     overhead, independent of anything else.
//   - The output UPDATE RATE dsp_task sees - governed instead by how
//     many items dsp_task pops off this FIFO each tick (see
//     ADC_SAMPLES_PER_TICK and dsp_task below), not by how often the ISR
//     happens to fire.
// dsp_task pops a FIXED count per tick (the true Fs_adc/Fs_dsp ratio,
// exactly 4 here) regardless of burst arrival pattern - the FIFO absorbs
// the burstiness of "16 samples arrive together every 4th tick" and
// dsp_task drains a steady 4/tick, so every tick gets a genuinely fresh
// sample without needing the ISR to fire that often. Single-producer
// (ISR)/single-consumer (dsp_task), so plain volatile head/tail is fine.
//
// (History: tried a float biquad directly in the ISR first - crashed,
// Xtensa ISRs have no FPU register-save area. Then tried filtering in
// dsp_task fed by a ring buffer that dsp_task fully DRAINED each tick -
// worked at N=4 but cost real per-tick time once the ISR fired every
// tick. Then tried Q15 fixed-point filtering back in the ISR to cut that
// cost - still noisy, and going back to first principles: the actual
// bug was conflating "ISR frame size" with "output refresh rate" by only
// ever exposing the LAST sample of each burst. This design separates
// them properly - fixed per-tick consumption from the FIFO, so frame
// size can go back up for ISR efficiency without reintroducing the
// staircase. Filtering is back to float, safe now since it only ever
// runs from dsp_task/task context.)
#define ADC_FIFO_SIZE   64   // power of two; headroom over one ADC_CONT_FRAME_SAMPLES-sized burst
#define ADC_FIFO_MASK   (ADC_FIFO_SIZE - 1)
static volatile uint16_t s_adc_fifo[ADC_FIFO_SIZE];
static volatile uint32_t s_adc_fifo_head = 0;   // written only by adc_conv_done_cb (ISR)
static volatile uint32_t s_adc_fifo_tail = 0;   // written only by dsp_task

// How many raw ADC samples dsp_task consumes from the FIFO each tick -
// must be the exact Fs_adc/Fs_dsp ratio for the FIFO to neither grow nor
// starve in steady state. 80000/20000 = 4 exactly; if either rate ever
// changes, check this stays an integer division with zero remainder.
#define ADC_SAMPLES_PER_TICK   (ADC_CONT_SAMPLE_FREQ_HZ / SAMPLE_RATE_HZ)

static ssb_biquad_t s_adc_lpf;   // float biquad - safe here since only ever called from dsp_task now,
                                  // never from the ISR (which stays integer-only, see adc_conv_done_cb)

// Continuity diagnostics: is the ADC stream actually gap-free at
// ADC_CONT_SAMPLE_FREQ_HZ, or are frames being dropped? The biquad's
// frequency response assumes uniform sample spacing - a stream with
// silent gaps isn't really "80kHz" even if most samples are on time.
// All ISR-incremented, plain integers, read/printed from loop() only
// (same rules as the other s_dbg_* counters).
static volatile uint32_t s_dbg_adc_samples_total = 0;   // running total, incremented by n each on_conv_done
static volatile uint32_t s_dbg_adc_callback_count = 0;  // how many times on_conv_done has fired
static volatile uint32_t s_dbg_adc_pool_ovf_count = 0;  // on_pool_ovf events - direct evidence of drops

// Does dsp_task ever find fewer than ADC_SAMPLES_PER_TICK samples
// waiting in s_adc_fifo? The FIFO's capacity can't create lookahead - it
// only protects against overflow if dsp_task briefly lags. Whether a
// tick ever comes up short depends on the (fixed, boot-order-determined)
// phase between burst arrival and tick timing, not on FIFO size. Written
// only by dsp_task, read/printed from loop() - same rules as the other
// s_dbg_* counters despite not being ISR-written this time.
static volatile uint32_t s_dbg_adc_fifo_starve_count = 0;      // ticks where available < ADC_SAMPLES_PER_TICK
static volatile uint32_t s_dbg_adc_fifo_min_available = 0xFFFFFFFFu;  // running low-water mark
static volatile uint32_t s_dbg_adc_fifo_drop_count = 0;        // our FIFO overflowing (distinct from driver pool_ovf)

static TaskHandle_t s_dsp_task;
static TaskHandle_t s_dac_task;
static QueueHandle_t s_envelope_queue;   // length 1, "latest value wins" (xQueueOverwrite)
static volatile ssb_sideband_t s_sideband = SSB_SIDEBAND_USB;

// Runtime switch between two-tone test signal and live mic input, toggled
// from loop() via serial commands 't'/'m' (see there). Starts from
// TWOTONE_TEST_MODE's compile-time value so existing behavior is
// unchanged if you never send a command, but dsp_task now branches on
// this variable rather than #if - both code paths are always compiled
// in and the ADC always runs (see init_adc() call in setup(), no longer
// conditional) so switching is instant with no re-init needed.
static volatile bool s_twotone_mode = (TWOTONE_TEST_MODE != 0);

// Live A/B toggle for the ADC LPF, via serial 'f' - see loop(). Lets you
// compare filtered-vs-raw on the same physical signal without a rebuild,
// to check whether an artifact is actually coming from the filter or
// from somewhere else entirely (this is what caught that a fixed-point
// limit-cycle fix changed nothing observable - useful to keep checking
// empirically rather than assuming, now that filtering runs a third way).
static volatile bool s_adc_lpf_bypass = false;

// Written by dsp_task/dac_task, printed by loop() on Core 1 at low
// priority - keeps Serial (slow) completely out of both real-time tasks.
static volatile float s_dbg_envelope = 0.0f;
static volatile float s_dbg_freq_dev = 0.0f;
static volatile uint16_t s_dbg_dac_code = 0;

// Worst-case timing diagnostics for dsp_task. Read/printed from loop()
// only (never from dsp_task itself - no Serial calls on the real-time
// path). max_busy_us is a running high-water mark, never reset, so it
// captures the worst case seen since boot even if it only happens once.
// overrun_count increments any sample whose processing took longer than
// one sample period - if this climbs, dsp_task is at risk of never
// yielding back to ulTaskNotifyTake, which starves IDLE0 and trips the
// task watchdog (this is what happened before the denormal-flush fix).
static volatile uint32_t s_dbg_max_busy_us = 0;
static volatile uint32_t s_dbg_overrun_count = 0;
static const uint32_t k_sample_period_us = 1000000UL / SAMPLE_RATE_HZ;

// Phase breakdown of the same total: which part of dsp_task's work is
// actually costing the most. Same rules as above - volatile, plain
// writes only, read/printed from loop(), never touched from dsp_task
// beyond these updates.
static volatile uint32_t s_dbg_max_adc_us = 0;
static volatile uint32_t s_dbg_max_dsp_us = 0;
static volatile uint32_t s_dbg_max_write_us = 0;

static bool IRAM_ATTR on_timer_alarm(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx)
{
    BaseType_t high_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_dsp_task, &high_task_woken);
    return high_task_woken == pdTRUE;
}

// adc_continuous's on_conv_done callback - ISR context, fires once per
// completed conversion frame. edata->conv_frame_buffer is a direct
// pointer into driver-owned memory (never free it) containing that
// frame's raw bytes. No adc_continuous_read() call here - the ESP-IDF
// docs are explicit that blocking driver calls don't belong in this
// callback; the pool still needs periodic draining, but that happens
// separately in loop() (see there), fully decoupled from this.
//
// Just pushes raw integers into s_adc_fifo - no filtering happens here
// at all. dsp_task does the filtering, draining a fixed count per tick -
// see the comment by s_adc_fifo for why this split is what actually
// solves both the noise and the timing issues.
static bool IRAM_ATTR adc_conv_done_cb(adc_continuous_handle_t handle,
                                        const adc_continuous_evt_data_t *edata,
                                        void *user_data)
{
    uint32_t n = edata->size / SOC_ADC_DIGI_DATA_BYTES_PER_CONV;
    if (n == 0) return false;
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
            s_dbg_adc_fifo_drop_count++;
            break;
        }
        s_adc_fifo[head] = (uint16_t)p->type2.data;
        head = next_head;
    }
    s_adc_fifo_head = head;
    return false;   // no higher-priority task needs waking from this event
}

// Fires if the underlying driver pool overflows - i.e. on_conv_done and/or
// the periodic drain in loop() aren't keeping up, and conversion data is
// being lost. We were never actually registering this before (the pool
// was only being drained to PREVENT overflow, never checked to see if it
// happened anyway) - direct evidence for whether the "continuous 80kHz"
// assumption behind the filter actually holds. Integer-only, ISR-safe.
static bool IRAM_ATTR adc_pool_ovf_cb(adc_continuous_handle_t handle,
                                       const adc_continuous_evt_data_t *edata,
                                       void *user_data)
{
    s_dbg_adc_pool_ovf_count++;
    return false;
}

// Always compiled in now (not gated on TWOTONE_TEST_MODE) since dsp_task
// branches on the runtime s_twotone_mode flag and can switch to this at
// any time via the 't' serial command - see s_twotone_mode.
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
        int64_t t_start_us = esp_timer_get_time();

        float sample;
        if (s_twotone_mode) {
            sample = generate_twotone_sample();
        } else {
            // Pops exactly ADC_SAMPLES_PER_TICK raw samples from
            // s_adc_fifo (fewer only if genuinely starved, e.g. the
            // first few ticks at startup before the first burst has
            // arrived) and runs each through s_adc_lpf HERE, in task
            // context - safe for float now, unlike the ISR that fills
            // this FIFO. Filtering every individual sample in order,
            // at a steady rate matched to production, is what actually
            // keeps this gap-free AND smooth - see the comment by
            // s_adc_fifo for the reasoning.
            static float s_last_filtered_adc = 2048.0f;
            bool bypass = s_adc_lpf_bypass;
            {
                uint32_t tail = s_adc_fifo_tail;
                uint32_t head = s_adc_fifo_head;   // snapshot - ISR may still be advancing it, fine for a single consumer
                uint32_t available = (head - tail) & ADC_FIFO_MASK;

                if (available < s_dbg_adc_fifo_min_available) s_dbg_adc_fifo_min_available = available;
                if (available < ADC_SAMPLES_PER_TICK) s_dbg_adc_fifo_starve_count++;

                uint32_t to_pop = (available < ADC_SAMPLES_PER_TICK) ? available : ADC_SAMPLES_PER_TICK;
                for (uint32_t i = 0; i < to_pop; i++) {
                    float raw = (float)s_adc_fifo[tail];
                    s_last_filtered_adc = bypass ? raw : ssb_biquad_process(&s_adc_lpf, raw);
                    tail = (tail + 1) & ADC_FIFO_MASK;
                }
                s_adc_fifo_tail = tail;
            }
            int raw = (int)s_last_filtered_adc;
            // Normalize 12-bit ADC (0-4095) to roughly [-1, 1] with DC removal.
            sample = (float)raw / 2048.0f - 1.0f;
            dc_estimate = dc_alpha * dc_estimate + (1.0f - dc_alpha) * sample;
            sample -= dc_estimate;
        }
        int64_t t_adc_done_us = esp_timer_get_time();

        float freq_dev_hz = 0.0f;
        float envelope = 0.0f;
        ssb_dsp_process_sample(s_ssb, sample, s_sideband, &freq_dev_hz, &envelope);
        int64_t t_dsp_done_us = esp_timer_get_time();

#if AD9851_ATTACHED
        uint32_t tx_freq = s_carrier_hz + (int32_t)freq_dev_hz;
        ad9851_set_frequency(s_ad9851, tx_freq);
#endif

        // envelope is roughly [0,1] for typical mic levels but not
        // rigorously bounded - clamp before handing off.
        envelope = envelope * 0.9 + 0.2;
        if (envelope < 0.0f) envelope = 0.0f;
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

        // Diagnostics: plain volatile writes, no Serial/printf here -
        // this stays cheap enough to leave enabled permanently rather
        // than only turning it on when chasing a specific problem.
        int64_t t_write_done_us = esp_timer_get_time();
        uint32_t adc_us   = (uint32_t)(t_adc_done_us   - t_start_us);
        uint32_t dsp_us   = (uint32_t)(t_dsp_done_us   - t_adc_done_us);
        uint32_t write_us = (uint32_t)(t_write_done_us - t_dsp_done_us);
        uint32_t busy_us  = (uint32_t)(t_write_done_us - t_start_us);
        if (adc_us   > s_dbg_max_adc_us)   s_dbg_max_adc_us   = adc_us;
        if (dsp_us   > s_dbg_max_dsp_us)   s_dbg_max_dsp_us   = dsp_us;
        if (write_us > s_dbg_max_write_us) s_dbg_max_write_us = write_us;
        if (busy_us  > s_dbg_max_busy_us)  s_dbg_max_busy_us  = busy_us;
        if (busy_us  > k_sample_period_us) s_dbg_overrun_count++;

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
    // it calls ssb_biquad_process() on s_adc_lpf every tick once mic mode
    // is active. Fine to init here (task context, at startup).
    ssb_biquad_lpf_init(&s_adc_lpf, ADC_LPF_CUTOFF_HZ, (float)ADC_CONT_SAMPLE_FREQ_HZ);

    // Must register before starting - the driver returns ESP_ERR_INVALID_STATE
    // if you try to add a callback while already running.
    adc_continuous_evt_cbs_t cbs = {
        .on_conv_done = adc_conv_done_cb,
        .on_pool_ovf  = adc_pool_ovf_cb,
    };
    ESP_ERROR_CHECK(adc_continuous_register_event_callbacks(s_adc, &cbs, NULL));

    ESP_ERROR_CHECK(adc_continuous_start(s_adc));

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
    // the struct layout or per-sample byte stride assumed in dsp_task's
    // read loop doesn't match this specific ESP-IDF version, rather than
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
        // Pre-Hilbert EQ (HPF + presence peak) and feed-forward compressor.
        // Runs on plain mults/adds per sample (no log/exp/pow in the hot
        // path - only at init), so this is negligible against the DSP
        // budget. Set .enable = false to go back to raw mic passthrough.
        .audio_fx = {
            .enable = AUDIO_FX_ENABLED,
            .hpf_freq_hz = 300.0f,
            .presence_freq_hz = 2200.0f,
            .presence_gain_db = 4.0f,
            .presence_q = 1.0f,
            .comp_threshold = 0.3f,
            .comp_ratio = 3.5f,
            .comp_attack_ms = 3.0f,
            .comp_release_ms = 120.0f,
        },
    };
    ESP_ERROR_CHECK(ssb_dsp_init(&dsp_cfg, &s_ssb));

    // Always started now, regardless of s_twotone_mode's initial value -
    // needed so the mic path is live and ready the moment a 't'/'m'
    // serial command switches modes at runtime (see s_twotone_mode).
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

    // dsp_task on Core 0, high priority - the phase-critical path.
    xTaskCreatePinnedToCore(dsp_task, "ssb_dsp_task", 4096, NULL,
                             configMAX_PRIORITIES - 2, &s_dsp_task, 0);

    init_sample_timer();

    Serial.printf("SSB mic test running: taps=%d fs=%uHz mode=%s ad9851=%s dac=MCP4725@0x%02X pwm_compare=%s\r\n",
             HILBERT_TAPS, SAMPLE_RATE_HZ,
             s_twotone_mode ? "TWO-TONE TEST" : "mic",
             AD9851_ATTACHED ? "attached" : "not attached (stubbed)",
             MCP4725_I2C_ADDR,
             PWM_COMPARISON_ENABLED ? "on" : "off");
    Serial.println("Send 't' for two-tone test signal, 'm' for live mic input, 'f' to toggle the ADC LPF on/off.");
}

void loop()
{
    // Runtime source switch: 't' -> two-tone test signal, 'm' -> live mic
    // input. dsp_task reads s_twotone_mode fresh every tick, so this
    // takes effect on the very next sample - no glitch/restart needed.
    // Unrecognized bytes (e.g. line endings from some serial monitors)
    // are silently ignored rather than echoed as an error.
    while (Serial.available()) {
        char c = Serial.read();
        if (c == 't' && !s_twotone_mode) {
            s_twotone_mode = true;
            Serial.println("-> two-tone test signal");
        } else if (c == 'm' && s_twotone_mode) {
            s_twotone_mode = false;
            Serial.println("-> live mic input");
        } else if (c == 'f') {
            s_adc_lpf_bypass = !s_adc_lpf_bypass;
            Serial.printf("-> ADC LPF %s\r\n", s_adc_lpf_bypass ? "BYPASSED (raw)" : "active");
        }
    }

    // Keep adc_continuous's internal pool from filling up. The
    // on_conv_done callback (see adc_conv_done_cb) already captures every
    // frame's newest sample for dsp_task's use as it arrives - this call's
    // only job is freeing up the underlying pool so it doesn't overflow
    // (on_pool_ovf), so its contents are simply discarded. Low priority,
    // not time-critical - fine to do here alongside the other loop() work.
    // Runs unconditionally now - the ADC is always active (see init_adc()
    // in setup()) regardless of s_twotone_mode, so this pool needs
    // draining either way.
    {
        uint8_t drain_buf[256];
        uint32_t drain_bytes = 0;
        while (adc_continuous_read(s_adc, drain_buf, sizeof(drain_buf), &drain_bytes, 0) == ESP_OK
               && drain_bytes > 0) {
            // discarded
        }
    }

    // Diagnostics only - throttled well below the sample rate, and this
    // task is lower priority than both real-time tasks, so it never
    // competes with either for CPU time or bus access.
    static uint32_t last_print_ms = 0;
    uint32_t now = millis();
    if (now - last_print_ms >= 45) {
        last_print_ms = now;
        Serial.printf("envelope=,%.3f  ,freq_dev=,%.1f,Hz  dac_code=,%u\r\n",
                      s_dbg_envelope, s_dbg_freq_dev, s_dbg_dac_code);
    }

    // Worst-case dsp_task timing, once a second - watch max_busy_us stay
    // under period_us with margin, and overruns stay at 0. If overruns
    // climb, dsp_task risks starving IDLE0 and tripping the task
    // watchdog - see the k_sample_period_us comment above.
    static uint32_t last_timing_print_ms = 0;
    if (now - last_timing_print_ms >= 1000) {
        last_timing_print_ms = now;
        Serial.printf("[timing] max_busy_us=%u (adc=%u dsp=%u write=%u) period_us=%u overruns=%u\r\n",
                      s_dbg_max_busy_us, s_dbg_max_adc_us, s_dbg_max_dsp_us, s_dbg_max_write_us,
                      k_sample_period_us, s_dbg_overrun_count);

        // Sub-phase breakdown of dsp_us itself, from ssb_dsp's internal
        // profiling - lets us see which part of the DSP call (audio_fx,
        // the Hilbert FIR, or atan2f/sqrtf) is actually costing time,
        // rather than guessing again.
        ssb_dsp_profile_t prof;
        ssb_dsp_get_profile(s_ssb, &prof);
        Serial.printf("[timing]   dsp breakdown: audio_fx=%u fir=%u atan2=%u sqrt=%u\r\n",
                      prof.max_audio_fx_us, prof.max_fir_us, prof.max_atan2_us, prof.max_sqrt_us);

        // ADC continuity check: actual samples/callbacks seen in this
        // ~1s window vs. what ADC_CONT_SAMPLE_FREQ_HZ implies, plus any
        // pool overflow events. If "actual" comes in noticeably below
        // "expected" (or pool_ovf is nonzero), the stream has real gaps -
        // the filter's uniform-sample-spacing assumption is being
        // violated, which would explain artifacts no amount of filter
        // debugging could fix.
        static uint32_t last_samples_total = 0;
        static uint32_t last_callback_count = 0;
        static uint32_t last_ms = 0;
        uint32_t samples_now   = s_dbg_adc_samples_total;
        uint32_t callbacks_now = s_dbg_adc_callback_count;
        uint32_t elapsed_ms    = now - last_ms;
        if (elapsed_ms > 0) {
            uint32_t actual_sps   = (uint32_t)((uint64_t)(samples_now - last_samples_total) * 1000 / elapsed_ms);
            uint32_t expected_cbs = (uint32_t)((uint64_t)ADC_CONT_SAMPLE_FREQ_HZ * elapsed_ms
                                                / 1000 / ADC_CONT_FRAME_SAMPLES);
            Serial.printf("[adc] actual=%u sps (expected=%u) callbacks=%u (expected~%u) pool_ovf_total=%u\r\n",
                          actual_sps, ADC_CONT_SAMPLE_FREQ_HZ,
                          callbacks_now - last_callback_count, expected_cbs,
                          s_dbg_adc_pool_ovf_count);
            Serial.printf("[adc]   fifo: min_available=%u (want>=%u) starve_ticks_total=%u drop_total=%u\r\n",
                          s_dbg_adc_fifo_min_available, ADC_SAMPLES_PER_TICK,
                          s_dbg_adc_fifo_starve_count, s_dbg_adc_fifo_drop_count);
        }
        last_samples_total  = samples_now;
        last_callback_count = callbacks_now;
        last_ms = now;
    }

    delay(10);
}