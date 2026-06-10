#pragma once

#include <Arduino.h>

#include "AudioManager.h"

struct TranscriptionResult {
  bool ok = false;
  int httpStatus = 0;
  String text;
  String error;
};

TranscriptionResult transcribeWithOpenAI(const RecordedAudio &audio);
