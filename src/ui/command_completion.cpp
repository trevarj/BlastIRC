#include "command_completion.hpp"

#include "../render/graphics.hpp"
#include "../theme/theme.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <render/core/renderer.h>
#include <render/scene/input_area.h>
#include <render/scene/rect_node.h>
#include <string>
#include <ui/controls/label.h>
#include <ui/controls/scroll_view.h>
#include <utility>

namespace blast::command_completion {
namespace {
template <class T, class... Args> T *child(Node &parent, Args &&...args) {
    auto node = std::make_unique<T>(std::forward<Args>(args)...);
    auto *result = node.get();
    parent.addChild(std::move(node));
    return result;
}

void preserveFocus(Node &node) {
    if (auto *area = dynamic_cast<InputArea *>(&node)) {
        area->setFocusable(false);
        area->setTabStop(false);
        area->setFocusOnPointerPress(false);
    }
    for (const auto &item : node.children())
        preserveFocus(*item);
}

void cancelPresses(Node &node) {
    if (auto *area = dynamic_cast<InputArea *>(&node))
        area->dispatchCancel();
    for (const auto &item : node.children())
        cancelPresses(*item);
}

class Row final : public InputArea {
  public:
    Row() {
        setFocusable(false);
        setTabStop(false);
        setFocusOnPointerPress(false);
        // Immediate touch-down makes dispatchCancel invalidate a held tap on rebind.
        setPassiveTouchPress(true);
        setClipChildren(true);
        background = child<RectNode>(*this);
        background->setHitTestVisible(false);
        icon = child<LucideIcon>(*this, "terminal");
        icon->setHitTestVisible(false);
        command = child<Label>(*this);
        description = child<Label>(*this);
        for (auto *label : {command, description}) {
            label->setUseMarkup(false);
            label->setMaxLines(1);
            label->setHitTestVisible(false);
        }
        command->setFontFamily("monospace");
        setOnEnter([this](const auto &) { paint(); });
        setOnLeave([this] { paint(); });
        setOnPress([this](const auto &) { paint(); });
        setOnCancel([this] { paint(); });
    }

    void bind(Match match, bool selected) {
        active = selected;
        const bool changedMode = literal != match.literal;
        literal = match.literal;
        if (changedMode || caption.empty() ||
            std::string_view(caption).substr(literal ? 0 : 1) != match.name) {
            caption.assign(literal ? "" : "/");
            caption.append(match.name);
            command->setText(caption);
        }
        description->setText(match.description);
        icon->setVisible(!literal);
        paint();
    }

    void style(float scale, const TextFont &font) {
        zoom = scale;
        if (description->font().family != font.family)
            description->setFontFamily(font.family);
        for (auto *label : {command, description}) {
            if (label->font().weight != font.weight)
                label->setFontWeight(font.weight);
            if (label->font().features != font.features)
                label->setFontFeatures(font.features);
            label->setFontSize(13 * scale);
        }
        paint();
        markLayoutDirty();
    }

  protected:
    LayoutSize doMeasure(Renderer &, const LayoutConstraints &constraints) override {
        return constraints.constrain(
            {constraints.hasMaxWidth ? constraints.maxWidth : 420 * zoom, 32 * zoom});
    }

    void doLayout(Renderer &renderer) override {
        background->setSize(width(), height());
        const float iconSize = 16 * zoom;
        icon->setPosition(8 * zoom, (height() - iconSize) / 2);
        icon->setSize(iconSize, iconSize);
        icon->layout(renderer);
        const float textX = (literal ? 8 : 32) * zoom;
        const float available = std::max(0.f, width() - textX - 8 * zoom);
        command->setMaxWidth(literal ? available : available * .45f);
        command->measure(renderer);
        command->setPosition(textX, (height() - command->height()) / 2);
        const float descriptionX = textX + command->width() + 12 * zoom;
        description->setMaxWidth(std::max(0.f, width() - descriptionX - 8 * zoom));
        description->measure(renderer);
        description->setPosition(descriptionX, (height() - description->height()) / 2);
    }

  private:
    void paint() {
        const bool highlight = active || pressed();
        const bool hover = hovered() && !highlight;
        RoundedRectStyle style{};
        style.fill = highlight ? themeSurfaceColor("primary_container")
                     : hover   ? themeSurfaceColor("hover")
                               : Color{0, 0, 0, 0};
        style.radius = themeCornerRadius();
        background->setStyle(style);
        // Resolve the translucent popup over the logical surface, then the row over the popup.
        const auto popup = themeSurfaceColor("surface_container_high");
        const auto plate = lerpColor(themeForegroundColor("surface"), popup, popup.a);
        const auto rowBackground = withAlpha(lerpColor(plate, style.fill, style.fill.a), 1.f);
        const auto accent = themePairedForeground(
            rowBackground, themeForegroundColor(highlight ? "on_primary_container" : "primary"));
        icon->setColor(accent);
        // Neutral text keeps selected names legible without relying on a marginal accent pair.
        const auto text =
            highlight ? themePairedForeground(rowBackground, themeForegroundColor("on_surface"))
                      : accent;
        command->setColor(text);
        description->setColor(
            highlight ? text
                      : themePairedForeground(
                            rowBackground,
                            themeForegroundColor(hover ? "on_hover" : "on_surface_variant")));
    }

