/*
 * iq_power.c - one pilot's band power per raw I/Q capture, filtered.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "iq_power.h"

#include "band_power.h"
#include "iq_dump.h"
#include "rf_frontend.h"

_Static_assert(IQ_SAMPLES <= BP_MAX_SAMPLES && IQ_SAMPLES <= IQ_DUMP_MAX_SAMPLES &&
                   IQ_SAMPLES % BP_BLOCK == 0,
               "IQ_SAMPLES must fit the meter and the dump engine");

static bp_coeffs_t s_pilot;
static bp_coeffs_t s_mirror;
static bp_filter_t s_filter;

esp_err_t iq_power_init(void)
{
    bp_coeffs_init(&s_pilot, +1);
    bp_coeffs_init(&s_mirror, -1);
    bp_filter_reset(&s_filter);
    return ESP_OK;
}

void iq_power_reset(void)
{
    bp_filter_reset(&s_filter);
}

esp_err_t iq_power_measure(iq_power_t *out)
{
    const uint32_t *samples;
    esp_err_t err = iq_dump_capture(IQ_SAMPLES, &samples);
    if (err != ESP_OK) return err;

    bp_result_t r;
    bp_measure(samples, IQ_SAMPLES, &s_pilot, rf_frontend_gain(), &r);
    out->band_db = r.db;
    out->filt_db = bp_filter_push(&s_filter, r.db);
    out->clip_permille = (uint16_t)(r.clipped * 1000u / r.used);
    out->gain_bad = (uint16_t)(r.gain_mismatch > UINT16_MAX ? UINT16_MAX : r.gain_mismatch);
    out->rssi = bp_rssi_from_db(out->filt_db);
    return ESP_OK;
}

esp_err_t iq_power_probe(float *pilot_db, float *mirror_db)
{
    const uint32_t *samples;
    esp_err_t err = iq_dump_capture(IQ_SAMPLES, &samples);
    if (err != ESP_OK) return err;
    bp_result_t r;
    bp_measure(samples, IQ_SAMPLES, &s_pilot, rf_frontend_gain(), &r);
    *pilot_db = r.db;
    bp_measure(samples, IQ_SAMPLES, &s_mirror, rf_frontend_gain(), &r);
    *mirror_db = r.db;
    return ESP_OK;
}
