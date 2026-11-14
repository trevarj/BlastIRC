#include "foundry_avatar.hpp"
#include "foundry_atlas_data.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <core/files/resource_paths.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <openssl/sha.h>
#include <render/core/image_decoder.h>
#include <render/core/texture_manager.h>
#include <ui/controls/image.h>

namespace blast {
namespace {
namespace atlas = foundryAtlas;
constexpr std::size_t tileBytes = atlas::tileSize * atlas::tileSize * 4;
static_assert(atlas::fuseCount == 12 && atlas::bodyCount == 24 && atlas::accessoryCount == 30 &&
              atlas::markCount == 8 && atlas::tileCount == 74);

using Rgb = std::array<float, 3>;
constexpr std::array<std::array<Rgb, 3>, 6> bodyPalettes{{
    {{{72, 81, 89}, {120, 134, 140}, {48, 55, 63}}},
    {{{174, 99, 66}, {228, 164, 107}, {113, 61, 50}}},
    {{{224, 131, 50}, {255, 193, 107}, {157, 77, 40}}},
    {{{40, 110, 118}, {100, 169, 165}, {32, 72, 80}}},
    {{{110, 135, 144}, {164, 192, 196}, {64, 91, 101}}},
    {{{178, 139, 61}, {231, 194, 107}, {118, 90, 49}}},
}};

std::array<Rgb, 6> bodyRoles(std::size_t palette) {
    const auto &colors = bodyPalettes[palette];
    return {colors[0],       colors[1],          colors[2],
            Rgb{32, 39, 43}, Rgb{246, 223, 173}, Rgb{169, 164, 154}};
}

std::array<Rgb, 6> tintedRoles(const std::array<Rgb, 6> &source, const Color &color) {
    auto target = source;
    target[0] = {255 * std::clamp(color.r, 0.f, 1.f), 255 * std::clamp(color.g, 0.f, 1.f),
                 255 * std::clamp(color.b, 0.f, 1.f)};
    for (int channel = 0; channel < 3; ++channel) {
        const float highlight =
            (source[1][channel] - source[0][channel]) / (255 - source[0][channel]);
        target[1][channel] = target[0][channel] + (255 - target[0][channel]) * highlight;
        target[2][channel] = target[0][channel] * source[2][channel] / source[0][channel];
    }
    return target;
}

Rgb tintBodyPixel(const std::uint8_t *pixel, const std::array<Rgb, 6> &source,
                  const std::array<Rgb, 6> &target) {
    for (std::size_t role = 0; role < source.size(); ++role)
        if (std::equal(source[role].begin(), source[role].end(), pixel))
            return target[role];
    float best = std::numeric_limits<float>::max();
    Rgb result{float(pixel[0]), float(pixel[1]), float(pixel[2])};
    for (std::size_t a = 0; a < source.size(); ++a) {
        for (std::size_t b = a + 1; b < source.size(); ++b) {
            float dot = 0, length = 0;
            for (int c = 0; c < 3; ++c) {
                const float delta = source[b][c] - source[a][c];
                dot += (pixel[c] - source[a][c]) * delta;
                length += delta * delta;
            }
            const float mix = std::clamp(dot / length, 0.f, 1.f);
            float distance = 0;
            Rgb correction{};
            for (int c = 0; c < 3; ++c) {
                const float original = std::lerp(source[a][c], source[b][c], mix);
                distance += (pixel[c] - original) * (pixel[c] - original);
                correction[c] = std::lerp(target[a][c], target[b][c], mix) - original;
            }
            if (distance < best) {
                best = distance;
                for (int c = 0; c < 3; ++c)
                    result[c] = std::clamp(pixel[c] + correction[c], 0.f, 255.f);
            }
        }
    }
    return result;
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
} // namespace

std::array<std::size_t, 4> foundryAvatarLayers(std::string_view nick, std::string_view mapping) {
    constexpr std::string_view prefix = "foundry-concept-1:";
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
    const std::size_t body = digest[0] % 4, palette = digest[1] % 6;
    const std::size_t fuse = digest[2] % 4, spark = digest[3] % 3;
    const std::size_t accessory = digest[4] % 5, mark = digest[5] % 4;
    return {atlas::fuseOffset + fuse * 3 + spark, atlas::bodyOffset + body * 6 + palette,
            atlas::accessoryOffset + accessory * 6 + palette,
            atlas::markOffset + (accessory == 1 ? 4 : 0) + mark};
}

float foundryAvatarBodyCenterY(std::string_view nick, std::string_view mapping) {
    static const auto centers = [] {
        std::array<float, atlas::bodyCount> result;
        result.fill(.5f);
        const auto &image = foundryAtlasImage();
        if (image.pixels.empty())
            return result;
        for (size_t body = 0; body < result.size(); ++body) {
            const auto tile = atlas::bodyOffset + body;
            const auto x = tile % atlas::columns * atlas::tileSize;
            const auto y = tile / atlas::columns * atlas::tileSize;
            size_t top = atlas::tileSize, bottom = 0;
            for (size_t row = 0; row < atlas::tileSize; ++row)
                for (size_t col = 0; col < atlas::tileSize; ++col)
                    if (image.pixels[((y + row) * atlas::width + x + col) * 4 + 3] >= 128) {
                        top = std::min(top, row);
                        bottom = std::max(bottom, row + 1);
                    }
            if (top < bottom)
                result[body] = float(top + bottom) / (2 * atlas::tileSize);
        }
        return result;
    }();
    return centers[foundryAvatarLayers(nick, mapping)[1] - atlas::bodyOffset];
}

bool composeFoundryAvatar(std::span<std::uint8_t> rgba, std::string_view nick,
                          const Color &bodyColor, std::string_view mapping) {
    if (rgba.size() != tileBytes)
        return false;
    const auto &atlasImage = foundryAtlasImage();
    if (atlasImage.pixels.empty())
        return false;
    std::fill(rgba.begin(), rgba.end(), 0);
    const auto layers = foundryAvatarLayers(nick, mapping);
    const auto sourceRoles = bodyRoles((layers[1] - atlas::bodyOffset) % 6);
    const auto targetRoles = tintedRoles(sourceRoles, bodyColor);
    for (const auto tile : layers) {
        const auto x = (tile % atlas::columns) * atlas::tileSize;
        const auto y = (tile / atlas::columns) * atlas::tileSize;
        for (std::size_t row = 0; row < atlas::tileSize; ++row) {
            const auto *source = atlasImage.pixels.data() + ((y + row) * atlas::width + x) * 4;
            auto *dest = rgba.data() + row * atlas::tileSize * 4;
            for (std::size_t col = 0; col < atlas::tileSize; ++col, source += 4, dest += 4) {
                const unsigned alpha = source[3], inverse = 255 - alpha;
                const unsigned outAlpha = alpha * 255 + dest[3] * inverse;
                if (!outAlpha)
                    continue;
                const auto rgb = alpha && tile == layers[1]
                                     ? tintBodyPixel(source, sourceRoles, targetRoles)
                                     : Rgb{float(source[0]), float(source[1]), float(source[2])};
                for (int channel = 0; channel < 3; ++channel)
                    dest[channel] = static_cast<std::uint8_t>(
                        (std::lround(rgb[channel]) * alpha * 255 +
                         dest[channel] * dest[3] * inverse + outAlpha / 2) /
                        outAlpha);
                dest[3] = static_cast<std::uint8_t>((outAlpha + 127) / 255);
            }
        }
    }
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
