#pragma once
/*
 * sdlog.h - 1 kHz RSSI logging to a microSD card over SPI (PIN_SD_CS builds).
 *
 * The lap timer loop pushes every sample into a lock-free ring
 * (sdlog_row.h). A writer task drains it into fixed-width CSV rows, one file
 * per logging session (/rssi_NNNN[_YYYYMMDD-HHMM].csv), and saves the file
 * every second, so a card stall never blocks lap timing and a power cut
 * loses at most about a second. Logging runs while the "sdLog" config flag
 * is on and a card is mounted. The card is mounted once at boot, and after
 * that only when the web UI asks (requestMount). requestUnmount closes the
 * log file and unmounts, so the card can be pulled out safely. A mounted card
 * is checked every 5 s while idle, so pulling it out without unmounting shows
 * on the web UI. Nothing retries a missing card: with no card, or no SD
 * module, the Arduino SD driver busy-waits up to 500 ms per attempt.
 *
 * The export reads the last N minutes of the newest file back as a readable
 * CSV with wall-clock times, for the web UI's download button.
 */
#include <Arduino.h>
#include <FS.h>

#include "sdlog_row.h"

#define SDLOG_SPI_HZ 20000000
#define SDLOG_SAVE_MS 1000
#define SDLOG_CHECK_MS 5000  // a mounted, idle card is checked this often
#define SDLOG_BATCH 256  // rows formatted per write

class SdLog {
   public:
    void init(uint8_t sck, uint8_t miso, uint8_t mosi, uint8_t cs);
    // From parallelTask, every pass: follows the config flag.
    void setEnabled(bool on) { enabled = on; }
    // Browser clock: local time = epochMs + tzOffsetMin, taken now.
    void setClock(int64_t epochMs, int32_t tzOffsetMin);
    // Web UI buttons. The writer task carries them out; /status shows progress.
    void requestMount() {
        unmountRequested = false;
        mountRequested = true;
    }
    void requestUnmount() {
        mountRequested = false;
        unmountRequested = true;
    }

    // Lap timer loop, once per 1 kHz sample. Race start/stop and laps are
    // marked on the row where they were first seen.
    void push(uint32_t ms, uint8_t raw, uint8_t filt, bool running, uint32_t lapSerial, uint32_t lastLapMs);

    // /status lines: the card itself, and the logging session.
    void cardString(char *buf, size_t len);
    void statusString(char *buf, size_t len);

    // Export of the newest log file's last `minutes`.
    // Holds the card mounted until the download ends or is dropped.
    struct Cursor {
        ~Cursor();
        SdLog *owner = nullptr;  // set while the export holds the card
        File file;
        uint32_t row;     // next row to send
        uint32_t endRow;  // one past the last row
        bool hasClock;
        int64_t clockOffsetMs;  // local wall-clock ms = row ms + this
        bool headerSent;
    };
    // Returns false (with a reason) when there is nothing to export.
    bool openExport(uint32_t minutes, Cursor &c, const char **why);
    // Fills buf with whole export lines; 0 at the end.
    size_t readExport(Cursor &c, uint8_t *buf, size_t maxLen);
    void closeExport(Cursor &c);

   private:
    static void taskEntry(void *arg);
    void run();
    void tryMount();
    bool mount();
    void beginUnmount(uint8_t state);
    bool finishUnmount();
    void probeCard();
    bool cardResponds();
    bool openSession();
    void closeSession();
    uint32_t nextSequence();
    bool newestFile(char *path, size_t len);
    bool clock(int64_t *offsetMs);

    uint8_t pinSck = 0, pinMiso = 0, pinMosi = 0, pinCs = 0;
    volatile bool enabled = false;
    volatile bool mounted = false;
    typedef enum {
        CARD_CHECKING,
        CARD_ABSENT,       // nothing answered on the SPI bus
        CARD_UNFORMATTED,  // a card answered, but has no FAT filesystem
        CARD_READY,        // mounted
        CARD_EJECTED,      // unmounted from the web UI: safe to remove
        CARD_FAILED,       // stopped responding while logging
    } card_state_e;
    volatile uint8_t cardState = CARD_CHECKING;
    volatile uint8_t cardKind = 0;  // sdcard_type_t
    volatile bool mountRequested = false;
    volatile bool unmountRequested = false;
    bool unmounting = false;      // waiting for exports to close
    uint8_t unmountState = CARD_EJECTED;
    portMUX_TYPE exportMux = portMUX_INITIALIZER_UNLOCKED;
    uint8_t exportsOpen = 0;      // downloads reading the card
    sdlog_ring_t ring = {};

    // Producer (loop) state for event detection.
    bool wasRunning = false;
    uint32_t lastLapSerial = 0;

    // Writer task state.
    File file;
    char path[40] = "";
    volatile bool sessionOpen = false;
    uint32_t sessionRows = 0;
    uint32_t writeErrors = 0;
    uint32_t lastCheckMs = 0;
    uint32_t lastSaveMs = 0;
    uint64_t cardBytes = 0;
    uint64_t usedBytes = 0;

    portMUX_TYPE clockMux = portMUX_INITIALIZER_UNLOCKED;
    bool clockSet = false;
    int64_t clockOffsetMs = 0;  // local wall-clock ms = millis() + this
};
