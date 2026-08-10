/*
 * Pure parsing half of the adsbdb route lookup.
 *
 * Split out from route_fetcher.cpp so it depends on nothing but ArduinoJson and
 * can be exercised off-device (see test/test_route_parse.cpp).
 */
#pragma once

#include <ArduinoJson.h>

#include <cstdio>

namespace services::route {

/** Prefer the IATA code; ICAO is the fallback for fields that lack one. */
inline const char* airportCode(JsonObjectConst obj) {
  if (obj.isNull()) {
    return nullptr;
  }
  for (const char* key : {"iata_code", "icao_code"}) {
    if (obj[key].is<const char*>()) {
      const char* value = obj[key].as<const char*>();
      if (value != nullptr && value[0] != '\0') {
        return value;
      }
    }
  }
  return nullptr;
}

/**
 * Render an adsbdb reply as "ORIG-DEST".
 *
 * Writes an empty string when the callsign has no usable route — including
 * malformed JSON, an error reply, or a route missing either endpoint — which
 * the caller caches as a negative result.
 */
inline void parseRouteJson(const char* payload, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';

  JsonDocument doc;
  if (deserializeJson(doc, payload) != DeserializationError::Ok) {
    return;
  }

  JsonObjectConst flightroute =
      doc["response"]["flightroute"].as<JsonObjectConst>();
  const char* origin = airportCode(flightroute["origin"].as<JsonObjectConst>());
  const char* destination =
      airportCode(flightroute["destination"].as<JsonObjectConst>());
  if (origin == nullptr || destination == nullptr) {
    return;
  }
  snprintf(out, out_len, "%s-%s", origin, destination);
}

}  // namespace services::route
