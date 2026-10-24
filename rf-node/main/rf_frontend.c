/*
 * rf_frontend.c - ESP32-C5 receive-only 5.8 GHz frontend for the RF node.
 *
 * Follows FPVGate C5MK's published description (docs/RADIO_INTERNALS.md
 * sections 6-8, https://github.com/LouisHitchcock/FPVGateC5MK). No C5MK
 * code is used. The calls and phy_param offsets were rechecked against our
 * own disassembly of ESP-IDF v6.0.2's libphy.a / librftest.a (Apache-2.0):
 *
 *  - phy_set_chanfreq(mhz, mode) calibrates the band through
 *    phy_chip_set_chan(); mode 1 is the wide analog bandwidth.
 *  - phy_set_rf_freq_offset(xtal, mhz, trim) retunes only the PLL. Espressif's
 *    own channel code (phy_chip_set_chan, phy_set_channel_rfpll_freq_new)
 *    passes xtal = phy_param byte 49 and trim = the int16 at byte 30.
 *  - force_rx_gain(1, idx, 0) writes the index to bits 24-31 of 0x600A702C
 *    and the force flag to bit 23. A nonzero third argument takes a
 *    Bluetooth path.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rf_frontend.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "band_power.h"
#include "iq_dump.h"
#include "node.h"

#define REG32(a)          (*(volatile uint32_t *)(uintptr_t)(a))

#define AGC_FORCE_REG     0x600A702Cu
#define AGC_FORCE_EN      (1u << 23)
#define AGC_FORCE_SHIFT   24
#define AGC_GAIN_MAX_SHIFT 8
#define AGC_GAIN_MAX_MASK 0x7Fu

#define BAND_SETUP_MHZ    5180u
#define BAND_WIDE         1u

#define PHY_PARAM_TRIM    30 /* int16 */
#define PHY_PARAM_XTAL    49 /* uint8 */

#define CHECK_SAMPLES     1024u

/* Closed ESP32-C5 PHY / RF test library symbols (esp_phy, Apache-2.0). */
extern uint8_t phy_param[];
extern int phy_set_chanfreq(uint32_t mhz, uint32_t bw_mode);
extern void phy_set_rf_freq_offset(uint32_t xtal, uint32_t mhz, int32_t trim);
extern void force_rx_gain(uint32_t enable, uint32_t gain_idx, uint32_t bt);
extern void phy_pbus_workmode(void);
extern void phy_pbus_xpd_tx_off(void);
extern void phy_pbus_xpd_rx_on(uint32_t on);
extern void phy_set_rxclk_en(uint32_t on);

static const char *TAG = "rf";
static uint16_t s_mhz;
static uint8_t s_gain = 30u;
static uint8_t s_gain_max = 89u;
static const char *s_check = "not_run";

bool rf_frontend_can_tune(uint16_t mhz)
{
    return mhz >= RF_MIN_MHZ && mhz <= RF_MAX_MHZ;
}

static void force_gain(void)
{
    force_rx_gain(1u, s_gain, 0u);
}

bool rf_frontend_hold_gain(void)
{
    /* Rewriting the force while it already holds disturbs the receiver
     * (C5MK RF_RESEARCH.md section 3), so write only when it is lost. */
    uint32_t v = REG32(AGC_FORCE_REG);
    if ((v & AGC_FORCE_EN) && (v >> AGC_FORCE_SHIFT) == s_gain) return false;
    force_gain();
    return true;
}

/* A capture is live if I/Q actually vary and carry the forced gain. */
static bool capture_live(bool stock)
{
    const uint32_t *s;
    esp_err_t err = stock ? iq_dump_stock(CHECK_SAMPLES, &s)
                          : iq_dump_capture(CHECK_SAMPLES, &s);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "check capture failed: %s", esp_err_to_name(err));
        return false;
    }
    int64_t si = 0, sq = 0, si2 = 0, sq2 = 0;
    uint32_t gain_ok = 0;
    for (uint32_t k = 0; k < CHECK_SAMPLES; ++k) {
        int32_t i = bp_sample_i(s[k]), q = bp_sample_q(s[k]);
        si += i;
        sq += q;
        si2 += i * i;
        sq2 += q * q;
        gain_ok += bp_sample_gain(s[k]) == s_gain;
    }
    double n = CHECK_SAMPLES;
    double var = (double)(si2 + sq2) / n - ((double)si * si + (double)sq * sq) / (n * n);
    ESP_LOGI(TAG, "check (%s): I/Q variance %.2f LSB^2, gain field %lu/%lu",
             stock ? "stock" : "direct", var, (unsigned long)gain_ok,
             (unsigned long)CHECK_SAMPLES);
    return var > 0.05 && gain_ok > CHECK_SAMPLES * 9u / 10u;
}

