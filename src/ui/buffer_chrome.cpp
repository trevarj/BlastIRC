#include "buffer_chrome.hpp"
#include "../model.hpp"
#include "../render/graphics.hpp"
#include "../theme/theme.hpp"
#include "channel_avatar.hpp"
#include "irc_format.hpp"
#include "ui.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <core/input/key_modifiers.h>
#include <map>
#include <render/core/renderer.h>
#include <render/scene/input_dispatcher.h>
#include <string>
#include <ui/builders.h>
#include <ui/controls/glyph.h>
#include <ui/controls/input.h>
#include <ui/controls/spinner.h>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace blast {
namespace {
void place(Node &node, float x, float y, float width, float height) {
    node.setPosition(x, y);
    node.setSize(std::max(0.f, width), std::max(0.f, height));
}
float hitExtent(float scale) { return std::max(32.f, 32 * scale); }
void labelStyle(Label &label, float size, std::string_view role) {
    if (label.font().family != themeUiFont().family)
        label.setFontFamily(themeUiFont().family);
    if (label.font().features != themeUiFont().features)
        label.setFontFeatures(themeUiFont().features);
    if (label.font().weight != themeUiFont().weight)
        label.setFontWeight(themeUiFont().weight);
    label.setFontSize(size);
    label.setColor(themeForegroundColor(role));
    label.setBaselineMode(LabelBaselineMode::TextFixedHeight);
}
void placeLabel(Label &label, Renderer &renderer, float x, float y, float width, float height) {
    label.setMaxWidth(std::max(1.f, width));
    label.measure(renderer);
    label.setPosition(x, y + (height - label.height()) / 2);
}
class ChromeButton final : public InputArea {
  public:
    ChromeButton(std::string_view icon,
                 std::function<void(Node &, std::optional<PointerData>)> activate)
        : activate_(std::move(activate)) {
        setFocusable(true);
        background_ = static_cast<Box *>(addChild(std::make_unique<Box>()));
        background_->setHitTestVisible(false);
        icon_ = static_cast<LucideIcon *>(addChild(std::make_unique<LucideIcon>(icon, 18)));
        icon_->setHitTestVisible(false);
        setOnClick([this](const PointerData &pointer) { activate_(*this, pointer); });
        setOnKeyDown([this](const KeyData &key) {
            if (key.sym == XKB_KEY_Return || key.sym == XKB_KEY_KP_Enter ||
                key.sym == XKB_KEY_space)
                activate_(*this, std::nullopt);
        });
        setOnEnter([this](const PointerData &) { style(); });
        setOnLeave([this] { style(); });
        setOnPress([this](const PointerData &) { style(); });
        setOnFocusGain([this] { style(); });
        setOnFocusLoss([this] { style(); });
    }
    void sync(float scale, bool selected = false) {
        scale_ = scale;
        selected_ = selected;
        style();
    }
    void hint(std::string_view value) {
        if (hint_ != value) {
            hint_ = value;
            setTooltip(hint_);
        }
    }

  protected:
    void doLayout(Renderer &renderer) override {
        place(*background_, 0, 0, width(), height());
        const float extent = std::min({18 * scale_, width(), height()});
        place(*icon_, (width() - extent) / 2, (height() - extent) / 2, extent, extent);
        InputArea::doLayout(renderer);
    }

  private:
    void style() {
        const bool primary = selected_ || pressed();
        const bool hover = hovered() && !primary;
        background_->setFill(primary ? themeSurfaceColor("primary_container")
                             : hover ? themeSurfaceColor("hover")
                                     : Color{0, 0, 0, 0});
        background_->setRadius(themeCornerRadius());
        if (focused())
            background_->setBorder(themeForegroundColor("secondary"), std::max(1.f, scale_));
        else
            background_->clearBorder();
        icon_->setColor(
            hover ? themeForegroundColor("on_hover")
                  : themeForegroundColor(primary ? "on_primary_container" : "on_surface"));
    }
    Box *background_;
    LucideIcon *icon_;
    std::function<void(Node &, std::optional<PointerData>)> activate_;
    std::string hint_;
    float scale_ = 1;
    bool selected_ = false;
};
} // namespace

