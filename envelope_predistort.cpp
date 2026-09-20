/**
 * envelope_predistort.cpp - see envelope_predistort.h.
 */

#include "envelope_predistort.h"

// Desired linear envelope [0,1] -> commanded PWM duty [0,1], 65 points
// (64 equal bins). REVISION 7 - see envelope_predistort.h for the full
// derivation, including the duty-for-duty comparison against REVISION 6
// that isolated a real (non-uniform) ~1.6dB shift concentrated in the
// turn-on knee, plausibly tied to the 5V PSU rework this revision was
// built to check. Built the same exhaustive-sweep way as REVISIONS 4-6
// (direct duty sweep, dBm read at every count, isotonic-regression-
// smoothed then PCHIP-inverted), but the first sweep to include duty=0
// itself (0-1023, 1024 points) rather than starting at duty=1, and
// without REVISION 6's repeated duty=1023 anchor reading. LUT[0]=0.0000
// (duty=0, the true hardware floor - not a "lowest tied duty" stand-in
// like every prior revision needed). LUT[64]=1.0000 (duty=1023) anchors
// to REVISION 6's ceiling by construction. Through the climb, commanded
// duty runs a few PWM counts above REVISION 6's at the same table index
// (see envelope_predistort.h for the full duty-for-duty dBm comparison
// that separates this from a simple calibration offset).
static const float ENV_PREDISTORT_LUT[65] = {
    0.0000f, 0.1828f, 0.2104f, 0.2302f, 0.2460f, 0.2614f, 0.2742f, 0.2874f,
    0.2995f, 0.3110f, 0.3225f, 0.3329f, 0.3440f, 0.3550f, 0.3646f, 0.3749f,
    0.3846f, 0.3943f, 0.4035f, 0.4141f, 0.4232f, 0.4330f, 0.4433f, 0.4530f,
    0.4618f, 0.4724f, 0.4810f, 0.4903f, 0.4995f, 0.5089f, 0.5185f, 0.5275f,
    0.5375f, 0.5468f, 0.5554f, 0.5659f, 0.5746f, 0.5838f, 0.5930f, 0.6026f,
    0.6119f, 0.6211f, 0.6298f, 0.6392f, 0.6485f, 0.6584f, 0.6679f, 0.6771f,
    0.6871f, 0.6969f, 0.7066f, 0.7157f, 0.7268f, 0.7369f, 0.7467f, 0.7580f,
    0.7691f, 0.7801f, 0.7926f, 0.8049f, 0.8203f, 0.8372f, 0.8610f, 0.8988f,
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
