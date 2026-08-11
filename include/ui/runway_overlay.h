#pragma once

#include <LovyanGFX.hpp>

namespace ui::runway {

void drawLargeAirportRunways(lgfx::LGFXBase& gfx);

/** Number of visible runway lines crossing a proposed aircraft-tag rectangle. */
int tagRectanglePenalty(int left, int top, int right, int bottom);

}  // namespace ui::runway
