/*
 * calibration.h - voice-guided auto-calibration of the RF node gain and the
 * lap timer's Enter/Exit thresholds, from real gate passes.
 *
 * Plain C99, header-only, no Arduino or ESP-IDF dependencies, so
 * tests/test_calibration.c can run it on the host. The AP node feeds it one
 * Kalman-filtered sample (the same value LapTimer compares) per 1 kHz RF node
 * sample, applies c->gain while it is active, and reads the result.
 *
 * Phase A (gain sweep): start high and step the gain down after every pass
 * that clips the I/Q ADC or leaves no headroom, until CAL_CLEAN_PASSES clean
 * passes in a row. The dB per gain step is not characterised, so the search is
 * empirical and does not model it.
 * Phase B (thresholds): at the locked gain, record the filtered RSSI while the
 * pilot flies laps, then choose Enter/Exit as the centre of the widest band
 * that reproduces the counted passes with the lap timer's own peak logic.
 *
 * Every tunable below is untested on hardware.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAL_MAX_GAIN          89
#define CAL_START_GAIN_DEFAULT 45   /* sweep starts here and goes down */
#define CAL_MIN_GAIN          5
#define CAL_GAIN_STEP         4     /* coarse step down after a clipped pass */
#define CAL_SETTLE_MS         400   /* after the RF node confirms a new gain */
#define CAL_BASELINE_MS       500   /* start of each pass window: baseline median */
#define CAL_PASS_TIMEOUT_MS   15000 /* no pass seen in this window -> retry */
#define CAL_MAX_RETRIES       3     /* retries after a timeout; one more timeout fails */
#define CAL_MIN_RISE          20    /* counts (~5 dB) above baseline to count as a pass */
#define CAL_PASS_END_FRACTION 0.5f  /* pass ends when RSSI falls below baseline + this * rise */
#define CAL_TAIL_MS           300   /* keep watching clip after the pass ends */
#define CAL_CLIP_MAX_PERMILLE 5     /* above this in a pass window = clipped */
#define CAL_PEAK_MAX          230   /* filtered peak above this = no headroom (~6 dB under rail) */
#define CAL_CLEAN_PASSES      2     /* clean passes in a row at one gain to accept it */
#define CAL_LAPS_DEFAULT      3     /* passes flown in the threshold phase */
#define CAL_LAPS_MAX          20
#define CAL_LAPS_TIMEOUT_MS   180000
#define CAL_LAPS_QUIET_MS     1000  /* below p_exit this long after the last lap -> stop */
#define CAL_TRACE_BUCKET_MS   10    /* trace stored as max-per-10ms */
#define CAL_TRACE_LEN         18000 /* 180 s at 10 ms */
#define CAL_MIN_SEPARATION    8     /* counts: below this the gate can't be told apart -> fail */
#define CAL_EVENT_LOG         8     /* recent events kept for a poller that missed some */

/* RSSI counts per dB: the RF node maps -6..54 dB onto 0-255
 * (BP_RSSI_* in rf-node/main/band_power.h). */
#define CAL_RSSI_PER_DB       4.25f

typedef enum {
    CAL_IDLE, CAL_SET_GAIN, CAL_SETTLE, CAL_WAIT_PASS, CAL_PASS_TAIL,
    CAL_RECORD_LAPS, CAL_DONE, CAL_FAILED
} cal_state_t;

typedef enum {   /* what the browser should say; seq increments on every new event */
    CAL_EV_NONE, CAL_EV_STARTED, CAL_EV_FLY_THROUGH, CAL_EV_CLIPPED, CAL_EV_PASS_CLEAN,
    CAL_EV_NO_PASS, CAL_EV_GAIN_LOCKED, CAL_EV_FLY_LAPS, CAL_EV_LAP_SEEN, CAL_EV_DONE,
    CAL_EV_FAIL_TOO_STRONG, CAL_EV_FAIL_NO_SIGNAL, CAL_EV_FAIL_NO_SEPARATION, CAL_EV_ABORTED,
    CAL_EV_COUNT
} cal_event_t;

/* The lap timer's peak logic (LapTimer::lapPeakCapture / lapPeakCaptured)
 * over 10 ms trace buckets, one step per bucket. */
