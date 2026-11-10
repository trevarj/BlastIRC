#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace blast::irc {

struct WireMessage {
    std::map<std::string, std::string> tags;
    std::string prefix;
    std::string command;
    std::vector<std::string> params;
};

// Bounded receive envelope, excluding tags and CRLF. Server CAP replies may
// exceed the outbound default before LINELEN is advertised.
inline constexpr std::size_t incomingCoreLimit = 65534;

// Input is one frame without CRLF. Malformed fields, framing bytes and limits
// return nullopt. Repeated spaces after prefix/command/parameters are accepted;
// leading spaces and repeated spaces immediately after tags are rejected.
// Commands are ASCII-uppercased; incoming tag keys are opaque, empty entries are
// ignored, and duplicate tags use the last value. Unknown escapes lose their
// backslash, and a final tag backslash is discarded (IRCv3). Tag limits (incoming
// 8191, outgoing 4096) include '@' and its separating space; outgoing tag data
// alone is limited to 4094 bytes. The core excludes tags and defaults to 510 bytes.
// Explicit core limits outside [510, incomingCoreLimit] are rejected.
// At most 15 parameters are accepted. This validates wire syntax, not command
// arity or server-specific nick rules.
std::optional<WireMessage> parse(std::string_view line, std::size_t coreLimit = 510);

// No CRLF is emitted. Invalid fields or outgoing limits throw invalid_argument.
// Empty tag values serialize without '='; trailing parameters use ':' as needed.
std::string serialize(const WireMessage &message, std::size_t coreLimit = 510);
// Conservative retained-memory accounting for bounded network batch queues;
// includes container storage, string capacities/terminators and map-node links.
std::size_t retainedBytes(const WireMessage &message);
// Profile syntax, not a server-specific alphabet: high-bit bytes are preserved.
// Reject IRC separators/ASCII controls and names too large for initial NICK.
bool validNickname(std::string_view value) noexcept;
// ASCII and strict-rfc1459 are explicit; unknown/empty mappings use RFC1459.
inline std::string casefold(std::string_view value, std::string_view mapping = "rfc1459") {
    std::string result(value);
    const bool rfc = mapping != "ascii";
    for (char &c : result) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        else if (rfc && c == '[')
            c = '{';
        else if (rfc && c == ']')
            c = '}';
        else if (rfc && c == '\\')
            c = '|';
        else if (rfc && mapping != "strict-rfc1459" && c == '^')
            c = '~';
    }
    return result;
}
std::string escapeTag(std::string_view value);
std::string unescapeTag(std::string_view value);

// CLIENTTAGDENY omits the client-only '+' prefix. Exceptions override matching
// denies; preserve namespace wildcard rules used by the client feature policy.
// Capability negotiation is a separate prerequisite for sending client tags.
inline bool clientTagAllowed(std::string_view policy, std::string_view tag) {
    if (tag.starts_with('+'))
        tag.remove_prefix(1);
    const auto matches = [](std::string_view pattern, std::string_view value) {
        std::size_t p = 0, v = 0, star = std::string_view::npos, retry = 0;
        while (v < value.size()) {
            if (p < pattern.size() && pattern[p] == value[v]) {
                ++p;
                ++v;
            } else if (p < pattern.size() && pattern[p] == '*') {
                star = p++;
                retry = v;
            } else if (star != std::string_view::npos) {
                p = star + 1;
                v = ++retry;
            } else
                return false;
        }
        while (p < pattern.size() && pattern[p] == '*')
            ++p;
        return p == pattern.size();
    };
    bool denied = false, allowed = false;
    while (!policy.empty()) {
        const auto comma = policy.find(',');
        auto rule = policy.substr(0, comma);
        const bool exception = rule.starts_with('-');
        if (exception)
            rule.remove_prefix(1);
        if (matches(rule, tag)) {
            if (exception)
                allowed = true;
            else
                denied = true;
        }
        if (comma == std::string_view::npos)
            break;
        policy.remove_prefix(comma + 1);
    }
    return allowed || !denied;
}

struct Timestamp {
    int64_t seconds;
    int nanoseconds;
};
// RFC3339: explicit Z or +/-HH:MM required. Fractions are padded/truncated to
// nanoseconds. Invalid calendar dates, year zero and leap seconds return nullopt.
std::optional<Timestamp> parseTimestampParts(std::string_view value);
// Seconds-only convenience for existing event/storage timestamp fields.
std::optional<int64_t> parseTimestamp(std::string_view value);
std::string timestampNow();
std::string prefixNick(std::string_view prefix);

} // namespace blast::irc
