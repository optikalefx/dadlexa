#pragma once

#include <Arduino.h>

#include "AudioManager.h"

bool sendTelegramMessage(const String &message);
bool sendTelegramAudio(const RecordedAudio &audio, const String &caption = "");
bool getTelegramUpdates(int64_t offset, String &response);
bool getTelegramFilePath(const String &fileId, String &filePath);
bool downloadTelegramFile(const String &filePath, uint8_t **buffer, size_t &size, size_t maxBytes);
