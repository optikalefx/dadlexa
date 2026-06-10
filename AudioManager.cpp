#include "AudioManager.h"

#include <driver/i2c.h>
#include <driver/i2s_std.h>
#include <esp_err.h>
#include <math.h>

#include "Config.h"
#include "src/JacobAudioBoard/codecs/es7210.h"
#include "src/JacobAudioBoard/codecs/es8311.h"

namespace {
constexpr uint8_t ES7210_I2C_ADDR = 0x40;
constexpr i2c_port_t I2C_PORT = I2C_NUM_0;
constexpr size_t READ_BUFFER_BYTES = 1024;
constexpr size_t WAV_HEADER_BYTES = 44;

constexpr uint8_t TCA9555_ADDRESS = 0x20;
constexpr uint8_t TCA9555_OUTPUT_PORT_1 = 0x03;
constexpr uint8_t TCA9555_CONFIG_PORT_1 = 0x07;
constexpr uint8_t SPEAKER_PA_BIT = 0;

i2s_chan_handle_t txChannel = nullptr;
i2s_chan_handle_t rxChannel = nullptr;
es7210_dev_handle_t es7210Handle = nullptr;
es8311_handle_t es8311Handle = nullptr;

void writeLe16(uint8_t *target, uint16_t value) {
  target[0] = value & 0xFF;
  target[1] = (value >> 8) & 0xFF;
}

void writeLe32(uint8_t *target, uint32_t value) {
  target[0] = value & 0xFF;
  target[1] = (value >> 8) & 0xFF;
  target[2] = (value >> 16) & 0xFF;
  target[3] = (value >> 24) & 0xFF;
}

void writeWavHeader(uint8_t *wav, uint32_t dataBytes) {
  memcpy(wav + 0, "RIFF", 4);
  writeLe32(wav + 4, 36 + dataBytes);
  memcpy(wav + 8, "WAVE", 4);
  memcpy(wav + 12, "fmt ", 4);
  writeLe32(wav + 16, 16);
  writeLe16(wav + 20, 1);
  writeLe16(wav + 22, 1);
  writeLe32(wav + 24, AUDIO_SAMPLE_RATE);
  writeLe32(wav + 28, AUDIO_SAMPLE_RATE * 2);
  writeLe16(wav + 32, 2);
  writeLe16(wav + 34, 16);
  memcpy(wav + 36, "data", 4);
  writeLe32(wav + 40, dataBytes);
}

uint32_t calculateRms(const int16_t *samples, size_t sampleCount) {
  if (sampleCount == 0) {
    return 0;
  }

  double sumSquares = 0;
  for (size_t i = 0; i < sampleCount; i++) {
    sumSquares += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
  }
  return static_cast<uint32_t>(sqrt(sumSquares / sampleCount));
}

void appendSample(int16_t *samples, size_t &writtenSamples, size_t maxSamples, int16_t sample, RecordedAudio &audio, double &sumSquares) {
  if (writtenSamples >= maxSamples) {
    return;
  }

  samples[writtenSamples++] = sample;
  int16_t magnitude = sample == INT16_MIN ? INT16_MAX : abs(sample);
  if (magnitude > audio.peak) {
    audio.peak = magnitude;
  }
  sumSquares += static_cast<double>(sample) * static_cast<double>(sample);
}

uint16_t readLe16(const uint8_t *source) {
  return static_cast<uint16_t>(source[0]) | (static_cast<uint16_t>(source[1]) << 8);
}

uint32_t readLe32(const uint8_t *source) {
  return static_cast<uint32_t>(source[0]) |
         (static_cast<uint32_t>(source[1]) << 8) |
         (static_cast<uint32_t>(source[2]) << 16) |
         (static_cast<uint32_t>(source[3]) << 24);
}
}

AudioManager audioManager;

bool AudioManager::begin() {
  if (initialized) {
    return true;
  }

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

  initialized = true;
  snprintf(lastErrorBuffer, sizeof(lastErrorBuffer), "ok");
  return true;
}

