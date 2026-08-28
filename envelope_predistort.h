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
 * REVISION 2 (superseded): 95 dBm readings at 0.1-0.5dB command steps (using
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
 * REVISION 3 (superseded): built from an automated SDRuno logger (not a
 * manual point-by-point read like REVISIONS 1-2), sweeping the '.'/','
 * fine master-gain command in 0.1dB steps from -30dB to +6.2dB - 363
 * dBm readings, far denser AND far wider-range than REVISION 2's 95
 * points over -16dB to +5dB, with visibly tighter repeatability (the
 * flat noise-floor segment scatters by only +/-0.037dB across 101
 * points) than a hand-read sweep could manage.
 *
 * Gate-voltage side NOT re-measured this revision (the automated logger
 * can't read it) - instead confirmed, not assumed: three spot checks
 * against REVISION 2's 17-point gate-voltage curve (0dB: 2.40V vs.
 * REVISION 2's 2.44V; the saturation ceiling: 3.26V, an exact match) show
 * the curve's own shape hasn't moved, so REVISION 2's interior
 * gate-voltage curve was kept UNCHANGED and only extended at both ends
 * with two new anchor points (-30dB=0.98V, +6.2dB=3.26V) so the wider new
 * domain doesn't rely on PCHIP extrapolating past REVISION 2's original
 * -16dB/+5dB fitted range (which the REVISION 1/2 notes below already
 * flag as going wild fast outside its domain).
 *
 * What changed vs. REVISION 2, in duty terms: the floor (LUT[0]) moved
 * from 0.3129 to 0.3008 - the new sweep's lower floor (down to -30dB
 * command, vs. REVISION 2's -16dB) resolves a bit more real signal before
 * hitting the true noise floor (found flat at -97.96dBm from -30dB to
 * -20dB, with a faint but real rising tail from -20dB to -17dB REVISION 2
 * never saw). Through the steep turn-on region the new data consistently
 * wants ~1.0-1.5 duty-percentage-points MORE than REVISION 2 assumed (the
 * new RF sweep reads ~0.28dB more attenuated there, on average, than
 * REVISION 2 measured - a real circuit/measurement difference, not
 * noise, given how tight this revision's repeatability is). The single
 * biggest change is at the top: LUT[64] moved from 0.9495 to 0.9882 - the
 * old table's steep-turn-on-only measurement density meant it never
 * caught the full slow final approach to saturation (REVISION 2 only
 * swept to +5dB; this revision's readings only truly flatten out by
 * ~+3.3dB and hold flat to +6.2dB), so REVISION 2 was likely under-driving
 * at full-envelope commands.
 *
 * REVISION 3's own resolution check (superseded, kept for history): simulating
 * envelope_predistort_process()'s own linear interpolation against the
 * dense 363-point ground truth showed the table was a good match almost
 * everywhere (RMS error under 5 of the RSET LEDC channel's 1024 PWM
 * counts), but two spots fell short of that: the final bin (98.4%-100%
 * envelope) alone accounted for up to ~29 counts of error, since the
 * curve's steep final approach to saturation doesn't fit well in one
 * straight segment; a few bins through the turn-on-to-plateau transition
 * (roughly 22-44% envelope) showed 6-10 counts.
 *
 * REVISION 4 (superseded): built from the 'd'/'>'/'<'/'N'/'B' direct duty
 * override commands (envelope_output.h), sweeping every single raw LEDC
 * duty count 1-1023 one at a time and reading dBm directly at each -
 * 1023 exact points, denser even than REVISION 3's 363, and for the
 * first time with NO gate-voltage inference anywhere in the chain: this
 * table's x-axis (duty) is the literal commanded value, not backed out
 * from a DC voltage reading via REVISION 1-3's `duty = V_gate / 3.26V`
 * conversion.
 *
 * That direct data overturned the gate-voltage conversion itself, not
 * just refined it. Smoking-gun cross-check: REVISION 3 believed duty
 * 0.3008 (~duty 308) was still at the noise floor (its LUT[0], derived
 * from a 0.98V gate reading at the -30dB command point). Commanding
 * duty=308 directly measures -75.4dBm - 22dB ABOVE the real noise floor,
 * already well up the curve. The real dead zone (output statistically
 * indistinguishable from the -97.6dBm floor) only runs to about duty
 * 200-226, not duty 308. Rebuilding the table from this direct data
 * (isotonic-regression-smoothed to remove single-sample measurement
 * noise, then inverted the same PCHIP way as REVISIONS 1-3) shows the
 * gap isn't just at the floor: through essentially the whole 0-90%
 * envelope range, REVISION 3 was commanding roughly 40-120 MORE PWM
 * counts (about 4-12% of the full 1024-count range) than this direct
 * data shows is actually needed for the same RF output, converging with
 * REVISION 3 only above ~90% envelope where both approach full duty.
 * Likely explanation: `V_gate / 3.26V` assumed gate voltage rises
 * straight-line proportional to commanded duty; real gate voltage
 * appears to climb FASTER than duty through most of the range and only
 * becomes proportional near saturation, i.e. the conversion wasn't
 * wrong by a fixed offset, it was the wrong SHAPE of function. This
 * doesn't retroactively change any of this project's earlier delay-
 * tuning/IMD conclusions - `env_predistort_enable` was off in every
 * two-tone/noise-loading capture analyzed so far, so this table was
 * never actually driving the RSET pin during those tests.
 *
 * REVISION 4's resolution check: simulating the 65-point piecewise-linear
 * table against the dense 1023-point ground truth gives RMS error under 1
 * count through the turn-on and mid-climb regions (5-95% envelope) -
 * better than REVISION 3 managed even against its own, sparser ground
 * truth, simply because every single duty count was actually measured
 * instead of inferred between 0.1dB command steps. The top plateau
 * (95-100%) still shows up to ~12 counts of error (a straight segment
 * still slightly underfits the last bit of compression before
 * saturation), and the very bottom point (LUT[0], envelope=0) shows a
 * large-looking error against the dense data by construction - but that's
 * the same noise-floor-degeneracy artifact flagged in REVISION 3's own
 * check, just larger here because REVISION 4 has ~180 dense ground-truth
 * points sitting in that dead zone instead of a handful: duty 1 through
 * ~200 are all electrically interchangeable (same floor-level RF
 * output), so ANY of them is an equally correct answer for "envelope=0"
 * and comparing LUT[0] against one particular dense-data duty in that
 * range isn't a real resolution shortfall.
 *
 * REVISION 5 (current): same methodology and same exhaustive duty=1..1023
 * direct sweep as REVISION 4 (dBm -> linear amplitude via 10^(dBm/20),
 * isotonic-regression/PAVA smoothing to enforce monotonicity while
 * removing single-count measurement noise, normalize to [0,1], PCHIP
 * inversion sampled at 65 even envelope points), but measured against the
 * REBUILT RSET/PWM filter hardware - the fixed-bias PNP first stage that
 * replaced the original design after the low-output bias-starvation
 * finding (see project history: the original transistor lost bias current
 * at low duty, collapsing the filter's own 78.125kHz notch depth exactly
 * when the fixed-amplitude PWM ripple mattered most relative to signal).
 * This table doesn't touch that notch/carrier-rejection question at all -
 * it's a baseband, DC-to-low-kHz static AM curve exactly like REVISIONS
 * 1-4 - but it's the first real-hardware evidence of whether the redesign
 * changed the envelope path's overall shape, and it did, substantially, at
 * both ends:
 *
 * Bottom end - dead zone shrank by roughly 20x: the isotonic-pooled floor
 * block (electrically-indistinguishable output, same degeneracy as
 * REVISION 4's dead zone) now runs only duty 1-9, not REVISION 4's
 * duty~200-226. Turn-on is correspondingly much earlier - measurable
 * output growth is already visible by duty~20-30 in the raw sweep. This
 * is a direct, positive confirmation that giving the first transistor its
 * own fixed bias (rather than one that collapsed at low PWM drive) fixed
 * the thing it was meant to fix: LUT[0] is still duty=1 by the same
 * "lowest duty in the tied floor block" convention as REVISION 4, but the
 * floor block itself is now a sliver of what it was.
 *
 * Top end - the new weak spot, and it's a bigger one than anything
 * REVISION 4 flagged: roughly the last third of the ENTIRE duty range
 * (duty ~653 through 1023, ~370 of 1023 counts) reads within about 0.3dB
 * of the measured maximum (-40.22dBm at duty 600 vs. -39.89dBm at
 * duty 1023). Against a ~53dB total floor-to-ceiling span, that 0.3dB
 * sliver is a tiny fraction of the table's normalized envelope axis, so
 * PCHIP inversion at even 65-point spacing collapses essentially all of
 * it into the table's SINGLE LAST BIN: LUT[63]=0.6387 (duty~653) to
 * LUT[64]=1.0000 (duty=1023) spans 370 duty counts in one linear segment.
 * REVISION 4's own top-plateau error was documented as "up to ~12 counts"
 * across its whole 95-100% region - this is over an order of magnitude
 * more compression, concentrated in one bin, on the new hardware. A
 * commanded envelope anywhere in [0.984, 1.0) will interpolate LINEARLY
 * across that 370-count span, which will under-drive duty (and therefore
 * under-shoot the intended RF amplitude) through most of that bin, since
 * the true curve is flat-then-a-late-knee, not a straight line. Whether
 * this matters in practice depends on how close to envelope=1.0 real
 * operation actually commands (master gain headroom may keep typical
 * drive well clear of it) - flagged here rather than silently absorbed,
 * same as REVISION 4's own known limitations were. If it turns out to
 * matter on real hardware, the fix is more resolution specifically in
 * that last bin (a non-uniform envelope grid, or simply more than 65
 * points), not a different fitting method - the underlying data and
 * derivation are otherwise sound.
 *
 * NOT YET VALIDATED on real hardware beyond the measurement itself, same
 * status every revision has carried at introduction - see REVISION 4's
 * own not-yet-validated note below, which now applies doubly here since
 * the duty axis has moved again, more this time at the top than the
 * bottom.
 *
 * PCHIP specifically (not a plain cubic spline), all revisions, to
 * avoid overshoot/ringing through the steep BS170 turn-on region, which
 * would break the monotonicity a pre-distortion table depends on to be
 * invertible at all.
 *
 * Shape found, REVISION 4 (supersedes the duty percentages below, which
 * were all built on the flawed gate-voltage conversion): dead below
 * ~20% duty (200-226 out of 1023 - output pinned at the analyzer's noise
 * floor regardless of how much lower you command, confirming this path
 * has a real, non-zero floor rather than reaching genuine envelope zero -
 * see project history's EER deep-null-vs-linear-range discussion), then
 * a steep turn-on through roughly 30-45% duty. Because the LUT's grid is
 * even in ENVELOPE, not duty, and this whole dead-zone-plus-initial-climb
 * is so compressed in duty terms, it collapses into the table's very
 * first bin (LUT[0]=0.001 to LUT[1]=0.2998) - real commanded duty stays
 * inside the dead zone for desired envelopes up to about 1%, then rises
 * fast to the real turn-on point by envelope~1.5%. Above that, a long
 * gently-compressing climb, same shape REVISIONS 1-3 all found - and
 * this time the plateau genuinely IS reached at duty=1.0 (LUT[64]),
 * because REVISION 4's own sweep went all the way to the hardware
 * ceiling (duty=1023) rather than needing REVISION 2/3's inference about
 * where flattening finishes. No genuinely linear stretch anywhere, which
 * is why a shaping table (not just picking a "clean window" sub-range)
 * was the right fix.
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
 * REVISION 4 NOT YET VALIDATED ON REAL HARDWARE beyond the measurement
 * itself - REVISION 2 did get a real-hardware A/B (see the IMPORTANT
 * NEGATIVE RESULTS above), but REVISION 4, like REVISION 3 before it, is
 * a straight table swap that hasn't been re-run through two-tone/IMD
 * testing yet - and given how much this revision moved the duty axis
 * versus REVISION 3 (see the REVISION 4 notes above), that check matters
 * more here than it did for REVISION 3's more modest refinement. Off by
 * default so existing tuning isn't disturbed until deliberately opted
 * into, same convention as envelope_gdeq.h's 'g'.
 *
 * REVISION 5 NOT YET VALIDATED ON REAL HARDWARE beyond the measurement
 * itself either - see REVISION 5's own notes above, in particular the
 * last-bin top-end compression, which is the thing most worth watching
 * for on a real two-tone/IMD re-check before trusting this table anywhere
 * near full envelope drive. Still off by default, same convention.
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
