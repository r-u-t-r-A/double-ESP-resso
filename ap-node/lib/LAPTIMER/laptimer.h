#ifdef C5VRX_LINK
#include "c5link.h"
typedef C5Link RssiSource;
#else
#include "RX5808.h"
typedef RX5808 RssiSource;
#endif
#include "buzzer.h"
#include "config.h"
#include "kalman.h"
#include "led.h"

typedef enum {
    STOPPED,
    WAITING,
    RUNNING
} laptimer_state_e;

#define LAPTIMER_LAP_HISTORY 10
#define LAPTIMER_RSSI_HISTORY 100

class LapTimer {
   public:
    void init(Config *config, RssiSource *rssiSource, Buzzer *buzzer, Led *l);
    void start();
    void stop();
    void handleLapTimerUpdate(uint32_t currentTimeMs);
    uint8_t getRssi();
    uint32_t getLapTime();
    bool isLapAvailable();
    bool isRunning() { return state == RUNNING; }
    uint32_t getLapSerial() { return lapSerial; }

   private:
    laptimer_state_e state = STOPPED;
    RssiSource *rx;
    Config *conf;
    Buzzer *buz;
    Led *led;
    KalmanFilter filter;
    boolean lapCountWraparound;
    uint32_t raceStartTimeMs;
    uint32_t startTimeMs;
    uint8_t lapCount;
    uint8_t rssiCount;
    uint32_t lapTimes[LAPTIMER_LAP_HISTORY];
    uint8_t rssi[LAPTIMER_RSSI_HISTORY];

    uint8_t rssiPeak;
    uint32_t rssiPeakTimeMs;

    bool lapAvailable = false;
    volatile uint32_t lapSerial = 0;

    void lapPeakCapture();
    bool lapPeakCaptured();
    void lapPeakReset();

    void startLap();
    void finishLap();
};
