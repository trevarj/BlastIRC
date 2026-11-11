#include "emoji_picker.hpp"

#include "../theme/theme.hpp"
#include <algorithm>
#include <array>
#include <core/input/key_modifiers.h>
#include <core/input/key_symbols.h>
#include <render/scene/input_dispatcher.h>
#include <ui/controls/box.h>
#include <ui/controls/input.h>
#include <ui/controls/label.h>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

namespace blast::emoji {
namespace {
template <class T> T *child(Node &parent) {
    auto item = std::make_unique<T>();
    auto *result = item.get();
    parent.addChild(std::move(item));
    return result;
}
void passive(InputArea &area) {
    area.setFocusable(false);
    area.setTabStop(false);
    area.setFocusOnPointerPress(false);
    area.setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT, BTN_MIDDLE}));
    area.setOnAxisHandler([](const auto &) { return true; });
}
} // namespace

struct Picker::Impl {
    Picker &owner;
    Catalog &catalog;
    InputDispatcher *dispatcher = nullptr;
    InputArea *blocker, *panel;
    Box *background;
    Label *title, *empty;
    Input *query;
    CompletionView *results;
    // Catalog's result storage is shared with composer completion; strings remain catalog-owned.
    std::array<Match, maxResults> matches{};
    std::size_t count = 0, selected = 0;
    LayoutRect anchor{};
    float scale = 1;
    std::function<void(std::string)> onChoose;
    std::function<void()> onCancel;

    Impl(Picker &view, Catalog &source) : owner(view), catalog(source) {
        blocker = child<InputArea>(owner);
        passive(*blocker);
        blocker->setOnClick([this](const auto &) { cancel(); });
        panel = child<InputArea>(owner);
        passive(*panel);
        panel->setClipChildren(true);
        background = child<Box>(*panel);
        background->setHitTestVisible(false);
        title = child<Label>(*panel);
        title->setUseMarkup(false);
        title->setMaxLines(1);
        title->setHitTestVisible(false);
        query = child<Input>(*panel);
        query->setMinLayoutWidth(0);
        query->setPlaceholder("Search emoji…");
        query->inputArea()->setTooltip(
            "Search reactions · Up/Down select · Enter react · Escape cancel");
        query->setOnChange([this](const auto &) { refresh(); });
        query->setOnSubmit([this](const auto &) { choose(selected); });
        query->setOnKeyEvent([this](std::uint32_t sym, std::uint32_t modifiers) {
            // Full KeyData routing guards composition before dispatch; this legacy
            // hook cannot distinguish dead-key previews from non-text commands.
            if (query->composing())
                return false;
            InputArea::KeyData key{};
            key.sym = sym;
            key.utf32 = xkb_keysym_to_utf32(sym);
            key.modifiers = modifiers;
            key.pressed = true;
            return owner.handleKey(key);
        });
        results = child<CompletionView>(*panel);
        results->setOnChoose([this](std::size_t index) { choose(index); });
        empty = child<Label>(*panel);
        empty->setText("No matching emoji");
        empty->setUseMarkup(false);
        empty->setMaxLines(1);
        empty->setHitTestVisible(false);
    }

    void refresh() {
        const auto found = catalog.matches(query->value());
        count = std::min(found.size(), matches.size());
        std::copy_n(found.begin(), count, matches.begin());
        selected = 0;
        bind();
    }
    void bind() {
        results->setMatches({matches.data(), count}, selected);
        results->setVisible(owner.visible() && count);
        empty->setVisible(!count);
        owner.markLayoutDirty();
    }
    void choose(std::size_t index) {
        if (!owner.active() || query->composing() || index >= count)
            return;
        std::string glyph(matches[index].glyph);
        auto callback = std::move(onChoose);
        owner.hide();
        if (callback)
            callback(std::move(glyph));
    }
    void cancel() {
        if (!owner.active())
            return;
        auto callback = std::move(onCancel);
        owner.hide();
        if (callback)
            callback();
    }
    void geometry() {
        const float margin = std::min({8 * scale, owner.width() / 8, owner.height() / 8});
        const float w = std::max(0.f, std::min(384 * scale, owner.width() - 2 * margin));
        const float h = std::max(0.f, std::min(450 * scale, owner.height() - 2 * margin));
        const bool centered = anchor.width <= 0 && anchor.height <= 0;
        const float x = centered ? (owner.width() - w) / 2 : anchor.x;
        const float y = centered ? (owner.height() - h) / 2 : anchor.y + anchor.height;
        panel->setPosition(std::clamp(x, margin, std::max(margin, owner.width() - w - margin)),
                           std::clamp(y, margin, std::max(margin, owner.height() - h - margin)));
        panel->setSize(w, h);
        blocker->setSize(owner.width(), owner.height());
        background->setSize(w, h);
        background->setFill(themeSurfaceColor("surface_container_high"));
        background->setBorder(themeForegroundColor("outline_variant"), scale);
        background->setRadius(themeCornerRadius());
        const auto &font = themeUiFont();
        for (auto *label : {title, empty}) {
            label->setFontFamily(font.family);
            label->setFontFeatures(font.features);
            label->setFontWeight(font.weight);
            label->setFontSize(14 * scale);
            label->setColor(themeForegroundColor("on_surface"));
        }
        query->setFontFamily(font.family);
        query->setFontFeatures(font.features);
        query->setFontWeight(font.weight);
        query->setFontSize(14 * scale);
        query->setControlHeight(36 * scale);
        query->setHorizontalPadding(8 * scale);
        const float pad = std::min(12 * scale, w / 8);
        const float inner = std::max(0.f, w - 2 * pad);
        title->setPosition(pad, pad);
        title->setMaxWidth(inner);
        query->setPosition(pad, 36 * scale);
        query->setSize(inner, 36 * scale);
        const float top = 80 * scale;
        const LayoutRect viewport{pad, top, inner, std::max(0.f, h - top - pad)};
        results->setAnchor({pad, top, 0, 0}, viewport, scale, font);
        results->setVisible(owner.active() && count);
        empty->setPosition(pad, top);
        empty->setMaxWidth(inner);
    }
};

