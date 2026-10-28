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
#ifdef C5VRX_LINK
static RaceControl race;
// About 18 KB (the lap trace). Written on the loop task only; parallelTask
// and the web task just read it.
static cal_t cal;
static bool calSaved = false;

// Start/stop requests come from the web task but run here, on the loop task,
// so cal_start never races cal_push.
static void handleCalibration() {
    uint8_t startGain, laps;
    if (ws.takeCalibStop()) cal_abort(&cal);
    if (ws.takeCalibStart(&startGain, &laps) && race.isIdle()) {
        cal_start(&cal, millis(), startGain, laps, config.getMinLapMs());
        calSaved = false;
    }
    if (cal.state == CAL_DONE && !calSaved) {
        calSaved = true;
        config.setRfGain(cal.gain);
        config.setEnterRssi(cal.enter);
        config.setExitRssi(cal.exit);
        DEBUG("Calibrated: gain %u enter %u exit %u separation %u\n", cal.gain, cal.enter, cal.exit,
              cal.separation);
    }
}
#endif
#ifdef ELRS_BACKPACK
static ElrsBackpack elrs;

// Short press starts the countdown, long press stops or cancels it.
static void handleRadioButton(uint32_t currentTimeMs) {
    elrs_press_e press = elrs.handle(currentTimeMs, config.getElrsEnabled(), config.getElrsMac());
    elrs.debugStats(currentTimeMs);
    if (press == ELRS_PRESS_SHORT && race.isIdle() && !cal_active(&cal)) {
        buzzer.beep(60);
        race.requestBegin();
    } else if (press == ELRS_PRESS_LONG && !race.isIdle()) {
        buzzer.beep(60);
        race.requestStop();
    }
}
#endif
#ifdef PIN_SD_CS
static SdLog sdlog;
#endif
#ifdef PIN_LED_STRIP
static LedStrip ledStrip;

static void handleGateLights(uint32_t currentTimeMs) {
    ledStrip.configure(config.getLedEnabled(), config.getLedCount(), config.getLedBrightness(), config.getLedType(), config.getLedOrder());
    ledstrip_inputs_t in = {};
    in.running = timer.isRunning();
    in.rssi = timer.getRssi();
    in.enterRssi = config.getEnterRssi();
    in.exitRssi = config.getExitRssi();
#ifdef C5VRX_LINK
    in.rfFault = !rx.healthy(currentTimeMs);
#endif
    in.batteryLow = monitor.isAlarming();
    ledStrip.handleLedStrip(currentTimeMs, in);
}
#endif

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
        // The calibrator owns the gain while it runs.
        rx.handleGainChange(currentTimeMs, cal_active(&cal) ? cal.gain : config.getRfGain());
        rx.debugStats(currentTimeMs);
#endif
#ifdef ELRS_BACKPACK
        handleRadioButton(currentTimeMs);
#endif
#ifdef C5VRX_LINK
        race.handleRaceControl(currentTimeMs);
#endif
        monitor.checkBatteryState(currentTimeMs, config.getAlarmThreshold());
#ifdef PIN_SD_CS
        sdlog.setEnabled(config.getSdLog());
#endif
#ifdef PIN_LED_STRIP
        handleGateLights(currentTimeMs);
#endif
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
#ifdef ELRS_BACKPACK
    // esp_now_init(), esp_wifi_set_mac() and the ELRS stats printf run on this task.
    xTaskCreatePinnedToCore(parallelTask, "parallelTask", 6144, NULL, 0, &xTimerTask, 0);
#else
    xTaskCreatePinnedToCore(parallelTask, "parallelTask", 3000, NULL, 0, &xTimerTask, 0);
#endif
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
    race.init(&config, &timer, &buzzer);
    ws.setRaceControl(&race);
    ws.setCalibrator(&cal);
#endif
#ifdef PIN_LED_STRIP
    ledStrip.init(PIN_LED_STRIP);
    ws.setLedStrip(&ledStrip);
#ifdef C5VRX_LINK
    race.setLedStrip(&ledStrip);
#endif
#endif
#ifdef ELRS_BACKPACK
    ws.setElrsBackpack(&elrs);
#endif
#ifdef PIN_SD_CS
    sdlog.init(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    ws.setSdLog(&sdlog);
#endif
    led.on(400);
    buzzer.beep(200);
    initParallelTask();
}

void loop() {
#ifdef C5VRX_LINK
    // One lap timer update per 1 kHz RF node sample, not per loop pass.
    while (rx.poll()) {
        uint32_t currentTimeMs = millis();
        timer.handleLapTimerUpdate(currentTimeMs);
        if (cal_active(&cal)) cal_push(&cal, currentTimeMs, timer.getRssi(), rx.readClip(), rx.gainApplied(cal.gain));
        ws.pushRssiSample(currentTimeMs, rx.readRssi(), timer.getRssi());
#ifdef PIN_SD_CS
        sdlog.push(currentTimeMs, rx.readRssi(), timer.getRssi(), timer.isRunning(), timer.getLapSerial(),
                   timer.peekLapTime());
#endif
    }
    handleCalibration();
    ElegantOTA.loop();
    delay(1);
#else
    uint32_t currentTimeMs = millis();
    timer.handleLapTimerUpdate(currentTimeMs);
    ElegantOTA.loop();
#endif
}
