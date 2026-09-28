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

/* UART link to the AP node. */
void link_start(void);
void link_send(const c5link_msg_t *msg);

/* USB-Serial/JTAG bring-up console. */
void console_start(void);
