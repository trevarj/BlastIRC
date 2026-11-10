#pragma once
#include "../model.hpp"
#include <cstdint>
#include <memory>
#include <render/core/color.h>
#include <render/core/renderer.h>
#include <string_view>

namespace blast {
// Raw, currently morphed Material 3 and terminal roles preserve authored alpha.
Color themeColor(std::string_view role);
// UI foregrounds stay opaque regardless of authored alpha or surface transparency.
Color themeForegroundColor(std::string_view role);
// Preserve a foreground when readable, otherwise repair to >=4.5:1 on an opaque background.
Color themePairedForeground(Color background, Color authored);
Color themeNickColor(std::uint32_t hash);
// Background alpha is opt-in and never applied to a control's text or children.
float themeSurfaceAlpha();
Color themeSurfaceFill(Color);
Color themeSurfaceColor(std::string_view role);
// Font families include explicit overrides and the current presentation defaults.
const TextFont &themeUiFont();
const TextFont &themeMessageFont();
// User zoom only: native render scale belongs to the renderer, never logical metrics.
float themeScale();
float themeMessageScale();
// Shared control and conversation-bubble radii in user-scaled native logical units.
float themeCornerRadius();
float themeBubbleRadius();
// Changes when a palette role or the shared background opacity changes.
std::uint64_t themeRevision();
const char *themeSource();
class Theme {
  public:
    Theme();
    ~Theme();
    void update(Model &);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace blast
