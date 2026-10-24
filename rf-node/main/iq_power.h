/*
 * iq_power.h - one pilot's band power per raw I/Q capture, filtered.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

/* 25.6 us of RF at 80 MS/s per reading. */
#define IQ_SAMPLES 2048u

typedef struct {
    float band_db;          /* this capture, dB re 1 LSB^2 */
    float filt_db;          /* median of 3, then EMA */
    uint16_t clip_permille; /* samples with I or Q at a rail */
    uint16_t gain_bad;      /* samples whose gain field != the forced index */
    uint8_t rssi;           /* 0..255 from filt_db */
} iq_power_t;

esp_err_t iq_power_init(void);

/* Restarts the filter: call after a retune or gain change. */
void iq_power_reset(void);

/* One capture and measurement (~0.25 ms of CPU). Must not be called
 * concurrently with itself, iq_power_probe() or a retune. */
esp_err_t iq_power_measure(iq_power_t *out);

/* One capture measured in the pilot band and in its mirror band, to check
 * the spectrum orientation: with a VTX on, pilot_db should be well above
 * mirror_db. Does not touch the filter. */
esp_err_t iq_power_probe(float *pilot_db, float *mirror_db);
