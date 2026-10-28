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
| RF → AP | `R,<seq>,<rssi>,<clip>` | One RSSI sample: `seq` 0–255 wraps, `rssi` 0–255, `clip` the capture's clipped I/Q samples per mille, capped at 255. Sent at 1 kHz, about 17 bytes each, so ~170 kbit/s, well under the 921600 baud link. RF firmware 2 and older send `R,<seq>,<rssi>`; the AP node still accepts that and reads `clip` as 0. |
| RF → AP | `S,<mhz>,<gain>,<state>,<fw>` | Status. Sent on boot, after every command, and once a second as a heartbeat. |
| AP → RF | `F,<mhz>` | Tune. The RF node accepts 5180–5917 MHz (R8 is the top). |
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
R,17,142,0*7F
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

## Desktop stream (AP node → laptop, Wi-Fi)

The AP node serves the 1 kHz samples the lap timer actually used as a
server-sent event stream at `GET /rssi`. It is a separate event source from the
web UI's `/events`, so browsers don't receive it. Batching and encoding are in
[`ap-node/lib/RSSISTREAM/rssi_stream.h`](../ap-node/lib/RSSISTREAM/rssi_stream.h),
tested by [`tests/test_rssi_stream.c`](../tests/test_rssi_stream.c).

- **Batching:** samples are collected only while a client is connected, and
  sent as one `b` event every 50 ms, about 50 samples each. That is about 6 kB/s.
- **First batch:** a new client starts with fresh samples, not a backlog.

| Event | Data | Meaning |
|-------|------|---------|
| `b` | `<idx0>,<ms0>,<raw hex>,<filt hex>,<dms>` | A batch of samples. |
| `mark` | `start,<race start ms>` | The race started. Lap 1 is timed from here. |
| `mark` | `stop,<ms>` | The race stopped. On connect, the current state is sent as `start` or `stop`. |
| `mark` | `lap,<lap ms>` | A lap was detected. The same value as the `/events` `lap` event. |

Fields of `b`:

- **`idx0`:** the running sample index of the first sample, a `uint32_t` that
  wraps. Consecutive batches continue the index, so a jump means samples were lost.
  - Loss happens when the SSE queue is full (Wi-Fi congestion), or when the web
    task stalls for more than about 0.45 s.
- **`ms0`:** the AP node's `millis()` for the first sample. It is the same clock
  as the lap timer's peak times.
- **`raw hex`:** 2 upper-case hex characters per sample. This is the RSSI as
  received from the RF node.
- **`filt hex`:** 2 upper-case hex characters per sample. This is the
  Kalman-filtered, rounded RSSI that `LapTimer` compares with enter/exit.
- **`dms`:** one decimal digit per sample, the ms since the previous sample
  (`0` for the first), clamped to 9. A sample's time is `ms0` plus the running sum.

Example (3 samples):

```
event: b
data: 81234,1052310,8A8C8F,868687,001
```

Try it with `curl -N http://20.0.0.1/rssi`.

## Calibration endpoints (AP node HTTP)

Auto-calibration runs on the AP node
([`ap-node/lib/CALIBRATION/calibration.h`](../ap-node/lib/CALIBRATION/calibration.h),
tested by [`tests/test_calibration.c`](../tests/test_calibration.c)). The web UI
drives it and speaks its events.

| Request | Meaning |
|---------|---------|
| `POST /calib/start` | Start. Optional JSON body `{"startGain":45,"laps":3}` (gain 5–89, laps 1–20). Replies `{"status":"OK","seq":<n>}`; events after `seq` belong to this run. Replies 409 `{"status":"busy"}` while a race runs or counts down. |
| `POST /calib/stop` | Abort. The configured gain and thresholds are kept. |
| `GET /calib` | Current state, polled every 250 ms by the web UI. |

While calibration runs, `/timer/start` and `/timer/begin` reply 409
`{"status":"calibrating"}`, and the ELRS radio start is ignored. On success the
AP node saves `rfGain`, `enterRssi` and `exitRssi` to its config itself.

Example `GET /calib`:

```json
{"state":"wait_pass","event":"fly_through","seq":12,"gain":41,"lastPeak":238,"lastClip":14,
 "clean":1,"lapsSeen":0,"lapsWanted":3,"enter":0,"exit":0,"sepDb":0.0,
 "events":[[10,"clipped",41],[11,"pass_clean",41],[12,"fly_through",41]]}
```

- **`state`:** `idle`, `starting` (request not yet taken by the lap timer
  loop), `set_gain`, `settle`, `wait_pass`, `pass_tail`, `record_laps`, `done`
  or `failed`.
- **`event` / `seq`:** the newest event, and its number. `seq` keeps counting
  across runs.
- **`events`:** the last 8 events as `[seq, name, arg]`, so a poll that missed
  one can still speak it. `arg` is the gain for `started`, `fly_through`,
  `clipped`, `pass_clean`, `no_pass` and `gain_locked`, the laps to fly for
  `fly_laps`, and the lap number for `lap_seen`.
  Other names: `done`, `fail_too_strong`, `fail_no_signal`,
  `fail_no_separation`, `aborted`.
- **`lastPeak` / `lastClip`:** the filtered peak and the highest clip per mille
  of the last judged pass.
- **`clean`:** clean passes in a row at the current gain.
- **`enter` / `exit` / `sepDb`:** the result once `state` is `done`. `sepDb` is
  the width of the band of working Enter values, in dB (4.25 counts per dB).
