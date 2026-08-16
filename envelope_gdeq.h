#pragma once

/**
 * envelope_gdeq.h
 *
 * ---- Envelope-path group-delay equalizer ----
 * Two cascaded first-order digital all-pass sections (ssb_allpass1_t, see
 * ssb_dsp.h) that flatten the ORIGINAL (non-Bessel) 2-pole Sallen-Key RSET
 * reconstruction filter's group-delay dispersion across the voice/two-tone
 * band - the point being to let that filter's better stopband rejection
 * (vs. the Bessel redesign adopted earlier specifically to fix dispersion,
 * see project history) be used WITHOUT paying the harmonic-dispersion IMD
 * penalty that motivated switching to Bessel in the first place. If this
 * works out on real hardware, it replaces the Bessel filter's tradeoff
 * with "good rejection AND flat phase simultaneously" instead of picking one.
 *
 * Coefficients fitted numerically against SallenKey_LP_filter_BC337.txt -
 * a real LTspice AC sweep of the ACTUAL circuit including the BC337 buffer
 * stage's own loading/parasitics, not an idealized 2-pole formula (an
 * idealized model can't capture the transistor stage's contribution to the
 * real dispersion, which is why this needed the sim file rather than just
 * recomputing from R/C values). Method: extracted group delay from the
 * sweep's phase column via tau(f) = -(1/2pi) dphi/df (cubic-spline
 * derivative, cross-checked against raw finite differences on the sweep's
 * own points - agreement within ~0.02us in-band, ~0.2us worst-case near
 * 3.2kHz), then numerically fit (a1, a2) minimizing the peak-to-peak
 * spread of (analog + digital) combined group delay over 100-4300Hz (the
 * two-tone fundamentals, the 1200Hz beat and its harmonics up to the 4th,
 * and the +4300Hz 5th-order IMD product - see project history for why that
 * product specifically matters, and group_delay_fit_notes.md for the fit
 * script/method in full, including the SAMPLE_RATE_HZ=16000 refit below).
 * Analog filter alone: 29.8us peak-to-peak over that band, unchanged by Fs
 * (the real sim's dispersion is worse than the 13.2us/23.3us figures from
 * earlier idealized-model estimates - this supersedes those now that real
 * sim data is in hand). The DIGITAL fit result depends on SAMPLE_RATE_HZ,
 * because a first-order all-pass section's delay curve is shaped over the
 * full 0-Nyquist range - the same 100-4300Hz audio band is a smaller
 * fraction of that range at a higher Fs, giving each section less
 * available curvature per Hz to work with:
 *   - At 10000Hz: single section best case 19.2us (barely better than
 *     nothing - the analog curve is NON-MONOTONIC, rising from 66us at
 *     100Hz to a ~77.6us peak near 2150Hz then falling to 48us at 4300Hz,
 *     and one section can only ever produce a monotonic curve - see
 *     ssb_allpass1_t's doc comment). Two sections, opposite-sign
 *     coefficients: 0.85us peak-to-peak - a ~35x improvement over the
 *     analog filter alone, and better than the Bessel filter's own
 *     measured 2.7us spread. Mean added delay: ~265us (2.65 samples).
 *   - At 16000Hz: two sections only reach 14.7us peak-to-peak - still
 *     ~2x better than the bare analog filter, but nowhere near the
 *     10000Hz fit's near-total flattening, for the bandwidth-fraction
 *     reason above. (For reference, adding more cascaded sections buys
 *     most of that back - 3 sections reached 10.1us, 4 reached 5.2us, in
 *     exploratory fits not wired into the code below - worth revisiting
 *     if 14.7us turns out to matter on real hardware; each extra section
 *     is one more IRAM_ATTR multiply-add per sample, negligible against
 *     the current [timing] budget.) Mean added delay: ~163us (2.611
 *     samples @ 16000Hz) - NOT the same real-time delay as the 10000Hz
 *     fit's ~265us, a genuine ~102us difference from the coefficients
 *     themselves, independent of anything to do with sample count
 *     scaling - see settings.h's relative_delay_samples header note.
 * NOT YET VALIDATED ON REAL HARDWARE at either Fs - this is a
 * numerically-fitted prediction against a simulated filter response, the
 * same status the Bessel filter's LTspice design had before real hardware
 * confirmed it.
 *
 * IMPORTANT SIDE EFFECT: an all-pass filter can only ADD delay, never
 * subtract it - flattening this curve pushes the envelope path's OVERALL
 * delay up (see the per-Fs mean-added-delay figures above), not just its
 * dispersion. The phase/envelope relative-delay line (see relative_delay.h,
 * '['/']') will need to be RE-TUNED once this is enabled: the theoretical
 * starting point is roughly the mean added delay above, in samples at
 * whichever Fs is active (positive = hold phase back, matching the sign
 * convention documented at relative_delay.h), a completely different
 * regime from the old best-known -0.20 to -0.25 samples found for the
 * Bessel filter - not a small tweak from that value.
 *
 * Applied unconditionally to `envelope` regardless of audio source (see
 * dsp_task in the .ino) - including ENVSTEP and AMTEST - so those
 * isolation tests exercise the same combined (digital+analog) response
 * real operation will see: ENVSTEP with this on/off is a direct scope A/B
 * of whether flattening group delay actually cleans up the step edge, and
 * AMTEST with this on/off is a direct check of whether it has any effect
 * on the still-unexplained AM-to-PM crosstalk asymmetry (-2.4kHz nulls,
 * +2.4kHz stuck at -40dB) - worth checking since that's the current
 * top-priority open item regardless of what it turns out to show. Toggle
 * via 'g', off by default so existing tuning isn't disturbed until
 * deliberately opted into.
 */

