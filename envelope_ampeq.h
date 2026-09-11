#pragma once

/**
 * envelope_ampeq.h
 *
 * ---- Envelope-path magnitude (insertion-loss) equalizer ----
 * TWO cascaded high-shelf biquads (ssb_shelf_biquad_t, see ssb_dsp.h - NOT
 * the unrelated, differently-shaped ssb_biquad_t already defined in
 * ssb_adc_filter.h; the two collided under the same name the first time
 * this was added and had to be renamed to fix a link error) on the
 * envelope path, partially compensating the analog reconstruction
 * filter's own high-frequency gain roll-off - the magnitude counterpart
 * to envelope_gdeq.h's phase/group-delay equalizer. The two are
 * independent and complementary: gdeq flattens the filter's group DELAY
 * (unity-magnitude all-pass, by construction cannot touch amplitude),
 * this flattens (partially) its GAIN roll-off (a real gain-shaping
 * filter, not all-pass - it DOES touch delay, more so now that a second,
 * higher-gain stage has been added - see the "Interaction with gdeq" note
 * below, and the 2026-09-04 entry in the #if block further down, for why
 * that's now a first-order consideration rather than a footnote).
 * Originally a single stage (2026-09-03); a second stage was added
 * 2026-09-04 specifically to extend the correction further toward
 * 8000Hz (Nyquist at this Fs). ---- 2026-09-04, later same day: split into
 * INDEPENDENT enable flags ---- Real-hardware validation of shelf2
 * (`ga_Trial2_TF.txt`, see the dated entry below) confirmed the predicted
 * magnitude gain (+6.7dB real at 8000Hz) but also confirmed dispersion
 * roughly doubles (75.7us->157.5us p-p), and the resulting two-tone IMD
 * came back marginally WORSE than shelf1 alone - so shelf1 and shelf2
 * were split into two independently-toggleable flags ('a' for shelf1,
 * 'A' for shelf2 - serial_commands.cpp) instead of one combined flag,
 * specifically so shelf1-only vs shelf1+shelf2 (the exact two configs
 * just A/B'd on the bench) can be re-selected from the serial console
 * without a reflash, for the wider IMD comparison the user wants to
 * record before deciding whether to keep pursuing shelf2 (e.g. after a
 * gdeq refit - see "Interaction with gdeq" below). Both default OFF,
 * same as before the split. Order in envelope_ampeq_process() is fixed
 * (shelf1 then shelf2 when both are on) but doesn't matter mathematically
 * - both LTI - so "shelf2 only, shelf1 off" is also a reachable, valid
 * combination via 'A' alone, even though that specific combination hasn't
 * been bench-tested (only off / shelf1-only / shelf1+shelf2 have real
 * data so far).
 *
 * ---- Why this exists ----
 * 2026-09-03: real-hardware amplitude+phase TF measurement
 * (`TF meas.txt`, same sweep as the very first group-delay TFA file -
 * phase columns match point-for-point, this one adds the amplitude
 * channel) confirmed the ENV_FILTER_PNP_BC327_ATTN analog filter's
 * insertion loss directly on the bench, referenced to the 100-300Hz
 * passband average (see group_delay_fit_notes.md's matching entry for
 * the full table and the residual-noise check):
 *
 *   700Hz   -0.45dB   (two-tone f1)
 *   1900Hz  -1.85dB   (two-tone f2)
 *   500Hz   -0.28dB   (IMD3 lower offset)
 *   1700Hz  -1.55dB   (IMD5 lower offset)
 *   3100Hz  -4.52dB   (IMD3 upper offset)
 *   4300Hz  -8.75dB   (IMD5 upper offset / old gdeq fit-band edge)
 *   8000Hz  -21.70dB  (MAX_FREQ_DEV_HZ limit - the worst measured point)
 *
 * This tracks the 2026-09-01 LTspice prediction well, especially at the
 * top end (-21.7 measured vs. -22.65 predicted @ 8000Hz) - real,
 * substantial, and now bench-confirmed, not just simulated.
 *
 * ---- Why a capped shelf, not a full inverse filter ----
 * A full inverse response would need up to ~+22dB of boost at 8000Hz
 * relative to the passband. Explicitly NOT attempted as a first cut:
 * that much boost, right at the same frequencies MAX_FREQ_DEV_HZ's
 * excursions and envelope-null transients already stress hardest, risks
 * trading a known, well-characterized amplitude-rolloff problem for an
 * unknown noise-floor/PWM-quantization/headroom problem that could be
 * worse. Discussed with the user 2026-09-03 - agreed to start with
 * something modest and measure the improvement before considering
 * anything closer to a full correction.
 *
 * ---- The chosen shelf ----
 * RBJ high-shelf (ssb_shelf_biquad_set_highshelf(), S=1 - see ssb_dsp.h),
 * corner ENV_AMPEQ_SHELF_FREQ_HZ, plateau gain ENV_AMPEQ_SHELF_GAIN_DB
 * below. A shelf (not a peaking/bell filter, and not the SAME shape as
 * the pre-Hilbert audio_fx chain's own 'e' presence EQ) was chosen
 * because the measured loss is MONOTONIC - it keeps climbing all the way
 * to 8000Hz - and a peaking filter, by construction, returns to 0dB on
 * both sides of its center (the existing +4dB/2200Hz/Q1 presence EQ is
 * already back to +0.00dB by 8000Hz - it cannot help here even in
 * principle; computed directly from biquad_set_peaking() during this
 * design, not assumed). A shelf's plateau instead holds a constant boost
 * at high frequency, matching the shape of a monotonic loss floor.
 *
 * Default: ENV_AMPEQ_SHELF_FREQ_HZ = 2500Hz, ENV_AMPEQ_SHELF_GAIN_DB =
 * 6.0dB (see the #define block below for the exact response this
 * produces and the resulting NET/residual loss at the table's key
 * frequencies). Both are simple #defines, easy to retune on the bench
 * without touching the filter math - this is a coarse, deliberately
 * conservative first cut, not a numerically-fitted correction the way
 * envelope_gdeq.h's coefficients are (there's no equivalent "minimize
 * peak-to-peak dispersion" objective for a single-shelf gain corrector;
 * two free parameters, chosen by inspection against the measured loss
 * table above, not by optimization).
 *
 * ---- Interaction with gdeq ----
 * Both are linear time-invariant filters, so mathematically the order
 * they're cascaded in doesn't change the combined response - applied
 * here right after envelope_gdeq_process() purely for code locality
 * (the two RSET-filter compensators living next to each other), not
 * because the order matters. A high-shelf biquad does have its own
 * group-delay contribution (unlike gdeq's all-pass sections, its
 * magnitude ISN'T flat, so its phase/delay isn't the free, independent
 * quantity ssb_allpass1_t's is) - NOT folded into gdeq's own fit.
 * Quantified 2026-09-03 for the single-shelf design (real hardware,
 * `ga_Trial1_TF.txt`): enabling 'a' alone reintroduced measured dispersion
 * from 19.4us to 81.7us peak-to-peak even though mean delay barely moved,
 * because gdeq's coefficients were fit against the analog filter ALONE.
 * CONFIRMED WORSE STILL on real hardware 2026-09-04 with shelf2 also on
 * (`ga_Trial2_TF.txt`): dispersion roughly DOUBLED again (measured
 * 75.7us->157.5us p-p, one consistent re-analysis pipeline for both
 * trials), and real two-tone IMD came back marginally worse than
 * shelf1-only - gdeq is overdue for a refit against the combined
 * analog+shelf1+shelf2 phase response, the same numerically-fit-against-
 * real-data method already used twice for smaller reasons (see
 * envelope_gdeq.h's history). NOT YET DONE. See group_delay_fit_notes.md's
 * matching 2026-09-04 entries for the full numbers and
 * gdeq_ampeq_delay_chart_v6.html for the real Trial1-vs-Trial2 chart.
 *
 * ---- Status ----
 * Shelf1: VALIDATED on real hardware 2026-09-03 (magnitude tracked
 * prediction to a few tenths of a dB; real IMD benefit confirmed, though
 * it does reintroduce the dispersion above). Shelf2: VALIDATED on real
 * hardware 2026-09-04 (magnitude gain real but somewhat short of
 * predicted; dispersion roughly doubles again; real IMD came back
 * marginally WORSE when combined with shelf1, in this still-unrefit-gdeq
 * state). Both OFF by default, independently toggled - 'a' for shelf1,
 * 'A' for shelf2 (serial_commands.cpp). Current recommendation (as of
 * 2026-09-04): run with 'a' ON / 'A' OFF (shelf1-only - the better real
 * compromise found so far) while a wider IMD comparison across both
 * configs is recorded; re-run through the 'w' chirp/TFA workflow any time
 * either flag changes to see the actual on-bench correction directly
 * before trusting a new config on two-tone/mic IMD testing.
 */

