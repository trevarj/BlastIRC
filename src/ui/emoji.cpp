#include "emoji.hpp"

#include "../theme/theme.hpp"
#include "logic.hpp"
#include <algorithm>
#include <array>
#include <core/files/resource_paths.h>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <render/core/renderer.h>
#include <render/scene/input_area.h>
#include <render/scene/rect_node.h>
#include <stdexcept>
#include <string>
#include <ui/controls/label.h>
#include <ui/controls/scroll_view.h>
#include <utility>
#include <vector>

namespace blast::emoji {
namespace {
struct Alias {
    std::string_view glyph, shortcode;
};
// Common shortcode spellings supplement (rather than replace) the SDK's names and keywords.
constexpr Alias aliases[] = {
    {"👍", "thumbsup"}, {"👍", "+1"},          {"👎", "thumbsdown"},
    {"👎", "-1"},       {"😄", "smile"},       {"😁", "grin"},
    {"😂", "joy"},      {"🤣", "rofl"},        {"😊", "blush"},
    {"❤️", "heart"},     {"😢", "cry"},         {"😭", "sob"},
    {"😆", "laughing"}, {"😅", "sweat_smile"}, {"😎", "sunglasses"},
    {"😉", "wink"},     {"😍", "heart_eyes"},  {"😘", "kissing_heart"},
    {"🤔", "thinking"}, {"🎉", "tada"},        {"✅", "white_check_mark"},
    {"❌", "x"},        {"👋", "wave"},        {"🙏", "pray"},
    {"👌", "ok_hand"},  {"👏", "clap"},        {"💯", "100"},
};
constexpr std::string_view defaults[] = {"😄", "👍", "😂", "❤️", "🎉", "🔥", "🤔", "👀"};

void normalize(std::string_view text, std::string &out) {
    out.clear();
    for (unsigned char c : text) {
        if (c >= 'A' && c <= 'Z')
            out += char(c + ('a' - 'A'));
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 128 || c == '+' ||
                 (c == '-' && out.empty() && text.size() == 2 && text[1] == '1'))
            out += char(c);
        else if (!out.empty() && out.back() != '_')
            out += '_';
    }
    if (!out.empty() && out.back() == '_')
        out.pop_back();
}

std::string normalized(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    normalize(text, result);
    return result;
}

int termScore(std::string_view query, std::string_view term) {
    if (query == term)
        return 30000;
    const int score = ui_logic::fuzzy(query, term);
    if (score <= -100000)
        return -100000;
    return score + (term.starts_with(query) ? 20000 : 0);
}

template <class T> T *child(Node &parent) {
    auto node = std::make_unique<T>();
    auto *result = node.get();
    parent.addChild(std::move(node));
    return result;
}

void preserveFocus(Node &node) {
    if (auto *area = dynamic_cast<InputArea *>(&node)) {
        area->setFocusable(false);
        area->setTabStop(false);
        area->setFocusOnPointerPress(false);
    }
    for (const auto &item : node.children())
        preserveFocus(*item);
}

void cancelPresses(Node &node) {
    if (auto *area = dynamic_cast<InputArea *>(&node))
        area->dispatchCancel();
    for (const auto &item : node.children())
        cancelPresses(*item);
}

class SuggestionRow final : public InputArea {
  public:
    SuggestionRow() {
        setFocusable(false);
        setTabStop(false);
        setFocusOnPointerPress(false);
        setPassiveTouchPress(true);
        setClipChildren(true);
        background = child<RectNode>(*this);
        background->setHitTestVisible(false);
        glyph = child<Label>(*this);
        shortcode = child<Label>(*this);
        name = child<Label>(*this);
        for (auto *label : {glyph, shortcode, name}) {
            label->setUseMarkup(false);
            label->setMaxLines(1);
            label->setHitTestVisible(false);
        }
        setOnEnter([this](const auto &) { paint(); });
        setOnLeave([this] { paint(); });
        setOnPress([this](const auto &) { paint(); });
        setOnCancel([this] { paint(); });
    }

