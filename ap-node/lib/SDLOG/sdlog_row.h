/*
 * sdlog_row.h - 1 kHz RSSI log rows for the SD card. Header-only, plain C99:
 * shared by the AP node and its host test.
 *
 * One producer (the lap timer loop) pushes every sample into a ring; one
 * consumer (the SD writer task) formats them into fixed-width CSV rows:
 *
 *   ms,raw,filtered,event,lap_ms
 *   0001052310,142,138,-,0000000
 *
 * ms is the AP node's millis(), raw the RSSI as received from the RF node,
 * filtered the Kalman-filtered RSSI the lap timer compares with Enter/Exit.
 * event is '-' (none), 'S' (race start), 'X' (race stop) or 'L' (lap, with
 * its time in lap_ms). Every row, and the header, is SDLOG_ROW_LEN bytes, so
 * a row is found by time with a binary search over file offsets.
 *
 * The export (sdlog_export_row) turns a row into a readable CSV line with a
 * wall-clock time column.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SDLOG_HEADER "ms,raw,filtered,event,lap_ms\n"
#define SDLOG_ROW_LEN 29u
#define SDLOG_RING_LEN 2048u  // power of two: 2 s at 1 kHz, rides out SD stalls (usually < 0.25 s)
#define SDLOG_RING_MARGIN 128u  // slots left to the producer while the consumer copies
#define SDLOG_LAP_MAX 9999999u  // 7 digits, 2.7 h

#define SDLOG_EV_NONE '-'
#define SDLOG_EV_START 'S'
#define SDLOG_EV_STOP 'X'
#define SDLOG_EV_LAP 'L'

#define SDLOG_EXPORT_HEADER "time,ms,raw,filtered,event,lap_ms\n"
// "YYYY-MM-DD HH:MM:SS.mmm," + "4294967295," + "255,255," + "start," + "9999999" + "\n" + NUL
#define SDLOG_EXPORT_ROW_MAX 64u

typedef struct {
    uint32_t ms;
    uint32_t lapMs;
    uint8_t raw;
    uint8_t filt;
    char event;
} sdlog_sample_t;

typedef struct {
    sdlog_sample_t buf[SDLOG_RING_LEN];
    uint32_t head;     // samples ever pushed; written by the producer only
    uint32_t tail;     // next sample to write; written by the consumer only
    uint32_t dropped;  // samples overwritten before the consumer got them
} sdlog_ring_t;

static inline void sdlog_push(sdlog_ring_t *r, const sdlog_sample_t *s)
{
    uint32_t h = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
    r->buf[h & (SDLOG_RING_LEN - 1u)] = *s;
    __atomic_store_n(&r->head, h + 1u, __ATOMIC_RELEASE);
}

static inline void sdlog_flush(sdlog_ring_t *r)
{
    r->tail = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
}

/* Copies up to max pending samples to out and returns the count. If the
 * consumer fell behind, the oldest samples are dropped (and counted) so the
 * producer can't overwrite a slot while it is being copied. */
static inline unsigned sdlog_pop(sdlog_ring_t *r, sdlog_sample_t *out, unsigned max)
{
    uint32_t h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    uint32_t pending = h - r->tail;
    if (pending > SDLOG_RING_LEN - SDLOG_RING_MARGIN) {
        uint32_t keep = SDLOG_RING_LEN - SDLOG_RING_MARGIN;
        r->dropped += pending - keep;
        r->tail = h - keep;
        pending = keep;
    }
    unsigned n = pending < max ? (unsigned)pending : max;
    for (unsigned i = 0; i < n; ++i) out[i] = r->buf[(r->tail + i) & (SDLOG_RING_LEN - 1u)];
    __atomic_store_n(&r->tail, r->tail + n, __ATOMIC_RELEASE);
    return n;
}

static inline void sdlog_put_digits(char *out, uint32_t v, unsigned width)
{
    for (unsigned i = width; i-- > 0;) {
        out[i] = (char)('0' + v % 10u);
        v /= 10u;
    }
}

/* Writes exactly SDLOG_ROW_LEN bytes (no NUL). */
static inline void sdlog_format_row(char *out, const sdlog_sample_t *s)
{
    uint32_t lap = s->event == SDLOG_EV_LAP ? s->lapMs : 0u;
    if (lap > SDLOG_LAP_MAX) lap = SDLOG_LAP_MAX;
    char ev = s->event;
    if (ev != SDLOG_EV_START && ev != SDLOG_EV_STOP && ev != SDLOG_EV_LAP) ev = SDLOG_EV_NONE;
    sdlog_put_digits(out, s->ms, 10);
    out[10] = ',';
    sdlog_put_digits(out + 11, s->raw, 3);
    out[14] = ',';
    sdlog_put_digits(out + 15, s->filt, 3);
    out[18] = ',';
    out[19] = ev;
    out[20] = ',';
    sdlog_put_digits(out + 21, lap, 7);
    out[28] = '\n';
}

