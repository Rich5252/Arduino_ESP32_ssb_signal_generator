/**
 * relative_delay.cpp - see relative_delay.h.
 */

#include "relative_delay.h"

#if AD9851_ATTACHED

static float s_freq_dev_ring[PHASE_DELAY_MAX_SAMPLES] = {0};
static float s_envelope_ring[PHASE_DELAY_MAX_SAMPLES] = {0};
static uint32_t s_delay_ring_idx = 0;   // shared index - both rings always written/read together
static volatile float s_relative_delay_samples = 0.0f;   // fractional - see relative_delay.h

// Linear interpolation between adjacent ring entries. 'back' is how many
// samples behind base_idx to read (0 = the just-written current sample,
// fractional values interpolate between the two nearest whole-sample
// entries). Caller is responsible for keeping 'back' within
// PHASE_DELAY_MAX_SAMPLES-2 so idx1 never wraps into not-yet-written data.
static inline float IRAM_ATTR interp_ring(const float *ring, uint32_t base_idx, float back)
{
    int32_t i0 = (int32_t)back;   // floor - back is always >= 0 by construction at call sites
    float frac = back - (float)i0;
    uint32_t idx0 = (base_idx + PHASE_DELAY_MAX_SAMPLES - (uint32_t)i0) % PHASE_DELAY_MAX_SAMPLES;
    uint32_t idx1 = (base_idx + PHASE_DELAY_MAX_SAMPLES - (uint32_t)i0 - 1) % PHASE_DELAY_MAX_SAMPLES;
    return ring[idx0] * (1.0f - frac) + ring[idx1] * frac;
}

void IRAM_ATTR relative_delay_apply(float freq_dev_hz, float envelope,
                                     float *out_delayed_freq_dev_hz, float *out_delayed_envelope)
{
    s_freq_dev_ring[s_delay_ring_idx] = freq_dev_hz;
    s_envelope_ring[s_delay_ring_idx] = envelope;

    float delay = s_relative_delay_samples;
    if (delay >= (float)(PHASE_DELAY_MAX_SAMPLES - 2))  delay = (float)(PHASE_DELAY_MAX_SAMPLES - 2);
    if (delay <= -(float)(PHASE_DELAY_MAX_SAMPLES - 2)) delay = -(float)(PHASE_DELAY_MAX_SAMPLES - 2);
    float freq_back = (delay > 0.0f) ? delay : 0.0f;    // positive: hold phase back
    float env_back  = (delay < 0.0f) ? -delay : 0.0f;   // negative: hold envelope back

    *out_delayed_freq_dev_hz = interp_ring(s_freq_dev_ring, s_delay_ring_idx, freq_back);
    *out_delayed_envelope    = interp_ring(s_envelope_ring, s_delay_ring_idx, env_back);

    s_delay_ring_idx = (s_delay_ring_idx + 1) % PHASE_DELAY_MAX_SAMPLES;
}

float relative_delay_get_samples(void)
{
    return s_relative_delay_samples;
}

void relative_delay_increase(void)
{
    if (s_relative_delay_samples < (float)(PHASE_DELAY_MAX_SAMPLES - 2))
        s_relative_delay_samples += DELAY_STEP_SAMPLES;
}

void relative_delay_decrease(void)
{
    if (s_relative_delay_samples > -(float)(PHASE_DELAY_MAX_SAMPLES - 2))
        s_relative_delay_samples -= DELAY_STEP_SAMPLES;
}

void relative_delay_set_samples(float samples)
{
    if (samples > (float)(PHASE_DELAY_MAX_SAMPLES - 2))  samples = (float)(PHASE_DELAY_MAX_SAMPLES - 2);
    if (samples < -(float)(PHASE_DELAY_MAX_SAMPLES - 2)) samples = -(float)(PHASE_DELAY_MAX_SAMPLES - 2);
    s_relative_delay_samples = samples;
}

#endif // AD9851_ATTACHED
