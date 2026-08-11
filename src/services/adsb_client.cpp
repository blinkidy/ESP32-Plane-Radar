#include "services/adsb_client.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <cstring>

#include "config.h"

namespace services::adsb {

namespace {

constexpr char kApiBase[] = "https://opendata.adsb.fi/api/v3/lat/";
constexpr char kRouteApiBase[] = "https://api.adsbdb.com/v0/callsign/";
constexpr float kKmPerNm = 1.852f;
constexpr int kConnectAttemptMs = 200;
constexpr unsigned long kRequestTimeoutMs = 10000;

Aircraft s_aircraft[kMaxAircraft];
size_t s_aircraft_count = 0;
PollFn s_poll_fn = nullptr;

constexpr size_t kRouteCacheSize = 48;
constexpr unsigned long kRouteCacheTtlMs = 5UL * 60UL * 1000UL;
struct RouteCacheEntry {
  char callsign[9];
  char route[12];
  unsigned long updated_ms;
  bool used;
};
RouteCacheEntry s_route_cache[kRouteCacheSize];
size_t s_route_replace_index = 0;

void pollNetwork() {
  if (s_poll_fn != nullptr) {
    s_poll_fn();
  }
}

int performGetWithPoll(HTTPClient& http) {
  http.setConnectTimeout(kConnectAttemptMs);
  const unsigned long deadline = millis() + kRequestTimeoutMs;
  while (millis() < deadline) {
    pollNetwork();
    const int code = http.GET();
    if (code > 0) {
      return code;
    }
    if (code != HTTPC_ERROR_CONNECTION_REFUSED &&
        code != HTTPC_ERROR_NOT_CONNECTED) {
      return code;
    }
    delay(5);
  }
  return HTTPC_ERROR_READ_TIMEOUT;
}

bool readResponseBodyWithPoll(HTTPClient& http, String& payload) {
  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr) {
    return false;
  }

  const int content_length = http.getSize();
  if (content_length > 0) {
    payload.reserve(static_cast<unsigned>(content_length + 1));
  }

  uint8_t buffer[512];
  const unsigned long deadline = millis() + kRequestTimeoutMs;
  while (millis() < deadline) {
    pollNetwork();
    const int available = stream->available();
    if (available > 0) {
      const int to_read =
          available > static_cast<int>(sizeof(buffer)) ? static_cast<int>(sizeof(buffer))
                                                       : available;
      const int read_bytes = stream->readBytes(buffer, to_read);
      if (read_bytes > 0) {
        payload.concat(reinterpret_cast<const char*>(buffer),
                       static_cast<unsigned>(read_bytes));
      }
    }
    if (content_length > 0 &&
        static_cast<int>(payload.length()) >= content_length) {
      break;
    }
    if (!http.connected() && stream->available() <= 0) {
      break;
    }
    delay(1);
  }

  return payload.length() > 0;
}

float kmToNauticalMiles(float km) { return km / kKmPerNm; }

bool readJsonFloat(const JsonObject& obj, const char* key, float* out) {
  if (obj[key].is<float>() || obj[key].is<double>() || obj[key].is<int>()) {
    *out = obj[key].as<float>();
    return true;
  }
  return false;
}

float pickNoseHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickTrackHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickGroundSpeed(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "gs", &v)) {
    return v;
  }
  return 0.0f;
}

bool isOnGround(const JsonObject& plane) {
  if (!plane["alt_baro"].is<const char*>()) {
    return false;
  }
  return strcmp(plane["alt_baro"].as<const char*>(), "ground") == 0;
}

void copyJsonStringTrimmed(const JsonObject& obj, const char* key, char* out,
                           size_t out_len) {
  out[0] = '\0';
  if (out_len == 0 || !obj[key].is<const char*>()) {
    return;
  }
  const char* s = obj[key].as<const char*>();
  size_t n = strnlen(s, out_len - 1);
  while (n > 0 && s[n - 1] == ' ') {
    --n;
  }
  memcpy(out, s, n);
  out[n] = '\0';
}

