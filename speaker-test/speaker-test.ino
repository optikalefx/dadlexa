#include <Adafruit_NeoPixel.h>
#include <ESP_I2S.h>
#include <Wire.h>
#include <math.h>

#include "es8311.h"

constexpr uint8_t LED_PIN = 38;
constexpr uint8_t LED_COUNT = 7;
constexpr uint8_t I2C_SDA_PIN = 11;
constexpr uint8_t I2C_SCL_PIN = 10;
constexpr uint8_t I2S_MCLK_PIN = 12;
constexpr uint8_t I2S_BCLK_PIN = 13;
constexpr uint8_t I2S_LRCK_PIN = 14;
constexpr uint8_t I2S_DOUT_PIN = 16;
constexpr uint32_t SAMPLE_RATE = 16000;
constexpr uint8_t VOLUME = 65;

constexpr uint8_t TCA9555_ADDRESS = 0x20;
constexpr uint8_t TCA9555_OUTPUT_PORT_1 = 0x03;
constexpr uint8_t TCA9555_CONFIG_PORT_1 = 0x07;
constexpr uint8_t SPEAKER_PA_BIT = 0;

Adafruit_NeoPixel ring(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
I2SClass i2s;
es8311_handle_t codec = nullptr;

void setRing(uint8_t red, uint8_t green, uint8_t blue) {
  for (uint8_t i = 0; i < LED_COUNT; i++) {
    ring.setPixelColor(i, ring.Color(red, green, blue));
  }
  ring.show();
}

bool readTca(uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(TCA9555_ADDRESS);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  if (Wire.requestFrom(TCA9555_ADDRESS, static_cast<uint8_t>(1)) != 1) {
    return false;
  }
  value = Wire.read();
  return true;
}

bool writeTca(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(TCA9555_ADDRESS);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission(true) == 0;
}

bool enableSpeakerAmp() {
  uint8_t config = 0xFF;
  if (!readTca(TCA9555_CONFIG_PORT_1, config)) {
    return false;
  }
  config &= ~(1 << SPEAKER_PA_BIT);
  if (!writeTca(TCA9555_CONFIG_PORT_1, config)) {
    return false;
  }

  uint8_t output = 0x00;
  if (!readTca(TCA9555_OUTPUT_PORT_1, output)) {
    return false;
  }
  output |= (1 << SPEAKER_PA_BIT);
  return writeTca(TCA9555_OUTPUT_PORT_1, output);
}

bool beginSpeaker() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);
  delay(100);

  codec = es8311_create(I2C_NUM_0, ES8311_ADDRESS_0);
  if (codec == nullptr) {
    return false;
  }

  es8311_clock_config_t clockConfig = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency = static_cast<int>(SAMPLE_RATE * 256),
    .sample_frequency = static_cast<int>(SAMPLE_RATE),
  };

  if (es8311_init(codec, &clockConfig, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) {
    return false;
  }
  if (es8311_voice_volume_set(codec, VOLUME, nullptr) != ESP_OK) {
    return false;
  }
  if (es8311_microphone_config(codec, false) != ESP_OK) {
    return false;
  }
  if (es8311_voice_mute(codec, false) != ESP_OK) {
    return false;
  }
  if (!enableSpeakerAmp()) {
    return false;
  }

  i2s.setPins(I2S_BCLK_PIN, I2S_LRCK_PIN, I2S_DOUT_PIN, -1, I2S_MCLK_PIN);
  return i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
}

void playTone(uint16_t frequencyHz, uint16_t durationMs) {
  constexpr uint16_t FRAMES = 128;
  int16_t samples[FRAMES * 2];
  uint32_t totalFrames = (SAMPLE_RATE * static_cast<uint32_t>(durationMs)) / 1000;
  float phase = 0.0f;
  float phaseStep = 2.0f * PI * static_cast<float>(frequencyHz) / SAMPLE_RATE;

  while (totalFrames > 0) {
    uint16_t frames = min<uint32_t>(FRAMES, totalFrames);
    for (uint16_t i = 0; i < frames; i++) {
      int16_t sample = static_cast<int16_t>(sin(phase) * 6000.0f);
      samples[i * 2] = sample;
      samples[i * 2 + 1] = sample;
      phase += phaseStep;
      if (phase >= 2.0f * PI) {
        phase -= 2.0f * PI;
      }
    }
    i2s.write(reinterpret_cast<const uint8_t *>(samples), frames * 2 * sizeof(int16_t));
    totalFrames -= frames;
  }
}

void setup() {
  ring.begin();
  ring.setBrightness(40);
  setRing(40, 40, 40);
  delay(1000);

  setRing(80, 60, 0);
  if (!beginSpeaker()) {
    setRing(255, 0, 0);
    return;
  }

  setRing(0, 255, 0);
}

void loop() {
  playTone(1200, 180);
  delay(350);
  playTone(420, 260);
  delay(1500);
}
