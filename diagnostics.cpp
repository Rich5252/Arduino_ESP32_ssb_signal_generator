/**
 * diagnostics.cpp - see diagnostics.h.
 */

#include "diagnostics.h"
#include "config.h"
#include "dsp_state.h"
#include "adc_capture.h"
#include "envelope_gdeq.h"
#include "envelope_ampeq.h"
#include "envelope_output.h"
#include "ssb_dsp.h"
#include "carrier_output.h"
#include "relative_delay.h"   // 2026-09-12: relative_delay_get_samples() - per-event jump log below
#include "test_signals.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_freertos_hooks.h"   // esp_register_freertos_idle_hook_for_cpu() - see core1_idle_hook() below
#include <Arduino.h>

// Written by dsp_task, printed by diagnostics_service() on Core 1 at low
// priority - keeps Serial (slow) completely out of the real-time task.
static volatile float s_dbg_envelope = 0.0f;
static volatile float s_dbg_freq_dev = 0.0f;
#if AD9851_ATTACHED
static volatile float s_dbg_delayed_freq_dev = 0.0f;   // post-delay-line value, actually used
static volatile uint32_t s_dbg_tx_freq = 0;             // the exact integer Hz value sent to
                                                          // ad9851_set_frequency() - ground truth

// 2026-09-10: tick-to-tick step-size high-water mark on tx_freq itself, for
// the random-TX-jump investigation (see moving_forward_notes.md's
// "new jump occurrence" entries). Unlike null_bias/weighted_bias (already
// confirmed decoupled from the real jumps) or the canary (which only
// catches corruption of s_carrier_hz/ftw_reciprocal specifically, and has
// now stayed clean across two observed jumps), this measures the actual
// ground-truth signal handed to the chip every tick, directly. If a real
// jump this size never shows up here even while one is observed at RF, the
// fault is downstream of every bit of digital math in this pipeline -
// REF_CLK, the AD9851's internal PLL, or the SPI transfer itself - not
// anything this firmware computes. s_dbg_have_prev_tx_freq guards the very
// first call after boot/reset, which has no previous tick to compare
// against.
static volatile uint32_t s_dbg_prev_tx_freq = 0;
static volatile bool s_dbg_have_prev_tx_freq = false;
static volatile uint32_t s_dbg_max_freq_dev_step_hz = 0;
static volatile uint32_t s_dbg_max_freq_dev_step_from_hz = 0;
static volatile uint32_t s_dbg_max_freq_dev_step_to_hz = 0;
static volatile uint32_t s_dbg_max_freq_dev_step_at_ms = 0;

// 2026-09-11: does a max_freq_dev_step event actually self-correct on the
// very next tick, or does tx_freq stay elevated/"stuck" for longer? The
// high-water-mark fields above can't answer that - they only ever record
// the two values straddling the single worst step, never what happens
// afterward. User's own objection: a genuine one-tick DSP transient (the
// EER/polar-transmitter null-crossing effect - see moving_forward_notes.md's
// 2026-09-11 entry) should recover automatically within one 62.5us tick,
// since freq_dev is recomputed fresh from atan2(Q,I) every sample with no
// persistent memory (the slew limiter, the one thing that WOULD carry state
// tick-to-tick, is currently off/unlimited in every preset - see
// ssb_dsp.c's freq_dev_slew_limit_hz); a real "sticks rather than blips"
// symptom (the original, much older canary-motivating theory) would not.
// This settles it empirically instead of by argument: captures this tick's
// tx_freq (the post-jump value itself) plus the next FREQ_STEP_TRACE_LEN-1
// ticks' worth, every time a NEW record-breaking step is set (re-arms and
// overwrites any still-filling older trace - only the worst event's
// aftermath matters). Printed as [dsp] post-step trace - see the print site
// near max_freq_dev_step below.
#define FREQ_STEP_TRACE_LEN 8
static volatile uint32_t s_dbg_freq_step_trace[FREQ_STEP_TRACE_LEN];
// 2026-09-12: parallel envelope capture for the trace above - added
// alongside the jump log below, same motivation (see that block's comment).
// The post-step trace could tell us tx_freq recovers within a tick, but not
// whether the jump itself happened at a near-null sample - this closes that
// gap for the SAME worst-ever event the tx_freq trace already captures.
static volatile float    s_dbg_freq_step_trace_envelope[FREQ_STEP_TRACE_LEN];
static volatile uint8_t  s_dbg_freq_step_trace_fill = 0;    // 0 = no trace captured yet this window
static volatile bool     s_dbg_freq_step_trace_armed = false;

// 2026-09-12: per-event jump LOG - a deliberately different tool from the
// high-water-mark fields and the post-step trace above, both of which only
// ever remember the SINGLE worst tx_freq step seen across an entire run.
// That's fine for "was there ever a bad one" but it means a multi-hour
// unattended capture hands back exactly one data point - not enough to
// settle whether every jump is genuinely explained by the null-crossing
// mechanism this file's whole investigation has been built around, which
// is exactly the user's own engineering doubt (moving_forward_notes.md,
// 2026-09-12: "I still have my engineering doubts that this is the only
// problem"). This keeps the last JUMP_LOG_LEN qualifying events (a lower,
// more inclusive bar than "new all-time record" - JUMP_LOG_THRESHOLD_HZ),
// each stamped with enough context to be judged individually rather than
// argued about in aggregate:
//   - envelope: envelope_at_freq_time (relative_delay.h/.cpp) - the
//     envelope value from the SAME original sample time as the (possibly
//     delayed) freq_dev value that produced this tx_freq step. 2026-09-12
//     CORRECTION, same day: this field originally used delayed_envelope
//     instead - the POST-delay envelope - reasoning that "was this
//     near-null AT THE MOMENT OF TRANSMISSION" was the physically relevant
//     question (per the relative_delay/near-null-spike interaction in
//     null_bias_investigation.md's earlier 2026-09-12 entry). That
//     reasoning was wrong in one specific way: for delay>0 (every two-tone
//     preset), envelope itself is read at zero lag inside
//     relative_delay_apply() (env_back=0), so delayed_envelope is just the
//     CURRENT tick's envelope - NOT time-matched to the freq_dev value
//     that got delayed. At large delay this can badly misclassify a
//     genuine null-crossing event as near_null=false, since the envelope
//     being compared against is from the wrong moment entirely. The very
//     first bench capture with this log (relative_delay=+4.60,
//     503384 events, 0% near_null) is likely exactly this failure mode,
//     not proof of a second mechanism - see moving_forward_notes.md's
//     2026-09-12 entry for the full reasoning and what would distinguish
//     the two.
//   - relative_delay_samples: current delay setting - lets a delay sweep
//     be correlated against jump occurrence directly, firmware-side,
//     instead of only inferring it from Aux SP behavior.
//   - busy_us: this tick's own DSP busy time - a timing-domain cause (the
//     Core-1-serial-bleed-into-Core-0 mechanism already root-caused twice
//     in this project's history) would show up here directly, as an
//     anomalous busy_us coinciding with the jump.
//   - audio_source: settles "does this also happen on mic input, not just
//     the perfectly-periodic two-tone test signal" without a separate
//     experiment - just leave 'J' logging running across a source change
//     and compare counts.
//   - near_null: envelope < ssb_dsp_get_null_bias_threshold() at capture -
//     the single most direct test of the null-crossing hypothesis. If the
//     near-null percentage across many logged events stays near 100%, that's
//     strong, repeated (not anecdotal) support for the existing theory. If a
//     meaningful fraction of events come back near_null=false, that's
//     direct, hard evidence of a SEPARATE mechanism at work.
#define JUMP_LOG_LEN 8
#define JUMP_LOG_THRESHOLD_HZ 300u   // well above ordinary in-band modulation
                                     // step sizes, well below every jump
                                     // magnitude actually observed so far
                                     // (hundreds to thousands of Hz) - lower
                                     // this if a run known to have jumps
                                     // still comes back with jump_log n=0

typedef struct {
    uint32_t at_ms;
    uint32_t from_hz;
    uint32_t to_hz;
    uint32_t step_hz;
    float    envelope;                  // envelope_at_freq_time (blended) - time-matched to from_hz/to_hz
    float    envelope_min;              // envelope_at_freq_time_min - see relative_delay.h/.cpp, 2026-09-12
    float    relative_delay_samples;
    uint32_t busy_us;                   // filled in slightly later by diagnostics_record_jump_busy_us()
    uint8_t  audio_source;              // audio_source_t, narrowed - see audio_source_name()
    // 2026-09-12: two DIFFERENT near-null questions, not the same one
    // measured twice - see relative_delay.h's out_envelope_at_freq_time_min
    // declaration comment for why a lopsided fractional delay needs both.
    bool     near_null_blended;         // envelope (the blend) < threshold - "was the RESULT near a null"
    bool     near_null_either;          // envelope_min < threshold - "did ANY contributing raw sample dip near a null"
    // 2026-09-12, later same day: the two RAW, undelayed freq_dev_hz ring
    // values interp_ring() blended to produce this event's (delayed)
    // freq_dev - see diagnostics_set_tx_info()'s declaration comment
    // (diagnostics.h) and relative_delay_apply()'s (relative_delay.h) for
    // the full reasoning. Logged for inspection, not used in any
    // near_null classification of their own.
    float    raw_freq_dev_near;
    float    raw_freq_dev_far;
} jump_log_entry_t;

static jump_log_entry_t s_jump_log[JUMP_LOG_LEN];
static uint32_t s_jump_log_write_idx = 0;
static uint32_t s_jump_log_count = 0;          // total qualifying events since boot/reset (can exceed JUMP_LOG_LEN - ring wraps)
static uint32_t s_jump_log_near_null_blended_count = 0;
static uint32_t s_jump_log_near_null_either_count = 0;
static bool           s_jump_pending = false;   // set by diagnostics_set_tx_info(), consumed by
static jump_log_entry_t s_jump_pending_entry;   // diagnostics_record_jump_busy_us() a few lines later, same tick

// 2026-09-12, yet later still: a SEPARATE, much slower-timescale trigger,
// built after the user reported (and then directly confirmed on the
// bench) that the 'J' log above is the wrong tool for "what changed to
// the frequency I can actually see/hear" - it fires on every single beat-
// null crossing (hundreds to thousands of times a second, per the
// captures logged in moving_forward_notes.md/null_bias_investigation.md's
// 2026-09-12 entries), so by the time a human reacts to an observed
// frequency shift and reads 'J', the ring has wrapped many times over
// with unrelated routine churn. Direct confirmation, not just theory: a
// capture taken deliberately right after Aux SP showed a real 1000->962Hz
// shift came back showing the EXACT SAME 3-state cycle, same values, as
// every "nothing happened" capture before it - the low-level log carries
// no signal at all about when a human-perceptible shift occurred, because
// its threshold (300Hz, per-tick) is answering a completely different,
// much smaller and much more frequent question than "did the steady-state
// frequency someone is watching just move."
//
// Approach: track two EMAs of delayed_freq_dev_hz - the same value the
// periodic [dsp] status line already reports - one "fast" (tau
// FREQ_EMA_FAST_TAU_S, chosen to be several times longer than one beat-
// null cycle's ~3-5ms period so the existing per-cycle churn averages out
// almost completely) and one much slower "reference" (tau
// FREQ_EMA_SLOW_TAU_S) that lags behind and represents "where this has
// been sitting." When they diverge by more than SLOW_JUMP_TRIGGER_HZ -
// deliberately set to the user's own independently-reported +/-5Hz
// visual-read tolerance on Aux SP (null_bias_investigation.md's
// 2026-09-12 entries), not a DSP-internal number - that's treated as a
// change a human watching the display would actually notice.
//
// On trigger, a coarse (SLOW_TRACE_BIN_TICKS-tick bins, not raw per-tick -
// keeps RAM/print-time modest while still showing the shape of the
// transition) trace spanning SLOW_TRACE_PRE_BINS bins before the trigger
// and SLOW_TRACE_POST_BINS after is LATCHED - unlike the 'J' ring, which
// keeps sliding forward forever, this one deliberately STOPS recording
// once it has an answer, specifically so a human-reaction-time delay
// before reading it (via the new 'K' command) can't erase it. Printing
// re-arms it (and resyncs the slow EMA to the fast one, so the two start
// equal again rather than immediately re-triggering while the slow EMA is
// still catching up from the just-reported event) for the next one.
// Deliberately NOT touched by diagnostics_reset()/'r' - same reasoning as
// the canary latches elsewhere in this file: a rare, significant event
// capture shouldn't silently vanish just because someone started a fresh
// routine measurement window before reading it.
typedef struct {
    float freq_mean_hz;   // mean of delayed_freq_dev_hz over the bin
    float env_min;        // min of envelope_at_freq_time over the bin - the most
                           // null-like single sample seen in that bin, same
                           // "did anything dip near a null" spirit as
                           // near_null_either above, just per-bin instead of
                           // per-blend
} slow_trace_bin_t;

#define SLOW_TRACE_BIN_TICKS 20     // 1.25ms/bin @ 16kHz - coarse enough to keep
                                    // the eventual print (PRE_BINS+POST_BINS
                                    // lines) manageable, fine enough to still
                                    // show the transition's shape against the
                                    // ~3-5ms beat-null cycle period
#define SLOW_TRACE_PRE_BINS  40    // 50ms of context before the trigger
#define SLOW_TRACE_POST_BINS 40    // 50ms captured after

