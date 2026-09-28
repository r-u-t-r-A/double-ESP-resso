/*
 * c5link_proto.h - line protocol between the double-ESP-resso RF node and
 * AP node. Header-only, plain C99, no allocation: shared by the ESP-IDF RF
 * node, the Arduino AP node and the host unit test.
 *
 * Every line is ASCII:  <payload>*<HH>\n
 * where HH is the upper-case hex XOR of all payload bytes (NMEA style).
 *
 *   RF -> AP  R,<seq 0-255>,<rssi 0-255>                    1 kHz RSSI sample
 *   RF -> AP  S,<mhz>,<gain>,<state>,<version>              status / ack / 1 s heartbeat
 *   AP -> RF  F,<mhz>                                       tune
 *   AP -> RF  G,<gain>                                      fixed RX gain index
 *   AP -> RF  Q                                             query status
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define C5LINK_BAUD        921600
#define C5LINK_MAX_LINE    48u
#define C5LINK_MAX_GAIN    89u
#define C5LINK_MIN_MHZ     5180u
#define C5LINK_MAX_MHZ     5885u

typedef enum {
    C5LINK_ST_OK = 0,
    C5LINK_ST_TUNING,
    C5LINK_ST_ERR_FREQ,
    C5LINK_ST_ERR_RF,
    C5LINK_ST_COUNT,
} c5link_state_t;

typedef enum {
    C5LINK_MSG_NONE = 0,
    C5LINK_MSG_RSSI,
    C5LINK_MSG_STATUS,
    C5LINK_MSG_FREQ,
    C5LINK_MSG_GAIN,
    C5LINK_MSG_QUERY,
} c5link_msg_type_t;

typedef struct {
    c5link_msg_type_t type;
    uint8_t seq;
    uint8_t rssi;
    uint16_t mhz;
    uint8_t gain;
    c5link_state_t state;
    uint16_t version;
} c5link_msg_t;

static inline const char *c5link_state_name(c5link_state_t s)
{
    static const char *const names[C5LINK_ST_COUNT] = {
        "OK", "TUNING", "ERR_FREQ", "ERR_RF",
    };
    return (unsigned)s < C5LINK_ST_COUNT ? names[s] : "?";
}

static inline uint8_t c5link_xor(const char *s, size_t n)
{
    uint8_t x = 0;
    for (size_t i = 0; i < n; ++i) x ^= (uint8_t)s[i];
    return x;
}

/* Appends "*HH\n" to payload[0..len) in buf. Returns total length or 0. */
static inline size_t c5link_seal(char *buf, size_t cap, size_t len)
{
    static const char hex[] = "0123456789ABCDEF";
    if (len + 4u > cap) return 0;
    uint8_t x = c5link_xor(buf, len);
    buf[len++] = '*';
    buf[len++] = hex[x >> 4];
    buf[len++] = hex[x & 0xFu];
    buf[len++] = '\n';
    if (len < cap) buf[len] = '\0';
    return len;
}

static inline size_t c5link_format(char *buf, size_t cap, const c5link_msg_t *m)
{
    int n;
    switch (m->type) {
    case C5LINK_MSG_RSSI:
        n = snprintf(buf, cap, "R,%u,%u", (unsigned)m->seq, (unsigned)m->rssi);
        break;
    case C5LINK_MSG_STATUS:
        n = snprintf(buf, cap, "S,%u,%u,%s,%u", (unsigned)m->mhz,
                     (unsigned)m->gain, c5link_state_name(m->state),
                     (unsigned)m->version);
        break;
    case C5LINK_MSG_FREQ:
        n = snprintf(buf, cap, "F,%u", (unsigned)m->mhz);
        break;
    case C5LINK_MSG_GAIN:
        n = snprintf(buf, cap, "G,%u", (unsigned)m->gain);
        break;
    case C5LINK_MSG_QUERY:
        n = snprintf(buf, cap, "Q");
        break;
    default:
        return 0;
    }
    if (n <= 0 || (size_t)n >= cap) return 0;
    return c5link_seal(buf, cap, (size_t)n);
}

