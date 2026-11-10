#include "message_selection.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>

namespace blast {
namespace {

void cursorBoundaries(std::string_view text, std::vector<std::size_t> &offsets,
                      std::vector<PangoLogAttr> &attrs) {
    offsets.clear();
    const auto count = g_utf8_strlen(text.data(), static_cast<gssize>(text.size()));
    attrs.resize(static_cast<std::size_t>(count) + 1);
    pango_get_log_attrs(text.data(), static_cast<int>(text.size()), 0, pango_language_get_default(),
                        attrs.data(), static_cast<int>(attrs.size()));
    const char *cursor = text.data();
    for (std::size_t i = 0; i < attrs.size(); ++i) {
        if (attrs[i].is_cursor_position)
            offsets.push_back(static_cast<std::size_t>(cursor - text.data()));
        if (i + 1 < attrs.size())
            cursor = g_utf8_next_char(cursor);
    }
    // Empty text and both fragment edges always have insertion positions.
    if (offsets.empty() || offsets.front() != 0)
        offsets.insert(offsets.begin(), 0);
    if (offsets.back() != text.size())
        offsets.push_back(text.size());
}

bool characterBoundary(std::string_view text, std::size_t byte) {
    return byte <= text.size() &&
           (byte == text.size() || (static_cast<unsigned char>(text[byte]) & 0xc0) != 0x80);
}

float distanceTo(float point, float start, float size) {
    return std::max({start - point, 0.0F, point - start - size});
}

// Paragraph separators and standalone format controls have no painted cell.
// Native caret measurement deliberately gives these a virtual space-sized slot.
bool hasPaintedCharacter(std::string_view text) {
    for (const char *p = text.data(), *end = p + text.size(); p < end; p = g_utf8_next_char(p)) {
        const auto type = g_unichar_type(g_utf8_get_char(p));
        if (g_utf8_get_char(p) == '\t' ||
            (type != G_UNICODE_CONTROL && type != G_UNICODE_FORMAT &&
             type != G_UNICODE_LINE_SEPARATOR && type != G_UNICODE_PARAGRAPH_SEPARATOR))
            return true;
    }
    return false;
}

} // namespace

bool MessageSelectionText::update(std::string_view text) {
    if (text_ == text)
        return false;
    if (text.size() >= static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::length_error("message selection text exceeds Pango limits");
    if (!g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr))
        throw std::invalid_argument("message selection requires decoded UTF-8");
    text_.assign(text);
    cursorBoundaries(text_, boundaries_, attrs_);
    return true;
}

std::size_t MessageSelectionText::snap(std::size_t byte, MessageSelectionSnap bias) const {
    const auto next = std::lower_bound(boundaries_.begin(), boundaries_.end(), byte);
    if (next == boundaries_.end())
        return boundaries_.back();
    if (*next == byte || next == boundaries_.begin())
        return *next;
    const auto previous = *(next - 1);
    if (bias == MessageSelectionSnap::Before)
        return previous;
    if (bias == MessageSelectionSnap::After)
        return *next;
    return byte - previous <= *next - byte ? previous : *next;
}

void MessageSelectionFragment::rebuild(Renderer &renderer, const MessageSelectionText &body,
                                       std::size_t begin, std::size_t end, std::string_view markup,
                                       float fontSize, const TextFont &font, float wrapWidth,
                                       bool useMarkup) {
    const auto text = body.text();
    if (begin > end || !characterBoundary(text, begin) || !characterBoundary(text, end))
        throw std::invalid_argument("message selection fragment has invalid display range");
    const auto plain = text.substr(begin, end - begin);
    emptyHit_ = body.snap(begin);
    cells_.clear();
    cursorBoundaries(plain, offsets_, attrs_);
    // A substring can REGROUP regional indicators differently from the body.
    // Keep every global boundary too; native queries still describe the actual
    // shaped substring, including coincident cells for an indivisible glyph.
    const auto &global = body.boundaries();
    for (auto it = std::upper_bound(global.begin(), global.end(), begin);
         it != global.end() && *it < end; ++it)
        offsets_.push_back(*it - begin);
    std::sort(offsets_.begin(), offsets_.end());
    offsets_.erase(std::unique(offsets_.begin(), offsets_.end()), offsets_.end());
    renderer.measureTextCaretStops(markup, fontSize, offsets_, wrapWidth, stops_, font, useMarkup);
    metrics_ = renderer.measureText(markup, fontSize, font, wrapWidth, 0, TextAlign::Start,
                                    TextEllipsize::End, useMarkup);
    if (stops_.size() != offsets_.size())
        throw std::runtime_error("native message selection caret geometry is unavailable");
    if (plain.empty())
        return;

    cells_.reserve(offsets_.size() - 1);
    std::size_t i = 0;
    for (std::size_t paragraph = 0; paragraph < plain.size();) {
        int delimiter = 0, next = 0;
        pango_find_paragraph_boundary(plain.data() + paragraph,
                                      static_cast<int>(plain.size() - paragraph), &delimiter,
                                      &next);
        const auto paragraphEnd = paragraph + static_cast<std::size_t>(delimiter);
        const auto paragraphNext = paragraph + static_cast<std::size_t>(next);
        const auto first =
            std::lower_bound(offsets_.begin(), offsets_.end(), paragraph) - offsets_.begin();
        const auto last =
            std::lower_bound(offsets_.begin(), offsets_.end(), paragraphEnd) - offsets_.begin();
        // Resolve each paragraph independently, just like the native layout.
        // Native carets give the context fallback for all-neutral paragraphs.
        auto base =
            stops_[first].x > stops_[last].x ? PANGO_DIRECTION_WEAK_RTL : PANGO_DIRECTION_WEAK_LTR;
        const std::unique_ptr<guint8, decltype(&g_free)> levels(
            delimiter
                ? pango_log2vis_get_embedding_levels(plain.data() + paragraph, delimiter, &base)
                : nullptr,
            &g_free);
        if (delimiter && !levels)
            throw std::runtime_error("native message selection bidi analysis is unavailable");
        std::size_t character = 0;
        const char *cursor = plain.data() + paragraph;
        for (; i + 1 < offsets_.size() && offsets_[i] < paragraphNext; ++i) {
            while (cursor < plain.data() + offsets_[i]) {
                cursor = g_utf8_next_char(cursor);
                ++character;
            }
            const auto &stop = stops_[i];
            const auto piece = plain.substr(offsets_[i], offsets_[i + 1] - offsets_[i]);
            if (offsets_[i] >= paragraphEnd || !hasPaintedCharacter(piece) || stop.cellWidth <= 0 ||
                stop.height <= 0)
                continue;
            // Wrapped-away spaces have no occupied glyph advance. The SDK gives
            // their zero-width native cell a virtual caret width, which must not
            // become a selected character. Do not apply this to unwrapped tabs.
            const auto &nextStop = stops_[i + 1];
            if (wrapWidth > 0 && stop.cellIsVirtual &&
                g_unichar_isspace(g_utf8_get_char(piece.data())) &&
                (nextStop.lineIndex != stop.lineIndex || nextStop.x == stop.x))
                continue;
            // Style-cut pieces map to legal WHOLE-BODY cluster endpoints.
            cells_.push_back({{stop.cellX, stop.y, stop.cellWidth, stop.height},
                              body.snap(begin + offsets_[i], MessageSelectionSnap::Before),
                              body.snap(begin + offsets_[i + 1], MessageSelectionSnap::After),
                              (levels.get()[character] & 1) != 0});
        }
        paragraph = paragraphNext;
    }
}

std::size_t MessageSelectionFragment::hit(float localX, float localY) const {
    const Cell *nearest = nullptr;
    float bestY = std::numeric_limits<float>::infinity();
    float bestX = std::numeric_limits<float>::infinity();
    for (const auto &cell : cells_) {
        const auto &r = cell.rect;
        const float dy = distanceTo(localY, r.y, r.height);
        const float dx = distanceTo(localX, r.x, r.width);
        if (nearest && dy == bestY && dx == bestX && r.x == nearest->rect.x &&
            r.y == nearest->rect.y && r.width == nearest->rect.width &&
            r.height == nearest->rect.height && cell.rtl == nearest->rtl) {
            // A local grapheme can cross a legal global boundary (RI regrouping).
            // Coincident cells share outer edges; never invent glyph divisions.
            const bool trailing = (localX >= r.x + r.width * 0.5F) != cell.rtl;
            if ((trailing && cell.end > nearest->end) || (!trailing && cell.begin < nearest->begin))
                nearest = &cell;
        }
        if (dy < bestY || (dy == bestY && dx < bestX)) {
            nearest = &cell;
            bestY = dy;
            bestX = dx;
        }
    }
    if (!nearest)
        return emptyHit_;
    const bool rightHalf = localX >= nearest->rect.x + nearest->rect.width * 0.5F;
    return rightHalf != nearest->rtl ? nearest->end : nearest->begin;
}

void MessageSelectionFragment::rectangles(std::size_t begin, std::size_t end,
                                          std::vector<MessageSelectionRect> &out) const {
    out.clear();
    if (begin > end)
        std::swap(begin, end);
    if (begin == end)
        return;
    for (const auto &cell : cells_)
        if (cell.begin < end && cell.end > begin)
            out.push_back(cell.rect);
    std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) {
        if (a.y != b.y)
            return a.y < b.y;
        if (a.height != b.height)
            return a.height < b.height;
        return a.x < b.x;
    });
    std::size_t count = 0;
    for (const auto rect : out) {
        if (count && out[count - 1].y == rect.y && out[count - 1].height == rect.height &&
            rect.x <= out[count - 1].x + out[count - 1].width) {
            auto &last = out[count - 1];
            last.width = std::max(last.x + last.width, rect.x + rect.width) - last.x;
        } else {
            out[count++] = rect;
        }
    }
    out.resize(count);
}

} // namespace blast
