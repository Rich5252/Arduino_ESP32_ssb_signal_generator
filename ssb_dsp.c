#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ssb_dsp.h"
#include "esp_log.h"
#include "esp_timer.h"


static const char *TAG = "ssb_dsp";

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

// ---- Pre-Hilbert audio conditioning: cascaded biquads + envelope compressor ----
// Direct Form I, float. Coefficients set once at init (trig only there);
// the per-sample path below is pure mult/add, no transcendentals.
typedef struct {
    float b0, b1, b2, a1, a2;
    float x1, x2, y1, y2;
} biquad_t;

// 2026-09-25: BP_COUNT breakpoints (threshold plus BP_COUNT-1 more, evenly
// spaced up to SSB_DSP_COMP_XMAX) define a 3-SEGMENT piecewise-linear
// approximation of a dB-domain/log soft-knee curve - see
// ssb_dsp_set_compressor_level()'s doc comment in ssb_dsp.h for the full
// design rationale. bp_x is fixed/shared across every calibrated level
// (set once at init from the two constants below); only bp_y (the curve's
// actual shape) and makeup_gain change when the level changes.
#define SSB_DSP_COMP_BP_COUNT   4
// Fixed, shared curve threshold - NOT independently configurable (see
// ssb_audio_fx_config_t's 2026-09-25 comment in ssb_dsp.h for why: every
// entry in k_comp_level_table below was calibrated against this exact
// value on real voice data, and would silently go stale if this drifted
// out from under it without a full recalibration).
#define SSB_DSP_COMP_THRESHOLD  0.30f
// Top breakpoint - deliberately a bit above 1.0 so genuine over-drive
// (mic gain set a little high, or a single loud syllable) still gets a
// smoothly-continued curve instead of a hard corner right at full scale.
#define SSB_DSP_COMP_XMAX       1.05f

typedef struct {
    float attack_coef, release_coef;   // one-pole envelope-follower coefficients
    float env;
    float bp_x[SSB_DSP_COMP_BP_COUNT]; // fixed/shared breakpoint x-locations (threshold..XMAX)
    float bp_y[SSB_DSP_COMP_BP_COUNT]; // CURRENT level's breakpoint y-values (pre-makeup) -
                                        // recomputed only when the level changes, see
                                        // ssb_dsp_set_compressor_level().
    float makeup_gain;                 // CURRENT level's FIXED makeup gain - a baked-in
                                        // constant from k_comp_level_table, never recomputed
                                        // per-sample from env/peak/anything else (see
                                        // ssb_dsp_set_compressor_level()'s doc comment in
                                        // ssb_dsp.h for why this replaced the 2026-09-24
                                        // peak-tracking scheme).
    int level_db;                      // current ssb_dsp_set_compressor_level() setting
} compressor_t;

// 2026-09-25: Mic Squelch - see ssb_dsp_set_squelch_enabled()'s doc
// comment in ssb_dsp.h for the full design rationale (why this exists
// alongside envelope_floor.h rather than reusing it, why it sits here
// rather than downstream on the demodulated envelope, and why three
// separate one-pole stages rather than one instantaneous compare).
#define SSB_DSP_SQUELCH_ATTACK_MS        5.0f   // detector: long enough that an isolated
                                                  // single-sample click barely moves it (see
                                                  // header doc comment's ~1.25%-of-impulse
                                                  // figure), short enough to track genuine
                                                  // speech onset within a few ms
#define SSB_DSP_SQUELCH_RELEASE_MS      30.0f    // detector: how fast the LEVEL ESTIMATE itself
                                                  // decays once real signal stops - deliberately
                                                  // much faster than the GAIN's own release below,
                                                  // this just tracks level, it doesn't shape the
                                                  // audible transition
#define SSB_DSP_SQUELCH_GAIN_ATTACK_MS   5.0f    // gate gain: fast open, don't clip speech onset
#define SSB_DSP_SQUELCH_GAIN_RELEASE_MS 150.0f   // gate gain: slow close, avoid audible chatter on
                                                  // brief in-word dips - this IS the number that
                                                  // shapes the audible open/close transition
#define SSB_DSP_SQUELCH_HYSTERESIS_RATIO 0.5f    // close threshold = 0.5x open threshold (~-6dB
                                                  // Schmitt margin) - standard noise-gate practice,
                                                  // stops chatter for a signal hovering at one level

typedef struct {
    volatile bool enable;
    volatile float threshold;   // OPEN threshold; close threshold is threshold*HYSTERESIS_RATIO
    float detector_attack_coef, detector_release_coef;
    float detector_env;         // smoothed fabsf(audio_sample) - the level ESTIMATE
    bool gate_open;             // Schmitt-trigger state
    float gain_attack_coef, gain_release_coef;
    float gain;                 // smoothed [0,1] multiplier actually applied to the sample
} squelch_t;

// 2026-09-25: per-level calibration table replacing the switchable-mode
// compressor. {ratio, makeup_gain} pairs are CONSTANTS baked in from an
// offline Python calibration against the user's own real mic-to-ADC
// recording (G4AHN.wav), bisecting ratio to hit each target dB of RMS
// gain (assuming mic gain has normalized input peak to ~0.85 - see
// ssb_dsp_set_mic_gain_db()), then fixing makeup_gain at the exact
// peak-restoring value found at that ratio. Index 0 is level 0's
// special-cased {ratio, makeup=1.0} limiter pair, NOT part of the same
// search family as 1-9 - see ssb_dsp_set_compressor_level()'s doc comment
// in ssb_dsp.h. Full methodology and the raw calibration run's output are
// logged in moving_forward_notes.md's 2026-09-25 entry.
//
// 2026-09-25 (second pass, same day): extended from a 0-6 table to 0-10 on
// explicit user request. Levels 7-9 continue bisecting for their literal
// nominal target (+7/+8/+9dB) same as 1-6 always did. Level 10 does NOT -
// a first attempt at this extension bisected literally for +9dB and +10dB
// and found BOTH saturate the search's ratio_hi=50.0 ceiling, converging
// to IDENTICAL {ratio=50.0, makeup=2.775268} entries at only +8.36dB
// each - not a bug in the search, but a real physical ceiling: as
// ratio->infinity the curve degenerates to a hard limiter fixed at
// SSB_DSP_COMP_THRESHOLD, and even that true hard-clip case only measures
// ~+8.53dB of RMS gain on G4AHN.wav (independently recomputed directly,
// not via bisection, to confirm this wasn't itself a search-bound
// artifact). Level 10 here is instead calibrated to an explicitly chosen,
// genuinely-achievable +8.35dB target (comfortably clear of the ~8.5dB
// asymptote so the bisection search converges normally, not against its
// bound) - distinct from level 9's +8.1dB, at the cost of the label no
// longer meaning a clean "+10dB". See the SSB_DSP_COMP_LEVEL_MAX doc
// comment in ssb_dsp.h for the full explanation and the historical note
// on an earlier listening test that judged true +10dB (under the old,
// pre-this-table scheme) to already hurt intelligibility - levels 7-10
// here are numerically verified (bisection cross-checked bit-for-bit
// against a standalone C reimplementation reading G4AHN.wav directly) but
// NOT yet subjectively re-validated by ear.
typedef struct {
    float ratio;
    float makeup_gain;
} comp_level_entry_t;

static const comp_level_entry_t k_comp_level_table[SSB_DSP_COMP_LEVEL_MAX - SSB_DSP_COMP_LEVEL_MIN + 1] = {
    /* level 0  (limiter, no makeup)  */ {  5.0000f, 1.000000f },
    /* level 1  (~+1.0dB measured)    */ {  1.1355f, 1.133124f },
    /* level 2  (~+2.0dB measured)    */ {  1.3123f, 1.282759f },
    /* level 3  (~+3.0dB measured)    */ {  1.5527f, 1.450924f },
    /* level 4  (~+4.0dB measured)    */ {  1.8990f, 1.639888f },
    /* level 5  (~+5.0dB measured)    */ {  2.4412f, 1.852198f },
    /* level 6  (~+6.0dB measured)    */ {  3.4117f, 2.090712f },
    /* level 7  (~+7.0dB measured)    */ {  5.6513f, 2.358638f },
    /* level 8  (~+7.6dB measured)    */ {  9.3100f, 2.534993f },
    /* level 9  (~+8.1dB measured)    */ { 20.1842f, 2.691641f },
    /* level 10 (~+8.35dB measured -  */
    /*   NOT +10dB, see comment above)*/ { 48.4601f, 2.773442f },
};

