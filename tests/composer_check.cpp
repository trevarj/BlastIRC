#include "../src/model.hpp"
#include "../src/render/graphics.hpp"
#include "../src/theme/theme.hpp"
#include "../src/ui/command_completion.hpp"
#include "../src/ui/editor.hpp"
#include "../src/ui/emoji.hpp"
#include "../src/ui/irc_format.hpp"
#include "../src/ui/irc_palette.hpp"
#include "../src/ui/logic.hpp"
#include "../src/ui/presentation.hpp"
#include "../src/ui/transcript.hpp"
#include "../src/ui/ui.hpp"
#include <GLES2/gl2.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <core/input/key_modifiers.h>
#include <core/text_clipboard.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <linux/input-event-codes.h>
#include <pango/pango.h>
#include <render/codecs/codecs.h>
#include <render/gl_shared_context.h>
#include <render/render_context.h>
#include <render/render_target.h>
#include <render/scene/custom_draw_node.h>
#include <render/scene/input_area.h>
#include <render/scene/input_dispatcher.h>
#include <render/scene/rect_node.h>
#include <render/scene/text_node.h>
#include <render/text/font_registry.h>
#include <source_location>
#include <thread>
#include <tuple>
#include <ui/controls/button.h>
#include <ui/controls/flex.h>
#include <ui/controls/input.h>
#include <ui/controls/label.h>
#include <ui/controls/scroll_view.h>
#include <ui/controls/scrollbar.h>
#include <ui/controls/select_popup_context.h>
#include <variant>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
class DeferredClipboard final : public TextClipboard {
  public:
    TextCallback pending;
    size_t requests = 0;
    std::optional<std::string> copied;
    std::optional<std::string> clipboardText() override { return std::nullopt; }
    void setClipboardText(std::string text) override { copied = std::move(text); }
    void requestClipboardText(TextCallback callback) override {
        ++requests;
        pending = std::move(callback);
    }
    void deliver(std::string text) {
        assert(pending);
        auto callback = std::move(pending);
        callback(std::move(text));
    }
};
class MessageMenuHost final : public SelectPopupContext {
  public:
    DropdownRequest request;
    DropdownCallbacks callbacks;
    bool open = false;
    void openSelectDropdown(const DropdownRequest &value, DropdownCallbacks handlers) override {
        assert(value.options.empty() && !value.menu.empty());
        request = value;
        callbacks = std::move(handlers);
        open = true;
    }
    void closeSelectDropdown() override {
        const bool wasOpen = open;
        auto dismiss = std::move(callbacks.onDismiss);
        open = false;
        callbacks = {};
        if (wasOpen && dismiss)
            dismiss();
    }
    bool isSelectDropdownOpen() const override { return open; }
    const ContextMenuEntry *entry(std::string_view path) const {
        const auto find = [](auto &&self, const auto &entries,
                             std::string_view remaining) -> const ContextMenuEntry * {
            // A real leaf label may itself contain '/', e.g. the text/paste source.
            for (const auto &item : entries)
                if (item.label == remaining && item.enabled && !item.header && !item.separator)
                    return &item;
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
Input *composerInput(Node &node) {
    if (auto *input = dynamic_cast<Input *>(&node))
        return input;
    for (const auto &child : node.children())
        if (auto *input = composerInput(*child))
            return input;
    return nullptr;
}
blast::emoji::CompletionView *completionPopup(Node &node) {
    if (auto *popup = dynamic_cast<blast::emoji::CompletionView *>(&node))
        return popup;
    for (const auto &child : node.children())
        if (auto *popup = completionPopup(*child))
            return popup;
    return nullptr;
}
Input *inputWithDraft(Node &node, std::string_view draft) {
    if (auto *input = dynamic_cast<Input *>(&node); input && input->value() == draft)
        return input;
    for (const auto &child : node.children())
        if (auto *input = inputWithDraft(*child, draft))
            return input;
    return nullptr;
}
RectNode *nativeCaret(Input &input) {
    for (const auto &viewport : input.children())
        if (viewport->clipChildren())
            for (const auto &node : viewport->children())
                if (node->zIndex() == 1 && node->type() == NodeType::Rect)
                    return static_cast<RectNode *>(node.get());
    return nullptr;
}
Button *action(Node &node, std::string_view prefix) {
    if (auto *button = dynamic_cast<Button *>(&node);
        button && button->label() && button->label()->text().starts_with(prefix))
        return button;
    for (const auto &child : node.children())
        if (auto *button = action(*child, prefix))
            return button;
    return nullptr;
}
void activate(Node &node, std::string_view prefix) {
    auto *button = action(node, prefix);
    assert(button && button->inputArea()->enabled());
    button->inputArea()->dispatchKey(0xff0d, 0, 0, true);
    button->inputArea()->dispatchKey(0xff0d, 0, 0, false);
}
Button *composerAction(Node &node, std::string_view role) {
    if (auto *button = dynamic_cast<Button *>(&node);
        button && button->inputArea()->tabFocusKey().ends_with(role))
        return button;
    for (const auto &child : node.children())
        if (auto *button = composerAction(*child, role))
            return button;
    return nullptr;
}
void activateComposerAction(Node &node, std::string_view role) {
    auto *button = composerAction(node, role);
    assert(button && button->inputArea()->enabled());
    button->inputArea()->dispatchKey(XKB_KEY_Return, '\r', 0, true);
    button->inputArea()->dispatchKey(XKB_KEY_Return, '\r', 0, false);
}
void retireComposerPointer(InputDispatcher &dispatcher, Node &affected) {
    const auto owned = [&](const Node *node) {
        for (; node; node = node->parent())
            if (node == &affected)
                return true;
        return false;
    };
    const auto *captured = dispatcher.capturedArea();
    if (owned(captured ? captured : dispatcher.pendingTouchPressArea()))
        dispatcher.cancelPointerCapture();
}
bool displays(Node &node, std::string_view text) {
    if (auto *label = dynamic_cast<Label *>(&node); label && label->text().contains(text))
        return true;
    for (const auto &child : node.children())
        if (displays(*child, text))
            return true;
    return false;
}
Button *tooltipAction(Node &node, std::string_view prefix) {
    if (auto *button = dynamic_cast<Button *>(&node)) {
        const auto tooltip = button->inputArea()->tooltipContent();
        if (const auto *text = std::get_if<std::string>(&tooltip);
            text && text->starts_with(prefix))
            return button;
    }
    for (const auto &child : node.children())
        if (auto *button = tooltipAction(*child, prefix))
            return button;
    return nullptr;
}
void activateTooltip(Node &node, std::string_view tooltip) {
    auto *button = tooltipAction(node, tooltip);
    assert(button && button->inputArea()->enabled());
    button->inputArea()->dispatchKey(XKB_KEY_Return, 0, 0, true);
    button->inputArea()->dispatchKey(XKB_KEY_Return, 0, 0, false);
}
void collectButtons(Node &node, std::vector<Button *> &buttons) {
    if (auto *button = dynamic_cast<Button *>(&node))
        buttons.push_back(button);
    for (const auto &child : node.children())
        collectButtons(*child, buttons);
}
TextNode *contextText(Node &node, std::string_view text) {
    if (auto *body = dynamic_cast<TextNode *>(&node); body && body->text().contains(text))
        return body;
    for (const auto &child : node.children())
        if (auto *body = contextText(*child, text))
            return body;
    return nullptr;
}
Node *conversationPane(Input &input) {
    for (auto *node = input.parent(); node; node = node->parent())
        for (const auto &child : node->children())
            if (dynamic_cast<blast::TranscriptView *>(child.get()))
                return node;
    return nullptr;
}
bool nativeVisible(const Node &node) {
    for (auto *ancestor = &node; ancestor; ancestor = ancestor->parent())
        if (!ancestor->visible())
            return false;
    return true;
}
LayoutRect sceneBounds(const Node &node) {
    LayoutRect result{.width = node.width(), .height = node.height()};
    Node::absolutePosition(&node, result.x, result.y);
    return result;
}
LayoutRect paintedClip(const Node &node, const Node &container) {
    auto clip = sceneBounds(container);
    for (auto *ancestor = node.parent(); ancestor; ancestor = ancestor->parent()) {
        if (!ancestor->clipChildren())
            continue;
        auto bounds = sceneBounds(*ancestor);
        if (const auto *partition = ancestor->pixelPartitionClip()) {
            float left, top, right, bottom;
            Node::mapToScene(ancestor, partition->left, partition->top, left, top);
            Node::mapToScene(ancestor, partition->right, partition->bottom, right, bottom);
            bounds = {std::min(left, right), std::min(top, bottom), std::abs(right - left),
                      std::abs(bottom - top)};
        }
        const float right = std::min(clip.x + clip.width, bounds.x + bounds.width);
        const float bottom = std::min(clip.y + clip.height, bounds.y + bounds.height);
        clip.x = std::max(clip.x, bounds.x);
        clip.y = std::max(clip.y, bounds.y);
        clip.width = std::max(0.f, right - clip.x);
        clip.height = std::max(0.f, bottom - clip.y);
    }
    return clip;
}
void withinBounds(const LayoutRect &rect, const LayoutRect &outer,
                  const std::source_location caller = std::source_location::current()) {
    const bool contained = rect.x >= outer.x - .5f && rect.y >= outer.y - .5f &&
                           rect.x + rect.width <= outer.x + outer.width + .5f &&
                           rect.y + rect.height <= outer.y + outer.height + .5f;
    if (!contained)
        std::fprintf(stderr,
                     "native composer bounds (%g,%g,%g,%g) outside (%g,%g,%g,%g) at %s:%u %s\n",
                     rect.x, rect.y, rect.width, rect.height, outer.x, outer.y, outer.width,
                     outer.height, caller.file_name(), caller.line(), caller.function_name());
    assert(contained);
}
void within(const Node &node, const Node &container,
            const std::source_location caller = std::source_location::current()) {
    withinBounds(sceneBounds(node), sceneBounds(container), caller);
}
void nativeSingleTextRow(Renderer &renderer, const Label &label,
                         const std::source_location caller = std::source_location::current()) {
    const TextNode *text = nullptr;
    for (const auto &child : label.children())
        if (const auto *node = dynamic_cast<const TextNode *>(child.get()))
            text = node;
    assert(text);
    const auto metrics =
        renderer.measureText(label.text(), label.fontSize(), label.font(), label.textWrapWidth(), 1,
                             label.textAlign(), label.ellipsize(), text->useMarkup());
    assert(metrics.lineCount == 1 && metrics.layoutHeight > 0 && label.height() > 0 &&
           metrics.inkRight > metrics.inkLeft && metrics.inkBottom > metrics.inkTop);
    const auto bounds = sceneBounds(label);
    const float baselineX = bounds.x + label.textOriginX();
    const float baselineY = bounds.y + label.baselineOffset();
    const LayoutRect ink{.x = baselineX + metrics.inkLeft,
                         .y = baselineY + metrics.inkTop,
                         .width = metrics.inkRight - metrics.inkLeft,
                         .height = metrics.inkBottom - metrics.inkTop};
    // Native single-line Labels cap-center glyphs; fallback-font logical leading may
    // extend outside that box even when every actual glyph remains safely inside.
    withinBounds(ink, bounds, caller);
}
bool editingDocumentMatches(const Input &input, const Input::EditingState &expected) {
    const auto actual = input.editingState();
    // Layout may clamp native caret scrolling; document semantics remain exact.
    return actual.current == expected.current && actual.preeditBase == expected.preeditBase &&
           actual.preeditStart == expected.preeditStart &&
           actual.preeditLength == expected.preeditLength &&
           actual.preeditCaretHidden == expected.preeditCaretHidden &&
           actual.history == expected.history;
}
LayoutRect paintedTextWithin(Renderer &renderer, TextNode &text, const Node &container) {
    const auto metrics =
        renderer.measureText(text.text(), text.fontSize(), text.font(), text.maxWidth(),
                             text.maxLines(), text.textAlign(), text.ellipsize(), text.useMarkup());
    assert(metrics.layoutBaseline > 0 && metrics.layoutHeight > 0 &&
           metrics.inkRight > metrics.inkLeft && metrics.inkBottom > metrics.inkTop);
    const auto baseline = sceneBounds(text), outer = paintedClip(text, container);
    const LayoutRect ink{.x = baseline.x + metrics.inkLeft,
                         .y = baseline.y + metrics.inkTop,
                         .width = metrics.inkRight - metrics.inkLeft,
                         .height = metrics.inkBottom - metrics.inkTop};
    const float left = std::max(ink.x, outer.x), top = std::max(ink.y, outer.y);
    const float right = std::min(ink.x + ink.width, outer.x + outer.width);
    const float bottom = std::min(ink.y + ink.height, outer.y + outer.height);
    const LayoutRect painted{left, top, std::max(0.f, right - left), std::max(0.f, bottom - top)};
    if (painted.width > 0 && painted.height > 0)
        withinBounds(painted, outer);
    return painted;
}
void registerNativeFonts() {
    // Match the real application's bundled fontconfig/Pango registration.
    for (const char *directory : {BLASTIRC_FONT_DIR, BLASTIRC_EMOJI_DIR}) {
        if (!*directory)
            continue;
        std::error_code error;
        std::filesystem::recursive_directory_iterator files(
            directory, std::filesystem::directory_options::skip_permission_denied, error),
            end;
        while (!error && files != end) {
            const auto &path = files->path();
            const auto extension = path.extension();
            if (extension == ".ttf" || extension == ".otf" || extension == ".ttc")
                text::registerFontFile(path);
            files.increment(error);
        }
    }
}
void checkIndependentMessageScales(Renderer &renderer) {
    using namespace blast;
    Model model;
    model.firstRun = false;
    model.settings.reducedMotion = true;
    model.settings.typingSend = model.settings.spellcheck = false;
    model.settings.fx = FxLevel::Off;
    model.identities.emplace_back();
    ServerConfig server;
    server.name = "scale";
    server.host = "example.invalid";
    model.servers.push_back(server);
    model.networks["scale"].state = "Ready";
    auto &buffer = model.buffers["scale/#room"];
    buffer.id = model.activeBuffer = "scale/#room";
    buffer.server = "scale";
    buffer.target = "#room";
    buffer.kind = "channel";
    buffer.joined = true;
    buffer.draft = "ScaleInput 猫😀 \x02raw\x02";
    Message message;
    message.id = "scale-body";
    message.nick = "ScaleNick";
    message.text = "ScaleBody 猫😀 with enough words to wrap repeatedly in the native viewport. ";
    for (int i = 0; i < 4; ++i)
        message.text += "Another measured line of native message text. ";
    buffer.messages.push_back(message);
    model.settings.workspace = {
        {"1", buffer.id}, {"focused", "1"}, {"native_navigation_mode", "hidden"}};
    Theme theme;
    theme.update(model);
    DeferredClipboard clipboard;
    InputDispatcher dispatcher;
    Ui ui(model, clipboard);
    dispatcher.setSceneRoot(&ui.root());
    ui.setInputDispatcher(dispatcher);
    ui.setWindowActive(true);
    const auto frame = [&] {
        theme.update(model);
        ui.update(720, minimumWindowHeight(renderer), 1);
        ui.root().layout(renderer);
        ui.root().layout(renderer);
    };
    frame();
    auto *input = inputWithDraft(ui.root(), buffer.draft);
    assert(input);
    auto *pane = conversationPane(*input);
    assert(pane);
    auto *send = tooltipAction(*pane, "Send message");
    assert(send);
    dispatcher.setFocus(input->inputArea());
    assert(input->insertText("!"));
    ui.execute("undo");
    auto selected = input->editingState();
    selected.current.selectionAnchor = selected.current.value.size() - 2;
    selected.current.cursorPos = selected.current.value.size() - 1;
    input->restoreEditingState(selected);
    input->textInputApplyEdit({.commitText = {},
                               .preeditText = "日本",
                               .preeditCursorBegin = 6,
                               .preeditCursorEnd = 6,
                               .hasPreedit = true});
    const auto document = input->editingState();
    const auto undo = document.history->undo, redo = document.history->redo;
    for (const auto layout : {MessageLayout::Bubbles, MessageLayout::Irc, MessageLayout::Stacked}) {
        model.settings.messageLayouts[buffer.id] = layout;
        float smallGlyphWidth = 0;
        for (const float messageScale : {.7f, 2.f}) {
            float messageWidth = 0;
            for (const float interfaceScale : {.7f, 2.f}) {
                model.settings.scale = interfaceScale;
                model.settings.messageScale = messageScale;
                frame();
                auto *body = contextText(*pane, "ScaleBody");
                auto *inputText = contextText(*input, "ScaleInput");
                assert(body && inputText && nativeVisible(*body) && nativeVisible(*inputText));
                assert(body->fontSize() == inputText->fontSize() &&
                       body->fontSize() == messageTextSize(model.settings, buffer));
                const auto bodyGlyph = renderer.measureText("Mg猫", body->fontSize(), body->font());
                const auto inputGlyph =
                    renderer.measureText("Mg猫", inputText->fontSize(), inputText->font());
                assert(std::abs(bodyGlyph.width - inputGlyph.width) < .02f &&
                       std::abs(bodyGlyph.layoutHeight - inputGlyph.layoutHeight) < .02f);
                if (messageWidth)
                    assert(std::abs(bodyGlyph.width - messageWidth) < .02f);
                messageWidth = bodyGlyph.width;
                assert(std::abs(send->inputArea()->width() - std::max(32.f, 32 * interfaceScale)) <
                       .5f);
                within(*input, *pane);
                within(*send->inputArea(), *pane);
                assert(editingDocumentMatches(*input, document) && document.history->undo == undo &&
                       document.history->redo == redo && input->composing());
                assert(buffer.messages.front().text == message.text);
            }
            if (smallGlyphWidth)
                assert(messageWidth > smallGlyphWidth);
            smallGlyphWidth = messageWidth;
        }
    }
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void checkComposerReplyCard(Renderer &renderer) {
    using namespace blast;
    Model model;
    model.settings.typingSend = false;
    model.settings.spellcheck = false;
    model.settings.reducedMotion = true;
    auto &buffer = model.buffers["reply/room"];
    buffer.id = "reply/room";
    buffer.server = "reply";
    buffer.target = "#room";
    buffer.draft = "\x02raw 猫😀\nIME\u0085draft\u2028selection\u2029undo cancel\x02";
    assert(ui_logic::composerTextValid(buffer.draft));
    Message source;
    source.id = "wire-reply-id";
    source.nick = "作者<&>";
    for (int i = 0; i < 80; ++i)
        source.nick += "長い";
    source.text = "\x02猫😀 <context>&\x02 \x1ditalic\nsecond line\u0085NEL\u2028LS"
                  "\u2029PS\rCR tail\x1d " +
                  std::string(2048, 'x') + " https://private.invalid/secret";
    buffer.messages.push_back(source);
    model.settings.nickColors[source.nick] = "#39A6B5";
    Theme theme;
    theme.update(model);
    DeferredClipboard clipboard;
    InputDispatcher dispatcher;
    EditorStore editors(
        model, clipboard, [](auto) {}, [](auto) {},
        [&](Node &node) { retireComposerPointer(dispatcher, node); });
    PaneState pane{.id = 701,
                   .buffer = buffer.id,
                   .reply = ui_logic::messageKey(source),
                   .focusComposer = false};
    Node root;
    root.setSize(800, 600);
    auto viewport = std::make_unique<Node>();
    viewport->setSize(800, 600);
    viewport->setClipChildren(true);
    auto *clip = root.addChild(std::move(viewport));
    auto *composer = clip->addChild(editors.composer(pane));
    auto *input = composerInput(*composer);
    assert(input);
    dispatcher.setSceneRoot(&root);
    editors.setInputDispatcher(dispatcher);
    dispatcher.setFocus(input->inputArea());
    auto selected = input->editingState();
    selected.current.selectionAnchor = 1;
    selected.current.cursorPos = 8;
    input->restoreEditingState(selected);
    const auto document = input->editingState();
    const auto undo = document.history->undo, redo = document.history->redo;
    const auto frame = [&](float width, float budget = 260.f) {
        editors.sync();
        // Zero means normal input needs no protected accessory minimum; reply lives
        // outside this budget. Measure the actual native input under the host limit.
        const float minimum = editors.minimumComposerHeight(pane.id, renderer, width, budget);
        assert(minimum >= 0 && minimum <= budget);
        editors.setComposerHeightLimit(pane.id, budget);
        LayoutConstraints constraints;
        constraints.setExactWidth(width);
        const float height = composer->measure(renderer, constraints).height;
        assert(height > 0 && height <= budget);
        composer->arrange(renderer, {12, 180, width, height});
        root.layout(renderer);
        root.layout(renderer);
        return height;
    };
    const auto inputFontMatchesMessages = [&] {
        auto *glyphs = contextText(*input, "raw");
        assert(glyphs &&
               std::abs(glyphs->fontSize() - messageTextSize(model.settings, buffer)) < .01f);
    };
    for (const float width : {160.f, 280.f, 640.f}) {
        frame(width);
        inputFontMatchesMessages();
        auto *cancel = composerAction(*composer, ":reply-cancel");
        auto *heading = contextText(*composer, "Replying to");
        auto *excerpt = contextText(*composer, "&lt;context&gt;&amp;");
        assert(cancel && heading && excerpt && heading->useMarkup() && excerpt->useMarkup());
        auto *card = dynamic_cast<Flex *>(cancel->parent());
        assert(card);
        RectNode *outline = nullptr, *railPaint = nullptr;
        const auto authorColor = nickColor(model, source.nick, buffer.server);
        const auto cardBounds = sceneBounds(*card);
        const auto headingBounds = sceneBounds(*heading->parent());
        const auto inspectPaint = [&](auto &&self, Node &node) -> void {
            if (!node.visible())
                return;
            if (auto *rect = dynamic_cast<RectNode *>(&node)) {
                const auto &style = rect->style();
                const auto bounds = sceneBounds(*rect);
                if (style.borderWidth > 0 && style.border.a > 0 &&
                    std::abs(bounds.width - cardBounds.width) < .5f &&
                    std::abs(bounds.height - cardBounds.height) < .5f)
                    outline = rect;
                if (style.fill.a > 0 && style.fill.a < authorColor.a &&
                    style.fill.r == authorColor.r && style.fill.g == authorColor.g &&
                    style.fill.b == authorColor.b && bounds.width > 0 &&
                    bounds.height > bounds.width &&
                    bounds.x + bounds.width <= headingBounds.x + .5f) {
                    withinBounds(bounds, cardBounds);
                    assert(bounds.y < headingBounds.y + headingBounds.height &&
                           bounds.y + bounds.height > headingBounds.y);
                    railPaint = rect;
                }
            }
            for (const auto &child : node.children())
                self(self, *child);
        };
        inspectPaint(inspectPaint, *card);
        assert(outline && outline->style().radius.tl > 0 && outline->style().radius.tr > 0 &&
               outline->style().radius.br > 0 && outline->style().radius.bl > 0 &&
               outline->style().fill.a == 0);
        assert(railPaint && railPaint->width() < heading->fontSize());
        const auto composerBounds = sceneBounds(*composer);
        assert(cardBounds.y + cardBounds.height <= composerBounds.y + .5f);
        within(*card, root);
        within(*cancel, *card);
        within(*cancel->inputArea(), *card);
        within(*heading->parent(), *card);
        within(*excerpt->parent(), *card);
        assert(cancel->inputArea()->width() >= 32 && cancel->inputArea()->height() >= 32);
        const auto tooltip = cancel->inputArea()->tooltipContent();
        assert(std::get<std::string>(tooltip) == "Cancel reply");
        assert(
            heading->text().contains("weight=\"bold\"") &&
            heading->text().contains(formatRgbHex(nickColor(model, source.nick, buffer.server))) &&
            excerpt->text().contains("猫😀") && excerpt->text().contains("weight=\"bold\"") &&
            excerpt->text().contains("style=\"italic\""));
        PangoAttrList *attributes = nullptr;
        char *display = nullptr;
        GError *error = nullptr;
        assert(pango_parse_markup(excerpt->text().c_str(), -1, 0, &attributes, &display, nullptr,
                                  &error));
        assert(!error && display);
        const std::string_view plain(display);
        const auto expectedPlain = "猫😀 <context>& italic second line  NEL   LS   PSCR tail " +
                                   std::string(2048, 'x') + " https://private.invalid/secret";
        assert(plain == expectedPlain);
        g_free(display);
        pango_attr_list_unref(attributes);
        const auto metrics = renderer.measureText(
            excerpt->text(), excerpt->fontSize(), excerpt->font(), excerpt->maxWidth(),
            excerpt->maxLines(), excerpt->textAlign(), excerpt->ellipsize(), excerpt->useMarkup());
        assert(metrics.lineCount == 1 && metrics.layoutHeight > 0 && excerpt->maxLines() == 1);
        const auto headingMetrics = renderer.measureText(
            heading->text(), heading->fontSize(), heading->font(), heading->maxWidth(),
            heading->maxLines(), heading->textAlign(), heading->ellipsize(), heading->useMarkup());
        assert(headingMetrics.lineCount == 1 && headingMetrics.layoutHeight > 0);
        assert(paintedTextWithin(renderer, *excerpt, *card).height > 0);
        assert(sceneBounds(*heading->parent()).x + heading->parent()->width() <=
               sceneBounds(*cancel).x + .5f);
        assert(input->editingStateMatches(document) && document.history->undo == undo &&
               document.history->redo == redo && buffer.draft == document.current.value &&
               buffer.messages.front().text == source.text &&
               buffer.messages.front().nick == source.nick);
    }
    // The actual native hit target obeys an ancestor viewport, even when the card is wider.
    auto *cancel = composerAction(*composer, ":reply-cancel");
    const auto hit = sceneBounds(*cancel->inputArea());
    clip->setSize(hit.x + hit.width / 2, 600);
    const float y = hit.y + hit.height / 2;
    assert(dispatcher.inputAreaAt(hit.x + hit.width / 4, y) == cancel->inputArea());
    assert(dispatcher.inputAreaAt(hit.x + 3 * hit.width / 4, y) != cancel->inputArea());
    const auto paintClip = paintedClip(*cancel, root);
    const float paintedWidth =
        std::max(0.f, std::min(hit.x + hit.width, paintClip.x + paintClip.width) -
                          std::max(hit.x, paintClip.x));
    assert(paintedWidth > 0 && paintedWidth < hit.width);
    clip->setSize(800, 600);

    // Concealed/deleted sources expose neither the actor, source URL nor tooltip text.
    for (const bool deleted : {false, true}) {
        buffer.messages.front().deleted = deleted;
        if (!deleted)
            model.settings.fools.insert(source.nick);
        frame(280);
        auto *heading = contextText(*composer, "Replying to");
        auto *excerpt = contextText(*composer, deleted ? "Message deleted" : "Message hidden");
        assert(heading && excerpt && heading->text().contains("unknown sender") &&
               !displays(*cancel->parent(), "作者") &&
               !displays(*cancel->parent(), "private.invalid") &&
               !displays(*cancel->parent(), "context"));
        for (auto *text : {heading, excerpt}) {
            const auto tooltip = static_cast<Label *>(text->parent())->tooltipContent();
            const auto *value = std::get_if<std::string>(&tooltip);
            assert(!value || value->empty());
        }
        model.settings.fools.clear();
    }
    for (const auto layout : {MessageLayout::Bubbles, MessageLayout::Irc, MessageLayout::Stacked}) {
        buffer.kind = "channel";
        model.settings.messageLayouts[buffer.id] = layout;
        for (const float interfaceScale : {.7f, 2.f})
            for (const float messageScale : {.7f, 2.f}) {
                model.settings.scale = interfaceScale;
                model.settings.messageScale = messageScale;
                theme.update(model);
                frame(640, 500);
                inputFontMatchesMessages();
                for (const auto *label : {contextText(*composer, "Replying to"),
                                          contextText(*composer, "Message deleted")}) {
                    assert(label && label->fontSize() == messageTextSize(model.settings, buffer));
                }
                assert(input->editingState().current == document.current &&
                       document.history->undo == undo && document.history->redo == redo);
            }
    }
    model.settings.scale = model.settings.messageScale = 1;
    model.settings.messageLayouts.erase(buffer.id);
    theme.update(model);
    buffer.messages.front() = source;
    frame(280);
    assert(editors.execute("paste", pane));
    clipboard.deliver("pending 猫😀\nsecond\nthird\nfourth\nfifth\nsixth");
    editors.sync();
    assert(input->editingStateMatches(document) && document.history->undo == undo &&
           document.history->redo == redo);
    input->textInputApplyEdit({.commitText = {},
                               .preeditText = "日本😀",
                               .preeditCursorBegin = 10,
                               .preeditCursorEnd = 10,
                               .hasPreedit = true});
    assert(input->composing());
    const auto ime = input->editingState();
    const auto imeUndo = ime.history->undo, imeRedo = ime.history->redo;
    const auto imeDocumentPreserved = [&] {
        const auto actual = input->editingState();
        // Native viewport offsets may change when width changes; the complete
        // preedit document, raw selection and history must not.
        return actual.current == ime.current && actual.preeditBase == ime.preeditBase &&
               actual.preeditStart == ime.preeditStart &&
               actual.preeditLength == ime.preeditLength &&
               actual.preeditCaretHidden == ime.preeditCaretHidden && actual.history == ime.history;
    };
    // Pending paste consumes the composer budget; reply remains a fixed two-line overlay.
    frame(640, 260);
    auto *replyCancel = composerAction(*composer, ":reply-cancel");
    auto *replyAuthor = contextText(*composer, "作者");
    auto *replyExcerpt = contextText(*composer, "&lt;context&gt;&amp;");
    auto *pasteContext = contextText(*composer, "Clipboard for ");
    auto *pasteInsert = composerAction(*composer, ":paste-insert");
    auto *pasteSend = composerAction(*composer, ":paste-send");
    auto *pasteCancel = composerAction(*composer, ":paste-cancel");
    assert(replyCancel && replyAuthor && replyExcerpt && pasteContext && pasteInsert && pasteSend &&
           pasteCancel);
    auto *replyCard = replyCancel->parent();
    const float replyHeight = replyCard->height();
    const float replyFont = replyExcerpt->fontSize();
    const float pasteHeight = composer->height();
    const auto fixedReply = [&] {
        nativeSingleTextRow(renderer, *static_cast<Label *>(replyAuthor->parent()));
        nativeSingleTextRow(renderer, *static_cast<Label *>(replyExcerpt->parent()));
        const auto authorBounds = sceneBounds(*replyAuthor->parent());
        const auto excerptBounds = sceneBounds(*replyExcerpt->parent());
        const auto cardBounds = sceneBounds(*replyCard);
        assert(authorBounds.y + authorBounds.height <= excerptBounds.y + .5f &&
               cardBounds.y + cardBounds.height <= sceneBounds(*composer).y + .5f &&
               std::abs(replyCard->height() - replyHeight) <= .5f &&
               replyExcerpt->fontSize() == replyFont);
        within(*replyAuthor->parent(), *replyCard);
        within(*replyExcerpt->parent(), *replyCard);
        within(*replyCancel->inputArea(), *replyCard);
        assert(paintedTextWithin(renderer, *replyExcerpt, *replyCard).height > 0);
    };
    fixedReply();
    for (auto *control : {pasteInsert, pasteSend, pasteCancel}) {
        assert(nativeVisible(*control) && control->inputArea()->enabled() &&
               control->inputArea()->width() >= 32 && control->inputArea()->height() >= 32);
        within(*control->inputArea(), *composer);
        nativeSingleTextRow(renderer, *control->label());
    }
    within(*pasteContext->parent(), *composer);
    const auto authorTooltip = static_cast<Label *>(replyAuthor->parent())->tooltipContent();
    const auto *authorDetails = std::get_if<std::string>(&authorTooltip);
    assert(authorDetails && authorDetails->contains(source.nick));
    for (const bool deleted : {false, true}) {
        buffer.messages.front().deleted = deleted;
        if (!deleted)
            model.settings.fools.insert(source.nick);
        frame(640, 260);
        const auto tooltip = static_cast<Label *>(replyAuthor->parent())->tooltipContent();
        const auto *details = std::get_if<std::string>(&tooltip);
        assert((!details || details->empty()) && !displays(*replyCard, "作者") &&
               !displays(*replyCard, "private.invalid") && !displays(*replyCard, "context"));
        model.settings.fools.clear();
    }
    buffer.messages.front() = source;
    frame(640, 260);
    assert(input->composing() && imeDocumentPreserved() && ime.history->undo == imeUndo &&
           ime.history->redo == imeRedo && buffer.messages.front().text == source.text &&
           buffer.messages.front().nick == source.nick && buffer.draft == document.current.value &&
           !pane.reply.empty() && model.actions.empty());
    const auto pasteCancelHit = sceneBounds(*pasteCancel->inputArea());
    const float pasteCancelX = pasteCancelHit.x + pasteCancelHit.width / 2;
    const float pasteCancelY = pasteCancelHit.y + pasteCancelHit.height / 2;
    assert(dispatcher.pointerButton(pasteCancelX, pasteCancelY, BTN_LEFT, true, 90, 900, false));
    assert(dispatcher.pointerButton(pasteCancelX, pasteCancelY, BTN_LEFT, false, 91, 910, false));
    frame(640, 260);
    fixedReply();
    assert(!nativeVisible(*pasteCancel) && nativeVisible(*replyCancel) &&
           nativeVisible(*replyAuthor) && composer->height() < pasteHeight);
    assert(input->composing() && imeDocumentPreserved() && ime.history->undo == imeUndo &&
           ime.history->redo == imeRedo && buffer.messages.front().text == source.text &&
           buffer.messages.front().nick == source.nick && buffer.draft == document.current.value &&
           !pane.reply.empty() && model.actions.empty());
    // Reply selection does not change normal-flow composer measurement.
    frame(280);
    const float replyingHeight = composer->height();
    const float reserved = editors.replyContextHeight(pane.id, renderer, 280);
    assert(reserved > 0);
    const auto reply = pane.reply;
    pane.reply.clear();
    frame(280);
    assert(std::abs(composer->height() - replyingHeight) <= .5f &&
           std::abs(editors.replyContextHeight(pane.id, renderer, 280) - reserved) <= .5f);
    pane.reply = reply;
    frame(280);
    assert(std::abs(composer->height() - replyingHeight) <= .5f);
    frame(160);
    assert(input->composing() && imeDocumentPreserved() && ime.history->undo == imeUndo &&
           ime.history->redo == imeRedo && buffer.messages.front().text == source.text &&
           buffer.draft == document.current.value);
    // Pointer cancellation must not focus away and commit the native IME preedit.
    cancel = composerAction(*composer, ":reply-cancel");
    const auto cancelHit = sceneBounds(*cancel->inputArea());
    const float x = cancelHit.x + cancelHit.width / 2;
    const float cy = cancelHit.y + cancelHit.height / 2;
    assert(dispatcher.pointerButton(x, cy, BTN_LEFT, true, 100, 1000, false));
    assert(dispatcher.pointerButton(x, cy, BTN_LEFT, false, 101, 1010, false));
    editors.sync();
    assert(pane.reply.empty() && input->composing() && imeDocumentPreserved() &&
           ime.history->undo == imeUndo && ime.history->redo == imeRedo &&
           dispatcher.focusedArea() == input->inputArea() && model.actions.empty() &&
           buffer.messages.front().text == source.text && buffer.draft == document.current.value);
    // Hosts can move the same retained row without changing the editor or owning its lifetime.
    pane.reply = reply;
    frame(280);
    auto external = std::make_unique<Node>();
    auto *host = root.addChild(std::move(external));
    editors.setReplyHost(pane.id, host);
    assert(!composerAction(*composer, ":reply-cancel") &&
           composerAction(*host, ":reply-cancel") == cancel);
    for (const float scale : {1.f, 1.25f, 1.f}) {
        model.settings.scale = scale;
        theme.update(model);
        for (const float width : {640.f, 160.f, 280.f}) {
            frame(width);
            const auto owner = sceneBounds(*composer);
            const float height = editors.replyContextHeight(pane.id, renderer, owner.width);
            assert(host->width() == 0 && host->height() == 0);
            // Match the Pane owner: arrange under a zero-bounds generic host.
            replyCard->arrange(renderer, {owner.x, owner.y - height, owner.width, height});
            for (int pass = 0; pass < 3; ++pass) {
                root.layout(renderer);
                const auto cardBounds = sceneBounds(*replyCard);
                assert(std::abs(cardBounds.width - owner.width) <= .5f &&
                       cardBounds.x + .5f >= owner.x &&
                       cardBounds.x + cardBounds.width <= owner.x + owner.width + .5f);
                within(*replyCard, root);
                within(*cancel->inputArea(), *replyCard);
                const auto hit = sceneBounds(*cancel->inputArea());
                assert(dispatcher.inputAreaAt(hit.x + hit.width / 2, hit.y + hit.height / 2) ==
                       cancel->inputArea());
                int lines = 0;
                for (auto *text : {replyAuthor, replyExcerpt}) {
                    assert(text->ellipsize() == TextEllipsize::End && text->maxLines() == 1);
                    const auto measured = renderer.measureText(
                        text->text(), text->fontSize(), text->font(), text->maxWidth(),
                        text->maxLines(), text->textAlign(), text->ellipsize(), text->useMarkup());
                    lines += measured.lineCount;
                    const auto painted = paintedTextWithin(renderer, *text, *replyCard);
                    assert(painted.x + painted.width <= hit.x + .5f && text->maxWidth() > 0 &&
                           text->maxWidth() <= hit.x - painted.x + .5f);
                }
                assert(lines == 2 && input->composing() && imeDocumentPreserved() &&
                       ime.history->undo == imeUndo && ime.history->redo == imeRedo &&
                       buffer.draft == document.current.value &&
                       buffer.messages.front().text == source.text &&
                       buffer.messages.front().nick == source.nick);
            }
        }
    }
    editors.setReplyHost(pane.id, nullptr);
    assert(host->children().empty() && composerAction(*composer, ":reply-cancel") == cancel);
    editors.setReplyHost(pane.id, host);
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
    clip->removeChild(composer).reset();
    assert(host->children().empty());
}

void checkComposerEditingCommands(Renderer &renderer) {
    using namespace blast;
    Model model;
    model.settings.spellcheck = false;
    auto &buffer = model.buffers["test/#editing"];
    buffer.id = "test/#editing";
    buffer.server = "test";
    buffer.target = "#editing";
    Theme theme;
    theme.update(model);
    DeferredClipboard clipboard;
    InputDispatcher dispatcher;
    EditorStore editors(
        model, clipboard, [](auto) {}, [](auto) {},
        [&](Node &node) { retireComposerPointer(dispatcher, node); });
    PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}, .focusComposer = false};
    Node root;
    root.setSize(640, 480);
    auto *composer = root.addChild(editors.composer(pane));
    auto *input = composerInput(*composer);
    assert(input);
    dispatcher.setSceneRoot(&root);
    editors.setInputDispatcher(dispatcher);
    dispatcher.setFocus(input->inputArea());
    const auto frame = [&] {
        model.now += .05;
        editors.sync();
        composer->arrange(renderer, {20, 300, 600, 150});
        composer->layout(renderer);
    };
    const auto reset = [&](const std::string &text, size_t cursor, size_t anchor) {
        input->setValue(text);
        auto state = input->editingState();
        state.current.cursorPos = cursor;
        state.current.selectionAnchor = anchor;
        input->restoreEditingState(state);
        frame();
    };
    const auto execute = [&](std::string_view command) {
        assert(editors.execute(command, pane));
        frame();
    };
    for (const auto &[before, after] : std::array<std::pair<std::string, std::string>, 3>{
             {{"aé猫", "a猫é"}, {"a👩‍💻猫", "a猫👩‍💻"}, {"a🇬🇧猫", "a猫🇬🇧"}}}) {
        reset(before, before.size(), before.size());
        const auto initial = input->editingState().current;
        const auto undoCount = input->editingState().history->undo.size();
        execute("transpose-chars");
        assert(input->value() == after && buffer.draft == after);
        assert(input->cursorPosition() == after.size() && input->selectionAnchor() == after.size());
        assert(input->editingState().history->undo.size() == undoCount + 1);
        execute("undo");
        assert(input->editingState().current == initial);
        execute("redo");
        assert(input->value() == after);
    }
    const auto foreground = themeForegroundColor("on_surface");
    const auto background = themeSurfaceColor("surface_container_high");
    const std::string formatted = "a\x02"
                                  "b\x02"
                                  "c";
    reset(formatted, 1, 1);
    execute("transpose-chars");
    const auto transposed = parseIrcFormatting(input->value(), foreground, background);
    assert(transposed.text == "bac");
    for (const auto &run : transposed.runs)
        for (size_t i = run.start; i < run.end; ++i)
            assert(run.style.bold == (i == 0));
    execute("undo");
    assert(input->value() == formatted && input->cursorPosition() == 1);
    reset("ab\x02\ncd", 2, 2);
    execute("transpose-chars");
    assert(input->displayProjection()->text == "ba\ncd");
    reset("a\tb", 3, 3);
    execute("transpose-chars");
    assert(input->value() == "ab\t");
    execute("undo");
    assert(input->value() == "a\tb" && input->cursorPosition() == 3);
    reset("ab\ncd", 2, 2);
    execute("transpose-chars");
    assert(input->value() == "ba\ncd" && input->cursorPosition() == 2);
    reset("ab\ncd", 3, 3);
    const auto lineStart = input->editingState();
    const auto lineUndo = lineStart.history->undo;
    execute("transpose-chars");
    assert(input->editingState().current == lineStart.current &&
           lineStart.history->undo == lineUndo);
    reset("aé猫", 4, 1);
    const auto selected = input->editingState().current;
    execute("transpose-chars");
    assert(input->editingState().current == selected);
    reset("a\né猫z", 7, 2);
    const auto beforeKill = input->editingState().current;
    execute("backward-kill-line");
    assert(input->value() == "a\nz" && input->cursorPosition() == 2);
    execute("yank");
    assert(input->value() == "a\né猫z");
    execute("undo");
    assert(input->value() == "a\nz");
    execute("undo");
    assert(input->editingState().current == beforeKill);
    reset("a\né猫z", 2, 2);
    execute("backward-kill-line");
    assert(input->value() == "aé猫z" && input->cursorPosition() == 1);
    reset("aé猫z", 3, 1);
    execute("copy");
    assert(clipboard.copied == "é" && input->value() == "aé猫z");
    execute("cut");
    assert(input->value() == "a猫z" && clipboard.copied == "é");
    execute("undo");
    assert(input->value() == "aé猫z" && input->cursorPosition() == 3 &&
           input->selectionAnchor() == 1);
    execute("select-all");
    assert(input->cursorPosition() == input->value().size() && input->selectionAnchor() == 0);
    execute("paste");
    clipboard.deliver("é");
    frame();
    assert(input->value() == "é");
    reset("hello", 5, 5);
    input->textInputApplyEdit({.commitText = {},
                               .preeditText = "猫",
                               .preeditCursorBegin = 3,
                               .preeditCursorEnd = 3,
                               .hasPreedit = true});
    frame();
    const auto composing = input->editingState();
    const auto imeUndo = composing.history->undo;
    const auto imeRedo = composing.history->redo;
    for (const auto *command :
         {"transpose-chars", "backward-kill-line", "redo", "cut", "select-all"}) {
        execute(command);
        assert(input->editingStateMatches(composing));
        assert(composing.history->undo == imeUndo && composing.history->redo == imeRedo);
        model.settings.bindings[command] = "C-F12 x";
        assert(editors.handleCompositionKey(XKB_KEY_F12, 0, KeyMod::Ctrl, false, false));
        assert(editors.handleCompositionKey('x', 'x', 0, false, false));
        assert(input->editingStateMatches(composing));
        model.settings.bindings.erase(command);
    }
    assert(!editors.handleCompositionKey(0x732b, 0x732b, 0, true, false));
    assert(input->editingStateMatches(composing));
    input->textInputResetPreedit();
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

void checkComposerSpellcheck(Renderer &renderer) {
    using namespace blast;
    Model model;
    model.settings.spellcheck = true;
    model.settings.spellcheckLanguage = "en_US";
    auto &buffer = model.buffers["test/#room"];
    buffer.id = "test/#room";
    buffer.server = "test";
    buffer.target = "#room";
    buffer.nicks.push_back({.name = "zzknownnick", .account = {}, .prefix = {}});
    model.networks["test"].channelTypes = "#&+!";
    auto &other = model.buffers["test/#other"];
    other.id = "test/#other";
    other.server = "test";
    other.target = "#other";
    other.draft = "other draft";
    Theme theme;
    theme.update(model);
    const auto foreground = themeForegroundColor("on_surface");
    const auto background = themeSurfaceColor("surface_container_high");
    DeferredClipboard clipboard;
    InputDispatcher dispatcher;
    EditorStore editors(
        model, clipboard, [](auto) {}, [](auto) {},
        [&](Node &node) { retireComposerPointer(dispatcher, node); });
    PaneState pane{.id = 1, .buffer = buffer.id, .reply = {}, .focusComposer = false};
    Node root;
    root.setSize(640, 480);
    editors.setOverlayHost(&root);
    auto *composer = root.addChild(editors.composer(pane));
    auto *input = composerInput(*composer);
    assert(input);
    dispatcher.setSceneRoot(&root);
    editors.setInputDispatcher(dispatcher);
    dispatcher.setFocus(input->inputArea());
    MessageMenuHost menu;
    input->setPopupContext(&menu);
    const auto frame = [&] {
        model.now += .05;
        editors.sync();
        composer->arrange(renderer, {20, 300, 600, 150});
        composer->layout(renderer);
    };
    const auto await = [&](auto ready) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            frame();
            if (ready())
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (std::chrono::steady_clock::now() < deadline);
        assert(false && "composer dictionary result did not reach its display projection");
    };
    using Ranges = std::vector<std::pair<std::size_t, std::size_t>>;
    const auto errors = [&] {
        const auto *projection = input->displayProjection();
        assert(projection);
        PangoAttrList *attributes = nullptr;
        char *text = nullptr;
        GError *error = nullptr;
        assert(pango_parse_markup(projection->markup.c_str(), -1, 0, &attributes, &text, nullptr,
                                  &error));
        assert(!error && text && projection->text == text);
        g_free(text);
        Ranges ranges;
        auto *iterator = pango_attr_list_get_iterator(attributes);
        do {
            auto *attribute = pango_attr_iterator_get(iterator, PANGO_ATTR_UNDERLINE);
            if (!attribute ||
                reinterpret_cast<PangoAttrInt *>(attribute)->value != PANGO_UNDERLINE_ERROR)
                continue;
            int begin = 0, end = 0;
            pango_attr_iterator_range(iterator, &begin, &end);
            const auto finish = std::min<std::size_t>(end, projection->text.size());
            if (!ranges.empty() && ranges.back().second == std::size_t(begin))
                ranges.back().second = finish;
            else
                ranges.emplace_back(begin, finish);
        } while (pango_attr_iterator_next(iterator));
        pango_attr_iterator_destroy(iterator);
        pango_attr_list_unref(attributes);
        return ranges;
    };
    const auto caret = [&](std::size_t position, std::size_t anchor) {
        auto state = input->editingState();
        state.current.cursorPos = position;
        state.current.selectionAnchor = anchor;
        input->restoreEditingState(state);
    };
    const auto openKeyboard = [&] {
        menu.closeSelectDropdown();
        input->inputArea()->dispatchKey(XKB_KEY_Menu, 0, 0, true);
        input->inputArea()->dispatchKey(XKB_KEY_Menu, 0, 0, false);
        assert(menu.open && !menu.request.inputSerial);
    };
    const auto readyTypo = [&] {
        input->setValue("teh");
        caret(1, 1);
        await([&] { return errors() == Ranges{{0, 3}}; });
        openKeyboard();
        assert(menu.entry("Spelling/the"));
    };

    // Annotation is display-only, including raw controls, multibyte offsets and selection.
    const std::string raw = "😀 \x02"
                            "t\x1d"
                            "eh\x1d\x02 suffix";
    input->setValue(raw);
    caret(8, 0);
    const auto original = input->editingState();
    const auto originalUndo = original.history->undo;
    const auto originalRedo = original.history->redo;
    await([&] { return errors() == Ranges{{5, 8}}; });
    assert(input->value() == raw && input->editingStateMatches(original) &&
           original.history->undo == originalUndo && original.history->redo == originalRedo);
    openKeyboard();
    assert(menu.entry("Spelling/the"));
    menu.select("Spelling/the");
    frame();
    assert(input->displayProjection()->text == "😀 the suffix" &&
           input->editingState().history->undo.size() == originalUndo.size() + 1);
    const auto corrected = parseIrcFormatting(input->value(), foreground, background);
    const auto before = parseIrcFormatting(raw, foreground, background);
    assert(!corrected.runs.empty() && corrected.runs.back().style == before.runs.back().style);
    assert(editors.execute("undo", pane));
    assert(input->value() == raw && input->editingState().current == original.current &&
           input->editingState().history->undo == originalUndo);
    menu.closeSelectDropdown();

    // Pointer lookup uses a real shaped glyph cell, not a synthetic raw offset or caret.
    input->setValue("hello teh");
    caret(0, 0);
    await([&] { return errors() == Ranges{{6, 9}}; });
    std::optional<std::pair<float, float>> hit;
    for (float y = 0; y < input->height() && !hit; y += 1)
        for (float x = 0; x < input->width() && !hit; x += 1)
            if (const auto byte = input->textByteAtPoint(x, y); byte && *byte >= 6 && *byte < 9)
                hit = std::pair{x, y};
    assert(hit);
    const auto pointerOriginal = input->editingState();
    const auto bounds = sceneBounds(*input);
    input->inputArea()->dispatchPress(hit->first, hit->second, BTN_RIGHT, true,
                                      bounds.x + hit->first, bounds.y + hit->second, 1001, 10010);
    input->inputArea()->dispatchPress(hit->first, hit->second, BTN_RIGHT, false,
                                      bounds.x + hit->first, bounds.y + hit->second, 1002, 10020);
    assert(menu.open && menu.request.inputSerial == 1001 && menu.entry("Spelling/the") &&
           input->editingStateMatches(pointerOriginal));
    menu.select("Spelling/the");
    assert(input->value() == "hello the");
    assert(editors.execute("undo", pane) &&
           input->editingState().current == pointerOriginal.current);

    // A pointer correction's edit range, not the distant caret/selection, owns suffix styling.
    const std::string pointerStyledRaw = "\x02t\x02"
                                         "eh suffix";
    input->setValue(pointerStyledRaw);
    caret(pointerStyledRaw.size(), pointerStyledRaw.size() - 2);
    await([&] { return errors() == Ranges{{0, 3}}; });
    hit.reset();
    for (float y = 0; y < input->height() && !hit; y += 1)
        for (float x = 0; x < input->width() && !hit; x += 1)
            if (const auto byte = input->textByteAtPoint(x, y); byte && *byte >= 1 && *byte < 5)
                hit = std::pair{x, y};
    assert(hit);
    const auto pointerStyledOriginal = input->editingState();
    const auto pointerStyledUndo = pointerStyledOriginal.history->undo;
    const auto pointerStyledDocument = parseIrcFormatting(pointerStyledRaw, foreground, background);
    assert(!pointerStyledDocument.runs.back().style.bold);
    input->inputArea()->dispatchPress(hit->first, hit->second, BTN_RIGHT, true,
                                      bounds.x + hit->first, bounds.y + hit->second, 1003, 10030);
    input->inputArea()->dispatchPress(hit->first, hit->second, BTN_RIGHT, false,
                                      bounds.x + hit->first, bounds.y + hit->second, 1004, 10040);
    assert(menu.open && menu.entry("Spelling/the") &&
           input->editingStateMatches(pointerStyledOriginal));
    menu.select("Spelling/the");
    const auto pointerStyledCorrected = parseIrcFormatting(input->value(), foreground, background);
    assert(pointerStyledCorrected.text == "the suffix" &&
           pointerStyledCorrected.runs.back().style == pointerStyledDocument.runs.back().style &&
           !pointerStyledCorrected.runs.back().style.bold &&
           input->editingState().history->undo.size() == pointerStyledUndo.size() + 1);
    assert(editors.execute("undo", pane) && input->value() == pointerStyledRaw &&
           input->editingState().current == pointerStyledOriginal.current &&
           input->editingState().history->undo == pointerStyledUndo);

    // Trailing control affinity must stop at the first UTF-8 scalar's leading byte.
    const std::string unicodeWord = "érrror";
    const std::string unicodeRaw = "\x02" + unicodeWord + "\x02 suffix";
    input->setValue(unicodeRaw);
    caret(3, 1);
    const auto unicodeOriginal = input->editingState();
    const auto unicodeUndo = unicodeOriginal.history->undo;
    const auto unicodeDocument = parseIrcFormatting(unicodeRaw, foreground, background);
    await([&] { return errors() == Ranges{{0, unicodeWord.size()}}; });
    openKeyboard();
    const auto *spelling = menu.entry("Spelling");
    assert(spelling);
    std::string unicodeSuggestion;
    for (const auto &child : spelling->children)
        if (child.enabled && !child.header && !child.separator && child.children.empty() &&
            child.value != std::size_t(-1)) {
            unicodeSuggestion = child.label;
            break;
        }
    assert(!unicodeSuggestion.empty() && unicodeSuggestion != unicodeWord);
    menu.select("Spelling/" + unicodeSuggestion);
    const auto unicodeCorrected = parseIrcFormatting(input->value(), foreground, background);
    assert(unicodeCorrected.text == unicodeSuggestion + " suffix" &&
           input->value().starts_with('\x02') && unicodeCorrected.runs.front().style.bold &&
           unicodeCorrected.runs.back().style == unicodeDocument.runs.back().style &&
           !unicodeCorrected.runs.back().style.bold &&
           input->editingState().history->undo.size() == unicodeUndo.size() + 1);
    assert(editors.execute("undo", pane) && input->value() == unicodeRaw &&
           input->editingState().current == unicodeOriginal.current &&
           input->editingState().history->undo == unicodeUndo);

    // A later ordinary typo proves the worker completed while each earlier token is excluded.
    for (const std::string excluded : {"https://example.com/teh", "zzknownnick", "#zzchannel",
                                       "`teh`", "\x11teh\x11", "/zzcommand teh\nhello"}) {
        input->setValue(excluded + " teh");
        const auto decoded = parseIrcFormatting(input->value(), foreground, background).text;
        const auto begin = decoded.size() - 3;
        await([&] { return errors() == Ranges{{begin, begin + 3}}; });
    }

    // Existing menu closures cannot apply to a newer document, source, IME or preference.
    for (int change = 0; change != 5; ++change) {
        readyTypo();
        const auto index = menu.entry("Spelling/the")->value;
        auto stale = menu.callbacks.onSelect;
        menu.closeSelectDropdown();
        if (change == 0) {
            input->textInputApplyEdit(
                {.commitText = "x", .preeditText = {}, .hasCommitText = true});
            assert(input->value() == "txeh" && buffer.draft == "txeh");
        } else if (change == 1)
            pane.buffer = other.id;
        else if (change == 2)
            input->textInputApplyEdit({.commitText = {},
                                       .preeditText = "teh",
                                       .preeditCursorBegin = 3,
                                       .preeditCursorEnd = 3,
                                       .hasPreedit = true});
        else if (change == 3)
            model.settings.spellcheck = false;
        else
            model.settings.spellcheckLanguage = "zz_unavailable";
        frame();
        const auto unchanged = input->editingState();
        const auto undo = unchanged.history->undo;
        stale(index);
        assert(input->editingStateMatches(unchanged) && unchanged.history->undo == undo);
        if (change == 2)
            assert(input->composing() && errors().empty());
        input->textInputResetPreedit();
        pane.buffer = buffer.id;
        model.settings.spellcheck = true;
        model.settings.spellcheckLanguage = "en_US";
        frame();
    }

    readyTypo();
    model.settings.spellcheck = false;
    frame();
    assert(errors().empty());
    openKeyboard();
    assert(!menu.entry("Spelling/the"));
    model.settings.spellcheck = true;
    await([&] { return errors() == Ranges{{0, 3}}; });
    model.settings.spellcheckLanguage = "zz_unavailable";
    frame();
    await([&] {
        openKeyboard();
        for (const auto &entry : menu.request.menu)
            if (entry.label == "Spelling")
                for (const auto &child : entry.children)
                    if (!child.enabled && child.label.contains("zz_unavailable") &&
                        !child.separator)
                        return errors().empty();
        return false;
    });
    assert(!menu.entry("Spelling/the"));

    // Command ghost and IME preedit must never become committed spelling decoration.
    model.settings.spellcheckLanguage = "en_US";
    input->setValue("/kick ");
    frame();
    assert(input->displayProjection()->text == "/kick " && errors().empty());
    auto *ghost = contextText(*input, "nick");
    assert(ghost && nativeVisible(*ghost) && !ghost->useMarkup() &&
           !ghost->participatesInLayout() && !ghost->hitTestVisible());
    assert(input->value() == "/kick " && input->textInputState().surroundingText == "/kick " &&
           input->displayProjection()->text == "/kick " && errors().empty());
    input->setValue("hello");
    frame();
    const auto committedBeforeIme = input->editingState();
    const auto imeUndo = committedBeforeIme.history->undo;
    const auto imeRedo = committedBeforeIme.history->redo;
    assert(!input->composing() && input->value() == "hello" && buffer.draft == "hello");
    input->textInputApplyEdit({.commitText = {},
                               .preeditText = "teh",
                               .preeditCursorBegin = 3,
                               .preeditCursorEnd = 3,
                               .hasPreedit = true});
    const auto composing = input->editingState();
    assert(composing.preeditBase && composing.preeditBase->value == "hello" &&
           composing.current.value == "helloteh" && composing.preeditStart == 5 &&
           composing.preeditLength == 3);
    frame();
    assert(input->composing());
    assert(errors().empty());
    assert(input->value() == "helloteh" && buffer.draft == "hello");
    assert(input->editingStateMatches(composing));
    assert(composing.history->undo == imeUndo && composing.history->redo == imeRedo);
    input->textInputResetPreedit();
    assert(input->value() == "hello" && buffer.draft == "hello" &&
           input->editingState().current == committedBeforeIme.current &&
           committedBeforeIme.history->undo == imeUndo &&
           committedBeforeIme.history->redo == imeRedo);
    input->setPopupContext(nullptr);
    editors.setOverlayHost(nullptr);
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}
void checkComposerColors(Renderer &renderer) {
    using namespace blast;
    const char *oldHome = std::getenv("XDG_CONFIG_HOME");
    const std::optional<std::string> previousHome =
        oldHome ? std::optional<std::string>{oldHome} : std::nullopt;
    char directory[] = "/tmp/blastirc-composer-colors-XXXXXX";
    const auto *created = mkdtemp(directory);
    assert(created && setenv("XDG_CONFIG_HOME", created, 1) == 0);
    const auto config = std::filesystem::path(created) / "blastirc";
    std::filesystem::create_directory(config);
    {
        Model model;
        model.settings.spellcheck = false;
        model.settings.reducedMotion = true;
        model.settings.themeSource = "builtin";
        auto &buffer = model.buffers["colors/#room"];
        buffer.id = "colors/#room";
        buffer.server = "colors";
        buffer.target = "#room";
        buffer.kind = "channel";
        buffer.nicks = {{.name = "Alice", .account = {}, .prefix = {}},
                        {.name = "[Peer]", .account = {}, .prefix = {}}};
        auto &network = model.networks["colors"];
        network.nick = "Me";
        network.caseMapping = "rfc1459";
        Theme theme;
        theme.update(model);
        DeferredClipboard clipboard;
        InputDispatcher dispatcher;
        EditorStore editors(
            model, clipboard, [](auto) {}, [](auto) {},
            [&](Node &node) { retireComposerPointer(dispatcher, node); });
        PaneState pane{.id = 891, .buffer = buffer.id, .reply = {}, .focusComposer = false};
        Node root;
        root.setSize(640, 480);
        auto *composer = root.addChild(editors.composer(pane));
        auto *input = composerInput(*composer);
        assert(input);
        dispatcher.setSceneRoot(&root);
        editors.setInputDispatcher(dispatcher);
        dispatcher.setFocus(input->inputArea());
        const auto frame = [&] {
            editors.sync();
            composer->arrange(renderer, {20, 300, 600, 150});
            composer->layout(renderer);
        };
        const auto colorAt = [&](std::size_t raw, PangoAttrType kind) -> std::optional<Color> {
            const auto *projection = input->displayProjection();
            assert(projection && raw < projection->rawToDisplay.size());
            PangoAttrList *attributes = nullptr;
            char *text = nullptr;
            GError *error = nullptr;
            assert(pango_parse_markup(projection->markup.c_str(), -1, 0, &attributes, &text,
                                      nullptr, &error));
            assert(!error && text && projection->text == text);
            g_free(text);
            std::optional<Color> result;
            auto *iterator = pango_attr_list_get_iterator(attributes);
            do {
                int begin = 0, end = 0;
                pango_attr_iterator_range(iterator, &begin, &end);
                const auto offset = projection->rawToDisplay[raw];
                if (offset < std::size_t(begin) || offset >= std::size_t(end))
                    continue;
                if (auto *attribute = pango_attr_iterator_get(iterator, kind)) {
                    const auto color = reinterpret_cast<PangoAttrColor *>(attribute)->color;
                    result =
                        Color{color.red / 65535.f, color.green / 65535.f, color.blue / 65535.f, 1};
                }
                break;
            } while (pango_attr_iterator_next(iterator));
            pango_attr_iterator_destroy(iterator);
            pango_attr_list_unref(attributes);
            return result;
        };
        const auto sameColor = [](Color a, Color b) {
            return std::abs(a.r - b.r) < .005f && std::abs(a.g - b.g) < .005f &&
                   std::abs(a.b - b.b) < .005f && std::abs(a.a - b.a) < 1e-6f;
        };
        const auto expectNick = [&](std::string_view token, const std::string &nick) {
            const auto offset = input->value().find(token);
            assert(offset != std::string::npos);
            for (std::size_t i = 0; i < token.size(); ++i) {
                const auto color = colorAt(offset + i, PANGO_ATTR_FOREGROUND);
                assert(color && sameColor(*color, nickColor(model, nick, buffer.server)));
            }
        };
        const std::string raw = "😀 Alice Me {peer} Unknown https://host/Alice AliceSuffix xAlice "
                                "\x02\x1d\x1f\x1e\x11"
                                "Alice"
                                "\x0f "
                                "\x03"
                                "04,02Alice"
                                "\x0f "
                                "\x04"
                                "12AB34,56789AAlice"
                                "\x0f "
                                "\x16"
                                "Alice"
                                "\x0f";
        input->setValue(raw);
        assert(input->insertText("!")); // Keep real undo history through decoration refreshes.
        frame();
        auto selection = input->editingState();
        selection.current.selectionAnchor = raw.find("Alice");
        selection.current.cursorPos = selection.current.selectionAnchor + 5;
        input->restoreEditingState(selection);
        frame();
        const auto document = input->editingState();
        const auto undo = document.history->undo, redo = document.history->redo;
        const auto map = input->displayProjection()->rawToDisplay;
        const auto unchanged = [&] {
            assert(input->editingStateMatches(document) && document.history->undo == undo &&
                   document.history->redo == redo && buffer.draft == input->value() &&
                   input->displayProjection()->rawToDisplay == map);
        };
        expectNick("Alice", "Alice");
        expectNick("Me", "Me");
        expectNick("{peer}", "[Peer]");
        for (const auto token : {"Unknown", "https://host/Alice", "AliceSuffix", "xAlice"}) {
            const auto start = input->value().find(token);
            for (std::size_t i = 0; i < std::string_view(token).size(); ++i)
                assert(!colorAt(start + i, PANGO_ATTR_FOREGROUND));
        }
        const auto styled = raw.find("\x02\x1d\x1f\x1e\x11") + 5;
        assert(colorAt(styled, PANGO_ATTR_FOREGROUND) &&
               sameColor(*colorAt(styled, PANGO_ATTR_FOREGROUND),
                         nickColor(model, "Alice", buffer.server)));
        PangoAttrList *attributes = nullptr;
        char *plain = nullptr;
        GError *error = nullptr;
        assert(pango_parse_markup(input->displayProjection()->markup.c_str(), -1, 0, &attributes,
                                  &plain, nullptr, &error));
        assert(!error && plain);
        g_free(plain);
        auto *styles = pango_attr_list_get_iterator(attributes);
        const auto styledOffset = map[styled];
        bool checkedStyle = false, checkedPlain = false;
        do {
            int begin = 0, end = 0;
            pango_attr_iterator_range(styles, &begin, &end);
            const auto integer = [&](PangoAttrType type, int fallback) {
                const auto *attribute = pango_attr_iterator_get(styles, type);
                return attribute ? reinterpret_cast<const PangoAttrInt *>(attribute)->value
                                 : fallback;
            };
            if (map[raw.find("Alice")] >= std::size_t(begin) &&
                map[raw.find("Alice")] < std::size_t(end)) {
                assert(integer(PANGO_ATTR_WEIGHT, PANGO_WEIGHT_NORMAL) == PANGO_WEIGHT_NORMAL);
                checkedPlain = true;
            }
            if (styledOffset >= std::size_t(begin) && styledOffset < std::size_t(end)) {
                assert(integer(PANGO_ATTR_WEIGHT, PANGO_WEIGHT_NORMAL) == PANGO_WEIGHT_BOLD &&
                       integer(PANGO_ATTR_STYLE, PANGO_STYLE_NORMAL) == PANGO_STYLE_ITALIC &&
                       integer(PANGO_ATTR_UNDERLINE, PANGO_UNDERLINE_NONE) ==
                           PANGO_UNDERLINE_SINGLE &&
                       integer(PANGO_ATTR_STRIKETHROUGH, 0));
                const auto *family = pango_attr_iterator_get(styles, PANGO_ATTR_FAMILY);
                assert(family &&
                       std::string_view(reinterpret_cast<const PangoAttrString *>(family)->value) ==
                           "monospace");
                checkedStyle = true;
            }
        } while (pango_attr_iterator_next(styles));
        assert(checkedStyle && checkedPlain);
        pango_attr_iterator_destroy(styles);
        pango_attr_list_unref(attributes);
        // Native shaped markup retains all authored non-color attributes.
        auto *glyphs = contextText(*input, "Alice");
        assert(glyphs && glyphs->useMarkup());
        const auto metrics = renderer.measureText(input->displayProjection()->markup,
                                                  glyphs->fontSize(), glyphs->font(), 600, 0,
                                                  glyphs->textAlign(), glyphs->ellipsize(), true);
        assert(metrics.layoutHeight > 0 && metrics.inkRight > metrics.inkLeft);
        const auto indexed = raw.find("04,02Alice") + 5;
        assert(sameColor(*colorAt(indexed, PANGO_ATTR_FOREGROUND), rgbHex(ircPalette[4])) &&
               sameColor(*colorAt(indexed, PANGO_ATTR_BACKGROUND), rgbHex(ircPalette[2])));
        const auto hexadecimal = raw.find("12AB34,56789AAlice") + 13;
        assert(sameColor(*colorAt(hexadecimal, PANGO_ATTR_FOREGROUND), rgbHex(0x12ab34)) &&
               sameColor(*colorAt(hexadecimal, PANGO_ATTR_BACKGROUND), rgbHex(0x56789a)));
        const auto reversed = raw.find("\x16") + 1;
        assert(sameColor(*colorAt(reversed, PANGO_ATTR_FOREGROUND),
                         themeSurfaceColor("surface_container_high")) &&
               sameColor(*colorAt(reversed, PANGO_ATTR_BACKGROUND),
                         themeForegroundColor("on_surface")));
        const auto parsed = parseIrcFormatting(input->value(), themeForegroundColor("on_surface"),
                                               themeSurfaceColor("surface_container_high"));
        assert(input->displayProjection()->text == parsed.text && map == parsed.rawToDisplay);
        buffer.nicks[0].name = "Unknown"; // Same-sized nicklist mutation.
        frame();
        assert(!colorAt(raw.find("Alice"), PANGO_ATTR_FOREGROUND));
        expectNick("Unknown", "Unknown");
        unchanged();
        network.nick = "Alice";
        network.caseMapping = "ascii";
        frame();
        expectNick("Alice", "Alice");
        assert(!colorAt(raw.find("Me"), PANGO_ATTR_FOREGROUND) &&
               !colorAt(raw.find("{peer}"), PANGO_ATTR_FOREGROUND));
        unchanged();
        model.settings.nickPalette = "VIVID";
        theme.update(model);
        frame();
        expectNick("Alice", "Alice");
        unchanged();
        model.settings.nickColors["Alice"] = "#d34a79";
        theme.update(model);
        frame();
        expectNick("Alice", "Alice");
        unchanged();
        buffer.kind = "query";
        buffer.target = "Me";
        frame();
        expectNick("Me", "Me");
        unchanged();
        auto &other = model.buffers["colors/other"];
        other.id = "colors/other";
        other.server = "colors";
        other.kind = "query";
        other.target = "Unknown";
        other.draft = input->value();
        pane.buffer = other.id;
        frame();
        assert(!colorAt(raw.find("Me"), PANGO_ATTR_FOREGROUND));
        expectNick("Unknown", "Unknown");
        pane.buffer = buffer.id;
        frame();
        unchanged();
        input->clearSelection();
        input->textInputApplyEdit({.commitText = {},
                                   .preeditText = "Alice猫",
                                   .preeditCursorBegin = 8,
                                   .preeditCursorEnd = 8,
                                   .hasPreedit = true});
        frame();
        const auto ime = input->editingState();
        const auto imeUndo = ime.history->undo, imeRedo = ime.history->redo;
        network.nick = "Me";
        buffer.nicks.clear();
        model.settings.nickColors["Me"] = "#37a96d";
        theme.update(model);
        frame();
        assert(input->composing() && input->editingStateMatches(ime) &&
               ime.history->undo == imeUndo && ime.history->redo == imeRedo);
        input->textInputResetPreedit();
        frame();
        const auto afterIme = input->editingState();
        const auto afterImeUndo = afterIme.history->undo, afterImeRedo = afterIme.history->redo;
        dispatcher.setFocus(nullptr);
        dispatcher.setSceneRoot(nullptr);

        command_completion::View popup;
        const std::array matches{command_completion::Match{"kick", "Remove a user"},
                                 command_completion::Match{"Alice", "Nickname", true}};
        TextFont font{};
        font.family = "sans-serif";
        const auto checkRows = [&](std::size_t selected) {
            popup.setMatches(matches, selected);
            popup.setAnchor({20, 400, 300, 40}, {0, 0, 640, 480}, 1, font);
            popup.setVisible(true);
            popup.layout(renderer);
            ScrollView *scroll = nullptr;
            for (const auto &child : popup.children())
                if (auto *view = dynamic_cast<ScrollView *>(child.get()))
                    scroll = view;
            assert(scroll);
            RectNode *plate = nullptr;
            for (const auto &child : scroll->children())
                if (auto *rect = dynamic_cast<RectNode *>(child.get()))
                    plate = rect;
            assert(plate &&
                   sameColor(plate->style().fill, themeSurfaceColor("surface_container_high")));
            std::size_t rows = 0;
            for (const auto &child : scroll->content()->children()) {
                auto *row = dynamic_cast<InputArea *>(child.get());
                if (!row || !row->visible())
                    continue;
                ++rows;
                const auto check = [&] {
                    RectNode *background = nullptr;
                    for (const auto &node : row->children())
                        if (auto *rect = dynamic_cast<RectNode *>(node.get()))
                            background = rect;
                    assert(background);
                    const auto fill = background->style().fill;
                    // Resolve the logical popup plate; translucent desktop pixels are unknown.
                    const auto plateFill = plate->style().fill;
                    const auto backing = withAlpha(themeColor("surface"), 1);
                    const auto resolvedPlate =
                        withAlpha(lerpColor(backing, plateFill, plateFill.a), 1);
                    const auto effective = withAlpha(lerpColor(resolvedPlate, fill, fill.a), 1);
                    std::size_t labels = 0;
                    for (const auto &node : row->children())
                        if (auto *label = dynamic_cast<Label *>(node.get())) {
                            ++labels;
                            assert(label->color().a == 1);
                            const auto a = relativeLuminance(label->color());
                            const auto b = relativeLuminance(effective);
                            assert((std::max(a, b) + .05f) / (std::min(a, b) + .05f) >= 4.5f);
                        }
                    assert(labels == 2);
                };
                check(); // Normal command and keyboard-selected literal rows.
                row->dispatchEnter(1, 1);
                check(); // Hover, including selected + hovered precedence.
                row->dispatchLeave();
            }
            assert(rows == matches.size());
        };
        checkRows(0);
        checkRows(1);
        // Authored light, low-contrast colors and user transparency exercise real theme loading.
        auto *file = std::fopen((config / "noctalia.toml").c_str(), "w");
        assert(file);
        assert(std::fputs("[colors]\nsurface='#fafafa'\nsurface_container_high='#fefefe'\n"
                          "on_surface='#eeeeee'\non_surface_variant='#dddddd'\n"
                          "primary='#eeeeee'\non_primary='#181818'\nprimary_container='#eeeeee55'\n"
                          "on_primary_container='#dddddd'\nhover='#ffffff66'\n"
                          "on_hover='#eeeeee'\ntertiary='#aaaaaa'\nerror='#bb2222'\n",
                          file) >= 0);
        assert(std::fclose(file) == 0);
        model.settings.themeSource = "auto";
        model.settings.transparency = true;
        for (const float opacity : {1.f, .3f}) {
            model.settings.opacity = opacity;
            // Advance the public frame clock through discovery and palette transition.
            model.now += 1;
            theme.update(model);
            model.now += 1;
            theme.update(model);
            assert(sameColor(themeColor("surface"), ::hex("#fafafa")) &&
                   sameColor(themeColor("primary_container"), ::hex("#eeeeee55")));
            frame();
            expectNick("Me", "Me");
            assert(input->editingStateMatches(afterIme) && afterIme.history->undo == afterImeUndo &&
                   afterIme.history->redo == afterImeRedo &&
                   input->displayProjection()->rawToDisplay == map);
            checkRows(0);
            checkRows(1);
        }
    }
    if (previousHome)
        assert(setenv("XDG_CONFIG_HOME", previousHome->c_str(), 1) == 0);
    else
        assert(unsetenv("XDG_CONFIG_HOME") == 0);
    std::filesystem::remove_all(created);
}
} // namespace

void checkEmojiCompletion();

void runComposerCheck() {
    using namespace blast;
    checkEmojiCompletion();
    {
        Model uploads;
        uploads.buffers["source"].id = "source";
        uploads.buffers["other"].id = "other";
        DeferredClipboard clipboard;
        InputDispatcher dispatcher;
        EditorStore store(
            uploads, clipboard, [](auto) {}, [](auto) {},
            [&](Node &node) { retireComposerPointer(dispatcher, node); });
        PaneState pane{.id = 91, .buffer = "source", .reply = {}, .focusComposer = false};
        Node root;
        auto *composer = root.addChild(store.composer(pane));
        auto *input = composerInput(*composer);
        auto *clip = composerAction(*composer, "upload");
        assert(input && clip && clip->visible() && clip->inputArea()->enabled());
        dispatcher.setSceneRoot(&root);
        store.setInputDispatcher(dispatcher);
        dispatcher.setFocus(input->inputArea());
        using Result = EditorStore::UploadInsertResult;
        const auto empty = store.captureUploadOrigin(pane.id);
        assert(empty);
        std::optional<EditorStore::UploadOrigin> requested;
        store.setOnUploadRequested([&](std::uint64_t id, const std::string &buffer, Node &anchor,
                                       std::optional<InputArea::PointerData> pointer) {
            assert(id == pane.id && buffer == "source" && &anchor == clip && !pointer);
            requested = store.captureUploadOrigin(id);
        });
        activateComposerAction(*composer, "upload");
        assert(requested && dispatcher.focusedArea() == input->inputArea());
        assert(store.uploadSourceCurrent(pane.id, *empty, clip));
        assert(!store.uploadSourceCurrent(pane.id, *empty, &root));
        composer->setVisible(false);
        store.sync();
        assert(!store.uploadSourceCurrent(pane.id, *empty, clip));
        composer->setVisible(true);
        store.sync();
        assert(store.uploadSourceCurrent(pane.id, *empty, clip));
        // Raw hidden IRC bytes are still a nonempty document.
        input->setValue("\x02");
        store.sync();
        assert(!clip->visible() && !clip->inputArea()->enabled());
        assert(!store.uploadSourceCurrent(pane.id, *empty, clip));
        assert(store.insertUpload(*empty, "https://example.test/file") == Result::Changed);
        input->setValue("");
        input->textInputApplyEdit({.commitText = {},
                                   .preeditText = "猫",
                                   .preeditCursorBegin = 3,
                                   .preeditCursorEnd = 3,
                                   .hasPreedit = true});
        store.sync();
        const auto preedit = input->editingState();
        assert(input->composing() && !clip->visible());
        assert(!store.uploadSourceCurrent(pane.id, *empty, clip));
        assert(!store.uploadSourceCurrent(pane.id, *empty, input));
        assert(store.insertUpload(*empty, "https://example.test/file", true) == Result::Composing);
        assert(input->editingStateMatches(preedit));
        input->textInputResetPreedit();
        input->setValue("abcXYZ");
        auto selected = input->editingState();
        selected.current.selectionAnchor = 3;
        selected.current.cursorPos = 6;
        input->restoreEditingState(selected);
        store.sync();
        // Image-paste source eligibility is independent of the empty-only paperclip.
        const auto imageSource = store.captureUploadOrigin(pane.id);
        assert(imageSource && !clip->visible() &&
               store.uploadSourceCurrent(pane.id, *imageSource, input) &&
               !store.uploadSourceCurrent(pane.id, *imageSource, clip) &&
               !store.uploadSourceCurrent(pane.id + 1, *imageSource, input));
        auto movedCaret = input->editingState();
        movedCaret.current.cursorPos = 4;
        input->restoreEditingState(movedCaret);
        assert(!store.uploadSourceCurrent(pane.id, *imageSource, input));
        input->restoreEditingState(selected);
        store.sync();
        const auto composingSource = store.captureUploadOrigin(pane.id);
        input->textInputApplyEdit({.commitText = {},
                                   .preeditText = "猫",
                                   .preeditCursorBegin = 3,
                                   .preeditCursorEnd = 3,
                                   .hasPreedit = true});
        const auto selectedPreedit = input->editingState();
        assert(!store.uploadSourceCurrent(pane.id, *composingSource, input) &&
               store.insertUpload(*composingSource, "image-url") == Result::Composing &&
               input->editingStateMatches(selectedPreedit));
        input->textInputResetPreedit();
        input->restoreEditingState(selected);
        store.sync();
        const auto nativeSource = store.captureUploadOrigin(pane.id);
        const auto nativeBefore = input->editingState();
        const auto nativeUndoSize = nativeBefore.history->undo.size();
        assert(store.insertUpload(*nativeSource, "image-url") == Result::Inserted);
        assert(input->value() == "abcimage-url" &&
               uploads.buffers.at("source").draft == input->value() &&
               input->editingState().history->undo.size() == nativeUndoSize + 1);
        assert(store.execute("undo", pane) &&
               input->editingState().current == nativeBefore.current);
        const auto origin = store.captureUploadOrigin(pane.id);
        assert(origin);
        const auto undoSize = input->editingState().history->undo.size();
        pane.buffer = "other";
        store.sync();
        assert(!store.uploadSourceCurrent(pane.id, *origin, clip));
        input->textInputApplyEdit({.commitText = {},
                                   .preeditText = "別",
                                   .preeditCursorBegin = 3,
                                   .preeditCursorEnd = 3,
                                   .hasPreedit = true});
        const auto otherPreedit = input->editingState();
        assert(store.insertUpload(*origin, "https://example.test/file") == Result::Inserted);
        assert(uploads.buffers.at("source").draft == "abchttps://example.test/file" &&
               input->editingStateMatches(otherPreedit) &&
               dispatcher.focusedArea() == input->inputArea() && !pane.focusComposer);
        input->textInputResetPreedit();
        pane.buffer = "source";
        store.sync();
        assert(input->editingState().history->undo.size() == undoSize + 1);
        assert(store.execute("undo", pane));
        assert(input->value() == "abcXYZ");
        pane.focusComposer = false;
        const auto edited = store.captureUploadOrigin(pane.id);
        assert(input->insertText("new"));
        store.sync();
        assert(store.insertUpload(*edited, "url") == Result::Changed);
        input->selectAll();
        const auto recoverySelection = input->editingState();
        assert(store.insertUpload(*edited, "url", true) == Result::Changed);
        assert(input->editingStateMatches(recoverySelection));
        input->clearSelection();
        assert(store.insertUpload(*edited, "url", true) == Result::Inserted);
        assert(input->value().ends_with("newurl"));
        // An identity-preserving rename remains a valid original document.
        input->setValue("");
        store.sync();
        const auto renamed = store.captureUploadOrigin(pane.id);
        auto node = uploads.buffers.extract("source");
        node.key() = "renamed";
        node.mapped().id = "renamed";
        uploads.buffers.insert(std::move(node));
        pane.buffer = "renamed";
        store.remapBuffer("source", "renamed", pane.id);
        store.sync();
        assert(store.uploadOriginBuffer(*renamed) == "renamed");
        assert(store.insertUpload(*renamed, "url") == Result::Inserted);
        input->setValue(std::string(ui_logic::composerLimit, 'x'));
        store.sync();
        const auto limited = store.captureUploadOrigin(pane.id);
        const auto full = input->editingState();
        assert(store.insertUpload(*limited, "url") == Result::Invalid);
        assert(input->editingStateMatches(full));
        uploads.buffers.erase("renamed");
        assert(store.insertUpload(*limited, "url", true) == Result::Missing);
        assert(!store.uploadSourceCurrent(pane.id, *limited, clip));
        store.sync();
        uploads.buffers["renamed"].id = "renamed";
        uploads.buffers["renamed"].draft = "";
        store.sync();
        assert(store.insertUpload(*limited, "url", true) == Result::Missing);
        const auto merged = store.captureUploadOrigin(pane.id);
        assert(merged);
        uploads.buffers.at("other").draft = "merged";
        uploads.buffers.erase("renamed");
        pane.buffer = "other";
        store.remapBuffer("renamed", "other", pane.id);
        store.sync();
        assert(store.insertUpload(*merged, "url", true) == Result::Missing);
        // Destination exists in the model but has never had a native composer.
        input->setValue("");
        store.sync();
        const auto unopenedMerge = store.captureUploadOrigin(pane.id);
        assert(unopenedMerge);
        uploads.buffers["unopened"].id = "unopened";
        store.prepareBufferRename("other", "unopened");
        uploads.buffers.erase("other");
        pane.buffer = "unopened";
        store.remapBuffer("other", "unopened", pane.id);
        store.sync();
        assert(input->value().empty());
        assert(store.insertUpload(*unopenedMerge, "url") == Result::Missing);
        assert(store.insertUpload(*unopenedMerge, "url", true) == Result::Missing);
        dispatcher.setFocus(nullptr);
        dispatcher.setSceneRoot(nullptr);
    }
    // One parsed document supplies display bytes, raw protocol offsets and trusted markup.
    const auto normalFg = rgbHex(0xe0e0e0), normalBg = rgbHex(0x101010);
    const auto parse = [&](std::string_view raw) {
        return parseIrcFormatting(raw, normalFg, normalBg);
    };
    {
        const std::string raw = "\x02"
                                "A猫\t"
                                "\x03"
                                "04,02B\x0f<&>'\"\n";
        const auto doc = parse(raw);
        assert(doc.text == "A猫    B<&>'\"\n" && doc.rawToDisplay.size() == raw.size() + 1);
        assert(doc.rawToDisplay.front() == 0 && doc.rawToDisplay.back() == doc.text.size() &&
               std::is_sorted(doc.rawToDisplay.begin(), doc.rawToDisplay.end()));
        assert(doc.rawToDisplay[1] == 0 && doc.rawToDisplay[2] == 1 && doc.rawToDisplay[3] == 1 &&
               doc.rawToDisplay[4] == 1 && doc.rawToDisplay[5] == 4 && doc.rawToDisplay[6] == 8);
        const auto colorEnd = raw.find('B');
        for (size_t offset = 6; offset <= colorEnd; ++offset)
            assert(doc.rawToDisplay[offset] == 8);
        assert(doc.styleAt(0) == IrcStyle{} && doc.styleAt(1).bold &&
               !doc.styleAt(colorEnd - 1).foreground);
        assert(doc.styleAt(colorEnd).bold &&
               doc.styleAt(colorEnd).foreground == rgbHex(ircPalette[4]) &&
               doc.styleAt(colorEnd).background == rgbHex(ircPalette[2]));
        assert(doc.finalStyle == IrcStyle{} && doc.runs.size() == 3);
        const auto markup = ircMarkup(doc), contrast = ircMarkup(doc, false);
        for (const auto *escaped : {"&lt;", "&amp;", "&gt;", "&apos;", "&quot;"})
            assert(markup.contains(escaped) && contrast.contains(escaped));
        assert(markup.contains("weight=\"bold\"") && markup.contains("foreground=") &&
               markup.contains("background=") && contrast.contains("weight=\"bold\"") &&
               !contrast.contains("foreground=") && !contrast.contains("background="));
    }
    for (const int number : {0, 15, 16, 98, 99}) {
        const auto code = (number < 10 ? "0" : "") + std::to_string(number);
        const auto doc = parse("\x03" + code + "x");
        assert(doc.text == "x" && doc.runs.size() == 1);
        assert(doc.finalStyle.foreground == (number == 99
                                                 ? std::optional<Color>{}
                                                 : std::optional(rgbHex(ircPalette[number % 16]))));
    }
    {
        const auto doc = parse("\x03"
                               "04,02a\x03"
                               "99b\x03"
                               "03,99c\x03"
                               "d");
        assert(doc.text == "abcd" && doc.runs.size() == 4);
        assert(doc.runs[0].style.foreground == rgbHex(ircPalette[4]) &&
               doc.runs[0].style.background == rgbHex(ircPalette[2]));
        assert(!doc.runs[1].style.foreground &&
               doc.runs[1].style.background == rgbHex(ircPalette[2]));
        assert(doc.runs[2].style.foreground == rgbHex(ircPalette[3]) &&
               !doc.runs[2].style.background && doc.runs[3].style == IrcStyle{});
        assert(parse("\x03"
                     "04,x")
                       .text == ",x" &&
               parse("\x03"
                     "123")
                       .text == "3");
    }
    {
        const auto doc = parse("\x04"
                               "123aBc,DeF456x\x04"
                               "abcdefy\x04z");
        assert(doc.text == "xyz" && doc.runs.size() == 3);
        assert(doc.runs[0].style.foreground == rgbHex(0x123abc) &&
               doc.runs[0].style.background == rgbHex(0xdef456));
        assert(doc.runs[1].style.foreground == rgbHex(0xabcdef) &&
               doc.runs[1].style.background == rgbHex(0xdef456));
        assert(doc.finalStyle == IrcStyle{});
        assert(parse("\x04"
                     "12345x")
                       .text == "12345x" &&
               parse("\x04"
                     "112233,xyz")
                       .text == ",xyz");
    }
    {
        const auto doc = parse("\x16"
                               "a\x16"
                               "b\x03"
                               "04,02\x16"
                               "c");
        assert(doc.text == "abc" && doc.runs.size() == 3);
        assert(doc.runs[0].style.reverse && doc.runs[0].style.foreground == normalBg &&
               doc.runs[0].style.background == normalFg);
        assert(doc.runs[1].style == IrcStyle{});
        assert(doc.runs[2].style.foreground == rgbHex(ircPalette[2]) &&
               doc.runs[2].style.background == rgbHex(ircPalette[4]));
        const auto decorations = parse("\x02\x1d\x1f\x1e\x11"
                                       "x\x0f"
                                       "y");
        const auto &style = decorations.runs.front().style;
        assert(style.bold && style.italic && style.underline && style.strike && style.monospace &&
               decorations.runs.back().style == IrcStyle{} && decorations.finalStyle == IrcStyle{});
        assert(ircMarkup(decorations).contains("font_family=\"monospace\"") &&
               ircMarkup(decorations).contains("strikethrough=\"true\""));
    }
    for (const auto &[raw, literal] : {std::pair{"\x03"
                                                 "4",
                                                 ",12literal"},
                                       {"\x04"
                                        "123456",
                                        ",ABCDEF"}}) {
        const auto style = parse(raw).finalStyle;
        const auto encoded = ircFormattingCodes(style);
        const auto restored = parse(encoded + literal);
        assert(restored.text == literal && restored.finalStyle == style &&
               !restored.finalStyle.background);
    }
    Model model;
    model.settings.typingSend = false;
    model.settings.reducedMotion = true;
    model.settings.cursorShape = "box";
    model.settings.cursorEffect = "trail";
    ServerConfig server;
    server.name = "test";
    server.host = "example.invalid";
    server.alias = "Private test network";
    model.servers.push_back(server);
    model.networks["test"].name = "Private test network";
    model.networks["test"].caps.insert("message-tags");
    auto &buffer = model.buffers["test/room"];
    buffer.id = "test/room";
    buffer.server = "test";
    buffer.target = "#room";
    buffer.draft = "draft";
    Message target;
    target.id = "authoritative-reply-token";
    target.nick = "猫";
    target.text = "\x02visible context\x02 " + std::string(4096, 'x');
    buffer.messages.push_back(target);
    const auto replyKey = ui_logic::messageKey(target);
    auto &other = model.buffers["test/other"];
    other.id = "test/other";
    other.server = "test";
    other.target = "#other";
    Theme theme;
    theme.update(model);
    DeferredClipboard clipboard;
    InputDispatcher dispatcher;
    EditorStore editors(
        model, clipboard, [](auto) {}, [](auto) {},
        [&](Node &affected) { retireComposerPointer(dispatcher, affected); });
    PaneState a{.id = 1, .buffer = buffer.id, .reply = replyKey, .focusComposer = false};
    PaneState b{.id = 2, .buffer = buffer.id, .reply = "missing-reply", .focusComposer = false};
    Node root;
    auto *first = root.addChild(editors.composer(a));
    auto *second = root.addChild(editors.composer(b));
    auto *input = composerInput(*first);
    auto *shared = composerInput(*second);
    assert(input && shared && input != shared);
    auto *caret = nativeCaret(*input);
    assert(caret);
    const auto originalDraft = input->editingState();
    const auto originalIme = input->textInputState();
    for (const auto *shape : {"line", "underline", "box"}) {
        model.settings.cursorShape = shape;
        editors.sync();
        assert(input->editingStateMatches(originalDraft));
        const auto ime = input->textInputState();
        assert(ime.cursor == originalIme.cursor && ime.anchor == originalIme.anchor &&
               ime.cursorRectWidth == originalIme.cursorRectWidth &&
               ime.cursorRectHeight == originalIme.cursorRectHeight);
    }
    model.settings.bindings["send"] = "C-RET";
    model.settings.bindings["newline"] = "M-RET";
    model.settings.bindings["send"] = "M-s";
    dispatcher.setSceneRoot(&root);
    editors.setInputDispatcher(dispatcher);
    dispatcher.setFocus(input->inputArea());
    {
        MessageMenuHost menu;
        input->setPopupContext(&menu);
        const auto openMenu = [&] {
            input->inputArea()->dispatchKey(XKB_KEY_Menu, 0, 0, true);
            input->inputArea()->dispatchKey(XKB_KEY_Menu, 0, 0, false);
            assert(menu.open && !menu.request.inputSerial);
        };
        const auto selectRaw = [&] {
            input->setValue("A\x02"
                            "猫\x02Z");
            auto selection = input->editingState();
            selection.current.selectionAnchor = 1;
            selection.current.cursorPos = 6;
            input->restoreEditingState(selection);
            editors.sync();
            return input->editingState();
        };
        auto selectedRaw = selectRaw();
        openMenu();
        for (const auto *label : {"Cut", "Copy", "Paste", "Select all", "Formatting"})
            assert(std::ranges::count_if(menu.request.menu, [&](const auto &entry) {
                       return entry.label == label;
                   }) == 1);
        for (const auto *label : {"Cut", "Copy", "Paste", "Select all", "Formatting"})
            assert(menu.entry(label));
        assert(menu.request.menu[0].label == "Undo" && menu.request.menu[1].label == "Redo");
        for (const auto *label : {"Bold", "Italic", "Underline", "Strikethrough", "Monospace",
                                  "Reverse", "Reset formatting"})
            assert(menu.entry(std::string("Formatting/") + label));
        for (const auto *group : {"Foreground color", "Background color"})
            for (int color = 0; color != 16; ++color)
                assert(menu.entry(std::string(group) + "/IRC color " + (color < 10 ? "0" : "") +
                                  std::to_string(color)));
        const auto undoCount = selectedRaw.history->undo.size();
        menu.select("Copy");
        assert(clipboard.copied == "\x02"
                                   "猫\x02" &&
               input->editingStateMatches(selectedRaw) &&
               selectedRaw.history->undo.size() == undoCount);
        openMenu();
        input->inputArea()->dispatchPress(2, 3, BTN_RIGHT, true, 137.75f, 219.25f, 701, 7010);
        input->inputArea()->dispatchPress(2, 3, BTN_RIGHT, false, 137.75f, 219.25f, 702, 7020);
        assert(menu.open && menu.request.anchorX == 137 && menu.request.anchorY == 219 &&
               menu.request.anchorWidth == 1 && menu.request.anchorHeight == 1 &&
               menu.request.inputSerial == 701 && input->editingStateMatches(selectedRaw));
        const auto cutIndex = menu.entry("Cut")->value;
        auto repeatCut = menu.callbacks.onSelect;
        menu.select("Cut");
        repeatCut(cutIndex);
        assert(input->value() == "AZ" &&
               clipboard.copied == "\x02"
                                   "猫\x02" &&
               input->editingState().history->undo.size() == undoCount + 1);
        openMenu();
        assert(menu.entry("Undo"));
        menu.select("Undo");
        assert(input->editingState().current == selectedRaw.current);
        openMenu();
        assert(menu.entry("Redo"));
        menu.select("Redo");
        assert(input->value() == "AZ");
        selectedRaw = selectRaw();
        openMenu();
        const auto pasteUndo = selectedRaw.history->undo.size();
        const auto requests = clipboard.requests;
        const auto pasteIndex = menu.entry("Paste")->value;
        auto repeatPaste = menu.callbacks.onSelect;
        menu.select("Paste");
        repeatPaste(pasteIndex);
        assert(clipboard.requests == requests + 1);
        clipboard.deliver("X");
        editors.sync();
        assert(input->value() == "AXZ" &&
               input->editingState().history->undo.size() == pasteUndo + 1);
        assert(editors.execute("undo", a) && input->editingState().current == selectedRaw.current);
        selectedRaw = selectRaw();
        openMenu();
        const auto formatUndo = selectedRaw.history->undo.size();
        const auto formatIndex = menu.entry("Formatting/Italic")->value;
        auto repeatFormat = menu.callbacks.onSelect;
        menu.select("Formatting/Italic");
        repeatFormat(formatIndex);
        assert(parse(input->value()).text == "A猫Z" && parse(input->value()).runs[1].style.italic &&
               input->editingState().history->undo.size() == formatUndo + 1);
        assert(editors.execute("undo", a) && input->editingState().current == selectedRaw.current);
        // Build real menus against the installed display projection, including an empty range.
        input->setValue("plain");
        input->clearSelection();
        editors.sync();
        const auto plain = input->editingState();
        const auto plainUndo = plain.history->undo.size();
        openMenu();
        assert(menu.entry("Formatting/Bold")->toggleState == 0);
        menu.select("Formatting/Bold");
        assert(input->value() != plain.current.value && parse(input->value()).text == "plain" &&
               parse(input->value()).styleAt(input->cursorPosition()).bold &&
               input->editingState().history->undo.size() == plainUndo + 1);
        assert(editors.execute("undo", a) && input->editingState().current == plain.current);
        for (const bool allBold : {true, false}) {
            input->setValue(allBold ? "\x02"
                                      "ab\x02"
                                    : "\x02"
                                      "a\x02"
                                      "b");
            input->selectAll();
            editors.sync();
            const auto before = input->editingState();
            const auto undo = before.history->undo.size();
            openMenu();
            assert(menu.entry("Formatting/Bold")->toggleState == (allBold ? 1 : 0));
            menu.select("Formatting/Bold");
            const auto toggled = parse(input->value());
            assert(toggled.text == "ab" && input->editingState().history->undo.size() == undo + 1);
            for (const auto &run : toggled.runs)
                assert(run.style.bold == !allBold);
            assert(editors.execute("undo", a) && input->editingState().current == before.current);
        }
        // Menu commands must never retarget a newer selection or source document.
        for (const int mutation : {0, 1, 2}) {
            selectedRaw = selectRaw();
            openMenu();
            const auto cut = menu.entry("Cut")->value;
            auto stale = menu.callbacks.onSelect;
            menu.closeSelectDropdown();
            if (mutation == 1) {
                a.buffer = other.id;
                editors.sync();
            } else if (mutation == 2) {
                buffer.draft = "external unsynced replacement";
            } else {
                assert(editors.execute("backward-char", a));
            }
            const auto before = input->editingState();
            const auto undo = before.history->undo;
            stale(cut);
            assert(input->editingStateMatches(before) && before.history->undo == undo);
            if (mutation == 2)
                assert(buffer.draft == "external unsynced replacement");
            a.buffer = buffer.id;
            editors.sync();
        }
        selectRaw();
        input->textInputApplyEdit({.commitText = {},
                                   .preeditText = "日本",
                                   .preeditCursorBegin = 0,
                                   .preeditCursorEnd = 6,
                                   .hasPreedit = true});
        const auto preedit = input->editingState();
        const auto undo = preedit.history->undo, redo = preedit.history->redo;
        openMenu();
        for (const auto *label : {"Undo", "Redo", "Cut", "Paste", "Select all", "Formatting",
                                  "Foreground color", "Background color"})
            assert(!menu.entry(label));
        assert(menu.entry("Copy"));
        menu.select("Copy");
        assert(input->composing() && input->editingStateMatches(preedit) &&
               preedit.history->undo == undo && preedit.history->redo == redo);
        menu.closeSelectDropdown();
        input->textInputResetPreedit();
        input->setPopupContext(nullptr);
        input->setValue("draft");
        editors.sync();
    }
    auto *actions = tooltipAction(*first, "Formatting toolbar");
    assert(actions);
    activateTooltip(*first, "Formatting toolbar");
    editors.sync();
    // Activate the native toolbar itself; every action is one shared raw-document undo.
    for (const auto &[title, byte] : {std::pair{"Bold", '\x02'},
                                      {"Italic", '\x1d'},
                                      {"Underline", '\x1f'},
                                      {"Strikethrough", '\x1e'},
                                      {"Monospace", '\x11'},
                                      {"Reverse", '\x16'}}) {
        assert(editors.execute("select-all", a));
        const auto before = input->editingState();
        const auto undoCount = before.history->undo.size();
        activateTooltip(*first, title);
        editors.sync();
        if (byte != '\x16')
            assert(input->value() == std::string(1, byte) + "draft" + byte);
        const auto doc = parse(input->value());
        IrcStyle expected;
        expected.bold = byte == '\x02';
        expected.italic = byte == '\x1d';
        expected.underline = byte == '\x1f';
        expected.strike = byte == '\x1e';
        expected.monospace = byte == '\x11';
        if (byte == '\x16') {
            expected.reverse = true;
            expected.foregroundSource = {IrcColor::Kind::Background};
            expected.backgroundSource = {IrcColor::Kind::Foreground};
            // Composer uses its real palette, not this parser test's supplied defaults.
            const auto projected = input->displayProjection();
            assert(projected && projected->markup.contains("foreground=") &&
                   projected->markup.contains("background="));
            expected.foreground = normalBg;
            expected.background = normalFg;
        }
        assert(doc.text == "draft" && doc.runs.size() == 1 && doc.runs.front().style == expected &&
               doc.finalStyle == IrcStyle{} && shared->value() == input->value() &&
               input->editingState().history->undo.size() == undoCount + 1);
        assert(editors.execute("undo", a) && input->value() == "draft");
        editors.sync();
        assert(input->editingState().current == before.current &&
               shared->value() == input->value());
    }
    for (const bool background : {false, true})
        for (size_t color = 0; color < ircPalette.size(); ++color) {
            const auto code = (color < 10 ? "0" : "") + std::to_string(color);
            assert(editors.execute("select-all", a));
            const auto undoCount = input->editingState().history->undo.size();
            activateTooltip(*first, std::string(background ? "Background" : "Foreground") +
                                        " IRC color " + code);
            editors.sync();
            const auto doc = parse(input->value());
            assert(doc.text == "draft" && doc.runs.size() == 1 && doc.finalStyle == IrcStyle{} &&
                   shared->value() == input->value());
            const auto &style = doc.runs.front().style;
            assert((background ? style.background : style.foreground) == rgbHex(ircPalette[color]));
            assert(!(background ? style.foreground : style.background) &&
                   input->editingState().history->undo.size() == undoCount + 1);
            assert(editors.execute("undo", a) && input->value() == "draft");
            editors.sync();
        }
    for (const auto &[raw, title] : {std::pair{"\x02", "Bold"},
                                     {"\x03"
                                      "04",
                                      "Reset formatting"}}) {
        input->setValue(raw);
        input->selectAll();
        const auto undoCount = input->editingState().history->undo.size();
        activateTooltip(*first, title);
        editors.sync();
        const auto modeOnly = parse(input->value());
        assert(input->value() != raw && modeOnly.text.empty() &&
               modeOnly.finalStyle == IrcStyle{} &&
               input->editingState().history->undo.size() == undoCount + 1);
        assert(editors.execute("undo", a) && input->value() == raw);
    }
    // Reset strips internal changes too, retaining actual raw tabs and the suffix.
    input->setValue("a\x02"
                    "b\t\x1d"
                    "c\x0f"
                    "de");
    auto formatting = input->editingState();
    formatting.current.selectionAnchor = 2;
    formatting.current.cursorPos = 8;
    input->restoreEditingState(formatting);
    activateTooltip(*first, "Reset formatting");
    editors.sync();
    assert(parse(input->value()).text == "ab    cde" && input->value().contains('\t'));
    for (const auto &run : parse(input->value()).runs)
        assert(run.style == IrcStyle{});
    assert(editors.execute("undo", a) && input->value() == formatting.current.value);
    input->setValue("\x04"
                    "AB\x02\x02"
                    "CDEF");
    formatting = input->editingState();
    formatting.current.selectionAnchor = 2;
    formatting.current.cursorPos = input->value().size();
    input->restoreEditingState(formatting);
    activateTooltip(*first, "Reset formatting");
    editors.sync();
    assert(parse(input->value()).text == "ABCDEF" &&
           parse(input->value()).finalStyle == IrcStyle{});
    assert(editors.execute("undo", a) && input->value() == formatting.current.value);
    input->setValue("\x02"
                    "a");
    const auto selectedAll = input->editingState();
    input->selectAll();
    assert(input->insertText("a"));
    assert(input->value() == "a" && parse(input->value()).finalStyle == IrcStyle{});
    assert(editors.execute("undo", a) && input->value() == selectedAll.current.value);
    // Native replacement crosses an old restore marker: inserted text inherits left style,
    // but the untouched suffix retains its original right style.
    input->setValue("a\x02"
                    "bc\x0f"
                    "de");
    formatting = input->editingState();
    formatting.current.selectionAnchor = 2;
    formatting.current.cursorPos = 6;
    input->restoreEditingState(formatting);
    assert(input->insertText("X"));
    const auto replaced = parse(input->value());
    assert(replaced.text == "aXe" && replaced.runs.size() == 3 && replaced.runs[1].style.bold &&
           replaced.runs.back().style == IrcStyle{});
    assert(editors.execute("undo", a) && input->value() == formatting.current.value);
    input->setValue("\x03"
                    "4");
    const auto openNumeric = input->editingState();
    const auto numericUndo = openNumeric.history->undo.size();
    assert(input->insertText("2"));
    const auto sealedNumeric = parse(input->value());
    assert(sealedNumeric.text == "2" &&
           sealedNumeric.finalStyle.foregroundSource == (IrcColor{IrcColor::Kind::Indexed, 4}) &&
           input->editingState().history->undo.size() == numericUndo + 1);
    assert(editors.execute("undo", a) && input->value() == openNumeric.current.value);
    input->setValue("\x04"
                    "ABxCDEF");
    auto openHex = input->editingState();
    openHex.current.cursorPos = openHex.current.selectionAnchor = 4;
    input->restoreEditingState(openHex);
    const auto hexUndo = openHex.history->undo.size();
    input->inputArea()->dispatchKey(XKB_KEY_BackSpace, 0, 0, true);
    input->inputArea()->dispatchKey(XKB_KEY_BackSpace, 0, 0, false);
    const auto sealedHex = parse(input->value());
    assert(sealedHex.text == "ABCDEF" && sealedHex.finalStyle == IrcStyle{} &&
           input->editingState().history->undo.size() == hexUndo + 1);
    assert(editors.execute("undo", a) && input->value() == openHex.current.value);
    for (const auto &[before, fragment, display] :
         {std::array<std::string_view, 3>{"12foo", "\x03", "12foo"},
          std::array<std::string_view, 3>{"2foo",
                                          "\x03"
                                          "4",
                                          "2foo"},
          std::array<std::string_view, 3>{"CDEFtext",
                                          "\x04"
                                          "AB",
                                          "ABCDEFtext"}}) {
        input->setValue(before);
        auto untouchedSuffix = input->editingState();
        untouchedSuffix.current.cursorPos = untouchedSuffix.current.selectionAnchor = 0;
        input->restoreEditingState(untouchedSuffix);
        const auto undoCount = untouchedSuffix.history->undo.size();
        assert(input->insertText(fragment));
        const auto sealed = parse(input->value());
        assert(sealed.text == display && sealed.runs.back().style == IrcStyle{} &&
               input->editingState().history->undo.size() == undoCount + 1);
        if (fragment == "\x03"
                        "4")
            assert(sealed.styleAt(input->cursorPosition()).foreground == rgbHex(ircPalette[4]));
        assert(editors.execute("undo", a) && input->value() == before);
    }
    for (const bool hexForeground : {false, true}) {
        const auto prefix = hexForeground ? std::string("\x04"
                                                        "123456")
                                          : std::string{};
        const std::string literal = hexForeground ? ",ABCDEF" : ",12literal";
        input->setValue(prefix + "suffix");
        formatting = input->editingState();
        formatting.current.cursorPos = formatting.current.selectionAnchor = prefix.size();
        input->restoreEditingState(formatting);
        activateTooltip(*first,
                        hexForeground ? "Background IRC color 02" : "Foreground IRC color 04");
        editors.sync();
        assert(input->insertText(literal));
        const auto colored = parse(input->value());
        assert(colored.text == literal + "suffix" && colored.runs.size() >= 2 &&
               colored.runs.front().style.foreground ==
                   (hexForeground ? rgbHex(0x123456) : rgbHex(ircPalette[4])) &&
               !colored.runs.back().style.background);
        if (hexForeground)
            assert(colored.runs.front().style.background == rgbHex(ircPalette[2]) &&
                   colored.runs.back().style.foreground == rgbHex(0x123456));
        else
            assert(colored.runs.back().style == IrcStyle{});
    }
    // Hidden closing metadata is still a real suffix whose original final style survives.
    input->setValue("\x02"
                    "a\x02");
    formatting = input->editingState();
    formatting.current.cursorPos = formatting.current.selectionAnchor = 2;
    input->restoreEditingState(formatting);
    activateTooltip(*first, "Bold");
    editors.sync();
    assert(input->insertText("X"));
    const auto hiddenSuffix = parse(input->value());
    assert(hiddenSuffix.text == "aX" && hiddenSuffix.runs.front().style.bold &&
           !hiddenSuffix.runs.back().style.bold && hiddenSuffix.finalStyle == IrcStyle{});
    // A caret-only toggle must restore an already-colored right-hand run after typing.
    input->setValue("a\x03"
                    "04,02b");
    formatting = input->editingState();
    formatting.current.cursorPos = formatting.current.selectionAnchor = 1;
    input->restoreEditingState(formatting);
    activateTooltip(*first, "Bold");
    editors.sync();
    assert(input->insertText("X"));
    const auto balanced = parse(input->value());
    assert(balanced.text == "aXb" && balanced.runs.size() == 3 && balanced.runs[1].style.bold &&
           !balanced.runs.back().style.bold &&
           balanced.runs.back().style.foreground == rgbHex(ircPalette[4]) &&
           balanced.runs.back().style.background == rgbHex(ircPalette[2]));
    const auto wire = input->value();
    assert(wire == "a\x02"
                   "X\x02\x03"
                   "04,02b");
    activateTooltip(*first, "Send message");
    assert(model.actions.size() == 1 && model.actions.front().type == "send" &&
           model.actions.front().text == wire && model.actions.front().buffer == buffer.id &&
           model.actions.front().extra == target.id);
    model.actions.clear();
    a.reply = replyKey;
    input->setValue("");
    activateTooltip(*first, "Bold");
    editors.sync();
    assert(!input->value().empty() && parse(input->value()).text.empty());
    const auto formatOnly = input->value();
    auto *formatOnlySend = tooltipAction(*first, "Send message");
    assert(formatOnlySend);
    formatOnlySend->inputArea()->dispatchKey(XKB_KEY_Return, 0, 0, true);
    formatOnlySend->inputArea()->dispatchKey(XKB_KEY_Return, 0, 0, false);
    assert(editors.execute("send", a));
    assert(model.actions.empty() && input->value() == formatOnly && a.reply == replyKey);
    // Wayland's XKB Return event carries CR; zero UTF32 is a non-text key.
    input->inputArea()->dispatchKey(XKB_KEY_Return, '\r', KeyMod::Shift, true);
    input->inputArea()->dispatchKey(XKB_KEY_Return, '\r', KeyMod::Shift, false);
    editors.sync();
    assert(parse(input->value()).text == "\n");
    const auto formatOnlyLine = input->value();
    formatOnlySend->inputArea()->dispatchKey(XKB_KEY_Return, 0, 0, true);
    formatOnlySend->inputArea()->dispatchKey(XKB_KEY_Return, 0, 0, false);
    assert(editors.execute("send", a) && model.actions.empty() &&
           input->value() == formatOnlyLine && a.reply == replyKey);
    input->setValue("draft");
    input->clearSelection();
    input->inputArea()->dispatchKey(XKB_KEY_End, 0, KeyMod::Ctrl, true);
    input->inputArea()->dispatchKey(XKB_KEY_End, 0, KeyMod::Ctrl, false);
    editors.flush();
    activateTooltip(*first, "Formatting toolbar");
    editors.sync();

    // Shared native document/history, but pane-local reply and viewport scroll.
    auto state = input->editingState();
    state.scrollY = 100;
    input->restoreEditingState(state);
    state.scrollY = 20;
    shared->restoreEditingState(state);
    editors.flush();
    assert(input->insertText("猫😀אב"));
    assert(input->value() == shared->value());
    assert(input->editingState().history == shared->editingState().history);
    assert(input->editingState().scrollY == 100 && shared->editingState().scrollY == 20);
    assert(editors.execute("undo", b));
    assert(input->value() == "draft" && shared->value() == "draft");
    assert(a.reply == replyKey && b.reply == "missing-reply");
    // Consume Undo's deliberate pane-B focus request before starting pane-A IME.
    editors.sync();
    assert(dispatcher.focusedArea() == shared->inputArea() && !b.focusComposer);
    dispatcher.setFocus(input->inputArea());
    assert(dispatcher.focusedArea() == input->inputArea() && !a.focusComposer && !b.focusComposer);

    // Word kills remove visible words, not their hidden boundary formatting.
    for (const auto raw : {"\x02"
                           "hello\x02",
                           "\x02"
                           "猫\x02"})
        for (const bool forward : {false, true}) {
            input->setValue(raw);
            auto before = input->editingState();
            before.current.cursorPos = before.current.selectionAnchor =
                forward ? 0 : before.current.value.size();
            input->restoreEditingState(before);
            editors.flush();
            assert(editors.execute(forward ? "kill-word" : "backward-kill-word", a));
            assert(input->value() == "\x02\x02" && parse(input->value()).text.empty() &&
                   parse(input->value()).finalStyle == IrcStyle{});
            assert(editors.execute("undo", a) && input->editingState().current == before.current);
        }
    // An empty word range retains both the old kill buffer and an active mark.
    input->setValue("saved");
    input->selectAll();
    assert(editors.execute("kill-region", a));
    input->setValue("\x02"
                    "hello\x02");
    assert(editors.execute("set-mark", a));
    const auto emptyKillBefore = input->editingState();
    const auto emptyKillUndo = emptyKillBefore.history->undo;
    const auto emptyKillRedo = emptyKillBefore.history->redo;
    std::vector<std::uint64_t> emptyKillNotifications;
    editors.setOnEditingStateChanged(
        [&](std::uint64_t source) { emptyKillNotifications.push_back(source); });
    assert(editors.execute("kill-word", a) && input->editingStateMatches(emptyKillBefore) &&
           emptyKillBefore.history->undo == emptyKillUndo &&
           emptyKillBefore.history->redo == emptyKillRedo && emptyKillNotifications.empty());
    editors.setOnEditingStateChanged({});
    assert(editors.execute("backward-char", a));
    assert(input->editingState().current.selectionAnchor == emptyKillBefore.current.cursorPos &&
           input->editingState().current.cursorPos < emptyKillBefore.current.cursorPos);
    assert(editors.execute("yank", a) && parse(input->value()).text == "hellsaved");
    editors.sync();

    // Replies retain the raw native document; dispatch owns the live tag-or-prefix decision.
    {
        const std::string raw = "\x03"
                                "04,02A猫B\x0fZ";
        const auto selectDraft = [&] {
            input->setValue(raw);
            auto state = input->editingState();
            state.current.selectionAnchor = 7;
            state.current.cursorPos = 10;
            input->restoreEditingState(state);
            editors.sync();
        };
        auto &network = model.networks.at("test");
        const auto checkSend = [&](std::string expectedId, bool dropCaps = false) {
            selectDraft();
            const auto before = input->editingState();
            const auto undoCount = before.history->undo.size();
            assert(displays(*first, "Replying to") && input->value() == raw);
            if (dropCaps)
                network.caps.erase("message-tags");
            assert(editors.execute("send", a) && model.actions.size() == 1);
            const auto &sent = model.actions.front();
            assert(sent.type == "send" && sent.buffer == buffer.id && sent.text == raw &&
                   sent.extra == expectedId && sent.attributes.at("reply-nick") == target.nick &&
                   a.reply.empty() && input->value().empty() && shared->value().empty() &&
                   input->editingState().history->undo.size() == undoCount + 1);
            model.actions.clear();
            assert(editors.execute("undo", a) && input->editingState().current == before.current);
            if (dropCaps)
                network.caps.insert("message-tags");
        };
        a.reply = replyKey;
        checkSend(target.id); // Capable connection.
        a.reply = replyKey;
        checkSend(target.id, true); // Capability disappears after the quote is already visible.
        network.caps.erase("message-tags");
        a.reply = replyKey;
        checkSend(target.id); // Capability absent at Send still carries fallback metadata.
        network.caps.insert("message-tags");
        network.clientTagDeny = "*";
        a.reply = replyKey;
        checkSend(target.id); // Tag denial must not discard the author.
        network.clientTagDeny.clear();
        buffer.messages.front().id = "local:reply-source";
        a.reply = ui_logic::messageKey(buffer.messages.front());
        checkSend({});
        buffer.messages.front() = target;
        buffer.messages.front().own = buffer.messages.front().pending = true;
        buffer.messages.front().label = "reply-source-label";
        a.reply = ui_logic::messageKey(buffer.messages.front());
        const auto echoKey = a.reply;
        checkSend({});
        // A pending echo's selection key survives acknowledgement with a real wire id.
        buffer.messages.front().pending = false;
        buffer.messages.front().id = "acknowledged-id";
        a.reply = echoKey;
        checkSend("acknowledged-id");
        buffer.messages.front() = target;
        model.settings.fools.insert(target.nick);
        a.reply = replyKey;
        selectDraft();
        assert(displays(*first, "Message hidden") && !displays(*first, "visible context"));
        checkSend(target.id); // A masked source supplies only its author, never its body.
        model.settings.fools.erase(target.nick);
        buffer.messages.front().deleted = true;
        a.reply = replyKey;
        checkSend(target.id); // Redacted body is never copied into outgoing text.
        buffer.messages.front() = target;
        const auto checkCommand = [&](std::string parent) {
            a.reply = std::move(parent);
            input->setValue("/me waves");
            editors.sync();
            assert(editors.execute("send", a) && model.actions.size() == 1 &&
                   model.actions.front().type == "command" &&
                   model.actions.front().text == "/me waves" &&
                   model.actions.front().extra.empty() && model.actions.front().attributes.empty());
            model.actions.clear();
        };
        checkCommand(replyKey);
        checkCommand("message:missing-source");
        buffer.messages.front().nick.clear();
        checkCommand(replyKey);
        buffer.messages.front() = target;
        a.reply = replyKey;
        selectDraft();
        const auto before = input->editingState();
        const auto undo = before.history->undo, redo = before.history->redo;
        buffer.messages.front().nick.clear();
        assert(editors.execute("send", a) && model.actions.empty() &&
               input->editingStateMatches(before) && a.reply == replyKey &&
               before.history->undo == undo && before.history->redo == redo);
        buffer.messages.front() = target;
        a.reply = "message:missing-source";
        assert(editors.execute("send", a) && model.actions.empty() &&
               input->editingStateMatches(before) && a.reply == "message:missing-source");
        a.reply = replyKey;
        editors.sync();
        activateComposerAction(*first, ":reply-cancel");
        editors.sync();
        assert(dispatcher.focusedArea() == input->inputArea() && !a.focusComposer);
        assert(a.reply.empty() && input->editingStateMatches(before) &&
               before.history->undo == undo && before.history->redo == redo);
        a.reply = replyKey;
        input->textInputApplyEdit({.commitText = {},
                                   .preeditText = "日本😀",
                                   .preeditCursorBegin = 10,
                                   .preeditCursorEnd = 10,
                                   .hasPreedit = true});
        const auto ime = input->editingState();
        const auto imeUndo = ime.history->undo, imeRedo = ime.history->redo;
        assert(ime.preeditBase && editors.execute("send", a) && model.actions.empty() &&
               input->composing() && input->editingStateMatches(ime) &&
               ime.history->undo == imeUndo && ime.history->redo == imeRedo && a.reply == replyKey);
        input->textInputResetPreedit();
    }

    // Native and asynchronous edits report their actual source after publishing the draft.
    {
        input->setValue("\x03"
                        "04,02A猫B\x0fZ");
        editors.flush();
        std::vector<std::uint64_t> sources;
        editors.setOnEditingStateChanged([&](std::uint64_t source) {
            assert(source == a.id || source == b.id);
            const auto state = (source == a.id ? input : shared)->editingState();
            const auto &base = state.preeditBase ? *state.preeditBase : state.current;
            assert(buffer.draft == base.value);
            sources.push_back(source);
        });
        dispatcher.keyEvent(XKB_KEY_Left, 0, 0, true);
        dispatcher.keyEvent(XKB_KEY_Left, 0, 0, false);
        assert(sources == std::vector<std::uint64_t>{a.id});
        sources.clear();
        for (const auto sym : {XKB_KEY_Home, XKB_KEY_End}) {
            dispatcher.keyEvent(sym, 0, 0, true);
            dispatcher.keyEvent(sym, 0, 0, false);
            assert(sources == std::vector<std::uint64_t>{a.id});
            assert(input->editingState().current == shared->editingState().current &&
                   input->cursorPosition() == (sym == XKB_KEY_Home ? 0 : input->value().size()) &&
                   input->selectionAnchor() == input->cursorPosition());
            sources.clear();
        }
        input->selectAll();
        assert(sources == std::vector<std::uint64_t>{a.id});
        sources.clear();
        assert(input->insertText("native edit"));
        assert(sources == std::vector<std::uint64_t>{a.id});
        sources.clear();
        dispatcher.setFocus(shared->inputArea());
        shared->selectAll();
        assert(sources == std::vector<std::uint64_t>{b.id});
        sources.clear();
        shared->textInputApplyEdit({.commitText = {},
                                    .preeditText = "日本",
                                    .preeditCursorBegin = 6,
                                    .preeditCursorEnd = 6,
                                    .hasPreedit = true});
        assert(sources == std::vector<std::uint64_t>{b.id});
        const auto fullPreedit = shared->editingState();
        sources.clear();
        shared->textInputApplyEdit({.commitText = "日",
                                    .preeditText = "本",
                                    .preeditCursorBegin = 3,
                                    .preeditCursorEnd = 3,
                                    .hasCommitText = true,
                                    .hasPreedit = true});
        const auto partialCommit = shared->editingState();
        assert(partialCommit.current.value == fullPreedit.current.value &&
               partialCommit.preeditBase != fullPreedit.preeditBase &&
               sources == std::vector<std::uint64_t>{b.id});
        sources.clear();
        editors.sync();
        editors.flush();
        assert(sources.empty() && input->editingState().current == partialCommit.current);
        auto viewport = shared->editingState();
        viewport.scrollY += 17;
        shared->restoreEditingState(viewport);
        editors.flush();
        editors.sync();
        assert(sources.empty());
        shared->textInputResetPreedit();
        sources.clear();
        buffer.draft = "recovered draft";
        editors.sync();
        assert(sources.empty() && buffer.draft == "recovered draft\n日" &&
               input->value() == "recovered draft\n日" && shared->value() == "recovered draft\n日");
        const auto pasteBefore = shared->editingState().current;
        auto expectedPaste = pasteBefore.value;
        const auto start = std::min(pasteBefore.cursorPos, pasteBefore.selectionAnchor);
        expectedPaste.replace(
            start, std::max(pasteBefore.cursorPos, pasteBefore.selectionAnchor) - start, "!");
        assert(editors.execute("paste", b) && clipboard.pending && sources.empty());
        clipboard.deliver("!");
        assert(sources == std::vector<std::uint64_t>{b.id} && input->value() == expectedPaste &&
               shared->value() == expectedPaste && buffer.draft == expectedPaste);
        sources.clear();
        editors.sync();
        editors.flush();
        assert(sources.empty());
        editors.setOnEditingStateChanged({});
        dispatcher.setFocus(input->inputArea());
    }

    // Direct composer commands must not replace the committed base beneath IME.
    input->setValue("cut text");
    input->selectAll();
    assert(editors.execute("kill-region", a));
    input->setValue("");
    assert(input->insertText("\x03"
                             "04,02A猫B\x0fZ"));
    input->selectAll();
    assert(input->insertText("temporary") && editors.execute("undo", a));
    auto pasteBase = input->editingState();
    pasteBase.current.selectionAnchor = 7;
    pasteBase.current.cursorPos = 10;
    input->restoreEditingState(pasteBase);
    editors.sync();
    const auto requestsBeforeIme = clipboard.requests;
    assert(editors.execute("paste", a) && clipboard.requests == requestsBeforeIme + 1 &&
           clipboard.pending);
    input->textInputApplyEdit({.commitText = {},
                               .preeditText = "日本😀",
                               .preeditCursorBegin = 10,
                               .preeditCursorEnd = 10,
                               .hasPreedit = true});
    const auto pasteIme = input->editingState();
    const auto pasteUndo = pasteIme.history->undo, pasteRedo = pasteIme.history->redo;
    assert(pasteIme.preeditBase && !pasteUndo.empty() && !pasteRedo.empty() &&
           parse(pasteIme.current.value).text == "A日本😀BZ");
    auto *directCancelPaste = composerAction(*first, ":paste-cancel");
    assert(directCancelPaste);
    const auto directPasteUnchanged = [&] {
        assert(input->composing() && input->editingStateMatches(pasteIme) &&
               pasteIme.history->undo == pasteUndo && pasteIme.history->redo == pasteRedo &&
               dispatcher.focusedArea() == input->inputArea() &&
               !nativeVisible(*directCancelPaste) && model.actions.empty() &&
               clipboard.requests == requestsBeforeIme + 1);
    };
    for (const auto *command : {"paste", "yank"}) {
        assert(editors.execute(command, a));
        editors.sync();
        directPasteUnchanged();
    }
    clipboard.deliver("late\nclipboard");
    editors.sync();
    directPasteUnchanged();
    input->textInputResetPreedit();
    input->setValue("draft");
    editors.sync();

    // Budget, font/zoom/style and effective trail suppression are paint/layout only.
    auto selected = input->editingState();
    selected.current.cursorPos = 4;
    selected.current.selectionAnchor = 1;
    input->restoreEditingState(selected);
    editors.flush();
    static_cast<TextInputClient &>(*input).textInputApplyEdit({.commitText = {},
                                                               .preeditText = "日本😀",
                                                               .preeditCursorBegin = 10,
                                                               .preeditCursorEnd = 10,
                                                               .hasPreedit = true});
    const auto composing = input->editingState();
    assert(composing.preeditBase && composing.preeditBase->value == "draft" &&
           composing.preeditBase->cursorPos == 4 && composing.preeditBase->selectionAnchor == 1);
    const auto undoBeforeLayout = composing.history->undo;
    const auto redoBeforeLayout = composing.history->redo;
    for (const float scale : {.7f, 1.f, 2.f}) {
        model.settings.scale = scale;
        model.settings.messageFont = "monospace";
        theme.update(model);
        editors.setComposerHeightLimit(a.id, 80 * scale);
        editors.sync();
        assert(dispatcher.focusedArea() == input->inputArea());
        assert(input->editingStateMatches(composing));
        assert(composing.history->undo == undoBeforeLayout &&
               composing.history->redo == redoBeforeLayout);
        assert(shared->composing() && a.reply == replyKey && b.reply == "missing-reply");
        assert(model.settings.cursorShape == "box" && model.settings.cursorEffect == "trail");
    }
    editors.setComposerHeightLimit(a.id, 0);
    assert(input->editingStateMatches(composing));
    assert(editors.execute("cancel", a));
    assert(!input->composing() && a.reply == replyKey);
    editors.sync();
    assert(displays(*first, "猫") && displays(*first, "visible context"));
    assert(!displays(*first, target.id));
    // Excerpts retain formatted source text; checkComposerReplyCard exercises native two-line
    // clipping.
    model.settings.fools.insert(target.nick);
    for (const auto display : {FoolDisplay::Minimized, FoolDisplay::Blurred, FoolDisplay::Hidden}) {
        model.settings.foolDisplay = display;
        editors.sync();
        assert(displays(*first, "Message hidden") && !displays(*first, "visible context") &&
               buffer.messages.front().text == target.text && a.reply == replyKey);
    }
    bool replyRevealed = false;
    const auto secondReply = b.reply;
    b.reply = replyKey;
    editors.setReplyReadable([&](std::uint64_t paneId, const std::string &messageId) {
        return replyRevealed && paneId == a.id && messageId == replyKey;
    });
    for (const auto display : {FoolDisplay::Minimized, FoolDisplay::Blurred}) {
        model.settings.foolDisplay = display;
        editors.sync();
        assert(!displays(*first, "visible context"));
        replyRevealed = true;
        editors.sync();
        assert(displays(*first, "visible context") && !displays(*second, "visible context") &&
               buffer.messages.front().text == target.text && a.reply == replyKey);
        replyRevealed = false;
        editors.sync();
        assert(displays(*first, "Message hidden") && !displays(*first, "visible context"));
    }
    editors.setReplyReadable({});
    b.reply = secondReply;
    model.settings.foolDisplay = FoolDisplay::Dimmed;
    editors.sync();
    assert(displays(*first, "visible context"));
    model.settings.foolDisplay = FoolDisplay::Hidden;
    model.settings.bufferFools[buffer.id] = true;
    editors.sync();
    assert(displays(*first, "visible context"));
    model.settings.bufferFools[buffer.id] = false;
    editors.setReplyReadable([](auto, const auto &) { return true; });
    editors.sync();
    assert(displays(*first, "Message hidden") && !displays(*first, "visible context"));
    {
        auto &mentions = model.buffers[GlobalMentionsBufferId];
        mentions.id = GlobalMentionsBufferId;
        mentions.kind = "mentions";
        mentions.target = "Mentions";
        mentions.draft = "/quit must not reach a synthetic server";
        const bool originalMention = buffer.messages.front().mention;
        buffer.messages.front().mention = true;
        auto aggregate = buffer.messages.front();
        aggregate.id = "aggregate-reply-token";
        aggregate.sourceBuffer = buffer.id;
        aggregate.sourceId = target.id;
        aggregate.sourceServer = buffer.server;
        aggregate.sourceTarget = buffer.target;
        mentions.messages.push_back(aggregate);
        assert(resolveMessageSource(model, mentions, mentions.messages.front()) == &buffer);
        const auto document = input->editingState();
        a.buffer = mentions.id;
        a.reply = ui_logic::messageKey(aggregate);
        model.settings.bufferFools[mentions.id] = true;
        editors.sync();
        assert(!input->enabled() && displays(*first, "Message hidden") &&
               !displays(*first, "visible context"));
        const auto requests = clipboard.requests;
        editors.execute("send", a);
        editors.execute("paste", a);
        editors.execute("insert:blocked", a);
        assert(model.actions.empty() && clipboard.requests == requests &&
               mentions.draft == "/quit must not reach a synthetic server" &&
               !editors.captureUploadOrigin(a.id));
        model.settings.bufferFools[buffer.id] = true;
        editors.sync();
        assert(displays(*first, "visible context"));
        mentions.messages.front().sourceId = "missing-source";
        editors.sync();
        assert(!displays(*first, "visible context") && displays(*first, "Unavailable message"));
        a.buffer = buffer.id;
        a.reply = replyKey;
        editors.sync();
        assert(input->enabled() && input->editingStateMatches(document));
        model.settings.bufferFools.erase(mentions.id);
        model.buffers.erase(mentions.id);
        buffer.messages.front().mention = originalMention;
    }
    editors.setReplyReadable({});
    model.settings.bufferFools.erase(buffer.id);
    model.settings.fools.erase(target.nick);
    model.settings.foolDisplay = FoolDisplay::Minimized;
    buffer.messages.front().deleted = true;
    editors.sync();
    assert(!displays(*first, "visible context") && a.reply == replyKey);
    buffer.messages.clear();
    editors.sync();
    assert(a.reply == replyKey && !displays(*first, target.id));

    const std::string clipboardText = "\x02one\x02\r\n\r\n/quit\r\n猫😀\r\n \r\nlast\r\n";
    const auto normalized = ui_logic::clipboardNewlines(clipboardText);
    const auto draftBefore = input->value();
    auto paste = [&] {
        assert(editors.execute("paste", a));
        clipboard.deliver(clipboardText);
        editors.sync();
        assert(model.actions.empty() && input->value() == draftBefore && a.reply == replyKey);
        assert(displays(*first, "#room") && displays(*first, "Private test network"));
        assert(displays(*first, "one") && !displays(*first, std::string("\x02one")));
        assert(composerAction(*first, ":paste-send"));
    };
    paste();
    activateTooltip(*first, "Formatting toolbar");
    editors.sync();
    assert(composerAction(*first, ":paste-cancel") && composerAction(*first, ":paste-insert") &&
           composerAction(*first, ":paste-send"));
    activateTooltip(*first, "Formatting toolbar");
    editors.sync();
    activateComposerAction(*first, ":paste-cancel");
    assert(model.actions.empty() && input->value() == draftBefore && a.reply == replyKey);
    paste();
    activateComposerAction(*first, ":paste-send");
    const std::vector<std::string> lines{"\x02one\x02", "/quit", "猫😀", " ", "last"};
    assert(model.actions.size() == lines.size());
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto &sent = model.actions[i];
        assert(sent.type == "send" && sent.buffer == buffer.id && sent.text == lines[i] &&
               sent.extra.empty());
    }
    assert(input->value() == draftBefore && shared->value() == draftBefore && a.reply == replyKey);
    model.actions.clear();
    paste();
    assert(editors.execute("select-all", a));
    activateComposerAction(*first, ":paste-insert");
    assert(input->value() == normalized && shared->value() == normalized && a.reply == replyKey);
    assert(model.actions.empty());
    assert(editors.execute("undo", b) && input->value() == draftBefore);

