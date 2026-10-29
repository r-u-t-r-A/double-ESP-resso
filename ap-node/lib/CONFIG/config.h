#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <stdint.h>

/*
## Pinout ##
| ESP32 | RX5880 |
| :------------- |:-------------|
| 33 | RSSI |
| GND | GND |
| 19 | CH1 |
| 22 | CH2 |
| 23 | CH3 |
| 3V3 | +5V |

* **Led** goes to pin 21 and GND
* The optional **Buzzer** goes to pin 25 or 27 and GND

*/

//double-ESP-resso: Seeed XIAO ESP32-C5 stacked on the RF node XIAO.
//Only 5V, GND, D1 and D5 are joined between the boards. D8-D10 are the XIAO's
//SPI pins and carry the SD card. D6/D7 (GPIO11/12) are the C5 UART0 console
//pins: the core's log output is driven out on GPIO11, so they cannot carry the
//link. D2/D3 are strapping pins.
#if defined(C5VRX_LINK)

#define PIN_LED 27            //XIAO user LED, active low
#define LED_INVERTED true
#define PIN_VBAT 6            //XIAO BAT_VOLT_PIN (divider enabled by PIN_VBAT_EN)
#define PIN_VBAT_EN 26
#define VBAT_SCALE 2
#define VBAT_ADD 2
#define PIN_C5LINK_RX 24      //D5 <- RF node TX
#define PIN_C5LINK_TX 0       //D1 -> RF node RX
#define PIN_BUZZER 1          //D0
#define PIN_LED_STRIP 25      //D2, WS2812B data (330R series resistor)
#define PIN_SD_SCK 8          //D8, microSD over SPI (XIAO SCK)
#define PIN_SD_MISO 9         //D9 (XIAO MISO)
#define PIN_SD_MOSI 10        //D10 (XIAO MOSI)
#define PIN_SD_CS 23          //D4: the XIAO's SS (D3) is a strapping pin
#define BUZZER_INVERTED false
#define DEFAULT_FREQUENCY 5658 //R1
#define DEFAULT_RF_GAIN 35
#define C5VRX_AP_TX_POWER WIFI_POWER_11dBm

//ESP23-C3
#elif defined(ESP32C3)

#define PIN_LED 1
#define PIN_VBAT 0
#define VBAT_SCALE 2
#define VBAT_ADD 2
#define PIN_RX5808_RSSI 3
#define PIN_RX5808_DATA 6     //CH1
#define PIN_RX5808_SELECT 7   //CH2
#define PIN_RX5808_CLOCK 4    //CH3
#define PIN_BUZZER 5
#define BUZZER_INVERTED false

//ESP32-S3
#elif defined(ESP32S3)

#define PIN_LED 2
#define PIN_VBAT 1
#define VBAT_SCALE 2
#define VBAT_ADD 2
#define PIN_RX5808_RSSI 13
#define PIN_RX5808_DATA 11     //CH1
#define PIN_RX5808_SELECT 10   //CH2
#define PIN_RX5808_CLOCK 12    //CH3
#define PIN_BUZZER 3
#define BUZZER_INVERTED false

//ESP32
#else

#define PIN_LED 21
#define PIN_VBAT 35
#define VBAT_SCALE 2
#define VBAT_ADD 2
#define PIN_RX5808_RSSI 33
#define PIN_RX5808_DATA 19   //CH1
#define PIN_RX5808_SELECT 22 //CH2
#define PIN_RX5808_CLOCK 23  //CH3
#define PIN_BUZZER 27
#define BUZZER_INVERTED false

#endif

#ifndef LED_INVERTED
#define LED_INVERTED false
#endif
#ifndef DEFAULT_FREQUENCY
#define DEFAULT_FREQUENCY 1111
#endif
#ifndef DEFAULT_RF_GAIN
#define DEFAULT_RF_GAIN 40
#endif
#define DEFAULT_LED_COUNT 60
#define DEFAULT_LED_BRIGHTNESS 80  // of 255; caps strip current
#define LED_COUNT_MAX 300

