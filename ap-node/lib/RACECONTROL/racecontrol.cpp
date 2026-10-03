#include "racecontrol.h"

#include "debug.h"

void RaceControl::init(Config *config, LapTimer *lapTimer) {
    conf = config;
    timer = lapTimer;
}

void RaceControl::emit(const char *event, uint32_t value) {
    if (notify) notify(event, value);
}

void RaceControl::begin(uint32_t currentTimeMs) {
    if (!isIdle()) return;
    // Browser speech runs at 150 wpm times the announcer rate (stored x10).
    uint32_t rate10 = conf->getAnnouncerRate();
    if (rate10 == 0) rate10 = 10;
    uint32_t speechMs = (uint32_t)RACE_ARM_WORDS * 60000u * 10u / (RACE_WORDS_PER_MINUTE * rate10);
    armDurationMs = speechMs + RACE_RANDOM_MIN_MS + esp_random() % (RACE_RANDOM_MAX_MS - RACE_RANDOM_MIN_MS + 1);
    armStartMs = currentTimeMs;
    arming = true;
    DEBUG("Race arming, start in %u ms\n", armDurationMs);
#ifdef PIN_LED_STRIP
    if (ledStrip) ledStrip->arm(currentTimeMs);
    ledRearmMs = currentTimeMs;
#endif
    emit("raceArm", speechMs);
}

void RaceControl::stop() {
    arming = false;
    timer->stop();
    emit("raceStop", 0);
}

void RaceControl::handleRaceControl(uint32_t currentTimeMs) {
    if (stopRequested) {
        stopRequested = false;
        beginRequested = false;
        stop();
        return;
    }
    if (beginRequested) {
        beginRequested = false;
        begin(currentTimeMs);
    }
    if (!arming) return;
#ifdef PIN_LED_STRIP
    // Keep the gate red past LEDSTRIP_ARM_TIMEOUT_MS at slow announcer rates.
    if (ledStrip && (currentTimeMs - ledRearmMs) > RACE_LED_REARM_MS) {
        ledStrip->arm(currentTimeMs);
        ledRearmMs = currentTimeMs;
    }
#endif
    if ((currentTimeMs - armStartMs) >= armDurationMs) {
        arming = false;
        timer->start();
        emit("raceStart", 0);
    }
}
