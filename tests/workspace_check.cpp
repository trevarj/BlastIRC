// Standalone: c++ -std=c++23 -Isrc tests/workspace_check.cpp src/ui/workspace.cpp -o
// workspace-check
#include "../src/model.hpp"
#include "../src/ui/logic.hpp"
#include "../src/ui/workspace.hpp"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <unistd.h>
#include <utility>

using namespace blast;
namespace {
using Id = std::uint64_t;
Model model() {
    Model m;
    for (const auto *name : {"room", "other", "renamed"}) {
        auto &buffer = m.buffers[name];
        buffer.id = name;
        buffer.draft = std::string("draft for ") + name;
        Message message;
        message.id = std::string("message-") + name;
        message.text = std::string("history for ") + name;
        message.history = true;
        buffer.messages.push_back(std::move(message));
    }
    m.activeBuffer = "room";
    return m;
}
std::map<Id, std::string> bindings(const Workspace &workspace) {
    std::map<Id, std::string> result;
    for (const auto &[id, pane] : workspace.panes) {
        assert(pane.id == id);
        result.emplace(id, pane.buffer);
    }
    return result;
}
void retainedSelectionCache() {
    auto m = model();
    Workspace w(m);
    const Id room = w.open("room");
    auto *retained = &w.panes.at(room);
    const auto reply = ui_logic::messageKey(m.buffers.at("room").messages.front());
    retained->reply = reply;
    retained->following = false;
    retained->unseen = 8;
    const Id other = w.open("other");
    const Id renamed = w.open("renamed");
    assert(w.panes.size() == 3);
    assert(w.focused == renamed && m.activeBuffer == "renamed");
    assert(w.open("room") == room && w.panes.size() == 3);
    assert(&w.panes.at(room) == retained && retained->reply == reply);
    assert(!retained->following && retained->unseen == 8 && retained->focusComposer);
    assert(!w.panes.at(other).focusComposer && !w.panes.at(renamed).focusComposer);
    assert(w.open("missing") == room && w.panes.size() == 3);
    w.focus(0);
    assert(w.focused == room);
    w.focus(other);
    assert(w.focused == other && m.activeBuffer == "other");
    w.focus(renamed);
    assert(w.focused == renamed && m.activeBuffer == "renamed");
    w.focus(room);
    m.buffers.at("room").draft = "edited independently";
    w.reconcile();
    assert(&w.panes.at(room) == retained && retained->reply == reply);
    assert(!retained->following && retained->unseen == 8);
    m.buffers["new"].id = "new";
    const Id added = w.open("new");
    assert(added != room && added != other && added != renamed && w.panes.size() == 4);
    assert(&w.panes.at(room) == retained && w.open("room") == room);
    assert(m.buffers.at("room").draft == "edited independently");
    assert(m.buffers.at("other").draft == "draft for other");
    assert(m.buffers.size() == 4); // Cache selection never closes an IRC conversation.
}
void persistenceAndReload() {
    auto m = model();
    m.settings.workspace = {{"10", "other"},
                            {"30", "room"},
                            {"20", "renamed"},
                            {"tabs", "30,10,20"},
                            {"focused", "10"},
                            {"selected_server", "network"},
                            {"last_buffer:network", "other"},
                            {"native_navigation_visible", "false"},
                            {"native_people_shown", "true"},
                            {"native_dialog", "settings"},
                            {"native_dialog_rect", "1 2 3 4"}};
    Workspace w(m);
    assert(
        (bindings(w) == std::map<Id, std::string>{{10, "other"}, {20, "renamed"}, {30, "room"}}));
    assert(w.focused == 10 && m.activeBuffer == "other");
    for (const auto &[id, pane] : w.panes)
        assert(pane.focusComposer == (id == 10));
    w.focus(20);
    w.persist();
    assert(!m.settings.workspace.contains("tabs"));
    assert(m.settings.workspace.at("focused") == "20");
    assert(m.settings.workspace.at("visible_buffers") == "renamed");
    assert(!m.settings.workspace.contains("native_navigation_visible"));
    assert(!m.settings.workspace.contains("native_people_shown"));
    assert(w.navigationMode == SidebarMode::Expanded);
    const auto settings = m.settings.workspace;
    Model restored = m;
    restored.activeBuffer = "room"; // Saved focus, not startup core selection, wins reload.
    Workspace loaded(restored);
    assert(bindings(loaded) == bindings(w) && loaded.focused == w.focused);
    assert(restored.activeBuffer == "renamed" && restored.settings.workspace == settings);
    loaded.persist();
    assert(restored.settings.workspace == settings);
    restored.buffers.erase("other");
    loaded.reconcile();
    loaded.persist();
    assert(!restored.settings.workspace.contains("10"));
    Workspace removedReload(restored);
    assert((bindings(removedReload) == std::map<Id, std::string>{{20, "renamed"}, {30, "room"}}));
}
void navigationPreference() {
    for (const auto *legacy : {"missing", "false", "true", "other"}) {
        auto m = model();
        if (std::string_view(legacy) != "missing")
            m.settings.workspace["native_navigation_expanded"] = legacy;
        Workspace w(m);
        const auto expected =
            std::string_view(legacy) == "false" ? SidebarMode::Compact : SidebarMode::Expanded;
        assert(w.navigationMode == expected);
        assert(m.settings.workspace.at("native_navigation_mode") == sidebarModeToken(expected));
        assert(!m.settings.workspace.contains("native_navigation_expanded"));
    }
    for (const auto *token : {"expanded", "compact", "hidden", "", "false", "Compact", "other"}) {
        for (const auto *legacy : {"false", "true"}) {
            auto m = model();
            m.settings.workspace = {{"native_navigation_mode", token},
                                    {"native_navigation_expanded", legacy}};
            Workspace w(m);
            const auto expected = std::string_view(token) == "compact"  ? SidebarMode::Compact
                                  : std::string_view(token) == "hidden" ? SidebarMode::Hidden
                                                                        : SidebarMode::Expanded;
            assert(w.navigationMode == expected);
            assert(m.settings.workspace.at("native_navigation_mode") == sidebarModeToken(expected));
            Workspace reloaded(m);
            assert(reloaded.navigationMode == expected);
        }
    }

    auto m = model();
    m.settings.workspace = {{"selected_server", "network"},
                            {"last_buffer:network", "other"},
                            {"native_dialog", "settings"},
                            {"native_dialog_rect", "1 2 3 4"},
                            {"extension_metadata", "untouched"}};
    Workspace w(m);
    const Id room = w.open("room");
    auto *retained = &w.panes.at(room);
    const auto reply = ui_logic::messageKey(m.buffers.at("room").messages.front());
    retained->reply = reply;
    retained->following = false;
    retained->unseen = 7;
    for (const auto mode : {SidebarMode::Hidden, SidebarMode::Compact, SidebarMode::Expanded}) {
        w.navigationMode = mode;
        // Presentation aliases must not override or become persisted preferences.
        for (const auto *alias : {"native_navigation_expanded", "native_navigation_visible",
                                  "native_navigation_shown", "native_people_shown"})
            m.settings.workspace[alias] = "false";
        w.open("other");
        w.focus(room);
        w.reconcile();
        w.persist();
        assert(w.navigationMode == mode);
        assert(m.settings.workspace.at("native_navigation_mode") == sidebarModeToken(mode));
        for (const auto *alias : {"native_navigation_expanded", "native_navigation_visible",
                                  "native_navigation_shown", "native_people_shown"})
            assert(!m.settings.workspace.contains(alias));
        assert(&w.panes.at(room) == retained && retained->reply == reply);
        assert(!retained->following && retained->unseen == 7);
        assert(m.settings.workspace.at("selected_server") == "network");
        assert(m.settings.workspace.at("last_buffer:network") == "other");
        assert(m.settings.workspace.at("native_dialog") == "settings");
        assert(m.settings.workspace.at("native_dialog_rect") == "1 2 3 4");
        assert(m.settings.workspace.at("extension_metadata") == "untouched");
        const auto settings = m.settings.workspace;
        Workspace reloaded(m);
        assert(reloaded.navigationMode == mode && m.settings.workspace == settings);
        assert(bindings(reloaded) == bindings(w) && reloaded.focused == room);
    }
}
void sidebarWidths() {
    auto m = model();
    Workspace w(m);
    assert(!w.serverRailWidth() && !w.channelSidebarWidth());
    assert(!w.compactChannelSidebarWidth());
    w.persist();
    assert(!m.settings.workspace.contains("server_rail_width"));
    assert(!m.settings.workspace.contains("channel_sidebar_width"));
    assert(!m.settings.workspace.contains("compact_channel_sidebar_width"));

    const auto selected = w.focused;
    auto *retained = &w.panes.at(selected);
    const auto reply = ui_logic::messageKey(m.buffers.at(retained->buffer).messages.front());
    retained->reply = reply;
    retained->following = false;
    retained->unseen = 7;
    retained->focusComposer = false;
    w.setServerRailWidth(93.25f);
    assert(w.serverRailWidth() == 93.25f && !w.channelSidebarWidth());
    w.persist();
    assert(!m.settings.workspace.contains("channel_sidebar_width"));
    assert(!m.settings.workspace.contains("compact_channel_sidebar_width"));
    w.setChannelSidebarWidth(287.125f);
    w.setCompactChannelSidebarWidth(72.125f);
    for (const auto mode : {SidebarMode::Hidden, SidebarMode::Compact, SidebarMode::Expanded}) {
        w.navigationMode = mode;
        w.reconcile();
        w.persist();
        auto restarted = model();
        restarted.settings.workspace = m.settings.workspace;
        Workspace loaded(restarted);
        assert(loaded.serverRailWidth() == 93.25f);
        assert(loaded.channelSidebarWidth() == 287.125f);
        assert(loaded.compactChannelSidebarWidth() == 72.125f);
        assert(loaded.navigationMode == mode);
        assert(loaded.focused == selected && bindings(loaded) == bindings(w));
        assert(&w.panes.at(selected) == retained && retained->reply == reply);
        assert(!retained->following && retained->unseen == 7 && !retained->focusComposer);
        assert(m.buffers.at("room").draft == "draft for room");
    }
    for (const auto invalid :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
          -std::numeric_limits<float>::infinity()}) {
        w.setServerRailWidth(invalid);
        w.setChannelSidebarWidth(invalid);
        w.setCompactChannelSidebarWidth(invalid);
        assert(w.compactChannelSidebarWidth() == 72.125f);
        assert(w.serverRailWidth() == 93.25f && w.channelSidebarWidth() == 287.125f);
    }
    w.setServerRailWidth(-100);
    w.setChannelSidebarWidth(1);
    w.setCompactChannelSidebarWidth(1);
    assert(w.compactChannelSidebarWidth() == Workspace::minCompactChannelSidebarWidth);
    assert(w.serverRailWidth() == Workspace::minServerRailWidth);
    assert(w.channelSidebarWidth() == Workspace::minChannelSidebarWidth);
    w.persist();
    Workspace minimumReload(m);
    assert(minimumReload.serverRailWidth() == Workspace::minServerRailWidth);
    assert(minimumReload.channelSidebarWidth() == Workspace::minChannelSidebarWidth);
    assert(minimumReload.compactChannelSidebarWidth() == Workspace::minCompactChannelSidebarWidth);
    w.setServerRailWidth(std::numeric_limits<float>::max());
    w.setChannelSidebarWidth(std::numeric_limits<float>::max());
    w.setCompactChannelSidebarWidth(std::numeric_limits<float>::max());
    w.persist();
    Workspace maximumReload(m);
    assert(maximumReload.serverRailWidth() == Workspace::maxServerRailWidth);
    assert(maximumReload.channelSidebarWidth() == Workspace::maxChannelSidebarWidth);
    assert(maximumReload.compactChannelSidebarWidth() == Workspace::maxCompactChannelSidebarWidth);
}

