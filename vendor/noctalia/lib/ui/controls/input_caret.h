#pragma once

#include "render/scene/node.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace input_caret {
inline float duration(float value) noexcept {
  return std::isfinite(value) ? std::clamp(value, 40.0F, 240.0F) : 120.0F;
}
inline float strength(float value) noexcept {
  return std::isfinite(value) ? std::clamp(value, 0.05F, 0.35F) : 0.20F;
}
inline bool equal(const LayoutRect& a, const LayoutRect& b) noexcept {
  return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

// Snap in scene/device coordinates, then intersect the editor's inward-snapped viewport.
inline LayoutRect clippedPixels(LayoutRect rect, LayoutRect viewport, float scale) noexcept {
  const float left = std::max(std::ceil(viewport.x * scale), std::round(rect.x * scale));
  const float top = std::max(std::ceil(viewport.y * scale), std::round(rect.y * scale));
  const float right = std::min(std::floor((viewport.x + viewport.width) * scale),
                               std::round((rect.x + rect.width) * scale));
  const float bottom = std::min(std::floor((viewport.y + viewport.height) * scale),
                                std::round((rect.y + rect.height) * scale));
  return {left / scale, top / scale, std::max(0.0F, right - left) / scale,
          std::max(0.0F, bottom - top) / scale};
}

// Six retained stamps and one finite native animation; no history grows with typing.
struct Trail {
  static constexpr std::size_t count = 6;
  LayoutRect previous{}, from{}, to{};
  bool anchored = false, active = false;

  void reset() noexcept { anchored = active = false; }
  bool move(LayoutRect rect, bool allowed) noexcept {
    if (!allowed) {
      reset();
      return false;
    }
    if (!anchored) {
      previous = rect;
      anchored = true;
      return false;
    }
    if (equal(previous, rect)) return false;
    from = previous;
    to = previous = rect;
    active = true;
    return true;
  }
  LayoutRect sample(float progress, std::size_t index) const noexcept {
    const float fraction = static_cast<float>(index) / static_cast<float>(count);
    const float t = std::clamp(progress, 0.0F, 1.0F) +
                    (1.0F - std::clamp(progress, 0.0F, 1.0F)) * fraction;
    return {from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t,
            from.width + (to.width - from.width) * t,
            from.height + (to.height - from.height) * t};
  }
  float opacity(float progress, std::size_t index, float amount) const noexcept {
    return active ? amount * (1.0F - std::clamp(progress, 0.0F, 1.0F)) *
                       (1.0F - static_cast<float>(index) / static_cast<float>(count)) : 0.0F;
  }
};
} // namespace input_caret
