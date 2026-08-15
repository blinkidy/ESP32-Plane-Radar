#include "ui/radar_display.h"

#include <lgfx/v1/lgfx_fonts.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/adsb_client.h"
#include "services/radar_location.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"
#include "ui/runway_overlay.h"

namespace radar_fonts = lgfx::v1::fonts;

namespace ui {
namespace radar {

uint16_t kColorBackground = 0x0000;
uint16_t kColorGrid = 0x0320;
uint16_t kColorLabel = 0xFFFF;
uint16_t kColorCenter = 0xFFFF;
uint16_t kColorAircraft = 0x001F;
uint16_t kColorMilitary = 0xFC00;
uint16_t kColorTrackVector = 0xFFFF;
uint16_t kColorTrail = 0x9D33;
uint16_t kColorTagType = 0x5DFF;
uint16_t kColorTagAltitude = 0xFFE0;
uint16_t kColorTagSpeed = 0xBDF7;
uint16_t kColorRunway = 0x4D5F;
uint16_t kColorRunwayLabel = 0x7DFF;

}  // namespace radar

namespace {

bool s_label_metrics_ready = false;
bool s_cardinal_use_vlw = false;
bool s_scale_use_vlw = false;
float s_cardinal_vlw_size = 0.56f;
float s_scale_vlw_size = 0.50f;
float s_tag_vlw_size = 0.56f;
const lgfx::GFXfont* s_cardinal_gfx = &radar_fonts::FreeSansBold12pt7b;
const lgfx::GFXfont* s_scale_gfx = &radar_fonts::FreeSansBold9pt7b;
const lgfx::GFXfont* s_tag_gfx = &radar_fonts::FreeSansBold12pt7b;

bool s_tag_label_metrics_ready = false;
bool s_tag_use_vlw = false;

int s_scale_label_max_w = 0;
int s_scale_label_h = 0;

lgfx::LovyanGFX* s_draw = &tft;
LGFX_Sprite s_frame(&tft);
bool s_frame_ready = false;
bool s_fast_tag_placement = false;

struct CachedTagChoice {
  char id[8] = {};
  uint8_t candidate = 0;
  bool valid = false;
  bool uses_edge_candidates = false;
};
CachedTagChoice s_tag_choices[services::adsb::kMaxAircraft];

CachedTagChoice* cachedTagChoice(const char* id) {
  CachedTagChoice* empty = nullptr;
  for (auto& choice : s_tag_choices) {
    if (choice.id[0] == '\0') {
      if (empty == nullptr) {
        empty = &choice;
      }
    } else if (strcmp(choice.id, id) == 0) {
      return &choice;
    }
  }
  if (empty != nullptr && id[0] != '\0') {
    strncpy(empty->id, id, sizeof(empty->id) - 1);
  }
  if (empty != nullptr || id[0] == '\0') {
    return empty;
  }
  // Reuse a deterministic slot after enough different aircraft have passed
  // through to fill the cache.
  size_t slot = 0;
  for (const char* p = id; *p != '\0'; ++p) {
    slot = (slot * 33U + static_cast<unsigned char>(*p)) %
           services::adsb::kMaxAircraft;
  }
  CachedTagChoice& replacement = s_tag_choices[slot];
  strncpy(replacement.id, id, sizeof(replacement.id) - 1);
  replacement.id[sizeof(replacement.id) - 1] = '\0';
  replacement.valid = false;
  return &replacement;
}

class DrawScope {
 public:
  explicit DrawScope(lgfx::LovyanGFX& gfx) : prev_(s_draw) { s_draw = &gfx; }
  ~DrawScope() { s_draw = prev_; }