// Denormal (subnormal) floats are numbers below ~1.2e-38f in magnitude -
// still valid IEEE-754 values, but most FPUs (including Xtensa's) fall
// back to a slow microcoded/trap path to handle them instead of the
// normal single-cycle path. Filter/envelope state that decays toward
// zero between speech bursts passes straight through this range, which
// is the classic cause of exactly this kind of "goes slow when the
// signal goes quiet" timing jump in audio DSP code. Flushing anything
// below this threshold straight to 0.0f keeps state out of that range;
// 1e-15f is many orders of magnitude below anything audible, so this has
// no effect on the signal itself.
static inline float IRAM_ATTR flush_denorm(float x)
{
    return (fabsf(x) < 1e-15f) ? 0.0f : x;
}

// ---- Fast atan2f/sqrtf replacements ----
// Measured: libm atan2f ~28us, sqrtf ~12us per call on this build - both
// far higher than single-precision hardware-FPU math should cost, which
// strongly suggests this libm promotes to double internally (Xtensa LX7's
// FPU is single-precision only, so double math falls back to a slow
// software-emulated path). The FIR/EQ code right above this, which is
// pure single-precision add/multiply, stays cheap - it's specifically
// these two library calls that are the outlier.
//
// Set SSB_DSP_FAST_TRIG to 0 to fall back to plain atan2f/sqrtf, e.g. for
// an A/B comparison once real hardware + a spectrum analyzer are in the
// loop. Default 1 (fast path).
//
// RULED OUT as the cause of the ~+100Hz two-tone frequency offset seen
// on real hardware (single 1000Hz tone landed exactly on frequency;
// 700/1900Hz two-tone measured at 800/1999Hz) - a direct A/B test with
// this set to 0 (real atan2f/sqrtf) showed no change to the offset.
// Reverted to 1 so CPU budget stays as designed while the next
// hypothesis (max_freq_dev_hz clamp asymmetry) is tested in isolation.
#ifndef SSB_DSP_FAST_TRIG
#define SSB_DSP_FAST_TRIG 1
#endif

#if SSB_DSP_FAST_TRIG

// Minimax polynomial approximation of atan(x) for x in [-1, 1].
// Coefficients are a well-known published 5-term minimax fit (this exact
// numeric set is widely reproduced across DSP references, not from any
// single source) - worst-case error is on the order of 1e-3 rad across
// the domain; spot-checked here at x=1 against the exact value:
// atan(1) = pi/4 = 0.7853982, polynomial evaluates to ~0.7854096,
// error ~1.1e-5 rad at that point.
static inline float IRAM_ATTR fast_atan(float x)
{
    float x2 = x * x;
    return x * (0.9998660f + x2 * (-0.3302995f + x2 * (0.1801410f
                + x2 * (-0.0851330f + x2 * 0.0208351f))));
}

// Full-range atan2 built from fast_atan via the standard reciprocal
// (atan(1/t) = pi/2 - atan(t)) and quadrant identities, so fast_atan
// only ever sees inputs in [-1, 1] where its error bound applies.
static inline float IRAM_ATTR fast_atan2(float y, float x)
{
    if (x == 0.0f && y == 0.0f) return 0.0f;

    float ax = fabsf(x), ay = fabsf(y);
    float angle;
    if (ax >= ay) {
        angle = fast_atan(ay / ax);
    } else {
        angle = (float)M_PI * 0.5f - fast_atan(ax / ay);
    }
    if (x < 0.0f) angle = (float)M_PI - angle;
    if (y < 0.0f) angle = -angle;
    return angle;
}

// sqrt(x) via the classic fast-inverse-sqrt bit-hack seed, refined with
// two Newton-Raphson iterations on the reciprocal before inverting back.
// Two iterations (rather than the traditional single-iteration "Quake"
// version) bring relative error down to roughly 1e-4 - below this
// system's own 12-bit ADC quantization noise floor (~2e-4) - at the cost
// of a handful more cycles, which is negligible against the ~12us this
// replaces.
static inline float IRAM_ATTR fast_sqrt(float x)
{
    if (x <= 0.0f) return 0.0f;
    union { float f; uint32_t i; } conv = { .f = x };
    conv.i = 0x5f3759dfu - (conv.i >> 1);
    float y = conv.f;
    y = y * (1.5f - 0.5f * x * y * y);
    y = y * (1.5f - 0.5f * x * y * y);
    return x * y;
}
#endif // SSB_DSP_FAST_TRIG

static inline float IRAM_ATTR biquad_process(biquad_t *bq, float x)
{
    float y = bq->b0 * x + bq->b1 * bq->x1 + bq->b2 * bq->x2
                          - bq->a1 * bq->y1 - bq->a2 * bq->y2;
    y = flush_denorm(y);
    bq->x2 = flush_denorm(bq->x1); bq->x1 = flush_denorm(x);
    bq->y2 = bq->y1; bq->y1 = y;
    return y;
}

// RBJ cookbook high-pass. fc/fs and Q -> normalized biquad coefficients.
static void biquad_set_highpass(biquad_t *bq, float fc, float fs, float q)
{
    float w0 = 2.0f * M_PI * fc / fs;
    float cosw0 = cosf(w0);
    float alpha = sinf(w0) / (2.0f * q);

    float a0 = 1.0f + alpha;
    bq->b0 = ((1.0f + cosw0) / 2.0f) / a0;
    bq->b1 = (-(1.0f + cosw0)) / a0;
    bq->b2 = ((1.0f + cosw0) / 2.0f) / a0;
    bq->a1 = (-2.0f * cosw0) / a0;
    bq->a2 = (1.0f - alpha) / a0;
    bq->x1 = bq->x2 = bq->y1 = bq->y2 = 0.0f;
}

// RBJ cookbook peaking EQ. fc/fs, Q, and gain in dB -> normalized coefficients.
static void biquad_set_peaking(biquad_t *bq, float fc, float fs, float q, float gain_db)
{
    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * M_PI * fc / fs;
    float cosw0 = cosf(w0);
    float alpha = sinf(w0) / (2.0f * q);

    float a0 = 1.0f + alpha / A;
    bq->b0 = (1.0f + alpha * A) / a0;
    bq->b1 = (-2.0f * cosw0) / a0;
    bq->b2 = (1.0f - alpha * A) / a0;
    bq->a1 = (-2.0f * cosw0) / a0;
    bq->a2 = (1.0f - alpha / A) / a0;
    bq->x1 = bq->x2 = bq->y1 = bq->y2 = 0.0f;
}

// ---- Generic first-order all-pass (group-delay equalizer primitive) ----
// See ssb_dsp.h's ssb_allpass1_t doc comment for the transfer function
// and group-delay formula this implements. One-multiply Direct Form I:
// y = x1 + a*(x - y1); x1 = x; y1 = y - algebraically identical to
// y = a*x + x1 - a*y1 (the textbook two-multiply form) but one multiply
// cheaper, same trick used for the RBJ biquads' normalized coefficients
// elsewhere in this file being worth the one-time division at init.
void ssb_allpass1_init(ssb_allpass1_t *f, float a)
{
    f->a = a;
    f->x1 = 0.0f;
    f->y1 = 0.0f;
}

void ssb_allpass1_reset(ssb_allpass1_t *f)
{
    f->x1 = 0.0f;
    f->y1 = 0.0f;
}

float IRAM_ATTR ssb_allpass1_process(ssb_allpass1_t *f, float x)
{
    float y = f->x1 + f->a * (x - f->y1);
    f->x1 = flush_denorm(x);
    f->y1 = flush_denorm(y);
    return f->y1;
}

