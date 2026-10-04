#include "config.h"

#include <EEPROM.h>

#include "debug.h"
#ifdef ELRS_BACKPACK
#include "elrs_backpack.h"
#endif
#ifdef PIN_LED_STRIP
#include "ledstrip_timing.h"
#endif

void Config::init(void) {
    if (sizeof(laptimer_config_t) > EEPROM_RESERVED_SIZE) {
        DEBUG("Config size too big, adjust reserved EEPROM size\n");
        return;
    }

    EEPROM.begin(EEPROM_RESERVED_SIZE);  // Size of EEPROM
    load();                              // Override default settings from EEPROM

    checkTimeMs = millis();

    DEBUG("EEPROM Init Successful\n");
}

void Config::load(void) {
    modified = false;
    EEPROM.get(0, conf);

    uint32_t version = 0xFFFFFFFF;
    if ((conf.version & CONFIG_MAGIC_MASK) == CONFIG_MAGIC) {
        version = conf.version & ~CONFIG_MAGIC_MASK;
    }

    // v2 -> v3 appended the ELRS fields, v3 -> v4 the LED chip and order.
    if (version == 2U) {
        memset(conf.elrsMac, 0, sizeof(conf.elrsMac));
        conf.elrsEnabled = 0;
        version = 3U;
        modified = true;
    }
    if (version == 3U) {
        conf.ledType = 0;   // WS2812B
        conf.ledOrder = 0;  // GRB
        version = 4U;
        modified = true;
    }
    if (version == CONFIG_VERSION) conf.version = CONFIG_VERSION | CONFIG_MAGIC;

    // If version is not current, reset to defaults
    if (version != CONFIG_VERSION) {
        setDefaults();
    }
}

void Config::write(void) {
    if (!modified) return;

    DEBUG("Writing to EEPROM\n");

    EEPROM.put(0, conf);
    EEPROM.commit();

    DEBUG("Writing to EEPROM done\n");

    modified = false;
}

void Config::toJson(AsyncResponseStream& destination) {
    // Use https://arduinojson.org/v6/assistant to estimate memory
    DynamicJsonDocument config(CONFIG_JSON_SIZE);
    config["freq"] = conf.frequency;
    config["minLap"] = conf.minLap;
    config["alarm"] = conf.alarm;
    config["anType"] = conf.announcerType;
    config["anRate"] = conf.announcerRate;
    config["enterRssi"] = conf.enterRssi;
    config["exitRssi"] = conf.exitRssi;
    config["name"] = conf.pilotName;
    config["ssid"] = conf.ssid;
    config["pwd"] = conf.password;
#ifdef C5VRX_LINK
    config["rfGain"] = conf.rfGain;
#endif
#ifdef PIN_LED_STRIP
    config["ledOn"] = conf.ledEnabled;
    config["ledCount"] = conf.ledCount;
    config["ledBright"] = conf.ledBrightness;
    config["ledType"] = conf.ledType;
    config["ledOrder"] = conf.ledOrder;
#endif
#ifdef C5VRX_LINK
    config["raceCtl"] = 1;
#endif
#ifdef ELRS_BACKPACK
    char mac[18];
    formatElrsMac(mac);
    config["elrsOn"] = conf.elrsEnabled;
    config["elrsUid"] = mac;
#endif
    serializeJson(config, destination);
}

void Config::toJsonString(char* buf) {
    DynamicJsonDocument config(CONFIG_JSON_SIZE);
    config["freq"] = conf.frequency;
    config["minLap"] = conf.minLap;
    config["alarm"] = conf.alarm;
    config["anType"] = conf.announcerType;
    config["anRate"] = conf.announcerRate;
    config["enterRssi"] = conf.enterRssi;
    config["exitRssi"] = conf.exitRssi;
    config["name"] = conf.pilotName;
    config["ssid"] = conf.ssid;
    config["pwd"] = conf.password;
#ifdef C5VRX_LINK
    config["rfGain"] = conf.rfGain;
#endif
#ifdef PIN_LED_STRIP
    config["ledOn"] = conf.ledEnabled;
    config["ledCount"] = conf.ledCount;
    config["ledBright"] = conf.ledBrightness;
    config["ledType"] = conf.ledType;
    config["ledOrder"] = conf.ledOrder;
#endif
#ifdef ELRS_BACKPACK
    char mac[18];
    formatElrsMac(mac);
    config["elrsOn"] = conf.elrsEnabled;
    config["elrsUid"] = mac;
#endif
    serializeJsonPretty(config, buf, CONFIG_JSON_SIZE);
}