struct BufferTitleLine::Impl {
    BufferTitleLine &owner;
    Model &model;
    BufferChromeCallbacks callbacks;
    InputDispatcher *dispatcher = nullptr;
    InputArea *identity;
    Box *networkChip;
    Box *avatarBackground;
    Image *avatarImage;
    Label *avatarInitials;
    NativeAvatarLabelPlacement avatarInitialsPlacement;
    Label *title, *network;
    InputArea *historyArea;
    Glyph *historyIcon;
    Label *historyStatus;
    Spinner *historySpinner;
    HistorySyncState historySync = HistorySyncState::Idle;
    std::array<ChromeButton *, 3> tools{};
    ChromeButton *topic;
    std::uint64_t pane = 0;
    float scale = 1;
    bool compact = false, active = false;
    SidebarMode navigationMode = SidebarMode::Expanded;
    bool channel = false, serverLog = false;
    bool hiddenTools = false;
    float measuredScale = 0, measuredRenderScale = 0;
    std::uint64_t measuredGeneration = 0;
    TextFont measuredFont;
    float titleNaturalWidth = 0, readableTitleWidth = 0, minimumTitleWidth = 0;
    float networkNaturalWidth = 0, networkInitialsWidth = 0;
    std::string networkInitials;
    SidebarMode cachedPeoplePreferred = SidebarMode::Hidden;
    SidebarMode cachedPeopleEffective = SidebarMode::Hidden;
    std::string cachedBuffer, cachedTitle, cachedNetwork, cachedTopic, tooltip;
    std::string avatarSource, loadedAvatarSource;
    int loadedAvatarSize = 0;
    std::map<std::string, std::string> bindings;

