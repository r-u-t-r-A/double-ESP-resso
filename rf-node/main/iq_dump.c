/*
 * iq_dump.c - raw 10-bit I/Q captures from the ESP32-C5 PHY's ADC dump engine.
 *
 * Follows FPVGate C5MK's published description (docs/RADIO_INTERNALS.md
 * sections 3-4). The register sequence was rechecked against our own
 * disassembly of adctrig() in ESP-IDF v6.0.2's librftest.a (Apache-2.0):
 *
 *   0x600A9004 control: count bits 0-16 (= N), done bit 18, trigger bit 19,
 *                       enable bit 31; adctrig clears bits 17, 20 and 21
 *   0x600A9008 setup:   rate bits 21-23, source bits 17-20 (left as adctrig
 *                       programmed them)
 *   0x60095004 SRAM:    bits 8-11 = 2 and bit 16 hand the bank to the modem;
 *                       adctrig clears both afterwards
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "iq_dump.h"

#include <stdbool.h>

#include "esp_cpu.h"
#include "esp_private/esp_clk.h"
#include "freertos/FreeRTOS.h"
#include "heap_memory_layout.h"

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))

#define DUMP_CTRL        0x600A9004u
#define CTRL_COUNT       0x0001FFFFu
#define CTRL_BIT17       (1u << 17)
#define CTRL_DONE        (1u << 18)
#define CTRL_TRIGGER     (1u << 19)
#define CTRL_BITS20_21   (3u << 20)
#define CTRL_ENABLE      (1u << 31)

#define SRAM_OWNER       0x60095004u
#define SRAM_OWNER_FIELD 0x00000F00u
#define SRAM_OWNER_MODEM 0x00000200u
#define SRAM_OWNER_HOLD  0x00010000u

/* The engine writes sample words from 0x40830000, and while it runs the
 * modem owns the whole 128 KiB bank 0x40820000-0x4083FFFF, so none of it may
 * hold heap. check_sram.py fails the build if static data reaches it. */
#define IQ_DUMP_BASE     0x40830000u
#define DUMP_BANK_START  0x40820000u
#define DUMP_BANK_END    0x40840000u
SOC_RESERVE_MEMORY_REGION(DUMP_BANK_START, DUMP_BANK_END, iq_dump_bank);

#define SENTINEL         0xDEADBEEFu /* bits 27-31 are never set in a sample */
#define GUARD            0xA5A5A5A0u
#define GUARD_WORDS      4u
#define TIMEOUT_US       1000u

/* adctrig(count - 1, selector, mode, rate code, -, wait style, -, -, -).
 * Only selector 0, mode 0, rate 0 (80 MS/s), wait style 0 is a pure receive
 * dump: mode 1 transmits a tone, selector 5/6 or mode 2 transmits a frame,
 * selector 12 starts the BLE receiver. stock_capture() is the only caller. */
extern void adctrig(uint32_t count_m1, uint32_t selector, uint32_t mode,
                    uint32_t rate, uint32_t unused, uint32_t wait_style,
                    uint32_t arg7, uint32_t arg8, uint32_t arg9);

/* librftest's wifi.o references the RF test console's parser. Never called. */
int cmd_parse(void *arg) __attribute__((weak));
int cmd_parse(void *arg)
{
    (void)arg;
    return -1;
}

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_programmed;
static iq_dump_stats_t s_stats;

static volatile uint32_t *const s_words = (volatile uint32_t *)IQ_DUMP_BASE;

static void arm_guards(uint32_t n)
{
    s_words[n - 1] = SENTINEL;
    for (uint32_t g = 0; g < GUARD_WORDS; ++g) s_words[n + g] = GUARD | g;
    __asm__ __volatile__("fence rw, rw" ::: "memory");
}

static esp_err_t check_guards(uint32_t n)
{
    __asm__ __volatile__("fence rw, rw" ::: "memory");
    if (s_words[n - 1] == SENTINEL) {
        ++s_stats.short_reject;
        return ESP_ERR_INVALID_SIZE;
    }
    for (uint32_t g = 0; g < GUARD_WORDS; ++g) {
        if (s_words[n + g] != (GUARD | g)) {
            ++s_stats.long_reject;
            return ESP_ERR_INVALID_SIZE;
        }
    }
    return ESP_OK;
}

esp_err_t iq_dump_stock(uint32_t n, const uint32_t **samples)
{
    if (n == 0 || n > IQ_DUMP_MAX_SAMPLES) return ESP_ERR_INVALID_ARG;
    arm_guards(n);
    adctrig(n - 1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u);
    ++s_stats.stock;
    esp_err_t err = check_guards(n);
    if (err != ESP_OK) return err;
    s_programmed = true;
    *samples = (const uint32_t *)IQ_DUMP_BASE;
    return ESP_OK;
}

static bool direct_capture(uint32_t n)
{
    const uint32_t timeout_cycles = (uint32_t)(esp_clk_cpu_freq() / 1000000) * TIMEOUT_US;
    bool done = false;

    portENTER_CRITICAL(&s_mux);
    uint32_t owner = REG32(SRAM_OWNER);
    REG32(SRAM_OWNER) = (owner & ~SRAM_OWNER_FIELD) | SRAM_OWNER_MODEM;
    REG32(SRAM_OWNER) |= SRAM_OWNER_HOLD;

    /* Same order as adctrig: count, enable, then a trigger pulse. */
    uint32_t ctrl = REG32(DUMP_CTRL);
    ctrl &= ~(CTRL_COUNT | CTRL_BIT17 | CTRL_BITS20_21 | CTRL_TRIGGER | CTRL_ENABLE);
    ctrl |= n & CTRL_COUNT;
    REG32(DUMP_CTRL) = ctrl;
    ctrl |= CTRL_ENABLE;
    REG32(DUMP_CTRL) = ctrl;
    REG32(DUMP_CTRL) = ctrl | CTRL_TRIGGER;
    REG32(DUMP_CTRL) = ctrl;

    uint32_t start = esp_cpu_get_cycle_count();
    while (esp_cpu_get_cycle_count() - start < timeout_cycles) {
        if (REG32(DUMP_CTRL) & CTRL_DONE) {
            done = true;
            break;
        }
    }

    REG32(DUMP_CTRL) &= ~CTRL_ENABLE;
    REG32(SRAM_OWNER) &= ~SRAM_OWNER_FIELD;
    REG32(SRAM_OWNER) &= ~SRAM_OWNER_HOLD;
    portEXIT_CRITICAL(&s_mux);
    return done;
}

esp_err_t iq_dump_capture(uint32_t n, const uint32_t **samples)
{
    if (n == 0 || n > IQ_DUMP_MAX_SAMPLES) return ESP_ERR_INVALID_ARG;
    if (!s_programmed) return iq_dump_stock(n, samples);

    arm_guards(n);
    if (!direct_capture(n)) {
        ++s_stats.timeouts;
        s_programmed = false; /* reprogram through adctrig next time */
        return ESP_ERR_TIMEOUT;
    }
    ++s_stats.direct;
    esp_err_t err = check_guards(n);
    if (err != ESP_OK) return err;
    *samples = (const uint32_t *)IQ_DUMP_BASE;
    return ESP_OK;
}

void iq_dump_stats(iq_dump_stats_t *out)
{
    *out = s_stats;
}
