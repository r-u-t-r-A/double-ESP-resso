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
//Only 5V, GND, D6 and D7 are joined; the RF node drives its IQ bus on D0-D3/D10.
#if defined(C5VRX_LINK)

#define PIN_LED 27            //XIAO user LED, active low
#define LED_INVERTED true
#define PIN_VBAT 6            //XIAO BAT_VOLT_PIN (divider enabled by PIN_VBAT_EN)
#define PIN_VBAT_EN 26
#define VBAT_SCALE 2
#define VBAT_ADD 2
#define PIN_C5LINK_RX 11      //D6 <- RF node TX
#define PIN_C5LINK_TX 12      //D7 -> RF node RX
#define PIN_BUZZER 8          //D8
#define BUZZER_INVERTED false
#define DEFAULT_FREQUENCY 5658 //R1
#define DEFAULT_RF_GAIN 40
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

#define EEPROM_RESERVED_SIZE 256
#define CONFIG_JSON_SIZE 384
#define CONFIG_MAGIC_MASK (0b11U << 30)
#define CONFIG_MAGIC (0b01U << 30)
#define CONFIG_VERSION 1U

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
    char* getSsid();
    char* getPassword();

   private:
    laptimer_config_t conf;
    bool modified;
    volatile uint32_t checkTimeMs = 0;
    void setDefaults();
};
