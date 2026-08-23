/**
 * envelope_predistort.cpp - see envelope_predistort.h.
 */

#include "envelope_predistort.h"

// Desired linear envelope [0,1] -> commanded PWM duty [0,1], 65 points
// (64 equal bins). REVISION 4 - see envelope_predistort.h for the full
// derivation. Built directly from the 'd'/'>'/'<'/'N'/'B' direct duty
// override commands (envelope_output.h) sweeping every single raw LEDC
// count 1-1023 against a measured dBm reading - no gate-voltage inference
// anywhere in this table, unlike REVISIONS 1-3. LUT[0] is 0.001 (duty=1,
// essentially fully off) - REVISION 4's exhaustive sweep found the whole
// path reads as noise floor from duty=1 clear through duty~200-226, so
// "envelope=0" is mapped to the LOWEST duty in that dead range rather
// than holding the gate open any further than necessary (REVISIONS 1-3
// assumed the dead zone ran to duty~0.30-0.32 instead - see
// envelope_predistort.h's REVISION 4 notes for why that turned out to be
// wrong by a wide margin, not just at this floor point but through most
// of the table). LUT[64] is a clean 1.0 - this sweep's own top point
// (duty=1023, literal 100%) IS the measured maximum, so unlike REVISIONS
// 2-3 there's no inference needed to find where output actually finishes
// saturating.
static const float ENV_PREDISTORT_LUT[65] = {
    0.0010f, 0.2998f, 0.3267f, 0.3442f, 0.3606f, 0.3727f, 0.3848f, 0.3955f,
    0.4088f, 0.4187f, 0.4273f, 0.4372f, 0.4463f, 0.4540f, 0.4639f, 0.4777f,
    0.4868f, 0.4949f, 0.5038f, 0.5103f, 0.5189f, 0.5277f, 0.5361f, 0.5438f,
    0.5522f, 0.5606f, 0.5664f, 0.5758f, 0.5841f, 0.5923f, 0.6005f, 0.6087f,
    0.6169f, 0.6250f, 0.6332f, 0.6413f, 0.6494f, 0.6575f, 0.6656f, 0.6738f,
    0.6819f, 0.6901f, 0.6983f, 0.7065f, 0.7148f, 0.7230f, 0.7313f, 0.7395f,
    0.7479f, 0.7563f, 0.7648f, 0.7734f, 0.7820f, 0.7907f, 0.7997f, 0.8089f,
    0.8182f, 0.8281f, 0.8382f, 0.8494f, 0.8617f, 0.8765f, 0.8956f, 0.9264f,
    1.0000f,
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