    Impl(BufferTitleLine &node, Model &state, BufferChromeCallbacks actions)
        : owner(node), model(state), callbacks(std::move(actions)) {
        owner.setClipChildren(true);
        identity = static_cast<InputArea *>(owner.addChild(std::make_unique<InputArea>()));
        identity->setFocusable(false);
        identity->setTabStop(false);
        identity->setFocusOnPointerPress(false);
        identity->setClipChildren(true);
        avatarBackground = static_cast<Box *>(identity->addChild(std::make_unique<Box>()));
        avatarBackground->setHitTestVisible(false);
        avatarImage = static_cast<Image *>(identity->addChild(std::make_unique<Image>()));
        avatarImage->setFit(ImageFit::Cover);
        avatarImage->setHitTestVisible(false);
        avatarImage->setVisible(false);
        avatarInitials = static_cast<Label *>(identity->addChild(ui::label({.text = ""})));
        avatarInitials->setUseMarkup(false);
        avatarInitials->setMaxLines(1);
        avatarInitials->setHitTestVisible(false);
        title = static_cast<Label *>(identity->addChild(ui::label({.text = "Conversation"})));
        networkChip = static_cast<Box *>(identity->addChild(std::make_unique<Box>()));
        networkChip->setHitTestVisible(false);
        network = static_cast<Label *>(identity->addChild(ui::label({.text = ""})));
        for (auto *label : {title, network}) {
            label->setMaxLines(1);
            label->setEllipsize(TextEllipsize::End);
            label->setUseMarkup(false);
            label->setHitTestVisible(false);
        }
        network->setFocusable(false);
        network->setTabStop(false);
        network->setAcceptedButtons(0);
        historyArea = static_cast<InputArea *>(owner.addChild(std::make_unique<InputArea>()));
        historyArea->setFocusable(false);
        historyArea->setTabStop(false);
        historyArea->setAcceptedButtons(0);
        historyArea->setVisible(false);
        historyStatus = static_cast<Label *>(historyArea->addChild(ui::label({.text = ""})));
        historyStatus->setUseMarkup(false);
        historyStatus->setMaxLines(1);
        historyStatus->setEllipsize(TextEllipsize::End);
        historyStatus->setHitTestVisible(false);
        historyStatus->setVisible(false);
        historyIcon = static_cast<Glyph *>(historyArea->addChild(std::make_unique<Glyph>()));
        historyIcon->setGlyph("history");
        historyIcon->setHitTestVisible(false);
        historySpinner = static_cast<Spinner *>(historyArea->addChild(std::make_unique<Spinner>()));
        historySpinner->setHitTestVisible(false);
        historySpinner->setVisible(false);
        constexpr std::array<std::string_view, 3> icons = {"users", "search", "ellipsis"};
        for (std::size_t i = 0; i < tools.size(); ++i) {
            auto tool = std::make_unique<ChromeButton>(
                icons[i], [this, i](Node &anchor, std::optional<InputArea::PointerData> pointer) {
                    if (i == 0)
                        callbacks.command(pane, "toggle-people");
                    else if (i == 1)
                        callbacks.command(pane, "search");
                    else
                        callbacks.paneMenu(pane, anchor, hiddenTools, pointer);
                });
            if (i == 0)
                tool->setFocusOnPointerPress(false);
            tools[i] = static_cast<ChromeButton *>(owner.addChild(std::move(tool)));
        }
        topic = static_cast<ChromeButton *>(owner.addChild(std::make_unique<ChromeButton>(
            "info", [this](Node &anchor, std::optional<InputArea::PointerData> pointer) {
                callbacks.topic(pane, anchor, pointer);
            })));
        topic->hint("Channel topic");
    }
    void sync(const PaneState &state, bool focused, float userScale, SidebarMode peopleEffective,
              SidebarMode navigationEffective) {
        const bool changedPane = pane != state.id;
        pane = state.id;
        active = focused;
        if (scale != userScale || navigationMode != navigationEffective) {
            scale = userScale;
            navigationMode = navigationEffective;
            owner.markLayoutDirty();
        }
        if (changedPane) {
            for (std::size_t i = 0; i < tools.size(); ++i)
                tools[i]->setTabFocusKey("buffer-tool:" + std::to_string(pane) + ":" +
                                         std::to_string(i));
            topic->setTabFocusKey("buffer-tool:" + std::to_string(pane) + ":topic");
        }
        const auto found = model.buffers.find(state.buffer);
        const auto *buffer = found == model.buffers.end() ? nullptr : &found->second;
        const auto nextHistorySync = buffer ? buffer->initialHistorySync : HistorySyncState::Idle;
        if (historySync != nextHistorySync) {
            historySync = nextHistorySync;
            historyStatus->setText(historySync == HistorySyncState::Queued ? "Waiting for history"
                                                                           : "Syncing history");
            historyArea->setTooltip(historyStatus->text());
            owner.markLayoutDirty();
        }
        historyArea->setVisible(historySync != HistorySyncState::Idle);
        const bool loadingHistory = historySync == HistorySyncState::Loading;
        historySpinner->setVisible(loadingHistory);
        historyIcon->setVisible(!loadingHistory);
        historyIcon->setColor(themeForegroundColor("on_surface_variant"));
        historyIcon->setGlyphSize(12 * scale);
        bool paneVisible = true;
        for (const Node *node = &owner; node && paneVisible; node = node->parent())
            paneVisible = node->visible();
        if (loadingHistory && paneVisible && !model.settings.reducedMotion &&
            model.settings.fx != FxLevel::Off)
            historySpinner->start();
        else
            historySpinner->stop();
        historySpinner->setColor(themeForegroundColor("on_surface_variant"));
        historySpinner->setSpinnerSize(12 * scale);
        labelStyle(*historyStatus, 11 * scale, "on_surface_variant");
        const bool nextChannel = buffer && buffer->kind == "channel";
        serverLog = buffer && buffer->kind == "server";
        const bool changedSource =
            changedPane || cachedBuffer != state.buffer || channel != nextChannel;
        cachedBuffer = state.buffer;
        if (channel != nextChannel) {
            channel = nextChannel;
            owner.markLayoutDirty();
        }
        const bool mentions = buffer && buffer->kind == "mentions";
        const std::string_view name = !buffer     ? std::string_view("Conversation")
                                      : mentions  ? std::string_view("Mentions")
                                      : serverLog ? std::string_view("Server log")
                                                  : std::string_view(buffer->target);
        const std::string_view server = mentions ? std::string_view("All networks")
                                        : buffer ? serverDisplayName(model, buffer->server)
                                                 : "";
        const std::string_view description =
            nextChannel ? std::string_view(buffer->topic) : std::string_view{};
        const bool changedIdentity = cachedTitle != name || cachedNetwork != server ||
                                     cachedTopic != description || changedSource;
        if (changedIdentity) {
            cachedTitle = name;
            cachedNetwork = server;
            cachedTopic = description;
            networkInitials = nativeAvatarInitials(cachedNetwork);
            network->clearTooltip();
            measuredScale = 0;
            title->setText(name);
            network->setText(server);
            avatarInitials->setText(nativeAvatarInitials(cachedTitle, channel));
            owner.markLayoutDirty();
            tooltip = cachedTitle;
            if (!cachedNetwork.empty())
                tooltip += "\n" + cachedNetwork;
            if (!cachedTopic.empty())
                tooltip += "\n" + cachedTopic;
        }
        const std::string_view nextAvatarSource =
            navigationMode != SidebarMode::Expanded && nextChannel && model.settings.avatars
                ? std::string_view(buffer->avatarPath)
                : std::string_view{};
        if (changedSource || avatarSource != nextAvatarSource) {
            avatarSource.assign(nextAvatarSource);
            loadedAvatarSize = 0;
            avatarImage->setVisible(false);
            owner.markLayoutDirty();
        }
        if (navigationMode == SidebarMode::Expanded && !serverLog) {
            identity->setVisible(false);
            avatarImage->setVisible(false);
        }
        labelStyle(*avatarInitials, 13 * scale, "on_surface_variant");
        avatarInitials->setFontWeight(FontWeight::SemiBold);
        avatarBackground->setFill(themeSurfaceColor("surface_container_high"));
        avatarBackground->setRadius(themeCornerRadius());
        labelStyle(*title, 14 * scale, active ? "on_surface" : "on_surface_variant");
        labelStyle(*network, 11 * scale, "on_surface_variant");
        networkChip->setFill(themeSurfaceColor("surface_container_high"));
        networkChip->setRadius(themeCornerRadius());
        for (std::size_t i = 0; i < tools.size(); ++i)
            tools[i]->sync(scale, i == 0 && peopleEffective != SidebarMode::Hidden);
        topic->sync(scale);
        const bool hintsChanged =
            bindings != model.settings.bindings || changedIdentity || !tools[0]->hasTooltip();
        if (hintsChanged) {
            bindings = model.settings.bindings;
            tools[1]->hint(commandLabel(model, "Search conversation", "search"));
            tools[2]->hint("More conversation actions\n" + tooltip);
        }
        if (hintsChanged || cachedPeoplePreferred != model.settings.peopleMode ||
            cachedPeopleEffective != peopleEffective) {
            cachedPeoplePreferred = model.settings.peopleMode;
            cachedPeopleEffective = peopleEffective;
            tools[0]->hint(commandLabel(
                model, sidebarModeToggleLabel("People", cachedPeoplePreferred, peopleEffective),
                "toggle-people"));
        }
    }
    float desiredHeight(Renderer &renderer, bool shortTitle) const {
        const float identityHeight =
            std::max(renderer.fontRowExtent(14 * scale, themeUiFont()),
                     renderer.fontRowExtent(11 * scale, themeUiFont()) + 8 * scale);
        return std::max(hitExtent(scale) + (shortTitle ? 0.f : 12 * scale),
                        identityHeight + 4 * scale);
    }
    void layout(Renderer &renderer) {
        const float gap = 6 * scale, extent = hitExtent(scale), avatarExtent = 28 * scale;
        const float chromeHeight = owner.height();
        if (measuredScale != scale || measuredRenderScale != renderer.renderScale() ||
            measuredGeneration != renderer.textMetricsGeneration() ||
            measuredFont != title->font()) {
            measuredScale = scale;
            measuredRenderScale = renderer.renderScale();
            measuredGeneration = renderer.textMetricsGeneration();
            measuredFont = title->font();
            titleNaturalWidth =
                std::ceil(renderer.measureText(cachedTitle, 14 * scale, title->font()).width);
            // Use the existing fallback title as a readable-name budget, not a viewport breakpoint.
            readableTitleWidth =
                std::min(titleNaturalWidth,
                         renderer.measureText("Conversation", 14 * scale, title->font()).width);
            minimumTitleWidth = std::min(
                titleNaturalWidth,
                std::ceil(
                    renderer.measureText(channel ? "#M…" : "M…", 14 * scale, title->font()).width));
            networkNaturalWidth =
                std::ceil(renderer.measureText(cachedNetwork, 11 * scale, network->font()).width);
            networkInitialsWidth =
                std::ceil(renderer.measureText(networkInitials, 11 * scale, network->font()).width);
        }
        const int fullCount = channel ? 4 : 2;
        const float fullReserved = fullCount * extent + (fullCount - 1) * gap;
        const float padding =
            std::min(8 * scale, std::max(0.f, (owner.width() - fullReserved) / 2));
        const float available = std::max(0.f, owner.width() - 2 * padding);
        const float chipPadding = 6 * scale;
        const float minimumNetworkWidth =
            cachedNetwork.empty()
                ? 0.f
                : std::min(networkNaturalWidth, networkInitialsWidth) + 2 * chipPadding;
        const float networkReserve = minimumNetworkWidth > 0 ? minimumNetworkWidth + gap : 0.f;
        hiddenTools = (navigationMode != SidebarMode::Expanded || serverLog) &&
                      available - fullReserved - gap <
                          avatarExtent + gap + readableTitleWidth + networkReserve;
        // Keep topic and People together when search folds into More.
        const bool showTopic = channel;
        const bool showPeople = channel;
        const std::array<bool, 3> visible{showPeople, !hiddenTools, true};
        const int count = hiddenTools ? (channel ? 3 : 1) : fullCount;
        const float reserved = count * extent + (count - 1) * gap;
        const float leftBudget = std::max(0.f, available - reserved - gap);
        const bool wantsIdentity = navigationMode != SidebarMode::Expanded || serverLog;
        const float identityMinimum = wantsIdentity ? avatarExtent + gap + minimumTitleWidth : 0.f;
        const float historyNatural =
            historySync == HistorySyncState::Idle
                ? 0.f
                : std::ceil(
                      renderer.measureText(historyStatus->text(), 11 * scale, historyStatus->font())
                          .width) +
                      18 * scale;
        const float historyBudget =
            historyNatural > 0
                ? std::min(historyNatural, std::max(std::min(12 * scale, leftBudget),
                                                    leftBudget - identityMinimum - gap))
                : 0.f;
        const float identityBudget =
            wantsIdentity
                ? std::max(0.f, leftBudget - historyBudget - (historyBudget > 0 ? gap : 0.f))
                : 0.f;
        if (historyBudget > 0) {
            const float historyX = padding + (identityBudget > 0 ? identityBudget + gap : 0.f);
            place(*historyArea, historyX, 0, historyBudget, chromeHeight);
            place(*historyIcon, 0, (chromeHeight - 12 * scale) / 2, 12 * scale, 12 * scale);
            place(*historySpinner, 0, (chromeHeight - 12 * scale) / 2, 12 * scale, 12 * scale);
            const float textWidth = std::max(0.f, historyBudget - 18 * scale);
            historyStatus->setVisible(textWidth >= 12 * scale);
            placeLabel(*historyStatus, renderer, 18 * scale, 0, textWidth, chromeHeight);
        }
        const bool showIdentity = wantsIdentity && identityBudget > 0;
        place(*identity, padding, 0, identityBudget, chromeHeight);
        identity->setVisible(showIdentity);
        if (!showIdentity) {
            if (avatarImage->hasImage())
                avatarImage->clear(renderer);
            loadedAvatarSource.clear();
            loadedAvatarSize = 0;
            avatarImage->setVisible(false);
        }
        if (showIdentity) {
            const float avatarTop = (chromeHeight - avatarExtent) / 2;
            place(*avatarBackground, 0, avatarTop, avatarExtent, avatarExtent);
            place(*avatarImage, 0, avatarTop, avatarExtent, avatarExtent);
            avatarImage->setRadius(themeCornerRadius());
            const int target =
                std::max(1, int(std::ceil(avatarExtent * std::max(1.f, renderer.renderScale()))));
            if (loadedAvatarSource != avatarSource || loadedAvatarSize != target) {
                loadedAvatarSource = avatarSource;
                loadedAvatarSize = target;
                loadChannelAvatarRaster(*avatarImage, renderer, avatarSource, target);
            }
            avatarImage->setVisible(avatarImage->hasImage());
            avatarInitials->setVisible(!avatarImage->hasImage());
            if (avatarInitials->visible())
                avatarInitialsPlacement.place(*avatarInitials, renderer, 0, avatarTop, avatarExtent,
                                              avatarExtent);
            const float titleX = avatarExtent + gap;
            const float textBudget = std::max(0.f, identityBudget - titleX);
            // Keep only a meaningful network minimum; the title owns all remaining space.
            const float titleBudget = textBudget >= minimumTitleWidth + networkReserve
                                          ? textBudget - networkReserve
                                          : textBudget;
            const float titleWidth = std::min(titleNaturalWidth, titleBudget);
            placeLabel(*title, renderer, titleX, 0, titleWidth, chromeHeight);
            const float chipBudget =
                std::min(120 * scale, std::max(0.f, textBudget - titleWidth - gap));
            const bool fullNetwork = networkNaturalWidth + 2 * chipPadding <= chipBudget;
            const bool showNetwork =
                !cachedNetwork.empty() &&
                (fullNetwork || networkInitialsWidth + 2 * chipPadding <= chipBudget);
            network->setVisible(showNetwork);
            networkChip->setVisible(showNetwork);
            if (showNetwork) {
                network->setText(fullNetwork ? cachedNetwork : networkInitials);
                if (fullNetwork)
                    network->clearTooltip();
                else if (!network->hasTooltip())
                    network->setTooltip(cachedNetwork);
                const float textWidth = fullNetwork ? networkNaturalWidth : networkInitialsWidth;
                const float chipWidth = textWidth + 2 * chipPadding;
                network->setMaxWidth(textWidth);
                network->measure(renderer);
                const float chipX = titleX + titleWidth + gap;
                const float chipHeight = network->height() + 8 * scale;
                place(*networkChip, chipX, (chromeHeight - chipHeight) / 2, chipWidth, chipHeight);
                placeLabel(*network, renderer, chipX + chipPadding, 0, textWidth, chromeHeight);
            }
        }
        float x = std::max(0.f, owner.width() - padding - reserved);
        const bool moveToolFocus =
            (!visible[0] && tools[0]->focused()) || (!visible[1] && tools[1]->focused());
        if (!showTopic && topic->visible())
            topic->dispatchCancel();
        const bool moveTopicFocus = !showTopic && topic->focused();
        topic->setVisible(showTopic);
        topic->setExcludeSubtreeFromTabOrder(!showTopic);
        for (std::size_t i = 0; i < tools.size(); ++i) {
            if (!visible[i] && tools[i]->visible())
                tools[i]->dispatchCancel();
            tools[i]->setVisible(visible[i]);
            tools[i]->setExcludeSubtreeFromTabOrder(!visible[i]);
            if (!visible[i])
                continue;
            place(*tools[i], x, (chromeHeight - extent) / 2, extent, extent);
            x += extent + gap;
            if (i == 0 && showTopic) {
                place(*topic, x, (chromeHeight - extent) / 2, extent, extent);
                x += extent + gap;
            }
        }
        if ((moveToolFocus || moveTopicFocus) && dispatcher)
            dispatcher->setFocus(tools[2]);
    }
};

