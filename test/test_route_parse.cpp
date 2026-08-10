/*
 * Host tests for the pure logic headers. Not part of the firmware build.
 *
 *   g++ -std=gnu++11 -I include -I <ArduinoJson>/src \
 *       -o /tmp/t test/test_route_parse.cpp && /tmp/t
 *
 * Build as gnu++11, not gnu++17: platformio.ini asks for gnu++17 but the
 * Arduino ESP32 framework's own -std=gnu++11 wins on the command line, so that
 * is what the firmware really compiles as. Testing at gnu++17 hid a constexpr
 * error that only the device build caught.
 */

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "services/aircraft_motion.h"
#include "services/route_parse.h"
#include "ui/radar_animation_policy.h"
#include "ui/radar_tag_layout_policy.h"

namespace {

int g_checks = 0;

void expectRoute(const char* label, const char* payload, const char* want) {
  char got[12];
  services::route::parseRouteJson(payload, got, sizeof(got));
  if (strcmp(got, want) != 0) {
    printf("FAIL %s: got \"%s\", want \"%s\"\n", label, got, want);
    abort();
  }
  ++g_checks;
}

void testRouteParsing() {
  // Shape of an api.adsbdb.com/v0/callsign/<cs> reply.
  expectRoute("iata",
              R"({"response":{"flightroute":{
                "callsign":"UAL123",
                "origin":{"iata_code":"PHX","icao_code":"KPHX",
                          "municipality":"Phoenix","name":"Sky Harbor"},
                "destination":{"iata_code":"DEN","icao_code":"KDEN",
                               "municipality":"Denver","name":"Denver Intl"}}}})",
              "PHX-DEN");

  // No IATA code (common for military/regional fields): fall back to ICAO.
  expectRoute("icao fallback",
              R"({"response":{"flightroute":{
                "origin":{"iata_code":"","icao_code":"KIWA"},
                "destination":{"icao_code":"KABQ"}}}})",
              "KIWA-KABQ");

  // adsbdb's "unknown callsign" reply.
  expectRoute("unknown callsign",
              R"({"response":"unknown callsign"})", "");

  // Route present but one endpoint missing — treat as no route.
  expectRoute("half a route",
              R"({"response":{"flightroute":{"origin":{"iata_code":"PHX"}}}})",
              "");

  expectRoute("malformed json", "{not json at all", "");
  expectRoute("empty body", "", "");
  expectRoute("null flightroute",
              R"({"response":{"flightroute":null}})", "");

  // Longest realistic output (ICAO-ICAO = 9 chars) must survive the buffer.
  char buf[12];
  services::route::parseRouteJson(
      R"({"response":{"flightroute":{"origin":{"icao_code":"KIWA"},
          "destination":{"icao_code":"EGLL"}}}})",
      buf, sizeof(buf));
  assert(strcmp(buf, "KIWA-EGLL") == 0);
  assert(strlen(buf) < sizeof(buf));
  ++g_checks;
}