#include <stdbool.h>
#include "config.h"
#include "ssb_dsp.h"

// Selected at compile time by SAMPLE_RATE_HZ AND ENV_FILTER_VARIANT
// (config.h), same defensive-#error convention as envelope_gdeq.h's
// ENV_GDEQ_A1/A2 - this shelf was chosen against a real TF measurement
// of ONE specific filter/Fs combination, and silently applying it to a
// different filter revision (different loss shape) or Fs (different
// Nyquist, different fraction-of-band the shelf's corner/plateau cover)
// would be a subtle, wrong-shaped correction, not a crash.
#if SAMPLE_RATE_HZ == 16000 && ENV_FILTER_VARIANT == ENV_FILTER_PNP_BC327_ATTN
  // Chosen 2026-09-03 by inspection against the measured insertion-loss
  // table in the header comment above - NOT a numerically-fitted
  // optimum. Response this produces (RBJ high-shelf, S=1, computed
  // directly, not estimated):
  //   500Hz    +0.01dB   ->  net  -0.27dB  (measured -0.28dB alone)
  //   700Hz    +0.03dB   ->  net  -0.42dB  (measured -0.45dB alone)
  //   1700Hz   +0.95dB   ->  net  -0.60dB  (measured -1.55dB alone)
  //   1900Hz   +1.38dB   ->  net  -0.47dB  (measured -1.85dB alone)
  //   3100Hz   +4.41dB   ->  net  -0.11dB  (measured -4.52dB alone)
  //   4300Hz   +5.69dB   ->  net  -3.06dB  (measured -8.75dB alone)
  //   8000Hz   +6.00dB   ->  net -15.70dB  (measured -21.70dB alone)
  // Recovers most of the loss through the two-tone/IMD-offset region
  // (500-3100Hz), meaningfully reduces it at the old 4300Hz gdeq
  // fit-band edge, and leaves a deliberately large fraction of the
  // 8000Hz-region loss UNCORRECTED - the conservative, "modest" choice
  // discussed with the user rather than chasing the full ~22dB there.
  #define ENV_AMPEQ_SHELF_FREQ_HZ   2500.0f
  #define ENV_AMPEQ_SHELF_GAIN_DB   6.0f

  // ---- 2026-09-04: second shelf stage, extending correction toward
  // 8000Hz (= Nyquist at this Fs - there is no "further" past this) ----
  // Requested by the user specifically to see whether closing more of the
  // still-large 8000Hz gap (-15.70dB net, above) improves the higher-order
  // IMD products further. Cascaded AFTER the shelf above (order doesn't
  // matter, both LTI - see "Interaction with gdeq" below for why it DOES
  // matter for the delay side). Chosen the same way as the first shelf -
  // by inspection against the measured loss table, not by optimization -
  // biased toward leaving the already-good 500-1900Hz region alone (its
  // own corner is high enough that it contributes <0.02dB there) while
  // doing the heavy lifting from ~3100Hz up:
  //   500Hz    +0.00dB   ->  net  -0.27dB  (unchanged from shelf-1-only)
  //   700Hz    +0.00dB   ->  net  -0.42dB  (unchanged)
  //   1700Hz   +0.01dB   ->  net  -0.60dB  (unchanged)
  //   1900Hz   +0.01dB   ->  net  -0.46dB  (unchanged)
  //   3100Hz   +0.08dB   ->  net  -0.02dB  (was -0.11dB)
  //   4300Hz   +0.54dB   ->  net  -2.52dB  (was -3.06dB)
  //   8000Hz  +10.00dB   ->  net  -5.70dB  (was -15.70dB - the main target)
  // Three candidates were compared before picking this one (5000Hz/+8dB,
  // 5500Hz/+9dB, 6000Hz/+10dB) - all left 500-1900Hz equally untouched;
  // 6000Hz/+10dB was chosen because it clears the 3100Hz IMD3-upper offset
  // to within 0.02dB (the other two either overshoot slightly positive or
  // undershoot) while giving the largest 8000Hz recovery of the three.
  // Poles at (fc,gain)=(6000Hz,+10dB): 0.654 magnitude - comfortably
  // stable, nowhere near the unit circle despite sitting closer to
  // Nyquist than shelf 1.
  //
  // GROUP-DELAY COST - read before enabling on the bench: this stage's
  // own analytic group delay (computed the same way as gdeq's fits, RBJ
  // shelf phase, -dphase/dw) is LARGER than shelf 1's, not smaller, even
  // though it's "just" a second biquad - a direct consequence of its
  // larger gain (+10dB vs +6dB) and closer-to-Nyquist corner, not an
  // implementation shortcoming:
  //   shelf 1 alone:        64.9us peak-to-peak, ~0.03us mean, over
  //                         50-7999Hz (extremes: -41.6us @ 1333Hz,
  //                         +23.3us @ 3586Hz)
  //   shelf 2 alone:       125.4us peak-to-peak, ~0.01us mean, over the
  //                         same band (extremes: -39.1us @ 5029Hz,
  //                         +86.3us @ 7037Hz)
  //   shelf 1 + shelf 2:   148.6us peak-to-peak, ~0.04us mean (delays
  //                         add directly - both stages' phases sum)
  // At the two-tone/IMD-offset frequencies specifically (combined
  // shelf 1 + shelf 2 delay, relative to their own flat asymptote - NOT
  // relative to the analog filter or gdeq):
  //   500Hz -42.7us   700Hz -45.6us   1700Hz -49.5us   1900Hz -43.2us
  //   3100Hz  +0.2us   4300Hz -12.4us   8000Hz +70.5us
  // That is a ~120us swing across exactly the band this project has spent
  // the most effort flattening (see envelope_gdeq.h) - this is NOT
  // avoidable by choosing a differently-shaped shelf. A causal,
  // minimum-phase magnitude filter's gain and phase responses are locked
  // together (same Hilbert-transform relationship that makes gdeq's
  // filters unity-magnitude BY CONSTRUCTION the only way to get delay
  // correction with zero gain side-effect) - more high-frequency gain
  // recovered here necessarily means more nearby phase/delay distortion,
  // for ANY shelf design, not just this one. So "extend the correction
  // without breaking group delay" cannot mean "find a shelf shape with no
  // delay cost" - it means accepting this stage's delay contribution and
  // then REFITTING envelope_gdeq.h's all-pass coefficients against the
  // new combined (analog + shelf 1 + shelf 2) phase response, the same
  // way gdeq has already been refit twice before for smaller reasons (the
  // BC337->PNP_BC327_ATTN filter swap, then the LTspice->real-hardware
  // data swap) - see that file's history and group_delay_fit_notes.md's
  // matching 2026-09-04 entry. NOT YET DONE - gdeq's current coefficients
  // (a1=0.026173, a2=0.236810) were fit against the analog filter ALONE,
  // before shelf 1 existed, and are already known (2026-09-03 g+a
  // real-hardware trial) to let shelf 1's own dispersion back in
  // uncorrected; adding shelf 2 on top without refitting will make that
  // worse, not better, on the group-delay side even though it may help
  // IMDs. Recommended sequence: enable this stage, re-run the 'w'
  // chirp/TFA workflow to measure the REAL combined magnitude+phase
  // response (don't trust this analytic prediction alone - shelf 1's
  // prediction was accurate to a few tenths of a dB/matched shape, but
  // gdeq's own history shows real hardware occasionally surprises), THEN
  // decide whether the resulting IMD change on two-tone is worth a gdeq
  // refit to recover flat delay.
  //
  // REAL-HARDWARE RESULT, 2026-09-04 (`ga_Trial2_TF.txt`, same day):
  // magnitude gain at 8000Hz was real but a bit short of predicted
  // (+6.7dB measured vs. +10.0dB modeled - net -9.30dB vs. the -5.70dB
  // predicted above). Delay dispersion roughly DOUBLED again on top of
  // shelf 1's own already-measured increase (75.7us->157.5us p-p,
  // consistent re-analysis of both trials), and matched this stage's own
  // predicted delay shape closely except at 8000Hz itself, where the real
  // jump (+85.1us) exceeded the +62.3us analytic prediction - real
  // hardware runs a bit worse than the idealized model right at the band
  // edge. Real two-tone IMD with both stages on: marginally WORSE than
  // shelf 1 alone. Per-stage enable flags were split as a result (see the
  // header-comment note above) so shelf 1-only vs shelf 1+shelf 2 can be
  // re-selected without a reflash - `'a'` still controls this stage
  // (shelf 1) exactly as before; shelf 2 below now has its own flag.
  // Current recommendation: leave shelf 2 OFF (via 'A') until a gdeq
  // refit makes a fair comparison possible - see group_delay_fit_notes.md's
  // matching entry and gdeq_ampeq_delay_chart_v6.html.
  #define ENV_AMPEQ_SHELF2_FREQ_HZ  6000.0f
  #define ENV_AMPEQ_SHELF2_GAIN_DB  10.0f
