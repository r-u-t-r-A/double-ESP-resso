/* Host test for ap-node/lib/RSSISTREAM/rssi_stream.h: batch text format,
 * batch size cap, overflow skipping and index wraparound.
 * Build: cc -std=c99 -Wall -Wextra -Werror -I ap-node/lib/RSSISTREAM tests/test_rssi_stream.c -o /tmp/t && /tmp/t */
#include "rssi_stream.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static rssi_stream_t s;
static char out[RSSI_STREAM_TEXT_MAX];

int main(void)
{
    /* Empty ring: nothing to send. */
    assert(rssi_stream_encode(&s, out) == 0);

    /* Format, including a clamped 12 ms gap. */
    rssi_stream_push(&s, 1000, 0x00, 0x10);
    rssi_stream_push(&s, 1001, 0xAB, 0x11);
    rssi_stream_push(&s, 1013, 0xFF, 0x12);
    assert(rssi_stream_encode(&s, out) == 3);
    assert(strcmp(out, "0,1000,00ABFF,101112,019") == 0);
    assert(rssi_stream_pending(&s) == 0);

    /* Batches are capped and continue where the last one ended. */
    for (unsigned i = 0; i < 150; ++i) rssi_stream_push(&s, 2000 + i, (uint8_t)i, (uint8_t)i);
    assert(rssi_stream_encode(&s, out) == RSSI_STREAM_BATCH_MAX);
    assert(strncmp(out, "3,2000,", 7) == 0);
    assert(strlen(out) == 7 + 5 * RSSI_STREAM_BATCH_MAX + 2);
    assert(rssi_stream_encode(&s, out) == 50);
    assert(strncmp(out, "103,2100,", 9) == 0);

    /* A stalled consumer loses the oldest samples, never reads torn slots. */
    for (unsigned i = 0; i < 2000; ++i) rssi_stream_push(&s, 5000 + i, 1, 2);
    assert(rssi_stream_encode(&s, out) == RSSI_STREAM_BATCH_MAX);
    unsigned idx0 = 0, ms0 = 0;
    assert(sscanf(out, "%u,%u,", &idx0, &ms0) == 2);
    assert(idx0 == 153 + 2000 - (RSSI_STREAM_LEN - RSSI_STREAM_MARGIN));
    assert(ms0 == 5000 + (idx0 - 153));
    rssi_stream_flush(&s);
    assert(rssi_stream_pending(&s) == 0);

    /* Index wraparound and the longest numbers fit the buffer. */
    s.head = s.tail = 0xFFFFFFF0u;
    for (unsigned i = 0; i < 120; ++i) rssi_stream_push(&s, 0xFFFFFFFFu, 0xFF, 0xFF);
    assert(rssi_stream_encode(&s, out) == RSSI_STREAM_BATCH_MAX);
    assert(strncmp(out, "4294967280,4294967295,", 22) == 0);
    assert(strlen(out) + 1 <= RSSI_STREAM_TEXT_MAX);
    assert(rssi_stream_encode(&s, out) == 20);
    assert(strncmp(out, "84,", 3) == 0);

    printf("test_rssi_stream: OK\n");
    return 0;
}
