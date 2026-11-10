#include "../src/application.hpp"
#include "../src/theme/theme.hpp"
#include "../src/ui/transcript.hpp"
#include "../src/ui/ui.hpp"
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <core/application_identity.h>
#include <core/input/key_modifiers.h>
#include <core/text_clipboard.h>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <render/scene/input_dispatcher.h>
#include <set>
#include <spawn.h>
#include <sys/wait.h>
#include <ui/controls/input.h>
#include <unistd.h>
#include <vector>

extern char **environ;

namespace {
class Clipboard final : public TextClipboard {
  public:
    TextCallback pending;
    std::optional<std::string> clipboardText() override { return std::nullopt; }
    void setClipboardText(std::string) override {}
    void requestClipboardText(TextCallback callback) override { pending = std::move(callback); }
};
Input *inputWithText(Node &node, const std::string &text, const Input *skip = nullptr) {
    if (auto *input = dynamic_cast<Input *>(&node);
        input && input != skip && input->value() == text)
        return input;
    for (const auto &child : node.children())
        if (auto *input = inputWithText(*child, text, skip))
            return input;
    return nullptr;
}
void compose(Input &input, const std::string &text) {
    static_cast<TextInputClient &>(input).textInputApplyEdit(
        {.commitText = {},
         .preeditText = text,
         .preeditCursorBegin = int(text.size()),
         .preeditCursorEnd = int(text.size()),
         .hasPreedit = true});
}
void cancelComposition(Input &input) {
    static_cast<TextInputClient &>(input).textInputResetPreedit();
}
void isolatedLinkFocusCheck(const std::filesystem::path &root) {
    auto executable = std::filesystem::canonical("/proc/self/exe").string();
    char mode[] = "link-focus";
    char *arguments[] = {executable.data(), mode, nullptr};
    std::string childPath = "PATH=" + root.string() + "/missing-native-url-opener";
    std::size_t count = 0;
    while (environ[count])
        ++count;
    std::vector<char *> environment;
    environment.reserve(count + 2);
    for (auto **entry = environ; *entry; ++entry)
        if (!std::string_view(*entry).starts_with("PATH="))
            environment.push_back(*entry);
    environment.push_back(childPath.data());
    environment.push_back(nullptr);
    pid_t child = -1;
    assert(posix_spawn(&child, executable.c_str(), nullptr, nullptr, arguments,
                       environment.data()) == 0);
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    assert(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
void transcriptCollisions() {
    blast::Model model;
    for (const auto *id : {"source", "destination", "other"}) {
        auto &buffer = model.buffers[id];
        buffer.id = buffer.target = id;
        blast::Message message;
        message.id = id;
        message.text = id;
        message.nick = "person";
        buffer.messages.push_back(message);
        ++buffer.messagesRevision;
    }
    Clipboard clipboard;
    blast::PaneState destinationPane{.id = 1, .buffer = "source", .reply = {}};
    blast::PaneState sourcePane{.id = 2, .buffer = "destination", .reply = {}};
    blast::PaneState inactivePane{.id = 3, .buffer = "source", .reply = {}};
    blast::TranscriptView destination(model, destinationPane, clipboard, [](auto) {}, [](auto) {});
    blast::TranscriptView source(model, sourcePane, clipboard, [](auto) {}, [](auto) {});
    blast::TranscriptView inactive(model, inactivePane, clipboard, [](auto) {}, [](auto) {});
    destination.sync();
    destinationPane.following = false;
    destinationPane.unseen = 7;
    destinationPane.buffer = "destination";
    destination.sync();
    destinationPane.following = false;
    destinationPane.unseen = 9;
    source.sync();
    sourcePane.buffer = "source";
    source.sync();
    sourcePane.following = false;
    sourcePane.unseen = 4;
    inactive.sync();
    inactivePane.buffer = "destination";
    inactive.sync();
    inactivePane.following = false;
    inactivePane.unseen = 8;
    inactivePane.buffer = "other";
    inactive.sync();
    auto &merged = model.buffers.at("destination");
    merged.messages.push_front(model.buffers.at("source").messages.front());
    ++merged.messagesRevision;
    model.buffers.erase("source");
    destination.remapBuffer("source", "destination");
    source.remapBuffer("source", "destination");
    inactive.remapBuffer("source", "destination");
    sourcePane.buffer = "destination";
    destination.sync();
    source.sync();
    inactive.sync();
    assert(!destinationPane.following && destinationPane.unseen == 9);
    assert(!sourcePane.following && sourcePane.unseen == 4);
    for (auto pair : {std::pair{&destination, &destinationPane}, std::pair{&source, &sourcePane}}) {
        const auto unseen = pair.second->unseen;
        pair.second->buffer = "other";
        pair.first->sync();
        pair.second->buffer = "destination";
        pair.first->sync();
        assert(!pair.second->following && pair.second->unseen == unseen);
    }
    inactivePane.buffer = "destination";
    inactive.sync();
    assert(!inactivePane.following && inactivePane.unseen == 8);
    destination.remapBuffer("destination", "destination");
    destination.sync();
    assert(destinationPane.unseen == 9);
}
} // namespace

void runModalFocusRemapChecks(
    blast::Model &,
    const std::function<void(const std::string &, const std::string &)> &renameBuffer,
    const std::function<void(const std::string &, blast::Message)> &appendMessage);
void runTranscriptLinkFocusCheck(
    blast::Model &,
    const std::function<void(const std::string &, const std::string &)> &renameBuffer,
    const std::function<void(const std::string &)> &closeBuffer);

namespace blast {
// Use the existing reducer regression friend, in a separate executable, so these
// checks cross the real core callback rather than reproducing its migration.
struct ApplicationRegression {
    static void prepare(Application &app) {
        app.model = Model{};
        app.model.firstRun = false;
        app.model.settings.retentionDays = 0;
        app.model.settings.reducedMotion = true;
        app.model.now = 100;
        app.model.networks["test"].nick = "Me";
    }
    static void unchangedDocument() {
        Application app;
        prepare(app);
        Theme theme;
        theme.update(app.model);
        auto &buffer = app.buffer("test", "Before", "query");
        buffer.draft = "alpha";
        const auto oldId = buffer.id;
        const auto otherId = app.buffer("test", "Other", "query").id;
        app.model.activeBuffer = oldId;
        Clipboard clipboard;
        InputDispatcher dispatcher;
        {
            Ui ui(app.model, clipboard);
            dispatcher.setSceneRoot(&ui.root());
            ui.setInputDispatcher(dispatcher);
            ui.update(1000, 700, 1);
            auto *input = inputWithText(ui.root(), "alpha");
            assert(input && app.model.bufferRenamed);
            dispatcher.setFocus(input->inputArea());
            assert(input->insertText("x"));
            ui.execute("undo");
            assert(input->value() == "alpha" && !input->editingState().history->redo.empty());
            compose(*input, "猫");
            const auto state = input->editingState();
            const auto undo = state.history->undo, redo = state.history->redo;
            app.renameBuffer(oldId, "After");
            const auto newId = app.model.activeBuffer;
            assert(newId != oldId && input->editingStateMatches(state));
            assert(state.history->undo == undo && state.history->redo == redo);
            assert(dispatcher.focusedArea() == input->inputArea());
            assert(app.model.activeBuffer == newId);
            assert(app.model.settings.workspace.at("visible_buffers") == newId);
            app.renameBuffer(newId, "AFTER");
            assert(input->editingStateMatches(state));
            cancelComposition(*input);
            ui.execute("paste");
            assert(clipboard.pending);
            app.renameBuffer(newId, "Final");
            const auto finalId = app.model.activeBuffer;
            auto reply = std::move(clipboard.pending);
            reply(std::string("!"));
            assert(input->value() == "alpha!" && app.model.buffers.at(finalId).draft == "alpha!");
            const auto inactive = input->editingState();
            ui.execute("buffer:" + otherId);
            app.renameBuffer(finalId, "Inactive");
            const auto inactiveId = std::string("test/inactive");
            ui.execute("buffer:" + inactiveId);
            assert(input->editingStateMatches(inactive));
            assert(app.model.buffers.at(inactiveId).draft == "alpha!");
            dispatcher.setFocus(nullptr);
            dispatcher.setSceneRoot(nullptr);
        }
        assert(!app.model.bufferRenamed);
    }
    static void collision(bool focusSource) {
        Application app;
        prepare(app);
        Theme theme;
        theme.update(app.model);
        auto &old = app.buffer("test", "Source", "query");
        old.draft = "source";
        const auto oldId = old.id;
        auto &destination = app.buffer("test", "Destination", "query");
        destination.draft = "destination";
        const auto newId = destination.id;
        app.model.activeBuffer = oldId;
        Clipboard clipboard;
        InputDispatcher dispatcher;
        Ui ui(app.model, clipboard);
        dispatcher.setSceneRoot(&ui.root());
        ui.setInputDispatcher(dispatcher);
        ui.update(1200, 800, 1);
        ui.execute("buffer:" + newId);
        ui.execute("buffer:" + oldId);
        auto *source = inputWithText(ui.root(), "source");
        auto *target = inputWithText(ui.root(), "destination");
        assert(source && target && source != target);
        assert(source->insertText("1") && target->insertText("2"));
        ui.execute("buffer:" + (focusSource ? oldId : newId));
        auto *active = focusSource ? source : target;
        dispatcher.setFocus(active->inputArea());
        compose(*source, "猫");
        compose(*target, "犬");
        const auto kept = active->editingState();
        const auto losingText = (focusSource ? target : source)->value();
        const std::string merged = "source1\ndestination2";
        app.renameBuffer(oldId, "Destination");
        assert(app.model.buffers.at(newId).draft == merged);
        const auto state = active->editingState();
        assert(state.history == kept.history && state.preeditBase);
        assert(state.preeditBase->value == merged);
        assert(state.current.value ==
               (focusSource ? "source1猫\ndestination2" : "source1\ndestination2犬"));
        const auto shift = focusSource ? 0u : std::string("source1\n").size();
        assert(state.current.cursorPos == kept.current.cursorPos + shift);
        assert(state.preeditStart == kept.preeditStart + shift);
        assert(active->editingStateMatches(state));
        assert(dispatcher.focusedArea() == active->inputArea());
        assert(app.model.activeBuffer == newId);
        assert(inputWithText(ui.root(), state.current.value) == active);
        assert(app.model.settings.workspace.at("visible_buffers") == newId);
        cancelComposition(*active);
        assert(active->value() == merged);
        dispatcher.keyEvent('z', 'z', KeyMod::Ctrl, true);
        assert(active->value() == (focusSource ? "source1" : "destination2"));
        dispatcher.keyEvent('y', 'y', KeyMod::Ctrl, true);
        assert(active->value() == merged);
        std::set<std::string> recovered;
        while (!active->editingState().history->undo.empty()) {
            ui.execute("undo");
            recovered.insert(active->value());
        }
        assert(recovered.contains("source") && recovered.contains("destination"));
        assert(recovered.contains(losingText));
        dispatcher.setFocus(nullptr);
        dispatcher.setSceneRoot(nullptr);
    }
    static void identicalDraftCollision() {
        Application app;
        prepare(app);
        Theme theme;
        theme.update(app.model);
        auto &old = app.buffer("test", "SameSource", "query");
        old.draft = "same";
        const auto oldId = old.id;
        auto &destination = app.buffer("test", "SameDestination", "query");
        destination.draft = "same";
        const auto newId = destination.id;
        app.model.activeBuffer = oldId;
        Clipboard clipboard;
        InputDispatcher dispatcher;
        Ui ui(app.model, clipboard);
        dispatcher.setSceneRoot(&ui.root());
        ui.setInputDispatcher(dispatcher);
        ui.update(1200, 800, 1);
        auto *source = inputWithText(ui.root(), "same");
        assert(source && app.model.activeBuffer == oldId);
        ui.execute("buffer:" + newId);
        auto *target = inputWithText(ui.root(), "same", source);
        assert(target && app.model.activeBuffer == newId);
        ui.execute("buffer:" + oldId);
        assert(source->insertText("1"));
        ui.execute("undo");
        ui.execute("buffer:" + newId);
        assert(target->insertText("2"));
        ui.execute("undo");
        ui.execute("buffer:" + oldId);
        const auto state = source->editingState();
        const auto redo = state.history->redo;
        app.renameBuffer(oldId, "SameDestination");
        assert(app.model.buffers.at(newId).draft == "same");
        assert(source->editingStateMatches(state));
        assert(app.model.activeBuffer == newId);
        assert(app.model.settings.workspace.at("visible_buffers") == newId);
        ui.execute("buffer:" + newId);
        assert(inputWithText(ui.root(), "same") == source);
        assert(state.history->redo == redo);
        assert(std::any_of(state.history->undo.begin(), state.history->undo.end(),
                           [](const auto &snapshot) { return snapshot.value == "same2"; }));
        dispatcher.keyEvent('y', 'y', KeyMod::Ctrl, true);
        assert(source->value() == "same1");
        dispatcher.setFocus(nullptr);
        dispatcher.setSceneRoot(nullptr);
    }
    static void modalFocusRemap() {
        Application app;
        prepare(app);
        runModalFocusRemapChecks(
            app.model,
            [&](const std::string &oldId, const std::string &target) {
                app.renameBuffer(oldId, target);
            },
            [&](const std::string &id, Message message) {
                auto *buffer = app.findBuffer(id);
                assert(buffer && app.insert(*buffer, std::move(message), false));
            });
    }
    static void runDeferredCoreCb() {
        Application app;
        prepare(app);
        runTranscriptLinkFocusCheck(
            app.model,
            [&](const std::string &oldId, const std::string &target) {
                app.renameBuffer(oldId, target);
            },
            [&](const std::string &id) {
                Action close{};
                close.type = "close_buffer";
                close.buffer = id;
                app.action(close);
            });
    }
    static void run() {
        unchangedDocument();
        collision(true);
        collision(false);
        identicalDraftCollision();
        modalFocusRemap();
        transcriptCollisions();
    }
};
} // namespace blast

void runComposerCheck();
void runComposerNativeCheck();
void runSettingsCheck();
void runTranscriptCheck();
void runUiIntegrationCheck();
void checkMediaPreview();

int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    setApplicationIdentity({.name = "blastirc", .envPrefix = "BLASTIRC"});
    blast::configureNativeControlBindings();
    char name[] = "/tmp/blastirc-native-rename-check-XXXXXX";
    const auto *created = mkdtemp(name);
    assert(created);
    const std::filesystem::path path(created);
    assert(setenv("XDG_CONFIG_HOME", created, 1) == 0);
    assert(setenv("XDG_DATA_HOME", created, 1) == 0);
    const std::string_view check(argv[1]);
    bool handled = true;
    if (check == "rename")
        blast::ApplicationRegression::run();
    else if (check == "composer")
        runComposerCheck();
    else if (check == "composer-native")
        runComposerNativeCheck();
    else if (check == "settings")
        runSettingsCheck();
    else if (check == "transcript")
        runTranscriptCheck();
    else if (check == "media-preview")
        checkMediaPreview();
    else if (check == "shell") {
        runUiIntegrationCheck();
        isolatedLinkFocusCheck(path);
    } else if (check == "link-focus") {
        const auto *openerPath = std::getenv("PATH");
        const std::filesystem::path openerDirectory = openerPath ? openerPath : "";
        // Direct invocations must not be able to launch the user's desktop opener.
        if (!openerPath || std::string_view(openerPath).find(':') != std::string_view::npos ||
            !openerDirectory.is_absolute() || std::filesystem::exists(openerDirectory))
            handled = false;
        else
            blast::ApplicationRegression::runDeferredCoreCb();
    } else
        handled = false;
    std::filesystem::remove_all(path);
    return handled ? 0 : 2;
}
