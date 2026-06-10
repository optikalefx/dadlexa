#include <Adafruit_NeoPixel.h>

constexpr uint8_t LED_PIN = 38;
constexpr uint8_t LED_COUNT = 7;

Adafruit_NeoPixel ring(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

void setRing(uint8_t red, uint8_t green, uint8_t blue) {
  for (uint8_t i = 0; i < LED_COUNT; i++) {
    ring.setPixelColor(i, ring.Color(red, green, blue));
  }
  ring.show();
}

void setup() {
  ring.begin();
  ring.setBrightness(40);
  ring.clear();
  ring.show();
}

void loop() {
  setRing(255, 0, 0);
  delay(1000);
  setRing(0, 255, 0);
  delay(1000);
  setRing(0, 0, 255);
  delay(1000);
}
