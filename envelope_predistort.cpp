/**
 * envelope_predistort.cpp - see envelope_predistort.h.
 */

#include "envelope_predistort.h"

// Desired linear envelope [0,1] -> commanded PWM duty [0,1], 65 points
// (64 equal bins), rebuilt from a much denser real hardware sweep than the
// original 33-point table - see envelope_predistort.h for the full
// derivation. LUT[0] is NOT 0.0 - it's 0.3008, the duty below which the
// path is dead (near the analyzer noise floor regardless of command), so
// "envelope=0" maps to the lowest achievable duty rather than one this
// hardware can't usefully go lower than anyway. LUT[64] is 0.9882, NOT
// 1.0 - REVISION 3's much denser top-end sweep revealed RF output doesn't
// actually finish saturating until duty is almost literal 100% (REVISION
// 2's 0.9495 undershot this - see envelope_predistort.h's REVISION 3
// notes - meaning that table was leaving real output on the table at
// full-envelope commands); the inversion correctly picks the LOWEST duty
// that already reaches full output rather than always maxing out.
static const float ENV_PREDISTORT_LUT[65] = {
    0.3008f, 0.4196f, 0.4423f, 0.4542f, 0.4678f, 0.4758f, 0.4825f, 0.4904f,
    0.5054f, 0.5134f, 0.5230f, 0.5294f, 0.5388f, 0.5471f, 0.5497f, 0.5621f,
    0.5724f, 0.5775f, 0.5864f, 0.5885f, 0.5988f, 0.6035f, 0.6122f, 0.6201f,
    0.6256f, 0.6345f, 0.6418f, 0.6493f, 0.6556f, 0.6644f, 0.6720f, 0.6791f,
    0.6856f, 0.6918f, 0.6973f, 0.7022f, 0.7072f, 0.7119f, 0.7170f, 0.7221f,
    0.7274f, 0.7332f, 0.7396f, 0.7468f, 0.7548f, 0.7640f, 0.7727f, 0.7819f,
    0.7908f, 0.7997f, 0.8088f, 0.8167f, 0.8248f, 0.8336f, 0.8424f, 0.8512f,
    0.8599f, 0.8689f, 0.8780f, 0.8867f, 0.8958f, 0.9035f, 0.9130f, 0.9277f,
    0.9882f,
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
