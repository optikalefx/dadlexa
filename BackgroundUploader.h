#pragma once

#include <Arduino.h>

#include "AudioManager.h"

bool uploadRecordedAudioAsync(RecordedAudio &audio, const String &caption = "");
