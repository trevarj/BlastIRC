#include "protocol.hpp"

#include <chrono>
#include <cstdio>
#include <stdexcept>

namespace blast::irc {
namespace {

constexpr std::size_t minimumCoreLimit = 510;
constexpr std::size_t maximumCoreLimit = incomingCoreLimit;
constexpr std::size_t incomingTagLimit = 8191;
constexpr std::size_t outgoingTagLimit = 4096; // 4094 tag-data bytes plus '@' and space.

bool letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

bool digit(char c) { return c >= '0' && c <= '9'; }

char upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }

bool framing(std::string_view value) {
    return value.find_first_of(std::string_view("\r\n\0", 3)) != std::string_view::npos;
}

bool token(std::string_view value) {
    if (value.empty())
        return false;
    for (unsigned char c : value) {
        if (c <= ' ' || c == 127)
            return false;
    }
    return true;
}

bool commandValid(std::string_view value) {
    if (value.empty())
        return false;
    bool letters = true;
    bool numeric = value.size() == 3;
    for (char c : value) {
        letters = letters && letter(c);
        numeric = numeric && digit(c);
    }
    return letters || numeric;
}

bool tagKeyValid(std::string_view key) {
    if (key.starts_with('+'))
        key.remove_prefix(1);
    const auto slash = key.find('/');
    if (slash != std::string_view::npos) {
        const auto vendor = key.substr(0, slash);
        if (vendor.empty())
            return false;
        for (char c : vendor) {
            if (!letter(c) && !digit(c) && c != '-' && c != '.')
                return false;
        }
        key.remove_prefix(slash + 1);
    }
    if (key.empty())
        return false;
    for (char c : key) {
        if (!letter(c) && !digit(c) && c != '-')
            return false;
    }
    return true;
}

bool targetCommand(std::string_view command) {
    const auto matches = [&](std::string_view candidate) {
        if (command.size() != candidate.size())
            return false;
        for (std::size_t i = 0; i < command.size(); ++i) {
            if (upper(command[i]) != candidate[i])
                return false;
        }
        return true;
    };
    return matches("PRIVMSG") || matches("NOTICE") || matches("TAGMSG") || matches("JOIN") ||
           matches("PART") || matches("KICK") || matches("MODE") || matches("TOPIC") ||
           matches("INVITE") || matches("NICK");
}

bool targetValid(std::string_view value) {
    if (!token(value) || value.front() == ':' || value.find('\a') != std::string_view::npos)
        return false;
    // Commas separate targets; an empty element cannot identify a recipient.
    return value.front() != ',' && value.back() != ',' &&
           value.find(",,") == std::string_view::npos;
}

std::size_t escapedSize(std::string_view value) {
    std::size_t size = value.size();
    for (char c : value) {
        if (c == ';' || c == ' ' || c == '\r' || c == '\n' || c == '\\')
            ++size;
    }
    return size;
}

void appendEscaped(std::string &result, std::string_view value) {
    for (char c : value) {
        switch (c) {
        case ';':
            result += "\\:";
            break;
        case ' ':
            result += "\\s";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\\':
            result += "\\\\";
            break;
        default:
            result += c;
            break;
        }
    }
}

void spaces(std::string_view &value) {
    const auto first = value.find_first_not_of(' ');
    value.remove_prefix(first == std::string_view::npos ? value.size() : first);
}

std::string_view takeToken(std::string_view &value) {
    const auto end = value.find(' ');
    const auto result = value.substr(0, end);
    value.remove_prefix(end == std::string_view::npos ? value.size() : end);
    spaces(value);
    return result;
}

bool trailing(std::string_view value) {
    return value.empty() || value.front() == ':' || value.find(' ') != std::string_view::npos;
}

} // namespace

