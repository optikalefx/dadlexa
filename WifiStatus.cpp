#include "WifiStatus.h"

#include <Arduino.h>
#include <WiFi.h>

#include "Config.h"
#include "LedRing.h"
#include "arduino_secrets.h"

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    ledRing.blink(80, 60, 0, 150, 150);
  }

  return WiFi.status() == WL_CONNECTED;
}
