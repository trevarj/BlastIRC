#include "../src/model.hpp"
#include "../src/preview.hpp"
#include "../src/ui/media_preview.hpp"
#include "preview_fixture.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <core/input/key_modifiers.h>
#include <core/text_clipboard.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <glib.h>
#include <render/codecs/codecs.h>
#include <render/core/texture_manager.h>
#include <render/render_context.h>
#include <render/render_target.h>
#include <render/scene/input_dispatcher.h>
#include <render/text/cairo_text_renderer.h>
#include <string>
#include <ui/controls/button.h>
#include <ui/controls/image.h>
#include <ui/controls/input.h>
#include <ui/controls/label.h>
#include <ui/controls/select_popup_context.h>
#include <ui/controls/slider.h>
#include <webp/encode.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
using namespace blast;
using namespace preview_fixture;

Button *loadButton(Node &node) {
    if (auto *button = dynamic_cast<Button *>(&node);
        button && button->inputArea()->tabFocusKey() == "blastirc.media.load-type")
        return button;
    for (const auto &child : node.children())
        if (auto *button = loadButton(*child))
            return button;
    return nullptr;
}
std::string gifBody() {
    // Same real two-frame fixture as preview_check.cpp: red, then blue at 100 ms.
    constexpr unsigned char bytes[]{
        0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x02, 0x00, 0x01, 0x00, 0x81, 0x00, 0x00, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0xff, 0x0b,
        0x4e, 0x45, 0x54, 0x53, 0x43, 0x41, 0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x00,
        0x00, 0x00, 0x21, 0xf9, 0x04, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00,
        0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x08, 0x05, 0x00, 0x01, 0x00, 0x08, 0x08, 0x00,
        0x21, 0xf9, 0x04, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x02,
        0x00, 0x01, 0x00, 0x81, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x08, 0x05, 0x00, 0x01, 0x00, 0x08, 0x08, 0x00, 0x3b,
    };
    return {reinterpret_cast<const char *>(bytes), sizeof(bytes)};
}
std::string imageBody() {
    constexpr std::array<std::uint8_t, 4> pixel{20, 40, 60, 255};
    std::uint8_t *encoded = nullptr;
    const auto size = WebPEncodeLosslessRGBA(pixel.data(), 1, 1, 4, &encoded);
    assert(size && encoded);
    std::string body(reinterpret_cast<const char *>(encoded), size);
    WebPFree(encoded);
    return body;
}
std::string audioBody() {
    std::string body(44 + 160, '\0');
    auto little = [&](std::size_t offset, unsigned value, unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i)
            body[offset + i] = static_cast<char>(value >> (8 * i));
    };
    body.replace(0, 4, "RIFF");
    little(4, body.size() - 8, 4);
    body.replace(8, 8, "WAVEfmt ");
    little(16, 16, 4);
    little(20, 1, 2); // PCM, mono, 8 kHz, 16-bit samples.
    little(22, 1, 2);
    little(24, 8000, 4);
    little(28, 16000, 4);
    little(32, 2, 2);
    little(34, 16, 2);
    body.replace(36, 4, "data");
    little(40, body.size() - 44, 4);
    return body;
}
void statusDiagnostics(Certificate &certificate, const std::string &body) {
    for (const int status : {403, 404, 429, 500, 206}) {
        Server server(certificate, [&](std::string_view path) {
            assert(path == "/rejected.webp");
            return Reply{body, "Content-Type: image/webp\r\n", status};
        });
        Model model;
        model.settings.previewAllowLocal = true;
        model.settings.imagePreviews = PreviewMode::Automatic;
        Node root;
        MediaPreviewController controller(model, root, certificate.caPath);
        const auto url = server.endpoint + "/rejected.webp";
        assert(controller.openUrl(url));
        until([&] {
            controller.update(800, 600, 1);
            return !controller.error().empty();
        });
        const auto diagnostic = "HTTP " + std::to_string(status);
        assert(controller.error().find(diagnostic) != std::string::npos);
        assert(!controller.ready() && !controller.replacesUrl(0));
        assert(server.requests == 1);
    }
}
void consent(Certificate &certificate) {
    Server server(certificate, [](std::string_view path) {
        assert(path == "/disguised.png");
        return Reply{audioBody(), "Content-Type: audio/wav\r\n", 200, true};
    });
    Model model;
    model.settings.previewAllowLocal = true;
    model.settings.imagePreviews = model.settings.mediaPreviews = PreviewMode::Click;
    Node root;
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&root);
    MediaPreviewController controller(model, root, certificate.caPath);
    controller.setInputDispatcher(dispatcher);
    unsigned changed = 0;
    controller.setChanged([&] { ++changed; });
    const auto url = server.endpoint + "/disguised.png";
    controller.bind(1, url, true);
    assert(server.requests == 0);
    assert(controller.openUrl(url, 1));
    const auto initial = changed;
    until([&] {
        controller.update(800, 600, 1);
        return changed > initial;
    });
    // The server has sent headers only: classification cannot consume the audio body.
    assert(!server.releaseBody && server.requests == 1);
    assert(controller.active() && !controller.ready() && !controller.error().empty());
    auto *button = loadButton(root);
    assert(button && button->visible() && button->enabled());
    assert(button->label() && button->label()->text() == "Load audio");
    for (unsigned i = 0; i < 20; ++i) {
        controller.update(800, 600, 1);
        std::this_thread::sleep_for(2ms);
    }
    assert(server.requests == 1 && !controller.ready());
    dispatcher.setFocus(button->inputArea());
    assert(dispatcher.focusedArea() == button->inputArea());
    // Route through the controller, native dispatcher and the real Button key handler.
    assert(controller.handleKey({.sym = XKB_KEY_Return, .pressed = true}));
    assert(controller.handleKey({.sym = XKB_KEY_Return, .pressed = false}));
    server.releaseBody = true;
    until([&] {
        controller.update(800, 600, 1);
        return controller.ready() || !controller.error().empty();
    });
    assert(server.requests == 2 && controller.ready() && controller.error().empty());
    assert(controller.replacesUrl(0));
    assert(controller.active());
    controller.close();
    assert(!controller.active());
}
// Same upload-only convention as transcript_check.cpp: real layout/decoder,
// capturing RGBA at the texture boundary, not claiming GPU pixel coverage.
class PreviewTextureSink final : public TextureManager {
  public:
    unsigned gifUploads = 0;
    bool rejectUploads = false;
    TextureHandle loadFromRgba(const std::uint8_t *data, int width, int height, bool) override {
        if (rejectUploads)
            return {};
        if (width == 2 && height == 1) {
            constexpr std::array<std::uint8_t, 8> firstFrame{255, 0, 0, 255, 255, 0, 0, 255};
            assert(std::equal(firstFrame.begin(), firstFrame.end(), data));
            ++gifUploads;
        }
        return {.id = TextureId(++uploads),
                .width = width,
                .height = height,
                .sourceWidth = width,
                .sourceHeight = height};
    }
    TextureHandle loadFromRaw(const std::uint8_t *, std::size_t, int, int, int, PixmapFormat,
                              bool) override {
        std::abort();
    }
    TextureHandle loadFromPixels(const std::uint8_t *, int, int, TextureDataFormat, TextureFilter,
                                 bool) override {
        std::abort();
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

  private:
    unsigned uploads = 0;
};
class PreviewImageRenderer final : public Renderer {
  public:
    explicit PreviewImageRenderer(RenderContext &) { text.initialize(nullptr, nullptr); }
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
                                       bool markup) override {
        text.measureCursorStopsWrapped(1.f, value, size, offsets, width, stops, font, false,
                                       markup);
    }
    void measureTextCaretStops(std::string_view value, float size,
                               const std::vector<std::size_t> &offsets, float width,
                               std::vector<TextCursorStop> &stops, const TextFont &font,
                               bool markup) override {
        text.measureCursorStopsWrapped(1.f, value, size, offsets, width, stops, font, true, markup);
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
    PreviewTextureSink textures;

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
void cacheConsumers(Certificate &certificate, const std::string &body) {
    Server server(certificate,
                  [&](std::string_view) { return Reply{body, "Content-Type: image/webp\r\n"}; });
    Model model;
    model.settings.previewAllowLocal = true;
    model.settings.imagePreviews = PreviewMode::Automatic;
    RenderContext context;
    PreviewImageRenderer renderer(context);
    Node root;
    MediaPreviewController controller(model, root, certificate.caPath);
    unsigned changed = 0;
    controller.setChanged([&] { ++changed; });
    assert(controller.openUrl(server.endpoint + "/pinned.png"));
    until([&] {
        controller.update(800, 600, 1);
        return controller.ready() || !controller.error().empty();
    });
    assert(controller.ready() && controller.active() && controller.error().empty());
    auto card = std::make_unique<MediaPreviewCard>(controller);
    auto &visibleCard = *card;
    root.addChild(std::move(card));
    for (std::uint64_t id = 1; id <= 18; ++id) {
        const auto initial = changed;
        controller.bind(id, server.endpoint + "/image-" + std::to_string(id) + ".png", true);
        until([&] {
            controller.update(800, 600, 1);
            return changed > initial;
        });
        assert(controller.ready(id) && controller.error(id).empty());
        assert(controller.active() && controller.ready());
        if (id == 1) {
            visibleCard.bind(1, server.endpoint + "/image-1.png", true);
            visibleCard.sync(renderer, 400, 1);
        }
    }
    assert(server.requests == 19);
    // All consumers remain bound, but old unpinned assets must release the 16-file budget.
    assert(controller.ready(1) && !controller.ready(2));
    assert(controller.ready(18) && controller.error(18).empty());
    assert(controller.ready() && controller.active());
    // Metadata-only Click entries must not evict either active native consumer at the map cap.
    for (std::uint64_t id = 100; id < 400; ++id)
        controller.bind(id, server.endpoint + "/history-" + std::to_string(id), true);
    assert(controller.active() && controller.ready());
    assert(visibleCard.measure(renderer, 400, 1) > 0 && controller.ready(1));
    assert(server.requests == 19);
    root.removeChild(&visibleCard);
}
void unpolledConsumers(Certificate &certificate, const std::string &body) {
    Server server(certificate,
                  [&](std::string_view) { return Reply{body, "Content-Type: image/webp\r\n"}; });
    Model model;
    model.settings.previewAllowLocal = true;
    model.settings.imagePreviews = PreviewMode::Automatic;
    Node root;
    MediaPreviewController controller(model, root, certificate.caPath);
    for (std::uint64_t id = 1; id <= 9; ++id)
        controller.bind(id, server.endpoint + "/burst-" + std::to_string(id) + ".png", true);
    // Completed assets still awaiting UI polling are not also pending reservations.
    until([&] { return server.requests >= 9; });
    controller.bind(10, server.endpoint + "/burst-10.png", true);
    until([&] {
        controller.update(800, 600, 1);
        for (std::uint64_t id = 1; id <= 10; ++id) {
            assert(controller.error(id).empty());
            if (!controller.ready(id))
                return false;
        }
        return true;
    });
    assert(server.requests == 10);
}
void metadataImage(Certificate &certificate, const std::string &body) {
    Server server(certificate, [&](std::string_view path) {
        if (path == "/page.png")
            return Reply{"<meta property='og:image' content='/actual.webp'>"};
        assert(path == "/actual.webp");
        return Reply{body, "Content-Type: image/webp\r\n"};
    });
    Model model;
    model.settings.previewAllowLocal = true;
    model.settings.linkPreviews = model.settings.imagePreviews = PreviewMode::Automatic;
    Node root;
    MediaPreviewController controller(model, root, certificate.caPath);
    controller.bind(1, server.endpoint + "/page.png", true);
    until([&] {
        controller.update(800, 600, 1);
        return controller.ready(1) || !controller.error(1).empty();
    });
    assert(controller.ready(1) && controller.error(1).empty());
    // Image classification, HTML metadata, then a different image URL are legitimate hops.
    assert(server.requests == 3);
}
void unreadableRetirement(Certificate &certificate, const std::string &body) {
    Server server(certificate, [&](std::string_view path) {
        return Reply{body, "Content-Type: image/webp\r\n", 200, path == "/late.png"};
    });
    Model model;
    model.settings.previewAllowLocal = true;
    model.settings.imagePreviews = PreviewMode::Automatic;
    RenderContext context;
    PreviewImageRenderer renderer(context);
    Node root;
    MediaPreviewController controller(model, root, certificate.caPath);
    const auto url = server.endpoint + "/late.png";
    controller.bind(99, url, true, false);
    assert(controller.openUrl(url, 99));
    until([&] { return server.requests == 1; });
    controller.update(800, 600, 1);
    assert(controller.active() && !controller.ready());
    assert(!server.releaseBody && controller.error().empty());
    controller.bind(99, url, false);
    assert(!controller.active() && !controller.ready());
    assert(controller.measure(renderer, 99, 400, 1) == 0);
    server.releaseBody = true;
    // A later successful transfer is a real worker/transport barrier after the canceled body.
    controller.bind(100, server.endpoint + "/barrier.png", true);
    until([&] {
        controller.update(800, 600, 1);
        return controller.ready(100) || !controller.error(100).empty();
    });
    assert(server.requests == 2 && controller.ready(100) && controller.error(100).empty());
    assert(!controller.active() && !controller.ready() && !controller.ready(99));
    assert(controller.measure(renderer, 99, 400, 1) == 0);
}
Button *buttonWithText(Node &node, std::string_view text) {
    if (auto *button = dynamic_cast<Button *>(&node);
        button && button->label() && button->label()->text() == text)
        return button;
    for (const auto &child : node.children())
        if (auto *button = buttonWithText(*child, text))
            return button;
    return nullptr;
}
InputArea *firstArea(Node &node) {
    if (auto *area = dynamic_cast<InputArea *>(&node))
        return area;
    for (const auto &child : node.children())
        if (auto *area = firstArea(*child))
            return area;
    return nullptr;
}
Label *visibleLabelWithText(Node &node, std::string_view text) {
    if (auto *label = dynamic_cast<Label *>(&node);
        label && label->visible() && label->text() == text)
        return label;
    for (const auto &child : node.children())
        if (auto *label = visibleLabelWithText(*child, text))
            return label;
    return nullptr;
}
void checkCaption(Renderer &renderer, MediaPreviewCard &card, std::string_view text,
                  float baseHeight) {
    auto *caption = visibleLabelWithText(card, text);
    assert(caption && caption->width() > 0 && caption->height() > 0);
    const float rowHeight = std::max(24.f, caption->height() + 8);
    const float expectedHeight = baseHeight + rowHeight + 8;
    assert(card.height() == expectedHeight && card.measure(renderer, 400, 1) == expectedHeight);
    float x = 0, y = 0;
    Node::mapToScene(caption, caption->width() / 2, caption->height() / 2, x, y);
    assert(card.containsScenePoint(x, y));
    auto *action = firstArea(card);
    assert(action && action->height() == expectedHeight - 8);
    float captionX = 0, captionY = 0, actionX = 0, actionY = 0;
    Node::mapToScene(caption, 0, 0, captionX, captionY);
    Node::mapToScene(action, 0, 0, actionX, actionY);
    if (!action->containsScenePoint(x, y)) {
        std::fprintf(stderr,
                     "Caption containment: text=%.*s caption=(%g,%g %gx%g) "
                     "action=(%g,%g %gx%g) center=(%g,%g) card=%gx%g\n",
                     static_cast<int>(text.size()), text.data(), captionX, captionY,
                     caption->width(), caption->height(), actionX, actionY, action->width(),
                     action->height(), x, y, card.width(), card.height());
    }
    assert(action->containsScenePoint(x, y));
    assert(captionX >= actionX && captionY >= actionY);
    assert(captionX + caption->width() <= actionX + action->width());
    assert(captionY + caption->height() <= actionY + action->height());
}
unsigned checkSliderBounds(Node &node) {
    if (auto *slider = dynamic_cast<Slider *>(&node)) {
        auto *area = firstArea(*slider);
        assert(area && area->width() > 0 && area->height() > 0);
        return 1;
    }
    unsigned count = 0;
    for (const auto &child : node.children())
        count += checkSliderBounds(*child);
    return count;
}
void pointerControls(Certificate &certificate) {
    Server server(certificate, [](std::string_view path) {
        assert(path == "/pointer.wav");
        return Reply{audioBody(), "Content-Type: audio/wav\r\n", 200, true};
    });
    Model model;
    model.settings.previewAllowLocal = true;
    model.settings.mediaPreviews = PreviewMode::Click;
    RenderContext context;
    ScaledRenderer renderer(context, 1.f);
    Node root;
    root.setSize(800, 600);
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&root);
    MediaPreviewController controller(model, root, certificate.caPath);
    controller.setInputDispatcher(dispatcher);
    unsigned backgroundClicks = 0;
    auto row = std::make_unique<InputArea>();
    row->setPosition(20, 20);
    row->setSize(400, 160);
    row->setOnClick([&](const auto &) { ++backgroundClicks; });
    auto card = std::make_unique<MediaPreviewCard>(controller);
    card->bind(1, server.endpoint + "/pointer.wav", true);
    card->sync(renderer, 400, 1);
    auto *cardNode = card.get();
    row->addChild(std::move(card));
    auto *rowNode = root.addChild(std::move(row));
    controller.update(800, 600, 1);
    root.layout(renderer);
    auto *load = firstArea(*cardNode);
    assert(load && load->width() > 0 && load->height() > 0);
    auto click = [&](Button &button) {
        float x = 0, y = 0;
        Node::mapToScene(button.inputArea(), button.inputArea()->width() / 2,
                         button.inputArea()->height() / 2, x, y);
        assert(dispatcher.inputAreaAt(x, y) == button.inputArea());
        assert(dispatcher.pointerButton(x, y, BTN_LEFT, true, 1, 1, false));
        assert(dispatcher.pointerButton(x, y, BTN_LEFT, false, 2, 2, false));
    };
    float loadX = 0, loadY = 0;
    Node::mapToScene(load, load->width() / 2, load->height() / 2, loadX, loadY);
    assert(cardNode->containsScenePoint(loadX, loadY));
    assert(server.requests == 0);
    assert(dispatcher.pointerButton(loadX, loadY, BTN_LEFT, true, 1, 1, false));
    assert(dispatcher.pointerButton(loadX, loadY, BTN_LEFT, false, 2, 2, false));
    assert(controller.active() && backgroundClicks == 0);
    until([&] { return server.requests == 1; });
    controller.update(800, 600, 1);
    root.layout(renderer);
    auto *close = buttonWithText(root, "Close");
    auto *play = buttonWithText(root, "Play");
    assert(close && play && close->visible() && play->visible());
    for (auto *button : {close, play})
        assert(button->inputArea()->width() > 0 && button->inputArea()->height() > 0);
    assert(checkSliderBounds(root) == 2);
    click(*close);
    assert(!controller.active() && backgroundClicks == 0);
    server.releaseBody = true;
    root.removeChild(rowNode);
}
class PreviewMenu final : public SelectPopupContext {
  public:
    DropdownRequest request;
    DropdownCallbacks callbacks;
    bool opened = false;
    void openSelectDropdown(const DropdownRequest &r, DropdownCallbacks c) override {
        request = r;
        callbacks = std::move(c);
        opened = true;
    }
    void closeSelectDropdown() override { opened = false; }
    bool isSelectDropdownOpen() const override { return opened; }
    void choose(std::string_view label) {
        for (const auto &item : request.menu)
            if (item.label == label && item.enabled) {
                auto selected = callbacks.onSelect;
                closeSelectDropdown();
                selected(item.value);
                return;
            }
        assert(false);
    }
};
class PreviewClipboard final : public TextClipboard {
  public:
    std::string text;
    std::optional<std::string> clipboardText() override { return text; }
    void setClipboardText(std::string value) override { text = std::move(value); }
};
Input *infoInput(Node &node) {
    if (auto *input = dynamic_cast<Input *>(&node);
        input && input->inputArea()->tabFocusKey() == "blastirc.media.info")
        return input;
    for (const auto &child : node.children())
        if (auto *input = infoInput(*child))
            return input;
    return nullptr;
}
Image *previewImage(Node &node) {
    if (auto *image = dynamic_cast<Image *>(&node))
        return image;
    for (const auto &child : node.children())
        if (auto *image = previewImage(*child))
            return image;
    return nullptr;
}
void captionConsumers(Certificate &certificate, const std::string &body) {
    Server server(certificate, [&](std::string_view path) {
        if (path == "/page")
            return Reply{"<title>Real image page title</title>"
                         "<meta property='og:image' content='/actual.webp'>"};
        if (path == "/actual.webp")
            return Reply{body, "Content-Type: image/webp\r\n"};
        if (path == "/audio.wav")
            return Reply{audioBody(), "Content-Type: audio/wav\r\n"};
        assert(path == "/video.y4m");
        // One real 2×2 YUV420 frame; no encoded-title metadata or decoder substitute.
        return Reply{"YUV4MPEG2 W2 H2 F1:1 Ip A1:1 C420jpeg\nFRAME\n" + std::string(4, '\x10') +
                         std::string(2, '\x80'),
                     "Content-Type: video/x-yuv4mpeg\r\n"};
    });
    Model model;
    model.settings.previewAllowLocal = true;
    model.settings.previewMaxHeight = 120;
    model.settings.linkPreviews = model.settings.imagePreviews = model.settings.mediaPreviews =
        PreviewMode::Automatic;
    RenderContext context;
    PreviewImageRenderer renderer(context);
    Node root;
    root.setSize(800, 600);
    MediaPreviewController controller(model, root, certificate.caPath);
    auto card = std::make_unique<MediaPreviewCard>(controller);
    auto *view = card.get();
    root.addChild(std::move(card));
    std::uint64_t id = 0;
    for (const auto path : {"/page", "/audio.wav", "/video.y4m"}) {
        const auto url = server.endpoint + path + "#original";
        view->bind(++id, url, true);
        until([&] {
            controller.update(800, 600, 1);
            return controller.ready(id) || !controller.error(id).empty();
        });
        assert(controller.ready(id) && controller.error(id).empty());
        assert(controller.replacesUrl(id) == (id != 1) && !controller.active());
        view->sync(renderer, 400, 1);
        root.layout(renderer);
        if (id != 1)
            checkCaption(renderer, *view, url, 96);
        if (id == 1) {
            auto *image = previewImage(*view);
            assert(image && image->visible() && image->hasImage());
            assert(view->height() == 128 && view->measure(renderer, 400, 1) == 128);
            assert(!visibleLabelWithText(*view, url));
            assert(!visibleLabelWithText(*view, "Real image page title"));
        }
    }
    root.removeChild(view);
}
void htmlThumbnailConsumers(Certificate &certificate, const std::string &body) {
    // Container preflight accepts this PNG; its empty IDAT cannot decode into pixels.
    std::string undecodable(57, '\0');
    undecodable.replace(0, 8, "\x89PNG\r\n\x1a\n", 8);
    undecodable[11] = 13;
    undecodable.replace(12, 4, "IHDR");
    undecodable[19] = undecodable[23] = 1;
    undecodable[24] = 8;
    undecodable[25] = 6;
    undecodable.replace(37, 4, "IDAT");
    undecodable.replace(49, 4, "IEND");
    assert(PreviewService::raster(
        {reinterpret_cast<const std::uint8_t *>(undecodable.data()), undecodable.size()}));
    for (const std::string_view scenario : {"missing", "pending", "denied", "failed", "undecodable",
                                            "upload", "valid", "suffix", "click"}) {
        const bool missing = scenario == "missing";
        const bool pending = scenario == "pending";
        const bool denied = scenario == "denied";
        const bool click = scenario == "click";
        const bool invalid = scenario == "undecodable";
        const bool valid = scenario == "valid" || scenario == "suffix";
        const bool upload = scenario == "upload";
        Server server(certificate, [&](std::string_view path) {
            if (path == "/page" || path == "/page.png")
                return Reply{missing ? "<title>No thumbnail</title>"
                                     : "<title>Thumbnail page</title>"
                                       "<meta property='og:image' content='/thumb.png'>"};
            assert(path == "/thumb.png");
            if (scenario == "failed")
                return Reply{"Unavailable", "Content-Type: text/plain\r\n", 404};
            return invalid ? Reply{undecodable, "Content-Type: image/png\r\n"}
                           : Reply{body, "Content-Type: image/webp\r\n", 200, pending};
        });
        Model model;
        model.settings.previewAllowLocal = true;
        model.settings.linkPreviews = PreviewMode::Automatic;
        model.settings.imagePreviews = denied  ? PreviewMode::Off
                                       : click ? PreviewMode::Click
                                               : PreviewMode::Automatic;
        RenderContext context;
        PreviewImageRenderer renderer(context);
        Node root;
        root.setSize(800, 600);
        InputDispatcher dispatcher;
        dispatcher.setSceneRoot(&root);
        MediaPreviewController controller(model, root, certificate.caPath);
        controller.setInputDispatcher(dispatcher);
        unsigned changed = 0;
        controller.setChanged([&] { ++changed; });
        auto card = std::make_unique<MediaPreviewCard>(controller);
        auto *view = card.get();
        root.addChild(std::move(card));
        const auto url = server.endpoint + (scenario == "suffix" ? "/page.png" : "/page");
        view->bind(17, url, true);
        auto hidden = [&] {
            assert(controller.measure(renderer, 17, 400, 1) == 0);
            assert(view->measure(renderer, 400, 1) == 0);
            view->sync(renderer, 400, 1);
            root.layout(renderer);
            assert(view->height() == 0 && !view->visible());
            assert(dispatcher.inputAreaAt(10, 10) != firstArea(*view));
            assert(!controller.replacesUrl(17));
        };
        // An image suffix may show its direct-image tile until MIME confirms HTML.
        if (scenario != "suffix")
            hidden();
        until([&] {
            controller.update(800, 600, 1);
            if (pending)
                return server.requests == 2;
            if (missing || denied || click)
                return changed > 0;
            return controller.ready(17) || !controller.error(17).empty();
        });
        const unsigned requests = scenario == "suffix" ? 3 : missing || denied || click ? 1 : 2;
        assert(server.requests == requests && !controller.active());
        if (valid) {
            assert(controller.ready(17) && controller.error(17).empty());
            assert(controller.measure(renderer, 17, 400, 1) > 0);
            view->sync(renderer, 400, 1);
            root.layout(renderer);
            auto *image = previewImage(*view);
            assert(view->visible() && image && image->visible() && image->hasImage());
            assert(image->sourceWidth() == 1 && image->sourceHeight() == 1);
            assert(!visibleLabelWithText(*view, url) && !controller.replacesUrl(17));
        } else if (upload) {
            assert(controller.ready(17) && controller.error(17).empty());
            assert(view->measure(renderer, 400, 1) > 0); // CPU pixels alone are insufficient.
            const auto beforeUpload = changed;
            renderer.textures.rejectUploads = true;
            view->sync(renderer, 400, 1);
            root.layout(renderer);
            assert(view->height() == 0 && !view->visible());
            assert(dispatcher.inputAreaAt(10, 10) != firstArea(*view));
            hidden();
            assert(controller.error(17) == "Image decode failed." && changed == beforeUpload);
            controller.update(800, 600, 1);
            assert(changed == beforeUpload + 1);
        } else {
            if (invalid)
                assert(controller.ready(17) && controller.error(17).empty());
            const auto beforeMeasure = changed;
            hidden();
            if (invalid) {
                assert(controller.error(17) == "Image decode failed.");
                assert(changed == beforeMeasure); // Layout never notifies re-entrantly.
                controller.update(800, 600, 1);
                assert(changed == beforeMeasure + 1);
            }
            if (scenario == "failed")
                assert(controller.error(17).find("HTTP 404") != std::string_view::npos);
        }
        if (pending) {
            assert(!controller.canLoadPreview(17, url));
            assert(!controller.loadPreview(17, url) && !controller.active());
        }
        const auto settledChanges = changed;
        for (unsigned i = 0; i < 12; ++i) {
            view->bind(17, url, true);
            controller.update(800, 600, 1);
            if (!valid)
                hidden();
            else
                assert(view->measure(renderer, 400, 1) > 0 && !controller.replacesUrl(17));
            std::this_thread::sleep_for(2ms);
            assert(server.requests == requests && changed == settledChanges);
        }
        if (click) {
            assert(controller.canLoadPreview(17, url));
            assert(controller.loadPreview(17, url));
            until([&] {
                controller.update(800, 600, 1);
                return controller.ready(17) || !controller.error(17).empty();
            });
            assert(controller.ready(17) && controller.error(17).empty());
            assert(server.requests == 2 && !controller.replacesUrl(17));
            assert(view->measure(renderer, 400, 1) > 0);
            controller.close();
        } else if (!valid && !pending) {
            assert(!controller.canLoadPreview(17, url));
            assert(!controller.loadPreview(17, url) && server.requests == requests);
        }
        server.releaseBody = true;
        model.settings.linkPreviews = model.settings.imagePreviews = PreviewMode::Off;
        controller.update(800, 600, 1);
        view->bind(18, server.endpoint + "/off", true);
        hidden();
        assert(view->measure(renderer, 400, 1) == 0);
        assert(!controller.canLoadPreview(18, server.endpoint + "/off"));
        assert(!controller.loadPreview(18, server.endpoint + "/off"));
        assert(server.requests == requests + (click ? 1 : 0));
        root.removeChild(view);
    }
    // Extensionless URLs need independent consent for Link -> Image MIME classification.
    for (const auto imageMode : {PreviewMode::Click, PreviewMode::Off}) {
        Server server(certificate, [&](std::string_view path) {
            assert(path == "/extensionless");
            return Reply{body, "Content-Type: image/webp\r\n"};
        });
        Model model;
        model.settings.previewAllowLocal = true;
        model.settings.linkPreviews = PreviewMode::Click;
        model.settings.imagePreviews = imageMode;
        RenderContext context;
        PreviewImageRenderer renderer(context);
        Node root;
        root.setSize(800, 600);
        MediaPreviewController controller(model, root, certificate.caPath);
        auto card = std::make_unique<MediaPreviewCard>(controller);
        auto *view = card.get();
        root.addChild(std::move(card));
        const auto url = server.endpoint + "/extensionless";
        view->bind(71, url, true);
        view->sync(renderer, 400, 1);
        assert(!view->visible() && view->height() == 0 && server.requests == 0);
        assert(controller.canLoadPreview(71, url));
        assert(controller.loadPreview(71, url));
        until([&] {
            controller.update(800, 600, 1);
            return !controller.error(71).empty();
        });
        assert(server.requests == 1 && !controller.ready(71) && !controller.replacesUrl(71));
        for (unsigned i = 0; i < 12; ++i) {
            view->bind(71, url, true);
            view->measure(renderer, 400, 1);
            controller.update(800, 600, 1);
            std::this_thread::sleep_for(2ms);
            assert(server.requests == 1 && !controller.ready(71));
        }
        if (imageMode == PreviewMode::Click) {
            assert(controller.canLoadPreview(71, url));
            assert(controller.loadPreview(71, url));
            until([&] {
                controller.update(800, 600, 1);
                return controller.ready(71) || !controller.error(71).empty();
            });
            assert(controller.ready(71) && controller.error(71).empty());
            view->sync(renderer, 400, 1);
            root.layout(renderer);
            auto *image = previewImage(*view);
            assert(image && image->visible() && image->hasImage());
            assert(image->sourceWidth() == 1 && image->sourceHeight() == 1);
            assert(server.requests == 2 && controller.replacesUrl(71));
        } else {
            assert(!controller.canLoadPreview(71, url));
            assert(!controller.loadPreview(71, url));
            assert(view->measure(renderer, 400, 1) == 0 && server.requests == 1);
        }
        controller.close();
        root.removeChild(view);
    }
}
void gifConsumers(Certificate &certificate) {
    const auto body = gifBody();
    Server server(certificate, [&](std::string_view path) {
        assert(path == "/animated.gif");
        return Reply{body, "Content-Type: image/gif\r\n"};
    });
    Model model;
    model.settings.previewAllowLocal = true;
    model.settings.imagePreviews = PreviewMode::Automatic;
    RenderContext context;
    PreviewImageRenderer renderer(context);
    PreviewMenu menu;
    Node root;
    root.setSize(800, 600);
    root.setPopupContext(&menu);
    MediaPreviewController controller(model, root, certificate.caPath);
    auto card = std::make_unique<MediaPreviewCard>(controller);
    auto *view = card.get();
    root.addChild(std::move(card));
    const auto url = server.endpoint + "/animated.gif";
    view->bind(42, url, true);
    until([&] {
        controller.update(800, 600, 1);
        return controller.ready(42) || !controller.error(42).empty();
    });
    assert(controller.ready(42) && controller.error(42).empty());
    view->sync(renderer, 400, 1);
    root.layout(renderer);
    checkCaption(renderer, *view, url, 120);
    auto *thumbnail = previewImage(*view);
    assert(thumbnail && thumbnail->visible() && thumbnail->hasImage());
    assert(thumbnail->sourceWidth() == 2 && thumbnail->sourceHeight() == 1);
    assert(renderer.textures.gifUploads == 1 && controller.replacesUrl(42));
    auto *area = firstArea(*view);
    assert(area);
    area->dispatchKey(XKB_KEY_Return, 0, 0, true);
    controller.update(800, 600, 1);
    root.layout(renderer);
    assert(controller.active() && controller.ready(42) && controller.error(42).empty());
    // Overlay independently decodes the same first frame, not a ready-only placeholder.
    assert(renderer.textures.gifUploads == 2 && server.requests == 1);
    const auto texture = thumbnail->textureId();
    std::this_thread::sleep_for(250ms); // Past the second frame's 100 ms deadline.
    controller.update(800, 600, 1);
    root.layout(renderer);
    assert(renderer.textures.gifUploads == 2 && thumbnail->textureId() == texture);
    controller.close();
    area->dispatchPress(10, 10, BTN_RIGHT, true, 10, 10, 1);
    area->dispatchPress(10, 10, BTN_RIGHT, false, 10, 10, 1);
    assert(menu.opened);
    menu.choose("Info");
    auto *info = infoInput(root);
    assert(info && info->value().starts_with(url));
    assert(info->value().find("Dimensions: 2 × 1") != std::string::npos);
    assert(info->value().find("Preview: static first frame") != std::string::npos);
    controller.close();
    root.removeChild(view);
}
void thumbnailConsumers(Certificate &certificate, const std::string &body) {
    Server server(certificate, [&](std::string_view path) {
        if (path == "/page")
            return Reply{"<title>Real page</title>"
                         "<meta name=\"description\" content=\"Real page description\">"};
        return Reply{body, "Content-Type: image/webp\r\n"};
    });
    Model model;
    model.settings.previewAllowLocal = true;
    model.settings.imagePreviews = model.settings.linkPreviews = PreviewMode::Click;
    RenderContext context;
    PreviewImageRenderer renderer(context);
    PreviewMenu menu;
    PreviewClipboard clipboard;
    auto *previousClipboard = Input::textClipboard();
    Input::setTextClipboard(&clipboard);
    Node root;
    root.setSize(800, 600);
    root.setPopupContext(&menu);
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&root);
    MediaPreviewController controller(model, root, certificate.caPath);
    controller.setInputDispatcher(dispatcher);

