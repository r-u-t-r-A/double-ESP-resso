/*
 * band_power.h - narrow-band power of one pilot from a raw 10-bit I/Q capture.
 *
 * The method is FPVGate C5MK's (Louis Hitchcock, https://github.com/
 * LouisHitchcock/FPVGateC5MK, paper section 4.3 and docs/RADIO_INTERNALS.md
 * section 9), reimplemented here from that published description. No C5MK
 * code is used.
 *
 *  - The LO sits BP_LO_OFFSET_MHZ below the pilot.
 *  - The C5's baseband spectrum is inverted (BP_SPECTRUM_SIGN), so the pilot
 *    appears at -10 MHz in I + jQ and its empty mirror at +10 MHz.
 *  - Each block of 8 samples is Hann-windowed and mixed down by 10 MHz. At
 *    80 MS/s that is exactly one cycle per block, so every block uses the
 *    same 8 coefficients: one windowed DFT bin, about 10 MHz wide.
 *  - The capture mean (DC) is removed, and |block|^2 is averaged.
 *
 * Normalised so a complex tone of amplitude A at the pilot reads A^2 (LSB^2).
 * The mean is removed in a single pass: the DC term is linear, so the raw
 * block sums are accumulated first and corrected at the end.
 *
 * Plain C99 with no ESP-IDF, so tests/test_band_power.c runs it on the host.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BP_SAMPLE_RATE_MHZ 80
#define BP_LO_OFFSET_MHZ   10
#define BP_BLOCK           8 /* 80 MS/s / 10 MHz: one mixer cycle */
/* RF above the LO appears at negative baseband frequency. */
#define BP_SPECTRUM_SIGN   (-1)
#define BP_COEF_ONE        16384 /* Q14 */
#define BP_MAX_SAMPLES     2048u /* keeps the int64 sums clear of overflow */

/* Sample word: bits 0-9 signed I, bits 10-19 signed Q, bits 20-26 the gain
 * index in force for that sample. */
static inline int32_t bp_sample_i(uint32_t w) { return (int32_t)(w << 22) >> 22; }
static inline int32_t bp_sample_q(uint32_t w) { return (int32_t)(w << 12) >> 22; }
static inline uint8_t bp_sample_gain(uint32_t w) { return (uint8_t)((w >> 20) & 0x7Fu); }

static inline uint32_t bp_make_sample(int32_t i, int32_t q, uint8_t gain)
{
    return ((uint32_t)i & 0x3FFu) | (((uint32_t)q & 0x3FFu) << 10) |
           ((uint32_t)(gain & 0x7Fu) << 20);
}

typedef struct {
    int32_t re[BP_BLOCK];
    int32_t im[BP_BLOCK];
    double dc_re, dc_im; /* block response to a constant 1 + 0j */
    double norm;         /* block |response|^2 to a unit tone at the band */
} bp_coeffs_t;

/* Coefficients for the band whose centre is rf_side * BP_LO_OFFSET_MHZ from
 * the LO: rf_side = +1 is the pilot, -1 its mirror (for checks only). */
static inline void bp_coeffs_init(bp_coeffs_t *c, int rf_side)
{
    const double pi = 3.14159265358979323846;
    /* Baseband cycles per block of the band centre: +-1. */
    int cycles = rf_side * BP_SPECTRUM_SIGN;
    double sum_re = 0.0, sum_im = 0.0, tone_re = 0.0, tone_im = 0.0;
    for (int k = 0; k < BP_BLOCK; ++k) {
        double w = 0.5 - 0.5 * cos(2.0 * pi * k / BP_BLOCK); /* periodic Hann */
        double ph = -2.0 * pi * cycles * k / BP_BLOCK;       /* mix to 0 Hz */
        c->re[k] = (int32_t)lround(BP_COEF_ONE * w * cos(ph));
        c->im[k] = (int32_t)lround(BP_COEF_ONE * w * sin(ph));
        sum_re += c->re[k];
        sum_im += c->im[k];
        /* Response to exp(+j 2 pi cycles k / BLOCK): c_k * tone_k. */
        double t_re = cos(-ph), t_im = sin(-ph);
        tone_re += c->re[k] * t_re - c->im[k] * t_im;
        tone_im += c->re[k] * t_im + c->im[k] * t_re;
    }
    c->dc_re = sum_re;
    c->dc_im = sum_im;
    c->norm = tone_re * tone_re + tone_im * tone_im;
}

typedef struct {
    float power;            /* LSB^2 */
    float db;               /* 10 log10(power), BP_DB_FLOOR when empty */
    float dc_i, dc_q;       /* capture mean, LSB */
    uint32_t clipped;       /* samples with I or Q at a rail */
    uint32_t gain_mismatch; /* samples whose gain field != the expected index */
    uint32_t used;          /* samples measured (whole blocks) */
} bp_result_t;

