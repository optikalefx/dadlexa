#pragma once

#include <Arduino.h>
#include <esp_err.h>

class WakeWordDetector {
public:
  bool begin();
  bool stop();
  bool pause();
  bool resume();
  bool detected();
  void clear();
  void markDetected();
  const char *lastError() const;

private:
  bool fail(const char *step);
  bool fail(const char *step, esp_err_t err);
  static void runTask(void *arg);
  void detectLoop();

  bool running = false;
  volatile bool taskShouldRun = false;
  void *taskHandle = nullptr;
  volatile bool wakeDetected = false;
  char lastErrorBuffer[96] = "not started";
};

extern WakeWordDetector wakeWordDetector;
