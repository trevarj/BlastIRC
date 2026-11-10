#pragma once
#include "../model.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace blast::fx {
inline float cubicOut(float t) {
    t = 1 - std::clamp(t, 0.f, 1.f);
    return 1 - t * t * t;
}
inline float backOut(float t) {
    t = std::clamp(t, 0.f, 1.f) - 1;
    return 1 + 2.70158f * t * t * t + 1.70158f * t * t;
}
inline float bounceOut(float t) {
    t = std::clamp(t, 0.f, 1.f);
    if (t < 1 / 2.75f)
        return 7.5625f * t * t;
    if (t < 2 / 2.75f) {
        t -= 1.5f / 2.75f;
        return 7.5625f * t * t + .75f;
    }
    if (t < 2.5f / 2.75f) {
        t -= 2.25f / 2.75f;
        return 7.5625f * t * t + .9375f;
    }
    t -= 2.625f / 2.75f;
    return 7.5625f * t * t + .984375f;
}
inline float smooth(float t) {
    t = std::clamp(t, 0.f, 1.f);
    return t * t * (3 - 2 * t);
}
inline float veil(float age) {
    if (age < 0)
        return 0;
    if (age < .3f)
        return smooth(age / .3f);
    if (age < 1.3f)
        return 1;
    return 1 - smooth((age - 1.3f) / 2.f);
}
struct MessageEntrance {
    float x = 0, y = 0, reveal = 1, scale = 1;
    bool active = false;
};
// Independent native transforms and reveal clocks; a row stays live until both settle.
inline constexpr double outgoingDuration = .70, incomingDuration = .55, sendRevealDuration = .45;
inline double messageMotionDuration(SendMotion motion) {
    switch (motion) {
    case SendMotion::Flight:
        return outgoingDuration;
    case SendMotion::Rise:
        return .38;
    case SendMotion::Pop:
        return .32;
    case SendMotion::Slide:
        return .55;
    case SendMotion::Drift:
        return .70;
    case SendMotion::None:
        return 0;
    }
    return 0;
}
inline double messageIncomingDuration(float milliseconds) {
    return std::isfinite(milliseconds) ? std::clamp(double(milliseconds), 100., 2000.) / 1000.
                                       : incomingDuration;
}
inline MessageEntrance messageEntrance(double age, bool own, float originX, float originY,
                                       float destinationX, float destinationY, bool enabled,
                                       SendMotion motion = SendMotion::Flight,
                                       SendReveal reveal = SendReveal::Pixels,
                                       SendMotion incomingMotion = SendMotion::None,
                                       SendReveal incomingReveal = SendReveal::Fade,
                                       float incomingDurationMs = 550.f) {
    if (!enabled || !std::isfinite(age) || age < 0)
        return {};
    if (!own) {
        motion = incomingMotion == SendMotion::Flight ? SendMotion::None : incomingMotion;
        reveal = incomingReveal;
    }
    const double duration = own ? 0 : messageIncomingDuration(incomingDurationMs);
    const double motionDuration =
        motion == SendMotion::None ? 0 : (own ? messageMotionDuration(motion) : duration);
    const double revealDuration =
        reveal == SendReveal::None ? 0 : (own ? sendRevealDuration : duration);
    MessageEntrance result;
    result.active = age < std::max(motionDuration, revealDuration);
    if (age < revealDuration)
        result.reveal = static_cast<float>(age / revealDuration);
    if (age < motionDuration) {
        const float t = static_cast<float>(age / motionDuration);
        switch (motion) {
        case SendMotion::Flight:
            // Bad composer geometry must not poison native transform matrices.
            if (std::isfinite(originX) && std::isfinite(originY) && std::isfinite(destinationX) &&
                std::isfinite(destinationY)) {
                const double progress = 1 - smooth(t);
                const double x = (double(originX) - destinationX) * progress;
                const double y = (double(originY) - destinationY) * progress;
                if (std::abs(x) <= double(std::numeric_limits<float>::max()) &&
                    std::abs(y) <= double(std::numeric_limits<float>::max())) {
                    result.x = static_cast<float>(x);
                    result.y = static_cast<float>(y);
                }
            }
            break;
        case SendMotion::Rise:
            result.y = 18 * (1 - cubicOut(t));
            break;
        case SendMotion::Pop:
            result.scale = .92f + .08f * backOut(t);
            break;
        case SendMotion::Slide:
            result.x = 24 * (1 - smooth(t));
            break;
        case SendMotion::Drift:
            result.x = 18 * (1 - smooth(t));
            result.y = 12 * (1 - cubicOut(t));
            break;
        case SendMotion::None:
            break;
        }
    }
    return result;
}
inline float messageBlockOpacity(float reveal, unsigned block) {
    // A fixed permutation, not per-frame randomness: blocks cannot flicker back off.
    const float rank = float((block * 37U) & 63U) / 64.f;
    return std::clamp((reveal * 1.2f - rank) / .2f, 0.f, 1.f);
}
} // namespace blast::fx