#define EEPROM_RESERVED_SIZE 256
#define CONFIG_JSON_SIZE 640
#define CONFIG_MAGIC_MASK (0b11U << 30)
#define CONFIG_MAGIC (0b01U << 30)
#define CONFIG_VERSION 7U
#define ELRS_MAC_LEN 6

// Who plays the race start tone (C5VRX_LINK builds). The browser's Web Audio
// tone arrives later than the AP node's buzzer, so play only one of them.
#define START_TONE_BUZZER 0
#define START_TONE_BROWSER 1
// What a short press of the ELRS radio button starts (ELRS_BACKPACK builds).
#define ELRS_ACTION_RACE 0       // the race countdown
#define ELRS_ACTION_CALIBRATE 1  // auto-calibration
// RotorHazard's tone lengths: each staging tone, and the start tone.
#define STAGE_TONE_MS 100
#define START_TONE_MS 700

#define EEPROM_CHECK_TIME_MS 1000

typedef struct {
    uint32_t version;
    uint16_t frequency;
    uint8_t minLap;
    uint8_t alarm;
    uint8_t announcerType;
    uint8_t announcerRate;
    uint8_t enterRssi;
    uint8_t exitRssi;
    char pilotName[21];
    char ssid[33];
    char password[33];
    uint8_t rfGain;  // RF node fixed RX gain index (C5VRX_LINK builds)
    uint8_t ledEnabled;     // WS2812B gate strip (PIN_LED_STRIP builds)
    uint8_t ledBrightness;
    uint16_t ledCount;
    // v3: appended so a v2 image keeps every field above.
    uint8_t elrsMac[ELRS_MAC_LEN];  // ELRS backpack UID as sent to (ELRS_BACKPACK builds)
    uint8_t elrsEnabled;
    // v4: LED chip (ledstrip_chip_e) and wire colour order (ledstrip_order_e).
    uint8_t ledType;
    uint8_t ledOrder;
    // v5: START_TONE_BUZZER or START_TONE_BROWSER.
    uint8_t startTone;
    // v6: log 1 kHz RSSI to the SD card (PIN_SD_CS builds).
    uint8_t sdLog;
    // v7: ELRS_ACTION_RACE or ELRS_ACTION_CALIBRATE (ELRS_BACKPACK builds).
    uint8_t elrsAction;
} laptimer_config_t;

class Config {
   public:
    void init();
    void load();
    void write();
    void toJson(AsyncResponseStream& destination);
    void toJsonString(char* buf);
    void fromJson(JsonObject source);
    void handleEeprom(uint32_t currentTimeMs);

    // getters and setters
    uint16_t getFrequency();
    uint32_t getMinLapMs();
    uint8_t getAlarmThreshold();
    uint8_t getEnterRssi();
    uint8_t getExitRssi();
    uint8_t getRfGain();
    bool getLedEnabled();
    uint16_t getLedCount();
    uint8_t getLedBrightness();
    uint8_t getLedType();
    uint8_t getLedOrder();
    uint8_t getAnnouncerRate();
    uint8_t getStartTone();
    bool getSdLog();
    bool getElrsEnabled();
    uint8_t getElrsAction();
    const uint8_t* getElrsMac();
    char* getSsid();
    char* getPassword();
#ifdef C5VRX_LINK
    // Auto-calibration results; saved by handleEeprom like a web UI change.
    void setEnterRssi(uint8_t rssi);
    void setExitRssi(uint8_t rssi);
    void setRfGain(uint8_t gain);
#endif

   private:
    laptimer_config_t conf;
    bool modified;
    volatile uint32_t checkTimeMs = 0;
    void setDefaults();
#ifdef ELRS_BACKPACK
    void formatElrsMac(char* buf);  // 18 bytes, "" when unset
#endif
};