#define FREQ_EMA_FAST_TAU_S 0.05f  // 50ms - several beat-null cycles' worth
#define FREQ_EMA_SLOW_TAU_S 2.0f   // 2s - deliberately much slower, so it lags
                                   // behind as "where this has been sitting"
static const float FREQ_EMA_DT_S = (float)SSB_SAMPLE_PERIOD_US / 1000000.0f;
static const float FREQ_EMA_FAST_ALPHA = FREQ_EMA_DT_S / (FREQ_EMA_FAST_TAU_S + FREQ_EMA_DT_S);
static const float FREQ_EMA_SLOW_ALPHA = FREQ_EMA_DT_S / (FREQ_EMA_SLOW_TAU_S + FREQ_EMA_DT_S);
#define SLOW_JUMP_TRIGGER_HZ 5.0f  // matches the user's own reported +/-5Hz
                                   // visual-read tolerance on Aux SP - see
                                   // this block's header comment

// 2026-09-12, yet later still: FIRST REAL 'K' CAPTURE turned out to be a
// false positive, caused by the EMA seeding itself, not a real event -
// before=697.40Hz after=704.63Hz (delta=+7.23Hz) with only 3 pre-bins
// filled (i.e. this fired within ~4ms of boot/reset) and a post-trace
// showing the exact same repeating cycle (800/800/800/400Hz, period 4
// bins) both before AND after the "trigger," with no visible transition
// anywhere in it. Root cause: both EMAs seed from a single RAW
// (unaveraged) sample on the very first tick - if that sample happens to
// land on one extreme of the ongoing periodic churn (800Hz here, not the
// cycle's ~700Hz time-average), the FAST EMA (tau=50ms) converges toward
// the true average within tens of ms while the SLOW EMA (tau=2s) is still
// sitting almost exactly at the biased seed value - diverging by more
// than SLOW_JUMP_TRIGGER_HZ almost immediately, from initialization bias
// alone, regardless of whether anything real happened. A resync at re-arm
// (diagnostics_print_slow_trace()) has the same exposure in miniature -
// the fast EMA it snaps the slow one to is itself only tau=50ms smoothed,
// so it can still be offset from the true multi-second average right
// after a busy cycle.
//
// FIRST FIX ATTEMPT (an 8s, ~4x-slow-tau warm-up hold on the trigger
// check) turned out to be UNDERSIZED, not wrong in kind - a follow-up
// capture at t=9239ms (i.e. AFTER that 8s hold had already lifted) still
// fired, and its new TREND block (added for exactly this reason) showed
// why directly: `fast` was already rock-steady at 699.07-699.08Hz for the
// entire visible 2s history, while `slow` was still climbing smoothly and
// monotonically (666.23 -> 687.31Hz over that same 2s, a textbook
// exponential settling curve) - i.e. still visibly converging from its
// boot seed value nearly 9.2s in. Extrapolating that curve back
// implies a seed value hundreds to over a thousand Hz away from the true
// ~699Hz average - entirely plausible given this project's own
// well-documented near-null freq_dev spikes (thousands of Hz, see the
// 'J' captures elsewhere in this file) landing on the single raw sample
// used to seed both EMAs. The "4 tau -> ~98% converged" heuristic behind
// the original 8s figure assumed a "reasonably-sized" initial error - it
// doesn't hold when the seed itself can be a thousand-Hz outlier, where
// even a small residual PERCENTAGE is still tens of Hz.
//
// REAL fix: stop seeding the slow EMA from a raw sample at all. Seed only
// the FAST EMA that way (its own short tau, ~50ms, makes it converge to
// the true running average almost immediately regardless of what the
// seed was), let it run alone for FREQ_EMA_BOOT_SETTLE_MS (~10x its own
// tau - by then it's converged from even a large seed error to within a
// small fraction of a percent), THEN snap slow = fast's already-converged
// value - never seeding slow from a potentially-extreme raw sample in the
// first place, rather than trying to out-wait an error whose size was
// never bounded to begin with. A short residual FREQ_EMA_WARMUP_MS hold
// on the trigger check remains afterward, purely as insurance (e.g.
// against the snap instant itself landing mid-spike) - now starting from
// an already-good value instead of a raw one, so it only needs to cover
// ordinary EMA noise, not an unbounded seed error.
#define FREQ_EMA_BOOT_SETTLE_MS 500.0f   // ~10x FREQ_EMA_FAST_TAU_S
static const uint32_t FREQ_EMA_BOOT_SETTLE_TICKS =
    (uint32_t)(FREQ_EMA_BOOT_SETTLE_MS * 1000.0f / (float)SSB_SAMPLE_PERIOD_US);
#define FREQ_EMA_WARMUP_MS 8000.0f
static const uint32_t FREQ_EMA_WARMUP_TICKS =
    (uint32_t)(FREQ_EMA_WARMUP_MS * 1000.0f / (float)SSB_SAMPLE_PERIOD_US);

static float    s_freq_ema_fast_hz = 0.0f;
static float    s_freq_ema_slow_hz = 0.0f;
static bool     s_freq_ema_fast_inited = false;   // fast EMA seeded from the first-ever raw sample
static bool     s_freq_ema_slow_inited = false;   // slow EMA snapped from fast after the boot settle
static uint32_t s_freq_ema_boot_settle_ticks_left = 0;   // counts down FREQ_EMA_BOOT_SETTLE_TICKS
                                                          // before the slow-EMA snap happens
static uint32_t s_freq_ema_warmup_ticks_left = 0;   // set to FREQ_EMA_WARMUP_TICKS once slow is
                                                     // snapped (boot) or resynced (re-arm) -
                                                     // trigger check is held off while nonzero

typedef enum { SLOW_TRACE_WATCHING = 0, SLOW_TRACE_CAPTURING_POST, SLOW_TRACE_LATCHED } slow_trace_state_t;
static slow_trace_state_t s_slow_trace_state = SLOW_TRACE_WATCHING;

static slow_trace_bin_t s_slow_pre_ring[SLOW_TRACE_PRE_BINS];
static uint32_t s_slow_pre_write_idx = 0;
static uint32_t s_slow_pre_fill = 0;   // like jump_log_count's own "have we wrapped yet"

static slow_trace_bin_t s_slow_post_bins[SLOW_TRACE_POST_BINS];
static uint32_t s_slow_post_fill = 0;

static float    s_slow_bin_sum_freq = 0.0f;
static float    s_slow_bin_min_env = 1e9f;
static uint32_t s_slow_bin_count = 0;

static uint32_t s_slow_trigger_at_ms = 0;
static float    s_slow_trigger_before_hz = 0.0f;   // slow (reference) EMA at the trigger instant
static float    s_slow_trigger_after_hz = 0.0f;    // fast EMA once the post capture completes
static float    s_slow_trigger_delta_hz = 0.0f;
static float    s_slow_trigger_relative_delay = 0.0f;
static uint32_t s_slow_trigger_delay_change_ms = 0;   // relative_delay_get_last_change_ms() at
                                                       // the trigger instant - see that
                                                       // function's declaration comment
                                                       // (relative_delay.h)

// 2026-09-12, yet later still: a SECOND, much-longer-timescale companion
// to the fine (1.25ms/bin) trace above - added after TWO consecutive real
// (non-boot-artifact) 'K' captures, one user-flagged "[] scan induced"
// and one flagged "spontaneous," BOTH came back showing a perfectly
// steady, non-drifting repeating cycle throughout their entire 50ms
// pre+50ms post window, with no visible transition anywhere - despite a
// genuine 8-11Hz fast/slow EMA gap having triggered them. If the signal
// were truly unchanging for as long as it's been running, a periodic
// waveform this regular (both EMAs' cutoffs sit far below the ~200Hz
// cycle-repeat rate) should long since have pulled BOTH EMAs to within
// a fraction of a Hz of the true periodic mean - an 8-11Hz gap that
// persists despite a locally flat 100ms window is best explained by a
// REAL change that happened, and fully resolved, on a timescale longer
// than 50ms but shorter than the slow EMA's ~2s memory - i.e. a
// continuous drift too gradual to show any visible slope over 50ms, but
// fast enough to separate a 50ms average from a 2s one. (Direct
// precedent: the +3.85 capture, this file's first genuinely real one,
// already showed this project's cycle mean drifting continuously over
// its own 50ms window - this is the same phenomenon at whatever slower
// rate applies at THIS delay/pair, now inferred rather than directly
// seen because it's too slow for the fine trace to resolve.)
//
// Rather than growing the fine trace's own span to multiple seconds
// (expensive - RAM and, more so, print volume - and it would just push
// the same "still not long enough" edge case further out), this instead
// periodically snapshots the two EMAs THEMSELVES (not raw bin data) over
// a much longer horizon (EMA_TREND_LEN samples, EMA_TREND_SAMPLE_TICKS
// apart) - directly answering "was fast/slow already diverging smoothly
// over the last couple of seconds" without needing fine per-tick detail
// over that whole span. Freezes the same way the pre-bin ring does -
// simply stops advancing once state leaves WATCHING - rather than an
// explicit copy-out.
#define EMA_TREND_LEN 80              // 80 samples
#define EMA_TREND_SAMPLE_TICKS 400    // 25ms/sample @ 16kHz -> 2s of total history,
                                       // matching FREQ_EMA_SLOW_TAU_S itself
static float    s_trend_fast_hz[EMA_TREND_LEN];
static float    s_trend_slow_hz[EMA_TREND_LEN];
static uint32_t s_trend_write_idx = 0;
static uint32_t s_trend_fill = 0;
static uint32_t s_trend_tick_count = 0;
#endif

static volatile uint32_t s_dbg_max_busy_us = 0;
static volatile uint32_t s_dbg_overrun_count = 0;
static const uint32_t k_sample_period_us = SSB_SAMPLE_PERIOD_US;

// WAKE-UP jitter - distinct from s_dbg_max_busy_us above, which only
// measures how long dsp_task's OWN work takes once it resumes. This
// measures the actual observed gap between successive ulTaskNotifyTake()
// returns - i.e. was dsp_task woken up ON TIME, regardless of how fast
// its own processing was. A task can have comfortable busy_us margin
// every single tick and STILL be intermittently woken late (preempted,
// scheduling delay, etc.) - that wouldn't show up in busy_us at all, but
// would still starve anything timing-sensitive that assumes a strictly
// periodic tick, like the ADC FIFO drain rate.
static volatile uint32_t s_dbg_max_tick_gap_us = 0;      // worst observed inter-tick gap
static volatile uint32_t s_dbg_late_tick_count = 0;      // ticks where the gap exceeded 1.5x nominal

// "Is that long [core1]/[timing]/[adc]/[dsp] print block sent in one go?"
// - yes, from the CPU's side: print_timing_and_adc_block() below is ~10
// back-to-back Serial.printf() calls with no yield in between, so it's one
// uninterrupted burst of loop()-context code every ~1s. Whether that maps
// to one blocking USB-CDC transaction depends on driver/buffer internals
// we can't see from here (and if Serial.printf's underlying write ever
// takes a portENTER_CRITICAL-style path while the buffer's full, that
// would mask interrupts up to gptimer's own intr_priority=3 level for
// however long it blocks - structurally the same contention mechanism the
// ADC ISR fix addressed, just via USB/Serial instead of the ADC driver).
// s_core1_busy_diag_us (below) already sums this block's cost, but only
// as a percentage of a 1s window - an occasional multi-ms stall could be
// hiding inside a small-looking average. This tracks the WORST single
// call instead, to catch that directly. Same single-writer-from-Core-1
// reasoning as the other diag statics - only ever touched from
// diagnostics_service()'s own context (print, measure, and reset all run
// on Core 1), no lock needed.
static volatile uint32_t s_dbg_max_diag_block_us = 0;   // worst single print_timing_and_adc_block() call

// CONFIRMED on real hardware: max_single_call_us came back at 5041 - a
// 5ms+ stall, once/sec, on Core 1 (the same core gptimer's alarm ISR runs
// on). That's ~80 sample periods' worth of time in one call - more than
// enough on its own to explain the observed pin5 bad edges, whether the
// mechanism is literal interrupt masking during a blocked USB-CDC write,
// or something else in that path. Root cause of the block itself (why
// Serial.printf() would stall that long) is presumably the host not
// draining the USB-CDC endpoint promptly - plausibly WORSE while you're
// actively typing (terminal app busy handling keystrokes/redraws instead
// of servicing the port), which fits the "correlates with serial
// activity" observation even though this specific block fires on a timer,
// not on keypresses. Fix: never let a diagnostic print block for that
// long - see diag_room_for() (just above print_status_line()) for the
// per-line guard this settled on, after a first attempt (one upfront
// check for the whole ~1.5KB block) turned out to be miscalibrated - real
// hardware showed availableForWrite() never reporting anywhere near that
// much free even at rest (avail=162 observed), so that version skipped
// EVERY cycle rather than just genuinely backlogged ones.
static volatile uint32_t s_dbg_diag_block_skip_count = 0;   // individual lines skipped by diag_room_for()

// Phase breakdown of the same total: which part of dsp_task's work is
// actually costing the most.
static volatile uint32_t s_dbg_max_adc_us = 0;
static volatile uint32_t s_dbg_max_dsp_us = 0;
static volatile uint32_t s_dbg_max_write_us = 0;