 private:
  lgfx::LovyanGFX* prev_;
};

int absDiff(int a, int b) { return std::abs(a - b); }

int measureGfxHeight(const lgfx::GFXfont& font) {
  tft.setFont(&font);
  tft.setTextSize(1);
  return tft.fontHeight();
}

int measureVlwHeight(float size) {
  tft.setTextSize(size);
  return tft.fontHeight();
}

float findVlwSizeForHeight(int target_px) {
  float lo = 0.25f;
  float hi = 1.2f;
  for (int i = 0; i < 16; ++i) {
    const float mid = (lo + hi) * 0.5f;
    if (measureVlwHeight(mid) < target_px) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return hi;
}

void applyScaleStyle();

const lgfx::GFXfont* pickGfxFontClosest(
    int target_px, const lgfx::GFXfont* const* candidates, size_t count) {
  const lgfx::GFXfont* best = candidates[0];
  int best_diff = absDiff(measureGfxHeight(*best), target_px);

  for (size_t i = 1; i < count; ++i) {
    const int diff = absDiff(measureGfxHeight(*candidates[i]), target_px);
    if (diff < best_diff) {
      best_diff = diff;
      best = candidates[i];
    }
  }
  return best;
}

void initLabelMetrics() {
  if (s_label_metrics_ready) {
    return;
  }

  const int cardinal_target = radar::kCardinalLabelHeightPx;

  if (displayFontIsSmooth()) {
    s_cardinal_use_vlw = true;
    s_cardinal_vlw_size = findVlwSizeForHeight(cardinal_target);
    const int cardinal_h = measureVlwHeight(s_cardinal_vlw_size);
    const int scale_target = cardinal_h - radar::kScaleBelowCardinalPx;
    s_scale_use_vlw = true;
    s_scale_vlw_size = findVlwSizeForHeight(scale_target);
  } else {
    const lgfx::GFXfont* cardinal_candidates[] = {&radar_fonts::FreeSansBold12pt7b,
                                                  &radar_fonts::FreeSansBold9pt7b};
    s_cardinal_gfx =
        pickGfxFontClosest(cardinal_target, cardinal_candidates, 2);
    s_cardinal_use_vlw = false;

    const int cardinal_h = measureGfxHeight(*s_cardinal_gfx);
    const int scale_target = cardinal_h - radar::kScaleBelowCardinalPx;
    const lgfx::GFXfont* scale_candidates[] = {&radar_fonts::FreeSansBold9pt7b,
                                               &radar_fonts::FreeSansBold12pt7b};
    s_scale_gfx = pickGfxFontClosest(scale_target, scale_candidates, 2);
    s_scale_use_vlw = false;
  }

  applyScaleStyle();
  s_scale_label_h = tft.fontHeight();
  s_scale_label_max_w = 0;
  char label[12];
  for (size_t i = 0; i < radar::kRangePresetCount; ++i) {
    for (bool miles : {false, true}) {
      radar::formatRing3Label(label, sizeof(label), radar::kRangePresets[i].ring3_km,
                              miles);
      const int w = tft.textWidth(label);
      if (w > s_scale_label_max_w) {
        s_scale_label_max_w = w;
      }
    }
  }

  s_label_metrics_ready = true;
}

void initTagLabelMetrics() {
  if (s_tag_label_metrics_ready) {
    return;
  }

  const int target = radar::kAircraftTagLabelHeightPx;
  if (displayFontIsSmooth()) {
    s_tag_use_vlw = true;
    s_tag_vlw_size = findVlwSizeForHeight(target);
  } else {
    const lgfx::GFXfont* tag_candidates[] = {&radar_fonts::FreeSansBold12pt7b,
                                               &radar_fonts::FreeSansBold9pt7b};
    s_tag_gfx = pickGfxFontClosest(target, tag_candidates, 2);
    s_tag_use_vlw = false;
  }

  s_tag_label_metrics_ready = true;
}

void initPalette() {
  radar::kColorBackground = tft.color565(radar::kBgR, radar::kBgG, radar::kBgB);
  radar::kColorGrid = tft.color565(radar::kGridR, radar::kGridG, radar::kGridB);
  radar::kColorLabel = tft.color565(255, 255, 255);
  radar::kColorCenter = tft.color565(255, 255, 255);
  // GC9A01 BGR panel: swap R/B in color565 so logical red renders red on screen.
  if (config::kDisplayRgbOrder) {
    radar::kColorAircraft =
        tft.color565(radar::kAircraftB, radar::kAircraftG, radar::kAircraftR);
    radar::kColorMilitary =
        tft.color565(radar::kMilitaryB, radar::kMilitaryG, radar::kMilitaryR);
  } else {
    radar::kColorAircraft =
        tft.color565(radar::kAircraftR, radar::kAircraftG, radar::kAircraftB);
    radar::kColorMilitary =
        tft.color565(radar::kMilitaryR, radar::kMilitaryG, radar::kMilitaryB);
  }
  radar::kColorTrackVector =
      tft.color565(radar::kTrackR, radar::kTrackG, radar::kTrackB);
  radar::kColorTrail =
      tft.color565(radar::kTrailR, radar::kTrailG, radar::kTrailB);
  radar::kColorTagType =
      tft.color565(radar::kTagTypeR, radar::kTagTypeG, radar::kTagTypeB);
  radar::kColorTagAltitude =
      tft.color565(radar::kTagAltR, radar::kTagAltG, radar::kTagAltB);
  radar::kColorTagSpeed =
      tft.color565(radar::kTagSpeedR, radar::kTagSpeedG, radar::kTagSpeedB);
  radar::kColorRunway =
      tft.color565(radar::kRunwayR, radar::kRunwayG, radar::kRunwayB);
  radar::kColorRunwayLabel = tft.color565(radar::kRunwayLabelR, radar::kRunwayLabelG,
                                          radar::kRunwayLabelB);
}

constexpr float kKmPerDeg = 111.0f;

void offsetKmFromCenter(float lat, float lon, float* dx_km, float* dy_km,
                        float* dist_km) {
  *dx_km =
      static_cast<float>(lon - services::location::lon()) * kKmPerDeg;
  *dy_km =
      static_cast<float>(lat - services::location::lat()) * kKmPerDeg;
  *dist_km = sqrtf((*dx_km) * (*dx_km) + (*dy_km) * (*dy_km));
}

float innerRingMaxKm() {
  const float outer_km = radar::rangeCurrent().outer_km;
  return outer_km * (static_cast<float>(radar::kGridOuterRadius -
                                       radar::kAircraftInsideRingInsetPx) /
                     static_cast<float>(radar::kGridOuterRadius));
}

/** Flat lat/lon as x/y: 1° ≈ 111 km, north = screen up. */
void latLonToScreen(float lat, float lon, int* out_x, int* out_y) {
  const float outer_km = radar::rangeCurrent().outer_km;
  const float px_per_km = static_cast<float>(radar::kGridOuterRadius) / outer_km;

  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);

  *out_x = radar::kCenterX + static_cast<int>(lroundf(dx_km * px_per_km));
  *out_y = radar::kCenterY - static_cast<int>(lroundf(dy_km * px_per_km));
}

bool isInsideOuterRingKm(float dist_km) { return dist_km <= innerRingMaxKm(); }

int distSqFromCenter(int x, int y) {
  const int dx = x - radar::kCenterX;
  const int dy = y - radar::kCenterY;
  return dx * dx + dy * dy;
}

bool isInsideOuterRing(int x, int y) {
  const int max_r = radar::kGridOuterRadius - radar::kAircraftInsideRingInsetPx;
  return distSqFromCenter(x, y) <= max_r * max_r;
}

/** Rim dot from true bearing; always on screen edge (even if target is 50+ km away). */
bool beyondRingEdgeDotFromLatLon(float lat, float lon, int* out_x, int* out_y) {
  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);
  if (dist_km < 0.01f) {
    return false;
  }
  if (isInsideOuterRingKm(dist_km)) {
    return false;
  }

  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int rim_r = radar::kCenterX - radar::kBeyondRingScreenMarginPx;
  const float angle_rad = atan2f(dx_km, dy_km);

