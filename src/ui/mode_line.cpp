#include "mode_line.hpp"
#include "../model.hpp"
#include "../theme/theme.hpp"
#include "dialogs.hpp"
#include "logic.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <core/input/key_symbols.h>
#include <core/timer_manager.h>
#include <map>
#include <optional>
#include <render/scene/input_area.h>
#include <string>
#include <ui/controls/box.h>
#include <ui/controls/glyph.h>
#include <ui/controls/label.h>
#include <ui/controls/select_popup_context.h>
#include <utility>
#include <vector>

namespace blast {
namespace {
template <class T> T *child(Node &parent) {
    auto node = std::make_unique<T>();
    auto *result = node.get();
    parent.addChild(std::move(node));
    return result;
}
Label *lineLabel(Node &parent) {
    auto *label = child<Label>(parent);
    label->setUseMarkup(false);
    label->setMaxLines(1);
    label->setEllipsize(TextEllipsize::End);
    label->setBaselineMode(LabelBaselineMode::InkCentered);
    return label;
}
bool historyEligible(const Buffer &chat) {
    return (chat.kind == "channel" || chat.kind == "query") && !chat.archived && !chat.muted &&
           chat.initialHistorySync != HistorySyncState::Idle;
}
void style(Label &label, const TextFont &font, float scale) {
    if (label.font().family != font.family)
        label.setFontFamily(font.family);
    if (label.font().features != font.features)
        label.setFontFeatures(font.features);
    label.setFontWeight(font.weight);
    label.setFontSize(12 * scale);
}
float textWidth(Renderer &renderer, std::string_view value, float scale, const TextFont &font) {
    return value.empty() ? 0 : std::ceil(renderer.measureText(value, 12 * scale, font, 0, 1).width);
}
void placeLabel(Label &label, Renderer &renderer, float x, float width, float height) {
    label.setVisible(width > 0);
    if (width > 0) {
        label.setMaxWidth(width);
        label.measure(renderer);
        label.setPosition(x, (height - label.height()) / 2);
    }
}
bool eligible(const Buffer &chat) {
    return (chat.kind == "channel" || chat.kind == "query") && !chat.archived && !chat.muted &&
           chat.unread > 0;
}
std::string_view activityNetwork(const Model &model, const Buffer &chat) {
    const auto display = serverDisplayName(model, chat.server);
    return display == "Network" && !chat.server.empty() ? std::string_view(chat.server) : display;
}
std::string overflowCaption(size_t count) {
    return "+" + std::to_string(count) + (count == 1 ? " chat" : " chats");
}
bool activateKey(const InputArea::KeyData &key) {
    return key.pressed && !key.preedit && key.modifiers == 0 && KeySymbol::isEnterOrSpace(key.sym);
}
ContextMenuEntry menuEntry(size_t value, std::string label, std::string_view icon = {}) {
    ContextMenuEntry entry;
    entry.label = std::move(label);
    entry.icon = icon;
    entry.value = value;
    entry.ellipsize = TextEllipsize::Middle;
    return entry;
}

} // namespace

struct ModeLine::Impl {
    enum Group { History, Feedback, Chord, Nick, NetworkName, State, GroupCount };
    struct Activity {
        std::string name, target, readMarker, caseMapping, caption, chatTitle, countText;
        size_t size = 0;
        uint64_t revision = 0;
        int count = 0;
        bool mentioned = false, query = false;
    };
    using ActivityMap = std::map<std::string, Activity, std::less<>>;
    struct Button {
        InputArea *area;
        Node *content;
        Label *title;
        Box *highlight;
        Box *focus;
        std::optional<Group> group;
        Label *marker = nullptr, *suffix = nullptr, *network = nullptr;
        std::array<Box *, 2> dividers{};
    };
    struct Slot {
        Button button;
        std::string id;
    };
    struct Scope {
        std::string_view buffer, server;
        const Buffer *chat;
        const Network *network;
        const ServerConfig *profile;
        bool onboarding;

        bool mentions() const { return chat && chat->kind == "mentions"; }
        bool writable() const {
            return chat && !onboarding &&
                   (chat->kind == "channel" || chat->kind == "query" || chat->kind == "server");
        }
        std::string_view state() const {
            return mentions()                           ? "Read only"
                   : network && network->retrying       ? "Retrying"
                   : network && !network->state.empty() ? std::string_view(network->state)
                                                        : "Disconnected";
        }
        uint64_t session() const { return network ? network->sessionGeneration : 0; }
    };
    struct HistoryRow {
        std::string id, name, target;
        HistorySyncState state;
        bool query;
    };
    using HistoryRows = std::vector<HistoryRow>;

    Node &owner;
    Model &model;
    std::function<void(std::string)> execute;
    std::function<void(Node &)> retire;
    std::array<Button, GroupCount> controls;
    std::array<std::string_view, GroupCount> hints;
    std::array<Glyph *, GroupCount> icons;
    ActivityMap activity;
    std::vector<const ActivityMap::value_type *> ordered;
    std::vector<Slot> slots;
    std::vector<std::array<float, 3>> measuredChips;
    Button overflow;
    Box *tip;
    Label *tipText;
    Timer hoverTimer;
    std::optional<Group> hoverGroup;
    InputArea *pointerFocused = nullptr;
    size_t firstOmitted = 0, historyRemaining = 0;
    decltype(Settings::fools) fools;
    bool foolUnread = false, visible = false, allowed = true, layoutReady = false;
    bool healthy = false, failureFeedback = false;
    float scale = 1, contentBottomInset = 0;
    Color activityInk{}, mentionInk{}, focusInk{};
    Color hoverFill{};
    struct CardColors {
        Color fill, network, text, count, mention, divider;
    };
    std::array<CardColors, 2> cardColors{};
    std::array<Color, GroupCount> controlInks{};
    std::string shownBuffer, shownServer, shownState;
    uint64_t shownSession = 0;
    bool shownProfile = false, shownWritable = false;

