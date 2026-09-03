/**
 * envelope_ampeq.cpp - see envelope_ampeq.h.
 */

#include "envelope_ampeq.h"

static ssb_shelf_biquad_t s_env_ampeq_shelf;
static volatile bool s_env_ampeq_enable = false;

void envelope_ampeq_init(void)
{
    ssb_shelf_biquad_set_highshelf(&s_env_ampeq_shelf, ENV_AMPEQ_SHELF_FREQ_HZ,
                                    (float)SAMPLE_RATE_HZ, ENV_AMPEQ_SHELF_GAIN_DB);
}

float IRAM_ATTR envelope_ampeq_process(float envelope)
{
    if (s_env_ampeq_enable) {
        envelope = ssb_shelf_biquad_process(&s_env_ampeq_shelf, envelope);
    }
    return envelope;
}

bool envelope_ampeq_get_enabled(void)
{
    return s_env_ampeq_enable;
}

void envelope_ampeq_set_enabled(bool enable)
{
    bool was_on = s_env_ampeq_enable;
    s_env_ampeq_enable = enable;
    if (enable && !was_on) {
        ssb_shelf_biquad_reset(&s_env_ampeq_shelf);
    }
}
