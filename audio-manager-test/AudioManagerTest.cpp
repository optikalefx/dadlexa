#include "AudioManagerTest.h"

#include <driver/i2c.h>
#include <driver/i2s_std.h>
#include <esp_err.h>
#include <math.h>

#include "es7210.h"
#include "es8311.h"

namespace {
constexpr uint8_t I2C_SDA_PIN = 11;
constexpr uint8_t I2C_SCL_PIN = 10;
constexpr uint8_t I2S_MCLK_PIN = 12;
constexpr uint8_t I2S_BCLK_PIN = 13;
constexpr uint8_t I2S_LRCK_PIN = 14;
constexpr uint8_t I2S_DIN_PIN = 15;
constexpr uint8_t I2S_DOUT_PIN = 16;

constexpr uint32_t SAMPLE_RATE = 16000;
constexpr uint8_t SPEAKER_VOLUME = 65;
constexpr uint8_t ES7210_I2C_ADDR = 0x40;
constexpr i2c_port_t I2C_PORT = I2C_NUM_0;

constexpr uint8_t TCA9555_ADDRESS = 0x20;
constexpr uint8_t TCA9555_OUTPUT_PORT_1 = 0x03;
constexpr uint8_t TCA9555_CONFIG_PORT_1 = 0x07;
constexpr uint8_t SPEAKER_PA_BIT = 0;

i2s_chan_handle_t txChannel = nullptr;
i2s_chan_handle_t rxChannel = nullptr;
es7210_dev_handle_t micCodec = nullptr;
es8311_handle_t speakerCodec = nullptr;
}

bool AudioManagerTest::begin() {
  if (!beginI2C()) {
    return false;
  }
  if (!beginI2S()) {
    return false;
  }
  delay(100);
  if (!beginSpeakerCodec()) {
    return false;
  }
  if (!beginMicCodec()) {
    return false;
  }

  snprintf(lastErrorBuffer, sizeof(lastErrorBuffer), "ok");
  return true;
}

bool AudioManagerTest::beginI2C() {
  i2c_config_t config = {};
  config.mode = I2C_MODE_MASTER;
  config.sda_io_num = static_cast<gpio_num_t>(I2C_SDA_PIN);
  config.scl_io_num = static_cast<gpio_num_t>(I2C_SCL_PIN);
  config.sda_pullup_en = GPIO_PULLUP_ENABLE;
  config.scl_pullup_en = GPIO_PULLUP_ENABLE;
  config.master.clk_speed = 100000;

  esp_err_t err = i2c_param_config(I2C_PORT, &config);
  if (err != ESP_OK) {
    return fail("i2c_param_config", err);
  }

  err = i2c_driver_install(I2C_PORT, config.mode, 0, 0, 0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    return fail("i2c_driver_install", err);
  }
  return true;
}

bool AudioManagerTest::beginI2S() {
  i2s_chan_config_t channelConfig = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  channelConfig.dma_desc_num = 8;
  channelConfig.dma_frame_num = 256;
  channelConfig.auto_clear_after_cb = true;

  esp_err_t err = i2s_new_channel(&channelConfig, &txChannel, &rxChannel);
  if (err != ESP_OK) {
    return fail("i2s_new_channel", err);
  }

  i2s_std_config_t stdConfig = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = static_cast<gpio_num_t>(I2S_MCLK_PIN),
      .bclk = static_cast<gpio_num_t>(I2S_BCLK_PIN),
      .ws = static_cast<gpio_num_t>(I2S_LRCK_PIN),
      .dout = static_cast<gpio_num_t>(I2S_DOUT_PIN),
      .din = static_cast<gpio_num_t>(I2S_DIN_PIN),
      .invert_flags = {
        .mclk_inv = false,
        .bclk_inv = false,
        .ws_inv = false,
      },
    },
  };

  err = i2s_channel_init_std_mode(txChannel, &stdConfig);
  if (err != ESP_OK) {
    return fail("i2s_init_tx", err);
  }
  err = i2s_channel_init_std_mode(rxChannel, &stdConfig);
  if (err != ESP_OK) {
    return fail("i2s_init_rx", err);
  }
  err = i2s_channel_enable(txChannel);
  if (err != ESP_OK) {
    return fail("i2s_enable_tx", err);
  }
  err = i2s_channel_enable(rxChannel);
  if (err != ESP_OK) {
    return fail("i2s_enable_rx", err);
  }
  return true;
}

bool AudioManagerTest::beginSpeakerCodec() {
  speakerCodec = es8311_create(I2C_PORT, ES8311_ADDRESS_0);
  if (speakerCodec == nullptr) {
    return fail("es8311_create");
  }

  es8311_clock_config_t clockConfig = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency = static_cast<int>(SAMPLE_RATE * 256),
    .sample_frequency = static_cast<int>(SAMPLE_RATE),
  };

  esp_err_t err = es8311_init(speakerCodec, &clockConfig, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
  if (err != ESP_OK) {
    return fail("es8311_init", err);
  }
  err = es8311_voice_volume_set(speakerCodec, SPEAKER_VOLUME, nullptr);
  if (err != ESP_OK) {
    return fail("es8311_volume", err);
  }
  err = es8311_microphone_config(speakerCodec, false);
  if (err != ESP_OK) {
    return fail("es8311_mic_config", err);
  }
  err = es8311_voice_mute(speakerCodec, false);
  if (err != ESP_OK) {
    return fail("es8311_unmute", err);
  }
  return enableSpeakerAmp();
}

