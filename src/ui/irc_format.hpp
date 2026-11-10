#pragma once
#include "irc_palette.hpp"
#include "logic.hpp"
#include <algorithm>
#include <cstdint>
#include <optional>
#include <render/core/color.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace blast {
struct IrcColor {
    enum class Kind { Foreground, Background, Indexed, Hex };
    Kind kind = Kind::Foreground;
    std::uint32_t value = 0;
    bool operator==(const IrcColor &) const = default;
};
struct IrcStyle {
    bool bold = false, italic = false, underline = false, strike = false;
    bool monospace = false, reverse = false;
    IrcColor foregroundSource{IrcColor::Kind::Foreground};
    IrcColor backgroundSource{IrcColor::Kind::Background};
    // Resolved rendering colors; source descriptors preserve symbolic defaults and palette IDs.
    std::optional<Color> foreground, background;
    bool operator==(const IrcStyle &) const = default;
};
inline void ircResolveColors(IrcStyle &style, Color normalForeground, Color normalBackground) {
    const auto resolve = [&](const IrcColor &source) {
        return source.kind == IrcColor::Kind::Foreground   ? normalForeground
               : source.kind == IrcColor::Kind::Background ? normalBackground
               : source.kind == IrcColor::Kind::Indexed    ? rgbHex(ircPalette[source.value % 16])
                                                           : rgbHex(source.value);
    };
    style.foreground = style.foregroundSource.kind == IrcColor::Kind::Foreground
                           ? std::nullopt
                           : std::optional(resolve(style.foregroundSource));
    style.background = style.backgroundSource.kind == IrcColor::Kind::Background
                           ? std::nullopt
                           : std::optional(resolve(style.backgroundSource));
}
struct IrcRun {
    size_t start, end;
    IrcStyle style;
};
struct IrcDocument {
    std::string text;
    std::vector<IrcRun> runs;
    // Byte offsets, including hidden controls/parameters and the final raw boundary.
    std::vector<size_t> rawToDisplay;
    struct StyleChange {
        size_t rawOffset;
        IrcStyle style;
    };
    std::vector<StyleChange> styleChanges{{0, {}}};
    IrcStyle finalStyle;

    // A formatting token takes effect at its end; lookup never reparses the input.
    const IrcStyle &styleAt(size_t rawOffset) const {
        const auto next = std::upper_bound(
            styleChanges.begin(), styleChanges.end(), rawOffset,
            [](size_t offset, const StyleChange &change) { return offset < change.rawOffset; });
        return next == styleChanges.begin() ? next->style : (next - 1)->style;
    }
};

namespace irc_detail {
// The network is a trust boundary: Pango receives valid UTF-8, never partial codepoints.
inline void appendScalar(std::string_view text, size_t &offset, std::string &out) {
    const auto c = static_cast<unsigned char>(text[offset]);
    size_t count = c < 0x80                 ? 1
                   : c >= 0xc2 && c <= 0xdf ? 2
                   : c >= 0xe0 && c <= 0xef ? 3
                   : c >= 0xf0 && c <= 0xf4 ? 4
                                            : 0;
    bool valid = count && count <= text.size() - offset;
    for (size_t i = 1; valid && i < count; ++i)
        valid = (static_cast<unsigned char>(text[offset + i]) & 0xc0) == 0x80;
    if (valid && count >= 3) {
        const auto next = static_cast<unsigned char>(text[offset + 1]);
        valid = !(c == 0xe0 && next < 0xa0) && !(c == 0xed && next >= 0xa0) &&
                !(c == 0xf0 && next < 0x90) && !(c == 0xf4 && next >= 0x90);
    }
    if (valid) {
        out.append(text.substr(offset, count));
        offset += count;
    } else {
        out += "�";
        ++offset;
    }
}
inline void appendEscaped(std::string &out, std::string_view text) {
    for (const char c : text)
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '\'':
            out += "&apos;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out += c;
            break;
        }
}
inline void appendMarkup(std::string &out, std::string_view text, const IrcStyle &style,
                         bool colors) {
    const bool span = style.bold || style.italic || style.underline || style.strike ||
                      style.monospace || (colors && (style.foreground || style.background));
    if (span) {
        out += "<span";
        if (style.bold)
            out += " weight=\"bold\"";
        if (style.italic)
            out += " style=\"italic\"";
        if (style.underline)
            out += " underline=\"single\"";
        if (style.strike)
            out += " strikethrough=\"true\"";
        if (style.monospace)
            out += " font_family=\"monospace\"";
        if (colors && style.foreground)
            out += " foreground=\"" + formatRgbHex(*style.foreground) + "\"";
        if (colors && style.background)
            out += " background=\"" + formatRgbHex(*style.background) + "\"";
        out += '>';
    }
    appendEscaped(out, text);
    if (span)
        out += "</span>";
}
struct DecorationCode {
    char byte;
    bool IrcStyle::*active;
};
inline constexpr DecorationCode decorationCodes[] = {{'\x02', &IrcStyle::bold},
                                                     {'\x1d', &IrcStyle::italic},
                                                     {'\x1f', &IrcStyle::underline},
                                                     {'\x1e', &IrcStyle::strike},
                                                     {'\x11', &IrcStyle::monospace}};

