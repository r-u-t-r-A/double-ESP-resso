#include "sdlog.h"

#include <SD.h>
#include <SPI.h>
#include <sd_diskio.h>

#include "diskio.h"  // FatFs disk_initialize(), for probing a card that won't mount
#include <sys/time.h>
#include <time.h>

#include "debug.h"

static sdlog_sample_t batch[SDLOG_BATCH];
static char rows[SDLOG_BATCH * SDLOG_ROW_LEN];
static uint8_t sector[512];  // idle card check

void SdLog::init(uint8_t sck, uint8_t miso, uint8_t mosi, uint8_t cs) {
    pinSck = sck;
    pinMiso = miso;
    pinMosi = mosi;
    pinCs = cs;
    // Same priority as the lap timer loop: SD writes block on SPI transfers
    // and card stalls, which the ring absorbs, while the loop keeps running.
    // FatFs keeps long file names on the stack.
    xTaskCreate(taskEntry, "sdlog", 6144, this, 1, NULL);
}

void SdLog::setClock(int64_t epochMs, int32_t tzOffsetMin) {
    int64_t offset = epochMs + (int64_t)tzOffsetMin * 60000 - (int64_t)millis();
    portENTER_CRITICAL(&clockMux);
    clockOffsetMs = offset;
    clockSet = true;
    portEXIT_CRITICAL(&clockMux);
}

bool SdLog::clock(int64_t *offsetMs) {
    portENTER_CRITICAL(&clockMux);
    bool set = clockSet;
    *offsetMs = clockOffsetMs;
    portEXIT_CRITICAL(&clockMux);
    return set;
}

void SdLog::push(uint32_t ms, uint8_t raw, uint8_t filt, bool running, uint32_t lapSerial, uint32_t lastLapMs) {
    sdlog_sample_t s = {ms, 0, raw, filt, SDLOG_EV_NONE};
    if (running && !wasRunning) {
        s.event = SDLOG_EV_START;
    } else if (!running && wasRunning) {
        s.event = SDLOG_EV_STOP;
    } else if (running && lapSerial != lastLapSerial) {
        s.event = SDLOG_EV_LAP;
        s.lapMs = lastLapMs;
    }
    wasRunning = running;
    lastLapSerial = lapSerial;
    if (enabled && mounted) sdlog_push(&ring, &s);
}

void SdLog::taskEntry(void *arg) {
    static_cast<SdLog *>(arg)->run();
}

bool SdLog::mount() {
    SPI.begin(pinSck, pinMiso, pinMosi, pinCs);
    if (!SD.begin(pinCs, SPI, SDLOG_SPI_HZ, "/sd", 3)) {
        SD.end();
        return false;
    }
    cardBytes = SD.totalBytes();
    usedBytes = SD.usedBytes();
    cardKind = SD.cardType();
    cardState = CARD_READY;
    DEBUG("SD card mounted, %llu MB\n", cardBytes / 1048576ULL);
    return true;
}

void SdLog::unmount() {
    closeSession();
    SD.end();
    mounted = false;
    cardState = CARD_CHECKING;
}

// SD.begin() fails the same way with no card and with an unformatted one.
// Initialising the card alone, without mounting, tells them apart.
void SdLog::probeCard() {
    uint8_t pdrv = sdcard_init(pinCs, &SPI, SDLOG_SPI_HZ);
    if (pdrv == 0xFF) {
        cardState = CARD_ABSENT;
        return;
    }
    DSTATUS st = disk_initialize(pdrv);
    cardKind = sdcard_type(pdrv);
    sdcard_uninit(pdrv);
    cardState = (st & STA_NOINIT) ? CARD_ABSENT : CARD_UNFORMATTED;
}

bool SdLog::cardResponds() {
    return SD.readRAW(sector, 0);
}

uint32_t SdLog::nextSequence() {
    uint32_t maxSeq = 0;
    File root = SD.open("/");
    if (!root) return 1;
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
        const char *name = f.name();
        unsigned seq;
        if (sscanf(name, "rssi_%4u", &seq) == 1 && seq > maxSeq) maxSeq = seq;
        f.close();
    }
    root.close();
    return maxSeq + 1;
}

