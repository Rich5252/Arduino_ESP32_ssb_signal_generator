#include <stdlib.h>
#include "ad9851.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"   // REG_WRITE()

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

// Real-hardware measurement (2026-08-16 session) found the hardware-SPI
// path's write_us was dominated by ESP-IDF's spi_master per-call overhead,
// not the 40-bit clock-out time itself: a clean post-boot-transient read
// at 4MHz SPI clock showed write_us=50, against a theoretical 40-bit/4MHz
// = 10us clock time + ~2us of prep - i.e. ~38us (roughly 3/4 of the total)
// was transaction-descriptor/bus-handling overhead in the driver, mostly
// independent of the SPI clock rate (doubling the clock 2MHz->4MHz only
// dropped write_us 55-59 -> 50, nowhere near proportional). Since the
// AD9851's serial protocol is just three GPIOs toggled with simple
// setup/hold timing (7ns-scale minimums per the datasheet - trivially
// satisfied even by unoptimized GPIO calls), bypassing the SPI peripheral
// entirely and bit-banging DATA/W_CLK/FQ_UD directly removes that
// per-call driver overhead. Set to 0 to fall back to the proven
// hardware-SPI path (kept fully intact below) if the bit-bang path ever
// needs to be ruled out as a cause of a problem.
//
// IMPORTANT after flipping this: re-verify the transmitted frequency on
// the spectrum analyzer / a calibrated receiver before trusting any other
// measurement - per AD9851.h's own long-standing warning, a bit-order or
// inversion mistake here produces the wrong frequency with no obvious
// symptom short of a spectrum analyser. Also worth re-scoping the DATA/
// W_CLK/FQ_UD lines once this is running.
//
// FIRST CUT of this used gpio_set_level() for every toggle and measured
// write_us=52 clean - essentially a WASH against the old SPI path's
// write_us=50, not the win expected. Root cause, found via the prep_us/
// spi_us split: spi_us (the toggle loop itself) measured 41us for what
// should be a ~10us job (40 bits x 3 gpio_set_level() calls each) -
// gpio_set_level() isn't a bare register write, it's a function call plus
// a critical-section/spinlock enter+exit each time (~330ns/call measured
// here), so trading one per-transaction SPI-driver overhead for 122
// smaller per-call overheads just moved the cost, it didn't remove it.
// Fixed below by using fast_gpio_set()/fast_gpio_clr() (direct
// GPIO_OUT_W1TS_REG/W1TC_REG register writes, no function-call or
// critical-section overhead) for every toggle in the hot path instead -
// only valid for pins 0-31 (this project's DATA/W_CLK/FQ_UD pins are
// 9-12, so that's fine; a board using a pin >=32 would need the
// GPIO_OUT1_W1TS_REG/W1TC_REG pair instead).
#define AD9851_USE_BITBANG 1

struct ad9851_s {
#if AD9851_USE_BITBANG
    int pin_data;                  // DATA/D7 - bit-banged directly, no SPI peripheral involved
    int pin_wclk;                  // W_CLK   - bit-banged directly
    int pin_fqud;                  // FQ_UD   - bit-banged directly (was the SPI CS line)
#else
    spi_device_handle_t spi;
#endif
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

    // Written from a different context (e.g. a serial command handler)
    // than the real-time path that reads it in ad9851_set_frequency() -
    // volatile for the same reason as the other cross-context flags
    // elsewhere in this project.
    volatile bool power_down;

    // Sub-phase timing high-water marks - see ad9851_profile_t (AD9851.h)
    // for what each covers. Plain (non-volatile) uint32_t, same
    // convention ssb_dsp.c already uses for its own profile fields:
    // written only from the real-time path, read cross-context by
    // ad9851_get_profile() via diagnostics_service() - 32-bit-aligned
    // reads/writes are atomic on this target and these are monotonic
    // high-water marks, so a torn read isn't a real risk.
    //
    // max_spi_us keeps its name for continuity/direct comparison against
    // earlier hardware-SPI readings, even though under AD9851_USE_BITBANG
    // it covers the bit-bang toggle loop instead of an actual SPI
    // transfer - see ad9851_set_frequency().
    uint32_t max_prep_us;
    uint32_t max_spi_us;
};

#define AD9851_CTRL_ENABLE_MULTIPLIER 0x01   // matches the Nano library's AD9851_ENABLE_MULTIPLIER
#define AD9851_CTRL_POWER_DOWN        0x04   // matches the Nano library's AD9851_POWER_DOWN

#define AD9851_FTW_RECIP_SHIFT 56   // generous headroom - see ad9851_init() for the
                                     // precision reasoning; not chosen to be minimal