BufferTitleLine::BufferTitleLine(Model &model, BufferChromeCallbacks callbacks)
    : impl(std::make_unique<Impl>(*this, model, std::move(callbacks))) {}
BufferTitleLine::~BufferTitleLine() = default;
void BufferTitleLine::sync(const PaneState &pane, bool focused, float scale,
                           SidebarMode peopleEffective, SidebarMode navigationEffective) {
    impl->sync(pane, focused, scale, peopleEffective, navigationEffective);
}
void BufferTitleLine::setCompact(bool compact) {
    if (impl->compact != compact) {
        impl->compact = compact;
        markLayoutDirty();
    }
}
void BufferTitleLine::setInputDispatcher(InputDispatcher &dispatcher) {
    impl->dispatcher = &dispatcher;
}
void BufferTitleLine::doLayout(Renderer &renderer) {
    impl->layout(renderer);
    Node::doLayout(renderer);
}
LayoutSize BufferTitleLine::doMeasure(Renderer &renderer, const LayoutConstraints &constraints) {
    return constraints.constrain(
        {constraints.hasMaxWidth ? constraints.maxWidth : 480 * impl->scale,
         impl->desiredHeight(renderer, impl->compact)});
}
void BufferTitleLine::doArrange(Renderer &renderer, const LayoutRect &rect) {
    setPosition(rect.x, rect.y);
    setSize(rect.width, rect.height);
    doLayout(renderer);
}

