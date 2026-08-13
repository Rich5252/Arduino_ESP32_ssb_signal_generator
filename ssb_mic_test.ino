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

// pre-defined settings tables
#include "settings.h"

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
#define AD9851_ATTACHED 1

// ---- Two-tone test mode: bypass the mic ADC with a synthesized signal.
// Zero-hardware smoke test of the DSP chain. ----
#define TWOTONE_TEST_MODE   1  // testing default - two-tone on at boot
#define TWOTONE_F1_HZ        700.0f
#define TWOTONE_F2_HZ       1900.0f
#define TWOTONE_AMPLITUDE    0.45f   // keep below 0.5 so peaks don't clip when summed
#define SINGLETONE_HZ        1000.0f  // a clean, unambiguous default - see generate_singletone_sample()
#define SINGLETONE_AMPLITUDE 0.7f     // single tone alone - more headroom available than the
                                      // two-tone sum needs, comparable to a moderately hot mic level

// ---- Pre-Hilbert audio conditioning (HPF + presence EQ + compressor) ----
// See ssb_dsp.h's ssb_audio_fx_config_t for the individual parameters,
// set below in dsp_cfg.audio_fx. Leave off with TWOTONE_TEST_MODE if you
// want to look at the raw DSP chain's spurious performance without any
// conditioning in the signal path.
#define AUDIO_FX_ENABLED 1  // was 0 (pending re-tuning) - now toggleable live per-stage via serial
                             // 'e' (EQ) / 'c' (compressor) commands, see loop() - flip back to 0 only
                             // if you want the whole subsystem compiled out entirely
#define MASTER_GAIN_STEP_DB 1.0f  // per '+'/'-' keypress - see ssb_dsp_set_master_gain_db()

// ---- MCP4725 DAC (RSET modulation output) ----
// NO LONGER CONNECTED (DAC hardware removed) - GPIO47/48 are the
// highest general-purpose pins on the S3: not strapping, not flash/
// PSRAM (even on Octal variants), not USB-JTAG, not shared with UART0/
// Serial. Note: if your exact module part number ends in "V" (e.g.
// N8R8V), these two run at 1.8V logic instead of 3.3V - check before
// this DAC (or anything else) actually gets wired back to them.
#define MCP4725_SDA_GPIO      47
#define MCP4725_SCL_GPIO      48
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

#define SAMPLE_RATE_HZ     10000u  // was 9600 - didn't divide either 80000 (ADC) or 2000000
                                    // (gptimer resolution_hz) evenly, causing a genuine ~4.8%
                                    // ADC_SAMPLES_PER_TICK mismatch (true_ratio measured 8.387
                                    // vs nominal 8) plus imprecise gptimer alarm timing. 10000
                                    // divides both cleanly (80000/10000=8, 2000000/10000=200)
                                    // for almost the same period (100us vs 104us) - restores
                                    // the FIFO's exact-ratio assumption the catch-up logic
                                    // depends on for smooth operation.
#define HILBERT_TAPS       65   // was briefly tested at 129 to check whether Hilbert filter
                                 // approximation accuracy was the source of the IMD floor that
                                 // tracks 1:1 with signal level below -6dB - real hardware A/B
                                 // showed no significant difference, ruling that hypothesis out
                                 // cleanly. Reverted to 65 since 129 bought nothing but extra FIR
                                 // cost. The floor's more likely explanation is now the sub-sample
                                 // timing residual - see s_relative_delay_samples's fractional
                                 // delay line, added specifically to test that instead. Must stay
                                 // ODD if changed again.
#define MAX_FREQ_DEV_HZ    8000.0f  // TEMPORARILY raised from 2800.0f for diagnostic A/B
                                     // testing - real hardware showed a consistent ~+100Hz
                                     // offset on BOTH tones of a 700/1900Hz two-tone test
                                     // (landed at 800/1999Hz) while a single 1000Hz tone was
                                     // exactly on frequency; fast_atan2/fast_sqrt already ruled
                                     // out via direct A/B (SSB_DSP_FAST_TRIG=0 test, no change).
                                     // This tests whether the (symmetric) clamp is engaging
                                     // asymmetrically against an asymmetric underlying two-tone
                                     // deviation signal near the beat envelope's nulls - a
                                     // single tone never approaches the old 2800Hz ceiling, so
                                     // this wouldn't have been visible there either way. Revert
                                     // to 2800.0f once this test is done, whichever way it goes -
                                     // 8000Hz is deliberately generous for testing, not a
                                     // considered permanent value.

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
#define AD9851_PIN_DATA   10
#define AD9851_PIN_WCLK   12
#define AD9851_PIN_FQUD   11
#define AD9851_PIN_RESET  9
#define REF_CLK_HZ        30000000u
#define CARRIER_HZ        14200160u  // +160Hz calibration offset - this AD9851 module's actual
                                      // REF_CLK isn't precisely 30MHz (expected given it's an
                                      // uncalibrated XO, not a precision reference); this value
                                      // makes the real transmitted output land on 14200000 exactly,
                                      // confirmed against the user's calibrated receiver
static ad9851_handle_t s_ad9851;
static volatile uint32_t s_carrier_hz = CARRIER_HZ;

// Phase/envelope relative-timing compensation. Originally assumed the
// envelope path (PWM -> analog Sallen-Key filter -> RSET, ~140us
// measured group delay via the 'p' envelope step test) always lagged
// the phase path (AD9851 SPI write, ~instant once the write completes),
// so only positive delay (holding freq_dev_hz back to let envelope
// catch up) was implemented. Real hardware testing found the opposite
// in some conditions - positive delay made two-tone IMDs WORSE, not
// better, suggesting the software reorder done earlier (PWM write moved
// ahead of the AD9851 write) may have already over-corrected, leaving
// envelope arriving slightly EARLY rather than late. Bipolar now:
// positive s_relative_delay_samples holds freq_dev_hz back (as before);
// negative holds the ENVELOPE back instead, letting the real optimum be
// found empirically in either direction rather than assumed. Sign
// convention: positive = phase delayed relative to envelope, negative =
// envelope delayed relative to phase.
//
// FRACTIONAL: real hardware testing found integer delay=0 beats both
// delay=1 (100us) and negative delay, meaning whatever residual timing
// error remains has to be under half a sample (~50us) - smaller than
// this delay line could ever resolve while restricted to whole-sample
// steps. Doubling HILBERT_TAPS (65->129) ruled out Hilbert filter
// approximation accuracy as the cause of the level-tracking IMD floor
// seen at low drive, which points back at this sub-sample timing
// residual as the more likely remaining explanation. Now linearly
// interpolated between adjacent ring entries - see interp_ring() - so
// delay can be set to e.g. 0.3 samples, not just 0 or 1.
#define PHASE_DELAY_MAX_SAMPLES 8   // ring buffer capacity (both rings) - generous headroom
                                     // over the ~1-2 samples actually expected to be needed
#define DELAY_STEP_SAMPLES 0.05f    // 5us per '['/']' keypress at 10kHz - tightened from an
                                     // initial 0.25 (25us) once real hardware testing found a
                                     // sweet spot near -0.25 samples, to resolve it more precisely
                                     // than that coarser step could
static float s_freq_dev_ring[PHASE_DELAY_MAX_SAMPLES] = {0};
static float s_envelope_ring[PHASE_DELAY_MAX_SAMPLES] = {0};
static uint32_t s_delay_ring_idx = 0;   // shared index - both rings always written/read together
static volatile float s_relative_delay_samples = 0.0f;   // now fractional - see comment above