Picker::Picker(Catalog &catalog) : impl_(std::make_unique<Impl>(*this, catalog)) {
    setParticipatesInLayout(false);
    setZIndex(1080);
    setVisible(false);
    setHitTestVisible(false);
}
Picker::~Picker() = default;
void Picker::setInputDispatcher(InputDispatcher &dispatcher) { impl_->dispatcher = &dispatcher; }
void Picker::show(std::string_view title, LayoutRect anchor,
                  std::function<void(std::string)> choose, std::function<void()> cancel) {
    auto &state = *impl_;
    hide();
    state.anchor = anchor;
    state.onChoose = std::move(choose);
    state.onCancel = std::move(cancel);
    state.title->setText(title);
    state.query->setValue("");
    setVisible(true);
    setHitTestVisible(true);
    state.refresh();
    state.geometry();
    if (state.dispatcher)
        state.dispatcher->setFocus(state.query->inputArea());
}
void Picker::hide() {
    auto &state = *impl_;
    state.results->setVisible(false);
    state.blocker->dispatchCancel();
    state.panel->dispatchCancel();
    if (state.dispatcher && state.dispatcher->focusedArea() == state.query->inputArea())
        state.dispatcher->setFocus(nullptr);
    setVisible(false);
    setHitTestVisible(false);
    state.onChoose = {};
    state.onCancel = {};
}
bool Picker::active() const { return visible(); }
void Picker::sync(float width, float height, float scale) {
    setSize(std::max(0.f, width), std::max(0.f, height));
    impl_->scale = std::max(.1f, scale);
    if (active())
        impl_->geometry();
}
bool Picker::handleKey(const InputArea::KeyData &key) {
    if (!active() || !key.pressed)
        return false;
    auto &state = *impl_;
    if (KeySymbol::isModifier(key.sym))
        return true;
    const auto modifiers =
        key.modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super | KeyMod::Shift);
    const bool ctrl = modifiers == KeyMod::Ctrl;
    if ((!modifiers && key.sym == XKB_KEY_Escape) ||
        (ctrl && (key.sym == XKB_KEY_g || key.sym == XKB_KEY_G))) {
        // Cancel private IME preedit first; a second key dismisses the picker.
        if (state.query->composing()) {
            state.query->textInputResetPreedit();
            state.refresh();
            return true;
        }
        state.cancel();
        return true;
    }
    // Preserve native IME text: panel commands must never commit/erase unfinished preedit.
    if (key.preedit)
        return false;
    if (state.query->composing()) {
        const bool printable = key.utf32 >= 0x20 && key.utf32 != 0x7f;
        return !printable || (modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super));
    }
    const bool previous = (!modifiers && key.sym == XKB_KEY_Up) ||
                          (ctrl && (key.sym == XKB_KEY_p || key.sym == XKB_KEY_P));
    const bool next = (!modifiers && key.sym == XKB_KEY_Down) ||
                      (ctrl && (key.sym == XKB_KEY_n || key.sym == XKB_KEY_N));
    if (previous || next) {
        if (state.count) {
            state.selected = previous ? (state.selected + state.count - 1) % state.count
                                      : (state.selected + 1) % state.count;
            state.bind();
        }
        return true;
    }
    if (!modifiers && (key.sym == XKB_KEY_Return || key.sym == XKB_KEY_KP_Enter)) {
        state.choose(state.selected);
        return true;
    }
    // Search is the only focusable control; do not tab into the obscured conversation.
    return key.sym == XKB_KEY_Tab || key.sym == XKB_KEY_ISO_Left_Tab;
}
Input &Picker::searchInput() { return *impl_->query; }
void Picker::doLayout(Renderer &renderer) {
    impl_->geometry();
    impl_->title->measure(renderer);
    impl_->empty->measure(renderer);
    impl_->blocker->layout(renderer);
    impl_->panel->layout(renderer);
}
} // namespace blast::emoji