inline void appendColorValue(std::string &out, const IrcColor &source) {
    if (source.kind == IrcColor::Kind::Indexed)
        out += std::to_string(source.value);
    else {
        constexpr std::string_view digits = "0123456789ABCDEF";
        for (int shift = 20; shift >= 0; shift -= 4)
            out += digits[(source.value >> shift) & 15];
    }
}
inline void appendForeground(std::string &out, const IrcColor &source) {
    if (source.kind == IrcColor::Kind::Foreground)
        out += "\x03"
               "99";
    else {
        out += source.kind == IrcColor::Kind::Indexed ? '\x03' : '\x04';
        appendColorValue(out, source);
    }
    // End the parameters before any selected/typed comma or hex-looking text.
    out += "\x02\x02";
}
} // namespace irc_detail

// Reset-based reconstruction uses source IDs, never the theme-resolved RGB fields.
inline std::string ircFormattingCodes(const IrcStyle &style) {
    const auto &foreground = style.foregroundSource;
    const auto &background = style.backgroundSource;
    using Kind = IrcColor::Kind;
    if (foreground.kind == Kind::Background && background.kind == Kind::Background)
        throw std::invalid_argument("IRC background default cannot occupy both color channels");
    std::string result(1, '\x0f');
    const auto beginColors = [&](bool constructedReverse) {
        // Flip the flag without changing the default sources expected by the program.
        if (constructedReverse != style.reverse)
            result += "\x16\x03"
                      "99,99\x02\x02";
    };
    if (background.kind == Kind::Foreground) {
        if (foreground.kind == Kind::Background) {
            beginColors(true);
            result += '\x16';
        } else if (foreground.kind == Kind::Foreground) {
            beginColors(true);
            result += '\x16';
            irc_detail::appendForeground(result, foreground);
        } else if (style.reverse) {
            // Reverse already puts the symbolic foreground default in the background.
            result += '\x16';
            irc_detail::appendForeground(result, foreground);
        } else {
            beginColors(false);
            irc_detail::appendForeground(result, foreground);
            result += '\x16';
            irc_detail::appendForeground(result, {Kind::Foreground});
            result += '\x16';
        }
    } else if (background.kind == Kind::Background) {
        beginColors(false);
        if (foreground.kind != Kind::Foreground)
            irc_detail::appendForeground(result, foreground);
    } else if (foreground.kind == Kind::Background) {
        beginColors(true);
        irc_detail::appendForeground(result, background);
        result += '\x16';
    } else if (foreground.kind == background.kind) {
        beginColors(false);
        result += foreground.kind == Kind::Indexed ? '\x03' : '\x04';
        irc_detail::appendColorValue(result, foreground);
        result += ',';
        irc_detail::appendColorValue(result, background);
        result += "\x02\x02";
    } else if (background.kind == Kind::Hex) {
        beginColors(true);
        irc_detail::appendForeground(result, background);
        result += '\x16';
        irc_detail::appendForeground(result, foreground);
    } else {
        beginColors(false);
        result += "\x03"
                  "99,";
        irc_detail::appendColorValue(result, background);
        result += "\x02\x02";
        if (foreground.kind != Kind::Foreground)
            irc_detail::appendForeground(result, foreground);
    }
    for (const auto &toggle : irc_detail::decorationCodes)
        if (style.*(toggle.active))
            result += toggle.byte;
    return result;
}

inline std::string ircFormattingTransition(const IrcStyle &before, const IrcStyle &after) {
    std::string toggles;
    for (const auto &toggle : irc_detail::decorationCodes)
        if (before.*(toggle.active) != after.*(toggle.active))
            toggles += toggle.byte;
    if (before.foregroundSource == after.foregroundSource &&
        before.backgroundSource == after.backgroundSource && before.reverse == after.reverse)
        return toggles;
    if (toggles.empty() && before.reverse != after.reverse &&
        before.foregroundSource == after.backgroundSource &&
        before.backgroundSource == after.foregroundSource)
        return "\x16";
    return ircFormattingCodes(after);
}

