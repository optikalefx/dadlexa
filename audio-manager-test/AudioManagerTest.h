#pragma once

#include <Arduino.h>

struct AudioStats {
  bool ok = false;
  size_t bytesRead = 0;
  uint32_t sampleCount = 0;
  int16_t peak = 0;
  uint32_t rms = 0;
};

class AudioManagerTest {
public:
  bool begin();
  bool playTone(uint16_t frequencyHz, uint16_t durationMs);
  AudioStats captureMicStats(uint16_t seconds);
  const char *lastError() const;

private:
  bool beginI2C();
  bool beginI2S();
  bool beginSpeakerCodec();
  bool beginMicCodec();
  bool enableSpeakerAmp();
  bool readTca(uint8_t reg, uint8_t &value);
  bool writeTca(uint8_t reg, uint8_t value);
  bool writeSilence(uint16_t durationMs);
  bool fail(const char *step);
  bool fail(const char *step, esp_err_t err);

  char lastErrorBuffer[96] = "not started";
};
