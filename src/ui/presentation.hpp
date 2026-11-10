#pragma once
#include <cstdint>
#include <string_view>

class Renderer;

namespace blast {
struct Settings;
struct Buffer;
struct PresentationMetrics {
    float fontSize, padding, inset, headerHeight, rowGap, groupedGap, widthFraction, avatarSize;
    bool inlineNick;
};
struct MessageSurface {
    float x = 0, y = 0, width = 0, height = 0;
    std::string_view server, target, id;
    bool own = false, presence = false, hovered = false, highlight = false;
    float opacity = 1;
};
const PresentationMetrics &presentationMetrics();
// Native logical height, shared by the actual window and supported-size consumers.
std::uint32_t minimumWindowHeight(Renderer &);
// Reserved independently of live participants, using the native UI-font row.
float typingIndicatorHeight(Renderer &, float scale);
bool messageBubbles(const Settings &settings, const Buffer &buffer);
float messageTextSize(const Settings &settings, const Buffer &buffer);
} // namespace blast
