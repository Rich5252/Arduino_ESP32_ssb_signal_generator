#pragma once

/**
 * envelope_ampeq.h
 *
 * ---- Envelope-path magnitude (insertion-loss) equalizer ----
 * A single high-shelf biquad (ssb_shelf_biquad_t, see ssb_dsp.h - NOT the
 * unrelated, differently-shaped ssb_biquad_t already defined in
 * ssb_adc_filter.h; the two collided under the same name the first time
 * this was added and had to be renamed to fix a link error) on the
 * envelope path, partially compensating the analog reconstruction
 * filter's own high-frequency gain roll-off - the magnitude counterpart
 * to envelope_gdeq.h's phase/group-delay equalizer. The two are
 * independent and complementary: gdeq flattens the filter's group DELAY
 * (unity-magnitude all-pass, by construction cannot touch amplitude),
 * this flattens (partially) its GAIN roll-off (a real gain-shaping
 * filter, not all-pass - it doesn't touch delay in any way this project
 * currently corrects for, though a genuine biquad does have its own
 * small delay/phase contribution near its corner; not separately
 * accounted for here since the shelf's plateau is well below where gdeq
 * itself is doing its own work - see the "Interaction with gdeq" note
 * below).
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
 * because the order matters. A high-shelf biquad does have its own small
 * group-delay contribution (unlike gdeq's all-pass sections, its
 * magnitude ISN'T flat, so its phase/delay isn't the free, independent
 * quantity ssb_allpass1_t's is) - not yet characterized or folded into
 * gdeq's own fit. If the combined on-bench group delay ends up
 * measurably different from the gdeq-alone prediction once this is
 * enabled, that's the first place to look.
 *
 * ---- Status ----
 * NOT YET VALIDATED ON REAL HARDWARE. Off by default, same convention as
 * 'g' - toggle via 'a' (serial_commands.cpp) to A/B it, ideally re-run
 * through the 'w' chirp/TFA workflow first (the amplitude channel will
 * show the actual on-bench correction directly, the same way the phase
 * channel already validated gdeq's refit) before trusting it on
 * two-tone/mic IMD testing.
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
#else
  #error "ENV_AMPEQ_SHELF_FREQ_HZ/GAIN_DB have only been chosen for ENV_FILTER_PNP_BC327_ATTN at SAMPLE_RATE_HZ=16000 - see envelope_ampeq.h / group_delay_fit_notes.md for the real-hardware TF data needed to pick new values"
#endif

// Zeroes the shelf biquad's state and (re)applies the coefficients above.
// Call once from setup() - always, regardless of the enabled default, so
// enabling later via 'a' only ever needs envelope_ampeq_set_enabled(),
// not a separate init path (same convention as envelope_gdeq_init()).
void envelope_ampeq_init(void);

// If enabled, runs `envelope` through the high-shelf biquad and returns
// the result; otherwise returns it unchanged. Call unconditionally from
// dsp_task, once per tick, for every audio source (same rationale as
// envelope_gdeq_process() - see that file - including ENVSTEP/AMTEST so
// those isolation tests exercise the real combined response).
float IRAM_ATTR envelope_ampeq_process(float envelope);

bool envelope_ampeq_get_enabled(void);

// Sets the enabled flag. On an off->on transition, also resets the
// biquad's state (avoids feeding stale x1/x2/y1/y2 from however long
// it's been since this was last on, or since boot, into the first
// sample after re-enabling - same reasoning as
// envelope_gdeq_set_enabled()'s reset). A no-op transition does NOT
// reset state, for the same reason gdeq's doesn't (continuous operation
// across preset switches that both have this on).
void envelope_ampeq_set_enabled(bool enable);
