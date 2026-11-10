#include "../src/theme/theme.hpp"
#include "../src/ui/presentation.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <ui/palette.h>
#include <ui/style.h>
#include <unistd.h>
#include <utility>

using namespace blast;
namespace {
struct ConfigDirectory {
    std::filesystem::path path;
    std::string previous;
    bool hadPrevious = false;
    ConfigDirectory() {
        if (const auto *value = std::getenv("XDG_CONFIG_HOME")) {
            hadPrevious = true;
            previous = value;
        }
        char name[] = "/tmp/blastirc-native-theme-check-XXXXXX";
        const auto *created = mkdtemp(name);
        assert(created);
        path = created;
        assert(setenv("XDG_CONFIG_HOME", path.c_str(), 1) == 0);
        std::filesystem::create_directory(path / "blastirc");
    }
    ~ConfigDirectory() {
        if (hadPrevious)
            setenv("XDG_CONFIG_HOME", previous.c_str(), 1);
        else
            unsetenv("XDG_CONFIG_HOME");
        std::filesystem::remove_all(path);
    }
};
bool near(Color a, Color b) {
    constexpr float tolerance = 1e-5f;
    return std::abs(a.r - b.r) < tolerance && std::abs(a.g - b.g) < tolerance &&
           std::abs(a.b - b.b) < tolerance && std::abs(a.a - b.a) < tolerance;
}
float contrast(Color foreground, Color background) {
    const Color composite{foreground.r * foreground.a + background.r * (1 - foreground.a),
                          foreground.g * foreground.a + background.g * (1 - foreground.a),
                          foreground.b * foreground.a + background.b * (1 - foreground.a), 1};
    const float foregroundLuminance = relativeLuminance(composite);
    const float backgroundLuminance = relativeLuminance(background);
    return (std::max(foregroundLuminance, backgroundLuminance) + .05f) /
           (std::min(foregroundLuminance, backgroundLuminance) + .05f);
}
Palette publish(const ConfigDirectory &config, const char *primary, const char *onPrimary,
                const char *onSurface, const char *inverseOnSurface, const char *scrim,
                const char *shadow) {
    {
        std::ofstream output(config.path / "blastirc" / "noctalia.toml");
        output << "[colors]\n"
               << "primary = '" << primary << "'\n"
               << "on_primary = '" << onPrimary << "'\n"
               << "surface = '#ffffff'\n"
               << "on_surface = '" << onSurface << "'\n"
               << "inverse_on_surface = '" << inverseOnSurface << "'\n"
               << "scrim = '" << scrim << "'\n"
               << "shadow = '" << shadow << "'\n"
               << "tertiary = '#6750a4'\n"
               << "hover = '#336699'\n"
               << "on_hover = '#ffffff'\n"
               << "error = '#ba1a1a'\n";
        output.close();
        assert(output);
    }
    Model model;
    model.settings.reducedMotion = true;
    Theme theme;
    theme.update(model);
    assert(std::string(themeSource()) == "Noctalia");
    assert(near(::palette.primary, hex(primary)));
    assert(near(themeColor("primary"), hex(primary)));
    assert(near(themeColor("on_primary"), hex(onPrimary)));
    assert(near(themeColor("on_surface"), hex(onSurface)));
    assert(near(themeColor("inverse_on_surface"), hex(inverseOnSurface)));
    assert(near(themeColor("scrim"), hex(scrim)));
    assert(near(themeColor("shadow"), hex(shadow)));
    assert(near(::palette.hover, hex("#336699")));
    assert(near(::palette.onHover, hex("#ffffff")));
    assert(contrast(::palette.onPrimary, ::palette.primary) >= 4.5f);
    assert(contrast(::palette.onError, ::palette.error) >= 4.5f);
    return ::palette;
}
void surfaces(const ConfigDirectory &config) {
    const auto writeSurface = [&](const char *surface) {
        std::ofstream output(config.path / "blastirc" / "noctalia.toml");
        output << "[colors]\n"
               << "primary = '#a8c7fa55'\n"
               << "on_primary = '#062e6f'\n"
               << "surface = '" << surface << "'\n"
               << "on_surface = '#e2e2e944'\n"
               << "tertiary = '#d5baf566'\n"
               << "error = '#ffb4ab77'\n"
               << "background = '#10131a22'\n"
               << "terminal_background = '#10131a22'\n"
               << "primary_container = '#28477733'\n"
               << "surface_container_low = '#191c2344'\n";
        output.close();
        assert(output);
    };
    const auto assertAlpha = [](float alpha) {
        assert(themeSurfaceAlpha() == alpha);
        for (const auto role :
             {"background", "surface", "surface_container_low", "surface_container",
              "surface_container_high", "surface_container_highest", "primary_container",
              "secondary_container", "tertiary_container", "terminal_background"}) {
            const auto authored = themeColor(role);
            assert(near(themeSurfaceColor(role), withAlpha(authored, alpha)));
        }
        assert(themeSurfaceFill(hex("#12345612")).a == alpha);
        for (const auto role : {"primary", "on_surface", "tertiary", "error"}) {
            const auto authored = themeColor(role);
            assert(authored.a < 1.f);
            assert(near(themeForegroundColor(role), withAlpha(authored, 1.f)));
        }
        for (const auto field :
             {&Palette::surface, &Palette::surfaceContainerLow, &Palette::surfaceContainer,
              &Palette::surfaceContainerHigh, &Palette::surfaceContainerHighest,
              &Palette::primaryContainer, &Palette::secondaryContainer, &Palette::tertiaryContainer,
              &Palette::errorContainer, &Palette::hover})
            assert((::palette.*field).a == alpha);
        for (const auto field :
             {&Palette::primary, &Palette::onPrimary, &Palette::secondary, &Palette::onSecondary,
              &Palette::tertiary, &Palette::onTertiary, &Palette::error, &Palette::onError,
              &Palette::onSurface, &Palette::onSurfaceVariant, &Palette::onPrimaryContainer,
              &Palette::onSecondaryContainer, &Palette::onTertiaryContainer,
              &Palette::onErrorContainer, &Palette::onHover})
            assert((::palette.*field).a == 1.f);
        for (const auto &pair : {std::pair{::palette.onPrimary, ::palette.primary},
                                 std::pair{::palette.onError, ::palette.error}}) {
            assert(contrast(pair.first, pair.second) >= 4.5f);
            // An opaque count plate must stay readable regardless of the desktop behind it.
            for (const auto backdrop : {hex("#000000"), hex("#ffffff"), hex("#ff00ff")}) {
                const Color effective{
                    pair.second.r * pair.second.a + backdrop.r * (1 - pair.second.a),
                    pair.second.g * pair.second.a + backdrop.g * (1 - pair.second.a),
                    pair.second.b * pair.second.a + backdrop.b * (1 - pair.second.a), 1};
                assert(contrast(pair.first, effective) >= 4.5f);
            }
        }
    };
    writeSurface("#14171e22");
    Model model;
    model.settings.reducedMotion = true;
    Theme theme;
    // A retained pre-refresh opacity must not opt the user into transparency.
    model.settings.opacity = .4f;
    model.settings.transparency = false;
    theme.update(model);
    assertAlpha(1.f);
    assert(near(themeColor("surface"), hex("#14171e22")));
    assert(Style::buttonBordersEnabled());
    assert(Style::inputBordersEnabled());
    assert(Style::cardBordersEnabled());
    assert(Style::popupBordersEnabled());
    const auto opaqueRevision = themeRevision();
    model.settings.transparency = true;
    theme.update(model);
    assert(themeRevision() > opaqueRevision);
    assertAlpha(.4f);
    // Surface opacity must not rewrite imported roles or their RGB values.
    assert(near(themeColor("primary"), hex("#a8c7fa55")));
    assert(near(themeColor("on_surface"), hex("#e2e2e944")));
    const auto revision = themeRevision();
    writeSurface("#39414f11");
    assert(std::raise(SIGUSR1) == 0);
    theme.update(model);
    assert(themeRevision() > revision);
    assert(std::string(themeSource()) == "Noctalia");
    assert(near(themeColor("surface"), hex("#39414f11")));
    assertAlpha(.4f);
    model.settings.opacity = .01f;
    theme.update(model);
    assertAlpha(.2f);
    model.settings.opacity = std::numeric_limits<float>::quiet_NaN();
    theme.update(model);
    assertAlpha(1.f);
    model.settings.opacity = .4f;
    model.settings.transparency = false;
    theme.update(model);
    assertAlpha(1.f);
}
void presentation() {
    Model model;
    model.settings.reducedMotion = true;
    Theme theme;
    theme.update(model);
    const auto uiFont = themeUiFont().family;
    const auto messageFont = themeMessageFont().family;
    model.settings.uiFont = "serif";
    model.settings.messageFont = "monospace";
    theme.update(model);
    assert(themeUiFont().family == "serif");
    assert(themeMessageFont().family == "monospace");
    model.settings.uiFont.clear();
    model.settings.messageFont.clear();
    theme.update(model);
    assert(themeUiFont().family == uiFont && themeMessageFont().family == messageFont);

    Buffer channel, query, server;
    channel.id = "network/#room";
    channel.kind = "channel";
    query.id = "network/nick";
    query.kind = "query";
    server.id = "network";
    server.kind = "server";
    assert(messageBubbles(model.settings, channel));
    assert(messageBubbles(model.settings, query));
    assert(!messageBubbles(model.settings, server));
    model.settings.ircChannels.insert(channel.id);
    assert(!messageBubbles(model.settings, channel));
    assert(messageBubbles(model.settings, query));
    model.settings.ircChannels.insert(query.id);
    assert(!messageBubbles(model.settings, query));
    model.settings.ircChannels.erase(channel.id);
    assert(messageBubbles(model.settings, channel));
    assert(!messageBubbles(model.settings, query));
}
} // namespace

