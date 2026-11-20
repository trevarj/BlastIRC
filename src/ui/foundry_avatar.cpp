#include "foundry_avatar.hpp"
#include "foundry_atlas_data.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <core/files/resource_paths.h>
#include <fstream>
#include <iostream>
#include <openssl/sha.h>
#include <render/core/image_decoder.h>
#include <render/core/texture_manager.h>
#include <ui/controls/image.h>

namespace blast {
namespace {
namespace atlas = foundryAtlas;
constexpr std::size_t tileBytes = atlas::tileSize * atlas::tileSize * 4;
static_assert(atlas::fuseCount == atlas::bodyKinds && atlas::bodyCount == atlas::bodyKinds &&
              atlas::tintCount == atlas::bodyKinds &&
              atlas::faceCount == atlas::bodyKinds * atlas::faceKinds &&
              atlas::backGearCount == atlas::bodyKinds * atlas::gearKinds &&
              atlas::frontGearCount == atlas::backGearCount);

struct Traits {
    std::size_t body, face, gear;
};

const std::uint8_t *tileRow(const DecodedRasterImage &image, std::size_t tile, std::size_t row) {
    const auto x = tile % atlas::columns * atlas::tileSize;
    const auto y = tile / atlas::columns * atlas::tileSize;
    return image.pixels.data() + ((y + row) * atlas::width + x) * 4;
}

void composite(std::span<std::uint8_t> rgba, const DecodedRasterImage &image, std::size_t tile,
               const std::array<unsigned, 3> &color, std::size_t tintTile = atlas::tileCount) {
    for (std::size_t row = 0; row < atlas::tileSize; ++row) {
        const auto *source = tileRow(image, tile, row);
        const auto *tint = tintTile == atlas::tileCount ? nullptr : tileRow(image, tintTile, row);
        auto *dest = rgba.data() + row * atlas::tileSize * 4;
        for (std::size_t col = 0; col < atlas::tileSize; ++col, source += 4, dest += 4) {
            const unsigned alpha = source[3];
            if (!alpha)
                continue;
            const unsigned weight = tint ? tint[col * 4] : 0;
            const unsigned behind = dest[3] * (255 - alpha);
            const unsigned front = alpha * 255;
            const unsigned total = front + behind;
            for (std::size_t channel = 0; channel < color.size(); ++channel) {
                const unsigned ink =
                    std::min(255u, source[channel] + (weight * color[channel] + 127) / 255);
                dest[channel] = static_cast<std::uint8_t>(
                    behind ? (ink * front + dest[channel] * behind + total / 2) / total : ink);
            }
            dest[3] = static_cast<std::uint8_t>((total + 127) / 255);
        }
    }
}

const DecodedRasterImage &foundryAtlasImage() {
    static const DecodedRasterImage image = [] {
        const auto fail = [](std::string_view reason) {
            std::cerr << "BlastIRC: Foundry avatar atlas: " << reason << '\n';
            return DecodedRasterImage{};
        };
        std::ifstream input(paths::assetPath("foundry/atlas.png"),
                            std::ios::binary | std::ios::ate);
        const auto size = input.tellg();
        if (!input || size < 24 || size > 4 * 1024 * 1024)
            return fail("missing asset or invalid encoded size");
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        input.seekg(0);
        if (!input.read(reinterpret_cast<char *>(bytes.data()), size))
            return fail("cannot read asset");
        constexpr std::array<std::uint8_t, 16> pngHeader = {137, 80, 78, 71, 13,  10,  26,  10,
                                                            0,   0,  0,  13, 'I', 'H', 'D', 'R'};
        const auto dimension = [&](std::size_t at) {
            return (std::uint32_t(bytes[at]) << 24) | (std::uint32_t(bytes[at + 1]) << 16) |
                   (std::uint32_t(bytes[at + 2]) << 8) | bytes[at + 3];
        };
        // Bound dimensions before invoking the codec, even for an overridden asset bundle.
        if (!std::equal(pngHeader.begin(), pngHeader.end(), bytes.begin()) ||
            dimension(16) != atlas::width || dimension(20) != atlas::height)
            return fail("invalid PNG header or atlas dimensions");
        auto decoded = decodeRasterImage(bytes.data(), bytes.size());
        if (!decoded)
            return fail(decoded.error());
        if (decoded->width != atlas::width || decoded->height != atlas::height ||
            decoded->pixels.size() != std::size_t(atlas::width) * atlas::height * 4)
            return fail("invalid decoded dimensions or pixel size");
        return std::move(*decoded);
    }();
    return image;
}

Traits avatarTraits(std::string_view nick, std::string_view mapping) {
    constexpr std::string_view prefix = "short-fuse-club-1:";
    // Reserve once; the ASCII prefix is unchanged by every supported IRC casemapping.
    std::string seed;
    seed.reserve(prefix.size() + nick.size());
    seed.append(prefix);
    seed.append(nick);
    // Same byte-preserving rules as irc::casefold (not locale/Unicode lowercasing).
    for (char &c : seed) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        else if (mapping != "ascii") {
            if (c == '[')
                c = '{';
            else if (c == ']')
                c = '}';
            else if (c == '\\')
                c = '|';
            else if (c == '^' && mapping != "strict-rfc1459")
                c = '~';
        }
    }
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char *>(seed.data()), seed.size(), digest.data());
    return {digest[0] % atlas::bodyKinds, digest[1] % atlas::faceKinds,
            digest[2] % atlas::gearKinds};
}
} // namespace

