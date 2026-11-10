#include "../src/ui/emoji.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <glib.h>
#include <render/core/renderer.h>
#include <render/scene/input_area.h>
#include <string_view>

namespace {
InputArea *suggestionRow(Node &node) {
    if (auto *area = dynamic_cast<InputArea *>(&node); area && area->passiveTouchPress())
        return area;
    for (const auto &child : node.children())
        if (auto *area = suggestionRow(*child))
            return area;
    return nullptr;
}
void preservesInputFocus(const Node &node) {
    if (auto *area = dynamic_cast<const InputArea *>(&node))
        assert(!area->focusable() && !area->tabStop() && !area->focusOnPointerPress());
    for (const auto &child : node.children())
        preservesInputFocus(*child);
}
} // namespace

void checkEmojiCompletion() {
    using namespace blast::emoji;
    Catalog catalog;
    assert(catalog.size() == 1913);
    for (auto query : {"thumbsup", "+1", "THUMBSUP", "thumbs_up"}) {
        const auto matches = catalog.matches(query);
        assert(!matches.empty() && matches.front().glyph == "👍");
    }
    assert(catalog.matches("-1").front().glyph == "👎");
    const auto fuzzy = catalog.matches("smle");
    assert(!fuzzy.empty() && fuzzy.front().glyph == "😄");
    const auto held = fuzzy.front();
    const auto *cached = fuzzy.data();
    assert(catalog.matches("smle").data() == cached);
    assert(catalog.matches("heart").front().glyph == "❤️");
    // Returned string views survive replacement of the bounded result array.
    assert(held.glyph == "😄" && held.shortcode == "smile");
    const auto family = catalog.matches("family_man_woman_girl_boy");
    assert(!family.empty() && family.front().glyph == "👨‍👩‍👧‍👦");
    assert(catalog.matches("zzzzzzzzzzzzzzzzzzzzzzzzzzzz").empty());

    const auto bare = catalog.matches("");
    assert(bare.size() == maxResults && bare.front().glyph == "😄");
    std::array<Match, maxResults> stable;
    std::copy(bare.begin(), bare.end(), stable.begin());
    for (std::size_t i = 0; i < bare.size(); ++i) {
        assert(g_utf8_validate(bare[i].glyph.data(), bare[i].glyph.size(), nullptr));
        assert(g_utf8_validate(bare[i].name.data(), bare[i].name.size(), nullptr));
        assert(!bare[i].shortcode.empty());
        for (std::size_t j = 0; j < i; ++j)
            assert(bare[i].glyph != bare[j].glyph);
    }
    catalog.matches("smle");
    const auto again = catalog.matches("");
    for (std::size_t i = 0; i < again.size(); ++i)
        assert(again[i].glyph == stable[i].glyph);

    CompletionView view;
    view.setMatches(again, 0);
    view.setAnchor({280, 180, 200, 40}, {10, 20, 300, 220}, 1, TextFont{});
    view.setVisible(true);
    assert(view.visible() && view.x() >= 10 && view.y() >= 20);
    assert(view.x() + view.width() <= 310 && view.y() + view.height() <= 240);
    preservesInputFocus(view);
    auto *row = suggestionRow(view);
    assert(row);
    row->dispatchPress(1, 1, BTN_LEFT, true);
    assert(row->pressed());
    view.setMatches(catalog.matches("+1"), 0);
    assert(!row->pressed()); // A held tap must not activate the new binding on release.
    row->dispatchPress(1, 1, BTN_LEFT, true);
    view.setVisible(false);
    assert(!view.visible() && !row->pressed());
}
