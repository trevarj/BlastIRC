#include "application.hpp"
#include "fx/effects.hpp"
#include "platform/capture.hpp"
#include "platform/clipboard.hpp"
#include "platform/window.hpp"
#include "theme/theme.hpp"
#include "ui/presentation.hpp"
#include "ui/ui.hpp"

#include <core/application_identity.h>
#include <core/deferred_call.h>
#include <core/files/resource_paths.h>
#include <core/input/key_modifiers.h>
#include <core/input/key_symbols.h>
#include <core/timer_manager.h>
#include <core/ui_phase.h>
#include <core/ui_strings.h>
#include <render/animation/animation_manager.h>
#include <render/codecs/codecs.h>
#include <render/gl_shared_context.h>
#include <render/render_context.h>
#include <render/render_target.h>
#include <render/scene/input_dispatcher.h>
#include <render/scene/node.h>
#include <render/text/font_registry.h>
#include <ui/controls/input.h>
#include <ui/controls/select_dropdown_popup.h>
#include <wayland-client.h>
#include <wayland/text_input_service.h>
#include <wayland/wayland_connection.h>
#include <wayland/wayland_seat.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <glib.h>
#include <iostream>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

extern char **environ;

namespace {
using Clock = std::chrono::steady_clock;
volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }

void notifications(const blast::Model &model, bool focused) {
    static std::vector<pid_t> children;
    std::erase_if(children, [](pid_t pid) { return waitpid(pid, nullptr, WNOHANG) != 0; });
    if (focused || !std::getenv("DBUS_SESSION_BUS_ADDRESS"))
        return;
    for (const auto &event : model.effects) {
        if (event.type != "notify" || event.history || children.size() >= 8)
            continue;
        static const std::string icon = [] {
            std::error_code error;
            const auto path = std::filesystem::canonical(paths::assetPath("icon.png"), error);
            return !error && std::filesystem::is_regular_file(path, error)
                       ? "--icon=" + path.string()
                       : std::string("--icon=io.github.trevarj.blastirc");
        }();
        const std::string title = event.nick + " in " + event.target;
        gchar *body = g_markup_escape_text(event.text.c_str(), -1);
        const char *args[] = {
            "notify-send", "--app-name=BlastIRC", icon.c_str(), "--category=im.received",
            "--",          title.c_str(),         body,         nullptr};
        pid_t pid = 0;
        const int result = posix_spawnp(&pid, "notify-send", nullptr, nullptr,
                                        const_cast<char *const *>(args), environ);
        g_free(body);
        if (result == 0)
            children.push_back(pid);
    }
}

struct Options {
    int frames = 0;
    std::uint32_t width = 1440, height = 940;
    std::string screenshot;
};

Options options(int argc, char **argv) {
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout
                << "BlastIRC\nUsage: blastirc [--frames N] [--size WIDTHxHEIGHT] [--screenshot "
                   "FILE.bmp]\n"
                   "Native Wayland IRCv3 client. C-x j jumps on the selected network; "
                   "C-x b switches buffers globally; M-x lists commands.\n"
                   "Defaults: C-p/C-n select messages; C-r replies; C-e opens reaction search. "
                   "Arrow keys and End retain native text editing.\n"
                   "--frames closes after N rendered frames; --screenshot captures the last "
                   "rendered native framebuffer on quit (also without --frames).\n";
            std::exit(0);
        } else if (arg == "--frames" && i + 1 < argc) {
            const std::string value = argv[++i];
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), result.frames);
            if (error != std::errc{} || end != value.data() + value.size() || result.frames < 1)
                throw std::runtime_error("--frames requires a positive integer");
        } else if (arg == "--size" && i + 1 < argc) {
            const std::string value = argv[++i];
            const auto split = value.find('x');
            if (split == std::string::npos)
                throw std::runtime_error("--size requires WIDTHxHEIGHT");
            const auto [widthEnd, widthError] =
                std::from_chars(value.data(), value.data() + split, result.width);
            const auto [heightEnd, heightError] = std::from_chars(
                value.data() + split + 1, value.data() + value.size(), result.height);
            if (widthError != std::errc{} || heightError != std::errc{} ||
                widthEnd != value.data() + split || heightEnd != value.data() + value.size() ||
                result.width < 720 || result.height < 480 || result.width > 32768 ||
                result.height > 32768)
                throw std::runtime_error("--size must be between 720x480 and 32768x32768");
        } else if (arg == "--screenshot" && i + 1 < argc) {
            result.screenshot = argv[++i];
            if (result.screenshot.empty())
                throw std::runtime_error("--screenshot requires a file path");
        } else {
            throw std::runtime_error("Unknown or incomplete argument: " + arg);
        }
    }
    return result;
}

