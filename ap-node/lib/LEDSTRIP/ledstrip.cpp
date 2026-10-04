#include "ledstrip.h"

#include "debug.h"

static const uint32_t COLOR_OFF = 0;

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
    armRequested = true;
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

void LedStrip::renderRunning(const ledstrip_inputs_t &in) {
    if (in.rssi >= in.enterRssi) {
        fill(Adafruit_NeoPixel::Color(255, 255, 255));  // drone in the gate
    } else {
        fill(Adafruit_NeoPixel::Color(0, 200, 0));
    }
}

void LedStrip::renderLapFlash(uint32_t elapsed) {
    // Three white chase segments sweeping around the ring over the flash.
    fill(Adafruit_NeoPixel::Color(0, 120, 0));
    if (count == 0) return;
    uint16_t seg = count / 6 + 1;
    uint16_t head = (uint16_t)((uint32_t)count * elapsed / LEDSTRIP_LAP_FLASH_MS);
    for (uint16_t k = 0; k < 3; ++k) {
        uint16_t base = (uint16_t)(head + k * count / 3);
        for (uint16_t i = 0; i < seg; ++i)
            strip.setPixelColor((base + i) % count, Adafruit_NeoPixel::Color(255, 255, 255));
    }
}

void LedStrip::handleLedStrip(uint32_t now, const ledstrip_inputs_t &in) {
    if (!ready || (now - lastFrameMs) < LEDSTRIP_FRAME_MS) return;
    lastFrameMs = now;

    // Track state transitions even while dark, so re-enabling shows the
    // right thing immediately.
    if (armRequested) {
        armRequested = false;
        arming = true;
    }
    if (arming && (in.running || (now - armTimeMs) > LEDSTRIP_ARM_TIMEOUT_MS)) {
        arming = false;
    }
    if (in.running && !wasRunning) {
        startFlashMs = now;
        lastLapSerial = in.lapSerial;
    }
    if (!in.running) startFlashMs = 0;
    wasRunning = in.running;
    if (in.lapSerial != lastLapSerial) {
        lastLapSerial = in.lapSerial;
        lapFlashMs = now;
    }

    if (!enabled || count == 0) {
        if (!wasDark) {
            strip.clear();
            show();
            wasDark = true;
        }
        return;
    }
    wasDark = false;

    // Adafruit_NeoPixel applies brightness when pixels are set, so choose it
    // before rendering the frame.
    bool startFlash = startFlashMs && (now - startFlashMs) < LEDSTRIP_START_FLASH_MS;
    uint16_t level = startFlash ? (uint16_t)brightness * 2u : brightness;
    strip.setBrightness(level > 255u ? 255u : (uint8_t)level);

    if (in.rfFault) {
        fill(((now / 500) & 1) ? Adafruit_NeoPixel::Color(160, 0, 255) : COLOR_OFF);
    } else if (lapFlashMs && (now - lapFlashMs) < LEDSTRIP_LAP_FLASH_MS) {
        renderLapFlash(now - lapFlashMs);
    } else if (startFlash) {
        fill(Adafruit_NeoPixel::Color(0, 255, 0));
    } else if (arming) {
        fill(Adafruit_NeoPixel::Color(wave(now, 600, 60, 255), 0, 0));
    } else if (in.running) {
        renderRunning(in);
    } else {
        renderIdle(now, in);
    }

    if (in.batteryLow && (now % 2000) < 200) {
        fill(Adafruit_NeoPixel::Color(255, 120, 0));
    }

    show();
}
