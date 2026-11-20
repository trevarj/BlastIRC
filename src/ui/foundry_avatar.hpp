#pragma once

#include <cstddef>
#include <cstdint>
#include <render/core/color.h>
#include <span>
#include <string_view>

class Image;
class Renderer;

namespace blast {

// Visible body midpoint as a fraction of tile height, excluding the separate fuse layer.
float foundryAvatarBodyCenterY(std::string_view nick, std::string_view mapping = "rfc1459");
// Writes exactly one atlas-sized straight-alpha RGBA tile. No renderer/GPU cache.
bool composeFoundryAvatar(std::span<std::uint8_t> rgba, std::string_view nick,
                          const Color &bodyColor, std::string_view mapping = "rfc1459");
// UI-thread only. Call when a virtual slot's identity changes, not on every frame.
// Image owns its uploaded pixels, including across GPU-resource reloads.
bool setFoundryAvatar(Image &image, Renderer &renderer, std::string_view nick,
                      const Color &bodyColor, std::string_view mapping = "rfc1459");

} // namespace blast
