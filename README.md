# double-ESP-resso

A cheap FPV race lap timer made from **two stacked Seeed XIAO ESP32-C5
boards** and no RX5808 module.

- The **RF node** turns the ESP32-C5's own 5 GHz Wi-Fi radio into a 5.8 GHz
  analog FPV power meter, using the receive path from
  [C5VRX](https://github.com/Twotoz/C5VRX).
- The **AP node** runs a port of [PhobosLT](https://github.com/phobos-/PhobosLT):
  a 2.4 GHz Wi-Fi access point, the web UI, lap detection, voice callouts and
  RSSI calibration.

```
 drone VTX ~~ 5.8 GHz ~~>  [ RF node  XIAO ESP32-C5 ]  MODEM_DIAG IQ -> power -> RSSI 0-255 @ 1 kHz
                                 |  D6/D7 UART, both directions
                           [ AP node  XIAO ESP32-C5 ]  PhobosLT lap logic + web UI
 phone / laptop <~~ 2.4 GHz Wi-Fi AP "PhobosLT_xxxxxx" ~~
```

The link is **two-way**. When you pick a band and channel in the web UI, the
AP node retunes the RF node. The RF gain is set the same way, from the
Calibration tab.

> **Status: experimental, not yet validated on hardware.**
> - **Tested:** both firmwares build, and the protocol and RSSI scaling pass
>   their host tests.
> - **Not yet measured:** tuning to R1/R3/R6/R7, RSSI behaviour during gate
>   passes, and desense from the AP radio.
>
> See [docs/bring-up.md](docs/bring-up.md) for the planned checks.

## Channels

The C5 radio can only be tuned from **5180 to 5885 MHz**.

| Band | Reachable | Not reachable |
|------|-----------|---------------|
| R (RaceBand) | R1 5658, R2, R3 5732, R4, R5, R6 5843, R7 5880 | R8 5917 |
| A / B / F | all | – |
| E | E1–E5 | E6–E8 |
| L | all (5362–5621), but these are far below the 5.66 GHz bootstrap center and untested | – |

Frequencies that fall between Wi-Fi channels are reached with the same
experimental `phy_set_freq()` step that C5VRX uses.

## Hardware

- 2 × Seeed Studio XIAO ESP32-C5.
- Optional:
  - A buzzer on the AP node's D8.
  - A 1S battery on the AP node's battery pads.
  - A **WS2812B strip around the gate** on the AP node's D4, with its own
    5 V supply. See [docs/hardware.md](docs/hardware.md#ws2812b-gate-led-strip-ap-node-d4).

**Join only these four pins between the boards:** 5V, GND, D6 and D7. The RF
node streams its 40–80 MHz IQ bus out on D0–D3 and D10, and D2/D3 are
strapping pins, so a full pin-to-pin stack is not safe. See
[docs/hardware.md](docs/hardware.md).

## Repository layout

| Path | What it contains |
|------|------------------|
| `rf-node/` | ESP-IDF v6.0.2 app for the RF node. GPL-3.0, derived from C5VRX. |
| `ap-node/` | PlatformIO / Arduino PhobosLT fork for the AP node. MIT, from PhobosLT. |
| `common/c5link_proto.h` | The shared UART line protocol (MIT). See [docs/protocol.md](docs/protocol.md). |
| `tools/rssi_log.py` | Records and plots RF node RSSI over USB, for calibration. |
| `tests/` | Host unit tests for the protocol and the RSSI scaling. |

## Build and flash

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
cc -std=c99 -Wall -Wextra -Werror -I rf-node/main tests/test_rssi_scale.c -lm -o /tmp/t_rssi && /tmp/t_rssi
```

## Using it

1. Power the stack and join Wi-Fi `PhobosLT_xxxxxx` (password `phoboslt`).
   Then open `http://20.0.0.1`.
2. **Configuration tab:** pick the band and channel (for example R1, R3, R6 or
   R7) and press *Save Configuration*. The *RF node* line should show
   `OK <MHz> MHz …` within a second.
3. **Calibration tab:** watch the live RSSI while you fly or carry the quad
   through the gate.
   - If the peak flat-lines at 255, lower the *RF node gain*.
   - Set *Enter* just below the gate-pass peak and *Exit* well above the
     far-away level, then save.
4. **Race tab:** press *Start Race*.

For finer calibration without the AP node, plug the RF node in over USB and run:

```sh
tools/rssi_log.py /dev/ttyACM0 --freq 5732 --gain 40 --seconds 60 -o pass.csv
tools/rssi_log.py --plot pass.csv
```

## Gate lights

With a WS2812B strip fitted, the gate shows the race state. States higher in
the table take priority.

| State | Lights |
|-------|--------|
| RF node link down or RF error | Purple blink |
| Lap recorded | Three white segments chase around the ring (0.6 s) |
| Race start (tone) | Bright green flash (1 s) |
| Start countdown ("Arm your quad") | Pulsing red. Times out after 15 s if the start never comes. |
| Race running | Green. White while the drone is inside the gate (RSSI ≥ Enter). |
| Idle | Breathing blue, plus an RSSI level meter that fills from *Exit* to *Enter* (useful for calibration) |
| Low battery | Short amber blip every 2 s, over any state |

The start countdown itself still runs in the browser. *Start Race* posts
`/timer/arm`, and the gate turns green when the page posts `/timer/start` at
the tone.

## How the RF node measures RSSI

The RF node brings the ESP32-C5 Wi-Fi PHY up **receive-only**, the same way as C5VRX:
- Packet AGC is disabled and a fixed gain index is forced.
- MODEM_DIAG is routed to GPIOs.
- PARLIO RX captures the Q4/I4 sample bus at 40 MS/s.

Every millisecond it:
1. Captures one 4096-sample window (about 102 µs).
2. Computes the mean I² + Q², treating each 4-bit code as the centre of its
   quantizer bucket.
3. Maps that power logarithmically to 0–255, about 10.8 counts per dB across
   the roughly 23.5 dB range available at one gain.

This is not calibrated dBm. Lap timing only needs a repeatable relative peak,
and PhobosLT's per-sample Kalman filter and enter/exit detection work on it
unchanged.

The C5VRX composite-video path is not used at all. For power measurement
there is no need for gapless sampling, demodulation or the DAC.

## Licensing

- **`rf-node/`** contains code derived from C5VRX and is licensed
  **GPL-3.0-or-later** (see `LICENSE`).
- **`ap-node/`** is derived from PhobosLT and stays **MIT**. The original
  copyright notice is kept in `ap-node/LICENSE`.
- **`common/c5link_proto.h`** is MIT, so both nodes can use it.
