#include "workspace.hpp"
#include "../model.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

namespace blast {
namespace {
using Id = std::uint64_t;
constexpr Id maxId = std::numeric_limits<std::int64_t>::max();
const std::string serverRailWidthKey = "server_rail_width";
const std::string channelSidebarWidthKey = "channel_sidebar_width";
const std::string compactChannelSidebarWidthKey = "compact_channel_sidebar_width";

std::optional<float> loadWidth(const std::map<std::string, std::string> &settings,
                               const std::string &key, float minimum, float maximum) {
    const auto found = settings.find(key);
    if (found == settings.end())
        return std::nullopt;
    const auto &text = found->second;
    float width = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), width);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(width))
        return std::nullopt;
    return std::clamp(width, minimum, maximum);
}

bool integer(std::string_view text, Id &id) {
    if (text.empty())
        return false;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), id);
    return error == std::errc{} && end == text.data() + text.size();
}
bool paneId(std::string_view text, Id &id) { return integer(text, id) && id > 0 && id <= maxId; }
void appendId(std::string &text, Id id) {
    char digits[32];
    const auto [end, error] = std::to_chars(digits, digits + sizeof(digits), id);
    if (error == std::errc{})
        text.append(digits, end);
}
void assign(std::map<std::string, std::string> &settings, const std::string &key,
            const std::string &value) {
    auto found = settings.find(key);
    if (found == settings.end())
        settings.emplace(key, value);
    else if (found->second != value)
        found->second = value;
}
void saveWidth(std::map<std::string, std::string> &settings, const std::string &key,
               std::optional<float> width, std::string &scratch) {
    if (!width) {
        settings.erase(key);
        return;
    }
    char digits[64];
    const auto [end, error] = std::to_chars(digits, digits + sizeof(digits), *width);
    if (error == std::errc{}) {
        scratch.assign(digits, end);
        assign(settings, key, scratch);
    }
}
} // namespace

Workspace::Workspace(Model &model) : model_(model), publishedActive_(model.activeBuffer) {
    const auto &settings = model_.settings.workspace;
    serverRailWidth_ =
        loadWidth(settings, serverRailWidthKey, minServerRailWidth, maxServerRailWidth);
    channelSidebarWidth_ =
        loadWidth(settings, channelSidebarWidthKey, minChannelSidebarWidth, maxChannelSidebarWidth);
    compactChannelSidebarWidth_ =
        loadWidth(settings, compactChannelSidebarWidthKey, minCompactChannelSidebarWidth,
                  maxCompactChannelSidebarWidth);
    if (const auto mode = settings.find("native_navigation_mode"); mode != settings.end()) {
        if (mode->second == "compact")
            navigationMode = SidebarMode::Compact;
        else if (mode->second == "hidden")
            navigationMode = SidebarMode::Hidden;
    } else if (const auto legacy = settings.find("native_navigation_expanded");
               legacy != settings.end() && legacy->second == "false") {
        navigationMode = SidebarMode::Compact;
    }
    for (const auto &[key, value] : settings) {
        Id id = 0;
        if (!paneId(key, id))
            continue;
        PaneState pane;
        pane.id = id;
        pane.buffer = value;
        panes.emplace(id, std::move(pane));
        saved_.emplace(id, SavedPane{std::to_string(id), value});
        if (id >= nextId_)
            nextId_ = id == maxId ? 1 : id + 1;
    }
    if (const auto found = settings.find("focused"); found != settings.end()) {
        Id id = 0;
        if (paneId(found->second, id))
            focused = id;
    }
    if (!panes.contains(focused) && !panes.empty())
        focused = panes.begin()->first;
    deduplicate();
    reconcile();
    // Native composers may claim focus while being constructed; only the selected pane may ask.
    for (auto &[id, pane] : panes)
        pane.focusComposer = id == focused;
    persist();
}

void Workspace::setServerRailWidth(float width) {
    if (std::isfinite(width))
        serverRailWidth_ = std::clamp(width, minServerRailWidth, maxServerRailWidth);
}
void Workspace::setChannelSidebarWidth(float width) {
    if (std::isfinite(width))
        channelSidebarWidth_ = std::clamp(width, minChannelSidebarWidth, maxChannelSidebarWidth);
}
void Workspace::setCompactChannelSidebarWidth(float width) {
    if (std::isfinite(width))
        compactChannelSidebarWidth_ =
            std::clamp(width, minCompactChannelSidebarWidth, maxCompactChannelSidebarWidth);
}