    void bind(Match value, bool selected) {
        if (bound.glyph != value.glyph || bound.shortcode != value.shortcode ||
            bound.name != value.name)
            dispatchCancel();
        bound = value;
        active = selected;
        glyph->setText(value.glyph);
        if (shortcodeValue != value.shortcode) {
            shortcodeValue = value.shortcode;
            caption.clear();
            caption += ':';
            caption += value.shortcode;
            caption += ':';
            shortcode->setText(caption);
        }
        name->setText(value.name);
        paint();
    }

    void style(float scale, const TextFont &font) {
        zoom = scale;
        for (auto *label : {glyph, shortcode, name}) {
            if (label->font().family != font.family)
                label->setFontFamily(font.family);
            if (label->font().weight != font.weight)
                label->setFontWeight(font.weight);
            if (label->font().features != font.features)
                label->setFontFeatures(font.features);
        }
        glyph->setFontSize(24 * zoom);
        shortcode->setFontSize(13 * zoom);
        name->setFontSize(11 * zoom);
        paint();
        markLayoutDirty();
    }

    Match bound{};

  protected:
    LayoutSize doMeasure(Renderer &, const LayoutConstraints &constraints) override {
        return constraints.constrain(
            {constraints.hasMaxWidth ? constraints.maxWidth : 320 * zoom, 44 * zoom});
    }
    void doLayout(Renderer &renderer) override {
        background->setSize(width(), height());
        const float textX = 48 * zoom;
        const float textWidth = std::max(0.f, width() - textX - 8 * zoom);
        glyph->setMaxWidth(34 * zoom);
        glyph->measure(renderer);
        glyph->setPosition(8 * zoom + (30 * zoom - glyph->width()) / 2,
                           (height() - glyph->height()) / 2);
        shortcode->setMaxWidth(textWidth);
        name->setMaxWidth(textWidth);
        shortcode->measure(renderer);
        name->measure(renderer);
        const float textHeight = shortcode->height() + name->height();
        shortcode->setPosition(textX, (height() - textHeight) / 2);
        name->setPosition(textX, shortcode->y() + shortcode->height());
    }

  private:
    void paint() {
        const bool highlight = active || pressed();
        const bool hover = hovered() && !highlight;
        RoundedRectStyle style{};
        style.fill = highlight ? themeSurfaceColor("primary_container")
                     : hover   ? themeSurfaceColor("hover")
                               : Color{0, 0, 0, 0};
        style.radius = themeCornerRadius();
        background->setStyle(style);
        const auto foreground = themeForegroundColor(highlight ? "on_primary_container"
                                                     : hover   ? "on_hover"
                                                               : "on_surface");
        glyph->setColor(foreground);
        shortcode->setColor(foreground);
        name->setColor(highlight || hover ? foreground
                                          : themeForegroundColor("on_surface_variant"));
    }
    RectNode *background;
    Label *glyph, *shortcode, *name;
    std::string_view shortcodeValue;
    std::string caption;
    float zoom = 1;
    bool active = false;
};
} // namespace

struct Catalog::Impl {
    struct Entry {
        std::string glyph, name, shortcode;
        std::vector<std::string> keywords;
        std::vector<std::string_view> aliases;
        Match match() const {
            return {glyph, aliases.empty() ? std::string_view(shortcode) : aliases.front(), name};
        }
    };
    std::vector<Entry> entries;
    std::array<Match, maxResults> results{};
    std::size_t count = 0;
    std::string cachedRaw, query;
    bool cached = false;

    Impl() {
        const auto path = paths::assetPath("emoji.json");
        try {
            std::ifstream file(path);
            if (!file)
                throw std::runtime_error("cannot open catalog");
            const auto json = nlohmann::json::parse(file);
            if (!json.is_array())
                throw std::runtime_error("catalog is not an array");
            entries.reserve(json.size());
            for (const auto &item : json) {
                Entry entry;
                entry.glyph = item.at("emoji").get<std::string>();
                entry.name = item.at("name").get<std::string>();
                if (entry.glyph.empty() || entry.name.empty())
                    throw std::runtime_error("empty catalog entry");
                entry.shortcode = normalized(entry.name);
                for (const auto &keyword : item.at("keywords"))
                    entry.keywords.push_back(normalized(keyword.get_ref<const std::string &>()));
                for (const auto &alias : aliases)
                    if (alias.glyph == entry.glyph)
                        entry.aliases.push_back(alias.shortcode);
                entries.push_back(std::move(entry));
            }
        } catch (const std::exception &error) {
            entries.clear();
            std::cerr << "BlastIRC: emoji catalog " << path << ": " << error.what() << '\n';
        }
    }

