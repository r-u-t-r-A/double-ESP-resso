# Voice announcements

The web UI speaks lap times through the browser's own text-to-speech (the Web
Speech API, wrapped by `ap-node/data/articulate.min.js`). Nothing is synthesised
on the ESP32. The start tone is different: it is generated with Web Audio, so it
plays even when voice is broken.

Voice needs two things:

1. Voice turned on in the web UI, every time the page is loaded.
2. A text-to-speech voice in the browser that works **without internet**. The
   timer's Wi-Fi AP has no internet, so Chrome's online "Google …" voices stay
   silent without any error.

## 1. Turn voice on in the web UI (every device)

1. Connect to the timer's Wi-Fi and open the web UI.
2. **Config** tab, **Announcer Type**: choose **Lap Time**, **2 Consecutive
   Laps Time** or **3 Consecutive Laps Time**. **Beep** and **None** don't
   speak lap times.
3. Click **Save Configuration**.
4. Click **Enable voice**.
5. Click **Test voice**. You should hear "testing sound for pilot …, 1, 2, 3".

The **Enable voice** setting is not saved. Click it again after every page
reload, or no announcements are spoken. Until you click it, the start tone is
the only sound.

## 2. Fedora Linux, Google Chrome

Chrome on Linux only uses the system speech engine (speech-dispatcher) when it
is started with `--enable-speech-dispatcher`. Without that flag Chrome offers
only its online voices.

### 2.1 Install and check the speech engine

```sh
sudo dnf install speech-dispatcher speech-dispatcher-espeak-ng espeak-ng
spd-say -w "lap timer test"
```

You must hear "lap timer test". If you hear nothing, fix the system audio or
speech-dispatcher first. Chrome can't speak if this doesn't.

### 2.2 Start Chrome with the flag (one-off test)

1. Quit Chrome completely: close every window and check `pgrep -a chrome`
   prints nothing. Chrome ignores the flag if an instance is already running.
2. Run:

   ```sh
   google-chrome-stable --enable-speech-dispatcher
   ```

### 2.3 Make the flag permanent

The Chrome launcher (`/opt/google/chrome/google-chrome`) doesn't read a flags
file, so override the desktop launcher in your home directory instead:

```sh
mkdir -p ~/.local/share/applications
for f in google-chrome.desktop com.google.Chrome.desktop; do
  [ -f /usr/share/applications/$f ] || continue
  sed 's|^Exec=/usr/bin/google-chrome-stable|Exec=/usr/bin/google-chrome-stable --enable-speech-dispatcher|' \
    /usr/share/applications/$f > ~/.local/share/applications/$f
done
update-desktop-database ~/.local/share/applications
```

Log out and back in, or quit Chrome fully, then start it from the app menu.
Chrome updates won't overwrite these copies. To undo the change, delete the two
files from `~/.local/share/applications`.

### 2.4 Check that Chrome sees offline voices

On the timer web UI, open DevTools (F12) → **Console** and run:

```js
speechSynthesis.getVoices().filter(v => v.localService).map(v => v.name + " (" + v.lang + ")")
```

- **Non-empty list** (espeak-ng voices such as `English (America)+…`): the flag
  works. Do section 1 and test.
- **Empty list**: Chrome wasn't started with the flag. Repeat 2.2 and make sure
  no old Chrome process was still running.

To see which voice Chrome uses by default:

```js
speechSynthesis.getVoices().find(v => v.default)
```

If the default voice has `localService: false`, the web UI will still try that
online voice and stay silent while you're on the timer's Wi-Fi. If that
happens, record it in `docs/bring-up.md`; the web UI code then needs to choose
a local voice itself.

## 3. Android, Google Chrome

Chrome on Android uses the phone's text-to-speech engine. It works offline only
if the voice data for your language is downloaded.

1. **Settings → System → Languages (& input) → Text-to-speech output**. The
   path differs by manufacturer; search Settings for "text-to-speech" if you
   can't find it.
2. Set **Preferred engine** to **Speech Services by Google**. On Samsung phones
   the Samsung engine also works if its English voice is downloaded.
3. Tap the gear icon next to the engine → **Install voice data** → **English
   (United States)** (or your language), and download it. Choose a voice
   **without** a cloud/network icon.
4. Turn the **Speech rate** and **Pitch** sliders back to the middle if they were
   changed. The web UI also has its own **Announcer Rate** setting.
5. Test offline: turn on airplane mode, then turn Wi-Fi back on and connect to
   the timer's AP. Tap **Play** (or **Listen to an example**) on the
   Text-to-speech screen. If this is silent, the web UI will be too.
6. Turn up the **media** volume. Speech uses media volume, not ringer volume.
7. When Android warns "This network has no internet access", choose **Stay
   connected**. Otherwise the phone may switch to mobile data and lose the
   timer page.
8. Open the web UI in Chrome and follow section 1.

Optional check: in Chrome, open `chrome://inspect` on the laptop with the phone
connected over USB debugging, and run the console commands from section 2.4.

## 4. Quick troubleshooting

| Symptom | Cause |
| --- | --- |
| Only the start tone, no "Arm your quad" | **Enable voice** wasn't clicked after the page loaded (section 1). |
| "Arm your quad" works but no lap times | **Announcer Type** is **None** or **Beep**. |
| **Test voice** is silent on Fedora | Chrome was started without `--enable-speech-dispatcher` (section 2). |
| **Test voice** is silent on Android | No offline voice data installed, or media volume is down (section 3). |
| Voice works on home Wi-Fi but not on the timer's Wi-Fi | An online voice is in use (sections 2.4 and 3.3). |
