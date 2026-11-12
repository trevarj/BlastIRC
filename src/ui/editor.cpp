#include "editor.hpp"
#include "../commands.hpp"
#include "../irc/protocol.hpp"
#include "../platform/clipboard.hpp"
#include "../render/graphics.hpp"
#include "../spellcheck.hpp"
#include "../theme/theme.hpp"
#include "../word_completion.hpp"
#include "command_completion.hpp"
#include "emoji.hpp"
#include "irc_format.hpp"
#include "logic.hpp"
#include "message_selection.hpp"
#include "presentation.hpp"
#include "transcript.hpp"
#include "ui.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <core/input/key_modifiers.h>
#include <core/input/key_symbols.h>
#include <core/text_clipboard.h>
#include <glib.h>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <render/scene/input_area.h>
#include <render/scene/input_dispatcher.h>
#include <set>
#include <ui/controls/button.h>
#include <ui/controls/flex.h>
#include <ui/controls/input.h>
#include <ui/controls/label.h>
#include <ui/controls/scroll_view.h>
#include <ui/controls/select_popup_context.h>
#include <utility>

namespace blast {
namespace {
std::string lower(std::string value) {
    for (char &c : value)
        c = std::tolower(static_cast<unsigned char>(c));
    return value;
}
void shown(Node &node, bool visible) {
    node.setVisible(visible);
    node.setParticipatesInLayout(visible);
}
bool composerWritable(const Model &model, const std::string &id) {
    const auto buffer = model.buffers.find(id);
    return buffer != model.buffers.end() && buffer->second.kind != "mentions";
}
float inputMinimumWidth(float scale) {
    // Preserve Noctalia Input's native 48-unit baseline and the scaled hit target.
    return std::max(48.f, 32 * scale);
}
float primaryMinimumWidth(float scale) {
    return inputMinimumWidth(scale) + 2 * std::max(32.f, 32 * scale) + 16 * scale;
}

std::unique_ptr<Flex> row() {
    auto result = std::make_unique<Flex>();
    result->setDirection(FlexDirection::Horizontal);
    result->setFillWidth(true);
    result->setGap(4 * themeScale());
    return result;
}
const Input::EditSnapshot &committed(const Input::EditingState &state) {
    return state.preeditBase ? *state.preeditBase : state.current;
}
size_t separateMessages(std::string_view text) {
    size_t count = 0;
    while (!text.empty()) {
        const auto end = text.find('\n');
        count += !text.substr(0, end).empty();
        if (end == std::string_view::npos)
            break;
        text.remove_prefix(end + 1);
    }
    return count;
}
bool visibleInScene(const Node &node) {
    for (auto *ancestor = &node; ancestor; ancestor = ancestor->parent())
        if (!ancestor->visible())
            return false;
    return true;
}
void keepPointerFocus(Node &node) {
    if (auto *area = dynamic_cast<InputArea *>(&node))
        area->setFocusOnPointerPress(false);
    for (const auto &child : node.children())
        keepPointerFocus(*child);
}
bool pointerPressedIn(const Node &node) {
    if (const auto *area = dynamic_cast<const InputArea *>(&node);
        area && (area->pressed() || area->passiveTouchPress()))
        return true;
    return std::any_of(node.children().begin(), node.children().end(),
                       [](const auto &child) { return pointerPressedIn(*child); });
}
struct FormatToggle {
    std::string_view title, command, markup;
    char byte;
    bool IrcStyle::*active;
};
constexpr std::array formatToggles{
    FormatToggle{"Bold", "bold", "<b>Bold</b>", '\x02', &IrcStyle::bold},
    FormatToggle{"Italic", "italic", "<i>Italic</i>", '\x1d', &IrcStyle::italic},
    FormatToggle{"Underline", "underline", "<u>Underline</u>", '\x1f', &IrcStyle::underline},
    FormatToggle{"Strikethrough", "strike", "<s>Strikethrough</s>", '\x1e', &IrcStyle::strike},
    FormatToggle{"Monospace", "monospace", "<span font_family=\"monospace\">Monospace</span>",
                 '\x11', &IrcStyle::monospace},
    FormatToggle{"Reverse", "reverse", "Reverse", '\x16', &IrcStyle::reverse},
};
Color composerForeground(const Model &) { return themeForegroundColor("on_surface"); }
void preserveIrcSuffix(const Input::EditSnapshot &before, Input::EditSnapshot &after, Color normal,
                       Color background, std::optional<SpellRange> replaced = {}) {
    if (before.value == after.value)
        return;
    constexpr std::string_view controls = "\x02\x03\x04\x0f\x11\x16\x1d\x1e\x1f";
    if (before.value.find_first_of(controls) == std::string::npos &&
        after.value.find_first_of(controls) == std::string::npos)
        return;
    size_t prefix = 0;
    while (prefix < before.value.size() && prefix < after.value.size() &&
           before.value[prefix] == after.value[prefix])
        ++prefix;
    prefix = std::min(prefix, replaced ? replaced->begin
                                       : std::min(before.cursorPos, before.selectionAnchor));
    while (prefix && ((prefix < before.value.size() &&
                       (static_cast<unsigned char>(before.value[prefix]) & 0xc0) == 0x80) ||
                      (prefix < after.value.size() &&
                       (static_cast<unsigned char>(after.value[prefix]) & 0xc0) == 0x80)))
        --prefix;
    size_t suffix = 0;
    const size_t retainedSuffix =
        before.value.size() -
        (replaced ? replaced->end : std::max(before.cursorPos, before.selectionAnchor));
    while (suffix < retainedSuffix && suffix < before.value.size() - prefix &&
           suffix < after.value.size() - prefix &&
           before.value[before.value.size() - 1 - suffix] ==
               after.value[after.value.size() - 1 - suffix])
        ++suffix;
    while (
        suffix &&
        ((static_cast<unsigned char>(before.value[before.value.size() - suffix]) & 0xc0) == 0x80 ||
         (static_cast<unsigned char>(after.value[after.value.size() - suffix]) & 0xc0) == 0x80))
        --suffix;
    const size_t oldEnd = before.value.size() - suffix;
    size_t newEnd = after.value.size() - suffix;
    const size_t colorStart =
        prefix ? before.value.find_last_of("\x03\x04", prefix - 1) : std::string::npos;
    const size_t terminator = colorStart == std::string::npos
                                  ? std::string::npos
                                  : before.value.find_first_of(controls, colorStart + 1);
    const bool colorBoundary = colorStart != std::string::npos && prefix - colorStart <= 14 &&
                               (terminator == std::string::npos || terminator >= prefix);
    const bool changedControls =
        std::string_view(before.value).substr(prefix, oldEnd - prefix).find_first_of(controls) !=
            std::string::npos ||
        std::string_view(after.value).substr(prefix, newEnd - prefix).find_first_of(controls) !=
            std::string::npos;
    if ((!changedControls && !colorBoundary) || (!suffix && !colorBoundary))
        return;
    const auto oldDocument = parseIrcFormatting(before.value, normal, background);
    auto newDocument = parseIrcFormatting(after.value, normal, background);
    if (colorBoundary) {
        const bool ordinaryInsertion =
            newEnd > prefix && controls.find(after.value[prefix]) == std::string_view::npos;
        bool swallowed = false;
        const size_t limit = std::min(after.value.size(), colorStart + 14);
        for (size_t position = colorStart + 1; position < limit;) {
            if (controls.find(after.value[position]) != std::string_view::npos)
                break;
            const size_t next = ui_logic::next(after.value, position);
            if (newDocument.rawToDisplay[next] != newDocument.rawToDisplay[position])
                break;
            if (position < prefix || position >= newEnd) {
                const size_t old = position < prefix ? position : oldEnd + position - newEnd;
                const size_t oldNext = ui_logic::next(before.value, old);
                swallowed = oldDocument.rawToDisplay[oldNext] != oldDocument.rawToDisplay[old];
            } else
                swallowed = ordinaryInsertion;
            if (swallowed)
                break;
            position = next;
        }
        if (swallowed) {
            // A hidden color token must not absorb ordinary typed text or retained suffix text.
            after.value.insert(prefix, "\x02\x02");
            if (after.cursorPos >= prefix)
                after.cursorPos += 2;
            if (after.selectionAnchor >= prefix)
                after.selectionAnchor += 2;
            newEnd += 2;
            newDocument = parseIrcFormatting(after.value, normal, background);
        }
    }
    if (!suffix)
        return;
    bool swallowedSuffix = false;
    const size_t suffixProbeEnd = std::min(before.value.size(), oldEnd + 14);
    for (size_t old = oldEnd; old < suffixProbeEnd;) {
        if (controls.find(before.value[old]) != std::string_view::npos)
            break;
        const size_t nextOld = ui_logic::next(before.value, old);
        const size_t current = newEnd + old - oldEnd;
        const size_t nextCurrent = ui_logic::next(after.value, current);
        if (newDocument.rawToDisplay[nextCurrent] != newDocument.rawToDisplay[current])
            break;
        if (oldDocument.rawToDisplay[nextOld] != oldDocument.rawToDisplay[old]) {
            swallowedSuffix = true;
            break;
        }
        old = nextOld;
    }
    if (swallowedSuffix) {
        // A pasted partial color parameter may not consume an untouched numeric/hex suffix.
        after.value.insert(newEnd, "\x02\x02");
        if (after.cursorPos >= newEnd)
            after.cursorPos += 2;
        if (after.selectionAnchor >= newEnd)
            after.selectionAnchor += 2;
        newEnd += 2;
        newDocument = parseIrcFormatting(after.value, normal, background);
    }
    const auto restore =
        ircFormattingTransition(newDocument.styleAt(newEnd), oldDocument.styleAt(oldEnd));
    after.value.insert(newEnd, restore);
    if (after.cursorPos > newEnd)
        after.cursorPos += restore.size();
    if (after.selectionAnchor > newEnd)
        after.selectionAnchor += restore.size();
}
} // namespace

struct EditorStore::UploadOrigin::State {
    std::weak_ptr<int> identity;
    std::string buffer, content;
    std::uint64_t revision;
    size_t cursor, anchor;
};

struct EditorStore::Impl {
    struct Draft {
        std::shared_ptr<int> identity = std::make_shared<int>(0);
        Input::EditingState state;
        std::string published, stash;
        std::vector<std::string> history;
        int historyPos = -1;
        std::uint64_t revision = 1;
        double edited = 0, lastTyping = -10;
        bool mark = false, typingActive = false;
    };
    struct PasteIntent {
        std::string buffer;
        std::uint64_t revision;
    };
    struct Composer : Flex {
        struct ReplyCard final : Flex {
          protected:
            void doLayout(Renderer &renderer) override {
                // The transparent native host may lay this out again after arrange.
                // Keep the real pane viewport, just as the composer itself does.
                if (sizeAssignedByLayout())
                    arrange(renderer, {x(), y(), width(), height()});
                else
                    Flex::doLayout(renderer);
            }
        };
        Impl &owner;
        PaneState &pane;
        std::string buffer;
        Input *input = nullptr;
        Flex *replyRow = nullptr, *replyRail = nullptr, *replyText = nullptr;
        Flex *tools = nullptr, *accessories = nullptr, *pasteActions = nullptr;
        ScrollView *formatting = nullptr, *pastePreviewView = nullptr;
        Label *replyHeading = nullptr, *replyLabel = nullptr, *pasteLabel = nullptr,
              *pasteContext = nullptr;
        Button *sendButton = nullptr, *formatButton = nullptr, *separateButton = nullptr;
        Button *uploadButton = nullptr;
        Button *formatFocused = nullptr;
        Button *replyCancelButton = nullptr;
        std::array<Button *, 3> pasteButtons{};
        bool actionMetricsDirty = true, compactActions = false;
        float actionRenderScale = 0;
        size_t actionMessageCount = 0;
        LayoutRect formatFocusBounds;
        LayoutSize formatFocusViewport;
        bool formatFocusLayoutValid = false;
        std::vector<std::pair<Button *, std::string>> formatButtons;
        std::vector<Flex *> formatRows;
        std::vector<Label *> formatLabels;
        IrcDocument formatDocument;
        bool projectionHasText = false;
        Color projectionForeground, projectionBackground;
        std::string nickSource, nickServer, nickMapping, nickOwn, nickQuery, nickPalette;
        std::vector<std::string> nickNames;
        std::map<std::string, std::string> nickOverrides;
        std::uint64_t nickTheme = 0;
        bool formattingOpen = false;
        std::string emojiQuery, emojiToken, emojiSuppressed;
        size_t emojiStart = 0, emojiEnd = 0, emojiSelected = 0;
        size_t emojiSuppressedStart = std::string::npos;
        std::uint64_t emojiRevision = 0;
        bool emojiWasEligible = false;
        std::string commandToken, commandSuppressed;
        size_t commandEnd = 0, commandSelected = 0;
        std::uint64_t commandRevision = 0;
        size_t commandStart = 0;
        bool literalCompletion = false;
        std::vector<std::string> completionCandidates;
        std::string completionKind, completionChannelTypes;
        std::string compositionChord;
        std::string predictedWord, predictionPrefix, predictionQuery;
        size_t predictionCaret = 0;
        bool predictionEnabled = false, predictionReady = false;
        bool predictionCompeting = false;
        size_t predictionMessages = 0;
        std::uint64_t predictionMessagesRevision = 0;
        std::string predictionBuffer, predictionServer, predictionTarget, predictionMapping,
            predictionOwn;
        std::vector<std::string> predictionHistory;
        size_t completionCaret = 0, completionAnchor = 0;
        std::set<std::string> predictionFools;
        FoolDisplay predictionFoolDisplay = FoolDisplay::Shown;
        void updatePrediction();
        bool hasLiteralCandidate(std::string_view) const;
        bool acceptPrediction();
        std::uint64_t compositionChordRevision = 0;
        std::vector<Button *> buttons;
        std::vector<std::pair<Button *, LucideIcon *>> icons;
        std::map<std::string, std::pair<float, float>> scrollPositions;
        std::string paste, pasteError, renderedPasteError, pasteDestination, renderedDestination;
        std::string renderedReply, replyScratch, pastePreview, destinationScratch, inputDestination;
        std::string replyAuthorMarkup, replyHeadingMarkup, replyTooltip;
        std::uint64_t replyTheme = 0;
        std::string fullPasteCaption, compactPasteCaption;
        std::shared_ptr<PasteIntent> pendingPaste;
        size_t pasteLines = 0, pasteMessages = 0;
        std::uint64_t revision = 0, pasteVersion = 0, renderedPasteVersion = 0;
        float scale = 0, heightLimit = 0;
        float messageSize = 0;
        TextFont font, labelFont;
        LayoutRect arrangedInput;
        std::shared_ptr<int> menuAlive = std::make_shared<int>(0);
        std::uint64_t menuGeneration = 0;
        bool menuOpen = false;
        std::string spellText, spellLanguage, spellRaw, spellError, spellBuffer, spellMapping;
        std::string spellChannelTypes;
        std::weak_ptr<int> spellIdentity;
        std::optional<SpellRange> spellingEdit;
        std::vector<std::string> spellNames, spellFoldedNames;
        std::vector<SpellRange> spellExcluded, spellScratch, spellMisspelled;
        std::uint64_t spellTicket = 0;
        bool spellNeedsCheck = false;
        std::chrono::steady_clock::time_point spellDue;
        struct SpellingCycle {
            std::string buffer, language, raw;
            std::weak_ptr<int> identity;
            std::uint64_t revision = 0, ticket = 0;
            size_t caret = 0, presses = 0, index = 0;
            SpellRange range{};
            std::vector<std::string> suggestions{};
        };
        std::optional<SpellingCycle> spellingCycle;
        bool cyclingSpelling = false;
        bool spellingCycleValid() const;
        std::optional<SpellRange> rawSpellingRange(SpellRange) const;
        void cycleSpelling();
        void applySpellingCycle();
        void updateSpelling();
        void excludeSpelling();
        bool updateNickDecoration();
        Input::DisplayProjection project(std::string_view);
        void contextMenu(const InputArea::PointerData *);
        Composer(Impl &, PaneState &);
        ~Composer() override;
        Button *button(Flex &, std::string_view, std::function<void()>);
        Button *iconButton(Flex &, std::string_view, std::string_view, std::function<void()>);
        void measureActions(Renderer &);
        float measurePreview(Renderer &, const LayoutConstraints &);
        float measureMinimum(Renderer &, float innerWidth, float maxComposerHeight);
        void setPaste(std::string, std::string error = {});
        bool pasteTargetValid();
        void insertPaste();
        void sendPaste();
        void cancelPaste(bool restoreFocus = true);
        void toggleFormatting();
        std::string focusKey(std::string_view role) const {
            return "composer:" + std::to_string(pane.id) + ":" + std::string(role);
        }
        void updateCompletion(bool force = false);
        void hideCompletion();
        void placeCompletion();
        bool updateCommandCompletion(bool eligible);
        void openLiteralCompletion();
        bool acceptCommandCompletion(size_t index);
        void updateUpload();
        bool acceptCompletion(size_t index);
        std::string help() const;
        void fit(Renderer &, const LayoutConstraints &);
        LayoutSize doMeasure(Renderer &, const LayoutConstraints &) override;
        void doArrange(Renderer &, const LayoutRect &) override;
        void doLayout(Renderer &) override;
        void sync();
    };
    Model &model;
    TextClipboard &clipboard;
    Clipboard *payloadClipboard;
    UiCommand command;
    FocusPane focus;
    std::function<void(Node &)> retirePointer;
    FocusPane onEditingStateChanged;
    UploadRequested onUploadRequested;
    ImageUploadRequested onImageUploadRequested;
    std::function<bool(std::uint64_t, const std::string &)> replyReadable;
    InputDispatcher *dispatcher = nullptr;
    std::map<std::string, Draft> drafts;
    std::vector<Composer *> views;
    Spellcheck spelling;
    std::uint64_t spellCounter = 0, spellInFlight = 0;
    void syncSpelling();
    std::unique_ptr<WordCompletion> words;
    Node *overlayHost = nullptr;
    emoji::CompletionView *emojiPopup = nullptr;
    Composer *emojiOwner = nullptr;
    std::unique_ptr<emoji::Catalog> emojiCatalog;
    std::span<const emoji::Match> emojiMatches;
    command_completion::View *commandPopup = nullptr;
    Composer *commandOwner = nullptr;
    std::array<command_completion::Match, command_completion::maxResults> commandMatches{};
    size_t commandCount = 0;
    std::shared_ptr<int> alive = std::make_shared<int>(0);
    std::string kill;
    bool restoring = false;

