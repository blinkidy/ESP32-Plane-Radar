#include "ui/radar_display.h"

#include <lgfx/v1/lgfx_fonts.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/adsb_client.h"
#include "services/radar_location.h"
#include "ui/radar_animation_policy.h"
#include "ui/radar_projection.h"
#include "ui/radar_range.h"
#include "ui/radar_tag_layout_policy.h"
#include "ui/radar_theme.h"
#include "ui/runway_overlay.h"

namespace fonts = lgfx::v1::fonts;

namespace ui {
namespace radar {

uint16_t kColorBackground = 0x0000;
uint16_t kColorGrid = 0x0320;
uint16_t kColorLabel = 0xFFFF;
uint16_t kColorCenter = 0xFFFF;
uint16_t kColorAircraft = 0x001F;
uint16_t kColorTrackVector = 0xFFFF;
uint16_t kColorTagType = 0x5DFF;
uint16_t kColorTagAltitude = 0xFFE0;
uint16_t kColorTagSpeed = 0xAD79;
uint16_t kColorRunway = 0x4D5F;
uint16_t kColorRunwayLabel = 0x7DFF;
uint16_t kColorRunwayIdent = 0x5DDA;

}  // namespace radar

namespace {

bool s_label_metrics_ready = false;
bool s_cardinal_use_vlw = false;
bool s_scale_use_vlw = false;
float s_cardinal_vlw_size = 0.56f;
float s_scale_vlw_size = 0.50f;
float s_tag_vlw_size = 0.56f;
const lgfx::GFXfont* s_cardinal_gfx = &fonts::FreeSansBold12pt7b;
const lgfx::GFXfont* s_scale_gfx = &fonts::FreeSansBold9pt7b;
const lgfx::GFXfont* s_tag_gfx = &fonts::FreeSansBold12pt7b;

bool s_tag_label_metrics_ready = false;
bool s_tag_use_vlw = false;

int s_scale_label_max_w = 0;
int s_scale_label_h = 0;

lgfx::LovyanGFX* s_draw = &tft;
LGFX_Sprite s_frame(&tft);
bool s_frame_ready = false;
bool s_frame_attempted = false;

/** Sweep trail ramp, background → phosphor, rebuilt in initPalette(). */
constexpr size_t kSweepSteps = 56;
uint16_t s_sweep_ramp[kSweepSteps];
uint16_t s_sweep_edge = 0;

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
    const lgfx::GFXfont* cardinal_candidates[] = {&fonts::FreeSansBold12pt7b,
                                                  &fonts::FreeSansBold9pt7b};
    s_cardinal_gfx =
        pickGfxFontClosest(cardinal_target, cardinal_candidates, 2);
    s_cardinal_use_vlw = false;

    const int cardinal_h = measureGfxHeight(*s_cardinal_gfx);
    const int scale_target = cardinal_h - radar::kScaleBelowCardinalPx;
    const lgfx::GFXfont* scale_candidates[] = {&fonts::FreeSansBold9pt7b,
                                               &fonts::FreeSansBold12pt7b};
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
    const lgfx::GFXfont* tag_candidates[] = {&fonts::FreeSansBold12pt7b,
                                               &fonts::FreeSansBold9pt7b};
    s_tag_gfx = pickGfxFontClosest(target, tag_candidates, 2);
    s_tag_use_vlw = false;
  }

  s_tag_label_metrics_ready = true;
}

uint8_t lerp8(uint8_t from, uint8_t to, float t) {
  return static_cast<uint8_t>(
      lroundf(static_cast<float>(from) +
              (static_cast<float>(to) - static_cast<float>(from)) * t));
}

bool s_sweep_ramp_ready = false;

/**
 * Precompute the trailing glow.
 *
 * Step 0 is the leading edge in bright phosphor; later steps decay toward the
 * background on a squared curve, which reads like a CRT afterglow rather than a
 * linear fade. Colours are opaque blends against the background because the
 * glow is painted under the graticule — the panel has no alpha channel, and a
 * read-modify-write blend per pixel would not fit the frame budget.
 */