std::optional<WireMessage> parse(std::string_view line, std::size_t coreLimit) {
    if (coreLimit < minimumCoreLimit || coreLimit > maximumCoreLimit || line.empty() ||
        line.size() > incomingTagLimit + coreLimit || framing(line))
        return std::nullopt;
    WireMessage message;
    if (line.front() == '@') {
        const auto end = line.find(' ');
        if (end == std::string_view::npos || end + 1 > incomingTagLimit)
            return std::nullopt;
        auto tags = line.substr(1, end - 1);
        if (tags.empty())
            return std::nullopt;
        while (true) {
            const auto separator = tags.find(';');
            const auto tag = tags.substr(0, separator);
            const auto equals = tag.find('=');
            const auto key = tag.substr(0, equals);
            // IRCv3 requires opaque incoming keys, including future key syntax.
            // Empty entries carry no metadata, but must not discard the message.
            if (!key.empty()) {
                const auto value =
                    equals == std::string_view::npos ? std::string_view{} : tag.substr(equals + 1);
                message.tags.insert_or_assign(std::string(key), unescapeTag(value));
            }
            if (separator == std::string_view::npos)
                break;
            tags.remove_prefix(separator + 1);
        }
        line.remove_prefix(end + 1);
    }
    if (line.empty() || line.size() > coreLimit || line.front() == ' ')
        return std::nullopt;
    if (line.front() == ':') {
        line.remove_prefix(1);
        const auto prefix = takeToken(line);
        if (!token(prefix) || line.empty())
            return std::nullopt;
        message.prefix = prefix;
    }
    const auto command = takeToken(line);
    if (!commandValid(command))
        return std::nullopt;
    message.command.reserve(command.size());
    for (char c : command)
        message.command += upper(c);
    while (!line.empty()) {
        if (message.params.size() == 15)
            return std::nullopt;
        if (line.front() == ':') {
            message.params.emplace_back(line.substr(1));
            break;
        }
        const auto param = takeToken(line);
        message.params.emplace_back(param);
    }
    if (targetCommand(message.command) && !message.params.empty() &&
        !targetValid(message.params.front()))
        return std::nullopt;
    return message;
}

std::string serialize(const WireMessage &message, std::size_t coreLimit) {
    const auto invalid = [] {
        throw std::invalid_argument("Invalid IRC wire message or size limit");
    };
    if (coreLimit < minimumCoreLimit || coreLimit > maximumCoreLimit ||
        !commandValid(message.command) || message.params.size() > 15 ||
        (!message.prefix.empty() && !token(message.prefix)))
        invalid();
    if (targetCommand(message.command) && !message.params.empty() &&
        !targetValid(message.params.front()))
        invalid();

    std::size_t coreSize = message.command.size();
    if (!message.prefix.empty()) {
        if (message.prefix.size() > coreLimit)
            invalid();
        coreSize += message.prefix.size() + 2;
    }
    for (std::size_t i = 0; i < message.params.size(); ++i) {
        const auto &param = message.params[i];
        if (framing(param) || param.size() > coreLimit ||
            (i + 1 < message.params.size() && trailing(param)))
            invalid();
        coreSize += 1 + param.size() + (i + 1 == message.params.size() && trailing(param) ? 1 : 0);
    }
    if (coreSize > coreLimit)
        invalid();

    std::size_t tagSize = message.tags.empty() ? 0 : 1;
    for (const auto &[key, value] : message.tags) {
        if (!tagKeyValid(key) || key.size() > outgoingTagLimit || value.size() > outgoingTagLimit ||
            value.find('\0') != std::string::npos)
            invalid();
        // Each entry accounts for its following semicolon, or the final space.
        tagSize += key.size() + 1 + (value.empty() ? 0 : 1 + escapedSize(value));
        if (tagSize > outgoingTagLimit)
            invalid();
    }

    std::string result;
    result.reserve(tagSize + coreSize);
    if (!message.tags.empty()) {
        result += '@';
        for (const auto &[key, value] : message.tags) {
            result += key;
            if (!value.empty()) {
                result += '=';
                appendEscaped(result, value);
            }
            result += ';';
        }
        result.back() = ' ';
    }
    if (!message.prefix.empty()) {
        result += ':';
        result += message.prefix;
        result += ' ';
    }
    for (char c : message.command)
        result += upper(c);
    for (std::size_t i = 0; i < message.params.size(); ++i) {
        result += ' ';
        if (i + 1 == message.params.size() && trailing(message.params[i]))
            result += ':';
        result += message.params[i];
    }
    return result;
}