#include <stdbool.h>
#include "config.h"
#include "ssb_dsp.h"

// Selected at compile time by SAMPLE_RATE_HZ (config.h) - see the fitting
// results in the header comment above for why these AREN'T simply rescaled
// from one Fs to the other the way a time-domain delay would be. The
// #error is deliberate: silently running with the wrong Fs's coefficients
// would be a subtle, hard-to-notice IMD/dispersion regression, not a
// crash - same failure class this project has repeatedly flagged for
// AD9851 bit-order mistakes. Add a new #elif (and a matching fit, see
// group_delay_fit_notes.md) rather than guessing if SAMPLE_RATE_HZ ever
// changes again.
#if SAMPLE_RATE_HZ == 16000
#define ENV_GDEQ_A1  -0.023900f
#define ENV_GDEQ_A2   0.447131f
#elif SAMPLE_RATE_HZ == 10000
#define ENV_GDEQ_A1   0.194594f
#define ENV_GDEQ_A2  -0.136698f
#else
#error "ENV_GDEQ_A1/A2 have only been fitted for SAMPLE_RATE_HZ = 10000 or 16000 - see group_delay_fit_notes.md for the fitting method to add another"
#endif

// Zeroes both all-pass sections' state and initializes their coefficients
// (ENV_GDEQ_A1/A2 above). Call once from setup() - always, regardless of
// the enabled default, so enabling later via 'g' only ever needs
// envelope_gdeq_set_enabled(), not a separate init path.
void envelope_gdeq_init(void);

// If enabled, runs `envelope` through both cascaded all-pass sections and
// returns the result; otherwise returns it unchanged. Call unconditionally
// from dsp_task, once per tick, for every audio source (see the rationale
// above for why this needs to cover ENVSTEP/AMTEST too, not just real
// signal paths).
float IRAM_ATTR envelope_gdeq_process(float envelope);

bool envelope_gdeq_get_enabled(void);

// Sets the enabled flag. On an off->on transition, also resets both
// sections' state (avoids feeding stale x1/y1 from however long it's been
// since this was last on, or since boot, into the first sample after
// re-enabling - same reasoning as ssb_dsp_set_compressor_enabled()'s env
// reset). A no-op transition (already in the requested state, or on->on
// e.g. across a preset reload) does NOT reset state - this is what keeps
// the filter running continuously across preset switches that all happen
// to have gdeq on, rather than glitching every time. This single function
// is shared by both the 'g' serial handler and the preset loader, so the
// off->on reset logic only has to be correct in one place.
void envelope_gdeq_set_enabled(bool enable);
