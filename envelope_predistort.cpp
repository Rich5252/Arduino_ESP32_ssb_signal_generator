/**
 * envelope_predistort.cpp - see envelope_predistort.h.
 */

#include "envelope_predistort.h"

// Desired linear envelope [0,1] -> commanded PWM duty [0,1], 65 points
// (64 equal bins), rebuilt from a much denser real hardware sweep than the
// original 33-point table - see envelope_predistort.h for the full
// derivation. LUT[0] is NOT 0.0 - it's 0.3129, the duty below which the
// path is dead (near the analyzer noise floor regardless of command), so
// "envelope=0" maps to the lowest achievable duty rather than one this
// hardware can't usefully go lower than anyway. LUT[64] is 0.9495, NOT
// 1.0 - the denser sweep revealed RF output actually saturates (plateaus
// at its max measured level) by ~95% duty, so commanding the full 100%
// beyond that buys nothing; the inversion correctly picks the LOWEST duty
// that already reaches full output rather than always maxing out.
static const float ENV_PREDISTORT_LUT[65] = {
    0.3129f, 0.4164f, 0.4396f, 0.4532f, 0.4644f, 0.4735f, 0.4815f, 0.4901f,
    0.4987f, 0.5081f, 0.5172f, 0.5262f, 0.5344f, 0.5414f, 0.5480f, 0.5548f,
    0.5625f, 0.5705f, 0.5779f, 0.5846f, 0.5910f, 0.5973f, 0.6037f, 0.6100f,
    0.6163f, 0.6227f, 0.6291f, 0.6352f, 0.6415f, 0.6483f, 0.6561f, 0.6669f,
    0.6789f, 0.6885f, 0.6957f, 0.7018f, 0.7074f, 0.7130f, 0.7190f, 0.7251f,
    0.7312f, 0.7373f, 0.7435f, 0.7499f, 0.7559f, 0.7618f, 0.7681f, 0.7753f,
    0.7841f, 0.7980f, 0.8052f, 0.8117f, 0.8181f, 0.8243f, 0.8319f, 0.8427f,
    0.8491f, 0.8560f, 0.8695f, 0.8779f, 0.8848f, 0.8952f, 0.9039f, 0.9217f,
    0.9495f,
};
#define ENV_PREDISTORT_LUT_LAST_IDX 64   // ENV_PREDISTORT_LUT's last valid index

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