bool AudioManager::beginI2C() {
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

bool AudioManager::beginI2S() {
  i2s_chan_config_t channelConfig = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  channelConfig.dma_desc_num = 8;
  channelConfig.dma_frame_num = 256;
  channelConfig.auto_clear_after_cb = true;

  esp_err_t err = i2s_new_channel(&channelConfig, &txChannel, &rxChannel);
  if (err != ESP_OK) {
    return fail("i2s_new_channel", err);
  }

  i2s_std_config_t stdConfig = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
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

bool AudioManager::beginSpeakerCodec() {
  es8311Handle = es8311_create(I2C_PORT, ES8311_ADDRESS_0);
  if (es8311Handle == nullptr) {
    return fail("es8311_create");
  }

  es8311_clock_config_t clockConfig = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency = static_cast<int>(AUDIO_SAMPLE_RATE * 256),
    .sample_frequency = static_cast<int>(AUDIO_SAMPLE_RATE),
  };

  esp_err_t err = es8311_init(es8311Handle, &clockConfig, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
  if (err != ESP_OK) {
    return fail("es8311_init", err);
  }
  err = es8311_voice_volume_set(es8311Handle, SPEAKER_VOLUME, nullptr);
  if (err != ESP_OK) {
    return fail("es8311_volume", err);
  }
  err = es8311_microphone_config(es8311Handle, false);
  if (err != ESP_OK) {
    return fail("es8311_mic_config", err);
  }
  err = es8311_voice_mute(es8311Handle, false);
  if (err != ESP_OK) {
    return fail("es8311_unmute", err);
  }
  return enableSpeakerAmp();
}

bool AudioManager::beginMicCodec() {
  es7210_i2c_config_t i2cConfig = {
    .i2c_port = I2C_PORT,
    .i2c_addr = ES7210_I2C_ADDR,
  };

  esp_err_t err = es7210_new_codec(&i2cConfig, &es7210Handle);
  if (err != ESP_OK) {
    return fail("es7210_create", err);
  }

  es7210_codec_config_t codecConfig = {
    .sample_rate_hz = AUDIO_SAMPLE_RATE,
    .mclk_ratio = 256,
    .i2s_format = ES7210_I2S_FMT_I2S,
    .bit_width = ES7210_I2S_BITS_16B,
    .mic_bias = ES7210_MIC_BIAS_2V87,
    .mic_gain = ES7210_MIC_GAIN_30DB,
    .flags = {
      .tdm_enable = 1,
    },
  };

  err = es7210_config_codec(es7210Handle, &codecConfig);
  if (err != ESP_OK) {
    return fail("es7210_config", err);
  }
  err = es7210_config_volume(es7210Handle, 0);
  if (err != ESP_OK) {
    return fail("es7210_volume", err);
  }
  return true;
}

bool AudioManager::readTca(uint8_t reg, uint8_t &value) {
  esp_err_t err = i2c_master_write_read_device(I2C_PORT, TCA9555_ADDRESS, &reg, 1, &value, 1, pdMS_TO_TICKS(1000));
  if (err != ESP_OK) {
    fail("tca_read", err);
    return false;
  }
  return true;
}

bool AudioManager::writeTca(uint8_t reg, uint8_t value) {
  uint8_t payload[2] = {reg, value};
  esp_err_t err = i2c_master_write_to_device(I2C_PORT, TCA9555_ADDRESS, payload, sizeof(payload), pdMS_TO_TICKS(1000));
  if (err != ESP_OK) {
    fail("tca_write", err);
    return false;
  }
  return true;
}

bool AudioManager::enableSpeakerAmp() {
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

AudioStats AudioManager::captureStats(uint16_t seconds) {
  AudioStats stats;
  if (!initialized || rxChannel == nullptr) {
    fail("stats_no_rx");
    return stats;
  }

  uint8_t buffer[READ_BUFFER_BYTES];
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

esp_err_t AudioManager::readInterleaved(void *buffer, size_t bytesToRead, size_t *bytesRead, uint32_t timeoutMs) {
  if (!initialized || rxChannel == nullptr) {
    if (bytesRead != nullptr) {
      *bytesRead = 0;
    }
    fail("read_no_rx");
    return ESP_ERR_INVALID_STATE;
  }
  return i2s_channel_read(rxChannel, buffer, bytesToRead, bytesRead, timeoutMs);
}

RecordedAudio AudioManager::recordUntilSilence() {
  RecordedAudio audio;
  if (!initialized || rxChannel == nullptr) {
    fail("record_no_rx");
    return audio;
  }

  const size_t maxSamples = static_cast<size_t>(AUDIO_SAMPLE_RATE) * MAX_RECORD_SECONDS;
  const size_t maxDataBytes = maxSamples * sizeof(int16_t);
  uint8_t *wav = static_cast<uint8_t *>(ps_malloc(WAV_HEADER_BYTES + maxDataBytes));
  if (wav == nullptr) {
    wav = static_cast<uint8_t *>(malloc(WAV_HEADER_BYTES + maxDataBytes));
  }
  if (wav == nullptr) {
    fail("record_alloc_wav");
    return audio;
  }

  int16_t *monoSamples = reinterpret_cast<int16_t *>(wav + WAV_HEADER_BYTES);
  size_t writtenSamples = 0;
  uint32_t silenceMs = 0;
  bool heardVoice = false;
  uint8_t voiceConfirmChunks = 0;
  double sumSquares = 0;
  uint32_t waitStartMs = millis();
  const size_t preRollSamples = (static_cast<size_t>(AUDIO_SAMPLE_RATE) * AUDIO_PREROLL_MS) / 1000;
  int16_t *preRoll = static_cast<int16_t *>(ps_malloc(preRollSamples * sizeof(int16_t)));
  if (preRoll == nullptr) {
    preRoll = static_cast<int16_t *>(malloc(preRollSamples * sizeof(int16_t)));
  }
  if (preRoll == nullptr) {
    free(wav);
    fail("record_alloc_preroll");
    return audio;
  }
  size_t preRollWriteIndex = 0;
  size_t preRollCount = 0;

  uint8_t readBuffer[READ_BUFFER_BYTES];
  int16_t chunkMono[READ_BUFFER_BYTES / (sizeof(int16_t) * 2)];

  while (writtenSamples < maxSamples) {
    uint32_t readStartMs = millis();
    size_t bytesRead = 0;
    esp_err_t err = i2s_channel_read(rxChannel, readBuffer, sizeof(readBuffer), &bytesRead, 1000);
    if (err == ESP_ERR_TIMEOUT || bytesRead == 0) {
      continue;
    }
    if (err != ESP_OK) {
      free(preRoll);
      free(wav);
      fail("i2s_record_read", err);
      return audio;
    }
    uint32_t chunkElapsedMs = max<uint32_t>(1, millis() - readStartMs);

    int16_t *stereo = reinterpret_cast<int16_t *>(readBuffer);
    size_t frameCount = bytesRead / (sizeof(int16_t) * 2);
    size_t chunkCount = 0;

    for (size_t frame = 0; frame < frameCount && writtenSamples < maxSamples; frame++) {
      int32_t left = stereo[frame * 2];
      int32_t right = stereo[frame * 2 + 1];
      int16_t mono = static_cast<int16_t>((left + right) / 2);
      chunkMono[chunkCount++] = mono;

      if (heardVoice) {
        appendSample(monoSamples, writtenSamples, maxSamples, mono, audio, sumSquares);
      } else {
        preRoll[preRollWriteIndex] = mono;
        preRollWriteIndex = (preRollWriteIndex + 1) % preRollSamples;
        if (preRollCount < preRollSamples) {
          preRollCount++;
        }
      }
    }

    uint32_t chunkRms = calculateRms(chunkMono, chunkCount);
    uint32_t chunkMs = max<uint32_t>(chunkElapsedMs, (chunkCount * 1000UL) / AUDIO_SAMPLE_RATE);

    if (!heardVoice) {
      if (chunkRms >= VOICE_START_RMS_THRESHOLD) {
        voiceConfirmChunks++;
        if (voiceConfirmChunks >= VOICE_START_CONFIRM_CHUNKS) {
          heardVoice = true;
          size_t start = preRollCount == preRollSamples ? preRollWriteIndex : 0;
          for (size_t i = 0; i < preRollCount; i++) {
            int16_t sample = preRoll[(start + i) % preRollSamples];
            appendSample(monoSamples, writtenSamples, maxSamples, sample, audio, sumSquares);
          }
        }
      } else {
        voiceConfirmChunks = 0;
      }

      if (!heardVoice && millis() - waitStartMs >= VOICE_WAIT_TIMEOUT_MS) {
        break;
      }
      continue;
    }

    if (chunkRms < SILENCE_RMS_THRESHOLD) {
      silenceMs += chunkMs;
      if (silenceMs >= SILENCE_STOP_MS) {
        break;
      }
    } else {
      silenceMs = 0;
    }
  }

  if (writtenSamples == 0) {
    free(preRoll);
    free(wav);
    return audio;
  }

  uint32_t dataBytes = writtenSamples * sizeof(int16_t);
  writeWavHeader(wav, dataBytes);

  audio.ok = true;
  audio.wav = wav;
  audio.wavSize = WAV_HEADER_BYTES + dataBytes;
  audio.sampleCount = writtenSamples;
  audio.durationMs = (writtenSamples * 1000UL) / AUDIO_SAMPLE_RATE;
  audio.rms = static_cast<uint32_t>(sqrt(sumSquares / writtenSamples));
  free(preRoll);
  return audio;
}

bool AudioManager::playTone(uint16_t frequencyHz, uint16_t durationMs) {
  if (!initialized || txChannel == nullptr) {
    return fail("tone_no_tx");
  }

  constexpr uint16_t FRAMES = 128;
  int16_t samples[FRAMES * 2];
  uint32_t totalFrames = (AUDIO_SAMPLE_RATE * static_cast<uint32_t>(durationMs)) / 1000;
  float phase = 0.0f;
  float phaseStep = 2.0f * PI * static_cast<float>(frequencyHz) / AUDIO_SAMPLE_RATE;

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

bool AudioManager::playWav(const uint8_t *wav, size_t wavSize) {
  if (!initialized || txChannel == nullptr) {
    return fail("play_no_tx");
  }
  if (wav == nullptr || wavSize < WAV_HEADER_BYTES) {
    return fail("wav_invalid");
  }
  if (memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0) {
    return fail("wav_not_riff");
  }

  uint16_t audioFormat = 0;
  uint16_t channels = 0;
  uint32_t sampleRate = 0;
  uint16_t bitsPerSample = 0;
  const uint8_t *data = nullptr;
  size_t dataBytes = 0;
  size_t offset = 12;

  while (offset + 8 <= wavSize) {
    const uint8_t *chunk = wav + offset;
    uint32_t chunkSize = readLe32(chunk + 4);
    offset += 8;
    if (offset + chunkSize > wavSize) {
      return fail("wav_chunk_bounds");
    }

    if (memcmp(chunk, "fmt ", 4) == 0 && chunkSize >= 16) {
      audioFormat = readLe16(wav + offset);
      channels = readLe16(wav + offset + 2);
      sampleRate = readLe32(wav + offset + 4);
      bitsPerSample = readLe16(wav + offset + 14);
    } else if (memcmp(chunk, "data", 4) == 0) {
      data = wav + offset;
      dataBytes = chunkSize;
    }

    offset += chunkSize + (chunkSize % 2);
  }

  if (audioFormat != 1 || bitsPerSample != 16 || (channels != 1 && channels != 2)) {
    return fail("wav_format_unsupported");
  }
  if (sampleRate != AUDIO_SAMPLE_RATE) {
    return fail("wav_rate_unsupported");
  }
  if (data == nullptr || dataBytes == 0) {
    return fail("wav_no_data");
  }

  return playPcm16(reinterpret_cast<const int16_t *>(data), dataBytes / sizeof(int16_t), channels);
}

bool AudioManager::playPcm16(const int16_t *samples, size_t sampleCount, uint16_t channelCount) {
  if (channelCount != 1 && channelCount != 2) {
    return fail("pcm_channels");
  }

  constexpr uint16_t FRAMES = 128;
  int16_t output[FRAMES * 2];
  size_t framesRemaining = sampleCount / channelCount;
  size_t frameOffset = 0;

  while (framesRemaining > 0) {
    uint16_t frames = min<size_t>(FRAMES, framesRemaining);
    for (uint16_t frame = 0; frame < frames; frame++) {
      if (channelCount == 1) {
        int16_t sample = samples[frameOffset + frame];
        output[frame * 2] = sample;
        output[frame * 2 + 1] = sample;
      } else {
        output[frame * 2] = samples[(frameOffset + frame) * 2];
        output[frame * 2 + 1] = samples[(frameOffset + frame) * 2 + 1];
      }
    }

    size_t bytesWritten = 0;
    size_t bytesToWrite = frames * 2 * sizeof(int16_t);
    esp_err_t err = i2s_channel_write(txChannel, output, bytesToWrite, &bytesWritten, 1000);
    if (err != ESP_OK || bytesWritten != bytesToWrite) {
      return fail("i2s_pcm_write", err == ESP_OK ? ESP_ERR_TIMEOUT : err);
    }

    frameOffset += frames;
    framesRemaining -= frames;
  }

  return writeSilence(60);
}

bool AudioManager::writeSilence(uint16_t durationMs) {
  constexpr uint16_t FRAMES = 128;
  int16_t samples[FRAMES * 2] = {};
  uint32_t totalFrames = (AUDIO_SAMPLE_RATE * static_cast<uint32_t>(durationMs)) / 1000;

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

void AudioManager::release(RecordedAudio &audio) {
  free(audio.wav);
  audio.wav = nullptr;
  audio.wavSize = 0;
  audio.ok = false;
}

const char *AudioManager::lastError() const {
  return lastErrorBuffer;
}

bool AudioManager::fail(const char *step) {
  snprintf(lastErrorBuffer, sizeof(lastErrorBuffer), "%s", step);
  return false;
}

bool AudioManager::fail(const char *step, esp_err_t err) {
  snprintf(lastErrorBuffer, sizeof(lastErrorBuffer), "%s: %s", step, esp_err_to_name(err));
  return false;
}
