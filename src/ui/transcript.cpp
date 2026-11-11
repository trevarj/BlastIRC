#include "transcript.hpp"
#include "../fx/effects.hpp"
#include "../fx/tween.hpp"
#include "../irc/protocol.hpp"
#include "../model.hpp"
#include "../render/graphics.hpp"
#include "../theme/theme.hpp"
#include "channel_avatar.hpp"
#include "foundry_avatar.hpp"
#include "irc_format.hpp"
#include "logic.hpp"
#include "media_preview.hpp"
#include "message_selection.hpp"
#include "presentation.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <core/input/key_modifiers.h>
#include <core/text_clipboard.h>
#include <cstdio>
#include <ctime>
#include <limits>
#include <optional>
#include <render/core/renderer.h>
#include <render/scene/input_area.h>
#include <render/scene/input_dispatcher.h>
#include <render/scene/rect_node.h>
#include <render/scene/text_node.h>
#include <spawn.h>
#include <sys/wait.h>
#include <ui/controls/button.h>
#include <ui/controls/glyph.h>
#include <ui/controls/image.h>
#include <ui/controls/label.h>
#include <ui/controls/scroll_view.h>
#include <ui/controls/select_popup_context.h>
#include <ui/controls/spinner.h>
#include <unordered_map>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

extern char **environ;

namespace blast {
namespace {
constexpr size_t absent = std::numeric_limits<size_t>::max();
// Wayland cursor-shape-v1 values, matching the SDK's uint32_t cursor API.
constexpr uint32_t cursorArrow = 1, cursorPointer = 4, cursorText = 9;
// Shared across pane destruction so an outstanding desktop launcher is still reaped.
std::vector<pid_t> urlOpeners;
// UI-only entry identities must not collide between retained transcript views.
uint64_t nextFocusMessageIdentity = 1;
constexpr std::array<std::string_view, 6> quickReactions = {"👍", "❤️", "😂", "🎉", "👀", "✅"};

bool capability(const Model &model, const Buffer &buffer, std::string_view name) {
    const auto network = model.networks.find(buffer.server);
    if (network == model.networks.end())
        return false;
    for (const auto &entry : network->second.caps)
        if (entry == name ||
            (entry.starts_with(name) && entry.size() > name.size() && entry[name.size()] == '='))
            return true;
    return false;
}
bool replyAllowed(const Model &model, const Buffer &buffer) {
    const auto network = model.networks.find(buffer.server);
    return network != model.networks.end() && network->second.state == "Ready" &&
           capability(model, buffer, "message-tags") &&
           irc::clientTagAllowed(network->second.clientTagDeny, "+reply");
}
bool ownReaction(const Network &network, const Message &message, std::string_view emoji) {
    const auto reaction = std::find_if(message.reactions.begin(), message.reactions.end(),
                                       [&](const auto &entry) { return entry.first == emoji; });
    if (reaction == message.reactions.end() || network.nick.empty())
        return false;
    const auto nick = irc::casefold(network.nick, network.caseMapping);
    return std::any_of(reaction->second.begin(), reaction->second.end(), [&](const auto &owner) {
        return irc::casefold(owner, network.caseMapping) == nick;
    });
}
bool reactionAllowed(const Model &model, const Buffer &buffer, const Message &message,
                     std::string_view emoji) {
    const auto network = model.networks.find(buffer.server);
    return network != model.networks.end() && network->second.state == "Ready" &&
           replyAllowed(model, buffer) &&
           irc::clientTagAllowed(network->second.clientTagDeny,
                                 ownReaction(network->second, message, emoji) ? "+draft/unreact"
                                                                              : "+draft/react");
}
bool quotable(const Buffer &buffer, const Message &message) {
    return buffer.kind != "search" && buffer.kind != "mentions" && !message.deleted &&
           !message.presence && !message.id.empty();
}
bool referenceable(const Buffer &buffer, const Message &message) {
    return quotable(buffer, message) && !message.pending && !message.id.starts_with("local:");
}
bool reactionPickerAllowed(const Model &model, const Buffer &buffer, const Message &message) {
    if (!referenceable(buffer, message) || !replyAllowed(model, buffer))
        return false;
    const auto &network = model.networks.at(buffer.server);
    if (network.state != "Ready")
        return false;
    if (irc::clientTagAllowed(network.clientTagDeny, "+draft/react"))
        return true;
    if (!irc::clientTagAllowed(network.clientTagDeny, "+draft/unreact"))
        return false;
    return std::any_of(
        message.reactions.begin(), message.reactions.end(),
        [&](const auto &reaction) { return ownReaction(network, message, reaction.first); });
}
bool typedActivity(const Message &message) {
    return message.presence && !message.action && message.activity != ActivityKind::None;
}
constexpr bool allowedUrl(std::string_view url) {
    for (const unsigned char c : url)
        if (c <= 32 || c == 127)
            return false;
    const auto colon = url.find(':');
    if (colon == std::string_view::npos)
        return false;
    const auto scheme = url.substr(0, colon);
    auto is = [&](std::string_view candidate) {
        if (scheme.size() != candidate.size())
            return false;
        for (size_t i = 0; i < scheme.size(); ++i) {
            char c = scheme[i];
            if (c >= 'A' && c <= 'Z')
                c += 'a' - 'A';
            if (c != candidate[i])
                return false;
        }
        return true;
    };
    return colon + 1 < url.size() &&
           (is("http") || is("https") || is("irc") || is("ircs") || is("mailto"));
}
static_assert(allowedUrl("https://example.org/a?b=c") && allowedUrl("HTTPS://example.org") &&
              !allowedUrl("javascript:alert(1)") && !allowedUrl("file:///etc/passwd") &&
              !allowedUrl("https://example.org\n--help") && !allowedUrl("--help"));

std::string localDate(int64_t timestamp, const char *format) {
    if (!timestamp)
        return {};
    const std::time_t value = timestamp;
    std::tm local{};
    localtime_r(&value, &local);
    char text[128]{};
    std::strftime(text, sizeof(text), format, &local);
    return text;
}
std::string clockText(const Message &message) {
    return message.timestamp           ? localDate(message.timestamp, "%H:%M")
           : message.time.size() >= 16 ? message.time.substr(11, 5)
                                       : "--:--";
}
bool sendIndicator(const Message &message) {
    return message.own &&
           (message.sendStatus == SendStatus::Queued || message.sendStatus == SendStatus::Sent ||
            message.sendStatus == SendStatus::Confirmed);
}
unsigned char nickByte(unsigned char c, std::string_view mapping) {
    if (c >= 'A' && c <= 'Z')
        return c + ('a' - 'A');
    if (mapping != "ascii") {
        if (c == '[')
            return '{';
        if (c == ']')
            return '}';
        if (c == '\\')
            return '|';
        if (mapping != "strict-rfc1459" && c == '^')
            return '~';
    }
    return c;
}
bool sameNick(std::string_view left, std::string_view right, std::string_view mapping) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(),
                      [&](unsigned char a, unsigned char b) {
                          return nickByte(a, mapping) == nickByte(b, mapping);
                      });
}
uint32_t nickHash(std::string_view nick, std::string_view mapping = "ascii") {
    uint32_t hash = 2166136261u;
    for (unsigned char c : nick)
        hash = (hash ^ nickByte(c, mapping)) * 16777619u;
    return hash;
}
} // namespace
Color nickColor(const Model &model, const std::string &nick, const std::string &server) {
    const auto network = model.networks.find(server);
    const auto mapping = network == model.networks.end() ? "rfc1459" : network->second.caseMapping;
    if (!model.settings.nickColors.empty()) {
        Color color;
        for (const auto &[name, value] : model.settings.nickColors)
            if (sameNick(name, nick, mapping) && tryParseHexColor(value, color))
                return color;
    }
    const auto hash = nickHash(nick, mapping);
    if (model.settings.nickPalette == "THEME")
        return withAlpha(themeNickColor(hash), 1.f);
    return hsv(model.settings.nickPalette == "CLASSIC" ? (hash % 12) / 12.f : (hash % 360) / 360.f,
               model.settings.nickPalette == "VIVID" ? .85f : .6f,
               model.settings.nickPalette == "CLASSIC" ? .85f : .92f);
}
namespace {
size_t signature(const Message &message, const Message *previous) {
    size_t hash = 0;
    auto mix = [&](size_t value) { hash ^= value + 0x9e3779b9 + (hash << 6) + (hash >> 2); };
    for (const auto *text :
         {&message.text, &message.nick, &message.reply, &message.id, &message.time})
        mix(std::hash<std::string>{}(*text));
    mix(std::hash<int64_t>{}(message.timestamp));
    mix(unsigned(message.deleted) | unsigned(message.presence) << 1 |
        unsigned(message.action) << 2 | unsigned(message.pending) << 3 |
        unsigned(message.own) << 4);
    mix(static_cast<size_t>(message.activity));
    mix(static_cast<size_t>(message.sendStatus));
    for (const auto &[emoji, nicks] : message.reactions) {
        mix(std::hash<std::string>{}(emoji));
        for (const auto &nick : nicks)
            mix(std::hash<std::string>{}(nick));
    }
    if (previous) {
        mix(std::hash<std::string>{}(previous->nick));
        mix(std::hash<int64_t>{}(previous->timestamp));
        mix(std::hash<std::string>{}(previous->time));
        mix(unsigned(previous->action) | unsigned(previous->presence) << 1);
    }
    return hash;
}
struct RichLink {
    size_t start, end;
    std::string url;
};
std::vector<RichLink> detectLinks(const IrcDocument &document) {
    std::vector<RichLink> links;
    for (size_t startByte = 0; startByte < document.text.size();) {
        while (startByte < document.text.size() &&
               std::isspace(static_cast<unsigned char>(document.text[startByte])))
            ++startByte;
        size_t end = startByte;
        while (end < document.text.size() &&
               !std::isspace(static_cast<unsigned char>(document.text[end])))
            ++end;
        size_t first = startByte;
        while (first < end && (document.text[first] == '(' || document.text[first] == '[' ||
                               document.text[first] == '<' || document.text[first] == '"'))
            ++first;
        size_t last = end;
        while (last > first &&
               std::string_view(".,;!?>\"").find(document.text[last - 1]) != std::string_view::npos)
            --last;
        for (const auto &pair : {std::pair('(', ')'), std::pair('[', ']')}) {
            while (last > first && document.text[last - 1] == pair.second &&
                   std::count(document.text.begin() + first, document.text.begin() + last,
                              pair.second) > std::count(document.text.begin() + first,
                                                        document.text.begin() + last, pair.first))
                --last;
        }
        const auto url = std::string_view(document.text).substr(first, last - first);
        if (allowedUrl(url))
            links.push_back({first, last, std::string(url)});
        startByte = end;
    }
    return links;
}
std::string markup(std::string_view text, const IrcStyle &style, bool link) {
    if (!link)
        return ircMarkup(text, style);
    auto linked = style;
    linked.underline = true;
    return ircMarkup(text, linked);
}
struct Fragment {
    std::string text, url;
    TextFont font;
    std::optional<Color> color;
    float x = 0, y = 0, width = 0, height = 0, baseline = 0;
    size_t linkStart = absent, displayBegin = 0, displayEnd = 0;
    MessageSelectionFragment selection;
    bool selectionReady = false;
    std::string nick;
    float inset = 0;
};
struct VisualLine {
    float top = 0, height = 0;
    size_t firstFragment = 0, endFragment = 0, displayBegin = 0, displayEnd = 0;
};
struct RichLayout {
    std::vector<Fragment> fragments;
    std::vector<VisualLine> lines;
    float width = 0, height = 0;
};
std::vector<NickAnnotation> annotateNicksWithLinks(const Model &model, const Buffer &buffer,
                                                   const IrcDocument &document,
                                                   std::string_view author,
                                                   const std::vector<RichLink> &links) {
    std::vector<NickAnnotation> mentions;
    const auto network = model.networks.find(buffer.server);
    const auto mapping = network == model.networks.end() ? "rfc1459" : network->second.caseMapping;
    auto nickCharacter = [](gunichar c) {
        return g_unichar_isalnum(c) || g_unichar_ismark(c) ||
               (c < 128 && std::string_view("-_[]\\`^{}|").find(char(c)) != std::string_view::npos);
    };
    for (size_t at = 0; at < document.text.size();) {
        const auto *start = document.text.c_str() + at;
        if (!nickCharacter(g_utf8_get_char(start))) {
            at = g_utf8_next_char(start) - document.text.c_str();
            continue;
        }
        size_t end = at;
        while (end < document.text.size() &&
               nickCharacter(g_utf8_get_char(document.text.c_str() + end)))
            end = g_utf8_next_char(document.text.c_str() + end) - document.text.c_str();
        const bool url = std::any_of(links.begin(), links.end(), [&](const auto &link) {
            return at < link.end && end > link.start;
        });
        if (!url) {
            const auto word = std::string_view(document.text).substr(at, end - at);
            for (const auto &nick : buffer.nicks)
                if (sameNick(nick.name, word, mapping)) {
                    mentions.push_back({at, end, nick.name});
                    break;
                }
            if (mentions.empty() || mentions.back().begin != at) {
                if (sameNick(author, word, mapping))
                    mentions.push_back({at, end, std::string(author)});
                else if (network != model.networks.end() &&
                         sameNick(network->second.nick, word, mapping))
                    mentions.push_back({at, end, network->second.nick});
                else if (buffer.kind == "query" && sameNick(buffer.target, word, mapping))
                    mentions.push_back({at, end, buffer.target});
            }
        }
        at = end;
    }
    return mentions;
}

RichLayout wrap(Renderer &renderer, const IrcDocument &document, float size, float width,
                float minimumLine, int columns, const Model &model, const Buffer &buffer,
                const std::string &author) {
    RichLayout result;
    const auto links = detectLinks(document);
    std::vector<size_t> boundaries;
    const auto characters = g_utf8_strlen(document.text.c_str(), document.text.size());
    std::vector<PangoLogAttr> attrs(characters + 1);
    pango_get_log_attrs(document.text.c_str(), document.text.size(), -1,
                        pango_language_get_default(), attrs.data(), attrs.size());
    std::vector<size_t> legal;
    size_t byte = 0;
    for (size_t i = 0; i < attrs.size(); ++i) {
        if (attrs[i].is_cursor_position)
            boundaries.push_back(byte);
        if (attrs[i].is_line_break && attrs[i].is_cursor_position)
            legal.push_back(byte);
        if (byte < document.text.size())
            byte = g_utf8_next_char(document.text.c_str() + byte) - document.text.c_str();
    }
    const auto mentions = annotateNicksWithLinks(model, buffer, document, author, links);
    auto pieces = [&](size_t begin, size_t end, float y, bool emit) {
        float x = 0;
        for (const auto &run : document.runs) {
            size_t at = std::max(begin, run.start);
            const size_t finish = std::min(end, run.end);
            TextFont font = themeMessageFont();
            if (run.style.bold)
                font.weight = FontWeight::Bold;
            if (run.style.monospace)
                font.family = "monospace";
            while (at < finish) {
                size_t next = finish;
                const RichLink *link = nullptr;
                for (const auto &candidate : links) {
                    if (candidate.start <= at && at < candidate.end) {
                        link = &candidate;
                        next = std::min(next, candidate.end);
                        break;
                    }
                    if (candidate.start > at) {
                        next = std::min(next, candidate.start);
                        break;
                    }
                }
                std::string nick;
                if (!link)
                    for (const auto &mention : mentions) {
                        if (mention.begin <= at && at < mention.end) {
                            nick = mention.nick;
                            next = std::min(next, mention.end);
                            break;
                        }
                        if (mention.begin > at) {
                            next = std::min(next, mention.begin);
                            break;
                        }
                    }
                auto style = run.style;
                auto pieceFont = font;
                if (!nick.empty()) {
                    style.foreground = nickColor(model, nick, buffer.server);
                    style.background.reset();
                    style.bold = true;
                    pieceFont.weight = FontWeight::Bold;
                }
                auto text = markup(std::string_view(document.text).substr(at, next - at), style,
                                   link != nullptr);
                const auto metrics = renderer.measureText(
                    text, size, pieceFont, 0, 1, TextAlign::Start, TextEllipsize::End, true);
                const float cellWidth = metrics.width;
                if (emit)
                    result.fragments.push_back({std::move(text),
                                                link ? link->url : std::string{},
                                                pieceFont,
                                                style.foreground,
                                                x,
                                                y,
                                                cellWidth,
                                                std::max(minimumLine, metrics.bottom - metrics.top),
                                                -metrics.top,
                                                link ? link->start : absent,
                                                at,
                                                next,
                                                {},
                                                false,
                                                nick,
                                                0});
                x += cellWidth;
                at = next;
            }
        }
        return x;
    };
    size_t begin = 0;
    float y = 0;
    do {
        const auto explicitEnd = document.text.find('\n', begin);
        const size_t paragraphEnd =
            explicitEnd == std::string::npos ? document.text.size() : explicitEnd;
        auto first = std::upper_bound(boundaries.begin(), boundaries.end(), begin);
        auto last = std::upper_bound(first, boundaries.end(), paragraphEnd);
        size_t count = last - first;
        if (columns > 0)
            count = std::min(count, size_t(columns));
        size_t low = 0, high = count;
        while (low < high) {
            const size_t middle = low + (high - low + 1) / 2;
            if (pieces(begin, first[middle - 1], 0, false) <= width)
                low = middle;
            else
                high = middle - 1;
        }
        size_t end = count ? first[low ? low - 1 : 0] : begin;
        if (end < paragraphEnd) {
            const auto word = std::upper_bound(legal.begin(), legal.end(), end);
            if (word != legal.begin() && *std::prev(word) > begin)
                end = *std::prev(word);
        }
        // Keep soft-wrap spaces in the logical line, not as painted or synthetic text.
        size_t paintedEnd = end;
        if (end < paragraphEnd) {
            auto spaceBoundary = std::lower_bound(boundaries.begin(), boundaries.end(), end);
            auto paintedBoundary = spaceBoundary;
            while (paintedEnd > begin && document.text[paintedEnd - 1] == ' ' &&
                   paintedBoundary != boundaries.begin() &&
                   *std::prev(paintedBoundary) == paintedEnd - 1) {
                --paintedEnd;
                --paintedBoundary;
            }
            while (end < paragraphEnd && document.text[end] == ' ' &&
                   std::next(spaceBoundary) != boundaries.end() &&
                   *std::next(spaceBoundary) == end + 1) {
                ++end;
                ++spaceBoundary;
            }
        }
        const size_t startFragment = result.fragments.size();
        const float lineWidth = pieces(begin, paintedEnd, y, true);
        float height = minimumLine;
        for (size_t i = startFragment; i < result.fragments.size(); ++i)
            height = std::max(height, result.fragments[i].height);
        for (size_t i = startFragment; i < result.fragments.size(); ++i)
            result.fragments[i].y += (height - result.fragments[i].height) * .5f;
        result.lines.push_back({y, height, startFragment, result.fragments.size(), begin, end});
        result.width = std::max(result.width, lineWidth);
        y += height;
        if (end == document.text.size())
            break;
        begin = end == paragraphEnd ? end + 1 : end;
    } while (begin <= document.text.size());
    result.height = y;
    return result;
}

// Heights remain double precision. Updating one visible wrap is O(log n), not a
// suffix walk through a 100,000-message history; no offscreen text is shaped.
struct HeightIndex {
    std::vector<double> tree{0};
    constexpr void clear() { tree.assign(1, 0); }
    constexpr double prefix(size_t count) const {
        double value = 0;
        for (; count; count -= count & -count)
            value += tree[count];
        return value;
    }
    constexpr void append(double height) {
        const size_t n = tree.size();
        const size_t first = n - (n & -n);
        tree.push_back(height + prefix(n - 1) - prefix(first));
    }
    constexpr void add(size_t index, double delta) {
        for (++index; index < tree.size(); index += index & -index)
            tree[index] += delta;
    }
    constexpr size_t at(double offset) const {
        size_t index = 0, step = 1;
        while (step < tree.size())
            step <<= 1;
        for (step >>= 1; step; step >>= 1) {
            const size_t next = index + step;
            if (next < tree.size() && tree[next] <= offset) {
                offset -= tree[next];
                index = next;
            }
        }
        return std::min(index, tree.size() - 1);
    }
};
static_assert([] {
    HeightIndex heights;
    heights.append(12.5);
    heights.append(40);
    heights.append(7.5);
    if (heights.prefix(3) != 60 || heights.at(12.5) != 1 || heights.at(59.9) != 2)
        return false;
    heights.add(0, 5);
    heights.append(10);
    return heights.prefix(4) == 75 && heights.at(17.4) == 0 && heights.at(65) == 3;
}());

bool nativeFocus(const Node &node, bool includeHidden = false) {
    if (!includeHidden && !node.visible())
        return false;
    if (const auto *area = dynamic_cast<const InputArea *>(&node); area && area->focused())
        return true;
    return std::any_of(
        node.children().begin(), node.children().end(),
        [includeHidden](const auto &child) { return nativeFocus(*child, includeHidden); });
}
bool contextKey(const InputArea::KeyData &key) {
    return key.pressed && !key.preedit &&
           ((key.sym == XKB_KEY_Menu && key.modifiers == 0) ||
            (key.sym == XKB_KEY_F10 && (key.modifiers == 0 || key.modifiers == KeyMod::Shift)));
}
void cancelMessagePresses(Node &node) {
    if (auto *area = dynamic_cast<InputArea *>(&node))
        area->dispatchCancel();
    for (const auto &child : node.children())
        cancelMessagePresses(*child);
}
std::optional<std::string> remappedMessageFocusKey(std::string_view key, std::string_view oldPrefix,
                                                   std::string_view newPrefix) {
    if (!key.starts_with(oldPrefix))
        return {};
    std::string result;
    result.reserve(newPrefix.size() + key.size() - oldPrefix.size());
    result.append(newPrefix).append(key.substr(oldPrefix.size()));
    return result;
}
void remapMessageFocusKeys(Node &node, std::string_view oldPrefix, std::string_view newPrefix) {
    if (auto *area = dynamic_cast<InputArea *>(&node))
        if (auto key = remappedMessageFocusKey(area->tabFocusKey(), oldPrefix, newPrefix))
            area->setTabFocusKey(std::move(*key));
    for (const auto &child : node.children())
        remapMessageFocusKeys(*child, oldPrefix, newPrefix);
}

struct Geometry {
    PresentationMetrics metrics{};
    float scale = 1, font = 16, uiFont = 14, metadataFont = 14, nickFont = 12, activityFont = 12,
          timeFont = 11, timeGutter = 0, nickGutter = 0, clockWidth = 0, chatWidth = 0;
    TextFont nickStyle, activityStyle;
    float header = 20, footer = 14, quote = 24, reaction = 29, day = 36, unread = 30;
    float bodyLine = 0, nickLine = 0, nickHeader = 0, activityLine = 0, metadataHeader = 0;
    float verticalPadding = 0;
    bool channel = false, inlineChannel = false;
    bool bubbles = false, avatars = false, timestamps = false, tight = false;
    std::string key;
    bool dense() const { return !bubbles; }
    float rowGap(bool presence, bool) const {
        return (presence ? 2.f : metrics.groupedGap) * scale;
    }
    float groupGap(bool presence, bool grouped) const {
        return !presence && !grouped ? (metrics.rowGap - metrics.groupedGap) * scale : 0;
    }
    float channelPrefix() const { return inlineChannel ? timeGutter + nickGutter + 8 * scale : 0; }
    float padding(bool presence) const { return presence ? 0 : metrics.padding * scale; }
    float paddingY(bool presence) const { return presence ? 0 : verticalPadding * scale; }
    float chipPadding() const { return (tight ? 4 : 6) * scale; }
    float chipPaddingY() const { return scale; }
    float activityPadding() const { return (tight ? 6 : 8) * scale; }
    float activityPaddingY() const { return 2 * scale; }
    float activityChevron() const { return 16 * scale; }
    float footerGap() const { return (tight ? 1 : 2) * scale; }
    float statusSize() const { return 12 * scale; }
    float statusGap() const { return 4 * scale; }
    bool footerVisible(const Message &entry) const {
        return bubbles && !entry.presence && (timestamps || sendIndicator(entry));
    }
    float footerWidth(const Message &entry) const {
        const float clock = timestamps ? clockWidth : 0;
        const float status = sendIndicator(entry) ? statusSize() : 0;
        return clock + status + (clock > 0 && status > 0 ? statusGap() : 0);
    }
    float footerExtent(const Message &entry, float width) const {
        if (!footerVisible(entry))
            return 0;
        const float clock =
            timestamps ? footer * std::max(1.f, std::ceil(clockWidth / std::max(1.f, width))) : 0;
        const float status = sendIndicator(entry) ? statusSize() : 0;
        return clock > 0 && status > 0 && footerWidth(entry) > width ? clock + statusGap() + status
                                                                     : std::max(clock, status);
    }
    float avatarLane(bool presence) const {
        // Compact avatars share the author header instead of narrowing the reading column.
        return bubbles && avatars && !presence && !tight ? (metrics.avatarSize + 10) * scale : 0;
    }
    float authorAvatarInset() const {
        return bubbles && tight && avatars ? (metrics.avatarSize + 6) * scale : 0;
    }
    float compactAvatarSize() const { return channel && avatars ? 20 * scale : 0; }
    float compactAvatarInset() const {
        return compactAvatarSize() > 0 ? compactAvatarSize() + 6 * scale : 0;
    }
    float prefixWidth(bool presence) const {
        return presence ? timeGutter : channel ? channelPrefix() : dense() ? timeGutter : 0;
    }
    float bodyWidth(float width, bool presence) const {
        const float minimum = channel && !presence ? 1.f : 32.f;
        const float available =
            std::max(minimum, width - 2 * metrics.inset * scale - avatarLane(presence));
        const float surface =
            presence
                ? available
                : std::min(available, std::max(240 * scale, available * metrics.widthFraction));
        return std::max(minimum, surface - 2 * padding(presence) - prefixWidth(presence));
    }
    bool stackedPrefix(bool, bool presence, bool) const {
        return !presence && channel && !inlineChannel;
    }
    bool nickVisible(bool grouped, bool presence) const {
        return !presence && (!grouped || (channel && !inlineChannel));
    }
    bool authorHeader(bool grouped, bool presence, bool clockVisible) const {
        return !presence &&
               ((!grouped && bubbles) || stackedPrefix(grouped, presence, clockVisible));
    }
};
Geometry geometry(Renderer &renderer, const Buffer &buffer, const Model &model, float width,
                  float height, float chatWidth) {
    Geometry result;
    result.chatWidth = chatWidth;
    result.metrics = presentationMetrics();
    result.scale = themeScale();
    result.bubbles = messageBubbles(model.settings, buffer);
    result.channel = !result.bubbles && (buffer.kind == "channel" || buffer.kind == "query");
    result.tight = result.bubbles && (width < 400 * result.scale || height < 180 * result.scale);
    if (result.bubbles) {
        result.metrics.padding = 12;
        result.metrics.rowGap = 14;
        result.metrics.groupedGap = 6;
        result.metrics.widthFraction = .90f;
    }
    if (result.tight) {
        result.metrics.padding = 4;
        result.metrics.inset = 4;
        result.metrics.headerHeight = 18;
        result.metrics.rowGap = 10;
        result.metrics.groupedGap = 4;
        result.metrics.avatarSize = std::min(result.metrics.avatarSize, 24.f);
    }
    if (result.dense()) {
        result.metrics.padding = 0;
        result.metrics.rowGap = result.channel ? 10 : 12;
        result.metrics.groupedGap = result.channel ? 4 : 5;
        result.metrics.widthFraction = 1;
    }
    result.verticalPadding = result.tight ? 4 : result.bubbles ? 6 : result.metrics.padding;
    result.font = messageTextSize(model.settings, buffer);
    // Native layout leading can exceed the font's clipping-only extent.
    const float messageScale = result.font / (result.bubbles ? 14.f : 15.f);
    result.activityFont = 12 * messageScale;
    result.activityStyle = themeMessageFont();
    result.activityStyle.family = "monospace";
    const auto activityStrut =
        renderer.measureText("Mg", result.activityFont, result.activityStyle, 0, 1);
    result.activityLine = std::max(1.f, activityStrut.bottom - activityStrut.top);
    const auto bodyStrut = renderer.measureText("Mg", result.font, themeMessageFont(), 0, 1);
    result.bodyLine = std::max(1.f, bodyStrut.bottom - bodyStrut.top);
    result.uiFont = 14 * result.scale;
    result.metadataFont = 14 * messageScale;
    result.nickFont = result.font * .82f;
    if (result.bubbles) {
        result.nickStyle = themeUiFont();
        result.nickStyle.weight = FontWeight::Bold;
        const auto nickStrut = renderer.measureText("Mg", result.nickFont, result.nickStyle, 0, 1);
        result.nickLine = std::max(1.f, nickStrut.bottom - nickStrut.top);
        result.nickHeader = result.nickLine + 2 * result.chipPaddingY() + 3 * result.scale;
    }
    result.timeFont = result.font * .72f;
    if (result.bubbles) {
        const auto footerStrut = renderer.measureText("Mg", result.timeFont, themeUiFont(), 0, 1);
        result.footer = std::max(1.f, footerStrut.bottom - footerStrut.top);
    } else
        result.footer = 0;
    result.timestamps = model.settings.timestamps;
    result.avatars = model.settings.avatars && model.settings.avatarStyle != "none";
    if (result.tight && result.avatars)
        result.nickHeader = std::max(result.nickHeader, result.metrics.avatarSize * result.scale);
    if (result.timestamps)
        for (const auto digit : std::string_view("0123456789-")) {
            const std::string text = std::string(2, digit) + ":" + std::string(2, digit);
            if (result.bubbles)
                result.clockWidth =
                    std::max(result.clockWidth,
                             renderer.measureText(text, result.timeFont, themeUiFont()).width);
            result.timeGutter =
                std::max({result.timeGutter, 44 * result.scale,
                          renderer.measureText(text, result.metadataFont, themeUiFont()).width +
                              8 * result.scale});
        }
    result.header = std::max((result.dense() ? 18 : result.metrics.headerHeight) * result.scale,
                             renderer.fontRowExtent(result.uiFont, themeUiFont()));
    result.metadataHeader =
        std::max((result.dense() ? 18 : result.metrics.headerHeight) * result.scale,
                 renderer.fontRowExtent(result.metadataFont, themeUiFont()));
    if (result.tight) {
        // Keep body and footer fonts intact; decorative padding yields on short viewports.
        const float paddingBudget = height - 2 * result.bodyLine - result.nickHeader -
                                    2 * result.metrics.rowGap * result.scale -
                                    2 * (result.footer + result.footerGap());
        result.verticalPadding = std::clamp(paddingBudget / (3 * result.scale), 0.f, 4.f);
    }
    result.quote = std::max(24 * result.scale, result.metadataHeader + 4 * result.scale);
    result.reaction = std::max(37 * result.scale, result.metadataHeader + 10 * result.scale);
    result.day = std::max(36 * result.scale, result.header + 16 * result.scale);
    result.unread = std::max(30 * result.scale, result.header + 10 * result.scale);
    if (result.channel) {
        const float available = std::max(1.f, width - 2 * result.metrics.inset * result.scale);
        const float nickAdornment = result.compactAvatarInset() + 2 * result.chipPadding();
        const float preferredNick =
            renderer.measureText("MMMMMMMMM", result.metadataFont, themeUiFont()).width +
            nickAdornment;
        const float minimumNick =
            renderer.measureText("MMMMM", result.metadataFont, themeUiFont()).width + nickAdornment;
        const float minimumBody =
            std::max(144 * result.scale,
                     renderer.measureText("MMMMMMMMMMMM", result.font, themeMessageFont()).width);
        const float remaining = available - result.timeGutter - 8 * result.scale - minimumBody;
        const float inlineNick = std::min(preferredNick, remaining);
        result.inlineChannel = messageLayout(model.settings, buffer) != MessageLayout::Stacked &&
                               inlineNick >= minimumNick &&
                               result.timeGutter + inlineNick + 8 * result.scale <= available / 3;
        if (result.inlineChannel)
            result.nickGutter = inlineNick;
        else {
            result.timeGutter = std::min(result.timeGutter, available * .4f);
            result.nickGutter = std::max(1.f, available - result.timeGutter);
        }
    }
    result.key = buffer.kind + "|" + themeMessageFont().family + "|" + themeMessageFont().features +
                 "|" + std::to_string(int(themeMessageFont().weight)) + "|" + themeUiFont().family +
                 "|" + themeUiFont().features + "|" + std::to_string(int(themeUiFont().weight)) +
                 "|" + std::to_string(result.scale) + "|" + std::to_string(result.bubbles) + "|" +
                 std::to_string(result.font) + "|" + std::to_string(result.avatars) +
                 std::to_string(result.timestamps) + "|" + std::to_string(result.tight) + "|" +
                 std::to_string(result.verticalPadding) + "|" +
                 std::to_string(result.inlineChannel) + "|" + std::to_string(result.timeGutter) +
                 "|" + std::to_string(result.nickGutter) + "|" +
                 std::to_string(renderer.textMetricsGeneration()) + "|" +
                 std::to_string(renderer.renderScale());
    result.key += "|" + model.settings.nickPalette + "|" + std::to_string(themeRevision()) + "|" +
                  std::to_string(model.settings.messageWrapColumns);
    for (const auto &[nick, color] : model.settings.nickColors)
        result.key += "|" + std::to_string(nick.size()) + ":" + nick + color;
    for (const auto &nick : buffer.nicks)
        result.key += "|" + std::to_string(nick.name.size()) + ":" + nick.name;
    return result;
}

} // namespace

