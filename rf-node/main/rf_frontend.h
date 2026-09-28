/*
 * rf_frontend.h - ESP32-C5 receive-only 5.8 GHz frontend for the RF node.
 *
 * Derived from C5VRX main/rf.c (https://github.com/Twotoz/C5VRX, GPL-3.0),
 * trimmed to what a power meter needs: bring-up, MODEM_DIAG Q4/I4 routing,
 * AGC off, fixed gain and tuning. No video, no gain controllers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define RF_MIN_MHZ 5180u
#define RF_MAX_MHZ 5885u

/* NVS must already be initialized (Wi-Fi requires it).
 * Brings up Wi-Fi/PHY receive-only, routes IQ to the PARLIO RX pins and
 * tunes to mhz with a fixed gain index. */
esp_err_t rf_frontend_start(uint16_t mhz, uint8_t gain_idx);

/* ESP_ERR_NOT_SUPPORTED when mhz is outside RF_MIN_MHZ..RF_MAX_MHZ. */
esp_err_t rf_frontend_tune(uint16_t mhz);
bool rf_frontend_can_tune(uint16_t mhz);

void rf_frontend_set_gain(uint8_t gain_idx);
uint16_t rf_frontend_mhz(void);
uint8_t rf_frontend_gain(void);