static int64_t s_dsp_tick_start_us = 0;         // captured once at gptimer_start(), and again on reset
static volatile uint32_t s_dbg_dsp_tick_count = 0;  // incremented once per dsp_task tick, unconditionally

static volatile bool s_diag_muted = false;

// 2026-09-09: canary latches for the random-TX-jump investigation (see
// moving_forward_notes.md) - 0 means "never seen a mismatch since boot/
// reset", any other value is the esp_timer millis() timestamp of the FIRST
// mismatch seen, kept even if a later read happens to match again (a
// glitch this fast could conceivably self-correct on a later corruption
// event before anyone looks) - same high-water-mark philosophy as
// s_dbg_max_busy_us etc. elsewhere in this file.
static volatile uint32_t s_dbg_canary_carrier_bad_since_ms = 0;
static volatile uint32_t s_dbg_canary_ftw_bad_since_ms = 0;

// 2026-09-11: same latch pattern, extended to the IIR filters' own
// feedback state (see ssb_dsp_get_iir_canary()/envelope_gdeq_get_canary()/
// envelope_ampeq_get_canary()/adc_capture_get_lpf_canary()'s doc comments,
// and moving_forward_notes.md's matching entry, for why these specifically
// - unlike freq_dev_hz/phase, which are recomputed fresh every tick, an
// IIR filter's own y1/y2/z1/z2/env state can carry a bad (NaN/Inf) value
// forward indefinitely once introduced). One latch per MODULE rather than
// per individual float/stage - keeps this list from growing to nine
// separate high-water marks for what would functionally be "the same
// canary, checked in five different places"; canary_print_status() names
// exactly which sub-state was bad when it prints the detail, the latch
// timestamp itself doesn't need that granularity.
static volatile uint32_t s_dbg_canary_eq_bad_since_ms = 0;
static volatile uint32_t s_dbg_canary_comp_bad_since_ms = 0;
static volatile uint32_t s_dbg_canary_gdeq_bad_since_ms = 0;
static volatile uint32_t s_dbg_canary_ampeq_bad_since_ms = 0;
static volatile uint32_t s_dbg_canary_adclpf_bad_since_ms = 0;

// Core 1 headroom - see the Fs jitter hunt's crosscore-wake finding: the
// ~1us->6us stretch on gptimer's notify-from-ISR call only happens when
// dsp_task was genuinely blocked, and is the cost of the crosscore IPI
// needed to wake a Core-0-pinned task from a Core-1 ISR. The structural
// fix (co-locate the ISR and the task on one core) already crashed once
// moving the TIMER onto Core 0, which had zero spare CPU. This
// measurement is what cleared the mirror option - moving dsp_task itself
// onto Core 1 - as worth trying instead of guessing blind: once delay(10)
// was correctly attributed (see diagnostics_record_core1_loop_timings()'s
// header comment), Core 1 (hosting loop(), Serial/USB CDC,
// adc_capture_service(), and the ADC's own on_conv_done ISR) turned out
// to be sitting ~98-99% idle, not the ~3% the uncorrected idle-hook
// reading suggested - comfortable headroom by the CPU-time math for
// dsp_task's own ~45-75% duty cycle. TRIED anyway, REVERTED: real
// hardware starved Serial completely (output AND commands, not just
// delayed) despite that headroom - so CPU-time budget alone isn't the
// whole story for whatever makes this core-sharing arrangement fail; see
// the .ino's "TRIED, REVERTED" note on dsp_task's xTaskCreatePinnedToCore()
// call. dsp_task is back on Core 0; this idle/breakdown reading is still
// the right one to watch if that mirror option gets revisited once the
// actual starvation mechanism is understood.
//
// esp_register_freertos_idle_hook_for_cpu() calls core1_idle_hook() every
// time IDLE1 actually gets scheduled - i.e. only when Core 1 genuinely
// has nothing else ready to run (idle priority is the lowest there is,
// so nothing here can preempt real work). Delta-sum-with-threshold: the
// gap between two consecutive calls is either IDLE1's own tight-loop
// overhead (small, uninterrupted - genuine idle time, count it) or
// something else ran on Core 1 in between (large gap - NOT idle, must be
// excluded rather than mis-counted as spare budget). The threshold just
// needs to sit comfortably above the hook's own call-to-call overhead
// (expected well under 1us) and comfortably below any real task/ISR
// activity worth caring about.
//
// Both statics are touched ONLY from Core 1 (the hook itself, and the
// [core1] print/reset below, both run on Core 1 - the hook can never
// preempt the print since idle is the lowest priority) - no lock needed,
// same single-core-ownership reasoning as the ADC FIFO's head/tail split.
#define CORE1_IDLE_GAP_THRESHOLD_US 10
static uint64_t s_core1_idle_us_accum = 0;
static int64_t  s_core1_idle_last_call_us = 0;
static bool     s_core1_idle_hook_registered = false;

// Threshold-free cross-check on the idle% accounting above: a plain
// count of every hook call, no gap filtering at all. CORE1_IDLE_GAP_
// THRESHOLD_US was a guess, not calibrated against this actual hardware/
// IDF build - if idle% ever comes back suspiciously low (can't be
// explained by the other measured categories), a call rate that's also
// very low corroborates "Core 1 really is that busy"; a call rate that's
// still substantial while idle_us reads low would instead point at the
// threshold itself silently discarding real idle gaps that are just
// wider than 10us (e.g. if IDLE1's own per-iteration housekeeping on
// this IDF version costs more than assumed).
static uint32_t s_core1_idle_hook_calls = 0;

// Core 1 BREAKDOWN - "where does that ~96-97% busy time actually go".
// Sums (not high-water marks - see diagnostics_record_core1_loop_timings()'s
// own header comment for why), one per loop() sub-call, plus idle above.
// Same single-core-ownership reasoning as the idle accumulator - only
// ever touched from Core 1 (loop()'s own context), no lock needed.
static uint64_t s_core1_busy_cmd_us     = 0;   // handle_serial_commands()
static uint64_t s_core1_busy_adc_svc_us = 0;   // adc_capture_service()
static uint64_t s_core1_busy_diag_us    = 0;   // diagnostics_service() itself (mostly the
                                                // throttled [timing]/[adc]/[dsp] printf block)
static uint64_t s_core1_busy_delay_us   = 0;   // loop()'s own delay(10) - see
                                                // diagnostics_record_core1_loop_timings()'s
                                                // header comment for why this one bucket
                                                // turned out to explain the whole "other"
                                                // mystery

// esp_freertos_idle_cb_t is bool(*)(void), not void(*)(void) - confirmed
// on real hardware (this project's exact esp32s3-libs build rejected the
// void signature outright, -fpermissive error). Return value isn't ours
// to interpret here - this hook is just accumulating a measurement, not
// influencing idle-task behavior (light sleep, WDT feeding, etc., which
// are handled elsewhere) - true is the safe, do-nothing-special choice.
static bool IRAM_ATTR core1_idle_hook(void)
{
    s_core1_idle_hook_calls++;   // unconditional - the threshold-free cross-check, see its own comment
    int64_t now = esp_timer_get_time();
    if (s_core1_idle_last_call_us != 0) {
        int64_t gap = now - s_core1_idle_last_call_us;
        if (gap > 0 && gap <= CORE1_IDLE_GAP_THRESHOLD_US) {
            s_core1_idle_us_accum += (uint64_t)gap;
        }
    }
    s_core1_idle_last_call_us = now;
    return true;
}

void IRAM_ATTR diagnostics_record_tick_start(int64_t t_start_us)
{
    s_dbg_dsp_tick_count++;   // unconditional - counts real elapsed ticks regardless of mode

    // Measured first thing each tick, so it reflects the true wake-up-to-
    // wake-up gap rather than anything downstream.
    static int64_t s_last_tick_start_us = 0;
    if (s_last_tick_start_us != 0) {
        uint32_t gap_us = (uint32_t)(t_start_us - s_last_tick_start_us);
        if (gap_us > s_dbg_max_tick_gap_us) s_dbg_max_tick_gap_us = gap_us;
        if (gap_us > (k_sample_period_us + k_sample_period_us / 2)) s_dbg_late_tick_count++;
    }
    s_last_tick_start_us = t_start_us;
}

void IRAM_ATTR diagnostics_record_phase_timings(uint32_t adc_us, uint32_t dsp_us,
                                                 uint32_t write_us, uint32_t busy_us)
{
    if (adc_us   > s_dbg_max_adc_us)   s_dbg_max_adc_us   = adc_us;
    if (dsp_us   > s_dbg_max_dsp_us)   s_dbg_max_dsp_us   = dsp_us;
    if (write_us > s_dbg_max_write_us) s_dbg_max_write_us = write_us;
    if (busy_us  > s_dbg_max_busy_us)  s_dbg_max_busy_us  = busy_us;
    if (busy_us  > k_sample_period_us) s_dbg_overrun_count++;
}

void diagnostics_record_core1_loop_timings(uint32_t cmd_us, uint32_t adc_svc_us,
                                            uint32_t diag_us, uint32_t delay_us)
{
    s_core1_busy_cmd_us     += cmd_us;
    s_core1_busy_adc_svc_us += adc_svc_us;
    s_core1_busy_diag_us    += diag_us;
    s_core1_busy_delay_us   += delay_us;
}

void IRAM_ATTR diagnostics_set_envelope_freqdev(float envelope, float freq_dev_hz)
{
    s_dbg_envelope = envelope;
    s_dbg_freq_dev = freq_dev_hz;
}

