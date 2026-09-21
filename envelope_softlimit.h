#pragma once

/**
 * envelope_softlimit.h
 *
 * ---- Soft-knee saturation for the envelope path ----
 * 2026-09-21: first cut, requested alongside envelope_alc.h as a graceful
 * alternative to relying purely on the final hard clamp (ssb_mic_test.ino's
 * "envelope < 0 / envelope > 1" pair) to catch overshoot the earlier
 * stages (envelope_gdeq_process()/envelope_ampeq_process(), and
 * envelope_alc.h's own leveler when it can't duck fast enough) let
 * through. Positioned AFTER envelope_alc_process() and BEFORE
 * envelope_predistort_process() - the user's explicit, final placement
 * instruction ("needs to go before Distortion correction since that is
 * needed 'always'") applies identically to this stage: predistort's own
 * [0,1] input clamp (envelope_predistort.cpp) must keep seeing a
 * controlled signal every tick regardless of predistort's own on/off
 * state, so this stage runs before it, not instead of it.
 *
 * ---- 2026-09-21, SAME DAY, BUGFIX: the first cut raised the DC level of
 * quiet/near-null signal - fixed by re-deriving the lower rail from
 * scratch ----
 * The first cut mirrored the upper knee's shape onto the lower rail
 * (ENV_SOFTLIMIT_KNEE_LO = 0.15, same tanh formula reflected around that
 * point). The user caught this on the bench ("at low signal level it
 * raises the env level DC value") and it's confirmed by direct
 * calculation, not just plausible: with the old formula, envelope=0.0
 * (a genuine envelope NULL - not an edge case, the single most common
 * operating point this project's whole null_bias_investigation.md exists
 * to study) mapped to 0.0358, and envelope=0.05 mapped to 0.0626 - a
 * real, constant-ish DC floor injected across the entire 0-0.15 range,
 * which is NOT overshoot at all, just ordinary quiet audio and null
 * crossings. Root cause: unlike the upper knee (0.85), which sits well
 * inside the legitimate operating range with real headroom between it and
 * the rail (1.0) for the curve to ease through, a lower knee at a POSITIVE
 * value like 0.15 sits inside the legitimate range on the WRONG side - it
 * treats ordinary low-but-valid signal as if it were undershoot, because
 * "undershoot" for this rail can only mean actually-negative envelope,
 * not "close to the rail from above."
 *
 * ---- Why the lower rail can't just mirror the upper one (the real
 * mathematical constraint, not just a tuning miss) ----
 * The upper knee works because there's a positive-width gap between the
 * knee (0.85) and the rail (1.0) for the curve to live in, while
 * everything below the knee - including values arbitrarily close to the
 * knee from below - stays completely untouched. For the lower rail, the
 * rail itself (0.0) IS the boundary of the legitimate range - there is no
 * equivalent gap "below the knee" that's still legitimate signal, because
 * anything from 0 up to 1 is normal. So the ONLY sound place for a lower
 * knee is 0.0 itself, engaging the curve exclusively for envelope < 0.0
 * (true, physically-invalid undershoot). But requiring the curve to (a)
 * pass through (0,0) matching the passthrough branch's value there, (b)
 * stay >= 0 for all envelope < 0 (never crossing back negative), and (c)
 * be monotonic (more negative input never means a SMALLER output
 * magnitude) is mathematically incompatible with also matching the
 * passthrough branch's slope of 1 at that boundary - a curve with slope 1
 * at x=0 that must stay >=0 for x<0 would have to go negative immediately
 * to the left of 0, contradiction. So a small derivative mismatch
 * (C0-continuous in VALUE, not C1 in slope) at exactly envelope=0 is
 * unavoidable for ANY smooth, non-negative, monotonic lower-rail curve -
 * this stage picks the curve shape that makes that mismatch as benign as
 * possible (see next section) rather than pretending it can be avoided.
 *
 * ---- The fix: a curve with ZERO slope at the rail, not unity slope ----
 * Lower side (envelope < 0.0f):
 *   u = envelope / ENV_SOFTLIMIT_LOWER_SPAN   (negative, since envelope < 0)
 *   y = ENV_SOFTLIMIT_LOWER_SPAN * (1 - sech(u))     [sech(u) = 1/cosh(u)]
 * At envelope=0: sech(0)=1, y=0 - matches the passthrough branch's value
 * exactly (fixes the reported bug: zero input now gives zero output, not
 * 0.0358). Near envelope=0, sech(u) = 1 - u^2/2 + O(u^4) (even function,
 * no LINEAR term) so y is QUADRATIC in envelope for small excursions
 * (y ~ envelope^2 / (2*ENV_SOFTLIMIT_LOWER_SPAN)) rather than the old
 * design's implicit near-|envelope| linear response - e.g. envelope=-0.01
 * now maps to ~0.00033, not ~0.06. This matters for a subtler reason than
 * just "smaller number": a linear |x|-shaped response to a small
 * symmetric ripple around zero (which is what a straight mirror of the
 * upper knee would give, since tanh(u)~u for small u) would FULL-WAVE
 * RECTIFY the ripple, giving MORE average DC bias than even the existing
 * hard clamp does on the same ripple (a hard clamp only zeroes the
 * negative half, i.e. half-wave rectification - genuinely less biased
 * than a symmetric fold-up would be). The quadratic (zero-slope-at-origin)
 * shape avoids that regression: for small ripple amplitudes it introduces
 * LESS bias than a hard clamp would, only asymptotically approaching the
 * ENV_SOFTLIMIT_LOWER_SPAN plateau for genuinely large negative
 * excursions (envelope -> -infinity => y -> ENV_SOFTLIMIT_LOWER_SPAN,
 * verified: sech(u) -> 0 as |u| -> infinity), where a graceful bound
 * matters more than matching a hard clamp's small-signal behavior.
 *
 * Upper side is UNCHANGED from the original design (it was never the
 * problem - see the bug analysis above for why the upper knee's geometry
 * doesn't have this issue):
 *   span = 1.0 - ENV_SOFTLIMIT_KNEE_HI
 *   y = ENV_SOFTLIMIT_KNEE_HI + span * tanhf((envelope - ENV_SOFTLIMIT_KNEE_HI) / span)
 * C1-continuous at ENV_SOFTLIMIT_KNEE_HI (value AND slope both match the
 * unchanged middle region there - see the original 2026-09-21 derivation,
 * unchanged by this bugfix).
 *
 * ---- Honest open question, not fully resolved by this fix ----
 * The lower-rail curve above is provably BETTER than both the original
 * buggy version (no more constant DC floor) and a plain hard clamp (less
 * bias for small ripples) at the extremes, but a smooth, non-negative,
 * monotonic curve matching passthrough's VALUE at envelope=0 can only be
 * made "least-bias" for small excursions by construction (zero slope at
 * the boundary) - it hasn't been proven optimal in any stronger sense, and
 * has NOT been bench-validated against how much genuine sub-zero ringing
 * envelope_gdeq_process()/envelope_ampeq_process() actually produce on
 * real hardware. If bench testing shows negligible real negative
 * excursion in practice, the honest conclusion may be that this whole
 * lower-rail branch is solving a largely theoretical problem and could be
 * simplified to a plain floor-at-0 (identical to what the downstream hard
 * clamp already does) without losing anything real - worth checking
 * before investing further tuning here.
 *
 * ---- Why a knee-based curve at all, not a single-shot tanh(envelope) ----
 * A plain tanhf(envelope) would compress the ENTIRE range, including
 * normal, well-inside-bounds envelope values that were never in danger of
 * clipping - changing this project's mapping/predistort LUT tuning
 * (env_pwm_offset/env_pwm_scale, or the predistort LUT itself) at every
 * operating point, not just at the extremes. Instead this passes envelope
 * through UNCHANGED for all of [0.0, ENV_SOFTLIMIT_KNEE_HI] and only
 * engages a curve outside that range - so normal-level audio (including
 * quiet audio and null crossings, after this bugfix) sees zero effect
 * from this stage, and only genuine overshoot/undershoot gets rounded off
 * instead of hard-clipped.
 *
 * ---- Handles BOTH rails, unlike the final hard clamp's symmetric pair
 * happening to be the only place both were previously handled ----
 * envelope can legitimately go negative here (gdeq is an all-pass with no
 * clamp of its own; ampeq's shelves are real filters, not all-pass, and
 * can ring past zero on a fast edge) - see the envelope-signal-path
 * hard-limit trace referenced in moving_forward_notes.md's 2026-09-21
 * entry for the full per-stage clamp/no-clamp map this was designed
 * against.
 *
 * ---- Stateless ----
 * A pure function of its input, same as envelope_predistort_process() -
 * see that module's own comment for why that means no reset-on-
 * enable/disable transition concern the way envelope_gdeq.h/
 * envelope_ampeq.h's IIR filter memory needs (nothing to glitch by
 * flipping the enable flag mid-stream).
 *
 * ---- Defaults (first cut, NOT bench-validated - see moving_forward_notes.md
 * 2026-09-21 entries) ----
 *   ENV_SOFTLIMIT_KNEE_HI = 0.85 - matches ENV_ALC_TARGET_LEVEL's spirit
 *     (envelope_alc.h) of leaving headroom before engaging; chosen so the
 *     two stages' engagement regions overlap somewhat (ALC should usually
 *     have already ducked a sustained peak below 0.85-0.90 by the time it
 *     reaches here) rather than leaving a gap where neither stage acts but
 *     the signal is still close to the rail.
 *   ENV_SOFTLIMIT_LOWER_SPAN = 0.15 - NOT a knee position (fixed by this
 *     bugfix at the actual rail, envelope=0.0) - this is the plateau width
 *     the lower-rail curve saturates toward for large negative excursions.
 *     Picked to match the upper knee's own headroom-from-rail (also 0.15)
 *     purely for numeric symmetry/memorability, not from any shared
 *     derivation - the two constants play genuinely different roles now.
 * Both picked by inspection (the same "modest, conservative first cut"
 * convention envelope_ampeq.h's shelf gains and envelope_alc.h's target
 * level used), not fitted against measured overshoot data.
 *
 * ---- Status ----
 * OFF by default, independently toggleable via 'S' (serial_commands.cpp) -
 * independent of envelope_alc.h's 'l' toggle, per the user's request to
 * switch ALC and Soft-Limit on/off separately. NOT YET BENCH-VALIDATED -
 * the lower-rail shape above is a reasoned fix for a real, confirmed bug,
 * not yet confirmed correct/beneficial on real hardware.
 */

#include <stdbool.h>
#include "esp_attr.h"    // IRAM_ATTR - see envelope_alc.h's matching include for why this is
                          // needed directly here (2026-09-21 build fix)

#define ENV_SOFTLIMIT_KNEE_HI       0.85f
#define ENV_SOFTLIMIT_LOWER_SPAN    0.15f

// No init needed (stateless) - provided anyway, currently a no-op, purely
// so call sites/future state additions have a consistent
// envelope_<module>_init() shape to call from setup() alongside every
// other envelope stage's init, without needing to remember which ones
// need it and which don't.
void envelope_softlimit_init(void);

// Runs `envelope` through the soft-knee curve described above and returns
// the result; returns `envelope` completely unchanged while disabled (not
// even re-clamped) - see ssb_mic_test.ino's dsp_task for the exact call
// site, positioned after envelope_alc_process() and before
// envelope_predistort_process()/the plain DC mapping.
float IRAM_ATTR envelope_softlimit_process(float envelope);

bool envelope_softlimit_get_enabled(void);
void envelope_softlimit_set_enabled(bool enable);