RouteCacheEntry* findRouteEntry(const char* callsign) {
  for (size_t i = 0; i < kRouteCacheSize; ++i) {
    if (s_route_cache[i].used &&
        strcmp(s_route_cache[i].callsign, callsign) == 0) {
      return &s_route_cache[i];
    }
  }
  return nullptr;
}

RouteCacheEntry* findFreshRoute(const char* callsign) {
  RouteCacheEntry* entry = findRouteEntry(callsign);
  if (entry == nullptr || millis() - entry->updated_ms >= kRouteCacheTtlMs) {
    return nullptr;
  }
  return entry;
}

const char* airportCode(JsonObjectConst airport) {
  if (airport["iata_code"].is<const char*>()) {
    const char* iata = airport["iata_code"].as<const char*>();
    if (iata != nullptr && iata[0] != '\0') {
      return iata;
    }
  }
  if (airport["icao_code"].is<const char*>()) {
    const char* icao = airport["icao_code"].as<const char*>();
    if (icao != nullptr && icao[0] != '\0') {
      return icao;
    }
  }
  return nullptr;
}

void parseRoute(const String& payload, const char* requested_callsign,
                char* out, size_t out_len) {
  out[0] = '\0';
  JsonDocument doc;
  if (deserializeJson(doc, payload) != DeserializationError::Ok) {
    return;
  }
  JsonObjectConst route =
      doc["response"]["flightroute"].as<JsonObjectConst>();
  const char* response_callsign = route["callsign"].as<const char*>();
  if (response_callsign == nullptr ||
      strcmp(response_callsign, requested_callsign) != 0) {
    return;
  }
  const char* origin =
      airportCode(route["origin"].as<JsonObjectConst>());
  const char* destination =
      airportCode(route["destination"].as<JsonObjectConst>());
  if (origin != nullptr && destination != nullptr) {
    snprintf(out, out_len, "%s-%s", origin, destination);
  }
}

void storeRoute(const char* callsign, const char* route) {
  RouteCacheEntry* entry = findRouteEntry(callsign);
  if (entry == nullptr) {
    entry = &s_route_cache[s_route_replace_index];
    s_route_replace_index = (s_route_replace_index + 1) % kRouteCacheSize;
  }
  strncpy(entry->callsign, callsign, sizeof(entry->callsign) - 1);
  entry->callsign[sizeof(entry->callsign) - 1] = '\0';
  strncpy(entry->route, route, sizeof(entry->route) - 1);
  entry->route[sizeof(entry->route) - 1] = '\0';
  entry->updated_ms = millis();
  entry->used = true;
}

void formatAltitudeTag(const JsonObject& plane, char* out, size_t out_len) {
  out[0] = '\0';
  if (out_len == 0) {
    return;
  }

  if (plane["alt_baro"].is<const char*>()) {
    const char* s = plane["alt_baro"].as<const char*>();
    if (strcmp(s, "ground") == 0) {
      strncpy(out, "GND", out_len - 1);
      out[out_len - 1] = '\0';
      return;
    }
  }

  float alt = 0.0f;
  if (readJsonFloat(plane, "alt_baro", &alt) ||
      readJsonFloat(plane, "alt_geom", &alt)) {
    snprintf(out, out_len, "%d ft", static_cast<int>(lroundf(alt)));
  }
}

void formatGroundSpeedTag(const JsonObject& plane, char* out, size_t out_len) {
  out[0] = '\0';
  if (out_len == 0) {
    return;
  }
  const float gs = pickGroundSpeed(plane);
  if (gs > 0.0f) {
    snprintf(out, out_len, "%d kt", static_cast<int>(lroundf(gs)));
  }
}

