#pragma once

#include "render/core/color.h"
#include "ui/controls/input.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

// Private to input.cpp. Positions are viewport-local logical pixels; colors are premultiplied.
namespace input_caret_trail {
  using Style = Input::CaretTrailStyle;
  using Clock = std::chrono::steady_clock;
  using input_caret::Corners;

  inline Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
  inline Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
  inline Vec2 operator*(Vec2 v, float s) { return {v.x * s, v.y * s}; }
  inline Vec2 normal(Vec2 v) { return {-v.y, v.x}; }
  inline float length(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }
  inline Vec2 center(LayoutRect r) { return {r.x + 0.5F * r.width, r.y + 0.5F * r.height}; }

  inline Color mix(Color a, Color b, float t) {
    return {std::lerp(a.r, b.r, t), std::lerp(a.g, b.g, t), std::lerp(a.b, b.b, t), std::lerp(a.a, b.a, t)};
  }

  inline float noise(std::uint32_t seed) {
    seed ^= seed >> 16;
    seed *= 0x7feb352dU;
    seed ^= seed >> 15;
    seed *= 0x846ca68bU;
    seed ^= seed >> 16;
    return static_cast<float>(seed >> 8) * (1.0F / 16777216.0F);
  }

  struct Vertex {
    std::array<float, 4> xyUv;
    std::array<float, 4> color;
  };

  class Mesh {
  public:
    std::vector<Vertex> vertices;

    void blur(const Corners& previous, const Corners& current, Color color, float pixel, float elapsedMs) {
      // A short shutter sampled between successive animated corners, not a halo around the idle caret.
      float travel = 0.0F;
      for (std::size_t i = 0; i < current.size(); ++i)
        travel = std::max(travel, length(previous[i] - current[i]));
      if (!(travel > 0.0F))
        return;
      const float exposure = std::min({1.0F, 16.0F / std::max(elapsedMs, 1.0F), 13.0F / std::max(travel, pixel)});
      const int taps = static_cast<int>(std::clamp(1.0F + travel * exposure / pixel, 2.0F, 8.0F));
      vertices.reserve(8 * 30);
      for (int i = taps - 1; i >= 0; --i) {
        const float t = static_cast<float>(i) / static_cast<float>(taps - 1);
        Corners sample;
        Corners outer;
        for (std::size_t j = 0; j < sample.size(); ++j) {
          sample[j] = current[j] + (previous[j] - current[j]) * (exposure * t);
          const float feather = pixel * (0.6F + 0.65F * t);
          outer[j] = sample[j] + Vec2{(j % 2 ? 1.0F : -1.0F) * feather, (j > 1 ? 1.0F : -1.0F) * feather};
        }
        Color tint = color;
        tint.a *= (i == 0 ? 0.73F : 0.69F * (1.0F - 0.78F * t) / static_cast<float>(taps - 1));
        const auto a = vertex(sample[0], tint), b = vertex(sample[1], tint);
        const auto c = vertex(sample[2], tint), d = vertex(sample[3], tint);
        const auto oa = vertex(outer[0]), ob = vertex(outer[1]), oc = vertex(outer[2]), od = vertex(outer[3]);
        quad(a, b, c, d);
        quad(oa, ob, a, b);
        quad(oc, od, c, d);
        quad(oa, oc, a, c);
        quad(ob, od, b, d);
      }
    }

    void ribbon(Vec2 a, Vec2 b, Vec2 ra, Vec2 rb, Color ca, Color cb) {
      const auto head = vertex(a, ca), tail = vertex(b, cb);
      quad(vertex(a - ra), vertex(b - rb), head, tail);
      quad(head, tail, vertex(a + ra), vertex(b + rb));
    }

    void spark(Vec2 p, Vec2 x, Vec2 y, Color color) {
      quad(
          vertex(p - x - y, color, {-1, -1}), vertex(p + x - y, color, {1, -1}), vertex(p - x + y, color, {-1, 1}),
          vertex(p + x + y, color, {1, 1})
      );
    }

  private:
    static Vertex vertex(Vec2 p, Color c = {0, 0, 0, 0}, Vec2 uv = {}) {
      return {{p.x, p.y, uv.x, uv.y}, {c.r * c.a, c.g * c.a, c.b * c.a, c.a}};
    }

