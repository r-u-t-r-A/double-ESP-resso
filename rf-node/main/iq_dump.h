/*
 * iq_dump.h - raw 10-bit I/Q captures from the ESP32-C5 PHY's ADC dump engine.
 *
 * The engine writes N consecutive 80 MS/s sample words (band_power.h has the
 * layout) into SRAM from IQ_DUMP_BASE. The first capture goes through the
 * vendor's adctrig(), which programs the engine (~1.7 ms). Later captures
 * drive the same register sequence directly (~55 us for 2048 samples).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#define IQ_DUMP_MAX_SAMPLES 2048u

typedef struct {
    uint32_t stock;        /* adctrig() captures */
    uint32_t direct;       /* register-driven captures */
    uint32_t timeouts;     /* direct captures that never finished */
    uint32_t short_reject; /* last word never written */
    uint32_t long_reject;  /* engine wrote past the end */
} iq_dump_stats_t;

/* One capture through the vendor adctrig(): receive-only arguments, fixed. */
esp_err_t iq_dump_stock(uint32_t n, const uint32_t **samples);

/* One direct capture. Falls back to iq_dump_stock() until a stock capture
 * has programmed the engine, and after a timeout. */
esp_err_t iq_dump_capture(uint32_t n, const uint32_t **samples);

void iq_dump_stats(iq_dump_stats_t *out);
