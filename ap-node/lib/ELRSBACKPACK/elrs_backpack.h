#pragma once

#include <Arduino.h>

#include "elrs_msp.h"

#define ELRS_EDGE_QUEUE_LEN 8
#define ELRS_RETRY_MS 5000u

// Hash a bind phrase the way the ExpressLRS configurator does and return the
// MAC the TX backpack sends to.
void elrsMacFromPhrase(const char *phrase, uint8_t mac[ELRS_UID_LEN]);

// Receives the ExpressLRS TX backpack's ESP-NOW frames and turns the DVR Rec
// switch edges into short/long presses. ESP-NOW needs the STA interface up on
// Wi-Fi channel 1, so the web server only marks the radio ready in AP mode.
class ElrsBackpack {
   public:
    void setRadioReady(bool ready) { radioReady = ready; }
    bool isActive() { return active; }
    // Call from parallelTask: (re)starts ESP-NOW when the config changes and
    // returns the press, if any, that completed by now.
    elrs_press_e handle(uint32_t currentTimeMs, bool enabled, const uint8_t mac[ELRS_UID_LEN]);

    void onReceive(const uint8_t *src, const uint8_t *data, int len);

   private:
    bool start(const uint8_t mac[ELRS_UID_LEN]);
    void stop();

    volatile bool radioReady = false;
    bool active = false;
    uint32_t lastStartMs = 0 - ELRS_RETRY_MS - 1;
    uint8_t activeMac[ELRS_UID_LEN] = {0};
    elrs_press_t press = {};

    struct Edge {
        uint32_t ms;
        bool on;
    };
    Edge edges[ELRS_EDGE_QUEUE_LEN];
    volatile uint8_t edgeHead = 0;
    volatile uint8_t edgeTail = 0;
    portMUX_TYPE edgeMux = portMUX_INITIALIZER_UNLOCKED;
};
