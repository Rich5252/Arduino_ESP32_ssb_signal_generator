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
 *       CORROBORATION, Fs-jitter-hunt: dsp_task was later TRIED on Core 1
 *       (to eliminate gptimer's crosscore-IPI wake cost - see the .ino's
 *       "TRIED, REVERTED" note on its xTaskCreatePinnedToCore() call) and
 *       REVERTED after real hardware starved Serial completely (output
 *       AND commands). Notably this happened with interp ('I') OFF, i.e.
 *       WITHOUT the 4x wake rate this paragraph's v3 warning is about -
 *       CPU-time budget alone said Core 1 had ample headroom (see
 *       diagnostics.cpp's [core1] busy breakdown, ~98-99% idle) yet
 *       loopTask still got starved outright, not just delayed. So
 *       whatever actually broke v3 may be a harder structural problem
 *       with sharing a core between dsp_task and loop() than "not enough
 *       spare CPU time" - root cause not yet understood either time.
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
 * s_p0..s_p3/s_m1/s_m2/s_tick_start_us/s_last_value are dsp_task-PRIVATE -
 * on_full_tick()/on_interp_tick() are only ever called from dsp_task
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
 * confirmed feature here. (dsp_task stays on Core 0 for this - see this
 * file's own "CORROBORATION, Fs-jitter-hunt" note above for why Core 1
 * isn't a safe alternative home for it.)
 *
 * ---- v4.2: linear ramp -> Catmull-Rom / cubic Hermite curve ----
 * Motivated by a real question: is there a cheaper way to improve
 * envelope-path fidelity than raising ENVELOPE_INTERP_FACTOR to 8?
 * Answer: raising it further is a dead end regardless of CPU budget -
 * RSET_MOD_LEDC_FREQ_HZ (the LEDC PWM carrier, fixed at 78125Hz) is a
 * hard ceiling on any useful envelope update rate, and 4x (64kHz) is
 * already the highest multiple of SAMPLE_RATE_HZ that stays under it; 8x
 * (128kHz) would write the duty register faster than the PWM carrier
 * itself can act on it, for zero real benefit. Curve SHAPE, not update
 * RATE, was the remaining lever: v4's ramp is a straight line between
 * consecutive full-tick samples, which has a kink at every tick boundary
 * - real spectral content a smooth signal shouldn't have. Catmull-Rom
 * fits a smooth cubic through the same samples instead, with no extra
 * PWM/wake-rate cost at all (same 4 calls per full tick as before, still
 * ENVELOPE_INTERP_FACTOR=4 - a few more FLOPs per call is the entire
 * added cost).
 *
 * Validated first in an offline Python simulation (not this codebase)
 * against two of this project's own real test signals, not an arbitrary
 * synthetic one: the two-tone envelope (700/1900Hz, TWOTONE_AMPLITUDE=
 * 0.45 - closed-form analytic envelope = 2A*|cos(pi*(f2-f1)*t)|, a
 * full-wave-rectified 600Hz cosine with a hard null every 1/1200s) and
 * the AM-test envelope (AM_TEST_MOD_HZ=1200Hz smooth sine, no fold at
 * all). Result was a genuine split, not a clean win either way: ~23dB
 * interpolation-error reduction on the smooth AM-test envelope, but only
 * ~2dB on the two-tone envelope specifically - because that error is
 * dominated by the null's own hard fold (a derivative discontinuity),
 * which no smooth interpolant, linear or cubic, can fit well; null-region
 * behavior is envelope_floor's job, not this one's. So: worth having as a
 * general envelope-quality improvement (mic audio and most other test
 * modes should see something closer to the AM-test result), but don't
 * expect it to move the two-tone IMD3 number much on its own.
 *
 * Data layout: s_prev_env/s_target_env (2 points) became s_p0..s_p3 (4
 * consecutive full-tick samples, oldest to newest) plus two tangents
 * s_m1/s_m2, all recomputed once per full tick and reused across that
 * tick's interp ticks - same reuse pattern v4 already used, just wider
 * history. The segment actually RENDERED during any given full tick is
 * [s_p1,s_p2] - s_p0/s_p3 only shape the tangents at each end (standard
 * Catmull-Rom: m1=(p2-p0)/2, m2=(p3-p1)/2), they're never points the
 * curve itself passes through.
 *
 * Latency cost: v4 already added one full tick (~62.5us) of group delay
 * by design (see "arrives at the new value only at the far end of the
 * interval" above) - v4.2 adds ONE MORE full tick on top of that (~125us
 * total now), because the tangent at the segment's own right end (s_m2)
 * needs s_p3 - the NEXT full tick's value - which only becomes known the
 * instant that next tick's on_full_tick() call happens. This is the
 * standard Catmull-Rom look-ahead requirement, not a bug, and it's
 * exactly the same category of fixed envelope-path delay relative_delay
 * ('['/']') already exists to compensate - re-tune it after enabling,
 * same as 'g' and 'I' itself already ask you to.
 *
 * A cubic, unlike v4's straight line, can mathematically over/undershoot
 * slightly beyond its own segment endpoints on a sharp enough feature -
 * neither real test signal above triggered this, but a synthetic
 * worst-case check (two duty-0 samples flanked by two much higher ones -
 * close to what a two-tone null looks like) did produce a small negative
 * excursion. compute_ramp_value() clamps its result to [0,1] to close
 * this off unconditionally, since envelope_output_write_pwm() casts
 * straight to an unsigned duty with no clamp of its own downstream - an
 * unclamped negative excursion would become a huge duty via unsigned
 * wraparound, not a small negative one.
 *
 * NOT yet validated on real hardware - baseline already captured before
 * this change (linear 'I' x4, current 'D' predistort table) via the
 * user's own logger, specifically so this change has a direct before/
 * after to compare against once flashed.
 *
 * ---- v4.3: curve made runtime-switchable (linear vs. Catmull-Rom) ----
 * Added for a direct A/B, prompted by the group-delay-equalizer/new-filter
 * investigation (see group_delay_fit_notes.md's 2026-09-01 entries):
 * v4.2's own validation already showed a smooth cubic structurally cannot
 * reproduce a two-tone null's hard fold (a genuine derivative
 * discontinuity - see the v4.2 note above) the way a straight-line ramp
 * naturally can, and there was no way to test that on real hardware
 * without a rebuild/reflash between the two curves. compute_ramp_value()
 * now branches on envelope_interp_get_curve(): ENVELOPE_INTERP_CURVE_
 * CATMULL_ROM (=0, the v4.2 behavior, unchanged) or ENVELOPE_INTERP_CURVE_
 * LINEAR (=1, a straight line from s_p1 to s_p2).
 *
 * Deliberately does NOT restore v4's original 2-point/1-tick-latency
 * history - both curve choices still evaluate over the SAME [s_p1,s_p2]
 * segment from the existing v4.2 4-point/2-tick-latency pipeline
 * (s_p0..s_p3/s_m1/s_m2 unchanged either way; linear mode simply doesn't
 * use s_m1/s_m2). That's an intentional choice, not laziness: reverting to
 * v4's narrower window for "linear" mode would confound the comparison
 * with a one-tick group-delay difference between the two curves, on top of
 * whatever the curve SHAPE itself does - exactly the kind of extra
 * variable this project has had to tease apart before (see gdeq's own
 * fit-window-vs-wideband-excitation confusion). Holding the data window
 * and timing identical between LINEAR and CATMULL_ROM isolates curve shape
 * alone, so a straight before/after comparison at a FIXED relative_delay
 * is meaningful without needing to also re-tune delay between the two
 * curve choices - matching the project's actual purpose here (a direct
 * A/B), not a literal restoration of v4's original zero-look-ahead
 * implementation.
 *
 * Runtime toggle via 'C' (serial_commands.cpp) - cycles CATMULL_ROM <->
 * LINEAR, independent of 'I' itself (the enable/disable toggle); only
 * affects rendered output while 'I' is ON (envelope_interp_get_enabled()
 * true) - with 'I' off, on_full_tick() takes its early-return plain-ZOH
 * path regardless of curve selection. No reseed needed on a curve switch
 * (unlike 'I' itself) - it only changes which formula reads the existing
 * s_p0..s_p3/s_m1/s_m2 state, not the state itself, so a mid-ramp switch
 * just changes the shape of the segment currently being rendered, not its
 * endpoints. Persisted in PersistentSettings as envelope_interp_curve
 * (settings.h), appended at the struct's end same as every other lever
 * added after presets already existed - defaults to ENVELOPE_INTERP_CURVE_
 * CATMULL_ROM (=0) so every existing preset keeps today's live behavior
 * unless explicitly set otherwise.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_attr.h"

// Deliberately modest first step (see header comment above for why this
// isn't just "copy Hans's 28x") - not currently exposed as a runtime-
// tunable the way the slew limiter is; revisit as a live '<'/'>'-style
// step if/once a fixed 4x is confirmed worthwhile on real hardware.
#define ENVELOPE_INTERP_FACTOR 4

// v4.3: which curve compute_ramp_value() evaluates over the [s_p1,s_p2]
// segment - see the "v4.3" header note above for the full rationale.
// CATMULL_ROM is 0 (not LINEAR) specifically so a PersistentSettings preset
// that doesn't explicitly set envelope_interp_curve (relying on C's
// zero-fill of trailing struct initializers) keeps today's actual live
// behavior, not v4's older one.
typedef enum {
    ENVELOPE_INTERP_CURVE_CATMULL_ROM = 0,  // v4.2 (current default) - smooth cubic Hermite
    ENVELOPE_INTERP_CURVE_LINEAR      = 1,  // v4's original straight-line ramp, for direct A/B
} envelope_interp_curve_t;

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

// v4.3: which curve is currently selected - see the "v4.3" header note
// above. Only affects rendered output while envelope_interp_get_enabled()
// is true.
envelope_interp_curve_t envelope_interp_get_curve(void);

// v4.3: switch curves live. No reseed/transient handling needed (unlike
// envelope_interp_set_enabled()) - see header note for why a mid-ramp
// switch is safe as a plain store.
void envelope_interp_set_curve(envelope_interp_curve_t curve);
