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

typedef struct {
    float attack_coef, release_coef;   // one-pole envelope-follower coefficients
    float threshold, inv_ratio;
    float env;
    float makeup_gain;   // automatic, see compute_compressor_makeup_gain() - keeps level roughly
                          // consistent whether the compressor is on or off, unlike EQ's gain
                          // (see ssb_dsp_s::master_gain comment for why that one's manual instead)
} compressor_t;

// Standard "unity gain at 0dBFS" makeup gain: for a signal peaking at
// full scale (0dB), the compressor's own gain reduction there is
// threshold_db * (1 - 1/ratio) (both threshold_db and this product are
// negative, i.e. a reduction) - this exactly cancels that, so a
// full-scale peak comes out at roughly the same level whether the
// compressor is engaged or not. Well-defined because threshold/ratio are
// known constants, unlike EQ gain which depends on the input spectrum.
static float compute_compressor_makeup_gain(float threshold, float ratio)
{
    if (threshold <= 0.0f || ratio <= 1.0f) return 1.0f;
    float threshold_db = 20.0f * log10f(threshold);
    float reduction_db = threshold_db * (1.0f - 1.0f / ratio);   // negative
    return powf(10.0f, -reduction_db / 20.0f);                    // boost to cancel it
}

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

// Feed-forward soft limiter above threshold, fixed ratio. No log/exp/pow
// per sample - attack/release coefficients (which DO need one expf each)
// are computed once at init/reconfigure, never per sample.
static inline float IRAM_ATTR compressor_process(compressor_t *c, float x)
{
    float ax = fabsf(x);
    float coef = (ax > c->env) ? c->attack_coef : c->release_coef;
    c->env = flush_denorm(c->env + coef * (ax - c->env));

    float gain;
    if (c->env > c->threshold) {
        float compressed = c->threshold + (c->env - c->threshold) * c->inv_ratio;
        gain = (c->env > 1e-6f) ? (compressed / c->env) : 1.0f;
    } else {
        gain = 1.0f;
    }
    return x * gain * c->makeup_gain;
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
    h->have_prev_phase = false;
    h->prev_phase = 0.0f;
    h->delay_head = 0;

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

    h->audio_fx_configured = cfg->audio_fx.enable;
    h->eq_enable = cfg->audio_fx.enable;     // default both stages "on" if configured at all -
    h->comp_enable = cfg->audio_fx.enable;   // individually toggled later via the setters below
    if (h->audio_fx_configured) {
        float hpf_fc      = cfg->audio_fx.hpf_freq_hz > 0.0f ? cfg->audio_fx.hpf_freq_hz : 300.0f;
        float presence_fc = cfg->audio_fx.presence_freq_hz > 0.0f ? cfg->audio_fx.presence_freq_hz : 2200.0f;
        float presence_q  = cfg->audio_fx.presence_q > 0.0f ? cfg->audio_fx.presence_q : 1.0f;
        float attack_ms   = cfg->audio_fx.comp_attack_ms > 0.0f ? cfg->audio_fx.comp_attack_ms : 3.0f;
        float release_ms  = cfg->audio_fx.comp_release_ms > 0.0f ? cfg->audio_fx.comp_release_ms : 120.0f;
        float threshold   = cfg->audio_fx.comp_threshold > 0.0f ? cfg->audio_fx.comp_threshold : 0.3f;
        float ratio        = cfg->audio_fx.comp_ratio > 1.0f ? cfg->audio_fx.comp_ratio : 3.5f;

        biquad_set_highpass(&h->eq_hpf, hpf_fc, h->sample_rate_hz, 0.707f);
        biquad_set_peaking(&h->eq_presence, presence_fc, h->sample_rate_hz, presence_q,
                            cfg->audio_fx.presence_gain_db);

        // One-pole envelope-follower coefficients: expf() called here at
        // init only, never in the per-sample compressor_process() path.
        h->comp.attack_coef  = 1.0f - expf(-1.0f / (h->sample_rate_hz * (attack_ms / 1000.0f)));
        h->comp.release_coef = 1.0f - expf(-1.0f / (h->sample_rate_hz * (release_ms / 1000.0f)));
        h->comp.threshold = threshold;
        h->comp.inv_ratio = 1.0f / ratio;
        h->comp.env = 0.0f;
        h->comp.makeup_gain = compute_compressor_makeup_gain(threshold, ratio);

        ESP_LOGI(TAG, "ssb_dsp audio_fx enabled: hpf=%.0fHz presence=%.0fHz/%+.1fdB/Q%.2f "
                 "comp=thresh%.2f/ratio%.1f:1/atk%.1fms/rel%.1fms",
                 hpf_fc, presence_fc, cfg->audio_fx.presence_gain_db, presence_q,
                 threshold, ratio, attack_ms, release_ms);
    }

    *out_handle = h;
    ESP_LOGI(TAG, "ssb_dsp initialized: taps=%d group_delay=%d samples, fs=%.0fHz, max_dev=%.0fHz",
             h->num_taps, h->center, h->sample_rate_hz, h->max_freq_dev_hz);
    return ESP_OK;
}

void ssb_dsp_set_compressor(ssb_dsp_handle_t handle, float threshold, float ratio)
{
    if (!handle || !handle->audio_fx_configured) return;
    if (threshold > 0.0f) handle->comp.threshold = threshold;
    if (ratio > 1.0f)     handle->comp.inv_ratio = 1.0f / ratio;
    // Recompute from whatever's now current (not just the just-passed
    // args - either one might have been left unchanged this call).
    float current_ratio = 1.0f / handle->comp.inv_ratio;
    handle->comp.makeup_gain = compute_compressor_makeup_gain(handle->comp.threshold, current_ratio);
}

void IRAM_ATTR ssb_dsp_set_eq_enabled(ssb_dsp_handle_t handle, bool enable)
{
    if (!handle || !handle->audio_fx_configured) return;
    handle->eq_enable = enable;
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
    if (enable) handle->comp.env = 0.0f;
}

bool ssb_dsp_get_eq_enabled(ssb_dsp_handle_t handle)
{
    return handle && handle->audio_fx_configured && handle->eq_enable;
}

bool ssb_dsp_get_compressor_enabled(ssb_dsp_handle_t handle)
{
    return handle && handle->audio_fx_configured && handle->comp_enable;
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

    // Optional pre-Hilbert conditioning: compressor -> EQ, on the raw
    // sample, before anything enters the Hilbert delay line. This keeps
    // it fully decoupled from the Hilbert filter's `center`-tap group
    // delay - the Hilbert path never sees an unconditioned sample.
    // EQ and compressor are checked independently now (eq_enable,
    // comp_enable) so each can be A/B'd live via serial command without
    // recompiling - see ssb_dsp_set_eq_enabled()/ssb_dsp_set_compressor_enabled().
    int64_t t0 = esp_timer_get_time();
    if (handle->audio_fx_configured) {
        if (handle->comp_enable) audio_sample = compressor_process(&handle->comp, audio_sample);
        if (handle->eq_enable) {
            audio_sample = biquad_process(&handle->eq_hpf, audio_sample);
            audio_sample = biquad_process(&handle->eq_presence, audio_sample);
        }
    }
    // Master gain - unconditional, works even with audio_fx_configured
    // false (raw passthrough). Single multiply; master_gain_linear==1.0f
    // (the default) costs nothing worth measuring, so this doesn't need
    // its own guard the way EQ/compressor do.
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