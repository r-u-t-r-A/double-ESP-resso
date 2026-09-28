/*
 * console.c - USB-Serial/JTAG bring-up console for the RF node.
 *
 * Lets the RF node be tested and calibrated without the AP node. It never
 * participates in measurement pacing: it only reads the latest sample.
 *
 * Commands (one per line):
 *   f <mhz>   tune          g <idx>   fixed RX gain
 *   s         status        r         toggle 100 Hz CSV stream
 * Stream format: RSSI,<ms>,<rssi>,<mean_power>,<clip_permille>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag_vfs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"

#include "node.h"

static void print_status(const c5link_msg_t *s)
{
    printf("STATUS,%u,%u,%s,%u\n", s->mhz, s->gain, c5link_state_name(s->state),
           s->version);
}

static void handle_line(char *line, bool *stream)
{
    c5link_msg_t cmd = {0}, status;
    char *arg = line + 1;
    while (*arg == ' ' || *arg == ',') ++arg;
    switch (line[0]) {
    case 'f':
    case 'F':
        cmd.type = C5LINK_MSG_FREQ;
        cmd.mhz = (uint16_t)strtoul(arg, NULL, 10);
        break;
    case 'g':
    case 'G': {
        unsigned long g = strtoul(arg, NULL, 10);
        if (g > C5LINK_MAX_GAIN) {
            printf("ERR gain 0..%u\n", C5LINK_MAX_GAIN);
            return;
        }
        cmd.type = C5LINK_MSG_GAIN;
        cmd.gain = (uint8_t)g;
        break;
    }
    case 's':
    case 'S':
        cmd.type = C5LINK_MSG_QUERY;
        break;
    case 'r':
    case 'R':
        *stream = !*stream;
        printf("STREAM,%s\n", *stream ? "on" : "off");
        return;
    default:
        printf("commands: f <mhz> | g <idx> | s | r\n");
        return;
    }
    node_apply(&cmd, &status);
    print_status(&status);
}

static void console_task(void *arg)
{
    (void)arg;
    char line[32];
    size_t len = 0;
    bool stream = false;
    uint32_t last_count = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10));
        /* IDF 6.0's non-blocking VFS read needs the driver; read the FIFO
         * directly like C5VRX does. This task is the sole reader. */
        for (unsigned k = 0; k < 64; ++k) {
            uint8_t c;
            if (usb_serial_jtag_ll_read_rxfifo(&c, 1) == 0) break;
            if (c == '\r' || c == '\n') {
                line[len] = '\0';
                if (len) handle_line(line, &stream);
                len = 0;
            } else if (len + 1 < sizeof(line)) {
                line[len++] = (char)c;
            }
        }
        if (stream) {
            iq_power_t p;
            uint32_t count;
            if (node_latest(&p, &count) && count != last_count) {
                last_count = count;
                printf("RSSI,%lld,%u,%.2f,%u\n", esp_timer_get_time() / 1000,
                       p.rssi, (double)p.mean_power, p.clip_permille);
            }
        }
    }
}

void console_start(void)
{
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    int flags = fcntl(fileno(stdout), F_GETFL, 0);
    fcntl(fileno(stdout), F_SETFL, flags | O_NONBLOCK);
    usb_serial_jtag_vfs_use_nonblocking();
    xTaskCreate(console_task, "console", 4096, NULL, 1, NULL);
}