const std::string &Workspace::fallbackBuffer() const {
    if (model_.buffers.contains(model_.activeBuffer))
        return model_.activeBuffer;
    if (const auto pane = panes.find(focused);
        pane != panes.end() && model_.buffers.contains(pane->second.buffer))
        return pane->second.buffer;
    static const std::string empty;
    return model_.buffers.empty() ? empty : model_.buffers.begin()->first;
}
Id Workspace::freshId() {
    while (panes.contains(nextId_))
        nextId_ = nextId_ == maxId ? 1 : nextId_ + 1;
    const Id id = nextId_;
    nextId_ = nextId_ == maxId ? 1 : nextId_ + 1;
    return id;
}
void Workspace::publish() {
    const auto &buffer = panes.at(focused).buffer;
    if (model_.activeBuffer != buffer)
        model_.activeBuffer = buffer;
    if (publishedActive_ != buffer)
        publishedActive_ = buffer;
}
Id Workspace::open(std::string_view buffer) {
    for (const auto &[id, pane] : panes)
        if (pane.buffer == buffer) {
            if (model_.buffers.contains(pane.buffer))
                focus(id);
            return focused;
        }
    std::string bufferId(buffer);
    if (!model_.buffers.contains(bufferId))
        return focused;
    if (panes.size() == 1 && panes.begin()->second.buffer.empty()) {
        auto &pane = panes.begin()->second;
        pane.buffer = std::move(bufferId);
        focus(pane.id);
        return pane.id;
    }
    const Id id = freshId();
    PaneState pane;
    pane.id = id;
    pane.buffer = std::move(bufferId);
    saved_.emplace(id, SavedPane{std::to_string(id), {}});
    panes.emplace(id, std::move(pane));
    focus(id);
    return id;
}
void Workspace::remove(Id id) {
    if (const auto saved = saved_.find(id); saved != saved_.end()) {
        model_.settings.workspace.erase(saved->second.key);
        saved_.erase(saved);
    }
    panes.erase(id);
}
void Workspace::focus(Id id) {
    const auto selected = panes.find(id);
    if (selected == panes.end())
        return;
    if (const auto previous = panes.find(focused); previous != panes.end() && focused != id)
        previous->second.focusComposer = false;
    focused = id;
    selected->second.focusComposer = true;
    publish();
}
void Workspace::deduplicate() {
    std::set<std::string_view> seen;
    // Keep the focused live PaneState even when its ID sorts after a duplicate.
    if (const auto selected = panes.find(focused); selected != panes.end())
        seen.insert(selected->second.buffer);
    for (auto it = panes.begin(); it != panes.end();) {
        if (it->first == focused || seen.insert(it->second.buffer).second)
            ++it;
        else {
            const Id id = it++->first;
            remove(id);
        }
    }
}
void Workspace::reconcile() {
    const bool externalSelection =
        model_.activeBuffer != publishedActive_ && model_.buffers.contains(model_.activeBuffer);
    const Id previousFocus = focused;
    bool remapped = false;
    for (auto &[id, pane] : panes) {
        auto &saved = saved_.at(id);
        const auto setting = model_.settings.workspace.find(saved.key);
        if (setting != model_.settings.workspace.end() && setting->second != saved.published &&
            model_.buffers.contains(setting->second)) {
            // Core may publish a rename/merge binding after the old buffer disappears.
            // Only reassignment away from a still-live conversation retires its quote.
            if (pane.buffer != setting->second && model_.buffers.contains(pane.buffer))
                pane.reply.clear();
            pane.buffer = setting->second;
            saved.published = setting->second;
            remapped = true;
        }
    }
    if (remapped)
        deduplicate();
    for (auto it = panes.begin(); it != panes.end();) {
        if (model_.buffers.contains(it->second.buffer) || panes.size() == 1)
            ++it;
        else {
            const Id id = it++->first;
            remove(id);
        }
    }
    if (panes.empty()) {
        const Id id = freshId();
        PaneState pane;
        pane.id = id;
        pane.buffer = fallbackBuffer();
        saved_.emplace(id, SavedPane{std::to_string(id), {}});
        panes.emplace(id, std::move(pane));
        focused = id;
    }
    if (!panes.contains(focused))
        focused = panes.begin()->first;
    auto &pane = panes.at(focused);
    if (!model_.buffers.contains(pane.buffer)) {
        const auto &fallback = fallbackBuffer();
        if (pane.buffer != fallback) {
            pane.buffer = fallback;
            pane.reply.clear();
            pane.following = true;
            pane.unseen = 0;
            pane.focusComposer = true;
        }
    }
    if (externalSelection)
        open(model_.activeBuffer);
    else if (focused != previousFocus)
        focus(focused);
    else
        publish();
}
void Workspace::remapBuffer(std::string_view oldId, std::string_view newId) {
    if (oldId == newId)
        return;
    // Callers may borrow either name from a pane that is renamed or merged below.
    const std::string oldBuffer(oldId), newBuffer(newId);
    for (auto &[id, pane] : panes)
        if (pane.buffer == oldBuffer) {
            pane.buffer = newBuffer;
            auto &saved = saved_.at(id);
            saved.published = newBuffer;
            if (auto setting = model_.settings.workspace.find(saved.key);
                setting != model_.settings.workspace.end())
                setting->second = newBuffer;
        }
    if (model_.activeBuffer == oldBuffer)
        model_.activeBuffer = newBuffer;
    if (publishedActive_ == oldBuffer)
        publishedActive_ = newBuffer;
    deduplicate();
}
void Workspace::persist() {
    auto &settings = model_.settings.workspace;
    std::erase_if(settings, [&](const auto &entry) {
        Id id = 0;
        return (integer(entry.first, id) &&
                (!panes.contains(id) || entry.first != saved_.at(id).key)) ||
               entry.first == "tabs" || entry.first == "native_tree" ||
               entry.first == "native_floats" || entry.first.starts_with("native_rect.") ||
               entry.first == "native_navigation_rect" || entry.first == "native_people_rect" ||
               entry.first == "native_navigation_expanded" ||
               entry.first == "native_navigation_visible" ||
               entry.first == "native_navigation_shown" || entry.first == "native_people_shown";
    });
    for (const auto &[id, pane] : panes) {
        auto &saved = saved_.at(id);
        assign(settings, saved.key, pane.buffer);
        if (saved.published != pane.buffer)
            saved.published = pane.buffer;
    }
    serialized_.clear();
    appendId(serialized_, focused);
    assign(settings, "focused", serialized_);
    assign(settings, "visible_buffers", panes.at(focused).buffer);
    assign(settings, "native_navigation_mode", std::string(sidebarModeToken(navigationMode)));
    saveWidth(settings, serverRailWidthKey, serverRailWidth_, serialized_);
    saveWidth(settings, channelSidebarWidthKey, channelSidebarWidth_, serialized_);
    saveWidth(settings, compactChannelSidebarWidthKey, compactChannelSidebarWidth_, serialized_);
}
} // namespace blast
