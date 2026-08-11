#include "ui/runway_overlay.h"

#include <cmath>
#include <cstdlib>

#include "data/large_airports.h"
#include "services/radar_location.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"

namespace ui::runway {
namespace {

constexpr float kKmPerDeg = 111.0f;
bool s_in_range[data::large_airports::kAirportCount];

struct ScreenRunway {
  int16_t x0;
  int16_t y0;
  int16_t x1;
  int16_t y1;
};
constexpr size_t kMaxVisibleRunways = 128;
ScreenRunway s_visible_runways[kMaxVisibleRunways];
size_t s_visible_runway_count = 0;

float e7ToDeg(int32_t e7) { return static_cast<float>(e7) * 1e-7f; }

void offsetKmFromCenter(float lat, float lon, float* dx_km, float* dy_km,
                        float* dist_km) {
  *dx_km =
      static_cast<float>(lon - services::location::lon()) * kKmPerDeg;
  *dy_km =
      static_cast<float>(lat - services::location::lat()) * kKmPerDeg;
  *dist_km = sqrtf((*dx_km) * (*dx_km) + (*dy_km) * (*dy_km));
}

void latLonToScreen(float lat, float lon, int* out_x, int* out_y) {
  const float outer_km = radar::rangeCurrent().outer_km;
  const float px_per_km =
      static_cast<float>(radar::kGridOuterRadius) / outer_km;

  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);

  *out_x = radar::kCenterX + static_cast<int>(lroundf(dx_km * px_per_km));
  *out_y = radar::kCenterY - static_cast<int>(lroundf(dy_km * px_per_km));
}

int distSqFromCenter(int x, int y) {
  const int dx = x - radar::kCenterX;
  const int dy = y - radar::kCenterY;
  return dx * dx + dy * dy;
}

void clipPointToOuterRing(int x0, int y0, int* x1, int* y1) {
  const int max_r = radar::kGridOuterRadius;
  const int max_r_sq = max_r * max_r;
  if (distSqFromCenter(*x1, *y1) <= max_r_sq) {
    return;
  }

  const int dx = *x1 - x0;
  const int dy = *y1 - y0;
  float t = 1.0f;
  for (int step = 0; step < 20; ++step) {
    const int px = x0 + static_cast<int>(lroundf(dx * t));
    const int py = y0 + static_cast<int>(lroundf(dy * t));
    if (distSqFromCenter(px, py) <= max_r_sq) {
      *x1 = px;
      *y1 = py;
      return;
    }
    t -= 0.05f;
    if (t <= 0.0f) {
      *x1 = x0;
      *y1 = y0;
      return;
    }
  }
}

bool segmentIntersectsDisc(int x0, int y0, int x1, int y1) {
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int r = radar::kGridOuterRadius;
  const int r_sq = r * r;

  if (distSqFromCenter(x0, y0) <= r_sq || distSqFromCenter(x1, y1) <= r_sq) {
    return true;
  }

  const int dx = x1 - x0;
  const int dy = y1 - y0;
  const int fx = x0 - cx;
  const int fy = y0 - cy;
  const int a = dx * dx + dy * dy;
  if (a == 0) {
    return false;
  }
  const int b = 2 * (fx * dx + fy * dy);
  const int c = fx * fx + fy * fy - r_sq;
  int disc = b * b - 4 * a * c;
  if (disc < 0) {
    return false;
  }
  disc = static_cast<int>(sqrtf(static_cast<float>(disc)));
  const float inv2a = 1.0f / (2.0f * static_cast<float>(a));
  const float t0 = (-static_cast<float>(b) - disc) * inv2a;
  const float t1 = (-static_cast<float>(b) + disc) * inv2a;
  return (t0 >= 0.0f && t0 <= 1.0f) || (t1 >= 0.0f && t1 <= 1.0f);
}

bool drawRunwayLine(lgfx::LGFXBase& gfx, const data::large_airports::Runway& rw) {
  const float le_lat = e7ToDeg(rw.le_lat_e7);
  const float le_lon = e7ToDeg(rw.le_lon_e7);
  const float he_lat = e7ToDeg(rw.he_lat_e7);
  const float he_lon = e7ToDeg(rw.he_lon_e7);

  int x0 = 0;
  int y0 = 0;
  int x1 = 0;
  int y1 = 0;
  latLonToScreen(le_lat, le_lon, &x0, &y0);
  latLonToScreen(he_lat, he_lon, &x1, &y1);

  if (!segmentIntersectsDisc(x0, y0, x1, y1)) {
    return false;
  }

  clipPointToOuterRing(x0, y0, &x1, &y1);
  clipPointToOuterRing(x1, y1, &x0, &y0);

  gfx.drawWideLine(x0, y0, x1, y1, radar::kRunwayLineHalfWidth,
                   radar::kColorRunway);
  if (s_visible_runway_count < kMaxVisibleRunways) {
    s_visible_runways[s_visible_runway_count++] = {
        static_cast<int16_t>(x0), static_cast<int16_t>(y0),
        static_cast<int16_t>(x1), static_cast<int16_t>(y1)};
  }
  return true;
}

int orientation(int ax, int ay, int bx, int by, int cx, int cy) {
  const int64_t cross = static_cast<int64_t>(bx - ax) * (cy - ay) -
                        static_cast<int64_t>(by - ay) * (cx - ax);
  return cross < 0 ? -1 : (cross > 0 ? 1 : 0);
}

bool segmentsCross(int ax, int ay, int bx, int by, int cx, int cy, int dx,
                   int dy) {
  return orientation(ax, ay, bx, by, cx, cy) !=
             orientation(ax, ay, bx, by, dx, dy) &&
         orientation(cx, cy, dx, dy, ax, ay) !=
             orientation(cx, cy, dx, dy, bx, by);
}

bool runwayCrossesRectangle(const ScreenRunway& runway, int left, int top,
                            int right, int bottom) {
  if ((runway.x0 >= left && runway.x0 <= right && runway.y0 >= top &&
       runway.y0 <= bottom) ||
      (runway.x1 >= left && runway.x1 <= right && runway.y1 >= top &&
       runway.y1 <= bottom)) {
    return true;
  }
  return segmentsCross(runway.x0, runway.y0, runway.x1, runway.y1, left, top,
                       right, top) ||
         segmentsCross(runway.x0, runway.y0, runway.x1, runway.y1, right, top,
                       right, bottom) ||
         segmentsCross(runway.x0, runway.y0, runway.x1, runway.y1, right,
                       bottom, left, bottom) ||
         segmentsCross(runway.x0, runway.y0, runway.x1, runway.y1, left,
                       bottom, left, top);
}

}  // namespace

