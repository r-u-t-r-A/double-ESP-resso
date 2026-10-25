/* Host test for ap-node/lib/SDLOG/sdlog_row.h.
 * Build: cc -std=c99 -Wall -Wextra -Werror -I ap-node/lib/SDLOG tests/test_sdlog_row.c */
#include "sdlog_row.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static sdlog_ring_t ring;

static void test_row(void)
{
    assert(strlen(SDLOG_HEADER) == SDLOG_ROW_LEN);

    char row[SDLOG_ROW_LEN + 1] = {0};
    sdlog_sample_t s = {.ms = 1052310, .raw = 142, .filt = 138, .event = SDLOG_EV_NONE, .lapMs = 77};
    sdlog_format_row(row, &s);
    assert(memcmp(row, "0001052310,142,138,-,0000000\n", SDLOG_ROW_LEN) == 0);  // lap_ms only on laps

    s = (sdlog_sample_t){.ms = 4294967295u, .raw = 255, .filt = 0, .event = SDLOG_EV_LAP, .lapMs = 12345};
    sdlog_format_row(row, &s);
    assert(memcmp(row, "4294967295,255,000,L,0012345\n", SDLOG_ROW_LEN) == 0);

    s.lapMs = 123456789;  // clamped to 7 digits
    sdlog_format_row(row, &s);
    assert(memcmp(row + 21, "9999999\n", 8) == 0);

    s.event = 'q';  // unknown events become '-'
    sdlog_format_row(row, &s);
    assert(row[19] == '-');

    sdlog_sample_t p;
    s = (sdlog_sample_t){.ms = 42, .raw = 7, .filt = 200, .event = SDLOG_EV_START, .lapMs = 0};
    sdlog_format_row(row, &s);
    assert(sdlog_parse_row(row, &p));
    assert(p.ms == 42 && p.raw == 7 && p.filt == 200 && p.event == SDLOG_EV_START && p.lapMs == 0);
    assert(!sdlog_parse_row(SDLOG_HEADER, &p));
    memcpy(row, "0000000042,256,200,S,0000000\n", SDLOG_ROW_LEN);  // raw out of range
    assert(!sdlog_parse_row(row, &p));
}

static void test_time(void)
{
    char t[24];
    sdlog_format_time(t, 0);
    assert(strcmp(t, "1970-01-01 00:00:00.000") == 0);
    sdlog_format_time(t, 1792951331340LL);
    assert(strcmp(t, "2026-10-25 18:02:11.340") == 0);
    sdlog_format_time(t, 951868799999LL);
    assert(strcmp(t, "2000-02-29 23:59:59.999") == 0);
    sdlog_format_time(t, 2147483648000LL);  // past the 32-bit seconds limit
    assert(strcmp(t, "2038-01-19 03:14:08.000") == 0);
}

static void test_export(void)
{
    char row[SDLOG_ROW_LEN + 1] = {0};
    char out[SDLOG_EXPORT_ROW_MAX];
    sdlog_sample_t s = {.ms = 12340, .raw = 142, .filt = 9, .event = SDLOG_EV_LAP, .lapMs = 12340};
    sdlog_format_row(row, &s);
    // Wall clock: row ms + offset, local time.
    size_t n = sdlog_export_row(out, row, 1, 1792951331340LL - 12340);
    out[n] = 0;
    assert(strcmp(out, "2026-10-25 18:02:11.340,12340,142,9,lap,12340\n") == 0);

    s = (sdlog_sample_t){.ms = 5, .raw = 0, .filt = 18, .event = SDLOG_EV_NONE, .lapMs = 0};
    sdlog_format_row(row, &s);
    n = sdlog_export_row(out, row, 0, 0);
    out[n] = 0;
    assert(strcmp(out, ",5,0,18,,\n") == 0);  // no clock: empty time column

    s = (sdlog_sample_t){.ms = 4294967295u, .raw = 255, .filt = 255, .event = SDLOG_EV_START, .lapMs = 0};
    sdlog_format_row(row, &s);
    n = sdlog_export_row(out, row, 1, 0);
    assert(n < SDLOG_EXPORT_ROW_MAX);
    assert(sdlog_export_row(out, SDLOG_HEADER, 1, 0) == 0);
}

static void test_ring(void)
{
    sdlog_sample_t out[SDLOG_RING_LEN];
    sdlog_sample_t s = {0};
    for (uint32_t i = 0; i < 10; ++i) {
        s.ms = i;
        sdlog_push(&ring, &s);
    }
    assert(sdlog_pop(&ring, out, 4) == 4 && out[0].ms == 0 && out[3].ms == 3);
    assert(sdlog_pop(&ring, out, 100) == 6 && out[5].ms == 9);
    assert(sdlog_pop(&ring, out, 100) == 0 && ring.dropped == 0);

    // Overrun: the oldest are dropped, the newest kept in order.
    for (uint32_t i = 0; i < SDLOG_RING_LEN + 500; ++i) {
        s.ms = 1000 + i;
        sdlog_push(&ring, &s);
    }
    unsigned n = sdlog_pop(&ring, out, SDLOG_RING_LEN);
    assert(n == SDLOG_RING_LEN - SDLOG_RING_MARGIN);
    assert(ring.dropped == 500 + SDLOG_RING_MARGIN);
    assert(out[n - 1].ms == 1000 + SDLOG_RING_LEN + 499);
    for (unsigned i = 1; i < n; ++i) assert(out[i].ms == out[i - 1].ms + 1);

    s.ms = 7;
    sdlog_push(&ring, &s);
    sdlog_flush(&ring);
    assert(sdlog_pop(&ring, out, 10) == 0);
}

int main(void)
{
    test_row();
    test_time();
    test_export();
    test_ring();
    puts("test_sdlog_row: OK");
    return 0;
}
