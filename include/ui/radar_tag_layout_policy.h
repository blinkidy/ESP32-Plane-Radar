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

constexpr int largerMagnitude(int a, int b) {
  const int abs_a = a < 0 ? -a : a;
  const int abs_b = b < 0 ? -b : b;
  return abs_a > abs_b ? abs_a : abs_b;
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
  const int dx = largerMagnitude(rect.left - cx, rect.right - 1 - cx);
  const int dy = largerMagnitude(rect.top - cy, rect.bottom - 1 - cy);
  return dx * dx + dy * dy <= radius * radius;
}

}  // namespace ui::radar
