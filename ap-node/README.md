# AP node: PhobosLT for the double-ESP-resso RF node

This is a fork of [PhobosLT](https://github.com/phobos-/PhobosLT) at upstream
commit `142cf08`, MIT licensed. The upstream README is kept as
`UPSTREAM_README.md`.

## What the `XIAO_C5` env changes

Everything is behind `-DC5VRX_LINK`, so the upstream environments are unchanged.

- **`lib/C5LINK/`** (new): an RSSI source that reads the RF node over UART1
  (RX D6/GPIO11, TX D7/GPIO12). Its interface matches the `RX5808` class:
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
- **`data/`** (web UI):
  - Shows the RF node status and an RF gain field. These appear only when
    `/config` includes `rfGain`.
  - Warns about channels the C5 can't tune.

## Build

```sh
pio run -e XIAO_C5 -t upload
pio run -e XIAO_C5 -t uploadfs
```