void Config::fromJson(JsonObject source) {
    if (source["freq"] != conf.frequency) {
        conf.frequency = source["freq"];
        modified = true;
    }
    if (source["minLap"] != conf.minLap) {
        conf.minLap = source["minLap"];
        modified = true;
    }
    if (source["alarm"] != conf.alarm) {
        conf.alarm = source["alarm"];
        modified = true;
    }
    if (source["anType"] != conf.announcerType) {
        conf.announcerType = source["anType"];
        modified = true;
    }
    if (source["anRate"] != conf.announcerRate) {
        conf.announcerRate = source["anRate"];
        modified = true;
    }
    if (source["enterRssi"] != conf.enterRssi) {
        conf.enterRssi = source["enterRssi"];
        modified = true;
    }
    if (source["exitRssi"] != conf.exitRssi) {
        conf.exitRssi = source["exitRssi"];
        modified = true;
    }
    if (source["name"] != conf.pilotName) {
        strlcpy(conf.pilotName, source["name"] | "", sizeof(conf.pilotName));
        modified = true;
    }
    if (source["ssid"] != conf.ssid) {
        strlcpy(conf.ssid, source["ssid"] | "", sizeof(conf.ssid));
        modified = true;
    }
    if (source["pwd"] != conf.password) {
        strlcpy(conf.password, source["pwd"] | "", sizeof(conf.password));
        modified = true;
    }
    if (source["ledOn"].is<int>()) {
        uint8_t on = source["ledOn"] ? 1 : 0;
        if (on != conf.ledEnabled) {
            conf.ledEnabled = on;
            modified = true;
        }
    }
    if (source["ledCount"].is<int>()) {
        int n = source["ledCount"];
        if (n >= 0 && n <= LED_COUNT_MAX && n != conf.ledCount) {
            conf.ledCount = n;
            modified = true;
        }
    }
    if (source["ledBright"].is<int>()) {
        int b = source["ledBright"];
        if (b >= 0 && b <= 255 && b != conf.ledBrightness) {
            conf.ledBrightness = b;
            modified = true;
        }
    }
#ifdef PIN_LED_STRIP
    if (source["ledType"].is<int>()) {
        int t = source["ledType"];
        if (t >= 0 && t < LEDSTRIP_CHIP_COUNT && t != conf.ledType) {
            conf.ledType = t;
            modified = true;
        }
    }
    if (source["ledOrder"].is<int>()) {
        int o = source["ledOrder"];
        if (o >= 0 && o < LEDSTRIP_ORDER_COUNT && o != conf.ledOrder) {
            conf.ledOrder = o;
            modified = true;
        }
    }
#endif
    if (source["rfGain"].is<int>()) {
        int gain = source["rfGain"];
        if (gain >= 0 && gain <= 89 && gain != conf.rfGain) {
            conf.rfGain = gain;
            modified = true;
        }
    }
#ifdef ELRS_BACKPACK
    if (source["elrsOn"].is<int>()) {
        uint8_t on = source["elrsOn"] ? 1 : 0;
        if (on != conf.elrsEnabled) {
            conf.elrsEnabled = on;
            modified = true;
        }
    }
    // The phrase itself is never stored, only the MAC derived from it.
    const char* phrase = source["elrsPhrase"] | "";
    if (phrase[0] != 0) {
        uint8_t mac[ELRS_MAC_LEN];
        elrsMacFromPhrase(phrase, mac);
        if (memcmp(mac, conf.elrsMac, sizeof(mac)) != 0) {
            memcpy(conf.elrsMac, mac, sizeof(mac));
            modified = true;
        }
    }
#endif
}

#ifdef ELRS_BACKPACK
void Config::formatElrsMac(char* buf) {
    if (!elrs_uid_is_set(conf.elrsMac)) {
        buf[0] = 0;
        return;
    }
    snprintf(buf, 18, "%02x:%02x:%02x:%02x:%02x:%02x", conf.elrsMac[0], conf.elrsMac[1], conf.elrsMac[2], conf.elrsMac[3], conf.elrsMac[4], conf.elrsMac[5]);
}
#endif

uint8_t Config::getAnnouncerRate() {
    return conf.announcerRate;
}

bool Config::getElrsEnabled() {
    return conf.elrsEnabled != 0;
}

const uint8_t* Config::getElrsMac() {
    return conf.elrsMac;
}

uint16_t Config::getFrequency() {
    return conf.frequency;
}

uint32_t Config::getMinLapMs() {
    return conf.minLap * 100;
}

uint8_t Config::getAlarmThreshold() {
    return conf.alarm;
}

uint8_t Config::getEnterRssi() {
    return conf.enterRssi;
}

uint8_t Config::getExitRssi() {
    return conf.exitRssi;
}

uint8_t Config::getRfGain() {
    return conf.rfGain;
}

bool Config::getLedEnabled() {
    return conf.ledEnabled != 0;
}

uint16_t Config::getLedCount() {
    return conf.ledCount;
}

uint8_t Config::getLedBrightness() {
    return conf.ledBrightness;
}

uint8_t Config::getLedType() {
    return conf.ledType;
}

uint8_t Config::getLedOrder() {
    return conf.ledOrder;
}

char* Config::getSsid() {
    return conf.ssid;
}

char* Config::getPassword() {
    return conf.password;
}

void Config::setDefaults(void) {
    DEBUG("Setting EEPROM defaults\n");
    // Reset everything to 0/false and then just set anything that zero is not appropriate
    memset(&conf, 0, sizeof(conf));
    conf.version = CONFIG_VERSION | CONFIG_MAGIC;
    conf.frequency = DEFAULT_FREQUENCY;
    conf.rfGain = DEFAULT_RF_GAIN;
    conf.ledEnabled = 1;
    conf.ledCount = DEFAULT_LED_COUNT;
    conf.ledBrightness = DEFAULT_LED_BRIGHTNESS;
    conf.minLap = 100;
    conf.alarm = 0;  // 0.0 V: low-battery alarm off
    conf.announcerType = 2;
    conf.announcerRate = 10;
    conf.enterRssi = 120;
    conf.exitRssi = 100;
    strlcpy(conf.ssid, "", sizeof(conf.ssid));
    strlcpy(conf.password, "", sizeof(conf.password));
    strlcpy(conf.pilotName, "", sizeof(conf.pilotName));
    modified = true;
    write();
}

void Config::handleEeprom(uint32_t currentTimeMs) {
    if (modified && ((currentTimeMs - checkTimeMs) > EEPROM_CHECK_TIME_MS)) {
        checkTimeMs = currentTimeMs;
        write();
    }
}
