/*
 * iq_power.c - bounded MODEM_DIAG Q4/I4 snapshots and their mean power.
 *
 * PARLIO RX pin order, clock and bit packing are copied from C5VRX
 * main/video.c prepare_rx(). Unlike C5VRX this does not run an infinite
 * ring: a lap timer only needs a representative ~100 us power window per
 * millisecond, so each measurement is one finite soft-delimiter receive.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "iq_power.h"

#include <string.h>

#include "driver/parlio_rx.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "rssi_scale.h"

#define IQ_RATE_HZ 40000000u

static parlio_rx_unit_handle_t s_rx;
static parlio_rx_delimiter_handle_t s_delimiter;
static DMA_ATTR __attribute__((aligned(64))) uint8_t s_window[IQ_WINDOW_BYTES];
/* Per-byte power (bits 0..8) and rail flag (bit 15): one load per sample. */
static uint16_t s_lut[256];
#define LUT_CLIP 0x8000u

esp_err_t iq_power_init(void)
{
    for (unsigned b = 0; b < 256u; ++b) {
        s_lut[b] = rssi_sample_power((uint8_t)b) |
                   (rssi_sample_clipped((uint8_t)b) ? LUT_CLIP : 0u);
    }

    const parlio_rx_unit_config_t cfg = {
        .trans_queue_depth = 1u,
        .max_recv_size = IQ_WINDOW_BYTES,
        .dma_burst_size = 32u,
        .data_width = 8u,
        .clk_src = PARLIO_CLK_SRC_DEFAULT,
        .ext_clk_freq_hz = 0u,
        .exp_clk_freq_hz = IQ_RATE_HZ,
        .clk_in_gpio_num = -1,
        .clk_out_gpio_num = -1,
        .valid_gpio_num = -1,
        /* Must match s_iq_pins[] in rf_frontend.c:
         * Q[9:6] on GPIO 1,0,25,7 then I[9:6] on GPIO 10,5,3,4. */
        .data_gpio_nums = {
            GPIO_NUM_1, GPIO_NUM_0, GPIO_NUM_25, GPIO_NUM_7,
            GPIO_NUM_10, GPIO_NUM_5, GPIO_NUM_3, GPIO_NUM_4,
        },
        .flags = {
            .free_clk = true,
            .clk_gate_en = false,
            .allow_pd = false,
        },
    };
    esp_err_t err = parlio_new_rx_unit(&cfg, &s_rx);
    if (err != ESP_OK) return err;

    const parlio_rx_soft_delimiter_config_t delim_cfg = {
        .sample_edge = PARLIO_SAMPLE_EDGE_POS,
        .bit_pack_order = PARLIO_BIT_PACK_ORDER_LSB,
        .eof_data_len = IQ_WINDOW_BYTES,
        .timeout_ticks = 0u,
    };
    err = parlio_new_rx_soft_delimiter(&delim_cfg, &s_delimiter);
    if (err != ESP_OK) return err;

    return parlio_rx_unit_enable(s_rx, true);
}

esp_err_t iq_power_measure(iq_power_t *out)
{
    const parlio_receive_config_t rcfg = {
        .delimiter = s_delimiter,
        .flags = {
            .partial_rx_en = false,
            .indirect_mount = false,
        },
    };
    esp_err_t err = parlio_rx_unit_receive(s_rx, s_window, sizeof(s_window), &rcfg);
    if (err != ESP_OK) return err;
    err = parlio_rx_soft_delimiter_start_stop(s_rx, s_delimiter, true);
    if (err != ESP_OK) return err;
    err = parlio_rx_unit_wait_all_done(s_rx, 5);
    (void)parlio_rx_soft_delimiter_start_stop(s_rx, s_delimiter, false);
    if (err != ESP_OK) return err;

    (void)esp_cache_msync(s_window, sizeof(s_window),
                          ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

    uint32_t sum = 0;
    uint32_t clipped = 0;
    for (size_t i = 0; i < sizeof(s_window); ++i) {
        uint16_t v = s_lut[s_window[i]];
        sum += v & ~LUT_CLIP;
        clipped += v >> 15;
    }
    out->mean_power = (float)sum / (float)sizeof(s_window);
    out->clip_permille = (uint16_t)(clipped * 1000u / sizeof(s_window));
    out->rssi = rssi_from_mean_power(out->mean_power);
    return ESP_OK;
}
