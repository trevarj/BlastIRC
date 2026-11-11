#include "irc/protocol.hpp"
#include "ui/foundry_atlas_data.hpp"
#include "ui/foundry_avatar.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <core/application_identity.h>
#include <cstdint>
#include <render/codecs/codecs.h>
#include <set>
#include <string>

int main() {
    namespace atlas = blast::foundryAtlas;
    setApplicationIdentity({.name = "blastirc", .envPrefix = "BLASTIRC"});
    codecs::installImageDecoder();
    constexpr std::size_t bytes = atlas::tileSize * atlas::tileSize * 4;
    std::array<std::uint8_t, bytes> first{}, second{};
    assert(!blast::composeFoundryAvatar(std::span(first).first(bytes - 1), "Alice"));
    assert(blast::composeFoundryAvatar(first, "Alice[\\^"));
    assert(blast::composeFoundryAvatar(second, "aLICE{|~"));
    assert(first == second);
    for (const auto mapping : {"ascii", "strict-rfc1459", "rfc1459", "unknown"}) {
        const std::string nick = "Nick[\\^\xc3\x89";
        const auto folded = blast::irc::casefold(nick, mapping);
        assert(blast::foundryAvatarLayers(nick, mapping) ==
               blast::foundryAvatarLayers(folded, mapping));
        assert(blast::composeFoundryAvatar(first, nick, mapping));
        assert(blast::composeFoundryAvatar(second, folded, mapping));
        assert(first == second);
    }
    assert(blast::composeFoundryAvatar(first, "Nick[\\^", "ascii"));
    assert(blast::composeFoundryAvatar(second, "Nick{|~", "ascii"));
    assert(first != second);
    assert(blast::composeFoundryAvatar(first, "Nick^", "strict-rfc1459"));
    assert(blast::composeFoundryAvatar(second, "Nick~", "strict-rfc1459"));
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

    std::set<std::uint64_t> rasters;
    bool transparent = false, opaque = false, antialiased = false;
    for (int i = 0; i < 64; ++i) {
        assert(blast::composeFoundryAvatar(first, "foundry-check-" + std::to_string(i)));
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
    assert(blast::composeFoundryAvatar(first, "Alice"));
    const auto retained = first;
    assert(blast::composeFoundryAvatar(second, "Bob"));
    assert(first == retained);
    assert(first != second);
}