// Linear interpolation between adjacent ring entries. 'back' is how many
// samples behind base_idx to read (0 = the just-written current sample,
// fractional values interpolate between the two nearest whole-sample
// entries). Caller is responsible for keeping 'back' within
// PHASE_DELAY_MAX_SAMPLES-2 so idx1 never wraps into not-yet-written data.
static inline float IRAM_ATTR interp_ring(const float *ring, uint32_t base_idx, float back)
{
    int32_t i0 = (int32_t)back;   // floor - back is always >= 0 by construction at call sites
    float frac = back - (float)i0;
    uint32_t idx0 = (base_idx + PHASE_DELAY_MAX_SAMPLES - (uint32_t)i0) % PHASE_DELAY_MAX_SAMPLES;
    uint32_t idx1 = (base_idx + PHASE_DELAY_MAX_SAMPLES - (uint32_t)i0 - 1) % PHASE_DELAY_MAX_SAMPLES;
    return ring[idx0] * (1.0f - frac) + ring[idx1] * frac;
}
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

// How many raw ADC samples dsp_task nominally consumes from the FIFO
// each tick - the exact Fs_adc/Fs_dsp ratio. 80000/20000 = 4 exactly; if
// either rate ever changes, check this stays an integer division with
// zero remainder.
#define ADC_SAMPLES_PER_TICK       (ADC_CONT_SAMPLE_FREQ_HZ / SAMPLE_RATE_HZ)

// CATCH-UP THRESHOLD - separate from the nominal rate above. This is
// the "genuine backlog" cutoff dsp_task's drain logic uses (see there):
// available <= this -> take nominal ADC_SAMPLES_PER_TICK; above it ->
// something abnormal has happened (startup, mode switch) and dsp_task
// drains aggressively to recover.
//
// History: a first version hard-capped consumption at exactly
// ADC_SAMPLES_PER_TICK with NO way to ever consume more, even with a
// backlog waiting. Fine only if production and consumption match
// perfectly forever - they don't quite (measured ~80645 sps actual vs
// 80000 nominal, i.e. dsp_task's true ADC_SAMPLES_PER_TICK ratio is
// ~4.03, not exactly 4), so a one-time backlog (e.g. the startup window
// before dsp_task starts draining) had no way to ever shrink again - the
// FIFO filled once and stayed pinned full, dropping nearly everything
// thereafter.
//
// A second version used min(available, this) as the drain count - which
// sounds like "normally 4, catch up to this when backlogged" but isn't:
// it actually means "always drain everything currently available, up to
// this cap" - so the instant a 16-sample burst landed, that WHOLE tick
// drained all 16 at once, leaving the FIFO empty and starving the next
// several ticks until the next burst - reintroducing the exact
// zero-order-hold staircase problem this FIFO design was built to fix,
// just with a different trigger (confirmed empirically: ~75% of ticks
// starved, matching "3 empty ticks per 4-tick burst cycle" almost
// exactly).
//
// Current (correct) design: dsp_task takes nominal ADC_SAMPLES_PER_TICK
// whenever that's available, and only exceeds it when available is
// ALREADY above this threshold - i.e. genuinely more backlog than normal
// bursty arrival ever produces on its own. The slow ~0.8% surplus lets
// "available" drift up gradually over many tens of milliseconds; only
// once it crosses this threshold does the aggressive catch-up kick in
// briefly to bring it back down - a gentle, infrequent correction
// instead of a violent one every single burst.
#define ADC_SAMPLES_PER_TICK_MAX   (2 * ADC_CONT_FRAME_SAMPLES)

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
static volatile uint32_t s_dbg_adc_fifo_max_available = 0;            // running high-water mark - confirms
                                                                        // backlog actually drains back down
                                                                        // rather than staying pinned near-full
static volatile uint32_t s_dbg_adc_fifo_drop_count = 0;        // our FIFO overflowing (distinct from driver pool_ovf)
static int64_t s_adc_start_us = 0;   // captured once at adc_continuous_start() - see the long-window
                                      // rate measurement in loop(); a longer averaging window gives a
                                      // much lower-noise estimate of the true achieved ADC rate than
                                      // the existing 1s window can, which matters for pinning down a
                                      // ~0.8%-scale mismatch with confidence rather than guessing

// Same long-window measurement approach, but for dsp_task's OWN tick
// rate via the gptimer - we'd only ever precisely measured the ADC side
// before. Chronic FIFO starvation despite the ADC itself running fast
// (not slow) only makes sense if dsp_task is also ticking faster than
// its own 20000Hz nominal, by more than the ADC's ~0.8% - this measures
// that directly instead of assuming gptimer is exact.
static int64_t s_dsp_tick_start_us = 0;         // captured once at gptimer_start()
static volatile uint32_t s_dbg_dsp_tick_count = 0;  // incremented once per dsp_task tick, unconditionally

static TaskHandle_t s_dsp_task;
static TaskHandle_t s_dac_task;
static QueueHandle_t s_envelope_queue;   // length 1, "latest value wins" (xQueueOverwrite)
static volatile ssb_sideband_t s_sideband = SSB_SIDEBAND_USB;

static volatile audio_source_t s_audio_source = (TWOTONE_TEST_MODE != 0) ? AUDIO_SRC_TWOTONE : AUDIO_SRC_MIC;

#define ENVSTEP_HZ   4.0f   // square wave rate - slow enough for easy scope triggering/viewing,
                             // fast enough not to be tedious to observe
#define FM_TEST_MOD_HZ  1200.0f  // matches the 700/1900Hz two-tone pair's beat frequency, for
                                  // direct comparability - spurs at n*1200Hz offsets would mean
                                  // something in AD9851 chain itself, not the DSP/Hilbert path
#define FM_TEST_DEV_HZ  3000.0f  // peak deviation - comparable order to real two-tone peak
                                  // deviations seen in testing (up to ~4800-4900Hz observed).
                                  // Gives modulation index beta=3000/1200=2.5 - a reasonably rich
                                  // sideband spectrum, good for comparing against the predicted
                                  // Bessel-function pattern
#define AM_TEST_MOD_HZ  1200.0f  // same rate as FM_TEST_MOD_HZ, for direct comparability between
                                  // the two isolation tests
#define AM_TEST_DEPTH   0.5f     // envelope swings the FULL available [0,1] range (0.5 +/- 0.5) -
                                  // 100% depth AM, deliberately stressing the RSET path's linearity
                                  // across its whole normalized range, same as real two-tone peaks
                                  // routinely reach in practice

// Takes plain int, not audio_source_t, deliberately: Arduino auto-
// generates function prototypes and inserts them at the very top of the
// translation unit, before any of the .ino's own typedefs are visible -
// a custom enum type in the signature breaks that auto-generated
// prototype ("was not declared in this scope"). int is a builtin type,
// always visible, so this sidesteps the problem entirely. The switch
// cases below still compare against the enum constants - fine, they're
// just int-valued.
static inline const char *audio_source_name(int src)
{
    switch (src) {
        case AUDIO_SRC_TWOTONE:    return "TWO-TONE TEST";
        case AUDIO_SRC_SINGLETONE: return "SINGLE-TONE TEST";
        case AUDIO_SRC_ENVSTEP:    return "ENVELOPE STEP TEST";
        case AUDIO_SRC_FMTEST:     return "FM TEST (AD9851 isolation)";
        case AUDIO_SRC_AMTEST:     return "AM TEST (RSET isolation)";
        default:                   return "mic";
    }
}

// Live A/B toggle for the ADC LPF, via serial 'f' - see loop(). Lets you
// compare filtered-vs-raw on the same physical signal without a rebuild,
// to check whether an artifact is actually coming from the filter or
// from somewhere else entirely (this is what caught that a fixed-point
// limit-cycle fix changed nothing observable - useful to keep checking
// empirically rather than assuming, now that filtering runs a third way).
static volatile bool s_adc_lpf_bypass = false;

// Silences the once-per-second [timing]/[adc]/[dsp] block, via serial
// 'v' - see loop(). Doesn't affect the underlying counters/watermarks at
// all (they keep accumulating correctly regardless, see the comment on
// the print gate itself) - purely about giving command confirmation
// prints (e.g. '['/']' phase-delay changes) a clean, uncluttered
// terminal to actually be visible in, since a wall of scrolling
// diagnostics every second makes a single confirmation line easy to
// miss - confirmed to be a genuine practical problem, not just a
// theoretical one.
static volatile bool s_diag_muted = false;

