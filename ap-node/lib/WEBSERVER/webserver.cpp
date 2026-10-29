#include "webserver.h"
#include <ElegantOTA.h>

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <esp_wifi.h>

#include "debug.h"
#ifdef C5VRX_LINK
#include "rssi_stream.h"
#ifdef PIN_SD_CS
#include <memory>
#endif
#endif

static const uint8_t DNS_PORT = 53;
static IPAddress netMsk(255, 255, 255, 0);
static DNSServer dnsServer;
static IPAddress ipAddress;
static AsyncWebServer server(80);
static AsyncEventSource events("/events");
#ifdef C5VRX_LINK
// 1 kHz raw + filtered RSSI for the desktop app (docs/protocol.md). A source of
// its own so browsers on /events don't receive it.
static AsyncEventSource rssiEvents("/rssi");
static rssi_stream_t rssiStream;
#endif

static const char *wifi_hostname = "plt";
static const char *wifi_ap_ssid_prefix = "PhobosLT";
static const char *wifi_ap_password = "phoboslt";
static const char *wifi_ap_address = "20.0.0.1";
String wifi_ap_ssid;

// The C5 AP node sits millimetres from a 5.8 GHz receiver: keep its own
// radio on 2.4 GHz only and at modest TX power to limit desense.
static void applyRadioLimits() {
#ifdef C5VRX_LINK
#if CONFIG_SOC_WIFI_SUPPORT_5G
    WiFi.setBandMode(WIFI_BAND_MODE_2G_ONLY);
#endif
    WiFi.setTxPower(C5VRX_AP_TX_POWER);
#endif
}

void Webserver::init(Config *config, LapTimer *lapTimer, BatteryMonitor *batMonitor, Buzzer *buzzer, Led *l) {

    ipAddress.fromString(wifi_ap_address);

    conf = config;
    timer = lapTimer;
    monitor = batMonitor;
    buz = buzzer;
    led = l;

    wifi_ap_ssid = String(wifi_ap_ssid_prefix) + "_" + WiFi.macAddress().substring(WiFi.macAddress().length() - 6);
    wifi_ap_ssid.replace(":", "");

    WiFi.persistent(false);
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR);
    esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_LR);
    if (conf->getSsid()[0] == 0) {
        changeMode = WIFI_AP;
    } else {
        changeMode = WIFI_STA;
    }
    changeTimeMs = millis();
    lastStatus = WL_DISCONNECTED;
}

void Webserver::sendRssiEvent(uint8_t rssi) {
    if (!servicesStarted) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", rssi);
    events.send(buf, "rssi");
}

void Webserver::sendLaptimeEvent(uint32_t lapTime) {
    if (!servicesStarted) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", lapTime);
    events.send(buf, "lap");
#ifdef C5VRX_LINK
    if (rssiStreamClients) {
        char mark[24];
        snprintf(mark, sizeof(mark), "lap,%u", lapTime);
        rssiEvents.send(mark, "mark");
    }
#endif
}

#ifdef C5VRX_LINK
void Webserver::pushRssiSample(uint32_t ms, uint8_t raw, uint8_t filt) {
    rssi_stream_push(&rssiStream, ms, raw, filt);
}

void Webserver::handleRssiStream(uint32_t currentTimeMs) {
    if (!servicesStarted || (currentTimeMs - rssiStreamMs) < WEB_RSSI_STREAM_PERIOD_MS) return;
    rssiStreamMs = currentTimeMs;

    bool clients = rssiEvents.count() > 0;
    if (!clients) {
        rssi_stream_flush(&rssiStream);  // a new client starts with fresh samples
        rssiStreamClients = false;
        return;
    }
    if (!rssiStreamClients) {
        rssi_stream_flush(&rssiStream);
        raceWasRunning = !timer->isRunning();  // announce the current race state
        rssiStreamClients = true;
    }

    bool running = timer->isRunning();
    if (running != raceWasRunning) {
        char mark[32];
        if (running) {
            snprintf(mark, sizeof(mark), "start,%u", timer->getRaceStartMs());
        } else {
            snprintf(mark, sizeof(mark), "stop,%u", currentTimeMs);
        }
        rssiEvents.send(mark, "mark");
        raceWasRunning = running;
    }

    // Normally one batch of ~50; catch up after a stall, a few batches at a time.
    static char text[RSSI_STREAM_TEXT_MAX];
    for (int i = 0; i < 4 && rssi_stream_encode(&rssiStream, text); ++i) {
        rssiEvents.send(text, "b");
    }
}
#endif

