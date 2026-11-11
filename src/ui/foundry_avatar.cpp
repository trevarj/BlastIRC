#include "foundry_avatar.hpp"
#include "foundry_atlas_data.hpp"

#include <algorithm>
#include <array>
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
static_assert(atlas::fuseCount == 12 && atlas::bodyCount == 24 && atlas::accessoryCount == 30 &&
              atlas::markCount == 8 && atlas::tileCount == 74);

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

bool composeFoundryAvatar(std::span<std::uint8_t> rgba, std::string_view nick,
                          std::string_view mapping) {
    if (rgba.size() != tileBytes)
        return false;
    const auto &atlasImage = foundryAtlasImage();
    if (atlasImage.pixels.empty())
        return false;
    std::fill(rgba.begin(), rgba.end(), 0);
    for (const auto tile : foundryAvatarLayers(nick, mapping)) {
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
                for (int channel = 0; channel < 3; ++channel)
                    dest[channel] = static_cast<std::uint8_t>((source[channel] * alpha * 255 +
                                                               dest[channel] * dest[3] * inverse +
                                                               outAlpha / 2) /
                                                              outAlpha);
                dest[3] = static_cast<std::uint8_t>((outAlpha + 127) / 255);
            }
        }
    }
    return true;
}

bool setFoundryAvatar(Image &image, Renderer &renderer, std::string_view nick,
                      std::string_view mapping) {
    // ponytail: one reusable UI-thread scratch tile; Image keeps its own bounded slot copy.
    static std::array<std::uint8_t, tileBytes> pixels;
    if (!composeFoundryAvatar(pixels, nick, mapping))
        return false;
    return image.setSourceRaw(renderer, pixels.data(), pixels.size(), atlas::tileSize,
                              atlas::tileSize, atlas::tileSize * 4, PixmapFormat::RGBA);
}

} // namespace blast