    void quad(const Vertex& a, const Vertex& b, const Vertex& c, const Vertex& d) {
      vertices.push_back(a);
      vertices.push_back(b);
      vertices.push_back(c);
      vertices.push_back(c);
      vertices.push_back(b);
      vertices.push_back(d);
    }
  };

  struct Burst {
    Vec2 head, direction, side;
    float distance = 0.0F, em = 0.0F;
    Clock::time_point born;
    std::uint32_t seed = 0;

    Vec2 at(float t, float lateral = 0.0F, float drift = 0.0F) const {
      return head - direction * (distance * t + drift) + side * lateral;
    }
  };

  inline void blaze(Mesh& mesh, const Burst& b, float age, float pixel) {
    const float fade = (1.0F - age) * (1.0F - age);
    const float phase = 6.0F * noise(b.seed) - 7.0F * age;
    constexpr int steps = 9;
    Vec2 previous, priorRim;
    Color priorHeat;
    for (int i = 0; i <= steps; ++i) {
      const float t = static_cast<float>(i) / steps;
      const float curl = std::sin(11.0F * t + phase);
      const auto p = b.at(t, b.em * 0.19F * curl * t, b.em * age * 0.31F) + Vec2{0, -b.em * (0.20F + age * 0.72F) * t};
      const float width = b.em * (0.20F + curl * 0.075F) * std::sin(std::numbers::pi_v<float> * t) + pixel;
      const auto rim = b.side * width;
      const Color heat{1.0F, std::lerp(0.55F, 0.13F, t), 0.025F, fade * (1.0F - t) * 0.84F};
      if (i > 0) {
        mesh.ribbon(previous, p, priorRim, rim, priorHeat, heat);
        const auto core = [](Color c) { return mix(c, {1.0F, 0.96F, 0.35F, c.a}, 0.62F); };
        mesh.ribbon(previous, p, priorRim * 0.45F, rim * 0.45F, core(priorHeat), core(heat));
      }
      previous = p;
      priorRim = rim;
      priorHeat = heat;
    }
    for (std::uint32_t i = 0; i < 10; ++i) {
      const auto seed = b.seed + 7U * i;
      const float t = noise(seed);
      const auto p = b.at(t, b.em * (noise(seed + 1) - 0.5F) * (0.35F + age), b.em * age * 0.7F)
          + Vec2{0, -b.em * (0.08F + age) * (0.25F + noise(seed + 2))};
      const float r = std::max(0.8F * pixel, b.em * 0.085F * (1.0F - 0.5F * t));
      mesh.spark(p, {r, 0}, {0, r * (1.3F + age)}, {1.0F, 0.52F + 0.38F * t, 0.07F, fade});
    }
  }

  inline void lightning(Mesh& mesh, const Burst& b, float age, float pixel) {
    const int steps = std::clamp(static_cast<int>(b.distance / (0.34F * b.em)), 4, 13);
    const auto epoch = static_cast<std::uint32_t>(age * 6.0F);
    const auto seed = b.seed + 37U * epoch;
    const float flash = (1.0F - age) * (0.72F + 0.28F * noise(seed + 1));
    const float core = std::max(0.85F * pixel, 0.061F * b.em);
    const auto arc = [&](Vec2 a, Vec2 z, Vec2 side, float wa, float wz) {
      mesh.ribbon(
          a, z, side * (wa * core * 3.4F), side * (wz * core * 3.4F), {0.14F, 0.52F, 1.0F, wa * flash * 0.74F},
          {0.14F, 0.52F, 1.0F, wz * flash * 0.74F}
      );
      mesh.ribbon(
          a, z, side * (wa * core), side * (wz * core), {0.83F, 0.96F, 1.0F, wa * flash},
          {0.83F, 0.96F, 1.0F, wz * flash}
      );
    };
    Vec2 previous = b.head;
    float priorWidth = 1.0F;
    for (int i = 1; i <= steps; ++i) {
      const float t = static_cast<float>(i) / static_cast<float>(steps);
      const auto segment = seed + static_cast<std::uint32_t>(i);
      const float jag = i == steps ? 0.0F : (i % 2 ? -1.0F : 1.0F) * (0.25F + noise(segment));
      const auto p = b.at(t, jag * b.em * 0.18F);
      const float width = 1.0F - 0.77F * t;
      arc(previous, p, b.side, priorWidth, width);
      if (i == steps / 3 || i == 2 * steps / 3) {
        const float reach = (i == steps / 3 ? 1.0F : -1.0F) * b.em * (0.32F + 0.22F * noise(segment + 19U));
        const Vec2 elbow = p + b.side * reach * 0.56F - b.direction * b.em * 0.20F;
        const Vec2 tip = p + b.side * reach - b.direction * b.em * (0.38F + 0.32F * noise(segment + 37U));
        const auto fork = tip - p;
        const auto side = normal(fork) * (1.0F / length(fork));
        arc(p, elbow, side, width * 0.74F, width * 0.53F);
        arc(elbow, tip, side, width * 0.53F, 0.0F);
      }
      previous = p;
      priorWidth = width;
    }
  }

