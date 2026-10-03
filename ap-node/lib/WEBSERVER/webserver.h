#include <ESPAsyncWebServer.h>
#include <WiFi.h>

#include "battery.h"
#include "laptimer.h"
#ifdef PIN_LED_STRIP
#include "ledstrip.h"
#endif
#ifdef C5VRX_LINK
#include "racecontrol.h"
#endif
#ifdef ELRS_BACKPACK
#include "elrs_backpack.h"
#endif

#define WIFI_CONNECTION_TIMEOUT_MS 30000
#define WIFI_RECONNECT_TIMEOUT_MS 500
#define WEB_RSSI_SEND_TIMEOUT_MS 200
#define WIFI_AP_CHANNEL 1  // the ELRS backpack's fixed ESP-NOW channel

class Webserver {
   public:
    void init(Config *config, LapTimer *lapTimer, BatteryMonitor *batMonitor, Buzzer *buzzer, Led *l);
    void handleWebUpdate(uint32_t currentTimeMs);
#ifdef C5VRX_LINK
    void setRssiSource(RssiSource *source) { rssiSource = source; }
#endif
#ifdef PIN_LED_STRIP
    void setLedStrip(LedStrip *s) { ledStrip = s; }
#endif
#ifdef C5VRX_LINK
    void setRaceControl(RaceControl *r);
#endif
#ifdef ELRS_BACKPACK
    void setElrsBackpack(ElrsBackpack *b) { elrs = b; }
#endif

   private:
    void startServices();
    void sendRssiEvent(uint8_t rssi);
    void sendLaptimeEvent(uint32_t lapTime);

    Config *conf;
    LapTimer *timer;
    BatteryMonitor *monitor;
    Buzzer *buz;
    Led *led;
#ifdef C5VRX_LINK
    RssiSource *rssiSource = nullptr;
#endif
#ifdef PIN_LED_STRIP
    LedStrip *ledStrip = nullptr;
#endif
#ifdef C5VRX_LINK
    RaceControl *race = nullptr;
#endif
#ifdef ELRS_BACKPACK
    ElrsBackpack *elrs = nullptr;
#endif

    wifi_mode_t wifiMode = WIFI_OFF;
    wl_status_t lastStatus = WL_IDLE_STATUS;
    volatile wifi_mode_t changeMode = WIFI_OFF;
    volatile uint32_t changeTimeMs = 0;
    bool servicesStarted = false;
    bool wifiConnected = false;

    bool sendRssi = false;
    uint32_t rssiSentMs = 0;
};
