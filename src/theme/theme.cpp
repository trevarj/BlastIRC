#include "theme.hpp"
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <map>
#include <render/animation/motion_service.h>
#include <stdexcept>
#include <string>
#include <toml++/toml.hpp>
#include <ui/palette.h>
#include <ui/style.h>

namespace blast {
namespace {
using RoleColors = std::map<std::string, Color, std::less<>>;
volatile std::sig_atomic_t reloadRequested = 0;
void reloadSignal(int) { reloadRequested = 1; }
Color parseColor(std::string_view value) {
    if (value.size() != 7 && value.size() != 9)
        throw std::runtime_error("expected #RRGGBB or #RRGGBBAA");
    return ::hex(value);
}
RoleColors builtin() {
    RoleColors p;
    const std::pair<const char *, const char *> colors[] = {
        {"primary", "#A8C7FA"},
        {"on_primary", "#062E6F"},
        {"primary_container", "#284777"},
        {"on_primary_container", "#D6E3FF"},
        {"secondary", "#A6D5BE"},
        {"on_secondary", "#103829"},
        {"secondary_container", "#2B4F40"},
        {"on_secondary_container", "#C1F1D9"},
        {"tertiary", "#D5BAF5"},
        {"on_tertiary", "#3B2458"},
        {"tertiary_container", "#523B70"},
        {"on_tertiary_container", "#EEDBFF"},
        {"hover", "#D5BAF5"},
        {"on_hover", "#3B2458"},
        {"error", "#FFB4AB"},
        {"on_error", "#690005"},
        {"error_container", "#93000A"},
        {"on_error_container", "#FFDAD6"},
        {"surface", "#14171E"},
        {"surface_variant", "#424751"},
        {"on_surface", "#E2E2E9"},
        {"on_surface_variant", "#C3C6D0"},
        {"background", "#10131A"},
        {"on_background", "#E2E2E9"},
        {"outline", "#8D919B"},
        {"outline_variant", "#424751"},
        {"shadow", "#000000"},
        {"scrim", "#000000"},
        {"inverse_surface", "#E2E2E9"},
        {"inverse_on_surface", "#2E3036"},
        {"inverse_primary", "#3F5F90"},
        {"surface_dim", "#10131A"},
        {"surface_bright", "#363941"},
        {"surface_container_lowest", "#0B0E14"},
        {"surface_container_low", "#191C23"},
        {"surface_container", "#1D2027"},
        {"surface_container_high", "#272A31"},
        {"surface_container_highest", "#32353C"},
        {"terminal_background", "#10131A"},
        {"terminal_foreground", "#E2E2E9"},
        {"terminal_selection_bg", "#284777"},
        {"terminal_selection_fg", "#D6E3FF"},
        {"terminal_cursor", "#A8C7FA"},
        {"terminal_normal_black", "#424751"},
        {"terminal_normal_red", "#EE8B95"},
        {"terminal_normal_green", "#99CCA7"},
        {"terminal_normal_yellow", "#D7C28B"},
        {"terminal_normal_blue", "#91B7E7"},
        {"terminal_normal_magenta", "#C5A1E5"},
        {"terminal_normal_cyan", "#88CED2"},
        {"terminal_normal_white", "#C3C6D0"},
        {"terminal_bright_black", "#8D919B"},
        {"terminal_bright_red", "#FFB4AB"},
        {"terminal_bright_green", "#B3E6C0"},
        {"terminal_bright_yellow", "#F3DDA6"},
        {"terminal_bright_blue", "#A8C7FA"},
        {"terminal_bright_magenta", "#D5BAF5"},
        {"terminal_bright_cyan", "#A5EDF0"},
        {"terminal_bright_white", "#E2E2E9"}};
    for (const auto &[name, value] : colors)
        p.emplace(name, parseColor(value));
    return p;
}
RoleColors current = builtin();
std::string source = "built-in";
TextFont uiFont{.family = "sans-serif", .features = {}},
    messageFont{.family = "sans-serif", .features = {}};
float userScale = 1, messageScale = 1, surfaceAlpha = 1;
std::uint64_t revision = 0;

Color pairedForeground(Color background, Color authored) {
    const float backgroundLuminance = relativeLuminance(background);
    const auto contrast = [background, backgroundLuminance](Color foreground) {
        const float luminance = relativeLuminance(lerpColor(background, foreground, foreground.a));
        return (std::max(luminance, backgroundLuminance) + .05f) /
               (std::min(luminance, backgroundLuminance) + .05f);
    };
    constexpr float minimumContrast = 4.5f;
    if (contrast(authored) >= minimumContrast)
        return authored;
    // Preserve imported accent plates and prefer a readable color from the same palette.
    for (const auto role : {"on_surface", "inverse_on_surface", "scrim", "shadow"}) {
        const auto candidate = themeColor(role);
        if (contrast(candidate) >= minimumContrast)
            return candidate;
    }
    return readableTextColorForBackground(background);
}

void publishPalette() {
    const auto color = themeForegroundColor;
    const auto fill = [](std::string_view role) { return themeSurfaceColor(role); };
    setResolvedThemeLight(relativeLuminance(color("surface")) > .179f);
    setPalette(::Palette{
        .primary = color("primary"),
        .onPrimary = withAlpha(pairedForeground(color("primary"), themeColor("on_primary")), 1.f),
        .primaryContainer = fill("primary_container"),
        .onPrimaryContainer = color("on_primary_container"),
        .secondary = color("secondary"),
        .onSecondary = color("on_secondary"),
        .secondaryContainer = fill("secondary_container"),
        .onSecondaryContainer = color("on_secondary_container"),
        .tertiary = color("tertiary"),
        .onTertiary = color("on_tertiary"),
        .tertiaryContainer = fill("tertiary_container"),
        .onTertiaryContainer = color("on_tertiary_container"),
        .error = color("error"),
        .onError = withAlpha(pairedForeground(color("error"), themeColor("on_error")), 1.f),
        .errorContainer = fill("error_container"),
        .onErrorContainer = color("on_error_container"),
        .surface = fill("surface"),
        .onSurface = color("on_surface"),
        .onSurfaceVariant = color("on_surface_variant"),
        .surfaceContainerLow = fill("surface_container_low"),
        .surfaceContainer = fill("surface_container"),
        .surfaceContainerHigh = fill("surface_container_high"),
        .surfaceContainerHighest = fill("surface_container_highest"),
        .outlineVariant = withAlpha(color("outline_variant"), .6f),
        .shadow = color("shadow"),
        .scrim = color("scrim"),
        .hover = fill("hover"),
        .onHover = color("on_hover"),
    });
}

void apply(const Settings &settings) {
    const float opacity = settings.transparency && std::isfinite(settings.opacity)
                              ? std::clamp(settings.opacity, .2f, 1.f)
                              : 1.f;
    if (surfaceAlpha != opacity) {
        surfaceAlpha = opacity;
        ++revision;
        publishPalette();
    }
    const std::string_view uiFamily = settings.uiFont.empty() ? std::string_view("sans-serif")
                                                              : std::string_view(settings.uiFont);
    const std::string_view messageFamily = settings.messageFont.empty()
                                               ? std::string_view("sans-serif")
                                               : std::string_view(settings.messageFont);
    if (uiFont.family != uiFamily)
        uiFont.family = uiFamily;
    if (messageFont.family != messageFamily)
        messageFont.family = messageFamily;
    uiFont.weight = static_cast<FontWeight>(settings.uiFontWeight);
    messageFont.weight = static_cast<FontWeight>(settings.messageFontWeight);
    userScale = std::isfinite(settings.scale) ? std::clamp(settings.scale, .7f, 2.f) : 1.f;
    messageScale =
        std::isfinite(settings.messageScale) ? std::clamp(settings.messageScale, .7f, 2.f) : 1.f;
    // Native controls take user zoom locally; the shared Noctalia radius is style-independent.
    Style::setCornerRadiusScale(1.f);
    Style::setRectangleCornerRadius(themeCornerRadius());
    Style::setButtonBordersEnabled(true);
    Style::setInputBordersEnabled(true);
    Style::setCardBordersEnabled(true);
    Style::setPopupBordersEnabled(true);
    Style::setPopupShadowsEnabled(true);
    MotionService::instance().setEnabled(settings.fx != FxLevel::Off && !settings.reducedMotion);
}
} // namespace
Color themeColor(std::string_view role) {
    const auto i = current.find(role);
    return i == current.end() ? current.find("on_surface")->second : i->second;
}
Color themeForegroundColor(std::string_view role) { return withAlpha(themeColor(role), 1.f); }
Color themePairedForeground(Color background, Color authored) {
    return pairedForeground(background, authored);
}
float themeSurfaceAlpha() { return surfaceAlpha; }
Color themeSurfaceFill(Color color) { return withAlpha(color, surfaceAlpha); }
Color themeSurfaceColor(std::string_view role) { return themeSurfaceFill(themeColor(role)); }
Color themeNickColor(std::uint32_t hash) {
    static constexpr const char *hues[] = {
        "terminal_normal_red",  "terminal_normal_green",   "terminal_normal_yellow",
        "terminal_normal_blue", "terminal_normal_magenta", "terminal_normal_cyan",
        "terminal_bright_red",  "terminal_bright_green",   "terminal_bright_yellow",
        "terminal_bright_blue", "terminal_bright_magenta", "terminal_bright_cyan"};
    return themeColor(hues[hash % std::size(hues)]);
}
const TextFont &themeUiFont() { return uiFont; }
const TextFont &themeMessageFont() { return messageFont; }
float themeScale() { return userScale; }
float themeMessageScale() { return messageScale; }
float themeCornerRadius() { return 12.f * userScale; }
float themeBubbleRadius() { return 16.f * userScale; }
std::uint64_t themeRevision() { return revision; }
const char *themeSource() { return source.c_str(); }
struct Theme::Impl {
    RoleColors from = current, to = current;
    std::filesystem::path path;
    std::filesystem::file_time_type stamp{};
    std::uintmax_t size = 0;
    double changed = -1, poll = -1;
    float progress = -1;
    bool existed = false;
    std::string selection;
    struct sigaction previous{};
    bool signalInstalled = false;
    Impl() {
        const char *xdg = std::getenv("XDG_CONFIG_HOME"), *home = std::getenv("HOME");
        path = (xdg && *xdg ? std::filesystem::path(xdg)
                            : std::filesystem::path(home ? home : ".") / ".config") /
               "blastirc" / "noctalia.toml";
        struct sigaction action{};
        action.sa_handler = reloadSignal;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;
        signalInstalled = sigaction(SIGUSR1, &action, &previous) == 0;
    }
    ~Impl() {
        if (signalInstalled)
            sigaction(SIGUSR1, &previous, nullptr);
    }
};
Theme::Theme() : impl(std::make_unique<Impl>()) { publishPalette(); }
Theme::~Theme() = default;
void Theme::update(Model &model) {
    apply(model.settings);
    auto &p = *impl;
    const bool requested = reloadRequested != 0;
    reloadRequested = 0;
    if (requested || p.poll < 0 || model.now - p.poll >= .25 ||
        p.selection != model.settings.themeSource) {
        p.poll = model.now;
        std::error_code ec;
        const bool exists = std::filesystem::is_regular_file(p.path, ec);
        const auto stamp = exists ? std::filesystem::last_write_time(p.path, ec)
                                  : std::filesystem::file_time_type{};
        const auto size = exists ? std::filesystem::file_size(p.path, ec) : 0;
        if (requested || p.selection != model.settings.themeSource || p.existed != exists ||
            p.stamp != stamp || p.size != size || p.changed < 0) {
            p.selection = model.settings.themeSource;
            p.existed = exists;
            p.stamp = stamp;
            p.size = size;
            try {
                RoleColors next = builtin();
                const bool forceBuiltin = p.selection == "builtin" || p.selection == "built-in";
                if (exists && !forceBuiltin) {
                    const auto file = toml::parse_file(p.path.string());
                    const auto *table = file["colors"].as_table();
                    if (!table)
                        throw std::runtime_error("missing [colors] table");
                    for (auto &[role, value] : next) {
                        if (const auto text = (*table)[role].value<std::string>())
                            value = parseColor(*text);
                    }
                    // Older templates omit hover roles; generated palettes traditionally use
                    // tertiary.
                    if (!(*table)["hover"].is_string())
                        next.at("hover") = next.at("tertiary");
                    if (!(*table)["on_hover"].is_string())
                        next.at("on_hover") = next.at("on_tertiary");
                    // Reject incomplete output instead of silently claiming a Noctalia theme.
                    for (auto required :
                         {"primary", "on_primary", "surface", "on_surface", "tertiary", "error"})
                        if (!(*table)[required].is_string())
                            throw std::runtime_error(std::string("missing color ") + required);
                    source = "Noctalia";
                } else
                    source = "built-in";
                p.from = current;
                p.to = std::move(next);
                p.changed = model.now;
                p.progress = -1;
            } catch (const std::exception &error) {
                source = "invalid palette (retained)";
                model.status = std::string("Theme: ") + error.what();
                p.from = current;
                p.to = current;
                p.changed = model.now;
                p.progress = -1;
            }
        }
    }
    const bool still = model.settings.fx == FxLevel::Off || model.settings.reducedMotion;
    const float t = still || p.progress == 1.f
                        ? 1.f
                        : std::clamp(float((model.now - p.changed) / .4), 0.f, 1.f);
    const float smooth = t * t * (3 - 2 * t);
    if (smooth != p.progress) {
        bool colorsChanged = false;
        for (auto &[role, value] : current) {
            const auto next = lerpColor(p.from.at(role), p.to.at(role), smooth);
            if (value != next) {
                value = next;
                colorsChanged = true;
            }
        }
        p.progress = smooth;
        if (colorsChanged) {
            ++revision;
            publishPalette();
        }
    }
}
} // namespace blast