typedef struct {
    uint32_t n;          /* captured passes */
    uint32_t last_cap;   /* bucket of the last captured peak */
    uint32_t peak_i;
    uint8_t peak;        /* 0 = not inside a pass */
    uint8_t between_max; /* highest value outside a pass after the first one,
                            rising edges below Enter included */
} cal_counter_t;

typedef struct {
    cal_state_t state;
    cal_event_t event; uint32_t event_seq;
    uint8_t gain;            /* gain the caller must apply while active */
    uint8_t start_gain;
    uint8_t laps_wanted, laps_seen;
    uint8_t clean_in_row, retries;
    uint32_t phase_ms;       /* ms at start of current phase/window */
    /* current pass window */
    uint8_t baseline, peak, clip_max;
    bool have_baseline;
    uint8_t base_hist[CAL_BASELINE_MS / CAL_TRACE_BUCKET_MS]; uint16_t base_n;
    uint32_t base_ms;
    uint8_t weakest_clean_peak;   /* min peak over the accepted clean passes */
    uint8_t last_peak, last_clip; /* for display */
    /* lap phase trace */
    uint8_t trace[CAL_TRACE_LEN]; uint32_t trace_n; uint8_t bucket_max; uint32_t bucket_ms;
    uint32_t min_lap_ms;
    uint8_t p_enter, p_exit;      /* provisional thresholds from phase A */
    cal_counter_t live;
    uint32_t quiet_ms;            /* start of the current run below p_exit */
    /* recent events with their argument (gain or lap number), by seq */
    uint8_t ev_log[CAL_EVENT_LOG]; uint8_t ev_arg[CAL_EVENT_LOG];
    /* result */
    uint8_t enter, exit, separation; /* separation in RSSI counts */
} cal_t;

static inline const char *cal_state_name(cal_state_t s)
{
    static const char *const names[] = {
        "idle", "set_gain", "settle", "wait_pass", "pass_tail", "record_laps", "done", "failed",
    };
    return (unsigned)s <= (unsigned)CAL_FAILED ? names[s] : "?";
}

static inline const char *cal_event_name(cal_event_t e)
{
    static const char *const names[CAL_EV_COUNT] = {
        "none", "started", "fly_through", "clipped", "pass_clean", "no_pass", "gain_locked",
        "fly_laps", "lap_seen", "done", "fail_too_strong", "fail_no_signal",
        "fail_no_separation", "aborted",
    };
    return (unsigned)e < CAL_EV_COUNT ? names[e] : "?";
}

static inline bool cal_active(const cal_t *c)   /* state not IDLE/DONE/FAILED */
{
    return c->state != CAL_IDLE && c->state != CAL_DONE && c->state != CAL_FAILED;
}

/* The event numbered seq and its argument. Only valid while it is still in
 * the log: event_seq - CAL_EVENT_LOG < seq <= event_seq. */
static inline cal_event_t cal_event_at(const cal_t *c, uint32_t seq, uint8_t *arg)
{
    if (arg) *arg = c->ev_arg[seq % CAL_EVENT_LOG];
    return (cal_event_t)c->ev_log[seq % CAL_EVENT_LOG];
}

static inline void cal_emit(cal_t *c, cal_event_t e, uint8_t arg)
{
    uint32_t seq = c->event_seq + 1;
    c->ev_log[seq % CAL_EVENT_LOG] = (uint8_t)e;
    c->ev_arg[seq % CAL_EVENT_LOG] = arg;
    c->event = e;
    c->event_seq = seq;
}

static inline void cal_fail(cal_t *c, cal_event_t e)
{
    c->state = CAL_FAILED;
    cal_emit(c, e, 0);
}

static inline void cal_counter_reset(cal_counter_t *k)
{
    memset(k, 0, sizeof(*k));
}

/* Feeds bucket i (value v). Returns true when a pass was captured; its peak
 * is then in *peak_out. The first pass counts at once (hole shot); later
 * peaks are only tracked min_lap_buckets after the previous captured peak. */
static inline bool cal_counter_step(cal_counter_t *k, uint32_t i, uint8_t v, uint8_t enter,
                                    uint8_t exit, uint32_t min_lap_buckets, uint8_t *peak_out)
{
    bool armed = k->n == 0 || i - k->last_cap > min_lap_buckets;
    if (armed && v >= enter && v > k->peak) {
        k->peak = v;
        k->peak_i = i;
    }
    if (k->peak && v < k->peak && v < exit) {
        if (peak_out) *peak_out = k->peak;
        k->n++;
        k->last_cap = k->peak_i;
        k->peak = 0;
        return true;
    }
    if (!k->peak && k->n > 0 && v > k->between_max) k->between_max = v;
    return false;
}

