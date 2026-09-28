# RF node ↔ AP node UART protocol

The protocol is defined in [`common/c5link_proto.h`](../common/c5link_proto.h)
and tested in [`tests/test_c5link_proto.c`](../tests/test_c5link_proto.c).

## Framing

- UART runs at 921600 baud, 8N1, with no flow control.
- Messages are ASCII lines: `<payload>*<HH>\n`.
- `HH` is the upper-case hex XOR of all payload bytes, as in NMEA.
- The receiver drops lines that fail the checksum or have a bad field, and
  resynchronises on the next `\n`.

## Messages

| Direction | Line | Meaning |
|-----------|------|---------|
| RF → AP | `R,<seq>,<rssi>` | One RSSI sample: `seq` 0–255 wraps, `rssi` 0–255. Sent at 1 kHz, about 14 bytes each, so ~140 kbit/s. |
| RF → AP | `S,<mhz>,<gain>,<state>,<fw>` | Status. Sent on boot, after every command, and once a second as a heartbeat. |
| AP → RF | `F,<mhz>` | Tune. The RF node accepts 5180–5885 MHz. |
| AP → RF | `G,<gain>` | Fixed RX gain index, 0–89. |
| AP → RF | `Q` | Ask for status. |

`<state>` is one of the following:

| State | Meaning |
|-------|---------|
| `OK` | Measuring on `<mhz>`. |
| `TUNING` | A retune is in progress. No `R` lines are sent. |
| `ERR_FREQ` | The requested frequency is outside the tunable window. The RF node stays on `<mhz>` but sends no `R` lines, so a wrong channel is never timed. |
| `ERR_RF` | The PHY retune failed. |

Example:

```
R,17,142*63
S,5732,40,OK,1*61
F,5843*60
```

## Ownership and recovery

- **The AP node owns the configuration.** The frequency and gain are stored in
  its PhobosLT config, set from the web UI.
- **The AP node resends until the RF node agrees.** Its `parallelTask` compares
  them with the last `S` line and resends `F` or `G` every 250 ms until they
  match. After `ERR_FREQ` or `ERR_RF` it backs off to every 2 s.
  - If the RF node reboots, the AP node reapplies the settings within about 1 s.
- **The RF node remembers its last settings.** It saves the last good
  frequency and gain in NVS, so it keeps measuring without an AP node, for
  example for USB calibration.
- **Lost samples are counted.** Gaps in `seq` are counted as dropped samples.
  They appear with the link state on the web UI's *RF node* line (`/status`).

## Timing

- The AP node runs one PhobosLT `LapTimer` update per received `R` sample. The
  Kalman filter therefore always sees a fixed 1 kHz input rate, and the
  lap-timer loop's own speed doesn't matter.
- Peak times are taken from the AP node's `millis()` when a sample is
  processed. UART transport and batching add a nearly constant 1–2 ms, which
  cancels out of lap times because each lap is measured peak to peak.