  inline void railgun(Mesh& mesh, const Burst& b, float age, float pixel) {
    constexpr float tau = 2.0F * std::numbers::pi_v<float>;
    const float turns = std::clamp(b.distance / (2.3F * b.em), 0.85F, 3.0F);
    const int count = std::clamp(static_cast<int>(b.distance / (0.19F * b.em)), 9, 25);
    const float phase = tau * (noise(b.seed) + age * 0.9F);
    for (int i = 0; i < count; ++i) {
      const float t = static_cast<float>(i) / static_cast<float>(count - 1);
      const float angle = phase + tau * turns * t;
      const float sine = std::sin(angle), cosine = std::cos(angle);
      for (float side : {-1.0F, 1.0F}) {
        const float depth = 0.5F + 0.5F * side * cosine;
        const auto p =
            b.at(t, side * sine * b.em * (0.14F + 0.30F * t) * (1.0F + 0.42F * age), b.em * (0.16F + age * 0.87F) * t);
        const float r = std::max(pixel, b.em * std::lerp(0.074F, 0.16F, depth)) * (1.0F - age * 0.36F);
        const float a = (1.0F - age) * std::lerp(0.32F, 0.88F, depth) * (1.0F - 0.44F * t);
        const auto dye = side > 0.0F ? Color{0.51F, 0.90F, 1.0F, a} : Color{0.90F, 0.48F, 1.0F, a};
        mesh.spark(p, {r, 0}, {0, r}, dye);
      }
    }
  }

  inline void torpedo(Mesh& mesh, const Burst& b, float age, float pixel) {
    const float fade = (1.0F - age) * (1.0F - age);
    const auto nose = b.at(0.0F, 0.0F, b.em * age * 0.6F);
    const auto shoulder = b.at(0.18F, 0.0F, b.em * age * 0.98F);
    const auto tail = b.at(1.0F, 0.0F, b.em * (0.2F + age * 1.65F));
    const auto broad = b.side * (b.em * (0.24F + age * 0.24F));
    const auto narrow = b.side * pixel;
    const Color blue{0.24F, 0.86F, 1.0F, fade * 0.64F};
    mesh.ribbon(nose, shoulder, narrow, broad, {0.69F, 1.0F, 0.93F, fade * 0.85F}, blue);
    mesh.ribbon(shoulder, tail, broad, {}, blue, {0.11F, 0.43F, 0.93F, 0.0F});
    for (std::uint32_t i = 0; i < 20; ++i) {
      const auto seed = b.seed + 5U * i;
      const float t = (static_cast<float>(i) + noise(seed)) / 20.0F;
      const float taper = 1.0F - t;
      const auto p = b.at(
          t, (noise(seed + 1) - 0.5F) * b.em * taper * (0.53F + age * 0.48F), b.em * age * (0.6F + noise(seed + 2))
      );
      const float r = std::max(pixel, b.em * (0.058F + 0.052F * taper));
      mesh.spark(
          p, b.direction * (r * (1.45F + taper + age)), b.side * (r * 0.7F), {0.42F, 0.97F, 0.93F, fade * taper}
      );
    }
  }