    Impl(Model &m, TextClipboard &c, UiCommand u, FocusPane f, std::function<void(Node &)> retire)
        : model(m), clipboard(c), payloadClipboard(dynamic_cast<Clipboard *>(&c)),
          command(std::move(u)), focus(std::move(f)), retirePointer(std::move(retire)) {}
    emoji::Catalog &catalog() {
        if (!emojiCatalog)
            emojiCatalog = std::make_unique<emoji::Catalog>();
        return *emojiCatalog;
    }
    Draft &draft(const std::string &id);
    ui_logic::Editor editor(const Draft &) const;
    void publish(const std::string &, Draft &);
    void apply(const std::string &, Draft &, ui_logic::Editor,
               std::optional<std::uint64_t> sourcePane, bool remember = true);
    void capture(Composer &, bool publishChange = true);
    void refresh(const std::string &);
    void recover(const std::string &, Draft &);
    void remapBuffer(std::string_view oldId, std::string_view newId, std::uint64_t activePane);
    bool valid(std::string_view, std::string_view);
    bool insert(PaneState &, std::string_view);
    bool execute(std::string_view, PaneState &);
    void send(PaneState &, Draft &);
    void paste(Composer &);
    bool permitted(const Buffer &, std::string_view) const;
};

EditorStore::Impl::Draft &EditorStore::Impl::draft(const std::string &id) {
    auto [entry, fresh] = drafts.try_emplace(id);
    if (!fresh && !entry->second.identity && model.buffers.contains(id)) {
        const auto revision = entry->second.revision;
        entry->second = Draft{};
        entry->second.revision = revision + 1;
        fresh = true;
    }
    if (fresh) {
        auto b = model.buffers.find(id);
        if (b != model.buffers.end()) {
            entry->second.state.current.value = b->second.draft;
            entry->second.state.current.cursorPos = entry->second.state.current.selectionAnchor =
                b->second.draft.size();
            entry->second.published = b->second.draft;
            if (b->second.draft.size() >= ui_logic::composerLimit)
                model.status = "Recovered draft exceeds 64 KiB. Delete, split, or send text before "
                               "adding more.";
        }
    }
    return entry->second;
}
ui_logic::Editor EditorStore::Impl::editor(const Draft &d) const {
    const auto &s = committed(d.state);
    ui_logic::Editor result;
    result.text = s.value;
    result.cursor = s.cursorPos;
    result.anchor = s.selectionAnchor;
    result.mark = d.mark;
    return result;
}
void EditorStore::Impl::publish(const std::string &id, Draft &d) {
    const auto &text = committed(d.state).value;
    if (auto b = model.buffers.find(id); b != model.buffers.end()) {
        if (b->second.draft != text)
            b->second.draft = text;
        if (d.published != text)
            d.published = text;
    }
}
void EditorStore::Impl::apply(const std::string &id, Draft &d, ui_logic::Editor e,
                              std::optional<std::uint64_t> sourcePane, bool remember) {
    const auto &before = committed(d.state);
    const bool changed = before.value != e.text;
    const bool edited =
        sourcePane && onEditingStateChanged &&
        (changed || before.cursorPos != e.cursor ||
         before.selectionAnchor != (e.mark ? e.anchor : e.cursor) || d.state.preeditBase ||
         d.state.preeditStart || d.state.preeditLength || d.state.preeditCaretHidden);
    if (changed && remember) {
        auto &undo = d.state.history->undo;
        if (undo.empty() || undo.back() != before)
            undo.push_back(before);
        if (undo.size() > 100)
            undo.erase(undo.begin());
        d.state.history->redo.clear();
    }
    d.state.current = {std::move(e.text), e.cursor, e.mark ? e.anchor : e.cursor};
    d.state.preeditBase.reset();
    d.state.preeditStart = d.state.preeditLength = 0;
    d.state.preeditCaretHidden = false;
    d.mark = e.mark;
    if (changed)
        d.edited = model.now;
    ++d.revision;
    publish(id, d);
    refresh(id);
    if (edited)
        onEditingStateChanged(*sourcePane);
}
void EditorStore::Impl::capture(Composer &view, bool publishChange) {
    if (restoring || view.buffer.empty())
        return;
    auto &d = draft(view.buffer);
    if (view.revision != d.revision)
        return;
    // A partial IME commit can leave the displayed text and caret unchanged.
    // Compare the complete native document, including its committed preedit base.
    if (view.input->editingStateMatches(d.state))
        return;
    if (!view.cyclingSpelling)
        view.spellingCycle.reset();
    auto state = view.input->editingState();
    view.scrollPositions[view.buffer] = {state.scrollX, state.scrollY};
    const auto &previous = d.state;
    const bool edited = state.current.cursorPos != previous.current.cursorPos ||
                        state.current.selectionAnchor != previous.current.selectionAnchor ||
                        state.preeditStart != previous.preeditStart ||
                        state.preeditLength != previous.preeditLength ||
                        state.preeditCaretHidden != previous.preeditCaretHidden ||
                        state.preeditBase != previous.preeditBase ||
                        state.current.value != previous.current.value;
    if (!edited && state.history == previous.history) {
        // Viewport offsets belong to the pane, not to the shared document revision.
        d.state.scrollX = state.scrollX;
        d.state.scrollY = state.scrollY;
        return;
    }
    const auto &before = committed(d.state);
    const auto &after = committed(state);
    const bool changed = before.value != after.value;
    if (changed) {
        d.edited = model.now;
        d.mark = false;
        d.historyPos = -1;
    }
    if (after.cursorPos != before.cursorPos || after.selectionAnchor != before.selectionAnchor) {
        d.mark = after.cursorPos != after.selectionAnchor;
    }
    d.state = std::move(state);
    view.revision = ++d.revision;
    if (publishChange)
        publish(view.buffer, d);
    refresh(view.buffer);
    if (publishChange && edited && onEditingStateChanged)
        onEditingStateChanged(view.pane.id);
}
void EditorStore::Impl::refresh(const std::string &id) {
    auto &d = draft(id);
    const bool previous = restoring;
    restoring = true;
    for (auto *view : views)
        if (view->buffer == id && view->revision != d.revision) {
            if (view->revision)
                view->input->restoreEditingState(d.state, true);
            else {
                const auto sharedScroll = std::pair{d.state.scrollX, d.state.scrollY};
                if (auto saved = view->scrollPositions.find(id);
                    saved != view->scrollPositions.end()) {
                    d.state.scrollX = saved->second.first;
                    d.state.scrollY = saved->second.second;
                }
                view->input->restoreEditingState(d.state);
                d.state.scrollX = sharedScroll.first;
                d.state.scrollY = sharedScroll.second;
            }
            view->revision = d.revision;
        }
    restoring = previous;
}
void EditorStore::Impl::recover(const std::string &id, Draft &d) {
    auto b = model.buffers.find(id);
    if (b == model.buffers.end())
        return;
    if (b->second.draft != d.published && !b->second.draft.empty() &&
        b->second.draft != committed(d.state).value) {
        auto e = editor(d);
        if (ui_logic::reconcileDraft(e, d.published, b->second.draft)) {
            apply(id, d, std::move(e), std::nullopt);
            if (committed(d.state).value.size() >= ui_logic::composerLimit)
                model.status = "Recovered draft exceeds 64 KiB. Delete, split, or send text before "
                               "adding more.";
        }
    }
    publish(id, d);
}
void EditorStore::Impl::remapBuffer(std::string_view oldId, std::string_view newId,
                                    std::uint64_t activePane) {
    if (oldId == newId)
        return;
    const std::string oldKey(oldId), newKey(newId);
    const auto buffer = model.buffers.find(newKey);
    if (buffer == model.buffers.end())
        return;
    const auto merged = buffer->second.draft;
    // Core already merged the drafts: capturing must not publish either stale half.
    for (auto *view : views)
        if (view->buffer == oldId || view->buffer == newId)
            capture(*view, false);
    auto source = drafts.find(oldKey), target = drafts.find(newKey);
    if (source == drafts.end() && target == drafts.end())
        return;
    bool keepSource = source != drafts.end();
    if (source != drafts.end() && target != drafts.end()) {
        const Composer *focused = nullptr, *preferred = nullptr, *visible = nullptr;
        for (const auto *view : views) {
            if (view->buffer != oldId && view->buffer != newId)
                continue;
            const bool visibleNow = visibleInScene(*view);
            if (visibleNow && view->input->inputArea()->focused()) {
                focused = view;
                break;
            }
            if (view->pane.id == activePane)
                preferred = view;
            if (visibleNow && !visible)
                visible = view;
        }
        const auto *context = focused ? focused : preferred ? preferred : visible;
        if (context)
            keepSource = context->buffer == oldId;
        else if (target->second.state.preeditBase && !source->second.state.preeditBase)
            keepSource = false;
    }
    auto &kept = keepSource ? source->second : target->second;
    const auto before = committed(kept.state);
    const bool changed = before.value != merged;
    if (changed)
        kept.identity = std::make_shared<int>(0);
    if (source != drafts.end() && target != drafts.end()) {
        // A merge is not a rename: neither old upload may target the merged document.
        kept.identity = std::make_shared<int>(0);
        auto &lost = keepSource ? target->second : source->second;
        // One native document has one linear undo stack and one IME range.
        // Keep the surviving stack/object; older independent edits remain undoable.
        auto &undo = kept.state.history->undo;
        std::vector<Input::EditSnapshot> imported;
        if (lost.state.history != kept.state.history) {
            imported = std::move(lost.state.history->undo);
            // An identical-draft collision makes no new edit: retain the other
            // document's redo states as recoverable native snapshots as well.
            if (!changed)
                for (auto state = lost.state.history->redo.rbegin();
                     state != lost.state.history->redo.rend(); ++state)
                    imported.push_back(std::move(*state));
        }
        const auto losingCommitted = committed(lost.state);
        imported.push_back(losingCommitted);
        if (!losingCommitted.value.empty() &&
            std::find(kept.history.begin(), kept.history.end(), losingCommitted.value) ==
                kept.history.end())
            kept.history.push_back(losingCommitted.value);
        if (lost.state.preeditBase) {
            imported.push_back(lost.state.current);
            kept.history.push_back(lost.state.current.value);
            lost.state.preeditBase.reset();
            lost.state.preeditStart = lost.state.preeditLength = 0;
            lost.state.preeditCaretHidden = false;
        }
        undo.insert(undo.begin(), std::make_move_iterator(imported.begin()),
                    std::make_move_iterator(imported.end()));
        kept.history.insert(kept.history.end(), std::make_move_iterator(lost.history.begin()),
                            std::make_move_iterator(lost.history.end()));
        for (const auto *text : {&lost.published, &lost.stash})
            if (!text->empty() &&
                std::find(kept.history.begin(), kept.history.end(), *text) == kept.history.end())
                kept.history.push_back(*text);
        kept.typingActive |= lost.typingActive;
        kept.lastTyping = std::max(kept.lastTyping, lost.lastTyping);
        kept.edited = std::max(kept.edited, lost.edited);
        kept.historyPos = -1;
    }
    if (keepSource) {
        auto node = drafts.extract(source);
        drafts.erase(newKey);
        node.key() = newKey;
        drafts.insert(std::move(node));
    } else if (source != drafts.end())
        drafts.erase(source);
    auto &d = drafts.at(newKey);
    for (auto *view : views) {
        if (view->buffer == oldId || view->buffer == newId) {
            retirePointer(*view->uploadButton);
            if (dispatcher && pointerPressedIn(*view))
                dispatcher->cancelPointerCapture();
            view->hideCompletion();
            view->compositionChord.clear();
            view->emojiRevision = 0;
            view->emojiWasEligible = false;
            view->input->resetCaretEffects();
        }
        if (auto saved = view->scrollPositions.extract(oldKey); saved) {
            if (view->buffer == oldId)
                view->scrollPositions.erase(newKey);
            saved.key() = newKey;
            view->scrollPositions.insert(std::move(saved));
        }
        if (view->pasteDestination == oldId)
            view->pasteDestination = newKey;
        if (view->buffer == oldId) {
            view->buffer = newKey;
            if (view->pendingPaste)
                view->pendingPaste->buffer = newKey;
        }
    }
    if (changed) {
        auto &undo = d.state.history->undo;
        if (undo.empty() || undo.back() != before)
            undo.push_back(before);
        d.state.history->redo.clear();
        // Source is the exact prefix, destination the exact suffix (identical
        // drafts are deduplicated). Never search for an ambiguous text occurrence.
        const auto shift = keepSource && merged.starts_with(before.value) ? size_t(0)
                           : !keepSource && merged.ends_with(before.value)
                               ? merged.size() - before.value.size()
                               : std::string::npos;
        if (shift != std::string::npos) {
            auto &state = d.state;
            if (state.preeditBase) {
                state.current.value = merged.substr(0, shift) + state.current.value +
                                      merged.substr(shift + before.value.size());
                state.preeditBase->value = merged;
                state.preeditBase->cursorPos += shift;
                state.preeditBase->selectionAnchor += shift;
                state.preeditStart += shift;
            } else
                state.current.value = merged;
            state.current.cursorPos += shift;
            state.current.selectionAnchor += shift;
        } else {
            // A native edit not present in core's snapshot remains recoverable,
            // but cannot supply a caret/preedit position in the replacement text.
            if (d.state.preeditBase)
                d.history.push_back(d.state.current.value);
            d.state.preeditBase.reset();
            d.state.preeditStart = d.state.preeditLength = 0;
            d.state.preeditCaretHidden = false;
            d.state.current = {merged, merged.size(), merged.size()};
            d.mark = false;
        }
        d.edited = model.now;
        ++d.revision;
    }
    d.published = merged;
    // Colliding views may have equal revisions but different native documents.
    for (auto *view : views)
        if (view->buffer == newId && !view->input->editingStateMatches(d.state))
            view->revision = 0;
    refresh(newKey);
}
bool EditorStore::Impl::valid(std::string_view before, std::string_view after) {
    if (!ui_logic::composerTextValid(after)) {
        model.status = "Text was not inserted: invalid UTF-8 or transport control characters.";
        return false;
    }
    if (before != after && !ui_logic::composerEditAllowed(before, after)) {
        model.status = "Composer limit: 64 KiB. Split this message before adding more text.";
        return false;
    }
    return true;
}
bool EditorStore::Impl::insert(PaneState &pane, std::string_view text) {
    if (!composerWritable(model, pane.buffer)) {
        model.status = "This conversation is unavailable or read-only; no text was inserted.";
        return false;
    }
    for (const auto *view : views)
        if (view->pane.id == pane.id && view->input->composing()) {
            model.status = "Finish or cancel the input-method composition before inserting text.";
            return false;
        }
    auto &d = draft(pane.buffer);
    recover(pane.buffer, d);
    auto e = editor(d);
    e.insert(text);
    Input::EditSnapshot candidate{std::move(e.text), e.cursor, e.anchor};
    preserveIrcSuffix(committed(d.state), candidate, composerForeground(model),
                      themeSurfaceColor("surface_container_high"));
    e.text = std::move(candidate.value);
    e.cursor = candidate.cursorPos;
    e.anchor = candidate.selectionAnchor;
    if (!valid(committed(d.state).value, e.text))
        return false;
    apply(pane.buffer, d, std::move(e), pane.id);
    pane.focusComposer = true;
    return true;
}
bool EditorStore::Impl::permitted(const Buffer &b, std::string_view tag) const {
    auto n = model.networks.find(b.server);
    return n != model.networks.end() && n->second.caps.contains("message-tags") &&
           irc::clientTagAllowed(n->second.clientTagDeny, tag);
}
void EditorStore::Impl::send(PaneState &pane, Draft &d) {
    if (!composerWritable(model, pane.buffer))
        return;
    if (d.state.preeditBase || d.state.preeditLength || d.state.preeditCaretHidden) {
        model.status = "Finish or cancel the input-method composition before sending.";
        return;
    }
    recover(pane.buffer, d);
    const auto text = committed(d.state).value;
    if (text.empty() || !model.buffers.contains(pane.buffer) ||
        parseIrcFormatting(text, composerForeground(model),
                           themeSurfaceColor("surface_container_high"))
                .text.find_first_not_of('\n') == std::string::npos)
        return;
    if (!ui_logic::composerTextValid(text)) {
        model.status =
            "Draft contains invalid UTF-8 or transport controls; edit it before sending.";
        return;
    }
    const auto &b = model.buffers.at(pane.buffer);
    const bool command = text.front() == '/' && !text.starts_with("//");
    Action outgoing{command ? "command" : "send",
                    pane.buffer,
                    text.starts_with("//") ? text.substr(1) : text,
                    {}};
    if (!command && !pane.reply.empty()) {
        const auto source =
            std::find_if(b.messages.begin(), b.messages.end(),
                         [&](const Message &m) { return ui_logic::messageKey(m) == pane.reply; });
        if (source == b.messages.end() || source->nick.empty()) {
            model.status =
                "Reply source or author is unavailable. Cancel the reply to send normally.";
            return;
        }
        if (!source->pending && !source->id.starts_with("local:"))
            outgoing.extra = source->id;
        outgoing.attributes.emplace("reply-nick", source->nick);
    }
    // Dispatch re-evaluates capabilities and builds any fallback prefix from this raw body.
    model.actions.push_back(std::move(outgoing));
    d.history.push_back(text);
    if (d.history.size() > 100)
        d.history.erase(d.history.begin());
    d.historyPos = -1;
    auto e = editor(d);
    e.text.clear();
    e.cursor = e.anchor = 0;
    e.mark = false;
    apply(pane.buffer, d, std::move(e), pane.id);
    pane.reply.clear();
    pane.following = pane.focusComposer = true;
    if (d.typingActive && model.settings.typingSend && permitted(b, "+typing"))
        model.actions.push_back({"typing", pane.buffer, "done", {}});
    d.typingActive = false;
}
void EditorStore::Impl::paste(Composer &view) {
    if (!composerWritable(model, view.buffer))
        return;
    if (view.input->composing()) {
        model.status = "Finish or cancel the input-method composition before pasting.";
        return;
    }
    auto &d = draft(view.buffer);
    const auto intent = std::make_shared<PasteIntent>(view.buffer, d.revision);
    view.pendingPaste = intent;
    const auto paneId = view.pane.id;
    const std::weak_ptr<int> token = alive;
    auto receive = [this, token, intent, paneId](std::optional<std::string> value,
                                                 std::string mime = {}) {
        if (token.expired())
            return;
        auto found = std::find_if(views.begin(), views.end(), [&](const Composer *v) {
            return v->pane.id == paneId && v->pendingPaste == intent &&
                   v->buffer == intent->buffer && v->pane.buffer == intent->buffer;
        });
        if (found == views.end()) {
            model.status = "Paste cancelled because its conversation view was closed.";
            return;
        }
        auto &view = **found;
        if (!composerWritable(model, view.buffer))
            return;
        view.pendingPaste.reset();
        if (view.input->composing()) {
            model.status = "Paste cancelled because an input-method composition is active.";
            return;
        }
        const auto &id = intent->buffer;
        if (!mime.empty()) {
            if (!value || draft(id).revision != intent->revision || !model.buffers.contains(id) ||
                model.buffers.at(id).draft != draft(id).published) {
                model.status = "Image paste cancelled because its draft or caret changed.";
                return;
            }
            if (!onImageUploadRequested) {
                model.status = "Image uploads are unavailable.";
                return;
            }
            onImageUploadRequested(paneId, id, *view.input, std::move(mime), std::move(*value));
            return;
        }
        if (!value) {
            model.status = "The current clipboard selection could not be read.";
            return;
        }
        if (draft(id).revision != intent->revision || !model.buffers.contains(id) ||
            model.buffers.at(id).draft != draft(id).published) {
            view.setPaste(ui_logic::clipboardNewlines(std::move(*value)),
                          "The draft or caret changed while reading the clipboard. "
                          "Review this destination before inserting.");
            return;
        }
        auto text = ui_logic::clipboardNewlines(std::move(*value));
        if (!ui_logic::composerTextValid(text)) {
            model.status = "Paste rejected: invalid UTF-8 or transport control characters. "
                           "Clipboard is unchanged.";
            return;
        }
        auto e = editor(draft(id));
        const size_t lines = std::count(text.begin(), text.end(), '\n') + 1;
        if (lines >= 6 || !ui_logic::composerInsertAllowed(e, text))
            view.setPaste(std::move(text));
        else
            insert(view.pane, text);
    };
    if (payloadClipboard) {
        payloadClipboard->requestPayload(
            static_cast<std::size_t>(
                std::min(model.settings.uploads.maxBytes,
                         uploadProviderLimit(model.settings.uploads.provider))),
            [receive = std::move(receive)](Clipboard::Payload payload) mutable {
                if (payload.status != Clipboard::PayloadStatus::Success) {
                    receive(std::nullopt);
                    return;
                }
                auto mime =
                    payload.mime.starts_with("image/") ? std::move(payload.mime) : std::string{};
                receive(std::move(payload.bytes), std::move(mime));
            });
    } else {
        clipboard.requestClipboardText(
            [receive = std::move(receive)](std::optional<std::string> text) mutable {
                receive(std::move(text));
            });
    }
}
bool EditorStore::Impl::execute(std::string_view op, PaneState &pane) {
    if (const auto buffer = model.buffers.find(pane.buffer);
        buffer != model.buffers.end() && buffer->second.kind == "mentions")
        return false;
    if (op.starts_with("insert-nick:")) {
        insert(pane, std::string(op.substr(12)) + ": ");
        return true;
    }
    if (op.starts_with("insert:")) {
        insert(pane, op.substr(7));
        return true;
    }
    auto view = std::find_if(views.begin(), views.end(),
                             [&](const Composer *v) { return v->pane.id == pane.id; });
    if (op == "cycle-spelling") {
        if (view != views.end())
            (*view)->cycleSpelling();
        return true;
    }
    if ((op == "copy" || op == "copy-region") && view != views.end()) {
        const auto &input = *(*view)->input;
        const auto start = std::min(input.cursorPosition(), input.selectionAnchor());
        const auto end = std::max(input.cursorPosition(), input.selectionAnchor());
        if (start != end) {
            kill.assign(input.value(), start, end - start);
            clipboard.setClipboardText(kill);
        }
        return true;
    }
    if (op == "paste") {
        if (view != views.end())
            paste(**view);
        return true;
    }
    if (op == "yank" && view != views.end() && (*view)->input->composing()) {
        model.status = "Finish or cancel the input-method composition before yanking.";
        return true;
    }
    if (op == "cancel" && view != views.end()) {
        auto &v = **view;
        if (v.input->composing()) {
            v.input->textInputResetPreedit();
            capture(v);
            return true;
        }
        if (!v.paste.empty()) {
            v.cancelPaste();
            return true;
        }
    }
    constexpr std::string_view operations[] = {"send",
                                               "newline",
                                               "history-previous",
                                               "history-next",
                                               "complete",
                                               "backward-char",
                                               "forward-char",
                                               "backward-word",
                                               "forward-word",
                                               "beginning-of-line",
                                               "end-of-line",
                                               "previous-line",
                                               "next-line",
                                               "set-mark",
                                               "cancel",
                                               "undo",
                                               "redo",
                                               "yank",
                                               "kill-region",
                                               "copy-region",
                                               "kill-line",
                                               "backward-kill-line",
                                               "transpose-chars",
                                               "kill-word",
                                               "backward-kill-word",
                                               "delete-char",
                                               "backward-delete",
                                               "copy",
                                               "cut",
                                               "select-all"};
    const bool formatting = op.starts_with("format:");
    if (formatting && view != views.end() && (*view)->input->composing()) {
        model.status = "Finish or cancel the input-method composition before changing formatting.";
        return true;
    }
    if (!formatting &&
        std::find(std::begin(operations), std::end(operations), op) == std::end(operations))
        return false;
    if (view != views.end() && (*view)->input->composing()) {
        model.status = "Finish or cancel the input-method composition before editing.";
        return true;
    }
    auto &d = draft(pane.buffer);
    recover(pane.buffer, d);
    if (op == "send") {
        send(pane, d);
        return true;
    }
    if (op == "undo" || op == "redo") {
        auto &source = op == "undo" ? d.state.history->undo : d.state.history->redo;
        auto &target = op == "undo" ? d.state.history->redo : d.state.history->undo;
        if (!source.empty()) {
            target.push_back(committed(d.state));
            if (target.size() > 100)
                target.erase(target.begin());
            auto state = std::move(source.back());
            source.pop_back();
            auto e = editor(d);
            e.text = std::move(state.value);
            e.cursor = state.cursorPos;
            e.anchor = state.selectionAnchor;
            e.mark = e.cursor != e.anchor;
            apply(pane.buffer, d, std::move(e), pane.id, false);
        }
        pane.focusComposer = true;
        return true;
    }
    auto e = editor(d);
    std::optional<std::pair<size_t, size_t>> wordKill;
    if (op == "newline")
        e.insert("\n");
    else if (op == "history-previous" || op == "history-next") {
        if (d.history.empty())
            return true;
        if (d.historyPos < 0) {
            d.stash = e.text;
            d.historyPos = int(d.history.size());
        }
        d.historyPos = std::clamp(d.historyPos + (op == "history-previous" ? -1 : 1), 0,
                                  int(d.history.size()));
        e.text = d.historyPos == int(d.history.size()) ? d.stash : d.history[d.historyPos];
        e.cursor = e.anchor = e.text.size();
        e.mark = false;
    } else if (op == "complete") {
        if (view != views.end()) {
            (*view)->spellingCycle.reset();
            (*view)->openLiteralCompletion();
        }
        return true;
    } else if (op == "select-all") {
        e.anchor = 0;
        e.cursor = e.text.size();
        e.mark = true;
    } else if (formatting) {
        const auto name = op.substr(7);
        const Color normal = composerForeground(model);
        const auto document =
            parseIrcFormatting(e.text, normal, themeSurfaceColor("surface_container_high"));
        bool selected = e.mark && e.cursor != e.anchor;
        if (selected && document.rawToDisplay[std::min(e.cursor, e.anchor)] ==
                            document.rawToDisplay[std::max(e.cursor, e.anchor)]) {
            // Hidden metadata has no visible selection: change the native caret's typing mode.
            e.mark = selected = false;
            e.anchor = e.cursor;
        }
        const size_t start = selected ? std::min(e.cursor, e.anchor) : e.cursor;
        const size_t end = selected ? std::max(e.cursor, e.anchor) : e.cursor;
        const auto &style = document.styleAt(start);
        const auto allSelected = [&](const auto &predicate) {
            if (!selected)
                return predicate(style);
            bool any = false;
            for (const auto &run : document.runs)
                if (run.end > document.rawToDisplay[start] &&
                    run.start < document.rawToDisplay[end]) {
                    any = true;
                    if (!predicate(run.style))
                        return false;
                }
            return any || predicate(style);
        };
        const FormatToggle *toggle = nullptr;
        for (const auto &candidate : formatToggles)
            if (name == candidate.command)
                toggle = &candidate;
        bool foreground = false, background = false, active = false;
        using ColorKind = IrcColor::Kind;
        std::optional<IrcColor> color;
        std::string digits;
        if (toggle)
            active = !allSelected([&](const IrcStyle &value) { return value.*(toggle->active); });
        else if (name.starts_with("foreground:") || name.starts_with("background:")) {
            foreground = name.starts_with("foreground:");
            background = !foreground;
            digits = name.substr(name.find(':') + 1);
            if (digits.size() != 2 || digits[0] < '0' || digits[0] > '1' || digits[1] < '0' ||
                digits[1] > '9' || (digits[0] == '1' && digits[1] > '5'))
                return false;
            color = IrcColor{ColorKind::Indexed,
                             static_cast<std::uint32_t>((digits[0] - '0') * 10 + digits[1] - '0')};
            if (allSelected([&](const IrcStyle &value) {
                    return (foreground ? value.foregroundSource : value.backgroundSource) == *color;
                }))
                color.reset();
        } else if (name != "reset")
            return false;
        const auto transform = [&](IrcStyle value) {
            if (toggle) {
                if (toggle->active == &IrcStyle::reverse && value.reverse != active)
                    std::swap(value.foregroundSource, value.backgroundSource);
                value.*(toggle->active) = active;
            } else if (foreground) {
                const auto fallback =
                    value.reverse && value.backgroundSource.kind != ColorKind::Background
                        ? ColorKind::Background
                        : ColorKind::Foreground;
                value.foregroundSource = color.value_or(IrcColor{fallback, 0});
            } else if (background) {
                const auto fallback =
                    value.reverse || value.foregroundSource.kind == ColorKind::Background
                        ? ColorKind::Foreground
                        : ColorKind::Background;
                value.backgroundSource = color.value_or(IrcColor{fallback, 0});
            } else
                value = {};
            ircResolveColors(value, normal, themeSurfaceColor("surface_container_high"));
            return value;
        };
        if (selected) {
            std::string replacement;
            auto current = style;
            for (size_t position = start; position < end;) {
                const size_t next = std::min(end, ui_logic::next(e.text, position));
                if (document.rawToDisplay[next] != document.rawToDisplay[position]) {
                    const auto desired = transform(document.styleAt(position));
                    replacement += ircFormattingTransition(current, desired);
                    replacement.append(e.text, position, next - position);
                    current = desired;
                }
                position = next;
            }
            replacement += ircFormattingTransition(current, document.styleAt(end));
            e.text.replace(start, end - start, replacement);
            e.cursor = e.anchor = start + replacement.size();
        } else {
            const auto desired = transform(style);
            std::string code = toggle                ? std::string(1, toggle->byte)
                               : foreground && color ? std::string("\x03") + digits + "\x02\x02"
                               : foreground || background ? ircFormattingCodes(desired)
                                                          : std::string("\x0f");
            const auto restore =
                start < e.text.size() ? ircFormattingTransition(desired, style) : std::string{};
            e.text.insert(start, code + restore);
            e.cursor = e.anchor = start + code.size();
        }
        e.mark = false;
    } else {
        if (op == "copy")
            op = "copy-region";
        if (op == "cut")
            op = "kill-region";
        const bool selected = e.mark && e.cursor != e.anchor;
        const auto *projection =
            view != views.end() ? (*view)->input->displayProjection() : nullptr;
        if (op == "transpose-chars") {
            if (selected)
                return true;
            const auto document = parseIrcFormatting(e.text, composerForeground(model),
                                                     themeSurfaceColor("surface_container_high"));
            MessageSelectionText graphemes;
            graphemes.update(document.text);
            std::vector<size_t> boundaries;
            boundaries.reserve(graphemes.boundaries().size());
            const auto &map = document.rawToDisplay;
            for (const auto boundary : graphemes.boundaries())
                boundaries.push_back(std::lower_bound(map.begin(), map.end(), boundary) -
                                     map.begin());
            boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
            const size_t oldCursor = e.cursor, oldAnchor = e.anchor;
            e.cursor = e.anchor =
                std::lower_bound(map.begin(), map.end(), map[e.cursor]) - map.begin();
            const auto range = e.transposition(boundaries);
            e.cursor = oldCursor;
            e.anchor = oldAnchor;
            if (range) {
                const auto [start, split, end] = *range;
                // Move each raw grapheme with its original style, then restore the suffix.
                std::string replacement =
                    ircFormattingTransition(document.styleAt(start), document.styleAt(split));
                replacement.append(e.text, split, end - split);
                replacement +=
                    ircFormattingTransition(document.styleAt(end), document.styleAt(start));
                replacement.append(e.text, start, split - start);
                replacement +=
                    ircFormattingTransition(document.styleAt(split), document.styleAt(end));
                e.replace(start, end, replacement);
            }
        } else if (projection && projection->rawToDisplay.size() == e.text.size() + 1 &&
                   (op == "backward-char" || op == "forward-char")) {
            const size_t old = e.cursor;
            do {
                const size_t previous = e.cursor;
                e.command(op, kill);
                if (e.cursor == previous)
                    break;
            } while (projection->rawToDisplay[e.cursor] == projection->rawToDisplay[old]);
            while (e.cursor < e.text.size()) {
                const size_t next = ui_logic::next(e.text, e.cursor);
                if (projection->rawToDisplay[next] != projection->rawToDisplay[e.cursor])
                    break;
                e.cursor = next;
            }
            if (!e.mark)
                e.anchor = e.cursor;
        } else if (projection && projection->rawToDisplay.size() == e.text.size() + 1 &&
                   (op == "backward-word" || op == "forward-word" || op == "kill-word" ||
                    op == "backward-kill-word")) {
            const bool forward = op == "forward-word" || op == "kill-word";
            const size_t boundary = (*view)->input->projectedWordBoundary(e.cursor, forward);
            if (op == "backward-word" || op == "forward-word") {
                e.cursor = boundary;
                if (!e.mark)
                    e.anchor = boundary;
            } else {
                const auto &map = projection->rawToDisplay;
                const auto displayCursor = map[e.cursor];
                size_t cursor =
                    forward ? size_t(std::upper_bound(map.begin(), map.end(), displayCursor) -
                                     map.begin() - 1)
                            : size_t(std::lower_bound(map.begin(), map.end(), displayCursor) -
                                     map.begin());
                while (forward && cursor && cursor < e.text.size() &&
                       (static_cast<unsigned char>(e.text[cursor]) & 0xc0) == 0x80)
                    --cursor;
                const size_t start = forward ? cursor : boundary, end = forward ? boundary : cursor;
                if (start >= end)
                    return true;
                wordKill = std::pair{start, end};
                e.replace(start, end, {});
            }
        } else if (projection && projection->rawToDisplay.size() == e.text.size() + 1 &&
                   !selected && (op == "backward-delete" || op == "delete-char")) {
            size_t start = e.cursor, end = e.cursor;
            if (op == "backward-delete") {
                while (end) {
                    const size_t previous = ui_logic::previous(e.text, end);
                    if (projection->rawToDisplay[previous] != projection->rawToDisplay[end])
                        break;
                    end = previous;
                }
                start = ui_logic::previous(e.text, end);
                e.replace(start, end, {});
            } else {
                do {
                    const size_t previous = end;
                    end = ui_logic::next(e.text, end);
                    if (end == previous)
                        break;
                } while (projection->rawToDisplay[end] == projection->rawToDisplay[start]);
                if (projection->rawToDisplay[start] != projection->rawToDisplay[end])
                    e.replace(start, end, {});
            }
        } else
            e.command(op, kill);
        if (selected && (op == "copy-region" || op == "kill-region"))
            clipboard.setClipboardText(kill);
    }
    if (op == "cancel")
        pane.reply.clear();
    const bool history = op == "history-previous" || op == "history-next";
    if (!history && e.text != committed(d.state).value) {
        Input::EditSnapshot candidate{std::move(e.text), e.cursor, e.anchor};
        preserveIrcSuffix(committed(d.state), candidate, composerForeground(model),
                          themeSurfaceColor("surface_container_high"));
        e.text = std::move(candidate.value);
        e.cursor = candidate.cursorPos;
        e.anchor = candidate.selectionAnchor;
    }
    if (!history && !valid(committed(d.state).value, e.text))
        return true;
    if (wordKill)
        kill.assign(committed(d.state).value, wordKill->first, wordKill->second - wordKill->first);
    apply(pane.buffer, d, std::move(e), pane.id);
    pane.focusComposer = true;
    return true;
}

bool EditorStore::Impl::Composer::updateNickDecoration() {
    const auto source = owner.model.buffers.find(buffer);
    const auto *current = source == owner.model.buffers.end() ? nullptr : &source->second;
    const auto network =
        current ? owner.model.networks.find(current->server) : owner.model.networks.end();
    const std::string_view server =
        current ? std::string_view(current->server) : std::string_view{};
    const std::string_view mapping = network == owner.model.networks.end()
                                         ? std::string_view{"rfc1459"}
                                         : std::string_view{network->second.caseMapping};
    const std::string_view own = network == owner.model.networks.end()
                                     ? std::string_view{}
                                     : std::string_view{network->second.nick};
    const std::string_view query = current && current->kind == "query"
                                       ? std::string_view{current->target}
                                       : std::string_view{};
    const auto &settings = owner.model.settings;
    bool changed = nickSource != buffer || nickServer != server || nickMapping != mapping ||
                   nickOwn != own || nickQuery != query || nickPalette != settings.nickPalette ||
                   nickOverrides != settings.nickColors || nickTheme != themeRevision() ||
                   nickNames.size() != (current ? current->nicks.size() : 0);
    if (!changed && current)
        for (size_t i = 0; i < nickNames.size() && !changed; ++i)
            changed = nickNames[i] != current->nicks[i].name;
    if (!changed)
        return false;
    nickSource = buffer;
    nickServer = server;
    nickMapping = mapping;
    nickOwn = own;
    nickQuery = query;
    nickPalette = settings.nickPalette;
    nickOverrides = settings.nickColors;
    nickTheme = themeRevision();
    nickNames.resize(current ? current->nicks.size() : 0);
    for (size_t i = 0; i < nickNames.size(); ++i)
        nickNames[i] = current->nicks[i].name;
    return true;
}

Input::DisplayProjection EditorStore::Impl::Composer::project(std::string_view raw) {
    formatDocument = parseIrcFormatting(raw, projectionForeground, projectionBackground);
    projectionHasText = formatDocument.text.find_first_not_of('\n') != std::string::npos;
    const auto source = owner.model.buffers.find(buffer);
    const auto nicks = source == owner.model.buffers.end()
                           ? std::vector<NickAnnotation>{}
                           : annotateNicks(owner.model, source->second, formatDocument);
    const auto rawEnd = raw.find_first_of(" \t\r\n");
    const size_t commandEnd =
        raw.starts_with('/') && !raw.starts_with("//")
            ? formatDocument.rawToDisplay[rawEnd == raw.npos ? raw.size() : rawEnd]
            : 0;
    const bool annotate = owner.model.settings.spellcheck && !input->composing() &&
                          spellBuffer == buffer &&
                          spellLanguage == owner.model.settings.spellcheckLanguage &&
                          spellText == formatDocument.text;
    const auto markup = [&](bool colors) {
        std::string result;
        for (const auto &run : formatDocument.runs) {
            size_t begin = run.start;
            while (begin < run.end) {
                size_t end = run.end;
                bool misspelled = false;
                if (begin < commandEnd)
                    end = std::min(end, commandEnd);
                if (annotate && !run.style.monospace)
                    for (const auto &range : spellMisspelled) {
                        if (range.end <= begin)
                            continue;
                        if (range.begin > begin)
                            end = std::min(end, range.begin);
                        else {
                            misspelled = true;
                            end = std::min(end, range.end);
                        }
                        break;
                    }
                auto style = run.style;
                for (const auto &nick : nicks) {
                    if (nick.end <= begin)
                        continue;
                    if (nick.begin > begin)
                        end = std::min(end, nick.begin);
                    else {
                        end = std::min(end, nick.end);
                        if (!style.reverse &&
                            style.foregroundSource.kind == IrcColor::Kind::Foreground)
                            style.foreground =
                                nickColor(owner.model, nick.nick, source->second.server);
                    }
                    break;
                }
                if (begin < commandEnd) {
                    style.bold = style.monospace = true;
                    style.foreground = themeForegroundColor("primary");
                }
                if (misspelled) {
                    // Override only the underline; the stored IRC style remains untouched.
                    style.underline = false;
                    result += "<span underline=\"error\" underline_color=\"" +
                              formatRgbHex(themeForegroundColor("error")) + "\">";
                }
                irc_detail::appendMarkup(
                    result, std::string_view(formatDocument.text).substr(begin, end - begin), style,
                    colors);
                if (misspelled)
                    result += "</span>";
                begin = end;
            }
        }
        return result;
    };
    auto normal = markup(true), contrast = markup(false);
    return {std::move(formatDocument.text), std::move(normal), std::move(contrast),
            std::move(formatDocument.rawToDisplay)};
}

void EditorStore::Impl::Composer::excludeSpelling() {
    spellScratch.clear();
    const auto exclude = [&](size_t begin, size_t end) {
        if (begin < end)
            spellScratch.push_back({begin, end});
    };
    const auto nickCharacter = [](const char *at) {
        const auto c = g_utf8_get_char(at);
        return g_unichar_isalnum(c) || g_unichar_ismark(c) ||
               (c < 128 && std::string_view{"-_[]\\`^{}|"}.find(static_cast<char>(c)) !=
                               std::string_view::npos);
    };
    const auto foldedText = irc::casefold(spellText, spellMapping);
    for (const auto &name : spellFoldedNames) {
        if (name.empty())
            continue;
        for (size_t begin = foldedText.find(name); begin != foldedText.npos;
             begin = foldedText.find(name, begin + name.size())) {
            const auto end = begin + name.size();
            const char *previous =
                begin ? g_utf8_find_prev_char(spellText.data(), spellText.data() + begin) : nullptr;
            if ((!previous || !nickCharacter(previous)) &&
                (end == spellText.size() || !nickCharacter(spellText.data() + end)))
                exclude(begin, end);
        }
    }
    for (const auto &run : formatDocument.runs)
        if (run.style.monospace)
            exclude(run.start, run.end);
    for (size_t begin = 0; begin < spellText.size();) {
        const auto newline = spellText.find('\n', begin);
        const auto end = newline == spellText.npos ? spellText.size() : newline;
        const auto first = spellText.find_first_not_of(" \t", begin);
        if (first < end && spellText[first] == '/' &&
            (first + 1 == end || spellText[first + 1] != '/'))
            exclude(begin, end);
        begin = end == spellText.size() ? end : end + 1;
    }
    for (size_t begin = spellText.find('`'); begin != spellText.npos;) {
        auto end = spellText.find('`', begin + 1);
        end = end == spellText.npos ? spellText.size() : end + 1;
        exclude(begin, end);
        begin = spellText.find('`', end);
    }
    const auto source = owner.model.buffers.find(buffer);
    const auto network = source == owner.model.buffers.end()
                             ? owner.model.networks.end()
                             : owner.model.networks.find(source->second.server);
    const std::string_view channelTypes = network == owner.model.networks.end()
                                              ? std::string_view{"#&+!"}
                                              : std::string_view{network->second.channelTypes};
    for (size_t begin = 0; begin < spellText.size();) {
        const auto character = g_utf8_get_char(spellText.data() + begin);
        if (g_unichar_isspace(character)) {
            begin =
                static_cast<size_t>(g_utf8_next_char(spellText.data() + begin) - spellText.data());
            continue;
        }
        size_t end = begin;
        while (end < spellText.size() &&
               !g_unichar_isspace(g_utf8_get_char(spellText.data() + end)))
            end = static_cast<size_t>(g_utf8_next_char(spellText.data() + end) - spellText.data());
        auto token = std::string_view(spellText).substr(begin, end - begin);
        while (!token.empty() &&
               std::string_view{"(\"'<@"}.find(token.front()) != std::string_view::npos)
            token.remove_prefix(1);
        while (!token.empty() &&
               std::string_view{".,:;!?()\"<>"}.find(token.back()) != std::string_view::npos)
            token.remove_suffix(1);
        // URI-like tokens are never dictionary prose, even for schemes not launchable by chat.
        const bool uri =
            token.find("://") != token.npos ||
            (token.size() >= 4 && g_ascii_strncasecmp(token.data(), "www.", 4) == 0) ||
            (token.size() >= 7 && g_ascii_strncasecmp(token.data(), "mailto:", 7) == 0);
        const bool channel =
            !token.empty() && channelTypes.find(token.front()) != channelTypes.npos;
        if (uri || channel)
            exclude(begin, end);
        begin = end;
    }
    std::sort(spellScratch.begin(), spellScratch.end(), [](auto a, auto b) {
        return a.begin < b.begin || (a.begin == b.begin && a.end < b.end);
    });
    size_t count = 0;
    for (const auto range : spellScratch) {
        if (count && range.begin <= spellScratch[count - 1].end)
            spellScratch[count - 1].end = std::max(spellScratch[count - 1].end, range.end);
        else
            spellScratch[count++] = range;
    }
    spellScratch.resize(count);
}

void EditorStore::Impl::Composer::updateSpelling() {
    const auto &settings = owner.model.settings;
    const auto source = owner.model.buffers.find(buffer);
    if (!settings.spellcheck || source == owner.model.buffers.end() || buffer != pane.buffer) {
        const bool decorated = !spellMisspelled.empty();
        spellTicket = 0;
        spellNeedsCheck = false;
        spellMisspelled.clear();
        spellError.clear();
        spellBuffer.clear();
        if (decorated)
            input->refreshDisplayProjection(true);
        return;
    }
    if (input->composing())
        return;
    const auto *projection = input->displayProjection();
    if (!projection)
        return;
    const auto network = owner.model.networks.find(source->second.server);
    const std::string_view mapping = network == owner.model.networks.end()
                                         ? std::string_view{"rfc1459"}
                                         : std::string_view{network->second.caseMapping};
    const std::string_view channelTypes = network == owner.model.networks.end()
                                              ? std::string_view{"#&+!"}
                                              : std::string_view{network->second.channelTypes};
    const std::string_view ownNick = network == owner.model.networks.end()
                                         ? std::string_view{}
                                         : std::string_view{network->second.nick};
    const std::string_view queryNick = source->second.kind == "query"
                                           ? std::string_view{source->second.target}
                                           : std::string_view{};
    const auto &nicks = source->second.nicks;
    const bool channelsChanged = spellChannelTypes != channelTypes;
    bool namesChanged = spellMapping != mapping || spellNames.size() != nicks.size() + 2;
    if (!namesChanged) {
        for (size_t i = 0; i < nicks.size() && !namesChanged; ++i)
            namesChanged = spellNames[i] != nicks[i].name;
        namesChanged = namesChanged || spellNames[nicks.size()] != ownNick ||
                       spellNames[nicks.size() + 1] != queryNick;
    }
    if (namesChanged) {
        spellMapping = mapping;
        spellNames.resize(nicks.size() + 2);
        spellFoldedNames.resize(spellNames.size());
        for (size_t i = 0; i < nicks.size(); ++i)
            spellNames[i] = nicks[i].name;
        spellNames[nicks.size()] = ownNick;
        spellNames[nicks.size() + 1] = queryNick;
        for (size_t i = 0; i < spellNames.size(); ++i)
            spellFoldedNames[i] = irc::casefold(spellNames[i], spellMapping);
    }
    const auto identity = owner.draft(buffer).identity;
    const bool sourceChanged = spellBuffer != buffer || spellIdentity.lock() != identity;
    const bool textChanged = spellText != projection->text;
    const bool languageChanged = spellLanguage != settings.spellcheckLanguage;
    const bool rawChanged = spellRaw != input->value();
    if (!sourceChanged && !textChanged && !languageChanged && !rawChanged && !namesChanged &&
        !channelsChanged)
        return;
    spellRaw = input->value();
    spellText = projection->text;
    spellIdentity = identity;
    spellChannelTypes = channelTypes;
    spellBuffer = buffer;
    spellLanguage = settings.spellcheckLanguage;
    excludeSpelling();
    const bool exclusionsChanged =
        spellScratch.size() != spellExcluded.size() ||
        !std::equal(spellScratch.begin(), spellScratch.end(), spellExcluded.begin(),
                    [](auto a, auto b) { return a.begin == b.begin && a.end == b.end; });
    if (!sourceChanged && !textChanged && !languageChanged && !exclusionsChanged)
        return;
    spellExcluded.swap(spellScratch);
    spellTicket = ++owner.spellCounter;
    spellNeedsCheck = true;
    spellDue = std::chrono::steady_clock::now() + std::chrono::milliseconds(180);
    const bool decorated = !spellMisspelled.empty();
    spellMisspelled.clear();
    spellError.clear();
    if (decorated)
        input->refreshDisplayProjection(true);
}

bool EditorStore::Impl::Composer::spellingCycleValid() const {
    if (!spellingCycle || !owner.model.settings.spellcheck || input->composing() || menuOpen ||
        !visibleInScene(*this) || !owner.dispatcher ||
        owner.dispatcher->focusedArea() != input->inputArea())
        return false;
    const auto &cycle = *spellingCycle;
    const auto source = owner.model.buffers.find(buffer);
    const auto draft = owner.drafts.find(buffer);
    return source != owner.model.buffers.end() && draft != owner.drafts.end() &&
           buffer == pane.buffer && buffer == cycle.buffer &&
           cycle.language == owner.model.settings.spellcheckLanguage &&
           cycle.identity.lock() == draft->second.identity &&
           cycle.revision == draft->second.revision && revision == cycle.revision &&
           source->second.draft == draft->second.published &&
           input->editingStateMatches(draft->second.state) && input->value() == cycle.raw &&
           input->cursorPosition() == cycle.caret && input->selectionAnchor() == cycle.caret &&
           input->displayProjection();
}

std::optional<SpellRange> EditorStore::Impl::Composer::rawSpellingRange(SpellRange range) const {
    const auto *projection = input->displayProjection();
    if (!projection)
        return std::nullopt;
    const auto &mapping = projection->rawToDisplay;
    const auto start = std::upper_bound(mapping.begin(), mapping.end(), range.begin);
    const auto end = std::lower_bound(mapping.begin(), mapping.end(), range.end);
    if (start == mapping.begin() || end == mapping.end())
        return std::nullopt;
    auto begin = static_cast<size_t>(start - mapping.begin() - 1);
    const auto &raw = input->value();
    while (begin && begin < raw.size() && (static_cast<unsigned char>(raw[begin]) & 0xc0) == 0x80)
        --begin;
    return SpellRange{begin, static_cast<size_t>(end - mapping.begin())};
}

void EditorStore::Impl::Composer::cycleSpelling() {
    owner.capture(*this);
    if (!spellingCycleValid())
        spellingCycle.reset();
    if (!spellingCycle) {
        const auto &draft = owner.draft(buffer);
        spellingCycle = SpellingCycle{.buffer = buffer,
                                      .language = owner.model.settings.spellcheckLanguage,
                                      .raw = input->value(),
                                      .identity = draft.identity,
                                      .revision = revision,
                                      .caret = input->cursorPosition()};
        if (!spellingCycleValid()) {
            spellingCycle.reset();
            return;
        }
        updateSpelling();
        spellingCycle->ticket = spellTicket;
    }
    ++spellingCycle->presses;
    if (spellingCycle->suggestions.empty() &&
        (spellNeedsCheck || owner.spellInFlight == spellTicket)) {
        spellDue = std::chrono::steady_clock::now();
        owner.syncSpelling();
        return;
    }
    applySpellingCycle();
}

void EditorStore::Impl::Composer::applySpellingCycle() {
    if (!spellingCycleValid()) {
        spellingCycle.reset();
        return;
    }
    auto &cycle = *spellingCycle;
    if (cycle.suggestions.empty()) {
        if (cycle.ticket != spellTicket || !spellError.empty()) {
            spellingCycle.reset();
            return;
        }
        const auto *projection = input->displayProjection();
        if (spellRaw != input->value() || spellText != projection->text ||
            cycle.caret >= projection->rawToDisplay.size()) {
            spellingCycle.reset();
            return;
        }
        const auto caret = projection->rawToDisplay[cycle.caret];
        std::optional<SpellRange> target;
        for (const auto range : spellMisspelled) {
            if (range.end > caret)
                break;
            bool adjacent = true;
            for (auto offset = range.end; offset < caret;) {
                const auto character = g_utf8_get_char(spellText.data() + offset);
                if (character == '\n' || character == '\r' || character == 0x85 ||
                    character == 0x2028 || character == 0x2029 ||
                    (!g_unichar_isspace(character) && !g_unichar_ispunct(character))) {
                    adjacent = false;
                    break;
                }
                offset = static_cast<size_t>(g_utf8_next_char(spellText.data() + offset) -
                                             spellText.data());
            }
            if (adjacent)
                target = range;
        }
        if (!target) {
            spellingCycle.reset();
            return;
        }
        const auto raw = rawSpellingRange(*target);
        if (!raw) {
            spellingCycle.reset();
            return;
        }
        cycle.range = *raw;
        cycle.suggestions = owner.spelling.suggest(
            std::string_view(spellText).substr(target->begin, target->end - target->begin),
            cycle.language);
        if (cycle.suggestions.empty()) {
            spellingCycle.reset();
            return;
        }
        cycle.index = (cycle.presses - 1) % cycle.suggestions.size();
    } else {
        cycle.index = (cycle.index + cycle.presses) % cycle.suggestions.size();
    }
    cycle.presses = 0;
    const auto *projection = input->displayProjection();
    const auto displayBegin = projection->rawToDisplay[cycle.range.begin];
    const auto &suggestion = cycle.suggestions[cycle.index];
    spellingEdit = cycle.range;
    cyclingSpelling = true;
    const bool replaced = input->replaceRange(cycle.range.begin, cycle.range.end, suggestion);
    cyclingSpelling = false;
    spellingEdit.reset();
    if (!replaced) {
        spellingCycle.reset();
        return;
    }
    // The IRC suffix rewriter can insert controls: derive the next range from the new projection.
    const auto range = rawSpellingRange({displayBegin, displayBegin + suggestion.size()});
    if (!range) {
        spellingCycle.reset();
        return;
    }
    cycle.range = *range;
    cycle.raw = input->value();
    cycle.caret = input->cursorPosition();
    cycle.revision = revision;
}

void EditorStore::Impl::syncSpelling() {
    for (auto *view : views) {
        if (view->spellingCycle && !view->spellingCycleValid())
            view->spellingCycle.reset();
        view->updateSpelling();
    }
    if (auto result = spelling.poll()) {
        if (result->ticket == spellInFlight)
            spellInFlight = 0;
        for (auto *view : views)
            if (view->spellTicket == result->ticket && model.settings.spellcheck &&
                view->spellLanguage == model.settings.spellcheckLanguage &&
                view->buffer == view->pane.buffer) {
                view->spellError = std::move(result->error);
                view->spellMisspelled = std::move(result->misspelled);
                view->input->refreshDisplayProjection(true);
                if (view->spellingCycle && view->spellingCycle->presses)
                    view->applySpellingCycle();
                break;
            }
    }
    if (spellInFlight && std::any_of(views.begin(), views.end(), [&](const auto *view) {
            return view->spellTicket == spellInFlight;
        }))
        return;
    // A changed source snapshot supersedes old work after its own debounce.
    spellInFlight = 0;
    const auto now = std::chrono::steady_clock::now();
    for (auto *view : views)
        if (view->spellNeedsCheck && !view->input->composing() && visibleInScene(*view) &&
            now >= view->spellDue) {
            view->spellNeedsCheck = false;
            spellInFlight = view->spellTicket;
            spelling.request(view->spellTicket, view->spellText, view->spellLanguage,
                             view->spellExcluded);
            break;
        }
}

void EditorStore::Impl::Composer::contextMenu(const InputArea::PointerData *pointer) {
    spellingCycle.reset();
    const auto invocation = pointer ? std::optional(*pointer) : std::nullopt;
    pointer = invocation ? &*invocation : nullptr;
    auto *popup = input->popupContext();
    if (!popup || buffer != pane.buffer || !owner.model.buffers.contains(buffer))
        return;
    owner.capture(*this);
    owner.syncSpelling();
    if (pointer)
        owner.retirePointer(*input);
    hideCompletion();
    const auto &draft = owner.draft(buffer);
    const auto source = buffer;
    const auto version = draft.revision;
    const bool composing = input->composing();
    const bool selected = input->cursorPosition() != input->selectionAnchor();
    std::vector<std::string> operations;
    std::vector<std::string> corrections;
    std::optional<SpellRange> correctionRange;
    size_t correctionFirst = std::numeric_limits<size_t>::max();
    const auto spellingLanguage = owner.model.settings.spellcheckLanguage;
    const auto spellingTicket = spellTicket;
    const std::weak_ptr<int> sourceIdentity = draft.identity;
    SelectPopupContext::DropdownRequest request;
    const auto add = [&](std::vector<ContextMenuEntry> &entries, std::string label,
                         std::string operation, const char *icon,
                         bool enabled) -> ContextMenuEntry & {
        ContextMenuEntry item;
        item.label = std::move(label);
        item.icon = icon;
        item.enabled = enabled;
        item.value = operations.size();
        operations.push_back(std::move(operation));
        entries.push_back(std::move(item));
        return entries.back();
    };
    add(request.menu, "Undo", "undo", "arrow-back-up",
        !composing && !draft.state.history->undo.empty());
    add(request.menu, "Redo", "redo", "arrow-forward-up",
        !composing && !draft.state.history->redo.empty());
    ContextMenuEntry separator;
    separator.separator = true;
    request.menu.push_back(separator);
    add(request.menu, "Cut", "cut", "cut", selected && !composing);
    add(request.menu, "Copy", "copy", "copy", selected);
    add(request.menu, "Paste", "paste", "clipboard", !composing);
    add(request.menu, "Select all", "select-all", "select-all", !composing);
    request.menu.push_back(separator);
    ContextMenuEntry formats;
    formats.label = "Formatting";
    formats.icon = "typography";
    formats.enabled = !composing;
    const auto &style = formatDocument.styleAt(input->cursorPosition());
    const auto allSelected = [&](const auto &enabledAt) {
        const auto *projection = input->displayProjection();
        const auto start = std::min(input->cursorPosition(), input->selectionAnchor());
        const auto end = std::max(input->cursorPosition(), input->selectionAnchor());
        if (!selected || !projection || start >= projection->rawToDisplay.size() ||
            end >= projection->rawToDisplay.size())
            return enabledAt(style);
        bool any = false;
        for (const auto &run : formatDocument.runs)
            if (run.end > projection->rawToDisplay[start] &&
                run.start < projection->rawToDisplay[end]) {
                any = true;
                if (!enabledAt(run.style))
                    return false;
            }
        return any || enabledAt(style);
    };
    for (const auto &toggle : formatToggles) {
        auto &item = add(formats.children, std::string(toggle.title),
                         "format:" + std::string(toggle.command), "typography", !composing);
        item.checkmark = true;
        item.toggleState =
            allSelected([&](const IrcStyle &value) { return value.*(toggle.active); }) ? 1 : 0;
    }
    add(formats.children, "Reset formatting", "format:reset", "clear-formatting", !composing);
    request.menu.push_back(std::move(formats));
    for (const bool foreground : {true, false}) {
        ContextMenuEntry colors;
        colors.label = foreground ? "Foreground color" : "Background color";
        colors.icon = "palette";
        colors.enabled = !composing;
        for (size_t index = 0; index < ircPalette.size(); ++index) {
            const auto code = (index < 10 ? "0" : "") + std::to_string(index);
            auto &item =
                add(colors.children, "IRC color " + code,
                    std::string(foreground ? "format:foreground:" : "format:background:") + code,
                    "palette", !composing);
            item.indicatorColor = fixedColorSpec(rgbHex(ircPalette[index]));
            item.checkmark = true;
            item.toggleState = allSelected([&](const IrcStyle &value) {
                const auto color = foreground ? value.foregroundSource : value.backgroundSource;
                return color.kind == IrcColor::Kind::Indexed && color.value == index;
            })
                                   ? 1
                                   : 0;
        }
        request.menu.push_back(std::move(colors));
    }
    if (owner.model.settings.spellcheck && !composing) {
        ContextMenuEntry spellingMenu;
        spellingMenu.label = "Spelling";
        spellingMenu.icon = "typography";
        if (!spellError.empty()) {
            add(spellingMenu.children, spellLanguage + ": " + spellError, "", "alert-circle",
                false);
        } else if (spellNeedsCheck || owner.spellInFlight == spellTicket) {
            add(spellingMenu.children, "Checking spelling…", "", "clock", false);
        } else if (const auto *projection = input->displayProjection();
                   projection && spellBuffer == buffer && spellText == projection->text) {
            const auto rawByte = pointer ? input->textByteAtPoint(pointer->localX, pointer->localY)
                                         : std::optional<size_t>{input->cursorPosition()};
            if (rawByte && *rawByte < projection->rawToDisplay.size()) {
                const auto displayByte = projection->rawToDisplay[*rawByte];
                for (const auto range : spellMisspelled)
                    if (displayByte >= range.begin &&
                        (displayByte < range.end || (!pointer && displayByte == range.end))) {
                        correctionRange = rawSpellingRange(range);
                        if (correctionRange) {
                            corrections = owner.spelling.suggest(
                                std::string_view(projection->text)
                                    .substr(range.begin, range.end - range.begin),
                                spellingLanguage);
                            correctionFirst = operations.size();
                            for (const auto &suggestion : corrections)
                                add(spellingMenu.children, suggestion, "", "typography", true);
                            if (corrections.empty())
                                add(spellingMenu.children, "No suggestions available", "",
                                    "info-circle", false);
                        }
                        break;
                    }
            }
        }
        if (!spellingMenu.children.empty())
            request.menu.push_back(std::move(spellingMenu));
    }
    float left, top, right, bottom;
    Node::transformedBounds(input, left, top, right, bottom);
    request.anchorX = int(std::floor(pointer ? pointer->sceneX : left));
    request.anchorY = int(std::floor(pointer ? pointer->sceneY : top));
    request.anchorWidth = pointer ? 1 : std::max(1, int(std::ceil(right - left)));
    request.anchorHeight = pointer ? 1 : std::max(1, int(std::ceil(bottom - top)));
    request.inputSerial = pointer ? std::optional<uint32_t>(pointer->serial) : std::nullopt;
    request.pointerContextMenu = pointer != nullptr;
    request.menuWidth = 240 * themeScale();
    request.maxMenuWidth = 420 * themeScale();
    request.fontSize = 14 * themeScale();
    request.font = themeUiFont();
    request.maxVisibleOptions = 12;
    const std::weak_ptr<int> token = menuAlive;
    const auto generation = ++menuGeneration;
    menuOpen = true;
    popup->openSelectDropdown(
        request,
        {
            .onSelect =
                [this, token, generation, source, version, sourceIdentity, spellingLanguage,
                 spellingTicket, correctionFirst, correctionRange,
                 corrections = std::move(corrections),
                 operations = std::move(operations)](size_t index) {
                    if (token.expired() || generation != menuGeneration)
                        return;
                    menuOpen = false;
                    ++menuGeneration;
                    if (index >= operations.size() || buffer != source || pane.buffer != source ||
                        !owner.model.buffers.contains(source) || !visibleInScene(*this))
                        return;
                    owner.capture(*this);
                    const auto &current = owner.draft(source);
                    if (current.revision != version ||
                        owner.model.buffers.at(source).draft != current.published)
                        return;
                    const auto &operation = operations[index];
                    if (input->composing() && operation != "copy")
                        return;
                    if (index >= correctionFirst && index - correctionFirst < corrections.size()) {
                        if (!owner.model.settings.spellcheck ||
                            owner.model.settings.spellcheckLanguage != spellingLanguage ||
                            spellTicket != spellingTicket || !correctionRange ||
                            sourceIdentity.lock() != current.identity)
                            return;
                        spellingEdit = correctionRange;
                        const bool replaced =
                            input->replaceRange(correctionRange->begin, correctionRange->end,
                                                corrections[index - correctionFirst]);
                        spellingEdit.reset();
                        if (replaced) {
                            pane.focusComposer = true;
                            if (owner.dispatcher)
                                owner.dispatcher->setFocus(input->inputArea());
                        }
                        return;
                    }
                    owner.execute(operation, pane);
                },
            .onDismiss =
                [this, token, generation] {
                    if (!token.expired() && generation == menuGeneration)
                        menuOpen = false;
                },
        });
}

Button *EditorStore::Impl::Composer::button(Flex &parent, std::string_view text,
                                            std::function<void()> fn) {
    auto node = std::make_unique<Button>();
    node->setText(text);
    node->setTabStop(true);
    node->inputArea()->setTabFocusKey(focusKey(text));
    node->inputArea()->setRetainsFocusOnPointerRelease(true);
    node->setVariant(ButtonVariant::Ghost);
    node->setOnClick(std::move(fn));
    auto *result = static_cast<Button *>(parent.addChild(std::move(node)));
    buttons.push_back(result);
    return result;
}
Button *EditorStore::Impl::Composer::iconButton(Flex &parent, std::string_view name,
                                                std::string_view title, std::function<void()> fn) {
    auto *result = button(parent, "", std::move(fn));
    result->setTooltip(title);
    auto icon = std::make_unique<LucideIcon>(name, 18);
    icon->setHitTestVisible(false);
    auto *image = static_cast<LucideIcon *>(result->addChild(std::move(icon)));
    icons.emplace_back(result, image);
    return result;
}
void EditorStore::Impl::Composer::setPaste(std::string text, std::string error) {
    owner.retirePointer(*accessories);
    paste = std::move(text);
    pasteError = std::move(error);
    pasteDestination = buffer;
    pasteLines = std::count(paste.begin(), paste.end(), '\n') + 1;
    pasteMessages = separateMessages(paste);
    pastePreview.clear();
    ui_logic::appendPreview(paste, pastePreview, 180);
    ++pasteVersion;
    const auto count = std::to_string(pasteMessages);
    fullPasteCaption = "Send " + count + " separate messages";
    compactPasteCaption = "Send " + count + " lines";
    separateButton->setText(compactActions ? compactPasteCaption : fullPasteCaption);
    pastePreviewView->setScrollOffset(0);
    markLayoutDirty();
}
bool EditorStore::Impl::Composer::pasteTargetValid() {
    if (pasteDestination != buffer || pane.buffer != buffer ||
        !composerWritable(owner.model, buffer)) {
        pasteError = "The clipboard destination changed or no longer exists; nothing was inserted "
                     "or sent. Cancel and paste again.";
        return false;
    }
    return true;
}
void EditorStore::Impl::Composer::insertPaste() {
    if (!pasteTargetValid())
        return;
    if (owner.insert(pane, paste))
        cancelPaste();
    else
        pasteError = owner.model.status;
}
void EditorStore::Impl::Composer::sendPaste() {
    if (!pasteTargetValid())
        return;
    if (!ui_logic::composerTextValid(paste)) {
        pasteError = "Invalid clipboard text; nothing was sent.";
        return;
    }
    std::string_view remaining = paste;
    while (!remaining.empty()) {
        const auto end = remaining.find('\n');
        const auto line = remaining.substr(0, end);
        if (!line.empty())
            owner.model.actions.push_back({"send", buffer, std::string(line), {}});
        if (end == std::string_view::npos)
            break;
        remaining.remove_prefix(end + 1);
    }
    cancelPaste();
}
void EditorStore::Impl::Composer::cancelPaste(bool restoreFocus) {
    owner.retirePointer(*accessories);
    compositionChord.clear();
    paste.clear();
    pasteError.clear();
    pasteDestination.clear();
    ++pasteVersion;
    if (restoreFocus)
        pane.focusComposer = true;
    markLayoutDirty();
}
void EditorStore::Impl::Composer::toggleFormatting() {
    const bool keyboard =
        owner.dispatcher && owner.dispatcher->focusedArea() == formatButton->inputArea();
    if (formattingOpen && owner.dispatcher && pointerPressedIn(*formatting))
        owner.dispatcher->cancelPointerCapture();
    formattingOpen = !formattingOpen;
    shown(*formatting, formattingOpen);
    formatButton->setSelected(formattingOpen);
    if (keyboard && owner.dispatcher) {
        if (formattingOpen) {
            owner.dispatcher->setFocus(formatButtons.front().first->inputArea());
            formatting->requestScrollIntoView(formatButtons.front().first);
        } else
            owner.dispatcher->setFocus(input->inputArea());
    } else if (!formattingOpen && owner.dispatcher) {
        for (const auto &[button, command] : formatButtons)
            if (owner.dispatcher->focusedArea() == button->inputArea()) {
                owner.dispatcher->setFocus(input->inputArea());
                break;
            }
    }
    updateCompletion(true);
    markLayoutDirty();
}
void EditorStore::Impl::Composer::hideCompletion() {
    literalCompletion = false;
    predictedWord.clear();
    predictionPrefix.clear();
    input->setInlineGhost({});
    if (owner.commandOwner == this) {
        owner.commandOwner = nullptr;
        owner.commandCount = 0;
        if (owner.commandPopup)
            owner.commandPopup->setVisible(false);
    }
    if (owner.emojiOwner != this)
        return;
    owner.emojiOwner = nullptr;
    owner.emojiMatches = {};
    if (owner.emojiPopup)
        owner.emojiPopup->setVisible(false);
}
void EditorStore::Impl::Composer::placeCompletion() {
    if (owner.commandOwner == this && owner.commandPopup && owner.overlayHost) {
        float left, top, right, bottom, x, y, endX, endY;
        Node::transformedBounds(input, left, top, right, bottom);
        Node::mapFromScene(owner.overlayHost, left, top, x, y);
        Node::mapFromScene(owner.overlayHost, right, bottom, endX, endY);
        owner.commandPopup->setAnchor(
            {x, y, std::max(0.f, endX - x), std::max(0.f, endY - y)},
            {0, 0, owner.overlayHost->width(), owner.overlayHost->height()}, scale, labelFont);
    }
    if (owner.emojiOwner != this || !owner.emojiPopup || !owner.overlayHost)
        return;
    float left, top, right, bottom, x, y, endX, endY;
    Node::transformedBounds(input, left, top, right, bottom);
    // Resizing may leave the old anchor outside; mapFromScene still provides local coordinates.
    Node::mapFromScene(owner.overlayHost, left, top, x, y);
    Node::mapFromScene(owner.overlayHost, right, bottom, endX, endY);
    owner.emojiPopup->setAnchor({x, y, std::max(0.f, endX - x), std::max(0.f, endY - y)},
                                {0, 0, owner.overlayHost->width(), owner.overlayHost->height()},
                                scale, labelFont);
}
void EditorStore::Impl::Composer::openLiteralCompletion() {
    owner.capture(*this);
    updateCompletion();
    if (owner.commandOwner == this) {
        if (literalCompletion)
            acceptCommandCompletion(commandSelected);
        return;
    }
    if (acceptPrediction())
        return;
    if (!owner.commandPopup || !emojiWasEligible)
        return;
    const auto *projection = input->displayProjection();
    if (!projection || input->cursorPosition() >= projection->rawToDisplay.size())
        return;
    const std::string_view text = projection->text;
    const size_t caret = projection->rawToDisplay[input->cursorPosition()];
    size_t start = caret;
    while (start &&
           !std::isspace(static_cast<unsigned char>(text[ui_logic::previous(text, start)])))
        start = ui_logic::previous(text, start);
    const auto stop =
        std::upper_bound(projection->rawToDisplay.begin(),
                         projection->rawToDisplay.begin() + input->cursorPosition() + 1, start);
    if (stop == projection->rawToDisplay.begin())
        return;
    hideCompletion();
    commandStart = static_cast<size_t>(stop - projection->rawToDisplay.begin() - 1);
    // Interior UTF-8 bytes share their scalar's display offset. Keep skipped IRC
    // controls outside the token, but never begin replacement inside a scalar.
    while (commandStart && commandStart < input->value().size() &&
           (static_cast<unsigned char>(input->value()[commandStart]) & 0xc0) == 0x80)
        --commandStart;
    commandEnd = input->cursorPosition();
    commandToken = std::string_view(input->value()).substr(commandStart, commandEnd - commandStart);
    commandRevision = revision;
    commandSelected = 0;
    completionCandidates.clear();
    const auto prefix = lower(std::string(text.substr(start, caret - start)));
    auto add = [&](std::string value) {
        if (!value.empty() && lower(value).starts_with(prefix))
            completionCandidates.push_back(std::move(value));
    };
    if (!start && prefix.starts_with("/"))
        for (const auto &command : commandCatalog)
            add("/" + std::string(command.name));
    const auto &context = owner.model.buffers.at(buffer);
    for (const auto &nick : context.nicks)
        add(nick.name);
    for (const auto &[id, target] : owner.model.buffers)
        if (target.server == context.server)
            add(target.target);
    std::sort(completionCandidates.begin(), completionCandidates.end());
    completionCandidates.erase(
        std::unique(completionCandidates.begin(), completionCandidates.end()),
        completionCandidates.end());
    owner.commandCount = std::min(completionCandidates.size(), owner.commandMatches.size());
    if (!owner.commandCount)
        return;
    for (size_t i = 0; i < owner.commandCount; ++i)
        owner.commandMatches[i] = {completionCandidates[i], {}, true};
    literalCompletion = true;
    owner.commandOwner = this;
    owner.commandPopup->setMatches({owner.commandMatches.data(), owner.commandCount}, 0);
    placeCompletion();
    owner.commandPopup->setVisible(true);
}
bool EditorStore::Impl::Composer::updateCommandCompletion(bool eligible) {
    if (!eligible)
        return false;
    const std::string_view raw = input->value();
    if (!raw.starts_with('/') || raw.starts_with("//"))
        return false;
    const auto end = raw.find_first_of(" \t\r\n");
    const size_t tokenEnd = end == raw.npos ? raw.size() : end;
    if (input->cursorPosition() == raw.size()) {
        const auto missing = missingCommandArguments(raw, completionKind, completionChannelTypes);
        if (!missing.empty()) {
            std::string ghost;
            if (!raw.empty() && !std::isspace(static_cast<unsigned char>(raw.back())))
                ghost += ' ';
            ghost += missing;
            input->setInlineGhost(std::move(ghost));
        }
    }
    if (!owner.commandPopup || input->cursorPosition() > tokenEnd || input->cursorPosition() == 0)
        return false;
    const auto token = raw.substr(0, tokenEnd);
    if (token == commandSuppressed)
        return true;
    if (commandToken != token)
        commandSelected = 0;
    commandToken = token;
    commandSuppressed.clear();
    commandStart = 0;
    commandEnd = tokenEnd;
    commandRevision = revision;
    const auto prefix = token.substr(1);
    for (const auto &command : commandCatalog) {
        if (prefix.size() <= command.name.size() &&
            commandEqual(prefix, command.name.substr(0, prefix.size()))) {
            owner.commandMatches[owner.commandCount++] = {command.name, command.description};
            if (owner.commandCount == owner.commandMatches.size())
                break;
        }
    }
    if (!owner.commandCount)
        return true;
    commandSelected = std::min(commandSelected, owner.commandCount - 1);
    owner.commandOwner = this;
    owner.commandPopup->setMatches({owner.commandMatches.data(), owner.commandCount},
                                   commandSelected);
    placeCompletion();
    owner.commandPopup->setVisible(true);
    return true;
}
bool EditorStore::Impl::Composer::acceptCommandCompletion(size_t index) {
    if (owner.commandOwner != this || index >= owner.commandCount || !owner.dispatcher ||
        owner.dispatcher->focusedArea() != input->inputArea() || input->composing() ||
        input->cursorPosition() != input->selectionAnchor() || buffer != pane.buffer ||
        !owner.model.buffers.contains(buffer) || !visibleInScene(*this) || formattingOpen)
        return false;
    owner.capture(*this);
    auto &draft = owner.draft(buffer);
    owner.recover(buffer, draft);
    if (commandRevision != revision || revision != draft.revision ||
        (literalCompletion ? input->cursorPosition() != commandEnd
                           : input->cursorPosition() > commandEnd) ||
        std::string_view(input->value()).substr(commandStart, commandEnd - commandStart) !=
            commandToken) {
        updateCompletion(true);
        return false;
    }
    std::string replacement = literalCompletion
                                  ? std::string(owner.commandMatches[index].name)
                                  : "/" + std::string(owner.commandMatches[index].name);
    if (literalCompletion) {
        const auto *projection = input->displayProjection();
        if (projection && projection->rawToDisplay[commandStart] == 0 &&
            !replacement.starts_with("/") && !replacement.starts_with("#") &&
            !replacement.starts_with("&"))
            replacement += ": ";
    } else if (commandEnd == input->value().size())
        replacement += ' ';
    auto editor = owner.editor(draft);
    editor.replace(commandStart, commandEnd, replacement);
    Input::EditSnapshot candidate{std::move(editor.text), editor.cursor, editor.anchor};
    preserveIrcSuffix(committed(draft.state), candidate, projectionForeground,
                      projectionBackground);
    if (!owner.valid(committed(draft.state).value, candidate.value))
        return false;
    editor.text = std::move(candidate.value);
    editor.cursor = candidate.cursorPos;
    editor.anchor = candidate.selectionAnchor;
    hideCompletion();
    owner.apply(buffer, draft, std::move(editor), pane.id);
    pane.focusComposer = true;
    updateCompletion(true);
    return true;
}
void EditorStore::Impl::Composer::updateCompletion(bool force) {
    const bool eligible =
        owner.overlayHost && owner.emojiPopup && owner.dispatcher &&
        owner.dispatcher->focusedArea() == input->inputArea() && !input->composing() &&
        input->cursorPosition() == input->selectionAnchor() && buffer == pane.buffer &&
        owner.model.buffers.contains(buffer) && !formattingOpen && visibleInScene(*this) &&
        owner.overlayHost->width() > 0 && owner.overlayHost->height() > 0;
    if (literalCompletion && owner.commandOwner == this && eligible &&
        commandRevision == revision && input->cursorPosition() == commandEnd) {
        placeCompletion();
        return;
    }
    const auto context = owner.model.buffers.find(buffer);
    const bool predict = owner.model.settings.wordPrediction;
    if (predict && !owner.words)
        owner.words = std::make_unique<WordCompletion>();
    const bool ready = owner.words && owner.words->ready();
    const auto messages =
        context == owner.model.buffers.end() ? 0 : context->second.messages.size();
    const auto messagesRevision =
        context == owner.model.buffers.end() ? 0 : context->second.messagesRevision;
    const auto network = context == owner.model.buffers.end()
                             ? owner.model.networks.end()
                             : owner.model.networks.find(context->second.server);
    const std::string_view kind = context == owner.model.buffers.end()
                                      ? std::string_view{}
                                      : std::string_view(context->second.kind);
    const std::string_view channelTypes = network == owner.model.networks.end()
                                              ? std::string_view("#&+!")
                                              : std::string_view(network->second.channelTypes);
    const auto foolDisplay =
        context == owner.model.buffers.end()
            ? FoolDisplay::Shown
            : effectiveFoolDisplay(owner.model.settings, buffer, context->second.target);
    const std::string_view server =
        context == owner.model.buffers.end() ? std::string_view{} : context->second.server;
    const std::string_view target =
        context == owner.model.buffers.end() ? std::string_view{} : context->second.target;
    const std::string_view mapping = network == owner.model.networks.end()
                                         ? std::string_view{"rfc1459"}
                                         : network->second.caseMapping;
    const std::string_view own =
        network == owner.model.networks.end() ? std::string_view{} : network->second.nick;
    const bool sourceChanged =
        predictionBuffer != buffer || predictionMessages != messages ||
        predictionMessagesRevision != messagesRevision || predictionServer != server ||
        predictionTarget != target || predictionMapping != mapping || predictionOwn != own ||
        predictionFools != owner.model.settings.fools || predictionFoolDisplay != foolDisplay;
    if (predict && (sourceChanged || !predictionEnabled)) {
        predictionHistory.clear();
        const bool concealFools =
            foolDisplay != FoolDisplay::Shown && foolDisplay != FoolDisplay::Dimmed;
        if (context != owner.model.buffers.end()) {
            size_t remaining = 32;
            for (auto entry = context->second.messages.rbegin();
                 entry != context->second.messages.rend() && remaining; ++entry, --remaining) {
                if (!entry->own || entry->deleted || entry->presence ||
                    (concealFools && isFool(owner.model, server, entry->nick)))
                    continue;
                const std::string_view raw = entry->text;
                auto end = std::min<size_t>(raw.size(), 512);
                while (end && end < raw.size() &&
                       (static_cast<unsigned char>(raw[end]) & 0xc0) == 0x80)
                    --end;
                auto plain = parseIrcFormatting(raw.substr(0, end), projectionForeground,
                                                projectionBackground)
                                 .text;
                if (end < raw.size()) {
                    while (!plain.empty()) {
                        const auto previous = ui_logic::previous(plain, plain.size());
                        if (g_unichar_isspace(g_utf8_get_char(plain.data() + previous)))
                            break;
                        plain.resize(previous);
                    }
                }
                const auto first = plain.find_first_not_of(" \t\r\n");
                if (first == plain.npos ||
                    (plain[first] == '/' &&
                     !std::string_view(plain).substr(first).starts_with("//")))
                    continue;
                predictionHistory.push_back(std::move(plain));
            }
        }
    }
    const bool competing = predict && eligible && hasLiteralCandidate(predictionQuery);
    if (!force && emojiRevision == revision && emojiWasEligible == eligible &&
        completionCaret == input->cursorPosition() &&
        completionAnchor == input->selectionAnchor() && completionKind == kind &&
        completionChannelTypes == channelTypes && predictionEnabled == predict &&
        predictionReady == ready && !sourceChanged && predictionCompeting == competing) {
        if (eligible)
            placeCompletion();
        return;
    }
    predictionEnabled = predict;
    predictionReady = ready;
    predictionBuffer = buffer;
    predictionMessages = messages;
    predictionMessagesRevision = messagesRevision;
    if (predictionFools != owner.model.settings.fools)
        predictionFools = owner.model.settings.fools;
    predictionFoolDisplay = foolDisplay;
    predictionServer = server;
    predictionTarget = target;
    predictionMapping = mapping;
    predictionOwn = own;
    completionCaret = input->cursorPosition();
    completionAnchor = input->selectionAnchor();
    completionKind = kind;
    completionChannelTypes = channelTypes;
    emojiRevision = revision;
    emojiWasEligible = eligible;
    // Rebinding a token cancels an already-held row press, even if its matches are unchanged.
    hideCompletion();
    predictionQuery.clear();
    predictionCompeting = false;
    if (updateCommandCompletion(eligible))
        return;
    if (!eligible)
        return;
    const auto *projection = input->displayProjection();
    if (!projection || input->cursorPosition() >= projection->rawToDisplay.size())
        return;
    const std::string_view text = projection->text;
    const size_t caret = projection->rawToDisplay[input->cursorPosition()];
    const auto queryCharacter = [](unsigned char c) {
        return c < 128 && (std::isalnum(c) || c == '_' || c == '+' || c == '-');
    };
    const auto locateColon = [&](size_t end) -> std::optional<size_t> {
        size_t start = end;
        while (start && queryCharacter(static_cast<unsigned char>(text[start - 1])))
            --start;
        if (!start || text[start - 1] != ':')
            return {};
        const size_t colon = start - 1;
        if (colon) {
            const auto previous = static_cast<unsigned char>(text[colon - 1]);
            if (!std::isspace(previous) && previous != '(' && previous != '[' && previous != '{' &&
                previous != ',' && previous != ';' && previous != '\'' && previous != '"')
                return {};
        }
        return colon;
    };
    size_t queryEnd = caret;
    auto colon = locateColon(queryEnd);
    if (!colon && caret && text[caret - 1] == ':') {
        queryEnd = caret - 1;
        colon = locateColon(queryEnd);
    }
    if (!colon) {
        updatePrediction();
        return;
    }
    const size_t queryStart = *colon + 1;
    while (queryEnd < text.size() && queryCharacter(static_cast<unsigned char>(text[queryEnd])))
        ++queryEnd;
    const size_t tokenEnd = queryEnd + (queryEnd < text.size() && text[queryEnd] == ':' ? 1 : 0);
    if (queryEnd - queryStart > 64 || (queryEnd == queryStart && tokenEnd != queryEnd))
        return;
    const auto startStop =
        std::upper_bound(projection->rawToDisplay.begin(), projection->rawToDisplay.end(), *colon);
    if (startStop == projection->rawToDisplay.begin())
        return;
    const size_t rawStart = static_cast<size_t>(startStop - projection->rawToDisplay.begin() - 1);
    const auto endStop = std::lower_bound(projection->rawToDisplay.begin() + rawStart,
                                          projection->rawToDisplay.end(), tokenEnd);
    if (endStop == projection->rawToDisplay.end() || *endStop != tokenEnd)
        return;
    const size_t rawEnd = static_cast<size_t>(endStop - projection->rawToDisplay.begin());
    const auto token = text.substr(*colon, tokenEnd - *colon);
    if (emojiSuppressedStart == rawStart && emojiSuppressed == token)
        return;
    if (emojiStart != rawStart || emojiEnd != rawEnd || emojiToken != token)
        emojiSelected = 0;
    emojiStart = rawStart;
    emojiEnd = rawEnd;
    if (emojiToken != token)
        emojiToken = token;
    const auto query = text.substr(queryStart, queryEnd - queryStart);
    if (emojiQuery != query)
        emojiQuery = query;
    emojiSuppressed.clear();
    emojiSuppressedStart = std::string::npos;
    owner.emojiMatches = owner.catalog().matches(emojiQuery);
    if (owner.emojiMatches.empty())
        return;
    emojiSelected = std::min(emojiSelected, owner.emojiMatches.size() - 1);
    owner.emojiOwner = this;
    owner.emojiPopup->setMatches(owner.emojiMatches, emojiSelected);
    placeCompletion();
    if (owner.emojiOwner == this)
        owner.emojiPopup->setVisible(true);
}

bool EditorStore::Impl::Composer::hasLiteralCandidate(std::string_view prefix) const {
    const auto source = owner.model.buffers.find(buffer);
    if (prefix.empty() || source == owner.model.buffers.end())
        return false;
    const auto matches = [prefix](std::string_view name) {
        return name.size() >= prefix.size() && commandEqual(prefix, name.substr(0, prefix.size()));
    };
    for (const auto &nick : source->second.nicks)
        if (matches(nick.name))
            return true;
    for (const auto &[id, target] : owner.model.buffers)
        if (target.server == source->second.server && matches(target.target))
            return true;
    return false;
}

void EditorStore::Impl::Composer::updatePrediction() {
    if (!owner.model.settings.wordPrediction || !emojiWasEligible || menuOpen ||
        owner.commandOwner == this || owner.emojiOwner == this ||
        input->cursorPosition() != input->value().size())
        return;
    const auto *projection = input->displayProjection();
    if (!projection || projection->text.empty() || projection->text.starts_with('/'))
        return;
    const auto &text = projection->text;
    size_t start = text.size(), letters = 0;
    while (start) {
        const auto previous = ui_logic::previous(text, start);
        const auto character = g_utf8_get_char(text.data() + previous);
        if (!g_unichar_isalpha(character) && !g_unichar_ismark(character))
            break;
        letters += g_unichar_isalpha(character) ? 1 : 0;
        start = previous;
    }
    const auto prefix = std::string_view(text).substr(start);
    if (!letters || prefix.size() > 128)
        return;
    if (start) {
        const auto previous = g_utf8_get_char(text.data() + ui_logic::previous(text, start));
        if (g_unichar_isalnum(previous) ||
            (previous < 128 &&
             std::string_view("#&+!/:@`_-").find(char(previous)) != std::string_view::npos))
            return;
    }
    for (const auto &run : formatDocument.runs)
        if (run.end > start && run.style.monospace)
            return;
    const auto &context = owner.model.buffers.at(buffer);
    if (context.kind != "channel" && context.kind != "query")
        return;
    predictionQuery = prefix;
    predictionCompeting = hasLiteralCandidate(prefix);
    if (predictionCompeting)
        return;
    if (owner.words) {
        owner.words->setHistory(predictionHistory);
        predictedWord = owner.words->complete(std::string_view(text).substr(0, start), prefix);
    }
    if (predictedWord.empty())
        return;
    predictionPrefix = prefix;
    predictionCaret = input->cursorPosition();
    input->setInlineGhost(predictedWord.substr(prefix.size()));
}

bool EditorStore::Impl::Composer::acceptPrediction() {
    if (predictedWord.empty())
        return false;
    const auto word = predictedWord, prefix = predictionPrefix;
    const auto caret = predictionCaret;
    updateCompletion(true);
    if (predictedWord != word || predictionPrefix != prefix || predictionCaret != caret ||
        input->cursorPosition() != caret || input->selectionAnchor() != caret ||
        !input->editingStateMatches(owner.draft(buffer).state))
        return false;
    const auto suffix = word.substr(prefix.size());
    spellingCycle.reset();
    hideCompletion();
    return input->replaceRange(caret, caret, suffix);
}
bool EditorStore::Impl::Composer::acceptCompletion(size_t index) {
    if (owner.emojiOwner != this || index >= owner.emojiMatches.size() || !owner.dispatcher ||
        owner.dispatcher->focusedArea() != input->inputArea() || input->composing() ||
        input->cursorPosition() != input->selectionAnchor() || buffer != pane.buffer ||
        !owner.model.buffers.contains(buffer) || !visibleInScene(*this) || formattingOpen)
        return false;
    owner.capture(*this);
    auto &draft = owner.draft(buffer);
    owner.recover(buffer, draft);
    if (emojiRevision != revision || revision != draft.revision) {
        updateCompletion(true);
        return false;
    }
    const auto glyph = owner.emojiMatches[index].glyph;
    auto editor = owner.editor(draft);
    editor.replace(emojiStart, emojiEnd, glyph);
    Input::EditSnapshot candidate{std::move(editor.text), editor.cursor, editor.anchor};
    preserveIrcSuffix(committed(draft.state), candidate, projectionForeground,
                      projectionBackground);
    if (!owner.valid(committed(draft.state).value, candidate.value))
        return false;
    editor.text = std::move(candidate.value);
    editor.cursor = candidate.cursorPos;
    editor.anchor = candidate.selectionAnchor;
    hideCompletion();
    owner.apply(buffer, draft, std::move(editor), pane.id);
    pane.focusComposer = true;
    updateCompletion(true);
    return true;
}
std::string EditorStore::Impl::Composer::help() const {
    std::string result = inputDestination;
    result += "\n" + commandLabel(owner.model, "Send", "send");
    result += "\n" + commandLabel(owner.model, "New line", "newline");
    result += "\n" + commandLabel(owner.model, "Complete nickname or command", "complete");
    result += "\n" + commandLabel(owner.model, "Switch conversation", "switch-buffer");
    result += "\n" + commandLabel(owner.model, "Open command palette", "execute-command");
    result +=
        "\n" + commandLabel(owner.model, "Cancel composition, paste, selection or reply", "cancel");
    result += "\nLeave the editor: Shift+Tab to native controls (unless remapped). "
              "Tab traverses controls; completion keeps its configured binding in the editor.";
    return result;
}
EditorStore::Impl::Composer::Composer(Impl &impl, PaneState &state)
    : owner(impl), pane(state), buffer(state.buffer) {
    setDirection(FlexDirection::Vertical);
    setAlign(FlexAlign::Stretch);
    setFillWidth(true);
    setClipChildren(true);
    auto reply = std::make_unique<ReplyCard>();
    reply->setDirection(FlexDirection::Horizontal);
    reply->setFillWidth(true);
    replyRow = reply.get();
    reply->setAlign(FlexAlign::Stretch);
    reply->setClipChildren(true);
    auto rail = std::make_unique<Flex>();
    replyRail = static_cast<Flex *>(reply->addChild(std::move(rail)));
    auto replyContext = std::make_unique<Flex>();
    replyContext->setDirection(FlexDirection::Vertical);
    replyContext->setAlign(FlexAlign::Stretch);
    replyContext->setFlexGrow(1);
    replyContext->setClipChildren(true);
    replyText = replyContext.get();
    auto heading = std::make_unique<Label>();
    heading->setMaxLines(1);
    heading->setUseMarkup(true);
    replyHeading = static_cast<Label *>(replyContext->addChild(std::move(heading)));
    auto label = std::make_unique<Label>();
    label->setMaxLines(1);
    label->setUseMarkup(true);
    replyLabel = static_cast<Label *>(replyContext->addChild(std::move(label)));
    reply->addChild(std::move(replyContext));
    replyCancelButton = iconButton(*reply, "x", "Cancel reply", [this] {
        pane.reply.clear();
        pane.focusComposer = true;
    });
    replyCancelButton->inputArea()->setTabFocusKey(focusKey("reply-cancel"));
    replyCancelButton->inputArea()->setFocusOnPointerPress(false);
    addChild(std::move(reply));
    replyRow->setParticipatesInLayout(false);
    auto field = std::make_unique<Input>();
    input = field.get();
    input->inputArea()->setTabFocusKey(focusKey("input"));
    input->setOnContextMenu([this](const auto *pointer) { contextMenu(pointer); });
    input->setMultiline(true);
    input->setSubmitOnEnter(true);
    // One rounded composer surface contains native editing and its action controls.
    input->setFrameVisible(false);
    input->setClearButtonEnabled(false);
    projectionForeground = composerForeground(owner.model);
    projectionBackground = themeSurfaceColor("surface_container_high");
    input->setDisplayProjector([this](std::string_view raw) { return project(raw); });
    input->setEditRewriter([this](const Input::EditSnapshot &before, Input::EditSnapshot &after,
                                  bool) {
        preserveIrcSuffix(before, after, projectionForeground, projectionBackground, spellingEdit);
    });
    input->setEditValidator([this](std::string_view before, std::string_view after, bool) {
        return owner.valid(before, after);
    });
    input->setOnEditingStateChanged([this] {
        const auto previousRevision = revision;
        owner.capture(*this);
        if (revision != previousRevision)
            compositionChord.clear();
        updateUpload();
        updateCompletion();
    });
    input->setOnSubmit([this](const std::string &) { owner.execute("send", pane); });
    input->setOnFocusGain([this] {
        pane.focusComposer = false;
        owner.model.activeBuffer = pane.buffer;
        owner.focus(pane.id);
        updateCompletion(true);
    });
    input->setOnFocusLoss([this] {
        owner.capture(*this);
        spellingCycle.reset();
        input->resetCaretEffects();
        compositionChord.clear();
        hideCompletion();
    });
    input->setOnKeyEvent([this](std::uint32_t sym, std::uint32_t modifiers) {
        const bool ctrl = modifiers & KeyMod::Ctrl, shift = modifiers & KeyMod::Shift;
        const bool pasteShortcut =
            (ctrl && (sym == 'v' || sym == 'V')) || (shift && KeySymbol::isInsert(sym));
        // Full-event composition shortcuts are handled before native focus traversal.
        if (input->composing())
            return false;
        // Configured Ui chords win first; these fallback edits must also update
        // the shared Emacs kill/yank state, not just native text or clipboard.
        if (!input->composing()) {
            std::string_view edit;
            if (modifiers == KeyMod::Ctrl && (sym == 'h' || sym == 'H'))
                edit = "backward-delete";
            else if (ctrl && KeySymbol::isBackspace(sym))
                edit = "backward-kill-word";
            else if (ctrl && KeySymbol::isDelete(sym))
                edit = "kill-word";
            else if (modifiers == KeyMod::Ctrl && (sym == 'c' || sym == 'C'))
                edit = "copy-region";
            else if (modifiers == KeyMod::Ctrl && (sym == 'x' || sym == 'X'))
                edit = "kill-region";
            if (!edit.empty()) {
                owner.execute(edit, pane);
                return true;
            }
        }
        if (pasteShortcut) {
            owner.paste(*this);
            return true;
        }
        if (ctrl && KeySymbol::isInsert(sym)) {
            owner.execute("copy-region", pane);
            return true;
        }
        if (shift && KeySymbol::isDelete(sym)) {
            owner.execute("kill-region", pane);
            return true;
        }
        return false;
    });
    auto panel = std::make_unique<ScrollView>();
    formatting = panel.get();
    panel->setFillWidth(true);
    panel->setReserveScrollbarGutter(true);
    panel->setViewportPaddingH(4 * themeScale());
    panel->setViewportPaddingV(4 * themeScale());
    auto *formatContent = panel->content();
    formatContent->setDirection(FlexDirection::Vertical);
    formatContent->setAlign(FlexAlign::Stretch);
    const auto formatAction = [this](Flex &row, std::string_view title, std::string command) {
        auto *control = button(row, title, [this, command] {
            if (buffer != pane.buffer || input->composing() || !formattingOpen ||
                !owner.model.buffers.contains(buffer) || !visibleInScene(*formatting))
                return;
            owner.execute(command, pane);
        });
        control->setTooltip(title);
        control->inputArea()->setTabFocusKey(focusKey(command));
        control->inputArea()->setFocusOnPointerPress(false);
        formatButtons.emplace_back(control, std::move(command));
        return control;
    };
    auto decorations = row();
    decorations->setWrap(true);
    formatRows.push_back(decorations.get());
    for (const auto &toggle : formatToggles) {
        auto *control =
            formatAction(*decorations, toggle.title, "format:" + std::string(toggle.command));
        control->label()->setUseMarkup(true);
        control->label()->setText(toggle.markup);
    }
    formatAction(*decorations, "Reset formatting", "format:reset");
    formatContent->addChild(std::move(decorations));
    for (const bool foreground : {true, false}) {
        auto title = std::make_unique<Label>();
        title->setText(foreground ? "Foreground" : "Background");
        formatLabels.push_back(static_cast<Label *>(formatContent->addChild(std::move(title))));
        auto samples = row();
        samples->setWrap(true);
        formatRows.push_back(samples.get());
        for (size_t index = 0; index < ircPalette.size(); ++index) {
            const auto code = (index < 10 ? "0" : "") + std::to_string(index);
            const auto title =
                std::string(foreground ? "Foreground IRC color " : "Background IRC color ") + code;
            auto *sample = formatAction(
                *samples, title,
                std::string(foreground ? "format:foreground:" : "format:background:") + code);
            sample->setText(code);
            const Color color = rgbHex(ircPalette[index]);
            auto colors = Button::defaultPalette(ButtonVariant::Ghost);
            for (auto *state : {&colors.normal, &colors.hover, &colors.pressed, &colors.disabled}) {
                state->bg = fixedColorSpec(color);
                state->label = fixedColorSpec(readableTextColorForBackground(color));
                state->border = colorSpecFromRole(ColorRole::OutlineVariant);
            }
            colors.borderWidth = 2;
            colors.selected = colors.normal;
            colors.selected->border = colorSpecFromRole(ColorRole::Primary);
            sample->setCustomPalette(colors);
        }
        formatContent->addChild(std::move(samples));
    }
    keepPointerFocus(*panel);
    shown(*panel, false);
    addChild(std::move(panel));
    auto toolbar = row();
    tools = toolbar.get();
    toolbar->setAlign(FlexAlign::Start);
    input->setFlexGrow(1);
    toolbar->addChild(std::move(field));
    uploadButton = iconButton(*toolbar, "paperclip", "Upload file", [this] {
        if (!input->value().empty() || input->composing() || buffer != pane.buffer ||
            !owner.model.buffers.contains(buffer) || !owner.onUploadRequested)
            return;
        owner.onUploadRequested(pane.id, buffer, *uploadButton, std::nullopt);
    });
    uploadButton->setParticipatesInLayout(false);
    uploadButton->inputArea()->setTabFocusKey(focusKey("upload"));
    uploadButton->inputArea()->setFocusOnPointerPress(false);
    uploadButton->inputArea()->setOnClick([this](const InputArea::PointerData &pointer) {
        if (!input->value().empty() || input->composing() || buffer != pane.buffer ||
            !owner.model.buffers.contains(buffer) || !owner.onUploadRequested)
            return;
        owner.onUploadRequested(pane.id, buffer, *uploadButton, pointer);
    });
    formatButton =
        iconButton(*toolbar, "plus", "Formatting toolbar", [this] { toggleFormatting(); });
    formatButton->inputArea()->setFocusOnPointerPress(false);
    formatButton->inputArea()->setTabFocusKey(focusKey("plus"));
    sendButton =
        iconButton(*toolbar, "send", "Send message", [this] { owner.execute("send", pane); });
    sendButton->inputArea()->setTabFocusKey(focusKey("send"));
    addChild(std::move(toolbar));
    input->inputArea()->setTooltipProvider([this]() -> TooltipContent { return help(); });
    sendButton->inputArea()->setTooltipProvider([this]() -> TooltipContent {
        return commandLabel(owner.model, "Send message", "send") + "\n" + inputDestination;
    });
    auto confirmation = std::make_unique<Flex>();
    accessories = confirmation.get();
    confirmation->setDirection(FlexDirection::Vertical);
    confirmation->setAlign(FlexAlign::Stretch);
    confirmation->setFillWidth(true);
    confirmation->setGap(2);
    auto context = std::make_unique<Label>();
    context->setUseMarkup(false);
    context->setFocusOnPointerPress(false);
    context->setTooltipProvider([this]() { return pasteLabel->tooltipContent(); });
    pasteContext = static_cast<Label *>(confirmation->addChild(std::move(context)));
    auto actionRow = std::make_unique<Flex>();
    pasteActions = actionRow.get();
    actionRow->setDirection(FlexDirection::Horizontal);
    actionRow->setWrap(true);
    actionRow->setFillWidth(true);
    actionRow->setGap(2);
    pasteButtons[0] = button(*actionRow, "Insert into draft", [this] { insertPaste(); });
    pasteButtons[0]->inputArea()->setTabFocusKey(focusKey("paste-insert"));
    separateButton = button(*actionRow, "", [this] { sendPaste(); });
    pasteButtons[1] = separateButton;
    separateButton->inputArea()->setTabFocusKey(focusKey("paste-send"));
    pasteButtons[2] = button(*actionRow, "Cancel paste", [this] { cancelPaste(); });
    pasteButtons[2]->inputArea()->setTabFocusKey(focusKey("paste-cancel"));
    for (auto *control : pasteButtons)
        control->inputArea()->setFocusOnPointerPress(false);
    confirmation->addChild(std::move(actionRow));
    auto preview = std::make_unique<ScrollView>();
    pastePreviewView = preview.get();
    preview->setFillWidth(true);
    preview->setViewportPaddingH(0);
    preview->setViewportPaddingV(0);
    preview->setReserveScrollbarGutter(true);
    preview->content()->setDirection(FlexDirection::Vertical);
    preview->content()->setAlign(FlexAlign::Stretch);
    auto explanation = std::make_unique<Label>();
    explanation->setUseMarkup(false);
    explanation->setMaxLines(0);
    pasteLabel = static_cast<Label *>(preview->content()->addChild(std::move(explanation)));
    confirmation->addChild(std::move(preview));
    shown(*accessories, false);
    addChild(std::move(confirmation));
    owner.views.push_back(this);
    owner.refresh(buffer);
    sync();
}
EditorStore::Impl::Composer::~Composer() {
    if (replyRow->parent() != this) {
        owner.retirePointer(*replyRow);
        replyRow->parent()->removeChild(replyRow);
    }
    if (owner.spellInFlight == spellTicket)
        owner.spellInFlight = 0;
    menuAlive.reset();
    owner.retirePointer(*uploadButton);
    if (menuOpen && input->popupContext())
        input->popupContext()->closeSelectDropdown();
    hideCompletion();
    owner.capture(*this);
    input->setOnEditingStateChanged({});
    input->setOnFocusLoss({});
    std::erase(owner.views, this);
}
void EditorStore::Impl::Composer::measureActions(Renderer &renderer) {
    const float renderScale = renderer.renderScale();
    if (!actionMetricsDirty && actionRenderScale == renderScale &&
        actionMessageCount == pasteMessages)
        return;
    float strut = 0;
    const std::array controls{pasteButtons[0], pasteButtons[1], pasteButtons[2], replyCancelButton};
    for (const auto *control : controls) {
        const auto metrics =
            renderer.measureText(control->label()->text(), 14 * scale, labelFont, 0, 1);
        strut = std::max(strut, metrics.layoutHeight);
    }
    const float actionHeight = std::max(32.f, std::ceil(strut) + 4);
    for (auto *control : controls) {
        control->setControlHeight(actionHeight);
        control->setPadding(2.f, compactActions ? 4.f : 6 * scale);
    }
    actionMetricsDirty = false;
    actionRenderScale = renderScale;
    actionMessageCount = pasteMessages;
}
float EditorStore::Impl::Composer::measurePreview(Renderer &renderer,
                                                  const LayoutConstraints &width) {
    if (compactActions || paste.empty()) {
        pastePreviewView->setSize(width.maxWidth, 0);
        shown(*pastePreviewView, false);
        return 0;
    }
    pastePreviewView->setSize(width.maxWidth, pastePreviewView->height());
    LayoutConstraints contentWidth;
    contentWidth.setExactWidth(std::max(1.f, pastePreviewView->contentViewportWidth(true)));
    const float natural = pastePreviewView->content()->measure(renderer, contentWidth).height +
                          2 * pastePreviewView->viewportPaddingV();
    const float height = std::min(160 * scale, std::max(32.f, natural));
    pastePreviewView->setSize(width.maxWidth, height);
    shown(*pastePreviewView, true);
    return height;
}
float EditorStore::Impl::Composer::measureMinimum(Renderer &renderer, float innerWidth,
                                                  float maxComposerHeight) {
    LayoutConstraints width;
    width.setExactWidth(std::max(1.f, innerWidth));
    const float target = std::max(32.f, 32 * scale);
    bool roomyFitsWidth = true;
    const auto measure = [&](bool compact) {
        if (compactActions != compact) {
            compactActions = compact;
            actionMetricsDirty = true;
        }
        setGap(compact ? 2.f : 4 * scale);
        tools->setGap(compact ? 2.f : 4 * scale);
        tools->setPadding(compact ? 0.f : 4 * scale);
        formatting->setViewportPaddingV(4 * scale);
        accessories->setGap(2.f);
        pasteButtons[0]->setText(compact ? "Insert" : "Insert into draft");
        separateButton->setText(compact ? compactPasteCaption : fullPasteCaption);
        pasteButtons[2]->setText(compact ? "Cancel" : "Cancel paste");
        pasteContext->setMaxLines(compact ? 1 : 0);
        measureActions(renderer);
        for (auto *control : pasteButtons) {
            control->setMinWidth(32);
            control->setMaxWidth(0);
            if (!compact && !paste.empty() &&
                control->measure(renderer, LayoutConstraints::unconstrained()).width >
                    width.maxWidth)
                roomyFitsWidth = false;
            if (compact)
                control->setMaxWidth(width.maxWidth);
        }
        const float lineHeight = renderer.measureText(" ", messageSize, font).layoutHeight;
        const float nativeRow = std::max(target, lineHeight + 12);
        float height = tools->paddingTop() + tools->paddingBottom();
        const float previewHeight = measurePreview(renderer, width);
        if (!paste.empty()) {
            height += pasteContext->Node::measure(renderer, width).height + accessories->gap() +
                      pasteActions->measure(renderer, width).height + gap();
            if (previewHeight > 0)
                height += accessories->gap() + previewHeight;
        }
        if (formattingOpen)
            height += target + 2 * (formatting->viewportPaddingV() + std::max(1.f, scale)) + gap();
        float inputHeight = nativeRow;
        if (paste.empty()) {
            LayoutConstraints primaryWidth = width;
            primaryWidth.setExactWidth(std::max(width.maxWidth, primaryMinimumWidth(scale)));
            tools->measure(renderer, primaryWidth);
            const float preferredInput = std::clamp(input->measuredMultilineHeight(), nativeRow,
                                                    std::max(nativeRow, 5 * lineHeight + 12));
            const float protectedInput =
                formattingOpen ? std::min(preferredInput, std::max(nativeRow, input->height()))
                               : nativeRow;
            if (formattingOpen) {
                const float reclaimed =
                    std::min(2 * formatting->viewportPaddingV(),
                             std::max(0.f, height + preferredInput - maxComposerHeight));
                formatting->setViewportPaddingV(formatting->viewportPaddingV() - reclaimed / 2);
                height -= reclaimed;
            }
            const float maximum =
                std::max(protectedInput, std::min(5 * lineHeight + 12, maxComposerHeight - height));
            inputHeight = std::clamp(preferredInput, protectedInput, maximum);
        }
        return height + inputHeight;
    };
    const float roomy = measure(false);
    const bool canCompact = !paste.empty();
    return canCompact && (!roomyFitsWidth || roomy > maxComposerHeight) ? measure(true) : roomy;
}
void EditorStore::Impl::Composer::fit(Renderer &renderer, const LayoutConstraints &constraints) {
    LayoutConstraints width;
    width.setExactWidth(constraints.hasMaxWidth ? constraints.maxWidth
                                                : std::max(this->width(), 360 * scale));
    measureActions(renderer);
    for (auto *control : pasteButtons)
        control->setMaxWidth(compactActions ? width.maxWidth : 0);
    float budget = heightLimit > 0 ? heightLimit : 200 * scale;
    if (constraints.hasMaxHeight)
        budget = std::min(budget, constraints.maxHeight);
    const float target = std::max(32.f, 32 * scale);
    const float padding = tools->paddingTop() + tools->paddingBottom();
    const float lineHeight = renderer.measureText(" ", messageSize, font).layoutHeight;
    const float minimum = std::max(target, lineHeight + 12);
    const float pasteActionsHeight =
        !paste.empty() ? pasteActions->measure(renderer, width).height : 0;
    const float previewHeight = measurePreview(renderer, width);
    const float pasteHeight = !paste.empty()
                                  ? pasteContext->Node::measure(renderer, width).height +
                                        accessories->gap() + pasteActionsHeight +
                                        (previewHeight > 0 ? accessories->gap() + previewHeight : 0)
                                  : 0;
    const float accessoryMinimum = !paste.empty() ? pasteHeight + gap() : 0;
    const float panelMinimum =
        formattingOpen ? target + 2 * (formatting->viewportPaddingV() + std::max(1.f, scale)) : 0;
    const float panelGap = formattingOpen ? gap() : 0;
    // The native Input shapes at the actual flex-assigned width; no second text layout.
    LayoutConstraints primaryWidth = width;
    primaryWidth.setExactWidth(std::max(width.maxWidth, primaryMinimumWidth(scale)));
    tools->measure(renderer, primaryWidth);
    const float maximum =
        std::max(minimum, std::min(5 * lineHeight + 12,
                                   budget - padding - accessoryMinimum - panelMinimum - panelGap));
    const float inputHeight = std::clamp(input->measuredMultilineHeight(), minimum, maximum);
    input->setSize(input->width(), inputHeight);
    if (formattingOpen) {
        formatting->measure(renderer, width);
        LayoutConstraints contentWidth;
        contentWidth.setExactWidth(std::max(1.f, formatting->contentViewportWidth(true)));
        for (const auto &[button, command] : formatButtons)
            if (!command.starts_with("format:foreground:") &&
                !command.starts_with("format:background:"))
                button->setMaxWidth(std::max(target, contentWidth.maxWidth));
        const float natural = formatting->content()->measure(renderer, contentWidth).height +
                              2 * (formatting->viewportPaddingV() + std::max(1.f, scale));
        // Expanding grows upward; it never steals the input's existing native row.
        const float panelHeight =
            std::clamp(budget - padding - inputHeight - accessoryMinimum - gap(), panelMinimum,
                       std::max(panelMinimum, std::min(natural, 160 * scale)));
        formatting->setSize(0, panelHeight);
    }
    shown(*accessories, !paste.empty());
    if (!paste.empty())
        accessories->setSize(0, pasteHeight);
}
LayoutSize EditorStore::Impl::Composer::doMeasure(Renderer &renderer,
                                                  const LayoutConstraints &constraints) {
    fit(renderer, constraints);
    return Flex::doMeasure(renderer, constraints);
}
void EditorStore::Impl::Composer::doArrange(Renderer &renderer, const LayoutRect &rect) {
    fit(renderer, LayoutConstraints::exact(rect.width, rect.height));
    Flex::doArrange(renderer, rect);
    const float minimumWidth = primaryMinimumWidth(scale);
    const bool constrained = width() < minimumWidth;
    setClipChildren(false);
    if (constrained) {
        // An impossible split keeps its full native controls, clipped by this viewport.
        // Never let a zero grow share make Input overlap the fixed action targets.
        tools->arrange(renderer, {tools->x(), tools->y(), minimumWidth, tools->height()});
        tools->setClipChildren(true);
    } else
        tools->setClipChildren(false);
    if (replyRow->parent() == this) {
        LayoutConstraints contextWidth;
        contextWidth.setExactWidth(std::max(1.f, width()));
        const float contextHeight = replyRow->measure(renderer, contextWidth).height;
        replyRow->arrange(renderer, {0, -contextHeight - gap(), width(), contextHeight});
    }
    if (uploadButton->visible()) {
        const float target = std::max(32.f, 32 * scale);
        uploadButton->arrange(renderer, {input->x() + std::max(0.f, input->width() - target),
                                         input->y(), std::min(target, input->width()), target});
    }
    float x = 0, y = 0;
    Node::absolutePosition(input, x, y);
    if (arrangedInput.width > 0 && (arrangedInput.x != x || arrangedInput.y != y))
        input->resetCaretEffects();
    arrangedInput = {x, y, input->width(), input->height()};
    Button *focused = nullptr;
    if (formattingOpen && owner.dispatcher)
        for (const auto &[button, command] : formatButtons)
            if (owner.dispatcher->focusedArea() == button->inputArea()) {
                focused = button;
                break;
            }
    if (!focused)
        formatFocusLayoutValid = false;
    else {
        float sceneX, sceneY, endSceneX, endSceneY, localX, localY, endX, endY;
        Node::mapToScene(focused, 0, 0, sceneX, sceneY);
        Node::mapToScene(focused, focused->width(), focused->height(), endSceneX, endSceneY);
        // The far corner can be on the content edge; containment is not conversion success.
        Node::mapFromScene(formatting->content(), sceneX, sceneY, localX, localY);
        Node::mapFromScene(formatting->content(), endSceneX, endSceneY, endX, endY);
        const LayoutRect bounds{localX, localY, endX - localX, endY - localY};
        const LayoutSize viewport{formatting->contentViewportWidth(true),
                                  formatting->contentViewportHeight()};
        const auto near = [](float a, float b) { return std::abs(a - b) < .02f; };
        const bool changed = focused != formatFocused || !formatFocusLayoutValid ||
                             !near(bounds.x, formatFocusBounds.x) ||
                             !near(bounds.y, formatFocusBounds.y) ||
                             !near(bounds.width, formatFocusBounds.width) ||
                             !near(bounds.height, formatFocusBounds.height) ||
                             !near(viewport.width, formatFocusViewport.width) ||
                             !near(viewport.height, formatFocusViewport.height);
        formatFocusBounds = bounds;
        formatFocusViewport = viewport;
        formatFocusLayoutValid = true;
        const float offset = formatting->scrollOffset();
        if (changed && (bounds.y < offset - .02f ||
                        bounds.y + bounds.height > offset + viewport.height + .02f)) {
            formatting->requestScrollIntoView(focused);
            formatting->layout(renderer);
        }
    }
    formatFocused = focused;
    if (owner.commandOwner == this) {
        placeCompletion();
        owner.commandPopup->layout(renderer);
    }
    if (owner.emojiOwner == this) {
        const auto width = owner.emojiPopup->width(), height = owner.emojiPopup->height();
        placeCompletion();
        if (owner.emojiPopup &&
            (owner.emojiPopup->width() != width || owner.emojiPopup->height() != height))
            owner.emojiPopup->layout(renderer);
    }
}
void EditorStore::Impl::Composer::doLayout(Renderer &renderer) {
    if (sizeAssignedByLayout()) {
        // A parent Node may call layout again after arrange; keep its real viewport constraints.
        arrange(renderer, {x(), y(), width(), height()});
        return;
    }
    LayoutConstraints constraints;
    if (width() > 0)
        constraints.setExactWidth(width());
    fit(renderer, constraints);
    Flex::doLayout(renderer);
}
void EditorStore::Impl::Composer::sync() {
    if (!input->composing() || !visibleInScene(*this))
        compositionChord.clear();
    if (owner.dispatcher && ((!visibleInScene(*this) && pointerPressedIn(*this)) ||
                             (!visibleInScene(*formatting) && pointerPressedIn(*formatting))))
        owner.dispatcher->cancelPointerCapture();
    if (!visibleInScene(*this) || !formattingOpen)
        formatFocusLayoutValid = false;
    if (menuOpen && (buffer != pane.buffer || !visibleInScene(*this))) {
        ++menuGeneration;
        menuOpen = false;
        if (input->popupContext())
            input->popupContext()->closeSelectDropdown();
    }
    if (buffer != pane.buffer) {
        owner.retirePointer(*uploadButton);
        compositionChord.clear();
        if (owner.dispatcher && pointerPressedIn(*this))
            owner.dispatcher->cancelPointerCapture();
        owner.capture(*this);
        input->resetCaretEffects();
        hideCompletion();
        emojiSuppressed.clear();
        commandSuppressed.clear();
        commandToken.clear();
        commandSelected = 0;
        emojiSuppressedStart = std::string::npos;
        emojiRevision = 0;
        formattingOpen = false;
        shown(*formatting, false);
        formatButton->setSelected(false);
        buffer = pane.buffer;
        revision = 0;
        cancelPaste(false);
        pendingPaste.reset();
        owner.refresh(buffer);
    }
    auto &d = owner.draft(buffer);
    owner.recover(buffer, d);
    updateUpload();
    input->setEnabled(composerWritable(owner.model, buffer));
    destinationScratch = "Message";
    if (const auto b = owner.model.buffers.find(buffer); b != owner.model.buffers.end()) {
        const auto name = serverDisplayName(owner.model, b->second.server);
        if (b->second.kind == "server") {
            destinationScratch = "Command";
            if (!name.empty()) {
                destinationScratch += " on ";
                ui_logic::appendPreview(name, destinationScratch, 256);
            }
        } else {
            if (!b->second.target.empty()) {
                destinationScratch += " ";
                ui_logic::appendPreview(b->second.target, destinationScratch, 128);
            }
            if (!name.empty()) {
                destinationScratch += " · ";
                ui_logic::appendPreview(name, destinationScratch, 256);
            }
        }
    }
    if (inputDestination != destinationScratch) {
        inputDestination.swap(destinationScratch);
        input->setPlaceholder(inputDestination);
    }
    const float nextScale = themeScale();
    const auto &nextFont = themeMessageFont();
    const auto activeBuffer = owner.model.buffers.find(buffer);
    const float nextSize = activeBuffer == owner.model.buffers.end()
                               ? messageTextSize(owner.model.settings, Buffer{})
                               : messageTextSize(owner.model.settings, activeBuffer->second);
    if (scale != nextScale || messageSize != nextSize || font != nextFont ||
        labelFont != themeUiFont()) {
        scale = nextScale;
        font = nextFont;
        messageSize = nextSize;
        labelFont = themeUiFont();
        actionMetricsDirty = true;
        const float target = std::max(32.f, 32 * scale);
        setGap(4 * scale);
        input->setControlHeight(target);
        input->setMinLayoutWidth(inputMinimumWidth(scale));
        input->setFontSize(messageSize);
        input->setFontFamily(font.family);
        input->setFontFeatures(font.features);
        input->setFontWeight(font.weight);
        input->setFrameRadius(themeCornerRadius());
        input->setHorizontalPadding(8 * scale);
        tools->setGap(4 * scale);
        tools->setPadding(4 * scale);
        tools->setRadius(themeCornerRadius());
        accessories->setGap(2);
        pastePreviewView->setContentScale(scale);
        formatting->setContentScale(scale);
        formatting->setViewportPaddingH(4 * scale);
        formatting->setViewportPaddingV(4 * scale);
        formatting->content()->setGap(4 * scale);
        formatting->setRadius(themeCornerRadius());
        for (auto *row : formatRows)
            row->setGap(4 * scale);
        for (auto *label : formatLabels) {
            label->setFontSize(14 * scale);
            label->setFontFamily(labelFont.family);
            label->setFontFeatures(labelFont.features);
            label->setFontWeight(labelFont.weight);
        }
        replyRow->setGap(4 * scale);
        replyRow->setPadding(6 * scale, 8 * scale);
        replyRow->setRadius(themeCornerRadius());
        replyText->setGap(2 * scale);
        replyRail->setMinWidth(2 * scale);
        replyRail->setMaxWidth(2 * scale);
        replyRail->setRadius(scale);
        for (auto *b : buttons) {
            b->setControlHeight(target);
            b->setRadius(themeCornerRadius());
            b->setFontSize(14 * scale);
            if (auto *label = b->label()) {
                label->setFontFamily(labelFont.family);
                label->setFontFeatures(labelFont.features);
                label->setFontWeight(labelFont.weight);
            }
        }
        for (auto *label : {replyHeading, replyLabel, pasteLabel, pasteContext}) {
            label->setFontSize(14 * scale);
            label->setFontFamily(labelFont.family);
            label->setFontFeatures(labelFont.features);
            label->setFontWeight(labelFont.weight);
        }
        for (auto *label : {replyHeading, replyLabel}) {
            label->setFontSize(messageSize);
            label->setFontFamily(font.family);
            label->setFontFeatures(font.features);
            label->setFontWeight(font.weight);
        }
        for (auto &[button, icon] : icons) {
            button->setMinWidth(target);
            button->setMaxWidth(target);
            button->setPadding(7 * scale);
            icon->setSize(18 * scale, 18 * scale);
        }
        for (const auto &[button, command] : formatButtons) {
            button->setMinWidth(target);
            button->setPadding(6 * scale);
            if (command.starts_with("format:foreground:") ||
                command.starts_with("format:background:"))
                button->setMaxWidth(target);
        }
    }
    const auto foreground = composerForeground(owner.model);
    const auto background = themeSurfaceColor("surface_container_high");
    const bool nickDecorationChanged = updateNickDecoration();
    if (projectionForeground != foreground || projectionBackground != background ||
        nickDecorationChanged) {
        projectionForeground = foreground;
        projectionBackground = background;
        input->refreshDisplayProjection(true);
    }
    formatting->setFill(background);
    formatting->setBorder(themeForegroundColor("outline_variant"), std::max(1.f, scale));
    const size_t selectionStart = std::min(input->cursorPosition(), input->selectionAnchor());
    const size_t selectionEnd = std::max(input->cursorPosition(), input->selectionAnchor());
    const auto &activeStyle = formatDocument.styleAt(input->cursorPosition());
    const auto *display = input->displayProjection();
    for (const auto &[button, command] : formatButtons) {
        const auto name = std::string_view(command).substr(7);
        const auto enabledAt = [&](const IrcStyle &style) {
            for (const auto &toggle : formatToggles)
                if (name == toggle.command)
                    return style.*(toggle.active);
            if (name.starts_with("foreground:") || name.starts_with("background:")) {
                const auto digits = name.substr(name.find(':') + 1);
                const IrcColor color{
                    IrcColor::Kind::Indexed,
                    static_cast<std::uint32_t>((digits[0] - '0') * 10 + digits[1] - '0')};
                return (name.starts_with("foreground:") ? style.foregroundSource
                                                        : style.backgroundSource) == color;
            }
            return false;
        };
        bool active = enabledAt(activeStyle);
        if (display && selectionStart != selectionEnd) {
            bool any = false;
            active = true;
            for (const auto &run : formatDocument.runs)
                if (run.end > display->rawToDisplay[selectionStart] &&
                    run.start < display->rawToDisplay[selectionEnd]) {
                    any = true;
                    active = active && enabledAt(run.style);
                }
            if (!any)
                active = enabledAt(activeStyle);
        }
        button->setSelected(active);
        button->setEnabled(!input->composing());
    }
    tools->setFill(themeSurfaceColor("surface_container_high"));
    tools->setBorder(
        themeForegroundColor(input->inputArea()->focused() ? "primary" : "outline_variant"),
        input->inputArea()->focused() ? std::max(1.f, 2 * scale) : std::max(1.f, scale));
    input->setTextColors({}, themeSurfaceColor("primary_container"),
                         themeForegroundColor("on_primary_container"),
                         themeForegroundColor("on_surface"));
    const auto &settings = owner.model.settings;
    const auto shape = settings.cursorShape == "box"         ? Input::CaretShape::Box
                       : settings.cursorShape == "underline" ? Input::CaretShape::Underline
                                                             : Input::CaretShape::Line;
    const auto effect = settings.reducedMotion || settings.fx == FxLevel::Off
                            ? Input::CaretEffect::Off
                        : settings.cursorEffect == "trail"  ? Input::CaretEffect::Trail
                        : settings.cursorEffect == "comet"  ? Input::CaretEffect::Comet
                        : settings.cursorEffect == "ribbon" ? Input::CaretEffect::Ribbon
                        : settings.cursorEffect == "pulse"  ? Input::CaretEffect::Pulse
                                                            : Input::CaretEffect::Off;
    input->setCaretStyle(shape, effect, settings.cursorTrailDurationMs,
                         settings.cursorTrailStrength);
    if (!visibleInScene(*this))
        input->resetCaretEffects();
    replyScratch.clear();
    Color replyColor = themeForegroundColor("on_surface_variant");
    std::string author = "unknown sender";
    replyTooltip.clear();
    if (!pane.reply.empty()) {
        const auto found = owner.model.buffers.find(buffer);
        const Message *message = nullptr;
        const Buffer *source = nullptr;
        if (found != owner.model.buffers.end()) {
            const auto &messages = found->second.messages;
            const auto target =
                std::find_if(messages.begin(), messages.end(), [&](const Message &m) {
                    return ui_logic::messageKey(m) == pane.reply;
                });
            if (target != messages.end())
                message = &*target;
        }
        if (message && found != owner.model.buffers.end()) {
            source = resolveMessageSource(owner.model, found->second, *message);
            if (!message->sourceBuffer.empty())
                message = source
                              ? sourceMessage(owner.model, message->sourceBuffer, message->sourceId)
                              : nullptr;
        }
        const auto policy =
            source ? effectiveFoolDisplay(owner.model.settings, source->id, source->target)
                   : FoolDisplay::Hidden;
        const bool concealed =
            message && (!source || (isFool(owner.model, source->server, message->nick) &&
                                    policy != FoolDisplay::Shown && policy != FoolDisplay::Dimmed &&
                                    (policy == FoolDisplay::Hidden || !owner.replyReadable ||
                                     !owner.replyReadable(pane.id, pane.reply))));
        if (!message)
            replyScratch = "Unavailable message (not loaded or removed)";
        else if (message->deleted)
            replyScratch = "Message deleted";
        else if (concealed)
            replyScratch = "Message hidden";
        else {
            if (!message->nick.empty()) {
                author = parseIrcFormatting(message->nick, foreground,
                                            themeSurfaceColor("surface_container"))
                             .text;
                replyColor = nickColor(owner.model, message->nick, source->server);
            }
            replyScratch = message->text;
            replyTooltip.assign("Replying to ");
            replyTooltip += author;
        }
    }
    replyRow->clearFill();
    replyRow->setBorder(themeForegroundColor("outline_variant"), std::max(1.f, scale));
    replyRail->setFill(withAlpha(replyColor, .45f));
    IrcStyle authorStyle;
    authorStyle.bold = true;
    authorStyle.foreground = replyColor;
    replyHeading->setColor(themeForegroundColor("on_surface_variant"));
    replyAuthorMarkup = ircMarkup(author, authorStyle);
    replyHeadingMarkup.assign("Replying to ");
    replyHeadingMarkup += replyAuthorMarkup;
    replyHeading->setText(replyHeadingMarkup);
    replyHeading->setTooltip(replyTooltip);
    replyLabel->setColor(foreground);
    if (replyScratch != renderedReply || replyTheme != themeRevision()) {
        renderedReply.swap(replyScratch);
        replyTheme = themeRevision();
        auto document =
            parseIrcFormatting(renderedReply, foreground, themeSurfaceColor("surface_container"));
        ircSingleParagraph(document);
        replyLabel->setText(ircMarkup(document));
    }
    shown(*replyRow, !pane.reply.empty());
    replyRow->setParticipatesInLayout(false);
    if (!paste.empty()) {
        auto &destination = destinationScratch;
        destination.clear();
        if (const auto b = owner.model.buffers.find(pasteDestination);
            b != owner.model.buffers.end()) {
            ui_logic::appendPreview(b->second.target, destination, 120);
            destination += " · ";
            ui_logic::appendPreview(serverDisplayName(owner.model, b->second.server), destination,
                                    192);
        } else
            destination = "Unavailable conversation";
        if (pasteVersion != renderedPasteVersion || pasteError != renderedPasteError ||
            destination != renderedDestination) {
            renderedPasteVersion = pasteVersion;
            renderedPasteError = pasteError;
            renderedDestination.swap(destination);
            std::string context;
            if (!pasteError.empty()) {
                context = "Warning: ";
                ui_logic::appendPreview(pasteError, context, context.size() + pasteError.size());
                context += " · ";
            }
            context += "Clipboard for " + renderedDestination + " · " +
                       std::to_string(pasteMessages) + " messages";
            size_t write = 0;
            for (size_t read = 0; read < context.size();) {
                const auto byte = static_cast<unsigned char>(context[read]);
                const bool unicodeSeparator =
                    byte == 0xe2 && read + 2 < context.size() &&
                    static_cast<unsigned char>(context[read + 1]) == 0x80 &&
                    (static_cast<unsigned char>(context[read + 2]) == 0xa8 ||
                     static_cast<unsigned char>(context[read + 2]) == 0xa9);
                const bool nextLine = byte == 0xc2 && read + 1 < context.size() &&
                                      static_cast<unsigned char>(context[read + 1]) == 0x85;
                if (unicodeSeparator || nextLine) {
                    context[write++] = ' ';
                    read += unicodeSeparator ? 3 : 2;
                } else {
                    if (write != read)
                        context[write] = context[read];
                    ++write;
                    ++read;
                }
            }
            context.resize(write);
            pasteContext->setText(context);
            std::string text =
                "Preview: " + (pastePreview.empty() ? "(no visible text)" : pastePreview) +
                "\nInsert " + std::to_string(pasteLines) + " lines into this draft, or send " +
                std::to_string(pasteMessages) +
                " nonempty separate messages. Separate send excludes the existing "
                "draft and reply; blank lines are skipped.";
            pasteLabel->setText(text);
            std::string tooltip;
            if (!pasteError.empty())
                tooltip = "Warning: " + pasteError + "\n";
            tooltip += "Clipboard for " + renderedDestination + " · " +
                       std::to_string(pasteMessages) + " messages\n" + text;
            pasteLabel->setTooltip(std::move(tooltip));
            pasteButtons[0]->setTooltip("Insert " + std::to_string(pasteLines) +
                                        " clipboard lines into the draft for " +
                                        renderedDestination);
            separateButton->setTooltip(
                fullPasteCaption + "\nClipboard for " + renderedDestination +
                "\nSend only the nonempty clipboard lines. Existing draft and "
                "reply are not sent or changed.");
            pasteButtons[2]->setTooltip(
                "Cancel paste. Keep the existing draft and reply unchanged.");
        }
    }
    separateButton->setEnabled(composerWritable(owner.model, buffer) && pasteMessages > 0 &&
                               !paste.empty());
    sendButton->setEnabled(composerWritable(owner.model, buffer) && !input->composing() &&
                           projectionHasText);
    for (auto &[button, icon] : icons)
        icon->setColor(button == uploadButton ? themeForegroundColor("outline")
                                              : button->foregroundColor());
    if (pane.focusComposer && owner.dispatcher && visibleInScene(*this)) {
        owner.dispatcher->setFocus(input->inputArea());
        if (input->inputArea()->focused())
            pane.focusComposer = false;
    }
    updateCompletion();
}

void EditorStore::Impl::Composer::updateUpload() {
    if (!uploadButton)
        return;
    const bool visible = input->value().empty() && !input->composing() &&
                         composerWritable(owner.model, buffer) && visibleInScene(*this);
    if (!visible && uploadButton->visible())
        owner.retirePointer(*uploadButton);
    uploadButton->setVisible(visible);
    uploadButton->setHitTestVisible(visible);
    uploadButton->setTabStop(visible);
    uploadButton->inputArea()->setEnabled(visible);
}

void EditorStore::setOnUploadRequested(UploadRequested callback) {
    impl_->onUploadRequested = std::move(callback);
}
void EditorStore::setOnImageUploadRequested(ImageUploadRequested callback) {
    impl_->onImageUploadRequested = std::move(callback);
}
void EditorStore::setReplyReadable(
    std::function<bool(std::uint64_t, const std::string &)> callback) {
    impl_->replyReadable = std::move(callback);
}
std::optional<EditorStore::UploadOrigin> EditorStore::captureUploadOrigin(std::uint64_t paneId) {
    for (auto *view : impl_->views) {
        if (view->pane.id != paneId || view->buffer != view->pane.buffer ||
            !composerWritable(impl_->model, view->buffer))
            continue;
        impl_->capture(*view);
        auto &draft = impl_->draft(view->buffer);
        impl_->recover(view->buffer, draft);
        const auto &snapshot = committed(draft.state);
        return UploadOrigin{std::make_shared<UploadOrigin::State>(
            UploadOrigin::State{draft.identity, view->buffer, snapshot.value, draft.revision,
                                snapshot.cursorPos, snapshot.selectionAnchor})};
    }
    return std::nullopt;
}
bool EditorStore::uploadSourceCurrent(std::uint64_t paneId, const UploadOrigin &origin,
                                      const Node *anchor) const {
    if (!origin.state)
        return false;
    const auto identity = origin.state->identity.lock();
    if (!identity)
        return false;
    for (const auto *view : impl_->views) {
        const bool paperclip = view->uploadButton == anchor;
        if (view->pane.id != paneId || (!paperclip && view->input != anchor) ||
            view->buffer != view->pane.buffer || view->buffer != origin.state->buffer ||
            !visibleInScene(*view->input) ||
            (paperclip &&
             (!visibleInScene(*view->uploadButton) || !view->input->value().empty())) ||
            view->input->composing())
            continue;
        const auto buffer = impl_->model.buffers.find(view->buffer);
        const auto draft = impl_->drafts.find(view->buffer);
        if (!composerWritable(impl_->model, view->buffer) || draft == impl_->drafts.end())
            return false;
        return draft->second.identity == identity &&
               draft->second.revision == origin.state->revision &&
               view->revision == draft->second.revision &&
               buffer->second.draft == origin.state->content &&
               view->input->value() == origin.state->content &&
               view->input->cursorPosition() == origin.state->cursor &&
               view->input->selectionAnchor() == origin.state->anchor;
    }
    return false;
}
std::string EditorStore::uploadOriginBuffer(const UploadOrigin &origin) const {
    if (!origin.state)
        return {};
    const auto identity = origin.state->identity.lock();
    if (identity)
        for (const auto &[id, draft] : impl_->drafts)
            if (draft.identity == identity && impl_->model.buffers.contains(id))
                return id;
    return origin.state->buffer;
}
EditorStore::UploadInsertResult EditorStore::insertUpload(const UploadOrigin &origin,
                                                          std::string_view text, bool recovery) {
    if (!origin.state)
        return UploadInsertResult::Missing;
    auto &store = *impl_;
    for (auto *view : store.views)
        store.capture(*view);
    const auto identity = origin.state->identity.lock();
    if (!identity)
        return UploadInsertResult::Missing;
    for (auto &[id, draft] : store.drafts) {
        if (draft.identity != identity)
            continue;
        if (!composerWritable(store.model, id))
            return UploadInsertResult::Missing;
        store.recover(id, draft);
        if (draft.state.preeditBase || draft.state.preeditLength || draft.state.preeditCaretHidden)
            return UploadInsertResult::Composing;
        const auto &before = committed(draft.state);
        if (!recovery &&
            (draft.revision != origin.state->revision || before.value != origin.state->content ||
             before.cursorPos != origin.state->cursor ||
             before.selectionAnchor != origin.state->anchor))
            return UploadInsertResult::Changed;
        // Recovery must never replace text the user selected after starting the upload.
        if (recovery && before.cursorPos != before.selectionAnchor)
            return UploadInsertResult::Changed;
        auto edit = store.editor(draft);
        edit.insert(text);
        Input::EditSnapshot candidate{std::move(edit.text), edit.cursor, edit.anchor};
        preserveIrcSuffix(before, candidate, composerForeground(store.model),
                          themeSurfaceColor("surface_container_high"));
        if (!store.valid(before.value, candidate.value))
            return UploadInsertResult::Invalid;
        edit.text = std::move(candidate.value);
        edit.cursor = candidate.cursorPos;
        edit.anchor = candidate.selectionAnchor;
        store.apply(id, draft, std::move(edit), std::nullopt);
        return UploadInsertResult::Inserted;
    }
    return UploadInsertResult::Missing;
}

EditorStore::EditorStore(Model &model, TextClipboard &clipboard, UiCommand command, FocusPane focus,
                         std::function<void(Node &)> retirePointer)
    : impl_(std::make_unique<Impl>(model, clipboard, std::move(command), std::move(focus),
                                   std::move(retirePointer))) {}
EditorStore::~EditorStore() { setOverlayHost(nullptr); }
std::unique_ptr<Node> EditorStore::composer(PaneState &pane) {
    return std::make_unique<Impl::Composer>(*impl_, pane);
}
emoji::Catalog &EditorStore::emojiCatalog() { return impl_->catalog(); }
float EditorStore::minimumComposerWidth(float userScale) { return primaryMinimumWidth(userScale); }
float EditorStore::minimumComposerHeight(std::uint64_t paneId, Renderer &renderer, float innerWidth,
                                         float maxComposerHeight) const {
    for (auto *view : impl_->views)
        if (view->pane.id == paneId) {
            const float height = view->measureMinimum(renderer, innerWidth, maxComposerHeight);
            return view->formattingOpen || !view->paste.empty() ? height : 0;
        }
    return 0;
}
void EditorStore::setReplyHost(std::uint64_t paneId, Node *host) {
    for (auto *view : impl_->views)
        if (view->pane.id == paneId) {
            Node *destination = host ? host : view;
            if (view->replyRow->parent() != destination) {
                impl_->retirePointer(*view->replyRow);
                destination->addChild(view->replyRow->parent()->removeChild(view->replyRow));
            }
            return;
        }
}
float EditorStore::replyContextHeight(std::uint64_t paneId, Renderer &renderer,
                                      float innerWidth) const {
    for (auto *view : impl_->views)
        if (view->pane.id == paneId) {
            LayoutConstraints constraints;
            constraints.setExactWidth(std::max(1.f, innerWidth));
            const float line = renderer.fontRowExtent(view->messageSize, view->font);
            return std::max(2 * line + view->replyText->gap(),
                            view->replyCancelButton->measure(renderer, constraints).height) +
                   view->replyRow->paddingTop() + view->replyRow->paddingBottom();
        }
    return 0;
}
void EditorStore::setOverlayHost(Node *host) {
    auto &store = *impl_;
    if (store.overlayHost == host)
        return;
    if (store.emojiPopup) {
        auto *previous = store.overlayHost;
        auto *popup = store.emojiPopup;
        store.emojiPopup = nullptr;
        store.emojiOwner = nullptr;
        store.emojiMatches = {};
        popup->setVisible(false);
        previous->removeChild(popup);
    }
    if (store.commandPopup) {
        auto *popup = store.commandPopup;
        store.commandPopup = nullptr;
        store.commandOwner = nullptr;
        store.commandCount = 0;
        popup->setVisible(false);
        store.overlayHost->removeChild(popup);
    }
    store.overlayHost = host;
    if (host) {
        auto popup = std::make_unique<emoji::CompletionView>();
        popup->setZIndex(1000);
        popup->setOnChoose([&store, token = std::weak_ptr<int>(store.alive)](size_t index) {
            if (!token.expired() && store.emojiOwner)
                store.emojiOwner->acceptCompletion(index);
        });
        store.emojiPopup = static_cast<emoji::CompletionView *>(host->addChild(std::move(popup)));
        store.emojiPopup->setVisible(false);
        auto commands = std::make_unique<command_completion::View>();
        commands->setZIndex(1000);
        commands->setOnChoose([&store, token = std::weak_ptr<int>(store.alive)](size_t index) {
            if (!token.expired() && store.commandOwner)
                store.commandOwner->acceptCommandCompletion(index);
        });
        store.commandPopup =
            static_cast<command_completion::View *>(host->addChild(std::move(commands)));
        store.commandPopup->setVisible(false);
    }
    for (auto *view : store.views)
        view->updateCompletion(true);
}
bool EditorStore::handleCompositionKey(std::uint32_t sym, std::uint32_t utf32,
                                       std::uint32_t modifiers, bool preedit, bool handledRepeat) {
    auto &store = *impl_;
    const auto *area = store.dispatcher ? store.dispatcher->focusedArea() : nullptr;
    const auto *native = area ? dynamic_cast<const Input *>(area->textInputClient()) : nullptr;
    if (!native || !native->composing() || area->capturesAllKeys())
        return false;
    for (auto *view : store.views) {
        if (view->input != native)
            continue;
        if (view->buffer != view->pane.buffer || !store.model.buffers.contains(view->buffer) ||
            !visibleInScene(*view)) {
            view->compositionChord.clear();
            return false;
        }
        if (view->compositionChordRevision != view->revision)
            view->compositionChord.clear();
        const bool printable = utf32 >= 0x20 && utf32 != 0x7f;
        const bool commandModifier = modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super);
        if (printable && (preedit || (!commandModifier && utf32 >= 0x80))) {
            view->compositionChord.clear();
            return false;
        }
        if (KeySymbol::isModifier(sym))
            return true;
        if (handledRepeat)
            return true;
        if (printable && !commandModifier && view->compositionChord.empty())
            return false;
        InputArea::KeyData key{};
        key.sym = sym;
        key.utf32 = utf32;
        key.modifiers = modifiers;
        key.pressed = true;
        key.preedit = preedit;
        const auto stroke = ui_logic::canonical(bindingStroke(key));
        if (KeySymbol::isEscape(sym) || stroke == "c-g") {
            view->compositionChord.clear();
            return false;
        }
        if (stroke.empty())
            return false;
        const bool pending = !view->compositionChord.empty();
        const auto full = pending ? view->compositionChord + " " + stroke : stroke;
        bool prefix = false, messagePrefix = false;
        for (const auto *command :
             {"message-prev", "message-next", "message-reply", "message-react", "conversation-back",
              "conversation-forward", "scroll-up", "scroll-down", "beginning-of-buffer",
              "end-of-buffer", "paste", "yank", "transpose-chars", "backward-kill-line", "redo",
              "copy", "cut", "select-all"}) {
            const auto binding = store.model.settings.bindings.find(command);
            if (binding == store.model.settings.bindings.end())
                continue;
            const auto candidate = ui_logic::canonical(binding->second);
            const bool message = std::string_view(command).starts_with("message-");
            if (candidate == full) {
                if (message)
                    store.model.status = "Finish or cancel the input-method composition before "
                                         "using message commands.";
                view->compositionChord.clear();
                return true;
            }
            const bool matches = candidate.starts_with(full + " ");
            prefix = prefix || matches;
            messagePrefix = messagePrefix || (message && matches);
        }
        if (prefix) {
            // Retain only known prefixes, bounded by the configured bindings.
            view->compositionChord = full;
            view->compositionChordRevision = view->revision;
            if (messagePrefix)
                store.model.status =
                    "Finish or cancel the input-method composition before using message commands.";
            return true;
        }
        view->compositionChord.clear();
        const bool ctrl = modifiers & KeyMod::Ctrl, shift = modifiers & KeyMod::Shift;
        return pending || (ctrl && (sym == 'v' || sym == 'V' || sym == 'y' || sym == 'Y')) ||
               (shift && KeySymbol::isInsert(sym));
    }
    return false;
}
bool EditorStore::handleCompletionKey(std::uint32_t sym, std::uint32_t modifiers) {
    auto &store = *impl_;
    if (modifiers || !store.dispatcher)
        return false;
    for (auto *view : store.views) {
        if (store.dispatcher->focusedArea() != view->input->inputArea())
            continue;
        view->updateCompletion();
        if (store.commandOwner == view && store.commandCount) {
            if (KeySymbol::isEscape(sym)) {
                view->commandSuppressed = view->commandToken;
                view->hideCompletion();
                return true;
            }
            if (KeySymbol::isEnter(sym) || KeySymbol::isTab(sym)) {
                view->acceptCommandCompletion(view->commandSelected);
                return true;
            }
            if (KeySymbol::isUp(sym) || KeySymbol::isDown(sym)) {
                view->commandSelected =
                    (view->commandSelected + (KeySymbol::isUp(sym) ? store.commandCount - 1 : 1)) %
                    store.commandCount;
                store.commandPopup->setMatches({store.commandMatches.data(), store.commandCount},
                                               view->commandSelected);
                view->placeCompletion();
                return true;
            }
            if (KeySymbol::isPageUp(sym) || KeySymbol::isPageDown(sym)) {
                const size_t page =
                    std::max(size_t{1}, static_cast<size_t>(store.commandPopup->height() /
                                                            (32 * std::max(.1f, view->scale))));
                view->commandSelected =
                    KeySymbol::isPageUp(sym)
                        ? view->commandSelected - std::min(view->commandSelected, page)
                        : std::min(view->commandSelected + page, store.commandCount - 1);
                store.commandPopup->setMatches({store.commandMatches.data(), store.commandCount},
                                               view->commandSelected);
                view->placeCompletion();
                return true;
            }
            return false;
        }
        if (store.emojiOwner != view || store.emojiMatches.empty())
            return false;
        if (KeySymbol::isEscape(sym)) {
            view->emojiSuppressed = view->emojiToken;
            view->emojiSuppressedStart = view->emojiStart;
            view->hideCompletion();
            return true;
        }
        if (KeySymbol::isEnter(sym) || KeySymbol::isTab(sym)) {
            view->acceptCompletion(view->emojiSelected);
            return true;
        }
        if (KeySymbol::isUp(sym) || KeySymbol::isDown(sym)) {
            const size_t count = store.emojiMatches.size();
            view->emojiSelected =
                (view->emojiSelected + (KeySymbol::isUp(sym) ? count - 1 : 1)) % count;
            store.emojiPopup->setMatches(store.emojiMatches, view->emojiSelected);
            view->placeCompletion();
            return true;
        }
        if (KeySymbol::isPageUp(sym) || KeySymbol::isPageDown(sym)) {
            const size_t page =
                std::max(size_t{1}, static_cast<size_t>(store.emojiPopup->height() /
                                                        (44 * std::max(.1f, view->scale))));
            view->emojiSelected =
                KeySymbol::isPageUp(sym)
                    ? view->emojiSelected - std::min(view->emojiSelected, page)
                    : std::min(view->emojiSelected + page, store.emojiMatches.size() - 1);
            store.emojiPopup->setMatches(store.emojiMatches, view->emojiSelected);
            view->placeCompletion();
            return true;
        }
        return false;
    }
    return false;
}
void EditorStore::setInputDispatcher(InputDispatcher &dispatcher) {
    impl_->dispatcher = &dispatcher;
}
void EditorStore::setOnEditingStateChanged(FocusPane callback) {
    impl_->onEditingStateChanged = std::move(callback);
}
void EditorStore::setComposerHeightLimit(std::uint64_t paneId, float maxHeight) {
    for (auto *view : impl_->views)
        if (view->pane.id == paneId) {
            const float limit = std::isfinite(maxHeight) ? std::max(0.f, maxHeight) : 0;
            if (view->heightLimit != limit) {
                view->heightLimit = limit;
                view->markLayoutDirty();
            }
            if (limit == 0)
                view->input->resetCaretEffects();
            break;
        }
}
bool EditorStore::ownsFocus(const InputArea *area) const {
    return area && std::any_of(impl_->views.begin(), impl_->views.end(),
                               [&](const auto *v) { return v->input->inputArea() == area; });
}
bool EditorStore::focusControls(bool backwards) {
    auto &store = *impl_;
    if (!store.dispatcher)
        return false;
    for (auto *view : store.views) {
        if (view->buffer != view->pane.buffer || !visibleInScene(*view))
            continue;
        if (!backwards) {
            if (store.dispatcher->focusedArea() == view->replyCancelButton->inputArea() &&
                view->replyRow->visible() && visibleInScene(*view->replyCancelButton)) {
                store.dispatcher->setFocus(view->input->inputArea());
                return true;
            }
            continue;
        }
        if (store.dispatcher->focusedArea() == view->input->inputArea() &&
            !view->input->composing()) {
            if (view->replyRow->visible() && !view->formattingOpen &&
                visibleInScene(*view->replyCancelButton)) {
                store.dispatcher->setFocus(view->replyCancelButton->inputArea());
                return true;
            }
            store.dispatcher->setFocus(view->formatButton->inputArea());
            return true;
        }
    }
    return false;
}
bool EditorStore::composing() const {
    return impl_->dispatcher &&
           std::any_of(impl_->views.begin(), impl_->views.end(), [&](const auto *v) {
               return v->input->inputArea() == impl_->dispatcher->focusedArea() &&
                      v->input->composing();
           });
}
bool EditorStore::execute(std::string_view command, PaneState &pane) {
    return impl_->execute(command, pane);
}
void EditorStore::prepareBufferRename(std::string_view oldId, std::string_view newId) {
    if (oldId == newId || !impl_->model.buffers.contains(std::string(newId)))
        return;
    for (auto *view : impl_->views)
        if (view->buffer == oldId || view->buffer == newId)
            impl_->retirePointer(*view->uploadButton);
    for (const auto id : {oldId, newId})
        if (auto draft = impl_->drafts.find(std::string(id)); draft != impl_->drafts.end())
            draft->second.identity = std::make_shared<int>(0);
}
void EditorStore::remapBuffer(std::string_view oldId, std::string_view newId,
                              std::uint64_t activePane) {
    impl_->remapBuffer(oldId, newId, activePane);
}
void EditorStore::sync() {
    auto &s = *impl_;
    for (auto *view : s.views)
        view->sync();
    s.syncSpelling();
    for (auto &[id, d] : s.drafts) {
        s.recover(id, d);
        auto b = s.model.buffers.find(id);
        if (b == s.model.buffers.end()) {
            d.identity.reset();
            continue;
        }
        const bool allowed = s.model.settings.typingSend &&
                             (b->second.kind == "channel" || b->second.kind == "query") &&
                             s.permitted(b->second, "+typing");
        const bool active = std::any_of(s.views.begin(), s.views.end(), [&](const auto *v) {
            return v->buffer == id && v->input->inputArea()->focused();
        });
        const auto &text = committed(d.state).value;
        if (allowed && active && !text.empty() && s.model.now - d.edited < 1 &&
            s.model.now - d.lastTyping > 3) {
            s.model.actions.push_back({"typing", id, "active", {}});
            d.lastTyping = s.model.now;
            d.typingActive = true;
        }
        if (d.typingActive && (!allowed || !active || text.empty() || s.model.now - d.edited > 6)) {
            if (allowed)
                s.model.actions.push_back({"typing", id, text.empty() ? "done" : "paused", {}});
            d.typingActive = false;
        }
    }
}
void EditorStore::flush() {
    for (auto *view : impl_->views)
        impl_->capture(*view);
    for (auto &[id, d] : impl_->drafts)
        impl_->recover(id, d);
}
} // namespace blast