void registerFonts() {
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

// Disconnect observers before their scene owners unwind, including on compositor/renderer failure.
struct SceneBindings {
    WaylandConnection &connection;
    blast::Window &window;
    InputDispatcher &input;
    SelectDropdownPopup &dropdown;
    Node &root;
    ~SceneBindings() {
        connection.setPointerEventCallback({});
        connection.setKeyboardEventCallback({});
        connection.setKeyboardModifiersCallback({});
        dropdown.closeSelectDropdown();
        root.setPopupContext(nullptr);
        input.pointerLeave();
        input.setFocus(nullptr);
        input.setTextInputContext(nullptr, nullptr);
        input.setSceneRoot(nullptr);
        window.setSceneRoot(nullptr);
        window.setOverlayNode(nullptr);
        window.setConfigureCallback({});
        window.setPrepareFrameCallback({});
        window.setFrameTickCallback({});
        window.setScaleChangedCallback({});
        window.setClosedCallback({});
        window.setRenderedCallback({});
        Input::setTextClipboard(nullptr);
    }
};
} // namespace

int main(int argc, char **argv) {
    try {
        const Options opts = options(argc, argv);
        std::signal(SIGUSR1, SIG_IGN);
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        std::signal(SIGPIPE, SIG_IGN);
        prctl(PR_SET_NAME, "blastirc", 0, 0, 0);
        setApplicationIdentity({.name = "blastirc", .envPrefix = "BLASTIRC"});
        setUiStrings({.keyName = {}, .selectPlaceholder = [] { return std::string("Select"); }});
        blast::configureNativeControlBindings();
        codecs::installImageDecoder();

        // The registered IME service outlives connection teardown.
        TextInputService ime;
        WaylandConnection connection;
        connection.connect();
        if (!connection.compositor() || !connection.hasXdgShell())
            throw std::runtime_error("BlastIRC requires Wayland compositor and xdg-shell");
        connection.setTextInputService(&ime);
        auto &clipboard = connection.extension<blast::Clipboard>();
        GlSharedContext shared;
        shared.initialize(connection.display());
        registerFonts();
        RenderContext render;
        render.initialize(shared);
        blast::Application app;
        blast::Theme theme;
        theme.update(app.model);
        std::string fontFamily = blast::themeUiFont().family;
        render.setTextFontFamily(fontFamily);
        AnimationManager animations;
        blast::Window window(connection, opts.width, opts.height);
        window.setRenderContext(&render);
        window.setAnimationManager(&animations);
        ScaledRenderer minimumMetrics(render, 1.f);
        TextFont minimumUiFont, minimumMessageFont;
        float minimumScale = 0, minimumMessageScale = 0;
        std::uint64_t minimumMetricsGeneration = 0;
        const auto syncMinimumHeight = [&] {
            const auto generation = render.textMetricsGeneration();
            const auto scale = blast::themeScale();
            const auto messageScale = blast::themeMessageScale();
            if (minimumScale == scale && minimumMessageScale == messageScale &&
                minimumUiFont == blast::themeUiFont() &&
                minimumMessageFont == blast::themeMessageFont() &&
                minimumMetricsGeneration == generation)
                return;
            window.setMinimumHeight(blast::minimumWindowHeight(minimumMetrics));
            minimumScale = scale;
            minimumMessageScale = messageScale;
            minimumUiFont = blast::themeUiFont();
            minimumMessageFont = blast::themeMessageFont();
            minimumMetricsGeneration = generation;
        };
        syncMinimumHeight();
        InputDispatcher input;
        SelectDropdownPopup dropdown(connection, render);
        blast::Ui ui(app.model, clipboard);
        ui.setInputDispatcher(input);
        auto &root = ui.root();
        root.setAnimationManager(&animations);
        root.setPopupContext(&dropdown);
        window.setSceneRoot(&root);
        input.setSceneRoot(&root);
        Input::setTextClipboard(&clipboard);
        blast::Effects effects;
        effects.attach(root);
        blast::Capture capture(window, !opts.screenshot.empty());
        window.setOverlayNode(&capture);
        SceneBindings bindings{connection, window, input, dropdown, root};
        const auto start = Clock::now();
        auto nextTick = start;
        std::uint64_t frame = 0;
        bool laidOutBeforeEffects = false, animationsAtLayout = false;

        window.setClosedCallback([&] { app.model.quit = true; });
        window.setRenderedCallback([&] {
            ++frame;
            if (opts.frames && frame >= static_cast<std::uint64_t>(opts.frames))
                app.model.quit = true;
            else if (opts.frames)
                window.requestRedraw();
        });
        window.setConfigureCallback([&](std::uint32_t, std::uint32_t) {
            nextTick = Clock::now();
            if (root.width() != static_cast<float>(window.width()) ||
                root.height() != static_cast<float>(window.height()))
                ui.invalidateInputGeometry();
            window.requestLayout();
        });
        window.setScaleChangedCallback([&](float) {
            nextTick = Clock::now();
            ui.invalidateInputGeometry();
            window.requestLayout();
        });
        const auto layoutScene = [&] {
            if (currentUiPhase() == UiPhase::Layout || currentUiPhase() == UiPhase::Render)
                return false;
            if (!render.makeCurrent(window.renderTarget()))
                return false;
            UiPhaseScope phase(UiPhase::Layout);
            root.setSize(static_cast<float>(window.width()), static_cast<float>(window.height()));
            root.layout(window.renderTarget().renderer());
            capture.setSize(static_cast<float>(window.width()),
                            static_cast<float>(window.height()));
            input.syncPointerHover();
            // Ancestor scrolling can move the caret after Input's own layout has completed.
            if (auto *area = input.focusedArea())
                if (auto *client = area->textInputClient())
                    ime.notifyClientStateChanged(client, TextInputChangeCause::Other);
            return true;
        };
        ui.setWindowActions({
            .minimize = [&] { window.minimize(); },
            .toggleMaximize = [&] { window.toggleMaximize(); },
            .close = [&] { app.model.quit = true; },
            .beginMove = [&](std::uint32_t serial) { window.beginMove(serial); },
            .beginResize = [&](std::uint32_t serial,
                               std::uint32_t edge) { window.beginResize(serial, edge); },
            .maximized = [&] { return window.maximized(); },
            .keyboardComposeEnabled =
                [&](bool enabled) { return connection.setKeyboardComposeEnabled(enabled); },
            .synchronizeLayout =
                [&] {
                    if (currentUiPhase() == UiPhase::Layout || currentUiPhase() == UiPhase::Render)
                        return false;
                    ui.update(static_cast<float>(window.width()),
                              static_cast<float>(window.height()), window.effectiveBufferScale());
                    // Input can mutate the scene after this barrier; the frame still needs its
                    // pass.
                    return layoutScene();
                },
            .requestFrame = [&] { window.requestFrameTick(); },
        });
        window.setPrepareFrameCallback([&](bool, bool needsLayout) {
            // Native animation ticks run after the core update and may change geometry again.
            if (needsLayout && (!laidOutBeforeEffects || animationsAtLayout))
                layoutScene();
        });
        window.setFrameTickCallback([&](float) {
            ui.applyPendingResize();
            // Finish the frame's changed input geometry before another captured event.
            if (root.layoutDirty())
                layoutScene();
        });
        input.setCursorShapeCallback([&](std::uint32_t serial, std::uint32_t shape) {
            connection.setCursorShape(serial, shape);
        });
        connection.setPointerEventCallback([&](const PointerEvent &event) {
            if (event.touch) {
                if (event.type == PointerEvent::Type::Enter ||
                    (event.type == PointerEvent::Type::Button && event.pressed)) {
                    if (ui.prepareTouchPress())
                        return;
                } else if (event.type == PointerEvent::Type::Leave) {
                    if (ui.consumeTouchLeave())
                        return;
                } else if ((event.type == PointerEvent::Type::Motion ||
                            event.type == PointerEvent::Type::Button) &&
                           ui.retiredTouch()) {
                    if (event.type == PointerEvent::Type::Button)
                        ui.discardPointerRelease(event.button, true);
                    return;
                }
            }
            if (event.type == PointerEvent::Type::Button && event.pressed) {
                clipboard.setInputSerial(event.serial);
                if (!event.touch)
                    ui.prepareMousePress();
            }
            if (dropdown.onPointerEvent(event)) {
                if (event.type == PointerEvent::Type::Button && !event.pressed)
                    ui.discardPointerRelease(event.button, event.touch);
                return;
            }
            const float x = static_cast<float>(event.sx), y = static_cast<float>(event.sy);
            const bool mainSurface = event.surface == window.wlSurface();
            if (mainSurface && (event.type == PointerEvent::Type::Axis ||
                                (event.type == PointerEvent::Type::Button && event.pressed) ||
                                (event.type == PointerEvent::Type::Enter && event.touch)))
                ui.beginPointerInteraction(x, y, event.type != PointerEvent::Type::Axis);
            if (mainSurface && event.type == PointerEvent::Type::Button &&
                ui.handlePointerButton(event.button, event.pressed, x, y, event.touch))
                return;
            if (!mainSurface) {
                if (event.type == PointerEvent::Type::Button && !event.pressed)
                    ui.discardPointerRelease(event.button, event.touch);
                return;
            }
            switch (event.type) {
            case PointerEvent::Type::Enter:
                input.pointerEnter(x, y, event.serial);
                break;
            case PointerEvent::Type::Leave:
                input.pointerLeave();
                break;
            case PointerEvent::Type::Motion:
                ui.handlePointerMotion(x, y, event.touch);
                if (!event.touch || !ui.retiredTouch())
                    input.pointerMotion(x, y, event.serial);
                break;
            case PointerEvent::Type::Button:
                input.pointerButton(x, y, event.button, event.pressed, event.serial, event.time,
                                    event.touch);
                break;
            case PointerEvent::Type::Axis:
                input.pointerAxis(x, y, event.axis, event.axisSource, event.axisValue,
                                  event.axisDiscrete, event.axisValue120, event.axisLines,
                                  event.axisGestureSerial);
                break;
            }
        });
        std::uint32_t popupOpeningKey = 0;
        connection.setKeyboardEventCallback([&](const KeyboardEvent &event) {
            if (event.pressed && !event.repeat)
                clipboard.setInputSerial(connection.lastInputSerial());
            if (popupOpeningKey && event.key == popupOpeningKey) {
                if (event.pressed && event.repeat)
                    return;
                // Retire Ui's consumed-key state too; a fresh press can follow a release
                // lost while another surface had keyboard focus.
                ui.handleKey({.sym = event.sym,
                              .utf32 = event.utf32,
                              .modifiers = event.modifiers,
                              .pressed = false,
                              .preedit = event.preedit},
                             event.key);
                popupOpeningKey = 0;
                if (!event.pressed)
                    return;
            }
            if (dropdown.isSelectDropdownOpen()) {
                if (event.pressed && !event.preedit && event.modifiers == KeyMod::Ctrl &&
                    event.sym == XKB_KEY_g)
                    app.model.status.clear();
                auto menuKey = event;
                if (!event.preedit && event.modifiers == KeyMod::Ctrl &&
                    (event.sym == XKB_KEY_n || event.sym == XKB_KEY_p)) {
                    menuKey.sym = event.sym == XKB_KEY_n ? XKB_KEY_Down : XKB_KEY_Up;
                    menuKey.utf32 = 0;
                    menuKey.modifiers = 0;
                }
                dropdown.onKeyboardEvent(menuKey);
                return;
            }
            if (connection.lastKeyboardSurface() != window.wlSurface())
                return;
            if (event.pressed && !KeySymbol::isModifier(event.sym) &&
                !ui.synchronizeInput(event.sym))
                throw std::runtime_error("Native keyboard input arrived during layout");
            if (auto *focused = input.focusedArea(); focused && focused->capturesAllKeys()) {
                // Key recorders must receive Tab too, before the dispatcher's focus-navigation
                // defaults.
                focused->dispatchKey(event.sym, event.utf32, event.modifiers, event.pressed,
                                     event.preedit);
                return;
            }
            const InputArea::KeyData key{.sym = event.sym,
                                         .utf32 = event.utf32,
                                         .modifiers = event.modifiers,
                                         .pressed = event.pressed,
                                         .preedit = event.preedit};
            if (!ui.handleKey(key, event.key))
                input.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed,
                               event.preedit);
            if (event.pressed && !event.preedit && KeySymbol::isEnter(event.sym) &&
                dropdown.isSelectDropdownOpen())
                popupOpeningKey = event.key;
        });

        if (!window.initialize())
            throw std::runtime_error("Cannot create BlastIRC native Wayland window");
        dropdown.setParent(window.xdgSurface(), window.wlSurface(),
                           connection.outputForSurface(window.wlSurface()));
        // An xdg toplevel enables IME only after the compositor's authoritative enter.
        input.setTextInputContext(window.wlSurface(), &ime);
        window.requestUpdate();
        const auto persist = [&] {
            ui.flush();
            // A close can precede the next core tick. Admit submitted actions, then retain
            // any buffer or selection that their final dispatch creates before saving.
            if (!app.model.actions.empty()) {
                app.tick(std::chrono::duration<double>(Clock::now() - start).count());
                ui.flush();
            }
            app.save();
        };
        std::vector<pollfd> fds;
        fds.reserve(8);
        try {
            while (!app.model.quit && !stopping) {
                laidOutBeforeEffects = false;
                for (auto &callback : DeferredCall::takePending())
                    callback();
                if (app.model.quit || stopping)
                    break;
                while (wl_display_prepare_read(connection.display()) != 0) {
                    if (wl_display_dispatch_pending(connection.display()) < 0)
                        throw std::runtime_error(connection.describeDisplayError(errno));
                }
                bool needsWrite = false;
                int flushed;
                do {
                    flushed = wl_display_flush(connection.display());
                } while (flushed < 0 && errno == EINTR && !stopping);
                if (flushed < 0) {
                    if (errno == EAGAIN)
                        needsWrite = true;
                    else {
                        const int error = errno;
                        wl_display_cancel_read(connection.display());
                        if (stopping)
                            break;
                        throw std::runtime_error(connection.describeDisplayError(error));
                    }
                }
                fds.clear();
                fds.push_back({wl_display_get_fd(connection.display()),
                               static_cast<short>(POLLIN | (needsWrite ? POLLOUT : 0)), 0});
                const int wakeFd = DeferredCall::wakeFd();
                if (wakeFd >= 0)
                    fds.push_back({wakeFd, POLLIN, 0});
                clipboard.appendPollFds(fds);
                const auto remaining =
                    std::chrono::ceil<std::chrono::milliseconds>(nextTick - Clock::now()).count();
                int timeout = static_cast<int>(std::max<std::int64_t>(0, remaining));
                for (const int deadline :
                     {connection.repeatPollTimeoutMs(), TimerManager::instance().pollTimeoutMs()})
                    if (deadline >= 0)
                        timeout = std::min(timeout, deadline);
                if (Surface::hasPendingFrameWork() || Surface::hasPendingRenders())
                    timeout = 0;
                const int polled = poll(fds.data(), fds.size(), timeout);
                if (polled < 0) {
                    const int error = errno;
                    wl_display_cancel_read(connection.display());
                    if (error == EINTR)
                        continue;
                    throw std::runtime_error("Native event poll failed: " +
                                             std::string(std::strerror(error)));
                }
                if (fds.front().revents & POLLIN) {
                    if (wl_display_read_events(connection.display()) < 0)
                        throw std::runtime_error(connection.describeDisplayError(errno));
                } else {
                    wl_display_cancel_read(connection.display());
                }
                if (wl_display_dispatch_pending(connection.display()) < 0)
                    throw std::runtime_error(connection.describeDisplayError(errno));
                if (fds.front().revents & (POLLERR | POLLHUP | POLLNVAL))
                    throw std::runtime_error(connection.describeDisplayError());
                if (wakeFd >= 0 && fds[1].revents & POLLIN)
                    DeferredCall::drainWakeFd();
                const bool active = connection.lastKeyboardSurface() == window.wlSurface() &&
                                    window.activated() && !window.suspended();
                ui.setWindowActive(active);
                clipboard.dispatchPoll(fds);
                connection.repeatTick();
                TimerManager::instance().tick();
                const auto now = Clock::now();
                if (now >= nextTick && !app.model.quit && !stopping) {
                    nextTick = now + std::chrono::milliseconds(16);
                    UiPhaseScope phase(UiPhase::Update);
                    app.tick(std::chrono::duration<double>(now - start).count());
                    notifications(app.model, active);
                    const auto revision = blast::themeRevision();
                    theme.update(app.model);
                    if (revision != blast::themeRevision())
                        root.markPaintDirty();
                    if (fontFamily != blast::themeUiFont().family) {
                        fontFamily = blast::themeUiFont().family;
                        render.setTextFontFamily(fontFamily);
                        root.markLayoutDirty();
                    }
                    syncMinimumHeight();
                    ui.update(static_cast<float>(window.width()),
                              static_cast<float>(window.height()), window.effectiveBufferScale());
                    if (root.layoutDirty()) {
                        laidOutBeforeEffects = layoutScene();
                        animationsAtLayout = animations.hasActive();
                    }
                    effects.draw(app.model);
                }
                if (!app.model.quit && !stopping) {
                    // The SDK suppresses node callbacks during Update/Layout; schedule their
                    // accumulated work now.
                    if (root.layoutDirty())
                        window.requestLayout();
                    else if (root.paintDirty())
                        window.requestRedraw();
                    Surface::drainPendingFrameWork();
                    Surface::drainPendingRenders();
                }
            }
        } catch (...) {
            try {
                persist();
            } catch (const std::exception &error) {
                std::cerr << "BlastIRC: saving during shutdown failed: " << error.what() << '\n';
            }
            throw;
        }
        persist();
        if (!opts.screenshot.empty() && frame) {
            capture.save(opts.screenshot);
            std::cout << "Screenshot: " << opts.screenshot << '\n';
        }
        std::cout << "BlastIRC closed after " << frame << " frames\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "BlastIRC: " << error.what() << '\n';
        return 1;
    }
}
