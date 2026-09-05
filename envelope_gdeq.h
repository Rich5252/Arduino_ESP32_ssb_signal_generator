#pragma once

/**
 * envelope_gdeq.h
 *
 * ---- Envelope-path group-delay equalizer ----
 * Two cascaded first-order digital all-pass sections (ssb_allpass1_t, see
 * ssb_dsp.h) that flatten the non-Bessel 2-pole Sallen-Key RSET
 * reconstruction filter's group-delay dispersion across the voice/two-tone
 * band - the point being to let that filter's better stopband rejection
 * (vs. the Bessel redesign adopted earlier specifically to fix dispersion,
 * see project history) be used WITHOUT paying the harmonic-dispersion IMD
 * penalty that motivated switching to Bessel in the first place. If this
 * works out on real hardware, it replaces the Bessel filter's tradeoff
 * with "good rejection AND flat phase simultaneously" instead of picking
 * one. Now covers TWO Sallen-Key hardware variants (ENV_FILTER_VARIANT,
 * config.h) with independently-fitted coefficients: the original BC337
 * NPN buffer design, and the newer PNP BC327 + gate-attenuator design -
 * see the 2026-09-01 entry further down for that second fit and how it
 * compares.
 *
 * Coefficients fitted numerically against SallenKey_LP_filter_BC337.txt
 * (BC337 variant) or SallenKey_LP_filter_PNP_BC327__RSET_Driver__Gate_
 * Attn.txt (PNP_BC327_ATTN variant) -
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
 * NOT YET VALIDATED ON REAL HARDWARE at either Fs, for the BC337 filter -
 * this is a numerically-fitted prediction against a simulated filter
 * response, the same status the Bessel filter's LTspice design had before
 * real hardware confirmed it.
 *
 * ---- 2026-09-01: PNP BC327 + gate-attenuator filter (ENV_FILTER_
 * PNP_BC327_ATTN) refit ---- New RSET driver topology, still Fs=16000 only
 * so far. Same method as above, applied to SallenKey_LP_filter_PNP_BC327__
 * RSET_Driver__Gate_Attn.txt - see group_delay_fit_notes.md's matching
 * entry for the full comparison against the BC337 filter. Headline
 * numbers: this filter's ANALOG response alone is both slower (93.4us mean
 * delay vs. BC337's 68.8us, +24.6us) and less flat (41.1us peak-to-peak
 * over 100-4300Hz vs. 29.8us) - a harder starting point for the equalizer
 * to work with. Two sections, a1=a2=0.622515 (grid-search-confirmed global
 * optimum, not a stuck/degenerate fit - see the #elif above): 41.1us ->
 * 13.7us peak-to-peak, actually slightly BETTER than the BC337 filter's
 * own 16000Hz fit (14.7us) despite the harder analog starting point. Mean
 * added delay: ~131.6us (2.105 samples @ 16000Hz) - LESS than the BC337
 * filter's 163us/2.611 samples, because a1=a2 here happens to need less
 * cumulative delay to flatten this particular dispersion shape.
 *
 * IMPORTANT, separate from the delay/dispersion result above: this filter
 * also has substantially more INSERTION LOSS than the BC337 filter,
 * growing with frequency (~1.7dB more at 500Hz, ~4.1dB more at 4300Hz,
 * ~6.9dB more at 8000Hz - roughly half the amplitude of the BC337 filter's
 * response by 8kHz). An all-pass equalizer is unity-magnitude BY
 * DEFINITION, so nothing above corrects any of this - the extra loss is
 * present regardless of how well delay gets flattened. Open question, not
 * yet resolved either way: whether that extra high-frequency loss
 * (attenuating exactly the range where an envelope null's fast transient
 * has its content, and where MAX_FREQ_DEV_HZ's up-to-8000Hz excursions
 * happen at the same instant) makes real IMD3/IMD5 performance worse
 * despite the improved delay flatness. See group_delay_fit_notes.md for
 * the reasoning and the suggested verification (a real two-tone IMD3/IMD5
 * spectrum comparison, BC337 vs. this filter, each with its own properly-
 * retuned delay) - not run yet.
 *
 * NOT YET VALIDATED ON REAL HARDWARE - same caveat as the BC337 fit above.
 *
 * ---- 2026-09-03: PNP_BC327_ATTN @ 16000Hz REFIT against real hardware TFA
 * data, extended to 8000Hz ---- Supersedes the 2026-09-01 LTspice-based fit
 * above for THIS filter/Fs combination (BC337 and the 10000Hz fits are
 * untouched - still LTspice-only). Two changes at once, both requested
 * together: (1) fit against a real sine-chirp TFA phase sweep of the actual
 * board (`Group_delay_off__on_F_Phase.txt`) instead of the LTspice sim,
 * now that Hi-Z buffers added to the TFA front end fixed a loading issue
 * that was previously making the measured phase inconsistent across the
 * envelope's DC operating range - directly answers group_delay_fit_notes.md's
 * "the fit is only exact at one DC bias point" open item, IF the improved
 * rig is what was making that point-dependence look worse than it really
 * is (not independently re-confirmed at multiple DC points this round -
 * see group_delay_fit_notes.md); (2) fit grid widened from 100-4300Hz to
 * 100-8000Hz, matching MAX_FREQ_DEV_HZ's real observed excursion limit
 * (config.h) rather than just the two-tone/IMD spectral band - directly
 * answers the OTHER open item, "the fit window doesn't cover what these
 * tests excite" (ENVSTEP/white-noise energy above 4300Hz was previously
 * unconstrained).
 *
 * Method: same two-cascaded-ssb_allpass1_t structure and same peak-to-peak
 * minimization objective as every fit above, only the source data and fit
 * band changed. New wrinkle this round: the real TFA sweep has ~1000
 * points/23kHz (much denser than LTspice's ~10 points/decade) and is
 * measurably noisier point-to-point - a light Savitzky-Golay smoothing
 * (31-point/~700Hz span, same window used for this project's other TFA
 * analyses) still leaves tens-of-us peak-to-peak swings above ~2kHz that
 * DON'T shrink monotonically the way real 2-pole-filter phase should -
 * checked by computing the residual (raw minus smoothed) and confirming
 * its RMS (0.5-2.6 degrees depending on band) is large enough to explain
 * swings that size once differentiated. Re-ran the smoothing-window choice
 * itself as a convergence check (61/91/121/151/181/221/261-point spans):
 * the extracted analog curve's own peak-to-peak stabilizes to within ~2us
 * from a 181-point (~4.2kHz) span onward, so that's the span used for the
 * fit target - wide enough to be past the noise floor, not so wide it
 * would smooth away a genuine single in-band hump if one were there.
 * See group_delay_fit_notes.md's matching 2026-09-03 entry for the full
 * window-sensitivity table and - important - a CORRECTION to this
 * project's own prior (2026-09-03, same day) real-hardware dispersion
 * report, which used the lighter 31-point smoothing and significantly
 * overstated the analog filter's and the OLD coefficients' actual
 * peak-to-peak dispersion as a result.
 *
 * Result, 100-8000Hz (the new fit band): analog filter alone (real
 * hardware, Hi-Z-buffered TFA) = 71.2us peak-to-peak, 71.2us mean delay -
 * a much gentler, more nearly-monotonic-decreasing curve than the LTspice
 * sim predicted (that sim's 41.1us p-p/93.4us mean was over the narrower
 * 100-4300Hz band only; real hardware's dispersion is smaller once you
 * average out the sim-vs-real shape mismatch, but the real curve extends
 * usefully further before flattening out). Two sections, `a1=0.026173`,
 * `a2=0.236810` (grid-search + Nelder-Mead, multiple starts converging to
 * the same point - same discipline as every fit above): 71.2us -> 18.5us
 * peak-to-peak (~3.9x). Mean added delay: ~125.4us (2.006 samples @
 * 16000Hz) - close to, and a bit LESS than, the old fit's 2.105 samples.
 * Re-checked over the OLD 100-4300Hz sub-band with these new coefficients:
 * 18.5us p-p there too (same worst-case points fall inside that narrower
 * range) - so the new fit is not a regression within the old band, it's
 * a strict widening of validated coverage.
 *
 * For reference/history, the OLD (2026-09-01, LTspice-fit) coefficients
 * were a1=a2=0.622515 - kept here as a comment, not deleted, per this
 * project's convention: applying THOSE coefficients to the NEW real
 * analog data over just 100-4300Hz actually tracks the LTspice prediction
 * reasonably well (36.8us -> 18.5us measured-analog/modeled-digital,
 * vs. the sim's own 41.1us -> 13.7us) - but the same old coefficients
 * applied over the new 100-8000Hz band blow up to 449us p-p, because they
 * were never fit to behave past 4300Hz. That blowup is the concrete
 * reason "just extend the existing coefficients' fit-grid comment without
 * re-fitting" wouldn't have worked - a first-order all-pass section's
 * delay keeps changing right up to Nyquist, and the old a=0.622515
 * sections are steep enough that the un-fit region past 4300Hz was
 * always going to diverge once looked at.
 *
 * GROUP-DELAY SIDE CONFIRMED ON REAL HARDWARE, 2026-09-03 (same day): these
 * coefficients were flashed to the board and re-measured with the TFA -
 * measured 71.3us -> 20.3us p-p over 100-8000Hz (~3.5x), mean added delay
 * 125.2us/2.003 samples, within ~1us of the prediction above and tracking
 * the predicted curve to ~2us RMS across the whole band. See
 * group_delay_fit_notes.md's matching entry for the full comparison and a
 * data-provenance note (that measurement file's two columns came out
 * swapped relative to the expected off/on order - resolved by a physical
 * sanity check, documented there).
 *
 * STILL NOT VALIDATED FOR IMD ON REAL HARDWARE - the group-delay
 * confirmation above is a different, narrower claim than IMD validation.
 * group_delay_fit_notes.md's extensive 2026-09-01 real-hardware IMD
 * testing (which found gdeq made 3rd-order IMD 6-10dB WORSE on two-tone,
 * and recommended leaving `g` off by default) was run against the OLD
 * coefficients, not these - it does NOT automatically carry over. This
 * refit directly addresses two of that writeup's four suspected causes
 * (fit-window mismatch and, possibly, the DC-bias-point measurement
 * inconsistency), but the other two (the unity-magnitude all-pass
 * structure still can't touch this filter's real insertion-loss penalty;
 * the envelope-interpolation confound) are unchanged, and none of this
 * replaces re-running the actual IMD comparison. **`g` stays off by
 * default until that re-test happens** - this is a better-fitted AND
 * now group-delay-validated equalizer, but not an IMD-validated one.
 *
 * ---- 2026-09-04: a second, 'a'+'A'-specific candidate fit exists,
 * NOT active by default ---- Once envelope_ampeq.h's shelf1+shelf2 both
 * got real-hardware use, the coefficients above (fit against the bare
 * analog filter alone) were confirmed to pass both shelves' own delay
 * dispersion straight through uncorrected (2026-09-03 g+a trial: 19.4us ->
 * 81.7us p-p; 2026-09-04 with shelf2 too: up to 157.5us p-p). Requested by
 * the user after directly confirming on the bench that envelope nulls
 * scope quicker and deeper with shelf1, quicker/deeper still with shelf2
 * (matching the candidate mechanism written up in
 * group_delay_fit_notes.md) - "maybe we should refine the grp delay again
 * to optimise that for the a+A case." A candidate refit
 * (a1=a2=-0.139115) was derived WITHOUT a new hardware sweep: the bare
 * (no-gdeq) analog+shelf1+shelf2 phase curve was reconstructed from the
 * existing `ga_Trial2_TF.txt` measurement by subtracting the ABOVE
 * coefficients' own exactly-known analytic phase (valid LTI cascade
 * algebra - gdeq runs purely digitally, earlier in the same chain the TFA
 * sweep measures end-to-end), then validated by self-consistency
 * (re-adding the above coefficients' phase to the reconstruction
 * reproduces the real Trial2 measurement almost exactly).
 *
 * Result: 157.5us -> 72.6us p-p (~2.2x) - real, but nowhere near the ~4x
 * flattening achieved above for the simpler analog-alone curve, and
 * confirmed to be a hard ceiling for this filter type (2/3/4-section
 * searches all landed on the same ~71-73us p-p floor) - the
 * analog+shelf1+shelf2 curve has two interior extrema plus a steep
 * near-Nyquist edge (shelf2's own delay signature peaks +86.3us right at
 * 7037Hz) that a cascaded single-real-pole all-pass can't fully track.
 * **The improvement is NOT uniform** - it fixes 8000Hz dramatically
 * (290.5us -> 217.9us) but ADDS 24-66us of delay at 500-3100Hz (the
 * two-tone's own fundamental frequencies) relative to the coefficients
 * above - a real redistribution, not a free improvement, and whether that
 * redistribution actually helps real IMD needs a bench test, not just a
 * smaller p-p number. See group_delay_fit_notes.md's matching 2026-09-04
 * entry for the full trade-off table and `gdeq_refit_a_A_candidate_v7.html`
 * for the chart.
 *
 * Selected via `ENV_GDEQ_USE_AA_CANDIDATE` in the `#if` block below - 0
 * (default) keeps the coefficients above active; flip to 1 only when
 * bench-testing with `'a'` AND `'A'` both on. **NOT YET VALIDATED ON REAL
 * HARDWARE for the a+A case specifically** - this is a candidate derived
 * from a reconstructed curve, not a fresh direct `'g'` OFF + `'a'`+`'A'`
 * ON sweep (the self-consistency check is strong, but every other fit in
 * this file was confirmed against a fresh direct measurement before being
 * trusted - recommended here too before drawing firm conclusions). Mean
 * added delay for the candidate: ~124.4us (1.990 samples @ 16000Hz) -
 * close to the default's 125.4us/2.006 samples, so switching between the
 * two should need only a small `'['`/`']'` nudge, not a from-scratch
 * relative-delay search.
 *
 * IMPORTANT SIDE EFFECT (both filters): an all-pass filter can only ADD
 * delay, never subtract it - flattening this curve pushes the envelope
 * path's OVERALL delay up (see the per-filter, per-Fs mean-added-delay
 * figures above), not just its dispersion. The phase/envelope
 * relative-delay line (see relative_delay.h, '['/']') will need to be
 * RE-TUNED once this is enabled, and AGAIN if ENV_FILTER_VARIANT is ever
 * switched (the two filters' theoretical starting points differ by
 * roughly half a sample - 2.611 for BC337, 2.006 for PNP_BC327_ATTN
 * (2026-09-03 real-hardware refit; was 2.105 under the superseded
 * 2026-09-01 LTspice fit), both @ 16000Hz): the theoretical starting
 * point is roughly the mean added
 * delay above for whichever filter/Fs combination is active (positive =
 * hold phase back, matching the sign convention documented at
 * relative_delay.h) - a completely different regime from the old
 * best-known -0.20 to -0.25 samples found for the Bessel filter, not a
 * small tweak from that value, for EITHER Sallen-Key variant.
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

// Selected at compile time by SAMPLE_RATE_HZ AND ENV_FILTER_VARIANT
// (config.h) - see the fitting results in the header comment above for why
// these AREN'T simply rescaled from one Fs (or one analog filter) to
// another the way a time-domain delay would be. The #error is deliberate:
// silently running with the wrong Fs/filter combination's coefficients
// would be a subtle, hard-to-notice IMD/dispersion regression, not a
// crash - same failure class this project has repeatedly flagged for
// AD9851 bit-order mistakes. Add a new #elif (and a matching fit, see
// group_delay_fit_notes.md) rather than guessing if SAMPLE_RATE_HZ or the
// analog filter ever changes again.
#if SAMPLE_RATE_HZ == 16000
  #if ENV_FILTER_VARIANT == ENV_FILTER_BC337
    #define ENV_GDEQ_A1  -0.023900f
    #define ENV_GDEQ_A2   0.447131f
  #elif ENV_FILTER_VARIANT == ENV_FILTER_PNP_BC327_ATTN
    // ---- 2026-09-04: a SECOND fit exists now, specific to the 'a'+'A'
    // ampeq case (both shelf1 AND shelf2 on) - see group_delay_fit_notes.md's
    // matching entry for the full derivation, the reconstructed-curve
    // method (no new hardware sweep was needed - the bare analog+shelf1+
    // shelf2 phase was recovered from the existing ga_Trial2_TF.txt
    // measurement by subtracting the CURRENT coefficients' exactly-known
    // analytic phase, validated by self-consistency), and IMPORTANTLY the
    // trade-off table showing the improvement is NOT uniform (fixes
    // 8000Hz a lot, ADDS 24-66us of delay at 500-3100Hz - the two-tone's
    // own fundamentals - relative to the values below). That candidate
    // (a1=a2=-0.139115) is NOT the active default - it would make things
    // WORSE for the 'g'-alone and 'g'+'a'-only cases the values below were
    // actually fit for (this filter type only supports ONE fixed pair at
    // compile time; see the open architectural question in
    // group_delay_fit_notes.md about whether that should ever become
    // runtime-selectable). Flip ENV_GDEQ_USE_AA_CANDIDATE to 1 below ONLY
    // when specifically bench-testing with 'a' AND 'A' both on - flip it
    // back to 0 (or just leave it, since testing sessions have been ending
    // with a revert-and-record-more-data pattern all through this project)
    // before trusting 'g'/'g'+'a' results again. NEITHER value below has
    // been re-measured on real hardware for the a+A case specifically -
    // this is a candidate to bench-test, not a confirmed result.
    #define ENV_GDEQ_USE_AA_CANDIDATE 1

    #if ENV_GDEQ_USE_AA_CANDIDATE
      // Grid search + global (differential-evolution) search + Nelder-Mead
      // refinement, same discipline as every fit in this file, against the
      // reconstructed analog+shelf1+shelf2 bare curve (88.4us p-p, 71.1us
      // mean over 100-8000Hz). 2/3/4-section searches all converged to
      // essentially the same ~71-73us p-p floor (a real ceiling for this
      // filter type on this curve shape - see group_delay_fit_notes.md for
      // why: two interior extrema plus a steep near-Nyquist edge from
      // shelf2's own delay signature), so 2 sections (same architecture as
      // below) was kept rather than adding more for a diminishing-returns
      // ~1-2us gain. Result: 157.5us -> 72.6us p-p (~2.2x) relative to
      // what applying the OTHER (default) coefficients to this same bare
      // curve would give - NOT a ~4x flattening like the original
      // analog-alone fit below achieved, and NOT uniform - see the header
      // comment above and group_delay_fit_notes.md's trade-off table
      // before drawing IMD conclusions from just this p-p number.
      #define ENV_GDEQ_A1  -0.139115f
      #define ENV_GDEQ_A2  -0.139115f
    #else
    // Refitted 2026-09-03 against REAL HARDWARE TFA data
    // (Group_delay_off__on_F_Phase.txt, Hi-Z-buffered measurement rig),
    // fit band widened to 100-8000Hz (was 100-4300Hz) - see the header
    // comment's "2026-09-03" section above and group_delay_fit_notes.md
    // for the full method, the smoothing-window sensitivity check, and
    // the correction to this project's own same-day dispersion report.
    // Grid-search + Nelder-Mead cross-checked (multiple starts converged
    // to the same point), same discipline as every fit in this file.
    // STILL NOT IMD-VALIDATED ON REAL HARDWARE - see header comment. Also
    // known (2026-09-03 g+a trial, then again 2026-09-04 with shelf2 too)
    // to NOT flatten well once ampeq's shelf(s) are on - these values were
    // fit against the bare analog filter ALONE, before either shelf
    // existed - see the ENV_GDEQ_USE_AA_CANDIDATE block above for the
    // shelf1+shelf2-specific alternative.
    #define ENV_GDEQ_A1   0.026173f
    #define ENV_GDEQ_A2   0.236810f
    // For reference/history (NOT active) - 2026-09-01 LTspice-only fit,
    // 100-4300Hz band only: a1=a2=0.622515f. Diverges badly (449us p-p)
    // if evaluated past 4300Hz, which is exactly why this needed a real
    // re-fit rather than just widening the old coefficients' claimed
    // range - see header comment.
    #endif
  #else
    #error "ENV_GDEQ_A1/A2 have only been fitted for ENV_FILTER_BC337 or ENV_FILTER_PNP_BC327_ATTN at SAMPLE_RATE_HZ=16000 - see group_delay_fit_notes.md for the fitting method to add another"
  #endif
#elif SAMPLE_RATE_HZ == 10000
  #if ENV_FILTER_VARIANT == ENV_FILTER_BC337
    #define ENV_GDEQ_A1   0.194594f
    #define ENV_GDEQ_A2  -0.136698f
  #else
    #error "ENV_FILTER_PNP_BC327_ATTN has not been fitted at SAMPLE_RATE_HZ=10000 - see group_delay_fit_notes.md for the fitting method"
  #endif
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
