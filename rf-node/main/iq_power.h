/*
 * iq_power.h - bounded MODEM_DIAG Q4/I4 snapshots and their mean power.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

/* 4096 bytes = 4096 complex samples = 102.4 us at 40 MS/s. */
#define IQ_WINDOW_BYTES 4096u

typedef struct {
    float mean_power;       /* half-LSB^2 units, see rssi_scale.h */
    uint16_t clip_permille; /* samples with I or Q at a rail */
    uint8_t rssi;           /* 0..255 */
} iq_power_t;

esp_err_t iq_power_init(void);

/* Captures one finite window and measures it. Blocks for ~0.1 ms plus
 * driver overhead. Must not be called concurrently. */
esp_err_t iq_power_measure(iq_power_t *out);
