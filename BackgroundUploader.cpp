#include "BackgroundUploader.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdlib.h>

#include "TelegramClient.h"
#include "TelegramReplyPoller.h"

namespace {
struct UploadJob {
  RecordedAudio audio;
  String caption;
};

void uploadTask(void *arg) {
  UploadJob *job = static_cast<UploadJob *>(arg);

  // This task is intentionally the post-recording network lane. When OpenAI
  // transcription is re-enabled, start it from here or split each destination
  // into its own task while this job owns the shared WAV buffer.
  bool sent = sendTelegramAudio(job->audio, job->caption);
  audioManager.release(job->audio);

  if (sent) {
    startTelegramReplyPolling();
  } else {
    sendTelegramMessage("Telegram audio upload failed.");
  }

  delete job;
  vTaskDelete(nullptr);
}
}

bool uploadRecordedAudioAsync(RecordedAudio &audio, const String &caption) {
  if (!audio.ok || audio.wav == nullptr || audio.wavSize == 0) {
    return false;
  }

  UploadJob *job = new UploadJob{audio, caption};
  if (job == nullptr) {
    return false;
  }

  TaskHandle_t handle = nullptr;
  BaseType_t result = xTaskCreatePinnedToCore(uploadTask, "Telegram Upload", 8192, job, 1, &handle, 0);
  if (result != pdPASS) {
    delete job;
    return false;
  }

  audio.wav = nullptr;
  audio.wavSize = 0;
  audio.ok = false;
  return true;
}
