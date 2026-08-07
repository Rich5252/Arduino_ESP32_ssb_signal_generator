#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ssb_dsp.h"
#include "esp_log.h"


static const char *TAG = "ssb_dsp";

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

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

    *out_handle = h;
    ESP_LOGI(TAG, "ssb_dsp initialized: taps=%d group_delay=%d samples, fs=%.0fHz, max_dev=%.0fHz",
             h->num_taps, h->center, h->sample_rate_hz, h->max_freq_dev_hz);
    return ESP_OK;
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

    // Push new sample into circular delay line (most recent at delay_head)
    handle->delay_head = (handle->delay_head + 1) % N;
    handle->delay_line[handle->delay_head] = audio_sample;

    // Direct ("I") path: the sample that is `center` samples old, i.e. time-
    // aligned with the Hilbert filter's group delay.
    int i_idx = (handle->delay_head - handle->center + N) % N;
    float I = handle->delay_line[i_idx];

    // Hilbert ("Q") path: convolve the whole delay line with the Hilbert taps.
    // delay_line[(delay_head - n + N) % N] holds the sample that is n steps
    // old, so this computes sum_n coeff[n] * x[k - n], a standard FIR.
    float Q = 0.0f;
    for (int n = 0; n < N; n++) {
        int idx = (handle->delay_head - n + N) % N;
        Q += handle->hilbert_coeffs[n] * handle->delay_line[idx];
    }

    float phase = atan2f(Q, I);
    float envelope = sqrtf(I * I + Q * Q);

    float dphi = 0.0f;
    if (handle->have_prev_phase) {
        dphi = wrap_pi(phase - handle->prev_phase);
    }
    handle->prev_phase = phase;
    handle->have_prev_phase = true;

    float freq_dev = dphi * handle->sample_rate_hz / (2.0f * M_PI);

    // Clamp: prevents phase noise near zero-crossings of the envelope from
    // producing large spurious instantaneous-frequency spikes (this is the
    // same "restrict the phase changes" step QCX-SSB applies).
    if (freq_dev > handle->max_freq_dev_hz)  freq_dev = handle->max_freq_dev_hz;
    if (freq_dev < -handle->max_freq_dev_hz) freq_dev = -handle->max_freq_dev_hz;

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