static inline int sdlog_parse_digits(const char *p, unsigned width, uint32_t *v)
{
    uint32_t x = 0;
    for (unsigned i = 0; i < width; ++i) {
        if (p[i] < '0' || p[i] > '9') return 0;
        x = x * 10u + (uint32_t)(p[i] - '0');
    }
    *v = x;
    return 1;
}

/* Parses a row written by sdlog_format_row. Returns 0 if malformed. */
static inline int sdlog_parse_row(const char *row, sdlog_sample_t *s)
{
    uint32_t ms, raw, filt, lap;
    if (row[10] != ',' || row[14] != ',' || row[18] != ',' || row[20] != ',' || row[28] != '\n') return 0;
    if (!sdlog_parse_digits(row, 10, &ms) || !sdlog_parse_digits(row + 11, 3, &raw) ||
        !sdlog_parse_digits(row + 15, 3, &filt) || !sdlog_parse_digits(row + 21, 7, &lap))
        return 0;
    if (raw > 255u || filt > 255u) return 0;
    s->ms = ms;
    s->raw = (uint8_t)raw;
    s->filt = (uint8_t)filt;
    s->event = row[19];
    s->lapMs = lap;
    return 1;
}

/* Local civil date from days since 1970-01-01 (proleptic Gregorian). */
static inline void sdlog_civil(int64_t days, int *y, unsigned *m, unsigned *d)
{
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned doe = (unsigned)(days - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yoe + era * 400) + (*m <= 2);
}

/* "YYYY-MM-DD HH:MM:SS.mmm" (23 chars + NUL) from local ms since the epoch. */
static inline void sdlog_format_time(char *out, int64_t localMs)
{
    int64_t days = localMs >= 0 ? localMs / 86400000 : -((-localMs + 86399999) / 86400000);
    uint32_t msOfDay = (uint32_t)(localMs - days * 86400000);
    int y;
    unsigned mo, d;
    sdlog_civil(days, &y, &mo, &d);
    sdlog_put_digits(out, (uint32_t)(y < 0 ? 0 : y), 4);
    out[4] = '-';
    sdlog_put_digits(out + 5, mo, 2);
    out[7] = '-';
    sdlog_put_digits(out + 8, d, 2);
    out[10] = ' ';
    sdlog_put_digits(out + 11, msOfDay / 3600000u, 2);
    out[13] = ':';
    sdlog_put_digits(out + 14, msOfDay / 60000u % 60u, 2);
    out[16] = ':';
    sdlog_put_digits(out + 17, msOfDay / 1000u % 60u, 2);
    out[19] = '.';
    sdlog_put_digits(out + 20, msOfDay % 1000u, 3);
    out[23] = 0;
}

/* One export line for a stored row; writes at most SDLOG_EXPORT_ROW_MAX bytes
 * and returns the length (no NUL), or 0 if the row is malformed. The time
 * column is local wall-clock time, row ms + clockOffsetMs, and is left empty
 * when hasClock is 0 (a file from an earlier boot). */
static inline size_t sdlog_export_row(char *out, const char *row, int hasClock, int64_t clockOffsetMs)
{
    sdlog_sample_t s;
    if (!sdlog_parse_row(row, &s)) return 0;
    size_t n = 0;
    if (hasClock) {
        sdlog_format_time(out, (int64_t)s.ms + clockOffsetMs);
        n = 23;
    }
    out[n++] = ',';
    // ms, raw and filtered without leading zeros
    char tmp[10];
    uint32_t fields[3] = {s.ms, s.raw, s.filt};
    for (unsigned f = 0; f < 3; ++f) {
        unsigned len = 0;
        uint32_t v = fields[f];
        do {
            tmp[len++] = (char)('0' + v % 10u);
            v /= 10u;
        } while (v);
        while (len) out[n++] = tmp[--len];
        out[n++] = ',';
    }
    const char *ev = s.event == SDLOG_EV_START ? "start"
                     : s.event == SDLOG_EV_STOP ? "stop"
                     : s.event == SDLOG_EV_LAP  ? "lap"
                                                : "";
    while (*ev) out[n++] = *ev++;
    out[n++] = ',';
    if (s.event == SDLOG_EV_LAP) {
        unsigned len = 0;
        uint32_t v = s.lapMs;
        do {
            tmp[len++] = (char)('0' + v % 10u);
            v /= 10u;
        } while (v);
        while (len) out[n++] = tmp[--len];
    }
    out[n++] = '\n';
    return n;
}

#ifdef __cplusplus
}
#endif
