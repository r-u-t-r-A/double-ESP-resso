#include "c5link.h"

#include "debug.h"

#define C5LINK_SERIAL Serial1
#define C5LINK_RX_BUFFER 2048

C5Link::C5Link(uint8_t _rxPin, uint8_t _txPin) : rxPin(_rxPin), txPin(_txPin) {}

void C5Link::init() {
    c5link_rx_reset(&rx);
    C5LINK_SERIAL.setRxBufferSize(C5LINK_RX_BUFFER);
    C5LINK_SERIAL.begin(C5LINK_BAUD, SERIAL_8N1, rxPin, txPin);
    c5link_msg_t q = {};
    q.type = C5LINK_MSG_QUERY;
    send(q);
    DEBUG("C5Link UART RX=%u TX=%u @%u\n", rxPin, txPin, C5LINK_BAUD);
}

void C5Link::send(const c5link_msg_t &msg) {
    char buf[C5LINK_MAX_LINE + 4];
    size_t n = c5link_format(buf, sizeof(buf), &msg);
    if (n) C5LINK_SERIAL.write((const uint8_t *)buf, n);
}

bool C5Link::poll() {
    while (C5LINK_SERIAL.available() > 0) {
        c5link_msg_t m;
        if (!c5link_rx_feed(&rx, (char)C5LINK_SERIAL.read(), &m)) continue;
        if (m.type == C5LINK_MSG_STATUS) {
            rfMhz = m.mhz;
            rfGain = m.gain;
            rfState = m.state;
            rfVersion = m.version;
            statusTimeMs = millis();
            statusSeen = true;
        } else if (m.type == C5LINK_MSG_RSSI) {
            if (haveSeq) dropped = dropped + (uint8_t)(m.seq - lastSeq - 1);
            haveSeq = true;
            lastSeq = m.seq;
            samples = samples + 1;
            rssi = m.rssi;
            return true;
        }
    }
    return false;
}

uint8_t C5Link::readRssi() {
    return rssi;
}

void C5Link::handleFrequencyChange(uint32_t currentTimeMs, uint16_t freqMhz) {
    if (freqMhz == 0 || freqMhz == 1111) return;  // PhobosLT power-down sentinel
    if (freqMhz != wantMhz) {
        wantMhz = freqMhz;
        freqSentMs = currentTimeMs - C5LINK_ERR_BACKOFF_MS;  // send now
    }
    bool agreed = statusSeen && rfMhz == wantMhz && rfState == C5LINK_ST_OK;
    if (agreed) return;
    bool failing = statusSeen && (rfState == C5LINK_ST_ERR_FREQ || rfState == C5LINK_ST_ERR_RF);
    uint32_t interval = failing ? C5LINK_ERR_BACKOFF_MS : C5LINK_RESEND_MS;
    if (currentTimeMs - freqSentMs < interval) return;
    freqSentMs = currentTimeMs;
    c5link_msg_t f = {};
    f.type = C5LINK_MSG_FREQ;
    f.mhz = wantMhz;
    send(f);
}

void C5Link::handleGainChange(uint32_t currentTimeMs, uint8_t gain) {
    if (gain > C5LINK_MAX_GAIN) return;
    if (statusSeen && rfGain == gain) return;
    if (currentTimeMs - gainSentMs < C5LINK_RESEND_MS) return;
    gainSentMs = currentTimeMs;
    c5link_msg_t g = {};
    g.type = C5LINK_MSG_GAIN;
    g.gain = gain;
    send(g);
}

bool C5Link::linkUp(uint32_t currentTimeMs) {
    return statusSeen && (currentTimeMs - statusTimeMs) < C5LINK_TIMEOUT_MS;
}

void C5Link::statusString(char *buf, size_t len, uint32_t currentTimeMs) {
    if (!linkUp(currentTimeMs)) {
        snprintf(buf, len, "DOWN");
        return;
    }
    snprintf(buf, len, "%s %u MHz gain %u fw %u samples %lu dropped %lu",
             c5link_state_name(rfState), rfMhz, rfGain, rfVersion,
             (unsigned long)samples, (unsigned long)dropped);
}
