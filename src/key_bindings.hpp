#pragma once

#include "commands.hpp"
#include "ui/irc_palette.hpp"
#include <map>
#include <string>
#include <string_view>

namespace blast {
enum class BindingGroup { Messaging, Cursor, Editing, Navigation, Other, Formatting, Irc };

struct BindingCommand {
    std::string label;
    std::string_view defaultChord;
    BindingGroup group;
};

inline const auto &bindingCommands() {
    static const auto commands = [] {
        using enum BindingGroup;
        std::map<std::string, BindingCommand, std::less<>> entries{
            {"switch-buffer", {"Switch conversation", "C-x b", Navigation}},
            {"execute-command", {"Execute command", "M-x", Navigation}},
            {"jump-channel", {"Jump to channel or conversation", "C-x j", Navigation}},
            {"conversation-back", {"Previous viewed conversation", "C-o", Navigation}},
            {"conversation-forward", {"Next viewed conversation", "C-i", Navigation}},
            {"next-activity", {"Next conversation with activity", "C-x C-SPC", Navigation}},
            {"global-mentions", {"Global mentions", "", Other}},
            {"toggle-fools", {"Toggle fools in this conversation", "C-x f", Navigation}},
            {"show-fools", {"Show fools", "", Other}},
            {"hide-fools", {"Hide fools", "", Other}},
            {"message-prev", {"Select previous message", "C-p", Other}},
            {"message-next", {"Select next message", "C-n", Other}},
            {"scroll-up", {"Scroll chat up half a page", "PRIOR", Other}},
            {"scroll-down", {"Scroll chat down half a page", "NEXT", Other}},
            {"message-reply", {"Reply to selected message", "C-r", Other}},
            {"message-react", {"React to selected message", "C-e", Other}},
            {"cancel", {"Cancel", "C-g", Navigation}},
            {"search", {"Search conversation", "C-s", Navigation}},
            {"settings", {"Settings", "C-,", Navigation}},
            {"toggle-people", {"Toggle People", "", Navigation}},
            {"toggle-navigation", {"Toggle navigation", "", Navigation}},
            {"toggle-networks", {"Toggle Networks", "", Navigation}},
            {"mute-buffer", {"Mute conversation", "", Other}},
            {"unmute-buffer", {"Unmute conversation", "", Other}},
            {"archive-buffer", {"Archive conversation", "", Other}},
            {"unarchive-buffer", {"Unarchive conversation", "", Other}},
            {"pin-buffer", {"Pin chat", "", Other}},
            {"unpin-buffer", {"Unpin chat", "", Other}},
            {"send", {"Send message", "RET", Messaging}},
            {"newline", {"Insert newline", "S-RET", Messaging}},
            {"beginning-of-line", {"Beginning of line", "C-a", Cursor}},
            {"end-of-line", {"End of line", "", Cursor}},
            {"backward-char", {"Previous character", "C-b", Cursor}},
            {"forward-char", {"Next character", "C-f", Cursor}},
            {"previous-line", {"Previous line", "", Cursor}},
            {"next-line", {"Next line", "", Cursor}},
            {"backward-word", {"Previous word", "M-b", Cursor}},
            {"forward-word", {"Next word", "M-f", Cursor}},
            {"beginning-of-buffer", {"Beginning of messages", "M-<", Navigation}},
            {"end-of-buffer", {"End of messages", "M->", Navigation}},
            {"kill-word", {"Cut next word", "M-d", Editing}},
            {"backward-kill-word", {"Cut previous word", "M-BACKSPACE", Editing}},
            {"delete-char", {"Delete next character", "C-d", Editing}},
            {"backward-delete", {"Delete previous character", "BACKSPACE", Editing}},
            {"kill-line", {"Cut to end of line", "C-k", Editing}},
            {"kill-region", {"Cut selection", "C-w", Editing}},
            {"copy-region", {"Copy selection", "M-w", Editing}},
            {"yank", {"Paste cut text", "C-y", Editing}},
            {"cycle-spelling", {"Cycle spelling suggestions", "C-;", Editing}},
            {"undo", {"Undo edit", "C-/", Editing}},
            {"redo", {"Redo", "", Editing}},
            {"copy", {"Copy selection", "", Editing}},
            {"cut", {"Cut selection", "", Editing}},
            {"paste", {"Paste clipboard", "", Editing}},
            {"select-all", {"Select all", "", Editing}},
            {"transpose-chars", {"Transpose characters", "C-t", Editing}},
            {"backward-kill-line", {"Cut to beginning of line", "", Editing}},
            {"set-mark", {"Set selection mark", "C-S-SPC", Editing}},
            {"history-previous", {"Previous sent message", "M-p", Messaging}},
            {"history-next", {"Next sent message", "M-n", Messaging}},
            {"complete", {"Complete nickname or command", "TAB", Messaging}},
            {"upload", {"Choose an upload source", "", Messaging}},
            {"connect", {"Connect or reconnect network", "", Other}},
            {"disconnect", {"Disconnect network", "", Other}},
            {"add-server", {"Add network", "", Other}},
            {"manage-networks", {"Manage networks", "", Other}},
            {"open-server-log", {"Open current network’s server log", "", Other}},
            {"load-history", {"Load older messages", "", Other}},
            {"clear-history", {"Clear local history", "", Other}},
            {"mark-read", {"Mark read", "", Other}},
            {"mark-all-read", {"Mark all buffers read", "", Other}},
            {"close-buffer", {"Close buffer", "", Other}},
            {"topic", {"Conversation details / topic", "", Other}},
            {"channel-tools", {"Open channel tools", "", Other}},
            {"settings-uploads", {"Open upload settings", "", Other}},
            {"quit", {"Quit BlastIRC", "", Other}},
            {"message-style:irc", {"Use traditional IRC rows", "", Other}},
            {"message-style:bubbles", {"Use message bubbles", "", Other}},
            {"message-style:stacked", {"Use stacked IRC rows", "", Other}},
            {"message-style:default", {"Use global message layout", "", Other}},
            {"presence:global", {"Use global presence mode", "", Other}},
            {"presence:all", {"Show all presence events", "", Other}},
            {"presence:smart", {"Use smart presence filtering", "", Other}},
            {"presence:hidden", {"Hide presence events", "", Other}},
            {"toggle-formatting", {"Toggle formatting toolbar", "", Formatting}},
            {"format:bold", {"Toggle bold", "", Formatting}},
            {"format:italic", {"Toggle italic", "", Formatting}},
            {"format:underline", {"Toggle underline", "", Formatting}},
            {"format:strike", {"Toggle strikethrough", "", Formatting}},
            {"format:monospace", {"Toggle monospace", "", Formatting}},
            {"format:reverse", {"Reverse text colors", "", Formatting}},
            {"format:reset", {"Reset formatting", "", Formatting}},
        };
        for (std::size_t i = 0; i < ircPalette.size(); ++i) {
            const auto code = (i < 10 ? "0" : "") + std::to_string(i);
            entries.emplace("format:foreground:" + code,
                            BindingCommand{"Foreground IRC color " + code, "", Formatting});
            entries.emplace("format:background:" + code,
                            BindingCommand{"Background IRC color " + code, "", Formatting});
        }
        for (const auto &command : commandCatalog) {
            const auto name = "/" + std::string(command.name);
            entries.emplace(
                name, BindingCommand{name + " · " + std::string(command.description), "", Irc});
        }
        return entries;
    }();
    return commands;
}

inline const std::map<std::string, std::string> &defaultBindings() {
    static const auto bindings = [] {
        std::map<std::string, std::string> entries;
        for (const auto &[name, command] : bindingCommands())
            entries.emplace_hint(entries.end(), name, command.defaultChord);
        return entries;
    }();
    return bindings;
}

inline std::string_view commandDisplayName(std::string_view name) {
    const auto found = bindingCommands().find(name);
    return found == bindingCommands().end() ? name : std::string_view(found->second.label);
}
} // namespace blast