std::vector<NickAnnotation> annotateNicks(const Model &model, const Buffer &buffer,
                                          const IrcDocument &document, std::string_view author) {
    return annotateNicksWithLinks(model, buffer, document, author, detectLinks(document));
}

struct TranscriptView::Impl {
    struct Row {
        std::string key, id, footerText;
        double height = 32;
        size_t signature = 0;
        uint64_t revision = 0, focusIdentity = 0;
        double entranceStarted = -1;
        bool measured = false, fragmentsReady = false, day = false, grouped = false, unread = false;
        bool unreadEligible = false;
        bool friendAuthor = false;
        bool footerStacked = false;
        bool showClock = true;
        bool foolOpen = false;
        bool projected = false, hiddenActivity = false, activitySummary = false,
             activityExpanded = false, activityOpen = false;
        size_t previousVisible = absent, activityBegin = absent, activityEnd = absent;
        std::string activityLabel;
        float contentWidth = 0, surfaceWidth = 0, prefixWidth = 0, bodyOffset = 0, bodyHeight = 0;
        float emotePrefix = 0;
        std::string quoteAuthor, quoteMarkup;
        Color quoteColor{};
        float quoteHeight = 0, quoteAuthorHeight = 0, quoteExcerptHeight = 0;
        float selectionWidth = 0;
        std::string previewUrl;
        float previewOffset = 0, previewHeight = 0;
        bool urlReplaced = false;
        float authorWidth = 0, authorHeight = 0, authorHeader = 0, footerWidth = 0,
              footerOffset = 0, footerAscent = 0, footerHeight = 0, clockWidth = 0, clockHeight = 0,
              contentEnd = 0;
        float prefixBaseline = 0, prefixAscent = 0, prefixHeight = 0;
        std::vector<Fragment> fragments;
        std::vector<VisualLine> lines;
        std::optional<MessageSelectionText> selectionText;
        bool selectionGeometryReady = false;
        struct Reaction {
            std::string emoji, label, users;
            float x, y, width;
        };
        std::vector<Reaction> reactions;
    };
    struct Cache {
        std::vector<Row> rows;
        std::unordered_map<std::string, size_t> keys, ids;
        HeightIndex heights;
        Geometry geometry;
        float width = 0, header = 0, scroll = 0;
        size_t divider = absent;
        int unreadCount = 0;
        std::string marker, newest, readKey, readTime;
        std::string policy;
        std::string foolPolicy;
        std::set<std::string> revealedFoolList;
        decltype(Settings::bufferFools) revealedBufferFools;
        FoolDisplay revealedFoolDisplay = FoolDisplay::Minimized;
        std::string revealedFoolMapping;
        bool following = true, windowActive = false, initialized = false;
        PresenceMode projectedPresence = PresenceMode::Smart;
        uint64_t participationRevision = 0;
        uint64_t mediaParticipationRevision = 0;
        size_t projectionFrom = 0;
        // Arrivals observed before a focus reveal settles are resolved against its final offset.
        int unseen = 0, pendingArrivals = 0;
        double lastRead = 0;
        uint64_t revision = 1;
        std::optional<uint64_t> messagesRevision;
        std::vector<size_t> materialized;
        std::vector<size_t> prepared;
        std::vector<size_t> arrivals;
        std::unordered_map<std::string, double> reactionTimes;
        std::vector<bool> smartEligible;
        std::unordered_map<std::string, std::deque<int64_t>> smartActivity;
    };
    struct Slot {
        InputArea *area = nullptr;
        MaterialNode *material = nullptr;
        RectNode *dense = nullptr, *focusRing = nullptr;
        Node *content = nullptr, *decorations = nullptr, *body = nullptr;
        MediaPreviewCard *preview = nullptr;
        PixelRevealNode *reveal = nullptr;
        RenderProxyNode *flight = nullptr;
        PixelRevealNode *flightReveal = nullptr;
        Node *flightGroup = nullptr;
        struct BodyText {
            TextNode *node;
            std::optional<Color> color;
        };
        std::vector<BodyText> text;
        std::vector<RectNode *> selectionRects;
        float bodyX = 0;
        std::vector<std::pair<Label *, std::string>> nicks;
        TextNode *channelTime = nullptr;
        TextNode *bubbleTime = nullptr;
        RectNode *nickChip = nullptr;
        Label *nickChipText = nullptr;
        Glyph *friendBadge = nullptr;
        bool ownNickChip = false;
        TextNode *avatarInitial = nullptr;
        Spinner *statusSpinner = nullptr;
        Glyph *statusCheck = nullptr;
        RectNode *avatarBackground = nullptr;
        std::vector<std::pair<RectNode *, std::string_view>> roleRects;
        struct ReactionControl {
            Button *button;
            float x, y;
        };
        std::vector<ReactionControl> reactions;
        float pressX = 0, pressY = 0;
        bool clickArmed = false;
        std::string key, buffer;
        size_t index = 0;
        uint64_t revision = 0, focusIdentity = 0;
        std::optional<uint64_t> messagesRevision;
        std::string avatarStyle, avatarMapping;
        std::string policy, sourceCaption;
        void updateFocusKey() {
            const std::string entryKey = "entry:" + std::to_string(focusIdentity);
            std::string identity = std::to_string(buffer.size());
            identity.reserve(identity.size() + 1 + buffer.size() + entryKey.size());
            identity += ':';
            identity += buffer;
            identity += entryKey;
            area->setTabFocusKey(std::move(identity));
        }
        std::string actionFocusKey(std::string_view role, std::string_view payload,
                                   size_t occurrence = absent) const {
            constexpr std::string_view marker = "action:";
            const std::string position = occurrence == absent ? "" : std::to_string(occurrence);
            const std::string entryKey = "entry:" + std::to_string(focusIdentity);
            const std::array<std::string_view, 5> parts{buffer, entryKey, role, payload, position};
            std::array<std::string, 5> lengths;
            size_t capacity = marker.size();
            for (size_t i = 0; i < parts.size(); ++i) {
                lengths[i] = std::to_string(parts[i].size());
                capacity += lengths[i].size() + 1 + parts[i].size();
            }
            std::string identity;
            identity.reserve(capacity);
            for (size_t i = 0; i < parts.size(); ++i) {
                identity += lengths[i];
                identity += ':';
                identity += parts[i];
                if (i == 0)
                    identity += marker;
            }
            return identity;
        }
    };
    struct TextEndpoint {
        uint64_t identity = 0;
        size_t byte = 0, hint = absent;
    };
    struct TextRange {
        std::string buffer;
        TextEndpoint anchor, focus;
    };
    struct OrderedTextRange {
        size_t first, last;
        TextEndpoint begin, end;
    };
    struct TextDrag {
        bool armed = false, active = false;
        uint64_t owner = 0, suppressedClick = 0;
        float pressX = 0, pressY = 0, x = 0, y = 0;
        double lastScroll = 0;
    };
    TranscriptView &view;
    Model &model;
    PaneState &pane;
    TextClipboard &clipboard;
    UiCommand command;
    FocusPane focus;
    ScrollView *scroll = nullptr;
    Node *canvas = nullptr;
    Button *history = nullptr, *unseen = nullptr;
    Label *empty = nullptr;
    Node *typing = nullptr;
    InputArea *keys = nullptr;
    InputDispatcher *dispatcher = nullptr;
    std::optional<InputDispatcher::TabFocusSnapshot> actionFocus;
    std::vector<Slot> slots;
    std::unordered_map<std::string, Cache> caches;
    std::string active, composerFocusKey;
    Cache *cache = nullptr;
    bool arranging = false, readEligible = false, windowActive = false, switched = false;
    std::optional<std::pair<float, float>> composerOrigin;
    Node *motionHost = nullptr;
    float typingInset = 0, typingRowHeight = 0, replyOverlayHeight = 0;
    std::optional<std::string> focusRevealTarget;
    bool popupOwned = false;
    uint64_t popupGeneration = 0;
    std::string popupPolicy;
    uint64_t selectedIdentity = 0, navigationRevealIdentity = 0;
    uint64_t sourceJumpIdentity = 0;
    size_t selectedIndexHint = absent;
    std::string jumpTarget;
    std::shared_ptr<int> alive = std::make_shared<int>(0);
    std::function<void(std::string, std::optional<InputArea::PointerData>)> onNickMenu;
    void nickMenu(const std::string &buffer, const std::string &nick,
                  std::optional<InputArea::PointerData> pointer = {}, uint64_t identity = 0) {
        if (buffer != active || !readableIdentity(identity))
            return;
        const auto found = model.buffers.find(buffer);
        const auto index = identityIndex(identity);
        if (found == model.buffers.end() || index >= found->second.messages.size())
            return;
        if (found->second.kind == "mentions") {
            openOriginal(buffer, cache->rows[index].key);
            return;
        }
        const auto value = "nick:" + buffer + "\n" + nick;
        if (onNickMenu)
            onNickMenu(value, pointer);
        else
            command(value);
    }
    struct TypingAvatar {
        RectNode *background = nullptr;
        Label *initial = nullptr;
        Image *image = nullptr;
        NativeAvatarLabelPlacement placement;
        std::string nick, mapping, style;
    };
    std::vector<std::string> typingParticipants;
    std::array<Label *, 2> typingNicks{};
    std::array<TypingAvatar, 3> typingAvatars{};
    RectNode *typingSurface = nullptr;
    Node *typingPaint = nullptr;
    Button *typingOverflow = nullptr;
    std::array<RectNode *, 3> typingDots{};
    Node *typingList = nullptr;
    InputArea *typingDismiss = nullptr;
    RectNode *typingListSurface = nullptr;
    Label *typingListTitle = nullptr;
    ScrollView *typingListScroll = nullptr;
    bool typingListOpen = false;
    size_t typingShown = absent, typingRemaining = absent;
    float typingOpacity = 0;
    double lastTyping = 0;
    Node *mentionFooter = nullptr;
    RectNode *mentionSurface = nullptr;
    Button *olderMention = nullptr, *newerMention = nullptr, *mentionCount = nullptr;
    std::string mentionCursorBuffer, mentionCursor;
    uint64_t mentionSelectionIdentity = 0;
    double effectsTick = -1;
    MediaPreviewController *mediaController = nullptr;
    Renderer *selectionRenderer = nullptr;
    std::optional<TextRange> selectedText;
    bool keyboardSelection = false;
    TextDrag textDrag;
    std::vector<MessageSelectionRect> selectionScratch;

    bool effectivelyVisible() const {
        for (const Node *node = &view; node; node = node->parent())
            if (!node->visible())
                return false;
        return true;
    }
    void rememberActionFocus(Slot &slot, Node &content) {
        if (!dispatcher || actionFocus || pane.focusComposer || slot.buffer != active ||
            !effectivelyVisible())
            return;
        for (Node *node = dispatcher->focusedArea(); node; node = node->parent())
            if (node == &content) {
                actionFocus = dispatcher->captureTabFocus();
                dispatcher->setFocus(nullptr);
                return;
            }
    }
    bool restoreActionFocus() {
        if (!actionFocus)
            return false;
        auto saved = std::move(*actionFocus);
        actionFocus.reset();
        if (!dispatcher || dispatcher->focusedArea() || pane.focusComposer ||
            !effectivelyVisible() || view.width() <= 0 || view.height() <= 0)
            return false;
        dispatcher->restoreTabFocus(std::move(saved));
        if (!dispatcher->focusedArea())
            dispatcher->setFocus(keys);
        return dispatcher->focusedArea() != nullptr;
    }
    void cancelFocusReveal() {
        sourceJumpIdentity = 0;
        if (focusRevealTarget) {
            focusRevealTarget.reset();
            scroll->cancelPendingScrollRequests();
        }
    }
    bool validFocusReveal() {
        if (!focusRevealTarget)
            return false;
        const auto *focused = dispatcher ? dispatcher->focusedArea() : nullptr;
        if (!focused || focused->tabFocusKey() != *focusRevealTarget) {
            cancelFocusReveal();
            return false;
        }
        // A removed/recycled row can retain native focus until the next dispatch.
        // Its old native IntoView request must not replace the surviving body anchor.
        for (const Node *node = focused; node; node = node->parent())
            if (!node->visible()) {
                cancelFocusReveal();
                return false;
            }
        return true;
    }
    void settleFocusReveal(bool revealed) {
        focusRevealTarget.reset();
        if (!cache)
            return;
        cache->scroll = scroll->scrollOffset();
        if (revealed)
            pane.following = scroll->maxScrollOffset() - cache->scroll <= 1;
        if (pane.following)
            pane.unseen = 0;
        else
            pane.unseen += cache->pendingArrivals;
        cache->pendingArrivals = 0;
    }
    void markRead(const Buffer &buffer) {
        // Read eligibility is host-owned; tail eligibility uses the position actually laid out.
        if (!readEligible || !pane.following || focusRevealTarget || !effectivelyVisible() ||
            view.height() <= 0 || scroll->maxScrollOffset() - scroll->scrollOffset() > 1 ||
            model.now - cache->lastRead <= .5)
            return;
        if (const auto *last = ui_logic::readTarget(buffer))
            if (last->id != cache->readKey || last->time != cache->readTime || buffer.unread > 0) {
                model.actions.push_back({"read", buffer.id, {}, {}});
                cache->readKey = last->id;
                cache->readTime = last->time;
                cache->lastRead = model.now;
            }
    }

