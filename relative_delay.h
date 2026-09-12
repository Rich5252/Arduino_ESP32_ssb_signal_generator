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
// out_envelope_at_freq_time (2026-09-12): the envelope value from the SAME
// original sample time as out_delayed_freq_dev_hz, regardless of which
// direction relative_delay is currently biased. This is NOT the same thing
// as out_delayed_envelope: when delay>0 (every two-tone preset), envelope
// itself is read at zero lag (env_back=0 below), so out_delayed_envelope is
// just the CURRENT tick's envelope - not time-matched to the freq_dev value
// that got delayed. Added because diagnostics.cpp's jump log needs to ask
// "was envelope near a null WHEN THIS (now-delayed) freq_dev value was
// actually computed", which out_delayed_envelope cannot answer once delay
// is large - see that file's near_null classification and the 2026-09-12
// entry in null_bias_investigation.md this was found from.
//
// out_envelope_at_freq_time_min (2026-09-12, same day): the smaller of the
// TWO raw envelope samples that out_envelope_at_freq_time actually blends
// together (via linear interpolation) - i.e. "did EITHER contributing raw
// sample dip near a null", not "did the blended result end up near a
// null". Added because a lopsided fractional delay (frac far from 0.5) can
// blend in a near-null raw sample at only 10-20% weight, which won't pull
// the BLENDED value below any reasonable near-null threshold even though a
// genuine null-crossing sample contributed to this output. First real
// jump-log capture at delay=+0.90 (frac=0.90, a 10%/90% blend) showed
// exactly this ambiguity - see null_bias_investigation.md's 2026-09-12
// entry for the full reasoning.
//
// out_raw_freq_dev_near/_far (2026-09-12, later same day): the two RAW
// freq_dev_hz ring entries out_delayed_freq_dev_hz itself blends together
// (near = the (1-frac)-weighted "just written" side, far = the
// frac-weighted "one sample further back" side) - not a blend, min, or max,
// the two individual numbers. Added after a delay=+4.28 capture found a
// clean 3-state jump cycle where 2 of 3 transitions had a near-null
// contributor hidden by a lopsided blend (caught by
// out_envelope_at_freq_time_min above) but the third, largest transition
// showed NO near-null involvement by either envelope test. Before treating
// that as proof of a null-independent mechanism, this answers a more basic
// question: were the two raw freq_dev_hz values already wildly different
// from each other (a genuine discontinuity exists in the raw, undelayed
// signal - just not one the envelope-near-null test happens to catch), or
// are both individually unremarkable (the large DELAYED step is then an
// artifact of interpolating across a large lag during a fast-changing part
// of the waveform, not evidence of a discrete event at all)? See
// null_bias_investigation.md's 2026-09-12 entries for the full reasoning.
//
// All four new outputs (this pair plus the two above): pass NULL if a
// caller doesn't need them (matches every other optional-output convention
// this codebase uses elsewhere... actually there are none - these are the
// first - guarded with NULL checks purely for caller-safety, not because
// any current call site omits them).
void IRAM_ATTR relative_delay_apply(float freq_dev_hz, float envelope,
                                     float *out_delayed_freq_dev_hz, float *out_delayed_envelope,
                                     float *out_envelope_at_freq_time,
                                     float *out_envelope_at_freq_time_min,
                                     float *out_raw_freq_dev_near, float *out_raw_freq_dev_far);

// 2026-09-12: marked IRAM_ATTR - diagnostics.cpp's new per-event jump log
// now calls this from diagnostics_set_tx_info(), which runs on the
// dsp_task hot path (same reasoning as ssb_dsp_get_null_bias_threshold()'s
// equivalent change, ssb_dsp.h). Trivial single-variable read.
float IRAM_ATTR relative_delay_get_samples(void);

// 2026-09-12, yet later still: millis() timestamp of the last
// relative_delay change from ANY source ('['/']'/'''/';' or preset
// loading via relative_delay_set_samples()) - added after a 'K'
// slow-trace capture explicitly flagged by the user as "[] scan induced"
// came back showing NO visible transition in its printed 50ms-before/
// 50ms-after trace, despite a real fast/slow EMA divergence having
// fired. Leading theory: the delay-change-induced excursion happened,
// then fully resolved, before the trace's 50ms pre-window even started -
// the slow (2s-tau) EMA can still be "remembering" a transient for
// seconds after it's otherwise fully passed. Rather than growing the
// trace buffers to multi-second span, this timestamps delay changes
// directly so 'K' can report "delay last touched Xms before this
// trigger," letting a human correlate a trigger against their own recent
// '['/']'/preset actions even when the trace itself doesn't visually
// capture the causal moment. Not IRAM_ATTR - only ever read from
// diagnostics_print_slow_trace() (Core 1, on-demand serial command
// context), never the hot path.
uint32_t relative_delay_get_last_change_ms(void);

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