void IRAM_ATTR diagnostics_set_tx_info(float delayed_freq_dev_hz, float delayed_envelope,
                                        float envelope_at_freq_time, float envelope_at_freq_time_min,
                                        float raw_freq_dev_near, float raw_freq_dev_far,
                                        uint32_t tx_freq)
{
#if AD9851_ATTACHED
    s_dbg_delayed_freq_dev = delayed_freq_dev_hz;
    s_dbg_tx_freq = tx_freq;
    (void)delayed_envelope;   // stored nowhere yet - kept for a future post-delay null_bias variant, see header

    // 2026-09-12, yet later still: slow-mean trigger - see its declaration
    // comment (this file, just above the struct/statics it uses) for the
    // full "the horse has bolted, and it's not even close" motivation.
    // Runs unconditionally every tick, independent of the >300Hz-step
    // jump log below and of s_dbg_have_prev_tx_freq (this doesn't need a
    // "previous tick" in that sense - the EMAs seed themselves from the
    // very first sample).
    {
        float dev = delayed_freq_dev_hz;
        if (!s_freq_ema_fast_inited) {
            // Fast EMA seeds from the first-ever raw sample, same as
            // before - but its own short tau converges it to the true
            // running average within a few tens of ms regardless of how
            // far off that raw seed was (see FREQ_EMA_BOOT_SETTLE_MS's
            // declaration comment). This starts the boot-settle countdown
            // that decides when slow is allowed to snap from it.
            s_freq_ema_fast_hz = dev;
            s_freq_ema_fast_inited = true;
            s_freq_ema_boot_settle_ticks_left = FREQ_EMA_BOOT_SETTLE_TICKS;
        } else {
            s_freq_ema_fast_hz += FREQ_EMA_FAST_ALPHA * (dev - s_freq_ema_fast_hz);
        }

        if (!s_freq_ema_slow_inited) {
            // Slow EMA deliberately does NOT update from raw samples
            // during this window - it has nothing valid to converge from
            // yet. Once the boot-settle countdown reaches zero, fast has
            // had ~10 of its own time constants to converge from
            // whatever the raw seed was - snap slow to that, a clean
            // transfer instead of a raw single-sample seed, then arm the
            // short residual warmup before the trigger check is trusted.
            if (s_freq_ema_boot_settle_ticks_left > 0) s_freq_ema_boot_settle_ticks_left--;
            if (s_freq_ema_boot_settle_ticks_left == 0) {
                s_freq_ema_slow_hz = s_freq_ema_fast_hz;
                s_freq_ema_slow_inited = true;
                s_freq_ema_warmup_ticks_left = FREQ_EMA_WARMUP_TICKS;
            }
        } else {
            s_freq_ema_slow_hz += FREQ_EMA_SLOW_ALPHA * (dev - s_freq_ema_slow_hz);
            if (s_freq_ema_warmup_ticks_left > 0) s_freq_ema_warmup_ticks_left--;
        }

        // Long-timescale EMA trend ring - see its own declaration comment
        // (this file, near jump_log_entry_t) for why. Deliberately keyed
        // off SLOW_TRACE_WATCHING AND slow being initialized, same
        // freeze-by-stop-writing pattern as the fine pre-bin ring just
        // below, so it holds exactly the run-up to whichever trigger just
        // fired, and never records a meaningless pre-snap slow value.
        if (s_slow_trace_state == SLOW_TRACE_WATCHING && s_freq_ema_slow_inited) {
            s_trend_tick_count++;
            if (s_trend_tick_count >= EMA_TREND_SAMPLE_TICKS) {
                s_trend_tick_count = 0;
                s_trend_fast_hz[s_trend_write_idx] = s_freq_ema_fast_hz;
                s_trend_slow_hz[s_trend_write_idx] = s_freq_ema_slow_hz;
                s_trend_write_idx = (s_trend_write_idx + 1) % EMA_TREND_LEN;
                if (s_trend_fill < EMA_TREND_LEN) s_trend_fill++;
            }
        }

        if (s_slow_trace_state != SLOW_TRACE_LATCHED) {
            s_slow_bin_sum_freq += dev;
            if (envelope_at_freq_time < s_slow_bin_min_env) s_slow_bin_min_env = envelope_at_freq_time;
            s_slow_bin_count++;

            if (s_slow_bin_count >= SLOW_TRACE_BIN_TICKS) {
                slow_trace_bin_t bin;
                bin.freq_mean_hz = s_slow_bin_sum_freq / (float)s_slow_bin_count;
                bin.env_min = s_slow_bin_min_env;
                s_slow_bin_sum_freq = 0.0f;
                s_slow_bin_min_env = 1e9f;
                s_slow_bin_count = 0;

                if (s_slow_trace_state == SLOW_TRACE_WATCHING) {
                    s_slow_pre_ring[s_slow_pre_write_idx] = bin;
                    s_slow_pre_write_idx = (s_slow_pre_write_idx + 1) % SLOW_TRACE_PRE_BINS;
                    if (s_slow_pre_fill < SLOW_TRACE_PRE_BINS) s_slow_pre_fill++;

                    // Trigger check held off until slow is actually
                    // initialized (the boot-settle snap, see
                    // FREQ_EMA_BOOT_SETTLE_TICKS) AND the short residual
                    // warmup since then has elapsed (FREQ_EMA_WARMUP_TICKS)
                    // - see both constants' declaration comments for the
                    // two-stage false-positive history this replaced. The
                    // pre-ring above keeps filling throughout regardless,
                    // so real history is already available the moment the
                    // hold lifts.
                    float delta = s_freq_ema_fast_hz - s_freq_ema_slow_hz;
                    if (delta < 0.0f) delta = -delta;
                    if (s_freq_ema_slow_inited && s_freq_ema_warmup_ticks_left == 0
                        && delta > SLOW_JUMP_TRIGGER_HZ) {
                        s_slow_trigger_at_ms = (uint32_t)(esp_timer_get_time() / 1000);
                        s_slow_trigger_before_hz = s_freq_ema_slow_hz;
                        s_slow_trigger_delta_hz = s_freq_ema_fast_hz - s_freq_ema_slow_hz;
                        s_slow_trigger_relative_delay = relative_delay_get_samples();
                        s_slow_trigger_delay_change_ms = relative_delay_get_last_change_ms();
                        s_slow_post_fill = 0;
                        s_slow_trace_state = SLOW_TRACE_CAPTURING_POST;
                    }
                } else {   // SLOW_TRACE_CAPTURING_POST
                    if (s_slow_post_fill < SLOW_TRACE_POST_BINS) {
                        s_slow_post_bins[s_slow_post_fill++] = bin;
                    }
                    if (s_slow_post_fill >= SLOW_TRACE_POST_BINS) {
                        s_slow_trigger_after_hz = s_freq_ema_fast_hz;
                        s_slow_trace_state = SLOW_TRACE_LATCHED;
                    }
                }
            }
        }
    }

    // See s_dbg_max_freq_dev_step_hz's own declaration comment. tx_freq is
    // this tick's ground-truth Hz value (carrier_output.h's own doc
    // comment) - comparing it against last tick's value catches any
    // one-tick digital discontinuity directly, no matter which upstream
    // stage (freq_dev_hz, the delay line, or the carrier addition) it came
    // from. esp_timer_get_time() (not millis()) since this runs on the
    // dsp_task hot path - it's the IRAM-safe, ISR-callable timer read this
    // codebase already relies on elsewhere.
    if (s_dbg_have_prev_tx_freq) {
        int32_t step = (int32_t)tx_freq - (int32_t)s_dbg_prev_tx_freq;
        uint32_t abs_step = (step < 0) ? (uint32_t)(-step) : (uint32_t)step;
        if (abs_step > s_dbg_max_freq_dev_step_hz) {
            s_dbg_max_freq_dev_step_hz = abs_step;
            s_dbg_max_freq_dev_step_from_hz = s_dbg_prev_tx_freq;
            s_dbg_max_freq_dev_step_to_hz = tx_freq;
            s_dbg_max_freq_dev_step_at_ms = (uint32_t)(esp_timer_get_time() / 1000);
            // Re-arm the post-step trace - see its declaration comment
            // above. fill=0 first, then the unconditional capture just
            // below runs this same call, so trace[0] ends up holding THIS
            // tick's (the post-jump) tx_freq value.
            s_dbg_freq_step_trace_fill = 0;
            s_dbg_freq_step_trace_armed = true;
        }
        if (s_dbg_freq_step_trace_armed) {
            s_dbg_freq_step_trace_envelope[s_dbg_freq_step_trace_fill] = envelope_at_freq_time;
            s_dbg_freq_step_trace[s_dbg_freq_step_trace_fill++] = tx_freq;
            if (s_dbg_freq_step_trace_fill >= FREQ_STEP_TRACE_LEN) {
                s_dbg_freq_step_trace_armed = false;
            }
        }

        // 2026-09-12: per-event jump log - see its declaration comment
        // (top of file) for full rationale. Deliberately independent of,
        // and a lower bar than, the "new all-time record" gate above, so
        // ordinary RECURRING jumps get captured too, not just a single
        // once-per-run worst case. busy_us isn't known yet at this point
        // in the tick - stashed as "pending" and finalized a few lines
        // later in ssb_mic_test.ino by diagnostics_record_jump_busy_us(),
        // once this tick's busy_us has actually been computed.
        if (abs_step > JUMP_LOG_THRESHOLD_HZ) {
            s_jump_pending_entry.at_ms = (uint32_t)(esp_timer_get_time() / 1000);
            s_jump_pending_entry.from_hz = s_dbg_prev_tx_freq;
            s_jump_pending_entry.to_hz = tx_freq;
            s_jump_pending_entry.step_hz = abs_step;
            s_jump_pending_entry.envelope = envelope_at_freq_time;
            s_jump_pending_entry.envelope_min = envelope_at_freq_time_min;
            s_jump_pending_entry.relative_delay_samples = relative_delay_get_samples();
            s_jump_pending_entry.audio_source = (uint8_t)dsp_state_get_audio_source();
            s_jump_pending_entry.raw_freq_dev_near = raw_freq_dev_near;
            s_jump_pending_entry.raw_freq_dev_far = raw_freq_dev_far;
            {
                float thr = ssb_dsp_get_null_bias_threshold(dsp_state_get_ssb());
                s_jump_pending_entry.near_null_blended = (envelope_at_freq_time < thr);
                s_jump_pending_entry.near_null_either  = (envelope_at_freq_time_min < thr);
            }
            s_jump_pending_entry.busy_us = 0;   // filled in shortly - see above
            s_jump_pending = true;
        }
    }
    s_dbg_prev_tx_freq = tx_freq;
    s_dbg_have_prev_tx_freq = true;
#else
    (void)delayed_freq_dev_hz;
    (void)delayed_envelope;
    (void)envelope_at_freq_time;
    (void)envelope_at_freq_time_min;
    (void)raw_freq_dev_near;
    (void)raw_freq_dev_far;
    (void)tx_freq;
#endif
}

void IRAM_ATTR diagnostics_record_jump_busy_us(uint32_t busy_us)
{
#if AD9851_ATTACHED
    // No-op on every ordinary tick - only ever true in the same tick
    // diagnostics_set_tx_info() just flagged a qualifying jump, a few
    // lines earlier in ssb_mic_test.ino. dsp_task runs single-threaded per
    // tick, so there's no re-entrancy risk between the two calls.
    if (!s_jump_pending) return;
    s_jump_pending_entry.busy_us = busy_us;
    s_jump_log[s_jump_log_write_idx] = s_jump_pending_entry;
    s_jump_log_write_idx = (s_jump_log_write_idx + 1) % JUMP_LOG_LEN;
    s_jump_log_count++;
    if (s_jump_pending_entry.near_null_blended) s_jump_log_near_null_blended_count++;
    if (s_jump_pending_entry.near_null_either)  s_jump_log_near_null_either_count++;
    s_jump_pending = false;
#else
    (void)busy_us;
#endif
}

void diagnostics_print_jump_log(void)
{
#if AD9851_ATTACHED
    uint32_t n = (s_jump_log_count < JUMP_LOG_LEN) ? s_jump_log_count : JUMP_LOG_LEN;
    // Two near-null percentages, deliberately - see near_null_blended/
    // near_null_either's declaration comments (jump_log_entry_t above) and
    // relative_delay.h's out_envelope_at_freq_time_min comment for why a
    // lopsided fractional delay can make these read very differently, and
    // why "either" (not just "blended") is the more complete answer to
    // "did a near-null sample contribute to this jump at all".
    Serial.printf("[dsp] jump_log: %u qualifying step(s) >%uHz since last reset, %u near-null-blended (%.0f%%), "
                  "%u near-null-either (%.0f%%) - ring holds the last %u\r\n",
                  s_jump_log_count, (unsigned)JUMP_LOG_THRESHOLD_HZ,
                  s_jump_log_near_null_blended_count,
                  s_jump_log_count ? (100.0f * (float)s_jump_log_near_null_blended_count / (float)s_jump_log_count) : 0.0f,
                  s_jump_log_near_null_either_count,
                  s_jump_log_count ? (100.0f * (float)s_jump_log_near_null_either_count / (float)s_jump_log_count) : 0.0f,
                  n);
    if (n == 0) return;

    // If the ring hasn't wrapped yet (count < LEN), the oldest entry is
    // simply index 0. Once it has wrapped, the oldest SURVIVING entry is
    // whatever the write index is about to overwrite next.
    uint32_t start = (s_jump_log_count < JUMP_LOG_LEN) ? 0 : s_jump_log_write_idx;
    for (uint32_t k = 0; k < n; k++) {
        jump_log_entry_t *e = &s_jump_log[(start + k) % JUMP_LOG_LEN];
        Serial.printf("[dsp]   jump[%u]: t=%ums %u->%uHz (step=%uHz) env=%.3f/%.3f%s%s delay=%+.2f "
                      "busy_us=%u src=%s\r\n",
                      (unsigned)k, e->at_ms, e->from_hz, e->to_hz, e->step_hz, e->envelope, e->envelope_min,
                      e->near_null_blended ? " NEAR_NULL" : "",
                      (e->near_null_either && !e->near_null_blended) ? " NEAR_NULL(either)" : "",
                      e->relative_delay_samples, e->busy_us,
                      audio_source_name(e->audio_source));
        // 2026-09-12, later same day: the two RAW (undelayed) freq_dev_hz
        // values this event's delayed step was interpolated from, plus
        // their own delta - see diagnostics_set_tx_info()'s declaration
        // comment (diagnostics.h) for why. A raw_delta close to step_hz
        // means the discontinuity already exists in the raw signal (a real
        // event, just not one the envelope-near-null test flags); a
        // raw_delta much smaller than step_hz means this was mostly an
        // interpolation artifact from blending across a large lag.
        float raw_delta = e->raw_freq_dev_near - e->raw_freq_dev_far;
        if (raw_delta < 0.0f) raw_delta = -raw_delta;
        Serial.printf("[dsp]     jump[%u] raw_freq_dev: near=%.1fHz far=%.1fHz raw_delta=%.1fHz\r\n",
                      (unsigned)k, e->raw_freq_dev_near, e->raw_freq_dev_far, raw_delta);
    }
#endif
}

