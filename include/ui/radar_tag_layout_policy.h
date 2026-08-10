/*
 * Tag placement geometry. Adapted from upstream PR #73
 * (SPDX-FileCopyrightText: 2026 RockBase IoT (Chengdu) CO., LTD.,
 *  SPDX-License-Identifier: Apache-2.0).
 */
#pragma once

namespace ui::radar {

struct TagRect {
  int left;
  int top;
  int right;
  int bottom;
};

constexpr bool tagRectInside(TagRect rect, int width, int height) {
  return rect.left >= 0 && rect.top >= 0 && rect.right <= width &&
         rect.bottom <= height && rect.left < rect.right &&
         rect.top < rect.bottom;
}

constexpr bool tagRectsOverlap(TagRect first, TagRect second) {
  return first.left < second.right && first.right > second.left &&
         first.top < second.bottom && first.bottom > second.top;
}

// Single-expression bodies throughout: the firmware compiles as gnu++11 (see
// platformio.ini), where a constexpr body must be exactly one return statement.
constexpr int absInt(int v) { return v < 0 ? -v : v; }

constexpr int largerMagnitude(int a, int b) {
  return absInt(a) > absInt(b) ? absInt(a) : absInt(b);
}

constexpr bool withinRadius(int dx, int dy, int radius) {
  return dx * dx + dy * dy <= radius * radius;
}

/**
 * Whether every corner falls inside the panel's visible circle.
 *
 * The panel is round, so the 240x240 bounding square is not the visible area:
 * a tag out along a diagonal can sit entirely within the square yet run off the
 * bezel. Rejecting those here makes the placement search fall through to an
 * inward candidate that the viewer can actually read.
 *
 * right/bottom are exclusive, so the last drawn pixel is one inside each.
 */
constexpr bool tagRectInsideDisc(TagRect rect, int cx, int cy, int radius) {
  return withinRadius(largerMagnitude(rect.left - cx, rect.right - 1 - cx),
                      largerMagnitude(rect.top - cy, rect.bottom - 1 - cy),
                      radius);
}

}  // namespace ui::radar