float foundryAvatarBodyCenterY(std::string_view nick, std::string_view mapping) {
    static const auto centers = [] {
        std::array<float, atlas::bodyCount> result;
        result.fill(.5f);
        const auto &image = foundryAtlasImage();
        if (image.pixels.empty())
            return result;
        for (size_t body = 0; body < result.size(); ++body) {
            size_t top = atlas::tileSize, bottom = 0;
            for (size_t row = 0; row < atlas::tileSize; ++row) {
                const auto *source = tileRow(image, atlas::bodyOffset + body, row);
                for (size_t col = 0; col < atlas::tileSize; ++col)
                    if (source[col * 4 + 3] >= 128) {
                        top = std::min(top, row);
                        bottom = std::max(bottom, row + 1);
                        break;
                    }
            }
            if (top < bottom)
                result[body] = float(top + bottom) / (2 * atlas::tileSize);
        }
        return result;
    }();
    return centers[avatarTraits(nick, mapping).body];
}

bool composeFoundryAvatar(std::span<std::uint8_t> rgba, std::string_view nick,
                          const Color &bodyColor, std::string_view mapping) {
    if (rgba.size() != tileBytes)
        return false;
    const auto &atlasImage = foundryAtlasImage();
    if (atlasImage.pixels.empty())
        return false;
    std::fill(rgba.begin(), rgba.end(), 0);
    const auto [body, face, gear] = avatarTraits(nick, mapping);
    const auto toByte = [](float value) {
        return static_cast<unsigned>(std::lround(255 * std::clamp(value, 0.f, 1.f)));
    };
    const std::array color{toByte(bodyColor.r), toByte(bodyColor.g), toByte(bodyColor.b)};
    composite(rgba, atlasImage, atlas::fuseOffset + body, color);
    if (gear)
        composite(rgba, atlasImage, atlas::backGearOffset + body * atlas::gearKinds + gear, color);
    composite(rgba, atlasImage, atlas::bodyOffset + body, color, atlas::tintOffset + body);
    if (gear)
        composite(rgba, atlasImage, atlas::frontGearOffset + body * atlas::gearKinds + gear, color);
    composite(rgba, atlasImage, atlas::faceOffset + body * atlas::faceKinds + face, color);
    return true;
}

bool setFoundryAvatar(Image &image, Renderer &renderer, std::string_view nick,
                      const Color &bodyColor, std::string_view mapping) {
    // ponytail: one reusable UI-thread scratch tile; Image keeps its own bounded slot copy.
    static std::array<std::uint8_t, tileBytes> pixels;
    if (!composeFoundryAvatar(pixels, nick, bodyColor, mapping))
        return false;
    return image.setSourceRaw(renderer, pixels.data(), pixels.size(), atlas::tileSize,
                              atlas::tileSize, atlas::tileSize * 4, PixmapFormat::RGBA);
}

} // namespace blast