    std::span<const Match> matches(std::string_view raw) {
        if (cached && cachedRaw == raw)
            return {results.data(), count};
        cachedRaw = raw;
        cached = true;
        normalize(raw, query);
        count = 0;
        if (query.empty()) {
            if (!raw.empty())
                return {};
            for (auto glyph : defaults) {
                const auto found =
                    std::find_if(entries.begin(), entries.end(),
                                 [glyph](const Entry &entry) { return entry.glyph == glyph; });
                if (found != entries.end())
                    results[count++] = found->match();
            }
            for (const auto &entry : entries) {
                if (count == maxResults)
                    break;
                if (std::none_of(results.begin(), results.begin() + count,
                                 [&](const Match &match) { return match.glyph == entry.glyph; }))
                    results[count++] = entry.match();
            }
            return {results.data(), count};
        }
        struct Scored {
            int score;
            const Entry *entry;
        };
        std::array<Scored, maxResults> best{};
        // ponytail: scan the fixed 1,913-entry catalog only when the query changes; keep just eight
        // winners.
        for (const auto &entry : entries) {
            int score = -100000;
            const auto consider = [&](std::string_view term, int priority) {
                const auto value = termScore(query, term);
                if (value > -100000)
                    score = std::max(score, value + priority);
            };
            consider(entry.shortcode, 100);
            for (auto alias : entry.aliases)
                consider(alias, 200);
            for (const auto &keyword : entry.keywords)
                consider(keyword, 0);
            if (score <= -100000)
                continue;
            std::size_t at = 0;
            // Source order is the stable tie-breaker, independent of hash iteration or locale.
            while (at < count && best[at].score >= score)
                ++at;
            if (at == maxResults)
                continue;
            count = std::min(count + 1, maxResults);
            for (std::size_t next = count - 1; next > at; --next)
                best[next] = best[next - 1];
            best[at] = {score, &entry};
        }
        for (std::size_t i = 0; i < count; ++i)
            results[i] = best[i].entry->match();
        return {results.data(), count};
    }
};

Catalog::Catalog() : impl_(std::make_unique<Impl>()) {}
Catalog::~Catalog() = default;
std::span<const Match> Catalog::matches(std::string_view query) { return impl_->matches(query); }
std::size_t Catalog::size() const { return impl_->entries.size(); }

struct CompletionView::Impl {
    CompletionView &view;
    ScrollView *scroll;
    std::array<SuggestionRow *, maxResults> rows{};
    std::size_t count = 0, selected = 0;
    LayoutRect anchor{}, viewport{};
    float scale = 1;
    TextFont font{};
    std::uint64_t revision = 0;
    bool styled = false;
    std::function<void(std::size_t)> onChoose;

    explicit Impl(CompletionView &owner) : view(owner), scroll(child<ScrollView>(owner)) {
        scroll->content()->setAlign(FlexAlign::Stretch);
        scroll->setReserveScrollbarGutter(true);
        for (std::size_t i = 0; i < rows.size(); ++i) {
            rows[i] = child<SuggestionRow>(*scroll->content());
            rows[i]->setVisible(false);
            rows[i]->setOnClick([this, i](const auto &) {
                if (view.visible() && i < count && onChoose)
                    onChoose(i);
            });
        }
        preserveFocus(view);
    }