    Impl(TranscriptView &owner, Model &state, PaneState &paneState, TextClipboard &clip,
         UiCommand execute, FocusPane focusPane)
        : view(owner), model(state), pane(paneState), clipboard(clip), command(std::move(execute)),
          focus(std::move(focusPane)) {
        composerFocusKey = "composer:" + std::to_string(pane.id) + ":input";
        view.setClipChildren(true);
        view.setFlexGrow(1);
        auto scroller = std::make_unique<ScrollView>();
        scroll = scroller.get();
        scroll->setViewportPaddingH(0);
        scroll->setViewportPaddingV(0);
        scroll->setReserveScrollbarGutter(true);
        scroll->setRadius(0);
        scroll->clearFill();
        scroll->setOnScrollChanged([this](float value) {
            if (cache)
                cache->scroll = value;
            if (!arranging) {
                cancelFocusReveal();
                pane.following = scroll->maxScrollOffset() - value <= 1;
                if (pane.following)
                    pane.unseen = 0;
                focus(pane.id);
                view.markLayoutDirty();
            }
        });
        auto body = std::make_unique<Node>();
        canvas = body.get();
        scroll->content()->setGap(0);
        scroll->content()->setPadding(0);
        scroll->content()->addChild(std::move(body));
        view.addChild(std::move(scroller));
        auto keyArea = std::make_unique<InputArea>();
        keys = keyArea.get();
        keys->setFocusable(true);
        keys->setTabStop(true);
        keys->setTabFocusKey("transcript:" + std::to_string(pane.id) + ":keyboard");
        keys->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}));
        keys->setOnPress([this](const auto &pointer) {
            if (pointer.pressed && pointer.button == BTN_LEFT)
                clearTextSelection();
            focus(pane.id);
        });
        keys->setOnKeyDown([this](const InputArea::KeyData &key) { handleKey(key); });
        canvas->addChild(std::move(keyArea));
        auto load = std::make_unique<Button>();
        history = load.get();
        history->inputArea()->setTabFocusKey("transcript:" + std::to_string(pane.id) +
                                             ":load-older");
        history->setVariant(ButtonVariant::Ghost);
        history->setTooltip("Load an older local page, then request server history when supported");
        history->setOnClick([this] {
            if (auto *buffer = current(); buffer && !buffer->loading) {
                focus(pane.id);
                model.actions.push_back(
                    {"history",
                     buffer->id,
                     {},
                     buffer->messages.empty() ? std::string{} : buffer->messages.front().id});
            }
        });
        canvas->addChild(std::move(load));
        auto noMessages = std::make_unique<Label>();
        empty = noMessages.get();
        empty->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
        canvas->addChild(std::move(noMessages));
        auto typingFooter = std::make_unique<Node>();
        typing = typingFooter.get();
        auto paint = std::make_unique<Node>();
        typingPaint = paint.get();
        typingPaint->setHitTestVisible(false);
        typing->addChild(std::move(paint));
        typingSurface = rectangle(*typingPaint, 0, 0, 1, 1, themeSurfaceColor("surface_container"),
                                  12 * themeScale());
        for (auto &avatar : typingAvatars) {
            avatar.background = rectangle(*typingPaint, 0, 0, 1, 1, {}, 0);
            avatar.initial = label(*typingPaint, "", 0, 0, 1, 12 * themeScale(), {}, themeUiFont());
            avatar.initial->setHitTestVisible(false);
            auto image = std::make_unique<Image>();
            avatar.image = image.get();
            image->setFit(ImageFit::Contain);
            image->setHitTestVisible(false);
            typingPaint->addChild(std::move(image));
        }
        for (auto &name : typingNicks) {
            name = label(*typingPaint, "", 0, 0, 1, 12 * themeScale(), {}, themeMessageFont());
            name->setHitTestVisible(false);
        }
        auto overflow = std::make_unique<Button>();
        typingOverflow = overflow.get();
        overflow->setVariant(ButtonVariant::Ghost);
        overflow->inputArea()->setFocusOnPointerPress(false);
        overflow->inputArea()->setTabFocusKey("transcript:" + std::to_string(pane.id) +
                                              ":typing-overflow");
        overflow->setTooltip("Show everyone typing · Arrow keys scroll · Escape close");
        overflow->setOnClick([this] { toggleTypingList(); });
        typing->addChild(std::move(overflow));
        for (auto &dot : typingDots)
            dot = rectangle(*typingPaint, 0, 0, 3 * themeScale(), 3 * themeScale(),
                            themeForegroundColor("on_surface_variant"), 2 * themeScale());
        view.addChild(std::move(typingFooter));
        auto list = std::make_unique<Node>();
        typingList = list.get();
        typingList->setZIndex(30);
        typingList->setVisible(false);
        auto dismiss = std::make_unique<InputArea>();
        typingDismiss = dismiss.get();
        dismiss->setFocusable(false);
        dismiss->setFocusOnPointerPress(false);
        dismiss->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}));
        dismiss->setOnClick([this](const auto &) { closeTypingList(); });
        typingList->addChild(std::move(dismiss));
        typingListSurface =
            rectangle(*typingList, 0, 0, 1, 1, themeSurfaceColor("surface_container_high"),
                      10 * themeScale());
        typingListTitle = label(*typingList, "Typing now", 0, 0, 1, 14 * themeScale(),
                                themeForegroundColor("on_surface"), themeUiFont());
        typingListTitle->setHitTestVisible(false);
        auto listScroll = std::make_unique<ScrollView>();
        typingListScroll = listScroll.get();
        listScroll->setReserveScrollbarGutter(true);
        typingList->addChild(std::move(listScroll));
        preserveTypingFocus(*typingListScroll);
        view.addChild(std::move(list));
        auto footer = std::make_unique<InputArea>();
        footer->setFocusable(false);
        footer->setFocusOnPointerPress(false);
        footer->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT}));
        mentionFooter = footer.get();
        mentionFooter->setZIndex(11);
        mentionFooter->setVisible(false);
        mentionSurface = rectangle(*mentionFooter, 0, 0, 1, 1,
                                   themeSurfaceColor("surface_container_high"), 14 * themeScale());
        auto arrow = [&](const char *text, const char *key, int direction) {
            auto button = std::make_unique<Button>();
            auto *result = button.get();
            result->setText(text);
            result->setVariant(ButtonVariant::Secondary);
            result->inputArea()->setFocusOnPointerPress(false);
            result->inputArea()->setTabFocusKey("transcript:" + std::to_string(pane.id) + key);
            result->setTooltip(direction < 0 ? "Older mention" : "Newer mention");
            result->setOnClick([this, direction] { navigateMention(direction); });
            mentionFooter->addChild(std::move(button));
            return result;
        };
        olderMention = arrow("↑", ":mention-older", -1);
        auto count = std::make_unique<Button>();
        mentionCount = count.get();
        mentionCount->setVariant(ButtonVariant::Ghost);
        mentionCount->inputArea()->setFocusOnPointerPress(false);
        mentionCount->inputArea()->setTabFocusKey("transcript:" + std::to_string(pane.id) +
                                                  ":mention-read");
        mentionCount->setTooltip("Mark this conversation read (including all mentions)");
        mentionCount->setOnClick([this] {
            const auto *buffer = current();
            if (buffer && pane.buffer == active && !retainedMentions(*buffer).empty())
                model.actions.push_back({"read", buffer->id, {}, {}});
        });
        mentionFooter->addChild(std::move(count));
        newerMention = arrow("↓", ":mention-newer", 1);
        view.addChild(std::move(footer));
        auto jump = std::make_unique<Button>();
        unseen = jump.get();
        unseen->inputArea()->setFocusOnPointerPress(false);
        unseen->inputArea()->setTabFocusKey("transcript:" + std::to_string(pane.id) +
                                            ":follow-newest");
        unseen->setVariant(ButtonVariant::Primary);
        unseen->setGlyph("chevron-down");
        unseen->setTooltip("Jump to latest");
        unseen->setOnClick([this] {
            scrollToBoundary(true);
            focus(pane.id);
        });
        unseen->setZIndex(10);
        unseen->setVisible(false);
        view.addChild(std::move(jump));
    }
    ~Impl() {
        for (auto &[id, retained] : caches)
            retireMedia(retained);
        for (auto &slot : slots)
            cancelEntrance(slot);
        closePopup();
        closeTypingList();
        alive.reset();
    }
    void retireMedia(const Cache &retained) {
        if (mediaController)
            for (const auto &row : retained.rows)
                mediaController->bind(row.focusIdentity, "", false);
    }
    void retireDeletedMedia() {
        if (!mediaController)
            return;
        for (auto &[id, retained] : caches) {
            const auto found = model.buffers.find(id);
            if (found == model.buffers.end() ||
                retained.mediaParticipationRevision == found->second.participationRevision)
                continue;
            retained.mediaParticipationRevision = found->second.participationRevision;
            for (const auto &message : found->second.messages) {
                if (!message.deleted)
                    continue;
                size_t index = absent;
                if (const auto row = retained.ids.find(message.id);
                    row != retained.ids.end() && row->second < retained.rows.size() &&
                    retained.rows[row->second].id == message.id)
                    index = row->second;
                if (index == absent) {
                    const auto row = retained.keys.find(ui_logic::messageKey(message));
                    if (row != retained.keys.end())
                        index = row->second;
                }
                if (index < retained.rows.size())
                    mediaController->bind(retained.rows[index].focusIdentity, "", false);
            }
        }
    }
    Buffer *current() {
        const auto found = model.buffers.find(pane.buffer);
        return found == model.buffers.end() ? nullptr : &found->second;
    }
    const Buffer *sourceContext(const Buffer &buffer, const Message &entry) const {
        const bool metadata = !entry.sourceBuffer.empty() || !entry.sourceId.empty() ||
                              !entry.sourceServer.empty() || !entry.sourceTarget.empty();
        if (buffer.kind == "search" && metadata) {
            const Buffer *source = nullptr;
            if (const auto live = model.buffers.find(entry.sourceBuffer);
                live != model.buffers.end())
                source = &live->second;
            else if (const auto retained = model.mentionSources.find(entry.sourceBuffer);
                     retained != model.mentionSources.end())
                source = &retained->second;
            if (!entry.sourceId.empty()) {
                const auto findOriginal = [&](const Buffer &candidate) -> const Message * {
                    const auto indexed = candidate.sourceMessageIndex.find(entry.sourceId);
                    if (indexed != candidate.sourceMessageIndex.end() &&
                        indexed->second < candidate.messages.size() &&
                        candidate.messages[indexed->second].id == entry.sourceId)
                        return &candidate.messages[indexed->second];
                    const auto found = std::find_if(
                        candidate.messages.begin(), candidate.messages.end(),
                        [&](const Message &message) { return message.id == entry.sourceId; });
                    return found == candidate.messages.end() ? nullptr : &*found;
                };
                const Message *original = nullptr;
                if (const auto live = model.buffers.find(entry.sourceBuffer);
                    live != model.buffers.end())
                    original = findOriginal(live->second);
                if (!original)
                    if (const auto retained = model.mentionSources.find(entry.sourceBuffer);
                        retained != model.mentionSources.end())
                        original = findOriginal(retained->second);
                if (original && original->deleted)
                    return nullptr;
            }
            return source && (source->kind == "channel" || source->kind == "query") &&
                           source->server == entry.sourceServer &&
                           source->target == entry.sourceTarget
                       ? source
                       : nullptr;
        }
        if (buffer.kind == "mentions") {
            if (entry.own || entry.presence || entry.deleted || !entry.mention)
                return nullptr;
        } else if (metadata)
            return nullptr;
        // Original tombstones retain their non-readable deletion placeholder.
        if (entry.deleted && buffer.kind != "mentions" && entry.sourceBuffer.empty() &&
            entry.sourceId.empty() && entry.sourceServer.empty() && entry.sourceTarget.empty())
            return &buffer;
        return resolveMessageSource(model, buffer, entry);
    }
    FoolDisplay foolPolicy(const Buffer &buffer, const Message &entry) const {
        const auto *source = sourceContext(buffer, entry);
        return source ? effectiveFoolDisplay(model.settings, source->id, source->target)
                      : FoolDisplay::Hidden;
    }
    bool foolish(const Buffer &buffer, const Message &entry) const {
        const auto *source = sourceContext(buffer, entry);
        return !source || (!entry.own && isFool(model, source->server, entry.nick));
    }
    bool friendAuthor(const Buffer &buffer, const Message &entry) const {
        const auto *source = sourceContext(buffer, entry);
        return source && !entry.presence && !entry.deleted &&
               isFriend(model, source->server, entry.nick);
    }
    const std::string &sourceServer(const Buffer &buffer, const Message &entry) const {
        const auto *source = sourceContext(buffer, entry);
        return source ? source->server : buffer.server;
    }
    const Message *quotedMessage(const Buffer &buffer, const Message &entry) const {
        const auto *source = sourceContext(buffer, entry);
        if (source && source->id == buffer.id && !entry.reply.empty()) {
            const auto index = resolve(buffer, entry.reply);
            if (index < buffer.messages.size())
                return &buffer.messages[index];
        }
        return source && !entry.reply.empty() ? sourceMessage(model, source->id, entry.reply)
                                              : nullptr;
    }
    bool quoteReadable(const Buffer &buffer, const Message &entry, const Message &quote) const {
        const auto *source = sourceContext(buffer, entry);
        if (!source || quote.deleted)
            return false;
        if (source->id == buffer.id) {
            const auto index = resolve(buffer, quote.id);
            if (index < buffer.messages.size())
                return readable(buffer, index);
        }
        const auto policy = effectiveFoolDisplay(model.settings, source->id, source->target);
        return quote.own || !isFool(model, source->server, quote.nick) ||
               policy == FoolDisplay::Shown || policy == FoolDisplay::Dimmed;
    }
    std::string privacyPolicy(const Buffer &buffer) const {
        std::string result = std::to_string(int(model.settings.foolDisplay));
        for (const auto &[id, shown] : model.settings.bufferFools)
            result += "|" + std::to_string(id.size()) + ":" + id + (shown ? "+" : "-");
        for (const auto &nick : model.settings.fools)
            result += "|" + std::to_string(nick.size()) + ":" + nick;
        for (const auto &[id, network] : model.networks) {
            result += "|" + id + ":" +
                      (model.settings.fools.empty() ? std::string{} : network.caseMapping) + ":" +
                      network.nick + ":" + network.state + ":" + network.clientTagDeny;
            for (const auto &cap : network.caps)
                result += "|" + cap;
        }
        if (buffer.kind == "mentions")
            for (const auto &[id, source] : model.mentionSources)
                result += "|" + id + ":" + std::to_string(source.messagesRevision);
        if (buffer.kind == "search") {
            for (const auto &[id, source] : model.buffers)
                result += "|" + id + ":" + source.server + ":" + source.target + ":" + source.kind +
                          ":" + std::to_string(source.messagesRevision) + ":" +
                          std::to_string(source.participationRevision);
            for (const auto &[id, source] : model.mentionSources)
                result += "|" + id + ":" + source.server + ":" + source.target + ":" + source.kind +
                          ":" + std::to_string(source.messagesRevision) + ":" +
                          std::to_string(source.participationRevision);
        }
        return result;
    }
    void openOriginal(const std::string &bufferId, const std::string &key) {
        const auto found = model.buffers.find(bufferId);
        const auto *entry = message(bufferId, key);
        if (bufferId != active || pane.buffer != active || found == model.buffers.end() || !entry ||
            !cache || !cache->keys.contains(key) || !readable(found->second, cache->keys.at(key)))
            return;
        if (found->second.kind == "search") {
            model.actions.push_back({"search-jump", bufferId, entry->id, {}});
            return;
        }
        const auto *source = sourceContext(found->second, *entry);
        if (source && found->second.kind == "mentions")
            model.actions.push_back({"source-jump", source->id, entry->sourceId, {}});
    }
    std::vector<const Message *> retainedMentions(const Buffer &buffer) const {
        std::vector<const Message *> result;
        const auto retained = model.mentionSources.find(buffer.id);
        if (buffer.kind == "mentions" || retained == model.mentionSources.end())
            return result;
        ui_logic::forEachUnreadMention(model, buffer, retained->second, [&](const Message &entry) {
            if (entry.deleted || entry.nick.empty() || !sourceContext(buffer, entry))
                return true;
            const auto policy = foolPolicy(buffer, entry);
            if (foolish(buffer, entry) && policy != FoolDisplay::Shown &&
                policy != FoolDisplay::Dimmed)
                return true;
            result.push_back(&entry);
            return true;
        });
        return result;
    }
    size_t mentionPosition(const Buffer &buffer, const std::vector<const Message *> &mentions) {
        if (mentionCursorBuffer != buffer.id) {
            mentionCursorBuffer = buffer.id;
            mentionCursor.clear();
        }
        const auto selected = identityIndex(selectedIdentity, selectedIndexHint);
        if (selectedIdentity != mentionSelectionIdentity) {
            mentionSelectionIdentity = selectedIdentity;
            if (selected < buffer.messages.size() && buffer.messages[selected].mention &&
                readable(buffer, selected))
                mentionCursor = buffer.messages[selected].id;
        }
        for (size_t i = 0; i < mentions.size(); ++i)
            if (mentions[i]->id == mentionCursor)
                return i;
        return mentions.size();
    }
    void navigateMention(int direction) {
        const auto *buffer = current();
        if (!buffer || pane.buffer != active)
            return;
        const auto mentions = retainedMentions(*buffer);
        const auto position = mentionPosition(*buffer, mentions);
        if (mentions.empty() || (direction < 0 && position == 0) ||
            (direction > 0 && position >= mentions.size() - 1))
            return;
        const auto next = direction < 0 ? position - 1 : position + 1;
        mentionCursor = mentions[next]->id;
        model.actions.push_back({"source-jump", buffer->id, mentionCursor, {}});
    }
    bool jumpToSourceMessage(std::string_view id) {
        auto *buffer = current();
        if (!buffer || buffer->kind == "mentions")
            return false;
        if (active != pane.buffer || !cache)
            sync();
        const auto entry = std::find_if(buffer->messages.begin(), buffer->messages.end(),
                                        [id](const Message &entry) { return entry.id == id; });
        if (entry == buffer->messages.end() || entry->deleted || !sourceContext(*buffer, *entry))
            return false;
        const auto policy = foolPolicy(*buffer, *entry);
        if (foolish(*buffer, *entry) && policy != FoolDisplay::Shown &&
            policy != FoolDisplay::Dimmed &&
            !readable(*buffer, size_t(entry - buffer->messages.begin())))
            return false;
        cancelFocusReveal();
        pane.following = false;
        mentionCursorBuffer = buffer->id;
        mentionCursor = entry->id;
        jumpTarget = entry->id;
        view.markLayoutDirty();
        return true;
    }
    bool readable(const Buffer &buffer, size_t index) const {
        if (index >= buffer.messages.size() || !cache || index >= cache->rows.size() ||
            cache->messagesRevision != buffer.messagesRevision ||
            cache->rows[index].id != buffer.messages[index].id || buffer.messages[index].deleted ||
            cache->rows[index].hiddenActivity ||
            (cache->rows[index].activitySummary && !cache->rows[index].activityExpanded))
            return false;
        const auto &entry = buffer.messages[index];
        const auto key = std::string_view(cache->rows[index].key);
        if (entry.own && !entry.history && !entry.label.empty()
                ? !key.starts_with("echo:") || key.substr(5) != entry.label
                : !key.starts_with("message:") || key.substr(8) != entry.id)
            return false;
        const auto *source = sourceContext(buffer, entry);
        if (!source)
            return false;
        const auto policy = foolPolicy(buffer, entry);
        const auto network = model.networks.find(source->server);
        const auto mapping =
            network == model.networks.end() ? "rfc1459" : network->second.caseMapping;
        return !foolish(buffer, entry) || policy == FoolDisplay::Shown ||
               policy == FoolDisplay::Dimmed ||
               (buffer.kind != "mentions" && policy != FoolDisplay::Hidden &&
                cache->rows[index].foolOpen && cache->revealedFoolList == model.settings.fools &&
                cache->revealedBufferFools == model.settings.bufferFools &&
                cache->revealedFoolDisplay == model.settings.foolDisplay &&
                cache->revealedFoolMapping == mapping);
    }
    bool concealedFool(const Buffer &buffer, size_t index) const {
        if (index >= buffer.messages.size())
            return false;
        const auto &entry = buffer.messages[index];
        if (entry.deleted || !foolish(buffer, entry) ||
            foolPolicy(buffer, entry) != FoolDisplay::Blurred)
            return false;
        // Access restrictions also cover summaries and tombstones, not just concealment.
        return !readable(buffer, index);
    }
    size_t identityIndex(uint64_t identity, size_t hint = absent) const {
        if (!cache || !identity)
            return absent;
        if (hint < cache->rows.size() && cache->rows[hint].focusIdentity == identity)
            return hint;
        const auto found =
            std::find_if(cache->rows.begin(), cache->rows.end(),
                         [identity](const Row &row) { return row.focusIdentity == identity; });
        return found == cache->rows.end() ? absent : size_t(found - cache->rows.begin());
    }
    bool readableIdentity(uint64_t identity) const {
        const auto buffer = model.buffers.find(active);
        return active == pane.buffer && buffer != model.buffers.end() &&
               readable(buffer->second, identityIndex(identity));
    }
    std::optional<OrderedTextRange> orderedTextRange() const {
        if (!selectedText || selectedText->buffer != active || !cache || pane.buffer != active)
            return std::nullopt;
        const auto buffer = model.buffers.find(active);
        if (buffer == model.buffers.end())
            return std::nullopt;
        size_t first = identityIndex(selectedText->anchor.identity, selectedText->anchor.hint);
        size_t last = identityIndex(selectedText->focus.identity, selectedText->focus.hint);
        if (first >= buffer->second.messages.size() || last >= buffer->second.messages.size() ||
            buffer->second.messages[first].deleted || buffer->second.messages[last].deleted ||
            cache->rows[first].key != ui_logic::messageKey(buffer->second.messages[first]) ||
            cache->rows[last].key != ui_logic::messageKey(buffer->second.messages[last]))
            return std::nullopt;
        auto begin = selectedText->anchor;
        auto end = selectedText->focus;
        if (first > last || (first == last && begin.byte > end.byte)) {
            std::swap(first, last);
            std::swap(begin, end);
        }
        return OrderedTextRange{first, last, begin, end};
    }
    void clearTextSelection() {
        const bool captured = textDrag.armed;
        textDrag = {};
        selectedText.reset();
        keyboardSelection = false;
        for (auto &slot : slots)
            for (auto *rect : slot.selectionRects)
                rect->setVisible(false);
        if (captured && dispatcher && dispatcher->pointerCaptured())
            dispatcher->cancelPointerCapture();
        view.markPaintDirty();
    }
    void ensureSelectionGeometry(Row &row, const Message &entry) {
        if (!selectionRenderer || row.selectionGeometryReady || !row.fragmentsReady)
            return;
        if (!row.selectionText)
            row.selectionText.emplace();
        const auto document = parseIrcFormatting(
            entry.deleted ? std::string_view("Message deleted") : entry.text,
            themeForegroundColor("on_surface"), themeSurfaceColor("surface_container"));
        row.selectionText->update(document.text);
        if (selectedText)
            for (auto *endpoint : {&selectedText->anchor, &selectedText->focus})
                if (endpoint->identity == row.focusIdentity)
                    endpoint->byte = row.selectionText->snap(
                        std::min(endpoint->byte, row.selectionText->text().size()));
        row.selectionGeometryReady = true;
    }
    void ensureFragmentSelection(Row &row, Fragment &fragment) {
        if (fragment.selectionReady || !selectionRenderer || !row.selectionText)
            return;
        fragment.selection.rebuild(*selectionRenderer, *row.selectionText, fragment.displayBegin,
                                   fragment.displayEnd, fragment.text, cache->geometry.font,
                                   fragment.font, 0, true);
        selectionScratch.reserve(fragment.displayEnd - fragment.displayBegin + 2);
        fragment.selectionReady = true;
    }
    std::optional<LayoutRect> viewportBounds() const {
        if (!view.visible() || view.width() <= 0 || view.height() <= 0)
            return std::nullopt;
        float left, top, right, bottom;
        Node::transformedBounds(scroll, left, top, right, bottom);
        for (const Node *node = &view; node; node = node->parent()) {
            if (!node->visible())
                return std::nullopt;
            if (node->clipChildren()) {
                float l, t, r, b;
                Node::transformedBounds(node, l, t, r, b);
                left = std::max(left, l);
                top = std::max(top, t);
                right = std::min(right, r);
                bottom = std::min(bottom, b);
            }
        }
        if (right <= left || bottom <= top)
            return std::nullopt;
        return LayoutRect{left, top, right - left, bottom - top};
    }
    std::optional<TextEndpoint> textHit(float sceneX, float sceneY, bool insideOnly,
                                        uint64_t onlyIdentity = 0) {
        auto *buffer = current();
        if (!buffer || !cache || !selectionRenderer)
            return std::nullopt;
        const auto viewport = viewportBounds();
        if (!viewport)
            return std::nullopt;
        const float right = viewport->x + viewport->width;
        const float bottom = viewport->y + viewport->height;
        if (insideOnly &&
            (sceneX < viewport->x || sceneX >= right || sceneY < viewport->y || sceneY >= bottom))
            return std::nullopt;
        // Keep textDrag's original pointer for autoscroll; clamp only this endpoint query.
        if (!insideOnly) {
            sceneX = std::clamp(sceneX, viewport->x, std::nextafter(right, viewport->x));
            sceneY = std::clamp(sceneY, viewport->y, std::nextafter(bottom, viewport->y));
        }
        Slot *nearest = nullptr;
        float best = std::numeric_limits<float>::infinity();
        float hitX = 0, hitY = 0;
        for (auto &slot : slots) {
            if (!slot.area->visible() || slot.buffer != active ||
                slot.index >= buffer->messages.size() || slot.index >= cache->rows.size() ||
                (onlyIdentity && slot.focusIdentity != onlyIdentity))
                continue;
            const auto &entry = buffer->messages[slot.index];
            const auto &row = cache->rows[slot.index];
            if (!readable(*buffer, slot.index) || (entry.presence && buffer->kind != "search") ||
                entry.deleted || row.lines.empty() || !row.fragmentsReady ||
                slot.focusIdentity != row.focusIdentity)
                continue;
            float bodyLeft = std::numeric_limits<float>::infinity();
            float bodyTop = bodyLeft;
            float bodyRight = -bodyLeft, bodyBottom = -bodyLeft;
            for (const auto &[localX, localY] :
                 {std::pair{slot.bodyX, row.bodyOffset},
                  std::pair{slot.bodyX + row.contentWidth, row.bodyOffset},
                  std::pair{slot.bodyX, row.bodyOffset + row.bodyHeight},
                  std::pair{slot.bodyX + row.contentWidth, row.bodyOffset + row.bodyHeight}}) {
                float x, y;
                Node::mapToScene(slot.content, localX, localY, x, y);
                bodyLeft = std::min(bodyLeft, x);
                bodyTop = std::min(bodyTop, y);
                bodyRight = std::max(bodyRight, x);
                bodyBottom = std::max(bodyBottom, y);
            }
            if (bodyRight <= viewport->x || bodyLeft >= right || bodyBottom <= viewport->y ||
                bodyTop >= bottom)
                continue;
            float x, y;
            // The return value is containment, not inversion success; drag endpoints may be
            // outside.
            Node::mapFromScene(slot.content, sceneX, sceneY, x, y);
            const float dy =
                std::max({row.bodyOffset - y, y - row.bodyOffset - row.bodyHeight, 0.f});
            const bool inside = dy == 0 && x >= slot.bodyX && x <= slot.bodyX + row.contentWidth;
            if (insideOnly && !inside)
                continue;
            if (dy < best) {
                best = dy;
                nearest = &slot;
                hitX = x;
                hitY = y;
            }
        }
        if (!nearest)
            return std::nullopt;
        auto &row = cache->rows[nearest->index];
        ensureSelectionGeometry(row, buffer->messages[nearest->index]);
        if (!row.selectionGeometryReady)
            return std::nullopt;
        const float lineY = hitY - row.bodyOffset;
        const VisualLine *line = &row.lines.back();
        for (const auto &candidate : row.lines)
            if (lineY < candidate.top + candidate.height) {
                line = &candidate;
                break;
            }
        if (line->firstFragment == line->endFragment)
            return TextEndpoint{row.focusIdentity, row.selectionText->snap(line->displayBegin),
                                nearest->index};
        size_t chosen = absent;
        best = std::numeric_limits<float>::infinity();
        for (size_t i = line->firstFragment;
             i < line->endFragment && i < row.fragments.size() && i < nearest->text.size(); ++i) {
            const auto *area = nearest->text[i].node->parent();
            const float dx = std::max({area->x() - (hitX - nearest->bodyX),
                                       hitX - nearest->bodyX - area->x() - area->width(), 0.f});
            if (dx < best) {
                best = dx;
                chosen = i;
            }
        }
        if (chosen == absent)
            return std::nullopt;
        auto &fragment = row.fragments[chosen];
        ensureFragmentSelection(row, fragment);
        const auto *text = nearest->text[chosen].node;
        const auto *area = text->parent();
        const auto &metrics = fragment.selection.metrics();
        const float scale = selectionRenderer->renderScale();
        const float localX = hitX - nearest->bodyX - area->x() - text->x() + metrics.alignOffsetX;
        const float localY = hitY - row.bodyOffset - area->y() - text->y() +
                             std::floor(metrics.layoutBaseline * scale) / scale;
        size_t byte = fragment.selection.hit(localX, localY);
        const auto logical = row.selectionText->text();
        if (chosen + 1 == line->endFragment && fragment.displayEnd < line->displayEnd &&
            line->displayEnd <= logical.size()) {
            const auto gap =
                logical.substr(fragment.displayEnd, line->displayEnd - fragment.displayEnd);
            const auto end =
                row.selectionText->snap(fragment.displayEnd, MessageSelectionSnap::After);
            if (byte <= end &&
                std::all_of(gap.begin(), gap.end(), [](char c) { return c == ' '; })) {
                // Native unpainted suffixes share the logical trailing caret, including bidi
                // controls.
                fragment.selection.rectangles(byte, end, selectionScratch);
                if (selectionScratch.empty())
                    byte = row.selectionText->snap(line->displayEnd, MessageSelectionSnap::After);
            }
        }
        return TextEndpoint{row.focusIdentity, byte, nearest->index};
    }
    bool beginTextSelection(const Slot &slot, const InputArea::PointerData &pointer,
                            const InputArea &receiver) {
        if (pointer.button != BTN_LEFT || !pointer.pressed)
            return false;
        float sceneX, sceneY;
        Node::mapToScene(&receiver, pointer.localX, pointer.localY, sceneX, sceneY);
        const auto hit = textHit(sceneX, sceneY, true, slot.focusIdentity);
        if (!hit)
            return false;
        slot.area->setCursorShape(cursorText);
        cancelFocusReveal();
        navigationRevealIdentity = 0;
        for (auto &entry : slots)
            for (auto *rect : entry.selectionRects)
                rect->setVisible(false);
        keyboardSelection = false;
        textDrag = {};
        textDrag.armed = true;
        textDrag.owner = slot.focusIdentity;
        textDrag.pressX = textDrag.x = sceneX;
        textDrag.pressY = textDrag.y = sceneY;
        textDrag.lastScroll = model.now;
        selectedText = TextRange{active, *hit, *hit};
        selectedIdentity = slot.focusIdentity;
        selectedIndexHint = slot.index;
        if (dispatcher) {
            const auto *focused = dispatcher->focusedArea();
            if (!focused || focused->tabFocusKey() != composerFocusKey) {
                pane.focusComposer = false;
                dispatcher->setFocus(keys);
            }
        }
        view.markPaintDirty();
        return true;
    }
    void moveTextSelection(float x, float y) {
        if (!textDrag.armed || !selectedText)
            return;
        textDrag.x = x;
        textDrag.y = y;
        if (!textDrag.active && std::max(std::abs(x - textDrag.pressX),
                                         std::abs(y - textDrag.pressY)) > 4 * themeScale())
            textDrag.active = true;
        if (!textDrag.active)
            return;
        if (const auto hit = textHit(x, y, false))
            selectedText->focus = *hit;
        view.markLayoutDirty();
    }
    void moveTextSelection(const InputArea &receiver, const InputArea::PointerData &pointer) {
        float sceneX, sceneY;
        Node::mapToScene(&receiver, pointer.localX, pointer.localY, sceneX, sceneY);
        moveTextSelection(sceneX, sceneY);
    }
    void finishTextSelection(uint64_t identity) {
        if (textDrag.armed && textDrag.owner == identity) {
            textDrag.suppressedClick = textDrag.active ? identity : 0;
            textDrag.armed = textDrag.active = false;
            textDrag.owner = 0;
            for (auto &slot : slots)
                if (slot.focusIdentity == identity)
                    slot.area->setCursorShape(cursorArrow);
            view.markPaintDirty();
        }
    }
    bool consumeSelectionClick(uint64_t identity) {
        if (textDrag.suppressedClick != identity)
            return false;
        textDrag.suppressedClick = 0;
        return true;
    }
    void cancelTextDrag(uint64_t identity) {
        if (textDrag.armed && textDrag.owner == identity) {
            textDrag.armed = false;
            clearTextSelection();
        }
    }
    void tickTextSelection() {
        if (!textDrag.armed || !textDrag.active || !cache)
            return;
        const double elapsed = std::clamp(model.now - textDrag.lastScroll, 0., .05);
        textDrag.lastScroll = model.now;
        if (!elapsed)
            return;
        const auto viewport = viewportBounds();
        if (!viewport)
            return;
        const float top = viewport->y, bottom = viewport->y + viewport->height;
        const float edge = std::min((bottom - top) * .25f, 24 * themeScale());
        if (edge <= 0)
            return;
        float direction = 0;
        if (textDrag.y < top + edge)
            direction = -std::clamp((top + edge - textDrag.y) / edge, 0.f, 1.f);
        else if (textDrag.y > bottom - edge)
            direction = std::clamp((textDrag.y - bottom + edge) / edge, 0.f, 1.f);
        if (direction != 0) {
            const bool wasArranging = arranging;
            arranging = true;
            scroll->setScrollOffset(scroll->scrollOffset() +
                                    direction * 320 * themeScale() * float(elapsed));
            arranging = wasArranging;
            pane.following = false;
            view.markLayoutDirty();
        }
    }
    void paintTextSelection(Slot &slot, const std::optional<OrderedTextRange> &selected,
                            float sceneTop, float sceneBottom) {
        size_t used = 0;
        if (selected && selectionRenderer && slot.index >= selected->first &&
            slot.index <= selected->last && slot.index < cache->rows.size()) {
            auto &row = cache->rows[slot.index];
            const auto &entry = model.buffers.at(active).messages[slot.index];
            if (readable(model.buffers.at(active), slot.index) &&
                (!entry.presence || model.buffers.at(active).kind == "search") && !entry.deleted) {
                ensureSelectionGeometry(row, entry);
                if (row.selectionText && row.selectionGeometryReady) {
                    const size_t begin = row.selectionText->snap(
                        std::min(slot.index == selected->first ? selected->begin.byte : 0,
                                 row.selectionText->text().size()),
                        MessageSelectionSnap::Before);
                    const size_t end = row.selectionText->snap(
                        std::min(slot.index == selected->last ? selected->end.byte
                                                              : row.selectionText->text().size(),
                                 row.selectionText->text().size()),
                        MessageSelectionSnap::After);
                    const float scale = selectionRenderer->renderScale();
                    float unused, clipTop, clipBottom;
                    // Viewport corners intentionally lie outside individual message rows.
                    Node::mapFromScene(slot.body, 0, sceneTop, unused, clipTop);
                    Node::mapFromScene(slot.body, 0, sceneBottom, unused, clipBottom);
                    if (clipBottom < clipTop)
                        std::swap(clipTop, clipBottom);
                    for (size_t i = 0; i < row.fragments.size() && i < slot.text.size(); ++i) {
                        auto &fragment = row.fragments[i];
                        const auto *text = slot.text[i].node;
                        const auto *area = text->parent();
                        if (area->y() + area->height() < clipTop || area->y() > clipBottom)
                            continue;
                        ensureFragmentSelection(row, fragment);
                        fragment.selection.rectangles(begin, end, selectionScratch);
                        const auto &metrics = fragment.selection.metrics();
                        for (const auto &cell : selectionScratch) {
                            if (used == slot.selectionRects.size()) {
                                auto *rect = rectangle(*slot.body, 0, 0, 1, 1, {}, themeScale());
                                rect->setZIndex(1);
                                addRowPaint(*slot.body, slot.body->removeChild(rect));
                                slot.selectionRects.push_back(rect);
                            }
                            auto *rect = slot.selectionRects[used++];
                            rect->setPosition(
                                area->x() + text->x() - metrics.alignOffsetX + cell.x,
                                area->y() + text->y() -
                                    std::floor(metrics.layoutBaseline * scale) / scale + cell.y);
                            rect->setSize(cell.width, cell.height);
                            auto style = rect->style();
                            style.fill = withAlpha(themeForegroundColor("primary"), .24f);
                            rect->setStyle(style);
                            rect->setVisible(true);
                        }
                    }
                }
            }
        }
        for (; used < slot.selectionRects.size(); ++used)
            slot.selectionRects[used]->setVisible(false);
    }
    bool copySelection() {
        const auto selected = orderedTextRange();
        if (!selected ||
            (selected->first == selected->last && selected->begin.byte == selected->end.byte))
            return false;
        const auto &buffer = model.buffers.at(active);
        std::string output;
        bool first = true;
        for (size_t i = selected->first; i <= selected->last; ++i) {
            const auto &entry = buffer.messages[i];
            if (!readable(buffer, i) || (entry.presence && buffer.kind != "search") ||
                entry.deleted)
                continue;
            auto document = parseIrcFormatting(entry.text, themeForegroundColor("on_surface"),
                                               themeSurfaceColor("surface_container"));
            size_t begin = i == selected->first ? selected->begin.byte : 0;
            size_t end = i == selected->last ? selected->end.byte : document.text.size();
            if (i == selected->first || i == selected->last) {
                MessageSelectionText logical;
                logical.update(document.text);
                begin = logical.snap(std::min(begin, document.text.size()),
                                     MessageSelectionSnap::Before);
                end =
                    logical.snap(std::min(end, document.text.size()), MessageSelectionSnap::After);
            }
            if (!first)
                output += '\n';
            output.append(document.text, begin, end - begin);
            first = false;
        }
        if (first || output.empty())
            return false;
        clipboard.setClipboardText(output);
        return true;
    }
    size_t targetIndex(const TranscriptMessageTarget &target) const {
        if (!cache || target.buffer != active || pane.buffer != active)
            return absent;
        const auto buffer = model.buffers.find(active);
        if (buffer == model.buffers.end())
            return absent;
        const size_t index = identityIndex(
            target.identity, target.identity == selectedIdentity ? selectedIndexHint : absent);
        if (index >= buffer->second.messages.size() || index >= cache->rows.size())
            return absent;
        const auto &entry = buffer->second.messages[index];
        return readable(buffer->second, index) &&
                       (!entry.presence || buffer->second.kind == "search") &&
                       cache->rows[index].key == ui_logic::messageKey(entry)
                   ? index
                   : absent;
    }
    std::optional<TranscriptMessageTarget>
    resolveMessage(const TranscriptMessageTarget &target) const {
        const size_t index = targetIndex(target);
        if (index == absent)
            return std::nullopt;
        const auto &buffer = model.buffers.at(active);
        const auto &entry = buffer.messages[index];
        TranscriptMessageTarget resolved{};
        resolved.buffer = active;
        resolved.identity = cache->rows[index].focusIdentity;
        resolved.nick = entry.nick;
        resolved.canReply = quotable(buffer, entry);
        resolved.canReact = reactionPickerAllowed(model, buffer, entry);
        const auto slot = std::find_if(slots.begin(), slots.end(), [&](const Slot &candidate) {
            return candidate.buffer == active && candidate.focusIdentity == resolved.identity &&
                   candidate.area->visible();
        });
        if (slot != slots.end()) {
            float left, top, right, bottom, viewLeft, viewTop, viewRight, viewBottom;
            Node::transformedBounds(slot->material, left, top, right, bottom);
            Node::transformedBounds(scroll, viewLeft, viewTop, viewRight, viewBottom);
            left = std::max(left, viewLeft);
            top = std::max(top, viewTop);
            right = std::min(right, viewRight);
            bottom = std::min(bottom, viewBottom);
            if (right > left && bottom > top)
                resolved.anchor = LayoutRect{left, top, right - left, bottom - top};
        }
        return resolved;
    }
    std::optional<TranscriptMessageTarget> selectedMessage() const {
        if (sourceJumpIdentity && effectivelyVisible()) {
            TranscriptMessageTarget target{};
            target.buffer = active;
            target.identity = sourceJumpIdentity;
            return resolveMessage(target);
        }
        if (!nativeFocus(view))
            return std::nullopt;
        TranscriptMessageTarget target{};
        target.buffer = active;
        if (keys->focused())
            target.identity = selectedIdentity;
        else
            for (const auto &slot : slots)
                if (slot.buffer == active && slot.area->visible() && nativeFocus(*slot.area)) {
                    target.identity = slot.focusIdentity;
                    break;
                }
        return target.identity ? resolveMessage(target) : std::nullopt;
    }
    bool copySelectedMessage() {
        if (keyboardSelection)
            return false;
        const auto target = selectedMessage();
        const size_t index = target ? targetIndex(*target) : absent;
        if (index == absent || model.buffers.at(active).messages[index].deleted)
            return false;
        const auto &entry = model.buffers.at(active).messages[index];
        clipboard.setClipboardText(parseIrcFormatting(entry.text,
                                                      themeForegroundColor("on_surface"),
                                                      themeSurfaceColor("surface_container"))
                                       .text);
        return true;
    }
    bool textSelectionCommand(std::string_view command) {
        const auto target = selectedMessage();
        const size_t index = target ? targetIndex(*target) : absent;
        if (index == absent || model.buffers.at(active).messages[index].deleted) {
            clearTextSelection();
            return false;
        }
        if (command == "set-mark") {
            if (keyboardSelection && selectedText &&
                selectedText->focus.identity == target->identity)
                selectedText->anchor = selectedText->focus;
            else {
                clearTextSelection();
                const TextEndpoint endpoint{target->identity, 0, index};
                selectedText = TextRange{active, endpoint, endpoint};
                keyboardSelection = true;
            }
            view.markPaintDirty();
            return true;
        }
        if (!keyboardSelection || !selectedText ||
            selectedText->focus.identity != target->identity || !orderedTextRange()) {
            clearTextSelection();
            return false;
        }
        const auto &entry = model.buffers.at(active).messages[index];
        const auto document = parseIrcFormatting(entry.text, themeForegroundColor("on_surface"),
                                                 themeSurfaceColor("surface_container"));
        MessageSelectionText logical;
        logical.update(document.text);
        auto &byte = selectedText->focus.byte;
        byte = logical.snap(std::min(byte, document.text.size()), MessageSelectionSnap::Before);
        const auto &boundaries = logical.boundaries();
        if (command == "backward-char") {
            const auto it = std::lower_bound(boundaries.begin(), boundaries.end(), byte);
            if (it != boundaries.begin())
                byte = *std::prev(it);
        } else if (command == "forward-char") {
            const auto it = std::upper_bound(boundaries.begin(), boundaries.end(), byte);
            if (it != boundaries.end())
                byte = *it;
        } else if (command == "beginning-of-line") {
            const auto pos = byte ? document.text.rfind('\n', byte - 1) : std::string::npos;
            byte =
                logical.snap(pos == std::string::npos ? 0 : pos + 1, MessageSelectionSnap::After);
        } else if (command == "end-of-line") {
            const auto pos = document.text.find('\n', byte);
            byte = logical.snap(pos == std::string::npos ? document.text.size() : pos,
                                MessageSelectionSnap::Before);
        } else
            return false;
        view.markPaintDirty();
        return true;
    }
    bool navigateMessage(int delta) {
        auto *buffer = current();
        if (!buffer || !cache || active != pane.buffer || !effectivelyVisible())
            return false;
        cancelFocusReveal();
        view.clearTextSelection();
        jumpTarget.clear();
        const bool ownsKeyboard = nativeFocus(view);
        if (!ownsKeyboard) {
            selectedIdentity = 0;
            selectedIndexHint = absent;
        }
        if (!ownsKeyboard && (!dispatcher || !dispatcher->focusedArea()))
            return false;
        size_t index = ownsKeyboard && keys->focused()
                           ? identityIndex(selectedIdentity, selectedIndexHint)
                           : absent;
        if (ownsKeyboard && !keys->focused())
            for (const auto &slot : slots)
                if (slot.buffer == active && nativeFocus(*slot.area)) {
                    index = identityIndex(slot.focusIdentity, slot.index);
                    if (index < cache->rows.size() && index < buffer->messages.size() &&
                        readable(*buffer, index) && !buffer->messages[index].presence) {
                        selectedIdentity = cache->rows[index].focusIdentity;
                        selectedIndexHint = index;
                    } else {
                        selectedIdentity = 0;
                        selectedIndexHint = absent;
                    }
                    break;
                }
        if (ownsKeyboard && index == absent)
            return false;
        if (delta < 0) {
            if (index == absent)
                index = buffer->messages.size();
            do {
                if (!index) {
                    if (selectedIdentity) {
                        pane.focusComposer = false;
                        if (dispatcher)
                            dispatcher->setFocus(keys);
                        return true;
                    }
                    return false;
                }
                const size_t previous = index - 1;
                if (previous < cache->rows.size() && buffer->messages[previous].presence &&
                    cache->rows[previous].activityBegin != absent &&
                    cache->rows[previous].activityBegin < previous)
                    index = cache->rows[previous].activityBegin;
                else
                    index = previous;
            } while (!readable(*buffer, index) || buffer->messages[index].presence);
        } else if (delta > 0 && index != absent) {
            ++index;
            while (index < buffer->messages.size() &&
                   (!readable(*buffer, index) || buffer->messages[index].presence)) {
                if (index < cache->rows.size() && cache->rows[index].activityEnd != absent &&
                    cache->rows[index].activityEnd > index)
                    index = cache->rows[index].activityEnd;
                else
                    ++index;
            }
        } else
            index = absent;
        if (index >= buffer->messages.size() || index >= cache->rows.size()) {
            selectedIdentity = navigationRevealIdentity = 0;
            selectedIndexHint = absent;
            view.markLayoutDirty();
            return false;
        }
        selectedIdentity = cache->rows[index].focusIdentity;
        selectedIndexHint = index;
        navigationRevealIdentity = selectedIdentity;
        for (auto &slot : slots)
            if (slot.buffer == active && slot.focusIdentity == selectedIdentity)
                cancelEntrance(slot);
        pane.following = false;
        pane.focusComposer = false;
        if (dispatcher)
            dispatcher->setFocus(keys);
        view.markLayoutDirty();
        return true;
    }
    bool replyToMessage(const TranscriptMessageTarget &target) {
        const size_t index = targetIndex(target);
        if (index == absent)
            return false;
        const auto &buffer = model.buffers.at(active);
        const auto &entry = buffer.messages[index];
        if (!quotable(buffer, entry))
            return false;
        reply(buffer.id, cache->rows[index].key);
        return true;
    }
    bool reactToMessage(const TranscriptMessageTarget &target, std::string emoji) {
        const size_t index = targetIndex(target);
        if (index == absent)
            return false;
        const auto &buffer = model.buffers.at(active);
        const auto &entry = buffer.messages[index];
        if (!referenceable(buffer, entry) || !reactionAllowed(model, buffer, entry, emoji))
            return false;
        react(buffer.id, cache->rows[index].key, std::move(emoji));
        return true;
    }
    const Message *message(std::string_view bufferId, std::string_view key) {
        const auto found = model.buffers.find(std::string(bufferId));
        if (found == model.buffers.end())
            return nullptr;
        const auto cached = caches.find(found->first);
        if (cached != caches.end()) {
            const auto index = cached->second.keys.find(std::string(key));
            if (index != cached->second.keys.end() &&
                index->second < found->second.messages.size() &&
                ui_logic::messageKey(found->second.messages[index->second]) == key)
                return &found->second.messages[index->second];
        }
        const auto row =
            std::find_if(found->second.messages.begin(), found->second.messages.end(),
                         [&](const auto &entry) { return ui_logic::messageKey(entry) == key; });
        return row == found->second.messages.end() ? nullptr : &*row;
    }
    bool scrollHalfPage(int direction) {
        if (!direction || !cache || active != pane.buffer || !current() || !effectivelyVisible() ||
            scroll->height() <= 0)
            return false;
        cancelFocusReveal();
        jumpTarget.clear();
        navigationRevealIdentity = 0;
        const float offset =
            std::clamp(scroll->scrollOffset() + scroll->height() * (direction < 0 ? -.5f : .5f),
                       0.f, scroll->maxScrollOffset());
        pane.following = scroll->maxScrollOffset() - offset <= 1;
        if (pane.following)
            pane.unseen = 0;
        const bool wasArranging = std::exchange(arranging, true);
        scroll->setScrollOffset(offset);
        arranging = wasArranging;
        view.markLayoutDirty();
        return true;
    }
    bool scrollToBoundary(bool newest) {
        if (!cache || active != pane.buffer || !current() || !effectivelyVisible() ||
            scroll->height() <= 0)
            return false;
        cancelFocusReveal();
        scroll->cancelPendingScrollRequests();
        jumpTarget.clear();
        navigationRevealIdentity = 0;
        pane.following = newest;
        if (newest) {
            clearTextSelection();
            pane.unseen = 0;
        } else {
            const bool wasArranging = std::exchange(arranging, true);
            scroll->setScrollOffset(0);
            arranging = wasArranging;
        }
        view.markLayoutDirty();
        return true;
    }
    static void preserveTypingFocus(Node &node) {
        if (auto *area = dynamic_cast<InputArea *>(&node)) {
            area->setFocusable(false);
            area->setTabStop(false);
            area->setFocusOnPointerPress(false);
        }
        for (const auto &child : node.children())
            preserveTypingFocus(*child);
    }
    void closeTypingList() {
        if (!typingListOpen)
            return;
        typingListOpen = false;
        cancelMessagePresses(*typingList);
        if (dispatcher)
            for (Node *node = dispatcher->capturedArea(); node; node = node->parent())
                if (node == typingList) {
                    dispatcher->cancelPointerCapture();
                    break;
                }
        typingList->setVisible(false);
        auto &content = *typingListScroll->content();
        while (!content.children().empty())
            content.removeChild(content.children().back().get());
        view.markLayoutDirty();
    }
    void toggleTypingList() {
        if (typingListOpen) {
            closeTypingList();
            return;
        }
        if (typingParticipants.empty() || active != pane.buffer)
            return;
        auto &content = *typingListScroll->content();
        while (!content.children().empty())
            content.removeChild(content.children().back().get());
        for (const auto &nick : typingParticipants) {
            auto row = std::make_unique<Flex>();
            row->setJustify(FlexJustify::Center);
            row->setAlign(FlexAlign::Start);
            row->setHitTestVisible(false);
            auto *name = label(*row, nick, 0, 0, 1, 14 * themeScale(),
                               nickColor(model, nick, current()->server), themeUiFont());
            name->setHitTestVisible(false);
            content.addChild(std::move(row));
        }
        typingListScroll->requestScrollToOffset(0);
        typingListOpen = true;
        typingList->setVisible(true);
        view.markLayoutDirty();
    }
    bool handleTypingKey(const InputArea::KeyData &key) {
        if (!key.pressed || key.preedit || !nativeFocus(view))
            return false;
        if (typingOverflow->inputArea()->focused() && typingListOpen && !key.modifiers) {
            const float step = 28 * themeScale();
            if (key.sym == XKB_KEY_Escape)
                closeTypingList();
            else if (key.sym == XKB_KEY_Up)
                typingListScroll->scrollBy(-step);
            else if (key.sym == XKB_KEY_Down)
                typingListScroll->scrollBy(step);
            else if (key.sym == XKB_KEY_Page_Up)
                typingListScroll->scrollBy(-typingListScroll->height());
            else if (key.sym == XKB_KEY_Page_Down)
                typingListScroll->scrollBy(typingListScroll->height());
            else if (key.sym == XKB_KEY_Home)
                typingListScroll->setScrollOffset(0);
            else if (key.sym == XKB_KEY_End)
                typingListScroll->setScrollOffset(typingListScroll->maxScrollOffset());
            else
                return false;
            view.markLayoutDirty();
            return true;
        }
        return false;
    }
    bool handleKey(const InputArea::KeyData &key) {
        if (!key.pressed || key.preedit || !nativeFocus(view))
            return false;
        if (handleTypingKey(key))
            return true;
        if (!key.modifiers && keys->focused() &&
            (key.sym == XKB_KEY_Return || key.sym == XKB_KEY_KP_Enter)) {
            const auto *buffer = current();
            const auto index = identityIndex(selectedIdentity, selectedIndexHint);
            if (!buffer || active != pane.buffer || !readable(*buffer, index) ||
                buffer->messages[index].presence || buffer->messages[index].id.empty())
                return false;
            const auto slot = std::find_if(slots.begin(), slots.end(), [&](const Slot &entry) {
                return entry.buffer == active && entry.focusIdentity == selectedIdentity &&
                       entry.area->visible() && !entry.area->excludeSubtreeFromTabOrder();
            });
            if (slot != slots.end())
                messageLinks(*slot->area, *buffer, index);
            return true;
        }
        if (contextKey(key)) {
            if (dispatcher && dispatcher->focusedArea() &&
                dispatcher->focusedArea()->tabFocusKey().starts_with("blastirc.media.thumbnail."))
                return false;
            const auto slot = std::find_if(slots.begin(), slots.end(), [&](const Slot &entry) {
                return entry.buffer == active && !entry.area->excludeSubtreeFromTabOrder() &&
                       (nativeFocus(*entry.area) ||
                        (keys->focused() && entry.focusIdentity == selectedIdentity));
            });
            if (slot == slots.end())
                return false;
            menu(*slot->area, slot->buffer, slot->key);
            return true;
        }
        if (key.sym == XKB_KEY_Home) {
            cancelFocusReveal();
            pane.following = false;
            scroll->setScrollOffset(0);
        } else if (key.sym == XKB_KEY_End) {
            cancelFocusReveal();
            pane.following = true;
            pane.unseen = 0;
        } else
            return false;
        view.markLayoutDirty();
        return true;
    }
    void launch(std::string url, uint64_t identity, bool preview = true) {
        const auto *buffer = current();
        if (!buffer || !readable(*buffer, identityIndex(identity)) || !allowedUrl(url))
            return;
        if (preview && mediaController && mediaController->openUrl(url, identity))
            return;
        if (urlOpeners.size() >= 16) {
            model.status = "Too many URL open requests are still running.";
            return;
        }
        char executable[] = "xdg-open";
        char *arguments[] = {executable, url.data(), nullptr};
        pid_t process = -1;
        const int error = posix_spawnp(&process, executable, nullptr, nullptr, arguments, environ);
        if (!error)
            urlOpeners.push_back(process);
        else
            model.status = "Cannot open URL: xdg-open could not be started.";
    }
    size_t resolve(const Buffer &buffer, const std::string &id) const {
        const auto found = cache->ids.find(id);
        if (found != cache->ids.end()) {
            if (found->second == absent)
                return absent;
            if (found->second < buffer.messages.size() && buffer.messages[found->second].id == id)
                return found->second;
        }
        // Only an unseen/changed reference scans IDs, never text or wrapping.
        // Negative results survive until history changes or an echo supplies that ID.
        const auto message = std::find_if(buffer.messages.begin(), buffer.messages.end(),
                                          [&](const auto &entry) { return entry.id == id; });
        const size_t index =
            message == buffer.messages.end() ? absent : size_t(message - buffer.messages.begin());
        cache->ids[id] = index;
        return index;
    }
    void messageLinks(Node &anchor, const Buffer &buffer, size_t index) {
        const auto &entry = buffer.messages[index];
        const auto identity = cache->rows[index].focusIdentity;
        const auto *context = sourceContext(buffer, entry);
        const auto source =
            std::array{entry.id,       entry.nick,         entry.reply,        entry.sourceBuffer,
                       entry.sourceId, entry.sourceServer, entry.sourceTarget, buffer.kind,
                       context->id,    context->server,    context->target};
        const auto document = parseIrcFormatting(entry.text, themeForegroundColor("on_surface"),
                                                 themeSurfaceColor("surface_container"));
        std::vector<std::pair<std::string, std::function<void()>>> actions;
        std::vector<ContextMenuEntry> entries;
        for (const auto &link : detectLinks(document)) {
            if (std::any_of(actions.begin(), actions.end(),
                            [&](const auto &action) { return action.first == link.url; }))
                continue;
            actions.emplace_back(link.url, [this, bufferId = buffer.id, identity, source,
                                            text = entry.text, url = link.url] {
                const auto found = model.buffers.find(bufferId);
                const auto index = identityIndex(identity);
                if (bufferId != active || pane.buffer != bufferId || found == model.buffers.end() ||
                    !readable(found->second, index))
                    return;
                const auto &current = found->second.messages[index];
                const auto *context = sourceContext(found->second, current);
                if (current.text != text || current.presence ||
                    std::array{current.id, current.nick, current.reply, current.sourceBuffer,
                               current.sourceId, current.sourceServer, current.sourceTarget,
                               found->second.kind, context->id, context->server,
                               context->target} != source)
                    return;
                const auto document =
                    parseIrcFormatting(current.text, themeForegroundColor("on_surface"),
                                       themeSurfaceColor("surface_container"));
                const auto links = detectLinks(document);
                if (std::any_of(links.begin(), links.end(),
                                [&](const auto &link) { return link.url == url; }))
                    launch(url, identity);
            });
            ContextMenuEntry item;
            item.label = link.url;
            item.value = actions.size() - 1;
            entries.push_back(std::move(item));
        }
        popup(anchor, std::move(actions), std::move(entries), {});
    }
    void closePopup() {
        ++popupGeneration;
        if (!popupOwned)
            return;
        popupOwned = false;
        if (view.popupContext())
            view.popupContext()->closeSelectDropdown();
    }
    void popup(Node &anchor, std::vector<std::pair<std::string, std::function<void()>>> actions,
               std::vector<ContextMenuEntry> menu, std::optional<InputArea::PointerData> pointer) {
        if (actions.empty() || !view.popupContext())
            return;
        float left, top, right, bottom;
        Node::transformedBounds(&anchor, left, top, right, bottom);
        const auto viewport = viewportBounds();
        if (!viewport)
            return;
        const float viewLeft = viewport->x, viewTop = viewport->y;
        const float viewRight = viewLeft + viewport->width, viewBottom = viewTop + viewport->height;
        if (right <= viewLeft || left >= viewRight || bottom <= viewTop || top >= viewBottom)
            return;
        left = std::clamp(left, viewLeft, viewRight);
        top = std::clamp(top, viewTop, viewBottom);
        right = std::clamp(right, left, viewRight);
        bottom = std::clamp(bottom, top, viewBottom);
        SelectPopupContext::DropdownRequest request;
        request.inputSerial = pointer ? std::optional<uint32_t>(pointer->serial) : std::nullopt;
        request.pointerContextMenu = pointer.has_value();
        request.anchorX = int(std::floor(pointer ? pointer->sceneX : left));
        request.anchorY = int(std::floor(pointer ? pointer->sceneY : top));
        request.anchorWidth = pointer ? 1 : std::max(1, int(std::ceil(right - left)));
        request.anchorHeight = pointer ? 1 : std::max(1, int(std::ceil(bottom - top)));
        request.menuWidth = 240 * themeScale();
        request.maxMenuWidth = 420 * themeScale();
        request.fontSize = 14 * themeScale();
        request.font = themeUiFont();
        request.maxVisibleOptions = 12;
        request.menu = std::move(menu);
        std::weak_ptr<int> guard = alive;
        const auto generation = ++popupGeneration;
        const auto current = model.buffers.find(active);
        popupPolicy =
            current == model.buffers.end() ? std::string{} : privacyPolicy(current->second);
        popupOwned = true;
        view.popupContext()->openSelectDropdown(
            request, {
                         .onSelect =
                             [this, guard, generation, actions = std::move(actions)](size_t index) {
                                 if (guard.expired())
                                     return;
                                 const auto current = model.buffers.find(active);
                                 if (generation == popupGeneration && pane.buffer == active &&
                                     current != model.buffers.end() &&
                                     popupPolicy == privacyPolicy(current->second) &&
                                     index < actions.size()) {
                                     popupOwned = false;
                                     ++popupGeneration;
                                     actions[index].second();
                                 }
                             },
                         .onDismiss =
                             [this, guard, generation] {
                                 if (!guard.expired() && generation == popupGeneration)
                                     popupOwned = false;
                             },
                     });
    }
    void reply(const std::string &bufferId, const std::string &key) {
        const auto found = model.buffers.find(bufferId);
        const auto *entry = message(bufferId, key);
        if (pane.buffer != bufferId || found == model.buffers.end() || !entry || !cache ||
            !cache->keys.contains(key) || !readable(found->second, cache->keys.at(key)) ||
            !quotable(found->second, *entry))
            return;
        focus(pane.id);
        pane.reply = key;
        pane.focusComposer = true;
    }
    void react(const std::string &bufferId, const std::string &key, std::string emoji) {
        const auto found = model.buffers.find(bufferId);
        const auto *entry = message(bufferId, key);
        if (bufferId == active && pane.buffer == active && found != model.buffers.end() && entry &&
            cache && cache->keys.contains(key) && readable(found->second, cache->keys.at(key)) &&
            referenceable(found->second, *entry) &&
            reactionAllowed(model, found->second, *entry, emoji))
            model.actions.push_back({"react", bufferId, std::move(emoji), entry->id});
    }
    void menu(Node &anchor, const std::string &bufferId, const std::string &key,
              std::optional<InputArea::PointerData> pointer = {}) {
        const auto found = model.buffers.find(bufferId);
        const auto *entry = message(bufferId, key);
        if (bufferId != active || pane.buffer != active || found == model.buffers.end() || !entry ||
            !cache || !cache->keys.contains(key) || !readable(found->second, cache->keys.at(key)))
            return;
        std::vector<std::pair<std::string, std::function<void()>>> actions;
        std::vector<ContextMenuEntry> entries;
        auto actionEntry = [&](const char *icon) {
            ContextMenuEntry item;
            item.label = actions.back().first;
            item.icon = icon;
            item.value = actions.size() - 1;
            return item;
        };
        std::optional<ContextMenuEntry> deletion;
        actions.emplace_back("Copy text", [this, bufferId, key] {
            const auto found = model.buffers.find(bufferId);
            if (bufferId != active || pane.buffer != active || found == model.buffers.end() ||
                !cache || !cache->keys.contains(key) ||
                !readable(found->second, cache->keys.at(key)))
                return;
            if (const auto *entry = message(bufferId, key)) {
                const auto document =
                    parseIrcFormatting(entry->text, themeForegroundColor("on_surface"),
                                       themeSurfaceColor("surface_container"));
                clipboard.setClipboardText(document.text);
            }
        });
        entries.push_back(actionEntry("clipboard"));
        const auto index = cache->keys.at(key);
        const auto identity = cache->rows[index].focusIdentity;
        const auto url = cache->rows[index].previewUrl;
        if (mediaController && !entry->presence && !foolish(found->second, *entry) &&
            mediaController->canLoadPreview(identity, url)) {
            const auto *context = sourceContext(found->second, *entry);
            const auto source = std::array{
                entry->id,       entry->nick,         entry->reply,        entry->sourceBuffer,
                entry->sourceId, entry->sourceServer, entry->sourceTarget, found->second.kind,
                context->id,     context->server,     context->target};
            actions.emplace_back("Load preview", [this, bufferId, identity, source,
                                                  text = entry->text, url] {
                const auto found = model.buffers.find(bufferId);
                const auto index = identityIndex(identity);
                if (bufferId != active || pane.buffer != bufferId || found == model.buffers.end() ||
                    !readable(found->second, index) || !mediaController)
                    return;
                const auto &entry = found->second.messages[index];
                const auto *context = sourceContext(found->second, entry);
                if (entry.text != text || entry.presence || foolish(found->second, entry) ||
                    std::array{entry.id, entry.nick, entry.reply, entry.sourceBuffer,
                               entry.sourceId, entry.sourceServer, entry.sourceTarget,
                               found->second.kind, context->id, context->server,
                               context->target} != source)
                    return;
                const auto document =
                    parseIrcFormatting(entry.text, themeForegroundColor("on_surface"),
                                       themeSurfaceColor("surface_container"));
                const auto links = detectLinks(document);
                if (!links.empty() && links.front().url == url)
                    mediaController->loadPreview(identity, url);
            });
            entries.push_back(actionEntry("download"));
        }
        if (found->second.kind == "mentions") {
            actions.emplace_back("Open original",
                                 [this, bufferId, key] { openOriginal(bufferId, key); });
            entries.push_back(actionEntry("arrow-up-right"));
        }
        if (quotable(found->second, *entry)) {
            actions.emplace_back("Reply", [this, bufferId, key] { reply(bufferId, key); });
            entries.push_back(actionEntry("arrow-back-up"));
        }
        if (referenceable(found->second, *entry)) {
            ContextMenuEntry reactions;
            reactions.label = "React";
            reactions.icon = "mood-smile";
            for (const auto emoji : quickReactions)
                if (reactionAllowed(model, found->second, *entry, emoji)) {
                    const auto network = model.networks.find(found->second.server);
                    const bool own = network != model.networks.end() &&
                                     ownReaction(network->second, *entry, emoji);
                    actions.emplace_back(std::string(own ? "Remove " : "React ") +
                                             std::string(emoji),
                                         [this, bufferId, key, emoji = std::string(emoji)] {
                                             react(bufferId, key, emoji);
                                         });
                    auto item = actionEntry("mood-smile");
                    item.label = emoji;
                    item.checkmark = true;
                    item.toggleState = own ? 1 : 0;
                    reactions.children.push_back(std::move(item));
                }
            if (cache && bufferId == active) {
                const auto origin = cache->keys.find(key);
                if (origin != cache->keys.end() && origin->second < cache->rows.size() &&
                    reactionPickerAllowed(model, found->second, *entry)) {
                    const auto identity = cache->rows[origin->second].focusIdentity;
                    actions.emplace_back("Choose emoji…", [this, bufferId, identity] {
                        command("message-react:" + bufferId + "\n" + std::to_string(identity));
                    });
                    reactions.children.push_back(actionEntry("mood-smile"));
                }
            }
            if (!reactions.children.empty())
                entries.push_back(std::move(reactions));
            if (entry->own && capability(model, found->second, "message-tags") &&
                (capability(model, found->second, "draft/message-redaction") ||
                 capability(model, found->second, "draft/message-redaction-0.1"))) {
                actions.emplace_back("Delete message", [this, bufferId, key] {
                    const auto found = model.buffers.find(bufferId);
                    const auto *entry = message(bufferId, key);
                    if (found != model.buffers.end() && entry && entry->own && cache &&
                        cache->keys.contains(key) && readable(found->second, cache->keys.at(key)) &&
                        referenceable(found->second, *entry) &&
                        capability(model, found->second, "message-tags") &&
                        (capability(model, found->second, "draft/message-redaction") ||
                         capability(model, found->second, "draft/message-redaction-0.1")))
                        model.actions.push_back({"redact", bufferId, {}, entry->id});
                });
                deletion = actionEntry("trash");
            }
        }
        if (found->second.kind != "mentions" && !entry->nick.empty()) {
            const auto nick = entry->nick;
            actions.emplace_back("Whois " + nick, [this, bufferId, key, nick] {
                const auto *source = message(bufferId, key);
                const auto found = model.buffers.find(bufferId);
                if (source && source->nick == nick && found != model.buffers.end() && cache &&
                    cache->keys.contains(key) && readable(found->second, cache->keys.at(key)))
                    command("nick-action:" + bufferId + "\n" + nick + "\nwhois");
            });
            ContextMenuEntry separator;
            separator.separator = true;
            entries.push_back(std::move(separator));
            entries.push_back(actionEntry("user"));
        }
        if (deletion) {
            ContextMenuEntry separator;
            separator.separator = true;
            entries.push_back(std::move(separator));
            entries.push_back(std::move(*deletion));
        }
        popup(anchor, std::move(actions), std::move(entries), pointer);
    }

    bool timestampVisible(const Row &row, const Buffer &buffer, size_t index) const {
        if (!model.settings.timestamps || row.hiddenActivity ||
            (row.activitySummary && !row.activityExpanded))
            return false;
        if (cache->geometry.bubbles)
            return true;
        size_t previous = row.projected ? row.previousVisible : index ? index - 1 : absent;
        while (previous < cache->rows.size() &&
               (cache->rows[previous].hiddenActivity ||
                (cache->rows[previous].activitySummary && !cache->rows[previous].activityExpanded)))
            previous = cache->rows[previous].previousVisible;
        if (previous >= buffer.messages.size())
            return true;
        const auto day = [](const Message &entry) {
            return entry.timestamp           ? localDate(entry.timestamp, "%Y-%m-%d")
                   : entry.time.size() >= 16 ? entry.time.substr(0, 10)
                                             : std::string{};
        };
        return row.day || day(buffer.messages[index]) != day(buffer.messages[previous]) ||
               clockText(buffer.messages[index]) != clockText(buffer.messages[previous]);
    }
    void rowState(Row &row, const Buffer &buffer, size_t index) {
        const auto &entry = buffer.messages[index];
        if (!row.projected)
            row.unread = index == cache->divider;
        const size_t previous = row.projected ? row.previousVisible : index ? index - 1 : absent;
        row.day =
            previous == absent || localDate(entry.timestamp, "%Y-%m-%d") !=
                                      localDate(buffer.messages[previous].timestamp, "%Y-%m-%d");
        row.grouped = buffer.kind != "mentions" && previous != absent && previous + 1 == index &&
                      !row.day && !row.unread && !entry.presence &&
                      !(cache->geometry.channel && entry.own) && !entry.action && !entry.deleted &&
                      !buffer.messages[previous].presence && !buffer.messages[previous].action &&
                      !buffer.messages[previous].deleted &&
                      entry.nick == buffer.messages[previous].nick &&
                      entry.timestamp >= buffer.messages[previous].timestamp &&
                      entry.timestamp - buffer.messages[previous].timestamp < 300;
        row.showClock = timestampVisible(row, buffer, index);
    }
    float activityBodyWidth(const Row &row, const Message &entry, const Geometry &g,
                            float width) const {
        const bool notice = typedActivity(entry);
        const bool collapsed = row.activitySummary && !row.activityExpanded;
        const float available =
            notice ? std::max(1.f, 2 * width - g.chatWidth - 2 * g.metrics.inset * g.scale) -
                         (collapsed ? 0 : g.prefixWidth(entry.presence))
                   : g.bodyWidth(width, entry.presence);
        return std::max(1.f, available - (notice ? 2 * g.activityPadding() : 0) -
                                 (row.activitySummary ? g.activityChevron() : 0));
    }
    void estimateHeight(Row &row, const Buffer &buffer, size_t index, const Geometry &g,
                        float width) {
        const auto &entry = buffer.messages[index];
        const bool notice = typedActivity(entry);
        const bool collapsed = row.activitySummary && !row.activityExpanded;
        const float paddingY = notice ? g.activityPaddingY() : g.paddingY(entry.presence);
        row.prefixWidth = collapsed ? 0 : g.prefixWidth(entry.presence);
        const float bodyWidth = activityBodyWidth(row, entry, g, width);
        const auto &text = collapsed ? row.activityLabel : entry.text;
        const float font = notice ? g.activityFont : g.font;
        const float line = notice ? g.activityLine : g.bodyLine;
        const float lines = std::max(1.f, std::ceil(float(text.size()) * font * .5f / bodyWidth));
        row.bodyHeight = line * lines;
        row.bodyOffset = paddingY + (row.day ? g.day : 0) + (row.unread ? g.unread : 0) +
                         g.groupGap(entry.presence, row.grouped) +
                         (!collapsed && g.authorHeader(row.grouped, entry.presence, row.showClock)
                              ? (g.bubbles ? g.nickHeader : g.metadataHeader)
                              : 0) +
                         (entry.reply.empty() ? 0 : g.quote) +
                         (buffer.kind == "mentions" ? g.metadataHeader : 0);
        row.height =
            row.bodyOffset + row.bodyHeight +
            (g.footerVisible(entry) ? g.footerExtent(entry, bodyWidth) + g.footerGap() : 0) +
            paddingY + g.rowGap(entry.presence, row.grouped);
        if (!entry.reactions.empty())
            row.height += g.reaction;
    }
    Row estimate(const Buffer &buffer, size_t index, const Geometry &g, float width) {
        Row row;
        row.key = ui_logic::messageKey(buffer.messages[index]);
        row.id = buffer.messages[index].id;
        row.focusIdentity = nextFocusMessageIdentity++;
        rowState(row, buffer, index);
        estimateHeight(row, buffer, index, g, width);
        return row;
    }
    void projectActivities(const Buffer &buffer, const Geometry &g, float width, size_t first,
                           bool reflow, size_t appendFrom = absent) {
        auto &c = *cache;
        const auto mode = effectivePresenceMode(model.settings, buffer.id, buffer.target);
        if (first == 0 || c.smartEligible.size() > buffer.messages.size()) {
            c.smartEligible.clear();
            c.smartActivity.clear();
        }
        auto &smartEligible = c.smartEligible;
        if (mode == PresenceMode::Smart) {
            const auto network = model.networks.find(buffer.server);
            const auto mapping =
                network == model.networks.end() ? "rfc1459" : network->second.caseMapping;
            const size_t processed = smartEligible.size();
            smartEligible.resize(buffer.messages.size());
            for (size_t i = processed; i < buffer.messages.size(); ++i) {
                const auto &entry = buffer.messages[i];
                auto &times = c.smartActivity[irc::casefold(entry.nick, mapping)];
                if (!entry.presence && !entry.deleted) {
                    times.push_back(entry.timestamp);
                    if (times.size() > 5)
                        times.pop_front();
                }
                size_t recent = 0;
                bool live = false;
                for (const auto at : times)
                    if (at <= entry.timestamp) {
                        live = live || entry.timestamp - at <= 300;
                        recent += entry.timestamp - at <= 7 * 86400;
                    }
                smartEligible[i] = entry.own || live || recent >= 5;
                if (entry.activity == ActivityKind::Nick && !entry.activityTarget.empty()) {
                    const auto old = irc::casefold(entry.nick, mapping);
                    const auto replacement = irc::casefold(entry.activityTarget, mapping);
                    if (old != replacement) {
                        auto &destination = c.smartActivity[replacement];
                        destination.insert(destination.end(), times.begin(), times.end());
                        std::sort(destination.begin(), destination.end());
                        while (destination.size() > 5)
                            destination.pop_front();
                        c.smartActivity.erase(old);
                    }
                }
            }
        }
        first = std::min(first, c.rows.size());
        if (first == c.rows.size()) {
            c.projectedPresence = mode;
            c.projectionFrom = absent;
            return;
        }
        size_t previous =
            first ? (c.rows[first].projected ? c.rows[first].previousVisible : first - 1) : absent;
        bool unreadPending = c.divider != absent && c.divider < first &&
                             (previous == absent || previous < c.divider);
        auto project = [&](size_t index, bool hidden, bool summary, bool expanded, size_t begin,
                           size_t end, std::string_view label) {
            auto &row = c.rows[index];
            const auto &entry = buffer.messages[index];
            const size_t oldPrevious = row.projected ? row.previousVisible
                                       : index       ? index - 1
                                                     : absent;
            const bool unread = index == c.divider || unreadPending;
            const bool muted = !entry.own && !model.settings.foolUnread && foolish(buffer, entry);
            bool reshape = row.hiddenActivity != hidden || row.activitySummary != summary ||
                           row.activityExpanded != expanded || row.activityLabel != label ||
                           oldPrevious != previous ||
                           row.unread != (hidden || muted ? false : unread) ||
                           (reflow && typedActivity(entry));
            const double oldHeight = row.height;
            const bool oldClock = row.showClock;
            row.hiddenActivity = hidden;
            row.activitySummary = summary;
            row.activityExpanded = expanded;
            row.activityBegin = begin;
            row.activityEnd = end;
            row.previousVisible = previous;
            row.projected = true;
            row.unread = hidden || muted ? false : unread;
            row.activityLabel = label;
            if (mediaController && !readable(buffer, index))
                mediaController->bind(row.focusIdentity, "", false);
            if (hidden) {
                row.day = row.grouped = false;
                unreadPending = unread;
                row.height = row.bodyHeight = row.bodyOffset = row.footerHeight = 0;
            } else {
                rowState(row, buffer, index);
                reshape = reshape || oldClock != row.showClock;
                if (reshape)
                    estimateHeight(row, buffer, index, g, width);
                previous = index;
                unreadPending = muted ? unread : false;
            }
            if (reshape || hidden) {
                row.measured = false;
                row.fragmentsReady = false;
                row.fragments.clear();
                row.lines.clear();
                row.selectionText.reset();
                row.selectionGeometryReady = false;
                row.reactions.clear();
            }
            if (row.height != oldHeight)
                c.heights.add(index, row.height - oldHeight);
        };
        for (size_t index = first; index < c.rows.size();) {
            const auto &entry = buffer.messages[index];
            if (!sourceContext(buffer, entry)) {
                project(index, true, false, false, index, index + 1, {});
                ++index;
                continue;
            }
            const auto policy = foolPolicy(buffer, entry);
            if (buffer.kind == "mentions" && foolish(buffer, entry) &&
                policy != FoolDisplay::Shown && policy != FoolDisplay::Dimmed) {
                project(index, true, false, false, index, index + 1, {});
                ++index;
                continue;
            }
            if (foolish(buffer, entry) && policy != FoolDisplay::Shown) {
                size_t end = index + 1;
                const bool minimized = policy == FoolDisplay::Minimized;
                if (minimized)
                    while (end < c.rows.size() && foolish(buffer, buffer.messages[end]) &&
                           foolPolicy(buffer, buffer.messages[end]) == policy &&
                           sourceContext(buffer, buffer.messages[end]) ==
                               sourceContext(buffer, entry))
                        ++end;
                bool open = false;
                for (size_t i = index; i < end; ++i)
                    open = open || c.rows[i].foolOpen;
                project(index, policy == FoolDisplay::Hidden, minimized, open, index, end,
                        minimized ? std::to_string(end - index) + " minimized messages" : "");
                for (size_t i = index + 1; i < end; ++i)
                    project(i, !open, false, open, index, end, {});
                index = end;
                continue;
            }
            if (typedActivity(entry) && mode == PresenceMode::Smart && !smartEligible[index]) {
                project(index, true, false, false, index, index + 1, {});
                ++index;
                continue;
            }
            if (buffer.kind != "channel" || !typedActivity(entry)) {
                project(index, typedActivity(entry) && mode == PresenceMode::Hidden, false, false,
                        absent, absent, {});
                ++index;
                continue;
            }
            const size_t retainedEnd = appendFrom != absent &&
                                               c.rows[index].activityBegin == index &&
                                               c.rows[index].activityEnd == appendFrom
                                           ? appendFrom
                                           : index + 1;
            size_t end = retainedEnd;
            const auto day = localDate(entry.timestamp, "%Y-%m-%d");
            while (end < c.rows.size() && end != c.divider && typedActivity(buffer.messages[end]) &&
                   !foolish(buffer, buffer.messages[end]) &&
                   (mode != PresenceMode::Smart || smartEligible[end]) &&
                   localDate(buffer.messages[end].timestamp, "%Y-%m-%d") == day)
                ++end;
            const bool summary = mode == PresenceMode::Smart && end - index > 1;
            bool expanded = false;
            if (summary) {
                if (retainedEnd > index + 1)
                    expanded = c.rows[index].activityExpanded;
                for (size_t i = retainedEnd > index + 1 ? retainedEnd : index; i < end; ++i)
                    expanded = expanded || c.rows[i].activityOpen;
            }
            const auto label = summary ? std::to_string(end - index) + " channel events" : "";
            project(index, mode == PresenceMode::Hidden, summary, expanded, index, end, label);
            if (retainedEnd > index + 1) {
                if (mode == PresenceMode::All || expanded) {
                    previous = retainedEnd - 1;
                    unreadPending = false;
                } else if (mode == PresenceMode::Hidden && c.divider >= index &&
                           c.divider < retainedEnd)
                    unreadPending = true;
            }
            for (size_t i = retainedEnd; i < end; ++i)
                project(i, mode == PresenceMode::Hidden || (summary && !expanded), false, expanded,
                        index, end, {});
            index = end;
        }
        c.projectedPresence = mode;
        c.projectionFrom = absent;
    }
    void toggleActivity(uint64_t identity) {
        const size_t index = identityIndex(identity);
        if (!cache || index >= cache->rows.size() || !cache->rows[index].activitySummary)
            return;
        const auto &row = cache->rows[index];
        const bool open = !row.activityExpanded;
        for (size_t i = row.activityBegin; i < row.activityEnd; ++i) {
            cache->rows[i].activityOpen = open;
            cache->rows[i].foolOpen = open;
        }
        cache->projectionFrom = std::min(cache->projectionFrom, row.activityBegin);
        pane.following = false;
        view.markLayoutDirty();
    }
    void rebuild(const Buffer &buffer, const Geometry &g, float width,
                 bool classifyArrivals = true) {
        auto &c = *cache;
        const size_t previousCount = c.rows.size();
        const bool markerChanged = c.marker != buffer.readMarker ||
                                   c.windowActive != windowActive || c.unreadCount != buffer.unread;
        const auto mode = effectivePresenceMode(model.settings, buffer.id, buffer.target);
        std::string foolPolicy = privacyPolicy(buffer);
        const bool policyChanged = c.foolPolicy != foolPolicy;
        if (policyChanged) {
            c.foolPolicy = std::move(foolPolicy);
            c.revealedFoolList = model.settings.fools;
            c.revealedBufferFools = model.settings.bufferFools;
            c.revealedFoolDisplay = model.settings.foolDisplay;
            const auto network = model.networks.find(buffer.server);
            c.revealedFoolMapping =
                network == model.networks.end() ? "rfc1459" : network->second.caseMapping;
            for (auto &row : c.rows)
                row.foolOpen = row.activityOpen = false;
            cancelFocusReveal();
            navigationRevealIdentity = selectedIdentity = 0;
            selectedIndexHint = absent;
            clearTextSelection();
            closePopup();
        }
        const bool participationChanged =
            mode == PresenceMode::Smart && c.participationRevision != buffer.participationRevision;
        const bool modeChanged =
            c.projectedPresence != mode || policyChanged || participationChanged;
        const bool reflow = c.width != width || c.geometry.key != g.key || policyChanged;
        if (reflow) {
            const float widthRatio =
                c.width > 1 ? std::clamp(c.width / std::max(1.f, width), .25f, 4.f) : 1;
            const float fontRatio = c.geometry.font > 0 ? g.font / c.geometry.font : 1;
            c.heights.clear();
            for (auto &row : c.rows) {
                row.height = std::max(double(24 * g.scale), row.height * widthRatio * fontRatio);
                row.measured = false;
                row.fragments.clear();
                row.lines.clear();
                c.heights.append(row.height);
            }
            c.width = width;
            c.geometry = g;
            ++c.revision;
        }
        // A stable identity epoch permits tail growth without rebuilding existing rows.
        const bool identityChanged = c.messagesRevision != buffer.messagesRevision;
        bool appendOnly = !identityChanged && c.rows.size() <= buffer.messages.size();
        if (appendOnly && !c.rows.empty())
            appendOnly =
                c.rows.front().key == ui_logic::messageKey(buffer.messages.front()) &&
                c.rows.back().key == ui_logic::messageKey(buffer.messages[c.rows.size() - 1]);
        const bool changed =
            identityChanged || c.rows.size() != buffer.messages.size() ||
            (!c.rows.empty() &&
             (c.rows.front().key != ui_logic::messageKey(buffer.messages.front()) ||
              c.rows.back().key != ui_logic::messageKey(buffer.messages.back())));
        std::string newest;
        if (changed && !buffer.messages.empty())
            newest = ui_logic::messageKey(buffer.messages.back());
        const bool unsettled = focusRevealTarget || c.pendingArrivals > 0;
        const bool observeArrivals = classifyArrivals && !newest.empty() && !c.newest.empty() &&
                                     newest != c.newest && (!pane.following || unsettled);
        auto observeArrival = [&](size_t index, Row &row) {
            const auto &entry = buffer.messages[index];
            // Restored unread survives hydration; later history replay is not a new arrival.
            row.unreadEligible =
                (buffer.unread > 0 || (!model.settings.foolUnread && foolish(buffer, entry))) &&
                (!entry.history || (!c.initialized && entry.arrival == 0));
            // Only newly observed live entries animate; retained row identity owns the lifetime.
            if (classifyArrivals && !switched && pane.following && effectivelyVisible() &&
                c.initialized && !entry.history && !entry.presence && entry.arrival > 0 &&
                model.now >= entry.arrival &&
                model.now - entry.arrival <
                    (entry.own ? fx::outgoingDuration
                               : fx::messageIncomingDuration(model.settings.incomingDurationMs)) &&
                !foolish(buffer, entry) && !model.settings.reducedMotion &&
                model.settings.fx != FxLevel::Off) {
                row.entranceStarted = model.now;
                c.arrivals.push_back(index);
            }
            if (observeArrivals && !entry.own && !entry.history && !entry.pending &&
                !entry.presence && (model.settings.foolUnread || !foolish(buffer, entry))) {
                if (unsettled)
                    ++c.pendingArrivals;
                else
                    ++pane.unseen;
            }
        };
        if (changed && !appendOnly) {
            std::vector<Row> rows;
            rows.reserve(buffer.messages.size());
            for (size_t i = 0; i < buffer.messages.size(); ++i) {
                auto key = ui_logic::messageKey(buffer.messages[i]);
                size_t existing = absent;
                if (const auto found = c.keys.find(key); found != c.keys.end() &&
                                                         found->second < c.rows.size() &&
                                                         c.rows[found->second].focusIdentity)
                    existing = found->second;
                if (existing == absent && !buffer.messages[i].id.empty())
                    if (const auto found = c.ids.find(buffer.messages[i].id);
                        found != c.ids.end() && found->second < c.rows.size() &&
                        c.rows[found->second].id == buffer.messages[i].id &&
                        c.rows[found->second].focusIdentity)
                        existing = found->second;
                if (existing == absent) {
                    rows.push_back(estimate(buffer, i, g, width));
                    observeArrival(i, rows.back());
                } else {
                    rows.push_back(std::move(c.rows[existing]));
                    c.rows[existing].focusIdentity = 0;
                    rows.back().key = std::move(key);
                    rows.back().measured = false;
                    rows.back().fragments.clear();
                    rows.back().lines.clear();
                }
                rows.back().id = buffer.messages[i].id;
            }
            if (mediaController)
                for (const auto &retired : c.rows)
                    if (retired.focusIdentity)
                        mediaController->bind(retired.focusIdentity, "", false);
            c.rows = std::move(rows);
            c.keys.clear();
            c.ids.clear();
            c.heights.clear();
            for (size_t i = 0; i < c.rows.size(); ++i) {
                if (c.rows[i].unread) {
                    c.rows[i].height -= g.unread;
                    c.rows[i].unread = false;
                }
                c.keys.emplace(c.rows[i].key, i);
                c.ids[c.rows[i].id] = i;
                c.heights.append(c.rows[i].height);
            }
            c.materialized.clear();
            c.divider = absent;
            c.marker.clear();
            ++c.revision;
        } else if (changed) {
            c.keys.reserve(buffer.messages.size());
            c.ids.reserve(buffer.messages.size());
            for (size_t i = c.rows.size(); i < buffer.messages.size(); ++i) {
                c.rows.push_back(estimate(buffer, i, g, width));
                observeArrival(i, c.rows.back());
                c.keys.emplace(c.rows.back().key, i);
                c.ids[c.rows.back().id] = i;
                c.heights.append(c.rows.back().height);
            }
            ++c.revision;
        }
        if (!newest.empty())
            c.newest = std::move(newest);
        c.messagesRevision = buffer.messagesRevision;
        if (changed || markerChanged || policyChanged) {
            const auto oldDivider = c.divider;
            if (c.unreadCount > 0 && buffer.unread == 0 &&
                (!policyChanged || c.marker != buffer.readMarker))
                for (auto &row : c.rows)
                    row.unreadEligible = false;
            c.marker = buffer.readMarker;
            c.windowActive = windowActive;
            c.unreadCount = buffer.unread;
            c.divider = absent;
            if (!windowActive && buffer.unread > 0) {
                for (size_t n = buffer.messages.size(); n > 0; --n)
                    if (buffer.messages[n - 1].id == c.marker ||
                        (c.marker.starts_with("local:") &&
                         std::string_view(c.marker).substr(6) == buffer.messages[n - 1].id) ||
                        (!c.marker.empty() && buffer.messages[n - 1].time == c.marker)) {
                        c.divider = n;
                        break;
                    }
                // Unread counts exclude own messages and activity, not simply tail rows.
                // They also keep a stale durable marker from including already-read rows.
                size_t counted = absent;
                int remaining = buffer.unread;
                for (size_t n = buffer.messages.size(); n > 0; --n) {
                    const auto &entry = buffer.messages[n - 1];
                    if (!entry.own && !entry.presence &&
                        (model.settings.foolUnread || !foolish(buffer, entry))) {
                        counted = n - 1;
                        if (--remaining == 0)
                            break;
                    }
                }
                if (c.divider == absent || (counted != absent && counted > c.divider))
                    c.divider = counted;
                while (c.divider < buffer.messages.size()) {
                    const auto &entry = buffer.messages[c.divider];
                    if (!entry.own && !entry.pending && !entry.presence &&
                        c.rows[c.divider].unreadEligible &&
                        (model.settings.foolUnread || !foolish(buffer, entry)))
                        break;
                    ++c.divider;
                }
                if (c.divider == buffer.messages.size())
                    c.divider = absent;
            }
            for (const auto index : {oldDivider, c.divider})
                if (index < c.rows.size()) {
                    auto &row = c.rows[index];
                    const bool unread = index == c.divider;
                    if (row.unread != unread) {
                        const double delta = (unread ? 1 : -1) * g.unread;
                        row.height += delta;
                        c.heights.add(index, delta);
                        row.unread = unread;
                        row.measured = false;
                    }
                }
        }
        if (changed || reflow || markerChanged || modeChanged || c.projectionFrom != absent) {
            size_t first = 0;
            if (!reflow && !markerChanged && !modeChanged && appendOnly) {
                first = c.projectionFrom;
                if (changed && previousCount) {
                    const auto &tail = c.rows[previousCount - 1];
                    first = std::min(first, tail.activityBegin == absent ? previousCount - 1
                                                                         : tail.activityBegin);
                }
                if (first == absent)
                    first = 0;
            }
            const size_t appendFrom = changed && appendOnly && !reflow && !markerChanged &&
                                              !modeChanged && c.projectionFrom == absent
                                          ? previousCount
                                          : absent;
            projectActivities(buffer, g, width, first, reflow, appendFrom);
        }
        c.initialized = true;
        c.participationRevision = buffer.participationRevision;
    }
    void reconcileBuffer(Cache &baseline, const Buffer &buffer, bool classifyArrivals) {
        const size_t anchor =
            baseline.heights.at(std::max(0., double(baseline.scroll - baseline.header)));
        uint64_t anchorIdentity = 0;
        double oldBodyTop = 0, oldBodyHeight = 0;
        if (!baseline.following && anchor < baseline.rows.size()) {
            anchorIdentity = baseline.rows[anchor].focusIdentity;
            oldBodyTop = baseline.header + baseline.heights.prefix(anchor) +
                         baseline.rows[anchor].bodyOffset;
            oldBodyHeight = baseline.rows[anchor].bodyHeight;
        }
        auto *activeCache = std::exchange(cache, &baseline);
        rebuild(buffer, baseline.geometry, baseline.width, classifyArrivals);
        cache = activeCache;
        if (anchorIdentity) {
            size_t relocated = anchor;
            if (relocated >= baseline.rows.size() ||
                baseline.rows[relocated].focusIdentity != anchorIdentity)
                for (relocated = 0; relocated < baseline.rows.size(); ++relocated)
                    if (baseline.rows[relocated].focusIdentity == anchorIdentity)
                        break;
            if (relocated < baseline.rows.size())
                baseline.scroll = ui_logic::anchoredBodyScroll(
                    baseline.scroll, oldBodyTop,
                    baseline.header + baseline.heights.prefix(relocated) +
                        baseline.rows[relocated].bodyOffset,
                    oldBodyHeight, baseline.rows[relocated].bodyHeight, false);
        }
    }

    double measure(Renderer &renderer, const Buffer &buffer, size_t index) {
        auto &c = *cache;
        auto &row = c.rows[index];
        if (row.hiddenActivity)
            return 0;
        c.prepared.push_back(index);
        const auto &entry = buffer.messages[index];
        const bool emote = entry.action && !entry.presence;
        const bool notice = typedActivity(entry);
        const bool collapsed = row.activitySummary && !row.activityExpanded;
        const auto &g = c.geometry;
        size_t fingerprint = signature(entry, index ? &buffer.messages[index - 1] : nullptr);
        fingerprint ^= size_t(timestampVisible(row, buffer, index)) << 17;
        const bool friendAuthor = this->friendAuthor(buffer, entry) && readable(buffer, index);
        fingerprint ^= size_t(friendAuthor) << 18;
        if (collapsed)
            fingerprint ^= std::hash<std::string>{}(row.activityLabel);
        if (entry.text.find('\x16') != std::string::npos)
            fingerprint ^= themeRevision();
        if (const auto *quoted = quotedMessage(buffer, entry)) {
            fingerprint ^= std::hash<std::string>{}(quoted->text) +
                           std::hash<std::string>{}(quoted->nick) + size_t(quoted->deleted);
            fingerprint ^= size_t(quoteReadable(buffer, entry, *quoted)) << 10;
        }
        if (row.measured && row.signature == fingerprint && row.fragmentsReady)
            return 0;
        const double oldHeight = row.height;
        row.signature = fingerprint;
        row.id = entry.id;
        c.ids[row.id] = index;
        rowState(row, buffer, index);
        row.friendAuthor = friendAuthor;
        const auto &metrics = g.metrics;
        const float padding = notice ? g.activityPadding() : g.padding(entry.presence);
        const float paddingY = notice ? g.activityPaddingY() : g.paddingY(entry.presence);
        row.prefixWidth = collapsed ? 0 : g.prefixWidth(entry.presence);
        const float availableBody = activityBodyWidth(row, entry, g, c.width);
        auto emoteFont = themeMessageFont();
        emoteFont.weight = FontWeight::Bold;
        const float friendExtent = row.friendAuthor ? 18 * g.scale : 0;
        row.emotePrefix =
            emote && g.bubbles
                ? g.authorAvatarInset() +
                      std::min(
                          std::max(0.f, availableBody - g.authorAvatarInset()) * .45f,
                          std::ceil(
                              renderer.measureText("↳ " + entry.nick, g.font, emoteFont).width) +
                              friendExtent + 8 * g.scale)
                : 0;
        row.prefixWidth += row.emotePrefix;
        const float bodyMax = std::max(1.f, availableBody - row.emotePrefix);
        const Color normal =
            entry.deleted || entry.presence    ? themeForegroundColor("on_surface_variant")
            : entry.own && g.bubbles && !emote ? themeForegroundColor("on_primary_container")
                                               : themeForegroundColor("on_surface");
        auto document = parseIrcFormatting(collapsed       ? std::string_view(row.activityLabel)
                                           : entry.deleted ? std::string_view("Message deleted")
                                                           : entry.text,
                                           normal, themeForegroundColor("surface_container"));
        if (notice)
            for (auto &run : document.runs)
                run.style.monospace = true;
        if (emote)
            for (auto &run : document.runs)
                run.style.italic = true;
        auto layout =
            wrap(renderer, document, notice ? g.activityFont : g.font, bodyMax,
                 notice ? g.activityLine : g.bodyLine, model.settings.messageWrapColumns, model,
                 sourceContext(buffer, entry) ? *sourceContext(buffer, entry) : buffer, entry.nick);
        const auto previewLinks = detectLinks(document);
        row.previewUrl = previewLinks.empty() ? std::string{} : previewLinks.front().url;
        if (mediaController)
            mediaController->bind(row.focusIdentity, row.previewUrl,
                                  readable(buffer, index) && !entry.presence && !entry.deleted,
                                  !foolish(buffer, entry));
        row.bodyHeight = layout.height;
        row.fragmentsReady = true;
        row.selectionGeometryReady = false;
        row.quoteAuthor.clear();
        row.quoteMarkup.clear();
        row.quoteHeight = row.quoteAuthorHeight = row.quoteExcerptHeight = 0;
        if (readable(buffer, index) && !entry.reply.empty()) {
            const Message *parent = quotedMessage(buffer, entry);
            const bool exposed = parent && quoteReadable(buffer, entry, *parent);
            row.quoteAuthor = exposed ? parent->nick : "Reply";
            row.quoteColor = exposed ? nickColor(model, parent->nick, sourceServer(buffer, entry))
                                     : themeForegroundColor("on_surface_variant");
            auto document = parseIrcFormatting(
                exposed                     ? std::string_view(parent->text)
                : parent && parent->deleted ? std::string_view("Message deleted")
                : parent                    ? std::string_view("Message hidden")
                                            : std::string_view("Message unavailable"),
                themeForegroundColor("on_surface"), themeSurfaceColor("surface_container"));
            ircSingleParagraph(document);
            for (const auto &run : document.runs)
                row.quoteMarkup += ircMarkup(
                    std::string_view(document.text).substr(run.start, run.end - run.start),
                    run.style);
        }
        row.contentWidth = bodyMax;
        row.authorWidth = row.authorHeight = 0;
        row.authorHeader = g.nickHeader;
        if (g.bubbles && !row.grouped && !entry.presence) {
            const auto author = renderer.measureText(entry.nick, g.nickFont, g.nickStyle);
            // Match the native ink-centered Label's whole-pixel line box.
            row.authorWidth = std::ceil(author.width) + 2 * g.chipPadding() + friendExtent;
            row.authorHeight = std::max(
                g.nickLine,
                std::round(std::max(author.bottom - author.top, author.inkBottom - author.inkTop)));
            row.authorHeader =
                std::max(g.nickHeader, row.authorHeight + 2 * g.chipPaddingY() + 3 * g.scale);
        }
        row.footerText = g.bubbles && !entry.presence && g.timestamps ? clockText(entry) : "";
        const bool status = g.bubbles && !entry.presence && sendIndicator(entry);
        const float statusSize = status ? g.statusSize() : 0;
        const float naturalFooter = g.footerVisible(entry) ? g.footerWidth(entry) : 0;
        const bool fitBubble = g.bubbles;
        row.reactions.clear();
        float reactionWidth = 0;
        const float reactionLimit =
            std::max(0.f, std::min(bodyMax, c.width - 2 * metrics.inset * g.scale -
                                                g.avatarLane(entry.presence) - 2 * padding -
                                                row.prefixWidth));
        if (readable(buffer, index))
            for (const auto &[emoji, nicks] : entry.reactions) {
                const auto label = emoji + " " + std::to_string(nicks.size());
                const float width =
                    renderer.measureText(label, g.metadataFont, themeUiFont()).width + 18 * g.scale;
                const float rx = row.reactions.empty() ? 0 : reactionWidth + 5 * g.scale;
                // Keep only complete chips in the first row; hidden chips have no native target.
                if (rx + width > reactionLimit)
                    break;
                std::string users;
                for (const auto &nick : nicks) {
                    if (!users.empty())
                        users += ", ";
                    users += nick;
                }
                row.reactions.push_back({emoji, label, std::move(users), rx, 0, width});
                reactionWidth = rx + width;
            }
        if (entry.presence)
            row.contentWidth = std::min(bodyMax, std::max({1.f, layout.width, reactionWidth}));
        else if (fitBubble) {
            float minimum = std::max(layout.width, naturalFooter);
            if (!row.grouped && !emote)
                minimum = std::max(minimum, row.authorWidth + g.authorAvatarInset());
            if (!row.quoteAuthor.empty()) {
                auto authorFont = themeUiFont();
                authorFont.weight = FontWeight::Bold;
                const float excerptWidth =
                    renderer
                        .measureText(row.quoteMarkup, g.metadataFont, themeUiFont(),
                                     std::max(1.f, bodyMax - 24 * g.scale), 2, TextAlign::Start,
                                     TextEllipsize::End, true)
                        .width;
                minimum = std::max(
                    minimum,
                    std::min(
                        bodyMax,
                        std::max(excerptWidth,
                                 renderer.measureText(row.quoteAuthor, g.metadataFont, authorFont)
                                     .width) +
                            24 * g.scale));
            }
            minimum = std::max(minimum, reactionWidth);
            row.contentWidth = std::min(bodyMax, std::max(1.f, minimum));
        }
        if (mediaController && !row.previewUrl.empty() && readable(buffer, index))
            row.contentWidth = std::min(bodyMax, std::max(row.contentWidth, 320 * g.scale));
        row.footerStacked = !row.footerText.empty() && status && naturalFooter > row.contentWidth;
        row.clockWidth = row.footerText.empty() ? 0 : std::min(g.clockWidth, row.contentWidth);
        TextMetrics footerMetrics{};
        if (!row.footerText.empty())
            footerMetrics =
                renderer.measureText(row.footerText, g.timeFont, themeUiFont(), row.clockWidth, 0,
                                     TextAlign::End, TextEllipsize::End, false);
        row.footerAscent = -footerMetrics.top;
        row.clockHeight = footerMetrics.bottom - footerMetrics.top;
        row.footerWidth =
            row.footerStacked
                ? std::max(row.clockWidth, statusSize)
                : row.clockWidth + statusSize + (row.clockWidth > 0 && status ? g.statusGap() : 0);
        row.footerHeight = row.footerStacked ? row.clockHeight + g.statusGap() + statusSize
                                             : std::max(row.clockHeight, statusSize);
        row.fragments = std::move(layout.fragments);
        row.lines = std::move(layout.lines);
        row.surfaceWidth = row.contentWidth + row.prefixWidth + 2 * padding +
                           (row.activitySummary ? g.activityChevron() : 0);
        if (!row.quoteAuthor.empty()) {
            const float quoteWidth = std::max(1.f, row.contentWidth - 24 * g.scale);
            auto authorFont = themeUiFont();
            authorFont.weight = FontWeight::Bold;
            row.quoteAuthorHeight =
                renderer.measureText(row.quoteAuthor, g.metadataFont, authorFont, quoteWidth, 1)
                    .layoutHeight;
            row.quoteExcerptHeight =
                renderer
                    .measureText(row.quoteMarkup, g.metadataFont, themeUiFont(), quoteWidth, 2,
                                 TextAlign::Start, TextEllipsize::End, true)
                    .layoutHeight;
            row.quoteHeight = row.quoteAuthorHeight + row.quoteExcerptHeight + 16 * g.scale;
        }
        row.bodyOffset = paddingY + (row.day ? g.day : 0) + (row.unread ? g.unread : 0) +
                         g.groupGap(entry.presence, row.grouped);
        row.prefixBaseline = row.prefixAscent = row.prefixHeight = 0;
        if (!collapsed && ((g.channel && g.nickVisible(row.grouped, entry.presence)) ||
                           (g.channel && !entry.presence && row.showClock) ||
                           (entry.presence && row.showClock))) {
            float descent = 0;
            auto include = [&](std::string_view text, float width, TextAlign align) {
                const auto measured =
                    renderer.measureText(text, notice ? g.activityFont : g.metadataFont,
                                         notice ? g.activityStyle : themeUiFont(),
                                         std::max(1.f, width), 1, align, TextEllipsize::End);
                row.prefixAscent = std::max(row.prefixAscent, -measured.top);
                descent = std::max(descent, measured.bottom);
            };
            if (g.nickVisible(row.grouped, entry.presence)) {
                const auto align = g.inlineChannel ? TextAlign::End : TextAlign::Start;
                const float nickWidth =
                    std::max(1.f, g.nickGutter - g.compactAvatarInset() - friendExtent -
                                      (entry.own && !entry.action ? 2 * g.chipPadding() : 0));
                if (entry.action)
                    include("↳ " + entry.nick, nickWidth, align);
                else
                    include(entry.nick, nickWidth, align);
            }
            if (row.showClock)
                include(clockText(entry), g.timeGutter - 4 * g.scale, TextAlign::Start);
            const float textHeight = row.prefixAscent + descent;
            row.prefixHeight = std::max(entry.presence ? 0.f : g.metadataHeader, textHeight);
            if (g.nickVisible(row.grouped, entry.presence))
                row.prefixHeight =
                    std::max(row.prefixHeight,
                             std::max(g.compactAvatarSize(),
                                      g.metadataHeader + (entry.own ? 2 * g.chipPaddingY() : 0)));
            row.prefixAscent += (row.prefixHeight - textHeight) * .5f;
            row.prefixBaseline = row.bodyOffset + row.prefixAscent;
        }
        if (!collapsed && (!emote || g.channel) &&
            g.authorHeader(row.grouped, entry.presence, row.showClock))
            row.bodyOffset += g.channel ? row.prefixHeight : row.authorHeader;
        row.bodyOffset += row.quoteHeight;
        if (buffer.kind == "mentions")
            row.bodyOffset += g.metadataHeader;
        if (!collapsed &&
            ((g.channel && g.inlineChannel && !entry.presence && row.prefixHeight > 0) ||
             (entry.presence && row.showClock))) {
            const float baseline = row.fragments.empty()
                                       ? -renderer.measureFont(g.font, themeMessageFont()).top
                                       : row.fragments.front().y + row.fragments.front().baseline;
            row.bodyOffset += std::max(0.f, row.prefixAscent - baseline);
            row.prefixBaseline = row.bodyOffset + baseline;
        }
        float bodyEnd = row.bodyOffset + row.bodyHeight;
        if (!collapsed && ((g.channel && !entry.presence) || (entry.presence && row.showClock)))
            bodyEnd = std::max(bodyEnd, row.prefixBaseline - row.prefixAscent + row.prefixHeight);
        row.selectionWidth = row.contentWidth;
        if (!g.bubbles && !entry.presence) {
            row.selectionWidth = layout.width;
            if (row.quoteHeight > 0 || !row.previewUrl.empty() || !entry.reactions.empty())
                row.selectionWidth = row.contentWidth;
            row.selectionWidth += row.prefixWidth;
            if (g.channel && !g.inlineChannel) {
                float metadataWidth = g.timestamps && row.showClock ? g.timeGutter : 0;
                if (g.nickVisible(row.grouped, entry.presence)) {
                    const float chipPadding = entry.own && !entry.action ? g.chipPadding() : 0;
                    const float adornments =
                        g.compactAvatarInset() + 2 * chipPadding + friendExtent;
                    const float textLimit = std::max(1.f, g.nickGutter - adornments);
                    const auto text = entry.action ? "↳ " + entry.nick : entry.nick;
                    metadataWidth = std::max(
                        metadataWidth,
                        g.timeGutter + adornments +
                            std::min(
                                textLimit,
                                std::ceil(renderer.measureText(text, g.metadataFont, themeUiFont())
                                              .width)));
                }
                row.selectionWidth = std::max(row.selectionWidth, metadataWidth);
            }
            row.selectionWidth =
                std::min(row.surfaceWidth, std::max(1.f, row.selectionWidth + 2 * padding));
        }
        row.previewHeight =
            mediaController && readable(buffer, index)
                ? mediaController->measure(renderer, row.focusIdentity, row.contentWidth, g.scale)
                : 0;
        const auto firstText = document.text.find_first_not_of(" \t\r\n");
        const auto lastText = document.text.find_last_not_of(" \t\r\n");
        row.urlReplaced =
            row.previewHeight > 0 && mediaController &&
            mediaController->replacesUrl(row.focusIdentity) && firstText != std::string::npos &&
            std::string_view(document.text).substr(firstText, lastText - firstText + 1) ==
                row.previewUrl;
        if (row.urlReplaced) {
            row.bodyHeight = 0;
            bodyEnd =
                std::max(row.bodyOffset, row.prefixBaseline - row.prefixAscent + row.prefixHeight);
        }
        row.previewOffset = bodyEnd + (row.previewHeight > 0 && !row.urlReplaced ? 6 * g.scale : 0);
        if (row.previewHeight > 0)
            bodyEnd = row.previewOffset + row.previewHeight;
        for (auto &reaction : row.reactions)
            reaction.y = bodyEnd;
        if (!row.reactions.empty())
            bodyEnd += g.reaction;
        row.contentEnd = bodyEnd;
        row.height =
            bodyEnd + (g.footerVisible(entry) ? row.footerHeight + g.footerGap() : 0) + paddingY;
        if (g.bubbles && g.avatars && !row.grouped && !entry.presence)
            row.height =
                std::max(row.height, double((row.day ? g.day : 0) + (row.unread ? g.unread : 0) +
                                            g.groupGap(entry.presence, row.grouped) +
                                            metrics.avatarSize * g.scale));
        if (g.footerVisible(entry))
            row.footerOffset = float(row.height) - paddingY - row.footerHeight;
        row.height += g.rowGap(entry.presence, row.grouped);
        row.measured = true;
        row.revision = ++c.revision;
        c.heights.add(index, row.height - oldHeight);
        return row.height - oldHeight;
    }

    Glyph *friendGlyph(Node &parent, float x, float y, float size, Color color) {
        auto hit = std::make_unique<InputArea>();
        hit->setPosition(x, y);
        hit->setSize(size, size);
        hit->setAcceptedButtons(0);
        hit->setTooltip("Friend");
        auto glyph = std::make_unique<Glyph>();
        auto *result = glyph.get();
        glyph->setGlyph("star");
        glyph->setGlyphSize(size);
        glyph->setColor(color);
        glyph->setSize(size, size);
        glyph->setHitTestVisible(false);
        hit->addChild(std::move(glyph));
        parent.addChild(std::move(hit));
        return result;
    }
    Label *label(Node &parent, std::string_view text, float x, float y, float width, float size,
                 Color color, const TextFont &font, std::string tooltip = {},
                 std::optional<ColorRole> role = {}) {
        auto child = std::make_unique<Label>();
        auto *node = child.get();
        node->setPassiveTouchPress(true);
        node->setFocusOnPointerPress(false);
        node->setText(text);
        node->setFontSize(size);
        node->setFontFamily(font.family);
        node->setFontFeatures(font.features);
        node->setFontWeight(font.weight);
        node->setMaxLines(1);
        node->setMaxWidth(std::max(1.f, width));
        if (role)
            node->setColor(colorSpecFromRole(*role));
        else
            node->setColor(color);
        node->setPosition(x, y);
        if (!tooltip.empty())
            node->setTooltip(std::move(tooltip));
        parent.addChild(std::move(child));
        return node;
    }
    RectNode *rectangle(Node &parent, float x, float y, float width, float height, Color color,
                        float radius = 0) {
        auto node = std::make_unique<RectNode>();
        node->setPosition(x, y);
        node->setSize(width, height);
        RoundedRectStyle style{};
        style.fill = color;
        style.radius = radius;
        node->setStyle(style);
        node->setHitTestVisible(false);
        auto *observer = node.get();
        parent.addChild(std::move(node));
        return observer;
    }
    void avatar(Renderer &renderer, Slot &slot, const Message &entry, float x, float y,
                float size) {
        Node &parent = *slot.content;
        const auto color =
            nickColor(model, entry.nick, sourceServer(model.buffers.at(active), entry));
        slot.avatarBackground = rectangle(parent, x, y, size, size,
                                          themeSurfaceFill(brighten(color, .18f)), size * .28f);
        if (model.settings.avatarStyle == "monogram" || model.settings.avatarStyle == "initials") {
            const auto initial = entry.nick.empty()
                                     ? std::string("?")
                                     : entry.nick.substr(0, ui_logic::next(entry.nick, 0));
            const float fontSize = size * .55f;
            const auto metrics = renderer.measureText(initial, fontSize, themeUiFont(), 0, 1);
            auto text = std::make_unique<TextNode>();
            slot.avatarInitial = text.get();
            text->setText(initial);
            text->setFontSize(fontSize);
            text->setFontFamily(themeUiFont().family);
            text->setFontFeatures(themeUiFont().features);
            text->setFontWeight(themeUiFont().weight);
            text->setColor(color);
            text->setMaxLines(1);
            text->setPosition(x + size * .5f - (metrics.inkLeft + metrics.inkRight) * .5f +
                                  metrics.alignOffsetX,
                              y + size * .5f - (metrics.inkTop + metrics.inkBottom) * .5f);
            text->setSize(std::max(1.f, metrics.width),
                          std::max(1.f, metrics.bottom - metrics.top));
            text->setHitTestVisible(false);
            parent.addChild(std::move(text));
        } else {
            const auto *source = sourceContext(model.buffers.at(active), entry);
            if (!source)
                return;
            const auto network = model.networks.find(source->server);
            const std::string_view mapping = network == model.networks.end()
                                                 ? std::string_view("rfc1459")
                                                 : std::string_view(network->second.caseMapping);
            auto image = std::make_unique<Image>();
            image->setPosition(x, y);
            image->setSize(size, size);
            image->setFit(ImageFit::Contain);
            image->setHitTestVisible(false);
            if (setFoundryAvatar(*image, renderer, entry.nick, mapping)) {
                slot.avatarBackground->setVisible(false);
                parent.addChild(std::move(image));
            }
        }
    }
    static void addRowPaint(Node &content, std::unique_ptr<Node> child) {
        // Keep this plain row tree paint-sorted so tiled proxies need no per-tile sort/copy.
        const auto &children = content.children();
        const auto position =
            std::upper_bound(children.begin(), children.end(), child->zIndex(),
                             [](auto z, const auto &existing) { return z < existing->zIndex(); });
        content.insertChildAt(size_t(position - children.begin()), std::move(child));
    }
    void bind(Renderer &renderer, Slot &slot, const Buffer &buffer, size_t index) {
        auto &c = *cache;
        auto &row = c.rows[index];
        const auto &entry = buffer.messages[index];
        const bool notice = typedActivity(entry);
        const bool collapsed = row.activitySummary && !row.activityExpanded;
        const auto &g = c.geometry;
        const auto &metrics = g.metrics;
        const float friendExtent = row.friendAuthor ? 18 * g.scale : 0;
        slot.index = index;
        slot.area->setPosition(0, float(c.header + c.heights.prefix(index)));
        slot.area->setSize(c.width, float(row.height));
        slot.area->setVisible(true);
        slot.area->setFocusable(readable(buffer, index) || row.activitySummary);
        const bool foolish = this->foolish(buffer, entry);
        slot.area->setCursorShape(
            row.activitySummary || concealedFool(buffer, index)     ? cursorPointer
            : textDrag.armed && textDrag.owner == row.focusIdentity ? cursorText
                                                                    : cursorArrow);
        slot.area->setTooltip(
            row.activitySummary
                ? foolish ? row.activityExpanded ? "Minimize messages" : "Reveal minimized messages"
                  : row.activityExpanded ? "Collapse channel activity"
                                         : "Expand channel activity"
            : foolish && !row.foolOpen && foolPolicy(buffer, entry) == FoolDisplay::Blurred
                ? "Click to reveal blurred message"
                : "");
        const bool rebound = slot.key != row.key || slot.buffer != active;
        const bool focusRebound = slot.buffer != active || slot.focusIdentity != row.focusIdentity;
        std::string_view avatarMapping;
        if (g.avatars && model.settings.avatarStyle != "monogram" &&
            model.settings.avatarStyle != "initials")
            if (const auto *source = sourceContext(buffer, entry)) {
                const auto network = model.networks.find(source->server);
                avatarMapping = network == model.networks.end()
                                    ? std::string_view("rfc1459")
                                    : std::string_view(network->second.caseMapping);
            }
        std::string_view sourceCaption;
        if (buffer.kind == "mentions")
            if (const auto *source = sourceContext(buffer, entry))
                sourceCaption = serverDisplayName(model, source->server);
        if (!rebound && !focusRebound && slot.revision == row.revision &&
            slot.messagesRevision == c.messagesRevision &&
            slot.avatarStyle == model.settings.avatarStyle && slot.avatarMapping == avatarMapping &&
            slot.policy == c.policy && slot.sourceCaption == sourceCaption)
            return;
        if (slot.content)
            rememberActionFocus(slot, *slot.content);
        cancelMessagePresses(*slot.area);
        slot.key = row.key;
        slot.buffer = active;
        slot.focusIdentity = row.focusIdentity;
        if (focusRebound)
            slot.updateFocusKey();
        slot.revision = row.revision;
        slot.messagesRevision = c.messagesRevision;
        slot.avatarStyle = model.settings.avatarStyle;
        slot.avatarMapping = avatarMapping;
        slot.policy = c.policy;
        slot.sourceCaption = sourceCaption;
        slot.text.clear();
        slot.preview = nullptr;
        slot.selectionRects.clear();
        slot.nicks.clear();
        slot.channelTime = nullptr;
        slot.bubbleTime = nullptr;
        slot.nickChip = nullptr;
        slot.nickChipText = nullptr;
        slot.friendBadge = nullptr;
        slot.ownNickChip = false;
        slot.avatarInitial = nullptr;
        slot.statusSpinner = nullptr;
        slot.statusCheck = nullptr;
        slot.avatarBackground = nullptr;
        slot.roleRects.clear();
        slot.reactions.clear();
        stopFlight(slot);
        if (slot.reveal)
            slot.reveal->setReveal(1);
        if (slot.reveal)
            slot.reveal->setSource(nullptr);
        if (slot.content)
            slot.area->removeChild(slot.content);
        if (slot.decorations)
            slot.area->removeChild(slot.decorations);
        const float top = (row.day ? g.day : 0) + (row.unread ? g.unread : 0) +
                          g.groupGap(entry.presence, row.grouped);
        auto decorations = std::make_unique<Node>();
        slot.decorations = decorations.get();
        // Header controls must beat the full-row message node in native hit order,
        // without a full-height decoration box masking the message's real controls.
        slot.decorations->setSize(c.width, top);
        slot.decorations->setZIndex(1);
        slot.area->addChild(std::move(decorations));
        auto content = std::make_unique<Node>();
        slot.content = content.get();
        slot.content->setSize(c.width, float(row.height));
        slot.area->addChild(std::move(content));
        if (!slot.reveal) {
            auto reveal = std::make_unique<PixelRevealNode>();
            slot.reveal = reveal.get();
            slot.area->addChild(std::move(reveal));
        }
        slot.reveal->setSource(slot.content);
        const float gap = g.rowGap(entry.presence, row.grouped);
        const float padding = notice ? g.activityPadding() : g.padding(entry.presence);
        const float avatarLane = g.avatarLane(entry.presence);
        const float surfaceX =
            notice           ? (g.chatWidth - row.surfaceWidth) * .5f
            : entry.presence ? (c.width - row.surfaceWidth) * .5f
            : entry.own && g.bubbles && !entry.action
                ? c.width - metrics.inset * g.scale - avatarLane - row.surfaceWidth
                : metrics.inset * g.scale + avatarLane;
        const float surfaceHeight = float(row.height) - top - gap;
        auto material = std::make_unique<MaterialNode>(MaterialKind::Message);
        slot.material = material.get();
        slot.material->setPosition(surfaceX, top);
        slot.material->setSize(row.surfaceWidth, surfaceHeight);
        slot.material->setVisible(g.bubbles && !entry.presence && !entry.action);
        slot.material->setHitTestVisible(false);
        slot.content->addChild(std::move(material));
        auto dense = std::make_unique<RectNode>();
        slot.dense = dense.get();
        slot.dense->setPosition(surfaceX, top);
        slot.dense->setSize(row.surfaceWidth, surfaceHeight);
        slot.dense->setVisible(notice || (!entry.presence && (g.dense() || entry.action)));
        slot.dense->setHitTestVisible(false);
        slot.content->addChild(std::move(dense));
        if (row.activitySummary) {
            auto chevron = std::make_unique<Glyph>();
            chevron->setGlyph(row.activityExpanded ? "chevron-down" : "chevron-right");
            chevron->setGlyphSize(g.statusSize());
            chevron->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
            chevron->setPosition(surfaceX + row.surfaceWidth - padding - g.statusSize(),
                                 top + (surfaceHeight - g.statusSize()) * .5f);
            chevron->setHitTestVisible(false);
            slot.content->addChild(std::move(chevron));
        }
        if (buffer.kind != "mentions" && g.dense() && !entry.presence && entry.mention &&
            !entry.own)
            slot.roleRects.emplace_back(
                rectangle(*slot.content, surfaceX, top, 2 * g.scale, surfaceHeight,
                          themeForegroundColor(entry.mention ? "tertiary" : "primary")),
                entry.mention ? "tertiary" : "primary");
        float separatorY = 0;
        if (row.day) {
            const auto date =
                entry.timestamp ? localDate(entry.timestamp, "%A, %B %e, %Y") : "Undated history";
            auto font = themeUiFont();
            font.weight = FontWeight::Medium;
            const float fontSize = 12 * g.scale;
            const auto metrics = renderer.measureText(date, fontSize, font, 0, 1);
            const float width = std::min(metrics.width + 16 * g.scale,
                                         std::max(1.f, 2 * c.width - view.width() - 8 * g.scale));
            const float height = metrics.bottom - metrics.top + 8 * g.scale;
            auto chip = std::make_unique<InputArea>();
            chip->setPosition((view.width() - width) * .5f, (g.day - height) * .5f);
            chip->setSize(width, height);
            chip->setPassiveTouchPress(true);
            chip->setFocusOnPointerPress(false);
            chip->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}));
            chip->setTooltip(date);
            slot.roleRects.emplace_back(rectangle(*chip, 0, 0, width, height,
                                                  themeSurfaceColor("surface_container_high"),
                                                  height * .5f),
                                        "surface_container_high");
            auto *day =
                label(*chip, date, 8 * g.scale, 4 * g.scale, std::max(1.f, width - 16 * g.scale),
                      fontSize, themeForegroundColor("on_surface_variant"), font, {},
                      ColorRole::OnSurfaceVariant);
            day->setMinWidth(std::max(1.f, width - 16 * g.scale));
            day->setTextAlign(TextAlign::Center);
            day->setHitTestVisible(false);
            slot.decorations->addChild(std::move(chip));
            separatorY += g.day;
        }
        if (row.unread) {
            slot.roleRects.emplace_back(
                rectangle(*slot.decorations, 0, separatorY + g.unread - 5 * g.scale, c.width,
                          g.scale, withAlpha(themeForegroundColor("primary"), .5f)),
                "primary");
            label(*slot.decorations, "New since your last visit", 12 * g.scale,
                  separatorY + 3 * g.scale, c.width - 24 * g.scale, g.uiFont,
                  themeForegroundColor("primary"), themeUiFont(), {}, ColorRole::Primary);
        }
        const float x = surfaceX + padding + row.prefixWidth;
        slot.bodyX = x;
        float y = top + (notice ? g.activityPaddingY() : g.paddingY(entry.presence));
        if (!collapsed && g.bubbles && g.avatars && !row.grouped && !entry.presence)
            avatar(renderer, slot, entry,
                   g.tight ? x - row.emotePrefix
                   : entry.own && !entry.action
                       ? c.width - metrics.inset * g.scale - metrics.avatarSize * g.scale
                       : metrics.inset * g.scale,
                   top + (g.tight ? g.paddingY(false) : 0), metrics.avatarSize * g.scale);
        if (!collapsed && ((g.channel && !entry.presence) || (entry.presence && row.showClock))) {
            auto prefix = [&](std::string text, float left, float width, bool nick) {
                auto hit = std::make_unique<InputArea>();
                auto *area = hit.get();
                area->setPassiveTouchPress(true);
                area->setFocusOnPointerPress(false);
                area->setPosition(left, row.prefixBaseline - row.prefixAscent);
                area->setSize(std::max(1.f, width), row.prefixHeight);
                area->setClipChildren(true);
                area->setTooltip(nick ? entry.nick : entry.time);
                if (nick) {
                    area->setFocusable(true);
                    area->setTabFocusKey(slot.actionFocusKey("nick", entry.nick));
                    area->setOnClick([this, buffer = active, nick = entry.nick,
                                      identity = row.focusIdentity](const auto &pointer) {
                        nickMenu(buffer, nick, pointer, identity);
                    });
                    area->setOnKeyDown([this, buffer = active, nick = entry.nick,
                                        identity = row.focusIdentity](const auto &key) {
                        if (key.sym == XKB_KEY_Return || key.sym == XKB_KEY_space ||
                            contextKey(key)) {
                            nickMenu(buffer, nick, {}, identity);
                        }
                    });
                    const float avatarSize = g.compactAvatarSize();
                    const float avatarInset = g.compactAvatarInset();
                    const float chipPadding = entry.own && !entry.action ? g.chipPadding() : 0;
                    const float textLimit =
                        std::max(1.f, width - avatarInset - 2 * chipPadding - friendExtent);
                    auto actorFont = themeUiFont();
                    if (entry.action)
                        actorFont.weight = FontWeight::Bold;
                    const float textWidth = std::min(
                        textLimit,
                        std::ceil(renderer.measureText(text, g.metadataFont, actorFont).width));
                    const float chipWidth = textWidth + 2 * chipPadding + friendExtent;
                    const float clusterWidth = avatarInset + chipWidth;
                    const float clusterX =
                        g.inlineChannel ? std::max(0.f, width - clusterWidth) : 0;
                    if (avatarSize > 0)
                        avatar(renderer, slot, entry, left + clusterX,
                               area->y() + (row.prefixHeight - avatarSize) * .5f, avatarSize);
                    const float nameX = clusterX + avatarInset;
                    if (entry.own && !entry.action) {
                        slot.ownNickChip = true;
                        const float chipHeight =
                            std::min(row.prefixHeight, g.metadataHeader + 2 * g.chipPaddingY());
                        slot.nickChip = rectangle(
                            *area, nameX, (row.prefixHeight - chipHeight) * .5f, chipWidth,
                            chipHeight, themeForegroundColor("primary"), 6 * g.scale);
                    }
                    auto *name =
                        label(*area, text, nameX + chipPadding, 0, textWidth, g.metadataFont,
                              entry.own && !entry.action
                                  ? themeForegroundColor("on_primary")
                                  : nickColor(model, entry.nick, sourceServer(buffer, entry)),
                              actorFont);
                    name->setMinWidth(textWidth);
                    name->setTextAlign(g.inlineChannel ? TextAlign::End : TextAlign::Start);
                    name->measure(renderer);
                    name->setPosition(nameX + chipPadding,
                                      row.prefixAscent - name->baselineOffset());
                    name->setHitTestVisible(false);
                    if (row.friendAuthor)
                        slot.friendBadge = friendGlyph(
                            *area, nameX + chipPadding + textWidth + 4 * g.scale,
                            (row.prefixHeight - 14 * g.scale) * .5f, 14 * g.scale,
                            entry.own && !entry.action
                                ? themeForegroundColor("on_primary")
                                : nickColor(model, entry.nick, sourceServer(buffer, entry)));
                    if (entry.own && !entry.action)
                        slot.nickChipText = name;
                    else
                        slot.nicks.emplace_back(name, entry.nick);
                } else {
                    area->setAcceptedButtons(0);
                    auto node = std::make_unique<TextNode>();
                    slot.channelTime = node.get();
                    node->setText(std::move(text));
                    node->setFontSize(notice ? g.activityFont : g.metadataFont);
                    const auto &font = notice ? g.activityStyle : themeUiFont();
                    node->setFontFamily(font.family);
                    node->setFontFeatures(font.features);
                    node->setFontWeight(font.weight);
                    node->setMaxWidth(std::max(1.f, width));
                    node->setMaxLines(1);
                    node->setPosition(0, row.prefixAscent);
                    node->setSize(std::max(1.f, width), row.prefixHeight);
                    node->setHitTestVisible(false);
                    area->addChild(std::move(node));
                }
                slot.content->addChild(std::move(hit));
            };
            if (row.showClock)
                prefix(clockText(entry), surfaceX + padding, g.timeGutter - 4 * g.scale, false);
            if (g.nickVisible(row.grouped, entry.presence))
                prefix(entry.action ? "↳ " + entry.nick : entry.nick, surfaceX + g.timeGutter,
                       g.nickGutter, true);
            if (g.stackedPrefix(row.grouped, entry.presence, row.showClock))
                y += row.prefixHeight;
        }
        if (g.dense() && !g.channel && !entry.presence && row.showClock)
            label(*slot.content, clockText(entry), surfaceX, y,
                  std::max(1.f, row.prefixWidth - 4 * g.scale), g.metadataFont,
                  themeForegroundColor("on_surface_variant"), themeUiFont(), entry.time,
                  ColorRole::OnSurfaceVariant);
        if (!collapsed && entry.action && !entry.presence && g.bubbles) {
            auto actorFont = themeMessageFont();
            actorFont.weight = FontWeight::Bold;
            auto *name = label(
                *slot.content, "↳ " + entry.nick, x - row.emotePrefix + g.authorAvatarInset(),
                row.bodyOffset,
                std::max(1.f, row.emotePrefix - g.authorAvatarInset() - friendExtent - 8 * g.scale),
                g.font, nickColor(model, entry.nick, sourceServer(buffer, entry)), actorFont,
                entry.nick);
            slot.nicks.emplace_back(name, entry.nick);
            if (row.friendAuthor)
                slot.friendBadge =
                    friendGlyph(*slot.content, x - 8 * g.scale - 14 * g.scale,
                                row.bodyOffset + (g.bodyLine - 14 * g.scale) * .5f, 14 * g.scale,
                                nickColor(model, entry.nick, sourceServer(buffer, entry)));
            name->setHitTestVisible(true);
            name->setFocusable(true);
            name->setTabFocusKey(slot.actionFocusKey("nick", entry.nick));
            name->setOnClick([this, buffer = active, nick = entry.nick,
                              identity = row.focusIdentity](const auto &pointer) {
                nickMenu(buffer, nick, pointer, identity);
            });
            name->setOnKeyDown([this, buffer = active, nick = entry.nick,
                                identity = row.focusIdentity](const auto &key) {
                if (key.sym == XKB_KEY_Return || key.sym == XKB_KEY_space || contextKey(key))
                    nickMenu(buffer, nick, {}, identity);
            });
        }
        if (!collapsed && !row.grouped && g.bubbles && !entry.presence && !entry.action) {
            const auto &author = entry.nick;
            const auto color =
                withAlpha(nickColor(model, entry.nick, sourceServer(buffer, entry)), 1.f);
            const float chipWidth =
                std::min(std::max(1.f, row.contentWidth - g.authorAvatarInset()), row.authorWidth);
            const float authorX =
                entry.own ? x + row.contentWidth - chipWidth : x + g.authorAvatarInset();
            slot.nickChip = rectangle(*slot.content, authorX, y, chipWidth,
                                      row.authorHeight + 2 * g.chipPaddingY(), color, 6 * g.scale);
            auto *name =
                label(*slot.content, author, authorX + g.chipPadding(), y + g.chipPaddingY(),
                      std::max(1.f, chipWidth - 2 * g.chipPadding() - friendExtent), g.nickFont,
                      readableTextColorForBackground(color), g.nickStyle, author);
            name->setBaselineMode(LabelBaselineMode::InkCentered);
            slot.nickChipText = name;
            if (row.friendAuthor)
                slot.friendBadge =
                    friendGlyph(*slot.content, authorX + chipWidth - g.chipPadding() - 14 * g.scale,
                                y + (row.authorHeight + 2 * g.chipPaddingY() - 14 * g.scale) * .5f,
                                14 * g.scale, readableTextColorForBackground(color));
            name->setHitTestVisible(true);
            name->setFocusable(true);
            name->setTabFocusKey(slot.actionFocusKey("nick", entry.nick));
            name->setOnClick([this, buffer = active, nick = entry.nick,
                              identity = row.focusIdentity](const auto &pointer) {
                nickMenu(buffer, nick, pointer, identity);
            });
            name->setOnKeyDown([this, buffer = active, nick = entry.nick,
                                identity = row.focusIdentity](const auto &key) {
                if (key.sym == XKB_KEY_Return || key.sym == XKB_KEY_space || contextKey(key)) {
                    nickMenu(buffer, nick, {}, identity);
                }
            });
            y += row.authorHeader;
        }
        if (buffer.kind == "mentions") {
            const auto *source = sourceContext(buffer, entry);
            std::string context;
            if (source)
                context = slot.sourceCaption + " / " + source->target;
            float contextX = x;
            float contextWidth = row.contentWidth;
            if (!g.bubbles && readable(buffer, index)) {
                auto actorFont = themeUiFont();
                actorFont.weight = FontWeight::Bold;
                const float actorWidth =
                    std::min(renderer.measureText(entry.nick, g.metadataFont, actorFont).width,
                             std::max(1.f, (row.contentWidth - friendExtent - 8 * g.scale) * .45f));
                auto *actor = label(*slot.content, entry.nick, x, y, actorWidth, g.metadataFont,
                                    nickColor(model, entry.nick, sourceServer(buffer, entry)),
                                    actorFont, entry.nick);
                actor->setHitTestVisible(true);
                actor->setFocusable(true);
                actor->setTabFocusKey(slot.actionFocusKey("original-actor", entry.sourceId));
                actor->setOnClick([this, bufferId = active, key = row.key](const auto &) {
                    openOriginal(bufferId, key);
                });
                actor->setOnKeyDown([this, bufferId = active, key = row.key](const auto &event) {
                    if (event.pressed && !event.preedit && !event.modifiers &&
                        (event.sym == XKB_KEY_Return || event.sym == XKB_KEY_space))
                        openOriginal(bufferId, key);
                });
                slot.nicks.emplace_back(actor, entry.nick);
                if (row.friendAuthor)
                    slot.friendBadge =
                        friendGlyph(*slot.content, x + actorWidth + 4 * g.scale,
                                    y + (g.metadataHeader - 14 * g.scale) * .5f, 14 * g.scale,
                                    nickColor(model, entry.nick, sourceServer(buffer, entry)));
                contextX += actorWidth + friendExtent + 8 * g.scale;
                contextWidth =
                    std::max(1.f, row.contentWidth - actorWidth - friendExtent - 8 * g.scale);
            }
            auto *origin =
                label(*slot.content, context, contextX, y, contextWidth, g.metadataFont,
                      themeForegroundColor("on_surface_variant"), themeUiFont(), context);
            origin->setHitTestVisible(true);
            origin->setFocusable(readable(buffer, index));
            origin->setTabFocusKey(slot.actionFocusKey("original", entry.sourceId));
            origin->setOnClick([this, bufferId = active, key = row.key](const auto &) {
                openOriginal(bufferId, key);
            });
            origin->setOnKeyDown([this, bufferId = active, key = row.key](const auto &event) {
                if (event.pressed && !event.preedit && !event.modifiers &&
                    (event.sym == XKB_KEY_Return || event.sym == XKB_KEY_space))
                    openOriginal(bufferId, key);
            });
            y += g.metadataHeader;
        }
        if (readable(buffer, index) && !entry.reply.empty()) {
            const float height = row.quoteHeight - 4 * g.scale;
            auto target = std::make_unique<InputArea>();
            auto *reference = target.get();
            reference->setPosition(x, y);
            reference->setSize(row.contentWidth, height);
            reference->setClipChildren(true);
            reference->setPassiveTouchPress(true);
            reference->setFocusOnPointerPress(false);
            slot.content->addChild(std::move(target));
            auto *card = rectangle(*reference, 0, 0, row.contentWidth, height,
                                   themeSurfaceColor("surface_container"), themeCornerRadius());
            auto style = card->style();
            style.border = themeForegroundColor("outline_variant");
            style.borderWidth = g.scale;
            card->setStyle(style);
            rectangle(*reference, 5 * g.scale, 6 * g.scale, 2 * g.scale,
                      std::max(1.f, height - 12 * g.scale), withAlpha(row.quoteColor, .6f),
                      g.scale);
            auto authorFont = themeUiFont();
            authorFont.weight = FontWeight::Bold;
            auto *author = label(*reference, row.quoteAuthor, 12 * g.scale, 6 * g.scale,
                                 std::max(1.f, row.contentWidth - 24 * g.scale), g.metadataFont,
                                 row.quoteColor, authorFont);
            author->setUseMarkup(false);
            auto *excerpt = label(*reference, row.quoteMarkup, 12 * g.scale,
                                  6 * g.scale + row.quoteAuthorHeight,
                                  std::max(1.f, row.contentWidth - 24 * g.scale), g.metadataFont,
                                  themeForegroundColor("on_surface"), themeUiFont());
            excerpt->setUseMarkup(true);
            excerpt->setMaxLines(2);
            author->setHitTestVisible(false);
            excerpt->setHitTestVisible(false);
            const auto *parent = quotedMessage(buffer, entry);
            if (parent && quoteReadable(buffer, entry, *parent)) {
                reference->setHitTestVisible(true);
                reference->setFocusable(true);
                reference->setTabFocusKey(slot.actionFocusKey("reply", entry.reply));
                const auto jump = [this, bufferId = active, key = row.key] {
                    const auto found = model.buffers.find(bufferId);
                    const auto *entry = message(bufferId, key);
                    if (!entry || found == model.buffers.end() || !cache ||
                        !cache->keys.contains(key) || !readable(found->second, cache->keys.at(key)))
                        return;
                    const auto *parent = quotedMessage(found->second, *entry);
                    const auto *source = sourceContext(found->second, *entry);
                    if (source && parent && quoteReadable(found->second, *entry, *parent)) {
                        if (source->id == bufferId &&
                            resolve(found->second, parent->id) < found->second.messages.size())
                            jumpToSourceMessage(parent->id);
                        else
                            model.actions.push_back({"source-jump", source->id, parent->id, {}});
                    }
                };
                reference->setOnClick([jump](const auto &) { jump(); });
                reference->setOnKeyDown([jump](const auto &key) {
                    if (key.pressed && !key.preedit && !key.modifiers &&
                        (key.sym == XKB_KEY_Return || key.sym == XKB_KEY_space))
                        jump();
                });
            }
        }
        auto body = std::make_unique<Node>();
        slot.body = body.get();
        body->setPosition(x, row.bodyOffset);
        body->setSize(row.contentWidth, row.bodyHeight);
        body->setForegroundBlurRadius(concealedFool(buffer, index) ? 10 * g.scale : 0);
        slot.content->addChild(std::move(body));
        size_t previousLink = absent;
        for (const auto &fragment : row.fragments) {
            if (row.urlReplaced)
                break;
            auto hit = std::make_unique<InputArea>();
            auto *area = hit.get();
            area->setPosition(fragment.x, fragment.y);
            area->setSize(std::max(1.f, fragment.width), fragment.height);
            area->setZIndex(2);
            area->setHitTestVisible(readable(buffer, index));
            if (fragment.url.empty() && fragment.nick.empty()) {
                // Hover owns the I-beam; presses still belong to the row's selection handlers.
                area->setAcceptedButtons(0);
                area->setCursorShape(cursorText);
            }
            if (!fragment.url.empty() || !fragment.nick.empty()) {
                const bool firstPiece = fragment.linkStart != previousLink;
                area->setFocusable(firstPiece);
                area->setTabStop(firstPiece);
                if (firstPiece)
                    area->setTabFocusKey(
                        slot.actionFocusKey("link", fragment.url, fragment.linkStart));
                previousLink = fragment.linkStart;
                area->setPassiveTouchPress(true);
                area->setFocusOnPointerPress(false);
                area->setTooltip(fragment.url);
                const auto identity = row.focusIdentity;
                std::weak_ptr<int> guard = alive;
                area->setOnPress([this, guard, identity, area](const auto &pointer) {
                    if (guard.expired())
                        return;
                    if (pointer.pressed) {
                        const auto slot =
                            std::find_if(slots.begin(), slots.end(), [identity](const Slot &entry) {
                                return entry.focusIdentity == identity;
                            });
                        if (slot != slots.end())
                            beginTextSelection(*slot, pointer, *area);
                    } else
                        finishTextSelection(identity);
                });
                area->setOnMotion([this, guard, area](const auto &pointer) {
                    if (!guard.expired())
                        moveTextSelection(*area, pointer);
                });
                area->setOnCancel([this, guard, identity] {
                    if (!guard.expired())
                        cancelTextDrag(identity);
                });
                area->setOnClick([this, identity, url = fragment.url](const auto &) {
                    if (!consumeSelectionClick(identity))
                        launch(url, identity);
                });
                area->setOnKeyDown([this, identity, url = fragment.url](const auto &key) {
                    if (key.sym == XKB_KEY_Return || key.sym == XKB_KEY_space)
                        launch(url, identity);
                });
            }
            if (!fragment.nick.empty()) {
                area->setFocusable(true);
                area->setTabStop(true);
                area->setTabFocusKey(
                    slot.actionFocusKey("mention", fragment.nick, fragment.displayBegin));
                area->setOnClick([this, identity = row.focusIdentity,
                                  nick = fragment.nick](const auto &pointer) {
                    if (readableIdentity(identity) && !consumeSelectionClick(identity))
                        nickMenu(active, nick, pointer, identity);
                });
                area->setOnKeyDown([this, identity = row.focusIdentity,
                                    nick = fragment.nick](const auto &key) {
                    if (readableIdentity(identity) &&
                        (key.sym == XKB_KEY_Return || key.sym == XKB_KEY_space || contextKey(key)))
                        nickMenu(active, nick, {}, identity);
                });
            }
            auto text = std::make_unique<TextNode>();
            text->setText(fragment.text);
            text->setUseMarkup(true);
            text->setFontSize(notice ? g.activityFont : g.font);
            text->setFontFamily(fragment.font.family);
            text->setFontFeatures(fragment.font.features);
            text->setFontWeight(fragment.font.weight);
            text->setMaxLines(1);
            text->setPosition(fragment.inset, fragment.baseline);
            text->setSize(fragment.width - 2 * fragment.inset, fragment.height);
            text->setHitTestVisible(false);
            slot.text.push_back({text.get(), fragment.color});
            area->addChild(std::move(text));
            slot.body->addChild(std::move(hit));
        }
        if (mediaController && row.previewHeight > 0 && readable(buffer, index)) {
            auto preview = std::make_unique<MediaPreviewCard>(*mediaController);
            slot.preview = preview.get();
            preview->setReadable([this](uint64_t identity) { return readableIdentity(identity); });
            preview->setOpenLink([this](std::string url, uint64_t identity) {
                launch(std::move(url), identity, false);
            });
            preview->bind(row.focusIdentity, row.previewUrl, true, !this->foolish(buffer, entry));
            preview->setPosition(x, row.previewOffset);
            preview->setSize(row.contentWidth, row.previewHeight);
            preview->sync(renderer, row.contentWidth, g.scale);
            slot.content->addChild(std::move(preview));
        }
        for (const auto &reaction : row.reactions) {
            auto button = std::make_unique<Button>();
            button->inputArea()->setPassiveTouchPress(true);
            button->inputArea()->setFocusOnPointerPress(false);
            button->inputArea()->setTabFocusKey(slot.actionFocusKey("reaction", reaction.emoji));
            button->setText(reaction.label);
            button->setFontSize(g.metadataFont);
            button->label()->setFontFamily(themeUiFont().family);
            button->label()->setFontFeatures(themeUiFont().features);
            button->label()->setFontWeight(themeUiFont().weight);
            button->setVariant(ButtonVariant::Secondary);
            button->setPosition(x + reaction.x, reaction.y + 3 * g.scale);
            button->setSize(reaction.width, g.reaction - 5 * g.scale);
            button->setControlHeight(g.reaction - 5 * g.scale);
            const bool enabled = referenceable(buffer, entry) &&
                                 reactionAllowed(model, buffer, entry, reaction.emoji);
            button->setEnabled(enabled);
            button->setTooltip(reaction.users +
                               (enabled ? "" : "\nThis reaction cannot be changed here."));
            button->setOnClick(
                [this, key = row.key, emoji = reaction.emoji] { react(active, key, emoji); });
            slot.reactions.push_back({button.get(), x + reaction.x, reaction.y + 3 * g.scale});
            addRowPaint(*slot.content, std::move(button));
        }
        if (!row.footerText.empty()) {
            auto time = std::make_unique<TextNode>();
            slot.bubbleTime = time.get();
            time->setText(row.footerText);
            time->setFontSize(g.timeFont);
            time->setFontFamily(themeUiFont().family);
            time->setFontFeatures(themeUiFont().features);
            time->setFontWeight(themeUiFont().weight);
            time->setMaxLines(0);
            time->setMaxWidth(row.clockWidth);
            time->setTextAlign(TextAlign::End);
            const float clockTop =
                row.footerStacked ? 0 : (row.footerHeight - row.clockHeight) * .5f;
            time->setPosition(x + row.contentWidth - row.footerWidth,
                              row.footerOffset + clockTop + row.footerAscent);
            time->setSize(row.clockWidth, row.clockHeight);
            time->setOpacity(.82f);
            time->setHitTestVisible(false);
            addRowPaint(*slot.content, std::move(time));
        }
        if (g.bubbles && !entry.presence && sendIndicator(entry)) {
            auto area = std::make_unique<InputArea>();
            const float statusTop = row.footerStacked ? row.clockHeight + g.statusGap()
                                                      : (row.footerHeight - g.statusSize()) * .5f;
            area->setPosition(x + row.contentWidth - g.statusSize(), row.footerOffset + statusTop);
            area->setSize(g.statusSize(), g.statusSize());
            area->setAcceptedButtons(0);
            area->setFocusOnPointerPress(false);
            area->setTooltip(entry.sendStatus == SendStatus::Queued      ? "Sending to server"
                             : entry.sendStatus == SendStatus::Confirmed ? "Confirmed by server"
                                                                         : "Sent to server");
            if (entry.sendStatus == SendStatus::Queued) {
                auto spinner = std::make_unique<Spinner>();
                slot.statusSpinner = spinner.get();
                spinner->setSpinnerSize(g.statusSize());
                spinner->setThickness(1.5f * g.scale);
                spinner->setHitTestVisible(false);
                area->addChild(std::move(spinner));
            } else {
                auto check = std::make_unique<Glyph>();
                slot.statusCheck = check.get();
                check->setGlyph("check");
                check->setGlyphSize(g.statusSize());
                check->setHitTestVisible(false);
                area->addChild(std::move(check));
            }
            addRowPaint(*slot.content, std::move(area));
        }
        if (!readable(buffer, index)) {
            const auto disable = [](auto &&self, Node &node) -> void {
                node.setHitTestVisible(false);
                if (auto *input = dynamic_cast<InputArea *>(&node)) {
                    input->setFocusable(false);
                    input->setExcludeSubtreeFromTabOrder(true);
                    input->setTooltip("");
                }
                for (const auto &child : node.children())
                    self(self, *child);
            };
            disable(disable, *slot.content);
        }
    }

    void stopFlight(Slot &slot) {
        if (slot.flightReveal) {
            slot.flightReveal->setSource(nullptr);
            slot.flightGroup->removeChild(slot.flightReveal);
            slot.flightReveal = nullptr;
        }
        if (slot.flight) {
            slot.flight->setSource(nullptr);
            slot.flightGroup->removeChild(slot.flight);
            slot.flight = nullptr;
        }
        if (slot.flightGroup) {
            motionHost->removeChild(slot.flightGroup);
            slot.flightGroup = nullptr;
        }
        if (slot.content)
            slot.content->setOpacity(1);
        if (!slot.area->hitTestVisible()) {
            slot.area->setHitTestVisible(true);
            slot.area->setExcludeSubtreeFromTabOrder(false);
            if (dispatcher)
                dispatcher->syncPointerHover();
        }
    }
    void cancelEntrance(Slot &slot) {
        stopFlight(slot);
        if (cache && slot.buffer == active && slot.index < cache->rows.size() &&
            cache->rows[slot.index].focusIdentity == slot.focusIdentity)
            cache->rows[slot.index].entranceStarted = -1;
        if (slot.content)
            slot.content->setPosition(0, 0);
        if (slot.content)
            slot.content->setScale(1);
        if (slot.reveal)
            slot.reveal->setReveal(1);
    }
    void excludeRows() {
        for (auto &slot : slots) {
            if (!slot.area->excludeSubtreeFromTabOrder() || slot.area->pressed() || slot.clickArmed)
                cancelMessagePresses(*slot.area);
            if (slot.statusSpinner)
                slot.statusSpinner->stop();
            cancelEntrance(slot);
            slot.area->setExcludeSubtreeFromTabOrder(true);
            slot.focusRing->setVisible(false);
        }
    }
    void visuals() {
        if (!cache || !view.visible() || view.width() <= 0 || view.height() <= 0) {
            excludeRows();
            return;
        }
        auto *buffer = current();
        if (!buffer)
            return;
        const auto viewport = viewportBounds();
        if (!viewport) {
            excludeRows();
            return;
        }
        const float left = viewport->x, top = viewport->y;
        const float right = viewport->x + viewport->width;
        const float bottom = viewport->y + viewport->height;
        fxViewport(left, top, right - left, bottom - top);
        const auto textRange = orderedTextRange();
        for (auto &slot : slots) {
            if (!slot.area->visible() || slot.index >= buffer->messages.size() || !slot.material ||
                slot.key != ui_logic::messageKey(buffer->messages[slot.index])) {
                if (slot.statusSpinner)
                    slot.statusSpinner->stop();
                cancelEntrance(slot);
                continue;
            }
            const auto &entry = buffer->messages[slot.index];
            const auto &g = cache->geometry;
            auto &row = cache->rows[slot.index];
            const float opacity =
                foolish(*buffer, entry) && foolPolicy(*buffer, entry) == FoolDisplay::Shown ? 1.f
                : foolish(*buffer, entry) && foolPolicy(*buffer, entry) == FoolDisplay::Dimmed
                    ? .38f
                : entry.pending && g.dense() ? .52f
                                             : 1;
            slot.content->setPosition(0, 0);
            slot.content->setScale(1);
            float destinationX, destinationY;
            Node::mapToScene(slot.material, slot.material->width() * .5f,
                             slot.material->height() * .5f, destinationX, destinationY);
            const auto owns = [&](const Node *node) {
                for (; node; node = node->parent())
                    if (node == slot.area)
                        return true;
                return false;
            };
            const bool interacting = (dispatcher && (owns(dispatcher->capturedArea()) ||
                                                     owns(dispatcher->pendingTouchPressArea()) ||
                                                     owns(dispatcher->focusedArea()))) ||
                                     (keys->focused() && selectedIdentity == slot.focusIdentity) ||
                                     (textDrag.armed && textDrag.owner == slot.focusIdentity);
            float finalLeft, finalTop, finalRight, finalBottom;
            Node::transformedBounds(slot.material, finalLeft, finalTop, finalRight, finalBottom);
            const bool destinationVisible =
                finalRight > left && finalLeft < right && finalBottom > top && finalTop < bottom;
            const bool enabled = row.entranceStarted >= 0 && (g.bubbles || !entry.own) &&
                                 !interacting && destinationVisible && !foolish(*buffer, entry) &&
                                 (!entry.own || (motionHost && composerOrigin)) &&
                                 !model.settings.reducedMotion && model.settings.fx != FxLevel::Off;
            const auto origin = composerOrigin.value_or(std::pair{destinationX, destinationY});
            const auto entrance = fx::messageEntrance(
                model.now - row.entranceStarted, entry.own, origin.first, origin.second,
                destinationX, destinationY, enabled, model.settings.sendMotion,
                model.settings.sendReveal, model.settings.incomingMotion,
                model.settings.incomingReveal, model.settings.incomingDurationMs);
            if (!entrance.active)
                row.entranceStarted = -1;
            const float lane = !row.grouped && g.avatars ? g.avatarLane(false) : 0;
            const LayoutRect revealBounds{slot.material->x() - (entry.own ? 0 : lane), 0,
                                          slot.material->width() + lane, slot.content->height()};
            if (entrance.active && entry.own) {
                slot.reveal->setReveal(1);
                slot.content->setPosition(entrance.x, entrance.y);
                if (!slot.flight) {
                    auto group = std::make_unique<Node>();
                    group->setHitTestVisible(false);
                    group->setParticipatesInLayout(false);
                    slot.flightGroup = group.get();
                    motionHost->addChild(std::move(group));
                    auto flight = std::make_unique<RenderProxyNode>(slot.content);
                    slot.flight = flight.get();
                    slot.flightGroup->addChild(std::move(flight));
                    auto reveal = std::make_unique<PixelRevealNode>(slot.flight);
                    slot.flightReveal = reveal.get();
                    slot.flightGroup->addChild(std::move(reveal));
                    slot.area->setHitTestVisible(false);
                    if (dispatcher)
                        dispatcher->syncPointerHover();
                }
                float sceneX, sceneY, hostX, hostY;
                Node::mapToScene(slot.content, 0, 0, sceneX, sceneY);
                Node::mapFromScene(motionHost, sceneX, sceneY, hostX, hostY);
                slot.flightGroup->setPosition(hostX, hostY);
                slot.flightGroup->setTransformOrigin(
                    slot.material->x() + slot.material->width() * .5f,
                    slot.material->y() + slot.material->height() * .5f);
                slot.flightGroup->setScale(entrance.scale);
                slot.flight->setPosition(0, 0);
                slot.flight->setSize(slot.content->width(), slot.content->height());
                slot.content->setOpacity(0);
                slot.flightReveal->setRevealBounds(revealBounds);
                slot.flightReveal->setReveal(entrance.reveal, opacity,
                                             model.settings.sendReveal == SendReveal::Pixels);
            } else {
                stopFlight(slot);
                slot.content->setPosition(entrance.x, entrance.y);
                slot.content->setTransformOrigin(slot.material->x() + slot.material->width() * .5f,
                                                 slot.material->height() * .5f);
                slot.content->setScale(entrance.scale);
                slot.reveal->setRevealBounds(revealBounds);
                slot.reveal->setReveal(entrance.reveal, opacity,
                                       model.settings.incomingReveal == SendReveal::Pixels);
            }
            paintTextSelection(slot, textRange, top, bottom);
            const bool hovered = slot.area->hovered();
            const Color normal = entry.deleted || entry.presence
                                     ? themeForegroundColor("on_surface_variant")
                                 : entry.own && g.bubbles && !entry.action
                                     ? themeForegroundColor("on_primary_container")
                                     : themeForegroundColor("on_surface");
            for (auto &text : slot.text)
                text.node->setColor(text.color.value_or(normal));
            for (auto &[label, nick] : slot.nicks)
                label->setColor(nickColor(model, nick, sourceServer(*buffer, entry)));
            if (slot.friendBadge)
                slot.friendBadge->setColor(
                    nickColor(model, entry.nick, sourceServer(*buffer, entry)));
            if (slot.nickChip) {
                const auto color =
                    slot.ownNickChip
                        ? themeForegroundColor("primary")
                        : withAlpha(nickColor(model, entry.nick, sourceServer(*buffer, entry)),
                                    1.f);
                auto style = slot.nickChip->style();
                style.fill = color;
                slot.nickChip->setStyle(style);
                slot.nickChipText->setColor(slot.ownNickChip
                                                ? themeForegroundColor("on_primary")
                                                : readableTextColorForBackground(color));
                if (slot.friendBadge)
                    slot.friendBadge->setColor(slot.ownNickChip
                                                   ? themeForegroundColor("on_primary")
                                                   : readableTextColorForBackground(color));
            }
            if (slot.avatarInitial)
                slot.avatarInitial->setColor(
                    nickColor(model, entry.nick, sourceServer(*buffer, entry)));
            if (slot.channelTime)
                slot.channelTime->setColor(themeForegroundColor("on_surface_variant"));
            if (slot.bubbleTime)
                slot.bubbleTime->setColor(themeForegroundColor(
                    entry.own && !entry.action ? "on_primary_container" : "on_surface_variant"));
            const auto statusColor = themeForegroundColor(
                entry.action && g.bubbles ? "on_tertiary_container" : "on_primary_container");
            if (slot.statusSpinner)
                slot.statusSpinner->setColor(statusColor);
            if (slot.statusCheck)
                slot.statusCheck->setColor(statusColor);
            float bounce = 0;
            const auto reactionAt = cache->reactionTimes.find(entry.id);
            if (!model.settings.reducedMotion && model.settings.fx != FxLevel::Off &&
                reactionAt != cache->reactionTimes.end())
                bounce =
                    -4 * g.scale *
                    std::sin(std::clamp(float((model.now - reactionAt->second) / .25), 0.f, 1.f) *
                             3.14159265f);
            for (const auto &reaction : slot.reactions)
                reaction.button->setPosition(reaction.x, reaction.y + bounce);
            if (slot.avatarBackground) {
                auto style = slot.avatarBackground->style();
                style.fill = themeSurfaceFill(
                    brighten(nickColor(model, entry.nick, sourceServer(*buffer, entry)), .18f));
                slot.avatarBackground->setStyle(style);
            }
            for (const auto &[rectangle, role] : slot.roleRects) {
                auto style = rectangle->style();
                style.fill = role == "surface_container_high"
                                 ? themeSurfaceColor(role)
                                 : withAlpha(themeForegroundColor(role), style.fill.a);
                rectangle->setStyle(style);
            }
            const bool mentionAccent = buffer->kind != "mentions" && entry.mention;
            RoundedRectStyle denseStyle{};
            denseStyle.fill = typedActivity(entry) ? themeSurfaceColor("surface_container_high")
                              : mentionAccent || hovered
                                  ? themeSurfaceColor(mentionAccent ? "tertiary_container"
                                                                    : "surface_container_high")
                                  : withAlpha(themeColor("surface"), 0);
            denseStyle.radius = typedActivity(entry)
                                    ? std::min(slot.dense->height() * .5f, 12 * g.scale)
                                : entry.action ? themeCornerRadius()
                                               : 0;
            slot.dense->setStyle(denseStyle);
            slot.material->setMessage({.server = buffer->server,
                                       .target = buffer->target,
                                       .id = entry.id,
                                       .own = entry.own,
                                       .presence = entry.presence,
                                       .hovered = hovered,
                                       .highlight = mentionAccent,
                                       .opacity = 1});
            slot.material->sync();
            float l, t, r, b;
            Node::transformedBounds(slot.material, l, t, r, b);
            l = std::max(l, left);
            t = std::max(t, top);
            r = std::min(r, right);
            b = std::min(b, bottom);
            const bool inViewport = r > l && b > t;
            if (!inViewport && !slot.area->excludeSubtreeFromTabOrder() &&
                !(textDrag.armed && textDrag.owner == slot.focusIdentity))
                cancelMessagePresses(*slot.area);
            slot.area->setExcludeSubtreeFromTabOrder(!inViewport || slot.flight);
            if (slot.statusSpinner) {
                float statusLeft, statusTop, statusRight, statusBottom;
                Node::transformedBounds(slot.statusSpinner, statusLeft, statusTop, statusRight,
                                        statusBottom);
                const bool statusVisible = inViewport && statusRight > left && statusLeft < right &&
                                           statusBottom > top && statusTop < bottom;
                if (statusVisible && !model.settings.reducedMotion &&
                    model.settings.fx != FxLevel::Off)
                    slot.statusSpinner->start();
                else
                    slot.statusSpinner->stop();
            }
            const bool focused =
                inViewport && !slot.flight &&
                (nativeFocus(*slot.area) ||
                 (keys->focused() && cache->rows[slot.index].focusIdentity == selectedIdentity));
            slot.focusRing->setVisible(focused);
            if (focused) {
                const auto &row = cache->rows[slot.index];
                const float width =
                    g.bubbles || entry.presence ? slot.material->width() : row.selectionWidth;
                slot.focusRing->setPosition(slot.material->x(), slot.material->y());
                slot.focusRing->setSize(width, slot.material->height());
                RoundedRectStyle style{};
                style.fill = withAlpha(themeColor("secondary"), 0);
                style.border = themeForegroundColor("secondary");
                style.borderWidth = std::min({g.scale, width, slot.material->height()});
                style.radius =
                    g.bubbles && !entry.action ? themeBubbleRadius() : themeCornerRadius();
                slot.focusRing->setStyle(style);
            }
            if (r > l && b > t)
                fxAnchor(buffer->server, buffer->target, entry.id, l, t, r - l, b - t);
        }
    }

    void beforeBufferRename(std::string_view oldId, std::string_view newId) {
        if (!cache || active != pane.buffer || (active != oldId && active != newId))
            return;
        const auto buffer = model.buffers.find(active);
        if (buffer == model.buffers.end())
            return;
        cache->following = pane.following;
        const float oldScroll = cache->scroll;
        reconcileBuffer(*cache, buffer->second, true);
        cache->unseen = pane.unseen;
        // Storage may reject the rename; observation alone must not interrupt native scrolling.
        if (cache->scroll != oldScroll)
            switched = true;
    }
    void remapBuffer(std::string_view oldId, std::string_view newId) {
        for (auto &slot : slots)
            cancelEntrance(slot);
        closePopup();
        if (oldId == newId)
            return;
        const std::string oldKey(oldId), newKey(newId);
        const bool sourceActive = active == oldId;
        const bool affected = sourceActive || active == newId;
        if (cache && affected) {
            if (!switched)
                cache->scroll = scroll->scrollOffset();
            cache->following = pane.following;
            cache->unseen = pane.unseen;
        }
        const auto source = caches.find(oldKey);
        if (source != caches.end()) {
            // The pane displaying either side retains its own viewport. Inactive
            // collisions keep the destination; a lone source cache moves intact.
            if (sourceActive || !caches.contains(newKey)) {
                auto retained = caches.extract(source);
                if (const auto discarded = caches.find(newKey); discarded != caches.end())
                    retireMedia(discarded->second);
                caches.erase(newKey);
                retained.key() = newKey;
                caches.insert(std::move(retained));
            } else {
                retireMedia(source->second);
                caches.erase(source);
            }
        }
        if (sourceActive)
            active = newKey;
        // Always repair this pointer, including active-destination collisions.
        const auto currentCache = caches.find(active);
        cache = currentCache == caches.end() ? nullptr : &currentCache->second;
        const auto retained = caches.find(newKey);
        if (retained != caches.end()) {
            auto &baseline = retained->second;
            baseline.messagesRevision.reset();
            if (const auto merged = model.buffers.find(newKey); merged != model.buffers.end()) {
                // Imports already existed at the merge boundary; a later append must not
                // reclassify them as arrivals. Reuse the keyed bridge without shaping.
                reconcileBuffer(baseline, merged->second, false);
                if (affected)
                    switched = true;
            }
        }
        if (selectedText && selectedText->buffer == oldId)
            selectedText->buffer = newKey;
        const std::string oldPrefix = std::to_string(oldKey.size()) + ":" + oldKey;
        const std::string newPrefix = std::to_string(newKey.size()) + ":" + newKey;
        if (actionFocus)
            for (auto *key : {&actionFocus->key, &actionFocus->fallbackKey})
                if (*key)
                    if (auto remapped = remappedMessageFocusKey(**key, oldPrefix, newPrefix))
                        *key = std::move(*remapped);
        if (focusRevealTarget)
            if (auto remapped = remappedMessageFocusKey(*focusRevealTarget, oldPrefix, newPrefix))
                focusRevealTarget = std::move(*remapped);
        // Keep live keys usable by modal return before the anchored rebuild runs.
        for (auto &slot : slots) {
            if (slot.buffer == oldId) {
                cancelMessagePresses(*slot.area);
                slot.buffer = newKey;
                remapMessageFocusKeys(*slot.area, oldPrefix, newPrefix);
            }
            if (slot.buffer == newId)
                slot.messagesRevision.reset();
        }
        if (affected)
            view.markLayoutDirty();
    }
    void sync() {
        retireDeletedMedia();
        std::erase_if(urlOpeners, [](pid_t process) {
            const auto result = waitpid(process, nullptr, WNOHANG);
            return result == process || (result < 0 && errno == ECHILD);
        });
        auto *buffer = current();
        if (active != pane.buffer || !cache) {
            for (auto &slot : slots)
                cancelEntrance(slot);
            closePopup();
            closeTypingList();
            actionFocus.reset();
            selectedIdentity = navigationRevealIdentity = 0;
            selectedIndexHint = absent;
            view.clearTextSelection();
            cancelFocusReveal();
            if (cache) {
                cache->scroll = scroll->scrollOffset();
                cache->following = pane.following;
                cache->unseen = pane.unseen;
                for (auto &row : cache->rows)
                    row.entranceStarted = -1;
            }
            active = pane.buffer;
            cache = &caches[active];
            for (auto &row : cache->rows)
                row.entranceStarted = -1;
            pane.following = cache->following;
            pane.unseen = cache->unseen;
            switched = true;
            jumpTarget.clear();
            for (auto &slot : slots) {
                if (slot.area->visible() || slot.area->pressed() || slot.clickArmed)
                    cancelMessagePresses(*slot.area);
                if (slot.statusSpinner)
                    slot.statusSpinner->stop();
                slot.area->setVisible(false);
            }
        }
        if (!buffer) {
            for (auto &slot : slots) {
                cancelEntrance(slot);
                if (slot.area->visible() || slot.area->pressed() || slot.clickArmed)
                    cancelMessagePresses(*slot.area);
                if (slot.statusSpinner)
                    slot.statusSpinner->stop();
                slot.area->setVisible(false);
            }
            history->setVisible(false);
            empty->setVisible(true);
            empty->setText("Select a conversation to see its messages.");
            typing->setVisible(false);
            mentionFooter->setVisible(false);
            unseen->setVisible(false);
            view.markLayoutDirty();
            return;
        }
        if (buffer->kind == "mentions" && !cache->initialized) {
            pane.following = false;
            cache->following = false;
            cache->scroll = 0;
        }
        if (effectsTick != model.now) {
            effectsTick = model.now;
            for (const auto &event : model.effects) {
                if (event.server != buffer->server || event.target != buffer->target)
                    continue;
                if (event.type == "reaction" || event.type == "redact")
                    closePopup();
                if (event.type == "redact")
                    clearTextSelection();
                if (event.type == "reaction" && !event.history && !event.id.empty())
                    cache->reactionTimes[event.id] = model.now;
            }
        }
        std::erase_if(cache->reactionTimes,
                      [this](const auto &entry) { return model.now - entry.second > .5; });
        if (pane.following && !focusRevealTarget && cache->pendingArrivals == 0)
            pane.unseen = 0;
        history->setVisible(buffer->kind != "mentions");
        history->setEnabled(buffer->kind != "mentions" && !buffer->loading);
        history->setText(buffer->loading ? "Loading history…" : "Load older messages");
        empty->setVisible(buffer->messages.empty());
        empty->setText(buffer->loading ? "Loading messages…" : "No messages yet");
        size_t typerCount = 0;
        bool typersChanged = false;
        const auto eligibleTyper = [&](const auto &entry) {
            return model.settings.typingReceive && model.now - entry.second < 12 &&
                   !isFool(model, buffer->server, entry.first);
        };
        for (const auto &entry : buffer->typing)
            if (eligibleTyper(entry)) {
                if (typerCount >= typingParticipants.size() ||
                    typingParticipants[typerCount] != entry.first)
                    typersChanged = true;
                ++typerCount;
            }
        if (typersChanged || typerCount != typingParticipants.size()) {
            closeTypingList();
            typingShown = absent;
            typingParticipants.clear();
            typingParticipants.reserve(typerCount);
            for (const auto &entry : buffer->typing)
                if (eligibleTyper(entry))
                    typingParticipants.push_back(entry.first);
            for (size_t i = 0; i < typingNicks.size(); ++i)
                typingNicks[i]->setText(i < typingParticipants.size()
                                            ? typingParticipants[i] +
                                                  (i == 0 && typerCount > 1 ? "," : "")
                                            : "");
        }
        for (size_t i = 0; i < typingDots.size(); ++i)
            if (typingDots[i])
                typingDots[i]->setOpacity(
                    model.settings.reducedMotion || model.settings.fx == FxLevel::Off
                        ? .65f
                        : .35f + .65f * float((1 + std::sin(model.now * 2.1 - i * .9)) * .5));
        const float delta = lastTyping ? float(std::clamp(model.now - lastTyping, 0., .1)) : .016f;
        lastTyping = model.now;
        const float target = typingParticipants.empty() ? 0.f : 1.f;
        if (model.settings.reducedMotion || model.settings.fx == FxLevel::Off)
            typingOpacity = target;
        else
            typingOpacity += (target - typingOpacity) * (1 - std::exp(-12 * delta));
        typing->setVisible(typingOpacity > .01f);
        typing->setOpacity(typingOpacity);
        // Signature checks and shaping happen during the next native layout and only
        // for the viewport. An unchanged offscreen history is never walked here.
        if (cache->messagesRevision == buffer->messagesRevision &&
            cache->rows.size() == buffer->messages.size())
            tickTextSelection();
        view.markLayoutDirty();
        visuals();
    }

    void layout(Renderer &renderer, bool allowFocusReveal = true) {
        selectionRenderer = &renderer;
        arranging = true;
        auto *buffer = current();
        const float width = std::max(1.f, view.width());
        const bool readOnly = buffer && (buffer->kind == "mentions" || buffer->kind == "search");
        const float footerHeight = readOnly ? 0 : typingRowHeight;
        const float bottomInset = readOnly ? 0 : typingInset;
        const float height = std::max(1.f, view.height());
        const float occlusion =
            readOnly ? 0 : std::min(height, std::max(footerHeight, replyOverlayHeight));
        scroll->setSize(width, height);
        if (!buffer || !cache) {
            cancelFocusReveal();
            canvas->setSize(width, height);
            empty->setPosition(12, 40);
            empty->setMaxWidth(std::max(1.f, width - 24));
            scroll->layout(renderer);
            arranging = false;
            return;
        }
        auto &c = *cache;
        const bool revealingFocus = allowFocusReveal && validFocusReveal();
        c.prepared.clear();
        const float oldScroll = switched || !allowFocusReveal ? c.scroll : scroll->scrollOffset();
        const size_t oldAnchor = c.heights.at(std::max(0., double(oldScroll - c.header)));
        std::string anchorKey;
        double oldBodyTop = 0, oldBodyHeight = 0, oldRowHeight = 0;
        std::optional<double> footerAnchor;
        if (!pane.following && jumpTarget.empty() && oldAnchor < c.rows.size()) {
            const auto &row = c.rows[oldAnchor];
            const double rowTop = c.header + c.heights.prefix(oldAnchor);
            anchorKey = row.key;
            oldBodyTop = rowTop + row.bodyOffset;
            oldBodyHeight = row.bodyHeight;
            oldRowHeight = row.height;
            const double footerTop = rowTop + row.footerOffset;
            if (row.footerHeight > 0 && oldScroll >= footerTop &&
                oldScroll < footerTop + row.footerHeight)
                footerAnchor = (oldScroll - footerTop) / row.footerHeight;
        }
        scroll->setContentScale(themeScale());
        const float bodyWidth = std::max(1.f, width - scroll->scrollbarGutter());
        const auto g = geometry(renderer, *buffer, model, bodyWidth, height, width);
        const bool reflow = c.width != bodyWidth || c.geometry.key != g.key;
        rebuild(*buffer, g, bodyWidth);
        if (selectedText) {
            selectedText->anchor.hint =
                identityIndex(selectedText->anchor.identity, selectedText->anchor.hint);
            selectedText->focus.hint =
                identityIndex(selectedText->focus.identity, selectedText->focus.hint);
            if (!orderedTextRange())
                clearTextSelection();
        }
        c.policy.clear();
        if (const auto network = model.networks.find(buffer->server);
            network != model.networks.end()) {
            c.policy = network->second.nick + "|" + network->second.clientTagDeny + "|" +
                       network->second.caseMapping + "|" + network->second.state;
            for (const auto &cap : network->second.caps)
                c.policy += "|" + cap;
        }
        if (popupOwned && popupPolicy != privacyPolicy(*buffer))
            closePopup();
        c.header = buffer->kind == "mentions" ? 0 : std::max(28 * g.scale, g.header + 8 * g.scale);
        history->setFontSize(g.uiFont);
        history->setControlHeight(c.header);
        history->setPosition(0, 0);
        history->setSize(bodyWidth, c.header);
        empty->setFontSize(g.uiFont);
        empty->setPosition(12 * g.scale, c.header + 12 * g.scale);
        empty->setMaxWidth(std::max(1.f, bodyWidth - 24 * g.scale));
        double desired = oldScroll;
        size_t anchor = absent;
        if (navigationRevealIdentity) {
            const size_t target = identityIndex(navigationRevealIdentity, selectedIndexHint);
            navigationRevealIdentity = 0;
            if (target < c.rows.size()) {
                anchor = target;
                measure(renderer, *buffer, target);
                const auto &row = c.rows[target];
                const double start = c.header + c.heights.prefix(target) + (row.day ? g.day : 0) +
                                     (row.unread ? g.unread : 0);
                const double end = c.header + c.heights.prefix(target) + row.height;
                if (start < desired)
                    desired = start;
                else if (end > desired + height - occlusion)
                    desired = std::min(start, end - (height - occlusion));
                anchorKey.clear();
            }
        }
        if (!jumpTarget.empty()) {
            const auto target = c.ids.find(jumpTarget);
            if (target != c.ids.end() && target->second < c.rows.size() &&
                readable(*buffer, target->second)) {
                anchor = target->second;
                measure(renderer, *buffer, anchor);
                desired = c.header + c.heights.prefix(anchor) + c.rows[anchor].bodyOffset;
                selectedIdentity = c.rows[anchor].focusIdentity;
                sourceJumpIdentity = selectedIdentity;
                selectedIndexHint = anchor;
            }
            jumpTarget.clear();
        }
        if (!anchorKey.empty()) {
            const auto found = c.keys.find(anchorKey);
            if (found != c.keys.end()) {
                anchor = found->second;
                measure(renderer, *buffer, anchor);
                const auto &row = c.rows[anchor];
                const double rowTop = c.header + c.heights.prefix(anchor);
                if (footerAnchor) {
                    desired = rowTop + (row.footerHeight > 0
                                            ? row.footerOffset + *footerAnchor * row.footerHeight
                                            : row.contentEnd);
                } else
                    desired =
                        ui_logic::anchoredBodyScroll(oldScroll, oldBodyTop, rowTop + row.bodyOffset,
                                                     oldBodyHeight, row.bodyHeight, reflow);
                const double rowEnd = rowTop + row.height;
                if ((reflow || row.height != oldRowHeight) && desired >= rowEnd)
                    desired = std::nextafter(static_cast<float>(rowEnd),
                                             -std::numeric_limits<float>::infinity());
            }
        }
        if (anchor != absent) {
            // Resolve the actual destination viewport BEFORE committing a native
            // deferred jump, preserving the original body through prepend/reflow.
            for (size_t i = c.rows[anchor].previousVisible; i != absent;
                 i = c.rows[i].previousVisible) {
                if (c.header + c.heights.prefix(i + 1) < desired - 100 * g.scale)
                    break;
                desired += measure(renderer, *buffer, i);
            }
            for (size_t i = anchor + 1; i < c.rows.size() && c.header + c.heights.prefix(i) <
                                                                 desired + height + 150 * g.scale;
                 ++i) {
                if (c.rows[i].hiddenActivity) {
                    if (c.rows[i].activityEnd > i && c.rows[i].activityEnd != absent)
                        i = c.rows[i].activityEnd - 1;
                    continue;
                }
                measure(renderer, *buffer, i);
            }
        }
        const float typingHeight = bottomInset;
        if (pane.following && !revealingFocus) {
            // Measure backwards from the tail, not an estimated index near the old
            // extent. Tall newest rows cannot push the bottom viewport into estimates.
            double covered = 0;
            size_t last = c.rows.empty() ? absent : c.rows.size() - 1;
            if (last != absent && c.rows[last].hiddenActivity)
                last = c.rows[last].previousVisible;
            for (size_t i = last; i != absent && covered < height + 150 * g.scale;
                 i = c.rows[i].previousVisible) {
                measure(renderer, *buffer, i);
                covered += c.rows[i].height;
            }
            desired =
                std::max(0., c.header + c.heights.prefix(c.rows.size()) + typingHeight - height);
        }
        desired = std::max(0., desired);
        std::vector<size_t> visible;
        size_t first = c.heights.at(std::max(0., desired - c.header - 100 * g.scale));
        for (size_t i = first;
             i < c.rows.size() && c.header + c.heights.prefix(i) < desired + height + 150 * g.scale;
             ++i) {
            if (c.rows[i].hiddenActivity) {
                if (c.rows[i].activityEnd > i && c.rows[i].activityEnd != absent)
                    i = c.rows[i].activityEnd - 1;
                continue;
            }
            const double delta = measure(renderer, *buffer, i);
            if (!pane.following && anchor != absent && i < anchor)
                desired += delta;
            visible.push_back(i);
            if (c.rows[i].activitySummary && !c.rows[i].activityExpanded)
                i = c.rows[i].activityEnd - 1;
        }
        c.prepared.insert(c.prepared.end(), c.materialized.begin(), c.materialized.end());
        for (const auto old : c.prepared)
            if (old < c.rows.size() && !std::binary_search(visible.begin(), visible.end(), old)) {
                if (textDrag.armed && c.rows[old].focusIdentity == textDrag.owner)
                    continue;
                c.rows[old].fragments.clear();
                c.rows[old].lines.clear();
                c.rows[old].fragmentsReady = false;
                c.rows[old].reactions.clear();
                c.rows[old].selectionText.reset();
                c.rows[old].selectionGeometryReady = false;
            }
        c.materialized = visible;
        // A hidden native-focused slot keeps its message until focus is pruned.
        // There is only one native focus, so pinning needs at most one extra slot.
        const bool pinnedFocus = std::any_of(slots.begin(), slots.end(), [&](const Slot &slot) {
            return nativeFocus(*slot.area, true) &&
                   (slot.buffer != active ||
                    std::none_of(visible.begin(), visible.end(),
                                 [&](size_t index) { return c.rows[index].key == slot.key; }));
        });
        const bool pinnedDrag =
            textDrag.armed && std::any_of(slots.begin(), slots.end(), [&](const Slot &slot) {
                return slot.buffer == active && slot.focusIdentity == textDrag.owner &&
                       std::none_of(visible.begin(), visible.end(), [&](size_t index) {
                           return c.rows[index].focusIdentity == textDrag.owner;
                       });
            });
        const bool dragOwnsFocus =
            pinnedDrag && std::any_of(slots.begin(), slots.end(), [&](const Slot &slot) {
                return slot.focusIdentity == textDrag.owner && nativeFocus(*slot.area, true);
            });
        while (slots.size() <
               visible.size() + size_t(pinnedFocus) + size_t(pinnedDrag && !dragOwnsFocus)) {
            auto area = std::make_unique<InputArea>();
            auto *observer = area.get();
            area->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}));
            area->setPassiveTouchPress(true);
            area->setFocusOnPointerPress(false);
            area->setFocusable(true);
            area->setTabStop(true);
            area->setTooltip(
                "Tap message background or timestamp for actions. "
                "Right-click or Menu / Shift+F10 also works; dragging does not open actions.");
            std::weak_ptr<int> guard = alive;
            area->setOnFocusGain([this, guard] {
                if (!guard.expired())
                    visuals();
            });
            area->setOnFocusLoss([this, guard] {
                if (!guard.expired())
                    visuals();
            });
            const size_t slotIndex = slots.size();
            area->setOnPress([this, slotIndex](const auto &pointer) {
                auto &slot = slots[slotIndex];
                if (pointer.pressed) {
                    cancelFocusReveal();
                    slot.clickArmed = true;
                    slot.pressX = pointer.localX;
                    slot.pressY = pointer.localY;
                    textDrag.suppressedClick = 0;
                    if (pointer.button == BTN_LEFT) {
                        if (beginTextSelection(slot, pointer, *slot.area)) {
                            if (!current() ||
                                (current()->kind != "mentions" && current()->kind != "search"))
                                slot.clickArmed = false;
                        } else
                            clearTextSelection();
                    }
                } else {
                    finishTextSelection(slot.focusIdentity);
                    if (std::max(std::abs(pointer.localX - slot.pressX),
                                 std::abs(pointer.localY - slot.pressY)) > 4 * themeScale())
                        slot.clickArmed = false;
                }
            });
            area->setOnMotion([this, slotIndex](const auto &pointer) {
                auto &slot = slots[slotIndex];
                if (slot.clickArmed &&
                    std::max(std::abs(pointer.localX - slot.pressX),
                             std::abs(pointer.localY - slot.pressY)) > 4 * themeScale())
                    slot.clickArmed = false;
                if (textDrag.armed && textDrag.owner == slot.focusIdentity)
                    moveTextSelection(*slot.area, pointer);
            });
            area->setOnCancel([this, guard, slotIndex] {
                if (!guard.expired()) {
                    slots[slotIndex].clickArmed = false;
                    cancelTextDrag(slots[slotIndex].focusIdentity);
                }
            });
            area->setOnClick([this, slotIndex](const auto &pointer) {
                auto &slot = slots[slotIndex];
                const bool armed = std::exchange(slot.clickArmed, false);
                if (!armed || !slot.area->visible() || slot.buffer != active ||
                    slot.area->excludeSubtreeFromTabOrder())
                    return;
                if (consumeSelectionClick(slot.focusIdentity))
                    return;
                if (pointer.button == BTN_LEFT && cache && slot.index < cache->rows.size() &&
                    cache->rows[slot.index].activitySummary) {
                    toggleActivity(slot.focusIdentity);
                    return;
                }
                if (pointer.button == BTN_LEFT && cache && slot.index < cache->rows.size() &&
                    !readable(*current(), slot.index) &&
                    foolPolicy(*current(), current()->messages[slot.index]) ==
                        FoolDisplay::Blurred) {
                    cache->rows[slot.index].foolOpen = true;
                    cache->projectionFrom = std::min(cache->projectionFrom, slot.index);
                    view.markLayoutDirty();
                    return;
                }
                if (pointer.button == BTN_LEFT && current() &&
                    (current()->kind == "mentions" || current()->kind == "search")) {
                    clearTextSelection();
                    openOriginal(active, slot.key);
                    return;
                }
                if (pointer.button == BTN_LEFT || pointer.button == BTN_RIGHT)
                    menu(*slot.area, active, slot.key, pointer);
            });
            area->setOnKeyDown([this, slotIndex](const auto &key) {
                const auto &slot = slots[slotIndex];
                if (key.pressed && !key.preedit && cache && slot.index < cache->rows.size() &&
                    cache->rows[slot.index].activitySummary &&
                    (key.sym == XKB_KEY_Return || key.sym == XKB_KEY_space)) {
                    toggleActivity(slot.focusIdentity);
                    return;
                }
                if (contextKey(key) && !slots[slotIndex].area->excludeSubtreeFromTabOrder())
                    menu(*slots[slotIndex].area, active, slots[slotIndex].key);
            });
            auto ring = std::make_unique<RectNode>();
            auto *focusRing = ring.get();
            ring->setHitTestVisible(false);
            ring->setVisible(false);
            ring->setZIndex(10);
            area->addChild(std::move(ring));
            canvas->addChild(std::move(area));
            Slot slot{};
            slot.area = observer;
            slot.focusRing = focusRing;
            slots.push_back(std::move(slot));
        }
        // Keep native child identity/focus for rows that remain in the viewport.
        // Only entering rows consume an unused slot; appending one message does
        // not recreate every already-visible control.
        std::vector<bool> used(slots.size(), false);
        for (const auto index : visible) {
            auto slot = std::find_if(slots.begin(), slots.end(), [&](const Slot &candidate) {
                return candidate.buffer == active && candidate.key == c.rows[index].key;
            });
            if (slot == slots.end())
                slot = std::find_if(slots.begin(), slots.end(), [&](const Slot &candidate) {
                    const auto position = size_t(&candidate - slots.data());
                    if (used[position] || nativeFocus(*candidate.area, true) ||
                        (textDrag.armed && candidate.focusIdentity == textDrag.owner))
                        return false;
                    return candidate.buffer != active ||
                           std::none_of(visible.begin(), visible.end(), [&](size_t row) {
                               return c.rows[row].key == candidate.key;
                           });
                });
            if (slot != slots.end()) {
                used[size_t(slot - slots.begin())] = true;
                bind(renderer, *slot, *buffer, index);
            }
        }
        if (textDrag.armed) {
            const auto captured = std::find_if(slots.begin(), slots.end(), [&](const Slot &slot) {
                return slot.buffer == active && slot.focusIdentity == textDrag.owner;
            });
            const size_t index = identityIndex(textDrag.owner);
            if (captured != slots.end() && index < c.rows.size()) {
                used[size_t(captured - slots.begin())] = true;
                captured->index = index;
                captured->area->setPosition(0, float(c.header + c.heights.prefix(index)));
                captured->area->setSize(c.width, float(c.rows[index].height));
            }
        }
        for (size_t i = 0; i < slots.size(); ++i)
            if (!used[i]) {
                cancelEntrance(slots[i]);
                rememberActionFocus(slots[i], *slots[i].area);
                if (slots[i].area->visible() || slots[i].area->pressed() || slots[i].clickArmed)
                    cancelMessagePresses(*slots[i].area);
                slots[i].area->setVisible(false);
            }
        const double rowsEnd = c.header + c.heights.prefix(c.rows.size());
        const auto mentions = retainedMentions(*buffer);
        const auto position = mentionPosition(*buffer, mentions);
        const auto mentionText = std::to_string(mentions.size());
        const float pillWidth =
            std::max(44 * g.scale,
                     std::ceil(renderer.measureText(mentionText, g.uiFont, themeUiFont()).width) +
                         24 * g.scale);
        const float controlHeight = 28 * g.scale;
        const float navigatorHeight = 3 * controlHeight;
        const float jumpExtent = 36 * g.scale;
        const float jumpTop = std::max(0.f, height - occlusion - jumpExtent - 8 * g.scale);
        const bool stackedRail = jumpTop >= navigatorHeight + 8 * g.scale;
        const bool railVisible = !mentions.empty() &&
                                 height >= occlusion + navigatorHeight + 8 * g.scale &&
                                 bodyWidth >= pillWidth + 24 * g.scale;
        mentionFooter->setVisible(railVisible);
        mentionFooter->setPosition(
            std::max(0.f, bodyWidth - pillWidth -
                              (stackedRail ? 12 * g.scale : jumpExtent + 20 * g.scale)),
            stackedRail ? jumpTop - navigatorHeight - 8 * g.scale
                        : std::max(0.f, height - occlusion - navigatorHeight - 8 * g.scale));
        mentionFooter->setSize(pillWidth, navigatorHeight);
        olderMention->setPosition(0, 0);
        olderMention->setSize(pillWidth, controlHeight);
        olderMention->setControlHeight(controlHeight);
        olderMention->setEnabled(!mentions.empty() && position > 0);
        newerMention->setPosition(0, 2 * controlHeight);
        newerMention->setSize(pillWidth, controlHeight);
        newerMention->setControlHeight(controlHeight);
        newerMention->setEnabled(!mentions.empty() && position < mentions.size() - 1);
        mentionCount->setText(mentionText);
        mentionSurface->setSize(pillWidth, navigatorHeight);
        auto pillStyle = mentionSurface->style();
        pillStyle.fill = themeSurfaceColor("surface_container_high");
        mentionSurface->setStyle(pillStyle);
        mentionCount->setFontSize(g.uiFont);
        mentionCount->setPosition(0, controlHeight);
        mentionCount->setSize(pillWidth, controlHeight);
        mentionCount->setControlHeight(controlHeight);
        mentionFooter->layout(renderer);
        typing->setPosition(8 * g.scale, std::max(0.f, height - footerHeight));
        // A zero-bounds host traverses only the real +N control; painted tray space
        // remains transparent to history selection and never clears composer focus.
        typing->setSize(0, 0);
        const float trayWidth = std::max(1.f, bodyWidth - 16 * g.scale);
        typingPaint->setClipChildren(true);
        typingPaint->setSize(trayWidth, footerHeight);
        const float avatarSize = 32 * g.scale;
        const bool avatars = model.settings.avatars && model.settings.avatarStyle != "none";
        size_t avatarCount = avatars ? std::min(typingParticipants.size(), size_t(3)) : 0;
        while (avatarCount && (44 + 18 * (avatarCount - 1) + 60) * g.scale > trayWidth)
            --avatarCount;
        const auto typingNetwork = model.networks.find(buffer->server);
        const std::string_view typingMapping =
            typingNetwork == model.networks.end()
                ? std::string_view("rfc1459")
                : std::string_view(typingNetwork->second.caseMapping);
        for (size_t i = 0; i < typingAvatars.size(); ++i) {
            auto &avatar = typingAvatars[i];
            const bool visible = i < avatarCount && trayWidth >= (40 + 18 * i) * g.scale;
            avatar.background->setVisible(visible);
            avatar.initial->setVisible(false);
            avatar.image->setVisible(false);
            if (!visible)
                continue;
            const auto &nick = typingParticipants[i];
            const bool changed = avatar.nick != nick || avatar.mapping != typingMapping ||
                                 avatar.style != model.settings.avatarStyle;
            if (changed) {
                avatar.nick = nick;
                avatar.mapping = typingMapping;
                avatar.style = model.settings.avatarStyle;
                avatar.initial->setText(nativeAvatarInitials(nick));
                if (avatar.style == "sprite") {
                    avatar.image->clear(renderer);
                    setFoundryAvatar(*avatar.image, renderer, nick, typingMapping);
                }
            }
            const auto color = nickColor(model, nick, buffer->server);
            auto style = avatar.background->style();
            style.fill = themeSurfaceFill(brighten(color, .18f));
            style.radius = avatarSize * .28f;
            avatar.background->setStyle(style);
            const float x = (4 + 18 * i) * g.scale;
            const float y = std::max(0.f, (footerHeight - avatarSize) / 2);
            avatar.background->setPosition(x, y);
            avatar.background->setSize(avatarSize, avatarSize);
            const bool sprite = avatar.style == "sprite" && avatar.image->hasImage();
            avatar.background->setVisible(!sprite);
            avatar.image->setVisible(sprite);
            avatar.initial->setVisible(!sprite);
            if (sprite) {
                avatar.image->setPosition(x, y);
                avatar.image->setSize(avatarSize, avatarSize);
            } else {
                avatar.initial->setColor(color);
                avatar.initial->setFontFamily(themeUiFont().family);
                avatar.initial->setFontFeatures(themeUiFont().features);
                avatar.initial->setFontSize(avatarSize * .55f);
                avatar.placement.place(*avatar.initial, renderer, x, y, avatarSize, avatarSize);
            }
        }
        float typingX = avatarCount ? (44 + 18 * (avatarCount - 1)) * g.scale : 10 * g.scale;
        typingX = std::min(typingX, std::max(0.f, trayWidth - 44 * g.scale));
        const float textStart = typingX;
        const float nameHeight = renderer.fontRowExtent(g.font, themeMessageFont());
        const float textY = std::max(0.f, (footerHeight - nameHeight - 7 * g.scale) / 2);
        const float overflowWidth = 40 * g.scale;
        size_t shown = 0;
        for (size_t i = 0; i < typingNicks.size(); ++i) {
            auto *name = typingNicks[i];
            const bool exists = i < typingParticipants.size();
            const bool needsOverflow = typingParticipants.size() > i + 1;
            const float available = std::max(0.f, trayWidth - typingX - 8 * g.scale -
                                                      (needsOverflow ? overflowWidth : 0));
            const bool visible = exists && shown == i && available >= 12 * g.scale;
            name->setVisible(visible);
            if (!visible)
                continue;
            name->setFontFamily(themeMessageFont().family);
            name->setFontFeatures(themeMessageFont().features);
            name->setFontWeight(themeMessageFont().weight);
            name->setFontSize(g.font);
            name->setColor(nickColor(model, typingParticipants[i], buffer->server));
            name->setMaxWidth(available);
            name->measure(renderer);
            const float w = std::min(available, name->width());
            name->setPosition(typingX, textY);
            typingX += w + 5 * g.scale;
            ++shown;
        }
        if (typingShown != shown) {
            typingShown = shown;
            for (size_t i = 0; i < typingNicks.size(); ++i)
                typingNicks[i]->setText(i < typingParticipants.size()
                                            ? typingParticipants[i] +
                                                  (i == 0 && shown > 1 ? "," : "")
                                            : "");
        }
        const size_t remaining = typingParticipants.size() - shown;
        if (typingRemaining != remaining) {
            typingRemaining = remaining;
            typingOverflow->setText(remaining ? "+" + std::to_string(remaining) : "");
        }
        typingOverflow->setVisible(remaining > 0);
        typingOverflow->setExcludeSubtreeFromTabOrder(remaining == 0);
        typingOverflow->setFontSize(g.font);
        typingOverflow->setPosition(typingX, textY);
        typingOverflow->setSize(std::min(overflowWidth, std::max(0.f, trayWidth - typingX)),
                                nameHeight);
        typingOverflow->setControlHeight(nameHeight);
        if (remaining)
            typingX += overflowWidth;
        else
            closeTypingList();
        for (size_t i = 0; i < typingDots.size(); ++i) {
            typingDots[i]->setPosition(textStart + i * 7 * g.scale,
                                       textY + nameHeight + 4 * g.scale);
            typingDots[i]->setSize(3 * g.scale, 3 * g.scale);
        }
        typingSurface->setVisible(g.bubbles && !typingParticipants.empty());
        typingSurface->setSize(std::min(trayWidth, typingX + 8 * g.scale), footerHeight);
        auto typingStyle = typingSurface->style();
        typingStyle.fill = themeSurfaceColor("surface_container");
        typingStyle.radius = 12 * g.scale;
        typingSurface->setStyle(typingStyle);
        typing->layout(renderer);
        if (typingListOpen) {
            typingList->setSize(width, height);
            typingDismiss->setSize(width, height);
            const float listWidth = std::max(0.f, std::min(280 * g.scale, width - 16 * g.scale));
            const float listHeight = std::min((typingParticipants.size() * 28 + 40) * g.scale,
                                              std::max(0.f, height - footerHeight - 8 * g.scale));
            const float listX =
                std::clamp(typing->x() + textStart, 0.f, std::max(0.f, width - listWidth));
            const float listY = std::max(0.f, height - footerHeight - listHeight - 4 * g.scale);
            typingListSurface->setPosition(listX, listY);
            typingListSurface->setSize(listWidth, listHeight);
            auto listStyle = typingListSurface->style();
            listStyle.fill = themeSurfaceColor("surface_container_high");
            typingListSurface->setStyle(listStyle);
            typingListTitle->setPosition(listX + 8 * g.scale, listY + 4 * g.scale);
            typingListTitle->setFontSize(14 * g.scale);
            typingListTitle->setMaxWidth(std::max(1.f, listWidth - 16 * g.scale));
            typingListScroll->setPosition(listX, listY + 32 * g.scale);
            typingListScroll->setSize(listWidth, std::max(0.f, listHeight - 32 * g.scale));
            typingListScroll->setContentScale(g.scale);
            typingListScroll->content()->setGap(0);
            for (size_t i = 0; i < typingListScroll->content()->children().size(); ++i) {
                auto &row = *static_cast<Flex *>(typingListScroll->content()->children()[i].get());
                auto *name = static_cast<Label *>(row.children().front().get());
                name->setFontSize(14 * g.scale);
                name->setFontFamily(themeUiFont().family);
                name->setFontFeatures(themeUiFont().features);
                name->setColor(nickColor(model, typingParticipants[i], buffer->server));
                name->setMaxWidth(std::max(1.f, listWidth - 24 * g.scale));
                row.setMinHeight(28 * g.scale);
                row.setMaxHeight(28 * g.scale);
                row.setPadding(0, 8 * g.scale);
            }
            typingList->layout(renderer);
        }
        const float extent =
            std::max(height, float(rowsEnd) + typingHeight +
                                 (buffer->messages.empty() ? g.header + 24 * g.scale : 0));
        canvas->setSize(bodyWidth, extent);
        keys->setSize(bodyWidth, extent);
        keys->setZIndex(-1);
        if (pane.following && !revealingFocus)
            desired = std::max(0., double(extent - height));
        c.scroll = float(desired);
        // No-op restoration must not cancel native wheel motion between frame ticks.
        if (!revealingFocus && (switched || c.scroll != scroll->scrollOffset()))
            scroll->requestScrollToOffset(c.scroll);
        // Binding can remove the requested action. Reuse its computed anchor, but give the
        // normal measured tail/viewport path one pass before committing a native offset.
        if (revealingFocus && !validFocusReveal()) {
            layout(renderer, false);
            return;
        }
        scroll->layout(renderer);
        c.scroll = scroll->scrollOffset();
        for (const auto index : c.arrivals)
            if (index < c.rows.size()) {
                auto &arrival = c.rows[index];
                const double top = c.header + c.heights.prefix(index) + arrival.bodyOffset;
                if (arrival.hiddenActivity || top >= c.scroll + height ||
                    top + arrival.bodyHeight <= c.scroll)
                    arrival.entranceStarted = -1;
            }
        c.arrivals.clear();
        bool settledReveal = revealingFocus;
        unseen->setPadding(0);
        unseen->setGlyphSize(20 * g.scale);
        unseen->setRadius(jumpExtent * .5f);
        unseen->setControlHeight(jumpExtent);
        unseen->setSize(jumpExtent, jumpExtent);
        unseen->setPosition(std::max(0.f, bodyWidth - jumpExtent - 12 * g.scale), jumpTop);
        arranging = false;
        switched = false;
        visuals();
        if (restoreActionFocus()) {
            const float beforeReveal = scroll->scrollOffset();
            // Consume focus-callback reveals now, without treating them as user scrolling.
            const bool queuedReveal = validFocusReveal();
            arranging = true;
            scroll->layout(renderer);
            arranging = false;
            c.scroll = scroll->scrollOffset();
            settledReveal = settledReveal || queuedReveal || c.scroll != beforeReveal;
        }
        if (settledReveal && validFocusReveal()) {
            // Native IntoView knows the viewport, not the reply floating over it.
            // Adjust only explicit focus reveals, within the existing scroll extent.
            float left, top, right, bottom, viewLeft, viewTop, viewRight, viewBottom;
            Node::transformedBounds(dispatcher->focusedArea(), left, top, right, bottom);
            Node::transformedBounds(scroll, viewLeft, viewTop, viewRight, viewBottom);
            const float delta =
                std::max(0.f, std::min(top - viewTop, bottom - (viewBottom - occlusion)));
            if (delta > 0) {
                arranging = true;
                scroll->setScrollOffset(scroll->scrollOffset() + delta);
                scroll->layout(renderer);
                arranging = false;
                c.scroll = scroll->scrollOffset();
                // Bind rows entering the corrected viewport before painting this frame.
                settleFocusReveal(true);
                layout(renderer, false);
                return;
            }
        }
        if (settledReveal || c.pendingArrivals > 0)
            settleFocusReveal(settledReveal);
        c.following = pane.following;
        c.unseen = pane.unseen;
        unseen->setVisible(
            !buffer->messages.empty() && scroll->maxScrollOffset() > 1 &&
            scroll->maxScrollOffset() - scroll->scrollOffset() > 1 &&
            bodyWidth >= jumpExtent + 24 * g.scale &&
            (stackedRail || !railVisible || bodyWidth >= pillWidth + jumpExtent + 24 * g.scale) &&
            height >= occlusion + jumpExtent + 8 * g.scale);
        unseen->setTooltip(pane.unseen > 0
                               ? "Jump to latest (" + std::to_string(pane.unseen) + " new messages)"
                               : "Jump to latest");
        unseen->layout(renderer);
        if (textDrag.armed && textDrag.active)
            moveTextSelection(textDrag.x, textDrag.y);
        visuals();
        markRead(*buffer);
    }
};

