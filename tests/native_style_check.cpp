#include <array>
#include <cassert>
#include <cmath>
#include <optional>
#include <render/scene/rect_node.h>
#include <ui/controls/button.h>
#include <ui/palette.h>
#include <ui/style.h>

namespace {
float backgroundAlpha(const Button &button) {
    for (const auto &child : button.children())
        if (const auto *rect = dynamic_cast<const RectNode *>(child.get()))
            return rect->style().fill.a;
    assert(false);
    return 0;
}
void assertSurface(const Button &button, float alpha) {
    assert(std::abs(backgroundAlpha(button) - alpha) < 1e-5f);
    assert(button.foregroundColor().a == 1.f);
}
void buttonSurfaces() {
    const auto original = palette;
    for (const float alpha : std::array{1.f, .4f}) {
        auto colors = original;
        colors.surface.a = colors.surfaceContainer.a = colors.surfaceContainerHigh.a = alpha;
        colors.hover.a = alpha;
        colors.primary.a = colors.secondary.a = colors.error.a = 1.f;
        colors.onSurface.a = colors.onPrimary.a = colors.onSecondary.a = colors.onError.a =
            colors.onHover.a = 1.f;
        setPalette(colors);
        for (const auto variant :
             std::array{ButtonVariant::Default, ButtonVariant::Primary, ButtonVariant::Secondary,
                        ButtonVariant::Destructive, ButtonVariant::Outline, ButtonVariant::Ghost,
                        ButtonVariant::Tab, ButtonVariant::TabActive}) {
            Button button;
            button.setVariant(variant);
            assertSurface(button, variant == ButtonVariant::Ghost || variant == ButtonVariant::Tab
                                      ? 0.f
                                      : alpha);
            button.setHoveredVisual(true);
            assertSurface(button, alpha);
            button.setPressedVisual(true);
            assertSurface(button, alpha);
            button.setPressedVisual(false);
            button.setHoveredVisual(false);
            button.setSelected(true);
            assertSurface(button, alpha);
            button.setSelected(false);
            if (variant == ButtonVariant::Ghost || variant == ButtonVariant::Tab ||
                variant == ButtonVariant::TabActive) {
                button.setKeyboardFocusHint(true);
                assertSurface(button, alpha);
            }
            button.setSurfaceOpacity(.5f);
            button.setPressedVisual(true);
            assertSurface(button, alpha * .5f);
            button.setPressedVisual(false);
            button.setKeyboardFocusHint(false);
            button.setHoveredVisual(true);
            assertSurface(button, alpha * .5f);
            button.setHoveredVisual(false);
            if (variant != ButtonVariant::Ghost && variant != ButtonVariant::Tab)
                assertSurface(button, alpha * .5f);
            button.setSurfaceOpacity(1.f);
            button.setEnabled(false);
            const float disabled = variant == ButtonVariant::Ghost || variant == ButtonVariant::Tab
                                       ? 0.f
                                       : alpha * .55f;
            assert(std::abs(backgroundAlpha(button) - disabled) < 1e-5f);
        }
    }
    setPalette(original);
}
} // namespace

int main() {
    Style::setCornerRadiusScale(1.f);
    Style::setRectangleCornerRadius(std::nullopt);
    constexpr std::array localScales{1.f, 14.f / 13.f};
    for (const float localScale : localScales) {
        assert(Style::scaledRadiusMd(localScale) == Style::radiusMd * localScale);
        assert(Style::scaledRadiusLg(localScale) == Style::radiusLg * localScale);
        assert(Style::scaledRadiusXl(localScale) == Style::radiusXl * localScale);
    }
    for (const float zoom : std::array{.7f, 1.f, 2.f}) {
        const float radius = Style::radiusMd * zoom;
        Style::setRectangleCornerRadius(radius);
        for (const float localScale : localScales) {
            assert(Style::scaledRadiusMd(localScale) == radius);
            assert(Style::scaledRadiusLg(localScale) == radius);
            assert(Style::scaledRadiusXl(localScale) == radius);
            // Semantic progress caps and explicit full/zero geometry stay independent.
            assert(Style::scaledRadiusSm(localScale) == Style::radiusSm * localScale);
            assert(Style::scaledRadius(0.f, localScale) == 0.f);
            constexpr float fullRadius = 1000.f;
            assert(Style::scaledRadius(fullRadius, localScale) == fullRadius * localScale);
        }
    }
    Style::setRectangleCornerRadius(std::nullopt);
    buttonSurfaces();
}
