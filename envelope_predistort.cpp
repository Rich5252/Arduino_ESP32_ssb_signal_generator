/**
 * envelope_predistort.cpp - see envelope_predistort.h.
 */

#include "envelope_predistort.h"

// Desired linear envelope [0,1] -> commanded PWM duty [0,1], 65 points
// (64 equal bins). REVISION 5 - see envelope_predistort.h for the full
// derivation and how this compares to REVISION 4. Built the same way as
// REVISION 4 (direct duty sweep 1-1023 via 'd'/'>'/'<'/'N'/'B', dBm read
// at every count, isotonic-regression-smoothed then PCHIP-inverted) but
// against the REBUILT RSET/PWM filter hardware (fixed-bias PNP stage,
// see project history) rather than the original filter REVISION 4
// characterized. LUT[0]=0.0010 (duty=1) same convention as REVISION 4 -
// lowest duty in the pooled floor block, which this time is only
// duty 1-9 (vs REVISION 4's duty~200-226) - the bias-starvation fix
// visibly shrank the dead zone by roughly 20x. LUT[64]=1.0000 (duty=1023,
// the measured maximum) also matches REVISION 4's convention. The new
// filter's OWN weak spot is the opposite end: roughly the last third of
// the whole duty range (duty ~653-1023) reads within 0.3dB of full
// saturation, which collapses into this table's single last bin far more
// severely than REVISION 4's top-plateau ever did - see envelope_predistort.h.
static const float ENV_PREDISTORT_LUT[65] = {
    0.0010f, 0.0652f, 0.0863f, 0.1008f, 0.1146f, 0.1245f, 0.1353f, 0.1448f,
    0.1533f, 0.1636f, 0.1723f, 0.1801f, 0.1884f, 0.1934f, 0.2044f, 0.2117f,
    0.2195f, 0.2308f, 0.2378f, 0.2454f, 0.2531f, 0.2586f, 0.2660f, 0.2705f,
    0.2812f, 0.2886f, 0.2922f, 0.3024f, 0.3097f, 0.3133f, 0.3230f, 0.3302f,
    0.3443f, 0.3518f, 0.3587f, 0.3662f, 0.3735f, 0.3812f, 0.3887f, 0.3962f,
    0.4036f, 0.4061f, 0.4139f, 0.4214f, 0.4288f, 0.4363f, 0.4443f, 0.4516f,
    0.4590f, 0.4666f, 0.4741f, 0.4812f, 0.4885f, 0.4964f, 0.5046f, 0.5128f,
    0.5215f, 0.5307f, 0.5323f, 0.5476f, 0.5599f, 0.5747f, 0.5967f, 0.6387f,
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