    void place(bool reveal = false) {
        const float margin =
            std::max(0.f, std::min({4 * scale, viewport.width / 4, viewport.height / 4}));
        const float left = viewport.x + margin, top = viewport.y + margin;
        const float right = viewport.x + viewport.width - margin;
        const float bottom = viewport.y + viewport.height - margin;
        const float above = std::max(0.f, std::min(anchor.y - margin, bottom) - top);
        const float below =
            std::max(0.f, bottom - std::max(anchor.y + anchor.height + margin, top));
        const float desired = count * 44 * scale + 10 * scale;
        const bool useAbove = above >= desired || above >= below;
        const float height = std::min(desired, useAbove ? above : below);
        const float width = std::max(0.f, std::min(360 * scale, right - left));
        const float y = useAbove ? anchor.y - margin - height : anchor.y + anchor.height + margin;
        view.setPosition(std::clamp(anchor.x, left, std::max(left, right - width)),
                         std::clamp(y, top, std::max(top, bottom - height)));
        const bool resized = view.width() != width || view.height() != height;
        view.setSize(width, height);
        if (count && (resized || reveal))
            scroll->requestScrollIntoView(rows[selected]);
        if (width <= 0 || height <= 0)
            view.setVisible(false);
    }
};

CompletionView::CompletionView() : impl_(std::make_unique<Impl>(*this)) {
    setParticipatesInLayout(false);
    setExcludeSubtreeFromTabOrder(true);
    setClipChildren(true);
    setZIndex(90);
    Node::setVisible(false);
}
CompletionView::~CompletionView() = default;

void CompletionView::setMatches(std::span<const Match> matches, std::size_t selected) {
    auto &state = *impl_;
    const auto count = std::min(matches.size(), maxResults);
    const auto nextSelected = count ? std::min(selected, count - 1) : 0;
    bool changed = count != state.count;
    for (std::size_t i = 0; i < count; ++i)
        changed = changed || state.rows[i]->bound.glyph != matches[i].glyph;
    if (changed)
        cancelPresses(*state.scroll);
    for (std::size_t i = 0; i < maxResults; ++i) {
        if (i < count)
            state.rows[i]->bind(matches[i], i == nextSelected);
        else if (state.rows[i]->visible())
            state.rows[i]->dispatchCancel();
        state.rows[i]->setVisible(i < count);
    }
    const bool reveal = changed || nextSelected != state.selected;
    state.count = count;
    state.selected = nextSelected;
    if (changed)
        state.scroll->requestScrollToOffset(0);
    if (count && reveal)
        state.scroll->requestScrollIntoView(state.rows[nextSelected]);
    if (!count)
        setVisible(false);
    state.place();
}

void CompletionView::setAnchor(LayoutRect anchor, LayoutRect viewport, float scale,
                               const TextFont &font) {
    auto &state = *impl_;
    state.anchor = anchor;
    state.viewport = viewport;
    scale = std::max(.1f, scale);
    const auto revision = themeRevision();
    const bool restyled =
        !state.styled || state.scale != scale || state.font != font || state.revision != revision;
    if (restyled) {
        state.styled = true;
        state.scale = scale;
        state.font = font;
        state.revision = revision;
        for (auto *row : state.rows)
            row->style(scale, font);
        state.scroll->setContentScale(scale);
        state.scroll->setViewportPaddingH(4 * scale);
        state.scroll->setViewportPaddingV(4 * scale);
        state.scroll->setFill(themeSurfaceColor("surface_container_high"));
        state.scroll->setBorder(themeForegroundColor("outline_variant"), scale);
        state.scroll->setRadius(themeCornerRadius());
        state.scroll->setScrollbarInsetV(4 * scale);
    }
    state.place(restyled);
}

void CompletionView::setOnChoose(std::function<void(std::size_t)> callback) {
    impl_->onChoose = std::move(callback);
}
void CompletionView::setVisible(bool visible) {
    if (!visible)
        cancelPresses(*impl_->scroll);
    Node::setVisible(visible && impl_->count && width() > 0 && height() > 0);
}
void CompletionView::doLayout(Renderer &renderer) {
    impl_->scroll->setSize(width(), height());
    impl_->scroll->layout(renderer);
}
} // namespace blast::emoji