TranscriptView::TranscriptView(Model &model, PaneState &pane, TextClipboard &clipboard,
                               UiCommand command, FocusPane focus)
    : impl(std::make_unique<Impl>(*this, model, pane, clipboard, std::move(command),
                                  std::move(focus))) {}
TranscriptView::~TranscriptView() = default;
void TranscriptView::sync() { impl->sync(); }
void TranscriptView::setOnNickMenu(
    std::function<void(std::string, std::optional<InputArea::PointerData>)> callback) {
    impl->onNickMenu = std::move(callback);
}
void TranscriptView::setInputDispatcher(InputDispatcher &dispatcher) {
    if (impl->dispatcher != &dispatcher) {
        impl->cancelFocusReveal();
        impl->actionFocus.reset();
        impl->dispatcher = &dispatcher;
    }
}
void TranscriptView::noteFocusReveal() {
    if (auto *focused = impl->dispatcher ? impl->dispatcher->focusedArea() : nullptr;
        focused && !focused->tabFocusKey().empty())
        for (Node *node = focused; node; node = node->parent())
            if (node == this) {
                impl->focusRevealTarget = focused->tabFocusKey();
                break;
            }
}
void TranscriptView::cancelFocusReveal() { impl->cancelFocusReveal(); }
bool TranscriptView::handleKey(const InputArea::KeyData &key) { return impl->handleKey(key); }
bool TranscriptView::handleTypingKey(const InputArea::KeyData &key) {
    return impl->handleTypingKey(key);
}
bool TranscriptView::navigateMessage(int delta) { return impl->navigateMessage(delta); }
bool TranscriptView::scrollHalfPage(int direction) { return impl->scrollHalfPage(direction); }
bool TranscriptView::scrollToBoundary(bool newest) { return impl->scrollToBoundary(newest); }
bool TranscriptView::jumpToSourceMessage(std::string_view id) {
    return impl->jumpToSourceMessage(id);
}
std::optional<TranscriptMessageTarget> TranscriptView::selectedMessage() const {
    return impl->selectedMessage();
}
std::optional<TranscriptMessageTarget>
TranscriptView::resolveMessage(const TranscriptMessageTarget &target) const {
    return impl->resolveMessage(target);
}
bool TranscriptView::replyToMessage(const TranscriptMessageTarget &target) {
    return impl->replyToMessage(target);
}
bool TranscriptView::reactToMessage(const TranscriptMessageTarget &target, std::string emoji) {
    return impl->reactToMessage(target, std::move(emoji));
}
bool TranscriptView::copySelection() { return impl->copySelection(); }
bool TranscriptView::copySelectedMessage() { return impl->copySelectedMessage(); }
bool TranscriptView::keyboardTextSelection() const { return impl->keyboardSelection; }
bool TranscriptView::textSelectionCommand(std::string_view command) {
    return impl->textSelectionCommand(command);
}
bool TranscriptView::messageReadable(const std::string &key) const {
    const auto *buffer = impl->current();
    if (!buffer || !impl->cache || impl->active != impl->pane.buffer)
        return false;
    const auto found = impl->cache->keys.find(key);
    return found != impl->cache->keys.end() && impl->readable(*buffer, found->second);
}
void TranscriptView::setMediaPreviewController(MediaPreviewController *controller) {
    if (impl->mediaController == controller)
        return;
    for (const auto &[id, retained] : impl->caches)
        impl->retireMedia(retained);
    for (auto &slot : impl->slots)
        if (slot.preview && slot.content) {
            slot.content->removeChild(slot.preview);
            slot.preview = nullptr;
        }
    impl->mediaController = controller;
    invalidateMediaPreviews();
}
void TranscriptView::invalidateMediaPreviews() {
    for (auto &[buffer, cache] : impl->caches)
        for (auto &row : cache.rows)
            row.measured = false;
    markLayoutDirty();
}
void TranscriptView::clearTextSelection() { impl->clearTextSelection(); }
void TranscriptView::beforeBufferRename(std::string_view oldId, std::string_view newId) {
    impl->beforeBufferRename(oldId, newId);
}
void TranscriptView::remapBuffer(std::string_view oldId, std::string_view newId) {
    impl->remapBuffer(oldId, newId);
}
void TranscriptView::setReadEligible(bool eligible) {
    impl->readEligible = eligible;
    if (!eligible)
        for (const Node *node = this; node; node = node->parent())
            if (!node->visible()) {
                impl->cancelFocusReveal();
                impl->excludeRows();
                break;
            }
}
void TranscriptView::setWindowActive(bool active) {
    if (impl->windowActive != active) {
        impl->windowActive = active;
        markLayoutDirty();
    }
}
void TranscriptView::setComposerOrigin(float sceneX, float sceneY) {
    impl->composerOrigin = std::pair{sceneX, sceneY};
}
void TranscriptView::setContextInset(float typingInset, float typingRowHeight,
                                     float replyOverlayHeight) {
    if (impl->typingInset == typingInset && impl->typingRowHeight == typingRowHeight &&
        impl->replyOverlayHeight == replyOverlayHeight)
        return;
    impl->typingInset = typingInset;
    impl->typingRowHeight = typingRowHeight;
    impl->replyOverlayHeight = replyOverlayHeight;
    markLayoutDirty();
}
void TranscriptView::setMotionHost(Node *host) {
    if (impl->motionHost == host)
        return;
    for (auto &slot : impl->slots)
        impl->cancelEntrance(slot);
    impl->motionHost = host;
    markLayoutDirty();
}
void TranscriptView::doLayout(Renderer &renderer) { impl->layout(renderer); }
LayoutSize TranscriptView::doMeasure(Renderer &, const LayoutConstraints &constraints) {
    return constraints.constrain({width(), height()});
}
void TranscriptView::doArrange(Renderer &renderer, const LayoutRect &rect) {
    setPosition(rect.x, rect.y);
    setSize(rect.width, rect.height);
    impl->layout(renderer);
}
} // namespace blast
