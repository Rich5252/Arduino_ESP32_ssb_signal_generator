#pragma once

/**
 * relative_delay.h
 *
 * Phase/envelope relative-timing compensation. Originally assumed the
 * envelope path (PWM -> analog Sallen-Key filter -> RSET, ~140us measured
 * group delay via the 'p' envelope step test) always lagged the phase
 * path (AD9851 SPI write, ~instant once the write completes), so only
 * positive delay (holding freq_dev_hz back to let envelope catch up) was
 * implemented. Real hardware testing found the opposite in some
 * conditions - positive delay made two-tone IMDs WORSE, not better,
 * suggesting the software reorder done earlier (PWM write moved ahead of
 * the AD9851 write - see envelope_output.h) may have already
 * over-corrected, leaving envelope arriving slightly EARLY rather than
 * late. Bipolar now: positive relative-delay-samples holds freq_dev_hz
 * back (as before); negative holds the ENVELOPE back instead, letting the
 * real optimum be found empirically in either direction rather than
 * assumed. Sign convention: positive = phase delayed relative to
 * envelope, negative = envelope delayed relative to phase.
 *
 * FRACTIONAL: real hardware testing found integer delay=0 beats both
 * delay=1 (100us) and negative delay, meaning whatever residual timing
 * error remains has to be under half a sample (~50us) - smaller than
 * this delay line could ever resolve while restricted to whole-sample
 * steps. Doubling HILBERT_TAPS (65->129) ruled out Hilbert filter
 * approximation accuracy as the cause of the level-tracking IMD floor
 * seen at low drive, which points back at this sub-sample timing
 * residual as the more likely remaining explanation. Now linearly
 * interpolated between adjacent ring entries, so delay can be set to
 * e.g. 0.3 samples, not just 0 or 1.
 *
 * Only meaningful when AD9851_ATTACHED - the whole point is delaying one
 * of two otherwise-independent output paths (AD9851 phase vs PWM/DAC
 * envelope) relative to the other, so this entire module compiles out
 * when there's no AD9851 to have a phase path at all (same guarding the
 * original .ino used).
 */

#include "config.h"
#include "esp_attr.h"

#if AD9851_ATTACHED

#define PHASE_DELAY_MAX_SAMPLES 8   // ring buffer capacity (both rings) - generous headroom
                                     // over the ~1-2 samples actually expected to be needed
#define DELAY_STEP_SAMPLES 0.05f    // 5us per '['/']' keypress at 10kHz (3.125us at 16kHz -
                                     // this is a fixed SAMPLE count, so raising SAMPLE_RATE_HZ
                                     // makes each keypress a finer real-time step for free,
                                     // same direction as the 10000->16000 change) - tightened
                                     // from an initial 0.25 (25us @ 10kHz) once real hardware
                                     // testing found a sweet spot near -0.25 samples, to resolve
                                     // it more precisely than that coarser step could

// Added 2026-09-06 after the candidate-B bench test found BOTH the default
// and candidate-B gdeq configs need a delay "compromise" chosen from
// within a sub-0.1-sample window (2.00-2.10 samples @ 16000Hz) where
// different two-tone IMD orders trade off against each other - too fine a
// distinction to make with only the 0.05-sample '['/']' step (2 presses
// covers that entire window with no intermediate points). Same
// coarse/fine pairing convention as MASTER_GAIN_STEP_DB/_FINE_STEP_DB
// ('+'/'-' vs '.'/','), just under different keys since '.'/',' and
// '{'/'}' (the natural shifted '['/']' pair) are both already taken -
// ';'/''' sit in the same keyboard neighborhood as '['/']' on a US QWERTY
// layout, which is why they were picked over an unrelated pair.
#define DELAY_FINE_STEP_SAMPLES 0.01f  // 1/5th of DELAY_STEP_SAMPLES - 0.625us at 16kHz

// Both rings are always written/read together, BEFORE either output is
// driven - the current relative-delay value decides whether the
// freq_dev or the envelope side actually gets held back; the other one
// reads its own just-written (undelayed) value. Call once per dsp_task
// tick, right after computing freq_dev_hz/envelope for that tick (mirrors
// exactly where the original inline block sat in dsp_task, immediately
// before the PWM/AD9851 writes).
void IRAM_ATTR relative_delay_apply(float freq_dev_hz, float envelope,
                                     float *out_delayed_freq_dev_hz, float *out_delayed_envelope);

float relative_delay_get_samples(void);

// ']' - increase relative delay by one DELAY_STEP_SAMPLES, clamped to
// +(PHASE_DELAY_MAX_SAMPLES-2).
void relative_delay_increase(void);

// '[' - decrease relative delay by one DELAY_STEP_SAMPLES, clamped to
// -(PHASE_DELAY_MAX_SAMPLES-2).
void relative_delay_decrease(void);

// ''' - increase relative delay by one DELAY_FINE_STEP_SAMPLES (2026-09-06),
// same clamp as relative_delay_increase().
void relative_delay_increase_fine(void);

// ';' - decrease relative delay by one DELAY_FINE_STEP_SAMPLES (2026-09-06),
// same clamp as relative_delay_decrease().
void relative_delay_decrease_fine(void);

// Sets relative delay directly (used by preset loading) - clamped the
// same way the '['/']' handlers are.
void relative_delay_set_samples(float samples);

#endif // AD9851_ATTACHED
