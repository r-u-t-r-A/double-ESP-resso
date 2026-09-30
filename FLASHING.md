# Flashing double-ESP-resso

The two boards run different firmware:

| Board | Firmware | Build tool |
|-------|----------|------------|
| **AP node** | PhobosLT fork (`ap-node/`) | PlatformIO |
| **RF node** | `rf-node/` | ESP-IDF v6.0.2, via the container below |

## Before you start

- **Label the boards.** Both are identical Seeed XIAO ESP32-C5s. Put an **AP**
  and an **RF** sticker on them before flashing.
- **Plug in only the board you are flashing.** When the boards are stacked,
  the other board is powered through the 5V link. Two USB hosts must not be
  connected at the same time.
- On Linux the board appears as `/dev/ttyACM0` when it is the only one plugged
  in. Check with `ls /dev/ttyACM*`. Your user needs access to the port (the
  `dialout` group on most distros).
- Close any serial monitor on the port before flashing.

**Tools**

- [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/) (`pio`).
  It also provides `esptool`.
- To build the RF node, either:
  - [Podman](https://podman.io/) or Docker with the `docker.io/espressif/idf:v6.0.2` image, or
  - a local ESP-IDF v6.0.2 install.

All commands below run from the repository root unless a `cd` says otherwise.

## 1. AP node

PlatformIO builds the firmware and picks the files and flash addresses itself.
Run both commands: the first writes the firmware, the second writes the web UI.

```sh
cd ap-node
pio run -e XIAO_C5 -t upload   --upload-port /dev/ttyACM0   # firmware
pio run -e XIAO_C5 -t uploadfs --upload-port /dev/ttyACM0   # web UI (LittleFS)
```

<details>
<summary>What gets written, for flashing manually with esptool</summary>

Files are in `ap-node/.pio/build/XIAO_C5/`:

| Address | File |
|---------|------|
| `0x2000` | `bootloader.bin` |
| `0x8000` | `partitions.bin` |
| `0xe000` | `boot_app0.bin` (from the Arduino core: `~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin`) |
| `0x10000` | `firmware.bin` |
| `0x670000` | `littlefs.bin` (built with `pio run -e XIAO_C5 -t buildfs`) |

Flash mode is DIO, flash size is 8 MB and flash frequency is 80 MHz. The
partition layout is Arduino `default_8MB.csv`.

</details>

**Check the AP node**

1. Press **RESET**.
2. A Wi-Fi network `PhobosLT_xxxxxx` appears. The password is `phoboslt`.
3. Join it and open `http://20.0.0.1`.

The *RF node* line shows `DOWN`, and a connected LED strip blinks purple,
until the RF node is flashed and stacked. That is expected at this stage.

After the first flash, and after any firmware that changes the config format,
the saved settings reset to defaults once.

## 2. RF node

Unplug the AP node and plug in the RF node.

> **Enter download mode by hand.** While the RF node firmware is running, its
> USB serial port does not respond (see *Known issue* below), so esptool's
> automatic reset fails with `Write timeout`. Before every RF node flash:
> hold **BOOT**, tap **RESET** (or replug USB), then release BOOT.

**Build.** Pick the command for your setup:

```sh
# with Podman (use `docker` instead of `podman` for Docker)
podman run --rm -v $PWD:/project:Z -w /project/rf-node docker.io/espressif/idf:v6.0.2 idf.py build

# or with a local ESP-IDF v6.0.2
cd rf-node && idf.py set-target esp32c5 build && cd ..
```

**Flash.** This uses the esptool bundled with PlatformIO:

```sh
cd rf-node
~/.platformio/penv/bin/python -m esptool --chip esp32c5 -p /dev/ttyACM0 -b 921600 \
  --before no_reset --after watchdog_reset \
  write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m \
  0x2000  build/bootloader/bootloader.bin \
  0x8000  build/partition_table/partition-table.bin \
  0x10000 build/rf-node.bin
```

With a local ESP-IDF you can run `idf.py -p /dev/ttyACM0 flash` from `rf-node/`
instead.

Use the underscore spelling (`write_flash`, `--flash_mode`) shown above.
PlatformIO may install esptool v4 or v5 depending on which platform ran last,
and only the underscore form works in both.

| Address | File (in `rf-node/build/`) |
|---------|----------------------------|
| `0x2000` | `bootloader/bootloader.bin` |
| `0x8000` | `partition_table/partition-table.bin` |
| `0x10000` | `rf-node.bin` |

The same list is in `rf-node/build/flasher_args.json` after a build.

`--after watchdog_reset` starts the new firmware straight away. Pressing
RESET afterwards works too.

**Check the RF node** through the AP node (step 3): the web UI's *RF node*
line must show `OK <MHz> MHz gain <g> fw 1 samples …`, with `samples`
climbing by about 1000 per second.

**Known issue: the RF node's USB console.** On the first hardware test
(2026-09-30) the RF node sent nothing over USB and never read input, even
before any radio code ran and with the official USB-Serial-JTAG driver
installed. The node itself runs normally; its boot trace in NVS showed that.
This is still being investigated. Until it is fixed, `tools/rssi_log.py` and
the console commands below do not work; use the AP node's Calibration tab.

Console commands (for when USB works):

| Command | Action |
|---------|--------|
| `f <mhz>` | Tune |
| `g <gain>` | Set fixed gain (0–89) |
| `r` | Toggle the RSSI CSV stream (used by `tools/rssi_log.py`) |
| `d` | Diagnostics: boot stage and capture counters |
| `t` | Boot trace saved in NVS |

## 3. Stack and run

1. Join only **5V, GND, D5 and D9** between the boards (see `docs/hardware.md`).
2. Power the stack from **one** USB cable, or from the battery.
3. Open `http://20.0.0.1`. Within about a second the *RF node* line changes to
   `OK 5658 MHz gain 40 …`.

Continue with `docs/bring-up.md` for the hardware checks.

## Troubleshooting

- **No serial port, or the connection fails:**
  1. Hold **BOOT**.
  2. Tap **RESET**, or replug the USB cable while holding BOOT.
  3. Release BOOT and run the flash command again.

  Press RESET afterwards to start the new firmware.
- **Port is busy:** close `pio device monitor`, `idf.py monitor` or any other
  serial terminal.
- **Permission denied on `/dev/ttyACM0`:** add your user to the `dialout`
  group, then log out and back in.
- **Wiping a board completely**, for example one that previously ran other
  firmware: add this before flashing:

  ```sh
  ~/.platformio/penv/bin/python -m esptool --chip esp32c5 -p /dev/ttyACM0 erase_flash
  ```

  This also clears the RF node's saved channel and gain, and the AP node's settings.
- **`Handler did not handle the request` at `http://20.0.0.1`:** the web UI
  filesystem did not mount. The serial log shows `Mounting LittleFS failed`.
  - The ESP32-C5 Arduino core only mounts LittleFS **disk version 2.0**, and
    the platform builds 2.1 images by default. `targets/XIAO_C5.ini` sets
    `board_build.littlefs_version = 2.0`.
  - Pull the latest repo and run the `uploadfs` step again.
- **Web UI missing or showing old pages** after an AP firmware update: run the
  `uploadfs` step again.
- **RF node shows `DOWN` in the web UI:**
  - **UART wiring:** the roles are crossed **in firmware**, so do **not**
    cross the wires. Connect D5 to D5 and D9 to D9, straight through, the same
    way stacked headers line up.
  - **RF node firmware:** check it on its own over USB (`s` must reply
    `STATUS,…`).
  - **Power:** check that the boards share GND and 5V.
