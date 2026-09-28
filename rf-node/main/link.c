/*
 * link.c - UART link from the RF node to the AP node.
 *
 * Stacked XIAO boards connect pin-to-pin, so the roles cross in firmware:
 * this node transmits on D6 (GPIO11) and receives on D7 (GPIO12).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "node.h"

#define LINK_UART      UART_NUM_1
#define LINK_TX_GPIO   11 /* XIAO D6 */
#define LINK_RX_GPIO   12 /* XIAO D7 */
#define LINK_TX_BUF    2048
#define LINK_RX_BUF    512
#define HEARTBEAT_US   1000000

static const char *TAG = "link";

void link_send(const c5link_msg_t *msg)
{
    char buf[C5LINK_MAX_LINE + 4];
    size_t n = c5link_format(buf, sizeof(buf), msg);
    /* The driver's TX ring buffer makes this non-blocking unless the AP side
     * stalls for longer than LINK_TX_BUF bytes; then samples are dropped
     * rather than pacing the measurement task. */
    size_t space = 0;
    if (n && uart_get_tx_buffer_free_size(LINK_UART, &space) == ESP_OK && space >= n)
        uart_write_bytes(LINK_UART, buf, n);
}

static void link_rx_task(void *arg)
{
    (void)arg;
    c5link_rx_t rx;
    c5link_rx_reset(&rx);
    uint8_t chunk[64];
    c5link_msg_t status;
    node_status(&status);
    link_send(&status); /* announce boot */
    int64_t next_heartbeat = esp_timer_get_time() + HEARTBEAT_US;

    for (;;) {
        int n = uart_read_bytes(LINK_UART, chunk, sizeof(chunk), pdMS_TO_TICKS(50));
        for (int i = 0; i < n; ++i) {
            c5link_msg_t cmd;
            if (!c5link_rx_feed(&rx, (char)chunk[i], &cmd)) continue;
            if (cmd.type != C5LINK_MSG_FREQ && cmd.type != C5LINK_MSG_GAIN &&
                cmd.type != C5LINK_MSG_QUERY)
                continue;
            node_apply(&cmd, &status);
            link_send(&status);
            next_heartbeat = esp_timer_get_time() + HEARTBEAT_US;
        }
        if (esp_timer_get_time() >= next_heartbeat) {
            node_status(&status);
            link_send(&status);
            next_heartbeat += HEARTBEAT_US;
        }
    }
}

void link_start(void)
{
    const uart_config_t cfg = {
        .baud_rate = C5LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART, LINK_RX_BUF, LINK_TX_BUF, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART, LINK_TX_GPIO, LINK_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    xTaskCreate(link_rx_task, "link_rx", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "UART%d %d baud TX=GPIO%d RX=GPIO%d", LINK_UART, C5LINK_BAUD,
             LINK_TX_GPIO, LINK_RX_GPIO);
}
