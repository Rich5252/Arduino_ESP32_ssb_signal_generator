#pragma once

/**
 * diagnostics.h
 *
 * Everything written by dsp_task purely for visibility (never read back by
 * any control-path logic) plus the periodic [timing]/[adc]/[dsp] print
 * blocks that used to live directly in loop(). This module is allowed to
 * reach into every other module's small getters to print a coherent
 * status picture - nothing else should ever depend on diagnostics.h in
 * return, so it's safe to sit at the "top" of the include graph.
 *
 * The hot-path record_* functions are IRAM_ATTR and called once per
 * dsp_task tick, mirroring exactly where the equivalent inline volatile
 * writes sat in the original dsp_task - see ssb_mic_test.ino's dsp_task()
 * for the call sites.
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_attr.h"

// Captures the dsp-tick long-window average's starting timestamp. Call
// once from setup(), right after init_sample_timer() - mirrors exactly
// where the original captured s_dsp_tick_start_us.
void diagnostics_init(void);

// Worst-case timing diagnostics for dsp_task. Read/printed via
// diagnostics_service() only (never from dsp_task itself - no Serial
// calls on the real-time path). max_busy_us is a running high-water
// mark, never reset except via diagnostics_reset(), so it captures the
// worst case seen since boot (or since the last reset) even if it only
// happens once. overrun_count increments any sample whose processing
// took longer than one sample period - if this climbs, dsp_task is at
// risk of never yielding back to ulTaskNotifyTake, which starves IDLE0
// and trips the task watchdog.
//
// Called once per dsp_task tick, right after ulTaskNotifyTake() returns -
// updates the tick counter and the wake-up jitter stats (gap since the
// previous tick's t_start_us).
//
// REVERTED here to its pre-Fs-jitter-hunt-diagnostics signature (just
// t_start_us) - a fuller version briefly existed that also correlated
// this against ulTaskNotifyTake()'s elapsed-count return value (a
// diagnostics_record_elapsed_fast_ticks() sibling function, called on
// every wake to catch coalescing/missed wakes), but real hardware showed
// a regression in pin5's own period/pulse-width after that diagnostics
// work was added, and reverting the NEXT layer added on top of it
// (a per-wake residual-lateness measurement) did NOT fix it - so this
// whole layer is backed out down to the last confirmed-good checkpoint
// (the intr_priority=3 fix on its own, before any of this) to test
// whether ANY of it was the actual cause, rather than continuing to
// guess which specific piece. If pin5 comes back clean at THIS
// checkpoint, re-add the elapsed-count tracking piece by piece; if it's
// STILL bad here, the cause isn't in this diagnostics code at all.
void IRAM_ATTR diagnostics_record_tick_start(int64_t t_start_us);

// Called once per dsp_task tick, after computing this tick's phase
// timings (adc_us/dsp_us/write_us/busy_us) - updates the four running
// high-water marks and overrun_count.
void IRAM_ATTR diagnostics_record_phase_timings(uint32_t adc_us, uint32_t dsp_us,
                                                 uint32_t write_us, uint32_t busy_us);

// Core 1 breakdown - "where does Core 1's time actually go". Called
// once per loop() iteration (Core 1, NOT a hot real-time path - safe to
// be a plain function, no IRAM_ATTR needed), right after
// handle_serial_commands()/adc_capture_service()/diagnostics_service()/
// delay(10) have all run, with each one's own wall-clock duration this
// iteration. Unlike diagnostics_record_phase_timings() above (worst-case
// high-water marks), these accumulate as SUMS over the reporting window
// - the question here is which of these is eating the most total time,
// not what any single call's worst case was.
//
// delay_us matters more than it looks: the very first version of this
// breakdown didn't bracket delay(10) at all, and the numbers it produced
// (idle=3.0-3.7% via the Core-1 idle hook, cmd/adc_svc/diag all under
// 1%, "other"=96.2%) looked like Core 1 was almost completely saturated
// by something unaccounted-for. Adding this bucket was the fix, not a
// vTaskList()-found rogue task (there wasn't one - see the 'L' serial
// command's task list, which came back with exactly the expected 8
// tasks and nothing resembling an Arduino "events" task, since this
// project never touches WiFi/BT and that task is only ever created
// lazily by the code paths that do). delay(10) SHOULD dominate every
// loop() iteration - it's the only other thing loop() does - so
// bracketing it directly should collapse "other" down near zero and
// show that the real idle time was there all along; the [core1] idle
// hook reading just wasn't seeing most of it (see core1_idle_hook()'s
// CORE1_IDLE_GAP_THRESHOLD_US comment - a real blocked wait's idle-hook
// calls are spaced by the OS tick period, not microseconds apart, so a
// 10us gap threshold excludes nearly all of a genuine 10ms block).
void diagnostics_record_core1_loop_timings(uint32_t cmd_us, uint32_t adc_svc_us,
                                            uint32_t diag_us, uint32_t delay_us);

// Called once per dsp_task tick with the final (post-gdeq, post-PWM-
// mapping) envelope and the pre-delay freq_dev_hz - same values the
// original stored in s_dbg_envelope/s_dbg_freq_dev.
void IRAM_ATTR diagnostics_set_envelope_freqdev(float envelope, float freq_dev_hz);

// AD9851-only: the delayed freq_dev_hz actually used and the exact
// integer Hz sent to the chip.
void IRAM_ATTR diagnostics_set_tx_info(float delayed_freq_dev_hz, uint32_t tx_freq);

// Zeros every counter/high-water-mark this module owns and restarts the
// dsp-tick long-window average from now. Does NOT touch any other
// module's diagnostics - the 'r' serial handler calls
// adc_capture_reset_diag() and ssb_dsp_reset_freq_dev_stats() separately,
// same as the original 'r' handler touched every counter inline.
void diagnostics_reset(void);

bool diagnostics_get_muted(void);
void diagnostics_toggle_muted(void);

// Call once per loop() iteration. Internally throttles: prints the
// envelope/freq_dev/dac_code status line at ~45ms intervals and the full
// [timing]/[adc]/[dsp] block at ~1000ms intervals, both gated on the mute
// flag - exactly the two timed blocks the original loop() had inline,
// just moved here so loop() doesn't need its own timing-gate statics.
void diagnostics_service(void);