bool SdLog::newestFile(char *out, size_t len) {
    uint32_t maxSeq = 0;
    File root = SD.open("/");
    if (!root) return false;
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
        unsigned seq;
        if (sscanf(f.name(), "rssi_%4u", &seq) == 1 && seq > maxSeq) {
            maxSeq = seq;
            snprintf(out, len, "/%s", f.name());
        }
        f.close();
    }
    root.close();
    return maxSeq > 0;
}

bool SdLog::openSession() {
    uint32_t seq = nextSequence();
    int64_t offset;
    if (clock(&offset)) {
        char stamp[24];
        sdlog_format_time(stamp, (int64_t)millis() + offset);
        // "YYYY-MM-DD HH:MM:SS.mmm" -> "YYYYMMDD-HHMM"
        snprintf(path, sizeof(path), "/rssi_%04u_%.4s%.2s%.2s-%.2s%.2s.csv", (unsigned)(seq % 10000), stamp,
                 stamp + 5, stamp + 8, stamp + 11, stamp + 14);
    } else {
        snprintf(path, sizeof(path), "/rssi_%04u.csv", (unsigned)(seq % 10000));
    }
    file = SD.open(path, FILE_WRITE);
    if (!file || file.write((const uint8_t *)SDLOG_HEADER, SDLOG_ROW_LEN) != SDLOG_ROW_LEN) {
        DEBUG("SD: cannot create %s\n", path);
        if (file) file.close();
        return false;
    }
    file.flush();
    sessionRows = 0;
    lastSaveMs = millis();
    sessionOpen = true;
    DEBUG("SD: logging to %s\n", path);
    return true;
}

void SdLog::closeSession() {
    if (!sessionOpen) return;
    file.flush();
    file.close();
    sessionOpen = false;
    usedBytes = SD.usedBytes();
}