    std::shared_ptr<int> alive = std::make_shared<int>(0);
    uint64_t popupGeneration = 0;
    Button *popupButton = nullptr;
    std::array<int, 4> popupAnchor{};
    std::shared_ptr<const HistoryRows> popupHistory;

    Impl(Node &owner, Model &model, std::function<void(std::string)> execute,
         std::function<void(Node &)> retire)
        : owner(owner), model(model), execute(std::move(execute)), retire(std::move(retire)) {
        constexpr std::array names{"history", "info-circle", "keyboard",
                                   "user",    "server",      "plug-connected-x"};
        constexpr std::array keys{"status-history", "status-feedback", "status-chord",
                                  "status-nick",    "status-network",  "status-connection"};
        for (size_t i = controls.size(); i-- > 0;) {
            const auto group = static_cast<Group>(i);
            auto &button = controls[i] = makeButton(group);
            auto *area = button.area;
            icons[i] = child<Glyph>(*button.content);
            icons[i]->setGlyph(names[i]);
            area->setTabFocusKey(keys[i]);
            area->setOnClick([this, group](const InputArea::PointerData &pointer) {
                activateGroup(group, pointer.serial);
            });
            area->setOnKeyDown([this, group](const InputArea::KeyData &key) {
                if (activateKey(key))
                    activateGroup(group, std::nullopt);
            });
        }
        controls[Nick].area->setFocusOnPointerPress(false);
        controls[Chord].area->setFocusOnPointerPress(false);
        tip = child<Box>(owner);
        tip->setHitTestVisible(false);
        tip->setVisible(false);
        tipText = child<Label>(*tip);
        tipText->setUseMarkup(false);
        tipText->setMaxLines(3);
        overflow = makeButton();
        overflow.area->setTabFocusKey("status-activity-overflow");
        overflow.area->setOnClick(
            [this](const InputArea::PointerData &pointer) { openOverflow(pointer.serial); });
        overflow.area->setOnKeyDown([this](const InputArea::KeyData &key) {
            if (activateKey(key))
                openOverflow(std::nullopt);
        });
    }
    ~Impl() {
        alive.reset();
        closePopup();
    }
    void hideTip() {
        hoverTimer.stop();
        hoverGroup.reset();
        tip->setVisible(false);
    }
    void startHover(Group group) {
        hideTip();
        const auto &button = controls[group];
        if (!allowed || popupButton || !button.area->enabled())
            return;
        hoverGroup = group;
        hoverTimer.start(std::chrono::milliseconds(400), [this, group] {
            const auto &area = *controls[group].area;
            if (allowed && !popupButton && hoverGroup == group && area.visible() &&
                area.enabled() && area.hovered()) {
                updateTipText(group);
                tip->setVisible(true);
                owner.markLayoutDirty();
            }
        });
    }
    void updateFocus(const Button &button) const {
        const auto &area = *button.area;
        button.focus->setVisible(allowed && area.enabled() && area.focused() &&
                                 pointerFocused != &area);
    }
    bool hovered(const Button &button) const {
        const auto &area = *button.area;
        return allowed && !popupButton && area.visible() && area.enabled() && area.hovered();
    }
    void updateHover(const Button &button) const {
        const bool shown = hovered(button);
        if (button.network || button.highlight->visible() != shown) {
            button.highlight->setVisible(button.network || shown);
            updateInk(button);
        }
    }
    void updateInk(const Button &button) const {
        if (button.network) {
            const auto &colors = cardColors[hovered(button)];
            button.highlight->setFill(colors.fill);
            button.network->setColor(colors.network);
            button.title->setColor(colors.text);
            button.marker->setColor(colors.mention);
            button.suffix->setColor(button.marker->visible() ? colors.mention : colors.count);
            for (auto *divider : button.dividers)
                divider->setFill(colors.divider);
            return;
        }
        const auto ink = [&](Color normal) {
            return button.highlight->visible() ? themePairedForeground(hoverFill, normal) : normal;
        };
        const auto normal = button.group ? controlInks[*button.group] : activityInk;
        const auto foreground = ink(normal);
        button.title->setColor(foreground);
        if (button.group)
            icons[*button.group]->setColor(foreground);
    }
    std::string_view caption(Group group) const { return controls[group].title->text(); }
    void updateTipText(Group group) {
        const auto hint = hints[group];
        const auto value = caption(group);
        std::string content;
        content.reserve(hint.size() + 1 + value.size());
        content.append(hint).append("\n").append(value);
        tipText->setText(content);
    }
    void setControl(Group group, std::string_view value, std::string_view hint) {
        auto &button = controls[group];
        auto &area = *button.area;
        if (button.title->setText(value) || hints[group] != hint) {
            if (popupButton == &button)
                closePopup();
            hints[group] = hint;
            if (area.pressed())
                retire(area);
            std::string tooltip;
            if (!value.empty()) {
                tooltip.reserve(value.size() + 1 + hint.size());
                tooltip.append(value).append("\n").append(hint);
            }
            area.setTooltip(std::move(tooltip));
            if (hoverGroup == group && tip->visible())
                updateTipText(group);
        }
    }
    Scope scope() const {
        const auto found = model.buffers.find(model.activeBuffer);
        const auto *chat = found == model.buffers.end() ? nullptr : &found->second;
        auto network = model.networks.end();
        if (chat && chat->kind != "mentions")
            network = model.networks.find(chat->server);
        else if (!chat && !model.networks.empty())
            network = model.networks.begin();
        const std::string_view server = chat ? std::string_view(chat->server)
                                        : network != model.networks.end() ? network->first
                                                                          : std::string_view{};
        const auto profile =
            std::find_if(model.servers.begin(), model.servers.end(),
                         [&](const auto &config) { return config.name == server; });
        const auto *config = profile == model.servers.end() ? nullptr : &*profile;
        const bool onboarding = chat && chat->kind == "server" && config &&
                                !model.onboardingNetwork.empty() &&
                                (config->name == model.onboardingNetwork ||
                                 config->sojuParent == model.onboardingNetwork);
        return {model.activeBuffer,
                server,
                chat,
                network == model.networks.end() ? nullptr : &network->second,
                config,
                onboarding};
    }
    bool matchesScope(const Scope &current) const {
        return shownBuffer == current.buffer && shownServer == current.server &&
               shownState == current.state() && shownSession == current.session() &&
               shownProfile == bool(current.profile) && shownWritable == current.writable();
    }
    void setScope(const Scope &current) {
        if (matchesScope(current))
            return;
        if (popupButton == &controls[State])
            closePopup();
        for (const auto group : {Nick, NetworkName, State})
            if (controls[group].area->pressed())
                retire(*controls[group].area);
        shownBuffer = current.buffer;
        shownServer = current.server;
        shownState = current.state();
        shownSession = current.session();
        shownProfile = bool(current.profile);
        shownWritable = current.writable();
    }
    Button makeButton(std::optional<Group> group = {}, bool activityChip = false) {
        auto *area = static_cast<InputArea *>(
            owner.insertChildAt(slots.size(), std::make_unique<InputArea>()));
        area->setVisible(false);
        area->setFocusable(true);
        area->setRetainsFocusOnPointerRelease(true);
        area->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
        area->setClipChildren(true);
        auto *highlight = child<Box>(*area);
        highlight->setHitTestVisible(false);
        highlight->setVisible(false);
        auto *content = child<Node>(*area);
        content->setHitTestVisible(false);
        auto *focus = child<Box>(*area);
        focus->setHitTestVisible(false);
        focus->setVisible(false);
        const Button button{area,
                            content,
                            lineLabel(*content),
                            highlight,
                            focus,
                            group,
                            activityChip ? lineLabel(*content) : nullptr,
                            activityChip ? lineLabel(*content) : nullptr,
                            activityChip ? lineLabel(*content) : nullptr,
                            activityChip ? std::array{child<Box>(*content), child<Box>(*content)}
                                         : std::array<Box *, 2>{}};
        for (auto *divider : button.dividers)
            if (divider)
                divider->setHitTestVisible(false);
        area->setOnFocusGain([this, button] {
            pointerFocused = nullptr;
            updateFocus(button);
        });
        area->setOnFocusLoss([focus] { focus->setVisible(false); });
        area->setOnEnter([this, button, group](const InputArea::PointerData &) {
            updateHover(button);
            if (group)
                startHover(*group);
        });
        area->setOnLeave([this, button, group] {
            updateHover(button);
            if (group && hoverGroup == group)
                hideTip();
        });
        area->setOnPress([this, button, group](const InputArea::PointerData &) {
            if (button.area->focused()) {
                pointerFocused = button.area;
                updateFocus(button);
            }
            if (group)
                hideTip();
        });
        return button;
    }
    void moveButton(Button &button, float x, float width, float contentHeight) {
        auto &area = *button.area;
        const float height = owner.height();
        if (area.pressed() && (area.x() != x || area.width() != width || area.height() != height))
            retire(area);
        area.setPosition(x, 0);
        area.setSize(width, height);
        area.setVisible(true);
        button.content->setFrameSize(width, contentHeight);
        const float inset = std::min(scale * 2, contentHeight / 4);
        const float highlightHeight = contentHeight - 2 * inset;
        button.highlight->setPosition(0, inset);
        button.highlight->setFrameSize(width, highlightHeight);
        const float radius = button.network ? 7 * scale : themeCornerRadius();
        button.highlight->setRadius(std::min(radius, std::min(width, highlightHeight) / 2));
        if (!button.network)
            button.highlight->setFill(hoverFill);
        button.focus->setFill(focusInk);
        const float rule = std::max(1.f, scale);
        button.focus->setPosition(0, height - rule);
        button.focus->setSize(width, rule);
    }
    void hideButton(Button &button) {
        if (button.area->visible()) {
            retire(*button.area);
            button.area->setVisible(false);
            updateHover(button);
        }
    }
    void enableButton(Button &button, bool enabled) {
        if (button.area->enabled() && !enabled && button.area->visible())
            retire(*button.area);
        button.area->setEnabled(enabled);
        updateFocus(button);
        updateHover(button);
    }
    const Buffer *eligibleBuffer(const std::string &id) const {
        const auto found = model.buffers.find(id);
        return found != model.buffers.end() && eligible(found->second) ? &found->second : nullptr;
    }
    void activateSlot(size_t index) {
        const auto &slot = slots[index];
        if (layoutReady && allowed && slot.button.area->visible() && eligibleBuffer(slot.id))
            execute("buffer:" + slot.id);
    }
    Slot &slotAt(size_t index) {
        if (index == slots.size()) {
            auto button = makeButton(std::nullopt, true);
            button.area->setOnClick(
                [this, index](const InputArea::PointerData &) { activateSlot(index); });
            button.area->setOnKeyDown([this, index](const InputArea::KeyData &key) {
                if (activateKey(key))
                    activateSlot(index);
            });
            slots.push_back({button, {}});
        }
        return slots[index];
    }
    void styleButton(Button &button, const TextFont &font) {
        style(*button.title, font, scale);
        updateInk(button);
    }
    static std::array<int, 4> anchor(const Button &button) {
        float left, top, rightEdge, bottom;
        Node::transformedBounds(button.area, left, top, rightEdge, bottom);
        return {static_cast<int>(std::round(left)), static_cast<int>(std::round(top)),
                std::max(1, static_cast<int>(std::round(rightEdge - left))),
                std::max(1, static_cast<int>(std::round(bottom - top)))};
    }
    void closePopup() {
        ++popupGeneration;
        popupHistory.reset();
        if (std::exchange(popupButton, nullptr))
            if (auto *context = owner.popupContext())
                context->closeSelectDropdown();
    }
    void openPopup(Button &button, std::optional<uint32_t> inputSerial,
                   std::vector<ContextMenuEntry> entries, std::function<void(size_t)> select,
                   std::shared_ptr<const HistoryRows> history = {}) {
        auto *context = button.area->popupContext();
        if (!context || popupButton || !allowed || !button.area->visible() || entries.empty())
            return;

        SelectPopupContext::DropdownRequest request;
        hideTip();
        const auto origin = anchor(button);
        popupAnchor = origin;
        request.anchorX = popupAnchor[0];
        request.anchorY = popupAnchor[1];
        request.anchorWidth = popupAnchor[2];
        request.anchorHeight = popupAnchor[3];
        request.inputSerial = inputSerial;
        request.menuWidth = 200 * scale;
        request.maxMenuWidth = 500 * scale;
        request.fontSize = 14 * scale;
        request.font = themeUiFont();
        request.maxVisibleOptions = 12;
        request.menu = std::move(entries);
        const auto generation = ++popupGeneration;
        const std::weak_ptr<int> guard = alive;
        popupButton = &button;
        popupHistory = std::move(history);
        context->openSelectDropdown(
            request, {[this, guard, generation, button = &button, origin,
                       select = std::move(select)](size_t index) {
                          if (guard.expired() || generation != popupGeneration)
                              return;
                          popupButton = nullptr;
                          popupHistory.reset();
                          ++popupGeneration;
                          if (allowed && button->area->visible() && anchor(*button) == origin)
                              select(index);
                      },
                      [this, guard, generation] {
                          if (!guard.expired() && generation == popupGeneration) {
                              popupButton = nullptr;
                              popupHistory.reset();
                          }
                      }});
        if (!context->isSelectDropdownOpen() && popupButton == &button)
            closePopup();
    }
    void openOverflow(std::optional<uint32_t> inputSerial) {
        if (!layoutReady || !allowed || popupButton || !overflow.area->visible() ||
            firstOmitted >= ordered.size())
            return;
        const size_t omitted = ordered.size() - firstOmitted;
        std::vector<ContextMenuEntry> entries;
        std::vector<std::string> ids;
        ids.reserve(omitted);
        entries.reserve(omitted);
        for (size_t i = firstOmitted; i < ordered.size(); ++i) {
            const auto &[id, entry] = *ordered[i];
            entries.push_back(menuEntry(ids.size(), entry.caption));
            ids.push_back(id);
        }
        openPopup(
            overflow, inputSerial, std::move(entries), [this, ids = std::move(ids)](size_t index) {
                if (!layoutReady || index >= ids.size() || firstOmitted + index >= ordered.size())
                    return;
                const auto &[id, entry] = *ordered[firstOmitted + index];
                const auto *chat = eligibleBuffer(ids[index]);
                if (id == ids[index] && chat && chat->unread == entry.count &&
                    chat->target == entry.target && (chat->kind == "query") == entry.query &&
                    activityNetwork(model, *chat) == entry.name &&
                    ui_logic::hasUnreadMention(model, *chat) == entry.mentioned)
                    execute("buffer:" + id);
            });
    }
    bool matchesHistory(std::string_view id, const Buffer &chat, const HistoryRow &row) const {
        return id == row.id && historyEligible(chat) && chat.initialHistorySync == row.state &&
               chat.target == row.target && (chat.kind == "query") == row.query &&
               activityNetwork(model, chat) == row.name;
    }
    void syncHistory() {
        size_t count = 0;
        bool changed = false;
        for (const auto &[id, chat] : model.buffers) {
            if (!historyEligible(chat))
                continue;
            if (popupHistory && !changed)
                changed = count >= popupHistory->size() ||
                          !matchesHistory(id, chat, (*popupHistory)[count]);
            ++count;
        }
        if (popupHistory && (changed || count != popupHistory->size()))
            closePopup();
        if (count != historyRemaining) {
            historyRemaining = count;
            setControl(History, count ? "History: " + std::to_string(count) + " remaining" : "",
                       "Show queued and loading conversations");
        }
    }
    void openHistory(std::optional<uint32_t> inputSerial) {
        auto rows = std::make_shared<HistoryRows>();
        rows->reserve(historyRemaining);
        std::vector<ContextMenuEntry> entries;
        entries.reserve(historyRemaining);
        for (const auto &[id, chat] : model.buffers) {
            if (!historyEligible(chat))
                continue;
            const auto name = activityNetwork(model, chat);
            const bool query = chat.kind == "query";
            const bool loading = chat.initialHistorySync == HistorySyncState::Loading;
            std::string label(name);
            label.append(query ? "/DM " : "/").append(chat.target);
            label.append(loading ? " — Loading" : " — Queued");
            entries.push_back(
                menuEntry(rows->size(), std::move(label), loading ? "loader" : "clock"));
            rows->push_back({id, std::string(name), chat.target, chat.initialHistorySync, query});
        }
        openPopup(
            controls[History], inputSerial, std::move(entries),
            [this, rows](size_t index) {
                if (index >= rows->size())
                    return;
                const auto &row = (*rows)[index];
                const auto chat = model.buffers.find(row.id);
                if (chat != model.buffers.end() && matchesHistory(chat->first, chat->second, row))
                    execute("buffer:" + row.id);
            },
            rows);
    }
    void openConnection(const Scope &current, std::optional<uint32_t> inputSerial) {
        if (current.mentions()) {
            execute("status-details:Global Mentions is read only and has no single network "
                    "connection.");
            return;
        }
        if (!current.profile) {
            execute(current.server.empty()
                        ? "status-details:No network profile is selected."
                        : "status-details:The network " + std::string(current.server) +
                              " no longer has a configured profile.");
            return;
        }
        const auto state = current.state();
        const bool connected = state != "Disconnected" && state != "Failed";
        std::vector<ContextMenuEntry> entries;
        entries.reserve(4);
        entries.push_back(menuEntry(0,
                                    state == "Retrying" ? "Retry now"
                                    : connected         ? "Reconnect"
                                                        : "Connect",
                                    connected ? "refresh" : "plug"));
        if (connected)
            entries.push_back(
                menuEntry(1, state == "Retrying" ? "Stop retrying" : "Disconnect", "plug-off"));
        entries.push_back(menuEntry(2, "Open server log", "terminal"));
        if (!Dialogs::certificateReviewNetwork(model, current.server).empty())
            entries.push_back(menuEntry(3, "Review certificate…", "certificate"));
        openPopup(controls[State], inputSerial, std::move(entries),
                  [this, connected, server = std::string(current.server),
                   buffer = std::string(current.buffer), state = std::string(state),
                   session = current.session()](size_t index) {
                      const auto latest = scope();
                      if (!latest.profile || latest.mentions() || latest.server != server ||
                          latest.buffer != buffer || latest.state() != state ||
                          latest.session() != session)
                          return;
                      if (index == 0)
                          execute("status-connect:" + server);
                      else if (index == 1 && connected)
                          execute("status-disconnect:" + server);
                      else if (index == 2)
                          execute("open-server-log:" + server);
                      else if (index == 3)
                          execute("review-certificate:" + server);
                  });
    }
    void activateGroup(Group group, std::optional<uint32_t> inputSerial) {
        hideTip();
        if (!allowed || popupButton || !controls[group].area->visible() || caption(group).empty())
            return;
        switch (group) {
        case History:
            openHistory(inputSerial);
            return;
        case Feedback:
            execute("status-details:" + std::string(caption(Feedback)));
            return;
        case Chord:
            execute("status-cancel-chord");
            return;
        default:
            break;
        }
        const auto current = scope();
        if (!matchesScope(current))
            return;
        if (group == NetworkName) {
            execute(current.mentions() || !current.profile ? "status-network:"
                                                           : "status-network:" + shownServer);
        } else if (group == State) {
            openConnection(current, inputSerial);
        } else if (current.mentions()) {
            execute("switch-buffer");
        } else if (!current.writable()) {
            execute("status-details:Open a writable conversation on this network to change your "
                    "nickname.");
        } else if (!current.profile || current.server.empty()) {
            execute("status-details:This conversation has no configured network profile for "
                    "changing your nickname.");
        } else {
            execute("status-nick:" + shownBuffer);
        }
    }
};

ModeLine::ModeLine(Model &model, std::function<void(std::string)> execute,
                   std::function<void(Node &)> retire)
    : impl_(std::make_unique<Impl>(*this, model, std::move(execute), std::move(retire))) {
    setZIndex(1);
    sync(1, SidebarMode::Expanded);
}
ModeLine::~ModeLine() = default;

void ModeLine::sync(float userScale, SidebarMode effectiveNavigation, std::string_view keyHint,
                    bool allowActivityMenu) {
    auto &p = *impl_;
    const bool ledgerVisible = effectiveNavigation == SidebarMode::Hidden;
    if (p.scale != userScale || p.visible != ledgerVisible || p.allowed != allowActivityMenu) {
        p.layoutReady = false;
        p.closePopup();
        p.hideTip();
        if (!allowActivityMenu)
            for (const auto &button : p.controls)
                if (button.area->pressed() || button.area->hovered())
                    p.retire(*button.area);
        p.scale = userScale;
        p.visible = ledgerVisible;
        p.allowed = allowActivityMenu;
        markLayoutDirty();
    }
    const auto &model = p.model;
    const auto current = p.scope();
    p.setScope(current);
    const bool mentions = current.mentions();
    const auto *net = current.network;
    const auto *config = current.profile;
    const std::string_view name =
        mentions ? std::string_view{"All networks"} : serverDisplayName(model, current.server);
    const auto state = current.state();
    std::string_view nick = mentions                       ? std::string_view{"Mentions"}
                            : net && net->state == "Ready" ? std::string_view(net->nick)
                                                           : std::string_view{};
    if (!mentions && nick.empty() && config)
        if (const auto identity =
                std::find_if(model.identities.begin(), model.identities.end(),
                             [&](const auto &entry) { return entry.name == config->identity; });
            identity != model.identities.end())
            nick = identity->nick;
    using enum Impl::Group;
    const bool connectionError = net && !net->error.empty();
    p.healthy = state == "Ready" && !connectionError;
    const std::string_view feedback = connectionError ? std::string_view(net->error) : model.status;
    // Model status has no severity; protect diagnostics except known routine notifications.
    p.failureFeedback =
        !feedback.empty() &&
        (connectionError ||
         !(feedback.starts_with("New message in ") || feedback == "All buffers marked as read" ||
           (feedback.contains(" · ") && feedback.contains('\n'))));
    p.setControl(Nick, nick,
                 mentions ? "Switch conversation"
                 : current.writable() && config && !current.server.empty()
                     ? "Prefill /nick in the composer"
                     : "Open status details");
    p.setControl(NetworkName, name,
                 !mentions && config ? "Open network settings" : "Manage networks");
    p.setControl(State,
                 p.healthy          ? "Connected"
                 : state == "Ready" ? "Failed"
                                    : state,
                 !mentions && config ? "Show connection actions" : "Open status details");
    p.setControl(Feedback, feedback, "Open status details");
    p.setControl(Chord, keyHint, "Cancel pending key chord");
    p.icons[State]->setGlyph(mentions ? "at" : p.healthy ? "plug" : "plug-connected-x");
    p.icons[Feedback]->setGlyph(p.failureFeedback ? "alert-circle" : "info-circle");
    p.syncHistory();
    const auto &font = themeUiFont();
    const auto backdrop = themeForegroundColor("background");
    const auto ink = [backdrop](std::string_view role) {
        return themePairedForeground(backdrop, themeForegroundColor(role));
    };
    p.activityInk = ink("primary");
    p.mentionInk = ink("error");
    if (const auto hover = themeForegroundColor("hover"); p.hoverFill != hover) {
        p.hoverFill = hover;
        markLayoutDirty();
    }
    if (const auto secondary = ink("secondary"); p.focusInk != secondary) {
        p.focusInk = secondary;
        markLayoutDirty();
    }
    const auto contextualInk = ink("on_surface_variant");
    const auto contextInk = ink("on_surface");
    for (size_t i = 0; i < p.controls.size(); ++i) {
        auto &button = p.controls[i];
        const bool hasCaption = !button.title->text().empty();
        button.area->setHitTestVisible(allowActivityMenu && hasCaption);
        p.enableButton(button, hasCaption);
        const auto color = i == State                           ? (state == "Retrying"                    ? p.focusInk
                                                                   : state == "Failed" || connectionError ? p.mentionInk
                                                                   : p.healthy                            ? p.activityInk
                                                                                                          : p.focusInk)
                           : i == Feedback && p.failureFeedback ? p.mentionInk
                           : i == Chord || i == Nick            ? contextInk
                                                                : contextualInk;
        p.controlInks[i] = color;
        style(*button.title, font, p.scale);
        p.updateInk(button);
        p.icons[i]->setGlyphSize(14 * p.scale);
    }
    if (!ledgerVisible) {
        for (auto &slot : p.slots)
            p.hideButton(slot.button);
        p.hideButton(p.overflow);
        return;
    }

    const auto text = themeForegroundColor("on_surface");
    const auto variant = themeForegroundColor("on_surface_variant");
    const auto restingFill = lerpColor(backdrop, text, .07f);
    for (size_t i = 0; i < p.cardColors.size(); ++i) {
        const auto fill = i ? lerpColor(restingFill, p.hoverFill, .16f) : restingFill;
        const auto foreground = themePairedForeground(fill, text);
        auto network = themePairedForeground(fill, lerpColor(variant, fill, .18f));
        if (network == foreground)
            network = themePairedForeground(fill, lerpColor(foreground, fill, .18f));
        const auto accent = [&](std::string_view role, std::string_view fallback) {
            const auto resolved = themePairedForeground(fill, themeForegroundColor(role));
            return resolved == foreground
                       ? themePairedForeground(fill, themeForegroundColor(fallback))
                       : resolved;
        };
        p.cardColors[i] = {fill,
                           network,
                           foreground,
                           accent("primary", "on_primary_container"),
                           accent("error", "on_error_container"),
                           lerpColor(fill, foreground, .22f)};
    }
    const bool foolRulesChanged =
        p.foolUnread != model.settings.foolUnread || p.fools != model.settings.fools;
    if (foolRulesChanged) {
        p.foolUnread = model.settings.foolUnread;
        p.fools = model.settings.fools;
    }
    bool presentationChanged = false;
    auto cached = p.activity.begin();
    for (const auto &[id, chat] : model.buffers) {
        if (!eligible(chat))
            continue;
        while (cached != p.activity.end() && cached->first < id) {
            cached = p.activity.erase(cached);
            presentationChanged = true;
        }
        const bool inserted = cached == p.activity.end() || cached->first != id;
        if (inserted)
            cached = p.activity.try_emplace(cached, id);
        auto &entry = (cached++)->second;
        const auto name = activityNetwork(model, chat);
        const auto network = model.networks.find(chat.server);
        const std::string_view mapping = network == model.networks.end()
                                             ? std::string_view{"rfc1459"}
                                             : std::string_view(network->second.caseMapping);
        const bool countChanged = entry.count != chat.unread;
        const bool mentionsChanged =
            inserted || countChanged || entry.readMarker != chat.readMarker ||
            entry.size != chat.messages.size() || entry.revision != chat.messagesRevision ||
            entry.caseMapping != mapping || foolRulesChanged;
        const bool mentioned =
            mentionsChanged ? ui_logic::hasUnreadMention(model, chat) : entry.mentioned;
        const bool query = chat.kind == "query";
        if (inserted || countChanged || entry.name != name || entry.target != chat.target ||
            entry.mentioned != mentioned || entry.query != query) {
            entry.caption.clear();
            if (mentioned)
                entry.caption += '@';
            entry.caption.append(name).append(query ? "/DM " : "/").append(chat.target);
            entry.countText = std::to_string(chat.unread);
            entry.caption.append(": ").append(entry.countText);
            entry.chatTitle = query ? "DM " + chat.target : chat.target;
            entry.name = name;
            entry.target = chat.target;
            entry.query = query;
            presentationChanged = true;
        }
        if (mentionsChanged) {
            entry.count = chat.unread;
            entry.readMarker = chat.readMarker;
            entry.size = chat.messages.size();
            entry.revision = chat.messagesRevision;
            entry.caseMapping = mapping;
            entry.mentioned = mentioned;
        }
    }
    if (cached != p.activity.end()) {
        p.activity.erase(cached, p.activity.end());
        presentationChanged = true;
    }
    if (presentationChanged) {
        p.layoutReady = false;
        if (p.popupButton == &p.overflow)
            p.closePopup();
        if (p.overflow.area->pressed())
            p.retire(*p.overflow.area);
        p.ordered.clear();
        p.ordered.reserve(p.activity.size());
        for (const bool mentioned : {true, false})
            for (const auto &entry : p.activity)
                if (entry.second.mentioned == mentioned)
                    p.ordered.push_back(&entry);
        markLayoutDirty();
    }
    p.enableButton(p.overflow, allowActivityMenu);
    p.styleButton(p.overflow, font);
    auto bold = font;
    bold.weight = FontWeight::Bold;
    for (auto &slot : p.slots) {
        p.enableButton(slot.button, allowActivityMenu);
        style(*slot.button.marker, bold, p.scale);
        style(*slot.button.suffix, bold, p.scale);
        style(*slot.button.network, font, p.scale);
        p.styleButton(slot.button, font);
    }
}

void ModeLine::setContentBottomInset(float inset) {
    if (impl_->contentBottomInset == inset)
        return;
    impl_->contentBottomInset = inset;
    markLayoutDirty();
}

LayoutSize ModeLine::doMeasure(Renderer &, const LayoutConstraints &constraints) {
    return constraints.constrain({420 * impl_->scale, 26 * impl_->scale});
}

void ModeLine::doLayout(Renderer &renderer) {
    auto &p = *impl_;
    using enum Impl::Group;
    const float contentHeight = std::max(0.f, height() - p.contentBottomInset);
    const auto &font = themeUiFont();
    auto bold = font;
    bold.weight = FontWeight::Bold;
    const auto measure = [&](std::string_view value) {
        return textWidth(renderer, value, p.scale, font);
    };
    const float ellipsis = measure("…");
    const float gap = std::min(12 * p.scale, width() / 32);
    const float inset = 4 * p.scale;
    const float cardInset = 6 * p.scale, cardGap = 4 * p.scale;
    const float dividerWidth = std::max(1.f, std::round(p.scale));
    const float dividerInset = 4 * p.scale;
    const float dividerSpan = 2 * dividerInset + dividerWidth;
    const float cardChrome = 2 * (cardInset + dividerSpan);
    const float iconPrefix = std::round(14 * p.scale) + std::ceil(4 * p.scale);
    std::array<float, GroupCount> ideal{}, used{}, labelWidths{}, iconWidths{};
    for (size_t i = 0; i < ideal.size(); ++i)
        ideal[i] = measure(p.controls[i].title->text());
    ideal[Nick] = std::min(ideal[Nick], std::ceil(140 * p.scale));
    ideal[NetworkName] = std::min(ideal[NetworkName], std::ceil(200 * p.scale));
    const size_t count = p.visible && height() > 0 ? p.ordered.size() : 0;
    const auto menuWidth = [&](size_t omitted) {
        return omitted ? measure(overflowCaption(omitted)) : 0.f;
    };
    const float overflowMinimum = menuWidth(count);
    const float stateMinimum = p.healthy ? 0.f : ideal[State];
    const float padding =
        std::clamp((width() - stateMinimum - overflowMinimum) / 2, 0.f, 8 * p.scale);
    float remaining = std::max(0.f, width() - 2 * padding);
    const auto take = [&](Impl::Group group, float budget, bool fitWhole = false) {
        const float space = std::min(remaining, budget);
        if (ideal[group] <= 0 || space <= 0)
            return;
        const float icon = space >= ideal[group] + gap + iconPrefix ? iconPrefix : 0.f;
        const float labelGap = std::min(gap, std::max(0.f, space - ideal[group] - icon));
        const float room = std::floor(space - icon - (fitWhole ? labelGap : gap));
        if (room < std::min(ideal[group], ellipsis) || (fitWhole && room < ideal[group]))
            return;
        const float label = std::min(ideal[group], room);
        used[group] = icon + label + (fitWhole ? labelGap : gap);
        iconWidths[group] = icon;
        labelWidths[group] = label;
        remaining -= used[group];
    };
    if (!p.healthy) {
        take(State, std::max(ideal[State], remaining - overflowMinimum), true);
        if (!used[State])
            take(State, remaining);
    }
    const float overflowFloor = std::min(overflowMinimum, remaining);
    take(Chord, remaining - overflowFloor, true);
    if (!used[Chord])
        take(Chord, remaining - overflowFloor);
    const auto identityFloor = [&](Impl::Group group) {
        return ideal[group] ? gap + std::min(ideal[group], ellipsis + measure("M")) : 0.f;
    };
    const float nickFloor = identityFloor(Nick), nameFloor = identityFloor(NetworkName);
    if (p.failureFeedback)
        take(Feedback, remaining - overflowFloor - nickFloor - nameFloor);

    const auto measureBold = [&](std::string_view value) {
        return textWidth(renderer, value, p.scale, bold);
    };
    const float marker = count ? measureBold("@") : 0.f;
    const float markerGap = 2 * p.scale;
    const auto markerSpan = [&](const Impl::Activity &entry) {
        return entry.mentioned ? marker + markerGap : 0.f;
    };
    const float channelMinimum = count ? measure("#…") : 0.f;
    const float dmMinimum = count ? measure("DM …") : 0.f;
    const auto minTitle = [&](const Impl::Activity &entry, float natural) {
        return std::min(natural, entry.query ? dmMinimum : channelMinimum);
    };
    float ledgerFloor = overflowFloor;
    if (count) {
        const auto &first = p.ordered.front()->second;
        const float firstMinimum = std::min(measure(first.name), ellipsis) +
                                   minTitle(first, measure(first.chatTitle)) +
                                   measureBold(first.countText) + markerSpan(first) + cardChrome +
                                   (count > 1 ? cardGap + menuWidth(count - 1) : 0.f);
        if (remaining >= firstMinimum + nickFloor + nameFloor)
            ledgerFloor = std::max(ledgerFloor, firstMinimum);
    }
    const float identityBudget = remaining - ledgerFloor;
    const float nameBudget =
        std::min(ideal[NetworkName] + gap, std::max(nameFloor, identityBudget * .55f));
    take(Nick, identityBudget - nameBudget);
    take(NetworkName, remaining - ledgerFloor);
    if (!used[Nick])
        take(Nick, remaining - ledgerFloor);

    p.measuredChips.clear();
    float naturalTotal = 0;
    for (size_t i = 0; i < count && naturalTotal <= remaining; ++i) {
        const auto &entry = p.ordered[i]->second;
        const auto name = measure(entry.name);
        const auto title = measure(entry.chatTitle);
        const auto countWidth = measureBold(entry.countText);
        p.measuredChips.push_back({name, title, countWidth});
        naturalTotal += name + title + countWidth + markerSpan(entry) + cardChrome +
                        (i + 1 < count ? cardGap : 0.f);
    }
    const bool allFit = p.measuredChips.size() == count && naturalTotal <= remaining;
    float x = padding;
    size_t shown = 0;
    for (; shown < p.measuredChips.size(); ++shown) {
        const auto &[id, entry] = *p.ordered[shown];
        const auto [naturalName, naturalTitle, countWidth] = p.measuredChips[shown];
        const float mark = markerSpan(entry);
        const float trailingGap = shown + 1 < count ? cardGap : 0.f;
        const float fixed = countWidth + mark + cardChrome + trailingGap;
        const float reserve = allFit ? 0.f : menuWidth(count - shown - 1);
        const float namesRoom = std::floor(remaining - reserve - fixed);
        const float nameMin = std::min(naturalName, ellipsis);
        const float titleMin = minTitle(entry, naturalTitle);
        if (namesRoom < nameMin + titleMin)
            break;
        const float nameWidth =
            std::min({naturalName, namesRoom - titleMin,
                      std::max({nameMin, namesRoom - naturalTitle, std::floor(namesRoom * .35f)})});
        const float title = std::min(naturalTitle, namesRoom - nameWidth);

        auto &slot = p.slotAt(shown);
        if (slot.id != id) {
            if (slot.button.area->visible())
                p.retire(*slot.button.area);
            slot.id = id;
            slot.button.area->setTabFocusKey("buffer:" + id);
        }
        auto &button = slot.button;
        const bool nameChanged = button.network->setText(entry.name);
        const bool titleChanged = button.title->setText(entry.chatTitle);
        const bool countChanged = button.suffix->setText(entry.countText);
        if (nameChanged || titleChanged || countChanged ||
            button.marker->visible() != entry.mentioned)
            button.area->setTooltip(entry.caption);
        button.marker->setText("@");
        style(*button.title, font, p.scale);
        style(*button.network, font, p.scale);
        style(*button.marker, bold, p.scale);
        style(*button.suffix, bold, p.scale);
        const float chipWidth = nameWidth + title + fixed - trailingGap;
        p.moveButton(button, x, chipWidth, contentHeight);
        float offset = cardInset;
        const auto placeDivider = [&](Box &divider) {
            divider.setPosition(offset + dividerInset, contentHeight / 4);
            divider.setSize(dividerWidth, contentHeight / 2);
            offset += dividerSpan;
        };
        placeLabel(*button.network, renderer, offset, nameWidth, contentHeight);
        offset += nameWidth;
        placeDivider(*button.dividers[0]);
        placeLabel(*button.marker, renderer, offset, entry.mentioned ? marker : 0, contentHeight);
        offset += mark;
        placeLabel(*button.title, renderer, offset, title, contentHeight);
        offset += title;
        placeDivider(*button.dividers[1]);
        placeLabel(*button.suffix, renderer, offset, countWidth, contentHeight);
        p.enableButton(button, p.allowed);
        x += chipWidth + trailingGap;
        remaining -= chipWidth + trailingGap;
    }
    for (size_t i = shown; i < p.slots.size(); ++i)
        p.hideButton(p.slots[i].button);

    const auto omitted = count - shown;
    const auto overflowLabel = omitted ? overflowCaption(omitted) : std::string{};
    const float overflowText = measure(overflowLabel);
    const bool showOverflow = omitted && overflowText <= remaining;
    if (p.firstOmitted != shown) {
        if (p.popupButton == &p.overflow)
            p.closePopup();
        if (p.overflow.area->pressed())
            p.retire(*p.overflow.area);
    }
    p.firstOmitted = shown;
    if (showOverflow) {
        if (p.overflow.title->setText(overflowLabel))
            p.overflow.area->setTooltip(overflowLabel);
        const float overflowSize = std::min(remaining, overflowText + 2 * inset);
        p.moveButton(p.overflow, x, overflowSize, contentHeight);
        placeLabel(*p.overflow.title, renderer, (overflowSize - overflowText) / 2, overflowText,
                   contentHeight);
        remaining -= overflowSize;
    } else {
        p.hideButton(p.overflow);
    }
    p.layoutReady = p.visible;

    if (p.healthy)
        take(State, remaining, true);
    if (!used[Feedback])
        take(Feedback, remaining);
    take(History, remaining);
    x = width() - padding;
    for (size_t i = used.size(); i-- > 0;) {
        auto &button = p.controls[i];
        const auto group = static_cast<Impl::Group>(i);
        if (labelWidths[i] <= 0 || height() <= 0) {
            p.hideButton(button);
            if (p.hoverGroup == group)
                p.hideTip();
            x -= used[i];
            continue;
        }
        const float buttonWidth = labelWidths[i] + iconWidths[i];
        const float side = (used[i] - buttonWidth) / 2;
        p.moveButton(button, x - used[i], used[i], contentHeight);
        placeLabel(*button.title, renderer, side + iconWidths[i], labelWidths[i], contentHeight);
        auto &icon = *p.icons[i];
        icon.setVisible(iconWidths[i] > 0);
        if (icon.visible()) {
            icon.measure(renderer);
            icon.setPosition(side, (contentHeight - icon.height()) / 2);
        }
        x -= used[i];
    }
    if (p.popupButton &&
        (!p.popupButton->area->visible() || p.anchor(*p.popupButton) != p.popupAnchor))
        p.closePopup();
    if (p.hoverGroup && p.tip->visible()) {
        if (width() <= 20 * p.scale) {
            p.hideTip();
            return;
        }
        const auto group = *p.hoverGroup;
        const float inset = 8 * p.scale;
        const auto fill = themeForegroundColor("surface_container_high");
        p.tip->setFill(fill);
        p.tip->setBorder(themeForegroundColor("outline_variant"), 1.f);
        p.tip->setRadius(inset);
        auto &label = *p.tipText;
        style(label, font, p.scale);
        label.setColor(themePairedForeground(fill, themeForegroundColor("on_surface")));
        label.setMaxWidth(std::min(width(), 420 * p.scale) - 2 * inset);
        label.measure(renderer);
        label.setPosition(inset, inset);
        p.tip->setSize(label.width() + 2 * inset, label.height() + 2 * inset);
        p.tip->setPosition(
            std::clamp(p.controls[group].area->x(), 0.f, std::max(0.f, width() - p.tip->width())),
            -p.tip->height() - 4 * p.scale);
    }
}
} // namespace blast
