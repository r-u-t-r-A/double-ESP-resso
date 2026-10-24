/* Host test for rf-node/main/band_power.h.
 * Build: cc -std=c99 -Wall -Wextra -Werror -I rf-node/main tests/test_band_power.c -lm */
#include "band_power.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define N 2048
static const double PI = 3.14159265358979323846;

static uint32_t buf[N];

/* Fills buf with a complex tone at rf_offset_mhz from the LO (as seen after
 * the C5's spectrum inversion), plus DC and optional noise, quantised to the
 * 10-bit sample word. */
static void synth(double amp, double rf_offset_mhz, double dc_i, double dc_q,
                  double noise, unsigned seed, uint8_t gain)
{
    srand(seed);
    double f = BP_SPECTRUM_SIGN * rf_offset_mhz / BP_SAMPLE_RATE_MHZ;
    for (int n = 0; n < N; ++n) {
        double i = dc_i + amp * cos(2.0 * PI * f * n + 0.3);
        double q = dc_q + amp * sin(2.0 * PI * f * n + 0.3);
        if (noise > 0.0) {
            i += noise * ((double)rand() / RAND_MAX - 0.5) * 3.4641; /* var = noise^2 */
            q += noise * ((double)rand() / RAND_MAX - 0.5) * 3.4641;
        }
        long ii = lround(i), qq = lround(q);
        if (ii > 511) ii = 511;
        if (ii < -512) ii = -512;
        if (qq > 511) qq = 511;
        if (qq < -512) qq = -512;
        buf[n] = bp_make_sample((int32_t)ii, (int32_t)qq, gain);
    }
}

/* Two-pass double-precision reference: subtract the mean, then window, mix
 * and average |block|^2, normalised by the response to a unit tone. */
static double reference_power(const uint32_t *s, size_t n, int rf_side)
{
    double mi = 0, mq = 0;
    for (size_t k = 0; k < n; ++k) {
        mi += bp_sample_i(s[k]);
        mq += bp_sample_q(s[k]);
    }
    mi /= n;
    mq /= n;
    int cycles = rf_side * BP_SPECTRUM_SIGN;
    double acc = 0, wsum = 0;
    for (int k = 0; k < BP_BLOCK; ++k) wsum += 0.5 - 0.5 * cos(2 * PI * k / BP_BLOCK);
    size_t blocks = n / BP_BLOCK;
    for (size_t b = 0; b < blocks; ++b) {
        double re = 0, im = 0;
        for (int k = 0; k < BP_BLOCK; ++k) {
            size_t idx = b * BP_BLOCK + k;
            double i = bp_sample_i(s[idx]) - mi, q = bp_sample_q(s[idx]) - mq;
            double w = 0.5 - 0.5 * cos(2 * PI * k / BP_BLOCK);
            double ph = -2 * PI * cycles * k / BP_BLOCK;
            re += w * (i * cos(ph) - q * sin(ph));
            im += w * (i * sin(ph) + q * cos(ph));
        }
        acc += re * re + im * im;
    }
    return acc / blocks / (wsum * wsum);
}

static bp_coeffs_t pilot, mirror;

static float measure(int rf_side, bp_result_t *r)
{
    bp_measure(buf, N, rf_side > 0 ? &pilot : &mirror, 30, r);
    return r->db;
}

static void test_decode(void)
{
    assert(bp_sample_i(bp_make_sample(-512, 0, 0)) == -512);
    assert(bp_sample_i(bp_make_sample(511, -1, 0)) == 511);
    assert(bp_sample_q(bp_make_sample(5, -1, 0)) == -1);
    assert(bp_sample_q(bp_make_sample(0, -512, 89)) == -512);
    assert(bp_sample_gain(bp_make_sample(-3, 7, 89)) == 89);
    assert(bp_sample_i(0x3FFu) == -1 && bp_sample_q(0x3FFu << 10) == -1);
}

static void test_tone(void)
{
    bp_result_t r;
    /* A tone of amplitude A at the pilot reads A^2: 100 -> 40 dB. */
    synth(100.0, BP_LO_OFFSET_MHZ, 0, 0, 0, 1, 30);
    float db = measure(+1, &r);
    printf("pilot tone A=100: %.2f dB\n", (double)db);
    assert(fabsf(db - 40.0f) < 0.1f);
    assert(r.clipped == 0 && r.gain_mismatch == 0 && r.used == N);

    /* The mirror band sees almost nothing of it (exact Hann null at bin 2). */
    float mdb = measure(-1, &r);
    printf("same tone in mirror band: %.2f dB\n", (double)mdb);
    assert(db - mdb > 40.0f);

    /* A tone on the mirror side is rejected by the pilot band. */
    synth(100.0, -BP_LO_OFFSET_MHZ, 0, 0, 0, 1, 30);
    assert(measure(+1, &r) < 0.0f);

    /* Raceband neighbours, 37 MHz either side of the pilot: -27 and +47 MHz
     * from the LO, well outside the ~10 MHz band. */
    synth(100.0, BP_LO_OFFSET_MHZ + 37, 0, 0, 0, 1, 30);
    float n_hi = measure(+1, &r);
    synth(100.0, BP_LO_OFFSET_MHZ - 37, 0, 0, 0, 1, 30);
    float n_lo = measure(+1, &r);
    printf("Raceband neighbours: %.2f / %.2f dB\n", (double)n_hi, (double)n_lo);
    assert(n_hi < 10.0f && n_lo < 10.0f);

    /* Inside the band the response is broad: 3 MHz off still reads close. */
    synth(100.0, BP_LO_OFFSET_MHZ + 3, 0, 0, 0, 1, 30);
    assert(measure(+1, &r) > 36.0f);
}

