#pragma once
#include <cstdint>

class Renderer;

namespace blast {
struct Settings;
struct Buffer;
enum class MessageLayout;
struct PresentationMetrics {
    float fontSize, padding, inset, headerHeight, rowGap, groupedGap, widthFraction, avatarSize;
    bool inlineNick;
};
const PresentationMetrics &presentationMetrics();
// Native logical height, shared by the actual window and supported-size consumers.
std::uint32_t minimumWindowHeight(Renderer &);
// Typing-only reservation, independent of participant count and avatar visibility.
float typingIndicatorHeight(Renderer &, float scale, float messageSize);
MessageLayout messageLayout(const Settings &settings, const Buffer &buffer);
bool messageBubbles(const Settings &settings, const Buffer &buffer);
float messageTextSize(const Settings &settings, const Buffer &buffer);
} // namespace blast
