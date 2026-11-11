#include "presentation.hpp"
#include "../model.hpp"
#include "../theme/theme.hpp"
#include <algorithm>
#include <cmath>
#include <render/core/renderer.h>

namespace blast {
const PresentationMetrics &presentationMetrics() {
    static constexpr PresentationMetrics metrics{15, 16, 8, 22, 10, 4, .80f, 32, false};
    return metrics;
}
std::uint32_t minimumWindowHeight(Renderer &renderer) {
    // Keep the established 480px at 1x/default fonts. Compare against that fixed
    // 15px native font, not today's message size, so larger fonts raise the floor.
    static const TextFont baselineFont{.family = "sans-serif", .features = {}};
    const float baseline = renderer.fontRowExtent(15.f, baselineFont);
    const float scale = themeScale();
    const float magnification =
        std::max({1.f, scale,
                  renderer.fontRowExtent(15.f * themeMessageScale(), themeMessageFont()) / baseline,
                  renderer.fontRowExtent(15.f * scale, themeUiFont()) / baseline});
    return static_cast<std::uint32_t>(std::ceil(480.f * magnification));
}
float typingIndicatorHeight(Renderer &renderer, float scale, float messageSize) {
    return std::max(32 * scale,
                    renderer.fontRowExtent(messageSize, themeMessageFont()) + 7 * scale) +
           8 * scale;
}
MessageLayout messageLayout(const Settings &settings, const Buffer &buffer) {
    if (buffer.kind != "channel" && buffer.kind != "query")
        return MessageLayout::Irc;
    const auto found = settings.messageLayouts.find(buffer.id);
    return found == settings.messageLayouts.end() ? MessageLayout::Bubbles : found->second;
}
bool messageBubbles(const Settings &settings, const Buffer &buffer) {
    return messageLayout(settings, buffer) == MessageLayout::Bubbles;
}
float messageTextSize(const Settings &settings, const Buffer &buffer) {
    const float scale =
        std::isfinite(settings.messageScale) ? std::clamp(settings.messageScale, .7f, 2.f) : 1.f;
    return (messageBubbles(settings, buffer) ? 14.f : presentationMetrics().fontSize) * scale;
}
} // namespace blast
