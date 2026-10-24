/*
 * rf_frontend.h - ESP32-C5 receive-only 5.8 GHz frontend for the RF node.
 *
 * Tunes the PHY's PLL directly, LO = pilot - 10 MHz, and forces a fixed RX
 * gain. Follows FPVGate C5MK's published description; see rf_frontend.c.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Pilot frequencies accepted: R1-R8 (5658-5917) are the tested set. */
#define RF_MIN_MHZ 5180u
#define RF_MAX_MHZ 5917u

/* NVS must already be initialized (Wi-Fi requires it). Brings up Wi-Fi with
 * no interface, calibrates the 5 GHz band, checks that captures are live,
 * then tunes to mhz with a fixed gain index. */
esp_err_t rf_frontend_start(uint16_t mhz, uint8_t gain_idx);

/* ESP_ERR_NOT_SUPPORTED when mhz is outside RF_MIN_MHZ..RF_MAX_MHZ. */
esp_err_t rf_frontend_tune(uint16_t mhz);
bool rf_frontend_can_tune(uint16_t mhz);

/* Clamped to the chip's highest calibrated index. */
void rf_frontend_set_gain(uint8_t gain_idx);
/* Re-forces the gain only if the PHY has dropped it. Returns true if it had. */
bool rf_frontend_hold_gain(void);

uint16_t rf_frontend_mhz(void);
uint8_t rf_frontend_gain(void);
uint8_t rf_frontend_gain_max(void);

/* Boot check outcome: "live", "live_rx_on", "flat" or "not_run". */
const char *rf_frontend_check(void);