inline IrcDocument parseIrcFormatting(std::string_view source, Color normalForeground,
                                      Color normalBackground) {
    IrcDocument document;
    document.text.reserve(source.size());
    document.rawToDisplay.resize(source.size() + 1);
    IrcStyle style;
    size_t start = 0;
    auto flush = [&] {
        if (document.text.size() != start)
            document.runs.push_back({start, document.text.size(), style});
        start = document.text.size();
    };
    for (size_t i = 0; i < source.size();) {
        const size_t rawStart = i, displayStart = document.text.size();
        const auto c = static_cast<unsigned char>(source[i]);
        if (c == 2 || c == 29 || c == 31 || c == 30 || c == 17 || c == 15 || c == 3 || c == 4 ||
            c == 22) {
            flush();
            ++i;
            if (c == 2)
                style.bold = !style.bold;
            else if (c == 29)
                style.italic = !style.italic;
            else if (c == 31)
                style.underline = !style.underline;
            else if (c == 30)
                style.strike = !style.strike;
            else if (c == 17)
                style.monospace = !style.monospace;
            else if (c == 15)
                style = {};
            else if (c == 22) {
                style.reverse = !style.reverse;
                std::swap(style.foregroundSource, style.backgroundSource);
            } else if (c == 3) {
                const auto value = ui_logic::colorNumber(source, i);
                style.foregroundSource = value && *value != 99
                                             ? IrcColor{IrcColor::Kind::Indexed, *value}
                                             : IrcColor{IrcColor::Kind::Foreground};
                if (!value)
                    style.backgroundSource = {IrcColor::Kind::Background};
                if (i + 1 < source.size() && source[i] == ',' && source[i + 1] >= '0' &&
                    source[i + 1] <= '9') {
                    ++i;
                    const auto background = ui_logic::colorNumber(source, i);
                    style.backgroundSource = background && *background != 99
                                                 ? IrcColor{IrcColor::Kind::Indexed, *background}
                                                 : IrcColor{IrcColor::Kind::Background};
                }
            } else {
                const auto value = ui_logic::hexColor(source, i);
                style.foregroundSource = value ? IrcColor{IrcColor::Kind::Hex, *value}
                                               : IrcColor{IrcColor::Kind::Foreground};
                if (!value)
                    style.backgroundSource = {IrcColor::Kind::Background};
                else if (i < source.size() && source[i] == ',') {
                    const auto comma = i++;
                    const auto background = ui_logic::hexColor(source, i);
                    if (background)
                        style.backgroundSource = {IrcColor::Kind::Hex, *background};
                    else
                        i = comma;
                }
            }
            if (c == 3 || c == 4 || c == 22)
                ircResolveColors(style, normalForeground, normalBackground);
            if (style != document.styleChanges.back().style)
                document.styleChanges.push_back({i, style});
        } else if (c == '\t') {
            document.text += "    ";
            ++i;
        } else if ((c < 32 && c != '\n') || c == 127)
            ++i;
        else
            irc_detail::appendScalar(source, i, document.text);
        std::fill(document.rawToDisplay.begin() + rawStart, document.rawToDisplay.begin() + i,
                  displayStart);
        document.rawToDisplay[i] = document.text.size();
    }
    flush();
    document.finalStyle = style;
    return document;
}

// Pango's negative line limit applies per paragraph. Preview cards need one
// paragraph so their native line budget caps the whole excerpt. Keep byte offsets
// stable for IRC runs; the original message and selection document stay untouched.
inline void ircSingleParagraph(IrcDocument &document) {
    auto &text = document.text;
    for (size_t at = 0; at < text.size(); ++at) {
        const auto tail = std::string_view(text).substr(at);
        const size_t bytes = tail.starts_with("\n")         ? 1
                             : tail.starts_with("\xC2\x85") ? 2
                             : tail.starts_with("\xE2\x80\xA8") || tail.starts_with("\xE2\x80\xA9")
                                 ? 3
                                 : 0;
        if (bytes) {
            std::fill_n(text.begin() + at, bytes, ' ');
            at += bytes - 1;
        }
    }
}

// Only parsed display text belongs here. Colorless markup retains identical shaping/text.
inline std::string ircMarkup(std::string_view text, const IrcStyle &style, bool colors = true) {
    std::string result;
    result.reserve(text.size());
    irc_detail::appendMarkup(result, text, style, colors);
    return result;
}
inline std::string ircMarkup(const IrcDocument &document, bool colors = true) {
    std::string result;
    result.reserve(document.text.size());
    for (const auto &run : document.runs)
        irc_detail::appendMarkup(
            result, std::string_view(document.text).substr(run.start, run.end - run.start),
            run.style, colors);
    return result;
}
} // namespace blast
