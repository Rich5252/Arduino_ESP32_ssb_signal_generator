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
 * moves sqrt(I^2+Q^2) without touching atan2(Q,I), so stepping '+'/'-'
 * sweeps JUST envelope amplitude, phase held rock-steady) with PWM ranging
 * set to its full 0-100% span (offset=0, scale=1) so the measurement isn't
 * pre-restricted to any particular sub-window - 19 points from -14dB to
 * +4dB command, each read as the resulting RF carrier level on the
 * spectrum analyzer plus the DC gate voltage at the BS170. Converted to
 * (commanded duty fraction, linear RF amplitude out) pairs - duty
 * normalized against the observed 100%-duty saturation point (gate
 * pinned at 3.26V from +3dB command onward, RF level flat too) - then
 * inverted via monotonic (PCHIP) interpolation so the table maps the
 * direction actually needed at runtime: desired linear envelope -> duty
 * to command. PCHIP specifically (not a plain cubic spline) to avoid
 * overshoot/ringing through the steep BS170 turn-on region, which would
 * break the monotonicity a pre-distortion table depends on to be
 * invertible at all.
 *
 * Shape found: near-dead below ~32% duty (gate under ~1.05V - output
 * pinned near the analyzer's noise floor regardless of how much lower you
 * command, confirming this path has a real, non-zero floor rather than
 * reaching genuine envelope zero - see project history's EER deep-null-
 * vs-linear-range discussion), a steep turn-on through roughly 36-55%
 * duty (60-137dBm measured per volt at the gate - the region needing the
 * most correction), then a long gently-compressing climb to 100%. No
 * genuinely linear stretch anywhere, which is why a shaping table (not
 * just picking a "clean window" sub-range) was the right fix.
 *
 * ENVELOPE_PREDISTORT_LUT REPLACES the linear
 * `envelope * pwm_scale + pwm_offset` mapping in dsp_task when enabled -
 * it isn't layered on top of it, since the table's own domain already IS
 * "desired envelope [0,1] -> duty [0,1]" end to end, calibrated across the
 * full measured range. See serial_commands.cpp's 'D' handler for the
 * toggle and its console note about this.
 *
 * NOT YET VALIDATED ON REAL HARDWARE beyond the measurement itself - the
 * table is only as good as those 19 points; the steep-turn-on region in
 * particular would benefit from denser sampling (half-dB steps through
 * -8 to -5dB command) if real-signal testing shows the correction isn't
 * tight enough there. Off by default so existing tuning isn't disturbed
 * until deliberately opted into, same convention as envelope_gdeq.h's 'g'.
 */

#include <stdbool.h>
#include "esp_attr.h"

// Runs the lookup table (linear-interpolated between its 33 points) if
// enabled; otherwise returns `envelope` unchanged, letting the caller fall
// back to its own linear offset/scale mapping. Stateless (no ring
// buffers/filter memory like envelope_gdeq.h has), so there's no init()
// or off->on reset to call - the table itself is a compile-time constant.
float IRAM_ATTR envelope_predistort_process(float envelope);

bool envelope_predistort_get_enabled(void);
void envelope_predistort_set_enabled(bool enable);
