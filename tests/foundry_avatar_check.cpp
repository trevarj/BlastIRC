#include "irc/protocol.hpp"
#include "ui/foundry_atlas_data.hpp"
#include "ui/foundry_avatar.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <core/application_identity.h>
#include <core/files/resource_paths.h>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <render/codecs/codecs.h>
#include <render/core/image_decoder.h>
#include <set>
#include <string>
#include <vector>

int main() {
    namespace atlas = blast::foundryAtlas;
    setApplicationIdentity({.name = "blastirc", .envPrefix = "BLASTIRC"});
    codecs::installImageDecoder();
    constexpr std::size_t bytes = atlas::tileSize * atlas::tileSize * 4;
    std::array<std::uint8_t, bytes> first{}, second{};
    const Color bodyColor{.2f, .6f, .8f, 1.f};
    assert(!blast::composeFoundryAvatar(std::span(first).first(bytes - 1), "Alice", bodyColor));
    assert(blast::composeFoundryAvatar(first, "Alice[\\^", bodyColor));
    assert(blast::composeFoundryAvatar(second, "aLICE{|~", bodyColor));
    assert(first == second);
    for (const auto mapping : {"ascii", "strict-rfc1459", "rfc1459", "unknown"}) {
        const std::string nick = "Nick[\\^\xc3\x89";
        const auto folded = blast::irc::casefold(nick, mapping);
        assert(blast::foundryAvatarLayers(nick, mapping) ==
               blast::foundryAvatarLayers(folded, mapping));
        assert(blast::composeFoundryAvatar(first, nick, bodyColor, mapping));
        assert(blast::composeFoundryAvatar(second, folded, bodyColor, mapping));
        assert(first == second);
    }
    assert(blast::composeFoundryAvatar(first, "Nick[\\^", bodyColor, "ascii"));
    assert(blast::composeFoundryAvatar(second, "Nick{|~", bodyColor, "ascii"));
    assert(first != second);
    assert(blast::composeFoundryAvatar(first, "Nick^", bodyColor, "strict-rfc1459"));
    assert(blast::composeFoundryAvatar(second, "Nick~", bodyColor, "strict-rfc1459"));
    assert(first != second);

    // Every shipped component is selectable, not merely a nonempty placeholder image.
    std::array<std::set<std::size_t>, 4> selected;
    for (int i = 0; i < 2048; ++i) {
        const auto layers = blast::foundryAvatarLayers("foundry-check-" + std::to_string(i));
        for (std::size_t layer = 0; layer < layers.size(); ++layer) {
            assert(layers[layer] < atlas::tileCount);
            selected[layer].insert(layers[layer]);
        }
    }
    assert(selected[0].size() == atlas::fuseCount);
    assert(selected[1].size() == atlas::bodyCount);
    assert(selected[2].size() == atlas::accessoryCount);
    assert(selected[3].size() == atlas::markCount);

    std::ifstream input(paths::assetPath("foundry/atlas.png"), std::ios::binary);
    const std::vector<std::uint8_t> encoded{std::istreambuf_iterator<char>(input),
                                            std::istreambuf_iterator<char>()};
    const auto decoded = decodeRasterImage(encoded.data(), encoded.size());
    assert(decoded);
    const auto pixel = [&](std::size_t tile, std::size_t at) {
        return decoded->pixels.data() +
               (((tile / atlas::columns) * atlas::tileSize + at / atlas::tileSize) * atlas::width +
                (tile % atlas::columns) * atlas::tileSize + at % atlas::tileSize) *
                   4;
    };
    constexpr std::array<std::array<int, 3>, 6> bases{{{72, 81, 89},
                                                       {174, 99, 66},
                                                       {224, 131, 50},
                                                       {40, 110, 118},
                                                       {110, 135, 144},
                                                       {178, 139, 61}}};
    constexpr std::array<std::array<int, 3>, 6> highlights{{{120, 134, 140},
                                                            {228, 164, 107},
                                                            {255, 193, 107},
                                                            {100, 169, 165},
                                                            {164, 192, 196},
                                                            {231, 194, 107}}};
    constexpr std::array<std::array<int, 3>, 6> shades{
        {{48, 55, 63}, {113, 61, 50}, {157, 77, 40}, {32, 72, 80}, {64, 91, 101}, {118, 90, 49}}};
    bool baseSeen = false, highlightSeen = false, shadeSeen = false;
    bool outlineSeen = false, accessorySeen = false, fuseSeen = false;
    for (int i = 0; i < 64; ++i) {
        const auto nick = "foundry-check-" + std::to_string(i);
        const auto layers = blast::foundryAvatarLayers(nick);
        const auto palette = (layers[1] - atlas::bodyOffset) % 6;
        assert(blast::composeFoundryAvatar(first, nick, bodyColor));
        assert(blast::composeFoundryAvatar(second, nick, Color{.8f, .3f, .1f, .2f}));
        for (std::size_t at = 0; at < atlas::tileSize * atlas::tileSize; ++at) {
            const auto offset = at * 4;
            assert(first[offset + 3] == second[offset + 3]);
            const auto *body = pixel(layers[1], at);
            const auto *accessory = pixel(layers[2], at);
            const auto *mark = pixel(layers[3], at);
            if (mark[3] == 255 || (accessory[3] == 255 && mark[3] == 0)) {
                const auto *top = mark[3] == 255 ? mark : accessory;
                assert(std::equal(top, top + 4, first.data() + offset));
                assert(std::equal(top, top + 4, second.data() + offset));
                accessorySeen = true;
            }
            if (!body[3]) {
                assert(std::equal(first.data() + offset, first.data() + offset + 4,
                                  second.data() + offset));
                fuseSeen |= pixel(layers[0], at)[3] != 0;
            }
            if (body[3] != 255 || accessory[3] || mark[3])
                continue;
            for (int role = 0; role < 3; ++role) {
                const auto &source = role == 0   ? bases[palette]
                                     : role == 1 ? highlights[palette]
                                                 : shades[palette];
                if (!std::equal(source.begin(), source.end(), body))
                    continue;
                baseSeen |= role == 0;
                highlightSeen |= role == 1;
                shadeSeen |= role == 2;
                for (int c = 0; c < 3; ++c) {
                    const float base = std::array{51.f, 153.f, 204.f}[c];
                    const float expected =
                        role == 0 ? base
                        : role == 1
                            ? base + (255 - base) * (highlights[palette][c] - bases[palette][c]) /
                                         (255 - bases[palette][c])
                            : base * shades[palette][c] / bases[palette][c];
                    assert(first[offset + c] == std::lround(expected));
                }
            }
            if (body[0] == 32 && body[1] == 39 && body[2] == 43) {
                assert(std::equal(body, body + 4, first.data() + offset));
                outlineSeen = true;
            }
        }
    }
    assert(baseSeen && highlightSeen && shadeSeen && outlineSeen && accessorySeen && fuseSeen);
    std::set<std::uint64_t> rasters;
    bool transparent = false, opaque = false, antialiased = false;
    for (int i = 0; i < 64; ++i) {
        assert(blast::composeFoundryAvatar(first, "foundry-check-" + std::to_string(i), bodyColor));
        std::uint64_t fingerprint = 14695981039346656037ULL;
        for (std::size_t at = 0; at < first.size(); ++at) {
            fingerprint = (fingerprint ^ first[at]) * 1099511628211ULL;
            if (at % 4 == 3) {
                transparent |= first[at] == 0;
                opaque |= first[at] == 255;
                antialiased |= first[at] > 0 && first[at] < 255;
                if (!first[at])
                    assert(first[at - 3] == 0 && first[at - 2] == 0 && first[at - 1] == 0);
            }
        }
        rasters.insert(fingerprint);
    }
    assert(rasters.size() >= 48);
    assert(transparent && opaque && antialiased);
    // Scratch/atlas reuse must not overwrite a caller's retained raster.
    assert(blast::composeFoundryAvatar(first, "Alice", bodyColor));
    const auto retained = first;
    assert(blast::composeFoundryAvatar(second, "Bob", bodyColor));
    assert(first == retained);
    assert(first != second);
}
