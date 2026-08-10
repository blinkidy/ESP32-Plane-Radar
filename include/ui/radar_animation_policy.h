/*
 * Sweep/animation timing. Adapted from upstream PR #73 (RockBase-Ronnie),
 * reduced to this fork's single board.
 */
#pragma once

#include <cstddef>

namespace ui::radar {

/** One full revolution of the sweep. Real PPI scopes run 4–12 s; 12 s reads calmly here. */
constexpr unsigned long kSweepPeriodMs = 12000UL;

/** ~10 fps: a full frame (composite + 115 KB SPI blit) costs well under this. */
constexpr unsigned long kAnimationIntervalMs = 100UL;

constexpr unsigned long sweepPhaseMs(unsigned long now_ms) {
  return now_ms % kSweepPeriodMs;
}

/** Nothing moves with the sweep off and no traffic — skip the redraw entirely. */
constexpr bool animationNeeded(bool sweep_enabled, size_t aircraft_count) {
  return sweep_enabled || aircraft_count > 0;
}

}  // namespace ui::radar
