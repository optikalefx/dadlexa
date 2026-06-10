#include "WakeWordDetector.h"

#include <ESP_SR.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>
#include <stdlib.h>

extern "C" {
#include "esp_wn_models.h"
#include "model_path.h"
}

#include "AudioManager.h"

namespace {
constexpr const char *MODEL_PARTITION_LABEL = "model";
constexpr const char *WAKE_MODEL_KEYWORD = "hiesp";
constexpr float WAKE_AUDIO_GAIN = 12.0f;

srmodel_list_t *models = nullptr;
const esp_wn_iface_t *wakeNet = nullptr;
model_iface_data_t *wakeNetData = nullptr;

int16_t clampSample(float sample) {
  if (sample > 32767.0f) {
    return 32767;
  }
  if (sample < -32768.0f) {
    return -32768;
  }
  return static_cast<int16_t>(sample);
}
}

WakeWordDetector wakeWordDetector;

bool WakeWordDetector::begin() {
  if (running) {
    return true;
  }

  wakeDetected = false;
  taskShouldRun = false;

  models = esp_srmodel_init(MODEL_PARTITION_LABEL);
  if (models == nullptr) {
    return fail("esp_srmodel_init");
  }

  char *modelName = esp_srmodel_filter(models, ESP_WN_PREFIX, WAKE_MODEL_KEYWORD);
  if (modelName == nullptr) {
    modelName = esp_srmodel_filter(models, ESP_WN_PREFIX, nullptr);
  }
  if (modelName == nullptr) {
    return fail("wakenet_model_not_found");
  }

  wakeNet = esp_wn_handle_from_name(modelName);
  if (wakeNet == nullptr) {
    return fail("esp_wn_handle_from_name");
  }

  wakeNetData = wakeNet->create(modelName, DET_MODE_95);
  if (wakeNetData == nullptr) {
    return fail("wakenet_create");
  }

  Serial.printf(
    "WakeNet started: model=%s sample_rate=%d chunk=%d words=%d threshold=%.3f gain=%.1f\n",
    modelName,
    wakeNet->get_samp_rate(wakeNetData),
    wakeNet->get_samp_chunksize(wakeNetData),
    wakeNet->get_word_num(wakeNetData),
    wakeNet->get_det_threshold(wakeNetData, 1),
    WAKE_AUDIO_GAIN
  );

  taskShouldRun = true;
  TaskHandle_t handle = nullptr;
  BaseType_t taskResult = xTaskCreatePinnedToCore(runTask, "WakeNet Task", 6 * 1024, this, 5, &handle, 0);
  if (taskResult != pdPASS) {
    taskShouldRun = false;
    return fail("wakenet_task_create");
  }

  taskHandle = handle;
  running = true;
  snprintf(lastErrorBuffer, sizeof(lastErrorBuffer), "ok");
  return true;
}

bool WakeWordDetector::stop() {
  if (!running) {
    return true;
  }

  taskShouldRun = false;
  uint32_t deadline = millis() + 1000;
  while (taskHandle != nullptr && millis() < deadline) {
    delay(10);
  }
  if (taskHandle != nullptr) {
    vTaskDelete(static_cast<TaskHandle_t>(taskHandle));
    taskHandle = nullptr;
  }

  if (wakeNet != nullptr && wakeNetData != nullptr) {
    wakeNet->destroy(wakeNetData);
  }
  wakeNetData = nullptr;
  wakeNet = nullptr;

  if (models != nullptr) {
    esp_srmodel_deinit(models);
  }
  models = nullptr;

  running = false;
  return true;
}

bool WakeWordDetector::pause() {
  taskShouldRun = false;
  return true;
}

bool WakeWordDetector::resume() {
  if (!running) {
    return begin();
  }

  wakeDetected = false;
  taskShouldRun = true;
  return true;
}

bool WakeWordDetector::detected() {
  return wakeDetected;
}

void WakeWordDetector::clear() {
  wakeDetected = false;
}

void WakeWordDetector::markDetected() {
  wakeDetected = true;
}

const char *WakeWordDetector::lastError() const {
  return lastErrorBuffer;
}

void WakeWordDetector::runTask(void *arg) {
  static_cast<WakeWordDetector *>(arg)->detectLoop();
}

void WakeWordDetector::detectLoop() {
  const int chunkSamples = wakeNet->get_samp_chunksize(wakeNetData);
  int16_t *monoBuffer = static_cast<int16_t *>(malloc(chunkSamples * sizeof(int16_t)));
  int16_t *stereoBuffer = static_cast<int16_t *>(malloc(chunkSamples * 2 * sizeof(int16_t)));
  if (monoBuffer == nullptr || stereoBuffer == nullptr) {
    free(monoBuffer);
    free(stereoBuffer);
    fail("wakenet_alloc_audio");
    taskHandle = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  uint32_t lastStatsMs = millis();
  uint32_t chunks = 0;
  uint32_t lastRms = 0;
  int16_t lastPeak = 0;

  while (taskShouldRun) {
    size_t bytesRead = 0;
    esp_err_t err = audioManager.readInterleaved(stereoBuffer, chunkSamples * 2 * sizeof(int16_t), &bytesRead, 1000);
    if (err != ESP_OK || bytesRead == 0) {
      delay(10);
      continue;
    }

    size_t framesRead = bytesRead / (sizeof(int16_t) * 2);
    double sumSquares = 0;
    int16_t peak = 0;
    for (size_t frame = 0; frame < framesRead; frame++) {
      int32_t left = stereoBuffer[frame * 2];
      int32_t right = stereoBuffer[frame * 2 + 1];
      int16_t mono = clampSample(((left + right) / 2.0f) * WAKE_AUDIO_GAIN);
      monoBuffer[frame] = mono;
      int16_t magnitude = mono == INT16_MIN ? INT16_MAX : abs(mono);
      if (magnitude > peak) {
        peak = magnitude;
      }
      sumSquares += static_cast<double>(mono) * static_cast<double>(mono);
    }
    for (size_t frame = framesRead; frame < static_cast<size_t>(chunkSamples); frame++) {
      monoBuffer[frame] = 0;
    }

    lastRms = framesRead > 0 ? static_cast<uint32_t>(sqrt(sumSquares / framesRead)) : 0;
    lastPeak = peak;
    chunks++;

    wakenet_state_t result = wakeNet->detect(wakeNetData, monoBuffer);
    if (result == WAKENET_DETECTED || result > 0) {
      Serial.printf("Wake word detected: rms=%lu peak=%d chunks=%lu\n", static_cast<unsigned long>(lastRms), lastPeak, static_cast<unsigned long>(chunks));
      markDetected();
      taskShouldRun = false;
      break;
    }

    if (millis() - lastStatsMs >= 5000) {
      Serial.printf("WakeNet listening: rms=%lu peak=%d chunks=%lu\n", static_cast<unsigned long>(lastRms), lastPeak, static_cast<unsigned long>(chunks));
      lastStatsMs = millis();
    }
  }

  free(monoBuffer);
  free(stereoBuffer);
  taskHandle = nullptr;
  vTaskDelete(nullptr);
}

bool WakeWordDetector::fail(const char *step) {
  snprintf(lastErrorBuffer, sizeof(lastErrorBuffer), "%s", step);
  Serial.print("Wake word error: ");
  Serial.println(lastErrorBuffer);
  return false;
}

bool WakeWordDetector::fail(const char *step, esp_err_t err) {
  snprintf(lastErrorBuffer, sizeof(lastErrorBuffer), "%s: %s", step, esp_err_to_name(err));
  Serial.print("Wake word error: ");
  Serial.println(lastErrorBuffer);
  return false;
}
