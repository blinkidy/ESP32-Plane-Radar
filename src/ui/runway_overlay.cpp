#include "ui/runway_overlay.h"

#include <lgfx/v1/lgfx_fonts.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "data/airports.h"
#include "hardware/display_font.h"
#include "ui/radar_projection.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"

// Not `fonts`: LovyanGFX >=1.2.2x declares a global `namespace fonts` (and a
// `using namespace fonts;`) in lgfx_fonts.hpp, which a same-named alias here
// would redeclare. platformio.ini floats on ^1.2.7, so this must work on both.
namespace radar_fonts = lgfx::v1::fonts;

namespace ui::runway {
namespace {

constexpr size_t kMaxRunwayLabels = 24;
/** Per-airport working set; airports with more runways just draw the longest. */
constexpr size_t kMaxRunwaysPerAirport = 10;

struct ScreenLabel {
  char text[6];
  int16_t x;
  int16_t y;
};

ScreenLabel s_runway_labels[kMaxRunwayLabels];
size_t s_runway_label_count = 0;

bool s_label_style_ready = false;
bool s_label_use_vlw = false;
float s_ident_vlw_size = 0.26f;
const lgfx::GFXfont* s_ident_gfx = &radar_fonts::FreeSansBold9pt7b;

int measureVlwHeight(lgfx::LGFXBase& gfx, float size) {
  gfx.setTextSize(size);
  return gfx.fontHeight();
}

float findVlwSizeForHeight(lgfx::LGFXBase& gfx, int target_px) {
  float lo = 0.15f;
  float hi = 1.2f;
  for (int i = 0; i < 14; ++i) {
    const float mid = (lo + hi) * 0.5f;
    if (measureVlwHeight(gfx, mid) < target_px) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return hi;
}

void initLabelStyle(lgfx::LGFXBase& gfx) {
  if (s_label_style_ready) {
    return;
  }

  if (displayFontIsSmooth()) {
    s_label_use_vlw = true;
    s_ident_vlw_size =
        findVlwSizeForHeight(gfx, radar::kRunwayIdentLabelHeightPx);
  } else {
    s_ident_gfx = &radar_fonts::FreeSansBold9pt7b;
    s_label_use_vlw = false;
  }
  s_label_style_ready = true;
}

void applyIdentStyle(lgfx::LGFXBase& gfx) {
  if (s_label_use_vlw) {
    displayFontSetSmoothSize(gfx, s_ident_vlw_size);
  } else {
    displayFontSetBitmap(gfx, s_ident_gfx);
    gfx.setTextSize(0.75f);
  }
}

float e7ToDeg(int32_t e7) { return static_cast<float>(e7) * 1e-7f; }

int distSqFromCenter(int x, int y) {
  const int dx = x - radar::kCenterX;
  const int dy = y - radar::kCenterY;
  return dx * dx + dy * dy;
}

bool insideOuterRing(int x, int y) {
  return distSqFromCenter(x, y) <=
         radar::kGridOuterRadius * radar::kGridOuterRadius;
}

void clipPointToOuterRing(int x0, int y0, int* x1, int* y1) {
  if (insideOuterRing(*x1, *y1)) {
    return;
  }

  const int dx = *x1 - x0;
  const int dy = *y1 - y0;
  float t = 1.0f;
  for (int step = 0; step < 20; ++step) {
    const int px = x0 + static_cast<int>(lroundf(dx * t));
    const int py = y0 + static_cast<int>(lroundf(dy * t));
    if (insideOuterRing(px, py)) {
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

/** Leading digits of a designator: "12L" → 12, "04" → 4. -1 when absent. */
int designatorNumber(const char* ident) {
  int value = 0;
  bool any = false;
  for (const char* p = ident; *p >= '0' && *p <= '9'; ++p) {
    value = value * 10 + (*p - '0');
    any = true;
  }
  return any ? value : -1;
}

void pushLabel(ScreenLabel* dst, size_t* count, size_t cap, const char* text,
               int x, int y) {
  if (*count >= cap || text == nullptr || text[0] == '\0') {
    return;
  }
  ScreenLabel& label = dst[*count];
  strncpy(label.text, text, sizeof(label.text) - 1);
  label.text[sizeof(label.text) - 1] = '\0';
  label.x = static_cast<int16_t>(x);
  label.y = static_cast<int16_t>(y);
  ++*count;
}

struct DrawnRunway {
  uint16_t runway_idx;
  int x0;
  int y0;
  int x1;
  int y1;
  bool le_inside;
  bool he_inside;
  int len_px;
};

/**
 * Runway designators for one airport.
 *
 * Parallel runways (KIWA's 12L/12C/12R) share an orientation, so they are
 * grouped by designator number and only the longest of each group is labelled —
 * three stacked tags on ~5 px of separation would be unreadable. A grouped
 * label drops the L/C/R suffix because it stands for the whole group.
 */
void collectRunwayIdentLabels(const DrawnRunway* drawn, size_t drawn_count) {
  bool handled[kMaxRunwaysPerAirport] = {};

  for (size_t i = 0; i < drawn_count; ++i) {
    if (handled[i]) {
      continue;
    }
    const auto& rw = data::airports::kRunways[drawn[i].runway_idx];
    const int le_num = designatorNumber(rw.le_ident);

    size_t group_size = 0;
    for (size_t j = i; j < drawn_count; ++j) {
      const auto& other = data::airports::kRunways[drawn[j].runway_idx];
      if (designatorNumber(other.le_ident) == le_num) {
        handled[j] = true;
        ++group_size;
      }
    }

    // drawn[] is longest-first, so drawn[i] is the pick for this orientation.
    if (drawn[i].len_px < radar::kRunwayIdentMinRunwayPx) {
      continue;
    }

    char le_text[6];
    char he_text[6];
    if (group_size > 1) {
      snprintf(le_text, sizeof(le_text), "%02d", le_num);
      snprintf(he_text, sizeof(he_text), "%02d",
               designatorNumber(rw.he_ident));
    } else {
      strncpy(le_text, rw.le_ident, sizeof(le_text) - 1);
      le_text[sizeof(le_text) - 1] = '\0';
      strncpy(he_text, rw.he_ident, sizeof(he_text) - 1);
      he_text[sizeof(he_text) - 1] = '\0';
    }

    const float len = static_cast<float>(drawn[i].len_px);
    const float ux = static_cast<float>(drawn[i].x1 - drawn[i].x0) / len;
    const float uy = static_cast<float>(drawn[i].y1 - drawn[i].y0) / len;
    const float off = static_cast<float>(radar::kRunwayIdentLabelHeightPx) *
                          0.5f +
                      static_cast<float>(radar::kRunwayIdentGapPx);

    if (drawn[i].le_inside) {
      pushLabel(s_runway_labels, &s_runway_label_count, kMaxRunwayLabels,
                le_text,
                drawn[i].x0 - static_cast<int>(lroundf(ux * off)),
                drawn[i].y0 - static_cast<int>(lroundf(uy * off)));
    }
    if (drawn[i].he_inside) {
      pushLabel(s_runway_labels, &s_runway_label_count, kMaxRunwayLabels,
                he_text,
                drawn[i].x1 + static_cast<int>(lroundf(ux * off)),
                drawn[i].y1 + static_cast<int>(lroundf(uy * off)));
    }
  }
}

/** Draw one airport's runway lines; returns true if anything landed on screen. */
bool drawOneAirport(lgfx::LGFXBase& gfx, size_t first, size_t last) {
  DrawnRunway drawn[kMaxRunwaysPerAirport];
  size_t drawn_count = 0;

  for (size_t i = first; i < last; ++i) {
    const auto& rw = data::airports::kRunways[i];

    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    radar::latLonToScreen(e7ToDeg(rw.le_lat_e7), e7ToDeg(rw.le_lon_e7), &x0,
                          &y0);
    radar::latLonToScreen(e7ToDeg(rw.he_lat_e7), e7ToDeg(rw.he_lon_e7), &x1,
                          &y1);
    if (!segmentIntersectsDisc(x0, y0, x1, y1)) {
      continue;
    }

    const bool le_inside = insideOuterRing(x0, y0);
    const bool he_inside = insideOuterRing(x1, y1);
    clipPointToOuterRing(x0, y0, &x1, &y1);
    clipPointToOuterRing(x1, y1, &x0, &y0);

    gfx.drawWideLine(x0, y0, x1, y1, radar::kRunwayLineHalfWidth,
                     radar::kColorRunway);

    if (drawn_count < kMaxRunwaysPerAirport) {
      const int dx = x1 - x0;
      const int dy = y1 - y0;
      DrawnRunway& item = drawn[drawn_count++];
      item.runway_idx = static_cast<uint16_t>(i);
      item.x0 = x0;
      item.y0 = y0;
      item.x1 = x1;
      item.y1 = y1;
      item.le_inside = le_inside;
      item.he_inside = he_inside;
      item.len_px =
          static_cast<int>(lroundf(sqrtf(static_cast<float>(dx * dx + dy * dy))));
    }
  }

  if (drawn_count == 0) {
    return false;
  }
  collectRunwayIdentLabels(drawn, drawn_count);
  return true;
}

void drawIdentLabel(lgfx::LGFXBase& gfx, const ScreenLabel& label) {
  const int tw = gfx.textWidth(label.text);
  const int th = gfx.fontHeight();
  gfx.setTextDatum(textdatum_t::middle_center);
  gfx.fillRect(label.x - tw / 2 - 1, label.y - th / 2, tw + 2, th,
               radar::kColorBackground);
  gfx.setTextColor(radar::kColorRunwayIdent, radar::kColorBackground);
  gfx.drawString(label.text, label.x, label.y);
}

}  // namespace

void drawAirportRunways(lgfx::LGFXBase& gfx) {
  if (!radar::showRunways()) {
    return;
  }
  displayFontEnsureLoaded(gfx);
  radar::projectionSync();

  s_runway_label_count = 0;

  const float radius_km = radar::fetchRadiusKm();
  const float radius_sq = radius_km * radius_km;

  // kRunways is grouped by airport, so one range test skips a whole airport.
  size_t i = 0;
  while (i < data::airports::kRunwayCount) {
    const uint16_t ap_idx = data::airports::kRunways[i].airport_idx;
    size_t end = i;
    while (end < data::airports::kRunwayCount &&
           data::airports::kRunways[end].airport_idx == ap_idx) {
      ++end;
    }

    const auto& ap = data::airports::kAirports[ap_idx];
    if (radar::distSqKmFromCenter(e7ToDeg(ap.lat_e7), e7ToDeg(ap.lon_e7)) <=
        radius_sq) {
      drawOneAirport(gfx, i, end);
    }
    i = end;
  }

  if (s_runway_label_count == 0) {
    return;
  }

  initLabelStyle(gfx);

  applyIdentStyle(gfx);
  for (size_t n = 0; n < s_runway_label_count; ++n) {
    drawIdentLabel(gfx, s_runway_labels[n]);
  }
}

}  // namespace ui::runway
