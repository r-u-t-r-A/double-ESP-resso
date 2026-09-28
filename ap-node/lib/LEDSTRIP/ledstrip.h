#pragma once
/*
 * ledstrip.h - WS2812B start-gate lighting driven by the lap timer state.
 *
 * Rendered from the low-priority parallel task at ~50 Hz. Frames go out over
 * RMT (Adafruit NeoPixel), so the lap timer loop is never paced by the strip.
 *
 * Priority, highest first:
 *   disabled                   -> dark
 *   RF link down / RF error    -> purple blink
 *   lap just recorded          -> white chase flash
 *   race start                 -> bright green flash, then running
 *   arming (web countdown)     -> pulsing red
 *   running                    -> green, white while the drone is crossing
 *   idle                       -> breathing blue plus an RSSI level meter
 *   low battery                -> short amber blip every 2 s over any state
 */
#include <Adafruit_NeoPixel.h>
#include <stdint.h>

#define LEDSTRIP_MAX_LEDS 300
#define LEDSTRIP_FRAME_MS 20
#define LEDSTRIP_ARM_TIMEOUT_MS 15000
#define LEDSTRIP_START_FLASH_MS 1000
#define LEDSTRIP_LAP_FLASH_MS 600

typedef struct {
    bool running;           // LapTimer RUNNING
    uint32_t lapSerial;     // increments on every recorded lap
    uint8_t rssi;           // filtered RSSI 0-255
    uint8_t enterRssi;
    uint8_t exitRssi;
    bool rfFault;           // RF link down or RF node in an error state
    bool batteryLow;
} ledstrip_inputs_t;

class LedStrip {
   public:
    void init(uint8_t pin);
    // Applies config; safe to call every frame (cheap when unchanged).
    void configure(bool enabled, uint16_t count, uint8_t brightness);
    // Web countdown started ("Arm your quad"): red until start or timeout.
    void arm(uint32_t currentTimeMs);
    void handleLedStrip(uint32_t currentTimeMs, const ledstrip_inputs_t &in);

   private:
    Adafruit_NeoPixel strip;
    bool ready = false;
    bool enabled = false;
    uint16_t count = 0;
    uint8_t brightness = 0;
    bool wasDark = false;

    volatile bool armRequested = false;
    volatile uint32_t armTimeMs = 0;
    bool arming = false;
    bool wasRunning = false;
    uint32_t startFlashMs = 0;
    uint32_t lastLapSerial = 0;
    uint32_t lapFlashMs = 0;
    uint32_t lastFrameMs = 0;

    void fill(uint32_t color);
    void renderIdle(uint32_t now, const ledstrip_inputs_t &in);
    void renderRunning(const ledstrip_inputs_t &in);
    void renderLapFlash(uint32_t elapsed);
    static uint8_t wave(uint32_t now, uint32_t periodMs, uint8_t lo, uint8_t hi);
};
