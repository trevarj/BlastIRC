#pragma once
#include <array>
#include <string>
#include <string_view>

namespace blast {
struct CommandInfo {
    std::string_view name, description, usage;
    bool client;
};
inline constexpr std::array commandCatalog{
    CommandInfo{"connect", "Connect a server", "[server]", true},
    CommandInfo{"disconnect", "Disconnect this server", "", true},
    CommandInfo{"clear", "Clear this buffer", "", true},
    CommandInfo{"messagestyle", "Change conversation display", "[bubbles|irc|stacked]", true},
    CommandInfo{"quit", "Quit BlastIRC", "[reason]", true},
    CommandInfo{"me", "Send an action", "action", true},
    CommandInfo{"msg", "Open a query and optionally send", "nick [message]", true},
    CommandInfo{"query", "Open a query and optionally send", "nick [message]", true},
    CommandInfo{"bouncer", "Select a discovered Soju network", "[bind] network-id", true},
    CommandInfo{"join", "Join channels", "#channel[,#channel] [keys]", true},
    CommandInfo{"part", "Leave a channel", "[#channel] [reason]", true},
    CommandInfo{"kick", "Remove someone from a channel", "[#channel] nick [reason]", true},
    CommandInfo{"nick", "Change your nickname", "newnick", true},
    CommandInfo{"away", "Set or clear away status", "[reason]", true},
    CommandInfo{"whois", "Look up a nickname", "[nick]", true},
    CommandInfo{"raw", "Send an IRC command", "command", true},
    CommandInfo{"quote", "Send an IRC command", "command", true},
    CommandInfo{"who", "List matching users", "[mask]", false},
    CommandInfo{"names", "List channel members", "[channels]", false},
    CommandInfo{"list", "List channels", "[channels]", false},
    CommandInfo{"mode", "Inspect or change modes", "target [modes [arguments]]", false},
    CommandInfo{"topic", "Inspect or change a topic", "channel [topic]", false},
    CommandInfo{"search", "Search server history", "target query", false},
};
inline std::string_view commandWord(std::string_view &text) {
    const auto start = text.find_first_not_of(" \t");
    if (start == text.npos) {
        text = {};
        return {};
    }
    text.remove_prefix(start);
    const auto end = text.find_first_of(" \t");
    auto result = text.substr(0, end);
    text = end == text.npos ? std::string_view{} : text.substr(end + 1);
    return result;
}
inline bool commandEqual(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        auto c = a[i];
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        if (c != b[i])
            return false;
    }
    return true;
}
// Only missing required input, never optional arguments or a restatement of typed text.
inline std::string_view missingCommandArguments(std::string_view input, std::string_view kind,
                                                std::string_view channelTypes = "#&+!") {
    if (input.starts_with('/'))
        input.remove_prefix(1);
    const auto name = commandWord(input);
    const auto first = commandWord(input);
    auto isChannel = [&](std::string_view value) {
        return !value.empty() && channelTypes.find(value.front()) != channelTypes.npos;
    };
    if (commandEqual(name, "me"))
        return first.empty() ? "action" : "";
    if (commandEqual(name, "msg") || commandEqual(name, "query"))
        return first.empty() ? "nick" : "";
    if (commandEqual(name, "join"))
        return first.empty() ? "#channel[,#channel]" : "";
    if (commandEqual(name, "nick"))
        return first.empty() ? "newnick" : "";
    if (commandEqual(name, "raw") || commandEqual(name, "quote"))
        return first.empty() ? "command" : "";
    if (commandEqual(name, "whois"))
        return first.empty() && kind != "query" ? "nick" : "";
    if (commandEqual(name, "part"))
        return !isChannel(first) && kind != "channel" ? "#channel" : "";
    if (commandEqual(name, "kick")) {
        if (isChannel(first))
            return commandWord(input).empty() ? "nick" : "";
        if (kind != "channel")
            return first.empty() ? "#channel nick" : "#channel";
        return first.empty() ? "nick" : "";
    }
    if (commandEqual(name, "bouncer"))
        return first.empty() || (commandEqual(first, "bind") && commandWord(input).empty())
                   ? "network-id"
                   : "";
    if (commandEqual(name, "mode"))
        return first.empty() ? "target" : "";
    if (commandEqual(name, "topic"))
        return first.empty() ? "channel" : "";
    if (commandEqual(name, "search"))
        return first.empty() ? "target query" : commandWord(input).empty() ? "query" : "";
    return {};
}
} // namespace blast
