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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "node.h"
#include "rf_frontend.h"

#define DEFAULT_MHZ  5658u /* R1 */
#define DEFAULT_GAIN 40u
#define SAMPLE_PERIOD_MS 1u

static const char *TAG = "node";

/* Serializes PHY retune/gain writes against capture. */
static SemaphoreHandle_t s_rf_lock;
static volatile c5link_state_t s_state = C5LINK_ST_OK;

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
        esp_err_t err = rf_frontend_tune(cmd->mhz);
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
        if (err != ESP_OK || s_state != C5LINK_ST_OK) continue;

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

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    uint16_t mhz;
    uint8_t gain;
    settings_load(&mhz, &gain);
    ESP_ERROR_CHECK(rf_frontend_start(mhz, gain));
    if (rf_frontend_mhz() != mhz) s_state = C5LINK_ST_ERR_RF;

    ESP_ERROR_CHECK(iq_power_init());

    link_start();
    console_start();
    xTaskCreate(measure_task, "measure", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "RF node v%u running: %u MHz gain %u", NODE_FW_VERSION,
             rf_frontend_mhz(), rf_frontend_gain());
}
