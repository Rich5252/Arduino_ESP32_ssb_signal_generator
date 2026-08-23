/**
 * envelope_interp.cpp - see envelope_interp.h.
 *
 * v4: folded into dsp_task's own fast tick, no ISR/task/timer/LEDC-fade of
 * our own left in this file at all - see header comment for the three
 * prior versions this replaced and why each failed on real hardware.
 *
 * v4.1: step-COUNTED ramp -> time-BASED ramp (see on_full_tick()/
 * on_interp_tick() and compute_ramp_value() below) - fixes real,
 * structural jitter found on hardware even after the notification-
 * coalescing fix. See envelope_interp.h for the full explanation.
 */

#include "envelope_interp.h"
#include "envelope_output.h"
#include "config.h"
#include "esp_timer.h"

// Real microsecond length of one DSP sample period, as a float - computed
// once here rather than reusing config.h's SSB_SAMPLE_PERIOD_US, which is
// an integer macro (1000000UL / SAMPLE_RATE_HZ) that truncates (62 instead
// of 62.5 at 16000Hz) - fine for its own existing uses elsewhere, but this
// module's ramp fraction is sensitive to that last half-microsecond in a
// way nothing else was, so it gets its own float-precision constant.
static const float SAMPLE_PERIOD_US_F = 1.0e6f / (float)SAMPLE_RATE_HZ;

// s_prev_env/s_target_env/s_tick_start_us/s_last_value are dsp_task-
// PRIVATE - only on_full_tick()/on_interp_tick() ever touch them, and both
// are only ever called from dsp_task itself, strictly sequentially. No
// other task reaches into these, so no volatile/locking is needed for
// them at all (a genuine change from v1-v3, which all needed a spinlock
// because an ISR or second task shared this exact state).
static float   s_prev_env     = 0.0f;
static float   s_target_env   = 0.0f;
static int64_t s_tick_start_us = 0;   // esp_timer_get_time() at the TRUE start of the current full tick - see header comment
static float   s_last_value   = 0.0f; // whatever was last submitted, enabled or not

// The two things serial command handling (loop(), Core 1 - a DIFFERENT
// task/core than dsp_task) actually needs to touch. Both single-word
// volatile bools, same convention already used elsewhere in this codebase
// for cross-task flags (envelope_gdeq/predistort's own enabled bools) -
// deliberately NOT reaching into s_prev_env/s_target_env directly from
// set_enabled(), which would turn them back into genuinely shared multi-
// word state needing a lock again. Instead, set_enabled() just raises
// s_reseed_pending; dsp_task applies the actual reseed itself, in its own
// context, the next time on_full_tick() runs - see there.
static volatile bool s_enabled        = false;
static volatile bool s_reseed_pending = false;

// Time-based, not step-counted (see header comment for why): returns
// where the ramp SHOULD be right now, based on actual elapsed time since
// this tick group's true start, not on how many on_interp_tick() calls
// have happened to fire. Whatever calls DO happen - however many, however
// irregularly spaced, even after a coalescing event drops some - each one
// computes the mathematically correct point on the prev->target line for
// the instant it actually runs, so the rendered envelope stays consistent
// tick to tick regardless of exactly how many discrete steps happened to
// render on any given cycle.
static float IRAM_ATTR compute_ramp_value(void)
{
    float elapsed_us = (float)(esp_timer_get_time() - s_tick_start_us);
    float frac = elapsed_us / SAMPLE_PERIOD_US_F;
    if (frac < 0.0f) {
        frac = 0.0f;
    } else if (frac > 1.0f) {
        frac = 1.0f;
    }
    return s_prev_env + frac * (s_target_env - s_prev_env);
}

void envelope_interp_init(void)
{
    s_prev_env = 0.0f;
    s_target_env = 0.0f;
    s_tick_start_us = 0;
    s_last_value = 0.0f;
    s_reseed_pending = false;
}

void IRAM_ATTR envelope_interp_on_full_tick(float envelope, int64_t tick_start_us)
{
    if (s_reseed_pending) {
        // An off->on transition happened since our last tick - anchor the
        // ramp at the last real value instead of gliding from whatever
        // s_prev_env/s_target_env happened to hold from before it was
        // last disabled. Applied here, in dsp_task's own context, rather
        // than inside set_enabled() itself (a different task/core) - see
        // the statics' own comment above for why.
        s_prev_env = s_last_value;
        s_target_env = s_last_value;
        s_reseed_pending = false;
    }

    // tick_start_us - captured by the caller at the TRUE top of this full
    // tick, before any DSP processing - not esp_timer_get_time() taken
    // HERE, which would already be tens of microseconds late (this call
    // site is reached only after the full Hilbert/atan2/sqrt/etc. pipeline
    // has already run). Using the true start as the ramp's t=0 reference
    // is what makes compute_ramp_value() time-accurate regardless of how
    // long this tick's own DSP processing took - see header comment.
    s_tick_start_us = tick_start_us;

    if (!s_enabled) {
        s_last_value = envelope;
        envelope_output_write_pwm(envelope);
        return;
    }

    // Start a new ramp from the previous full-tick's value to this one -
    // arriving at the true new value only at the far end of the interval
    // is intentional, not a bug; see header comment for why.
    s_prev_env = s_target_env;
    s_target_env = envelope;

    envelope_output_write_pwm(compute_ramp_value());

    s_last_value = envelope;
}

void IRAM_ATTR envelope_interp_on_interp_tick(void)
{
    if (!s_enabled || s_reseed_pending) {
        // Either disabled (hold - matches pre-feature ZOH behavior
        // exactly), or a reseed is waiting for the next full tick to
        // apply it (see on_full_tick()) - hold rather than ramp from
        // stale state in the meantime. Either way: no output change.
        return;
    }

    envelope_output_write_pwm(compute_ramp_value());
}

bool envelope_interp_get_enabled(void)
{
    return s_enabled;
}

void envelope_interp_set_enabled(bool enable)
{
    if (enable == s_enabled) {
        return;   // no-op transition, same convention as envelope_gdeq_set_enabled()
    }

    if (enable) {
        s_reseed_pending = true;   // dsp_task applies this on its next full tick - see on_full_tick()
    }

    s_enabled = enable;
}

bool envelope_interp_reseed_pending(void)
{
    return s_reseed_pending;
}
