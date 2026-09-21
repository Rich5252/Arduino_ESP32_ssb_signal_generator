/**
 * envelope_softlimit.cpp - see envelope_softlimit.h.
 */

#include "envelope_softlimit.h"
#include <math.h>   // tanhf(), coshf() - see envelope_softlimit.h's 2026-09-21 bugfix
                    // entry for why the lower rail uses 1/coshf() (sech) rather than
                    // tanhf() the way the upper rail does

static volatile bool s_env_softlimit_enable = false;

void envelope_softlimit_init(void)
{
    // Stateless - see envelope_softlimit.h. Nothing to do.
}

float IRAM_ATTR envelope_softlimit_process(float envelope)
{
    if (!s_env_softlimit_enable) {
        return envelope;
    }

    if (envelope > ENV_SOFTLIMIT_KNEE_HI) {
        float span = 1.0f - ENV_SOFTLIMIT_KNEE_HI;
        envelope = ENV_SOFTLIMIT_KNEE_HI + span * tanhf((envelope - ENV_SOFTLIMIT_KNEE_HI) / span);
    } else if (envelope < 0.0f) {
        // 2026-09-21 bugfix - see envelope_softlimit.h's matching entry for
        // the full derivation. The lower rail is anchored at the actual
        // rail (0.0), not a positive "knee" - envelope=0.0 gives y=0.0
        // exactly (sech(0)=1, so 1-sech(0)=0), and the curve has ZERO
        // slope there (not unity), so small negative excursions map to a
        // much smaller output than |envelope| would - deliberately less
        // aggressive than a straight tanh mirror, which was confirmed to
        // inject a constant ~0.036 DC floor at true envelope nulls.
        float u = envelope / ENV_SOFTLIMIT_LOWER_SPAN;   // negative, since envelope < 0
        envelope = ENV_SOFTLIMIT_LOWER_SPAN * (1.0f - 1.0f / coshf(u));
    }

    return envelope;
}

bool envelope_softlimit_get_enabled(void)
{
    return s_env_softlimit_enable;
}

void envelope_softlimit_set_enabled(bool enable)
{
    // Stateless - see envelope_softlimit.h - no reset-on-transition
    // concern, a plain store is correct.
    s_env_softlimit_enable = enable;
}
