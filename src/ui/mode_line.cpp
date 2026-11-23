#include "mode_line.hpp"
#include "../model.hpp"
#include "../theme/theme.hpp"
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
        std::string name, target, readMarker, caseMapping, caption;
        size_t suffixOffset = 0, size = 0;
        uint64_t revision = 0;
        int count = 0;
        bool mentioned = false, query = false;

        std::string_view title() const {
            const size_t start = mentioned ? 1 : 0;
            return std::string_view(caption).substr(start, suffixOffset - start);
        }
        std::string_view suffix() const { return std::string_view(caption).substr(suffixOffset); }
    };
    using ActivityMap = std::map<std::string, Activity, std::less<>>;
    struct Button {
        InputArea *area;
        Node *content;
        Label *title;
        Box *highlight;
        Box *focus;
        std::optional<Group> group;
        Label *marker = nullptr, *suffix = nullptr;
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

        bool mentions() const { return chat && chat->kind == "mentions"; }
        bool writable() const {
            return chat &&
                   (chat->kind == "channel" || chat->kind == "query" || chat->kind == "server");
        }
        std::string_view state() const {
            return mentions()                           ? "Read only"
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
    std::vector<std::array<float, 2>> measuredChips;
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
    void updateHover(const Button &button) const {
        const auto &area = *button.area;
        const bool available = allowed && !popupButton && area.visible() && area.enabled();
        const bool shown = available && area.hovered();
        if (button.highlight->visible() != shown) {
            button.highlight->setVisible(shown);
            updateInk(button);
        }
    }
    void updateInk(const Button &button) const {
        const auto ink = [&](Color normal) {
            return button.highlight->visible() ? themePairedForeground(hoverFill, normal) : normal;
        };
        const auto normal = button.group ? controlInks[*button.group] : activityInk;
        const auto foreground = ink(normal);
        button.title->setColor(foreground);
        if (button.group)
            icons[*button.group]->setColor(foreground);
        if (button.marker)
            button.marker->setColor(ink(mentionInk));
        if (button.suffix)
            button.suffix->setColor(foreground);
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
        return {model.activeBuffer, server, chat,
                network == model.networks.end() ? nullptr : &network->second,
                profile == model.servers.end() ? nullptr : &*profile};
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
        const Button button{area, content, lineLabel(*content), highlight, focus, group,
                            activityChip ? lineLabel(*content) : nullptr,
                            activityChip ? lineLabel(*content) : nullptr};
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
        button.highlight->setRadius(
            std::min(themeCornerRadius(), std::min(width, highlightHeight) / 2));
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
        entries.reserve(connected ? 3 : 2);
        entries.push_back(
            menuEntry(0, connected ? "Reconnect" : "Connect", connected ? "refresh" : "plug"));
        if (connected)
            entries.push_back(menuEntry(1, "Disconnect", "plug-off"));
        entries.push_back(menuEntry(2, "Open server log", "terminal"));
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
        const auto color = i == State ? (state == "Failed" || connectionError ? p.mentionInk
                                         : p.healthy                          ? p.activityInk
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
            entry.suffixOffset = entry.caption.size();
            entry.caption.append(": ").append(std::to_string(chat.unread));
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
        style(*slot.button.suffix, font, p.scale);
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

    const float marker = count ? textWidth(renderer, "@", p.scale, bold) : 0.f;
    float ledgerFloor = overflowFloor;
    if (count) {
        const auto &first = p.ordered.front()->second;
        const float firstMinimum = (first.mentioned ? marker : 0.f) + measure(first.suffix()) +
                                   ellipsis + 2 * inset + menuWidth(count - 1);
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
        const float suffix = measure(entry.suffix());
        const float title = measure(entry.title());
        p.measuredChips.push_back({title, suffix});
        naturalTotal += title + suffix + (entry.mentioned ? marker : 0.f) + 2 * inset;
    }
    const bool allFit = p.measuredChips.size() == count && naturalTotal <= remaining;
    float x = padding;
    size_t shown = 0;
    for (; shown < p.measuredChips.size(); ++shown) {
        const auto &[id, entry] = *p.ordered[shown];
        const auto [naturalTitle, suffix] = p.measuredChips[shown];
        const float mark = entry.mentioned ? marker : 0.f;
        const float fixed = suffix + mark + 2 * inset;
        const float reserve = allFit ? 0.f : menuWidth(count - shown - 1);
        const float title = std::min(naturalTitle, std::floor(remaining - reserve - fixed));
        if (title < std::min(naturalTitle, ellipsis))
            break;

        auto &slot = p.slotAt(shown);
        if (slot.id != id) {
            if (slot.button.area->visible())
                p.retire(*slot.button.area);
            slot.id = id;
            slot.button.area->setTabFocusKey("buffer:" + id);
        }
        if (slot.button.title->setText(entry.title()) ||
            slot.button.suffix->text() != entry.suffix() ||
            slot.button.marker->visible() != entry.mentioned)
            slot.button.area->setTooltip(entry.caption);
        slot.button.marker->setText("@");
        slot.button.suffix->setText(entry.suffix());
        p.enableButton(slot.button, p.allowed);
        style(*slot.button.marker, bold, p.scale);
        style(*slot.button.suffix, font, p.scale);
        p.styleButton(slot.button, font);
        const float chipWidth = title + fixed;
        p.moveButton(slot.button, x, chipWidth, contentHeight);
        placeLabel(*slot.button.marker, renderer, inset, mark, contentHeight);
        placeLabel(*slot.button.title, renderer, inset + mark, title, contentHeight);
        placeLabel(*slot.button.suffix, renderer, inset + mark + title, suffix, contentHeight);
        x += chipWidth;
        remaining -= chipWidth;
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
