#pragma once

#include <LovyanGFX.hpp>

namespace ui::runway {

void drawLargeAirportRunways(lgfx::LGFXBase& gfx);

/** True when a screen-space tag rectangle crosses a currently drawn runway. */
bool tagRectOverlapsRunway(int left, int top, int right, int bottom);

}  // namespace ui::runway