// 2026-09-12, yet later still: 'K' serial command - see the slow-mean-
// trigger struct/statics' own declaration comment (above, near
// jump_log_entry_t) for the full motivation and design. Prints either a
// live "still watching" readout (nothing has crossed SLOW_JUMP_TRIGGER_HZ
// yet) or the full latched pre/post trace (it has), then re-arms for the
// next event - see the header comment for why re-arming also resyncs the
// slow EMA to the fast one.
void diagnostics_print_slow_trace(void)
{
#if AD9851_ATTACHED
    if (s_slow_trace_state != SLOW_TRACE_LATCHED) {
        // Two distinct holds, reported separately so it's never ambiguous
        // which one (if either) is currently blocking a trigger:
        // boot-settle (slow not snapped from fast yet at all - only ever
        // nonzero once, right after boot) and the short residual warmup
        // after slow IS initialized (also re-armed after every trigger is
        // read). See FREQ_EMA_BOOT_SETTLE_TICKS/FREQ_EMA_WARMUP_TICKS's
        // declaration comments for why both exist.
        if (!s_freq_ema_slow_inited) {
            float settle_ms_left = (float)s_freq_ema_boot_settle_ticks_left * (float)SSB_SAMPLE_PERIOD_US / 1000.0f;
            Serial.printf("[dsp] slow_trace: still watching - slow EMA not yet initialized "
                          "(boot_settle_left=%.0fms, fast_ema=%.2fHz so far) pre_bins_filled=%u/%u\r\n",
                          settle_ms_left, s_freq_ema_fast_hz, s_slow_pre_fill, (unsigned)SLOW_TRACE_PRE_BINS);
            return;
        }
        float warmup_ms_left = (float)s_freq_ema_warmup_ticks_left * (float)SSB_SAMPLE_PERIOD_US / 1000.0f;
        Serial.printf("[dsp] slow_trace: still watching - fast_ema=%.2fHz slow_ema=%.2fHz "
                      "delta=%.2fHz (fires at +/-%.1fHz, warmup_left=%.0fms) pre_bins_filled=%u/%u\r\n",
                      s_freq_ema_fast_hz, s_freq_ema_slow_hz,
                      s_freq_ema_fast_hz - s_freq_ema_slow_hz, (float)SLOW_JUMP_TRIGGER_HZ, warmup_ms_left,
                      s_slow_pre_fill, (unsigned)SLOW_TRACE_PRE_BINS);
        return;
    }

    Serial.printf("[dsp] slow_trace: TRIGGERED at t=%ums before=%.2fHz after=%.2fHz delta=%+.2fHz "
                  "delay=%+.2f - %u pre-bin(s) + %u post-bin(s), %.2fms/bin\r\n",
                  s_slow_trigger_at_ms, s_slow_trigger_before_hz, s_slow_trigger_after_hz,
                  s_slow_trigger_delta_hz, s_slow_trigger_relative_delay,
                  s_slow_pre_fill, s_slow_post_fill,
                  (double)((float)(SLOW_TRACE_BIN_TICKS * SSB_SAMPLE_PERIOD_US) / 1000.0f));

    // 2026-09-12, yet later still: correlates this trigger against the
    // user's own recent '['/']'/preset actions - see
    // relative_delay_get_last_change_ms()'s declaration comment
    // (relative_delay.h) for why. A 0 reading means no delay change has
    // ever been recorded this boot (nothing to correlate against).
    if (s_slow_trigger_delay_change_ms != 0) {
        int32_t since_ms = (int32_t)s_slow_trigger_at_ms - (int32_t)s_slow_trigger_delay_change_ms;
        Serial.printf("[dsp]   relative_delay was last changed %dms before this trigger\r\n",
                      (int)since_ms);
    } else {
        Serial.printf("[dsp]   relative_delay has not been changed since boot\r\n");
    }

    // Oldest-surviving-entry math mirrors diagnostics_print_jump_log()'s
    // own ring-read logic just above. Bin indices are printed relative to
    // the trigger (negative = before, the LAST pre-bin - index -1 - is the
    // bin where the delta first crossed threshold; positive = after).
    uint32_t start = (s_slow_pre_fill < SLOW_TRACE_PRE_BINS) ? 0 : s_slow_pre_write_idx;
    int32_t first_label = -(int32_t)s_slow_pre_fill;
    for (uint32_t k = 0; k < s_slow_pre_fill; k++) {
        slow_trace_bin_t *b = &s_slow_pre_ring[(start + k) % SLOW_TRACE_PRE_BINS];
        bool is_last_pre = (k == s_slow_pre_fill - 1);
        Serial.printf("[dsp]   slow_trace[%+4d]: freq=%.1fHz env_min=%.3f%s\r\n",
                      (int)(first_label + (int32_t)k), b->freq_mean_hz, b->env_min,
                      is_last_pre ? " <-- TRIGGER (delta first exceeded threshold here)" : "");
    }
    for (uint32_t k = 0; k < s_slow_post_fill; k++) {
        slow_trace_bin_t *b = &s_slow_post_bins[k];
        Serial.printf("[dsp]   slow_trace[%+4d]: freq=%.1fHz env_min=%.3f\r\n",
                      (int)(k + 1), b->freq_mean_hz, b->env_min);
    }

    // Long-timescale EMA trend - see EMA_TREND_LEN's declaration comment
    // for why this exists. Added after two consecutive real triggers
    // showed a perfectly flat fine trace above despite a genuine
    // fast/slow gap - this shows whether that gap built up as a smooth,
    // multi-second drift (too slow for the fine trace above to resolve)
    // or something more abrupt. Same oldest-to-newest ring-read pattern
    // as the fine trace, frozen at the trigger instant the same way.
    Serial.printf("[dsp]   slow_trace TREND (last ~%.1fs, %.0fms/sample, frozen at the trigger):\r\n",
                  (double)((float)EMA_TREND_LEN * (float)EMA_TREND_SAMPLE_TICKS * (float)SSB_SAMPLE_PERIOD_US / 1000000.0f),
                  (double)((float)EMA_TREND_SAMPLE_TICKS * (float)SSB_SAMPLE_PERIOD_US / 1000.0f));
    uint32_t tstart = (s_trend_fill < EMA_TREND_LEN) ? 0 : s_trend_write_idx;
    int32_t tfirst_label = -(int32_t)s_trend_fill;
    for (uint32_t k = 0; k < s_trend_fill; k++) {
        uint32_t idx = (tstart + k) % EMA_TREND_LEN;
        Serial.printf("[dsp]     trend[%+4d]: fast=%.2fHz slow=%.2fHz\r\n",
                      (int)(tfirst_label + (int32_t)k), s_trend_fast_hz[idx], s_trend_slow_hz[idx]);
    }

    // Re-arm for the next event now that this one's been read - matches
    // this command's "read it, then watch for the next one" design (see
    // this function's own declaration comment). Resyncing slow to fast
    // avoids an immediate re-trigger storm: right after a genuine step,
    // the slow (2s-tau) EMA is still catching up to the fast one and would
    // otherwise stay >SLOW_JUMP_TRIGGER_HZ apart - and hence keep
    // re-triggering with an near-empty pre-buffer - for up to several
    // seconds after every real event. Also re-arms the warm-up hold
    // (FREQ_EMA_WARMUP_TICKS's declaration comment) - the resync above
    // uses the fast EMA's OWN value, which is only tau=50ms smoothed and
    // so can itself still be offset from the true multi-second average;
    // without this, the false-positive failure mode that motivated the
    // warm-up in the first place could recur right after every real event
    // too, just in a smaller, "re-arm bias" form instead of "boot bias."
    s_freq_ema_slow_hz = s_freq_ema_fast_hz;
    s_freq_ema_warmup_ticks_left = FREQ_EMA_WARMUP_TICKS;
    s_slow_trace_state = SLOW_TRACE_WATCHING;
    s_slow_pre_write_idx = 0;
    s_slow_pre_fill = 0;
    s_slow_bin_sum_freq = 0.0f;
    s_slow_bin_min_env = 1e9f;
    s_slow_bin_count = 0;
    s_trend_write_idx = 0;
    s_trend_fill = 0;
    s_trend_tick_count = 0;
#endif
}

// Owned by the [adc] 1-second rate print below; needs to survive a
// diagnostics_reset() so the very next print after a reset doesn't
// compute a bogus huge delta against stale pre-reset values.
static uint32_t s_last_samples_total = 0;
static uint32_t s_last_callback_count = 0;
static uint32_t s_last_rate_print_ms = 0;

void diagnostics_reset(void)
{
    s_dbg_max_busy_us = 0;
    s_dbg_overrun_count = 0;
    s_dbg_max_adc_us = 0;
    s_dbg_max_dsp_us = 0;
    s_dbg_max_write_us = 0;
    s_dbg_max_tick_gap_us = 0;
    s_dbg_late_tick_count = 0;
    s_dbg_max_diag_block_us = 0;
    s_dbg_diag_block_skip_count = 0;
    s_dbg_dsp_tick_count = 0;
    s_dsp_tick_start_us = esp_timer_get_time();
    s_last_samples_total = 0;
    s_last_callback_count = 0;
    s_last_rate_print_ms = millis();
    s_core1_idle_us_accum = 0;
    s_core1_idle_last_call_us = 0;
    s_core1_idle_hook_calls = 0;
    s_core1_busy_cmd_us = 0;
    s_core1_busy_adc_svc_us = 0;
    s_core1_busy_diag_us = 0;
    s_core1_busy_delay_us = 0;

#if AD9851_ATTACHED
    // Reset like every other high-water mark in this file (unlike the
    // canary two entries below) - the intended use is the same "r, wait,
    // read" pattern already established for every other test in this
    // investigation (see moving_forward_notes.md), so a fresh window
    // should start at zero. s_dbg_have_prev_tx_freq=false (not just
    // zeroing s_dbg_prev_tx_freq) so the very next tick after a reset
    // isn't compared against a stale pre-reset value - same reasoning as
    // s_last_samples_total/s_last_callback_count above.
    s_dbg_max_freq_dev_step_hz = 0;
    s_dbg_max_freq_dev_step_from_hz = 0;
    s_dbg_max_freq_dev_step_to_hz = 0;
    s_dbg_max_freq_dev_step_at_ms = 0;
    s_dbg_have_prev_tx_freq = false;
    s_dbg_freq_step_trace_fill = 0;
    s_dbg_freq_step_trace_armed = false;

    // 2026-09-12: per-event jump log - same "r, wait, read" pattern as
    // every other high-water mark in this file. Deliberately NOT clearing
    // s_jump_log[]'s actual contents - stale entries just get overwritten
    // as new ones arrive after the reset, and s_jump_log_count going back
    // to 0 is what diagnostics_print_jump_log() treats as "empty".
    s_jump_log_write_idx = 0;
    s_jump_log_count = 0;
    s_jump_log_near_null_blended_count = 0;
    s_jump_log_near_null_either_count = 0;
    s_jump_pending = false;

    // 2026-09-12, yet later still: the slow-mean trigger (see its own
    // declaration comment, near jump_log_entry_t above) is deliberately
    // NOT touched here - same reasoning as the canary latches just below:
    // it exists to catch a rare, significant event, and 'r' gets pressed
    // routinely as part of normal measurement hygiene. Resetting it here
    // would risk silently discarding a captured (or capturing-in-progress)
    // event the user hasn't read yet just because they started an
    // unrelated fresh window. It only ever clears via 'K' actually
    // printing a latched trace (see diagnostics_print_slow_trace()).
#endif

    // Deliberately NOT resetting s_dbg_canary_carrier_bad_since_ms /
    // s_dbg_canary_ftw_bad_since_ms (or, as of 2026-09-11, the five IIR
    // canary latches - s_dbg_canary_eq_/_comp_/_gdeq_/_ampeq_/_adclpf_
    // bad_since_ms) here - they're meant to catch a rare, possibly
    // once-per-session event (see canary_check_background()), and this
    // reset gets called often (every 'r' keypress) as part of normal
    // day-to-day measurement hygiene. Resetting them here would mean any
    // corruption event that happened before the last 'r' silently
    // disappears the moment someone starts a fresh measurement window -
    // exactly the opposite of what a canary is for. They only ever clear
    // on reboot.
}

bool diagnostics_get_muted(void)
{
    return s_diag_muted;
}

void diagnostics_toggle_muted(void)
{
    s_diag_muted = !s_diag_muted;
    // Deliberately printed regardless of the new mute state - this
    // confirmation itself needs to always be visible, or muting silently
    // would just create a different confusing problem ("did that command
    // even register?").
    Serial.printf("-> periodic [timing]/[adc]/[dsp] diagnostics %s\r\n",
                  s_diag_muted ? "MUTED (command confirmations only)" : "resumed");
}

void diagnostics_init(void)
{
    s_dsp_tick_start_us = esp_timer_get_time();

    // Core 1 headroom measurement - see core1_idle_hook()'s own comment
    // above. cpuid=1 is passed explicitly to the registration call, so
    // it doesn't matter which core calls this function itself (setup()
    // runs on Core 1 anyway, but that's incidental here).
    esp_err_t err = esp_register_freertos_idle_hook_for_cpu(core1_idle_hook, 1);
    if (err == ESP_OK) {
        s_core1_idle_hook_registered = true;
    } else {
        // Non-fatal - just means the [core1] idle% line below will
        // always read 0% instead of a real measurement. Printed once
        // here rather than failing silently, since a 0% reading could
        // otherwise be mistaken for "genuinely no spare CPU" instead of
        // "hook never got registered".
        Serial.printf("WARNING: Core 1 idle hook registration failed (err=%d) - "
                      "[core1] idle%% will read as 0, not a real measurement\r\n", (int)err);
    }
}

// Real hardware measurement (max_single_call_us=5041, and separately
// availableForWrite() reported avail=162 at what appears to be this
// board's normal RESTING/drained state) showed the original all-or-
// nothing "does the WHOLE ~1.5KB block fit right now" guard was wrong on
// two counts: 2048 bytes turned out to be more than this board's USB-CDC
// TX buffer ever reports free even when idle (so the guard tripped on
// EVERY cycle, not just genuinely backlogged ones - the block "never
// printing" was this threshold being unreachable, not the host actually
// falling behind), and even a lower whole-block threshold couldn't
// guarantee no blocking anyway: with a total capacity well under the
// block's ~1.5KB, filling the buffer partway through a printf still has
// to wait for the driver to drain more before the rest of that same call
// can queue - a single upfront check can't protect against that.
//
// Fix: guard EVERY individual Serial.printf() call separately, each
// against a conservative estimate of THAT line's own worst-case length -
// never asking any single call to queue more than what's already free, so
// none of them can block, regardless of how small the buffer actually is.
// The cost is a patchier print (an individual line can go missing on a
// tight cycle) rather than an all-or-nothing block - a straightforwardly
// better trade once "guarantee we can't block" is the actual goal.
static bool diag_room_for(uint32_t min_bytes)
{
    if ((uint32_t)Serial.availableForWrite() >= min_bytes) {
        return true;
    }
    s_dbg_diag_block_skip_count++;
    return false;
}

static void print_status_line(void)
{
    if (!diag_room_for(130)) {
        return;
    }
#if AD9851_ATTACHED
    Serial.printf("envelope=,%.3f  ,freq_dev=,%.1f,Hz  dac_code=,%u  ,delayed=,%.1f,Hz  tx_freq=,%u,Hz\r\n",
                  s_dbg_envelope, s_dbg_freq_dev, envelope_output_get_last_dac_code(),
                  s_dbg_delayed_freq_dev, s_dbg_tx_freq);
#else
    Serial.printf("envelope=,%.3f  ,freq_dev=,%.1f,Hz  dac_code=,%u\r\n",
                  s_dbg_envelope, s_dbg_freq_dev, envelope_output_get_last_dac_code());
#endif
}

