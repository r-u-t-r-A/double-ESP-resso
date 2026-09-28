/* Host test for rf-node/main/rssi_scale.h.
 * Build: cc -std=c99 -Wall -Wextra -Werror -I rf-node/main tests/test_rssi_scale.c -lm */
#include "rssi_scale.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    /* Byte decoding: 0x00 -> I=0,Q=0 -> (1+1). 0x77 -> 7,7 -> 225+225. 0x88 -> -8,-8. */
    assert(rssi_sample_power(0x00) == 2);
    assert(rssi_sample_power(0xFF) == 2);          /* -1,-1 -> (-1)^2*2 */
    assert(rssi_sample_power(0x77) == 450);
    assert(rssi_sample_power(0x88) == 450);        /* (-15)^2*2 */
    assert(rssi_sample_power(0x70) == 225 + 1);    /* I=7, Q=0 */
    assert(rssi_sample_clipped(0x70) && rssi_sample_clipped(0x08));
    assert(!rssi_sample_clipped(0x66) && !rssi_sample_clipped(0x00));

    assert(rssi_from_mean_power(0.0f) == 0);
    assert(rssi_from_mean_power(2.0f) == 0);
    assert(rssi_from_mean_power(450.0f) == 255);
    assert(rssi_from_mean_power(10000.0f) == 255);

    /* Monotonic and roughly 10.8 counts per dB. */
    uint8_t prev = 0;
    for (float p = 2.0f; p <= 450.0f; p *= 1.05f) {
        uint8_t v = rssi_from_mean_power(p);
        assert(v >= prev);
        prev = v;
    }
    int a = rssi_from_mean_power(20.0f), b = rssi_from_mean_power(200.0f);
    assert(b - a >= 106 && b - a <= 111);  /* 10 dB apart */

    puts("rssi_scale: all tests passed");
    return 0;
}
