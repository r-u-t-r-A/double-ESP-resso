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

void ElrsBackpack::onReceive(const uint8_t *src, const uint8_t *data, int len) {
    // The TX backpack sends from the UID MAC, the same address it sends to.
    if (len <= 0 || memcmp(src, activeMac, ELRS_UID_LEN) != 0) return;
    elrs_msp_frame_t frame;
    bool on;
    uint16_t delayS;
    if (!elrs_msp_parse(data, (size_t)len, &frame)) return;
    if (!elrs_msp_recording_state(&frame, &on, &delayS)) return;

    uint32_t now = millis();
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
    esp_err_t err = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
    if (err != ESP_OK) {
        DEBUG("ELRS: STA protocol with LR failed (%d), using b/g/n\n", err);
        esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    }
    err = esp_wifi_set_mac(WIFI_IF_STA, activeMac);
    if (err != ESP_OK) {
        DEBUG("ELRS: setting STA MAC failed (%d)\n", err);
        return false;
    }
    if (esp_now_init() != ESP_OK) {
        DEBUG("ELRS: esp_now_init failed\n");
        return false;
    }
    instance = this;
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
    esp_now_unregister_recv_cb();
    esp_now_deinit();
    DEBUG("ELRS: stopped\n");
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
    return result;
}
