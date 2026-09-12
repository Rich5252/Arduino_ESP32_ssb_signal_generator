/**
 * relative_delay.cpp - see relative_delay.h.
 */

#include "relative_delay.h"
#include <Arduino.h>   // 2026-09-12, yet later still: millis() - see s_last_delay_change_ms below

#if AD9851_ATTACHED

static float s_freq_dev_ring[PHASE_DELAY_MAX_SAMPLES] = {0};
static float s_envelope_ring[PHASE_DELAY_MAX_SAMPLES] = {0};
static uint32_t s_delay_ring_idx = 0;   // shared index - both rings always written/read together
static volatile float s_relative_delay_samples = 0.0f;   // fractional - see relative_delay.h

// 2026-09-12, yet later still: added after a 'K' slow-trace capture (see
// diagnostics.cpp/null_bias_investigation.md) that the user explicitly
// flagged as "[] scan induced" came back showing a PERFECTLY steady,
// unchanging repeating cycle in both its pre- and post-trigger windows -
// no visible transition anywhere in the printed 50ms+50ms trace, despite
// a real (5Hz+) fast/slow EMA divergence having fired. Leading
// explanation: relative_delay changes are ALREADY well-established
// (this file's whole history) to shift the transmitted frequency, often
// significantly - a brief excursion during a `'['`/`']'` scan could have
// dragged the slow (2s-tau) EMA away from the current value, then fully
// resolved (the signal already back to its current steady cycle) before
// the trace's 50ms pre-window even starts, since the slow EMA can still
// be "remembered" for seconds after a transient has otherwise fully
// passed. Rather than growing the pre/post trace buffers to multi-second
// span (expensive, and still just pushes the same edge case further
// out), this instead timestamps every relative_delay change directly, so
// 'K's output can show "delay last touched Xms before this trigger" -
// letting a human correlate a trigger against their OWN recent
// '['/']'/preset actions without needing the trace itself to visually
// capture the causal moment.
static volatile uint32_t s_last_delay_change_ms = 0;

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

// 2026-09-12: companion to interp_ring() above, added after the first
// diagnostically-useful jump-log capture (relative_delay=+0.90) showed a
// clean repeating 3-state cycle where only 1 of 3 states classified
// near_null against the BLENDED envelope_at_freq_time - see the same-day
// null_bias_investigation.md entry this was found from. At a lopsided
// fractional delay (frac far from 0.5 - 0.90 here means a 10%/90% blend),
// a genuinely near-null RAW sample can sit in the minority-weighted ring
// entry without pulling the BLENDED value below threshold at all, making
// interp_ring() alone under-report how often a near-null sample actually
// contributed to a given delayed freq_dev value. Returns the smaller
// (closer to zero) of the two raw ring entries the same interp_ring() call
// would have blended - i.e. "did EITHER contributing raw sample dip near a
// null", the more permissive/inclusive counterpart to the blended
// envelope's "was the RESULT near a null" question. Same index arithmetic
// as interp_ring() (duplicated rather than having interp_ring() itself
// grow an extra output - keeps that function's existing two call sites,
// which don't need this, untouched).
static inline float IRAM_ATTR interp_ring_min(const float *ring, uint32_t base_idx, float back)
{
    int32_t i0 = (int32_t)back;
    uint32_t idx0 = (base_idx + PHASE_DELAY_MAX_SAMPLES - (uint32_t)i0) % PHASE_DELAY_MAX_SAMPLES;
    uint32_t idx1 = (base_idx + PHASE_DELAY_MAX_SAMPLES - (uint32_t)i0 - 1) % PHASE_DELAY_MAX_SAMPLES;
    return (ring[idx0] < ring[idx1]) ? ring[idx0] : ring[idx1];
}

// 2026-09-12, later same day: a THIRD sibling, for a different question
// than either of the two above. The delay=+4.28 capture found a clean
// 3-state cycle where 2 of 3 transitions DID have a hidden near-null
// contributor (caught by interp_ring_min() above) but the THIRD, and
// largest (~5761Hz), showed near_null_either=false too - neither raw
// sample near a null by any envelope-based test. Before treating that as
// proof of a null-independent mechanism, it's worth seeing the two RAW
// freq_dev_hz values themselves (not just their envelopes): if they're
// already wildly different from each other, the discontinuity exists in
// the raw, undelayed signal (a genuine event, just not one flagged by the
// envelope-near-null test) - if they're both unremarkable, the large
// delayed step is instead an artifact purely of interpolating across a
// large delay (freq_back) during a part of the waveform where freq_dev is
// naturally changing fast, not evidence of any discrete "event" at all.
// Outputs both raw values (not a blend or a min/max - the two individual
// numbers are what's diagnostic here) via out-params, same index math as
// the two helpers above.
static inline void IRAM_ATTR interp_ring_components(const float *ring, uint32_t base_idx, float back,
                                                     float *out_near, float *out_far)
{
    int32_t i0 = (int32_t)back;
    uint32_t idx0 = (base_idx + PHASE_DELAY_MAX_SAMPLES - (uint32_t)i0) % PHASE_DELAY_MAX_SAMPLES;
    uint32_t idx1 = (base_idx + PHASE_DELAY_MAX_SAMPLES - (uint32_t)i0 - 1) % PHASE_DELAY_MAX_SAMPLES;
    *out_near = ring[idx0];   // the (1-frac)-weighted, "just written" side
    *out_far  = ring[idx1];   // the frac-weighted, "one sample further back" side
}

