/*
 * ledstrip_timing.h - per-chip bit timing for single-wire RGB LED drivers,
 * in 100 ns RMT ticks (10 MHz). Header-only, plain C99: shared by the AP node
 * and the host test that checks each entry against its datasheet window.
 *
 * Datasheet nominal values (each +-150 ns):
 *   WS2812B          T0H 400  T0L 850   T1H 800  T1L 450   reset >= 280 us
 *   WS2812           T0H 350  T0L 800   T1H 700  T1L 600   reset >= 50 us
 *   WS2811 800 kHz   T0H 250  T0L 1000  T1H 600  T1L 650   reset >= 280 us
 *   WS2811 400 kHz   T0H 500  T0L 2000  T1H 1200 T1L 1300  reset >= 280 us
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LEDSTRIP_TICK_NS 100u
#define LEDSTRIP_RESET_US 300u  // covers the longest reset of all chips

typedef enum {
    LEDSTRIP_WS2812B = 0,
    LEDSTRIP_WS2812,
    LEDSTRIP_WS2811,       // high-speed (800 kHz) mode, used by most strips
    LEDSTRIP_WS2811_400K,  // low-speed mode (SET pin tied for 400 kHz)
    LEDSTRIP_CHIP_COUNT,
} ledstrip_chip_e;

typedef struct {
    uint8_t t0h, t0l, t1h, t1l;  // ticks
} ledstrip_timing_t;

static const ledstrip_timing_t LEDSTRIP_TIMINGS[LEDSTRIP_CHIP_COUNT] = {
    {4, 8, 8, 4},     // WS2812B: 1.2 us bit
    {4, 8, 7, 6},     // WS2812: 1.2 / 1.3 us bit
    {3, 9, 6, 6},     // WS2811 800 kHz: 1.2 us bit
    {5, 20, 12, 13},  // WS2811 400 kHz: 2.5 us bit
};

static inline const ledstrip_timing_t *ledstrip_timing(unsigned chip)
{
    return &LEDSTRIP_TIMINGS[chip < LEDSTRIP_CHIP_COUNT ? chip : LEDSTRIP_WS2812B];
}

/* Wire byte order, as a byte offset of R, G and B within each 3-byte pixel. */
typedef enum {
    LEDSTRIP_GRB = 0,  // WS2812B, WS2812
    LEDSTRIP_RGB,      // most WS2811 ICs
    LEDSTRIP_BRG,
    LEDSTRIP_RBG,
    LEDSTRIP_GBR,
    LEDSTRIP_BGR,
    LEDSTRIP_ORDER_COUNT,
} ledstrip_order_e;

static inline unsigned ledstrip_order_index(unsigned order)
{
    return order < LEDSTRIP_ORDER_COUNT ? order : LEDSTRIP_GRB;
}

#ifdef __cplusplus
}
#endif
