/**
 * carrier_output.cpp - see carrier_output.h.
 */

#include "carrier_output.h"

#if AD9851_ATTACHED

static ad9851_handle_t s_ad9851;
static volatile uint32_t s_carrier_hz = CARRIER_HZ;

void carrier_output_init(void)
{
    ad9851_config_t ad_cfg = {
        .spi_host = SPI2_HOST,
        .pin_data = AD9851_PIN_DATA,
        .pin_wclk = AD9851_PIN_WCLK,
        .pin_fqud = AD9851_PIN_FQUD,
        .pin_reset = AD9851_PIN_RESET,
        .ref_clk_hz = REF_CLK_HZ,
        .use_6x_multiplier = true,
        // Was 2000000. AD9851's own datasheet timing (7ns min W_CLK
        // period, ~24MHz even derated 6x for the multiplier case) and
        // the ESP32 SPI peripheral both have far more headroom than
        // 2MHz used - but W_CLK/FQ_UD/DATA all pass through discrete
        // BS170 inverting level-shifter stages (AD9851_INVERTING_LEVEL_SHIFT,
        // see AD9851.c) whose real switching speed hasn't been
        // characterized. A conservative 2x step to 4MHz rather than
        // jumping straight to what the chip/ESP32 alone could take -
        // AD9851.h's own TIMING note warns getting this wrong produces
        // silently wrong output with "no obvious symptom short of a
        // spectrum analyser," so verify the transmitted frequency is
        // still exactly correct on the spectrum analyzer after
        // reflashing, before considering pushing this higher.
        .spi_clock_hz = 4000000,
    };
    ESP_ERROR_CHECK(ad9851_init(&ad_cfg, &s_ad9851));
    ad9851_set_frequency(s_ad9851, s_carrier_hz);
}

uint32_t IRAM_ATTR carrier_output_set_freq_dev(float delayed_freq_dev_hz)
{
    uint32_t tx_freq = s_carrier_hz + (int32_t)delayed_freq_dev_hz;
    ad9851_set_frequency(s_ad9851, tx_freq);
    return tx_freq;
}

bool carrier_output_get_rf_enabled(void)
{
    return ad9851_get_output_enabled(s_ad9851);
}

void carrier_output_set_rf_enabled(bool enable)
{
    ad9851_set_output_enabled(s_ad9851, enable);
}

void carrier_output_get_profile(ad9851_profile_t *out)
{
    ad9851_get_profile(s_ad9851, out);
}

#endif // AD9851_ATTACHED
