/*
 * node.h - RF node state shared by the UART link and the USB console.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "c5link_proto.h"
#include "iq_power.h"

#define NODE_FW_VERSION 1u

/* Applies an AP->RF command (F, G or Q). Blocks while retuning.
 * Always leaves the resulting status in *status. */
void node_apply(const c5link_msg_t *cmd, c5link_msg_t *status);
void node_status(c5link_msg_t *status);

/* Latest measurement, for the USB console. Returns false if none yet. */
bool node_latest(iq_power_t *out, uint32_t *count);

/* Bring-up diagnostics, reported by the USB console 'd' command. */
typedef struct {
    const char *stage;       /* last boot step reached */
    uint32_t measure_ok;
    uint32_t measure_err;
    int last_err;            /* esp_err_t of the last failed capture */
} node_diag_t;
void node_diag(node_diag_t *out);

/* Bring-up trace: records that a boot step was reached as an NVS key in
 * namespace "trace" (readable from download mode with nvs_tool.py), so a
 * hang is visible even when USB output is lost. */
void node_trace(const char *key);
/* Prints the trace namespace (console 't' command). */
void node_trace_dump(void);

/* UART link to the AP node. */
void link_start(void);
void link_send(const c5link_msg_t *msg);

/* USB-Serial/JTAG bring-up console. */
void console_start(void);
