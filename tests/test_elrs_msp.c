/* Host test for ap-node/lib/ELRSBACKPACK/elrs_msp.h.
 * Build: cc -std=c99 -Wall -Wextra -Werror -I ap-node/lib/ELRSBACKPACK tests/test_elrs_msp.c -o /tmp/t && /tmp/t */
#include "elrs_msp.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Bytes the ELRS TX backpack sends for DVR Rec on / off with 0 s delay. */
static const uint8_t rec_on[] = {0x24, 0x58, 0x3C, 0x00, 0x05, 0x03, 0x03, 0x00, 0x01, 0x00, 0x00, 0x4A};
static const uint8_t rec_off[] = {0x24, 0x58, 0x3C, 0x00, 0x05, 0x03, 0x03, 0x00, 0x00, 0x00, 0x00, 0xC9};

static void test_frames(void)
{
    elrs_msp_frame_t f;
    bool on = false;
    uint16_t delay = 99;

    assert(elrs_msp_parse(rec_on, sizeof(rec_on), &f));
    assert(f.type == '<' && f.function == ELRS_MSP_FUNC_SET_RECORDING_STATE && f.size == 3);
    assert(elrs_msp_recording_state(&f, &on, &delay));
    assert(on && delay == 0);

    assert(elrs_msp_parse(rec_off, sizeof(rec_off), &f));
    assert(elrs_msp_recording_state(&f, &on, &delay));
    assert(!on && delay == 0);

    uint8_t buf[sizeof(rec_on)];

    memcpy(buf, rec_on, sizeof(buf));
    buf[sizeof(buf) - 1] ^= 0x01;
    assert(!elrs_msp_parse(buf, sizeof(buf), &f));  // bad CRC

    assert(!elrs_msp_parse(rec_on, sizeof(rec_on) - 1, &f));  // truncated

    memcpy(buf, rec_on, sizeof(buf));
    buf[6] = ELRS_MSP_MAX_PAYLOAD + 1;
    assert(!elrs_msp_parse(buf, sizeof(buf), &f));  // oversized length

    memcpy(buf, rec_on, sizeof(buf));
    buf[1] = 'M';
    assert(!elrs_msp_parse(buf, sizeof(buf), &f));  // MSP v1

    /* A valid frame with another function (MSP_SET_VTX_CONFIG 0x59) is not a recording state. */
    uint8_t vtx[] = {0x24, 0x58, 0x3C, 0x00, 0x59, 0x00, 0x01, 0x00, 0x05, 0x00};
    uint8_t crc = 0;
    for (size_t i = 3; i < sizeof(vtx) - 1; ++i) crc = elrs_crc8_dvb_s2(crc, vtx[i]);
    vtx[sizeof(vtx) - 1] = crc;
    assert(elrs_msp_parse(vtx, sizeof(vtx), &f));
    assert(f.function == 0x59);
    assert(!elrs_msp_recording_state(&f, &on, &delay));

    /* Non-zero delay is decoded little-endian. */
    uint8_t dly[] = {0x24, 0x58, 0x3C, 0x00, 0x05, 0x03, 0x03, 0x00, 0x01, 0x78, 0x00, 0x00};
    crc = 0;
    for (size_t i = 3; i < sizeof(dly) - 1; ++i) crc = elrs_crc8_dvb_s2(crc, dly[i]);
    dly[sizeof(dly) - 1] = crc;
    assert(elrs_msp_parse(dly, sizeof(dly), &f));
    assert(elrs_msp_recording_state(&f, &on, &delay));
    assert(on && delay == 120);
}

static void test_uid(void)
{
    /* md5('-DMY_BINDING_PHRASE="test"')[0..5] */
    const uint8_t uid[ELRS_UID_LEN] = {0x4f, 0x04, 0xfd, 0x82, 0x21, 0x55};
    const uint8_t zero[ELRS_UID_LEN] = {0};
    uint8_t mac[ELRS_UID_LEN];
    elrs_uid_to_mac(uid, mac);
    assert(mac[0] == 0x4e && memcmp(mac + 1, uid + 1, ELRS_UID_LEN - 1) == 0);
    assert(elrs_uid_is_set(uid));
    assert(!elrs_uid_is_set(zero));
}

static void test_press(void)
{
    elrs_press_t p;
    elrs_press_reset(&p);

    /* Short press: fires on release. */
    assert(elrs_press_edge(&p, true, 1000) == ELRS_PRESS_NONE);
    assert(elrs_press_tick(&p, 1500) == ELRS_PRESS_NONE);
    assert(elrs_press_edge(&p, false, 1300) == ELRS_PRESS_SHORT);
    assert(elrs_press_tick(&p, 5000) == ELRS_PRESS_NONE);

    /* Long press: fires once while held, release is silent. */
    assert(elrs_press_edge(&p, true, 10000) == ELRS_PRESS_NONE);
    assert(elrs_press_tick(&p, 10999) == ELRS_PRESS_NONE);
    assert(elrs_press_tick(&p, 11000) == ELRS_PRESS_LONG);
    assert(elrs_press_tick(&p, 11500) == ELRS_PRESS_NONE);
    assert(elrs_press_edge(&p, false, 12000) == ELRS_PRESS_NONE);

    /* Release arriving before the tick noticed a long hold is still long. */
    assert(elrs_press_edge(&p, true, 20000) == ELRS_PRESS_NONE);
    assert(elrs_press_edge(&p, false, 21200) == ELRS_PRESS_LONG);

    /* Orphan "off" is ignored. */
    assert(elrs_press_edge(&p, false, 30000) == ELRS_PRESS_NONE);

    /* Lost "off": the hold turns into a long press, and the next press works. */
    assert(elrs_press_edge(&p, true, 40000) == ELRS_PRESS_NONE);
    assert(elrs_press_tick(&p, 41000) == ELRS_PRESS_LONG);
    assert(elrs_press_edge(&p, true, 50000) == ELRS_PRESS_NONE);
    assert(elrs_press_edge(&p, false, 50200) == ELRS_PRESS_SHORT);

    /* millis() wraparound. */
    assert(elrs_press_edge(&p, true, 0xFFFFFF00u) == ELRS_PRESS_NONE);
    assert(elrs_press_tick(&p, 0x00000100u) == ELRS_PRESS_NONE);
    assert(elrs_press_tick(&p, 0x00000400u) == ELRS_PRESS_LONG);
}

int main(void)
{
    test_frames();
    test_uid();
    test_press();
    printf("test_elrs_msp: OK\n");
    return 0;
}