void invalidSidebarWidths() {
    for (const auto *invalid : {"", "nan", "NaN", "inf", "-inf", "1e999", "1e-999", "85px", " 85",
                                "85 ", "85,5", "unrecognized"}) {
        auto m = model();
        m.settings.workspace["server_rail_width"] = invalid;
        m.settings.workspace["channel_sidebar_width"] = "280.5";
        Workspace railInvalid(m);
        assert(!railInvalid.serverRailWidth() && railInvalid.channelSidebarWidth() == 280.5f);
        assert(!m.settings.workspace.contains("server_rail_width"));
        m.settings.workspace["server_rail_width"] = "84.5";
        m.settings.workspace["channel_sidebar_width"] = invalid;
        Workspace channelInvalid(m);
        assert(channelInvalid.serverRailWidth() == 84.5f && !channelInvalid.channelSidebarWidth());
        assert(!m.settings.workspace.contains("channel_sidebar_width"));
        m.settings.workspace["channel_sidebar_width"] = "280.5";
        m.settings.workspace["compact_channel_sidebar_width"] = invalid;
        Workspace compactInvalid(m);
        assert(!compactInvalid.compactChannelSidebarWidth());
        assert(compactInvalid.serverRailWidth() == 84.5f);
        assert(compactInvalid.channelSidebarWidth() == 280.5f);
        assert(!m.settings.workspace.contains("compact_channel_sidebar_width"));
    }
    for (const auto *outside : {"-1000", "0", "1e20"}) {
        auto m = model();
        m.settings.workspace["server_rail_width"] = outside;
        m.settings.workspace["channel_sidebar_width"] = outside;
        m.settings.workspace["compact_channel_sidebar_width"] = outside;
        Workspace w(m);
        const bool maximum = std::string_view(outside) == "1e20";
        assert(w.serverRailWidth() ==
               (maximum ? Workspace::maxServerRailWidth : Workspace::minServerRailWidth));
        assert(w.channelSidebarWidth() ==
               (maximum ? Workspace::maxChannelSidebarWidth : Workspace::minChannelSidebarWidth));
        assert(w.compactChannelSidebarWidth() == (maximum
                                                      ? Workspace::maxCompactChannelSidebarWidth
                                                      : Workspace::minCompactChannelSidebarWidth));
        const auto saved = m.settings.workspace;
        Workspace reloaded(m);
        assert(reloaded.serverRailWidth() == w.serverRailWidth());
        assert(reloaded.channelSidebarWidth() == w.channelSidebarWidth());
        assert(reloaded.compactChannelSidebarWidth() == w.compactChannelSidebarWidth());
        assert(m.settings.workspace == saved);
    }
    auto m = model();
    Workspace w(m);
    w.setServerRailWidth(std::numeric_limits<float>::quiet_NaN());
    w.setChannelSidebarWidth(std::numeric_limits<float>::infinity());
    w.setCompactChannelSidebarWidth(-std::numeric_limits<float>::infinity());
    w.persist();
    assert(!w.serverRailWidth() && !w.channelSidebarWidth());
    assert(!m.settings.workspace.contains("server_rail_width"));
    assert(!m.settings.workspace.contains("channel_sidebar_width"));
    assert(!w.compactChannelSidebarWidth());
    assert(!m.settings.workspace.contains("compact_channel_sidebar_width"));
}