  *out_x = cx + static_cast<int>(lroundf(sinf(angle_rad) * rim_r));
  *out_y = cy - static_cast<int>(lroundf(cosf(angle_rad) * rim_r));
  return true;
}

void drawBeyondRingDot(int x, int y, uint16_t color) {
  s_draw->fillSmoothCircle(x, y, radar::kBeyondRingDotRadiusPx,
                           color);
}

void drawHistoryTrail(const services::adsb::Aircraft& aircraft) {
  for (uint8_t i = 0; i < aircraft.trail_count; ++i) {
    int x = 0;
    int y = 0;
    latLonToScreen(aircraft.trail[i].lat, aircraft.trail[i].lon, &x, &y);
    if (distSqFromCenter(x, y) <=
        radar::kGridOuterRadius * radar::kGridOuterRadius) {
      s_draw->fillSmoothCircle(x, y, radar::kTrailDotRadiusPx,
                               radar::kColorTrail);
    }
  }
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

int speedLineLengthPx(float gs_knots) {
  if (gs_knots <= 0.0f) {
    return 0;
  }

  // Fixed screen scale: 60 s horizon at gs, not tied to current range zoom.
  constexpr float kKmPerKnotPerHorizon =
      1.852f * radar::kAircraftTrackHorizonSec / 3600.0f;
  const float px =
      gs_knots * kKmPerKnotPerHorizon * radar::kGridOuterRadius /
      radar::kAircraftTrackRefOuterKm * radar::kAircraftTrackLengthScale;

  const int len = static_cast<int>(px + 0.5f);
  if (len < radar::kAircraftSpeedLineMinPx) {
    return radar::kAircraftSpeedLineMinPx;
  }
  return len;
}

void noseTip(int cx, int cy, float heading_deg, int* tip_x, int* tip_y) {
  constexpr float kDegToRad = 0.01745329252f;
  const float rad = heading_deg * kDegToRad;
  *tip_x = cx + static_cast<int>(lroundf(sinf(rad) * radar::kAircraftNoseLenPx));
  *tip_y = cy - static_cast<int>(lroundf(cosf(rad) * radar::kAircraftNoseLenPx));
}

void drawHeadingTriangle(int cx, int cy, float heading_deg, uint16_t color) {
  constexpr float kDegToRad = 0.01745329252f;
  const float rad = heading_deg * kDegToRad;
  const float sin_h = sinf(rad);
  const float cos_h = cosf(rad);

  int tip_x = 0;
  int tip_y = 0;
  noseTip(cx, cy, heading_deg, &tip_x, &tip_y);

  const int base_x =
      cx - static_cast<int>(lroundf(sin_h * static_cast<float>(radar::kAircraftTailLenPx)));
  const int base_y =
      cy + static_cast<int>(lroundf(cos_h * static_cast<float>(radar::kAircraftTailLenPx)));

  const int wing_x = static_cast<int>(lroundf(cos_h * radar::kAircraftTailHalfPx));
  const int wing_y = static_cast<int>(lroundf(sin_h * radar::kAircraftTailHalfPx));

  s_draw->fillTriangle(tip_x, tip_y, base_x + wing_x, base_y + wing_y,
                       base_x - wing_x, base_y - wing_y, color);
}

void drawSpeedVector(int cx, int cy, float heading_deg, float track_deg,
                     float gs_knots, uint16_t color) {
  const int len = speedLineLengthPx(gs_knots);
  if (len <= 0) {
    return;
  }

  int tip_x = 0;
  int tip_y = 0;
  noseTip(cx, cy, heading_deg, &tip_x, &tip_y);

  constexpr float kDegToRad = 0.01745329252f;
  const float rad = track_deg * kDegToRad;
  int ex = tip_x + static_cast<int>(lroundf(sinf(rad) * len));
  int ey = tip_y - static_cast<int>(lroundf(cosf(rad) * len));
  clipPointToOuterRing(tip_x, tip_y, &ex, &ey);
  if (ex == tip_x && ey == tip_y) {
    return;
  }
  s_draw->drawWideLine(tip_x, tip_y, ex, ey, radar::kAircraftTrackLineHalfWidth,
                       color);
}

void applyTagStyle() {
  if (s_tag_use_vlw) {
    displayFontSetSmoothSize(*s_draw, s_tag_vlw_size);
  } else {
    displayFontSetBitmap(*s_draw, s_tag_gfx);
  }
}

int measureTagBlockWidth(const services::adsb::Aircraft& plane) {
  applyTagStyle();
  int max_w = 0;
  if (plane.callsign[0] != '\0') {
    const int w = s_draw->textWidth(plane.callsign);
    if (w > max_w) {
      max_w = w;
    }
  }
  if (plane.type[0] != '\0') {
    const int w = s_draw->textWidth(plane.type);
    if (w > max_w) {
      max_w = w;
    }
  }
  if (plane.alt[0] != '\0') {
    const int w = s_draw->textWidth(plane.alt);
    if (w > max_w) {
      max_w = w;
    }
  }
  if (plane.speed[0] != '\0') {
    const int w = s_draw->textWidth(plane.speed);
    if (w > max_w) {
      max_w = w;
    }
  }
  return max_w;
}

int tagBlockLineCount(const services::adsb::Aircraft& plane) {
  int lines = 0;
  lines += plane.callsign[0] != '\0';
  lines += plane.type[0] != '\0';
  lines += plane.alt[0] != '\0';
  lines += plane.speed[0] != '\0';
  return std::max(1, lines);
}

struct TagPlacement {
  int anchor_x;
  int top;
  int left;
  int right;
  int bottom;
  bool tag_on_right;
};

bool tagPlacementsTouch(const TagPlacement& a, const TagPlacement& b);

constexpr int kRunwayTagOverlapPenalty = 10000;
constexpr int kEdgeInwardTagPenalty = 1000;
constexpr int kOtherTagOverlapPenalty = 1000000;
constexpr int kDistantTagPenalty = 100000000;

int tagPlacementInkScore(const TagPlacement& placement) {
  // The physical panel has no MISO connection, and detailed framebuffer
  // sampling is deliberately disabled on busy scopes. Returning the same score
  // preserves the center-facing tie-breaker while geometric collision checks
  // still keep tags away from runways and one another.
  if (s_draw == &tft || s_fast_tag_placement) {
    return 0;
  }

  // The frame already contains the grid, runways, trails, vectors, and aircraft
  // symbols. Prefer the candidate whose footprint covers the fewest drawn
  // pixels, rather than always putting the tag on a fixed side of the target.
  const uint32_t background = s_draw->readPixel(0, 0);
  int score = 0;
  // Runway and tag intersections are checked geometrically below, so a coarse
  // sample is enough for general grid/trail avoidance and substantially cuts
  // work on busy, zoomed-out scopes.
  const int sample_top = std::max(0, placement.top);
  const int sample_bottom = std::min(radar::kSize, placement.bottom);
  const int sample_left = std::max(0, placement.left);
  const int sample_right = std::min(radar::kSize, placement.right);
  for (int y = sample_top; y < sample_bottom; y += 4) {
    for (int x = sample_left; x < sample_right; x += 4) {
      if (s_draw->readPixel(x, y) != background) {
        ++score;
      }
    }
  }
  return score;
}

TagPlacement makeTagPlacement(int left, int top, int block_w, int block_h,
                              bool align_left) {
  // Do not pull tags into either the rectangular framebuffer or the round
  // panel. Natural clipping lets an entering aircraft symbol lead its details
  // onto the scope and lets an exiting aircraft pull its details off-screen.
  return {align_left ? left : left + block_w, top, left, left + block_w,
          top + block_h, align_left};
}

float tagCenterDistanceForGap(float direction_x, float direction_y, int block_w,
                              int block_h, int gap) {
  // Find the center offset whose actual point-to-rectangle distance is `gap`.
  // Adding the two projected half-extents overestimates this offset diagonally
  // and can push a wide, multi-line tag beyond kAircraftTagMaxDistancePx.
  const float half_w = block_w * 0.5f;
  const float half_h = block_h * 0.5f;
  float low = 0.0f;
  float high = half_w + half_h + gap;
  for (int step = 0; step < 16; ++step) {
    const float mid = (low + high) * 0.5f;
    const float dx = std::max(0.0f, fabsf(direction_x * mid) - half_w);
    const float dy = std::max(0.0f, fabsf(direction_y * mid) - half_h);
    if (dx * dx + dy * dy < gap * gap) {
      low = mid;
    } else {
      high = mid;
    }
  }
  return high;
}

TagPlacement placeAircraftTag(int x, int y,
                              const services::adsb::Aircraft& plane,
                              const TagPlacement* placed,
                              size_t placed_count, const int* aircraft_x,
                              const int* aircraft_y, size_t aircraft_count) {
  initTagLabelMetrics();
  applyTagStyle();

  const int line_h = s_draw->fontHeight();
  const int block_w = measureTagBlockWidth(plane);
  const int block_h = line_h * tagBlockLineCount(plane);
  const int symbol_half =
      radar::kAircraftNoseLenPx + radar::kAircraftTailHalfPx;
  const int gap = symbol_half + radar::kAircraftLabelGapPx;
  const int radial_x = x - radar::kCenterX;
  const int radial_y = y - radar::kCenterY;
  const int edge_threshold = radar::kGridOuterRadius * 3 / 4;
  const bool near_edge = radial_x * radial_x + radial_y * radial_y >=
                         edge_threshold * edge_threshold;

  // Near the rim, try positions tangent to the circle before the usual
  // center-facing choices. A tangential tag is clipped by the round panel as
  // it enters, so the aircraft appears first, without putting its history
  // trail directly underneath the details.
  TagPlacement candidates[14];
  size_t candidate_count = 0;
  const auto add_candidate = [&](int left, int top, bool align_left) {
    candidates[candidate_count++] =
        makeTagPlacement(left, top, block_w, block_h, align_left);
  };
  if (near_edge) {
    const float radial_len = sqrtf(static_cast<float>(radial_x * radial_x +
                                                       radial_y * radial_y));
    const float tangent_x = -radial_y / radial_len;
    const float tangent_y = radial_x / radial_len;
    const float tangent_distance = tagCenterDistanceForGap(
        tangent_x, tangent_y, block_w, block_h, gap);
    for (const float direction : {-1.0f, 1.0f}) {
      const int center_x =
          x + static_cast<int>(
                  lroundf(tangent_x * tangent_distance * direction));
      const int center_y =
          y + static_cast<int>(
                  lroundf(tangent_y * tangent_distance * direction));
      add_candidate(center_x - block_w / 2, center_y - block_h / 2,
                    tangent_x * direction >= 0.0f);
    }
  }
  // Preserve the normal preference for placing details toward the center.
  if (x < radar::kCenterX) {
    add_candidate(x + gap, y - block_h / 2, true);
    add_candidate(x - gap - block_w, y - block_h / 2, false);
  } else {
    add_candidate(x - gap - block_w, y - block_h / 2, false);
    add_candidate(x + gap, y - block_h / 2, true);
  }
  add_candidate(x - block_w / 2, y - gap - block_h, true);
  add_candidate(x - block_w / 2, y + gap, true);
  add_candidate(x + gap, y - gap - block_h, true);
  add_candidate(x - gap - block_w, y - gap - block_h, false);
  add_candidate(x + gap, y + gap, true);
  add_candidate(x - gap - block_w, y + gap, false);
  // Side placements offset by a half block give moving aircraft room to keep
  // their own label visible before it truly reaches a neighboring label.
  add_candidate(x + gap, y - block_h, true);
  add_candidate(x - gap - block_w, y - block_h, false);
  add_candidate(x + gap, y, true);
  add_candidate(x - gap - block_w, y, false);

  const auto hard_conflict = [&](const TagPlacement& candidate) {
    const int nearest_x =
        std::max(candidate.left, std::min(x, candidate.right));
    const int nearest_y =
        std::max(candidate.top, std::min(y, candidate.bottom));
    const int tag_dx = x - nearest_x;
    const int tag_dy = y - nearest_y;
    const int max_distance = radar::kAircraftTagMaxDistancePx;
    if (tag_dx * tag_dx + tag_dy * tag_dy > max_distance * max_distance ||
        runway::tagRectOverlapsRunway(candidate.left, candidate.top,
                                      candidate.right, candidate.bottom)) {
      return true;
    }
    for (size_t p = 0; p < placed_count; ++p) {
      if (tagPlacementsTouch(candidate, placed[p])) {
        return true;
      }
    }
    // Treat every aircraft symbol as occupied even when busy-scope pixel
    // sampling is disabled. Do not let another aircraft's text cover it.
    for (size_t a = 0; a < aircraft_count; ++a) {
      if (aircraft_x[a] == x && aircraft_y[a] == y) {
        continue;
      }
      const int symbol_x =
          std::max(candidate.left, std::min(aircraft_x[a], candidate.right));
      const int symbol_y =
          std::max(candidate.top, std::min(aircraft_y[a], candidate.bottom));
      const int dx = aircraft_x[a] - symbol_x;
      const int dy = aircraft_y[a] - symbol_y;
      if (dx * dx + dy * dy <= symbol_half * symbol_half) {
        return true;
      }
    }
    return false;
  };

  CachedTagChoice* cached = cachedTagChoice(plane.id);
  if (cached != nullptr && cached->valid &&
      cached->uses_edge_candidates == near_edge &&
      cached->candidate < candidate_count &&
      !hard_conflict(candidates[cached->candidate])) {
    return candidates[cached->candidate];
  }

  size_t best = 0;
  int best_score = 0;
  for (size_t i = 0; i < candidate_count; ++i) {
    int score = tagPlacementInkScore(candidates[i]);
    if (near_edge && i >= 2) {
      // Prefer either tangential entry position unless it creates a genuine
      // runway/tag/symbol conflict. This prevents a low-ink center-facing tag
      // from becoming visible before the aircraft at the rim.
      score += kEdgeInwardTagPenalty;
    }
    const int nearest_x =
        std::max(candidates[i].left, std::min(x, candidates[i].right));
    const int nearest_y =
        std::max(candidates[i].top, std::min(y, candidates[i].bottom));
    const int tag_dx = x - nearest_x;
    const int tag_dy = y - nearest_y;
    const int max_distance = radar::kAircraftTagMaxDistancePx;
    if (tag_dx * tag_dx + tag_dy * tag_dy > max_distance * max_distance) {
      score += kDistantTagPenalty;
    }
    if (runway::tagRectOverlapsRunway(candidates[i].left, candidates[i].top,
                                      candidates[i].right,
                                      candidates[i].bottom)) {
      score += kRunwayTagOverlapPenalty;
    }
    for (size_t p = 0; p < placed_count; ++p) {
      if (tagPlacementsTouch(candidates[i], placed[p])) {
        score += kOtherTagOverlapPenalty;
      }
    }
    for (size_t a = 0; a < aircraft_count; ++a) {
      if (aircraft_x[a] == x && aircraft_y[a] == y) {
        continue;
      }
      const int symbol_x = std::max(
          candidates[i].left, std::min(aircraft_x[a], candidates[i].right));
      const int symbol_y = std::max(
          candidates[i].top, std::min(aircraft_y[a], candidates[i].bottom));
      const int dx = aircraft_x[a] - symbol_x;
      const int dy = aircraft_y[a] - symbol_y;
      if (dx * dx + dy * dy <= symbol_half * symbol_half) {
        score += kOtherTagOverlapPenalty;
      }
    }
    if (i == 0 || score < best_score) {
      best = i;
      best_score = score;
    }
  }
  if (cached != nullptr) {
    cached->candidate = static_cast<uint8_t>(best);
    cached->valid = true;
    cached->uses_edge_candidates = near_edge;
  }
  return candidates[best];
}

bool tagPlacementsTouch(const TagPlacement& a, const TagPlacement& b) {
  const int gap = radar::kAircraftTagCollisionGapPx;
  return a.left <= b.right + gap && b.left <= a.right + gap &&
         a.top <= b.bottom + gap && b.top <= a.bottom + gap;
}

void drawAircraftTag(const services::adsb::Aircraft& plane,
                     const TagPlacement& placement) {
  applyTagStyle();
  s_draw->setTextDatum(placement.tag_on_right ? textdatum_t::top_left
                                               : textdatum_t::top_right);
  const int line_h = s_draw->fontHeight();
  const int anchor_x = placement.anchor_x;
  int ly = placement.top;

  if (plane.callsign[0] != '\0') {
    s_draw->setTextColor(plane.military ? radar::kColorMilitary
                                        : radar::kColorLabel,
                         radar::kColorBackground);
    s_draw->drawString(plane.callsign, anchor_x, ly);
    ly += line_h;
  }

  if (plane.type[0] != '\0') {
    s_draw->setTextColor(radar::kColorTagType, radar::kColorBackground);
    s_draw->drawString(plane.type, anchor_x, ly);
    ly += line_h;
  }

  if (plane.alt[0] != '\0') {
    s_draw->setTextColor(radar::kColorTagAltitude, radar::kColorBackground);
    s_draw->drawString(plane.alt, anchor_x, ly);
    ly += line_h;
  }

  if (plane.speed[0] != '\0') {
    s_draw->setTextColor(radar::kColorTagSpeed, radar::kColorBackground);
    s_draw->drawString(plane.speed, anchor_x, ly);
  }
}

size_t tagGroupRoot(size_t* parents, size_t index) {
  while (parents[index] != index) {
    parents[index] = parents[parents[index]];
    index = parents[index];
  }
  return index;
}

void joinTagGroups(size_t* parents, size_t a, size_t b) {
  const size_t root_a = tagGroupRoot(parents, a);
  const size_t root_b = tagGroupRoot(parents, b);
  if (root_a != root_b) {
    parents[root_b] = root_a;
  }
}

struct AircraftDrawItem {
  size_t index = 0;
  int x = 0;
  int y = 0;
  int dist_sq = 0;
};

struct BeyondDotDrawItem {
  int x = 0;
  int y = 0;
  int dist_sq = 0;
  // Snapshot the matching aircraft color when this independent list is built;
  // indexing the in-ring items[] list here would associate the wrong aircraft.
  uint16_t color = 0;
};

void sortDrawItemsFarFirst(AircraftDrawItem* items, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const AircraftDrawItem key = items[i];
    size_t j = i;
    while (j > 0 && items[j - 1].dist_sq < key.dist_sq) {
      items[j] = items[j - 1];
      --j;
    }
    items[j] = key;
  }
}

void sortBeyondDotsFarFirst(BeyondDotDrawItem* items, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const BeyondDotDrawItem key = items[i];
    size_t j = i;
    while (j > 0 && items[j - 1].dist_sq < key.dist_sq) {
      items[j] = items[j - 1];
      --j;
    }
    items[j] = key;
  }
}

