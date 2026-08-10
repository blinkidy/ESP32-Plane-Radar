#pragma once

#include <cstddef>

namespace services::adsb {

struct Aircraft {
  /** ICAO hex address — stable across polls, used to match tracks. */
  char id[8];
  float lat;
  float lon;
  float nose_deg;
  float track_deg;
  float gs_knots;
  char callsign[9];
  char type[5];
  char alt[12];
  /** Ground speed tag, e.g. "420 kt". */
  char speed[10];
  /** Origin→destination airport codes, e.g. "PHX-DEN". */
  char route[12];
};

constexpr size_t kMaxAircraft = 64;

size_t aircraftCount();

/**
 * Aircraft with positions dead-reckoned to now_ms.
 *
 * The returned buffer is rebuilt on every call and stays valid until the next
 * one. Writes *count with the number of entries.
 */
const Aircraft* aircraftList(unsigned long now_ms, size_t* count);

/** True while fetchUpdate() has a TLS session open. */
bool fetchInFlight();

/** Hook invoked during long HTTP I/O (e.g. wifiLoop). Optional. */
using PollFn = void (*)();
void setPollFn(PollFn fn);

/** Fetch aircraft within fetch_radius_km of center_lat/lon from adsb.fi. */
bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km);

}  // namespace services::adsb
