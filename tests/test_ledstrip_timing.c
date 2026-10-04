/* Host test for ap-node/lib/LEDSTRIP/ledstrip_timing.h: every chip's RMT bit
 * timing must sit inside its datasheet window (nominal +-150 ns).
 * Build: cc -std=c99 -Wall -Wextra -Werror -I ap-node/lib/LEDSTRIP tests/test_ledstrip_timing.c -o /tmp/t && /tmp/t */
#include "ledstrip_timing.h"

#include <assert.h>
#include <stdio.h>

typedef struct {
    const char *name;
    int t0h, t0l, t1h, t1l;  // datasheet nominal, ns
} datasheet_t;

static const datasheet_t SHEETS[LEDSTRIP_CHIP_COUNT] = {
    {"WS2812B", 400, 850, 800, 450},
    {"WS2812", 350, 800, 700, 600},
    {"WS2811 800k", 250, 1000, 600, 650},
    {"WS2811 400k", 500, 2000, 1200, 1300},
};

static int within(int ticks, int nominalNs)
{
    int ns = ticks * (int)LEDSTRIP_TICK_NS;
    return ns >= nominalNs - 150 && ns <= nominalNs + 150;
}

int main(void)
{
    for (unsigned c = 0; c < LEDSTRIP_CHIP_COUNT; ++c) {
        const ledstrip_timing_t *t = ledstrip_timing(c);
        const datasheet_t *d = &SHEETS[c];
        int ok = within(t->t0h, d->t0h) && within(t->t0l, d->t0l) && within(t->t1h, d->t1h) && within(t->t1l, d->t1l);
        if (!ok) printf("%s timing out of spec: %u/%u %u/%u ticks\n", d->name, t->t0h, t->t0l, t->t1h, t->t1l);
        assert(ok);
        /* A one must stay high clearly longer than a zero. */
        assert(t->t1h >= t->t0h + 3);
    }
    assert(ledstrip_timing(LEDSTRIP_CHIP_COUNT) == ledstrip_timing(LEDSTRIP_WS2812B));
    assert(ledstrip_order_index(99) == LEDSTRIP_GRB);
    printf("test_ledstrip_timing: OK\n");
    return 0;
}