void drawAircraft() {
  initLabelMetrics();

  const size_t n = services::adsb::aircraftCount();
  const services::adsb::Aircraft* planes = services::adsb::aircraftList();

  // These fixed-size workspaces live in BSS rather than the ESP32-C3's small
  // Arduino loop-task stack. Only the entries below draw_count/dot_count are
  // used during a frame.
  static AircraftDrawItem items[services::adsb::kMaxAircraft];
  static BeyondDotDrawItem dots[services::adsb::kMaxAircraft];
  size_t draw_count = 0;
  size_t dot_count = 0;

  for (size_t i = 0; i < n; ++i) {
    float dx_km = 0.0f;
    float dy_km = 0.0f;
    float dist_km = 0.0f;
    offsetKmFromCenter(planes[i].lat, planes[i].lon, &dx_km, &dy_km, &dist_km);

    if (isInsideOuterRingKm(dist_km)) {
      int x = 0;
      int y = 0;
      latLonToScreen(planes[i].lat, planes[i].lon, &x, &y);
      items[draw_count].index = i;
      items[draw_count].x = x;
      items[draw_count].y = y;
      items[draw_count].dist_sq = distSqFromCenter(x, y);
      ++draw_count;
      continue;
    }

    int dot_x = 0;
    int dot_y = 0;
    if (!beyondRingEdgeDotFromLatLon(planes[i].lat, planes[i].lon, &dot_x,
                                     &dot_y)) {
      continue;
    }
    dots[dot_count].x = dot_x;
    dots[dot_count].y = dot_y;
    dots[dot_count].dist_sq = distSqFromCenter(dot_x, dot_y);
    dots[dot_count].color =
        planes[i].military ? radar::kColorMilitary : radar::kColorAircraft;
    ++dot_count;
  }

  sortBeyondDotsFarFirst(dots, dot_count);
  for (size_t d = 0; d < dot_count; ++d) {
    drawBeyondRingDot(dots[d].x, dots[d].y, dots[d].color);
  }

  sortDrawItemsFarFirst(items, draw_count);
  // Framebuffer sampling scales with aircraft count, candidate count, and tag
  // area. On busy scopes retain the inexpensive geometric runway/tag checks but
  // skip the cosmetic grid/trail sampling that otherwise causes long stalls.
  constexpr size_t kDetailedPlacementAircraftLimit = 16;
  s_fast_tag_placement = draw_count > kDetailedPlacementAircraftLimit;
  for (size_t d = 0; d < draw_count; ++d) {
    drawHistoryTrail(planes[items[d].index]);
  }
  for (size_t d = 0; d < draw_count; ++d) {
    const size_t i = items[d].index;
    const int x = items[d].x;
    const int y = items[d].y;
    drawSpeedVector(x, y, planes[i].nose_deg, planes[i].track_deg,
                    planes[i].gs_knots, radar::kColorTrackVector);
    const uint16_t aircraft_color =
        planes[i].military ? radar::kColorMilitary : radar::kColorAircraft;
    drawHeadingTriangle(x, y, planes[i].nose_deg, aircraft_color);
  }

  static TagPlacement placements[services::adsb::kMaxAircraft];
  static TagPlacement placed[services::adsb::kMaxAircraft];
  static int aircraft_x[services::adsb::kMaxAircraft];
  static int aircraft_y[services::adsb::kMaxAircraft];
  static size_t placement_order[services::adsb::kMaxAircraft];
  for (size_t d = 0; d < draw_count; ++d) {
    aircraft_x[d] = items[d].x;
    aircraft_y[d] = items[d].y;
    placement_order[d] = d;
  }
  for (size_t i = 1; i < draw_count; ++i) {
    const size_t key = placement_order[i];
    size_t j = i;
    while (j > 0 &&
           strcmp(planes[items[placement_order[j - 1]].index].id,
                  planes[items[key].index].id) > 0) {
      placement_order[j] = placement_order[j - 1];
      --j;
    }
    placement_order[j] = key;
  }
  for (size_t order = 0; order < draw_count; ++order) {
    const size_t d = placement_order[order];
    const size_t i = items[d].index;
    placements[d] = placeAircraftTag(items[d].x, items[d].y, planes[i],
                                     placed, order, aircraft_x, aircraft_y,
                                     draw_count);
    placed[order] = placements[d];
  }

  static bool collides[services::adsb::kMaxAircraft];
  static size_t parents[services::adsb::kMaxAircraft];
  for (size_t d = 0; d < draw_count; ++d) {
    collides[d] = false;
    parents[d] = d;
  }
  for (size_t a = 0; a < draw_count; ++a) {
    for (size_t b = a + 1; b < draw_count; ++b) {
      if (tagPlacementsTouch(placements[a], placements[b])) {
        collides[a] = true;
        collides[b] = true;
        joinTagGroups(parents, a, b);
      }
    }
  }
  const size_t phase = millis() / radar::kAircraftTagPagePeriodMs;
  static bool visible[services::adsb::kMaxAircraft];
  for (size_t d = 0; d < draw_count; ++d) {
    visible[d] = false;
  }
  // Page each collision component independently. Within a component, greedily
  // show every compatible tag, so A and C can both be visible when only B
  // overlaps them. A singleton component is therefore visible immediately.
  for (size_t root_candidate = 0; root_candidate < draw_count;
       ++root_candidate) {
    if (tagGroupRoot(parents, root_candidate) != root_candidate) {
      continue;
    }
    static size_t members[services::adsb::kMaxAircraft];
    size_t member_count = 0;
    for (size_t d = 0; d < draw_count; ++d) {
      if (tagGroupRoot(parents, d) == root_candidate) {
        members[member_count++] = d;
      }
    }
    // Aircraft distance/order can change on every ADS-B refresh. Sort by the
    // stable ICAO identifier so the selected page does not change until the
    // configured page period expires.
    for (size_t i = 1; i < member_count; ++i) {
      const size_t key = members[i];
      size_t j = i;
      while (j > 0 &&
             strcmp(planes[items[members[j - 1]].index].id,
                    planes[items[key].index].id) > 0) {
        members[j] = members[j - 1];
        --j;
      }
      members[j] = key;
    }
    for (size_t rank = 0; rank < member_count; ++rank) {
      const size_t d = members[(phase + rank) % member_count];
      bool blocked = false;
      for (size_t prior = 0; prior < member_count; ++prior) {
        const size_t other = members[prior];
        if (visible[other] &&
            tagPlacementsTouch(placements[d], placements[other])) {
          blocked = true;
          break;
        }
      }
      if (!blocked) {
        visible[d] = true;
      }
    }
  }

  for (size_t d = 0; d < draw_count; ++d) {
    if (visible[d] && collides[d]) {
      const size_t i = items[d].index;
      // Keep the selected aircraft blue for the entire time its information is
      // displayed in a collision group.
      drawHeadingTriangle(items[d].x, items[d].y, planes[i].nose_deg,
                          radar::kColorTagType);
    }
  }

  for (size_t d = 0; d < draw_count; ++d) {
    if (!visible[d]) {
      continue;
    }
    const size_t i = items[d].index;
    drawAircraftTag(planes[i], placements[d]);
  }
}