#else
  #error "ENV_AMPEQ_SHELF_FREQ_HZ/GAIN_DB have only been chosen for ENV_FILTER_PNP_BC327_ATTN at SAMPLE_RATE_HZ=16000 - see envelope_ampeq.h / group_delay_fit_notes.md for the real-hardware TF data needed to pick new values"
#endif

// Zeroes BOTH shelf biquads' state and (re)applies the coefficients above.
// Call once from setup() - always, regardless of either enabled default,
// so enabling either stage later only ever needs its own _set_enabled(),
// not a separate init path (same convention as envelope_gdeq_init()).
void envelope_ampeq_init(void);

// Runs `envelope` through whichever of the two high-shelf biquads are
// currently enabled, in cascade (shelf 1 then shelf 2 when both are on -
// order doesn't affect the result, both LTI), and returns the result;
// unchanged if neither is enabled. Call unconditionally from dsp_task,
// once per tick, for every audio source (same rationale as
// envelope_gdeq_process() - see that file - including ENVSTEP/AMTEST so
// those isolation tests exercise the real combined response).
float IRAM_ATTR envelope_ampeq_process(float envelope);

// ---- Shelf 1 (2500Hz/+6dB) - 'a' in serial_commands.cpp ----
// Same enable/disable API and reset-on-off->on-transition behavior this
// module has had since 2026-09-03 (see envelope_gdeq_set_enabled() for
// the reasoning) - unchanged by the 2026-09-04 flag split below, existing
// callers/presets/settings fields keep working as-is.
bool envelope_ampeq_get_enabled(void);
void envelope_ampeq_set_enabled(bool enable);

