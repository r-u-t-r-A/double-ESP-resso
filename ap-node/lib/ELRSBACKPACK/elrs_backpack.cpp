#include "elrs_backpack.h"

#include <MD5Builder.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "debug.h"

static ElrsBackpack *instance = nullptr;

void elrsMacFromPhrase(const char *phrase, uint8_t mac[ELRS_UID_LEN]) {
    String flag = String("-DMY_BINDING_PHRASE=\"") + phrase + "\"";
    uint8_t digest[16];
    MD5Builder md5;
    md5.begin();
    md5.add(flag);
    md5.calculate();
    md5.getBytes(digest);
    elrs_uid_to_mac(digest, mac);
}

static void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (instance && info) instance->onReceive(info->src_addr, data, len);
}

// ESP-NOW is a vendor-specific action frame: category 127, Espressif OUI 18:fe:34.
static void onPromiscuous(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (!instance || type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *f = pkt->payload;
    if (pkt->rx_ctrl.sig_len < 28) return;
    if (f[0] != 0xD0 || f[24] != 0x7F || f[25] != 0x18 || f[26] != 0xFE || f[27] != 0x34) return;
    instance->onSniff(f + 4, f + 10, pkt->rx_ctrl.rssi, pkt->rx_ctrl.channel);
}

static void formatMac(char *buf, const uint8_t *mac) {
    snprintf(buf, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void ElrsBackpack::onSniff(const uint8_t *dst, const uint8_t *src, int8_t rssi, uint8_t channel) {
    stats.air++;
    memcpy(stats.airDst, dst, ELRS_UID_LEN);
    memcpy(stats.airSrc, src, ELRS_UID_LEN);
    stats.airRssi = rssi;
    stats.airChannel = channel;
}

void ElrsBackpack::onReceive(const uint8_t *src, const uint8_t *data, int len) {
    stats.rx++;
    memcpy(stats.rxSrc, src, ELRS_UID_LEN);
    // The TX backpack sends from the UID MAC, the same address it sends to.
    if (len <= 0 || memcmp(src, activeMac, ELRS_UID_LEN) != 0) return;
    stats.rxFromUid++;
    elrs_msp_frame_t frame;
    bool on;
    uint16_t delayS;
    if (!elrs_msp_parse(data, (size_t)len, &frame)) return;
    stats.rxMsp++;
    stats.lastFunction = frame.function;
    if (!elrs_msp_recording_state(&frame, &on, &delayS)) return;
    stats.rxRecording++;
    stats.lastOn = on;

    uint32_t now = millis();
    stats.lastRecordingMs = now;
    portENTER_CRITICAL(&edgeMux);
    uint8_t next = (edgeHead + 1) % ELRS_EDGE_QUEUE_LEN;
    if (next != edgeTail) {
        edges[edgeHead] = {now, on};
        edgeHead = next;
    }
    portEXIT_CRITICAL(&edgeMux);
}

bool ElrsBackpack::start(const uint8_t mac[ELRS_UID_LEN]) {
    memcpy(activeMac, mac, ELRS_UID_LEN);
    instance = this;
    esp_err_t err = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
    stats.protocolErr = err;
    if (err != ESP_OK) {
        DEBUG("ELRS: STA protocol with LR failed (%d), using b/g/n\n", err);
        esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    }
    // The sniffer runs even if the steps below fail, so /status can show what is on the air.
    wifi_promiscuous_filter_t filter = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(onPromiscuous);
    esp_wifi_set_promiscuous(true);
    err = esp_wifi_set_mac(WIFI_IF_STA, activeMac);
    stats.macErr = err;
    if (err != ESP_OK) {
        DEBUG("ELRS: setting STA MAC failed (%d)\n", err);
        return false;
    }
    err = esp_now_init();
    stats.espNowErr = err;
    if (err != ESP_OK) {
        DEBUG("ELRS: esp_now_init failed (%d)\n", err);
        return false;
    }
    esp_now_register_recv_cb(onEspNowRecv);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, activeMac, ELRS_UID_LEN);
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
    DEBUG("ELRS: listening for backpack %02x:%02x:%02x:%02x:%02x:%02x\n", activeMac[0], activeMac[1], activeMac[2], activeMac[3], activeMac[4], activeMac[5]);
    return true;
}

void ElrsBackpack::stop() {
    esp_wifi_set_promiscuous(false);
    esp_now_unregister_recv_cb();
    esp_now_deinit();
    DEBUG("ELRS: stopped\n");
}

void ElrsBackpack::statusString(char *buf, size_t len, uint32_t currentTimeMs) {
    char want[18], sta[18], airSrc[18], airDst[18], rxSrc[18];
    uint8_t staMac[ELRS_UID_LEN] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, staMac);
    uint8_t primary = 0;
    wifi_second_chan_t second;
    esp_wifi_get_channel(&primary, &second);
    formatMac(want, activeMac);
    formatMac(sta, staMac);
    formatMac(airSrc, stats.airSrc);
    formatMac(airDst, stats.airDst);
    formatMac(rxSrc, stats.rxSrc);
    char last[32] = "never";
    if (stats.rxRecording) {
        snprintf(last, sizeof(last), "%s %lus ago", stats.lastOn ? "on" : "off", (unsigned long)((currentTimeMs - stats.lastRecordingMs) / 1000));
    }
    snprintf(buf, len,
             "\tUID:\t%s (STA MAC %s, channel %u)\n"
             "\tStart errors:\tprotocol %d, mac %d, esp_now %d\n"
             "\tOn air:\t%lu ESP-NOW frames, last %s -> %s, %d dBm, ch %u\n"
             "\tReceived:\t%lu, last from %s\n"
             "\tFrom UID:\t%lu, MSP ok %lu, last function 0x%04x\n"
             "\tDVR switch:\t%lu messages, last %s\n"
             "\tPresses:\t%lu short, %lu long",
             want, sta, primary, stats.protocolErr, stats.macErr, stats.espNowErr,
             (unsigned long)stats.air, airSrc, airDst, stats.airRssi, stats.airChannel,
             (unsigned long)stats.rx, rxSrc,
             (unsigned long)stats.rxFromUid, (unsigned long)stats.rxMsp, stats.lastFunction,
             (unsigned long)stats.rxRecording, last,
             (unsigned long)stats.shortPresses, (unsigned long)stats.longPresses);
}

bool ElrsBackpack::wasStarted() {
    return instance == this;
}

void ElrsBackpack::debugStats(uint32_t currentTimeMs) {
    if (instance != this || currentTimeMs - statsMs < 10000) return;
    statsMs = currentTimeMs;
    char buf[512];
    statusString(buf, sizeof(buf), currentTimeMs);
    DEBUG("ELRS backpack:\n%s\n", buf);
}

elrs_press_e ElrsBackpack::handle(uint32_t currentTimeMs, bool enabled, const uint8_t mac[ELRS_UID_LEN]) {
    bool want = radioReady && enabled && elrs_uid_is_set(mac);
    if (active && (!want || memcmp(mac, activeMac, ELRS_UID_LEN) != 0)) {
        stop();
        active = false;
        elrs_press_reset(&press);
    }
    if (want && !active && (currentTimeMs - lastStartMs) > ELRS_RETRY_MS) {
        lastStartMs = currentTimeMs;
        active = start(mac);
    }
    if (!active) return ELRS_PRESS_NONE;

    elrs_press_e result = ELRS_PRESS_NONE;
    for (;;) {
        Edge e;
        portENTER_CRITICAL(&edgeMux);
        bool have = edgeTail != edgeHead;
        if (have) {
            e = edges[edgeTail];
            edgeTail = (edgeTail + 1) % ELRS_EDGE_QUEUE_LEN;
        }
        portEXIT_CRITICAL(&edgeMux);
        if (!have) break;
        elrs_press_e p = elrs_press_edge(&press, e.on, e.ms);
        if (p != ELRS_PRESS_NONE) result = p;
    }
    elrs_press_e t = elrs_press_tick(&press, currentTimeMs);
    if (t != ELRS_PRESS_NONE) result = t;
    if (result == ELRS_PRESS_SHORT) stats.shortPresses++;
    if (result == ELRS_PRESS_LONG) stats.longPresses++;
    if (result != ELRS_PRESS_NONE) DEBUG("ELRS: %s press\n", result == ELRS_PRESS_SHORT ? "short" : "long");
    return result;
}