bool AudioManagerTest::beginMicCodec() {
  es7210_i2c_config_t i2cConfig = {
    .i2c_port = I2C_PORT,
    .i2c_addr = ES7210_I2C_ADDR,
  };

  esp_err_t err = es7210_new_codec(&i2cConfig, &micCodec);
  if (err != ESP_OK) {
    return fail("es7210_create", err);
  }

  es7210_codec_config_t codecConfig = {
    .sample_rate_hz = SAMPLE_RATE,
    .mclk_ratio = 256,
    .i2s_format = ES7210_I2S_FMT_I2S,
    .bit_width = ES7210_I2S_BITS_16B,
    .mic_bias = ES7210_MIC_BIAS_2V87,
    .mic_gain = ES7210_MIC_GAIN_30DB,
    .flags = {
      .tdm_enable = 1,
    },
  };

  err = es7210_config_codec(micCodec, &codecConfig);
  if (err != ESP_OK) {
    return fail("es7210_config", err);
  }
  err = es7210_config_volume(micCodec, 0);
  if (err != ESP_OK) {
    return fail("es7210_volume", err);
  }
  return true;
}

bool AudioManagerTest::readTca(uint8_t reg, uint8_t &value) {
  esp_err_t err = i2c_master_write_read_device(I2C_PORT, TCA9555_ADDRESS, &reg, 1, &value, 1, pdMS_TO_TICKS(1000));
  if (err != ESP_OK) {
    fail("tca_read", err);
    return false;
  }
  return true;
}

bool AudioManagerTest::writeTca(uint8_t reg, uint8_t value) {
  uint8_t payload[2] = {reg, value};
  esp_err_t err = i2c_master_write_to_device(I2C_PORT, TCA9555_ADDRESS, payload, sizeof(payload), pdMS_TO_TICKS(1000));
  if (err != ESP_OK) {
    fail("tca_write", err);
    return false;
  }
  return true;
}

bool AudioManagerTest::enableSpeakerAmp() {
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

bool AudioManagerTest::playTone(uint16_t frequencyHz, uint16_t durationMs) {
  if (txChannel == nullptr) {
    return fail("tone_no_tx");
  }

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

    size_t bytesWritten = 0;
    size_t bytesToWrite = frames * 2 * sizeof(int16_t);
    esp_err_t err = i2s_channel_write(txChannel, samples, bytesToWrite, &bytesWritten, 1000);
    if (err != ESP_OK || bytesWritten != bytesToWrite) {
      return fail("i2s_tone_write", err == ESP_OK ? ESP_ERR_TIMEOUT : err);
    }
    totalFrames -= frames;
  }

  return writeSilence(60);
}

bool AudioManagerTest::writeSilence(uint16_t durationMs) {
  constexpr uint16_t FRAMES = 128;
  int16_t samples[FRAMES * 2] = {};
  uint32_t totalFrames = (SAMPLE_RATE * static_cast<uint32_t>(durationMs)) / 1000;

  while (totalFrames > 0) {
    uint16_t frames = min<uint32_t>(FRAMES, totalFrames);
    size_t bytesWritten = 0;
    size_t bytesToWrite = frames * 2 * sizeof(int16_t);
    esp_err_t err = i2s_channel_write(txChannel, samples, bytesToWrite, &bytesWritten, 1000);
    if (err != ESP_OK || bytesWritten != bytesToWrite) {
      return fail("i2s_silence_write", err == ESP_OK ? ESP_ERR_TIMEOUT : err);
    }
    totalFrames -= frames;
  }
  return true;
}

AudioStats AudioManagerTest::captureMicStats(uint16_t seconds) {
  AudioStats stats;
  if (rxChannel == nullptr) {
    fail("stats_no_rx");
    return stats;
  }

  constexpr size_t BUFFER_BYTES = 1024;
  uint8_t buffer[BUFFER_BYTES];
  uint32_t deadline = millis() + static_cast<uint32_t>(seconds) * 1000;
  double sumSquares = 0;

  while (millis() < deadline) {
    size_t bytesRead = 0;
    esp_err_t err = i2s_channel_read(rxChannel, buffer, sizeof(buffer), &bytesRead, 1000);
    if (err == ESP_ERR_TIMEOUT || bytesRead == 0) {
      continue;
    }
    if (err != ESP_OK) {
      fail("i2s_read", err);
      return stats;
    }

    stats.bytesRead += bytesRead;
    int16_t *samples = reinterpret_cast<int16_t *>(buffer);
    size_t sampleCount = bytesRead / sizeof(int16_t);
    for (size_t i = 0; i < sampleCount; i++) {
      int16_t sample = samples[i];
      int16_t magnitude = sample == INT16_MIN ? INT16_MAX : abs(sample);
      if (magnitude > stats.peak) {
        stats.peak = magnitude;
      }
      sumSquares += static_cast<double>(sample) * static_cast<double>(sample);
    }
    stats.sampleCount += sampleCount;
  }

  if (stats.sampleCount > 0) {
    stats.ok = true;
    stats.rms = static_cast<uint32_t>(sqrt(sumSquares / stats.sampleCount));
  }
  return stats;
}

const char *AudioManagerTest::lastError() const {
  return lastErrorBuffer;
}

bool AudioManagerTest::fail(const char *step) {
  snprintf(lastErrorBuffer, sizeof(lastErrorBuffer), "%s", step);
  return false;
}

bool AudioManagerTest::fail(const char *step, esp_err_t err) {
  snprintf(lastErrorBuffer, sizeof(lastErrorBuffer), "%s: %s", step, esp_err_to_name(err));
  return false;
}