// ---- Shelf 2 (6000Hz/+10dB) - 'A' in serial_commands.cpp ----
// Added 2026-09-04 as an INDEPENDENT flag (see the header-comment note
// above for why: shelf1-only vs shelf1+shelf2 needed to be A/B-able from
// the serial console without a reflash, once real hardware showed shelf2
// makes two-tone IMD marginally worse in this still-unrefit-gdeq state).
// Same reset-on-off->on-transition behavior, applied to shelf 2's own
// biquad state only - toggling shelf 2 does NOT reset shelf 1's state and
// vice versa, so either can be flipped independently without disturbing
// the other's continuity.
bool envelope_ampeq_shelf2_get_enabled(void);
void envelope_ampeq_shelf2_set_enabled(bool enable);

/**
 * @brief 2026-09-11: NaN/Inf canary for this module's two shelf biquads -
 *        see moving_forward_notes.md's matching entry for why this exists
 *        (an IIR filter's own feedback state, unlike the memoryless
 *        freq_dev/atan2 computation, can carry a bad value forward
 *        indefinitely once introduced - flush_denorm() elsewhere in this
 *        codebase only catches near-zero subnormals, not NaN/Inf). Checks
 *        y1/y2 - the values actually fed back into the next sample - not
 *        x1/x2, since a bad output would already be visible at y before it
 *        could reach the input-history side anyway, same reasoning as
 *        checking only y1 for ssb_allpass1_t's canary. true = finite (OK)
 *        even when the corresponding shelf is currently disabled - a
 *        disabled shelf's state is frozen, not being fed anything, so
 *        "still finite" is the right steady-state answer while off, not a
 *        false positive.
 */
typedef struct {
    bool shelf1_finite;
    bool shelf2_finite;
} env_ampeq_canary_t;

void envelope_ampeq_get_canary(env_ampeq_canary_t *out);