static void tune_lo(uint16_t pilot_mhz)
{
    int16_t trim;
    memcpy(&trim, &phy_param[PHY_PARAM_TRIM], sizeof(trim));
    phy_set_rf_freq_offset(phy_param[PHY_PARAM_XTAL],
                           (uint32_t)pilot_mhz - BP_LO_OFFSET_MHZ, trim);
}

esp_err_t rf_frontend_start(uint16_t mhz, uint8_t gain_idx)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    /* No interface: nothing associates or transmits. Promiscuous receive
     * with an empty filter keeps the PHY powered without buffering frames. */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    node_trace("t10_wifi_init");
    if ((err = esp_wifi_init(&cfg)) != ESP_OK) return err;
    if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) return err;
    if ((err = esp_wifi_set_mode(WIFI_MODE_NULL)) != ESP_OK) return err;
    if ((err = esp_wifi_start()) != ESP_OK) return err;
    node_trace("t11_wifi_start");
    if ((err = esp_wifi_set_ps(WIFI_PS_NONE)) != ESP_OK) return err;
    wifi_promiscuous_filter_t filter = {.filter_mask = 0};
    (void)esp_wifi_set_promiscuous_filter(&filter);
    if ((err = esp_wifi_set_promiscuous(true)) != ESP_OK) return err;
    if ((err = esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE)) != ESP_OK) return err;
    node_trace("t12_promisc");

    /* adctrig() prints its registers through phy_printf at INFO. */
    esp_log_level_set("phy", ESP_LOG_WARN);

    uint32_t agc = REG32(AGC_FORCE_REG);
    uint8_t gmax = (uint8_t)((agc >> AGC_GAIN_MAX_SHIFT) & AGC_GAIN_MAX_MASK);
    if (gmax) s_gain_max = gmax;
    s_gain = gain_idx > s_gain_max ? s_gain_max : gain_idx;

    phy_set_chanfreq(BAND_SETUP_MHZ, BAND_WIDE);
    node_trace("t13_band");
    force_gain();

    /* Boot check: captures must be live. Then switch the transmit path off
     * as a safety measure and check again; only if captures went flat turn
     * the receive path on explicitly. */
    bool live = capture_live(true);
    phy_pbus_workmode();
    phy_pbus_xpd_tx_off();
    bool live_tx_off = capture_live(true);
    s_check = "live";
    if (!live_tx_off) {
        phy_pbus_xpd_rx_on(1u);
        phy_set_rxclk_en(1u);
        s_check = capture_live(true) ? "live_rx_on" : "flat";
    }
    ESP_LOGI(TAG, "boot check: %s (before TX off: %s)", s_check, live ? "live" : "flat");
    node_trace("t14_check");

    s_mhz = BAND_SETUP_MHZ;
    err = rf_frontend_tune(rf_frontend_can_tune(mhz) ? mhz : RF_MIN_MHZ);
    if (err != ESP_OK) return err;
    (void)capture_live(false); /* first direct capture, logged */
    node_trace("t15_tuned");
    ESP_LOGI(TAG, "RF ready: %u MHz (LO %u), gain %u of %u", s_mhz,
             s_mhz - BP_LO_OFFSET_MHZ, s_gain, s_gain_max);
    return ESP_OK;
}

esp_err_t rf_frontend_tune(uint16_t mhz)
{
    if (!rf_frontend_can_tune(mhz)) return ESP_ERR_NOT_SUPPORTED;
    node_trace("t70_tune_start");
    tune_lo(mhz);
    (void)rf_frontend_hold_gain();
    node_trace("t71_tune_done");
    s_mhz = mhz;
    return ESP_OK;
}

void rf_frontend_set_gain(uint8_t gain_idx)
{
    s_gain = gain_idx > s_gain_max ? s_gain_max : gain_idx;
    (void)rf_frontend_hold_gain();
}

uint16_t rf_frontend_mhz(void)
{
    return s_mhz;
}

uint8_t rf_frontend_gain(void)
{
    return s_gain;
}

uint8_t rf_frontend_gain_max(void)
{
    return s_gain_max;
}

const char *rf_frontend_check(void)
{
    return s_check;
}
