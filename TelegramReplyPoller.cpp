#include "TelegramReplyPoller.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdlib.h>

#include "AudioManager.h"
#include "Config.h"
#include "LedRing.h"
#include "TelegramClient.h"
#include "arduino_secrets.h"

namespace {
volatile bool pollerRunning = false;
volatile bool yellowChaseActive = false;
int64_t nextUpdateOffset = -1;

int64_t extractInt64After(const String &text, const String &key, int startAt = 0) {
  int keyIndex = text.indexOf(key, startAt);
  if (keyIndex < 0) {
    return -1;
  }
  int valueStart = keyIndex + key.length();
  while (valueStart < static_cast<int>(text.length()) && (text[valueStart] == ' ' || text[valueStart] == ':')) {
    valueStart++;
  }

  int64_t value = 0;
  bool foundDigit = false;
  while (valueStart < static_cast<int>(text.length()) && isDigit(text[valueStart])) {
    foundDigit = true;
    value = value * 10 + (text[valueStart] - '0');
    valueStart++;
  }
  return foundDigit ? value : -1;
}

String extractStringAfter(const String &text, const String &key, int startAt = 0) {
  int keyIndex = text.indexOf(key, startAt);
  if (keyIndex < 0) {
    return "";
  }
  int valueStart = keyIndex + key.length();
  int valueEnd = text.indexOf('"', valueStart);
  if (valueEnd < 0) {
    return "";
  }
  String value = text.substring(valueStart, valueEnd);
  value.replace("\\/", "/");
  return value;
}

bool updateIsFromConfiguredChat(const String &updateJson) {
  int64_t fromId = extractInt64After(updateJson, "\"id\":");
  char fromIdText[24];
  snprintf(fromIdText, sizeof(fromIdText), "%lld", static_cast<long long>(fromId));
  return fromId >= 0 && String(fromIdText) == String(TELEGRAM_CHAT_ID);
}

String extractReplyFileId(const String &updateJson, String &kind) {
  int voiceIndex = updateJson.indexOf("\"voice\":");
  if (voiceIndex >= 0) {
    kind = "voice";
    return extractStringAfter(updateJson, "\"file_id\":\"", voiceIndex);
  }

  int audioIndex = updateJson.indexOf("\"audio\":");
  if (audioIndex >= 0) {
    kind = "audio";
    return extractStringAfter(updateJson, "\"file_id\":\"", audioIndex);
  }

  int documentIndex = updateJson.indexOf("\"document\":");
  if (documentIndex >= 0) {
    kind = "document";
    return extractStringAfter(updateJson, "\"file_id\":\"", documentIndex);
  }

  return "";
}

void chaseYellowOnce(uint8_t index) {
  ledRing.setPixel(index, 255, 180, 0);
}

void yellowChaseTask(void *) {
  uint8_t index = 0;
  while (yellowChaseActive) {
    chaseYellowOnce(index++);
    delay(120);
  }
  vTaskDelete(nullptr);
}

void startYellowChase() {
  yellowChaseActive = true;
  TaskHandle_t handle = nullptr;
  xTaskCreatePinnedToCore(yellowChaseTask, "Yellow Chase", 2048, nullptr, 1, &handle, 1);
}

void stopYellowChase() {
  yellowChaseActive = false;
  delay(150);
}

bool isWavFile(const uint8_t *data, size_t size) {
  return data != nullptr && size >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WAVE", 4) == 0;
}

bool handleReplyFile(const String &fileId, const String &kind) {
  String filePath;
  if (!getTelegramFilePath(fileId, filePath)) {
    sendTelegramMessage("Could not get Telegram reply file path.");
    return false;
  }

  uint8_t chase = 0;
  uint8_t *data = nullptr;
  size_t size = 0;
  chaseYellowOnce(chase++);
  bool downloaded = downloadTelegramFile(filePath, &data, size, TELEGRAM_REPLY_MAX_DOWNLOAD_BYTES);
  if (!downloaded) {
    sendTelegramMessage("Could not download Telegram reply audio.");
    return false;
  }

  if (!isWavFile(data, size)) {
    free(data);
    if (kind == "voice" || filePath.endsWith(".oga") || filePath.endsWith(".ogg")) {
      sendTelegramMessage("Telegram voice replies are OGG/Opus. This build can poll and download them, but playback needs ESP-ADF/Opus decoder integration.");
    } else {
      sendTelegramMessage("Telegram reply audio format is not playable yet. WAV/PCM is supported in this build.");
    }
    ledRing.setColor(0, 0, 255);
    return false;
  }

  for (uint8_t i = 0; i < LED_COUNT * 2; i++) {
    chaseYellowOnce(chase++);
    delay(60);
  }
  startYellowChase();
  bool played = audioManager.playWav(data, size);
  stopYellowChase();
  free(data);
  ledRing.setColor(0, 0, 255);

  if (!played) {
    String message = "Could not play Telegram reply WAV: ";
    message += audioManager.lastError();
    sendTelegramMessage(message);
  }
  return played;
}

bool processUpdates(const String &response) {
  bool handledReply = false;
  int searchFrom = 0;

  while (true) {
    int updateIndex = response.indexOf("\"update_id\":", searchFrom);
    if (updateIndex < 0) {
      break;
    }

    int nextUpdateIndex = response.indexOf("\"update_id\":", updateIndex + 12);
    String updateJson = nextUpdateIndex >= 0 ? response.substring(updateIndex, nextUpdateIndex) : response.substring(updateIndex);
    int64_t updateId = extractInt64After(updateJson, "\"update_id\":");
    if (updateId >= 0 && updateId >= nextUpdateOffset) {
      nextUpdateOffset = updateId + 1;
    }

    if (updateIsFromConfiguredChat(updateJson)) {
      String kind;
      String fileId = extractReplyFileId(updateJson, kind);
      if (fileId.length() > 0) {
        handledReply = handleReplyFile(fileId, kind);
        break;
      }
    }

    if (nextUpdateIndex < 0) {
      break;
    }
    searchFrom = nextUpdateIndex;
  }

  return handledReply;
}

void primeUpdateOffset() {
  if (nextUpdateOffset >= 0) {
    return;
  }

  String response;
  if (!getTelegramUpdates(-1, response)) {
    nextUpdateOffset = 0;
    return;
  }

  int searchFrom = 0;
  int64_t maxUpdateId = -1;
  while (true) {
    int updateIndex = response.indexOf("\"update_id\":", searchFrom);
    if (updateIndex < 0) {
      break;
    }
    int64_t updateId = extractInt64After(response, "\"update_id\":", updateIndex);
    if (updateId > maxUpdateId) {
      maxUpdateId = updateId;
    }
    searchFrom = updateIndex + 12;
  }
  nextUpdateOffset = maxUpdateId >= 0 ? maxUpdateId + 1 : 0;
}

void pollTask(void *) {
  pollerRunning = true;
  primeUpdateOffset();
  uint32_t deadline = millis() + TELEGRAM_REPLY_WAIT_TIMEOUT_MS;

  while (millis() < deadline) {
    String response;
    if (getTelegramUpdates(nextUpdateOffset, response) && processUpdates(response)) {
      break;
    }

    uint32_t sleepUntil = millis() + TELEGRAM_REPLY_POLL_INTERVAL_MS;
    while (millis() < sleepUntil) {
      delay(100);
    }
  }

  pollerRunning = false;
  vTaskDelete(nullptr);
}
}

void startTelegramReplyPolling() {
  if (pollerRunning) {
    return;
  }

  TaskHandle_t handle = nullptr;
  BaseType_t result = xTaskCreatePinnedToCore(pollTask, "Telegram Poll", 12288, nullptr, 1, &handle, 0);
  if (result != pdPASS) {
    sendTelegramMessage("Telegram reply polling failed to start.");
  }
}
