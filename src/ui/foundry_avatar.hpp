#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

class Image;
class Renderer;

namespace blast {

// Tile indices in drawing order: fuse, body, accessory, mark.
std::array<std::size_t, 4> foundryAvatarLayers(std::string_view nick,
                                               std::string_view mapping = "rfc1459");
// Writes exactly one atlas-sized straight-alpha RGBA tile. No renderer/GPU cache.
bool composeFoundryAvatar(std::span<std::uint8_t> rgba, std::string_view nick,
                          std::string_view mapping = "rfc1459");
// UI-thread only. Call when a virtual slot's identity changes, not on every frame.
// Image owns its uploaded pixels, including across GPU-resource reloads.
bool setFoundryAvatar(Image &image, Renderer &renderer, std::string_view nick,
                      std::string_view mapping = "rfc1459");

} // namespace blast
