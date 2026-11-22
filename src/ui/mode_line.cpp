#include "mode_line.hpp"
#include "../model.hpp"
#include "../theme/theme.hpp"
#include "logic.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <core/input/key_symbols.h>
#include <map>
#include <optional>
#include <render/scene/input_area.h>
#include <string>
#include <ui/controls/box.h>
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
    label->setBaselineMode(LabelBaselineMode::TextFixedHeight);
    return label;
}
void text(Label &label, std::string_view value) {
    if (label.setText(value))
        label.setTooltip(std::string(value));
}
void style(Label &label, const TextFont &font, float scale, Color ink) {
    if (label.font().family != font.family)
        label.setFontFamily(font.family);
    if (label.font().features != font.features)
        label.setFontFeatures(font.features);
    label.setFontWeight(font.weight);
    label.setFontSize(12 * scale);
    label.setColor(ink);
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
        Label *title;
        Box *focus;
    };
    struct Slot {
        Button button;
        Label *marker, *suffix;
        std::string id;
    };

    Node &owner;
    Model &model;
    std::function<void(std::string)> execute;
    std::function<void(Node &)> retire;
    std::array<Label *, GroupCount> right;
    ActivityMap activity;
    std::vector<const ActivityMap::value_type *> ordered;
    std::vector<Slot> slots;
    std::vector<std::array<float, 2>> measuredChips;
    Button overflow;
    size_t firstOmitted = 0, historyRemaining = 0;
    decltype(Settings::fools) fools;
    bool foolUnread = false, visible = false, allowed = true, layoutReady = false;
    bool healthy = false, failureFeedback = false;
    float scale = 1;
    Color activityInk{}, mentionInk{}, focusInk{};

    std::shared_ptr<int> alive = std::make_shared<int>(0);
    uint64_t popupGeneration = 0;
    bool popupOpen = false;
    std::array<int, 4> popupAnchor{};

    Impl(Node &owner, Model &model, std::function<void(std::string)> execute,
         std::function<void(Node &)> retire)
        : owner(owner), model(model), execute(std::move(execute)), retire(std::move(retire)),
          overflow(makeButton()) {
        for (auto &label : right)
            label = lineLabel(owner);
        overflow.area->setTabFocusKey("status-activity-overflow");
        overflow.area->setOnClick(
            [this](const InputArea::PointerData &pointer) { openPopup(pointer.serial); });
        overflow.area->setOnKeyDown([this](const InputArea::KeyData &key) {
            if (activateKey(key))
                openPopup(std::nullopt);
        });
    }
    ~Impl() {
        alive.reset();
        closePopup();
    }
    Button makeButton() {
        auto *area = static_cast<InputArea *>(
            owner.insertChildAt(slots.size(), std::make_unique<InputArea>()));
        area->setVisible(false);
        area->setFocusable(true);
        area->setRetainsFocusOnPointerRelease(true);
        area->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
        area->setClipChildren(true);
        auto *focus = child<Box>(*area);
        focus->setHitTestVisible(false);
        focus->setVisible(false);
        const auto rule = [this, area, focus] {
            focus->setFill(focusInk);
            focus->setVisible(area->enabled() && (area->focused() || area->hovered()));
        };
        area->setOnFocusGain(rule);
        area->setOnFocusLoss(rule);
        area->setOnEnter([rule](const InputArea::PointerData &) { rule(); });
        area->setOnLeave(rule);
        return {area, lineLabel(*area), focus};
    }
    void moveButton(Button &button, float x, float width, float height) {
        auto &area = *button.area;
        if (area.pressed() && (area.x() != x || area.width() != width || area.height() != height))
            retire(area);
        area.setPosition(x, 0);
        area.setSize(width, height);
        area.setVisible(true);
        const float rule = std::max(1.f, scale);
        button.focus->setPosition(0, height - rule);
        button.focus->setSize(width, rule);
    }
    void hideButton(Button &button) {
        if (button.area->visible()) {
            retire(*button.area);
            button.area->setVisible(false);
        }
    }
    void enableButton(Button &button, bool enabled) {
        if (button.area->enabled() && !enabled && button.area->visible())
            retire(*button.area);
        button.area->setEnabled(enabled);
        button.focus->setVisible(enabled && (button.area->focused() || button.area->hovered()));
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
            auto button = makeButton();
            button.area->setOnClick(
                [this, index](const InputArea::PointerData &) { activateSlot(index); });
            button.area->setOnKeyDown([this, index](const InputArea::KeyData &key) {
                if (activateKey(key))
                    activateSlot(index);
            });
            slots.push_back({button, lineLabel(*button.area), lineLabel(*button.area), {}});
        }
        return slots[index];
    }
    void styleButton(Button &button, const TextFont &font) {
        style(*button.title, font, scale, activityInk);
        button.focus->setFill(focusInk);
    }
    std::array<int, 4> anchor() const {
        float left, top, rightEdge, bottom;
        Node::transformedBounds(overflow.area, left, top, rightEdge, bottom);
        return {static_cast<int>(std::round(left)), static_cast<int>(std::round(top)),
                std::max(1, static_cast<int>(std::round(rightEdge - left))),
                std::max(1, static_cast<int>(std::round(bottom - top)))};
    }
    void closePopup() {
        ++popupGeneration;
        if (std::exchange(popupOpen, false))
            if (auto *context = owner.popupContext())
                context->closeSelectDropdown();
    }
    void openPopup(std::optional<uint32_t> inputSerial) {
        auto *context = overflow.area->popupContext();
        if (!context || popupOpen || !layoutReady || !allowed || !overflow.area->visible() ||
            firstOmitted >= ordered.size())
            return;

        SelectPopupContext::DropdownRequest request;
        popupAnchor = anchor();
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
        const size_t omitted = ordered.size() - firstOmitted;
        std::vector<std::string> ids;
        ids.reserve(omitted);
        request.menu.reserve(omitted);
        for (size_t i = firstOmitted; i < ordered.size(); ++i) {
            const auto &[id, entry] = *ordered[i];
            ContextMenuEntry item;
            item.label = entry.caption;
            item.value = ids.size();
            item.ellipsize = TextEllipsize::Middle;
            request.menu.push_back(std::move(item));
            ids.push_back(id);
        }
        const auto generation = ++popupGeneration;
        const std::weak_ptr<int> guard = alive;
        popupOpen = true;
        context->openSelectDropdown(
            request, {[this, guard, generation, ids = std::move(ids)](size_t index) {
                          if (guard.expired() || generation != popupGeneration)
                              return;
                          popupOpen = false;
                          ++popupGeneration;
                          if (!layoutReady || !allowed || !overflow.area->visible() ||
                              anchor() != popupAnchor || index >= ids.size() ||
                              index >= ordered.size() - firstOmitted)
                              return;
                          const auto &[id, entry] = *ordered[firstOmitted + index];
                          const auto *chat = eligibleBuffer(ids[index]);
                          if (id == ids[index] && chat && chat->unread == entry.count &&
                              chat->target == entry.target &&
                              (chat->kind == "query") == entry.query &&
                              activityNetwork(model, *chat) == entry.name &&
                              ui_logic::hasUnreadMention(model, *chat) == entry.mentioned)
                              execute("buffer:" + id);
                      },
                      [this, guard, generation] {
                          if (!guard.expired() && generation == popupGeneration)
                              popupOpen = false;
                      }});
    }
};