#ifdef C5VRX_LINK
static void sendRaceEvent(const char *event, uint32_t value) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", value);
    events.send(buf, event);
}

void Webserver::setRaceControl(RaceControl *r) {
    race = r;
    race->setNotifier(sendRaceEvent);
}

bool Webserver::takeCalibStart(uint8_t *startGain, uint8_t *laps) {
    if (!calStartReq) return false;
    *startGain = calStartGain;
    *laps = calLaps;
    calStartReq = false;
    return true;
}

bool Webserver::takeCalibStop() {
    if (!calStopReq) return false;
    calStopReq = false;
    return true;
}

// POST /calib/start, optional JSON body {"startGain":45,"laps":3}.
void Webserver::requestCalibStart(AsyncWebServerRequest *request, JsonVariant *json) {
    if (!cal || timer->isRunning() || (race && !race->isIdle())) {
        request->send(409, "application/json", "{\"status\": \"busy\"}");
        return;
    }
    uint8_t startGain = CAL_START_GAIN_DEFAULT;
    uint8_t laps = CAL_LAPS_DEFAULT;
    if (json && (*json)["startGain"].is<int>()) startGain = constrain((*json)["startGain"].as<int>(), CAL_MIN_GAIN, CAL_MAX_GAIN);
    if (json && (*json)["laps"].is<int>()) laps = constrain((*json)["laps"].as<int>(), 1, CAL_LAPS_MAX);
    calStartGain = startGain;
    calLaps = laps;
    calStopReq = false;
    calStartReq = true;
    // Events after this seq belong to the new run.
    char buf[48];
    snprintf(buf, sizeof(buf), "{\"status\": \"OK\", \"seq\": %lu}", (unsigned long)cal->event_seq);
    request->send(200, "application/json", buf);
    led->on(200);
}

// GET /calib. Read without a lock while loop() writes it: a torn read only
// shows a stale field for one poll.
void Webserver::sendCalibStatus(AsyncWebServerRequest *request) {
    if (!cal) {
        request->send(404, "application/json", "{\"status\": \"no calibrator\"}");
        return;
    }
    char buf[768];
    uint32_t seq = cal->event_seq;
    const char *state = calStartReq ? "starting" : cal_state_name(cal->state);
    int n = snprintf(buf, sizeof(buf),
                     "{\"state\":\"%s\",\"event\":\"%s\",\"seq\":%lu,\"gain\":%u,\"lastPeak\":%u,\"lastClip\":%u,"
                     "\"clean\":%u,\"lapsSeen\":%u,\"lapsWanted\":%u,\"enter\":%u,\"exit\":%u,\"sepDb\":%.1f,\"events\":[",
                     state, cal_event_name(cal->event), (unsigned long)seq, cal->gain, cal->last_peak, cal->last_clip,
                     cal->clean_in_row, cal->laps_seen, cal->laps_wanted, cal->enter, cal->exit,
                     cal->separation / CAL_RSSI_PER_DB);
    // The last few events as [seq,"name",arg], so a poll that missed one can still speak it.
    uint32_t first = seq >= CAL_EVENT_LOG ? seq - CAL_EVENT_LOG + 1 : 1;
    for (uint32_t s = first; s <= seq && n > 0 && n < (int)sizeof(buf); ++s) {
        uint8_t arg;
        cal_event_t e = cal_event_at(cal, s, &arg);
        n += snprintf(buf + n, sizeof(buf) - n, "%s[%lu,\"%s\",%u]", s == first ? "" : ",", (unsigned long)s,
                      cal_event_name(e), arg);
    }
    if (n > 0 && n < (int)sizeof(buf)) snprintf(buf + n, sizeof(buf) - n, "]}");
    request->send(200, "application/json", buf);
}
#endif

