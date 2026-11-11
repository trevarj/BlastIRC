#include "ui/style.h"

#include <algorithm>

namespace {

  float g_cornerRadiusScale = 1.0F;
  std::optional<float> g_rectangleCornerRadius;
  bool g_buttonBordersEnabled = true;
  bool g_inputBordersEnabled = true;
  bool g_popupBordersEnabled = true;
  bool g_rtl = false;
  bool g_popupShadowsEnabled = true;
  bool g_cardBordersEnabled = true;
  bool g_closeButtonsEnabled = true;

} // namespace

namespace Style {

  float cornerRadiusScale() noexcept { return g_cornerRadiusScale; }

  void setCornerRadiusScale(float scale) noexcept { g_cornerRadiusScale = std::clamp(scale, 0.0F, 2.0F); }

  void setRectangleCornerRadius(std::optional<float> radius) noexcept {
    g_rectangleCornerRadius = radius;
  }

  bool buttonBordersEnabled() noexcept { return g_buttonBordersEnabled; }

  void setButtonBordersEnabled(bool enabled) {
    if (g_buttonBordersEnabled == enabled) {
      return;
    }
    g_buttonBordersEnabled = enabled;
    buttonBordersChanged().emit();
  }

  Signal<>& buttonBordersChanged() {
    static Signal<> signal;
    return signal;
  }

  bool inputBordersEnabled() noexcept { return g_inputBordersEnabled; }

  void setInputBordersEnabled(bool enabled) {
    if (g_inputBordersEnabled == enabled) {
      return;
    }
    g_inputBordersEnabled = enabled;
    inputBordersChanged().emit();
  }

  Signal<>& inputBordersChanged() {
    static Signal<> signal;
    return signal;
  }

  bool popupBordersEnabled() noexcept { return g_popupBordersEnabled; }
  void setPopupBordersEnabled(bool enabled) { g_popupBordersEnabled = enabled; }

  bool rtl() noexcept { return g_rtl; }
  void setRtl(bool rtl) noexcept { g_rtl = rtl; }
  bool cardBordersEnabled() noexcept { return g_cardBordersEnabled; }
  void setCardBordersEnabled(bool enabled) { g_cardBordersEnabled = enabled; }

  bool popupShadowsEnabled() noexcept { return g_popupShadowsEnabled; }
  void setPopupShadowsEnabled(bool enabled) { g_popupShadowsEnabled = enabled; }

  bool closeButtonsEnabled() noexcept { return g_closeButtonsEnabled; }
  void setCloseButtonsEnabled(bool enabled) { g_closeButtonsEnabled = enabled; }

  float scaledRadius(float radius, float localScale) noexcept { return radius * localScale * g_cornerRadiusScale; }

  float scaledRadiusSm(float localScale) noexcept { return scaledRadius(radiusSm, localScale); }

  float scaledRadiusMd(float localScale) noexcept {
    return g_rectangleCornerRadius ? *g_rectangleCornerRadius : scaledRadius(radiusMd, localScale);
  }

  float scaledRadiusLg(float localScale) noexcept {
    return g_rectangleCornerRadius ? *g_rectangleCornerRadius : scaledRadius(radiusLg, localScale);
  }

  float scaledRadiusXl(float localScale) noexcept {
    return g_rectangleCornerRadius ? *g_rectangleCornerRadius : scaledRadius(radiusXl, localScale);
  }

} // namespace Style
