# double-ESP-resso: working notes

These notes are for continuing development, including in Claude Code sessions.
The user-facing documentation is `README.md` and `docs/`.

## What this project is

A cheap single-pilot FPV lap timer built from two stacked Seeed XIAO ESP32-C5 boards.

- **RF node** (`rf-node/`, ESP-IDF v6.0.2, GPL-3.0):
  - Measures narrow-band power of one channel from raw 10-bit I/Q and sends RSSI
    (0-255) at 1 kHz over UART.
  - The RSSI path (firmware 2, 2026-10-24) reimplements FPVGate C5MK's published method
    (paper + `docs/RADIO_INTERNALS.md`, `RF_RESEARCH.md`, `PROVENANCE.md`). A checkout is at
    `~/fpv-lap-timing-shenanigans/refs/FPVGateC5MK`, and the paper at `~/FPVGate_C5MK_Paper.pdf`.
  - Firmware 1 was derived from [C5VRX](https://github.com/Twotoz/C5VRX) (MODEM_DIAG + PARLIO).
- **AP node** (`ap-node/`, PhobosLT fork at upstream `142cf08`, MIT):
  - Runs the 2.4 GHz Wi-Fi AP, the web UI and lap detection.
- **Link:** the two-way UART protocol in `common/c5link_proto.h`. See `docs/protocol.md`.

## Decisions and why

- **RSSI = C5MK band power, reimplemented, never copied.** C5MK is CC BY-NC-SA 4.0, which
  can't be mixed with our GPL. Work from its docs and paper (facts and methods), and from
  our own disassembly of the Apache-2.0 `libphy.a` / `librftest.a`. Don't read or port its
  `multipilot/src` code.
  - Wi-Fi in `WIFI_MODE_NULL` + promiscuous keeps the PHY powered. `phy_set_chanfreq(5180, 1)`
    calibrates the band once, then `phy_set_rf_freq_offset(phy_param[49], pilot-10,
    (int16)phy_param[30])` tunes the PLL. These offsets were rechecked in IDF 6.0.2's libphy.
  - `iq_dump.c`: one stock `adctrig()` programs the dump engine, then direct register
    captures follow (2048 words at 0x40830000). The bank 0x40820000-0x4083FFFF is
    heap-reserved, and `rf-node/check_sram.py` fails the build if static data reaches it.
  - `band_power.h` (plain C, host-tested): LO 10 MHz below the pilot, spectrum inverted
    (`BP_SPECTRUM_SIGN`), Hann-8 blocks mixed to 0 Hz, single-pass DC removal, median-of-3
    + EMA 0.3.
  - The gain is re-forced only when `0x600A702C` shows it was lost. Rewriting a held force
    causes spikes (C5MK RF_RESEARCH section 3).
  - Single pilot, so no `hopNoWait`: the stock 398 us retune is fine.
- **Two stacked XIAO C5 boards** keep the BOM to two parts.
  - The RF board can't also run a Wi-Fi AP, because packet TX would disturb the
    I/Q capture.
- **Lap detection stays in PhobosLT on the AP node.** The RF node only sends raw RSSI.
- **The link must be two-way.** The user wants to change the tracked channel
  (R1/R3/R6/R7 matter) from the PhobosLT web UI.
  - The AP node owns the config and resends `F`/`G` until the RF node's `S`
    status matches.
- **Only 5V, GND, D5 and D9 are joined between the boards.**
  - Not D6/D7: GPIO11/12 are the C5 UART0 console pins. Both chips' ROM
    boot logs and the Arduino core's console are driven out on GPIO11, which
    garbled the link (found on hardware 2026-09-30).
  - Firmware 1 drove an IQ bus on D0-D3 and D10. Firmware 2 drives no pins besides the UART.
  - D2/D3 are strapping pins.
- **Scope is single pilot for now.** Multi-pilot on one C5 would need fast retune
  measurements first, because retune cost is unmeasured.
- **RSSI scaling:** band power in dB re 1 LSB², filtered, with -6..54 dB mapped to
  0-255 (4.25 counts/dB). The floor is about -2 dB, so about 18 counts. It is not
  calibrated dBm.

## Gate LEDs

- WS2812B / WS2812 / WS2811 (800 or 400 kHz) strip on the AP node's D4 (GPIO23), driven by
  `lib/LEDSTRIP` from `parallelTask`. The chip and colour order are set in the web UI (config v4).
  - Adafruit NeoPixel only renders the pixel buffer. Frames go out through our own RMT encoder
    (`ledstrip_timing.h`, 100 ns ticks, two memory blocks).
  - The library's IDF 5 `espShow` ignores 400 kHz, uses WS2812 timing that is out of spec
    for WS2811, and uses one RMT block (48 symbols on the C5, no DMA), which Wi-Fi interrupts can underrun.
- The gate is 50 cm in diameter, so about 94 LEDs at 60/m.
- The start countdown runs on the AP node (`lib/RACECONTROL`). The web UI posts `/timer/begin`
  and reacts to the `raceArm` / `raceStart` / `raceStop` SSE events for voice, tone and display.

## Desktop app stream

- `GET /rssi` (SSE, `#ifdef C5VRX_LINK`) streams every 1 kHz lap-timer sample, raw and
  filtered, in 50 ms batches, plus race/lap marks. It is for a Python desktop app
  (logging, replay, calibration). See `docs/protocol.md`.
- `loop()` pushes into a lock-free SPSC ring (`lib/RSSISTREAM/rssi_stream.h`, plain C,
  tested by `tests/test_rssi_stream.c`), and `parallelTask` drains it only while a client
  is connected.
- Unproven: throughput on hardware, and whether the extra 2.4 GHz traffic raises the
  noise floor.

## ELRS radio start

- `lib/ELRSBACKPACK` (`-DELRS_BACKPACK`) listens for the ExpressLRS TX backpack's ESP-NOW
  `MSP_ELRS_BACKPACK_SET_RECORDING_STATE` (0x0305), which the radio's "DVR Rec" AUX switch sends.
  - Each edge is sent once, with no repeats.
  - A short press (under 1 s) begins the countdown; holding for 1 s stops or cancels it.
- The backpack sends to the bind-phrase UID (`md5('-DMY_BINDING_PHRASE="…"')[0:6]`, bit 0
  cleared) on channel 1. The AP node runs `WIFI_AP_STA` with the SoftAP on channel 1, and
  sets the STA MAC to that UID.
- AP mode only; radio start can't work while the timer is joined to a home network in STA mode.
- The MSP parsing and press classification are plain C in `elrs_msp.h`, covered by `tests/test_elrs_msp.c`.
- User docs: `docs/RADIO_START.md`.

## Status (2026-09-28)

- Both firmwares build locally and in GitHub Actions, and the host tests pass.
- **2026-09-30:** first hardware bring-up. The link works (D5/D9) and RSSI
  responds to a VTX. Details and the fixes made are in `docs/bring-up.md`.
- **Open:** the RF node's USB console is silent. Flash the RF node from manual
  download mode; debug it through its NVS boot trace (namespace `trace`,
  read with `nvs_tool.py`) and the AP node's link counters, which print every
  10 s on the AP's USB console.
- **2026-10-24:** the RSSI path was rewritten (firmware 2, C5MK method). It builds,
  and `check_sram` and the host tests pass. It has **not run on hardware yet**.
- Unproven:
  - All of firmware 2 on IDF 6.0.2: the boot check, the direct trigger, and the spectrum
    sign (console `m`). C5MK only measured on IDF 5.5.1.
  - The default gain of 30 (the RF node default; the AP pushes its own config value).
  - Meter CPU cost: about 37 instructions/sample, so about 0.35 ms per 1 ms tick (estimated).
  - Desense from the 2.4 GHz AP.
  - XIAO battery sense on GPIO6/26, and whether the LED is active-low.
  - The gate LED strip (added after the first hardware-free build).
  - ELRS radio start: the STA MAC override and ESP-NOW alongside the SoftAP on the C5, and
    reception of the backpack's LR-enabled frames.
- The upstream PhobosLT envs (esp32dev, C3, S3) fail to build even on unmodified
  upstream because of library drift. Only `XIAO_C5` is maintained.

## Next step

Bring up firmware 2 on the RF node alone over USB (`docs/bring-up.md` section 1):
1. Flash it. `d` must show `check=live`, with `DUMP` direct captures climbing and no timeouts.
2. Run `m` with a VTX on the tuned channel: the pilot must be well above the mirror.
3. `f 5658` / `5732` / `5843` / `5880` / `5917` with a VTX on. `f 5925` must return `ERR_FREQ`.
4. Record the noise floor and gate passes at several gains with `tools/rssi_log.py`, then
   choose the default gain.

## Commands

```sh
# RF node (no local IDF needed)
podman run --rm -v $PWD:/project:Z -w /project/rf-node docker.io/espressif/idf:v6.0.2 idf.py set-target esp32c5 build
# flash: cd rf-node && idf.py -p /dev/ttyACM0 flash monitor   (or esptool with build/flasher_args.json)

# AP node
cd ap-node && pio run -e XIAO_C5 -t upload && pio run -e XIAO_C5 -t uploadfs

# host tests
cc -std=c99 -Wall -Wextra -Werror -I common tests/test_c5link_proto.c -o /tmp/t_proto && /tmp/t_proto
cc -std=c99 -Wall -Wextra -Werror -I rf-node/main tests/test_band_power.c -lm -o /tmp/t_bp && /tmp/t_bp
cc -std=c99 -Wall -Wextra -Werror -I ap-node/lib/ELRSBACKPACK tests/test_elrs_msp.c -o /tmp/t_elrs && /tmp/t_elrs
cc -std=c99 -Wall -Wextra -Werror -I ap-node/lib/LEDSTRIP tests/test_ledstrip_timing.c -o /tmp/t_led && /tmp/t_led
cc -std=c99 -Wall -Wextra -Werror -I ap-node/lib/RSSISTREAM tests/test_rssi_stream.c -o /tmp/t_stream && /tmp/t_stream
```

## Conventions

- Keep `common/c5link_proto.h` plain C99, MIT, header-only. Both nodes and the tests include it.
- Guard AP-node changes with `#ifdef C5VRX_LINK` so the fork stays diffable against upstream.
- Record hardware results with dates in `docs/bring-up.md`.
