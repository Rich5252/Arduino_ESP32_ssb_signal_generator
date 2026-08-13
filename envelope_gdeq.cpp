/**
 * envelope_gdeq.cpp - see envelope_gdeq.h.
 */

#include "envelope_gdeq.h"

static ssb_allpass1_t s_env_gdeq_1;
static ssb_allpass1_t s_env_gdeq_2;
static volatile bool s_env_gdeq_enable = false;

void envelope_gdeq_init(void)
{
    ssb_allpass1_init(&s_env_gdeq_1, ENV_GDEQ_A1);
    ssb_allpass1_init(&s_env_gdeq_2, ENV_GDEQ_A2);
}

float IRAM_ATTR envelope_gdeq_process(float envelope)
{
    if (s_env_gdeq_enable) {
        envelope = ssb_allpass1_process(&s_env_gdeq_1, envelope);
        envelope = ssb_allpass1_process(&s_env_gdeq_2, envelope);
    }
    return envelope;
}

bool envelope_gdeq_get_enabled(void)
{
    return s_env_gdeq_enable;
}

void envelope_gdeq_set_enabled(bool enable)
{
    bool was_on = s_env_gdeq_enable;
    s_env_gdeq_enable = enable;
    if (enable && !was_on) {
        ssb_allpass1_reset(&s_env_gdeq_1);
        ssb_allpass1_reset(&s_env_gdeq_2);
    }
}
