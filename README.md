# Dadlexa

Native ESP-IDF firmware for the Waveshare ESP32-S3 smart speaker board.

## Current Behavior

- Boots with no LED ring activity.
- Connects to Wi-Fi, starts the local web server, and sends `online` to Telegram.
- Listens for WakeNet `Hi ESP`.
- On wake:
  - enables the speaker path for the interaction
  - 220 ms high beep and solid green ring before recording
  - records 16 kHz mono WAV until about 2.5 seconds of silence
  - 260 ms low beep and blue ring when recording ends
  - checks the recording against the song phrases loaded from the SD card
  - plays the matching MP3 with a yellow ring, or starts the Telegram flow when no song matches
  - returns to quiet idle only after command/song/reply handling finishes
- Audio idle behavior:
  - wake-word listening keeps the microphone and WakeNet active
  - speaker codec output is muted and the external speaker amp is powered down during true idle
  - wake/done beeps are generated at 16 kHz in microphone mode to avoid cutting off the cue
  - the speaker path remains available during the whole interaction and is shut down explicitly at the end
- MP3 playback:
  - reads files from `/sdcard/songs`
  - decodes MP3 audio on the device and switches the codec to the file's sample rate and channel count
  - returns the audio hardware to quiet wake-word idle when playback finishes
  - can be stopped with K1
- Background task:
  - green chase while preparing, encoding, and uploading Ogg/Opus voice to Telegram
  - blue flash after upload is sent
  - yellow chase while polling Telegram for a reply
  - blue flash after a poll with no reply
  - green flash immediately before playing a received Telegram voice/audio reply
- Buttons:
  - K1 replays the latest Telegram reply when an MP3 is not playing
  - K2 replays the most recently uploaded recording

## SD Music Library

The music library is stored on a FAT-formatted SD card:

```text
/manifest.txt
/songs/
  milkshake.mp3
  bedtime.mp3
```

Each non-comment line in `manifest.txt` maps a spoken phrase to an MP3 filename:

```text
play milkshake|milkshake.mp3
play bedtime|bedtime.mp3
```

A manifest value without a path is
loaded from `/sdcard/songs`; `.mp3` is added when the extension is omitted.
Up to 64 manifest entries are loaded.

The SD card and speech commands are initialized on the first wake. If the
recorded phrase matches a manifest entry, the corresponding song plays locally.
Otherwise, the recording continues through the existing Telegram upload and
reply workflow.


## Web Upload

After Wi-Fi connects, open the speaker's IP address in a browser:

```text
http://<device-ip>/
```

The page displays the current manifest entries and MP3 files. To add content:

1. Enter the phrase that should trigger the song.
2. Select an MP3 file.
3. Click **Upload and reboot**.

The upload page shows progress. The server writes the MP3 through
`/sdcard/songs/.upload.tmp`, renames it to the selected filename, appends the
`phrase|filename` mapping to `/sdcard/manifest.txt`, and reboots after about
three seconds. The new command is loaded after the reboot.

Upload constraints:

- Maximum multipart request size: 8 MiB.
- Filenames must end in `.mp3` and may contain letters, numbers, spaces, `_`,
  `-`, and `.`.
- Phrases may contain letters, numbers, spaces, apostrophes, and `-`.
- The web server currently has no authentication, so it should only be exposed
  on a trusted local network.

HTTP endpoints:

- `GET /` - upload and library page.
- `GET /api/list` - JSON containing `manifest` and `songs`.
- `POST /api/upload` - multipart upload with `phrase` and `song` fields.

## Source Layout

- `main/main.c` - ESP-IDF entry point, app startup, and main wake loop.
- `main/app_config.*` - board constants and local secrets bridge.
- `main/audio_board.*` - ES8311/ES7210 codec setup, I2S, speaker amp gating, tones, recording, playback mode switching.
- `main/led_ring.*` - WS2812 LED ring driver and status helpers.
- `main/wake_word.*` - ESP-SR WakeNet `Hi ESP` detection.
- `main/voice_commands.*` - ESP-SR MultiNet song command recognition.
- `main/sd_music_library.*` - SD card mount and manifest loading.
- `main/mp3_file_player.*` - streaming MP3 decode and local playback.
- `main/web_admin.*` - local content listing and upload web server.
- `main/voice_flow.*` - wake, command, playback, upload, and reply flow.
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
- Idle audio state: ES8311 output muted and speaker amp disabled; ES7210 mic remains active for WakeNet
- K1 button: TCA9555 at `0x20`, port 1 bit 1, active low
- K2 button: TCA9555 at `0x20`, port 1 bit 2, active low
- SDMMC: CLK `40`, CMD `42`, D0 `41`, 1-bit bus
- Wake model partition is flashed at `0x310000`

## Build Notes

This app uses ESP-IDF plus ESP-SR from ESP-ADF. The local ESP-ADF checkout is ignored at `idf-tests/.esp-adf/`.

```sh
export ADF_PATH=~/arduino/jacob-smart-speaker/idf-tests/.esp-adf
. ~/.platformio/packages/framework-espidf/export.sh
python ~/.platformio/packages/framework-espidf/tools/idf.py -B build-native-app-2 build
```

Flash command from the current working setup:

```sh
cd ~/arduino/jacob-smart-speaker
export ADF_PATH=~/arduino/jacob-smart-speaker/idf-tests/.esp-adf
source ~/.platformio/packages/framework-espidf/export.sh
~/.espressif/python_env/idf5.5_py3.14_env/bin/python ~/.platformio/packages/framework-espidf/tools/idf.py -p /dev/cu.usbmodem1201 flash
```

The USB modem suffix can change between machines or reconnects; check with
`ls /dev/cu.usbmodem*` and substitute the active port if needed.

Monitor
```sh
export ADF_PATH=~/arduino/jacob-smart-speaker/idf-tests/.esp-adf && source ~/.platformio/packages/framework-espidf/export.sh && ~/.espressif/python_env/idf5.5_py3.14_env/bin/python ~/.platformio/packages/framework-espidf/tools/idf.py -p /dev/cu.usbmodem1201 monitor
```
