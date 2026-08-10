#include <stdlib.h>
#include "ad9851.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"

// Set once, based on the actual BS170 circuit used: gate driven directly
// by the ESP32 GPIO, source to ground, drain pulled up to the AD9851
// side's rail. This is an INVERTING switch (GPIO high -> MOSFET on ->
// AD9851 side pulled LOW; GPIO low -> MOSFET off -> pulled-up HIGH) -
// confirmed empirically (RESET was reading permanently active/high on
// the AD9851 side despite the driver setting its GPIO low). This is
// different from the more common non-inverting bidirectional BS170
// level-shifter circuit (gate tied to a fixed low-side rail instead of
// driven by the signal) - if you ever rebuild with THAT circuit instead,
// every inversion-compensation below needs removing, not just this flag.
#define AD9851_INVERTING_LEVEL_SHIFT 1

struct ad9851_s {
    spi_device_handle_t spi;
    uint32_t ref_clk_hz;
    bool use_6x_multiplier;
    int pin_reset;

    // Precomputed once at init: ftw = (freq_hz * ftw_reciprocal) >> (FTW_RECIP_SHIFT - 32),
    // algebraically equal to freq_hz * 2^32 / effective_ref_hz but computed via a
    // multiply+shift instead of a 64-bit divide. Xtensa has no hardware integer
    // divider - a divide here would cost real cycles on every single call, and this
    // runs up to 20000 times/sec (once per audio sample) in the phase-modulation
    // architecture. Same principle as the Nano library's calculateFTWIncrement()/
    // setClock() reciprocal technique (there used for precomputed FT4/FT8 tone
    // steps; here for a continuously-varying frequency instead of a fixed set).
    uint64_t ftw_reciprocal;
};

#define AD9851_FTW_RECIP_SHIFT 56   // generous headroom - see ad9851_init() for the
                                     // precision reasoning; not chosen to be minimal

// Hardware SPI sends MSB-first by default; the AD9851 wants LSB-first
// within each byte (confirmed via the proven Nano library's
// SPISettings(..., LSBFIRST, ...)) - reversed in software here rather
// than depending on a specific ESP-IDF LSB-first flag name, to avoid
// getting a memorized macro name wrong on hardware this matters for.
static inline uint8_t IRAM_ATTR reverse_bits8(uint8_t b)
{
    b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
    b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
    b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
    return b;
}

esp_err_t ad9851_init(const ad9851_config_t *cfg, ad9851_handle_t *out_handle)
{
    if (!cfg || !out_handle) {
        return ESP_ERR_INVALID_ARG;
    }

    ad9851_handle_t h = calloc(1, sizeof(struct ad9851_s));
    if (!h) {
        return ESP_ERR_NO_MEM;
    }

    h->ref_clk_hz = cfg->ref_clk_hz;
    h->use_6x_multiplier = cfg->use_6x_multiplier;
    h->pin_reset = cfg->pin_reset;

    // Precompute the Hz-to-FTW reciprocal once here (a one-time divide is
    // fine - it's ad9851_set_frequency(), called up to 20000x/sec, that
    // needs to avoid one). K=56 leaves ample headroom below 2^64 for the
    // multiply in ad9851_set_frequency() even at HF frequencies with
    // margin (freq_hz up to ~2^27 * reciprocal up to ~2^29 => product
    // ~2^56, safely under 2^64) while carrying far more precision than
    // the 32-bit FTW itself can resolve - negligible rounding error
    // relative to computing the division directly every call.
    uint32_t effective_ref_hz_init = cfg->ref_clk_hz * (cfg->use_6x_multiplier ? 6u : 1u);
    if (effective_ref_hz_init == 0) {
        free(h);
        return ESP_ERR_INVALID_ARG;
    }
    h->ftw_reciprocal = (1ULL << AD9851_FTW_RECIP_SHIFT) / effective_ref_hz_init;

    gpio_config_t reset_cfg = {
        .pin_bit_mask = 1ULL << cfg->pin_reset,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&reset_cfg);
    if (err != ESP_OK) {
        free(h);
        return err;
    }
#if AD9851_INVERTING_LEVEL_SHIFT
    gpio_set_level(cfg->pin_reset, 1);   // GPIO high -> AD9851 side pulled LOW (inactive) - idle state
#else
    gpio_set_level(cfg->pin_reset, 0);
#endif

    spi_bus_config_t buscfg = {
        .mosi_io_num = cfg->pin_data,
        .sclk_io_num = cfg->pin_wclk,
        .miso_io_num = -1,     // AD9851 serial interface is write-only
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 8,  // never more than 5 bytes (40 bits) per transfer
    };
    err = spi_bus_initialize(cfg->spi_host, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        free(h);
        return err;
    }

    // FQ_UD wired as CS. Both W_CLK and FQ_UD pass through inverting
    // BS170 stages (see AD9851_INVERTING_LEVEL_SHIFT), so what the SPI
    // peripheral drives is the logical opposite of what the AD9851
    // actually sees - compensated two ways here:
    //   - mode=2 (CPOL=1,CPHA=0): ESP32-side clock idles HIGH and
    //     captures on its own falling edge. After inversion, the AD9851
    //     sees idle LOW with data valid before ITS rising edge - exactly
    //     what it needs (mode 0 behavior, achieved via mode 2 on our side).
    //   - SPI_DEVICE_POSITIVE_CS: ESP32-side CS goes HIGH during the
    //     transfer, LOW after. After inversion, AD9851 sees FQ_UD LOW
    //     while shifting (correct) and a LOW-to-HIGH transition when the
    //     transfer ends (the required latch pulse).
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = cfg->spi_clock_hz,
#if AD9851_INVERTING_LEVEL_SHIFT
        .mode = 2,
        .flags = SPI_DEVICE_POSITIVE_CS,
#else
        .mode = 0,   // CPOL=0, CPHA=0 - AD9851's W_CLK idles low, data valid before rising edge
#endif
        .spics_io_num = cfg->pin_fqud,
        .queue_size = 1,
    };
    err = spi_bus_add_device(cfg->spi_host, &devcfg, &h->spi);
    if (err != ESP_OK) {
        spi_bus_free(cfg->spi_host);
        free(h);
        return err;
    }

    // Master reset. ESP32 GPIO toggling is far faster than the AD9851's
    // minimum reset pulse width needs, so 1us here is ample margin, not
    // a tight requirement.