void Webserver::handleWebUpdate(uint32_t currentTimeMs) {
    if (timer->isLapAvailable()) {
        sendLaptimeEvent(timer->getLapTime());
    }

    if (sendRssi && ((currentTimeMs - rssiSentMs) > WEB_RSSI_SEND_TIMEOUT_MS)) {
        sendRssiEvent(timer->getRssi());
        rssiSentMs = currentTimeMs;
    }
#ifdef C5VRX_LINK
    handleRssiStream(currentTimeMs);
    // Tell open pages when a calibration starts, from anywhere (the radio
    // button too), so they follow it and speak its prompts.
    bool calNow = calibrating();
    if (calNow && !calWasActive) events.send("1", "calibStart");
    calWasActive = calNow;
#endif

    wl_status_t status = WiFi.status();

    if (status != lastStatus && wifiMode == WIFI_STA) {
        DEBUG("WiFi status = %u\n", status);
        switch (status) {
            case WL_NO_SSID_AVAIL:
            case WL_CONNECT_FAILED:
            case WL_CONNECTION_LOST:
                changeTimeMs = currentTimeMs;
                changeMode = WIFI_AP;
                break;
            case WL_DISCONNECTED:  // try reconnection
                changeTimeMs = currentTimeMs;
                break;
            case WL_CONNECTED:
                buz->beep(200);
                led->off();
                wifiConnected = true;
                break;
            default:
                break;
        }
        lastStatus = status;
    }
    if (status != WL_CONNECTED && wifiMode == WIFI_STA && (currentTimeMs - changeTimeMs) > WIFI_CONNECTION_TIMEOUT_MS) {
        changeTimeMs = currentTimeMs;
        if (!wifiConnected) {
            changeMode = WIFI_AP;  // if we didnt manage to ever connect to wifi network
        } else {
            DEBUG("WiFi Connection failed, reconnecting\n");
            WiFi.reconnect();
            startServices();
            buz->beep(100);
            led->blink(200);
        }
    }
    if (changeMode != wifiMode && changeMode != WIFI_OFF && (currentTimeMs - changeTimeMs) > WIFI_RECONNECT_TIMEOUT_MS) {
        switch (changeMode) {
            case WIFI_AP:
                DEBUG("Changing to WiFi AP mode\n");

                WiFi.disconnect();
                wifiMode = WIFI_AP;
                WiFi.setHostname(wifi_hostname);  // hostname must be set before the mode is set to STA
#ifdef ELRS_BACKPACK
                // ESP-NOW from the ELRS backpack arrives on the STA interface, channel 1.
                WiFi.mode(WIFI_AP_STA);
#else
                WiFi.mode(wifiMode);
#endif
                applyRadioLimits();
                changeTimeMs = currentTimeMs;
                WiFi.softAPConfig(ipAddress, ipAddress, netMsk);
                WiFi.softAP(wifi_ap_ssid.c_str(), wifi_ap_password, WIFI_AP_CHANNEL);
                applyRadioLimits();
#ifdef ELRS_BACKPACK
                if (elrs) elrs->setRadioReady(true);
#endif
                startServices();
                buz->beep(1000);
                led->on(1000);
                break;
            case WIFI_STA:
                DEBUG("Connecting to WiFi network\n");
#ifdef ELRS_BACKPACK
                // The router picks the channel; ESP-NOW needs channel 1.
                if (elrs) elrs->setRadioReady(false);
#endif
                wifiMode = WIFI_STA;
                WiFi.setHostname(wifi_hostname);  // hostname must be set before the mode is set to STA
                WiFi.mode(wifiMode);
                applyRadioLimits();
                changeTimeMs = currentTimeMs;
                WiFi.begin(conf->getSsid(), conf->getPassword());
                startServices();
                led->blink(200);
            default:
                break;
        }

        changeMode = WIFI_OFF;
    }

    if (servicesStarted) {
        dnsServer.processNextRequest();
    }
}

/** Is this an IP? */
static boolean isIp(String str) {
    for (size_t i = 0; i < str.length(); i++) {
        int c = str.charAt(i);
        if (c != '.' && (c < '0' || c > '9')) {
            return false;
        }
    }
    return true;
}

/** IP to String? */
static String toStringIp(IPAddress ip) {
    String res = "";
    for (int i = 0; i < 3; i++) {
        res += String((ip >> (8 * i)) & 0xFF) + ".";
    }
    res += String(((ip >> 8 * 3)) & 0xFF);
    return res;
}

static bool captivePortal(AsyncWebServerRequest *request) {
    extern const char *wifi_hostname;

    if (!isIp(request->host()) && request->host() != (String(wifi_hostname) + ".local")) {
        DEBUG("Request redirected to captive portal\n");
        request->redirect(String("http://") + toStringIp(request->client()->localIP()));
        return true;
    }
    return false;
}