/* Counts passes in trace[0..n) as LapTimer would with these thresholds.
 * Optionally writes up to max_peaks per-pass peaks, and the highest value
 * seen between captured passes. */
static inline uint32_t cal_count_passes(const uint8_t *trace, uint32_t n, uint8_t enter,
                                        uint8_t exit, uint32_t min_lap_buckets, uint8_t *peaks,
                                        uint32_t max_peaks, uint8_t *between_max)
{
    cal_counter_t k;
    cal_counter_reset(&k);
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t pk;
        uint32_t before = k.n;
        if (cal_counter_step(&k, i, trace[i], enter, exit, min_lap_buckets, &pk) && peaks &&
            before < max_peaks)
            peaks[before] = pk;
    }
    if (between_max) *between_max = k.between_max;
    return k.n;
}

static inline uint8_t cal_default_exit(uint8_t enter, uint8_t lo)
{
    int d = (enter - lo) / 4;
    if (d < 2) d = 2;
    return (uint8_t)(enter > d ? enter - d : 0);
}

/* Picks Enter/Exit that give exactly `want` passes in the trace, centred in
 * the widest band of Enter values that does. *sep is that band's width in
 * RSSI counts: roughly the weakest gate peak minus the highest bump between
 * passes. Returns false when no band is CAL_MIN_SEPARATION wide. */
static inline bool cal_pick_thresholds(const uint8_t *trace, uint32_t n, uint32_t min_lap_buckets,
                                       uint32_t want, uint8_t *enter, uint8_t *exit, uint8_t *sep)
{
    if (n == 0 || want == 0) return false;
    uint8_t lo = 255, hi = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (trace[i] < lo) lo = trace[i];
        if (trace[i] > hi) hi = trace[i];
    }
    if (hi - lo < 2 * CAL_MIN_SEPARATION) return false;

    /* Widest contiguous run of Enter values (scanned downwards) with count == want. */
    int best_lo = -1, best_hi = -1, run_hi = -1;
    for (int e = hi - 1; e >= lo + 2; --e) {
        uint8_t x = cal_default_exit((uint8_t)e, lo);
        bool ok = cal_count_passes(trace, n, (uint8_t)e, x, min_lap_buckets, NULL, 0, NULL) == want;
        if (ok && run_hi < 0) run_hi = e;
        if (ok && run_hi - e > best_hi - best_lo) {
            best_hi = run_hi;
            best_lo = e;
        }
        if (!ok) run_hi = -1;
    }
    if (best_lo < 0) return false;
    int s = best_hi - best_lo;
    if (s < CAL_MIN_SEPARATION) return false;
    int en = best_lo + s / 2;

    /* Exit: centre of the widest run below Enter that keeps the count. */
    int xb_lo = -1, xb_hi = -1;
    run_hi = -1;
    for (int x = en - 2; x >= lo + 1; --x) {
        bool ok = cal_count_passes(trace, n, (uint8_t)en, (uint8_t)x, min_lap_buckets, NULL, 0,
                                   NULL) == want;
        if (ok && run_hi < 0) run_hi = x;
        if (ok && run_hi - x > xb_hi - xb_lo) {
            xb_hi = run_hi;
            xb_lo = x;
        }
        if (!ok) run_hi = -1;
    }
    int ex = xb_lo >= 0 ? xb_lo + (xb_hi - xb_lo) / 2 : cal_default_exit((uint8_t)en, lo);
    if (ex > en - 2) ex = en - 2;

    *enter = (uint8_t)en;
    *exit = (uint8_t)ex;
    *sep = (uint8_t)s;
    return true;
}

static inline void cal_open_window(cal_t *c, uint32_t now_ms, bool say_fly)
{
    c->state = CAL_WAIT_PASS;
    c->phase_ms = now_ms;
    c->have_baseline = false;
    c->base_n = 0;
    c->base_ms = now_ms - CAL_TRACE_BUCKET_MS;  /* take the first sample */
    c->baseline = 0;
    c->peak = 0;
    c->clip_max = 0;
    if (say_fly) cal_emit(c, CAL_EV_FLY_THROUGH, c->gain);
}