ModeLine::ModeLine(Model &model, std::function<void(std::string)> execute,
                   std::function<void(Node &)> retire)
    : impl_(std::make_unique<Impl>(*this, model, std::move(execute), std::move(retire))) {
    setClipChildren(true);
    setPaintContained(true);
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
        p.scale = userScale;
        p.visible = ledgerVisible;
        p.allowed = allowActivityMenu;
        markLayoutDirty();
    }
    const auto &model = p.model;
    const auto buffer = model.buffers.find(model.activeBuffer);
    const bool mentions = buffer != model.buffers.end() && buffer->second.kind == "mentions";
    auto network = model.networks.end();
    if (buffer != model.buffers.end() && !mentions)
        network = model.networks.find(buffer->second.server);
    else if (buffer == model.buffers.end() && !model.networks.empty())
        network = model.networks.begin();
    const Network *net = network == model.networks.end() ? nullptr : &network->second;
    const std::string_view serverName = buffer != model.buffers.end()
                                            ? std::string_view(buffer->second.server)
                                        : net ? std::string_view(network->first)
                                              : std::string_view{};
    const auto config = std::find_if(model.servers.begin(), model.servers.end(),
                                     [&](const auto &server) { return server.name == serverName; });
    const std::string_view name =
        mentions ? std::string_view{"All networks"} : serverDisplayName(model, serverName);
    const std::string_view state = mentions                     ? "Read only"
                                   : net && !net->state.empty() ? std::string_view(net->state)
                                                                : "Disconnected";
    std::string_view nick = mentions                       ? std::string_view{"Mentions"}
                            : net && net->state == "Ready" ? std::string_view(net->nick)
                                                           : std::string_view{};
    if (!mentions && nick.empty() && config != model.servers.end())
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
    text(*p.right[Nick], nick);
    text(*p.right[NetworkName], name);
    text(*p.right[State], p.healthy ? "Connected" : state == "Ready" ? "Failed" : state);
    text(*p.right[Feedback], feedback);
    text(*p.right[Chord], keyHint);
    const auto historyRemaining = static_cast<size_t>(
        std::count_if(model.buffers.begin(), model.buffers.end(), [](const auto &entry) {
            return entry.second.initialHistorySync != HistorySyncState::Idle;
        }));
    if (p.historyRemaining != historyRemaining) {
        p.historyRemaining = historyRemaining;
        text(*p.right[History],
             historyRemaining ? "History: " + std::to_string(historyRemaining) + " remaining" : "");
    }
    const auto &font = themeUiFont();
    const auto backdrop = themeForegroundColor("background");
    const auto ink = [backdrop](std::string_view role) {
        return themePairedForeground(backdrop, themeForegroundColor(role));
    };
    p.activityInk = ink("primary");
    p.mentionInk = ink("error");
    p.focusInk = ink("secondary");
    const auto contextualInk = ink("on_surface_variant");
    const auto contextInk = ink("on_surface");
    for (size_t i = 0; i < p.right.size(); ++i) {
        const auto color = i == State ? (state == "Failed" || connectionError ? p.mentionInk
                                         : p.healthy                          ? p.activityInk
                                                                              : p.focusInk)
                           : i == Feedback && p.failureFeedback ? p.mentionInk
                           : i == Chord || i == Nick            ? contextInk
                                                                : contextualInk;
        style(*p.right[i], font, p.scale, color);
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
        p.styleButton(slot.button, font);
        style(*slot.marker, bold, p.scale, p.mentionInk);
        style(*slot.suffix, font, p.scale, p.activityInk);
    }
}

