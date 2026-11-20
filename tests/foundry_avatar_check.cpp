#include "irc/protocol.hpp"
#include "ui/foundry_atlas_data.hpp"
#include "ui/foundry_avatar.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <cassert>
#include <core/application_identity.h>
#include <cstdint>
#include <render/codecs/codecs.h>
#include <set>
#include <span>
#include <string>

int main() {
    namespace atlas = blast::foundryAtlas;
    setApplicationIdentity({.name = "blastirc", .envPrefix = "BLASTIRC"});
    codecs::installImageDecoder();
    constexpr std::size_t pixels = atlas::tileSize * atlas::tileSize;
    std::array<std::uint8_t, pixels * 4> first{}, second{};
    const Color bodyColor{.2f, .6f, .8f, 1.f};
    const Color warmTint{.8f, .3f, .1f, .2f};
    assert(
        !blast::composeFoundryAvatar(std::span(first).first(first.size() - 1), "Alice", bodyColor));
    assert(blast::composeFoundryAvatar(first, "Alice[\\^", bodyColor));
    assert(blast::composeFoundryAvatar(second, "aLICE{|~", bodyColor));
    assert(first == second);
    for (const auto mapping : {"ascii", "strict-rfc1459", "rfc1459", "unknown"}) {
        const std::string nick = "Nick[\\^\xc3\x89";
        const auto folded = blast::irc::casefold(nick, mapping);
        assert(blast::composeFoundryAvatar(first, nick, bodyColor, mapping));
        assert(blast::composeFoundryAvatar(second, folded, bodyColor, mapping));
        assert(first == second);
    }

    std::set<std::uint64_t> rasters;
    // Sample the lower half so headwear and the spark don't stand in for new body shapes.
    std::array<std::bitset<pixels / 2>, 3> silhouettes;
    std::size_t distinctSilhouettes = 0;
    bool transparent = false, opaque = false, antialiased = false;
    bool asciiDistinguishes = false, strictDistinguishes = false;
    for (int i = 0; i < 192; ++i) {
        const auto nick = "foundry-check-" + std::to_string(i);
        assert(blast::composeFoundryAvatar(first, nick, bodyColor));
        assert(blast::composeFoundryAvatar(second, nick, warmTint));
        std::bitset<pixels / 2> silhouette;
        std::uint64_t fingerprint = 14695981039346656037ULL;
        std::size_t retinted = 0, fixedMarks = 0;
        for (std::size_t at = 0; at < first.size(); at += 4) {
            const auto r = first[at], g = first[at + 1], b = first[at + 2], a = first[at + 3];
            const auto r2 = second[at], g2 = second[at + 1], b2 = second[at + 2];
            assert(a == second[at + 3]);
            transparent |= a == 0;
            opaque |= a == 255;
            antialiased |= a > 0 && a < 255;
            if (!a)
                assert(!r && !g && !b && !r2 && !g2 && !b2);
            if (a == 255) {
                retinted += b - r > 24 && r2 - b2 > 24;
                fixedMarks +=
                    r == r2 && g == g2 && b == b2 && std::max({r, g, b}) - std::min({r, g, b}) > 32;
            }
            if (distinctSilhouettes < silhouettes.size() && at / 4 >= pixels / 2)
                silhouette[at / 4 - pixels / 2] = a >= 128;
            for (const auto channel : {r, g, b, a})
                fingerprint = (fingerprint ^ channel) * 1099511628211ULL;
        }
        assert(retinted >= 100);
        assert(fixedMarks >= 32);
        rasters.insert(fingerprint);
        if (distinctSilhouettes < silhouettes.size() &&
            std::all_of(
                silhouettes.begin(), silhouettes.begin() + distinctSilhouettes,
                [&](const auto &other) { return (silhouette ^ other).count() >= pixels / 40; }))
            silhouettes[distinctSilhouettes++] = silhouette;

        // Distinct keys can select the same avatar; probe more than one pair.
        if (!asciiDistinguishes) {
            assert(blast::composeFoundryAvatar(first, nick + "[\\^", bodyColor, "ascii"));
            assert(blast::composeFoundryAvatar(second, nick + "{|~", bodyColor, "ascii"));
            asciiDistinguishes = first != second;
        }
        if (!strictDistinguishes) {
            assert(blast::composeFoundryAvatar(first, nick + "^", bodyColor, "strict-rfc1459"));
            assert(blast::composeFoundryAvatar(second, nick + "~", bodyColor, "strict-rfc1459"));
            strictDistinguishes = first != second;
        }
    }
    assert(asciiDistinguishes && strictDistinguishes);
    assert(distinctSilhouettes == silhouettes.size());
    // At this sampling size the cast should show far more than a two-axis subset.
    assert(rasters.size() >= 55);
    assert(transparent && opaque && antialiased);
    // Scratch/atlas reuse must not overwrite a caller's retained raster.
    assert(blast::composeFoundryAvatar(first, "Alice", bodyColor));
    const auto retained = first;
    assert(blast::composeFoundryAvatar(second, "Bob", warmTint));
    assert(first == retained);
    assert(first != second);
    assert(blast::composeFoundryAvatar(second, "Alice", bodyColor));
    assert(second == retained);
}
