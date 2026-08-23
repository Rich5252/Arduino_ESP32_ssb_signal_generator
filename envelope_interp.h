#pragma once

/**
 * envelope_interp.h
 *
 * ---- Envelope output interpolation (ZOH-imaging reduction) ----
 * Right now the commanded PWM duty only changes once per DSP tick
 * (SAMPLE_RATE_HZ = 16000) - a classic zero-order-hold staircase feeding
 * the analog Sallen-Key reconstruction filter. A ZOH held at Fs puts its
 * first spectral image at Fs itself, and per the real LTspice sweep behind
 * envelope_gdeq.h, the analog filter only provides -27dB of attenuation
 * there - not deeply buried at all. This mirrors exactly what QRP Labs'
 * G0UPL found and fixed in the QMX SSB firmware ("Bringing SSB to QMX",
 * see project history): before linearly interpolating the amplitude
 * stream, the oscilloscope showed clear overshoot on rising steps and
 * undershoot on falling ones; after interpolating, "NO visible trace of
 * any overshoot/undershoot". He went to 28x (driven by his own DAC/CPU
 * clock constraints); ENVELOPE_INTERP_FACTOR=4 here is a deliberately
 * modest first step, not a re-derivation of his number - see below.
 *
 * Checked against the same filter data before writing any of this: moving
 * the first ZOH image from Fs (16kHz, -27dB down) out to 4*Fs (64kHz,
 * -63dB down) is +36dB of "for free" suppression, just from relocating the
 * image into a part of the filter's stopband it was never asked to fight
 * in-band. NOT yet validated on real hardware - same status every other
 * not-yet-bench-confirmed feature here starts at (gdeq, predistort).
 *
 * ---- Design history (FOUR attempts - the first three each failed on real
 * hardware in a different way; recorded here because each rules out an
 * approach that looks reasonable on paper) ----
 *
 *   v1: a second, independent gptimer at 64kHz, whose ISR did the
 *       interpolation arithmetic AND called envelope_output_write_pwm()
 *       (ledc_set_duty()/ledc_update_duty()) directly from true interrupt
 *       context. REBOOTED the ESP32 - those are general ESP-IDF driver
 *       calls with no IRAM/ISR-safety guarantee.
 *   v2: kept the 64kHz ISR, deferred only the PWM write to a dedicated
 *       task via vTaskNotifyGiveFromISR. Still crashed - a "Coprocessor
 *       exception" panic, because the ISR still did the interpolation
 *       *arithmetic* (floats), and on this chip's Xtensa cores the FPU is
 *       itself a coprocessor whose context only survives a *task* switch;
 *       any float op in a true ISR is unsafe regardless of what's done
 *       with the result.
 *   v3: moved ALL the work (arithmetic + write) into a dedicated task, ISR
 *       reduced to an integer step-counter + notify. Stopped crashing, but
 *       waking a task 64,000 times/second was enough CPU overhead to
 *       starve handle_serial_commands() (loop(), priority 1, same Core 1
 *       the task was pinned to) - 'I' stopped rebooting the board but the
 *       board then stopped responding to serial input entirely. Then
 *       rebuilt around the LEDC peripheral's own hardware duty-fade engine
 *       (ledc_set_fade_with_step()+ledc_fade_start()) instead of a
 *       software timer/ISR/task at all - no more per-sub-step CPU cost.
 *       That fixed the crash and the starvation, but exposed a THIRD,
 *       different problem: the LEDC PWM clock (78125Hz) has no phase
 *       relationship to SAMPLE_RATE_HZ's own gptimer (78125/16000 isn't
 *       even an integer ratio), so every fade's first step waited for an
 *       essentially random PWM-clock boundary - up to ~12.8us of
 *       unsynchronized timing jitter per tick, visible on real hardware as
 *       the envelope's effective frequency jittering.
 *   v4 (current): the user's own suggestion - stop trying to interpolate
 *       from a SECOND clock domain entirely. Instead, speed up the
 *       EXISTING sample gptimer itself (the one dsp_task already blocks
 *       on) to ENVELOPE_INTERP_FACTOR x SAMPLE_RATE_HZ, and do the full,
 *       expensive DSP pipeline (Hilbert/atan2/sqrt/freq_dev/ADC/AD9851/
 *       diagnostics - everything) on only 1 in ENVELOPE_INTERP_FACTOR of
 *       those ticks ("full" ticks, still true SAMPLE_RATE_HZ), while the
 *       other 3 just walk a linear interpolation ramp and call the same
 *       plain envelope_output_write_pwm() the original code always used.
 *       This is a strictly simpler machine than v1-v3: ONE clock domain
 *       (no cross-clock jitter - fixes v3's bug), ZERO new ISRs or tasks
 *       of our own (everything happens inside dsp_task, already a normal,
 *       proven-safe FreeRTOS task - fixes v1/v2's bugs), and dsp_task has
 *       ALWAYS lived on Core 0, physically separate from loop()/Serial on
 *       Core 1 - so running it more often cannot starve serial commands
 *       the way pinning a NEW task onto Core 1 did in v3 (that was the
 *       actual mistake in v3, not "task priority too high" - no priority
 *       tweak within v3's architecture could have fixed it).
 *
 * ---- v4 design ----
 * dsp_task (see the .ino) keeps a plain, single-threaded fast-tick counter
 * and treats tick (counter % ENVELOPE_INTERP_FACTOR == 0) as a "full"
 * tick - unchanged DSP pipeline, still running at the true SAMPLE_RATE_HZ
 * exactly as before this feature existed. Every other filter constant,
 * diagnostic counter, ADC capture call, and AD9851 write in the codebase
 * keeps assuming SAMPLE_RATE_HZ unchanged - the gptimer's rate changed,
 * SAMPLE_RATE_HZ itself deliberately did NOT. The counter itself is
 * advanced by ulTaskNotifyTake()'s own return value (how many hardware
 * ticks actually elapsed), not by a bare +1 per wake - see the .ino for
 * why: xClearCountOnExit=pdTRUE silently coalesces multiple real ticks
 * into one wake whenever dsp_task is even briefly late, and a naive +1
 * counter desyncs permanently the first time that happens.
 *
 * At each full tick, envelope_interp_on_full_tick(delayed_envelope,
 * tick_start_us) is called (replacing the old, unconditional
 * envelope_output_write_pwm() call at that point) - tick_start_us is
 * esp_timer_get_time() captured by dsp_task at the TRUE top of this tick,
 * before any DSP processing, and is what makes the ramp below time-
 * accurate rather than step-counted (see next paragraph for why that
 * distinction turned out to matter). When disabled, it's a direct pass-
 * through - IDENTICAL behavior to before this feature existed. When
 * enabled, it starts a new ramp from the previous full-tick's envelope
 * value to this one. On each of the ENVELOPE_INTERP_FACTOR-1 "interp"
 * ticks in between, envelope_interp_on_interp_tick() writes the next
 * point on that ramp via the same plain envelope_output_write_pwm().
 * No-op when disabled - PWM duty simply holds its last value between full
 * ticks, i.e. plain ZOH, matching pre-feature behavior exactly.
 *
 * v4.1 bug fix, found on real hardware: the envelope's timing still
 * looked jittery even AFTER the notification-coalescing fix above landed
 * (which only guaranteed full-tick PHASE stayed locked to the true 16kHz
 * grid - it said nothing about the RAMP's own shape). Root cause: the
 * first cut counted ramp position by a plain step counter (s_step,
 * incremented once per on_interp_tick() call, frac=s_step/FACTOR) rather
 * than by real elapsed time. That's fragile against exactly the same
 * coalescing effect described above, but now applied to "interp" ticks
 * specifically - and it turns out to fire on almost every group, not
 * rarely: a full tick's own DSP processing (tens of us) is comparable to
 * or LONGER than one fast sub-period (SAMPLE_PERIOD_US_F /
 * ENVELOPE_INTERP_FACTOR = ~15.6us), so by the time dsp_task finishes a
 * full tick's work and gets back to blocking, 1-2 of the FOLLOWING
 * interp ticks have usually already elapsed and coalesced away - losing
 * that many s_step increments, every cycle, by an amount that varies with
 * exactly how long that particular tick's DSP processing happened to
 * take. That made the rendered ramp shape irregular tick to tick - a real,
 * structural, sample-content-dependent source of jitter in the fine-
 * grained envelope shape, distinct from (and not fixed by) the earlier
 * full-tick-phase fix.
 *
 * The fix: stop counting steps: compute_ramp_value() (envelope_interp.cpp)
 * derives frac directly from esp_timer_get_time() - s_tick_start_us,
 * divided by the true sample period. Whatever calls DO happen - however
 * many, however irregularly spaced, even with some coalesced away - each
 * one computes the mathematically correct point on the prev->target line
 * for the instant it actually runs. Fewer calls in a given cycle just
 * means fewer visible steps in that cycle's staircase, not a wrong value
 * at the steps that do render - which is what actually mattered for the
 * jitter, since the analog reconstruction filter downstream only ever
 * sees the written VALUES and when they land, not how many software calls
 * produced them.
 *
 * Why ramp forward from the OLD value to the NEW one across the
 * FOLLOWING interval, rather than jumping straight to the new value
 * immediately: there is no way to interpolate TOWARD a sample that
 * hasn't been computed yet without introducing latency somewhere - any
 * causal (non-clairvoyant) smoothing of a step MUST spend the interval
 * AFTER a new value becomes known smoothing the transition INTO it, which
 * means the reconstructed output only reaches that true value at the far
 * end of the interval. This trades exactly one sample period
 * (1/SAMPLE_RATE_HZ, 62.5us) of pure added group delay for eliminating
 * the step discontinuity - a completely standard reconstruction-filter
 * tradeoff, and one already within what this project's existing relative-
 * delay line ('['/']', relative_delay.h) is built to compensate for
 * exactly this kind of envelope-path delay. v1-v3 all had this same
 * property already (look back at their own "arrives at the new target
 * right as the next tick fires" framing) - v4 doesn't change the
 * TIMING model, only how (and how safely) it's implemented.
 *
 * No locking anywhere in this version, unlike v1-v3 (which all needed a
 * spinlock because an ISR or second task shared the ramp state directly):
 * s_prev_env/s_target_env/s_tick_start_us/s_last_value are dsp_task-
 * PRIVATE - on_full_tick()/on_interp_tick() are only ever called from dsp_task
 * itself, strictly sequentially, and no other task reaches into them.
 * envelope_interp_set_enabled() IS called from a different task (whichever
 * one processes serial commands - loop(), Core 1) - but rather than
 * having it reach into that dsp_task-private state directly (which would
 * make it shared again and need a lock), it only raises a single-word
 * volatile flag (s_reseed_pending); dsp_task itself applies the actual
 * reseed, in its own context, the next time on_full_tick() runs. Both
 * s_enabled and s_reseed_pending are plain volatile bools, same
 * convention already used elsewhere in this codebase for single-word
 * cross-task flags (envelope_gdeq/predistort's own enabled bools).
 *
 * Off by default (same convention as 'g'/'D'/'x'/'z'/'{'/'}') so existing
 * tuning isn't disturbed until deliberately opted into. Toggle via 'I'.
 * On an off->on transition, the ramp's prev/target are both seeded to the
 * last submitted value and marked already-arrived, so enabling never
 * produces a glide-from-zero (or glide-from-stale-value) transient - see
 * envelope_interp_set_enabled()'s own comment.
 *
 * Runs regardless of AD9851_ATTACHED - like envelope_output.cpp itself,
 * the PWM/RSET envelope path doesn't depend on the AD9851 phase path being
 * present.
 *
 * NOT yet checked on real hardware (this is a fresh rewrite): whether
 * Core 0 comfortably sustains the interp ticks' small extra register-write
 * work on top of the full tick's already-proven budget, at 4x the wake
 * rate. Expected to have ample margin (the full tick's own cost is
 * unchanged from before this feature existed; the interp ticks are
 * trivial - a few FLOPs and the same plain PWM write the original code
 * always made), but worth watching on the existing '[timing]' diagnostic
 * overlay ('v') after enabling 'I', same as every other not-yet-bench-
 * confirmed feature here.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_attr.h"

// Deliberately modest first step (see header comment above for why this
// isn't just "copy Hans's 28x") - not currently exposed as a runtime-
// tunable the way the slew limiter is; revisit as a live '<'/'>'-style
// step if/once a fixed 4x is confirmed worthwhile on real hardware.
#define ENVELOPE_INTERP_FACTOR 4

// Resets the ramp state. Call once from setup(), after envelope_output_
// init() (harmless either order now that there's no LEDC/gptimer setup
// left in here, but kept for call-site consistency with every other
// module's _init()).
void envelope_interp_init(void);

// Call once per "full" DSP tick (see header comment for what that means)
// with the newly computed, already-relative-delayed envelope value, and
// tick_start_us = esp_timer_get_time() captured at the TRUE top of this
// tick (before any DSP processing) - the ramp's time-accuracy depends on
// this being the real tick start, not a timestamp taken at this call site
// (which is reached only after the full DSP pipeline has already run) -
// see header comment. In place of calling envelope_output_write_pwm()
// directly, which this REPLACES at that call site (doesn't add a second
// one).
void IRAM_ATTR envelope_interp_on_full_tick(float envelope, int64_t tick_start_us);

// Call once per "interp" tick (the ENVELOPE_INTERP_FACTOR-1 fast ticks
// between each pair of full ticks) - no arguments, walks the ramp one
// more sub-step internally. No-op when disabled.
void IRAM_ATTR envelope_interp_on_interp_tick(void);

bool envelope_interp_get_enabled(void);

// On an off->on transition, seeds the ramp's prev/target from the last
// submitted value (not zero) and marks it already-arrived, so the very
// next full tick starts a normal, correctly-anchored ramp instead of
// gliding from a stale or zero value - see header comment. A no-op
// transition (already in the requested state) does nothing, same
// convention as envelope_gdeq_set_enabled().
void envelope_interp_set_enabled(bool enable);

// True from the moment an off->on transition is requested until
// dsp_task's next full tick actually applies the reseed (on_full_tick()
// above) - i.e. the window where set_enabled(true) has returned but the
// internal switch hasn't taken effect yet. Exposed so a caller that wants
// to report state only once the switch is truly complete internally, not
// just once the request has been queued, can poll this first - see
// serial_commands.cpp's 'I' handler.
bool envelope_interp_reseed_pending(void);
