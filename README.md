# Jacob Smart Speaker

Native ESP-IDF firmware for the Waveshare ESP32-S3 smart speaker board.

## Current Behavior

- Boots with no LED ring activity.
- Connects to Wi-Fi and sends `online` to Telegram.
- Listens for WakeNet `Hi ESP`.
- On wake:
  - high beep and solid green ring while recording
  - records 16 kHz mono WAV until about 2.5 seconds of silence
  - low beep and blue ring when recording ends
  - starts a background Telegram upload task
- Background task:
  - green chase while preparing, encoding, and uploading Ogg/Opus voice to Telegram
  - blue flash after upload is sent
  - yellow chase while polling Telegram for a reply
  - blue flash after a poll with no reply
  - green flash immediately before playing a received Telegram voice/audio reply

## Source Layout

- `main/main.c` - ESP-IDF entry point, app startup, and main wake loop.
- `main/app_config.*` - board constants and local secrets bridge.
- `main/audio_board.*` - ES8311/ES7210 codec setup, I2S, tones, recording, playback mode switching.
- `main/led_ring.*` - WS2812 LED ring driver and status helpers.
- `main/wake_word.*` - ESP-SR WakeNet `Hi ESP` detection.
- `main/voice_flow.*` - wake/record/upload/poll/playback flow.
- `main/telegram_service.*` - Telegram Bot API calls.
- `main/wifi_service.*` - Wi-Fi station setup.
- `main/micro_opus_player.*` - Telegram OGG/Opus playback.

## Required Local Secrets

Create `arduino_secrets.h` locally. It is ignored by git.

```cpp
#pragma once

#define WIFI_SSID "..."
#define WIFI_PASSWORD "..."
#define TELEGRAM_BOT_TOKEN "..."
#define TELEGRAM_CHAT_ID "..."
```

## Hardware Notes

- LED ring: GPIO `38`, 7 pixels
- I2C: SDA `11`, SCL `10`
- I2S: MCLK `12`, BCLK `13`, LRCK `14`, DIN `15`, DOUT `16`
- Speaker codec: ES8311 at `0x18`
- Mic codec: ES7210 at `0x40`
- Speaker amp enable: TCA9555 at `0x20`, port 1 bit 0
- K1 button: TCA9555 at `0x20`, port 1 bit 1, active low
- K2 button: TCA9555 at `0x20`, port 1 bit 2, active low
- Wake model partition is flashed at `0x310000`

## Build Notes

This app uses ESP-IDF plus ESP-SR from ESP-ADF. The local ESP-ADF checkout is ignored at `idf-tests/.esp-adf/`.

```sh
export ADF_PATH=/Users/sean/arduino/jacob-smart-speaker/idf-tests/.esp-adf
. /Users/sean/.platformio/packages/framework-espidf/export.sh
python /Users/sean/.platformio/packages/framework-espidf/tools/idf.py -B build-native-app-2 build
```

Flash command from the current working setup:

```sh
cd /Users/sean/arduino/jacob-smart-speaker
export ADF_PATH=/Users/sean/arduino/jacob-smart-speaker/idf-tests/.esp-adf
source /Users/sean/.platformio/packages/framework-espidf/export.sh
/Users/sean/.espressif/python_env/idf5.5_py3.14_env/bin/python /Users/sean/.platformio/packages/framework-espidf/tools/idf.py -p /dev/cu.usbmodem2101 flash
```

Monitor
```sh
export ADF_PATH=/Users/sean/arduino/jacob-smart-speaker/idf-tests/.esp-adf && source /Users/sean/.platformio/packages/framework-espidf/export.sh && /Users/sean/.espressif/python_env/idf5.5_py3.14_env/bin/python /Users/sean/.platformio/packages/framework-espidf/tools/idf.py -p /dev/cu.usbmodem2101 monitor
```


TODO

Yes. The cleanest approach is to run a small HTTP server on the ESP32 when it is on Wi-Fi.
A practical design:
Add a local web page on the device
Visit something like http://jacob-speaker.local/ or the device IP.
Page shows current manifest.txt entries and files in /sdcard/songs.

Upload MP3 files over HTTP
Browser form uploads milkshake.mp3.
ESP32 streams the upload directly to /sdcard/songs/milkshake.mp3.
Avoid buffering the whole MP3 in RAM.

Edit or append manifest entries
A form with:phrase: play milkshake
filename: milkshake.mp3

ESP32 rewrites /sdcard/manifest.txt safely.

Reload commands
After manifest update, firmware reloads the SD music library and rebuilds MultiNet commands.
Or simpler: show “restart required” and reboot the device after upload/edit.

Important details:
Add simple auth. At minimum, a password in arduino_secrets.h.
Use temporary files for safe writes:upload to /sdcard/songs/.upload.tmp
rename to final filename after success
write manifest to /sdcard/manifest.tmp
rename over manifest.txt

Validate filenames: allow only letters, numbers, spaces, _, -, and .mp3.
Keep exact matching behavior: manifest value should equal the filename.
I’d probably implement this as:
web_admin.c/h
routes:GET / admin page
GET /api/songs
GET /api/manifest
POST /api/upload
POST /api/manifest
POST /api/reload or POST /api/reboot

The one caveat: reloading MultiNet commands live may be trickier than rebooting because the current code initializes commands once. The simplest robust version is: upload song + update manifest + reboot. On boot, it picks up the new manifest.
