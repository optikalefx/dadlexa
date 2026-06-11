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
  - green chase while uploading the WAV to Telegram
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
/Users/sean/.espressif/python_env/idf5.5_py3.14_env/bin/python -m esptool --chip esp32s3 -p /dev/cu.usbmodem2101 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 build-native-app-2/bootloader/bootloader.bin 0x8000 build-native-app-2/partition_table/partition-table.bin 0xe000 build-native-app-2/ota_data_initial.bin 0x10000 build-native-app-2/jacob_smart_speaker.bin 0x310000 build-native-app-2/srmodels/srmodels.bin
```

## Next Latency Work

The current Telegram debug path uploads a complete WAV after recording ends. For faster assistant behavior, stream compressed audio directly to OpenAI while recording and keep Telegram for status/transcript/reply control.