    RectNode *background;
    LucideIcon *icon;
    Label *command, *description;
    std::string caption;
    float zoom = 1;
    bool active = false;
    bool literal = false;
};
} // namespace

struct View::Impl {
    View &view;
    ScrollView *scroll;
    std::array<Row *, maxResults> rows{};
    std::size_t count = 0, selected = 0;
    LayoutRect anchor{}, viewport{};
    float scale = 1;
    TextFont font{};
    std::uint64_t revision = 0;
    bool styled = false;
    std::function<void(std::size_t)> onChoose;

    explicit Impl(View &owner) : view(owner), scroll(child<ScrollView>(owner)) {
        scroll->content()->setAlign(FlexAlign::Stretch);
        scroll->content()->setGap(0);
        scroll->setReserveScrollbarGutter(true);
        for (std::size_t i = 0; i < rows.size(); ++i) {
            rows[i] = child<Row>(*scroll->content());
            rows[i]->setVisible(false);
            rows[i]->setOnClick([this, i](const auto &) {
                if (view.visible() && i < count && onChoose)
                    onChoose(i);
            });
        }
        preserveFocus(view);
    }

    void place(bool reveal = false) {
        const float margin =
            std::max(0.f, std::min({4 * scale, viewport.width / 4, viewport.height / 4}));
        const float left = viewport.x + margin, top = viewport.y + margin;
        const float right = viewport.x + viewport.width - margin;
        const float bottom = viewport.y + viewport.height - margin;
        const float edge = std::clamp(anchor.y - margin, top, std::max(top, bottom));
        const float height = std::min(count * 32 * scale + 10 * scale, edge - top);
        const float width = std::max(0.f, std::min(480 * scale, right - left));
        view.setPosition(std::clamp(anchor.x, left, std::max(left, right - width)), edge - height);
        const bool resized = view.width() != width || view.height() != height;
        view.setSize(width, height);
        if (count && (resized || reveal))
            scroll->requestScrollIntoView(rows[selected]);
        if (width <= 0 || height <= 0)
            view.setVisible(false);
    }
};

View::View() : impl_(std::make_unique<Impl>(*this)) {
    setParticipatesInLayout(false);
    setExcludeSubtreeFromTabOrder(true);
    setClipChildren(true);
    setZIndex(90);
    Node::setVisible(false);
}
View::~View() = default;

void View::setMatches(std::span<const Match> matches, std::size_t selected) {
    auto &state = *impl_;
    // Rebinding always retires held presses, including identical match/selection updates.
    cancelPresses(*state.scroll);
    state.count = std::min(matches.size(), maxResults);
    state.selected = state.count ? std::min(selected, state.count - 1) : 0;
    for (std::size_t i = 0; i < maxResults; ++i) {
        if (i < state.count)
            state.rows[i]->bind(matches[i], i == state.selected);
        state.rows[i]->setVisible(i < state.count);
    }
    if (!state.count)
        setVisible(false);
    state.place(true);
}

void View::setAnchor(LayoutRect anchor, LayoutRect viewport, float scale, const TextFont &font) {
    auto &state = *impl_;
    state.anchor = anchor;
    state.viewport = viewport;
    scale = std::max(.1f, scale);
    const auto revision = themeRevision();
    const bool restyled =
        !state.styled || state.scale != scale || state.font != font || state.revision != revision;
    if (restyled) {
        state.styled = true;
        state.scale = scale;
        state.font = font;
        state.revision = revision;
        for (auto *row : state.rows)
            row->style(scale, font);
        state.scroll->setContentScale(scale);
        state.scroll->setViewportPaddingH(4 * scale);
        state.scroll->setViewportPaddingV(4 * scale);
        state.scroll->setFill(themeSurfaceColor("surface_container_high"));
        state.scroll->setBorder(themeForegroundColor("outline_variant"), scale);
        state.scroll->setRadius(themeCornerRadius());
        state.scroll->setScrollbarInsetV(4 * scale);
    }
    state.place(restyled);
}

void View::setOnChoose(std::function<void(std::size_t)> callback) {
    impl_->onChoose = std::move(callback);
}

void View::setVisible(bool visible) {
    const bool show = visible && impl_->count && width() > 0 && height() > 0;
    if (!show)
        cancelPresses(*impl_->scroll);
    Node::setVisible(show);
}

void View::doLayout(Renderer &renderer) {
    impl_->scroll->setSize(width(), height());
    impl_->scroll->layout(renderer);
}
} // namespace blast::command_completion