std::size_t retainedBytes(const WireMessage &message) {
    std::size_t bytes = sizeof(WireMessage) + message.prefix.capacity() + 1 +
                        message.command.capacity() + 1 +
                        message.params.capacity() * sizeof(std::string);
    for (const auto &param : message.params)
        bytes += param.capacity() + 1;
    for (const auto &[key, value] : message.tags) {
        bytes += sizeof(decltype(message.tags)::value_type) + 4 * sizeof(void *) + key.capacity() +
                 1 + value.capacity() + 1;
    }
    return bytes;
}
bool validNickname(std::string_view value) noexcept {
    return token(value) && value.size() <= 505 && value.front() != '$' &&
           value.find_first_of(",:#&*?!@") == std::string_view::npos;
}

std::string escapeTag(std::string_view value) {
    std::string result;
    result.reserve(escapedSize(value));
    appendEscaped(result, value);
    return result;
}

std::string unescapeTag(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        char c = value[i];
        if (c == '\\') {
            if (++i == value.size())
                break;
            switch (value[i]) {
            case ':':
                c = ';';
                break;
            case 's':
                c = ' ';
                break;
            case 'r':
                c = '\r';
                break;
            case 'n':
                c = '\n';
                break;
            default:
                c = value[i];
                break;
            }
        }
        result += c;
    }
    return result;
}

std::optional<Timestamp> parseTimestampParts(std::string_view value) {
    if (value.size() < 20 || value[4] != '-' || value[7] != '-' ||
        (value[10] != 'T' && value[10] != 't') || value[13] != ':' || value[16] != ':')
        return std::nullopt;
    const auto number = [&](std::size_t start, std::size_t count) {
        int result = 0;
        for (std::size_t i = start; i < start + count; ++i) {
            if (!digit(value[i]))
                return -1;
            result = result * 10 + value[i] - '0';
        }
        return result;
    };
    const int year = number(0, 4), month = number(5, 2), day = number(8, 2);
    const int hour = number(11, 2), minute = number(14, 2), second = number(17, 2);
    if (year < 1 || month < 1 || day < 1 || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59)
        return std::nullopt;
    const std::chrono::year_month_day date{std::chrono::year{year},
                                           std::chrono::month{static_cast<unsigned>(month)},
                                           std::chrono::day{static_cast<unsigned>(day)}};
    if (!date.ok())
        return std::nullopt;
    std::size_t pos = 19;
    int nanoseconds = 0, digits = 0;
    if (value[pos] == '.') {
        const auto start = ++pos;
        while (pos < value.size() && digit(value[pos])) {
            if (digits < 9)
                nanoseconds = nanoseconds * 10 + value[pos] - '0';
            ++digits;
            ++pos;
        }
        if (pos == start || pos == value.size())
            return std::nullopt;
    }
    for (; digits < 9; ++digits)
        nanoseconds *= 10;
    int offset = 0;
    if (value[pos] == 'Z' || value[pos] == 'z') {
        if (pos + 1 != value.size())
            return std::nullopt;
    } else {
        if ((value[pos] != '+' && value[pos] != '-') || value.size() - pos != 6 ||
            value[pos + 3] != ':')
            return std::nullopt;
        const int hours = number(pos + 1, 2), minutes = number(pos + 4, 2);
        if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59)
            return std::nullopt;
        offset = (hours * 60 + minutes) * 60 * (value[pos] == '+' ? 1 : -1);
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::sys_days{date}.time_since_epoch())
                             .count() +
                         hour * 3600 + minute * 60 + second - offset;
    return Timestamp{seconds, nanoseconds};
}

std::optional<int64_t> parseTimestamp(std::string_view value) {
    const auto parsed = parseTimestampParts(value);
    return parsed ? std::optional<int64_t>{parsed->seconds} : std::nullopt;
}

std::string timestampNow() {
    using namespace std::chrono;
    const auto now = floor<seconds>(system_clock::now());
    const auto days = floor<std::chrono::days>(now);
    const year_month_day date{days};
    const hh_mm_ss time{now - days};
    char text[32];
    std::snprintf(text, sizeof(text), "%04d-%02u-%02uT%02lld:%02lld:%02lldZ",
                  static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
                  static_cast<unsigned>(date.day()), static_cast<long long>(time.hours().count()),
                  static_cast<long long>(time.minutes().count()),
                  static_cast<long long>(time.seconds().count()));
    return text;
}

std::string prefixNick(std::string_view prefix) {
    return std::string(prefix.substr(0, prefix.find_first_of("!@")));
}

} // namespace blast::irc
