#pragma once

#include "buzzer.h"
#include "laptimer.h"
#ifdef PIN_LED_STRIP
#include "ledstrip.h"
#endif

// Start sequence. The browser says "Arm your quad" on raceArm, then staging
// follows RotorHazard's default race format: three staging tones 1 s apart,
// then the start tone 0.5-4 s (random) after the last one.
#define RACE_ARM_WORDS 3
#define RACE_WORDS_PER_MINUTE 150
#define RACE_STAGE_TONES 3
#define RACE_STAGE_TONE_INTERVAL_MS 1000
#define RACE_START_DELAY_MIN_MS 500
#define RACE_START_DELAY_RANDOM_MS 3500
#define RACE_LED_REARM_MS 1000

// SSE notification: event is "raceArm" (value = speech lead ms), "raceTone"
// (value = staging tones still to come), "raceStart" or "raceStop" (value 0).
typedef void (*race_notify_fn)(const char *event, uint32_t value);

// Runs the race start countdown on the AP node so the web UI button and the
// radio button behave the same. Requests may come from any task; the state
// machine only runs in handleRaceControl() on parallelTask.
class RaceControl {
   public:
    void init(Config *config, LapTimer *lapTimer, Buzzer *buzzer);
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
    Buzzer *buz;
#ifdef PIN_LED_STRIP
    LedStrip *ledStrip = nullptr;
#endif
    race_notify_fn notify = nullptr;

    volatile bool beginRequested = false;
    volatile bool stopRequested = false;
    bool arming = false;
    uint32_t armStartMs = 0;
    uint32_t speechMs = 0;      // "Arm your quad", before the first staging tone
    uint32_t armDurationMs = 0; // arm to start tone
    uint8_t tonesPlayed = 0;
    uint32_t ledRearmMs = 0;
};
