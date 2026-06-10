#include <WiFi.h>

#include "AudioManager.h"
#include "BackgroundUploader.h"
#include "Config.h"
#include "LedRing.h"
#include "OpenAITranscriber.h"
#include "TelegramClient.h"
#include "WakeWordDetector.h"
#include "WifiStatus.h"

void listenForWakeWord();
void handleVoiceRequest();

void setup() {
  ledRing.begin();
  ledRing.setColor(40, 40, 40);

  Serial.begin(115200);
  delay(1500);

  if (!connectWiFi()) {
    Serial.println("WiFi connection failed");
    ledRing.setColor(255, 0, 0);
    return;
  }

  ledRing.setColor(0, 80, 80);

  Serial.print("WiFi connected, IP: ");
  Serial.println(WiFi.localIP());

  bool sent = sendTelegramMessage("Jacob smart speaker is online.");
  Serial.println(sent ? "Telegram test message sent" : "Telegram test message failed");
  ledRing.setColor(sent ? 0 : 255, sent ? 255 : 0, 0);
  delay(1200);
  ledRing.clear();

  ledRing.setColor(0, 0, 255);
  if (!audioManager.begin()) {
    Serial.print("Audio init failed: ");
    Serial.println(audioManager.lastError());
    String message = "Audio init failed: ";
    message += audioManager.lastError();
    sendTelegramMessage(message);
    ledRing.setColor(255, 0, 0);
    return;
  }

  if (!wakeWordDetector.begin()) {
    Serial.print("Wake word init failed: ");
    Serial.println(wakeWordDetector.lastError());
    ledRing.setColor(255, 0, 0);
    return;
  }

  listenForWakeWord();
}

void loop() {
  if (!wakeWordDetector.detected()) {
    delay(50);
    return;
  }

  wakeWordDetector.clear();
  wakeWordDetector.stop();
  handleVoiceRequest();
  wakeWordDetector.begin();
  listenForWakeWord();
}

void listenForWakeWord() {
  ledRing.setColor(0, 0, 255);
}

void handleVoiceRequest() {
  ledRing.setColor(0, 255, 0);
  audioManager.playTone(TALK_READY_TONE_HZ, TALK_READY_TONE_MS);
  RecordedAudio audio = audioManager.recordUntilSilence();
  ledRing.setColor(255, 0, 0);
  audioManager.playTone(DONE_TONE_HZ, DONE_TONE_MS);

  if (!audio.ok) {
    sendTelegramMessage("Voice recording failed or timed out before speech was detected.");
    return;
  }

  ledRing.setColor(255, 0, 0);
  if (!uploadRecordedAudioAsync(audio, "Recorded audio")) {
    sendTelegramMessage("Telegram audio upload failed to start.");
    audioManager.release(audio);
  }

  // Transcription is disabled while testing Telegram audio uploads.
  // TranscriptionResult transcription = transcribeWithOpenAI(audio);
  // if (transcription.ok) {
  //   sendTelegramMessage("Transcript:\n" + transcription.text);
  // } else {
  //   String errorMessage = "OpenAI transcription failed";
  //   errorMessage += "\nHTTP: ";
  //   errorMessage += transcription.httpStatus;
  //   errorMessage += "\n";
  //   errorMessage += transcription.error;
  //   sendTelegramMessage(errorMessage);
  // }
}
