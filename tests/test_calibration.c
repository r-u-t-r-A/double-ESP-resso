/* Host test for ap-node/lib/CALIBRATION/calibration.h: the gain sweep, the
 * lap timer peak counter and the Enter/Exit search, on synthetic 1 kHz traces.
 * Build: cc -std=c99 -Wall -Wextra -Werror -I ap-node/lib/CALIBRATION tests/test_calibration.c -o /tmp/t_cal && /tmp/t_cal */
#include "calibration.h"

#include <assert.h>
#include <stdio.h>

#define FLOOR 20
#define PASS_HALF_MS 150 /* gate passes are triangles 300 ms wide */
#define CLIP_RAW 240     /* unclamped model value above this clips the ADC */

static cal_t cal;
static uint8_t trace[CAL_TRACE_LEN];

static uint32_t lcg_state;

static int noise(void) /* -2..2 */
{
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return (int)((lcg_state >> 16) % 5u) - 2;
}

/* Triangle of the given height centred on c, half-width h ms. */
static int tri(uint32_t t, uint32_t c, uint32_t h, int height)
{
    uint32_t d = t > c ? t - c : c - t;
    return d >= h ? 0 : (int)((int64_t)height * (int64_t)(h - d) / (int64_t)h);
}

/* Model: returns the unclamped RSSI at time t for the RF node's gain. */
typedef int (*model_fn)(uint32_t t, int gain);

