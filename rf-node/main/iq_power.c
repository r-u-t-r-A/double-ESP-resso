/*
 * iq_power.c - MODEM_DIAG Q4/I4 capture and window power.
 *
 * PARLIO RX setup, the interrupt-free infinite ring and the completed-
 * descriptor window picker are copied from C5VRX main/video.c (prepare_rx,
 * video_start, patch_descriptors_clear_eof, get_completed_rx_sample_window),
 * which is proven on hardware.
 *
 * A finite soft-delimiter receive per sample was tried first. On hardware
 * the first capture succeeded (716 ms after boot) and then every task starved
 * (no further captures, no errors, console dead), which is consistent with an
 * RX interrupt storm while the free-running clock keeps sampling. The ring
 * with all RX DMA/PARLIO interrupts disabled avoids that entirely.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "iq_power.h"

#include <string.h>

#include "driver/parlio_rx.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "hal/dma_types.h"
#include "soc/ahb_dma_struct.h"
#include "soc/parl_io_struct.h"
#include "rssi_scale.h"

#define IQ_RATE_HZ      40000000u
#define RAW_RING_BYTES  32768u
#define MAX_RING_DSCR   16
#define PARLIO_PERI_ID  9          /* AHB GDMA peripheral select for PARL_IO */

static parlio_rx_unit_handle_t s_rx;
static parlio_rx_delimiter_handle_t s_delimiter;
static DMA_ATTR __attribute__((aligned(64))) uint8_t s_ring[RAW_RING_BYTES];
static uint8_t s_window[IQ_WINDOW_BYTES];

/* Per-byte power (bits 0..8) and rail flag (bit 15): one load per sample. */
static uint16_t s_lut[256];
#define LUT_CLIP 0x8000u

typedef struct {
    dma_descriptor_t *dscr;
    uint8_t *buffer;
    uint32_t length;
} ring_node_t;

static ring_node_t s_nodes[MAX_RING_DSCR];
static int s_node_count;
static int s_dma_ch = -1;