void applyCardinalStyle() {
  if (s_cardinal_use_vlw) {
    displayFontSetSmoothSize(*s_draw, s_cardinal_vlw_size);
  } else {
    displayFontSetBitmap(*s_draw, s_cardinal_gfx);
  }
}

void applyScaleStyle() {
  if (s_scale_use_vlw) {
    displayFontSetSmoothSize(*s_draw, s_scale_vlw_size);
  } else {
    displayFontSetBitmap(*s_draw, s_scale_gfx);
  }
}

void drawCardinalLabel(const char* text, int x, int y, textdatum_t datum) {
  applyCardinalStyle();
  s_draw->setTextDatum(datum);
  s_draw->setTextColor(radar::kColorLabel, radar::kColorBackground);
  s_draw->drawString(text, x, y);
}

void drawScaleLabelWithBackground(const char* text, int x, int y) {
  applyScaleStyle();
  s_draw->setTextDatum(textdatum_t::middle_right);

  const int tw = s_draw->textWidth(text);
  const int th = s_draw->fontHeight();
  constexpr int kPadX = 3;
  constexpr int kPadY = 2;

  const int left = x - tw - kPadX;
  const int top = y - th / 2 - kPadY;

  s_draw->fillRect(left, top, tw + kPadX * 2, th + kPadY * 2,
                   radar::kColorBackground);
  s_draw->setTextColor(radar::kColorGrid, radar::kColorBackground);
  s_draw->drawString(text, x, y);
}