#define BP_DB_FLOOR (-30.0f)

static inline int bp_is_clipped(int32_t v) { return v >= 511 || v <= -512; }

/* Band power of samples[0..n), n <= BP_MAX_SAMPLES (whole blocks only). */
static inline void bp_measure(const uint32_t *samples, size_t n, const bp_coeffs_t *c,
                              uint8_t gain, bp_result_t *out)
{
    if (n > BP_MAX_SAMPLES) n = BP_MAX_SAMPLES;
    size_t blocks = n / BP_BLOCK;
    int64_t sum_sq = 0, tot_re = 0, tot_im = 0;
    int32_t sum_i = 0, sum_q = 0;
    uint32_t clipped = 0, mismatch = 0;

    for (size_t b = 0; b < blocks; ++b) {
        const uint32_t *w = samples + b * BP_BLOCK;
        int32_t acc_re = 0, acc_im = 0;
        for (int k = 0; k < BP_BLOCK; ++k) {
            uint32_t s = w[k];
            int32_t i = bp_sample_i(s), q = bp_sample_q(s);
            sum_i += i;
            sum_q += q;
            clipped += (uint32_t)(bp_is_clipped(i) | bp_is_clipped(q));
            mismatch += (uint32_t)(bp_sample_gain(s) != gain);
            acc_re += i * c->re[k] - q * c->im[k];
            acc_im += i * c->im[k] + q * c->re[k];
        }
        tot_re += acc_re;
        tot_im += acc_im;
        sum_sq += (int64_t)acc_re * acc_re + (int64_t)acc_im * acc_im;
    }

    out->used = (uint32_t)(blocks * BP_BLOCK);
    out->clipped = clipped;
    out->gain_mismatch = mismatch;
    if (!blocks) {
        out->power = 0.0f;
        out->db = BP_DB_FLOOR;
        out->dc_i = out->dc_q = 0.0f;
        return;
    }
    double mi = (double)sum_i / out->used, mq = (double)sum_q / out->used;
    /* Each block's response to the mean: D = mean * dc. */
    double d_re = mi * c->dc_re - mq * c->dc_im;
    double d_im = mi * c->dc_im + mq * c->dc_re;
    /* sum |S_b - D|^2 = sum |S_b|^2 - 2 Re(conj(D) sum S_b) + B |D|^2 */
    double e = (double)sum_sq - 2.0 * (d_re * (double)tot_re + d_im * (double)tot_im) +
               (double)blocks * (d_re * d_re + d_im * d_im);
    double p = e / ((double)blocks * c->norm);
    if (p < 0.0) p = 0.0;
    out->power = (float)p;
    out->db = p > 1e-3 ? (float)(10.0 * log10(p)) : BP_DB_FLOOR;
    out->dc_i = (float)mi;
    out->dc_q = (float)mq;
}

/* Median of the last 3 readings (drops single-capture spikes such as 5 GHz
 * Wi-Fi bursts), then an EMA. */
#define BP_EMA_ALPHA 0.3f

typedef struct {
    float hist[3];
    uint8_t count;
    float ema;
} bp_filter_t;

static inline void bp_filter_reset(bp_filter_t *f)
{
    f->count = 0;
    f->ema = 0.0f;
}

static inline float bp_median3(float a, float b, float c)
{
    if (a > b) { float t = a; a = b; b = t; }
    if (b > c) b = c;
    return a > b ? a : b;
}

static inline float bp_filter_push(bp_filter_t *f, float db)
{
    f->hist[0] = f->hist[1];
    f->hist[1] = f->hist[2];
    f->hist[2] = db;
    if (f->count < 3) ++f->count;
    float m = f->count < 3 ? db : bp_median3(f->hist[0], f->hist[1], f->hist[2]);
    f->ema = f->count == 1 ? m : f->ema + BP_EMA_ALPHA * (m - f->ema);
    return f->ema;
}

/* dB -> 0..255 for PhobosLT. The floor is about -3 dB, a full-scale tone
 * (A = 511) reads 54 dB: 60 dB in 255 counts is 4.25 counts per dB. */
#define BP_RSSI_DB_MIN (-6.0f)
#define BP_RSSI_DB_MAX 54.0f

static inline uint8_t bp_rssi_from_db(float db)
{
    float v = (db - BP_RSSI_DB_MIN) * (255.0f / (BP_RSSI_DB_MAX - BP_RSSI_DB_MIN)) + 0.5f;
    if (!(v > 0.0f)) return 0;
    if (v >= 255.0f) return 255;
    return (uint8_t)v;
}
