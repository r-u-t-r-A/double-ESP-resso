#include "debug.h"
#include "led.h"
#include "webserver.h"
#include <ElegantOTA.h>

#ifdef C5VRX_LINK
static C5Link rx(PIN_C5LINK_RX, PIN_C5LINK_TX);
#else
static RX5808 rx(PIN_RX5808_RSSI, PIN_RX5808_DATA, PIN_RX5808_SELECT, PIN_RX5808_CLOCK);
#endif
static Config config;
static Webserver ws;
static Buzzer buzzer;
static Led led;
static LapTimer timer;
static BatteryMonitor monitor;

static TaskHandle_t xTimerTask = NULL;

static void parallelTask(void *pvArgs) {
    for (;;) {
        uint32_t currentTimeMs = millis();
        buzzer.handleBuzzer(currentTimeMs);
        led.handleLed(currentTimeMs);
        ws.handleWebUpdate(currentTimeMs);
        config.handleEeprom(currentTimeMs);
        rx.handleFrequencyChange(currentTimeMs, config.getFrequency());
#ifdef C5VRX_LINK
        rx.handleGainChange(currentTimeMs, config.getRfGain());
#endif
        monitor.checkBatteryState(currentTimeMs, config.getAlarmThreshold());
        buzzer.handleBuzzer(currentTimeMs);
        led.handleLed(currentTimeMs);
#ifdef C5VRX_LINK
        // Single-core C5: let the lap timer loop and the idle task run.
        vTaskDelay(1);
#endif
    }
}

static void initParallelTask() {
    disableCore0WDT();
    xTaskCreatePinnedToCore(parallelTask, "parallelTask", 3000, NULL, 0, &xTimerTask, 0);
}

void setup() {
    DEBUG_INIT;
    config.init();
    rx.init();
    buzzer.init(PIN_BUZZER, BUZZER_INVERTED);
    led.init(PIN_LED, LED_INVERTED);
    timer.init(&config, &rx, &buzzer, &led);
#ifdef PIN_VBAT_EN
    pinMode(PIN_VBAT_EN, OUTPUT);
    digitalWrite(PIN_VBAT_EN, HIGH);
#endif
    monitor.init(PIN_VBAT, VBAT_SCALE, VBAT_ADD, &buzzer, &led);
    ws.init(&config, &timer, &monitor, &buzzer, &led);
#ifdef C5VRX_LINK
    ws.setRssiSource(&rx);
#endif
    led.on(400);
    buzzer.beep(200);
    initParallelTask();
}

void loop() {
#ifdef C5VRX_LINK
    // One lap timer update per 1 kHz RF node sample, not per loop pass.
    while (rx.poll()) {
        timer.handleLapTimerUpdate(millis());
    }
    ElegantOTA.loop();
    delay(1);
#else
    uint32_t currentTimeMs = millis();
    timer.handleLapTimerUpdate(currentTimeMs);
    ElegantOTA.loop();
#endif
}
