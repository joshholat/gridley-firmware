# Waveshare 1.75C starter

This sketch targets the **ESP32-S3-Touch-AMOLED-1.75C**, the 466 × 466
aluminum-case board. It is not for the similar 1.75 board without the C suffix.

The screen shows whether the AXP2101 detects a battery, its measured voltage,
whether USB power is present, whether the battery is charging, and the Wi-Fi state.
If no network is configured or a saved connection fails, the board creates a setup network. Join the network
and use a phone or computer browser to open http://192.168.4.1/. The network
name and password appear on the board's screen. Enter your normal 2.4 GHz Wi-Fi
details in the page. Those details are saved on the board, not in this source.

When connected, hold the **BOOT** button for three seconds to clear the saved
Wi-Fi details and show the setup network again.

## Source and dependencies

- Sketch: WaveshareStarter/WaveshareStarter.ino
- Board pins: WaveshareStarter/pin_config.h
- Espressif Arduino ESP32 core: **3.3.11**
- Waveshare's bundled **GFX Library for Arduino 1.6.4** and
  **XPowersLib 0.2.6**

The board pin map and display/PMIC initialization follow Waveshare's official
examples at commit 4bfdc80eedbf95031e384610796bed15e0240312:
https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C

The sketch sets the AXP2101 charger to **100 mA, 4.2 V** for the connected
320 mAh EEMB single-cell LiPo. Check that the battery's red wire reaches the
board's + contact and black wire reaches - before running on or charging
the battery.

## Backup

backups/factory-original-2026-10-20.bin is a full 32 MB copy of the
board's pre-flash memory. It may contain saved network details, so keep it
private. The file is ignored by Git. Its SHA-256 checksum is in the matching
`.sha256` file.

## Live check, 2026-10-20

The starter compiled and flashed successfully. Its serial status reports
`Wi-Fi: setup | Battery: not detected | USB: yes | charging: no` while the
user-reported battery is connected. The AXP2101 does not provide a meaningful
battery voltage reading when it marks the battery absent. Confirm the
connector is fully seated and the battery's red/+ and black/- wires match the
board's battery contacts before further charging or unplugged testing.