void SdLog::run() {
    for (;;) {
        uint32_t now = millis();
        if (!mounted) {
            if (lastMountTryMs == 0 || (now - lastMountTryMs) >= SDLOG_MOUNT_RETRY_MS) {
                lastMountTryMs = now;
                mounted = mount();
                if (!mounted) {
                    probeCard();
                    DEBUG("SD: %s\n", cardState == CARD_UNFORMATTED ? "card has no FAT filesystem" : "no card");
                }
                lastCheckMs = now;
            }
            sdlog_flush(&ring);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (!enabled) {
            if (sessionOpen) closeSession();
            sdlog_flush(&ring);
            // Idle: notice a card being pulled out.
            if ((now - lastCheckMs) >= SDLOG_MOUNT_RETRY_MS) {
                lastCheckMs = now;
                if (!cardResponds()) {
                    DEBUG("SD: card gone\n");
                    unmount();
                    lastMountTryMs = now;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (!sessionOpen && !openSession()) {
            ++writeErrors;
            unmount();
            continue;
        }

        unsigned n = sdlog_pop(&ring, batch, SDLOG_BATCH);
        if (n) {
            for (unsigned i = 0; i < n; ++i) sdlog_format_row(rows + i * SDLOG_ROW_LEN, &batch[i]);
            size_t bytes = (size_t)n * SDLOG_ROW_LEN;
            if (file.write((const uint8_t *)rows, bytes) != bytes) {
                DEBUG("SD: write failed, remounting\n");
                ++writeErrors;
                unmount();
                continue;
            }
            sessionRows += n;
        }
        if ((now - lastSaveMs) >= SDLOG_SAVE_MS) {
            lastSaveMs = now;
            file.flush();  // file size and data on the card: a power cut loses ~1 s
        }
        if (n < SDLOG_BATCH) vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static const char *cardKindName(uint8_t kind) {
    switch (kind) {
        case CARD_MMC:
            return "MMC";
        case CARD_SD:
            return "SDSC";
        case CARD_SDHC:
            return "SDHC/SDXC";
        default:
            return "unknown type";
    }
}

void SdLog::cardString(char *buf, size_t len) {
    switch (cardState) {
        case CARD_READY: {
            // usedBytes is read at mount and after each session; count this session's rows on top.
            uint64_t used = usedBytes + (sessionOpen ? (uint64_t)(sessionRows + 1) * SDLOG_ROW_LEN : 0);
            uint64_t freeBytes = cardBytes > used ? cardBytes - used : 0;
            snprintf(buf, len, "OK, %s, %.1f GB, %.1f GB free", cardKindName(cardKind),
                     (double)cardBytes / 1e9, (double)freeBytes / 1e9);
            break;
        }
        case CARD_UNFORMATTED:
            snprintf(buf, len, "ERROR: card found (%s) but not readable: format it as FAT32", cardKindName(cardKind));
            break;
        case CARD_ABSENT:
            snprintf(buf, len, "ERROR: no card detected (check wiring and that a card is inserted)");
            break;
        default:
            snprintf(buf, len, "checking...");
            break;
    }
}

void SdLog::statusString(char *buf, size_t len) {
    if (!enabled) {
        snprintf(buf, len, "off");
    } else if (!mounted) {
        snprintf(buf, len, "waiting for a card, errors %lu", (unsigned long)writeErrors);
    } else {
        uint32_t freeMb = (uint32_t)((cardBytes - usedBytes) / 1048576ULL);
        snprintf(buf, len, "logging %s, %lu rows, %lu MB free, dropped %lu, errors %lu", sessionOpen ? path : "-",
                 (unsigned long)sessionRows, (unsigned long)freeMb, (unsigned long)ring.dropped,
                 (unsigned long)writeErrors);
    }
}

bool SdLog::openExport(uint32_t minutes, Cursor &c, const char **why) {
    if (!mounted) {
        *why = "no SD card";
        return false;
    }
    char p[40];
    bool current = sessionOpen;
    if (current) {
        strlcpy(p, path, sizeof(p));
    } else if (!newestFile(p, sizeof(p))) {
        *why = "no log files on the SD card";
        return false;
    }
    c.file = SD.open(p, FILE_READ);
    if (!c.file) {
        *why = "cannot open the log file";
        return false;
    }
    uint32_t rowCount = c.file.size() > SDLOG_ROW_LEN ? (uint32_t)((c.file.size() - SDLOG_ROW_LEN) / SDLOG_ROW_LEN) : 0;
    if (rowCount == 0) {
        c.file.close();
        *why = "the log file is empty";
        return false;
    }
    // Rows are 1 ms apart only nominally: find the first row of the window by
    // a binary search on the ms column.
    char row[SDLOG_ROW_LEN];
    sdlog_sample_t s;
    auto rowMs = [&](uint32_t i, uint32_t *ms) {
        if (!c.file.seek((uint32_t)(SDLOG_ROW_LEN * (i + 1))) || c.file.read((uint8_t *)row, SDLOG_ROW_LEN) != SDLOG_ROW_LEN ||
            !sdlog_parse_row(row, &s))
            return false;
        *ms = s.ms;
        return true;
    };
    uint32_t lastMs;
    if (!rowMs(rowCount - 1, &lastMs)) {
        c.file.close();
        *why = "the log file is damaged";
        return false;
    }
    uint32_t window = minutes * 60000u;
    uint32_t fromMs = lastMs > window ? lastMs - window : 0;
    uint32_t lo = 0, hi = rowCount - 1;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2, ms;
        if (!rowMs(mid, &ms) || ms < fromMs) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    c.row = lo;
    c.endRow = rowCount;
    c.headerSent = false;
    // Wall-clock times only for this boot's session: millis() restarts at boot.
    c.hasClock = current && clock(&c.clockOffsetMs);
    if (!c.hasClock) c.clockOffsetMs = 0;
    c.file.seek((uint32_t)(SDLOG_ROW_LEN * (c.row + 1)));
    return true;
}

size_t SdLog::readExport(Cursor &c, uint8_t *buf, size_t maxLen) {
    size_t n = 0;
    if (!c.headerSent) {
        size_t h = sizeof(SDLOG_EXPORT_HEADER) - 1;
        if (maxLen < h) return 0;
        memcpy(buf, SDLOG_EXPORT_HEADER, h);
        n = h;
        c.headerSent = true;
    }
    char row[SDLOG_ROW_LEN];
    while (c.row < c.endRow && maxLen - n >= SDLOG_EXPORT_ROW_MAX) {
        if (c.file.read((uint8_t *)row, SDLOG_ROW_LEN) != SDLOG_ROW_LEN) {
            c.row = c.endRow;  // truncated: end the download
            break;
        }
        ++c.row;
        n += sdlog_export_row((char *)buf + n, row, c.hasClock, c.clockOffsetMs);
    }
    return n;
}

void SdLog::closeExport(Cursor &c) {
    if (c.file) c.file.close();
}