    auto card = std::make_unique<MediaPreviewCard>(controller);
    auto *view = card.get();
    root.addChild(std::move(card));
    const auto url = server.endpoint + "/image.webp#original";
    unsigned opened = 0;
    view->setOpenLink([&](std::string original, std::uint64_t message) {
        assert(original == url && message == 42);
        ++opened;
    });
    view->bind(42, url, true);
    view->sync(renderer, 400, 1);
    controller.update(800, 600, 1);
    root.layout(renderer);
    auto *area = firstArea(*view);
    assert(area && area->focusable() && area->tabStop());
    auto rightClick = [&] {
        area->dispatchPress(10, 10, BTN_RIGHT, true, 10, 10, 1);
        area->dispatchPress(10, 10, BTN_RIGHT, false, 10, 10, 1);
        assert(menu.opened);
    };
    rightClick();
    menu.choose("Copy Link");
    assert(clipboard.text == url && server.requests == 0);
    rightClick();
    menu.choose("Open Link");
    assert(opened == 1 && server.requests == 0 && !controller.active());
    rightClick();
    menu.choose("Info");
    auto *info = infoInput(root);
    assert(controller.active() && info && info->value() == url && server.requests == 0);
    info->textInputApplyEdit({.commitText = "forbidden", .preeditText = {}, .hasCommitText = true});
    assert(info->value() == url);
    info->inputArea()->dispatchKey(XKB_KEY_space, ' ', 0, true);
    assert(info->value() == url);
    info->inputArea()->dispatchKey(XKB_KEY_a, 0, KeyMod::Ctrl, true);
    const auto selection = info->textInputState();
    assert(std::abs(selection.cursor - selection.anchor) == static_cast<int>(url.size()));
    info->inputArea()->dispatchKey(XKB_KEY_c, 0, KeyMod::Ctrl, true);
    assert(clipboard.text == url);
    controller.close();
    area->dispatchKey(XKB_KEY_Return, 0, 0, true);
    assert(controller.active());
    until([&] {
        controller.update(800, 600, 1);
        return controller.ready(42) || !controller.error(42).empty();
    });
    assert(controller.ready(42) && controller.replacesUrl(42) && server.requests == 1);
    controller.close();
    view->sync(renderer, 400, 1);
    root.layout(renderer);
    assert(!buttonWithText(*view, "View image") && !buttonWithText(*view, "Open player"));
    checkCaption(renderer, *view, url, 120);
    auto *thumbnail = previewImage(*view);
    assert(thumbnail && thumbnail->visible() && thumbnail->hasImage());
    area->dispatchPress(10, 10, BTN_LEFT, true, 10, 10, 1);
    area->dispatchPress(10, 10, BTN_LEFT, false, 10, 10, 1);
    assert(controller.active() && controller.ready(42) && server.requests == 1);
    controller.close();
    rightClick();
    menu.choose("Info");
    assert(info->value().starts_with(url));
    assert(info->value().find("Type: Image") != std::string::npos);
    assert(info->value().find("Size: " + std::to_string(body.size()) + " bytes") !=
           std::string::npos);
    assert(info->value().find("Dimensions: 1 × 1") != std::string::npos);
    assert(info->value().find("Duration:") == std::string::npos);
    controller.close();
    const auto pageUrl = server.endpoint + "/page";
    const auto pageInfo = pageUrl + "\n\nReal page\nReal page description\n\nType: Web page";
    view->bind(43, pageUrl, true);
    assert(view->measure(renderer, 400, 1) == 0);
    view->sync(renderer, 400, 1);
    root.layout(renderer);
    assert(!view->visible() && view->height() == 0);
    assert(dispatcher.inputAreaAt(10, 10) != area && server.requests == 1);
    assert(controller.canLoadPreview(43, pageUrl));
    assert(controller.loadPreview(43, pageUrl));
    until([&] {
        controller.update(800, 600, 1);
        return info->visible() && info->value() == pageInfo;
    });
    assert(controller.active() && !controller.replacesUrl(43) && server.requests == 2);
    assert(info->visible() && info->value() == pageInfo);
    assert(dispatcher.focusedArea() == info->inputArea());
    info->inputArea()->dispatchKey(XKB_KEY_a, 0, KeyMod::Ctrl, true);
    const auto pageSelection = info->textInputState();
    assert(std::abs(pageSelection.cursor - pageSelection.anchor) ==
           static_cast<int>(pageInfo.size()));
    info->inputArea()->dispatchKey(XKB_KEY_c, 0, KeyMod::Ctrl, true);
    assert(clipboard.text == pageInfo);
    assert(info->value().find("Size:") == std::string::npos);
    assert(info->value().find("Dimensions:") == std::string::npos);
    controller.close();
    view->sync(renderer, 400, 1);
    root.layout(renderer);
    assert(view->measure(renderer, 400, 1) == 0 && view->height() == 0 && !view->visible());
    assert(dispatcher.inputAreaAt(10, 10) != area);
    assert(!visibleLabelWithText(*view, pageUrl));
    assert(!visibleLabelWithText(*view, "Real page"));
    assert(!controller.canLoadPreview(43, pageUrl));
    for (unsigned i = 0; i < 10; ++i) {
        view->bind(43, pageUrl, true);
        view->sync(renderer, 400, 1);
        controller.update(800, 600, 1);
        assert(view->measure(renderer, 400, 1) == 0 && server.requests == 2);
    }
    bool liveReadable = true;
    view->setReadable([&](std::uint64_t message) {
        assert(message == 42);
        return liveReadable;
    });
    for (const auto choice : {"Info", "Copy Link", "Open Link"}) {
        liveReadable = true;
        view->bind(42, url, true);
        view->sync(renderer, 400, 1);
        rightClick();
        const auto previousCopy = clipboard.text;
        const auto previousOpened = opened;
        liveReadable = false;
        menu.choose(choice);
        assert(!controller.active() && clipboard.text == previousCopy && opened == previousOpened);
        assert(view->measure(renderer, 400, 1) == 0 && server.requests == 2);
    }
    // Keyboard and pointer activation must also consult the caller's current policy.
    view->bind(42, url, true);
    area->dispatchKey(XKB_KEY_Return, 0, 0, true);
    assert(!controller.active() && view->measure(renderer, 400, 1) == 0);
    view->bind(42, url, true);
    area->dispatchPress(10, 10, BTN_LEFT, true, 10, 10, 1);
    area->dispatchPress(10, 10, BTN_LEFT, false, 10, 10, 1);
    assert(!controller.active() && view->measure(renderer, 400, 1) == 0 && server.requests == 2);
    liveReadable = true;
    model.settings.imagePreviews = PreviewMode::Off;
    controller.update(800, 600, 1);
    view->bind(42, url, true);
    assert(view->measure(renderer, 400, 1) == 0 && !controller.replacesUrl(42));
    assert(!controller.openUrl(url) && server.requests == 2);
    root.removeChild(view);
    Input::setTextClipboard(previousClipboard);
}
} // namespace

void checkMediaPreview() {
    codecs::installImageDecoder();
    const auto previous = std::signal(SIGPIPE, SIG_IGN);
    TempDirectory directory;
    Certificate certificate(directory.path);
    const auto body = imageBody();
    statusDiagnostics(certificate, body);
    gifConsumers(certificate);
    captionConsumers(certificate, body);
    htmlThumbnailConsumers(certificate, body);
    consent(certificate);
    cacheConsumers(certificate, body);
    unpolledConsumers(certificate, body);
    metadataImage(certificate, body);
    unreadableRetirement(certificate, body);
    pointerControls(certificate);
    thumbnailConsumers(certificate, body);
    std::signal(SIGPIPE, previous);
}
