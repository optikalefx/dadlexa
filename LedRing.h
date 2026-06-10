#pragma once

#include <Arduino.h>

class LedRing {
public:
  void begin();
  void setColor(uint8_t red, uint8_t green, uint8_t blue);
  void setPixel(uint8_t index, uint8_t red, uint8_t green, uint8_t blue);
  void clear();
  void blink(uint8_t red, uint8_t green, uint8_t blue, uint16_t onMs, uint16_t offMs);
};

extern LedRing ledRing;
