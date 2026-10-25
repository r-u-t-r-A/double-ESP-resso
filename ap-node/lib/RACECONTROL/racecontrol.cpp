#include "racecontrol.h"

#include "debug.h"

void RaceControl::init(Config *config, LapTimer *lapTimer, Buzzer *buzzer) {
    conf = config;
    timer = lapTimer;
    buz = buzzer;
}

void RaceControl::emit(const char *event, uint32_t value) {
    if (notify) notify(event, value);
}

void RaceControl::begin(uint32_t currentTimeMs) {
    if (!isIdle()) return;
    // Browser speech runs at 150 wpm times the announcer rate (stored x10).
    uint32_t rate10 = conf->getAnnouncerRate();
    if (rate10 == 0) rate10 = 10;
    speechMs = (uint32_t)RACE_ARM_WORDS * 60000u * 10u / (RACE_WORDS_PER_MINUTE * rate10);
    armDurationMs = speechMs + (RACE_STAGE_TONES - 1) * RACE_STAGE_TONE_INTERVAL_MS +
                    RACE_START_DELAY_MIN_MS + esp_random() % (RACE_START_DELAY_RANDOM_MS + 1);
    armStartMs = currentTimeMs;
    tonesPlayed = 0;
    arming = true;
    DEBUG("Race staging, start in %u ms\n", armDurationMs);
#ifdef PIN_LED_STRIP
    if (ledStrip) ledStrip->arm(currentTimeMs);
    ledRearmMs = currentTimeMs;
#endif
    emit("raceArm", speechMs);
}

void RaceControl::stop() {
    arming = false;
#ifdef PIN_LED_STRIP
    // Red for a moment, then idle; also ends staging, which the strip tracks
    // on its own and would otherwise keep showing.
    if (ledStrip) ledStrip->stop();
#endif
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
    // Keep the gate staging past LEDSTRIP_ARM_TIMEOUT_MS at slow announcer rates.
    if (ledStrip && (currentTimeMs - ledRearmMs) > RACE_LED_REARM_MS) {
        ledStrip->arm(currentTimeMs);
        ledRearmMs = currentTimeMs;
    }
#endif
    uint32_t elapsed = currentTimeMs - armStartMs;
    if (tonesPlayed < RACE_STAGE_TONES &&
        elapsed >= speechMs + (uint32_t)tonesPlayed * RACE_STAGE_TONE_INTERVAL_MS) {
        ++tonesPlayed;
        if (conf->getStartTone() == START_TONE_BUZZER) buz->beep(STAGE_TONE_MS);
        emit("raceTone", RACE_STAGE_TONES - tonesPlayed);
    }
    if (elapsed >= armDurationMs) {
        arming = false;
        timer->start();  // plays the start tone on the buzzer
        emit("raceStart", 0);
    }
}