struct TopicPopover::Impl {
    TopicPopover &owner;
    InputDispatcher *dispatcher = nullptr;
    InputArea *blocker, *panel;
    Box *background;
    Label *title;
    Input *text;
    ChromeButton *close;
    LayoutRect anchor{};
    float scale = 1;
    std::string appliedFamily, appliedFeatures;
    std::function<void()> dismiss;

    explicit Impl(TopicPopover &view) : owner(view) {
        blocker = static_cast<InputArea *>(owner.addChild(std::make_unique<InputArea>()));
        panel = static_cast<InputArea *>(owner.addChild(std::make_unique<InputArea>()));
        for (auto *area : {blocker, panel}) {
            area->setFocusable(false);
            area->setTabStop(false);
            area->setFocusOnPointerPress(false);
            area->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT, BTN_MIDDLE}));
            area->setOnAxisHandler([](const auto &) { return true; });
        }
        blocker->setOnClick([this](const auto &) { cancel(); });
        panel->setClipChildren(true);
        background = static_cast<Box *>(panel->addChild(std::make_unique<Box>()));
        background->setHitTestVisible(false);
        title = static_cast<Label *>(panel->addChild(ui::label({.text = "Channel topic"})));
        title->setUseMarkup(false);
        title->setMaxLines(1);
        title->setHitTestVisible(false);
        text = static_cast<Input *>(panel->addChild(std::make_unique<Input>()));
        text->setMultiline(true);
        text->setDisplayProjector([](std::string_view raw) {
            auto document = parseIrcFormatting(raw, themeForegroundColor("on_surface"),
                                               themeSurfaceColor("surface_container_high"));
            std::string normal, contrast;
            for (const auto &run : document.runs) {
                const auto visible =
                    std::string_view(document.text).substr(run.start, run.end - run.start);
                irc_detail::appendMarkup(normal, visible, run.style, true);
                irc_detail::appendMarkup(contrast, visible, run.style, false);
            }
            return Input::DisplayProjection{std::move(document.text), std::move(normal),
                                            std::move(contrast), std::move(document.rawToDisplay)};
        });
        text->setMinLayoutWidth(0);
        text->setPlaceholder("No topic set");
        text->inputArea()->setTabFocusKey("channel-topic-text");
        text->inputArea()->setTooltip("Channel topic · Select and copy · Escape close");
        // Reject every native edit path without disabling selection and copy.
        text->setEditValidator([](auto, auto, bool) { return false; });
        text->setOnKeyEvent([this](std::uint32_t sym, std::uint32_t) {
            if (sym != XKB_KEY_Escape)
                return false;
            cancel();
            return true;
        });
        close = static_cast<ChromeButton *>(panel->addChild(std::make_unique<ChromeButton>(
            "x", [this](Node &, std::optional<InputArea::PointerData>) { cancel(); })));
        close->hint("Close channel topic");
        close->setTabFocusKey("channel-topic-close");
    }
    void cancel() {
        auto callback = std::move(dismiss);
        owner.hide();
        if (callback)
            callback();
    }
    void geometry() {
        const float margin = std::min({8 * scale, owner.width() / 8, owner.height() / 8});
        const float w = std::max(0.f, std::min(520 * scale, owner.width() - 2 * margin));
        const float h = std::max(0.f, std::min(260 * scale, owner.height() - 2 * margin));
        place(*blocker, 0, 0, owner.width(), owner.height());
        place(*panel,
              std::clamp(anchor.x + anchor.width - w, margin,
                         std::max(margin, owner.width() - w - margin)),
              std::clamp(anchor.y + anchor.height + 4 * scale, margin,
                         std::max(margin, owner.height() - h - margin)),
              w, h);
        place(*background, 0, 0, w, h);
        background->setFill(themeSurfaceColor("surface_container_high"));
        background->setBorder(themeForegroundColor("outline_variant"), scale);
        background->setRadius(themeCornerRadius());
        const float pad = std::min(12 * scale, w / 8);
        const float extent = hitExtent(scale);
        labelStyle(*title, 14 * scale, "on_surface");
        title->setPosition(pad, pad);
        title->setMaxWidth(std::max(0.f, w - 3 * pad - extent));
        close->sync(scale);
        place(*close, std::max(0.f, w - pad - extent), pad, extent, extent);
        const auto &font = themeUiFont();
        if (appliedFamily != font.family) {
            appliedFamily = font.family;
            text->setFontFamily(appliedFamily);
        }
        if (appliedFeatures != font.features) {
            appliedFeatures = font.features;
            text->setFontFeatures(appliedFeatures);
        }
        text->setFontWeight(font.weight);
        text->setFontSize(14 * scale);
        text->setHorizontalPadding(8 * scale);
        place(*text, pad, extent + 2 * pad, std::max(0.f, w - 2 * pad),
              std::max(0.f, h - extent - 3 * pad));
    }
};

