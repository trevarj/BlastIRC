#pragma once

#include "render/core/mat3.h"
#include "render/scene/node.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace input_caret {
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

using Corners = std::array<Vec2, 4>;

inline Corners corners(LayoutRect rect) noexcept {
  const float right = rect.x + rect.width, bottom = rect.y + rect.height;
  return {{{rect.x, rect.y}, {right, rect.y}, {rect.x, bottom}, {right, bottom}}};
}

inline bool advance(Corners& current, LayoutRect target, float elapsedMs, float scale) noexcept {
  const auto end = corners(target);
  const float centerX = target.x + target.width * 0.5F;
  const float centerY = target.y + target.height * 0.5F;
  Corners delta;
  std::array<float, 4> alignment;
  for (std::size_t i = 0; i < end.size(); ++i) {
    const auto& p = end[i];
    const Vec2 d{p.x - current[i].x, p.y - current[i].y};
    const float length = std::sqrt(d.x * d.x + d.y * d.y);
    delta[i] = d;
    alignment[i] = length > 0.0F ? (d.x * (p.x - centerX) + d.y * (p.y - centerY)) / length : 0.0F;
  }
  const auto [low, high] = std::minmax_element(alignment.begin(), alignment.end());
  const float spread = *high - *low;
  const float limit = 0.5F / scale;
  bool moving = false;
  for (std::size_t i = 0; i < end.size(); ++i) {
    const float direction = spread > 0.0F ? (alignment[i] - *low) / spread : 0.0F;
    const float decayMs = std::lerp(400.0F, 100.0F, direction);
    const float remaining = std::exp2(-10.0F * elapsedMs / decayMs);
    const Vec2 gap{delta[i].x * remaining, delta[i].y * remaining};
    current[i] = {end[i].x - gap.x, end[i].y - gap.y};
    moving |= std::abs(gap.x) >= limit || std::abs(gap.y) >= limit;
  }
  if (!moving) current = end;
  return moving;
}

inline float easeInOut(float x) noexcept {
  float t = x;
  for (int i = 0; i < 4; ++i) {
    const float dx = (0.52F * t - 0.78F) * t + 1.26F;
    const float slope = (1.56F * t - 1.56F) * t + 1.26F;
    t -= (t * dx - x) / slope;
  }
  return t * t * (3.0F - 2.0F * t);
}
} // namespace input_caret
