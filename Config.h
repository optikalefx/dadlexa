#pragma once

#include <Arduino.h>

constexpr uint8_t LED_PIN = 38;
constexpr uint8_t LED_COUNT = 7;
constexpr uint8_t LED_BRIGHTNESS = 40;
constexpr uint32_t WIFI_TIMEOUT_MS = 20000;

constexpr uint8_t I2C_SDA_PIN = 11;
constexpr uint8_t I2C_SCL_PIN = 10;

constexpr uint8_t I2S_MCLK_PIN = 12;
constexpr uint8_t I2S_BCLK_PIN = 13;
constexpr uint8_t I2S_LRCK_PIN = 14;
constexpr uint8_t I2S_DIN_PIN = 15;
constexpr uint8_t I2S_DOUT_PIN = 16;

constexpr uint32_t AUDIO_SAMPLE_RATE = 16000;
constexpr uint16_t MIC_TEST_SECONDS = 2;
constexpr uint16_t VOICE_START_RMS_THRESHOLD = 35;
constexpr uint8_t VOICE_START_CONFIRM_CHUNKS = 2;
constexpr uint16_t SILENCE_RMS_THRESHOLD = 18;
constexpr uint16_t SILENCE_STOP_MS = 2500;
constexpr uint16_t VOICE_WAIT_TIMEOUT_MS = 10000;
constexpr uint16_t MAX_RECORD_SECONDS = 20;
constexpr uint16_t AUDIO_CHUNK_MS = 100;
constexpr uint16_t AUDIO_PREROLL_MS = 1000;

constexpr uint8_t SPEAKER_VOLUME = 65;
constexpr uint16_t TALK_READY_TONE_HZ = 1200;
constexpr uint16_t TALK_READY_TONE_MS = 180;
constexpr uint16_t DONE_TONE_HZ = 420;
constexpr uint16_t DONE_TONE_MS = 260;

constexpr uint32_t TELEGRAM_REPLY_POLL_INTERVAL_MS = 5000;
constexpr uint32_t TELEGRAM_REPLY_WAIT_TIMEOUT_MS = 300000;
constexpr size_t TELEGRAM_REPLY_MAX_DOWNLOAD_BYTES = 1024 * 1024;
