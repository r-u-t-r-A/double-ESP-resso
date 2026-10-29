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
- **Open issue:** the RF node's USB console stays silent (see [FLASHING.md](FLASHING.md)).

## 1. RF node alone (USB)

Firmware 2 (2026-10-24) replaced the 4-bit MODEM_DIAG/PARLIO capture with
raw 10-bit I/Q captures and a band-power meter (FPVGate C5MK's method; see the
README). None of this has run on our hardware yet. The expected values below
are C5MK's own XIAO ESP32-C5 measurements (chip v1.0, ESP-IDF 5.5.1, gain 30);
we build on ESP-IDF 6.0.2, whose PHY library may differ.

- [ ] Flash the RF node. The boot log shows `boot check: live`,
  `RF ready: 5658 MHz (LO 5648), gain <g> of <max>` and `RF node v2 running`.
- [ ] `d` shows `check=live` (or `live_rx_on`), `gainmax` about 89, and on the
  `DUMP` line `direct` climbing at about 1000/s with `timeouts`, `short` and
  `long` at 0. `gain_reforced` should stay at 0.
  - `check=flat`: the PHY isn't receiving. Stop here.
  - `timeouts` or `short` climbing: the direct trigger doesn't match this
    IDF's engine. Every capture then falls back to the slow stock path.
- [ ] Type `s` and check the reply `STATUS,<mhz>,<gain>,OK,3`.
- [ ] **Spectrum orientation:** with a VTX on R1 and `f 5658`, `m` prints
  `MIRROR,5658,<pilot_db>,<mirror_db>`. The pilot should be 15–35 dB above
  the mirror. If the mirror is the higher one, flip `BP_SPECTRUM_SIGN` in
  `rf-node/main/band_power.h`.
- [ ] Noise floor with no VTX: `tools/rssi_log.py … --seconds 20`. C5MK saw
  -1.7 to -1.9 dB of band power (`filt_db`), so RSSI around 18, with a
  filtered spread of about 0.1 dB. `gain_bad` must stay at 0.
- [ ] Tune each channel with a VTX on. Each must reply `OK`, and RSSI must
  rise with the VTX on and return to the floor from the first sample after
  it goes off:
  - [ ] `f 5658` (R1)
  - [ ] `f 5732` (R3)
  - [ ] `f 5843` (R6)
  - [ ] `f 5880` (R7)
  - [ ] `f 5917` (R8, newly reachable)
- [ ] `f 5925` (E7) must reply `ERR_FREQ`.
- [ ] Take gate-pass traces at several gains (for example 20 / 25 / 30 / 35).
  Pick the default gain where:
  - the pass peak stays below 255 and `clip_permille` stays near 0;
  - the far-away level is well above the noise floor.
- [ ] Adjacent channel: with the node on R3 and a second VTX on R1 or R4 at the
  gate, the R3 RSSI must not reach the enter threshold. Image rejection is
  only about 18 dB, so also try a VTX 20 MHz *below* the pilot (its mirror).

## 2. UART link (both boards stacked)

- [ ] The AP `/status` page shows `RF Node: OK 5658 MHz gain <g> fw 3 …`.
- [ ] Change the channel in the web UI and save. The RF node retunes, which `/status` confirms.
- [ ] Reset only the RF node. The AP node reapplies the frequency and gain within about 1 s.
- [ ] With the Calibration tab open for 60 s, the `dropped` count stays at 0.

## 3. Coexistence

- [ ] Compare RF node noise-floor and gate-pass RSSI with the AP Wi-Fi idle and with a phone streaming the Calibration tab.
- [ ] If the AP radio desenses the receiver, lower `C5VRX_AP_TX_POWER` in `ap-node/lib/CONFIG/config.h`.

## 4. Gate LED strip

- [ ] Power the strip from its own 5 V supply. The idle state shows colour twinkles.
- [ ] Set the LED count in the web UI. The whole ring lights, with no stale pixels past the end.
- [ ] Select the strip's **LED chip** and **colour order**. Staging shows orange (two on, one off), and a stop shows pure red.
- [ ] With a phone streaming the Calibration tab (heavy Wi-Fi traffic), the ring shows no flicker or wrong-colour pixels toward its far end. That would mean RMT underruns.
- [ ] Power-up: a 5 s rainbow, then colour twinkles: on a dark ring, random LEDs fade up to yellow, red, pink or purple and back off, about 5 a second on 94 LEDs.
- [ ] Press *Start Race*: "Arm your quad" with the gate orange, three staging tones 1 s apart, then 0.5–4 s later the long start tone with the gate solid green for 4 s, then running green.
- [ ] Stop a countdown before the start tone (web *Stop Race*, or a long press on the radio). The gate shows red for 4 s, then the colour twinkles.
- [ ] *Start tone* = *AP node buzzer*: only the buzzer plays the staging and start tones, and voice still speaks. *Browser*: only the browser plays them.
- [ ] Carry a VTX through the gate:
  - [ ] Idle: the meter fills.
  - [ ] Race running: entering the gate starts a blue sparkle over the green.
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

## 6. SD card logging (AP node)

- [ ] Rewire for the 2026-10-25 pin move: buzzer on AP D0, link joined at **D1** (not D9), and flash both boards. `/status` shows `RF Node: OK …`.
- [ ] Boot with no card: the Config tab's *SD card* field shows `ERROR: no card detected` in red, and the gate LEDs keep running smoothly (no `LED strip: no frame` lines on the console). Insert a FAT32 card and press **Mount SD card**: it shows `OK, SDHC/SDXC, … GB free` in green, with logging still off.
- [ ] An exFAT card (most 64 GB and larger) shows `ERROR: card found … format it as FAT32`.
- [ ] Tick **Log RSSI to SD card** and save: within a few seconds *SD logging* shows `logging /rssi_0001_….csv` with the row count climbing at about 1000/s.
- [ ] Note `Heap: Free` on `/status` with logging on. It should stay well above 40 KB.
- [ ] Log for 10 minutes with the Calibration tab open: `dropped 0`, `errors 0`.
- [ ] Run a race with laps, then **Export last 15 min**. The CSV opens in a spreadsheet, the `time` column matches the clock, and `start`, `lap` (with `lap_ms`) and `stop` rows appear where expected.
- [ ] While logging, press **Unmount SD card**: *SD card* shows `unmounted, safe to remove the card` and *SD logging* `paused`. Pull the card, read the file on a PC (it ends cleanly), put it back and press **Mount SD card**: a new numbered file starts.
- [ ] Unmount during an **Export last 15 min** download: the export finishes first, then the card unmounts.
- [ ] Pull the card while logging without unmounting: *SD card* shows `ERROR: card stopped responding`. Pulling it while logging is off shows `ERROR: no card detected` within about 5 s. Neither retries on its own.
- [ ] Desense: compare the RF node's noise floor and a gate pass with logging on and off.

## 7. Auto-calibration

- [ ] Flash both nodes (RF firmware 3 sends the clip field). Pass close to the antenna at a high gain: the Calibration tab's status line shows a non-zero clip.
- [ ] Run Auto calibrate on a whoop track; record the locked gain, separation and lap accuracy with the date.

## 8. Field

- [ ] Tune the enter/exit thresholds and minimum lap time.
- [ ] Compare lap times against video frame timing. The target is ±100 ms.
- [ ] Confirm the XIAO battery-sense scaling (GPIO6/GPIO26) against a multimeter.