static void test_dc_and_reference(void)
{
    bp_result_t r;
    /* DC alone reads nothing; DC under a tone changes nothing. */
    synth(0.0, 0, -5.0, 10.0, 0, 1, 30);
    assert(measure(+1, &r) == BP_DB_FLOOR);
    assert(fabsf(r.dc_i + 5.0f) < 0.01f && fabsf(r.dc_q - 10.0f) < 0.01f);
    synth(30.0, BP_LO_OFFSET_MHZ, -5.0, 10.0, 0, 1, 30);
    float with_dc = measure(+1, &r);
    synth(30.0, BP_LO_OFFSET_MHZ, 0, 0, 0, 1, 30);
    float without = measure(+1, &r);
    assert(fabsf(with_dc - without) < 0.05f);

    /* Single-pass integer meter against the two-pass double reference, on
     * noise, noise + DC and noisy tones, both bands. */
    struct { double amp, off, dci, dcq, noise; } cases[] = {
        {0, 0, 0, 0, 1.2},      {0, 0, -5, 10, 1.2},   {20, 10, -2, 3, 1.2},
        {200, 10, 1, -1, 4.0},  {50, 13, 0, 0, 2.0},   {8, -10, 6, 6, 1.0},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        synth(cases[c].amp, cases[c].off, cases[c].dci, cases[c].dcq, cases[c].noise,
              (unsigned)(c + 7), 30);
        for (int side = -1; side <= 1; side += 2) {
            bp_measure(buf, N, side > 0 ? &pilot : &mirror, 30, &r);
            double ref = reference_power(buf, N, side);
            double rel = fabs(r.power - ref) / (ref > 1e-9 ? ref : 1e-9);
            assert(rel < 2e-3); /* only Q14 coefficient rounding differs */
        }
    }

    /* Noise floor: per-axis variance s^2 reads 2 s^2 * sum(w^2) / sum(w)^2
     * = 0.375 s^2 for a periodic Hann of 8. */
    synth(0.0, 0, 0, 0, 1.5, 3, 30);
    bp_measure(buf, N, &pilot, 30, &r);
    printf("noise s=1.5: %.3f LSB^2 (expect ~%.3f)\n", (double)r.power, 0.375 * (1.5 * 1.5 + 1.0 / 12)); /* + quantisation */
    assert(r.power > 0.6f && r.power < 1.2f);
}

static void test_flags(void)
{
    bp_result_t r;
    synth(800.0, BP_LO_OFFSET_MHZ, 0, 0, 0, 1, 30); /* overdriven */
    measure(+1, &r);
    assert(r.clipped > N / 4);
    synth(10.0, BP_LO_OFFSET_MHZ, 0, 0, 0, 1, 31); /* wrong gain field */
    measure(+1, &r);
    assert(r.gain_mismatch == N);
    bp_measure(buf, 7, &pilot, 31, &r); /* less than one block */
    assert(r.used == 0 && r.db == BP_DB_FLOOR);
}

static void test_filter(void)
{
    bp_filter_t f;
    bp_filter_reset(&f);
    assert(bp_filter_push(&f, 10.0f) == 10.0f); /* first reading passes */
    for (int k = 0; k < 50; ++k) bp_filter_push(&f, 10.0f);
    /* A single 30 dB spike is removed by the median. */
    assert(fabsf(bp_filter_push(&f, 40.0f) - 10.0f) < 1e-4f);
    assert(fabsf(bp_filter_push(&f, 10.0f) - 10.0f) < 1e-4f);
    /* A step settles through the EMA. */
    float v = 0;
    for (int k = 0; k < 3; ++k) v = bp_filter_push(&f, 20.0f);
    assert(v > 10.0f && v < 20.0f);
    for (int k = 0; k < 40; ++k) v = bp_filter_push(&f, 20.0f);
    assert(fabsf(v - 20.0f) < 0.01f);
    assert(bp_median3(1, 3, 2) == 2 && bp_median3(3, 1, 2) == 2 && bp_median3(2, 3, 1) == 2);
}

static void test_rssi(void)
{
    assert(bp_rssi_from_db(BP_DB_FLOOR) == 0);
    assert(bp_rssi_from_db(BP_RSSI_DB_MIN) == 0);
    assert(bp_rssi_from_db(BP_RSSI_DB_MAX) == 255);
    assert(bp_rssi_from_db(80.0f) == 255);
    assert(bp_rssi_from_db(NAN) == 0);
    int a = bp_rssi_from_db(0.0f), b = bp_rssi_from_db(10.0f);
    assert(b - a >= 42 && b - a <= 43); /* 4.25 counts per dB */
    uint8_t prev = 0;
    for (float db = -10.0f; db < 60.0f; db += 0.25f) {
        uint8_t v = bp_rssi_from_db(db);
        assert(v >= prev);
        prev = v;
    }
}

int main(void)
{
    bp_coeffs_init(&pilot, +1);
    bp_coeffs_init(&mirror, -1);
    test_decode();
    test_tone();
    test_dc_and_reference();
    test_flags();
    test_filter();
    test_rssi();
    puts("band_power: all tests passed");
    return 0;
}
