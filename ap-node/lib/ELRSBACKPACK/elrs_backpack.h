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
    bool wasStarted();
    // Call from parallelTask: (re)starts ESP-NOW when the config changes and
    // returns the press, if any, that completed by now.
    elrs_press_e handle(uint32_t currentTimeMs, bool enabled, const uint8_t mac[ELRS_UID_LEN]);

    void onReceive(const uint8_t *src, const uint8_t *data, int len);
    void onSniff(const uint8_t *dst, const uint8_t *src, int8_t rssi, uint8_t channel);

    // Diagnostics for /status and the serial log.
    void statusString(char *buf, size_t len, uint32_t currentTimeMs);
    void debugStats(uint32_t currentTimeMs);

   private:
    bool start(const uint8_t mac[ELRS_UID_LEN]);
    void stop();

    struct Stats {
        // Every ESP-NOW frame on the channel, seen by the promiscuous sniffer.
        uint32_t air;
        uint8_t airSrc[ELRS_UID_LEN];
        uint8_t airDst[ELRS_UID_LEN];
        int8_t airRssi;
        uint8_t airChannel;
        // Frames ESP-NOW delivered to us, then the ones that passed each check.
        uint32_t rx;
        uint32_t rxFromUid;
        uint32_t rxMsp;
        uint32_t rxRecording;
        uint8_t rxSrc[ELRS_UID_LEN];
        uint16_t lastFunction;
        bool lastOn;
        uint32_t lastRecordingMs;
        uint32_t shortPresses;
        uint32_t longPresses;
        int protocolErr;
        int macErr;
        int espNowErr;
    };
    Stats stats = {};
    uint32_t statsMs = 0;

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
