#pragma once

namespace ui {

/** Draw the static sonar/radar grid (black disc, green overlay, labels). */
void radarDisplayDraw();

/** Redraw aircraft only (blits cached grid; no full-screen clear). */
void radarDisplayRefreshAircraft();

/**
 * True once the off-screen frame sprite exists.
 *
 * Animation requires it: drawing straight to the panel at frame rate flickers,
 * so the caller should hold at the poll rate when this is false.
 */
bool radarDisplayCanAnimate();

}  // namespace ui