int main() {
    ConfigDirectory config;

    // Preserve a sufficient authored foreground even when another role is stronger.
    auto p = publish(config, "#a8c7fa", "#062e6f", "#000000", "#000000", "#000000", "#000000");
    assert(near(p.onPrimary, hex("#062e6f")));
    assert(contrast(p.onSurface, p.primary) > contrast(p.onPrimary, p.primary));
    // A partial imported palette may retain the built-in dark on_error on a dark error plate.
    assert(near(themeColor("error"), hex("#ba1a1a")));
    assert(near(themeColor("on_error"), hex("#690005")));
    assert(contrast(themeColor("on_error"), p.error) < 4.5f);
    assert(contrast(p.onError, p.error) >= 4.5f);
    assert(p.onError.a == 1.f);

    // The reported light palette needs Scrim, not its insufficient surface roles.
    p = publish(config, "#5d65f5", "#dadcff", "#0e0e43", "#e4e4e7", "#000000", "#f3edf7");
    assert(p.onPrimary == p.scrim);
    assert(contrast(themeColor("on_primary"), p.primary) < 4.5f);
    assert(contrast(themeColor("on_surface"), p.primary) < 4.5f);
    assert(contrast(themeColor("inverse_on_surface"), p.primary) < 4.5f);

    // First sufficient role wins; do not choose the stronger black alternatives.
    p = publish(config, "#a8c7fa", "#a8c7fa", "#062e6f", "#000000", "#000000", "#000000");
    assert(p.onPrimary == p.onSurface);
    assert(contrast(p.scrim, p.primary) > contrast(p.onPrimary, p.primary));

    // Adjacent gray values straddle WCAG's 4.5:1 threshold on white.
    p = publish(config, "#ffffff", "#767676", "#000000", "#000000", "#000000", "#000000");
    assert(near(p.onPrimary, hex("#767676")));
    p = publish(config, "#ffffff", "#777777", "#000000", "#000000", "#000000", "#000000");
    assert(contrast(themeColor("on_primary"), p.primary) < 4.5f);
    assert(p.onPrimary == p.onSurface);

    // With no sufficient authored candidate, publish the SDK's readable foreground.
    p = publish(config, "#777777", "#777777", "#777777", "#777777", "#777777", "#777777");
    assert(p.onPrimary == readableTextColorForBackground(p.primary));

    // Foreground alpha matters: opaque black passes, translucent black does not.
    p = publish(config, "#ffffff", "#00000020", "#062e6f", "#000000", "#000000", "#000000");
    assert(contrast(withAlpha(themeColor("on_primary"), 1), p.primary) >= 4.5f);
    assert(contrast(themeColor("on_primary"), p.primary) < 4.5f);
    assert(p.onPrimary == p.onSurface);
    surfaces(config);
    presentation();
}
