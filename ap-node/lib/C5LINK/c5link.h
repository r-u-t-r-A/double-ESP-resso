#pragma once
/*
 * c5link.h - RSSI source backed by the double-ESP-resso RF node over UART.
 *
 * Drop-in replacement for the RX5808 class as far as LapTimer is concerned
 * (readRssi / handleFrequencyChange), plus gain control, a non-blocking
 * poll() that returns true once per new 1 kHz sample, and link status.
 */
#include <Arduino.h>
#include <stdint.h>

#include "c5link_proto.h"

#define C5LINK_RESEND_MS 250       // re-send F/G while the RF node disagrees
#define C5LINK_ERR_BACKOFF_MS 2000 // slower retries after ERR_FREQ/ERR_RF
#define C5LINK_TIMEOUT_MS 2500     // no status for this long = link down

class C5Link {
   public:
    C5Link(uint8_t rxPin, uint8_t txPin);
    void init();

    // Parses pending UART bytes; true when a new RSSI sample was consumed.
    // Call repeatedly until it returns false.
    bool poll();
    uint8_t readRssi();

    // Called periodically with the desired settings from Config.
    void handleFrequencyChange(uint32_t currentTimeMs, uint16_t freqMhz);
    void handleGainChange(uint32_t currentTimeMs, uint8_t gain);

    bool linkUp(uint32_t currentTimeMs);
    // Link up and the RF node measuring (OK or briefly TUNING).
    bool healthy(uint32_t currentTimeMs) {
        return linkUp(currentTimeMs) && (rfState == C5LINK_ST_OK || rfState == C5LINK_ST_TUNING);
    }
    void statusString(char *buf, size_t len, uint32_t currentTimeMs);

   private:
    uint8_t rxPin;
    uint8_t txPin;
    c5link_rx_t rx;

    volatile uint8_t rssi = 0;
    bool haveSeq = false;
    uint8_t lastSeq = 0;
    volatile uint32_t samples = 0;
    volatile uint32_t dropped = 0;

    volatile bool statusSeen = false;
    volatile uint32_t statusTimeMs = 0;
    volatile uint16_t rfMhz = 0;
    volatile uint8_t rfGain = 0;
    volatile c5link_state_t rfState = C5LINK_ST_OK;
    volatile uint16_t rfVersion = 0;

    uint16_t wantMhz = 0;
    uint32_t freqSentMs = 0;
    uint32_t gainSentMs = 0;

    void send(const c5link_msg_t &msg);
};
