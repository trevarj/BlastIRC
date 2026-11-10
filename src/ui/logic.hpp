#pragma once
#include "../irc/protocol.hpp"
#include "../model.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace blast::ui_logic {
inline float userScale(float scale) {
    return std::isfinite(scale) ? std::clamp(scale, .7f, 2.f) : 1.f;
}
// Anchor within the body proportionally on reflow; retain signed space before/after it.
inline double anchoredBodyScroll(double scroll, double oldBodyTop, double newBodyTop,
                                 double oldHeight, double newHeight, bool reflow) {
    double offset = scroll - oldBodyTop;
    if (reflow && offset > 0)
        offset = offset > oldHeight ? newHeight + offset - oldHeight
                 : oldHeight > 0    ? offset / oldHeight * newHeight
                                    : 0;
    return std::max(0., newBodyTop + offset);
}
inline std::string messageKey(const Message &msg) {
    // History labels identify whole batches, not messages. Only own live echoes need
    // their send label to bridge pending and acknowledged IDs. Application supplies
    // a unique local ID whenever the server omits msgid, and deduplicates repeated IDs.
    if (msg.own && !msg.history && !msg.label.empty())
        return "echo:" + msg.label;
    return "message:" + msg.id;
}
inline void remapBufferFocusKey(std::string &key, std::string_view oldID, std::string_view newID) {
    if (oldID == newID)
        return;
    if (key.starts_with("buffer:") && std::string_view(key).substr(7) == oldID) {
        key.replace(7, oldID.size(), newID);
        return;
    }
    const auto prefix = std::to_string(oldID.size()) + ":" + std::string(oldID);
    if (key.starts_with(prefix))
        key.replace(0, prefix.size(), std::to_string(newID.size()) + ":" + std::string(newID));
}
inline void remapPaneFocusKey(std::string &key, uint64_t oldID, uint64_t newID) {
    if (oldID == newID)
        return;
    for (std::string_view prefix : {"composer:", "buffer-tool:", "transcript:"}) {
        if (!key.starts_with(prefix))
            continue;
        const auto oldNumber = std::to_string(oldID);
        const auto suffix = std::string_view(key).substr(prefix.size());
        if (suffix.starts_with(oldNumber) && suffix.size() > oldNumber.size() &&
            suffix[oldNumber.size()] == ':')
            key.replace(prefix.size(), oldNumber.size(), std::to_string(newID));
        return;
    }
}
inline const Message *readTarget(const Buffer &buffer) {
    // Match Application::markRead; pending row continuity is not a read position.
    auto last =
        std::find_if(buffer.messages.rbegin(), buffer.messages.rend(),
                     [](const Message &message) { return !message.pending && !message.presence; });
    return last == buffer.messages.rend() ? nullptr : &*last;
}
inline bool hasUnreadMention(const Model &model, const Buffer &buffer) {
    if (buffer.unread <= 0)
        return false;
    const auto marker = irc::parseTimestampParts(buffer.readMarker);
    const auto seconds = marker ? marker->seconds : 0;
    const auto fraction = marker ? marker->nanoseconds : 0;
    const std::string_view stored = buffer.readMarker;
    const auto id = stored.starts_with("local:") ? stored.substr(6) : stored;
    bool after = stored.empty();
    if (!seconds && !after)
        after =
            std::none_of(buffer.messages.begin(), buffer.messages.end(), [&](const auto &message) {
                return message.id == stored || message.id == id;
            });
    for (const auto &message : buffer.messages) {
        if (!seconds && (message.id == stored || message.id == id)) {
            after = true;
            continue;
        }
        if (!message.mention || message.own || message.presence ||
            (!model.settings.foolUnread && isFool(model, buffer.server, message.nick)))
            continue;
        bool newer = after;
        if (seconds) {
            const auto time = irc::parseTimestampParts(message.time);
            newer = message.timestamp > seconds ||
                    (message.timestamp == seconds && (time ? time->nanoseconds : 0) > fraction);
        }
        if (newer)
            return true;
    }
    return false;
}
inline std::optional<unsigned> colorNumber(std::string_view text, size_t &offset) {
    unsigned value = 0, digits = 0;
    while (offset < text.size() && digits < 2 && text[offset] >= '0' && text[offset] <= '9') {
        value = value * 10 + static_cast<unsigned>(text[offset++] - '0');
        ++digits;
    }
    return digits ? std::optional<unsigned>(value) : std::nullopt;
}
inline std::optional<unsigned> hexColor(std::string_view text, size_t &offset) {
    if (offset + 6 > text.size())
        return std::nullopt;
    unsigned rgb = 0;
    for (size_t j = 0; j < 6; ++j) {
        const char c = text[offset + j];
        const int value = c >= '0' && c <= '9'   ? c - '0'
                          : c >= 'a' && c <= 'f' ? c - 'a' + 10
                          : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                 : -1;
        if (value < 0)
            return std::nullopt;
        rgb = (rgb << 4) | unsigned(value);
    }
    offset += 6;
    return rgb;
}
inline size_t previous(std::string_view s, size_t p) {
    if (!p)
        return 0;
    --p;
    while (p && (static_cast<unsigned char>(s[p]) & 0xc0) == 0x80)
        --p;
    return p;
}
inline size_t next(std::string_view s, size_t p) {
    if (p >= s.size())
        return s.size();
    ++p;
    while (p < s.size() && (static_cast<unsigned char>(s[p]) & 0xc0) == 0x80)
        ++p;
    return p;
}
inline bool word(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 128; }
inline size_t wordBack(std::string_view s, size_t p) {
    while (p && !word(s[previous(s, p)]))
        p = previous(s, p);
    while (p && word(s[previous(s, p)]))
        p = previous(s, p);
    return p;
}
inline size_t wordForward(std::string_view s, size_t p) {
    while (p < s.size() && !word(s[p]))
        p = next(s, p);
    while (p < s.size() && word(s[p]))
        p = next(s, p);
    return p;
}
inline int fuzzy(std::string_view needle, std::string_view haystack) {
    int score = 0, run = 0;
    size_t p = 0;
    for (unsigned char raw : needle) {
        unsigned char c = std::tolower(raw);
        bool found = false;
        for (; p < haystack.size(); ++p)
            if (std::tolower(static_cast<unsigned char>(haystack[p])) == c) {
                score += 10 + run * 4 + (p == 0 || !word(haystack[p - 1]) ? 12 : 0);
                ++run;
                ++p;
                found = true;
                break;
            } else {
                score -= 1;
                run = 0;
            }
        if (!found)
            return -100000;
    }
    return score - static_cast<int>(haystack.size() / 8);
}
enum class SidebarFilter { All, Unread, Dms, Archived };
inline bool sidebarMatches(const Buffer &buffer, std::string_view query, SidebarFilter filter) {
    return buffer.archived == (filter == SidebarFilter::Archived) &&
           (filter != SidebarFilter::Archived || buffer.kind == "channel" ||
            buffer.kind == "query") &&
           (filter != SidebarFilter::Unread || buffer.unread > 0) &&
           (filter != SidebarFilter::Dms || buffer.kind == "query") &&
           (fuzzy(query, buffer.target) > -100000 || fuzzy(query, buffer.server) > -100000);
}
inline char jumpHintKey(uint32_t sym) {
    if (sym >= 'A' && sym <= 'Z')
        return static_cast<char>(sym + ('a' - 'A'));
    if ((sym >= 'a' && sym <= 'z') || (sym >= '0' && sym <= '9'))
        return static_cast<char>(sym);
    return 0;
}
struct JumpHint {
    std::string buffer, keys;
};
inline std::vector<JumpHint> avyHints(const Model &model, std::string_view server,
                                      char reservedKey = 0) {
    char homeKeys[] = "asdfghjkl";
    const std::string_view home(homeKeys,
                                std::remove(homeKeys, homeKeys + 9, reservedKey) - homeKeys);
    struct Group {
        size_t count = 0, width = 0, next = 0;
    };
    std::array<Group, 128> groups{};
    std::vector<JumpHint> hints;
    hints.reserve(model.buffers.size());
    size_t fallback = 0;
    // Model's BufferID-keyed map gives a stable order independent of display names.
    for (const auto &[id, buffer] : model.buffers) {
        if (buffer.archived || buffer.server != server ||
            (buffer.kind != "channel" && buffer.kind != "query"))
            continue;
        char initial = 0;
        for (unsigned char c : buffer.target) {
            if (c >= 'A' && c <= 'Z')
                c += 'a' - 'A';
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
                initial = static_cast<char>(c);
                break;
            }
        }
        // A name initial reserved by the user's Cancel binding uses the home-key fallback too.
        if (!initial || initial == reservedKey)
            initial = home[fallback++ % home.size()];
        hints.push_back({id, std::string(1, initial)});
        ++groups[static_cast<unsigned char>(initial)].count;
    }
    for (auto &group : groups) {
        // ponytail: fixed-width home-key suffixes keep each initial's group prefix-free.
        for (size_t remaining = group.count ? group.count - 1 : 0; remaining;
             remaining /= home.size())
            ++group.width;
    }
    for (auto &hint : hints) {
        auto &group = groups[static_cast<unsigned char>(hint.keys.front())];
        hint.keys.resize(1 + group.width);
        size_t index = group.next++;
        for (size_t i = hint.keys.size(); i > 1;) {
            hint.keys[--i] = home[index % home.size()];
            index /= home.size();
        }
    }
    return hints;
}
inline void appendPreview(std::string_view text, std::string &out, size_t limit = 512) {
    // ponytail: scan at most 2048 input bytes per fragment and keep 512 UTF-8-safe output bytes.
    size_t end = std::min(text.size(), size_t(2048));
    if (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
        end = previous(text, end);
    text = text.substr(0, end);
    bool space = !out.empty() && out.back() == ' ';
    for (size_t i = 0; i < end && out.size() < limit;) {
        const unsigned char c = text[i];
        if (c == 3 || c == 4) {
            ++i;
            if (c == 3) {
                colorNumber(text, i);
                if (i + 1 < text.size() && text[i] == ',' && text[i + 1] >= '0' &&
                    text[i + 1] <= '9') {
                    ++i;
                    colorNumber(text, i);
                }
            } else if (hexColor(text, i) && i < text.size() && text[i] == ',') {
                const size_t comma = i++;
                if (!hexColor(text, i))
                    i = comma;
            }
        } else if (c == ' ' || (c >= 9 && c <= 13)) {
            if (!out.empty() && !space)
                out += ' ';
            space = true;
            ++i;
        } else if (c < 32 || c == 127)
            ++i;
        else {
            const size_t nextByte = next(text, i);
            if (nextByte > end || out.size() + nextByte - i > limit)
                break;
            out.append(text.data() + i, nextByte - i);
            space = false;
            i = nextByte;
        }
    }
}
inline bool sidebarPreview(const Model &model, const Buffer &buffer, std::string_view draft,
                           std::string &out) {
    out.clear();
    const bool hasDraft = !draft.empty();
    if (hasDraft) {
        out = "Draft: ";
        appendPreview(draft, out);
    } else if (buffer.kind != "channel" && buffer.kind != "query")
        out = buffer.kind == "search" ? "Search results" : "Server activity";
    else {
        // ponytail: inspect only the newest 64 loaded messages of a visible navigation row.
        size_t scanned = 0;
        for (auto it = buffer.messages.rbegin(); it != buffer.messages.rend() && scanned < 64;
             ++it, ++scanned) {
            if (it->deleted || it->presence ||
                (isFool(model, buffer.server, it->nick) &&
                 effectiveFoolDisplay(model.settings, buffer.id, buffer.target) !=
                     FoolDisplay::Shown &&
                 effectiveFoolDisplay(model.settings, buffer.id, buffer.target) !=
                     FoolDisplay::Dimmed))
                continue;
            if (it->pending)
                out = "[Pending] ";
            if (it->action) {
                out += "* ";
                appendPreview(it->nick, out, 511);
                out += ' ';
            } else {
                appendPreview(it->own ? std::string_view("You") : std::string_view(it->nick), out,
                              510);
                out += ": ";
            }
            appendPreview(it->text, out);
            break;
        }
        if (out.empty())
            out = "No messages yet";
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return hasDraft;
}
inline std::string canonical(std::string_view chord) {
    std::string result, token;
    auto flush = [&] {
        if (token.empty())
            return;
        for (char &c : token)
            c = std::tolower(static_cast<unsigned char>(c));
        if (token == "enter" || token == "return")
            token = "ret";
        if (token == "space")
            token = "spc";
        if (!result.empty())
            result += ' ';
        result += token;
        token.clear();
    };
    for (char c : chord) {
        if (std::isspace(static_cast<unsigned char>(c)))
            flush();
        else
            token += c;
    }
    flush();
    return result;
}
inline bool bindingConflicts(std::string_view first, std::string_view second) {
    auto a = canonical(first), b = canonical(second);
    if (a.empty() || b.empty())
        return false;
    if (a.size() > b.size())
        a.swap(b);
    return b.starts_with(a) && (a.size() == b.size() || b[a.size()] == ' ');
}
template <class Bindings, class HasSavedBinding>
inline void migrateMessageBindings(Bindings &bindings, HasSavedBinding hasSavedBinding) {
    constexpr std::array messageNames = {"message-prev", "message-next", "message-reply",
                                         "message-react"};
    for (const auto &[name, stock, message] :
         std::array<std::array<const char *, 3>, 3>{{{"previous-line", "C-p", "message-prev"},
                                                     {"next-line", "C-n", "message-next"},
                                                     {"end-of-line", "C-e", "message-react"}}}) {
        auto it = bindings.find(name);
        if (it == bindings.end() || canonical(it->second) != canonical(stock))
            continue;
        // Permit later legacy rebindings unless they steal an explicit message shortcut.
        if (hasSavedBinding(message) &&
            std::none_of(messageNames.begin(), messageNames.end(), [&](const char *action) {
                auto saved = bindings.find(action);
                return hasSavedBinding(action) && saved != bindings.end() &&
                       bindingConflicts(it->second, saved->second);
            }))
            continue;
        it->second.clear();
    }
    for (const auto &[name, stock] :
         std::array<std::pair<const char *, const char *>, 4>{{{"message-prev", "C-p"},
                                                               {"message-next", "C-n"},
                                                               {"message-reply", "C-r"},
                                                               {"message-react", "C-e"}}}) {
        if (hasSavedBinding(name))
            continue;
        const bool conflict = std::any_of(bindings.begin(), bindings.end(), [&](const auto &entry) {
            return entry.first != name && bindingConflicts(stock, entry.second);
        });
        bindings.insert_or_assign(name, conflict ? "" : stock);
    }
}
struct Snapshot {
    std::string text;
    size_t cursor = 0, anchor = 0;
    bool mark = false;
};
struct Editor {
    std::string text;
    size_t cursor = 0, anchor = 0;
    bool mark = false;
    std::vector<Snapshot> undo;
    void selection(size_t caret, size_t start, size_t end) {
        caret = std::min(caret, text.size());
        start = std::min(start, text.size());
        end = std::min(end, text.size());
        // Keep a zero-width Emacs mark until an actual mouse/native selection changes it.
        if (caret == cursor && start == (mark ? anchor : cursor) && end == cursor)
            return;
        cursor = caret;
        anchor = start == caret ? end : start;
        mark = start != end;
    }
    void remember() {
        if (undo.empty() || undo.back().text != text) {
            undo.push_back({text, cursor, anchor, mark});
            if (undo.size() > 100)
                undo.erase(undo.begin());
        }
    }
    void replace(size_t a, size_t b, std::string_view value) {
        a = std::min(a, text.size());
        b = std::min(b, text.size());
        text.replace(a, b - a, value);
        cursor = a + value.size();
        anchor = cursor;
        mark = false;
    }
    size_t insertedSize(std::string_view value) const {
        size_t a = std::min(cursor, text.size()), b = std::min(anchor, text.size());
        return text.size() - (mark ? std::max(a, b) - std::min(a, b) : 0) + value.size();
    }
    void insert(std::string_view value) {
        cursor = std::min(cursor, text.size());
        anchor = std::min(anchor, text.size());
        size_t a = mark ? std::min(cursor, anchor) : cursor,
               b = mark ? std::max(cursor, anchor) : cursor;
        if (std::string_view(text).substr(a, b - a) != value)
            remember();
        replace(a, b, value);
    }
    // Boundaries come from the displayed text's native grapheme cursor stops.
    std::optional<std::array<size_t, 3>> transposition(std::span<const size_t> boundaries) const {
        if ((mark && cursor != anchor) || boundaries.size() < 3)
            return std::nullopt;
        auto middle = std::lower_bound(boundaries.begin(), boundaries.end(), cursor);
        if (middle == boundaries.end() || *middle != cursor || middle == boundaries.begin())
            return std::nullopt;
        if (middle + 1 == boundaries.end() ||
            std::string_view(text).substr(*middle, *(middle + 1) - *middle).find('\n') !=
                std::string_view::npos)
            --middle;
        if (middle == boundaries.begin() || middle + 1 == boundaries.end())
            return std::nullopt;
        const size_t start = *(middle - 1), split = *middle, end = *(middle + 1);
        if (std::string_view(text).substr(start, end - start).find('\n') != std::string_view::npos)
            return std::nullopt;
        return std::array{start, split, end};
    }
    bool transpose(std::span<const size_t> boundaries) {
        const auto range = transposition(boundaries);
        if (!range)
            return false;
        const auto [start, split, end] = *range;
        remember();
        std::rotate(text.begin() + start, text.begin() + split, text.begin() + end);
        cursor = anchor = end;
        mark = false;
        return true;
    }
    void command(std::string_view op, std::string &kill) {
        cursor = std::min(cursor, text.size());
        anchor = std::min(anchor, text.size());
        size_t to = cursor;
        if (op == "backward-char")
            to = previous(text, cursor);
        else if (op == "forward-char")
            to = next(text, cursor);
        else if (op == "backward-word")
            to = wordBack(text, cursor);
        else if (op == "forward-word")
            to = wordForward(text, cursor);
        else if (op == "beginning-of-line") {
            auto p = text.rfind('\n', cursor ? cursor - 1 : 0);
            to = cursor && p != std::string::npos ? p + 1 : 0;
        } else if (op == "end-of-line") {
            auto p = text.find('\n', cursor);
            to = p == std::string::npos ? text.size() : p;
        } else if (op == "beginning-of-buffer")
            to = 0;
        else if (op == "end-of-buffer")
            to = text.size();
        else if (op == "previous-line" || op == "next-line") {
            auto start = text.rfind('\n', cursor ? cursor - 1 : 0);
            start = cursor && start != std::string::npos ? start + 1 : 0;
            size_t col = 0;
            for (size_t p = start; p < cursor; p = next(text, p))
                ++col;
            size_t limit = text.size();
            if (op == "previous-line") {
                if (!start) {
                    to = 0;
                    col = 0;
                } else {
                    size_t prev = start > 1 ? text.rfind('\n', start - 2) : std::string::npos;
                    to = prev == std::string::npos ? 0 : prev + 1;
                    limit = start - 1;
                }
            } else {
                size_t end = text.find('\n', cursor);
                if (end == std::string::npos) {
                    to = text.size();
                    col = 0;
                } else {
                    to = end + 1;
                    size_t following = text.find('\n', to);
                    limit = following == std::string::npos ? text.size() : following;
                }
            }
            while (col-- && to < limit)
                to = next(text, to);
        } else if (op == "set-mark") {
            anchor = cursor;
            mark = !mark;
            return;
        } else if (op == "cancel") {
            mark = false;
            anchor = cursor;
            return;
        } else if (op == "undo") {
            if (!undo.empty()) {
                text = undo.back().text;
                cursor = undo.back().cursor;
                anchor = undo.back().anchor;
                mark = undo.back().mark;
                undo.pop_back();
            }
            return;
        } else if (op == "yank") {
            insert(kill);
            return;
        } else if (op == "kill-region" || op == "copy-region") {
            if (mark && cursor != anchor) {
                size_t a = std::min(cursor, anchor), b = std::max(cursor, anchor);
                kill = text.substr(a, b - a);
                if (op == "kill-region") {
                    remember();
                    replace(a, b, "");
                } else
                    mark = false;
            }
            return;
        } else if (op == "kill-line") {
            size_t end = text.find('\n', cursor);
            if (end == std::string::npos)
                end = text.size();
            else if (end == cursor)
                ++end;
            if (end == cursor)
                return;
            kill = text.substr(cursor, end - cursor);
            remember();
            replace(cursor, end, "");
            return;
        } else if (op == "backward-kill-line") {
            const auto newline = cursor ? text.rfind('\n', cursor - 1) : std::string::npos;
            size_t start = newline == std::string::npos ? 0 : newline + 1;
            if (start == cursor && cursor)
                --start;
            if (start == cursor)
                return;
            kill = text.substr(start, cursor - start);
            remember();
            replace(start, cursor, "");
            return;
        } else if (op == "kill-word" || op == "backward-kill-word") {
            size_t a = op == "kill-word" ? cursor : wordBack(text, cursor),
                   b = op == "kill-word" ? wordForward(text, cursor) : cursor;
            if (a == b)
                return;
            kill = text.substr(a, b - a);
            remember();
            replace(a, b, "");
            return;
        } else if (op == "delete-char" || op == "backward-delete") {
            size_t a = cursor, b = cursor;
            if (mark && anchor != cursor) {
                a = std::min(anchor, cursor);
                b = std::max(anchor, cursor);
            } else if (op == "delete-char")
                b = next(text, cursor);
            else
                a = previous(text, cursor);
            if (a == b)
                return;
            remember();
            replace(a, b, "");
            return;
        }
        cursor = to;
        if (!mark)
            anchor = cursor;
    }
};
inline constexpr size_t composerLimit = 65536;
// Reject malformed/scalar-invalid UTF-8 and transport controls, never truncate a paste.
// IRC formatting controls are intentional text; NUL/CR/DEL are not.
inline bool composerTextValid(std::string_view text) {
    for (size_t i = 0; i < text.size();) {
        const unsigned char lead = text[i++];
        if (lead < 0x80) {
            if (lead == 0x7f || (lead < 0x20 && lead != '\n' && lead != '\t' && lead != 2 &&
                                 lead != 3 && lead != 4 && lead != 15 && lead != 17 && lead != 22 &&
                                 lead != 29 && lead != 30 && lead != 31))
                return false;
            continue;
        }
        unsigned value = 0, count = 0, minimum = 0;
        if (lead >= 0xc2 && lead <= 0xdf) {
            value = lead & 0x1f;
            count = 1;
            minimum = 0x80;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            value = lead & 0x0f;
            count = 2;
            minimum = 0x800;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            value = lead & 7;
            count = 3;
            minimum = 0x10000;
        } else
            return false;
        if (count > text.size() - i)
            return false;
        while (count--) {
            const unsigned char c = text[i++];
            if ((c & 0xc0) != 0x80)
                return false;
            value = (value << 6) | (c & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            return false;
    }
    return true;
}
inline std::string clipboardNewlines(std::string text) {
    // A CRLF clipboard selection is text, not an IRC transport command.
    size_t out = 0;
    for (size_t i = 0; i < text.size(); ++i)
        if (text[i] != '\r' || i + 1 == text.size() || text[i + 1] != '\n')
            text[out++] = text[i];
    text.resize(out);
    return text;
}
inline bool composerInsertAllowed(const Editor &editor, std::string_view text) {
    return editor.insertedSize(text) < composerLimit;
}
inline bool composerEditAllowed(std::string_view before, std::string_view after) {
    if (after.size() < composerLimit)
        return true;
    if (after.size() >= before.size())
        return false;
    // Oversized recovered text remains deletable, but cannot acquire new text.
    size_t kept = 0;
    for (char c : before)
        if (kept < after.size() && after[kept] == c)
            ++kept;
    return kept == after.size();
}
inline bool reconcileDraft(Editor &editor, std::string &published, std::string &modelDraft) {
    bool recovered = false;
    if (modelDraft != published && !modelDraft.empty() && modelDraft != editor.text) {
        // Application::restoreDraft prepends recovery to the last published draft.
        std::string_view prefix = modelDraft;
        if (!published.empty() && prefix.size() > published.size() && prefix.ends_with(published) &&
            prefix[prefix.size() - published.size() - 1] == '\n')
            prefix.remove_suffix(published.size() + 1);
        editor.remember();
        const size_t cursor = std::min(editor.cursor, editor.text.size()),
                     anchor = std::min(editor.anchor, editor.text.size());
        const size_t shift = prefix.size() + !editor.text.empty();
        editor.text.insert(0, shift, '\n');
        editor.text.replace(0, prefix.size(), prefix);
        editor.cursor = cursor + shift;
        editor.anchor = anchor + shift;
        recovered = true;
    }
    if (modelDraft != editor.text)
        modelDraft = editor.text;
    if (published != modelDraft)
        published = modelDraft;
    return recovered;
}
} // namespace blast::ui_logic