void numericBindingIdentityAndAllocation() {
    auto m = model();
    constexpr Id last = 9223372036854775807ULL;
    m.settings.workspace = {{"1", "room"},
                            {"9223372036854775807", "other"},
                            {"tabs", "9223372036854775807,1"},
                            {"focused", "9223372036854775807"}};
    Workspace w(m);
    assert(w.focused == last && w.panes.at(1).buffer == "room");
    auto *first = &w.panes.at(1);
    auto *lastState = &w.panes.at(last);
    const Id added = w.open("renamed");
    assert(added == 2 && w.panes.size() == 3);
    assert(&w.panes.at(1) == first && &w.panes.at(last) == lastState);
    w.persist();
    assert(m.settings.workspace.at("1") == "room");
    assert(m.settings.workspace.at("9223372036854775807") == "other");
    assert(m.settings.workspace.at("2") == "renamed");
    assert(!m.settings.workspace.contains("tabs"));
    Workspace reloaded(m);
    assert(bindings(reloaded) == bindings(w) && reloaded.focused == added);
}
struct ConfigDirectory {
    std::filesystem::path path;
    std::string previous;
    bool hadPrevious = false;
    ConfigDirectory() {
        if (const auto *value = std::getenv("XDG_CONFIG_HOME")) {
            hadPrevious = true;
            previous = value;
        }
        char name[] = "/tmp/blastirc-workspace-check-XXXXXX";
        const auto *created = mkdtemp(name);
        assert(created);
        path = created;
        assert(setenv("XDG_CONFIG_HOME", path.c_str(), 1) == 0);
        std::filesystem::create_directory(path / "blastirc");
    }
    ~ConfigDirectory() {
        if (hadPrevious)
            setenv("XDG_CONFIG_HOME", previous.c_str(), 1);
        else
            unsetenv("XDG_CONFIG_HOME");
        std::filesystem::remove_all(path);
    }
};
void legacyBindingsIgnoreOrdering() {
    ConfigDirectory config;
    const std::string legacy =
        "[Window][Other###pane2]\nPos=9,9\nSize=999,999\nDockId=0x00000001,0\n"
        "[Docking][Data]\nDockSpace ID=0x00000001 Pos=0,0 Size=999,999 Selected=0x1\n";
    {
        std::ofstream file(config.path / "blastirc" / "layout.ini");
        file << legacy;
    }
    auto m = model();
    m.settings.workspace = {{"9", "room"},
                            {"2", "other"},
                            {"15", "room"},
                            {"30", "renamed"},
                            {"50", "closed"},
                            {"focused", "15"},
                            {"0", "other"},
                            {"18446744073709551615", "room"},
                            {"tabs", "30,9,2,15,50"},
                            {"native_tree", "H(0.8,L(30;0),L(2,9,15;0))"},
                            {"native_floats", "15 0.1 0.2 0.4 0.5"},
                            {"native_rect.2", "9 9 999 999"},
                            {"native_rect.18446744073709551614", "1 2 3 4"},
                            {"native_navigation_rect", "1 2 3 4"},
                            {"native_people_rect", "5 6 7 8"},
                            {"native_navigation_visible", "false"},
                            {"selected_server", "network"},
                            {"last_buffer:network", "room"},
                            {"native_dialog", "settings"},
                            {"native_dialog_rect", "1 2 3 4"}};
    Workspace w(m);
    assert((bindings(w) == std::map<Id, std::string>{{2, "other"}, {15, "room"}, {30, "renamed"}}));
    assert(w.focused == 15 && m.activeBuffer == "room");
    assert(!m.settings.workspace.contains("tabs"));
    assert(m.settings.workspace.at("visible_buffers") == "room");
    for (const auto *key :
         {"9", "50", "0", "18446744073709551615", "native_tree", "native_floats", "native_rect.2",
          "native_rect.18446744073709551614", "native_navigation_rect", "native_people_rect",
          "native_navigation_visible"})
        assert(!m.settings.workspace.contains(key));
    assert(w.navigationMode == SidebarMode::Expanded);
    assert(m.settings.workspace.at("native_navigation_mode") == "expanded");
    assert(m.settings.workspace.at("selected_server") == "network");
    assert(m.settings.workspace.at("last_buffer:network") == "room");
    assert(m.settings.workspace.at("native_dialog") == "settings");
    assert(m.settings.workspace.at("native_dialog_rect") == "1 2 3 4");
    assert(m.buffers.at("room").draft == "draft for room");
    assert(m.buffers.at("room").messages.size() == 1);
    assert(m.buffers.at("room").messages.front().text == "history for room");
    std::ifstream untouched(config.path / "blastirc" / "layout.ini");
    const std::string contents((std::istreambuf_iterator<char>(untouched)), {});
    assert(contents == legacy); // Migration neither reads nor rewrites a foreign layout file.

    auto malformed = model();
    malformed.settings.workspace = {{"2", "room"},
                                    {"5", "other"},
                                    {"8", "renamed"},
                                    {"tabs", "8,nope,8,0,999,18446744073709551616,2,"},
                                    {"focused", "5"}};
    Workspace recovered(malformed);
    assert((bindings(recovered) ==
            std::map<Id, std::string>{{2, "room"}, {5, "other"}, {8, "renamed"}}));
    assert(recovered.focused == 5 && !malformed.settings.workspace.contains("tabs"));

    for (const auto *focus : {"missing", "999", "5junk", "0", "18446744073709551616"}) {
        auto invalid = model();
        invalid.settings.workspace = {
            {"2", "room"}, {"5", "other"}, {"8", "renamed"}, {"tabs", "8,5,2"}, {"focused", focus}};
        Workspace fallback(invalid);
        assert(fallback.focused == 2 && invalid.activeBuffer == "room");
        assert(!invalid.settings.workspace.contains("tabs"));
    }
}
void reconcileCoreSelectionAndClosure() {
    auto m = model();
    Workspace w(m);
    const Id room = w.open("room");
    const Id other = w.open("other");
    w.focus(room);
    auto *roomState = &w.panes.at(room);
    const auto roomReply = ui_logic::messageKey(m.buffers.at("room").messages.front());
    roomState->reply = roomReply;
    roomState->following = false;
    roomState->unseen = 3;
    m.activeBuffer = "renamed";
    w.reconcile();
    const Id renamed = w.focused;
    assert(w.panes.at(renamed).buffer == "renamed" && w.panes.size() == 3);
    assert(&w.panes.at(room) == roomState && roomState->reply == roomReply);
    assert(!roomState->following && roomState->unseen == 3);
    m.activeBuffer = "other";
    w.reconcile();
    assert(w.focused == other && w.panes.size() == 3);
    m.activeBuffer = "does-not-exist";
    w.reconcile();
    assert(w.focused == other && m.activeBuffer == "other");
    m.buffers.erase("room");
    w.reconcile();
    assert(!w.panes.contains(room) && w.focused == other);
    auto *survivor = &w.panes.at(renamed);
    const auto survivorReply = ui_logic::messageKey(m.buffers.at("renamed").messages.front());
    survivor->reply = survivorReply;
    survivor->focusComposer = false;
    m.buffers.erase("other"); // Core selection has not yet caught up with a closed selected buffer.
    w.reconcile();
    assert(w.focused == renamed && survivor->focusComposer);
    assert(survivor->reply == survivorReply && m.activeBuffer == "renamed");
    survivor->following = false;
    survivor->unseen = 9;
    m.buffers.clear();
    w.reconcile();
    assert(w.panes.size() == 1 && &w.panes.at(renamed) == survivor);
    assert(survivor->buffer.empty() && survivor->reply.empty());
    assert(survivor->following && survivor->unseen == 0 && m.activeBuffer.empty());
    w.persist();
    assert(m.settings.workspace.at("visible_buffers").empty());
    m.buffers["new"].id = "new";
    m.activeBuffer = "new";
    w.reconcile();
    assert(w.panes.size() == 1 && w.focused == renamed && survivor->buffer == "new");

    auto fallbackModel = model();
    fallbackModel.activeBuffer = "missing";
    Workspace fallback(fallbackModel);
    assert(fallbackModel.buffers.contains(fallback.panes.at(fallback.focused).buffer));
    Model empty;
    Workspace welcome(empty);
    const Id welcomeId = welcome.focused;
    empty.buffers["connected"].id = "connected";
    assert(welcome.open("connected") == welcomeId && welcome.panes.size() == 1);
}
void numericBindingReassignmentClearsOnlyQuote() {
    auto m = model();
    m.settings.workspace = {{"10", "room"}, {"20", "renamed"}, {"focused", "10"}};
    m.buffers.at("room").draft = "unsent source draft";
    m.buffers.at("other").draft = "unsent destination draft";
    Workspace w(m);
    auto *pane = &w.panes.at(10);
    auto *unrelated = &w.panes.at(20);
    const auto sourceReply = ui_logic::messageKey(m.buffers.at("room").messages.front());
    const auto destinationReply = ui_logic::messageKey(m.buffers.at("other").messages.front());
    const auto unrelatedReply = ui_logic::messageKey(m.buffers.at("renamed").messages.front());
    pane->reply = sourceReply;
    pane->following = false;
    pane->unseen = 7;
    pane->focusComposer = false;
    unrelated->reply = unrelatedReply;
    unrelated->following = false;
    unrelated->unseen = 4;
    unrelated->focusComposer = false;
    const auto *sourceDocument = &m.buffers.at("room");
    const auto *destinationDocument = &m.buffers.at("other");

    // Both chats still exist: this is reassignment, not a published rename or merge.
    m.settings.workspace["10"] = "other";
    w.reconcile();
    assert((bindings(w) == std::map<Id, std::string>{{10, "other"}, {20, "renamed"}}));
    assert(w.focused == 10 && m.activeBuffer == "other");
    assert(&w.panes.at(10) == pane && pane->id == 10 && pane->buffer == "other");
    assert(pane->reply.empty() && !pane->following && pane->unseen == 7);
    assert(!pane->focusComposer);
    assert(&w.panes.at(20) == unrelated && unrelated->reply == unrelatedReply);
    assert(!unrelated->following && unrelated->unseen == 4 && !unrelated->focusComposer);
    assert(m.buffers.size() == 3);
    assert(&m.buffers.at("room") == sourceDocument);
    assert(&m.buffers.at(pane->buffer) == destinationDocument);
    assert(sourceDocument->draft == "unsent source draft");
    assert(destinationDocument->draft == "unsent destination draft");
    assert(m.buffers.at("renamed").draft == "draft for renamed");
    for (const auto *name : {"room", "other", "renamed"}) {
        const auto &document = m.buffers.at(name);
        assert(document.messages.size() == 1);
        assert(document.messages.front().text == std::string("history for ") + name);
        assert(document.messages.front().history);
    }
    assert(ui_logic::messageKey(sourceDocument->messages.front()) == sourceReply);
    assert(ui_logic::messageKey(destinationDocument->messages.front()) == destinationReply);
    assert(ui_logic::messageKey(m.buffers.at("renamed").messages.front()) == unrelatedReply);
    w.persist();
    assert(m.settings.workspace.at("10") == "other");
    assert(m.settings.workspace.at("20") == "renamed");
    Workspace reloaded(m);
    assert(bindings(reloaded) == bindings(w) && reloaded.focused == 10);
}
void renamePreservesLiveState() {
    auto m = model();
    // A message ID matching the old buffer ID is not a buffer reference.
    m.buffers.at("room").messages.front().id = "room";
    const auto reply = ui_logic::messageKey(m.buffers.at("room").messages.front());
    Workspace w(m);
    const Id id = w.open("room");
    auto *pane = &w.panes.at(id);
    pane->reply = reply;
    pane->following = false;
    pane->unseen = 7;
    pane->focusComposer = false;
    const auto *document = &m.buffers.at("room");
    auto node = m.buffers.extract("room");
    node.key() = "moved";
    node.mapped().id = "moved";
    m.buffers.insert(std::move(node));
    m.activeBuffer = "moved";
    m.settings.workspace[std::to_string(id)] = "moved";
    w.remapBuffer(pane->buffer, "moved");
    w.persist(); // Core saves may happen before the next UI frame/reconcile.
    assert(&w.panes.at(id) == pane && pane->buffer == "moved");
    assert(pane->reply == reply && !pane->following && pane->unseen == 7);
    assert(!pane->focusComposer && w.focused == id);
    assert(&m.buffers.at("moved") == document && document->draft == "draft for room");
    assert(m.settings.workspace.at(std::to_string(id)) == "moved");
    assert(m.settings.workspace.at("visible_buffers") == "moved");
    w.reconcile();
    w.remapBuffer("moved", "moved");
    assert(&w.panes.at(id) == pane && pane->reply == reply && !pane->focusComposer);
}
void renameCollision(int focusedPane) {
    auto m = model();
    Workspace w(m);
    const Id source = w.open("room"), destination = w.open("other"), unrelated = w.open("renamed");
    const Id selected = focusedPane == 0 ? source : focusedPane == 1 ? destination : unrelated;
    w.focus(selected);
    const Id winner = selected == destination ? destination : source;
    auto *retained = &w.panes.at(winner);
    auto *selectedState = &w.panes.at(selected);
    for (auto &[id, pane] : w.panes) {
        pane.reply = ui_logic::messageKey(m.buffers.at(pane.buffer).messages.front());
        pane.following = false;
        pane.unseen = static_cast<int>(id + 4);
        pane.focusComposer = false;
    }
    const auto selectedReply = selectedState->reply, retainedReply = retained->reply;
    const auto *document = &m.buffers.at("other");
    auto &merged = m.buffers.at("other").messages;
    const auto &sourceHistory = m.buffers.at("room").messages;
    merged.insert(merged.end(), sourceHistory.begin(), sourceHistory.end());
    m.buffers.erase("room");
    w.remapBuffer(w.panes.at(source).buffer, w.panes.at(destination).buffer);
    w.persist();
    assert((bindings(w) == std::map<Id, std::string>{{winner, "other"}, {unrelated, "renamed"}}));
    assert(w.panes.size() == 2 && w.focused == selected);
    assert(&w.panes.at(selected) == selectedState && selectedState->reply == selectedReply);
    assert(&w.panes.at(winner) == retained && retained->reply == retainedReply);
    assert(retained->buffer == "other" && !retained->following && !retained->focusComposer);
    assert(retained->unseen == static_cast<int>(winner + 4));
    assert(&m.buffers.at("other") == document && document->draft == "draft for other");
    assert(document->messages.size() == 2);
    assert(ui_logic::messageKey(document->messages.at(winner == source ? 1 : 0)) == retainedReply);
    const Id loser = winner == source ? destination : source;
    assert(!m.settings.workspace.contains(std::to_string(loser)));
    assert(m.settings.workspace.at("visible_buffers") ==
           (selected == unrelated ? "renamed" : "other"));
    w.reconcile();
    assert(&w.panes.at(selected) == selectedState && !selectedState->focusComposer);
    Workspace reloaded(m);
    assert(bindings(reloaded) == bindings(w) && reloaded.focused == selected);
}
void settingsRenameConvergence() {
    for (const Id selected : {Id{90}, Id{10}, Id{20}}) {
        auto m = model();
        m.settings.workspace = {{"90", "room"},
                                {"10", "other"},
                                {"20", "renamed"},
                                {"focused", std::to_string(selected)},
                                {"tabs", "90,20,10"}};
        Workspace w(m);
        const Id winner = selected == 90 ? 90 : 10;
        const Id loser = winner == 90 ? 10 : 90;
        auto *selectedState = &w.panes.at(selected);
        auto *winnerState = &w.panes.at(winner);
        for (auto &[id, pane] : w.panes) {
            pane.reply = ui_logic::messageKey(m.buffers.at(pane.buffer).messages.front());
            pane.following = false;
            pane.unseen = static_cast<int>(id);
            pane.focusComposer = false;
        }
        const auto winnerReply = winnerState->reply;
        m.settings.workspace["90"] = "other";
        auto &merged = m.buffers.at("other").messages;
        const auto &sourceHistory = m.buffers.at("room").messages;
        merged.insert(merged.end(), sourceHistory.begin(), sourceHistory.end());
        m.buffers.erase("room");
        w.reconcile(); // Core can publish numeric binding renames before the next frame.
        assert(w.focused == selected && &w.panes.at(selected) == selectedState);
        assert(&w.panes.at(winner) == winnerState && winnerState->buffer == "other");
        assert(winnerState->reply == winnerReply && !winnerState->following);
        assert(merged.size() == 2);
        assert(ui_logic::messageKey(merged.at(winner == 90 ? 1 : 0)) == winnerReply);
        assert(winnerState->unseen == static_cast<int>(winner));
        assert(!selectedState->focusComposer && !w.panes.contains(loser));
        assert(m.activeBuffer == (selected == 20 ? "renamed" : "other"));
        w.persist();
        assert(!m.settings.workspace.contains(std::to_string(loser)));
        assert(m.settings.workspace.at(std::to_string(winner)) == "other");
        Workspace reloaded(m);
        assert(bindings(reloaded) == bindings(w) && reloaded.focused == selected);
    }
}
} // namespace
int main() {
    retainedSelectionCache();
    persistenceAndReload();
    navigationPreference();
    sidebarWidths();
    invalidSidebarWidths();
    numericBindingIdentityAndAllocation();
    legacyBindingsIgnoreOrdering();
    reconcileCoreSelectionAndClosure();
    numericBindingReassignmentClearsOnlyQuote();
    renamePreservesLiveState();
    renameCollision(0);
    renameCollision(1);
    renameCollision(2);
    settingsRenameConvergence();
}