static void handleRoot(AsyncWebServerRequest *request) {
    if (captivePortal(request)) {  // If captive portal redirect instead of displaying the page.
        return;
    }
    request->send(LittleFS, "/index.html", "text/html");
}

static void handleNotFound(AsyncWebServerRequest *request) {
    if (captivePortal(request)) {  // If captive portal redirect instead of displaying the error page.
        return;
    }
    String message = F("File Not Found\n\n");
    message += F("URI: ");
    message += request->url();
    message += F("\nMethod: ");
    message += (request->method() == HTTP_GET) ? "GET" : "POST";
    message += F("\nArguments: ");
    message += request->args();
    message += F("\n");

    for (uint8_t i = 0; i < request->args(); i++) {
        message += String(F(" ")) + request->argName(i) + F(": ") + request->arg(i) + F("\n");
    }
    AsyncWebServerResponse *response = request->beginResponse(404, "text/plain", message);
    response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("Expires", "-1");
    request->send(response);
}

static bool startLittleFS() {
    if (!LittleFS.begin()) {
        DEBUG("LittleFS mount failed\n");
        return false;
    }
    DEBUG("LittleFS mounted sucessfully\n");
    return true;
}

static void startMDNS() {
    if (!MDNS.begin(wifi_hostname)) {
        DEBUG("Error starting mDNS\n");
        return;
    }

    String instance = String(wifi_hostname) + "_" + WiFi.macAddress();
    instance.replace(":", "");
    MDNS.setInstanceName(instance);
    MDNS.addService("http", "tcp", 80);
}

