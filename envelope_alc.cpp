/**
 * envelope_alc.cpp - see envelope_alc.h.
 */

#include "envelope_alc.h"
#include <math.h>   // expf(), fabsf(), isfinite()

static volatile bool  s_env_alc_enable = false;
static float          s_alc_gain = 1.0f;
static float          s_alc_attack_coeff = 0.0f;
static float          s_alc_release_coeff = 0.0f;

// Small envelope magnitude below which we don't even attempt g_desired =
// target/fabsf(envelope) - avoids a huge (or, at exactly 0.0f, an infinite)
// desired gain from a near-null sample chasing s_alc_gain toward
// ENV_ALC_MAX_GAIN's own ceiling every time the envelope passes through
// zero (routine for an SSB envelope, not a rare edge case). Below this
// threshold there's effectively nothing to level anyway, so g_desired is
// just pinned at ENV_ALC_MAX_GAIN (the same "no need to boost" case a
// small-but-nonzero below-target envelope already takes).
#define ENV_ALC_NULL_EPSILON   1.0e-6f

void envelope_alc_init(void)
{
    // Standard one-pole coeff = 1 - exp(-1/(tau_seconds * SAMPLE_RATE_HZ)) -
    // see envelope_alc.h's header comment for why this is Fs-independent
    // (computed from real time constants) unlike envelope_ampeq.h/
    // envelope_gdeq.h's Fs-and-filter-variant-specific coefficients.
    s_alc_attack_coeff  = 1.0f - expf(-1.0f / ((ENV_ALC_ATTACK_MS  * 0.001f) * (float)SAMPLE_RATE_HZ));
    s_alc_release_coeff = 1.0f - expf(-1.0f / ((ENV_ALC_RELEASE_MS * 0.001f) * (float)SAMPLE_RATE_HZ));
    s_alc_gain = 1.0f;
}

float IRAM_ATTR envelope_alc_process(float envelope)
{
    if (!s_env_alc_enable) {
        return envelope;
    }

    float abs_env = fabsf(envelope);
    float g_desired;
    if (abs_env <= ENV_ALC_NULL_EPSILON) {
        g_desired = ENV_ALC_MAX_GAIN;
    } else {
        g_desired = ENV_ALC_TARGET_LEVEL / abs_env;
        if (g_desired > ENV_ALC_MAX_GAIN) {
            g_desired = ENV_ALC_MAX_GAIN;
        }
    }

    // Asymmetric smoothing: fast attack when ducking (g_desired below the
    // current gain), slow release when recovering (g_desired above it) -
    // see envelope_alc.h's "Algorithm" section.
    if (g_desired < s_alc_gain) {
        s_alc_gain += (g_desired - s_alc_gain) * s_alc_attack_coeff;
    } else {
        s_alc_gain += (g_desired - s_alc_gain) * s_alc_release_coeff;
    }

    if (s_alc_gain < ENV_ALC_MIN_GAIN) s_alc_gain = ENV_ALC_MIN_GAIN;
    if (s_alc_gain > ENV_ALC_MAX_GAIN) s_alc_gain = ENV_ALC_MAX_GAIN;

    return envelope * s_alc_gain;
}

bool envelope_alc_get_enabled(void)
{
    return s_env_alc_enable;
}

void envelope_alc_set_enabled(bool enable)
{
    bool was_on = s_env_alc_enable;
    s_env_alc_enable = enable;
    if (enable && !was_on) {
        s_alc_gain = 1.0f;   // see envelope_alc.h - don't inherit a stale gain
    }
}

float envelope_alc_get_gain(void)
{
    return s_alc_gain;
}

bool envelope_alc_get_canary(void)
{
    return isfinite(s_alc_gain);
}