#if AD9851_USE_BITBANG
// Direct GPIO_OUT_W1TS_REG/W1TC_REG ("write 1 to set"/"write 1 to clear")
// register writes - a single MMIO store each, no function-call overhead
// and no critical section, unlike gpio_set_level() (see the measurement
// note by AD9851_USE_BITBANG above for why that matters here). Only
// covers pins 0-31, which is all this driver needs.
static inline void IRAM_ATTR fast_gpio_set(int pin)
{
    REG_WRITE(GPIO_OUT_W1TS_REG, 1UL << pin);
}
static inline void IRAM_ATTR fast_gpio_clr(int pin)
{
    REG_WRITE(GPIO_OUT_W1TC_REG, 1UL << pin);
}
#else
// Hardware SPI sends MSB-first by default; the AD9851 wants LSB-first
// within each byte (confirmed via the proven Nano library's
// SPISettings(..., LSBFIRST, ...)) - reversed in software here rather
// than depending on a specific ESP-IDF LSB-first flag name, to avoid
// getting a memorized macro name wrong on hardware this matters for.
//
// Only needed by the hardware-SPI path: the bit-bang path below sends
// each byte's bits directly starting from bit0, which is already
// LSB-first at the wire, so it doesn't need this reversal step at all.
static inline uint8_t IRAM_ATTR reverse_bits8(uint8_t b)
{
    b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
    b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
    b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
    return b;
}
#endif

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
    h->power_down = false;

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

#if AD9851_USE_BITBANG
    h->pin_data = cfg->pin_data;
    h->pin_wclk = cfg->pin_wclk;
    h->pin_fqud = cfg->pin_fqud;

    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << cfg->pin_data) | (1ULL << cfg->pin_wclk) | (1ULL << cfg->pin_fqud),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&io_cfg);
    if (err != ESP_OK) {
        free(h);
        return err;
    }

    // Idle levels, chosen to reproduce exactly what the proven SPI-mode-2
    // + SPI_DEVICE_POSITIVE_CS setup already did electrically (see the
    // non-bitbang branch below for the derivation this mirrors):
    //   - W_CLK idle GPIO HIGH -> AD9851 side idle LOW (mode-0-equivalent
    //     idle state on the AD9851's own pin).
    //   - FQ_UD idle GPIO LOW -> AD9851 side idle HIGH (matches "AD9851
    //     sees FQ_UD LOW only while shifting, LOW-to-HIGH latch pulse
    //     when a transfer ends" - so between transfers it sits HIGH).
    //   - DATA's idle level is irrelevant - always overwritten before
    //     every clock pulse.
    gpio_set_level(cfg->pin_wclk, 1);
    gpio_set_level(cfg->pin_fqud, 0);
    gpio_set_level(cfg->pin_data, 0);
#else
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

    // Acquire the SPI bus once, here, and never release it until
    // ad9851_deinit() - this is a dedicated single-device bus (the
    // AD9851's serial interface is the only thing on it), and
    // ad9851_set_frequency() calls spi_device_polling_transmit() up to
    // 20000x/sec from the real-time path. Per ESP-IDF's SPI master docs,
    // a transmit call detects when the calling task already holds the
    // bus (via spi_device_acquire_bus()) and skips its own internal
    // acquire/release each time - pure overhead otherwise, on every
    // single sample, for a lock no other device is ever contending for.
    err = spi_device_acquire_bus(h->spi, portMAX_DELAY);
    if (err != ESP_OK) {
        spi_bus_remove_device(h->spi);
        spi_bus_free(cfg->spi_host);
        free(h);
        return err;
    }
#endif

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
    // then pulse(FQ_UD), no explicit data bit set beforehand).
#if AD9851_USE_BITBANG
    // DATA is already 0 from the idle-level setup above, and (per the
    // non-bitbang branch's comment) its value here isn't meaningful
    // content, so it's left un-inverted, matching that branch exactly.
    // One W_CLK pulse (idle HIGH -> LOW -> HIGH, i.e. one AD9851-side
    // rising-then-falling edge), then one FQ_UD pulse (idle LOW -> HIGH
    // -> LOW on the ESP32 side, i.e. AD9851-side HIGH -> LOW -> HIGH,
    // ending on the LOW-to-HIGH latch transition, back at FQ_UD's normal
    // idle level).
    fast_gpio_clr(h->pin_wclk);
    fast_gpio_set(h->pin_wclk);
    fast_gpio_set(h->pin_fqud);
    fast_gpio_clr(h->pin_fqud);
#else
    // Done as a 1-bit SPI transfer so the peripheral itself generates the
    // single W_CLK edge and the FQ_UD(CS) pulse - no need to temporarily
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
        spi_device_release_bus(h->spi);
        spi_bus_remove_device(h->spi);
        spi_bus_free(cfg->spi_host);
        free(h);
        return err;
    }
#endif

    *out_handle = h;
    return ESP_OK;
    // Caller MUST call ad9851_set_frequency() next, immediately - see
    // the header comment. The 40-bit register is left holding the
    // mode-select pulse's residual value at this point, not a valid word.
}

