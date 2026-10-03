# Hardware bring-up checklist

Record the results here (with dates) as they are measured.

## Results so far

**2026-09-30, first hardware session**
- **AP node:**
  - The web UI failed until the LittleFS image was built as disk version 2.0.
  - The link initially failed because D6/D7 carry the C5's UART0 console on
    both boards. It was moved to D5/D9.
- **RF node:**
  - **Capture:** the finite per-sample PARLIO receive starved the CPU after
    the first capture. It was replaced with C5VRX's interrupt-free continuous
    ring.
  - **Memory:** the MAC dump window 0x40830000–0x4083FFFF is now reserved
    from the heap.
- **Link:** works. The AP shows `OK 5880 MHz gain 40 fw 1`, with about 1000
  samples/s and no drops after startup.
- **RSSI:** reads 0 with no transmitter on the channel. It responds with a VTX
  on the channel and more gain ("works somewhat"). It is not calibrated yet.
- **Open issue:** the RF node's USB console stays silent (see `FLASHING.md`).

## 1. RF node alone (USB)

- [ ] Flash the RF node. The boot log shows `RF ready: <MHz> MHz, gain <g>` and `RF node v1 running`.
- [ ] Type `s` and check the reply `STATUS,<mhz>,<gain>,OK,1`.
- [ ] Tune each channel with a VTX powered on nearby. Each must reply `OK` and the RSSI must rise with the VTX on:
  - [ ] `f 5658` (R1)
  - [ ] `f 5732` (R3)
  - [ ] `f 5843` (R6)
  - [ ] `f 5880` (R7)
- [ ] `f 5917` (R8) must reply `ERR_FREQ`.
- [ ] Measure the noise floor with no VTX: `tools/rssi_log.py … --seconds 20`. Record the RSSI mean and spread.
- [ ] Take gate-pass traces at several gains (for example 20 / 30 / 40 / 50). Pick the default gain where:
  - the pass peak stays below 255 and clipping stays low;
  - the far-away level is well above the noise floor.
- [ ] Adjacent channel: with the node on R3 and a second VTX on R1 or R4 at the gate, the R3 RSSI must not reach the enter threshold.
- [ ] If a finite PARLIO receive fails or times out (`iq_power_measure` errors), switch to C5VRX's infinite-ring capture instead: `get_completed_rx_sample_window()` in C5VRX `main/video.c`.

## 2. UART link (both boards stacked)

- [ ] The AP `/status` page shows `RF Node: OK 5658 MHz gain 40 …`.
- [ ] Change the channel in the web UI and save. The RF node retunes, which `/status` confirms.
- [ ] Reset only the RF node. The AP node reapplies the frequency and gain within about 1 s.
- [ ] With the Calibration tab open for 60 s, the `dropped` count stays at 0.

## 3. Coexistence

- [ ] Compare RF node noise-floor and gate-pass RSSI with the AP Wi-Fi idle and with a phone streaming the Calibration tab.
- [ ] If the AP radio desenses the receiver, lower `C5VRX_AP_TX_POWER` in `ap-node/lib/CONFIG/config.h`.

## 4. Gate LED strip

- [ ] Power the strip from its own 5 V supply. The idle state shows breathing blue.
- [ ] Set the LED count in the web UI. The whole ring lights, with no stale pixels past the end.
- [ ] Press *Start Race*: the gate pulses red during the countdown and flashes green at the tone.
- [ ] Carry a VTX through the gate:
  - [ ] Idle: the meter fills.
  - [ ] Race running: the ring turns white while inside, and the lap chase plays.
- [ ] Unplug the RF node: the gate blinks purple within about 3 s.
- [ ] Repeat the desense check from §3 with the strip lit at the chosen brightness.

## 5. ELRS radio start

See [RADIO_START.md](RADIO_START.md) for the radio and backpack setup.

- [ ] In AP mode with *Radio start* on and a bind phrase saved, `/status` shows `ELRS Backpack: listening`.
  - [ ] If it doesn't, check the serial log for `ELRS:` errors. The STA MAC override (`esp_wifi_set_mac`) in `WIFI_AP_STA` mode and `esp_now_init` are the unproven steps on the C5 core.
  - [ ] Check whether the log says the STA protocol with `WIFI_PROTOCOL_LR` failed. If it did, record whether frames still arrive with b/g/n only.
- [ ] Phones and laptops still join the AP, and the web UI works, while ESP-NOW is listening on channel 1.
- [ ] A short press on the radio button starts the countdown, and a 1 s hold stops it, at gate distance and at the far end of the track.
- [ ] Count missed presses over 20 presses at the pilot's position.
- [ ] Repeat the RF node noise-floor check from §3 with ESP-NOW listening.

## 6. Field

- [ ] Tune the enter/exit thresholds and minimum lap time.
- [ ] Compare lap times against video frame timing. The target is ±100 ms.
- [ ] Confirm the XIAO battery-sense scaling (GPIO6/GPIO26) against a multimeter.
