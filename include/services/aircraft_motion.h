/*
 * Dead-reckoning between ADS-B polls. Adapted from upstream PR #73
 * (RockBase-Ronnie).
 *
 * adsb.fi is polled every ~3 s but the radar now redraws at ~10 fps, so
 * positions are extrapolated along each aircraft's track and cross-faded into
 * the next real fix. Without the fade, every poll would visibly snap.
 */
#pragma once

#include <cmath>

#include "services/adsb_client.h"

namespace services::adsb::motion {

/** Cross-fade from the last displayed position into the new fix. */
constexpr unsigned long kBlendDurationMs = 1200UL;
/** Stop extrapolating a stale track rather than flying it off the scope. */
constexpr unsigned long kMaxPredictionMs = 10000UL;
constexpr float kKnotsToMetersPerSecond = 0.514444f;
constexpr float kMetersPerDegreeLatitude = 111320.0f;
constexpr float kDegToRad = 0.01745329252f;

struct Position {
  float lat;
  float lon;
};

struct AircraftTrack {
  Aircraft aircraft;
  Position blend_from;
  unsigned long updated_ms;
};

/** Unsigned wrap-safe: millis() rollover yields the correct elapsed time. */
constexpr unsigned long elapsedMs(unsigned long now_ms, unsigned long then_ms) {
  return now_ms - then_ms;
}

constexpr unsigned long predictionElapsedMs(unsigned long elapsed_ms) {
  return elapsed_ms < kMaxPredictionMs ? elapsed_ms : kMaxPredictionMs;
}

constexpr float clamp01(float value) {
  return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

/**
 * Ease in/out so a new fix arrives without a visible velocity step.
 *
 * Split across two single-expression functions rather than naming `t` in a
 * local: the firmware compiles as gnu++11 (see platformio.ini), where a
 * constexpr body must be exactly one return statement.
 */
constexpr float smoothstepUnit(float t) { return t * t * (3.0f - 2.0f * t); }

constexpr float smoothstep(float value) {
  return smoothstepUnit(clamp01(value));
}

constexpr bool hasStableId(const Aircraft& aircraft) {
  return aircraft.id[0] != '\0';
}

inline Position predictPosition(const Aircraft& aircraft,
                                unsigned long elapsed_ms) {
  if (aircraft.gs_knots <= 0.0f) {
    return {aircraft.lat, aircraft.lon};
  }

  const float distance_m =
      aircraft.gs_knots * kKnotsToMetersPerSecond *
      (static_cast<float>(predictionElapsedMs(elapsed_ms)) / 1000.0f);
  const float track_rad = aircraft.track_deg * kDegToRad;
  const float north_m = distance_m * cosf(track_rad);
  const float east_m = distance_m * sinf(track_rad);
  const float lon_scale =
      kMetersPerDegreeLatitude * cosf(aircraft.lat * kDegToRad);

  return {
      aircraft.lat + north_m / kMetersPerDegreeLatitude,
      fabsf(lon_scale) < 1.0f ? aircraft.lon : aircraft.lon + east_m / lon_scale,
  };
}

inline Position displayPosition(const AircraftTrack& track,
                                unsigned long now_ms) {
  const unsigned long elapsed = elapsedMs(now_ms, track.updated_ms);
  const Position target = predictPosition(track.aircraft, elapsed);
  const float alpha = smoothstep(static_cast<float>(elapsed) /
                                 static_cast<float>(kBlendDurationMs));
  return {
      track.blend_from.lat + alpha * (target.lat - track.blend_from.lat),
      track.blend_from.lon + alpha * (target.lon - track.blend_from.lon),
  };
}

inline AircraftTrack makeInitialTrack(const Aircraft& aircraft,
                                      unsigned long now_ms) {
  return {aircraft, {aircraft.lat, aircraft.lon}, now_ms};
}

/** Carry the currently displayed position forward so the fade starts smooth. */
inline AircraftTrack updateTrack(const AircraftTrack& previous,
                                 const Aircraft& aircraft,
                                 unsigned long now_ms) {
  return {aircraft, displayPosition(previous, now_ms), now_ms};
}

}  // namespace services::adsb::motion