static uint8_t clamp_rssi(int v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* ---- simulation driver --------------------------------------------------- */

#define MAX_EVENTS 256
static cal_event_t evs[MAX_EVENTS];
static uint8_t ev_args[MAX_EVENTS];
static uint32_t ev_ms[MAX_EVENTS];
static int nev;
static uint32_t seen_seq;
static int rf_gain;          /* what the simulated RF node runs at */
static uint32_t gain_cmd_ms; /* when the AP asked for a new gain */
static uint32_t phase_b_ms;  /* first ms in CAL_RECORD_LAPS, 0 = not yet */

static void collect(uint32_t t)
{
    while (seen_seq < cal.event_seq) {
        ++seen_seq;
        assert(nev < MAX_EVENTS);
        evs[nev] = cal_event_at(&cal, seen_seq, &ev_args[nev]);
        ev_ms[nev] = t;
        ++nev;
    }
}

static void sim_reset(uint8_t start_gain, uint8_t laps, uint32_t min_lap_ms, int initial_rf_gain)
{
    lcg_state = 12345u;
    nev = 0;
    seen_seq = cal.event_seq;
    rf_gain = initial_rf_gain;
    gain_cmd_ms = 0;
    phase_b_ms = 0;
    cal_start(&cal, 1, start_gain, laps, min_lap_ms);
    collect(1);
}

/* Runs until calibration ends or limit_ms; returns the end time. */
static uint32_t sim_run(model_fn model, uint32_t limit_ms)
{
    uint32_t t = 1;
    while (cal_active(&cal) && t < limit_ms) {
        ++t;
        /* The RF node applies a G command 30 ms after the gain changes. */
        if (rf_gain != cal.gain) {
            if (gain_cmd_ms == 0) gain_cmd_ms = t;
            if (t - gain_cmd_ms >= 30) {
                rf_gain = cal.gain;
                gain_cmd_ms = 0;
            }
        }
        if (cal.state == CAL_RECORD_LAPS && phase_b_ms == 0) phase_b_ms = t;
        int raw = model(t, rf_gain);
        uint8_t clip = raw > CLIP_RAW ? 20 : 0;
        cal_push(&cal, t, clamp_rssi(raw + noise()), clip, rf_gain == cal.gain);
        collect(t);
    }
    return t;
}

static int count_ev(cal_event_t e)
{
    int n = 0;
    for (int i = 0; i < nev; ++i) n += evs[i] == e;
    return n;
}

static int find_ev(cal_event_t e)
{
    for (int i = 0; i < nev; ++i)
        if (evs[i] == e) return i;
    return -1;
}

static void dump_events(void)
{
    for (int i = 0; i < nev; ++i)
        printf("  %7u ms %-18s %u\n", (unsigned)ev_ms[i], cal_event_name(evs[i]), ev_args[i]);
}

/* ---- models --------------------------------------------------------------- */

#define SWEEP_K 6 /* peak = FLOOR + 6 * gain: 45 -> 290, 41 -> 266, 37 -> 242, 33 -> 218 */
#define SWEEP_PERIOD 4000

static int model_sweep(uint32_t t, int gain)
{
    uint32_t c = 3000 + (t > 1000 ? (t - 1000) / SWEEP_PERIOD : 0) * SWEEP_PERIOD;
    return FLOOR + tri(t, c, PASS_HALF_MS, SWEEP_K * gain) +
           tri(t, c + SWEEP_PERIOD, PASS_HALF_MS, SWEEP_K * gain);
}

/* A pass whose value dips to ~73 % between two humps 80 ms apart. */
static int model_dip(uint32_t t, int gain)
{
    (void)gain;
    uint32_t c = 3000 + (t > 1000 ? (t - 1000) / SWEEP_PERIOD : 0) * SWEEP_PERIOD;
    int a = tri(t, c - 40, PASS_HALF_MS, 80);
    int b = tri(t, c + 40, PASS_HALF_MS, 80);
    return FLOOR + (a > b ? a : b);
}

static int model_always_clips(uint32_t t, int gain)
{
    (void)gain;
    return model_sweep(t, 89) > FLOOR + 50 ? 250 : FLOOR;
}

static int model_floor(uint32_t t, int gain)
{
    (void)t;
    (void)gain;
    return FLOOR;
}

/* Gate passes at +60 every 4 s during the gain sweep. In the lap phase the
 * signal is a staircase of humps 800 ms apart (+60, +55, ... +10): no band of
 * Enter values separates any pass count from the next one. */
static int model_staircase(uint32_t t, int gain)
{
    (void)gain;
    if (phase_b_ms == 0) {
        uint32_t c = 3000 + (t > 1000 ? (t - 1000) / SWEEP_PERIOD : 0) * SWEEP_PERIOD;
        return FLOOR + tri(t, c, PASS_HALF_MS, 60) + tri(t, c + SWEEP_PERIOD, PASS_HALF_MS, 60);
    }
    uint32_t dt = t - phase_b_ms;
    uint32_t k = dt / 800;
    if (k >= 11) return FLOOR;
    return FLOOR + tri(dt, k * 800 + 400, PASS_HALF_MS, 60 - 5 * (int)k);
}

/* ---- trace builders -------------------------------------------------------- */

/* 10 ms max buckets of floor + noise + gate passes + between-pass bumps. */
static uint32_t build_trace(int laps, uint32_t lap_ms, int gate, int bump, uint32_t bump_at_ms,
                            uint32_t bump_half_ms)
{
    uint32_t n = 0;
    uint32_t total = 1000 + (uint32_t)laps * lap_ms + 1500;
    uint8_t bmax = 0;
    for (uint32_t t = 0; t < total; ++t) {
        int v = FLOOR + noise();
        for (int l = 0; l < laps; ++l) {
            uint32_t c = 1000 + (uint32_t)l * lap_ms;
            v += tri(t, c, PASS_HALF_MS, gate);
            if (bump) v += tri(t, c + bump_at_ms, bump_half_ms, bump);
        }
        uint8_t r = clamp_rssi(v);
        if (r > bmax) bmax = r;
        if (t % CAL_TRACE_BUCKET_MS == CAL_TRACE_BUCKET_MS - 1) {
            trace[n++] = bmax;
            bmax = 0;
        }
    }
    return n;
}

/* ---- tests ------------------------------------------------------------------ */

static void test_sweep_locks_first_clean_gain(void)
{
    sim_reset(45, 3, 2000, 35);
    uint32_t end = sim_run(model_sweep, 300000);
    if (cal.state != CAL_DONE) dump_events();
    assert(cal.state == CAL_DONE && !cal_active(&cal));
    assert(evs[0] == CAL_EV_STARTED && ev_args[0] == 45);

    /* 45, 41 and 37 clip (unclamped peak above 240); 33 is the first clean gain. */
    assert(count_ev(CAL_EV_CLIPPED) == 3);
    uint8_t expect[] = {41, 37, 33};
    for (int i = 0, k = 0; i < nev; ++i)
        if (evs[i] == CAL_EV_CLIPPED) assert(ev_args[i] == expect[k++]);
    int locked = find_ev(CAL_EV_GAIN_LOCKED);
    assert(locked > 0 && ev_args[locked] == 33 && cal.gain == 33);
    assert(FLOOR + SWEEP_K * cal.gain <= CAL_PEAK_MAX);
    /* Two clean passes in a row at the locked gain, none before it. */
    assert(count_ev(CAL_EV_PASS_CLEAN) == CAL_CLEAN_PASSES);
    assert(evs[locked - 1] == CAL_EV_PASS_CLEAN && evs[locked - 2] == CAL_EV_FLY_THROUGH);
    assert(evs[locked + 1] == CAL_EV_FLY_LAPS && ev_args[locked + 1] == 3);
    assert(count_ev(CAL_EV_LAP_SEEN) == 3 && cal.laps_seen == 3);
    assert(evs[nev - 1] == CAL_EV_DONE);

    /* Thresholds sit between the floor and the gate peak and reproduce 3 passes. */
    int peak = FLOOR + SWEEP_K * 33;
    assert(cal.enter > FLOOR + 2 && cal.enter < peak);
    assert(cal.exit > FLOOR + 2 && cal.exit + 2 <= cal.enter);
    assert(cal.separation >= CAL_MIN_SEPARATION);
    assert(cal_count_passes(cal.trace, cal.trace_n, cal.enter, cal.exit, 200, NULL, 0, NULL) == 3);
    printf("sweep: locked gain %u, enter %u exit %u sep %u (%.1f dB), %u ms\n", cal.gain,
           cal.enter, cal.exit, cal.separation, cal.separation / CAL_RSSI_PER_DB, (unsigned)end);
}

static void test_dip_counts_once(void)
{
    /* Counter alone: a dip below Enter but above Exit doesn't split the pass. */
    const uint8_t t1[] = {20, 20, 60, 120, 150, 95, 150, 155, 120, 60, 20, 20};
    uint8_t pk[4], between = 0;
    assert(cal_count_passes(t1, sizeof(t1), 100, 80, 0, pk, 4, &between) == 1 && pk[0] == 155);
    /* A dip below Exit does split it, as in LapTimer. */
    const uint8_t t2[] = {20, 120, 150, 70, 150, 20};
    assert(cal_count_passes(t2, sizeof(t2), 100, 80, 0, NULL, 0, NULL) == 2);
    /* ... unless the second peak falls inside the minimum lap. */
    assert(cal_count_passes(t2, sizeof(t2), 100, 80, 3, NULL, 0, NULL) == 1);
    /* Between-pass max and per-pass peaks. */
    const uint8_t t3[] = {20, 130, 20, 45, 20, 140, 20, 50, 20, 135, 20};
    assert(cal_count_passes(t3, sizeof(t3), 100, 80, 0, pk, 4, &between) == 3);
    assert(pk[0] == 130 && pk[1] == 140 && pk[2] == 135 && between == 50);

    /* Full state machine with no minimum lap: each double-humped pass is
     * judged once in the sweep and counted once in the lap phase. */
    sim_reset(20, 3, 0, 20);
    sim_run(model_dip, 120000);
    if (cal.state != CAL_DONE) dump_events();
    assert(cal.state == CAL_DONE);
    assert(count_ev(CAL_EV_CLIPPED) == 0 && count_ev(CAL_EV_PASS_CLEAN) == 2);
    int locked = find_ev(CAL_EV_GAIN_LOCKED);
    /* Passes at 3 s and 7 s lock it; laps are the passes at 11, 15 and 19 s. */
    assert(ev_ms[locked] > 7000 && ev_ms[locked] < 8000);
    assert(count_ev(CAL_EV_LAP_SEEN) == 3);
    for (int i = 0, k = 0; i < nev; ++i)
        if (evs[i] == CAL_EV_LAP_SEEN) {
            uint32_t c = 11000 + 4000u * (uint32_t)k++;
            assert(ev_ms[i] > c && ev_ms[i] < c + 500);
        }
    assert(cal_count_passes(cal.trace, cal.trace_n, cal.enter, cal.exit, 0, NULL, 0, NULL) == 3);
    printf("dip: enter %u exit %u sep %u\n", cal.enter, cal.exit, cal.separation);
}

static void test_whoop_track_thresholds(void)
{
    /* 3 laps of 6 s; the course passes close to the timer 3 s after the gate:
     * a wide bump 25 counts above the floor. Gate passes are 80 above it. */
    lcg_state = 777u;
    uint32_t n = build_trace(3, 6000, 80, 25, 3000, 400);
    uint32_t mlb = 2000 / CAL_TRACE_BUCKET_MS;
    uint8_t en = 0, ex = 0, sep = 0;
    assert(cal_pick_thresholds(trace, n, mlb, 3, &en, &ex, &sep));
    assert(en > FLOOR + 25 + 2);  /* above the bump, noise included */
    assert(en < FLOOR + 80);
    assert(ex > FLOOR + 2);       /* above the floor */
    assert(ex + 2 <= en);
    assert(sep > 0 && sep >= CAL_MIN_SEPARATION);
    uint8_t between = 0;
    assert(cal_count_passes(trace, n, en, ex, mlb, NULL, 0, &between) == 3);
    assert(between >= FLOOR + 25 - 2 && between < en);  /* bump (or a rising edge) */
    /* A threshold below the bump would have counted it as a lap. */
    assert(cal_count_passes(trace, n, FLOOR + 15, FLOOR + 8, mlb, NULL, 0, NULL) > 3);
    printf("whoop: enter %u exit %u sep %u (%.1f dB), bump max %u\n", en, ex, sep,
           sep / CAL_RSSI_PER_DB, between);
}

static void test_no_separation_fails(void)
{
    /* Bumps as high as the gate passes: no Enter gives the 3 real passes. */
    lcg_state = 99u;
    uint32_t n = build_trace(3, 6000, 60, 60, 3000, PASS_HALF_MS);
    uint8_t en, ex, sep;
    assert(!cal_pick_thresholds(trace, n, 2000 / CAL_TRACE_BUCKET_MS, 3, &en, &ex, &sep));
    /* Too little range at all. */
    lcg_state = 5u;
    n = build_trace(3, 6000, 10, 0, 0, 0);
    assert(!cal_pick_thresholds(trace, n, 200, 3, &en, &ex, &sep));

    /* Full state machine: clean sweep, then a lap phase where humps of every
     * height leave no band of Enter values wide enough. */
    sim_reset(30, 3, 500, 30);
    sim_run(model_staircase, 200000);
    if (cal.state != CAL_FAILED) dump_events();
    assert(cal.state == CAL_FAILED && !cal_active(&cal));
    assert(find_ev(CAL_EV_GAIN_LOCKED) >= 0 && cal.laps_seen >= 3);
    assert(evs[nev - 1] == CAL_EV_FAIL_NO_SEPARATION && cal.event == CAL_EV_FAIL_NO_SEPARATION);
    printf("no separation: failed after %u counted passes\n", cal.laps_seen);
}

static void test_too_strong_at_min_gain(void)
{
    sim_reset(13, 3, 2000, 13);
    sim_run(model_always_clips, 120000);
    assert(cal.state == CAL_FAILED);
    assert(count_ev(CAL_EV_CLIPPED) == 2);  /* 13 -> 9 -> 5 */
    assert(cal.gain == CAL_MIN_GAIN);
    assert(evs[nev - 1] == CAL_EV_FAIL_TOO_STRONG);
    assert(find_ev(CAL_EV_GAIN_LOCKED) < 0);
}

static void test_no_pass_retries_then_fails(void)
{
    sim_reset(45, 3, 2000, 45);
    uint32_t end = sim_run(model_floor, 200000);
    assert(cal.state == CAL_FAILED);
    assert(count_ev(CAL_EV_FLY_THROUGH) == 1);
    assert(count_ev(CAL_EV_NO_PASS) == CAL_MAX_RETRIES);
    assert(evs[nev - 1] == CAL_EV_FAIL_NO_SIGNAL);
    /* Settle, then one window plus CAL_MAX_RETRIES retries. */
    uint32_t expect = CAL_SETTLE_MS + (CAL_MAX_RETRIES + 1) * CAL_PASS_TIMEOUT_MS;
    assert(end > expect && end < expect + 100);
}

static void test_abort_and_start_limits(void)
{
    sim_reset(45, 3, 2000, 45);
    sim_run(model_floor, 2000);
    assert(cal_active(&cal) && cal.state == CAL_WAIT_PASS);
    uint32_t seq = cal.event_seq;
    cal_abort(&cal);
    assert(cal.state == CAL_IDLE && !cal_active(&cal));
    assert(cal.event == CAL_EV_ABORTED && cal.event_seq == seq + 1);
    cal_abort(&cal);  /* no-op when idle */
    assert(cal.event_seq == seq + 1);
    /* Pushing while idle does nothing. */
    cal_push(&cal, 5000, 200, 100, true);
    assert(cal.state == CAL_IDLE && cal.event_seq == seq + 1);

    /* Start clamps the gain and laps, and keeps counting seq. */
    cal_start(&cal, 10, 200, 0, 1000);
    assert(cal.gain == CAL_MAX_GAIN && cal.laps_wanted == CAL_LAPS_DEFAULT);
    assert(cal.event == CAL_EV_STARTED && cal.event_seq == seq + 2);
    assert(cal_event_at(&cal, seq + 1, NULL) == CAL_EV_ABORTED);
    cal_start(&cal, 10, 0, 250, 1000);
    assert(cal.gain == CAL_MIN_GAIN && cal.laps_wanted == CAL_LAPS_MAX);
    /* Waits for the RF node to confirm the gain, however long it takes. */
    cal_push(&cal, 20000, 20, 0, false);
    assert(cal.state == CAL_SET_GAIN);
    cal_push(&cal, 20001, 20, 0, true);
    assert(cal.state == CAL_SETTLE);
    cal_abort(&cal);

    assert(strcmp(cal_state_name(CAL_WAIT_PASS), "wait_pass") == 0);
    assert(strcmp(cal_event_name(CAL_EV_FAIL_NO_SEPARATION), "fail_no_separation") == 0);
}

int main(void)
{
    test_sweep_locks_first_clean_gain();
    test_dip_counts_once();
    test_whoop_track_thresholds();
    test_no_separation_fails();
    test_too_strong_at_min_gain();
    test_no_pass_retries_then_fails();
    test_abort_and_start_limits();
    printf("test_calibration: OK (cal_t is %u bytes)\n", (unsigned)sizeof(cal_t));
    return 0;
}
