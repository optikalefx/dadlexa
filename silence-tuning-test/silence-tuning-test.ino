#include "../AudioManager.h"
#include "../Config.h"

namespace {
constexpr size_t TUNING_READ_BUFFER_BYTES = 1024;
constexpr uint32_t REPORT_INTERVAL_MS = 100;

struct ChannelStats {
  uint32_t rms = 0;
  int16_t peak = 0;
};

ChannelStats calculateStats(const int16_t *samples, size_t count, size_t stride, size_t offset) {
  ChannelStats stats;
  if (count == 0) {
    return stats;
  }

  double sumSquares = 0;
  for (size_t i = 0; i < count; i++) {
    int16_t sample = samples[i * stride + offset];
    int16_t magnitude = sample == INT16_MIN ? INT16_MAX : abs(sample);
    if (magnitude > stats.peak) {
      stats.peak = magnitude;
    }
    sumSquares += static_cast<double>(sample) * static_cast<double>(sample);
  }
  stats.rms = static_cast<uint32_t>(sqrt(sumSquares / count));
  return stats;
}

ChannelStats calculateMonoStats(const int16_t *stereo, size_t frameCount) {
  ChannelStats stats;
  if (frameCount == 0) {
    return stats;
  }

  double sumSquares = 0;
  for (size_t frame = 0; frame < frameCount; frame++) {
    int32_t left = stereo[frame * 2];
    int32_t right = stereo[frame * 2 + 1];
    int16_t mono = static_cast<int16_t>((left + right) / 2);
    int16_t magnitude = mono == INT16_MIN ? INT16_MAX : abs(mono);
    if (magnitude > stats.peak) {
      stats.peak = magnitude;
    }
    sumSquares += static_cast<double>(mono) * static_cast<double>(mono);
  }
  stats.rms = static_cast<uint32_t>(sqrt(sumSquares / frameCount));
  return stats;
}
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("Silence tuning test");
  Serial.print("Current voice start RMS threshold: ");
  Serial.println(VOICE_START_RMS_THRESHOLD);
  Serial.print("Current silence RMS threshold: ");
  Serial.println(SILENCE_RMS_THRESHOLD);
  Serial.println("Speak normally, pause, and watch mono_rms values.");
  Serial.println("Columns: mono_rms mono_peak left_rms right_rms state would_start would_stop");

  if (!audioManager.begin()) {
    Serial.print("Audio init failed: ");
    Serial.println(audioManager.lastError());
    while (true) {
      delay(1000);
    }
  }
}

void loop() {
  static uint32_t silenceMs = 0;
  static uint8_t voiceConfirmChunks = 0;
  static bool recording = false;

  uint8_t readBuffer[TUNING_READ_BUFFER_BYTES];
  size_t bytesRead = 0;
  esp_err_t err = audioManager.readInterleaved(readBuffer, sizeof(readBuffer), &bytesRead, 1000);
  if (err != ESP_OK || bytesRead == 0) {
    Serial.print("read failed: ");
    Serial.println(esp_err_to_name(err));
    delay(100);
    return;
  }

  int16_t *stereo = reinterpret_cast<int16_t *>(readBuffer);
  size_t frameCount = bytesRead / (sizeof(int16_t) * 2);
  ChannelStats left = calculateStats(stereo, frameCount, 2, 0);
  ChannelStats right = calculateStats(stereo, frameCount, 2, 1);
  ChannelStats mono = calculateMonoStats(stereo, frameCount);
  uint32_t chunkMs = max<uint32_t>(1, (frameCount * 1000UL) / AUDIO_SAMPLE_RATE);

  bool wouldStart = false;
  bool wouldStop = false;
  if (!recording) {
    if (mono.rms >= VOICE_START_RMS_THRESHOLD) {
      voiceConfirmChunks++;
      if (voiceConfirmChunks >= VOICE_START_CONFIRM_CHUNKS) {
        recording = true;
        silenceMs = 0;
        wouldStart = true;
      }
    } else {
      voiceConfirmChunks = 0;
    }
  } else if (mono.rms < SILENCE_RMS_THRESHOLD) {
    silenceMs += chunkMs;
    if (silenceMs >= SILENCE_STOP_MS) {
      recording = false;
      voiceConfirmChunks = 0;
      silenceMs = 0;
      wouldStop = true;
    }
  } else {
    silenceMs = 0;
  }

  Serial.print("mono_rms=");
  Serial.print(mono.rms);
  Serial.print(" mono_peak=");
  Serial.print(mono.peak);
  Serial.print(" left_rms=");
  Serial.print(left.rms);
  Serial.print(" right_rms=");
  Serial.print(right.rms);
  Serial.print(" state=");
  Serial.print(recording ? "recording" : "waiting");
  Serial.print(" would_start=");
  Serial.print(wouldStart ? "yes" : "no");
  Serial.print(" would_stop=");
  Serial.print(wouldStop ? "yes" : "no");
  Serial.print(" silence_ms=");
  Serial.println(silenceMs);

  delay(REPORT_INTERVAL_MS);
}
