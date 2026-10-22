# Gridley firmware

This sketch targets the **ESP32-S3-Touch-AMOLED-1.75C**, the 466 × 466
aluminum-case board. It is not for the similar 1.75 board without the C suffix.

The home screen is a bright blue perspective-grid app launcher with a large white
**GRIDLEY** title, **Chicago Spinner**, **Grid Maze**, **Sound Grid**, and **Settings**. Its prompt is
"PICK A PROGRAM, USER."
Settings appears as a smaller utility button below the three programs.
Chicago Spinner animates and randomly picks one bar from a locally saved list,
without repeating the immediately previous pick. Once Gridley joins Wi-Fi, it
looks up bars and pubs in OpenStreetMap within 3.2 km (about 2 miles) of the
approximate center of Chicago 60612. It saves up to 16 randomly sampled results
and refreshes once a day. The saved list keeps the spinner working offline or
when the public map service is busy. It does not check live hours. Settings
retains the original battery, USB, charging, and Wi-Fi status display.

Tap a launcher button to open an app and **BACK** to return. Tap **SPIN** or
**SPIN AGAIN** in the bar app. From a phone or computer on the same Wi-Fi network,
open `http://gridley.local/bars` or the IP address shown in Settings. The page
shows the current bars, their source, and a **Refresh nearby bars now** button.
It also lets you switch to a custom list of up to 16 lines in
`Name | Street address` format. Editing that list selects it immediately; the
OpenStreetMap cache remains available if you switch back. The automatic search
stays centered on 60612; changing the custom list's area label does not move it.
Gridley has no GPS, so it does not follow your phone's location.

**Grid Maze** creates a new connected Tron-style maze each round. Tilt Gridley
to move the glowing puck through the cyan corridors to the gold exit while
avoiding the pink sentinel. It patrols the maze and follows you down clear
straight corridors. Touching it shows a brief "ZAPPED!" screen, then starts a
new round. The onboard QMI8658 motion sensor drives the puck; tapping the maze
nudges it one square in the direction you tapped. Downward movement needs a
little more tilt, so a normal viewing angle is less likely to move the puck.
Reaching the exit triggers a short pixel-art explosion and congratulations
screen, then starts another new maze automatically. If the sensor is
unavailable, touch control still works.

**Sound Grid** turns the two onboard microphones into a live neon artwork.
The spokes, central orb, and color pulses react to nearby voices, music, and
other sounds. It adjusts to the room's background noise automatically. The
microphones are sampled only while Sound Grid is open; tap **BACK** to stop
listening. The artwork runs entirely on the puck without Wi-Fi or a cloud
service and does not save or transmit audio. Its live **dBFS** number measures
the microphone signal relative to digital full scale: values closer to 0 mean
louder sound. It is not a calibrated environmental dB SPL meter.

If no network is configured or a saved connection fails, the board creates a
setup network. Join it and open http://192.168.4.1/ in a browser. The network
name and password appear in Settings. Enter your normal 2.4 GHz Wi-Fi details
on that page. The bar list can also be edited at http://192.168.4.1/bars.
Wi-Fi details and both bar lists are saved on the board, not in source control.
The OpenStreetMap service requires internet access; the setup network alone
does not provide it. Public Overpass servers may be slow or unavailable, and
some map entries omit street addresses. Gridley retries failed refreshes after
an hour and retains the last working list. Data © OpenStreetMap contributors.

When connected, hold the **BOOT** button for three seconds to clear the saved
Wi-Fi details and show the setup network again.

Double tap **BOOT** (two quick press-and-release actions, within about 450 ms)
to play the locally installed sound through the built-in speaker. The sound is
stored in the firmware, so playback works without Wi-Fi. A single tap has no
action. The three-second Wi-Fi reset now waits until BOOT is released before
restarting, preventing an accidental entry into flashing mode.

In **Settings**, tap the volume bar to choose a level from 0 to 100 in 5%
increments, or use the **−** and **+** buttons to change it by 5%. Changes take
effect immediately and are saved on the board for the next power-on. Settings
also shows the percentage of the program flash partition used by the installed
firmware. This does not include the separate data partition or represent all
32 MB of flash. Everything fits on one screen without scrolling. Battery and
charging values are read when Settings opens; leave and reopen it to refresh
them.

## Add or replace the BOOT sound

