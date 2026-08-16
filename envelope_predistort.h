#pragma once

/**
 * envelope_predistort.h
 *
 * ---- Envelope pre-distortion (RSET/PWM/BS170/AD9851 static AM linearizer) ----
 * A lookup table that corrects for the STATIC (memoryless) nonlinearity of
 * the whole envelope->RF-amplitude chain - PWM's RC reconstruction filter,
 * the BS170 gate-to-drain transfer curve, and the AD9851's own RSET-to-DAC-
 * current relationship, all lumped together as one measured end-to-end
 * curve rather than modeled component-by-component. This is a DIFFERENT
 * problem from envelope_gdeq.h's: gdeq flattens group DELAY vs. frequency
 * (a dynamic/timing effect, needed because envelope and phase must arrive
 * time-aligned); this corrects envelope AMPLITUDE vs. commanded level (a
 * static effect, present even with a constant, unmodulated carrier) - see
 * the two-mechanism discussion in project history for why a 3rd-order IMD
 * product that barely responds to relative-delay tuning, alongside
 * higher-order products that respond strongly, pointed at two separate
 * mechanisms needing two separate fixes instead of one delay knob doing
 * both jobs.
 *
 * Table derived from a real hardware measurement: 's' (single-tone, which
 * a Hilbert-based SSB chain renders as a genuinely constant envelope AND
 * constant frequency deviation - scaling I/Q uniformly via master gain
 * moves sqrt(I^2+Q^2) without touching atan2(Q,I), so stepping the master
 * gain sweeps JUST envelope amplitude, phase held rock-steady) with PWM
 * ranging set to its full 0-100% span (offset=0, scale=1) so the
 * measurement isn't pre-restricted to any particular sub-window.
 *
 * REVISION 2 (current): 95 dBm readings at 0.1-0.5dB command steps (using
 * the '.'/',' fine master-gain command, see config.h's
 * MASTER_GAIN_FINE_STEP_DB) from -16dB to +5dB, densest through -11dB to
 * -5dB where the curve is steepest, plus 17 DC gate-voltage readings at
 * coarser steps across the same span. Gate voltage is only sparsely
 * measured because it's a slower/fiddlier reading than the spectrum
 * analyzer's dBm - but it's what actually calibrates commanded duty (see
 * below), so the 17 points are interpolated (PCHIP, monotonic - gate
 * voltage vs. command is a smooth, well-behaved curve with none of the
 * sharp features that make the RF-vs-duty curve itself interesting) up to
 * the full 95-point density before use. Commanded duty fraction at each
 * point is gate_voltage / 3.26V - 3.26V being the observed ceiling the
 * gate voltage saturates at, confirmed against REVISION 1's independently
 * known LUT[0]=0.3190 (1.04V / 3.26V = 0.31902, an exact match) as the
 * reverse-engineered derivation this whole conversion is built on.
 * Resulting (duty, RF amplitude) pairs, normalized and inverted via
 * monotonic (PCHIP) interpolation same as REVISION 1, now resampled onto
 * a 65-point grid (64 bins) instead of 33 - see envelope_predistort.cpp.
 *
 * REVISION 1 (original, superseded): 19 points from -14dB to +4dB
 * command at whole-dB steps, each read as RF dBm plus gate voltage,
 * resampled onto a 33-point grid. Steep-turn-on region was under-resolved
 * (only ~4 of the 19 points fell in it) - this revision exists to fix
 * that.
 *
 * PCHIP specifically (not a plain cubic spline), both revisions, to avoid
 * overshoot/ringing through the steep BS170 turn-on region, which would
 * break the monotonicity a pre-distortion table depends on to be
 * invertible at all.
 *
 * Shape found (confirmed and sharpened by REVISION 2): near-dead below
 * ~31% duty (gate under ~1.05V - output pinned near the analyzer's noise
 * floor regardless of how much lower you command, confirming this path
 * has a real, non-zero floor rather than reaching genuine envelope zero -
 * see project history's EER deep-null-vs-linear-range discussion), a
 * steep turn-on through roughly 31-55% duty (the region needing the most
 * correction, and where REVISION 2 concentrated its extra density), then
 * a long gently-compressing climb - but NOT all the way to 100%: REVISION
 * 2 revealed RF output actually plateaus at its max measured level by
 * ~95% duty (dBm flat from there to 100%), so the true "envelope=1"
 * duty is ~0.9495, not 1.0 - REVISION 1 didn't have a fine enough grid
 * near the top to see this and assumed literal 100%. No genuinely linear
 * stretch anywhere, which is why a shaping table (not just picking a
 * "clean window" sub-range) was the right fix.
 *
 * A real-hardware A/B test of REVISION 1 surfaced two IMPORTANT NEGATIVE
 * RESULTS worth recording so they aren't retried: (1) trying to avoid
 * this table's steep/low region altogether by clamping or compressing
 * envelope away from true zero (envelope_floor.h's 'x'/'z') made nearly
 * every IMD product WORSE, gradually, at every tested floor depth -
 * deliberately warping the true envelope trajectory costs more than
 * whatever this table's imperfection costs, so the right fix is a more
 * accurate table (this revision), not avoiding the region. (2) an
 * unrelated attempt to also freeze freq_dev_hz near envelope nulls (on
 * the theory atan2() there is noise) caused a hard phase discontinuity
 * and was removed entirely - see envelope_floor.cpp's header comment.
 *
 * ENVELOPE_PREDISTORT_LUT REPLACES the linear
 * `envelope * pwm_scale + pwm_offset` mapping in dsp_task when enabled -
 * it isn't layered on top of it, since the table's own domain already IS
 * "desired envelope [0,1] -> duty [0,1]" end to end, calibrated across the
 * full measured range. See serial_commands.cpp's 'D' handler for the
 * toggle and its console note about this.
 *
 * NOT YET VALIDATED ON REAL HARDWARE beyond the measurement itself - the
 * table is only as good as this revision's 95 points. Off by default so
 * existing tuning isn't disturbed until deliberately opted into, same
 * convention as envelope_gdeq.h's 'g'.
 */

#include <stdbool.h>
#include "esp_attr.h"

// Runs the lookup table (linear-interpolated between its 65 points) if
// enabled; otherwise returns `envelope` unchanged, letting the caller fall
// back to its own linear offset/scale mapping. Stateless (no ring
// buffers/filter memory like envelope_gdeq.h has), so there's no init()
// or off->on reset to call - the table itself is a compile-time constant.
float IRAM_ATTR envelope_predistort_process(float envelope);

bool envelope_predistort_get_enabled(void);
void envelope_predistort_set_enabled(bool enable);