// ---- Generic high-shelf biquad (magnitude equalizer primitive) ----
// See ssb_dsp.h's ssb_shelf_biquad_t doc comment (including why it's named
// ssb_shelf_biquad_t rather than ssb_biquad_t - a NAME COLLISION with
// ssb_adc_filter.h's own, different, ssb_biquad_t caused a "multiple
// definition" link error the first time this was added; renamed to fix
// it). RBJ Audio EQ Cookbook high-shelf, S=1 (max slope, no
// transition-band bump/dip) - same family of formula as this file's own
// PRIVATE biquad_set_peaking() above, just the shelf variant instead of
// the peaking/bell variant, and exposed publicly for reuse outside this
// file (biquad_set_peaking/highpass stay private - they're wired one
// specific way into the pre-Hilbert audio_fx chain below and don't need a
// public API of their own).
void ssb_shelf_biquad_set_highshelf(ssb_shelf_biquad_t *f, float fc, float fs, float gain_db)
{
    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * M_PI * fc / fs;
    float cosw0 = cosf(w0);
    float sinw0 = sinf(w0);
    // S=1 shelf slope: (A + 1/A)*(1/S - 1) term vanishes, leaving the
    // simplified alpha below (still the general RBJ formula, just with
    // S=1 substituted in rather than exposed as a separate parameter -
    // this project has no use yet for a shallower/steeper shelf, and a
    // steeper-than-S=1 slope introduces a peak/dip right at the corner
    // that would fight the point of a smooth, boring partial correction).
    float alpha = (sinw0 / 2.0f) * sqrtf(2.0f);
    float sqrtA = sqrtf(A);

    float a0 =        (A + 1.0f) - (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha;
    f->b0 = ( A * ((A + 1.0f) + (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha) ) / a0;
    f->b1 = ( -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw0) ) / a0;
    f->b2 = ( A * ((A + 1.0f) + (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha) ) / a0;
    f->a1 = ( 2.0f * ((A - 1.0f) - (A + 1.0f) * cosw0) ) / a0;
    f->a2 = ( (A + 1.0f) - (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha ) / a0;
    f->x1 = f->x2 = f->y1 = f->y2 = 0.0f;
}

void ssb_shelf_biquad_reset(ssb_shelf_biquad_t *f)
{
    f->x1 = f->x2 = f->y1 = f->y2 = 0.0f;
}

float IRAM_ATTR ssb_shelf_biquad_process(ssb_shelf_biquad_t *f, float x)
{
    float y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2
                         - f->a1 * f->y1 - f->a2 * f->y2;
    y = flush_denorm(y);
    f->x2 = flush_denorm(f->x1); f->x1 = flush_denorm(x);
    f->y2 = f->y1; f->y1 = y;
    return y;
}

// 2026-09-25: 3-segment piecewise-linear interpolation over the compressor's
// fixed breakpoints (bp_x[0]=threshold .. bp_x[3]=XMAX). Unrolled 3-way
// branch instead of a loop - same reasoning as the FIR/delay-line code
// elsewhere in this file (Xtensa has no hardware integer divider and a
// 4-element loop isn't worth the branch overhead). Caller guarantees
// a > bp_x[0] (the below-threshold case is handled separately, cheaper,
// in compressor_process()). Roughly 2x a plain single-segment linear
// curve's per-sample cost - see moving_forward_notes.md's CPU-benchmark
// entry from earlier this session for the (x86, non-representative but
// directionally useful) measurement this estimate is based on; the
// authoritative number is ssb_dsp_get_profile()'s max_audio_fx_us on real
// hardware.
static inline float IRAM_ATTR piecewise3_interp(const float *bp_x, const float *bp_y, float a)
{
    if (a <= bp_x[1]) {
        return bp_y[0] + (a - bp_x[0]) * (bp_y[1] - bp_y[0]) / (bp_x[1] - bp_x[0]);
    } else if (a <= bp_x[2]) {
        return bp_y[1] + (a - bp_x[1]) * (bp_y[2] - bp_y[1]) / (bp_x[2] - bp_x[1]);
    } else {
        // Segment 3 extends past bp_x[3] too (linear extrapolation) rather
        // than hard-clamping here - ssb_dsp_process_sample()'s output clamp
        // (see that function) is what actually protects against over-range
        // output, so this doesn't need its own separate ceiling logic.
        return bp_y[2] + (a - bp_x[2]) * (bp_y[3] - bp_y[2]) / (bp_x[3] - bp_x[2]);
    }
}

// Feed-forward soft-knee compressor: env decides HOW MUCH gain reduction
// applies (via the 3-segment piecewise curve above), that gain is applied
// to the raw (signed) sample x, then a FIXED per-level makeup gain (see
// ssb_dsp_set_compressor_level()) restores level. No log/exp/pow per
// sample - attack/release coefficients (expf, at init/reconfigure) and the
// curve's own breakpoint y-values (log10f/powf, only when the level
// changes) are the only transcendental math anywhere near this; the
// per-sample path here is pure compares/mults/adds/one divide.
//
// 2026-09-25: makeup_gain is now a FIXED constant (see compressor_t's own
// comment) - no peak tracker, no per-sample recomputation, replacing the
// 2026-09-24 dynamic peak-tracking scheme entirely (removed along with the
// switchable-mode compressor - see ssb_dsp.h's 2026-09-25 comment above
// the old enum for why).
static inline float IRAM_ATTR compressor_process(compressor_t *c, float x)
{
    float ax = fabsf(x);
    float coef = (ax > c->env) ? c->attack_coef : c->release_coef;
    c->env = flush_denorm(c->env + coef * (ax - c->env));

    float gain;
    if (c->env > c->bp_x[0]) {
        float compressed = piecewise3_interp(c->bp_x, c->bp_y, c->env);
        gain = (c->env > 1e-6f) ? (compressed / c->env) : 1.0f;
    } else {
        gain = 1.0f;
    }

    return x * gain * c->makeup_gain;
}

// Three-stage noise gate - see ssb_dsp_set_squelch_enabled()'s doc comment
// in ssb_dsp.h for the full rationale. Detector tracks level (rejecting
// brief clicks by construction), a Schmitt trigger turns that into a
// hysteretic open/closed decision, and a separately-smoothed gain is what
// actually gets multiplied - the only thing touching the sample value is
// that last, continuous gain, so there is no hard clamp/kink anywhere in
// the signal path itself (the class of bug envelope_floor.cpp's own
// header documents two earlier, reverted designs hitting).
static inline float IRAM_ATTR squelch_process(squelch_t *s, float x)
{
    float ax = fabsf(x);
    float dcoef = (ax > s->detector_env) ? s->detector_attack_coef : s->detector_release_coef;
    s->detector_env = flush_denorm(s->detector_env + dcoef * (ax - s->detector_env));

    float open_thr = s->threshold;
    float close_thr = s->threshold * SSB_DSP_SQUELCH_HYSTERESIS_RATIO;
    if (s->gate_open) {
        if (s->detector_env < close_thr) s->gate_open = false;
    } else {
        if (s->detector_env > open_thr) s->gate_open = true;
    }

    float target_gain = s->gate_open ? 1.0f : 0.0f;
    float gcoef = (target_gain > s->gain) ? s->gain_attack_coef : s->gain_release_coef;
    s->gain = flush_denorm(s->gain + gcoef * (target_gain - s->gain));

    return x * s->gain;
}

struct ssb_dsp_s {
    int num_taps;
    int center;                 // (num_taps - 1) / 2, also the direct-path delay
    float *hilbert_coeffs;      // windowed ideal-Hilbert-transformer taps
    float *delay_line;          // circular buffer of recent audio samples
    int delay_head;             // index of most recently written sample
    float prev_phase;
    bool have_prev_phase;
    float sample_rate_hz;
    float max_freq_dev_hz;

    // See ssb_dsp_get_freq_dev_stats() - running high-water mark of the
    // TRUE (pre-clamp) peak deviation, and how many samples the clamp
    // has actually had to engage on, since init or the last
    // ssb_dsp_reset_freq_dev_stats() call.
    float max_unclamped_freq_dev_hz;
    uint32_t freq_dev_clip_count;

    // Null-bias diagnostic - see ssb_dsp_get_null_bias_stats() in
    // ssb_dsp.h. dphi_sum/dphi_sample_count let the mean per-sample phase
    // delta (hence mean carrier frequency offset) be read directly off
    // the firmware, no SDR needed, to cross-check against real-hardware
    // spectrum measurements. near_null_* restricts the same sum to
    // samples where envelope < null_bias_threshold, to test whether the
    // offset is concentrated at two-tone destructive-interference nulls
    // specifically (as the "offset scales with tone spacing, i.e. with
    // null rate" real-hardware evidence suggests) rather than spread
    // uniformly across the signal.
    float dphi_sum;
    uint32_t dphi_sample_count;
    float near_null_dphi_sum;
    uint32_t near_null_sample_count;
    volatile float null_bias_threshold;

    // Envelope^2-weighted companion to dphi_sum above - see
    // ssb_dsp_null_bias_stats_t's env2_dphi_sum/env2_sum doc comment in
    // ssb_dsp.h. This is the quantity that actually corresponds to where
    // an SDR/spectrum analyzer sees the signal's energy centered: for an
    // analytic signal A(t)e^{jphi(t)}, the energy-weighted average of
    // instantaneous frequency equals the power spectrum's centroid - a
    // standard identity, NOT the same thing as dphi_sum's plain unweighted
    // average. Real hardware confirmed the plain average (100s of Hz) does
    // NOT show up as a same-size on-air shift (tones measured near their
    // correct frequency, deviations in Hz not 100s) - exactly what this
    // predicts, since the huge near-null dphi excursions get suppressed by
    // their own near-zero envelope^2 weight here, where they dominated the
    // unweighted sum.
    float env2_dphi_sum;
    float env2_sum;

    // See ssb_dsp_set_freq_dev_slew_limit_hz()'s doc comment in ssb_dsp.h.
    // freq_dev_slew_limit_hz is the configured limit (SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ
    // = off), written occasionally from a command handler hence volatile,
    // same reasoning as master_gain_db below. slew_limited_prev_freq_dev_hz
    // is the limiter's own internal per-sample state (the last value it
    // actually output) - real-time-path-only, never touched from outside
    // ssb_dsp_process_sample(), so it does NOT need to be volatile.
    volatile float freq_dev_slew_limit_hz;
    float slew_limited_prev_freq_dev_hz;

    // audio_fx_configured: was the EQ/compressor subsystem set up at all
    // at ssb_dsp_init() (i.e. was ssb_audio_fx_config_t::enable true)?
    // This gates whether the biquad/compressor state even exists - if
    // false, eq_enable/comp_enable below are meaningless and all the
    // toggle functions are no-ops.
    //
    // eq_enable/comp_enable: independently runtime-toggleable (e.g. via
    // serial command) once configured - volatile since they're written
    // from a different context (a command handler) than the real-time
    // path that reads them in ssb_dsp_process_sample(), same reasoning
    // as the volatile mode flags elsewhere in this project. Both default
    // to audio_fx_configured's value at init (i.e. "on" if the subsystem
    // was configured at all, until explicitly toggled).
    bool audio_fx_configured;
    volatile bool eq_enable;
    volatile bool comp_enable;
    biquad_t eq_hpf;
    biquad_t eq_presence;
    compressor_t comp;

    // Master gain trim - always available, independent of
    // audio_fx_configured (works even in raw passthrough). Unlike the
    // compressor's makeup gain, this is deliberately MANUAL rather than
    // automatic: EQ's actual effect on perceived level depends on how
    // much of the real program spectrum sits near presence_freq_hz,
    // which isn't something we can compute correctly without knowing the
    // input signal - a guessed "compensation" number would just be
    // wrong. This is the tool for trimming that out by ear/scope
    // instead, e.g. via serial '+'/'-' commands.
    // master_gain_db is what get/set work in (human-friendly); linear is
    // precomputed by the setter and is what the per-sample path actually
    // multiplies by, keeping the hot path a single plain multiply.
    volatile float master_gain_db;
    volatile float master_gain_linear;

    // 2026-09-25: Mic Gain - same exact pattern as master_gain_db/linear
    // just above, applied at the OTHER end of the chain (see
    // ssb_dsp_process_sample()) - before compressor/EQ instead of after.
    // See ssb_dsp_set_mic_gain_db()'s doc comment in ssb_dsp.h for why this
    // was missing and what it fixes. Always available, independent of
    // audio_fx_configured - useful even with EQ/compressor both off.
    volatile float mic_gain_db;
    volatile float mic_gain_linear;

    // 2026-09-25: Mic Squelch - see ssb_dsp_set_squelch_enabled()'s doc
    // comment in ssb_dsp.h. Unconditional/always-available, same reasoning
    // as mic_gain_db just above (a mic-input-quality fix, not part of the
    // audio_fx_configured EQ/compressor subsystem).
    squelch_t squelch;

    // Sub-phase timing high-water marks, see ssb_dsp_get_profile().
    uint32_t max_audio_fx_us;
    uint32_t max_fir_us;
    uint32_t max_atan2_us;
    uint32_t max_sqrt_us;
};

// Generate a windowed (Hamming) ideal discrete Hilbert transformer:
//   h[n] = 0                       for (n - center) even
//   h[n] = 2 / (pi * (n - center)) for (n - center) odd
// multiplied by a Hamming window to control stopband ripple / bandwidth.
static void generate_hilbert_coeffs(float *coeffs, int num_taps)
{
    int center = (num_taps - 1) / 2;
    for (int n = 0; n < num_taps; n++) {
        int k = n - center;
        float h;
        if (k == 0 || (k % 2) == 0) {
            h = 0.0f;
        } else {
            h = 2.0f / (M_PI * (float)k);
        }
        float w = 0.54f - 0.46f * cosf(2.0f * M_PI * (float)n / (float)(num_taps - 1));
        coeffs[n] = h * w;
    }
}

esp_err_t ssb_dsp_init(const ssb_dsp_config_t *cfg, ssb_dsp_handle_t *out_handle)
{
    if (!cfg || !out_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->num_taps < 3 || (cfg->num_taps % 2) == 0) {
        ESP_LOGE(TAG, "num_taps must be odd and >= 3 (got %d)", cfg->num_taps);
        return ESP_ERR_INVALID_ARG;
    }

    ssb_dsp_handle_t h = calloc(1, sizeof(struct ssb_dsp_s));
    if (!h) {
        return ESP_ERR_NO_MEM;
    }

    h->num_taps = cfg->num_taps;
    h->center = (cfg->num_taps - 1) / 2;
    h->sample_rate_hz = (float)cfg->sample_rate_hz;
    h->max_freq_dev_hz = cfg->max_freq_dev_hz > 0.0f ? cfg->max_freq_dev_hz : 3000.0f;
    h->max_unclamped_freq_dev_hz = 0.0f;
    h->freq_dev_clip_count = 0;
    h->dphi_sum = 0.0f;
    h->dphi_sample_count = 0;
    h->near_null_dphi_sum = 0.0f;
    h->near_null_sample_count = 0;
    h->env2_dphi_sum = 0.0f;
    h->env2_sum = 0.0f;
    h->null_bias_threshold = 0.05f;   // see ssb_dsp_set_null_bias_threshold() - envelope is
                                        // roughly [0,1] for full-scale input, so this starts at
                                        // ~5% of full scale; tune live if it catches too few/many
                                        // samples for a given signal's actual peak envelope.
    h->have_prev_phase = false;
    h->prev_phase = 0.0f;
    h->delay_head = 0;
    h->freq_dev_slew_limit_hz = SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ;
    h->slew_limited_prev_freq_dev_hz = 0.0f;

    h->hilbert_coeffs = calloc(h->num_taps, sizeof(float));
    h->delay_line = calloc(h->num_taps, sizeof(float));
    if (!h->hilbert_coeffs || !h->delay_line) {
        free(h->hilbert_coeffs);
        free(h->delay_line);
        free(h);
        return ESP_ERR_NO_MEM;
    }

    generate_hilbert_coeffs(h->hilbert_coeffs, h->num_taps);

    h->master_gain_db = 0.0f;
    h->master_gain_linear = 1.0f;
    h->mic_gain_db = 0.0f;
    h->mic_gain_linear = 1.0f;

    // 2026-09-25: Mic Squelch defaults - OFF (existing behavior unchanged
    // until deliberately opted into), threshold at a conservative starting
    // point (see ssb_dsp_set_squelch_enabled()'s doc comment - this is a
    // reasoned first cut, not a measured optimum, tune on the bench).
    // Coefficients computed once here (expf() at init only, never in the
    // per-sample squelch_process() path), same convention as the
    // compressor's own attack/release coefficients below.
    h->squelch.enable = false;
    h->squelch.threshold = 0.01f;
    h->squelch.detector_attack_coef  = 1.0f - expf(-1.0f / (h->sample_rate_hz * (SSB_DSP_SQUELCH_ATTACK_MS / 1000.0f)));
    h->squelch.detector_release_coef = 1.0f - expf(-1.0f / (h->sample_rate_hz * (SSB_DSP_SQUELCH_RELEASE_MS / 1000.0f)));
    h->squelch.detector_env = 0.0f;
    h->squelch.gate_open = false;
    h->squelch.gain_attack_coef  = 1.0f - expf(-1.0f / (h->sample_rate_hz * (SSB_DSP_SQUELCH_GAIN_ATTACK_MS / 1000.0f)));
    h->squelch.gain_release_coef = 1.0f - expf(-1.0f / (h->sample_rate_hz * (SSB_DSP_SQUELCH_GAIN_RELEASE_MS / 1000.0f)));
    h->squelch.gain = 0.0f;

    h->audio_fx_configured = cfg->audio_fx.enable;
    h->eq_enable = cfg->audio_fx.enable;     // default both stages "on" if configured at all -
    h->comp_enable = cfg->audio_fx.enable;   // individually toggled later via the setters below
    if (h->audio_fx_configured) {
        float hpf_fc      = cfg->audio_fx.hpf_freq_hz > 0.0f ? cfg->audio_fx.hpf_freq_hz : 300.0f;
        float presence_fc = cfg->audio_fx.presence_freq_hz > 0.0f ? cfg->audio_fx.presence_freq_hz : 2200.0f;
        float presence_q  = cfg->audio_fx.presence_q > 0.0f ? cfg->audio_fx.presence_q : 1.0f;
        float attack_ms   = cfg->audio_fx.comp_attack_ms > 0.0f ? cfg->audio_fx.comp_attack_ms : 3.0f;
        float release_ms  = cfg->audio_fx.comp_release_ms > 0.0f ? cfg->audio_fx.comp_release_ms : 120.0f;

        biquad_set_highpass(&h->eq_hpf, hpf_fc, h->sample_rate_hz, 0.707f);
        biquad_set_peaking(&h->eq_presence, presence_fc, h->sample_rate_hz, presence_q,
                            cfg->audio_fx.presence_gain_db);

        // One-pole envelope-follower coefficients: expf() called here at
        // init only, never in the per-sample compressor_process() path.
        h->comp.attack_coef  = 1.0f - expf(-1.0f / (h->sample_rate_hz * (attack_ms / 1000.0f)));
        h->comp.release_coef = 1.0f - expf(-1.0f / (h->sample_rate_hz * (release_ms / 1000.0f)));
        h->comp.env = 0.0f;

        // 2026-09-25: fixed/shared breakpoint x-locations - see
        // SSB_DSP_COMP_THRESHOLD/SSB_DSP_COMP_XMAX and compressor_t's own
        // comment. Set once here, never touched again (not even by
        // ssb_dsp_set_compressor_level(), which only ever rewrites bp_y).
        h->comp.bp_x[0] = SSB_DSP_COMP_THRESHOLD;
        h->comp.bp_x[1] = SSB_DSP_COMP_THRESHOLD + (SSB_DSP_COMP_XMAX - SSB_DSP_COMP_THRESHOLD) / 3.0f;
        h->comp.bp_x[2] = SSB_DSP_COMP_THRESHOLD + 2.0f * (SSB_DSP_COMP_XMAX - SSB_DSP_COMP_THRESHOLD) / 3.0f;
        h->comp.bp_x[3] = SSB_DSP_COMP_XMAX;

        // Default to level 0 (plain limiter, no boost) - a deliberate
        // behavior change from the old fixed-makeup-gain design (see
        // moving_forward_notes.md's 2026-09-25 entry): dial in
        // ssb_dsp_set_compressor_level() explicitly to get any RMS boost.
        ssb_dsp_set_compressor_level(h, SSB_DSP_COMP_LEVEL_MIN);

        ESP_LOGI(TAG, "ssb_dsp audio_fx enabled: hpf=%.0fHz presence=%.0fHz/%+.1fdB/Q%.2f "
                 "comp=thresh%.2f/atk%.1fms/rel%.1fms, level=%d (0=limiter-only default)",
                 hpf_fc, presence_fc, cfg->audio_fx.presence_gain_db, presence_q,
                 (double)SSB_DSP_COMP_THRESHOLD, attack_ms, release_ms, SSB_DSP_COMP_LEVEL_MIN);
    }

    *out_handle = h;
    ESP_LOGI(TAG, "ssb_dsp initialized: taps=%d group_delay=%d samples, fs=%.0fHz, max_dev=%.0fHz",
             h->num_taps, h->center, h->sample_rate_hz, h->max_freq_dev_hz);
    return ESP_OK;
}

void IRAM_ATTR ssb_dsp_set_eq_enabled(ssb_dsp_handle_t handle, bool enable)
{
    if (!handle || !handle->audio_fx_configured) return;
    bool was_on = handle->eq_enable;
    handle->eq_enable = enable;
    // 2026-09-11: reset both biquads' state on re-enable - mirrors
    // ssb_dsp_set_compressor_enabled()'s identical fix just below, which
    // this one should have matched from the start. eq_hpf/eq_presence only
    // update their own x1/x2/y1/y2 state inside biquad_process(), which
    // isn't called at all while eq_enable is false (see the
    // audio_fx_configured block in ssb_dsp_process_sample()) - so that
    // state sits frozen, not decaying, while eq is off. Without this,
    // re-enabling fed that stale (possibly large, whatever it happened to
    // be at the instant eq was switched off) state straight into the very
    // next sample's output - a real discontinuity at the toggle instant,
    // not "forever" (these are BIBO-stable filters for normal coefficients,
    // so the stale-state transient decays within a few dozen samples), but
    // still a genuine bug, and exactly the class of "can a filter's own
    // memory get stuck on a bad value" question raised this session -
    // found while checking whether that's possible anywhere in this IIR
    // chain. See moving_forward_notes.md's 2026-09-11 entry.
    if (enable && !was_on) {
        handle->eq_hpf.x1 = handle->eq_hpf.x2 = handle->eq_hpf.y1 = handle->eq_hpf.y2 = 0.0f;
        handle->eq_presence.x1 = handle->eq_presence.x2 = handle->eq_presence.y1 = handle->eq_presence.y2 = 0.0f;
    }
}

void IRAM_ATTR ssb_dsp_set_compressor_enabled(ssb_dsp_handle_t handle, bool enable)
{
    if (!handle || !handle->audio_fx_configured) return;
    handle->comp_enable = enable;
    // Reset the envelope follower on re-enable so it doesn't resume from
    // a stale value that may have drifted while disabled (the follower
    // keeps running its OWN state update only inside compressor_process,
    // which isn't called at all while comp_enable is false, so env is
    // simply frozen, not decaying - starting clean avoids a jump/thump).
    // 2026-09-25: makeup_gain is no longer reset here - it's a fixed
    // per-level constant now (see ssb_dsp_set_compressor_level()), not
    // state that drifts while disabled, so there's nothing stale to clear.
    if (enable) {
        handle->comp.env = 0.0f;
    }
}

bool ssb_dsp_get_eq_enabled(ssb_dsp_handle_t handle)
{
    return handle && handle->audio_fx_configured && handle->eq_enable;
}

bool ssb_dsp_get_compressor_enabled(ssb_dsp_handle_t handle)
{
    return handle && handle->audio_fx_configured && handle->comp_enable;
}

// 2026-09-25: recomputes bp_y (the 3-segment curve's actual shape) from
// this level's calibrated ratio, and copies in its fixed makeup_gain - see
// this function's doc comment in ssb_dsp.h for the full design. log10f/
// powf here are fine: this only runs when the level changes (a rare,
// human-triggered event via a serial command), never per audio sample.
// No env reset - switching level doesn't stop/restart the compressor the
// way enable/disable does, so there's no frozen/stale state to clean up,
// only a different curve/makeup being applied to state that's already
// live and valid (same reasoning the old mode-switch function had).
void IRAM_ATTR ssb_dsp_set_compressor_level(ssb_dsp_handle_t handle, int level_db)
{
    if (!handle || !handle->audio_fx_configured) return;
    if (level_db < SSB_DSP_COMP_LEVEL_MIN) level_db = SSB_DSP_COMP_LEVEL_MIN;
    if (level_db > SSB_DSP_COMP_LEVEL_MAX) level_db = SSB_DSP_COMP_LEVEL_MAX;

    const comp_level_entry_t *e = &k_comp_level_table[level_db - SSB_DSP_COMP_LEVEL_MIN];
    float thr_db = 20.0f * log10f(handle->comp.bp_x[0]);
    for (int i = 0; i < SSB_DSP_COMP_BP_COUNT; i++) {
        float a_db = 20.0f * log10f(handle->comp.bp_x[i]);
        float out_db = thr_db + (a_db - thr_db) / e->ratio;
        handle->comp.bp_y[i] = powf(10.0f, out_db / 20.0f);
    }
    handle->comp.makeup_gain = e->makeup_gain;
    handle->comp.level_db = level_db;
}

int ssb_dsp_get_compressor_level(ssb_dsp_handle_t handle)
{
    if (!handle || !handle->audio_fx_configured) return SSB_DSP_COMP_LEVEL_MIN;
    return handle->comp.level_db;
}

void ssb_dsp_get_iir_canary(ssb_dsp_handle_t handle, ssb_dsp_iir_canary_t *out)
{
    if (!out) return;
    if (!handle || !handle->audio_fx_configured) {
        // Nothing was ever set up (see ssb_dsp_set_eq_enabled()'s own
        // early-return for the same guard) - report healthy rather than
        // reading uninitialized/zeroed struct fields that were never
        // actually driven by any filter math.
        out->eq_hpf_finite = true;
        out->eq_presence_finite = true;
        out->compressor_env_finite = true;
        return;
    }
    out->eq_hpf_finite = isfinite(handle->eq_hpf.y1) && isfinite(handle->eq_hpf.y2);
    out->eq_presence_finite = isfinite(handle->eq_presence.y1) && isfinite(handle->eq_presence.y2);
    // 2026-09-25: makeup_gain is a fixed per-level constant now (not
    // per-sample-computed state), but still worth including here - a
    // corrupted comp struct (e.g. stack/heap smash) would show up in any
    // of its fields, and this canary is cheap either way.
    out->compressor_env_finite = isfinite(handle->comp.env) && isfinite(handle->comp.makeup_gain);
}

void IRAM_ATTR ssb_dsp_set_master_gain_db(ssb_dsp_handle_t handle, float gain_db)
{
    if (!handle) return;
    handle->master_gain_db = gain_db;
    handle->master_gain_linear = powf(10.0f, gain_db / 20.0f);
}

float ssb_dsp_get_master_gain_db(ssb_dsp_handle_t handle)
{
    return handle ? handle->master_gain_db : 0.0f;
}

void IRAM_ATTR ssb_dsp_set_mic_gain_db(ssb_dsp_handle_t handle, float gain_db)
{
    if (!handle) return;
    handle->mic_gain_db = gain_db;
    handle->mic_gain_linear = powf(10.0f, gain_db / 20.0f);
}

float ssb_dsp_get_mic_gain_db(ssb_dsp_handle_t handle)
{
    return handle ? handle->mic_gain_db : 0.0f;
}

void IRAM_ATTR ssb_dsp_set_squelch_enabled(ssb_dsp_handle_t handle, bool enable)
{
    if (!handle) return;
    handle->squelch.enable = enable;
    // Reset state on enable so a stale gain/gate-open value from a
    // previous session doesn't briefly pass or block audio incorrectly
    // the instant this is turned back on - same "start closed, prove
    // there's a signal" conservative default ssb_dsp_init() uses.
    if (enable) {
        handle->squelch.gate_open = false;
        handle->squelch.gain = 0.0f;
        handle->squelch.detector_env = 0.0f;
    }
}

bool ssb_dsp_get_squelch_enabled(ssb_dsp_handle_t handle)
{
    return handle ? handle->squelch.enable : false;
}

void IRAM_ATTR ssb_dsp_set_squelch_threshold(ssb_dsp_handle_t handle, float threshold)
{
    if (!handle) return;
    if (threshold < SSB_DSP_SQUELCH_THRESHOLD_MIN) threshold = SSB_DSP_SQUELCH_THRESHOLD_MIN;
    if (threshold > SSB_DSP_SQUELCH_THRESHOLD_MAX) threshold = SSB_DSP_SQUELCH_THRESHOLD_MAX;
    handle->squelch.threshold = threshold;
}

float ssb_dsp_get_squelch_threshold(ssb_dsp_handle_t handle)
{
    return handle ? handle->squelch.threshold : 0.0f;
}

void ssb_dsp_get_freq_dev_stats(ssb_dsp_handle_t handle, ssb_dsp_freq_dev_stats_t *out)
{
    if (!out) return;
    if (!handle) { out->max_unclamped_freq_dev_hz = 0.0f; out->clip_count = 0; return; }
    out->max_unclamped_freq_dev_hz = handle->max_unclamped_freq_dev_hz;
    out->clip_count = handle->freq_dev_clip_count;
}

void ssb_dsp_reset_freq_dev_stats(ssb_dsp_handle_t handle)
{
    if (!handle) return;
    handle->max_unclamped_freq_dev_hz = 0.0f;
    handle->freq_dev_clip_count = 0;
    handle->dphi_sum = 0.0f;
    handle->dphi_sample_count = 0;
    handle->near_null_dphi_sum = 0.0f;
    handle->near_null_sample_count = 0;
    handle->env2_dphi_sum = 0.0f;
    handle->env2_sum = 0.0f;
}

void ssb_dsp_get_null_bias_stats(ssb_dsp_handle_t handle, ssb_dsp_null_bias_stats_t *out)
{
    if (!out) return;
    if (!handle) {
        out->dphi_sum = 0.0f;
        out->dphi_sample_count = 0;
        out->near_null_dphi_sum = 0.0f;
        out->near_null_sample_count = 0;
        out->env2_dphi_sum = 0.0f;
        out->env2_sum = 0.0f;
        return;
    }
    out->dphi_sum = handle->dphi_sum;
    out->dphi_sample_count = handle->dphi_sample_count;
    out->near_null_dphi_sum = handle->near_null_dphi_sum;
    out->near_null_sample_count = handle->near_null_sample_count;
    out->env2_dphi_sum = handle->env2_dphi_sum;
    out->env2_sum = handle->env2_sum;
}

void IRAM_ATTR ssb_dsp_set_null_bias_threshold(ssb_dsp_handle_t handle, float threshold)
{
    if (!handle) return;
    if (threshold < 0.0f) threshold = 0.0f;
    handle->null_bias_threshold = threshold;
}

// 2026-09-12: marked IRAM_ATTR (previously wasn't, unlike the setter above)
// because diagnostics.cpp's new per-event jump log now calls this from
// diagnostics_set_tx_info(), which runs on the dsp_task hot path - same
// IRAM discipline this codebase applies everywhere else on that path.
// Trivial single-field read, so this is a pure classification change, not
// a behavior change.
float IRAM_ATTR ssb_dsp_get_null_bias_threshold(ssb_dsp_handle_t handle)
{
    return handle ? handle->null_bias_threshold : 0.05f;
}

// Step size and bounds for '{'/'}' - see ssb_dsp.h's doc comment. Step
// chosen well above the ~60Hz/sample worst-case seen on real (non-null)
// two-tone content, so every step in the useful range stays meaningfully
// coarser than that floor. MIN sits comfortably above that same ~60Hz/
// sample figure too, so even the tightest reachable setting shouldn't
// start touching legitimate content. MAX_FINITE is the last step before
// snapping to fully off - a limit that high can already barely ever
// engage (raw freq_dev itself never exceeds max_freq_dev_hz, ~8000Hz, so
// a same-sign single-sample swing that large is already the largest
// possible), it exists as one more step on the way to off rather than a
// functionally meaningful setting of its own.
#define FREQ_DEV_SLEW_STEP_HZ        250.0f
#define FREQ_DEV_SLEW_MIN_HZ         100.0f
#define FREQ_DEV_SLEW_MAX_FINITE_HZ 8000.0f
// First value dialed in when going from off -> on via '{' - comfortably
// above real content's own worst-case slew, comfortably below a null
// event's ~8000Hz/sample, so it actually engages only where intended.
#define FREQ_DEV_SLEW_START_HZ      2000.0f

void IRAM_ATTR ssb_dsp_set_freq_dev_slew_limit_hz(ssb_dsp_handle_t handle, float limit_hz)
{
    if (!handle) return;
    if (limit_hz < FREQ_DEV_SLEW_MIN_HZ) limit_hz = FREQ_DEV_SLEW_MIN_HZ;
    handle->freq_dev_slew_limit_hz = limit_hz;
}

float ssb_dsp_get_freq_dev_slew_limit_hz(ssb_dsp_handle_t handle)
{
    return handle ? handle->freq_dev_slew_limit_hz : SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ;
}

void ssb_dsp_raise_freq_dev_slew_limit(ssb_dsp_handle_t handle)
{
    if (!handle) return;
    float cur = handle->freq_dev_slew_limit_hz;
    if (cur >= FREQ_DEV_SLEW_MAX_FINITE_HZ) {
        handle->freq_dev_slew_limit_hz = SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ;  // one more step past the
                                                                                // last finite step = off
    } else {
        handle->freq_dev_slew_limit_hz = cur + FREQ_DEV_SLEW_STEP_HZ;
    }
}

void ssb_dsp_lower_freq_dev_slew_limit(ssb_dsp_handle_t handle)
{
    if (!handle) return;
    float cur = handle->freq_dev_slew_limit_hz;
    if (cur >= SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ) {
        handle->freq_dev_slew_limit_hz = FREQ_DEV_SLEW_START_HZ;  // off -> on at a sane starting point,
                                                                    // not a step down from a huge number
    } else {
        float next = cur - FREQ_DEV_SLEW_STEP_HZ;
        handle->freq_dev_slew_limit_hz = next < FREQ_DEV_SLEW_MIN_HZ ? FREQ_DEV_SLEW_MIN_HZ : next;
    }
}

void ssb_dsp_get_profile(ssb_dsp_handle_t handle, ssb_dsp_profile_t *out)
{
    if (!handle || !out) return;
    out->max_audio_fx_us = handle->max_audio_fx_us;
    out->max_fir_us = handle->max_fir_us;
    out->max_atan2_us = handle->max_atan2_us;
    out->max_sqrt_us = handle->max_sqrt_us;
}

int ssb_dsp_group_delay_samples(ssb_dsp_handle_t handle)
{
    return handle ? handle->center : 0;
}

static inline float wrap_pi(float x)
{
    while (x > M_PI)  x -= 2.0f * M_PI;
    while (x < -M_PI) x += 2.0f * M_PI;
    return x;
}

void IRAM_ATTR ssb_dsp_process_sample(ssb_dsp_handle_t handle,
                             float audio_sample,
                             ssb_sideband_t sideband,
                             float *out_freq_dev_hz,
                             float *out_envelope)
{
    int N = handle->num_taps;

    // 2026-09-25: Mic Gain - unconditional, applied BEFORE compressor/EQ,
    // works even with audio_fx_configured false (raw passthrough), same
    // "always available" reasoning as master_gain_linear below. This is
    // the stage that was missing - see ssb_dsp_set_mic_gain_db()'s doc
    // comment in ssb_dsp.h. Single multiply; mic_gain_linear==1.0f (the
    // default) costs nothing worth measuring.
    int64_t t0 = esp_timer_get_time();
    audio_sample *= handle->mic_gain_linear;

    // 2026-09-25: Mic Squelch - deliberately BEFORE the compressor, not
    // downstream on the envelope like envelope_floor/ALC/Soft-Limit - see
    // ssb_dsp_set_squelch_enabled()'s doc comment in ssb_dsp.h for why
    // ordering matters here specifically (the compressor's makeup_gain
    // multiplies every sample unconditionally, so gating after it would
    // mean gating already-amplified noise). Unconditional check on
    // squelch.enable, independent of audio_fx_configured - same "always
    // available" reasoning as mic gain just above.
    if (handle->squelch.enable) audio_sample = squelch_process(&handle->squelch, audio_sample);

    // Optional pre-Hilbert conditioning: compressor -> EQ, on the raw
    // sample, before anything enters the Hilbert delay line. This keeps
    // it fully decoupled from the Hilbert filter's `center`-tap group
    // delay - the Hilbert path never sees an unconditioned sample.
    // EQ and compressor are checked independently now (eq_enable,
    // comp_enable) so each can be A/B'd live via serial command without
    // recompiling - see ssb_dsp_set_eq_enabled()/ssb_dsp_set_compressor_enabled().
    if (handle->audio_fx_configured) {
        if (handle->comp_enable) audio_sample = compressor_process(&handle->comp, audio_sample);
        if (handle->eq_enable) {
            audio_sample = biquad_process(&handle->eq_hpf, audio_sample);
            audio_sample = biquad_process(&handle->eq_presence, audio_sample);
        }
        // 2026-09-25: hard output safety clamp - added specifically because
        // offline calibration against the user's real mic-to-ADC recording
        // (G4AHN.wav) measured genuine full-scale overshoot (up to 1.79 at
        // the most aggressive tested compression level) that a "worst-case
        // steady-state envelope" analysis alone did NOT predict. Mechanism:
        // compressor_process()'s gain decision is based on the SMOOTHED
        // envelope (env), but applied to the INSTANTANEOUS raw sample - a
        // fast attack transient can momentarily exceed env before the
        // envelope follower catches up (a well-known feed-forward-
        // compressor limitation; a look-ahead design would avoid it at the
        // cost of added latency, not attempted here). This clamp is the
        // actual backstop against that, not the fixed per-level makeup
        // gain table alone - see ssb_dsp_set_compressor_level()'s doc
        // comment in ssb_dsp.h for the full numeric finding. Placed after
        // EQ too, since the presence-peak boost can also add a little
        // level on top of whatever the compressor produced.
        if (audio_sample > 1.0f) audio_sample = 1.0f;
        else if (audio_sample < -1.0f) audio_sample = -1.0f;
    }
    // Master gain - unconditional, works even with audio_fx_configured
    // false (raw passthrough). Single multiply; master_gain_linear==1.0f
    // (the default) costs nothing worth measuring, so this doesn't need
    // its own guard the way EQ/compressor do. Deliberately AFTER the
    // safety clamp above - this is the RF/output power control, not part
    // of what that clamp is protecting (a user setting master_gain_db > 0
    // is a deliberate choice, not the transient-overshoot bug being
    // guarded against).
    audio_sample *= handle->master_gain_linear;
    int64_t t1 = esp_timer_get_time();
    uint32_t audio_fx_us = (uint32_t)(t1 - t0);
    if (audio_fx_us > handle->max_audio_fx_us) handle->max_audio_fx_us = audio_fx_us;

    // Push new sample into circular delay line (most recent at delay_head).
    // Flushed here, not just on readback below, since a denormal value
    // sitting in the buffer gets multiplied against every one of the
    // num_taps Hilbert coefficients as it ages through - one flush here
    // covers the whole FIR sum below, not just the direct I-path read.
    //
    // Branch instead of modulo for the wrap: Xtensa has no hardware
    // integer divider, so % is a real (costly) division. A single
    // modulo here is cheap on its own, but kept as a branch anyway for
    // consistency with the FIR loop below, where the same pattern
    // repeated N times was a real cost.
    int head = handle->delay_head + 1;
    if (head >= N) head = 0;
    handle->delay_head = head;
    handle->delay_line[head] = flush_denorm(audio_sample);

    // Direct ("I") path: the sample that is `center` samples old, i.e. time-
    // aligned with the Hilbert filter's group delay.
    int i_idx = head - handle->center;
    if (i_idx < 0) i_idx += N;
    float I = handle->delay_line[i_idx];

    // Hilbert ("Q") path: convolve the whole delay line with the Hilbert
    // taps. delay_line[(head - n + N) % N] holds the sample that is n
    // steps old, so this computes sum_n coeff[n] * x[k - n], a standard
    // FIR - but computing that modulo N times per sample (once per tap)
    // was a meaningful chunk of the measured dsp_us budget, since Xtensa
    // has no hardware integer divider. Split into two modulo-free,
    // contiguous ranges over the same circular buffer instead - the
    // first covers n=0..head (idx counts down from head to 0, no wrap
    // needed), the second covers n=head+1..N-1 (idx counts down from
    // N-1 to head+1, the wrapped portion, needing only a plain add of N
    // rather than a modulo). Mathematically identical to the original
    // per-tap modulo formula - verified against it at both the head=0
    // and head=N-1 wrap boundaries.
    //
    // I/Q are flushed here (not just the new EQ/compressor state above)
    // because they decay toward zero during silence the same way - this
    // path predates today's changes, consistent with the "very occasional,
    // pre-existing" overruns rather than something newly introduced.
    float Q = 0.0f;
    int n = 0;
    for (; n <= head; n++) {
        Q += handle->hilbert_coeffs[n] * handle->delay_line[head - n];
    }
    for (; n < N; n++) {
        Q += handle->hilbert_coeffs[n] * handle->delay_line[head - n + N];
    }
    Q = flush_denorm(Q);
    int64_t t2 = esp_timer_get_time();
    uint32_t fir_us = (uint32_t)(t2 - t1);
    if (fir_us > handle->max_fir_us) handle->max_fir_us = fir_us;

#if SSB_DSP_FAST_TRIG
    float phase = fast_atan2(Q, I);
#else
    float phase = atan2f(Q, I);
#endif
    int64_t t3 = esp_timer_get_time();
    uint32_t atan2_us = (uint32_t)(t3 - t2);
    if (atan2_us > handle->max_atan2_us) handle->max_atan2_us = atan2_us;

#if SSB_DSP_FAST_TRIG
    float envelope = fast_sqrt(I * I + Q * Q);
#else
    float envelope = sqrtf(I * I + Q * Q);
#endif
    int64_t t4 = esp_timer_get_time();
    uint32_t sqrt_us = (uint32_t)(t4 - t3);
    if (sqrt_us > handle->max_sqrt_us) handle->max_sqrt_us = sqrt_us;

    float dphi = 0.0f;
    if (handle->have_prev_phase) {
        dphi = wrap_pi(phase - handle->prev_phase);

        // Null-bias diagnostic accumulator (see ssb_dsp_get_null_bias_stats()
        // in ssb_dsp.h) - deliberately BEFORE slew/clamp below, same as
        // max_unclamped_freq_dev_hz, so this reflects the true wrap_pi output
        // exactly as fast_atan2/atan2f and this file's own null-crossing
        // phase disambiguation produced it.
        //
        // CORRECTION (confirmed against real hardware): dphi_sum's plain
        // unweighted mean is NOT what an SDR reads as the carrier's average
        // frequency offset - real two-tone transmissions measured near
        // their correct frequency (deviations in Hz, not the 100s of Hz
        // this plain average predicted). The physically correct quantity is
        // the ENVELOPE^2-WEIGHTED average of instantaneous frequency - a
        // standard identity (for an analytic signal A(t)e^{jphi(t)}, the
        // power spectrum's centroid equals the energy-weighted average of
        // (1/2pi)dphi/dt, NOT the plain time-average). env2_dphi_sum/
        // env2_sum below accumulate exactly that. The near-null samples
        // that dominated dphi_sum's plain average happen to be exactly
        // where envelope (and so envelope^2) is smallest - the correct
        // weighting suppresses their contribution almost entirely, which is
        // exactly why the plain average overstated things so badly.
        // dphi_sum/near_null_dphi_sum are kept for mechanistic diagnosis
        // (which samples the effect concentrates at) - see
        // ssb_dsp_null_bias_stats_t in ssb_dsp.h for both.
        handle->dphi_sum += dphi;
        handle->dphi_sample_count++;
        if (envelope < handle->null_bias_threshold) {
            handle->near_null_dphi_sum += dphi;
            handle->near_null_sample_count++;
        }
        float env2 = envelope * envelope;
        handle->env2_dphi_sum += env2 * dphi;
        handle->env2_sum += env2;
    }
    handle->prev_phase = phase;
    handle->have_prev_phase = true;

    float freq_dev = dphi * handle->sample_rate_hz / (2.0f * M_PI);

    // Tracked BEFORE clamping - the true peak the signal actually wants
    // to reach, and how often the clamp actually has to intervene. This
    // is what lets max_freq_dev_hz be set from real evidence (via
    // ssb_dsp_get_freq_dev_stats()) instead of another round of guessing
    // a constant and re-measuring on real hardware - confirmed on real
    // hardware that too-tight a clamp doesn't just fail to protect
    // against wild spikes, it can also bias a two-tone signal's average
    // output frequency (asymmetric clipping of otherwise-legitimate
    // content) and directly hurt sideband suppression.
    float abs_freq_dev = fabsf(freq_dev);
    if (abs_freq_dev > handle->max_unclamped_freq_dev_hz) handle->max_unclamped_freq_dev_hz = abs_freq_dev;

    // Slew-rate limit: see ssb_dsp_set_freq_dev_slew_limit_hz()'s doc
    // comment in ssb_dsp.h. Runs AFTER the true-peak diagnostic above (so
    // that stays meaningful) and BEFORE the magnitude clamp below (which
    // still applies on top, unchanged, as a final safety net regardless
    // of this setting). Off (limit_hz == SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ)
    // reduces to freq_dev unchanged every sample - the delta can never
    // exceed a limit that large, so neither branch below ever fires.
    {
        float limit_hz = handle->freq_dev_slew_limit_hz;
        float delta = freq_dev - handle->slew_limited_prev_freq_dev_hz;
        if (delta > limit_hz) delta = limit_hz;
        else if (delta < -limit_hz) delta = -limit_hz;
        freq_dev = handle->slew_limited_prev_freq_dev_hz + delta;
        handle->slew_limited_prev_freq_dev_hz = freq_dev;
    }

    // Clamp: prevents phase noise near zero-crossings of the envelope from
    // producing large spurious instantaneous-frequency spikes (this is the
    // same "restrict the phase changes" step QCX-SSB applies).
    if (freq_dev > handle->max_freq_dev_hz) {
        freq_dev = handle->max_freq_dev_hz;
        handle->freq_dev_clip_count++;
    } else if (freq_dev < -handle->max_freq_dev_hz) {
        freq_dev = -handle->max_freq_dev_hz;
        handle->freq_dev_clip_count++;
    }

    if (sideband == SSB_SIDEBAND_LSB) {
        freq_dev = -freq_dev;
    }

    if (out_freq_dev_hz) *out_freq_dev_hz = freq_dev;
    if (out_envelope)    *out_envelope = envelope;
}

void ssb_dsp_deinit(ssb_dsp_handle_t handle)
{
    if (!handle) return;
    free(handle->hilbert_coeffs);
    free(handle->delay_line);
    free(handle);
}