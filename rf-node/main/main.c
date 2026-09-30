/*
 * main.c - double-ESP-resso RF node.
 *
 * Measures 5.8 GHz carrier power on one FPV channel at 1 kHz and streams it
 * to the AP node over UART. The AP node owns the configuration; this node
 * stores the last frequency/gain in NVS so it keeps measuring on its own.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "node.h"
#include "rf_frontend.h"

#define DEFAULT_MHZ  5658u /* R1 */
#define RF_BOOT_MHZ  5865u /* Wi-Fi ch173: no phy_set_freq() needed */
#define DEFAULT_GAIN 40u
#define SAMPLE_PERIOD_MS 1u

static const char *TAG = "node";

/* Serializes PHY retune/gain writes against capture. */
static SemaphoreHandle_t s_rf_lock;
static volatile c5link_state_t s_state = C5LINK_ST_OK;

static volatile const char *s_stage = "reset";
static volatile uint32_t s_measure_ok;
static volatile uint32_t s_measure_err;
static volatile int s_last_err;

static nvs_handle_t s_trace = 0;
/* Non-zero when the previous boot hung inside a retune to this frequency. */
static uint16_t s_prev_hang_mhz;

void node_trace(const char *key)
{
    if (!s_trace) return;
    (void)nvs_set_u32(s_trace, key, (uint32_t)(esp_timer_get_time() / 1000));
    (void)nvs_commit(s_trace);
}

void node_trace_dump(void)
{
    printf("TRACE,prev_hang_mhz=%u\n", s_prev_hang_mhz);
    nvs_iterator_t it = NULL;
    esp_err_t res = nvs_entry_find(NVS_DEFAULT_PART_NAME, "trace", NVS_TYPE_ANY, &it);
    while (res == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        uint32_t u = 0;
        int32_t i = 0;
        uint16_t h = 0;
        if (nvs_get_u32(s_trace, info.key, &u) == ESP_OK)
            printf("TRACE,%s=%lu\n", info.key, (unsigned long)u);
        else if (nvs_get_i32(s_trace, info.key, &i) == ESP_OK)
            printf("TRACE,%s=%ld\n", info.key, (long)i);
        else if (nvs_get_u16(s_trace, info.key, &h) == ESP_OK)
            printf("TRACE,%s=%u\n", info.key, h);
        res = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
}

/* Retune guard: a flag in flash survives a hang, so the next boot knows the
 * previous retune never returned and does not repeat it automatically. */
static esp_err_t guarded_tune(uint16_t mhz)
{
    if (s_trace) {
        (void)nvs_set_u16(s_trace, "tuning", mhz);
        (void)nvs_commit(s_trace);
    }
    esp_err_t err = rf_frontend_tune(mhz);
    if (s_trace) {
        (void)nvs_erase_key(s_trace, "tuning");
        (void)nvs_commit(s_trace);
    }
    return err;
}

static void stage(const char *name)
{
    s_stage = name;
    ESP_LOGI(TAG, "boot: %s", name);
}

void node_diag(node_diag_t *out)
{
    out->stage = (const char *)s_stage;
    out->measure_ok = s_measure_ok;
    out->measure_err = s_measure_err;
    out->last_err = s_last_err;
}

static portMUX_TYPE s_latest_mux = portMUX_INITIALIZER_UNLOCKED;
static iq_power_t s_latest;
static uint32_t s_count;

static void settings_load(uint16_t *mhz, uint8_t *gain)
{
    *mhz = DEFAULT_MHZ;
    *gain = DEFAULT_GAIN;
    nvs_handle_t h;
    if (nvs_open("rfnode", NVS_READONLY, &h) != ESP_OK) return;
    uint16_t m;
    uint8_t g;
    if (nvs_get_u16(h, "mhz", &m) == ESP_OK && rf_frontend_can_tune(m)) *mhz = m;
    if (nvs_get_u8(h, "gain", &g) == ESP_OK && g <= C5LINK_MAX_GAIN) *gain = g;
    nvs_close(h);
}

static void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("rfnode", NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_u16(h, "mhz", rf_frontend_mhz());
    (void)nvs_set_u8(h, "gain", rf_frontend_gain());
    (void)nvs_commit(h);
    nvs_close(h);
}

void node_status(c5link_msg_t *status)
{
    *status = (c5link_msg_t){
        .type = C5LINK_MSG_STATUS,
        .mhz = rf_frontend_mhz(),
        .gain = rf_frontend_gain(),
        .state = s_state,
        .version = NODE_FW_VERSION,
    };
}

void node_apply(const c5link_msg_t *cmd, c5link_msg_t *status)
{
    switch (cmd->type) {
    case C5LINK_MSG_FREQ:
        if (!rf_frontend_can_tune(cmd->mhz)) {
            s_state = C5LINK_ST_ERR_FREQ;
            break;
        }
        if (cmd->mhz == rf_frontend_mhz() && s_state == C5LINK_ST_OK) break;
        xSemaphoreTake(s_rf_lock, portMAX_DELAY);
        s_state = C5LINK_ST_TUNING;
        esp_err_t err = guarded_tune(cmd->mhz);
        s_state = err == ESP_OK ? C5LINK_ST_OK : C5LINK_ST_ERR_RF;
        xSemaphoreGive(s_rf_lock);
        if (err == ESP_OK) settings_save();
        else ESP_LOGE(TAG, "tune %u failed: %s", cmd->mhz, esp_err_to_name(err));
        break;
    case C5LINK_MSG_GAIN:
        if (cmd->gain == rf_frontend_gain()) break;
        xSemaphoreTake(s_rf_lock, portMAX_DELAY);
        rf_frontend_set_gain(cmd->gain);
        xSemaphoreGive(s_rf_lock);
        settings_save();
        break;
    default:
        break;
    }
    node_status(status);
}

bool node_latest(iq_power_t *out, uint32_t *count)
{
    taskENTER_CRITICAL(&s_latest_mux);
    *out = s_latest;
    *count = s_count;
    taskEXIT_CRITICAL(&s_latest_mux);
    return *count != 0;
}

static void measure_task(void *arg)
{
    (void)arg;
    uint8_t seq = 0;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
        /* Skip samples while a retune/gain write owns the PHY, like the
         * RX5808 returning 0 during its tune time. */
        if (xSemaphoreTake(s_rf_lock, 0) != pdTRUE) continue;
        iq_power_t p;
        esp_err_t err = iq_power_measure(&p);
        xSemaphoreGive(s_rf_lock);
        if (err != ESP_OK) {
            s_measure_err = s_measure_err + 1;
            s_last_err = err;
            if (s_measure_err == 1) {
                node_trace("t51_meas_err");
                if (s_trace) (void)nvs_set_i32(s_trace, "t52_err_code", err);
            }
            continue;
        }
        s_measure_ok = s_measure_ok + 1;
        if (s_measure_ok == 1) node_trace("t50_meas_ok");
        if (s_measure_ok == 5000) node_trace("t53_meas_5k");
        if (s_state != C5LINK_ST_OK) continue;

        taskENTER_CRITICAL(&s_latest_mux);
        s_latest = p;
        ++s_count;
        taskEXIT_CRITICAL(&s_latest_mux);

        c5link_msg_t m = {.type = C5LINK_MSG_RSSI, .seq = seq++, .rssi = p.rssi};
        link_send(&m);
    }
}

