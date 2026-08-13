/**
 * dsp_state.cpp - see dsp_state.h.
 */

#include <math.h>
#include "dsp_state.h"
#include "config.h"

static ssb_dsp_handle_t s_ssb;
static volatile ssb_sideband_t s_sideband = SSB_SIDEBAND_USB;
static volatile audio_source_t s_audio_source = (TWOTONE_TEST_MODE != 0) ? AUDIO_SRC_TWOTONE : AUDIO_SRC_MIC;

// Master gain (set via ssb_dsp_set_master_gain_db, '+'/'-') only applies
// inside ssb_dsp_process_sample() - which AMTEST/ENVSTEP deliberately
// bypass, so it previously had zero effect on their envelope level. This
// cached LINEAR value lets those modes apply the same gain control
// without recomputing powf(10, dB/20) every tick (10kHz) - updated only
// when gain actually changes (see dsp_state_set_master_gain_db(), called
// from the '+'/'-' handlers and setup()'s initial default), not read from
// ssb_dsp every sample.
static volatile float s_master_gain_linear_cache = 1.0f;

esp_err_t dsp_state_init(const ssb_dsp_config_t *cfg)
{
    return ssb_dsp_init(cfg, &s_ssb);
}

ssb_dsp_handle_t IRAM_ATTR dsp_state_get_ssb(void)
{
    return s_ssb;
}

ssb_sideband_t IRAM_ATTR dsp_state_get_sideband(void)
{
    return s_sideband;
}

audio_source_t IRAM_ATTR dsp_state_get_audio_source(void)
{
    return s_audio_source;
}

void dsp_state_set_audio_source(audio_source_t src)
{
    s_audio_source = src;
}

void dsp_state_set_master_gain_db(float db)
{
    ssb_dsp_set_master_gain_db(s_ssb, db);
    s_master_gain_linear_cache = powf(10.0f, db / 20.0f);
}

float IRAM_ATTR dsp_state_get_master_gain_linear(void)
{
    return s_master_gain_linear_cache;
}

const char *audio_source_name(int src)
{
    switch (src) {
        case AUDIO_SRC_TWOTONE:    return "TWO-TONE TEST";
        case AUDIO_SRC_SINGLETONE: return "SINGLE-TONE TEST";
        case AUDIO_SRC_ENVSTEP:    return "ENVELOPE STEP TEST";
        case AUDIO_SRC_FMTEST:     return "FM TEST (AD9851 isolation)";
        case AUDIO_SRC_AMTEST:     return "AM TEST (RSET isolation)";
        default:                   return "mic";
    }
}
