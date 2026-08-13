#pragma once

/**
 * carrier_output.h
 *
 * AD9851 DDS carrier: init, per-tick frequency update (carrier + delayed
 * frequency deviation), and RF output on/off. Only compiled in when
 * AD9851_ATTACHED (see config.h) - mirrors the original .ino's guarding
 * exactly, so this still builds standalone (mic->DSP->DAC, no AD9851
 * board yet) with AD9851_ATTACHED set to 0.
 */

#include "config.h"

#if AD9851_ATTACHED

#include <stdint.h>
#include <stdbool.h>
#include "ad9851.h"

#define AD9851_PIN_DATA   10
#define AD9851_PIN_WCLK   12
#define AD9851_PIN_FQUD   11
#define AD9851_PIN_RESET  9
#define REF_CLK_HZ        30000000u
#define CARRIER_HZ        14200160u  // +160Hz calibration offset - this AD9851 module's actual
                                      // REF_CLK isn't precisely 30MHz (expected given it's an
                                      // uncalibrated XO, not a precision reference); this value
                                      // makes the real transmitted output land on 14200000 exactly,
                                      // confirmed against the user's calibrated receiver

// Inits the AD9851 SPI driver and sets the initial carrier frequency
// (CARRIER_HZ). Call once from setup().
void carrier_output_init(void);

// tx_freq = carrier + (int32_t)delayed_freq_dev_hz, sent to the chip via
// ad9851_set_frequency(). Returns the exact integer Hz value sent, so the
// caller can feed it straight to diagnostics_set_tx_info() - ground truth
// for what the chip is actually asked to produce, independent of any of
// the upstream DSP/delay-line reasoning. Call once per dsp_task tick.
uint32_t IRAM_ATTR carrier_output_set_freq_dev(float delayed_freq_dev_hz);

bool carrier_output_get_rf_enabled(void);
void carrier_output_set_rf_enabled(bool enable);

#endif // AD9851_ATTACHED
