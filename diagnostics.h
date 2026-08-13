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
void IRAM_ATTR diagnostics_record_tick_start(int64_t t_start_us);

// Called once per dsp_task tick, after computing this tick's phase
// timings (adc_us/dsp_us/write_us/busy_us) - updates the four running
// high-water marks and overrun_count.
void IRAM_ATTR diagnostics_record_phase_timings(uint32_t adc_us, uint32_t dsp_us,
                                                 uint32_t write_us, uint32_t busy_us);

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
