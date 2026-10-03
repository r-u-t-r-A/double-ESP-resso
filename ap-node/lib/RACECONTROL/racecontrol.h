#pragma once

#include "laptimer.h"
#ifdef PIN_LED_STRIP
#include "ledstrip.h"
#endif

// Start sequence: "Arm your quad" / "Starting on the tone in less than five"
// is spoken by the browser on raceArm; the tone comes 1-5 s after that.
#define RACE_ARM_WORDS 11
#define RACE_WORDS_PER_MINUTE 150
#define RACE_RANDOM_MIN_MS 1000
#define RACE_RANDOM_MAX_MS 5000
#define RACE_LED_REARM_MS 1000

// SSE notification: event is "raceArm" (value = speech lead ms), "raceStart"
// or "raceStop" (value 0).
typedef void (*race_notify_fn)(const char *event, uint32_t value);

// Runs the race start countdown on the AP node so the web UI button and the
// radio button behave the same. Requests may come from any task; the state
// machine only runs in handleRaceControl() on parallelTask.
class RaceControl {
   public:
    void init(Config *config, LapTimer *lapTimer);
#ifdef PIN_LED_STRIP
    void setLedStrip(LedStrip *s) { ledStrip = s; }
#endif
    void setNotifier(race_notify_fn fn) { notify = fn; }

    void requestBegin() { beginRequested = true; }
    void requestStop() { stopRequested = true; }
    bool isArming() { return arming; }
    bool isIdle() { return !arming && !timer->isRunning(); }

    void handleRaceControl(uint32_t currentTimeMs);

   private:
    void begin(uint32_t currentTimeMs);
    void stop();
    void emit(const char *event, uint32_t value);

    Config *conf;
    LapTimer *timer;
#ifdef PIN_LED_STRIP
    LedStrip *ledStrip = nullptr;
#endif
    race_notify_fn notify = nullptr;

    volatile bool beginRequested = false;
    volatile bool stopRequested = false;
    bool arming = false;
    uint32_t armStartMs = 0;
    uint32_t armDurationMs = 0;
    uint32_t ledRearmMs = 0;
};
