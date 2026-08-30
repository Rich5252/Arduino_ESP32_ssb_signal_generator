/**
 * envelope_predistort.cpp - see envelope_predistort.h.
 */

#include "envelope_predistort.h"

// Desired linear envelope [0,1] -> commanded PWM duty [0,1], 65 points
// (64 equal bins). REVISION 6 - see envelope_predistort.h for the full
// derivation and how this compares to REVISION 5. Built the same way as
// REVISIONS 4-5 (direct duty sweep 1-1023 via 'd'/'>'/'<'/'N'/'B', dBm
// read at every count including a tight 3-reading repeatability check at
// duty=1023, isotonic-regression-smoothed then PCHIP-inverted). This
// sweep's floor and ceiling both moved versus REVISION 5 from two
// confirmed, stacked causes: 10dB less RX attenuation (30dB pad vs.
// REVISION 5's 40dB), plus a deliberate RSET/PWM gate-drive redesign
// (MOSFET gate now 0.94V at duty=0 up to 3.38V at duty=1023, engineered
// to not compromise BJT bias or filter/group-delay stability) that
// genuinely spreads real output across more of the duty range - see
// envelope_predistort.h for the attenuation-corrected duty-matched
// comparison that separates the two effects. LUT[0]=0.0010 (duty=1), same
// convention as every prior revision - the strictly-pooled floor block is
// now just duty=1 alone, but the climb off it is far more gradual in dBm
// terms than REVISION 5's (a real gain-reduction effect, not just a
// pooling artifact), so normalized output doesn't reach 1% of full swing
// until duty~166 (REVISION 5: duty~56) - a genuinely longer practical
// dead zone. LUT[64]=1.0000 (duty=1023, the measured maximum, mean of 3
// repeats within 0.002dB of each other) also matches prior convention.
// This revision's top end is genuinely better resolved than REVISION 5's,
// confirmed real by the attenuation-corrected comparison: only duty
// 870-1023 (~15% of the range) sits within 0.3dB of saturation, versus
// REVISION 5's duty 653-1023 (~36%), so the final table bin
// (LUT[63]=0.8957/duty~916 to LUT[64]=1.0000/duty=1023, a 107-count span)
// compresses far less than REVISION 5's 370-count final bin - see
// envelope_predistort.h.
static const float ENV_PREDISTORT_LUT[65] = {
    0.0010f, 0.1766f, 0.2021f, 0.2229f, 0.2390f, 0.2555f, 0.2684f, 0.2807f,
    0.2908f, 0.3066f, 0.3170f, 0.3282f, 0.3372f, 0.3475f, 0.3580f, 0.3676f,
    0.3758f, 0.3865f, 0.4030f, 0.4130f, 0.4225f, 0.4315f, 0.4412f, 0.4454f,
    0.4575f, 0.4669f, 0.4769f, 0.4866f, 0.4954f, 0.5039f, 0.5121f, 0.5215f,
    0.5283f, 0.5385f, 0.5479f, 0.5572f, 0.5665f, 0.5758f, 0.5851f, 0.5945f,
    0.6038f, 0.6133f, 0.6226f, 0.6321f, 0.6410f, 0.6506f, 0.6604f, 0.6698f,
    0.6794f, 0.6892f, 0.6995f, 0.7092f, 0.7192f, 0.7293f, 0.7396f, 0.7500f,
    0.7614f, 0.7724f, 0.7843f, 0.7976f, 0.8124f, 0.8310f, 0.8550f, 0.8957f,
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
