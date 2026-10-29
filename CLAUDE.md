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
- **Only 5V, GND, D1 and D5 are joined between the boards** (D9 until 2026-10-25; it moved
  so the AP node's SD card could take the XIAO SPI pins D8-D10, and the buzzer moved D8 -> D0).
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
- **Auto-calibration** (2026-10-28, `ap-node/lib/CALIBRATION/calibration.h`, plain C,
  tested by `tests/test_calibration.c`). The web UI's *Auto calibrate* speaks prompts while the
  pilot flies real laps.
  - Why gain: RSSI 255 is the 10-bit I/Q ADC rail, so a close pass at high gain clips, and no
    RSSI width or scale fixes that. Raising gain lifts the floor and the peak equally, so the
    target is the highest gain whose gate peak doesn't clip and keeps headroom.
  - Why not fixed distances (RotorHazard's 2 m / 7 m): on a small whoop track the quad is within
    a few metres of the timer everywhere, so it uses real gate passes.
  - RF firmware 3 adds the capture's clip per mille to `R` lines (`R,<seq>,<rssi>,<clip>`). The
    AP still parses the old 3-field line, with clip 0, so only `CAL_PEAK_MAX` catches clipping
    with an old RF node.
  - Phase A: start at gain 45, open a pass window (500 ms baseline median, then a rise of 20
    counts), judge each pass after a 300 ms tail. Clipped (> 5 per mille) or peak > 230 steps the
    gain down by 4 and waits for the RF node's `S` to confirm the gain plus 400 ms to settle.
    Two clean passes in a row lock it. The dB per step is unknown, so the search is empirical.
  - Phase B: record the Kalman-filtered RSSI (the value `LapTimer` compares) as 10 ms maxima
    while the pilot flies N laps, counted live with provisional thresholds from phase A. Enter
    is the centre of the widest band of Enter values that gives the same count through
    `LapTimer`'s peak logic and minimum lap; Exit is the centre of the matching Exit band. Our
    own method, not RotorHazard's (MIT NON-AI).
  - Integration (`#ifdef C5VRX_LINK`): `loop()` calls `cal_push` per sample and runs
    `cal_start` / `cal_abort` on request flags from `/calib/start` and `/calib/stop`, so the
    18 KB struct is only written on the loop task. `parallelTask` sends `cal.gain` while it
    runs. On success the gain, Enter and Exit go through `Config` setters once, and
    `handleEeprom` saves them. Races can't start while calibrating, and vice versa.
  - `GET /calib` returns the last 8 events with sequence numbers, because a 250 ms poll can
    miss events emitted together (pass_clean, gain_locked, fly_laps).
  - Unproven: every tunable in `calibration.h` is untested on hardware, especially
    `CAL_PEAK_MAX`, `CAL_GAIN_STEP` and `CAL_MIN_RISE`. A between-pass bump more than
    `CAL_MIN_RISE` above the floor can be taken for a gate pass in phase A. That lowers the
    provisional thresholds, so phase B then counts bumps as laps, and the result fits that
    count. Also unproven: how long a gain change takes to settle. The final threshold search
    runs on the loop task, about 0.4 ms on a desktop for a 3-lap trace and 3 ms for a full
    180 s one. Scaled to the C5 that is maybe 20-40 ms and 0.2 s; the second outlasts the 2 KB
    UART buffer (about 120 samples), so a few link samples may drop once, after recording.
- **RF node firmware is updated over USB only** (decided 2026-10-29). Flashing it from the AP
  through the ROM loader would need crossed D6/D7 wires (UART0; a straight pin-to-pin join
  garbled the link before) and a way into download mode: either the undocumented
  `LP_AON_FORCE_DOWNLOAD_BOOT` register ("internal debugging only") or EN/BOOT wires on the
  button pads. The AP has no free GPIOs for those wires (only D3, a strapping pin). App-level
  OTA over the D1/D5 link would need a two-slot partition table and update code on both
  nodes. Not worth it for now.

## Gate LEDs

- WS2812B / WS2812 / WS2811 (800 or 400 kHz) strip on the AP node's D2 (GPIO25), driven by
  `lib/LEDSTRIP` from `parallelTask`. The chip and colour order are set in the web UI (config v4).
  - Adafruit NeoPixel only renders the pixel buffer. Frames go out through our own RMT encoder
    (`ledstrip_timing.h`, 100 ns ticks, two memory blocks).
  - The library's IDF 5 `espShow` ignores 400 kHz, uses WS2812 timing that is out of spec
    for WS2811, and uses one RMT block (48 symbols on the C5, no DMA), which Wi-Fi interrupts can underrun.
- The gate is 50 cm in diameter, so about 94 LEDs at 60/m.
- The start countdown runs on the AP node (`lib/RACECONTROL`). The web UI posts `/timer/begin`
  and reacts to the `raceArm` / `raceTone` / `raceStart` / `raceStop` SSE events for voice,
  tones and display.
  - The sequence is RotorHazard's default race format (behaviour only, no RH code: RH's licence
    is "MIT NON-AI"). "Arm your quad" (speech time from the announcer rate), then 3 staging
    tones 1 s apart (100 ms), then the start tone (700 ms) 0.5-4 s after the last one.
  - Tones come from either the AP buzzer or the browser, never both: config v5 `startTone`
    (`START_TONE_BUZZER` default / `START_TONE_BROWSER`), checked in `RaceControl`,
    `LapTimer::start()`, and the browser's `raceTone` / `raceStart` handlers (RH tones:
    440 Hz / 880 Hz triangle). Voice is separate.
  - Gate colours follow RH's default LED effects for events (staging orange 2-on-1-off, start
    green 4 s, stop red 4 s, gate entry seat-1 blue sparkle, startup rainbow), with the latest
    event replacing the previous one. Our idle (colour twinkles + RSSI meter) and running green
    stay, by the user's choice. The lap chase flash was dropped (RH: none).
  - Idle colour twinkles: like WLED's *Colortwinkles* but written from scratch (WLED is EUPL, don't
    port it). The ring is dark. New twinkles start at a steady 5 % of the LEDs per second, each
    on a dark LED in a random colour (yellow, red, pink, purple). Each rises 8/frame (0.6 s) and
    falls by level/32+1 per frame (about 1.7 s). On average that lights about 4 full-LED
    equivalents on 94 LEDs, against about 11 for fairy lights. Chosen for low idle power.
  - `RaceControl::stop()` calls `LedStrip::stop()`, which also ends staging. Without it the
    strip's own staging flag outlived a cancelled countdown.

