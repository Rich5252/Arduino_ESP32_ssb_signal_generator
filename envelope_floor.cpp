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

#define ENV_FLOOR_STEP 0.02f
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
