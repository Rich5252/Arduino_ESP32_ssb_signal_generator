#pragma once

/**
 * adc_capture.h
 *
 * Mic ADC capture: adc_continuous (DMA) setup, the ISR->task FIFO, the
 * per-sample biquad LPF, and the associated continuity diagnostics. Moved
 * out of ssb_mic_test.ino as-is during the module-split refactor - see
 * that file's original top-of-file comment block (now trimmed down, the
 * ADC-specific history notes live here instead) for the full "why adc_
 * continuous instead of adc_oneshot, why a FIFO, why float filtering only
 * happens in task context" backstory. None of that design changed, only
 * where the code physically lives.
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_attr.h"

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
// just pushes raw integers into the FIFO now (see the AVERAGING note
// below); adc_capture_read_next_sample() drains a fixed count per tick and
// does the actual filtering. No driver call and no blocking in the hot
// ISR/task path either way.
//
// The underlying pool still needs periodic draining via
// adc_continuous_read() to avoid on_pool_ovf - see adc_capture_service(),
// called from loop() at low priority on Core 1, fully decoupled from
// dsp_task.
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
//      arithmetic cost at all) into the FIFO; adc_capture_read_next_
//      sample() pops a FIXED count per tick (the true Fs_adc/Fs_dsp
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
// (see adc_capture_get_diag()). Raising baud rate is the primary fix;
// this is cheap additional margin on top.
#define ADC_CONT_BUF_BYTES        16384  // ~4096 samples, ~50ms headroom @ 80kHz

// 2nd-order LPF applied per raw sample in adc_conv_done_cb's downstream
// consumer, replacing the old N=16 boxcar average - see the AVERAGING
// comment above. Sits at the top of the voice band on purpose (same
// reasoning the old 5kHz boxcar corner used): filtering broadband ADC
// noise without eating wanted audio. Real tuning knob now - not yet
// verified by ear or spectrum analyser with the AD9851 in the loop.
//
// Two filter families now share this same fc_hz - toggled live via serial
// 'f' (see adc_lpf_mode_t below):
//   - Butterworth (ssb_biquad_lpf_init): maximally flat passband, -3dB
//     exactly at ADC_LPF_CUTOFF_HZ.
//   - Chebyshev Type I (ssb_biquad_chebyshev_lpf_init), ripple
//     ADC_LPF_CHEBYSHEV_RIPPLE_DB below: genuinely "same 3dB bandwidth,
//     faster rolloff" - ssb_biquad_chebyshev_lpf_init() internally solves
//     (once, at init time - see ssb_adc_filter.c) for the ripple-edge
//     parameter that puts its TRUE -3dB point at exactly ADC_LPF_CUTOFF_HZ
//     too, same as the Butterworth filter, rather than the textbook
//     Chebyshev convention of feeding the ripple-edge frequency directly
//     (verified numerically to be the wrong choice here - it shifts -3dB
//     up to ~3725Hz and is actually WEAKER than Butterworth through most
//     of the near stopband, the opposite of "faster rolloff"). With -3dB
//     points matched, the Chebyshev filter is monotonically MORE
//     attenuated than Butterworth above 3000Hz - up to ~3.8dB more by
//     10-20kHz, both converging to the same ultimate -12dB/octave slope
//     far out (filter ORDER, not family, sets that).
#define ADC_LPF_CUTOFF_HZ             3000.0f
#define ADC_LPF_CHEBYSHEV_RIPPLE_DB   1.0f   // standard/commonly-cited spec; small (~0.7dB peak)
                                              // in-band ripple bump in exchange for the steeper
                                              // rolloff above - inconsequential for mic audio

// Raw ADC samples cross the ISR->task boundary through a FIFO as plain
// integers - the ISR does NO filtering at all, just copies. This
// decouples two things that were previously tangled together:
//   - ADC_CONT_FRAME_SAMPLES (how many samples the DMA/ISR handles per
//     interrupt) - can now be whatever's efficient for the driver/ISR
//     overhead, independent of anything else.
//   - The output UPDATE RATE dsp_task sees - governed instead by how
//     many items adc_capture_read_next_sample() pops off the FIFO per
//     call (see ADC_SAMPLES_PER_TICK below), not by how often the ISR
//     happens to fire.
// Single-producer (ISR)/single-consumer (dsp_task via
// adc_capture_read_next_sample()), so plain volatile head/tail is fine.
#define ADC_FIFO_SIZE   64   // power of two; headroom over one ADC_CONT_FRAME_SAMPLES-sized burst
#define ADC_FIFO_MASK   (ADC_FIFO_SIZE - 1)

// How many raw ADC samples dsp_task nominally consumes from the FIFO
// each tick - the exact Fs_adc/Fs_dsp ratio. 80000/16000 = 5 exactly (was
// 80000/10000 = 8 before SAMPLE_RATE_HZ was raised); if either rate ever
// changes, check this stays an integer division with zero remainder.
#define ADC_SAMPLES_PER_TICK       (ADC_CONT_SAMPLE_FREQ_HZ / SAMPLE_RATE_HZ)

// CATCH-UP THRESHOLD - separate from the nominal rate above. This is the
// "genuine backlog" cutoff the drain logic uses: available <= this ->
// take nominal ADC_SAMPLES_PER_TICK; above it -> something abnormal has
// happened (startup, mode switch) and it drains aggressively to recover.
// See adc_capture.cpp's adc_capture_read_next_sample() for the full
// history of why this two-tier design replaced two earlier, broken ones.
#define ADC_SAMPLES_PER_TICK_MAX   (2 * ADC_CONT_FRAME_SAMPLES)

typedef struct {
    uint32_t samples_total;       // running total, incremented by n each on_conv_done
    uint32_t callback_count;      // how many times on_conv_done has fired
    uint32_t pool_ovf_count;      // on_pool_ovf events - direct evidence of drops
    uint32_t fifo_starve_count;   // ticks where available < ADC_SAMPLES_PER_TICK
    uint32_t fifo_min_available;  // running low-water mark
    uint32_t fifo_max_available;  // running high-water mark
    uint32_t fifo_drop_count;     // our FIFO overflowing (distinct from driver pool_ovf)
} adc_capture_diag_t;

// Starts adc_continuous, registers the event callbacks, inits the LPF, and
// runs the one-time raw diagnostic dump - the full original init_adc()
// body. Call once from setup().
void adc_capture_init(void);

// Pops the next batch of raw ADC samples out of the FIFO (nominal
// ADC_SAMPLES_PER_TICK, with the same catch-up/bleed-off logic the
// original inline block in dsp_task had), runs each through the selected
// biquad (or passes it through raw if OFF), and returns the last (most
// recent) filtered value in ADC-code units (~0-4095) - exactly what
// dsp_task used to keep in s_last_filtered_adc before normalizing to
// [-1,1] and DC-blocking itself. Called once per dsp_task tick, mic mode
// only.
float IRAM_ATTR adc_capture_read_next_sample(void);

// Drains adc_continuous's internal pool so it doesn't overflow -
// discards everything read. Low priority, not time-critical; call from
// loop() every iteration (runs unconditionally, same as the original).
void adc_capture_service(void);

// Live A/B/C toggle for the ADC anti-alias/noise LPF, via serial 'f' - see
// serial_commands.cpp. OFF passes raw ADC samples through unchanged (was
// the boolean "bypass=true" state before this became 3-way); BUTTERWORTH
// and CHEBYSHEV select which of the two filters (see ADC_LPF_CUTOFF_HZ /
// ADC_LPF_CHEBYSHEV_RIPPLE_DB above) processes the signal.
typedef enum {
    ADC_LPF_MODE_OFF = 0,
    ADC_LPF_MODE_BUTTERWORTH = 1,
    ADC_LPF_MODE_CHEBYSHEV = 2,
} adc_lpf_mode_t;

void adc_capture_set_lpf_mode(adc_lpf_mode_t mode);
adc_lpf_mode_t adc_capture_get_lpf_mode(void);

// Short human-readable name for the current mode ("off"/"Butterworth"/
// "Chebyshev") - used by the 'f' handler's printf and the 'P' status/
// preset-dump line, so both stay in sync with the enum automatically
// rather than duplicating a string-selection ternary in two places.
const char *adc_capture_lpf_mode_name(adc_lpf_mode_t mode);

// Zeros every counter/watermark below (the 'r' command's ADC-side reset).
void adc_capture_reset_diag(void);

void adc_capture_get_diag(adc_capture_diag_t *out);

// esp_timer microsecond timestamp captured once at adc_continuous_start()
// (and again on 'r', "restarting the long-window average from now" per
// the original comment) - used by diagnostics.cpp's long-window ADC rate
// calculation.
int64_t adc_capture_get_start_us(void);