static inline uint8_t cal_median(uint8_t *v, uint16_t n)
{
    for (uint16_t i = 1; i < n; ++i) {  /* insertion sort, n <= 50 */
        uint8_t x = v[i];
        uint16_t j = i;
        while (j > 0 && v[j - 1] > x) {
            v[j] = v[j - 1];
            --j;
        }
        v[j] = x;
    }
    return v[n / 2];
}

static inline void cal_start_laps(cal_t *c, uint32_t now_ms)
{
    uint8_t w = c->weakest_clean_peak;
    uint8_t b = w > c->baseline ? c->baseline : w;
    c->p_enter = (uint8_t)(b + (w - b) / 2);
    c->p_exit = (uint8_t)(b + (w - b) / 4);
    c->trace_n = 0;
    c->bucket_max = 0;
    c->bucket_ms = now_ms;
    c->laps_seen = 0;
    cal_counter_reset(&c->live);
    c->quiet_ms = now_ms;
    c->state = CAL_RECORD_LAPS;
    c->phase_ms = now_ms;
    cal_emit(c, CAL_EV_FLY_LAPS, c->laps_wanted);
}

static inline void cal_finish_laps(cal_t *c)
{
    if (c->laps_seen == 0) {
        cal_fail(c, CAL_EV_FAIL_NO_SIGNAL);
        return;
    }
    uint32_t mlb = c->min_lap_ms / CAL_TRACE_BUCKET_MS;
    uint8_t en, ex, sep;
    if (!cal_pick_thresholds(c->trace, c->trace_n, mlb, c->laps_seen, &en, &ex, &sep)) {
        cal_fail(c, CAL_EV_FAIL_NO_SEPARATION);
        return;
    }
    c->enter = en;
    c->exit = ex;
    c->separation = sep;
    c->state = CAL_DONE;
    cal_emit(c, CAL_EV_DONE, 0);
}

/* Judges the pass that just ended (CAL_PASS_TAIL done). */
static inline void cal_judge_pass(cal_t *c, uint32_t now_ms)
{
    c->retries = 0;
    c->last_peak = c->peak;
    c->last_clip = c->clip_max;
    if (c->clip_max > CAL_CLIP_MAX_PERMILLE || c->peak > CAL_PEAK_MAX) {
        c->clean_in_row = 0;
        c->weakest_clean_peak = 255;
        if (c->gain <= CAL_MIN_GAIN) {
            cal_fail(c, CAL_EV_FAIL_TOO_STRONG);
            return;
        }
        c->gain = c->gain - CAL_GAIN_STEP > CAL_MIN_GAIN ? (uint8_t)(c->gain - CAL_GAIN_STEP)
                                                         : CAL_MIN_GAIN;
        cal_emit(c, CAL_EV_CLIPPED, c->gain);
        c->state = CAL_SET_GAIN;
        c->phase_ms = now_ms;
        return;
    }
    c->clean_in_row++;
    if (c->peak < c->weakest_clean_peak) c->weakest_clean_peak = c->peak;
    cal_emit(c, CAL_EV_PASS_CLEAN, c->gain);
    if (c->clean_in_row >= CAL_CLEAN_PASSES) {
        cal_emit(c, CAL_EV_GAIN_LOCKED, c->gain);
        cal_start_laps(c, now_ms);
    } else {
        cal_open_window(c, now_ms, true);
    }
}

static inline void cal_start(cal_t *c, uint32_t now_ms, uint8_t start_gain, uint8_t laps,
                             uint32_t min_lap_ms)
{
    uint32_t seq = c->event_seq;  /* keep counting so a poller sees the new run */
    uint8_t ev_log[CAL_EVENT_LOG], ev_arg[CAL_EVENT_LOG];
    memcpy(ev_log, c->ev_log, sizeof(ev_log));
    memcpy(ev_arg, c->ev_arg, sizeof(ev_arg));
    memset(c, 0, sizeof(*c));
    c->event_seq = seq;
    memcpy(c->ev_log, ev_log, sizeof(ev_log));
    memcpy(c->ev_arg, ev_arg, sizeof(ev_arg));

    if (start_gain < CAL_MIN_GAIN) start_gain = CAL_MIN_GAIN;
    if (start_gain > CAL_MAX_GAIN) start_gain = CAL_MAX_GAIN;
    if (laps == 0) laps = CAL_LAPS_DEFAULT;
    if (laps > CAL_LAPS_MAX) laps = CAL_LAPS_MAX;
    c->start_gain = start_gain;
    c->gain = start_gain;
    c->laps_wanted = laps;
    c->min_lap_ms = min_lap_ms;
    c->clean_in_row = 0;
    c->weakest_clean_peak = 255;
    cal_emit(c, CAL_EV_STARTED, c->gain);
    c->state = CAL_SET_GAIN;
    c->phase_ms = now_ms;
}

