#pragma once

namespace ui {

/** Draw the static sonar/radar grid (black disc, green overlay, labels). */
void radarDisplayDraw();

/** Redraw aircraft only (blits cached grid; no full-screen clear). */
void radarDisplayRefreshAircraft();

/** True while a selected tag needs its aircraft-symbol pulse animated. */
bool radarDisplayNeedsAnimation();

}  // namespace ui
