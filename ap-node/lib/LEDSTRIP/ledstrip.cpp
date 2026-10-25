#include "ledstrip.h"

#include "debug.h"

static const uint32_t COLOR_OFF = 0;
// RotorHazard ColorVal and seat colour values.
static const uint32_t RH_ORANGE = 0xFF8000;  // race staging
static const uint32_t RH_GREEN = 0x00FF00;   // race start
static const uint32_t RH_RED = 0xFF0000;     // race stop
static const uint32_t RH_SEAT1 = 0x0022FF;   // seat 1 (blue), gate entrance
static const uint32_t COLOR_RUNNING = 0x00C800;

// Adafruit buffer layout for each ledstrip_order_e.
static const neoPixelType NEO_ORDERS[LEDSTRIP_ORDER_COUNT] = {NEO_GRB, NEO_RGB, NEO_BRG, NEO_RBG, NEO_GBR, NEO_BGR};

void LedStrip::init(uint8_t p) {
    pin = p;
    // Adafruit only renders into its buffer here; it never drives the pin.
    strip.updateType(NEO_GRB + NEO_KHZ800);
    strip.updateLength(0);
    // Two RMT memory blocks halve the refill interrupt rate during a frame.
    ready = rmtInit(pin, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_2, 1000000000u / LEDSTRIP_TICK_NS) ||
            rmtInit(pin, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 1000000000u / LEDSTRIP_TICK_NS);
    if (!ready) DEBUG("LED strip: RMT init on GPIO%u failed\n", pin);
}

void LedStrip::configure(bool en, uint16_t n, uint8_t bright, uint8_t chipType, uint8_t colorOrder) {
    if (n > LEDSTRIP_MAX_LEDS) n = LEDSTRIP_MAX_LEDS;
    if (!en) n = count;  // keep the length so the strip can be blanked
    if (chipType >= LEDSTRIP_CHIP_COUNT) chipType = LEDSTRIP_WS2812B;
    colorOrder = ledstrip_order_index(colorOrder);
    if (colorOrder != order) {
        order = colorOrder;
        strip.updateType(NEO_ORDERS[order] + NEO_KHZ800);
        DEBUG("LED strip: colour order %u\n", order);
    }
    if (chipType != chip) {
        chip = chipType;
        DEBUG("LED strip: chip %u\n", chip);
    }
    if (n != count) {
        // Blank the old length first so shortening leaves no stale pixels.
        strip.clear();
        show();
        strip.updateLength(n);
        count = n;
        DEBUG("LED strip: %u LEDs\n", count);
    }
    brightness = bright;
    enabled = en;
}

void LedStrip::show() {
    if (!ready || count == 0) return;
    const uint8_t *pixels = strip.getPixels();
    uint32_t bits = (uint32_t)count * 3u * 8u;
    if (bits > symbolCapacity) {
        rmt_data_t *grown = (rmt_data_t *)realloc(symbols, bits * sizeof(rmt_data_t));
        if (!grown) {
            DEBUG("LED strip: out of memory for %u LEDs\n", count);
            return;
        }
        symbols = grown;
        symbolCapacity = bits;
    }
    const ledstrip_timing_t *t = ledstrip_timing(chip);
    uint32_t i = 0;
    for (uint32_t b = 0; b < (uint32_t)count * 3u; ++b) {
        for (uint8_t mask = 0x80; mask; mask >>= 1) {
            bool one = pixels[b] & mask;
            symbols[i].level0 = 1;
            symbols[i].duration0 = one ? t->t1h : t->t0h;
            symbols[i].level1 = 0;
            symbols[i].duration1 = one ? t->t1l : t->t0l;
            ++i;
        }
    }
    // Back-to-back frames would be latched as one long frame.
    uint32_t since = micros() - lastShowUs;
    if (since < LEDSTRIP_RESET_US) delayMicroseconds(LEDSTRIP_RESET_US - since);
    rmtWrite(pin, symbols, bits, RMT_WAIT_FOR_EVER);
    lastShowUs = micros();
}

void LedStrip::arm(uint32_t currentTimeMs) {
    armTimeMs = currentTimeMs;
    stopRequested = false;
    armRequested = true;
}

void LedStrip::stop() {
    armRequested = false;
    stopRequested = true;
}

uint8_t LedStrip::wave(uint32_t now, uint32_t periodMs, uint8_t lo, uint8_t hi) {
    // Triangle wave, cheap and smooth enough for breathing effects.
    uint32_t phase = now % periodMs;
    uint32_t half = periodMs / 2;
    uint32_t t = phase < half ? phase : periodMs - phase;
    return lo + (uint8_t)((uint32_t)(hi - lo) * t / half);
}

void LedStrip::fill(uint32_t color) {
    strip.fill(color, 0, count);
}

void LedStrip::renderIdle(uint32_t now, const ledstrip_inputs_t &in) {
    uint8_t b = wave(now, 3000, 10, 60);
    fill(Adafruit_NeoPixel::Color(0, 0, b));

    // RSSI level meter: lights a growing arc from exitRssi towards
    // enterRssi, which makes threshold calibration visible at the gate.
    if (in.enterRssi > in.exitRssi && in.rssi > in.exitRssi) {
        uint32_t span = in.enterRssi - in.exitRssi;
        uint32_t level = in.rssi - in.exitRssi;
        if (level > span) level = span;
        uint16_t lit = (uint16_t)((uint32_t)count * level / span);
        bool over = in.rssi >= in.enterRssi;
        uint32_t c = over ? Adafruit_NeoPixel::Color(255, 255, 255)
                          : Adafruit_NeoPixel::Color(0, 180, 255);
        strip.fill(c, 0, lit);
    }
}