## Desktop app stream

- `GET /rssi` (SSE, `#ifdef C5VRX_LINK`) streams every 1 kHz lap-timer sample, raw and
  filtered, in 50 ms batches, plus race/lap marks. It is for a Python desktop app
  (logging, replay, calibration). See `docs/protocol.md`.
- `loop()` pushes into a lock-free SPSC ring (`lib/RSSISTREAM/rssi_stream.h`, plain C,
  tested by `tests/test_rssi_stream.c`), and `parallelTask` drains it only while a client
  is connected.
- Unproven: throughput on hardware, and whether the extra 2.4 GHz traffic raises the
  noise floor.

## SD card logging

- `lib/SDLOG` (`#ifdef PIN_SD_CS`): microSD over SPI on the AP node, SCK D8, MISO D9, MOSI D10,
  CS D4 (not the XIAO's SS on D3, a strapping pin). Config v6 `sdLog` switches it on.
- `loop()` pushes every 1 kHz sample (raw, filtered, race start/stop/lap events) into a
  lock-free 2048-sample ring (`sdlog_row.h`, plain C, tested by `tests/test_sdlog_row.c`). A
  writer task (priority 0, level with `parallelTask`) writes fixed-width 29-byte rows, one file per session
  (`/rssi_NNNN[_date].csv`), and flushes every 1 s. Card stalls only fill the ring.
- The writer task mounts the card once at boot whether or not logging is on, and after that
  only on the Config tab's *Mount SD card* (`POST /sdlog/mount`). *Unmount SD card*
  (`/sdlog/unmount`) closes the log file and unmounts, so the card can be swapped without a
  reboot; it waits for any export download to finish (a lease taken in `openExport`, released by
  `closeExport` or the `Cursor` destructor when a download is dropped). A mounted idle card is
  checked with `SD.readRAW()` every 5 s. Nothing retries a missing card: with no card, or no SD
  module (MISO floats), the Arduino SD driver's `sdWait()` busy-waits up to 500 ms per call,
  which at priority 1 froze the gate LEDs (2026-10-29). When `SD.begin()` fails, `sdcard_init()` +
  `disk_initialize()` tell "no card" from "card without FAT". `/status` lines `SD Card:` and
  `SD Logging:` feed the Config tab fields.
- Fixed-width rows let `/sdlog/export?min=15` binary-search the window by `ms`. It streams a
  chunked CSV with wall-clock times from the browser clock (`/clock`, sent on page load).
  Times are only possible for this boot's file, because `millis()` restarts at boot.
- Unproven: SD library behaviour on the C5 (Arduino 3.3.8), write stalls versus the ring,
  export speed over the AP, SPI noise near the RF node, and free heap with the ring (static
  RAM is 84 KB, 101 KB with the auto-calibration trace).

## ELRS radio start

- `lib/ELRSBACKPACK` (`-DELRS_BACKPACK`) listens for the ExpressLRS TX backpack's ESP-NOW
  `MSP_ELRS_BACKPACK_SET_RECORDING_STATE` (0x0305), which the radio's "DVR Rec" AUX switch sends.
  - Each edge is sent once, with no repeats.
  - A short press (under 1 s) begins the countdown; holding for 1 s stops or cancels it.
  - Config v7 `elrsAction` (Config tab *Radio button starts*): `ELRS_ACTION_CALIBRATE` makes
    the short press start auto-calibration instead, with the start gain and laps last used
    from the web UI (defaults 45 / 3 after boot); a long press stops it. The press only sets a
    flag; `loop()` runs `cal_start`, as for `/calib/start`. Voice comes from the browser, so
    `handleWebUpdate` sends a `calibStart` SSE event whenever calibration starts, and an open
    page follows it and speaks from the "started" event on.
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
cc -std=c99 -Wall -Wextra -Werror -I ap-node/lib/SDLOG tests/test_sdlog_row.c -o /tmp/t_sdlog && /tmp/t_sdlog
cc -std=c99 -Wall -Wextra -Werror -I ap-node/lib/CALIBRATION tests/test_calibration.c -o /tmp/t_cal && /tmp/t_cal
```

## Conventions

- Keep `common/c5link_proto.h` plain C99, MIT, header-only. Both nodes and the tests include it.
- Guard AP-node changes with `#ifdef C5VRX_LINK` so the fork stays diffable against upstream.
- Record hardware results with dates in `docs/bring-up.md`.