static inline void sync_m2c(const void *addr, size_t size)
{
    uint32_t start = (uint32_t)addr & ~(64u - 1u);
    uint32_t end = ((uint32_t)addr + size + 63u) & ~(64u - 1u);
    (void)esp_cache_msync((void *)start, end - start,
                          ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static inline void sync_c2m(const void *addr, size_t size)
{
    uint32_t start = (uint32_t)addr & ~(64u - 1u);
    uint32_t end = ((uint32_t)addr + size + 63u) & ~(64u - 1u);
    (void)esp_cache_msync((void *)start, end - start,
                          ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

/* Clear suc_eof on every RX descriptor so the ring never stalls on EOF, and
 * remember the descriptor list for the window picker. */
static int patch_ring_descriptors(void)
{
    uint32_t first = AHB_DMA.channel[s_dma_ch].in.in_dscr_bf0.val;
    if (first < 0x40800000u || first >= 0x40860000u) return 0;
    dma_descriptor_t *curr = (dma_descriptor_t *)(uintptr_t)first;
    int count = 0;
    while (curr && count < MAX_RING_DSCR) {
        curr->dw0.suc_eof = 0;
        sync_c2m(curr, sizeof(*curr));
        s_nodes[count].dscr = curr;
        s_nodes[count].buffer = (uint8_t *)curr->buffer;
        s_nodes[count].length = curr->dw0.size ? curr->dw0.size : 4092u;
        curr = curr->next;
        ++count;
        if ((uintptr_t)curr == first) break;
    }
    __asm__ __volatile__("fence rw, rw" ::: "memory");
    return count;
}

/* A descriptor GDMA has already finished, never the one being written. */
static const uint8_t *completed_window(void)
{
    uint32_t current = AHB_DMA.channel[s_dma_ch].in.in_dscr_bf0.val;
    int idx = -1;
    for (int i = 0; i < s_node_count; ++i) {
        if ((uintptr_t)s_nodes[i].dscr == current) { idx = i; break; }
    }
    if (idx < 0) return NULL;
    for (int back = 1; back < s_node_count; ++back) {
        const ring_node_t *n = &s_nodes[(idx - back + s_node_count) % s_node_count];
        if (n->buffer && n->length >= IQ_WINDOW_BYTES &&
            n->buffer >= s_ring && n->buffer + IQ_WINDOW_BYTES <= s_ring + sizeof(s_ring))
            return n->buffer;
    }
    return NULL;
}

esp_err_t iq_power_init(void)
{
    for (unsigned b = 0; b < 256u; ++b) {
        s_lut[b] = rssi_sample_power((uint8_t)b) |
                   (rssi_sample_clipped((uint8_t)b) ? LUT_CLIP : 0u);
    }

    const parlio_rx_unit_config_t cfg = {
        .trans_queue_depth = 1u,
        .max_recv_size = sizeof(s_ring),
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
        .eof_data_len = sizeof(s_ring),
        .timeout_ticks = 0u,
    };
    err = parlio_new_rx_soft_delimiter(&delim_cfg, &s_delimiter);
    if (err != ESP_OK) return err;
    err = parlio_rx_unit_enable(s_rx, false);
    if (err != ESP_OK) return err;

    memset(s_ring, 0, sizeof(s_ring));
    sync_c2m(s_ring, sizeof(s_ring));

    /* Start the cyclic ring exactly like C5VRX start_rx(). */
    err = parlio_rx_soft_delimiter_start_stop(s_rx, s_delimiter, true);
    if (err != ESP_OK) return err;
    const parlio_receive_config_t rcfg = {
        .delimiter = s_delimiter,
        .flags = {
            .partial_rx_en = true,
            .indirect_mount = false,
        },
    };
    err = parlio_rx_unit_receive(s_rx, s_ring, sizeof(s_ring), &rcfg);
    if (err != ESP_OK) return err;

    /* Pure continuous hardware mode: no GDMA RX or PARLIO interrupts at all,
     * and no EOF generation, so the CPU is never interrupted by capture. */
    AHB_DMA.in_intr[0].ena.val = 0;
    AHB_DMA.in_intr[1].ena.val = 0;
    AHB_DMA.in_intr[2].ena.val = 0;
    PARL_IO.rx_genrl_cfg.rx_eof_gen_sel = 1;
    PARL_IO.int_ena.val = 0;

    for (int i = 0; i < 3; ++i) {
        if (AHB_DMA.channel[i].in.in_peri_sel.peri_in_sel_chn == PARLIO_PERI_ID)
            s_dma_ch = i;
    }
    if (s_dma_ch < 0) return ESP_ERR_NOT_FOUND;
    s_node_count = patch_ring_descriptors();
    if (s_node_count < 2) return ESP_ERR_INVALID_STATE;

    PARL_IO.int_clr.val = UINT32_MAX;
    AHB_DMA.in_intr[s_dma_ch].clr.val = UINT32_MAX;
    return ESP_OK;
}

esp_err_t iq_power_measure(iq_power_t *out)
{
    const uint8_t *src = completed_window();
    if (!src) return ESP_ERR_INVALID_STATE;
    sync_m2c(src, IQ_WINDOW_BYTES);
    memcpy(s_window, src, IQ_WINDOW_BYTES);

    uint32_t sum = 0;
    uint32_t clipped = 0;
    for (size_t i = 0; i < IQ_WINDOW_BYTES; ++i) {
        uint16_t v = s_lut[s_window[i]];
        sum += v & ~LUT_CLIP;
        clipped += v >> 15;
    }
    out->mean_power = (float)sum / (float)IQ_WINDOW_BYTES;
    out->clip_permille = (uint16_t)(clipped * 1000u / IQ_WINDOW_BYTES);
    out->rssi = rssi_from_mean_power(out->mean_power);
    return ESP_OK;
}
