/*
 * iq_power.h - bounded MODEM_DIAG Q4/I4 snapshots and their mean power.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

/* One completed GDMA descriptor: 4092 complex samples = 102.3 us at 40 MS/s. */
#define IQ_WINDOW_BYTES 4092u

typedef struct {
    float mean_power;       /* half-LSB^2 units, see rssi_scale.h */
    uint16_t clip_permille; /* samples with I or Q at a rail */
    uint8_t rssi;           /* 0..255 */
} iq_power_t;

esp_err_t iq_power_init(void);

/* Measures the most recently completed ring descriptor. Non-blocking
 * (~0.1 ms of CPU). Must not be called concurrently. */
esp_err_t iq_power_measure(iq_power_t *out);