LayoutSize ModeLine::doMeasure(Renderer &, const LayoutConstraints &constraints) {
    return constraints.constrain({420 * impl_->scale, 26 * impl_->scale});
}

void ModeLine::doLayout(Renderer &renderer) {
    auto &p = *impl_;
    using enum Impl::Group;
    const auto &font = themeUiFont();
    auto bold = font;
    bold.weight = FontWeight::Bold;
    const auto measure = [&](std::string_view value) {
        return textWidth(renderer, value, p.scale, font);
    };
    const float ellipsis = measure("…");
    const float gap = std::min(12 * p.scale, width() / 32);
    const float inset = 4 * p.scale;
    std::array<float, GroupCount> ideal{}, used{}, labelWidths{};
    for (size_t i = 0; i < ideal.size(); ++i)
        ideal[i] = measure(p.right[i]->text());
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
        const float labelGap = std::min(gap, std::max(0.f, space - ideal[group]));
        const float room = std::floor(space - (fitWhole ? labelGap : gap));
        if (room < std::min(ideal[group], ellipsis) || (fitWhole && room < ideal[group]))
            return;
        const float label = std::min(ideal[group], room);
        used[group] = label + (fitWhole ? labelGap : gap);
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
        if (slot.button.title->setText(entry.title()) || slot.suffix->text() != entry.suffix() ||
            slot.marker->visible() != entry.mentioned)
            slot.button.area->setTooltip(entry.caption);
        slot.marker->setText("@");
        slot.suffix->setText(entry.suffix());
        p.enableButton(slot.button, p.allowed);
        p.styleButton(slot.button, font);
        style(*slot.marker, bold, p.scale, p.mentionInk);
        style(*slot.suffix, font, p.scale, p.activityInk);
        const float chipWidth = title + fixed;
        p.moveButton(slot.button, x, chipWidth, height());
        placeLabel(*slot.marker, renderer, inset, mark, height());
        placeLabel(*slot.button.title, renderer, inset + mark, title, height());
        placeLabel(*slot.suffix, renderer, inset + mark + title, suffix, height());
        x += chipWidth;
        remaining -= chipWidth;
    }
    for (size_t i = shown; i < p.slots.size(); ++i)
        p.hideButton(p.slots[i].button);

    const auto omitted = count - shown;
    const auto overflowLabel = omitted ? overflowCaption(omitted) : std::string{};
    const float overflowText = measure(overflowLabel);
    const bool showOverflow = omitted && overflowText <= remaining;
    if (p.firstOmitted != shown || (p.popupOpen && !showOverflow)) {
        p.closePopup();
        if (p.overflow.area->pressed())
            p.retire(*p.overflow.area);
    }
    p.firstOmitted = shown;
    if (showOverflow) {
        if (p.overflow.title->setText(overflowLabel))
            p.overflow.area->setTooltip(overflowLabel);
        const float overflowSize = std::min(remaining, overflowText + 2 * inset);
        p.moveButton(p.overflow, x, overflowSize, height());
        placeLabel(*p.overflow.title, renderer, (overflowSize - overflowText) / 2, overflowText,
                   height());
        remaining -= overflowSize;
        if (p.popupOpen && p.anchor() != p.popupAnchor)
            p.closePopup();
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
        placeLabel(*p.right[i], renderer, x - labelWidths[i], labelWidths[i], height());
        x -= used[i];
    }
}
} // namespace blast