// Null-bias diagnostic - see ssb_dsp_get_null_bias_stats() in ssb_dsp.h.
// plain_mean is the UNWEIGHTED average freq_dev - CONFIRMED on real
// hardware NOT to match what an SDR reads (deviations of Hz, not the 100s
// of Hz plain_mean showed) - kept only so near_null_% / near_null_contrib
// below can still localize the mechanism (do the near-null samples account
// for most of plain_mean's nonzero value?). weighted_mean is the
// physically meaningful one: the envelope^2-weighted average instantaneous
// frequency, which is what actually equals the transmitted power
// spectrum's centroid (a standard identity - see ssb_dsp.h) - this is the
// number to compare against a real spectrum measurement.
//
// 2026-09-09: pulled out of print_timing_and_adc_block() into its own
// function, called on its own schedule from diagnostics_service() and
// deliberately NOT gated by s_diag_muted (unlike everything else in that
// block) - see that call site's comment for why. Still gated per-line by
// diag_room_for() for USB-CDC TX buffer safety, same as every other block
// in this file.
static void print_null_bias_block(void)
{
    ssb_dsp_null_bias_stats_t nb_stats;
    ssb_dsp_get_null_bias_stats(dsp_state_get_ssb(), &nb_stats);
    const float k_two_pi = 6.28318530718f;
    float plain_mean_hz = (nb_stats.dphi_sample_count > 0)
        ? (nb_stats.dphi_sum / (float)nb_stats.dphi_sample_count) * SAMPLE_RATE_HZ / k_two_pi
        : 0.0f;
    float weighted_mean_hz = (nb_stats.env2_sum > 0.0f)
        ? (nb_stats.env2_dphi_sum / nb_stats.env2_sum) * SAMPLE_RATE_HZ / k_two_pi
        : 0.0f;
    float near_null_pct = (nb_stats.dphi_sample_count > 0)
        ? 100.0f * (float)nb_stats.near_null_sample_count / (float)nb_stats.dphi_sample_count
        : 0.0f;
    float near_null_contrib_hz = (nb_stats.dphi_sample_count > 0)
        ? (nb_stats.near_null_dphi_sum / (float)nb_stats.dphi_sample_count) * SAMPLE_RATE_HZ / k_two_pi
        : 0.0f;

    // Expected baseline: for an equal-amplitude two-tone signal, the
    // analytic-signal instantaneous frequency AWAY from envelope nulls is
    // the CONSTANT (f1+f2)/2, not ~0 - that's the actual mechanism this
    // Hilbert/EER technique uses to place two tones (shift the carrier by
    // their average, let the envelope's own harmonic content produce the
    // +-spacing/2 sidebands). dphi_sum/env2_dphi_sum are accumulated
    // BEFORE the LSB sign flip at the end of ssb_dsp_process_sample(), so
    // both always compare against the USB-convention +(f1+f2)/2 regardless
    // of the sideband currently selected. weighted_bias is the number that
    // should actually predict/match a real spectrum measurement;
    // plain_bias is kept only for the mechanistic (near-null) breakdown
    // below.
    float f1 = test_signals_get_twotone_f1_hz();
    float f2 = test_signals_get_twotone_f2_hz();
    float expected_center_hz = 0.5f * (f1 + f2);
    float plain_bias_hz = plain_mean_hz - expected_center_hz;
    float weighted_bias_hz = weighted_mean_hz - expected_center_hz;

    // Three short calls, each comfortably under this board's usual
    // free-buffer headroom (see diag_room_for()'s header comment, citing a
    // real ~162-byte resting measurement) - a single combined printf here
    // previously needed ~200+ bytes and silently lost its diag_room_for()
    // gate on every cycle, so it never printed at all. Matches every other
    // block in this file's own established per-call granularity.
    if (diag_room_for(140)) {
        Serial.printf("[dsp]   null_bias: f1=%.0f f2=%.0f expected_center=%.2fHz "
                      "plain_mean=%.2fHz plain_bias=%.2fHz\r\n",
                      f1, f2, expected_center_hz, plain_mean_hz, plain_bias_hz);
    }
    if (diag_room_for(120)) {
        Serial.printf("[dsp]   null_bias2: weighted_mean=%.2fHz weighted_bias=%.2fHz "
                      "(this is the one to compare against the SDR)\r\n",
                      weighted_mean_hz, weighted_bias_hz);
    }
    if (diag_room_for(150)) {
        Serial.printf("[dsp]   null_bias3: near_null_samples=%.2f%% near_null_contrib=%.2fHz "
                      "(rest=%.2fHz) threshold=%.3f\r\n",
                      near_null_pct, near_null_contrib_hz,
                      plain_mean_hz - near_null_contrib_hz,
                      ssb_dsp_get_null_bias_threshold(dsp_state_get_ssb()));
    }
}

#if AD9851_ATTACHED
// 2026-09-09: canary check for the random-TX-jump investigation (see
// moving_forward_notes.md's "leading theory" entry). s_carrier_hz
// (carrier_output.cpp) and ad9851_s::ftw_reciprocal (AD9851.c) are the only
// two values anywhere in the freq_dev_hz -> TX-frequency path that are
// written once at boot and never touched again by any normal code path -
// every other stage recomputes fresh (or resends its full state) every
// single 62.5us tick, so a wrong value that PERSISTS rather than
// self-correcting on the next tick can only mean one of these two got
// corrupted (RAM bit flip from ESD/RF pickup, or a stray write from an
// unrelated bug elsewhere). carrier_hz is checked against the compile-time
// CARRIER_HZ constant itself (immune to RAM corruption); ftw_reciprocal is
// checked against an independent shadow copy taken once at init (see
// ad9851_get_canary()'s doc comment, AD9851.h, for why that's not a
// perfect guarantee but still strong evidence). Deliberately called from
// the SAME ungated (mute-exempt) 1000ms timer as print_null_bias_block() -
// see that call site's comment - so this keeps checking even while running
// muted for the jump hunt.
// 2026-09-09, revised same day: originally printed an [canary] OK/MISMATCH
// line every second unconditionally (see the removed history below) so it
// could be watched live while muted. That turned out to be a real mistake:
// it meant "muted" no longer actually meant silent - there was now ALWAYS
// 5 lines/sec of Serial traffic (this plus print_null_bias_block(), also
// exempted at the time) regardless of 'v', and the user reported a new
// "noisy" symptom that behaved exactly like the old unmuted-diagnostics
// noise mechanism despite 'v' genuinely being off - i.e. the fix for
// watching the canary silently had itself quietly broken "silently".
// Redesigned to be genuinely silent during normal (healthy) operation:
// canary_check_background() below is called unconditionally, every
// diagnostics_service() tick (not on any timer), but only actually prints
// the FIRST time either check transitions from OK to MISMATCH - after
// that it's a no-op forever (the transition already happened and latched;
// re-printing every tick would add back exactly the noise this rework
// exists to remove). Checking every tick rather than once a second is a
// free improvement while at it - the event gets reported with far less
// latency, since there's no cost to checking when there's nothing to print.
// canary_print_status() is the explicit "show me right now" version for
// diagnostics_print_now() (on-demand, user-requested output is fine to
// always print - it isn't a background stream).
static void canary_check_background(void)
{
    uint32_t carrier_hz_now = carrier_output_get_carrier_hz();
    if (carrier_hz_now != CARRIER_HZ && s_dbg_canary_carrier_bad_since_ms == 0) {
        s_dbg_canary_carrier_bad_since_ms = millis();
        if (diag_room_for(130)) {
            Serial.printf("[canary] carrier_hz=%u (boot=%u) MISMATCH! first seen at t=%ums\r\n",
                          carrier_hz_now, CARRIER_HZ, s_dbg_canary_carrier_bad_since_ms);
        }
    }

    ad9851_canary_t ad_canary;
    carrier_output_get_canary(&ad_canary);
    if (ad_canary.ftw_reciprocal_now != ad_canary.ftw_reciprocal_known_good
        && s_dbg_canary_ftw_bad_since_ms == 0) {
        s_dbg_canary_ftw_bad_since_ms = millis();
        if (diag_room_for(150)) {
            Serial.printf("[canary] ftw_reciprocal=0x%016llx (boot=0x%016llx) MISMATCH! first seen at t=%ums\r\n",
                          (unsigned long long)ad_canary.ftw_reciprocal_now,
                          (unsigned long long)ad_canary.ftw_reciprocal_known_good,
                          s_dbg_canary_ftw_bad_since_ms);
        }
    }

    // 2026-09-11: IIR feedback-state canaries - see the latches' own
    // declaration comment above and each module's *_get_canary() doc
    // comment for why. Same "check every tick unconditionally, only print
    // on the first transition to bad" pattern as the two checks above -
    // cheap (a handful of isfinite() calls) even every tick, and NaN/Inf
    // in feedback state can't self-correct once introduced, so there's no
    // risk of missing a fast self-healing event the way there might be for
    // something that recovers on its own.
    ssb_dsp_iir_canary_t iir_c;
    ssb_dsp_get_iir_canary(dsp_state_get_ssb(), &iir_c);
    if ((!iir_c.eq_hpf_finite || !iir_c.eq_presence_finite) && s_dbg_canary_eq_bad_since_ms == 0) {
        s_dbg_canary_eq_bad_since_ms = millis();
        if (diag_room_for(110)) {
            Serial.printf("[canary] eq biquad state MISMATCH (hpf_ok=%d presence_ok=%d)! first seen at t=%ums\r\n",
                          (int)iir_c.eq_hpf_finite, (int)iir_c.eq_presence_finite, s_dbg_canary_eq_bad_since_ms);
        }
    }
    if (!iir_c.compressor_env_finite && s_dbg_canary_comp_bad_since_ms == 0) {
        s_dbg_canary_comp_bad_since_ms = millis();
        if (diag_room_for(90)) {
            Serial.printf("[canary] compressor env MISMATCH! first seen at t=%ums\r\n", s_dbg_canary_comp_bad_since_ms);
        }
    }

    env_gdeq_canary_t gdeq_c;
    envelope_gdeq_get_canary(&gdeq_c);
    if ((!gdeq_c.stage1_finite || !gdeq_c.stage2_finite) && s_dbg_canary_gdeq_bad_since_ms == 0) {
        s_dbg_canary_gdeq_bad_since_ms = millis();
        if (diag_room_for(110)) {
            Serial.printf("[canary] gdeq allpass state MISMATCH (s1_ok=%d s2_ok=%d)! first seen at t=%ums\r\n",
                          (int)gdeq_c.stage1_finite, (int)gdeq_c.stage2_finite, s_dbg_canary_gdeq_bad_since_ms);
        }
    }

    env_ampeq_canary_t ampeq_c;
    envelope_ampeq_get_canary(&ampeq_c);
    if ((!ampeq_c.shelf1_finite || !ampeq_c.shelf2_finite) && s_dbg_canary_ampeq_bad_since_ms == 0) {
        s_dbg_canary_ampeq_bad_since_ms = millis();
        if (diag_room_for(110)) {
            Serial.printf("[canary] ampeq shelf state MISMATCH (s1_ok=%d s2_ok=%d)! first seen at t=%ums\r\n",
                          (int)ampeq_c.shelf1_finite, (int)ampeq_c.shelf2_finite, s_dbg_canary_ampeq_bad_since_ms);
        }
    }

    adc_lpf_canary_t adc_c;
    adc_capture_get_lpf_canary(&adc_c);
    if ((!adc_c.butterworth_finite || !adc_c.chebyshev_finite) && s_dbg_canary_adclpf_bad_since_ms == 0) {
        s_dbg_canary_adclpf_bad_since_ms = millis();
        if (diag_room_for(120)) {
            Serial.printf("[canary] adc lpf state MISMATCH (butw_ok=%d cheb_ok=%d)! first seen at t=%ums\r\n",
                          (int)adc_c.butterworth_finite, (int)adc_c.chebyshev_finite, s_dbg_canary_adclpf_bad_since_ms);
        }
    }
}

