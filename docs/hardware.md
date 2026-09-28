# Hardware

The timer uses two Seeed Studio XIAO ESP32-C5 boards, stacked. One is the
**RF node** and the other is the **AP node**.

## XIAO ESP32-C5 header

| Pin | D0 | D1 | D2 | D3 | D4 | D5 | D6 | D7 | D8 | D9 | D10 |
|-----|----|----|----|----|----|----|----|----|----|----|-----|
| GPIO | 1 | 0 | 25 | 7 | 23 | 24 | 11 | 12 | 8 | 9 | 10 |

## What each board drives

The RF node:

| Pins | Use |
|------|-----|
| D0, D1, D2, D3, D10 and GPIO 3/4/5 (not on the header) | **MODEM_DIAG Q4/I4 outputs**, toggling at 40–80 MHz |
| D6 (GPIO11) | UART TX to the AP node |
| D7 (GPIO12) | UART RX from the AP node |

The AP node:

| Pins | Use |
|------|-----|
| D6 (GPIO11) | UART RX from the RF node |
| D7 (GPIO12) | UART TX to the RF node |
| D8 (GPIO8) | Optional buzzer |
| GPIO27 | User LED |
| GPIO6 / GPIO26 | Battery sense ADC / divider enable |

## Stacking rule: join 5V, GND, D6 and D7 only

When XIAO boards are stacked, each pin meets the same pin on the other board.
Joining **all** pins is not safe, for two reasons:

- The RF node's IQ bus on D0–D3 and D10 would be loaded by the AP node's pins
  and would radiate over a longer trace.
- D2 (GPIO25) and D3 (GPIO7) are ESP32-C5 strapping pins. If the AP node
  resets while the RF node is driving them, the AP node can boot in the wrong
  mode.

Fit header pins only at **5V, GND, D6 and D7**, or wire those four pins
directly. Because the pins meet pin-to-pin, the UART roles cross in firmware:

```
RF node D6 (TX) ─────── D6 (RX) AP node
RF node D7 (RX) ─────── D7 (TX) AP node
RF node 5V      ─────── 5V      AP node
RF node GND     ─────── GND     AP node
```

## Power

One USB-C cable or a battery powers both boards through the 5V link.

- To flash a board, plug in **only that board's** USB cable.
- The XIAO's VBUS back-feed protection has not been verified, so don't
  connect two USB hosts at once.

## RF considerations

- The AP node runs its Wi-Fi AP on 2.4 GHz only, at 11 dBm, a few millimetres
  from a 5.8 GHz receiver. The two bands are far apart, but desense has not
  been measured yet (see `bring-up.md`).
- The RF node uses the board's own antenna path. The RF and AP boards should
  ideally use antennas pointing away from each other.

## Unverified details

- The XIAO ESP32-C5 battery-sense divider ratio and enable polarity. The
  firmware drives GPIO26 high and assumes a 1:2 divider.
- Whether the XIAO user LED is active-low. The firmware assumes it is.

## WS2812B gate LED strip (AP node, D4)

The AP node drives a WS2812B strip around the gate on **D4 (GPIO23)**. D4 is not
joined to the RF node.

```
5 V supply (+) ──┬───────────────────── strip +5V
                 └─ 1000 µF ─┐
5 V supply (−) ──┴───────────┴───────── strip GND ── AP node GND
AP node D4 ── 330 Ω ─────────────────── strip DIN
```

- **Power the strip from its own 5 V supply**, for example a buck converter
  from the flight battery. A XIAO's USB or 5V pin cannot supply it.
  - Tie the supply ground to the AP node's GND.
  - Put the capacitor right at the strip input.
- **Sizing the supply:** a 50 cm diameter gate is about 157 cm around. That is
  about 94 LEDs at 60/m, or 47 at 30/m. Each LED draws up to 60 mA at full
  white.

| Case | Current |
|------|---------|
| 94 LEDs, full brightness, white | 5.6 A |
| 94 LEDs, default brightness cap 80/255, white | about 1.8 A |
| Racing green at the default cap | about 0.5 A |

  Size the supply for the brightness you configure.
- **Level shifting:** 3.3 V data usually works with a short data wire. If the
  first LEDs flicker, add a 74AHCT125 level shifter.
- **Keep it away from the RF node:** route the strip's supply and its wiring
  away from the RF node. Rerun the desense check (bring-up §3) with the strip lit.
- **Powering the XIAO stack from the strip supply:** only feed the stack's 5V
  pin from that supply when no USB cable is plugged in.

The LED count, brightness cap and on/off switch are set in the web UI
(Configuration tab). The colour meanings are listed in `README.md`.