#if AD9851_INVERTING_LEVEL_SHIFT
    gpio_set_level(cfg->pin_reset, 0);   // GPIO low -> AD9851 side HIGH (reset asserted)
    esp_rom_delay_us(1);
    gpio_set_level(cfg->pin_reset, 1);   // GPIO high -> AD9851 side LOW (released, back to idle)
    esp_rom_delay_us(1);
#else
    gpio_set_level(cfg->pin_reset, 1);
    esp_rom_delay_us(1);
    gpio_set_level(cfg->pin_reset, 0);
    esp_rom_delay_us(1);
#endif

    // Enter serial mode: one W_CLK pulse with DATA=0, then one FQ_UD
    // pulse - the standard AD9851/9850 "select serial mode" sequence,
    // ported exactly from the proven Nano library's reset() (pulse(WCLK)
    // then pulse(FQ_UD), no explicit data bit set beforehand). Done as a
    // 1-bit SPI transfer so the peripheral itself generates the single
    // W_CLK edge and the FQ_UD(CS) pulse - no need to temporarily
    // reclaim pins the SPI bus now owns. DATA's actual value here isn't
    // meaningful content (unlike ad9851_set_frequency()'s buffer), so
    // it's left un-inverted even under AD9851_INVERTING_LEVEL_SHIFT -
    // the mode/CS fixes above are what make this sequence work, not the
    // data bit's level.
    spi_transaction_t mode_select = {
        .length = 1,
        .flags = SPI_TRANS_USE_TXDATA,
        .tx_data = {0, 0, 0, 0},
    };
    err = spi_device_polling_transmit(h->spi, &mode_select);
    if (err != ESP_OK) {
        spi_bus_remove_device(h->spi);
        spi_bus_free(cfg->spi_host);
        free(h);
        return err;
    }

    *out_handle = h;
    return ESP_OK;
    // Caller MUST call ad9851_set_frequency() next, immediately - see
    // the header comment. The 40-bit register is left holding the
    // mode-select pulse's residual value at this point, not a valid word.
}

void IRAM_ATTR ad9851_set_frequency(ad9851_handle_t handle, uint32_t freq_hz)
{
    if (!handle) return;

    // Multiply+shift, not freq_hz*2^32/effective_ref_hz directly - see
    // ftw_reciprocal's comment in the struct for why. Algebraically
    // identical result, no division in this hot-path call.
    uint32_t ftw = (uint32_t)(((uint64_t)freq_hz * handle->ftw_reciprocal)
                               >> (AD9851_FTW_RECIP_SHIFT - 32));

    uint8_t buf[5];
    buf[0] = (uint8_t)(ftw & 0xFF);           // FTW LSB first - matches the proven
    buf[1] = (uint8_t)((ftw >> 8) & 0xFF);    // Nano library's
    buf[2] = (uint8_t)((ftw >> 16) & 0xFF);   // for (b=0;b<4;b++, ftw>>=8)
    buf[3] = (uint8_t)((ftw >> 24) & 0xFF);   //   transfer(ftw & 0xFF)
    buf[4] = handle->use_6x_multiplier ? 0x01 : 0x00;  // control byte: bit0 = 6x
                                                         // multiplier enable, phase=0,
                                                         // not powered down (matches
                                                         // AD9851_ENABLE_MULTIPLIER)

    // Bit ORDER reversal (LSB-first, see reverse_bits8's comment) and bit
    // VALUE inversion (DATA passes through its own inverting BS170, see
    // AD9851_INVERTING_LEVEL_SHIFT) are independent operations - both
    // needed together, order between them doesn't matter.
    for (int i = 0; i < 5; i++) {
        buf[i] = reverse_bits8(buf[i]);
#if AD9851_INVERTING_LEVEL_SHIFT
        buf[i] = (uint8_t)~buf[i];
#endif
    }

    spi_transaction_t t = {
        .length = 40,   // bits
        .tx_buffer = buf,
    };
    // Polling (not queued) transmit - blocks until done, no FreeRTOS
    // queue/semaphore involved, lowest and most deterministic latency
    // for a call sitting in the real-time sample path. buf is a local
    // stack array, safe since this call is synchronous.
    spi_device_polling_transmit(handle->spi, &t);
}

void ad9851_deinit(ad9851_handle_t handle)
{
    if (!handle) return;
    spi_bus_remove_device(handle->spi);
    free(handle);
}