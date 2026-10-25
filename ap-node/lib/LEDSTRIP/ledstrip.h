#pragma once
/*
 * ledstrip.h - WS2812B / WS2812 / WS2811 start-gate lighting driven by the
 * lap timer state.
 *
 * Rendered from the low-priority parallel task at ~50 Hz. Adafruit NeoPixel
 * only holds the pixel buffer (wire byte order, brightness applied); frames
 * are sent by our own RMT encoder with per-chip timing from ledstrip_timing.h,
 * because the library's IDF 5 backend ignores the 400 kHz flag and uses one
 * RMT memory block, which Wi-Fi interrupt latency can underrun.
 *
 * Race events use RotorHazard's default LED effects and colours; the latest
 * event replaces the previous one, as in RotorHazard. Priority, highest first:
 *   disabled                   -> dark
 *   RF link down / RF error    -> purple blink
 *   power-up                   -> rainbow cycle (RH "Server Startup")
 *   race stop                  -> solid red for 4 s (RH "Race Stop")
 *   race start                 -> solid green for 4 s (RH "Race Start")
 *   gate entry while racing    -> seat-1 blue sparkle over green (RH "Gate Entrance")
 *   staging                    -> orange, two on / one off (RH "Race Staging")
 *   running                    -> green
 *   idle                       -> breathing blue plus an RSSI level meter
 *   low battery                -> short amber blip every 2 s over any state
 */
#include <Adafruit_NeoPixel.h>
#include <esp32-hal-rmt.h>
#include <stdint.h>

#include "ledstrip_timing.h"

#define LEDSTRIP_MAX_LEDS 300
#define LEDSTRIP_FRAME_MS 20
#define LEDSTRIP_ARM_TIMEOUT_MS 15000
#define LEDSTRIP_STARTUP_MS 5000
#define LEDSTRIP_EVENT_MS 4000         // RH "Solid" effect hold (start, stop)
#define LEDSTRIP_RAINBOW_STEP_MS 10    // one colour-wheel step
#define LEDSTRIP_SPARKLE_STEP_MS 100   // RH sparkle defaults: 100 steps of 100 ms,
#define LEDSTRIP_SPARKLE_STEPS 100     // ~1 new pixel per step, 5 % decay per step
#define LEDSTRIP_SPARKLE_DECAY_PCT 95

typedef struct {
    bool running;           // LapTimer RUNNING
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
    // chip is a ledstrip_chip_e, order a ledstrip_order_e.
    void configure(bool enabled, uint16_t count, uint8_t brightness, uint8_t chip, uint8_t order);
    // Race staging: orange until the start, a stop, or a timeout.
    void arm(uint32_t currentTimeMs);
    // Race stopped or staging cancelled: red, then idle.
    void stop();
    void handleLedStrip(uint32_t currentTimeMs, const ledstrip_inputs_t &in);

   private:
    Adafruit_NeoPixel strip;
    bool ready = false;
    bool enabled = false;
    uint16_t count = 0;
    uint8_t brightness = 0;
    uint8_t chip = LEDSTRIP_WS2812B;
    uint8_t order = LEDSTRIP_GRB;
    bool wasDark = false;

    uint8_t pin = 0;
    rmt_data_t *symbols = nullptr;
    uint32_t symbolCapacity = 0;
    uint32_t lastShowUs = 0;

    typedef enum {
        FX_NONE,
        FX_STARTUP,
        FX_START,
        FX_STOP,
        FX_SPARKLE,
    } effect_e;

    volatile bool armRequested = false;
    volatile bool stopRequested = false;
    volatile uint32_t armTimeMs = 0;
    bool arming = false;
    bool wasRunning = false;
    bool inGate = false;
    bool startupDone = false;
    effect_e effect = FX_NONE;
    uint32_t effectMs = 0;
    uint32_t lastFrameMs = 0;
    uint8_t sparkle[LEDSTRIP_MAX_LEDS];  // per-pixel sparkle level
    uint16_t sparkleSteps = 0;
    uint32_t sparkleStepMs = 0;

    void show();
    void fill(uint32_t color);
    void startEffect(effect_e fx, uint32_t now);
    bool effectActive(uint32_t now);
    void stepSparkle(uint32_t now);
    void renderIdle(uint32_t now, const ledstrip_inputs_t &in);
    void renderStaging();
    void renderRainbow(uint32_t now);
    void renderSparkle();
    static uint32_t colorWheel(uint8_t pos);
    static uint8_t wave(uint32_t now, uint32_t periodMs, uint8_t lo, uint8_t hi);
};
