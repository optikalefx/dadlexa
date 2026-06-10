#include <Adafruit_NeoPixel.h>

#include "AudioManagerTest.h"

constexpr uint8_t LED_PIN = 38;
constexpr uint8_t LED_COUNT = 7;

Adafruit_NeoPixel ring(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
AudioManagerTest audio;

void setRing(uint8_t red, uint8_t green, uint8_t blue) {
  for (uint8_t i = 0; i < LED_COUNT; i++) {
    ring.setPixelColor(i, ring.Color(red, green, blue));
  }
  ring.show();
}

void printStats(const AudioStats &stats) {
  Serial.print("mic ok=");
  Serial.print(stats.ok ? "true" : "false");
  Serial.print(" bytes=");
  Serial.print(stats.bytesRead);
  Serial.print(" samples=");
  Serial.print(stats.sampleCount);
  Serial.print(" peak=");
  Serial.print(stats.peak);
  Serial.print(" rms=");
  Serial.println(stats.rms);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  ring.begin();
  ring.setBrightness(40);
  setRing(40, 40, 40);
  delay(700);

  Serial.println("audio manager test boot");
  setRing(80, 60, 0);
  if (!audio.begin()) {
    Serial.print("audio begin failed: ");
    Serial.println(audio.lastError());
    setRing(255, 0, 0);
    return;
  }

  Serial.println("audio begin ok");
  setRing(0, 0, 255);
  delay(500);

  Serial.println("ready tone");
  setRing(0, 255, 0);
  if (!audio.playTone(1200, 180)) {
    Serial.print("ready tone failed: ");
    Serial.println(audio.lastError());
    setRing(255, 0, 0);
    return;
  }

  Serial.println("mic capture");
  setRing(0, 0, 255);
  AudioStats stats = audio.captureMicStats(2);
  printStats(stats);

  Serial.println("done tone");
  setRing(255, 0, 0);
  if (!audio.playTone(420, 260)) {
    Serial.print("done tone failed: ");
    Serial.println(audio.lastError());
    return;
  }

  setRing(stats.ok ? 0 : 255, stats.ok ? 255 : 0, 0);
}

void loop() {
  delay(1000);
}