The supplied MP3 is committed as `assets/tron-legacy.mp3`. To prepare it for a
fresh build:

1. Install the converter dependency: `python3 -m pip install miniaudio`.
2. Run `python3 tools/prepare_audio.py`.
3. Compile and upload `WaveshareStarter/WaveshareStarter.ino`.

The converter makes an ignored `WaveshareStarter/audio_clip.h` containing
16 kHz stereo PCM audio. Clips must be 15 seconds or shorter. The sketch still
builds without that generated file, but the double tap only logs that no sound
is installed. To use a different clip, pass its path to the converter. The
current board has the 3.32-second clip supplied by the user.

## Source and dependencies

- Sketch: WaveshareStarter/WaveshareStarter.ino
- Board pins: WaveshareStarter/pin_config.h
- Espressif Arduino ESP32 core: **3.3.11**
- Waveshare's bundled **GFX Library for Arduino 1.6.4** and
  **XPowersLib 0.2.6**
- **SensorLib 0.5.1** for the CST9217 touch controller
- The same SensorLib installation drives the QMI8658 tilt sensor
- ESP32 Arduino's built-in `ESP_I2S` library for audio output and microphone input
- `es8311.c`, `es8311.h`, and `es8311_reg.h` from Waveshare's 1.75C
  `07_ES8311` example (Espressif Apache-2.0 licensed codec driver)
- `es7210.cpp`, `es7210.h`, and `audio_hal.h` from Waveshare's 1.75C
  `06_ES7210` example (Espressif MIT licensed microphone driver)

The board pin map and display/PMIC initialization follow Waveshare's official
examples at commit 4bfdc80eedbf95031e384610796bed15e0240312:
https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C

The sketch sets the AXP2101 charger to **100 mA, 4.2 V** for the connected
320 mAh EEMB single-cell LiPo. Check that the battery's red wire reaches the
board's + contact and black wire reaches - before running on or charging
the battery.

## Offline starter bar list

The initial list contains Cobra Lounge, The Press Room, Inner Town Pub,
Empty Bottle, and Sportsman's Club. Their locations were checked against
[Cobra Lounge](https://cobralounge.com/contact/),
[The Press Room](https://www.pressroomchicago.com/reservations),
[Inner Town Pub](https://www.innertownpub.com/),
[Empty Bottle](https://www.emptybottle.com/faq), and
[Sportsman's Club ordering page](https://order.toasttab.com/online/sportsmans-club-948-north-western-avenue).
The starter list is used until an OpenStreetMap refresh succeeds, and remains
available as the custom list. The picker is not a live recommendation or hours
service.

## Backup

backups/factory-original-2026-10-20.bin is a full 32 MB copy of the
board's pre-flash memory. It may contain saved network details, so keep it
private. The file is ignored by Git. Its SHA-256 checksum is in the matching
`.sha256` file.

## Live check, 2026-10-20

The launcher compiled, flashed, and passed upload verification. Its serial
status reported `Wi-Fi: setup | Battery: 3.97 V | USB: yes | charging: yes`.
The board was not yet connected to a home Wi-Fi network at this check.

## Live check, 2026-10-22

The OpenStreetMap build compiled at 46% of the program partition, flashed, and
passed upload verification. Gridley connected to Wi-Fi, and
`http://gridley.local/bars` served the new page. During this check, the
Private.coffee Overpass service timed out and the main Overpass service refused
the connection. Gridley kept its five saved bars and scheduled a later retry.
A successful live map refresh has not yet been observed on the board.

## Sound Grid live check, 2026-10-22

The Sound Grid build compiled at 46% of the program partition. A diagnostic
boot on the connected puck read both ES7210 channels. With the onboard sound
playing, microphone RMS rose from roughly 35 in a quiet room to over 3,000,
and the artwork's level rose to 1.0. The final launcher build then flashed and
passed upload verification. Its serial status showed Wi-Fi connected, speaker
ready, and 4.14 V battery voltage. The display artwork was not visually
inspected from a camera during this check.

## Sensitivity and launcher update, 2026-10-22

The Sound Grid sensitivity was increased for nearby speech, and the artwork
now shows live microphone dBFS. The three programs were regrouped above a
smaller Settings utility button with an opaque background. In a low-volume
onboard playback check, roughly -56 to -54 dBFS produced a visible artwork
level of about 0.25 to 0.56, while quiet readings near -60 dBFS stayed low.