void drawGridRing(int cx, int cy, int r, uint16_t color) {
  if (r <= 0) {
    return;
  }
  const int thickness =
      std::max(1, static_cast<int>(radar::kGridStrokeHalfWidth * 2.0f));
  for (int i = 0; i < thickness && r - i > 0; ++i) {
    s_draw->drawCircle(cx, cy, r - i, color);
  }
}

void drawRings(int cx, int cy, int outer_radius) {
  for (int i = 1; i <= radar::kRingCount; ++i) {
    const int r = (outer_radius * i) / radar::kRingCount;
    drawGridRing(cx, cy, r, radar::kColorGrid);
  }
}

void drawCrosshairs(int cx, int cy, int radius, uint16_t color) {
  s_draw->drawWideLine(cx, cy - radius, cx, cy + radius,
                       radar::kGridStrokeHalfWidth, color);
  s_draw->drawWideLine(cx - radius, cy, cx + radius, cy,
                       radar::kGridStrokeHalfWidth, color);
}

void drawCenterDot(int cx, int cy) {
  s_draw->fillSmoothCircle(cx, cy, radar::kCenterDotRadius, radar::kColorCenter);
}

void drawCardinalLabels() {
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int edge = radar::kSize - 1;

  drawCardinalLabel("N", cx, radar::kCardinalNorthOffsetY, textdatum_t::top_center);
  drawCardinalLabel("S", cx, edge + radar::kCardinalSouthOffsetY,
                    textdatum_t::bottom_center);
  drawCardinalLabel("W", 0, cy, textdatum_t::middle_left);
  drawCardinalLabel("E", edge, cy, textdatum_t::middle_right);
}

