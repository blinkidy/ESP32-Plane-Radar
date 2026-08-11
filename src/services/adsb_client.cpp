#include "services/adsb_client.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <cstring>

#include "config.h"
#include "services/aircraft_motion.h"
#include "services/net_session.h"
#include "services/route_fetcher.h"

namespace services::adsb {

namespace {

constexpr char kApiBase[] = "https://opendata.adsb.fi/api/v3/lat/";
constexpr float kKmPerNm = 1.852f;
constexpr int kConnectAttemptMs = 200;
constexpr unsigned long kRequestTimeoutMs = 10000;

using motion::AircraftTrack;

/** Committed tracks (position + last fix), read by the renderer. */
AircraftTrack s_tracks[kMaxAircraft];
/** Parse scratch: only copied into s_tracks once a fetch fully succeeds. */
Aircraft s_incoming[kMaxAircraft];
/** Rebuilt per aircraftList() call with dead-reckoned positions. */
Aircraft s_display[kMaxAircraft];
size_t s_aircraft_count = 0;
PollFn s_poll_fn = nullptr;

int findTrackById(const char* id) {
  if (id == nullptr || id[0] == '\0') {
    return -1;
  }
  for (size_t i = 0; i < s_aircraft_count; ++i) {
    if (strcmp(s_tracks[i].aircraft.id, id) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

/**
 * Swap in a new set of fixes.
 *
 * Runs on the main task between poll callbacks, so the renderer — which is
 * driven from those same callbacks — never observes a half-updated list.
 *
 * Resolved in two passes: an aircraft's new blend origin is read from the old
 * track table, and incoming order need not match it, so writing in place during
 * the first pass would let an earlier write clobber a later read.
 */
motion::Position s_blend_from[kMaxAircraft];  // pass-one scratch

void replaceTracks(const Aircraft* incoming, size_t count,
                   unsigned long now_ms) {
  for (size_t i = 0; i < count; ++i) {
    const int previous = motion::hasStableId(incoming[i])
                             ? findTrackById(incoming[i].id)
                             : -1;
    s_blend_from[i] =
        previous >= 0
            ? motion::displayPosition(s_tracks[previous], now_ms)
            : motion::Position{incoming[i].lat, incoming[i].lon};
  }
  for (size_t i = 0; i < count; ++i) {
    s_tracks[i].aircraft = incoming[i];
    s_tracks[i].blend_from = s_blend_from[i];
    s_tracks[i].updated_ms = now_ms;
  }
  s_aircraft_count = count;
}

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

/**
 * Ground speed only — never TAS/IAS.
 *
 * Airspeed can differ from ground speed by 100+ kt in a jet stream, and this
 * value now does three jobs that all mean "over the ground": the speed vector,
 * the "kt" tag, and dead-reckoned motion between polls. An aircraft reporting
 * no gs is left at 0, which reads as "unknown" everywhere rather than as a
 * confidently wrong number.
 */
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

/** Ground speed in knots — the unit every other aviation readout uses. */
void formatSpeedTag(const JsonObject& plane, char* out, size_t out_len) {
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
  copyJsonStringTrimmed(plane, "hex", ac->id, sizeof(ac->id));
  copyJsonStringTrimmed(plane, "flight", ac->callsign, sizeof(ac->callsign));
  if (ac->callsign[0] == '\0') {
    strncpy(ac->callsign, ac->id, sizeof(ac->callsign) - 1);
    ac->callsign[sizeof(ac->callsign) - 1] = '\0';
  }

  copyJsonStringTrimmed(plane, "t", ac->type, sizeof(ac->type));
  formatAltitudeTag(plane, ac->alt, sizeof(ac->alt));
  formatSpeedTag(plane, ac->speed, sizeof(ac->speed));
  // Misses on the first sighting and fills in on a later poll.
  services::route::getRoute(ac->callsign, ac->route, sizeof(ac->route));
}

}  // namespace

void setPollFn(PollFn fn) { s_poll_fn = fn; }

size_t aircraftCount() { return s_aircraft_count; }

const Aircraft* aircraftList(unsigned long now_ms, size_t* count) {
  for (size_t i = 0; i < s_aircraft_count; ++i) {
    s_display[i] = s_tracks[i].aircraft;
    // Route lookups finish independently of ADS-B position polls. Hydrate a
    // completed lookup directly into the display copy so origin/destination
    // appears immediately, even when the next position request is delayed.
    if (s_display[i].route[0] == '\0') {
      services::route::getRoute(s_display[i].callsign, s_display[i].route,
                                sizeof(s_display[i].route));
    }
    const motion::Position position =
        motion::displayPosition(s_tracks[i], now_ms);
    s_display[i].lat = position.lat;
    s_display[i].lon = position.lon;
  }
  if (count != nullptr) {
    *count = s_aircraft_count;
  }
  return s_display;
}

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km) {
  // Exclusive for the whole call, so the route task cannot open a second TLS
  // session alongside ours — and cannot have one open when we start. Never
  // waits: this runs on the task that also drives the display.
  if (!net::trySession()) {
    return false;
  }
  const net::SessionLease lease;

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

  // Parsing and committing a busy response can take long enough to show on a
  // 10 fps scope. Give the cosmetic sweep a frame before that CPU-only work.
  pollNetwork();
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("adsb: JSON parse error: %s\n", err.c_str());
    return false;
  }

  JsonArray ac = doc["ac"].as<JsonArray>();
  if (ac.isNull()) {
    replaceTracks(nullptr, 0, millis());
    return true;
  }

  size_t n = 0;
  for (JsonObject plane : ac) {
    // Yield animation time while a newly busy scope is being populated. This
    // also keeps button and portal servicing responsive during a large reply.
    pollNetwork();
    if (n >= kMaxAircraft) {
      break;
    }
    if (!plane["lat"].is<float>() || !plane["lon"].is<float>()) {
      continue;
    }
    if (isOnGround(plane) && !config::kAdsbShowGroundAircraft) {
      continue;
    }

    s_incoming[n].lat = plane["lat"].as<float>();
    s_incoming[n].lon = plane["lon"].as<float>();
    // Match upstream: aircraft attitude follows reported heading, while the
    // motion vector and dead reckoning independently follow ground track.
    s_incoming[n].nose_deg = pickNoseHeading(plane);
    s_incoming[n].track_deg = pickTrackHeading(plane);
    s_incoming[n].gs_knots = pickGroundSpeed(plane);
    fillTagFields(&s_incoming[n], plane);
    ++n;
  }

  pollNetwork();
  replaceTracks(s_incoming, n, millis());
  Serial.printf("adsb: %u aircraft\n", static_cast<unsigned>(n));
  return true;
}

}  // namespace services::adsb
