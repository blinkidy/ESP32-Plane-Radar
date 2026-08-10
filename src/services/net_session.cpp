#include "services/net_session.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace services::net {
namespace {

SemaphoreHandle_t s_session_mutex = nullptr;
volatile bool s_held = false;

}  // namespace

void sessionInit() {
  if (s_session_mutex != nullptr) {
    return;
  }
  s_session_mutex = xSemaphoreCreateMutex();
  if (s_session_mutex == nullptr) {
    Serial.println("net: session mutex alloc failed");
  }
}

bool trySession() {
  // Fail open if the mutex could not be created: degrading to the old
  // unserialised behaviour beats refusing to fetch anything at all.
  if (s_session_mutex == nullptr) {
    return true;
  }
  if (xSemaphoreTake(s_session_mutex, 0) != pdTRUE) {
    return false;
  }
  s_held = true;
  return true;
}

void acquireSession() {
  if (s_session_mutex == nullptr) {
    return;
  }
  xSemaphoreTake(s_session_mutex, portMAX_DELAY);
  s_held = true;
}

void releaseSession() {
  if (s_session_mutex == nullptr) {
    return;
  }
  s_held = false;
  xSemaphoreGive(s_session_mutex);
}

bool sessionBusy() { return s_held; }

}  // namespace services::net
