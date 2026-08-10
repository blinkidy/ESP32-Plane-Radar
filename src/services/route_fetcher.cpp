#include "services/route_fetcher.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstring>

#include "services/net_session.h"
#include "services/route_parse.h"
#include "ui/radar_range.h"

namespace services::route {
namespace {

constexpr char kApiBase[] = "https://api.adsbdb.com/v0/callsign/";
constexpr size_t kCallsignLen = 9;
constexpr size_t kRouteLen = 12;
constexpr size_t kCacheSize = 48;
constexpr size_t kQueueDepth = 12;
/** Be a good citizen on a free public API. */
constexpr unsigned long kMinRequestSpacingMs = 600UL;
constexpr uint16_t kHttpTimeoutMs = 6000;
/** The UI must never stall on the cache; a missed frame's route can wait. */
constexpr TickType_t kCacheWaitTicks = pdMS_TO_TICKS(20);
/** Bytes on ESP-IDF (not words). The mbedTLS handshake needs the headroom. */
constexpr uint32_t kTaskStackBytes = 10240;

enum class Slot : uint8_t {
  kFree,
  kPending,
  kResolved,
};

struct CacheEntry {
  char callsign[kCallsignLen];
  char route[kRouteLen];
  uint32_t last_used;
  Slot state;
};

CacheEntry s_cache[kCacheSize];
SemaphoreHandle_t s_cache_mutex = nullptr;
QueueHandle_t s_queue = nullptr;
TaskHandle_t s_task = nullptr;

CacheEntry* findEntryLocked(const char* callsign) {
  for (size_t i = 0; i < kCacheSize; ++i) {
    if (s_cache[i].state != Slot::kFree &&
        strcmp(s_cache[i].callsign, callsign) == 0) {
      return &s_cache[i];
    }
  }
  return nullptr;
}

/** Free slot if there is one, else the least recently used resolved entry. */
CacheEntry* claimSlotLocked() {
  CacheEntry* best = nullptr;
  for (size_t i = 0; i < kCacheSize; ++i) {
    if (s_cache[i].state == Slot::kFree) {
      return &s_cache[i];
    }
    // Never evict a pending entry: its lookup is already in the queue.
    if (s_cache[i].state == Slot::kPending) {
      continue;
    }
    if (best == nullptr || s_cache[i].last_used < best->last_used) {
      best = &s_cache[i];
    }
  }
  return best;
}

void storeResultLocked(const char* callsign, const char* route) {
  CacheEntry* entry = findEntryLocked(callsign);
  if (entry == nullptr) {
    entry = claimSlotLocked();
    if (entry == nullptr) {
      return;
    }
    strncpy(entry->callsign, callsign, kCallsignLen - 1);
    entry->callsign[kCallsignLen - 1] = '\0';
  }
  strncpy(entry->route, route, kRouteLen - 1);
  entry->route[kRouteLen - 1] = '\0';
  entry->last_used = millis();
  entry->state = Slot::kResolved;
}

void storeResult(const char* callsign, const char* route) {
  if (s_cache_mutex == nullptr ||
      xSemaphoreTake(s_cache_mutex, portMAX_DELAY) != pdTRUE) {
    return;
  }
  storeResultLocked(callsign, route);
  xSemaphoreGive(s_cache_mutex);
}

void fetchOne(const char* callsign) {
  String url = kApiBase;
  url += callsign;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.printf("route: http.begin failed for %s\n", callsign);
    return;
  }
  http.setTimeout(kHttpTimeoutMs);
  http.setConnectTimeout(kHttpTimeoutMs);

