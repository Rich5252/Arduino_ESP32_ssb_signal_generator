#pragma once

/**
 * envelope_alc.h
 *
 * ---- Automatic Level Control (envelope-path leveler) ----
 * 2026-09-21: first cut, requested by the user alongside envelope_softlimit.h
 * as a graceful alternative to relying on the final hard clamp
 * (ssb_mic_test.ino's "envelope < 0 / envelope > 1" pair) to catch
 * overshoot. Runs a simple feed-forward gain-riding leveler on the
 * envelope, AFTER envelope_gdeq_process()/envelope_ampeq_process() (so it
 * sees the real post-shelf overshoot those stages can introduce - shelf2
 * in particular is NOT unity-magnitude by construction) and BEFORE
 * envelope_predistort_process() - per the user's explicit, final placement
 * instruction: "needs to go before Distortion correction since that is
 * needed 'always'" (predistort's own [0,1] input clamp - see
 * envelope_predistort.cpp - must keep seeing a controlled signal on every
 * tick, on or off; this stage runs before it and before that clamp, not
 * instead of it).
 *
 * ---- Why gain-riding rather than a real "boost quiet audio" ALC ----
 * A classic ham-radio "ALC" (Automatic Level Control) can both attenuate
 * loud peaks AND boost quiet passages back up toward a target level. This
 * first cut deliberately does ONLY the attenuating half (ENV_ALC_MAX_GAIN
 * is unity - see below): boosting a quiet envelope here would increase
 * modulation depth beyond what the actual audio contains, which for a
 * polar TX envelope path means synthesizing MORE RF deviation than the
 * source signal ever asked for - a very different (and much riskier)
 * proposition than a receive-audio ALC boosting perceived loudness. So
 * this stage only ever pulls peaks back down toward ENV_ALC_TARGET_LEVEL,
 * and gain recovers back toward 1.0 (never above) once the peak has
 * passed. If bench testing shows genuine benefit from also lifting quiet
 * passages, that would be a deliberate second change to ENV_ALC_MAX_GAIN,
 * not silently rolled into this first cut.
 *
 * ---- Algorithm ----
 * Each tick: measure the instantaneous peak-detector gain that WOULD hold
 * fabsf(envelope) exactly at ENV_ALC_TARGET_LEVEL (g_desired =
 * target/fabsf(envelope), or ENV_ALC_MAX_GAIN when envelope is already
 * below target - no need to boost), then smooth the actual applied gain
 * toward g_desired with an asymmetric one-pole filter: fast ATTACK when
 * g_desired is LOWER than the current gain (need to duck quickly, before
 * the peak reaches predistort/the final clamp) and slow RELEASE when
 * g_desired is HIGHER (recovering after the peak has passed - slow on
 * purpose, to avoid audible/RF "pumping" from chasing every fast
 * transient back up). Both coefficients are computed from real time
 * constants (ENV_ALC_ATTACK_MS/ENV_ALC_RELEASE_MS below) via the standard
 * one-pole coeff = 1 - exp(-1/(tau_seconds * SAMPLE_RATE_HZ)) mapping, at
 * envelope_alc_init() time - Fs-independent by construction, unlike the
 * ampeq shelves or gdeq's coefficients, so no #if SAMPLE_RATE_HZ guard is
 * needed here the way envelope_ampeq.h/envelope_gdeq.h need one.
 *
 * Deliberately just ONE state variable (the smoothed gain, s_alc_gain) -
 * no separate peak-detector state - since g_desired is already a
 * memoryless function of the current sample; the one-pole smoothing of
 * the GAIN itself is what gives the attack/release behavior, the same
 * structural simplification a "backwards" (gain-computer-then-smooth)
 * compressor/leveler topology uses instead of smoothing the level first.
 *
 * ---- Defaults (first cut, NOT bench-validated - see moving_forward_notes.md
 * 2026-09-21 entry) ----
 *   ENV_ALC_TARGET_LEVEL = 0.90 - leaves 10% headroom below the [0,1]
 *     envelope ceiling before predistort/the final clamp even engages;
 *     chosen by inspection (matches the kind of "modest, conservative
 *     first cut" convention envelope_ampeq.h's shelf gains used), not
 *     fitted against real overshoot data.
 *   ENV_ALC_ATTACK_MS = 2.0 - fast enough to catch a shelf2-style
 *     overshoot transient before it reaches predistort's own clamp.
 *   ENV_ALC_RELEASE_MS = 200.0 - slow enough that recovery after a single
 *     loud syllable/tone burst doesn't itself become an audible/RF
 *     artifact (classic ALC/leveler release-time territory).
 *   ENV_ALC_MIN_GAIN = 0.20 - floor so a single huge, possibly-spurious
 *     outlier sample can't collapse the gain toward zero and mute
 *     everything that follows until release time has fully elapsed.
 *   ENV_ALC_MAX_GAIN = 1.00 - see "Why gain-riding" above: this stage
 *     never boosts.
 *
 * ---- Status ----
 * OFF by default, independently toggleable via 'l' (serial_commands.cpp) -
 * independent of envelope_softlimit.h's 'S' toggle, per the user's request
 * to be able to switch ALC and Soft-Limit on/off separately. NOT YET
 * BENCH-VALIDATED - implemented from first principles (a standard
 * gain-computer leveler topology) but the specific default numbers above
 * are starting points for on-the-bench tuning, not measured optima, same
 * epistemic status this project gives every other "first cut, refine on
 * real hardware" feature (see envelope_ampeq.h's own history for the
 * pattern this follows).
 */

