# Hardware bring-up checklist

Nothing below has been done yet. Record the results here (with dates) as they are measured.

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

## 4. Field

- [ ] Tune the enter/exit thresholds and minimum lap time.
- [ ] Compare lap times against video frame timing. The target is ±100 ms.
- [ ] Confirm the XIAO battery-sense scaling (GPIO6/GPIO26) against a multimeter.
