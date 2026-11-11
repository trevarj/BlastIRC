#include "../src/application.hpp"
#include "../src/model.hpp"
#include "../src/render/graphics.hpp"
#include "../src/storage.hpp"
#include "../src/theme/theme.hpp"
#include "../src/ui/buffer_chrome.hpp"
#include "../src/ui/dialogs.hpp"
#include "../src/ui/emoji_picker.hpp"
#include "../src/ui/mode_line.hpp"
#include "../src/ui/presentation.hpp"
#include "../src/ui/sidebar.hpp"
#include "../src/ui/transcript.hpp"
#include "../src/ui/ui.hpp"
#include <algorithm>
#include <array>
#include <cairo.h>
#include <cassert>
#include <charconv>
#include <cmath>
#include <core/input/key_modifiers.h>
#include <core/input/key_symbols.h>
#include <core/text_clipboard.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <glib.h>
#include <iterator>
#include <optional>
#include <render/codecs/codecs.h>
#include <render/core/texture_manager.h>
#include <render/scene/input_dispatcher.h>
#include <render/scene/rect_node.h>
#include <render/text/cairo_text_renderer.h>
#include <string>
#include <string_view>
#include <ui/controls/button.h>
#include <ui/controls/image.h>
#include <ui/controls/input.h>
#include <ui/controls/label.h>
#include <ui/controls/scroll_view.h>
#include <ui/controls/select.h>
#include <ui/controls/select_popup_context.h>
#include <ui/controls/spinner.h>
#include <ui/palette.h>
#include <utility>
#include <variant>
#include <vector>

namespace {
// Same CPU-only native layout adapter as transcript_check: real text and icon
// decoding, with texture uploads retained only as handles, not GPU pixels.
class CpuTextureUploadSink final : public TextureManager {
  public:
    TextureHandle loadFromRgba(const std::uint8_t *data, int width, int height, bool) override {
        return upload(data, width, height);
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
    std::uint32_t uploadCount() const { return uploads; }

  private:
    std::uint32_t uploads = 0;
    TextureHandle upload(const std::uint8_t *data, int width, int height) {
        assert(data && width > 0 && height > 0);
        return {.id = TextureId(++uploads),
                .width = width,
                .height = height,
                .sourceWidth = width,
                .sourceHeight = height};
    }
};

class PangoRenderer final : public Renderer {
  public:
    PangoRenderer() { text.initialize(nullptr, nullptr); }
    TextMetrics measureText(std::string_view value, float size, const TextFont &font, float width,
                            int lines, TextAlign align, TextEllipsize ellipsize,
                            bool markup) override {
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
    std::uint32_t uploadCount() const { return textures.uploadCount(); }

  private:
    CpuTextureUploadSink textures;
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

class MenuHost final : public SelectPopupContext {
  public:
    DropdownRequest request;
    DropdownCallbacks callbacks;
    bool open = false;
    void openSelectDropdown(const DropdownRequest &value, DropdownCallbacks handlers) override {
        request = value;
        callbacks = std::move(handlers);
        open = true;
    }
    void closeSelectDropdown() override {
        open = false;
        callbacks = {};
    }
    bool isSelectDropdownOpen() const override { return open; }
    void select(std::string_view label) {
        assert(open && callbacks.onSelect);
        std::size_t value;
        if (!request.menu.empty()) {
            const auto findLeaf =
                [&](auto &&self,
                    const std::vector<ContextMenuEntry> &entries) -> const ContextMenuEntry * {
                for (const auto &entry : entries) {
                    if (!entry.enabled || entry.separator || entry.header)
                        continue;
                    if (!entry.children.empty()) {
                        if (const auto *leaf = self(self, entry.children))
                            return leaf;
                    } else if (entry.label == label)
                        return &entry;
                }
                return nullptr;
            };
            const auto *leaf = findLeaf(findLeaf, request.menu);
            assert(leaf);
            value = leaf->value;
        } else {
            const auto found = std::find(request.options.begin(), request.options.end(), label);
            assert(found != request.options.end());
            value = std::size_t(found - request.options.begin());
        }
        auto select = std::move(callbacks.onSelect);
        closeSelectDropdown();
        select(value);
    }
};

class Clipboard final : public TextClipboard {
  public:
    std::optional<std::string> clipboardText() override { return std::nullopt; }
    void setClipboardText(std::string) override {}
};

Input *inputWithText(Node &node, const std::string &text) {
    if (auto *input = dynamic_cast<Input *>(&node); input && input->value() == text)
        return input;
    for (const auto &child : node.children())
        if (auto *input = inputWithText(*child, text))
            return input;
    return nullptr;
}

InputArea *areaWithKey(Node &node, std::string_view key) {
    if (auto *area = dynamic_cast<InputArea *>(&node); area && area->tabFocusKey() == key)
        return area;
    for (const auto &child : node.children())
        if (auto *area = areaWithKey(*child, key))
            return area;
    return nullptr;
}

InputArea *followNewestControl(Node &node) {
    if (auto *area = dynamic_cast<InputArea *>(&node);
        area && area->tabFocusKey().ends_with(":follow-newest"))
        return area;
    for (const auto &child : node.children())
        if (auto *area = followNewestControl(*child))
            return area;
    return nullptr;
}

InputArea *composerControl(Node &node, std::string_view role) {
    if (auto *area = dynamic_cast<InputArea *>(&node);
        area && area->tabFocusKey().starts_with("composer:") && area->tabFocusKey().ends_with(role))
        return area;
    for (const auto &child : node.children())
        if (auto *area = composerControl(*child, role))
            return area;
    return nullptr;
}

InputArea *areaWithTooltip(Node &node, std::string_view text) {
    if (auto *area = dynamic_cast<InputArea *>(&node); area && area->focusable()) {
        const auto tooltip = area->tooltipContent();
        if (const auto *value = std::get_if<std::string>(&tooltip); value && *value == text)
            return area;
    }
    for (const auto &child : node.children())
        if (auto *area = areaWithTooltip(*child, text))
            return area;
    return nullptr;
}

Node *paneFor(Input &input) {
    for (auto *node = input.parent(); node; node = node->parent())
        for (const auto &child : node->children())
            if (dynamic_cast<blast::TranscriptView *>(child.get()))
                return node;
    assert(false && "Composer must belong to a real Ui conversation pane");
    return nullptr;
}

bool visible(const Node &node) {
    for (const auto *ancestor = &node; ancestor; ancestor = ancestor->parent())
        if (!ancestor->visible())
            return false;
    return true;
}

template <class T> T *visibleNode(Node &node) {
    if (!visible(node))
        return nullptr;
    if (auto *found = dynamic_cast<T *>(&node))
        return found;
    for (const auto &child : node.children())
        if (auto *found = visibleNode<T>(*child))
            return found;
    return nullptr;
}

void hostKey(blast::Ui &ui, InputDispatcher &dispatcher, std::uint32_t sym,
             std::uint32_t modifiers = 0, std::uint32_t utf32 = 0) {
    for (const bool pressed : {true, false}) {
        if (pressed && !KeySymbol::isModifier(sym))
            assert(ui.synchronizeInput(sym));
        InputArea::KeyData event{};
        event.sym = sym;
        event.utf32 = utf32;
        event.modifiers = modifiers;
        event.pressed = pressed;
        if (!ui.handleKey(event))
            dispatcher.keyEvent(sym, utf32, modifiers, pressed);
    }
}

void nativeKey(blast::Ui &ui, InputDispatcher &dispatcher, std::uint32_t sym,
               std::uint32_t modifiers = 0, std::uint32_t utf32 = 0) {
    for (const bool pressed : {true, false})
        if (!ui.handleKey({.sym = sym, .utf32 = utf32, .modifiers = modifiers, .pressed = pressed}))
            dispatcher.keyEvent(sym, utf32, modifiers, pressed);
}

std::array<float, 2> pointerPoint(const Node &node) {
    std::array<float, 2> point{};
    Node::mapToScene(&node, node.width() / 4, node.height() / 2, point[0], point[1]);
    return point;
}

void pointerButton(InputDispatcher &dispatcher, const std::array<float, 2> &point,
                   std::uint32_t button, bool pressed) {
    static std::uint32_t serial = 0;
    ++serial;
    assert(
        dispatcher.pointerButton(point[0], point[1], button, pressed, serial, serial * 10, false));
}

void click(InputDispatcher &dispatcher, const Node &node, std::uint32_t button = BTN_LEFT) {
    const auto point = pointerPoint(node);
    pointerButton(dispatcher, point, button, true);
    pointerButton(dispatcher, point, button, false);
}

Label *labelWithText(Node &node, std::string_view text) {
    if (auto *label = dynamic_cast<Label *>(&node); label && label->text() == text)
        return label;
    for (const auto &child : node.children())
        if (auto *label = labelWithText(*child, text))
            return label;
    return nullptr;
}

Label *visibleLabelContaining(Node &node, std::string_view text) {
    if (!visible(node))
        return nullptr;
    if (auto *label = dynamic_cast<Label *>(&node);
        label && label->text().find(text) != std::string::npos)
        return label;
    for (const auto &child : node.children())
        if (auto *label = visibleLabelContaining(*child, text))
            return label;
    return nullptr;
}

Label *paintedHintAt(Node &node, const blast::SidebarHintRect &row) {
    if (!visible(node))
        return nullptr;
    if (auto *label = dynamic_cast<Label *>(&node)) {
        float x = 0, y = 0;
        Node::mapToScene(label, label->width() / 2, label->height() / 2, x, y);
        if (x >= row.x && x < row.x + row.width && y >= row.y && y < row.y + row.height)
            return label;
    }
    for (const auto &child : node.children())
        if (auto *label = paintedHintAt(*child, row))
            return label;
    return nullptr;
}

Button *primaryButton(Node &node) {
    if (!visible(node))
        return nullptr;
    if (auto *button = dynamic_cast<Button *>(&node);
        button && button->variant() == ButtonVariant::Primary)
        return button;
    for (const auto &child : node.children())
        if (auto *button = primaryButton(*child))
            return button;
    return nullptr;
}

InputArea *memberRow(Node &node, std::string_view nick) {
    if (auto *area = dynamic_cast<InputArea *>(&node))
        for (const auto &child : area->children())
            if (auto *label = dynamic_cast<Label *>(child.get()); label && label->text() == nick)
                return area;
    for (const auto &child : node.children())
        if (auto *area = memberRow(*child, nick))
            return area;
    return nullptr;
}

std::vector<std::array<float, 5>> selectionGeometry(const Node &node) {
    std::vector<std::array<float, 5>> geometry;
    for (const auto &child : node.children())
        if (const auto *rect = dynamic_cast<const RectNode *>(child.get()); rect && rect->visible())
            geometry.push_back(
                {rect->x(), rect->y(), rect->width(), rect->height(), rect->style().borderWidth});
    return geometry;
}

constexpr std::array edgeBuffers{"test/first", "test/second", "test/third", "test/fourth"};
constexpr std::array edgeDrafts{"first draft", "second draft", "third draft", "fourth draft"};

blast::Model edgeModel() {
    blast::Model model;
    model.firstRun = false;
    model.now = 100;
    model.settings.retentionDays = 0;
    model.settings.reducedMotion = true;
    model.settings.typingSend = false;
    model.settings.peopleMode = blast::SidebarMode::Hidden;
    model.identities.emplace_back();
    blast::ServerConfig server;
    server.name = "test";
    server.host = "example.invalid";
    model.servers.push_back(server);
    model.networks["test"].nick = "Me";
    for (std::size_t i = 0; i < edgeBuffers.size(); ++i) {
        auto &buffer = model.buffers[edgeBuffers[i]];
        buffer.id = edgeBuffers[i];
        buffer.server = "test";
        buffer.target = std::string(edgeBuffers[i]).substr(5);
        buffer.kind = "query";
        buffer.draft = edgeDrafts[i];
    }
    model.activeBuffer = edgeBuffers[0];
    model.settings.workspace["selected_server"] = "test";
    return model;
}

template <class T> int visibleCount(Node &node) {
    if (!node.visible())
        return 0;
    int count = dynamic_cast<T *>(&node) ? 1 : 0;
    for (const auto &child : node.children())
        count += visibleCount<T>(*child);
    return count;
}

blast::BufferTitleLine *headerFor(Input &input) {
    for (const auto &child : paneFor(input)->children())
        if (auto *header = dynamic_cast<blast::BufferTitleLine *>(child.get()))
            return header;
    assert(false && "Every conversation keeps its header");
    return nullptr;
}

InputArea *headerTool(Input &input, std::string_view actionLabel) {
    for (const auto &child : headerFor(input)->children())
        if (auto *area = dynamic_cast<InputArea *>(child.get()); area && area->focusable()) {
            const auto tooltip = area->tooltipContent();
            if (const auto *text = std::get_if<std::string>(&tooltip);
                text && text->find(actionLabel) != std::string::npos)
                return area;
        }
    assert(false && "Expected the conversation's actual header tool");
    return nullptr;
}

void presentation(blast::Ui &ui, const blast::Model &model, std::string_view active,
                  blast::SidebarMode people,
                  blast::SidebarMode navigation = blast::SidebarMode::Expanded) {
    using blast::SidebarMode;
    assert(visibleCount<blast::TranscriptView>(ui.root()) == 1);
    assert(visibleCount<blast::Sidebar>(ui.root()) == int(navigation != SidebarMode::Hidden));
    assert(visibleCount<blast::People>(ui.root()) == int(people != SidebarMode::Hidden));
    assert(visibleCount<blast::BufferTitleLine>(ui.root()) == 1);
    const auto geometry = [&](auto &&self, Node &node) -> void {
        if (visible(node)) {
            if (dynamic_cast<blast::Sidebar *>(&node))
                assert(node.width() ==
                       (navigation == SidebarMode::Expanded ? 250 : 56) * model.settings.scale);
            if (dynamic_cast<blast::People *>(&node))
                assert(node.width() ==
                       (people == SidebarMode::Expanded ? 220 : 56) * model.settings.scale);
        }
        for (const auto &child : node.children())
            self(self, *child);
    };
    geometry(geometry, ui.root());
    assert(model.activeBuffer == active);
    assert(model.settings.workspace.at("visible_buffers") == active);
}

void minimumWindowHeightCheck() {
    using namespace blast;
    Model model;
    Theme theme;
    PangoRenderer renderer;
    theme.update(model);
    assert(themeScale() == 1 && presentationMetrics().fontSize == 15);
    assert(minimumWindowHeight(renderer) == 480);
    model.settings.scale = .7f;
    theme.update(model);
    assert(minimumWindowHeight(renderer) == 480);
    model.settings.scale = 2;
    theme.update(model);
    assert(minimumWindowHeight(renderer) >= 960);
    model.settings.scale = .7f;
    model.settings.messageScale = 2;
    theme.update(model);
    const auto largeMessageMinimum = minimumWindowHeight(renderer);
    const float defaultRow = renderer.fontRowExtent(15, themeUiFont());
    assert(
        largeMessageMinimum ==
        static_cast<std::uint32_t>(std::ceil(
            480.f * std::max(1.f, renderer.fontRowExtent(30, themeMessageFont()) / defaultRow))));
    assert(largeMessageMinimum > 480 && themeScale() == .7f && themeMessageScale() == 2.f);
    model.settings.messageScale = .7f;
    theme.update(model);
    assert(minimumWindowHeight(renderer) == 480);
    model.settings.messageScale = 1;
    model.settings.scale = 1;
    theme.update(model);
    const float baseline = renderer.fontRowExtent(15, themeMessageFont());
    assert(baseline > 0);
    // Compare resolved native metrics, not family names: unavailable fonts may alias.
    std::vector<std::pair<float, std::uint32_t>> samples;
    for (const auto *family :
         {"sans-serif", "serif", "monospace", "DejaVu Serif", "Liberation Serif"}) {
        model.settings.uiFont = family;
        model.settings.messageFont.clear();
        theme.update(model);
        const float row = renderer.fontRowExtent(15, themeUiFont());
        const auto uiMinimum = minimumWindowHeight(renderer);
        assert(uiMinimum >= 480);
        assert(row > baseline ? uiMinimum > 480 : uiMinimum == 480);
        model.settings.uiFont.clear();
        model.settings.messageFont = family;
        theme.update(model);
        assert(renderer.fontRowExtent(15, themeMessageFont()) == row);
        const auto messageMinimum = minimumWindowHeight(renderer);
        assert(messageMinimum == uiMinimum);
        for (const auto &[previousRow, previousMinimum] : samples) {
            if (row >= previousRow)
                assert(messageMinimum >= previousMinimum);
            if (row <= previousRow)
                assert(messageMinimum <= previousMinimum);
        }
        samples.emplace_back(row, messageMinimum);
    }
}

void retainedBufferStateCheck(bool directMessage, blast::SidebarMode peoplePreference) {
    using namespace blast;
    constexpr std::array buffers{"test/#channel", "test/#second", "test/#third", "test/#fourth"};
    constexpr std::array drafts{"channel draft", "second draft", "third draft", "fourth draft"};
    std::string destination = directMessage ? "test/person" : "test/*server*";
    const std::string destinationDraft = directMessage ? "private draft" : "server draft";
    Clipboard clipboard;
    {
        Storage storage;
        Model model;
        storage.load(model);
        model = Model{};
        model.firstRun = false;
        model.now = 100;
        model.settings.retentionDays = 0;
        model.settings.reducedMotion = true;
        model.settings.typingSend = false;
        model.settings.peopleMode = peoplePreference;
        model.identities.emplace_back();
        ServerConfig server;
        server.name = "test";
        server.host = "example.invalid";
        model.servers.push_back(server);
        model.networks["test"].name = "Retained editor regression network";
        model.networks["test"].nick = "Me";
        for (std::size_t i = 0; i < drafts.size(); ++i) {
            auto &buffer = model.buffers[buffers[i]];
            buffer.id = buffers[i];
            buffer.server = "test";
            buffer.target = std::string(buffers[i]).substr(5);
            buffer.draft = drafts[i];
            buffer.kind = "channel";
            buffer.joined = true;
            buffer.nicks.push_back({.name = "Person", .account = {}, .prefix = {}});
        }
        auto &target = model.buffers[destination];
        target.id = destination;
        target.server = "test";
        target.target = directMessage ? "Person" : "*server*";
        target.kind = directMessage ? "query" : "server";
        target.draft = destinationDraft;
        if (directMessage) {
            Message incoming;
            incoming.id = "fresh-query-message";
            incoming.nick = "Person";
            incoming.text = "Opening this conversation must acknowledge its visible transcript.";
            incoming.time = "2026-11-07T13:43:05.545Z";
            incoming.timestamp = 1794058985545000000LL;
            target.messages.push_back(std::move(incoming));
            ++target.messagesRevision;
            target.unread = 1;
        }
        model.activeBuffer = buffers[0];
        Theme theme;
        theme.update(model);
        InputDispatcher dispatcher;
        PangoRenderer renderer;
        Ui ui(model, clipboard);
        dispatcher.setSceneRoot(&ui.root());
        ui.setInputDispatcher(dispatcher);
        ui.setWindowActive(true);
        ui.update(2000, 1200, 1);
        presentation(ui, model, buffers[0], peoplePreference);
        std::array<Input *, 4> inputs{};
        std::array<Node *, 4> panes{};
        std::array<Input::EditingState, 4> states;
        std::array<Input::EditHistory, 4> histories;
        const auto documentMatches = [](const Input &input, const Input::EditingState &expected) {
            const auto actual = input.editingState();
            // Native layout may clamp caret scrolling, never the retained document.
            return actual.current == expected.current &&
                   actual.preeditBase == expected.preeditBase &&
                   actual.preeditStart == expected.preeditStart &&
                   actual.preeditLength == expected.preeditLength &&
                   actual.preeditCaretHidden == expected.preeditCaretHidden &&
                   actual.history == expected.history;
        };
        for (std::size_t i = 0; i < drafts.size(); ++i) {
            ui.execute("buffer:" + std::string(buffers[i]));
            inputs[i] = inputWithText(ui.root(), drafts[i]);
            assert(inputs[i] && visible(*inputs[i]));
            panes[i] = paneFor(*inputs[i]);
            assert(inputs[i]->insertText("!"));
            ui.execute("undo");
            auto state = inputs[i]->editingState();
            state.current.cursorPos = 3;
            state.current.selectionAnchor = 1;
            state.scrollY = float(17 * (i + 1));
            inputs[i]->restoreEditingState(state);
            states[i] = inputs[i]->editingState();
            histories[i] = *states[i].history;
            assert(!states[i].history->redo.empty());
        }
        ui.execute("buffer:" + std::string(buffers[0]));
        const auto channelUndo = states[0].history->undo;
        const auto channelRedo = states[0].history->redo;
        auto *retainedSearch = headerTool(*inputs[0], "Search conversation");
        assert(retainedSearch);
        const auto unchanged = [&] {
            assert(model.activeBuffer == buffers[0]);
            assert(model.settings.peopleMode == peoplePreference);
            for (std::size_t i = 0; i < inputs.size(); ++i) {
                assert(inputWithText(ui.root(), drafts[i]) == inputs[i]);
                assert(documentMatches(*inputs[i], states[i]) &&
                       states[i].history->undo == histories[i].undo &&
                       states[i].history->redo == histories[i].redo);
                assert(visible(*panes[i]) == (i == 0));
            }
            assert(visible(*headerFor(*inputs[0])));
            auto *search = headerTool(*inputs[0], "Search conversation");
            auto *more = headerTool(*inputs[0], "More conversation actions");
            // Narrow native headers intentionally fold Search into More. The
            // responsive-header consumer below verifies that menu action and focus.
            assert(search == retainedSearch && more && visible(*more) && more->focusable() &&
                   (more->acceptedButtons() & InputArea::buttonMask({BTN_LEFT})) != 0 &&
                   (visible(*search) || search->excludeSubtreeFromTabOrder()));
        };
        // Scale changes and compact navigation retain each real native editor.
        for (const float scale : {.7f, 1.f, 2.f}) {
            model.settings.scale = scale;
            theme.update(model);
            const auto minimumHeight = minimumWindowHeight(renderer);
            assert(minimumHeight >= (scale == 2 ? 960u : 480u));
            ui.update(720, minimumHeight, 1);
            ui.root().layout(renderer);
            unchanged();
            ui.update(2000, 1200, 1);
            unchanged();
            ui.execute("toggle-navigation");
            unchanged();
            assert(visibleCount<Sidebar>(ui.root()) == 1);
            ui.execute("toggle-navigation");
            unchanged();
            assert(visibleCount<Sidebar>(ui.root()) == 0);
            ui.execute("toggle-navigation");
            unchanged();
        }
        // Live family changes use the same declared minimum as startup and keep editors.
        for (const auto *family : {"sans-serif", "serif", "monospace"}) {
            model.settings.uiFont = family;
            model.settings.messageFont = family;
            theme.update(model);
            ui.update(720, minimumWindowHeight(renderer), 1);
            ui.root().layout(renderer);
            unchanged();
        }
        model.settings.uiFont = model.settings.messageFont = "DejaVu Serif";
        for (const int messageWeight : {400, 700, 400})
            for (const int uiWeight : {400, 700, 400}) {
                model.settings.uiFontWeight = uiWeight;
                model.settings.messageFontWeight = messageWeight;
                theme.update(model);
                ui.update(720, minimumWindowHeight(renderer), 1);
                ui.root().layout(renderer);
                unchanged();
                assert(static_cast<int>(themeUiFont().weight) == uiWeight);
                assert(static_cast<int>(themeMessageFont().weight) == messageWeight);
                for (std::size_t i = 0; i < inputs.size(); ++i) {
                    auto *text = labelWithText(*inputs[i], drafts[i]);
                    assert(text && text->font() == themeMessageFont());
                    assert(renderer
                               .measureText(text->text(), text->fontSize(), text->font(), 0, 0,
                                            TextAlign::Start, TextEllipsize::End, false)
                               .width ==
                           renderer
                               .measureText(drafts[i], text->fontSize(), themeMessageFont(), 0, 0,
                                            TextAlign::Start, TextEllipsize::End, false)
                               .width);
                }
            }
        model.settings.uiFont.clear();
        model.settings.messageFont.clear();
        model.settings.scale = 1;
        theme.update(model);
        ui.update(2000, 1200, 1);
        unchanged();
        ui.execute("buffer:" + destination);
        auto *active = inputWithText(ui.root(), destinationDraft);
        assert(active && active != inputs[0] && visible(*active));
        presentation(ui, model, destination, SidebarMode::Hidden);
        assert(model.settings.peopleMode == peoplePreference);
        if (directMessage) {
            // Visible composition must establish actual read eligibility, not just a caret.
            model.now += 1;
            ui.update(2000, 1200, 1);
            ui.root().layout(renderer);
            model.now += 1;
            ui.update(2000, 1200, 1);
            ui.root().layout(renderer);
            assert(dispatcher.focusedArea() == active->inputArea());
            assert(
                std::any_of(model.actions.begin(), model.actions.end(), [&](const Action &action) {
                    return action.type == "read" && action.buffer == destination;
                }));
        }
        assert(active->insertText("!"));
        ui.execute("undo");
        auto destinationState = active->editingState();
        destinationState.current.cursorPos = 4;
        destinationState.current.selectionAnchor = 1;
        destinationState.scrollY = 29;
        active->restoreEditingState(destinationState);
        destinationState = active->editingState();
        const auto destinationUndo = destinationState.history->undo;
        const auto destinationRedo = destinationState.history->redo;
        assert(!destinationRedo.empty());
        ui.execute("buffer:" + std::string(buffers[0]));
        assert(inputWithText(ui.root(), drafts[0]) == inputs[0] && visible(*inputs[0]));
        assert(documentMatches(*inputs[0], states[0]) &&
               dispatcher.focusedArea() == inputs[0]->inputArea());
        assert(states[0].history->undo == channelUndo && states[0].history->redo == channelRedo);
        // After a legitimate offset clamp, the native insertion rectangle still
        // belongs to the actual visible text viewport (not an inactive caret node).
        const Node *viewport = nullptr;
        for (const auto &child : inputs[0]->children())
            if (child->clipChildren())
                viewport = child.get();
        assert(viewport && visible(*viewport));
        const auto caret = inputs[0]->textInputState();
        float left, top, right, bottom;
        Node::mapToScene(viewport, 0, 0, left, top);
        Node::mapToScene(viewport, viewport->width(), viewport->height(), right, bottom);
        assert(caret.cursorRectWidth > 0 && caret.cursorRectHeight > 0 &&
               caret.cursorRectX >= std::min(left, right) - .5f &&
               caret.cursorRectY >= std::min(top, bottom) - .5f &&
               caret.cursorRectX + caret.cursorRectWidth <= std::max(left, right) + .5f &&
               caret.cursorRectY + caret.cursorRectHeight <= std::max(top, bottom) + .5f);
        presentation(ui, model, buffers[0], peoplePreference);
        ui.execute("buffer:" + std::string(buffers[1]));
        assert(model.activeBuffer == buffers[1] && visible(*inputs[1]));
        ui.execute("buffer:" + destination);
        assert(active->editingStateMatches(destinationState) && visible(*active));

        if (directMessage) {
            static_cast<TextInputClient &>(*active).textInputApplyEdit({.commitText = {},
                                                                        .preeditText = "猫",
                                                                        .preeditCursorBegin = 3,
                                                                        .preeditCursorEnd = 3,
                                                                        .hasPreedit = true});
            const auto composing = active->editingState();
            assert(active->composing() && model.bufferRenamed);
            const auto oldId = destination;
            destination = "test/renamed-person";
            auto renamed = model.buffers.extract(oldId);
            renamed.key() = renamed.mapped().id = destination;
            renamed.mapped().target = "RenamedPerson";
            model.buffers.insert(std::move(renamed));
            model.activeBuffer = destination;
            model.bufferRenamed(oldId, destination);
            assert(model.settings.workspace.at("visible_buffers") == destination);
            assert(active->editingStateMatches(composing));
            assert(dispatcher.focusedArea() == active->inputArea());
            assert(composing.history->undo == destinationUndo &&
                   composing.history->redo == destinationRedo);
            static_cast<TextInputClient &>(*active).textInputResetPreedit();
            assert(active->editingStateMatches(destinationState));
        }
        const auto actions = model.actions.size();
        ui.execute("buffer:" + std::string(buffers[3]));
        assert(inputWithText(ui.root(), drafts[3]) == inputs[3] && visible(*inputs[3]));
        assert(documentMatches(*inputs[3], states[3]) &&
               states[3].history->undo == histories[3].undo &&
               states[3].history->redo == histories[3].redo);
        ui.execute("buffer:" + destination);
        assert(std::none_of(model.actions.begin() + actions, model.actions.end(),
                            [](const Action &action) { return action.type == "part"; }));
        assert(active->editingStateMatches(destinationState));
        presentation(ui, model, destination, SidebarMode::Hidden);
        ui.flush();
        assert(model.buffers.at(destination).draft == destinationDraft);
        for (std::size_t i = 0; i < drafts.size(); ++i)
            assert(model.buffers.at(buffers[i]).draft == drafts[i]);
        const auto frame = [&](float width = 2000, float height = 1200) {
            ui.update(width, height, 1);
            ui.root().layout(renderer);
        };
        // Complete retained-document checks before testing laid-out panel geometry.
        ui.execute("buffer:" + std::string(buffers[0]));
        frame();
        const auto panelEditingState = inputs[0]->editingState();
        const auto panelState = [&](SidebarMode people, SidebarMode navigation) {
            frame();
            presentation(ui, model, buffers[0], people, navigation);
            auto *identity =
                labelWithText(*headerFor(*inputs[0]), model.buffers.at(buffers[0]).target);
            assert(identity && visible(*identity) == (navigation != SidebarMode::Expanded));
            auto *channel = areaWithKey(ui.root(), std::string("buffer:") + buffers[0]);
            assert(channel && visible(*channel) == (navigation != SidebarMode::Hidden));
            auto *channelName = labelWithText(*channel, model.buffers.at(buffers[0]).target);
            assert(channelName && visible(*channelName) == (navigation == SidebarMode::Expanded));
            auto *person = memberRow(ui.root(), "Person");
            if (people == SidebarMode::Hidden) {
                assert(!person || !visible(*person));
            } else {
                assert(person && visible(*person));
                auto *nick = labelWithText(*person, "Person");
                assert(nick && visible(*nick) == (people == SidebarMode::Expanded));
                if (people == SidebarMode::Compact)
                    assert(person->width() <= 56 && person->height() > 0);
            }
            assert(inputs[0]->editingStateMatches(panelEditingState));
            assert(model.buffers.at(buffers[0]).draft == drafts[0]);
        };
        const auto peopleCycle =
            peoplePreference == SidebarMode::Expanded
                ? std::array{SidebarMode::Compact, SidebarMode::Hidden, SidebarMode::Expanded}
            : peoplePreference == SidebarMode::Compact
                ? std::array{SidebarMode::Hidden, SidebarMode::Expanded, SidebarMode::Compact}
                : std::array{SidebarMode::Expanded, SidebarMode::Compact, SidebarMode::Hidden};
        // Commands and the actual native tools both traverse all three presentations.
        for (const bool button : {false, true}) {
            for (const auto navigation :
                 {SidebarMode::Compact, SidebarMode::Hidden, SidebarMode::Expanded}) {
                if (button) {
                    auto *toggle = areaWithKey(ui.root(), "topbar-navigation");
                    assert(toggle && visible(*toggle));
                    click(dispatcher, *toggle);
                } else {
                    ui.execute("toggle-navigation");
                }
                panelState(peoplePreference, navigation);
                unchanged();
            }
            for (const auto people : peopleCycle) {
                if (button)
                    click(dispatcher, *headerTool(*inputs[0], "People"));
                else
                    ui.execute("toggle-people");
                panelState(people, SidebarMode::Expanded);
                assert(model.settings.peopleMode == people);
            }
            unchanged();
        }
        for (int step = 0; step < 3 && model.settings.peopleMode != SidebarMode::Expanded; ++step)
            ui.execute("toggle-people");
        frame();
        auto *navigationToggle = areaWithKey(ui.root(), "topbar-navigation");
        auto *peopleToggle = headerTool(*inputs[0], "People");
        assert(navigationToggle && visible(*navigationToggle) && visible(*peopleToggle));
        // Both completed clicks queue before one native update; each must advance the mode.
        click(dispatcher, *navigationToggle);
        click(dispatcher, *navigationToggle);
        click(dispatcher, *peopleToggle);
        click(dispatcher, *peopleToggle);
        frame();
        panelState(SidebarMode::Hidden, SidebarMode::Hidden);
        assert(model.settings.workspace.at("native_navigation_mode") == "hidden");
        assert(model.settings.peopleMode == SidebarMode::Hidden);
        ui.execute("toggle-navigation");
        for (int step = 0; step < 3 && model.settings.peopleMode != peoplePreference; ++step)
            ui.execute("toggle-people");
        frame();
        unchanged();
        // Responsive fitting is presentation only; widening restores each preferred mode.
        frame(720, 480);
        assert(model.settings.peopleMode == peoplePreference);
        assert(model.settings.workspace.at("native_navigation_mode") == "expanded");
        frame();
        panelState(peoplePreference, SidebarMode::Expanded);
        unchanged();
        frame(360, 480);
        presentation(ui, model, buffers[0], SidebarMode::Hidden, SidebarMode::Compact);
        assert(model.settings.peopleMode == peoplePreference);
        for (const auto constrainedPeople : peopleCycle) {
            ui.execute("toggle-people");
            frame(360, 480);
            assert(model.settings.peopleMode == constrainedPeople);
            assert(visibleCount<People>(ui.root()) == 0);
            assert(inputs[0]->editingStateMatches(panelEditingState));
        }
        ui.execute("toggle-navigation");
        frame(360, 480);
        assert(model.settings.workspace.at("native_navigation_mode") == "hidden");
        assert(visibleCount<Sidebar>(ui.root()) == 0);
        ui.execute("toggle-navigation");
        frame();
        panelState(peoplePreference, SidebarMode::Expanded);
        ui.execute("toggle-navigation");
        ui.execute("toggle-navigation");
        panelState(peoplePreference, SidebarMode::Hidden);
        frame(720, 480);
        assert(visibleCount<Sidebar>(ui.root()) == 0);
        frame();
        panelState(peoplePreference, SidebarMode::Hidden);
        ui.execute("toggle-navigation");
        unchanged();
        // A temporary Hidden→Compact jump still reserves real space for the transcript.
        ui.execute("toggle-navigation");
        ui.execute("toggle-navigation");
        for (int step = 0; step < 3 && model.settings.peopleMode != SidebarMode::Expanded; ++step)
            ui.execute("toggle-people");
        frame(720, 480);
        presentation(ui, model, buffers[0], SidebarMode::Expanded, SidebarMode::Hidden);
        for (const bool cancel : {true, false}) {
            dispatcher.setFocus(inputs[0]->inputArea());
            ui.execute("jump-channel");
            frame(720, 480);
            presentation(ui, model, buffers[0], SidebarMode::Compact, SidebarMode::Compact);
            auto *jump = areaWithKey(ui.root(), "channel-jump");
            assert(jump && visible(*jump));
            bool checkedTranscriptWidth = false;
            for (const auto &child : paneFor(*inputs[0])->children())
                if (auto *transcript = dynamic_cast<TranscriptView *>(child.get())) {
                    assert(visible(*transcript) && transcript->width() >= 360);
                    checkedTranscriptWidth = true;
                }
            assert(checkedTranscriptWidth);
            assert(model.settings.peopleMode == SidebarMode::Expanded);
            assert(model.settings.workspace.at("native_navigation_mode") == "hidden");
            if (cancel) {
                nativeKey(ui, dispatcher, XKB_KEY_Escape);
            } else {
                auto *row = areaWithKey(ui.root(), std::string("buffer:") + buffers[1]);
                Sidebar *sidebar = nullptr;
                for (Node *node = row; node; node = node->parent())
                    if (auto *candidate = dynamic_cast<Sidebar *>(node))
                        sidebar = candidate;
                assert(sidebar);
                Label *choice = nullptr;
                for (const auto &hint : sidebar->hintRects())
                    if (hint.buffer == buffers[1])
                        choice = paintedHintAt(*jump, hint);
                assert(choice && visible(*choice));
                const auto keys = choice->text();
                for (const auto key : keys) {
                    nativeKey(ui, dispatcher, key, 0, key);
                    frame(720, 480);
                }
            }
            frame(720, 480);
            const auto selected = cancel ? 0 : 1;
            presentation(ui, model, buffers[selected], SidebarMode::Expanded, SidebarMode::Hidden);
            assert(!visible(*jump) && dispatcher.focusedArea() == inputs[selected]->inputArea());
            assert(inputs[0]->editingStateMatches(panelEditingState));
            assert(model.settings.peopleMode == SidebarMode::Expanded);
            assert(model.settings.workspace.at("native_navigation_mode") == "hidden");
            ui.execute("buffer:" + std::string(buffers[0]));
            frame(720, 480);
        }
        for (int step = 0; step < 3 && model.settings.peopleMode != peoplePreference; ++step)
            ui.execute("toggle-people");
        ui.execute("toggle-navigation");
        frame();
        unchanged();

        ui.execute("buffer:" + destination);
        assert(documentMatches(*active, destinationState) &&
               destinationState.history->undo == destinationUndo &&
               destinationState.history->redo == destinationRedo);
        ui.execute("buffer:" + std::string(buffers[0]));
        for (int step = 0; step < 3 && model.settings.peopleMode != SidebarMode::Expanded; ++step)
            ui.execute("toggle-people");
        frame();
        // A resize delivered with a queued click determines the choice, not the preceding frame.
        click(dispatcher, *navigationToggle);
        frame(720, 480);
        presentation(ui, model, buffers[0], SidebarMode::Expanded, SidebarMode::Hidden);
        assert(model.settings.workspace.at("native_navigation_mode") == "hidden");
        assert(model.settings.peopleMode == SidebarMode::Expanded);
        ui.execute("toggle-navigation");
        frame(720, 480);
        if (directMessage) {
            // A preceding queued DM switch frees left-panel space before the next toggle.
            auto *query = areaWithKey(ui.root(), "buffer:" + destination);
            assert(query && visible(*query));
            click(dispatcher, *query);
            click(dispatcher, *navigationToggle);
            frame(720, 480);
            presentation(ui, model, destination, SidebarMode::Hidden, SidebarMode::Compact);
            assert(model.settings.workspace.at("native_navigation_mode") == "compact");
            assert(model.settings.peopleMode == SidebarMode::Expanded);
            assert(model.buffers.at(destination).draft == destinationDraft);
            ui.execute("toggle-navigation");
            ui.execute("toggle-navigation");
        } else {
            ui.execute("buffer:" + destination);
        }
        for (int step = 0; step < 3 && model.settings.peopleMode != peoplePreference; ++step)
            ui.execute("toggle-people");
        frame();
        ui.execute("toggle-navigation");
        ui.execute("toggle-navigation");
        assert(model.settings.workspace.at("native_navigation_mode") == "hidden");
        storage.save(model);
        dispatcher.setFocus(nullptr);
        dispatcher.setSceneRoot(nullptr);
    }
    {
        Storage storage;
        Model restarted;
        storage.load(restarted);
        assert(!restarted.firstRun && restarted.activeBuffer == destination);
        assert(restarted.settings.peopleMode == peoplePreference);
        assert(restarted.settings.workspace.at("native_navigation_mode") == "hidden");
        Theme theme;
        theme.update(restarted);
        InputDispatcher dispatcher;
        Ui ui(restarted, clipboard);
        dispatcher.setSceneRoot(&ui.root());
        ui.setInputDispatcher(dispatcher);
        ui.setWindowActive(true);
        ui.update(2000, 1200, 1);
        presentation(ui, restarted, destination, SidebarMode::Hidden, SidebarMode::Hidden);
        auto *active = inputWithText(ui.root(), destinationDraft);
        assert(active && visible(*active));
        for (std::size_t i = 0; i < drafts.size(); ++i) {
            assert(restarted.buffers.at(buffers[i]).draft == drafts[i]);
            ui.execute("buffer:" + std::string(buffers[i]));
            auto *input = inputWithText(ui.root(), drafts[i]);
            assert(input && visible(*input) && !visible(*active));
        }
        ui.execute("buffer:" + std::string(buffers[0]));
        presentation(ui, restarted, buffers[0], peoplePreference, SidebarMode::Hidden);
        assert(restarted.settings.peopleMode == peoplePreference);
        ui.flush();
        dispatcher.setFocus(nullptr);
        dispatcher.setSceneRoot(nullptr);
    }
}

void nativePanelResizeCheck() {
    using namespace blast;
    for (const float scale : {1.f, 1.5f}) {
        Storage storage;
        Model model;
        storage.load(model);
        model = edgeModel();
        model.settings.scale = scale;
        model.settings.workspace["native_navigation_mode"] = "expanded";
        Clipboard clipboard;
        Theme theme;
        theme.update(model);
        InputDispatcher dispatcher;
        PangoRenderer renderer;
        Ui ui(model, clipboard);
        dispatcher.setSceneRoot(&ui.root());
        ui.setInputDispatcher(dispatcher);
        ui.setWindowActive(true);
        const auto frame = [&](float width = 2400.f) {
            ui.update(width, 1200, 1);
            ui.applyPendingResize();
            ui.root().layout(renderer);
        };
        frame();
        auto *logo = visibleNode<Image>(ui.root());
        assert(logo && logo->hasImage());
        const int logoSourceWidth = logo->sourceWidth();
        const int logoSourceHeight = logo->sourceHeight();
        assert(logoSourceWidth > 320 && logoSourceHeight > 320);
        const auto logoGeometry = [&] {
            const auto width = model.settings.workspace.find("server_rail_width");
            const float railWidth =
                (width == model.settings.workspace.end() ? 76.f : std::stof(width->second)) * scale;
            const auto box = serverAvatarBox(railWidth, scale);
            auto *server = areaWithKey(ui.root(), "server:test");
            assert(server && visible(*server) && visible(*logo));
            RectNode *avatar = nullptr;
            for (const auto &child : server->children())
                if (auto *rect = dynamic_cast<RectNode *>(child.get());
                    rect && visible(*rect) && std::abs(rect->width() - box.width) < .1f &&
                    std::abs(rect->height() - box.height) < .1f)
                    avatar = rect;
            assert(avatar);
            assert(std::abs(logo->width() - box.width) < .1f &&
                   std::abs(logo->height() - box.height) < .1f);
            float logoX, logoY, avatarX, avatarY;
            Node::mapToScene(logo, logo->width() * .5f, logo->height() * .5f, logoX, logoY);
            Node::mapToScene(avatar, avatar->width() * .5f, avatar->height() * .5f, avatarX,
                             avatarY);
            assert(std::abs(logoX - avatarX) < .1f);
            assert(logo->sourceWidth() == logoSourceWidth &&
                   logo->sourceHeight() == logoSourceHeight);
            // Relayout of the real image must reuse the original decode/upload.
            const auto uploads = renderer.uploadCount();
            logo->parent()->markLayoutDirty();
            logo->parent()->layout(renderer);
            assert(renderer.uploadCount() == uploads);
        };
        logoGeometry();
        auto *composer = inputWithText(ui.root(), edgeDrafts[0]);
        assert(composer && visible(*composer));
        auto state = composer->editingState();
        state.current.cursorPos = 4;
        state.current.selectionAnchor = 1;
        composer->restoreEditingState(state);
        state = composer->editingState();
        dispatcher.setFocus(composer->inputArea());
        std::string expectedNavigation = "expanded";
        const auto unchanged = [&] {
            assert(model.activeBuffer == edgeBuffers[0]);
            assert(model.settings.peopleMode == SidebarMode::Hidden);
            assert(model.settings.workspace.at("native_navigation_mode") == expectedNavigation);
            assert(inputWithText(ui.root(), edgeDrafts[0]) == composer);
            assert(composer->editingStateMatches(state));
            assert(model.buffers.at(edgeBuffers[0]).draft == edgeDrafts[0]);
        };
        const auto drag = [&](std::string_view key, float logicalDelta) {
            auto *handle = areaWithKey(ui.root(), key);
            assert(handle && visible(*handle));
            const auto start = pointerPoint(*handle);
            pointerButton(dispatcher, start, BTN_LEFT, true);
            assert(dispatcher.pointerCaptured());
            // Leave the narrow handle: native capture must retain the drag.
            const std::array end{start[0] + logicalDelta * scale, start[1]};
            dispatcher.pointerMotion(end[0], end[1], 1000);
            frame();
            assert(dispatcher.pointerCaptured());
            if (key == "server-rail-resize")
                logoGeometry();
            pointerButton(dispatcher, end, BTN_LEFT, false);
            assert(!dispatcher.pointerCaptured());
            frame();
            unchanged();
            assert(dispatcher.focusedArea() == composer->inputArea());
        };
        auto *rail = areaWithKey(ui.root(), "server-rail-resize");
        auto *channels = areaWithKey(ui.root(), "channel-sidebar-resize");
        assert(rail && channels && visible(*rail) && visible(*channels));
        const auto liveStart = pointerPoint(*rail);
        pointerButton(dispatcher, liveStart, BTN_LEFT, true);
        dispatcher.pointerMotion(liveStart[0] + 12 * scale, liveStart[1], 1001);
        dispatcher.pointerMotion(liveStart[0] + 20 * scale, liveStart[1], 1002);
        assert(std::abs(pointerPoint(*rail)[0] - liveStart[0]) < .1f);
        frame();
        assert(std::abs(pointerPoint(*rail)[0] - liveStart[0] - 20 * scale) < .1f);
        logoGeometry();
        assert(dispatcher.pointerCaptured());
        dispatcher.cancelPointerCapture();
        frame();
        drag("server-rail-resize", -20);
        // Batched native motions do not move geometry until the surface tick; release
        // still applies its own latest scene coordinate when no tick has arrived.
        const auto batchedStart = pointerPoint(*rail);
        pointerButton(dispatcher, batchedStart, BTN_LEFT, true);
        dispatcher.pointerMotion(batchedStart[0] + 12 * scale, batchedStart[1], 1001);
        dispatcher.pointerMotion(batchedStart[0] + 20 * scale, batchedStart[1], 1002);
        assert(std::abs(pointerPoint(*rail)[0] - batchedStart[0]) < .1f);
        const std::array batchedEnd{batchedStart[0] + 24 * scale, batchedStart[1]};
        pointerButton(dispatcher, batchedEnd, BTN_LEFT, false);
        assert(std::abs(std::stof(model.settings.workspace.at("server_rail_width")) - 100) < .1f);
        frame();
        assert(std::abs(pointerPoint(*rail)[0] - batchedEnd[0]) < .1f);
        frame();
        assert(std::abs(pointerPoint(*rail)[0] - batchedEnd[0]) < .1f);
        drag("server-rail-resize", -24);

        // Pending coordinates belong only to their original capture and geometry.
        for (int transition = 0; transition < 5; ++transition) {
            const auto start = pointerPoint(*rail);
            pointerButton(dispatcher, start, BTN_LEFT, true);
            dispatcher.pointerMotion(start[0] + 32 * scale, start[1], 1003);
            if (transition == 0)
                dispatcher.cancelPointerCapture();
            else if (transition == 1)
                frame(720);
            else if (transition == 2) {
                model.settings.scale = scale + .25f;
                frame();
                model.settings.scale = scale;
            } else if (transition == 3)
                ui.execute("settings");
            else
                ui.execute("toggle-navigation");
            frame();
            assert(!dispatcher.pointerCaptured());
            assert(std::abs(std::stof(model.settings.workspace.at("server_rail_width")) - 76) <
                   .1f);
            pointerButton(dispatcher, start, BTN_LEFT, false);
            if (transition == 3)
                ui.execute("settings"); // Dismiss only; composer Cancel would collapse selection.
            else if (transition == 4) {
                ui.execute("toggle-navigation");
                ui.execute("toggle-navigation");
            }
            frame();
            assert(std::abs(pointerPoint(*rail)[0] - start[0]) < .1f);
        }
        dispatcher.setFocus(composer->inputArea());
        const auto railBefore = pointerPoint(*rail);
        const auto channelsBefore = pointerPoint(*channels);
        drag("server-rail-resize", 24);
        assert(std::abs(pointerPoint(*rail)[0] - railBefore[0] - 24 * scale) < .1f);
        assert(std::abs(pointerPoint(*channels)[0] - channelsBefore[0] - 24 * scale) < .1f);
        assert(std::abs(std::stof(model.settings.workspace.at("server_rail_width")) - 100) < .1f);
        const auto channelStart = pointerPoint(*channels);
        drag("channel-sidebar-resize", 48);
        assert(std::abs(pointerPoint(*channels)[0] - channelStart[0] - 48 * scale) < .1f);
        assert(std::abs(std::stof(model.settings.workspace.at("channel_sidebar_width")) - 298) <
               .1f);
        assert(std::abs(pointerPoint(*rail)[0] - railBefore[0] - 24 * scale) < .1f);
        for (const auto key : {"server-rail-resize", "channel-sidebar-resize"})
            drag(key, 10000);
        assert(std::stof(model.settings.workspace.at("server_rail_width")) == 160);
        assert(std::stof(model.settings.workspace.at("channel_sidebar_width")) == 420);
        assert(composer->width() > 0 && visible(*composer));
        for (const auto key : {"server-rail-resize", "channel-sidebar-resize"})
            drag(key, -10000);
        assert(std::stof(model.settings.workspace.at("server_rail_width")) == 56);
        assert(std::stof(model.settings.workspace.at("channel_sidebar_width")) == 160);
        dispatcher.setFocus(channels);
        nativeKey(ui, dispatcher, XKB_KEY_Right);
        frame();
        assert(std::stof(model.settings.workspace.at("channel_sidebar_width")) == 168);
        unchanged();
        dispatcher.setFocus(composer->inputArea());
        drag("server-rail-resize", 44);
        drag("channel-sidebar-resize", 130);
        const auto savedRail = pointerPoint(*rail);
        const auto savedChannels = pointerPoint(*channels);
        frame(720);
        unchanged();
        assert(std::stof(model.settings.workspace.at("server_rail_width")) == 100);
        assert(std::stof(model.settings.workspace.at("channel_sidebar_width")) == 298);
        frame();
        assert(std::abs(pointerPoint(*rail)[0] - savedRail[0]) < .1f);
        assert(std::abs(pointerPoint(*channels)[0] - savedChannels[0]) < .1f);
        expectedNavigation = "compact";
        ui.execute("toggle-navigation");
        frame();
        unchanged();
        assert(visible(*channels));
        auto *compactRow = areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[0]);
        assert(compactRow && visible(*compactRow));
        auto *compactInitials = labelWithText(*compactRow, "FI");
        assert(compactInitials && visible(*compactInitials));
        const float compactWidthBefore = compactRow->width();
        const float compactFontBefore = compactInitials->fontSize();
        const auto compactHandleBefore = pointerPoint(*channels);
        dispatcher.setFocus(composer->inputArea());
        drag("channel-sidebar-resize", 24);
        assert(std::abs(pointerPoint(*channels)[0] - compactHandleBefore[0] - 24 * scale) < .1f);
        assert(std::abs(compactRow->width() - compactWidthBefore - 24 * scale) < .1f);
        assert(compactInitials->fontSize() > compactFontBefore);
        assert(std::stof(model.settings.workspace.at("compact_channel_sidebar_width")) == 80);
        assert(std::stof(model.settings.workspace.at("channel_sidebar_width")) == 298);
        expectedNavigation = "hidden";
        ui.execute("toggle-navigation");
        frame();
        unchanged();
        assert(!visible(*channels));
        assert(std::stof(model.settings.workspace.at("compact_channel_sidebar_width")) == 80);
        expectedNavigation = "expanded";
        ui.execute("toggle-navigation");
        frame();
        unchanged();
        assert(visible(*channels));
        assert(std::abs(pointerPoint(*channels)[0] - savedChannels[0]) < .1f);
        ui.flush();
        storage.save(model);
        dispatcher.setFocus(nullptr);
        dispatcher.setSceneRoot(nullptr);

        Storage reopened;
        Model restored;
        reopened.load(restored);
        assert(restored.settings.workspace.at("server_rail_width") ==
               model.settings.workspace.at("server_rail_width"));
        assert(restored.settings.workspace.at("channel_sidebar_width") ==
               model.settings.workspace.at("channel_sidebar_width"));
        assert(restored.settings.workspace.at("compact_channel_sidebar_width") ==
               model.settings.workspace.at("compact_channel_sidebar_width"));
        theme.update(restored);
        Ui restarted(restored, clipboard);
        restarted.update(2400, 1200, 1);
        restarted.root().layout(renderer);
        auto *restoredRail = areaWithKey(restarted.root(), "server-rail-resize");
        auto *restoredChannels = areaWithKey(restarted.root(), "channel-sidebar-resize");
        assert(restoredRail && restoredChannels && visible(*restoredRail) &&
               visible(*restoredChannels));
        assert(std::abs(pointerPoint(*restoredRail)[0] - savedRail[0]) < .1f);
        assert(std::abs(pointerPoint(*restoredChannels)[0] - savedChannels[0]) < .1f);
        assert(inputWithText(restarted.root(), edgeDrafts[0]));
        restarted.execute("toggle-navigation");
        restarted.update(2400, 1200, 1);
        restarted.root().layout(renderer);
        assert(restored.settings.workspace.at("native_navigation_mode") == "compact");
        assert(visible(*restoredChannels));
        auto *restoredCompactRow =
            areaWithKey(restarted.root(), std::string("buffer:") + edgeBuffers[0]);
        assert(restoredCompactRow && visible(*restoredCompactRow));
        assert(std::abs(restoredCompactRow->width() - compactWidthBefore - 24 * scale) < .1f);
    }
}

void sidebarConversationFilterCheck() {
    using namespace blast;
    const std::string firstLog = "one/*server*", secondLog = "two/*server*";
    const std::string channelId = "one/#room", queryId = "one/person";
    const std::string secondQueryId = "two/remote-person";
    Model model;
    model.firstRun = false;
    model.now = 100;
    model.settings.retentionDays = 0;
    model.settings.reducedMotion = true;
    model.settings.typingSend = false;
    model.settings.peopleMode = SidebarMode::Hidden;
    model.identities.emplace_back();
    for (const std::string network : {"one", "two"}) {
        ServerConfig config;
        config.name = network;
        config.host = "example.invalid";
        model.servers.push_back(config);
        auto &log = model.buffers[network + "/*server*"];
        log.id = network + "/*server*";
        log.server = network;
        log.target = "*server*";
        log.kind = "server";
        log.unread = network == "one" ? 0 : 3;
    }
    auto &channel = model.buffers[channelId];
    channel.id = channelId;
    channel.server = "one";
    channel.target = "#room";
    channel.kind = "channel";
    auto &query = model.buffers[queryId];
    channel.draft = "Keep this channel draft";
    query.id = queryId;
    query.server = "one";
    query.target = "person";
    query.kind = "query";
    query.draft = "Keep this query draft";
    auto &remoteQuery = model.buffers[secondQueryId];
    remoteQuery.id = secondQueryId;
    remoteQuery.server = "two";
    remoteQuery.target = "remote-person";
    remoteQuery.kind = "query";
    remoteQuery.unread = 3;
    remoteQuery.draft = "Keep this remote draft";
    model.activeBuffer = channelId;
    model.settings.workspace["selected_server"] = "one";
    model.settings.workspace["last_buffer:two"] = secondQueryId;
    Clipboard clipboard;
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    const auto frame = [&] {
        ui.update(1600, 1000, 1);
        ui.root().layout(renderer);
    };
    frame();
    auto *first = areaWithKey(ui.root(), "buffer:" + queryId);
    auto *second = areaWithKey(ui.root(), "buffer:" + secondQueryId);
    auto *channelRow = areaWithKey(ui.root(), "buffer:" + channelId);
    auto *queryRow = areaWithKey(ui.root(), "buffer:" + queryId);
    auto *searchArea = areaWithKey(ui.root(), "sidebar-search");
    auto *firstServer = areaWithKey(ui.root(), "server:one");
    auto *secondServer = areaWithKey(ui.root(), "server:two");
    assert(first && channelRow && queryRow && searchArea && visible(*first));
    assert(!second || !visible(*second));
    assert(firstServer && secondServer && visible(*firstServer) && visible(*secondServer));
    const auto selectedGeometry = selectionGeometry(*firstServer);
    const auto unselectedGeometry = selectionGeometry(*secondServer);
    Node *sidebar = searchArea;
    while (sidebar && !dynamic_cast<Sidebar *>(sidebar))
        sidebar = sidebar->parent();
    assert(sidebar);
    auto *search = inputWithText(*sidebar, "");
    assert(search && search->inputArea() == searchArea);
    Button *all = nullptr, *unread = nullptr, *dms = nullptr;
    for (const auto &child : search->parent()->children())
        if (auto *button = dynamic_cast<Button *>(child.get())) {
            if (button->label()->text() == "All")
                all = button;
            else if (button->label()->text() == "Unread")
                unread = button;
            else if (button->label()->text() == "DMs")
                dms = button;
        }
    assert(all && unread && dms);
    const auto activate = [&](InputArea &area) {
        dispatcher.setFocus(&area);
        dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
        dispatcher.keyEvent(XKB_KEY_Return, 0, 0, false);
        frame();
    };
    const auto logsHidden = [&] {
        for (const auto &id : {firstLog, secondLog}) {
            const auto *row = areaWithKey(ui.root(), "buffer:" + id);
            assert(!row || !visible(*row));
        }
    };
    const auto unchangedSelection = [&] {
        assert(model.activeBuffer == channelId);
        assert(model.buffers.at(firstLog).unread == 0 && model.buffers.at(secondLog).unread == 3);
        assert(model.buffers.at(channelId).draft == "Keep this channel draft");
        assert(model.buffers.at(queryId).draft == "Keep this query draft");
        assert(model.buffers.at(secondQueryId).draft == "Keep this remote draft");
        assert(std::none_of(model.actions.begin(), model.actions.end(), [&](const Action &action) {
            return action.type == "read" &&
                   (action.buffer == firstLog || action.buffer == secondLog);
        }));
        logsHidden();
    };

    // Selecting the current server is never a second sidebar toggle.
    activate(*firstServer);
    unchangedSelection();
    assert(model.settings.workspace.at("native_navigation_mode") == "expanded");
    assert(visibleCount<Sidebar>(ui.root()) == 1);
    ui.execute("toggle-navigation");
    frame();
    activate(*firstServer);
    unchangedSelection();
    assert(model.settings.workspace.at("native_navigation_mode") == "compact");
    assert(visibleCount<Sidebar>(ui.root()) == 1 && sidebar->width() == 56);
    assert(!visible(*searchArea) && visible(*channelRow) && visible(*queryRow));
    ui.execute("toggle-navigation");
    ui.execute("toggle-navigation");
    frame();
    Dialogs *dialogs = nullptr;
    for (const auto &child : ui.root().children())
        if (auto *candidate = dynamic_cast<Dialogs *>(child.get()))
            dialogs = candidate;
    assert(dialogs);
    auto *settings = areaWithKey(ui.root(), "rail-settings");
    assert(settings && visible(*settings));
    activate(*settings);
    assert(dialogs->settingsOpen() && visible(*dialogs));
    unchangedSelection();
    ui.execute("settings");
    frame();
    assert(!dialogs->active() && !visible(*dialogs));
    unchangedSelection();
    auto *addNetwork = areaWithKey(ui.root(), "rail-add-server");
    assert(addNetwork && visible(*addNetwork));
    const auto profilesBefore = model.servers.size();
    activate(*addNetwork);
    assert(dialogs->active() && visible(*dialogs) && model.servers.size() == profilesBefore + 1);
    auto *address = inputWithText(*dialogs, model.servers.back().host);
    assert(address);
    dispatcher.setFocus(address->inputArea());
    address->selectAll();
    assert(address->insertText("new-network.example.invalid"));
    frame();
    assert(model.servers.back().host == "new-network.example.invalid");
    assert(model.servers[0].host == "example.invalid" &&
           model.servers[1].host == "example.invalid");
    auto *connect = primaryButton(*dialogs);
    assert(connect && connect->enabled());
    const auto firstAction = model.actions.size();
    activate(*connect->inputArea());
    assert(std::count_if(
               model.actions.begin() + firstAction, model.actions.end(), [&](const Action &action) {
                   return action.type == "save-connect" && action.text == model.servers.back().name;
               }) == 1);
    assert(std::none_of(model.actions.begin() + firstAction, model.actions.end(),
                        [](const Action &action) { return action.type == "connect"; }));
    ui.execute("cancel");
    frame();
    assert(!dialogs->active() && !visible(*dialogs) && model.servers.size() == profilesBefore + 1);
    unchangedSelection();

    // Search ordinary conversations while the active channel remains pinned.
    search->setValue(model.buffers.at(queryId).target);
    frame();
    assert(visible(*first) && visible(*channelRow));
    unchangedSelection();
    activate(*unread->inputArea());
    assert(!visible(*first));
    unchangedSelection();
    activate(*all->inputArea());
    search->setValue(model.buffers.at(queryId).target);
    frame();
    assert(visible(*first));
    search->setValue(model.buffers.at(queryId).server);
    frame();
    assert(visible(*first));
    unchangedSelection();
    search->setValue(model.buffers.at(queryId).target);
    frame();
    activate(*first);
    assert(model.activeBuffer == queryId);
    assert(model.settings.workspace.at("visible_buffers") == queryId);
    activate(*secondServer);
    assert(model.settings.workspace.at("selected_server") == "two");
    assert(model.activeBuffer == secondQueryId);
    // Selection changes painted geometry even with every color ignored.
    assert(selectionGeometry(*firstServer) != selectedGeometry);
    assert(selectionGeometry(*secondServer) != unselectedGeometry);
    first = areaWithKey(ui.root(), "buffer:" + queryId);
    second = areaWithKey(ui.root(), "buffer:" + secondQueryId);
    assert((!first || !visible(*first)) && second && visible(*second));
    assert(visible(*firstServer) && visible(*secondServer));
    activate(*firstServer);
    assert(model.settings.workspace.at("selected_server") == "one");
    assert(model.activeBuffer == queryId); // Each server restores its last conversation.
    assert(selectionGeometry(*firstServer) == selectedGeometry);
    assert(selectionGeometry(*secondServer) == unselectedGeometry);
    first = areaWithKey(ui.root(), "buffer:" + queryId);
    second = areaWithKey(ui.root(), "buffer:" + secondQueryId);
    channelRow = areaWithKey(ui.root(), "buffer:" + channelId);
    queryRow = areaWithKey(ui.root(), "buffer:" + queryId);
    assert(first && channelRow && queryRow);
    activate(*dms->inputArea());
    assert(visible(*first));
    search->setValue("");
    frame();
    assert(visible(*queryRow) && visible(*first) && !visible(*channelRow));
    assert(!second || !visible(*second));
    assert(model.activeBuffer == queryId);
    assert(model.settings.workspace.at("visible_buffers") == queryId);
    search->setValue("no matching conversation");
    frame();
    assert(visible(*queryRow) && !visible(*channelRow)); // Active conversations stay reachable.
    logsHidden();
    ui.execute("buffer:" + firstLog);
    frame();
    assert(model.activeBuffer == firstLog);
    logsHidden(); // Even selected or unread logs never become ordinary channel rows.
    assert(model.buffers.at(queryId).draft == "Keep this query draft");
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void sidebarAvatarCheck() {
    using namespace blast;
    const auto path = std::filesystem::path(std::getenv("XDG_CONFIG_HOME")) / "sidebar-avatar.png";
    auto *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1024, 1024);
    auto *context = cairo_create(surface);
    cairo_set_source_rgb(context, .8, .2, .1);
    cairo_paint(context);
    assert(cairo_surface_write_to_png(surface, path.c_str()) == CAIRO_STATUS_SUCCESS);
    cairo_destroy(context);
    cairo_surface_destroy(surface);
    // Valid PNG metadata mentioning SVG must still take the bounded raster path.
    constexpr unsigned char textChunk[]{0x00, 0x00, 0x00, 0x09, 0x74, 0x45, 0x58,
                                        0x74, 0x6e, 0x6f, 0x74, 0x65, 0x00, 0x3c,
                                        0x73, 0x76, 0x67, 0x7b, 0xab, 0xb0, 0x98};
    std::string png;
    {
        std::ifstream input(path, std::ios::binary);
        png.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    assert(png.size() > 33 && png.substr(12, 4) == "IHDR");
    png.insert(33, reinterpret_cast<const char *>(textChunk), sizeof(textChunk));
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(png.data(), png.size());
        assert(output);
    }
    auto model = edgeModel();
    model.settings.workspace["native_navigation_mode"] = "expanded";
    for (auto &[id, buffer] : model.buffers) {
        buffer.draft.clear();
        buffer.avatarPath = path.string();
    }
    model.buffers.at(edgeBuffers[0]).kind = "channel";
    model.buffers.at(edgeBuffers[3]).kind = "server";
    model.buffers.at(edgeBuffers[0]).draft = "avatar draft";
    Clipboard clipboard;
    Theme theme;
    theme.update(model);
    PangoRenderer renderer;
    Ui ui(model, clipboard);
    const auto frame = [&] {
        ui.update(1600, 1000, 1);
        ui.root().layout(renderer);
    };
    frame();
    auto *channel = areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[0]);
    auto *query = areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[1]);
    auto *server = areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[3]);
    assert(channel && query);
    assert(!server || !visible(*server));
    auto *initials = labelWithText(*channel, "FI");
    assert(initials && !visible(*initials));
    Image *avatar = nullptr;
    LucideIcon *draft = nullptr;
    for (const auto &child : channel->children()) {
        if (auto *image = dynamic_cast<Image *>(child.get()))
            avatar = image;
        if (auto *icon = dynamic_cast<LucideIcon *>(child.get()))
            draft = icon;
    }
    assert(avatar && avatar->hasImage() && visible(*avatar));
    const auto targetPixels = int(std::ceil(avatar->width() * renderer.renderScale()));
    assert(avatar->sourceWidth() == targetPixels && avatar->sourceHeight() == targetPixels);
    assert(draft && visible(*draft) && draft->width() < avatar->width() &&
           draft->height() < avatar->height());
    bool backedDraft = false;
    for (const auto &child : channel->children())
        if (const auto *rect = dynamic_cast<const RectNode *>(child.get());
            rect && visible(*rect) &&
            rect->style().fill == themeForegroundColor("tertiary_container"))
            backedDraft = rect->style().fill.a == 1.f && rect->x() <= draft->x() &&
                          rect->y() <= draft->y() &&
                          rect->x() + rect->width() >= draft->x() + draft->width() &&
                          rect->y() + rect->height() >= draft->y() + draft->height();
    assert(backedDraft);
    assert(visibleCount<Image>(*query) == 0);
    auto *input = inputWithText(ui.root(), "avatar draft");
    assert(input);
    auto *header = headerFor(*input);
    auto *headerInitials = labelWithText(*header, "FI");
    auto *headerName = labelWithText(*header, model.buffers.at(edgeBuffers[0]).target);
    assert(headerInitials && headerName);
    Image *headerAvatar = nullptr;
    for (const auto &child : headerName->parent()->children())
        if (auto *image = dynamic_cast<Image *>(child.get()))
            headerAvatar = image;
    assert(headerAvatar);
    assert(!visible(*headerAvatar) && !visible(*headerInitials) && !visible(*headerName));
    ui.execute("toggle-navigation");
    frame();
    assert(headerAvatar->hasImage() && visible(*headerAvatar) && !visible(*headerInitials));
    assert(headerAvatar->width() == 28 && headerAvatar->height() == 28);
    assert(headerAvatar->sourceWidth() == 28 && headerAvatar->sourceHeight() == 28);
    float avatarRight = 0, avatarY = 0, nameLeft = 0, nameY = 0;
    Node::mapToScene(headerAvatar, headerAvatar->width(), 0, avatarRight, avatarY);
    Node::mapToScene(headerName, 0, 0, nameLeft, nameY);
    assert(visible(*headerName) && avatarRight < nameLeft);
    model.settings.scale = 2;
    theme.update(model);
    frame();
    assert(headerAvatar->width() == 56 && headerAvatar->sourceWidth() == 56);
    model.settings.scale = 1;
    theme.update(model);
    frame();
    model.buffers.at(edgeBuffers[0]).avatarPath.clear();
    frame();
    assert(!visible(*avatar) && visible(*initials) && visible(*draft));
    assert(!visible(*headerAvatar) && visible(*headerInitials) && visible(*headerName));
    model.buffers.at(edgeBuffers[0]).avatarPath = path.string();
    frame();
    assert(avatar->hasImage() && visible(*avatar) && !visible(*initials) && visible(*draft));
    assert(headerAvatar->hasImage() && visible(*headerAvatar) && !visible(*headerInitials));
    model.settings.avatars = false;
    frame();
    assert(!visible(*avatar) && visible(*initials) && visible(*draft));
    assert(!visible(*headerAvatar) && visible(*headerInitials) && visible(*headerName));
    assert(visibleCount<Image>(*query) == 0);
    input->selectAll();
    ui.execute("kill-region");
    frame();
    assert(input->value().empty() && model.buffers.at(edgeBuffers[0]).draft.empty());
    assert(visible(*initials) && !visible(*draft) && !visible(*avatar));
    model.settings.avatars = true;
    model.buffers.at(edgeBuffers[0]).avatarPath = path.string() + ".missing";
    frame();
    assert(!visible(*headerAvatar) && visible(*headerInitials));
    assert(!visible(*avatar) && visible(*initials));
    model.buffers.at(edgeBuffers[0]).avatarPath = path.string();
    frame();
    assert(headerAvatar->hasImage() && visible(*headerAvatar) && visible(*avatar));
    std::filesystem::remove(path);
    model.buffers.at(edgeBuffers[0]).avatarPath.clear();
    frame();
    assert(!visible(*headerAvatar) && visible(*headerInitials));
    assert(!visible(*avatar) && visible(*initials));
    ui.execute("toggle-navigation");
    ui.execute("toggle-navigation");
    frame();
    assert(!visible(*headerAvatar) && !visible(*headerInitials) && !visible(*headerName));
}

void serverLogConsumerCheck() {
    using namespace blast;
    auto model = edgeModel();
    for (const auto *name : {"logged", "saved-empty"}) {
        ServerConfig server;
        server.name = name;
        server.host = "example.invalid";
        model.servers.push_back(std::move(server));
    }
    auto &log = model.buffers["logged/*server*"];
    log.id = "logged/*server*";
    log.server = "logged";
    log.kind = "server";
    log.target = "*internal-server-target*";
    log.topic = "not a channel topic";
    log.draft = "retained log draft";
    model.settings.workspace["native_navigation_mode"] = "compact";
    Clipboard clipboard;
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    MenuHost menu;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    ui.root().setPopupContext(&menu);
    const auto frame = [&] {
        ui.update(1600, 1000, 1);
        ui.root().layout(renderer);
    };
    frame();
    auto *input = inputWithText(ui.root(), edgeDrafts[0]);
    assert(input && visible(*input));
    const auto document = input->editingState();
    for (const auto *source : {"logged", "saved-empty"}) {
        auto *avatar = areaWithKey(ui.root(), std::string("server:") + source);
        assert(avatar && visible(*avatar));
        click(dispatcher, *avatar, BTN_RIGHT);
        assert(menu.open);
        menu.select("Open server log");
        frame();
        // The public Ui/Core boundary carries the menu source, not the active chat
        // or an invented log buffer ID. Core owns creation for saved empty profiles.
        assert(std::any_of(model.actions.begin(), model.actions.end(), [&](const Action &a) {
            return a.type == "open-server-log" && a.buffer == source && a.text.empty() &&
                   a.extra.empty();
        }));
        assert(model.activeBuffer == edgeBuffers[0] && input->editingStateMatches(document));
        assert(model.buffers.at(edgeBuffers[0]).draft == edgeDrafts[0]);
    }
    assert(!model.buffers.contains("saved-empty/*server*"));
    assert(!areaWithKey(ui.root(), "buffer:logged/*server*"));
    ui.execute("buffer:" + log.id);
    frame();
    auto *logInput = inputWithText(ui.root(), log.draft);
    assert(logInput && visible(*logInput));
    auto *header = headerFor(*logInput);
    auto *title = labelWithText(*header, "Server log");
    auto *network = labelWithText(*header, log.server);
    assert(title && network && visible(*title) && visible(*network));
    float titleRight = 0, titleY = 0, networkLeft = 0, networkY = 0;
    Node::mapToScene(title, title->width(), 0, titleRight, titleY);
    Node::mapToScene(network, 0, 0, networkLeft, networkY);
    assert(title->width() > 0 && network->width() > 0 && titleRight < networkLeft);
    assert(!visibleLabelContaining(*header, log.target));
    assert(!visibleLabelContaining(*header, log.topic));
    assert(!visible(*headerTool(*logInput, "People"))); // No channel-only People tool.
    dispatcher.setFocus(logInput->inputArea());
    const auto logDocument = logInput->editingState();
    for (auto *identity : {title, network}) {
        click(dispatcher, *identity);
        frame();
        assert(!menu.open && model.activeBuffer == log.id);
        assert(dispatcher.focusedArea() == logInput->inputArea());
        assert(logInput->editingStateMatches(logDocument) && log.draft == "retained log draft");
    }
    // Reusing the pane for an ordinary conversation must replace log identity.
    ui.execute("buffer:" + std::string(edgeBuffers[0]));
    frame();
    header = headerFor(*input);
    assert(visibleLabelContaining(*header, model.buffers.at(edgeBuffers[0]).target));
    assert(!visibleLabelContaining(*header, "Server log"));
    assert(!visibleLabelContaining(*header, "logged"));
    assert(input->editingStateMatches(document));
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void queuedServerLogPaletteCheck() {
    using namespace blast;
    const std::string configHome = std::getenv("XDG_CONFIG_HOME");
    const std::string dataHome = std::getenv("XDG_DATA_HOME");
    const auto isolated = std::filesystem::path(configHome) / "queued-log-palette";
    assert(setenv("XDG_CONFIG_HOME", isolated.c_str(), 1) == 0);
    assert(setenv("XDG_DATA_HOME", isolated.c_str(), 1) == 0);
    {
        Application app;
        for (const bool composing : {false, true}) {
            app.model = edgeModel();
            auto &model = app.model;
            ServerConfig source;
            source.name = "queued-log-source";
            source.host = "example.invalid";
            model.servers.push_back(source);
            for (auto &server : model.servers)
                server.sojuEnabled = false;
            const std::string logId = source.name + "/*server*";
            assert(!model.buffers.contains(logId));
            model.settings.workspace["native_navigation_mode"] = "compact";
            Clipboard clipboard;
            Theme theme;
            theme.update(model);
            InputDispatcher dispatcher;
            PangoRenderer renderer;
            MenuHost menu;
            Ui ui(model, clipboard);
            dispatcher.setSceneRoot(&ui.root());
            ui.setInputDispatcher(dispatcher);
            ui.setWindowActive(true);
            ui.root().setPopupContext(&menu);
            const auto frame = [&] {
                ui.update(1600, 1000, 1);
                ui.root().layout(renderer);
            };
            WindowActions actions{};
            actions.synchronizeLayout = [&] {
                frame();
                return true;
            };
            ui.setWindowActions(std::move(actions));
            frame();
            auto *origin = inputWithText(ui.root(), edgeDrafts[0]);
            auto *avatar = areaWithKey(ui.root(), "server:" + source.name);
            assert(origin && avatar && visible(*origin) && visible(*avatar));
            dispatcher.setFocus(origin->inputArea());
            const auto originDocument = origin->editingState();
            const auto firstAction = model.actions.size();
            click(dispatcher, *avatar, BTN_RIGHT);
            assert(menu.open);
            menu.select("Open server log");
            frame(); // Admit native menu intent into Core's queue without ticking Core.
            const auto queued =
                std::find_if(model.actions.begin() + firstAction, model.actions.end(),
                             [](const Action &action) { return action.type == "open-server-log"; });
            assert(queued != model.actions.end());
            const Action selectedLog = *queued;
            assert(selectedLog.buffer == source.name && selectedLog.text.empty() &&
                   selectedLog.extra.empty());
            assert(model.activeBuffer == edgeBuffers[0] && !model.buffers.contains(logId));

            // Deliver Alt+x and native text before Core drains the actual menu action.
            hostKey(ui, dispatcher, XKB_KEY_x, KeyMod::Alt);
            frame();
            auto *query = dispatcher.focusedArea()
                              ? dynamic_cast<Input *>(dispatcher.focusedArea()->textInputClient())
                              : nullptr;
            assert(query && query != origin && visible(*query) && query->value().empty());
            for (const char ch : std::string_view("sett"))
                hostKey(ui, dispatcher, ch, 0, ch);
            hostKey(ui, dispatcher, XKB_KEY_Left);
            hostKey(ui, dispatcher, XKB_KEY_x, 0, 'x');
            hostKey(ui, dispatcher, XKB_KEY_z, KeyMod::Ctrl);
            assert(query->value() == "sett");
            hostKey(ui, dispatcher, XKB_KEY_Left, KeyMod::Shift);
            const auto selectedQuery = query->editingState();
            assert(selectedQuery.current.cursorPos != selectedQuery.current.selectionAnchor);
            assert(!selectedQuery.history->undo.empty() && !selectedQuery.history->redo.empty());
            if (composing)
                static_cast<TextInputClient &>(*query).textInputApplyEdit({.commitText = {},
                                                                           .preeditText = "猫",
                                                                           .preeditCursorBegin = 3,
                                                                           .preeditCursorEnd = 3,
                                                                           .hasPreedit = true});
            frame();
            const auto document = query->editingState();
            const auto undo = document.history->undo, redo = document.history->redo;
            assert(query->composing() == composing);
            if (composing)
                assert(document.preeditBase == selectedQuery.current);
            assert(model.activeBuffer == edgeBuffers[0] && !model.buffers.contains(logId));
            assert(std::count_if(model.actions.begin(), model.actions.end(),
                                 [&](const Action &action) {
                                     return action.type == selectedLog.type &&
                                            action.buffer == selectedLog.buffer &&
                                            action.text == selectedLog.text &&
                                            action.extra == selectedLog.extra;
                                 }) == 1);

            // No callback stand-in: Application creates/selects the saved profile's log.
            app.tick(1);
            assert(model.activeBuffer == logId);
            const auto &log = model.buffers.at(model.activeBuffer);
            assert(log.server == selectedLog.buffer && log.kind == "server" && log.draft.empty());
            frame();
            const auto unchanged = [&] {
                assert(visible(*query) && dispatcher.focusedArea() == query->inputArea());
                assert(query->value() == document.current.value &&
                       query->editingStateMatches(document) && query->composing() == composing);
                assert(query->editingState().history == document.history &&
                       document.history->undo == undo && document.history->redo == redo);
                assert(origin->editingStateMatches(originDocument));
                for (std::size_t i = 0; i < edgeBuffers.size(); ++i)
                    assert(model.buffers.at(edgeBuffers[i]).draft == edgeDrafts[i]);
                assert(log.draft.empty() && model.activeBuffer == log.id &&
                       model.settings.workspace.at("selected_server") == log.server);
            };
            unchanged();
            frame();
            unchanged();
            if (composing) {
                static_cast<TextInputClient &>(*query).textInputResetPreedit();
                assert(query->editingStateMatches(selectedQuery));
            } else {
                hostKey(ui, dispatcher, XKB_KEY_s, 0, 's');
                assert(query->value() != document.current.value && log.draft.empty());
            }
            hostKey(ui, dispatcher, XKB_KEY_Escape);
            frame();
            auto *logInput =
                dispatcher.focusedArea()
                    ? dynamic_cast<Input *>(dispatcher.focusedArea()->textInputClient())
                    : nullptr;
            assert(!visible(*query) && logInput && logInput != query && visible(*logInput) &&
                   logInput->value().empty());
            auto *network = labelWithText(*headerFor(*logInput), log.server);
            assert(network && visible(*network));
            assert(model.activeBuffer == log.id &&
                   model.settings.workspace.at("selected_server") == log.server);

            // Explicit user selection still dismisses, unlike delayed Core reconciliation.
            hostKey(ui, dispatcher, XKB_KEY_x, KeyMod::Alt);
            frame();
            assert(visible(*query) && dispatcher.focusedArea() == query->inputArea());
            ui.execute("buffer:" + std::string(edgeBuffers[1]));
            frame();
            auto *next = inputWithText(ui.root(), edgeDrafts[1]);
            assert(next && visible(*next) && !visible(*query) &&
                   dispatcher.focusedArea() == next->inputArea());
            assert(model.activeBuffer == edgeBuffers[1] &&
                   model.settings.workspace.at("selected_server") ==
                       model.buffers.at(edgeBuffers[1]).server);
            for (std::size_t i = 0; i < edgeBuffers.size(); ++i)
                assert(model.buffers.at(edgeBuffers[i]).draft == edgeDrafts[i]);
            assert(log.draft.empty());
            dispatcher.setFocus(nullptr);
            dispatcher.setSceneRoot(nullptr);
        }
    }
    assert(setenv("XDG_CONFIG_HOME", configHome.c_str(), 1) == 0);
    assert(setenv("XDG_DATA_HOME", dataHome.c_str(), 1) == 0);
}

void sojuDisplayNameConsumerCheck() {
    using namespace blast;
    auto model = edgeModel();
    const std::string childKey = "soju-managed-opaque-42";
    const std::string childId = childKey + "/#label-proof";
    ServerConfig parent;
    parent.name = "label-account";
    parent.alias = "Bouncer account";
    parent.sojuAccount = true;
    parent.bouncer = "soju";
    model.servers.push_back(parent);
    ServerConfig child;
    child.name = childKey;
    child.sojuParent = parent.name;
    child.sojuNetworkId = "42";
    child.bouncer = "soju:42";
    model.servers.push_back(child);
    auto &buffer = model.buffers[childId];
    buffer.id = childId;
    buffer.server = childKey;
    buffer.target = "#label-proof";
    buffer.kind = "channel";
    buffer.joined = true;
    buffer.topic = "A live catalog must not change this conversation's identity.";
    model.networks[childKey].name = childKey;
    model.networks[childKey].state = "Ready";
    model.networks[childKey].nick = "Me";
    model.settings.workspace["native_navigation_mode"] = "expanded";
    model.settings.workspace["soju-group:" + parent.name] = "expanded";
    Clipboard clipboard;
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    const auto frame = [&] {
        ui.update(1600, 1000, 1);
        ui.root().layout(renderer);
    };
    WindowActions actions{};
    actions.synchronizeLayout = [&] {
        frame();
        return true;
    };
    ui.setWindowActions(std::move(actions));
    ui.execute("buffer:" + childId);
    frame();
    const auto consumers = [&](std::string_view caption, bool allowElidedHeader = false) {
        assert(model.activeBuffer == childId && buffer.id == childId && buffer.server == childKey);
        assert(model.settings.workspace.at("selected_server") == childKey);
        auto *placeholder =
            visibleLabelContaining(ui.root(), "Message #label-proof · " + std::string(caption));
        assert(placeholder);
        Node *editor = placeholder->parent();
        while (editor && !dynamic_cast<Input *>(editor))
            editor = editor->parent();
        auto *input = dynamic_cast<Input *>(editor);
        assert(input && input->value().empty() && visible(*input));
        auto *header = headerFor(*input);
        // Expanded navigation owns network identity; the header chip is shown
        // only once navigation is compact/hidden.
        assert(!visibleLabelContaining(*header, caption));
        ui.execute("toggle-navigation");
        frame();
        assert(model.settings.workspace.at("native_navigation_mode") == "compact");
        if (!visibleLabelContaining(*header, caption)) {
            assert(allowElidedHeader);
            const auto captionChip = [&](auto &&self, Node &node) -> Label * {
                if (!visible(node))
                    return nullptr;
                if (auto *label = dynamic_cast<Label *>(&node); label && label->hasTooltip()) {
                    const auto content = label->tooltipContent();
                    if (const auto *text = std::get_if<std::string>(&content);
                        text && *text == caption)
                        return label;
                }
                for (const auto &child : node.children())
                    if (auto *label = self(self, *child))
                        return label;
                return nullptr;
            };
            auto *chip = captionChip(captionChip, *header);
            assert(chip && !chip->text().empty() &&
                   chip->text().find(childKey) == std::string::npos);
        }
        assert(!visibleLabelContaining(*header, childKey));
        ui.execute("toggle-navigation");
        ui.execute("toggle-navigation");
        frame();
        assert(model.settings.workspace.at("native_navigation_mode") == "expanded");
        auto *rail = areaWithKey(ui.root(), "server:" + childKey);
        assert(rail && visible(*rail) && labelWithText(*rail, caption));
        const auto railHint = rail->tooltipContent();
        const auto *railText = std::get_if<std::string>(&railHint);
        assert(railText && railText->find(caption) != std::string::npos &&
               railText->find(childKey) == std::string::npos);
        auto *row = areaWithKey(ui.root(), "buffer:" + childId);
        assert(row && visible(*row));
        const auto rowHint = row->tooltipContent();
        const auto *rowText = std::get_if<std::string>(&rowHint);
        assert(rowText && rowText->find("#label-proof\n" + std::string(caption) + "\n") == 0 &&
               rowText->find(childKey) == std::string::npos);
        auto *line = visibleNode<ModeLine>(ui.root());
        assert(line && visibleLabelContaining(*line, caption));
    };
    consumers("Network");

    // A filtered-out buffer must enter an already-open palette on catalog arrival
    // and again on rename, without another query edit or a buffer revision change.
    for (const std::string caption : {"Aurora network", "Borealis network"}) {
        ui.execute("buffer:" + std::string(edgeBuffers[0]));
        frame();
        ui.execute("switch-buffer");
        frame();
        auto *query = dispatcher.focusedArea()
                          ? dynamic_cast<Input *>(dispatcher.focusedArea()->textInputClient())
                          : nullptr;
        assert(query && visible(*query) && query->value().empty());
        assert(visibleLabelContaining(*query, "Find a conversation…"));
        assert(query->insertText(caption));
        frame();
        const auto document = query->editingState();
        const std::string result = "#label-proof  ·  " + caption;
        assert(!visibleLabelContaining(ui.root(), result));
        model.networks[parent.name].sojuNetworks["42"].attributes["name"] = caption;
        frame();
        assert(visible(*query) && dispatcher.focusedArea() == query->inputArea() &&
               query->editingStateMatches(document) && query->value() == caption);
        assert(visibleLabelContaining(ui.root(), result));
        assert(model.servers.back().alias.empty() && buffer.messagesRevision == 0);
        hostKey(ui, dispatcher, XKB_KEY_Return);
        frame();
        assert(!visible(*query));
        consumers(caption);
    }

    ui.execute("topic");
    frame();
    auto *dialogs = visibleNode<Dialogs>(ui.root());
    assert(dialogs && visibleLabelContaining(*dialogs, "#label-proof · Borealis network"));
    assert(model.status == "#label-proof · Borealis network\n" + buffer.topic);
    model.servers.back().alias = "My personal network";
    frame();
    assert(visibleLabelContaining(*dialogs, "#label-proof · My personal network"));
    assert(!visibleLabelContaining(*dialogs, "Borealis network"));
    model.networks[parent.name].sojuNetworks["42"].attributes["name"] = "Remote renamed again";
    frame();
    assert(model.servers.back().alias == "My personal network");
    assert(visibleLabelContaining(*dialogs, "#label-proof · My personal network"));
    hostKey(ui, dispatcher, XKB_KEY_Escape);
    frame();
    consumers("My personal network", true);

    for (const std::string &search :
         {std::string("My personal network"), "#label-proof " + childKey}) {
        ui.execute("buffer:" + std::string(edgeBuffers[0]));
        frame();
        ui.execute("switch-buffer");
        frame();
        auto *query = dispatcher.focusedArea()
                          ? dynamic_cast<Input *>(dispatcher.focusedArea()->textInputClient())
                          : nullptr;
        assert(query && visible(*query) && query->value().empty());
        assert(visibleLabelContaining(*query, "Find a conversation…"));
        assert(query->insertText(search));
        frame();
        auto *result = visibleLabelContaining(ui.root(), "#label-proof  ·  My personal network");
        assert(result && result->text() == "#label-proof  ·  My personal network");
        assert(result->text().find(childKey) == std::string::npos);
        assert(!visibleLabelContaining(ui.root(), "Remote renamed again"));
        hostKey(ui, dispatcher, XKB_KEY_Return);
        frame();
        assert(!visible(*query) && model.activeBuffer == childId);
        consumers("My personal network", true);
    }
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void sojuFoldConsumerCheck() {
    using namespace blast;
    Storage storage;
    Model loaded;
    storage.load(loaded);
    auto model = edgeModel();
    ServerConfig parent;
    parent.name = "fold-account";
    parent.alias = "Original account alias";
    parent.host = "bouncer.invalid";
    parent.bouncer = "soju";
    parent.sojuAccount = true;
    parent.identity = model.identities.front().name;
    parent.sasl = "PLAIN";
    model.identities.front().username = "fold-account-user";
    model.identities.front().password = "fold-account-secret";
    model.servers.push_back(parent);
    model.servers[0].sojuParent = parent.name;
    model.servers[0].sojuNetworkId = "7";
    model.servers[0].bouncer = "soju:7";
    model.settings.workspace["native_navigation_mode"] = "expanded";
    model.buffers.at(edgeBuffers[2]).unread = 5;
    auto &accountUnread = model.buffers["fold-account/query"];
    accountUnread.id = "fold-account/query";
    accountUnread.server = parent.name;
    accountUnread.kind = "query";
    accountUnread.target = "query";
    accountUnread.unread = 3;
    Clipboard clipboard;
    Theme theme;
    theme.update(model);
    PangoRenderer renderer;
    {
        InputDispatcher dispatcher;
        Ui ui(model, clipboard);
        dispatcher.setSceneRoot(&ui.root());
        ui.setInputDispatcher(dispatcher);
        ui.setWindowActive(true);
        const auto frame = [&] {
            ui.update(1600, 1000, 1);
            ui.root().layout(renderer);
        };
        frame();
        auto *input = inputWithText(ui.root(), edgeDrafts[0]);
        auto *avatar = areaWithKey(ui.root(), "server:fold-account");
        auto *child = areaWithKey(ui.root(), "server:test");
        assert(input && avatar && child && visible(*child));
        auto *accountCaption = labelWithText(*avatar, parent.alias);
        assert(accountCaption);
        auto state = input->editingState();
        state.current.cursorPos = 8;
        state.current.selectionAnchor = 2;
        input->restoreEditingState(state);
        dispatcher.setFocus(input->inputArea());
        static_cast<TextInputClient &>(*input).textInputApplyEdit({.commitText = {},
                                                                   .preeditText = "猫",
                                                                   .preeditCursorBegin = 3,
                                                                   .preeditCursorEnd = 3,
                                                                   .hasPreedit = true});
        frame();
        const auto composing = input->editingState();
        const auto undo = composing.history->undo, redo = composing.history->redo;
        const std::array geometry{input->x(),
                                  input->y(),
                                  input->width(),
                                  input->height(),
                                  paneFor(*input)->width(),
                                  paneFor(*input)->height()};
        const auto unchanged = [&] {
            assert(model.activeBuffer == edgeBuffers[0] && input->composing());
            assert(dispatcher.focusedArea() == input->inputArea());
            assert(input->editingStateMatches(composing));
            assert(composing.history->undo == undo && composing.history->redo == redo);
            assert(model.buffers.at(edgeBuffers[0]).draft == edgeDrafts[0]);
            assert((geometry == std::array{input->x(), input->y(), input->width(), input->height(),
                                           paneFor(*input)->width(), paneFor(*input)->height()}));
        };
        for (const auto *intent : {"collapsed", "expanded", "collapsed"}) {
            click(dispatcher, *avatar);
            frame();
            assert(model.settings.workspace.at("soju-group:fold-account") == intent);
            assert(visible(*child) == (std::string_view(intent) == "expanded"));
            const bool collapsed = std::string_view(intent) == "collapsed";
            auto *count = labelWithText(*avatar, collapsed ? "8" : "3");
            assert(count && visible(*count) && count->color() == ::palette.onPrimary);
            assert(labelWithText(*child, "5"));
            bool plateFound = false;
            for (const auto &node : avatar->children())
                if (const auto *rect = dynamic_cast<const RectNode *>(node.get());
                    rect && visible(*rect) && rect->style().fill == ::palette.primary &&
                    rect->style().radius == 4.f) {
                    plateFound = true;
                    assert(rect->style().fill.a == 1.f);
                    assert(count->x() >= rect->x() && count->y() >= rect->y());
                    assert(count->x() + count->width() <= rect->x() + rect->width() + .5f);
                }
            assert(plateFound);
            unchanged();
        }
        // Invalid parents and ordinary children cannot acquire account fold state.
        ui.execute("soju-group-toggle:test");
        ui.execute("soju-group-toggle:missing");
        frame();
        assert(!model.settings.workspace.contains("soju-group:test"));
        assert(!model.settings.workspace.contains("soju-group:missing"));
        unchanged();
        assert(std::any_of(model.actions.begin(), model.actions.end(),
                           [](const Action &a) { return a.type == "save"; }));
        // Edit the live profile, not a replacement rail or a caption-only model stand-in.
        ui.execute("server:" + parent.name);
        frame();
        auto *dialogs = visibleNode<Dialogs>(ui.root());
        assert(dialogs && dialogs->settingsOpen());
        auto *alias = inputWithText(*dialogs, parent.alias);
        auto *pickerCaption = labelWithText(*dialogs, parent.alias);
        auto *picker = pickerCaption ? dynamic_cast<Select *>(pickerCaption->parent()) : nullptr;
        assert(alias && alias->enabled() && picker);
        dispatcher.setFocus(alias->inputArea());
        alias->selectAll();
        alias->textInputApplyEdit(
            {.commitText = "Renamed account", .preeditText = {}, .hasCommitText = true});
        frame();
        assert(inputWithText(*dialogs, "Renamed account") == alias &&
               picker->selectedText() == "Renamed account");
        assert(areaWithKey(ui.root(), "server:" + parent.name) == avatar &&
               accountCaption->text() == "Renamed account");
        assert(model.servers.back().name == parent.name &&
               model.servers.back().alias == "Renamed account" &&
               model.servers.front().sojuParent == parent.name &&
               model.servers.front().sojuNetworkId == "7" &&
               model.settings.workspace.at("soju-group:" + parent.name) == "collapsed");
        auto *username = inputWithText(*dialogs, "fold-account-user");
        auto *password = inputWithText(*dialogs, "fold-account-secret");
        assert(username && password && password->enabled());
        dispatcher.setFocus(username->inputArea());
        username->selectAll();
        username->textInputApplyEdit(
            {.commitText = "edited-account-user", .preeditText = {}, .hasCommitText = true});
        dispatcher.setFocus(password->inputArea());
        password->selectAll();
        password->textInputApplyEdit(
            {.commitText = "edited-account-secret", .preeditText = {}, .hasCommitText = true});
        const auto passwordEditing = password->editingState();
        model.networks[parent.name].state = "Failed";
        model.networks[parent.name].error = "Account connection refused";
        frame();
        assert(inputWithText(*dialogs, "edited-account-user") == username &&
               inputWithText(*dialogs, "edited-account-secret") == password &&
               password->editingStateMatches(passwordEditing) &&
               dispatcher.focusedArea() == password->inputArea() &&
               model.identities.front().password == "edited-account-secret" &&
               model.servers.back().sasl == "PLAIN" && model.servers.back().password.empty());
        hostKey(ui, dispatcher, XKB_KEY_Escape);
        frame();
        ui.flush();
        storage.save(model);
        dispatcher.setFocus(nullptr);
        dispatcher.setSceneRoot(nullptr);
    }
    Model restarted;
    storage.load(restarted);
    assert(restarted.settings.workspace.at("soju-group:fold-account") == "collapsed");
    const auto savedParent =
        std::find_if(restarted.servers.begin(), restarted.servers.end(),
                     [&](const auto &profile) { return profile.name == parent.name; });
    const auto savedChild =
        std::find_if(restarted.servers.begin(), restarted.servers.end(),
                     [](const auto &profile) { return profile.name == "test"; });
    assert(savedParent != restarted.servers.end() && savedParent->sojuAccount &&
           savedParent->alias == "Renamed account" && savedParent->host == parent.host &&
           savedChild != restarted.servers.end() && savedChild->sojuParent == parent.name &&
           savedChild->sojuNetworkId == "7");
    assert(savedParent->sasl == "PLAIN" && savedParent->password.empty() &&
           restarted.identities.size() == model.identities.size() &&
           restarted.identities.front().username == "edited-account-user" &&
           restarted.identities.front().password == "edited-account-secret");
    theme.update(restarted);
    Ui ui(restarted, clipboard);
    ui.update(1600, 1000, 1);
    ui.root().layout(renderer);
    auto *avatar = areaWithKey(ui.root(), "server:fold-account");
    auto *child = areaWithKey(ui.root(), "server:test");
    assert(avatar && visible(*avatar) && child && !visible(*child));
    assert(labelWithText(*avatar, "Renamed account"));
    assert(restarted.activeBuffer == edgeBuffers[0]);
}

void staleUploadHostMenuCheck() {
    using namespace blast;
    for (const bool hostStage : {false, true})
        for (const std::string_view change :
             {"typed", "preedit", "switch", "delete", "cancel", "replace"}) {
            auto model = edgeModel();
            model.buffers.at(edgeBuffers[0]).draft.clear();
            Clipboard clipboard;
            Theme theme;
            theme.update(model);
            InputDispatcher dispatcher;
            PangoRenderer renderer;
            MenuHost menu;
            Ui ui(model, clipboard);
            dispatcher.setSceneRoot(&ui.root());
            ui.setInputDispatcher(dispatcher);
            ui.setWindowActive(true);
            ui.root().setPopupContext(&menu);
            const auto frame = [&] {
                ui.update(1600, 1000, 1);
                ui.root().layout(renderer);
            };
            // Reproduce the native producer's close/dismiss-before-activation order,
            // retaining the selected callback across close just as ContextMenuPopup does.
            const auto closeNative = [&] {
                auto dismiss = menu.callbacks.onDismiss;
                menu.closeSelectDropdown();
                if (dismiss)
                    dismiss();
            };
            const auto selectNative = [&](std::size_t value) {
                auto select = menu.callbacks.onSelect;
                assert(menu.open && select);
                closeNative();
                select(value);
            };
            const auto textSource = [&] {
                const auto found =
                    std::find_if(menu.request.menu.begin(), menu.request.menu.end(),
                                 [](const ContextMenuEntry &entry) {
                                     return entry.enabled && entry.label == "Text / paste…";
                                 });
                assert(found != menu.request.menu.end());
                return found->value;
            };
            frame();
            auto *composer = composerControl(ui.root(), "input");
            assert(composer);
            auto *input = dynamic_cast<Input *>(composer->textInputClient());
            auto *paperclip = composerControl(ui.root(), "upload");
            assert(input && visible(*input) && paperclip && visible(*paperclip));
            click(dispatcher, *paperclip);
            assert(menu.open && menu.callbacks.onSelect);
            std::size_t choice = textSource();
            Input *retainedUpload = nullptr;
            if (hostStage) {
                selectNative(choice);
                frame();
                assert(!menu.open);
                auto *editor = areaWithKey(ui.root(), "upload-text");
                retainedUpload =
                    editor ? dynamic_cast<Input *>(editor->textInputClient()) : nullptr;
                assert(retainedUpload && retainedUpload->insertText("retained upload\n猫"));
                auto *hostChoice = areaWithKey(ui.root(), "upload-host");
                assert(hostChoice && visible(*hostChoice));
                ui.execute("upload-host");
                frame();
                assert(menu.open && menu.callbacks.onSelect);
                const auto provider =
                    std::find_if(menu.request.menu.begin(), menu.request.menu.end(),
                                 [&](const ContextMenuEntry &entry) {
                                     return entry.enabled && entry.radio &&
                                            entry.value == static_cast<std::size_t>(
                                                               model.settings.uploads.provider);
                                 });
                assert(provider != menu.request.menu.end() && provider->toggleState == 1 &&
                       menu.request.selectedIndex == provider->value);
                const auto other = std::find_if(menu.request.menu.begin(), menu.request.menu.end(),
                                                [&](const ContextMenuEntry &entry) {
                                                    return entry.enabled && entry.radio &&
                                                           entry.value != provider->value;
                                                });
                assert(other != menu.request.menu.end());
                choice = other->value;
            }
            auto staleSelect = menu.callbacks.onSelect;
            auto *closeUpload = areaWithKey(ui.root(), "upload-close");
            assert(closeUpload && visible(*closeUpload) == hostStage);
            const auto providerBefore = model.settings.uploads.provider;
            std::optional<Input::EditingState> expectedComposition;
            std::vector<Input::EditSnapshot> expectedUndo, expectedRedo;
            if (change == "cancel" || change == "replace") {
                closeNative();
            } else if (change == "switch") {
                ui.execute("buffer:" + std::string(edgeBuffers[1]));
            } else if (change == "delete") {
                model.buffers.erase(edgeBuffers[0]);
            } else {
                TextInputEdit edit{};
                if (change == "typed") {
                    edit.commitText = "new draft";
                    edit.hasCommitText = true;
                } else {
                    edit.preeditText = "猫";
                    edit.preeditCursorBegin = edit.preeditCursorEnd = 3;
                    edit.hasPreedit = true;
                }
                static_cast<TextInputClient &>(*input).textInputApplyEdit(edit);
                if (change == "preedit") {
                    expectedComposition = input->editingState();
                    expectedUndo = expectedComposition->history->undo;
                    expectedRedo = expectedComposition->history->redo;
                    assert(input->composing() && expectedComposition->preeditBase &&
                           expectedComposition->preeditBase->value.empty() &&
                           expectedComposition->current.value == edit.preeditText);
                }
            }
            // Once editing has begun, changing the original draft is allowed: the
            // retained upload can still be recovered without replacing that draft.
            // Dismiss this optional host menu before delivering its late callback.
            if (hostStage && change != "cancel" && change != "replace")
                closeNative();
            frame();
            assert(!menu.open);
            if (change == "replace") {
                if (hostStage) {
                    ui.execute("upload-host");
                    frame();
                } else {
                    click(dispatcher, *paperclip);
                }
                assert(menu.open && menu.callbacks.onSelect);
                if (!hostStage)
                    textSource();
            }
            // Late source callbacks cannot start an upload; dismissed host callbacks
            // cannot replace the retained upload or a newer host/source popup.
            staleSelect(choice);
            frame();
            assert(menu.open == (change == "replace") && visible(*closeUpload) == hostStage &&
                   model.settings.uploads.provider == providerBefore);
            if (change == "replace") {
                if (!hostStage)
                    textSource();
                closeNative();
                frame();
                assert(!menu.open);
            }
            if (hostStage) {
                assert(retainedUpload && retainedUpload->value() == "retained upload\n猫");
                ui.execute("upload-host");
                frame();
                assert(menu.open &&
                       menu.request.selectedIndex == static_cast<std::size_t>(providerBefore));
                closeNative();
                frame();
            }
            assert(model.buffers.at(edgeBuffers[1]).draft == edgeDrafts[1]);
            if (change == "typed")
                assert(input->value() == "new draft" &&
                       model.buffers.at(edgeBuffers[0]).draft == "new draft");
            if (change == "preedit") {
                assert(expectedComposition && input->composing() &&
                       input->editingStateMatches(*expectedComposition) &&
                       expectedComposition->history->undo == expectedUndo &&
                       expectedComposition->history->redo == expectedRedo &&
                       model.buffers.at(edgeBuffers[0]).draft.empty());
            }
            if (change == "switch")
                assert(model.activeBuffer == edgeBuffers[1]);
            if (change == "delete")
                assert(!model.buffers.contains(edgeBuffers[0]));
            dispatcher.setFocus(nullptr);
            dispatcher.setSceneRoot(nullptr);
        }
}

void titlebarGeometryCheck() {
    auto model = edgeModel();
    Clipboard clipboard;
    blast::Theme theme;
    PangoRenderer renderer;
    blast::Ui ui(model, clipboard);
    for (const float scale : {1.f, 2.f}) {
        model.settings.scale = scale;
        theme.update(model);
        ui.update(1600, 1000, 1);
        ui.root().layout(renderer);
        auto *title = labelWithText(ui.root(), "BlastIRC");
        auto *logo = visibleNode<Image>(ui.root());
        auto *control = areaWithKey(ui.root(), "topbar-navigation");
        assert(title && logo && logo->hasImage() && control);
        float titleX = 0, titleY = 0, logoRight = 0, logoY = 0;
        Node::mapToScene(title, 0, 0, titleX, titleY);
        Node::mapToScene(logo, logo->width(), 0, logoRight, logoY);
        assert(logoRight < titleX);
        // Optical placement must not move the shared native titlebar hit targets.
        float controlX = 0, controlCenter = 0, logoX = 0, logoCenter = 0;
        Node::mapToScene(control, 0, control->height() / 2, controlX, controlCenter);
        Node::mapToScene(logo, 0, logo->height() / 2, logoX, logoCenter);
        assert(std::abs(controlCenter - logoCenter) <= .5f);
        assert(control->width() >= 32 * scale && control->height() >= 32 * scale);
    }
}

void railReadAllCheck() {
    using namespace blast;
    auto model = edgeModel();
    for (auto &[id, buffer] : model.buffers)
        buffer.unread = 0;
    auto &search = model.buffers["search:rail"];
    search.kind = "search";
    search.unread = 9;
    auto &serverLog = model.buffers["other/*server*"];
    serverLog.id = "other/*server*";
    serverLog.server = "other";
    serverLog.kind = "server";
    Theme theme;
    theme.update(model);
    PangoRenderer renderer;
    InputDispatcher dispatcher;
    std::vector<std::string> commands;
    ServerRail rail(
        model, [&](std::string command) { commands.push_back(std::move(command)); },
        [&](Node &row, InputArea *replacement) {
            if (dispatcher.capturedArea() == &row || dispatcher.pendingTouchPressArea() == &row)
                dispatcher.cancelPointerCapture();
            if (dispatcher.focusedArea() == &row)
                dispatcher.setFocus(replacement);
        });
    rail.setSize(76, 600);
    dispatcher.setSceneRoot(&rail);
    const auto frame = [&] {
        rail.sync("test", 76, 600, 1);
        rail.layout(renderer);
        dispatcher.syncPointerHover();
    };
    frame();
    auto *readAll = areaWithKey(rail, "rail-mark-all-read");
    auto *add = areaWithKey(rail, "rail-add-server");
    auto *scroll = visibleNode<ScrollView>(rail);
    assert(readAll && add && scroll && !visible(*readAll) && !readAll->enabled());
    const float emptyScrollHeight = scroll->height(), addTop = add->y();
    assert(dispatcher.firstTabFocusUnder(readAll) == nullptr);
    serverLog.unread = 1;
    frame();
    assert(areaWithKey(rail, "rail-mark-all-read") == readAll && visible(*readAll));
    assert(readAll->y() + readAll->height() < add->y() && add->y() == addTop);
    assert(scroll->height() < emptyScrollHeight);
    dispatcher.setFocus(readAll);
    for (const auto key : {XKB_KEY_Return, XKB_KEY_space}) {
        const auto before = commands.size();
        dispatcher.keyEvent(key, 0, 0, true);
        dispatcher.keyEvent(key, 0, 0, false);
        assert(commands.size() == before + 1 && commands.back() == "mark-all-read");
    }
    for (const bool touch : {false, true}) {
        auto point = pointerPoint(*readAll);
        const auto before = commands.size();
        dispatcher.pointerMotion(point[0], point[1], 100);
        assert(dispatcher.pointerButton(point[0], point[1], BTN_LEFT, true, 101, 1010, touch));
        serverLog.unread = 0;
        frame();
        assert(!visible(*readAll) && !readAll->enabled());
        assert(!dispatcher.pointerCaptured() && dispatcher.focusedArea() != readAll);
        assert(scroll->height() == emptyScrollHeight && add->y() == addTop);
        serverLog.unread = 1;
        frame();
        dispatcher.pointerButton(point[0], point[1], BTN_LEFT, false, 102, 1020, touch);
        assert(commands.size() == before); // Retired streams cannot click the restored row.
        point = pointerPoint(*readAll);
        dispatcher.pointerButton(point[0], point[1], BTN_LEFT, true, 103, 1030, touch);
        dispatcher.pointerButton(point[0], point[1], BTN_LEFT, false, 104, 1040, touch);
        assert(commands.size() == before + 1 && commands.back() == "mark-all-read");
    }
    dispatcher.setSceneRoot(nullptr);
    Clipboard clipboard;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    const auto uiFrame = [&] {
        ui.update(1600, 1000, 1);
        ui.root().layout(renderer);
    };
    for (const bool touch : {false, true}) {
        uiFrame();
        auto *row = areaWithKey(ui.root(), "rail-mark-all-read");
        assert(row && visible(*row));
        const auto point = pointerPoint(*row);
        dispatcher.setFocus(row);
        ui.beginPointerInteraction(point[0], point[1]);
        assert(!ui.handlePointerButton(BTN_LEFT, true, point[0], point[1], touch));
        dispatcher.pointerButton(point[0], point[1], BTN_LEFT, true, 201, 2010, touch);
        serverLog.unread = 0;
        uiFrame();
        assert(!visible(*row) && !dispatcher.pointerCaptured());
        assert(dispatcher.focusedArea() == areaWithKey(ui.root(), "rail-add-server"));
        serverLog.unread = 1;
        uiFrame();
        assert(ui.handlePointerButton(BTN_LEFT, false, point[0], point[1], touch));
        uiFrame(); // Drain retained-control commands before asserting the retired stream was inert.
        assert(std::none_of(model.actions.begin(), model.actions.end(),
                            [](const Action &action) { return action.type == "read-all"; }));
    }
    click(dispatcher, *areaWithKey(ui.root(), "rail-mark-all-read"));
    uiFrame(); // The rail queues its command; the surface update dispatches it.
    assert(std::any_of(model.actions.begin(), model.actions.end(),
                       [](const Action &action) { return action.type == "read-all"; }));
    dispatcher.setSceneRoot(nullptr);
}

void initialHistoryConsumerCheck() {
    using namespace blast;
    auto model = edgeModel();
    auto &buffer = model.buffers.at(edgeBuffers[0]);
    buffer.kind = "channel";
    buffer.joined = true;
    Message cached;
    cached.id = "cached-initial-history";
    cached.nick = "Peer";
    cached.text = "Retained conversation https://example.invalid/cached-history";
    cached.time = "2026-11-07T12:34:56Z";
    cached.history = true;
    buffer.messages.push_back(cached);
    ++buffer.messagesRevision;
    Clipboard clipboard;
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    const auto frame = [&](float width = 1600.f) {
        ui.update(width, 900, 1);
        ui.root().layout(renderer);
    };
    frame();
    auto *input = inputWithText(ui.root(), edgeDrafts[0]);
    assert(input && visible(*input));
    auto *header = headerFor(*input);
    // Status is independent of the identity subtree, which Expanded navigation hides.
    InputArea *historyArea = nullptr;
    for (const auto &child : header->children())
        if (auto *area = dynamic_cast<InputArea *>(child.get()))
            for (const auto &item : area->children())
                if (dynamic_cast<Spinner *>(item.get()))
                    historyArea = area;
    assert(historyArea);
    const float headerHeight = header->height();
    const auto status = [&] { return visibleNode<Label>(*historyArea); };
    auto *spinner = visibleNode<Spinner>(*header);
    assert(!spinner && !status());
    buffer.loading = true; // Manual history/search activity is not initial synchronization.
    frame();
    assert(!status() && !visibleNode<Spinner>(*header));
    for (const auto navigation :
         {SidebarMode::Expanded, SidebarMode::Compact, SidebarMode::Hidden}) {
        for (const auto state :
             {HistorySyncState::Queued, HistorySyncState::Loading, HistorySyncState::Idle}) {
            buffer.initialHistorySync = state;
            frame();
            assert(header->height() == headerHeight);
            auto *label = status();
            assert(bool(label) == (state != HistorySyncState::Idle));
            if (label) {
                assert(!label->text().empty() && label->width() > 0 && label->height() > 0);
                assert(label->x() >= 0 && label->x() + label->width() <= historyArea->width() + 1);
                assert(label->y() >= 0 &&
                       label->y() + label->height() <= historyArea->height() + 1);
            }
            spinner = visibleNode<Spinner>(*header);
            assert(bool(spinner) == (state == HistorySyncState::Loading));
            assert(!spinner || !spinner->spinning()); // Reduced motion retains legible status.
            auto *retained =
                areaWithTooltip(*paneFor(*input), "https://example.invalid/cached-history");
            assert(retained && visible(*retained));
            assert(visibleCount<TranscriptView>(ui.root()) == 1);
            assert(visibleCount<Sidebar>(ui.root()) == int(navigation != SidebarMode::Hidden));
            assert(buffer.messages.size() == 1 && buffer.messages.front().id == cached.id);
        }
        ui.execute("toggle-navigation");
        frame();
    }
    buffer.initialHistorySync = HistorySyncState::Loading;
    frame(360);
    assert(header->height() == headerHeight && visible(*historyArea));
    assert(historyArea->width() >= 12 && visibleNode<Spinner>(*historyArea));
    assert(historyArea->hasTooltip());
    const auto narrowTooltip = historyArea->tooltipContent();
    const auto *narrowCaption = std::get_if<std::string>(&narrowTooltip);
    assert(narrowCaption && !narrowCaption->empty());
    buffer.initialHistorySync = HistorySyncState::Idle;
    frame();
    auto *line = visibleNode<ModeLine>(ui.root());
    assert(line);
    auto &background = model.buffers.at(edgeBuffers[1]);
    background.initialHistorySync = HistorySyncState::Queued;
    frame();
    auto *remaining = visibleLabelContaining(*line, "1");
    assert(remaining && !status());
    buffer.initialHistorySync = HistorySyncState::Loading;
    frame();
    assert(visible(*remaining) && remaining->text().find("2") != std::string::npos);
    const auto tooltipContent = remaining->tooltipContent();
    const auto *tooltip = std::get_if<std::string>(&tooltipContent);
    assert(tooltip && *tooltip == remaining->text());
    buffer.initialHistorySync = background.initialHistorySync = HistorySyncState::Idle;
    frame();
    assert(!visible(*remaining) && !status());
    model.settings.reducedMotion = false;
    buffer.initialHistorySync = HistorySyncState::Loading;
    frame();
    spinner = visibleNode<Spinner>(*header);
    assert(spinner && spinner->spinning());
    ui.execute("buffer:" + std::string(edgeBuffers[1]));
    frame();
    assert(!visible(*header) && !spinner->spinning());
    buffer.initialHistorySync = HistorySyncState::Idle;
    frame();
    ui.execute("buffer:" + std::string(edgeBuffers[0]));
    frame();
    assert(visible(*header) && !status() && !spinner->spinning());
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void statusIdentityCheck() {
    using namespace blast;
    auto model = edgeModel();
    model.identities[0].nick = "ConfiguredLocal";
    Identity otherIdentity;
    otherIdentity.name = "other";
    otherIdentity.nick = "ConfiguredOther";
    model.identities.push_back(std::move(otherIdentity));
    ServerConfig otherServer;
    otherServer.name = "other";
    otherServer.identity = "other";
    model.servers.push_back(std::move(otherServer));
    auto &other = model.buffers["other/*server*"];
    other.id = "other/*server*";
    other.server = "other";
    other.kind = "server";
    model.networks["other"].state = "Disconnected";
    auto &network = model.networks["test"];
    network.state = "Ready";
    network.nick = "CurrentLocal";
    Theme theme;
    theme.update(model);
    PangoRenderer renderer;
    ModeLine line(model);
    line.setSize(1200, 30);
    const auto frame = [&] {
        line.sync(1, SidebarMode::Expanded);
        line.layout(renderer);
    };
    frame();
    assert(visibleLabelContaining(line, "CurrentLocal"));
    assert(!visibleLabelContaining(line, "ConfiguredLocal"));
    network.nick = "RenamedLocal";
    assert(!areaWithKey(line, "status-mark-all-read"));
    frame();
    assert(visibleLabelContaining(line, "RenamedLocal"));
    assert(!visibleLabelContaining(line, "CurrentLocal"));
    network.state = "Disconnected";
    frame();
    assert(visibleLabelContaining(line, "ConfiguredLocal"));
    assert(!visibleLabelContaining(line, "RenamedLocal"));
    model.activeBuffer = other.id;
    frame();
    assert(visibleLabelContaining(line, "ConfiguredOther"));
    assert(!visibleLabelContaining(line, "ConfiguredLocal"));
    model.activeBuffer = edgeBuffers[0];
    model.servers[0].alias.assign(200, 'a');
    network.nick.assign(30, 'n');
    network.state = "Ready";
    model.status = "Reconnect failed; retained draft.";
    line.setSize(720, 30);
    frame();
    auto *feedback = labelWithText(line, model.status);
    assert(feedback && visible(*feedback));
    const auto feedbackNatural =
        renderer.measureText(feedback->text(), feedback->fontSize(), feedback->font(), 0, 1,
                             TextAlign::Start, TextEllipsize::End, false);
    assert(feedback->width() + .5f >= feedbackNatural.width);
    model.networks.erase("test");
    model.servers.erase(model.servers.begin());
    model.networks["other"].state = "Ready";
    model.networks["other"].nick = "UnrelatedLiveNick";
    model.status.clear();
    line.setSize(1200, 30);
    frame();
    assert(visibleLabelContaining(line, "Network"));
    assert(visibleLabelContaining(line, "Disconnected"));
    assert(!visibleLabelContaining(line, "UnrelatedLiveNick"));
    // A serverless global view has no single network connection or nick to report.
    auto &feed = model.buffers[GlobalMentionsBufferId];
    feed.id = GlobalMentionsBufferId;
    feed.kind = "mentions";
    feed.target = "Mentions";
    model.activeBuffer = feed.id;
    model.networks["test"].state = "Ready";
    model.networks["test"].nick = "LocalConnectedNick";
    frame();
    assert(!visibleLabelContaining(line, "Disconnected"));
    assert(!visibleLabelContaining(line, "UnrelatedLiveNick"));
    assert(!visibleLabelContaining(line, "LocalConnectedNick"));
    auto *globalContext = visibleLabelContaining(line, "All networks");
    auto *readOnly = visibleLabelContaining(line, "Read only");
    assert(globalContext && readOnly && visibleLabelContaining(line, "Mentions"));
    assert(globalContext->width() > 0 && readOnly->width() > 0);
    // Unrelated connection failure cannot change global view identity or status.
    model.networks["other"].state = "Failed";
    model.networks["other"].error = "Unrelated failure";
    frame();
    assert(!visibleLabelContaining(line, "Disconnected"));
    assert(!visibleLabelContaining(line, "Unrelated failure"));
    assert(visibleLabelContaining(line, "All networks") &&
           visibleLabelContaining(line, "Read only"));
}

void selectedServerAvyCheck() {
    using namespace blast;
    auto model = edgeModel();
    const auto add = [&](std::string id, std::string server, std::string target, std::string kind) {
        auto &buffer = model.buffers[id];
        buffer.id = std::move(id);
        buffer.server = std::move(server);
        buffer.target = std::move(target);
        buffer.kind = std::move(kind);
        buffer.draft = buffer.target + " draft";
    };
    for (const auto *name : {"#alpha", "#alpine", "#beta"})
        add("test/" + std::string(name), "test", name, "channel");
    for (const auto *name : {"zulu", "zumba"})
        add("test/" + std::string(name), "test", name, "query");
    add("test/9lives", "test", "9lives", "query");
    for (int i = 0; i < 30; ++i) {
        const auto name = "middle" + std::to_string(i);
        add("test/" + name, "test", name, "query");
        const auto remoteName = "middle-remote" + std::to_string(i);
        add("other/" + remoteName, "other", remoteName, "query");
    }
    add("test/*server*", "test", "*server*", "server");
    add("test/search-results", "test", "search-results", "search");
    add("other/#alpha", "other", "#alpha", "channel");
    add("other/#onlyother", "other", "#onlyother", "channel");
    ServerConfig other;
    other.name = "other";
    other.host = "other.invalid";
    model.servers.push_back(std::move(other));
    model.networks["other"].nick = "OtherMe";
    auto &originBuffer = model.buffers.at(edgeBuffers[0]);
    Message focusAnchor;
    focusAnchor.id = "avy-transcript-focus-anchor";
    focusAnchor.nick = "Peer";
    focusAnchor.text = "Retain native transcript keyboard focus when sidebar rows change.";
    focusAnchor.time = "2026-11-07T12:34:56Z";
    originBuffer.messages.push_back(std::move(focusAnchor));
    ++originBuffer.messagesRevision;
    Clipboard clipboard;
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    MenuHost menu;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    ui.root().setPopupContext(&menu);
    const auto frame = [&] {
        ui.update(1600, 900, 1);
        ui.root().layout(renderer);
    };
    const auto prefix = [&](std::uint32_t finalKey) {
        nativeKey(ui, dispatcher, 'x', KeyMod::Ctrl);
        nativeKey(ui, dispatcher, finalKey, 0, finalKey);
        frame();
    };
    frame();
    auto *input = inputWithText(ui.root(), edgeDrafts[0]);
    assert(input && visible(*input));
    auto *searchArea = areaWithKey(ui.root(), "sidebar-search");
    auto *search = searchArea ? dynamic_cast<Input *>(searchArea->textInputClient()) : nullptr;
    assert(search);
    Sidebar *sidebar = nullptr;
    for (auto *node = searchArea->parent(); node; node = node->parent())
        if (auto *candidate = dynamic_cast<Sidebar *>(node))
            sidebar = candidate;
    assert(sidebar);
    auto state = input->editingState();
    state.current.cursorPos = 2;
    state.current.selectionAnchor = 1;
    input->restoreEditingState(state);
    state = input->editingState();
    dispatcher.setFocus(input->inputArea());
    assert(model.settings.bindings.at("jump-channel") == "C-x j");
    assert(model.settings.bindings.at("switch-buffer") == "C-x b");
    prefix('j');
    auto *jump = areaWithKey(ui.root(), "channel-jump");
    assert(jump && visible(*jump) && dispatcher.focusedArea() == jump);
    assert(!jump->textInputClient());
    const auto hint = [&](std::string_view id) -> Label * {
        for (const auto &row : sidebar->hintRects())
            if (row.buffer == id)
                return paintedHintAt(*jump, row);
        return nullptr;
    };
    auto *alpha = hint("test/#alpha");
    auto *alpine = hint("test/#alpine");
    auto *beta = hint("test/#beta");
    assert(alpha && alpine && beta && visible(*alpha) && visible(*alpine) && visible(*beta));
    const auto alphaKeys = alpha->text(), alpineKeys = alpine->text();
    assert(alphaKeys == "aa" && alpineKeys == "as" && beta->text() == "b");
    assert(!hint("test/*server*") && !hint("test/search-results") && !hint("other/#alpha"));
    assert(input->editingStateMatches(state) && model.activeBuffer == edgeBuffers[0]);
    // Unsupported keys must not fall through to the native composer.
    for (const bool pressed : {true, false})
        assert(ui.handleKey({.sym = XKB_KEY_exclam, .utf32 = '!', .pressed = pressed}));
    frame();
    assert(!visible(*jump) && dispatcher.focusedArea() == input->inputArea());
    assert(input->editingStateMatches(state));
    prefix('j');
    nativeKey(ui, dispatcher, 'x', 0, 'x'); // No candidate has this initial.
    frame();
    assert(!visible(*jump) && input->editingStateMatches(state));
    prefix('j');
    nativeKey(ui, dispatcher, XKB_KEY_Escape);
    frame();
    assert(!visible(*jump) && dispatcher.focusedArea() == input->inputArea());
    assert(input->editingStateMatches(state));

    model.settings.bindings["jump-channel"] = "C-x q";
    prefix('j');
    assert(!visible(*jump) && input->editingStateMatches(state));
    prefix('q');
    assert(visible(*jump));
    nativeKey(ui, dispatcher, 'a', 0, 'a');
    frame();
    alpha = hint("test/#alpha");
    alpine = hint("test/#alpine");
    assert(alpha && alpine && visible(*alpha) && visible(*alpine));
    assert(alpha->text() == "a" && alpine->text() == "s");
    assert(model.activeBuffer == edgeBuffers[0] && !hint("test/#beta"));
    nativeKey(ui, dispatcher, XKB_KEY_BackSpace);
    frame();
    assert(hint("test/#alpha")->text() == alphaKeys && hint("test/#beta")->text() == "b");
    for (const auto key : alpineKeys)
        nativeKey(ui, dispatcher, key, 0, key);
    frame();
    assert(!visible(*jump) && model.activeBuffer == "test/#alpine");
    auto *alpineInput = inputWithText(ui.root(), "#alpine draft");
    assert(alpineInput && visible(*alpineInput) &&
           dispatcher.focusedArea() == alpineInput->inputArea());
    assert(input->editingStateMatches(state));
    prefix('q');
    auto *numericHint = hint("test/9lives");
    assert(numericHint && numericHint->text() == "9");
    nativeKey(ui, dispatcher, XKB_KEY_KP_9, 0, '9');
    frame();
    assert(!visible(*jump) && model.activeBuffer == "test/9lives");
    auto *numericInput = inputWithText(ui.root(), "9lives draft");
    assert(numericInput && visible(*numericInput) &&
           dispatcher.focusedArea() == numericInput->inputArea());
    assert(input->editingStateMatches(state));

    // The global picker remains independent of the selected-network jump scope.
    prefix('b');
    auto *picker = dispatcher.focusedArea()
                       ? dynamic_cast<Input *>(dispatcher.focusedArea()->textInputClient())
                       : nullptr;
    assert(picker && picker != input && picker != alpineInput && visible(*picker));
    TextInputEdit pickerEdit{};
    pickerEdit.commitText = "onlyother";
    pickerEdit.hasCommitText = true;
    static_cast<TextInputClient &>(*picker).textInputApplyEdit(pickerEdit);
    assert(picker->value() == "onlyother");
    frame();
    nativeKey(ui, dispatcher, XKB_KEY_Return);
    frame();
    assert(model.activeBuffer == "other/#onlyother");
    prefix('q');
    assert(visible(*jump) && hint("other/#alpha") && hint("other/#onlyother"));
    assert(!hint("test/#alpha") && !hint("test/first"));
    nativeKey(ui, dispatcher, 'a', 0, 'a');
    frame();
    assert(model.activeBuffer == "other/#alpha" && !visible(*jump));

    ui.execute("buffer:" + std::string(edgeBuffers[0]));
    frame();
    ScrollView *scroll = nullptr;
    auto *firstRow = areaWithKey(*sidebar, std::string("buffer:") + edgeBuffers[0]);
    assert(firstRow);
    for (auto *node = firstRow->parent(); node; node = node->parent())
        if (auto *candidate = dynamic_cast<ScrollView *>(node))
            scroll = candidate;
    assert(scroll);
    Button *dms = nullptr;
    for (const auto &child : search->parent()->children())
        if (auto *button = dynamic_cast<Button *>(child.get());
            button && button->label()->text() == "DMs")
            dms = button;
    assert(dms);
    auto *unreadFilter = areaWithKey(*sidebar, "sidebar-filter:unread");
    assert(unreadFilter && visible(*unreadFilter));
    model.buffers.at("test/middle0").unread = 1;
    click(dispatcher, *unreadFilter);
    frame();
    assert(visible(*areaWithKey(*sidebar, "buffer:test/middle0")));
    assert(!visible(*areaWithKey(*sidebar, "buffer:test/middle1")));
    TranscriptView *transcript = nullptr;
    for (const auto &child : paneFor(*input)->children())
        if (auto *view = dynamic_cast<TranscriptView *>(child.get()))
            transcript = view;
    assert(transcript && visible(*transcript));
    const auto keyboardArea = [&](auto &&self, Node &node) -> InputArea * {
        if (auto *area = dynamic_cast<InputArea *>(&node);
            area && visible(*area) && area->tabFocusKey().starts_with("transcript:") &&
            area->tabFocusKey().ends_with(":keyboard"))
            return area;
        for (const auto &child : node.children())
            if (auto *area = self(self, *child))
                return area;
        return nullptr;
    };
    auto *transcriptKeyboard = keyboardArea(keyboardArea, *transcript);
    assert(transcriptKeyboard);
    dispatcher.setFocus(transcriptKeyboard);
    prefix('q');
    assert(visible(*jump));
    model.buffers.at("test/middle0").unread = 0;
    model.buffers.at("test/middle1").unread = 1;
    model.buffers.at("test/middle2").unread = 2;
    frame();
    nativeKey(ui, dispatcher, XKB_KEY_Escape);
    frame();
    assert(!visible(*jump) && dispatcher.focusedArea() == transcriptKeyboard);
    assert(visible(*transcriptKeyboard));
    assert(!visible(*areaWithKey(*sidebar, "buffer:test/middle0")));
    assert(visible(*areaWithKey(*sidebar, "buffer:test/middle1")));
    assert(visible(*areaWithKey(*sidebar, "buffer:test/middle2")));
    assert(model.activeBuffer == edgeBuffers[0] && input->editingStateMatches(state));
    model.buffers.at("test/middle1").unread = 0;
    model.buffers.at("test/middle2").unread = 0;
    click(dispatcher, *dms);
    search->setValue("middle");
    frame();
    assert(!visible(*areaWithKey(*sidebar, "buffer:test/#alpha")));
    scroll->setScrollOffset(160);
    frame();
    const float savedOffset = scroll->scrollOffset();
    assert(savedOffset > 0);
    dispatcher.setFocus(searchArea);
    std::uint32_t pointerSerial = 10000;
    const auto nativeMotion = [&](const std::array<float, 2> &point, bool touch) {
        ui.handlePointerMotion(point[0], point[1], touch);
        dispatcher.pointerMotion(point[0], point[1], ++pointerSerial);
    };
    const auto nativeButton = [&](const std::array<float, 2> &point, bool pressed, bool touch) {
        ++pointerSerial;
        if (pressed)
            ui.beginPointerInteraction(point[0], point[1]);
        if (ui.handlePointerButton(BTN_LEFT, pressed, point[0], point[1], touch))
            return true;
        assert(dispatcher.pointerButton(point[0], point[1], BTN_LEFT, pressed, pointerSerial,
                                        pointerSerial * 10, touch));
        return false;
    };
    // A competing touch must not interfere with dismissal of the held native mouse source.
    dispatcher.setFocus(nullptr);
    prefix('q');
    assert(visible(*jump) && dispatcher.focusedArea() == jump);
    const auto heldPoint = pointerPoint(*headerTool(*input, "More conversation actions"));
    nativeMotion(heldPoint, false);
    assert(!nativeButton(heldPoint, true, false));
    nativeButton(heldPoint, true, true);
    nativeKey(ui, dispatcher, XKB_KEY_Escape);
    assert(!visible(*jump) && !dispatcher.focusedArea() && !dispatcher.pointerCaptured());
    assert(nativeButton(heldPoint, false, false));
    assert(nativeButton(heldPoint, false, true));
    frame();
    assert(!menu.open && !dispatcher.focusedArea() && scroll->scrollOffset() == savedOffset);
    assert(model.activeBuffer == edgeBuffers[0] && input->editingStateMatches(state));

    dispatcher.setFocus(searchArea);
    prefix('q');
    const auto touchPoint = pointerPoint(*sidebar);
    nativeMotion(touchPoint, true);
    assert(!nativeButton(touchPoint, true, true));
    const float beforeDrag = scroll->scrollOffset();
    auto dragPoint = touchPoint;
    dragPoint[1] -= 4;
    nativeMotion(dragPoint, true);
    frame();
    assert(visible(*jump) && scroll->scrollOffset() == beforeDrag);
    dragPoint[1] = touchPoint[1] - 48;
    nativeMotion(dragPoint, true);
    frame();
    assert(visible(*jump) && scroll->scrollOffset() > beforeDrag);
    assert(nativeButton(dragPoint, false, true));
    frame();
    assert(visible(*jump) && dispatcher.focusedArea() == jump && !dispatcher.pointerCaptured());
    // The next stationary native touch is a tap, not the preceding drag's release.
    nativeMotion(touchPoint, true);
    assert(!nativeButton(touchPoint, true, true));
    assert(!nativeButton(touchPoint, false, true));
    assert(!visible(*jump) && dispatcher.focusedArea() == searchArea);
    frame();
    assert(scroll->scrollOffset() == savedOffset && search->value() == "middle");
    assert(model.activeBuffer == edgeBuffers[0] && input->editingStateMatches(state));
    for (const bool controlCancel : {false, true}) {
        prefix('q');
        assert(visible(*jump) && !hint("test/zulu") && !hint("test/zumba"));
        assert(search->value() == "middle");
        if (!controlCancel) {
            for (std::size_t page = 0; page < model.buffers.size() && !hint("test/zumba"); ++page) {
                nativeKey(ui, dispatcher, XKB_KEY_Page_Down);
                frame();
            }
            auto *zulu = hint("test/zulu");
            auto *zumba = hint("test/zumba");
            assert(zulu && zumba && zulu->text() == "za" && zumba->text() == "zs");
            nativeKey(ui, dispatcher, XKB_KEY_Page_Down);
            frame();
            const float bottom = scroll->scrollOffset();
            assert(bottom == scroll->maxScrollOffset());
            // The clamped Down must not cancel the following Up before layout.
            nativeKey(ui, dispatcher, XKB_KEY_Down);
            nativeKey(ui, dispatcher, XKB_KEY_Up);
            frame();
            assert(scroll->scrollOffset() < bottom && input->editingStateMatches(state));
            const float pagedOffset = scroll->scrollOffset();
            assert(pagedOffset > savedOffset && input->editingStateMatches(state));
            nativeKey(ui, dispatcher, XKB_KEY_Page_Up);
            frame();
            assert(scroll->scrollOffset() < pagedOffset);
        }
        nativeKey(ui, dispatcher, 'z', 0, 'z');
        frame();
        auto *zulu = hint("test/zulu");
        auto *zumba = hint("test/zumba");
        assert(zulu && zumba && zulu->text() == "a" && zumba->text() == "s");
        assert(scroll->scrollOffset() == 0);
        assert(!visible(*areaWithKey(*sidebar, "buffer:test/middle0")));
        if (controlCancel)
            nativeKey(ui, dispatcher, 'g', KeyMod::Ctrl);
        else
            nativeKey(ui, dispatcher, XKB_KEY_Escape);
        frame();
        assert(!visible(*jump) && dispatcher.focusedArea() == searchArea);
        assert(search->value() == "middle" && scroll->scrollOffset() == savedOffset);
        assert(!visible(*areaWithKey(*sidebar, "buffer:test/#alpha")));
        assert(visible(*areaWithKey(*sidebar, "buffer:test/middle0")));
        assert(model.activeBuffer == edgeBuffers[0] && input->editingStateMatches(state));
    }
    // Later paging must win over a prefix reset queued before the same layout.
    prefix('q');
    nativeKey(ui, dispatcher, 'm', 0, 'm');
    nativeKey(ui, dispatcher, XKB_KEY_Page_Down);
    frame();
    assert(visible(*jump) && scroll->scrollOffset() > 0);
    assert(search->value() == "middle" && input->editingStateMatches(state));
    nativeKey(ui, dispatcher, XKB_KEY_Escape);
    // A second jump before layout must retain the original cancellation offset.
    nativeKey(ui, dispatcher, 'x', KeyMod::Ctrl);
    nativeKey(ui, dispatcher, 'q', 0, 'q');
    nativeKey(ui, dispatcher, XKB_KEY_Escape);
    frame();
    assert(dispatcher.focusedArea() == searchArea && scroll->scrollOffset() == savedOffset);

    // Entering and cancelling before layout must discard an older row-focus reveal.
    auto *offscreenRow = areaWithKey(*sidebar, "buffer:test/middle0");
    assert(offscreenRow && visible(*offscreenRow));
    dispatcher.setFocus(offscreenRow);
    assert(scroll->scrollOffset() == savedOffset);
    nativeKey(ui, dispatcher, 'x', KeyMod::Ctrl);
    nativeKey(ui, dispatcher, 'q', 0, 'q');
    nativeKey(ui, dispatcher, XKB_KEY_Escape);
    frame();
    assert(!visible(*jump) && dispatcher.focusedArea() == offscreenRow);
    assert(scroll->scrollOffset() == savedOffset && search->value() == "middle");
    assert(input->editingStateMatches(state));
    dispatcher.setFocus(searchArea);
    // A stale short-list layout must not acknowledge cancellation before normal sync.
    constexpr std::array<std::string_view, 2> shortlist{"test/zulu", "test/zumba"};
    sidebar->setHintCandidates(shortlist);
    frame();
    assert(scroll->scrollOffset() == 0 && !visible(*jump));
    sidebar->clearHintCandidates(false);
    sidebar->finishHintFocusRestore();
    ui.root().layout(renderer);
    frame();
    assert(scroll->scrollOffset() == savedOffset && search->value() == "middle");
    assert(!visible(*areaWithKey(*sidebar, "buffer:test/#alpha")));

    // A later native search edit supersedes cancellation's queued offset.
    prefix('q');
    nativeKey(ui, dispatcher, 'm', 0, 'm');
    nativeKey(ui, dispatcher, XKB_KEY_Page_Down);
    frame();
    nativeKey(ui, dispatcher, XKB_KEY_Escape);
    search->selectAll();
    TextInputEdit filterEdit{};
    filterEdit.commitText = "m";
    filterEdit.hasCommitText = true;
    static_cast<TextInputClient &>(*search).textInputApplyEdit(filterEdit);
    frame();
    assert(search->value() == "m" && scroll->maxScrollOffset() > savedOffset);
    assert(scroll->scrollOffset() == 0 && input->editingStateMatches(state));
    search->selectAll();
    filterEdit.commitText = "middle";
    static_cast<TextInputClient &>(*search).textInputApplyEdit(filterEdit);
    frame();
    scroll->setScrollOffset(savedOffset);
    frame();

    // A later real row focus must reveal that row instead of applying old scroll.
    prefix('q');
    nativeKey(ui, dispatcher, 'm', 0, 'm');
    nativeKey(ui, dispatcher, XKB_KEY_Page_Down);
    frame();
    nativeKey(ui, dispatcher, XKB_KEY_Escape);
    auto *revealRow = areaWithKey(*sidebar, "buffer:test/middle9");
    assert(revealRow && visible(*revealRow));
    std::array<float, 4> rowBounds{}, viewBounds{};
    Node::transformedBounds(revealRow, rowBounds[0], rowBounds[1], rowBounds[2], rowBounds[3]);
    Node::transformedBounds(scroll, viewBounds[0], viewBounds[1], viewBounds[2], viewBounds[3]);
    assert(rowBounds[3] > viewBounds[3]);
    dispatcher.setFocus(revealRow);
    frame();
    Node::transformedBounds(revealRow, rowBounds[0], rowBounds[1], rowBounds[2], rowBounds[3]);
    assert(dispatcher.focusedArea() == revealRow && scroll->scrollOffset() != savedOffset);
    assert(rowBounds[1] + .5f >= viewBounds[1] && rowBounds[3] <= viewBounds[3] + .5f);
    assert(search->value() == "middle" && input->editingStateMatches(state));
    dispatcher.setFocus(searchArea);
    scroll->setScrollOffset(savedOffset);
    frame();

    // A cancelled network's pending offset must never scroll the next network.
    prefix('q');
    nativeKey(ui, dispatcher, 'm', 0, 'm');
    nativeKey(ui, dispatcher, XKB_KEY_Page_Down);
    frame();
    assert(scroll->scrollOffset() > 0);
    nativeKey(ui, dispatcher, XKB_KEY_Escape);
    auto *otherRail = areaWithKey(ui.root(), "server:other");
    auto *testRail = areaWithKey(ui.root(), "server:test");
    assert(otherRail && testRail && visible(*otherRail) && visible(*testRail));
    click(dispatcher, *otherRail);
    frame();
    assert(model.activeBuffer == "other/#alpha");
    assert(scroll->maxScrollOffset() > savedOffset && scroll->scrollOffset() == 0);
    assert(visible(*areaWithKey(*sidebar, "buffer:other/middle-remote0")));
    assert(search->value() == "middle" && input->editingStateMatches(state));
    click(dispatcher, *testRail);
    frame();
    assert(model.activeBuffer == edgeBuffers[0]);
    scroll->setScrollOffset(savedOffset);
    frame();
    dispatcher.setFocus(searchArea);
    prefix('q');
    nativeKey(ui, dispatcher, 'z', 0, 'z');
    frame();
    nativeKey(ui, dispatcher, 's', 0, 's');
    frame();
    assert(model.activeBuffer == "test/zumba" && !visible(*jump));
    assert(search->value() == "middle");
    auto *committed = areaWithKey(*sidebar, "buffer:test/zumba");
    auto *zumbaInput = inputWithText(ui.root(), "zumba draft");
    assert(committed && visible(*committed) && zumbaInput && visible(*zumbaInput));
    assert(dispatcher.focusedArea() == zumbaInput->inputArea());

    ui.execute("buffer:" + std::string(edgeBuffers[0]));
    frame();
    auto *header = headerFor(*input);
    assert(visible(*header) && !visibleLabelContaining(*header, "first"));
    ui.execute("toggle-navigation");
    frame();
    assert(visibleCount<Sidebar>(ui.root()) == 1 && sidebar->width() == 56);
    auto *title = visibleLabelContaining(*header, "first");
    assert(title && visible(*headerTool(*input, "Search conversation")) &&
           visible(*headerTool(*input, "More conversation actions")));
    dispatcher.setFocus(input->inputArea());
    const auto point = pointerPoint(*title);
    dispatcher.pointerButton(point[0], point[1], BTN_LEFT, true, 500, 5000, false);
    dispatcher.pointerButton(point[0], point[1], BTN_LEFT, false, 501, 5010, false);
    frame();
    assert(!menu.open && model.activeBuffer == edgeBuffers[0]);
    assert(dispatcher.focusedArea() == input->inputArea() && input->editingStateMatches(state));
    ui.execute("toggle-navigation");
    frame();
    assert(model.settings.workspace.at("native_navigation_mode") == "hidden");
    assert(visibleCount<Sidebar>(ui.root()) == 0 && visible(*title));
    const auto hiddenReadMarker = model.buffers.at(edgeBuffers[0]).readMarker;
    for (const bool controlCancel : {false, true}) {
        prefix('q');
        assert(visible(*jump) && sidebar->width() == 56 && visible(*sidebar));
        assert(model.settings.workspace.at("native_navigation_mode") == "hidden");
        if (controlCancel)
            nativeKey(ui, dispatcher, 'g', KeyMod::Ctrl);
        else
            nativeKey(ui, dispatcher, XKB_KEY_Escape);
        frame();
        assert(!visible(*jump) && !visible(*sidebar));
        assert(model.settings.workspace.at("native_navigation_mode") == "hidden");
        assert(model.activeBuffer == edgeBuffers[0] && input->editingStateMatches(state));
        assert(dispatcher.focusedArea() == input->inputArea());
        assert(model.buffers.at(edgeBuffers[0]).readMarker == hiddenReadMarker);
    }
    prefix('q');
    auto *hiddenChoice = hint("test/#alpha");
    assert(hiddenChoice && visible(*hiddenChoice));
    const auto hiddenKeys = hiddenChoice->text();
    for (const auto key : hiddenKeys) {
        nativeKey(ui, dispatcher, key, 0, key);
        frame();
    }
    assert(model.activeBuffer == "test/#alpha" && !visible(*jump) && !visible(*sidebar));
    assert(model.settings.workspace.at("native_navigation_mode") == "hidden");
    assert(input->editingStateMatches(state));
    assert(visibleCount<TranscriptView>(ui.root()) == 1);
    assert(model.settings.workspace.at("visible_buffers") == "test/#alpha");
    auto *chosenTranscript = visibleNode<TranscriptView>(ui.root());
    assert(chosenTranscript);
    auto *chosenInput = inputWithText(*chosenTranscript->parent(), "#alpha draft");
    assert(chosenInput && visible(*chosenInput));
    assert(dispatcher.focusedArea() == chosenInput->inputArea());
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void renameFixtureBuffer(blast::Model &model, const std::string &from, const std::string &to) {
    auto buffer = model.buffers.extract(from);
    assert(!buffer.empty() && model.bufferRenamed);
    buffer.key() = to;
    buffer.mapped().id = to;
    buffer.mapped().target = to.substr(to.find('/') + 1);
    model.buffers.insert(std::move(buffer));
    model.bufferRenamed(from, to);
}
void sidebarActivityPinsCheck() {
    using namespace blast;
    Model model;
    constexpr std::array ids{"one/#alpha", "one/person", "one/#beta", "one/visitor"};
    for (std::size_t i = 0; i < ids.size(); ++i) {
        auto &buffer = model.buffers[ids[i]];
        buffer.id = ids[i];
        buffer.server = "one";
        buffer.target = std::string(ids[i]).substr(4);
        buffer.kind = i % 2 == 0 ? "channel" : "query";
        buffer.lastChatTimestamp = 400 - i * 100;
        buffer.lastChatSequence = i + 1;
    }
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    MenuHost menu;
    std::vector<std::string> commands;
    Sidebar sidebar(model, [&](std::string command) { commands.push_back(std::move(command)); });
    sidebar.setPopupContext(&menu);
    dispatcher.setSceneRoot(&sidebar);
    const auto frame = [&] {
        sidebar.setSize(320, 900);
        sidebar.sync("one", ids[0], 320, 900, 1, SidebarMode::Expanded);
        sidebar.layout(renderer);
    };
    frame();
    std::array<InputArea *, 4> rows{};
    for (std::size_t i = 0; i < ids.size(); ++i) {
        rows[i] = areaWithKey(sidebar, "buffer:" + std::string(ids[i]));
        assert(rows[i] && visible(*rows[i]));
    }
    const auto order = [&](std::array<int, 4> expected) {
        for (std::size_t i = 0; i < rows.size(); ++i) {
            assert(areaWithKey(sidebar, "buffer:" + std::string(ids[i])) == rows[i]);
            assert(visible(*rows[i]));
        }
        for (std::size_t i = 1; i < expected.size(); ++i)
            assert(pointerPoint(*rows[expected[i - 1]])[1] < pointerPoint(*rows[expected[i]])[1]);
    };
    order({0, 1, 2, 3}); // Channels and queries share one activity order.
    dispatcher.setFocus(rows[1]);
    const auto oldPoint = pointerPoint(*rows[3]);
    model.buffers.at(ids[3]).lastChatTimestamp = 500;
    frame();
    order({3, 0, 1, 2});
    assert(pointerPoint(*rows[3])[1] < oldPoint[1]);
    assert(dispatcher.focusedArea() == rows[1]);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        click(dispatcher, *rows[i]);
        assert(commands.back() == "buffer:" + std::string(ids[i]));
    }

    // Backfill, typing and presence update presentation, not chat activity metadata.
    auto &quiet = model.buffers.at(ids[2]);
    Message history;
    history.id = "newer-history-only";
    history.time = "2099-01-01T00:00:00Z";
    history.timestamp = 4070908800;
    history.history = true;
    quiet.messages.push_back(history);
    ++quiet.messagesRevision;
    frame();
    order({3, 0, 1, 2});
    quiet.typing["Peer"] = model.now + 30;
    frame();
    order({3, 0, 1, 2});
    Message presence = history;
    presence.id = "newer-presence-only";
    presence.history = false;
    presence.presence = true;
    presence.activity = ActivityKind::Join;
    quiet.messages.push_back(std::move(presence));
    ++quiet.messagesRevision;
    ++quiet.participationRevision;
    quiet.nicks.push_back({"Peer", {}, {}});
    frame();
    order({3, 0, 1, 2});

    // A held popup belongs to its full ID, even when another row takes its old slot.
    click(dispatcher, *rows[2], BTN_RIGHT);
    assert(menu.open);
    model.buffers.at(ids[2]).lastChatTimestamp = 600;
    frame();
    order({2, 3, 0, 1});
    assert(menu.open);
    menu.select("Pin chat");
    assert(model.actions.back().type == "pin_buffer" && model.actions.back().buffer == ids[2] &&
           model.actions.back().text == "on");
    model.settings.pinnedBuffers = {ids[2]};
    frame();
    order({2, 3, 0, 1});
    click(dispatcher, *rows[1], BTN_RIGHT);
    menu.select("Pin chat");
    assert(model.actions.back().type == "pin_buffer" && model.actions.back().buffer == ids[1] &&
           model.actions.back().text == "on");
    model.settings.pinnedBuffers.insert(model.settings.pinnedBuffers.begin(), ids[1]);
    frame();
    order({1, 2, 3, 0}); // Newest pin first, not newest message first.
    model.buffers.at(ids[0]).lastChatTimestamp = 900;
    model.buffers.at(ids[2]).lastChatTimestamp = 1000;
    frame();
    order({1, 2, 0, 3});
    click(dispatcher, *rows[1], BTN_RIGHT);
    model.buffers.at(ids[3]).lastChatTimestamp = 1100;
    frame();
    order({1, 2, 3, 0});
    menu.select("Unpin chat");
    assert(model.actions.back().type == "pin_buffer" && model.actions.back().buffer == ids[1] &&
           model.actions.back().text == "off");
    model.settings.pinnedBuffers.erase(model.settings.pinnedBuffers.begin());
    frame();
    order({2, 3, 0, 1}); // Unpin rejoins its actual activity position.
    click(dispatcher, *rows[1]);
    assert(commands.back() == "buffer:" + std::string(ids[1]));

    // Live relationship changes reorder the native rows, not just their menu actions.
    model.settings.friends = {"PERSON"};
    frame();
    order({2, 1, 3, 0});
    auto *friends = visibleLabelContaining(sidebar, "Friends");
    assert(friends && pointerPoint(*rows[2])[1] < pointerPoint(*friends)[1] &&
           pointerPoint(*friends)[1] < pointerPoint(*rows[1])[1]);
    model.settings.pinnedBuffers = {ids[1], ids[2]};
    frame();
    order({1, 2, 3, 0});
    assert(!visibleLabelContaining(sidebar, "Friends")); // A pinned friend has no second row.
    const auto countRows = [&](auto &&self, Node &node, std::string_view key) -> int {
        int count = 0;
        if (auto *area = dynamic_cast<InputArea *>(&node);
            area && visible(*area) && area->tabFocusKey() == key)
            ++count;
        for (const auto &child : node.children())
            count += self(self, *child, key);
        return count;
    };
    assert(countRows(countRows, sidebar, "buffer:" + std::string(ids[1])) == 1);
    model.settings.friends.insert("VISITOR");
    frame();
    order({1, 2, 3, 0});
    friends = visibleLabelContaining(sidebar, "Friends");
    assert(friends && pointerPoint(*rows[2])[1] < pointerPoint(*friends)[1] &&
           pointerPoint(*friends)[1] < pointerPoint(*rows[3])[1]);

    model.settings.pinnedBuffers = {ids[2]};
    model.settings.friends = {"pal{"};
    model.buffers.at(ids[1]).target = "Pal[";
    model.networks["one"].caseMapping = "rfc1459";
    frame();
    order({2, 1, 3, 0});
    model.networks["one"].caseMapping = "ascii";
    frame();
    order({2, 3, 0, 1});
    assert(!visibleLabelContaining(sidebar, "Friends"));
    model.networks["one"].caseMapping = "rfc1459";
    frame();
    order({2, 1, 3, 0});
    model.buffers.at(ids[1]).target = "Stranger";
    frame();
    order({2, 3, 0, 1}); // Same buffer ID, renamed query target is no longer a friend.
    model.settings.friends = {"Stranger", "visitor"};
    frame();
    order({2, 3, 1, 0}); // Friends retain their activity comparator within the partition.

    auto *search = inputWithText(sidebar, "");
    assert(search);
    search->setValue("Stranger");
    frame();
    assert(visible(*rows[0]) && visible(*rows[1]));
    assert(!visible(*rows[2]) && !visible(*rows[3])); // Neither pins nor friends bypass search.
    assert(pointerPoint(*rows[0])[1] < pointerPoint(*rows[1])[1]);
    assert(countRows(countRows, sidebar, "buffer:" + std::string(ids[0])) == 1);
    search->setValue("");
    auto *dms = areaWithKey(sidebar, "sidebar-filter:dms");
    assert(dms && visible(*dms));
    click(dispatcher, *dms);
    frame();
    assert(visible(*rows[0]) && !visible(*rows[2]));
    assert(pointerPoint(*rows[0])[1] < pointerPoint(*rows[3])[1] &&
           pointerPoint(*rows[3])[1] < pointerPoint(*rows[1])[1]);
    assert(countRows(countRows, sidebar, "buffer:" + std::string(ids[0])) == 1);
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void activityReorderAvySourceIdentityCheck() {
    using namespace blast;
    auto model = edgeModel();
    for (std::size_t i = 0; i < edgeBuffers.size(); ++i) {
        auto &buffer = model.buffers.at(edgeBuffers[i]);
        buffer.lastChatTimestamp = 400 - i * 100;
        Message message;
        message.id = "same-raw-message-id";
        message.nick = "Peer" + std::to_string(i);
        message.text = "Distinct source with a repeated raw message ID";
        message.time = "2026-11-07T12:34:56Z";
        buffer.messages.push_back(std::move(message));
        ++buffer.messagesRevision;
    }
    model.networks["test"].caps = {"message-tags"};
    model.networks["test"].state = "Ready";
    Clipboard clipboard;
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    MenuHost menu;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    ui.root().setPopupContext(&menu);
    const auto frame = [&] {
        ui.update(1600, 900, 1);
        ui.root().layout(renderer);
    };
    frame();
    auto *input = inputWithText(ui.root(), edgeDrafts[0]);
    assert(input && visible(*input));
    auto state = input->editingState();
    state.current.cursorPos = 2;
    state.current.selectionAnchor = 1;
    input->restoreEditingState(state);
    state = input->editingState();
    dispatcher.setFocus(input->inputArea());
    nativeKey(ui, dispatcher, 'p', KeyMod::Ctrl);
    frame();
    auto *transcript = visibleNode<TranscriptView>(*paneFor(*input));
    assert(transcript);
    const auto selected = transcript->selectedMessage();
    auto *messageFocus = dispatcher.focusedArea();
    assert(selected && selected->buffer == edgeBuffers[0] && selected->nick == "Peer0");
    auto *originRow = areaWithKey(ui.root(), "buffer:" + std::string(edgeBuffers[0]));
    auto *destinationRow = areaWithKey(ui.root(), "buffer:" + std::string(edgeBuffers[3]));
    assert(originRow && destinationRow);
    assert(pointerPoint(*originRow)[1] < pointerPoint(*destinationRow)[1]);
    model.buffers.at(edgeBuffers[3]).lastChatTimestamp = 500;
    frame();
    assert(areaWithKey(ui.root(), "buffer:" + std::string(edgeBuffers[0])) == originRow);
    assert(areaWithKey(ui.root(), "buffer:" + std::string(edgeBuffers[3])) == destinationRow);
    assert(pointerPoint(*destinationRow)[1] < pointerPoint(*originRow)[1]);
    assert(dispatcher.focusedArea() == messageFocus && input->editingStateMatches(state));
    const auto resolved = transcript->resolveMessage(*selected);
    assert(resolved && resolved->buffer == edgeBuffers[0] && resolved->nick == "Peer0" &&
           resolved->identity == selected->identity);
    assert(transcript->reactToMessage(*selected, "👍"));
    assert(model.actions.back().type == "react" && model.actions.back().buffer == edgeBuffers[0] &&
           model.actions.back().extra == "same-raw-message-id");

    nativeKey(ui, dispatcher, 'g', KeyMod::Ctrl);
    frame();
    ui.execute("jump-channel");
    frame();
    auto *jump = areaWithKey(ui.root(), "channel-jump");
    auto *search = areaWithKey(ui.root(), "sidebar-search");
    Sidebar *sidebar = nullptr;
    for (Node *node = search; node; node = node->parent())
        if (auto *candidate = dynamic_cast<Sidebar *>(node))
            sidebar = candidate;
    assert(jump && visible(*jump) && sidebar);
    // Reorder while hints are live; choose the keys painted on the actual destination row.
    model.buffers.at(edgeBuffers[2]).lastChatTimestamp = 600;
    frame();
    std::string keys;
    for (const auto &hint : sidebar->hintRects())
        if (hint.buffer == edgeBuffers[3]) {
            auto *label = paintedHintAt(*jump, hint);
            assert(label && visible(*label));
            keys = label->text();
        }
    assert(!keys.empty());
    for (const auto key : keys)
        nativeKey(ui, dispatcher, key, 0, key);
    frame();
    assert(model.activeBuffer == edgeBuffers[3] && !visible(*jump));
    assert(input->editingStateMatches(state));
    auto *destinationInput = inputWithText(ui.root(), edgeDrafts[3]);
    assert(destinationInput && visible(*destinationInput) &&
           dispatcher.focusedArea() == destinationInput->inputArea());
    nativeKey(ui, dispatcher, 'p', KeyMod::Ctrl);
    frame();
    auto *destinationTranscript = visibleNode<TranscriptView>(*paneFor(*destinationInput));
    assert(destinationTranscript);
    const auto destination = destinationTranscript->selectedMessage();
    assert(destination && destination->buffer == edgeBuffers[3] && destination->nick == "Peer3");
    assert(destinationTranscript->reactToMessage(*destination, "👍"));
    assert(model.actions.back().type == "react" && model.actions.back().buffer == edgeBuffers[3] &&
           model.actions.back().extra == "same-raw-message-id");

    // Overflow actions retain the displayed conversation through a further activity move.
    nativeKey(ui, dispatcher, 'g', KeyMod::Ctrl);
    frame();
    click(dispatcher, *headerTool(*destinationInput, "More conversation actions"));
    assert(menu.open);
    model.buffers.at(edgeBuffers[1]).lastChatTimestamp = 700;
    frame();
    menu.select("Pin chat");
    frame();
    assert(model.actions.back().type == "pin_buffer" &&
           model.actions.back().buffer == edgeBuffers[3] && model.actions.back().text == "on");
    model.settings.pinnedBuffers = {edgeBuffers[3]};
    frame();
    assert(pointerPoint(*destinationRow)[1] < pointerPoint(*originRow)[1]);
    click(dispatcher, *headerTool(*destinationInput, "More conversation actions"));
    menu.select("Unpin chat");
    frame();
    assert(model.actions.back().type == "pin_buffer" &&
           model.actions.back().buffer == edgeBuffers[3] && model.actions.back().text == "off");
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void sidebarArchiveControlsCheck() {
    using namespace blast;
    Model model;
    for (const auto &[id, kind] :
         {std::pair{"one/#room", "channel"}, std::pair{"one/person", "query"},
          std::pair{"one/search", "search"}}) {
        auto &buffer = model.buffers[id];
        buffer.id = id;
        buffer.server = "one";
        buffer.target = std::string(id).substr(4);
        buffer.kind = kind;
        buffer.draft = "retained draft";
        buffer.unread = 7;
    }
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    MenuHost menu;
    std::vector<std::string> commands;
    Sidebar sidebar(model, [&](std::string command) { commands.push_back(std::move(command)); });
    sidebar.setSize(320, 900);
    sidebar.setPopupContext(&menu);
    dispatcher.setSceneRoot(&sidebar);
    std::string focused = "one/#room";
    SidebarMode mode = SidebarMode::Expanded;
    float height = 900, width = 320, scale = 1;
    const auto frame = [&] {
        sidebar.setSize(mode == SidebarMode::Compact ? 76 * scale : width, height);
        sidebar.sync("one", focused, sidebar.width(), height, scale, mode);
        sidebar.layout(renderer);
    };
    const auto row = [&](std::string_view id) {
        auto *area = areaWithKey(sidebar, "buffer:" + std::string(id));
        assert(area);
        return area;
    };
    const auto entry = [&](std::string_view label) -> const ContextMenuEntry * {
        const auto find = [&](auto &&self, const auto &entries) -> const ContextMenuEntry * {
            for (const auto &item : entries) {
                if (item.label == label)
                    return &item;
                if (const auto *found = self(self, item.children))
                    return found;
            }
            return nullptr;
        };
        return find(find, menu.request.menu);
    };
    frame();
    click(dispatcher, *row("one/#room"), BTN_RIGHT);
    assert(menu.open && entry("Mute") && !entry("Mute")->icon.empty());
    assert(entry("Archive") && !entry("Archive")->icon.empty());
    focused = "one/person"; // A later focus change cannot retarget the captured row.
    menu.select("Mute");
    assert(model.actions.size() == 1 && model.actions.back().type == "mute_buffer" &&
           model.actions.back().buffer == "one/#room" && model.actions.back().text == "on");
    model.buffers.at("one/#room").muted = true;
    frame();
    click(dispatcher, *row("one/#room"), BTN_RIGHT);
    assert(entry("Unmute") && !entry("Unmute")->icon.empty());
    menu.select("Archive");
    assert(model.actions.back().type == "archive_buffer" &&
           model.actions.back().buffer == "one/#room" && model.actions.back().text == "on");
    model.buffers.at("one/#room").archived = true;
    focused = "one/#room";
    frame();
    assert(!visible(*row("one/#room"))); // Active pin must not leak an archived chat.
    auto *archived = areaWithKey(sidebar, "sidebar-filter:archived");
    assert(archived && visible(*archived));
    click(dispatcher, *archived);
    frame();
    assert(visible(*row("one/#room")) && !visible(*row("one/person")));
    click(dispatcher, *row("one/#room"));
    assert(commands.back() == "buffer:one/#room" && model.buffers.at("one/#room").archived);
    click(dispatcher, *row("one/#room"), BTN_RIGHT);
    assert(entry("Unarchive") && !entry("Unarchive")->icon.empty());
    menu.select("Unarchive");
    assert(model.actions.back().type == "archive_buffer" &&
           model.actions.back().buffer == "one/#room" && model.actions.back().text == "off");
    assert(model.buffers.at("one/#room").unread == 7 &&
           model.buffers.at("one/#room").draft == "retained draft");
    // Natural filter captions retain the unread count through resize, zoom and count changes.
    for (const float nextScale : {.7f, 1.f, 2.f}) {
        scale = nextScale;
        height = 900 * scale;
        for (const int unread : {1, 99, 9999999}) {
            model.buffers.at("one/#room").unread = unread;
            for (const float logicalWidth : {250.f, 160.f, 320.f}) {
                width = logicalWidth * scale;
                frame();
                auto *archiveButton = dynamic_cast<Button *>(archived->parent());
                assert(archiveButton &&
                       archiveButton->label()->text() == "Archived · " + std::to_string(unread));
                auto *selector = areaWithKey(sidebar, "sidebar-filter-menu");
                assert(selector);
                if (unread == 1 && logicalWidth == 250)
                    assert(visible(*archived));
                if (visible(*archived)) {
                    assert(!visible(*selector));
                    float right = 0, top = -1;
                    for (const auto key : {"sidebar-filter:all", "sidebar-filter:unread",
                                           "sidebar-filter:dms", "sidebar-filter:archived"}) {
                        auto *area = areaWithKey(sidebar, key);
                        assert(area && visible(*area));
                        auto *button = dynamic_cast<Button *>(area->parent());
                        assert(button && button->x() >= right);
                        if (top < 0)
                            top = button->y();
                        assert(button->y() == top &&
                               button->x() + button->width() <= button->parent()->width() + .01f);
                        const auto *label = button->label();
                        const auto natural =
                            renderer
                                .measureText(label->text(), label->fontSize(), label->font(), 0.f,
                                             0, TextAlign::Start, TextEllipsize::End, false)
                                .width;
                        const bool wrapFits =
                            label->textWrapWidth() == 0 || label->textWrapWidth() + .01f >= natural;
                        assert(wrapFits && label->width() + .01f >= natural);
                        right = button->x() + button->width();
                    }
                } else {
                    assert(visible(*selector));
                    click(dispatcher, *selector);
                    menu.select("Archived · " + std::to_string(unread));
                    frame();
                    assert(visible(*row("one/#room")));
                }
            }
        }
    }
    scale = 1;
    width = 320;
    height = 900;
    model.buffers.at("one/#room").unread = 7;
    for (const auto nextMode : {SidebarMode::Expanded, SidebarMode::Compact}) {
        mode = nextMode;
        height = 300;
        frame();
        auto *selector = areaWithKey(sidebar, "sidebar-filter-menu");
        assert(selector && visible(*selector));
        click(dispatcher, *selector);
        assert(menu.open);
        menu.select("All");
        frame();
        assert(!visible(*row("one/#room")) && visible(*row("one/person")));
        click(dispatcher, *selector);
        menu.select("Archived · 7");
        frame();
        assert(visible(*row("one/#room")) && !visible(*row("one/person")));
    }
    // Stale callbacks reject removed/nonchat origins and rename invalidates the popup.
    mode = SidebarMode::Expanded;
    height = 900;
    frame();
    click(dispatcher, *row("one/#room"), BTN_RIGHT);
    auto stale = menu.callbacks.onSelect;
    const auto count = model.actions.size();
    model.buffers.at("one/#room").kind = "search";
    menu.closeSelectDropdown();
    stale(4);
    assert(model.actions.size() == count);
    model.buffers.at("one/#room").kind = "channel";
    click(dispatcher, *row("one/#room"), BTN_RIGHT);
    stale = menu.callbacks.onSelect;
    sidebar.remapBuffer("one/#room", "one/#renamed");
    stale(5);
    assert(!menu.open && model.actions.size() == count);
    frame();
    click(dispatcher, *row("one/#room"), BTN_RIGHT);
    stale = menu.callbacks.onSelect;
    model.buffers.erase("one/#room");
    stale(4);
    assert(model.actions.size() == count);
    menu.closeSelectDropdown();
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);

    auto paneModel = edgeModel();
    theme.update(paneModel);
    Clipboard clipboard;
    Ui ui(paneModel, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    ui.root().setPopupContext(&menu);
    const auto paneFrame = [&] {
        ui.update(1600, 1000, 1);
        ui.root().layout(renderer);
    };
    std::string origin = edgeBuffers[0];
    for (const auto &[label, command] :
         {std::pair{"Mute", "mute-buffer"}, std::pair{"Unmute", "unmute-buffer"},
          std::pair{"Archive", "archive-buffer"}, std::pair{"Unarchive", "unarchive-buffer"}}) {
        auto &buffer = paneModel.buffers.at(origin);
        buffer.muted = std::string_view(label) == "Unmute";
        buffer.archived = std::string_view(label) == "Unarchive";
        ui.execute("buffer:" + origin);
        paneFrame();
        auto *input = inputWithText(ui.root(), edgeDrafts[0]);
        assert(input && visible(*input));
        click(dispatcher, *areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[1]));
        click(dispatcher, *headerTool(*input, "More conversation actions"));
        paneFrame();
        assert(menu.open && paneModel.activeBuffer == edgeBuffers[1]);
        assert(entry(label) && !entry(label)->icon.empty());
        const auto before = paneModel.actions.size();
        menu.select(label);
        if (std::string_view(label) == "Mute") {
            const auto old = origin;
            origin = "test/renamed-controls";
            renameFixtureBuffer(paneModel, old, origin);
        }
        paneFrame();
        assert(paneModel.actions.size() == before + 1);
        const auto &action = paneModel.actions.back();
        const bool mute = std::string_view(command).find("mute") != std::string_view::npos;
        const bool enabled =
            std::string_view(label) == "Mute" || std::string_view(label) == "Archive";
        assert(action.type == (mute ? "mute_buffer" : "archive_buffer") &&
               action.buffer == origin && action.text == (enabled ? "on" : "off"));
        assert(paneModel.activeBuffer == edgeBuffers[1]);
    }
    for (const std::string kind : {"server", "search"}) {
        paneModel.buffers.at(origin).kind = kind;
        const auto before = paneModel.actions.size();
        for (const std::string command :
             {"mute-buffer", "unmute-buffer", "archive-buffer", "unarchive-buffer"})
            ui.execute("buffer-command:" + origin + "\n" + command);
        paneFrame();
        assert(paneModel.actions.size() == before);
    }
    const auto beforeMissing = paneModel.actions.size();
    ui.execute("buffer-command:missing/origin\narchive-buffer");
    paneFrame();
    assert(paneModel.actions.size() == beforeMissing);
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void queuedPaneCommandCheck(bool rename) {
    using namespace blast;
    enum class Intent { History, Read, Style, Presence, Close };
    const std::array cases{std::pair{"Load older messages", Intent::History},
                           std::pair{"Mark read", Intent::Read},
                           std::pair{"Traditional IRC", Intent::Style},
                           std::pair{"Traditional · two-line", Intent::Style},
                           std::pair{"Message bubbles", Intent::Style},
                           std::pair{"Hidden", Intent::Presence},
                           std::pair{"Close buffer", Intent::Close}};
    auto model = edgeModel();
    Clipboard clipboard;
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    MenuHost menu;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    ui.root().setPopupContext(&menu);
    const auto frame = [&] {
        ui.update(1600, 1000, 1);
        ui.root().layout(renderer);
    };
    frame();
    auto *originInput = inputWithText(ui.root(), edgeDrafts[0]);
    assert(originInput && originInput->insertText("!"));
    ui.execute("undo");
    const auto originState = originInput->editingState();
    assert(!originState.history->redo.empty());
    std::string origin = edgeBuffers[0];
    for (const auto &[label, intent] : cases) {
        ui.execute("buffer:" + origin);
        frame();
        auto *input = inputWithText(ui.root(), edgeDrafts[0]);
        assert(input && visible(*input));
        click(dispatcher, *areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[1]));
        click(dispatcher, *headerTool(*input, "More conversation actions"));
        assert(menu.open && model.activeBuffer == origin);
        frame();
        assert(menu.open && model.activeBuffer == edgeBuffers[1]);
        const auto display = std::find_if(
            menu.request.menu.begin(), menu.request.menu.end(),
            [](const ContextMenuEntry &entry) { return entry.label == "Message display"; });
        assert(display != menu.request.menu.end() && display->children.size() == 3);
        const auto selected = model.settings.messageLayouts.find(origin);
        const auto layout = selected == model.settings.messageLayouts.end() ? MessageLayout::Bubbles
                                                                            : selected->second;
        const std::array expected{MessageLayout::Bubbles, MessageLayout::Irc,
                                  MessageLayout::Stacked};
        for (std::size_t i = 0; i < expected.size(); ++i)
            assert(display->children[i].radio &&
                   display->children[i].toggleState == (layout == expected[i] ? 1 : 0));
        const auto firstAction = model.actions.size();
        menu.select(label);
        if (rename && intent == Intent::History) {
            const auto previous = origin;
            origin = "test/renamed";
            renameFixtureBuffer(model, previous, origin);
        }
        frame();
        const auto targeted = [&](std::string_view type) {
            const auto first = model.actions.begin() + firstAction;
            assert(std::count_if(first, model.actions.end(), [&](const Action &action) {
                       return action.type == type && action.buffer == origin;
                   }) == 1);
            assert(std::none_of(first, model.actions.end(), [&](const Action &action) {
                return action.type == type && action.buffer != origin;
            }));
        };
        switch (intent) {
        case Intent::History:
            targeted("history");
            break;
        case Intent::Read:
            targeted("read");
            break;
        case Intent::Style:
            if (std::string_view(label) == "Message bubbles")
                assert(!model.settings.messageLayouts.contains(origin));
            else
                assert(model.settings.messageLayouts.at(origin) ==
                       (std::string_view(label) == "Traditional IRC" ? MessageLayout::Irc
                                                                     : MessageLayout::Stacked));
            assert(!model.settings.messageLayouts.contains(edgeBuffers[1]));
            break;
        case Intent::Presence:
            assert(model.settings.channelPresence.at(origin) == PresenceMode::Hidden);
            assert(!model.settings.channelPresence.contains(edgeBuffers[1]));
            break;
        case Intent::Close:
            targeted("close_buffer");
            break;
        }
        assert(model.activeBuffer == origin);
        assert(model.buffers.at(origin).draft == edgeDrafts[0]);
        assert(model.buffers.at(edgeBuffers[1]).draft == edgeDrafts[1]);
        assert(model.settings.workspace.at("visible_buffers") == origin);
        assert(inputWithText(ui.root(), edgeDrafts[0]) == input && visible(*input));
        assert(input == originInput && input->editingStateMatches(originState));
    }
    hostKey(ui, dispatcher, XKB_KEY_x, KeyMod::Alt);
    frame();
    for (const char ch : std::string_view("message-style:stacked"))
        hostKey(ui, dispatcher, ch, 0, ch);
    frame();
    hostKey(ui, dispatcher, XKB_KEY_Return);
    frame();
    assert(model.settings.messageLayouts.at(origin) == MessageLayout::Stacked);
    assert(!model.settings.messageLayouts.contains(edgeBuffers[1]));
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void peopleFriendOrderingCheck() {
    using namespace blast;
    auto model = edgeModel();
    model.settings.peopleMode = SidebarMode::Expanded;
    auto &buffer = model.buffers.at(edgeBuffers[0]);
    buffer.kind = "channel";
    buffer.joined = true;
    buffer.nicks = {{.name = "Zoe[", .account = {}, .prefix = {}},
                    {.name = "Carl", .account = {}, .prefix = {}},
                    {.name = "Zed", .account = {}, .prefix = "@"},
                    {.name = "Bea", .account = {}, .prefix = "+"},
                    {.name = "Amy", .account = {}, .prefix = {}},
                    {.name = "Aaron", .account = {}, .prefix = "@"}};
    model.settings.friends = {"zOE{", "AMY", "zed"};
    model.networks["test"].caseMapping = "rfc1459";
    Clipboard clipboard;
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    const auto frame = [&] {
        ui.update(1600, 1000, 1);
        ui.root().layout(renderer);
    };
    const auto order = [&](std::array<std::string_view, 6> expected) {
        auto *people = areaWithKey(ui.root(), "people-members");
        assert(people && visible(*people));
        InputArea *previous = nullptr;
        for (const auto name : expected) {
            auto *row = memberRow(*people, name);
            assert(row && visible(*row));
            if (previous)
                assert(pointerPoint(*previous)[1] < pointerPoint(*row)[1]);
            previous = row;
        }
    };
    frame();
    order({"@Zed", "Amy", "Zoe[", "@Aaron", "+Bea", "Carl"});
    model.settings.friends.erase("AMY");
    frame();
    order({"@Zed", "Zoe[", "@Aaron", "+Bea", "Amy", "Carl"});
    model.networks["test"].caseMapping = "ascii";
    frame();
    order({"@Zed", "@Aaron", "+Bea", "Amy", "Carl", "Zoe["});
    model.networks["test"].caseMapping = "rfc1459";
    frame();
    order({"@Zed", "Zoe[", "@Aaron", "+Bea", "Amy", "Carl"});
    model.settings.friends.clear();
    frame();
    order({"@Aaron", "@Zed", "+Bea", "Amy", "Carl", "Zoe["});
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void queuedMemberInsertCheck(bool rename, bool keyboard) {
    using namespace blast;
    auto model = edgeModel();
    model.settings.peopleMode = SidebarMode::Expanded;
    for (const auto *id : {edgeBuffers[0], edgeBuffers[1]}) {
        auto &buffer = model.buffers.at(id);
        buffer.kind = "channel";
        buffer.joined = true;
        buffer.nicks.push_back({.name = "Bob", .account = {}, .prefix = {}});
    }
    Clipboard clipboard;
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    const auto frame = [&] {
        ui.update(1600, 1000, 1);
        ui.root().layout(renderer);
    };
    frame();
    auto *people = areaWithKey(ui.root(), "people-members");
    assert(people && visible(*people));
    auto *bob = memberRow(*people, "Bob");
    assert(bob && visible(*bob));
    if (keyboard) {
        // A key first resolves older pointer intents. Accept the A-roster key
        // before B's click instead of pretending Enter can bypass that refresh.
        dispatcher.setFocus(people);
        nativeKey(ui, dispatcher, XKB_KEY_Return);
        click(dispatcher, *areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[1]));
    } else {
        click(dispatcher, *areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[1]));
        click(dispatcher, *bob);
    }
    assert(model.activeBuffer == edgeBuffers[0]); // Both native intents still await a frame.
    std::string origin = edgeBuffers[0];
    if (rename) {
        origin = "test/renamed";
        renameFixtureBuffer(model, edgeBuffers[0], origin);
    }
    frame();
    assert(keyboard ? model.activeBuffer == edgeBuffers[1] : model.activeBuffer == origin);
    assert(model.buffers.at(origin).draft == std::string(edgeDrafts[0]) + "Bob: ");
    assert(model.buffers.at(edgeBuffers[1]).draft == edgeDrafts[1]);
    ui.execute("buffer:" + origin);
    frame();
    ui.execute("undo");
    assert(model.buffers.at(origin).draft == edgeDrafts[0]);
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void pendingShutdownFlushCheck() {
    using namespace blast;
    auto model = edgeModel();
    Clipboard clipboard;
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    ui.update(1600, 1000, 1);
    ui.root().layout(renderer);
    auto *input = inputWithText(ui.root(), edgeDrafts[0]);
    assert(input && input->insertText("!"));
    click(dispatcher, *areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[1]));
    assert(model.activeBuffer == edgeBuffers[0]);
    // Shutdown flush must persist accepted native intents without another UI update.
    ui.flush();
    assert(model.activeBuffer == edgeBuffers[1]);
    assert(model.settings.workspace.at("visible_buffers") == edgeBuffers[1]);
    Storage storage;
    storage.save(model);
    Model restored;
    storage.load(restored);
    assert(restored.activeBuffer == edgeBuffers[1]);
    assert(restored.buffers.at(edgeBuffers[0]).draft == std::string(edgeDrafts[0]) + "!");
    for (std::size_t i = 1; i < edgeBuffers.size(); ++i)
        assert(restored.buffers.at(edgeBuffers[i]).draft == edgeDrafts[i]);
    // A final core tick may publish a newly selected query after the first flush.
    auto &query = model.buffers["test/ShutdownPeer"];
    query.id = "test/ShutdownPeer";
    query.server = "test";
    query.target = "ShutdownPeer";
    query.kind = "query";
    query.draft = "external shutdown draft";
    model.activeBuffer = query.id;
    ui.flush();
    assert(model.settings.workspace.at("visible_buffers") == query.id);
    storage.save(model);
    storage.load(restored);
    assert(restored.activeBuffer == query.id);
    assert(restored.settings.workspace.at("visible_buffers") == query.id);
    assert(restored.buffers.at(edgeBuffers[0]).draft == std::string(edgeDrafts[0]) + "!");
    assert(restored.buffers.at(query.id).draft == "external shutdown draft");
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void queuedBufferMergeCheck(bool contextCommand) {
    using namespace blast;
    auto model = edgeModel();
    Clipboard clipboard;
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    MenuHost menu;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    ui.root().setPopupContext(&menu);
    const auto frame = [&] {
        ui.update(1600, 1000, 1);
        ui.root().layout(renderer);
    };
    frame();
    const auto firstAction = model.actions.size();
    if (contextCommand) {
        ui.execute("buffer:" + std::string(edgeBuffers[2]));
        frame();
        auto *source = inputWithText(ui.root(), edgeDrafts[2]);
        assert(source && visible(*source));
        click(dispatcher, *areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[0]));
        click(dispatcher, *headerTool(*source, "More conversation actions"));
        assert(menu.open && model.activeBuffer == edgeBuffers[2]);
        menu.select("Close buffer");
    } else {
        auto *source = areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[2]);
        assert(source && visible(*source));
        click(dispatcher, *source);
        assert(model.activeBuffer == edgeBuffers[0]);
    }
    assert(model.bufferRenamed);
    model.buffers.erase(edgeBuffers[2]);
    model.bufferRenamed(edgeBuffers[2], edgeBuffers[1]);
    frame();
    assert(model.activeBuffer == edgeBuffers[1]);
    assert(model.settings.workspace.at("visible_buffers") == edgeBuffers[1]);
    assert(model.buffers.at(edgeBuffers[1]).draft == edgeDrafts[1]);
    auto *destination = inputWithText(ui.root(), edgeDrafts[1]);
    assert(destination && visible(*destination));
    if (contextCommand)
        assert(std::count_if(model.actions.begin() + firstAction, model.actions.end(),
                             [&](const Action &action) {
                                 return action.type == "close_buffer" &&
                                        action.buffer == edgeBuffers[1];
                             }) == 1);
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void heldMemberIdentityCheck() {
    using namespace blast;
    auto model = edgeModel();
    model.settings.peopleMode = SidebarMode::Expanded;
    for (const auto *id : {edgeBuffers[0], edgeBuffers[1]}) {
        auto &buffer = model.buffers.at(id);
        buffer.kind = "channel";
        buffer.joined = true;
        buffer.nicks.push_back({.name = "Bob", .account = {}, .prefix = {}});
    }
    Clipboard clipboard;
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    const auto frame = [&] {
        ui.update(1600, 1000, 1);
        ui.root().layout(renderer);
    };
    frame();
    auto *people = areaWithKey(ui.root(), "people-members");
    assert(people && visible(*people));
    auto *bob = memberRow(*people, "Bob");
    assert(bob && visible(*bob));
    auto point = pointerPoint(*bob);
    pointerButton(dispatcher, point, BTN_LEFT, true);
    assert(bob->pressed() && dispatcher.pointerCaptured());
    model.buffers.at(edgeBuffers[0])
        .nicks.push_back({.name = "Aaron", .account = {}, .prefix = {}});
    frame();
    pointerButton(dispatcher, point, BTN_LEFT, false);
    frame();
    assert(model.buffers.at(edgeBuffers[0]).draft == edgeDrafts[0]);

    // A real subsequent click still inserts the intended row, proving cancellation
    // did not merely disable members or miss the native hit target.
    auto *aaron = memberRow(*people, "Aaron");
    assert(aaron && visible(*aaron));
    click(dispatcher, *aaron);
    frame();
    const auto &inserted = model.buffers.at(edgeBuffers[0]).draft;
    assert(inserted != edgeDrafts[0] && inserted.find("Aaron") != std::string::npos);
    ui.execute("undo");
    assert(model.buffers.at(edgeBuffers[0]).draft == edgeDrafts[0]);

    model.buffers.at(edgeBuffers[0])
        .nicks.erase(model.buffers.at(edgeBuffers[0]).nicks.begin() + 1);
    frame();
    bob = memberRow(*people, "Bob");
    assert(bob && visible(*bob));
    point = pointerPoint(*bob);
    pointerButton(dispatcher, point, BTN_LEFT, true);
    assert(bob->pressed());
    auto *next = areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[1]);
    assert(next && visible(*next));
    dispatcher.setFocus(next);
    nativeKey(ui, dispatcher, XKB_KEY_Return); // Same nick, different channel via sidebar.
    frame();
    pointerButton(dispatcher, point, BTN_LEFT, false);
    frame();
    assert(model.activeBuffer == edgeBuffers[1]);
    assert(model.buffers.at(edgeBuffers[0]).draft == edgeDrafts[0]);
    assert(model.buffers.at(edgeBuffers[1]).draft == edgeDrafts[1]);
    // Deferred touch admission requires actual overflow, unlike the immediate presses above.
    for (int index = 0; index < 40; ++index) {
        const auto suffix = std::to_string(index);
        model.buffers.at(edgeBuffers[1])
            .nicks.push_back({.name = "Overflow" + suffix, .account = {}, .prefix = {}});
        const auto id = "test/zz-overflow-" + suffix;
        auto &buffer = model.buffers[id];
        buffer.id = id;
        buffer.server = "test";
        buffer.target = "zz-overflow-" + suffix;
        buffer.kind = "query";
    }
    frame();
    // A deferred touch has no pressed row/capture; mouse Enter must not lose its panel origin.
    std::uint32_t serial = 30000;
    for (const bool navigation : {false, true}) {
        auto *channel = areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[0]);
        auto *member = memberRow(*people, "Bob");
        assert(channel && member && visible(*channel) && visible(*member));
        auto *origin = navigation ? channel : member;
        auto *otherPanel = navigation ? member : channel;
        ScrollView *viewport = nullptr;
        for (Node *node = origin->parent(); node; node = node->parent())
            if (auto *candidate = dynamic_cast<ScrollView *>(node))
                viewport = candidate;
        assert(viewport && viewport->maxScrollOffset() > 0);
        const auto downPoint = pointerPoint(*origin);
        const auto enterPoint = pointerPoint(*otherPanel);
        dispatcher.pointerEnter(downPoint[0], downPoint[1], ++serial);
        ui.beginPointerInteraction(downPoint[0], downPoint[1]);
        assert(!ui.handlePointerButton(BTN_LEFT, true, downPoint[0], downPoint[1], true));
        ++serial;
        assert(dispatcher.pointerButton(downPoint[0], downPoint[1], BTN_LEFT, true, serial,
                                        serial * 10, true));
        assert(!origin->pressed() && !dispatcher.pointerCaptured());
        dispatcher.pointerEnter(enterPoint[0], enterPoint[1], ++serial);
        assert(dispatcher.hoveredArea() == otherPanel);
        const auto command = navigation ? "toggle-navigation" : "toggle-people";
        ui.execute(command);
        ui.execute(command);
        frame();
        assert(navigation ? visibleCount<Sidebar>(ui.root()) == 0
                          : visibleCount<People>(ui.root()) == 0);
        // Restore a live action at the original coordinates before releasing the old touch.
        ui.execute(command);
        frame();
        assert(visible(*origin) && pointerPoint(*origin) == downPoint);
        auto *const releaseFocus = dispatcher.focusedArea();
        const bool retired = ui.retiredTouch();
        if (retired) {
            ui.discardPointerRelease(BTN_LEFT, true);
        } else if (!ui.handlePointerButton(BTN_LEFT, false, downPoint[0], downPoint[1], true)) {
            ++serial;
            dispatcher.pointerButton(downPoint[0], downPoint[1], BTN_LEFT, false, serial,
                                     serial * 10, true);
        }
        const bool leaveConsumed = ui.consumeTouchLeave();
        if (!leaveConsumed)
            dispatcher.pointerLeave();
        assert(leaveConsumed);
        frame();
        assert(retired && !dispatcher.pointerCaptured() && !origin->pressed());
        assert(dispatcher.focusedArea() == releaseFocus);
        assert(model.activeBuffer == edgeBuffers[1]);
        assert(model.buffers.at(edgeBuffers[0]).draft == edgeDrafts[0]);
        assert(model.buffers.at(edgeBuffers[1]).draft == edgeDrafts[1]);
    }
    // Retiring an old touch must preserve a newer mouse capture in either release order.
    for (const bool touchReleaseFirst : {false, true}) {
        auto *touchChannel = areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[0]);
        auto *mouseMember = memberRow(*people, "Bob");
        assert(touchChannel && mouseMember && visible(*touchChannel) && visible(*mouseMember));
        const auto oldTouchPoint = pointerPoint(*touchChannel);
        const auto mousePoint = pointerPoint(*mouseMember);
        ScrollView *touchScroll = nullptr;
        for (auto *node = touchChannel->parent(); node; node = node->parent())
            if (auto *candidate = dynamic_cast<ScrollView *>(node))
                touchScroll = candidate;
        assert(touchScroll && touchScroll->maxScrollOffset() > 0);
        assert(!ui.prepareTouchPress());
        dispatcher.pointerEnter(oldTouchPoint[0], oldTouchPoint[1], ++serial);
        assert(!ui.prepareTouchPress());
        ui.beginPointerInteraction(oldTouchPoint[0], oldTouchPoint[1]);
        assert(!ui.handlePointerButton(BTN_LEFT, true, oldTouchPoint[0], oldTouchPoint[1], true));
        ++serial;
        assert(dispatcher.pointerButton(oldTouchPoint[0], oldTouchPoint[1], BTN_LEFT, true, serial,
                                        serial * 10, true));
        assert(!touchChannel->pressed() && !dispatcher.pointerCaptured());
        dispatcher.pointerEnter(mousePoint[0], mousePoint[1], ++serial);
        ui.prepareMousePress();
        ui.beginPointerInteraction(mousePoint[0], mousePoint[1]);
        assert(!ui.handlePointerButton(BTN_LEFT, true, mousePoint[0], mousePoint[1], false));
        ++serial;
        assert(dispatcher.pointerButton(mousePoint[0], mousePoint[1], BTN_LEFT, true, serial,
                                        serial * 10, false));
        assert(mouseMember->pressed() && dispatcher.pointerCaptured());
        ui.execute("toggle-navigation");
        ui.execute("toggle-navigation");
        frame();
        assert(visibleCount<Sidebar>(ui.root()) == 0);
        assert(mouseMember->pressed() && dispatcher.pointerCaptured());
        const auto retireOldTouch = [&] {
            assert(ui.retiredTouch());
            ui.discardPointerRelease(BTN_LEFT, true);
            const bool consumed = ui.consumeTouchLeave();
            if (!consumed)
                dispatcher.pointerLeave();
            assert(consumed);
        };
        if (touchReleaseFirst) {
            auto *const heldFocus = dispatcher.focusedArea();
            auto *composer = inputWithText(ui.root(), edgeDrafts[1]);
            assert(composer && visible(*composer));
            const auto heldEditing = composer->editingState();
            const auto heldScroll = touchScroll->scrollOffset();
            const bool motionRetired = ui.retiredTouch();
            if (!motionRetired) {
                ui.handlePointerMotion(oldTouchPoint[0], oldTouchPoint[1] - 48, true);
                dispatcher.pointerMotion(oldTouchPoint[0], oldTouchPoint[1] - 48, ++serial);
            }
            assert(motionRetired);
            retireOldTouch();
            frame();
            assert(mouseMember->pressed() && dispatcher.pointerCaptured());
            assert(dispatcher.focusedArea() == heldFocus);
            assert(model.buffers.at(edgeBuffers[1]).draft == edgeDrafts[1]);
            assert(composer->editingStateMatches(heldEditing));
            assert(touchScroll->scrollOffset() == heldScroll);
        }
        assert(!ui.handlePointerButton(BTN_LEFT, false, mousePoint[0], mousePoint[1], false));
        ++serial;
        assert(dispatcher.pointerButton(mousePoint[0], mousePoint[1], BTN_LEFT, false, serial,
                                        serial * 10, false));
        frame();
        const auto mouseDraft = model.buffers.at(edgeBuffers[1]).draft;
        const auto insertedBob = mouseDraft.find("Bob");
        assert(insertedBob != std::string::npos &&
               mouseDraft.find("Bob", insertedBob + 3) == std::string::npos);
        assert(model.activeBuffer == edgeBuffers[1] && !dispatcher.pointerCaptured());
        if (!touchReleaseFirst) {
            auto *const mouseFocus = dispatcher.focusedArea();
            retireOldTouch();
            frame();
            assert(model.activeBuffer == edgeBuffers[1]);
            assert(model.buffers.at(edgeBuffers[1]).draft == mouseDraft);
            assert(dispatcher.focusedArea() == mouseFocus);
        }
        ui.execute("undo");
        assert(model.buffers.at(edgeBuffers[1]).draft == edgeDrafts[1]);
        ui.execute("toggle-navigation");
        frame();
    }
    // A new touch stream is consumed before it can disturb an existing mouse capture.
    auto *capturedMember = memberRow(*people, "Bob");
    auto *uncapturedChannel = areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[0]);
    assert(capturedMember && uncapturedChannel && visible(*capturedMember) &&
           visible(*uncapturedChannel));
    const auto capturedPoint = pointerPoint(*capturedMember);
    const auto uncapturedPoint = pointerPoint(*uncapturedChannel);
    dispatcher.pointerEnter(capturedPoint[0], capturedPoint[1], ++serial);
    ui.prepareMousePress();
    ui.beginPointerInteraction(capturedPoint[0], capturedPoint[1]);
    assert(!ui.handlePointerButton(BTN_LEFT, true, capturedPoint[0], capturedPoint[1], false));
    ++serial;
    assert(dispatcher.pointerButton(capturedPoint[0], capturedPoint[1], BTN_LEFT, true, serial,
                                    serial * 10, false));
    assert(capturedMember->pressed() && dispatcher.pointerCaptured());
    auto *capturedComposer = inputWithText(ui.root(), edgeDrafts[1]);
    assert(capturedComposer && visible(*capturedComposer));
    const auto capturedEditing = capturedComposer->editingState();
    auto *const capturedFocus = dispatcher.focusedArea();
    const bool enterConsumed = ui.prepareTouchPress();
    if (!enterConsumed)
        dispatcher.pointerEnter(uncapturedPoint[0], uncapturedPoint[1], ++serial);
    assert(enterConsumed);
    const bool downConsumed = ui.prepareTouchPress();
    if (!downConsumed) {
        ui.beginPointerInteraction(uncapturedPoint[0], uncapturedPoint[1]);
        if (!ui.handlePointerButton(BTN_LEFT, true, uncapturedPoint[0], uncapturedPoint[1], true)) {
            ++serial;
            dispatcher.pointerButton(uncapturedPoint[0], uncapturedPoint[1], BTN_LEFT, true, serial,
                                     serial * 10, true);
        }
    }
    assert(downConsumed);
    ui.execute("toggle-navigation");
    ui.execute("toggle-navigation");
    frame();
    assert(visibleCount<Sidebar>(ui.root()) == 0 && ui.retiredTouch());
    assert(capturedMember->pressed() && dispatcher.pointerCaptured());
    const bool motionRetired = ui.retiredTouch();
    if (!motionRetired) {
        ui.handlePointerMotion(uncapturedPoint[0], uncapturedPoint[1] - 48, true);
        dispatcher.pointerMotion(uncapturedPoint[0], uncapturedPoint[1] - 48, ++serial);
    }
    assert(motionRetired);
    ui.discardPointerRelease(BTN_LEFT, true);
    const bool capturedLeaveConsumed = ui.consumeTouchLeave();
    if (!capturedLeaveConsumed)
        dispatcher.pointerLeave();
    assert(capturedLeaveConsumed);
    frame();
    assert(capturedMember->pressed() && dispatcher.pointerCaptured());
    assert(dispatcher.focusedArea() == capturedFocus);
    assert(capturedComposer->editingStateMatches(capturedEditing));
    assert(model.buffers.at(edgeBuffers[1]).draft == edgeDrafts[1]);
    assert(!ui.handlePointerButton(BTN_LEFT, false, capturedPoint[0], capturedPoint[1], false));
    ++serial;
    assert(dispatcher.pointerButton(capturedPoint[0], capturedPoint[1], BTN_LEFT, false, serial,
                                    serial * 10, false));
    frame();
    const auto capturedDraft = model.buffers.at(edgeBuffers[1]).draft;
    const auto capturedBob = capturedDraft.find("Bob");
    assert(capturedBob != std::string::npos &&
           capturedDraft.find("Bob", capturedBob + 3) == std::string::npos);
    assert(model.activeBuffer == edgeBuffers[1] && !dispatcher.pointerCaptured());
    ui.execute("undo");
    assert(model.buffers.at(edgeBuffers[1]).draft == edgeDrafts[1]);
    ui.execute("toggle-navigation");
    frame();
    // A new legitimate touch works after the consumed stream's paired Leave.
    auto *freshMember = memberRow(*people, "Bob");
    assert(freshMember && visible(*freshMember));
    const auto freshPoint = pointerPoint(*freshMember);
    assert(!ui.prepareTouchPress());
    dispatcher.pointerEnter(freshPoint[0], freshPoint[1], ++serial);
    assert(!ui.prepareTouchPress());
    assert(!ui.retiredTouch());
    ui.beginPointerInteraction(freshPoint[0], freshPoint[1]);
    assert(!ui.handlePointerButton(BTN_LEFT, true, freshPoint[0], freshPoint[1], true));
    ++serial;
    assert(dispatcher.pointerButton(freshPoint[0], freshPoint[1], BTN_LEFT, true, serial,
                                    serial * 10, true));
    assert(!ui.handlePointerButton(BTN_LEFT, false, freshPoint[0], freshPoint[1], true));
    ++serial;
    assert(dispatcher.pointerButton(freshPoint[0], freshPoint[1], BTN_LEFT, false, serial,
                                    serial * 10, true));
    const bool freshLeaveConsumed = ui.consumeTouchLeave();
    if (!freshLeaveConsumed)
        dispatcher.pointerLeave();
    assert(!freshLeaveConsumed);
    frame();
    const auto &freshDraft = model.buffers.at(edgeBuffers[1]).draft;
    const auto freshBob = freshDraft.find("Bob");
    assert(freshBob != std::string::npos &&
           freshDraft.find("Bob", freshBob + 3) == std::string::npos);
    assert(model.activeBuffer == edgeBuffers[1] && !dispatcher.pointerCaptured());
    ui.execute("undo");
    assert(model.buffers.at(edgeBuffers[1]).draft == edgeDrafts[1]);
    // Cancelling a touch before its first frame delivers only Leave, not Enter or Down.
    auto *cancelMember = memberRow(*people, "Bob");
    assert(cancelMember && visible(*cancelMember));
    const auto cancelPoint = pointerPoint(*cancelMember);
    dispatcher.pointerEnter(cancelPoint[0], cancelPoint[1], ++serial);
    ui.prepareMousePress();
    ui.beginPointerInteraction(cancelPoint[0], cancelPoint[1]);
    assert(!ui.handlePointerButton(BTN_LEFT, true, cancelPoint[0], cancelPoint[1], false));
    ++serial;
    assert(dispatcher.pointerButton(cancelPoint[0], cancelPoint[1], BTN_LEFT, true, serial,
                                    serial * 10, false));
    assert(cancelMember->pressed() && dispatcher.pointerCaptured() && !ui.retiredTouch());
    auto *const cancelFocus = dispatcher.focusedArea();
    auto *cancelComposer = inputWithText(ui.root(), edgeDrafts[1]);
    assert(cancelComposer && visible(*cancelComposer));
    const auto cancelEditing = cancelComposer->editingState();
    const bool cancelConsumed = ui.consumeTouchLeave();
    if (!cancelConsumed)
        dispatcher.pointerLeave();
    assert(cancelConsumed);
    frame();
    assert(cancelMember->pressed() && dispatcher.pointerCaptured());
    assert(dispatcher.focusedArea() == cancelFocus);
    assert(cancelComposer->editingStateMatches(cancelEditing));
    assert(model.buffers.at(edgeBuffers[1]).draft == edgeDrafts[1]);
    assert(!ui.handlePointerButton(BTN_LEFT, false, cancelPoint[0], cancelPoint[1], false));
    ++serial;
    assert(dispatcher.pointerButton(cancelPoint[0], cancelPoint[1], BTN_LEFT, false, serial,
                                    serial * 10, false));
    frame();
    const auto &cancelDraft = model.buffers.at(edgeBuffers[1]).draft;
    const auto cancelBob = cancelDraft.find("Bob");
    assert(cancelBob != std::string::npos &&
           cancelDraft.find("Bob", cancelBob + 3) == std::string::npos);
    assert(model.activeBuffer == edgeBuffers[1] && !dispatcher.pointerCaptured());
    ui.execute("undo");
    assert(model.buffers.at(edgeBuffers[1]).draft == edgeDrafts[1]);
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void pendingComposerWidthGeometryCheck() {
    using namespace blast;
    std::string draft;
    for (int segment = 0; segment < 32; ++segment)
        draft += "alpha beta gamma delta ";
    for (const auto key : {XKB_KEY_Home, XKB_KEY_End}) {
        auto model = edgeModel();
        model.buffers.at(edgeBuffers[0]).draft = draft.substr(0, draft.size() - 1);
        Clipboard clipboard;
        Theme theme;
        theme.update(model);
        InputDispatcher dispatcher;
        PangoRenderer renderer;
        Ui ui(model, clipboard);
        dispatcher.setSceneRoot(&ui.root());
        ui.setInputDispatcher(dispatcher);
        ui.setWindowActive(true);
        const auto frame = [&] {
            ui.update(1600, 1000, 1);
            ui.root().layout(renderer);
        };
        WindowActions actions{};
        actions.synchronizeLayout = [&] {
            frame();
            return true;
        };
        ui.setWindowActions(std::move(actions));
        frame();
        auto *input = inputWithText(ui.root(), model.buffers.at(edgeBuffers[0]).draft);
        assert(input && visible(*input));
        dispatcher.setFocus(input->inputArea());
        assert(input->insertText(std::string_view(draft).substr(draft.size() - 1)));
        assert(input->insertText("!"));
        ui.execute("undo");
        frame();
        assert(input->value() == draft);
        Label *main = nullptr;
        for (const auto &child : input->children())
            if (child->clipChildren())
                for (const auto &layer : child->children())
                    if (auto *label = dynamic_cast<Label *>(layer.get()))
                        main = label;
        assert(main && main->textWrapWidth() > 0);
        const auto rows = [&] {
            const auto *projection = input->displayProjection();
            assert(projection && projection->text == draft && main->text() == projection->markup);
            std::vector<std::size_t> displayed;
            displayed.reserve(draft.size() + 1);
            for (std::size_t raw = 0; raw <= draft.size(); ++raw)
                displayed.push_back(projection->rawToDisplay[raw]);
            std::vector<TextCursorStop> result;
            renderer.measureTextCaretStops(main->text(), main->fontSize(), displayed,
                                           main->textWrapWidth(), result, main->font(), true);
            assert(result.size() == draft.size() + 1 && result.front().lineIndex == 0);
            return result;
        };
        const auto lineStart = [](const auto &measured, auto line) {
            const auto found =
                std::find_if(measured.begin(), measured.end(),
                             [line](const auto &stop) { return stop.lineIndex == line; });
            assert(found != measured.end());
            return std::size_t(found - measured.begin());
        };
        const auto oldRows = rows();
        const auto secondLine = oldRows.front().lineIndex + 1;
        const auto thirdLine = secondLine + 1;
        const auto oldHome = lineStart(oldRows, secondLine);
        const auto oldEnd = lineStart(oldRows, thirdLine);
        const auto oldWidth = input->width();
        const auto oldWrapWidth = main->textWrapWidth();
        ui.execute("toggle-navigation");
        frame();
        assert(input->width() > oldWidth && main->textWrapWidth() > oldWrapWidth);
        const auto compactRows = rows();
        const auto compactHome = lineStart(compactRows, secondLine);
        const auto compactEnd = lineStart(compactRows, thirdLine);
        assert(compactHome != oldHome && compactEnd != oldEnd);
        const auto cursor = std::max(oldHome, compactHome) + 1;
        assert(cursor < std::min(oldEnd, compactEnd));
        ui.execute("toggle-navigation");
        ui.execute("toggle-navigation");
        frame();
        auto base = input->editingState();
        base.current.cursorPos = base.current.selectionAnchor = cursor;
        input->restoreEditingState(base);
        frame();
        base = input->editingState();
        assert(!base.history->undo.empty() && !base.history->redo.empty());
        const auto undo = base.history->undo, redo = base.history->redo;
        assert(input->width() == oldWidth && main->textWrapWidth() == oldWrapWidth &&
               !input->textGeometryDirty());
        auto *toggle = areaWithKey(ui.root(), "topbar-navigation");
        assert(toggle && visible(*toggle));
        click(dispatcher, *toggle);
        assert(input->width() == oldWidth && !input->textGeometryDirty());
        // The queued sidebar click changes allocation inside key admission, without a fixture
        // frame.
        hostKey(ui, dispatcher, key);
        assert(input->width() > oldWidth && main->textWrapWidth() > oldWrapWidth);
        const auto currentRows = rows();
        const auto expected = key == XKB_KEY_Home ? lineStart(currentRows, secondLine)
                                                  : lineStart(currentRows, thirdLine);
        assert(expected == (key == XKB_KEY_Home ? compactHome : compactEnd));
        const auto current = input->editingState();
        assert(current.current.value == draft && current.current.cursorPos == expected &&
               current.current.selectionAnchor == expected);
        assert(current.history == base.history && current.history->undo == undo &&
               current.history->redo == redo);
        const auto caret = input->textInputState();
        assert(caret.surroundingText == draft && caret.cursor == int(expected) &&
               caret.anchor == int(expected));
        assert(dispatcher.focusedArea() == input->inputArea());
        presentation(ui, model, edgeBuffers[0], SidebarMode::Hidden, SidebarMode::Compact);
        assert(model.buffers.at(edgeBuffers[0]).draft == draft);
        if (key == XKB_KEY_End) {
            // Measure a distinct next click before opening the dialog or holding any pointer.
            auto nextGesture = current;
            nextGesture.current.cursorPos = nextGesture.current.selectionAnchor = cursor;
            input->restoreEditingState(nextGesture);
            frame();
            const auto nextCaret = input->textInputState();
            assert(input->cursorPosition() == cursor && expected != cursor);
            assert(input->editingState().scrollY == current.scrollY &&
                   input->editingState().scrollX == current.scrollX);
            input->restoreEditingState(current);
            frame();
            assert(input->editingStateMatches(current));
            auto *const originalFocus = dispatcher.focusedArea();
            const std::array<float, 2> nextPoint{float(nextCaret.cursorRectX),
                                                 nextCaret.cursorRectY +
                                                     nextCaret.cursorRectHeight / 2.f};
            const auto first = model.actions.size();
            ui.execute("search");
            frame();
            auto *searchArea = dispatcher.focusedArea();
            auto *search =
                searchArea ? dynamic_cast<Input *>(searchArea->textInputClient()) : nullptr;
            assert(search && search != input && visible(*search));
            Dialogs *searchOwner = nullptr;
            for (Node *node = search; node; node = node->parent())
                if (auto *dialogs = dynamic_cast<Dialogs *>(node))
                    searchOwner = dialogs;
            assert(searchOwner && searchOwner->active());
            const auto searchPoint = pointerPoint(*searchArea);
            std::uint32_t serial = 60000;
            dispatcher.pointerEnter(searchPoint[0], searchPoint[1], ++serial);
            ui.prepareMousePress();
            ui.beginPointerInteraction(searchPoint[0], searchPoint[1]);
            assert(!ui.handlePointerButton(BTN_LEFT, true, searchPoint[0], searchPoint[1], false));
            ++serial;
            assert(dispatcher.pointerButton(searchPoint[0], searchPoint[1], BTN_LEFT, true, serial,
                                            serial * 10, false));
            assert(searchArea->pressed() && dispatcher.pointerCaptured());
            TextInputEdit edit{};
            edit.commitText = "dismissed held query";
            edit.hasCommitText = true;
            search->textInputApplyEdit(edit);
            assert(search->textGeometryDirty());
            hostKey(ui, dispatcher, XKB_KEY_Escape);
            assert(!visible(*search));
            // No fixture frame after the edit/dismissal: hidden capture must retire before motion.
            ui.handlePointerMotion(nextPoint[0], nextPoint[1], false);
            assert(!dispatcher.pointerCaptured() && !searchArea->pressed());
            dispatcher.pointerMotion(nextPoint[0], nextPoint[1], ++serial);
            const bool consumed =
                ui.handlePointerButton(BTN_LEFT, false, nextPoint[0], nextPoint[1], false);
            if (!consumed) {
                ++serial;
                dispatcher.pointerButton(nextPoint[0], nextPoint[1], BTN_LEFT, false, serial,
                                         serial * 10, false);
            }
            assert(consumed && !dispatcher.pointerCaptured());
            assert(dispatcher.focusedArea() == originalFocus &&
                   input->editingStateMatches(current));
            assert(current.history->undo == undo && current.history->redo == redo);
            assert(model.buffers.at(edgeBuffers[0]).draft == draft);
            assert(std::none_of(
                model.actions.begin() + first, model.actions.end(), [](const Action &a) {
                    return a.type == "search" || a.type == "send" || a.type == "command";
                }));
            assert(dispatcher.inputAreaAt(nextPoint[0], nextPoint[1]) == input->inputArea());
            ui.prepareMousePress();
            ui.beginPointerInteraction(nextPoint[0], nextPoint[1]);
            assert(!ui.handlePointerButton(BTN_LEFT, true, nextPoint[0], nextPoint[1], false));
            ++serial;
            assert(dispatcher.pointerButton(nextPoint[0], nextPoint[1], BTN_LEFT, true, serial,
                                            serial * 10, false));
            assert(input->inputArea()->pressed() && dispatcher.pointerCaptured());
            assert(input->cursorPosition() == cursor && input->selectionAnchor() == cursor);
            assert(!ui.handlePointerButton(BTN_LEFT, false, nextPoint[0], nextPoint[1], false));
            ++serial;
            assert(dispatcher.pointerButton(nextPoint[0], nextPoint[1], BTN_LEFT, false, serial,
                                            serial * 10, false));
            assert(!input->inputArea()->pressed() && !dispatcher.pointerCaptured());
            assert(dispatcher.focusedArea() == originalFocus && input->value() == draft);
            assert(current.history->undo == undo && current.history->redo == redo);
            // A touch must be admitted even when the previous mouse Up is still outstanding.
            assert(input->editingState().scrollY == current.scrollY &&
                   input->editingState().scrollX == current.scrollX);
            ui.execute("search");
            frame();
            searchArea = dispatcher.focusedArea();
            search = searchArea ? dynamic_cast<Input *>(searchArea->textInputClient()) : nullptr;
            assert(search && search != input && visible(*search));
            const auto heldSearchPoint = pointerPoint(*searchArea);
            dispatcher.pointerEnter(heldSearchPoint[0], heldSearchPoint[1], ++serial);
            ui.prepareMousePress();
            ui.beginPointerInteraction(heldSearchPoint[0], heldSearchPoint[1]);
            assert(!ui.handlePointerButton(BTN_LEFT, true, heldSearchPoint[0], heldSearchPoint[1],
                                           false));
            ++serial;
            assert(dispatcher.pointerButton(heldSearchPoint[0], heldSearchPoint[1], BTN_LEFT, true,
                                            serial, serial * 10, false));
            assert(searchArea->pressed() && dispatcher.pointerCaptured());
            search->textInputApplyEdit(edit);
            assert(search->textGeometryDirty());
            hostKey(ui, dispatcher, XKB_KEY_Escape);
            assert(!visible(*search));
            const std::array<float, 2> touchPoint{float(caret.cursorRectX),
                                                  caret.cursorRectY + caret.cursorRectHeight / 2.f};
            assert(!ui.prepareTouchPress());
            ui.beginPointerInteraction(touchPoint[0], touchPoint[1]);
            dispatcher.pointerEnter(touchPoint[0], touchPoint[1], ++serial);
            assert(!ui.prepareTouchPress());
            ui.beginPointerInteraction(touchPoint[0], touchPoint[1]);
            assert(!ui.handlePointerButton(BTN_LEFT, true, touchPoint[0], touchPoint[1], true));
            ++serial;
            assert(dispatcher.pointerButton(touchPoint[0], touchPoint[1], BTN_LEFT, true, serial,
                                            serial * 10, true));
            assert(input->inputArea()->pressed() && dispatcher.pointerCaptured());
            assert(input->cursorPosition() == expected && input->selectionAnchor() == expected);
            assert(ui.handlePointerButton(BTN_LEFT, false, heldSearchPoint[0], heldSearchPoint[1],
                                          false));
            assert(input->inputArea()->pressed() && dispatcher.pointerCaptured());
            assert(!ui.handlePointerButton(BTN_LEFT, false, touchPoint[0], touchPoint[1], true));
            ++serial;
            assert(dispatcher.pointerButton(touchPoint[0], touchPoint[1], BTN_LEFT, false, serial,
                                            serial * 10, true));
            assert(!ui.consumeTouchLeave());
            dispatcher.pointerLeave();
            assert(!input->inputArea()->pressed() && !dispatcher.pointerCaptured());
            assert(dispatcher.focusedArea() == originalFocus && input->value() == draft);
            assert(current.history->undo == undo && current.history->redo == redo);
            assert(std::none_of(
                model.actions.begin() + first, model.actions.end(), [](const Action &a) {
                    return a.type == "search" || a.type == "send" || a.type == "command";
                }));

            // Core may publish a search-result pane while the native window is inactive.
            TextInputEdit committedEdit{};
            committedEdit.commitText = "!";
            committedEdit.hasCommitText = true;
            input->textInputApplyEdit(committedEdit);
            assert(input->textGeometryDirty());
            const auto inactiveState = input->editingState();
            const auto inactiveUndo = inactiveState.history->undo;
            const auto inactiveRedo = inactiveState.history->redo;
            const std::string resultId = "test/*search*";
            auto &results = model.buffers[resultId];
            results.id = resultId;
            results.server = "test";
            results.target = "*search*";
            results.kind = "search";
            ui.setWindowActive(false);
            model.activeBuffer = resultId;
            ui.update(1600, 1000, 1);
            if (ui.root().layoutDirty())
                ui.root().layout(renderer);
            auto *inactiveTranscript = visibleNode<TranscriptView>(ui.root());
            assert(inactiveTranscript && inactiveTranscript->width() > 0 &&
                   inactiveTranscript->height() > 0);
            assert(dispatcher.focusedArea() == input->inputArea() && !visible(*input) &&
                   input->textGeometryDirty());
            // A re-entry key precedes the next active tick; private handling repairs and consumes
            // it.
            assert(ui.synchronizeInput(XKB_KEY_End));
            InputArea::KeyData repairKey{};
            repairKey.sym = XKB_KEY_End;
            repairKey.pressed = true;
            const bool repairConsumed = ui.handleKey(repairKey);
            if (!repairConsumed)
                dispatcher.keyEvent(XKB_KEY_End, 0, 0, true);
            assert(repairConsumed);
            repairKey.pressed = false;
            if (!ui.handleKey(repairKey))
                dispatcher.keyEvent(XKB_KEY_End, 0, 0, false);
            auto *repairedFocus = dispatcher.focusedArea();
            auto *resultTranscript = visibleNode<TranscriptView>(ui.root());
            assert(repairedFocus && visible(*repairedFocus) && repairedFocus != input->inputArea());
            assert(resultTranscript && repairedFocus->tabFocusKey().starts_with("buffer-tool:"));
            BufferTitleLine *focusedHeader = nullptr;
            bool inResultPane = false;
            for (Node *node = repairedFocus; node; node = node->parent()) {
                if (auto *header = dynamic_cast<BufferTitleLine *>(node))
                    focusedHeader = header;
                if (node == resultTranscript->parent())
                    inResultPane = true;
            }
            assert(focusedHeader && visible(*focusedHeader) && inResultPane);
            presentation(ui, model, resultId, SidebarMode::Hidden, SidebarMode::Compact);
            assert(input->editingStateMatches(inactiveState));
            assert(inactiveState.history->undo == inactiveUndo &&
                   inactiveState.history->redo == inactiveRedo);
            assert(model.buffers.at(edgeBuffers[0]).draft == inactiveState.current.value);
            assert(results.draft.empty() && results.messages.empty());
        }
        dispatcher.setFocus(nullptr);
        dispatcher.setSceneRoot(nullptr);
    }
}

void deferredSettingsTouchGeometryCheck() {
    using namespace blast;
    for (const bool motion : {false, true}) {
        auto model = edgeModel();
        const std::string committed(20, 'i');
        model.settings.bindings["complete"] = committed;
        Clipboard clipboard;
        Theme theme;
        theme.update(model);
        InputDispatcher dispatcher;
        PangoRenderer renderer;
        Ui ui(model, clipboard);
        dispatcher.setSceneRoot(&ui.root());
        ui.setInputDispatcher(dispatcher);
        ui.setWindowActive(true);
        const auto frame = [&] {
            ui.update(1600, 1000, 1);
            ui.root().layout(renderer);
        };
        int synchronizations = 0;
        WindowActions actions{};
        actions.synchronizeLayout = [&] {
            ++synchronizations;
            frame();
            return true;
        };
        ui.setWindowActions(std::move(actions));
        frame();
        auto *composer = inputWithText(ui.root(), edgeDrafts[0]);
        assert(composer);
        const auto composerState = composer->editingState();
        const auto composerUndo = composerState.history->undo;
        const auto composerRedo = composerState.history->redo;
        ui.execute("settings");
        frame();
        auto *dialogs = visibleNode<Dialogs>(ui.root());
        assert(dialogs && dialogs->settingsOpen());
        auto *keyboard = areaWithKey(*dialogs, "blastirc.settings.category.1");
        assert(keyboard && visible(*keyboard) && dynamic_cast<Button *>(keyboard->parent()));
        click(dispatcher, *keyboard);
        frame();
        auto *area = areaWithKey(ui.root(), "blastirc.settings.binding.complete");
        assert(area && visible(*area) && !area->passiveTouchPress());
        auto *field = dynamic_cast<Input *>(area->textInputClient());
        assert(field && field->value() == committed);
        ScrollView *viewport = nullptr;
        for (Node *node = area->parent(); node; node = node->parent()) {
            if (auto *ancestor = dynamic_cast<InputArea *>(node))
                assert(ancestor->touchScrollAxis() != InputArea::TouchScrollAxis::Horizontal);
            if (auto *scroll = dynamic_cast<ScrollView *>(node))
                viewport = scroll;
        }
        assert(viewport && viewport->orientation() == ScrollOrientation::Vertical);
        dispatcher.setFocus(area);
        frame();
        assert(viewport->maxScrollOffset() > 0);
        auto base = field->editingState();
        base.current.cursorPos = base.current.selectionAnchor = 3;
        field->restoreEditingState(base);
        frame();
        base = field->editingState();
        const auto undo = base.history->undo, redo = base.history->redo;
        const auto caretAt = [&](std::size_t raw) {
            auto state = base;
            state.current.cursorPos = state.current.selectionAnchor = raw;
            field->restoreEditingState(state);
            frame();
            return field->textInputState();
        };
        const auto baseCaret = caretAt(3);
        const auto downCaret = caretAt(8);
        const auto motionCaret = caretAt(16);
        float left = 0, top = 0, right = 0, bottom = 0;
        Node::transformedBounds(area, left, top, right, bottom);
        assert(right > left && bottom - top > 4);
        const std::array<float, 2> downPoint{
            float(downCaret.cursorRectX),
            motion ? downCaret.cursorRectY + downCaret.cursorRectHeight / 2.f : top + 2};
        const std::array<float, 2> motionPoint{float(motionCaret.cursorRectX), downPoint[1]};
        const std::array<float, 2> upPoint{downPoint[0], top - 2};
        assert(motionPoint[0] - downPoint[0] > 8);
        field->restoreEditingState(base);
        frame();
        TextInputEdit preedit{};
        preedit.preeditText = std::string(20, 'M');
        preedit.hasPreedit = true;
        // Observe the real painted preedit range before starting either deferred gesture.
        field->textInputApplyEdit(preedit);
        frame();
        const auto previewStart = field->textInputState();
        preedit.preeditCursorBegin = preedit.preeditCursorEnd = int(preedit.preeditText.size());
        field->textInputApplyEdit(preedit);
        frame();
        const auto previewEnd = field->textInputState();
        assert(downPoint[0] > previewStart.cursorRectX && motionPoint[0] < previewEnd.cursorRectX);
        field->textInputResetPreedit();
        field->restoreEditingState(base);
        frame();
        assert(dispatcher.inputAreaAt(upPoint[0], upPoint[1]) != area);
        assert(dispatcher.inputAreaAt(downPoint[0], downPoint[1]) == area);
        const auto scrollOffset = viewport->scrollOffset();
        std::uint32_t serial = 50000;
        assert(!ui.prepareTouchPress());
        ui.beginPointerInteraction(downPoint[0], downPoint[1]);
        dispatcher.pointerEnter(downPoint[0], downPoint[1], ++serial);
        assert(!ui.prepareTouchPress());
        ui.beginPointerInteraction(downPoint[0], downPoint[1]);
        assert(!ui.handlePointerButton(BTN_LEFT, true, downPoint[0], downPoint[1], true));
        ++serial;
        assert(dispatcher.pointerButton(downPoint[0], downPoint[1], BTN_LEFT, true, serial,
                                        serial * 10, true));
        assert(dispatcher.pendingTouchPressArea() == area);
        assert(!area->pressed() && !dispatcher.pointerCaptured());
        if (!motion) {
            // Wayland Up uses the last position; cross the field edge below scroll slop first.
            ui.handlePointerMotion(upPoint[0], upPoint[1], true);
            dispatcher.pointerMotion(upPoint[0], upPoint[1], ++serial);
            assert(dispatcher.pendingTouchPressArea() == area);
            assert(!area->pressed() && !dispatcher.pointerCaptured());
        }
        preedit.preeditCursorBegin = preedit.preeditCursorEnd = 0;
        field->textInputApplyEdit(preedit);
        assert(field->composing() && field->textGeometryDirty());
        const auto beforeSynchronization = synchronizations;
        // No fixture update/layout intervenes: the real host callback must admit current geometry.
        if (motion) {
            assert(!ui.retiredTouch());
            ui.handlePointerMotion(motionPoint[0], motionPoint[1], true);
            assert(synchronizations > beforeSynchronization && !field->textGeometryDirty() &&
                   field->composing());
            dispatcher.pointerMotion(motionPoint[0], motionPoint[1], ++serial);
            assert(area->pressed() && dispatcher.pointerCaptured());
            assert(!field->composing() && field->cursorPosition() == 16 &&
                   field->selectionAnchor() == 3);
            assert(!ui.handlePointerButton(BTN_LEFT, false, motionPoint[0], motionPoint[1], true));
            ++serial;
            assert(dispatcher.pointerButton(motionPoint[0], motionPoint[1], BTN_LEFT, false, serial,
                                            serial * 10, true));
        } else {
            assert(!ui.handlePointerButton(BTN_LEFT, false, upPoint[0], upPoint[1], true));
            assert(synchronizations > beforeSynchronization && !field->textGeometryDirty() &&
                   field->composing());
            ++serial;
            assert(dispatcher.pointerButton(upPoint[0], upPoint[1], BTN_LEFT, false, serial,
                                            serial * 10, true));
        }
        assert(!dispatcher.pendingTouchPressArea());
        assert(!ui.consumeTouchLeave());
        dispatcher.pointerLeave();
        const auto current = field->editingState();
        const auto caret = field->textInputState();
        const auto &expectedCaret = motion ? motionCaret : baseCaret;
        const std::size_t expectedRaw = motion ? 16 : 3;
        assert(!field->composing() && !field->textGeometryDirty());
        assert(current.current.value == committed && current.current.cursorPos == expectedRaw &&
               current.current.selectionAnchor == 3);
        assert(current.history == base.history && current.history->undo == undo &&
               current.history->redo == redo);
        assert(caret.surroundingText == committed && caret.cursor == int(expectedRaw) &&
               caret.anchor == 3);
        assert(caret.cursorRectX == expectedCaret.cursorRectX &&
               caret.cursorRectY == expectedCaret.cursorRectY &&
               caret.cursorRectWidth == expectedCaret.cursorRectWidth &&
               caret.cursorRectHeight == expectedCaret.cursorRectHeight);
        assert(dispatcher.focusedArea() == area && !area->pressed() &&
               !dispatcher.pointerCaptured());
        assert(viewport->scrollOffset() == scrollOffset);
        assert(model.settings.bindings.at("complete") == committed);
        assert(model.activeBuffer == edgeBuffers[0] &&
               model.buffers.at(edgeBuffers[0]).draft == edgeDrafts[0]);
        assert(composer->editingStateMatches(composerState));
        assert(composerState.history->undo == composerUndo &&
               composerState.history->redo == composerRedo);
        if (motion) {
            auto *otherArea = areaWithKey(ui.root(), "blastirc.settings.binding.history-next");
            assert(otherArea && visible(*otherArea));
            auto *other = dynamic_cast<Input *>(otherArea->textInputClient());
            assert(other && !other->textGeometryDirty());
            const auto otherState = other->editingState();
            const auto otherUndo = otherState.history->undo, otherRedo = otherState.history->redo;
            const auto captureCaret = caretAt(0);
            const auto otherCaret = other->textInputState();
            const std::array<float, 2> capturePoint{float(captureCaret.cursorRectX),
                                                    captureCaret.cursorRectY +
                                                        captureCaret.cursorRectHeight / 2.f};
            const std::array<float, 2> outsidePoint{
                motionPoint[0], otherCaret.cursorRectY + otherCaret.cursorRectHeight / 2.f};
            assert(std::abs(capturePoint[0] - downPoint[0]) > 8);
            assert(dispatcher.inputAreaAt(capturePoint[0], capturePoint[1]) == area);
            assert(dispatcher.inputAreaAt(outsidePoint[0], outsidePoint[1]) == otherArea);
            const auto captureWidth = field->width();
            dispatcher.pointerEnter(capturePoint[0], capturePoint[1], ++serial);
            ui.prepareMousePress();
            ui.beginPointerInteraction(capturePoint[0], capturePoint[1]);
            assert(
                !ui.handlePointerButton(BTN_LEFT, true, capturePoint[0], capturePoint[1], false));
            ++serial;
            assert(dispatcher.pointerButton(capturePoint[0], capturePoint[1], BTN_LEFT, true,
                                            serial, serial * 10, false));
            assert(area->pressed() && dispatcher.pointerCaptured());
            field->textInputApplyEdit(preedit);
            // Paint the preview first; the unframed focus loss below then invalidates that cache.
            frame();
            assert(field->composing() && !field->textGeometryDirty() && area->pressed() &&
                   dispatcher.pointerCaptured());
            dispatcher.setFocus(otherArea);
            assert(dispatcher.focusedArea() == otherArea && area->pressed() &&
                   dispatcher.pointerCaptured());
            assert(!field->composing() && field->textGeometryDirty() &&
                   !other->textGeometryDirty());
            // Focus loss dirtied the captured editor; neither focus nor this hit point owns the
            // drag.
            ui.handlePointerMotion(outsidePoint[0], outsidePoint[1], false);
            assert(!field->textGeometryDirty() && field->width() == captureWidth);
            assert(dispatcher.inputAreaAt(outsidePoint[0], outsidePoint[1]) != area);
            dispatcher.pointerMotion(outsidePoint[0], outsidePoint[1], ++serial);
            const auto dragged = field->editingState();
            assert(dragged.current.value == committed && dragged.current.cursorPos == 16 &&
                   dragged.current.selectionAnchor == 0);
            assert(dragged.history == base.history && dragged.history->undo == undo &&
                   dragged.history->redo == redo);
            const auto draggedCaret = field->textInputState();
            assert(draggedCaret.surroundingText == committed && draggedCaret.cursor == 16 &&
                   draggedCaret.anchor == 0 && draggedCaret.cursorRectX == motionCaret.cursorRectX);
            assert(dispatcher.focusedArea() == otherArea && other->editingStateMatches(otherState));
            assert(otherState.history->undo == otherUndo && otherState.history->redo == otherRedo);
            assert(
                !ui.handlePointerButton(BTN_LEFT, false, outsidePoint[0], outsidePoint[1], false));
            ++serial;
            assert(dispatcher.pointerButton(outsidePoint[0], outsidePoint[1], BTN_LEFT, false,
                                            serial, serial * 10, false));
            assert(!area->pressed() && !dispatcher.pointerCaptured());
            assert(field->editingStateMatches(dragged) && dispatcher.focusedArea() == otherArea);
            assert(model.settings.bindings.at("complete") == committed);
            assert(composer->editingStateMatches(composerState));
            assert(composerState.history->undo == composerUndo &&
                   composerState.history->redo == composerRedo);
        }
        dispatcher.setFocus(nullptr);
        dispatcher.setSceneRoot(nullptr);
    }
}

void typingOverflowFocusCheck() {
    using namespace blast;
    auto model = edgeModel();
    model.settings.typingReceive = true;
    auto &buffer = model.buffers.at(edgeBuffers[0]);
    for (const auto *nick : {"TypingAlice", "TypingBob", "TypingCarol", "TypingDave"})
        buffer.typing[nick] = model.now;
    for (int i = 0; i < 40; ++i) {
        Message message;
        message.id = message.text = "Typing overflow history " + std::to_string(i);
        message.nick = "HistoryPeer";
        buffer.messages.push_back(std::move(message));
    }
    Clipboard clipboard;
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    MenuHost menu;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    ui.root().setPopupContext(&menu);
    const auto frame = [&] {
        ui.update(1600, 1000, 1);
        ui.root().layout(renderer);
    };
    frame();
    auto *input = inputWithText(ui.root(), edgeDrafts[0]);
    assert(input && visible(*input));
    auto selected = input->editingState();
    selected.current.cursorPos = 8;
    selected.current.selectionAnchor = 2;
    input->restoreEditingState(selected);
    dispatcher.setFocus(input->inputArea());
    input->textInputApplyEdit({.commitText = {},
                               .preeditText = "猫",
                               .preeditCursorBegin = 3,
                               .preeditCursorEnd = 3,
                               .hasPreedit = true});
    frame();
    const auto composing = input->editingState();
    const auto undo = composing.history->undo, redo = composing.history->redo;
    const auto actions = model.actions.size(), messages = buffer.messages.size();
    const std::string composerKey(input->inputArea()->tabFocusKey());
    const auto paneKey = composerKey.substr(9, composerKey.rfind(':') - 9);
    auto *overflow = areaWithKey(ui.root(), "transcript:" + paneKey + ":typing-overflow");
    assert(overflow && visible(*overflow));
    const auto unchanged = [&] {
        assert(dispatcher.focusedArea() == input->inputArea() && input->composing());
        assert(input->editingStateMatches(composing));
        assert(composing.history->undo == undo && composing.history->redo == redo);
        assert(buffer.draft == edgeDrafts[0] && model.activeBuffer == buffer.id);
        assert(model.actions.size() == actions && buffer.messages.size() == messages);
        assert(!menu.open && menu.request.menu.empty() && menu.request.options.empty());
    };
    click(dispatcher, *overflow);
    frame();
    auto *last = labelWithText(ui.root(), "TypingDave");
    assert(last && visible(*last));
    ScrollView *list = nullptr;
    for (auto *node = last->parent(); node; node = node->parent())
        if (auto *candidate = dynamic_cast<ScrollView *>(node)) {
            list = candidate;
            break;
        }
    assert(list && labelWithText(*list, "TypingAlice") && labelWithText(*list, "TypingBob") &&
           labelWithText(*list, "TypingCarol"));
    unchanged();
    click(dispatcher, *overflow);
    frame();
    assert(!labelWithText(ui.root(), "TypingDave") ||
           !visible(*labelWithText(ui.root(), "TypingDave")));
    unchanged();
    // Explicit keyboard ownership exercises Ui routing ahead of global chat paging/cancel.
    input->textInputResetPreedit();
    auto *transcript = visibleNode<TranscriptView>(*paneFor(*input));
    auto *history = transcript ? visibleNode<ScrollView>(*transcript) : nullptr;
    assert(history && history->maxScrollOffset() > 0);
    history->setScrollOffset(0);
    frame();
    const float historyOffset = history->scrollOffset();
    dispatcher.setFocus(overflow);
    const auto keyboardDocument = input->editingState();
    hostKey(ui, dispatcher, XKB_KEY_Return);
    frame();
    assert(labelWithText(ui.root(), "TypingDave") &&
           visible(*labelWithText(ui.root(), "TypingDave")));
    for (const auto sym : std::array<std::uint32_t, 2>{XKB_KEY_Page_Down, XKB_KEY_Escape}) {
        assert(ui.handleKey({.sym = sym, .pressed = true}));
        ui.handleKey({.sym = sym, .pressed = false});
        frame();
        assert(std::abs(history->scrollOffset() - historyOffset) < .1f);
        assert(input->editingStateMatches(keyboardDocument) && buffer.draft == edgeDrafts[0]);
        assert(model.actions.size() == actions && buffer.messages.size() == messages);
    }
    assert(!labelWithText(ui.root(), "TypingDave") ||
           !visible(*labelWithText(ui.root(), "TypingDave")));
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void ordinaryTypingFocusCheck() {
    using namespace blast;
    auto model = edgeModel();
    model.settings.bindings["settings"] = "z";
    model.settings.bindings["toggle-people"] = "c-x q";
    Clipboard clipboard;
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    const auto frame = [&] {
        ui.update(1600, 1000, 1);
        ui.root().layout(renderer);
    };
    frame();
    auto *input = inputWithText(ui.root(), edgeDrafts[0]);
    assert(input && visible(*input));
    const std::string composerKey(input->inputArea()->tabFocusKey());
    const auto paneKey = composerKey.substr(9, composerKey.rfind(':') - 9);
    auto *transcript = areaWithKey(ui.root(), "transcript:" + paneKey + ":keyboard");
    auto *tool = headerTool(*input, "Search conversation");
    assert(transcript && visible(*transcript) && tool && visible(*tool));
    assert(input->replaceRange(0, input->value().size(), ""));
    for (auto *origin : {transcript, tool}) {
        dispatcher.setFocus(origin);
        hostKey(ui, dispatcher, XKB_KEY_A, KeyMod::Shift, 'A');
        assert(dispatcher.focusedArea() == input->inputArea());
    }
    dispatcher.setFocus(nullptr);
    hostKey(ui, dispatcher, 0x0100732b, KeyMod::Shift, U'猫');
    // Held-key repeats traverse the same host path without an intervening release.
    dispatcher.setFocus(transcript);
    for (int repeat = 0; repeat < 3; ++repeat) {
        const InputArea::KeyData event{.sym = XKB_KEY_b, .utf32 = 'b', .pressed = true};
        if (!ui.handleKey(event))
            dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed);
    }
    if (!ui.handleKey({.sym = XKB_KEY_b, .utf32 = 'b', .pressed = false}))
        dispatcher.keyEvent(XKB_KEY_b, 'b', 0, false);
    assert(input->value() == "AA猫bbb");
    frame();
    assert(model.buffers.at(edgeBuffers[0]).draft == input->value());
    const auto draft = input->value();
    dispatcher.setFocus(transcript);
    hostKey(ui, dispatcher, XKB_KEY_x, KeyMod::Ctrl, 'x');
    hostKey(ui, dispatcher, XKB_KEY_q, 0, 'q');
    frame();
    assert(model.settings.peopleMode == SidebarMode::Expanded && input->value() == draft);
    dispatcher.setFocus(transcript);
    hostKey(ui, dispatcher, XKB_KEY_x, KeyMod::Ctrl, 'x');
    hostKey(ui, dispatcher, XKB_KEY_v, 0, 'v');
    assert(input->value() == draft && dispatcher.focusedArea() == transcript);
    // A configured ordinary single key wins before the composer fallback.
    hostKey(ui, dispatcher, XKB_KEY_z, 0, 'z');
    frame();
    auto *dialogs = visibleNode<Dialogs>(ui.root());
    assert(dialogs && dialogs->settingsOpen() && input->value() == draft);
    dispatcher.setFocus(nullptr);
    hostKey(ui, dispatcher, XKB_KEY_a, 0, 'a');
    assert(dispatcher.focusedArea() != input->inputArea() && input->value() == draft);
    ui.execute("settings");
    frame();
    auto *searchArea = areaWithKey(ui.root(), "sidebar-search");
    auto *search = searchArea ? dynamic_cast<Input *>(searchArea->textInputClient()) : nullptr;
    assert(search && visible(*search));
    dispatcher.setFocus(searchArea);
    hostKey(ui, dispatcher, XKB_KEY_a, 0, 'a');
    assert(dispatcher.focusedArea() == searchArea && search->value() == "a" &&
           input->value() == draft);
    search->setValue("");
    // Native Space activation opens search instead of entering a space in chat.
    dispatcher.setFocus(tool);
    hostKey(ui, dispatcher, XKB_KEY_space, 0, ' ');
    frame();
    assert(dialogs->active() && !dialogs->settingsOpen() && input->value() == draft);
    hostKey(ui, dispatcher, XKB_KEY_a, 0, 'a');
    assert(dispatcher.focusedArea() != input->inputArea() && input->value() == draft);
    hostKey(ui, dispatcher, XKB_KEY_Escape);
    frame();
    dispatcher.setFocus(transcript);
    assert(!ui.handleKey(
        {.sym = XKB_KEY_dead_acute, .utf32 = 0x00b4, .pressed = true, .preedit = true}));
    assert(dispatcher.focusedArea() == transcript && input->value() == draft);
    // Hidden utility-view composers cannot be a fallback destination.
    for (const std::string kind : {"search", "mentions"}) {
        auto &utility = model.buffers["test/typing-" + kind];
        utility.id = "test/typing-" + kind;
        utility.server = "test";
        utility.kind = kind;
        utility.target = kind;
        utility.draft = "retained utility draft";
        ui.execute("buffer:" + utility.id);
        frame();
        dispatcher.setFocus(nullptr);
        hostKey(ui, dispatcher, XKB_KEY_a, 0, 'a');
        assert(!dispatcher.focusedArea() && utility.draft == "retained utility draft");
    }
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void tightSearchFocusCheck() {
    using namespace blast;
    for (const bool peopleAction : {false, true}) {
        auto model = edgeModel();
        const std::string target = "#header-overflow-with-a-long-conversation-name";
        const std::string origin = "test/" + target;
        auto &buffer = model.buffers[origin];
        buffer.id = origin;
        buffer.server = "test";
        buffer.target = target;
        buffer.kind = "channel";
        buffer.joined = true;
        buffer.draft = edgeDrafts[0];
        Nick bob{};
        bob.name = "Bob";
        buffer.nicks.push_back(std::move(bob));
        model.activeBuffer = origin;
        model.settings.workspace["native_navigation_mode"] = "compact";
        Clipboard clipboard;
        Theme theme;
        theme.update(model);
        InputDispatcher dispatcher;
        PangoRenderer renderer;
        MenuHost menu;
        Ui ui(model, clipboard);
        dispatcher.setSceneRoot(&ui.root());
        ui.setInputDispatcher(dispatcher);
        ui.setWindowActive(true);
        ui.root().setPopupContext(&menu);
        const auto frame = [&](float width, float height) {
            ui.update(width, height, 1);
            ui.root().layout(renderer);
        };
        const auto wide = [&] {
            model.settings.scale = 1;
            theme.update(model);
            frame(1600, 1000);
        };
        const auto tight = [&] {
            model.settings.scale = 2;
            theme.update(model);
            frame(720, 480);
        };
        wide();
        auto *input = inputWithText(ui.root(), edgeDrafts[0]);
        assert(input && visible(*input));
        auto *header = headerFor(*input);
        auto *tool = headerTool(*input, peopleAction ? "People" : "Search conversation");
        auto *more = headerTool(*input, "More conversation actions");
        assert(visible(*header) && visible(*tool) && visible(*more));
        dispatcher.setFocus(tool);
        tight();
        assert(visible(*header) && visible(*more));
        const bool directTool = visible(*tool);
        assert(dispatcher.focusedArea() == (directTool ? tool : more));
        assert(model.activeBuffer == origin && visibleCount<TranscriptView>(ui.root()) == 1);
        // The first Enter activates the surviving focused action, whether direct or overflowed.
        nativeKey(ui, dispatcher, XKB_KEY_Return);
        frame(720, 480);
        assert(menu.open == !directTool);
        Dialogs *dialogs = nullptr;
        for (const auto &child : ui.root().children())
            if (auto *candidate = dynamic_cast<Dialogs *>(child.get()))
                dialogs = candidate;
        assert(dialogs);
        if (peopleAction) {
            if (menu.open)
                menu.select(commandLabel(
                    model,
                    sidebarModeToggleLabel("People", SidebarMode::Hidden, SidebarMode::Hidden),
                    "toggle-people"));
            frame(720, 480);
            assert(model.settings.peopleMode == SidebarMode::Expanded && !dialogs->active());
            wide();
            assert(visibleCount<People>(ui.root()) == 1);
            auto *member = memberRow(ui.root(), "Bob");
            assert(member && visible(*member));
            ui.execute("toggle-people");
            ui.execute("toggle-people");
            frame(1600, 1000);
        } else {
            if (menu.open)
                menu.select(commandLabel(model, commandDisplayName("search"), "search"));
            frame(720, 480);
            assert(dialogs->active() && visible(*dialogs) && !dialogs->settingsOpen());
            auto *query = dispatcher.focusedArea()
                              ? dynamic_cast<Input *>(dispatcher.focusedArea()->textInputClient())
                              : nullptr;
            assert(query && query != input && visible(*query));
            TextInputEdit edit{};
            edit.commitText = "responsive header query";
            edit.hasCommitText = true;
            static_cast<TextInputClient &>(*query).textInputApplyEdit(edit);
            const auto firstAction = model.actions.size();
            nativeKey(ui, dispatcher, XKB_KEY_Return);
            frame(720, 480);
            assert(!dialogs->active());
            assert(std::count_if(model.actions.begin() + firstAction, model.actions.end(),
                                 [&](const Action &action) {
                                     return action.type == "search" && action.buffer == origin &&
                                            action.text == "responsive header query";
                                 }) == 1);
            wide();
        }
        assert(visible(*tool) && model.settings.peopleMode == SidebarMode::Hidden &&
               !dialogs->active());
        std::uint32_t serial = 20000;
        const auto nativeButton = [&](const std::array<float, 2> &point, bool pressed) {
            ++serial;
            if (pressed)
                ui.beginPointerInteraction(point[0], point[1]);
            if (ui.handlePointerButton(BTN_LEFT, pressed, point[0], point[1], false))
                return;
            const bool handled = dispatcher.pointerButton(point[0], point[1], BTN_LEFT, pressed,
                                                          serial, serial * 10, false);
            if (pressed)
                assert(handled);
        };
        dispatcher.setFocus(tool);
        nativeButton(pointerPoint(*tool), true);
        assert(tool->pressed() && dispatcher.pointerCaptured());
        tight();
        assert(dispatcher.focusedArea() == (visible(*tool) ? tool : more));
        if (!visible(*tool))
            assert(!tool->pressed());
        // Layout cancels hidden controls; the host retires dispatcher capture on the next pointer
        // event. Verify capture retirement after the real host release path below, not mid-frame.
        // A retained action may still own the press; release outside so resizing cannot activate
        // it.
        nativeButton({-10, -10}, false);
        assert(!tool->pressed() && !dispatcher.pointerCaptured());
        frame(720, 480);
        assert(!dialogs->active() && model.settings.peopleMode == SidebarMode::Hidden &&
               !menu.open);
        assert(model.activeBuffer == origin && buffer.draft == edgeDrafts[0]);
        dispatcher.setFocus(more);
        nativeKey(ui, dispatcher, XKB_KEY_Return);
        assert(menu.open);
        const auto firstAction = model.actions.size();
        menu.select("Load older messages");
        frame(720, 480);
        assert(std::count_if(model.actions.begin() + firstAction, model.actions.end(),
                             [&](const Action &action) {
                                 return action.type == "history" && action.buffer == origin;
                             }) == 1);
        assert(dispatcher.focusedArea() && visible(*dispatcher.focusedArea()));
        dispatcher.setFocus(nullptr);
        dispatcher.setSceneRoot(nullptr);
    }
}

void channelTopicCheck() {
    using namespace blast;
    auto model = edgeModel();
    auto &channel = model.buffers.at(edgeBuffers[0]);
    channel.kind = "channel";
    channel.joined = true;
    channel.topic = "\x02猫\x02 — raw topic\nsecond line \x03"
                    "04https://example.invalid/topic\x0f";
    std::string displayedTopic = "猫 — raw topic\nsecond line https://example.invalid/topic";
    model.settings.workspace["native_navigation_mode"] = "hidden";
    struct TopicClipboard final : TextClipboard {
        std::string copied;
        std::optional<std::string> clipboardText() override { return copied; }
        void setClipboardText(std::string value) override { copied = std::move(value); }
    } clipboard;
    auto *previousClipboard = Input::textClipboard();
    Input::setTextClipboard(&clipboard);
    Theme theme;
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    for (const float width : {1600.f, 720.f}) {
        theme.update(model);
        ui.update(width, 480, 1);
        ui.root().layout(renderer);
        auto *composer = inputWithText(ui.root(), edgeDrafts[0]);
        assert(composer);
        auto *people = headerTool(*composer, "People");
        auto *topic = headerTool(*composer, "Channel topic");
        assert(visible(*people) && visible(*topic));
        assert(topic->x() >= people->x() + people->width());
        assert(topic->x() + topic->width() <= headerFor(*composer)->width() + .5f);
        dispatcher.setFocus(topic);
        click(dispatcher, *topic);
        ui.update(width, 480, 1);
        ui.root().layout(renderer);
        auto *text = inputWithText(ui.root(), channel.topic);
        assert(text && visible(*text) && text->height() > 2 * topic->height());
        assert(text->displayProjection() && text->displayProjection()->text == displayedTopic);
        text->selectAll();
        const auto selected = text->editingState();
        assert(selected.current.cursorPos != selected.current.selectionAnchor);
        nativeKey(ui, dispatcher, 'c', KeyMod::Ctrl);
        assert(clipboard.copied == channel.topic);
        assert(!text->insertText("must not modify the topic") && text->value() == channel.topic);
        const auto originalTopic = channel.topic;
        channel.topic.clear();
        ui.update(width, 480, 1);
        ui.root().layout(renderer);
        assert(visible(*text) && text->value().empty());
        channel.topic = originalTopic + "\nLive topic update";
        displayedTopic += "\nLive topic update";
        ui.update(width, 480, 1);
        ui.root().layout(renderer);
        assert(visible(*text) && text->value() == channel.topic);
        assert(text->displayProjection() && text->displayProjection()->text == displayedTopic);
        nativeKey(ui, dispatcher, XKB_KEY_Escape);
        assert(!visible(*text));
        assert(dispatcher.focusedArea() == topic);
        click(dispatcher, *topic);
        ui.update(width, 480, 1);
        ui.root().layout(renderer);
        assert(visible(*text));
        ui.execute("execute-command");
        ui.update(width, 480, 1);
        ui.root().layout(renderer);
        auto *paletteQuery =
            dispatcher.focusedArea()
                ? dynamic_cast<Input *>(dispatcher.focusedArea()->textInputClient())
                : nullptr;
        assert(paletteQuery && paletteQuery != composer && paletteQuery != text &&
               visible(*paletteQuery) && !visible(*text));
        nativeKey(ui, dispatcher, XKB_KEY_Escape);
    }
    channel.kind = "query";
    ui.update(720, 480, 1);
    ui.root().layout(renderer);
    auto *composer = inputWithText(ui.root(), edgeDrafts[0]);
    assert(composer && !visible(*headerTool(*composer, "Channel topic")));
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);

    TopicPopover popover;
    dispatcher.setSceneRoot(&popover);
    popover.setInputDispatcher(dispatcher);
    popover.sync(320, 240, 1);
    int dismissed = 0;
    std::string raw;
    for (int i = 0; i < 30; ++i)
        raw += "猫 topic segment with selectable Unicode and wrapping ";
    raw += "\nlast line";
    popover.show(raw, {.x = 305, .y = 8, .width = 15, .height = 24}, [&] { ++dismissed; });
    popover.layout(renderer);
    auto &text = popover.topicInput();
    assert(popover.active() && text.value() == raw && visible(text));
    std::array<float, 4> bounds{};
    Node::transformedBounds(&text, bounds[0], bounds[1], bounds[2], bounds[3]);
    assert(bounds[0] >= 0 && bounds[1] >= 0 && bounds[2] <= 320 && bounds[3] <= 240);
    assert(text.height() > popover.height() / 3 && text.width() > 0);
    text.selectAll();
    assert(text.editingState().current.cursorPos != text.editingState().current.selectionAnchor);
    assert(dispatcher.focusedArea() == text.inputArea());
    const InputArea::KeyData tab{.sym = XKB_KEY_Tab, .pressed = true};
    assert(popover.handleKey(tab));
    auto *close = dispatcher.focusedArea();
    assert(close && close != text.inputArea() && visible(*close));
    assert(popover.handleKey(tab) && dispatcher.focusedArea() == text.inputArea());
    assert(popover.handleKey({.sym = XKB_KEY_Tab, .modifiers = KeyMod::Shift, .pressed = true}));
    assert(dispatcher.focusedArea() == close);
    dispatcher.setFocus(text.inputArea());
    assert(!text.insertText("rejected") && text.value() == raw);
    text.textInputApplyEdit(
        {.commitText = "also rejected", .preeditText = {}, .hasCommitText = true});
    assert(text.value() == raw);
    const InputArea::KeyData escape{.sym = XKB_KEY_Escape, .pressed = true};
    assert(popover.handleKey(escape) && dismissed == 1 && !popover.active());
    assert(!popover.handleKey(escape) && dismissed == 1);
    popover.show("", {.x = 305, .y = 8, .width = 15, .height = 24}, [&] { ++dismissed; });
    popover.layout(renderer);
    assert(popover.topicInput().value().empty() && visible(popover.topicInput()));
    assert(popover.handleKey(escape) && dismissed == 2 && !popover.active());
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
    Input::setTextClipboard(previousClipboard);
}

void alphaForegroundCheck() {
    using namespace blast;
    const auto *configHome = std::getenv("XDG_CONFIG_HOME");
    assert(configHome);
    const auto path = std::filesystem::path(configHome) / "blastirc" / "noctalia.toml";
    assert(!std::filesystem::exists(path));
    std::filesystem::create_directories(path.parent_path());
    {
        std::ofstream output(path);
        output << "[colors]\n"
                  "primary = '#ffffff33'\n"
                  "on_primary = '#ffffff22'\n"
                  "surface = '#eeeeee22'\n"
                  "on_surface = '#11111144'\n"
                  "on_surface_variant = '#22222244'\n"
                  "secondary = '#33669900'\n"
                  "tertiary = '#735573'\n"
                  "error = '#ba1a1a'\n"
                  "terminal_foreground = '#12345644'\n";
        output.close();
        assert(output);
    }
    auto model = edgeModel();
    model.settings.peopleMode = SidebarMode::Compact;
    auto &memberBuffer = model.buffers.at(edgeBuffers[0]);
    memberBuffer.kind = "channel";
    memberBuffer.joined = true;
    memberBuffer.nicks.push_back({.name = "BadgeOperator", .account = {}, .prefix = "@"});
    model.buffers.at(edgeBuffers[2]).unread = 7;
    auto &unread = model.buffers.at(edgeBuffers[2]);
    Message mention;
    mention.id = "sidebar-mention";
    mention.nick = "Person";
    mention.text = "Me: retained unread mention";
    mention.mention = true;
    unread.messages.push_back(std::move(mention));
    for (int index = 0; index < 7; ++index) {
        Message message;
        message.id = "sidebar-unread-" + std::to_string(index);
        message.nick = "Person";
        message.text = "Retained unread message " + std::to_string(index);
        unread.messages.push_back(std::move(message));
    }
    ++unread.messagesRevision;
    unread.readMarker = "local:sidebar-mention";
    model.settings.pinnedBuffers = {unread.id};
    Clipboard clipboard;
    ServerConfig account;
    account.name = "badge-account";
    account.host = "bouncer.invalid";
    account.bouncer = "soju";
    account.sojuAccount = true;
    model.servers.push_back(account);
    model.servers[0].sojuParent = account.name;
    model.servers[0].sojuNetworkId = "7";
    model.settings.workspace["soju-group:badge-account"] = "expanded";
    auto &accountUnread = model.buffers["badge-account/query"];
    accountUnread = unread;
    accountUnread.id = "badge-account/query";
    accountUnread.server = account.name;
    Theme theme;
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    const auto frame = [&] {
        theme.update(model);
        ui.update(1600, std::max(1000.f, static_cast<float>(minimumWindowHeight(renderer))), 1);
        ui.root().layout(renderer);
    };
    // Both ordinary and unusually tall native struts must fit without font-specific policy.
    for (const auto *family : {"sans-serif", "DejaVu Math TeX Gyre"})
        for (const float scale : {.7f, 1.f, 2.f})
            for (const bool transparent : {false, true}) {
                model.settings.uiFont = family;
                model.settings.transparency = transparent;
                model.settings.scale = scale;
                model.settings.opacity = .4f;
                frame();
                assert(themeColor("on_surface").a < 1.f && themeColor("secondary").a == 0.f);
                auto *member = memberRow(ui.root(), "@BadgeOperator");
                auto *privilege = member ? labelWithText(*member, "@") : nullptr;
                assert(member && privilege && visible(*privilege));
                bool privilegePlate = false;
                for (const auto &child : member->children())
                    if (const auto *rect = dynamic_cast<const RectNode *>(child.get());
                        rect && visible(*rect) && rect->style().radius == 4 * scale &&
                        rect->style().fill == themeSurfaceColor("primary")) {
                        privilegePlate = true;
                        assert(rect->style().fill.a == (transparent ? .4f : 1.f));
                    }
                assert(privilegePlate);
                auto *row = areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[2]);
                assert(row && visible(*row));
                auto *badge = labelWithText(*row, "7");
                assert(badge && visible(*badge));
                auto *rowTitle = labelWithText(*row, unread.target);
                assert(rowTitle);
                const auto checkRow = [&](const InputArea &owner) {
                    assert(owner.parent());
                    for (const auto &sibling : owner.parent()->children()) {
                        const auto *neighbor = dynamic_cast<const InputArea *>(sibling.get());
                        if (!neighbor || neighbor == &owner || !visible(*neighbor))
                            continue;
                        assert(owner.y() + owner.height() <= neighbor->y() + .5f ||
                               neighbor->y() + neighbor->height() <= owner.y() + .5f);
                    }
                    for (const auto &child : owner.children()) {
                        const auto *caption = dynamic_cast<const Label *>(child.get());
                        if (!caption || !visible(*caption) || caption->text().empty())
                            continue;
                        const TextNode *glyphs = nullptr;
                        for (const auto &node : caption->children())
                            if (const auto *text = dynamic_cast<const TextNode *>(node.get()))
                                glyphs = text;
                        assert(glyphs);
                        const auto metrics = renderer.measureText(
                            glyphs->text(), glyphs->fontSize(), glyphs->font(), glyphs->maxWidth(),
                            glyphs->maxLines(), glyphs->textAlign(), glyphs->ellipsize(),
                            glyphs->useMarkup());
                        assert(caption->height() + .5f >= metrics.layoutHeight);
                        assert(caption->height() + .5f >=
                               renderer.fontRowExtent(caption->fontSize(), caption->font()));
                        if (caption == rowTitle || caption == badge) {
                            assert(caption->x() >= 0 && caption->y() >= 0 &&
                                   caption->x() + caption->width() <= owner.width() + .5f &&
                                   caption->y() + caption->height() <= owner.height() + .5f);
                        }
                        // Initials deliberately center painted ink, not their font's
                        // unpainted leading. Prove actual glyph containment for every label.
                        float sceneX, sceneY, baselineX, baselineY;
                        Node::mapToScene(glyphs, 0, 0, sceneX, sceneY);
                        Node::mapFromScene(&owner, sceneX, sceneY, baselineX, baselineY);
                        const float inkLeft = baselineX + metrics.inkLeft - metrics.alignOffsetX;
                        const float inkRight = baselineX + metrics.inkRight - metrics.alignOffsetX;
                        const float inkTop = baselineY + metrics.inkTop;
                        const float inkBottom = baselineY + metrics.inkBottom;
                        assert(inkRight > inkLeft && inkBottom > inkTop && inkLeft >= 0 &&
                               inkTop >= 0 && inkRight <= owner.width() + .5f &&
                               inkBottom <= owner.height() + .5f);
                    }
                };
                const auto checkBadge = [&](const Node &owner, const Label &label, Color fill) {
                    const Color expectedFill = fill;
                    const RectNode *plate = nullptr;
                    for (const auto &child : owner.children())
                        if (const auto *rect = dynamic_cast<const RectNode *>(child.get());
                            rect && visible(*rect) && rect->style().fill == expectedFill &&
                            rect->style().radius == 4 * model.settings.scale)
                            plate = rect;
                    assert(plate && label.color().a == 1.f);
                    assert(plate->style().fill.a == 1.f);
                    if (&owner == row)
                        checkRow(*row);
                    const float nativeRow = renderer.fontRowExtent(label.fontSize(), label.font());
                    assert(label.height() + .5f >= nativeRow);
                    assert(plate->height() >= 16 * scale && plate->width() >= plate->height());
                    const auto &radius = plate->style().radius;
                    for (const float corner : {radius.tl, radius.tr, radius.br, radius.bl})
                        assert(corner > 0 && corner < plate->height() / 2);
                    assert(label.x() >= plate->x() && label.y() >= plate->y());
                    assert(label.x() + label.width() <= plate->x() + plate->width() + .5f);
                    assert(label.y() + label.height() <= plate->y() + plate->height() + .5f);
                    assert(plate->x() >= 0 && plate->y() >= 0);
                    assert(plate->x() + plate->width() <= owner.width() + .5f);
                    assert(plate->y() + plate->height() <= owner.height() + .5f);
                    // Compare effective ink/plate colors, not just nominal palette roles.
                    for (const Color backdrop : {hex("#000000"), hex("#ffffff"), hex("#ff00ff")}) {
                        const auto over = [](Color front, Color back) {
                            return Color{front.r * front.a + back.r * (1 - front.a),
                                         front.g * front.a + back.g * (1 - front.a),
                                         front.b * front.a + back.b * (1 - front.a), 1};
                        };
                        const Color background = over(plate->style().fill, backdrop);
                        const Color ink = over(label.color(), background);
                        const float fg = relativeLuminance(ink), bg = relativeLuminance(background);
                        assert((std::max(fg, bg) + .05f) / (std::min(fg, bg) + .05f) >= 4.5f);
                    }
                };
                checkBadge(*row, *badge, ::palette.primary);
                auto *server = areaWithKey(ui.root(), "server:test");
                assert(server);
                auto *serverBadge = labelWithText(*server, "7");
                assert(serverBadge && visible(*serverBadge));
                checkBadge(*server, *serverBadge, ::palette.primary);
                auto *accountRow = areaWithKey(ui.root(), "server:badge-account");
                auto *accountBadge = accountRow ? labelWithText(*accountRow, "7") : nullptr;
                assert(accountRow && accountBadge && visible(*accountBadge));
                checkBadge(*accountRow, *accountBadge, ::palette.primary);
                const auto checkCountHeight = [&] {
                    const std::array owners{row, server, accountRow};
                    std::array<std::array<float, 2>, 3> geometry{};
                    for (std::size_t index = 0; index < owners.size(); ++index)
                        geometry[index] = {owners[index]->y(), owners[index]->height()};
                    unread.unread = 0;
                    accountUnread.unread = 0;
                    frame();
                    assert(!visible(*badge) && !visible(*serverBadge) && !visible(*accountBadge));
                    for (std::size_t index = 0; index < owners.size(); ++index) {
                        assert(std::abs(owners[index]->y() - geometry[index][0]) < .01f);
                        assert(std::abs(owners[index]->height() - geometry[index][1]) < .01f);
                    }
                    checkRow(*row);
                    unread.unread = 7;
                    accountUnread.unread = 7;
                    frame();
                    for (std::size_t index = 0; index < owners.size(); ++index) {
                        assert(std::abs(owners[index]->y() - geometry[index][0]) < .01f);
                        assert(std::abs(owners[index]->height() - geometry[index][1]) < .01f);
                    }
                    checkRow(*row);
                    assert(visible(*badge) && visible(*serverBadge) && visible(*accountBadge));
                };
                auto *caption = labelWithText(*row, unread.target);
                assert(caption && visible(*caption));
                assert(caption->y() >= 2 * scale);
                assert(caption->y() + caption->height() + 2 * scale <= row->height() + .5f);
                checkCountHeight();
                const auto checkHovered = [&](InputArea &area, const Label &label, Color fill) {
                    const auto point = pointerPoint(area);
                    dispatcher.pointerMotion(point[0], point[1], 30000);
                    frame();
                    assert(area.hovered());
                    checkBadge(area, label, fill);
                    dispatcher.pointerMotion(-10, -10, 30001);
                    frame();
                };
                const auto checkSelected = [&](const Label &label, Color fill) {
                    const auto origin = model.activeBuffer;
                    model.activeBuffer = unread.id;
                    frame();
                    checkBadge(*row, label, fill);
                    checkHovered(*row, label, fill);
                    model.activeBuffer = origin;
                    frame();
                };
                const auto checkMuted = [&](const Label &label, Color unmutedFill) {
                    const auto count = unread.unread;
                    const auto marker = unread.readMarker;
                    const std::string text(label.text());
                    const Color unmutedInk = label.color();
                    unread.muted = true;
                    frame();
                    const Color mutedFill = themeForegroundColor("surface_container_highest");
                    assert(mutedFill != unmutedFill);
                    assert(relativeLuminance(mutedFill) < relativeLuminance(unmutedFill));
                    assert(label.color() ==
                           withAlpha(themePairedForeground(
                                         mutedFill, themeForegroundColor("on_surface_variant")),
                                     1.f));
                    checkBadge(*row, label, mutedFill);
                    checkHovered(*row, label, mutedFill);
                    checkSelected(label, mutedFill);
                    assert(visible(label) && label.text() == text);
                    assert(unread.unread == count && unread.readMarker == marker);
                    unread.muted = false;
                    frame();
                    assert(label.color() == unmutedInk && label.text() == text);
                    checkBadge(*row, label, unmutedFill);
                };
                checkSelected(*badge, ::palette.primary);
                checkHovered(*row, *badge, ::palette.primary);
                checkMuted(*badge, ::palette.primary);
                checkHovered(*server, *serverBadge, ::palette.primary);
                checkHovered(*accountRow, *accountBadge, ::palette.primary);
                unread.readMarker.clear();
                unread.unread = 8;
                frame();
                auto *expandedMention = labelWithText(*row, "@8");
                assert(expandedMention && visible(*expandedMention));
                checkBadge(*row, *expandedMention, ::palette.error);
                checkHovered(*row, *expandedMention, ::palette.error);
                checkSelected(*expandedMention, ::palette.error);
                checkMuted(*expandedMention, ::palette.error);
                unread.readMarker = "local:sidebar-mention";
                unread.unread = 7;
                frame();
                for (const auto &child : row->children())
                    if (const auto *label = dynamic_cast<const Label *>(child.get()))
                        assert(label->color().a == 1.f);
                assert(badge->color() == ::palette.onPrimary);
                ui.execute("toggle-navigation");
                frame();
                assert(std::abs(row->width() - 48 * scale) < .01f && visible(*row));
                badge = labelWithText(*row, "7");
                assert(badge && visible(*badge));
                assert(badge->color() == ::palette.onPrimary);
                checkBadge(*row, *badge, ::palette.primary);
                checkHovered(*row, *badge, ::palette.primary);
                checkSelected(*badge, ::palette.primary);
                checkMuted(*badge, ::palette.primary);
                checkCountHeight();
                unread.unread = 100;
                frame();
                auto *capped = labelWithText(*row, "9+");
                auto *serverCapped = labelWithText(*server, "99+");
                assert(capped && serverCapped && visible(*capped) && visible(*serverCapped));
                checkBadge(*row, *capped, ::palette.primary);
                checkBadge(*server, *serverCapped, ::palette.primary);
                checkMuted(*capped, ::palette.primary);
                unread.unread = 7;
                frame();
                const float fg = relativeLuminance(badge->color());
                const float bg = relativeLuminance(::palette.primary);
                assert((std::max(fg, bg) + .05f) / (std::min(fg, bg) + .05f) >= 4.5f);
                dispatcher.setFocus(row);
                int focusedBorders = 0;
                const auto opaqueBorders = [&](auto &&self, const Node &node) -> void {
                    if (const auto *rect = dynamic_cast<const RectNode *>(&node);
                        rect && visible(*rect) && rect->style().borderWidth > 0) {
                        ++focusedBorders;
                        assert(rect->style().border.a == 1.f);
                    }
                    for (const auto &child : node.children())
                        self(self, *child);
                };
                opaqueBorders(opaqueBorders, *row);
                assert(focusedBorders > 0);
                unread.readMarker.clear();
                unread.unread = 8;
                frame();
                auto *mentionBadge = labelWithText(*row, "@8");
                assert(mentionBadge && visible(*mentionBadge) && mentionBadge->color().a == 1.f);
                assert(mentionBadge->color() == ::palette.onError);
                checkBadge(*row, *mentionBadge, ::palette.error);
                checkHovered(*row, *mentionBadge, ::palette.error);
                checkSelected(*mentionBadge, ::palette.error);
                checkMuted(*mentionBadge, ::palette.error);
                serverBadge = labelWithText(*server, "8");
                assert(serverBadge && visible(*serverBadge));
                checkBadge(*server, *serverBadge, ::palette.error);
                checkHovered(*server, *serverBadge, ::palette.error);
                accountUnread.readMarker.clear();
                accountUnread.unread = 8;
                frame();
                accountBadge = labelWithText(*accountRow, "8");
                assert(accountBadge && visible(*accountBadge));
                checkBadge(*accountRow, *accountBadge, ::palette.error);
                checkHovered(*accountRow, *accountBadge, ::palette.error);
                accountUnread.readMarker = "local:sidebar-mention";
                accountUnread.unread = 7;
                const auto tooltip = row->tooltipContent();
                const auto *textTooltip = std::get_if<std::string>(&tooltip);
                assert(textTooltip && textTooltip->find("Unread mention") != std::string::npos);
                unread.readMarker = "local:sidebar-mention";
                unread.unread = 7;
                frame();
                assert(visible(*badge) && badge->text() == "7");
                ui.execute("toggle-navigation");
                ui.execute("toggle-navigation");
                frame();
                auto *input = inputWithText(ui.root(), edgeDrafts[0]);
                assert(input && visible(*input));
                dispatcher.setFocus(input->inputArea());
                frame();
                auto *text = labelWithText(*input, edgeDrafts[0]);
                assert(text && visible(*text) && text->color().a == 1.f);
                assert(model.buffers.at(edgeBuffers[0]).draft == edgeDrafts[0]);
            }
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
    std::filesystem::remove(path);
}

void transcriptActionFocusCheck(bool link) {
    using namespace blast;
    constexpr std::string_view url = "https://example.invalid/native-keyboard-focus";
    auto model = edgeModel();
    auto &buffer = model.buffers.at(edgeBuffers[0]);
    model.networks[buffer.server].caps = {"message-tags"};
    model.networks[buffer.server].state = "Ready";
    Message message;
    message.id = "native-focus-message";
    message.nick = "Peer";
    message.text = "FocusAnchor " + std::string(url);
    message.time = "2026-11-07T12:34:56Z";
    message.reactions["👍"] = {"Peer"};
    buffer.messages.push_back(std::move(message));
    ++buffer.messagesRevision;
    Clipboard clipboard;
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    const auto frame = [&] {
        ui.update(1600, 1000, 1);
        ui.root().layout(renderer);
    };
    frame();
    auto *composer = inputWithText(ui.root(), edgeDrafts[0]);
    assert(composer && visible(*composer));
    const auto editorState = composer->editingState();
    if (!link) {
        auto *unread = areaWithKey(ui.root(), "sidebar-filter:unread");
        assert(unread && visible(*unread));
        model.buffers.at(edgeBuffers[1]).unread = 1;
        click(dispatcher, *unread);
        frame();
        auto *sidebarOrigin = areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[1]);
        auto *linkOrigin = areaWithTooltip(*paneFor(*composer), url);
        assert(sidebarOrigin && linkOrigin && visible(*sidebarOrigin) && visible(*linkOrigin));
        for (auto *origin : {sidebarOrigin, linkOrigin}) {
            dispatcher.setFocus(origin);
            nativeKey(ui, dispatcher, 'x', KeyMod::Alt, 'x');
            frame();
            auto *chooser = dispatcher.focusedArea()
                                ? dynamic_cast<Input *>(dispatcher.focusedArea()->textInputClient())
                                : nullptr;
            assert(chooser && chooser != composer && visible(*chooser));
            TextInputEdit commandEdit{};
            commandEdit.commitText = "jump-channel";
            commandEdit.hasCommitText = true;
            static_cast<TextInputClient &>(*chooser).textInputApplyEdit(commandEdit);
            nativeKey(ui, dispatcher, XKB_KEY_Return);
            frame();
            auto *jump = areaWithKey(ui.root(), "channel-jump");
            assert(jump && visible(*jump) && dispatcher.focusedArea() == jump);
            nativeKey(ui, dispatcher, XKB_KEY_Escape);
            frame();
            assert(!visible(*jump) && dispatcher.focusedArea() == origin && visible(*origin));
            assert(model.activeBuffer == buffer.id && buffer.draft == edgeDrafts[0] &&
                   composer->editingStateMatches(editorState));
        }
    }
    {
        dispatcher.setFocus(nullptr);
        theme.update(model);
        frame();
        TranscriptView *transcript = nullptr;
        for (const auto &child : paneFor(*composer)->children())
            if (auto *view = dynamic_cast<TranscriptView *>(child.get()))
                transcript = view;
        assert(transcript && visible(*transcript));
        const auto actionControl = [&](bool forLink) -> InputArea * {
            if (forLink)
                return areaWithTooltip(*transcript, url);
            auto *label = labelWithText(*transcript, "👍 1");
            assert(label);
            for (auto *node = label->parent(); node; node = node->parent())
                if (auto *button = dynamic_cast<Button *>(node)) {
                    assert(button->enabled());
                    return button->inputArea();
                }
            return nullptr;
        };
        if (!link)
            for (const bool focusLink : {true, false}) {
                model.buffers.at(edgeBuffers[1]).unread = 1;
                model.buffers.at(edgeBuffers[2]).unread = 0;
                model.buffers.at(edgeBuffers[3]).unread = 0;
                frame();
                auto *control = actionControl(focusLink);
                assert(control && visible(*control));
                dispatcher.setFocus(control);
                nativeKey(ui, dispatcher, 'x', KeyMod::Ctrl);
                nativeKey(ui, dispatcher, 'j', 0, 'j');
                frame();
                auto *jump = areaWithKey(ui.root(), "channel-jump");
                assert(jump && visible(*jump));
                model.buffers.at(edgeBuffers[1]).unread = 0;
                model.buffers.at(edgeBuffers[2]).unread = 1;
                model.buffers.at(edgeBuffers[3]).unread = 2;
                frame();
                nativeKey(ui, dispatcher, XKB_KEY_Escape);
                frame();
                assert(!visible(*jump) && dispatcher.focusedArea() == control);
                assert(visible(*control));
                assert(!visible(*areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[1])));
                assert(visible(*areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[2])));
                assert(visible(*areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[3])));
                assert(model.activeBuffer == buffer.id &&
                       composer->editingStateMatches(editorState) && buffer.draft == edgeDrafts[0]);
            }
        for (const auto key : {XKB_KEY_Return, XKB_KEY_space}) {
            auto *control = actionControl(link);
            assert(control && visible(*control));
            dispatcher.setFocus(control);
            assert(dispatcher.focusedArea() == control);
            const auto firstAction = model.actions.size();
            constexpr std::string_view priorStatus = "Waiting for transcript keyboard action";
            model.status = priorStatus;
            nativeKey(ui, dispatcher, key);
            if (link) {
                // The isolated wrapper child has no xdg-open in its explicit environment.
                assert(model.status != priorStatus);
            } else
                assert(std::count_if(model.actions.begin() + firstAction, model.actions.end(),
                                     [&](const Action &action) {
                                         return action.type == "react" &&
                                                action.buffer == buffer.id && action.text == "👍" &&
                                                action.extra == "native-focus-message";
                                     }) == 1);
            assert(dispatcher.focusedArea() == control);
            frame();
            assert(dispatcher.focusedArea() == control && visible(*control));
            if (link) {
                ModeLine *line = nullptr;
                for (const auto &child : ui.root().children())
                    if (auto *candidate = dynamic_cast<ModeLine *>(child.get()))
                        line = candidate;
                assert(line && visible(*line));
                auto *feedback = labelWithText(*line, model.status);
                assert(feedback && visible(*feedback));
            }
            assert(model.activeBuffer == buffer.id && composer->editingStateMatches(editorState));
            assert(buffer.draft == edgeDrafts[0]);
        }
    }
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void modalFocusRemapCheck(
    blast::Model &model,
    const std::function<void(const std::string &, const std::string &)> &renameBuffer,
    bool convergence) {
    using namespace blast;
    model = edgeModel();
    const std::string sourceId = edgeBuffers[0], destinationId = edgeBuffers[1];
    constexpr std::string_view sourceUrl = "https://example.invalid/modal-origin";
    const auto addMessage = [&](const std::string &buffer, std::string id, std::string url) {
        Message message;
        message.id = std::move(id);
        message.nick = "Peer";
        message.text = "Modal focus " + std::move(url);
        message.time = "2026-11-07T12:34:56Z";
        model.buffers.at(buffer).messages.push_back(std::move(message));
        ++model.buffers.at(buffer).messagesRevision;
    };
    addMessage(sourceId, "modal-source", std::string(sourceUrl));
    if (convergence)
        for (int i = 0; i < 3; ++i)
            addMessage(destinationId, "modal-destination-" + std::to_string(i),
                       "https://example.invalid/modal-destination-" + std::to_string(i));
    model.buffers.at(edgeBuffers[2]).unread = 1;
    Clipboard clipboard;
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    const auto frame = [&] {
        ui.update(1600, 1000, 1);
        ui.root().layout(renderer);
    };
    frame();
    Dialogs *dialogs = nullptr;
    for (const auto &child : ui.root().children())
        if (auto *candidate = dynamic_cast<Dialogs *>(child.get()))
            dialogs = candidate;
    assert(dialogs);
    auto *unread = areaWithKey(ui.root(), "sidebar-filter:unread");
    assert(unread && visible(*unread));
    click(dispatcher, *unread);
    frame();
    assert(visible(*areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[2])));
    Input *destination = nullptr;
    std::optional<Input::EditingState> destinationState;
    if (convergence) {
        ui.execute("buffer:" + destinationId);
        frame();
        destination = inputWithText(ui.root(), edgeDrafts[1]);
        assert(destination && visible(*destination) && destination->insertText("!"));
        ui.execute("undo");
        destinationState = destination->editingState();
        for (int i = 0; i < 3; ++i) {
            auto *link =
                areaWithTooltip(*paneFor(*destination),
                                "https://example.invalid/modal-destination-" + std::to_string(i));
            assert(link && visible(*link));
        }
        ui.execute("buffer:" + sourceId);
        frame();
    }
    auto *source = inputWithText(ui.root(), edgeDrafts[0]);
    assert(source && visible(*source) && source->insertText("!"));
    ui.execute("undo");
    const auto sourceState = source->editingState();
    if (convergence)
        dispatcher.setFocus(source->inputArea());
    else {
        auto *link = areaWithTooltip(*paneFor(*source), sourceUrl);
        assert(link && visible(*link));
        dispatcher.setFocus(link);
    }
    ui.execute("settings");
    frame();
    assert(dialogs->settingsOpen() && visible(*dialogs));
    model.buffers.at(edgeBuffers[2]).unread = 0;
    model.buffers.at(edgeBuffers[1]).unread = 1;
    model.buffers.at(edgeBuffers[3]).unread = 2;
    if (convergence) {
        model.activeBuffer = destinationId;
        frame();
        assert(dialogs->settingsOpen() && model.activeBuffer == destinationId);
        assert(destination && visible(*destination) && !visible(*source));
        assert(dispatcher.focusedArea() != destination->inputArea());
        renameBuffer(sourceId, model.buffers.at(destinationId).target);
        frame();
        assert(!model.buffers.contains(sourceId) && model.activeBuffer == destinationId);
        const auto mergedDraft = model.buffers.at(destinationId).draft;
        assert(mergedDraft.find(edgeDrafts[0]) != std::string::npos &&
               mergedDraft.find(edgeDrafts[1]) != std::string::npos);
        assert(inputWithText(ui.root(), mergedDraft) == destination && visible(*destination));
        assert(destination->editingState().history == destinationState->history);
        assert(!visible(*areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[2])));
        assert(visible(*areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[1])));
        assert(visible(*areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[3])));
        ui.execute("settings"); // Dismiss without the cancel command's composer-focus side effect.
        frame();
        assert(!dialogs->active() && dispatcher.focusedArea() == destination->inputArea());
        auto *client = dispatcher.focusedArea()->textInputClient();
        assert(client == static_cast<TextInputClient *>(destination));
        TextInputEdit insertion{};
        insertion.commitText = "!";
        insertion.hasCommitText = true;
        client->textInputApplyEdit(insertion);
        assert(destination->value() != mergedDraft &&
               model.buffers.at(destinationId).draft == destination->value());
        ui.execute("undo");
        assert(destination->value() == mergedDraft);
    } else {
        renameBuffer(sourceId, "RenamedOrigin");
        frame();
        assert(!model.buffers.contains(sourceId) && model.activeBuffer == "test/renamedorigin");
        auto *renamedLink = areaWithTooltip(*paneFor(*source), sourceUrl);
        assert(renamedLink && visible(*renamedLink));
        assert(!visible(*areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[2])));
        assert(visible(*areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[1])));
        assert(visible(*areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[3])));
        ui.execute("settings"); // Exercise Dialogs' saved return focus, not Ui's cancel fallback.
        frame();
        assert(!dialogs->active() && dispatcher.focusedArea() == renamedLink);
        assert(source->editingStateMatches(sourceState) && visible(*source));
        assert(model.buffers.at(model.activeBuffer).draft == edgeDrafts[0]);
    }
    assert(!visible(*areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[2])));
    assert(visible(*areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[1])));
    assert(visible(*areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[3])));
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void deferredAvyRenameFocusCheck(
    blast::Model &model,
    const std::function<void(const std::string &, const std::string &)> &renameBuffer,
    const std::function<void(const std::string &)> &closeBuffer) {
    using namespace blast;
    model = edgeModel();
    const std::string sourceId = edgeBuffers[0];
    constexpr std::string_view url = "https://example.invalid/deferred-avy-return";
    for (const auto *name : {"zulu", "zumba"}) {
        auto &buffer = model.buffers["test/" + std::string(name)];
        buffer.id = "test/" + std::string(name);
        buffer.server = "test";
        buffer.target = name;
        buffer.kind = "query";
        buffer.draft = std::string(name) + " draft";
    }
    Message message;
    message.id = "deferred-avy-focus";
    message.nick = "Peer";
    message.text = "Deferred native focus " + std::string(url);
    message.time = "2026-11-07T12:34:56Z";
    model.buffers.at(sourceId).messages.push_back(std::move(message));
    ++model.buffers.at(sourceId).messagesRevision;
    Clipboard clipboard;
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    const auto update = [&] { ui.update(1600, 1000, 1); };
    WindowActions windowActions{};
    windowActions.synchronizeLayout = [&] {
        update();
        ui.root().layout(renderer);
        return true;
    };
    ui.setWindowActions(std::move(windowActions));
    update();
    ui.root().layout(renderer);
    auto *input = inputWithText(ui.root(), edgeDrafts[0]);
    assert(input && visible(*input) && input->insertText("!"));
    ui.execute("undo");
    const auto editorState = input->editingState();
    auto *link = areaWithTooltip(*paneFor(*input), url);
    assert(link && visible(*link));
    dispatcher.setFocus(link);
    nativeKey(ui, dispatcher, 'x', KeyMod::Ctrl);
    nativeKey(ui, dispatcher, 'j', 0, 'j');
    update();
    ui.root().layout(renderer);
    nativeKey(ui, dispatcher, 'z', 0, 'z');
    update();
    ui.root().layout(renderer);
    auto *jump = areaWithKey(ui.root(), "channel-jump");
    assert(jump && visible(*jump) && dispatcher.focusedArea() == jump);
    assert(visible(*areaWithKey(ui.root(), "buffer:test/zulu")));
    assert(visible(*areaWithKey(ui.root(), "buffer:test/zumba")));
    assert(!visible(*areaWithKey(ui.root(), "buffer:" + sourceId)));

    // One core batch: the return target is renamed, then every partial-hint match closes.
    renameBuffer(sourceId, "RenamedFocus");
    closeBuffer("test/zulu");
    closeBuffer("test/zumba");
    assert(!model.buffers.contains(sourceId) && !model.buffers.contains("test/zulu") &&
           !model.buffers.contains("test/zumba") && model.activeBuffer == "test/renamedfocus");
    update();
    ui.flush();
    update();

    // Dispatch the next real key to the renamed link, whether return was eager or needs layout.
    constexpr std::string_view priorStatus = "Waiting for renamed link activation";
    model.status = priorStatus;
    for (const bool pressed : {true, false}) {
        if (pressed)
            assert(ui.synchronizeInput());
        if (!ui.handleKey({.sym = XKB_KEY_Return, .pressed = pressed}))
            dispatcher.keyEvent(XKB_KEY_Return, 0, 0, pressed);
    }
    assert(model.status != priorStatus);
    update();
    ui.root().layout(renderer);
    auto *renamedLink = areaWithTooltip(*paneFor(*input), url);
    assert(renamedLink && visible(*renamedLink) && dispatcher.focusedArea() == renamedLink);
    ModeLine *line = nullptr;
    for (const auto &child : ui.root().children())
        if (auto *candidate = dynamic_cast<ModeLine *>(child.get()))
            line = candidate;
    assert(line && visible(*line));
    auto *feedback = labelWithText(*line, model.status);
    assert(feedback && visible(*feedback));
    assert(model.activeBuffer == "test/renamedfocus" && input->editingStateMatches(editorState));
    assert(model.buffers.at(model.activeBuffer).draft == edgeDrafts[0]);
    for (std::size_t i = 1; i < edgeBuffers.size(); ++i)
        assert(model.buffers.at(edgeBuffers[i]).draft == edgeDrafts[i]);
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void halfPageChatKeysCheck() {
    using namespace blast;
    auto model = edgeModel();
    model.networks.at("test").state = "Ready";
    model.networks.at("test").caps = {"message-tags"};
    auto &buffer = model.buffers.at(edgeBuffers[0]);
    for (int i = 0; i < 100; ++i) {
        Message message;
        message.id = "half-page-" + std::to_string(i);
        message.nick = "Peer";
        message.text = "Variable wrapped history " + std::string(i % 3 * 80 + 20, 'x');
        message.time = "2026-11-07T12:34:56Z";
        message.timestamp = i + 1;
        buffer.messages.push_back(std::move(message));
    }
    ++buffer.messagesRevision;
    buffer.readMarker = buffer.messages.back().id;
    buffer.typing["Peer"] = model.now + 100;
    Clipboard clipboard;
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    float height = 700;
    const auto frame = [&] {
        ui.update(1200, height, 1);
        ui.root().layout(renderer);
    };
    frame();
    auto *input = inputWithText(ui.root(), edgeDrafts[0]);
    auto *transcript = visibleNode<TranscriptView>(ui.root());
    auto *scroll = transcript ? visibleNode<ScrollView>(*transcript) : nullptr;
    assert(input && transcript && scroll);
    auto state = input->editingState();
    state.current.cursorPos = 8;
    state.current.selectionAnchor = 2;
    input->restoreEditingState(state);
    dispatcher.setFocus(input->inputArea());
    frame();
    state = input->editingState();
    const auto undo = state.history->undo, redo = state.history->redo;
    const auto unchanged = [&] {
        assert(input->editingStateMatches(state));
        assert(state.history->undo == undo && state.history->redo == redo);
        assert(model.buffers.at(edgeBuffers[0]).draft == edgeDrafts[0]);
        assert(dispatcher.focusedArea() == input->inputArea());
    };
    const auto page = [&](std::uint32_t key, int direction) {
        const float before = scroll->scrollOffset(), half = scroll->height() * .5f;
        const float expected =
            std::clamp(before + direction * half, 0.f, scroll->maxScrollOffset());
        hostKey(ui, dispatcher, key);
        assert(std::abs(scroll->scrollOffset() - expected) < 1.f);
        frame();
        assert(std::abs(scroll->scrollOffset() - expected) < 1.f);
        unchanged();
    };
    for (const float viewportHeight : {700.f, 480.f, 850.f}) {
        height = viewportHeight;
        frame();
        // Materialize the finite fixture before comparing absolute offsets across layout.
        // Otherwise measuring newly exposed wrapped rows changes cumulative coordinates
        // while correctly retaining the same visible body anchor.
        for (float offset = 0; offset < scroll->maxScrollOffset();
             offset += std::max(1.f, scroll->height() * .5f)) {
            scroll->setScrollOffset(offset);
            frame();
        }
        assert(std::abs(scroll->height() - transcript->height()) < .5f);
        assert(scroll->maxScrollOffset() > scroll->height() * 2);
        scroll->setScrollOffset(scroll->maxScrollOffset());
        frame();
        page(XKB_KEY_Page_Up, -1);
        page(XKB_KEY_Page_Down, 1);
        page(XKB_KEY_Page_Down, 1); // Already at bottom.
        scroll->setScrollOffset(scroll->height() * .2f);
        frame();
        page(XKB_KEY_Page_Up, -1); // Clamp a partial page at top.
        page(XKB_KEY_Page_Up, -1);
        page(XKB_KEY_Page_Down, 1);
    }
    // Buffer boundaries navigate loaded messages, never the selected native draft.
    assert(model.settings.bindings.at("beginning-of-buffer") == "M-<");
    assert(model.settings.bindings.at("end-of-buffer") == "M->");
    const auto historyActions = [&] {
        return std::count_if(model.actions.begin(), model.actions.end(),
                             [](const Action &action) { return action.type == "history"; });
    };
    const auto historyBefore = historyActions();
    const auto boundary = [&](std::uint32_t sym, bool newest) {
        hostKey(ui, dispatcher, sym, KeyMod::Alt);
        frame();
        assert(std::abs(scroll->scrollOffset() - (newest ? scroll->maxScrollOffset() : 0.f)) < 1.f);
        unchanged();
        assert(historyActions() == historyBefore);
    };
    boundary(XKB_KEY_less, false);
    boundary(XKB_KEY_greater, true);
    // Held navigation is consumed on the same route instead of editing the draft.
    for (int i = 0; i < 2; ++i) {
        assert(ui.handleKey({.sym = XKB_KEY_less, .modifiers = KeyMod::Alt, .pressed = true}));
        frame();
        assert(scroll->scrollOffset() < 1.f);
        unchanged();
    }
    assert(ui.handleKey({.sym = XKB_KEY_less, .modifiers = KeyMod::Alt, .pressed = false}));
    boundary(XKB_KEY_greater, true);
    // Multi-stroke custom bindings and disabled bindings retain their normal semantics.
    model.settings.bindings["beginning-of-buffer"] = "C-x <";
    model.settings.bindings["end-of-buffer"] = "";
    hostKey(ui, dispatcher, 'x', KeyMod::Ctrl);
    hostKey(ui, dispatcher, XKB_KEY_less);
    frame();
    assert(scroll->scrollOffset() < 1.f);
    unchanged();
    hostKey(ui, dispatcher, XKB_KEY_greater, KeyMod::Alt);
    frame();
    assert(scroll->scrollOffset() < 1.f);
    unchanged();
    model.settings.bindings["beginning-of-buffer"] = "M-<";
    model.settings.bindings["end-of-buffer"] = "M->";
    boundary(XKB_KEY_greater, true);
    // Held keys use the same command path, without falling through to draft editing.
    scroll->setScrollOffset(scroll->maxScrollOffset());
    frame();
    const float beforeRepeat = scroll->scrollOffset(), half = scroll->height() * .5f;
    for (int i = 0; i < 2; ++i) {
        assert(ui.handleKey({.sym = XKB_KEY_Page_Up, .pressed = true}));
        assert(std::abs(scroll->scrollOffset() - (beforeRepeat - (i + 1) * half)) < 1.f);
        frame();
    }
    assert(ui.handleKey({.sym = XKB_KEY_Page_Up, .pressed = false}));
    assert(std::abs(scroll->scrollOffset() - (beforeRepeat - 2 * half)) < 1.f);
    unchanged();
    // Existing configurable binding table is authoritative, including an empty binding.
    model.settings.bindings["scroll-up"] = "C-u";
    model.settings.bindings["scroll-down"] = "";
    const float beforeRebound = scroll->scrollOffset();
    hostKey(ui, dispatcher, 'u', KeyMod::Ctrl);
    assert(std::abs(scroll->scrollOffset() - (beforeRebound - half)) < 1.f);
    frame();
    assert(std::abs(scroll->scrollOffset() - (beforeRebound - half)) < 1.f);
    unchanged();
    const auto keyboardArea = [&](auto &&self, Node &node) -> InputArea * {
        if (auto *area = dynamic_cast<InputArea *>(&node);
            area && area->tabFocusKey().starts_with("transcript:") &&
            area->tabFocusKey().ends_with(":keyboard"))
            return area;
        for (const auto &child : node.children())
            if (auto *area = self(self, *child))
                return area;
        return nullptr;
    };
    auto *keyboard = keyboardArea(keyboardArea, *transcript);
    assert(keyboard);
    dispatcher.setFocus(keyboard);
    const float beforeUnbound = scroll->scrollOffset();
    hostKey(ui, dispatcher, XKB_KEY_Page_Up);
    hostKey(ui, dispatcher, XKB_KEY_Page_Down);
    frame();
    assert(scroll->scrollOffset() == beforeUnbound && input->editingStateMatches(state));
    hostKey(ui, dispatcher, 'u', KeyMod::Ctrl);
    assert(std::abs(scroll->scrollOffset() - (beforeUnbound - half)) < 1.f);
    frame();
    assert(std::abs(scroll->scrollOffset() - (beforeUnbound - half)) < 1.f);
    assert(dispatcher.focusedArea() == keyboard && input->editingStateMatches(state));
    hostKey(ui, dispatcher, XKB_KEY_less, KeyMod::Alt);
    frame();
    assert(scroll->scrollOffset() < 1.f && dispatcher.focusedArea() == keyboard &&
           input->editingStateMatches(state));
    hostKey(ui, dispatcher, XKB_KEY_greater, KeyMod::Alt);
    frame();
    assert(std::abs(scroll->scrollOffset() - scroll->maxScrollOffset()) < 1.f &&
           dispatcher.focusedArea() == keyboard && input->editingStateMatches(state));
    // The upload control is intentionally hidden for a nonempty draft. Use a real
    // visible pane control without changing the draft to manufacture a toolbar.
    model.settings.bindings["scroll-up"] = "PRIOR";
    auto *more = headerTool(*input, "More conversation actions");
    assert(more && visible(*more));
    dispatcher.setFocus(more);
    assert(dispatcher.focusedArea() == more);
    const float beforeControl = scroll->scrollOffset();
    hostKey(ui, dispatcher, XKB_KEY_Page_Up);
    hostKey(ui, dispatcher, XKB_KEY_less, KeyMod::Alt);
    hostKey(ui, dispatcher, XKB_KEY_greater, KeyMod::Alt);
    frame();
    assert(dispatcher.focusedArea() == more);
    assert(scroll->scrollOffset() == beforeControl && input->editingStateMatches(state));
    dispatcher.setFocus(input->inputArea());
    ui.execute("settings");
    frame();
    auto *dialogs = visibleNode<Dialogs>(ui.root());
    assert(dialogs && dialogs->settingsOpen());
    hostKey(ui, dispatcher, XKB_KEY_Page_Up);
    hostKey(ui, dispatcher, XKB_KEY_less, KeyMod::Alt);
    hostKey(ui, dispatcher, XKB_KEY_greater, KeyMod::Alt);
    frame();
    assert(scroll->scrollOffset() == beforeControl && input->editingStateMatches(state));
    auto *settingsSearch = areaWithKey(*dialogs, "blastirc.settings.search");
    auto *settingsInput =
        settingsSearch ? dynamic_cast<Input *>(settingsSearch->textInputClient()) : nullptr;
    assert(settingsInput);
    settingsInput->setValue("navigation");
    auto settingsState = settingsInput->editingState();
    settingsState.current.cursorPos = 5;
    settingsState.current.selectionAnchor = 2;
    settingsInput->restoreEditingState(settingsState);
    dispatcher.setFocus(settingsSearch);
    frame();
    settingsState = settingsInput->editingState();
    hostKey(ui, dispatcher, XKB_KEY_less, KeyMod::Alt);
    hostKey(ui, dispatcher, XKB_KEY_greater, KeyMod::Alt);
    frame();
    assert(settingsInput->editingStateMatches(settingsState) &&
           dispatcher.focusedArea() == settingsSearch && scroll->scrollOffset() == beforeControl &&
           input->editingStateMatches(state));
    hostKey(ui, dispatcher, XKB_KEY_Home, KeyMod::Ctrl);
    assert(settingsInput->cursorPosition() == 0 && settingsInput->selectionAnchor() == 0);
    hostKey(ui, dispatcher, XKB_KEY_End, KeyMod::Ctrl);
    assert(settingsInput->cursorPosition() == settingsInput->value().size() &&
           settingsInput->selectionAnchor() == settingsInput->value().size() &&
           scroll->scrollOffset() == beforeControl && input->editingStateMatches(state));
    settingsInput->setValue("");
    model.status = "Cancel this transient feedback";
    hostKey(ui, dispatcher, 'g', KeyMod::Ctrl);
    frame();
    assert(!dialogs->active());
    assert(model.status.empty());
    dispatcher.setFocus(input->inputArea());
    frame();
    assert(inputWithText(ui.root(), edgeDrafts[0]) == input && visible(*input) &&
           dispatcher.focusedArea() == input->inputArea());
    model.settings.bindings["scroll-up"] = "PRIOR";
    model.settings.bindings["scroll-down"] = "NEXT";
    // Active IME composition retains its text and caret instead of running chat commands.
    static_cast<TextInputClient &>(*input).textInputApplyEdit({.commitText = {},
                                                               .preeditText = "compose",
                                                               .preeditCursorBegin = 7,
                                                               .preeditCursorEnd = 7,
                                                               .hasPreedit = true});
    frame();
    assert(input->composing() && dispatcher.focusedArea() == input->inputArea());
    const auto composing = input->editingState();
    const auto composingKey = [&](std::uint32_t sym, std::uint32_t modifiers = 0) {
        hostKey(ui, dispatcher, sym, modifiers);
        frame();
        assert(input->composing() && input->editingStateMatches(composing) &&
               dispatcher.focusedArea() == input->inputArea());
        assert(scroll->scrollOffset() == beforeControl);
    };
    composingKey(XKB_KEY_Page_Up);
    composingKey(XKB_KEY_Page_Down);
    composingKey(XKB_KEY_less, KeyMod::Alt);
    composingKey(XKB_KEY_greater, KeyMod::Alt);
    model.settings.bindings["beginning-of-buffer"] = "C-x <";
    composingKey('x', KeyMod::Ctrl);
    composingKey(XKB_KEY_less);
    model.settings.bindings["beginning-of-buffer"] = "M-<";
    model.settings.bindings["scroll-up"] = "C-u";
    composingKey('u', KeyMod::Ctrl);
    auto *status = visibleNode<ModeLine>(ui.root());
    assert(status);
    assert(composing.preeditBase);
    const auto committedDraft = composing.preeditBase->value;
    const auto compositionUndo = composing.history->undo, compositionRedo = composing.history->redo;
    model.status = "Cancel composition without losing the draft";
    frame();
    assert(visibleLabelContaining(*status, model.status));
    hostKey(ui, dispatcher, 'g', KeyMod::Ctrl);
    frame();
    assert(model.status.empty());
    assert(!visibleLabelContaining(*status, "Cancel composition without losing the draft"));
    assert(visibleLabelContaining(*status, "Me") && visibleLabelContaining(*status, "test"));
    assert(!input->composing() && input->value() == committedDraft);
    assert(input->editingState().history->undo == compositionUndo &&
           input->editingState().history->redo == compositionRedo);
    assert(buffer.draft == edgeDrafts[0] && dispatcher.focusedArea() == input->inputArea());
    // A message reply and selected draft survive both boundaries and a later arrival.
    hostKey(ui, dispatcher, 'p', KeyMod::Ctrl);
    frame();
    hostKey(ui, dispatcher, 'r', KeyMod::Ctrl);
    frame();
    auto *cancelReply = composerControl(*paneFor(*input), ":reply-cancel");
    assert(cancelReply && visible(*cancelReply) && dispatcher.focusedArea() == input->inputArea());
    const auto replyDraft = input->editingState();
    const auto replyUndo = replyDraft.history->undo, replyRedo = replyDraft.history->redo;
    for (const auto sym : {XKB_KEY_less, XKB_KEY_greater}) {
        hostKey(ui, dispatcher, sym, KeyMod::Alt);
        frame();
        assert(input->editingStateMatches(replyDraft) && visible(*cancelReply) &&
               replyDraft.history->undo == replyUndo && replyDraft.history->redo == replyRedo &&
               dispatcher.focusedArea() == input->inputArea());
        assert(std::abs(scroll->scrollOffset() -
                        (sym == XKB_KEY_less ? 0.f : scroll->maxScrollOffset())) < 1.f);
    }
    Message arrival;
    arrival.id = "boundary-follow-arrival";
    arrival.nick = "Peer";
    arrival.text = "Newest message after returning to follow";
    arrival.time = "2026-11-07T12:35:56Z";
    arrival.timestamp = 200;
    buffer.messages.push_back(std::move(arrival));
    ++buffer.messagesRevision;
    frame();
    assert(std::abs(scroll->scrollOffset() - scroll->maxScrollOffset()) < 1.f &&
           input->editingStateMatches(replyDraft) && visible(*cancelReply) &&
           dispatcher.focusedArea() == input->inputArea());
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void hiddenUnreadStatusCheck(
    blast::Model &model,
    const std::function<void(const std::string &, blast::Message)> &appendMessage) {
    using namespace blast;
    model = edgeModel();
    model.settings.workspace["native_navigation_mode"] = "hidden";
    model.servers[0].alias = "Local";
    model.networks["test"].state = "Ready";
    ServerConfig remote;
    remote.name = "other";
    remote.alias = "Remote";
    model.servers.push_back(remote);
    model.networks["other"].state = "Ready";
    model.networks["other"].nick = "OtherMe";
    const auto add = [&](std::string server, std::string target, std::string kind) {
        const auto id = server + "/" + target;
        auto &buffer = model.buffers[id];
        buffer.id = id;
        buffer.server = std::move(server);
        buffer.target = std::move(target);
        buffer.kind = std::move(kind);
        return id;
    };
    const auto normal = add("test", "#same", "channel");
    const auto mention = add("other", "#same", "channel");
    const auto archived = add("test", "#archived", "channel");
    const auto fools = add("test", "#fools", "channel");
    const auto system = add("test", "*server*", "server");
    int sequence = 0;
    const auto incoming = [&](const std::string &id, bool own = false, bool presence = false,
                              bool mentioned = false, std::string nick = "Peer") {
        Message message;
        message.id = "hidden-unread-" + std::to_string(++sequence);
        message.nick = std::move(nick);
        message.text = mentioned ? "Me: a genuine own-nick mention" : "ordinary channel message";
        message.time = "2026-11-07T13:00:00Z";
        message.timestamp = 1794056400;
        message.own = own;
        message.presence = presence;
        message.mention = mentioned;
        const auto key = message.id;
        appendMessage(id, std::move(message));
        return key;
    };
    incoming(normal);
    incoming(normal, true, false, true);
    incoming(normal, false, true, true);
    assert(model.buffers.at(normal).unread == 1);
    const auto oldMention = incoming(mention, false, false, true);
    model.buffers.at(mention).readMarker = "local:" + oldMention;
    model.buffers.at(mention).unread = 0;
    incoming(mention);
    incoming(archived);
    model.buffers.at(archived).archived = true;
    incoming(system);
    incoming(edgeBuffers[1]);
    model.settings.fools.insert("FOOL");
    incoming(fools);
    incoming(fools, false, false, true, "fool");
    assert(model.buffers.at(fools).unread == 1);
    model.buffers.at(normal).muted = true;
    Clipboard clipboard;
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(false);
    float width = 2000;
    const auto frame = [&] {
        ui.update(width, 900, 1);
        ui.root().layout(renderer);
    };
    frame();
    auto *line = visibleNode<ModeLine>(ui.root());
    assert(line && visibleCount<Sidebar>(ui.root()) == 0);
    const auto segment = [&](std::string_view name) { return visibleLabelContaining(*line, name); };
    auto *local = segment("Local/#same");
    auto *other = segment("Remote/#same");
    auto *fool = segment("Local/#fools");
    assert(local && other && fool && local != other);
    assert(local->text().find(std::to_string(model.buffers.at(normal).unread)) !=
           std::string_view::npos);
    assert(other->text().front() != '@' && fool->text().front() != '@');
    assert(!segment("#archived") && !segment("*server*") && !segment("second"));
    const auto normalColor = other->color();
    const auto normalWeight = other->font().weight;
    const auto newestMention = incoming(mention, false, false, true);
    frame();
    other = segment("Remote/#same");
    assert(other && other->text().front() == '@');
    assert(other->color() != normalColor && other->font().weight > normalWeight);
    assert(other->text().find(std::to_string(model.buffers.at(mention).unread)) !=
           std::string_view::npos);
    // No new controls, and stable arrivals retain the native text nodes.
    const auto childCount = line->children().size();
    frame();
    assert(segment("Local/#same") == local && line->children().size() == childCount);
    assert(!areaWithKey(*line, "status-mark-all-read"));
    ui.execute("toggle-navigation");
    frame();
    assert(visibleCount<Sidebar>(ui.root()) == 1 && !segment("Local/#same"));
    ui.execute("toggle-navigation");
    frame();
    auto *sidebar = visibleNode<Sidebar>(ui.root());
    assert(sidebar && sidebar->width() == 56 && !segment("Local/#same"));
    ui.execute("toggle-navigation");
    frame();
    assert(visibleCount<Sidebar>(ui.root()) == 0 && segment("Local/#same"));
    ui.execute("jump-channel");
    frame();
    auto *jump = areaWithKey(ui.root(), "channel-jump");
    sidebar = visibleNode<Sidebar>(ui.root());
    assert(jump && visible(*jump) && sidebar && sidebar->width() == 56);
    assert(!segment("Local/#same") && !segment("Remote/#same"));
    nativeKey(ui, dispatcher, XKB_KEY_Escape);
    frame();
    assert(visibleCount<Sidebar>(ui.root()) == 0 && segment("Remote/#same"));
    // A retained mention at/before the read marker cannot accent newer ordinary unread.
    model.buffers.at(mention).readMarker = "local:" + newestMention;
    model.buffers.at(mention).unread = 0;
    incoming(mention);
    frame();
    other = segment("Remote/#same");
    assert(other && other->text().front() != '@' && other->color() == normalColor);
    model.buffers.at(mention).unread = 0;
    frame();
    assert(!segment("Remote/#same"));
    // Let Core include fools, then verify the same retained mention classifier.
    model.settings.foolUnread = true;
    incoming(fools, false, false, true, "fool");
    frame();
    fool = segment("Local/#fools");
    assert(fool && fool->text().front() == '@' && model.buffers.at(fools).unread == 3);
    // Eligibility changes invalidate style without an unread/message revision.
    model.settings.foolUnread = false;
    frame();
    fool = segment("Local/#fools");
    assert(fool && fool->text().front() != '@' && model.buffers.at(fools).unread == 3);
    model.settings.fools.clear();
    frame();
    fool = segment("Local/#fools");
    assert(fool && fool->text().front() == '@');
    model.settings.fools = {"FOOL", "{FOOL}"};
    incoming(fools, false, false, true, "[fool]");
    frame();
    fool = segment("Local/#fools");
    assert(fool && fool->text().front() != '@');
    model.networks["test"].caseMapping = "ascii";
    frame();
    fool = segment("Local/#fools");
    assert(fool && fool->text().front() == '@');
    ui.execute("toggle-navigation");
    frame();
    auto *foolRow = areaWithKey(ui.root(), "buffer:" + fools);
    assert(foolRow && visible(*foolRow));
    const auto badge = "@" + std::to_string(model.buffers.at(fools).unread);
    assert(labelWithText(*foolRow, badge));
    model.networks["test"].caseMapping = "rfc1459";
    frame();
    assert(!labelWithText(*foolRow, badge));
    model.settings.fools.clear();
    frame();
    assert(labelWithText(*foolRow, badge));
    ui.execute("toggle-navigation");
    ui.execute("toggle-navigation");
    frame();
    model.status = "Connection feedback";
    width = 720;
    frame();
    assert(visibleLabelContaining(*line, model.status));
    assert(visibleLabelContaining(*line, "Me") && visibleLabelContaining(*line, "Connected"));
    for (const auto &child : line->children())
        if (visible(*child))
            assert(child->x() >= 0 && child->x() + child->width() <= line->width() + .5f);
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void focusedUnreadTransitionCheck(
    blast::Model &model,
    const std::function<void(const std::string &, blast::Message)> &appendMessage) {
    using namespace blast;
    model = edgeModel();
    const std::string id = edgeBuffers[0];
    for (int i = 0; i < 40; ++i) {
        Message message;
        message.id = "focus-history-" + std::to_string(i);
        message.nick = "Peer";
        message.text = "Retained conversation history " + std::to_string(i);
        message.time = "2026-11-07T12:34:56Z";
        message.timestamp = i + 1;
        appendMessage(id, std::move(message));
    }
    auto &buffer = model.buffers.at(id);
    buffer.readMarker = buffer.messages.back().id;
    buffer.unread = 0;
    Clipboard clipboard;
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    const auto frame = [&] {
        ui.update(1000, 500, 1);
        ui.root().layout(renderer);
    };
    frame();
    auto *transcript = visibleNode<TranscriptView>(ui.root());
    assert(transcript);
    ScrollView *scroll = nullptr;
    for (const auto &child : transcript->children())
        if (auto *candidate = dynamic_cast<ScrollView *>(child.get()))
            scroll = candidate;
    assert(scroll && scroll->maxScrollOffset() > 0);
    scroll->setScrollOffset(scroll->maxScrollOffset() * .75f);
    frame();
    const auto readRequests = [&] {
        return std::count_if(model.actions.begin(), model.actions.end(), [&](const Action &action) {
            return action.type == "read" && action.buffer == id;
        });
    };
    const auto initialReads = readRequests();
    const auto marker = buffer.readMarker;
    ui.setWindowActive(false);
    Message incoming;
    incoming.id = "focus-away-remote";
    incoming.nick = "Peer";
    incoming.text = "Genuine remote arrival while the window is away";
    incoming.time = "2026-11-07T13:00:00Z";
    incoming.timestamp = 1000;
    appendMessage(id, std::move(incoming));
    frame();
    assert(buffer.unread == 1 && buffer.readMarker == marker);
    assert(readRequests() == initialReads);
    auto *follow = followNewestControl(*transcript);
    assert(follow && visible(*follow));
    // Returning to a focused window hides the divider, not the durable unread data.
    ui.setWindowActive(true);
    frame();
    auto *separator = labelWithText(*transcript, "New since your last visit");
    assert(!separator || !visible(*separator));
    assert(buffer.unread == 1 && buffer.readMarker == marker);
    assert(readRequests() == initialReads);
    assert(scroll->maxScrollOffset() - scroll->scrollOffset() > 1);
    ui.setWindowActive(false);
    scroll->setScrollOffset(scroll->maxScrollOffset());
    frame();
    separator = labelWithText(*transcript, "New since your last visit");
    assert(separator && visible(*separator));
    assert(buffer.unread == 1 && buffer.readMarker == marker);
    assert(readRequests() == initialReads);
    ui.setWindowActive(true);
    model.now += 1;
    frame();
    separator = labelWithText(*transcript, "New since your last visit");
    assert(!separator || !visible(*separator));
    assert(readRequests() == initialReads + 1);
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

enum class RenameArrivalCase { ActiveSourceMerge, ActiveDestinationMerge, StandaloneRename };

void renameArrivalCheck(
    blast::Model &model,
    const std::function<void(const std::string &, const std::string &)> &renameBuffer,
    const std::function<void(const std::string &, blast::Message)> &appendMessage,
    RenameArrivalCase kind) {
    using namespace blast;
    model = edgeModel();
    const bool collision = kind != RenameArrivalCase::StandaloneRename;
    const bool activeSource = kind != RenameArrivalCase::ActiveDestinationMerge;
    const std::string sourceId = edgeBuffers[0];
    const std::string destinationId = collision ? edgeBuffers[1] : "test/unusedarrival";
    for (const bool source : {true, false}) {
        if (!source && !collision)
            continue;
        for (int i = 0; i < 32; ++i) {
            Message message;
            message.id =
                std::string(source ? "source-old-" : "destination-old-") + std::to_string(i);
            message.nick = "Peer";
            message.text =
                std::string(source ? "Source cached history " : "Destination cached history ") +
                std::to_string(i) + "\nRetain this known conversation viewport.";
            message.time = "2026-11-07T12:34:56Z";
            message.timestamp = i * 2 + (source ? 1 : 2);
            message.arrival = model.now - 10;
            appendMessage(source ? sourceId : destinationId, std::move(message));
        }
    }
    Clipboard clipboard;
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    const auto frame = [&] {
        ui.update(1600, 800, 1);
        ui.root().layout(renderer);
    };
    frame();
    if (collision) {
        ui.execute("buffer:" + destinationId);
        frame();
    }
    const std::string activeId = activeSource ? sourceId : destinationId;
    ui.execute("buffer:" + activeId);
    frame();
    auto *input = inputWithText(ui.root(), edgeDrafts[activeSource ? 0 : 1]);
    assert(input && visible(*input));
    TranscriptView *transcript = nullptr;
    for (const auto &child : paneFor(*input)->children())
        if (auto *view = dynamic_cast<TranscriptView *>(child.get()))
            transcript = view;
    assert(transcript && visible(*transcript));
    ScrollView *scroll = nullptr;
    for (const auto &child : transcript->children())
        if (auto *candidate = dynamic_cast<ScrollView *>(child.get()))
            scroll = candidate;
    assert(scroll && scroll->maxScrollOffset() > 0);
    auto *follow = followNewestControl(*transcript);
    assert(follow && !visible(*follow));
    scroll->setScrollOffset(scroll->maxScrollOffset() * .5f);
    frame();
    assert(scroll->scrollOffset() > 0 && scroll->maxScrollOffset() - scroll->scrollOffset() > 1);
    assert(visible(*follow));
    dispatcher.setFocus(input->inputArea());
    static_cast<TextInputClient &>(*input).textInputApplyEdit({.commitText = "",
                                                               .preeditText = "compose",
                                                               .preeditCursorBegin = 7,
                                                               .preeditCursorEnd = 7,
                                                               .hasPreedit = true});
    frame();
    const auto composing = input->editingState();
    click(dispatcher, *follow);
    frame();
    assert(!visible(*follow) && scroll->maxScrollOffset() - scroll->scrollOffset() <= 1);
    assert(dispatcher.focusedArea() == input->inputArea() && input->composing() &&
           input->editingStateMatches(composing));
    static_cast<TextInputClient &>(*input).textInputResetPreedit();
    scroll->setScrollOffset(scroll->maxScrollOffset() * .5f);
    frame();
    assert(visible(*follow));

    Message incoming;
    incoming.id = activeSource ? "premerge-source-live" : "premerge-destination-live";
    incoming.nick = "Peer";
    incoming.text = "Only this live arrival should become unseen.";
    incoming.time = "2026-11-07T13:00:00Z";
    incoming.timestamp = 1000;
    incoming.arrival = model.now;
    assert(collision || !model.buffers.contains(destinationId));
    const std::string target = collision ? model.buffers.at(destinationId).target : "UnusedArrival";
    // Both real core operations run before the UI can observe either changed list.
    appendMessage(activeId, std::move(incoming));
    renameBuffer(sourceId, target);
    frame();
    assert(model.activeBuffer == destinationId && !model.buffers.contains(sourceId));
    assert(visible(*transcript) && visible(*follow));
    Button *followButton = nullptr;
    for (auto *node = static_cast<Node *>(follow); node; node = node->parent())
        if (auto *button = dynamic_cast<Button *>(node)) {
            followButton = button;
            break;
        }
    assert(followButton && followButton->enabled());
    const auto followHint = follow->tooltipContent();
    const auto *captionText = std::get_if<std::string>(&followHint);
    assert(captionText);
    const auto &caption = *captionText;
    const auto digit = caption.find_first_of("0123456789");
    assert(digit != std::string::npos);
    int count = 0;
    const auto [end, error] =
        std::from_chars(caption.data() + digit, caption.data() + caption.size(), count);
    assert(error == std::errc{} && end > caption.data() + digit && count == 1);
    assert(input->value() == model.buffers.at(destinationId).draft &&
           input->value().find(edgeDrafts[0]) != std::string::npos);
    if (collision)
        assert(input->value().find(edgeDrafts[1]) != std::string::npos);
    click(dispatcher, *follow);
    frame();
    assert(!visible(*follow) && scroll->maxScrollOffset() - scroll->scrollOffset() <= 1);
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void pinnedCurrentSendCheck() {
    using namespace blast;
    auto model = edgeModel();
    const std::string origin = edgeBuffers[0], draft = "pinned source draft";
    model.buffers.at(origin).draft = draft;
    ServerConfig foreign;
    foreign.name = "foreign";
    foreign.host = "foreign.invalid";
    model.servers.push_back(std::move(foreign));
    Clipboard clipboard;
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    const auto frame = [&] {
        ui.update(1600, 1000, 1);
        ui.root().layout(renderer);
    };
    frame();
    auto *input = inputWithText(ui.root(), draft);
    auto *current = areaWithKey(ui.root(), "buffer:" + origin);
    auto *unread = areaWithKey(ui.root(), "sidebar-filter:unread");
    auto *foreignRail = areaWithKey(ui.root(), "server:foreign");
    assert(input && current && unread && foreignRail);
    click(dispatcher, *unread);
    frame();
    assert(visible(*current) &&
           !visible(*areaWithKey(ui.root(), std::string("buffer:") + edgeBuffers[1])));
    auto *name = labelWithText(*current, model.buffers.at(origin).target);
    assert(name && visible(*name) && visible(*input) && input->value() == draft);
    assert(std::none_of(model.buffers.begin(), model.buffers.end(),
                        [](const auto &entry) { return entry.second.server == "foreign"; }));
    click(dispatcher, *foreignRail);
    frame();
    assert(model.settings.workspace.at("selected_server") == "foreign");
    assert(model.activeBuffer == origin && visible(*current) && visible(*name));
    auto *send = composerControl(*input->parent(), ":send");
    assert(send && visible(*send));
    dispatcher.setFocus(send);
    const auto firstAction = model.actions.size();
    nativeKey(ui, dispatcher, XKB_KEY_Return);
    frame();
    assert(std::count_if(
               model.actions.begin() + firstAction, model.actions.end(), [&](const Action &action) {
                   return action.type == "send" && action.buffer == origin && action.text == draft;
               }) == 1);
    assert(std::none_of(
        model.actions.begin() + firstAction, model.actions.end(),
        [&](const Action &action) { return action.type == "send" && action.buffer != origin; }));
    ui.execute("undo");
    frame();
    assert(input->value() == draft && model.buffers.at(origin).draft == draft);
    const auto editorState = input->editingState();
    click(dispatcher, *foreignRail);
    frame();
    assert(model.activeBuffer == origin &&
           model.settings.workspace.at("selected_server") == "foreign");
    for (const auto *target : {"zulu", "zumba"}) {
        auto &buffer = model.buffers["foreign/" + std::string(target)];
        buffer.id = "foreign/" + std::string(target);
        buffer.server = "foreign";
        buffer.target = target;
        buffer.kind = "query";
        buffer.draft = std::string(target) + " foreign draft";
    }
    frame();
    auto *zulu = areaWithKey(ui.root(), "buffer:foreign/zulu");
    auto *zumba = areaWithKey(ui.root(), "buffer:foreign/zumba");
    assert(zulu && zumba && !visible(*zulu) && !visible(*zumba) && visible(*current));
    nativeKey(ui, dispatcher, 'x', KeyMod::Ctrl);
    nativeKey(ui, dispatcher, 'j', 0, 'j');
    frame();
    auto *jump = areaWithKey(ui.root(), "channel-jump");
    assert(jump && visible(*jump) && !visible(*current));
    assert(visible(*zulu) && visible(*zumba) && model.activeBuffer == origin);
    nativeKey(ui, dispatcher, XKB_KEY_Escape);
    frame();
    assert(!visible(*jump) && areaWithKey(ui.root(), "buffer:" + origin) == current);
    assert(visible(*current) && visible(*name) && !visible(*zulu) && !visible(*zumba));
    assert(model.activeBuffer == origin && input->editingStateMatches(editorState));
    assert(model.buffers.at(origin).draft == draft &&
           model.buffers.at("foreign/zulu").draft == "zulu foreign draft" &&
           model.buffers.at("foreign/zumba").draft == "zumba foreign draft");
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void minimumPendingPasteBoundsCheck() {
    using namespace blast;
    class PendingClipboard final : public TextClipboard {
      public:
        TextCallback pending;
        std::optional<std::string> clipboardText() override { return std::nullopt; }
        void setClipboardText(std::string) override {}
        void requestClipboardText(TextCallback callback) override { pending = std::move(callback); }
    } clipboard;
    auto model = edgeModel();
    model.settings.scale = 2;
    model.buffers.at(edgeBuffers[0]).draft = "abc";
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    const auto frame = [&] {
        ui.update(720, minimumWindowHeight(renderer), 1);
        ui.root().layout(renderer);
    };
    const auto bounds = [](const Node &node) {
        std::array<float, 4> result{};
        Node::transformedBounds(&node, result[0], result[1], result[2], result[3]);
        return result;
    };
    frame();
    auto *input = inputWithText(ui.root(), "abc");
    assert(input && visible(*input));
    auto *pane = paneFor(*input);
    Node *composer = input;
    while (composer->parent() != pane)
        composer = composer->parent();
    auto *primaryRow = input->parent();
    auto *plus = composerControl(*composer, ":plus");
    auto *send = composerControl(*composer, ":send");
    assert(plus && send && visible(*plus) && visible(*send));
    const float inputHeight = input->height(), nativeHeight = input->inputArea()->height();
    const auto document = input->editingState();
    auto *inputText = labelWithText(*input, "abc");
    assert(inputText && visible(*inputText));
    const float inputFontSize = inputText->fontSize();
    ui.execute("paste");
    assert(clipboard.pending);
    auto deliver = std::move(clipboard.pending);
    deliver(std::string("one\ntwo\nthree\nfour\nfive\nsix"));
    frame();
    auto *insert = composerControl(*composer, ":paste-insert");
    auto *separate = composerControl(*composer, ":paste-send");
    auto *cancel = composerControl(*composer, ":paste-cancel");
    assert(insert && separate && cancel && visible(*insert) && visible(*separate) &&
           visible(*cancel));
    const auto paneBounds = bounds(*pane);
    for (const Node *node :
         std::array<const Node *, 9>{composer, input, input->inputArea(), primaryRow, plus, send,
                                     insert, separate, cancel}) {
        assert(visible(*node));
        const auto assigned = bounds(*node);
        assert(assigned[1] + .5f >= paneBounds[1] && assigned[3] <= paneBounds[3] + .5f);
    }
    for (const auto *area : {input->inputArea(), plus, send, insert, separate, cancel})
        assert(area->width() >= 32 && area->height() >= 32);
    std::uint32_t serial = 30000;
    for (const auto *area : {input->inputArea(), plus, send, insert, separate, cancel}) {
        const auto assigned = bounds(*area);
        auto clipped = assigned;
        for (auto *ancestor = area->parent(); ancestor; ancestor = ancestor->parent())
            if (ancestor->clipChildren()) {
                const auto clip = bounds(*ancestor);
                clipped[0] = std::max(clipped[0], clip[0]);
                clipped[1] = std::max(clipped[1], clip[1]);
                clipped[2] = std::min(clipped[2], clip[2]);
                clipped[3] = std::min(clipped[3], clip[3]);
            }
        assert(clipped[2] - clipped[0] >= 32 && clipped[3] - clipped[1] >= 32);
        assert(clipped[0] <= assigned[0] + .5f && clipped[1] <= assigned[1] + .5f &&
               clipped[2] + .5f >= assigned[2] && clipped[3] + .5f >= assigned[3]);
        if (area != insert && area != separate && area != cancel)
            continue; // Primary Send may intentionally be disabled during confirmation.
        const float x = (clipped[0] + clipped[2]) / 2, y = (clipped[1] + clipped[3]) / 2;
        ui.handlePointerMotion(x, y, false);
        dispatcher.pointerMotion(x, y, ++serial);
        bool hitsControl = false;
        for (auto *hit = static_cast<Node *>(dispatcher.hoveredArea()); hit; hit = hit->parent())
            hitsControl |= hit == area;
        assert(hitsControl);
    }
    assert(std::abs(input->height() - inputHeight) <= .5f &&
           std::abs(input->inputArea()->height() - nativeHeight) <= .5f);
    assert(input->editingStateMatches(document) && model.buffers.at(edgeBuffers[0]).draft == "abc");
    inputText = labelWithText(*input, "abc");
    assert(inputText && visible(*inputText) && inputText->fontSize() == inputFontSize);
    assert(std::none_of(model.actions.begin(), model.actions.end(), [](const Action &action) {
        return action.type == "send" || action.type == "command";
    }));
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

blast::Model messageActionModel() {
    using namespace blast;
    auto model = edgeModel();
    model.buffers.clear();
    auto &buffer = model.buffers["test/#message-actions"];
    buffer.id = model.activeBuffer = "test/#message-actions";
    buffer.server = "test";
    buffer.target = "#message-actions";
    buffer.kind = "channel";
    buffer.joined = true;
    buffer.draft = "keep this draft";
    for (const auto &[id, nick] :
         {std::pair{"message-older", "OlderPeer"}, std::pair{"message-newest", "NewestPeer"}}) {
        Message message{};
        message.id = id;
        message.nick = nick;
        message.text = std::string(nick) + " real conversation message";
        message.time = "2026-11-07T12:34:56Z";
        buffer.messages.push_back(std::move(message));
    }
    Message presence{};
    presence.id = "presence-tail";
    presence.nick = "PresencePeer";
    presence.text = "Presence is not a selectable real message";
    presence.presence = true;
    presence.time = "2026-11-07T12:35:00Z";
    buffer.messages.push_back(std::move(presence));
    ++buffer.messagesRevision;
    model.networks["test"].caps = {"message-tags"};
    model.networks["test"].state = "Ready";
    return model;
}

void messageReactionPickerCheck() {
    using namespace blast;
    auto model = messageActionModel();
    const std::string origin = model.activeBuffer, draft = model.buffers.at(origin).draft;
    auto &buffer = model.buffers.at(origin);
    Clipboard clipboard;
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    MenuHost menu;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    ui.root().setPopupContext(&menu);
    const auto frame = [&] {
        ui.update(1600, 1000, 1);
        ui.root().layout(renderer);
    };
    WindowActions actions{};
    actions.synchronizeLayout = [&] {
        frame();
        return true;
    };
    ui.setWindowActions(std::move(actions));
    frame();
    auto *input = inputWithText(ui.root(), draft);
    auto *transcript = visibleNode<TranscriptView>(ui.root());
    assert(input && transcript && visible(*input));
    auto state = input->editingState();
    state.current.cursorPos = 8;
    state.current.selectionAnchor = 2;
    input->restoreEditingState(state);
    frame();
    const auto document = input->editingState();
    const auto undo = document.history->undo, redo = document.history->redo;
    const auto unchanged = [&] {
        assert(input->editingStateMatches(document) && buffer.draft == draft);
        assert(document.history->undo == undo && document.history->redo == redo);
    };
    const auto composerFocused = [&] {
        assert(dispatcher.focusedArea() == input->inputArea());
        unchanged();
    };
    const auto noMessageActions = [&](std::size_t first) {
        assert(
            std::none_of(model.actions.begin() + first, model.actions.end(), [](const Action &a) {
                return a.type == "react" || a.type == "send" || a.type == "command";
            }));
    };
    const auto feedback = [&] {
        auto *line = visibleNode<ModeLine>(ui.root());
        assert(line);
        auto *label = labelWithText(*line, model.status);
        assert(label && visible(*label));
    };
    const auto selectNewest = [&] {
        composerFocused();
        hostKey(ui, dispatcher, 'p', KeyMod::Ctrl);
        frame();
        auto *area = dispatcher.focusedArea();
        assert(area && !area->textInputClient());
        const auto selected = transcript->selectedMessage();
        assert(selected && selected->buffer == origin && selected->nick == "NewestPeer");
        unchanged();
        return area;
    };
    emoji::Picker *picker = nullptr;
    const auto openPicker = [&] {
        const auto first = model.actions.size();
        hostKey(ui, dispatcher, 'e', KeyMod::Ctrl);
        frame();
        auto *shown = visibleNode<emoji::Picker>(ui.root());
        assert(shown);
        if (picker)
            assert(shown == picker);
        picker = shown;
        auto *query = dispatcher.focusedArea()
                          ? dynamic_cast<Input *>(dispatcher.focusedArea()->textInputClient())
                          : nullptr;
        assert(query && query != input && visible(*query) && query->value().empty());
        noMessageActions(first);
        unchanged();
        return query;
    };
    const auto typeThumbsup = [&](Input &query) {
        TextInputEdit edit{};
        edit.commitText = "thumbsup";
        edit.hasCommitText = true;
        static_cast<TextInputClient &>(query).textInputApplyEdit(edit);
        frame();
    };
    const auto paints = [&](Node &node) {
        std::vector<Color> result;
        const auto collect = [&](auto &&self, const Node &current) -> void {
            if (!visible(current))
                return;
            if (const auto *rect = dynamic_cast<const RectNode *>(&current))
                result.push_back(rect->style().fill);
            for (const auto &child : current.children())
                self(self, *child);
        };
        collect(collect, node);
        return result;
    };
    {
        // Returning from message navigation to a textbox restores C-e's editing intent.
        selectNewest();
        dispatcher.setFocus(input->inputArea());
        assert(model.settings.bindings.at("message-react") == "C-e");
        const std::string firstLine = "first café é 猫🙂";
        const std::string multiline = firstLine + "\nsecond λ line\n";
        const auto nativeEndOfLine = [&](Input &field, bool composer) {
            field.restoreEditingState(Input::EditingState{});
            assert(field.insertText(multiline));
            assert(field.insertText("undo probe"));
            hostKey(ui, dispatcher, 'z', KeyMod::Ctrl);
            assert(field.value() == multiline);
            const auto document = field.editingState();
            const auto savedUndo = document.history->undo, savedRedo = document.history->redo;
            assert(!savedUndo.empty() && !savedRedo.empty());
            for (const auto &[cursor, expected] :
                 {std::pair{std::size_t{6}, firstLine.size()},
                  std::pair{firstLine.size(), firstLine.size()},
                  std::pair{firstLine.size() + 1, multiline.size() - 1},
                  std::pair{multiline.size(), multiline.size()}}) {
                auto before = document;
                before.current.cursorPos = cursor;
                before.current.selectionAnchor = 0;
                field.restoreEditingState(before);
                frame();
                const auto firstAction = model.actions.size();
                const auto status = model.status;
                hostKey(ui, dispatcher, 'e', KeyMod::Ctrl);
                frame();
                const auto after = field.editingState();
                assert(after.current.value == multiline && field.cursorPosition() == expected &&
                       field.selectionAnchor() == (composer ? 0 : expected));
                assert(after.history == document.history && after.history->undo == savedUndo &&
                       after.history->redo == savedRedo);
                assert(dispatcher.focusedArea() == field.inputArea());
                assert(model.actions.size() == firstAction && model.status == status);
                assert(!visibleNode<emoji::Picker>(ui.root()) && !menu.open);
            }
        };
        nativeEndOfLine(*input, true);
        input->restoreEditingState(document);
        frame();
        unchanged();
        // The real attachment affordance is available only for an empty composer.
        input->restoreEditingState(Input::EditingState{});
        frame();
        assert(input->value().empty() && buffer.draft.empty());
        auto *paperclip = composerControl(ui.root(), "upload");
        assert(paperclip && visible(*paperclip));
        click(dispatcher, *paperclip);
        menu.select("Text / paste…");
        frame();
        auto *pasteArea = areaWithKey(ui.root(), "upload-text");
        auto *paste = pasteArea ? dynamic_cast<Input *>(pasteArea->textInputClient()) : nullptr;
        assert(paste && visible(*paste) && dispatcher.focusedArea() == pasteArea);
        nativeEndOfLine(*paste, false);
        auto *cancel = areaWithKey(ui.root(), "upload-close");
        assert(cancel && visible(*cancel));
        click(dispatcher, *cancel);
        frame();
        dispatcher.setFocus(input->inputArea());
        input->restoreEditingState(document);
        frame();
        unchanged();
    }

    {
        auto *modalOrigin = selectNewest();
        const std::string transcriptFocusKey{modalOrigin->tabFocusKey()};
        const auto peopleMode = model.settings.peopleMode;
        for (const bool noFocus : {false, true}) {
            dispatcher.setFocus(noFocus ? nullptr : modalOrigin);
            for (const auto people :
                 {SidebarMode::Expanded, SidebarMode::Compact, SidebarMode::Hidden}) {
                auto *toggle = headerTool(*input, "People");
                assert(visible(*toggle));
                click(dispatcher, *toggle);
                frame();
                assert(model.settings.peopleMode == people);
                presentation(ui, model, origin, people);
                if (noFocus) {
                    assert(!dispatcher.focusedArea());
                } else {
                    assert(dispatcher.focusedArea() &&
                           dispatcher.focusedArea()->tabFocusKey() == transcriptFocusKey);
                    const auto selected = transcript->selectedMessage();
                    assert(selected && selected->buffer == origin &&
                           selected->nick == "NewestPeer");
                }
                unchanged();
            }
            assert(model.settings.peopleMode == peopleMode);
        }
        modalOrigin = areaWithKey(ui.root(), transcriptFocusKey);
        assert(modalOrigin && visible(*modalOrigin));
        dispatcher.setFocus(modalOrigin);
        const auto restoredSelection = transcript->selectedMessage();
        assert(restoredSelection && restoredSelection->buffer == origin &&
               restoredSelection->nick == "NewestPeer");
        for (int index = 0; index < 40; ++index) {
            auto target = "zz-picker-overflow-" + std::to_string(index);
            const auto id = "test/" + target;
            auto &entry = model.buffers[id];
            entry.id = id;
            entry.server = "test";
            entry.target = std::move(target);
            entry.kind = "query";
        }
        frame();
        auto *channel = areaWithKey(ui.root(), "buffer:" + origin);
        assert(channel && visible(*channel));
        ScrollView *viewport = nullptr;
        for (Node *node = channel->parent(); node; node = node->parent())
            if (auto *candidate = dynamic_cast<ScrollView *>(node))
                viewport = candidate;
        assert(viewport && viewport->maxScrollOffset() > 0);
        const auto touchPoint = pointerPoint(*channel);
        std::uint32_t serial = 40000;
        assert(!ui.prepareTouchPress());
        dispatcher.pointerEnter(touchPoint[0], touchPoint[1], ++serial);
        assert(!ui.prepareTouchPress());
        ui.beginPointerInteraction(touchPoint[0], touchPoint[1]);
        assert(!ui.handlePointerButton(BTN_LEFT, true, touchPoint[0], touchPoint[1], true));
        ++serial;
        assert(dispatcher.pointerButton(touchPoint[0], touchPoint[1], BTN_LEFT, true, serial,
                                        serial * 10, true));
        assert(!channel->pressed() && !dispatcher.pointerCaptured());
        const auto first = model.actions.size();
        auto *query = openPicker();
        assert(ui.retiredTouch());
        const auto mousePoint = pointerPoint(*query->inputArea());
        dispatcher.pointerEnter(mousePoint[0], mousePoint[1], ++serial);
        ui.prepareMousePress();
        ui.beginPointerInteraction(mousePoint[0], mousePoint[1]);
        assert(!ui.handlePointerButton(BTN_LEFT, true, mousePoint[0], mousePoint[1], false));
        ++serial;
        assert(dispatcher.pointerButton(mousePoint[0], mousePoint[1], BTN_LEFT, true, serial,
                                        serial * 10, false));
        assert(query->inputArea()->pressed() && dispatcher.pointerCaptured());
        const auto heldQuery = query->editingState();
        const bool motionRetired = ui.retiredTouch();
        if (!motionRetired) {
            ui.handlePointerMotion(touchPoint[0], touchPoint[1] - 48, true);
            dispatcher.pointerMotion(touchPoint[0], touchPoint[1] - 48, ++serial);
        }
        assert(motionRetired);
        ui.discardPointerRelease(BTN_LEFT, true);
        const bool leaveConsumed = ui.consumeTouchLeave();
        if (!leaveConsumed)
            dispatcher.pointerLeave();
        assert(leaveConsumed);
        frame();
        assert(query->inputArea()->pressed() && dispatcher.pointerCaptured());
        assert(dispatcher.focusedArea() == query->inputArea());
        assert(query->editingStateMatches(heldQuery));
        noMessageActions(first);
        unchanged();
        assert(!ui.handlePointerButton(BTN_LEFT, false, mousePoint[0], mousePoint[1], false));
        ++serial;
        assert(dispatcher.pointerButton(mousePoint[0], mousePoint[1], BTN_LEFT, false, serial,
                                        serial * 10, false));
        frame();
        assert(!dispatcher.pointerCaptured() && visible(*picker));
        noMessageActions(first);
        unchanged();
        hostKey(ui, dispatcher, XKB_KEY_Escape);
        frame();
        assert(!visible(*picker) && dispatcher.focusedArea() == modalOrigin);
        hostKey(ui, dispatcher, 'g', KeyMod::Ctrl);
        frame();
        composerFocused();
    }

    auto *reactionOrigin = selectNewest();
    const auto firstReaction = model.actions.size();
    auto *query = openPicker();
    auto *results = visibleNode<emoji::CompletionView>(*picker);
    assert(results);
    const auto initialPaint = paints(*results);
    hostKey(ui, dispatcher, 'n', KeyMod::Ctrl);
    frame();
    assert(paints(*results) != initialPaint && dispatcher.focusedArea() == query->inputArea());
    hostKey(ui, dispatcher, 'p', KeyMod::Ctrl);
    frame();
    assert(paints(*results) == initialPaint && query->value().empty());
    typeThumbsup(*query);
    hostKey(ui, dispatcher, XKB_KEY_Return);
    frame();
    assert(!visible(*picker));
    assert(std::count_if(model.actions.begin() + firstReaction, model.actions.end(),
                         [&](const Action &a) {
                             return a.type == "react" && a.buffer == origin &&
                                    a.extra == "message-newest" && a.text == "👍";
                         }) == 1);
    assert(dispatcher.focusedArea() == reactionOrigin && !reactionOrigin->textInputClient());
    unchanged();
    hostKey(ui, dispatcher, 'p', KeyMod::Ctrl);
    frame();
    const auto previous = transcript->selectedMessage();
    assert(previous && previous->nick == "OlderPeer" && dispatcher.focusedArea() == reactionOrigin);
    hostKey(ui, dispatcher, 'n', KeyMod::Ctrl);
    frame();
    const auto resumed = transcript->selectedMessage();
    assert(resumed && resumed->nick == "NewestPeer" && dispatcher.focusedArea() == reactionOrigin);
    unchanged();
    hostKey(ui, dispatcher, 'g', KeyMod::Ctrl);
    frame();
    composerFocused();

    for (const bool controlCancel : {false, true}) {
        auto *selectedArea = selectNewest();
        query = openPicker();
        const auto first = model.actions.size();
        TextInputEdit preedit{};
        preedit.preeditText = "a";
        preedit.preeditCursorBegin = preedit.preeditCursorEnd = 1;
        preedit.hasPreedit = true;
        static_cast<TextInputClient &>(*query).textInputApplyEdit(preedit);
        frame();
        const auto composing = query->editingState();
        assert(query->composing());
        auto *preeditResults = visibleNode<emoji::CompletionView>(*picker);
        assert(preeditResults);
        const auto preeditPaint = paints(*preeditResults);
        for (const auto sym : {XKB_KEY_n, XKB_KEY_p, XKB_KEY_Return}) {
            hostKey(ui, dispatcher, sym, sym == XKB_KEY_Return ? 0 : KeyMod::Ctrl);
            frame();
            assert(visible(*picker) && query->editingStateMatches(composing) &&
                   paints(*preeditResults) == preeditPaint);
            noMessageActions(first);
            unchanged();
        }
        if (controlCancel)
            hostKey(ui, dispatcher, 'g', KeyMod::Ctrl);
        else
            hostKey(ui, dispatcher, XKB_KEY_Escape);
        frame();
        assert(visible(*picker) && !query->composing() && query->value().empty() &&
               dispatcher.focusedArea() == query->inputArea());
        if (controlCancel)
            hostKey(ui, dispatcher, 'g', KeyMod::Ctrl);
        else
            hostKey(ui, dispatcher, XKB_KEY_Escape);
        frame();
        assert(!visible(*picker) && dispatcher.focusedArea() == selectedArea &&
               !selectedArea->textInputClient());
        unchanged();
        noMessageActions(first);
        hostKey(ui, dispatcher, 'g', KeyMod::Ctrl);
        frame();
        composerFocused();
    }

    auto *selectedArea = selectNewest();
    const auto beforeContext = model.actions.size();
    hostKey(ui, dispatcher, XKB_KEY_Menu);
    assert(menu.open);
    menu.select("Choose emoji…");
    frame();
    assert(visibleNode<emoji::Picker>(ui.root()) == picker);
    query = dispatcher.focusedArea()
                ? dynamic_cast<Input *>(dispatcher.focusedArea()->textInputClient())
                : nullptr;
    assert(query && query != input);
    noMessageActions(beforeContext);
    auto *send = composerControl(*input->parent(), ":send");
    assert(send && visible(*send));
    const auto outside = pointerPoint(*send);
    assert(!query->parent()->containsScenePoint(outside[0], outside[1]));
    std::uint32_t serial = 40000;
    for (const bool pressed : {true, false}) {
        ++serial;
        if (pressed)
            ui.beginPointerInteraction(outside[0], outside[1]);
        if (!ui.handlePointerButton(BTN_LEFT, pressed, outside[0], outside[1], false))
            dispatcher.pointerButton(outside[0], outside[1], BTN_LEFT, pressed, serial, serial * 10,
                                     false);
    }
    frame();
    assert(!visible(*picker) && dispatcher.focusedArea() == selectedArea);
    noMessageActions(beforeContext);
    unchanged();
    hostKey(ui, dispatcher, 'g', KeyMod::Ctrl);
    frame();
    composerFocused();

    selectNewest();
    const auto contextTarget = transcript->selectedMessage();
    assert(contextTarget && contextTarget->anchor);
    const auto contextBounds = *contextTarget->anchor;
    hostKey(ui, dispatcher, 'g', KeyMod::Ctrl);
    frame();
    composerFocused();
    const auto firstContextReaction = model.actions.size();
    const std::array<float, 2> contextPoint{contextBounds.x + contextBounds.width / 2,
                                            contextBounds.y + contextBounds.height / 2};
    for (const bool pressed : {true, false}) {
        ++serial;
        if (pressed)
            ui.beginPointerInteraction(contextPoint[0], contextPoint[1]);
        if (!ui.handlePointerButton(BTN_RIGHT, pressed, contextPoint[0], contextPoint[1], false))
            dispatcher.pointerButton(contextPoint[0], contextPoint[1], BTN_RIGHT, pressed, serial,
                                     serial * 10, false);
    }
    frame();
    assert(menu.open);
    composerFocused();
    menu.select("Choose emoji…");
    frame();
    assert(visibleNode<emoji::Picker>(ui.root()) == picker);
    query = dispatcher.focusedArea()
                ? dynamic_cast<Input *>(dispatcher.focusedArea()->textInputClient())
                : nullptr;
    assert(query && query != input);
    noMessageActions(firstContextReaction);
    typeThumbsup(*query);
    hostKey(ui, dispatcher, XKB_KEY_Return);
    frame();
    assert(!visible(*picker));
    assert(std::count_if(model.actions.begin() + firstContextReaction, model.actions.end(),
                         [&](const Action &a) {
                             return a.type == "react" && a.buffer == origin &&
                                    a.extra == "message-newest" && a.text == "👍";
                         }) == 1);
    composerFocused();

    auto *capabilityOrigin = selectNewest();
    query = openPicker();
    typeThumbsup(*query);
    model.networks["test"].caps.clear();
    const auto beforeCapabilityChange = model.actions.size();
    const std::string capabilityStatus = "Waiting for capability revalidation";
    model.status = capabilityStatus;
    hostKey(ui, dispatcher, XKB_KEY_Return);
    frame();
    assert(!visible(*picker) && model.status != capabilityStatus);
    noMessageActions(beforeCapabilityChange);
    feedback();
    assert(dispatcher.focusedArea() == capabilityOrigin && !capabilityOrigin->textInputClient());
    unchanged();
    const std::string unsupportedStatus = "Waiting for unsupported reaction feedback";
    model.status = unsupportedStatus;
    hostKey(ui, dispatcher, 'e', KeyMod::Ctrl);
    frame();
    assert(!visible(*picker) && model.status != unsupportedStatus);
    assert(dispatcher.focusedArea() == capabilityOrigin);
    noMessageActions(beforeCapabilityChange);
    feedback();
    unchanged();
    hostKey(ui, dispatcher, 'g', KeyMod::Ctrl);
    frame();
    composerFocused();
    model.networks["test"].caps = {"message-tags"};
    frame();

    auto *removalOrigin = selectNewest();
    query = openPicker();
    typeThumbsup(*query);
    std::erase_if(buffer.messages,
                  [](const Message &message) { return message.id == "message-newest"; });
    ++buffer.messagesRevision;
    const auto beforeRemoval = model.actions.size();
    const std::string removalStatus = "Waiting for source-message revalidation";
    model.status = removalStatus;
    hostKey(ui, dispatcher, XKB_KEY_Return);
    frame();
    assert(!visible(*picker) && model.status != removalStatus);
    noMessageActions(beforeRemoval);
    feedback();
    assert(dispatcher.focusedArea() == removalOrigin && !removalOrigin->textInputClient());
    unchanged();
    hostKey(ui, dispatcher, 'g', KeyMod::Ctrl);
    frame();
    composerFocused();

    TextInputEdit preedit{};
    preedit.preeditText = "猫";
    preedit.preeditCursorBegin = preedit.preeditCursorEnd = 3;
    preedit.hasPreedit = true;
    static_cast<TextInputClient &>(*input).textInputApplyEdit(preedit);
    frame();
    const auto composing = input->editingState();
    assert(input->composing());
    const auto beforeComposerIme = model.actions.size();
    for (const auto sym : {'p', 'n', 'e', 'r'}) {
        const std::string prior = "Waiting for composition-safe message command";
        model.status = prior;
        hostKey(ui, dispatcher, sym, KeyMod::Ctrl);
        frame();
        assert(input->composing() && input->editingStateMatches(composing) &&
               dispatcher.focusedArea() == input->inputArea() && !visible(*picker));
        assert(model.status != prior);
        feedback();
        noMessageActions(beforeComposerIme);
    }
    static_cast<TextInputClient &>(*input).textInputResetPreedit();
    frame();
    composerFocused();
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void messageReplyCheck() {
    using namespace blast;
    for (const bool tagged : {true, false}) {
        auto model = messageActionModel();
        // Keep the body marker separate from nick-token styling: native text is fragmented.
        model.buffers.at(model.activeBuffer).messages[1].text =
            "ReplyTailBody real conversation message";
        // Overflow from the first frame: reply must cover real tail text, not an empty history.
        auto &replyMessages = model.buffers.at(model.activeBuffer).messages;
        for (int i = 0; i < 48; ++i) {
            Message message{};
            message.id = "reply-baseline-" + std::to_string(i);
            message.nick = "BaselinePeer";
            message.text = "Reply baseline history " + std::to_string(i);
            message.time = "2026-11-07T12:30:00Z";
            replyMessages.insert(replyMessages.begin(), std::move(message));
        }
        ++model.buffers.at(model.activeBuffer).messagesRevision;
        const std::string origin = model.activeBuffer, draft = model.buffers.at(origin).draft;
        const std::string otherId = "test/#reply-other";
        auto &other = model.buffers[otherId];
        other.id = otherId;
        other.server = "test";
        other.target = "#reply-other";
        other.kind = "channel";
        other.joined = true;
        other.draft = "unrelated draft";
        if (!tagged)
            model.networks["test"].caps.clear();
        Clipboard clipboard;
        Theme theme;
        theme.update(model);
        InputDispatcher dispatcher;
        PangoRenderer renderer;
        Ui ui(model, clipboard);
        dispatcher.setSceneRoot(&ui.root());
        ui.setInputDispatcher(dispatcher);
        ui.setWindowActive(true);
        float windowWidth = 1600;
        const auto frame = [&] {
            ui.update(windowWidth, 1000, 1);
            ui.root().layout(renderer);
            ui.root().layout(renderer);
        };
        WindowActions actions{};
        actions.synchronizeLayout = [&] {
            frame();
            return true;
        };
        ui.setWindowActions(std::move(actions));
        frame();
        auto *input = inputWithText(ui.root(), draft);
        assert(input && visible(*input));
        dispatcher.setFocus(input->inputArea());
        assert(input->insertText("!"));
        hostKey(ui, dispatcher, XKB_KEY_BackSpace);
        assert(input->insertText("?"));
        hostKey(ui, dispatcher, 'z', KeyMod::Ctrl);
        frame();
        assert(input->value() == draft && !input->editingState().history->undo.empty() &&
               !input->editingState().history->redo.empty());
        auto state = input->editingState();
        state.current.cursorPos = 8;
        state.current.selectionAnchor = 2;
        input->restoreEditingState(state);
        frame();
        const auto before = input->editingState();
        hostKey(ui, dispatcher, 'p', KeyMod::Ctrl);
        frame();
        auto *selected = dispatcher.focusedArea();
        assert(selected && !selected->textInputClient());
        auto *history = visibleNode<TranscriptView>(*paneFor(*input));
        auto *historyScroll = history ? visibleNode<ScrollView>(*history) : nullptr;
        assert(history && historyScroll);
        const auto originalTarget = history->selectedMessage();
        assert(originalTarget && originalTarget->nick == "NewestPeer");
        const auto bounds = [](const Node &node) {
            std::array<float, 4> result{};
            Node::transformedBounds(&node, result[0], result[1], result[2], result[3]);
            return result;
        };
        const auto latestBounds = [&] {
            TextNode *found = nullptr;
            const auto visit = [&](auto &&self, Node &node) -> void {
                if (!visible(node))
                    return;
                if (auto *text = dynamic_cast<TextNode *>(&node);
                    text && text->text().contains("ReplyTailBody"))
                    found = text;
                for (const auto &child : node.children())
                    self(self, *child);
            };
            visit(visit, *history);
            assert(found);
            const auto measured = renderer.measureText(
                found->text(), found->fontSize(), found->font(), found->maxWidth(),
                found->maxLines(), found->textAlign(), found->ellipsize(), found->useMarkup());
            const auto rect = bounds(*found);
            assert(measured.inkRight > measured.inkLeft && measured.inkBottom > measured.inkTop);
            return std::array<float, 4>{rect[0] + measured.inkLeft, rect[1] + measured.inkTop,
                                        rect[0] + measured.inkRight, rect[1] + measured.inkBottom};
        };
        Node *composer = input;
        auto *pane = paneFor(*input);
        while (composer->parent() != pane)
            composer = composer->parent();
        const auto composerBounds = bounds(*composer), inputBounds = bounds(*input);
        const auto latestBeforeReply = latestBounds();
        const float contentViewportHeight = historyScroll->contentViewportHeight();
        const float historyHeight = history->height(), viewportHeight = historyScroll->height();
        const float historyOffset = historyScroll->scrollOffset();
        const float historyMaximum = historyScroll->maxScrollOffset();
        const auto stableHistory = [&] {
            assert(std::abs(history->height() - historyHeight) < .5f &&
                   std::abs(historyScroll->height() - viewportHeight) < .5f &&
                   std::abs(viewportHeight - historyHeight) < .5f &&
                   std::abs(historyScroll->scrollOffset() - historyOffset) < .5f &&
                   std::abs(historyScroll->maxScrollOffset() - historyMaximum) < .5f &&
                   std::abs(historyScroll->contentViewportHeight() - contentViewportHeight) < .5f);
            const auto latest = latestBounds(), currentComposer = bounds(*composer),
                       currentInput = bounds(*input);
            for (std::size_t i = 0; i < latest.size(); ++i) {
                assert(std::abs(latest[i] - latestBeforeReply[i]) < .5f);
                assert(std::abs(currentComposer[i] - composerBounds[i]) < .5f);
                assert(std::abs(currentInput[i] - inputBounds[i]) < .5f);
            }
        };
        hostKey(ui, dispatcher, 'r', KeyMod::Ctrl);
        frame();
        assert(dispatcher.focusedArea() == input->inputArea());
        auto *cancelReply = composerControl(*pane, ":reply-cancel");
        assert(cancelReply);
        stableHistory();
        const auto beforeUndo = before.history->undo, beforeRedo = before.history->redo;
        assert(visible(*cancelReply) && input->editingStateMatches(before) &&
               before.history->undo == beforeUndo && before.history->redo == beforeRedo);
        const auto &buffer = model.buffers.at(origin);
        const auto source =
            std::find_if(buffer.messages.begin(), buffer.messages.end(),
                         [](const Message &message) { return message.id == "message-newest"; });
        assert(source != buffer.messages.end());
        // Locate the shared native card by its content, not its private child ordering.
        Node *card = cancelReply;
        while (card && !labelWithText(*card, source->text))
            card = card->parent();
        assert(card && card != composer && visible(*card));
        auto *excerpt = labelWithText(*card, source->text);
        auto *heading = visibleLabelContaining(*card, ">" + source->nick + "</span>");
        assert(heading && excerpt && heading != excerpt && visible(*excerpt));
        auto *headingText = visibleNode<TextNode>(*heading);
        auto *excerptText = visibleNode<TextNode>(*excerpt);
        assert(headingText && excerptText && headingText->useMarkup() && excerptText->useMarkup());
        const auto authorEnd = heading->text().find(">" + source->nick + "</span>");
        const auto authorStart = heading->text().rfind("<span", authorEnd);
        assert(authorStart != std::string::npos);
        const auto authorStyle = heading->text().substr(authorStart, authorEnd - authorStart);
        assert(authorStyle.contains("weight=\"bold\"") &&
               authorStyle.contains("foreground=\"" +
                                    formatRgbHex(nickColor(model, source->nick, buffer.server)) +
                                    "\""));
        const auto within = [&](const Node &node, const Node &owner) {
            const auto child = bounds(node), parent = bounds(owner);
            assert(child[2] > child[0] && child[3] > child[1]);
            assert(child[0] + .5f >= parent[0] && child[1] + .5f >= parent[1] &&
                   child[2] <= parent[2] + .5f && child[3] <= parent[3] + .5f);
        };
        within(*card, *pane);
        within(*card, *history);
        assert(bounds(*card)[3] <= bounds(*composer)[1] + .5f);
        const auto latestUnderCard = latestBounds(), activeCard = bounds(*card);
        assert(latestUnderCard[0] < activeCard[2] && latestUnderCard[2] > activeCard[0] &&
               latestUnderCard[1] < activeCard[3] && latestUnderCard[3] > activeCard[1]);
        for (const Node *node : std::array<const Node *, 3>{heading, excerpt, cancelReply})
            within(*node, *card);
        const auto headingBounds = bounds(*heading), excerptBounds = bounds(*excerpt),
                   cancelBounds = bounds(*cancelReply);
        assert(headingBounds[3] <= excerptBounds[1] + .5f &&
               headingBounds[2] <= cancelBounds[0] + .5f &&
               excerptBounds[2] <= cancelBounds[0] + .5f);
        assert(headingText->maxLines() == 1 && excerptText->maxLines() == 1);
        // Exercise the real Pane consumer, including its zero-bounds overlay host.
        // The generic root pass after arrange used to replace the constrained Flex width.
        const Message originalSource = *source;
        auto &longSource =
            model.buffers.at(origin)
                .messages[static_cast<std::size_t>(source - buffer.messages.begin())];
        longSource.nick = "作者";
        longSource.text = "\x02ONEexcerpt";
        for (int i = 0; i < 256; ++i) {
            longSource.nick += "長い猫😀";
            longSource.text += "長い猫😀";
        }
        longSource.text += "\x02\x1d終\x1d";
        const Message rawSource = longSource;
        ++model.buffers.at(origin).messagesRevision;
        for (const float scale : {1.f, 1.25f, 1.f}) {
            model.settings.scale = scale;
            theme.update(model);
            for (const float width : {1600.f, 900.f, 1600.f}) {
                windowWidth = width;
                frame();
                assert(card->parent() && card->parent()->width() == 0 &&
                       card->parent()->height() == 0);
                for (int pass = 0; pass < 3; ++pass) {
                    ui.root().layout(renderer);
                    within(*card, *pane);
                    within(*card, *history);
                    within(*cancelReply, *card);
                    within(*cancelReply, *history);
                    within(*cancelReply, *pane);
                    const auto cardBounds = bounds(*card), hit = bounds(*cancelReply);
                    const auto historyBounds = bounds(*history);
                    assert(cardBounds[0] + .5f >= historyBounds[0] &&
                           cardBounds[2] <= historyBounds[2] + .5f &&
                           cardBounds[3] <= bounds(*composer)[1] + .5f);
                    assert(dispatcher.inputAreaAt((hit[0] + hit[2]) / 2, (hit[1] + hit[3]) / 2) ==
                           cancelReply);
                    RectNode *outline = nullptr;
                    const auto findOutline = [&](auto &&self, Node &node) -> void {
                        if (!node.visible())
                            return;
                        if (auto *rect = dynamic_cast<RectNode *>(&node);
                            rect && rect->style().borderWidth > 0 && rect->style().border.a > 0 &&
                            std::abs(rect->width() - card->width()) < .5f &&
                            std::abs(rect->height() - card->height()) < .5f)
                            outline = rect;
                        for (const auto &child : node.children())
                            self(self, *child);
                    };
                    findOutline(findOutline, *card);
                    assert(outline);
                    within(*outline, *history);
                    within(*outline, *pane);
                    int lines = 0;
                    for (auto *text : {headingText, excerptText}) {
                        within(*text->parent(), *card);
                        const auto textBounds = bounds(*text);
                        const auto measured = renderer.measureText(
                            text->text(), text->fontSize(), text->font(), text->maxWidth(),
                            text->maxLines(), text->textAlign(), text->ellipsize(),
                            text->useMarkup());
                        const auto natural = renderer.measureText(
                            text->text(), text->fontSize(), text->font(), 0, 1, text->textAlign(),
                            text->ellipsize(), text->useMarkup());
                        lines += measured.lineCount;
                        assert(text->ellipsize() == TextEllipsize::End && text->maxLines() == 1 &&
                               text->maxWidth() > 0 &&
                               text->maxWidth() <= hit[0] - textBounds[0] + .5f &&
                               textBounds[2] <= hit[0] + .5f && natural.width > text->maxWidth() &&
                               measured.width <= text->maxWidth() + .5f &&
                               textBounds[0] + measured.inkLeft >= cardBounds[0] - .5f &&
                               textBounds[0] + measured.inkRight <= hit[0] + .5f);
                    }
                    assert(lines == 2 && bounds(*heading)[3] <= bounds(*excerpt)[1] + .5f &&
                           excerptText->text().contains("ONEexcerpt") &&
                           excerptText->text().contains("weight=\"bold\"") &&
                           excerptText->text().contains("style=\"italic\"") &&
                           input->editingState().current == before.current &&
                           before.history->undo == beforeUndo &&
                           before.history->redo == beforeRedo &&
                           model.buffers.at(origin).draft == draft &&
                           longSource.nick == rawSource.nick && longSource.text == rawSource.text);
                }
            }
        }
        longSource = originalSource;
        ++model.buffers.at(origin).messagesRevision;
        windowWidth = 1600;
        frame();
        stableHistory();
        // Cancel removes only the local reply selection, never draft text or native undo.
        click(dispatcher, *cancelReply);
        frame();
        stableHistory();
        const auto hiddenCardBounds = bounds(*card);
        const float emptyX = (hiddenCardBounds[0] + hiddenCardBounds[2]) * .5f;
        const float emptyY = (hiddenCardBounds[1] + hiddenCardBounds[3]) * .5f;
        assert(dispatcher.inputAreaAt(emptyX, emptyY) != cancelReply);
        assert(!visible(*cancelReply) && input->editingStateMatches(before) &&
               before.history->undo == beforeUndo && before.history->redo == beforeRedo);
        hostKey(ui, dispatcher, 'p', KeyMod::Ctrl);
        frame();
        hostKey(ui, dispatcher, 'r', KeyMod::Ctrl);
        frame();
        assert(visible(*cancelReply) && input->editingStateMatches(before));
        stableHistory();
        // Typing appears, expires and is disabled without reallocating the reply/history zone.
        for (const int phase : {0, 1, 2, 3, 0}) {
            auto &live = model.buffers.at(origin);
            live.typing.clear();
            model.settings.typingReceive = phase != 3;
            if (phase != 0)
                live.typing["OverlayPeer"] = model.now - (phase == 2 ? 30 : 0);
            frame();
            stableHistory();
            assert(visible(*cancelReply) && input->editingStateMatches(before) &&
                   before.history->undo == beforeUndo && before.history->redo == beforeRedo);
        }
        // Materialize and scroll real history while the hosted reply remains visible.
        // Keep the existing newest reply/source and the earlier layout/toggle checks intact.
        std::vector<Message> olderMessages;
        olderMessages.reserve(160);
        for (int i = 0; i < 160; ++i) {
            Message message{};
            message.id = "reply-history-" + std::to_string(i);
            message.nick = "HistoryPeer";
            message.text = "Earlier reply history " + std::to_string(i);
            message.time = "2026-11-07T12:30:00Z";
            olderMessages.push_back(std::move(message));
        }
        auto &live = model.buffers.at(origin);
        live.messages.insert(live.messages.begin(), std::make_move_iterator(olderMessages.begin()),
                             std::make_move_iterator(olderMessages.end()));
        ++live.messagesRevision;
        frame();
        assert(historyScroll->maxScrollOffset() > historyScroll->height() * 2);
        assert(visible(*cancelReply) && dispatcher.focusedArea() == input->inputArea());
        for (const bool following : {true, false}) {
            historyScroll->setScrollOffset(historyScroll->maxScrollOffset() -
                                           (following ? 0.f : 24.f));
            frame();
            const float offset = historyScroll->scrollOffset(),
                        maximum = historyScroll->maxScrollOffset(),
                        viewport = historyScroll->contentViewportHeight();
            const auto latest = latestBounds(), editor = bounds(*input),
                       composerRect = bounds(*composer);
            for (const bool replying : {false, true, false, true}) {
                if (replying)
                    assert(history->replyToMessage(*originalTarget));
                else
                    click(dispatcher, *cancelReply);
                frame();
                assert(visible(*cancelReply) == replying &&
                       std::abs(historyScroll->scrollOffset() - offset) < .5f &&
                       std::abs(historyScroll->maxScrollOffset() - maximum) < .5f &&
                       std::abs(historyScroll->height() - viewportHeight) < .5f &&
                       std::abs(historyScroll->contentViewportHeight() - viewport) < .5f &&
                       input->editingStateMatches(before) && live.draft == draft);
                const auto current = latestBounds(), currentEditor = bounds(*input),
                           currentComposer = bounds(*composer);
                for (std::size_t i = 0; i < latest.size(); ++i) {
                    assert(std::abs(current[i] - latest[i]) < .5f);
                    assert(std::abs(currentEditor[i] - editor[i]) < .5f);
                    assert(std::abs(currentComposer[i] - composerRect[i]) < .5f);
                }
                if (replying && following) {
                    const auto band = bounds(*card);
                    assert(current[0] < band[2] && current[2] > band[0] && current[1] < band[3] &&
                           current[3] > band[1]);
                }
            }
        }
        const float beforeHistoryScroll = historyScroll->scrollOffset();
        for (int i = 0; i < 4; ++i) {
            hostKey(ui, dispatcher, XKB_KEY_Page_Up);
            frame();
        }
        assert(historyScroll->scrollOffset() < beforeHistoryScroll - historyScroll->height() &&
               historyScroll->maxScrollOffset() - historyScroll->scrollOffset() >
                   historyScroll->height() &&
               visible(*cancelReply) && dispatcher.focusedArea() == input->inputArea());
        const float scrolledOffset = historyScroll->scrollOffset();
        const float scrolledMaximum = historyScroll->maxScrollOffset();
        const auto replyTarget = history->resolveMessage(*originalTarget);
        assert(replyTarget && replyTarget->nick == "NewestPeer" && !replyTarget->anchor);
        const auto scrolledEditor = input->editingState();
        const auto unchangedScrolledDraft = [&] {
            assert(input->editingStateMatches(before) && live.draft == draft &&
                   before.history->undo == beforeUndo && before.history->redo == beforeRedo);
            const auto current = input->editingState();
            assert(current.scrollX == scrolledEditor.scrollX &&
                   current.scrollY == scrolledEditor.scrollY);
            assert(std::abs(historyScroll->scrollOffset() - scrolledOffset) < .5f &&
                   std::abs(historyScroll->maxScrollOffset() - scrolledMaximum) < .5f &&
                   std::abs(historyScroll->height() - viewportHeight) < .5f);
        };
        const auto beforeKeyboardCancel = model.actions.size();
        const auto noOutgoing = [&] {
            assert(std::none_of(model.actions.begin() + beforeKeyboardCancel, model.actions.end(),
                                [](const Action &action) {
                                    return action.type == "send" || action.type == "react" ||
                                           action.type == "command" || action.type == "upload";
                                }));
        };
        // Match the native host's actual Shift+Tab keysym; never focus Cancel directly.
        hostKey(ui, dispatcher, XKB_KEY_ISO_Left_Tab, KeyMod::Shift);
        assert(dispatcher.focusedArea() == cancelReply && visible(*cancelReply));
        frame();
        assert(dispatcher.focusedArea() == cancelReply && visible(*cancelReply));
        unchangedScrolledDraft();
        noOutgoing();
        // Shift is released: a real forward Tab returns to this exact retained editor.
        hostKey(ui, dispatcher, XKB_KEY_Tab, 0);
        frame();
        assert(dispatcher.focusedArea() == input->inputArea() && visible(*cancelReply));
        unchangedScrolledDraft();
        noOutgoing();
        hostKey(ui, dispatcher, XKB_KEY_ISO_Left_Tab, KeyMod::Shift);
        frame();
        assert(dispatcher.focusedArea() == cancelReply && visible(*cancelReply));
        unchangedScrolledDraft();
        noOutgoing();
        hostKey(ui, dispatcher, XKB_KEY_Return);
        frame();
        assert(!visible(*cancelReply) && dispatcher.focusedArea() == input->inputArea() &&
               model.activeBuffer == origin);
        unchangedScrolledDraft();
        noOutgoing();
        // Re-enter the original reply through the native message commands for send/privacy checks.
        hostKey(ui, dispatcher, 'p', KeyMod::Ctrl);
        frame();
        hostKey(ui, dispatcher, 'r', KeyMod::Ctrl);
        frame();
        assert(visible(*cancelReply) && dispatcher.focusedArea() == input->inputArea() &&
               input->editingStateMatches(before));
        noOutgoing();
        // The quote survives a live capability loss; send routing is resolved downstream.
        model.networks["test"].caps.clear();
        frame();
        assert(visible(*cancelReply) && input->editingStateMatches(before));
        const auto firstAction = model.actions.size();
        hostKey(ui, dispatcher, XKB_KEY_Return);
        frame();
        assert(std::count_if(model.actions.begin() + firstAction, model.actions.end(),
                             [&](const Action &action) {
                                 return action.type == "send" && action.buffer == origin &&
                                        action.text == draft && action.extra == "message-newest" &&
                                        action.attributes.contains("reply-nick") &&
                                        action.attributes.at("reply-nick") == "NewestPeer";
                             }) == 1);
        assert(input->value().empty() && model.buffers.at(origin).draft.empty());
        // Native undo recovers raw text, not a nick-prefixed transport fallback.
        hostKey(ui, dispatcher, 'z', KeyMod::Ctrl);
        frame();
        assert(input->value() == draft);
        hostKey(ui, dispatcher, 'p', KeyMod::Ctrl);
        frame();
        hostKey(ui, dispatcher, 'r', KeyMod::Ctrl);
        frame();
        assert(visible(*cancelReply));
        const auto savedPane =
            std::find_if(model.settings.workspace.begin(), model.settings.workspace.end(),
                         [&](const auto &entry) {
                             return entry.second == origin && !entry.first.empty() &&
                                    std::all_of(entry.first.begin(), entry.first.end(),
                                                [](char c) { return c >= '0' && c <= '9'; });
                         });
        assert(savedPane != model.settings.workspace.end());
        savedPane->second = otherId;
        frame();
        assert(model.activeBuffer == otherId && input->value() == other.draft &&
               !visible(*cancelReply) && model.buffers.at(origin).draft == draft);
        dispatcher.setFocus(nullptr);
        dispatcher.setSceneRoot(nullptr);
    }
}

void messageLinkPickerSnapshotCheck() {
    using namespace blast;
    auto model = messageActionModel();
    const std::string origin = model.activeBuffer, draft = model.buffers.at(origin).draft;
    constexpr std::string_view url = "https://example.invalid/reaction-snapshot-link";
    auto &buffer = model.buffers.at(origin);
    auto message = std::find_if(buffer.messages.begin(), buffer.messages.end(),
                                [](const Message &entry) { return entry.id == "message-newest"; });
    assert(message != buffer.messages.end());
    message->text += " " + std::string(url);
    ++buffer.messagesRevision;
    Clipboard clipboard;
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    const auto frame = [&] {
        ui.update(1600, 1000, 1);
        ui.root().layout(renderer);
    };
    WindowActions actions{};
    actions.synchronizeLayout = [&] {
        frame();
        return true;
    };
    ui.setWindowActions(std::move(actions));
    frame();
    auto *input = inputWithText(ui.root(), draft);
    assert(input && visible(*input));
    auto state = input->editingState();
    state.current.cursorPos = 8;
    state.current.selectionAnchor = 2;
    input->restoreEditingState(state);
    frame();
    const auto document = input->editingState();
    const auto undo = document.history->undo, redo = document.history->redo;
    const auto unchanged = [&] {
        assert(input->editingStateMatches(document) && buffer.draft == draft);
        assert(document.history->undo == undo && document.history->redo == redo);
    };
    hostKey(ui, dispatcher, 'p', KeyMod::Ctrl);
    frame();
    auto *link = areaWithTooltip(*paneFor(*input), url);
    assert(link && visible(*link));
    for (int step = 0; step < 128 && dispatcher.focusedArea() != link; ++step) {
        hostKey(ui, dispatcher, XKB_KEY_Tab);
        frame();
    }
    assert(dispatcher.focusedArea() == link);
    unchanged();
    const auto firstReaction = model.actions.size();
    hostKey(ui, dispatcher, 'e', KeyMod::Ctrl);
    frame();
    auto *picker = visibleNode<emoji::Picker>(ui.root());
    auto *query = dispatcher.focusedArea()
                      ? dynamic_cast<Input *>(dispatcher.focusedArea()->textInputClient())
                      : nullptr;
    assert(picker && query && query != input);
    TextInputEdit edit{};
    edit.commitText = "thumbsup";
    edit.hasCommitText = true;
    static_cast<TextInputClient &>(*query).textInputApplyEdit(edit);
    hostKey(ui, dispatcher, XKB_KEY_Return);
    frame();
    auto *restoredLink = areaWithTooltip(*paneFor(*input), url);
    assert(!visible(*picker) && restoredLink && visible(*restoredLink) &&
           dispatcher.focusedArea() == restoredLink);
    assert(std::count_if(model.actions.begin() + firstReaction, model.actions.end(),
                         [&](const Action &a) {
                             return a.type == "react" && a.buffer == origin &&
                                    a.extra == "message-newest" && a.text == "👍";
                         }) == 1);
    unchanged();

    // A real reaction echo rebuilds the message's native controls after restoration.
    message->reactions["👍"] = {"Me"};
    frame();
    auto *reaction = areaWithTooltip(*paneFor(*input), model.networks.at("test").nick);
    assert(reaction && visible(*reaction));
    restoredLink = areaWithTooltip(*paneFor(*input), url);
    assert(restoredLink && visible(*restoredLink) && dispatcher.focusedArea() == restoredLink);
    unchanged();
    const auto beforeCancel = model.actions.size();
    hostKey(ui, dispatcher, 'e', KeyMod::Ctrl);
    frame();
    assert(visibleNode<emoji::Picker>(ui.root()) == picker);
    hostKey(ui, dispatcher, XKB_KEY_Escape);
    frame();
    restoredLink = areaWithTooltip(*paneFor(*input), url);
    assert(!visible(*picker) && restoredLink && dispatcher.focusedArea() == restoredLink);
    assert(std::none_of(model.actions.begin() + beforeCancel, model.actions.end(),
                        [](const Action &a) {
                            return a.type == "react" || a.type == "send" || a.type == "command";
                        }));
    unchanged();
    hostKey(ui, dispatcher, 'g', KeyMod::Ctrl);
    frame();
    assert(dispatcher.focusedArea() == input->inputArea());
    unchanged();
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void messagingSourceControlsCheck() {
    using namespace blast;
    auto model = edgeModel();
    const std::string origin = edgeBuffers[0];
    model.settings.foolDisplay = FoolDisplay::Blurred;
    Message retained;
    retained.id = "retained-source-message";
    retained.nick = "Peer";
    retained.text = "Me: retained mention";
    retained.mention = true;
    model.buffers.at(origin).messages.push_back(retained);
    Clipboard clipboard;
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    const auto frame = [&] {
        ui.update(1600, 1000, 1);
        ui.root().layout(renderer);
    };
    frame();
    ui.execute("buffer-command:" + origin + "\nshow-fools");
    frame();
    assert(effectiveFoolDisplay(model.settings, origin, model.buffers.at(origin).target) ==
           FoolDisplay::Shown);
    ui.execute("buffer:" + std::string(edgeBuffers[1]));
    frame();
    const auto active = model.activeBuffer;
    ui.execute("buffer-command:" + origin + "\nhide-fools");
    frame();
    assert(model.activeBuffer == active && !model.settings.bufferFools.at(origin));
    assert(model.settings.foolDisplay == FoolDisplay::Blurred);
    ui.execute("buffer:" + origin);
    frame();
    auto *input = inputWithText(ui.root(), model.buffers.at(origin).draft);
    assert(input);
    dispatcher.setFocus(input->inputArea());
    input->textInputApplyEdit({.commitText = {},
                               .preeditText = "猫",
                               .preeditCursorBegin = 3,
                               .preeditCursorEnd = 3,
                               .hasPreedit = true});
    frame();
    const auto composing = input->editingState();
    // Core's already-validated same-source jump must not replace native editing state.
    auto &message = model.buffers.at(origin).messages.front();
    model.pendingSourceJump = {.revision = 1, .buffer = origin, .message = message.id};
    frame();
    assert(input->composing() && input->editingStateMatches(composing));
    assert(dispatcher.focusedArea() == input->inputArea());
    input->textInputResetPreedit();
    auto &feed = model.buffers[GlobalMentionsBufferId];
    feed.id = GlobalMentionsBufferId;
    feed.kind = "mentions";
    feed.target = "Mentions";
    feed.draft = "Never send this synthetic draft";
    ui.execute("global-mentions");
    frame();
    assert(model.activeBuffer == GlobalMentionsBufferId);
    assert(!visible(*input));
    const auto before = model.actions.size();
    ui.execute("send");
    frame();
    assert(std::none_of(model.actions.begin() + before, model.actions.end(),
                        [](const Action &action) { return action.type == "send"; }));
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void conversationHistoryNativeCheck() {
    using namespace blast;
    auto model = edgeModel();
    auto &channel = model.buffers.at(edgeBuffers[0]);
    channel.kind = "channel";
    channel.joined = true;
    channel.nicks = {{.name = "HistoryPeer", .account = {}, .prefix = ""}};
    model.networks["test"].caps = {"message-tags"};
    for (int i = 0; i < 80; ++i) {
        Message message;
        message.id = "history-" + std::to_string(i);
        message.nick = "HistoryPeer";
        message.text = "Retain this conversation's scroll and reply target " + std::to_string(i);
        message.time = "2026-11-07T12:34:56Z";
        channel.messages.push_back(std::move(message));
    }
    Clipboard clipboard;
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    const auto frame = [&] {
        ui.update(1600, 1000, 1);
        ui.root().layout(renderer);
    };
    frame();
    std::array<Input *, 4> inputs{};
    std::array<Input::EditingState, 4> states;
    std::array<Input::EditHistory, 4> histories;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        ui.execute("buffer:" + std::string(edgeBuffers[i]));
        frame();
        inputs[i] = inputWithText(ui.root(), edgeDrafts[i]);
        assert(inputs[i] && visible(*inputs[i]));
        dispatcher.setFocus(inputs[i]->inputArea());
        // Deletion separates typing groups; undo the final insertion while retaining
        // the earlier insertion/deletion as independently undoable native edits.
        hostKey(ui, dispatcher, '!', KeyMod::Shift, '!');
        hostKey(ui, dispatcher, XKB_KEY_BackSpace);
        hostKey(ui, dispatcher, '?', KeyMod::Shift, '?');
        assert(inputs[i]->value() != edgeDrafts[i]);
        hostKey(ui, dispatcher, 'z', KeyMod::Ctrl);
        assert(inputs[i]->value() == edgeDrafts[i]);
        auto state = inputs[i]->editingState();
        state.current.cursorPos = 3;
        state.current.selectionAnchor = 1;
        inputs[i]->restoreEditingState(state);
        frame();
        states[i] = inputs[i]->editingState();
        histories[i] = *states[i].history;
        assert(!histories[i].undo.empty() && !histories[i].redo.empty());
    }
    const auto at = [&](std::size_t index) {
        assert(model.activeBuffer == edgeBuffers[index]);
        assert(inputWithText(ui.root(), edgeDrafts[index]) == inputs[index]);
        assert(visible(*inputs[index]) && dispatcher.focusedArea() == inputs[index]->inputArea());
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            assert(inputs[i]->editingStateMatches(states[i]));
            assert(states[i].history->undo == histories[i].undo &&
                   states[i].history->redo == histories[i].redo);
            assert(model.buffers.at(edgeBuffers[i]).draft == edgeDrafts[i]);
        }
    };
    const auto travel = [&](bool forward, std::size_t index) {
        hostKey(ui, dispatcher, forward ? XKB_KEY_i : XKB_KEY_o, KeyMod::Ctrl);
        frame();
        at(index);
    };
    at(3);
    // Repainting, reselecting the same row and shutdown persistence add no visits.
    for (int i = 0; i < 3; ++i) {
        ui.execute("buffer:" + std::string(edgeBuffers[3]));
        frame();
        ui.flush();
    }
    travel(false, 2);
    travel(false, 1);
    travel(false, 0);
    travel(false, 0);
    hostKey(ui, dispatcher, 'p', KeyMod::Ctrl);
    frame();
    auto *transcript = visibleNode<TranscriptView>(*paneFor(*inputs[0]));
    auto *scroll = transcript ? visibleNode<ScrollView>(*transcript) : nullptr;
    assert(transcript && scroll && transcript->selectedMessage());
    hostKey(ui, dispatcher, 'r', KeyMod::Ctrl);
    frame();
    auto *reply = composerControl(*paneFor(*inputs[0]), ":reply-cancel");
    assert(reply && visible(*reply));
    scroll->setScrollOffset(scroll->maxScrollOffset() / 2);
    frame();
    const auto offset = scroll->scrollOffset();
    assert(scroll->maxScrollOffset() > 0);
    travel(true, 1);
    travel(false, 0);
    assert(visible(*reply) && visibleNode<TranscriptView>(*paneFor(*inputs[0])) == transcript);
    assert(std::abs(scroll->scrollOffset() - offset) < .5f);
    travel(true, 1);
    // Selecting a new destination after Back replaces, rather than appends to, Forward.
    click(dispatcher, *areaWithKey(ui.root(), "buffer:" + std::string(edgeBuffers[3])));
    assert(model.activeBuffer == edgeBuffers[1]);
    ui.flush();
    frame();
    at(3);
    travel(true, 3);
    travel(false, 1);
    travel(false, 0);
    travel(true, 1);
    travel(true, 3);
    // Tab remains completion, not the physical Ctrl+i history chord.
    ui.execute("buffer:" + std::string(edgeBuffers[0]));
    frame();
    assert(inputs[0]->replaceRange(0, inputs[0]->value().size(), "HistoryP"));
    dispatcher.setFocus(inputs[0]->inputArea());
    hostKey(ui, dispatcher, XKB_KEY_Tab);
    frame();
    // Literal nickname completion opens its candidates before Tab accepts one.
    assert(model.activeBuffer == edgeBuffers[0] && inputs[0]->value() == "HistoryP" &&
           dispatcher.focusedArea() == inputs[0]->inputArea());
    hostKey(ui, dispatcher, XKB_KEY_Tab);
    frame();
    assert(model.activeBuffer == edgeBuffers[0] && inputs[0]->value().find("HistoryPeer") == 0);
    hostKey(ui, dispatcher, XKB_KEY_o, KeyMod::Ctrl);
    frame();
    assert(model.activeBuffer == edgeBuffers[3]);
    hostKey(ui, dispatcher, XKB_KEY_i, KeyMod::Ctrl);
    frame();
    assert(model.activeBuffer == edgeBuffers[0] && visible(*inputs[0]));
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void conversationHistorySelectionCheck(bool flushAdmission) {
    using namespace blast;
    const std::string configHome = std::getenv("XDG_CONFIG_HOME");
    const std::string dataHome = std::getenv("XDG_DATA_HOME");
    const auto isolated = std::filesystem::path(configHome) / "conversation-history";
    assert(setenv("XDG_CONFIG_HOME", isolated.c_str(), 1) == 0);
    assert(setenv("XDG_DATA_HOME", isolated.c_str(), 1) == 0);
    {
        Application app;
        app.model = edgeModel();
        auto &model = app.model;
        for (auto &server : model.servers)
            server.sojuEnabled = false;
        ServerConfig remote;
        remote.name = "history-remote";
        remote.host = "example.invalid";
        remote.sojuEnabled = false;
        model.servers.push_back(remote);
        auto &remoteBuffer = model.buffers["history-remote/Peer"];
        remoteBuffer.id = "history-remote/Peer";
        remoteBuffer.server = remote.name;
        remoteBuffer.target = "Peer";
        remoteBuffer.kind = "query";
        remoteBuffer.draft = "remote retained draft";
        model.settings.workspace["last_buffer:" + remote.name] = remoteBuffer.id;
        auto &utility = model.buffers["test/history-search"];
        utility.id = "test/history-search";
        utility.server = "test";
        utility.kind = "search";
        utility.target = "history-search";
        Clipboard clipboard;
        Theme theme;
        theme.update(model);
        InputDispatcher dispatcher;
        PangoRenderer renderer;
        Ui ui(model, clipboard);
        dispatcher.setSceneRoot(&ui.root());
        ui.setInputDispatcher(dispatcher);
        ui.setWindowActive(true);
        const auto frame = [&] {
            ui.update(1600, 1000, 1);
            ui.root().layout(renderer);
        };
        frame();
        auto *origin = inputWithText(ui.root(), edgeDrafts[0]);
        assert(origin);
        const auto state = origin->editingState();
        // Choose the actual painted Avy hint, not a copy of its key assignment.
        hostKey(ui, dispatcher, 'x', KeyMod::Ctrl);
        hostKey(ui, dispatcher, 'j', 0, 'j');
        frame();
        auto *jump = areaWithKey(ui.root(), "channel-jump");
        auto *sidebar = visibleNode<Sidebar>(ui.root());
        assert(jump && visible(*jump) && sidebar);
        std::string keys;
        for (const auto &hint : sidebar->hintRects())
            if (hint.buffer == edgeBuffers[1]) {
                auto *label = paintedHintAt(*jump, hint);
                assert(label);
                keys = label->text();
            }
        assert(!keys.empty());
        for (const char key : keys)
            hostKey(ui, dispatcher, key, 0, key);
        frame();
        assert(model.activeBuffer == edgeBuffers[1]);
        // Switch-buffer's real palette selects an existing conversation.
        hostKey(ui, dispatcher, 'x', KeyMod::Ctrl);
        hostKey(ui, dispatcher, 'b', 0, 'b');
        frame();
        auto *query = dispatcher.focusedArea()
                          ? dynamic_cast<Input *>(dispatcher.focusedArea()->textInputClient())
                          : nullptr;
        assert(query && query != origin && visible(*query));
        for (const char key : std::string_view("third"))
            hostKey(ui, dispatcher, key, 0, key);
        frame();
        hostKey(ui, dispatcher, XKB_KEY_Return);
        frame();
        assert(model.activeBuffer == edgeBuffers[2]);
        ui.execute("buffer:" + utility.id);
        frame();
        assert(model.activeBuffer == utility.id);
        hostKey(ui, dispatcher, 'o', KeyMod::Ctrl);
        frame();
        assert(model.activeBuffer == edgeBuffers[2]);
        hostKey(ui, dispatcher, 'o', KeyMod::Ctrl);
        frame();
        assert(model.activeBuffer == edgeBuffers[1]);
        hostKey(ui, dispatcher, 'i', KeyMod::Ctrl);
        frame();
        assert(model.activeBuffer == edgeBuffers[2]);
        // A queued native row click must not overwrite a newer Core selection.
        auto *queuedRow = areaWithKey(ui.root(), "buffer:" + std::string(edgeBuffers[3]));
        assert(queuedRow && visible(*queuedRow));
        click(dispatcher, *queuedRow);
        assert(model.activeBuffer == edgeBuffers[2]);
        // The real Core action changes activeBuffer before Ui sees it.
        model.actions.push_back({"query", edgeBuffers[2], "HistoryCorePeer", ""});
        app.tick(1);
        // Core casefolds stable IDs while preserving the displayed query target.
        const std::string coreBuffer = "test/historycorepeer";
        assert(model.activeBuffer == coreBuffer);
        assert(model.buffers.at(coreBuffer).target == "HistoryCorePeer" &&
               model.buffers.at(coreBuffer).kind == "query");
        if (flushAdmission)
            ui.flush();
        else
            frame();
        assert(model.activeBuffer == coreBuffer);
        frame();
        ui.flush();
        hostKey(ui, dispatcher, 'o', KeyMod::Ctrl);
        frame();
        assert(model.activeBuffer == edgeBuffers[3]);
        auto *queuedInput = inputWithText(ui.root(), edgeDrafts[3]);
        assert(queuedInput && visible(*queuedInput) &&
               dispatcher.focusedArea() == queuedInput->inputArea());
        hostKey(ui, dispatcher, 'o', KeyMod::Ctrl);
        frame();
        assert(model.activeBuffer == edgeBuffers[2]);
        hostKey(ui, dispatcher, 'i', KeyMod::Ctrl);
        frame();
        assert(model.activeBuffer == edgeBuffers[3]);
        hostKey(ui, dispatcher, 'i', KeyMod::Ctrl);
        frame();
        assert(model.activeBuffer == coreBuffer);
        hostKey(ui, dispatcher, 'i', KeyMod::Ctrl);
        frame();
        assert(model.activeBuffer == coreBuffer);
        hostKey(ui, dispatcher, 'o', KeyMod::Ctrl);
        frame();
        assert(model.activeBuffer == edgeBuffers[3]);
        hostKey(ui, dispatcher, 'o', KeyMod::Ctrl);
        frame();
        hostKey(ui, dispatcher, 'o', KeyMod::Ctrl);
        frame();
        hostKey(ui, dispatcher, 'o', KeyMod::Ctrl);
        frame();
        assert(model.activeBuffer == edgeBuffers[0] && visible(*origin) &&
               origin->editingStateMatches(state));
        auto *rail = areaWithKey(ui.root(), "server:history-remote");
        assert(rail && visible(*rail));
        click(dispatcher, *rail);
        frame();
        auto *remoteInput = inputWithText(ui.root(), remoteBuffer.draft);
        assert(model.activeBuffer == remoteBuffer.id && remoteInput && visible(*remoteInput));
        hostKey(ui, dispatcher, 'o', KeyMod::Ctrl);
        frame();
        assert(model.activeBuffer == edgeBuffers[0] &&
               dispatcher.focusedArea() == origin->inputArea() &&
               origin->editingStateMatches(state));
        hostKey(ui, dispatcher, 'i', KeyMod::Ctrl);
        frame();
        assert(model.activeBuffer == remoteBuffer.id && visible(*remoteInput) &&
               dispatcher.focusedArea() == remoteInput->inputArea());
        dispatcher.setFocus(nullptr);
        dispatcher.setSceneRoot(nullptr);
    }
    assert(setenv("XDG_CONFIG_HOME", configHome.c_str(), 1) == 0);
    assert(setenv("XDG_DATA_HOME", dataHome.c_str(), 1) == 0);
}

void conversationHistoryOwnershipCheck() {
    using namespace blast;
    auto model = edgeModel();
    Clipboard clipboard;
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    PangoRenderer renderer;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    const auto frame = [&] {
        ui.update(1600, 1000, 1);
        ui.root().layout(renderer);
    };
    frame();
    ui.execute("buffer:" + std::string(edgeBuffers[1]));
    frame();
    auto *input = inputWithText(ui.root(), edgeDrafts[1]);
    assert(input);
    dispatcher.setFocus(input->inputArea());
    static_cast<TextInputClient &>(*input).textInputApplyEdit({.commitText = {},
                                                               .preeditText = "猫",
                                                               .preeditCursorBegin = 3,
                                                               .preeditCursorEnd = 3,
                                                               .hasPreedit = true});
    const auto composing = input->editingState();
    for (const auto key : {XKB_KEY_o, XKB_KEY_i})
        hostKey(ui, dispatcher, key, KeyMod::Ctrl);
    frame();
    assert(model.activeBuffer == edgeBuffers[1] && input->composing() &&
           input->editingStateMatches(composing) && dispatcher.focusedArea() == input->inputArea());
    static_cast<TextInputClient &>(*input).textInputResetPreedit();
    const auto state = input->editingState();
    for (const auto command : {"search", "settings"}) {
        ui.execute(command);
        frame();
        auto *dialogs = visibleNode<Dialogs>(ui.root());
        assert(dialogs && dialogs->active());
        auto *focus = dispatcher.focusedArea();
        for (const auto key : {XKB_KEY_o, XKB_KEY_i})
            hostKey(ui, dispatcher, key, KeyMod::Ctrl);
        frame();
        assert(dialogs->active() && model.activeBuffer == edgeBuffers[1] &&
               dispatcher.focusedArea() == focus && input->editingStateMatches(state));
        if (std::string_view(command) == "settings") {
            auto *keyboard = areaWithKey(*dialogs, "blastirc.settings.category.1");
            assert(keyboard);
            click(dispatcher, *keyboard);
            frame();
            auto *record = areaWithKey(*dialogs, "blastirc.settings.record.complete");
            assert(record && visible(*record));
            const auto originalComplete = model.settings.bindings.at("complete");
            click(dispatcher, *record);
            frame();
            assert(record->capturesAllKeys());
            hostKey(ui, dispatcher, XKB_KEY_o, KeyMod::Ctrl);
            hostKey(ui, dispatcher, XKB_KEY_i, KeyMod::Ctrl);
            assert(record->capturesAllKeys() &&
                   model.settings.bindings.at("complete") == originalComplete);
            hostKey(ui, dispatcher, XKB_KEY_Return, KeyMod::Ctrl);
            frame();
            assert(model.settings.bindings.at("complete") == "c-o c-i" &&
                   !record->capturesAllKeys());
            assert(model.activeBuffer == edgeBuffers[1] && input->editingStateMatches(state));
            ui.execute("settings");
        } else
            hostKey(ui, dispatcher, XKB_KEY_Escape);
        frame();
    }
    hostKey(ui, dispatcher, XKB_KEY_o, KeyMod::Ctrl);
    frame();
    assert(model.activeBuffer == edgeBuffers[0]);
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void conversationHistoryMissingBindingsCheck() {
    using namespace blast;
    // Constructor migration must respect exact and longer-prefix owners in either direction.
    for (const auto chord : {"C-o", "c-O x", "C-i", "c-I x"}) {
        auto model = edgeModel();
        model.settings.bindings.erase("conversation-back");
        model.settings.bindings.erase("conversation-forward");
        model.settings.bindings["settings"] = chord;
        Clipboard clipboard;
        Theme theme;
        theme.update(model);
        InputDispatcher dispatcher;
        PangoRenderer renderer;
        Ui ui(model, clipboard);
        dispatcher.setSceneRoot(&ui.root());
        ui.setInputDispatcher(dispatcher);
        ui.setWindowActive(true);
        const auto frame = [&] {
            ui.update(1600, 1000, 1);
            ui.root().layout(renderer);
        };
        frame();
        ui.execute("buffer:" + std::string(edgeBuffers[1]));
        frame();
        const bool back = std::string_view(chord).find_first_of("oO") != std::string_view::npos;
        assert(model.settings.bindings.at(back ? "conversation-back" : "conversation-forward")
                   .empty());
        assert(model.settings.bindings.at(back ? "conversation-forward" : "conversation-back") ==
               (back ? "C-i" : "C-o"));
        assert(model.settings.bindings.at("settings") == chord);
        hostKey(ui, dispatcher, back ? XKB_KEY_o : XKB_KEY_i, KeyMod::Ctrl);
        if (std::string_view(chord).ends_with(" x"))
            hostKey(ui, dispatcher, 'x', 0, 'x');
        frame();
        auto *dialogs = visibleNode<Dialogs>(ui.root());
        assert(dialogs && dialogs->settingsOpen() && model.activeBuffer == edgeBuffers[1]);
        dispatcher.setFocus(nullptr);
        dispatcher.setSceneRoot(nullptr);
    }
    for (const bool back : {false, true}) {
        const auto present = back ? "conversation-forward" : "conversation-back";
        const auto missing = back ? "conversation-back" : "conversation-forward";
        const auto prefix = back ? "C-o x" : "C-i x";
        for (const auto custom : {"", "C-x h", prefix}) {
            auto model = edgeModel();
            model.settings.bindings[present] = custom;
            model.settings.bindings.erase(missing);
            model.settings.bindings["settings"] = prefix;
            Clipboard clipboard;
            Ui ui(model, clipboard);
            assert(model.settings.bindings.at(present) == custom);
            assert(model.settings.bindings.at(missing).empty());
        }
        // The customized new command alone can own the missing default's prefix.
        auto model = edgeModel();
        model.settings.bindings[present] = prefix;
        model.settings.bindings.erase(missing);
        Clipboard clipboard;
        Ui ui(model, clipboard);
        assert(model.settings.bindings.at(present) == prefix);
        assert(model.settings.bindings.at(missing).empty());
    }
}

} // namespace

void runModalFocusRemapChecks(
    blast::Model &model,
    const std::function<void(const std::string &, const std::string &)> &renameBuffer,
    const std::function<void(const std::string &, blast::Message)> &appendMessage) {
    codecs::installImageDecoder();
    focusedUnreadTransitionCheck(model, appendMessage);
    hiddenUnreadStatusCheck(model, appendMessage);
    modalFocusRemapCheck(model, renameBuffer, false);
    modalFocusRemapCheck(model, renameBuffer, true);
    renameArrivalCheck(model, renameBuffer, appendMessage, RenameArrivalCase::ActiveSourceMerge);
    renameArrivalCheck(model, renameBuffer, appendMessage,
                       RenameArrivalCase::ActiveDestinationMerge);
    renameArrivalCheck(model, renameBuffer, appendMessage, RenameArrivalCase::StandaloneRename);
}

void runTranscriptLinkFocusCheck(
    blast::Model &model,
    const std::function<void(const std::string &, const std::string &)> &renameBuffer,
    const std::function<void(const std::string &)> &closeBuffer) {
    codecs::installImageDecoder();
    transcriptActionFocusCheck(true);
    deferredAvyRenameFocusCheck(model, renameBuffer, closeBuffer);
    messageLinkPickerSnapshotCheck();
}

void runUiIntegrationCheck() {
    codecs::installImageDecoder();
    conversationHistoryNativeCheck();
    conversationHistorySelectionCheck(false);
    conversationHistorySelectionCheck(true);
    conversationHistoryOwnershipCheck();
    conversationHistoryMissingBindingsCheck();
    minimumWindowHeightCheck();
    sidebarConversationFilterCheck();
    sidebarArchiveControlsCheck();
    sidebarActivityPinsCheck();
    peopleFriendOrderingCheck();
    activityReorderAvySourceIdentityCheck();
    messagingSourceControlsCheck();
    pinnedCurrentSendCheck();
    sidebarAvatarCheck();
    statusIdentityCheck();
    initialHistoryConsumerCheck();
    railReadAllCheck();
    serverLogConsumerCheck();
    queuedServerLogPaletteCheck();
    sojuDisplayNameConsumerCheck();
    sojuFoldConsumerCheck();
    titlebarGeometryCheck();
    staleUploadHostMenuCheck();
    nativePanelResizeCheck();
    halfPageChatKeysCheck();
    retainedBufferStateCheck(true, blast::SidebarMode::Expanded);
    retainedBufferStateCheck(false, blast::SidebarMode::Compact);
    retainedBufferStateCheck(true, blast::SidebarMode::Hidden);
    selectedServerAvyCheck();
    tightSearchFocusCheck();
    ordinaryTypingFocusCheck();
    typingOverflowFocusCheck();
    channelTopicCheck();
    queuedBufferMergeCheck(false);
    queuedBufferMergeCheck(true);
    heldMemberIdentityCheck();
    for (const bool rename : {false, true}) {
        queuedPaneCommandCheck(rename);
        queuedMemberInsertCheck(rename, false);
        queuedMemberInsertCheck(rename, true);
    }
    pendingShutdownFlushCheck();
    transcriptActionFocusCheck(false);
    minimumPendingPasteBoundsCheck();
    messageReactionPickerCheck();
    messageReplyCheck();
    alphaForegroundCheck();
    deferredSettingsTouchGeometryCheck();
    pendingComposerWidthGeometryCheck();
}
