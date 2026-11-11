#include "../src/application.hpp"
#include "../src/spellcheck.hpp"
#include "../src/theme/theme.hpp"
#include "../src/ui/channel_avatar.hpp"
#include "../src/ui/dialogs.hpp"
#include "../src/ui/foundry_atlas_data.hpp"
#include "../src/ui/foundry_avatar.hpp"
#include "../src/ui/transcript.hpp"
#include "../src/ui/ui.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <core/input/key_modifiers.h>
#include <core/text_clipboard.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <glib.h>
#include <iterator>
#include <linux/input-event-codes.h>
#include <render/animation/animation_manager.h>
#include <render/animation/motion_service.h>
#include <render/codecs/codecs.h>
#include <render/core/texture_manager.h>
#include <render/scene/glyph_node.h>
#include <render/scene/input_dispatcher.h>
#include <render/text/cairo_text_renderer.h>
#include <thread>
#include <typeinfo>
#include <ui/controls/button.h>
#include <ui/controls/context_menu.h>
#include <ui/controls/image.h>
#include <ui/controls/input.h>
#include <ui/controls/label.h>
#include <ui/controls/scroll_view.h>
#include <ui/controls/select.h>
#include <ui/controls/select_popup_context.h>
#include <ui/controls/slider.h>
#include <ui/controls/toggle.h>
#include <ui/palette.h>
#include <ui/style.h>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
// CPU-only native renderer: Pango supplies real metrics; RGBA image uploads retain pixels.
class CpuTextureUploadSink final : public TextureManager {
  public:
    TextureHandle loadFromRgba(const std::uint8_t *data, int width, int height, bool) override {
        const auto handle = upload(data, width, height);
        images.emplace_back(
            handle.id, std::vector<std::uint8_t>(data, data + std::size_t(width) * height * 4));
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
    std::vector<std::pair<TextureId, std::vector<std::uint8_t>>> images;

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
    const std::vector<std::uint8_t> &pixels(const Image &image) const {
        for (const auto &[id, bytes] : textures.images)
            if (id == image.textureId())
                return bytes;
        std::abort();
    }

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

template <class T, class Predicate> T *find(Node &node, const Predicate &predicate) {
    if (!node.visible())
        return nullptr;
    if (auto *control = dynamic_cast<T *>(&node); control && predicate(*control))
        return control;
    for (const auto &child : node.children())
        if (auto *control = find<T>(*child, predicate))
            return control;
    return nullptr;
}

template <class T> T *named(Node &node, std::string_view name) {
    // Follow the visible form's document/tab order. Treat native controls as leaves:
    // their rendered values and selected options are not labels naming other controls.
    bool namedLabel = false;
    const auto visit = [&](auto &&self, Node &candidate) -> T * {
        if (!candidate.visible())
            return nullptr;
        const bool control =
            dynamic_cast<Input *>(&candidate) || dynamic_cast<Select *>(&candidate) ||
            dynamic_cast<Slider *>(&candidate) || dynamic_cast<Toggle *>(&candidate) ||
            dynamic_cast<Button *>(&candidate);
        if (control) {
            auto *match = namedLabel ? dynamic_cast<T *>(&candidate) : nullptr;
            namedLabel = false;
            return match;
        }
        if (auto *label = dynamic_cast<Label *>(&candidate); label && label->text() == name)
            namedLabel = true;
        for (const auto &child : candidate.children())
            if (auto *match = self(self, *child))
                return match;
        return nullptr;
    };
    auto *control = visit(visit, node);
    if (!control)
        std::fprintf(stderr, "Settings control lookup failed: type=%s, label='%.*s'\n",
                     typeid(T).name(), int(name.size()), name.data());
    assert(control);
    return control;
}

Button *button(Node &node, std::string_view name) {
    auto *control = find<Button>(
        node, [name](const Button &candidate) { return candidate.label()->text() == name; });
    assert(control);
    return control;
}

InputArea *keyed(Node &node, std::string_view key) {
    auto *area = find<InputArea>(
        node, [key](const InputArea &candidate) { return candidate.tabFocusKey() == key; });
    assert(area);
    return area;
}

InputArea::KeyData stroke(std::uint32_t sym, std::uint32_t modifiers = 0) {
    InputArea::KeyData key{};
    key.sym = sym;
    key.modifiers = modifiers;
    key.pressed = true;
    return key;
}

void press(InputDispatcher &dispatcher, Button &control) {
    dispatcher.setFocus(control.inputArea());
    dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
}

void activate(InputDispatcher &dispatcher, Node &control) {
    auto *area = dispatcher.firstTabFocusUnder(&control);
    assert(area);
    dispatcher.setFocus(area);
    dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
}

void edit(InputDispatcher &dispatcher, Input &field, const std::string &text) {
    dispatcher.setFocus(field.inputArea());
    field.selectAll();
    TextInputEdit update{};
    update.commitText = text;
    update.hasCommitText = true;
    field.textInputApplyEdit(update);
    assert(field.value() == text);
}

void sliderInput(Node &node, InputDispatcher &dispatcher) {
    if (!node.visible())
        return;
    if (auto *slider = dynamic_cast<Slider *>(&node)) {
        auto *area = dispatcher.firstTabFocusUnder(slider);
        assert(area && slider->enabled());
        const auto value = slider->value();
        // Unconsumed wheel frames remain available to the parent ScrollView,
        // including fractional touchpad frames that must not edit a setting.
        for (const auto source : {WL_POINTER_AXIS_SOURCE_WHEEL, WL_POINTER_AXIS_SOURCE_FINGER})
            for (const float delta : {-1.f, 1.f}) {
                assert(!area->dispatchAxis(0, 0, WL_POINTER_AXIS_VERTICAL_SCROLL, source,
                                           delta * 15, 0, 0, delta));
                assert(slider->value() == value);
            }
        dispatcher.setFocus(area);
        dispatcher.keyEvent(XKB_KEY_Home, 0, 0, true);
        assert(slider->value() == slider->minValue());
        dispatcher.keyEvent(XKB_KEY_End, 0, 0, true);
        assert(slider->value() == slider->maxValue());
        slider->setValue(value);
    }
    for (const auto &child : node.children())
        sliderInput(*child, dispatcher);
}

std::string fileText(const std::filesystem::path &path) {
    std::ifstream stream(path);
    assert(stream);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void spellingControls(blast::Model &model, blast::Dialogs &dialogs, InputDispatcher &dispatcher) {
    auto *enabled = named<Toggle>(dialogs, "Check spelling");
    assert(model.settings.spellcheck);
    activate(dispatcher, *enabled);
    assert(!model.settings.spellcheck);
    activate(dispatcher, *enabled);
    assert(model.settings.spellcheck);
    auto *language = named<Select>(dialogs, "Spelling language");
    const auto installed = blast::Spellcheck::languages();
    assert(!installed.empty()); // The packaged test environment supplies a real dictionary.
    for (std::size_t index = 0; index < installed.size(); ++index) {
        language->setSelectedIndex(index + 1);
        assert(model.settings.spellcheckLanguage == installed[index]);
    }
    language->setSelectedIndex(0);
    assert(model.settings.spellcheckLanguage == "auto");
}

void settingsTreeControls() {
    using namespace blast;
    Model model;
    model.firstRun = false;
    model.settings.bindings = defaultBindings();
    model.settings.bindings["custom-action"] = "";
    model.settings.cues["mention"] = {true, .5f, 1.f};
    model.identities.push_back({"tree", "TreeNick", "treeuser", "Tree User", {}, {}, {}});
    ServerConfig server;
    server.name = "Tree network";
    server.identity = "tree";
    model.servers.push_back(server);
    Theme theme;
    theme.update(model);
    PangoRenderer renderer;
    InputDispatcher dispatcher;
    Dialogs dialogs(model, [](std::string) {});
    dispatcher.setSceneRoot(&dialogs);
    dialogs.setInputDispatcher(dispatcher);
    dialogs.open("settings");
    const auto frame = [&](bool compact = false) {
        dialogs.sync(compact ? 620 : 1200, 900, 1);
        dialogs.layout(renderer);
    };
    frame(true);
    auto *pickerArea = keyed(dialogs, "blastirc.settings.location");
    auto *picker = dynamic_cast<Select *>(pickerArea->parent());
    assert(picker);
    frame();
    auto *nav = keyed(dialogs, "blastirc.settings.categories");
    const auto key = [&](std::uint32_t sym) {
        dispatcher.setFocus(nav);
        dispatcher.keyEvent(sym, 0, 0, true);
        frame();
    };
    const std::array<std::vector<std::string>, 7> leaves{{
        {"appearance.theme", "appearance.fonts", "appearance.spellcheck", "appearance.cursor",
         "appearance.messages", "appearance.previews", "appearance.navigation"},
        {"keyboard.messaging", "keyboard.cursor", "keyboard.editing", "keyboard.navigation",
         "keyboard.other"},
        {"chat.uploads", "chat.typing", "chat.people", "chat.colors", "chat.history"},
        {"effects.general", "effects.sending", "effects.receiving", "effects.physics"},
        {"sounds.playback", "sounds.cues"},
        {"networks.saved", "networks.endpoint", "networks.authentication", "networks.identity",
         "networks.channels", "networks.actions"},
        {"identities.saved", "identities.profile"},
    }};
    for (std::size_t category = 0; category < leaves.size(); ++category) {
        picker->setSelectedIndex(category);
        frame();
        key(XKB_KEY_Left);
        assert(!find<InputArea>(dialogs, [&](const InputArea &area) {
            return area.tabFocusKey() == "blastirc.settings.section." + leaves[category].front();
        }));
        key(XKB_KEY_Right);
        for (const auto &id : leaves[category]) {
            auto *area = keyed(dialogs, "blastirc.settings.section." + id);
            auto *choice = dynamic_cast<Button *>(area->parent());
            assert(choice);
            press(dispatcher, *choice);
            frame();
            assert(choice->foregroundColor() == ::palette.onPrimary);
            auto *root = dynamic_cast<Button *>(
                keyed(dialogs, "blastirc.settings.category." + std::to_string(category))->parent());
            assert(root && root->foregroundColor() == ::palette.onSurface);
            const auto compactSelection = picker->selectedIndex();
            assert(compactSelection >= 7 && dispatcher.focusedArea() == nav);
            frame(true);
            assert(picker->selectedIndex() == compactSelection &&
                   dispatcher.focusedArea() == pickerArea);
            frame();
            assert(dispatcher.focusedArea() == nav);
            key(XKB_KEY_Left);
            assert(picker->selectedIndex() == category);
        }
    }
    // Keyboard movement follows only visible entries and keeps the roving host as one Tab stop.
    key(XKB_KEY_Home);
    key(XKB_KEY_Left);
    key(XKB_KEY_Down);
    assert(picker->selectedIndex() == 1);
    key(XKB_KEY_Right);
    key(XKB_KEY_Right);
    assert(picker->selectedIndex() >= 7);
    key(XKB_KEY_Left);
    assert(picker->selectedIndex() == 1);
    key(XKB_KEY_Return);
    key(XKB_KEY_End);
    assert(dynamic_cast<Button *>(
               keyed(dialogs, "blastirc.settings.section.identities.profile")->parent())
               ->foregroundColor() == ::palette.onPrimary);

    // Conditional controls retain their leaf and refresh the search index after rebuilds.
    picker->setSelectedIndex(2);
    frame();
    key(XKB_KEY_Right);
    press(dispatcher, *dynamic_cast<Button *>(
                          keyed(dialogs, "blastirc.settings.section.chat.uploads")->parent()));
    named<Select>(dialogs, "Default file host")
        ->setSelectedIndex(std::size_t(UploadProvider::Custom));
    frame();
    assert(
        dynamic_cast<Button *>(keyed(dialogs, "blastirc.settings.section.chat.uploads")->parent())
            ->foregroundColor() == ::palette.onPrimary);
    auto *conditionalQuery = named<Input>(dialogs, "Search settings");
    edit(dispatcher, *conditionalQuery, "Custom HTTPS endpoint");
    dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
    frame();
    assert(named<Input>(dialogs, "Custom HTTPS endpoint") &&
           dispatcher.focusedArea() == conditionalQuery->inputArea());
    named<Select>(dialogs, "Default file host")
        ->setSelectedIndex(std::size_t(UploadProvider::Litterbox));
    frame();
    edit(dispatcher, *conditionalQuery, "Custom HTTPS endpoint");
    named<Select>(dialogs, "Matching settings")->setSelectedIndex(0);
    assert(named<Select>(dialogs, "Matching settings")->selectedText().empty());
    named<Select>(dialogs, "Default file host")
        ->setSelectedIndex(std::size_t(UploadProvider::Soju));
    frame();

    // Managed children remove direct-network leaves; selection falls back to their root.
    picker->setSelectedIndex(5);
    frame();
    press(dispatcher, *button(dialogs, "Add soju account"));
    frame();
    const auto accountIndex = model.servers.size() - 1;
    const auto accountName = model.servers.back().name;
    key(XKB_KEY_Right);
    assert(keyed(dialogs, "blastirc.settings.section.networks.remote"));
    press(dispatcher, *button(dialogs, "Add network"));
    frame();
    model.servers.back().sojuParent = accountName;
    model.servers.back().sojuNetworkId = "tree-child";
    auto *networks = named<Select>(dialogs, "Network");
    networks->setSelectedIndex(0);
    frame();
    networks = named<Select>(dialogs, "Network");
    press(dispatcher, *dynamic_cast<Button *>(
                          keyed(dialogs, "blastirc.settings.section.networks.endpoint")->parent()));
    networks->setSelectedIndex(model.servers.size() - 1);
    frame();
    assert(picker->selectedIndex() == 5 && named<Toggle>(dialogs, "Attach this network locally"));
    assert(!find<InputArea>(dialogs, [](const InputArea &area) {
        return area.tabFocusKey() == "blastirc.settings.section.networks.endpoint";
    }));
    named<Select>(dialogs, "Network")->setSelectedIndex(accountIndex);
    frame();
    // Keep the rest of this isolated fixture on its original direct profile.
    model.servers.resize(1);
    named<Select>(dialogs, "Network")->setSelectedIndex(0);
    frame();

    picker->setSelectedIndex(0);
    frame();
    constexpr std::array avatarIds{"sprite", "monogram", "initials", "none"};
    for (std::size_t i = 0; i < avatarIds.size(); ++i) {
        named<Select>(dialogs, "Avatar style")->setSelectedIndex(i);
        assert(model.settings.avatarStyle == avatarIds[i]);
    }
    named<Select>(dialogs, "Avatar style")->setSelectedIndex(0);
    constexpr std::array paletteIds{"THEME", "CLASSIC", "VIVID"};
    for (std::size_t i = 0; i < paletteIds.size(); ++i) {
        named<Select>(dialogs, "Nick palette")->setSelectedIndex(i);
        assert(model.settings.nickPalette == paletteIds[i]);
    }
    picker->setSelectedIndex(4);
    frame();
    constexpr std::array pitchIds{"natural", "musical", "fixed"};
    for (std::size_t i = 0; i < pitchIds.size(); ++i) {
        named<Select>(dialogs, "Pitch variation")->setSelectedIndex(i);
        assert(model.settings.pitchVariation == pitchIds[i]);
    }
    picker->setSelectedIndex(0);
    frame();
    auto *themeScale = named<Slider>(dialogs, "Interface scale");
    auto *query = named<Input>(dialogs, "Search settings");
    edit(dispatcher, *query, "UI font family");
    dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
    frame();
    assert(dispatcher.focusedArea() == query->inputArea());
    assert(named<Input>(dialogs, "UI font family"));
    assert(!find<Slider>(dialogs, [&](const Slider &control) { return &control == themeScale; }));
    auto *results = named<Select>(dialogs, "Matching settings");
    auto *resultsGroup = results->parent();
    assert(resultsGroup->visible() && resultsGroup->participatesInLayout() &&
           dispatcher.firstTabFocusUnder(resultsGroup));
    edit(dispatcher, *query, "");
    frame();
    assert(!resultsGroup->visible() && !resultsGroup->participatesInLayout() &&
           !dispatcher.firstTabFocusUnder(resultsGroup));
    edit(dispatcher, *query, "retention");
    dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
    frame();
    assert(resultsGroup->visible() && resultsGroup->participatesInLayout() &&
           dispatcher.firstTabFocusUnder(resultsGroup));
    auto *retention = named<Input>(dialogs, "Retention days (1–3650)");
    const auto lastValid = model.settings.retentionDays;
    edit(dispatcher, *retention, "not-a-number");
    picker->setSelectedIndex(0);
    frame();
    model.actions.clear();
    press(dispatcher, *button(dialogs, "Save now"));
    assert(model.actions.empty() && retention->invalid() &&
           model.settings.retentionDays == lastValid);
    edit(dispatcher, *query, "retention");
    dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
    frame();
    assert(named<Input>(dialogs, "Retention days (1–3650)") == retention &&
           retention->value() == "not-a-number");
    edit(dispatcher, *retention, "42");
    assert(model.settings.retentionDays == 42 && !retention->invalid());
    press(dispatcher, *button(dialogs, "Save now"));
    assert(model.actions.size() == 1 && model.actions.back().type == "save");
    dialogs.dismiss();
    assert(model.settings.retentionDays == 42); // Close is not Undo.
}

void settingsTreeAnimatedSelection() {
    using namespace blast;
    auto &motion = MotionService::instance();
    const bool priorEnabled = motion.enabled();
    const float priorSpeed = motion.speed();
    Model model;
    model.firstRun = false;
    model.settings.fx = FxLevel::Full;
    model.settings.reducedMotion = false;
    Theme theme;
    theme.update(model);
    motion.setSpeed(1);
    AnimationManager animations;
    PangoRenderer renderer;
    InputDispatcher dispatcher;
    Dialogs dialogs(model, [](std::string) {});
    dialogs.setAnimationManager(&animations);
    dispatcher.setSceneRoot(&dialogs);
    dialogs.setInputDispatcher(dispatcher);
    dialogs.open("settings");
    const auto frame = [&] {
        dialogs.sync(1200, 900, 1);
        dialogs.layout(renderer);
    };
    frame();
    auto *nav = keyed(dialogs, "blastirc.settings.categories");
    auto *root = dynamic_cast<Button *>(keyed(dialogs, "blastirc.settings.category.0")->parent());
    auto *fonts = dynamic_cast<Button *>(
        keyed(dialogs, "blastirc.settings.section.appearance.fonts")->parent());
    assert(root && fonts && motion.enabled());
    const auto renderedColor = [](Color color) {
        return std::array{std::lround(color.r * 255), std::lround(color.g * 255),
                          std::lround(color.b * 255), std::lround(color.a * 255)};
    };
    const auto settle = [&] {
        // Native animations advance from wall time, not the tick's delta argument.
        std::this_thread::sleep_for(std::chrono::milliseconds(Style::animFast + 10));
        animations.tick(0);
        frame();
    };
    const auto fontsSelected = [&] {
        assert(dispatcher.focusedArea() == nav);
        assert(renderedColor(fonts->foregroundColor()) == renderedColor(::palette.onPrimary));
        assert(renderedColor(root->foregroundColor()) != renderedColor(::palette.onPrimary));
    };
    const auto key = [&](std::uint32_t sym) {
        dispatcher.keyEvent(sym, 0, 0, true);
        dispatcher.keyEvent(sym, 0, 0, false);
        frame();
    };
    dispatcher.setFocus(nav);
    key(XKB_KEY_Right); // Theme & size.
    key(XKB_KEY_Down);  // Fonts, before the previous transition completes.
    assert(animations.hasActive());
    settle();
    fontsSelected();

    // Leave the pointer over the parent while keyboard focus and selection move to its leaf.
    float x = 0, y = 0;
    Node::mapToScene(root->inputArea(), root->width() / 4, root->height() / 2, x, y);
    dispatcher.pointerEnter(x, y, 1);
    assert(dispatcher.pointerButton(x, y, BTN_LEFT, true, 2, 20, false));
    assert(dispatcher.pointerButton(x, y, BTN_LEFT, false, 3, 30, false));
    frame();
    settle();
    assert(renderedColor(root->foregroundColor()) == renderedColor(::palette.onPrimary));
    key(XKB_KEY_Right);
    key(XKB_KEY_Down);
    settle();
    fontsSelected();

    motion.setEnabled(false);
    key(XKB_KEY_Left);
    key(XKB_KEY_Right);
    key(XKB_KEY_Down);
    fontsSelected();
    animations.tick(0);
    dialogs.setAnimationManager(nullptr);
    motion.setSpeed(priorSpeed);
    motion.setEnabled(priorEnabled);
}

void bindingGeometryControls() {
    using namespace blast;
    PangoRenderer renderer;
    std::array<std::string, 2> families;
    {
        Model model;
        model.firstRun = false;
        Theme theme;
        theme.update(model);
        InputDispatcher dispatcher;
        Dialogs dialogs(model, [](std::string) {});
        dispatcher.setSceneRoot(&dialogs);
        dialogs.setInputDispatcher(dispatcher);
        dialogs.open("settings");
        dialogs.sync(720, 480, 1);
        dialogs.layout(renderer);
        auto *installed = named<Select>(dialogs, "Installed UI fonts");
        for (std::size_t index = 0; index < families.size(); ++index) {
            installed->setSelectedIndex(index);
            families[index] = installed->selectedText();
            assert(!families[index].empty());
        }
        // Exercise actual installed families, not aliases which may resolve to the same face.
        assert(families[0] != families[1]);
    }

    constexpr std::array commands{"complete",           "newline",       "backward-delete",
                                  "backward-kill-word", "switch-buffer", "beginning-of-buffer",
                                  "end-of-buffer"};
    struct Bounds {
        float x, y, width, height;
    };
    const auto bounds = [](Node &node) {
        Bounds result{};
        Node::mapToScene(&node, 0, 0, result.x, result.y);
        float right = 0, bottom = 0;
        Node::mapToScene(&node, node.width(), node.height(), right, bottom);
        result.width = right - result.x;
        result.height = bottom - result.y;
        assert(result.width > 0 && result.height > 0);
        return result;
    };
    constexpr float tolerance = .5f;
    const auto separate = [](const Bounds &a, const Bounds &b) {
        return a.x + a.width <= b.x + tolerance || b.x + b.width <= a.x + tolerance ||
               a.y + a.height <= b.y + tolerance || b.y + b.height <= a.y + tolerance;
    };
    for (const auto &family : families)
        for (const float scale : {1.f, 1.5f}) {
            Model model;
            model.firstRun = false;
            model.settings.uiFont = family;
            model.settings.scale = scale;
            for (const auto *command : commands)
                model.settings.bindings[command] = defaultBindings().at(command);
            Theme theme;
            theme.update(model);
            InputDispatcher dispatcher;
            Dialogs dialogs(model, [](std::string) {});
            dispatcher.setSceneRoot(&dialogs);
            dialogs.setInputDispatcher(dispatcher);
            dialogs.open("settings");
            dialogs.sync(720 * scale, 480 * scale, scale);
            auto *category = find<Select>(dialogs, [](const Select &) { return true; });
            assert(category);
            category->setSelectedIndex(1);
            const auto frame = [&](float width) {
                dialogs.sync(width, 1000 * scale, scale);
                dialogs.layout(renderer);
            };
            const auto inspect = [&](float width, bool wide) {
                frame(width);
                std::array<std::array<Bounds, 3>, commands.size()> rows;
                for (std::size_t index = 0; index < commands.size(); ++index) {
                    const std::string command = commands[index];
                    if (command == "beginning-of-buffer" || command == "end-of-buffer") {
                        auto *binding = keyed(dialogs, "blastirc.settings.binding." + command);
                        bool navigation = false;
                        for (auto *node = binding->parent(); node; node = node->parent()) {
                            const auto *heading = find<Label>(*node, [](const Label &label) {
                                return label.text() == "Navigation" ||
                                       label.text() == "Cursor movement";
                            });
                            if (heading) {
                                navigation = heading->text() == "Navigation";
                                break;
                            }
                        }
                        assert(navigation);
                        assert(find<Label>(dialogs, [&](const Label &label) {
                            return label.text() == (command == "beginning-of-buffer"
                                                        ? "Beginning of messages"
                                                        : "End of messages");
                        }));
                    }
                    rows[index] = {bounds(*keyed(dialogs, "blastirc.settings.binding." + command)),
                                   bounds(*keyed(dialogs, "blastirc.settings.record." + command)),
                                   bounds(*keyed(dialogs, "blastirc.settings.reset." + command))};
                    for (const auto &control : rows[index]) {
                        assert(control.x >= -tolerance);
                        assert(control.x + control.width <= width + tolerance);
                    }
                    if (wide) {
                        const auto &current = rows[index][0];
                        const auto &record = rows[index][1];
                        const auto &reset = rows[index][2];
                        assert(current.x + current.width <= record.x + tolerance);
                        assert(record.x + record.width <= reset.x + tolerance);
                        // All three controls really share a row; a stacked layout cannot
                        // accidentally satisfy the column-alignment regression.
                        assert(current.y < record.y + record.height &&
                               record.y < current.y + current.height);
                        assert(record.y < reset.y + reset.height &&
                               reset.y < record.y + record.height);
                        for (std::size_t column = 0; column < rows[index].size(); ++column)
                            assert(std::abs(rows[index][column].x - rows[0][column].x) <=
                                   tolerance);
                        assert(std::abs(current.x + current.width - rows[0][0].x -
                                        rows[0][0].width) <= tolerance);
                    }
                }
                // Check every interactive rectangle, including adjacent stacked rows.
                for (std::size_t row = 0; row < rows.size(); ++row)
                    for (std::size_t column = 0; column < rows[row].size(); ++column)
                        for (std::size_t other = row; other < rows.size(); ++other)
                            for (std::size_t next = other == row ? column + 1 : 0;
                                 next < rows[other].size(); ++next)
                                assert(separate(rows[row][column], rows[other][next]));
            };
            inspect(1600 * scale, true);
            for (const float width : {440.f, 500.f, 580.f, 720.f, 820.f})
                inspect(width * scale, false);
            inspect(1600 * scale, true);
        }
}

void bindingSearchRevealGeometry() {
    using namespace blast;
    PangoRenderer renderer;
    Model model;
    model.firstRun = false;
    model.settings.bindings = defaultBindings();
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    Dialogs dialogs(model, [](std::string) {});
    dispatcher.setSceneRoot(&dialogs);
    dialogs.setInputDispatcher(dispatcher);
    dialogs.open("settings");
    const auto frame = [&](float width) {
        dialogs.sync(width, 480, 1);
        dialogs.layout(renderer);
    };
    frame(720); // Only Appearance has been visited or laid out.
    auto *query = named<Input>(dialogs, "Search settings");
    const auto visibleTarget = [&](std::string_view command) {
        auto *target = keyed(dialogs, "blastirc.settings.binding." + std::string(command));
        ScrollView *detail = nullptr;
        for (auto *ancestor = target->parent(); ancestor && !detail; ancestor = ancestor->parent())
            detail = dynamic_cast<ScrollView *>(ancestor);
        assert(detail);
        float left, top, right, bottom, viewportLeft, viewportTop;
        Node::mapToScene(target, 0, 0, left, top);
        Node::mapToScene(target, target->width(), target->height(), right, bottom);
        Node::mapToScene(detail->content(), 0, detail->scrollOffset(), viewportLeft, viewportTop);
        constexpr float tolerance = .5f;
        assert(right > left && bottom > top);
        assert(left >= viewportLeft - tolerance && top >= viewportTop - tolerance);
        assert(right <= viewportLeft +
                            detail->contentViewportWidth(detail->reservesScrollbarGutter()) +
                            tolerance);
        assert(bottom <= viewportTop + detail->contentViewportHeight() + tolerance);
        // Include every native clip, without depending on the control's nesting depth.
        for (auto *ancestor = target->parent(); ancestor; ancestor = ancestor->parent()) {
            if (!ancestor->clipChildren())
                continue;
            float clipLeft, clipTop, clipRight, clipBottom;
            Node::mapToScene(ancestor, 0, 0, clipLeft, clipTop);
            Node::mapToScene(ancestor, ancestor->width(), ancestor->height(), clipRight,
                             clipBottom);
            assert(left >= clipLeft - tolerance && top >= clipTop - tolerance);
            assert(right <= clipRight + tolerance && bottom <= clipBottom + tolerance);
        }
        assert(dispatcher.focusedArea() == query->inputArea());
    };
    const auto search = [&](std::string_view command, float width) {
        edit(dispatcher, *query, std::string(commandDisplayName(command)));
        dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
        frame(width);
        visibleTarget(command); // First completed layout, not a retry or settled frame.
        frame(width);
        visibleTarget(command);
    };
    search("transpose-chars", 720);
    // A new search and compact-to-wide reflow share the next native layout.
    search("backward-kill-line", 1600);
}

void whoisProfileControls() {
    using namespace blast;
    Model model;
    model.firstRun = false;
    codecs::installImageDecoder();
    PangoRenderer renderer;
    model.buffers["conversation"].draft = "unsent 猫 draft";
    Theme theme;
    theme.update(model);
    struct Clipboard final : TextClipboard {
        std::string copied;
        std::optional<std::string> clipboardText() override { return copied; }
        void setClipboardText(std::string text) override { copied = std::move(text); }
    } clipboard;
    auto *previousClipboard = Input::textClipboard();
    Input::setTextClipboard(&clipboard);
    InputDispatcher dispatcher;
    Node root;
    auto composerOwner = std::make_unique<Input>();
    auto *composer = composerOwner.get();
    composer->setValue(model.buffers["conversation"].draft);
    composer->inputArea()->setTabFocusKey("profile-test.composer");
    composer->selectAll();
    root.addChild(std::move(composerOwner));
    auto dialogOwner = std::make_unique<Dialogs>(model, [](std::string) {});
    auto *dialogs = dialogOwner.get();
    root.addChild(std::move(dialogOwner));
    dispatcher.setSceneRoot(&root);
    dialogs->setInputDispatcher(dispatcher);
    dispatcher.setFocus(composer->inputArea());
    composer->textInputApplyEdit({.commitText = {},
                                  .preeditText = "日本",
                                  .preeditCursorBegin = 6,
                                  .preeditCursorEnd = 6,
                                  .hasPreedit = true});
    assert(composer->composing());
    const auto composingState = composer->editingState();
    const auto sync = [&] { dialogs->sync(720, 480, 1); };
    const auto layout = [&] {
        sync();
        dialogs->layout(renderer);
    };
    auto &profile = model.whoisProfile;
    profile.requestId = "request-one";
    profile.server = "network";
    profile.target = "Alice,Bob";
    profile.open = profile.loading = true;
    ++profile.revision;
    sync();
    assert(!dialogs->active() && dispatcher.focusedArea() == composer->inputArea() &&
           composer->editingStateMatches(composingState));
    composer->textInputResetPreedit();
    const auto draftState = composer->editingState();
    sync();
    assert(dialogs->active() && !dialogs->settingsOpen());
    assert(named<Input>(*dialogs, "Loading")->value().find(profile.target) != std::string::npos);
    auto *openingFocus = dispatcher.focusedArea();
    assert(openingFocus && openingFocus != composer->inputArea());
    WhoisRecord alice;
    alice.nick = "Alice";
    alice.username = "alice-user";
    alice.host = "cloak.example";
    alice.realName = "Alice 猫";
    alice.account = "alice-account";
    alice.server = "irc.example";
    alice.serverInfo = "Example server";
    alice.away = "Out for lunch";
    alice.idle = "42";
    alice.signon = "1700000000";
    alice.channels = "@#one +#two";
    alice.details = {"is an IRC operator", "uses TLS"};
    profile.records.push_back(alice);
    ++profile.revision;
    sync();
    assert(dispatcher.focusedArea() == openingFocus);
    const auto field = [&](std::size_t index, std::string_view key) {
        const std::string selector =
            "blastirc.whois." + std::to_string(index) + "." + std::string(key);
        auto *input = find<Input>(*dialogs, [&](const Input &candidate) {
            return candidate.inputArea()->tabFocusKey() == selector;
        });
        assert(input);
        return input;
    };
    assert(field(0, "nick")->value() == alice.nick &&
           field(0, "username")->value() == alice.username &&
           field(0, "host")->value() == alice.host &&
           field(0, "real-name")->value() == alice.realName &&
           field(0, "account")->value() == alice.account &&
           field(0, "server")->value() == alice.server &&
           field(0, "server-info")->value() == alice.serverInfo &&
           field(0, "away")->value() == alice.away && field(0, "idle")->value() == "42 s" &&
           field(0, "channels")->value() == alice.channels &&
           field(0, "details")->value() == "is an IRC operator\nuses TLS");
    const auto signonSeconds = [&] {
        std::tm parsed{};
        parsed.tm_isdst = -1;
        const auto text = field(0, "signon")->value();
        const auto *zone = strptime(text.c_str(), "%Y-%m-%d %H:%M:%S", &parsed);
        assert(zone && *zone == ' ' && zone[1]);
        return std::mktime(&parsed);
    };
    assert(signonSeconds() == 1700000000 && profile.records[0].signon == alice.signon);
    profile.records[0].idle = "90061";
    profile.records[0].signon = "18446744073709551615";
    ++profile.revision;
    sync();
    assert(field(0, "idle")->value() == "1d 1h 1m 1 s");
    const auto invalidTimestamp = field(0, "signon")->value();
    assert(!invalidTimestamp.empty() && invalidTimestamp != profile.records[0].signon);
    profile.records[0].idle = "0";
    profile.records[0].signon = "not-a-number";
    ++profile.revision;
    sync();
    assert(field(0, "idle")->value() == "0 s" && field(0, "signon")->value() == invalidTimestamp);
    profile.records[0].idle = "18446744073709551616";
    profile.records[0].signon = "0";
    ++profile.revision;
    sync();
    assert(!field(0, "idle")->value().empty() &&
           field(0, "idle")->value() != profile.records[0].idle && signonSeconds() == 0);
    profile.records[0].idle = alice.idle;
    profile.records[0].signon = alice.signon;
    ++profile.revision;
    sync();
    layout();
    auto *aliceAvatar = find<Image>(*dialogs, [](const Image &) { return true; });
    assert(aliceAvatar && aliceAvatar->hasImage());
    const auto expectAvatar = [&](const Image &image, std::string_view nick,
                                  std::string_view mapping) {
        assert(image.width() == 40 && image.height() == 40);
        std::array<std::uint8_t, foundryAtlas::tileSize * foundryAtlas::tileSize * 4> expected{};
        assert(composeFoundryAvatar(expected, nick, mapping));
        assert(renderer.pixels(image) ==
               std::vector<std::uint8_t>(expected.begin(), expected.end()));
    };
    expectAvatar(*aliceAvatar, alice.nick, "rfc1459");
    const auto firstTexture = aliceAvatar->textureId();
    layout();
    assert(aliceAvatar->textureId() == firstTexture);
    assert(find<Label>(*dialogs, [&](const Label &candidate) {
        return candidate.text() == alice.nick && candidate.font().weight == FontWeight::Bold &&
               candidate.color() == nickColor(model, alice.nick, profile.server);
    }));
    auto *host = field(0, "host");
    dispatcher.setFocus(host->inputArea());
    host->selectAll();
    dispatcher.keyEvent(XKB_KEY_c, 0, KeyMod::Ctrl, true);
    assert(clipboard.copied == alice.host);
    assert(!host->insertText("overwrite"));
    dispatcher.keyEvent(XKB_KEY_BackSpace, 0, 0, true);
    TextInputEdit forbidden{};
    forbidden.commitText = "overwrite";
    forbidden.hasCommitText = true;
    host->textInputApplyEdit(forbidden);
    assert(host->value() == alice.host);
    const auto selection = host->editingState();
    WhoisRecord bob;
    bob.nick = "Bob";
    bob.host = "bob.example";
    profile.records.push_back(bob);
    profile.records[0].account = "updated-account";
    profile.loading = false;
    ++profile.revision;
    sync();
    assert(field(0, "host") == host && dispatcher.focusedArea() == host->inputArea() &&
           host->editingStateMatches(selection));
    assert(field(0, "account")->value() == "updated-account" &&
           field(1, "host")->value() == bob.host);
    assert(!find<Input>(*dialogs, [](const Input &candidate) {
        return candidate.inputArea()->tabFocusKey() == "blastirc.whois.1.account";
    }));
    assert(!find<Input>(*dialogs, [](const Input &candidate) {
        return candidate.inputArea()->tabFocusKey() == "blastirc.whois.loading";
    }));
    layout();
    auto *bobAvatar =
        find<Image>(*dialogs, [&](const Image &image) { return &image != aliceAvatar; });
    assert(bobAvatar && bobAvatar->hasImage());
    expectAvatar(*bobAvatar, bob.nick, "rfc1459");
    // Source-network mapping and record identity change without a WHOIS revision.
    profile.records[0].nick = "[Alice^]";
    model.networks[profile.server].caseMapping = "ascii";
    model.networks[alice.server].caseMapping = "rfc1459";
    layout();
    expectAvatar(*aliceAvatar, "[Alice^]", "ascii");
    const auto asciiPixels = renderer.pixels(*aliceAvatar);
    model.networks[profile.server].caseMapping = "strict-rfc1459";
    layout();
    expectAvatar(*aliceAvatar, "[Alice^]", "strict-rfc1459");
    assert(renderer.pixels(*aliceAvatar) != asciiPixels);
    model.settings.avatarStyle = "monogram";
    layout();
    assert(!aliceAvatar->visible() && aliceAvatar->parent()->visible());
    assert(find<Label>(*aliceAvatar->parent(), [](const Label &label) {
        return label.text() == nativeAvatarInitials("[Alice^]");
    }));
    model.settings.avatarStyle = "initials";
    layout();
    assert(!aliceAvatar->visible() && aliceAvatar->parent()->participatesInLayout());
    model.settings.avatarStyle = "none";
    layout();
    assert(!aliceAvatar->parent()->visible() && !aliceAvatar->parent()->participatesInLayout());
    model.settings.avatarStyle = "sprite";
    model.settings.avatars = false;
    layout();
    assert(!aliceAvatar->parent()->visible() && !aliceAvatar->parent()->participatesInLayout());
    model.settings.avatars = true;
    layout();
    assert(aliceAvatar->visible() && aliceAvatar->hasImage());
    expectAvatar(*aliceAvatar, "[Alice^]", "strict-rfc1459");
    dialogs->rebindRenderer(renderer);
    dialogs->invalidateGpuResources(renderer, 1);
    layout();
    assert(aliceAvatar->hasImage());
    expectAvatar(*aliceAvatar, "[Alice^]", "strict-rfc1459");
    assert(field(0, "host") == host && dispatcher.focusedArea() == host->inputArea() &&
           host->editingStateMatches(selection));
    profile.error = "Server ended the request early";
    ++profile.revision;
    sync();
    assert(named<Input>(*dialogs, "Error")->value() == profile.error && field(0, "host") == host &&
           dispatcher.focusedArea() == host->inputArea());
    profile.requestId = "request-two";
    profile.records = {bob};
    profile.error.clear();
    profile.loading = true;
    ++profile.revision;
    sync();
    assert(field(0, "nick")->value() == bob.nick && field(0, "host")->value() == bob.host);
    layout();
    auto *replacementAvatar = find<Image>(*dialogs, [](const Image &) { return true; });
    assert(replacementAvatar && replacementAvatar->hasImage());
    expectAvatar(*replacementAvatar, bob.nick, "strict-rfc1459");
    assert(dialogs->handleKey(stroke(XKB_KEY_Escape)));
    assert(!profile.open && !profile.loading && !dialogs->active());
    sync();
    assert(!dialogs->active() && dispatcher.focusedArea() == composer->inputArea() &&
           composer->editingStateMatches(draftState) &&
           model.buffers["conversation"].draft == "unsent 猫 draft");
    ++profile.revision;
    sync();
    assert(!dialogs->active());

    dialogs->open("settings");
    sync();
    auto *query = named<Input>(*dialogs, "Search settings");
    edit(dispatcher, *query, "retention");
    query->textInputApplyEdit({.commitText = {},
                               .preeditText = "未",
                               .preeditCursorBegin = 3,
                               .preeditCursorEnd = 3,
                               .hasPreedit = true});
    assert(query->composing());
    const auto settingsEdit = query->editingState();
    profile.requestId = "request-three";
    profile.open = profile.loading = true;
    ++profile.revision;
    sync();
    assert(dialogs->settingsOpen() && dispatcher.focusedArea() == query->inputArea() &&
           query->editingStateMatches(settingsEdit));
    profile.loading = false;
    ++profile.revision;
    sync();
    assert(dialogs->settingsOpen() && named<Input>(*dialogs, "Search settings") == query &&
           query->editingStateMatches(settingsEdit));
    dialogs->dismiss();
    sync();
    assert(dialogs->active() && !dialogs->settingsOpen());
    press(dispatcher, *button(*dialogs, "Close"));
    assert(!profile.open && !dialogs->active());
    Input::setTextClipboard(previousClipboard);
}

void nativePopupFonts() {
    class MenuHost final : public SelectPopupContext {
      public:
        DropdownRequest request;
        DropdownCallbacks callbacks;
        void openSelectDropdown(const DropdownRequest &value, DropdownCallbacks handlers) override {
            request = value;
            callbacks = std::move(handlers);
        }
        void closeSelectDropdown() override {
            auto dismiss = std::move(callbacks.onDismiss);
            callbacks = {};
            if (dismiss)
                dismiss();
        }
        bool isSelectDropdownOpen() const override { return bool(callbacks.onDismiss); }
    } host;
    PangoRenderer renderer;
    Select select;
    select.setOptions({"Wide popup option WWWWWiiii"});
    select.setPopupContext(&host);
    auto *trigger = find<Label>(select, [](const Label &) { return true; });
    assert(trigger);
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&select);
    ContextMenuControl menu;
    menu.setMenuWidth(600);
    std::vector<ContextMenuControlEntry> entries(4);
    entries[0].label = "Wide popup option WWWWWiiii";
    entries[0].icon = "settings";
    entries[1].label = "Disabled";
    entries[1].enabled = false;
    entries[2].label = "Heading";
    entries[2].header = true;
    entries[3].label = "Preview";
    entries[3].markupPreview = "<b>Bold</b> preview";
    menu.setEntries(entries);
    char32_t iconCodepoint = 0;
    for (const char *family : {"", "serif", "monospace"}) {
        for (const int weight : {300, 400, 500, 600, 700, 400}) {
            const TextFont font{static_cast<FontWeight>(weight), family, "tnum"};
            trigger->setFontFamily(font.family);
            trigger->setFontFeatures(font.features);
            trigger->setFontWeight(font.weight);
            select.layout(renderer);
            const float x = select.width() / 2, y = select.height() / 2;
            dispatcher.pointerEnter(x, y, 1);
            assert(dispatcher.pointerButton(x, y, BTN_LEFT, true, 2, 20, false));
            assert(dispatcher.pointerButton(x, y, BTN_LEFT, false, 3, 30, false));
            assert(host.isSelectDropdownOpen() && host.request.font == font);
            menu.setFont(host.request.font);
            menu.layout(renderer);
            for (const auto &entry : entries) {
                auto expected = font;
                if (entry.header)
                    expected.weight = FontWeight::Bold;
                const auto &text = entry.markupPreview.empty() ? entry.label : entry.markupPreview;
                const auto *label =
                    find<Label>(menu, [&](const Label &value) { return value.text() == text; });
                assert(label && label->font() == expected);
                auto measuredEntry = entry;
                measuredEntry.icon.clear();
                constexpr float scale = 1.25f;
                const float expectedWidth =
                    std::ceil(renderer
                                  .measureText(text, Style::fontSizeCaption * scale, expected, 0, 1,
                                               TextAlign::Start, entry.ellipsize,
                                               !entry.markupPreview.empty())
                                  .width) +
                    28 * scale;
                assert(ContextMenuControl::preferredWidth(renderer, {measuredEntry}, scale, false,
                                                          font) == expectedWidth);
            }
            const auto *icon = find<GlyphNode>(menu, [](const GlyphNode &) { return true; });
            assert(icon && icon->codepoint() != 0);
            if (iconCodepoint)
                assert(icon->codepoint() == iconCodepoint);
            iconCodepoint = icon->codepoint();
            host.closeSelectDropdown();
        }
    }
    dispatcher.setSceneRoot(nullptr);
}

} // namespace

void runSettingsCheck() {
    using namespace blast;
    nativePopupFonts();
    settingsTreeControls();
    settingsTreeAnimatedSelection();
    bindingGeometryControls();
    bindingSearchRevealGeometry();
    Application app;
    app.model = Model{};
    Model &model = app.model;
    model.firstRun = false;
    model.identities.push_back({"shared", "SharedNick", "shareduser", "Shared Real Name",
                                "identity-secret", "/private/cert.pem", "/private/key.pem"});
    model.identities.push_back(
        {"separate", "OtherNick", "otheruser", "Other Real Name", "other-secret", {}, {}});
    ServerConfig alpha, beta;
    alpha.name = "Alpha network";
    beta.name = "Beta network";
    alpha.identity = beta.identity = "shared";
    alpha.password = "alpha-server-secret";
    beta.password = "beta-server-secret";
    model.servers = {alpha, beta};
    model.settings.bindings = defaultBindings();
    model.settings.cues["mention"] = {true, .5f, 1.f};
    model.networks[beta.name].state = "Ready";
    Theme theme;
    theme.update(model);
    InputDispatcher dispatcher;
    Dialogs dialogs(model, [](std::string) {});
    dispatcher.setSceneRoot(&dialogs);
    dialogs.setInputDispatcher(dispatcher);
    dialogs.open("settings");
    const auto sync = [&] { dialogs.sync(720, 480, 2); };
    sync();
    const auto category = [&](std::size_t index) {
        auto *picker = find<Select>(dialogs, [](const Select &) { return true; });
        assert(picker);
        assert(dispatcher.firstTabFocusUnder(picker));
        picker->setSelectedIndex(index);
        sync();
        assert(dialogs.settingsOpen());
        assert(dispatcher.firstTabFocusUnder(&dialogs));
        sliderInput(dialogs, dispatcher);
    };
    // Search discovers unvisited categories without rebuilding invalid edits or losing focus.
    auto *settingsQuery = named<Input>(dialogs, "Search settings");
    edit(dispatcher, *settingsQuery, "Retention days");
    auto *results = named<Select>(dialogs, "Matching settings");
    dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
    sync();
    auto *retention = named<Input>(dialogs, "Retention days (1–3650)");
    assert(dispatcher.focusedArea() == settingsQuery->inputArea());
    edit(dispatcher, *retention, "invalid-retention");
    assert(retention->invalid());
    edit(dispatcher, *settingsQuery, "UI font family");
    results->setSelectedIndex(0);
    sync();
    assert(named<Input>(dialogs, "UI font family"));
    for (const char *name : {"UI font weight", "Message font weight"}) {
        edit(dispatcher, *settingsQuery, name);
        auto *matches = named<Select>(dialogs, "Matching settings");
        matches->setSelectedIndex(0);
        assert(matches->selectedText().find(name) != std::string::npos);
        dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
        sync();
        assert(named<Select>(dialogs, name));
    }
    edit(dispatcher, *settingsQuery, "msg txt scl");
    named<Select>(dialogs, "Matching settings")->setSelectedIndex(0);
    assert(named<Select>(dialogs, "Matching settings")->selectedText().find("Message text scale") !=
           std::string::npos);
    dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
    sync();
    assert(named<Slider>(dialogs, "Message text scale"));
    edit(dispatcher, *settingsQuery, "Retention days");
    dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
    sync();
    assert(named<Input>(dialogs, "Retention days (1–3650)") == retention &&
           retention->value() == "invalid-retention" && retention->invalid());
    edit(dispatcher, *retention, std::to_string(model.settings.retentionDays));
    edit(dispatcher, *settingsQuery, "identity-secret");
    assert(named<Select>(dialogs, "Matching settings")->selectedText().empty());
    edit(dispatcher, *settingsQuery, "");

    // All seven pages use their production value callbacks. Compact pixel layout
    // and clipping are checked by the real Wayland workflow, not a fake renderer.
    category(0);
    spellingControls(model, dialogs, dispatcher);
    model.settings.spellcheckLanguage = "unavailable_dictionary_for_settings_check";
    dialogs.open("settings");
    sync();
    auto *missingLanguage = named<Select>(dialogs, "Spelling language");
    assert(missingLanguage->selectedText().empty());
    auto *availability = find<Label>(dialogs, [](const Label &label) {
        return label.visible() && !dynamic_cast<const Select *>(label.parent()) &&
               label.text().find("unavailable_dictionary_for_settings_check") !=
                   std::string_view::npos;
    });
    assert(availability);
    missingLanguage->setSelectedIndex(1);
    assert(model.settings.spellcheckLanguage == Spellcheck::languages().front());
    assert(availability->text().find("unavailable_dictionary_for_settings_check") ==
           std::string_view::npos);
    assert(availability->text().find(model.settings.spellcheckLanguage) != std::string_view::npos);
    missingLanguage->setSelectedIndex(0);
    assert(model.settings.spellcheckLanguage == "auto");
    assert(model.settings.foolDisplay == FoolDisplay::Minimized && !model.settings.foolUnread &&
           model.settings.messageWrapColumns == 80);
    for (int style = 0; style < 4; ++style) {
        named<Select>(dialogs, "Fool messages")->setSelectedIndex(style);
        assert(model.settings.foolDisplay == FoolDisplay(style));
    }
    activate(dispatcher, *named<Toggle>(dialogs, "Count fool messages as unread"));
    assert(model.settings.foolUnread);
    for (int columns : {0, 80, 1000}) {
        named<Slider>(dialogs, "Message wrap columns (0 = unlimited)")->setValue(columns);
        assert(model.settings.messageWrapColumns == columns);
    }
    assert(model.settings.linkPreviews == PreviewMode::Click &&
           model.settings.imagePreviews == PreviewMode::Click &&
           model.settings.mediaPreviews == PreviewMode::Click);
    named<Select>(dialogs, "Link previews")->setSelectedIndex(0);
    named<Select>(dialogs, "Image previews")->setSelectedIndex(2);
    assert(model.settings.linkPreviews == PreviewMode::Off &&
           model.settings.imagePreviews == PreviewMode::Automatic &&
           model.settings.mediaPreviews == PreviewMode::Click);
    named<Select>(dialogs, "Audio and video previews")->setSelectedIndex(2);
    edit(dispatcher, *named<Input>(dialogs, "Preview thumbnail height (64–240 px)"), "160");
    edit(dispatcher, *named<Input>(dialogs, "Media size limit (1–512 MiB)"), "32");
    edit(dispatcher, *named<Input>(dialogs, "Media duration limit (1–7200 seconds)"), "300");
    assert(model.settings.previewMaxHeight == 160 && model.settings.previewMediaMaxMiB == 32 &&
           model.settings.previewMediaMaxSeconds == 300);
    assert(!model.settings.previewAllowLocal);
    activate(dispatcher, *named<Toggle>(dialogs, "Allow local-network preview requests"));
    assert(model.settings.previewAllowLocal);
    named<Select>(dialogs, "Theme source")->setSelectedIndex(1);
    assert(model.settings.themeSource == "builtin");
    assert(!model.settings.transparency);
    activate(dispatcher, *named<Toggle>(dialogs, "Enable transparency"));
    named<Slider>(dialogs, "Surface opacity")->setValue(.64);
    assert(model.settings.transparency && std::abs(model.settings.opacity - .64f) < .001f);
    auto *interfaceScale = named<Slider>(dialogs, "Interface scale");
    auto *messageScale = named<Slider>(dialogs, "Message text scale");
    const float previousInterfaceScale = model.settings.scale;
    const float previousMessageScale = model.settings.messageScale;
    dispatcher.setFocus(dispatcher.firstTabFocusUnder(messageScale));
    dispatcher.keyEvent(XKB_KEY_End, 0, 0, true);
    assert(model.settings.messageScale == 2.f && model.settings.scale == previousInterfaceScale);
    dispatcher.setFocus(dispatcher.firstTabFocusUnder(interfaceScale));
    dispatcher.keyEvent(XKB_KEY_Home, 0, 0, true);
    assert(model.settings.scale == .7f && model.settings.messageScale == 2.f);
    interfaceScale->setValue(previousInterfaceScale);
    messageScale->setValue(previousMessageScale);
    named<Select>(dialogs, "Cursor shape")->setSelectedIndex(1);
    auto *cursorAnimation = named<Select>(dialogs, "Cursor animation");
    constexpr std::array cursorEffects{"off", "trail", "comet", "ribbon", "pulse"};
    for (std::size_t index = 0; index < cursorEffects.size(); ++index) {
        cursorAnimation->setSelectedIndex(index);
        assert(model.settings.cursorEffect == cursorEffects[index]);
    }
    cursorAnimation->setSelectedIndex(1);
    named<Slider>(dialogs, "Animation duration (40–240 ms)")->setValue(222);
    named<Slider>(dialogs, "Animation strength (0.05–0.35)")->setValue(.31);
    assert(model.settings.cursorShape == "box" && model.settings.cursorEffect == "trail");
    assert(model.settings.cursorTrailDurationMs == 222 &&
           std::abs(model.settings.cursorTrailStrength - .31f) < .001f);
    auto *uiWeight = named<Select>(dialogs, "UI font weight");
    auto *messageWeight = named<Select>(dialogs, "Message font weight");
    assert(uiWeight && messageWeight && uiWeight->selectedText() == "400" &&
           messageWeight->selectedText() == "400");
    for (std::size_t index = 0; index < 5; ++index) {
        uiWeight->setSelectedIndex(index);
        assert(model.settings.uiFontWeight == 300 + int(index) * 100 &&
               model.settings.messageFontWeight == 400);
        theme.update(model);
        sync();
    }
    auto *ordinary = find<Label>(
        dialogs, [](const Label &caption) { return caption.text() == "UI font weight"; });
    auto *heading =
        find<Label>(dialogs, [](const Label &caption) { return caption.text() == "Fonts"; });
    // The category tree is hidden in this fixture's compact presentation.
    // Exercise its explicit heading weight in the actual wide presentation.
    dialogs.sync(1200, 900, 1);
    auto *categoryRoot =
        dynamic_cast<Button *>(keyed(dialogs, "blastirc.settings.category.0")->parent());
    assert(ordinary && heading && categoryRoot);
    assert(static_cast<int>(ordinary->font().weight) == 700);
    uiWeight->setSelectedIndex(1);
    messageWeight->setSelectedIndex(3);
    theme.update(model);
    sync();
    dialogs.sync(1200, 900, 1);
    assert(model.settings.uiFontWeight == 400 && model.settings.messageFontWeight == 600);
    assert(static_cast<int>(ordinary->font().weight) == 400 &&
           heading->font().weight == FontWeight::Bold &&
           categoryRoot->label()->font().weight == FontWeight::Bold);
    sync();
    auto *uiFamily = named<Input>(dialogs, "UI font family");
    auto *messageFamily = named<Input>(dialogs, "Message font family");
    const auto beforeUiFamily = model.settings.uiFont;
    const auto beforeMessageFamily = model.settings.messageFont;
    edit(dispatcher, *uiFamily, "serif");
    assert(model.settings.uiFont == beforeUiFamily &&
           model.settings.messageFont == beforeMessageFamily &&
           dispatcher.focusedArea() == uiFamily->inputArea());
    dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
    edit(dispatcher, *messageFamily, "monospace");
    assert(model.settings.uiFont == "serif" && model.settings.messageFont == beforeMessageFamily);
    dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
    theme.update(model);
    assert(themeUiFont().family == "serif" && themeMessageFont().family == "monospace");
    edit(dispatcher, *uiFamily, "");
    assert(model.settings.uiFont == "serif" && model.settings.messageFont == "monospace");
    auto *installedUi = named<Select>(dialogs, "Installed UI fonts");
    installedUi->setSelectedIndex(0);
    assert(!installedUi->selectedText().empty());
    theme.update(model);
    assert(themeUiFont().family == installedUi->selectedText() &&
           themeMessageFont().family == "monospace" &&
           uiFamily->value() == installedUi->selectedText());
    edit(dispatcher, *uiFamily, "serif");
    dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
    edit(dispatcher, *uiFamily, "unapplied-font-query");
    dialogs.dismiss();
    dialogs.open("settings");
    sync();
    assert(model.settings.uiFont == "serif" &&
           named<Input>(dialogs, "UI font family")->value() == "serif" &&
           model.settings.messageFont == "monospace");
    assert(named<Select>(dialogs, "UI font weight")->selectedText() == "400" &&
           named<Select>(dialogs, "Message font weight")->selectedText() == "600");

    category(1);
    auto *record = keyed(dialogs, "blastirc.settings.record.complete");
    dispatcher.setFocus(record);
    assert(dispatcher.focusedArea() == record);
    dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
    assert(record->capturesAllKeys());
    const auto tab = stroke(XKB_KEY_Tab), escape = stroke(XKB_KEY_Escape);
    assert(dialogs.handleKey(tab) && dialogs.handleKey(escape));
    assert(dialogs.settingsOpen());
    assert(model.settings.bindings.at("complete") == defaultBindings().at("complete"));
    assert(dialogs.handleKey(stroke(XKB_KEY_Return, KeyMod::Ctrl)));
    const auto recorded = bindingStroke(tab) + " " + bindingStroke(escape);
    assert(model.settings.bindings.at("complete") == recorded && !record->capturesAllKeys());
    dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
    assert(dialogs.handleKey(stroke(XKB_KEY_F6)));
    assert(dialogs.handleKey(stroke(XKB_KEY_g, KeyMod::Ctrl)));
    assert(dialogs.settingsOpen() && model.settings.bindings.at("complete") == recorded);
    assert(!record->capturesAllKeys());
    auto *fieldArea = keyed(dialogs, "blastirc.settings.binding.complete");
    Input *bindingField = nullptr;
    for (Node *node = fieldArea; node; node = node->parent())
        if ((bindingField = dynamic_cast<Input *>(node)))
            break;
    assert(bindingField && bindingField->value() == recorded);
    auto *bindingRow = bindingField->parent()->parent();
    press(dispatcher, *button(*bindingRow, "Reset"));
    assert(model.settings.bindings.at("complete") == defaultBindings().at("complete"));
    edit(dispatcher, *bindingField, "M-F6");
    assert(model.settings.bindings.at("complete") == "M-F6");
    assert(commandLabel(model, "Complete", "complete").find("M-F6") != std::string::npos);

    category(2);
    named<Select>(dialogs, "Presence rows")->setSelectedIndex(2);
    activate(dispatcher, *named<Toggle>(dialogs, "Send typing indicators"));
    assert(model.settings.presence == PresenceMode::Hidden && !model.settings.typingSend);
    named<Select>(dialogs, "Default file host")
        ->setSelectedIndex(std::size_t(UploadProvider::Custom));
    sync();
    edit(dispatcher, *named<Input>(dialogs, "Upload size limit (MiB)"), "100");
    assert(model.settings.uploads.maxBytes == 100ULL * 1048576);
    named<Select>(dialogs, "Default file host")
        ->setSelectedIndex(std::size_t(UploadProvider::CrafterBin));
    sync();
    const auto cappedUploadLimit = uploadProviderLimit(UploadProvider::CrafterBin);
    assert(model.settings.uploads.maxBytes == cappedUploadLimit &&
           validateUploadConfig(model.settings.uploads).empty());
    auto *cappedSize = named<Input>(dialogs, "Upload size limit (MiB)");
    assert(!cappedSize->invalid() &&
           cappedSize->value() == std::to_string(cappedUploadLimit / 1048576));
    named<Select>(dialogs, "Default file host")
        ->setSelectedIndex(std::size_t(UploadProvider::Custom));
    sync();
    assert(model.settings.uploads.maxBytes == cappedUploadLimit &&
           validateUploadConfig(model.settings.uploads).empty());
    edit(dispatcher, *named<Input>(dialogs, "Custom HTTPS endpoint"), "https://upload.example/a");
    edit(dispatcher, *named<Input>(dialogs, "Custom HTTP username"), "alice");
    edit(dispatcher, *named<Input>(dialogs, "Custom upload password"), "private-upload-secret");
    edit(dispatcher, *named<Input>(dialogs, "Expiry hours (empty means host default)"), "24");
    assert(model.settings.uploads.password == "private-upload-secret" &&
           model.settings.uploads.username == "alice");
    // Same authority retains credentials; a new authority retires both immediately.
    edit(dispatcher, *named<Input>(dialogs, "Custom HTTPS endpoint"), "https://upload.example/b");
    assert(model.settings.uploads.password == "private-upload-secret");
    edit(dispatcher, *named<Input>(dialogs, "Custom HTTPS endpoint"), "");
    assert(model.settings.uploads.password == "private-upload-secret" &&
           model.settings.uploads.customEndpoint == "https://upload.example/b");
    edit(dispatcher, *named<Input>(dialogs, "Custom HTTPS endpoint"), "https://upload.example/b");
    edit(dispatcher, *named<Input>(dialogs, "Custom HTTPS endpoint"),
         "https://different.example/b");
    assert(model.settings.uploads.password.empty() && model.settings.uploads.username.empty() &&
           named<Input>(dialogs, "Custom upload password")->value().empty());
    auto *uploadEndpoint = named<Input>(dialogs, "Custom HTTPS endpoint");
    edit(dispatcher, *uploadEndpoint, "http://unsafe.example");
    assert(uploadEndpoint->invalid() &&
           model.settings.uploads.customEndpoint == "https://different.example/b");
    edit(dispatcher, *uploadEndpoint, "https://different.example/b");
    edit(dispatcher, *named<Input>(dialogs, "Custom upload password"), "saved-upload-secret");

    category(3);
    auto *movement = named<Select>(dialogs, "Sending movement");
    auto *reveal = named<Select>(dialogs, "Sending reveal");
    constexpr std::array movements{SendMotion::None, SendMotion::Flight, SendMotion::Rise,
                                   SendMotion::Pop,  SendMotion::Slide,  SendMotion::Drift};
    constexpr std::array reveals{SendReveal::None, SendReveal::Fade, SendReveal::Pixels};
    // Both controls remain independent across every combination.
    for (std::size_t motion = 0; motion < movements.size(); ++motion) {
        movement->setSelectedIndex(motion);
        for (std::size_t effect = 0; effect < reveals.size(); ++effect) {
            reveal->setSelectedIndex(effect);
            assert(model.settings.sendMotion == movements[motion] &&
                   model.settings.sendReveal == reveals[effect]);
        }
    }
    assert(model.settings.incomingMotion == SendMotion::None &&
           model.settings.incomingReveal == SendReveal::Fade &&
           model.settings.incomingDurationMs == 550.f);
    constexpr std::array incomingMotions{SendMotion::None, SendMotion::Rise, SendMotion::Pop,
                                         SendMotion::Slide, SendMotion::Drift};
    for (std::size_t motion = 0; motion < incomingMotions.size(); ++motion) {
        named<Select>(dialogs, "Incoming movement")->setSelectedIndex(motion);
        for (std::size_t effect = 0; effect < reveals.size(); ++effect) {
            named<Select>(dialogs, "Incoming reveal")->setSelectedIndex(effect);
            assert(model.settings.incomingMotion == incomingMotions[motion] &&
                   model.settings.incomingReveal == reveals[effect]);
            assert(model.settings.sendMotion == SendMotion::Drift &&
                   model.settings.sendReveal == SendReveal::Pixels);
        }
    }
    named<Slider>(dialogs, "Incoming duration (100–2000 ms)")->setValue(900);
    assert(model.settings.incomingDurationMs == 900);
    named<Select>(dialogs, "Effects")->setSelectedIndex(0);
    assert(model.settings.fx == FxLevel::Off);
    activate(dispatcher, *named<Toggle>(dialogs, "Reduced motion"));
    named<Slider>(dialogs, "Gravity")->setValue(150);
    named<Slider>(dialogs, "Particle drag")->setValue(2);
    named<Slider>(dialogs, "Overlay shake")->setValue(.5);
    sync();
    assert(model.settings.fx == FxLevel::Off && model.settings.reducedMotion);
    assert(model.settings.gravity == 150 && model.settings.drag == 2 &&
           model.settings.shake == .5f);
    assert(model.settings.sound && model.settings.cursorEffect == "trail" &&
           model.settings.cursorShape == "box");

    category(4);
    activate(dispatcher, *named<Toggle>(dialogs, "Sound enabled"));
    named<Slider>(dialogs, "Master volume")->setValue(.7);
    activate(dispatcher, *named<Toggle>(dialogs, "mention"));
    sync();
    assert(!model.settings.sound && !model.settings.cues.at("mention").enabled);
    assert(std::abs(model.settings.masterVolume - .7f) < .001f);

    category(5);
    assert(!find<Input>(dialogs, [&](const Input &field) {
        return field.value() == model.identities[0].password;
    }));
    auto *scope = find<Label>(dialogs, [&](const Label &label) {
        return label.text().find(alpha.name) != std::string::npos &&
               label.text().find(beta.name) != std::string::npos;
    });
    assert(scope); // Both real network names disclose the selected identity's scope.
    const auto networkId = model.servers[0].name;
    edit(dispatcher, *named<Input>(dialogs, "Display alias"), "Friendly alpha");
    edit(dispatcher, *named<Input>(dialogs, "Avatar image path"), "/tmp/alpha avatar.png");
    assert(model.servers[0].name == networkId && model.servers[0].alias == "Friendly alpha" &&
           model.servers[0].avatar == "/tmp/alpha avatar.png" && model.servers[1].alias.empty());
    edit(dispatcher, *named<Input>(dialogs, "Server password (this network only)"), "alpha-new");
    assert(model.servers[0].password == "alpha-new" &&
           model.servers[1].password == "beta-server-secret" &&
           model.identities[0].password == "identity-secret");
    named<Select>(dialogs, "Network")->setSelectedIndex(1);
    sync();
    model.actions.clear();
    const auto preferences =
        std::filesystem::path(std::getenv("XDG_CONFIG_HOME")) / "blastirc" / "settings.toml";
    const auto profiles = preferences.parent_path() / "config.toml";
    assert(!std::filesystem::exists(preferences));
    std::filesystem::create_directories(preferences);
    press(dispatcher, *button(dialogs, "Save and reconnect"));
    assert(dialogs.settingsOpen() && model.actions.size() == 1);
    const auto connectRequest = model.actions[0].requestId;
    assert(connectRequest && model.actions[0].type == "save-connect" &&
           model.actions[0].text == beta.name);
    app.tick(1);
    sync();
    assert(model.settingsSaveResult.requestId == connectRequest &&
           !model.settingsSaveResult.success && !model.settingsSaveResult.error.empty());
    assert(model.networks.at(beta.name).state == "Ready" && dialogs.settingsOpen());
    assert(find<Label>(dialogs, [&](const Label &label) {
        return label.text() == model.settingsSaveResult.error;
    }));
    press(dispatcher, *button(dialogs, "Edit identity"));
    sync();

    // The link enters the one credential editor for the chosen shared identity.
    edit(dispatcher, *named<Input>(dialogs, "Nickname"), "RenamedNick");
    edit(dispatcher, *named<Input>(dialogs, "SASL password (PLAIN)"), "new-identity-secret");
    edit(dispatcher, *named<Input>(dialogs, "Profile name"), "shared-renamed");
    assert(model.identities[0].nick == "RenamedNick" &&
           model.identities[0].password == "new-identity-secret");
    assert(model.servers[0].identity == "shared-renamed" &&
           model.servers[1].identity == "shared-renamed");
    assert(model.identities[1].password == "other-secret");
    category(6);
    named<Select>(dialogs, "Profile")->setSelectedIndex(1);
    sync();
    edit(dispatcher, *named<Input>(dialogs, "Nickname"), "OnlyOtherNick");
    assert(model.identities[1].nick == "OnlyOtherNick" &&
           model.identities[0].nick == "RenamedNick");
    auto *nickname = named<Input>(dialogs, "Nickname");
    edit(dispatcher, *nickname, "#bad");
    assert(nickname->invalid());
    const auto savedProfiles = fileText(profiles);
    model.actions.clear();
    press(dispatcher, *button(dialogs, "Save now"));
    assert(model.actions.empty()); // UI and storage share the same profile validator.
    assert(!app.save() && fileText(profiles) == savedProfiles);
    app.tick(32); // The automatic save also rejects the live, incomplete profile.
    assert(fileText(profiles) == savedProfiles &&
           model.settingsSaveResult.requestId == connectRequest);
    edit(dispatcher, *nickname, "OnlyOtherNick");
    assert(!nickname->invalid());

    category(5);
    press(dispatcher, *button(dialogs, "Add network"));
    sync();
    assert(model.servers.size() == 3 && named<Select>(dialogs, "Network")->selectedIndex() == 2);
    auto *port = named<Input>(dialogs, "Port (1–65535)");
    edit(dispatcher, *port, "0");
    const auto validPort = model.servers[2].port;
    model.actions.clear();
    press(dispatcher, *button(dialogs, "Save now"));
    assert(port->invalid() && model.actions.empty() && model.servers[2].port == validPort);
    edit(dispatcher, *port, "6697");
    press(dispatcher, *button(dialogs, "Save now"));
    assert(!port->invalid() && model.actions.size() == 1 && model.actions[0].type == "save");
    const auto failedRequest = model.actions[0].requestId;
    assert(failedRequest > connectRequest);
    app.tick(33);
    sync();
    assert(model.settingsSaveResult.requestId == failedRequest &&
           !model.settingsSaveResult.success);
    const auto failedError = model.settingsSaveResult.error;
    assert(!failedError.empty() && model.status == failedError && dialogs.settingsOpen());
    assert(port->value() == "6697" && button(dialogs, "Save now")->enabled());
    assert(find<Label>(dialogs, [&](const Label &label) { return label.text() == failedError; }));

    assert(std::filesystem::remove(preferences));
    press(dispatcher, *button(dialogs, "Save now"));
    const auto successfulRequest = model.actions[0].requestId;
    assert(successfulRequest > failedRequest);
    sync(); // The old core result/global status must not become this request's outcome.
    assert(!button(dialogs, "Save now")->enabled());
    assert(!find<Label>(dialogs, [&](const Label &label) { return label.text() == failedError; }));
    app.tick(34);
    sync();
    assert(model.settingsSaveResult.requestId == successfulRequest &&
           model.settingsSaveResult.success && model.settingsSaveResult.error.empty());
    assert(model.status.empty() && button(dialogs, "Save now")->enabled());
    assert(!find<Label>(dialogs, [&](const Label &label) { return label.text() == failedError; }));
    Model loaded;
    app.storage().load(loaded);
    assert(loaded.settings.cursorShape == "box" && loaded.settings.cursorEffect == "trail" &&
           loaded.settings.cursorTrailDurationMs == 222 &&
           std::abs(loaded.settings.cursorTrailStrength - .31f) < .001f);
    assert(loaded.settings.sendMotion == model.settings.sendMotion &&
           loaded.settings.sendReveal == model.settings.sendReveal);
    assert(loaded.settings.incomingMotion == model.settings.incomingMotion &&
           loaded.settings.incomingReveal == model.settings.incomingReveal &&
           loaded.settings.incomingDurationMs == model.settings.incomingDurationMs &&
           loaded.settings.foolDisplay == model.settings.foolDisplay &&
           loaded.settings.foolUnread == model.settings.foolUnread &&
           loaded.settings.messageWrapColumns == model.settings.messageWrapColumns);
    assert(loaded.settings.linkPreviews == model.settings.linkPreviews &&
           loaded.settings.imagePreviews == model.settings.imagePreviews &&
           loaded.settings.mediaPreviews == model.settings.mediaPreviews &&
           loaded.settings.previewMaxHeight == model.settings.previewMaxHeight &&
           loaded.settings.previewMediaMaxMiB == model.settings.previewMediaMaxMiB &&
           loaded.settings.previewMediaMaxSeconds == model.settings.previewMediaMaxSeconds);
    assert(loaded.settings.previewAllowLocal == model.settings.previewAllowLocal);
    assert(loaded.settings.uiFont == "serif" && loaded.settings.messageFont == "monospace");
    assert(loaded.settings.uiFontWeight == 400 && loaded.settings.messageFontWeight == 600);
    assert(loaded.identities[0].nick == "RenamedNick" &&
           loaded.identities[1].nick == "OnlyOtherNick" &&
           loaded.servers[0].identity == "shared-renamed" &&
           loaded.servers[1].identity == "shared-renamed");
    assert(loaded.settings.transparency && std::abs(loaded.settings.opacity - .64f) < .001f &&
           loaded.servers[0].alias == "Friendly alpha" &&
           loaded.servers[0].avatar == "/tmp/alpha avatar.png");
    assert(loaded.settings.uploads.provider == UploadProvider::Custom &&
           loaded.settings.uploads.maxBytes == cappedUploadLimit &&
           loaded.settings.uploads.password == "saved-upload-secret" &&
           loaded.settings.uploads.customEndpoint == "https://different.example/b");
    // Native forms carry actual remote IDs and typed attributes, never caption-derived identity.
    ServerConfig account;
    account.name = "Soju stable account";
    account.sojuAccount = true;
    account.bouncer = "soju";
    account.identity = model.identities.front().name;
    account.password = "legacy-control-pass";
    account.alias = "My bouncer";
    account.sojuEnabled = false;
    model.servers.push_back(account);
    auto &catalog = model.networks[account.name];
    catalog.state = "Ready";
    catalog.caps.insert("soju.im/bouncer-networks");
    catalog.sojuDiscoveryComplete = true;
    catalog.sojuNetworks["remote-42"] = {
        "remote-42",
        {{"name", "Friendly remote"}, {"host", "irc.example"}, {"port", "6697"}, {"tls", "1"}}};
    ServerConfig child = account;
    child.alias.clear();
    child.password.clear();
    child.name = "Stable legacy child";
    child.sojuAccount = false;
    child.sojuParent = account.name;
    child.sojuNetworkId = "remote-42";
    child.bouncer = "soju:remote-42";
    model.servers.push_back(child);
    dialogs.open("server:" + account.name);
    sync();
    const auto accountIndex = model.servers.size() - 2;
    const auto identityCount = model.identities.size();
    assert(named<Select>(dialogs, "Authentication method")->selectedIndex() == 2 &&
           !named<Input>(dialogs, "Account password")->enabled() &&
           named<Input>(dialogs, "Control connection PASS (management only)")->value() ==
               account.password);
    named<Select>(dialogs, "Authentication method")->setSelectedIndex(0);
    assert(model.servers[accountIndex].sasl == "PLAIN" &&
           named<Input>(dialogs, "Account password")->enabled());
    assert(!find<Label>(dialogs, [](const Label &label) {
        return label.text() == "Server password (this network only)" ||
               label.text() == "Control connection PASS (management only)";
    }));
    edit(dispatcher, *named<Input>(dialogs, "Account username"), "native-account-user");
    edit(dispatcher, *named<Input>(dialogs, "Account password"), "native-account-secret");
    assert(model.identities.front().username == "native-account-user" &&
           model.identities.front().password == "native-account-secret" &&
           model.identities.size() == identityCount &&
           model.servers[accountIndex].password == "legacy-control-pass" &&
           model.servers.front().password == "alpha-new");
    auto *accountPassword = named<Input>(dialogs, "Account password");
    dispatcher.setFocus(accountPassword->inputArea());
    catalog.state = "Failed";
    catalog.error = "Account transport failed";
    model.networks[child.name].state = "Failed";
    model.networks[child.name].error = "Child binding failed";
    sync();
    assert(named<Input>(dialogs, "Account password") == accountPassword &&
           accountPassword->value() == "native-account-secret" &&
           dispatcher.focusedArea() == accountPassword->inputArea());
    assert(find<Label>(dialogs, [&](const Label &label) {
        return label.text().find(account.alias + ": " + catalog.state) != std::string::npos &&
               label.text().find("Account transport failed") != std::string::npos &&
               label.text().find("Friendly remote:") != std::string::npos &&
               label.text().find("Child binding failed") != std::string::npos;
    }));
    named<Select>(dialogs, "Shared identity profile")->setSelectedIndex(1);
    sync();
    edit(dispatcher, *named<Input>(dialogs, "Account password"), "other-account-secret");
    assert(model.identities[1].password == "other-account-secret" &&
           model.identities[0].password == "native-account-secret");
    named<Select>(dialogs, "Authentication method")->setSelectedIndex(1);
    assert(model.servers[accountIndex].sasl == "EXTERNAL" &&
           !named<Input>(dialogs, "Account password")->enabled());
    named<Select>(dialogs, "Shared identity profile")->setSelectedIndex(0);
    sync();
    assert(named<Input>(dialogs, "Account password")->value() == "native-account-secret" &&
           named<Select>(dialogs, "Authentication method")->selectedIndex() == 1);
    named<Select>(dialogs, "Authentication method")->setSelectedIndex(2);
    assert(model.servers[accountIndex].sasl.empty() &&
           named<Input>(dialogs, "Control connection PASS (management only)")->value() ==
               "legacy-control-pass");
    named<Select>(dialogs, "Authentication method")->setSelectedIndex(0);
    activate(dispatcher, *named<Toggle>(dialogs, "Connect automatically"));
    assert(model.servers[accountIndex].sojuEnabled);
    assert(app.save());
    Model savedAccount;
    app.storage().load(savedAccount);
    assert(savedAccount.identities.size() == identityCount &&
           savedAccount.identities[0].username == "native-account-user" &&
           savedAccount.identities[0].password == "native-account-secret" &&
           savedAccount.servers[accountIndex].sasl == "PLAIN" &&
           savedAccount.servers[accountIndex].identity == model.identities[0].name &&
           savedAccount.servers[accountIndex].password == "legacy-control-pass" &&
           savedAccount.servers[accountIndex].sojuEnabled &&
           savedAccount.servers[accountIndex].name == account.name &&
           savedAccount.servers.back().sojuParent == account.name &&
           savedAccount.servers[0].password == "alpha-new");
    catalog.state = "Ready";
    catalog.error.clear();
    model.networks.erase(child.name);
    dialogs.open("server:" + child.name);
    sync();
    const auto remoteCaption = catalog.sojuNetworks.at("remote-42").attributes.at("name");
    assert(named<Select>(dialogs, "Network")->selectedText() == remoteCaption &&
           model.servers.back().name == child.name);
    const auto childIndex = model.servers.size() - 1;
    const auto discoveredRemote = catalog.sojuNetworks.at("remote-42");
    catalog.sojuNetworks.erase("remote-42");
    sync();
    assert(named<Select>(dialogs, "Network")->selectedText() == "Network" &&
           named<Select>(dialogs, "Network")->selectedIndex() == childIndex &&
           model.servers[childIndex].name == child.name);
    catalog.sojuNetworks["remote-42"] = discoveredRemote;
    sync();
    assert(named<Select>(dialogs, "Network")->selectedText() == remoteCaption &&
           named<Select>(dialogs, "Network")->selectedIndex() == childIndex);
    catalog.sojuNetworks["remote-42"].attributes["name"] = "Live renamed network";
    sync();
    assert(named<Select>(dialogs, "Network")->selectedText() == "Live renamed network" &&
           named<Select>(dialogs, "Network")->selectedIndex() == childIndex);
    model.servers[childIndex].alias = "My personal network";
    catalog.sojuNetworks["remote-42"].attributes["name"] = "Another catalog rename";
    sync();
    assert(named<Select>(dialogs, "Network")->selectedText() == "My personal network" &&
           named<Select>(dialogs, "Network")->selectedIndex() == childIndex);
    model.servers[childIndex].alias.clear();
    catalog.sojuNetworks["remote-42"] = discoveredRemote;
    dialogs.open("edit-soju-network:" + child.name);
    sync();
    assert(named<Input>(dialogs, "Remote host")->value() == "irc.example" &&
           named<Input>(dialogs, "Remote name")->enabled());
    edit(dispatcher, *named<Input>(dialogs, "Remote name"), "New remote alias");
    model.actions.clear();
    press(dispatcher, *button(dialogs, "Apply remote changes"));
    assert(model.actions.back().type == "soju-change-network" &&
           model.actions.back().buffer == account.name &&
           model.actions.back().text == "remote-42" &&
           model.actions.back().attributes.at("name") == "New remote alias" &&
           !model.actions.back().attributes.contains("pass") &&
           !model.actions.back().attributes.contains("sasl-password"));
    catalog.sojuMutationPending = true;
    sync();
    assert(!button(dialogs, "Apply remote changes")->enabled());
    catalog.sojuMutationPending = false;
    catalog.sojuMutationError = "Remote request rejected.";
    sync();
    assert(find<Label>(dialogs, [](const Label &label) {
        return label.text() == "Request failed: Remote request rejected.";
    }));
    auto *remoteNameField = named<Input>(dialogs, "Remote name");
    const auto retainedRemoteName = remoteNameField->value();
    const auto remoteInfo = catalog.sojuNetworks.at("remote-42");
    catalog.sojuNetworks.erase("remote-42");
    sync();
    assert(named<Input>(dialogs, "Remote name") == remoteNameField &&
           remoteNameField->value() == retainedRemoteName &&
           !button(dialogs, "Apply remote changes")->enabled());
    assert(!find<Button>(dialogs, [](const Button &candidate) {
        return candidate.label()->text() == "Create remote network";
    }));
    edit(dispatcher, *named<Input>(dialogs, "Remote pass"), "unavailable-context-secret");
    // Opening an edit before discovery completes must retain edit intent.
    dialogs.open("edit-soju-network:" + child.name);
    sync();
    assert(!button(dialogs, "Apply remote changes")->enabled());
    assert(named<Input>(dialogs, "Remote pass")->value().empty() &&
           named<Input>(dialogs, "Remote host")->value().empty());
    assert(!find<Button>(dialogs, [](const Button &candidate) {
        return candidate.label()->text() == "Create remote network";
    }));
    catalog.sojuNetworks["remote-42"] = remoteInfo;
    sync();
    assert(button(dialogs, "Apply remote changes")->enabled());
    dialogs.open("delete-soju-network:" + child.name);
    sync();
    model.actions.clear();
    press(dispatcher, *button(dialogs, "Delete remote network"));
    assert(model.actions.back().type == "soju-delete-network" &&
           model.actions.back().buffer == account.name && model.actions.back().text == "remote-42");
    // A remote edit must not cross account boundaries, even when IDs coincide.
    dialogs.open("edit-soju-network:" + child.name);
    sync();
    edit(dispatcher, *named<Input>(dialogs, "Remote pass"), "account-A-only-secret");
    ServerConfig secondAccount = account;
    secondAccount.name = "Second soju account";
    secondAccount.alias = "Second bouncer";
    model.servers.push_back(secondAccount);
    auto &secondCatalog = model.networks[secondAccount.name];
    secondCatalog.state = "Ready";
    secondCatalog.caps.insert("soju.im/bouncer-networks");
    auto *accountAField = named<Input>(dialogs, "Remote pass");
    dispatcher.setFocus(accountAField->inputArea());
    sync(); // Expose the newly discovered account in the existing native picker.
    assert(named<Input>(dialogs, "Remote pass") == accountAField &&
           dispatcher.focusedArea() == accountAField->inputArea() &&
           accountAField->value() == "account-A-only-secret");
    named<Select>(dialogs, "Network")->setSelectedIndex(model.servers.size() - 1);
    sync();
    assert(named<Select>(dialogs, "Network")->selectedText() == secondAccount.alias);
    assert(named<Input>(dialogs, "Remote pass")->value().empty() &&
           named<Input>(dialogs, "Remote host")->value().empty());
    edit(dispatcher, *named<Input>(dialogs, "Remote host"), "second.example");
    press(dispatcher, *button(dialogs, "Create remote network"));
    assert(model.actions.back().buffer == secondAccount.name &&
           model.actions.back().type == "soju-add-network" &&
           model.actions.back().attributes.at("host") == "second.example" &&
           !model.actions.back().attributes.contains("pass"));
    // Retirement of an earlier child must not retarget live native profile callbacks.
    auto *liveAlias = named<Input>(dialogs, "Account alias");
    const auto focusedBeforeRetirement = liveAlias->inputArea();
    dispatcher.setFocus(focusedBeforeRetirement);
    const auto retiredChild =
        std::find_if(model.servers.begin(), model.servers.end(),
                     [&](const auto &entry) { return entry.name == child.name; });
    model.servers.erase(retiredChild);
    sync();
    assert(named<Input>(dialogs, "Account alias") == liveAlias &&
           dispatcher.focusedArea() == focusedBeforeRetirement);
    edit(dispatcher, *liveAlias, "Second account alias");
    edit(dispatcher, *named<Input>(dialogs, "Avatar image path"), "/tmp/second-account.png");
    sync();
    assert(named<Input>(dialogs, "Account alias") == liveAlias &&
           named<Select>(dialogs, "Network")->selectedText() == "Second account alias");
    const auto secondProfile =
        std::find_if(model.servers.begin(), model.servers.end(),
                     [&](const auto &entry) { return entry.name == secondAccount.name; });
    assert(secondProfile != model.servers.end() && secondProfile->alias == "Second account alias" &&
           secondProfile->avatar == "/tmp/second-account.png");
    model.servers.erase(secondProfile);
    sync();
    edit(dispatcher, *liveAlias, "Must not modify another profile");
    assert(std::none_of(model.servers.begin(), model.servers.end(), [](const auto &entry) {
        return entry.alias == "Must not modify another profile";
    }));
    dialogs.open("server:" + beta.name);
    sync();
    const auto otherName = model.servers.front().name;
    const auto otherHost = model.servers.front().host;
    edit(dispatcher, *named<Input>(dialogs, "Network name"), otherName);
    assert(named<Input>(dialogs, "Network name")->invalid());
    edit(dispatcher, *named<Input>(dialogs, "Server address"), "still-beta.example");
    const auto stillBeta = std::find_if(model.servers.begin(), model.servers.end(),
                                        [&](const auto &entry) { return entry.name == beta.name; });
    assert(stillBeta != model.servers.end() && stillBeta->host == "still-beta.example" &&
           model.servers.front().host == otherHost);
    edit(dispatcher, *named<Input>(dialogs, "Network name"), beta.name);
    // Legacy display selectors require selecting a separate base-account identity.
    Identity selectorIdentity = model.identities.front();
    selectorIdentity.name = "Legacy selector identity";
    selectorIdentity.username = "account/display-selector";
    model.identities.push_back(selectorIdentity);
    ServerConfig legacy = account;
    legacy.name = "Legacy conversion source";
    legacy.sojuAccount = false;
    legacy.alias.clear();
    legacy.identity = selectorIdentity.name;
    model.servers.push_back(legacy);
    dialogs.open("server:" + legacy.name);
    sync();
    press(dispatcher, *button(dialogs, "Convert to soju account"));
    press(dispatcher, *button(dialogs, "Convert account"));
    assert(!model.servers.back().sojuAccount &&
           model.identities.back().username == "account/display-selector");
    named<Select>(dialogs, "Shared identity profile")->setSelectedIndex(0);
    sync();
    press(dispatcher, *button(dialogs, "Convert to soju account"));
    press(dispatcher, *button(dialogs, "Convert account"));
    assert(model.servers.back().sojuAccount &&
           model.servers.back().identity == model.identities.front().name &&
           model.identities.back().username == "account/display-selector");
    // Friendly captions can collide; selecting either still uses its stable profile key.
    ServerConfig captionChildA = child;
    captionChildA.name = "opaque-child-key-A";
    ServerConfig captionChildB = child;
    captionChildB.name = "opaque-child-key-B";
    captionChildB.sojuParent = legacy.name;
    model.servers.push_back(captionChildA);
    model.servers.push_back(captionChildB);
    model.networks[legacy.name].sojuNetworks[captionChildB.sojuNetworkId] = {
        captionChildB.sojuNetworkId, {{"name", remoteCaption}}};
    dialogs.open("server:" + captionChildA.name);
    sync();
    const std::string captionA(named<Select>(dialogs, "Network")->selectedText());
    assert(captionA.find(remoteCaption) != std::string::npos &&
           captionA.find(account.alias) != std::string::npos &&
           captionA.find(captionChildA.name) == std::string::npos);
    named<Select>(dialogs, "Network")->setSelectedIndex(model.servers.size() - 1);
    sync();
    const std::string captionB(named<Select>(dialogs, "Network")->selectedText());
    assert(captionB != captionA && captionB.find(remoteCaption) != std::string::npos &&
           captionB.find(legacy.name) != std::string::npos &&
           captionB.find(captionChildB.name) == std::string::npos);
    activate(dispatcher, *named<Toggle>(dialogs, "Attach this network locally"));
    assert(model.actions.back().buffer == captionChildB.name);
    auto *attachmentControl = named<Toggle>(dialogs, "Attach this network locally");
    model.networks[legacy.name].sojuNetworks[captionChildB.sojuNetworkId].attributes["name"] =
        "Renamed downstream";
    sync();
    assert(named<Toggle>(dialogs, "Attach this network locally") == attachmentControl &&
           named<Select>(dialogs, "Network")->selectedText() == "Renamed downstream" &&
           model.servers.back().name == captionChildB.name);
    model.servers.back().alias = "Personal downstream alias";
    sync();
    assert(named<Select>(dialogs, "Network")->selectedText() == "Personal downstream alias" &&
           model.servers.back().name == captionChildB.name);
    const auto previousActiveBuffer = model.activeBuffer;
    const std::string topicId = captionChildB.name + "/#caption-topic";
    auto &topicBuffer = model.buffers[topicId];
    topicBuffer.server = captionChildB.name;
    topicBuffer.target = "#caption-topic";
    topicBuffer.kind = "channel";
    topicBuffer.joined = true;
    model.activeBuffer = topicId;
    dialogs.open("topic");
    sync();
    auto *liveTopicTitle = find<Label>(dialogs, [](const Label &label) {
        return label.text() == "#caption-topic · Personal downstream alias";
    });
    assert(liveTopicTitle);
    model.servers.back().alias.clear();
    sync();
    assert(liveTopicTitle->text() == "#caption-topic · Renamed downstream");
    model.networks[legacy.name].sojuNetworks[captionChildB.sojuNetworkId].attributes["name"] =
        "Topic catalog rename";
    sync();
    assert(liveTopicTitle->text() == "#caption-topic · Topic catalog rename" &&
           model.activeBuffer == topicId);
    topicBuffer.kind = "server";
    topicBuffer.target = captionChildB.name;
    sync();
    assert(liveTopicTitle->text() == "Server log · Topic catalog rename");
    dialogs.open("topic");
    sync();
    assert(find<Label>(dialogs, [](const Label &label) {
        return label.text() == "Server log · Topic catalog rename";
    }));
    model.servers.back().alias = "Personal downstream alias";
    model.activeBuffer = previousActiveBuffer;
    dialogs.open("server:" + captionChildB.name);
    model.buffers.erase(topicId);
    // A literal alias may equal a generated disambiguation caption: identity is still index/key.
    ServerConfig captionChildC = captionChildA;
    captionChildC.name = "opaque-child-key-C";
    captionChildC.sojuNetworkId = "another-remote-id";
    model.servers.push_back(captionChildC);
    model.networks[account.name].sojuNetworks[captionChildC.sojuNetworkId] = {
        captionChildC.sojuNetworkId, {{"name", remoteCaption}}};
    dialogs.open("server:" + captionChildA.name);
    sync();
    assert(named<Select>(dialogs, "Network")->selectedText() ==
           remoteCaption + " · " + account.alias + " (1)");
    activate(dispatcher, *named<Toggle>(dialogs, "Attach this network locally"));
    assert(model.actions.back().buffer == captionChildA.name);
    named<Select>(dialogs, "Network")->setSelectedIndex(model.servers.size() - 1);
    sync();
    assert(named<Select>(dialogs, "Network")->selectedText() ==
               remoteCaption + " · " + account.alias + " (2)" &&
           named<Select>(dialogs, "Network")->selectedIndex() == model.servers.size() - 1);
    activate(dispatcher, *named<Toggle>(dialogs, "Attach this network locally"));
    assert(model.actions.back().buffer == captionChildC.name);
    ServerConfig literalCaption;
    literalCaption.name = "Literal caption profile";
    literalCaption.identity = model.identities.front().name;
    literalCaption.alias = remoteCaption + " · " + account.alias + " (1)";
    model.servers.push_back(literalCaption);
    dialogs.open("server:" + literalCaption.name);
    sync();
    assert(named<Select>(dialogs, "Network")->selectedIndex() == model.servers.size() - 1 &&
           named<Input>(dialogs, "Network name")->value() == literalCaption.name);
    const auto retiredKey = "Network " + std::to_string(model.servers.size() + 1);
    model.buffers[retiredKey + "/#history"].server = retiredKey;
    press(dispatcher, *button(dialogs, "Add soju account"));
    assert(model.servers.back().sojuAccount && model.servers.back().sasl == "PLAIN" &&
           model.servers.back().name != retiredKey &&
           model.buffers.contains(retiredKey + "/#history"));
    press(dispatcher, *button(dialogs, "Close"));
    assert(!dialogs.active() && model.settings.cursorShape == "box" &&
           model.settings.cursorEffect == "trail" && model.identities[0].nick == "RenamedNick");
    whoisProfileControls();
}