void IRAM_ATTR ad9851_set_frequency(ad9851_handle_t handle, uint32_t freq_hz)
{
    if (!handle) return;

    int64_t t0 = esp_timer_get_time();

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
    buf[4] = handle->power_down
             ? AD9851_CTRL_POWER_DOWN
             : (handle->use_6x_multiplier ? AD9851_CTRL_ENABLE_MULTIPLIER : 0x00);
    // control byte: power-down bit takes priority and is sent alone
    // (matches the Nano library's exact behavior - AD9851_POWER_DOWN is
    // sent by itself, not OR'd with the multiplier bit); otherwise bit0 =
    // 6x multiplier enable, phase=0, not powered down.

#if !AD9851_USE_BITBANG
    // Bit ORDER reversal (LSB-first, see reverse_bits8's comment) and bit
    // VALUE inversion (DATA passes through its own inverting BS170, see
    // AD9851_INVERTING_LEVEL_SHIFT) are independent operations - both
    // needed together, order between them doesn't matter. Only needed
    // for the hardware-SPI path; the bit-bang path below applies the
    // inversion per-bit at GPIO-set time instead, and doesn't need the
    // order reversed since it already walks each byte from bit0 up.
    for (int i = 0; i < 5; i++) {
        buf[i] = reverse_bits8(buf[i]);
#if AD9851_INVERTING_LEVEL_SHIFT
        buf[i] = (uint8_t)~buf[i];
#endif
    }
#endif

    int64_t t1 = esp_timer_get_time();
    uint32_t prep_us = (uint32_t)(t1 - t0);
    if (prep_us > handle->max_prep_us) handle->max_prep_us = prep_us;

#if AD9851_USE_BITBANG
    // FQ_UD: begin shift - ESP32 GPIO HIGH -> AD9851 side LOW, the
    // "shifting in progress" state (see the idle-level comment in
    // ad9851_init()).
    fast_gpio_set(handle->pin_fqud);

    for (int i = 0; i < 5; i++) {
        uint8_t byte = buf[i];
        for (int bit = 0; bit < 8; bit++) {
            uint8_t b = (uint8_t)((byte >> bit) & 1);   // bit0 first = LSB-first at the wire
#if AD9851_INVERTING_LEVEL_SHIFT
            if (b) fast_gpio_clr(handle->pin_data); else fast_gpio_set(handle->pin_data);
#else
            if (b) fast_gpio_set(handle->pin_data); else fast_gpio_clr(handle->pin_data);
#endif
            // Clock pulse: ESP32 W_CLK HIGH->LOW->HIGH. After the
            // inverting level shift this is AD9851-side LOW->HIGH->LOW -
            // DATA is set up above before this transition, so it's valid
            // before the AD9851's own rising edge, same relationship the
            // SPI path's mode-2-compensating-for-mode-0 setup achieves.
            fast_gpio_clr(handle->pin_wclk);
            fast_gpio_set(handle->pin_wclk);
        }
    }

    // End shift: ESP32 GPIO LOW -> AD9851 side HIGH, the required
    // LOW-to-HIGH latch transition, back at FQ_UD's normal idle level.
    fast_gpio_clr(handle->pin_fqud);
#else
    spi_transaction_t t = {
        .length = 40,   // bits
        .tx_buffer = buf,
    };
    // Polling (not queued) transmit - blocks until done, no FreeRTOS
    // queue/semaphore involved, lowest and most deterministic latency
    // for a call sitting in the real-time sample path. buf is a local
    // stack array, safe since this call is synchronous. The bus is
    // already held (see ad9851_init()'s spi_device_acquire_bus() call),
    // so this call skips its own internal acquire/release.
    spi_device_polling_transmit(handle->spi, &t);
#endif

    int64_t t2 = esp_timer_get_time();
    uint32_t spi_us = (uint32_t)(t2 - t1);
    if (spi_us > handle->max_spi_us) handle->max_spi_us = spi_us;
}

void ad9851_get_profile(ad9851_handle_t handle, ad9851_profile_t *out)
{
    if (!handle || !out) return;
    out->max_prep_us = handle->max_prep_us;
    out->max_spi_us = handle->max_spi_us;
}

void ad9851_deinit(ad9851_handle_t handle)
{
    if (!handle) return;
#if !AD9851_USE_BITBANG
    spi_device_release_bus(handle->spi);   // matches ad9851_init()'s acquire
    spi_bus_remove_device(handle->spi);
#endif
    free(handle);
}

void IRAM_ATTR ad9851_set_output_enabled(ad9851_handle_t handle, bool enable)
{
    if (!handle) return;
    handle->power_down = !enable;
    // Takes effect on the NEXT ad9851_set_frequency() call - no transfer
    // sent from here, keeping this call cheap regardless of context.
    // dsp_task already calls ad9851_set_frequency() every sample, so the
    // change reaches the chip within one sample period either way.
}

bool ad9851_get_output_enabled(ad9851_handle_t handle)
{
    return handle && !handle->power_down;
}