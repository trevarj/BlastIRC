#include "../src/render/graphics.hpp"
#include "../src/theme/theme.hpp"
#include "../src/ui/irc_format.hpp"
#include "../src/ui/media_preview.hpp"
#include "../src/ui/message_selection.hpp"
#include "../src/ui/presentation.hpp"
#include "../src/ui/transcript.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <core/input/key_modifiers.h>
#include <core/text_clipboard.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <glib.h>
#include <iterator>
#include <limits>
#include <render/animation/animation_manager.h>
#include <render/animation/motion_service.h>
#include <render/backend/render_backend.h>
#include <render/codecs/codecs.h>
#include <render/core/renderer.h>
#include <render/core/texture_manager.h>
#include <render/scene/glyph_node.h>
#include <render/scene/input_dispatcher.h>
#include <render/scene/rect_node.h>
#include <render/scene/text_node.h>
#include <render/text/cairo_text_renderer.h>
#include <render/text/glyph_registry.h>
#include <source_location>
#include <thread>
#include <tuple>
#include <ui/controls/button.h>
#include <ui/controls/glyph.h>
#include <ui/controls/image.h>
#include <ui/controls/input.h>
#include <ui/controls/label.h>
#include <ui/controls/scroll_view.h>
#include <ui/controls/select_popup_context.h>
#include <ui/controls/spinner.h>
#include <ui/style.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
// Native decoding/layout is real; only GPU upload is discarded. Retained RGBA
// fingerprints let avatar consumers check identity without claiming GPU-pixel proof.
class CpuTextureUploadSink final : public TextureManager {
  public:
    TextureHandle loadFromRgba(const std::uint8_t *data, int width, int height, bool) override {
        auto handle = upload(data, width, height);
        uint64_t hash = 14695981039346656037ull;
        for (size_t i = 0; i < size_t(width) * size_t(height) * 4; ++i)
            hash = (hash ^ data[i]) * 1099511628211ull;
        rgbaHashes.emplace_back(handle.id, hash);
        return handle;
    }
    TextureHandle loadFromRaw(const std::uint8_t *data, std::size_t, int width, int height, int,
                              PixmapFormat, bool) override {
        return upload(data, width, height);
    }
    TextureHandle loadFromPixels(const std::uint8_t *data, int width, int height, TextureDataFormat,
                                 TextureFilter, bool) override {
        return upload(data, width, height);
    }
    TextureHandle createEmpty(int, int, TextureDataFormat, TextureFilter) override { std::abort(); }
    bool replace(TextureHandle &, const std::uint8_t *, int, int, TextureDataFormat, TextureFilter,
                 bool) override {
        std::abort();
    }
    bool updateSubImage(TextureHandle &, const std::uint8_t *, int, int, int, int,
                        TextureDataFormat) override {
        std::abort();
    }
    void unload(TextureHandle &handle) override { handle = {}; }
    void cleanup() override {}
    void abandonGpuResources() noexcept override {}
    void probeExtensions() override { std::abort(); }
    std::uint32_t uploads = 0;
    std::vector<std::pair<TextureId, uint64_t>> rgbaHashes;
    uint64_t rgbaHash(TextureId id) const {
        const auto found = std::find_if(rgbaHashes.begin(), rgbaHashes.end(),
                                        [id](const auto &entry) { return entry.first == id; });
        assert(found != rgbaHashes.end());
        return found->second;
    }

  private:
    TextureHandle upload(const std::uint8_t *data, int width, int height) {
        assert(data && width > 0 && height > 0);
        return {.id = TextureId(++uploads),
                .width = width,
                .height = height,
                .sourceWidth = width,
                .sourceHeight = height};
    }
};

// SDK tests/input_bidi_test.cpp's adapter, retaining native Pango/Cairo/HarfBuzz
// measurements (including glyph measurements), with the upload-only sink above.
class TranscriptTestRenderer final : public Renderer {
  public:
    TranscriptTestRenderer() { text.initialize(nullptr, nullptr); }
    TextMetrics measureText(std::string_view value, float size, const TextFont &font = {},
                            float width = 0, int lines = 0, TextAlign align = TextAlign::Start,
                            TextEllipsize ellipsize = TextEllipsize::End,
                            bool markup = false) override {
        return metrics(
            text.measure(1.f, value, size, font, width, lines, align, ellipsize, markup));
    }
    TextMetrics measureFont(float size, const TextFont &font) override {
        return metrics(text.measureFont(1.f, size, font));
    }
    void measureTextCursorStops(std::string_view value, float size,
                                const std::vector<std::size_t> &offsets, std::vector<float> &stops,
                                const TextFont &font) override {
        text.measureCursorStops(1.f, value, size, offsets, stops, font);
    }
    void measureTextCursorStopsWrapped(std::string_view value, float size,
                                       const std::vector<std::size_t> &offsets, float width,
                                       std::vector<TextCursorStop> &stops, const TextFont &font,
                                       bool useMarkup) override {
        text.measureCursorStopsWrapped(1.f, value, size, offsets, width, stops, font, false,
                                       useMarkup);
    }
    void measureTextCaretStops(std::string_view value, float size,
                               const std::vector<std::size_t> &offsets, float width,
                               std::vector<TextCursorStop> &stops, const TextFont &font,
                               bool useMarkup) override {
        text.measureCursorStopsWrapped(1.f, value, size, offsets, width, stops, font, true,
                                       useMarkup);
    }
    void measureTextCharSpans(std::string_view value, float size,
                              const std::vector<std::size_t> &offsets,
                              std::vector<TextCharSpan> &spans, const TextFont &font) override {
        text.measureCharSpans(1.f, value, size, offsets, spans, font);
    }
    TextMetrics measureGlyph(char32_t codepoint, float size) override {
        assert(g_unichar_validate(codepoint));
        char utf8[6];
        const int length = g_unichar_to_utf8(codepoint, utf8);
        return metrics(text.measure(1.f, std::string_view(utf8, length), size));
    }
    TextureManager &textureManager() override { return textures; }
    float renderScale() const noexcept override { return 1.f; }
    CpuTextureUploadSink textures;

  private:
    CairoTextRenderer text;
    static TextMetrics metrics(const CairoTextRenderer::TextMetrics &m) {
        return {.width = m.width,
                .left = m.left,
                .right = m.right,
                .top = m.top,
                .bottom = m.bottom,
                .inkTop = m.inkTop,
                .inkBottom = m.inkBottom,
                .inkLeft = m.inkLeft,
                .inkRight = m.inkRight,
                .capHeight = m.capHeight,
                .lineCount = m.lineCount,
                .alignOffsetX = m.alignOffsetX,
                .layoutBaseline = m.layoutBaseline,
                .layoutHeight = m.layoutHeight};
    }
};

class Clipboard final : public TextClipboard {
  public:
    std::optional<std::string> clipboardText() override { return value; }
    void setClipboardText(std::string text) override { value = std::move(text); }
    void requestClipboardText(TextCallback callback) override { callback(value); }

  private:
    std::optional<std::string> value;
};

class MenuHost final : public SelectPopupContext {
  public:
    DropdownRequest request;
    DropdownCallbacks callbacks;
    bool open = false;
    bool dismissReplacedOnOpen = false;
    void openSelectDropdown(const DropdownRequest &value, DropdownCallbacks handlers) override {
        assert(value.options.empty() && !value.menu.empty());
        auto previousDismiss =
            dismissReplacedOnOpen ? callbacks.onDismiss : std::function<void()>{};
        request = value;
        callbacks = std::move(handlers);
        open = true;
        if (previousDismiss)
            previousDismiss();
    }
    void closeSelectDropdown() override {
        open = false;
        callbacks = {};
    }
    bool isSelectDropdownOpen() const override { return open; }
    const ContextMenuEntry *entry(std::string_view path) const {
        const auto find = [](auto &&self, const auto &entries,
                             std::string_view remaining) -> const ContextMenuEntry * {
            const auto slash = remaining.find('/');
            const auto label = remaining.substr(0, slash);
            for (const auto &item : entries)
                if (item.label == label && item.enabled && !item.header && !item.separator)
                    return slash == std::string_view::npos
                               ? &item
                               : self(self, item.children, remaining.substr(slash + 1));
            return nullptr;
        };
        return find(find, request.menu, path);
    }
    bool contains(std::string_view path) const { return entry(path); }
    void select(std::string_view path) {
        const auto *found = entry(path);
        assert(open && found && found->enabled && found->children.empty() && callbacks.onSelect);
        auto select = std::move(callbacks.onSelect);
        const auto index = found->value;
        assert(index != std::size_t(-1));
        closeSelectDropdown();
        select(index);
    }
};

struct ConfigDirectory {
    std::filesystem::path path;
    std::optional<std::string> previous;
    ConfigDirectory() {
        if (const auto *value = std::getenv("XDG_CONFIG_HOME"))
            previous = value;
        char name[] = "/tmp/blastirc-transcript-check-XXXXXX";
        const auto *created = mkdtemp(name);
        assert(created);
        path = created;
        assert(setenv("XDG_CONFIG_HOME", path.c_str(), 1) == 0);
    }
    ~ConfigDirectory() {
        if (previous)
            setenv("XDG_CONFIG_HOME", previous->c_str(), 1);
        else
            unsetenv("XDG_CONFIG_HOME");
        std::filesystem::remove_all(path);
    }
};

void near(float actual, float expected,
          std::source_location location = std::source_location::current()) {
    if (!(std::abs(actual - expected) < .02f))
        std::fprintf(stderr, "%s:%u %s: actual=%.9g expected=%.9g delta=%.9g\n",
                     location.file_name(), location.line(), location.function_name(), actual,
                     expected, actual - expected);
    assert(std::abs(actual - expected) < .02f);
}

void nearRadii(const Radii &actual, const Radii &expected,
               std::source_location location = std::source_location::current()) {
    near(actual.tl, expected.tl, location);
    near(actual.tr, expected.tr, location);
    near(actual.br, expected.br, location);
    near(actual.bl, expected.bl, location);
}

bool roundedCorners(const Radii &radii) {
    return radii.tl > 0 && radii.tr > 0 && radii.br > 0 && radii.bl > 0;
}

void messageSelectionGeometry(TranscriptTestRenderer &renderer) {
    blast::MessageSelectionText text;
    assert(text.update("e\xCC\x81 X"));
    assert(text.text() == "e\xCC\x81 X");
    assert((text.boundaries() == std::vector<std::size_t>{0, 3, 4, 5}));
    for (const std::size_t byte : {1u, 2u}) {
        assert(text.snap(byte, blast::MessageSelectionSnap::Before) == 0);
        assert(text.snap(byte, blast::MessageSelectionSnap::After) == 3);
        assert(text.snap(byte) == (byte == 1 ? 0u : 3u));
    }
    std::vector<blast::MessageSelectionRect> rectangles;
    for (const auto &[begin, end, markup] :
         {std::tuple{0u, 1u, "<b>e</b>"}, std::tuple{1u, 3u, "<i>\xCC\x81</i>"}}) {
        blast::MessageSelectionFragment fragment;
        fragment.rebuild(renderer, text, begin, end, markup, 14, TextFont{}, 0, true);
        fragment.rectangles(0, 3, rectangles);
        assert(rectangles.size() == 1);
        const auto selected = rectangles.front();
        assert(selected.width > 0 && selected.height > 0);
        std::vector<TextCursorStop> native;
        renderer.measureTextCaretStops(markup, 14, {0, end - begin}, 0, native, TextFont{}, true);
        assert(native.size() == 2);
        near(selected.x, native.front().cellX);
        near(selected.y, native.front().y);
        near(selected.width, native.front().cellWidth);
        near(selected.height, native.front().height);
        assert(fragment.metrics().layoutBaseline > 0);
        assert(fragment.metrics().layoutHeight >= selected.height - .02f);
        for (const float fraction : {.1f, .9f}) {
            const auto hit = fragment.hit(selected.x + selected.width * fraction,
                                          selected.y + selected.height * .5f);
            assert(hit == 0 || hit == 3);
        }
        fragment.rectangles(3, 3, rectangles);
        assert(rectangles.empty());
    }

    assert(text.update("aאבb"));
    assert((text.boundaries() == std::vector<std::size_t>{0, 1, 3, 5, 6}));
    blast::MessageSelectionFragment bidi;
    bidi.rebuild(renderer, text, 0, text.text().size(), text.text(), 14, TextFont{}, 0, false);
    std::vector<TextCursorStop> native;
    renderer.measureTextCaretStops(text.text(), 14, text.boundaries(), 0, native, TextFont{},
                                   false);
    assert(native.size() == text.boundaries().size());
    const float hebrewLeft = std::min(native[1].cellX, native[2].cellX);
    const float hebrewRight =
        std::max(native[1].cellX + native[1].cellWidth, native[2].cellX + native[2].cellWidth);
    assert(native[1].cellWidth > 0 && native[2].cellWidth > 0);
    bidi.rectangles(1, 5, rectangles);
    assert(rectangles.size() == 1);
    near(rectangles.front().x, hebrewLeft);
    near(rectangles.front().width, hebrewRight - hebrewLeft);
    near(rectangles.front().y, native[1].y);
    near(rectangles.front().height, native[1].height);
    for (std::size_t i : {1u, 2u}) {
        const auto &cell = native[i];
        assert(bidi.hit(cell.cellX + cell.cellWidth * .1f, cell.y + cell.height * .5f) ==
               text.boundaries()[i + 1]);
        assert(bidi.hit(cell.cellX + cell.cellWidth * .9f, cell.y + cell.height * .5f) ==
               text.boundaries()[i]);
    }
    bidi.rectangles(0, text.text().size(), rectangles);
    assert(!rectangles.empty());
    float selectedWidth = 0, paintedWidth = 0;
    for (const auto &rectangle : rectangles)
        selectedWidth += rectangle.width;
    for (std::size_t i = 0; i + 1 < native.size(); ++i)
        paintedWidth += native[i].cellWidth;
    near(selectedWidth, paintedWidth);
    assert(native.back().cellWidth > 0); // Native end caret is not painted message text.
    bidi.rectangles(text.text().size(), text.text().size(), rectangles);
    assert(rectangles.empty());

    assert(text.update("\U0001F1E6\U0001F1E7\U0001F1E8"));
    assert((text.boundaries() == std::vector<std::size_t>{0, 8, 12}));
    blast::MessageSelectionFragment flags;
    const auto flagFragment = text.text().substr(4);
    flags.rebuild(renderer, text, 4, 12, flagFragment, 14, TextFont{}, 0, false);
    renderer.measureTextCaretStops(flagFragment, 14, {0, 4, 8}, 0, native, TextFont{}, false);
    assert(native.size() == 3 && native.front().cellWidth > 0);
    flags.rectangles(8, 12, rectangles);
    assert(rectangles.size() == 1);
    near(rectangles.front().x, native[1].cellX);
    near(rectangles.front().width, native[1].cellWidth);
    const auto &firstFlagCell = native.front();
    assert(flags.hit(firstFlagCell.cellX + firstFlagCell.cellWidth * .1f,
                     firstFlagCell.y + firstFlagCell.height * .5f) == 0);
    const bool coincident =
        firstFlagCell.cellX == native[1].cellX && firstFlagCell.cellWidth == native[1].cellWidth &&
        firstFlagCell.y == native[1].y && firstFlagCell.height == native[1].height;
    assert(flags.hit(firstFlagCell.cellX + firstFlagCell.cellWidth * .9f,
                     firstFlagCell.y + firstFlagCell.height * .5f) == (coincident ? 12u : 8u));
    blast::MessageSelectionFragment precedingFlag;
    precedingFlag.rebuild(renderer, text, 0, 4, text.text().substr(0, 4), 14, TextFont{}, 0, false);
    precedingFlag.rectangles(0, 8, rectangles);
    assert(rectangles.size() == 1);
    const auto precedingCell = rectangles.front();
    assert(precedingFlag.hit(precedingCell.x + precedingCell.width * .9f,
                             precedingCell.y + precedingCell.height * .5f) == 8);

    assert(text.update("a\nא."));
    blast::MessageSelectionFragment paragraphs;
    paragraphs.rebuild(renderer, text, 0, text.text().size(), text.text(), 14, TextFont{}, 0,
                       false);
    renderer.measureTextCaretStops(text.text(), 14, text.boundaries(), 0, native, TextFont{},
                                   false);
    assert(native.size() == 5);
    const auto &punctuation = native[3];
    assert(punctuation.cellWidth > 0 && punctuation.y > native.front().y);
    paragraphs.rectangles(4, 5, rectangles);
    assert(rectangles.size() == 1);
    near(rectangles.front().x, punctuation.cellX);
    near(rectangles.front().width, punctuation.cellWidth);
    assert(paragraphs.hit(punctuation.cellX + punctuation.cellWidth * .1f,
                          punctuation.y + punctuation.height * .5f) == 5);
    assert(paragraphs.hit(punctuation.cellX + punctuation.cellWidth * .9f,
                          punctuation.y + punctuation.height * .5f) == 4);

    assert(text.update("\t"));
    blast::MessageSelectionFragment tab;
    tab.rebuild(renderer, text, 0, 1, text.text(), 14, TextFont{}, 0, false);
    tab.rectangles(0, 1, rectangles);
    assert(rectangles.size() == 1);
    const auto tabCell = rectangles.front();
    assert(tabCell.width > 0 && tabCell.height > 0);
    assert(tab.hit(tabCell.x + tabCell.width * .1f, tabCell.y + tabCell.height * .5f) == 0);
    assert(tab.hit(tabCell.x + tabCell.width * .9f, tabCell.y + tabCell.height * .5f) == 1);

    assert(text.update("a a"));
    const float wrapWidth = renderer.measureText("a", 14, TextFont{}, 0, 1).width +
                            renderer.measureText(" ", 14, TextFont{}, 0, 1).width * .5f;
    blast::MessageSelectionFragment wrapped;
    wrapped.rebuild(renderer, text, 0, 3, text.text(), 14, TextFont{}, wrapWidth, false);
    renderer.measureTextCaretStops(text.text(), 14, text.boundaries(), wrapWidth, native,
                                   TextFont{}, false);
    assert(native.size() == 4 && native[1].y != native[2].y);
    wrapped.rectangles(1, 2, rectangles);
    assert(rectangles.empty());
    for (std::size_t byte : {0u, 2u}) {
        wrapped.rectangles(byte, byte + 1, rectangles);
        assert(rectangles.size() == 1 && rectangles.front().width > 0 &&
               rectangles.front().height > 0);
        near(rectangles.front().x, native[byte].cellX);
        near(rectangles.front().y, native[byte].y);
        near(rectangles.front().width, native[byte].cellWidth);
        near(rectangles.front().height, native[byte].height);
    }

    assert(text.update("a b"));
    const std::string raisedMarkup = "a <span rise=\"6000\" size=\"200%\">b</span>";
    blast::MessageSelectionFragment raised;
    raised.rebuild(renderer, text, 0, 3, raisedMarkup, 14, TextFont{}, 300, true);
    renderer.measureTextCaretStops(raisedMarkup, 14, text.boundaries(), 300, native, TextFont{},
                                   true);
    assert(native.size() == 4 && native[1].lineIndex == native[2].lineIndex);
    assert(native[1].y != native[2].y && native[1].cellWidth > 0);
    raised.rectangles(1, 2, rectangles);
    assert(rectangles.size() == 1);
    near(rectangles.front().x, native[1].cellX);
    near(rectangles.front().y, native[1].y);
    near(rectangles.front().width, native[1].cellWidth);
    near(rectangles.front().height, native[1].height);

    const auto combiningDocument = blast::parseIrcFormatting(std::string{"\x02"
                                                                         "a \xCC\x81"
                                                                         "b"
                                                                         "\x02"},
                                                             Color{1, 1, 1, 1}, Color{0, 0, 0, 1});
    assert(combiningDocument.text == "a \xCC\x81"
                                     "b");
    const auto combiningMarkup = blast::ircMarkup(combiningDocument);
    assert(text.update(combiningDocument.text));
    assert((text.boundaries() == std::vector<std::size_t>{0, 1, 4, 5}));
    TextFont combiningFont;
    combiningFont.family = "sans-serif";
    blast::MessageSelectionFragment combiningSpace;
    combiningSpace.rebuild(renderer, text, 0, text.text().size(), combiningMarkup, 20,
                           combiningFont, 360, true);
    renderer.measureTextCaretStops(combiningMarkup, 20, text.boundaries(), 360, native,
                                   combiningFont, true);
    assert(native.size() == 4 && native[1].cellWidth > 0 && !native[1].cellIsVirtual);
    combiningSpace.rectangles(1, 4, rectangles);
    assert(rectangles.size() == 1);
    near(rectangles.front().x, native[1].cellX);
    near(rectangles.front().y, native[1].y);
    near(rectangles.front().width, native[1].cellWidth);
    near(rectangles.front().height, native[1].height);
}

template <class T> T *find(Node &node) {
    if (auto *result = dynamic_cast<T *>(&node))
        return result;
    for (const auto &child : node.children())
        if (auto *result = find<T>(*child))
            return result;
    return nullptr;
}
std::string decodedMarkup(std::string_view markup) {
    std::string plain;
    GMarkupParser parser{};
    parser.text = [](GMarkupParseContext *, const gchar *text, gsize length, gpointer output,
                     GError **) { static_cast<std::string *>(output)->append(text, length); };
    auto *context =
        g_markup_parse_context_new(&parser, G_MARKUP_TREAT_CDATA_AS_TEXT, &plain, nullptr);
    const bool parsed =
        g_markup_parse_context_parse(context, "<body>", -1, nullptr) &&
        g_markup_parse_context_parse(context, markup.data(), gssize(markup.size()), nullptr) &&
        g_markup_parse_context_parse(context, "</body>", -1, nullptr) &&
        g_markup_parse_context_end_parse(context, nullptr);
    g_markup_parse_context_free(context);
    assert(parsed);
    return plain;
}

TextNode *bodyText(Node &node, std::string_view token, bool complete = false) {
    if (!node.visible())
        return nullptr;
    TextNode *first = nullptr;
    TextNode *matched = nullptr;
    std::string fragments;
    // Native wrapping can split even the marker word. Reassemble only this
    // body's sibling fragments; never join text across separate message rows.
    // Native Labels can repeat message text in reply excerpts; only body-fragment
    // InputAreas belong to the original message selection/copy consumer.
    for (const auto &child : node.children()) {
        const auto *area = dynamic_cast<InputArea *>(child.get());
        if (!area || !area->visible() || dynamic_cast<const Label *>(area))
            continue;
        for (const auto &fragment : area->children())
            if (auto *text = dynamic_cast<TextNode *>(fragment.get());
                text && text->visible() && text->useMarkup()) {
                if (!first)
                    first = text;
                if (complete)
                    fragments += "<piece>";
                fragments += text->text();
                if (complete)
                    fragments += "</piece>";
            }
    }
    if (first) {
        // Link/style spans are renderer markup, not boundaries in the displayed message.
        struct Decoded {
            std::string text;
            std::vector<std::size_t> ends;
        } plain;
        GMarkupParser parser{};
        parser.text = [](GMarkupParseContext *, const gchar *text, gsize length, gpointer output,
                         GError **) { static_cast<Decoded *>(output)->text.append(text, length); };
        parser.end_element = [](GMarkupParseContext *, const gchar *name, gpointer output,
                                GError **) {
            if (std::string_view(name) == "piece") {
                auto &decoded = *static_cast<Decoded *>(output);
                decoded.ends.push_back(decoded.text.size());
            }
        };
        auto *context =
            g_markup_parse_context_new(&parser, G_MARKUP_TREAT_CDATA_AS_TEXT, &plain, nullptr);
        const bool parsed = g_markup_parse_context_parse(context, "<body>", -1, nullptr) &&
                            g_markup_parse_context_parse(context, fragments.data(),
                                                         gssize(fragments.size()), nullptr) &&
                            g_markup_parse_context_parse(context, "</body>", -1, nullptr) &&
                            g_markup_parse_context_end_parse(context, nullptr);
        g_markup_parse_context_free(context);
        assert(parsed);
        bool matches = !complete && plain.text.find(token) != std::string::npos;
        if (complete) {
            // Wrapping may omit spaces BETWEEN painted fragments, never printed characters.
            std::size_t source = 0, painted = 0;
            matches = true;
            for (const auto end : plain.ends) {
                const auto piece = std::string_view(plain.text).substr(painted, end - painted);
                if (painted && !token.substr(source).starts_with(piece))
                    while (source < token.size() && token[source] == ' ')
                        ++source;
                if (!token.substr(source).starts_with(piece)) {
                    matches = false;
                    break;
                }
                source += piece.size();
                painted = end;
            }
            matches = matches && source == token.size();
        }
        if (matches) {
            if (!complete)
                return first;
            matched = first;
        }
    }
    for (const auto &child : node.children())
        if (auto *text = bodyText(*child, token, complete)) {
            if (!complete)
                return text;
            assert(!matched && "complete body locator must identify a unique message row");
            matched = text;
        }
    return matched;
}
struct Body {
    TextNode &text;
    InputArea &fragment;
    Node &content;
    InputArea &row;
    Node &body;
    float x() const { return body.x() + fragment.x(); }
    float y() const { return body.y() + fragment.y(); }
};
Body body(Node &view, std::string_view token, bool complete = false,
          std::source_location location = std::source_location::current()) {
    auto *text = bodyText(view, token, complete);
    if (!text) {
        std::fprintf(stderr, "%s:%u missing transcript body '%.*s', view=%.1fx%.1f\n",
                     location.file_name(), location.line(), int(token.size()), token.data(),
                     view.width(), view.height());
        auto dump = [&](auto &&self, Node &node, bool visible) -> void {
            visible &= node.visible();
            if (auto *fragment = dynamic_cast<TextNode *>(&node);
                fragment && fragment->useMarkup()) {
                float x, y;
                Node::absolutePosition(fragment, x, y);
                std::fprintf(stderr, "  body visible=%d at=%.1f,%.1f text='%s'\n", visible, x, y,
                             fragment->text().c_str());
            }
            for (const auto &child : node.children())
                self(self, *child, visible);
        };
        dump(dump, view, true);
    }
    assert(text);
    auto *fragment = dynamic_cast<InputArea *>(text->parent());
    assert(fragment && fragment->parent());
    auto *body = fragment->parent();
    auto *content = body->parent();
    auto *row = content ? dynamic_cast<InputArea *>(content->parent()) : nullptr;
    assert(row && row->visible());
    return {*text, *fragment, *content, *row, *body};
}

void unchanged(const blast::Buffer &actual, const blast::Buffer &before) {
    assert(actual.messages.size() == before.messages.size());
    assert(actual.unread == before.unread && actual.readMarker == before.readMarker);
    for (std::size_t i = 0; i < before.messages.size(); ++i) {
        const auto fields = [](const blast::Message &m) {
            return std::tie(m.id, m.nick, m.text, m.time, m.reply, m.label, m.timestamp, m.arrival,
                            m.own, m.history, m.pending, m.deleted, m.mention, m.action, m.presence,
                            m.reactions, m.sendStatus, m.activity, m.activityTarget);
        };
        assert(fields(actual.messages[i]) == fields(before.messages[i]));
    }
}

void presenceGeometry(TranscriptTestRenderer &renderer, const Body &b) {
    const auto measured = renderer.measureText(b.text.text(), b.text.fontSize(), b.text.font(), 0,
                                               1, TextAlign::Start, TextEllipsize::End, true);
    assert(measured.width > 0 && measured.bottom > measured.top && measured.lineCount == 1);
    near(b.fragment.width(), measured.width);
    // Native baseline and Pango's painted logical extent stay inside the hit box.
    near(b.text.y() + measured.top, 0);
    assert(b.text.y() + measured.bottom <= b.fragment.height() + .02f);
    assert(b.row.height() >= b.y() + b.fragment.height());
    assert(b.row.height() < b.fragment.height() * 2);
    auto *surface = find<blast::MaterialNode>(b.content);
    assert(surface && !surface->visible());
    near(surface->y(), b.y());
    near(surface->x() + surface->width() * .5f, b.row.width() * .5f);
    assert(surface->height() >= b.fragment.height());
    for (const auto &child : b.content.children())
        if (const auto *rect = dynamic_cast<RectNode *>(child.get()))
            assert(!rect->visible() || rect->style().fill.a == 0);
}
void rail(const Body &b, std::string_view role) {
    int count = 0;
    const auto *surface = find<blast::MaterialNode>(b.content);
    assert(surface);
    for (const auto &child : b.content.children()) {
        const auto *rect = dynamic_cast<RectNode *>(child.get());
        if (!rect || !rect->visible() || rect->style().fill.a == 0 || rect->width() <= 0 ||
            rect->width() >= b.text.fontSize() || rect->height() <= rect->width())
            continue;
        ++count;
        assert(!role.empty());
        assert(rect->style().fill == blast::themeForegroundColor(role));
        near(rect->x(), surface->x());
        near(rect->y(), surface->y());
        near(rect->height(), surface->height());
    }
    assert(count == (role.empty() ? 0 : 1));
}

struct BodyBounds {
    float top, height;
};
BodyBounds bounds(const Body &b) {
    float top = std::numeric_limits<float>::infinity(), bottom = -top;
    for (const auto &child : b.body.children()) {
        const auto *area = dynamic_cast<InputArea *>(child.get());
        if (!area)
            continue;
        for (const auto &node : area->children())
            if (const auto *text = dynamic_cast<TextNode *>(node.get());
                text && text->useMarkup()) {
                top = std::min(top, area->y());
                bottom = std::max(bottom, area->y() + area->height());
            }
    }
    assert(std::isfinite(top) && bottom > top);
    float x, y;
    Node::absolutePosition(&b.body, x, y);
    return {y + top, bottom - top};
}

TextNode *plainText(Node &node, std::string_view value) {
    if (auto *text = dynamic_cast<TextNode *>(&node);
        text && !text->useMarkup() && text->text() == value)
        return text;
    for (const auto &child : node.children())
        if (auto *text = plainText(*child, value))
            return text;
    return nullptr;
}

float baseline(const TextNode &text) {
    float x, y;
    Node::absolutePosition(&text, x, y);
    return y;
}

LayoutRect sceneBounds(const Node &node);
LayoutRect nativeTextInk(TranscriptTestRenderer &renderer, const TextNode &text);

void traditionalDecorations(TranscriptTestRenderer &renderer, float zoom) {
    ConfigDirectory config;
    blast::Model model;
    model.settings.scale = zoom;
    model.settings.messageScale = .85f * zoom;
    model.settings.timestamps = false;
    model.settings.uiFont = "sans-serif";
    model.settings.messageFont = "serif";
    model.settings.uiFontWeight = 700;
    model.settings.messageFontWeight = 300;
    model.settings.friends.insert("Peer");
    model.settings.avatars = true;
    model.settings.reducedMotion = true;
    model.settings.themeSource = "builtin";
    blast::Theme theme;
    theme.update(model);
    auto &buffer = model.buffers["traditional-decorations"];
    buffer.id = buffer.target = "traditional-decorations";
    buffer.kind = "channel";
    model.settings.messageLayouts[buffer.id] = blast::MessageLayout::Irc;
    const std::string rtlNick = "אבגדהוזחטיכלמנסעפצקרשתLongNickname";
    for (int i = 0; i < 80; ++i) {
        blast::Message message;
        message.id = "decoration-" + std::to_string(i);
        message.text = "DecorationBody" + std::to_string(i);
        message.nick = i == 0 ? rtlNick : i % 2 ? "Me" : "Peer";
        message.own = i % 2;
        message.mention = i == 2;
        buffer.messages.push_back(std::move(message));
    }
    const auto before = buffer;
    blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
    Clipboard clipboard;
    blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
    view.setReadEligible(false);
    LayoutRect viewport{17, 23, 900 * zoom, 5000 * zoom};
    const auto arrange = [&] {
        view.sync();
        view.arrange(renderer, viewport);
    };
    const auto avatar = [&](const Body &rendered) {
        Node *result = nullptr;
        for (const auto &child : rendered.content.children())
            if ((dynamic_cast<RectNode *>(child.get()) || dynamic_cast<Image *>(child.get())) &&
                child->visible() && std::abs(child->width() - 20 * zoom) < .02f &&
                std::abs(child->height() - 20 * zoom) < .02f) {
                assert(!result);
                result = child.get();
            }
        return result;
    };
    const auto geometry = [&](bool enabled, bool monogram) {
        for (int i : {0, 1, 2}) {
            const auto rendered = body(view, "DecorationBody" + std::to_string(i), true);
            auto *nick = plainText(rendered.content, buffer.messages[i].nick);
            assert(nick && nick->visible());
            auto *hit = dynamic_cast<InputArea *>(nick->parent()->parent());
            assert(hit && hit->visible());
            float hx, hy;
            Node::absolutePosition(hit, hx, hy);
            const auto ink =
                renderer.measureText(nick->text(), nick->fontSize(), nick->font(), nick->maxWidth(),
                                     1, nick->textAlign(), nick->ellipsize(), false);
            assert(ink.lineCount == 1 && ink.width <= hit->width() + .02f);
            const auto nativeInk = nativeTextInk(renderer, *nick);
            assert(nativeInk.width > 0 && nativeInk.height > 0);
            const auto labelBounds = sceneBounds(*nick->parent());
            const auto bodyBounds = sceneBounds(rendered.body);
            const bool stacked =
                viewport.width <= 240 * zoom ||
                model.settings.messageLayouts.at(buffer.id) == blast::MessageLayout::Stacked;
            if (stacked) {
                near(bodyBounds.x, hx + (enabled ? 26 * zoom : 0));
                near(bodyBounds.x,
                     labelBounds.x -
                         (i == 1 ? labelBounds.x - sceneBounds(*find<RectNode>(*hit)).x : 0));
                assert(nativeInk.y + nativeInk.height <= bodyBounds.y + .02f);
            } else {
                assert(labelBounds.x + labelBounds.width <= bodyBounds.x + .02f);
                assert(nativeInk.x + nativeInk.width <= bodyBounds.x + .02f);
            }
            if (i == 2) {
                auto *badge = find<GlyphNode>(*hit);
                assert(badge && badge->visible());
                assert(dynamic_cast<Glyph *>(badge->parent()));
                const auto badgeBounds = sceneBounds(*badge->parent());
                near(badgeBounds.width, 14 * zoom);
                near(badgeBounds.height, 14 * zoom);
                assert(labelBounds.x + labelBounds.width <= badgeBounds.x + .02f);
                if (!stacked)
                    assert(badgeBounds.x + badgeBounds.width <= bodyBounds.x + .02f);
            }
            assert(hx >= viewport.x - .02f &&
                   hx + hit->width() <= viewport.x + viewport.width + .02f);
            auto *picture = avatar(rendered);
            assert(bool(picture) == enabled);
            if (picture) {
                float ax, ay;
                Node::absolutePosition(picture, ax, ay);
                assert(ax >= viewport.x - .02f &&
                       ax + picture->width() <= viewport.x + viewport.width + .02f);
                float labelX, labelY;
                Node::absolutePosition(nick->parent(), labelX, labelY);
                assert(ax + picture->width() <= labelX + .02f);
                assert(labelX - ax - picture->width() <= 20 * zoom + .02f);
                assert(std::abs((ay + picture->height() * .5f) - (hy + hit->height() * .5f)) <=
                       hit->height());
                if (monogram) {
                    assert(dynamic_cast<RectNode *>(picture));
                    auto *initial = plainText(rendered.content, i == 1 ? "M" : i == 2 ? "P" : "א");
                    assert(initial && initial->visible());
                } else {
                    auto *image = dynamic_cast<Image *>(picture);
                    assert(image && image->hasImage());
                }
                if (stacked)
                    assert(ax + picture->width() + 5.9f * zoom <= bodyBounds.x);
            }
            if (i == 1) {
                auto *chip = find<RectNode>(*hit);
                assert(!find<GlyphNode>(*hit));
                assert(chip && chip->visible() &&
                       chip->style().fill == blast::themeColor("primary"));
                assert(nick->color() == blast::themeForegroundColor("on_primary"));
                const auto chipBounds = sceneBounds(*chip);
                const auto labelBounds = sceneBounds(*nick->parent());
                const float padding = labelBounds.x - chipBounds.x;
                assert(padding > 0);
                near(chipBounds.width, nick->maxWidth() + 2 * padding);
                assert(!plainText(rendered.content, "YOU") && !plainText(rendered.content, "Sent"));
                rail(rendered, {});
            }
            if (stacked)
                assert(hy + hit->height() <= bounds(rendered).top + .02f);
            else
                near(baseline(*nick), baseline(rendered.text));
        }
        rail(body(view, "DecorationBody2", true), "tertiary");
    };
    for (const char *style : {"monogram", "sprite"}) {
        model.settings.avatarStyle = style;
        for (const float width : {900 * zoom, 240 * zoom}) {
            viewport.width = width;
            arrange();
            geometry(true, std::string_view(style) == "monogram");
            model.settings.avatars = false;
            arrange();
            geometry(false, false);
            model.settings.avatars = true;
        }
    }
    // Forced stacking at wide widths and transitions back through adaptive IRC.
    model.settings.avatarStyle = "monogram";
    for (const auto layout : {blast::MessageLayout::Stacked, blast::MessageLayout::Irc,
                              blast::MessageLayout::Stacked}) {
        model.settings.messageLayouts[buffer.id] = layout;
        viewport.width = 900 * zoom;
        arrange();
        geometry(true, true);
        viewport.width = 240 * zoom;
        arrange();
        geometry(true, true);
    }
    model.settings.messageLayouts[buffer.id] = blast::MessageLayout::Irc;
    model.settings.avatarStyle = "none";
    arrange();
    geometry(false, false);
    model.settings.avatarStyle = "monogram";
    arrange();
    const auto oldInitialColor =
        plainText(body(view, "DecorationBody2", true).content, "P")->color();
    const auto oldPrimary = blast::themeColor("primary");
    std::filesystem::create_directories(config.path / "blastirc");
    auto *palette = std::fopen((config.path / "blastirc" / "noctalia.toml").c_str(), "w");
    assert(palette);
    assert(std::fputs("[colors]\nprimary='#c04080'\non_primary='#ffffff'\n"
                      "surface='#101010'\non_surface='#eeeeee'\n"
                      "tertiary='#40c080'\nerror='#ff0000'\n",
                      palette) >= 0);
    assert(std::fclose(palette) == 0);
    model.settings.themeSource = "auto";
    model.settings.nickColors["Peer"] = "#e07030";
    model.settings.transparency = true;
    model.settings.opacity = .4f;
    theme.update(model);
    arrange();
    geometry(true, true);
    assert(blast::themeColor("primary") != oldPrimary);
    assert(plainText(body(view, "DecorationBody2", true).content, "P")->color() != oldInitialColor);
    view.setVisible(false);
    view.sync();
    view.setVisible(true);
    arrange();
    geometry(true, true);
    viewport.height = 160 * zoom;
    arrange();
    auto *scroll = find<ScrollView>(view);
    assert(scroll && scroll->maxScrollOffset() > 0);
    scroll->setScrollOffset(scroll->maxScrollOffset());
    arrange();
    assert(avatar(body(view, "DecorationBody79", true)));
    scroll->setScrollOffset(0);
    arrange();
    geometry(true, true);
    unchanged(buffer, before);
    assert(model.actions.empty());
}

void traditionalClocks(TranscriptTestRenderer &renderer, float zoom) {
    blast::Model model;
    model.settings.scale = zoom;
    model.settings.messageScale = .85f * zoom;
    model.settings.timestamps = true;
    model.settings.avatars = false;
    model.settings.uiFont = "sans-serif";
    model.settings.messageFont = "serif";
    model.settings.uiFontWeight = 700;
    model.settings.messageFontWeight = 300;
    model.settings.friends.insert("Peer");
    model.settings.presence = blast::PresenceMode::Smart;
    model.settings.foolDisplay = blast::FoolDisplay::Hidden;
    model.settings.fools.insert("Muted");
    model.settings.reducedMotion = true;
    model.settings.themeSource = "builtin";
    blast::Theme theme;
    theme.update(model);
    auto &buffer = model.buffers["traditional-clocks"];
    buffer.id = buffer.target = "traditional-clocks";
    buffer.kind = "channel";
    model.settings.messageLayouts[buffer.id] = blast::MessageLayout::Irc;
    // Whole-minute noon avoids local-day crossings in the seconds-offset cases.
    constexpr std::int64_t noon = 1794054840;
    const auto append = [&](std::string token, std::string nick, std::int64_t timestamp) {
        blast::Message message;
        message.id = message.text = std::move(token);
        message.nick = std::move(nick);
        message.timestamp = timestamp;
        buffer.messages.push_back(std::move(message));
    };
    append("ClockFirst", "Peer", noon);
    append("ClockSameAuthor", "Peer", noon + 1);
    append("ClockOtherAuthor", "Other", noon + 20);
    append("ClockHiddenFool", "Muted", noon + 600);
    append("ClockAfterHidden", "Peer", noon + 30);
    append("ClockMinute", "Other", noon + 60);
    for (int i = 0; i < 3; ++i) {
        append("ClockActivity" + std::to_string(i), "Peer", noon + 600 + i);
        buffer.messages.back().presence = true;
        buffer.messages.back().activity =
            i % 2 ? blast::ActivityKind::Part : blast::ActivityKind::Join;
    }
    append("ClockAfterActivity", "Other", noon + 90);
    append("ClockNextDay", "Peer", noon + 86460);
    append("ClockNextDayRepeat", "Other", noon + 86480);
    append("ClockFallback", "Peer", 0);
    buffer.messages.back().time = "2026-11-07T12:34:01Z";
    append("ClockFallbackRepeat", "Other", 0);
    buffer.messages.back().time = "2026-11-07T12:34:59Z";
    append("ClockUnknown", "Peer", 0);
    append("ClockUnknownRepeat", "Other", 0);
    for (int i = 0; i < 100; ++i)
        append("ClockTail" + std::to_string(i), i % 2 ? "Peer" : "Other", noon + 172800 + i);
    const auto before = buffer;
    blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
    Clipboard clipboard;
    blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
    view.setReadEligible(false);
    LayoutRect viewport{17, 23, 900 * zoom, 10000 * zoom};
    const auto arrange = [&] {
        view.sync();
        view.arrange(renderer, viewport);
    };
    const auto clock = [&](std::string_view token, std::string_view expected, bool shown) {
        auto *ink = plainText(body(view, token, true).content, expected);
        assert((ink && ink->visible()) == shown);
    };
    // Derive the local native clock from the first rendered row, not UTC assumptions.
    arrange();
    std::string firstClock, minuteClock;
    const auto readClock = [&](std::string_view token) {
        std::string result;
        const auto scan = [&](auto &&self, Node &node) -> void {
            if (auto *text = dynamic_cast<TextNode *>(&node);
                text && text->visible() && !text->useMarkup() && text->text().size() == 5 &&
                text->text()[2] == ':')
                result = text->text();
            for (const auto &child : node.children())
                self(self, *child);
        };
        scan(scan, body(view, token, true).content);
        assert(!result.empty());
        return result;
    };
    firstClock = readClock("ClockFirst");
    minuteClock = readClock("ClockMinute");
    assert(firstClock != minuteClock);
    const auto geometry = [&] {
        clock("ClockFirst", firstClock, true);
        for (const auto token : {"ClockSameAuthor", "ClockOtherAuthor", "ClockAfterHidden"})
            clock(token, firstClock, false);
        clock("ClockMinute", minuteClock, true);
        clock("ClockAfterActivity", minuteClock, false);
        clock("ClockNextDay", minuteClock, true);
        clock("ClockNextDayRepeat", minuteClock, false);
        clock("ClockFallback", "12:34", true);
        clock("ClockFallbackRepeat", "12:34", false);
        clock("ClockUnknown", "--:--", true);
        clock("ClockUnknownRepeat", "--:--", false);
        assert(!bodyText(view, "ClockHiddenFool") && !bodyText(view, "ClockActivity0"));
        for (const auto token : {"ClockSameAuthor", "ClockOtherAuthor", "ClockAfterHidden",
                                 "ClockMinute", "ClockAfterActivity", "ClockNextDay"})
            near(body(view, token, true).x(), body(view, "ClockFirst", true).x());
        const bool stacked =
            viewport.width <= 240 * zoom ||
            model.settings.messageLayouts.at(buffer.id) == blast::MessageLayout::Stacked;
        for (const auto token : {"ClockFirst", "ClockMinute", "ClockFallback", "ClockUnknown"}) {
            const auto rendered = body(view, token, true);
            const auto value = token == std::string_view("ClockFirst")      ? firstClock
                               : token == std::string_view("ClockMinute")   ? minuteClock
                               : token == std::string_view("ClockFallback") ? "12:34"
                                                                            : "--:--";
            auto *time = plainText(rendered.content, value);
            auto *nick = plainText(rendered.content,
                                   token == std::string_view("ClockMinute") ? "Other" : "Peer");
            assert(time && nick);
            const auto timeBounds = nativeTextInk(renderer, *time);
            const auto nickBounds = sceneBounds(*nick->parent());
            const auto bodyBounds = sceneBounds(rendered.body);
            const auto rowBounds = sceneBounds(rendered.row);
            assert(timeBounds.x >= rowBounds.x - .02f &&
                   timeBounds.x + timeBounds.width <= rowBounds.x + rowBounds.width + .02f);
            const auto nickInk = nativeTextInk(renderer, *nick);
            if (stacked) {
                near(time->fontSize(), .75f * blast::messageTextSize(model.settings, buffer));
                assert(time->font() == blast::themeUiFont());
                float clusterRight = nickBounds.x + nickBounds.width;
                if (auto *badge = find<GlyphNode>(*nick->parent()->parent())) {
                    assert(dynamic_cast<Glyph *>(badge->parent()));
                    const auto badgeBounds = sceneBounds(*badge->parent());
                    near(badgeBounds.width, 14 * zoom);
                    near(badgeBounds.height, 14 * zoom);
                    clusterRight = std::max(clusterRight, badgeBounds.x + badgeBounds.width);
                }
                near(sceneBounds(*time->parent()).x, clusterRight + 8 * zoom);
                assert(nickInk.x + nickInk.width <= timeBounds.x + .02f);
                near(baseline(*time), baseline(*nick));
                assert(timeBounds.y + timeBounds.height <= bodyBounds.y + .02f);
            } else {
                assert(timeBounds.x + timeBounds.width <= nickBounds.x + .02f);
                assert(nickBounds.x + nickBounds.width <= bodyBounds.x + .02f);
                assert(timeBounds.x + timeBounds.width <= nickInk.x + .02f);
                assert(nickInk.x + nickInk.width <= bodyBounds.x + .02f);
                near(baseline(*time), baseline(rendered.text));
                near(baseline(*nick), baseline(rendered.text));
            }
        }
    };
    for (const auto layout : {blast::MessageLayout::Irc, blast::MessageLayout::Stacked})
        for (const bool avatars : {false, true})
            for (const char *style : {"monogram", "sprite"}) {
                model.settings.messageLayouts[buffer.id] = layout;
                model.settings.avatars = avatars;
                model.settings.avatarStyle = style;
                for (const float width : {900 * zoom, 240 * zoom}) {
                    viewport.width = width;
                    arrange();
                    geometry();
                    const float bodyX = body(view, "ClockOtherAuthor", true).x();
                    model.settings.timestamps = false;
                    arrange();
                    clock("ClockFirst", firstClock, false);
                    clock("ClockMinute", minuteClock, false);
                    clock("ClockFallback", "12:34", false);
                    if (width <= 240 * zoom || layout == blast::MessageLayout::Stacked)
                        near(body(view, "ClockOtherAuthor", true).x(), bodyX);
                    else
                        assert(body(view, "ClockOtherAuthor", true).x() < bodyX);
                    model.settings.timestamps = true;
                    arrange();
                    geometry();
                    near(body(view, "ClockOtherAuthor", true).x(), bodyX);
                }
            }
    viewport.height = 140 * zoom;
    arrange();
    auto *scroll = find<ScrollView>(view);
    assert(scroll && scroll->maxScrollOffset() > 0);
    scroll->setScrollOffset(scroll->maxScrollOffset());
    arrange();
    assert(bodyText(view, "ClockTail99"));
    scroll->setScrollOffset(0);
    arrange();
    clock("ClockFirst", firstClock, true);
    clock("ClockSameAuthor", firstClock, false);
    viewport.height = 10000 * zoom;
    arrange();
    geometry();
    unchanged(buffer, before);
    assert(model.actions.empty());
}

void containedBounds(LayoutRect inner, LayoutRect outer,
                     std::source_location location = std::source_location::current());
LayoutRect nativeHistoryClip(blast::TranscriptView &view);

void channelConversation(TranscriptTestRenderer &renderer, float zoom) {
    blast::Model model;
    model.settings.scale = zoom;
    model.settings.messageScale = zoom;
    model.settings.timestamps = true;
    model.settings.reducedMotion = true;
    model.settings.themeSource = "builtin";
    blast::Theme theme;
    theme.update(model);
    auto &buffer = model.buffers["channel"];
    buffer.id = buffer.target = "channel";
    model.settings.messageLayouts[buffer.id] = blast::MessageLayout::Irc;
    buffer.unread = 7;
    const std::string longNick = "長い名前оченьдлинноеимяLongNativeNickname";
    for (int i = 0; i < 40; ++i) {
        blast::Message message;
        message.id = "channel-" + std::to_string(i);
        message.nick = i == 2 || i == 3 ? longNick : i % 2 ? "Alice" : "Bob";
        message.time = "2026-11-07T12:34:56Z";
        message.text = i == 0
                           ? "ChannelAnchor native body shaping wraps this retained first message "
                             "across several lines when the timestamp and nickname lanes yield to "
                             "a narrow viewport, without anchoring to a date or author heading."
                       : i == 1 ? "AlphaLine"
                       : i == 2 ? "BetaLine"
                       : i == 3 ? "GroupedLine"
                       : i == 4 ? "WrappedLine \x02"
                                  "bold native shaping"
                                  "\x02 https://example.org/channel/geometry repeated words keep "
                                  "the native rich fragments wrapping into the same body column."
                                : "ChannelTail-" + std::to_string(i);
        message.action = i == 5;
        message.deleted = i == 6;
        message.pending = i == 7;
        if (i == 7) {
            message.reply = "channel-1";
            message.reactions["👍"] = {"Alice"};
        }
        buffer.messages.push_back(std::move(message));
    }
    buffer.readMarker = buffer.messages.back().id;
    auto before = buffer;
    blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
    Clipboard clipboard;
    std::string command;
    blast::TranscriptView view(
        model, pane, clipboard, [&](auto value) { command = value; }, [](auto) {});
    view.setReadEligible(false);
    const LayoutRect wide{17, 23, 900 * zoom, 2000 * zoom};
    view.sync();
    view.arrange(renderer, wide);
    auto *scroll = find<ScrollView>(view);
    assert(scroll);
    const auto arrangeFirstRows = [&](const LayoutRect &viewport) {
        // Reposition after each bounded pass as native row heights replace estimates.
        for (int pass = 0; pass < 3; ++pass) {
            scroll->setScrollOffset(0);
            view.sync();
            view.arrange(renderer, viewport);
        }
        near(scroll->scrollOffset(), 0);
    };
    arrangeFirstRows(wide);
    const auto alpha = body(view, "AlphaLine");
    const auto beta = body(view, "BetaLine");
    const auto grouped = body(view, "GroupedLine");
    const auto wrapped = body(view, "WrappedLine");
    assert(!bodyText(view, "AlphaLineBetaLine"));
    auto *author = plainText(alpha.content, "Alice");
    auto *longAuthor = plainText(beta.content, longNick);
    auto *clock = plainText(body(view, "ChannelAnchor").content, "12:34");
    assert(author && longAuthor && clock && !plainText(grouped.content, longNick));
    assert(!plainText(alpha.content, "12:34") && !plainText(beta.content, "12:34"));
    near(alpha.x(), body(view, "ChannelAnchor").x());
    near(baseline(*author), baseline(alpha.text));
    near(baseline(*longAuthor), baseline(beta.text));
    const auto nickControl = [](TextNode &text) -> InputArea * {
        for (auto *node = text.parent(); node; node = node->parent())
            if (auto *area = dynamic_cast<InputArea *>(node);
                area && area->focusable() && area->hitTestVisible())
                return area;
        return nullptr;
    };
    const auto visibleNickInk = [&](TextNode &text, InputArea &hit) {
        const auto metrics = renderer.measureText(
            text.text(), text.fontSize(), text.font(), text.maxWidth(), text.maxLines(),
            text.textAlign(), text.ellipsize(), text.useMarkup());
        assert(metrics.lineCount == 1 && metrics.inkRight > metrics.inkLeft &&
               metrics.inkBottom > metrics.inkTop);
        float inkLeft, inkTop, inkRight, inkBottom, hitLeft, hitTop, hitRight, hitBottom;
        Node::mapToScene(&text, metrics.inkLeft, metrics.inkTop, inkLeft, inkTop);
        Node::mapToScene(&text, metrics.inkRight, metrics.inkBottom, inkRight, inkBottom);
        Node::transformedBounds(&hit, hitLeft, hitTop, hitRight, hitBottom);
        assert(std::min(inkRight, hitRight) > std::max(inkLeft, hitLeft) &&
               std::min(inkBottom, hitBottom) > std::max(inkTop, hitTop));
    };
    auto *authorControl = nickControl(*author);
    assert(authorControl);
    visibleNickInk(*author, *authorControl);
    near(alpha.x(), beta.x());
    near(alpha.x(), grouped.x());
    near(alpha.x(), wrapped.x());
    const auto line = bounds(alpha);
    float rowLeft, rowTop, rowRight, rowBottom;
    Node::transformedBounds(&alpha.row, rowLeft, rowTop, rowRight, rowBottom);
    float bodyLeft, bodyTop, bodyRight, bodyBottom;
    Node::transformedBounds(&alpha.body, bodyLeft, bodyTop, bodyRight, bodyBottom);
    assert(bodyLeft >= rowLeft - .02f && bodyRight <= rowRight + .02f && bodyTop >= rowTop - .02f &&
           bodyBottom <= rowBottom + .02f);
    assert(alpha.row.height() >= line.height && line.top >= rowTop - .02f &&
           line.top + line.height <= rowBottom + .02f);
    auto *nickHit = nickControl(*longAuthor);
    assert(nickHit && nickHit->focusable());
    assert(nickHit->x() + nickHit->width() < beta.x());
    const auto nickMetrics = renderer.measureText(
        longAuthor->text(), longAuthor->fontSize(), longAuthor->font(), longAuthor->maxWidth(), 1,
        longAuthor->textAlign(), longAuthor->ellipsize(), false);
    assert(nickMetrics.lineCount == 1 && nickMetrics.width <= nickHit->width() + .02f);
    visibleNickInk(*longAuthor, *nickHit);
    for (const auto &child : wrapped.body.children()) {
        const auto *hit = dynamic_cast<InputArea *>(child.get());
        if (!hit)
            continue;
        for (const auto &node : hit->children())
            if (const auto *text = dynamic_cast<TextNode *>(node.get());
                text && text->useMarkup()) {
                assert(wrapped.body.x() + hit->x() >= alpha.x() - .02f);
                assert(wrapped.body.x() + hit->x() + hit->width() <= wrapped.row.width() + .02f);
                assert(wrapped.body.y() + hit->y() + hit->height() <= wrapped.row.height() + .02f);
            }
    }
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&view);
    float nickX, nickY;
    Node::mapToScene(nickHit, nickHit->width() * .5f, nickHit->height() * .5f, nickX, nickY);
    for (const bool touch : {false, true}) {
        command.clear();
        assert(dispatcher.pointerButton(nickX, nickY, BTN_LEFT, true, 1, 10, touch));
        assert(nickHit->pressed() && command.empty());
        assert(dispatcher.pointerButton(nickX, nickY, BTN_LEFT, false, 2, 20, touch));
        assert(command == "nick:" + buffer.id + "\n" + longNick);
        command.clear();
        assert(dispatcher.pointerButton(nickX, nickY, BTN_LEFT, true, 3, 30, touch));
        view.setVisible(false);
        view.sync();
        view.setVisible(true);
        view.sync();
        arrangeFirstRows(wide);
        dispatcher.pointerButton(nickX, nickY, BTN_LEFT, false, 4, 40, touch);
        assert(command.empty());
    }
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
    unchanged(buffer, before);

    const float withTime = alpha.x();
    model.settings.timestamps = false;
    arrangeFirstRows(wide);
    assert(!plainText(body(view, "AlphaLine").content, "12:34"));
    assert(body(view, "AlphaLine").x() < withTime);
    model.settings.timestamps = true;
    arrangeFirstRows(wide);
    near(body(view, "AlphaLine").x(), withTime);

    const LayoutRect narrow{17, 23, 240 * zoom, 2000 * zoom};
    arrangeFirstRows(narrow);
    const auto compact = body(view, "AlphaLine");
    auto *compactAuthor = plainText(compact.content, "Alice");
    assert(compactAuthor);
    auto *compactHit = nickControl(*compactAuthor);
    assert(compactHit && compactHit->y() + compactHit->height() <= compact.y() + .02f);
    assert(compact.x() < withTime);
    near(sceneBounds(compact.body).x, sceneBounds(*compactAuthor->parent()).x);
    assert(sceneBounds(compact.body).width + .02f >= compactAuthor->parent()->width());
    visibleNickInk(*compactAuthor, *compactHit);
    const auto compactGrouped = body(view, "GroupedLine");
    auto *groupedAuthor = plainText(compactGrouped.content, longNick);
    assert(groupedAuthor);
    auto *groupedHit = nickControl(*groupedAuthor);
    assert(groupedHit && groupedHit->y() + groupedHit->height() <= compactGrouped.y() + .02f);
    const auto previousBounds = bounds(body(view, "BetaLine"));
    assert(bounds(compactGrouped).top >= previousBounds.top + previousBounds.height);
    if (zoom == 2.f) {
        arrangeFirstRows({17, 23, 720, narrow.height});
        const auto minimum = body(view, "AlphaLine");
        auto *minimumAuthor = plainText(minimum.content, "Alice");
        assert(minimumAuthor);
        auto *prefix = nickControl(*minimumAuthor);
        assert(prefix && prefix->y() + prefix->height() <= minimum.y() + .02f);
        const auto minimumBody = sceneBounds(minimum.body);
        const auto minimumNick = sceneBounds(*minimumAuthor->parent());
        near(minimumBody.x, minimumNick.x);
        assert(minimumNick.x + minimumNick.width <= minimumBody.x + minimumBody.width + .02f);
        containedBounds(minimumBody, nativeHistoryClip(view));
        containedBounds(sceneBounds(*prefix), nativeHistoryClip(view));
        visibleNickInk(*minimumAuthor, *prefix);
    }
    unchanged(buffer, before);
    assert(model.actions.empty());

    if (zoom != 1.f)
        return;
    LayoutRect viewport{17, 23, wide.width, 160};
    pane.following = true;
    view.sync();
    view.arrange(renderer, viewport);
    assert(scroll && scroll->maxScrollOffset() > 0 && pane.following);
    scroll->setScrollOffset(0);
    view.sync();
    view.arrange(renderer, viewport);
    auto anchor = bounds(body(view, "ChannelAnchor"));
    scroll->setScrollOffset(scroll->scrollOffset() + anchor.top - view.y() + anchor.height * .25f);
    view.sync();
    view.arrange(renderer, viewport);
    auto anchored = [&] {
        const auto retained = bounds(body(view, "ChannelAnchor"));
        near(retained.top - view.y(), -retained.height * .25f);
        assert(!pane.following);
    };
    anchored();
    model.settings.messageLayouts.erase(buffer.id);
    view.sync();
    view.arrange(renderer, viewport);
    anchored();
    assert(find<blast::MaterialNode>(body(view, "ChannelAnchor").content)->visible());
    model.settings.timestamps = false;
    view.sync();
    view.arrange(renderer, viewport);
    anchored();
    model.settings.timestamps = true;
    view.sync();
    view.arrange(renderer, viewport);
    anchored();
    viewport.width = narrow.width;
    view.arrange(renderer, viewport);
    anchored();
    viewport.width = wide.width;
    view.arrange(renderer, viewport);
    anchored();
    model.settings.messageLayouts[buffer.id] = blast::MessageLayout::Irc;
    view.sync();
    view.arrange(renderer, viewport);
    anchored();
    // Prepending the same author removes the retained row's day/group boundary.
    auto older = buffer.messages.front();
    older.id = older.text = "PrependedChannel";
    older.history = true;
    buffer.messages.push_front(older);
    before = buffer;
    view.sync();
    view.arrange(renderer, viewport);
    anchored();
    assert(!plainText(body(view, "ChannelAnchor").content, "Bob"));
    unchanged(buffer, before);
    auto arrival = buffer.messages.back();
    arrival.id = arrival.text = "UnfollowedChannelArrival";
    buffer.messages.push_back(arrival);
    before = buffer;
    view.sync();
    view.arrange(renderer, viewport);
    anchored();
    assert(pane.unseen == 1);
    unchanged(buffer, before);
    viewport.width = narrow.width;
    view.arrange(renderer, viewport);
    anchored();
    model.settings.timestamps = false;
    model.settings.messageFont = model.settings.uiFont = "serif";
    model.settings.scale = 2;
    theme.update(model);
    view.sync();
    view.arrange(renderer, viewport);
    anchored();
    viewport.width = 720;
    view.arrange(renderer, viewport);
    anchored();
    unchanged(buffer, before);
    assert(pane.unseen == 1 && model.actions.empty());
    model.now = 2;
    view.setReadEligible(true);
    view.sync();
    assert(model.actions.empty());
    pane.following = true;
    view.sync();
    view.arrange(renderer, viewport);
    near(scroll->scrollOffset(), scroll->maxScrollOffset());
    assert(pane.following && pane.unseen == 0);
    assert(model.actions.size() == 1 && model.actions.back().type == "read");
}

TextNode *bubbleFooter(Node &node, float) {
    if (auto *text = dynamic_cast<TextNode *>(&node);
        text && text->visible() && !text->useMarkup() && text->maxLines() == 0 &&
        text->textAlign() == TextAlign::End && text->text().size() == 5 && text->text()[2] == ':')
        return text;
    for (const auto &child : node.children())
        if (child->visible())
            if (auto *text = bubbleFooter(*child, 0))
                return text;
    return nullptr;
}

Node *bubbleSurface(const Body &b) {
    auto *material = find<blast::MaterialNode>(b.content);
    assert(material);
    if (material->visible())
        return material;
    for (const auto &child : b.content.children())
        if (auto *rect = dynamic_cast<RectNode *>(child.get());
            rect && rect->visible() &&
            rect->style().fill == blast::themeSurfaceColor("tertiary_container") &&
            rect->x() == material->x() && rect->y() == material->y() &&
            rect->width() == material->width() && rect->height() == material->height())
            return rect;
    return nullptr;
}

InputArea *bubbleStatus(Node &content) {
    for (const auto &child : content.children())
        if (auto *area = dynamic_cast<InputArea *>(child.get());
            area && area->visible() && area->acceptedButtons() == 0 && !area->focusable() &&
            (find<Spinner>(*area) || find<GlyphNode>(*area)))
            return area;
    return nullptr;
}

void bubbleDelivery(const Body &b, const blast::Message &message, float zoom) {
    const bool queued = message.own && message.sendStatus == blast::SendStatus::Queued;
    const bool sent = message.own && (message.sendStatus == blast::SendStatus::Sent ||
                                      message.sendStatus == blast::SendStatus::Confirmed);
    auto *status = bubbleStatus(b.content);
    assert(bool(status) == (queued || sent));
    assert(bool(find<Spinner>(b.content)) == queued);
    auto *check = status ? find<GlyphNode>(*status) : nullptr;
    assert(bool(check) == sent);
    if (check) {
        assert(GlyphRegistry::contains("check"));
        assert(check->codepoint() == GlyphRegistry::lookup("check"));
        near(check->fontSize(), 12 * zoom);
    }
    if (!status)
        return;
    assert(status->children().size() == 1);
    assert(status->acceptedButtons() == 0 && !status->focusable());
    near(status->width(), 12 * zoom);
    near(status->height(), 12 * zoom);
    auto *surface = bubbleSurface(b);
    assert(surface && surface->visible());
    assert(status->x() >= surface->x() - .02f &&
           status->x() + status->width() <= surface->x() + surface->width() + .02f &&
           status->y() >= surface->y() - .02f &&
           status->y() + status->height() <= surface->y() + surface->height() + .02f);
    for (const auto &child : b.content.children()) {
        if (child.get() == status || !child->visible())
            continue;
        const bool bodyFragment = std::any_of(
            child->children().begin(), child->children().end(), [](const auto &fragment) {
                const auto *text = dynamic_cast<TextNode *>(fragment.get());
                return text && text->useMarkup();
            });
        if (dynamic_cast<Label *>(child.get()) || dynamic_cast<Button *>(child.get()) ||
            bodyFragment)
            assert(child->y() + child->height() <= status->y() + .02f);
    }
    assert(b.body.y() + b.body.height() <= status->y() + .02f);
    const auto tooltip = status->tooltipContent();
    const auto *text = std::get_if<std::string>(&tooltip);
    assert(text &&
           *text == (queued                                               ? "Sending to server"
                     : message.sendStatus == blast::SendStatus::Confirmed ? "Confirmed by server"
                                                                          : "Sent to server"));
}

void bubbleGeometry(TranscriptTestRenderer &renderer, const Body &b, float zoom,
                    std::string_view clock) {
    auto *surface = bubbleSurface(b);
    auto *footer = bubbleFooter(b.content, zoom);
    assert(surface && surface->visible() && footer);
    assert(footer->text() == clock);
    assert(footer->fontSize() < b.text.fontSize());
    assert(footer->opacity() > .7f && footer->opacity() < 1);
    assert(footer->color().a == 1);
    near(b.content.opacity(), 1);
    const auto measured =
        renderer.measureText(footer->text(), footer->fontSize(), footer->font(), footer->maxWidth(),
                             0, footer->textAlign(), footer->ellipsize(), false);
    float footerX, footerY, contentX, contentY;
    Node::absolutePosition(footer, footerX, footerY);
    Node::absolutePosition(&b.content, contentX, contentY);
    footerX -= contentX;
    footerY -= contentY;
    const float footerTop = footerY + measured.top;
    const float footerBottom = footerY + measured.bottom;
    assert(measured.width <= footer->maxWidth() + .02f);
    const float inset = b.x() - surface->x();
    assert(inset > 0);
    auto *status = bubbleStatus(b.content);
    const float footerRight = status ? status->x() + status->width() : footerX + footer->width();
    near(footerRight, surface->x() + surface->width() - inset);
    if (status) {
        near(status->width(), 12 * zoom);
        near(status->height(), 12 * zoom);
        assert(status->acceptedButtons() == 0 && !status->focusable());
        assert(status->x() >= surface->x() + inset - .02f && status->y() >= footerTop - .02f &&
               status->y() + status->height() <= surface->y() + surface->height() + .02f);
        assert(status->y() >= footerBottom - .02f ||
               status->x() >= footerX + footer->width() + 4 * zoom - .02f);
    }
    assert(footerX >= surface->x() + inset - .02f);
    assert(footerTop >= surface->y() && footerBottom <= surface->y() + surface->height() + .02f);
    assert(surface->x() >= 0 && surface->x() + surface->width() <= b.row.width() + .02f);
    assert(surface->y() + surface->height() <= b.row.height() + .02f);
    for (const auto &child : b.body.children())
        if (const auto *area = dynamic_cast<InputArea *>(child.get()))
            for (const auto &fragment : area->children())
                if (const auto *text = dynamic_cast<TextNode *>(fragment.get());
                    text && text->useMarkup()) {
                    assert(b.body.x() + area->x() >= surface->x() + inset - .02f);
                    assert(b.body.x() + area->x() + area->width() <=
                           surface->x() + surface->width() - inset + .02f);
                    assert(b.body.y() + area->y() + area->height() <= footerTop + .02f);
                }
    for (const auto &child : b.content.children())
        if (dynamic_cast<Label *>(child.get()) || dynamic_cast<Button *>(child.get()))
            assert(child->y() + child->height() <= footerTop + .02f);
}

void rowGapReflow(TranscriptTestRenderer &renderer, bool presence) {
    blast::Model model;
    model.settings.timestamps = false;
    model.settings.avatars = false;
    model.settings.reducedMotion = true;
    model.settings.themeSource = "builtin";
    blast::Theme theme;
    theme.update(model);
    auto &buffer = model.buffers["gap"];
    buffer.id = buffer.target = "gap";
    buffer.kind = "channel";
    if (!presence)
        model.settings.messageLayouts[buffer.id] = blast::MessageLayout::Irc;
    for (int i = 0; i < 12; ++i) {
        blast::Message message;
        message.id = message.text = "GapAnchor-" + std::to_string(i);
        message.nick = i % 2 ? "Peer" : "Other";
        message.presence = presence;
        buffer.messages.push_back(std::move(message));
    }
    blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
    Clipboard clipboard;
    blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
    view.setReadEligible(false);
    view.sync();
    const LayoutRect viewport{17, 23, 600, 80};
    view.arrange(renderer, viewport);
    auto *scroll = find<ScrollView>(view);
    assert(scroll && scroll->maxScrollOffset() > 0);
    scroll->setScrollOffset(0);
    view.sync();
    view.arrange(renderer, viewport);
    const auto anchor = body(view, "GapAnchor-2");
    const float rowHeight = anchor.row.height();
    auto *surface = find<blast::MaterialNode>(anchor.content);
    assert(surface);
    float x, y, left, top, right, bottom;
    Node::absolutePosition(surface, x, y);
    const float surfaceEnd = y + surface->height();
    Node::transformedBounds(&anchor.row, left, top, right, bottom);
    assert(bottom > surfaceEnd);
    scroll->setScrollOffset(scroll->scrollOffset() + (surfaceEnd + bottom) * .5f - view.y());
    const float retained = scroll->scrollOffset();
    view.sync();
    view.arrange(renderer, viewport);
    near(scroll->scrollOffset(), retained);
    view.arrange(renderer, {viewport.x, viewport.y, 900, viewport.height});
    near(body(view, "GapAnchor-2").row.height(), rowHeight);
    near(scroll->scrollOffset(), retained);
    view.arrange(renderer, viewport);
    near(scroll->scrollOffset(), retained);
    assert(!pane.following && model.actions.empty());
}

LayoutRect sceneBounds(const Node &node) {
    float left, top, right, bottom;
    Node::transformedBounds(&node, left, top, right, bottom);
    return {left, top, right - left, bottom - top};
}

LayoutRect intersectBounds(LayoutRect a, LayoutRect b) {
    const float right = std::min(a.x + a.width, b.x + b.width);
    const float bottom = std::min(a.y + a.height, b.y + b.height);
    a.x = std::max(a.x, b.x);
    a.y = std::max(a.y, b.y);
    return {a.x, a.y, right - a.x, bottom - a.y};
}

void containedBounds(LayoutRect inner, LayoutRect outer, std::source_location location) {
    const bool positive = inner.width > 0 && inner.height > 0;
    const bool contained = inner.x >= outer.x - .02f && inner.y >= outer.y - .02f &&
                           inner.x + inner.width <= outer.x + outer.width + .02f &&
                           inner.y + inner.height <= outer.y + outer.height + .02f;
    if (!positive || !contained)
        std::fprintf(stderr,
                     "%s:%u native containment failed: inner=(%.3f, %.3f, %.3f, %.3f) "
                     "outer=(%.3f, %.3f, %.3f, %.3f)\n",
                     location.file_name(), location.line(), inner.x, inner.y, inner.width,
                     inner.height, outer.x, outer.y, outer.width, outer.height);
    assert(positive);
    assert(contained);
}

LayoutRect ancestorPaintClip(const Node &node) {
    LayoutRect clip{};
    bool clipped = false;
    for (auto *ancestor = node.parent(); ancestor; ancestor = ancestor->parent()) {
        assert(ancestor->visible());
        if (ancestor->clipChildren()) {
            const auto bounds = sceneBounds(*ancestor);
            clip = clipped ? intersectBounds(clip, bounds) : bounds;
            clipped = true;
        }
    }
    assert(clipped && clip.width > 0 && clip.height > 0);
    return clip;
}

LayoutRect nativeHistoryClip(blast::TranscriptView &view) {
    auto *scroll = find<ScrollView>(view);
    assert(scroll && scroll->clipChildren() && scroll->content());
    const auto clip = ancestorPaintClip(*scroll->content());
    // Native paint clipping includes the scrollbar gutter; contentViewportWidth() does not.
    near(clip.height, scroll->contentViewportHeight());
    const auto scrollBounds = sceneBounds(*scroll);
    containedBounds(clip, scrollBounds);
    const auto transcript = sceneBounds(view);
    containedBounds(scrollBounds, transcript);
    near(clip.y + clip.height, scrollBounds.y + scrollBounds.height);
    return clip;
}

LayoutRect nativeTextInk(TranscriptTestRenderer &renderer, const TextNode &text) {
    const auto measured =
        renderer.measureText(text.text(), text.fontSize(), text.font(), text.maxWidth(),
                             text.maxLines(), text.textAlign(), text.ellipsize(), text.useMarkup());
    float left, top, right, bottom;
    Node::mapToScene(&text, measured.inkLeft, measured.inkTop, left, top);
    Node::mapToScene(&text, measured.inkRight, measured.inkBottom, right, bottom);
    assert(right > left && bottom > top);
    return {left, top, right - left, bottom - top};
}

// CPU native glyph metrics and real scene clips, not a GPU pixel assertion.
LayoutRect revealBubbleText(TranscriptTestRenderer &renderer, blast::TranscriptView &view,
                            LayoutRect layout, std::string_view token, bool footer,
                            std::size_t fragment = 0) {
    const auto target = [&]() -> TextNode * {
        const auto rendered = body(view, token, true);
        if (footer)
            return bubbleFooter(rendered.content, 2);
        std::size_t index = 0;
        for (const auto &area : rendered.body.children())
            for (const auto &child : area->children())
                if (auto *text = dynamic_cast<TextNode *>(child.get());
                    text && text->useMarkup() && index++ == fragment)
                    return text;
        return nullptr;
    };
    auto *text = target();
    assert(text);
    auto clip = nativeHistoryClip(view);
    auto ink = nativeTextInk(renderer, *text);
    assert(ink.height <= clip.height + .02f && ink.width <= clip.width + .02f);
    auto *scroll = find<ScrollView>(view);
    // Lazy row measurement can revise the native scroll extent during arrange.
    // Recenter against the resulting painted geometry, not an estimated offset.
    for (int pass = 0; pass < 16; ++pass) {
        if (ink.x >= clip.x - .02f && ink.y >= clip.y - .02f &&
            ink.x + ink.width <= clip.x + clip.width + .02f &&
            ink.y + ink.height <= clip.y + clip.height + .02f)
            break;
        const float offset = std::clamp(scroll->scrollOffset() + ink.y + ink.height * .5f -
                                            (clip.y + clip.height * .5f),
                                        0.f, scroll->maxScrollOffset());
        scroll->setScrollOffset(offset);
        view.sync();
        view.arrange(renderer, layout);
        text = target();
        assert(text);
        clip = nativeHistoryClip(view);
        ink = nativeTextInk(renderer, *text);
    }
    text = target();
    assert(text);
    clip = nativeHistoryClip(view);
    ink = nativeTextInk(renderer, *text);
    const auto rendered = body(view, token, true);
    auto *surface = bubbleSurface(rendered);
    assert(surface);
    for (const Node *node : {surface, &rendered.body, static_cast<Node *>(text)}) {
        const auto actualClip = ancestorPaintClip(*node);
        containedBounds(actualClip, clip);
        containedBounds(intersectBounds(sceneBounds(*node), actualClip), clip);
    }
    containedBounds(ink, ancestorPaintClip(*text));
    containedBounds(ink, clip);
    bubbleGeometry(renderer, rendered, 2, "12:34");
    return ink;
}

void reachableBubble(TranscriptTestRenderer &renderer, blast::TranscriptView &view,
                     LayoutRect layout, std::string_view token) {
    const auto rendered = body(view, token, true);
    std::size_t fragments = 0;
    for (const auto &area : rendered.body.children())
        for (const auto &child : area->children())
            if (const auto *text = dynamic_cast<TextNode *>(child.get()); text && text->useMarkup())
                ++fragments;
    assert(fragments);
    for (std::size_t i = 0; i < fragments; ++i)
        revealBubbleText(renderer, view, layout, token, false, i);
    revealBubbleText(renderer, view, layout, token, true);
}

void tightBubbleViewport(TranscriptTestRenderer &renderer, const char *kind) {
    blast::Model model;
    model.settings.scale = 2;
    model.settings.timestamps = true;
    model.settings.avatars = false;
    model.settings.reducedMotion = true;
    model.settings.themeSource = "builtin";
    blast::Theme theme;
    theme.update(model);
    auto &buffer = model.buffers["compact"];
    buffer.id = buffer.target = "compact";
    buffer.kind = kind;
    buffer.server = "network";
    auto &network = model.networks[buffer.server];
    network.state = "Ready";
    network.nick = "Me";
    network.caps = {"message-tags", "draft/message-redaction"};
    for (int i = 0; i < 12; ++i) {
        blast::Message message;
        message.id = message.text = "Compact-" + std::to_string(i);
        message.nick = i % 2 ? "Me" : "Peer";
        message.own = i % 2;
        message.time = "2026-11-07T12:34:56Z";
        buffer.messages.push_back(std::move(message));
    }
    blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
    Clipboard clipboard;
    MenuHost host;
    blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
    view.setPopupContext(&host);
    view.setReadEligible(false);
    view.sync();
    view.arrange(renderer, {17, 23, 900, 800});
    const float airyHeight = body(view, "Compact-11").row.height();
    // Native one-line composer and shell allocations at minimum 720x480, zoom 2.
    const float composerHeight =
        std::max(64.f, renderer.fontRowExtent(32, blast::themeMessageFont()) + 12) + 16;
    const float transcriptHeight = 248 - 8 - 12 - composerHeight;
    view.arrange(renderer, {17, 23, 900, transcriptHeight});
    assert(body(view, "Compact-11").row.height() < airyHeight);
    view.arrange(renderer, {17, 23, 900, 800});
    near(body(view, "Compact-11").row.height(), airyHeight);
    if (std::string_view(kind) == "query") {
        InputDispatcher dispatcher;
        dispatcher.setSceneRoot(&view);
        for (const auto *token : {"Compact-10", "Compact-11"}) {
            revealBubbleText(renderer, view, {17, 23, 900, 800}, token, false);
            const auto rendered = body(view, token);
            auto *surface = bubbleSurface(rendered);
            assert(surface && surface->x() > 0 &&
                   surface->x() + surface->width() < rendered.row.width());
            const float middleY = surface->y() + surface->height() * .5f;
            const auto click = [&](float localX, float localY, int button, bool touch) {
                float x, y;
                Node::mapToScene(&rendered.row, localX, localY, x, y);
                assert(dispatcher.pointerButton(x, y, button, true, 101, 1010, touch));
                dispatcher.pointerButton(x, y, button, false, 102, 1020, touch);
            };
            for (const auto button : {BTN_LEFT, BTN_RIGHT})
                for (const bool touch : {false, true}) {
                    if (touch && button == BTN_RIGHT)
                        continue;
                    click(surface->x() * .5f, middleY, button, touch);
                    assert(!host.open && model.actions.empty());
                    click((surface->x() + surface->width() + rendered.row.width()) * .5f, middleY,
                          button, touch);
                    assert(!host.open && model.actions.empty());
                    if (surface->y() + surface->height() < rendered.row.height()) {
                        click(surface->x() + surface->width() * .5f,
                              (surface->y() + surface->height() + rendered.row.height()) * .5f,
                              button, touch);
                        assert(!host.open && model.actions.empty());
                    }
                    click(surface->x() + 2, surface->y() + surface->height() * .5f, button, touch);
                    assert(host.open && host.contains("Copy text") &&
                           host.request.inputSerial == 101);
                    host.select("Copy text");
                    assert(clipboard.clipboardText() == token);
                }
        }
        model.settings.messageLayouts[buffer.id] = blast::MessageLayout::Irc;
        view.sync();
        view.arrange(renderer, {17, 23, 900, 800});
        const auto dense = body(view, "Compact-11");
        float denseX, denseY;
        Node::mapToScene(&dense.row, dense.row.width() - 2, dense.row.height() * .5f, denseX,
                         denseY);
        assert(dispatcher.pointerButton(denseX, denseY, BTN_RIGHT, true, 103, 1030, false));
        dispatcher.pointerButton(denseX, denseY, BTN_RIGHT, false, 104, 1040, false);
        assert(host.open && host.contains("Copy text"));
        host.select("Copy text");
        assert(clipboard.clipboardText() == "Compact-11");
        model.settings.messageLayouts.erase(buffer.id);
        view.sync();
        view.arrange(renderer, {17, 23, 900, 800});
        dispatcher.setSceneRoot(nullptr);
    }
    view.arrange(renderer, {17, 23, 512, transcriptHeight});
    for (const auto *token : {"Compact-10", "Compact-11"})
        reachableBubble(renderer, view, {17, 23, 512, transcriptHeight}, token);
    if (std::string_view(kind) == "query") {
        InputDispatcher dispatcher;
        dispatcher.setSceneRoot(&view);
        auto keyboardReply = [&] {
            revealBubbleText(renderer, view, {17, 23, 512, transcriptHeight}, "Compact-11", false);
            const auto outgoing = body(view, "Compact-11");
            dispatcher.setFocus(nullptr);
            bool reached = false;
            for (int i = 0; i < 64 && dispatcher.cycleTabFocus(false); ++i)
                if (dispatcher.focusedArea() == &outgoing.row) {
                    reached = true;
                    break;
                }
            assert(reached);
            float bodyX, bodyY, tapX, tapY;
            const auto footerPoint = [&] {
                const auto ink = revealBubbleText(renderer, view, {17, 23, 512, transcriptHeight},
                                                  "Compact-11", true);
                tapX = ink.x + ink.width * .5f;
                tapY = ink.y + ink.height * .5f;
            };
            footerPoint();
            assert(dispatcher.pointerButton(tapX, tapY, BTN_LEFT, true, 3, 30, true));
            assert(dispatcher.pointerButton(tapX, tapY, BTN_LEFT, false, 4, 40, true));
            assert(host.open && host.contains("Copy text") && host.contains("Reply"));
            assert(host.request.inputSerial == 3);
            host.select("Copy text");
            assert(clipboard.clipboardText() == buffer.messages.back().text);
            const auto copied = clipboard.clipboardText();
            const auto bodyInk = revealBubbleText(renderer, view, {17, 23, 512, transcriptHeight},
                                                  "Compact-11", false);
            bodyX = bodyInk.x + bodyInk.width * .5f;
            bodyY = bodyInk.y + bodyInk.height * .5f;
            assert(dispatcher.pointerButton(bodyX, bodyY, BTN_LEFT, true, 5, 50, false));
            dispatcher.pointerMotion(bodyX + 32, bodyY, 6);
            dispatcher.pointerMotion(bodyX, bodyY, 7);
            assert(dispatcher.pointerButton(bodyX, bodyY, BTN_LEFT, false, 8, 80, false));
            assert(!host.open && clipboard.clipboardText() == copied && model.actions.empty());
            assert(dispatcher.pointerButton(bodyX, bodyY, BTN_RIGHT, true, 1, 10, false));
            assert(dispatcher.pointerButton(bodyX, bodyY, BTN_RIGHT, false, 2, 20, false));
            assert(host.open && host.contains("Copy text") && host.contains("Reply"));
            assert(host.request.inputSerial == 1);
            assert(host.request.pointerContextMenu);
            assert(host.request.anchorX == int(std::floor(bodyX)) &&
                   host.request.anchorY == int(std::floor(bodyY)) &&
                   host.request.anchorWidth == 1 && host.request.anchorHeight == 1);
            host.select("Copy text");
            assert(clipboard.clipboardText() == buffer.messages.back().text);
            dispatcher.setFocus(&body(view, "Compact-11").row);
            dispatcher.keyEvent(XKB_KEY_Menu, 0, 0, true);
            dispatcher.keyEvent(XKB_KEY_Menu, 0, 0, false);
            assert(host.open && host.contains("Copy text") && host.contains("Reply") &&
                   host.contains("React/👍") && host.contains("Delete message"));
            assert(!host.request.inputSerial && !host.request.pointerContextMenu);
            const auto anchor =
                intersectBounds(sceneBounds(body(view, "Compact-11").row), nativeHistoryClip(view));
            assert(anchor.width > 0 && anchor.height > 0);
            const LayoutRect requested{float(host.request.anchorX), float(host.request.anchorY),
                                       float(host.request.anchorWidth),
                                       float(host.request.anchorHeight)};
            assert(requested.width > 0 && requested.height > 0);
            const auto overlap = intersectBounds(requested, anchor);
            assert(overlap.width > 0 && overlap.height > 0);
            // Native popup coordinates are integral; outward rounding may add at most one pixel.
            assert(requested.x >= anchor.x - 1 && requested.y >= anchor.y - 1 &&
                   requested.x + requested.width <= anchor.x + anchor.width + 1 &&
                   requested.y + requested.height <= anchor.y + anchor.height + 1);
            const auto clip = nativeHistoryClip(view);
            assert(requested.x >= clip.x - 1 && requested.y >= clip.y - 1 &&
                   requested.x + requested.width <= clip.x + clip.width + 1 &&
                   requested.y + requested.height <= clip.y + clip.height + 1);
            host.select("Reply");
            assert(pane.reply == "message:" + buffer.messages.back().id && pane.focusComposer);
            dispatcher.setFocus(nullptr);
            for (const bool touch : {false, true}) {
                footerPoint();
                auto original = buffer.messages.back();
                assert(dispatcher.pointerButton(tapX, tapY, BTN_LEFT, true, 9, 90, touch));
                dispatcher.setFocus(nullptr);
                buffer.messages.back().id += ":replacement";
                buffer.messages.back().text += ":replacement";
                view.sync();
                view.arrange(renderer, {17, 23, 512, transcriptHeight});
                dispatcher.pointerButton(tapX, tapY, BTN_LEFT, false, 10, 100, touch);
                assert(!host.open && clipboard.clipboardText() == copied && model.actions.empty());
                buffer.messages.back() = std::move(original);
                view.sync();
                view.arrange(renderer, {17, 23, 512, transcriptHeight});
                footerPoint();
                auto id = buffer.messages.back().id;
                assert(dispatcher.pointerButton(tapX, tapY, BTN_LEFT, true, 19, 190, touch));
                dispatcher.setFocus(nullptr);
                buffer.messages.back().id += ":aba";
                view.sync();
                view.arrange(renderer, {17, 23, 512, transcriptHeight});
                buffer.messages.back().id = std::move(id);
                view.sync();
                view.arrange(renderer, {17, 23, 512, transcriptHeight});
                dispatcher.pointerButton(tapX, tapY, BTN_LEFT, false, 20, 200, touch);
                assert(!host.open && clipboard.clipboardText() == copied && model.actions.empty());
                footerPoint();
                assert(dispatcher.pointerButton(tapX, tapY, BTN_LEFT, true, 11, 110, touch));
                view.setVisible(false);
                view.sync();
                view.setVisible(true);
                view.sync();
                view.arrange(renderer, {17, 23, 512, transcriptHeight});
                dispatcher.pointerButton(tapX, tapY, BTN_LEFT, false, 12, 120, touch);
                assert(!host.open && clipboard.clipboardText() == copied && model.actions.empty());
                footerPoint();
                auto text = buffer.messages.back().text;
                assert(dispatcher.pointerButton(tapX, tapY, BTN_LEFT, true, 13, 130, touch));
                buffer.messages.back().text += ":edited";
                view.sync();
                view.arrange(renderer, {17, 23, 512, transcriptHeight});
                dispatcher.pointerButton(tapX, tapY, BTN_LEFT, false, 14, 140, touch);
                assert(!host.open && clipboard.clipboardText() == copied && model.actions.empty());
                buffer.messages.back().text = std::move(text);
                view.sync();
                view.arrange(renderer, {17, 23, 512, transcriptHeight});
                footerPoint();

                auto *heldRow = &body(view, "Compact-11").row;
                auto *scroller = find<ScrollView>(view);
                assert(scroller);
                const float savedOffset = scroller->scrollOffset();
                const float excursion = heldRow->height() + 16;
                assert(savedOffset > excursion);
                assert(dispatcher.pointerButton(tapX, tapY, BTN_LEFT, true, 15, 150, touch));
                dispatcher.setFocus(nullptr);
                scroller->setScrollOffset(savedOffset - excursion);
                view.sync();
                view.arrange(renderer, {17, 23, 512, transcriptHeight});
                assert(heldRow->visible() && heldRow->excludeSubtreeFromTabOrder());
                scroller->setScrollOffset(savedOffset);
                view.sync();
                view.arrange(renderer, {17, 23, 512, transcriptHeight});
                assert(&body(view, "Compact-11").row == heldRow &&
                       !heldRow->excludeSubtreeFromTabOrder());
                dispatcher.pointerButton(tapX, tapY, BTN_LEFT, false, 16, 160, touch);
                assert(!host.open && clipboard.clipboardText() == copied && model.actions.empty());
                footerPoint();

                const std::string originalBuffer = buffer.id;
                const std::string remappedBuffer = buffer.id + ":moved";
                auto &destination = model.buffers[remappedBuffer];
                destination = buffer;
                destination.id = destination.target = remappedBuffer;
                assert(dispatcher.pointerButton(tapX, tapY, BTN_LEFT, true, 17, 170, touch));
                dispatcher.setFocus(nullptr);
                pane.buffer = remappedBuffer;
                view.remapBuffer(originalBuffer, remappedBuffer);
                dispatcher.pointerButton(tapX, tapY, BTN_LEFT, false, 18, 180, touch);
                assert(!host.open && clipboard.clipboardText() == copied && model.actions.empty());
                pane.buffer = originalBuffer;
                view.remapBuffer(remappedBuffer, originalBuffer);
                model.buffers.erase(remappedBuffer);
                view.sync();
                view.arrange(renderer, {17, 23, 512, transcriptHeight});
            }
        };
        keyboardReply();
        buffer.messages.back().reply = buffer.messages.front().id;
        view.sync();
        view.arrange(renderer, {17, 23, 512, transcriptHeight});
        keyboardReply();
        dispatcher.setSceneRoot(nullptr);
        buffer.messages.back().reply.clear();
        view.sync();
        view.arrange(renderer, {17, 23, 512, transcriptHeight});
    }
    const LayoutRect narrow{17, 23, 240, transcriptHeight};
    view.arrange(renderer, narrow);
    auto *scroll = find<ScrollView>(view);
    assert(scroll && scroll->maxScrollOffset() > 0);
    scroll->setScrollOffset(0);
    view.sync();
    view.arrange(renderer, narrow);
    auto &retainedMessage = buffer.messages.front();
    auto retainedBody = [&] { return body(view, retainedMessage.text); };
    auto footerBounds = [&] {
        auto *footer = bubbleFooter(retainedBody().content, 2);
        if (!footer) {
            auto *status = bubbleStatus(retainedBody().content);
            assert(status);
            float x, y;
            Node::absolutePosition(status, x, y);
            return BodyBounds{y, status->height()};
        }
        const auto measured = renderer.measureText(footer->text(), footer->fontSize(),
                                                   footer->font(), footer->maxWidth(), 0,
                                                   footer->textAlign(), footer->ellipsize(), false);
        float x, y;
        Node::absolutePosition(footer, x, y);
        return BodyBounds{y + measured.top, measured.bottom - measured.top};
    };
    const auto separate = footerBounds();
    const auto separateBody = bounds(retainedBody());
    assert(separate.top >= separateBody.top + separateBody.height);
    scroll->setScrollOffset(scroll->scrollOffset() + separate.top - view.y() +
                            separate.height * .5f);
    view.sync();
    view.arrange(renderer, narrow);
    auto retained = footerBounds();
    near(retained.top - view.y(), -retained.height * .5f);
    view.arrange(renderer, {17, 23, 512, transcriptHeight});
    retained = footerBounds();
    near(retained.top - view.y(), -retained.height * .5f);
    const auto widerBody = bounds(retainedBody());
    assert(retained.top >= widerBody.top + widerBody.height - .02f);
    const auto anchored = retainedBody();
    float left, top, right, bottom;
    Node::transformedBounds(&anchored.row, left, top, right, bottom);
    assert(top <= view.y() && bottom > view.y() && !pane.following);
    auto *surface = find<blast::MaterialNode>(anchored.content);
    assert(surface);
    float surfaceX, surfaceY;
    Node::absolutePosition(surface, surfaceX, surfaceY);
    const float gapStart = surfaceY + surface->height();
    assert(bottom > gapStart);
    scroll->setScrollOffset(scroll->scrollOffset() + (gapStart + bottom) * .5f - view.y());
    const float gapOffset = scroll->scrollOffset();
    for (int pass = 0; pass < 3; ++pass) {
        view.sync();
        view.arrange(renderer, {17, 23, 512, transcriptHeight});
        near(scroll->scrollOffset(), gapOffset);
    }
    const float gapRowHeight = retainedBody().row.height();
    view.arrange(renderer, {17, 23, 640, transcriptHeight});
    near(retainedBody().row.height(), gapRowHeight);
    near(scroll->scrollOffset(), gapOffset);
    view.arrange(renderer, {17, 23, 512, transcriptHeight});
    near(scroll->scrollOffset(), gapOffset);

    auto &withReactions = retainedMessage;
    withReactions.reactions["👍"] = {"Me"};
    withReactions.reactions["❤️"] = {"Me", "Peer"};
    withReactions.reactions["🎉"] = {"Peer"};
    for (const bool pendingOnly : {false, true}) {
        model.settings.timestamps = !pendingOnly;
        withReactions.pending = pendingOnly;
        withReactions.sendStatus =
            pendingOnly ? blast::SendStatus::Queued : blast::SendStatus::None;
        if (pendingOnly) {
            withReactions.own = true;
            withReactions.nick = network.nick;
        }
        view.sync();
        view.arrange(renderer, narrow);
        const auto footer = footerBounds();
        scroll->setScrollOffset(scroll->scrollOffset() + footer.top - view.y() +
                                footer.height * .5f);
        view.sync();
        view.arrange(renderer, narrow);
        if (pendingOnly) {
            for (const auto status : {blast::SendStatus::Sent, blast::SendStatus::Confirmed}) {
                withReactions.sendStatus = status;
                view.sync();
                view.arrange(renderer, narrow);
                bubbleDelivery(retainedBody(), withReactions, 2);
                const auto retainedFooter = footerBounds();
                near(retainedFooter.top - view.y(), -retainedFooter.height * .5f);
            }
            withReactions.pending = false;
            withReactions.sendStatus = blast::SendStatus::Unknown;
        } else
            model.settings.timestamps = false;
        view.sync();
        view.arrange(renderer, narrow);
        const auto remaining = retainedBody();
        assert(!bubbleFooter(remaining.content, 2));
        assert(!find<Spinner>(remaining.content));
        assert(!bubbleStatus(remaining.content));
        float reactionBottom = -std::numeric_limits<float>::infinity();
        for (const auto &child : remaining.content.children())
            if (dynamic_cast<Button *>(child.get())) {
                float x, y;
                Node::absolutePosition(child.get(), x, y);
                reactionBottom = std::max(reactionBottom, y + child->height());
            }
        const auto remainingBody = bounds(remaining);
        assert(reactionBottom > remainingBody.top + remainingBody.height);
        assert(view.y() >= reactionBottom - .02f &&
               view.y() - reactionBottom <
                   renderer.fontRowExtent(remaining.text.fontSize(), remaining.text.font()) * .5f);
    }
    assert(model.actions.empty());
}

void compactMeaningfulBodies(TranscriptTestRenderer &renderer, const char *kind) {
    for (const bool own : {false, true})
        for (const int count : {2, 6}) {
            blast::Model model;
            model.settings.scale = 2;
            model.settings.timestamps = true;
            model.settings.avatars = true;
            model.settings.avatarStyle = "monogram";
            model.settings.reducedMotion = true;
            model.settings.themeSource = "builtin";
            blast::Theme theme;
            theme.update(model);
            auto &buffer = model.buffers["meaningful-compact"];
            buffer.id = buffer.target = "meaningful-compact";
            buffer.kind = kind;
            for (int i = 0; i < count; ++i) {
                blast::Message message;
                message.id = "meaningful-" + std::to_string(i);
                message.nick = own ? "Me" : "Peer";
                message.own = own;
                message.time = "2026-11-07T12:34:56Z";
                message.text = i == count - 2   ? "Astra history one."
                               : i == count - 1 ? "Astra history two."
                                                : "Older Astra history " + std::to_string(i);
                buffer.messages.push_back(std::move(message));
            }
            const auto original = buffer;
            blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
            Clipboard clipboard;
            blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
            view.setReadEligible(false);
            view.sync();
            view.arrange(renderer, {17, 23, 900, 800});
            const float nativeBodyFont = body(view, "Astra history two.").text.fontSize();
            for (const float width : {392.f, 424.f}) {
                pane.following = true;
                view.sync();
                view.arrange(renderer, {17, 23, width, 132});
                auto *scroll = find<ScrollView>(view);
                assert(scroll);
                scroll->setScrollOffset(scroll->maxScrollOffset());
                view.sync();
                view.arrange(renderer, {17, 23, width, 132});
                const auto check = [&](const char *sentence) {
                    const auto rendered = body(view, sentence);
                    const auto position = bounds(rendered);
                    const auto strut =
                        renderer.measureText("Mg", nativeBodyFont, rendered.text.font(), 0, 1);
                    near(rendered.text.fontSize(), nativeBodyFont);
                    assert(position.height <= strut.bottom - strut.top + .02f);
                    bubbleGeometry(renderer, rendered, 2, "12:34");
                    auto *footer = bubbleFooter(rendered.content, 2);
                    assert(footer && footer->fontSize() < rendered.text.fontSize());
                    if (auto *author = plainText(rendered.content, own ? "Me" : "Peer")) {
                        RectNode *avatar = nullptr;
                        for (const auto &child : rendered.content.children())
                            if (auto *rect = dynamic_cast<RectNode *>(child.get());
                                rect && rect->visible() && rect->style().fill.a > 0 &&
                                std::abs(rect->width() - rect->height()) < .02f &&
                                (!avatar || rect->width() > avatar->width()))
                                avatar = rect;
                        assert(avatar && plainText(rendered.content, own ? "M" : "P"));
                        const auto avatarBounds = sceneBounds(*avatar);
                        const auto authorInk = nativeTextInk(renderer, *author);
                        assert(avatarBounds.y + avatarBounds.height <= position.top + .02f);
                        assert(authorInk.x + authorInk.width <= avatarBounds.x + .02f ||
                               avatarBounds.x + avatarBounds.width <= authorInk.x + .02f ||
                               authorInk.y + authorInk.height <= avatarBounds.y + .02f ||
                               avatarBounds.y + avatarBounds.height <= authorInk.y + .02f);
                        containedBounds(authorInk, sceneBounds(*bubbleSurface(rendered)));
                    }
                    reachableBubble(renderer, view, {17, 23, width, 132}, sentence);
                };
                check("Astra history one.");
                check("Astra history two.");
                scroll->setScrollOffset(0);
                view.sync();
                view.arrange(renderer, {17, 23, width, 132});
                for (int i = 0; i < count - 2; ++i) {
                    const auto &message = buffer.messages[std::size_t(i)];
                    reachableBubble(renderer, view, {17, 23, width, 132}, message.text);
                    near(body(view, message.text, true).text.fontSize(), nativeBodyFont);
                }
            }
            assert(model.actions.empty());
            unchanged(buffer, original);
        }
}

void bubbleConversation(TranscriptTestRenderer &renderer, float zoom, const char *kind) {
    blast::Model model;
    model.settings.scale = zoom;
    model.settings.messageScale = zoom;
    model.settings.timestamps = true;
    model.settings.avatars = false;
    model.settings.reducedMotion = true;
    model.settings.nickColors["Alice"] = "#70c0ff";
    model.settings.nickColors["Me"] = "#e6b86a";
    model.settings.themeSource = "builtin";
    blast::Theme theme;
    theme.update(model);
    auto &buffer = model.buffers["network/conversation"];
    buffer.id = "network/conversation";
    buffer.target = "conversation";
    buffer.kind = kind;
    for (int i = 0; i < 8; ++i) {
        blast::Message message;
        message.id = "bubble-" + std::to_string(i);
        message.nick = i < 2 || i == 4 || i == 5 ? "Alice" : "Me";
        message.own = i == 2 || i == 3 || i >= 6;
        message.text = i == 0   ? "IncomingOne"
                       : i == 1 ? "IncomingGrouped"
                       : i == 2 ? "OutgoingOne"
                       : i == 3 ? "OutgoingGrouped"
                       : i == 4 ? "ActionOne"
                       : i == 5 ? "WrappedBubble \x02native rich text\x02 and "
                                  "https://example.org/footer wrap across several lines while "
                                  "the quote, reactions and timestamp keep their own space."
                       : i == 6 ? "PendingBubble"
                                : "DeletedBubble";
        message.time = "2026-11-07T12:" + std::to_string(34 + i) + ":56Z";
        message.action = i == 4;
        message.pending = i == 6;
        message.sendStatus = i == 6   ? blast::SendStatus::Queued
                             : i == 2 ? blast::SendStatus::Sent
                             : i == 3 ? blast::SendStatus::Confirmed
                                      : blast::SendStatus::None;
        message.deleted = i == 7;
        if (i == 5 || i == 6) {
            message.reply = "bubble-0";
            message.reactions["👍"] = {"Alice"};
            message.reactions["❤️"] = {"Me", "Alice"};
            message.reactions["🎉"] = {"Me"};
        }
        buffer.messages.push_back(std::move(message));
    }
    buffer.readMarker = buffer.messages.back().id;
    const auto before = buffer;
    blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
    Clipboard clipboard;
    MenuHost host;
    blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
    view.setPopupContext(&host);
    view.setReadEligible(false);
    const LayoutRect wide{17, 23, 800 * zoom, 4000 * zoom};
    view.sync();
    view.arrange(renderer, wide);
    const auto messageBody = [&](std::size_t index) {
        const auto &message = buffer.messages[index];
        const std::string displayed = message.deleted
                                          ? "Message deleted"
                                          : blast::parseIrcFormatting(message.text, {}, {}).text;
        return body(view, displayed, true);
    };
    assert(blast::messageBubbles(model.settings, buffer));
    const auto incoming = body(view, "IncomingOne");
    const auto grouped = body(view, "IncomingGrouped");
    const auto outgoing = body(view, "OutgoingOne");
    auto dayChip = [&] {
        auto *day = plainText(body(view, "IncomingOne").row, "Undated history");
        assert(day && day->parent());
        auto *label = dynamic_cast<Label *>(day->parent());
        auto *chip = label ? dynamic_cast<InputArea *>(label->parent()) : nullptr;
        assert(chip && !chip->focusable());
        near(day->fontSize(), 12 * zoom);
        assert(day->fontWeight() == FontWeight::Medium);
        float x, y;
        Node::absolutePosition(chip, x, y);
        near(x + chip->width() * .5f, view.x() + view.width() * .5f);
        auto *background = find<RectNode>(*chip);
        assert(background);
        assert(background->style().fill == blast::themeSurfaceColor("surface_container_high"));
        return chip;
    };
    {
        auto *chip = dayChip();
        float x, y;
        Node::mapToScene(chip, chip->width() * .5f, chip->height() * .5f, x, y);
        InputDispatcher pointer;
        pointer.setSceneRoot(&view);
        for (const auto button : {BTN_LEFT, BTN_RIGHT}) {
            assert(pointer.pointerButton(x, y, button, true, 81, 810, false));
            pointer.pointerButton(x, y, button, false, 82, 820, false);
            assert(!host.open && !pointer.focusedArea() && pane.reply.empty());
        }
        pointer.setSceneRoot(nullptr);
    }
    auto *left = find<blast::MaterialNode>(incoming.content);
    auto *right = find<blast::MaterialNode>(outgoing.content);
    assert(left && right && left->visible() && right->visible());
    assert(left->x() < incoming.row.width() * .5f && right->x() > outgoing.row.width() * .5f);
    assert(plainText(incoming.content, "Alice") && plainText(outgoing.content, "Me") &&
           !plainText(grouped.content, "Alice") &&
           !plainText(body(view, "OutgoingGrouped").content, "Me"));
    for (const auto &[rendered, nick, expected] :
         {std::tuple{incoming, "Alice", Color{112.f / 255, 192.f / 255, 1, 1}},
          std::tuple{outgoing, "Me", Color{230.f / 255, 184.f / 255, 106.f / 255, 1}}}) {
        auto *chip = plainText(rendered.content, nick);
        assert(chip);
        auto *footer = bubbleFooter(rendered.content, zoom);
        assert(footer && footer->fontSize() < chip->fontSize() &&
               chip->fontSize() < rendered.text.fontSize());
        assert(chip->fontWeight() == FontWeight::Bold);
        const auto foreground = readableTextColorForBackground(expected);
        near(chip->color().r, foreground.r);
        near(chip->color().g, foreground.g);
        near(chip->color().b, foreground.b);
        near(chip->color().a, 1);
        RectNode *background = nullptr;
        for (const auto &child : rendered.content.children())
            if (auto *rect = dynamic_cast<RectNode *>(child.get());
                rect && rect->style().fill == expected)
                background = rect;
        assert(background);
        float chipX, chipY, bodyX, bodyY;
        Node::absolutePosition(chip, chipX, chipY);
        Node::absolutePosition(&rendered.fragment, bodyX, bodyY);
        const auto measured =
            renderer.measureText(chip->text(), chip->fontSize(), chip->font(), chip->maxWidth(), 1,
                                 chip->textAlign(), chip->ellipsize(), false);
        const auto natural =
            renderer.measureText(chip->text(), chip->fontSize(), chip->font(), 0, 1);
        // Short native labels retain every glyph, not an ellipsis caused by a fractional limit.
        near(measured.width, natural.width);
        assert(chipY + measured.bottom <= bodyY + .02f);
        float backgroundX, backgroundY;
        Node::absolutePosition(background, backgroundX, backgroundY);
        assert(chipX + measured.inkLeft >= backgroundX - .02f &&
               chipX + measured.inkRight <= backgroundX + background->width() + .02f);
        assert(chipY + measured.inkTop >= backgroundY - .02f &&
               chipY + measured.inkBottom <= backgroundY + background->height() + .02f);
    }
    assert(grouped.row.height() < incoming.row.height());
    const auto action = body(view, "ActionOne");
    assert(action.text.text().find("style=\"italic\"") != std::string::npos);
    assert(!find<blast::MaterialNode>(action.content)->visible());
    assert(!bubbleSurface(action));
    assert(action.text.color() == blast::themeForegroundColor("on_surface"));
    auto geometry = [&] {
        dayChip();
        for (std::size_t i = 0; i < buffer.messages.size(); ++i) {
            const auto rendered = messageBody(i);
            if (buffer.messages[i].action) {
                assert(!bubbleSurface(rendered));
                auto *actor = plainText(rendered.content, "↳ " + buffer.messages[i].nick);
                assert(actor && actor->fontWeight() == FontWeight::Bold &&
                       actor->color() ==
                           blast::nickColor(model, buffer.messages[i].nick, buffer.server));
                auto *clock = bubbleFooter(rendered.content, zoom);
                assert(clock && clock->text() == "12:" + std::to_string(34 + i) &&
                       clock->fontSize() < rendered.text.fontSize() &&
                       clock->color() == blast::themeForegroundColor("on_surface_variant"));
                containedBounds(nativeTextInk(renderer, rendered.text), sceneBounds(rendered.row));
                containedBounds(nativeTextInk(renderer, *clock), sceneBounds(rendered.row));
            } else
                bubbleGeometry(renderer, rendered, zoom, "12:" + std::to_string(34 + i));
            bubbleDelivery(rendered, buffer.messages[i], zoom);
            if (buffer.messages[i].deleted) {
                const auto original =
                    blast::parseIrcFormatting(buffer.messages[i].text, {}, {}).text;
                assert(!bodyText(view, original, true));
                assert(!rendered.row.focusable());
                assert(!view.messageReadable(blast::ui_logic::messageKey(buffer.messages[i])));
            }
        }
    };
    geometry();
    const float withFooter = grouped.row.height();
    model.settings.timestamps = false;
    view.sync();
    view.arrange(renderer, wide);
    assert(body(view, "IncomingGrouped").row.height() < withFooter);
    for (std::size_t i = 0; i < buffer.messages.size(); ++i) {
        const auto rendered = messageBody(i);
        near(rendered.text.fontSize(), 14 * zoom);
        assert(!bubbleFooter(rendered.content, zoom));
        auto *pending = find<Spinner>(rendered.content);
        bubbleDelivery(rendered, buffer.messages[i], zoom);
        if (pending) {
            assert(!pending->spinning());
            near(pending->width(), 12 * zoom);
            near(pending->height(), 12 * zoom);
            auto *status = bubbleStatus(rendered.content);
            assert(status && status->y() >= rendered.y() + rendered.fragment.height() - .02f);
        }
    }
    model.settings.timestamps = true;
    view.sync();
    view.arrange(renderer, wide);
    geometry();
    near(body(view, "IncomingGrouped").row.height(), withFooter);
    buffer.messages.front().pending = true;
    for (const auto status : {blast::SendStatus::Queued, blast::SendStatus::Sent,
                              blast::SendStatus::Confirmed, blast::SendStatus::Unknown}) {
        buffer.messages.front().sendStatus = status;
        view.sync();
        view.arrange(renderer, wide);
        geometry();
        assert(!bubbleStatus(body(view, "IncomingOne").content));
    }
    buffer.messages.front().pending = false;
    buffer.messages.front().sendStatus = blast::SendStatus::None;
    view.sync();
    view.arrange(renderer, wide);
    auto &delivery = buffer.messages[6];
    delivery.pending = false; // Legacy timeout is not evidence of server acceptance.
    for (const auto status :
         {blast::SendStatus::Queued, blast::SendStatus::Sent, blast::SendStatus::Confirmed,
          blast::SendStatus::Unknown, blast::SendStatus::None}) {
        delivery.sendStatus = status;
        view.sync();
        view.arrange(renderer, wide);
        geometry();
    }
    delivery.pending = true;
    delivery.sendStatus = blast::SendStatus::Queued;
    model.settings.reducedMotion = false;
    model.settings.fx = blast::FxLevel::Subtle;
    view.sync();
    view.arrange(renderer, wide);
    auto *animatedPending = find<Spinner>(body(view, "PendingBubble").content);
    assert(animatedPending && animatedPending->spinning());
    model.settings.fx = blast::FxLevel::Off;
    view.sync();
    view.arrange(renderer, wide);
    auto *staticPending = find<Spinner>(body(view, "PendingBubble").content);
    assert(staticPending && !staticPending->spinning());
    model.settings.reducedMotion = true;
    view.sync();
    view.arrange(renderer, wide);
    const float bubbleHeight = incoming.row.height();
    model.settings.messageLayouts[buffer.id] = blast::MessageLayout::Irc;
    view.sync();
    view.arrange(renderer, wide);
    assert(!blast::messageBubbles(model.settings, buffer));
    assert(!find<blast::MaterialNode>(body(view, "IncomingOne").content)->visible());
    assert(body(view, "IncomingOne").row.height() < bubbleHeight);
    near(body(view, "IncomingOne").x(), body(view, "OutgoingOne").x());
    assert(!bubbleStatus(body(view, "PendingBubble").content));
    assert(!bubbleStatus(body(view, "OutgoingOne").content));
    model.settings.messageLayouts.erase(buffer.id);
    view.sync();
    view.arrange(renderer, wide);
    near(body(view, "IncomingOne").row.height(), bubbleHeight);
    assert(find<blast::MaterialNode>(body(view, "OutgoingOne").content)->visible());
    model.settings.messageLayouts[buffer.id] = blast::MessageLayout::Stacked;
    view.sync();
    view.arrange(renderer, wide);
    assert(!blast::messageBubbles(model.settings, buffer));
    assert(!find<blast::MaterialNode>(body(view, "IncomingOne").content)->visible());
    model.settings.messageLayouts.erase(buffer.id);
    view.sync();
    view.arrange(renderer, wide);
    near(body(view, "IncomingOne").row.height(), bubbleHeight);
    for (const float width : {240 * zoom, 240.f}) {
        view.arrange(renderer, {17, 23, width, wide.height});
        geometry();
    }
    model.settings.avatars = true;
    view.sync();
    view.arrange(renderer, {17, 23, 240 * zoom, wide.height});
    geometry();
    float avatarSize = 0;
    for (const char *avatarStyle : {"monogram", "identicon"}) {
        model.settings.avatarStyle = avatarStyle;
        view.sync();
        view.arrange(renderer, {17, 23, 240 * zoom, wide.height});
        geometry();
        for (const auto &[token, initial] :
             {std::pair{"IncomingOne", "A"}, std::pair{"OutgoingOne", "M"}}) {
            const auto rendered = body(view, token);
            const bool monogram = std::string_view(avatarStyle) == "monogram";
            Node *avatar = nullptr;
            if (monogram) {
                for (const auto &child : rendered.content.children())
                    if (auto *rect = dynamic_cast<RectNode *>(child.get());
                        rect && rect->visible() &&
                        std::abs(rect->width() - rect->height()) < .02f &&
                        (!avatar || rect->width() > avatar->width()))
                        avatar = rect;
                assert(avatar);
                avatarSize = avatar->width();
            } else {
                auto *image = find<Image>(rendered.content);
                assert(image && image->visible() && image->hasImage());
                avatar = image;
                near(avatar->width(), avatarSize);
            }
            near(avatar->width(), avatar->height());
            containedBounds(sceneBounds(*avatar), sceneBounds(rendered.row));
            const float centerX = avatar->x() + avatar->width() * .5f;
            const float centerY = avatar->y() + avatar->height() * .5f;
            if (monogram) {
                auto *glyph = plainText(rendered.content, initial);
                assert(glyph && glyph->visible() && glyph->parent() == &rendered.content);
                containedBounds(nativeTextInk(renderer, *glyph), sceneBounds(*avatar));
                const auto measured = renderer.measureText(
                    glyph->text(), glyph->fontSize(), glyph->font(), glyph->maxWidth(), 1,
                    glyph->textAlign(), glyph->ellipsize(), false);
                near(glyph->x() - measured.alignOffsetX +
                         (measured.inkLeft + measured.inkRight) * .5f,
                     centerX);
                near(glyph->y() + (measured.inkTop + measured.inkBottom) * .5f, centerY);
            }
        }
    }
    const float initialOpacity = model.settings.opacity;
    const bool initialTransparency = model.settings.transparency;
    for (const float opacity : {.2f, .85f, 1.f}) {
        model.settings.transparency = true;
        model.settings.opacity = opacity;
        theme.update(model);
        view.sync();
        view.arrange(renderer, {17, 23, 240 * zoom, wide.height});
        auto *background = find<RectNode>(*dayChip());
        assert(background);
        near(background->style().fill.a, opacity);
    }
    model.settings.opacity = initialOpacity;
    model.settings.transparency = initialTransparency;
    theme.update(model);
    view.sync();
    view.arrange(renderer, {17, 23, 240 * zoom, wide.height});
    blast::Buffer other = buffer;
    other.id = "another/conversation";
    model.settings.messageLayouts[buffer.id] = blast::MessageLayout::Irc;
    assert(!blast::messageBubbles(model.settings, buffer) &&
           blast::messageBubbles(model.settings, other));
    other.kind = "server";
    assert(!blast::messageBubbles(model.settings, other));
    unchanged(buffer, before);
    assert(model.actions.empty());
}

void tailAppendKeepsActions(TranscriptTestRenderer &renderer) {
    blast::Model model;
    model.settings.timestamps = false;
    model.settings.avatars = false;
    model.settings.reducedMotion = true;
    model.settings.themeSource = "builtin";
    blast::Theme theme;
    theme.update(model);
    auto &buffer = model.buffers["append"];
    buffer.id = buffer.target = "append";
    buffer.kind = "query";
    buffer.server = "network";
    buffer.messagesRevision = 1;
    auto &network = model.networks[buffer.server];
    network.state = "Ready";
    network.nick = "Me";
    network.caps = {"message-tags", "draft/message-redaction"};
    for (int i = 0; i < 30; ++i) {
        blast::Message message;
        message.id = "append-" + std::to_string(i);
        message.text = "OldRow-" + std::to_string(i);
        message.nick = network.nick;
        message.own = true;
        buffer.messages.push_back(std::move(message));
    }
    blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
    Clipboard clipboard;
    MenuHost host;
    blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
    view.setPopupContext(&host);
    view.setReadEligible(false);
    const LayoutRect viewport{17, 23, 700, 160};
    view.sync();
    view.arrange(renderer, viewport);
    auto *scroll = find<ScrollView>(view);
    assert(scroll && scroll->maxScrollOffset() > 0);
    scroll->setScrollOffset(0);
    view.sync();
    view.arrange(renderer, viewport);
    const std::string oldId = buffer.messages.front().id;
    const std::string oldText = buffer.messages.front().text;
    const auto retained = body(view, oldText);
    const auto before = bounds(retained);
    float x, y;
    auto *surface = bubbleSurface(retained);
    assert(surface && retained.x() > surface->x());
    Node::mapToScene(surface, (retained.x() - surface->x()) * .5f,
                     retained.y() - surface->y() + retained.fragment.height() * .5f, x, y);
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&view);
    view.setInputDispatcher(dispatcher);
    auto composer = std::make_unique<Input>();
    auto *input = composer.get();
    input->setValue("draft");
    view.addChild(std::move(composer));
    input->arrange(renderer, {12, 120, 240, 32});
    dispatcher.setFocus(input->inputArea());
    TextInputEdit preedit;
    preedit.preeditText = "compose";
    preedit.preeditCursorBegin = preedit.preeditCursorEnd = 7;
    preedit.hasPreedit = true;
    input->textInputApplyEdit(preedit);
    input->arrange(renderer, {12, 120, 240, 32});
    assert(input->composing());
    const auto composition = input->editingState();
    assert(dispatcher.pointerButton(x, y, BTN_LEFT, true, 54, 540, true));
    assert(retained.row.pressed() && dispatcher.focusedArea() == input->inputArea());
    dispatcher.pointerMotion(x + 6, y, 55);
    dispatcher.pointerMotion(x, y, 56);
    dispatcher.pointerButton(x, y, BTN_LEFT, false, 57, 570, true);
    assert(!host.open && model.actions.empty() && scroll->scrollOffset() == 0);
    assert(dispatcher.focusedArea() == input->inputArea() &&
           input->editingStateMatches(composition));
    assert(dispatcher.pointerButton(x, y, BTN_LEFT, true, 51, 510, true));
    assert(retained.row.pressed());
    assert(dispatcher.focusedArea() == input->inputArea() &&
           input->editingStateMatches(composition));
    dispatcher.pointerMotion(x, y - 48, 52);
    view.sync();
    view.arrange(renderer, viewport);
    assert(!retained.row.pressed() && scroll->scrollOffset() > 0);
    dispatcher.pointerButton(x, y - 48, BTN_LEFT, false, 53, 530, true);
    assert(!host.open && model.actions.empty());
    assert(dispatcher.focusedArea() == input->inputArea() &&
           input->editingStateMatches(composition));
    scroll->setScrollOffset(0);
    view.sync();
    view.arrange(renderer, viewport);
    float blankX, blankY;
    assert(surface->x() > 2);
    Node::mapToScene(&retained.row, surface->x() * .5f, surface->y() + surface->height() * .5f,
                     blankX, blankY);
    assert(dispatcher.pointerButton(blankX, blankY, BTN_LEFT, true, 58, 580, true));
    dispatcher.pointerMotion(blankX, blankY - 48, 59);
    view.sync();
    view.arrange(renderer, viewport);
    assert(scroll->scrollOffset() > 0);
    dispatcher.pointerButton(blankX, blankY - 48, BTN_LEFT, false, 60, 600, true);
    assert(!host.open && model.actions.empty() && dispatcher.focusedArea() == input->inputArea() &&
           input->editingStateMatches(composition));
    scroll->setScrollOffset(0);
    view.sync();
    view.arrange(renderer, viewport);
    assert(dispatcher.pointerButton(x, y, BTN_LEFT, true, 41, 410, true));
    assert(dispatcher.focusedArea() == input->inputArea() &&
           input->editingStateMatches(composition));
    blast::Message arrival;
    arrival.id = "append-tail";
    arrival.text = "OffscreenArrival";
    arrival.nick = network.nick;
    arrival.own = true;
    const auto identityEpoch = buffer.messagesRevision;
    buffer.messages.push_back(std::move(arrival));
    assert(buffer.messagesRevision == identityEpoch);
    view.sync();
    view.arrange(renderer, viewport);
    const auto after = bounds(body(view, oldText));
    near(after.top, before.top);
    near(after.height, before.height);
    assert(!pane.following);
    assert(dispatcher.pointerButton(x, y, BTN_LEFT, false, 42, 420, true));
    assert(host.open && host.contains("Reply") && host.request.inputSerial == 41);
    host.select("Reply");
    assert(pane.reply == "message:" + oldId && pane.focusComposer);
    dispatcher.setFocus(&body(view, oldText).row);
    dispatcher.keyEvent(XKB_KEY_F10, 0, KeyMod::Shift, true);
    dispatcher.keyEvent(XKB_KEY_F10, 0, KeyMod::Shift, false);
    assert(host.open && host.contains("React/👍"));
    host.select("React/👍");
    assert(model.actions.size() == 1 && model.actions.back().type == "react" &&
           model.actions.back().buffer == buffer.id && model.actions.back().extra == oldId);
    model.actions.clear();
    pane.focusComposer = false;
    pane.following = true;
    view.sync();
    view.arrange(renderer, viewport);
    InputArea *revealTarget = nullptr;
    for (size_t i = buffer.messages.size() - 2; i >= 10; --i)
        if (bodyText(view, buffer.messages[i].text, true)) {
            const auto candidate = body(view, buffer.messages[i].text, true);
            float left, top, right, bottom;
            Node::transformedBounds(&candidate.row, left, top, right, bottom);
            if (bottom <= view.y()) {
                revealTarget = &candidate.row;
                break;
            }
        }
    assert(revealTarget);
    const float tail = scroll->scrollOffset();
    dispatcher.setFocus(revealTarget);
    scroll->requestScrollIntoView(revealTarget);
    view.noteFocusReveal();
    view.sync();
    view.arrange(renderer, viewport);
    const float revealed = scroll->scrollOffset();
    assert(revealed < tail && !pane.following);
    view.sync();
    view.arrange(renderer, viewport);
    near(scroll->scrollOffset(), revealed);
    const float userOffset = std::min(scroll->maxScrollOffset(), revealed + 5);
    scroll->setScrollOffset(userOffset);
    view.sync();
    view.arrange(renderer, viewport);
    near(scroll->scrollOffset(), userOffset);
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void focusRevealIntents(TranscriptTestRenderer &renderer) {
    blast::Model model;
    model.settings.timestamps = false;
    model.settings.avatars = false;
    model.settings.reducedMotion = true;
    model.settings.themeSource = "builtin";
    blast::Theme theme;
    theme.update(model);
    auto &buffer = model.buffers["reveal"];
    buffer.id = buffer.target = "reveal";
    buffer.kind = "query";
    buffer.messagesRevision = 1;
    for (int i = 0; i < 30; ++i) {
        blast::Message message;
        message.id = "reveal-" + std::to_string(i);
        message.text =
            "RevealRow-" + std::to_string(i) + " https://example.org/reveal/" + std::to_string(i);
        message.nick = "Peer";
        message.time = "2026-11-07T12:34:56Z";
        if (i == 29)
            message.reply = "reveal-0";
        buffer.messages.push_back(std::move(message));
    }
    blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
    Clipboard clipboard;
    blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&view);
    view.setInputDispatcher(dispatcher);
    view.setReadEligible(false);
    const LayoutRect viewport{17, 23, 700, 180};
    auto arrange = [&] {
        view.sync();
        view.arrange(renderer, viewport);
    };
    arrange();
    auto *scroll = find<ScrollView>(view);
    assert(scroll && scroll->maxScrollOffset() > 0);
    InputArea *keyboard = nullptr;
    auto findKeyboard = [&](auto &&self, Node &node) -> void {
        if (auto *area = dynamic_cast<InputArea *>(&node);
            area && area->tabFocusKey().starts_with("transcript:") &&
            area->tabFocusKey().ends_with(":keyboard"))
            keyboard = area;
        for (const auto &child : node.children())
            self(self, *child);
    };
    findKeyboard(findKeyboard, view);
    assert(keyboard);
    auto tail = [&] {
        dispatcher.setFocus(nullptr);
        pane.following = true;
        pane.unseen = 0;
        arrange();
        near(scroll->scrollOffset(), scroll->maxScrollOffset());
    };
    auto queueReveal = [&] {
        InputArea *target = nullptr;
        std::string targetText;
        for (size_t i = buffer.messages.size() - 2; i >= 10; --i)
            if (bodyText(view, buffer.messages[i].text, true)) {
                const auto candidate = body(view, buffer.messages[i].text, true);
                float left, top, right, bottom;
                Node::transformedBounds(&candidate.row, left, top, right, bottom);
                if (bottom <= view.y()) {
                    target = &candidate.row;
                    targetText = buffer.messages[i].text;
                    break;
                }
            }
        assert(target);
        dispatcher.setFocus(target);
        scroll->requestScrollIntoView(target);
        view.noteFocusReveal();
        return targetText;
    };

    // Boundaries operate on the loaded transcript, never the older-history action.
    // Keep the keyboard canvas focused so layout cannot legitimately transfer focus
    // when a virtualized message leaves the viewport.
    tail();
    dispatcher.setFocus(keyboard);
    pane.focusComposer = false;
    const auto loaded = buffer;
    assert(view.scrollToBoundary(false));
    near(scroll->scrollOffset(), 0);
    assert(!pane.following && dispatcher.focusedArea() == keyboard && !pane.focusComposer);
    arrange();
    near(scroll->scrollOffset(), 0);
    assert(!pane.following && dispatcher.focusedArea() == keyboard);
    const auto firstInk = nativeTextInk(renderer, body(view, "RevealRow-0").text);
    containedBounds(firstInk, nativeHistoryClip(view));
    auto *olderLabel = plainText(view, "Load older messages");
    assert(olderLabel && !buffer.loading);
    auto *older = olderLabel->parent();
    while (older && !dynamic_cast<Button *>(older))
        older = older->parent();
    assert(older && static_cast<Button *>(older)->enabled());
    unchanged(buffer, loaded);
    assert(model.actions.empty());

    // Select actual native body text, then collect an arrival while at the beginning.
    auto &selectionText = body(view, "RevealRow-0").text;
    const auto selectionPoint = [&](std::size_t byte) {
        std::vector<TextCursorStop> stops;
        renderer.measureTextCaretStops(selectionText.text(), selectionText.fontSize(), {byte}, 0,
                                       stops, selectionText.font(), true);
        assert(stops.size() == 1);
        const auto metrics = renderer.measureText(selectionText.text(), selectionText.fontSize(),
                                                  selectionText.font(), 0, 0, TextAlign::Start,
                                                  TextEllipsize::End, true);
        float x, y;
        Node::absolutePosition(&selectionText, x, y);
        return std::pair{x - metrics.alignOffsetX + stops[0].x,
                         y - std::floor(metrics.layoutBaseline) + stops[0].y +
                             stops[0].height * .5f};
    };
    const auto from = selectionPoint(0), to = selectionPoint(9);
    assert(dispatcher.pointerButton(from.first, from.second, BTN_LEFT, true, 71, 710, false));
    dispatcher.pointerMotion(to.first, to.second, 72);
    dispatcher.pointerButton(to.first, to.second, BTN_LEFT, false, 73, 730, false);
    assert(view.copySelection() && clipboard.clipboardText() == "RevealRow");
    blast::Message boundaryArrival;
    boundaryArrival.id = "boundary-arrival";
    boundaryArrival.nick = "Peer";
    boundaryArrival.text = "BoundaryArrival";
    buffer.messages.push_back(boundaryArrival);
    arrange();
    assert(!pane.following && pane.unseen == 1);
    dispatcher.setFocus(keyboard);
    assert(view.scrollToBoundary(true));
    assert(dispatcher.focusedArea() == keyboard && !pane.focusComposer);
    assert(pane.following && pane.unseen == 0 && !view.copySelection());
    assert(clipboard.clipboardText() == "RevealRow");
    arrange();
    near(scroll->scrollOffset(), scroll->maxScrollOffset());
    containedBounds(nativeTextInk(renderer, body(view, "BoundaryArrival").text),
                    nativeHistoryClip(view));

    // Each independent deferred reveal must lose to the newest boundary, including
    // subsequent native layouts and a new, genuinely tall arrival.
    for (int pending = 0; pending < 3; ++pending) {
        if (pending == 0) {
            queueReveal();
        } else if (pending == 1) {
            assert(view.jumpToSourceMessage(buffer.messages.front().id));
        } else {
            dispatcher.setFocus(&body(view, "BoundaryArrival").row);
            assert(view.navigateMessage(-1));
        }
        auto *focusBeforeBoundary = dispatcher.focusedArea();
        assert(view.scrollToBoundary(true));
        assert(dispatcher.focusedArea() == focusBeforeBoundary && !pane.focusComposer);
        arrange();
        arrange();
        near(scroll->scrollOffset(), scroll->maxScrollOffset());
        assert(pane.following && pane.unseen == 0 && model.actions.empty());
        containedBounds(nativeTextInk(renderer, body(view, "BoundaryArrival").text),
                        nativeHistoryClip(view));
    }
    dispatcher.setFocus(keyboard);
    boundaryArrival.id = "boundary-tall-arrival";
    boundaryArrival.text = "BoundaryTallArrival";
    for (int i = 0; i < 48; ++i)
        boundaryArrival.text += "\nMeasured native tail follows the boundary command.";
    buffer.messages.push_back(boundaryArrival);
    arrange();
    arrange();
    near(scroll->scrollOffset(), scroll->maxScrollOffset());
    const auto tallBounds = sceneBounds(body(view, "BoundaryTallArrival").body);
    const auto tailClip = nativeHistoryClip(view);
    assert(tallBounds.height > tailClip.height);
    const auto exposedTail = intersectBounds(tallBounds, tailClip);
    assert(exposedTail.height > 0);
    assert(tallBounds.y + tallBounds.height <= tailClip.y + tailClip.height + .5f);
    assert(pane.following && pane.unseen == 0 && dispatcher.focusedArea() == keyboard &&
           !pane.focusComposer && model.actions.empty());
    buffer.messages.pop_back();
    buffer.messages.pop_back();
    ++buffer.messagesRevision;
    tail();

    // Moving to the canvas is not another reveal; it must invalidate the old action's request.
    tail();
    const float canvasOffset = scroll->scrollOffset();
    queueReveal();
    dispatcher.setFocus(keyboard);
    arrange();
    near(scroll->scrollOffset(), canvasOffset);
    assert(dispatcher.focusedArea() == keyboard && pane.following);

    queueReveal();
    assert(view.handleKey({.sym = XKB_KEY_End, .pressed = true}));
    arrange();
    near(scroll->scrollOffset(), scroll->maxScrollOffset());
    assert(pane.following);
    queueReveal();
    assert(view.handleKey({.sym = XKB_KEY_Home, .pressed = true}));
    arrange();
    near(scroll->scrollOffset(), 0);
    assert(!pane.following);

    // A real reply-preview activation supersedes the queued action reveal without losing its jump.
    tail();
    queueReveal();
    const auto replyBody = body(view, "RevealRow-29");
    InputArea *reference = nullptr;
    for (const auto &child : replyBody.content.children())
        if (auto *area = dynamic_cast<InputArea *>(child.get());
            area && area->focusable() && area->tabFocusKey().find("5:reply") != std::string::npos)
            reference = area;
    assert(reference);
    float x, y;
    Node::mapToScene(reference, reference->width() * .5f, reference->height() * .5f, x, y);
    assert(dispatcher.pointerButton(x, y, BTN_LEFT, true, 81, 810, true));
    dispatcher.pointerButton(x, y, BTN_LEFT, false, 82, 820, true);
    arrange();
    near(bounds(body(view, "RevealRow-0")).top, viewport.y);
    assert(!pane.following && model.actions.empty());

    // A failed pending tail disappearing is not a replay of every retained message.
    for (const bool withIncoming : {false, true}) {
        tail();
        scroll->setScrollOffset(scroll->maxScrollOffset() * .5f);
        arrange();
        const std::string suffix = withIncoming ? "with-arrival" : "without-arrival";
        blast::Message prior;
        prior.id = "prior-" + suffix;
        prior.text = "PriorUnread-" + suffix;
        prior.nick = "Peer";
        buffer.messages.push_back(std::move(prior));
        arrange();
        assert(!pane.following && pane.unseen == 1);
        blast::Message failed;
        failed.id = "failed-" + suffix;
        failed.label = "failed-send-" + suffix;
        failed.text = "FailedSend-" + suffix;
        failed.nick = "Me";
        failed.own = failed.pending = true;
        buffer.messages.push_back(std::move(failed));
        arrange();
        const int oldUnseen = pane.unseen;
        queueReveal();
        buffer.messages.pop_back();
        ++buffer.messagesRevision;
        if (withIncoming) {
            blast::Message incoming;
            incoming.id = "after-failure";
            incoming.text = "IncomingAfterFailure";
            incoming.nick = "Peer";
            buffer.messages.push_back(std::move(incoming));
        }
        view.sync();
        view.sync();
        view.arrange(renderer, viewport);
        assert(!pane.following && pane.unseen == oldUnseen + int(withIncoming) &&
               model.actions.empty());
    }

    // Repeated syncs before layout must neither mark unseen arrivals read nor count them twice.
    tail();
    model.now = 10;
    view.setReadEligible(true);
    queueReveal();
    auto arrive = [&](int index) {
        blast::Message message;
        message.id = "incoming-" + std::to_string(index);
        message.text = "IncomingReveal-" + std::to_string(index);
        message.nick = "Peer";
        message.time = "2026-11-07T12:35:00Z";
        buffer.messages.push_back(std::move(message));
        ++buffer.unread;
    };
    arrive(1);
    view.sync();
    view.sync();
    arrive(2);
    view.sync();
    assert(model.actions.empty() && pane.unseen == 0);
    view.arrange(renderer, viewport);
    assert(!pane.following && pane.unseen == 2 && model.actions.empty());
    arrange();
    assert(!pane.following && pane.unseen == 2 && model.actions.empty());
    assert(view.handleKey({.sym = XKB_KEY_End, .pressed = true}));
    view.sync();
    assert(model.actions.empty());
    view.arrange(renderer, viewport);
    near(scroll->scrollOffset(), scroll->maxScrollOffset());
    assert(pane.following && pane.unseen == 0 && model.actions.size() == 1 &&
           model.actions.back().type == "read" && model.actions.back().buffer == buffer.id);

    // Cancellation while unfollowed keeps the already-computed body anchor, not an old offset.
    view.setReadEligible(false);
    model.actions.clear();
    tail();
    scroll->setScrollOffset(scroll->maxScrollOffset() * .5f);
    arrange();
    std::string marker;
    for (const auto &entry : buffer.messages)
        if (bodyText(view, entry.text, true)) {
            const auto position = bounds(body(view, entry.text, true));
            if (position.top >= viewport.y &&
                position.top + position.height <= viewport.y + viewport.height) {
                marker = entry.text;
                break;
            }
        }
    assert(!marker.empty());
    const float markerTop = bounds(body(view, marker, true)).top;
    const std::string removedAboveAnchor = queueReveal();
    const auto aboveAnchor =
        std::find_if(buffer.messages.begin(), buffer.messages.end(),
                     [&](const auto &entry) { return entry.text == removedAboveAnchor; });
    assert(aboveAnchor != buffer.messages.end());
    buffer.messages.erase(aboveAnchor);
    ++buffer.messagesRevision;
    blast::Message anchorArrival;
    anchorArrival.id = "arrival-after-anchor-removal";
    anchorArrival.text = "ArrivalAfterAnchorRemoval";
    anchorArrival.nick = "Peer";
    buffer.messages.push_back(std::move(anchorArrival));
    view.sync();
    view.sync();
    view.arrange(renderer, viewport);
    near(bounds(body(view, marker, true)).top, markerTop);
    assert(!pane.following && pane.unseen == 1 && model.actions.empty());

    // A retained focused action can leave the materialized tail during the bounded retry.
    blast::Message focusedMessage;
    focusedMessage.id = "retained-focus-link";
    focusedMessage.nick = "Peer";
    focusedMessage.text = "RetainedLink https://example.org/reveal/retained";
    buffer.messages.push_back(std::move(focusedMessage));
    const std::string retainedText = buffer.messages.back().text;
    tail();
    InputArea *retainedLink = nullptr;
    auto findLink = [&](auto &&self, Node &node) -> void {
        if (auto *area = dynamic_cast<InputArea *>(&node); area && area->focusable()) {
            const auto hint = area->tooltipContent();
            if (const auto *url = std::get_if<std::string>(&hint);
                url && url->starts_with("https://example.org/reveal/"))
                retainedLink = area;
        }
        for (const auto &child : node.children())
            self(self, *child);
    };
    findLink(findLink, body(view, retainedText).content);
    assert(retainedLink);
    pane.focusComposer = false;
    dispatcher.setFocus(retainedLink);
    scroll->requestScrollIntoView(retainedLink);
    view.noteFocusReveal();
    blast::Message retainedTail;
    retainedTail.id = "tall-after-retained-focus";
    retainedTail.nick = "Peer";
    retainedTail.text = "TallAfterRetainedFocus";
    for (int i = 0; i < 48; ++i)
        retainedTail.text += "\nA tall new tail leaves the old action outside materialization.";
    buffer.messages.push_back(std::move(retainedTail));
    model.settings.timestamps = true;
    arrange();
    near(scroll->scrollOffset(), scroll->maxScrollOffset());
    assert(pane.following && pane.unseen == 0 && dispatcher.focusedArea() == keyboard &&
           model.actions.empty());
    assert(std::any_of(buffer.messages.begin(), buffer.messages.end(),
                       [&](const auto &entry) { return entry.text == retainedText; }));
    assert(!bodyText(view, retainedText));
    buffer.messages.pop_back();
    ++buffer.messagesRevision;
    model.settings.timestamps = false;

    // Losing the focused row during binding restores the normal *measured* new tail.
    view.setReadEligible(false);
    model.actions.clear();
    tail();
    const std::string removedText = queueReveal();
    const auto removed = std::find_if(buffer.messages.begin(), buffer.messages.end(),
                                      [&](const auto &entry) { return entry.text == removedText; });
    assert(removed != buffer.messages.end());
    buffer.messages.erase(removed);
    ++buffer.messagesRevision;
    blast::Message tall;
    tall.id = "tall-after-focus-removal";
    tall.nick = "Peer";
    tall.text = "TallAfterFocusRemoval";
    for (int i = 0; i < 48; ++i)
        tall.text += "\nA genuinely tall arrival must be measured before following its new tail.";
    buffer.messages.push_back(std::move(tall));
    view.sync();
    view.sync();
    view.arrange(renderer, viewport);
    near(scroll->scrollOffset(), scroll->maxScrollOffset());
    assert(pane.following && pane.unseen == 0 && model.actions.empty());
    assert(!bodyText(view, removedText));
    assert(bounds(body(view, "TallAfterFocusRemoval")).height > viewport.height);
    // An explicit native-action reveal must bind the corrected viewport in this pass,
    // even when reply occlusion moves it farther than the normal row overscan.
    dispatcher.setFocus(keyboard);
    buffer.messages.clear();
    for (int i = 0; i < 80; ++i) {
        blast::Message message;
        message.id = "overlay-reveal-" + std::to_string(i);
        message.nick = "Peer";
        message.text = "OverlayReveal-" + std::to_string(i) +
                       " https://example.org/reveal/overlay/" + std::to_string(i);
        buffer.messages.push_back(std::move(message));
    }
    ++buffer.messagesRevision;
    const LayoutRect tallViewport{17, 23, 700, 700};
    view.setContextInset(0, 0, 500);
    view.sync();
    view.arrange(renderer, tallViewport);
    scroll->setScrollOffset(scroll->maxScrollOffset() * .25f);
    view.arrange(renderer, tallViewport);
    std::vector<bool> beforeReveal;
    std::string overlayTarget;
    for (const auto &entry : buffer.messages) {
        beforeReveal.push_back(bodyText(view, entry.text, true) != nullptr);
        if (!beforeReveal.back())
            continue;
        const auto rect = bounds(body(view, entry.text, true));
        if (rect.top >= tallViewport.y + tallViewport.height - 120 &&
            rect.top + rect.height <= tallViewport.y + tallViewport.height)
            overlayTarget = entry.text;
    }
    assert(!overlayTarget.empty());
    retainedLink = nullptr;
    findLink(findLink, body(view, overlayTarget, true).content);
    assert(retainedLink);
    dispatcher.setFocus(retainedLink);
    pane.focusComposer = false;
    scroll->requestScrollIntoView(retainedLink);
    view.noteFocusReveal();
    const float beforeOverlayReveal = scroll->scrollOffset();
    view.arrange(renderer, tallViewport);
    assert(scroll->scrollOffset() > beforeOverlayReveal + 150 &&
           dispatcher.focusedArea() == retainedLink);
    const auto revealedInk = nativeTextInk(renderer, body(view, overlayTarget, true).text);
    containedBounds(revealedInk, {tallViewport.x, tallViewport.y, tallViewport.width,
                                  tallViewport.height - 500});
    std::vector<bool> materialized;
    bool entered = false;
    for (std::size_t i = 0; i < buffer.messages.size(); ++i) {
        materialized.push_back(bodyText(view, buffer.messages[i].text, true) != nullptr);
        entered |= materialized.back() && !beforeReveal[i];
    }
    assert(entered);
    const float settledOffset = scroll->scrollOffset();
    view.arrange(renderer, tallViewport);
    near(scroll->scrollOffset(), settledOffset);
    for (std::size_t i = 0; i < buffer.messages.size(); ++i)
        if (bodyText(view, buffer.messages[i].text, true))
            assert(materialized[i]);
    view.setContextInset(0, 0, 0);

    // Neither an empty transcript nor one fitting on screen needs a history fetch
    // or a focus handoff to implement either boundary.
    dispatcher.setFocus(keyboard);
    buffer.messages.clear();
    ++buffer.messagesRevision;
    for (const bool populated : {false, true}) {
        if (populated) {
            blast::Message shortRow;
            shortRow.id = "boundary-short";
            shortRow.nick = "Peer";
            shortRow.text = "BoundaryShort";
            buffer.messages.push_back(std::move(shortRow));
        }
        arrange();
        near(scroll->maxScrollOffset(), 0);
        pane.focusComposer = false;
        for (const bool newest : {false, true}) {
            assert(view.scrollToBoundary(newest));
            assert(dispatcher.focusedArea() == keyboard && !pane.focusComposer);
            arrange();
            near(scroll->scrollOffset(), 0);
            near(scroll->maxScrollOffset(), 0);
            assert(pane.following == newest && pane.unseen == 0 && model.actions.empty());
            assert(dispatcher.focusedArea() == keyboard && !pane.focusComposer);
            if (populated)
                containedBounds(nativeTextInk(renderer, body(view, "BoundaryShort").text),
                                nativeHistoryClip(view));
            else
                assert(plainText(view, "No messages yet"));
        }
    }
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void mergeArrivalBaseline(TranscriptTestRenderer &renderer) {
    for (const auto &[sourceActive, arrivalBeforeMerge, pendingReveal] :
         {std::tuple{false, false, false}, std::tuple{true, false, false},
          std::tuple{false, true, false}, std::tuple{true, true, false},
          std::tuple{false, true, true}, std::tuple{true, true, true}}) {
        blast::Model model;
        model.settings.timestamps = false;
        model.settings.avatars = false;
        model.settings.reducedMotion = true;
        model.settings.themeSource = "builtin";
        blast::Theme theme;
        theme.update(model);
        for (const std::string id : {"merge-source", "merge-destination"}) {
            auto &buffer = model.buffers[id];
            buffer.id = buffer.target = id;
            buffer.kind = "query";
            buffer.messagesRevision = 1;
            for (int i = 0; i < 20; ++i) {
                blast::Message message;
                message.id = id + "-" + std::to_string(i);
                message.text = "Body-" + message.id;
                message.nick = "Peer";
                buffer.messages.push_back(std::move(message));
            }
        }
        blast::PaneState pane{.id = 1, .buffer = "merge-source", .reply = {}};
        Clipboard clipboard;
        MenuHost host;
        blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
        view.setPopupContext(&host);
        view.setReadEligible(false);
        InputDispatcher dispatcher;
        dispatcher.setSceneRoot(&view);
        view.setInputDispatcher(dispatcher);
        const LayoutRect viewport{17, 23, 700, 180};
        auto arrange = [&] {
            view.sync();
            view.arrange(renderer, viewport);
        };
        arrange();
        pane.buffer = "merge-destination";
        arrange();
        if (sourceActive) {
            pane.buffer = "merge-source";
            arrange();
        }
        auto *scroll = find<ScrollView>(view);
        assert(scroll && scroll->maxScrollOffset() > 0);
        scroll->setScrollOffset(scroll->maxScrollOffset() * .5f);
        arrange();
        std::string marker;
        for (const auto &entry : model.buffers.at(pane.buffer).messages)
            if (bodyText(view, entry.text, true)) {
                const auto position = bounds(body(view, entry.text, true));
                if (position.top >= viewport.y &&
                    position.top + position.height <= viewport.y + viewport.height) {
                    marker = entry.text;
                    break;
                }
            }
        assert(!marker.empty());
        const float markerTop = bounds(body(view, marker, true)).top;
        pane.focusComposer = false;
        dispatcher.setFocus(&body(view, marker, true).row);
        if (pendingReveal) {
            scroll->requestScrollIntoView(&body(view, marker, true).row);
            view.noteFocusReveal();
        }
        auto arrive = [&] {
            blast::Message incoming;
            incoming.id = "live-at-merge-boundary";
            incoming.text = "OnlyGenuineNewArrival";
            incoming.nick = "Peer";
            model.buffers.at(pane.buffer).messages.push_back(std::move(incoming));
        };
        if (arrivalBeforeMerge)
            arrive();
        view.beforeBufferRename("merge-source", "merge-destination");
        auto &source = model.buffers.at("merge-source");
        auto &destination = model.buffers.at("merge-destination");
        destination.messages.insert(destination.messages.begin(),
                                    std::make_move_iterator(source.messages.begin()),
                                    std::make_move_iterator(source.messages.end()));
        ++destination.messagesRevision;
        pane.buffer = destination.id;
        model.buffers.erase("merge-source");
        view.remapBuffer("merge-source", destination.id);
        if (!arrivalBeforeMerge)
            arrive();
        if (pendingReveal)
            assert(pane.unseen == 0);
        view.sync();
        view.sync();
        view.arrange(renderer, viewport);
        if (!pendingReveal)
            near(bounds(body(view, marker, true)).top, markerTop);
        assert(!pane.following && pane.unseen == 1 && model.actions.empty());
        assert(view.handleKey({.sym = XKB_KEY_Menu, .pressed = true}));
        assert(host.open && host.contains("Copy text"));
        host.select("Copy text");
        assert(clipboard.clipboardText() == marker);
        dispatcher.setFocus(nullptr);
        dispatcher.setSceneRoot(nullptr);
    }
}

void renameArrivalBaseline(TranscriptTestRenderer &renderer) {
    for (const bool sameId : {false, true}) {
        blast::Model model;
        model.settings.timestamps = false;
        model.settings.avatars = false;
        model.settings.reducedMotion = true;
        model.settings.themeSource = "builtin";
        blast::Theme theme;
        theme.update(model);
        const std::string oldId = "rename-source";
        const std::string newId = sameId ? oldId : "rename-unused";
        auto &buffer = model.buffers[oldId];
        buffer.id = oldId;
        buffer.target = "MixedNick";
        buffer.kind = "query";
        buffer.messagesRevision = 1;
        for (int i = 0; i < 30; ++i) {
            blast::Message message;
            message.id = "rename-" + std::to_string(i);
            message.text = "RenameBody-" + std::to_string(i);
            message.nick = "Peer";
            buffer.messages.push_back(std::move(message));
        }
        blast::PaneState pane{.id = 1, .buffer = oldId, .reply = {}};
        Clipboard clipboard;
        MenuHost host;
        blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
        view.setPopupContext(&host);
        view.setReadEligible(false);
        InputDispatcher dispatcher;
        dispatcher.setSceneRoot(&view);
        view.setInputDispatcher(dispatcher);
        const LayoutRect viewport{17, 23, 700, 180};
        auto arrange = [&] {
            view.sync();
            view.arrange(renderer, viewport);
        };
        arrange();
        auto *scroll = find<ScrollView>(view);
        assert(scroll && scroll->maxScrollOffset() > 0);
        scroll->setScrollOffset(scroll->maxScrollOffset() * .5f);
        arrange();
        std::string marker;
        for (const auto &entry : buffer.messages)
            if (bodyText(view, entry.text, true)) {
                const auto position = bounds(body(view, entry.text, true));
                if (position.top >= viewport.y &&
                    position.top + position.height <= viewport.y + viewport.height) {
                    marker = entry.text;
                    break;
                }
            }
        assert(!marker.empty());
        const float markerTop = bounds(body(view, marker, true)).top;
        pane.focusComposer = false;
        dispatcher.setFocus(&body(view, marker, true).row);
        blast::Message incoming;
        incoming.id = "live-before-rename";
        incoming.text = "GenuinePreRenameArrival";
        incoming.nick = "Peer";
        buffer.messages.push_back(std::move(incoming));
        view.beforeBufferRename(oldId, newId);
        assert(pane.unseen == 1 && model.actions.empty());
        if (sameId)
            buffer.target = "mIXEDnICK";
        else {
            auto renamed = model.buffers.extract(oldId);
            renamed.key() = newId;
            renamed.mapped().id = newId;
            renamed.mapped().target = "OtherNick";
            ++renamed.mapped().messagesRevision;
            model.buffers.insert(std::move(renamed));
        }
        pane.buffer = newId;
        view.remapBuffer(oldId, newId);
        view.sync();
        view.sync();
        view.arrange(renderer, viewport);
        near(bounds(body(view, marker, true)).top, markerTop);
        assert(!pane.following && pane.unseen == 1 && model.actions.empty());
        assert(view.handleKey({.sym = XKB_KEY_Menu, .pressed = true}));
        assert(host.open && host.contains("Copy text"));
        host.select("Copy text");
        assert(clipboard.clipboardText() == marker);
        dispatcher.setFocus(nullptr);
        dispatcher.setSceneRoot(nullptr);
    }
}

void messageIdentityRevision(TranscriptTestRenderer &renderer) {
    blast::Model model;
    model.settings.timestamps = false;
    model.settings.avatars = false;
    model.settings.reducedMotion = true;
    model.settings.themeSource = "builtin";
    blast::Theme theme;
    theme.update(model);
    auto &buffer = model.buffers["identity"];
    buffer.id = buffer.target = "identity";
    buffer.kind = "query";
    buffer.server = "network";
    auto &network = model.networks[buffer.server];
    network.state = "Ready";
    network.nick = "Me";
    network.caps = {"message-tags", "draft/message-redaction"};
    buffer.messagesRevision = 1;
    for (const auto *id : {"A", "C", "B", "D"}) {
        blast::Message message;
        message.id = id;
        message.text = "Shown-" + message.id;
        message.nick = network.nick;
        message.own = true;
        buffer.messages.push_back(std::move(message));
    }
    blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
    Clipboard clipboard;
    MenuHost host;
    Node paneHost;
    auto transcript =
        std::make_unique<blast::TranscriptView>(model, pane, clipboard, [](auto) {}, [](auto) {});
    auto &view = *transcript;
    paneHost.addChild(std::move(transcript));
    view.setPopupContext(&host);
    view.setReadEligible(false);
    const LayoutRect viewport{17, 23, 700, 800};
    paneHost.setSize(viewport.x + viewport.width, viewport.y + viewport.height);
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&paneHost);
    view.setInputDispatcher(dispatcher);
    auto arrange = [&] {
        view.sync();
        view.arrange(renderer, viewport);
    };
    auto open = [&](const blast::Message &entry) {
        dispatcher.setFocus(&body(view, entry.text).row);
        dispatcher.keyEvent(XKB_KEY_Menu, 0, 0, true);
        dispatcher.keyEvent(XKB_KEY_Menu, 0, 0, false);
        assert(host.open && host.contains("Copy text"));
    };
    auto rowsMatchActions = [&] {
        float previous = -std::numeric_limits<float>::infinity();
        for (const auto &entry : buffer.messages) {
            const auto displayed = body(view, entry.text);
            assert(displayed.row.y() > previous);
            previous = displayed.row.y();
            open(entry);
            host.select("Copy text");
            assert(clipboard.clipboardText() == entry.text);
            open(entry);
            assert(host.contains("Delete message"));
            host.select("Delete message");
            assert(model.actions.size() == 1 && model.actions.back().type == "redact" &&
                   model.actions.back().buffer == buffer.id &&
                   model.actions.back().extra == entry.id);
            model.actions.clear();
        }
        dispatcher.setFocus(nullptr);
    };
    arrange();
    rowsMatchActions();
    std::swap(buffer.messages[1], buffer.messages[2]);
    ++buffer.messagesRevision;
    arrange();
    rowsMatchActions();
    buffer.messages[1].id = "E";
    buffer.messages[1].text = "Shown-E";
    ++buffer.messagesRevision;
    arrange();
    rowsMatchActions();

    auto &echo = buffer.messages[1];
    echo.id = "local:before";
    echo.label = "send-identity";
    echo.pending = true;
    echo.text = "EchoBefore";
    ++buffer.messagesRevision;
    arrange();
    open(echo);
    assert(host.contains("Reply") && !host.contains("React/👍") &&
           !host.contains("Delete message"));
    host.select("Copy text");
    dispatcher.setFocus(nullptr);
    echo.id = "server:confirmed";
    echo.pending = false;
    echo.text = "EchoAfter";
    ++buffer.messagesRevision;
    arrange();
    rowsMatchActions();
    open(echo);
    assert(host.contains("Reply"));
    host.select("Reply");
    assert(pane.reply == "echo:" + echo.label && pane.focusComposer);
    open(echo);
    assert(host.contains("React/👍"));
    host.select("React/👍");
    assert(model.actions.size() == 1 && model.actions.back().type == "react" &&
           model.actions.back().extra == echo.id && model.actions.back().buffer == buffer.id);
    model.actions.clear();
    dispatcher.setFocus(nullptr);
    echo.reactions["👍"] = {"Peer"};
    arrange();
    auto *reaction = find<Button>(body(view, echo.text).content);
    assert(reaction && reaction->enabled());
    float reactionX, reactionY;
    Node::mapToScene(reaction->inputArea(), reaction->inputArea()->width() * .5f,
                     reaction->inputArea()->height() * .5f, reactionX, reactionY);
    auto composer = std::make_unique<Input>();
    auto *input = composer.get();
    input->setValue("draft");
    paneHost.addChild(std::move(composer));
    paneHost.setSize(viewport.x + viewport.width, viewport.y + viewport.height + 48);
    input->arrange(renderer, {12, viewport.y + viewport.height + 8, 240, 32});
    dispatcher.setFocus(input->inputArea());
    TextInputEdit preedit;
    preedit.preeditText = "compose";
    preedit.preeditCursorBegin = preedit.preeditCursorEnd = 7;
    preedit.hasPreedit = true;
    input->textInputApplyEdit(preedit);
    assert(input->composing());
    assert(dispatcher.pointerButton(reactionX, reactionY, BTN_LEFT, true, 61, 610, true));
    assert(reaction->inputArea()->pressed() && model.actions.empty());
    assert(dispatcher.focusedArea() == input->inputArea() && input->composing());
    dispatcher.pointerButton(reactionX, reactionY, BTN_LEFT, false, 62, 620, true);
    assert(model.actions.size() == 1 && model.actions.back().type == "react" &&
           model.actions.back().extra == echo.id);
    model.actions.clear();
    for (const char *state : {"Disconnected", "Failed"}) {
        open(echo);
        assert(host.contains("React/👍"));
        network.state = state;
        arrange();
        assert(!host.open);
        reaction = find<Button>(body(view, echo.text).content);
        assert(reaction && !reaction->enabled());
        open(echo);
        assert(host.contains("Reply") && !host.contains("React"));
        host.select("Copy text");
        assert(clipboard.clipboardText() == echo.text && model.actions.empty());
        network.state = "Ready";
        arrange();
        reaction = find<Button>(body(view, echo.text).content);
        assert(reaction && reaction->enabled());
    }
    pane.focusComposer = false;
    dispatcher.setFocus(reaction->inputArea());
    echo.reactions["👍"].push_back("Other");
    arrange();
    reaction = find<Button>(body(view, echo.text).content);
    assert(reaction);
    assert(dispatcher.focusedArea() == reaction->inputArea());
    dispatcher.keyEvent(XKB_KEY_space, 0, 0, true);
    dispatcher.keyEvent(XKB_KEY_space, 0, 0, false);
    assert(model.actions.size() == 1 && model.actions.back().type == "react" &&
           model.actions.back().text == "👍" && model.actions.back().extra == echo.id &&
           dispatcher.focusedArea() == reaction->inputArea());
    model.actions.clear();
    for (const bool relabel : {true, false}) {
        const std::string oldID = echo.id;
        assert(view.handleKey({.sym = XKB_KEY_Home, .pressed = true}));
        auto *scroll = find<ScrollView>(view);
        assert(scroll);
        scroll->requestScrollIntoView(reaction->inputArea());
        view.noteFocusReveal();
        if (relabel)
            echo.label += ":next";
        else
            echo.id += ":next";
        ++buffer.messagesRevision;
        ++model.now;
        blast::Event echoed{};
        echoed.type = "echo";
        echoed.server = buffer.server;
        echoed.target = buffer.target;
        echoed.id = oldID;
        model.effects.clear();
        model.effects.push_back(std::move(echoed));
        arrange();
        reaction = find<Button>(body(view, echo.text).content);
        assert(reaction && dispatcher.focusedArea() == reaction->inputArea());
        assert(pane.unseen == 0);
        dispatcher.keyEvent(XKB_KEY_space, 0, 0, true);
        dispatcher.keyEvent(XKB_KEY_space, 0, 0, false);
        assert(model.actions.size() == 1 && model.actions.back().type == "react" &&
               model.actions.back().text == "👍" && model.actions.back().extra == echo.id);
        model.actions.clear();
    }
    model.effects.clear();
    dispatcher.setFocus(input->inputArea());
    echo.reactions["👍"].push_back("NewPeer");
    arrange();
    reaction = find<Button>(body(view, echo.text).content);
    assert(reaction && dispatcher.focusedArea() == input->inputArea());
    Node::mapToScene(reaction->inputArea(), reaction->inputArea()->width() * .5f,
                     reaction->inputArea()->height() * .5f, reactionX, reactionY);
    assert(dispatcher.pointerButton(reactionX, reactionY, BTN_LEFT, true, 63, 630, true));
    paneHost.setVisible(false);
    view.setReadEligible(false);
    assert(!reaction->inputArea()->pressed());
    paneHost.setVisible(true);
    arrange();
    dispatcher.pointerButton(reactionX, reactionY, BTN_LEFT, false, 64, 640, true);
    assert(!host.open && model.actions.empty());

    const auto retained = body(view, echo.text);
    const auto before = bounds(retained);
    float x, y;
    auto *surface = bubbleSurface(retained);
    assert(surface && retained.x() > surface->x());
    Node::mapToScene(surface, (retained.x() - surface->x()) * .5f,
                     retained.y() - surface->y() + retained.fragment.height() * .5f, x, y);
    assert(dispatcher.pointerButton(x, y, BTN_LEFT, true, 33, 330, true));
    paneHost.setVisible(false);
    view.setReadEligible(false);
    paneHost.setVisible(true);
    arrange();
    dispatcher.pointerButton(x, y, BTN_LEFT, false, 34, 340, true);
    assert(!host.open && model.actions.empty());
    assert(dispatcher.pointerButton(x, y, BTN_LEFT, true, 31, 310, true));
    // No source cache exists; the destination still needs a keyed rebuild.
    view.remapBuffer("never-visited", buffer.id);
    arrange();
    dispatcher.pointerButton(x, y, BTN_LEFT, false, 32, 320, true);
    assert(!host.open && model.actions.empty());
    const auto after = bounds(body(view, echo.text));
    near(after.top, before.top);
    near(after.height, before.height);
    open(echo);
    host.select("Reply");
    assert(pane.reply == "echo:" + echo.label);
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void channelActivity(TranscriptTestRenderer &renderer, float zoom) {
    blast::Model model;
    model.settings.scale = zoom;
    model.settings.messageScale = zoom;
    model.settings.timestamps = true;
    model.settings.presence = blast::PresenceMode::Smart;
    model.settings.reducedMotion = true;
    model.settings.foolDisplay = blast::FoolDisplay::Blurred;
    model.settings.themeSource = "builtin";
    blast::Theme theme;
    theme.update(model);
    auto &buffer = model.buffers["activity"];
    buffer.id = "activity";
    buffer.target = "#activity";
    buffer.server = "network";
    auto &network = model.networks[buffer.server];
    network.state = "Ready";
    network.caps = {"message-tags"};
    // Summary rendering is exercised with established participants, not idle join spam.
    for (int i = 0; i < 5; ++i) {
        blast::Message prior;
        prior.id = "participant-" + std::to_string(i);
        prior.nick = i == 4 ? "OtherPeer" : "Peer";
        prior.text = "Earlier meaningful conversation " + std::to_string(i);
        buffer.messages.push_back(std::move(prior));
    }
    const std::string original[] = {"Ada joined the channel.",
                                    "Bo left the channel.",
                                    "Cy is now Cee.",
                                    "Utility error remains readable.",
                                    "Normal conversation remains.",
                                    "Legacy activity summary remains.",
                                    "Dee quit the network.",
                                    "Eli joined the channel."};
    const blast::ActivityKind kinds[] = {blast::ActivityKind::Join, blast::ActivityKind::Part,
                                         blast::ActivityKind::Nick, blast::ActivityKind::None,
                                         blast::ActivityKind::None, blast::ActivityKind::None,
                                         blast::ActivityKind::Quit, blast::ActivityKind::Join};
    for (std::size_t i = 0; i < std::size(original); ++i) {
        blast::Message message;
        message.id = "activity-" + std::to_string(i);
        message.nick = i == 1 ? "OtherPeer" : "Peer";
        message.text = original[i];
        message.time = "2026-11-07T12:34:56Z";
        message.activity = kinds[i];
        message.presence = i != 4;
        buffer.messages.push_back(std::move(message));
    }
    const auto before = buffer;
    blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
    Clipboard clipboard;
    MenuHost host;
    blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
    view.setPopupContext(&host);
    view.setReadEligible(false);
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&view);
    view.setInputDispatcher(dispatcher);
    const LayoutRect viewport{17, 23, 700 * zoom, 2000 * zoom};
    const auto arrange = [&] {
        view.sync();
        view.arrange(renderer, viewport);
    };
    const auto controls = [&](std::string_view tooltip) {
        std::vector<InputArea *> found;
        const auto visit = [&](auto &&self, Node &node) -> void {
            if (!node.visible())
                return;
            if (auto *area = dynamic_cast<InputArea *>(&node)) {
                const auto content = area->tooltipContent();
                if (const auto *text = std::get_if<std::string>(&content); text && *text == tooltip)
                    found.push_back(area);
            }
            for (const auto &child : node.children())
                self(self, *child);
        };
        visit(visit, view);
        std::sort(found.begin(), found.end(), [](Node *a, Node *b) {
            float ax, ay, bx, by;
            Node::absolutePosition(a, ax, ay);
            Node::absolutePosition(b, bx, by);
            return ay < by;
        });
        return found;
    };
    const auto actual = [&](std::size_t index) {
        const auto rendered = body(view, original[index]);
        near(rendered.body.foregroundBlurRadius(), 0);
        near(rendered.text.fontSize(),
             kinds[index] == blast::ActivityKind::None ? 14 * zoom : 12 * zoom);
        if (kinds[index] != blast::ActivityKind::None) {
            assert(rendered.text.font().family == "monospace");
            assert(plainText(rendered.content, "12:34"));
        }
    };
    const auto toggle = [&](InputArea &control, std::uint32_t key) {
        dispatcher.setFocus(&control);
        dispatcher.keyEvent(key, 0, 0, true);
        dispatcher.keyEvent(key, 0, 0, false);
        arrange();
    };
    arrange();
    auto folded = controls("Expand channel activity");
    assert(folded.size() == 2);
    assert(bodyText(*folded[0], "3") && bodyText(*folded[1], "2"));
    for (auto *control : folded) {
        assert(!plainText(*control, "12:34"));
        auto *chevron = find<GlyphNode>(*control);
        assert(chevron && chevron->codepoint() == GlyphRegistry::lookup("chevron-right"));
        near(body(*control, "channel events").body.foregroundBlurRadius(), 0);
    }
    assert(!view.messageReadable("message:activity-0") &&
           !view.messageReadable("message:activity-6"));
    for (const std::size_t i : {0u, 1u, 2u, 6u, 7u})
        assert(!bodyText(view, original[i]));
    for (const std::size_t i : {3u, 4u, 5u})
        actual(i);
    const float withSummary = body(view, original[4]).row.y();

    buffer.readMarker = buffer.messages[5].id;
    arrange();
    // A durable marker without unread chat must not split a presence-only summary.
    for (const std::size_t i : {0u, 1u, 2u})
        assert(!bodyText(view, original[i]));
    assert(controls("Expand channel activity").size() == 2);
    buffer.readMarker.clear();
    buffer.messages[12].timestamp = 86400; // A different local day cannot join the preceding run.
    arrange();
    assert(controls("Expand channel activity").size() == 1);
    actual(6);
    near(body(view, original[7]).text.fontSize(), 12 * zoom);
    buffer.messages[12].timestamp = 0;
    model.settings.presence = blast::PresenceMode::Hidden;
    arrange();
    assert(controls("Expand channel activity").empty());
    assert(body(view, original[4]).row.y() < withSummary);
    for (const std::size_t i : {0u, 1u, 2u, 6u, 7u})
        assert(!bodyText(view, original[i]));
    for (const std::size_t i : {3u, 4u, 5u})
        actual(i);
    model.settings.channelPresence[buffer.target] = blast::PresenceMode::All;
    arrange();
    for (std::size_t i = 0; i < std::size(original); ++i)
        actual(i);
    model.settings.channelPresence[buffer.id] = blast::PresenceMode::Global;
    assert(blast::effectivePresenceMode(model.settings, buffer.id, buffer.target) ==
           blast::PresenceMode::Hidden);
    arrange();
    assert(!bodyText(view, original[0]));
    model.settings.presence = blast::PresenceMode::Smart;
    assert(blast::effectivePresenceMode(model.settings, buffer.id, buffer.target) ==
           blast::PresenceMode::Smart);
    arrange();
    assert(controls("Expand channel activity").size() == 2);
    assert(!bodyText(view, original[0]));
    model.settings.channelPresence[buffer.id] = blast::PresenceMode::Hidden;
    arrange();
    assert(!bodyText(view, original[0]));
    model.settings.channelPresence.clear();
    model.settings.presence = blast::PresenceMode::Hidden;
    buffer.kind = "query";
    arrange();
    assert(controls("Expand channel activity").empty());
    for (const std::size_t i : {0u, 1u, 2u, 6u, 7u})
        assert(!bodyText(view, original[i]));
    for (const std::size_t i : {3u, 4u, 5u})
        actual(i);
    model.settings.presence = blast::PresenceMode::All;
    arrange();
    assert(controls("Expand channel activity").empty());
    for (std::size_t i = 0; i < std::size(original); ++i)
        actual(i);
    buffer.kind = "channel";
    model.settings.presence = blast::PresenceMode::Smart;
    arrange();
    folded = controls("Expand channel activity");
    assert(folded.size() == 2);
    float x, y;
    auto *countText = bodyText(*folded[0], "3");
    assert(countText && countText->parent());
    auto *countArea = countText->parent();
    Node::mapToScene(countArea, countArea->width() * .5f, countArea->height() * .5f, x, y);
    assert(dispatcher.pointerButton(x, y, BTN_LEFT, true, 101, 1010, false));
    dispatcher.pointerButton(x, y, BTN_LEFT, false, 102, 1020, false);
    arrange();
    for (const std::size_t i : {0u, 1u, 2u})
        actual(i);
    auto expanded = controls("Collapse channel activity");
    assert(expanded.size() == 1);
    auto *down = find<GlyphNode>(*expanded.front());
    assert(down && down->codepoint() == GlyphRegistry::lookup("chevron-down"));
    toggle(*expanded.front(), XKB_KEY_space);
    assert(!bodyText(view, original[0]));
    folded = controls("Expand channel activity");
    toggle(*folded.front(), XKB_KEY_Return);
    folded = controls("Expand channel activity");
    assert(folded.size() == 1);
    toggle(*folded.front(), XKB_KEY_Return);
    for (std::size_t i = 0; i < std::size(original); ++i)
        actual(i);
    const auto leader = body(view, original[0]);
    dispatcher.setFocus(&leader.row);
    dispatcher.keyEvent(XKB_KEY_Menu, 0, 0, true);
    dispatcher.keyEvent(XKB_KEY_Menu, 0, 0, false);
    assert(host.open && !host.contains("Reply") && !host.contains("React"));
    host.select("Copy text");
    assert(clipboard.clipboardText() == original[0]);
    unchanged(buffer, before);

    auto appended = buffer.messages.back();
    appended.id = "activity-appended";
    appended.text = "Fay joined the channel.";
    buffer.messages.push_back(appended); // Real append: do not forge an identity-revision event.
    arrange();
    assert(bodyText(view, appended.text) && controls("Collapse channel activity").size() == 2);
    auto prepended = buffer.messages[5];
    prepended.id = "activity-prepended";
    prepended.text = "Ari joined the channel.";
    buffer.messages.insert(buffer.messages.begin() + 5, prepended);
    ++buffer.messagesRevision;
    arrange();
    assert(bodyText(view, prepended.text) && bodyText(view, appended.text));
    assert(controls("Collapse channel activity").size() == 2);
    const auto beforeRename = buffer;
    const std::string oldId = buffer.id, renamed = "renamed-activity";
    view.beforeBufferRename(oldId, renamed);
    auto node = model.buffers.extract(oldId);
    node.key() = renamed;
    node.mapped().id = node.mapped().target = renamed;
    model.buffers.insert(std::move(node));
    pane.buffer = renamed;
    view.remapBuffer(oldId, renamed);
    arrange();
    assert(bodyText(view, prepended.text) && bodyText(view, appended.text));
    expanded = controls("Collapse channel activity");
    assert(expanded.size() == 2);
    toggle(*expanded.front(), XKB_KEY_space);
    assert(!bodyText(view, prepended.text) && bodyText(view, appended.text));
    assert(controls("Expand channel activity").size() == 1);
    unchanged(buffer, beforeRename);
    assert(model.actions.empty());
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void smartPresenceEligibility(TranscriptTestRenderer &renderer) {
    constexpr int64_t eventTime = 20 * 86400;
    struct Scenario {
        const char *name;
        std::vector<int64_t> prior;
        bool own = false, eligible = false;
    };
    const std::vector<Scenario> scenarios{
        {"live-at-300", {eventTime - 300}, false, true},
        {"idle-at-301", {eventTime - 301}},
        {"five-in-seven-days",
         {eventTime - 7 * 86400, eventTime - 600, eventTime - 500, eventTime - 400,
          eventTime - 301},
         false,
         true},
        {"only-four", {eventTime - 600, eventTime - 500, eventTime - 400, eventTime - 301}},
        {"expired-fifth",
         {eventTime - 7 * 86400 - 1, eventTime - 600, eventTime - 500, eventTime - 400,
          eventTime - 301}},
        {"future-chat",
         {eventTime + 1, eventTime + 2, eventTime + 3, eventTime + 4, eventTime + 5}},
        {"own-idle", {}, true, true},
    };
    for (const auto &scenario : scenarios) {
        blast::Model model;
        model.settings.timestamps = model.settings.avatars = false;
        model.settings.reducedMotion = true;
        model.settings.presence = blast::PresenceMode::Smart;
        blast::Theme theme;
        theme.update(model);
        auto &buffer = model.buffers["smart-boundary"];
        buffer.id = buffer.target = "smart-boundary";
        buffer.kind = "channel";
        buffer.server = "network";
        model.networks[buffer.server].caseMapping = "rfc1459";
        for (const auto timestamp : scenario.prior) {
            blast::Message chat;
            chat.id = "chat-" + std::to_string(buffer.messages.size());
            chat.nick = "[PEER]";
            chat.text = "Meaningful retained chat " + chat.id;
            chat.timestamp = timestamp;
            buffer.messages.push_back(std::move(chat));
        }
        const auto event = [&](std::string id, std::string nick, bool own) {
            blast::Message message;
            message.id = id;
            message.nick = std::move(nick);
            message.text = std::string(scenario.name) + "-" + id;
            message.timestamp = eventTime;
            message.presence = true;
            message.activity = own ? blast::ActivityKind::Part : blast::ActivityKind::Join;
            message.own = own;
            buffer.messages.push_back(std::move(message));
        };
        // RFC1459 brackets and ASCII case must match the participant's retained chat.
        event("eligible-first", "{peer}", scenario.own);
        event("eligible-second", "{peer}", scenario.own);
        event("idle-excluded", "UnrelatedIdleNick", false);
        blast::Message forced;
        forced.id = "forced-notice";
        forced.nick = "Utility";
        forced.text = "Someone joined the channel. Away: this is a forced notice, not an event.";
        forced.presence = true; // Deliberately untyped: no prose/regex activity inference.
        forced.timestamp = eventTime;
        buffer.messages.push_back(forced);
        const auto before = buffer;
        blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
        Clipboard clipboard;
        blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
        view.setReadEligible(false);
        InputDispatcher dispatcher;
        dispatcher.setSceneRoot(&view);
        view.setInputDispatcher(dispatcher);
        const auto arrange = [&] {
            view.sync();
            view.arrange(renderer, {17, 23, 1200, 2000});
        };
        const auto summary = [&]() -> InputArea * {
            InputArea *found = nullptr;
            const auto visit = [&](auto &&self, Node &node) -> void {
                if (!node.visible())
                    return;
                if (auto *area = dynamic_cast<InputArea *>(&node)) {
                    const auto tooltip = area->tooltipContent();
                    if (const auto *text = std::get_if<std::string>(&tooltip);
                        text && *text == "Expand channel activity") {
                        assert(!found);
                        found = area;
                    }
                }
                for (const auto &child : node.children())
                    self(self, *child);
            };
            visit(visit, view);
            return found;
        };
        arrange();
        auto *collapsed = summary();
        assert(bool(collapsed) == scenario.eligible);
        assert(!bodyText(view, std::string(scenario.name) + "-idle-excluded"));
        assert(bodyText(view, forced.text));
        if (collapsed) {
            assert(bodyText(*collapsed, "2") && !bodyText(*collapsed, "3"));
            dispatcher.setFocus(collapsed);
            dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
            dispatcher.keyEvent(XKB_KEY_Return, 0, 0, false);
            arrange();
        }
        assert(bool(bodyText(view, std::string(scenario.name) + "-eligible-first")) ==
               scenario.eligible);
        assert(bool(bodyText(view, std::string(scenario.name) + "-eligible-second")) ==
               scenario.eligible);
        assert(!bodyText(view, std::string(scenario.name) + "-idle-excluded"));
        assert(bodyText(view, forced.text));
        unchanged(buffer, before);
        assert(model.actions.empty());
        dispatcher.setFocus(nullptr);
        dispatcher.setSceneRoot(nullptr);
    }
}

void ownJoinProjection(TranscriptTestRenderer &renderer) {
    for (int layout = 0; layout < 3; ++layout)
        for (const bool history : {false, true}) {
            blast::Model model;
            model.settings.timestamps = model.settings.avatars = false;
            model.settings.reducedMotion = true;
            model.settings.presence = blast::PresenceMode::Smart;
            blast::Theme theme;
            theme.update(model);
            auto &buffer = model.buffers["own-join"];
            buffer.id = buffer.target = "own-join";
            buffer.kind = "channel";
            buffer.server = "network";
            model.networks[buffer.server].nick = "CurrentNick";
            if (layout)
                model.settings.messageLayouts[buffer.id] =
                    layout == 1 ? blast::MessageLayout::Irc : blast::MessageLayout::Stacked;
            const auto append = [&](const char *id, bool own, bool presence,
                                    blast::ActivityKind kind, bool action = false) {
                blast::Message message;
                message.id = message.text = id;
                // Persisted ownership must not depend on today's nickname or notice prose.
                message.nick = own ? "OldNick" : "Participant";
                message.timestamp = 1000;
                message.own = own;
                message.presence = presence;
                message.activity = kind;
                message.history = history;
                message.action = action;
                buffer.messages.push_back(std::move(message));
            };
            append("OwnJoinAlone", true, true, blast::ActivityKind::Join);
            blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
            Clipboard clipboard;
            blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
            view.setReadEligible(false);
            InputDispatcher dispatcher;
            dispatcher.setSceneRoot(&view);
            view.setInputDispatcher(dispatcher);
            const auto arrange = [&] {
                view.sync();
                view.arrange(renderer, {17, 23, 1200, 2000});
            };
            const auto summaries = [&](std::string_view tooltip) {
                std::vector<InputArea *> found;
                const auto visit = [&](auto &&self, Node &node) -> void {
                    if (!node.visible())
                        return;
                    if (auto *area = dynamic_cast<InputArea *>(&node)) {
                        const auto content = area->tooltipContent();
                        if (const auto *text = std::get_if<std::string>(&content);
                            text && *text == tooltip)
                            found.push_back(area);
                    }
                    for (const auto &child : node.children())
                        self(self, *child);
                };
                visit(visit, view);
                return found;
            };
            const auto noOwnJoins = [&] {
                for (const char *id : {"OwnJoinAlone", "OwnJoinBetween", "OwnJoinTail"}) {
                    assert(!bodyText(view, id));
                    assert(!view.messageReadable(std::string("message:") + id));
                }
            };
            for (const auto mode : {blast::PresenceMode::Smart, blast::PresenceMode::All,
                                    blast::PresenceMode::Hidden}) {
                model.settings.presence = mode;
                arrange();
                noOwnJoins();
                assert(summaries("Expand channel activity").empty());
                assert(summaries("Collapse channel activity").empty());
            }
            model.settings.presence = blast::PresenceMode::Smart;
            append("ParticipantChat", false, false, blast::ActivityKind::None);
            append("ParticipantJoinFirst", false, true, blast::ActivityKind::Join);
            arrange();
            assert(bodyText(view, "ParticipantJoinFirst"));
            append("OwnJoinBetween", true, true, blast::ActivityKind::Join);
            arrange();
            noOwnJoins();
            assert(bodyText(view, "ParticipantJoinFirst"));
            assert(summaries("Expand channel activity").empty());
            append("ParticipantJoinSecond", false, true, blast::ActivityKind::Join);
            arrange();
            auto folded = summaries("Expand channel activity");
            assert(folded.size() == 1 && bodyText(*folded.front(), "2"));
            append("OwnJoinTail", true, true, blast::ActivityKind::Join);
            arrange();
            noOwnJoins();
            folded = summaries("Expand channel activity");
            assert(folded.size() == 1 && bodyText(*folded.front(), "2") &&
                   !bodyText(*folded.front(), "3"));
            assert(!bodyText(view, "ParticipantJoinFirst") &&
                   !bodyText(view, "ParticipantJoinSecond"));
            dispatcher.setFocus(folded.front());
            dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
            dispatcher.keyEvent(XKB_KEY_Return, 0, 0, false);
            arrange();
            noOwnJoins();
            assert(bodyText(view, "ParticipantJoinFirst") &&
                   bodyText(view, "ParticipantJoinSecond"));
            assert(summaries("Collapse channel activity").size() == 1);
            append("OwnChat", true, false, blast::ActivityKind::None);
            append("OwnAction", true, false, blast::ActivityKind::None, true);
            append("OwnLegacy joined the channel.", true, true, blast::ActivityKind::None);
            append("OwnPart", true, true, blast::ActivityKind::Part);
            const auto before = buffer;
            for (const auto mode : {blast::PresenceMode::All, blast::PresenceMode::Hidden,
                                    blast::PresenceMode::Smart}) {
                model.settings.presence = mode;
                arrange();
                noOwnJoins();
                for (const char *id :
                     {"OwnChat", "OwnAction", "OwnLegacy joined the channel.", "ParticipantChat"})
                    assert(bodyText(view, id));
                for (const char *id : {"ParticipantJoinFirst", "ParticipantJoinSecond", "OwnPart"})
                    assert(bool(bodyText(view, id)) == (mode != blast::PresenceMode::Hidden));
                unchanged(buffer, before);
            }
            assert(model.actions.empty());
            dispatcher.setFocus(nullptr);
            dispatcher.setSceneRoot(nullptr);
        }
}

void messageTextSelection(TranscriptTestRenderer &renderer, float zoom) {
    blast::Model model;
    model.settings.scale = zoom;
    model.settings.messageScale = zoom;
    model.settings.timestamps = true;
    model.settings.reducedMotion = true;
    model.settings.themeSource = "builtin";
    blast::Theme theme;
    theme.update(model);
    auto &buffer = model.buffers["selection"];
    buffer.id = buffer.target = "selection";
    const std::string firstPlain = "Alpha e\xCC\x81 X tail";
    const std::string middlePlain =
        "Middle words preserve every native wrapping space across a narrow retained selection.";
    const std::string messages[] = {"Alpha \x02"
                                    "e"
                                    "\x02\x1d"
                                    "\xCC\x81"
                                    "\x1d"
                                    " X tail",
                                    "Presence is not copied", middlePlain, "Beta selected body"};
    for (int i = 0; i < 4; ++i) {
        blast::Message message;
        message.id = "selection-" + std::to_string(i);
        message.nick = "Peer";
        message.text = messages[i];
        message.presence = i == 1;
        buffer.messages.push_back(std::move(message));
    }
    blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
    Clipboard clipboard;
    MenuHost host;
    blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
    view.setPopupContext(&host);
    view.setReadEligible(false);
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&view);
    view.setInputDispatcher(dispatcher);
    LayoutRect viewport{17, 23, 1200 * zoom, 800 * zoom};
    const auto arrange = [&] {
        view.sync();
        view.arrange(renderer, viewport);
    };
    const auto decode = decodedMarkup;
    // Initial endpoint messages are unwrapped. Query the ACTUAL styled fragments,
    // then convert native layout-local caret coordinates through the drawn baseline.
    const auto point = [&](std::string_view token, std::size_t byte) {
        const auto rendered = body(view, token);
        std::size_t begin = 0;
        for (const auto &child : rendered.body.children())
            if (auto *area = dynamic_cast<InputArea *>(child.get()))
                for (const auto &node : area->children())
                    if (auto *text = dynamic_cast<TextNode *>(node.get());
                        text && text->useMarkup()) {
                        const auto plain = decode(text->text());
                        if (byte <= begin + plain.size()) {
                            std::vector<TextCursorStop> stops;
                            renderer.measureTextCaretStops(text->text(), text->fontSize(),
                                                           {byte - begin}, 0, stops, text->font(),
                                                           true);
                            assert(stops.size() == 1);
                            const auto metrics = renderer.measureText(
                                text->text(), text->fontSize(), text->font(), 0, 0,
                                TextAlign::Start, TextEllipsize::End, true);
                            float x, y;
                            Node::absolutePosition(text, x, y);
                            return std::pair{x - metrics.alignOffsetX + stops[0].x,
                                             y - std::floor(metrics.layoutBaseline) + stops[0].y +
                                                 stops[0].height * .5f};
                        }
                        begin += plain.size();
                    }
        assert(false && "selection endpoint must exist in native fragment");
        std::abort();
    };
    const auto drag = [&](const auto &from, const auto &to) {
        assert(dispatcher.pointerButton(from.first, from.second, BTN_LEFT, true, 91, 910, false));
        dispatcher.pointerMotion(to.first, to.second, 92);
        dispatcher.pointerButton(to.first, to.second, BTN_LEFT, false, 93, 930, false);
        arrange();
        assert(!host.open && model.actions.empty());
    };
    arrange();
    dispatcher.setFocus(&body(view, "Alpha").row);
    assert(view.copySelectedMessage() && clipboard.clipboardText() == firstPlain);
    assert(view.textSelectionCommand("set-mark"));
    clipboard.setClipboardText("sentinel");
    assert(!view.copySelection() && !view.copySelectedMessage() &&
           clipboard.clipboardText() == "sentinel");
    for (int i = 0; i < 6; ++i)
        assert(view.textSelectionCommand("forward-char"));
    assert(view.textSelectionCommand("set-mark"));
    assert(view.textSelectionCommand("forward-char"));
    assert(view.copySelection() && clipboard.clipboardText() == "e\xCC\x81");
    viewport.width = 240 * zoom;
    arrange();
    assert(view.copySelection() && clipboard.clipboardText() == "e\xCC\x81");
    assert(view.textSelectionCommand("end-of-line"));
    assert(view.copySelection() && clipboard.clipboardText() == "e\xCC\x81 X tail");
    view.clearTextSelection();
    buffer.messages.front().text = "\x02"
                                   "e\xCC\x81 first\nsecond";
    assert(view.textSelectionCommand("set-mark"));
    assert(view.textSelectionCommand("end-of-line"));
    assert(view.copySelection() && clipboard.clipboardText() == "e\xCC\x81 first");
    assert(view.textSelectionCommand("forward-char"));
    assert(view.textSelectionCommand("end-of-line"));
    assert(view.copySelection() && clipboard.clipboardText() == "e\xCC\x81 first\nsecond");
    view.clearTextSelection();
    buffer.messages.front().text = messages[0];
    buffer.messages.front().deleted = true;
    clipboard.setClipboardText("sentinel");
    assert(!view.copySelection() && !view.copySelectedMessage() &&
           !view.textSelectionCommand("set-mark") && clipboard.clipboardText() == "sentinel");
    buffer.messages.front().deleted = false;
    viewport.width = 1200 * zoom;
    arrange();
    const auto caret = point("Alpha", 6);
    assert(dispatcher.pointerButton(caret.first, caret.second, BTN_LEFT, true, 89, 890, false));
    dispatcher.pointerButton(caret.first, caret.second, BTN_LEFT, false, 90, 900, false);
    assert(!host.open && !view.copySelection());
    drag(point("Alpha", 6), point("Alpha", 9));
    assert(view.copySelection() && clipboard.clipboardText() == "e\xCC\x81");
    const auto selectedBody = body(view, "Alpha");
    std::vector<const RectNode *> highlights;
    for (const auto &child : selectedBody.body.children())
        if (auto *rect = dynamic_cast<RectNode *>(child.get());
            rect && rect->visible() &&
            rect->style().fill == withAlpha(blast::themeForegroundColor("primary"), .24f)) {
            assert(rect->width() > 0 && rect->height() > 0);
            highlights.push_back(rect);
        }
    assert(highlights.size() >= 2); // Both shaped style-cut halves of the selected grapheme.
    blast::MessageSelectionText selectedLogical;
    selectedLogical.update(firstPlain);
    std::size_t fragmentBegin = 0, coveredCells = 0;
    for (const auto &child : selectedBody.body.children())
        if (auto *area = dynamic_cast<InputArea *>(child.get()))
            for (const auto &node : area->children())
                if (auto *text = dynamic_cast<TextNode *>(node.get()); text && text->useMarkup()) {
                    const auto fragmentEnd = fragmentBegin + decode(text->text()).size();
                    blast::MessageSelectionFragment native;
                    native.rebuild(renderer, selectedLogical, fragmentBegin, fragmentEnd,
                                   text->text(), text->fontSize(), text->font(), 0, true);
                    std::vector<blast::MessageSelectionRect> cells;
                    native.rectangles(6, 9, cells);
                    const auto &metrics = native.metrics();
                    for (const auto &cell : cells) {
                        const float x = area->x() + text->x() - metrics.alignOffsetX + cell.x;
                        const float scale = renderer.renderScale();
                        const float y = area->y() + text->y() -
                                        std::floor(metrics.layoutBaseline * scale) / scale + cell.y;
                        assert(std::any_of(
                            highlights.begin(), highlights.end(), [&](const auto *rect) {
                                return rect->x() <= x + .02f && rect->y() <= y + .02f &&
                                       rect->x() + rect->width() >= x + cell.width - .02f &&
                                       rect->y() + rect->height() >= y + cell.height - .02f;
                            }));
                        ++coveredCells;
                    }
                    fragmentBegin = fragmentEnd;
                }
    assert(coveredCells >= 2);
    view.clearTextSelection();
    assert(!view.copySelection() && clipboard.clipboardText() == "e\xCC\x81");
    drag(point("Alpha", 6), point("Beta", 4));
    const std::string copied = "e\xCC\x81 X tail\n" + middlePlain + "\nBeta";
    assert(view.copySelection() && clipboard.clipboardText() == copied);
    buffer.messages[2].deleted = true;
    arrange();
    assert(view.copySelection() && clipboard.clipboardText() == "e\xCC\x81 X tail\nBeta");
    const auto deleted = body(view, "Message deleted");
    for (const auto &child : deleted.body.children())
        if (auto *rect = dynamic_cast<RectNode *>(child.get()); rect && rect->visible())
            assert(rect->style().fill != withAlpha(blast::themeForegroundColor("primary"), .24f));
    buffer.messages[2].deleted = false;
    arrange();
    assert(view.copySelection() && clipboard.clipboardText() == copied);
    viewport.width = 240 * zoom;
    arrange();
    assert(view.copySelection() && clipboard.clipboardText() == copied);
    buffer.messages.pop_back();
    ++buffer.messagesRevision;
    arrange();
    assert(!view.copySelection() && clipboard.clipboardText() == copied);

    view.clearTextSelection();
    blast::Message overscan;
    overscan.id = "selection-overscan-marker";
    overscan.nick = "Peer";
    overscan.text = "UnseenOverscanBoundaryMarker";
    buffer.messages.push_back(overscan);
    viewport.height = 180 * zoom;
    arrange();
    auto *scroll = find<ScrollView>(view);
    assert(scroll && scroll->maxScrollOffset() > 0);
    scroll->setScrollOffset(0);
    arrange();
    const auto hiddenMarker = bounds(body(view, overscan.text));
    assert(hiddenMarker.top >= viewport.y + viewport.height - .02f);
    const auto start = point("Alpha", 0);
    const auto visibleEnd = point("Alpha", firstPlain.size());
    const float beyond = viewport.y + viewport.height + 24 * zoom;
    const float pastOverscan = hiddenMarker.top + hiddenMarker.height + 24 * zoom;
    const float immediateOffset = scroll->scrollOffset();
    assert(dispatcher.pointerButton(start.first, start.second, BTN_LEFT, true, 97, 970, false));
    dispatcher.pointerMotion(visibleEnd.first, pastOverscan, 98);
    dispatcher.pointerButton(visibleEnd.first, pastOverscan, BTN_LEFT, false, 99, 990, false);
    assert(view.copySelection() && clipboard.clipboardText());
    const std::string beforeMarker = firstPlain + "\n" + middlePlain;
    assert(beforeMarker.starts_with(*clipboard.clipboardText()));
    assert(clipboard.clipboardText()->find(overscan.text) == std::string::npos);
    near(scroll->scrollOffset(), immediateOffset);
    assert(!host.open && model.actions.empty());
    view.clearTextSelection();
    assert(dispatcher.pointerButton(start.first, start.second, BTN_LEFT, true, 94, 940, false));
    dispatcher.pointerMotion(start.first, beyond, 95);
    const float beforeScroll = scroll->scrollOffset();
    const auto nextMessageExposed = [&] {
        const auto ink = nativeTextInk(renderer, body(view, "Middle").text);
        const auto visible = intersectBounds(ink, nativeHistoryClip(view));
        return visible.width > 0 && visible.height > 0;
    };
    // Advance native drag autoscroll until the next message is actually painted,
    // rather than assuming a fixed frame count crosses font- and layout-sized gaps.
    for (int frame = 0; frame < 240 && !nextMessageExposed(); ++frame) {
        model.now += .016;
        arrange();
    }
    assert(nextMessageExposed());
    assert(scroll->scrollOffset() > beforeScroll);
    dispatcher.pointerButton(start.first, beyond, BTN_LEFT, false, 96, 960, false);
    assert(view.copySelection());
    assert(clipboard.clipboardText() && clipboard.clipboardText()->starts_with(firstPlain));
    std::size_t markerOffset = 0, exposedMarkerBytes = 0;
    bool markerExposed = false;
    const auto exposedMarker = body(view, overscan.text);
    const auto historyClip = nativeHistoryClip(view);
    for (const auto &child : exposedMarker.body.children())
        if (auto *area = dynamic_cast<InputArea *>(child.get()))
            for (const auto &node : area->children())
                if (auto *text = dynamic_cast<TextNode *>(node.get()); text && text->useMarkup()) {
                    const auto plain = decode(text->text());
                    float x, y;
                    Node::absolutePosition(area, x, y);
                    if (y < historyClip.y + historyClip.height &&
                        y + area->height() > historyClip.y) {
                        markerExposed = true;
                        exposedMarkerBytes = markerOffset + plain.size();
                    }
                    markerOffset += plain.size();
                }
    assert(markerOffset == overscan.text.size());
    const std::string fullRange = beforeMarker + "\n" + overscan.text;
    assert(fullRange.starts_with(*clipboard.clipboardText()));
    const auto exposedLimit = beforeMarker.size() + (markerExposed ? 1 + exposedMarkerBytes : 0);
    assert(clipboard.clipboardText()->size() <= exposedLimit);
    assert(!host.open && model.actions.empty());
    view.clearTextSelection();
    viewport = {17, 23, 1200 * zoom, 800 * zoom};
    blast::Message lines;
    lines.id = "selection-visual-lines";
    lines.nick = "Peer";
    lines.text = "q\nA substantially longer second visual line.";
    buffer.messages.push_back(std::move(lines));
    blast::Message link;
    link.id = "selection-link";
    link.nick = "Peer";
    link.text = "https://example.org/selection-drag";
    buffer.messages.push_back(link);
    arrange();
    scroll->setScrollOffset(0);
    arrange();
    const auto shortLine = point("q", 0);
    const float longLineWidth = renderer
                                    .measureText("A substantially longer second visual line.",
                                                 14 * zoom, blast::themeMessageFont(), 0, 1)
                                    .width;
    drag(shortLine, std::pair{shortLine.first + longLineWidth * .8f, shortLine.second});
    assert(view.copySelection() && clipboard.clipboardText() == "q");
    view.clearTextSelection();
    drag(point(link.text, 0), point(link.text, link.text.size()));
    assert(view.copySelection() && clipboard.clipboardText() == link.text);
    view.clearTextSelection();
    const std::string wrappedGap(64, ' ');
    const std::string rtlWord = "אבגדה", mixedWord = "aאב", suffixWord = "א\u200f";
    for (const auto &word : {rtlWord, mixedWord, suffixWord}) {
        blast::Message message;
        message.id = "selection-wrapped-gap-" + word;
        message.nick = "Peer";
        message.text = word + wrappedGap + "tail";
        buffer.messages.push_back(std::move(message));
    }
    viewport.width = 240 * zoom;
    viewport.height = 2000 * zoom;
    arrange();
    scroll->setScrollOffset(0);
    arrange();
    const auto rtl = body(view, rtlWord);
    const auto rtlStart = point(rtlWord, 0);
    float rtlX, rtlY;
    Node::absolutePosition(&rtl.fragment, rtlX, rtlY);
    drag(rtlStart, std::pair{rtlX + rtl.fragment.width() + 12 * zoom, rtlStart.second});
    assert(!view.copySelection()); // Visual right is logical BEGIN, not omitted-space end.
    drag(point(rtlWord, 0), std::pair{rtlX - 12 * zoom, rtlStart.second});
    assert(view.copySelection() && clipboard.clipboardText() == rtlWord + wrappedGap);
    view.clearTextSelection();

    const auto mixed = body(view, mixedWord);
    blast::MessageSelectionText mixedLogical;
    mixedLogical.update(mixedWord);
    std::vector<TextCursorStop> mixedCells;
    renderer.measureTextCaretStops(mixed.text.text(), mixed.text.fontSize(),
                                   mixedLogical.boundaries(), 0, mixedCells, mixed.text.font(),
                                   true);
    assert(mixedCells.size() == mixedLogical.boundaries().size());
    const auto &lastCell = mixedCells[mixedCells.size() - 2];
    const float localX = lastCell.cellX + lastCell.cellWidth * .1f;
    const float localY = lastCell.y + lastCell.height * .5f;
    blast::MessageSelectionFragment mixedNative;
    mixedNative.rebuild(renderer, mixedLogical, 0, mixedWord.size(), mixed.text.text(),
                        mixed.text.fontSize(), mixed.text.font(), 0, true);
    assert(mixedNative.hit(localX, localY) == mixedWord.size());
    float mixedX, mixedY;
    Node::absolutePosition(&mixed.text, mixedX, mixedY);
    const auto &mixedMetrics = mixedNative.metrics();
    const auto mixedEnd = std::pair{mixedX - mixedMetrics.alignOffsetX + localX,
                                    mixedY - std::floor(mixedMetrics.layoutBaseline) + localY};
    drag(point(mixedWord, 0), mixedEnd);
    assert(view.copySelection() && clipboard.clipboardText() == mixedWord + wrappedGap);
    view.clearTextSelection();
    const auto suffix = body(view, suffixWord);
    blast::MessageSelectionText suffixLogical;
    suffixLogical.update(suffixWord);
    blast::MessageSelectionFragment suffixNative;
    suffixNative.rebuild(renderer, suffixLogical, 0, suffixWord.size(), suffix.text.text(),
                         suffix.text.fontSize(), suffix.text.font(), 0, true);
    std::vector<blast::MessageSelectionRect> suffixPaint;
    suffixNative.rectangles(0, 2, suffixPaint);
    assert(suffixPaint.size() == 1 && suffixPaint.front().width > 0);
    const auto paintedCell = suffixPaint.front();
    suffixNative.rectangles(2, suffixWord.size(), suffixPaint);
    assert(suffixPaint.empty()); // Standalone RLM is not a fabricated painted cell.
    bool equivalentFound = false;
    for (const float fraction : {.1f, .9f}) {
        const float cellX = paintedCell.x + paintedCell.width * fraction;
        const float cellY = paintedCell.y + paintedCell.height * .5f;
        const auto hit = suffixNative.hit(cellX, cellY);
        suffixNative.rectangles(
            hit, suffixLogical.snap(suffixWord.size(), blast::MessageSelectionSnap::After),
            suffixPaint);
        if (hit == suffixWord.size() || !suffixPaint.empty())
            continue;
        assert(hit == 2);
        float x, y;
        Node::absolutePosition(&suffix.text, x, y);
        const auto &metrics = suffixNative.metrics();
        const auto suffixEnd = std::pair{x - metrics.alignOffsetX + cellX,
                                         y - std::floor(metrics.layoutBaseline) + cellY};
        drag(point(suffixWord, 0), suffixEnd);
        assert(view.copySelection() && clipboard.clipboardText() == suffixWord + wrappedGap);
        equivalentFound = true;
        break;
    }
    assert(equivalentFound);
    view.clearTextSelection();
    dispatcher.setSceneRoot(nullptr);
}

void emptyLineSelection(TranscriptTestRenderer &renderer, float zoom) {
    blast::Model model;
    model.settings.scale = zoom;
    model.settings.messageScale = zoom;
    model.settings.timestamps = false;
    model.settings.avatars = false;
    model.settings.reducedMotion = true;
    model.settings.themeSource = "builtin";
    blast::Theme theme;
    theme.update(model);
    auto &buffer = model.buffers["empty-selection"];
    buffer.id = buffer.target = "empty-selection";
    for (const auto text : {"Lead establishes a grouped author.", "\n", "\nabc\n"}) {
        blast::Message message;
        message.id = "empty-" + std::to_string(buffer.messages.size());
        message.nick = "Peer";
        message.text = text;
        buffer.messages.push_back(std::move(message));
    }
    const auto before = buffer;
    blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
    Clipboard clipboard;
    MenuHost host;
    blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
    view.setPopupContext(&host);
    view.setReadEligible(false);
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&view);
    view.setInputDispatcher(dispatcher);
    LayoutRect viewport{17, 23, 700 * zoom, 800 * zoom};
    const auto arrange = [&] {
        view.sync();
        view.arrange(renderer, viewport);
    };
    const auto drag = [&](float x, float fromY, float toY) {
        assert(dispatcher.pointerButton(x, fromY, BTN_LEFT, true, 111, 1110, false));
        dispatcher.pointerMotion(x, toY, 112);
        dispatcher.pointerButton(x, toY, BTN_LEFT, false, 113, 1130, false);
        arrange();
        assert(!host.open);
    };
    arrange();
    dispatcher.setFocus(&body(view, "abc").row);
    assert(view.navigateMessage(-1)); // LF-only body, with no fabricated glyph to hit.
    arrange();
    const auto lf = view.selectedMessage();
    assert(lf && lf->anchor);
    const auto strut = renderer.measureText("Mg", 14 * zoom, blast::themeMessageFont(), 0, 1);
    const float lineHeight = strut.bottom - strut.top;
    const float padding = (lf->anchor->height - 2 * lineHeight) * .5f;
    assert(lineHeight > 0 && padding >= -.02f);
    const float x = lf->anchor->x + lf->anchor->width * .5f;
    const float firstBand = lf->anchor->y + padding + lineHeight * .5f;
    drag(x, firstBand, firstBand + lineHeight);
    assert(view.copySelection() && clipboard.clipboardText() == "\n");
    viewport.width = 240 * zoom;
    arrange();
    assert(view.copySelection() && clipboard.clipboardText() == "\n");
    view.clearTextSelection();
    const auto middle = body(view, "abc");
    float middleX, middleY;
    Node::mapToScene(&middle.fragment, middle.fragment.width() * .5f,
                     middle.fragment.height() * .5f, middleX, middleY);
    drag(middleX, middleY - lineHeight, middleY + lineHeight);
    assert(view.copySelection() && clipboard.clipboardText() == "\nabc\n");
    view.clearTextSelection();
    unchanged(buffer, before);
    assert(model.actions.empty());
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void messageLinkEnter(TranscriptTestRenderer &renderer) {
    blast::Model model;
    model.settings.reducedMotion = true;
    blast::Theme theme;
    theme.update(model);
    auto &buffer = model.buffers["enter-links"];
    buffer.id = buffer.target = "enter-links";
    buffer.kind = "query";
    buffer.server = "network";
    buffer.draft = "Untouched \x02 draft";
    blast::Message message;
    message.id = "original-id";
    message.nick = "Peer";
    message.text = "LinkBody without links";
    buffer.messages.push_back(message);
    blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = "untouched-reply"};
    Clipboard clipboard;
    MenuHost host;
    blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
    view.setPopupContext(&host);
    view.setReadEligible(false);
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&view);
    view.setInputDispatcher(dispatcher);
    const auto arrange = [&] {
        view.sync();
        view.arrange(renderer, {17, 23, 1000, 800});
    };
    const auto selectMessage = [&] {
        arrange();
        dispatcher.setFocus(&body(view, "LinkBody").row);
        assert(view.navigateMessage(-1));
        arrange();
        assert(dispatcher.focusedArea()->tabFocusKey().ends_with(":keyboard"));
    };
    const auto enter = [&](uint32_t sym = XKB_KEY_Return) {
        return view.handleKey({.sym = sym, .pressed = true});
    };
    const auto select = [&](size_t leaf) {
        assert(host.open && leaf < host.request.menu.size());
        const auto item = host.request.menu[leaf];
        assert(item.enabled && item.children.empty() && !item.header && !item.separator);
        auto callback = host.callbacks.onSelect;
        host.closeSelectDropdown();
        callback(item.value);
    };
    selectMessage();
    assert(enter() && !host.open && model.actions.empty());
    assert(!view.handleKey({.sym = XKB_KEY_Return, .modifiers = KeyMod::Ctrl, .pressed = true}));
    assert(!view.handleKey({.sym = XKB_KEY_Return, .pressed = true, .preedit = true}));
    assert(!view.handleKey({.sym = XKB_KEY_Return, .pressed = false}));
    dispatcher.setFocus(&body(view, "LinkBody").row);
    assert(!enter() && !host.open); // A focused native control keeps its own Enter behavior.

    // A private executable records explicit opens without a browser, network, or user config.
    ConfigDirectory launchDirectory;
    const auto executable = launchDirectory.path / "xdg-open";
    const auto record = launchDirectory.path / "xdg-open.url";
    auto *script = std::fopen(executable.c_str(), "w");
    assert(script);
    assert(std::fputs("#!/bin/sh\nprintf '%s' \"$1\" > \"$0.url\"\n", script) >= 0);
    assert(std::fclose(script) == 0);
    std::filesystem::permissions(executable, std::filesystem::perms::owner_all);
    const auto oldPath =
        std::getenv("PATH") ? std::optional<std::string>(std::getenv("PATH")) : std::nullopt;
    assert(setenv("PATH", launchDirectory.path.c_str(), 1) == 0);
    const auto opened = [&](std::string_view expected) {
        for (int i = 0; i < 200 && (!std::filesystem::exists(record) ||
                                    std::filesystem::file_size(record) != expected.size());
             ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        auto *file = std::fopen(record.c_str(), "r");
        assert(file);
        char value[512]{};
        const auto count = std::fread(value, 1, sizeof(value), file);
        assert(std::fclose(file) == 0);
        assert(std::string_view(value, count) == expected);
        std::filesystem::remove(record);
    };
    buffer.messages.front().text = "LinkBody \x02https://example.org/one\x02";
    ++buffer.messagesRevision;
    selectMessage();
    dispatcher.keyEvent(XKB_KEY_KP_Enter, 0, 0, true);
    dispatcher.keyEvent(XKB_KEY_KP_Enter, 0, 0, false);
    assert(host.open && host.request.menu.size() == 1);
    assert(host.request.menu.front().label == "https://example.org/one");
    assert(!std::filesystem::exists(record)); // Opening the picker never opens the URL.
    select(0);
    opened("https://example.org/one");
    InputArea *focusedLink = nullptr;
    const auto findLink = [&](auto &&self, Node &node) -> void {
        if (auto *area = dynamic_cast<InputArea *>(&node);
            area && area->tabFocusKey().find("https://example.org/one") != std::string::npos)
            focusedLink = area;
        for (const auto &child : node.children())
            self(self, *child);
    };
    findLink(findLink, view);
    assert(focusedLink);
    dispatcher.setFocus(focusedLink);
    assert(!enter() && !host.open);
    dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
    dispatcher.keyEvent(XKB_KEY_Return, 0, 0, false);
    opened("https://example.org/one"); // The native focused-link action still opens directly.

    buffer.messages.front().text =
        "LinkBody \x02https://example.org/one\x02 and https://example.org/two "
        "https://example.org/one javascript:alert(1) file:///private";
    ++buffer.messagesRevision;
    selectMessage();
    assert(enter() && host.open && host.request.menu.size() == 2);
    assert(host.request.menu[0].label == "https://example.org/one" &&
           host.request.menu[1].label == "https://example.org/two");
    const auto selected = *view.selectedMessage();
    blast::Message preceding;
    preceding.id = "preceding";
    preceding.text = "Earlier unrelated https://example.org/wrong";
    preceding.nick = "Peer";
    buffer.messages.insert(buffer.messages.begin(), preceding);
    ++buffer.messagesRevision;
    arrange();
    assert(view.selectedMessage()->identity == selected.identity);
    select(1);
    opened("https://example.org/two"); // Stable identity, not the old vector index.
    buffer.messages.erase(buffer.messages.begin());
    ++buffer.messagesRevision;

    // A forbidden launch would fail synchronously and set status; denied callbacks must not try.
    assert(setenv("PATH", "/blastirc-no-open-executable", 1) == 0);
    const auto denied = [&](const auto &mutate, const auto &restore) {
        selectMessage();
        assert(enter() && host.open);
        auto callback = host.callbacks.onSelect;
        host.closeSelectDropdown();
        mutate();
        model.status.clear();
        callback(0);
        assert(model.status.empty() && model.actions.empty());
        restore();
    };
    const auto saved = buffer.messages.front();
    denied([&] { buffer.messages.front().text += " replacement"; },
           [&] { buffer.messages.front() = saved; });
    denied([&] { buffer.messages.front().id = "different-id"; },
           [&] { buffer.messages.front() = saved; });
    denied([&] { buffer.messages.front().deleted = true; },
           [&] { buffer.messages.front() = saved; });
    denied([&] { buffer.messages.clear(); }, [&] { buffer.messages.push_back(saved); });
    denied([&] { ++buffer.messagesRevision; }, [] {});
    denied([&] { buffer.server = "other-network"; }, [&] { buffer.server = "network"; });
    denied([&] { buffer.target = "other-target"; }, [&] { buffer.target = buffer.id; });
    model.settings.foolDisplay = blast::FoolDisplay::Hidden;
    denied([&] { model.settings.fools.insert("Peer"); }, [&] { model.settings.fools.clear(); });
    model.settings.fools.insert("Peer");
    arrange();
    host.request = {};
    assert(!enter() && !host.open && host.request.menu.empty());
    model.settings.bufferFools[buffer.id] = true; // Explicit Show fools permits the source.
    selectMessage();
    assert(enter() && host.open && host.request.menu.size() == 2);
    host.closeSelectDropdown();

    auto &source = model.buffers["network/#source"];
    source.id = "network/#source";
    source.server = buffer.server;
    source.target = "#source";
    source.kind = "channel";
    source.messages.push_back(saved);
    source.messages.front().mention = true;
    buffer.kind = "mentions";
    buffer.messages.front().mention = true;
    buffer.messages.front().sourceBuffer = source.id;
    buffer.messages.front().sourceId = saved.id;
    buffer.messages.front().sourceServer = source.server;
    buffer.messages.front().sourceTarget = source.target;
    ++buffer.messagesRevision;
    arrange();
    host.request = {};
    assert(!enter() && !host.open && host.request.menu.empty()); // Feed Show fools grants nothing.
    model.settings.bufferFools[source.id] = true;
    selectMessage();
    assert(enter() && host.open && host.request.menu.size() == 2);
    host.closeSelectDropdown();
    denied([&] { source.messages.front().text += " changed"; },
           [&] { source.messages.front().text = saved.text; });
    denied([&] { source.messages.front().deleted = true; },
           [&] { source.messages.front().deleted = false; });
    denied([&] { buffer.messages.front().sourceServer = "wrong"; },
           [&] { buffer.messages.front().sourceServer = source.server; });
    denied([&] { model.settings.bufferFools[source.id] = false; },
           [&] { model.settings.bufferFools[source.id] = true; });
    const auto feedRow = buffer.messages.front();
    auto &other = model.buffers["other-source"];
    other = source;
    other.id = "other-source";
    denied(
        [&] { buffer.messages.front().sourceBuffer = other.id; },
        [&] { buffer.messages.front() = feedRow; }); // Even another valid source is not original.
    denied(
        [&] {
            source.messages.front().text += " changed";
            buffer.messages.front().text = source.messages.front().text;
        },
        [&] {
            source.messages.front().text = saved.text;
            buffer.messages.front() = feedRow;
        });
    assert(buffer.draft == "Untouched \x02 draft" && pane.reply == "untouched-reply");
    if (oldPath)
        assert(setenv("PATH", oldPath->c_str(), 1) == 0);
    else
        assert(unsetenv("PATH") == 0);
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void messageNavigation(TranscriptTestRenderer &renderer, bool traditional) {
    blast::Model model;
    model.settings.timestamps = true;
    model.settings.reducedMotion = true;
    model.settings.themeSource = "builtin";
    blast::Theme theme;
    theme.update(model);
    auto &buffer = model.buffers["navigation"];
    buffer.id = buffer.target = "navigation";
    buffer.server = "network";
    if (traditional)
        model.settings.messageLayouts[buffer.id] = blast::MessageLayout::Irc;
    auto &network = model.networks[buffer.server];
    network.state = "Ready";
    network.nick = "Me";
    network.caps = {"message-tags"};
    for (int i = 0; i < 5; ++i) {
        blast::Message message;
        message.id = "server:navigation-" + std::to_string(i);
        message.text = "NavigationBody" + std::to_string(i);
        message.nick = i == 3 ? "Me" : "Peer";
        message.own = i == 3;
        message.presence = i == 1 || i == 4;
        message.action = i == 2;
        if (i == 3) {
            message.id = "local:navigation";
            message.label = "navigation-send";
            message.pending = true;
            message.sendStatus = blast::SendStatus::Queued;
        }
        buffer.messages.push_back(std::move(message));
    }
    blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
    Clipboard clipboard;
    Node scene;
    scene.setSize(900, 1000);
    auto transcript =
        std::make_unique<blast::TranscriptView>(model, pane, clipboard, [](auto) {}, [](auto) {});
    auto &view = *transcript;
    scene.addChild(std::move(transcript));
    auto composer = std::make_unique<Input>();
    auto *input = composer.get();
    scene.addChild(std::move(composer));
    input->arrange(renderer, {17, 850, 500, 40});
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&scene);
    view.setInputDispatcher(dispatcher);
    view.setReadEligible(false);
    const LayoutRect viewport{17, 23, 700, 800};
    const auto arrange = [&] {
        view.sync();
        view.arrange(renderer, viewport);
    };
    const auto selected = [&](std::string_view nick) {
        const auto target = view.selectedMessage();
        assert(target && target->identity && target->buffer == buffer.id && target->nick == nick);
        assert(dispatcher.focusedArea() &&
               dispatcher.focusedArea()->tabFocusKey().ends_with(":keyboard"));
        return *target;
    };
    arrange();
    dispatcher.setFocus(input->inputArea());
    assert(!view.selectedMessage());
    assert(view.navigateMessage(-1));
    arrange();
    const auto queued = selected("Me");
    buffer.draft = "Retained composer draft";
    const auto draft = buffer.draft;
    assert(queued.canReply && !queued.canReact && !pane.following && !pane.focusComposer);
    assert(view.replyToMessage(queued) && pane.reply == "echo:navigation-send" &&
           pane.focusComposer && buffer.draft == draft && model.actions.empty());
    assert(!view.reactToMessage(queued, "👍"));
    pane.reply.clear();
    pane.focusComposer = false;
    assert(view.navigateMessage(-1));
    arrange();
    const auto action = selected("Peer");
    assert(action.identity != queued.identity && action.canReply && action.canReact);
    assert(view.navigateMessage(-1));
    arrange();
    const auto oldest = selected("Peer");
    assert(oldest.identity != action.identity);
    assert(view.navigateMessage(-1));
    assert(selected("Peer").identity == oldest.identity);
    assert(view.navigateMessage(1));
    arrange();
    assert(selected("Peer").identity == action.identity);
    assert(view.navigateMessage(1));
    arrange();
    assert(selected("Me").identity == queued.identity);
    assert(!view.navigateMessage(1));
    assert(!view.selectedMessage()); // The caller can return keyboard ownership to the composer.
    dispatcher.setFocus(input->inputArea());
    assert(view.navigateMessage(-1));
    arrange();
    assert(selected("Me").identity == queued.identity);
    dispatcher.setFocus(input->inputArea());
    assert(!view.selectedMessage() && view.resolveMessage(queued));

    buffer.messages[3].id = "server:confirmed-navigation";
    buffer.messages[3].pending = false;
    buffer.messages[3].sendStatus = blast::SendStatus::Confirmed;
    ++buffer.messagesRevision;
    arrange();
    const auto confirmed = view.resolveMessage(queued);
    assert(confirmed && confirmed->identity == queued.identity && confirmed->canReply &&
           confirmed->canReact && confirmed->anchor);
    assert(confirmed->anchor->width > 0 && confirmed->anchor->height > 0);
    assert(confirmed->anchor->x >= viewport.x - .02f && confirmed->anchor->y >= viewport.y - .02f &&
           confirmed->anchor->x + confirmed->anchor->width <= viewport.x + viewport.width + .02f &&
           confirmed->anchor->y + confirmed->anchor->height <= viewport.y + viewport.height + .02f);
    assert(view.replyToMessage(queued));
    assert(pane.reply == "echo:navigation-send" && pane.focusComposer && buffer.draft == draft);
    pane.reply.clear();
    pane.focusComposer = false;
    assert(view.reactToMessage(queued, "👍"));
    assert(model.actions.size() == 1 && model.actions.back().type == "react" &&
           model.actions.back().buffer == buffer.id &&
           model.actions.back().extra == "server:confirmed-navigation" &&
           model.actions.back().text == "👍");
    model.actions.clear();
    for (const char *state : {"Disconnected", "Failed"}) {
        network.state = state;
        arrange();
        const auto offline = view.resolveMessage(queued);
        assert(offline && offline->canReply && !offline->canReact);
        assert(view.replyToMessage(queued) && pane.reply == "echo:navigation-send" &&
               buffer.draft == draft && !view.reactToMessage(queued, "👍") &&
               model.actions.empty());
        pane.reply.clear();
        pane.focusComposer = false;
    }
    network.state = "Ready";
    arrange();
    dispatcher.setFocus(input->inputArea());
    assert(view.navigateMessage(-1));
    arrange();
    assert(selected("Me").identity == queued.identity);
    dispatcher.setFocus(&body(view, "NavigationBody2").row);
    const auto focusedAction = view.selectedMessage();
    assert(focusedAction && focusedAction->identity == action.identity);
    assert(view.replyToMessage(*focusedAction) && pane.reply == "message:server:navigation-2");
    pane.reply.clear();
    pane.focusComposer = false;
    assert(view.reactToMessage(*focusedAction, "😂"));
    assert(model.actions.size() == 1 && model.actions.back().extra == "server:navigation-2" &&
           model.actions.back().text == "😂");
    model.actions.clear();
    network.caps.clear();
    arrange();
    const auto denied = view.resolveMessage(queued);
    assert(denied && denied->canReply && !denied->canReact);
    assert(view.replyToMessage(queued) && pane.reply == "echo:navigation-send" &&
           buffer.draft == draft && !view.reactToMessage(queued, "👍") && model.actions.empty());
    pane.reply.clear();
    pane.focusComposer = false;
    network.caps = {"message-tags"};
    std::swap(buffer.messages[2], buffer.messages[3]);
    ++buffer.messagesRevision;
    arrange();
    assert(view.resolveMessage(queued));
    auto otherBuffer = queued;
    otherBuffer.buffer = "different-buffer";
    assert(!view.resolveMessage(otherBuffer) && !view.replyToMessage(otherBuffer) &&
           !view.reactToMessage(otherBuffer, "👍"));
    auto removed = buffer.messages[2];
    buffer.messages.erase(buffer.messages.begin() + 2);
    ++buffer.messagesRevision;
    arrange();
    assert(!view.resolveMessage(queued));
    removed.text = "Different message with a reused wire id";
    removed.label.clear();
    buffer.messages.push_back(std::move(removed));
    ++buffer.messagesRevision;
    arrange();
    assert(!view.resolveMessage(queued) && !view.replyToMessage(queued) &&
           !view.reactToMessage(queued, "👍"));
    assert(model.actions.empty());
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void linkPreviewMenus(TranscriptTestRenderer &renderer) {
    blast::Model model;
    model.settings.reducedMotion = true;
    model.settings.themeSource = "builtin";
    blast::Theme theme;
    theme.update(model);
    auto &buffer = model.buffers["preview-menu"];
    buffer.id = buffer.target = "preview-menu";
    buffer.server = "network";
    buffer.kind = "channel";
    auto &network = model.networks[buffer.server];
    network.state = "Ready";
    network.nick = "Me";
    blast::Message message;
    message.id = "preview-message";
    message.nick = "Peer";
    message.text = "https://example.org/first https://example.org/second";
    buffer.messages.push_back(message);
    blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
    Clipboard clipboard;
    MenuHost host;
    Node overlay;
    blast::MediaPreviewController controller(model, overlay);
    blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
    view.setMediaPreviewController(&controller);
    view.setPopupContext(&host);
    view.setReadEligible(false);
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&view);
    view.setInputDispatcher(dispatcher);
    const auto arrange = [&] {
        view.sync();
        view.arrange(renderer, {17, 23, 700, 800});
    };
    const auto open = [&] {
        arrange();
        dispatcher.setFocus(&body(view, "https://example.org/first").row);
        assert(view.handleKey({.sym = XKB_KEY_Menu, .pressed = true}));
        assert(host.open && host.contains("Copy text") && host.contains("Load preview"));
        assert(host.entry("Load preview")->icon == "download");
        assert(!host.contains("Copy Link") && !host.contains("Open Link"));
    };
    open();
    const auto selected = view.selectedMessage();
    assert(selected && controller.canLoadPreview(selected->identity, "https://example.org/first"));
    assert(!controller.canLoadPreview(selected->identity, "https://example.org/second"));
    host.select("Copy text");
    assert(clipboard.clipboardText() == message.text);
    const auto &urlBody = body(view, "https://example.org/first");
    float urlX, urlY;
    Node::mapToScene(&urlBody.fragment, urlBody.fragment.width() * .5f,
                     urlBody.fragment.height() * .5f, urlX, urlY);
    assert(dispatcher.pointerButton(urlX, urlY, BTN_RIGHT, true, 17, 170, false));
    dispatcher.pointerButton(urlX, urlY, BTN_RIGHT, false, 18, 180, false);
    assert(host.open && host.contains("Load preview"));
    host.closeSelectDropdown();
    const auto denied = [&](auto change, auto restore) {
        open();
        change(); // Held native menus must reauthorize even before transcript reflow.
        host.select("Load preview");
        assert(!controller.active());
        restore();
        assert(controller.canLoadPreview(selected->identity, "https://example.org/first"));
    };
    auto &entry = buffer.messages.front();
    denied([&] { entry.text += " changed"; }, [&] { entry.text = message.text; });
    denied([&] { entry.nick = "Different"; }, [&] { entry.nick = message.nick; });
    denied([&] { entry.sourceBuffer = "missing-source"; }, [&] { entry.sourceBuffer.clear(); });
    denied([&] { entry.deleted = true; }, [&] { entry.deleted = false; });
    denied([&] { entry.presence = true; }, [&] { entry.presence = false; });
    denied([&] { pane.buffer = "other"; }, [&] { pane.buffer = buffer.id; });
    denied([&] { model.settings.fools.insert("Peer"); }, [&] { model.settings.fools.clear(); });
    denied([&] { model.settings.linkPreviews = blast::PreviewMode::Off; },
           [&] { model.settings.linkPreviews = blast::PreviewMode::Click; });
    model.settings.linkPreviews = blast::PreviewMode::Off;
    arrange();
    dispatcher.setFocus(&body(view, "https://example.org/first").row);
    assert(view.handleKey({.sym = XKB_KEY_Menu, .pressed = true}));
    assert(host.open && host.contains("Copy text") && !host.contains("Load preview"));
    host.closeSelectDropdown();
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void keyboardMenus(TranscriptTestRenderer &renderer, bool traditional = false,
                   const char *kind = "channel") {
    blast::Model model;
    model.settings.timestamps = true;
    model.settings.reducedMotion = true;
    model.settings.themeSource = "builtin";
    blast::Theme theme;
    theme.update(model);
    auto &buffer = model.buffers["keyboard-channel"];
    buffer.id = buffer.target = "keyboard-channel";
    buffer.kind = kind;
    if (traditional)
        model.settings.messageLayouts[buffer.id] = blast::MessageLayout::Irc;
    buffer.server = "network";
    auto &network = model.networks[buffer.server];
    network.state = "Ready";
    network.nick = "Me";
    network.caps = {"message-tags", "draft/message-redaction"};
    for (int i = 0; i < 4; ++i) {
        blast::Message message;
        message.id = "keyboard-message-" + std::to_string(i);
        message.nick = i == 1 ? "Peer" : "Me";
        message.own = i != 1;
        message.pending = i == 3;
        message.text = i == 0   ? "KeyboardOriginal"
                       : i == 1 ? "KeyboardWrapped native fragments and "
                                  "https://example.org/keyboard/menu wrap without hiding their "
                                  "message actions behind a nickname or link focus target. "
                                  "https://example.org/keyboard/menu"
                       : i == 2 ? "KeyboardReplyBody"
                                : "KeyboardPendingBody";
        if (i == 2)
            message.reply = "keyboard-message-0";
        buffer.messages.push_back(std::move(message));
    }
    const auto before = buffer;
    Clipboard clipboard;
    MenuHost host;
    std::string command;
    blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
    blast::TranscriptView view(
        model, pane, clipboard, [&](auto value) { command = value; }, [](auto) {});
    view.setPopupContext(&host);
    view.setReadEligible(false);
    const LayoutRect rect{17, 23, 360, 720};
    view.sync();
    view.arrange(renderer, rect);
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&view);
    view.setInputDispatcher(dispatcher);
    const auto replyBody = body(view, "KeyboardReplyBody");
    InputArea *reference = nullptr;
    for (const auto &child : replyBody.content.children())
        if (auto *area = dynamic_cast<InputArea *>(child.get());
            area && area->focusable() && area->tabFocusKey().find("5:reply") != std::string::npos)
            reference = area;
    assert(reference);
    float referenceX, referenceY;
    Node::mapToScene(reference, reference->width() * .5f, reference->height() * .5f, referenceX,
                     referenceY);
    assert(dispatcher.pointerButton(referenceX, referenceY, BTN_LEFT, true, 71, 710, true));
    assert(reference->pressed() && pane.following && !host.open && model.actions.empty());
    dispatcher.pointerButton(referenceX, referenceY, BTN_LEFT, false, 72, 720, true);
    assert(!pane.following && !host.open && model.actions.empty());
    pane.following = true;
    view.sync();
    view.arrange(renderer, rect);
    {
        const auto sender = body(view, "KeyboardWrapped");
        float x, y;
        Node::mapToScene(&sender.fragment, sender.fragment.width() * .25f,
                         sender.fragment.height() * .5f, x, y);
        assert(dispatcher.pointerButton(x, y, BTN_RIGHT, true, 801, 8010, false));
        dispatcher.pointerButton(x, y, BTN_RIGHT, false, 802, 8020, false);
        assert(host.open && host.request.inputSerial == 801 &&
               host.request.anchorX == int(std::floor(x)) &&
               host.request.anchorY == int(std::floor(y)) && host.request.anchorWidth == 1 &&
               host.request.anchorHeight == 1);
        host.select("Whois Peer");
        assert(command == "nick-action:" + buffer.id + "\nPeer\nwhois");
        command.clear();
    }
    auto linkTargets = [&] {
        const auto wrapped = body(view, "KeyboardWrapped");
        std::vector<InputArea *> targets;
        auto visit = [&](auto &&self, Node &node) -> void {
            if (auto *area = dynamic_cast<InputArea *>(&node);
                area && area->focusable() && area->tabStop()) {
                const auto hint = area->tooltipContent();
                if (const auto *url = std::get_if<std::string>(&hint);
                    url && *url == "https://example.org/keyboard/menu")
                    targets.push_back(area);
            }
            for (const auto &child : node.children())
                self(self, *child);
        };
        visit(visit, wrapped.content);
        return targets;
    };
    const auto originalLinks = linkTargets();
    assert(originalLinks.size() == 2);
    // This transcript-only fixture has no editor to consume its initial focus request.
    pane.focusComposer = false;
    dispatcher.setFocus(originalLinks[1]);
    view.arrange(renderer, {rect.x, rect.y, 600, rect.height});
    const auto reflowedLinks = linkTargets();
    assert(reflowedLinks.size() == 2);
    assert(dispatcher.focusedArea() == reflowedLinks[1]);
    assert(view.handleKey({.sym = XKB_KEY_Menu, .pressed = true}));
    assert(host.open && host.contains("Copy text"));
    host.select("Copy text");
    assert(clipboard.clipboardText() == buffer.messages[1].text);
    dispatcher.setFocus(nullptr);
    view.arrange(renderer, rect);
    const auto currentLinks = linkTargets();
    assert(currentLinks.size() == 2);
    dispatcher.setFocus(currentLinks[1]);
    assert(view.handleKey({.sym = XKB_KEY_Menu, .pressed = true}));
    auto textWithLinks = buffer.messages[1].text;
    buffer.messages[1].text = "KeyboardWrapped remains readable after its URL action is removed";
    view.sync();
    view.arrange(renderer, rect);
    assert(dispatcher.focusedArea() == &body(view, "KeyboardWrapped").row);
    host.select("Copy text");
    assert(clipboard.clipboardText() == buffer.messages[1].text);
    dispatcher.setFocus(nullptr);
    buffer.messages[1].text = std::move(textWithLinks);
    view.sync();
    view.arrange(renderer, rect);

    InputArea *keyboard = nullptr;
    auto findKeyboard = [&](auto &&self, Node &node) -> void {
        if (auto *area = dynamic_cast<InputArea *>(&node);
            area && area->tabFocusKey().starts_with("transcript:") &&
            area->tabFocusKey().ends_with(":keyboard"))
            keyboard = area;
        for (const auto &child : node.children())
            self(self, *child);
    };
    findKeyboard(findKeyboard, view);
    assert(keyboard);
    const auto restoredLinks = linkTargets();
    assert(restoredLinks.size() == 2);
    dispatcher.setFocus(restoredLinks[1]);
    auto removed = std::move(buffer.messages[1]);
    buffer.messages.erase(buffer.messages.begin() + 1);
    ++buffer.messagesRevision;
    view.sync();
    view.arrange(renderer, rect);
    assert(dispatcher.focusedArea() == keyboard);
    dispatcher.setFocus(nullptr);
    buffer.messages.insert(buffer.messages.begin() + 1, std::move(removed));
    ++buffer.messagesRevision;
    view.sync();
    view.arrange(renderer, rect);
    auto focusOutline = [&](InputArea &row) {
        RectNode *outline = nullptr;
        for (const auto &child : row.children())
            if (auto *rect = dynamic_cast<RectNode *>(child.get());
                rect && rect->visible() && rect->style().borderWidth > 0 &&
                rect->style().border == blast::themeColor("secondary"))
                outline = rect;
        assert(outline && outline->style().fill.a == 0);
        const auto bounds = sceneBounds(*outline);
        auto *material = find<blast::MaterialNode>(row);
        assert(material);
        const auto surface = sceneBounds(*material);
        near(bounds.x, surface.x);
        near(bounds.y, surface.y);
        near(bounds.height, surface.height);
        if (blast::messageBubbles(model.settings, buffer))
            near(bounds.width, surface.width);
        else
            assert(bounds.width > 0 && bounds.width <= surface.width + .02f);
        containedBounds(bounds, sceneBounds(row));
        const auto clip = nativeHistoryClip(view);
        containedBounds(intersectBounds(bounds, clip), clip);
    };
    auto menuAnchor = [&](InputArea &row) {
        const auto clip = nativeHistoryClip(view);
        const auto visible = intersectBounds(sceneBounds(row), clip);
        containedBounds(visible, clip);
        const LayoutRect anchor{float(host.request.anchorX), float(host.request.anchorY),
                                float(host.request.anchorWidth), float(host.request.anchorHeight)};
        assert(anchor.width > 0 && anchor.height > 0);
        // Popup coordinates are integer-rasterized; the native selected row's visible
        // intersection, not the full transcript or bubble, supplies this keyboard anchor.
        const auto sameEdge = [](float actual, float expected) {
            assert(std::abs(actual - expected) <= 1.02f);
        };
        sameEdge(anchor.x, visible.x);
        sameEdge(anchor.y, visible.y);
        sameEdge(anchor.x + anchor.width, visible.x + visible.width);
        sameEdge(anchor.y + anchor.height, visible.y + visible.height);
        containedBounds(anchor,
                        {clip.x - 1.02f, clip.y - 1.02f, clip.width + 2.04f, clip.height + 2.04f});
    };
    auto open = [&](std::string_view token, std::uint32_t key, std::uint32_t modifiers = 0) {
        auto &row = body(view, token).row;
        dispatcher.setFocus(nullptr);
        bool reached = false;
        for (int i = 0; i < 128 && dispatcher.cycleTabFocus(false); ++i)
            if (dispatcher.focusedArea() == &row) {
                reached = true;
                break;
            }
        assert(reached && row.focused());
        focusOutline(row);
        dispatcher.keyEvent(key, 0, modifiers, true);
        dispatcher.keyEvent(key, 0, modifiers, false);
        assert(host.open && host.contains("Copy text"));
        menuAnchor(row);
    };
    for (const bool policyCleanup : {false, true}) {
        open("KeyboardWrapped", XKB_KEY_Menu);
        assert(host.callbacks.onDismiss);
        host.dismissReplacedOnOpen = true;
        open("KeyboardReplyBody", XKB_KEY_Menu);
        host.dismissReplacedOnOpen = false;
        assert(host.open && host.contains("Copy text"));
        const auto capabilities = network.caps;
        const auto originalBuffer = pane.buffer;
        const std::string replacement = originalBuffer + "/popup-replacement";
        if (policyCleanup)
            network.caps.clear();
        else {
            auto &other = model.buffers[replacement];
            other.id = other.target = replacement;
            other.kind = buffer.kind;
            pane.buffer = replacement;
        }
        view.sync();
        view.arrange(renderer, rect);
        assert(!host.open);
        network.caps = capabilities;
        pane.buffer = originalBuffer;
        view.sync();
        view.arrange(renderer, rect);
        if (!policyCleanup)
            model.buffers.erase(replacement);
    }
    open("KeyboardWrapped", XKB_KEY_Menu);
    host.select("Copy text");
    assert(clipboard.clipboardText() == buffer.messages[1].text);
    open("KeyboardReplyBody", XKB_KEY_F10, KeyMod::Shift);
    host.select("Reply");
    assert(pane.reply == "message:" + buffer.messages[2].id && pane.focusComposer);
    dispatcher.keyEvent(XKB_KEY_F10, 0, KeyMod::Ctrl, true);
    dispatcher.keyEvent(XKB_KEY_F10, 0, KeyMod::Ctrl, false);
    assert(!host.open && model.actions.empty());
    open("KeyboardWrapped", XKB_KEY_Menu);
    host.select("React/👍");
    assert(model.actions.size() == 1 && model.actions.back().type == "react" &&
           model.actions.back().buffer == buffer.id && model.actions.back().text == "👍" &&
           model.actions.back().extra == buffer.messages[1].id);
    model.actions.clear();
    open("KeyboardOriginal", XKB_KEY_F10);
    host.select("Delete message");
    assert(model.actions.size() == 1 && model.actions.back().type == "redact" &&
           model.actions.back().extra == buffer.messages[0].id);
    model.actions.clear();
    open("KeyboardWrapped", XKB_KEY_Menu);
    assert(host.contains("React/Choose emoji…"));
    host.select("React/Choose emoji…");
    const std::string reactionPrefix = "message-react:" + buffer.id + "\n";
    assert(command.starts_with(reactionPrefix));
    blast::TranscriptMessageTarget chosen{};
    chosen.buffer = buffer.id;
    chosen.identity = std::stoull(command.substr(reactionPrefix.size()));
    const auto chosenMessage = view.resolveMessage(chosen);
    assert(chosenMessage && chosenMessage->nick == "Peer" && chosenMessage->canReact);
    assert(view.reactToMessage(chosen, "👀"));
    assert(model.actions.size() == 1 && model.actions.back().type == "react" &&
           model.actions.back().buffer == buffer.id && model.actions.back().text == "👀" &&
           model.actions.back().extra == buffer.messages[1].id);
    model.actions.clear();
    open("KeyboardPendingBody", XKB_KEY_F10);
    assert(host.contains("Reply") && !host.contains("React/👍") &&
           !host.contains("Delete message"));
    host.select("Copy text");
    assert(clipboard.clipboardText() == buffer.messages[3].text);
    network.caps.clear();
    view.sync();
    view.arrange(renderer, rect);
    open("KeyboardOriginal", XKB_KEY_Menu);
    assert(host.contains("Reply") && !host.contains("React/👍") &&
           !host.contains("Delete message"));
    host.select("Copy text");
    assert(clipboard.clipboardText() == buffer.messages[0].text);
    assert(model.actions.empty());
    dispatcher.setFocus(nullptr);
    const LayoutRect shortView{rect.x, rect.y, rect.width, 64};
    view.arrange(renderer, shortView);
    auto *scroll = find<ScrollView>(view);
    assert(scroll && scroll->maxScrollOffset() > 0);
    scroll->setScrollOffset(scroll->maxScrollOffset() * .45f);
    view.sync();
    view.arrange(renderer, shortView);
    assert(!pane.following);
    const float retainedScroll = scroll->scrollOffset();
    const char *tokens[] = {"KeyboardOriginal", "KeyboardWrapped", "KeyboardReplyBody",
                            "KeyboardPendingBody"};
    std::vector<std::pair<InputArea *, std::size_t>> rows;
    int offscreen = 0;
    for (std::size_t i = 0; i < std::size(tokens); ++i)
        if (bodyText(view, tokens[i])) {
            auto &row = body(view, tokens[i]).row;
            rows.emplace_back(&row, i);
            const auto visible = intersectBounds(sceneBounds(row), nativeHistoryClip(view));
            offscreen += visible.width <= 0 || visible.height <= 0;
        }
    assert(offscreen > 0);
    assert(bounds(body(view, "KeyboardWrapped")).height > nativeHistoryClip(view).height);
    InputArea *firstFocus = nullptr;
    bool tallRowReached = false;
    int focusedRows = 0;
    for (int i = 0; i < 128 && dispatcher.cycleTabFocus(false); ++i) {
        auto *focused = dispatcher.focusedArea();
        assert(focused);
        if (!firstFocus)
            firstFocus = focused;
        else if (focused == firstFocus)
            break;
        view.sync();
        auto row = std::find_if(rows.begin(), rows.end(), [&](const auto &entry) {
            for (auto *node = static_cast<Node *>(focused); node; node = node->parent())
                if (node == entry.first)
                    return true;
            return false;
        });
        if (row == rows.end())
            continue;
        const auto clip = nativeHistoryClip(view);
        containedBounds(intersectBounds(sceneBounds(*row->first), clip), clip);
        focusOutline(*row->first);
        if (focused != row->first)
            continue;
        ++focusedRows;
        tallRowReached |= row->second == 1;
        dispatcher.keyEvent(i % 2 ? XKB_KEY_Menu : XKB_KEY_F10, 0, 0, true);
        dispatcher.keyEvent(i % 2 ? XKB_KEY_Menu : XKB_KEY_F10, 0, 0, false);
        assert(host.open && host.contains("Copy text"));
        menuAnchor(*row->first);
        host.select("Copy text");
        assert(clipboard.clipboardText() == buffer.messages[row->second].text);
    }
    assert(focusedRows > 0 && tallRowReached);
    near(scroll->scrollOffset(), retainedScroll);
    assert(!pane.following && pane.unseen == 0 && model.actions.empty());
    open("KeyboardWrapped", XKB_KEY_Menu);
    host.select("Copy text");
    scroll->setScrollOffset(scroll->maxScrollOffset());
    view.sync();
    view.arrange(renderer, shortView);
    dispatcher.keyEvent(XKB_KEY_F10, 0, 0, true);
    dispatcher.keyEvent(XKB_KEY_F10, 0, 0, false);
    assert(!host.open && model.actions.empty());
    unchanged(buffer, before);

    network.caps = {"message-tags", "draft/message-redaction"};
    scroll->setScrollOffset(retainedScroll);
    view.sync();
    view.arrange(renderer, shortView);
    open("KeyboardWrapped", XKB_KEY_F10, KeyMod::Shift);
    host.select("Copy text");
    auto *protectedRow = &body(view, "KeyboardWrapped").row;
    assert(dispatcher.focusedArea() == protectedRow);
    auto retainsOriginalBody = [&](auto &&self, Node &node) -> bool {
        if (auto *text = dynamic_cast<TextNode *>(&node);
            text && text->useMarkup() && text->text().find("KeyboardWrapped") != std::string::npos)
            return true;
        for (const auto &child : node.children())
            if (self(self, *child))
                return true;
        return false;
    };
    std::size_t poolCeiling = 0;
    for (int batch = 0; batch < 3; ++batch) {
        for (int i = 0; i < 64; ++i) {
            blast::Message arrival;
            arrival.id = arrival.text = "PoolArrival-" + std::to_string(batch * 64 + i);
            arrival.nick = "Me";
            arrival.own = true;
            buffer.messages.push_back(std::move(arrival));
        }
        const auto appended = buffer;
        view.sync();
        view.arrange(renderer, shortView);
        scroll->setScrollOffset(scroll->maxScrollOffset());
        view.sync();
        view.arrange(renderer, shortView);
        assert(!dispatcher.focusedArea() || dispatcher.focusedArea() == protectedRow);
        assert(retainsOriginalBody(retainsOriginalBody, *protectedRow));
        const auto newest = body(view, "PoolArrival-" + std::to_string(batch * 64 + 63));
        assert(&newest.row != protectedRow);
        for (const auto &child : newest.row.children())
            if (const auto *outline = dynamic_cast<RectNode *>(child.get());
                outline && outline->style().borderWidth > 0)
                assert(!outline->visible());
        std::size_t poolSize = 0;
        for (const auto &child : protectedRow->parent()->children())
            if (auto *area = dynamic_cast<InputArea *>(child.get());
                area && find<blast::MaterialNode>(*area))
                ++poolSize;
        if (!batch)
            poolCeiling = poolSize;
        else
            assert(poolSize <= poolCeiling + 1);
        unchanged(buffer, appended);
    }
    const auto replyBefore = pane.reply;
    dispatcher.keyEvent(XKB_KEY_F10, 0, KeyMod::Shift, true);
    dispatcher.keyEvent(XKB_KEY_F10, 0, KeyMod::Shift, false);
    assert(!host.open && model.actions.empty() && pane.reply == replyBefore);
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void followAndReflow(TranscriptTestRenderer &renderer, blast::Theme &theme, blast::Model &model,
                     blast::PaneState &pane, blast::TranscriptView &view) {
    auto &buffer = model.buffers.at(pane.buffer);
    for (int i = 0; i < 32; ++i) {
        blast::Message message;
        message.id = "tail-" + std::to_string(i);
        message.nick = "Me";
        message.presence = true;
        message.text = i == 24
                           ? "AnchorToken \x02"
                             "bold native shaping"
                             "\x02"
                             " wraps this presence notification with https://example.org/geometry "
                             "and more text so narrower widths genuinely reflow the retained body."
                           : "Tail" + std::to_string(i);
        buffer.messages.push_back(std::move(message));
    }
    auto before = buffer;
    view.sync();
    view.arrange(renderer, {17, 23, 560, 160});
    auto *scroll = find<ScrollView>(view);
    assert(scroll && scroll->maxScrollOffset() > 0 && pane.following);
    near(scroll->scrollOffset(), scroll->maxScrollOffset());
    unchanged(buffer, before);
    // Navigate to the retained source before measuring its body; tail overscan
    // need not contain this older row under every native font.
    assert(view.jumpToSourceMessage("tail-24"));
    view.arrange(renderer, {17, 23, 560, 160});

    const auto oldBody = bounds(body(view, "AnchorToken"));
    const float desired = scroll->scrollOffset() + oldBody.top - view.y() + .25f * oldBody.height;
    assert(desired > 0 && desired < scroll->maxScrollOffset());
    scroll->setScrollOffset(desired);
    assert(!pane.following);
    view.sync();
    view.arrange(renderer, {17, 23, 560, 160});
    auto anchored = bounds(body(view, "AnchorToken"));
    near(anchored.top - view.y(), -.25f * anchored.height);

    model.settings.messageFont = "serif";
    model.settings.scale = 1.5f;
    theme.update(model);
    view.sync();
    view.arrange(renderer, {17, 23, 360, 160});
    anchored = bounds(body(view, "AnchorToken"));
    assert(anchored.height > oldBody.height && !pane.following && pane.unseen == 0);
    near(anchored.top - view.y(), -.25f * anchored.height);
    unchanged(buffer, before);

    scroll->setScrollOffset(scroll->maxScrollOffset());
    assert(pane.following);
    blast::Message message;
    message.id = message.text = "FinalArrival";
    message.nick = "Me";
    message.presence = true;
    buffer.messages.push_back(message);
    before = buffer;
    view.sync();
    view.arrange(renderer, {17, 23, 360, 160});
    assert(pane.following && pane.unseen == 0);
    near(scroll->scrollOffset(), scroll->maxScrollOffset());
    const auto last = bounds(body(view, "FinalArrival"));
    near(last.top + last.height + 2 * model.settings.scale, view.y() + scroll->height());
    unchanged(buffer, before);
    assert(model.actions.empty());
}

void smoothWheel(TranscriptTestRenderer &renderer, std::string_view renameTarget = "") {
    struct MotionState {
        bool enabled = MotionService::instance().enabled();
        float speed = MotionService::instance().speed();
        ~MotionState() {
            MotionService::instance().setSpeed(speed);
            MotionService::instance().setEnabled(enabled);
        }
    } motion;
    blast::Model model;
    model.settings.themeSource = "builtin";
    model.settings.fx = blast::FxLevel::Full;
    model.settings.reducedMotion = false;
    model.settings.timestamps = model.settings.avatars = false;
    blast::Theme theme;
    theme.update(model);
    // Keep real walltime animation, but widen its partial-frame window: a
    // single native detent otherwise has only a 50ms minimum duration.
    MotionService::instance().setSpeed(.25f);
    assert(MotionService::instance().enabled());
    auto &buffer = model.buffers["wheel"];
    buffer.id = buffer.target = "wheel";
    buffer.unread = 7;
    for (int i = 0; i < 40; ++i) {
        blast::Message message;
        message.id = message.text = "WheelTail" + std::to_string(i);
        message.nick = "Me";
        message.presence = true;
        buffer.messages.push_back(std::move(message));
    }
    buffer.readMarker = buffer.messages.back().id;
    auto before = buffer;
    AnimationManager animations;
    blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
    Clipboard clipboard;
    blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
    view.setReadEligible(false);
    view.setAnimationManager(&animations);
    const LayoutRect rect{17, 23, 560, 240};
    view.sync();
    view.arrange(renderer, rect);
    auto *scroll = find<ScrollView>(view);
    assert(scroll && pane.following);
    const float start = scroll->scrollOffset();
    near(start, scroll->maxScrollOffset());
    const float step = Style::scrollWheelStep;
    assert(start > step);
    const auto anchorBefore = bounds(body(view, "WheelTail35"));
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&view);
    // Main receives normalized axisLines from WaylandSeat: value120 / 120.
    // InputDispatcher forwards those units rather than deriving them from raw tags.
    assert(dispatcher.pointerAxis(rect.x + 80, rect.y + 120, WL_POINTER_AXIS_VERTICAL_SCROLL,
                                  WL_POINTER_AXIS_SOURCE_WHEEL, -15, 0, -120, -1.f));
    near(scroll->scrollOffset(), start); // No animation setter has run yet.
    const float target = start - step;
    const auto began = std::chrono::steady_clock::now();
    for (int frame = 1; frame <= 16; ++frame) {
        std::this_thread::sleep_until(began + std::chrono::milliseconds(16 * frame));
        model.now = .016 * frame;
        if (!renameTarget.empty() && frame == 2) {
            view.beforeBufferRename(buffer.id, renameTarget);
            if (renameTarget == buffer.id) {
                buffer.target = "WhEeL";
                view.remapBuffer(buffer.id, buffer.id);
            }
            // A different-ID request intentionally has no post-remap: storage can reject it.
        }
        // Match the real frame order: retained sync/layout precedes setters.
        // Both the first pre-setter layout and later partially-scrolled layouts
        // must leave the SDK's wheel animation alive.
        view.sync();
        view.arrange(renderer, rect);
        const float previous = scroll->scrollOffset();
        animations.tick(16);
        const float current = scroll->scrollOffset();
        assert(current <= previous && current >= target - .02f);
        if (frame <= 2) {
            assert(current < previous && current > target);
            assert(!pane.following);
        }
    }
    assert(!animations.hasActive());
    view.sync();
    view.arrange(renderer, rect);
    near(scroll->scrollOffset(), target);
    const auto anchorAfter = bounds(body(view, "WheelTail35"));
    near(anchorAfter.top - anchorBefore.top, step);
    near(anchorAfter.height, anchorBefore.height);
    assert(!pane.following && pane.unseen == 0 && model.actions.empty());
    unchanged(buffer, before);

    blast::Message arrival;
    arrival.id = arrival.text = "WhileScrolledUp";
    arrival.nick = "Other";
    buffer.messages.push_back(arrival);
    assert(buffer.messages.size() == before.messages.size() + 1);
    before = buffer;
    view.sync();
    view.arrange(renderer, rect);
    assert(!pane.following && pane.unseen == 1 && model.actions.empty());
    near(scroll->scrollOffset(), target);
    near(bounds(body(view, "WheelTail35")).top, anchorAfter.top);
    unchanged(buffer, before);
    dispatcher.setSceneRoot(nullptr);
}

void check(TranscriptTestRenderer &renderer, float zoom, bool traditional) {
    blast::Model model;
    model.settings.scale = zoom;
    model.settings.messageScale = zoom;
    model.settings.timestamps = false;
    model.settings.avatars = true;
    model.settings.reducedMotion = true;
    model.settings.themeSource = "builtin";
    blast::Theme theme;
    theme.update(model);
    auto &buffer = model.buffers["geometry"];
    buffer.id = buffer.target = "geometry";
    buffer.server = "network";
    model.networks[buffer.server].nick = "Me";
    model.networks[buffer.server].state = "Ready";
    model.networks[buffer.server].caps = {"message-tags", "draft/message-redaction"};
    if (traditional)
        model.settings.messageLayouts[buffer.id] = blast::MessageLayout::Irc;
    buffer.unread = 7;
    for (const auto *token :
         {"SeedPresence", "OwnPresence", "MentionPresence", "OwnConversation"}) {
        blast::Message message;
        message.id = message.text = token;
        message.nick = "Me";
        message.own = message.text != "SeedPresence";
        message.presence = message.text != "OwnConversation";
        message.mention = message.text == "MentionPresence";
        buffer.messages.push_back(message);
    }
    buffer.readMarker = buffer.messages.back().id;
    const auto before = buffer;
    blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
    MenuHost host;
    Clipboard clipboard;
    blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
    view.setPopupContext(&host);
    view.setReadEligible(false);
    view.sync();
    view.arrange(renderer, {17, 23, 560, 560});
    const auto own = body(view, "OwnPresence");
    const auto mention = body(view, "MentionPresence");
    const auto conversation = body(view, "OwnConversation");
    presenceGeometry(renderer, own);
    presenceGeometry(renderer, mention);
    assert(own.row.parent() == mention.row.parent());
    near(mention.row.y() - own.row.y(), own.row.height());
    near(conversation.row.y() - mention.row.y(), mention.row.height());
    rail(own, {});
    rail(mention, {});
    rail(conversation, {});
    assert(!plainText(own.content, "Me") && !plainText(mention.content, "Me"));
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&view);
    dispatcher.setFocus(&own.row);
    dispatcher.keyEvent(XKB_KEY_Menu, 0, 0, true);
    dispatcher.keyEvent(XKB_KEY_Menu, 0, 0, false);
    assert(host.open && host.contains("Copy text") && !host.contains("Reply") &&
           !host.contains("React/👍") && !host.contains("Delete message"));
    host.select("Copy text");
    assert(clipboard.clipboardText() == buffer.messages[1].text);
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
    model.settings.timestamps = true;
    view.sync();
    view.arrange(renderer, {17, 23, 560, 560});
    presenceGeometry(renderer, body(view, "OwnPresence"));
    presenceGeometry(renderer, body(view, "MentionPresence"));
    model.settings.timestamps = false;
    unchanged(buffer, before);
    assert(model.actions.empty());
    if (zoom == 1.f)
        followAndReflow(renderer, theme, model, pane, view);
}

} // namespace

void bubbleNickBoundaries(TranscriptTestRenderer &renderer) {
    for (const float zoom : {1.f, 1.25f, 2.f}) {
        blast::Model model;
        model.settings.scale = zoom;
        model.settings.messageScale = zoom;
        model.settings.timestamps = true;
        model.settings.avatars = false;
        model.settings.reducedMotion = true;
        blast::Theme theme;
        theme.update(model);
        auto &buffer = model.buffers["nick-boundary"];
        buffer.id = buffer.target = "nick-boundary";
        buffer.kind = "query";
        blast::Message message;
        message.id = "nick-message";
        message.text = "Hi";
        message.time = "2026-11-07T12:34:56Z";
        buffer.messages.push_back(message);
        blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
        Clipboard clipboard;
        std::string command;
        blast::TranscriptView view(
            model, pane, clipboard, [&](auto value) { command = value; }, [](auto) {});
        for (const char *family : {"sans-serif", "serif", "monospace"}) {
            model.settings.uiFont = family;
            theme.update(model);
            for (const std::string nick :
                 {"Wi", "Alice", "i_l", "WWWWW", "長い名前оченьдлинноеимяLongNativeNickname"}) {
                buffer.messages.front().nick = nick;
                for (const float width : {800.f * zoom, 240.f}) {
                    for (const bool avatars : {false, true}) {
                        model.settings.avatars = avatars;
                        for (const bool own : {false, true})
                            for (const std::string sentence :
                                 {"Hi",
                                  "Hi\nA longer message body with enough native text to wrap "
                                  "across several lines in the narrow conversation layout."}) {
                                buffer.messages.front().own = own;
                                buffer.messages.front().text = sentence;
                                const auto before = buffer;
                                view.sync();
                                view.arrange(renderer, {17, 23, width, 1000 * zoom});
                                const auto rendered = body(view, "Hi");
                                auto *text = plainText(rendered.content, nick);
                                auto *label =
                                    text ? dynamic_cast<Label *>(text->parent()) : nullptr;
                                assert(label && label->focusable());
                                const auto natural = renderer.measureText(nick, text->fontSize(),
                                                                          text->font(), 0, 1);
                                const auto bounded = renderer.measureText(
                                    nick, text->fontSize(), text->font(), text->maxWidth(), 1);
                                assert(bounded.lineCount == 1);
                                assert(bounded.width <= text->maxWidth() + .02f);
                                const auto *surface = bubbleSurface(rendered);
                                float x, y, sx, sy;
                                Node::absolutePosition(text, x, y);
                                Node::absolutePosition(surface, sx, sy);
                                assert(x + bounded.inkLeft >= sx - .02f &&
                                       x + bounded.inkRight <= sx + surface->width() + .02f);
                                RectNode *chip = nullptr;
                                const auto control = sceneBounds(*label);
                                for (const auto &child : rendered.content.children())
                                    if (auto *rect = dynamic_cast<RectNode *>(child.get());
                                        rect && rect->visible() && rect->style().fill.a > 0 &&
                                        roundedCorners(rect->style().radius)) {
                                        const auto bounds = sceneBounds(*rect);
                                        if (bounds.x <= control.x + .02f &&
                                            bounds.y <= control.y + .02f &&
                                            bounds.x + bounds.width >=
                                                control.x + control.width - .02f &&
                                            bounds.y + bounds.height >= control.y - .02f) {
                                            assert(!chip);
                                            chip = rect;
                                        }
                                    }
                                assert(chip);
                                const auto chipBounds = sceneBounds(*chip);
                                const auto contentBounds = sceneBounds(rendered.body);
                                Image *avatar = nullptr;
                                for (const auto &child : rendered.content.children())
                                    if (auto *image = dynamic_cast<Image *>(child.get());
                                        image && image->visible()) {
                                        assert(!avatar && image->hasImage());
                                        avatar = image;
                                    }
                                assert(bool(avatar) == avatars);
                                if (own)
                                    near(chipBounds.x + chipBounds.width,
                                         contentBounds.x + contentBounds.width);
                                else if (!avatar || sceneBounds(*avatar).x < contentBounds.x - .02f)
                                    near(chipBounds.x, contentBounds.x);
                                if (avatar) {
                                    const auto bounds = sceneBounds(*avatar);
                                    assert(bounds.width > 0);
                                    near(bounds.width, bounds.height);
                                    containedBounds(bounds, sceneBounds(rendered.row));
                                    // Compact avatars share the author header; neither the whole
                                    // chip nor its native focusable control may occupy the avatar's
                                    // painted square.
                                    const auto disjoint = [&](LayoutRect other) {
                                        assert(bounds.x + bounds.width <= other.x + .02f ||
                                               other.x + other.width <= bounds.x + .02f ||
                                               bounds.y + bounds.height <= other.y + .02f ||
                                               other.y + other.height <= bounds.y + .02f);
                                    };
                                    disjoint(chipBounds);
                                    disjoint(control);
                                    if (bounds.x >= contentBounds.x - .02f &&
                                        bounds.x < contentBounds.x + contentBounds.width)
                                        near(bounds.x, contentBounds.x);
                                }
                                assert(text->fontSize() < rendered.text.fontSize());
                                const float horizontalPadding = control.x - chipBounds.x;
                                const bool compactAvatar =
                                    avatar && sceneBounds(*avatar).x >= contentBounds.x - .02f;
                                const float availableWidth =
                                    compactAvatar ? chipBounds.width - 2 * horizontalPadding
                                                  : contentBounds.width - 2 * horizontalPadding;
                                near(text->maxWidth(), chipBounds.width - 2 * horizontalPadding);
                                // Ellipsis is legitimate when the actual compact header cannot
                                // fit the native nickname, regardless of its byte length.
                                if (natural.width <= availableWidth + .02f)
                                    near(bounded.width, natural.width);
                                containedBounds(nativeTextInk(renderer, *text), chipBounds);
                                command.clear();
                                InputDispatcher dispatcher;
                                dispatcher.setSceneRoot(&view);
                                dispatcher.setFocus(label);
                                dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
                                dispatcher.keyEvent(XKB_KEY_Return, 0, 0, false);
                                assert(command == "nick:" + buffer.id + "\n" + nick);
                                dispatcher.setFocus(nullptr);
                                dispatcher.setSceneRoot(nullptr);
                                unchanged(buffer, before);
                            }
                    }
                }
            }
        }
    }
}

void visitSeparator(TranscriptTestRenderer &renderer) {
    blast::Model model;
    model.settings.reducedMotion = true;
    model.settings.avatars = false;
    blast::Theme theme;
    theme.update(model);
    auto &buffer = model.buffers["visit"];
    buffer.id = buffer.target = "visit";
    buffer.kind = "query";
    blast::Message prior;
    prior.id = "prior";
    prior.nick = "Peer";
    prior.text = "Prior";
    buffer.messages.push_back(prior);
    buffer.readMarker = prior.id;
    auto alreadyRead = prior;
    alreadyRead.id = "already-read";
    alreadyRead.text = "AlreadyRead";
    buffer.messages.push_back(alreadyRead);
    blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
    Clipboard clipboard;
    blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
    const auto arrange = [&] {
        view.sync();
        view.arrange(renderer, {17, 23, 700, 1000});
    };
    arrange();
    pane.following = false;
    // Window focus alone must not reinterpret a stale durable read marker as an arrival.
    view.setWindowActive(true);
    arrange();
    view.setWindowActive(false);
    arrange();
    assert(!plainText(view, "New since your last visit"));
    ++buffer.messagesRevision;
    arrange();
    assert(!plainText(view, "New since your last visit"));
    buffer.typing["TypingPeer"] = model.now;
    arrange();
    assert(!plainText(view, "New since your last visit") && pane.unseen == 0);
    buffer.typing.clear();
    blast::Message own;
    own.nick = "Me";
    own.id = "queued";
    own.text = "OwnSend";
    own.own = own.pending = true;
    own.sendStatus = blast::SendStatus::Queued;
    buffer.messages.push_back(own);
    arrange();
    assert(!plainText(view, "New since your last visit") && pane.unseen == 0);
    for (const auto status : {blast::SendStatus::Sent, blast::SendStatus::Confirmed}) {
        buffer.messages.back().pending = false;
        buffer.messages.back().sendStatus = status;
        arrange();
        assert(!plainText(view, "New since your last visit") && pane.unseen == 0);
    }
    blast::Message replay;
    replay.id = "history-replay";
    replay.nick = "Peer";
    replay.text = "HistoricalReplay";
    replay.history = true;
    buffer.messages.push_back(replay);
    buffer.unread = 2; // Core recount includes both the stale-marker tail and replay.
    arrange();
    assert(!plainText(view, "New since your last visit") && pane.unseen == 0);
    blast::Message presence;
    presence.id = "presence";
    presence.nick = "Peer";
    presence.text = "Peer joined";
    presence.presence = true;
    buffer.messages.push_back(presence);
    arrange();
    assert(!plainText(view, "New since your last visit") && pane.unseen == 0);
    blast::Message remote;
    remote.id = "remote";
    remote.nick = "Peer";
    remote.text = "RemoteArrival";
    buffer.messages.push_back(remote);
    buffer.unread = 3;
    arrange();
    assert(plainText(body(view, "RemoteArrival").row, "New since your last visit"));
    assert(!plainText(body(view, "OwnSend").row, "New since your last visit"));
    assert(pane.unseen == 1);
    view.setWindowActive(true);
    arrange();
    assert(!plainText(view, "New since your last visit"));
    assert(buffer.readMarker == prior.id && buffer.unread == 3 && pane.unseen == 1);
    view.setWindowActive(false);
    arrange();
    assert(plainText(body(view, "RemoteArrival").row, "New since your last visit"));
    assert(buffer.readMarker == prior.id && buffer.unread == 3 && pane.unseen == 1);
    assert(model.actions.empty());
    buffer.unread = 0;
    arrange();
    assert(!plainText(view, "New since your last visit"));
    buffer.readMarker = "local:" + remote.id;
    arrange();
    view.setWindowActive(true);
    arrange();
    view.setWindowActive(false);
    arrange();
    assert(!plainText(view, "New since your last visit"));

    // A fresh view must retain genuine persisted unread even though hydration marks it history.
    buffer.readMarker = "local:" + replay.id;
    buffer.unread = 1;
    buffer.messages.back().history = true;
    blast::TranscriptView restored(model, pane, clipboard, [](auto) {}, [](auto) {});
    restored.sync();
    restored.arrange(renderer, {17, 23, 700, 1000});
    assert(plainText(body(restored, "RemoteArrival").row, "New since your last visit"));
    model.settings.foolDisplay = blast::FoolDisplay::Dimmed;
    model.settings.fools.insert("Peer");
    buffer.unread = 0;
    restored.sync();
    restored.arrange(renderer, {17, 23, 700, 1000});
    assert(!plainText(restored, "New since your last visit"));
    auto other = remote;
    other.id = "other-unread";
    other.nick = "OtherPeer";
    other.text = "OtherUnread";
    buffer.messages.push_back(other);
    buffer.unread = 1;
    restored.sync();
    restored.arrange(renderer, {17, 23, 700, 1000});
    assert(plainText(body(restored, "OtherUnread").row, "New since your last visit"));
    assert(!plainText(body(restored, "RemoteArrival").row, "New since your last visit"));
    model.settings.foolUnread = true;
    buffer.unread = 2;
    restored.sync();
    restored.arrange(renderer, {17, 23, 700, 1000});
    assert(plainText(body(restored, "RemoteArrival").row, "New since your last visit"));
    assert(!plainText(body(restored, "OtherUnread").row, "New since your last visit"));
    buffer.unread = 0;
    restored.sync();
    restored.arrange(renderer, {17, 23, 700, 1000});
    assert(!plainText(restored, "New since your last visit"));

    // Replay already received before first opening a conversation is not startup restoration.
    auto &unvisited = model.buffers["unvisited"];
    unvisited.id = unvisited.target = "unvisited";
    unvisited.messages = {prior, replay};
    unvisited.messages.back().arrival = 10;
    unvisited.readMarker = prior.id;
    unvisited.unread = 1;
    blast::PaneState unopenedPane{.id = 2, .buffer = unvisited.id, .reply = {}};
    blast::TranscriptView unopened(model, unopenedPane, clipboard, [](auto) {}, [](auto) {});
    unopened.sync();
    unopened.arrange(renderer, {17, 23, 700, 1000});
    assert(!plainText(unopened, "New since your last visit"));

    auto &empty = model.buffers["empty-visit"];
    empty.id = empty.target = "empty-visit";
    blast::PaneState emptyPane{.id = 3, .buffer = empty.id, .reply = {}};
    blast::TranscriptView emptyView(model, emptyPane, clipboard, [](auto) {}, [](auto) {});
    emptyView.setWindowActive(true);
    emptyView.sync();
    emptyView.arrange(renderer, {17, 23, 700, 1000});
    emptyView.setWindowActive(false);
    emptyView.sync();
    emptyView.arrange(renderer, {17, 23, 700, 1000});
    assert(!plainText(emptyView, "New since your last visit"));
    empty.messages.push_back(remote);
    empty.unread = 1;
    emptyView.sync();
    emptyView.arrange(renderer, {17, 23, 700, 1000});
    assert(plainText(body(emptyView, "RemoteArrival").row, "New since your last visit"));
    empty.readMarker = remote.id;
    emptyView.sync();
    emptyView.arrange(renderer, {17, 23, 700, 1000});
    assert(!plainText(emptyView, "New since your last visit"));
    model.settings.foolUnread = false;
    empty.unread = 0;
    emptyView.sync();
    emptyView.arrange(renderer, {17, 23, 700, 1000});
    auto newlyMuted = remote;
    newlyMuted.id = "newly-muted";
    newlyMuted.text = "NewlyMutedUnread";
    empty.messages.push_back(newlyMuted);
    emptyView.sync();
    emptyView.arrange(renderer, {17, 23, 700, 1000});
    assert(!plainText(emptyView, "New since your last visit"));
    model.settings.foolUnread = true;
    empty.unread = 1;
    emptyView.sync();
    emptyView.arrange(renderer, {17, 23, 700, 1000});
    assert(plainText(body(emptyView, "NewlyMutedUnread").row, "New since your last visit"));
}

void messageEntranceLifecycle(TranscriptTestRenderer &renderer) {
    for (const bool own : {false, true}) {
        blast::Model model;
        model.now = 10;
        model.settings.avatars = false;
        model.settings.reducedMotion = false;
        model.settings.fx = blast::FxLevel::Full;
        model.settings.incomingMotion = blast::SendMotion::Slide;
        blast::Theme theme;
        theme.update(model);
        auto &buffer = model.buffers["entrance"];
        buffer.id = buffer.target = "entrance";
        buffer.kind = "query";
        blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
        Clipboard clipboard;
        MenuHost host;
        Node motionHost;
        motionHost.setSize(800, 700);
        blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
        view.setPopupContext(&host);
        const LayoutRect viewport{17, 23, 700, 300};
        view.setComposerOrigin(367, 340);
        view.setMotionHost(&motionHost);
        const auto arrange = [&] {
            view.sync();
            view.arrange(renderer, viewport);
        };
        arrange(); // An initialized empty conversation is a valid arrival baseline.
        blast::Message message;
        message.id = "local-entrance";
        message.label = "stable-send-label";
        message.nick = own ? "Me" : "Peer";
        message.text = "LiveEntrance";
        message.own = message.pending = own;
        message.arrival = model.now;
        buffer.messages.push_back(message);
        arrange();
        auto rendered = body(view, message.text);
        assert(rendered.content.opacity() == 0);
        auto *date = plainText(view, "Undated history");
        assert(date);
        float dateX, dateY;
        Node::absolutePosition(date, dateX, dateY);
        if (own) {
            float x, y;
            const auto *surface = bubbleSurface(rendered);
            Node::mapToScene(surface, surface->width() * .5f, surface->height() * .5f, x, y);
            near(x, 367);
            near(y, 340);
        } else {
            assert(rendered.content.x() > 0);
            near(rendered.content.y(), 0);
        }
        model.now += .3;
        arrange();
        date = plainText(view, "Undated history");
        float stillX, stillY;
        Node::absolutePosition(date, stillX, stillY);
        near(stillX, dateX);
        near(stillY, dateY);
        const float inFlightDistance =
            std::hypot(body(view, message.text).content.x(), body(view, message.text).content.y());
        if (!own) {
            const auto moving = body(view, message.text);
            auto *surface = bubbleSurface(moving);
            assert(surface && moving.content.x() > 0 &&
                   surface->x() + surface->width() + moving.content.x() < moving.row.width());
            InputDispatcher dispatcher;
            dispatcher.setSceneRoot(&view);
            const auto rightClick = [&](float localX) {
                float x, y;
                Node::mapToScene(&moving.row, localX, surface->y() + surface->height() * .5f, x, y);
                assert(dispatcher.pointerButton(x, y, BTN_RIGHT, true, 31, 310, false));
                dispatcher.pointerButton(x, y, BTN_RIGHT, false, 32, 320, false);
            };
            rightClick(surface->x() + moving.content.x() * .5f);
            assert(!host.open);
            rightClick(surface->x() + surface->width() + moving.content.x() * .5f);
            assert(host.open && host.contains("Copy text"));
            host.select("Copy text");
            assert(clipboard.clipboardText() == message.text);
            dispatcher.setSceneRoot(nullptr);
        }
        if (own) {
            buffer.messages.back().id = "server-echo";
            buffer.messages.back().pending = false;
            buffer.messages.back().sendStatus = blast::SendStatus::Confirmed;
            ++buffer.messagesRevision;
            model.now += .1;
            arrange();
            const auto echo = body(view, message.text);
            assert(std::hypot(echo.content.x(), echo.content.y()) < inFlightDistance);
        }
        model.now = 11;
        arrange();
        const auto settled = body(view, message.text);
        near(settled.content.x(), 0);
        near(settled.content.y(), 0);
        near(settled.content.opacity(), 1);
        // Settled rows never replay when settings change.
        model.settings.reducedMotion = true;
        arrange();
        model.settings.reducedMotion = false;
        arrange();
        near(body(view, message.text).content.opacity(), 1);
        for (const bool reduced : {false, true}) {
            model.now += 1;
            message.id = message.label = reduced ? "reduced-arrival" : "off-arrival";
            message.text = reduced ? "ReducedEntrance" : "OffEntrance";
            message.arrival = model.now;
            buffer.messages.push_back(message);
            arrange();
            assert(body(view, message.text).content.opacity() == 0);
            if (reduced)
                model.settings.reducedMotion = true;
            else
                model.settings.fx = blast::FxLevel::Off;
            arrange();
            const auto cancelled = body(view, message.text);
            near(cancelled.content.x(), 0);
            near(cancelled.content.y(), 0);
            near(cancelled.content.opacity(), 1);
            model.settings.reducedMotion = false;
            model.settings.fx = blast::FxLevel::Full;
            arrange();
            near(body(view, message.text).content.opacity(), 1);
        }
        assert(model.actions.empty());
        view.setMotionHost(nullptr);
    }
}

void independentSendEffects(TranscriptTestRenderer &renderer) {
    for (const bool pixels : {false, true}) {
        blast::Model model;
        model.now = 20;
        model.settings.avatars = false;
        model.settings.fx = blast::FxLevel::Full;
        model.settings.reducedMotion = false;
        model.settings.sendMotion = pixels ? blast::SendMotion::Pop : blast::SendMotion::Flight;
        model.settings.sendReveal = pixels ? blast::SendReveal::Pixels : blast::SendReveal::Fade;
        blast::Theme theme;
        theme.update(model);
        auto &buffer = model.buffers["independent-send"];
        buffer.id = buffer.target = "independent-send";
        buffer.kind = "query";
        blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
        Clipboard clipboard;
        Node motionHost;
        motionHost.setSize(800, 700);
        blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
        view.setMotionHost(&motionHost);
        view.setComposerOrigin(367, 340);
        const auto arrange = [&] {
            view.sync();
            view.arrange(renderer, {17, 23, 700, 300});
        };
        arrange();
        blast::Message message;
        message.id = message.label = "independent-local";
        message.nick = "Me";
        message.text = "IndependentSend";
        message.own = message.pending = true;
        message.arrival = model.now;
        buffer.messages.push_back(message);
        arrange();
        model.now += pixels ? .4 : .5;
        arrange();
        const auto rendered = body(view, message.text);
        assert(motionHost.children().size() == 1);
        auto &group = *motionHost.children().front();
        auto *reveal = find<blast::PixelRevealNode>(group);
        assert(reveal);
        Node *proxy = nullptr;
        for (const auto &child : group.children())
            if (child.get() != reveal)
                proxy = child.get();
        assert(proxy);
        if (pixels) {
            near(rendered.content.x(), 0);
            near(rendered.content.y(), 0);
            near(group.scaleX(), 1);
            near(group.scaleY(), 1);
            assert(!reveal->children().empty());
        } else {
            assert(std::hypot(rendered.content.x(), rendered.content.y()) > .02f);
            near(proxy->opacity(), 1);
            assert(reveal->children().empty());
        }
        model.now += 2;
        arrange();
        assert(motionHost.children().empty());
        const auto restored = body(view, message.text);
        near(restored.content.x(), 0);
        near(restored.content.y(), 0);
        near(restored.content.opacity(), 1);
        assert(model.actions.empty());
        view.setMotionHost(nullptr);
    }
}

void cappedTranscriptBodies(TranscriptTestRenderer &renderer) {
    for (const bool traditional : {false, true}) {
        blast::Model model;
        model.settings.timestamps = model.settings.avatars = false;
        model.settings.reducedMotion = true;
        blast::Theme theme;
        theme.update(model);
        auto &buffer = model.buffers["grapheme-cap"];
        buffer.id = buffer.target = "grapheme-cap";
        buffer.kind = "channel";
        if (traditional)
            model.settings.messageLayouts[buffer.id] = blast::MessageLayout::Irc;
        const std::string ascii =
            std::string(40, 'a') + "\x02" + std::string(40, 'b') + "\x02" + "c";
        // The 80th grapheme crosses an IRC style boundary; the emoji is the 81st.
        const std::string unicode = std::string(79, 'x') + "e\x02\u0301\x02👩‍💻";
        const std::string legal = std::string(75, 'z') + " boundary";
        const std::string combiningSpace = std::string(80, 'x') + " \u0301tail";
        const std::string explicitLines = "Original \x02"
                                          "decoded\x02 e\u0301 👩‍💻\n\nlast & <line>\n";
        // IRC decoding strips CR controls: CRLF retains LF, lone CR joins text.
        const std::string carriageReturns = "CRLFStart\r\nCRLFMiddle\rCRLFEnd\n";
        const std::vector<std::string> source{ascii,         unicode,        legal,
                                              explicitLines, combiningSpace, carriageReturns};
        for (std::size_t i = 0; i < source.size(); ++i) {
            blast::Message message;
            message.id = "cap-" + std::to_string(i);
            message.nick = "Peer";
            message.text = source[i];
            buffer.messages.push_back(std::move(message));
        }
        const auto before = buffer;
        blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
        Clipboard clipboard;
        MenuHost host;
        blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
        view.setReadEligible(false);
        view.setPopupContext(&host);
        InputDispatcher dispatcher;
        dispatcher.setSceneRoot(&view);
        view.setInputDispatcher(dispatcher);
        const auto arrange = [&] {
            view.sync();
            view.arrange(renderer, {17, 23, 6000, 1600});
        };
        // Reassemble actual shaped fragments by line, not by style run.
        const auto lines = [&](std::string_view token) {
            std::vector<std::pair<float, std::string>> result;
            for (const auto &child : body(view, token).body.children())
                if (auto *area = dynamic_cast<InputArea *>(child.get()); area && area->visible())
                    for (const auto &node : area->children())
                        if (auto *text = dynamic_cast<TextNode *>(node.get());
                            text && text->visible() && text->useMarkup()) {
                            const float center = area->y() + area->height() * .5f;
                            if (result.empty() || std::abs(result.back().first - center) > .02f)
                                result.emplace_back(center, "");
                            result.back().second += decodedMarkup(text->text());
                        }
            std::vector<std::string> resultText;
            for (const auto &[y, text] : result)
                resultText.push_back(text);
            return resultText;
        };
        arrange();
        assert(model.settings.messageWrapColumns == 80);
        assert((lines(std::string(40, 'a')) ==
                std::vector<std::string>{std::string(40, 'a') + std::string(40, 'b'), "c"}));
        assert((lines(std::string(79, 'x')) ==
                std::vector<std::string>{std::string(79, 'x') + "e\u0301", "👩‍💻"}));
        assert((lines(std::string(75, 'z')) ==
                std::vector<std::string>{std::string(75, 'z'), "boundary"}));
        // The leading space and its combining accent are one native grapheme.
        // Omitting ASCII soft-wrap gaps must not consume half of that grapheme.
        assert((lines(std::string(80, 'x')) ==
                std::vector<std::string>{std::string(80, 'x'), " \u0301tail"}));
        assert((lines("CRLFStart") == std::vector<std::string>{"CRLFStart", "CRLFMiddleCRLFEnd"}));
        const std::vector<std::string> decoded{
            std::string(40, 'a') + std::string(40, 'b') + "c",
            std::string(79, 'x') + "e\u0301👩‍💻",
            legal,
            "Original decoded e\u0301 👩‍💻\n\nlast & <line>\n",
            combiningSpace,
            "CRLFStart\nCRLFMiddleCRLFEnd\n"};
        const std::vector<std::string> tokens{std::string(40, 'a'), std::string(79, 'x') + "e",
                                              std::string(75, 'z'), "Original",
                                              std::string(80, 'x'), "CRLFStart"};
        for (std::size_t i = 0; i < decoded.size(); ++i) {
            const auto rendered = body(view, tokens[i]);
            dispatcher.setFocus(&rendered.row);
            dispatcher.keyEvent(XKB_KEY_Menu, 0, 0, true);
            dispatcher.keyEvent(XKB_KEY_Menu, 0, 0, false);
            assert(host.open && host.contains("Copy text"));
            host.select("Copy text");
            assert(clipboard.clipboardText() == decoded[i]);
        }
        // Compare body geometry: outer rows also contain author/day/group decorations.
        // Explicit blank/trailing lines consume body height without becoming model edits.
        assert(body(view, "Original").body.height() >
               body(view, std::string(40, 'a')).body.height());
        const auto explicitBody = body(view, "Original");
        const auto strut = renderer.measureText("Mg", explicitBody.text.fontSize(),
                                                explicitBody.text.font(), 0, 1);
        const float minimumLine = std::max(1.f, strut.bottom - strut.top);
        float lastTop = 0, lastBottom = 0;
        bool foundText = false;
        for (const auto &child : explicitBody.body.children())
            if (const auto *area = dynamic_cast<const InputArea *>(child.get());
                area && area->visible())
                for (const auto &node : area->children())
                    if (const auto *text = dynamic_cast<const TextNode *>(node.get());
                        text && text->visible() && text->useMarkup() &&
                        !decodedMarkup(text->text()).empty()) {
                        foundText = true;
                        lastTop = std::max(lastTop, area->y());
                        lastBottom = std::max(lastBottom, area->y() + area->height());
                    }
        assert(foundText && lastTop >= 2 * minimumLine - .02f);
        assert(explicitBody.body.height() - lastBottom >= minimumLine - .02f);
        model.settings.messageWrapColumns = 0;
        arrange();
        assert(lines(std::string(40, 'a')).size() == 1);
        assert(lines(std::string(79, 'x')).size() == 1);
        assert((lines(std::string(75, 'z')) == std::vector<std::string>{legal}));
        assert((lines(std::string(80, 'x')) == std::vector<std::string>{combiningSpace}));
        assert((lines("CRLFStart") == std::vector<std::string>{"CRLFStart", "CRLFMiddleCRLFEnd"}));
        unchanged(buffer, before);
        dispatcher.setFocus(nullptr);
        dispatcher.setSceneRoot(nullptr);
    }
}

void typingFooterGeometry(TranscriptTestRenderer &renderer) {
    // Typing names may carry the inline separator; retained hidden nodes are not visible text.
    const auto plainText = [](Node &root, std::string_view value) -> TextNode * {
        const auto visit = [&](auto &&self, Node &node) -> TextNode * {
            if (!node.visible())
                return nullptr;
            if (auto *text = dynamic_cast<TextNode *>(&node);
                text && (text->text() == value || text->text() == std::string(value) + ","))
                return text;
            for (const auto &child : node.children())
                if (auto *text = self(self, *child))
                    return text;
            return nullptr;
        };
        for (auto *node = root.parent(); node; node = node->parent())
            if (!node->visible())
                return nullptr;
        return visit(visit, root);
    };
    for (const bool traditional : {false, true})
        for (const float zoom : {1.f, 2.f}) {
            blast::Model model;
            model.now = 100;
            model.settings.scale = zoom;
            model.settings.messageScale = zoom;
            model.settings.uiFont = "sans-serif";
            model.settings.messageFont = "serif";
            model.settings.uiFontWeight = 700;
            model.settings.messageFontWeight = 300;
            model.settings.timestamps = model.settings.avatars = false;
            model.settings.reducedMotion = true;
            model.settings.typingReceive = true;
            model.settings.nickColors["TypingPeer"] = "#70c0ff";
            model.settings.nickColors["OtherTypingPeer"] = "#e6b86a";
            blast::Theme theme;
            theme.update(model);
            auto &buffer = model.buffers["typing-footer"];
            buffer.id = buffer.target = "typing-footer";
            buffer.kind = "query";
            if (traditional)
                model.settings.messageLayouts[buffer.id] = blast::MessageLayout::Irc;
            for (int i = 0; i < 32; ++i) {
                blast::Message message;
                message.id = message.text = "FooterRow-" + std::to_string(i);
                message.nick = "Peer";
                buffer.messages.push_back(std::move(message));
            }
            blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
            Clipboard clipboard;
            blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
            view.setReadEligible(false);
            const float messageSize = blast::messageTextSize(model.settings, buffer);
            const float typingHeight = blast::typingIndicatorHeight(renderer, zoom, messageSize),
                        replyHeight = 64 * zoom;
            near(typingHeight, std::max(32 * zoom, renderer.fontRowExtent(
                                                       messageSize, blast::themeMessageFont()) +
                                                       7 * zoom) +
                                   8 * zoom);
            const float obstruction = typingHeight + 4 * zoom + replyHeight;
            view.setContextInset(typingHeight, typingHeight, 0);
            const LayoutRect viewport{17, 23, 800 * zoom, 400 * zoom};
            const auto arrange = [&] {
                view.sync();
                view.arrange(renderer, viewport);
            };
            const auto typingRow = [&](const std::vector<std::string> &participants) {
                auto *phrase =
                    plainText(view, participants.size() == 1 ? "is typing" : "are typing");
                assert(phrase && dynamic_cast<Label *>(phrase->parent()));
                assert(!plainText(view, participants.size() == 1 ? "are typing" : "is typing"));
                assert(phrase->font() == blast::themeMessageFont());
                near(phrase->fontSize(), messageSize);
                assert(phrase->color() == blast::themeForegroundColor("on_surface_variant"));
                auto *footer = phrase->parent()->parent();
                const auto phraseBounds = sceneBounds(*phrase->parent());
                const auto footerBounds = sceneBounds(*footer);
                containedBounds(phraseBounds, footerBounds);
                containedBounds(phraseBounds, ancestorPaintClip(*phrase));
                size_t shown = 0;
                for (const auto &nick : participants)
                    if (auto *name = plainText(*footer, nick)) {
                        ++shown;
                        near(baseline(*name), baseline(*phrase));
                        const auto nameBounds = sceneBounds(*name->parent());
                        assert(nameBounds.x + nameBounds.width + 4.9f * zoom <= phraseBounds.x);
                        containedBounds(nameBounds, footerBounds);
                    }
                if (shown < participants.size()) {
                    auto *more = plainText(*footer->parent(),
                                           "+" + std::to_string(participants.size() - shown));
                    auto *moreLabel = dynamic_cast<Label *>(more ? more->parent() : nullptr);
                    auto *moreButton =
                        moreLabel ? dynamic_cast<Button *>(moreLabel->parent()) : nullptr;
                    assert(moreLabel && moreLabel->visible());
                    assert(moreButton && moreButton->visible() && moreButton->enabled());
                    const auto moreBounds = sceneBounds(*moreLabel);
                    assert(moreBounds.x + moreBounds.width <= phraseBounds.x + .02f);
                    containedBounds(moreBounds, footerBounds);
                    containedBounds(nativeTextInk(renderer, *more), footerBounds);
                    containedBounds(sceneBounds(*moreButton), footerBounds);
                }
                float previousRight = phraseBounds.x + phraseBounds.width;
                size_t dots = 0;
                for (const auto &child : footer->children())
                    if (auto *dot = dynamic_cast<RectNode *>(child.get());
                        dot && dot->visible() && dot->width() <= 3 * zoom &&
                        dot->height() <= 3 * zoom) {
                        const auto dotBounds = sceneBounds(*dot);
                        assert(dotBounds.x >= previousRight + 3.9f * zoom);
                        assert(dotBounds.y >= phraseBounds.y - .02f &&
                               dotBounds.y + dotBounds.height <=
                                   phraseBounds.y + phraseBounds.height + .02f);
                        containedBounds(dotBounds, footerBounds);
                        containedBounds(dotBounds, ancestorPaintClip(*dot));
                        if (model.settings.reducedMotion ||
                            model.settings.fx == blast::FxLevel::Off)
                            near(dot->opacity(), .65f);
                        else
                            near(dot->opacity(),
                                 .35f + .65f * float((1 + std::sin(model.now * 2.1 - dots * .9)) *
                                                     .5));
                        previousRight = dotBounds.x + dotBounds.width;
                        ++dots;
                    }
                assert(dots == 3);
                near(footer->height(), typingHeight);
            };
            arrange();
            auto *scroll = find<ScrollView>(view);
            assert(scroll && scroll->maxScrollOffset() > 0);
            near(scroll->height(), viewport.height);
            near(scroll->contentViewportHeight(), viewport.height);
            near(nativeHistoryClip(view).height, viewport.height);
            const auto tail = bounds(body(view, "FooterRow-31"));
            assert(tail.top + tail.height <= viewport.y + viewport.height - typingHeight + .5f);
            for (const bool following : {true, false}) {
                scroll->setScrollOffset(following ? scroll->maxScrollOffset()
                                                  : scroll->maxScrollOffset() - 80 * zoom);
                arrange();
                const float offset = scroll->scrollOffset(), maximum = scroll->maxScrollOffset();
                const auto baseline = bounds(body(view, "FooterRow-30"));
                const float rowHeight = body(view, "FooterRow-30").row.height();
                const auto latest = bounds(body(view, "FooterRow-31"));
                for (const bool replying : {true, false, true, false}) {
                    view.setContextInset(typingHeight, typingHeight, replying ? obstruction : 0);
                    arrange();
                    near(scroll->scrollOffset(), offset);
                    near(scroll->maxScrollOffset(), maximum);
                    near(scroll->height(), viewport.height);
                    near(scroll->contentViewportHeight(), viewport.height);
                    near(nativeHistoryClip(view).height, viewport.height);
                    const auto current = bounds(body(view, "FooterRow-31"));
                    near(current.top, latest.top);
                    near(current.height, latest.height);
                    assert(pane.following == following);
                    if (replying && following) {
                        const float cardTop = viewport.y + viewport.height - obstruction;
                        const float cardBottom = cardTop + replyHeight;
                        assert(current.top < cardBottom && current.top + current.height > cardTop);
                    }
                }
                int typingCycle = 0;
                for (const int phase : {0, 1, 2, 1, 2, 1, 2, 0}) {
                    if (phase == 0)
                        buffer.typing.clear();
                    if (phase == 1) {
                        buffer.typing.clear();
                        buffer.typing["TypingPeer"] = model.now;
                        if (++typingCycle % 2 == 0)
                            buffer.typing["OtherTypingPeer"] = model.now;
                    }
                    // Expiry retains the raw typing entry; a new participant set
                    // rebuilds the native footer again on the next active phase.
                    model.now += phase == 2 ? 12.25 : .25;
                    arrange();
                    near(scroll->height(), viewport.height);
                    near(scroll->contentViewportHeight(), viewport.height);
                    near(nativeHistoryClip(view).height, viewport.height);
                    near(scroll->width(), viewport.width);
                    near(view.x(), viewport.x);
                    near(view.y(), viewport.y);
                    near(view.width(), viewport.width);
                    near(view.height(), viewport.height);
                    near(scroll->scrollOffset(), offset);
                    near(scroll->maxScrollOffset(), maximum);
                    const auto current = bounds(body(view, "FooterRow-30"));
                    near(current.top, baseline.top);
                    near(current.height, baseline.height);
                    near(body(view, "FooterRow-30").row.height(), rowHeight);
                    assert(pane.following == following);
                    if (phase == 1) {
                        for (const auto &[nick, at] : buffer.typing) {
                            auto *name = plainText(view, nick);
                            auto *label = name ? dynamic_cast<Label *>(name->parent()) : nullptr;
                            assert(name && name->visible() && label && label->visible());
                            assert(label->width() > 0 && label->height() > 0);
                            const auto glyphs = renderer.measureText(
                                name->text(), name->fontSize(), name->font(), name->maxWidth(),
                                name->maxLines(), name->textAlign(), name->ellipsize(),
                                name->useMarkup());
                            assert(glyphs.width > 0 && glyphs.inkRight > glyphs.inkLeft &&
                                   glyphs.inkBottom > glyphs.inkTop);
                            float x, y;
                            Node::absolutePosition(name, x, y);
                            const LayoutRect ink{x + glyphs.inkLeft, y + glyphs.inkTop,
                                                 glyphs.inkRight - glyphs.inkLeft,
                                                 glyphs.inkBottom - glyphs.inkTop};
                            const Color nickColor =
                                nick == "TypingPeer"
                                    ? Color{112.f / 255, 192.f / 255, 1, 1}
                                    : Color{230.f / 255, 184.f / 255, 106.f / 255, 1};
                            auto *footer = label->parent();
                            assert(footer && footer->visible());
                            assert(name->color() == nickColor);
                            near(name->fontSize(), messageSize);
                            assert(name->font() == blast::themeMessageFont());
                            // Names, neutral phrase and dots share the fixed reserved tray.
                            containedBounds(ink, sceneBounds(*footer));
                            containedBounds(ink, ancestorPaintClip(*name));
                            containedBounds(ink, {viewport.x,
                                                  viewport.y + viewport.height - typingHeight,
                                                  viewport.width, typingHeight});
                        }
                        std::vector<std::string> participants;
                        for (const auto &[nick, at] : buffer.typing)
                            participants.push_back(nick);
                        typingRow(participants);
                    }
                    if (phase != 1)
                        assert(!plainText(view, "TypingPeer") &&
                               !plainText(view, "OtherTypingPeer") &&
                               !plainText(view, "is typing") && !plainText(view, "are typing"));
                }
            }
            InputDispatcher dispatcher;
            dispatcher.setSceneRoot(&view);
            view.setInputDispatcher(dispatcher);
            view.setContextInset(typingHeight, typingHeight, obstruction);
            assert(view.jumpToSourceMessage("FooterRow-26"));
            arrange();
            const auto jumped = bounds(body(view, "FooterRow-26"));
            assert(jumped.top >= viewport.y - .5f &&
                   jumped.top + jumped.height <= viewport.y + viewport.height - obstruction + .5f);
            dispatcher.setFocus(&body(view, "FooterRow-26").row);
            assert(view.navigateMessage(1));
            arrange();
            const auto navigated = bounds(body(view, "FooterRow-27"));
            assert(navigated.top >= viewport.y - .5f &&
                   navigated.top + navigated.height <=
                       viewport.y + viewport.height - obstruction + .5f);
            dispatcher.setFocus(nullptr);
            view.setContextInset(typingHeight, typingHeight, 0);
            arrange();
            MenuHost host;
            view.setPopupContext(&host);
            const auto overflow = [&](auto &&self, Node &node) -> InputArea * {
                if (auto *area = dynamic_cast<InputArea *>(&node);
                    area && area->tabFocusKey() == "transcript:1:typing-overflow")
                    return area;
                for (const auto &child : node.children())
                    if (auto *area = self(self, *child))
                        return area;
                return nullptr;
            };
            const auto clickOverflow = [&] {
                auto *area = overflow(overflow, view);
                assert(area && area->visible());
                assert(dynamic_cast<Button *>(area->parent()));
                float x, y;
                Node::mapToScene(area, area->width() / 2, area->height() / 2, x, y);
                assert(dispatcher.pointerButton(x, y, BTN_LEFT, true, 190, 1900, false));
                dispatcher.pointerButton(x, y, BTN_LEFT, false, 191, 1910, false);
                arrange();
                assert(!host.open && host.request.menu.empty() && host.request.options.empty());
            };
            const std::vector<std::string> eligible{"Alice", "Bob", "Carol", "Dave", "Eve"};
            buffer.typing.clear();
            for (const auto &nick : eligible)
                buffer.typing[nick] = model.now;
            buffer.typing["Expired"] = model.now - 13;
            buffer.typing["Muted"] = model.now;
            model.settings.fools.insert("Muted");
            model.settings.avatars = true;
            model.settings.avatarStyle = "monogram";
            arrange();
            const auto manyOffset = scroll->scrollOffset(), manyMaximum = scroll->maxScrollOffset();
            assert(plainText(view, "Alice") && plainText(view, "Bob") && plainText(view, "+3"));
            assert(!plainText(view, "Carol") && !plainText(view, "Expired") &&
                   !plainText(view, "Muted"));
            auto *inlineName = plainText(view, "Alice");
            assert(inlineName && inlineName->parent() && inlineName->parent()->parent());
            auto *tray = inlineName->parent()->parent();
            const auto avatarCount = [&] {
                size_t count = 0;
                for (const auto &child : tray->children())
                    if (auto *rect = dynamic_cast<RectNode *>(child.get());
                        rect && rect->visible() && std::abs(rect->width() - 32 * zoom) < .5f &&
                        std::abs(rect->height() - 32 * zoom) < .5f)
                        ++count;
                return count;
            };
            assert(avatarCount() == 3);
            typingRow(eligible);
            size_t surfaces = 0;
            for (const auto &child : tray->children())
                if (auto *rect = dynamic_cast<RectNode *>(child.get());
                    rect && rect->visible() && rect->style().fill.a > 0 &&
                    rect->width() > 32 * zoom + 1 && rect->height() > 7 * zoom)
                    ++surfaces;
            assert(surfaces == (traditional ? 0u : 1u));
            model.settings.avatars = false;
            arrange();
            assert(avatarCount() == 0 && plainText(view, "Alice") && plainText(view, "Bob"));
            typingRow(eligible);
            near(scroll->scrollOffset(), manyOffset);
            near(scroll->maxScrollOffset(), manyMaximum);
            for (const bool avatars : {false, true})
                for (const float width : {160.f, 200.f, 240.f}) {
                    model.settings.avatars = avatars;
                    arrange();
                    view.arrange(renderer, {viewport.x, viewport.y, width * zoom, viewport.height});
                    size_t narrowNames = 0;
                    for (const auto &nick : eligible)
                        narrowNames += plainText(view, nick) != nullptr;
                    assert(narrowNames <= 2);
                    assert(plainText(view, "+" + std::to_string(eligible.size() - narrowNames)));
                    typingRow(eligible);
                    near(view.height(), viewport.height);
                    near(tray->height(), typingHeight);
                }
            model.settings.avatars = true;
            arrange();
            const auto savedTyping = buffer.typing;
            for (const auto &participants :
                 {std::vector<std::string>{"Alice"}, std::vector<std::string>{"Alice", "Bob"}}) {
                buffer.typing.clear();
                for (const auto &nick : participants)
                    buffer.typing[nick] = model.now;
                for (const bool avatars : {false, true})
                    for (const float width : {160.f, 240.f, 800.f}) {
                        model.settings.avatars = avatars;
                        arrange();
                        view.arrange(renderer,
                                     {viewport.x, viewport.y, width * zoom, viewport.height});
                        typingRow(participants);
                    }
            }
            buffer.typing = savedTyping;
            model.settings.avatars = true;
            arrange();
            const float motionOffset = scroll->scrollOffset(),
                        motionMaximum = scroll->maxScrollOffset();
            const auto motionBody = bounds(body(view, "FooterRow-30"));
            for (const bool reduced : {false, true})
                for (const auto fx : {blast::FxLevel::Full, blast::FxLevel::Off}) {
                    model.settings.reducedMotion = reduced;
                    model.settings.fx = fx;
                    for (int frame = 0; frame < 2; ++frame) {
                        model.now += .25;
                        arrange();
                        typingRow(eligible);
                        near(scroll->scrollOffset(), motionOffset);
                        near(scroll->maxScrollOffset(), motionMaximum);
                        near(bounds(body(view, "FooterRow-30")).top, motionBody.top);
                        near(scroll->height(), viewport.height);
                    }
                }
            model.settings.reducedMotion = true;
            model.settings.fx = blast::FxLevel::Full;
            // Width reflow changes offscreen height estimates and absolute scroll.
            // Opening the list must preserve the restored viewport, not undo reflow.
            const float popupOffset = scroll->scrollOffset(),
                        popupMaximum = scroll->maxScrollOffset();
            const auto popupBody = bounds(body(view, "FooterRow-30"));
            const auto countNodes = [&](auto &&self, const Node &node) -> size_t {
                size_t count = 1;
                for (const auto &child : node.children())
                    count += self(self, *child);
                return count;
            };
            const auto boundedNodes = countNodes(countNodes, view);
            clickOverflow();
            for (const auto &nick : eligible) {
                auto *text = plainText(view, nick);
                assert(text && dynamic_cast<Label *>(text->parent()));
            }
            auto *last = plainText(view, "Eve");
            ScrollView *list = nullptr;
            for (auto *node = last->parent(); node; node = node->parent())
                if (auto *candidate = dynamic_cast<ScrollView *>(node)) {
                    list = candidate;
                    break;
                }
            assert(list && list != scroll);
            for (const auto &nick : eligible)
                assert(plainText(*list, nick));
            assert(!plainText(view, "Expired") && !plainText(view, "Muted"));
            near(scroll->scrollOffset(), popupOffset);
            near(scroll->maxScrollOffset(), popupMaximum);
            const auto openedBody = bounds(body(view, "FooterRow-30"));
            near(openedBody.top, popupBody.top);
            near(openedBody.height, popupBody.height);
            dispatcher.setFocus(overflow(overflow, view));
            assert(view.handleKey({.sym = XKB_KEY_Escape, .pressed = true}));
            arrange();
            assert(!plainText(view, "Eve"));
            dispatcher.setFocus(nullptr);
            clickOverflow();
            // Retained panes are hidden without syncing their transcript on a buffer switch.
            // Keep the same participants alive so expiry cannot accidentally close the list.
            model.now += .25;
            for (const auto &nick : eligible)
                buffer.typing[nick] = model.now;
            view.setReadEligible(false);
            arrange();
            assert(plainText(view, "Eve"));
            auto *typingFocus = dispatcher.focusedArea();
            const bool typingComposerFocus = pane.focusComposer;
            const auto typingBuffer = pane.buffer;
            const auto selectedBuffer = model.activeBuffer;
            model.activeBuffer = "typing-other-query";
            view.setVisible(false);
            view.setReadEligible(false);
            assert(pane.buffer == typingBuffer && buffer.typing.at("Eve") == model.now);
            model.activeBuffer = selectedBuffer;
            view.setVisible(true);
            arrange();
            assert(!plainText(view, "Eve") && plainText(view, "+3"));
            assert(dispatcher.focusedArea() == typingFocus &&
                   pane.focusComposer == typingComposerFocus);
            near(scroll->scrollOffset(), popupOffset);
            near(scroll->maxScrollOffset(), popupMaximum);
            clickOverflow();
            buffer.typing.erase("Eve");
            arrange();
            assert(!plainText(view, "Carol") && !plainText(view, "Eve"));
            buffer.typing["Eve"] = model.now;
            arrange();
            for (int i = 0; i < 100; ++i)
                buffer.typing["Zed" + std::to_string(i)] = model.now;
            arrange();
            assert(plainText(view, "+103"));
            assert(countNodes(countNodes, view) <= boundedNodes + 2);
            // Policy changes close the full list rather than leaving stale identities.
            clickOverflow();
            model.settings.typingReceive = false;
            arrange();
            assert(!plainText(view, "Eve") && !plainText(view, "Alice"));
            model.settings.typingReceive = true;
            buffer.typing.clear();
            buffer.server = "typing-avatar-network";
            buffer.typing["[Alice]"] = model.now;
            model.settings.avatarStyle = "sprite";
            model.networks[buffer.server].caseMapping = "rfc1459";
            const auto visibleImage = [&]() -> Image * {
                for (const auto &child : tray->children())
                    if (auto *image = dynamic_cast<Image *>(child.get());
                        image && image->visible() && image->hasImage())
                        return image;
                return nullptr;
            };
            arrange();
            auto *sprite = visibleImage();
            assert(sprite && sprite->sourceWidth() == 96 && sprite->sourceHeight() == 96);
            const auto rfcHash = renderer.textures.rgbaHash(sprite->textureId());
            const auto texture = sprite->textureId();
            const auto uploads = renderer.textures.uploads;
            arrange();
            assert(visibleImage()->textureId() == texture && renderer.textures.uploads == uploads);
            model.networks[buffer.server].caseMapping = "ascii";
            arrange();
            assert(visibleImage() &&
                   renderer.textures.rgbaHash(visibleImage()->textureId()) != rfcHash);
            model.networks[buffer.server].caseMapping = "rfc1459";
            buffer.typing.clear();
            buffer.typing["{alice}"] = model.now;
            arrange();
            assert(visibleImage() &&
                   renderer.textures.rgbaHash(visibleImage()->textureId()) == rfcHash);
            const float spriteOffset = scroll->scrollOffset(),
                        spriteMaximum = scroll->maxScrollOffset();
            model.settings.avatarStyle = "none";
            arrange();
            assert(!visibleImage() && avatarCount() == 0 && plainText(view, "{alice}"));
            near(tray->height(), typingHeight);
            near(scroll->scrollOffset(), spriteOffset);
            near(scroll->maxScrollOffset(), spriteMaximum);
            buffer.typing.clear();
            buffer.server.clear();
            // Empty typing space is not a second viewport: history ink and selection pass through.
            scroll->setScrollOffset(scroll->maxScrollOffset() - typingHeight * .75f);
            arrange();
            const auto behind = body(view, "FooterRow-31");
            const auto glyphs = renderer.measureText(
                behind.text.text(), behind.text.fontSize(), behind.text.font(),
                behind.text.maxWidth(), behind.text.maxLines(), behind.text.textAlign(),
                behind.text.ellipsize(), behind.text.useMarkup());
            float textX, textY;
            Node::absolutePosition(&behind.text, textX, textY);
            const LayoutRect ink{textX + glyphs.inkLeft, textY + glyphs.inkTop,
                                 glyphs.inkRight - glyphs.inkLeft,
                                 glyphs.inkBottom - glyphs.inkTop};
            assert(ink.y + ink.height > viewport.y + viewport.height - typingHeight);
            containedBounds(ink, nativeHistoryClip(view));
            const auto fragment = sceneBounds(behind.fragment);
            const float y = fragment.y + fragment.height * .5f;
            const float x = fragment.x + 1;
            assert(dispatcher.pointerButton(x, y, BTN_LEFT, true, 201, 2010, false));
            dispatcher.pointerMotion(fragment.x + fragment.width - 1, y, 202);
            dispatcher.pointerButton(fragment.x + fragment.width - 1, y, BTN_LEFT, false, 203, 2030,
                                     false);
            arrange();
            assert(view.copySelection() && !clipboard.clipboardText()->empty());
            dispatcher.setFocus(nullptr);
            dispatcher.setSceneRoot(nullptr);
            assert(model.actions.empty());
        }
}

void transcriptPointerCursors(TranscriptTestRenderer &renderer) {
    blast::Model model;
    model.settings.timestamps = model.settings.avatars = false;
    model.settings.reducedMotion = true;
    blast::Theme theme;
    theme.update(model);
    auto &buffer = model.buffers["cursor-source"];
    buffer.id = buffer.target = "cursor-source";
    buffer.server = "network";
    buffer.kind = "query";
    blast::Message message;
    message.id = "cursor-message";
    message.nick = "Peer";
    message.text = "PlainCursor https://example.org/cursor";
    buffer.messages.push_back(message);
    blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
    Clipboard clipboard;
    blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
    view.setReadEligible(false);
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&view);
    view.setInputDispatcher(dispatcher);
    uint32_t cursor = 0;
    dispatcher.setCursorShapeCallback([&](uint32_t, uint32_t shape) { cursor = shape; });
    view.sync();
    view.arrange(renderer, {17, 23, 900, 600});
    const auto plain = body(view, "PlainCursor");
    float x, y;
    Node::mapToScene(&plain.fragment, plain.fragment.width() / 2, plain.fragment.height() / 2, x,
                     y);
    dispatcher.pointerMotion(x, y, 1);
    assert(cursor == 9); // native text/I-beam, not a clickable row
    assert(dispatcher.pointerButton(x, y, BTN_LEFT, true, 2, 20, false));
    dispatcher.pointerMotion(x + 20, y, 3);
    assert(cursor == 9); // capture retains selection cursor
    dispatcher.pointerButton(x + 20, y, BTN_LEFT, false, 4, 40, false);
    InputArea *link = nullptr;
    const auto visit = [&](auto &&self, Node &node) -> void {
        if (auto *area = dynamic_cast<InputArea *>(&node);
            area && area->tabFocusKey().find("https://example.org/cursor") != std::string::npos)
            link = area;
        for (const auto &child : node.children())
            self(self, *child);
    };
    visit(visit, view);
    assert(link);
    Node::mapToScene(link, link->width() / 2, link->height() / 2, x, y);
    dispatcher.pointerMotion(x, y, 5);
    assert(cursor == 4); // native pointer for the actual link
    Node::mapToScene(&plain.row, plain.row.width() - 20, plain.row.height() / 2, x, y);
    dispatcher.pointerMotion(x, y, 6);
    assert(cursor == 1); // message padding/context handling is not a hyperlink
    Node::mapToScene(&view, 450, 450, x, y);
    dispatcher.pointerMotion(x, y, 7);
    assert(cursor == 1); // empty history canvas
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void sourceMentionConsumers(TranscriptTestRenderer &renderer) {
    blast::Model model;
    model.settings.timestamps = model.settings.avatars = false;
    model.settings.reducedMotion = true;
    model.settings.foolDisplay = blast::FoolDisplay::Hidden;
    model.settings.fools.insert("MutedPeer");
    blast::Theme theme;
    theme.update(model);
    auto &source = model.buffers["network/#source"];
    source.id = "network/#source";
    source.server = "network";
    source.target = "#source";
    source.kind = "channel";
    auto &network = model.networks[source.server];
    network.nick = "Me";
    network.name = "Friendly network";
    network.state = "Ready";
    network.caps = {"message-tags", "draft/message-redaction"};
    blast::ServerConfig parent;
    parent.name = "bouncer";
    parent.sojuAccount = true;
    model.servers.push_back(parent);
    blast::ServerConfig child;
    child.name = source.server;
    child.sojuParent = parent.name;
    child.sojuNetworkId = "42";
    model.servers.push_back(child);
    blast::Message original;
    original.id = "same-id";
    original.nick = "MutedPeer";
    original.text = "SourcePrivate Me https://example.org/private";
    original.mention = true;
    source.messages.push_back(original);
    model.mentionSources[source.id] = source;
    auto &feed = model.buffers[std::string(blast::GlobalMentionsBufferId)];
    feed.id = std::string(blast::GlobalMentionsBufferId);
    feed.kind = "mentions";
    auto row = original;
    row.id = "aggregate-source-row";
    row.sourceBuffer = source.id;
    row.sourceId = original.id;
    row.sourceServer = source.server;
    row.sourceTarget = source.target;
    feed.messages.push_back(row);
    blast::PaneState pane{.id = 1, .buffer = feed.id, .reply = {}};
    Clipboard clipboard;
    MenuHost host;
    std::vector<std::string> commands;
    blast::TranscriptView view(
        model, pane, clipboard, [&](auto command) { commands.push_back(command); }, [](auto) {});
    view.setReadEligible(false);
    view.setPopupContext(&host);
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&view);
    view.setInputDispatcher(dispatcher);
    const auto arrange = [&] {
        view.sync();
        view.arrange(renderer, {17, 23, 1000, 600});
    };
    const auto menu = [&] {
        dispatcher.setFocus(&body(view, "SourcePrivate").row);
        dispatcher.keyEvent(XKB_KEY_Menu, 0, 0, true);
        dispatcher.keyEvent(XKB_KEY_Menu, 0, 0, false);
        assert(host.open);
    };
    const auto actorInk = [&]() {
        TextNode *actor = nullptr;
        const auto visit = [&](auto &&self, Node &node) -> void {
            if (!node.visible())
                return;
            if (auto *text = dynamic_cast<TextNode *>(&node);
                text && !text->useMarkup() && text->text() == original.nick) {
                assert(!actor); // Exactly one sender annotation, separate from message body.
                actor = text;
            }
            for (const auto &child : node.children())
                self(self, *child);
        };
        visit(visit, view);
        return actor;
    };
    arrange();
    assert(!bodyText(view, "SourcePrivate"));
    assert(!actorInk());
    // The aggregate cannot grant permission to expose the original.
    model.settings.bufferFools[feed.id] = true;
    arrange();
    assert(!view.messageReadable("message:" + row.id));
    model.settings.bufferFools[source.id] = true;
    arrange();
    assert(view.messageReadable("message:" + row.id));
    near(body(view, "SourcePrivate").content.opacity(), 1);
    assert(plainText(view, "Network / " + source.target));
    assert(!plainText(view, network.name + " / " + source.target));
    auto &visibleRow = body(view, "SourcePrivate").row;
    dispatcher.setFocus(&visibleRow);
    const auto focusKey = visibleRow.tabFocusKey();
    const auto messagesRevision = feed.messagesRevision;
    auto &catalogName =
        model.networks[parent.name].sojuNetworks[child.sojuNetworkId].attributes["name"];
    catalogName = "Catalog network";
    arrange(); // Catalog arrival must refresh an already materialized row.
    assert(!plainText(view, "Network / " + source.target));
    assert(plainText(view, "Catalog network / " + source.target));
    assert(dispatcher.focusedArea() == &visibleRow && visibleRow.tabFocusKey() == focusKey);
    catalogName = "Renamed network";
    arrange(); // No message revision changes accompany a live catalog rename.
    assert(!plainText(view, "Catalog network / " + source.target));
    assert(plainText(view, "Renamed network / " + source.target));
    assert(dispatcher.focusedArea() == &visibleRow && visibleRow.tabFocusKey() == focusKey);
    assert(feed.messagesRevision == messagesRevision);
    assert(feed.messages.front().sourceBuffer == source.id &&
           feed.messages.front().sourceServer == source.server &&
           feed.messages.front().sourceTarget == source.target &&
           feed.messages.front().sourceId == original.id);
    auto *actor = actorInk();
    assert(actor && actor->font().weight == FontWeight::Bold);
    assert(actor->color() == blast::nickColor(model, original.nick, source.server));
    auto *origin = plainText(view, "Renamed network / " + source.target);
    assert(origin);
    const auto actorBounds = sceneBounds(*actor);
    const auto originBounds = sceneBounds(*origin);
    assert(actorBounds.x + actorBounds.width <= originBounds.x + .02f);
    const auto bodyBounds = sceneBounds(body(view, "SourcePrivate").text);
    assert(actorBounds.y + actorBounds.height <= bodyBounds.y + .02f);
    const auto clickActor = [&] {
        const float centerX = actorBounds.x + actorBounds.width * .5f;
        const float centerY = actorBounds.y + actorBounds.height * .5f;
        assert(dispatcher.inputAreaAt(centerX, centerY));
        dispatcher.pointerButton(centerX, centerY, BTN_LEFT, true, 101, 1010, false);
        dispatcher.pointerButton(centerX, centerY, BTN_LEFT, false, 102, 1020, false);
    };
    clickActor();
    assert(model.actions.size() == 1 && model.actions.back().type == "source-jump" &&
           model.actions.back().buffer == source.id && model.actions.back().text == original.id);
    assert(commands.empty());
    model.actions.clear();
    assert(!plainText(view, source.server + " / " + source.target));
    const auto rendered = body(view, "SourcePrivate");
    assert(!bubbleSurface(rendered)); // Dense aggregate rows have no forced mention fill.
    for (const auto &child : rendered.content.children())
        if (const auto *rect = dynamic_cast<RectNode *>(child.get()); rect && rect->visible())
            assert(rect->style().fill != blast::themeForegroundColor("tertiary"));
    float textX, textY;
    Node::mapToScene(&rendered.fragment, 2, rendered.fragment.height() * .5f, textX, textY);
    dispatcher.pointerButton(textX, textY, BTN_LEFT, true, 103, 1030, false);
    dispatcher.pointerButton(textX, textY, BTN_LEFT, false, 104, 1040, false);
    assert(model.actions.size() == 1 && model.actions.back().type == "source-jump" &&
           model.actions.back().buffer == source.id && model.actions.back().text == original.id);
    assert(!host.open && !view.copySelection());
    model.actions.clear();
    dispatcher.pointerButton(textX, textY, BTN_LEFT, true, 105, 1050, false);
    dispatcher.pointerMotion(textX + 30, textY, 106);
    dispatcher.pointerButton(textX + 30, textY, BTN_LEFT, false, 107, 1070, false);
    assert(model.actions.empty() && !host.open && view.copySelection());
    model.settings.bufferFools[source.id] = false;
    clickActor(); // The materialized sender cannot retain permission after its source is hidden.
    assert(model.actions.empty() && commands.empty());
    model.settings.bufferFools[source.id] = true;
    menu();
    host.select("Copy text");
    assert(clipboard.clipboardText() == original.text);
    assert(feed.messages.front().text == original.text);
    menu();
    assert(host.contains("Copy text") && host.contains("Open original"));
    assert(!host.contains("Reply") && !host.contains("React") && !host.contains("Delete message") &&
           !host.contains("Whois MutedPeer"));
    const auto selected = view.selectedMessage();
    assert(selected && !selected->canReply && !selected->canReact);
    assert(!view.replyToMessage(*selected) && !view.reactToMessage(*selected, "👍"));
    host.select("Open original");
    assert(model.actions.size() == 1 && model.actions.back().type == "source-jump" &&
           model.actions.back().buffer == source.id && model.actions.back().text == original.id);
    model.actions.clear();
    menu();
    clipboard.setClipboardText("sentinel");
    model.settings.bufferFools[source.id] = false;
    host.select("Copy text");
    assert(clipboard.clipboardText() == "sentinel" && !view.resolveMessage(*selected));
    arrange();
    assert(!bodyText(view, "SourcePrivate"));
    assert(!actorInk());
    model.settings.bufferFools[source.id] = true;
    arrange();
    menu();
    // A source tombstone beats a retained positive snapshot before feed reflow.
    source.messages.front().deleted = true;
    assert(!view.messageReadable("message:" + row.id) && !view.resolveMessage(*selected));
    host.select("Copy text");
    assert(clipboard.clipboardText() == "sentinel");
    source.messages.front().deleted = false;
    model.settings.bufferFools[source.id] = false;
    feed.messages.front().own = true;
    ++feed.messagesRevision;
    arrange();
    assert(!bodyText(view, "SourcePrivate"));
    feed.messages.front().own = false;
    model.settings.bufferFools[source.id] = true;
    feed.messages.front().sourceServer = "wrong-network";
    ++feed.messagesRevision;
    arrange();
    assert(!bodyText(view, "SourcePrivate"));
    assert(!actorInk());
    feed.messages.front() = row;
    feed.messages.front().nick = "ForgedActor";
    ++feed.messagesRevision;
    arrange();
    assert(!bodyText(view, "SourcePrivate") && !actorInk() && !plainText(view, "ForgedActor"));
    feed.messages.front() = row;
    ++feed.messagesRevision;
    arrange();
    assert(view.messageReadable("message:" + row.id));
    const auto sourceId = source.id;
    model.buffers.erase(sourceId);
    model.mentionSources.clear();
    assert(!view.messageReadable("message:" + row.id));
    arrange();
    assert(!bodyText(view, "SourcePrivate") && model.actions.empty() && commands.empty());
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void searchSourceConsumers(TranscriptTestRenderer &renderer) {
    blast::Model model;
    model.settings.timestamps = model.settings.avatars = false;
    model.settings.foolDisplay = blast::FoolDisplay::Hidden;
    blast::Theme theme;
    theme.update(model);
    auto &source = model.buffers["network/#source"];
    source.id = "network/#source";
    source.server = "network";
    source.target = "#source";
    source.kind = "channel";
    auto &results = model.buffers["network/*search*"];
    results.id = "network/*search*";
    results.server = source.server;
    results.kind = "search";
    blast::Message row;
    row.id = "search-row";
    row.nick = "Peer";
    row.text = "[#source] Peer: SearchBody unique text";
    row.presence = row.history = true;
    row.sourceBuffer = source.id;
    row.sourceServer = source.server;
    row.sourceTarget = source.target;
    row.sourceId = "original-id";
    row.sourceText = "SearchBody unique text";
    results.messages.push_back(row);
    blast::Message original;
    original.id = row.sourceId;
    original.nick = row.nick;
    original.text = row.sourceText;
    source.messages.push_back(original);
    blast::PaneState pane{.id = 1, .buffer = results.id, .reply = {}};
    Clipboard clipboard;
    MenuHost host;
    blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
    view.setReadEligible(false);
    view.setPopupContext(&host);
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&view);
    view.setInputDispatcher(dispatcher);
    const auto arrange = [&] {
        view.sync();
        view.arrange(renderer, {17, 23, 1000, 600});
    };
    arrange();
    const auto rendered = body(view, "SearchBody");
    float x, y;
    Node::mapToScene(&rendered.fragment, 2, rendered.fragment.height() * .5f, x, y);
    dispatcher.pointerButton(x, y, BTN_LEFT, true, 101, 1010, false);
    dispatcher.pointerButton(x, y, BTN_LEFT, false, 102, 1020, false);
    assert(model.actions.size() == 1 && model.actions.back().type == "search-jump" &&
           model.actions.back().buffer == results.id && model.actions.back().text == row.id);
    model.actions.clear();
    dispatcher.pointerButton(x, y, BTN_LEFT, true, 103, 1030, false);
    dispatcher.pointerMotion(x + 30, y, 104);
    dispatcher.pointerButton(x + 30, y, BTN_LEFT, false, 105, 1050, false);
    assert(model.actions.empty() && view.copySelection());
    dispatcher.setFocus(&rendered.row);
    dispatcher.keyEvent(XKB_KEY_Menu, 0, 0, true);
    dispatcher.keyEvent(XKB_KEY_Menu, 0, 0, false);
    assert(host.open && !host.contains("Reply") && !host.contains("React") &&
           !host.contains("Delete message"));
    const auto selected = view.selectedMessage();
    assert(selected && !selected->canReply && !selected->canReact);
    assert(!view.replyToMessage(*selected) && !view.reactToMessage(*selected, "👍"));
    source.messages.front().deleted = true;
    ++source.participationRevision;
    assert(!view.messageReadable("message:" + row.id) && !view.resolveMessage(*selected));
    arrange();
    assert(!bodyText(view, "SearchBody") && model.actions.empty());
    source.messages.front().deleted = false;
    ++source.participationRevision;
    arrange();
    model.settings.fools.insert("Peer");
    arrange();
    assert(!view.messageReadable("message:" + row.id));
    assert(!bodyText(view, "SearchBody") && model.actions.empty());
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void retainedMentionFooter(TranscriptTestRenderer &renderer) {
    blast::Model model;
    model.settings.timestamps = model.settings.avatars = false;
    model.settings.reducedMotion = true;
    blast::Theme theme;
    theme.update(model);
    auto &buffer = model.buffers["network/#mentions"];
    buffer.id = "network/#mentions";
    buffer.server = "network";
    buffer.target = "#mentions";
    buffer.kind = "channel";
    buffer.draft = "untouched draft";
    buffer.unread = 260;
    auto &retained = model.mentionSources[buffer.id];
    retained.id = buffer.id;
    retained.server = buffer.server;
    retained.target = buffer.target;
    retained.kind = buffer.kind;
    for (int i = 0; i < 260; ++i) {
        blast::Message entry;
        entry.id = "retained-" + std::to_string(i);
        entry.text = "RetainedBody-" + std::to_string(i);
        entry.nick = "Peer";
        entry.mention = i == 0 || i == 250 || i == 259;
        entry.timestamp = 1700000000 + i;
        retained.messages.push_back(entry);
        if (i >= 250)
            buffer.messages.push_back(entry);
    }
    blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = "existing-reply"};
    Clipboard clipboard;
    blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
    view.setReadEligible(false);
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&view);
    view.setInputDispatcher(dispatcher);
    auto composer = std::make_unique<Input>();
    auto *input = composer.get();
    input->setValue("untouched draft");
    input->inputArea()->setTabFocusKey("composer:1:input");
    view.addChild(std::move(composer));
    const auto arrange = [&] {
        view.sync();
        view.arrange(renderer, {17, 23, 900, 600});
        input->arrange(renderer, {12, 560, 240, 32});
    };
    const auto control = [&](std::string_view suffix) {
        Button *found = nullptr;
        const auto visit = [&](auto &&self, Node &node) -> void {
            if (auto *button = dynamic_cast<Button *>(&node);
                button && button->inputArea()->tabFocusKey().ends_with(suffix))
                found = button;
            for (const auto &child : node.children())
                self(self, *child);
        };
        visit(visit, view);
        assert(found);
        return found;
    };
    const auto click = [&](Button *button) {
        float x, y;
        Node::mapToScene(button, button->width() / 2, button->height() / 2, x, y);
        dispatcher.pointerButton(x, y, BTN_LEFT, true, 101, 1010, false);
        dispatcher.pointerButton(x, y, BTN_LEFT, false, 102, 1020, false);
    };
    arrange();
    dispatcher.setFocus(input->inputArea());
    TextInputEdit preedit;
    preedit.preeditText = "compose";
    preedit.preeditCursorBegin = preedit.preeditCursorEnd = 7;
    preedit.hasPreedit = true;
    input->textInputApplyEdit(preedit);
    assert(input->composing());
    arrange();
    const auto composition = input->editingState();
    assert(composition.preeditBase && composition.preeditLength == preedit.preeditText.size());
    assert(composition.current.value.substr(composition.preeditStart, composition.preeditLength) ==
           preedit.preeditText);
    const auto unchangedComposition = [&] {
        assert(dispatcher.focusedArea() == input->inputArea());
        assert(input->composing());
        assert(input->editingStateMatches(composition));
    };
    auto *older = control(":mention-older"), *newer = control(":mention-newer");
    auto *markRead = control(":mention-read");
    assert(plainText(*markRead, "3") && !plainText(view, "@3"));
    assert(older->x() == markRead->x() && newer->x() == markRead->x() &&
           older->y() < markRead->y() && markRead->y() < newer->y());
    assert(older->enabled() && !newer->enabled());
    auto *scroll = find<ScrollView>(view);
    assert(scroll);
    auto *follow = control(":follow-newest");
    assert(!follow->visible());
    view.arrange(renderer, {17, 23, 900, 180});
    assert(scroll->maxScrollOffset() > 0);
    scroll->setScrollOffset(0);
    view.sync();
    view.arrange(renderer, {17, 23, 900, 180});
    assert(!pane.following && pane.unseen == 0 && follow->visible());
    near(follow->width(), follow->height());
    float followLeft, followTop, followRight, followBottom;
    float navLeft, navTop, navRight, navBottom;
    Node::transformedBounds(follow, followLeft, followTop, followRight, followBottom);
    Node::transformedBounds(markRead->parent(), navLeft, navTop, navRight, navBottom);
    assert(followLeft > view.x() + view.width() * .5f);
    assert(navBottom < followTop);
    assert(followRight <= view.x() + view.width() && followBottom <= view.y() + view.height());
    // Reply occlusion moves controls, never the viewport; cramped controls disappear.
    for (const LayoutRect viewport : {LayoutRect{17, 23, 900, 260}, LayoutRect{17, 23, 900, 180},
                                      LayoutRect{17, 23, 160, 180}, LayoutRect{17, 23, 80, 100}}) {
        view.setContextInset(0, 0, 64);
        view.arrange(renderer, viewport);
        near(scroll->height(), viewport.height);
        near(scroll->contentViewportHeight(), viewport.height);
        const float cardTop = viewport.y + viewport.height - 64;
        for (const Node *control : {static_cast<Node *>(follow), markRead->parent()})
            if (control->visible()) {
                const auto rect = sceneBounds(*control);
                assert(rect.y >= viewport.y - .5f && rect.y + rect.height <= cardTop + .5f &&
                       rect.x >= viewport.x - .5f &&
                       rect.x + rect.width <= viewport.x + viewport.width + .5f);
            }
    }
    view.setContextInset(0, 0, 0);
    view.arrange(renderer, {17, 23, 900, 180});
    view.arrange(renderer, {17, 23, 900, 100});
    Node::transformedBounds(follow, followLeft, followTop, followRight, followBottom);
    Node::transformedBounds(markRead->parent(), navLeft, navTop, navRight, navBottom);
    assert(navRight < followLeft);
    view.arrange(renderer, {17, 23, 80, 100});
    assert(scroll->maxScrollOffset() > 0);
    // At this width the mention rail may hide, leaving enough room for the FAB.
    // Require safe usable geometry rather than a particular visibility decision.
    if (follow->visible()) {
        containedBounds(sceneBounds(*follow), nativeHistoryClip(view));
        if (markRead->parent()->visible()) {
            const auto fab = sceneBounds(*follow), rail = sceneBounds(*markRead->parent());
            assert(fab.x + fab.width <= rail.x || rail.x + rail.width <= fab.x ||
                   fab.y + fab.height <= rail.y || rail.y + rail.height <= fab.y);
        }
    }
    view.arrange(renderer, {17, 23, 900, 180});
    click(follow);
    unchangedComposition();
    view.sync();
    view.arrange(renderer, {17, 23, 900, 180});
    assert(pane.following && pane.unseen == 0 && !follow->visible());
    near(scroll->scrollOffset(), scroll->maxScrollOffset());
    arrange();
    assert(!follow->visible());
    const float reservedHeight = scroll->height();
    buffer.readMarker = "local:retained-0";
    arrange();
    assert(plainText(*markRead, "2") && older->enabled() && !newer->enabled());
    unchangedComposition();
    near(scroll->height(), reservedHeight);
    buffer.readMarker.clear();
    arrange();
    assert(plainText(*markRead, "3"));
    const auto beforeDisabledNewer = model.actions.size();
    click(newer);
    unchangedComposition();
    assert(model.actions.size() == beforeDisabledNewer);
    click(older);
    unchangedComposition();
    assert(model.actions.back().type == "source-jump" &&
           model.actions.back().text == "retained-259");
    assert(view.jumpToSourceMessage("retained-259"));
    arrange();
    assert(older->enabled() && !newer->enabled());
    click(older);
    unchangedComposition();
    assert(model.actions.back().text == "retained-250");
    assert(view.jumpToSourceMessage("retained-250"));
    arrange();
    click(older);
    unchangedComposition();
    assert(model.actions.back().text == "retained-0");
    assert(!view.jumpToSourceMessage("retained-0")); // Core must materialize first.
    buffer.messages = retained.messages;
    ++buffer.messagesRevision;
    assert(view.jumpToSourceMessage("retained-0"));
    arrange();
    assert(!older->enabled() && newer->enabled());
    const auto atEnd = model.actions.size();
    click(older);
    unchangedComposition();
    assert(model.actions.size() == atEnd); // no wrap
    for (const auto *id : {"retained-250", "retained-259"}) {
        click(newer);
        unchangedComposition();
        assert(model.actions.back().text == id);
        assert(view.jumpToSourceMessage(id));
        arrange();
        unchangedComposition();
    }
    assert(older->enabled() && !newer->enabled());
    const auto atNewest = model.actions.size();
    click(newer);
    unchangedComposition();
    assert(model.actions.size() == atNewest);
    assert(pane.reply == "existing-reply" && pane.focusComposer &&
           buffer.draft == "untouched draft");
    assert(view.selectedMessage() && view.selectedMessage()->nick == "Peer");
    model.settings.typingReceive = false;
    arrange();
    near(scroll->height(), 600);
    click(newer); // Disabled overlay arrow must not fall through to the canvas.
    unchangedComposition();
    assert(model.actions.size() == atNewest);
    click(older);
    unchangedComposition();
    assert(model.actions.back().text == "retained-250");
    assert(view.jumpToSourceMessage("retained-250"));
    arrange();
    unchangedComposition();
    const auto beforeReadBodyY = sceneBounds(body(view, "RetainedBody-250").body).y;
    auto &other = model.buffers["network/#other-unread"];
    other.id = "network/#other-unread";
    other.unread = 7;
    const auto beforeRead = model.actions.size();
    click(control(":mention-read"));
    unchangedComposition();
    assert(model.actions.size() == beforeRead + 1 && model.actions.back().type == "read" &&
           model.actions.back().buffer == buffer.id && other.unread == 7);
    // Model the existing Core read result; retained history must not keep the control visible.
    buffer.unread = 0;
    arrange();
    assert(!older->parent()->visible());
    near(scroll->height(), 600);
    near(sceneBounds(body(view, "RetainedBody-250").body).y, beforeReadBodyY);
    buffer.unread = 260;
    arrange();
    const auto overlayOffset = scroll->scrollOffset();
    retained.messages.clear();
    arrange();
    near(scroll->height(), 600);
    near(scroll->scrollOffset(), overlayOffset);
    assert(!older->parent()->visible());
    near(reservedHeight, 600);
    pane.following = false;
    view.sync();
    view.arrange(renderer, {17, 23, 900, 10000});
    view.arrange(renderer, {17, 23, 900, scroll->height() + scroll->maxScrollOffset() + 1.f});
    assert(scroll->maxScrollOffset() == 0 && !follow->visible());
    buffer.messages.clear();
    ++buffer.messagesRevision;
    arrange();
    assert(!follow->visible());
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void foolTranscriptPolicy(TranscriptTestRenderer &renderer) {
    blast::Model model;
    model.settings.timestamps = model.settings.avatars = false;
    model.settings.reducedMotion = true;
    model.settings.fools.insert("MutedPeer");
    blast::Theme theme;
    theme.update(model);
    auto &buffer = model.buffers["fool-transcript"];
    buffer.id = buffer.target = "fool-transcript";
    buffer.target = "#fools";
    buffer.kind = "query";
    buffer.server = "policy-network";
    auto &network = model.networks[buffer.server];
    network.state = "Ready";
    network.nick = "Me";
    network.caps = {"message-tags"};
    for (int i = 0; i < 4; ++i) {
        blast::Message message;
        message.id = "policy-" + std::to_string(i);
        message.nick = i == 1 || i == 2 ? "MutedPeer" : "Peer";
        message.text = i == 0   ? "VisibleBefore"
                       : i == 3 ? "VisibleAfter"
                                : "SecretBody-" + std::to_string(i);
        buffer.messages.push_back(std::move(message));
    }
    const auto before = buffer;
    blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
    Clipboard clipboard;
    MenuHost host;
    blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
    view.setReadEligible(false);
    view.setPopupContext(&host);
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&view);
    view.setInputDispatcher(dispatcher);
    const auto arrange = [&] {
        view.sync();
        view.arrange(renderer, {17, 23, 1200, 1000});
    };
    const auto point = [&](std::string_view token, bool end) {
        const auto rendered = body(view, token);
        std::vector<TextCursorStop> stops;
        renderer.measureTextCaretStops(rendered.text.text(), rendered.text.fontSize(),
                                       {end ? token.size() : 0}, 0, stops, rendered.text.font(),
                                       true);
        assert(stops.size() == 1);
        const auto metrics = renderer.measureText(rendered.text.text(), rendered.text.fontSize(),
                                                  rendered.text.font(), 0, 0, TextAlign::Start,
                                                  TextEllipsize::End, true);
        float x, y;
        Node::absolutePosition(&rendered.text, x, y);
        return std::pair{x - metrics.alignOffsetX + stops[0].x,
                         y - std::floor(metrics.layoutBaseline) + stops[0].y +
                             stops[0].height * .5f};
    };
    const auto selectAcross = [&](const std::string &expected) {
        view.clearTextSelection();
        const auto from = point("VisibleBefore", false), to = point("VisibleAfter", true);
        assert(dispatcher.pointerButton(from.first, from.second, BTN_LEFT, true, 91, 910, false));
        dispatcher.pointerMotion(to.first, to.second, 92);
        dispatcher.pointerButton(to.first, to.second, BTN_LEFT, false, 93, 930, false);
        assert(view.copySelection() && clipboard.clipboardText() == expected);
        view.clearTextSelection();
    };
    for (const auto mode :
         {blast::FoolDisplay::Minimized, blast::FoolDisplay::Hidden, blast::FoolDisplay::Dimmed}) {
        dispatcher.setFocus(nullptr);
        model.settings.foolDisplay = mode;
        arrange();
        const bool readable = mode == blast::FoolDisplay::Dimmed;
        assert(bool(bodyText(view, "SecretBody-1")) == readable);
        assert(bool(bodyText(view, "SecretBody-2")) == readable);
        selectAcross(readable ? "VisibleBefore\nSecretBody-1\nSecretBody-2\nVisibleAfter"
                              : "VisibleBefore\nVisibleAfter");
        dispatcher.setFocus(&body(view, "VisibleAfter").row);
        assert(view.navigateMessage(-1));
        arrange();
        auto selected = view.selectedMessage();
        assert(selected && selected->nick == (readable ? "MutedPeer" : "Peer"));
        assert(view.replyToMessage(*selected));
        assert(pane.reply == (readable ? "message:policy-2" : "message:policy-0"));
        pane.reply.clear();
        pane.focusComposer = false;
        if (readable) {
            const auto secret = body(view, "SecretBody-1");
            assert(secret.content.opacity() > 0 && secret.content.opacity() < 1);
            dispatcher.setFocus(&secret.row);
            dispatcher.keyEvent(XKB_KEY_Menu, 0, 0, true);
            dispatcher.keyEvent(XKB_KEY_Menu, 0, 0, false);
            assert(host.open && host.contains("Copy text"));
            host.select("Copy text");
            assert(clipboard.clipboardText() == "SecretBody-1");
        } else if (mode == blast::FoolDisplay::Minimized) {
            const auto summary = body(view, "2 minimized messages");
            near(summary.body.foregroundBlurRadius(), 0);
            for (const float width : {240.f, 1200.f}) {
                buffer.kind = "channel";
                model.settings.messageLayouts[buffer.id] = blast::MessageLayout::Irc;
                view.sync();
                view.arrange(renderer, {17, 23, width, 1000});
                const auto compactSummary = body(view, "2 minimized messages");
                assert(!plainText(compactSummary.content, "MutedPeer"));
                const auto summaryBounds = bounds(compactSummary);
                const auto beforeBounds = bounds(body(view, "VisibleBefore"));
                const auto afterBounds = bounds(body(view, "VisibleAfter"));
                assert(summaryBounds.top >= beforeBounds.top + beforeBounds.height);
                assert(afterBounds.top >= summaryBounds.top + summaryBounds.height);
            }
            buffer.kind = "query";
            model.settings.messageLayouts.erase(buffer.id);
            arrange();
            dispatcher.setFocus(&body(view, "2 minimized messages").row);
            dispatcher.keyEvent(XKB_KEY_Menu, 0, 0, true);
            dispatcher.keyEvent(XKB_KEY_Menu, 0, 0, false);
            assert(!host.open || !host.contains("Copy text"));
            host.closeSelectDropdown();
            dispatcher.setFocus(&body(view, "2 minimized messages").row);
            dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
            dispatcher.keyEvent(XKB_KEY_Return, 0, 0, false);
            arrange();
            assert(bodyText(view, "SecretBody-1") && bodyText(view, "SecretBody-2"));
            selectAcross("VisibleBefore\nSecretBody-1\nSecretBody-2\nVisibleAfter");
            dispatcher.setFocus(&body(view, "VisibleAfter").row);
            assert(view.navigateMessage(-1));
            arrange();
            selected = view.selectedMessage();
            assert(selected && selected->nick == "MutedPeer");
        }
        unchanged(buffer, before);
    }
    model.settings.foolDisplay = blast::FoolDisplay::Hidden;
    model.settings.bufferFools[buffer.id] = true;
    arrange();
    const auto shown = body(view, "SecretBody-1");
    near(shown.content.opacity(), 1);
    near(shown.body.foregroundBlurRadius(), 0);
    assert(view.messageReadable("message:policy-1"));
    selectAcross("VisibleBefore\nSecretBody-1\nSecretBody-2\nVisibleAfter");
    dispatcher.setFocus(&shown.row);
    dispatcher.keyEvent(XKB_KEY_Menu, 0, 0, true);
    dispatcher.keyEvent(XKB_KEY_Menu, 0, 0, false);
    assert(host.open && host.contains("Copy text"));
    const auto stale = view.selectedMessage();
    model.settings.bufferFools[buffer.id] = false;
    clipboard.setClipboardText("unchanged");
    host.select("Copy text");
    assert(clipboard.clipboardText() == "unchanged");
    assert(!view.messageReadable("message:policy-1"));
    if (stale) {
        assert(!view.resolveMessage(*stale));
        assert(!view.replyToMessage(*stale));
        assert(!view.reactToMessage(*stale, "👍"));
    }
    arrange();
    assert(!bodyText(view, "SecretBody-1") && !bodyText(view, "SecretBody-2"));
    model.settings.bufferFools.erase(buffer.id);
    model.settings.bufferFools[buffer.target] = true;
    arrange();
    assert(view.messageReadable("message:policy-1"));
    assert(model.actions.empty());
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void blurredBodyAndReplyPrivacy(TranscriptTestRenderer &renderer) {
    blast::Model model;
    model.settings.timestamps = model.settings.avatars = false;
    model.settings.reducedMotion = true;
    model.settings.fools.insert("MutedPeer");
    model.settings.foolDisplay = blast::FoolDisplay::Blurred;
    blast::Theme theme;
    theme.update(model);
    auto &buffer = model.buffers["blur-privacy"];
    buffer.id = buffer.target = "blur-privacy";
    buffer.kind = "query";
    const std::string secret = "Private body https://example.org/private";
    for (int i = 0; i < 4; ++i) {
        blast::Message message;
        message.id = "blur-" + std::to_string(i);
        message.nick = i == 0 ? "MutedPeer" : "Peer";
        message.text = i == 0   ? secret
                       : i == 1 ? "OrdinaryIntervening"
                       : i == 2 ? "ReplyBody"
                                : "VisibleTail";
        if (i == 2)
            message.reply = "blur-0";
        buffer.messages.push_back(std::move(message));
    }
    const auto before = buffer;
    blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
    Clipboard clipboard;
    clipboard.setClipboardText("sentinel");
    MenuHost host;
    std::vector<std::string> commands;
    blast::TranscriptView view(
        model, pane, clipboard, [&](auto command) { commands.emplace_back(command); }, [](auto) {});
    view.setPopupContext(&host);
    view.setReadEligible(false);
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&view);
    view.setInputDispatcher(dispatcher);
    const auto arrange = [&] {
        view.sync();
        view.arrange(renderer, {17, 23, 1400, 1000});
    };
    const auto quote = [&](bool readable) {
        const auto rendered = body(view, "ReplyBody");
        bool author = false, excerpt = false;
        const auto inspect = [&](auto &&self, Node &node) -> void {
            if (!node.visible())
                return;
            if (auto *text = dynamic_cast<TextNode *>(&node)) {
                const auto value = text->useMarkup() ? decodedMarkup(text->text()) : text->text();
                author |= value == "MutedPeer";
                excerpt |= value == secret;
                if (!readable) {
                    assert(value.find("MutedPeer") == std::string::npos);
                    assert(value.find("Private body") == std::string::npos);
                    assert(value.find("https://example.org/private") == std::string::npos);
                }
            }
            if (!readable)
                if (const auto *area = dynamic_cast<InputArea *>(&node)) {
                    const auto tooltip = area->tooltipContent();
                    if (const auto *value = std::get_if<std::string>(&tooltip))
                        assert(value->find("MutedPeer") == std::string::npos &&
                               value->find("https://example.org/private") == std::string::npos);
                }
            for (const auto &child : node.children())
                self(self, *child);
        };
        inspect(inspect, rendered.content);
        assert(author == readable && excerpt == readable);
    };
    const auto toggle = [&](InputArea &row) {
        dispatcher.setFocus(&row);
        dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
        dispatcher.keyEvent(XKB_KEY_Return, 0, 0, false);
        arrange();
    };
    arrange();
    assert(!view.messageReadable("message:blur-0") && !view.messageReadable("message:missing"));
    quote(false);
    model.settings.foolDisplay = blast::FoolDisplay::Hidden;
    arrange();
    quote(false);
    model.settings.foolDisplay = blast::FoolDisplay::Blurred;
    arrange();
    const auto masked = body(view, "Private body");
    assert(masked.body.foregroundBlurRadius() > 0);
    near(body(view, "OrdinaryIntervening").body.foregroundBlurRadius(), 0);
    assert(!masked.row.focusable());
    bool urlFound = false;
    for (const auto &child : masked.body.children())
        if (auto *area = dynamic_cast<InputArea *>(child.get())) {
            assert(!area->focusable() && !area->hitTestVisible());
            const auto tooltip = area->tooltipContent();
            assert(!std::get_if<std::string>(&tooltip) || std::get<std::string>(tooltip).empty());
            for (const auto &fragment : area->children())
                if (auto *text = dynamic_cast<TextNode *>(fragment.get());
                    text && decodedMarkup(text->text()).find("https://") != std::string::npos)
                    urlFound = true;
        }
    assert(urlFound);
    dispatcher.setFocus(&body(view, "OrdinaryIntervening").row);
    const auto safeBefore = view.selectedMessage();
    assert(safeBefore && safeBefore->nick == "Peer");
    assert(view.navigateMessage(-1));
    const auto safeAfter = view.selectedMessage();
    assert(safeAfter && safeAfter->identity == safeBefore->identity && safeAfter->nick == "Peer" &&
           !view.messageReadable("message:blur-0"));
    assert(!view.copySelection() && clipboard.clipboardText() == "sentinel");
    float x, y;
    Node::mapToScene(&masked.body, 1, masked.fragment.height() * .5f, x, y);
    dispatcher.pointerButton(x, y, BTN_RIGHT, true, 199, 1990, false);
    dispatcher.pointerButton(x, y, BTN_RIGHT, false, 200, 2000, false);
    assert(!host.open && !view.messageReadable("message:blur-0"));
    assert(clipboard.clipboardText() == "sentinel");
    dispatcher.pointerButton(x, y, BTN_LEFT, true, 201, 2010, false);
    dispatcher.pointerMotion(x + masked.body.width() - 2, y, 202);
    dispatcher.pointerButton(x + masked.body.width() - 2, y, BTN_LEFT, false, 203, 2030, false);
    assert(!view.copySelection() && !view.messageReadable("message:blur-0"));
    assert(clipboard.clipboardText() == "sentinel" && commands.empty());
    // Root click reveals; disabled URL descendants must not receive the click.
    Node::mapToScene(&masked.body, 2, masked.fragment.height() * .5f, x, y);
    assert(dispatcher.pointerButton(x, y, BTN_LEFT, true, 204, 2040, false));
    dispatcher.pointerButton(x, y, BTN_LEFT, false, 205, 2050, false);
    arrange();
    assert(view.messageReadable("message:blur-0") && commands.empty());
    near(body(view, "Private body").body.foregroundBlurRadius(), 0);
    quote(true);
    dispatcher.setFocus(&body(view, "Private body").row);
    dispatcher.keyEvent(XKB_KEY_Menu, 0, 0, true);
    dispatcher.keyEvent(XKB_KEY_Menu, 0, 0, false);
    assert(host.open && host.contains("Copy text"));
    host.select("Copy text");
    assert(clipboard.clipboardText() == secret);
    // Intervening ordinary row keeps the reply's previousVisible unchanged.
    dispatcher.setFocus(nullptr);
    model.settings.foolDisplay = blast::FoolDisplay::Minimized;
    arrange();
    assert(!view.messageReadable("message:blur-0"));
    quote(false);
    toggle(body(view, "1 minimized messages").row);
    assert(view.messageReadable("message:blur-0"));
    quote(true);
    toggle(body(view, "Private body").row);
    assert(!view.messageReadable("message:blur-0"));
    quote(false);
    unchanged(buffer, before);
    toggle(body(view, "1 minimized messages").row);
    assert(view.messageReadable("message:blur-0"));
    auto inserted = buffer.messages.front();
    inserted.id = "new-unrevealed-fool";
    inserted.text = "Different private message";
    buffer.messages.push_front(inserted);
    ++buffer.messagesRevision;
    // Cache still associates index zero with the revealed OLD source.
    assert(!view.messageReadable("message:" + inserted.id));
    buffer.messages.pop_front();
    ++buffer.messagesRevision;
    unchanged(buffer, before);
    model.settings.foolDisplay = blast::FoolDisplay::Blurred;
    arrange();
    buffer.messages[1].action = true;
    arrange();
    near(body(view, "OrdinaryIntervening").body.foregroundBlurRadius(), 0);
    assert(view.messageReadable("message:blur-1"));
    buffer.messages.front().deleted = true;
    ++buffer.participationRevision;
    arrange();
    near(body(view, "Message deleted").body.foregroundBlurRadius(), 0);
    assert(!view.messageReadable("message:blur-0"));
    quote(false);
    buffer.messages.front().deleted = false;
    buffer.messages.front().nick = "OrdinaryReplacement";
    arrange();
    near(body(view, "Private body").body.foregroundBlurRadius(), 0);
    assert(view.messageReadable("message:blur-0"));
    buffer.messages.front().nick = "MutedPeer";
    arrange();
    assert(body(view, "Private body").body.foregroundBlurRadius() > 0);
    assert(!view.messageReadable("message:blur-0"));
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void smartPresenceNickTransfer(TranscriptTestRenderer &renderer) {
    blast::Model model;
    model.settings.timestamps = model.settings.avatars = false;
    model.settings.reducedMotion = true;
    model.settings.presence = blast::PresenceMode::Smart;
    blast::Theme theme;
    theme.update(model);
    auto &buffer = model.buffers["smart-nick"];
    buffer.id = buffer.target = "smart-nick";
    buffer.kind = "channel";
    buffer.server = "network";
    model.networks[buffer.server].caseMapping = "rfc1459";
    blast::Message chat;
    chat.id = "nick-chat";
    chat.nick = "[OldPeer]";
    chat.text = "Meaningful participant chat";
    chat.timestamp = 100000;
    buffer.messages.push_back(chat);
    blast::Message rename;
    rename.id = "nick-change";
    rename.nick = "{oldpeer}";
    rename.text = "Participant changed nick";
    rename.timestamp = 100100;
    rename.presence = true;
    rename.activity = blast::ActivityKind::Nick;
    rename.activityTarget = "[NewPeer]";
    buffer.messages.push_back(rename);
    blast::Message part;
    part.id = "nick-part";
    part.nick = "{NEWPEER}";
    part.text = "Renamed participant departed";
    part.timestamp = 100200;
    part.presence = true;
    part.activity = blast::ActivityKind::Part;
    buffer.messages.push_back(part);
    const auto before = buffer;
    blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
    Clipboard clipboard;
    blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
    view.setReadEligible(false);
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&view);
    view.setInputDispatcher(dispatcher);
    const auto arrange = [&] {
        view.sync();
        view.arrange(renderer, {17, 23, 1400, 1000});
    };
    arrange();
    InputArea *summary = nullptr;
    const auto visit = [&](auto &&self, Node &node) -> void {
        if (!node.visible())
            return;
        if (auto *area = dynamic_cast<InputArea *>(&node)) {
            const auto tooltip = area->tooltipContent();
            if (const auto *text = std::get_if<std::string>(&tooltip);
                text && *text == "Expand channel activity")
                summary = area;
        }
        for (const auto &child : node.children())
            self(self, *child);
    };
    visit(visit, view);
    assert(summary && bodyText(*summary, "2"));
    dispatcher.setFocus(summary);
    dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
    dispatcher.keyEvent(XKB_KEY_Return, 0, 0, false);
    arrange();
    assert(bodyText(view, rename.text) && bodyText(view, part.text));
    unchanged(buffer, before);
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void inlineNicknameChips(TranscriptTestRenderer &renderer) {
    blast::Model model;
    model.settings.timestamps = model.settings.avatars = false;
    model.settings.reducedMotion = true;
    blast::Theme theme;
    theme.update(model);
    auto &buffer = model.buffers["inline-nicks"];
    buffer.id = buffer.target = "inline-nicks";
    buffer.kind = "query";
    buffer.server = "network";
    model.networks[buffer.server].caseMapping = "rfc1459";
    buffer.nicks = {{.name = "Alice", .account = {}, .prefix = {}},
                    {.name = "{Nick}", .account = {}, .prefix = {}}};
    const std::string original = "Alice malice [nICK] https://example.org/Alice";
    blast::Message message;
    message.id = "inline-nick-body";
    message.nick = "Alice";
    message.text = original;
    buffer.messages.push_back(message);
    const auto before = buffer;
    blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
    Clipboard clipboard;
    std::vector<std::string> commands;
    blast::TranscriptView view(
        model, pane, clipboard, [&](auto command) { commands.emplace_back(command); }, [](auto) {});
    view.setReadEligible(false);
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&view);
    view.setInputDispatcher(dispatcher);
    const auto arrange = [&] {
        view.sync();
        view.arrange(renderer, {17, 23, 1800, 1000});
    };
    arrange();
    const auto rendered = body(view, original, true);
    std::vector<RectNode *> chips;
    InputArea *alice = nullptr, *brackets = nullptr, *url = nullptr, *plainFragment = nullptr;
    TextNode *aliceText = nullptr, *urlText = nullptr, *plainInk = nullptr;
    bool plainSubstring = false;
    for (const auto &child : rendered.body.children()) {
        if (auto *rect = dynamic_cast<RectNode *>(child.get()); rect && rect->visible())
            chips.push_back(rect);
        if (auto *area = dynamic_cast<InputArea *>(child.get()))
            for (const auto &fragment : area->children())
                if (auto *text = dynamic_cast<TextNode *>(fragment.get())) {
                    const auto plain = decodedMarkup(text->text());
                    if (plain == "Alice") {
                        alice = area;
                        aliceText = text;
                    } else if (plain == "[nICK]")
                        brackets = area;
                    else if (plain == "https://example.org/Alice") {
                        url = area;
                        urlText = text;
                    } else if (plain.find("malice") != std::string::npos) {
                        plainSubstring = true;
                        plainFragment = area;
                        plainInk = text;
                        assert(!area->focusable());
                    }
                }
    }
    assert(alice && brackets && url && aliceText && urlText && plainSubstring);
    assert(chips.empty());
    for (auto *area : {alice, brackets})
        assert(area->focusable() && area->hitTestVisible());
    assert(aliceText->fontWeight() == FontWeight::Bold);
    assert(aliceText->text().find("background=") == std::string::npos);
    bool authorColorMatches = false;
    for (const auto &child : rendered.content.children())
        if (const auto *rect = dynamic_cast<RectNode *>(child.get()); rect && rect->visible()) {
            authorColorMatches =
                authorColorMatches ||
                aliceText->text().find(formatRgbHex(rect->style().fill)) != std::string::npos;
        }
    assert(authorColorMatches);
    assert(url->focusable() && url->hitTestVisible());
    const auto tooltip = url->tooltipContent();
    assert(std::get_if<std::string>(&tooltip) &&
           std::get<std::string>(tooltip) == "https://example.org/Alice");
    // Native Tab traversal, not focusable flags alone: mentions must each retain
    // a stop even though they share the non-link linkStart sentinel.
    dispatcher.setFocus(nullptr);
    assert(dispatcher.cycleTabFocusInSubtree(&rendered.body, false));
    assert(dispatcher.focusedArea() == alice);
    assert(dispatcher.cycleTabFocusInSubtree(&rendered.body, false));
    assert(dispatcher.focusedArea() == brackets);
    assert(dispatcher.cycleTabFocusInSubtree(&rendered.body, false));
    assert(dispatcher.focusedArea() == url);
    assert(dispatcher.cycleTabFocusInSubtree(&rendered.body, true));
    assert(dispatcher.focusedArea() == brackets);
    assert(dispatcher.cycleTabFocusInSubtree(&rendered.body, true));
    assert(dispatcher.focusedArea() == alice);
    dispatcher.setFocus(nullptr);
    const auto caret = [&](TextNode &text, std::size_t byte) {
        std::vector<TextCursorStop> stops;
        renderer.measureTextCaretStops(text.text(), text.fontSize(), {byte}, 0, stops, text.font(),
                                       true);
        assert(stops.size() == 1);
        const auto metrics = renderer.measureText(text.text(), text.fontSize(), text.font(), 0, 0,
                                                  TextAlign::Start, TextEllipsize::End, true);
        float x, y;
        Node::absolutePosition(&text, x, y);
        return std::pair{x - metrics.alignOffsetX + stops[0].x,
                         y - std::floor(metrics.layoutBaseline) + stops[0].y +
                             stops[0].height * .5f};
    };
    assert(plainFragment && plainInk);
    std::uint32_t cursor = 0;
    dispatcher.setCursorShapeCallback([&](std::uint32_t, std::uint32_t shape) { cursor = shape; });
    float plainX, plainY;
    Node::mapToScene(plainFragment, plainFragment->width() * .5f, plainFragment->height() * .5f,
                     plainX, plainY);
    assert(dispatcher.inputAreaAt(plainX, plainY) == plainFragment);
    dispatcher.pointerMotion(plainX, plainY, 290);
    assert(cursor == 9); // Readable plain text owns hover, not a clickable nickname action.
    const auto plainFrom = caret(*plainInk, 0);
    const auto plainTo = caret(*plainInk, decodedMarkup(plainInk->text()).size());
    assert(dispatcher.pointerButton(plainFrom.first, plainFrom.second, BTN_LEFT, true, 291, 2910,
                                    false));
    dispatcher.pointerMotion(plainTo.first, plainTo.second, 292);
    dispatcher.pointerButton(plainTo.first, plainTo.second, BTN_LEFT, false, 293, 2930, false);
    arrange();
    assert(view.copySelection() && clipboard.clipboardText()->find("malice") != std::string::npos);
    assert(commands.empty() && model.actions.empty());
    const auto baseline = bounds(rendered);
    const auto from = caret(*aliceText, 0);
    const auto to = caret(*urlText, std::string_view("https://example.org/Alice").size());
    assert(dispatcher.pointerButton(from.first, from.second, BTN_LEFT, true, 301, 3010, false));
    dispatcher.pointerMotion(to.first, to.second, 302);
    dispatcher.pointerButton(to.first, to.second, BTN_LEFT, false, 303, 3030, false);
    arrange();
    assert(view.copySelection() && clipboard.clipboardText() == original);
    assert(commands.empty());
    const auto selected = bounds(body(view, original, true));
    near(selected.top, baseline.top);
    near(selected.height, baseline.height);
    unchanged(buffer, before);
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void smartPresenceRedactionInvalidation(TranscriptTestRenderer &renderer) {
    blast::Model model;
    model.settings.timestamps = model.settings.avatars = false;
    model.settings.reducedMotion = true;
    model.settings.fx = blast::FxLevel::Off;
    model.settings.presence = blast::PresenceMode::Smart;
    blast::Theme theme;
    theme.update(model);
    auto &buffer = model.buffers["smart-redaction"];
    buffer.id = buffer.target = "smart-redaction";
    buffer.kind = "channel";
    blast::Message chat;
    chat.id = "qualifying-chat";
    chat.nick = "Participant";
    chat.text = "The only qualifying conversation";
    chat.timestamp = 100000;
    buffer.messages.push_back(chat);
    blast::Message event;
    event.id = "participant-join";
    event.nick = "Participant";
    event.text = "Previously eligible join";
    event.timestamp = 100010;
    event.presence = true;
    event.activity = blast::ActivityKind::Join;
    buffer.messages.push_back(event);
    blast::Message forced;
    forced.id = "forced-separator";
    forced.nick = "Utility";
    forced.text = "An untyped forced notice";
    forced.timestamp = 100015;
    forced.presence = true;
    buffer.messages.push_back(forced);
    event.id = "participant-part";
    event.text = "Previously eligible departure";
    event.timestamp = 100020;
    event.activity = blast::ActivityKind::Part;
    buffer.messages.push_back(event);
    const auto raw = buffer;
    const auto identities = buffer.messagesRevision;
    blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
    Clipboard clipboard;
    blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
    view.setReadEligible(false);
    const auto arrange = [&] {
        view.sync();
        view.arrange(renderer, {17, 23, 1400, 1000});
    };
    arrange();
    assert(bodyText(view, raw.messages[1].text));
    assert(bodyText(view, raw.messages[3].text));
    unchanged(buffer, raw);
    assert(view.messageReadable("message:" + buffer.messages.front().id));
    // Redaction is not an identity/order change and must invalidate Smart
    // participation even with all visual effects disabled.
    buffer.messages.front().deleted = true;
    buffer.messages.front().text.clear();
    ++buffer.participationRevision;
    // Model privacy changes are authoritative before another viewport measurement.
    assert(!view.messageReadable("message:" + buffer.messages.front().id));
    const auto redacted = buffer;
    assert(buffer.messagesRevision == identities);
    arrange();
    assert(!bodyText(view, raw.messages[1].text));
    assert(!bodyText(view, raw.messages[3].text));
    assert(bodyText(view, forced.text));
    assert(buffer.messages.size() == raw.messages.size());
    assert(buffer.messages[1].id == raw.messages[1].id &&
           buffer.messages[1].text == raw.messages[1].text);
    assert(buffer.messages[3].id == raw.messages[3].id &&
           buffer.messages[3].text == raw.messages[3].text);
    assert(buffer.messagesRevision == identities);
    unchanged(buffer, redacted);
    assert(model.actions.empty());
}

void contentFitAndConsecutiveGroups(TranscriptTestRenderer &renderer) {
    blast::Model model;
    model.settings.timestamps = model.settings.avatars = false;
    model.settings.reducedMotion = true;
    model.settings.presence = blast::PresenceMode::Hidden;
    blast::Theme theme;
    theme.update(model);
    auto &buffer = model.buffers["content-fit"];
    buffer.id = buffer.target = "content-fit";
    buffer.kind = "channel";
    for (int i = 0; i < 7; ++i) {
        blast::Message entry;
        entry.id = "fit-" + std::to_string(i);
        entry.nick = i == 3 ? "B" : "A";
        entry.text = i == 0 ? "tiny\nlines" : "Short-" + std::to_string(i);
        if (i == 1) {
            entry.presence = true;
            entry.activity = blast::ActivityKind::Join;
        }
        buffer.messages.push_back(std::move(entry));
    }
    blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
    Clipboard clipboard;
    blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
    view.setReadEligible(false);
    view.sync();
    view.arrange(renderer, {17, 23, 900, 1600});
    assert(!bodyText(view, "Short-1"));
    // A hidden event and a visible different sender each break the author group.
    for (const int i : {2, 4})
        assert(plainText(body(view, "Short-" + std::to_string(i)).content, "A"));
    for (const int i : {5, 6})
        assert(!plainText(body(view, "Short-" + std::to_string(i)).content, "A"));
    const auto multiline = body(view, "tiny");
    auto *surface = bubbleSurface(multiline);
    assert(surface && surface->width() < 200);
    for (const auto &child : multiline.body.children())
        if (const auto *area = dynamic_cast<InputArea *>(child.get()))
            assert(area->x() + area->width() <= multiline.body.width() + .02f);
    const auto gapBefore = [&](std::string_view left, std::string_view right) {
        const auto first = body(view, left), next = body(view, right);
        auto *a = bubbleSurface(first);
        auto *b = bubbleSurface(next);
        assert(a && b);
        float ax, ay, bx, by;
        Node::absolutePosition(a, ax, ay);
        Node::absolutePosition(b, bx, by);
        return by - ay - a->height();
    };
    const float groupGap = gapBefore("Short-3", "Short-4");
    const float messageGap = gapBefore("Short-4", "Short-5");
    assert(groupGap > messageGap && messageGap >= 4);
}

// Records the primitive emitted by the real material painter; no GPU or replacement layout.
class TranscriptPaintRecorder final : public RenderBackend {
  public:
    struct Rect {
        float width, height;
        RoundedRectStyle style;
    };
    std::vector<Rect> rectangles;
    CpuTextureUploadSink textures;
    void initialize(GlSharedContext &) override { std::abort(); }
    void cleanup() override {}
    bool makeCurrent(RenderTarget &) override { std::abort(); }
    bool makeCurrentNoSurface() override { std::abort(); }
    bool beginFrame(RenderTarget &) override { std::abort(); }
    void endFrame(RenderTarget &) override { std::abort(); }
    RenderGraphicsResetStatus graphicsResetStatus() override { std::abort(); }
    void invalidateGpuResources() override { std::abort(); }
    void abandonAfterGraphicsReset() noexcept override {}
    std::unique_ptr<RenderSurfaceTarget> createSurfaceTarget(wl_surface *) override {
        std::abort();
    }
    std::unique_ptr<RenderFramebuffer> createFramebuffer(std::uint32_t, std::uint32_t) override {
        std::abort();
    }
    void bindFramebuffer(const RenderFramebuffer &) override { std::abort(); }
    void bindDefaultFramebuffer() override { std::abort(); }
    void setViewport(std::uint32_t, std::uint32_t) override { std::abort(); }
    void clear(Color) override { std::abort(); }
    void setBlendMode(RenderBlendMode) override { std::abort(); }
    int maxTextureSize() override { std::abort(); }
    void setScissor(RenderScissor) override { std::abort(); }
    void disableScissor() override { std::abort(); }
    void drawRect(float, float, float width, float height, const RoundedRectStyle &style,
                  const Mat3 &) override {
        rectangles.push_back({width, height, style});
    }
    void drawImage(const RenderImageDraw &) override { std::abort(); }
    void drawGlyph(const RenderGlyphDraw &) override { std::abort(); }
    void drawSpinner(float, float, float, float, const SpinnerStyle &, const Mat3 &) override {
        std::abort();
    }
    void drawFullscreenTexture(TextureId, bool) override { std::abort(); }
    void drawFullscreenTint(Color) override { std::abort(); }
    void drawFramebufferBlur(TextureId, std::uint32_t, std::uint32_t, float, float,
                             float) override {
        std::abort();
    }
    TextureManager &textureManager() override { return textures; }
};

void selectedContentGeometry(TranscriptTestRenderer &renderer) {
    for (const int variant : {0, 1, 2}) {
        const bool dense = variant != 0;
        blast::Model model;
        model.settings.timestamps = true;
        model.settings.avatars = false;
        model.settings.reducedMotion = true;
        blast::Theme theme;
        theme.update(model);
        auto &buffer = model.buffers["selection-material"];
        buffer.id = buffer.target = "selection-material";
        buffer.kind = variant == 2 ? "query" : "channel";
        if (dense)
            model.settings.messageLayouts[buffer.id] = blast::MessageLayout::Irc;
        for (int i = 0; i < 6; ++i) {
            blast::Message entry;
            entry.id = "material-" + std::to_string(i);
            entry.nick = i < 3 ? "Peer" : "Me";
            entry.own = i >= 3;
            entry.text = "MaterialBody-" + std::to_string(i);
            if (i % 3 == 2)
                entry.text += "\nsecond line with \x02"
                              "bold 世界\x02";
            entry.time = "2026-11-07T12:34:56Z";
            buffer.messages.push_back(std::move(entry));
        }
        blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
        Clipboard clipboard;
        blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
        view.setReadEligible(false);
        InputDispatcher dispatcher;
        dispatcher.setSceneRoot(&view);
        view.setInputDispatcher(dispatcher);
        const LayoutRect viewport{17, 23, 900, 1200};
        view.sync();
        view.arrange(renderer, viewport);
        for (int i = 0; i < 6; ++i) {
            const auto rendered = body(view, "MaterialBody-" + std::to_string(i));
            const auto before = sceneBounds(rendered.row);
            dispatcher.setFocus(&rendered.row);
            view.arrange(renderer, viewport);
            RectNode *ring = nullptr;
            for (const auto &child : rendered.row.children())
                if (auto *rect = dynamic_cast<RectNode *>(child.get());
                    rect && rect->visible() && rect->style().fill.a == 0 &&
                    rect->style().borderWidth > 0) {
                    assert(!ring);
                    ring = rect;
                }
            assert(ring && roundedCorners(ring->style().radius));
            const auto bounds = sceneBounds(*ring);
            auto *material = find<blast::MaterialNode>(rendered.content);
            assert(material);
            const auto surface = sceneBounds(*material);
            near(bounds.x, surface.x);
            near(bounds.y, surface.y);
            near(bounds.height, surface.height);
            near(sceneBounds(rendered.row).height, before.height);
            near(sceneBounds(rendered.row).y, before.y);
            assert(bounds.width < rendered.row.width() * .8f);
            for (const auto &fragment : rendered.body.children())
                if (fragment->visible() && dynamic_cast<InputArea *>(fragment.get()))
                    containedBounds(sceneBounds(*fragment), bounds);
            if (!dense) {
                TranscriptPaintRecorder paint;
                material->draw(paint, {.surfaceWidth = viewport.width,
                                       .surfaceHeight = viewport.height,
                                       .width = material->width(),
                                       .height = material->height()});
                assert(paint.rectangles.size() == 1);
                near(bounds.width, paint.rectangles.front().width);
                near(bounds.height, paint.rectangles.front().height);
                nearRadii(ring->style().radius, paint.rectangles.front().style.radius);
            } else {
                nearRadii(ring->style().radius, Radii{blast::themeCornerRadius()});
                // Actual text/metadata, not a full-width empty reading strip.
                assert(bounds.width < surface.width * .8f);
                const auto inspect = [&](auto &&self, Node &node) -> void {
                    if (!node.visible())
                        return;
                    if (auto *text = dynamic_cast<TextNode *>(&node)) {
                        const auto metrics = renderer.measureText(
                            text->text(), text->fontSize(), text->font(), text->maxWidth(),
                            text->maxLines(), text->textAlign(), text->ellipsize(),
                            text->useMarkup());
                        float x, y;
                        Node::absolutePosition(text, x, y);
                        if (metrics.width > 0)
                            containedBounds({x + metrics.left, y + metrics.top, metrics.width,
                                             metrics.bottom - metrics.top},
                                            bounds);
                    }
                    for (const auto &child : node.children())
                        self(self, *child);
                };
                inspect(inspect, rendered.content);
            }
        }
        // A partially visible selection keeps the material geometry, not its clipped anchor.
        const auto rendered = body(view, "MaterialBody-2");
        dispatcher.setFocus(&rendered.row);
        view.arrange(renderer, viewport);
        auto *scroll = find<ScrollView>(view);
        const LayoutRect clipped{17, 23, 900, 110};
        // Focus alone does not reveal an offscreen row in this standalone host.
        // Materialize the same selected source under the clipped native viewport.
        assert(view.jumpToSourceMessage(buffer.messages[2].id));
        view.arrange(renderer, clipped);
        const auto resized = body(view, "MaterialBody-2");
        auto *resizedMaterial = find<blast::MaterialNode>(resized.content);
        assert(resizedMaterial);
        const auto materialBounds = sceneBounds(*resizedMaterial);
        const auto clip = nativeHistoryClip(view);
        scroll->setScrollOffset(scroll->scrollOffset() + materialBounds.y - clip.y +
                                materialBounds.height * .5f);
        view.arrange(renderer, clipped);
        const auto partial = body(view, "MaterialBody-2");
        RectNode *ring = nullptr;
        for (const auto &child : partial.row.children())
            if (auto *rect = dynamic_cast<RectNode *>(child.get()); rect && rect->visible() &&
                                                                    rect->style().fill.a == 0 &&
                                                                    rect->style().borderWidth > 0)
                ring = rect;
        assert(ring);
        const auto bounds = sceneBounds(*ring);
        const auto visible = intersectBounds(bounds, nativeHistoryClip(view));
        assert(visible.height > 0 && visible.height < bounds.height);
        near(bounds.height, find<blast::MaterialNode>(partial.content)->height());
        dispatcher.setFocus(nullptr);
        dispatcher.setSceneRoot(nullptr);
    }
}

void replyCardNativeGeometry(TranscriptTestRenderer &renderer) {
    for (const bool dense : {false, true}) {
        blast::Model model;
        model.settings.timestamps = model.settings.avatars = false;
        model.settings.reducedMotion = true;
        blast::Theme theme;
        theme.update(model);
        auto &buffer = model.buffers["reply-card-native"];
        buffer.id = buffer.target = "reply-card-native";
        buffer.kind = "channel";
        if (dense)
            model.settings.messageLayouts[buffer.id] = blast::MessageLayout::Irc;
        blast::Message parent;
        parent.id = "quoted-parent";
        parent.nick = "<b>世界_LongAuthor_оченьдлинноеимя_ABCDEFGHIJKLMNOPQRSTUVWXYZ</b>";
        parent.text =
            "\x02"
            "é世界\x02 \x1d"
            "مرحبا\n\xC2\x85\xE2\x80\xA8\xE2\x80\xA9\r\x1d & <literal> "
            "https://example.org/quoted Unicode 👩‍💻 excerpt wraps across several lines "
            "without displacing the message below or overflowing a narrow card.";
        const auto excerptPlain =
            "é世界 مرحبا" + std::string(10, ' ') +
            "& <literal> https://example.org/quoted Unicode 👩‍💻 excerpt wraps across "
            "several "
            "lines "
            "without displacing the message below or overflowing a narrow card.";
        buffer.messages.push_back(parent);
        blast::Message reply;
        reply.id = "reply";
        reply.nick = "Me";
        reply.own = true;
        reply.text = "OK";
        reply.reply = parent.id;
        for (int i = 0; i < 12; ++i) {
            auto filler = reply;
            filler.id = "between-" + std::to_string(i);
            filler.text = "Intervening-" + std::to_string(i);
            filler.reply.clear();
            buffer.messages.push_back(std::move(filler));
        }
        buffer.messages.push_back(reply);
        const auto original = buffer;
        blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
        Clipboard clipboard;
        std::vector<std::string> commands;
        Node root;
        root.setClipChildren(true);
        auto &view = *static_cast<blast::TranscriptView *>(
            root.addChild(std::make_unique<blast::TranscriptView>(
                model, pane, clipboard, [&](auto value) { commands.emplace_back(value); },
                [](auto) {})));
        view.setReadEligible(false);
        InputDispatcher dispatcher;
        dispatcher.setSceneRoot(&root);
        view.setInputDispatcher(dispatcher);
        for (const float width : {900.f, 240.f}) {
            pane.following = true;
            const LayoutRect viewport{17, 23, width, 380};
            root.setSize(viewport.x + viewport.width, viewport.y + viewport.height);
            view.sync();
            view.arrange(renderer, viewport);
            const auto rendered = body(view, "OK", true);
            InputArea *card = nullptr;
            const auto locate = [&](auto &&self, Node &node) -> void {
                if (auto *area = dynamic_cast<InputArea *>(&node);
                    area && area->focusable() &&
                    area->tabFocusKey().find("5:reply") != std::string::npos)
                    card = area;
                for (const auto &child : node.children())
                    self(self, *child);
            };
            locate(locate, rendered.content);
            assert(card);
            auto cardBounds = sceneBounds(*card);
            assert(cardBounds.width > rendered.fragment.width());
            assert(cardBounds.y + cardBounds.height <= sceneBounds(rendered.body).y + .02f);
            containedBounds(cardBounds, sceneBounds(*find<blast::MaterialNode>(rendered.content)));
            RectNode *outline = nullptr;
            for (const auto &child : card->children())
                if (auto *rect = dynamic_cast<RectNode *>(child.get());
                    rect && rect->visible() && rect->style().borderWidth > 0 &&
                    std::abs(sceneBounds(*rect).y - cardBounds.y) < .02f &&
                    std::abs(rect->width() - cardBounds.width) < .02f)
                    outline = rect;
            assert(outline && roundedCorners(outline->style().radius) &&
                   outline->style().border.a > 0);
            near(outline->height(), cardBounds.height);
            int authors = 0, excerpts = 0;
            const auto inspect = [&](auto &&self, Node &node) -> void {
                if (auto *text = dynamic_cast<TextNode *>(&node)) {
                    const auto metrics = renderer.measureText(
                        text->text(), text->fontSize(), text->font(), text->maxWidth(),
                        text->maxLines(), text->textAlign(), text->ellipsize(), text->useMarkup());
                    assert(metrics.lineCount >= 1 && metrics.lineCount <= 2);
                    float x, y;
                    Node::absolutePosition(text, x, y);
                    containedBounds({x + metrics.left, y + metrics.top, metrics.width,
                                     metrics.bottom - metrics.top},
                                    cardBounds);
                    if (text->useMarkup()) {
                        ++excerpts;
                        const auto plain = decodedMarkup(text->text());
                        assert(plain == excerptPlain);
                        auto expectedDocument = blast::parseIrcFormatting(parent.text, {}, {});
                        expectedDocument.text = excerptPlain;
                        const auto expected = blast::ircMarkup(expectedDocument);
                        near(renderer
                                 .measureText(text->text(), text->fontSize(), text->font(), 0, 0,
                                              TextAlign::Start, TextEllipsize::End, true)
                                 .width,
                             renderer
                                 .measureText(expected, text->fontSize(), text->font(), 0, 0,
                                              TextAlign::Start, TextEllipsize::End, true)
                                 .width);
                    } else {
                        ++authors;
                        assert(text->text() == parent.nick && metrics.lineCount == 1);
                        assert(text->fontWeight() == FontWeight::Bold);
                    }
                }
                for (const auto &child : node.children())
                    self(self, *child);
            };
            inspect(inspect, *card);
            assert(authors == 1 && excerpts == 1);
            // Tight layout may rematerialize the card; clipping alone must not resize it.
            const LayoutRect clipped{17, 23, width, 110};
            view.arrange(renderer, clipped);
            card = nullptr;
            locate(locate, body(view, "OK", true).content);
            assert(card);
            cardBounds = sceneBounds(*card);
            authors = excerpts = 0;
            inspect(inspect, *card);
            assert(authors == 1 && excerpts == 1);
            // Clip the real native ancestor, not an assumed typing-footer cutoff or
            // a scroll request beyond the newest row's maximum offset.
            const auto exposed = intersectBounds(cardBounds, nativeHistoryClip(view));
            assert(exposed.height > 0);
            const float clipBottom = exposed.y + exposed.height * .5f;
            root.setSize(viewport.x + viewport.width, clipBottom);
            card = nullptr;
            locate(locate, body(view, "OK", true).content);
            assert(card);
            const auto partial = sceneBounds(*card);
            near(partial.width, cardBounds.width);
            near(partial.height, cardBounds.height);
            const auto visible = intersectBounds(partial, ancestorPaintClip(*card));
            assert(visible.height > 0 && visible.height < partial.height);
            near(visible.y + visible.height, clipBottom);
            const float hiddenY =
                clipBottom + std::min(1.f, (exposed.y + exposed.height - clipBottom) * .5f);
            assert(dispatcher.inputAreaAt(partial.x + partial.width * .5f, hiddenY) != card);
            if (width > 240) {
                // Padding belongs to the same jump target, not just the excerpt glyphs.
                const float x = partial.x + 1, y = visible.y + visible.height * .5f;
                assert(dispatcher.pointerButton(x, y, BTN_LEFT, true, 301, 3010, false));
                dispatcher.pointerButton(x, y, BTN_LEFT, false, 302, 3020, false);
            } else {
                dispatcher.setFocus(card);
                dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
                dispatcher.keyEvent(XKB_KEY_Return, 0, 0, false);
            }
            assert(!pane.following && commands.empty() && model.actions.empty());
            root.setSize(viewport.x + viewport.width, viewport.y + viewport.height);
            view.arrange(renderer, viewport);
            near(sceneBounds(body(view, "é世界").body).y, viewport.y);
            unchanged(buffer, original);
        }
        dispatcher.setFocus(nullptr);
        dispatcher.setSceneRoot(nullptr);
    }
}

void foundryTranscriptAvatars(TranscriptTestRenderer &renderer) {
    std::array<uint64_t, 3> hashes{};
    for (size_t variant = 0; variant < hashes.size(); ++variant) {
        blast::Model model;
        model.settings.timestamps = false;
        model.settings.avatars = true;
        model.settings.avatarStyle = "sprite";
        model.settings.reducedMotion = true;
        blast::Theme theme;
        theme.update(model);
        auto &buffer = model.buffers["avatar-network/#room"];
        buffer.id = "avatar-network/#room";
        buffer.kind = "channel";
        buffer.server = "avatar-network";
        buffer.target = "#room";
        model.networks[buffer.server].caseMapping = variant == 2 ? "ascii" : "rfc1459";
        blast::Message entry;
        entry.id = "avatar-message";
        entry.nick = variant == 1 ? "{alice}" : "[Alice]";
        entry.text = "FoundryAvatarBody";
        buffer.messages.push_back(entry);
        blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
        Clipboard clipboard;
        blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
        view.setReadEligible(false);
        const auto arrange = [&] {
            view.sync();
            view.arrange(renderer, {17, 23, 700, 600});
        };
        arrange();
        auto rendered = body(view, "FoundryAvatarBody");
        auto *image = find<Image>(rendered.content);
        assert(image && image->visible() && image->hasImage());
        assert(image->sourceWidth() == 96 && image->sourceHeight() == 96);
        const auto avatarBounds = sceneBounds(*image);
        assert(avatarBounds.width > 0);
        near(avatarBounds.width, avatarBounds.height);
        containedBounds(avatarBounds, sceneBounds(rendered.row));
        for (const auto &child : rendered.content.children())
            if (const auto *rect = dynamic_cast<RectNode *>(child.get());
                rect && rect->visible() && rect->width() == image->width() &&
                rect->height() == image->height())
                assert(sceneBounds(*rect).x != avatarBounds.x ||
                       sceneBounds(*rect).y != avatarBounds.y);
        const auto texture = image->textureId();
        hashes[variant] = renderer.textures.rgbaHash(texture);
        const auto uploads = renderer.textures.uploads;
        arrange();
        assert(find<Image>(body(view, "FoundryAvatarBody").content)->textureId() == texture &&
               renderer.textures.uploads == uploads); // Stable rows do not recompose each frame.
        if (variant == 2) {
            model.networks[buffer.server].caseMapping = "rfc1459";
            arrange();
            auto *reclassified = find<Image>(body(view, "FoundryAvatarBody").content);
            assert(reclassified &&
                   renderer.textures.rgbaHash(reclassified->textureId()) == hashes[0]);
        }
        model.settings.avatarStyle = "monogram";
        arrange();
        assert(!find<Image>(body(view, "FoundryAvatarBody").content));
        bool matchingMonogramBox = false;
        for (const auto &child : body(view, "FoundryAvatarBody").content.children())
            if (const auto *rect = dynamic_cast<RectNode *>(child.get()); rect && rect->visible()) {
                const auto bounds = sceneBounds(*rect);
                matchingMonogramBox |= std::abs(bounds.width - avatarBounds.width) < .02f &&
                                       std::abs(bounds.height - avatarBounds.height) < .02f;
            }
        assert(matchingMonogramBox);
        model.settings.avatarStyle = "none";
        arrange();
        assert(!find<Image>(body(view, "FoundryAvatarBody").content));
    }
    assert(hashes[0] ==
           hashes[1]); // Network casemap-equivalent senders share actual raster pixels.
    assert(hashes[0] != hashes[2]); // ASCII preserves bracket spelling as a different identity.
}

void friendAuthorIndicators(TranscriptTestRenderer &renderer) {
    for (int mode = 0; mode < 5; ++mode) {
        blast::Model model;
        model.settings.timestamps = model.settings.avatars = false;
        model.settings.reducedMotion = true;
        model.settings.friends.insert("{a}");
        blast::Theme theme;
        theme.update(model);
        auto &source = model.buffers["friend-network/#room"];
        source.id = "friend-network/#room";
        source.server = "friend-network";
        source.target = "#room";
        source.kind = "channel";
        model.networks[source.server].caseMapping = "rfc1459";
        blast::Message entry;
        entry.id = "friend-message";
        entry.nick = "[A]";
        entry.text = "FriendBody selectable words";
        entry.action = mode == 2 || mode == 3;
        entry.mention = true;
        source.messages.push_back(entry);
        if (mode == 1 || mode == 3)
            model.settings.messageLayouts[source.id] = blast::MessageLayout::Irc;
        blast::Buffer *shown = &source;
        if (mode == 4) {
            model.mentionSources[source.id] = source;
            auto &feed = model.buffers[std::string(blast::GlobalMentionsBufferId)];
            feed.id = std::string(blast::GlobalMentionsBufferId);
            feed.kind = "mentions";
            auto aggregate = entry;
            aggregate.id = "friend-aggregate";
            aggregate.sourceBuffer = source.id;
            aggregate.sourceServer = source.server;
            aggregate.sourceTarget = source.target;
            aggregate.sourceId = entry.id;
            feed.messages.push_back(std::move(aggregate));
            shown = &feed;
            model.networks[""].caseMapping = "ascii"; // Never use the aggregate's network.
        }
        blast::PaneState pane{.id = 1, .buffer = shown->id, .reply = {}};
        Clipboard clipboard;
        blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
        view.setReadEligible(false);
        InputDispatcher dispatcher;
        dispatcher.setSceneRoot(&view);
        view.setInputDispatcher(dispatcher);
        const auto arrange = [&] {
            view.sync();
            view.arrange(renderer, {17, 23, 700, 600});
        };
        const auto indicator = [&]() {
            InputArea *found = nullptr;
            const auto visit = [&](auto &&self, Node &node) -> void {
                if (!node.visible())
                    return;
                if (auto *area = dynamic_cast<InputArea *>(&node)) {
                    const auto tooltip = area->tooltipContent();
                    if (const auto *text = std::get_if<std::string>(&tooltip);
                        text && *text == "Friend") {
                        assert(!found);
                        found = area;
                    }
                }
                for (const auto &child : node.children())
                    self(self, *child);
            };
            visit(visit, view);
            return found;
        };
        arrange();
        auto *badge = indicator();
        assert(badge);
        const auto *glyph = find<GlyphNode>(*badge);
        assert(glyph && glyph->visible() && glyph->codepoint() == GlyphRegistry::lookup("star"));
        const auto badgeBounds = sceneBounds(*badge);
        const auto rendered = body(view, "FriendBody");
        auto *actor = plainText(rendered.content, entry.action ? "↳ [A]" : "[A]");
        assert(actor);
        const auto actorBounds = sceneBounds(*actor);
        assert(actorBounds.x + actorBounds.width <= badgeBounds.x + .02f);
        if (mode == 2)
            assert(badgeBounds.x + badgeBounds.width + 7.9f <= sceneBounds(rendered.body).x);
        if (mode == 0)
            containedBounds(badgeBounds, sceneBounds(*bubbleSurface(rendered)));
        float x, y;
        Node::mapToScene(&rendered.fragment, 2, rendered.fragment.height() * .5f, x, y);
        dispatcher.pointerButton(x, y, BTN_LEFT, true, 301, 3010, false);
        dispatcher.pointerMotion(x + 30, y, 302);
        dispatcher.pointerButton(x + 30, y, BTN_LEFT, false, 303, 3030, false);
        assert(view.copySelection());
        const auto selected = clipboard.clipboardText();
        model.settings.friends.clear();
        arrange();
        assert(!indicator() && view.copySelection() && clipboard.clipboardText() == selected);
        model.settings.friends.insert("{a}");
        arrange();
        assert(indicator() && view.copySelection() && clipboard.clipboardText() == selected);
        model.networks[source.server].caseMapping = "ascii";
        arrange();
        assert(!indicator() && view.copySelection() && clipboard.clipboardText() == selected);
        model.settings.friends = {"[a]"};
        arrange();
        assert(indicator() && view.copySelection() && clipboard.clipboardText() == selected);
        model.settings.nickColors["{a}"] = "#ffffff";
        arrange(); // Keep the following casemap-only refresh free of color-setting reflow.
        model.networks[source.server].caseMapping = "rfc1459";
        arrange(); // Friendship stays true, including in the source-backed aggregate.
        const auto checkBadgeColor = [&] {
            auto *current = indicator();
            assert(current);
            const auto *nativeGlyph = find<GlyphNode>(*current);
            auto *currentActor =
                plainText(body(view, "FriendBody").content, entry.action ? "↳ [A]" : "[A]");
            assert(nativeGlyph && currentActor && nativeGlyph->color() == currentActor->color());
            assert(view.copySelection() && clipboard.clipboardText() == selected);
        };
        checkBadgeColor();
        model.settings.nickColors["{a}"] = "#000000";
        arrange();
        checkBadgeColor();
        if (mode == 4) {
            model.buffers.erase(source.id);
            model.mentionSources.clear();
            arrange();
            assert(!indicator() && !bodyText(view, "FriendBody"));
        }
        dispatcher.setFocus(nullptr);
        dispatcher.setSceneRoot(nullptr);
    }
}

void actionPresentation(TranscriptTestRenderer &renderer) {
    for (const int weight : {300, 400, 500, 600, 700})
        for (const bool traditional : {false, true}) {
            blast::Model model;
            model.settings.timestamps = true;
            model.settings.avatars = false;
            model.settings.reducedMotion = true;
            model.settings.friends = {"A", std::string(80, 'W')};
            model.settings.presence = blast::PresenceMode::All;
            model.settings.messageFont = "monospace";
            model.settings.uiFont = "sans-serif";
            model.settings.messageFontWeight = weight;
            model.settings.uiFontWeight = weight == 700 ? 300 : 700;
            blast::Theme theme;
            theme.update(model);
            auto &buffer = model.buffers["actions"];
            buffer.id = buffer.target = "actions";
            buffer.kind = "channel";
            buffer.server = "network";
            if (traditional)
                model.settings.messageLayouts[buffer.id] = blast::MessageLayout::Irc;
            const std::string raw = "ActionPlain \x02"
                                    "ActionBold\x02 \x1f"
                                    "ActionUnder\x1f \x03"
                                    "04ActionRed\x0f ActionReset \x11"
                                    "ActionMono\x11 😀👩‍💻";
            for (int i = 0; i < 4; ++i) {
                blast::Message message;
                message.id = "action-" + std::to_string(i);
                message.nick = i < 2 ? "A" : "Peer";
                message.text = i < 2 ? raw : i == 2 ? "NormalBody" : "PresenceBody";
                message.action = i < 2;
                message.own = i == 1;
                message.presence = i == 3;
                message.time = "2026-11-07T12:34:56Z";
                buffer.messages.push_back(std::move(message));
            }
            // Give each emote a unique body locator without changing its style controls.
            buffer.messages[1].text.replace(0, 11, "OwnEmote");
            const auto before = buffer;
            Clipboard clipboard;
            MenuHost host;
            blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
            blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
            view.setReadEligible(false);
            view.setPopupContext(&host);
            InputDispatcher dispatcher;
            dispatcher.setSceneRoot(&view);
            view.setInputDispatcher(dispatcher);
            const auto arrange = [&] {
                view.sync();
                view.arrange(renderer, {17, 23, 1100, 1800});
            };
            arrange();
            for (const auto token : {"ActionPlain", "OwnEmote"}) {
                const auto rendered = body(view, token);
                auto *actor = plainText(rendered.content, "↳ A");
                assert(actor && actor->font().weight == FontWeight::Bold);
                const float actorNatural =
                    renderer.measureText(actor->text(), actor->fontSize(), actor->font()).width;
                assert(actor->maxWidth() + .02f >= actorNatural);
                assert(actor->color() == blast::nickColor(model, "A", buffer.server));
                assert(!bubbleSurface(rendered));
                // No filled row-sized substitute card may replace the material bubble.
                for (const auto &child : rendered.content.children())
                    if (const auto *rect = dynamic_cast<RectNode *>(child.get());
                        rect && rect->visible())
                        assert(rect->style().fill.a == 0);
                std::string markup;
                for (const auto &area : rendered.body.children())
                    for (const auto &child : area->children())
                        if (const auto *text = dynamic_cast<TextNode *>(child.get());
                            text && text->useMarkup())
                            markup += text->text();
                assert(markup.find("style=\"italic\"") != std::string::npos);
                assert(markup.find("weight=\"bold\" style=\"italic\">ActionBold") !=
                       std::string::npos);
                assert(markup.find("style=\"italic\" underline=\"single\">ActionUnder") !=
                       std::string::npos);
                assert(markup.find("style=\"italic\" foreground=\"" +
                                   formatRgbHex(rgbHex(blast::ircPalette[4])) + "\">ActionRed") !=
                       std::string::npos);
                assert(markup.find("style=\"italic\"> ActionReset") != std::string::npos);
                assert(markup.find("font_family=\"monospace\"") != std::string::npos);
                assert(markup.find("ActionMono") != std::string::npos);
                assert(markup.find("😀👩‍💻") != std::string::npos);
                if (!traditional) {
                    auto *clock = bubbleFooter(rendered.content, 1);
                    assert(clock &&
                           clock->color() == blast::themeForegroundColor("on_surface_variant"));
                }
                float x, y;
                Node::mapToScene(&rendered.fragment, rendered.fragment.width() * .5f,
                                 rendered.fragment.height() * .5f, x, y);
                dispatcher.pointerButton(x, y, BTN_RIGHT, true, 901, 9010, false);
                dispatcher.pointerButton(x, y, BTN_RIGHT, false, 902, 9020, false);
                assert(host.open);
                host.select("Copy text");
                const auto &message =
                    buffer.messages[token == std::string_view("OwnEmote") ? 1 : 0];
                assert(clipboard.clipboardText() ==
                       blast::parseIrcFormatting(message.text, {}, {}).text);
            }
            const auto normal = body(view, "NormalBody");
            assert(!plainText(normal.content, "↳ Peer"));
            assert(normal.text.text().find("style=\"italic\"") == std::string::npos);
            assert(bool(bubbleSurface(normal)) == !traditional);
            const auto presence = body(view, "PresenceBody");
            assert(!plainText(presence.content, "↳ Peer") && !bubbleSurface(presence));
            model.settings.avatars = true;
            for (const float width : {1100.f, 240.f}) {
                view.sync();
                view.arrange(renderer, {17, 23, width, 1800});
                for (const auto token : {"ActionPlain", "OwnEmote"}) {
                    const auto rendered = body(view, token);
                    auto *actor = plainText(rendered.content, "↳ A");
                    assert(actor);
                    const auto actorBounds = sceneBounds(*actor);
                    const float actorNatural =
                        renderer.measureText(actor->text(), actor->fontSize(), actor->font()).width;
                    assert(actor->maxWidth() + .02f >= actorNatural);
                    const auto bodyBounds = sceneBounds(rendered.body);
                    if (!traditional)
                        assert(actorBounds.x + actorBounds.width + 7.9f <= bodyBounds.x);
                    else if (width > 240) {
                        assert(actorBounds.x + actorBounds.width + 7.9f <= bodyBounds.x);
                        near(baseline(*actor), baseline(rendered.text));
                    } else {
                        assert(sceneBounds(*actor->parent()).y +
                                   sceneBounds(*actor->parent()).height <=
                               bodyBounds.y + .02f);
                        near(sceneBounds(*actor->parent()).x, bodyBounds.x);
                    }
                    for (const auto &child : rendered.content.children())
                        if (auto *rect = dynamic_cast<RectNode *>(child.get());
                            rect && rect->visible() && rect->style().fill.a > 0 &&
                            rect->width() >= 12 &&
                            std::abs(rect->width() - rect->height()) < .02f) {
                            const auto avatarBounds = sceneBounds(*rect);
                            assert(intersectBounds(actorBounds, avatarBounds).width <= .02f ||
                                   intersectBounds(actorBounds, avatarBounds).height <= .02f);
                            const auto bodyBounds = sceneBounds(rendered.body);
                            assert(intersectBounds(bodyBounds, avatarBounds).width <= .02f ||
                                   intersectBounds(bodyBounds, avatarBounds).height <= .02f);
                        }
                    if (auto *image = find<Image>(rendered.content)) {
                        const auto avatarBounds = sceneBounds(*image);
                        assert(intersectBounds(actorBounds, avatarBounds).width <= .02f ||
                               intersectBounds(actorBounds, avatarBounds).height <= .02f);
                        assert(intersectBounds(bodyBounds, avatarBounds).width <= .02f ||
                               intersectBounds(bodyBounds, avatarBounds).height <= .02f);
                    }
                }
            }
            const std::string longActor(80, 'W');
            buffer.messages.front().nick = longActor;
            ++buffer.messagesRevision;
            view.sync();
            view.arrange(renderer, {17, 23, 240, 1800});
            const auto narrow = body(view, "ActionPlain");
            auto *boundedActor = plainText(narrow.content, "↳ " + longActor);
            assert(boundedActor);
            assert(boundedActor->maxWidth() < renderer
                                                  .measureText(boundedActor->text(),
                                                               boundedActor->fontSize(),
                                                               boundedActor->font())
                                                  .width);
            const auto bounded = sceneBounds(*boundedActor);
            const auto rowBounds = sceneBounds(narrow.row);
            assert(bounded.x >= rowBounds.x - .02f &&
                   bounded.x + bounded.width <= rowBounds.x + rowBounds.width + .02f);
            if (!traditional)
                assert(bounded.x + bounded.width + 7.9f <= sceneBounds(narrow.body).x);
            buffer.messages.front().nick = "A";
            ++buffer.messagesRevision;
            model.settings.avatars = false;
            arrange();
            model.settings.fools.insert("A");
            for (const auto privacy : {blast::FoolDisplay::Hidden, blast::FoolDisplay::Blurred}) {
                model.settings.foolDisplay = privacy;
                arrange();
                assert(!view.messageReadable("message:action-0"));
                assert(view.messageReadable("message:action-1")); // Own messages remain readable.
                assert(view.messageReadable("message:action-2"));
                if (privacy == blast::FoolDisplay::Hidden)
                    assert(!bodyText(view, "ActionPlain") && bodyText(view, "OwnEmote"));
                else
                    assert(body(view, "ActionPlain").body.foregroundBlurRadius() > 0);
                assert(!view.copySelection());
            }
            unchanged(buffer, before);
            assert(model.actions.empty());
            dispatcher.setFocus(nullptr);
            dispatcher.setSceneRoot(nullptr);
        }
}

void nativeReactionRow(TranscriptTestRenderer &renderer) {
    for (const int mode : {0, 1, 2})
        for (const float scale : {1.f, 2.f}) {
            blast::Model model;
            model.settings.scale = model.settings.messageScale = scale;
            model.settings.uiFont = "DejaVu Serif";
            model.settings.uiFontWeight = 700;
            model.settings.messageFontWeight = 300;
            model.settings.timestamps = false;
            model.settings.avatars = true;
            model.settings.avatarStyle = "monogram";
            model.settings.reducedMotion = true;
            blast::Theme theme;
            theme.update(model);
            auto &buffer = model.buffers["reaction-row"];
            buffer.id = buffer.target = "reaction-row";
            buffer.kind = "channel";
            buffer.server = "network";
            if (mode)
                model.settings.messageLayouts[buffer.id] =
                    mode == 1 ? blast::MessageLayout::Irc : blast::MessageLayout::Stacked;
            auto &network = model.networks[buffer.server];
            network.state = "Ready";
            network.nick = "Me";
            network.caps = {"message-tags"};
            for (const bool own : {false, true}) {
                blast::Message entry;
                entry.id = own ? "own-reactions" : "peer-reactions";
                entry.text = own ? "OwnReactionBody" : "PeerReactionBody";
                entry.nick = own ? "Me" : "Peer";
                entry.own = own;
                for (const auto *emoji :
                     {"☀", "☁", "☂", "☃", "☄", "★", "☆", "☕", "☘", "☠", "☮", "☯",
                      "👨‍👩‍👧‍👦👨‍👩‍👧‍👦👨‍👩‍👧‍"
                      "👦"})
                    entry.reactions[emoji] = {"Peer"};
                entry.reactions["☂"] = std::vector<std::string>(10000, "Peer");
                buffer.messages.push_back(std::move(entry));
            }
            auto before = buffer;
            blast::PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
            Clipboard clipboard;
            blast::TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
            view.setReadEligible(false);
            InputDispatcher dispatcher;
            dispatcher.setSceneRoot(&view);
            view.setInputDispatcher(dispatcher);
            float width = 1000 * scale;
            const auto arrange = [&] {
                view.sync();
                view.arrange(renderer, {17, 23, width, 1400 * scale});
            };
            const auto buttons = [&](const blast::Message &entry) {
                const auto rendered = body(view, entry.text);
                std::vector<Button *> result;
                for (const auto &child : rendered.content.children())
                    if (auto *button = dynamic_cast<Button *>(child.get())) {
                        assert(button->visible() && button->enabled());
                        result.push_back(button);
                    }
                return result;
            };
            const auto checkRow = [&](const blast::Message &entry) {
                const auto rendered = body(view, entry.text);
                auto *scroll = find<ScrollView>(view);
                assert(scroll && rendered.row.parent());
                // The row's real canvas excludes the reserved scrollbar gutter.
                const auto canvas = sceneBounds(*rendered.row.parent());
                assert(canvas.width <= scroll->contentViewportWidth() + .02f);
                const auto actual = buttons(entry);
                const bool bubbles = blast::messageBubbles(model.settings, buffer);
                const bool tight = bubbles && width < 400 * scale;
                const float inset = (tight ? 4 : blast::presentationMetrics().inset) * scale;
                float budget = rendered.body.width();
                if (bubbles) {
                    const float lane =
                        tight ? 0 : (blast::presentationMetrics().avatarSize + 10) * scale;
                    const float available = std::max(1.f, canvas.width - 2 * inset - lane);
                    budget = std::min(available, std::max(240 * scale, available * .9f)) -
                             2 * (tight ? 4 : 12) * scale;
                }
                const auto bodyBounds = sceneBounds(rendered.body);
                budget =
                    std::max(0.f, std::min(budget, canvas.x + canvas.width - inset - bodyBounds.x));
                std::size_t count = 0;
                float extent = 0;
                for (const auto &[emoji, users] : entry.reactions) {
                    const auto label = emoji + " " + std::to_string(users.size());
                    const float chipWidth =
                        renderer.measureText(label, 14 * scale, blast::themeUiFont()).width +
                        18 * scale;
                    const float x = count ? extent + 5 * scale : 0;
                    if (x + chipWidth > budget)
                        break;
                    assert(count < actual.size());
                    auto *button = actual[count];
                    assert(button->label()->text() == label &&
                           !button->inputArea()->tabFocusKey().empty());
                    assert(button->label()->font() == blast::themeUiFont());
                    const auto painted = sceneBounds(*button);
                    const auto hit = sceneBounds(*button->inputArea());
                    near(painted.x, bodyBounds.x + x);
                    near(painted.width, chipWidth);
                    near(hit.x, painted.x);
                    near(hit.y, painted.y);
                    near(hit.width, painted.width);
                    near(hit.height, painted.height);
                    containedBounds(painted, nativeHistoryClip(view));
                    containedBounds(hit, nativeHistoryClip(view));
                    assert(painted.x + painted.width <= canvas.x + canvas.width + .02f);
                    if (count) {
                        const auto previous = sceneBounds(*actual[count - 1]);
                        near(painted.y, previous.y);
                        near(hit.y, sceneBounds(*actual[count - 1]->inputArea()).y);
                        near(painted.x - previous.x - previous.width, 5 * scale);
                    }
                    if (bubbles)
                        containedBounds(painted, sceneBounds(*bubbleSurface(rendered)));
                    extent = x + chipWidth;
                    ++count;
                }
                assert(actual.size() == count);
                if (bubbles && count) {
                    assert(rendered.body.width() + .02f >= extent);
                    // A short body must not force its bubble to the maximum width.
                    if (budget - extent > scale)
                        assert(rendered.body.width() < budget - .02f);
                }
                assert(entry.reactions == before.messages[entry.own ? 1 : 0].reactions);
                return count;
            };
            arrange();
            const auto &entry = buffer.messages.front();
            const auto wide = checkRow(entry);
            assert(wide > 1);
            checkRow(buffer.messages.back());
            for (const int uiWeight : {400, 700, 400}) {
                model.settings.uiFontWeight = uiWeight;
                theme.update(model);
                arrange();
                checkRow(entry);
                checkRow(buffer.messages.back());
                unchanged(buffer, before);
                assert(model.settings.messageFontWeight == 300);
            }
            pane.focusComposer = false;
            const std::string firstKey(buttons(entry).front()->inputArea()->tabFocusKey());
            dispatcher.setFocus(buttons(entry).front()->inputArea());
            width = 240 * scale;
            arrange();
            const auto narrow = checkRow(entry);
            assert(narrow > 0 && narrow < wide);
            checkRow(buffer.messages.back());
            auto *surviving = buttons(entry).front();
            assert(dispatcher.focusedArea() == surviving->inputArea() &&
                   surviving->inputArea()->tabFocusKey() == firstKey);
            float x, y;
            Node::mapToScene(surviving->inputArea(), surviving->width() * .5f,
                             surviving->height() * .5f, x, y);
            assert(dispatcher.pointerButton(x, y, BTN_LEFT, true, 701, 7010, false));
            dispatcher.pointerButton(x, y, BTN_LEFT, false, 702, 7020, false);
            assert(model.actions.size() == 1 && model.actions.back().type == "react" &&
                   model.actions.back().buffer == buffer.id &&
                   model.actions.back().extra == entry.id &&
                   model.actions.back().text == entry.reactions.begin()->first);
            model.actions.clear();
            width = 1000 * scale;
            arrange();
            const std::string removedKey(buttons(entry).back()->inputArea()->tabFocusKey());
            dispatcher.setFocus(buttons(entry).back()->inputArea());
            width = 240 * scale;
            arrange();
            checkRow(entry);
            auto *focused = dispatcher.focusedArea();
            assert(focused && focused->tabFocusKey() != removedKey);
            bool attached = false;
            for (const Node *node = focused; node; node = node->parent()) {
                assert(node->visible());
                attached |= node == &view;
            }
            assert(attached);
            unchanged(buffer, before);
            dispatcher.setFocus(nullptr);
            // The first oversized key sorts before a short key that would fit.
            for (auto &message : buffer.messages) {
                message.reactions.clear();
                message.reactions[std::string(300, '!') + "👨‍👩‍👧‍👦"] = {"Peer"};
                message.reactions["👍"] = {"Peer"};
            }
            ++buffer.messagesRevision;
            before = buffer;
            arrange();
            for (const auto &message : buffer.messages)
                assert(checkRow(message) == 0);
            unchanged(buffer, before);
            model.settings.presence = blast::PresenceMode::All;
            for (auto &message : buffer.messages) {
                message.presence = true;
                message.text = message.own ? "QR" : "PR";
                message.reactions = {{"☀", {"Peer"}}, {"☁", {"Peer"}}};
            }
            // Own JOINs are projected out; PART keeps typed activity geometry visible for both
            // authors.
            for (const auto activity : {blast::ActivityKind::None, blast::ActivityKind::Part}) {
                for (auto &message : buffer.messages)
                    message.activity = activity;
                ++buffer.messagesRevision;
                before = buffer;
                arrange();
                for (const auto &message : buffer.messages) {
                    const auto rendered = body(view, message.text);
                    float right = 0;
                    std::size_t count = 0;
                    for (const auto &child : rendered.content.children())
                        if (auto *button = dynamic_cast<Button *>(child.get())) {
                            const auto painted = sceneBounds(*button);
                            containedBounds(painted, nativeHistoryClip(view));
                            containedBounds(sceneBounds(*button->inputArea()),
                                            nativeHistoryClip(view));
                            right = std::max(right, painted.x + painted.width);
                            ++count;
                        }
                    assert(count == 2);
                    const auto bounded = sceneBounds(rendered.body);
                    assert(right <= bounded.x + bounded.width + .02f);
                }
                unchanged(buffer, before);
            }
            assert(model.actions.empty());
            dispatcher.setFocus(nullptr);
            dispatcher.setSceneRoot(nullptr);
        }
}

void independentMessageScales(TranscriptTestRenderer &renderer) {
    using namespace blast;
    Model model;
    model.settings.avatars = false;
    model.settings.timestamps = true;
    model.settings.reducedMotion = true;
    auto &buffer = model.buffers["scale/#room"];
    buffer.id = buffer.target = "scale/#room";
    buffer.kind = "channel";
    Message entry;
    entry.id = "scaled-message";
    entry.nick = "ScaleNick";
    entry.time = "2026-11-07T12:34:56Z";
    entry.text = "ScaleBody é 猫😀 retains selection while native wrapping and live reflow "
                 "change independently from interface geometry. More words fill several lines.";
    entry.reactions["👍"] = {"ScaleNick"};
    buffer.messages.push_back(entry);
    const auto before = buffer;
    Theme theme;
    Clipboard clipboard;
    PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}};
    TranscriptView view(model, pane, clipboard, [](auto) {}, [](auto) {});
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&view);
    view.setInputDispatcher(dispatcher);
    view.setReadEligible(false);
    const auto arrange = [&] {
        theme.update(model);
        view.sync();
        view.arrange(renderer, {17, 23, 420, 1600});
    };
    for (const bool traditional : {false, true}) {
        view.clearTextSelection();
        if (traditional)
            model.settings.messageLayouts[buffer.id] = blast::MessageLayout::Irc;
        else
            model.settings.messageLayouts.erase(buffer.id);
        model.settings.scale = model.settings.messageScale = 1;
        arrange();
        const auto rendered = body(view, "ScaleBody");
        const auto &text = rendered.text;
        std::vector<TextCursorStop> stops;
        renderer.measureTextCaretStops(text.text(), text.fontSize(), {0, 9}, 0, stops, text.font(),
                                       true);
        assert(stops.size() == 2);
        const auto metrics = renderer.measureText(text.text(), text.fontSize(), text.font(), 0, 0,
                                                  TextAlign::Start, TextEllipsize::End, true);
        float x, y;
        Node::absolutePosition(&text, x, y);
        const auto point = [&](const TextCursorStop &stop) {
            return std::pair{x - metrics.alignOffsetX + stop.x,
                             y - std::floor(metrics.layoutBaseline) + stop.y + stop.height * .5f};
        };
        const auto start = point(stops.front()), end = point(stops.back());
        assert(dispatcher.pointerButton(start.first, start.second, BTN_LEFT, true, 91, 910, false));
        dispatcher.pointerMotion(end.first, end.second, 92);
        dispatcher.pointerButton(end.first, end.second, BTN_LEFT, false, 93, 930, false);
        assert(view.copySelection() && clipboard.clipboardText() == "ScaleBody");
        model.settings.uiFont = model.settings.messageFont = "DejaVu Serif";
        model.settings.uiFontWeight = model.settings.messageFontWeight = 400;
        arrange();
        const auto regularFont = themeMessageFont();
        const auto regularWidth = renderer.measureText("Weight measurement", 24, regularFont).width;
        model.settings.messageFontWeight = 700;
        arrange();
        const auto boldFont = themeMessageFont();
        const auto boldWidth = renderer.measureText("Weight measurement", 24, boldFont).width;
        assert(boldWidth > regularWidth);
        const float threshold = (regularWidth + boldWidth) * .5f;
        const auto regularWrap =
            renderer.measureText("Weight measurement", 24, regularFont, threshold);
        const auto boldWrap = renderer.measureText("Weight measurement", 24, boldFont, threshold);
        assert(regularWrap.lineCount == 1 && boldWrap.lineCount > regularWrap.lineCount);
        assert(boldWrap.layoutHeight > regularWrap.layoutHeight);
        float regularBodyHeight = 0;
        for (const int messageWeight : {400, 700, 400}) {
            float independentWidth = 0;
            for (const int uiWeight : {400, 700, 400}) {
                model.settings.messageFontWeight = messageWeight;
                model.settings.uiFontWeight = uiWeight;
                arrange();
                const auto current = body(view, "ScaleBody");
                assert(static_cast<int>(current.text.font().weight) == messageWeight);
                assert(static_cast<int>(themeUiFont().weight) == uiWeight);
                const float width =
                    renderer.measureText("Weight measurement", 24, current.text.font()).width;
                near(width, messageWeight == 400 ? regularWidth : boldWidth);
                near(renderer.measureText("Weight measurement", 24, themeUiFont()).width,
                     uiWeight == 400 ? regularWidth : boldWidth);
                const auto wrapped =
                    renderer.measureText("Weight measurement", 24, current.text.font(), threshold);
                assert(wrapped.lineCount ==
                       (messageWeight == 400 ? regularWrap.lineCount : boldWrap.lineCount));
                if (independentWidth)
                    near(width, independentWidth);
                independentWidth = width;
                if (messageWeight == 400 && regularBodyHeight)
                    near(current.fragment.height(), regularBodyHeight);
                if (messageWeight == 400)
                    regularBodyHeight = current.fragment.height();
                assert(view.copySelection() && clipboard.clipboardText() == "ScaleBody");
                unchanged(buffer, before); // Includes combining text and emoji; no raster claim.
                assert(model.actions.empty());
            }
        }
        model.settings.uiFontWeight = model.settings.messageFontWeight = 400;
        float smallWidth = 0;
        for (const float messageScale : {.7f, 2.f}) {
            float messageWidth = 0;
            for (const float interfaceScale : {.7f, 2.f}) {
                model.settings.scale = interfaceScale;
                model.settings.messageScale = messageScale;
                arrange();
                const auto current = body(view, "ScaleBody");
                near(current.text.fontSize(), messageTextSize(model.settings, buffer));
                const auto glyph =
                    renderer.measureText("Mg猫", current.text.fontSize(), current.text.font());
                if (messageWidth)
                    near(glyph.width, messageWidth);
                messageWidth = glyph.width;
                assert(current.fragment.height() > 0 && current.row.height() > 0);
                assert(view.copySelection() && clipboard.clipboardText() == "ScaleBody");
                auto *nick = plainText(current.content, "ScaleNick");
                auto *reaction = find<Button>(current.content);
                assert(nick && reaction);
                near(nick->fontSize(),
                     traditional ? 14 * messageScale : current.text.fontSize() * .82f);
                near(reaction->label()->fontSize(), 14 * messageScale);
                unchanged(buffer, before);
                assert(model.actions.empty());
            }
            if (smallWidth)
                assert(messageWidth > smallWidth);
            smallWidth = messageWidth;
        }
    }
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void runTranscriptCheck() {
    codecs::installImageDecoder();
    ConfigDirectory config;
    TranscriptTestRenderer renderer;
    linkPreviewMenus(renderer);
    nativeReactionRow(renderer);
    independentMessageScales(renderer);
    messageSelectionGeometry(renderer);
    selectedContentGeometry(renderer);
    replyCardNativeGeometry(renderer);
    actionPresentation(renderer);
    friendAuthorIndicators(renderer);
    foundryTranscriptAvatars(renderer);
    bubbleNickBoundaries(renderer);
    visitSeparator(renderer);
    messageEntranceLifecycle(renderer);
    independentSendEffects(renderer);
    cappedTranscriptBodies(renderer);
    typingFooterGeometry(renderer);
    transcriptPointerCursors(renderer);
    sourceMentionConsumers(renderer);
    searchSourceConsumers(renderer);
    retainedMentionFooter(renderer);
    foolTranscriptPolicy(renderer);
    blurredBodyAndReplyPrivacy(renderer);
    smartPresenceNickTransfer(renderer);
    smartPresenceRedactionInvalidation(renderer);
    contentFitAndConsecutiveGroups(renderer);
    inlineNicknameChips(renderer);
    for (const float zoom : {1.f, 2.f})
        for (const bool traditional : {false, true})
            check(renderer, zoom, traditional);
    smoothWheel(renderer);
    smoothWheel(renderer, "wheel");
    smoothWheel(renderer, "uncommitted-wheel-rename");
    messageIdentityRevision(renderer);
    messageNavigation(renderer, false);
    messageNavigation(renderer, true);
    messageLinkEnter(renderer);
    messageTextSelection(renderer, 1);
    messageTextSelection(renderer, 2);
    emptyLineSelection(renderer, 1);
    emptyLineSelection(renderer, 2);
    channelActivity(renderer, 1);
    channelActivity(renderer, 2);
    smartPresenceEligibility(renderer);
    ownJoinProjection(renderer);
    tailAppendKeepsActions(renderer);
    focusRevealIntents(renderer);
    mergeArrivalBaseline(renderer);
    renameArrivalBaseline(renderer);
    rowGapReflow(renderer, false);
    rowGapReflow(renderer, true);
    for (const char *kind : {"channel", "query"}) {
        tightBubbleViewport(renderer, kind);
        compactMeaningfulBodies(renderer, kind);
    }
    keyboardMenus(renderer);
    keyboardMenus(renderer, true);
    keyboardMenus(renderer, false, "query");
    for (const float zoom : {1.f, 2.f}) {
        traditionalDecorations(renderer, zoom);
        traditionalClocks(renderer, zoom);
        channelConversation(renderer, zoom);
        for (const char *kind : {"channel", "query"})
            bubbleConversation(renderer, zoom, kind);
    }
}