// Envelope-to-PWM-duty range mapping - separate knob from master gain.
// Master gain (ssb_dsp_set_master_gain_db) scales the WHOLE signal chain
// (phase deviation and envelope together, inside ssb_dsp); this only
// remaps envelope's own [0,1] output into a duty-cycle range before it
// drives the BS170 gate via PWM. Controls where the envelope's quiet-to-
// loud excursion actually sits relative to the Vgs sweet spot already
// characterized on real hardware (~2.3V +/-0.75V) - a mismatch here is a
// plausible independent contributor to the observed nonlinearity,
// separate from overall drive level. Runtime-tunable via 'u'/'j'
// (offset) and 'i'/'k' (scale/span) so it can be swept empirically
// rather than needing a reflash per trial.
#define ENV_PWM_STEP 0.02f   // 2% duty per keypress
static volatile float s_env_pwm_offset = 0.2f;   // was a hardcoded constant
static volatile float s_env_pwm_scale  = 0.9f;   // was a hardcoded constant

// ---- Envelope-path group-delay equalizer ----
// Two cascaded first-order digital all-pass sections (ssb_allpass1_t, see
// ssb_dsp.h) that flatten the ORIGINAL (non-Bessel) 2-pole Sallen-Key RSET
// reconstruction filter's group-delay dispersion across the voice/two-tone
// band - the point being to let that filter's better stopband rejection
// (vs. the Bessel redesign adopted earlier specifically to fix dispersion,
// see project history) be used WITHOUT paying the harmonic-dispersion IMD
// penalty that motivated switching to Bessel in the first place. If this
// works out on real hardware, it replaces the Bessel filter's tradeoff
// with "good rejection AND flat phase simultaneously" instead of picking one.
//
// Coefficients fitted numerically against SallenKey_LP_filter_BC337.txt -
// a real LTspice AC sweep of the ACTUAL circuit including the BC337 buffer
// stage's own loading/parasitics, not an idealized 2-pole formula (an
// idealized model can't capture the transistor stage's contribution to the
// real dispersion, which is why this needed the sim file rather than just
// recomputing from R/C values). Method: extracted group delay from the
// sweep's phase column via tau(f) = -(1/2pi) dphi/df (cubic-spline
// derivative, cross-checked against raw finite differences on the sweep's
// own points - agreement within ~0.02us in-band, ~0.2us worst-case near
// 3.2kHz), then numerically fit (a1, a2) minimizing the peak-to-peak
// spread of (analog + digital) combined group delay over 100-4300Hz (the
// two-tone fundamentals, the 1200Hz beat and its harmonics up to the 4th,
// and the +4300Hz 5th-order IMD product - see project history for why that
// product specifically matters). Result:
//   - Analog filter alone: 29.8us peak-to-peak over that band (the real
//     sim's dispersion is worse than the 13.2us/23.3us figures from
//     earlier idealized-model estimates - this supersedes those now that
//     real sim data is in hand).
//   - Single all-pass section, best case: 19.2us - barely better than
//     nothing, because the analog delay curve is NON-MONOTONIC (rises
//     from 66us at 100Hz to a ~77.6us peak near 2150Hz, then falls to
//     48us at 4300Hz) and one section can only ever produce a monotonic
//     delay curve (see ssb_allpass1_t's doc comment).
//   - Two sections, opposite-sign coefficients: 0.85us peak-to-peak - a
//     ~35x improvement over the analog filter alone, and better than the
//     Bessel filter's own measured 2.7us spread.
// NOT YET VALIDATED ON REAL HARDWARE - this is a numerically-fitted
// prediction against a simulated filter response, the same status the
// Bessel filter's LTspice design had before real hardware confirmed it.
//
// IMPORTANT SIDE EFFECT: an all-pass filter can only ADD delay, never
// subtract it - flattening this curve pushes the envelope path's OVERALL
// delay up by ~265us on average (2.65 samples @ 10kHz), not just its
// dispersion. The existing phase/envelope relative-delay line
// (s_relative_delay_samples, '['/']') will need to be RE-TUNED FROM
// SCRATCH once this is enabled: the theoretical starting point is
// roughly +2.65 samples (positive = hold phase back, matching the sign
// convention documented at s_relative_delay_samples's declaration), a
// completely different regime from the old best-known -0.20 to -0.25
// samples found for the Bessel filter - not a small tweak from that value.
//
// Applied unconditionally to `envelope` regardless of audio source (see
// dsp_task) - including ENVSTEP and AMTEST - so those isolation tests
// exercise the same combined (digital+analog) response real operation
// will see: ENVSTEP with this on/off is a direct scope A/B of whether
// flattening group delay actually cleans up the step edge, and AMTEST
// with this on/off is a direct check of whether it has any effect on the
// still-unexplained AM-to-PM crosstalk asymmetry (-2.4kHz nulls,
// +2.4kHz stuck at -40dB) - worth checking since that's the current
// top-priority open item regardless of what it turns out to show.
// Toggle via 'g', off by default so existing tuning isn't disturbed
// until deliberately opted into.
#define ENV_GDEQ_A1   0.194594f
#define ENV_GDEQ_A2  -0.136698f
static ssb_allpass1_t s_env_gdeq_1;
static ssb_allpass1_t s_env_gdeq_2;
static volatile bool s_env_gdeq_enable = false;

// Master gain (set via ssb_dsp_set_master_gain_db, '+'/'-') only applies
// inside ssb_dsp_process_sample() - which AMTEST/ENVSTEP deliberately
// bypass, so it previously had zero effect on their envelope level. This
// cached LINEAR value lets those modes apply the same gain control
// without recomputing powf(10, dB/20) every tick (10kHz) - updated only
// when gain actually changes (see the '+'/'-' handlers and the initial
// default setting in setup()), not read from ssb_dsp every sample.
static volatile float s_master_gain_linear_cache = 1.0f;

// Written by dsp_task/dac_task, printed by loop() on Core 1 at low
// priority - keeps Serial (slow) completely out of both real-time tasks.
static volatile float s_dbg_envelope = 0.0f;
static volatile float s_dbg_freq_dev = 0.0f;
#if AD9851_ATTACHED
static volatile float s_dbg_delayed_freq_dev = 0.0f;   // post-delay-line value, actually used
static volatile uint32_t s_dbg_tx_freq = 0;             // the exact integer Hz value sent to
                                                          // ad9851_set_frequency() - ground truth
                                                          // for what the chip is actually asked
                                                          // to produce, independent of any of the
                                                          // upstream DSP/delay-line reasoning
#endif
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

// WAKE-UP jitter - distinct from s_dbg_max_busy_us above, which only
// measures how long dsp_task's OWN work takes once it resumes. This
// measures the actual observed gap between successive ulTaskNotifyTake()
// returns - i.e. was dsp_task woken up ON TIME, regardless of how fast
// its own processing was. A task can have comfortable busy_us margin
// every single tick and STILL be intermittently woken late (preempted,
// scheduling delay, etc.) - that wouldn't show up in busy_us at all, but
// would still starve anything timing-sensitive that assumes a strictly
// periodic tick, like the ADC FIFO drain rate. Given this project's
// earlier history of cross-core I2C-driver jitter on dsp_task, a prime
// suspect if this climbs is PWM_COMPARISON_ENABLED running both DAC
// paths (I2C + PWM) simultaneously.
static volatile uint32_t s_dbg_max_tick_gap_us = 0;      // worst observed inter-tick gap
static volatile uint32_t s_dbg_late_tick_count = 0;      // ticks where the gap exceeded 1.5x nominal

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
// branches on the runtime s_audio_source selector and can switch to this
// at any time via the 't' serial command - see s_audio_source.
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

// Single, clean tone - isolates the DSP/RF chain (Hilbert, phase/freq
// modulation, AD9851 output) from mic-side confounds like preamp hum or
// room noise when characterizing basic sideband suppression/splatter -
// easier to read on a spectrum analyser than two-tone's own IMD products
// when THAT'S not what you're trying to measure. Switch to via 's'.
static float s_singletone_phase = 0.0f;