void Webserver::startServices() {
    if (servicesStarted) {
        MDNS.end();
        startMDNS();
        return;
    }

    startLittleFS();

    server.on("/", handleRoot);
    server.on("/generate_204", handleRoot);  // handle Andriod phones doing shit to detect if there is 'real' internet and possibly dropping conn.
    server.on("/gen_204", handleRoot);
    server.on("/library/test/success.html", handleRoot);
    server.on("/hotspot-detect.html", handleRoot);
    server.on("/connectivity-check.html", handleRoot);
    server.on("/check_network_status.txt", handleRoot);
    server.on("/ncsi.txt", handleRoot);
    server.on("/fwlink", handleRoot);

    server.on("/status", [this](AsyncWebServerRequest *request) {
        char buf[2048];
        char configBuf[CONFIG_JSON_SIZE];
        conf->toJsonString(configBuf);
        char rfBuf[96] = "n/a";
#ifdef C5VRX_LINK
        if (rssiSource) rssiSource->statusString(rfBuf, sizeof(rfBuf), millis());
#endif
        const char *elrsStatus = "n/a";
        char elrsDetail[512] = "";
#ifdef ELRS_BACKPACK
        if (elrs) {
            if (elrs->isActive()) {
                elrsStatus = "listening";
            } else if (!conf->getElrsEnabled()) {
                elrsStatus = "off";
            } else if (!elrs_uid_is_set(conf->getElrsMac())) {
                elrsStatus = "no bind phrase";
            } else if (elrs->wasStarted()) {
                elrsStatus = "start failed, see errors below";
            } else {
                elrsStatus = "not running (AP mode only)";
            }
            if (elrs->wasStarted()) {
                elrs->statusString(elrsDetail, sizeof(elrsDetail) - 1, millis());
                strcat(elrsDetail, "\n");
            }
        }
#endif
        char sdCardBuf[112] = "n/a";
        char sdBuf[160] = "n/a";
#ifdef PIN_SD_CS
        if (sdlog) {
            sdlog->cardString(sdCardBuf, sizeof(sdCardBuf));
            sdlog->statusString(sdBuf, sizeof(sdBuf));
        }
#endif
        float voltage = (float)monitor->getBatteryVoltage() / 10;
        const char *format =
            "\
Heap:\n\
\tFree:\t%i\n\
\tMin:\t%i\n\
\tSize:\t%i\n\
\tAlloc:\t%i\n\
LittleFS:\n\
\tUsed:\t%i\n\
\tTotal:\t%i\n\
Chip:\n\
\tModel:\t%s Rev %i, %i Cores, SDK %s\n\
\tFlashSize:\t%i\n\
\tFlashSpeed:\t%iMHz\n\
\tCPU Speed:\t%iMHz\n\
Network:\n\
\tIP:\t%s\n\
\tMAC:\t%s\n\
EEPROM:\n\
%s\n\
RF Node:\t%s\n\
ELRS Backpack:\t%s\n\
%s\
SD Card:\t%s\n\
SD Logging:\t%s\n\
Battery Voltage:\t%0.1fv";

        snprintf(buf, sizeof(buf), format,
                 ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getHeapSize(), ESP.getMaxAllocHeap(), LittleFS.usedBytes(), LittleFS.totalBytes(),
                 ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(), ESP.getSdkVersion(), ESP.getFlashChipSize(), ESP.getFlashChipSpeed() / 1000000, getCpuFrequencyMhz(),
                 WiFi.localIP().toString().c_str(), WiFi.macAddress().c_str(), configBuf, rfBuf, elrsStatus, elrsDetail, sdCardBuf, sdBuf, voltage);
        request->send(200, "text/plain", buf);
        led->on(200);
    });

    server.on("/timer/start", HTTP_POST, [this](AsyncWebServerRequest *request) {
#ifdef C5VRX_LINK
        if (calibrating()) {
            request->send(409, "application/json", "{\"status\": \"calibrating\"}");
            return;
        }
#endif
        timer->start();
        request->send(200, "application/json", "{\"status\": \"OK\"}");
    });

    // Browser start sequence began ("Arm your quad"): gate lights go red
    // until /timer/start arrives or the arm times out.
    server.on("/timer/arm", HTTP_POST, [this](AsyncWebServerRequest *request) {
#ifdef PIN_LED_STRIP
        if (ledStrip) ledStrip->arm(millis());
#endif
        request->send(200, "application/json", "{\"status\": \"OK\"}");
    });

#ifdef C5VRX_LINK
    // Firmware start sequence, shared with the ELRS radio button.
    server.on("/timer/begin", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (calibrating()) {
            request->send(409, "application/json", "{\"status\": \"calibrating\"}");
            return;
        }
        if (race) race->requestBegin();
        request->send(200, "application/json", "{\"status\": \"OK\"}");
    });

    // Auto-calibration (docs/protocol.md). The JSON handler must come first:
    // the plain POST handler below would also match a request with a body.
    AsyncCallbackJsonWebHandler *calibJsonHandler = new AsyncCallbackJsonWebHandler("/calib/start", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        requestCalibStart(request, &json);
    });
    calibJsonHandler->setMethod(HTTP_POST);
    server.addHandler(calibJsonHandler);
    server.on("/calib/start", HTTP_POST, [this](AsyncWebServerRequest *request) {
        requestCalibStart(request, nullptr);
    });
    server.on("/calib/stop", HTTP_POST, [this](AsyncWebServerRequest *request) {
        calStartReq = false;
        calStopReq = true;
        request->send(200, "application/json", "{\"status\": \"OK\"}");
    });
    server.on("/calib", HTTP_GET, [this](AsyncWebServerRequest *request) {
        sendCalibStatus(request);
    });
#endif

    server.on("/timer/stop", HTTP_POST, [this](AsyncWebServerRequest *request) {
#ifdef C5VRX_LINK
        if (race) {
            race->requestStop();
        } else {
            timer->stop();
        }
#else
        timer->stop();
#endif
        request->send(200, "application/json", "{\"status\": \"OK\"}");
    });

