# Gridley firmware

This sketch targets the **ESP32-S3-Touch-AMOLED-1.75C**, the 466 × 466
aluminum-case board. It is not for the similar 1.75 board without the C suffix.

The home screen is a bright blue perspective-grid app launcher with a large white
**GRIDLEY** title, **Chicago Spinner**, and **Settings**. Its prompt is
"PICK A PROGRAM, USER."
Chicago Spinner animates and randomly picks one bar from a locally saved list,
without repeating the immediately previous pick. Its starter list covers bars
around Chicago 60612. It does not check live hours or automatically search for
new places. Settings retains the original battery, USB, charging, and Wi-Fi
status display.

Tap a launcher button to open an app and **BACK** to return. Tap **SPIN** or
**SPIN AGAIN** in the bar app. The bar list and area label can be edited from a
phone or computer on the same Wi-Fi network: open the IP address shown in
Settings. Enter up to 16 lines in `Name | Street address` format. Changes are
saved on Gridley, and the new list is available immediately. Changing the area
label alone does not discover bars; edit the list as well if changing areas.

If no network is configured or a saved connection fails, the board creates a
setup network. Join it and open http://192.168.4.1/ in a browser. The network
name and password appear in Settings. Enter your normal 2.4 GHz Wi-Fi details
on that page. The bar list can also be edited at http://192.168.4.1/bars.
Wi-Fi details and the bar list are saved on the board, not in source control.

When connected, hold the **BOOT** button for three seconds to clear the saved
Wi-Fi details and show the setup network again.

Double tap **BOOT** (two quick press-and-release actions, within about 450 ms)
to play the locally installed sound through the built-in speaker. The sound is
stored in the firmware, so playback works without Wi-Fi. A single tap has no
action. The three-second Wi-Fi reset now waits until BOOT is released before
restarting, preventing an accidental entry into flashing mode.

In **Settings**, tap the volume bar to choose a level from 0 to 100, or use the
**−** and **+** buttons to change it by 10. Changes take effect immediately and
are saved on the board for the next power-on. The status information and volume
control fit on one screen without scrolling.

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
- ESP32 Arduino's built-in `ESP_I2S` library for audio output
- `es8311.c`, `es8311.h`, and `es8311_reg.h` from Waveshare's 1.75C
  `07_ES8311` example (Espressif Apache-2.0 licensed codec driver)

The board pin map and display/PMIC initialization follow Waveshare's official
examples at commit 4bfdc80eedbf95031e384610796bed15e0240312:
https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C

The sketch sets the AXP2101 charger to **100 mA, 4.2 V** for the connected
320 mAh EEMB single-cell LiPo. Check that the battery's red wire reaches the
board's + contact and black wire reaches - before running on or charging
the battery.

## Starter bar list

The initial list contains Cobra Lounge, The Press Room, Inner Town Pub,
Empty Bottle, and Sportsman's Club. Their locations were checked against
[Cobra Lounge](https://cobralounge.com/contact/),
[The Press Room](https://www.pressroomchicago.com/reservations),
[Inner Town Pub](https://www.innertownpub.com/),
[Empty Bottle](https://www.emptybottle.com/faq), and
[Sportsman's Club ordering page](https://order.toasttab.com/online/sportsmans-club-948-north-western-avenue).
The picker is a starting point, not a live recommendation or hours service.

## Backup

backups/factory-original-2026-10-20.bin is a full 32 MB copy of the
board's pre-flash memory. It may contain saved network details, so keep it
private. The file is ignored by Git. Its SHA-256 checksum is in the matching
`.sha256` file.

## Live check, 2026-10-20

The launcher compiled, flashed, and passed upload verification. Its serial
status reported `Wi-Fi: setup | Battery: 3.97 V | USB: yes | charging: yes`.
The board was not yet connected to a home Wi-Fi network at this check.