  const int code = http.GET();
  if (code == HTTP_CODE_OK) {
    char route[kRouteLen];
    parseRouteJson(http.getString().c_str(), route, sizeof(route));
    storeResult(callsign, route);
    Serial.printf("route: %s -> %s\n", callsign,
                  route[0] != '\0' ? route : "(none)");
  } else if (code == HTTP_CODE_NOT_FOUND || code == HTTP_CODE_BAD_REQUEST) {
    // adsbdb knows nothing about this callsign — cache that so we stop asking.
    storeResult(callsign, "");
    Serial.printf("route: HTTP %d for %s\n", code, callsign);
  } else {
    // Transient: drop the pending marker so a later poll retries.
    if (s_cache_mutex != nullptr &&
        xSemaphoreTake(s_cache_mutex, portMAX_DELAY) == pdTRUE) {
      CacheEntry* entry = findEntryLocked(callsign);
      if (entry != nullptr && entry->state == Slot::kPending) {
        entry->state = Slot::kFree;
      }
      xSemaphoreGive(s_cache_mutex);
    }
    Serial.printf("route: HTTP %d error for %s\n", code, callsign);
  }
  http.end();
}

void fetchTask(void*) {
  char callsign[kCallsignLen];

  for (;;) {
    if (xQueueReceive(s_queue, callsign, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    while (WiFi.status() != WL_CONNECTED) {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    // One TLS session at a time. Held across the whole request, so an ADS-B
    // poll starting mid-lookup blocks rather than opening a second context.
    {
      net::acquireSession();
      const net::SessionLease lease;
      fetchOne(callsign);
    }
    // Spacing is deliberately outside the lock: the ADS-B poll gets first
    // claim on the radio during the gap rather than queueing behind us.
    vTaskDelay(pdMS_TO_TICKS(kMinRequestSpacingMs));
  }
}

/** adsb.fi pads callsigns; the API wants them bare. */
void trimCallsign(const char* in, char* out) {
  size_t n = strnlen(in, kCallsignLen - 1);
  while (n > 0 && (in[n - 1] == ' ' || in[n - 1] == '\t')) {
    --n;
  }
  memcpy(out, in, n);
  out[n] = '\0';
}

}  // namespace

void init() {
  if (s_task != nullptr) {
    return;
  }
  for (size_t i = 0; i < kCacheSize; ++i) {
    s_cache[i].state = Slot::kFree;
  }
  s_cache_mutex = xSemaphoreCreateMutex();
  s_queue = xQueueCreate(kQueueDepth, kCallsignLen);
  if (s_cache_mutex == nullptr || s_queue == nullptr) {
    Serial.println("route: queue/mutex alloc failed");
    return;
  }
  if (xTaskCreate(fetchTask, "route", kTaskStackBytes, nullptr, 1, &s_task) !=
      pdPASS) {
    s_task = nullptr;
    Serial.println("route: task create failed");
  }
}

bool getRoute(const char* callsign, char* out_route, size_t out_len) {
  if (out_len == 0) {
    return false;
  }
  out_route[0] = '\0';
  if (callsign == nullptr || callsign[0] == '\0' || !ui::radar::showRoutes() ||
      s_cache_mutex == nullptr) {
    return false;
  }

  char trimmed[kCallsignLen];
  trimCallsign(callsign, trimmed);
  if (trimmed[0] == '\0') {
    return false;
  }

  if (xSemaphoreTake(s_cache_mutex, kCacheWaitTicks) != pdTRUE) {
    return false;
  }

  CacheEntry* entry = findEntryLocked(trimmed);
  if (entry != nullptr) {
    const bool resolved = entry->state == Slot::kResolved;
    if (resolved) {
      entry->last_used = millis();
      strncpy(out_route, entry->route, out_len - 1);
      out_route[out_len - 1] = '\0';
    }
    xSemaphoreGive(s_cache_mutex);
    return resolved;
  }

  // Reserve the slot first so repeat polls don't re-queue the same callsign,
  // and hand it back if the queue is full.
  CacheEntry* slot = claimSlotLocked();
  if (slot != nullptr) {
    strncpy(slot->callsign, trimmed, kCallsignLen - 1);
    slot->callsign[kCallsignLen - 1] = '\0';
    slot->route[0] = '\0';
    slot->last_used = millis();
    slot->state = Slot::kPending;
    if (xQueueSend(s_queue, trimmed, 0) != pdTRUE) {
      slot->state = Slot::kFree;
    }
  }
  xSemaphoreGive(s_cache_mutex);
  return false;
}

}  // namespace services::route
