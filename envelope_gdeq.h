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
 * ---- 2026-09-05: VALIDATED on real hardware (group delay), then found to
 * be a genuinely mixed result once the full spectrum was checked - NOW
 * RUNTIME-SELECTABLE via `'G'`, not just a compile-time flag ----
 * `gaA_Trial3_Optimised_gd_TF.txt` (corrected re-run, flag confirmed
 * active) measured 87.7us p-p / 195.7us mean, matching the 72.6us/197.1us
 * prediction above closely (RMS 2.7us) - real, substantial confirmation
 * of the group-delay claim. BUT a same-session two-tone spectrum
 * comparison (`2gaA_spectra.txt` vs `3gaA_spectra.txt`, both at their own
 * correctly-tuned `relative_delay`) then found a genuinely two-sided
 * result: classic close-in two-tone IMD (3rd-11th order) is better with
 * this candidate at every order checked, but a much larger forest of
 * additional intermodulation lines appears roughly 4-16kHz from carrier,
 * running 11-19dB HIGHER than the default coefficients produce in the same
 * region - not an unrelated spur (falls on the GCD(700,1900)=100Hz grid,
 * genuine intermodulation of the same two tones), and not a delay-
 * misalignment artifact (both configs were at their own bench-optimal
 * relative delay). A subsequent design-space search (same grid+polish
 * method as every fit in this file, several smoothness-aware objectives,
 * 2/3/4-section cascades) found NO better coefficient set exists in this
 * filter architecture - the candidate below is already at the Pareto-
 * optimal point for both p-p dispersion and local delay-curve smoothness
 * simultaneously, so whatever's driving the far-out spur growth is outside
 * plain group-delay theory (see group_delay_fit_notes.md's matching
 * 2026-09-05 entries for the full derivation, the spectra chart, and the
 * coefficient-search results). **Net: NOT currently recommended for
 * adoption** - real group-delay and close-in-IMD wins, but a real far-out
 * spectral cost too, and redesigning the filter itself doesn't fix it.
 *
 * Given that ambiguity - and this project's own repeated finding that no
 * single scalar metric here (p-p dispersion, or classic low-order IMD) has
 * reliably predicted overall real-world quality - this now answers its own
 * "should this become runtime-selectable" question from the entry above:
 * yes. `'G'` (envelope_gdeq_set_use_aa_candidate(), serial_commands.cpp)
 * switches between the two coefficient sets live, without a reflash, on
 * ENV_FILTER_PNP_BC327_ATTN @ 16000Hz only (`ENV_GDEQ_HAS_AA_CANDIDATE`
 * below) - the only filter/Fs this candidate has ever been fit for.
 * Switching calls `ssb_allpass1_init()` on both sections with the new
 * coefficient (documented as cheap/task-context-safe, and it zeroes state
 * the same way enabling `'g'` from off does - no stale x1/y1 glitch on the
 * next sample). Persisted via a new trailing `PersistentSettings` field
 * (`env_gdeq_use_aa_candidate`, off/false by default, same convention as
 * `env_ampeq_shelf2_enable`). **Re-tune `'['`/`']'` after switching** -
 * bench-confirmed 2026-09-05: 2 samples (default) vs. 2.83 samples
 * (candidate) for minimum close-in IMD, NOT the ~124.4us/125.4us
 * theoretical-mean-delay figures below, which turned out to under-predict
 * the real shift by about 1 sample (see group_delay_fit_notes.md's
 * `relative_delay` retuning entries for why - the relevant reading is
 * local group delay AT the actual tone frequencies, not the mean over the
 * whole 100-8000Hz band, which happened to look almost identical between
 * the two sets while the local values at 500-2200Hz differ by 52-64us).
 *
 * Mean added delay for the candidate: ~124.4us (1.990 samples @ 16000Hz) -
 * close to the default's 125.4us/2.006 samples in theory, but see the
 * paragraph above for why the REAL retuned optimum differs by more than
 * that difference alone would suggest.
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
 *
 * ---- 2026-09-06: a THIRD coefficient set ("candidate B"), and 'G' becomes
 * a 3-way CYCLE instead of a 2-way toggle ---- The mid-band local-slope-
 * vs-p-p Pareto refit (group_delay_fit_notes.md, redone against the real,
 * cross-validated HiRes chirp data instead of the noisy reconstruction the
 * 2026-09-05 version of this search used) found something sharper than
 * that entry's own "already Pareto-optimal" conclusion: the a+A candidate
 * above (a1=a2=-0.139115), while nearly optimal for FULL-BAND p-p, makes
 * the LOCAL group-delay slope through 2800-4500Hz genuinely WORSE than
 * running with gdeq off entirely (49.6us/kHz vs. bare's own 36.8us/kHz) -
 * a real regression, not just a smaller-than-hoped improvement, and a
 * sharper, now real-data-backed version of the mechanism implicated in
 * that far-out two-tone spur forest. "Candidate B" (a1=a2=+0.09) trades
 * back some of the a+A candidate's p-p advantage (105.2us -> 136.0us,
 * still meaningfully better than the default pair's 156.5us) for a real
 * 43% reduction in that local mid-band slope (49.6 -> 28.1us/kHz, close to
 * the default pair's own best-achievable 26.1us/kHz). Same symmetric
 * single-parameter (a1=a2) design as the existing candidate, fitted for
 * the same ENV_FILTER_PNP_BC327_ATTN @ SAMPLE_RATE_HZ=16000 combination
 * against the same real bare (analog+shelf1+shelf2) curve. See
 * `ENV_GDEQ_A1_CANDIDATE_B`/`ENV_GDEQ_A2_CANDIDATE_B` below and
 * `env_gdeq_variant_t` for how it's selected.
 *
 * **THIS IS A MODEL PREDICTION, NOT YET A BENCH RESULT** - unlike every
 * other coefficient set in this file, candidate B has not been measured on
 * real hardware at all (no TFA sweep, no two-tone spur-forest check). It's
 * offered for bench-testing, not as a recommendation - treat it with at
 * least as much caution as the a+A candidate's own "mixed result, not
 * currently recommended" status above, more so until it's actually been on
 * the bench.
 *
 * With three sets now selectable, `s_env_gdeq_use_aa_candidate`'s plain
 * bool storage stopped being able to express "which one" - replaced with
 * an `env_gdeq_variant_t` enum (`ENV_GDEQ_VARIANT_DEFAULT` = 0,
 * `ENV_GDEQ_VARIANT_AA_CANDIDATE` = 1, `ENV_GDEQ_VARIANT_CANDIDATE_B` = 2),
 * same style as `envelope_interp_curve_t`'s own enum (deliberately
 * DEFAULT = 0, so a preset/struct field that zero-fills - see settings.h -
 * lands on today's actual default, not a stale mid-project one). `'G'`
 * (serial_commands.cpp) now CYCLES default -> a+A candidate -> candidate B
 * -> default -> ..., same modulo-cycle convention 'C' (envelope_interp.h)
 * and 'f' (adc_capture.h) already use, automatically skipping any variant
 * this build never fitted (`envelope_gdeq_variant_available()`) rather
 * than landing on a coefficient pair that was never fit for the active
 * filter/Fs - same "never silently run with an unfit pair" discipline the
 * old `ENV_GDEQ_HAS_AA_CANDIDATE` guard enforced, generalized to
 * `ENV_GDEQ_HAS_CANDIDATE_B` for the new set. `envelope_gdeq_get_use_aa_
 * candidate()`/`envelope_gdeq_set_use_aa_candidate()` are gone - every
 * call site (serial_commands.cpp, ssb_mic_test.ino, settings.h's
 * `PersistentSettings`) now reads/writes the enum instead.
 */

#include <stdbool.h>
#include "config.h"
#include "ssb_dsp.h"

// Which fitted gdeq coefficient set is active - see the header comment's
// 2026-09-06 entry above for why this replaced a plain bool. Deliberately
// an ordinary enum (not enum class) so it behaves like every other project
// enum here (audio_source_t, envelope_interp_curve_t, adc_lpf_mode_t) -
// usable directly as a PersistentSettings field, printable via a
// name-lookup table, and safe to `% ENV_GDEQ_VARIANT_COUNT` for the 'G'
// cycle. ENV_GDEQ_VARIANT_DEFAULT is deliberately value 0 - see
// envelope_interp_curve_t's own comment in envelope_interp.h for why that
// matters for struct zero-fill.
typedef enum {
    ENV_GDEQ_VARIANT_DEFAULT = 0,
    ENV_GDEQ_VARIANT_AA_CANDIDATE = 1,
    ENV_GDEQ_VARIANT_CANDIDATE_B = 2,
    ENV_GDEQ_VARIANT_COUNT = 3
} env_gdeq_variant_t;

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
// ENV_GDEQ_HAS_AA_CANDIDATE defaults to 0 - only the PNP_BC327_ATTN@16000Hz
// branch below (the only filter/Fs this candidate has ever been fit for)
// #defines it to 1. Checked by 'G' (serial_commands.cpp) to decide whether
// the runtime toggle does anything on this build, rather than silently
// switching to a coefficient pair that was never fit for the active
// filter/Fs - same "don't silently run with the wrong pair" discipline as
// the #error guards below.
#define ENV_GDEQ_HAS_AA_CANDIDATE 0
// Same convention, for the 2026-09-06 "candidate B" set - see this file's
// header comment and ENV_GDEQ_A1_CANDIDATE_B/ENV_GDEQ_A2_CANDIDATE_B below.
// Kept as its own independent flag (not folded into ENV_GDEQ_HAS_AA_
// CANDIDATE) so a future filter/Fs fit could have one set without the
// other without misrepresenting which sets it actually has.
#define ENV_GDEQ_HAS_CANDIDATE_B 0

#if SAMPLE_RATE_HZ == 16000
  #if ENV_FILTER_VARIANT == ENV_FILTER_BC337
    #define ENV_GDEQ_A1  -0.023900f
    #define ENV_GDEQ_A2   0.447131f
  #elif ENV_FILTER_VARIANT == ENV_FILTER_PNP_BC327_ATTN
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
    // existed - see ENV_GDEQ_A1_AA_CANDIDATE/ENV_GDEQ_A2_AA_CANDIDATE
    // below for the shelf1+shelf2-specific alternative, and this file's
    // "2026-09-05" header entry for why that alternative is now
    // RUNTIME-selectable (`'G'`) rather than a separate compile-time
    // build. THIS pair (below) is what's active at boot and whenever
    // envelope_gdeq_set_variant(ENV_GDEQ_VARIANT_DEFAULT) is in effect -
    // i.e. the default, recommended state.
    #define ENV_GDEQ_A1   0.026173f
    #define ENV_GDEQ_A2   0.236810f
    // For reference/history (NOT active) - 2026-09-01 LTspice-only fit,
    // 100-4300Hz band only: a1=a2=0.622515f. Diverges badly (449us p-p)
    // if evaluated past 4300Hz, which is exactly why this needed a real
    // re-fit rather than just widening the old coefficients' claimed
    // range - see header comment.

    // ---- 'a'+'A'-specific candidate (2026-09-04 fit, 2026-09-05 mixed
    // real-hardware result - see header comment above in full) ----
    // Grid search + global (differential-evolution) search + Nelder-Mead
    // refinement, same discipline as every fit in this file, against the
    // reconstructed analog+shelf1+shelf2 bare curve (88.4us p-p, 71.1us
    // mean over 100-8000Hz). 2/3/4-section searches all converged to
    // essentially the same ~71-73us p-p floor (a real ceiling for this
    // filter type on this curve shape - see group_delay_fit_notes.md for
    // why: two interior extrema plus a steep near-Nyquist edge from
    // shelf2's own delay signature), so 2 sections (same architecture as
    // above) was kept rather than adding more for a diminishing-returns
    // ~1-2us gain. Result: 157.5us -> 72.6us p-p (~2.2x) relative to what
    // the default pair above gives on this same bare curve - NOT a ~4x
    // flattening like the analog-alone fit above achieved, and NOT
    // uniform - see the header comment and group_delay_fit_notes.md's
    // trade-off table. Available at runtime via `'G'`
    // (envelope_gdeq_set_variant(ENV_GDEQ_VARIANT_AA_CANDIDATE)) ONLY for
    // this filter/Fs - this is the only combination it's ever been fit for.
    #define ENV_GDEQ_A1_AA_CANDIDATE  -0.139115f
    #define ENV_GDEQ_A2_AA_CANDIDATE  -0.139115f
    #undef  ENV_GDEQ_HAS_AA_CANDIDATE
    #define ENV_GDEQ_HAS_AA_CANDIDATE 1

    // ---- "Candidate B" (2026-09-06 mid-band Pareto refit - see this
    // file's header comment above in full) ---- Grid search
    // ((a1,a2) in [-0.30,0.30]^2, 0.01 step) + Nelder-Mead polish, same
    // discipline as every fit in this file, against the REAL measured
    // bare (analog+shelf1+shelf2) curve from HiRes_aA_TF.txt - a genuine
    // upgrade over the reconstructed curve the a+A candidate above was fit
    // against. Objective: minimize the worst-case (max abs) local group-
    // delay slope in 2800-4500Hz subject to a full-band (387-7547Hz) p-p
    // budget, then take the point on the resulting Pareto envelope nearest
    // a1=a2=+0.09. Result: full-band p-p 136.0us (vs. the a+A candidate's
    // 105.2us and the default pair's 156.5us), local 2800-4500Hz max slope
    // 28.1us/kHz (vs. the a+A candidate's 49.6us/kHz - WORSE than bare's
    // own 36.8us/kHz - and the default pair's 26.1us/kHz). See
    // group_delay_fit_notes.md's 2026-09-06 entry for the full derivation,
    // the two-stage-differentiation methodology fix behind the local-slope
    // metric, and the additive-model validation against real measured
    // aAG3/aAg2 hardware data. Available at runtime via `'G'`
    // (envelope_gdeq_set_variant()) ONLY for this filter/Fs - same
    // restriction as the a+A candidate above, for the same reason (never
    // fit anywhere else). NOT YET VALIDATED ON REAL HARDWARE AT ALL - see
    // header comment.
    #define ENV_GDEQ_A1_CANDIDATE_B  0.09f
    #define ENV_GDEQ_A2_CANDIDATE_B  0.09f
    #undef  ENV_GDEQ_HAS_CANDIDATE_B
    #define ENV_GDEQ_HAS_CANDIDATE_B 1
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

// Fallback definition so envelope_gdeq.cpp compiles unconditionally (it
// references ENV_GDEQ_A1_AA_CANDIDATE/A2 behind a runtime, not #if, check -
// see envelope_gdeq_set_variant()) on filter/Fs combinations where
// ENV_GDEQ_HAS_AA_CANDIDATE is 0. Never actually selected at runtime on
// those builds - envelope_gdeq_set_variant() refuses to select the
// candidate unless ENV_GDEQ_HAS_AA_CANDIDATE is 1 - so the specific
// value here doesn't matter; same as the default pair keeps this a no-op.
#ifndef ENV_GDEQ_A1_AA_CANDIDATE
  #define ENV_GDEQ_A1_AA_CANDIDATE ENV_GDEQ_A1
  #define ENV_GDEQ_A2_AA_CANDIDATE ENV_GDEQ_A2
#endif

// Same reasoning as the ENV_GDEQ_A1_AA_CANDIDATE fallback immediately
// above, for the 2026-09-06 candidate B set - never actually selected at
// runtime where ENV_GDEQ_HAS_CANDIDATE_B is 0
// (envelope_gdeq_variant_available() gates it), so the fallback value
// doesn't matter beyond letting this file compile unconditionally.
#ifndef ENV_GDEQ_A1_CANDIDATE_B
  #define ENV_GDEQ_A1_CANDIDATE_B ENV_GDEQ_A1
  #define ENV_GDEQ_A2_CANDIDATE_B ENV_GDEQ_A2
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

// Added 2026-09-05 as a runtime toggle between two fitted coefficient
// pairs; extended 2026-09-06 to a 3-way cycle (default / a+A candidate /
// candidate B, `env_gdeq_variant_t` above) once a third set existed - see
// this file's header comment for the full history and real-hardware
// results of each. Returns which set is currently selected - always
// ENV_GDEQ_VARIANT_DEFAULT on a filter/Fs build where the requested
// variant was never fitted (see envelope_gdeq_variant_available() below).
env_gdeq_variant_t envelope_gdeq_get_variant(void);

// True if `variant` was actually fitted for the active filter/Fs
// (ENV_FILTER_VARIANT + SAMPLE_RATE_HZ, config.h) - ENV_GDEQ_VARIANT_DEFAULT
// is always true; ENV_GDEQ_VARIANT_AA_CANDIDATE/_CANDIDATE_B follow
// ENV_GDEQ_HAS_AA_CANDIDATE/ENV_GDEQ_HAS_CANDIDATE_B above. Used by the
// 'G' serial handler to skip over unfitted variants when cycling, rather
// than landing on a coefficient pair that was never fit for this build -
// same "never silently run with an unfit pair" discipline
// envelope_gdeq_set_variant() itself also enforces.
bool envelope_gdeq_variant_available(env_gdeq_variant_t variant);

// Short, human-readable name for `variant`, for serial replies/banners -
// includes each set's own coefficients and, for candidate B, a reminder
// that it's a model prediction, not yet bench-validated (see the header
// comment's 2026-09-06 entry).
const char *envelope_gdeq_variant_name(env_gdeq_variant_t variant);

// Selects which coefficient pair is active and re-initializes BOTH
// sections with it via ssb_allpass1_init() (documented as cheap/safe from
// task context, and it zeroes state as a side effect - same no-stale-x1/y1
// reasoning as envelope_gdeq_set_enabled()'s off->on reset, so switching
// live never glitches the next sample with a mismatched coefficient/state
// pair). Requesting a variant that envelope_gdeq_variant_available() says
// isn't fitted for this build falls back to ENV_GDEQ_VARIANT_DEFAULT
// instead - same "don't silently run with an unfit pair" discipline as
// before, generalized from the old two-state toggle. After switching,
// re-tune '['/']' - the bench-confirmed optimum for the a+A candidate
// differs by about 1 sample from the default pair's (2 vs. 2.83 @ 16000Hz
// for the g+a+A config), NOT the smaller shift the pairs' theoretical mean
// delays alone would suggest (see envelope_gdeq.h's header comment and
// group_delay_fit_notes.md for why - it's a local-group-delay-at-the-tone-
// frequencies effect, not a mean-delay one); candidate B hasn't been
// bench-tuned at all yet, so '['/']' will need finding from scratch there.
void envelope_gdeq_set_variant(env_gdeq_variant_t variant);
