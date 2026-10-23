# Gridley firmware

Gridley is a little programmable puck: a round touchscreen, motion controls,
a speaker, microphones, and a bright blue grid straight out of a sci-fi arcade.
I built it on the [Waveshare ESP32-S3-Touch-AMOLED-1.75C](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75C), inspired by [Muse Gadgets](https://gadgets.muse.ai/) and *Tron*. The idea is simple: buy an off-the-shelf board, flash this firmware, then make the puck your own.

This is an independent hobby project, not a Muse or Disney product. The firmware
is for the **1.75C aluminum-case board** with a 466 × 466 display. The similarly
named 1.75 board without the **C** uses different hardware.

## Build one

### 1. Get the parts

- [Waveshare ESP32-S3 1.75-inch round AMOLED board, aluminum case, no battery](https://www.amazon.com/dp/B0GGXHXMRW) ([Waveshare specs](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75C))
- [EEMB 402535 3.7 V, 320 mAh LiPo battery with a 1.25 mm two-pin plug](https://www.amazon.com/dp/B08215N9R8), if you want portable power
- A USB-C **data** cable and a computer for flashing

The board can be programmed over USB before you install the battery. Waveshare
also sells a version with a battery included; check the exact board model and
connector polarity if you use different parts.

### 2. Check the battery plug before connecting it

**Do not plug in a battery just because the connector fits.** The linked EEMB
battery's two wires were in the opposite positions from what my board needed.
The fix is to change the wire positions *in the plug housing* so the battery's
positive lead reaches the board's **+** contact and its negative lead reaches
**−**. You are not reversing the electrical polarity supplied to the board.

With the battery and USB disconnected, compare the plug to the polarity marks
on your own board. I used [this video](https://www.youtube.com/watch?v=PaxzYtSDF4E)
to learn how to release and swap the connector contacts. Keep the two bare
contacts from touching each other, and verify the finished plug with a
multimeter before connecting it. If you cannot confirm which contact is
positive, use a battery already wired for this board or get help from someone
experienced with LiPo connectors. Never connect or charge it with polarity
reversed.

### 3. Get the firmware ready

Clone this repository, then open `WaveshareStarter/WaveshareStarter.ino` in the
Arduino IDE:

```sh
git clone https://github.com/joshholat/gridley-firmware.git
cd gridley-firmware
```

Follow [Waveshare's 1.75C Arduino setup guide](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75C/Arduino)
for the ESP32-S3 board settings and USB connection. Install these versions
through the Arduino IDE's Boards and Library Managers:

- **esp32 by Espressif Systems 3.3.11** in Arduino Boards Manager
- **GFX Library for Arduino 1.6.4** and **XPowersLib 0.2.6**
- **SensorLib 0.5.1** for touch and motion sensing

The sketch also uses the ESP32 core's `ESP_I2S` library. Its ES8311 speaker
and ES7210 microphone driver files are already in `WaveshareStarter/`.
Waveshare's example source and library bundle are linked from its setup guide.

If you want the BOOT-button sound in your build, generate the audio header
*before* compiling:

```sh
python3 -m pip install miniaudio
python3 tools/prepare_audio.py path/to/your-clip.mp3
```

Use an MP3 up to 15 seconds long. The converter writes the ignored
`WaveshareStarter/audio_clip.h` file. Without it, the rest of the firmware
still builds, but the double tap has no sound to play. The script uses
`assets/tron-legacy.mp3` if you omit the path; check that you have rights to
use or redistribute any clip you choose.

### 4. Flash it and make it yours

Connect the board to your laptop with the USB-C data cable, select its serial
port in the Arduino IDE, and upload `WaveshareStarter/WaveshareStarter.ino`.
Flashing replaces the factory firmware. If you may want to restore it later,
back up the board's flash first and keep the backup private: it may contain
saved Wi-Fi details. If upload fails, see [Waveshare's flashing guide](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75C/Firmware-Flashing)
for recovery and download mode.

An AI coding agent with access to this repository, your Arduino toolchain, and
the board's USB port can do the software setup and flashing with you. For
example, ask it:

> Read this README and Waveshare's Arduino guide for the **1.75C** board.
> Install the listed dependencies, compile `WaveshareStarter/WaveshareStarter.ino`,
> upload it over USB, and verify that Gridley boots. Then help me change the
> launcher or add a program of my own. Confirm the board and port before
> flashing; back up the factory image first if I want to keep it.

Hardware work still needs a human to check the battery connector and attach
the battery safely. Once the firmware is running, edit the sketch to put
whatever you want on the puck.

## First run

Gridley opens to a blue perspective-grid launcher. Tap a program to open it
and **BACK** to return. The current firmware includes:

| Program | What it does |
| --- | --- |
| **Grid Maze** | Tilt or tap to guide a puck through a maze, dodge a sentinel, and press BOOT to fire a short-range missile. |
| **Disc Defender** | Tilt or drag to aim a shield around the core; press BOOT or tap the center to charge a block. |
| **Gridley Says** | Follow timed shake, twist, tilt, tap, swipe, and BOOT commands. |
| **Sound Grid** | Turn the two microphones into a live neon sound visualizer. Audio stays on the device. |
| **Settings** | Check battery, charging, USB, Wi-Fi, volume, and program-flash usage. |

If Gridley has no saved Wi-Fi connection, it creates a setup network. Its name
and password appear in **Settings**. Join that network and open
<http://192.168.4.1/> to enter your 2.4 GHz Wi-Fi details. The details are
saved on the board, not in this repository. Once connected, open
<http://gridley.local/> or the IP address shown in Settings to see the device
page. Hold **BOOT** for three seconds to clear the saved Wi-Fi configuration.

Outside the games, double tap **BOOT** to play the sound compiled into your
firmware, if you generated an audio header. Settings lets you adjust speaker
volume. The Sound Grid dBFS readout shows microphone level relative to digital
full scale; it is not a calibrated sound-pressure meter.

## Make your own program

The main sketch is `WaveshareStarter/WaveshareStarter.ino`; hardware pins are
in `WaveshareStarter/pin_config.h`. Start by changing a launcher label,
color, or screen, then upload again. For a new feature, the existing games
show how to read touch, the QMI8658 motion sensor, the BOOT button, and the
speaker. `tools/prepare_audio.py` converts a short MP3 into audio that can
play without Wi-Fi.

The board setup and pin map follow [Waveshare's 1.75C examples](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C).
The included ES8311 driver comes from Waveshare's `07_ES8311` example
(Espressif Apache-2.0); the ES7210 driver comes from `06_ES7210` (Espressif
MIT). The sketch configures the AXP2101 charger for **100 mA, 4.2 V** for the
320 mAh single-cell battery listed above. Recheck charging settings before
using a different battery.