void initSweepRamp() {
  if (s_sweep_ramp_ready) {
    return;
  }
  s_sweep_ramp_ready = true;

  const float last = static_cast<float>(kSweepSteps - 1);
  for (size_t i = 0; i < kSweepSteps; ++i) {
    const float t = static_cast<float>(i) / last;
    const float decay = (1.0f - t) * (1.0f - t);
    // Hottest colour only in the first few degrees behind the edge.
    const float edge_mix = t < 0.08f ? (1.0f - t / 0.08f) : 0.0f;
    const uint8_t r = lerp8(radar::kSweepTrailR, radar::kSweepEdgeR, edge_mix);
    const uint8_t g = lerp8(radar::kSweepTrailG, radar::kSweepEdgeG, edge_mix);
    const uint8_t b = lerp8(radar::kSweepTrailB, radar::kSweepEdgeB, edge_mix);
    s_sweep_ramp[i] = tft.color565(lerp8(radar::kBgR, r, decay),
                                   lerp8(radar::kBgG, g, decay),
                                   lerp8(radar::kBgB, b, decay));
  }
  s_sweep_edge =
      tft.color565(radar::kSweepEdgeR, radar::kSweepEdgeG, radar::kSweepEdgeB);
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
  } else {
    radar::kColorAircraft =
        tft.color565(radar::kAircraftR, radar::kAircraftG, radar::kAircraftB);
  }
  radar::kColorTrackVector =
      tft.color565(radar::kTrackR, radar::kTrackG, radar::kTrackB);
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
  radar::kColorRunwayIdent = tft.color565(
      radar::kRunwayIdentR, radar::kRunwayIdentG, radar::kRunwayIdentB);
  initSweepRamp();
}

using radar::offsetKmFromCenter;

float innerRingMaxKm() {
  const float outer_km = radar::rangeCurrent().outer_km;
  return outer_km * (static_cast<float>(radar::kGridOuterRadius -
                                       radar::kAircraftInsideRingInsetPx) /
                     static_cast<float>(radar::kGridOuterRadius));
}

using radar::latLonToScreen;

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