int scaleLabelAnchorX(int cx, int outer_radius) {
  return cx + outer_radius - radar::kScaleGapFromOuterRing;
}

void drawScaleLabel(int cx, int cy, int outer_radius) {
  char scale_label[12];
  radar::formatCurrentRing3Label(scale_label, sizeof(scale_label));
  drawScaleLabelWithBackground(scale_label,
                               scaleLabelAnchorX(cx, outer_radius), cy);
}

template <typename Gfx>
void drawStaticGrid(Gfx& gfx) {
  initLabelMetrics();
  const DrawScope scope(gfx);
  displayFontEnsureLoaded(gfx);
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int grid_r = radar::kGridOuterRadius;

  gfx.fillScreen(radar::kColorBackground);
  drawRings(cx, cy, grid_r);
  drawCrosshairs(cx, cy, grid_r, radar::kColorGrid);
  initPalette();
  runway::drawLargeAirportRunways(gfx);
  drawCenterDot(cx, cy);
  drawCardinalLabels();
  drawScaleLabel(cx, cy, grid_r);
  gfx.setTextDatum(textdatum_t::top_left);
}

bool ensureFrameSprite() {
  if (s_frame_ready) {
    return true;
  }
  s_frame.setColorDepth(16);
  if (!s_frame.createSprite(radar::kSize, radar::kSize)) {
    Serial.println("radar: frame sprite alloc failed");
    return false;
  }
  s_frame_ready = true;
  return true;
}

// Double-buffered frame: composite the grid AND aircraft into the off-screen
// sprite, then blit it to the panel in a single pushSprite. Because the panel
// is updated in one pass, labels never show an erase/redraw gap — no flicker.
void renderFrame() {
  drawStaticGrid(s_frame);  // opens its own DrawScope(s_frame)
  {
    const DrawScope scope(s_frame);
    drawAircraft();
  }
  s_frame.pushSprite(0, 0);
  tft.setTextDatum(textdatum_t::top_left);
}

}  // namespace

void radarDisplayDraw() {
  initPalette();
  initLabelMetrics();

  if (ensureFrameSprite()) {
    renderFrame();
    return;
  }

  // Fallback when the sprite can't be allocated: draw straight to the panel.
  const DrawScope scope(tft);
  drawStaticGrid(tft);
  drawAircraft();
  tft.setTextDatum(textdatum_t::top_left);
}

void radarDisplayRefreshAircraft() {
  initPalette();

  if (ensureFrameSprite()) {
    renderFrame();
    return;
  }

  radarDisplayDraw();
}

}  // namespace ui
