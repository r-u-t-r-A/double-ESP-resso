# double-ESP-resso

A cheap FPV race lap timer made from **two stacked Seeed XIAO ESP32-C5
boards** and no RX5808 module.

- The **RF node** turns the ESP32-C5's own 5 GHz Wi-Fi radio into a 5.8 GHz
  analog FPV power meter. It measures narrow-band power from raw I/Q
  captures, the method published by
  [FPVGate C5MK](https://github.com/LouisHitchcock/FPVGateC5MK).
- The **AP node** runs a port of [PhobosLT](https://github.com/phobos-/PhobosLT):
  a 2.4 GHz Wi-Fi access point, the web UI, lap detection, voice callouts and
  RSSI calibration. Voice callouts need an offline browser voice; see
  [docs/VOICE.md](docs/VOICE.md). Races can be started and stopped from an
  ExpressLRS radio button through its TX backpack; see
  [docs/RADIO_START.md](docs/RADIO_START.md).

```
 drone VTX ~~ 5.8 GHz ~~>  [ RF node  XIAO ESP32-C5 ]  raw I/Q -> band power -> RSSI 0-255 @ 1 kHz
                                 |  D1/D5 UART, both directions
                           [ AP node  XIAO ESP32-C5 ]  PhobosLT lap logic + web UI
 phone / laptop <~~ 2.4 GHz Wi-Fi AP "PhobosLT_xxxxxx" ~~
```

The link is **two-way**. When you pick a band and channel in the web UI, the
AP node retunes the RF node. The RF gain is set the same way, from the
Calibration tab.

> **Status: experimental, not yet validated on hardware.**
> - **Tested:** both firmwares build, and the protocol and band-power meter
>   pass their host tests.
> - **Not yet measured:** the raw I/Q meter (added 2026-10-24) on our
>   hardware, RSSI behaviour during gate passes, and desense from the AP radio.
>
> See [docs/bring-up.md](docs/bring-up.md) for the planned checks.

## Channels

The RF node accepts **5180 to 5917 MHz**.

| Band | Reachable | Not reachable |
|------|-----------|---------------|
| R (RaceBand) | R1–R8 (5658–5917) | – |
| A / B / F | all | – |
| E | E1–E6 | E7, E8 |
| L | all (5362–5621), untested with a transmitter | – |

Every frequency is tuned the same way: the PLL is set directly, with the LO
10 MHz below the pilot. FPVGate C5MK tested R1–R8 with a transmitter.

## Hardware

- 2 × Seeed Studio XIAO ESP32-C5.
- Optional:
  - A buzzer on the AP node's D0.
  - A 1S battery on the AP node's battery pads.
  - A **WS2812B strip around the gate** on the AP node's D2, with its own
    5 V supply. See [docs/hardware.md](docs/hardware.md#ws2812b-gate-led-strip-ap-node-d2).
  - A **microSD card** on the AP node's SPI pins (D8–D10, CS on D4) for 1 kHz
    RSSI logging. See [docs/hardware.md](docs/hardware.md#microsd-card-ap-node-spi).

**Join only these four pins between the boards:** 5V, GND, D1 and D5. D2/D3
are strapping pins, D6/D7 carry the UART0 console and D8–D10 the SD card, so a
full pin-to-pin stack is not safe. See
[docs/hardware.md](docs/hardware.md).

## Repository layout

| Path | What it contains |
|------|------------------|
| `rf-node/` | ESP-IDF v6.0.2 app for the RF node. GPL-3.0. |
| `ap-node/` | PlatformIO / Arduino PhobosLT fork for the AP node. MIT, from PhobosLT. |
| `common/c5link_proto.h` | The shared UART line protocol (MIT). See [docs/protocol.md](docs/protocol.md). |
| `tools/rssi_log.py` | Records and plots RF node RSSI over USB, for calibration. |
| `tests/` | Host unit tests: protocol, band-power meter, ELRS, LED strip, RSSI stream, SD log rows. |

## Documentation

| Document | Contents |
| --- | --- |
| [docs/FLASHING.md](docs/FLASHING.md) | Step-by-step flashing of both boards |
| [docs/hardware.md](docs/hardware.md) | Wiring, board stacking and the gate LED strip |
| [docs/protocol.md](docs/protocol.md) | RF node ↔ AP node UART protocol |
| [docs/bring-up.md](docs/bring-up.md) | Hardware bring-up checklist and results |
| [docs/VOICE.md](docs/VOICE.md) | Getting voice announcements working in the browser |
| [docs/RADIO_START.md](docs/RADIO_START.md) | Starting and stopping races from an ExpressLRS radio |

## Build and flash

Step-by-step flashing instructions for both boards are in **[docs/FLASHING.md](docs/FLASHING.md)**. The commands below are a short version.

Plug in **one board at a time**; the other board is powered through the 5V link.

### RF node

The RF node is an ESP-IDF v6.0.2 app. If you don't have ESP-IDF installed, use the container:

```sh
podman run --rm -v $PWD:/project:Z -w /project/rf-node docker.io/espressif/idf:v6.0.2 idf.py build
```

With ESP-IDF installed locally:

```sh
cd rf-node && idf.py set-target esp32c5 build flash monitor
```

### AP node

The AP node builds with PlatformIO:

```sh
cd ap-node
pio run -e XIAO_C5 -t upload          # firmware
pio run -e XIAO_C5 -t uploadfs        # web UI (LittleFS)
```

The `XIAO_C5` env uses the Tasmota `platform-espressif32` 2026.05.50 build,
which ships Arduino core 3.3.8 with ESP32-C5 support. The original PhobosLT
envs (`PhobosLT`, `ESP32C3`, `ESP32S3`) are kept for reference. They currently
fail to build upstream as well, because the old AsyncTCP fork no longer
matches newer ESPAsyncWebServer releases.

### Host tests

```sh
cc -std=c99 -Wall -Wextra -Werror -I common tests/test_c5link_proto.c -o /tmp/t_proto && /tmp/t_proto
cc -std=c99 -Wall -Wextra -Werror -I rf-node/main tests/test_band_power.c -lm -o /tmp/t_bp && /tmp/t_bp
cc -std=c99 -Wall -Wextra -Werror -I ap-node/lib/ELRSBACKPACK tests/test_elrs_msp.c -o /tmp/t_elrs && /tmp/t_elrs
cc -std=c99 -Wall -Wextra -Werror -I ap-node/lib/LEDSTRIP tests/test_ledstrip_timing.c -o /tmp/t_led && /tmp/t_led
cc -std=c99 -Wall -Wextra -Werror -I ap-node/lib/RSSISTREAM tests/test_rssi_stream.c -o /tmp/t_stream && /tmp/t_stream
cc -std=c99 -Wall -Wextra -Werror -I ap-node/lib/SDLOG tests/test_sdlog_row.c -o /tmp/t_sdlog && /tmp/t_sdlog
```

## Using it

1. Power the stack and join Wi-Fi `PhobosLT_xxxxxx` (password `phoboslt`).
   Then open `http://20.0.0.1`.
2. **Configuration tab:** pick the band and channel (for example R1, R3, R6 or
   R7) and press *Save Configuration*. The *RF node* line should show
   `OK <MHz> MHz …` within a second.
3. **Calibration tab:** press *Auto calibrate* and take off. The timer turns
   voice on and talks you through it while you fly normal laps:
   - "Fly through the gate now": fly through once. If the pass clipped the
     receiver ("Too strong"), the timer lowers the gain and asks again, until
     two passes in a row are clean.
   - "Now fly 3 laps": fly your normal line; each counted lap is called out.
   - At the end it says the separation in dB and the Enter and Exit values,
     and saves them with the gain. The sliders and the chart lines move to
     the new values.
   - Aim the antenna across the gate opening, away from the rest of the
     track. If the timer says the gate can't be told apart from the track,
     re-aim it and try again.
   - *Stop* cancels; the previous gain and thresholds stay. You can still set
     them by hand: if the peak flat-lines at 255, lower the *RF node gain*,
     set *Enter* just below the gate-pass peak and *Exit* well above the
     far-away level, then save.
4. **Race tab:** press *Start Race*.

For finer calibration without the AP node, plug the RF node in over USB and run:

```sh
tools/rssi_log.py /dev/ttyACM0 --freq 5732 --gain 30 --seconds 60 -o pass.csv
tools/rssi_log.py --plot pass.csv
```

## RSSI logging to SD

With a microSD card fitted, tick **Log RSSI to SD card (1 kHz)** on the Config
tab and save. The AP node then writes every sample the lap timer sees, about
105 MB per hour, until you untick it.

Under the checkbox, two fields show the card's state:

- **SD card** is checked every 5 s, whether logging is on or not. It shows `OK`
  with the card type, size and free space in green. It shows an error in red
  when no card answers (check the wiring and the card), or when a card answers
  but can't be read. Use FAT32: cards of 64 GB and up usually come as exFAT.
- **SD logging** shows the current file, row count, and any dropped samples or
  write errors.

- One file per logging session, numbered: `rssi_0007_20261025-1802.csv`. The
  date comes from the browser when the page loads; without it the file is just
  `rssi_0007.csv`.
- Columns: `ms` (AP node time), `raw` (RSSI from the RF node), `filtered` (what
  the lap timer compares with Enter/Exit), `event` (`S` race start, `X` stop,
  `L` lap, `-` none) and `lap_ms` (the lap time, on lap rows).
- **Export last 15 min of RSSI log** under the Calibration graph downloads the
  end of the newest file as a CSV with a wall-clock `time` column. That's about
  38 MB and takes one to two minutes over the timer's Wi-Fi. For whole
  sessions, put the card in a computer.
- The file is saved every second, so pulling the power loses about a second.
  Untick logging before you take the card out.

## Gate lights

With a WS2812B strip fitted, the gate shows the race state. Race events use
[RotorHazard](https://github.com/RotorHazard/RotorHazard)'s default LED
effects and colours, and the newest event replaces the previous one. States
higher in the table take priority.

| State | Lights |
|-------|--------|
| RF node link down or RF error | Purple blink |
| Power-up | Rainbow cycle (5 s) |
| Race stopped (also a cancelled countdown) | Solid red (4 s), then idle |
| Race start (tone) | Solid green (4 s) |
| Drone enters the gate while racing | Blue sparkle over green, fading out over about 20 s |
| Staging ("Arm your quad" and the staging tones) | Orange, two LEDs on and one off. Times out after 15 s if the start never comes. |
| Race running | Green |
| Idle | Bouncing balls (yellow, orange, purple and blue dots falling onto the first LED and bouncing, like WLED's effect), plus an RSSI level meter that fills from *Exit* to *Enter* (useful for calibration) |
| Low battery | Short amber blip every 2 s, over any state |

### Start sequence

The start follows RotorHazard's default race format, and the AP node runs it
for both the web *Start Race* button and the radio button:

1. The gate turns orange and the browser says "Arm your quad".
2. Three short staging tones, 1 s apart.
3. A random 0.5–4 s pause after the last staging tone.
4. The start tone (700 ms). The timer starts and the gate turns green.

*Start tone* on the Config tab picks where the tones play: the AP node's
buzzer, or the browser (440 Hz staging and 880 Hz start tones, as in
RotorHazard). Only one of them plays.

## How the RF node measures RSSI

The method is [FPVGate C5MK](https://github.com/LouisHitchcock/FPVGateC5MK)'s,
reimplemented from its paper and docs. No C5MK code is used.

The RF node starts Wi-Fi with no interface, so nothing transmits, and keeps
the PHY receiving. It calibrates the 5 GHz band once, then:
- tunes the PLL directly (`phy_set_rf_freq_offset`) with the LO **10 MHz
  below the pilot**;
- forces a fixed gain index, and re-forces it only if the PHY drops it.

Every millisecond it:
1. Captures 2048 raw samples (25.6 µs) from the PHY's ADC dump engine:
   10-bit I and Q at 80 MS/s, written into a reserved SRAM bank.
2. Mixes the pilot to 0 Hz and sums Hann-windowed blocks of 8 samples. That
   is the power in a band about 10 MHz wide around the pilot, with DC
   removed. The C5's spectrum is inverted, so the pilot sits at −10 MHz.
3. Filters the dB value (median of 3, then EMA with α 0.3).
4. Maps −6 to 54 dB to 0–255, which is 4.25 counts per dB.

Compared with the earlier wideband 4-bit MODEM_DIAG capture, this gives
10-bit samples instead of 4-bit, about 60 dB of span instead of 23.5 dB, and
a band that ignores Raceband neighbours (37 MHz away) and most Wi-Fi. No
GPIOs are used.

This is not calibrated dBm. Lap timing only needs a repeatable relative peak,
and PhobosLT's Kalman filter and enter/exit detection work on it unchanged.

## Licensing

- **`rf-node/`** is licensed **GPL-3.0-or-later** (see `LICENSE`). Its first
  versions were derived from C5VRX. The current RSSI path follows FPVGate
  C5MK's published method but contains no C5MK code. C5MK itself is
  CC BY-NC-SA 4.0, which can't be mixed with the GPL, so don't copy its
  source into this tree.
- **`ap-node/`** is derived from PhobosLT and stays **MIT**. The original
  copyright notice is kept in `ap-node/LICENSE`.
- **`common/c5link_proto.h`** is MIT, so both nodes can use it.