void drawBeyondRingDot(int x, int y) {
  s_draw->fillSmoothCircle(x, y, radar::kBeyondRingDotRadiusPx,
                           radar::kColorAircraft);
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

/**
 * Tag verbosity, tried in order until the block finds space on screen.
 *
 * Five lines of traffic detail rarely all fit on a 240 px disc, so a tag that
 * cannot be placed sheds its least critical rows before being dropped.
 */
enum class TagDetail {
  kFull,     // callsign, type, route, altitude, speed
  kCompact,  // callsign, route, altitude
  kMinimal,  // callsign, altitude
};

constexpr size_t kMaxTagLines = 5;

struct TagLine {
  const char* text;
  uint16_t color;
};

size_t buildTagLines(const services::adsb::Aircraft& plane, TagDetail detail,
                     TagLine* out) {
  size_t n = 0;
  const bool full = detail == TagDetail::kFull;
  const bool with_route = detail != TagDetail::kMinimal;

  if (plane.callsign[0] != '\0') {
    out[n++] = {plane.callsign, radar::kColorLabel};
  }
  if (full && plane.type[0] != '\0') {
    out[n++] = {plane.type, radar::kColorTagType};
  }
  if (with_route && plane.route[0] != '\0') {
    out[n++] = {plane.route, radar::kColorRunwayLabel};
  }
  if (plane.alt[0] != '\0') {
    out[n++] = {plane.alt, radar::kColorTagAltitude};
  }
  if (full && plane.speed[0] != '\0') {
    out[n++] = {plane.speed, radar::kColorTagSpeed};
  }
  return n;
}

int measureTagBlockWidth(const TagLine* lines, size_t count) {
  applyTagStyle();
  int max_w = 0;
  for (size_t i = 0; i < count; ++i) {
    const int w = s_draw->textWidth(lines[i].text);
    if (w > max_w) {
      max_w = w;
    }
  }
  return max_w;
}

void drawAircraftTag(int left, int top, const TagLine* lines, size_t count) {
  applyTagStyle();
  const int line_h = s_draw->fontHeight();
  s_draw->setTextDatum(textdatum_t::top_left);

  int ly = top;
  for (size_t i = 0; i < count; ++i) {
    s_draw->setTextColor(lines[i].color, radar::kColorBackground);
    s_draw->drawString(lines[i].text, left, ly);
    ly += line_h;
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
};

struct TagPlacement {
  radar::TagRect rect = {0, 0, 0, 0};
  TagDetail detail = TagDetail::kFull;
  bool valid = false;
};

bool tagConflictsWithPlaced(radar::TagRect rect, const radar::TagRect* placed,
                            size_t placed_count) {
  for (size_t i = 0; i < placed_count; ++i) {
    if (radar::tagRectsOverlap(rect, placed[i])) {
      return true;
    }
  }
  return false;
}

bool tagOverlapsAircraftSymbol(radar::TagRect rect,
                               const AircraftDrawItem* items,
                               size_t item_count) {
  constexpr int kSymbolHalf =
      radar::kAircraftNoseLenPx + radar::kAircraftTailHalfPx;
  for (size_t i = 0; i < item_count; ++i) {
    const radar::TagRect symbol = {
        items[i].x - kSymbolHalf,
        items[i].y - kSymbolHalf,
        items[i].x + kSymbolHalf + 1,
        items[i].y + kSymbolHalf + 1,
    };
    if (radar::tagRectsOverlap(rect, symbol)) {
      return true;
    }
  }
  return false;
}

/**
 * First candidate slot around the symbol that clears the screen edge, every
 * other aircraft, and every tag already placed. Preference runs outward from
 * the radar centre first, so tags fan away from the busy middle.
 */
TagPlacement findTagPlacement(const TagLine* lines, size_t line_count, int x,
                              int y, TagDetail detail,
                              const AircraftDrawItem* items, size_t item_count,
                              const radar::TagRect* placed,
                              size_t placed_count) {
  if (line_count == 0) {
    return {};
  }
  applyTagStyle();
  const int width = measureTagBlockWidth(lines, line_count);
  const int height = s_draw->fontHeight() * static_cast<int>(line_count);
  if (width <= 0 || height <= 0) {
    return {};
  }

  const int gap = radar::kAircraftNoseLenPx + radar::kAircraftTailHalfPx +
                  radar::kAircraftLabelGapPx;
  const int left = x - gap - width;
  const int right = x + gap;
  const int middle_x = x - width / 2;
  const int top = y - gap - height;
  const int bottom = y + gap;
  const int middle_y = y - height / 2;
  const int outward_x = x < radar::kCenterX ? left : right;
  const int inward_x = x < radar::kCenterX ? right : left;
  const int outward_y = y < radar::kCenterY ? top : bottom;
  const int inward_y = y < radar::kCenterY ? bottom : top;
  const radar::TagRect candidates[] = {
      {outward_x, middle_y, outward_x + width, middle_y + height},
      {middle_x, outward_y, middle_x + width, outward_y + height},
      {outward_x, outward_y, outward_x + width, outward_y + height},
      {outward_x, inward_y, outward_x + width, inward_y + height},
      {inward_x, outward_y, inward_x + width, outward_y + height},
      {inward_x, middle_y, inward_x + width, middle_y + height},
      {middle_x, inward_y, middle_x + width, inward_y + height},
      {inward_x, inward_y, inward_x + width, inward_y + height},
  };

  for (const radar::TagRect& candidate : candidates) {
    if (!radar::tagRectInside(candidate, radar::kSize, radar::kSize) ||
        tagConflictsWithPlaced(candidate, placed, placed_count) ||
        tagOverlapsAircraftSymbol(candidate, items, item_count)) {
      continue;
    }
    return {candidate, detail, true};
  }
  return {};
}

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

void drawAircraft(unsigned long now_ms) {
  initLabelMetrics();
  initTagLabelMetrics();

  size_t n = 0;
  const services::adsb::Aircraft* planes =
      services::adsb::aircraftList(now_ms, &n);

  // Static, not stack: drawAircraft() also runs from the ADS-B poll hook, i.e.
  // inside HTTPClient's TLS read loop, where the ~3 KB these would cost is more
  // than the 8 KB Arduino loop stack can spare. Single-task, never re-entered.
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
    ++dot_count;
  }

  sortBeyondDotsFarFirst(dots, dot_count);
  for (size_t d = 0; d < dot_count; ++d) {
    drawBeyondRingDot(dots[d].x, dots[d].y);
  }

  sortDrawItemsFarFirst(items, draw_count);
  for (size_t d = 0; d < draw_count; ++d) {
    const size_t i = items[d].index;
    const int x = items[d].x;
    const int y = items[d].y;
    drawSpeedVector(x, y, planes[i].nose_deg, planes[i].track_deg,
                    planes[i].gs_knots, radar::kColorTrackVector);
    drawHeadingTriangle(x, y, planes[i].nose_deg, radar::kColorAircraft);
  }
  static radar::TagRect placed_tags[services::adsb::kMaxAircraft];
  size_t placed_tag_count = 0;
  // items[] is far-first; walk it backwards so the closest traffic — the most
  // relevant — claims its tag space before anything further out.
  for (size_t d = draw_count; d > 0; --d) {
    const AircraftDrawItem& item = items[d - 1];
    const services::adsb::Aircraft& plane = planes[item.index];

    constexpr TagDetail kDetailLadder[] = {
        TagDetail::kFull, TagDetail::kCompact, TagDetail::kMinimal};
    TagLine lines[kMaxTagLines];
    size_t line_count = 0;
    TagPlacement placement;
    for (const TagDetail detail : kDetailLadder) {
      line_count = buildTagLines(plane, detail, lines);
      placement = findTagPlacement(lines, line_count, item.x, item.y, detail,
                                   items, draw_count, placed_tags,
                                   placed_tag_count);
      if (placement.valid) {
        break;
      }
    }
    if (!placement.valid) {
      continue;
    }

    drawAircraftTag(placement.rect.left, placement.rect.top, lines, line_count);
    placed_tags[placed_tag_count++] = placement.rect;
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

/** Sweep bearing in radians, clockwise from north. */
float sweepAngleRad(unsigned long now_ms) {
  constexpr float kTwoPi = 6.28318530718f;
  return static_cast<float>(radar::sweepPhaseMs(now_ms)) /
         static_cast<float>(radar::kSweepPeriodMs) * kTwoPi;
}

/** Sweep reaches the panel rim, not just the outermost range ring. */
constexpr int kSweepRadius = radar::kCenterX;

void sweepPoint(float angle_rad, int radius, int* x, int* y) {
  *x = radar::kCenterX +
       static_cast<int>(lroundf(sinf(angle_rad) * static_cast<float>(radius)));
  *y = radar::kCenterY -
       static_cast<int>(lroundf(cosf(angle_rad) * static_cast<float>(radius)));
}

/**
 * The decaying trail, drawn as a fan of thin triangles under the graticule so
 * the rings, runways and traffic all stay legible on top of it.
 */
void drawSweepGlow(unsigned long now_ms, bool enabled) {
  if (!enabled || !radar::showSweep()) {
    return;
  }

  constexpr float kTrailRad = 1.22173048f;  // 70°
  constexpr float kStepRad = kTrailRad / static_cast<float>(kSweepSteps);
  const float lead = sweepAngleRad(now_ms);

  // +1 px of radius: adjacent triangles share rounded vertices, and the extra
  // overlap keeps seams from showing as background-coloured hairlines.
  for (size_t i = 0; i < kSweepSteps; ++i) {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    sweepPoint(lead - static_cast<float>(i) * kStepRad, kSweepRadius + 1, &x0,
               &y0);
    sweepPoint(lead - static_cast<float>(i + 1) * kStepRad, kSweepRadius + 1,
               &x1, &y1);
    s_draw->fillTriangle(radar::kCenterX, radar::kCenterY, x0, y0, x1, y1,
                         s_sweep_ramp[i]);
  }
}

/** The leading edge, drawn over the grid so the scan line stays crisp. */
void drawSweepLine(unsigned long now_ms, bool enabled) {
  if (!enabled || !radar::showSweep()) {
    return;
  }
  int x = 0;
  int y = 0;
  sweepPoint(sweepAngleRad(now_ms), kSweepRadius, &x, &y);
  s_draw->drawWideLine(radar::kCenterX, radar::kCenterY, x, y, 1.0f,
                       s_sweep_edge);
}

template <typename Gfx>
void drawStaticGrid(Gfx& gfx, unsigned long now_ms, bool with_sweep) {
  initLabelMetrics();
  const DrawScope scope(gfx);
  displayFontEnsureLoaded(gfx);
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int grid_r = radar::kGridOuterRadius;

  gfx.fillScreen(radar::kColorBackground);
  initPalette();
  drawSweepGlow(now_ms, with_sweep);
  drawRings(cx, cy, grid_r);
  drawCrosshairs(cx, cy, grid_r, radar::kColorGrid);
  runway::drawAirportRunways(gfx);
  drawSweepLine(now_ms, with_sweep);
  drawCenterDot(cx, cy);
  drawCardinalLabels();
  drawScaleLabel(cx, cy, grid_r);
  gfx.setTextDatum(textdatum_t::top_left);
}

bool ensureFrameSprite() {
  if (s_frame_ready) {
    return true;
  }
  // Retrying a failed 115 KB allocation every frame would only thrash the heap.
  if (s_frame_attempted) {
    return false;
  }
  s_frame_attempted = true;
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
void renderFrame(unsigned long now_ms) {
  drawStaticGrid(s_frame, now_ms, true);  // opens its own DrawScope(s_frame)
  {
    const DrawScope scope(s_frame);
    drawAircraft(now_ms);
  }
  s_frame.pushSprite(0, 0);
  tft.setTextDatum(textdatum_t::top_left);
}

}  // namespace

void radarDisplayDraw() {
  initPalette();
  initLabelMetrics();
  const unsigned long now_ms = millis();

  if (ensureFrameSprite()) {
    renderFrame(now_ms);
    return;
  }

  // Fallback when the sprite can't be allocated: draw straight to the panel.
  // Visibly flickers, so the sweep is left off this path.
  const DrawScope scope(tft);
  drawStaticGrid(tft, now_ms, false);
  drawAircraft(now_ms);
  tft.setTextDatum(textdatum_t::top_left);
}

void radarDisplayRefreshAircraft() {
  initPalette();

  if (ensureFrameSprite()) {
    renderFrame(millis());
    return;
  }

  radarDisplayDraw();
}

bool radarDisplayCanAnimate() { return s_frame_ready; }

}  // namespace ui