#include <stdbool.h>
#include "config.h"      // SAMPLE_RATE_HZ
#include "esp_attr.h"    // IRAM_ATTR - envelope_ampeq.h/envelope_gdeq.h get this transitively
                          // via ssb_dsp.h's own #include "esp_attr.h"; included directly here
                          // instead since this module has no other reason to pull in ssb_dsp.h
                          // (2026-09-21 build fix - Arduino IDE's toolchain doesn't define
                          // IRAM_ATTR globally the way it might appear to from the .ino's own
                          // implicit Arduino.h - omitting this made IRAM_ATTR an undeclared
                          // identifier, which the compiler misparsed as a variable declaration,
                          // producing "expected initializer before 'envelope_alc_process'")

#define ENV_ALC_TARGET_LEVEL   0.90f
#define ENV_ALC_ATTACK_MS      2.0f
#define ENV_ALC_RELEASE_MS     200.0f
#define ENV_ALC_MIN_GAIN       0.20f
#define ENV_ALC_MAX_GAIN       1.00f

// Computes the attack/release one-pole coefficients from the constants
// above and SAMPLE_RATE_HZ, and resets gain to unity. Call once from
// setup() - always, regardless of the enabled default, same convention as
// envelope_ampeq_init()/envelope_gdeq_init() (so enabling later only ever
// needs envelope_alc_set_enabled(), not a separate init path).
void envelope_alc_init(void);

// Runs `envelope` through the leveler described above and returns the
// result; returns `envelope` unchanged (gain pinned at 1.0, no state
// advanced) while disabled. Call unconditionally from dsp_task, once per
// tick, positioned after envelope_ampeq_process() and before
// envelope_predistort_process()/the plain DC mapping - see
// ssb_mic_test.ino's dsp_task for the exact call site.
float IRAM_ATTR envelope_alc_process(float envelope);

// Same enable/disable API and reset-on-off->on-transition convention as
// envelope_gdeq_set_enabled()/envelope_ampeq_set_enabled() - resets
// s_alc_gain back to unity on an off->on transition so re-enabling after
// a long time off never inherits a stale, possibly-very-low gain from
// whatever the envelope was doing the last time this ran.
bool envelope_alc_get_enabled(void);
void envelope_alc_set_enabled(bool enable);

// Diagnostic getter - the leveler's own currently-applied gain, exposed
// for diagnostics.cpp's periodic status line (same "expose internal state
// for the bench, don't make the user infer it" convention as
// adc_capture_get_true_ratio()). Meaningful even while disabled (reads
// back 1.0, the pinned value).
float envelope_alc_get_gain(void);

/**
 * @brief NaN/Inf canary for this module's one piece of state (s_alc_gain) -
 *        same reasoning/convention as env_ampeq_canary_t
 *        (envelope_ampeq.h): a bad value in a variable that's fed forward
 *        every tick (unlike envelope_softlimit.h's stateless computation)
 *        can persist indefinitely once introduced, most plausibly here via
 *        a divide against a near-zero fabsf(envelope) - guarded against
 *        directly in envelope_alc_process(), but checked here too as a
 *        second line of defense. true = finite (OK), including while
 *        disabled (gain is pinned at the finite value 1.0f, not being fed
 *        anything).
 */
bool envelope_alc_get_canary(void);