static inline float generate_singletone_sample(void)
{
    const float two_pi = 2.0f * (float)M_PI;
    float sample = SINGLETONE_AMPLITUDE * sinf(s_singletone_phase);
    s_singletone_phase += two_pi * SINGLETONE_HZ / (float)SAMPLE_RATE_HZ;
    if (s_singletone_phase > two_pi) s_singletone_phase -= two_pi;
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
        s_dbg_dsp_tick_count++;   // unconditional - counts real elapsed ticks regardless of mode

        // Measured FIRST, before any other work this tick, so it reflects
        // the true wake-up-to-wake-up gap rather than anything downstream.
        {
            static int64_t s_last_tick_start_us = 0;
            if (s_last_tick_start_us != 0) {
                uint32_t gap_us = (uint32_t)(t_start_us - s_last_tick_start_us);
                if (gap_us > s_dbg_max_tick_gap_us) s_dbg_max_tick_gap_us = gap_us;
                if (gap_us > (k_sample_period_us + k_sample_period_us / 2)) s_dbg_late_tick_count++;
            }
            s_last_tick_start_us = t_start_us;
        }

        float sample;
        if (s_audio_source == AUDIO_SRC_TWOTONE) {
            sample = generate_twotone_sample();
        } else if (s_audio_source == AUDIO_SRC_SINGLETONE) {
            sample = generate_singletone_sample();
        } else if (s_audio_source == AUDIO_SRC_ENVSTEP) {
            sample = 0.0f;   // unused - ssb_dsp_process_sample() is bypassed entirely for this mode, see below
        } else if (s_audio_source == AUDIO_SRC_FMTEST) {
            sample = 0.0f;   // unused - ssb_dsp_process_sample() is bypassed entirely for this mode too, see below
        } else if (s_audio_source == AUDIO_SRC_AMTEST) {
            sample = 0.0f;   // unused - ssb_dsp_process_sample() is bypassed entirely for this mode too, see below
        } else {
            // Pops up to ADC_SAMPLES_PER_TICK_MAX raw samples from
            // s_adc_fifo (normally only ADC_SAMPLES_PER_TICK will be
            // available and that's all that gets consumed - the higher
            // cap only engages to clear a genuine backlog, see the
            // comment by ADC_SAMPLES_PER_TICK_MAX for why that matters)
            // and runs each through s_adc_lpf HERE, in task context -
            // safe for float now, unlike the ISR that fills this FIFO.
            static float s_last_filtered_adc = 2048.0f;
            bool bypass = s_adc_lpf_bypass;
            {
                uint32_t tail = s_adc_fifo_tail;
                uint32_t head = s_adc_fifo_head;   // snapshot - ISR may still be advancing it, fine for a single consumer
                uint32_t available = (head - tail) & ADC_FIFO_MASK;

                if (available < s_dbg_adc_fifo_min_available) s_dbg_adc_fifo_min_available = available;
                if (available > s_dbg_adc_fifo_max_available) s_dbg_adc_fifo_max_available = available;
                if (available < ADC_SAMPLES_PER_TICK) s_dbg_adc_fifo_starve_count++;

                // Nominal 4/tick whenever that many are genuinely
                // available - NOT "everything available up to the cap"
                // (an earlier version did that, which greedily drained
                // the FIFO to near-zero the instant any burst landed,
                // then starved for the next several ticks until the next
                // one arrived - reintroducing the exact zero-order-hold
                // staircase problem this whole FIFO design was meant to
                // fix).
                //
                // ADC_SAMPLES_PER_TICK_NOMINAL_PLUS_ONE handles the small
                // persistent surplus (true_ratio measured ~4.032, not
                // exactly 4 - see the [dsp] long-window diagnostic) by
                // opportunistically taking one extra sample whenever one
                // happens to already be waiting, continuously bleeding
                // off the surplus in the smallest possible increment.
                // Without this, a fixed cap-of-32 catch-up threshold (see
                // ADC_SAMPLES_PER_TICK_MAX) still works, but the ~0.8%
                // surplus takes ~870 ticks (~43ms) to accumulate enough
                // to trigger it - producing one big periodic 28-sample
                // gulp roughly 23 times/sec, which shows up as regular
                // visible/audible spikes (confirmed empirically: observed
                // ~30/s). Soaking up 1 extra sample at a time instead
                // means the correction is spread continuously rather than
                // concentrated into periodic jolts.
                //
                // ADC_SAMPLES_PER_TICK_MAX is still checked first and
                // kept as a genuine-backlog fallback (startup, mode
                // switch) - that scenario needs to recover fast, not
                // trickle back 1 sample at a time.
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
        if (s_audio_source == AUDIO_SRC_ENVSTEP) {
            // Direct square wave, bypassing ssb_dsp_process_sample()
            // entirely - no Hilbert FIR, no atan2/sqrt, no mic - isolates
            // JUST the PWM->analog filter->RSET path's own step response
            // for measuring its group delay directly on a scope, rather
            // than trying to read it off a subtle two-tone envelope
            // feature. freq_dev_hz stays 0 - carrier held constant,
            // no phase modulation while this mode is active. Amplitude
            // scaled by master gain (s_master_gain_linear_cache) so '+'/
            // '-' has an effect here too, same as it does on real signal
            // paths - this mode bypasses ssb_dsp itself, where master
            // gain normally applies, so without this it would be inert.
            static float s_envstep_phase = 0.0f;
            envelope = (s_envstep_phase < 0.5f) ? 0.0f : s_master_gain_linear_cache;
            s_envstep_phase += ENVSTEP_HZ / (float)SAMPLE_RATE_HZ;
            if (s_envstep_phase >= 1.0f) s_envstep_phase -= 1.0f;
        } else if (s_audio_source == AUDIO_SRC_FMTEST) {
            // Direct sinusoidal frequency modulation, ALSO bypassing
            // ssb_dsp_process_sample() entirely - the mirror-image
            // isolation test to ENVSTEP: this exercises JUST the
            // AD9851/SPI/delay-line chain with a clean, mathematically
            // known FM signal, with no Hilbert FIR/atan2/sqrt involved
            // at all. envelope held fixed (constant drive, no AM) so
            // only the phase/frequency path is under test. Expected
            // result is a textbook FM sideband forest at
            // fc +/- n*FM_TEST_MOD_HZ - see the enum comment for the
            // diagnostic logic.
            static float s_fmtest_phase = 0.0f;
            const float two_pi = 2.0f * (float)M_PI;
            freq_dev_hz = FM_TEST_DEV_HZ * sinf(s_fmtest_phase);
            s_fmtest_phase += two_pi * FM_TEST_MOD_HZ / (float)SAMPLE_RATE_HZ;
            if (s_fmtest_phase > two_pi) s_fmtest_phase -= two_pi;
            envelope = 1.0f;   // fixed, full-scale - no AM content, phase path only
        } else if (s_audio_source == AUDIO_SRC_AMTEST) {
            // Direct sinusoidal amplitude modulation, ALSO bypassing
            // ssb_dsp_process_sample() entirely - mirror image of
            // FMTEST: exercises JUST the RSET/PWM/analog-filter/
            // transistor path with a clean, mathematically known AM
            // signal. freq_dev_hz stays exactly 0 - phase/carrier held
            // completely fixed, no FM at all. Ideal linear AM should
            // produce only a single sideband pair at
            // fc +/- AM_TEST_MOD_HZ - see the enum comment for the
            // diagnostic logic. Envelope still goes through the SAME
            // s_env_pwm_offset/s_env_pwm_scale mapping as real operation
            // below, so results are directly comparable to real testing.
            // Mean (carrier amplitude, i.e. AM_TEST_DEPTH itself) stays
            // FIXED regardless of gain - only the SWING around that mean
            // (modulation depth) scales with master gain. Without this
            // split, gain would move both together, muddying "is this
            // testing depth or overall level" - now '+'/'-' maps cleanly
            // onto depth alone, and 'u'/'j' (PWM offset) remains the
            // control for carrier amplitude, matching how those two
            // knobs are conceptually separate in the real signal chain.
            // Above 0dB the swing can still push peaks past 1.0, which
            // the PWM clamp downstream then flattens - a deliberate way
            // to probe the RSET path's saturation behavior, not a bug.
            static float s_amtest_phase = 0.0f;
            const float two_pi = 2.0f * (float)M_PI;
            float swing = AM_TEST_DEPTH * s_master_gain_linear_cache;
            envelope = AM_TEST_DEPTH + swing * sinf(s_amtest_phase);
            s_amtest_phase += two_pi * AM_TEST_MOD_HZ / (float)SAMPLE_RATE_HZ;
            if (s_amtest_phase > two_pi) s_amtest_phase -= two_pi;
            freq_dev_hz = 0.0f;   // carrier held completely fixed - AM content only
        } else {
            ssb_dsp_process_sample(s_ssb, sample, s_sideband, &freq_dev_hz, &envelope);
        }
        int64_t t_dsp_done_us = esp_timer_get_time();

        // Envelope-path group-delay equalizer (see ENV_GDEQ_A1/A2's
        // declaration comment for the coefficients/rationale). Applied
        // here, unconditionally across every audio source that reaches
        // this point (mic/two-tone/single-tone via ssb_dsp_process_sample
        // above, or ENVSTEP/AMTEST's own direct envelope assignment) -
        // one insertion point covers all of them identically, including
        // the isolation test modes, deliberately (see the comment at the
        // declaration for why that's useful rather than a shortcut).
        // Off by default (s_env_gdeq_enable) - toggle via 'g'.
        if (s_env_gdeq_enable) {
            envelope = ssb_allpass1_process(&s_env_gdeq_1, envelope);
            envelope = ssb_allpass1_process(&s_env_gdeq_2, envelope);
        }

        // envelope is roughly [0,1] for typical mic levels but not
        // rigorously bounded - clamp before handing off. Offset/scale
        // are now runtime-tunable (see s_env_pwm_offset/s_env_pwm_scale) -
        // this is the PWM duty range, a separate knob from master gain.
        envelope = envelope * s_env_pwm_scale + s_env_pwm_offset;
        if (envelope < 0.0f) envelope = 0.0f;
        if (envelope > 1.0f) envelope = 1.0f;

        // PWM write goes FIRST now, before the AD9851 SPI transfer -
        // deliberately, not incidentally. ledc_set_duty()+ledc_update_duty()
        // is a near-instant register write, while ad9851_set_frequency()
        // takes a measured ~20-54us (the SPI clock time itself, see the
        // [timing] write_us figures). Doing the AD9851 write first (the
        // original order) meant the envelope's own register write didn't
        // happen until that whole SPI transfer had finished - adding
        // tens of microseconds of PURELY SOFTWARE-caused lag on top of
        // whatever PWM's own update-boundary timing and the analog
        // reconstruction filter's group delay already add downstream.
        // This reorder removes one real, measurable contributor to that
        // gap for free - it doesn't eliminate PWM's own inherent delay
        // or the analog filter's group delay, both of which still exist
        // after this register write completes.
        //
        // Both rings are always written/read together here (shared
        // index), BEFORE either output is driven - s_relative_delay_samples
        // (runtime-tunable via '['/']', signed) decides whether the
        // freq_dev or the envelope side actually gets held back; the
        // other one reads its own just-written (undelayed) value. See
        // the declaration comment for why this needs to be bipolar -
        // real hardware testing found positive delay (phase held back)
        // made things worse in some conditions, the opposite of what was
        // originally assumed. Fractional via interp_ring() - real
        // hardware testing found the residual timing error is under one
        // whole sample (~100us), which whole-sample-only delay couldn't
        // resolve.
        float delayed_envelope = envelope;
        float delayed_freq_dev_hz = freq_dev_hz;
#if AD9851_ATTACHED
        {
            s_freq_dev_ring[s_delay_ring_idx] = freq_dev_hz;
            s_envelope_ring[s_delay_ring_idx] = envelope;

            float delay = s_relative_delay_samples;
            if (delay >= (float)(PHASE_DELAY_MAX_SAMPLES - 2))  delay = (float)(PHASE_DELAY_MAX_SAMPLES - 2);
            if (delay <= -(float)(PHASE_DELAY_MAX_SAMPLES - 2)) delay = -(float)(PHASE_DELAY_MAX_SAMPLES - 2);
            float freq_back = (delay > 0.0f) ? delay : 0.0f;    // positive: hold phase back
            float env_back  = (delay < 0.0f) ? -delay : 0.0f;   // negative: hold envelope back

            delayed_freq_dev_hz = interp_ring(s_freq_dev_ring, s_delay_ring_idx, freq_back);
            delayed_envelope    = interp_ring(s_envelope_ring, s_delay_ring_idx, env_back);

            s_delay_ring_idx = (s_delay_ring_idx + 1) % PHASE_DELAY_MAX_SAMPLES;
        }
#endif

#if PWM_COMPARISON_ENABLED
        {
            uint32_t max_duty = (1u << RSET_MOD_LEDC_RES) - 1u;
            uint32_t duty = (uint32_t)(delayed_envelope * (float)max_duty);
            ledc_set_duty(LEDC_LOW_SPEED_MODE, RSET_MOD_LEDC_CH, duty);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, RSET_MOD_LEDC_CH);
        }
#endif

#if AD9851_ATTACHED
        uint32_t tx_freq = s_carrier_hz + (int32_t)delayed_freq_dev_hz;
        ad9851_set_frequency(s_ad9851, tx_freq);

        // Unlike s_dbg_freq_dev (which is the PRE-delay value from
        // ssb_dsp_process_sample and has always been blind to whatever
        // the delay line does), these are what's ACTUALLY sent to the
        // chip - the only way to directly verify from firmware whether
        // changing s_relative_delay_samples ever alters the computed
        // frequency itself (it shouldn't - a pure sample delay can't
        // change frequency content - vs. just when a given value gets
        // sent).
        s_dbg_delayed_freq_dev = delayed_freq_dev_hz;
        s_dbg_tx_freq = tx_freq;
#endif

        // Non-blocking, always succeeds - overwrites whatever was there.
        // dac_task will pick up the latest value whenever it next runs;
        // this call never waits on the I2C bus. Kept after both real
        // outputs above - this path only drives the (currently
        // disconnected) MCP4725, not RSET, so its own latency doesn't
        // affect the timing analysis above at all.
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
    s_dsp_tick_start_us = esp_timer_get_time();
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
            .presence_gain_db = 2.0f,
            .presence_q = 1.0f,
            .comp_threshold = 0.1f,
            .comp_ratio = 3.5f,
            .comp_attack_ms = 3.0f,
            .comp_release_ms = 120.0f,
        },
    };
    ESP_ERROR_CHECK(ssb_dsp_init(&dsp_cfg, &s_ssb));

    // Testing session defaults - set explicitly here rather than baked
    // into ssb_dsp's own init defaults, which stay general-purpose
    // (both stages default to audio_fx.enable's value, gain to unity).
    // Both audio_fx stages off and a -2dB starting gain, per the current
    // test protocol - isolates the phase/envelope timing question from
    // EQ/compressor's own contribution to distortion.
    ssb_dsp_set_eq_enabled(s_ssb, false);
    ssb_dsp_set_compressor_enabled(s_ssb, false);
    ssb_dsp_set_master_gain_db(s_ssb, -2.0f);
    s_master_gain_linear_cache = powf(10.0f, -2.0f / 20.0f);   // keep in sync with the line above

    // Envelope group-delay equalizer coefficients - see ENV_GDEQ_A1/A2's
    // declaration comment. Initialized (states zeroed) regardless of
    // s_env_gdeq_enable's default, so enabling it later via 'g' doesn't
    // need a separate init path, only ssb_allpass1_reset() (see there).
    ssb_allpass1_init(&s_env_gdeq_1, ENV_GDEQ_A1);
    ssb_allpass1_init(&s_env_gdeq_2, ENV_GDEQ_A2);

    // Always started now, regardless of s_audio_source's initial value -
    // needed so the mic path is live and ready the moment a 't'/'s'/'m'
    // serial command switches source at runtime (see s_audio_source).
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
             audio_source_name(s_audio_source),
             AD9851_ATTACHED ? "attached" : "not attached (stubbed)",
             MCP4725_I2C_ADDR,
             PWM_COMPARISON_ENABLED ? "on" : "off");
    Serial.println("Send 't' for two-tone test signal, 's' for single-tone test signal, 'm' for live mic input, 'p' for envelope step test, 'y' for FM isolation test, 'h' for AM isolation test, 'f' to toggle the ADC LPF on/off, 'r' to reset diagnostics, 'v' to mute periodic diagnostics.");
    Serial.printf("Send 'e' to toggle EQ (currently %s), 'c' to toggle compressor (currently %s), "
                  "'+'/'-' for master gain (currently %+.1fdB, %.1fdB/step).\r\n",
                  ssb_dsp_get_eq_enabled(s_ssb) ? "ON" : "off",
                  ssb_dsp_get_compressor_enabled(s_ssb) ? "ON" : "off",
                  ssb_dsp_get_master_gain_db(s_ssb), MASTER_GAIN_STEP_DB);
