/*
 * elrs_msp.h - decode the ExpressLRS TX backpack's ESP-NOW frames on the
 * AP node. Header-only, plain C99, no allocation: shared by the Arduino AP
 * node and the host unit test.
 *
 * Each ESP-NOW payload from the backpack is one MSPv2 frame:
 *
 *   '$' 'X' type flags func_lo func_hi size_lo size_hi payload[size] crc
 *
 * type is '<' (command) or '>' (reply); crc is crc8_dvb_s2 over flags..payload.
 *
 * The radio's "DVR Rec" AUX switch makes the TX send function 0x0305
 * (MSP_ELRS_BACKPACK_SET_RECORDING_STATE) once per switch change, with payload
 *   state u8 (0/1), delay_s u16 little-endian.
 *
 * The backpack addresses frames to the bind-phrase UID:
 *   md5("-DMY_BINDING_PHRASE=\"<phrase>\"")[0..5] with bit 0 of byte 0 cleared.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ELRS_UID_LEN                      6u
#define ELRS_MSP_HEADER_LEN               8u
#define ELRS_MSP_MAX_PAYLOAD              64u
#define ELRS_MSP_FUNC_SET_RECORDING_STATE 0x0305u
#define ELRS_PRESS_LONG_MS                1000u

typedef struct {
    uint8_t type;
    uint8_t flags;
    uint16_t function;
    uint16_t size;
    const uint8_t *payload;  // points into the parsed buffer
} elrs_msp_frame_t;

static inline uint8_t elrs_crc8_dvb_s2(uint8_t crc, uint8_t b)
{
    crc ^= b;
    for (int i = 0; i < 8; ++i)
        crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0xD5) : (uint8_t)(crc << 1);
    return crc;
}

/* Parse exactly one MSPv2 frame occupying buf[0..len). */
static inline bool elrs_msp_parse(const uint8_t *buf, size_t len, elrs_msp_frame_t *out)
{
    if (len < ELRS_MSP_HEADER_LEN + 1) return false;
    if (buf[0] != '$' || buf[1] != 'X') return false;
    if (buf[2] != '<' && buf[2] != '>') return false;
    uint16_t size = (uint16_t)(buf[6] | (buf[7] << 8));
    if (size > ELRS_MSP_MAX_PAYLOAD) return false;
    if (len != (size_t)ELRS_MSP_HEADER_LEN + size + 1) return false;
    uint8_t crc = 0;
    for (size_t i = 3; i < ELRS_MSP_HEADER_LEN + size; ++i) crc = elrs_crc8_dvb_s2(crc, buf[i]);
    if (crc != buf[ELRS_MSP_HEADER_LEN + size]) return false;
    out->type = buf[2];
    out->flags = buf[3];
    out->function = (uint16_t)(buf[4] | (buf[5] << 8));
    out->size = size;
    out->payload = buf + ELRS_MSP_HEADER_LEN;
    return true;
}

/* Decode a recording-state frame. The delay is optional on the wire. */
static inline bool elrs_msp_recording_state(const elrs_msp_frame_t *f, bool *on, uint16_t *delay_s)
{
    if (f->function != ELRS_MSP_FUNC_SET_RECORDING_STATE || f->size < 1) return false;
    *on = f->payload[0] != 0;
    *delay_s = f->size >= 3 ? (uint16_t)(f->payload[1] | (f->payload[2] << 8)) : 0;
    return true;
}

/* The MAC the backpack sends to: the raw UID with its multicast bit cleared. */
static inline void elrs_uid_to_mac(const uint8_t uid[ELRS_UID_LEN], uint8_t mac[ELRS_UID_LEN])
{
    for (unsigned i = 0; i < ELRS_UID_LEN; ++i) mac[i] = uid[i];
    mac[0] &= (uint8_t)~0x01u;
}

static inline bool elrs_uid_is_set(const uint8_t uid[ELRS_UID_LEN])
{
    for (unsigned i = 0; i < ELRS_UID_LEN; ++i)
        if (uid[i] != 0) return true;
    return false;
}

/*
 * Short/long press of a momentary radio button mapped to the DVR AUX channel.
 * A press is an "on" edge followed by an "off" edge. A long press fires from
 * elrs_press_tick() while the button is still held; a short press fires on
 * release. A lost "off" therefore looks like a long press, never a short one.
 */
typedef enum {
    ELRS_PRESS_NONE = 0,
    ELRS_PRESS_SHORT,
    ELRS_PRESS_LONG,
} elrs_press_e;

typedef struct {
    bool down;
    bool longFired;
    uint32_t downMs;
} elrs_press_t;

static inline void elrs_press_reset(elrs_press_t *p)
{
    p->down = false;
    p->longFired = false;
    p->downMs = 0;
}

static inline elrs_press_e elrs_press_edge(elrs_press_t *p, bool on, uint32_t nowMs)
{
    if (on) {
        // Also restarts a press whose "off" was lost.
        p->down = true;
        p->longFired = false;
        p->downMs = nowMs;
        return ELRS_PRESS_NONE;
    }
    if (!p->down) return ELRS_PRESS_NONE;
    p->down = false;
    if (p->longFired) return ELRS_PRESS_NONE;
    return (uint32_t)(nowMs - p->downMs) >= ELRS_PRESS_LONG_MS ? ELRS_PRESS_LONG : ELRS_PRESS_SHORT;
}

static inline elrs_press_e elrs_press_tick(elrs_press_t *p, uint32_t nowMs)
{
    if (p->down && !p->longFired && (uint32_t)(nowMs - p->downMs) >= ELRS_PRESS_LONG_MS) {
        p->longFired = true;
        return ELRS_PRESS_LONG;
    }
    return ELRS_PRESS_NONE;
}

#ifdef __cplusplus
}
#endif