void LedStrip::renderStaging() {
    // RH "Pattern 2-1": two pixels on, one off, around the ring.
    for (uint16_t i = 0; i < count; ++i) strip.setPixelColor(i, (i % 3) < 2 ? RH_ORANGE : COLOR_OFF);
}

uint32_t LedStrip::colorWheel(uint8_t pos) {
    if (pos < 85) return Adafruit_NeoPixel::Color(pos * 3, 255 - pos * 3, 0);
    if (pos < 170) {
        pos -= 85;
        return Adafruit_NeoPixel::Color(255 - pos * 3, 0, pos * 3);
    }
    pos -= 170;
    return Adafruit_NeoPixel::Color(0, pos * 3, 255 - pos * 3);
}

void LedStrip::renderRainbow(uint32_t now) {
    // RH "rainbowCycle": the whole wheel spread over the ring, rotating.
    uint8_t j = (uint8_t)(now / LEDSTRIP_RAINBOW_STEP_MS);
    for (uint16_t i = 0; i < count; ++i)
        strip.setPixelColor(i, colorWheel((uint8_t)(i * 256u / count + j)));
}

void LedStrip::stepSparkle(uint32_t now) {
    while ((now - sparkleStepMs) >= LEDSTRIP_SPARKLE_STEP_MS) {
        sparkleStepMs += LEDSTRIP_SPARKLE_STEP_MS;
        for (uint16_t i = 0; i < count; ++i) sparkle[i] = (uint8_t)(sparkle[i] * LEDSTRIP_SPARKLE_DECAY_PCT / 100u);
        if (sparkleSteps < LEDSTRIP_SPARKLE_STEPS) {
            ++sparkleSteps;
            // Each pixel lights with chance 1/count: about one new one per step.
            for (uint16_t i = 0; i < count; ++i)
                if (random(count) == 0) sparkle[i] = 255;
        }
    }
}

void LedStrip::renderSparkle() {
    // Seat-colour sparkle added over the running green.
    fill(COLOR_RUNNING);
    for (uint16_t i = 0; i < count; ++i) {
        if (!sparkle[i]) continue;
        uint32_t l = sparkle[i];
        uint8_t g = (uint8_t)(((RH_SEAT1 >> 8) & 0xFF) * l / 255u);
        uint8_t b = (uint8_t)((RH_SEAT1 & 0xFF) * l / 255u);
        strip.setPixelColor(i, Adafruit_NeoPixel::Color(0, g > 200 ? g : 200, b));
    }
}

void LedStrip::startEffect(effect_e fx, uint32_t now) {
    effect = fx;
    effectMs = now;
    if (fx == FX_SPARKLE) {
        memset(sparkle, 0, sizeof(sparkle));
        sparkleSteps = 0;
        sparkleStepMs = now;
    }
}

bool LedStrip::effectActive(uint32_t now) {
    switch (effect) {
        case FX_STARTUP:
            return (now - effectMs) < LEDSTRIP_STARTUP_MS;
        case FX_START:
        case FX_STOP:
            return (now - effectMs) < LEDSTRIP_EVENT_MS;
        case FX_SPARKLE: {
            if (sparkleSteps < LEDSTRIP_SPARKLE_STEPS) return true;
            for (uint16_t i = 0; i < count; ++i)
                if (sparkle[i]) return true;
            return false;
        }
        default:
            return false;
    }
}

void LedStrip::handleLedStrip(uint32_t now, const ledstrip_inputs_t &in) {
    if (!ready || (now - lastFrameMs) < LEDSTRIP_FRAME_MS) return;
    lastFrameMs = now;

    // Track race events even while dark, so re-enabling shows the right
    // thing immediately. Each event replaces the current effect.
    if (!startupDone) {
        startupDone = true;
        startEffect(FX_STARTUP, now);
    }
    if (armRequested) {
        armRequested = false;
        arming = true;
        effect = FX_NONE;
    }
    if (stopRequested) {
        stopRequested = false;
        arming = false;
        startEffect(FX_STOP, now);
    }
    if (arming && (in.running || (now - armTimeMs) > LEDSTRIP_ARM_TIMEOUT_MS)) {
        arming = false;
    }
    if (in.running && !wasRunning) startEffect(FX_START, now);
    wasRunning = in.running;
    // Gate entrance: RSSI reaches Enter, re-armed once it falls below Exit.
    if (!in.running || in.rssi < in.exitRssi) {
        inGate = false;
    } else if (!inGate && in.rssi >= in.enterRssi) {
        inGate = true;
        startEffect(FX_SPARKLE, now);
    }
    if (effect == FX_SPARKLE) stepSparkle(now);
    if (effect != FX_NONE && !effectActive(now)) effect = FX_NONE;

    if (!enabled || count == 0) {
        if (!wasDark) {
            strip.clear();
            show();
            wasDark = true;
        }
        return;
    }
    wasDark = false;

    // Adafruit_NeoPixel applies brightness when pixels are set, so set it
    // before rendering the frame.
    strip.setBrightness(brightness);

    if (in.rfFault) {
        fill(((now / 500) & 1) ? Adafruit_NeoPixel::Color(160, 0, 255) : COLOR_OFF);
    } else if (effect == FX_STARTUP) {
        renderRainbow(now);
    } else if (effect == FX_STOP) {
        fill(RH_RED);
    } else if (effect == FX_START) {
        fill(RH_GREEN);
    } else if (effect == FX_SPARKLE) {
        renderSparkle();
    } else if (arming) {
        renderStaging();
    } else if (in.running) {
        fill(COLOR_RUNNING);
    } else {
        renderIdle(now, in);
    }

    if (in.batteryLow && (now % 2000) < 200) {
        fill(Adafruit_NeoPixel::Color(255, 120, 0));
    }

    show();
}
