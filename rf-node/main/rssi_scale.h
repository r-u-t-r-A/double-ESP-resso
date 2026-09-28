/*
 * rssi_scale.h - Q4/I4 window power -> 0..255 PhobosLT-style RSSI.
 *
 * Each MODEM_DIAG byte is one complex sample: bits 3:0 = Q[9:6], bits 7:4 =
 * I[9:6], both two's-complement -8..+7. A 4-bit code is a quantizer bucket,
 * so power uses the bucket centre (s + 0.5). In half-LSB units that is
 * (2s + 1)^2 per axis: minimum 1 + 1 = 2 (dead noise), maximum 225 + 225 = 450
 * (both axes at the rail).
 *
 * The mean window power is mapped logarithmically so the ~23.5 dB span of a
 * single fixed gain fills 0..255 (~10.8 counts per dB).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#define RSSI_POWER_MIN 2.0f
#define RSSI_POWER_MAX 450.0f

static inline uint16_t rssi_sample_power(uint8_t byte)
{
    int q = (int8_t)(uint8_t)(byte << 4) >> 4;
    int i = (int8_t)byte >> 4;
    int q2 = 2 * q + 1;
    int i2 = 2 * i + 1;
    return (uint16_t)(q2 * q2 + i2 * i2);
}

static inline int rssi_sample_clipped(uint8_t byte)
{
    int q = (int8_t)(uint8_t)(byte << 4) >> 4;
    int i = (int8_t)byte >> 4;
    return q == -8 || q == 7 || i == -8 || i == 7;
}

/* Mean power in half-LSB^2 units -> 0..255. */
static inline uint8_t rssi_from_mean_power(float mean_power)
{
    if (!(mean_power > RSSI_POWER_MIN)) return 0;
    float span_db = 10.0f * log10f(RSSI_POWER_MAX / RSSI_POWER_MIN);
    float db = 10.0f * log10f(mean_power / RSSI_POWER_MIN);
    float v = db * (255.0f / span_db) + 0.5f;
    if (v >= 255.0f) return 255;
    return (uint8_t)v;
}