  inline void pixiedust(Mesh& mesh, const Burst& b, float age, float pixel) {
    for (std::uint32_t i = 0; i < 23; ++i) {
      const auto seed = b.seed + 11U * i;
      const float life = 0.47F + 0.53F * noise(seed + 1);
      if (age >= life)
        continue;
      const float t = (static_cast<float>(i) + noise(seed)) / 23.0F;
      const auto p = b.at(
                         t, (noise(seed + 2) - 0.5F) * b.em * (0.65F + age * 1.06F),
                         (noise(seed + 3) - 0.24F) * b.em * (0.30F + age * 0.78F)
                     )
          + Vec2{0, -b.em * age * 0.32F};
      const float twinkle = 0.5F + 0.5F * std::sin(age * (23.0F + noise(seed + 4) * 21.0F) + 6.28F * noise(seed + 5));
      const float a = (1.0F - age / life) * (0.25F + 0.75F * twinkle * twinkle);
      const float r = std::max(0.85F * pixel, b.em * (0.047F + noise(seed + 6) * 0.10F));
      const auto dye = mix({1.0F, 0.64F, 0.92F, a}, {1.0F, 0.98F, 0.68F, a}, noise(seed + 7));
      mesh.spark(p, {r, 0}, {0, r}, dye);
      if (i % 4 == 0 && twinkle > 0.58F) {
        const float arm = r * (1.5F + twinkle);
        const Color white{1.0F, 0.98F, 0.87F, a * 0.65F};
        mesh.spark(p, {arm, 0}, {0, pixel}, white);
        mesh.spark(p, {pixel, 0}, {0, arm}, white);
      }
    }
  }

  class Effects {
  public:
    void reset() {
      m_count = 0;
      mesh.vertices.clear();
    }

    void emit(LayoutRect old, LayoutRect target, float em, float pixel, Clock::time_point now) {
      const Vec2 head = center(target);
      const Vec2 delta = head - center(old);
      const float distance = length(delta);
      if (!std::isfinite(distance) || distance < 0.75F * pixel)
        return;
      if (m_count == m_bursts.size()) {
        std::move(m_bursts.begin() + 1, m_bursts.end(), m_bursts.begin());
        --m_count;
      }
      const auto direction = delta * (1.0F / distance);
      em = std::clamp(std::max({em, old.height, target.height}), 9.0F, 48.0F);
      m_bursts[m_count++] = {head, direction, normal(direction),     std::min(distance, em * 24.0F),
                             em,   now,       ++m_seed * 0x9e3779b9U};
    }

    void sample(Style style, Clock::time_point now, float pixel) {
      using Draw = void (*)(Mesh&, const Burst&, float, float);
      Draw draw;
      float lifetimeMs;
      std::size_t vertexBudget;
      switch (style) {
      case Style::Blaze:
        draw = blaze;
        lifetimeMs = 310.0F;
        vertexBudget = 276;
        break;
      case Style::Lightning:
        draw = lightning;
        lifetimeMs = 155.0F;
        vertexBudget = 408;
        break;
      case Style::Railgun:
        draw = railgun;
        lifetimeMs = 365.0F;
        vertexBudget = 300;
        break;
      case Style::Torpedo:
        draw = torpedo;
        lifetimeMs = 315.0F;
        vertexBudget = 144;
        break;
      case Style::Pixiedust:
        draw = pixiedust;
        lifetimeMs = 520.0F;
        vertexBudget = 210;
        break;
      default:
        return;
      }
      mesh.vertices.clear();
      if (m_count != 0)
        mesh.vertices.reserve(m_bursts.size() * vertexBudget);
      std::size_t alive = 0;
      for (std::size_t i = 0; i < m_count; ++i) {
        const auto& burst = m_bursts[i];
        const float age = std::chrono::duration<float, std::milli>(now - burst.born).count() / lifetimeMs;
        if (age >= 1.0F)
          continue;
        draw(mesh, burst, age, pixel);
        if (alive != i)
          m_bursts[alive] = burst;
        ++alive;
      }
      m_count = alive;
    }

    Mesh mesh;

  private:
    std::array<Burst, 4> m_bursts{};
    std::size_t m_count = 0;
    std::uint32_t m_seed = 0;
  };
} // namespace input_caret_trail