void drawLargeAirportRunways(lgfx::LGFXBase& gfx) {
  s_visible_runway_count = 0;
  if (!radar::showRunways()) {
    return;
  }
  const float radius_km = radar::fetchRadiusKm();

  for (size_t i = 0; i < data::large_airports::kAirportCount; ++i) {
    s_in_range[i] = false;
  }

  for (size_t i = 0; i < data::large_airports::kRunwayCount; ++i) {
    const auto& rw = data::large_airports::kRunways[i];
    const uint16_t ap_idx = rw.airport_idx;
    if (!s_in_range[ap_idx]) {
      const auto& ap = data::large_airports::kAirports[ap_idx];
      float dx_km = 0.0f;
      float dy_km = 0.0f;
      float dist_km = 0.0f;
      offsetKmFromCenter(e7ToDeg(ap.lat_e7), e7ToDeg(ap.lon_e7), &dx_km, &dy_km,
                         &dist_km);
      s_in_range[ap_idx] = (dist_km <= radius_km);
    }
    if (!s_in_range[ap_idx]) {
      continue;
    }
    drawRunwayLine(gfx, rw);
  }
}

int tagRectanglePenalty(int left, int top, int right, int bottom) {
  int penalty = 0;
  for (size_t i = 0; i < s_visible_runway_count; ++i) {
    if (runwayCrossesRectangle(s_visible_runways[i], left, top, right,
                               bottom)) {
      ++penalty;
    }
  }
  return penalty;
}

}  // namespace ui::runway