void app_main(void)
{
    s_rf_lock = xSemaphoreCreateMutex();
    configASSERT(s_rf_lock);

    /* Console first: it must answer even if RF bring-up stalls. */
    console_start();
    stage("nvs");
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    if (nvs_open("trace", NVS_READWRITE, &s_trace) == ESP_OK) {
        if (nvs_get_u16(s_trace, "tuning", &s_prev_hang_mhz) == ESP_OK) {
            /* Keep the previous boot's trace for 't', drop only the flag. */
            (void)nvs_erase_key(s_trace, "tuning");
            (void)nvs_commit(s_trace);
            ESP_LOGE(TAG, "previous boot hung retuning to %u MHz", s_prev_hang_mhz);
        } else {
            (void)nvs_erase_all(s_trace);
            node_trace("t02_nvs");
        }
    }

    uint16_t mhz;
    uint8_t gain;
    settings_load(&mhz, &gain);
    /* Bring RF up on the plain Wi-Fi center first; the saved channel may need
     * the experimental phy_set_freq() step, which runs last (boot_tune) so a
     * failure there cannot keep the console and link from starting. */
    stage("rf_frontend_start");
    ESP_ERROR_CHECK(rf_frontend_start(RF_BOOT_MHZ, gain));
    node_trace("t30_rf_ok");

    stage("iq_power_init");
    ESP_ERROR_CHECK(iq_power_init());
    node_trace("t31_iq_ok");

    stage("link_start");
    link_start();
    stage("running");
    node_trace("t40_running");
    xTaskCreate(measure_task, "measure", 4096, NULL, 5, NULL);

    if (s_prev_hang_mhz) {
        /* Stay on the bootstrap center; the AP can still request a retune. */
        s_state = C5LINK_ST_ERR_RF;
    } else if (mhz != rf_frontend_mhz()) {
        stage("boot_tune");
        c5link_msg_t tune = {.type = C5LINK_MSG_FREQ, .mhz = mhz}, status;
        node_apply(&tune, &status);
        stage("running");
    }
    ESP_LOGI(TAG, "RF node v%u running: %u MHz gain %u", NODE_FW_VERSION,
             rf_frontend_mhz(), rf_frontend_gain());
}