static void canary_print_status(void)
{
    uint32_t carrier_hz_now = carrier_output_get_carrier_hz();
    if (carrier_hz_now == CARRIER_HZ) {
        Serial.printf("[canary] carrier_hz=%u (boot=%u) OK\r\n", carrier_hz_now, CARRIER_HZ);
    } else {
        Serial.printf("[canary] carrier_hz=%u (boot=%u) MISMATCH! first seen at t=%ums\r\n",
                      carrier_hz_now, CARRIER_HZ, s_dbg_canary_carrier_bad_since_ms);
    }

    ad9851_canary_t ad_canary;
    carrier_output_get_canary(&ad_canary);
    if (ad_canary.ftw_reciprocal_now == ad_canary.ftw_reciprocal_known_good) {
        Serial.printf("[canary] ftw_reciprocal=0x%016llx (boot=0x%016llx) OK\r\n",
                      (unsigned long long)ad_canary.ftw_reciprocal_now,
                      (unsigned long long)ad_canary.ftw_reciprocal_known_good);
    } else {
        Serial.printf("[canary] ftw_reciprocal=0x%016llx (boot=0x%016llx) MISMATCH! first seen at t=%ums\r\n",
                      (unsigned long long)ad_canary.ftw_reciprocal_now,
                      (unsigned long long)ad_canary.ftw_reciprocal_known_good,
                      s_dbg_canary_ftw_bad_since_ms);
    }

    // 2026-09-11: IIR feedback-state canaries - see canary_check_background()
    // for why these exist. Printed as ONE compact "OK" line covering all
    // five modules in the (overwhelmingly common) healthy case - matches
    // this file's own diag_room_for() lesson about not routinely printing
    // more than this board's Serial buffer can actually hold - and only
    // expands into per-module MISMATCH detail lines in the rare case one
    // is actually bad, same as every other canary here. No diag_room_for()
    // guard needed - this whole function is the on-demand/user-requested
    // path (see this function's own doc comment above canary_check_
    // background()), which is fine to always print in full.
    ssb_dsp_iir_canary_t iir_c;
    ssb_dsp_get_iir_canary(dsp_state_get_ssb(), &iir_c);
    env_gdeq_canary_t gdeq_c;
    envelope_gdeq_get_canary(&gdeq_c);
    env_ampeq_canary_t ampeq_c;
    envelope_ampeq_get_canary(&ampeq_c);
    adc_lpf_canary_t adc_c;
    adc_capture_get_lpf_canary(&adc_c);

    bool all_iir_ok = iir_c.eq_hpf_finite && iir_c.eq_presence_finite && iir_c.compressor_env_finite
                    && gdeq_c.stage1_finite && gdeq_c.stage2_finite
                    && ampeq_c.shelf1_finite && ampeq_c.shelf2_finite
                    && adc_c.butterworth_finite && adc_c.chebyshev_finite;
    if (all_iir_ok) {
        Serial.printf("[canary] iir_state: OK (eq/comp/gdeq/ampeq/adc_lpf)\r\n");
    } else {
        if (!iir_c.eq_hpf_finite || !iir_c.eq_presence_finite) {
            Serial.printf("[canary] eq biquad state MISMATCH (hpf_ok=%d presence_ok=%d)! first seen at t=%ums\r\n",
                          (int)iir_c.eq_hpf_finite, (int)iir_c.eq_presence_finite, s_dbg_canary_eq_bad_since_ms);
        }
        if (!iir_c.compressor_env_finite) {
            Serial.printf("[canary] compressor env MISMATCH! first seen at t=%ums\r\n", s_dbg_canary_comp_bad_since_ms);
        }
        if (!gdeq_c.stage1_finite || !gdeq_c.stage2_finite) {
            Serial.printf("[canary] gdeq allpass state MISMATCH (s1_ok=%d s2_ok=%d)! first seen at t=%ums\r\n",
                          (int)gdeq_c.stage1_finite, (int)gdeq_c.stage2_finite, s_dbg_canary_gdeq_bad_since_ms);
        }
        if (!ampeq_c.shelf1_finite || !ampeq_c.shelf2_finite) {
            Serial.printf("[canary] ampeq shelf state MISMATCH (s1_ok=%d s2_ok=%d)! first seen at t=%ums\r\n",
                          (int)ampeq_c.shelf1_finite, (int)ampeq_c.shelf2_finite, s_dbg_canary_ampeq_bad_since_ms);
        }
        if (!adc_c.butterworth_finite || !adc_c.chebyshev_finite) {
            Serial.printf("[canary] adc lpf state MISMATCH (butw_ok=%d cheb_ok=%d)! first seen at t=%ums\r\n",
                          (int)adc_c.butterworth_finite, (int)adc_c.chebyshev_finite, s_dbg_canary_adclpf_bad_since_ms);
        }
    }
}
#endif // AD9851_ATTACHED

static void print_timing_and_adc_block(uint32_t now)
{
    // See diag_room_for()'s header comment above (just before
    // print_status_line()) for why this is checked per-line rather than
    // once for the whole block.

    // mode= goes through audio_source_name() - correctly identifies
    // ENVSTEP/FMTEST/AMTEST, not just TWOTONE/SINGLETONE/mic (see
    // dsp_state.cpp's audio_source_name()). gdeq= tells you whether the
    // group-delay equalizer was on during this measurement window.
    if (diag_room_for(160)) {
        Serial.printf("[timing] mode=%s gdeq=%s max_busy_us=%u (adc=%u dsp=%u write=%u) period_us=%u overruns=%u\r\n",
                      audio_source_name(dsp_state_get_audio_source()),
                      envelope_gdeq_get_enabled() ? "ON" : "off",
                      s_dbg_max_busy_us, s_dbg_max_adc_us, s_dbg_max_dsp_us, s_dbg_max_write_us,
                      k_sample_period_us, s_dbg_overrun_count);
    }

#if AD9851_ATTACHED
    // 2026-09-07: moved to run right after the main [timing] line (was
    // last in this block, after wakeup-jitter/core1/dsp-breakdown). Every
    // diag_room_for() check in this function fires back-to-back with no
    // chance for the USB-CDC TX buffer to drain in between (all inside
    // one synchronous call), so on a real board whose resting
    // availableForWrite() is only ~150-200 bytes (see diag_room_for()'s
    // own comment), each line's *own* guard passing doesn't mean the
    // NEXT line's guard will - the buffer keeps draining across the same
    // burst. This line needed the single largest reservation (150 bytes)
    // of any check in the block, and used to be checked fifth/last, so it
    // was structurally the most likely one to lose that race and get
    // silently dropped every cycle (see [diag] skip_total= below to
    // confirm lines are being skipped at all) - not a compile-time or
    // hardware gap, just starved for buffer priority. Splits the
    // [timing] line's write_us (dominated by the AD9851 SPI write) into
    // CPU-side prep (FTW math + bit-reversal loop) vs. the
    // spi_device_polling_transmit()/bit-bang-loop call itself - see
    // ad9851_profile_t (AD9851.h) and the bus-acquire-once change in
    // ad9851_init() this is meant to validate the effect of.
    if (diag_room_for(150)) {
        ad9851_profile_t ad_prof;
        carrier_output_get_profile(&ad_prof);
        Serial.printf("[timing]   ad9851 breakdown: prep_us=%u spi_us=%u (prep+spi=%u vs. write_us=%u "
                      "above - gap is remaining driver/call overhead)\r\n",
                      ad_prof.max_prep_us, ad_prof.max_spi_us,
                      ad_prof.max_prep_us + ad_prof.max_spi_us, s_dbg_max_write_us);
    }

    // 2026-09-10: see s_dbg_max_freq_dev_step_hz's own declaration comment
    // (top of file) for what this measures and why it was added. from/to
    // are the two consecutive tx_freq values straddling the worst step
    // seen, so a real jump's actual Hz values are visible directly, not
    // just its magnitude - lets this be cross-checked against whatever the
    // SDR/receiver showed at the same wall-clock time (t=...ms is since
    // boot, same clock family as the [canary] timestamps).
    if (diag_room_for(140)) {
        Serial.printf("[dsp]   max_freq_dev_step: %uHz (%u -> %u Hz, at t=%ums)\r\n",
                      s_dbg_max_freq_dev_step_hz, s_dbg_max_freq_dev_step_from_hz,
                      s_dbg_max_freq_dev_step_to_hz, s_dbg_max_freq_dev_step_at_ms);
    }

    // 2026-09-11: does that step above recover on its own or stick? See
    // s_dbg_freq_step_trace's declaration comment. Only printed once the
    // capture has actually filled (fill==0 means no step event has been
    // recorded yet this window, e.g. right after 'r') - built into one
    // local buffer first, then a single Serial.print(), same reasoning as
    // this project's other multi-value diagnostic lines (avoids splitting
    // one logical line across several diag_room_for()-gated calls, which
    // could tear it in half if a guard trips mid-line).
    //
    // 2026-09-11 CORRECTION, same day: first version of this line asked
    // diag_room_for(200) - which per THIS FILE's own diag_room_for() header
    // comment (real hardware measurement: availableForWrite() maxes out
    // around ~162 bytes even at this board's fully-drained resting state)
    // is a request that can never succeed. Confirmed on the bench: the line
    // never printed once across a whole capture, even though
    // max_freq_dev_step (140-byte request) printed repeatedly in the same
    // window. Shortened the label text and dropped the request to 130 bytes
    // - worst case here is ~19 bytes of label + 8 values * up to 9 bytes
    // each + CRLF =~ 93 bytes, comfortably under both the request and the
    // board's real ceiling, matching the sibling lines' sizing convention
    // instead of guessing a round number.
    if (s_dbg_freq_step_trace_fill > 0 && diag_room_for(130)) {
        char buf[110];
        int off = snprintf(buf, sizeof(buf), "[dsp]   post-step:");
        for (uint8_t i = 0; i < s_dbg_freq_step_trace_fill && off < (int)sizeof(buf) - 12; i++) {
            off += snprintf(buf + off, sizeof(buf) - off, " %u", s_dbg_freq_step_trace[i]);
        }
        Serial.print(buf);
        Serial.print("\r\n");
    }

    // 2026-09-12: envelope alongside the tx_freq trace above, for the SAME
    // worst-ever event - the direct, per-event test of whether that one
    // recorded jump happened at a near-null (low transmitted power) sample.
    // Kept as its own line/gate rather than folded into the line above -
    // same reasoning already on record just above (the 2026-09-11
    // correction) for why this file splits multi-value lines rather than
    // risking a single oversized request that silently never printed.
    // Budget: ~21-byte label + 8 * up to 7 bytes ("-0.123 ") + CRLF ~= 80
    // bytes worst case, comfortably under the 100-byte request.
    if (s_dbg_freq_step_trace_fill > 0 && diag_room_for(100)) {
        char buf[95];
        int off = snprintf(buf, sizeof(buf), "[dsp]   post-step-env:");
        for (uint8_t i = 0; i < s_dbg_freq_step_trace_fill && off < (int)sizeof(buf) - 9; i++) {
            off += snprintf(buf + off, sizeof(buf) - off, " %.3f", s_dbg_freq_step_trace_envelope[i]);
        }
        Serial.print(buf);
        Serial.print("\r\n");
    }

    // 2026-09-12: per-event jump log aggregate - see diagnostics_print_jump_log()
    // for the full per-event dump ('J'). This one-line summary is cheap
    // enough to leave in the always-on periodic block (unlike the full
    // dump) so an unattended run's serial log shows both near-null
    // percentages building up over hours without needing 'J' pressed at
    // just the right moment. Two percentages now, not one - see
    // near_null_blended/near_null_either's declaration comments above.
    // Budget re-computed for the extra field: worst case (10-digit
    // counters, never realistically reached) is ~103 bytes - rounded up to
    // 150 for margin, same "compute it, then round up, don't guess a round
    // number" approach as this file's other lines after the 2026-09-11
    // diag_room_for(200) lesson.
    if (diag_room_for(150)) {
        Serial.printf("[dsp]   jump_log: n=%u blended=%u (%.0f%%) either=%u (%.0f%%) - 'J' for detail\r\n",
                      s_jump_log_count,
                      s_jump_log_near_null_blended_count,
                      s_jump_log_count ? (100.0f * (float)s_jump_log_near_null_blended_count / (float)s_jump_log_count) : 0.0f,
                      s_jump_log_near_null_either_count,
                      s_jump_log_count ? (100.0f * (float)s_jump_log_near_null_either_count / (float)s_jump_log_count) : 0.0f);
    }
#endif

    if (diag_room_for(100)) {
        Serial.printf("[timing]   wakeup jitter: max_gap_us=%u (nominal=%u) late_ticks_total=%u\r\n",
                      s_dbg_max_tick_gap_us, k_sample_period_us, s_dbg_late_tick_count);
    }
    // Worst single print_timing_and_adc_block() call since last reset -
    // see s_dbg_max_diag_block_us's own comment (near its declaration)
    // for the real hardware measurement (5041us) that motivated this
    // whole per-line-guard rework, and diag_room_for()'s comment for why
    // it's checked per-line now. Kept deliberately short here - this
    // exact line used to carry a long explanation INLINE in the printf
    // string itself, which meant transmitting ~220 extra bytes every
    // single cycle - a real, self-inflicted contributor to the buffer
    // pressure this whole rework exists to fix. The explanation belongs
    // in comments (here and at the static's declaration), not on the wire.
    if (diag_room_for(90)) {
        Serial.printf("[core1]   diag print block: max_single_call_us=%u (worst since last reset)\r\n",
                      s_dbg_max_diag_block_us);
    }

    // Sub-phase breakdown of dsp_us itself, from ssb_dsp's internal
    // profiling - lets us see which part of the DSP call (audio_fx, the
    // Hilbert FIR, or atan2f/sqrtf) is actually costing time, rather than
    // guessing again.
    if (diag_room_for(90)) {
        ssb_dsp_profile_t prof;
        ssb_dsp_get_profile(dsp_state_get_ssb(), &prof);
        Serial.printf("[timing]   dsp breakdown: audio_fx=%u fir=%u atan2=%u sqrt=%u\r\n",
                      prof.max_audio_fx_us, prof.max_fir_us, prof.max_atan2_us, prof.max_sqrt_us);
    }

    // Evidence for setting MAX_FREQ_DEV_HZ from real data instead of
    // guessing again - max_unclamped is the TRUE peak deviation the
    // signal actually reaches (before any clamping), clip_count is how
    // many samples the clamp has actually had to intervene on.
    if (diag_room_for(110)) {
        ssb_dsp_freq_dev_stats_t fd_stats;
        ssb_dsp_get_freq_dev_stats(dsp_state_get_ssb(), &fd_stats);
        Serial.printf("[dsp]   freq_dev: max_unclamped=%.0fHz (limit=%.0fHz) clip_count=%u\r\n",
                      fd_stats.max_unclamped_freq_dev_hz, MAX_FREQ_DEV_HZ, fd_stats.clip_count);
    }

    // Null-bias diagnostic block moved out to its own function,
    // print_null_bias_block() (above) - see 2026-09-09 moving_forward_notes.md
    // entry: it's now printed on its own cadence, deliberately EXEMPT from
    // s_diag_muted (see diagnostics_service()), so it can be watched live
    // while chasing the random two-tone frequency-jump symptom without
    // re-enabling the full (noisier) diagnostic stream.

    // ADC continuity check: actual samples/callbacks seen in this ~1s
    // window vs. what ADC_CONT_SAMPLE_FREQ_HZ implies, plus any pool
    // overflow events. If "actual" comes in noticeably below "expected"
    // (or pool_ovf is nonzero), the stream has real gaps - the filter's
    // uniform-sample-spacing assumption is being violated, which would
    // explain artifacts no amount of filter debugging could fix.
    adc_capture_diag_t adc_diag;
    adc_capture_get_diag(&adc_diag);

    uint32_t elapsed_ms = now - s_last_rate_print_ms;
    if (elapsed_ms > 0) {
        uint32_t actual_sps   = (uint32_t)((uint64_t)(adc_diag.samples_total - s_last_samples_total) * 1000 / elapsed_ms);
        uint32_t expected_cbs = (uint32_t)((uint64_t)ADC_CONT_SAMPLE_FREQ_HZ * elapsed_ms
                                            / 1000 / ADC_CONT_FRAME_SAMPLES);
        if (diag_room_for(110)) {
            Serial.printf("[adc] actual=%u sps (expected=%u) callbacks=%u (expected~%u) pool_ovf_total=%u\r\n",
                          actual_sps, ADC_CONT_SAMPLE_FREQ_HZ,
                          adc_diag.callback_count - s_last_callback_count, expected_cbs,
                          adc_diag.pool_ovf_count);
        }

        // Core 1 headroom over this SAME ~1s window - see core1_idle_hook()'s
        // own comment. Snapshot-then-reset rather than a running total: a
        // per-window reading is more useful here than a cumulative-since-
        // boot average would be, since it stays responsive to whatever's
        // currently happening on Core 1 (a Serial burst, a mode switch)
        // instead of smoothing it away over the long run. Safe to read/
        // reset without a lock - see the statics' own comment above for
        // why (idle priority can never preempt this loop()-context code).
        if (!s_core1_idle_hook_registered) {
            if (diag_room_for(70)) {
                Serial.printf("[core1] idle hook not registered - no measurement available\r\n");
            }
        } else {
            double idle_pct = (double)s_core1_idle_us_accum * 100.0 / ((double)elapsed_ms * 1000.0);
            uint32_t hook_calls_per_sec = (uint32_t)((uint64_t)s_core1_idle_hook_calls * 1000 / elapsed_ms);
            // Full rationale for why this hook-based figure reads low (and
            // [core1] busy breakdown's delay=% below is the trustworthy
            // number) lives in CORE1_IDLE_GAP_THRESHOLD_US's own comment
            // now, not on the wire every second - same "don't transmit an
            // essay every cycle" fix as the diag-block line above.
            if (diag_room_for(90)) {
                Serial.printf("[core1] idle(hook)=%.1f%% over %ums (cross-check only - see busy "
                              "breakdown's delay%% below)\r\n",
                              idle_pct, elapsed_ms);
            }
            if (diag_room_for(60)) {
                Serial.printf("[core1]   idle hook calls/s=%u (cross-check)\r\n", hook_calls_per_sec);
            }
            s_core1_idle_us_accum = 0;
            s_core1_idle_hook_calls = 0;

            // Breakdown of the window - "where does Core 1's time actually
            // go". cmd/adc_svc/diag/delay are the four loop() sub-calls we
            // can measure directly (see diagnostics_record_
            // core1_loop_timings()'s header comment - delay(10) used to go
            // unmeasured and its time landed entirely in "other", which is
            // what made "other" look like ~96% of Core 1 before this
            // bucket was added). "other" here is deliberately NOT reduced
            // by idle(hook)% above - that figure covers the same physical
            // time as delay% but via the unreliable hook/threshold method,
            // so subtracting both would double-count and mask real
            // "other" cost under the clamp. What's left in "other" after
            // cmd/adc_svc/diag/delay is genuinely unaccounted for: ISR
            // time (chiefly the ADC's on_conv_done, which fires
            // continuously regardless of mode) plus USB CDC driver
            // overhead, neither of which loop() ever sees directly to
            // time itself.
            double window_us   = (double)elapsed_ms * 1000.0;
            double cmd_pct     = (double)s_core1_busy_cmd_us     * 100.0 / window_us;
            double adc_svc_pct = (double)s_core1_busy_adc_svc_us * 100.0 / window_us;
            double diag_pct    = (double)s_core1_busy_diag_us    * 100.0 / window_us;
            double delay_pct   = (double)s_core1_busy_delay_us   * 100.0 / window_us;
            double other_pct   = 100.0 - cmd_pct - adc_svc_pct - diag_pct - delay_pct;
            if (other_pct < 0.0) other_pct = 0.0;   // clamp - rounding/overlap across independently-measured windows, not a real negative cost
            if (diag_room_for(140)) {
                Serial.printf("[core1]   busy breakdown: cmd=%.1f%% adc_svc=%.1f%% diag=%.1f%% delay=%.1f%% "
                              "other(ADC ISR + USB CDC + ...)=%.1f%%\r\n",
                              cmd_pct, adc_svc_pct, diag_pct, delay_pct, other_pct);
            }
            s_core1_busy_cmd_us = 0;
            s_core1_busy_adc_svc_us = 0;
            s_core1_busy_diag_us = 0;
            s_core1_busy_delay_us = 0;
        }

        // Long-window average - much lower noise than the 1s figure
        // above, since averaging error shrinks with window length. This
        // is what to trust for pinning down the TRUE achieved ADC rate
        // vs. the requested ADC_CONT_SAMPLE_FREQ_HZ.
        int64_t elapsed_since_start_us = esp_timer_get_time() - adc_capture_get_start_us();
        if (elapsed_since_start_us > 0) {
            double long_avg_sps = (double)adc_diag.samples_total * 1000000.0 / (double)elapsed_since_start_us;
            double error_pct = (long_avg_sps - (double)ADC_CONT_SAMPLE_FREQ_HZ)
                                * 100.0 / (double)ADC_CONT_SAMPLE_FREQ_HZ;
            if (diag_room_for(110)) {
                Serial.printf("[adc]   long-window avg=%.2f sps over %.1fs (%.3f%% vs nominal %uHz)\r\n",
                              long_avg_sps, elapsed_since_start_us / 1000000.0,
                              error_pct, ADC_CONT_SAMPLE_FREQ_HZ);
            }

            // Same measurement for dsp_task's own tick rate (gptimer) -
            // only ever measured the ADC side precisely before. Chronic
            // FIFO starvation despite the ADC running fast (not slow)
            // only makes sense if THIS clock is also running fast, by
            // more than the ADC's own error.
            int64_t dsp_elapsed_us = esp_timer_get_time() - s_dsp_tick_start_us;
            if (dsp_elapsed_us > 0) {
                double long_avg_tps = (double)s_dbg_dsp_tick_count * 1000000.0 / (double)dsp_elapsed_us;
                double tick_error_pct = (long_avg_tps - (double)SAMPLE_RATE_HZ)
                                         * 100.0 / (double)SAMPLE_RATE_HZ;
                // The number that actually matters for the FIFO: the TRUE
                // ratio of the two measured rates, vs. the nominal
                // ADC_SAMPLES_PER_TICK the drain logic assumes. If this
                // deviates meaningfully from that nominal value, that -
                // not either clock's error in isolation - is the real
                // cause of sustained starvation or backlog.
                double true_ratio = long_avg_sps / long_avg_tps;
                if (diag_room_for(130)) {
                    Serial.printf("[dsp]   long-window avg=%.2f ticks/s (%.3f%% vs nominal %uHz) true_ratio=%.4f (nominal=%u)\r\n",
                                  long_avg_tps, tick_error_pct, SAMPLE_RATE_HZ,
                                  true_ratio, ADC_SAMPLES_PER_TICK);
                }
            }
        }
        if (diag_room_for(130)) {
            Serial.printf("[adc]   fifo: available now min=%u max=%u (want>=%u,<%u) starve_ticks_total=%u drop_total=%u\r\n",
                          adc_diag.fifo_min_available, adc_diag.fifo_max_available,
                          ADC_SAMPLES_PER_TICK, ADC_FIFO_SIZE,
                          adc_diag.fifo_starve_count, adc_diag.fifo_drop_count);
        }
    }
    s_last_samples_total  = adc_diag.samples_total;
    s_last_callback_count = adc_diag.callback_count;
    s_last_rate_print_ms  = now;

    // Visibility for the per-line guards above - deliberately last (least
    // important to preserve) and itself guarded, so a tight cycle just
    // drops this too rather than blocking to force it out.
    if (diag_room_for(70)) {
        Serial.printf("[diag]   skip_total=%u (lines dropped by the per-line TX-buffer guards above)\r\n",
                      s_dbg_diag_block_skip_count);
    }
}

