#include "LedRing.h"

#include <Adafruit_NeoPixel.h>

#include "Config.h"

namespace {
Adafruit_NeoPixel ring(LED_COUNT, LED_PIN, NEO_RGB + NEO_KHZ800);
}

LedRing ledRing;

void LedRing::begin() {
  ring.begin();
  ring.setBrightness(LED_BRIGHTNESS);
  clear();
}

void LedRing::setColor(uint8_t red, uint8_t green, uint8_t blue) {
  for (uint8_t i = 0; i < LED_COUNT; i++) {
    ring.setPixelColor(i, ring.Color(red, green, blue));
  }
  ring.show();
}

void LedRing::setPixel(uint8_t index, uint8_t red, uint8_t green, uint8_t blue) {
  ring.clear();
  ring.setPixelColor(index % LED_COUNT, ring.Color(red, green, blue));
  ring.show();
}

void LedRing::clear() {
  ring.clear();
  ring.show();
}

void LedRing::blink(uint8_t red, uint8_t green, uint8_t blue, uint16_t onMs, uint16_t offMs) {
  setColor(red, green, blue);
  delay(onMs);
  clear();
  delay(offMs);
}
