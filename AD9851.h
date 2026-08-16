#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_attr.h"
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief ESP-IDF hardware-SPI driver for the AD9851 DDS, matching the
 *        protocol used by the proven Arduino Nano AD9851.h library this
 *        was ported from (LSB-first bit order, SPI Mode 0, 4 FTW bytes
 *        then a control byte, FQ_UD pulsed after each transfer) - NOT
 *        reconstructed from the datasheet tables directly, since those
 *        are genuinely ambiguous between serial/parallel byte ordering
 *        and getting this wrong would silently produce the wrong
 *        frequency with no obvious symptom short of a spectrum analyser.
 *
 * FQ_UD is wired as the SPI bus's CS line (spics_io_num) rather than a
 * separate manually-toggled GPIO: the ESP-IDF SPI driver asserts CS LOW
 * for the duration of a transaction and returns it HIGH immediately
 * after - which is exactly the FQ_UD timing the AD9851 wants (low while
 * shifting, low-to-high transition to latch). No separate FQ_UD pulse
 * code needed.
 *
 * TIMING: ad9851_set_frequency() is intended to be called every audio
 * sample (e.g. from dsp_task at 20kHz) for continuous phase modulation.
 * A 40-bit transfer takes 40/spi_clock_hz seconds of hard SPI clock time
 * alone - e.g. 20us at 2MHz. Check this against your real-time budget
 * (e.g. via the existing [timing] instrumentation) once this is wired
 * up; don't assume it fits just because the rest of the pipeline had
 * margin before this was added.
 *
 * ad9851_init() acquires the SPI bus once (spi_device_acquire_bus(),
 * never released until ad9851_deinit()) rather than letting every
 * ad9851_set_frequency() call take/release it internally - this is a
 * dedicated single-device bus, called up to 20000x/sec from the
 * real-time path, so the per-call acquire/release lock overhead
 * spi_device_polling_transmit() would otherwise pay every time is pure
 * waste here. Measured real-world write_us has been running well above
 * the raw 40/spi_clock_hz bit-time estimate (e.g. ~59us observed at
 * 2MHz vs. ~20us theoretical) - see ad9851_profile_t below for splitting
 * out how much of that gap is CPU-side prep vs. the SPI transfer itself,
 * to find out how much this actually recovers.
 */

typedef struct {
    spi_host_device_t spi_host;   ///< e.g. SPI2_HOST
    int pin_data;                 ///< D7 - wired as SPI MOSI
    int pin_wclk;                 ///< W_CLK - wired as SPI SCLK
    int pin_fqud;                 ///< FQ_UD - wired as SPI CS (see above, not a plain GPIO)
    int pin_reset;                ///< RESET - plain GPIO, toggled once at init
    uint32_t ref_clk_hz;          ///< Crystal/reference frequency, e.g. 30000000
    bool use_6x_multiplier;       ///< Enable the internal 6x REFCLK multiplier
    int spi_clock_hz;             ///< SPI clock rate - see TIMING note above
} ad9851_config_t;

typedef struct ad9851_s *ad9851_handle_t;

/**
 * @brief Initialize the AD9851: sets up the SPI bus/device, pulses
 *        RESET, and performs the standard "enter serial mode" sequence
 *        (one W_CLK pulse + one FQ_UD pulse with DATA=0).
 *
 *        IMPORTANT: per the AD9851 datasheet, the 40-bit register must
 *        be immediately overwritten with a valid word after entering
 *        serial mode, or it may randomly engage the 6x multiplier or
 *        factory test mode. Call ad9851_set_frequency() immediately
 *        after this returns ESP_OK - don't skip it or reorder it.
 */
esp_err_t ad9851_init(const ad9851_config_t *cfg, ad9851_handle_t *out_handle);

/**
 * @brief Set the output frequency. Computes the 32-bit frequency tuning
 *        word and sends the full 40-bit serial word (FTW + control
 *        byte). Blocking (spi_device_polling_transmit - no queue/
 *        semaphore involved, lowest latency for the real-time path) -
 *        see the TIMING note above for how long this actually takes.
 */
void IRAM_ATTR ad9851_set_frequency(ad9851_handle_t handle, uint32_t freq_hz);

/**
 * @brief Sub-phase timing breakdown of ad9851_set_frequency(), each a
 *        running high-water mark in microseconds since ad9851_init() -
 *        same pattern as ssb_dsp_profile_t (ssb_dsp.h). max_prep_us
 *        covers the FTW multiply-shift plus the 5-byte bit-reversal
 *        loop; max_spi_us covers just spi_device_polling_transmit()
 *        itself. Splits what the [timing] line's write_us figure lumps
 *        together as one number, so a slow write can be attributed to
 *        genuine SPI clock-out time vs. CPU-side prep vs. (by
 *        subtraction against write_us) whatever driver-call overhead
 *        remains, instead of guessing which lever to pull.
 */
typedef struct {
    uint32_t max_prep_us;
    uint32_t max_spi_us;
} ad9851_profile_t;

void ad9851_get_profile(ad9851_handle_t handle, ad9851_profile_t *out);

/**
 * @brief Enable/disable RF output via the AD9851's own power-down
 *        control bit (same one the Nano library uses - AD9851_POWER_DOWN,
 *        0x04) rather than stopping SPI writes or toggling RESET: this
 *        is a clean, proper power-down, not just "stop updating and
 *        leave the last frequency running." Takes effect on the next
 *        ad9851_set_frequency() call - no separate transfer needed, no
 *        extra latency added to the real-time path. Defaults to enabled
 *        after ad9851_init(). IRAM_ATTR: safe to call from the real-time
 *        path, though intended for occasional calls (e.g. a serial
 *        command), not per-sample.
 */
void IRAM_ATTR ad9851_set_output_enabled(ad9851_handle_t handle, bool enable);
bool ad9851_get_output_enabled(ad9851_handle_t handle);

/**
 * @brief Free the SPI device and handle. Does not touch RESET, so the
 *        AD9851 keeps outputting its last-programmed frequency.
 */
void ad9851_deinit(ad9851_handle_t handle);

#ifdef __cplusplus
}
#endif