TopicPopover::TopicPopover() : impl(std::make_unique<Impl>(*this)) {
    setParticipatesInLayout(false);
    setZIndex(1080);
    setVisible(false);
    setHitTestVisible(false);
}
TopicPopover::~TopicPopover() = default;
void TopicPopover::setInputDispatcher(InputDispatcher &dispatcher) {
    impl->dispatcher = &dispatcher;
}
void TopicPopover::show(std::string_view topic, LayoutRect anchor, std::function<void()> dismiss) {
    hide();
    impl->anchor = anchor;
    impl->dismiss = std::move(dismiss);
    impl->text->setValue(topic);
    setVisible(true);
    setHitTestVisible(true);
    impl->geometry();
    if (impl->dispatcher)
        impl->dispatcher->setFocus(impl->text->inputArea());
}
void TopicPopover::hide() {
    impl->blocker->dispatchCancel();
    impl->panel->dispatchCancel();
    impl->text->inputArea()->dispatchCancel();
    impl->close->dispatchCancel();
    if (impl->dispatcher && (impl->dispatcher->focusedArea() == impl->text->inputArea() ||
                             impl->dispatcher->focusedArea() == impl->close))
        impl->dispatcher->setFocus(nullptr);
    setVisible(false);
    setHitTestVisible(false);
    impl->dismiss = {};
}
bool TopicPopover::active() const { return visible(); }
void TopicPopover::sync(float width, float height, float scale) {
    setSize(std::max(0.f, width), std::max(0.f, height));
    impl->scale = std::max(.1f, scale);
    if (active())
        impl->geometry();
}
bool TopicPopover::handleKey(const InputArea::KeyData &key) {
    if (!active() || !key.pressed)
        return false;
    if (key.sym == XKB_KEY_Tab || key.sym == XKB_KEY_ISO_Left_Tab) {
        if (impl->dispatcher)
            impl->dispatcher->cycleTabFocusInSubtree(this, key.modifiers & KeyMod::Shift);
        return true;
    }
    if (key.sym != XKB_KEY_Escape)
        return false;
    impl->cancel();
    return true;
}
Input &TopicPopover::topicInput() { return *impl->text; }
void TopicPopover::doLayout(Renderer &renderer) {
    impl->geometry();
    Node::doLayout(renderer);
}

} // namespace blast
