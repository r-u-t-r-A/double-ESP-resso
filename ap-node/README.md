# AP node: PhobosLT for the double-ESP-resso RF node

This is a fork of [PhobosLT](https://github.com/phobos-/PhobosLT) at upstream
commit `142cf08`, MIT licensed. The upstream README is kept as
`UPSTREAM_README.md`.

## What the `XIAO_C5` env changes

Everything is behind `-DC5VRX_LINK`, so the upstream environments are unchanged.

- **`lib/C5LINK/`** (new): an RSSI source that reads the RF node over UART1
  (RX D5/GPIO24, TX D9/GPIO9). Its interface matches the `RX5808` class:
  - `readRssi()` returns the latest RSSI sample.
  - `handleFrequencyChange()` retunes the RF node.
  - It adds `handleGainChange()`, a non-blocking `poll()`, and link status.
  - The protocol itself is in `../common/c5link_proto.h`.
- **`lib/LAPTIMER`**: works with either source through `typedef … RssiSource`.
- **`src/main.cpp`**: runs one `LapTimer` update per 1 kHz RF sample, and
  yields every millisecond so both tasks get CPU time on the single-core C5.
- **`lib/CONFIG`**:
  - XIAO C5 pin block.
  - New `rfGain` setting. `CONFIG_VERSION` is bumped to 1, so saved settings
    reset once.
  - The default frequency is R1 (5658 MHz).
- **`lib/WEBSERVER`**:
  - The Wi-Fi AP/STA is forced to 2.4 GHz only, at 11 dBm.
  - `/status` shows an `RF Node:` line.
- **`lib/LEDSTRIP/`** (new):
  - Drives the WS2812B gate lights on `PIN_LED_STRIP` (D4) using Adafruit
    NeoPixel over RMT.
  - Renders from the background task at 50 Hz.
  - Colours depend on race state, laps, RSSI, RF link health and the battery alarm.
- **`lib/CONFIG`** also stores `ledOn`, `ledCount` and `ledBright`, with
  `CONFIG_VERSION` bumped to 2.
  - Version 3 appends `elrsOn` and the ELRS backpack UID; v2 settings are kept.
- **`lib/RACECONTROL/`** (new): runs the start countdown on the node.
  - It arms the gate, waits for the spoken lead-in plus a random 1–5 s, then
    starts the lap timer.
  - It reports `raceArm` / `raceStart` / `raceStop` as SSE events.
- **`lib/ELRSBACKPACK/`** (new, `-DELRS_BACKPACK`): starts and stops races
  from an ExpressLRS radio button via the TX backpack's ESP-NOW messages. See
  [../docs/RADIO_START.md](../docs/RADIO_START.md).
- **`lib/WEBSERVER`** adds:
  - `POST /timer/begin`, which starts the firmware countdown. `/timer/stop` also
    cancels a countdown.
  - `POST /timer/arm`, which turns the gate red during the countdown. It's kept
    for the old browser countdown.
  - In AP mode, the SoftAP runs on channel 1 as `WIFI_AP_STA`, for ESP-NOW.
- **`data/`** (web UI):
  - Shows the RF node status and an RF gain field. These appear only when
    `/config` includes `rfGain`.
  - Warns about channels the C5 can't tune.
  - LED strip settings.
  - *Start Race* posts `/timer/begin` when `/config` includes `raceCtl`, and
    otherwise runs the old browser countdown.
  - ELRS radio-start settings, shown only when `/config` includes `elrsUid`.

## Build

```sh
pio run -e XIAO_C5 -t upload
pio run -e XIAO_C5 -t uploadfs
```
