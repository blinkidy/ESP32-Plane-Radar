#include "ui/radar_projection.h"

#include <cmath>

#include "services/radar_location.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"

namespace ui::radar {
namespace {

constexpr float kKmPerDegLat = 111.0f;
constexpr float kDegToRad = 0.01745329252f;

float s_center_lat = 0.0f;
float s_center_lon = 0.0f;
float s_km_per_deg_lon = kKmPerDegLat;
bool s_ready = false;

/** Shortest signed east-west separation, so the ±180° seam doesn't wrap. */
float wrapLonDelta(float delta_deg) {
  if (delta_deg > 180.0f) {
    return delta_deg - 360.0f;
  }
  if (delta_deg < -180.0f) {
    return delta_deg + 360.0f;
  }
  return delta_deg;
}

}  // namespace

void projectionSync() {
  const float lat = static_cast<float>(services::location::lat());
  const float lon = static_cast<float>(services::location::lon());
  if (s_ready && lat == s_center_lat && lon == s_center_lon) {
    return;
  }
  s_center_lat = lat;
  s_center_lon = lon;
  // Guard the poles: cos() collapses to 0 and the scale would blow up.
  const float cos_lat = cosf(lat * kDegToRad);
  s_km_per_deg_lon = kKmPerDegLat * (cos_lat < 0.02f ? 0.02f : cos_lat);
  s_ready = true;
}

void offsetKmFromCenter(float lat, float lon, float* dx_km, float* dy_km,
                        float* dist_km) {
  projectionSync();
  *dx_km = wrapLonDelta(lon - s_center_lon) * s_km_per_deg_lon;
  *dy_km = (lat - s_center_lat) * kKmPerDegLat;
  *dist_km = sqrtf((*dx_km) * (*dx_km) + (*dy_km) * (*dy_km));
}

float distSqKmFromCenter(float lat, float lon) {
  projectionSync();
  const float dx = wrapLonDelta(lon - s_center_lon) * s_km_per_deg_lon;
  const float dy = (lat - s_center_lat) * kKmPerDegLat;
  return dx * dx + dy * dy;
}

float pixelsPerKm() {
  return static_cast<float>(kGridOuterRadius) / rangeCurrent().outer_km;
}

void latLonToScreen(float lat, float lon, int* out_x, int* out_y) {
  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);

  const float px_per_km = pixelsPerKm();
  *out_x = kCenterX + static_cast<int>(lroundf(dx_km * px_per_km));
  *out_y = kCenterY - static_cast<int>(lroundf(dy_km * px_per_km));
}

}  // namespace ui::radar
