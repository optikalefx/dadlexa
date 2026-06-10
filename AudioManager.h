#pragma once

#include <Arduino.h>
#include <esp_err.h>

struct AudioStats {
  bool ok = false;
  size_t bytesRead = 0;
  uint32_t sampleCount = 0;
  int16_t peak = 0;
  uint32_t rms = 0;
};

struct RecordedAudio {
  bool ok = false;
  uint8_t *wav = nullptr;
  size_t wavSize = 0;
  uint32_t durationMs = 0;
  uint32_t sampleCount = 0;
  int16_t peak = 0;
  uint32_t rms = 0;
};

class AudioManager {
public:
  bool begin();
  AudioStats captureStats(uint16_t seconds);
  RecordedAudio recordUntilSilence();
  esp_err_t readInterleaved(void *buffer, size_t bytesToRead, size_t *bytesRead, uint32_t timeoutMs);
  bool playTone(uint16_t frequencyHz, uint16_t durationMs);
  bool playWav(const uint8_t *wav, size_t wavSize);
  void release(RecordedAudio &audio);
  const char *lastError() const;

private:
  bool beginI2C();
  bool beginI2S();
  bool beginSpeakerCodec();
  bool beginMicCodec();
  bool enableSpeakerAmp();
  bool readTca(uint8_t reg, uint8_t &value);
  bool writeTca(uint8_t reg, uint8_t value);
  bool playPcm16(const int16_t *samples, size_t sampleCount, uint16_t channelCount);
  bool writeSilence(uint16_t durationMs);
  bool fail(const char *step);
  bool fail(const char *step, esp_err_t err);

  bool initialized = false;
  char lastErrorBuffer[96] = "not started";
};

extern AudioManager audioManager;