#if AD9851_ATTACHED
    Serial.printf("Send 'o' to toggle AD9851 RF output on/off (currently %s), "
                  "'['/']' for relative phase/envelope delay (currently %+.2f samples, ~%+.0fus, "
                  "%.2f/step - positive delays phase, negative delays envelope).\r\n",
                  ad9851_get_output_enabled(s_ad9851) ? "ON" : "off",
                  s_relative_delay_samples, s_relative_delay_samples * 1000000.0f / SAMPLE_RATE_HZ,
                  DELAY_STEP_SAMPLES);
#endif
    Serial.printf("Send 'u'/'j' for PWM duty range offset, 'i'/'k' for span "
                  "(currently %.0f%%-%.0f%%, offset=%.2f scale=%.2f, %.0f%%/step).\r\n",
                  s_env_pwm_offset * 100.0f,
                  (s_env_pwm_offset + s_env_pwm_scale > 1.0f ? 1.0f : s_env_pwm_offset + s_env_pwm_scale) * 100.0f,
                  s_env_pwm_offset, s_env_pwm_scale, ENV_PWM_STEP * 100.0f);
    Serial.printf("Send 'g' to toggle the envelope group-delay equalizer (currently %s) - "
                  "fitted against the original Sallen-Key filter's real LTspice response, "
                  "not yet validated on hardware; re-tune '['/']' from scratch after enabling.\r\n",
                  s_env_gdeq_enable ? "ON" : "off");
}