void fillTagFields(Aircraft* ac, const JsonObject& plane) {
  copyJsonStringTrimmed(plane, "flight", ac->callsign, sizeof(ac->callsign));
  if (ac->callsign[0] == '\0') {
    copyJsonStringTrimmed(plane, "hex", ac->callsign, sizeof(ac->callsign));
  }

  copyJsonStringTrimmed(plane, "t", ac->type, sizeof(ac->type));
  ac->route[0] = '\0';
  RouteCacheEntry* cached = findFreshRoute(ac->callsign);
  if (cached != nullptr) {
    strncpy(ac->route, cached->route, sizeof(ac->route) - 1);
    ac->route[sizeof(ac->route) - 1] = '\0';
  }
  formatAltitudeTag(plane, ac->alt, sizeof(ac->alt));
  formatGroundSpeedTag(plane, ac->speed, sizeof(ac->speed));
}

}  // namespace

void setPollFn(PollFn fn) { s_poll_fn = fn; }

size_t aircraftCount() { return s_aircraft_count; }

const Aircraft* aircraftList() { return s_aircraft; }

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km) {
  const float dist_nm = kmToNauticalMiles(fetch_radius_km);

  String url = kApiBase;
  url += String(center_lat, 6);
  url += "/lon/";
  url += String(center_lon, 6);
  url += "/dist/";
  url += String(dist_nm, 1);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("adsb: http.begin failed");
    return false;
  }

  http.setTimeout(kRequestTimeoutMs);
  const int code = performGetWithPoll(http);
  if (code != HTTP_CODE_OK) {
    Serial.printf("adsb: HTTP %d\n", code);
    http.end();
    return false;
  }

  String payload;
  if (!readResponseBodyWithPoll(http, payload)) {
    Serial.println("adsb: empty response");
    http.end();
    return false;
  }
  http.end();

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("adsb: JSON parse error: %s\n", err.c_str());
    return false;
  }

  JsonArray ac = doc["ac"].as<JsonArray>();
  if (ac.isNull()) {
    s_aircraft_count = 0;
    return true;
  }

  size_t n = 0;
  for (JsonObject plane : ac) {
    if (n >= kMaxAircraft) {
      break;
    }
    if (!plane["lat"].is<float>() || !plane["lon"].is<float>()) {
      continue;
    }
    if (isOnGround(plane) && !config::kAdsbShowGroundAircraft) {
      continue;
    }

    s_aircraft[n].lat = plane["lat"].as<float>();
    s_aircraft[n].lon = plane["lon"].as<float>();
    s_aircraft[n].nose_deg = pickNoseHeading(plane);
    s_aircraft[n].track_deg = pickTrackHeading(plane);
    s_aircraft[n].gs_knots = pickGroundSpeed(plane);
    fillTagFields(&s_aircraft[n], plane);
    ++n;
  }

  s_aircraft_count = n;
  Serial.printf("adsb: %u aircraft\n", static_cast<unsigned>(n));
  return true;
}

bool fetchPendingRoute() {
  Aircraft* aircraft = nullptr;
  for (size_t i = 0; i < s_aircraft_count; ++i) {
    if (s_aircraft[i].callsign[0] != '\0' &&
        findFreshRoute(s_aircraft[i].callsign) == nullptr) {
      aircraft = &s_aircraft[i];
      break;
    }
  }
  if (aircraft == nullptr) {
    return false;
  }

  String url = kRouteApiBase;
  url += aircraft->callsign;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  char route[sizeof(aircraft->route)];
  route[0] = '\0';
  if (http.begin(client, url)) {
    http.setTimeout(4000);
    if (http.GET() == HTTP_CODE_OK) {
      parseRoute(http.getString(), aircraft->callsign, route, sizeof(route));
    }
    http.end();
  }
  storeRoute(aircraft->callsign, route);
  strncpy(aircraft->route, route, sizeof(aircraft->route) - 1);
  aircraft->route[sizeof(aircraft->route) - 1] = '\0';
  Serial.printf("route: %s -> %s\n", aircraft->callsign,
                route[0] != '\0' ? route : "(unavailable)");
  return route[0] != '\0';
}

}  // namespace services::adsb
