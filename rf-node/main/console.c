/*
 * console.c - USB-Serial/JTAG bring-up console for the RF node.
 *
 * Lets the RF node be tested and calibrated without the AP node. It never
 * participates in measurement pacing: it only reads the latest sample.
 *
 * Commands (one per line):
 *   f <mhz>   tune          g <idx>   fixed RX gain
 *   s         status        r         toggle 100 Hz CSV stream
 *   d         diagnostics: boot check, capture and gain counters
 *   m         one capture in the pilot band and its mirror band
 *   t         bring-up trace (kept from a boot that hung while retuning)
 * Stream format: RSSI,<ms>,<rssi>,<filt_db>,<band_db>,<clip_permille>,<gain_bad>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "iq_dump.h"
#include "node.h"
#include "rf_frontend.h"

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
    case 'd':
    case 'D': {
        node_diag_t d;
        iq_dump_stats_t ds;
        node_diag(&d);
        iq_dump_stats(&ds);
        printf("DIAG,stage=%s,check=%s,gain=%u,gainmax=%u,measure_ok=%lu,measure_err=%lu,"
               "last_err=%s,gain_reforced=%lu\n",
               d.stage, rf_frontend_check(), rf_frontend_gain(), rf_frontend_gain_max(),
               (unsigned long)d.measure_ok, (unsigned long)d.measure_err,
               d.last_err ? esp_err_to_name(d.last_err) : "none",
               (unsigned long)d.gain_reforced);
        printf("DUMP,stock=%lu,direct=%lu,timeouts=%lu,short=%lu,long=%lu\n",
               (unsigned long)ds.stock, (unsigned long)ds.direct, (unsigned long)ds.timeouts,
               (unsigned long)ds.short_reject, (unsigned long)ds.long_reject);
        return;
    }
    case 'm':
    case 'M': {
        node_diag_t d;
        node_diag(&d);
        if (strcmp(d.stage, "running") != 0) {
            printf("BOOT,%s\n", d.stage);
            return;
        }
        float pilot, mirror;
        esp_err_t err = node_probe(&pilot, &mirror);
        if (err != ESP_OK) printf("ERR,%s\n", esp_err_to_name(err));
        else printf("MIRROR,%u,%.2f,%.2f\n", rf_frontend_mhz(), (double)pilot, (double)mirror);
        return;
    }
    case 't':
    case 'T':
        node_trace_dump();
        return;
    case 'r':
    case 'R':
        *stream = !*stream;
        printf("STREAM,%s\n", *stream ? "on" : "off");
        return;
    default:
        printf("commands: f <mhz> | g <idx> | s | r | d | m | t\n");
        return;
    }
    node_diag_t d;
    node_diag(&d);
    if (strcmp(d.stage, "running") != 0 && cmd.type != C5LINK_MSG_QUERY) {
        printf("BOOT,%s\n", d.stage); /* RF not up yet: don't touch the PHY */
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
    uint32_t loops = 0;
    bool got_rx = false;
    node_trace("t01_console");

    for (;;) {
        uint8_t rx[32];
        /* Driver read doubles as the 10 ms loop tick. */
        int got = usb_serial_jtag_read_bytes(rx, sizeof(rx), pdMS_TO_TICKS(10));
        if (++loops == 500) node_trace("t60_console_5s");
        if (loops == 3000) node_trace("t61_console_30s");
        for (int k = 0; k < got; ++k) {
            uint8_t c = rx[k];
            if (!got_rx) {
                got_rx = true;
                node_trace("t62_console_rx");
            }
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
                printf("RSSI,%lld,%u,%.2f,%.2f,%u,%u\n", esp_timer_get_time() / 1000,
                       p.rssi, (double)p.filt_db, (double)p.band_db, p.clip_permille,
                       p.gain_bad);
            }
        }
    }
}

void console_start(void)
{
    /* Use the real USB-Serial-JTAG driver. The driver-less non-blocking VFS
     * (as in C5VRX) left this board's console deaf and mute under IDF 6.0:
     * no RX drained, no TX, host writes timing out. The driver's TX side
     * drops output after a short timeout when no host is reading, so logging
     * cannot stall other tasks. */
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 256;
    cfg.tx_buffer_size = 1024;
    if (usb_serial_jtag_driver_install(&cfg) == ESP_OK) usb_serial_jtag_vfs_use_driver();
    setvbuf(stdout, NULL, _IONBF, 0);
    xTaskCreate(console_task, "console", 4096, NULL, 1, NULL);
}
