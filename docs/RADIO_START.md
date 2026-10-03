# Start and stop races from an ExpressLRS radio

You can start a race with a button on your radio, so you don't have to press
*Start Race* on a laptop and then rush to put on your goggles.

- **Short press** (under 1 s): starts the race countdown. You hear "Arm your
  quad / Starting on the tone in less than five", then the tone 1–5 s later.
- **Long press** (hold for 1 s): stops the race, or cancels the countdown.

The AP node buzzer chirps when it recognises a press.

No extra receiver is needed. The ExpressLRS TX module's built-in **TX
backpack** sends the radio's *DVR Rec* switch over ESP-NOW, and the AP node
listens for it. This is the same message RotorHazard's Timer backpack uses.

## Requirements

- ExpressLRS TX firmware **2.3.0 or newer**, with a TX backpack. This covers
  most current TX modules and radios with internal ELRS, which have an
  ESP8285/ESP32 backpack chip.
- TX backpack firmware bound to a known bind phrase (step 1).
- The AP node in its own **access point mode** (the default, with no home Wi-Fi
  SSID configured). ESP-NOW from the backpack is fixed on Wi-Fi channel 1, so
  radio start doesn't work while the timer is joined to a home network.

## 1. Bind the TX backpack to your bind phrase

The timer needs the bind phrase the backpack uses. Use your normal ELRS bind
phrase, the one your TX and receivers use.

- **Flash it in:** in the ExpressLRS Configurator, open the **Backpack** tab,
  pick your TX module's backpack target, enter the **same bind phrase**, and
  flash it. On radios with internal ELRS this is usually done over Wi-Fi.
- **Or bind it:** if the backpack was flashed without a bind phrase, press
  **Bind** in the ELRS Lua script. The TX hands its own UID, which comes from
  its bind phrase, to the backpack.

## 2. Set up the button on the radio (EdgeTX)

1. Pick a free AUX channel. ELRS numbers them from channel 5: **AUX1 = CH5**,
   AUX2 = CH6, …, AUX8 = CH12.
2. **Model → Mixes**, on that channel: Source = a **momentary** switch or button,
   for example **SH** on Radiomaster radios. Weight 100, offset 0.
3. Check on the **Channel Monitor** that the channel is **-100 when released**
   and **+100 while pressed**. If it's the other way round, set the weight to
   -100.

## 3. Turn on DVR Rec in the ELRS Lua script

Open **Tools → ExpressLRS → Backpack**:

| Setting | Value |
| --- | --- |
| Backpack | **On** |
| DVR Rec | **AUX n ▲** for the channel from step 2, where ▲ = "on" when the channel is high |
| DVR Srt Dly | **0s** |
| DVR Stp Dly | **0s** |

The delays don't affect the timer, which reacts to each press straight away.
They only delay goggle DVR recording.

If **DVR Rec** uses the wrong polarity (▼), the timer sees the button as
held all the time. Every release then looks like a new hold, and races stop or
won't start. Fix it by choosing ▲ or inverting the mix in step 2.

## 4. Enter the bind phrase on the timer

1. Connect to the timer's Wi-Fi and open the web UI.
2. **Config** tab:
   - **Radio start (ELRS):** tick it.
   - **ELRS bind phrase:** type the phrase from step 1. The timer stores only
     the 6-byte UID hashed from it, never the phrase. Leave the field empty to
     keep the current UID.
3. Click **Save Configuration**. **ELRS backpack UID** now shows the address
   the timer listens on, for example `4e:04:fd:82:21:55` for the phrase `test`.
4. Open `http://20.0.0.1/status`. The line `ELRS Backpack:` should read
   `listening`.

## 5. Test

1. With the web UI open on the **Race** tab and voice enabled (see
   [VOICE.md](VOICE.md)), short-press the button on the radio.
2. The AP buzzer chirps and the gate turns red. The browser says "Arm your quad
   …", and the tone follows.
3. Hold the button for 1 s. The race stops and the browser says "Race stopped".

The web UI's *Start Race* button uses exactly the same countdown on the timer,
so the button and the radio behave the same way.

## Troubleshooting

`http://20.0.0.1/status` shows an ELRS block under `ELRS Backpack:`. The AP
node's USB serial log (460800 baud) prints the same block every 10 s. Press the
radio button a few times, reload the page, and work down the list:

| Line | What it tells you |
| --- | --- |
| `Start errors` | All three should be `0`. A non-zero `mac` or `esp_now` value means ESP-NOW couldn't start on the C5. |
| `UID … (STA MAC …, channel …)` | The STA MAC must equal the UID, and the channel must be `1`. |
| `On air` | Every ESP-NOW frame the timer hears on its channel, from any device. **0** means the backpack isn't transmitting on channel 1: check that Backpack is On, DVR Rec is set, and the TX firmware is 2.3.0 or newer. |
| `On air … last A -> B` | The backpack's real UID. If `B` differs from `UID`, the bind phrases differ: rebind the backpack or fix the phrase on the timer. |
| `Received` | Frames ESP-NOW delivered to the timer. If `On air` counts up but this stays at 0, the STA MAC override isn't taking effect. |
| `From UID` / `MSP ok` | Frames from the bound backpack, and the ones that parsed. |
| `DVR switch` | Recording-state messages. If other functions arrive (`last function`) but none of these, the DVR Rec AUX setting isn't sending. |
| `Presses` | Classified presses. If presses are counted but the race doesn't start, the timer was already running or counting down. |

## Notes and limitations

- **One message per press edge.** The backpack sends each change once, without
  repeats. If a press is missed, press again.
  - A lost "release" makes a press look long, so it can only stop or cancel a
    race, never start one by accident.
- **Goggles with a VRx backpack** bound to the same phrase also start and stop
  DVR recording on every press.
  - With a short press, recording stops again straight away. If you want DVR
    of the race, start recording in the goggles yourself.
- **AP mode only.** If a home Wi-Fi SSID is configured and the timer joins
  that network, `/status` shows `not running (AP mode only)`.
- **Wi-Fi channel.** The timer's own Wi-Fi access point is fixed on channel 1,
  to match the backpack.
