/**
 * envelope_predistort.cpp - see envelope_predistort.h.
 */

#include "envelope_predistort.h"

// Desired linear envelope [0,1] -> commanded PWM duty [0,1], 33 points
// (32 equal bins), derived offline from the real hardware sweep described
// in envelope_predistort.h via monotonic (PCHIP) interpolation then
// resampled onto this even grid. LUT[0] is NOT 0.0 - it's 0.3190, the
// lowest duty actually measured (~32%, gate ~1.05V) - below that the
// path is dead (near the analyzer noise floor regardless of command), so
// "envelope=0" maps to the lowest achievable duty rather than a duty this
// hardware can't actually produce a meaningfully lower output at anyway.
// LUT[32] = 1.0 (100% duty), the observed saturation point.
static const float ENV_PREDISTORT_LUT[33] = {
    0.3190f, 0.4368f, 0.4637f, 0.4852f, 0.5034f, 0.5180f, 0.5312f, 0.5461f,
    0.5633f, 0.5805f, 0.5951f, 0.6075f, 0.6191f, 0.6313f, 0.6457f, 0.6628f,
    0.6806f, 0.6967f, 0.7101f, 0.7219f, 0.7330f, 0.7440f, 0.7558f, 0.7685f,
    0.7811f, 0.7942f, 0.8084f, 0.8245f, 0.8427f, 0.8625f, 0.8880f, 0.9242f,
    1.0000f,
};
#define ENV_PREDISTORT_LUT_LAST_IDX 32   // ENV_PREDISTORT_LUT's last valid index

static volatile bool s_env_predistort_enable = false;

float IRAM_ATTR envelope_predistort_process(float envelope)
{
    if (!s_env_predistort_enable) {
        return envelope;
    }

    if (envelope < 0.0f) envelope = 0.0f;
    if (envelope > 1.0f) envelope = 1.0f;

    float pos = envelope * (float)ENV_PREDISTORT_LUT_LAST_IDX;   // 0..32
    int i0 = (int)pos;
    if (i0 >= ENV_PREDISTORT_LUT_LAST_IDX) {
        return ENV_PREDISTORT_LUT[ENV_PREDISTORT_LUT_LAST_IDX];   // envelope == 1.0 exactly
    }
    float frac = pos - (float)i0;
    return ENV_PREDISTORT_LUT[i0] + frac * (ENV_PREDISTORT_LUT[i0 + 1] - ENV_PREDISTORT_LUT[i0]);
}

bool envelope_predistort_get_enabled(void)
{
    return s_env_predistort_enable;
}

void envelope_predistort_set_enabled(bool enable)
{
    // No state to reset (unlike envelope_gdeq's off->on filter-memory
    // reset) - this is a pure function of its input, nothing to glitch.
    s_env_predistort_enable = enable;
}