void IRAM_ATTR relative_delay_apply(float freq_dev_hz, float envelope,
                                     float *out_delayed_freq_dev_hz, float *out_delayed_envelope,
                                     float *out_envelope_at_freq_time,
                                     float *out_envelope_at_freq_time_min,
                                     float *out_raw_freq_dev_near, float *out_raw_freq_dev_far)
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

    // 2026-09-12: see this function's declaration comment (relative_delay.h)
    // for why this is a THIRD, separate read rather than reusing
    // out_delayed_envelope above - deliberately always reads the envelope
    // ring at freq_back (the SAME lag freq_dev was just read at), no matter
    // which of freq_back/env_back is currently nonzero, so it stays
    // time-matched to out_delayed_freq_dev_hz specifically.
    if (out_envelope_at_freq_time) {
        *out_envelope_at_freq_time = interp_ring(s_envelope_ring, s_delay_ring_idx, freq_back);
    }
    // See interp_ring_min()'s declaration comment just above for why this
    // is a genuinely different (more permissive) question than the
    // blended value above, not a redundant re-read.
    if (out_envelope_at_freq_time_min) {
        *out_envelope_at_freq_time_min = interp_ring_min(s_envelope_ring, s_delay_ring_idx, freq_back);
    }
    // See interp_ring_components()'s declaration comment above - the two
    // RAW freq_dev_hz values out_delayed_freq_dev_hz itself blends, not a
    // derived min/blend, since here it's the two individual numbers (and
    // whether THEY already differ wildly) that's diagnostic.
    if (out_raw_freq_dev_near && out_raw_freq_dev_far) {
        interp_ring_components(s_freq_dev_ring, s_delay_ring_idx, freq_back,
                                out_raw_freq_dev_near, out_raw_freq_dev_far);
    }

    s_delay_ring_idx = (s_delay_ring_idx + 1) % PHASE_DELAY_MAX_SAMPLES;
}

float IRAM_ATTR relative_delay_get_samples(void)
{
    return s_relative_delay_samples;
}

// 2026-09-12, yet later still: see s_last_delay_change_ms's own
// declaration comment above. Not IRAM_ATTR - only ever called from
// diagnostics_print_slow_trace() (Core 1, on-demand serial command
// context), same as every other non-hot-path getter in this file.
uint32_t relative_delay_get_last_change_ms(void)
{
    return s_last_delay_change_ms;
}

void relative_delay_increase(void)
{
    if (s_relative_delay_samples < (float)(PHASE_DELAY_MAX_SAMPLES - 2))
        s_relative_delay_samples += DELAY_STEP_SAMPLES;
    s_last_delay_change_ms = millis();
}

void relative_delay_decrease(void)
{
    if (s_relative_delay_samples > -(float)(PHASE_DELAY_MAX_SAMPLES - 2))
        s_relative_delay_samples -= DELAY_STEP_SAMPLES;
    s_last_delay_change_ms = millis();
}

void relative_delay_increase_fine(void)
{
    if (s_relative_delay_samples < (float)(PHASE_DELAY_MAX_SAMPLES - 2))
        s_relative_delay_samples += DELAY_FINE_STEP_SAMPLES;
    s_last_delay_change_ms = millis();
}

void relative_delay_decrease_fine(void)
{
    if (s_relative_delay_samples > -(float)(PHASE_DELAY_MAX_SAMPLES - 2))
        s_relative_delay_samples -= DELAY_FINE_STEP_SAMPLES;
    s_last_delay_change_ms = millis();
}

void relative_delay_set_samples(float samples)
{
    if (samples > (float)(PHASE_DELAY_MAX_SAMPLES - 2))  samples = (float)(PHASE_DELAY_MAX_SAMPLES - 2);
    if (samples < -(float)(PHASE_DELAY_MAX_SAMPLES - 2)) samples = -(float)(PHASE_DELAY_MAX_SAMPLES - 2);
    s_relative_delay_samples = samples;
    s_last_delay_change_ms = millis();
}

#endif // AD9851_ATTACHED