// Owned by the [adc] 1-second rate print below; file-scope (not a local
// static) so the 'r' reset command can zero them too - otherwise the
// very next print after a reset would compute a bogus huge delta against
// stale pre-reset values.
static uint32_t s_last_samples_total = 0;
static uint32_t s_last_callback_count = 0;
static uint32_t s_last_rate_print_ms = 0;

void loop()
{
    // Runtime source switch: 't' -> two-tone, 's' -> single-tone, 'm' ->
    // live mic. dsp_task reads s_audio_source fresh every tick, so this
    // takes effect on the very next sample - no glitch/restart needed.
    // Unrecognized bytes (e.g. line endings from some serial monitors)
    // are silently ignored rather than echoed as an error.
    while (Serial.available()) {
        char c = Serial.read();
        if (c == 't' && s_audio_source != AUDIO_SRC_TWOTONE) {
            s_audio_source = AUDIO_SRC_TWOTONE;
            Serial.println("-> two-tone test signal");
        } else if (c == 's' && s_audio_source != AUDIO_SRC_SINGLETONE) {
            s_audio_source = AUDIO_SRC_SINGLETONE;
            Serial.printf("-> single-tone test signal (%.0fHz)\r\n", SINGLETONE_HZ);
        } else if (c == 'm' && s_audio_source != AUDIO_SRC_MIC) {
            s_audio_source = AUDIO_SRC_MIC;
            Serial.println("-> live mic input");
        } else if (c == 'p' && s_audio_source != AUDIO_SRC_ENVSTEP) {
            s_audio_source = AUDIO_SRC_ENVSTEP;
            Serial.printf("-> envelope step test (%.1fHz square wave, carrier fixed - "
                          "measure the RSET node's rise/settling time against this edge)\r\n", ENVSTEP_HZ);
        } else if (c == 'y' && s_audio_source != AUDIO_SRC_FMTEST) {
            s_audio_source = AUDIO_SRC_FMTEST;
            Serial.printf("-> FM isolation test (%.0fHz sine mod, %.0fHz peak deviation, beta=%.2f - "
                          "expect FM sidebands at fc+/-n*%.0fHz, no envelope content - "
                          "isolates AD9851/SPI chain from Hilbert/DSP math)\r\n",
                          FM_TEST_MOD_HZ, FM_TEST_DEV_HZ, FM_TEST_DEV_HZ/FM_TEST_MOD_HZ, FM_TEST_MOD_HZ);
        } else if (c == 'h' && s_audio_source != AUDIO_SRC_AMTEST) {
            s_audio_source = AUDIO_SRC_AMTEST;
            Serial.printf("-> AM isolation test (%.0fHz sine mod, %.0f%% depth, carrier fixed - "
                          "expect ONLY fc+/-%.0fHz sideband pair, no FM content - "
                          "isolates RSET/PWM/filter path from AD9851/DSP)\r\n",
                          AM_TEST_MOD_HZ, AM_TEST_DEPTH * 200.0f, AM_TEST_MOD_HZ);
#if AD9851_ATTACHED
        } else if (c == ']') {
            if (s_relative_delay_samples < (float)(PHASE_DELAY_MAX_SAMPLES - 2))
                s_relative_delay_samples += DELAY_STEP_SAMPLES;
            Serial.printf("-> relative delay %+.2f samples (~%+.0fus) - %s\r\n",
                          s_relative_delay_samples, s_relative_delay_samples * 1000000.0f / SAMPLE_RATE_HZ,
                          s_relative_delay_samples > 0.0f ? "phase held back" :
                          s_relative_delay_samples < 0.0f ? "envelope held back" : "aligned");
        } else if (c == '[') {
            if (s_relative_delay_samples > -(float)(PHASE_DELAY_MAX_SAMPLES - 2))
                s_relative_delay_samples -= DELAY_STEP_SAMPLES;
            Serial.printf("-> relative delay %+.2f samples (~%+.0fus) - %s\r\n",
                          s_relative_delay_samples, s_relative_delay_samples * 1000000.0f / SAMPLE_RATE_HZ,
                          s_relative_delay_samples > 0.0f ? "phase held back" :
                          s_relative_delay_samples < 0.0f ? "envelope held back" : "aligned");
#endif
        } else if (c == 'u') {
            if (s_env_pwm_offset < 1.0f) s_env_pwm_offset += ENV_PWM_STEP;
            Serial.printf("-> PWM duty range %.0f%%-%.0f%% (offset=%.2f, scale=%.2f)\r\n",
                          s_env_pwm_offset * 100.0f,
                          (s_env_pwm_offset + s_env_pwm_scale > 1.0f ? 1.0f : s_env_pwm_offset + s_env_pwm_scale) * 100.0f,
                          s_env_pwm_offset, s_env_pwm_scale);
        } else if (c == 'j') {
            if (s_env_pwm_offset > 0.0f) s_env_pwm_offset -= ENV_PWM_STEP;
            Serial.printf("-> PWM duty range %.0f%%-%.0f%% (offset=%.2f, scale=%.2f)\r\n",
                          s_env_pwm_offset * 100.0f,
                          (s_env_pwm_offset + s_env_pwm_scale > 1.0f ? 1.0f : s_env_pwm_offset + s_env_pwm_scale) * 100.0f,
                          s_env_pwm_offset, s_env_pwm_scale);
        } else if (c == 'i') {
            if (s_env_pwm_scale < 1.0f) s_env_pwm_scale += ENV_PWM_STEP;
            Serial.printf("-> PWM duty range %.0f%%-%.0f%% (offset=%.2f, scale=%.2f)\r\n",
                          s_env_pwm_offset * 100.0f,
                          (s_env_pwm_offset + s_env_pwm_scale > 1.0f ? 1.0f : s_env_pwm_offset + s_env_pwm_scale) * 100.0f,
                          s_env_pwm_offset, s_env_pwm_scale);
        } else if (c == 'k') {
            if (s_env_pwm_scale > 0.0f) s_env_pwm_scale -= ENV_PWM_STEP;
            Serial.printf("-> PWM duty range %.0f%%-%.0f%% (offset=%.2f, scale=%.2f)\r\n",
                          s_env_pwm_offset * 100.0f,
                          (s_env_pwm_offset + s_env_pwm_scale > 1.0f ? 1.0f : s_env_pwm_offset + s_env_pwm_scale) * 100.0f,
                          s_env_pwm_offset, s_env_pwm_scale);
        } else if (c == 'g') {
            bool now_on = !s_env_gdeq_enable;
            s_env_gdeq_enable = now_on;
            if (now_on) {
                // Avoid feeding stale x1/y1 from however long it's been
                // since this was last on (or since boot) into the first
                // sample after re-enabling - same reasoning as
                // ssb_dsp_set_compressor_enabled()'s env reset.
                ssb_allpass1_reset(&s_env_gdeq_1);
                ssb_allpass1_reset(&s_env_gdeq_2);
            }
            Serial.printf("-> envelope group-delay equalizer %s%s\r\n", now_on ? "ON" : "off",
                          now_on ? " - re-tune relative delay ('['/']') from scratch, "
                                   "theoretical starting point ~+2.65 samples (see declaration comment)" : "");
        } else if (c == 'f') {
            s_adc_lpf_bypass = !s_adc_lpf_bypass;
            Serial.printf("-> ADC LPF %s\r\n", s_adc_lpf_bypass ? "BYPASSED (raw)" : "active");
        } else if (c == 'v') {
            s_diag_muted = !s_diag_muted;
            // Deliberately printed regardless of the new mute state -
            // this confirmation itself needs to always be visible, or
            // muting silently would just create a different confusing
            // problem ("did that command even register?").
            Serial.printf("-> periodic [timing]/[adc]/[dsp] diagnostics %s\r\n",
                          s_diag_muted ? "MUTED (command confirmations only)" : "resumed");
        } else if (c == 'r') {
            // Resets every diagnostic counter/watermark for a clean
            // measurement window, without needing a full reflash. Useful
            // after switching modes (e.g. 't' then 'm') so counters like
            // drop_total/starve_ticks_total reflect only what happens
            // AFTER the reset, not history carried over from a different
            // mode or an earlier test run in the same boot.
            s_dbg_max_busy_us = 0;
            s_dbg_overrun_count = 0;
            s_dbg_max_adc_us = 0;
            s_dbg_max_dsp_us = 0;
            s_dbg_max_write_us = 0;
            s_dbg_max_tick_gap_us = 0;
            s_dbg_late_tick_count = 0;
            s_dbg_adc_samples_total = 0;
            s_dbg_adc_callback_count = 0;
            s_dbg_adc_pool_ovf_count = 0;
            s_dbg_adc_fifo_starve_count = 0;
            s_dbg_adc_fifo_min_available = 0xFFFFFFFFu;
            s_dbg_adc_fifo_max_available = 0;
            s_dbg_adc_fifo_drop_count = 0;
            s_dbg_dsp_tick_count = 0;
            s_dsp_tick_start_us = esp_timer_get_time();
            s_last_samples_total = 0;
            s_last_callback_count = 0;
            s_last_rate_print_ms = millis();
            s_adc_start_us = esp_timer_get_time();   // restarts the long-window average from now
            ssb_dsp_reset_freq_dev_stats(s_ssb);
            Serial.println("-> diagnostics reset, clean window starting now");
        } else if (c == 'e') {
            bool now_on = !ssb_dsp_get_eq_enabled(s_ssb);
            ssb_dsp_set_eq_enabled(s_ssb, now_on);
            Serial.printf("-> EQ (HPF+presence) %s\r\n", now_on ? "ON" : "off");
        } else if (c == 'c') {
            bool now_on = !ssb_dsp_get_compressor_enabled(s_ssb);
            ssb_dsp_set_compressor_enabled(s_ssb, now_on);
            Serial.printf("-> compressor %s\r\n", now_on ? "ON" : "off");
        } else if (c == '+') {
            float new_gain = ssb_dsp_get_master_gain_db(s_ssb) + MASTER_GAIN_STEP_DB;
            ssb_dsp_set_master_gain_db(s_ssb, new_gain);
            s_master_gain_linear_cache = powf(10.0f, new_gain / 20.0f);
            Serial.printf("-> master gain %+.1f dB\r\n", new_gain);
        } else if (c == '-') {
            float new_gain = ssb_dsp_get_master_gain_db(s_ssb) - MASTER_GAIN_STEP_DB;
            ssb_dsp_set_master_gain_db(s_ssb, new_gain);
            s_master_gain_linear_cache = powf(10.0f, new_gain / 20.0f);
            Serial.printf("-> master gain %+.1f dB\r\n", new_gain);
#if AD9851_ATTACHED
        } else if (c == 'o') {
            bool now_on = !ad9851_get_output_enabled(s_ad9851);
            ad9851_set_output_enabled(s_ad9851, now_on);
            Serial.printf("-> AD9851 RF output %s\r\n", now_on ? "ON" : "off (powered down)");
#endif
        } else if (c >= '0' && c <= '4') {
            int preset = c - '0';

            const PersistentSettings& p =
                settingsPresets[preset];

            s_audio_source = p.audio_source;
            s_relative_delay_samples = p.relative_delay_samples;
            s_env_pwm_offset = p.env_pwm_offset;
            s_env_pwm_scale = p.env_pwm_scale;
            s_env_gdeq_enable = p.env_gdeq_enable;
            s_adc_lpf_bypass = p.adc_lpf_bypass;

            ssb_dsp_set_eq_enabled(
                s_ssb,
                p.eq_enable
            );

            ssb_dsp_set_compressor_enabled(
                s_ssb,
                p.compressor_enable
            );

            ssb_dsp_set_master_gain_db(
                s_ssb,
                p.master_gain_db
            );

            s_master_gain_linear_cache =
                powf(10.0f, p.master_gain_db / 20.0f);

#if AD9851_ATTACHED
            ad9851_set_output_enabled(
                s_ad9851,
                p.ad9851_output_enable
            );
#endif

            Serial.printf("-> preset %d: %s\r\n",
                preset,
                settingsPresets[preset].name);
        }
        
    }

    // Keep adc_continuous's internal pool from filling up. The
    // on_conv_done callback (see adc_conv_done_cb) already captures every
    // frame's newest sample for dsp_task's use as it arrives - this call's
    // only job is freeing up the underlying pool so it doesn't overflow
    // (on_pool_ovf), so its contents are simply discarded. Low priority,
    // not time-critical - fine to do here alongside the other loop() work.
    // Runs unconditionally now - the ADC is always active (see init_adc()
    // in setup()) regardless of s_audio_source, so this pool needs
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
    if (!s_diag_muted && now - last_print_ms >= 45) {
        last_print_ms = now;
#if AD9851_ATTACHED
        Serial.printf("envelope=,%.3f  ,freq_dev=,%.1f,Hz  dac_code=,%u  ,delayed=,%.1f,Hz  tx_freq=,%u,Hz\r\n",
                      s_dbg_envelope, s_dbg_freq_dev, s_dbg_dac_code,
                      s_dbg_delayed_freq_dev, s_dbg_tx_freq);
#else
        Serial.printf("envelope=,%.3f  ,freq_dev=,%.1f,Hz  dac_code=,%u\r\n",
                      s_dbg_envelope, s_dbg_freq_dev, s_dbg_dac_code);
#endif
    }

    // Worst-case dsp_task timing, once a second - watch max_busy_us stay
    // under period_us with margin, and overruns stay at 0. If overruns
    // climb, dsp_task risks starving IDLE0 and tripping the task
    // watchdog - see the k_sample_period_us comment above.
    static uint32_t last_timing_print_ms = 0;
    if (!s_diag_muted && now - last_timing_print_ms >= 1000) {
        last_timing_print_ms = now;
        // mode= now goes through audio_source_name() (previously an
        // inline TWOTONE/SINGLETONE/else-"MIC" ternary that silently
        // mislabeled ENVSTEP/FMTEST/AMTEST as "MIC" too) - noticed while
        // adding gdeq= here, since ENVSTEP/AMTEST-with-gdeq-toggled is
        // specifically the workflow this diagnostic line needs to
        // correctly identify for (see ENV_GDEQ_A1/A2's declaration
        // comment). commands.md's claim that this field "always tells
        // you which signal source is actually active" wasn't quite true
        // before this fix.
        Serial.printf("[timing] mode=%s gdeq=%s max_busy_us=%u (adc=%u dsp=%u write=%u) period_us=%u overruns=%u\r\n",
                      audio_source_name(s_audio_source),
                      s_env_gdeq_enable ? "ON" : "off",
                      s_dbg_max_busy_us, s_dbg_max_adc_us, s_dbg_max_dsp_us, s_dbg_max_write_us,
                      k_sample_period_us, s_dbg_overrun_count);
        Serial.printf("[timing]   wakeup jitter: max_gap_us=%u (nominal=%u) late_ticks_total=%u\r\n",
                      s_dbg_max_tick_gap_us, k_sample_period_us, s_dbg_late_tick_count);

        // Sub-phase breakdown of dsp_us itself, from ssb_dsp's internal
        // profiling - lets us see which part of the DSP call (audio_fx,
        // the Hilbert FIR, or atan2f/sqrtf) is actually costing time,
        // rather than guessing again.
        ssb_dsp_profile_t prof;
        ssb_dsp_get_profile(s_ssb, &prof);
        Serial.printf("[timing]   dsp breakdown: audio_fx=%u fir=%u atan2=%u sqrt=%u\r\n",
                      prof.max_audio_fx_us, prof.max_fir_us, prof.max_atan2_us, prof.max_sqrt_us);

        // Evidence for setting MAX_FREQ_DEV_HZ from real data instead of
        // guessing again - max_unclamped is the TRUE peak deviation the
        // signal actually reaches (before any clamping), clip_count is
        // how many samples the clamp has actually had to intervene on.
        // Confirmed on real hardware that too tight a clamp both biases
        // two-tone output frequency AND hurts sideband suppression - the
        // right value sits somewhere above max_unclamped's real peak
        // with some margin, not an arbitrary guess either direction.
        {
            ssb_dsp_freq_dev_stats_t fd_stats;
            ssb_dsp_get_freq_dev_stats(s_ssb, &fd_stats);
            Serial.printf("[dsp]   freq_dev: max_unclamped=%.0fHz (limit=%.0fHz) clip_count=%u\r\n",
                          fd_stats.max_unclamped_freq_dev_hz, MAX_FREQ_DEV_HZ, fd_stats.clip_count);
        }

        // ADC continuity check: actual samples/callbacks seen in this
        // ~1s window vs. what ADC_CONT_SAMPLE_FREQ_HZ implies, plus any
        // pool overflow events. If "actual" comes in noticeably below
        // "expected" (or pool_ovf is nonzero), the stream has real gaps -
        // the filter's uniform-sample-spacing assumption is being
        // violated, which would explain artifacts no amount of filter
        // debugging could fix.
        uint32_t samples_now   = s_dbg_adc_samples_total;
        uint32_t callbacks_now = s_dbg_adc_callback_count;
        uint32_t elapsed_ms    = now - s_last_rate_print_ms;
        if (elapsed_ms > 0) {
            uint32_t actual_sps   = (uint32_t)((uint64_t)(samples_now - s_last_samples_total) * 1000 / elapsed_ms);
            uint32_t expected_cbs = (uint32_t)((uint64_t)ADC_CONT_SAMPLE_FREQ_HZ * elapsed_ms
                                                / 1000 / ADC_CONT_FRAME_SAMPLES);
            Serial.printf("[adc] actual=%u sps (expected=%u) callbacks=%u (expected~%u) pool_ovf_total=%u\r\n",
                          actual_sps, ADC_CONT_SAMPLE_FREQ_HZ,
                          callbacks_now - s_last_callback_count, expected_cbs,
                          s_dbg_adc_pool_ovf_count);

            // Long-window average - much lower noise than the 1s figure
            // above, since averaging error shrinks with window length.
            // This is what to trust for pinning down the TRUE achieved
            // ADC rate vs. the requested ADC_CONT_SAMPLE_FREQ_HZ (e.g. to
            // confirm whether an observed mismatch is a real clock
            // divider quantization effect or just measurement noise).
            int64_t elapsed_since_start_us = esp_timer_get_time() - s_adc_start_us;
            if (elapsed_since_start_us > 0) {
                double long_avg_sps = (double)samples_now * 1000000.0 / (double)elapsed_since_start_us;
                double error_pct = (long_avg_sps - (double)ADC_CONT_SAMPLE_FREQ_HZ)
                                    * 100.0 / (double)ADC_CONT_SAMPLE_FREQ_HZ;
                Serial.printf("[adc]   long-window avg=%.2f sps over %.1fs (%.3f%% vs nominal %uHz)\r\n",
                              long_avg_sps, elapsed_since_start_us / 1000000.0,
                              error_pct, ADC_CONT_SAMPLE_FREQ_HZ);

                // Same measurement for dsp_task's own tick rate (gptimer)
                // - only ever measured the ADC side precisely before.
                // Chronic FIFO starvation despite the ADC running fast
                // (not slow) only makes sense if THIS clock is also
                // running fast, by more than the ADC's own error.
                int64_t dsp_elapsed_us = esp_timer_get_time() - s_dsp_tick_start_us;
                if (dsp_elapsed_us > 0) {
                    double long_avg_tps = (double)s_dbg_dsp_tick_count * 1000000.0 / (double)dsp_elapsed_us;
                    double tick_error_pct = (long_avg_tps - (double)SAMPLE_RATE_HZ)
                                             * 100.0 / (double)SAMPLE_RATE_HZ;
                    // The number that actually matters for the FIFO: the
                    // TRUE ratio of the two measured rates, vs. the
                    // nominal ADC_SAMPLES_PER_TICK the drain logic
                    // assumes. If this deviates meaningfully from that
                    // nominal value, that - not either clock's error in
                    // isolation - is the real cause of sustained
                    // starvation or backlog (e.g. SAMPLE_RATE_HZ=9600
                    // gave true_ratio=8.387 vs nominal 8 - a genuine
                    // ~4.8% mismatch from 9600 not dividing 80000
                    // evenly, well beyond the ADC clock's own ~0.8%
                    // error alone - see SAMPLE_RATE_HZ's comment).
                    double true_ratio = long_avg_sps / long_avg_tps;
                    Serial.printf("[dsp]   long-window avg=%.2f ticks/s (%.3f%% vs nominal %uHz) true_ratio=%.4f (nominal=%u)\r\n",
                                  long_avg_tps, tick_error_pct, SAMPLE_RATE_HZ,
                                  true_ratio, ADC_SAMPLES_PER_TICK);
                }
            }
            Serial.printf("[adc]   fifo: available now min=%u max=%u (want>=%u,<%u) starve_ticks_total=%u drop_total=%u\r\n",
                          s_dbg_adc_fifo_min_available, s_dbg_adc_fifo_max_available,
                          ADC_SAMPLES_PER_TICK, ADC_FIFO_SIZE,
                          s_dbg_adc_fifo_starve_count, s_dbg_adc_fifo_drop_count);
        }
        s_last_samples_total  = samples_now;
        s_last_callback_count = callbacks_now;
        s_last_rate_print_ms  = now;
    }

    delay(10);
}