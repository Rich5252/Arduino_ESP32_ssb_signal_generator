/**
 * envelope_floor.cpp - see envelope_floor.h.
 *
 * NOTE 1: this originally also froze freq_dev_hz at its last above-floor
 * value while below the floor, on the theory that atan2() near a null is
 * untrustworthy noise. Real-hardware testing showed that was wrong for a
 * two-tone signal: the fast phase rotation right at a destructive-
 * interference null is genuine required signal content (envelope can
 * never go negative, so a true zero-crossing can only be represented by
 * phase sweeping through a large angle quickly there), not noise to
 * suppress. Freezing the RATE let accumulated phase silently drift away
 * from truth for the whole hold, then snap back as a hard one-sample
 * discontinuity the instant the hold released - invisible at small floor
 * values (barely engaged, tiny debt), catastrophic once the floor got
 * large enough to cover most of the fast-rotation region (large debt,
 * products jumping to 0dB right at that threshold). Removed.
 *
 * NOTE 2: the envelope side was then a hard clamp (raw_envelope < floor
 * ? floor : raw_envelope) - a min()-style operation. Real-hardware
 * testing showed nearly every product rising gently with each floor
 * increment, which is the signature of a NEW problem this introduced
 * rather than the intended effect: a hard clamp leaves a genuine
 * discontinuity in the envelope's DERIVATIVE at every crossing point (the
 * value is continuous, the slope isn't), and a two-tone envelope crosses
 * a small floor very often - near every null. That's a broadband
 * distortion source in its own right, smaller than the phase-freeze bug
 * above but the same species of problem, and it would have confounded
 * any real read on whether avoiding the LUT's steep region actually
 * helps. Replaced with a smooth affine remap: the WHOLE [0,1] envelope
 * range is continuously compressed into [floor,1] (envelope=1 still maps
 * to 1, only the bottom is raised), so there is no threshold and no kink
 * anywhere in the trajectory - the only remaining effect is the one
 * actually being tested, less of the swing reaching the LUT's steepest,
 * least-characterized region.
 */

#include "envelope_floor.h"

// 2026-09-25: 0.02 -> 0.001, at the user's explicit request. 0.02 (in dB
// terms, 20*log10(0.02) = -34dBc - the achievable null depth once floor
// engages at all, since floor+= (1-floor)*0 = floor is the minimum
// output this remap can ever produce) was the ONLY non-zero value
// reachable with one 'x' press - a huge first step given this project's
// own predistort measurements put true-zero null depth at ~-58dBc (see
// envelope_predistort.h). 0.001 (-60dB) is comparably fine-grained to
// that number - closer to "nudge the floor up a hair" than "give up 24dB
// of null depth in one keypress." Also deliberately close to one raw PWM
// LSB at RSET_MOD_LEDC_RES=10 bits (1/1023 = 0.000978, see
// envelope_output.h) - a step finer than the output hardware can actually
// resolve would just add keypresses without a measurable effect, so this
// is close to the natural bottom of usefully fine, not an arbitrary
// round number.
#define ENV_FLOOR_STEP 0.001f
#define ENV_FLOOR_MAX  0.50f

static volatile float s_envelope_floor = 0.0f;

float IRAM_ATTR envelope_floor_apply(float raw_envelope)
{
    float floor = s_envelope_floor;
    // Smooth, continuous compression of the whole range into [floor,1] -
    // see NOTE 2 above for why this replaced a hard clamp. No branch, no
    // threshold, so no new corner/kink is introduced anywhere.
    return floor + (1.0f - floor) * raw_envelope;
}

float envelope_floor_get(void)
{
    return s_envelope_floor;
}

void envelope_floor_set(float floor)
{
    if (floor < 0.0f) floor = 0.0f;
    if (floor > ENV_FLOOR_MAX) floor = ENV_FLOOR_MAX;
    s_envelope_floor = floor;
}

void envelope_floor_raise(void)
{
    envelope_floor_set(s_envelope_floor + ENV_FLOOR_STEP);
}

void envelope_floor_lower(void)
{
    envelope_floor_set(s_envelope_floor - ENV_FLOOR_STEP);
}