    // A delayed clipboard response cannot silently use a different caret/destination.
    assert(editors.execute("paste", a));
    assert(editors.execute("backward-char", a));
    clipboard.deliver("!");
    editors.sync();
    assert(input->value() == draftBefore && model.actions.empty());
    activateComposerAction(*first, ":paste-cancel");
    assert(editors.execute("paste", a));
    a.buffer = other.id;
    clipboard.deliver("wrong destination");
    assert(other.draft.empty() && buffer.draft == draftBefore && model.actions.empty());
    editors.sync();
    a.buffer = buffer.id;
    editors.sync();

    // Malformed payload admission stays all-or-nothing, including delayed confirmation.
    assert(editors.execute("paste", a));
    assert(editors.execute("backward-char", a));
    clipboard.deliver(std::string("bad\0text", 8));
    editors.sync();
    activateComposerAction(*first, ":paste-insert");
    assert(input->value() == draftBefore && model.actions.empty());
    assert(editors.execute("cancel", a) && a.reply == replyKey);
    assert(editors.execute("paste", a));
    clipboard.deliver(std::string(ui_logic::composerLimit, 'x') + "\nmore");
    editors.sync();
    activateComposerAction(*first, ":paste-insert");
    assert(input->value() == draftBefore && model.actions.empty() && a.reply == replyKey);
    assert(!displays(*first, std::string(300, 'x')));
    assert(editors.execute("cancel", a));