void testDeadReckoning() {
  using namespace services::adsb;
  using namespace services::adsb::motion;

  Aircraft a{};
  a.id[0] = 'a';
  a.lat = 33.3078f;
  a.lon = -111.655f;  // KIWA
  a.track_deg = 90.0f;
  a.gs_knots = 400.0f;

  const AircraftTrack t = makeInitialTrack(a, 1000);

  // A fresh fix displays exactly where it was reported.
  const Position at_fix = displayPosition(t, 1000);
  assert(at_fix.lat == a.lat && at_fix.lon == a.lon);
  ++g_checks;

  // 400 kt due east for 10 s = 2.058 km; 1 deg lon = 92.99 km at 33.3 N.
  const Position later = displayPosition(t, 1000 + 10000);
  const float dlon = later.lon - a.lon;
  assert(fabsf(later.lat - a.lat) < 1e-4f);
  assert(fabsf(dlon - 0.02213f) < 5e-4f);
  ++g_checks;

  // Extrapolation is clamped, so a track that stops updating stops moving.
  assert(displayPosition(t, 1000 + 60000).lon ==
         displayPosition(t, 1000 + 600000).lon);
  ++g_checks;

  // A stationary target never drifts.
  Aircraft parked = a;
  parked.gs_knots = 0.0f;
  const AircraftTrack pt = makeInitialTrack(parked, 0);
  assert(displayPosition(pt, 30000).lat == parked.lat);
  assert(displayPosition(pt, 30000).lon == parked.lon);
  ++g_checks;

  // A new fix blends from where the aircraft was last drawn, not from the raw
  // previous fix, so the symbol never jumps backwards.
  Aircraft moved = a;
  moved.lat = 33.31f;
  const AircraftTrack updated = updateTrack(t, moved, 3000);
  const Position start = displayPosition(updated, 3000);
  const Position was = displayPosition(t, 3000);
  assert(fabsf(start.lat - was.lat) < 1e-6f);
  assert(fabsf(start.lon - was.lon) < 1e-6f);
  ++g_checks;

  // ...and converges on the new fix once the blend completes.
  const Position settled = displayPosition(updated, 3000 + kBlendDurationMs);
  assert(fabsf(settled.lat - moved.lat) < 1e-4f);
  ++g_checks;

  // millis() rollover: unsigned wraparound still gives a sane elapsed time.
  // unsigned long is 32-bit on the ESP32 and 64-bit here, so compare low bits.
  assert(static_cast<unsigned>(elapsedMs(0x10UL, 0xFFFFF000UL)) == 0x1010u);
  ++g_checks;
}

void testPolicies() {
  using namespace ui::radar;

  static_assert(sweepPhaseMs(kSweepPeriodMs + 5) == 5);
  static_assert(sweepPhaseMs(kSweepPeriodMs) == 0);
  // Nothing to animate with the sweep off and an empty scope.
  static_assert(!animationNeeded(false, 0));
  static_assert(animationNeeded(false, 3));
  static_assert(animationNeeded(true, 0));

  static_assert(tagRectsOverlap({0, 0, 10, 10}, {5, 5, 15, 15}));
  static_assert(!tagRectsOverlap({0, 0, 10, 10}, {10, 0, 20, 10}));
  static_assert(!tagRectsOverlap({0, 0, 10, 10}, {0, 10, 10, 20}));
  static_assert(tagRectInside({0, 0, 240, 240}, 240, 240));
  static_assert(!tagRectInside({-1, 0, 10, 10}, 240, 240));
  static_assert(!tagRectInside({0, 0, 241, 10}, 240, 240));
  static_assert(!tagRectInside({10, 0, 10, 10}, 240, 240));  // zero width

  // Round panel: the bounding square is not the visible area. A tag out along
  // a diagonal can pass tagRectInside and still run off the bezel.
  constexpr int kCx = 120;
  constexpr int kCy = 120;
  constexpr int kR = 118;
  static_assert(tagRectInsideDisc({100, 100, 140, 140}, kCx, kCy, kR));
  // A 2-line tag placed outward from traffic on the 040 bearing at max range:
  // wholly inside the square, but its far corner is r=133, off the glass.
  static_assert(tagRectInside({193, 33, 221, 63}, 240, 240));
  static_assert(!tagRectInsideDisc({193, 33, 221, 63}, kCx, kCy, kR));
  // Hugging the rim near an axis is fine — that is where N/S/E/W live.
  static_assert(tagRectInsideDisc({105, 6, 135, 36}, kCx, kCy, kR));
  // Exclusive right/bottom: a rect ending exactly on the rim still fits.
  static_assert(tagRectInsideDisc({kCx, kCy, kCx + kR + 1, kCy + 1}, kCx, kCy, kR));
  static_assert(!tagRectInsideDisc({kCx, kCy, kCx + kR + 2, kCy + 1}, kCx, kCy, kR));

  using services::adsb::motion::clamp01;
  using services::adsb::motion::smoothstep;
  static_assert(smoothstep(0.0f) == 0.0f);
  static_assert(smoothstep(1.0f) == 1.0f);
  static_assert(clamp01(2.0f) == 1.0f);
  static_assert(clamp01(-1.0f) == 0.0f);
  g_checks += 22;
}

}  // namespace

int main() {
  testRouteParsing();
  testDeadReckoning();
  testPolicies();
  printf("all good — %d checks\n", g_checks);
  return 0;
}