static inline void cal_abort(cal_t *c)
{
    if (!cal_active(c)) return;
    c->state = CAL_IDLE;
    cal_emit(c, CAL_EV_ABORTED, 0);
}

/* Call once per lap-timer sample. rssi = Kalman-filtered RSSI (timer.getRssi()),
 * clip = raw clip per mille from the link, gain_ok = RF node reports c->gain. */
static inline void cal_push(cal_t *c, uint32_t now_ms, uint8_t rssi, uint8_t clip, bool gain_ok)
{
    switch (c->state) {
    case CAL_SET_GAIN:
        /* No timeout: the caller keeps re-sending G until the RF node agrees. */
        if (gain_ok) {
            c->state = CAL_SETTLE;
            c->phase_ms = now_ms;
        }
        break;
    case CAL_SETTLE:
        if (now_ms - c->phase_ms >= CAL_SETTLE_MS) cal_open_window(c, now_ms, true);
        break;
    case CAL_WAIT_PASS:
        if (now_ms - c->phase_ms >= CAL_PASS_TIMEOUT_MS) {
            c->retries++;
            if (c->retries > CAL_MAX_RETRIES) {
                cal_fail(c, CAL_EV_FAIL_NO_SIGNAL);
                return;
            }
            cal_emit(c, CAL_EV_NO_PASS, c->gain);
            cal_open_window(c, now_ms, false);  /* NO_PASS already says "fly through" */
            break;
        }
        if (!c->have_baseline) {
            if (now_ms - c->base_ms >= CAL_TRACE_BUCKET_MS &&
                c->base_n < sizeof(c->base_hist)) {
                c->base_hist[c->base_n++] = rssi;
                c->base_ms = now_ms;
            }
            if (now_ms - c->phase_ms >= CAL_BASELINE_MS) {
                c->baseline = cal_median(c->base_hist, c->base_n);
                c->have_baseline = true;
            }
            break;
        }
        if (rssi > c->peak) c->peak = rssi;
        if (clip > c->clip_max) c->clip_max = clip;
        if ((int)c->peak >= (int)c->baseline + CAL_MIN_RISE) {
            float end = (float)c->baseline + CAL_PASS_END_FRACTION * (float)(c->peak - c->baseline);
            if ((float)rssi < end) {
                c->state = CAL_PASS_TAIL;
                c->phase_ms = now_ms;
            }
        }
        break;
    case CAL_PASS_TAIL:
        if (clip > c->clip_max) c->clip_max = clip;
        if (now_ms - c->phase_ms >= CAL_TAIL_MS) cal_judge_pass(c, now_ms);
        break;
    case CAL_RECORD_LAPS: {
        if (now_ms - c->bucket_ms >= CAL_TRACE_BUCKET_MS) {
            uint32_t i = c->trace_n;
            c->trace[c->trace_n++] = c->bucket_max;
            if (cal_counter_step(&c->live, i, c->bucket_max, c->p_enter, c->p_exit,
                                 c->min_lap_ms / CAL_TRACE_BUCKET_MS, NULL)) {
                if (c->laps_seen < 255) c->laps_seen++;
                cal_emit(c, CAL_EV_LAP_SEEN, c->laps_seen);
            }
            c->bucket_max = 0;
            c->bucket_ms = now_ms;
        }
        if (rssi > c->bucket_max) c->bucket_max = rssi;
        if (rssi >= c->p_exit) c->quiet_ms = now_ms;
        bool quiet = c->laps_seen >= c->laps_wanted && now_ms - c->quiet_ms >= CAL_LAPS_QUIET_MS;
        if (quiet || now_ms - c->phase_ms >= CAL_LAPS_TIMEOUT_MS || c->trace_n >= CAL_TRACE_LEN)
            cal_finish_laps(c);
        break;
    }
    default:
        break;
    }
}

#ifdef __cplusplus
}
#endif