void diagnostics_service(void)
{
    static uint32_t last_print_ms = 0;
    uint32_t now = millis();
    if (!s_diag_muted && now - last_print_ms >= 45) {
        last_print_ms = now;
        print_status_line();
    }

    static uint32_t last_timing_print_ms = 0;
    if (!s_diag_muted && now - last_timing_print_ms >= 1000) {
        last_timing_print_ms = now;
        // See s_dbg_max_diag_block_us's own comment - measuring this
        // call's own wall-clock cost directly, not just relying on the
        // existing summed diag_us bucket, to check whether a single
        // occurrence of this print burst is ever long enough to matter.
        int64_t t_diagblock0 = esp_timer_get_time();
        print_timing_and_adc_block(now);
        int64_t t_diagblock1 = esp_timer_get_time();
        uint32_t diagblock_us = (uint32_t)(t_diagblock1 - t_diagblock0);
        if (diagblock_us > s_dbg_max_diag_block_us) s_dbg_max_diag_block_us = diagblock_us;

        // 2026-09-09: null_bias block used to live here too, then got
        // pulled out to its own UNGATED (mute-exempt) timer so it could be
        // watched live while chasing the random two-tone frequency jump.
        // Reverted same day: (1) that experiment already ran and showed
        // weighted_bias doesn't track the real jump at all (see
        // moving_forward_notes.md) - its live-while-muted use case is
        // gone; (2) worse, running it (and the canary, at the time)
        // unconditionally meant "muted" no longer actually meant silent -
        // there was now ALWAYS 5 lines/sec of Serial traffic regardless of
        // 'v', and the user reported a new "noisy" symptom that behaved
        // exactly like the old unmuted-diagnostics noise mechanism despite
        // 'v' genuinely being off. Back under the normal mute gate, same
        // as everything else in this block - still reachable on demand via
        // diagnostics_print_now() regardless of mute state.
        print_null_bias_block();
    }

#if AD9851_ATTACHED
    // Deliberately OUTSIDE the s_diag_muted gate above, and checked every
    // call rather than on a timer - see canary_check_background()'s own
    // comment for why this one is safe to run unconditionally where
    // null_bias wasn't: it only ever prints once, on the actual transition
    // to a mismatch, so it costs nothing during normal (healthy) operation
    // - unlike null_bias/the old canary design, there's no ongoing
    // steady-state Serial traffic for this to add back.
    canary_check_background();
#endif
}

// 2026-09-07: on-demand snapshot, added because the periodic block above
// is genuinely awkward to catch on purpose - it only fires once a
// second, and while `'v'` is muted it doesn't print at all (by design -
// see diagnostics_toggle_muted()'s own comment). That made a clean A/B
// (e.g. "mute, wait 10s so nothing prints, then read what accumulated
// during the silence") unnecessarily fiddly: un-muting resumes the
// ongoing 1Hz stream rather than giving one clean read exactly when
// asked for. This bypasses BOTH gates - prints once, immediately, on
// request, whether muted or not, without touching last_print_ms/
// last_timing_print_ms/last_null_bias_print_ms (so it doesn't perturb any
// of the periodic blocks' own independent schedules either). Deliberately
// does NOT reset any counters - it's a read, not a `'r'`. Still goes
// through print_status_line()/print_timing_and_adc_block()/
// print_null_bias_block()'s own diag_room_for() guards internally, so it
// can't block waiting on the TX buffer any more than the periodic path
// could. 2026-09-09: added the explicit print_null_bias_block() call here
// since that block moved out of print_timing_and_adc_block() - without
// this line the on-demand snapshot would have silently stopped including
// null_bias/null_bias2/null_bias3.
void diagnostics_print_now(void)
{
    Serial.printf("-> on-demand diagnostic snapshot (ignores mute/throttle, doesn't reset counters):\r\n");
    print_status_line();
    print_timing_and_adc_block(millis());
    print_null_bias_block();
#if AD9851_ATTACHED
    canary_print_status();
#endif
}