/* Parses an unsigned decimal field; advances *p past it and one ',' if any. */
static inline bool c5link_field_u(const char **p, const char *end,
                                  unsigned max, unsigned *out)
{
    const char *s = *p;
    unsigned v = 0;
    if (s >= end || *s < '0' || *s > '9') return false;
    while (s < end && *s >= '0' && *s <= '9') {
        v = v * 10u + (unsigned)(*s - '0');
        if (v > max) return false;
        ++s;
    }
    if (s < end) {
        if (*s != ',') return false;
        ++s;
    }
    *out = v;
    *p = s;
    return true;
}

static inline int c5link_hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Parses one line without its trailing '\n' (a trailing '\r' is tolerated).
 * Returns false for any malformed or checksum-failing line. */
static inline bool c5link_parse(const char *line, size_t len, c5link_msg_t *m)
{
    if (len && line[len - 1] == '\r') --len;
    if (len < 4u || len > C5LINK_MAX_LINE || line[len - 3] != '*') return false;
    int hi = c5link_hexval(line[len - 2]);
    int lo = c5link_hexval(line[len - 1]);
    size_t plen = len - 3u;
    if (hi < 0 || lo < 0 || c5link_xor(line, plen) != (uint8_t)(hi << 4 | lo))
        return false;

    const char *p = line + 1;
    const char *end = line + plen;
    unsigned a, b;
    memset(m, 0, sizeof(*m));

    switch (line[0]) {
    case 'Q':
        m->type = C5LINK_MSG_QUERY;
        return plen == 1u;
    case 'R':
        if (p >= end || *p++ != ',') return false;
        if (!c5link_field_u(&p, end, 255u, &a)) return false;
        if (!c5link_field_u(&p, end, 255u, &b) || p != end) return false;
        m->type = C5LINK_MSG_RSSI;
        m->seq = (uint8_t)a;
        m->rssi = (uint8_t)b;
        return true;
    case 'F':
        if (p >= end || *p++ != ',') return false;
        if (!c5link_field_u(&p, end, 9999u, &a) || p != end) return false;
        m->type = C5LINK_MSG_FREQ;
        m->mhz = (uint16_t)a;
        return true;
    case 'G':
        if (p >= end || *p++ != ',') return false;
        if (!c5link_field_u(&p, end, C5LINK_MAX_GAIN, &a) || p != end) return false;
        m->type = C5LINK_MSG_GAIN;
        m->gain = (uint8_t)a;
        return true;
    case 'S': {
        if (p >= end || *p++ != ',') return false;
        if (!c5link_field_u(&p, end, 9999u, &a)) return false;
        if (!c5link_field_u(&p, end, 255u, &b)) return false;
        const char *comma = (const char *)memchr(p, ',', (size_t)(end - p));
        if (!comma) return false;
        size_t slen = (size_t)(comma - p);
        int st = -1;
        for (int i = 0; i < (int)C5LINK_ST_COUNT; ++i) {
            const char *n = c5link_state_name((c5link_state_t)i);
            if (strlen(n) == slen && memcmp(n, p, slen) == 0) { st = i; break; }
        }
        if (st < 0) return false;
        p = comma + 1;
        unsigned ver;
        if (!c5link_field_u(&p, end, 65535u, &ver) || p != end) return false;
        m->type = C5LINK_MSG_STATUS;
        m->mhz = (uint16_t)a;
        m->gain = (uint8_t)b;
        m->state = (c5link_state_t)st;
        m->version = (uint16_t)ver;
        return true;
    }
    default:
        return false;
    }
}

/* Incremental line assembler for a byte stream. */
typedef struct {
    char buf[C5LINK_MAX_LINE + 2u];
    size_t len;
    bool overflow;
} c5link_rx_t;

static inline void c5link_rx_reset(c5link_rx_t *rx)
{
    rx->len = 0;
    rx->overflow = false;
}

/* Feeds one byte. Returns true and fills *m when a valid line completed. */
static inline bool c5link_rx_feed(c5link_rx_t *rx, char c, c5link_msg_t *m)
{
    if (c == '\n') {
        bool ok = !rx->overflow && c5link_parse(rx->buf, rx->len, m);
        c5link_rx_reset(rx);
        return ok;
    }
    if (rx->len >= sizeof(rx->buf)) {
        rx->overflow = true;
        return false;
    }
    rx->buf[rx->len++] = c;
    return false;
}

#ifdef __cplusplus
}
#endif