    // An already-open confirmation follows the synchronous native identity migration.
    paste();
    const auto document = input->editingState();
    const auto oldId = buffer.id;
    const std::string renamed = "test/renamed";
    auto node = model.buffers.extract(oldId);
    node.key() = renamed;
    node.mapped().id = renamed;
    node.mapped().target = "#renamed";
    model.buffers.insert(std::move(node));
    a.buffer = b.buffer = renamed;
    editors.remapBuffer(oldId, renamed, a.id);
    editors.sync();
    assert(input->editingStateMatches(document) && a.reply == replyKey);
    activateComposerAction(*first, ":paste-send");
    assert(model.actions.size() == lines.size());
    for (const auto &sent : model.actions)
        assert(sent.buffer == renamed && sent.extra.empty());
    model.actions.clear();
    root.setSize(800, 600);
    editors.setOverlayHost(&root);
    auto *completion = completionPopup(root);
    assert(completion);
    const auto openCompletion = [&](std::string_view raw, Input &active) {
        active.setValue(raw);
        dispatcher.setFocus(active.inputArea());
        editors.sync();
        assert(completion->visible() && dispatcher.focusedArea() == active.inputArea());
    };
    for (const auto &prefix : {std::string("\x02"), std::string("\x03"
                                                                "04")}) {
        openCompletion(prefix + ":smle", *input);
        const auto undoCount = input->editingState().history->undo.size();
        assert(editors.handleCompletionKey(XKB_KEY_Return, 0));
        editors.sync();
        assert(input->value() == prefix + "😄" && shared->value() == input->value() &&
               input->editingState().history->undo.size() == undoCount + 1 &&
               model.actions.empty() && !completion->visible());
        assert(editors.execute("undo", a));
        editors.sync();
        assert(input->value() == prefix + ":smle");
    }
    openCompletion(":smle", *input);
    auto viewportOnly = input->editingState();
    viewportOnly.scrollX += 2;
    viewportOnly.scrollY += 17;
    input->restoreEditingState(viewportOnly);
    editors.sync();
    assert(completion->visible() && editors.handleCompletionKey(XKB_KEY_Return, 0));
    editors.sync();
    assert(input->value() == "😄" && shared->value() == "😄" && model.actions.empty());
    input->setValue("before :smle: after");
    auto insideToken = input->editingState();
    insideToken.current.cursorPos = insideToken.current.selectionAnchor = 10;
    input->restoreEditingState(insideToken);
    editors.sync();
    assert(completion->visible() && editors.handleCompletionKey(XKB_KEY_Return, 0));
    editors.sync();
    assert(input->value() == "before 😄 after" && shared->value() == input->value());
    assert(editors.execute("undo", a));
    editors.sync();
    assert(input->editingState().current == insideToken.current && model.actions.empty());
    openCompletion(":", *input);
    assert(editors.handleCompletionKey(XKB_KEY_Down, 0) &&
           editors.handleCompletionKey(XKB_KEY_Up, 0) && input->value() == ":");
    assert(editors.handleCompletionKey(XKB_KEY_Tab, 0));
    editors.sync();
    assert(input->value() == "😄" && shared->value() == "😄" && model.actions.empty());
    assert(editors.execute("undo", a) && input->value() == ":");
    openCompletion(":sm", *input);
    assert(editors.handleCompletionKey(XKB_KEY_Escape, 0));
    editors.sync();
    assert(input->value() == ":sm" && !completion->visible() &&
           !editors.handleCompletionKey(XKB_KEY_Return, 0));
    assert(input->insertText("le"));
    editors.sync();
    assert(completion->visible() && editors.handleCompletionKey(XKB_KEY_Return, 0));
    editors.sync();
    assert(input->value() == "😄" && model.actions.empty());
    for (const auto *literal : {"https://example.invalid/:smle", "12:34"}) {
        input->setValue(literal);
        editors.sync();
        assert(!completion->visible() && !editors.handleCompletionKey(XKB_KEY_Return, 0) &&
               input->value() == literal);
    }
    openCompletion(":smle", *input);
    auto selectedCompletion = input->editingState();
    selectedCompletion.current.selectionAnchor = 0;
    input->restoreEditingState(selectedCompletion);
    editors.sync();
    selectedCompletion = input->editingState();
    assert(!completion->visible());
    for (const auto key : {XKB_KEY_Up, XKB_KEY_Down, XKB_KEY_Tab, XKB_KEY_Return, XKB_KEY_Escape,
                           XKB_KEY_Left, XKB_KEY_Right}) {
        assert(!editors.handleCompletionKey(key, 0) &&
               !editors.handleCompletionKey(key, KeyMod::Ctrl) &&
               input->editingStateMatches(selectedCompletion));
    }
    input->clearSelection();
    input->textInputApplyEdit({.commitText = {},
                               .preeditText = "猫",
                               .preeditCursorBegin = 3,
                               .preeditCursorEnd = 3,
                               .hasPreedit = true});
    editors.sync();
    const auto completionPreedit = input->editingState();
    assert(input->composing() && !completion->visible());
    for (const auto key : {XKB_KEY_Up, XKB_KEY_Down, XKB_KEY_Tab, XKB_KEY_Return, XKB_KEY_Escape,
                           XKB_KEY_Left, XKB_KEY_Right})
        assert(!editors.handleCompletionKey(key, 0) &&
               input->editingStateMatches(completionPreedit));
    input->textInputResetPreedit();
    openCompletion(":smle", *shared);
    assert(editors.handleCompletionKey(XKB_KEY_Return, 0));
    editors.sync();
    assert(input->value() == "😄" && shared->value() == "😄" &&
           dispatcher.focusedArea() == shared->inputArea() && model.actions.empty());
    openCompletion(":smle", *shared);
    ScrollView *completionScroll = nullptr;
    for (const auto &child : completion->children())
        if (auto *scroll = dynamic_cast<ScrollView *>(child.get()))
            completionScroll = scroll;
    assert(completionScroll && !completionScroll->content()->children().empty());
    auto *heldRow =
        dynamic_cast<InputArea *>(completionScroll->content()->children().front().get());
    assert(heldRow && heldRow->passiveTouchPress());
    heldRow->dispatchPress(1, 1, BTN_LEFT, true);
    assert(heldRow->pressed());
    const std::string emojiRenamed = "test/emoji-renamed";
    auto emojiNode = model.buffers.extract(renamed);
    emojiNode.key() = emojiRenamed;
    emojiNode.mapped().id = emojiRenamed;
    model.buffers.insert(std::move(emojiNode));
    a.buffer = b.buffer = emojiRenamed;
    editors.remapBuffer(renamed, emojiRenamed, a.id);
    editors.sync();
    assert(!heldRow->pressed() && input->value() == ":smle" && shared->value() == ":smle" &&
           model.actions.empty());
    model.buffers.erase(emojiRenamed);
    editors.sync();
    assert(!completion->visible() && !editors.handleCompletionKey(XKB_KEY_Return, 0) &&
           model.actions.empty());
    editors.setOverlayHost(nullptr);
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

// Invoked separately as composer-native in the private Wayland/EGL smoke environment.
void runComposerNativeCheck() {
    using namespace blast;
    codecs::installImageDecoder();
    auto *display = wl_display_connect(nullptr);
    assert(display);
    {
        GlSharedContext shared;
        shared.initialize(display);
        registerNativeFonts();
        RenderContext context;
        context.initialize(shared);
        ScaledRenderer renderer(context, 1.f);
        checkIndependentMessageScales(renderer);
        checkComposerReplyCard(renderer);
        checkComposerEditingCommands(renderer);
        checkComposerSpellcheck(renderer);
        checkComposerColors(renderer);
        {
            emoji::Catalog catalog;
            emoji::CompletionView completion;
            const auto matches = catalog.matches("");
            assert(matches.size() == emoji::maxResults);
            completion.setMatches(matches, matches.size() - 1);
            TextFont completionFont{};
            completionFont.family = "sans-serif";
            completion.setAnchor({40, 400, 240, 40}, {0, 0, 720, 600}, 1, completionFont);
            completion.setVisible(true);
            completion.layout(renderer);
            ScrollView *scroll = nullptr;
            for (const auto &child : completion.children())
                if (auto *view = dynamic_cast<ScrollView *>(child.get()))
                    scroll = view;
            assert(scroll && scroll->content()->children().size() == matches.size());
            auto *selected = dynamic_cast<InputArea *>(scroll->content()->children().back().get());
            assert(selected && selected->passiveTouchPress() && nativeVisible(*selected));
            within(*selected, completion);
            const float generousHeight = completion.height();
            completion.setAnchor({40, 180, 200, 40}, {0, 0, 240, 220}, 1, completionFont);
            completion.layout(renderer);
            assert(completion.height() < generousHeight && scroll->scrollable() &&
                   scroll->scrollOffset() > 0);
            within(*selected, completion); // Selected last result follows an overlay shrink.
            completionFont.family = "monospace";
            completion.setAnchor({40, 180, 200, 40}, {0, 0, 240, 220}, 1.5f, completionFont);
            completion.layout(renderer);
            assert(scroll->scrollable() && scroll->scrollOffset() > 0);
            within(*selected, completion); // Font/zoom restyling also keeps the row revealed.
        }
        // Real composer consumers: command completion, native projection and contextual ghost.
        {
            Model model;
            auto &buffer = model.buffers["test/#room"];
            buffer.id = "test/#room";
            buffer.server = "test";
            buffer.target = "#room";
            buffer.kind = "channel";
            model.networks["test"].channelTypes = "#&+!";
            Theme theme;
            theme.update(model);
            DeferredClipboard clipboard;
            InputDispatcher dispatcher;
            EditorStore editors(
                model, clipboard, [](auto) {}, [](auto) {},
                [&](Node &node) { retireComposerPointer(dispatcher, node); });
            PaneState pane{};
            pane.id = 1;
            pane.buffer = buffer.id;
            Node root;
            root.setSize(640, 480);
            auto *composer = root.addChild(editors.composer(pane));
            auto *input = composerInput(*composer);
            assert(input);
            dispatcher.setSceneRoot(&root);
            editors.setInputDispatcher(dispatcher);
            editors.setOverlayHost(&root);
            dispatcher.setFocus(input->inputArea());
            command_completion::View *popup = nullptr;
            for (const auto &child : root.children())
                if (auto *view = dynamic_cast<command_completion::View *>(child.get()))
                    popup = view;
            assert(popup);
            const auto frame = [&] {
                editors.sync();
                composer->arrange(renderer, {20, 350, 600, 100});
                composer->layout(renderer);
                popup->layout(renderer);
            };
            // Native input dispatch prepares layout before typing can anchor a popup.
            frame();
            auto *emojiPopup = completionPopup(root);
            assert(emojiPopup && static_cast<Node *>(emojiPopup) != static_cast<Node *>(popup));
            input->setValue("/ki");
            frame();
            assert(popup->visible());
            assert(!emojiPopup->visible());
            const auto before = input->editingState();
            const auto undoBefore = before.history->undo.size();
            assert(editors.handleCompletionKey(XKB_KEY_Tab, 0));
            frame();
            assert(input->value() == "/kick " && input->displayProjection()->text == "/kick ");
            assert(input->editingState().history->undo.size() == undoBefore + 1 &&
                   input->editingState().history->undo.back() == before.current);
            const float nativeHeight = input->height();
            auto *ghost = contextText(*input, "nick");
            assert(ghost && nativeVisible(*ghost));
            const auto &projection = *input->displayProjection();
            auto *commandGlyphs = contextText(*input, "/kick");
            assert(commandGlyphs);
            const auto glyphs = renderer.measureText(
                projection.markup, commandGlyphs->fontSize(), commandGlyphs->font(), 600, 1,
                commandGlyphs->textAlign(), commandGlyphs->ellipsize(), true);
            assert(glyphs.layoutHeight > 0 && glyphs.inkRight > glyphs.inkLeft);
            input->setValue("/kick somebody");
            frame();
            assert(input->height() == nativeHeight && (!ghost || !nativeVisible(*ghost)));
            input->setValue("/kick #other ");
            frame();
            assert(nativeVisible(*ghost) && input->value() == "/kick #other ");
            input->setValue("/whois");
            buffer.kind = "query";
            frame();
            assert(!nativeVisible(*ghost));
            buffer.kind = "server";
            frame();
            assert(nativeVisible(*ghost)); // Context changes without a document edit.
            auto selected = input->editingState();
            selected.current.selectionAnchor = 0;
            input->restoreEditingState(selected);
            frame();
            assert(!nativeVisible(*ghost) && !popup->visible() &&
                   !editors.handleCompletionKey(XKB_KEY_Return, 0));
            input->clearSelection();
            auto middle = input->editingState();
            middle.current.cursorPos = middle.current.selectionAnchor = 2;
            input->restoreEditingState(middle);
            frame();
            assert(!nativeVisible(*ghost));
            input->setValue("/kick ");
            input->textInputApplyEdit({.commitText = {},
                                       .preeditText = "あ",
                                       .preeditCursorBegin = 3,
                                       .preeditCursorEnd = 3,
                                       .hasPreedit = true});
            frame();
            assert(input->composing() && !nativeVisible(*ghost) && !popup->visible() &&
                   !editors.handleCompletionKey(XKB_KEY_Tab, 0));
            input->textInputResetPreedit();
            input->setValue("/ni");
            frame();
            assert(popup->visible());
            ScrollView *scroll = nullptr;
            for (const auto &child : popup->children())
                if (auto *view = dynamic_cast<ScrollView *>(child.get()))
                    scroll = view;
            assert(scroll && !scroll->content()->children().empty());
            auto *held = dynamic_cast<InputArea *>(scroll->content()->children().front().get());
            assert(held);
            held->dispatchPress(1, 1, BTN_LEFT, true);
            assert(held->pressed());
            input->setValue("/jo");
            frame();
            assert(!held->pressed() && input->value() == "/jo");
            assert(editors.handleCompletionKey(XKB_KEY_Escape, 0));
            frame();
            assert(!popup->visible() && !editors.handleCompletionKey(XKB_KEY_Return, 0));
            auto &other = model.buffers["test/#other"];
            other.id = "test/#other";
            other.server = "test";
            other.target = "#other";
            other.kind = "channel";
            other.draft = "/jo";
            pane.buffer = other.id;
            frame();
            assert(popup->visible() && input->value() == "/jo");
            held->dispatchPress(1, 1, BTN_LEFT, true);
            assert(held->pressed());
            pane.buffer = buffer.id;
            frame();
            assert(!held->pressed() && buffer.draft == "/jo" && other.draft == "/jo");
            input->setValue("/my-server-extension arg");
            frame();
            assert(!popup->visible() && input->value() == "/my-server-extension arg");
            buffer.kind = "channel";
            buffer.nicks = {{.name = "Alice", .account = {}, .prefix = {}},
                            {.name = "Alfred", .account = {}, .prefix = {}},
                            {.name = "Alice", .account = {}, .prefix = {}},
                            {.name = "猫友", .account = {}, .prefix = {}}};
            other.nicks = {{.name = "AlElsewhere", .account = {}, .prefix = {}}};
            auto &foreign = model.buffers["foreign/#outside"];
            foreign.id = "foreign/#outside";
            foreign.server = "foreign";
            foreign.target = "#outside";
            foreign.kind = "channel";
            const auto openExplicit = [&](std::string_view text) {
                input->setValue(text);
                frame();
                const auto before = input->editingState();
                const auto undo = before.history->undo, redo = before.history->redo;
                assert(editors.execute("complete", pane));
                frame();
                assert(input->editingState().current == before.current &&
                       before.history->undo == undo && before.history->redo == redo &&
                       dispatcher.focusedArea() == input->inputArea() && model.actions.empty());
            };
            const auto rows = [&]() -> Node * {
                for (const auto &child : popup->children())
                    if (auto *view = dynamic_cast<ScrollView *>(child.get()))
                        return view->content();
                return nullptr;
            };
            const auto visibleRows = [&] {
                assert(rows());
                return std::count_if(rows()->children().begin(), rows()->children().end(),
                                     [](const auto &row) { return row->visible(); });
            };
            // Ordinary nick typing stays quiet; even a unique explicit match needs acceptance.
            input->setValue("Ali");
            frame();
            assert(!popup->visible());
            openExplicit("aLi");
            assert(popup->visible() && visibleRows() == 1);
            const auto unique = input->editingState();
            const auto uniqueUndo = unique.history->undo.size();
            assert(editors.handleCompletionKey(XKB_KEY_Return, 0));
            frame();
            assert(input->value() == "Alice: " && !popup->visible() && model.actions.empty() &&
                   dispatcher.focusedArea() == input->inputArea() &&
                   input->editingState().history->undo.size() == uniqueUndo + 1);
            assert(editors.execute("undo", pane));
            frame();
            assert(input->editingState().current == unique.current);
            openExplicit("aL");
            assert(popup->visible() && visibleRows() == 2);
            assert(editors.handleCompletionKey(XKB_KEY_Down, 0));
            assert(input->value() == "aL");
            assert(editors.handleCompletionKey(XKB_KEY_Up, 0));
            assert(editors.handleCompletionKey(XKB_KEY_Tab, 0));
            frame();
            assert(input->value() == "Alfred: " && model.actions.empty());
            openExplicit("AlE");
            assert(!popup->visible() && !editors.handleCompletionKey(XKB_KEY_Tab, 0) &&
                   input->value() == "AlE"); // Nicks from another buffer are not candidates.
            openExplicit("#out");
            assert(!popup->visible() && input->value() == "#out"); // Nor another server's targets.
            for (const auto &[prefix, accepted] :
                 {std::pair{"#ot", "#other"}, std::pair{"&lo", "&local"},
                  std::pair{"/ki", "/kick "}}) {
                if (std::string_view(prefix).starts_with("&")) {
                    auto &local = model.buffers["test/&local"];
                    local.id = "test/&local";
                    local.server = "test";
                    local.target = "&local";
                    local.kind = "channel";
                }
                openExplicit(prefix);
                assert(popup->visible());
                assert(editors.handleCompletionKey(XKB_KEY_Tab, 0));
                frame();
                assert(input->value() == accepted && model.actions.empty());
            }
            // Replacement stops at the caret, retaining Unicode and hidden IRC bytes after it.
            const std::string head = "\x03"
                                     "04猫\x0f ";
            const std::string tail = "ce\x02 日本😀";
            input->setValue(head + "Ali" + tail);
            auto middleNick = input->editingState();
            middleNick.current.cursorPos = middleNick.current.selectionAnchor = head.size() + 3;
            input->restoreEditingState(middleNick);
            frame();
            middleNick = input->editingState();
            const auto middleUndo = middleNick.history->undo.size();
            assert(editors.execute("complete", pane));
            frame();
            assert(popup->visible() && input->editingState().current == middleNick.current);
            assert(editors.handleCompletionKey(XKB_KEY_Tab, 0));
            frame();
            assert(input->value() == head + "Alice" + tail &&
                   input->cursorPosition() == head.size() + 5 &&
                   input->selectionAnchor() == input->cursorPosition() &&
                   input->editingState().history->undo.size() == middleUndo + 1);
            assert(editors.execute("undo", pane));
            frame();
            assert(input->editingState().current == middleNick.current);
            input->setValue("Alice 日本");
            auto startNick = input->editingState();
            startNick.current.cursorPos = startNick.current.selectionAnchor = 3;
            input->restoreEditingState(startNick);
            frame();
            startNick = input->editingState();
            const auto startUndo = startNick.history->undo.size();
            assert(editors.execute("complete", pane));
            frame();
            assert(popup->visible() && input->editingState().current == startNick.current);
            assert(editors.handleCompletionKey(XKB_KEY_Return, 0));
            frame();
            assert(input->value() == "Alice: ce 日本" && input->cursorPosition() == 7 &&
                   input->editingState().history->undo.size() == startUndo + 1 &&
                   model.actions.empty());
            assert(editors.execute("undo", pane));
            frame();
            assert(input->editingState().current == startNick.current);
            // A raw caret before hidden formatting must not map past that caret.
            for (const std::string &prefix : {std::string{}, std::string{"hello "}}) {
                const std::string raw = prefix + "\x02"
                                                 "Alice";
                input->setValue(raw);
                frame();
                input->inputArea()->dispatchKey(XKB_KEY_Home, 0, KeyMod::Ctrl, true);
                input->inputArea()->dispatchKey(XKB_KEY_Home, 0, KeyMod::Ctrl, false);
                if (!prefix.empty()) {
                    auto caret = input->editingState();
                    caret.current.cursorPos = caret.current.selectionAnchor = prefix.size();
                    input->restoreEditingState(caret);
                }
                frame();
                const auto beforeControl = input->editingState();
                const auto controlUndo = beforeControl.history->undo.size();
                assert(beforeControl.current.cursorPos == prefix.size() &&
                       editors.execute("complete", pane));
                frame();
                assert(popup->visible() && input->editingState().current == beforeControl.current);
                // Empty prefix sorts #other, #room and &local before Alfred.
                for (int row = 0; row != 3; ++row)
                    assert(editors.handleCompletionKey(XKB_KEY_Down, 0));
                assert(editors.handleCompletionKey(XKB_KEY_Return, 0));
                frame();
                const std::string inserted = prefix.empty() ? "Alfred: " : "Alfred";
                assert(input->value() == prefix + inserted + raw.substr(prefix.size()) &&
                       input->cursorPosition() == prefix.size() + inserted.size() &&
                       input->selectionAnchor() == input->cursorPosition() &&
                       input->editingState().history->undo.size() == controlUndo + 1 &&
                       model.actions.empty());
                assert(editors.execute("undo", pane));
                frame();
                assert(input->editingState().current == beforeControl.current);
            }
            for (const std::string &prefix :
                 {std::string{}, std::string{"hello "}, std::string{"\x02"}}) {
                const std::string suffix = "友\x0f 日本😀";
                input->setValue(prefix + "猫" + suffix);
                auto cjkNick = input->editingState();
                cjkNick.current.cursorPos = cjkNick.current.selectionAnchor = prefix.size() + 3;
                input->restoreEditingState(cjkNick);
                frame();
                cjkNick = input->editingState();
                const auto cjkUndo = cjkNick.history->undo, cjkRedo = cjkNick.history->redo;
                assert(!popup->visible() && editors.execute("complete", pane));
                frame();
                auto *candidate = contextText(*popup, "猫友");
                assert(popup->visible() && visibleRows() == 1 && candidate &&
                       nativeVisible(*candidate) && candidate->text() == "猫友" &&
                       !candidate->useMarkup() &&
                       input->editingState().current == cjkNick.current &&
                       cjkNick.history->undo == cjkUndo && cjkNick.history->redo == cjkRedo &&
                       model.actions.empty());
                assert(editors.handleCompletionKey(XKB_KEY_Tab, 0));
                frame();
                const std::string inserted = prefix == "hello " ? "猫友" : "猫友: ";
                assert(input->value() == prefix + inserted + suffix && !popup->visible() &&
                       input->cursorPosition() == prefix.size() + inserted.size() &&
                       input->selectionAnchor() == input->cursorPosition() &&
                       input->editingState().history->undo.size() == cjkUndo.size() + 1 &&
                       input->editingState().history->undo.back() == cjkNick.current &&
                       model.actions.empty() && dispatcher.focusedArea() == input->inputArea());
                assert(editors.execute("undo", pane));
                frame();
                assert(input->editingState().current == cjkNick.current);
            }
            openExplicit("Al");
            const auto escapedNick = input->editingState();
            const auto escapeUndo = escapedNick.history->undo,
                       escapeRedo = escapedNick.history->redo;
            assert(editors.handleCompletionKey(XKB_KEY_Escape, 0));
            frame();
            assert(!popup->visible() && input->editingState().current == escapedNick.current &&
                   escapedNick.history->undo == escapeUndo &&
                   escapedNick.history->redo == escapeRedo &&
                   !editors.handleCompletionKey(XKB_KEY_Return, 0));
            assert(input->insertText("i"));
            frame();
            assert(!popup->visible()); // Dismissing does not enable automatic nick popups.
            openExplicit("Al");
            input->setValue("unrelated");
            assert(!editors.handleCompletionKey(XKB_KEY_Return, 0));
            frame();
            assert(!popup->visible() && input->value() == "unrelated");
            openExplicit("Al");
            input->textInputApplyEdit({.commitText = {},
                                       .preeditText = "日本",
                                       .preeditCursorBegin = 6,
                                       .preeditCursorEnd = 6,
                                       .hasPreedit = true});
            frame();
            const auto nickIme = input->editingState();
            const auto nickImeUndo = nickIme.history->undo, nickImeRedo = nickIme.history->redo;
            assert(editors.execute("complete", pane));
            assert(!popup->visible() && !editors.handleCompletionKey(XKB_KEY_Return, 0) &&
                   input->editingStateMatches(nickIme) && nickIme.history->undo == nickImeUndo &&
                   nickIme.history->redo == nickImeRedo);
            input->textInputResetPreedit();
            openExplicit("Al");
            dispatcher.setFocus(nullptr);
            frame();
            assert(!popup->visible() && !editors.handleCompletionKey(XKB_KEY_Return, 0) &&
                   input->value() == "Al");
            dispatcher.setFocus(input->inputArea());
            frame();
            assert(!popup->visible());
            openExplicit("Ali");
            auto *nickRow = dynamic_cast<InputArea *>(rows()->children().front().get());
            assert(nickRow && nickRow->passiveTouchPress());
            for (const bool pressed : {true, false})
                nickRow->dispatchPress(1, 1, BTN_LEFT, pressed);
            frame();
            assert(input->value() == "Alice: " && !popup->visible() && model.actions.empty() &&
                   dispatcher.focusedArea() == input->inputArea());
            openExplicit("Al");
            nickRow = dynamic_cast<InputArea *>(rows()->children().front().get());
            assert(nickRow);
            nickRow->dispatchPress(1, 1, BTN_LEFT, true);
            assert(nickRow->pressed());
            input->setValue("Ali");
            frame();
            assert(!nickRow->pressed() && !popup->visible());
            nickRow->dispatchPress(1, 1, BTN_LEFT, false);
            assert(input->value() == "Ali");
            // Seed the destination through its native editor: assigning model.draft
            // after it has an editing session is external recovery, not replacement.
            pane.buffer = other.id;
            frame();
            input->setValue("Al");
            frame();
            assert(input->value() == "Al" && other.draft == "Al");
            pane.buffer = buffer.id;
            frame();
            openExplicit("Al");
            nickRow = dynamic_cast<InputArea *>(rows()->children().front().get());
            nickRow->dispatchPress(1, 1, BTN_LEFT, true);
            assert(nickRow->pressed());
            pane.buffer = other.id;
            frame();
            assert(!nickRow->pressed() && !popup->visible());
            nickRow->dispatchPress(1, 1, BTN_LEFT, false);
            assert(input->value() == "Al" && buffer.draft == "Al" && other.draft == "Al" &&
                   model.actions.empty());
            pane.buffer = buffer.id;
            frame();
            assert(!popup->visible());
            editors.setOverlayHost(nullptr);
            dispatcher.setFocus(nullptr);
            dispatcher.setSceneRoot(nullptr);
        }
        // Unicode destination separators may not create extra compact Pango paragraphs.
        {
            Model model;
            model.settings.scale = 1;
            model.settings.uiFont = "monospace";
            model.settings.reducedMotion = true;
            model.networks["test"].name = "Network\u2029second";
            auto &buffer = model.buffers["test/#room"];
            buffer.id = "test/#room";
            buffer.server = "test";
            buffer.target = "#room\u2029second\u2028third\u0085fourth";
            buffer.draft = "draft";
            Theme theme;
            theme.update(model);
            DeferredClipboard clipboard;
            InputDispatcher dispatcher;
            EditorStore editors(
                model, clipboard, [](auto) {}, [](auto) {},
                [&](Node &affected) { retireComposerPointer(dispatcher, affected); });
            PaneState pane{};
            pane.id = 1;
            pane.buffer = buffer.id;
            pane.focusComposer = false;
            Node root;
            auto *composer = root.addChild(editors.composer(pane));
            auto *input = composerInput(*composer);
            dispatcher.setSceneRoot(&root);
            editors.setInputDispatcher(dispatcher);
            assert(input);
            dispatcher.setFocus(input->inputArea());
            assert(editors.execute("paste", pane));
            clipboard.deliver("one\n\n\n\n\nlast");
            editors.sync();
            const float minimum = editors.minimumComposerHeight(pane.id, renderer, 392, 202);
            assert(minimum > 0 && minimum <= 202);
            editors.setComposerHeightLimit(pane.id, minimum);
            composer->arrange(renderer, {0, 0, 392, minimum});
            composer->layout(renderer);
            auto *contextTextNode = contextText(*composer, "Clipboard for ");
            auto *contextLabel =
                contextTextNode ? dynamic_cast<Label *>(contextTextNode->parent()) : nullptr;
            assert(contextLabel && contextLabel->fontSize() > 0 &&
                   contextLabel->font().family == model.settings.uiFont);
            const float unzoomedContextFont = contextLabel->fontSize();
            model.settings.scale = 2;
            theme.update(model);
            editors.sync();
            const float zoomMinimum = editors.minimumComposerHeight(pane.id, renderer, 392, 202);
            assert(zoomMinimum > 0 && zoomMinimum <= 202);
            editors.setComposerHeightLimit(pane.id, zoomMinimum);
            composer->arrange(renderer, {0, 0, 392, zoomMinimum});
            composer->layout(renderer);
            assert(contextLabel->fontSize() == 2 * unzoomedContextFont &&
                   contextLabel->font().family == model.settings.uiFont);
            assert(contextLabel && nativeVisible(*contextLabel) &&
                   contextLabel->text().contains("#room second third fourth") &&
                   contextLabel->text().find('\n') == std::string::npos &&
                   contextLabel->text().find("\u2028") == std::string::npos &&
                   contextLabel->text().find("\u2029") == std::string::npos &&
                   contextLabel->text().find("\u0085") == std::string::npos &&
                   contextLabel->height() > 0);
            nativeSingleTextRow(renderer, *contextLabel);
            const auto tooltip = contextLabel->tooltipContent();
            const auto *details = std::get_if<std::string>(&tooltip);
            assert(details && details->contains(buffer.target));
            within(*contextLabel, *composer);
            for (const auto *role : {":paste-insert", ":paste-send", ":paste-cancel"}) {
                auto *button = composerAction(*composer, role);
                assert(button && nativeVisible(*button) && button->inputArea()->width() >= 32 &&
                       button->inputArea()->height() >= 32);
                within(*button, *composer);
            }
            assert(input->value() == "draft" && dispatcher.focusedArea() == input->inputArea());
            dispatcher.setFocus(nullptr);
            dispatcher.setSceneRoot(nullptr);
        }
        struct LayoutCase {
            float width, height, scale;
        };
        // Every supported minimum retains two native body rows, including expanded context.
        for (const auto &[width, requestedHeight, scale] :
             std::array{LayoutCase{1440.f, 940.f, 1.f}, LayoutCase{720.f, 480.f, 1.f},
                        LayoutCase{720.f, 480.f, 2.f}}) {
            Model model;
            model.firstRun = false;
            model.now = 100;
            model.settings.scale = scale;
            model.settings.reducedMotion = true;
            model.settings.fx = FxLevel::Off;
            model.settings.sound = model.settings.typingSend = false;
            model.identities.emplace_back();
            ServerConfig server;
            server.name = "test";
            server.host = "example.invalid";
            model.servers.push_back(server);
            model.networks["test"].name = "Native composer network";
            model.networks["test"].state = "Ready";
            model.networks["test"].caps.insert("message-tags");
            auto &buffer = model.buffers["test/#room"];
            buffer.server = "test";
            buffer.id = "test/#room";
            buffer.target = "#room";
            buffer.topic = "A real channel topic that stays visible in the active pane";
            buffer.joined = true;
            buffer.draft = "Keep this draft";
            for (int i = 0; i < 24; ++i) {
                Message message;
                message.id = "remote-" + std::to_string(i);
                message.nick = "Peer";
                message.text = "HistoryBody" + std::to_string(i);
                if (i == 0)
                    message.text += " " + std::string(240, 'r');
                message.timestamp = 1700000000000000000LL + i * 1000000000LL;
                buffer.messages.push_back(std::move(message));
            }
            model.activeBuffer = buffer.id;
            model.settings.workspace = {
                {"1", buffer.id}, {"focused", "1"}, {"native_navigation_visible", "false"}};
            if (width > 720) {
                auto &other = model.buffers["test/#other"];
                other.id = "test/#other";
                other.server = "test";
                other.target = "#other";
                other.joined = true;
                model.settings.workspace["native_navigation_visible"] = "true";
            }
            Theme theme;
            theme.update(model);
            context.setTextFontFamily(themeUiFont().family);
            const float height = std::max(requestedHeight, float(minimumWindowHeight(renderer)));
            std::fprintf(stderr, "native composer layout: %gx%g zoom %g\n", width, height, scale);
            DeferredClipboard clipboard;
            InputDispatcher dispatcher;
            Ui ui(model, clipboard);
            dispatcher.setSceneRoot(&ui.root());
            ui.setInputDispatcher(dispatcher);
            ui.setWindowActive(true);
            const auto frame = [&] {
                ui.update(width, height, 1);
                ui.root().layout(renderer);
            };
            frame(); // The regression must first allocate an ordinary one-line composer.
            auto *input = inputWithDraft(ui.root(), buffer.draft);
            assert(input && input->value() == buffer.draft);
            auto *pane = conversationPane(*input);
            assert(pane);
            if (width > 720)
                assert(pane->width() > 720 && nativeVisible(*pane));
            auto *composer = input->parent();
            while (composer->parent() != pane)
                composer = composer->parent();
            TranscriptView *transcript = nullptr;
            for (const auto &child : pane->children())
                if (auto *view = dynamic_cast<TranscriptView *>(child.get()))
                    transcript = view;
            assert(transcript);
            std::uint32_t pointerSerial = 10;
            const auto pointerClick = [&](Button &button) {
                const auto bounds = sceneBounds(*button.inputArea());
                const float x = bounds.x + bounds.width / 2, y = bounds.y + bounds.height / 2;
                assert(dispatcher.inputAreaAt(x, y) == button.inputArea());
                for (const bool pressed : {true, false}) {
                    ++pointerSerial;
                    assert(dispatcher.pointerButton(x, y, BTN_LEFT, pressed, pointerSerial,
                                                    pointerSerial * 10, false));
                    assert(dispatcher.focusedArea() == input->inputArea());
                }
            };
            // Exercise the real source/host/edit/review path without opening a picker or sending.
            if (width > 720 && scale == 1.f) {
                const auto originalDraft = input->editingState();
                const auto originalUploads = model.settings.uploads;
                model.settings.uploads.provider = UploadProvider::X0At;
                model.settings.uploads.maxBytes = 100ULL * 1024 * 1024;
                input->setValue("");
                frame();
                auto *paperclip = composerAction(*composer, "upload");
                assert(paperclip && nativeVisible(*paperclip));
                MessageMenuHost uploadMenu;
                auto *previousHost = ui.root().popupContext();
                ui.root().setPopupContext(&uploadMenu);
                const auto actionsBeforeUpload = model.actions.size();
                dispatcher.setFocus(input->inputArea());
                pointerClick(*paperclip);
                assert(uploadMenu.open && uploadMenu.entry("Text / paste…"));
                uploadMenu.select("Text / paste…"); // Native dismissal happens before selection.
                assert(!uploadMenu.open); // The configured usable host needs no second popup.
                // A delayed panel focus must never cancel a newly started source IME.
                input->textInputApplyEdit({.commitText = {},
                                           .preeditText = "猫",
                                           .preeditCursorBegin = 3,
                                           .preeditCursorEnd = 3,
                                           .hasPreedit = true});
                const auto sourceIme = input->editingState();
                frame();
                auto *review = action(ui.root(), "Review text");
                auto *makeDefault = action(ui.root(), "Make default");
                assert(review && nativeVisible(*review) && makeDefault &&
                       !nativeVisible(*makeDefault));
                auto *uploadPanel = review->parent()->parent();
                Input *text = nullptr;
                for (const auto &child : uploadPanel->children())
                    if (auto *candidate = dynamic_cast<Input *>(child.get()))
                        text = candidate;
                assert(text && text != input && nativeVisible(*text) && text->value().empty());
                assert(text->height() >= 5 * 13.f && text->width() > 240.f);
                assert(input->composing() && input->editingStateMatches(sourceIme) &&
                       dispatcher.focusedArea() == input->inputArea());
                input->textInputResetPreedit();
                frame();
                assert(dispatcher.focusedArea() == text->inputArea() &&
                       text->inputArea()->tabFocusKey() == "upload-text");
                // The entry focus is one-shot: ordinary native Tab can leave it and stay away.
                const auto nativeTab = [&](std::uint32_t modifiers) {
                    const std::uint32_t sym =
                        modifiers & KeyMod::Shift ? XKB_KEY_ISO_Left_Tab : XKB_KEY_Tab;
                    for (const bool pressed : {true, false}) {
                        const InputArea::KeyData key{
                            .sym = sym, .modifiers = modifiers, .pressed = pressed};
                        if (!ui.handleKey(key))
                            dispatcher.keyEvent(sym, 0, modifiers, pressed);
                    }
                };
                nativeTab(0);
                frame();
                auto *close = action(*uploadPanel, "Close");
                assert(close && dispatcher.focusedArea() == close->inputArea());
                frame();
                assert(dispatcher.focusedArea() == close->inputArea());
                nativeTab(KeyMod::Shift);
                frame();
                assert(dispatcher.focusedArea() == text->inputArea());
                activate(ui.root(), "Review text");
                frame();
                assert(nativeVisible(*text) && nativeVisible(*review) && text->value().empty());
                const std::string paste =
                    std::string(900, 'w') + "\nPrivate test text\n猫\tsecond line\n\n";
                assert(text->insertText(paste) && text->value() == paste && input->value().empty());
                frame();
                assert(text->measuredMultilineHeight() > text->height() &&
                       text->editingState().scrollY > 0);
                text->textInputApplyEdit({.commitText = {},
                                          .preeditText = "未",
                                          .preeditCursorBegin = 3,
                                          .preeditCursorEnd = 3,
                                          .hasPreedit = true});
                assert(text->composing());
                assert(text->value() == paste + "未");
                const auto pasteIme = text->editingState();
                const auto pasteUndo = pasteIme.history->undo;
                const auto pasteRedo = pasteIme.history->redo;
                const auto assertUploadDocument =
                    [&](const Input::EditingState &expected,
                        const std::vector<Input::EditSnapshot> &undo,
                        const std::vector<Input::EditSnapshot> &redo) {
                        assert(nativeVisible(*text));
                        assert(dispatcher.focusedArea() == text->inputArea());
                        assert(text->composing() == expected.preeditBase.has_value());
                        const auto current = text->editingState();
                        assert(current.current == expected.current);
                        assert(current.preeditBase == expected.preeditBase);
                        assert(current.preeditStart == expected.preeditStart);
                        assert(current.preeditLength == expected.preeditLength);
                        assert(current.preeditCaretHidden == expected.preeditCaretHidden);
                        assert(current.history == expected.history);
                        assert(current.history->undo == undo);
                        assert(current.history->redo == redo);
                        // Native layout may reveal the preedit caret by scrolling its viewport.
                        assert(std::isfinite(current.scrollX) && current.scrollX >= 0);
                        assert(std::isfinite(current.scrollY) && current.scrollY > 0);
                        assert(model.actions.size() == actionsBeforeUpload);
                        assert(input->value().empty() && buffer.draft.empty());
                    };
                activate(ui.root(), "Review text");
                frame();
                assertUploadDocument(pasteIme, pasteUndo, pasteRedo);
                activate(ui.root(), "Choose host…");
                frame();
                assert(!uploadMenu.open);
                assertUploadDocument(pasteIme, pasteUndo, pasteRedo);
                text->textInputResetPreedit();
                const auto resetPaste = text->editingState();
                assert(!text->composing() && resetPaste.current == *pasteIme.preeditBase);
                assert(!resetPaste.preeditBase && resetPaste.preeditLength == 0);
                assert(resetPaste.history == pasteIme.history &&
                       resetPaste.history->undo == pasteUndo &&
                       resetPaste.history->redo == pasteRedo);
                dispatcher.setFocus(text->inputArea());
                dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
                dispatcher.keyEvent(XKB_KEY_Return, 0, 0, false);
                assert(text->value() == paste + "\n");
                activate(ui.root(), "Choose host…");
                frame();
                assert(uploadMenu.open && uploadMenu.request.selectedIndex ==
                                              static_cast<std::size_t>(UploadProvider::X0At));
                const auto hostLabel = std::string(uploadProviderLabel(UploadProvider::Uguu));
                assert(uploadMenu.entry(hostLabel));
                uploadMenu.select(hostLabel);
                frame();
                assert(text->value() == paste + "\n" && nativeVisible(*text) &&
                       nativeVisible(*makeDefault) && model.actions.size() == actionsBeforeUpload);
                const auto reviewedText = text->editingState();
                const auto reviewedUndo = reviewedText.history->undo;
                const auto reviewedRedo = reviewedText.history->redo;
                activate(ui.root(), "Review text");
                frame();
                auto *upload = action(ui.root(), "Upload");
                assert(upload && nativeVisible(*upload) && nativeVisible(*text) &&
                       !nativeVisible(*review) && displays(ui.root(), "Text paste") &&
                       displays(ui.root(), "text/plain") &&
                       displays(ui.root(), std::to_string(text->value().size()) + " bytes") &&
                       displays(ui.root(), "Visibility:") &&
                       displays(ui.root(), "Reviewed text below"));
                assert(text->value() == paste + "\n");
                assertUploadDocument(reviewedText, reviewedUndo, reviewedRedo);
                // Starting composition after review must not upload the previous snapshot.
                dispatcher.setFocus(text->inputArea());
                text->textInputApplyEdit({.commitText = {},
                                          .preeditText = "未",
                                          .preeditCursorBegin = 3,
                                          .preeditCursorEnd = 3,
                                          .hasPreedit = true});
                assert(text->composing() && text->value() == paste + "\n未" &&
                       model.actions.size() == actionsBeforeUpload);
                const auto reviewIme = text->editingState();
                const auto reviewImeUndo = reviewIme.history->undo;
                const auto reviewImeRedo = reviewIme.history->redo;
                frame();
                assert(!nativeVisible(*upload));
                assertUploadDocument(reviewIme, reviewImeUndo, reviewImeRedo);
                ui.execute("upload-start");
                frame();
                assert(!nativeVisible(*upload));
                assertUploadDocument(reviewIme, reviewImeUndo, reviewImeRedo);
                assert(displays(ui.root(), "Finish input-method composition"));
                text->textInputApplyEdit({.commitText = "未",
                                          .preeditText = {},
                                          .hasCommitText = true,
                                          .hasPreedit = true});
                frame();
                assert(!text->composing() && nativeVisible(*review) && !nativeVisible(*upload) &&
                       text->value() == paste + "\n未");
                // Editing the retained review is real editing, but invalidates upload consent.
                dispatcher.setFocus(text->inputArea());
                assert(text->insertText("not yet reviewed"));
                frame();
                assert(nativeVisible(*text) && nativeVisible(*review) && !nativeVisible(*upload) &&
                       model.actions.size() == actionsBeforeUpload && input->value().empty() &&
                       buffer.draft.empty());
                ui.execute("upload-start"); // A stale action cannot upload the old text snapshot.
                frame();
                assert(nativeVisible(*review) && !nativeVisible(*upload) &&
                       model.actions.size() == actionsBeforeUpload);
                const auto amendedText = text->editingState();
                const auto amendedUndo = amendedText.history->undo;
                const auto amendedRedo = amendedText.history->redo;
                activate(ui.root(), "Review text");
                frame();
                assert(nativeVisible(*upload) && !nativeVisible(*review));
                assertUploadDocument(amendedText, amendedUndo, amendedRedo);
                activate(ui.root(), "Make default");
                frame();
                assert(model.settings.uploads.provider == UploadProvider::Uguu &&
                       model.settings.uploads.maxBytes ==
                           uploadProviderLimit(UploadProvider::Uguu));
                assert(model.actions.size() == actionsBeforeUpload + 1 &&
                       model.actions.back().type == "save" && input->value().empty() &&
                       buffer.draft.empty());
                ui.execute("upload-close");
                frame();
                assert(!nativeVisible(*upload) && !nativeVisible(*makeDefault) &&
                       !nativeVisible(*text) && text->value().empty());
                ui.root().setPopupContext(previousHost);
                model.actions.pop_back();
                model.settings.uploads = originalUploads;
                input->restoreEditingState(originalDraft);
                frame();
                assert(input->value() == originalDraft.current.value &&
                       buffer.draft == originalDraft.current.value);
            }
            // The attached conversation material is the actual painted control boundary.
            MaterialNode *card = nullptr;
            for (const auto &child : pane->children())
                if (auto *material = dynamic_cast<MaterialNode *>(child.get());
                    material && nativeVisible(*material)) {
                    const auto painted = sceneBounds(*material), allocated = sceneBounds(*composer);
                    if (painted.x <= allocated.x && painted.y <= allocated.y &&
                        painted.x + painted.width >= allocated.x + allocated.width &&
                        painted.y + painted.height >= allocated.y + allocated.height) {
                        assert(!card);
                        card = material;
                    }
                }
            assert(card);
            auto *send = tooltipAction(*composer, "Send message");
            auto *actions = tooltipAction(*composer, "Formatting toolbar");
            assert(send && actions);
            auto *inputSurface = input->parent();
            assert(inputSurface && actions->parent() == inputSurface &&
                   send->parent() == inputSurface);
            const float historyMinimum =
                2 *
                renderer.fontRowExtent(messageTextSize(model.settings, buffer), themeMessageFont());
            const auto chrome = [&](std::string_view stage, bool replying, int historyRows = 2) {
                assert(nativeVisible(*pane) && nativeVisible(*composer) && nativeVisible(*card) &&
                       nativeVisible(*input) && nativeVisible(*send) && nativeVisible(*actions));
                within(*composer, *pane);
                within(*pane, ui.root());
                within(*card, *pane);
                within(*composer, *card);
                within(*input, *composer);
                within(*input->inputArea(), *composer);
                within(*send->inputArea(), *composer);
                within(*inputSurface, *composer);
                within(*input, *inputSurface);
                within(*actions, *inputSurface);
                within(*send, *inputSurface);
                const auto editorBounds = sceneBounds(*input);
                const auto actionBounds = sceneBounds(*actions);
                const auto sendBounds = sceneBounds(*send);
                assert(editorBounds.x + editorBounds.width <= actionBounds.x + .5f &&
                       actionBounds.x + actionBounds.width <= sendBounds.x + .5f);
                for (const Node *control :
                     {static_cast<Node *>(input), static_cast<Node *>(input->inputArea()),
                      static_cast<Node *>(actions), static_cast<Node *>(actions->inputArea()),
                      static_cast<Node *>(send), static_cast<Node *>(send->inputArea())}) {
                    within(*control, *composer);
                    within(*control, *card);
                    within(*control, *pane);
                }
                for (const auto *area :
                     {input->inputArea(), actions->inputArea(), send->inputArea()})
                    assert(area->width() >= 32 && area->height() >= 32);
                if (replying) {
                    auto *cancel = composerAction(*pane, ":reply-cancel");
                    assert(cancel && nativeVisible(*cancel));
                    assert(cancel->inputArea()->width() >= 32 &&
                           cancel->inputArea()->height() >= 32);
                    // Reply is a sibling overlay, not part of the composer material.
                    within(*cancel, *pane);
                    within(*cancel->inputArea(), *pane);
                }
                const float required = historyRows * (historyMinimum / 2);
                if (transcript->height() <= 0 || transcript->height() + .5f < required) {
                    auto *separate = composerAction(*composer, ":paste-send");
                    std::fprintf(
                        stderr,
                        "native composer history: case=%gx%g zoom=%g stage=%.*s requireRows=%d "
                        "reply=%d paste=%d lines=%zu actual=%g minimum=%g bodyFont=%s fontPx=%g "
                        "bodyRow=%g inputFontPx=%g inputRow=%g inputW=%g inputH=%g preferredH=%g "
                        "rootH=%g cardH=%g paneH=%g\n",
                        width, height, scale, static_cast<int>(stage.size()), stage.data(),
                        historyRows, replying, separate && separate->inputArea()->enabled(),
                        size_t(std::count(input->value().begin(), input->value().end(), '\n') + 1),
                        transcript->height(), required, themeMessageFont().family.c_str(),
                        messageTextSize(model.settings, buffer), historyMinimum / 2,
                        messageTextSize(model.settings, buffer),
                        renderer.fontRowExtent(messageTextSize(model.settings, buffer),
                                               themeMessageFont()),
                        input->width(), input->height(), input->measuredMultilineHeight(),
                        composer->height(), card->height(), pane->height());
                }
                assert(transcript->height() > 0 && transcript->height() + .5f >= required);
            };
            chrome("one-line", false);
            const float oneLineHeight = composer->height(), oneLineInput = input->height();
            const float oneLineInputWidth = input->width();
            const float oneLinePreferredInput = input->measuredMultilineHeight();
            assert(oneLineInput <=
                   std::max(std::max(32.f, 32 * scale), input->measuredMultilineHeight()) + .5f);

            // The plus opens a bounded, upward native toolbar without reallocating the input row.
            dispatcher.setFocus(input->inputArea());
            auto selectedDraft = input->editingState();
            selectedDraft.current.selectionAnchor = 1;
            selectedDraft.current.cursorPos = 4;
            input->restoreEditingState(selectedDraft);
            const auto selectionUndo = selectedDraft.history->undo;
            const auto selectionRedo = selectedDraft.history->redo;
            frame();
            const float stableInputHeight = input->height();
            pointerClick(*actions);
            frame();
            assert(inputWithDraft(ui.root(), selectedDraft.current.value) == input &&
                   editingDocumentMatches(*input, selectedDraft) &&
                   selectedDraft.history->undo == selectionUndo &&
                   selectedDraft.history->redo == selectionRedo &&
                   std::abs(input->height() - stableInputHeight) <= .5f);
            // Check the focused native caret's public window geometry separately from
            // a nonempty selection; custom cursor painting need not use the SDK RectNode.
            const auto expandedSelection = input->editingState();
            auto collapsedSelection = expandedSelection;
            collapsedSelection.current.selectionAnchor = collapsedSelection.current.cursorPos;
            input->restoreEditingState(collapsedSelection);
            frame();
            assert(editingDocumentMatches(*input, collapsedSelection));
            const auto expandedCaret = input->textInputState();
            assert(dispatcher.focusedArea() == input->inputArea() && nativeVisible(*input) &&
                   expandedCaret.cursorRectWidth > 0 && expandedCaret.cursorRectHeight > 0);
            const Node *textViewport = nullptr;
            for (const auto &child : input->children())
                if (child->clipChildren())
                    textViewport = child.get();
            assert(textViewport);
            withinBounds({static_cast<float>(expandedCaret.cursorRectX),
                          static_cast<float>(expandedCaret.cursorRectY),
                          static_cast<float>(expandedCaret.cursorRectWidth),
                          static_cast<float>(expandedCaret.cursorRectHeight)},
                         paintedClip(*textViewport, *textViewport));
            input->restoreEditingState(expandedSelection);
            frame();
            assert(editingDocumentMatches(*input, selectedDraft) &&
                   selectedDraft.history->undo == selectionUndo &&
                   selectedDraft.history->redo == selectionRedo &&
                   std::abs(input->height() - stableInputHeight) <= .5f);
            auto *bold = tooltipAction(*composer, "Bold");
            assert(bold && nativeVisible(*bold));
            ScrollView *formattingPanel = nullptr;
            for (auto *ancestor = bold->parent(); ancestor && ancestor != composer;
                 ancestor = ancestor->parent())
                if (auto *scroll = dynamic_cast<ScrollView *>(ancestor))
                    formattingPanel = scroll;
            assert(formattingPanel && nativeVisible(*formattingPanel));
            within(*formattingPanel, *composer);
            within(*formattingPanel, *card);
            const auto panelBounds = sceneBounds(*formattingPanel);
            assert(panelBounds.height > 0 &&
                   panelBounds.y + panelBounds.height <= sceneBounds(*inputSurface).y + .5f);
            assert(formattingPanel->contentViewportHeight() >= bold->height());
            formattingPanel->requestScrollIntoView(bold);
            frame();
            bool checkedViewportClip = false;
            for (auto *ancestor = bold->parent(); ancestor && ancestor != formattingPanel;
                 ancestor = ancestor->parent())
                if (ancestor->clipChildren()) {
                    within(*bold, *ancestor);
                    within(*bold->inputArea(), *ancestor);
                    checkedViewportClip = true;
                }
            assert(checkedViewportClip);
            const auto revealedBold = sceneBounds(*bold->inputArea());
            for (const float y : {revealedBold.y + 1, revealedBold.y + revealedBold.height - 1})
                assert(dispatcher.inputAreaAt(revealedBold.x + revealedBold.width / 2, y) ==
                       bold->inputArea());
            if (width <= 720)
                assert(formattingPanel->scrollable());
            std::vector<Button *> formattingButtons;
            collectButtons(*formattingPanel, formattingButtons);
            assert(formattingButtons.size() == 39);
            constexpr std::array decorations{
                "Bold",      "Italic",  "Underline",       "Strikethrough",
                "Monospace", "Reverse", "Reset formatting"};
            for (size_t index = 0; index < formattingButtons.size(); ++index) {
                const auto tooltip = formattingButtons[index]->inputArea()->tooltipContent();
                const auto *text = std::get_if<std::string>(&tooltip);
                assert(text);
                if (index < decorations.size())
                    assert(*text == decorations[index]);
                else {
                    const auto color = (index - decorations.size()) % 16;
                    const auto code = (color < 10 ? "0" : "") + std::to_string(color);
                    assert(*text == std::string(index < 23 ? "Foreground" : "Background") +
                                        " IRC color " + code);
                }
            }
            const auto undoCount = input->editingState().history->undo.size();
            pointerClick(*bold);
            frame();
            const auto styledDraft =
                parseIrcFormatting(input->value(), rgbHex(0xffffff), rgbHex(0x000000));
            assert(styledDraft.text == selectedDraft.current.value &&
                   styledDraft.runs.size() == 3 && styledDraft.runs[1].style.bold &&
                   !styledDraft.runs.back().style.bold &&
                   input->editingState().history->undo.size() == undoCount + 1 &&
                   dispatcher.focusedArea() == input->inputArea() &&
                   std::abs(input->height() - stableInputHeight) <= .5f);
            ui.execute("undo");
            frame();
            assert(input->editingState().current == selectedDraft.current);
            const auto heldFormat = input->editingState();
            const auto heldUndo = heldFormat.history->undo, heldRedo = heldFormat.history->redo;
            const auto boldBounds = sceneBounds(*bold->inputArea());
            const float boldX = boldBounds.x + boldBounds.width / 2;
            const float boldY = boldBounds.y + boldBounds.height / 2;
            ++pointerSerial;
            assert(dispatcher.pointerButton(boldX, boldY, BTN_LEFT, true, pointerSerial,
                                            pointerSerial * 10, false));
            assert(bold->inputArea()->pressed() && dispatcher.pointerCaptured());
            // Pane sync owns visibility: search state hides the composer without rebinding it.
            const auto heldBufferKind = buffer.kind;
            buffer.kind = "search";
            frame(); // Buffer identity is deliberately unchanged.
            assert(!nativeVisible(*composer) && !bold->inputArea()->pressed() &&
                   !dispatcher.pointerCaptured());
            ++pointerSerial;
            dispatcher.pointerButton(boldX, boldY, BTN_LEFT, false, pointerSerial,
                                     pointerSerial * 10, false);
            assert(input->editingState().current == heldFormat.current &&
                   heldFormat.history->undo == heldUndo && heldFormat.history->redo == heldRedo);
            buffer.kind = heldBufferKind;
            frame();
            dispatcher.setFocus(input->inputArea());
            // Native keyboard activation restores the composer rather than stranding focus.
            dispatcher.setFocus(bold->inputArea());
            activateTooltip(*composer, "Bold");
            frame();
            assert(dispatcher.focusedArea() == input->inputArea());
            ui.execute("undo");
            frame();
            pointerClick(*actions);
            frame();
            assert(!nativeVisible(*formattingPanel) &&
                   std::abs(input->height() - stableInputHeight) <= .5f);
            input->restoreEditingState(selectedDraft);
            dispatcher.keyEvent(XKB_KEY_End, 0, KeyMod::Ctrl, true);
            dispatcher.keyEvent(XKB_KEY_End, 0, KeyMod::Ctrl, false);
            frame();
            {
                const auto saved = input->editingState();
                const auto key = [&](std::uint32_t sym, std::uint32_t modifiers) {
                    for (const bool pressed : {true, false}) {
                        InputArea::KeyData event{
                            .sym = sym, .modifiers = modifiers, .pressed = pressed};
                        if (!ui.handleKey(event))
                            dispatcher.keyEvent(sym, 0, modifiers, pressed, false);
                    }
                };
                const std::string draft = "a猫b\nsecond";
                input->setValue(draft);
                auto state = input->editingState();
                state.current.cursorPos = state.current.selectionAnchor = 1;
                input->restoreEditingState(state);
                dispatcher.setFocus(input->inputArea());
                key(XKB_KEY_space, KeyMod::Ctrl);
                key(XKB_KEY_e, KeyMod::Ctrl);
                assert(input->editingState().current.selectionAnchor == 1 &&
                       input->editingState().current.cursorPos == 5 && input->value() == draft &&
                       buffer.draft == draft && dispatcher.focusedArea() == input->inputArea());
                key(XKB_KEY_w, KeyMod::Alt);
                assert(clipboard.copied == "猫b");
                key(XKB_KEY_g, KeyMod::Ctrl);
                state = input->editingState();
                state.current.selectionAnchor = 0;
                state.current.cursorPos = 1;
                input->restoreEditingState(state);
                key(XKB_KEY_e, KeyMod::Ctrl);
                assert(input->editingState().current.selectionAnchor == 0 &&
                       input->editingState().current.cursorPos == 5 && input->value() == draft);
                key(XKB_KEY_w, KeyMod::Alt);
                assert(clipboard.copied == "a猫b");
                input->restoreEditingState(saved);
                frame();
            }

            // Select and read a real retained message through the public native consumer path.
            const auto document = input->editingState();
            const auto documentUndo = document.history->undo, documentRedo = document.history->redo;
            const auto documentPreserved = [&] {
                return editingDocumentMatches(*input, document) &&
                       document.history->undo == documentUndo &&
                       document.history->redo == documentRedo;
            };
            dispatcher.setFocus(input->inputArea());
            for (size_t i = 0; i < buffer.messages.size(); ++i) {
                ui.execute("message-prev");
                frame();
            }
            const auto selectedMessage = transcript->selectedMessage();
            assert(selectedMessage && selectedMessage->canReply);
            const auto readerOwnsFocus = [&] {
                auto *focused = dispatcher.focusedArea();
                if (!focused || focused == input->inputArea() || focused->textInputClient())
                    return false;
                for (auto *node = static_cast<Node *>(focused); node; node = node->parent())
                    if (node == transcript)
                        return true;
                return false;
            };
            assert(readerOwnsFocus());
            {
                const auto key = [&](std::uint32_t sym, std::uint32_t modifiers) {
                    for (const bool pressed : {true, false}) {
                        InputArea::KeyData event{
                            .sym = sym, .modifiers = modifiers, .pressed = pressed};
                        if (!ui.handleKey(event))
                            dispatcher.keyEvent(sym, 0, modifiers, pressed, false);
                    }
                };
                key(XKB_KEY_w, KeyMod::Alt);
                assert(clipboard.copied == buffer.messages.front().text);
                clipboard.copied = "sentinel";
                key(XKB_KEY_space, KeyMod::Ctrl);
                key(XKB_KEY_w, KeyMod::Alt);
                assert(clipboard.copied == "sentinel");
                key(XKB_KEY_Right, 0);
                key(XKB_KEY_w, KeyMod::Alt);
                assert(clipboard.copied == "H");
                key(XKB_KEY_e, KeyMod::Ctrl);
                key(XKB_KEY_w, KeyMod::Alt);
                assert(clipboard.copied == buffer.messages.front().text);
                assert(readerOwnsFocus() && documentPreserved());
                key(XKB_KEY_g, KeyMod::Ctrl);
                assert(dispatcher.focusedArea() == input->inputArea() && documentPreserved());
                for (size_t i = 0; i < buffer.messages.size(); ++i) {
                    ui.execute("message-prev");
                    frame();
                }
                assert(readerOwnsFocus() && !transcript->keyboardTextSelection());
                const auto nextBinding = model.settings.bindings.at("message-next");
                model.settings.bindings["message-next"] = "RIGHT";
                key(XKB_KEY_space, KeyMod::Ctrl);
                key(XKB_KEY_Right, 0);
                assert(!transcript->keyboardTextSelection() && readerOwnsFocus());
                key(XKB_KEY_w, KeyMod::Alt);
                assert(clipboard.copied == buffer.messages[1].text);
                model.settings.bindings["message-next"] = "RIGHT x";
                key(XKB_KEY_space, KeyMod::Ctrl);
                key(XKB_KEY_Right, 0);
                assert(transcript->keyboardTextSelection());
                key(XKB_KEY_x, 0);
                assert(!transcript->keyboardTextSelection() && readerOwnsFocus());
                key(XKB_KEY_w, KeyMod::Alt);
                assert(clipboard.copied == buffer.messages[2].text && documentPreserved());
                model.settings.bindings["message-next"] = nextBinding;
                ui.execute("message-prev");
                ui.execute("message-prev");
                frame();
            }
            assert(ui.handleKey({.sym = XKB_KEY_Home, .pressed = true}));
            ui.handleKey({.sym = XKB_KEY_Home, .pressed = false});
            frame();
            TextNode *anchor = nullptr;
            LayoutRect bodyInk;
            LayoutRect readingClip;
            LayoutRect bodyPaint;
            const auto bodyVisible = [&] {
                anchor = contextText(*transcript, "HistoryBody0");
                if (!anchor || !nativeVisible(*anchor))
                    return false;
                const auto metrics = renderer.measureText(anchor->text(), anchor->fontSize(),
                                                          anchor->font(), anchor->maxWidth(),
                                                          anchor->maxLines(), anchor->textAlign(),
                                                          anchor->ellipsize(), anchor->useMarkup());
                const auto baseline = sceneBounds(*anchor);
                bodyInk = {baseline.x + metrics.inkLeft, baseline.y + metrics.inkTop,
                           metrics.inkRight - metrics.inkLeft, metrics.inkBottom - metrics.inkTop};
                readingClip = paintedClip(*anchor, *transcript);
                const float left = std::max(bodyInk.x, readingClip.x);
                const float top = std::max(bodyInk.y, readingClip.y);
                const float right =
                    std::min(bodyInk.x + bodyInk.width, readingClip.x + readingClip.width);
                const float bottom =
                    std::min(bodyInk.y + bodyInk.height, readingClip.y + readingClip.height);
                bodyPaint = {left, top, std::max(0.f, right - left), std::max(0.f, bottom - top)};
                return bodyPaint.width > 0 && bodyPaint.height > 0;
            };
            // Scroll the public history viewport, never internal row wrappers or overscan nodes.
            for (size_t i = 0; i < buffer.messages.size() && !bodyVisible(); ++i) {
                assert(anchor && transcript->scrollHalfPage(bodyInk.y < readingClip.y ? -1 : 1));
                frame();
            }
            assert(bodyVisible());
            const auto initialPaint = paintedTextWithin(renderer, *anchor, *transcript);
            assert(initialPaint.width > 0 && initialPaint.height > 0);
            assert(documentPreserved());
            assert(input->editingState().history->undo == documentUndo &&
                   input->editingState().history->redo == documentRedo &&
                   buffer.draft == document.current.value);
            MessageMenuHost replyMenu;
            auto *previousMenuHost = ui.root().popupContext();
            ui.root().setPopupContext(&replyMenu);
            const float bodyX = bodyPaint.x + bodyPaint.width / 2;
            const float bodyY = bodyPaint.y + bodyPaint.height / 2;
            auto *bodyTarget = dispatcher.inputAreaAt(bodyX, bodyY);
            bool targetsHistory = false;
            for (auto *node = static_cast<Node *>(bodyTarget); node; node = node->parent())
                if (node == transcript)
                    targetsHistory = true;
            assert(bodyTarget && targetsHistory && !bodyTarget->textInputClient());
            for (const bool pressed : {true, false}) {
                ++pointerSerial;
                if (pressed)
                    ui.beginPointerInteraction(bodyX, bodyY);
                if (!ui.handlePointerButton(BTN_RIGHT, pressed, bodyX, bodyY, false))
                    assert(dispatcher.pointerButton(bodyX, bodyY, BTN_RIGHT, pressed, pointerSerial,
                                                    pointerSerial * 10, false));
            }
            assert(replyMenu.open && replyMenu.request.anchorWidth > 0 &&
                   replyMenu.request.anchorHeight > 0);
            const auto viewport = sceneBounds(*transcript);
            // Native DropdownRequest coordinates occupy the viewport's outward integer envelope.
            const bool anchorContained =
                replyMenu.request.anchorX >= std::floor(viewport.x) &&
                replyMenu.request.anchorY >= std::floor(viewport.y) &&
                replyMenu.request.anchorX + replyMenu.request.anchorWidth <=
                    std::ceil(viewport.x + viewport.width) &&
                replyMenu.request.anchorY + replyMenu.request.anchorHeight <=
                    std::ceil(viewport.y + viewport.height);
            if (!anchorContained)
                std::fprintf(stderr,
                             "native composer menu anchor: stage=reply-from-painted-body "
                             "window=%gx%g zoom=%g request-int=(%d,%d,%d,%d) "
                             "viewport-float=(%g,%g,%g,%g)\n",
                             width, height, scale, replyMenu.request.anchorX,
                             replyMenu.request.anchorY, replyMenu.request.anchorWidth,
                             replyMenu.request.anchorHeight, viewport.x, viewport.y, viewport.width,
                             viewport.height);
            assert(anchorContained);
            frame(); // Settle the actual context gesture before measuring its reader anchor.
            assert(replyMenu.open && bodyVisible() && documentPreserved());
            assert(input->editingState().history->undo == documentUndo &&
                   input->editingState().history->redo == documentRedo &&
                   buffer.draft == document.current.value);
            float anchorY = sceneBounds(*anchor).y;
            ScrollView *historyScroll = nullptr;
            for (auto *node = anchor->parent(); node; node = node->parent())
                if ((historyScroll = dynamic_cast<ScrollView *>(node)))
                    break;
            assert(historyScroll);
            const auto anchored = [&](std::string_view stage, bool requireVisible = false) {
                auto *body = contextText(*transcript, "HistoryBody0");
                const auto visibleInk = body && nativeVisible(*body)
                                            ? paintedTextWithin(renderer, *body, *transcript)
                                            : LayoutRect{};
                const bool paintVisible = visibleInk.width > 0 && visibleInk.height > 0;
                const float actualY = body ? sceneBounds(*body).y : std::nanf("");
                const auto nativeClip = body ? paintedClip(*body, *transcript) : LayoutRect{};
                const float actualOffset = historyScroll->scrollOffset();
                const float maximumOffset = historyScroll->maxScrollOffset();
                // Preserve the reader's baseline whenever reachable; at a native
                // scroll boundary require the exact closest feasible baseline.
                const float desiredOffset = actualOffset + actualY - anchorY;
                const float feasibleOffset = std::clamp(desiredOffset, 0.f, maximumOffset);
                const float feasibleY = actualY + actualOffset - feasibleOffset;
                if ((!paintVisible && requireVisible) ||
                    (paintVisible && std::abs(actualY - feasibleY) > .5f)) {
                    const auto historyBounds = sceneBounds(*transcript);
                    const auto composerBounds = sceneBounds(*composer);
                    std::fprintf(stderr,
                                 "native composer body anchor: stage=%.*s window=%gx%g zoom=%g "
                                 "body=%d visible=%d y=%g expected=%g delta=%g bodyW=%g "
                                 "paint=(%g,%g,%g,%g) transcript=(%g,%g,%g,%g) "
                                 "composer=(%g,%g,%g,%g) inputH=%g "
                                 "scroll=%g maxScroll=%g desiredScroll=%g feasibleScroll=%g "
                                 "feasibleY=%g nativeClip=(%g,%g,%g,%g)\n",
                                 int(stage.size()), stage.data(), width, height, scale,
                                 body != nullptr, paintVisible, actualY, anchorY, actualY - anchorY,
                                 body ? body->width() : 0, visibleInk.x, visibleInk.y,
                                 visibleInk.width, visibleInk.height, historyBounds.x,
                                 historyBounds.y, historyBounds.width, historyBounds.height,
                                 composerBounds.x, composerBounds.y, composerBounds.width,
                                 composerBounds.height, input->height(), actualOffset,
                                 maximumOffset, desiredOffset, feasibleOffset, feasibleY,
                                 nativeClip.x, nativeClip.y, nativeClip.width, nativeClip.height);
                }
                if (requireVisible)
                    assert(paintVisible);
                if (paintVisible) {
                    assert(std::abs(actualY - feasibleY) <= .5f);
                    // A verified boundary clamp establishes the reader's new
                    // visible baseline; later chrome changes preserve that view.
                    if (desiredOffset < 0 || desiredOffset > maximumOffset)
                        anchorY = actualY;
                }
            };
            replyMenu.select("Reply");
            ui.root().setPopupContext(previousMenuHost);
            frame();
            assert(displays(*pane, "HistoryBody0") &&
                   dispatcher.focusedArea() == input->inputArea());
            chrome("reply", true);
            anchored("reply");
            auto *cancelReply = composerAction(*pane, ":reply-cancel");
            assert(cancelReply && nativeVisible(*cancelReply));
            within(*cancelReply, *pane);
            assert(cancelReply->inputArea()->width() > 0 &&
                   cancelReply->inputArea()->height() > 0 && composer->height() >= oneLineHeight &&
                   std::abs(input->height() - oneLineInput) <= .5f);
            assert(std::abs(input->width() - oneLineInputWidth) <= .5f &&
                   std::abs(input->measuredMultilineHeight() - oneLinePreferredInput) <= .5f);
            const auto replyHit = sceneBounds(*cancelReply->inputArea());
            assert(dispatcher.inputAreaAt(replyHit.x + replyHit.width / 2,
                                          replyHit.y + replyHit.height / 2) ==
                   cancelReply->inputArea());
            assert(documentPreserved());
            const float replyHeight = composer->height();

            ui.execute("paste");
            clipboard.deliver("\x02one\x02\r\n\r\n/quit\r\n猫😀\r\n \r\nlast\r\n");
            frame();
            chrome("paste", true);
            anchored("paste");
            assert(buffer.draft == "Keep this draft");
            assert(documentPreserved());
            assert(std::none_of(model.actions.begin(), model.actions.end(), [](const Action &a) {
                return a.type == "send" || a.type == "command";
            }));
            auto *insertPaste = composerAction(*composer, ":paste-insert");
            auto *sendPaste = composerAction(*composer, ":paste-send");
            auto *cancelPaste = composerAction(*composer, ":paste-cancel");
            assert(insertPaste && sendPaste && cancelPaste);
            auto *accessory = dynamic_cast<Flex *>(insertPaste->parent());
            assert(accessory && nativeVisible(*accessory));
            within(*accessory, *composer);
            auto *pasteContextText = contextText(*composer, "Clipboard for ");
            auto *pasteContext =
                pasteContextText ? dynamic_cast<Label *>(pasteContextText->parent()) : nullptr;
            assert(pasteContext && nativeVisible(*pasteContext) &&
                   pasteContext->text().contains(buffer.target) && pasteContext->height() > 0 &&
                   !pasteContext->focusOnPointerPress() && sendPaste->label() &&
                   sendPaste->label()->text().contains("5"));
            const std::string normalPasteContext = pasteContext->text();
            if (width == 720 && scale == 2) {
                assert(pasteContext->text().find('\n') == std::string::npos);
                nativeSingleTextRow(renderer, *pasteContext);
            }
            for (auto *button : {insertPaste, sendPaste, cancelPaste, insertPaste}) {
                assert(dispatcher.cycleTabFocusInSubtree(pasteContext->parent(), false));
                assert(dispatcher.focusedArea() == button->inputArea());
            }
            dispatcher.setFocus(input->inputArea());
            assert(documentPreserved());
            within(*pasteContext, *composer);
            const auto contextHit = sceneBounds(*pasteContext);
            assert(dispatcher.inputAreaAt(contextHit.x + contextHit.width / 2,
                                          contextHit.y + contextHit.height / 2) == pasteContext);
            const auto contextTooltip = pasteContext->tooltipContent();
            const auto *contextDetails = std::get_if<std::string>(&contextTooltip);
            assert(contextDetails && contextDetails->contains(buffer.target) &&
                   contextDetails->contains("/quit") && contextDetails->contains("猫😀"));
            const auto pasteUndo = document.history->undo;
            const auto pasteRedo = document.history->redo;
            const auto freshTouchSurvivesMouseUp = [&](float mouseX, float mouseY,
                                                       std::string_view count) {
                const auto touchBounds = sceneBounds(*pasteContext);
                const float touchX = touchBounds.x + touchBounds.width / 2;
                const float touchY = touchBounds.y + touchBounds.height / 2;
                size_t downs = 0, ups = 0, cancels = 0, clicks = 0;
                std::optional<std::pair<float, float>> motion;
                pasteContext->setOnPress([&](const InputArea::PointerData &event) {
                    if (event.pressed)
                        ++downs;
                    else
                        ++ups;
                });
                pasteContext->setOnMotion([&](const InputArea::PointerData &event) {
                    motion = std::pair{event.localX, event.localY};
                });
                pasteContext->setOnCancel([&] { ++cancels; });
                pasteContext->setOnClick([&](const InputArea::PointerData &) { ++clicks; });
                assert(!ui.prepareTouchPress());
                ui.beginPointerInteraction(touchX, touchY);
                assert(!ui.handlePointerButton(BTN_LEFT, true, touchX, touchY, true));
                ++pointerSerial;
                assert(dispatcher.pointerButton(touchX, touchY, BTN_LEFT, true, pointerSerial,
                                                pointerSerial * 10, true));
                assert(downs == 1 && ups == 0 && cancels == 0 && clicks == 0 &&
                       pasteContext->pressed() && dispatcher.capturedArea() == pasteContext);
                assert(ui.handlePointerButton(BTN_LEFT, false, mouseX, mouseY));
                assert(downs == 1 && ups == 0 && cancels == 0 && clicks == 0 &&
                       pasteContext->pressed() && dispatcher.capturedArea() == pasteContext);
                ui.handlePointerMotion(touchX + 1, touchY + 1, true);
                float expectedLocalX = 0, expectedLocalY = 0;
                Node::mapFromScene(pasteContext, touchX + 1, touchY + 1, expectedLocalX,
                                   expectedLocalY);
                dispatcher.pointerMotion(touchX + 1, touchY + 1, ++pointerSerial);
                assert(motion && motion->first == expectedLocalX &&
                       motion->second == expectedLocalY && downs == 1 && ups == 0 && cancels == 0 &&
                       clicks == 0 && pasteContext->pressed() &&
                       dispatcher.capturedArea() == pasteContext);
                assert(!ui.handlePointerButton(BTN_LEFT, false, touchX + 1, touchY + 1, true));
                ++pointerSerial;
                assert(dispatcher.pointerButton(touchX + 1, touchY + 1, BTN_LEFT, false,
                                                pointerSerial, pointerSerial * 10, true));
                assert(downs == 1 && ups == 1 && cancels == 0 && clicks == 1);
                assert(!pasteContext->pressed() && !dispatcher.pointerCaptured() &&
                       documentPreserved() && document.history->undo == pasteUndo &&
                       document.history->redo == pasteRedo &&
                       sendPaste->label()->text().contains(count) &&
                       dispatcher.focusedArea() == input->inputArea());
                for (const auto *role : {":paste-insert", ":paste-send", ":paste-cancel"}) {
                    auto *button = composerAction(*composer, role);
                    assert(button && nativeVisible(*button) && button->inputArea()->enabled() &&
                           button->inputArea()->width() >= 32 &&
                           button->inputArea()->height() >= 32);
                    within(*button, *composer);
                    const auto hit = sceneBounds(*button->inputArea());
                    assert(dispatcher.inputAreaAt(hit.x + hit.width / 2, hit.y + hit.height / 2) ==
                           button->inputArea());
                }
                assert(
                    std::none_of(model.actions.begin(), model.actions.end(), [](const Action &a) {
                        return a.type == "send" || a.type == "command";
                    }));
                pasteContext->setOnPress({});
                pasteContext->setOnMotion({});
                pasteContext->setOnCancel({});
                pasteContext->setOnClick({});
            };
            if (width > 720)
                assert(composer->height() > replyHeight);
            for (auto *button : {insertPaste, sendPaste, cancelPaste}) {
                assert(button->parent() == accessory && nativeVisible(*button) &&
                       button->inputArea()->enabled() && button->height() >= 32);
                within(*button, *accessory);
                within(*button, *composer);
                const auto hit = sceneBounds(*button->inputArea());
                assert(dispatcher.inputAreaAt(hit.x + hit.width / 2, hit.y + hit.height / 2) ==
                       button->inputArea());
            }
            // A real tall, narrow allocation retains the original bounded native preview.
            const float roomyWidth = width + std::max(32.f, 32 * scale);
            float roomyHeight = 2400;
            const auto roomyPreviewFrame = [&] {
                ui.update(roomyWidth, roomyHeight, 1);
                ui.root().layout(renderer);
            };
            const std::string scrollingClipboard =
                "/quit\n猫😀 " + std::string(2000, 'x') + "\nmore\n \nlast\n";
            ui.execute("paste");
            clipboard.deliver(scrollingClipboard);
            roomyPreviewFrame();
            auto *previewText = contextText(*composer, "Preview:");
            assert(previewText && nativeVisible(*previewText) &&
                   previewText->text().contains("/quit") && previewText->text().contains("猫😀"));
            ScrollView *pastePreview = nullptr;
            for (auto *ancestor = previewText->parent(); ancestor; ancestor = ancestor->parent())
                if ((pastePreview = dynamic_cast<ScrollView *>(ancestor)))
                    break;
            assert(pastePreview && nativeVisible(*pastePreview) && pastePreview->height() > 0 &&
                   pastePreview->height() <= 160 * scale);
            within(*pastePreview, *composer);
            for (auto *ancestor = pasteContext->parent(); ancestor; ancestor = ancestor->parent())
                assert(ancestor != pastePreview); // Pinned context is outside the scrolling body.
            if (scale == 2)
                assert(pastePreview->scrollable());
            if (pastePreview->scrollable()) {
                pastePreview->requestScrollToBottom();
                roomyPreviewFrame();
                assert(pastePreview->scrollOffset() > 0 &&
                       pastePreview->scrollOffset() == pastePreview->maxScrollOffset() &&
                       nativeVisible(*pasteContext) &&
                       dispatcher.focusedArea() == input->inputArea());
            }
            if (scale == 2) {
                InputArea *previewThumb = nullptr;
                const auto findThumb = [&](auto &&self, Node &node) -> void {
                    if (dynamic_cast<Scrollbar *>(&node) && nativeVisible(node))
                        for (const auto &child : node.children())
                            if (auto *area = dynamic_cast<InputArea *>(child.get());
                                area && area->width() > 0 && area->height() > 0) {
                                const auto hit = sceneBounds(*area);
                                if (dispatcher.inputAreaAt(hit.x + hit.width / 2,
                                                           hit.y + hit.height / 2) == area &&
                                    (!previewThumb ||
                                     hit.width * hit.height <
                                         previewThumb->width() * previewThumb->height()))
                                    previewThumb = area;
                            }
                    for (const auto &child : node.children())
                        self(self, *child);
                };
                bool compactScene = false;
                WindowActions pointerHost{};
                pointerHost.synchronizeLayout = [&] {
                    if (compactScene)
                        frame();
                    else
                        roomyPreviewFrame();
                    return true;
                };
                ui.setWindowActions(std::move(pointerHost));
                // A deferred preview touch is an owned gesture even before native capture.
                auto *previewArea = dynamic_cast<InputArea *>(previewText->parent());
                assert(previewArea);
                const auto previewBounds = sceneBounds(*pastePreview);
                const float previewX = previewBounds.x + previewBounds.width / 2;
                const float previewY = previewBounds.y + previewBounds.height / 2;
                assert(dispatcher.inputAreaAt(previewX, previewY) == previewArea);
                std::vector<bool> deferredPresses;
                previewArea->setOnPress([&](const InputArea::PointerData &event) {
                    deferredPresses.push_back(event.pressed);
                });
                ui.execute("paste");
                assert(clipboard.pending);
                assert(!ui.prepareTouchPress());
                ui.beginPointerInteraction(previewX, previewY);
                assert(!ui.handlePointerButton(BTN_LEFT, true, previewX, previewY, true));
                ++pointerSerial;
                assert(dispatcher.pointerButton(previewX, previewY, BTN_LEFT, true, pointerSerial,
                                                pointerSerial * 10, true));
                assert(!dispatcher.capturedArea() &&
                       dispatcher.pendingTouchPressArea() == previewArea &&
                       deferredPresses.empty());
                roomyHeight = 3600;
                roomyPreviewFrame();
                assert(nativeVisible(*pastePreview) && nativeVisible(*previewArea) &&
                       dispatcher.pendingTouchPressArea() == previewArea);
                auto *oldPointHit = dispatcher.inputAreaAt(previewX, previewY);
                assert(oldPointHit != previewArea);
                for (auto *ancestor = static_cast<Node *>(oldPointHit); ancestor;
                     ancestor = ancestor->parent())
                    assert(ancestor != pasteContext->parent());
                clipboard.deliver("pending-one\n\n\n\n\npending-two");
                roomyPreviewFrame();
                if (ui.retiredTouch())
                    ui.discardPointerRelease(BTN_LEFT, true);
                else if (!ui.handlePointerButton(BTN_LEFT, false, previewX, previewY, true)) {
                    ++pointerSerial;
                    dispatcher.pointerButton(previewX, previewY, BTN_LEFT, false, pointerSerial,
                                             pointerSerial * 10, true);
                }
                assert(deferredPresses.empty() && ui.consumeTouchLeave() &&
                       !dispatcher.pendingTouchPressArea() && documentPreserved());
                previewArea->setOnPress({});
                roomyHeight = 2400;
                roomyPreviewFrame();

                // Replacing paste data must not retire a different live control's mouse capture.
                const auto plusBounds = sceneBounds(*actions->inputArea());
                const float plusX = plusBounds.x + plusBounds.width / 2;
                const float plusY = plusBounds.y + plusBounds.height / 2;
                ui.beginPointerInteraction(plusX, plusY);
                assert(!ui.handlePointerButton(BTN_LEFT, true, plusX, plusY));
                ++pointerSerial;
                assert(dispatcher.pointerButton(plusX, plusY, BTN_LEFT, true, pointerSerial,
                                                pointerSerial * 10, false));
                assert(actions->inputArea()->pressed());
                ui.execute("paste");
                clipboard.deliver(scrollingClipboard);
                roomyPreviewFrame();
                assert(actions->inputArea()->pressed() &&
                       dispatcher.capturedArea() == actions->inputArea());
                const auto outside = sceneBounds(*pasteContext);
                const float outsideX = outside.x + outside.width / 2;
                const float outsideY = outside.y + outside.height / 2;
                ui.handlePointerMotion(outsideX, outsideY, false);
                dispatcher.pointerMotion(outsideX, outsideY, ++pointerSerial);
                assert(!ui.handlePointerButton(BTN_LEFT, false, outsideX, outsideY));
                ++pointerSerial;
                assert(dispatcher.pointerButton(outsideX, outsideY, BTN_LEFT, false, pointerSerial,
                                                pointerSerial * 10, false));
                assert(!actions->inputArea()->pressed() && !nativeVisible(*formattingPanel) &&
                       documentPreserved());
                pastePreview->requestScrollToBottom();
                roomyPreviewFrame();
                findThumb(findThumb, *pastePreview);
                assert(previewThumb);
                const auto thumb = sceneBounds(*previewThumb);
                const float mouseX = thumb.x + thumb.width / 2;
                const float mouseY = thumb.y + thumb.height / 2;
                ui.beginPointerInteraction(mouseX, mouseY);
                assert(!ui.handlePointerButton(BTN_LEFT, true, mouseX, mouseY));
                ++pointerSerial;
                assert(dispatcher.pointerButton(mouseX, mouseY, BTN_LEFT, true, pointerSerial,
                                                pointerSerial * 10, false));
                assert(previewThumb->pressed() && dispatcher.capturedArea() == previewThumb);
                compactScene = true;
                frame();
                assert(!nativeVisible(*pastePreview) && previewThumb->pressed() &&
                       dispatcher.capturedArea() == previewThumb);
                freshTouchSurvivesMouseUp(mouseX, mouseY, "5");
                ui.setWindowActions({});
            }
            ui.execute("paste");
            clipboard.deliver("\x02one\x02\r\n\r\n/quit\r\n猫😀\r\n \r\nlast\r\n");
            roomyPreviewFrame();
            frame();
            assert(documentPreserved());
            // Replacing preview A cancels its captured Send press before preview B is armed.
            const auto heldSend = sceneBounds(*sendPaste->inputArea());
            const float heldX = heldSend.x + heldSend.width / 2;
            const float heldY = heldSend.y + heldSend.height / 2;
            WindowActions replacementHost{};
            replacementHost.synchronizeLayout = [&] {
                frame();
                return true;
            };
            ui.setWindowActions(std::move(replacementHost));
            ui.beginPointerInteraction(heldX, heldY);
            assert(!ui.handlePointerButton(BTN_LEFT, true, heldX, heldY));
            ++pointerSerial;
            dispatcher.pointerButton(heldX, heldY, BTN_LEFT, true, pointerSerial,
                                     pointerSerial * 10, false);
            assert(sendPaste->inputArea()->pressed() &&
                   dispatcher.focusedArea() == input->inputArea());
            const auto requestsBeforeReplacement = clipboard.requests;
            for (const bool pressed : {true, false}) {
                const InputArea::KeyData key{
                    .sym = XKB_KEY_v, .modifiers = KeyMod::Ctrl, .pressed = pressed};
                if (!ui.handleKey(key, KEY_V))
                    dispatcher.keyEvent(key.sym, key.utf32, key.modifiers, key.pressed);
            }
            assert(clipboard.requests == requestsBeforeReplacement + 1 && clipboard.pending);
            clipboard.deliver("replacement-one\n\n\n\n\nreplacement-two");
            frame();
            assert(!sendPaste->inputArea()->pressed());
            freshTouchSurvivesMouseUp(heldX, heldY, "2");
            ui.setWindowActions({});
            frame();
            auto *replacementSend = composerAction(*composer, ":paste-send");
            assert(replacementSend && nativeVisible(*replacementSend) &&
                   dispatcher.focusedArea() == input->inputArea() && documentPreserved() &&
                   std::none_of(model.actions.begin(), model.actions.end(), [](const Action &a) {
                       return a.type == "send" || a.type == "command";
                   }));
            const auto actionsBeforeFreshSend = model.actions.size();
            pointerClick(*replacementSend);
            frame();
            assert(model.actions.size() == actionsBeforeFreshSend + 2 &&
                   model.actions[actionsBeforeFreshSend].type == "send" &&
                   model.actions[actionsBeforeFreshSend].buffer == buffer.id &&
                   model.actions[actionsBeforeFreshSend].text == "replacement-one" &&
                   model.actions[actionsBeforeFreshSend + 1].type == "send" &&
                   model.actions[actionsBeforeFreshSend + 1].buffer == buffer.id &&
                   model.actions[actionsBeforeFreshSend + 1].text == "replacement-two" &&
                   dispatcher.focusedArea() == input->inputArea() && documentPreserved());
            model.actions.clear();
            ui.execute("paste");
            input->selectAll();
            ui.flush(); // Publish the intervening selection before restoring the saved document.
            input->restoreEditingState(document);
            ui.flush(); // Real caret revisions make the outstanding clipboard response stale.
            clipboard.deliver("\x02one\x02\r\n\r\n/quit\r\n猫😀\r\n \r\nlast\r\n");
            frame();
            chrome("paste-warning", true);
            assert(nativeVisible(*pasteContext) && pasteContext->height() > 0 &&
                   pasteContext->text() != normalPasteContext &&
                   pasteContext->text().contains(buffer.target) && documentPreserved());
            if (width == 720 && scale == 2) {
                assert(pasteContext->text().find('\n') == std::string::npos);
                nativeSingleTextRow(renderer, *pasteContext);
            }
            const auto warningTooltip = pasteContext->tooltipContent();
            const auto *warningDetails = std::get_if<std::string>(&warningTooltip);
            assert(warningDetails && warningDetails->contains(buffer.target) &&
                   *warningDetails != *contextDetails && warningDetails->contains("/quit") &&
                   warningDetails->contains("猫😀"));
            within(*pasteContext, *composer);
            for (auto *button : {insertPaste, sendPaste, cancelPaste}) {
                assert(nativeVisible(*button) && button->inputArea()->enabled() &&
                       button->inputArea()->width() >= 32 && button->inputArea()->height() >= 32);
                within(*button, *composer);
                const auto hit = sceneBounds(*button->inputArea());
                assert(dispatcher.inputAreaAt(hit.x + hit.width / 2, hit.y + hit.height / 2) ==
                       button->inputArea());
            }
            activateComposerAction(*composer, ":paste-cancel");
            frame();
            chrome("paste-cancel", true);
            anchored("paste-cancel");
            assert(std::abs(composer->height() - replyHeight) <= .5f);
            activateComposerAction(*pane, ":reply-cancel");
            frame();
            chrome("reply-cancel", false);
            anchored("reply-cancel", true);
            assert(documentPreserved() && input->editingState().history->undo == documentUndo &&
                   input->editingState().history->redo == documentRedo &&
                   buffer.draft == document.current.value);
            assert(std::abs(composer->height() - oneLineHeight) <= .5f);

            ui.execute("format:bold");
            frame();
            chrome("format", false);
            ui.execute("undo");
            frame();
            assert(input->value() == document.current.value);
            input->selectAll();
            assert(input->insertText(
                "Line one אבג العربية\nLine two 日本語 猫 😀 👩‍💻\nLine "
                "three\nLine four\n"
                "Line five\nLine six\nLine seven\nLine eight"));
            const auto multiline = input->editingState();
            frame();
            chrome("multiline", false);
            anchored("multiline");
            const auto afterLayout = input->editingState();
            assert(afterLayout.current == multiline.current &&
                   afterLayout.history == multiline.history);
            assert(input->height() < input->measuredMultilineHeight());
            assert(std::isfinite(afterLayout.scrollY) && afterLayout.scrollY >= 0 &&
                   afterLayout.scrollY <= input->measuredMultilineHeight() - input->height() + .5f);
            if (width > 720)
                assert(input->height() > oneLineInput);
            ui.execute("undo");
            frame();
            chrome("multiline-undo", false);
            anchored("multiline-undo", true);
            assert(input->value() == document.current.value &&
                   std::abs(composer->height() - oneLineHeight) <= .5f);

            // Real Pango layout may clamp viewport pixels, never the native IME document.
            assert(dispatcher.focusedArea() == input->inputArea());
            static_cast<TextInputClient &>(*input).textInputApplyEdit({.commitText = {},
                                                                       .preeditText = "日本😀",
                                                                       .preeditCursorBegin = 10,
                                                                       .preeditCursorEnd = 10,
                                                                       .hasPreedit = true});
            const auto ime = input->editingState();
            const auto imeUndo = ime.history->undo, imeRedo = ime.history->redo;
            assert(ime.preeditBase);
            frame();
            chrome("preedit", false);
            anchored("preedit");
            const auto laidOutIme = input->editingState();
            assert(laidOutIme.current == ime.current && laidOutIme.preeditBase == ime.preeditBase &&
                   laidOutIme.preeditStart == ime.preeditStart &&
                   laidOutIme.preeditLength == ime.preeditLength &&
                   laidOutIme.preeditCaretHidden == ime.preeditCaretHidden &&
                   laidOutIme.history == ime.history && ime.history->undo == imeUndo &&
                   ime.history->redo == imeRedo);
            assert(std::isfinite(laidOutIme.scrollY) && laidOutIme.scrollY >= 0 &&
                   laidOutIme.scrollY <=
                       std::max(0.f, input->measuredMultilineHeight() - input->height()) + .5f);
            assert(input->textInputState().surroundingText == ime.preeditBase->value);
            const float preeditInputHeight = input->height();
            const auto compositionUnchanged = [&] {
                const auto state = input->editingState();
                assert(dispatcher.focusedArea() == input->inputArea() && input->composing() &&
                       state.current == ime.current && state.preeditBase == ime.preeditBase &&
                       state.preeditStart == ime.preeditStart &&
                       state.preeditLength == ime.preeditLength &&
                       state.preeditCaretHidden == ime.preeditCaretHidden &&
                       state.history == ime.history && state.history->undo == imeUndo &&
                       state.history->redo == imeRedo &&
                       std::abs(input->height() - preeditInputHeight) <= .5f);
            };
            anchored("preedit-before-panel-open", true);
            pointerClick(*actions);
            frame();
            assert(nativeVisible(*formattingPanel));
            anchored("preedit-panel-open");
            compositionUnchanged();
            for (auto *button : formattingButtons)
                assert(!button->inputArea()->enabled());
            InputArea *panelBackground = nullptr, *scrollbarArea = nullptr;
            const auto inspectPanel = [&](auto &&self, Node &node) -> void {
                if (auto *area = dynamic_cast<InputArea *>(&node)) {
                    assert(!area->focusOnPointerPress());
                    if (area->parent() == formattingPanel)
                        panelBackground = area;
                    if (dynamic_cast<Scrollbar *>(area->parent()) && nativeVisible(*area))
                        scrollbarArea = area;
                }
                for (const auto &child : node.children())
                    self(self, *child);
            };
            inspectPanel(inspectPanel, *formattingPanel);
            const auto clickDuringPreedit = [&](InputArea &area) {
                const auto bounds = sceneBounds(area);
                const float x = bounds.x + bounds.width / 2, y = bounds.y + bounds.height / 2;
                auto *hit = dispatcher.inputAreaAt(x, y);
                assert(hit);
                auto *ancestor = static_cast<Node *>(hit);
                while (ancestor && ancestor != formattingPanel)
                    ancestor = ancestor->parent();
                assert(ancestor == formattingPanel);
                for (const bool pressed : {true, false}) {
                    ++pointerSerial;
                    dispatcher.pointerButton(x, y, BTN_LEFT, pressed, pointerSerial,
                                             pointerSerial * 10, false);
                    compositionUnchanged();
                }
                frame();
                compositionUnchanged();
            };
            clickDuringPreedit(*bold->inputArea()); // Disabled decoration cannot end IME.
            bold->inputArea()->dispatchKey(XKB_KEY_Return, 0, 0, true);
            bold->inputArea()->dispatchKey(XKB_KEY_Return, 0, 0, false);
            ui.execute("format:bold");
            frame();
            compositionUnchanged();
            assert(panelBackground);
            clickDuringPreedit(*panelBackground);
            if (formattingPanel->scrollable()) {
                assert(scrollbarArea);
                clickDuringPreedit(*scrollbarArea);
            }
            anchored("preedit-before-panel-close");
            pointerClick(*actions);
            frame();
            assert(!nativeVisible(*formattingPanel));
            anchored("preedit-panel-close", true);
            compositionUnchanged();
            ui.execute("cancel");
            frame();
            chrome("preedit-cancel", false);
            anchored("preedit-cancel", true);
            assert(!input->composing() && input->value() == document.current.value);

            // Ui leaves active-IME keys to the real Input's composer callback.
            // Populate both history directions: C-y must not become SDK Redo.
            input->setValue("native killed text");
            input->selectAll();
            ui.execute("kill-region");
            frame();
            assert(input->value().empty());
            input->setValue("");
            assert(input->insertText("\x03"
                                     "04,02A猫B\x0fZ"));
            input->selectAll();
            assert(input->insertText("temporary"));
            ui.execute("undo");
            frame();
            auto clipboardBase = input->editingState();
            clipboardBase.current.selectionAnchor = 7;
            clipboardBase.current.cursorPos = 10;
            input->restoreEditingState(clipboardBase);
            frame();
            const auto clipboardRequests = clipboard.requests;
            ui.execute("paste");
            assert(clipboard.requests == clipboardRequests + 1 && clipboard.pending);
            input->textInputApplyEdit({.commitText = {},
                                       .preeditText = "日本😀",
                                       .preeditCursorBegin = 10,
                                       .preeditCursorEnd = 10,
                                       .hasPreedit = true});
            frame();
            const auto clipboardIme = input->editingState();
            const auto clipboardUndo = clipboardIme.history->undo;
            const auto clipboardRedo = clipboardIme.history->redo;
            const auto surrounding = input->textInputState();
            auto *clipboardCaret = nativeCaret(*input);
            assert(clipboardCaret && clipboardIme.preeditBase && !clipboardUndo.empty() &&
                   !clipboardRedo.empty() &&
                   clipboardIme.preeditBase->value == "\x03"
                                                      "04,02A猫B\x0fZ" &&
                   clipboardIme.current.value == "\x03"
                                                 "04,02A日本😀B\x0fZ" &&
                   surrounding.surroundingText == clipboardIme.preeditBase->value &&
                   surrounding.cursor == 10 && surrounding.anchor == 7 &&
                   surrounding.cursorRectWidth > 0 && surrounding.cursorRectHeight > 0);
            const auto caretBounds = sceneBounds(*clipboardCaret);
            auto *nativeCancelPaste = composerAction(*composer, ":paste-cancel");
            assert(nativeCancelPaste);
            const auto clipboardUnchanged = [&] {
                const auto state = input->textInputState();
                const auto bounds = sceneBounds(*clipboardCaret);
                assert(input->composing() && input->editingStateMatches(clipboardIme) &&
                       clipboardIme.history->undo == clipboardUndo &&
                       clipboardIme.history->redo == clipboardRedo &&
                       dispatcher.focusedArea() == input->inputArea() &&
                       input->value() == clipboardIme.current.value &&
                       state.surroundingText == surrounding.surroundingText &&
                       state.cursor == surrounding.cursor && state.anchor == surrounding.anchor &&
                       state.cursorRectX == surrounding.cursorRectX &&
                       state.cursorRectY == surrounding.cursorRectY &&
                       state.cursorRectWidth == surrounding.cursorRectWidth &&
                       state.cursorRectHeight == surrounding.cursorRectHeight &&
                       bounds.x == caretBounds.x && bounds.y == caretBounds.y &&
                       bounds.width == caretBounds.width && bounds.height == caretBounds.height &&
                       clipboard.requests == clipboardRequests + 1 &&
                       !nativeVisible(*nativeCancelPaste) && model.actions.empty());
            };
            const auto physicalKey = [](std::uint32_t sym) -> std::uint32_t {
                switch (sym) {
                case XKB_KEY_Control_L:
                    return KEY_LEFTCTRL;
                case XKB_KEY_Shift_L:
                    return KEY_LEFTSHIFT;
                case XKB_KEY_v:
                    return KEY_V;
                case XKB_KEY_Insert:
                    return KEY_INSERT;
                case XKB_KEY_y:
                    return KEY_Y;
                case XKB_KEY_F9:
                    return KEY_F9;
                case XKB_KEY_F8:
                    return KEY_F8;
                case XKB_KEY_Tab:
                    return KEY_TAB;
                case XKB_KEY_x:
                    return KEY_X;
                case XKB_KEY_p:
                    return KEY_P;
                case XKB_KEY_q:
                    return KEY_Q;
                case XKB_KEY_Escape:
                    return KEY_ESC;
                case XKB_KEY_eacute:
                case XKB_KEY_e:
                    return KEY_E;
                default:
                    assert(false);
                    return 0;
                }
            };
            model.settings.bindings["paste"] = "C-F9";
            model.settings.bindings["yank"] = "C-F8";
            for (const auto &[sym, modifiers] :
                 {std::pair{XKB_KEY_v, KeyMod::Ctrl}, std::pair{XKB_KEY_Insert, KeyMod::Shift},
                  std::pair{XKB_KEY_y, KeyMod::Ctrl}, std::pair{XKB_KEY_F9, KeyMod::Ctrl},
                  std::pair{XKB_KEY_F8, KeyMod::Ctrl}}) {
                const auto modifierSym =
                    modifiers == KeyMod::Shift ? XKB_KEY_Shift_L : XKB_KEY_Control_L;
                for (const bool preedit : {false, true})
                    for (const auto &[keySym, pressed] :
                         {std::pair{modifierSym, true}, std::pair{sym, true}, std::pair{sym, false},
                          std::pair{modifierSym, false}}) {
                        const InputArea::KeyData key{.sym = static_cast<std::uint32_t>(keySym),
                                                     .modifiers = modifiers,
                                                     .pressed = pressed,
                                                     .preedit = preedit};
                        if (!ui.handleKey(key, physicalKey(key.sym)))
                            dispatcher.keyEvent(key.sym, key.utf32, key.modifiers, key.pressed,
                                                key.preedit);
                        clipboardUnchanged();
                    }
                frame();
                clipboardUnchanged();
            }
            for (const auto *command : {"paste", "yank"}) {
                ui.execute(command);
                frame();
                clipboardUnchanged();
            }
            clipboard.deliver("late\nclipboard");
            frame();
            clipboardUnchanged();
            const auto compositionKey = [&](std::uint32_t sym, std::uint32_t utf32,
                                            std::uint32_t modifiers, bool pressed,
                                            bool preedit = false) {
                const InputArea::KeyData key{.sym = sym,
                                             .utf32 = utf32,
                                             .modifiers = modifiers,
                                             .pressed = pressed,
                                             .preedit = preedit};
                if (!ui.handleKey(key, physicalKey(sym)))
                    dispatcher.keyEvent(sym, utf32, modifiers, pressed, preedit);
            };
            model.settings.bindings["paste"] = "TAB";
            for (const bool pressed : {true, false}) {
                compositionKey(XKB_KEY_Tab, '\t', 0, pressed);
                clipboardUnchanged();
            }
            model.settings.bindings["paste"] = "C-x p";
            const auto pastePrefix = [&] {
                for (const auto &[sym, modifiers, pressed] :
                     {std::tuple{XKB_KEY_Control_L, KeyMod::Ctrl, true},
                      std::tuple{XKB_KEY_x, KeyMod::Ctrl, true},
                      std::tuple{XKB_KEY_x, KeyMod::Ctrl, true}, // Physical repeat, no release.
                      std::tuple{XKB_KEY_x, KeyMod::Ctrl, false},
                      std::tuple{XKB_KEY_Control_L, KeyMod::Ctrl, false}}) {
                    compositionKey(sym, 0, modifiers, pressed);
                    clipboardUnchanged();
                }
            };
            pastePrefix();
            for (const bool pressed : {true, true, false}) {
                compositionKey(XKB_KEY_p, 'p', 0, pressed);
                clipboardUnchanged();
            }
            model.settings.bindings["paste"] = "C-x C-v p";
            pastePrefix();
            for (const auto &[sym, modifiers, pressed] :
                 {std::tuple{XKB_KEY_Control_L, KeyMod::Ctrl, true},
                  std::tuple{XKB_KEY_v, KeyMod::Ctrl, true},
                  std::tuple{XKB_KEY_v, KeyMod::Ctrl, false},
                  std::tuple{XKB_KEY_Control_L, KeyMod::Ctrl, false}}) {
                compositionKey(sym, 0, modifiers, pressed);
                clipboardUnchanged();
            }
            for (const bool pressed : {true, true, false}) {
                compositionKey(XKB_KEY_p, 'p', 0, pressed);
                clipboardUnchanged();
            }
            model.settings.bindings["paste"] = "C-x p";
            pastePrefix();
            for (const bool pressed : {true, false}) {
                compositionKey(XKB_KEY_q, 'q', 0, pressed); // Failed chord cannot insert its tail.
                clipboardUnchanged();
            }
            pastePrefix();
            compositionKey(XKB_KEY_Escape, 0, 0, true);
            compositionKey(XKB_KEY_Escape, 0, 0, false);
            frame();
            assert(!input->composing() && input->value() == clipboardIme.preeditBase->value &&
                   input->editingState().history->undo == clipboardUndo &&
                   input->editingState().history->redo == clipboardRedo &&
                   clipboard.requests == clipboardRequests + 1);
            input->textInputApplyEdit({.commitText = {},
                                       .preeditText = "日本😀",
                                       .preeditCursorBegin = 10,
                                       .preeditCursorEnd = 10,
                                       .hasPreedit = true});
            frame();
            clipboardUnchanged();
            // A binding named for a composed character must not steal real IME text.
            model.settings.bindings["paste"] = "é";
            compositionKey(XKB_KEY_eacute, 0xe9, 0, true, true);
            compositionKey(XKB_KEY_e, 'e', 0, false, true);
            frame();
            assert(input->composing() &&
                   input->editingState().preeditBase == clipboardIme.preeditBase &&
                   input->value() == "\x03"
                                     "04,02AéB\x0fZ" &&
                   input->editingState().history->undo == clipboardUndo &&
                   input->editingState().history->redo == clipboardRedo &&
                   clipboard.requests == clipboardRequests + 1 &&
                   dispatcher.focusedArea() == input->inputArea());
            compositionKey(XKB_KEY_eacute, 0xe9, 0, true);
            compositionKey(XKB_KEY_e, 'e', 0, false);
            frame();
            assert(!input->composing() &&
                   input->value() == "\x03"
                                     "04,02AéB\x0fZ" &&
                   clipboard.requests == clipboardRequests + 1 &&
                   dispatcher.focusedArea() == input->inputArea());
            ui.execute("cancel");
            frame();
            dispatcher.setFocus(nullptr);
            dispatcher.setSceneRoot(nullptr);
        }

        const auto prepareBuffers = [](Model &model) {
            model.firstRun = false;
            model.now = 100;
            model.settings.reducedMotion = true;
            model.settings.fx = FxLevel::Off;
            model.settings.sound = model.settings.typingSend = false;
            model.identities.emplace_back();
            ServerConfig server;
            server.name = "test";
            server.host = "example.invalid";
            model.servers.push_back(server);
            model.networks["test"].name = "Native pane width network";
            model.networks["test"].state = "Ready";
            for (const auto &[id, kind, draft] :
                 {std::array<std::string_view, 3>{"test/#channel", "channel", "Channel draft"},
                  std::array<std::string_view, 3>{"test/Person", "query", "Query draft"}}) {
                auto &buffer = model.buffers[std::string(id)];
                buffer.id = id;
                buffer.server = "test";
                buffer.target = id.substr(5);
                buffer.kind = kind;
                buffer.joined = true;
                buffer.draft = draft;
            }
            model.activeBuffer = "test/#channel";
            model.settings.workspace = {
                {"1", "test/#channel"}, {"focused", "1"}, {"native_navigation_visible", "false"}};
        };
        // Exercise completion before the real Ui's send/completion/navigation bindings.
        {
            Model model;
            prepareBuffers(model);
            Theme theme;
            DeferredClipboard clipboard;
            InputDispatcher dispatcher;
            Ui ui(model, clipboard);
            dispatcher.setSceneRoot(&ui.root());
            ui.setInputDispatcher(dispatcher);
            ui.setWindowActive(true);
            float layoutWidth = 1440, layoutHeight = 940;
            const auto frame = [&](float width = 1440, float height = 940) {
                layoutWidth = width;
                layoutHeight = height;
                theme.update(model);
                context.setTextFontFamily(themeUiFont().family);
                ui.update(width, height, 1);
                ui.root().layout(renderer);
            };
            WindowActions windowActions{};
            windowActions.synchronizeLayout = [&] {
                frame(layoutWidth, layoutHeight);
                return true;
            };
            ui.setWindowActions(std::move(windowActions));
            const auto key = [&](std::uint32_t sym) {
                assert(ui.handleKey({.sym = sym, .pressed = true}));
                ui.handleKey({.sym = sym, .pressed = false});
            };
            frame();
            auto *input = inputWithDraft(ui.root(), "Channel draft");
            auto *popup = completionPopup(ui.root());
            assert(input && popup);
            dispatcher.setFocus(input->inputArea());
            // Real host preflight -> app capture -> Dispatcher, with no manual frame barriers.
            const auto batchKey = [&](std::uint32_t sym, std::uint32_t code, std::uint32_t utf32,
                                      std::uint32_t modifiers) {
                for (const bool pressed : {true, false}) {
                    if (pressed)
                        assert(ui.synchronizeInput(sym));
                    const InputArea::KeyData event{
                        .sym = sym, .utf32 = utf32, .modifiers = modifiers, .pressed = pressed};
                    if (!ui.handleKey(event, code))
                        dispatcher.keyEvent(sym, utf32, modifiers, pressed);
                }
                assert(input->cursorPosition() <= input->value().size() &&
                       input->selectionAnchor() <= input->value().size());
            };
            command_completion::View *nickPopup = nullptr;
            for (const auto &child : ui.root().children())
                if (auto *view = dynamic_cast<command_completion::View *>(child.get()))
                    nickPopup = view;
            assert(nickPopup);
            model.buffers.at("test/#channel").nicks = {
                {.name = "Alice", .account = {}, .prefix = {}},
                {.name = "Alfred", .account = {}, .prefix = {}},
                {.name = "éclair", .account = {}, .prefix = {}},
                {.name = "猫友", .account = {}, .prefix = {}},
                {.name = "😀Peer", .account = {}, .prefix = {}}};
            input->setValue("Ali");
            frame();
            const auto nickBefore = input->editingState();
            const auto nickUndo = nickBefore.history->undo.size();
            assert(!nickPopup->visible());
            batchKey(XKB_KEY_Tab, KEY_TAB, '\t', 0);
            assert(nickPopup->visible() && input->editingState().current == nickBefore.current &&
                   input->editingState().history->undo.size() == nickUndo &&
                   model.actions.empty() && dispatcher.focusedArea() == input->inputArea());
            auto *nickCandidate = contextText(*nickPopup, "Alice");
            assert(nickCandidate && nativeVisible(*nickCandidate) &&
                   nickCandidate->text() == "Alice" && !nickCandidate->useMarkup());
            // The real Ui must consume Enter before its send binding, including the release.
            batchKey(XKB_KEY_Return, KEY_ENTER, '\r', 0);
            assert(input->value() == "Alice: " && !nickPopup->visible() && model.actions.empty() &&
                   input->editingState().history->undo.size() == nickUndo + 1 &&
                   dispatcher.focusedArea() == input->inputArea());
            ui.execute("undo");
            frame();
            assert(input->editingState().current == nickBefore.current && !nickPopup->visible());
            input->setValue("Al");
            frame();
            batchKey(XKB_KEY_Tab, KEY_TAB, '\t', 0);
            assert(nickPopup->visible() && input->value() == "Al" && model.actions.empty());
            batchKey(XKB_KEY_Down, KEY_DOWN, 0, 0);
            assert(input->value() == "Al");
            batchKey(XKB_KEY_Tab, KEY_TAB, '\t', 0);
            assert(input->value() == "Alice: " && !nickPopup->visible() && model.actions.empty() &&
                   dispatcher.focusedArea() == input->inputArea());
            input->setValue("Al");
            frame();
            batchKey(XKB_KEY_Tab, KEY_TAB, '\t', 0);
            batchKey(XKB_KEY_Escape, KEY_ESC, 0, 0);
            assert(input->value() == "Al" && !nickPopup->visible() && model.actions.empty() &&
                   dispatcher.focusedArea() == input->inputArea());
            batchKey(XKB_KEY_i, KEY_I, 'i', 0);
            assert(input->value() == "Ali" && !nickPopup->visible());
            // Native Tab must respect 2/3/4-byte scalar starts, hidden controls and raw suffixes.
            for (const auto &[typed, nick] :
                 {std::pair{"é", "éclair"}, std::pair{"猫", "猫友"}, std::pair{"😀", "😀Peer"}}) {
                for (const std::string &prefix :
                     {std::string{}, std::string{"hello "}, std::string{"\x02"}}) {
                    const std::string suffix = "友\x0f 日本😀";
                    input->setValue(prefix + typed + suffix);
                    auto unicodeNick = input->editingState();
                    unicodeNick.current.cursorPos = unicodeNick.current.selectionAnchor =
                        prefix.size() + std::string_view(typed).size();
                    input->restoreEditingState(unicodeNick);
                    frame();
                    unicodeNick = input->editingState();
                    const auto unicodeUndo = unicodeNick.history->undo,
                               unicodeRedo = unicodeNick.history->redo;
                    assert(!nickPopup->visible());
                    batchKey(XKB_KEY_Tab, KEY_TAB, '\t', 0);
                    frame();
                    auto *candidate = contextText(*nickPopup, nick);
                    assert(nickPopup->visible() && candidate && nativeVisible(*candidate) &&
                           candidate->text() == nick && !candidate->useMarkup() &&
                           input->editingState().current == unicodeNick.current &&
                           unicodeNick.history->undo == unicodeUndo &&
                           unicodeNick.history->redo == unicodeRedo && model.actions.empty() &&
                           dispatcher.focusedArea() == input->inputArea());
                    if (prefix == "hello ")
                        batchKey(XKB_KEY_Return, KEY_ENTER, '\r', 0);
                    else
                        batchKey(XKB_KEY_Tab, KEY_TAB, '\t', 0);
                    const std::string inserted =
                        std::string(nick) + (prefix == "hello " ? "" : ": ");
                    assert(input->value() == prefix + inserted + suffix && !nickPopup->visible() &&
                           input->cursorPosition() == prefix.size() + inserted.size() &&
                           input->selectionAnchor() == input->cursorPosition() &&
                           input->editingState().history->undo.size() == unicodeUndo.size() + 1 &&
                           input->editingState().history->undo.back() == unicodeNick.current &&
                           model.actions.empty() && dispatcher.focusedArea() == input->inputArea());
                    ui.execute("undo");
                    frame();
                    assert(input->editingState().current == unicodeNick.current &&
                           !nickPopup->visible());
                }
            }
            input->setValue("abcXYZ");
            frame(); // Shape the original six bytes, never the edited five-byte snapshot.
            batchKey(XKB_KEY_a, KEY_A, 1, KeyMod::Ctrl);
            assert(input->cursorPosition() == 0 && input->selectionAnchor() == 0);
            batchKey(XKB_KEY_d, KEY_D, 4, KeyMod::Ctrl);
            assert(input->value() == "bcXYZ");
            batchKey(XKB_KEY_End, KEY_END, 0, 0);
            assert(input->cursorPosition() == 5 && input->selectionAnchor() == 5 &&
                   model.buffers.at("test/#channel").draft == "bcXYZ");
            batchKey(XKB_KEY_h, KEY_H, 8, KeyMod::Ctrl);
            assert(input->value() == "bcXY" && input->cursorPosition() == 4 &&
                   input->selectionAnchor() == 4 &&
                   model.buffers.at("test/#channel").draft == "bcXY");
            batchKey(XKB_KEY_Return, KEY_ENTER, '\r', 0);
            assert(model.actions.size() == 1 && model.actions.front().type == "send" &&
                   model.actions.front().text == "bcXY" && input->value().empty());
            model.actions.clear();
            // Click the painted wrapped composition, then map prefix/inside/suffix to its base.
            const std::string preedit = "日本😀" + std::string(160, 'W') + "\nwrapped";
            std::uint32_t clickSerial = 30;
            for (const auto &[paintedOffset, committedOffset] :
                 {std::pair<std::size_t, std::size_t>{1, 1}, {5, 2}, {2 + preedit.size() + 1, 6}}) {
                input->setValue("ab猫XYZtail");
                auto middle = input->editingState();
                middle.current.selectionAnchor = 2;
                middle.current.cursorPos = 5; // Preedit replaces the selected CJK character.
                input->restoreEditingState(middle);
                input->textInputApplyEdit({.commitText = {},
                                           .preeditText = preedit,
                                           .preeditCursorBegin = 10,
                                           .preeditCursorEnd = 10,
                                           .hasPreedit = true});
                auto composing = input->editingState();
                composing.current.cursorPos = composing.current.selectionAnchor = paintedOffset;
                input->restoreEditingState(composing);
                frame();
                const auto paintedCaret = input->textInputState();
                const float x = paintedCaret.cursorRectX;
                const float y = paintedCaret.cursorRectY + paintedCaret.cursorRectHeight / 2;
                const auto undo = composing.history->undo, redo = composing.history->redo;
                assert(composing.preeditBase &&
                       input->measuredMultilineHeight() > 2 * paintedCaret.cursorRectHeight &&
                       dispatcher.inputAreaAt(x, y) == input->inputArea());
                for (const bool pressed : {true, false}) {
                    if (pressed)
                        ui.beginPointerInteraction(x, y);
                    ++clickSerial;
                    if (!ui.handlePointerButton(BTN_LEFT, pressed, x, y))
                        assert(dispatcher.pointerButton(x, y, BTN_LEFT, pressed, clickSerial,
                                                        clickSerial * 1000, false));
                    if (pressed) {
                        // A deferred press can be followed by drag motion in the same dispatch.
                        const auto committedCaret = input->textInputState();
                        dispatcher.pointerMotion(committedCaret.cursorRectX,
                                                 committedCaret.cursorRectY +
                                                     committedCaret.cursorRectHeight / 2,
                                                 ++clickSerial);
                        assert(input->cursorPosition() == committedOffset &&
                               input->selectionAnchor() == committedOffset);
                    }
                }
                assert(!input->composing() && input->value() == "ab猫XYZtail" &&
                       input->cursorPosition() == committedOffset &&
                       input->selectionAnchor() == committedOffset &&
                       input->editingState().history == composing.history &&
                       composing.history->undo == undo && composing.history->redo == redo);
            }
            // Equal surrounding bytes cannot move the native replacement to another occurrence.
            for (const auto &[start, end, text, hit, expected] :
                 {std::tuple<std::size_t, std::size_t, std::string, std::size_t, std::size_t>{
                      0, 0, "a", 1, 0},
                  {0, 0, "aa", 1, 0},
                  {4, 4, "aa", 5, 4},
                  {1, 3, "aa", 2, 1},
                  {1, 2, "a", 2, 2}}) {
                // Isolate single presses from the preceding case's toolkit multi-click sequence.
                for (const bool pressed : {true, false})
                    input->inputArea()->dispatchPress(0, 0, BTN_MIDDLE, pressed);
                input->setValue("aaaa");
                auto base = input->editingState();
                base.current.selectionAnchor = start;
                base.current.cursorPos = end;
                input->restoreEditingState(base);
                input->textInputApplyEdit({.commitText = {},
                                           .preeditText = text,
                                           .preeditCursorBegin = static_cast<int>(text.size()),
                                           .preeditCursorEnd = static_cast<int>(text.size()),
                                           .hasPreedit = true});
                auto composing = input->editingState();
                assert(composing.preeditBase && composing.preeditStart == start &&
                       composing.preeditLength == text.size());
                composing.current.cursorPos = composing.current.selectionAnchor = hit;
                input->restoreEditingState(composing);
                frame();
                const auto paintedCaret = input->textInputState();
                const float x = paintedCaret.cursorRectX;
                const float y = paintedCaret.cursorRectY + paintedCaret.cursorRectHeight / 2;
                const auto undo = composing.history->undo, redo = composing.history->redo;
                assert(dispatcher.inputAreaAt(x, y) == input->inputArea());
                for (const bool pressed : {true, false}) {
                    if (pressed)
                        ui.beginPointerInteraction(x, y);
                    ++clickSerial;
                    if (!ui.handlePointerButton(BTN_LEFT, pressed, x, y))
                        assert(dispatcher.pointerButton(x, y, BTN_LEFT, pressed, clickSerial,
                                                        clickSerial * 1000, false));
                }
                assert(!input->composing() && input->value() == "aaaa" &&
                       input->cursorPosition() == expected &&
                       input->selectionAnchor() == expected &&
                       input->editingState().history == composing.history &&
                       composing.history->undo == undo && composing.history->redo == redo);
            }
            // The command palette is an actual plain native Input, not a composer projection.
            ui.execute("switch-buffer");
            frame();
            auto *picker = dispatcher.focusedArea()
                               ? dynamic_cast<Input *>(dispatcher.focusedArea()->textInputClient())
                               : nullptr;
            assert(picker && picker != input && !picker->displayProjection());
            for (const auto &[sym, code, expected] :
                 {std::tuple{XKB_KEY_Home, KEY_HOME, std::size_t{0}},
                  std::tuple{XKB_KEY_End, KEY_END, std::size_t{6}}}) {
                picker->setValue("abcXYZ");
                auto base = picker->editingState();
                base.current.cursorPos = base.current.selectionAnchor = 2;
                picker->restoreEditingState(base);
                picker->textInputApplyEdit({.commitText = {},
                                            .preeditText = "日本😀",
                                            .preeditCursorBegin = 10,
                                            .preeditCursorEnd = 10,
                                            .hasPreedit = true});
                frame();
                const auto composing = picker->editingState();
                const auto undo = composing.history->undo, redo = composing.history->redo;
                batchKey(sym, code, 0, 0);
                assert(!picker->composing() && picker->value() == "abcXYZ" &&
                       picker->cursorPosition() == expected &&
                       picker->selectionAnchor() == expected &&
                       picker->editingState().history == composing.history &&
                       composing.history->undo == undo && composing.history->redo == redo);
            }
            ui.execute("cancel");
            frame();
            dispatcher.setFocus(input->inputArea());
            input->setValue("\x02:smle");
            frame();
            assert(nativeVisible(*popup) && dispatcher.focusedArea() == input->inputArea());
            within(*popup, ui.root());
            key(XKB_KEY_Return);
            frame();
            assert(input->value() == "\x02"
                                     "😄" &&
                   !popup->visible() && model.actions.empty() &&
                   dispatcher.focusedArea() == input->inputArea());
            key(XKB_KEY_Return);
            frame();
            assert(model.actions.size() == 1 && model.actions.front().type == "send" &&
                   model.actions.front().text == "\x02"
                                                 "😄" &&
                   input->value().empty());
            model.actions.clear();
            input->setValue(":");
            frame();
            key(XKB_KEY_Down);
            key(XKB_KEY_Up);
            key(XKB_KEY_Tab);
            frame();
            assert(input->value() == "😄" && model.actions.empty() &&
                   dispatcher.focusedArea() == input->inputArea());
            ui.execute("undo");
            frame();
            assert(input->value() == ":" && popup->visible());
            key(XKB_KEY_Escape);
            frame();
            assert(input->value() == ":" && !popup->visible());
            assert(input->insertText("smle"));
            frame();
            assert(popup->visible());
            key(XKB_KEY_Return);
            frame();
            assert(input->value() == "😄" && model.actions.empty());
            input->setValue(":");
            frame();
            for (size_t row = 1; row < emoji::maxResults; ++row)
                key(XKB_KEY_Down);
            frame();
            ScrollView *scroll = nullptr;
            for (const auto &child : popup->children())
                if (auto *view = dynamic_cast<ScrollView *>(child.get()))
                    scroll = view;
            assert(scroll && scroll->content()->children().size() == emoji::maxResults);
            auto *last = dynamic_cast<InputArea *>(scroll->content()->children().back().get());
            assert(last && last->passiveTouchPress());
            within(*last, *popup);
            const float generous = popup->height();
            frame(720, 300);
            assert(popup->visible() && popup->height() < generous && scroll->scrollable() &&
                   scroll->scrollOffset() > 0 && dispatcher.focusedArea() == input->inputArea());
            within(*popup, ui.root());
            within(*last, *popup);
            model.settings.scale = 1.5f;
            model.settings.messageFont = "monospace";
            frame(720, 480);
            within(*popup, ui.root());
            within(*last, *popup);
            const auto hit = sceneBounds(*last);
            const float x = hit.x + hit.width / 2, y = hit.y + hit.height / 2;
            assert(dispatcher.inputAreaAt(x, y) == last);
            for (const bool pressed : {true, false}) {
                assert(dispatcher.pointerButton(x, y, BTN_LEFT, pressed, pressed ? 21 : 22,
                                                pressed ? 1000 : 1010, false));
                assert(dispatcher.focusedArea() == input->inputArea());
            }
            frame(720, 480);
            emoji::Catalog catalog;
            assert(input->value() == catalog.matches("").back().glyph && !popup->visible() &&
                   model.actions.empty() && dispatcher.focusedArea() == input->inputArea());
            model.settings.scale = 1;
            model.settings.messageFont = "sans-serif";
            frame(720, 480);
            auto *pane = conversationPane(*input);
            assert(pane);
            activateTooltip(*pane, "Formatting toolbar");
            frame(720, 480);
            auto *lastBackground = tooltipAction(*pane, "Background IRC color 15");
            assert(lastBackground && nativeVisible(*lastBackground));
            ScrollView *formatScroll = nullptr;
            for (auto *ancestor = lastBackground->parent(); ancestor && ancestor != pane;
                 ancestor = ancestor->parent())
                if (auto *view = dynamic_cast<ScrollView *>(ancestor))
                    formatScroll = view;
            assert(formatScroll && formatScroll->scrollable());
            dispatcher.setFocus(lastBackground->inputArea());
            frame(720, 480);
            within(*lastBackground, *formatScroll);
            assert(dispatcher.focusedArea() == lastBackground->inputArea());
            formatScroll->requestScrollToOffset(0);
            frame(720, 480);
            frame(720, 480); // Stable geometry preserves manual scroll despite retained focus.
            assert(formatScroll->scrollOffset() == 0 &&
                   dispatcher.focusedArea() == lastBackground->inputArea());
            for (const auto size : {LayoutSize{600, 480}, LayoutSize{600, 240}}) {
                frame(size.width, size.height);
                assert(dispatcher.focusedArea() == lastBackground->inputArea() &&
                       formatScroll->scrollOffset() > 0);
                within(*lastBackground, *formatScroll);
                const auto control = sceneBounds(*lastBackground->inputArea());
                const auto content = sceneBounds(*formatScroll->content());
                const float localTop = control.y - content.y;
                assert(localTop >= formatScroll->scrollOffset() - .5f &&
                       localTop + control.height <= formatScroll->scrollOffset() +
                                                        formatScroll->contentViewportHeight() +
                                                        .5f);
                assert(dispatcher.inputAreaAt(control.x + control.width / 2,
                                              control.y + control.height / 2) ==
                       lastBackground->inputArea());
            }
            dispatcher.setFocus(nullptr);
            dispatcher.setSceneRoot(nullptr);
        }
        // Measure the actual consumer allocation, not only preferred-height helper values.
        {
            Model model;
            prepareBuffers(model);
            Theme theme;
            DeferredClipboard clipboard;
            InputDispatcher dispatcher;
            Ui ui(model, clipboard);
            dispatcher.setSceneRoot(&ui.root());
            ui.setInputDispatcher(dispatcher);
            ui.setWindowActive(true);
            const auto frame = [&](float width = 1440, float height = 940) {
                theme.update(model);
                context.setTextFontFamily(themeUiFont().family);
                ui.update(width, height, 1);
                ui.root().layout(renderer);
            };
            frame();
            auto *input = inputWithDraft(ui.root(), "Channel draft");
            assert(input);
            dispatcher.setFocus(input->inputArea());
            for (const auto *family : {"sans-serif", "monospace"})
                for (const float zoom : {.7f, 1.f, 1.5f, 2.f}) {
                    model.settings.messageFont = family;
                    model.settings.scale = zoom;
                    float emptyHeight = 0;
                    for (const auto *raw : {"", "Hi",
                                            "\x02"
                                            "Hi\x02",
                                            "\x03"
                                            "04,02Hi\x0f"}) {
                        input->setValue(raw);
                        frame();
                        const auto state = input->editingState();
                        auto *clip = composerAction(*conversationPane(*input), "upload");
                        assert(clip && clip->visible() == !*raw &&
                               clip->inputArea()->enabled() == !*raw);
                        if (!*raw)
                            within(*clip, *input);
                        assert(input->value() == raw && nativeVisible(*input) &&
                               dispatcher.focusedArea() == input->inputArea() &&
                               input->height() > 0 && std::isfinite(input->height()));
                        if (!*raw)
                            emptyHeight = input->height();
                        else
                            assert(std::abs(input->height() - emptyHeight) <= .5f);
                        assert(state.scrollY == 0 &&
                               input->measuredMultilineHeight() <= input->height() + .5f);
                    }
                    input->setValue("first\nsecond");
                    frame();
                    const float twoRows = input->height();
                    assert(twoRows >= emptyHeight &&
                           input->height() + .5f >= input->measuredMultilineHeight());
                    std::string wrapped;
                    for (int repeat = 0; repeat < 160; ++repeat)
                        wrapped += "\x02"
                                   "Wide\x02"
                                   " 猫 אב 👩‍💻 ";
                    input->setValue(wrapped);
                    frame(720, 480);
                    const auto end = input->editingState();
                    assert(input->height() >= emptyHeight &&
                           input->measuredMultilineHeight() > input->height() &&
                           std::isfinite(end.scrollY) && end.scrollY > 0 &&
                           end.scrollY <= input->measuredMultilineHeight() - input->height() + .5f);
                    const float capped = input->height();
                    assert(input->insertText(wrapped));
                    frame(720, 480);
                    assert(std::abs(input->height() - capped) <= .5f &&
                           input->measuredMultilineHeight() > input->height() &&
                           input->editingState().scrollY > end.scrollY);
                    dispatcher.keyEvent(XKB_KEY_Home, 0, KeyMod::Ctrl, true);
                    dispatcher.keyEvent(XKB_KEY_Home, 0, KeyMod::Ctrl, false);
                    frame(720, 480);
                    assert(input->cursorPosition() == 0 && input->editingState().scrollY == 0);
                    dispatcher.keyEvent(XKB_KEY_End, 0, KeyMod::Ctrl, true);
                    dispatcher.keyEvent(XKB_KEY_End, 0, KeyMod::Ctrl, false);
                    frame(720, 480);
                    assert(input->cursorPosition() == input->value().size() &&
                           input->editingState().scrollY > 0);
                    ui.execute("undo");
                    frame(720, 480);
                    assert(input->value() == wrapped);
                }
            dispatcher.setFocus(nullptr);
            dispatcher.setSceneRoot(nullptr);
        }
        struct NativeComposer {
            Input *input;
            Node *pane, *composer, *card;
            Button *actions, *send;
            std::array<InputArea *, 3> areas() const {
                return {input->inputArea(), actions->inputArea(), send->inputArea()};
            }
        };
        const auto nativeComposer = [](Input &input) {
            auto *pane = conversationPane(input);
            assert(pane);
            auto *composer = input.parent();
            while (composer->parent() != pane)
                composer = composer->parent();
            Node *card = nullptr;
            const auto allocated = sceneBounds(*composer);
            for (const auto &child : pane->children())
                if (auto *material = dynamic_cast<MaterialNode *>(child.get());
                    material && nativeVisible(*material)) {
                    const auto painted = sceneBounds(*material);
                    if (painted.x <= allocated.x && painted.y <= allocated.y &&
                        painted.x + painted.width >= allocated.x + allocated.width &&
                        painted.y + painted.height >= allocated.y + allocated.height) {
                        assert(!card);
                        card = material;
                    }
                }
            auto *actions = tooltipAction(*composer, "Formatting toolbar");
            auto *send = tooltipAction(*composer, "Send message");
            assert(card && actions && send);
            return NativeComposer{&input, pane, composer, card, actions, send};
        };
        const auto disjoint = [](const Node &first, const Node &second) {
            const auto a = sceneBounds(first), b = sceneBounds(second);
            assert(a.x + a.width <= b.x + .5f || b.x + b.width <= a.x + .5f ||
                   a.y + a.height <= b.y + .5f || b.y + b.height <= a.y + .5f);
        };
        const auto usable = [&](const NativeComposer &view, Ui &ui, InputDispatcher &dispatcher) {
            within(*view.pane, ui.root());
            within(*view.card, *view.pane);
            within(*view.composer, *view.card);
            const std::array<Node *, 3> controls{view.input, view.actions, view.send};
            const auto areas = view.areas();
            for (std::size_t i = 0; i < controls.size(); ++i) {
                for (const Node *node : {controls[i], static_cast<Node *>(areas[i])}) {
                    assert(nativeVisible(*node) && node->width() > 0 && node->height() > 0);
                    within(*node, *view.composer);
                    within(*node, *view.card);
                    within(*node, *view.pane);
                    within(*node, ui.root());
                }
                assert(areas[i]->enabled());
                const auto hit = sceneBounds(*areas[i]);
                assert(dispatcher.inputAreaAt(hit.x + hit.width / 2, hit.y + hit.height / 2) ==
                       areas[i]);
                for (std::size_t j = i + 1; j < controls.size(); ++j) {
                    disjoint(*controls[i], *controls[j]);
                    disjoint(*areas[i], *areas[j]);
                }
            }
        };
        const auto sameRect = [](std::string_view stage, const LayoutRect &a, const LayoutRect &b,
                                 const Model &model) {
            const bool same =
                a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
            if (!same)
                std::fprintf(stderr,
                             "native composer rectangle: stage=%.*s focused=%s "
                             "navigationIntent=%s navigationShown=%s "
                             "actual=(%g,%g,%g,%g) expected=(%g,%g,%g,%g)\n",
                             int(stage.size()), stage.data(),
                             model.settings.workspace.at("focused").c_str(),
                             model.settings.workspace.at("native_navigation_visible").c_str(),
                             model.settings.workspace.at("native_navigation_shown").c_str(), a.x,
                             a.y, a.width, a.height, b.x, b.y, b.width, b.height);
            assert(same);
        };

        // Consume the real native Input/Button allocations, not a duplicate minimum-width formula.
        {
            Model model;
            prepareBuffers(model);
            Theme theme;
            DeferredClipboard clipboard;
            InputDispatcher dispatcher;
            Ui ui(model, clipboard);
            dispatcher.setSceneRoot(&ui.root());
            ui.setInputDispatcher(dispatcher);
            ui.setWindowActive(true);
            const auto frame = [&](float width, float height) {
                theme.update(model);
                context.setTextFontFamily(themeUiFont().family);
                ui.update(width, height, 1);
                ui.root().layout(renderer);
            };
            frame(1440, 940);
            const std::array<std::string, 2> bufferIds{"test/#channel", "test/Person"};
            const std::array<std::string_view, 2> drafts{"Channel draft", "Query draft"};
            std::array<Input *, 2> inputs{};
            std::array<NativeComposer, 2> views{};
            std::array<LayoutRect, 2> generous{};
            std::array<Input::EditingState, 2> documents;
            for (std::size_t i = 0; i < inputs.size(); ++i) {
                ui.execute("buffer:" + bufferIds[i]);
                frame(1440, 940);
                inputs[i] = inputWithDraft(ui.root(), drafts[i]);
                assert(inputs[i] && nativeVisible(*inputs[i]));
                views[i] = nativeComposer(*inputs[i]);
                generous[i] = sceneBounds(*views[i].pane);
                assert(inputs[i]->insertText("!"));
                ui.execute("undo");
                auto selected = inputs[i]->editingState();
                selected.current.cursorPos = 3;
                selected.current.selectionAnchor = 1;
                inputs[i]->restoreEditingState(selected);
                documents[i] = inputs[i]->editingState();
            }
            const auto undo = std::array{documents[0].history->undo, documents[1].history->undo};
            const auto redo = std::array{documents[0].history->redo, documents[1].history->redo};
            ui.execute("buffer:" + bufferIds[0]);
            frame(1440, 940);
            ui.flush();
            const auto unchanged = [&] {
                ui.flush();
                for (std::size_t i = 0; i < inputs.size(); ++i) {
                    const auto state = inputs[i]->editingState();
                    assert(inputWithDraft(ui.root(), documents[i].current.value) == inputs[i] &&
                           conversationPane(*inputs[i]) == views[i].pane);
                    assert(state.current == documents[i].current &&
                           state.history == documents[i].history &&
                           state.history->undo == undo[i] && state.history->redo == redo[i] &&
                           !state.preeditBase);
                    assert(nativeVisible(*views[i].pane) == (model.activeBuffer == bufferIds[i]));
                }
            };
            for (float scale : {.7f, 1.f, 1.5f, 2.f}) {
                model.settings.scale = scale;
                for (const auto size : {LayoutSize{1440, 940}, LayoutSize{720, 480}})
                    for (std::uint64_t focus : {1, 2}) {
                        std::fprintf(stderr, "native tab composer: %gx%g zoom %g focus %llu\n",
                                     size.width, size.height, scale,
                                     static_cast<unsigned long long>(focus));
                        ui.execute("buffer:" + bufferIds[focus - 1]);
                        frame(size.width, size.height);
                        assert(inputWithDraft(ui.root(), documents[focus - 1].current.value) ==
                               inputs[focus - 1]);
                        views[focus - 1] = nativeComposer(*inputs[focus - 1]);
                        assert(dispatcher.focusedArea() == inputs[focus - 1]->inputArea());
                        usable(views[focus - 1], ui, dispatcher);
                        assert(model.settings.workspace.at("visible_buffers") ==
                               (focus == 1 ? "test/#channel" : "test/Person"));
                        unchanged();
                    }
            }
            // Resizing and navigation preserve each cached buffer's native editor.
            model.settings.scale = 1;
            for (std::size_t i = 0; i < views.size(); ++i) {
                ui.execute("buffer:" + bufferIds[i]);
                frame(1440, 940);
                assert(inputWithDraft(ui.root(), documents[i].current.value) == inputs[i]);
                views[i] = nativeComposer(*inputs[i]);
                unchanged();
                sameRect("matrix-restore", sceneBounds(*views[i].pane), generous[i], model);
            }
            dispatcher.setFocus(nullptr);
            dispatcher.setSceneRoot(nullptr);
        }

        // Hidden buffer history/selection and active native IME survive compact roundtrips.
        {
            Model model;
            prepareBuffers(model);
            model.settings.scale = 2;
            Theme theme;
            DeferredClipboard clipboard;
            InputDispatcher dispatcher;
            Ui ui(model, clipboard);
            dispatcher.setSceneRoot(&ui.root());
            ui.setInputDispatcher(dispatcher);
            ui.setWindowActive(true);
            const auto frame = [&](float width, float height) {
                theme.update(model);
                context.setTextFontFamily(themeUiFont().family);
                ui.update(width, height, 1);
                ui.root().layout(renderer);
            };
            frame(2000, 1200);
            const std::array<std::string, 2> bufferIds{"test/#channel", "test/Person"};
            const std::array<std::string_view, 2> drafts{"Channel draft", "Query draft"};
            std::array<Input *, 2> inputs{};
            inputs[0] = inputWithDraft(ui.root(), drafts[0]);
            assert(inputs[0] && nativeVisible(*inputs[0]));
            assert(inputs[0]->insertText("!"));
            ui.execute("undo");
            auto selection = inputs[0]->editingState();
            selection.current.cursorPos = 3;
            selection.current.selectionAnchor = 1;
            inputs[0]->restoreEditingState(selection);
            const auto channelState = inputs[0]->editingState();
            const auto channelUndo = channelState.history->undo;
            const auto channelRedo = channelState.history->redo;
            std::array<Node *, 2> panes{};
            std::array<LayoutRect, 2> generous{};
            for (std::size_t i = 0; i < inputs.size(); ++i) {
                ui.execute("buffer:" + bufferIds[i]);
                frame(2000, 1200);
                inputs[i] = inputWithDraft(ui.root(), drafts[i]);
                assert(inputs[i] && nativeVisible(*inputs[i]));
                panes[i] = conversationPane(*inputs[i]);
                assert(panes[i] && nativeVisible(*panes[i]));
                generous[i] = sceneBounds(*panes[i]);
            }
            const auto intent = [&] {
                ui.flush();
                for (std::size_t i = 0; i < inputs.size(); ++i) {
                    assert(conversationPane(*inputs[i]) == panes[i]);
                    assert(nativeVisible(*panes[i]) == (model.activeBuffer == bufferIds[i]));
                }
                const auto state = inputs[0]->editingState();
                assert(state.current == channelState.current &&
                       state.history == channelState.history &&
                       state.history->undo == channelUndo && state.history->redo == channelRedo);
            };
            for (std::uint64_t focus : {1, 2}) {
                ui.execute("buffer:" + bufferIds[focus - 1]);
                frame(720, 480);
                assert(inputWithDraft(ui.root(), drafts[focus - 1]) == inputs[focus - 1] &&
                       nativeVisible(*inputs[focus - 1]));
                nativeComposer(*inputs[focus - 1]);
                intent();
            }
            auto *input = inputs[1];
            static_cast<TextInputClient &>(*input).textInputApplyEdit({.commitText = {},
                                                                       .preeditText = "日本😀",
                                                                       .preeditCursorBegin = 10,
                                                                       .preeditCursorEnd = 10,
                                                                       .hasPreedit = true});
            const auto ime = input->editingState();
            const auto undo = ime.history->undo, redo = ime.history->redo;
            assert(ime.preeditBase);
            for (const auto size : {LayoutSize{2000, 1200}, LayoutSize{720, 480}}) {
                frame(size.width, size.height);
                intent();
                const auto state = input->editingState();
                assert(state.current == ime.current && state.preeditBase == ime.preeditBase &&
                       state.preeditStart == ime.preeditStart &&
                       state.preeditLength == ime.preeditLength &&
                       state.preeditCaretHidden == ime.preeditCaretHidden &&
                       state.history == ime.history && state.history->undo == undo &&
                       state.history->redo == redo);
                assert(dispatcher.focusedArea() == input->inputArea() &&
                       input->textInputState().surroundingText == ime.preeditBase->value);
            }
            ui.execute("cancel");
            for (std::size_t i = 0; i < panes.size(); ++i) {
                ui.execute("buffer:" + bufferIds[i]);
                frame(2000, 1200);
                assert(inputWithDraft(ui.root(), drafts[i]) == inputs[i] &&
                       nativeVisible(*inputs[i]));
                nativeComposer(*inputs[i]);
                intent();
                sameRect("buffer-restore", sceneBounds(*panes[i]), generous[i], model);
            }
            dispatcher.setFocus(nullptr);
            dispatcher.setSceneRoot(nullptr);
        }

        // Impossible aggregate: native controls may be clipped, never paint or receive hits
        // across their real composer/card/pane boundaries. Read before swap, just like Capture.
        {
            wl_compositor *compositor = nullptr;
            auto *registry = wl_display_get_registry(display);
            const wl_registry_listener listener{
                [](void *data, wl_registry *registry, std::uint32_t name, const char *interface,
                   std::uint32_t version) {
                    if (std::string_view(interface) == "wl_compositor")
                        *static_cast<wl_compositor **>(data) =
                            static_cast<wl_compositor *>(wl_registry_bind(
                                registry, name, &wl_compositor_interface, std::min(version, 4u)));
                },
                [](void *, wl_registry *, std::uint32_t) {}};
            wl_registry_add_listener(registry, &listener, &compositor);
            assert(wl_display_roundtrip(display) >= 0 && compositor);
            auto *surface = wl_compositor_create_surface(compositor);
            assert(surface);
            {
                RenderTarget target;
                target.create(surface, context);
                target.setLogicalSize(360, 480);
                target.resize(360, 480);
                struct Readback final : CustomDrawNode {
                    mutable std::vector<std::uint8_t> pixels;
                    void draw(RenderBackend &, const CustomDrawContext &) const override {
                        pixels.resize(360 * 480 * 4);
                        glReadPixels(0, 0, 360, 480, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                        assert(glGetError() == GL_NO_ERROR);
                    }
                } readback;
                readback.setSize(360, 480);
                readback.setHitTestVisible(false);
                // Real projected Input → TextNode → GLES readback: explicit IRC colors
                // survive the tint/cache path, and shadows contain glyph coverage, not BGs.
                {
                    Node scene;
                    scene.setSize(360, 480);
                    auto background = std::make_unique<RectNode>();
                    RoundedRectStyle black;
                    black.fillMode = FillMode::Solid;
                    black.fill = rgbHex(0x000000);
                    background->setStyle(black);
                    background->setSize(360, 480);
                    scene.addChild(std::move(background));
                    auto field = std::make_unique<Input>();
                    auto *rich = field.get();
                    rich->setMultiline(true);
                    rich->setFrameVisible(false);
                    rich->setFontFamily("sans-serif");
                    rich->setFontSize(48);
                    rich->setTextColors(rgbHex(0x0000ff), {}, {}, {});
                    rich->setDisplayProjector([](std::string_view raw) {
                        const auto doc =
                            parseIrcFormatting(raw, rgbHex(0x0000ff), rgbHex(0x000000));
                        return Input::DisplayProjection{doc.text, ircMarkup(doc),
                                                        ircMarkup(doc, false), doc.rawToDisplay};
                    });
                    rich->setValue("\x04"
                                   "ff0000,00ff00X \x0f"
                                   "Y");
                    rich->setPosition(20, 20);
                    scene.addChild(std::move(field));
                    rich->measure(renderer, LayoutConstraints::exact(320, 280));
                    rich->setSize(320, 280);
                    rich->layout(renderer);
                    Label *label = nullptr;
                    for (const auto &viewport : rich->children())
                        if (viewport->clipChildren())
                            for (const auto &child : viewport->children())
                                if (auto *candidate = dynamic_cast<Label *>(child.get()))
                                    label = candidate;
                    assert(label && label->text() == rich->displayProjection()->markup);
                    label->setShadow(rgbHex(0xffff00), 0, 100);
                    const auto captureRich = [&] {
                        readback.pixels.clear();
                        context.renderScene(target, &scene, &readback);
                        assert(readback.pixels.size() == 360 * 480 * 4);
                        return readback.pixels;
                    };
                    captureRich();
                    const auto blue = captureRich();
                    std::array<size_t, 4>
                        inks{}; // explicit red, explicit green BG, base blue, shadow
                    for (int y = 0; y < 280; ++y)
                        for (int x = 0; x < 360; ++x) {
                            const auto offset = (size_t(479 - y) * 360 + x) * 4;
                            const auto r = blue[offset], g = blue[offset + 1], b = blue[offset + 2];
                            if (y < 100) {
                                inks[0] += r > 200 && g < 30 && b < 30;
                                inks[1] += g > 200 && r < 30 && b < 30;
                                inks[2] += b > 200 && r < 30 && g < 30;
                            } else {
                                inks[3] += r > 200 && g > 200 && b < 30;
                                assert(!(g > 200 && r < 30 && b < 30) &&
                                       !(r > 200 && g < 30 && b < 30) &&
                                       !(b > 200 && r < 30 && g < 30));
                            }
                        }
                    for (const auto ink : inks)
                        assert(ink > 8);
                    rich->setTextColors(rgbHex(0xff00ff), {}, {}, {});
                    rich->layout(renderer);
                    const auto magenta = captureRich();
                    size_t changedBaseInk = 0;
                    for (int y = 0; y < 100; ++y)
                        for (int x = 0; x < 360; ++x) {
                            const auto offset = (size_t(479 - y) * 360 + x) * 4;
                            const auto r = blue[offset], g = blue[offset + 1], b = blue[offset + 2];
                            if ((r > 200 && g < 30 && b < 30) || (g > 200 && r < 30 && b < 30))
                                assert(std::equal(blue.begin() + offset, blue.begin() + offset + 4,
                                                  magenta.begin() + offset));
                            if (b > 200 && r < 30 && g < 30) {
                                assert(magenta[offset] > 200 && magenta[offset + 1] < 30 &&
                                       magenta[offset + 2] > 200);
                                ++changedBaseInk;
                            }
                        }
                    assert(changedBaseInk > 8);
                    auto half = rgbHex(0xff00ff);
                    half.a = .5f;
                    rich->setTextColors(half, {}, {}, {});
                    rich->layout(renderer);
                    const auto faded = captureRich();
                    size_t fadedColorInk = 0;
                    for (int y = 0; y < 100; ++y)
                        for (int x = 0; x < 360; ++x) {
                            const auto offset = (size_t(479 - y) * 360 + x) * 4;
                            if (blue[offset + 1] > 240 && blue[offset] < 8 &&
                                blue[offset + 2] < 8) {
                                assert(faded[offset + 1] >= 115 && faded[offset + 1] <= 140 &&
                                       faded[offset] < 8 && faded[offset + 2] < 8);
                                ++fadedColorInk;
                            }
                        }
                    assert(fadedColorInk > 8);
                    InputDispatcher richFocus;
                    richFocus.setSceneRoot(&scene);
                    richFocus.setFocus(rich->inputArea());
                    rich->setTextColors(rgbHex(0x0000ff), rgbHex(0x00ffff), rgbHex(0xffffff), {});
                    auto selection = rich->editingState();
                    selection.current.selectionAnchor = rich->value().find('X');
                    selection.current.cursorPos = selection.current.selectionAnchor + 2;
                    rich->restoreEditingState(selection);
                    rich->layout(renderer);
                    const auto selected = captureRich();
                    size_t selectedBackground = 0;
                    for (int y = 0; y < 100; ++y)
                        for (int x = 0; x < 360; ++x) {
                            const auto offset = (size_t(479 - y) * 360 + x) * 4;
                            if (blue[offset + 1] > 240 && blue[offset] < 8 &&
                                blue[offset + 2] < 8) {
                                selectedBackground += selected[offset] < 30 &&
                                                      selected[offset + 1] > 200 &&
                                                      selected[offset + 2] > 200;
                            }
                        }
                    // Ignore raster edge rounding, but require selection over most explicit BG ink.
                    assert(selectedBackground > 8 && selectedBackground * 4 >= fadedColorInk * 3);
                    richFocus.setFocus(nullptr);
                    richFocus.setSceneRoot(nullptr);
                }
                Model model;
                prepareBuffers(model);
                model.settings.scale = 2;
                Theme theme;
                theme.update(model);
                context.setTextFontFamily(themeUiFont().family);
                DeferredClipboard clipboard;
                InputDispatcher dispatcher;
                Ui ui(model, clipboard);
                dispatcher.setSceneRoot(&ui.root());
                ui.setInputDispatcher(dispatcher);
                ui.setWindowActive(true);
                ui.update(360, 480, 1);
                ui.root().layout(renderer);
                dispatcher.setFocus(nullptr);
                const std::array views{nativeComposer(*inputWithDraft(ui.root(), "Channel draft"))};
                const auto inside = [](const LayoutRect &r, float x, float y) {
                    return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
                };
                // A captured release is not a click in the clipped-away part of a real Button.
                const auto &view = views.front();
                const auto originalWidth = target.logicalWidth();
                const auto originalHeight = target.logicalHeight();
                const auto buttonViewportWidth =
                    originalWidth + static_cast<std::uint32_t>(view.actions->width());
                const auto buttonDocument = view.input->editingState();
                const auto buttonUndo = buttonDocument.history->undo;
                const auto buttonRedo = buttonDocument.history->redo;
                auto *const buttonFocus = dispatcher.focusedArea();
                target.setLogicalSize(buttonViewportWidth, originalHeight);
                target.resize(buttonViewportWidth, originalHeight);
                ui.update(static_cast<float>(buttonViewportWidth),
                          static_cast<float>(originalHeight), 1);
                ui.root().layout(renderer);
                // This fixture deliberately tests an intermediate clip, independently
                // of the production composer's transparent-overlay overflow policy.
                const bool originalComposerClip = view.composer->clipChildren();
                view.composer->setClipChildren(true);
                auto clip = sceneBounds(*view.composer);
                Button *clippedButton = nullptr;
                for (auto *candidate : {view.actions, view.send}) {
                    const auto bounds = sceneBounds(*candidate);
                    if (bounds.x < clip.x + clip.width &&
                        bounds.x + bounds.width > clip.x + clip.width)
                        clippedButton = candidate;
                }
                assert(clippedButton);
                auto *actionsArea = clippedButton->inputArea();
                const auto button = sceneBounds(*actionsArea);
                const float visibleLeft = std::max(button.x, clip.x);
                const float visibleRight = std::min(button.x + button.width, clip.x + clip.width);
                const float visibleTop = std::max(button.y, clip.y);
                const float visibleBottom =
                    std::min(button.y + button.height, clip.y + clip.height);
                assert(visibleRight > visibleLeft && visibleBottom > visibleTop &&
                       button.x + button.width > clip.x + clip.width);
                const float pressX = (visibleLeft + visibleRight) / 2;
                const float pressY = (visibleTop + visibleBottom) / 2;
                const auto rootBounds = sceneBounds(ui.root());
                const auto cardBounds = sceneBounds(*view.card),
                           paneBounds = sceneBounds(*view.pane);
                const float releaseLeft = std::max(button.x, clip.x + clip.width);
                const float releaseRight =
                    std::min({button.x + button.width, rootBounds.x + rootBounds.width,
                              cardBounds.x + cardBounds.width, paneBounds.x + paneBounds.width});
                assert(releaseRight > releaseLeft);
                const float releaseX = (releaseLeft + releaseRight) / 2;
                assert(inside(button, releaseX, pressY) && inside(rootBounds, releaseX, pressY) &&
                       inside(cardBounds, releaseX, pressY) &&
                       inside(paneBounds, releaseX, pressY) && !inside(clip, releaseX, pressY));
                assert(dispatcher.inputAreaAt(pressX, pressY) == actionsArea &&
                       dispatcher.inputAreaAt(releaseX, pressY) != actionsArea);
                int clicks = 0;
                clippedButton->setOnClick([&] { ++clicks; });
                std::uint32_t serial = 100;
                const auto pointerButton = [&](float x, float y, bool pressed) {
                    ++serial;
                    assert(dispatcher.pointerButton(x, y, BTN_LEFT, pressed, serial, serial * 10,
                                                    false));
                };
                pointerButton(pressX, pressY, true);
                assert(dispatcher.pointerCaptured() && actionsArea->pressed());
                dispatcher.pointerMotion(releaseX, pressY, ++serial);
                assert(dispatcher.pointerCaptured() && actionsArea->pressed());
                pointerButton(releaseX, pressY, false);
                std::fprintf(
                    stderr,
                    "native composer captured-click: stage=outside-release clicks=%d "
                    "button=(%g,%g,%g,%g) clip=(%g,%g,%g,%g) press=(%g,%g) release=(%g,%g)\n",
                    clicks, button.x, button.y, button.width, button.height, clip.x, clip.y,
                    clip.width, clip.height, pressX, pressY, releaseX, pressY);
                assert(clicks == 0 && !dispatcher.pointerCaptured() && !actionsArea->pressed());
                pointerButton(pressX, pressY, true);
                pointerButton(pressX, pressY, false);
                std::fprintf(stderr,
                             "native composer captured-click: stage=inside-release clicks=%d\n",
                             clicks);
                assert(clicks == 1 && !dispatcher.pointerCaptured() && !actionsArea->pressed());
                clippedButton->setOnClick({});
                // Restore the original partial Input clip before drag and raster coverage.
                target.setLogicalSize(originalWidth, originalHeight);
                target.resize(originalWidth, originalHeight);
                ui.update(static_cast<float>(originalWidth), static_cast<float>(originalHeight), 1);
                ui.root().layout(renderer);
                view.composer->setClipChildren(true);
                clip = sceneBounds(*view.composer);
                assert(view.input->editingStateMatches(buttonDocument) &&
                       buttonDocument.history->undo == buttonUndo &&
                       buttonDocument.history->redo == buttonRedo &&
                       dispatcher.focusedArea() == buttonFocus);

                // Clipping click activation must not cancel a native Input's captured selection
                // drag.
                auto *inputArea = view.input->inputArea();
                const auto inputBounds = sceneBounds(*inputArea);
                assert(inputBounds.x < clip.x + clip.width &&
                       inputBounds.x + inputBounds.width > clip.x + clip.width);
                const auto document = view.input->editingState();
                auto start = document;
                start.current.cursorPos = start.current.selectionAnchor = 0;
                start.scrollX = start.scrollY = 0;
                view.input->restoreEditingState(start);
                const float inputX = inputBounds.x + .5f, inputY = inputBounds.y + .5f;
                const float dragX = clip.x + clip.width + inputBounds.width;
                const float dragY = clip.y + clip.height + inputBounds.height;
                assert(dispatcher.inputAreaAt(inputX, inputY) == inputArea &&
                       !inside(clip, dragX, dragY));
                pointerButton(inputX, inputY, true);
                const auto pressed = view.input->editingState();
                assert(pressed.current.cursorPos == 0 && pressed.current.selectionAnchor == 0);
                dispatcher.pointerMotion(dragX, dragY, ++serial);
                const auto dragged = view.input->editingState();
                std::fprintf(
                    stderr,
                    "native composer selection-drag: stage=outside-motion cursor=%zu anchor=%zu\n",
                    dragged.current.cursorPos, dragged.current.selectionAnchor);
                assert(dispatcher.pointerCaptured() && inputArea->pressed() &&
                       dragged.current.cursorPos > 0 && dragged.current.selectionAnchor == 0 &&
                       dragged.current.value == document.current.value);
                pointerButton(dragX, dragY, false);
                assert(!dispatcher.pointerCaptured() && !inputArea->pressed() &&
                       view.input->editingState().current == dragged.current);
                dispatcher.pointerMotion(inputX, inputY, ++serial);
                assert(view.input->editingState().current == dragged.current);
                view.input->restoreEditingState(document);
                dispatcher.setFocus(nullptr);
                const auto capture = [&] {
                    readback.pixels.clear();
                    context.renderScene(target, &ui.root(), &readback);
                    assert(readback.pixels.size() == 360 * 480 * 4);
                    return readback.pixels;
                };
                capture(); // Warm GPU resources without changing native layout.
                for (const auto &view : views) {
                    assert(nativeVisible(*view.pane) && view.pane->width() > 0);
                    within(*view.composer, *view.card);
                    within(*view.card, *view.pane);
                    within(*view.pane, ui.root());
                    const auto clip = sceneBounds(*view.composer);
                    assert(view.composer->clipChildren());
                    bool overflow = false;
                    for (auto *area : view.areas()) {
                        const auto bounds = sceneBounds(*area);
                        overflow |=
                            bounds.x < clip.x || bounds.x + bounds.width > clip.x + clip.width;
                    }
                    assert(
                        overflow); // Exercise a genuinely impossible row, not a fitting substitute.
                    const auto visible = capture();
                    view.composer->setVisible(false);
                    const auto hidden =
                        capture(); // No update/layout/focus mutation between frames.
                    view.composer->setVisible(true);
                    sameRect("impossible-capture", sceneBounds(*view.composer), clip, model);
                    std::size_t changed = 0;
                    for (int y = 0; y < 480; ++y)
                        for (int x = 0; x < 360; ++x) {
                            const auto offset = (std::size_t(479 - y) * 360 + x) * 4;
                            const bool differs =
                                !std::equal(visible.begin() + offset, visible.begin() + offset + 4,
                                            hidden.begin() + offset);
                            if (differs) {
                                assert(x >= std::floor(clip.x) && y >= std::floor(clip.y) &&
                                       x < std::ceil(clip.x + clip.width) &&
                                       y < std::ceil(clip.y + clip.height));
                                ++changed;
                            }
                        }
                    assert(changed > 0);
                    const auto card = sceneBounds(*view.card), pane = sceneBounds(*view.pane);
                    for (auto *area : view.areas()) {
                        const auto bounds = sceneBounds(*area);
                        for (float y : {bounds.y + .5f, bounds.y + bounds.height / 2,
                                        bounds.y + bounds.height - .5f})
                            for (float x = -.5f; x <= 360.5f; x += 1) {
                                auto *hit = dispatcher.inputAreaAt(x, y);
                                if (hit == area)
                                    assert(inside(clip, x, y) && inside(card, x, y) &&
                                           inside(pane, x, y));
                            }
                    }
                }
                view.composer->setClipChildren(originalComposerClip);
                dispatcher.setSceneRoot(nullptr);
            }
            wl_surface_destroy(surface);
            wl_compositor_destroy(compositor);
            wl_registry_destroy(registry);
        }
    }
    wl_display_disconnect(display);
}
