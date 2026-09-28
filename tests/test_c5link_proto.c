/* Host test for common/c5link_proto.h.
 * Build: cc -std=c99 -Wall -Wextra -Werror -I common tests/test_c5link_proto.c -o /tmp/t && /tmp/t */
#include "c5link_proto.h"

#include <assert.h>
#include <stdio.h>

static int roundtrip(const c5link_msg_t *in, c5link_msg_t *out)
{
    char buf[C5LINK_MAX_LINE + 4];
    size_t n = c5link_format(buf, sizeof(buf), in);
    assert(n > 0 && buf[n - 1] == '\n');
    c5link_rx_t rx;
    c5link_rx_reset(&rx);
    int got = 0;
    for (size_t i = 0; i < n; ++i)
        if (c5link_rx_feed(&rx, buf[i], out)) ++got;
    return got;
}

int main(void)
{
    c5link_msg_t out;

    c5link_msg_t r = {.type = C5LINK_MSG_RSSI, .seq = 255, .rssi = 0};
    assert(roundtrip(&r, &out) == 1);
    assert(out.type == C5LINK_MSG_RSSI && out.seq == 255 && out.rssi == 0);

    c5link_msg_t s = {.type = C5LINK_MSG_STATUS, .mhz = 5732, .gain = 40,
                      .state = C5LINK_ST_ERR_FREQ, .version = 3};
    assert(roundtrip(&s, &out) == 1);
    assert(out.type == C5LINK_MSG_STATUS && out.mhz == 5732 && out.gain == 40 &&
           out.state == C5LINK_ST_ERR_FREQ && out.version == 3);

    c5link_msg_t f = {.type = C5LINK_MSG_FREQ, .mhz = 5880};
    assert(roundtrip(&f, &out) == 1 && out.type == C5LINK_MSG_FREQ && out.mhz == 5880);

    c5link_msg_t g = {.type = C5LINK_MSG_GAIN, .gain = 89};
    assert(roundtrip(&g, &out) == 1 && out.type == C5LINK_MSG_GAIN && out.gain == 89);

    c5link_msg_t q = {.type = C5LINK_MSG_QUERY};
    assert(roundtrip(&q, &out) == 1 && out.type == C5LINK_MSG_QUERY);

    /* Known wire format. */
    char buf[64];
    size_t n = c5link_format(buf, sizeof(buf), &f);
    assert(n == 10 && memcmp(buf, "F,5880*", 7) == 0);

    /* Examples documented in docs/protocol.md. */
    const char *doc[] = {"R,17,142*63", "S,5732,40,OK,1*61", "F,5843*60"};
    for (size_t i = 0; i < 3; ++i) assert(c5link_parse(doc[i], strlen(doc[i]), &out));

    /* Rejections. */
    const char *bad[] = {
        "F,5880*00",      /* bad checksum */
        "F,5880",         /* no checksum */
        "G,90*",          /* truncated */
        "R,1,256*00",     /* out of range */
        "X*58",           /* unknown type */
        "",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        assert(!c5link_parse(bad[i], strlen(bad[i]), &out));

    /* Gain > max rejected even with a valid checksum. */
    strcpy(buf, "G,90");
    n = c5link_seal(buf, sizeof(buf), 4);
    assert(!c5link_parse(buf, n - 1, &out));

    /* Trailing CR tolerated. */
    n = c5link_format(buf, sizeof(buf), &q);
    buf[n - 1] = '\r';
    assert(c5link_parse(buf, n, &out) && out.type == C5LINK_MSG_QUERY);

    /* Garbage then a valid line: the assembler resynchronises on '\n'. */
    c5link_rx_t rx;
    c5link_rx_reset(&rx);
    const char *noise = "#$%^&*()_+ this is a very long garbage line that overflows the buffer\n";
    for (const char *p = noise; *p; ++p) assert(!c5link_rx_feed(&rx, *p, &out));
    n = c5link_format(buf, sizeof(buf), &r);
    int got = 0;
    for (size_t i = 0; i < n; ++i) got += c5link_rx_feed(&rx, buf[i], &out);
    assert(got == 1 && out.type == C5LINK_MSG_RSSI);

    puts("c5link_proto: all tests passed");
    return 0;
}