#ifdef PIN_SD_CS
    // Browser clock for SD log file names and export times: ?now=<epoch ms>&tz=<min east of UTC>.
    server.on("/clock", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (sdlog && request->hasParam("now")) {
            int64_t now = strtoll(request->getParam("now")->value().c_str(), nullptr, 10);
            int32_t tz = request->hasParam("tz") ? request->getParam("tz")->value().toInt() : 0;
            if (now > 0) sdlog->setClock(now, tz);
        }
        request->send(200, "application/json", "{\"status\": \"OK\"}");
    });

    // Config tab buttons. The SD writer task does the work; /status shows it.
    server.on("/sdlog/mount", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (sdlog) sdlog->requestMount();
        request->send(200, "application/json", "{\"status\": \"OK\"}");
    });
    server.on("/sdlog/unmount", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (sdlog) sdlog->requestUnmount();
        request->send(200, "application/json", "{\"status\": \"OK\"}");
    });

    // The newest SD log's last ?min= minutes (default 15) as a CSV download.
    server.on("/sdlog/export", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!sdlog) {
            request->send(404, "text/plain", "no SD logger");
            return;
        }
        uint32_t minutes = request->hasParam("min") ? request->getParam("min")->value().toInt() : 15;
        if (minutes == 0 || minutes > 600) minutes = 15;
        char stamp[24] = "";
        if (request->hasParam("now")) {
            int64_t now = strtoll(request->getParam("now")->value().c_str(), nullptr, 10);
            int32_t tz = request->hasParam("tz") ? request->getParam("tz")->value().toInt() : 0;
            if (now > 0) {
                sdlog->setClock(now, tz);
                sdlog_format_time(stamp, now + (int64_t)tz * 60000);
            }
        }
        auto cursor = std::make_shared<SdLog::Cursor>();
        const char *why = "";
        if (!sdlog->openExport(minutes, *cursor, &why)) {
            request->send(503, "text/plain", why);
            return;
        }
        SdLog *log = sdlog;
        AsyncWebServerResponse *response = request->beginChunkedResponse(
            "text/csv", [log, cursor](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
                (void)index;
                if (cursor->row < cursor->endRow && maxLen < SDLOG_EXPORT_ROW_MAX + sizeof(SDLOG_EXPORT_HEADER))
                    return RESPONSE_TRY_AGAIN;  // too small for a line now
                size_t n = log->readExport(*cursor, buffer, maxLen);
                if (n == 0) log->closeExport(*cursor);
                return n;
            });
        char disposition[80];
        if (stamp[0]) {
            // "YYYY-MM-DD HH:MM:SS.mmm" -> rssi_YYYYMMDD-HHMM_last15min.csv
            snprintf(disposition, sizeof(disposition), "attachment; filename=\"rssi_%.4s%.2s%.2s-%.2s%.2s_last%umin.csv\"",
                     stamp, stamp + 5, stamp + 8, stamp + 11, stamp + 14, (unsigned)minutes);
        } else {
            snprintf(disposition, sizeof(disposition), "attachment; filename=\"rssi_last%umin.csv\"", (unsigned)minutes);
        }
        response->addHeader("Content-Disposition", disposition);
        request->send(response);
    });
#endif

    server.on("/timer/rssiStart", HTTP_POST, [this](AsyncWebServerRequest *request) {
        sendRssi = true;
        request->send(200, "application/json", "{\"status\": \"OK\"}");
        led->on(200);
    });

    server.on("/timer/rssiStop", HTTP_POST, [this](AsyncWebServerRequest *request) {
        sendRssi = false;
        request->send(200, "application/json", "{\"status\": \"OK\"}");
        led->on(200);
    });

    server.on("/config", HTTP_GET, [this](AsyncWebServerRequest *request) {
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        conf->toJson(*response);
        request->send(response);
        led->on(200);
    });

    AsyncCallbackJsonWebHandler *configJsonHandler = new AsyncCallbackJsonWebHandler("/config", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        JsonObject jsonObj = json.as<JsonObject>();
#ifdef DEBUG_OUT
        serializeJsonPretty(jsonObj, DEBUG_OUT);
        DEBUG("\n");
#endif
        conf->fromJson(jsonObj);
        request->send(200, "application/json", "{\"status\": \"OK\"}");
        led->on(200);
    });

    server.serveStatic("/", LittleFS, "/").setCacheControl("max-age=600");

    events.onConnect([this](AsyncEventSourceClient *client) {
        if (client->lastId()) {
            DEBUG("Client reconnected! Last message ID that it got is: %u\n", client->lastId());
        }
        client->send("start", NULL, millis(), 1000);
        led->on(200);
    });

    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Max-Age", "600");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "POST,GET,OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "*");

    server.onNotFound(handleNotFound);

    server.addHandler(&events);
#ifdef C5VRX_LINK
    server.addHandler(&rssiEvents);
#endif
    server.addHandler(configJsonHandler);

    ElegantOTA.setAutoReboot(true);
    ElegantOTA.begin(&server);

    server.begin();

    dnsServer.start(DNS_PORT, "*", ipAddress);
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);

    startMDNS();

    servicesStarted = true;
}
