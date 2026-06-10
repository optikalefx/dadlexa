# Jacob Smart Speaker

Arduino firmware for a Waveshare ESP32-S3 audio/smart-speaker board. The current app listens for a wake word, records speech until silence, plays status tones, and uploads the captured WAV file to Telegram in the background.

## Current Behavior

- Boots, connects to Wi-Fi, and sends `Jacob smart speaker is online.` to Telegram.
- Shows blue while ready/listening.
- Uses WakeNet `Hi ESP` as the wake word for now.
- On wake:
  - green LED ring + high tone = recording started
  - records until silence
  - red LED ring + low tone = recording finished
  - returns to blue quickly while the WAV uploads to Telegram in a background task
- OpenAI transcription code exists but is currently disabled while Telegram audio upload is being tested.

## Board Notes

Known pin/config values:

- LED ring: GPIO `38`, 7 pixels
- I2C: SDA `11`, SCL `10`
- I2S: MCLK `12`, BCLK `13`, LRCK `14`, DIN `15`, DOUT `16`
- Audio: 16 kHz, 16-bit
- Mic codec: ES7210
- Speaker codec: ES8311
- Speaker amp enable: TCA9555 at I2C address `0x20`, port 1 bit 0

## Files

- `jacob-smart-speaker.ino` - main app flow
- `Config.h` - pins, audio thresholds, tones, timeouts
- `AudioManager.*` - app-level audio capture/playback manager
- `WakeWordDetector.*` - WakeNet `Hi ESP` detection
- `BackgroundUploader.*` - background post-recording upload task
- `TelegramClient.*` - Telegram text/audio API calls
- `OpenAITranscriber.*` - Whisper transcription support, currently not called
- `LedRing.*` - LED ring control
- `WifiStatus.*` - Wi-Fi connection helper
- `src/JacobAudioBoard/codecs/` - board codec drivers
- `led-test/`, `speaker-test/`, `audio-manager-test/`, `silence-tuning-test/` - isolated test sketches

## Secrets

Create `arduino_secrets.h` locally. It is ignored by git.

Expected values:

```cpp
#pragma once

#define WIFI_SSID "..."
#define WIFI_PASSWORD "..."
#define OPENAI_API_KEY "..."
#define TELEGRAM_BOT_TOKEN "..."
#define TELEGRAM_CHAT_ID "..."
```

## Build And Upload

Use the ESP-SR partition scheme for the main app so the wake-word model partition exists:

```sh
arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=esp_sr_16,PSRAM=opi jacob-smart-speaker.ino
arduino-cli upload -p /dev/cu.usbmodem2101 --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=esp_sr_16,PSRAM=opi jacob-smart-speaker.ino
```

The ESP-SR model data lives in a separate flash partition labeled `model`. The app loads it with `esp_srmodel_init("model")`. If a test sketch uses a different partition layout, the next main upload may rewrite the large `srmodels.bin` model partition and take longer.

## Recording Thresholds

Current tuned values in `Config.h`:

```cpp
VOICE_START_RMS_THRESHOLD = 35
SILENCE_RMS_THRESHOLD = 18
SILENCE_STOP_MS = 2500
```

These came from `silence-tuning-test`, where room silence was roughly `mono_rms=4-10` and normal speech was roughly `mono_rms=30-300+`.

## Roadmap

- Re-enable OpenAI transcription in the background post-recording path.
- Upload/send both Telegram audio and OpenAI transcription without blocking readiness.
- Later support Telegram voice/audio replies played on the speaker.
- Later support OpenAI audio streaming and SD card playback.
- Replace `Hi ESP` with a custom wake phrase/model if available.
