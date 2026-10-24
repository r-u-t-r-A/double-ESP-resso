/*
 * rssi_stream.h - 1 kHz lap-timer sample ring for the desktop app's /rssi SSE
 * stream. Header-only, plain C99: shared by the AP node and its host test.
 *
 * One producer (the lap timer loop) pushes every sample; one consumer (the
 * web task) drains them into text batches:
 *
 *   <idx0>,<ms0>,<raw hex>,<filt hex>,<dms digits>
 *
 * idx0 is the running sample index of the first sample and ms0 its millis().
 * raw/filt are 2 upper-case hex chars per sample. dms is one decimal digit per
 * sample: the ms since the previous sample (0 for the first), clamped to 9.
 * A jump in idx0 beyond the previous batch means samples were lost.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RSSI_STREAM_LEN 512u  // power of two, about 0.5 s at 1 kHz
#define RSSI_STREAM_MARGIN 64u  // slots kept free for the producer while encoding
#define RSSI_STREAM_BATCH_MAX 100u
// "4294967295,4294967295," + 2+1 + 2+1 + 1 chars per sample + NUL
#define RSSI_STREAM_TEXT_MAX (22u + 5u * RSSI_STREAM_BATCH_MAX + 3u)

typedef struct {
    uint32_t ms;
    uint8_t raw;
    uint8_t filt;
} rssi_stream_sample_t;

typedef struct {
    rssi_stream_sample_t buf[RSSI_STREAM_LEN];
    uint32_t head;  // samples ever pushed; written by the producer only
    uint32_t tail;  // next sample to send; written by the consumer only
} rssi_stream_t;

static inline void rssi_stream_push(rssi_stream_t *s, uint32_t ms, uint8_t raw, uint8_t filt)
{
    uint32_t h = __atomic_load_n(&s->head, __ATOMIC_RELAXED);
    rssi_stream_sample_t *slot = &s->buf[h & (RSSI_STREAM_LEN - 1u)];
    slot->ms = ms;
    slot->raw = raw;
    slot->filt = filt;
    __atomic_store_n(&s->head, h + 1u, __ATOMIC_RELEASE);
}

// Skips everything already queued, e.g. when the first client connects.
static inline void rssi_stream_flush(rssi_stream_t *s)
{
    s->tail = __atomic_load_n(&s->head, __ATOMIC_ACQUIRE);
}

static inline uint32_t rssi_stream_pending(const rssi_stream_t *s)
{
    return __atomic_load_n(&s->head, __ATOMIC_ACQUIRE) - s->tail;
}

/* Encodes up to RSSI_STREAM_BATCH_MAX pending samples into out (at least
 * RSSI_STREAM_TEXT_MAX bytes) and returns the count, 0 if none were pending.
 * If the consumer fell behind, the oldest samples are dropped so the
 * producer can't overwrite a slot while it is being read. */
static inline unsigned rssi_stream_encode(rssi_stream_t *s, char *out)
{
    static const char hex[] = "0123456789ABCDEF";
    uint32_t head = __atomic_load_n(&s->head, __ATOMIC_ACQUIRE);
    if (head - s->tail > RSSI_STREAM_LEN - RSSI_STREAM_MARGIN) s->tail = head - (RSSI_STREAM_LEN - RSSI_STREAM_MARGIN);
    uint32_t n = head - s->tail;
    if (n == 0) return 0;
    if (n > RSSI_STREAM_BATCH_MAX) n = RSSI_STREAM_BATCH_MAX;

    const rssi_stream_sample_t *first = &s->buf[s->tail & (RSSI_STREAM_LEN - 1u)];
    char *p = out;
    // Decimal without printf so the header stays freestanding-friendly.
    uint32_t nums[2] = {s->tail, first->ms};
    for (int k = 0; k < 2; ++k) {
        char tmp[10];
        int len = 0;
        uint32_t v = nums[k];
        do {
            tmp[len++] = (char)('0' + v % 10u);
            v /= 10u;
        } while (v);
        while (len) *p++ = tmp[--len];
        *p++ = ',';
    }
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t v = s->buf[(s->tail + i) & (RSSI_STREAM_LEN - 1u)].raw;
        *p++ = hex[v >> 4];
        *p++ = hex[v & 15u];
    }
    *p++ = ',';
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t v = s->buf[(s->tail + i) & (RSSI_STREAM_LEN - 1u)].filt;
        *p++ = hex[v >> 4];
        *p++ = hex[v & 15u];
    }
    *p++ = ',';
    uint32_t prev = first->ms;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t ms = s->buf[(s->tail + i) & (RSSI_STREAM_LEN - 1u)].ms;
        uint32_t d = ms - prev;
        *p++ = (char)('0' + (d > 9u ? 9u : d));
        prev = ms;
    }
    *p = '\0';
    s->tail += n;
    return (unsigned)n;
}

#ifdef __cplusplus
}
#endif
