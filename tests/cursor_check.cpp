#include "../src/ui/irc_format.hpp"
#include <GLES2/gl2.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <core/application_identity.h>
#include <core/input/key_modifiers.h>
#include <cstdio>
#include <limits>
#include <pango/pangocairo.h>
#include <render/animation/animation_manager.h>
#include <render/animation/motion_service.h>
#include <render/gl_shared_context.h>
#include <render/render_context.h>
#include <render/render_target.h>
#include <render/scene/custom_draw_node.h>
#include <render/scene/input_area.h>
#include <render/scene/input_dispatcher.h>
#include <render/scene/rect_node.h>
#include <render/scene/text_node.h>
#include <render/text/cairo_text_renderer.h>
#include <string_view>
#include <thread>
#include <ui/controls/input.h>
#include <ui/controls/input_caret.h>
#include <ui/controls/label.h>
#include <vector>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
bool near(float a, float b) { return std::abs(a - b) < 0.02F; }

void pixelPartitionCheck(wl_display *display, RenderContext &context) {
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
        // Actual per-axis ratios (101/64, 83/48), deliberately not contentScale().
        RenderTarget target;
        target.create(surface, context);
        target.setLogicalSize(64, 48);
        target.resize(101, 83);
        struct Readback final : CustomDrawNode {
            mutable std::vector<std::uint8_t> pixels;
            void draw(RenderBackend &, const CustomDrawContext &) const override {
                pixels.resize(101 * 83 * 4);
                glReadPixels(0, 0, 101, 83, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                assert(glGetError() == GL_NO_ERROR);
            }
        } readback;
        readback.setSize(64, 48);
        readback.setHitTestVisible(false);
        const auto capture = [&](Node &scene) {
            readback.pixels.clear();
            context.renderScene(target, &scene, &readback);
            assert(readback.pixels.size() == 101 * 83 * 4);
            return readback.pixels;
        };

        // The live source outlives both proxy trees. No cloned models or text/AA dependencies.
        Node source;
        source.setSize(64, 48);
        auto fill = std::make_unique<RectNode>();
        RoundedRectStyle style;
        style.fill = rgba(.2F, .4F, .6F, .5F);
        style.noAa = true;
        fill->setStyle(style);
        fill->setSize(64, 48);
        source.addChild(std::move(fill));
        RenderProxyNode reference(&source);
        reference.setSize(64, 48);
        Node partitioned;
        partitioned.setSize(64, 48);
        const std::array<float, 4> xs{0.F, 17.3F, 39.7F, 64.F};
        const std::array<float, 4> ys{0.F, 13.2F, 31.6F, 48.F};
        for (std::size_t row = 0; row + 1 < ys.size(); ++row)
            for (std::size_t column = 0; column + 1 < xs.size(); ++column) {
                auto tile = std::make_unique<Node>();
                tile->setPosition(0, 0);
                tile->setSize(64, 48);
                tile->setClipChildren(true);
                tile->setPixelPartitionClip(xs[column], ys[row], xs[column + 1], ys[row + 1]);
                auto proxy = std::make_unique<RenderProxyNode>(&source);
                proxy->setPosition(0, 0);
                proxy->setSize(64, 48);
                tile->addChild(std::move(proxy));
                partitioned.addChild(std::move(tile));
            }
        for (const auto offset :
             {std::array{.17F, .43F}, std::array{.37F, .11F}, std::array{.71F, .67F}}) {
            reference.setPosition(offset[0], offset[1]);
            partitioned.setPosition(offset[0], offset[1]);
            const auto expected = capture(reference);
            const auto actual = capture(partitioned);
            // Ignore outer round-vs-outward edges, but include every internal seam and crossing.
            for (int y = 4; y < 79; ++y)
                for (int x = 8; x < 94; ++x) {
                    const auto index = (std::size_t(82 - y) * 101 + x) * 4;
                    assert(expected[index] > 0 && expected[index + 3] >= 126 &&
                           expected[index + 3] <= 129);
                    for (std::size_t channel = 0; channel < 4; ++channel) {
                        const int delta = int(actual[index + channel]) - expected[index + channel];
                        if (std::abs(delta) > 1)
                            std::fprintf(stderr,
                                         "pixel partition seam: offset=(%g,%g) pixel=(%d,%d) "
                                         "channel=%zu expected=%u actual=%u\n",
                                         offset[0], offset[1], x, y, channel,
                                         unsigned(expected[index + channel]),
                                         unsigned(actual[index + channel]));
                        assert(std::abs(delta) <= 1);
                    }
                }
        }
        // Consumer rendering: every mode changes pixels without touching native text/IME state.
        AnimationManager effects;
        Input editor;
        editor.setAnimationManager(&effects);
        editor.setFrameVisible(false);
        editor.setFontSize(14);
        editor.setValue("abcd");
        editor.setSize(64, 48);
        InputDispatcher focus;
        focus.setSceneRoot(&editor);
        focus.setFocus(editor.inputArea());
        ScaledRenderer renderer(context, 1.F);
        const bool motion = MotionService::instance().enabled();
        MotionService::instance().setEnabled(true);
        std::array<std::vector<std::uint8_t>, 4> images;
        std::size_t modeIndex = 0;
        for (const auto mode : {Input::CaretEffect::Trail, Input::CaretEffect::Comet,
                                Input::CaretEffect::Ribbon, Input::CaretEffect::Pulse}) {
            auto state = editor.editingState();
            state.current.cursorPos = state.current.selectionAnchor = 1;
            editor.restoreEditingState(state);
            editor.setCaretStyle(Input::CaretShape::Box, mode, 240, .35F);
            editor.measure(renderer, LayoutConstraints::exact(64, 48));
            editor.layout(renderer);
            editor.moveCaretRight();
            const auto moved = editor.editingState();
            const auto ime = editor.textInputState();
            assert(effects.hasActive());
            images[modeIndex] = capture(editor);
            editor.resetCaretEffects();
            const auto plain = capture(editor);
            assert(images[modeIndex] != plain && editor.editingStateMatches(moved));
            assert(editor.textInputState().cursorRectX == ime.cursorRectX);
            Node *viewport = nullptr;
            for (const auto &child : editor.children())
                if (child->clipChildren())
                    viewport = child.get();
            assert(viewport && viewport->clipChildren());
            // Outside the viewport no effect pixel may leak, including the rotated/ring edges.
            for (int y = 0; y < 83; ++y)
                for (int x = 0; x < 101; ++x) {
                    const float lx = (x + .5F) * 64.F / 101.F;
                    const float ly = (y + .5F) * 48.F / 83.F;
                    if (lx >= viewport->x() - 1 && lx <= viewport->x() + viewport->width() + 1 &&
                        ly >= viewport->y() - 1 && ly <= viewport->y() + viewport->height() + 1)
                        continue;
                    const auto pixel = (std::size_t(82 - y) * 101 + x) * 4;
                    for (std::size_t c = 0; c < 4; ++c)
                        assert(images[modeIndex][pixel + c] == plain[pixel + c]);
                }
            for (std::size_t prior = 0; prior < modeIndex; ++prior)
                assert(images[prior] != images[modeIndex]);
            ++modeIndex;
            editor.layout(renderer); // First layout after reset only anchors.
            editor.moveCaretLeft();
            assert(effects.hasActive());
            editor.setPosition(2, 0);
            editor.layout(renderer);
            assert(!effects.hasActive());
            editor.setPosition(0, 0);
            editor.layout(renderer);
            editor.moveCaretRight();
            assert(effects.hasActive());
            editor.textInputApplyEdit({.commitText = {},
                                       .preeditText = "猫",
                                       .preeditCursorBegin = 0,
                                       .preeditCursorEnd = 0,
                                       .hasPreedit = true});
            editor.layout(renderer);
            assert(!effects.hasActive());
            editor.textInputResetPreedit();
            editor.layout(renderer);
            editor.moveCaretLeft();
            assert(effects.hasActive());
            MotionService::instance().setEnabled(false);
            effects.tick(0);
            assert(!effects.hasActive());
            MotionService::instance().setEnabled(true);
            editor.layout(renderer);
            editor.moveCaretRight();
            assert(effects.hasActive());
            focus.setFocus(nullptr);
            assert(!effects.hasActive());
            focus.setFocus(editor.inputArea());
            editor.layout(renderer);
            editor.moveCaretLeft();
            assert(effects.hasActive());
            editor.setCaretStyle(Input::CaretShape::Box, Input::CaretEffect::Off);
            assert(!effects.hasActive());
        }
        for (const bool composing : {false, true}) {
            editor.selectAll();
            if (composing)
                editor.textInputApplyEdit({.commitText = {},
                                           .preeditText = "猫",
                                           .preeditCursorBegin = 0,
                                           .preeditCursorEnd = 0,
                                           .hasPreedit = true});
            editor.layout(renderer);
            const auto preserved = editor.editingState();
            const auto undo = preserved.history->undo, redo = preserved.history->redo;
            editor.setOnContextMenu([&](const InputArea::PointerData *) {
                assert(editor.editingStateMatches(preserved));
                assert(preserved.history->undo == undo && preserved.history->redo == redo);
            });
            float left = 0, top = 0, right = 0, bottom = 0;
            Node::transformedBounds(editor.inputArea(), left, top, right, bottom);
            const float x = (left + right) * .5F, y = (top + bottom) * .5F;
            assert(focus.inputAreaAt(x, y) == editor.inputArea());
            focus.pointerButton(x, y, BTN_RIGHT, true, 400, 1000, false);
            focus.pointerMotion(left + (right - left) * .75F, y, 401);
            focus.pointerButton(x, y, BTN_RIGHT, false, 402, 1010, false);
            assert(editor.editingStateMatches(preserved));
            focus.keyEvent(XKB_KEY_Menu, 0, 0, true);
            assert(editor.editingStateMatches(preserved));
            focus.keyEvent(XKB_KEY_F10, 0, KeyMod::Shift, true);
            assert(editor.editingStateMatches(preserved));
            assert(preserved.history->undo == undo && preserved.history->redo == redo);
            editor.setOnContextMenu({});
            editor.textInputResetPreedit();
        }
        for (const bool pointer : {false, true}) {
            Node menuScene;
            menuScene.setSize(64, 48);
            auto field = std::make_unique<Input>();
            auto *temporary = field.get();
            temporary->setOnContextMenu(
                [&](const InputArea::PointerData *) { menuScene.removeChild(temporary); });
            temporary->measure(renderer, LayoutConstraints::exact(64, 48));
            temporary->setSize(64, 48);
            temporary->layout(renderer);
            menuScene.addChild(std::move(field));
            focus.setSceneRoot(&menuScene);
            focus.setFocus(temporary->inputArea());
            if (pointer) {
                float left = 0, top = 0, right = 0, bottom = 0;
                Node::transformedBounds(temporary->inputArea(), left, top, right, bottom);
                const float x = (left + right) * .5F, y = (top + bottom) * .5F;
                assert(focus.inputAreaAt(x, y) == temporary->inputArea());
                focus.pointerButton(x, y, BTN_RIGHT, true, 403, 1020, false);
                focus.pointerButton(x, y, BTN_RIGHT, false, 404, 1030, false);
            } else
                focus.keyEvent(XKB_KEY_Menu, 0, 0, true);
            assert(menuScene.children().empty());
            assert(focus.focusedArea() == nullptr && !focus.pointerCaptured() &&
                   focus.pendingTouchPressArea() == nullptr);
            focus.setSceneRoot(&editor);
        }
        focus.setFocus(nullptr);
        focus.setSceneRoot(nullptr);
        MotionService::instance().setEnabled(motion);
    }
    wl_surface_destroy(surface);
    wl_compositor_destroy(compositor);
    wl_registry_destroy(registry);
}

// Run with --native inside the same isolated Wayland/EGL environment as the app captures.
void nativeCheck() {
    auto *display = wl_display_connect(nullptr);
    assert(display);
    {
        GlSharedContext shared;
        shared.initialize(display);
        RenderContext context;
        context.initialize(shared);
        pixelPartitionCheck(display, context);
        ScaledRenderer renderer(context, 1.F);
        // The block and inverse glyph use exactly the native cell/layout, never the IME edge.
        const bool initialMotion = MotionService::instance().enabled();
        MotionService::instance().setEnabled(false);
        for (const float scale : {1.F, 1.5F, 2.F}) {
            ScaledRenderer scaled(context, scale);
            Input block;
            block.setMultiline(true);
            block.setFontSize(16);
            block.setFontFamily("monospace");
            block.setValue("Wi猫👩‍💻 á אב");
            block.setCaretStyle(Input::CaretShape::Box, Input::CaretEffect::Off);
            auto state = block.editingState();
            state.current.cursorPos = state.current.selectionAnchor = 0;
            block.restoreEditingState(state);
            InputDispatcher focus;
            focus.setSceneRoot(&block);
            focus.setFocus(block.inputArea());
            const auto bounds = LayoutConstraints::exact(180, 80);
            block.measure(scaled, bounds);
            block.setSize(180, 80);
            for (const auto fill : {rgbHex(0xeeeeee), rgbHex(0x111111)}) {
                block.setTextColors({}, {}, {}, fill);
                block.layout(scaled);
                Node *viewport = nullptr;
                RectNode *caret = nullptr;
                Label *original = nullptr;
                Node *inverse = nullptr;
                for (const auto &child : block.children())
                    if (child->clipChildren()) {
                        viewport = child.get();
                        for (const auto &layer : child->children()) {
                            if (layer->type() == NodeType::Rect && layer->zIndex() == 1)
                                caret = static_cast<RectNode *>(layer.get());
                            if (auto *label = dynamic_cast<Label *>(layer.get()))
                                original = label;
                            if (layer->zIndex() == 2)
                                inverse = layer.get();
                        }
                    }
                assert(viewport && caret && original && inverse && inverse->visible());
                assert(caret->style().fillMode == FillMode::Solid && caret->style().fill == fill);
                assert(inverse->clipChildren() && !inverse->hitTestVisible());
                assert(near(inverse->x(), caret->x()) && near(inverse->y(), caret->y()) &&
                       near(inverse->width(), caret->width()) &&
                       near(inverse->height(), caret->height()));
                assert(caret->x() >= 0 && caret->y() >= 0 &&
                       caret->x() + caret->width() <= viewport->width() + .01F &&
                       caret->y() + caret->height() <= viewport->height() + .01F);
                auto *glyph = dynamic_cast<Label *>(inverse->children().front().get());
                assert(glyph && glyph->text() == original->text() &&
                       glyph->font() == original->font() &&
                       near(glyph->x() + inverse->x(), original->x()) &&
                       near(glyph->y() + inverse->y(), original->y()));
                bool foundInk = false;
                for (const auto &child : glyph->children())
                    if (const auto *ink = dynamic_cast<const TextNode *>(child.get())) {
                        foundInk = true;
                        assert(!ink->useMarkup() &&
                               ink->color() == readableTextColorForBackground(fill));
                    }
                assert(foundInk);
                const auto imeEdge = block.textInputState();
                assert(caret->width() > imeEdge.cursorRectWidth);
                const auto saved = block.editingState();
                block.selectAll();
                assert(!caret->visible() && !inverse->visible());
                block.restoreEditingState(saved);
                block.layout(scaled);
                assert(caret->visible() && inverse->visible());
                block.setCaretStyle(Input::CaretShape::Line, Input::CaretEffect::Off);
                block.layout(scaled);
                assert(!inverse->visible() && block.editingStateMatches(saved));
                const auto lineIme = block.textInputState();
                assert(lineIme.cursorRectWidth == imeEdge.cursorRectWidth &&
                       lineIme.cursorRectHeight == imeEdge.cursorRectHeight);
                block.setCaretStyle(Input::CaretShape::Box, Input::CaretEffect::Off);
            }
            auto end = block.editingState();
            end.current.cursorPos = end.current.selectionAnchor = block.value().size();
            block.restoreEditingState(end);
            block.layout(scaled);
            assert(block.textInputState().cursor ==
                   static_cast<std::int32_t>(block.value().size()));
            for (const auto &viewport : block.children())
                if (viewport->clipChildren())
                    for (const auto &layer : viewport->children())
                        if (layer->zIndex() == 1 && layer->type() == NodeType::Rect)
                            assert(layer->width() > block.textInputState().cursorRectWidth);
            // Plain native fields must reshape the committed row after cancelling preedit too.
            for (const auto &[sym, expected] : {std::pair{XKB_KEY_Home, std::size_t{6}},
                                                std::pair{XKB_KEY_End, std::size_t{12}}}) {
                block.setValue("first\nsecond");
                auto base = block.editingState();
                base.current.cursorPos = base.current.selectionAnchor = 8;
                block.restoreEditingState(base);
                block.textInputApplyEdit({.commitText = {},
                                          .preeditText = "日本😀\nwrapped\n",
                                          .preeditCursorBegin = 10,
                                          .preeditCursorEnd = 10,
                                          .hasPreedit = true});
                block.layout(scaled);
                const auto composing = block.editingState();
                const auto undo = composing.history->undo, redo = composing.history->redo;
                focus.keyEvent(sym, 0, 0, true);
                focus.keyEvent(sym, 0, 0, false);
                assert(!block.composing() && block.value() == "first\nsecond" &&
                       block.cursorPosition() == expected && block.selectionAnchor() == expected &&
                       composing.history->undo == undo && composing.history->redo == redo);
            }
            block.setMultiline(false);
            block.setTextAlign(TextAlign::Center);
            block.setValue("abcXYZ");
            auto centered = block.editingState();
            centered.current.cursorPos = centered.current.selectionAnchor = 4;
            block.restoreEditingState(centered);
            block.layout(scaled);
            const auto committedFour = block.textInputState();
            centered.current.cursorPos = centered.current.selectionAnchor = 2;
            block.restoreEditingState(centered);
            block.layout(scaled);
            const auto committedTwo = block.textInputState();
            block.textInputApplyEdit({.commitText = {},
                                      .preeditText = "WWWWWWWW",
                                      .preeditCursorBegin = 1,
                                      .preeditCursorEnd = 1,
                                      .hasPreedit = true});
            block.layout(scaled);
            const auto painted = block.textInputState();
            const auto composing = block.editingState();
            const auto undo = composing.history->undo, redo = composing.history->redo;
            const auto allocatedX = block.x(), allocatedY = block.y();
            const auto allocatedW = block.width(), allocatedH = block.height();
            const float pressX = painted.cursorRectX;
            const float pressY = painted.cursorRectY + painted.cursorRectHeight / 2;
            assert(focus.pointerButton(pressX, pressY, BTN_LEFT, true, 100, 1000, false));
            const auto cancelled = block.textInputState();
            assert(!block.composing() && block.cursorPosition() == 2 &&
                   cancelled.cursorRectX == committedTwo.cursorRectX &&
                   cancelled.cursorRectY == committedTwo.cursorRectY && block.x() == allocatedX &&
                   block.y() == allocatedY && block.width() == allocatedW &&
                   block.height() == allocatedH);
            focus.pointerMotion(committedFour.cursorRectX,
                                committedFour.cursorRectY + committedFour.cursorRectHeight / 2,
                                101);
            assert(block.cursorPosition() == 4 && block.selectionAnchor() == 2 &&
                   block.value() == "abcXYZ" && composing.history->undo == undo &&
                   composing.history->redo == redo);
            focus.pointerButton(committedFour.cursorRectX,
                                committedFour.cursorRectY + committedFour.cursorRectHeight / 2,
                                BTN_LEFT, false, 102, 1020, false);
            block.setTextAlign(TextAlign::Start);
            block.setMultiline(true);
            block.setSize(180, 80);
            block.setValue("");
            block.layout(scaled);
            for (const auto &viewport : block.children())
                if (viewport->clipChildren())
                    for (const auto &layer : viewport->children())
                        if (layer->zIndex() == 2)
                            assert(!layer->visible());
            std::string longRtl;
            for (int repeat = 0; repeat < 64; ++repeat)
                longRtl += "אב";
            for (const bool clipped : {false, true}) {
                block.setMultiline(!clipped);
                block.setValue(clipped ? longRtl : "אב");
                block.setCaretStyle(Input::CaretShape::Line, Input::CaretEffect::Off);
                block.layout(scaled);
                const auto insertion = block.textInputState();
                const auto document = block.editingState();
                for (const auto shape : {Input::CaretShape::Box, Input::CaretShape::Underline}) {
                    block.setCaretStyle(shape, Input::CaretEffect::Off);
                    block.layout(scaled);
                    const auto shaped = block.textInputState();
                    assert(block.editingStateMatches(document) &&
                           shaped.cursorRectX == insertion.cursorRectX &&
                           shaped.cursorRectWidth == insertion.cursorRectWidth);
                    for (const auto &viewport : block.children())
                        if (viewport->clipChildren())
                            for (const auto &layer : viewport->children())
                                if (layer->zIndex() == 1 && layer->type() == NodeType::Rect) {
                                    float x = 0, y = 0;
                                    Node::absolutePosition(layer.get(), x, y);
                                    assert(layer->visible() && layer->width() > 0 &&
                                           layer->x() >= 0 &&
                                           layer->x() + layer->width() <= viewport->width() + .01F);
                                    if (clipped) {
                                        assert(layer->width() <=
                                               insertion.cursorRectWidth + 1.F / scale);
                                        assert(std::abs(x - insertion.cursorRectX) <= .5F);
                                    } else {
                                        assert(layer->width() > insertion.cursorRectWidth &&
                                               x + layer->width() <= insertion.cursorRectX + .5F);
                                    }
                                }
                }
            }
            focus.setFocus(nullptr);
            focus.setSceneRoot(nullptr);
        }
        // Rich inputs paint and navigate the same decoded native Pango layout; raw IME
        // protocol bytes and history never become markup/display offsets.
        for (const float scale : {1.F, 1.5F, 2.F})
            for (const auto *family : {"sans-serif", "monospace"}) {
                ScaledRenderer scaled(context, scale);
                Input rich;
                rich.setMultiline(true);
                rich.setFontFamily(family);
                rich.setFontSize(20);
                rich.setTextColors(rgbHex(0xffffff), rgbHex(0x225588), rgbHex(0xffffff),
                                   rgbHex(0xeeeeee));
                size_t projectionCalls = 0;
                rich.setDisplayProjector([&](std::string_view raw) {
                    ++projectionCalls;
                    const auto doc =
                        blast::parseIrcFormatting(raw, rgbHex(0xffffff), rgbHex(0x111111));
                    return Input::DisplayProjection{doc.text, blast::ircMarkup(doc),
                                                    blast::ircMarkup(doc, false), doc.rawToDisplay};
                });
                const std::string raw = "\x02"
                                        "Wi\x02 \x03"
                                        "04,02猫\x0f \x1d"
                                        "אב\x1d "
                                        "á 👩‍💻\tZ\nlast";
                rich.setValue(raw);
                rich.setCaretStyle(Input::CaretShape::Box, Input::CaretEffect::Off);
                InputDispatcher focus;
                focus.setSceneRoot(&rich);
                focus.setFocus(rich.inputArea());
                const auto layout = [&] {
                    rich.measure(scaled, LayoutConstraints::exact(360, 140));
                    rich.setSize(360, 140);
                    rich.layout(scaled);
                };
                layout();
                Node *viewport = nullptr, *inverse = nullptr;
                RectNode *caret = nullptr;
                Label *main = nullptr;
                for (const auto &child : rich.children())
                    if (child->clipChildren()) {
                        viewport = child.get();
                        for (const auto &layer : child->children()) {
                            if (auto *label = dynamic_cast<Label *>(layer.get()))
                                main = label;
                            if (layer->type() == NodeType::Rect && layer->zIndex() == 1)
                                caret = static_cast<RectNode *>(layer.get());
                            if (layer->zIndex() == 2)
                                inverse = layer.get();
                        }
                    }
                assert(viewport && main && caret && inverse);
                auto *contrast = dynamic_cast<Label *>(inverse->children().front().get());
                assert(contrast);
                const auto *projection = rich.displayProjection();
                assert(projection && main->text() == projection->markup &&
                       contrast->text() == projection->contrastMarkup &&
                       main->font() == contrast->font() &&
                       !contrast->text().contains("foreground=") &&
                       !contrast->text().contains("background="));
                for (const auto *label : {main, contrast})
                    for (const auto &child : label->children())
                        if (const auto *text = dynamic_cast<TextNode *>(child.get()))
                            assert(text->useMarkup());
                std::vector<size_t> displayOffsets;
                for (size_t offset = 0; offset <= raw.size(); ++offset)
                    if (offset == raw.size() ||
                        (static_cast<unsigned char>(raw[offset]) & 0xc0) != 0x80)
                        displayOffsets.push_back(projection->rawToDisplay[offset]);
                std::vector<TextCursorStop> cells, contrastCells;
                scaled.measureTextCaretStops(main->text(), main->fontSize(), displayOffsets,
                                             main->textWrapWidth(), cells, main->font(), true);
                scaled.measureTextCaretStops(contrast->text(), contrast->fontSize(), displayOffsets,
                                             contrast->textWrapWidth(), contrastCells,
                                             contrast->font(), true);
                assert(cells.size() == displayOffsets.size() &&
                       contrastCells.size() == cells.size());
                const auto metrics = scaled.measureText(main->text(), main->fontSize(),
                                                        main->font(), main->textWrapWidth(), 0,
                                                        TextAlign::Start, TextEllipsize::End, true);
                const auto contrastMetrics = scaled.measureText(
                    contrast->text(), contrast->fontSize(), contrast->font(),
                    contrast->textWrapWidth(), 0, TextAlign::Start, TextEllipsize::End, true);
                assert(near(metrics.width, contrastMetrics.width) &&
                       near(metrics.layoutBaseline, contrastMetrics.layoutBaseline) &&
                       near(metrics.layoutHeight, contrastMetrics.layoutHeight) &&
                       metrics.layoutBaseline > 0 && metrics.layoutHeight > metrics.layoutBaseline);
                size_t index = 0;
                for (size_t offset = 0; offset <= raw.size(); ++offset) {
                    if (offset < raw.size() &&
                        (static_cast<unsigned char>(raw[offset]) & 0xc0) == 0x80)
                        continue;
                    const auto &cell = cells[index], &other = contrastCells[index++];
                    assert(near(cell.x, other.x) && near(cell.y, other.y) &&
                           near(cell.height, other.height) && near(cell.cellX, other.cellX) &&
                           near(cell.cellWidth, other.cellWidth) && cell.cellWidth > 0);
                    auto state = rich.editingState();
                    state.current.cursorPos = state.current.selectionAnchor = offset;
                    rich.restoreEditingState(state);
                    layout();
                    const auto ime = rich.textInputState();
                    assert(ime.surroundingText == raw && ime.cursor == static_cast<int>(offset) &&
                           ime.anchor == static_cast<int>(offset));
                    assert(inverse->visible() && caret->visible() &&
                           near(inverse->x(), caret->x()) && near(inverse->y(), caret->y()) &&
                           near(inverse->width(), caret->width()) &&
                           near(inverse->height(), caret->height()));
                    assert(near(contrast->x() + inverse->x() + contrast->textOriginX(),
                                main->x() + main->textOriginX()) &&
                           near(contrast->y() + inverse->y() + contrast->baselineOffset(),
                                main->y() + main->baselineOffset()));
                    const float originX = main->x() + main->textOriginX() - metrics.alignOffsetX;
                    const float originY = main->y() + main->baselineOffset() -
                                          std::floor(metrics.layoutBaseline * scale) / scale;
                    const float tolerance = 1.01F / scale;
                    assert(std::abs(caret->x() - (originX + cell.cellX)) <= tolerance &&
                           std::abs(caret->y() - (originY + cell.y)) <= tolerance &&
                           std::abs(caret->width() - cell.cellWidth) <= 2 * tolerance &&
                           std::abs(caret->height() - cell.height) <= 2 * tolerance);
                    float caretX = 0, caretY = 0;
                    Node::absolutePosition(viewport, caretX, caretY);
                    assert(std::abs(ime.cursorRectX - (caretX + originX + cell.x)) <= 1.5F &&
                           std::abs(ime.cursorRectY - (caretY + originY + cell.y)) <= 1.5F);
                }
                auto selection = rich.editingState();
                selection.current.selectionAnchor = raw.find("Wi");
                selection.current.cursorPos = raw.size();
                rich.restoreEditingState(selection);
                layout();
                assert(!caret->visible() && !inverse->visible() &&
                       rich.textInputState().cursor == static_cast<int>(raw.size()) &&
                       rich.textInputState().anchor == static_cast<int>(raw.find("Wi")));
                size_t selectionOverlays = 0;
                for (const auto &layer : viewport->children())
                    if (layer->clipChildren() && layer->visible() && layer.get() != inverse)
                        for (const auto &child : layer->children())
                            if (const auto *overlay = dynamic_cast<Label *>(child.get())) {
                                ++selectionOverlays;
                                assert(overlay->text() == contrast->text() &&
                                       overlay->font() == main->font() &&
                                       near(layer->x() + overlay->x() + overlay->textOriginX(),
                                            main->x() + main->textOriginX()) &&
                                       near(layer->y() + overlay->y() + overlay->baselineOffset(),
                                            main->y() + main->baselineOffset()));
                                for (const auto &ink : overlay->children())
                                    if (const auto *text = dynamic_cast<TextNode *>(ink.get()))
                                        assert(text->useMarkup());
                            }
                assert(selectionOverlays >= 2);
                // Preedit protocol state refers to the raw committed document, while the
                // composition caret follows the projected Japanese/emoji bytes.
                auto selected = rich.editingState();
                selected.current.cursorPos = selected.current.selectionAnchor = raw.find("猫");
                rich.restoreEditingState(selected);
                const auto beforeIme = rich.textInputState();
                rich.textInputApplyEdit({.commitText = {},
                                         .preeditText = "日本😀",
                                         .preeditCursorBegin = 10,
                                         .preeditCursorEnd = 10,
                                         .hasPreedit = true});
                layout();
                const auto composing = rich.editingState();
                const auto preeditIme = rich.textInputState();
                assert(composing.preeditBase && composing.preeditBase->value == raw &&
                       composing.preeditStart == selected.current.cursorPos &&
                       composing.preeditLength == 10 && rich.composing());
                assert(preeditIme.surroundingText == raw && preeditIme.cursor == beforeIme.cursor &&
                       preeditIme.anchor == beforeIme.anchor);
                const auto *preeditProjection = rich.displayProjection();
                assert(preeditProjection && preeditProjection->text.contains("日本😀猫"));
                std::vector<TextCursorStop> preeditCell;
                scaled.measureTextCaretStops(
                    preeditProjection->markup, main->fontSize(),
                    {preeditProjection->rawToDisplay[composing.current.cursorPos]},
                    main->textWrapWidth(), preeditCell, main->font(), true);
                assert(preeditCell.size() == 1);
                const auto preeditMetrics = scaled.measureText(
                    main->text(), main->fontSize(), main->font(), main->textWrapWidth(), 0,
                    TextAlign::Start, TextEllipsize::End, true);
                float viewportX = 0, viewportY = 0;
                Node::absolutePosition(viewport, viewportX, viewportY);
                const float preeditY = viewportY + main->y() + main->baselineOffset() -
                                       std::floor(preeditMetrics.layoutBaseline * scale) / scale +
                                       preeditCell[0].y;
                assert(std::abs(preeditIme.cursorRectY - preeditY) <= 1.5F &&
                       std::abs(preeditIme.cursorRectX -
                                (viewportX + main->x() + main->textOriginX() -
                                 preeditMetrics.alignOffsetX + preeditCell[0].x)) <= 1.5F);
                rich.textInputResetPreedit();
                assert(!rich.composing() && rich.value() == raw);
                // Metadata is not a navigation/deletion step; zero-pixel Unicode scalars are.
                const std::string metadata = "a\x03"
                                             "04,02b\x0f"
                                             "c";
                rich.setValue(metadata);
                auto at = rich.editingState();
                at.current.cursorPos = at.current.selectionAnchor = 0;
                rich.restoreEditingState(at);
                layout();
                for (size_t expected = 1; expected <= 3; ++expected) {
                    rich.moveCaretRight();
                    assert(rich.displayProjection()->rawToDisplay[rich.cursorPosition()] ==
                           expected);
                }
                for (size_t expected : {2, 1, 0}) {
                    rich.moveCaretLeft();
                    assert(rich.displayProjection()->rawToDisplay[rich.cursorPosition()] ==
                           expected);
                }
                for (const bool backwards : {false, true}) {
                    rich.setValue(metadata);
                    at = rich.editingState();
                    at.current.cursorPos = at.current.selectionAnchor =
                        metadata.find('b') + (backwards ? 2 : 0);
                    rich.restoreEditingState(at);
                    layout();
                    const auto undoCount = at.history->undo.size();
                    const auto key = backwards ? XKB_KEY_BackSpace : XKB_KEY_Delete;
                    rich.inputArea()->dispatchKey(key, 0, 0, true);
                    rich.inputArea()->dispatchKey(key, 0, 0, false);
                    assert(rich.value() == "a\x03"
                                           "04,02\x0f"
                                           "c" &&
                           rich.displayProjection()->text == "ac" &&
                           rich.editingState().history->undo.size() == undoCount + 1);
                    rich.inputArea()->dispatchKey('z', 0, KeyMod::Ctrl, true);
                    rich.inputArea()->dispatchKey('z', 0, KeyMod::Ctrl, false);
                    assert(rich.value() == metadata);
                }
                rich.setValue("á👩‍💻");
                at = rich.editingState();
                at.current.cursorPos = at.current.selectionAnchor = 0;
                rich.restoreEditingState(at);
                layout();
                for (size_t expected : {1, 3, 7, 10, 14}) {
                    rich.moveCaretRight();
                    assert(rich.cursorPosition() == expected);
                }
                for (size_t expected : {10, 7, 3, 1, 0}) {
                    rich.moveCaretLeft();
                    assert(rich.cursorPosition() == expected);
                }
                // Visual RTL word starts must land after leading metadata, not before its style.
                const std::string boldRtl = "\x02"
                                            "אב\x02";
                rich.setMultiline(false);
                rich.setValue(boldRtl);
                layout();
                const auto rtlDocument =
                    blast::parseIrcFormatting(boldRtl, rgbHex(0xffffff), rgbHex(0x111111));
                const auto &rtlMap = rich.displayProjection()->rawToDisplay;
                assert(rich.displayProjection()->text == "אב" &&
                       rtlMap == rtlDocument.rawToDisplay);
                auto rtlStart = static_cast<size_t>(
                    std::upper_bound(rtlMap.begin(), rtlMap.end(), 0) - rtlMap.begin() - 1);
                // The display map repeats at UTF-8 continuation bytes as well as hidden metadata.
                while (rtlStart > 0 && rtlStart < boldRtl.size() &&
                       (static_cast<unsigned char>(boldRtl[rtlStart]) & 0xc0) == 0x80)
                    --rtlStart;
                const auto rtlEnd = static_cast<size_t>(
                    std::lower_bound(rtlMap.begin(), rtlMap.end(), rtlDocument.text.size()) -
                    rtlMap.begin());
                assert(boldRtl.substr(rtlStart, rtlEnd - rtlStart) == "אב" &&
                       !rtlDocument.styleAt(0).bold && rtlDocument.styleAt(rtlStart).bold &&
                       rtlDocument.styleAt(rtlEnd).bold &&
                       !rtlDocument.styleAt(boldRtl.size()).bold);
                for (int repeat = 0; repeat < 2; ++repeat) {
                    rich.inputArea()->dispatchKey(XKB_KEY_Right, 0, KeyMod::Ctrl, true);
                    rich.inputArea()->dispatchKey(XKB_KEY_Right, 0, KeyMod::Ctrl, false);
                    assert(rich.cursorPosition() == rtlStart &&
                           rich.selectionAnchor() == rtlStart &&
                           rtlDocument.styleAt(rich.cursorPosition()).bold);
                    rich.inputArea()->dispatchKey(XKB_KEY_Left, 0, KeyMod::Ctrl, true);
                    rich.inputArea()->dispatchKey(XKB_KEY_Left, 0, KeyMod::Ctrl, false);
                    assert(rich.cursorPosition() == rtlEnd && rich.selectionAnchor() == rtlEnd &&
                           rtlDocument.styleAt(rich.cursorPosition()).bold);
                }
                rich.setMultiline(true);
                // Home/End follow native rows, not font-run caret tops.
                const auto rowKey = [&](std::uint32_t sym) {
                    focus.keyEvent(sym, 0, 0, true);
                    focus.keyEvent(sym, 0, 0, false);
                    assert(rich.cursorPosition() <= rich.value().size() &&
                           rich.selectionAnchor() == rich.cursorPosition());
                    const auto ime = rich.textInputState();
                    assert(ime.cursor == static_cast<int>(rich.cursorPosition()) &&
                           ime.anchor == ime.cursor);
                };
                const std::string mixedRow = "A\x02猫\x02 😀 \x1dZ\x1d tail";
                for (const auto &text :
                     {mixedRow, mixedRow + "\nsecond", mixedRow + std::string(80, 'W')}) {
                    rich.setValue(text);
                    layout();
                    const auto *projection = rich.displayProjection();
                    std::vector<std::size_t> offsets;
                    for (std::size_t byte = 0; byte <= text.size(); ++byte)
                        if (byte == text.size() ||
                            (static_cast<unsigned char>(text[byte]) & 0xc0) != 0x80)
                            offsets.push_back(byte);
                    std::vector<std::size_t> displayed;
                    for (const auto byte : offsets)
                        displayed.push_back(projection->rawToDisplay[byte]);
                    std::vector<TextCursorStop> rows;
                    scaled.measureTextCaretStops(main->text(), main->fontSize(), displayed,
                                                 main->textWrapWidth(), rows, main->font(), true);
                    assert(rows.size() == offsets.size() && rows.front().lineHeight > 0);
                    if (text == mixedRow)
                        assert(rows.back().lineIndex == 0);
                    else
                        assert(rows.back().lineIndex > 0);
                    rowKey(XKB_KEY_Home);
                    const auto lastRow = rows.back().lineIndex;
                    const auto first = std::find_if(rows.begin(), rows.end(), [&](const auto &row) {
                        return row.lineIndex == lastRow;
                    });
                    assert(rich.cursorPosition() == offsets[first - rows.begin()]);
                    rowKey(XKB_KEY_End);
                    assert(rich.cursorPosition() == text.size());
                    if (lastRow > 0) {
                        rowKey(XKB_KEY_Up);
                        const auto current =
                            std::lower_bound(offsets.begin(), offsets.end(), rich.cursorPosition());
                        assert(current != offsets.end() &&
                               rows[current - offsets.begin()].lineIndex == lastRow - 1);
                        rowKey(XKB_KEY_Home);
                        const auto previous =
                            std::find_if(rows.begin(), rows.end(), [&](const auto &row) {
                                return row.lineIndex == lastRow - 1;
                            });
                        assert(rich.cursorPosition() == offsets[previous - rows.begin()]);
                        rowKey(XKB_KEY_End);
                        const auto end = std::find_if(previous, rows.end(), [&](const auto &row) {
                            return row.lineIndex == lastRow;
                        });
                        const auto boundary = static_cast<std::size_t>(end - rows.begin());
                        const auto previousByte = offsets[boundary - 1];
                        const auto expected =
                            text[previousByte] == '\n' ? previousByte : offsets[boundary];
                        assert(rich.cursorPosition() == expected);
                    }
                }
                const auto committedRows = mixedRow + "\nsecond";
                for (const auto &[sym, expected] : {std::pair{XKB_KEY_Home, mixedRow.size() + 1},
                                                    std::pair{XKB_KEY_End, committedRows.size()}}) {
                    rich.setValue(committedRows);
                    auto base = rich.editingState();
                    base.current.cursorPos = base.current.selectionAnchor = mixedRow.size() + 3;
                    rich.restoreEditingState(base);
                    rich.textInputApplyEdit({.commitText = {},
                                             .preeditText = "日本😀\nwrapped\n",
                                             .preeditCursorBegin = 10,
                                             .preeditCursorEnd = 10,
                                             .hasPreedit = true});
                    layout();
                    const auto composing = rich.editingState();
                    const auto undo = composing.history->undo, redo = composing.history->redo;
                    rowKey(sym);
                    assert(!rich.composing() && rich.value() == committedRows &&
                           rich.cursorPosition() == expected && composing.history->undo == undo &&
                           composing.history->redo == redo);
                }
                {
                    rich.setValue("\x02"
                                  "first\x02\nsecond");
                    auto base = rich.editingState();
                    base.current.cursorPos = base.current.selectionAnchor = 4;
                    rich.restoreEditingState(base);
                    layout();
                    const auto committedFour = rich.textInputState();
                    base.current.cursorPos = base.current.selectionAnchor = 3;
                    rich.restoreEditingState(base);
                    layout();
                    const auto committedThree = rich.textInputState();
                    rich.textInputApplyEdit({.commitText = {},
                                             .preeditText = "日本😀\nwrapped",
                                             .preeditCursorBegin = 3,
                                             .preeditCursorEnd = 3,
                                             .hasPreedit = true});
                    layout();
                    const auto painted = rich.textInputState();
                    const auto composing = rich.editingState();
                    const auto undo = composing.history->undo, redo = composing.history->redo;
                    const auto allocatedX = rich.x(), allocatedY = rich.y();
                    const auto allocatedW = rich.width(), allocatedH = rich.height();
                    const float pressX = painted.cursorRectX;
                    const float pressY = painted.cursorRectY + painted.cursorRectHeight / 2;
                    assert(focus.pointerButton(pressX, pressY, BTN_LEFT, true, 100, 1000, false));
                    const auto cancelled = rich.textInputState();
                    assert(!rich.composing() && rich.cursorPosition() == 3 &&
                           cancelled.cursorRectX == committedThree.cursorRectX &&
                           cancelled.cursorRectY == committedThree.cursorRectY &&
                           rich.x() == allocatedX && rich.y() == allocatedY &&
                           rich.width() == allocatedW && rich.height() == allocatedH);
                    focus.pointerMotion(
                        committedFour.cursorRectX,
                        committedFour.cursorRectY + committedFour.cursorRectHeight / 2, 101);
                    assert(rich.cursorPosition() == 4 && rich.selectionAnchor() == 3 &&
                           composing.history->undo == undo && composing.history->redo == redo);
                    focus.pointerButton(committedFour.cursorRectX,
                                        committedFour.cursorRectY +
                                            committedFour.cursorRectHeight / 2,
                                        BTN_LEFT, false, 102, 1020, false);
                }
                {
                    rich.setValue("\x02"
                                  "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda"
                                  "\x02");
                    layout();
                    const auto &display = rich.displayProjection()->text;
                    std::vector<std::size_t> offsets;
                    for (std::size_t byte = 0; byte <= display.size(); ++byte)
                        offsets.push_back(byte);
                    std::vector<TextCursorStop> native;
                    scaled.measureTextCaretStops(main->text(), main->fontSize(), offsets,
                                                 main->textWrapWidth(), native, main->font(), true);
                    std::size_t space = 0;
                    while (space + 1 < native.size() &&
                           !(display[space] == ' ' &&
                             (native[space + 1].lineIndex != native[space].lineIndex ||
                              native[space + 1].x == native[space].x)))
                        ++space;
                    assert(space > 0 && space < display.size() && native[space].cellWidth > 0 &&
                           native[space].cellIsVirtual && display[space - 1] != ' ');
                    const auto paintedSelections = [&] {
                        std::size_t count = 0;
                        for (const auto &layer : viewport->children())
                            if (layer->type() == NodeType::Rect &&
                                static_cast<const RectNode *>(layer.get())->style().fill ==
                                    rgbHex(0x225588) &&
                                layer->visible() && layer->width() > 0 && layer->height() > 0)
                                ++count;
                        return count;
                    };
                    auto selection = rich.editingState();
                    // The leading bold token is one raw byte, with no display cell.
                    selection.current.cursorPos = selection.current.selectionAnchor = space + 1;
                    rich.restoreEditingState(selection);
                    layout();
                    assert(caret->visible() && inverse->visible() && caret->width() > 0);
                    selection.current.cursorPos = space + 2;
                    rich.restoreEditingState(selection);
                    layout();
                    assert(paintedSelections() == 0);
                    selection.current.selectionAnchor = space;
                    selection.current.cursorPos = space + 1;
                    rich.restoreEditingState(selection);
                    layout();
                    assert(paintedSelections() > 0);
                    for (const auto &text : {std::string{"\x02"
                                                         "a\nb"
                                                         "\x02"},
                                             std::string{"\x02"
                                                         "a ́b"
                                                         "\x02"}}) {
                        rich.setValue(text);
                        auto selected = rich.editingState();
                        selected.current.selectionAnchor = 2;
                        selected.current.cursorPos =
                            3; // Hard newline or the combining cluster's space.
                        rich.restoreEditingState(selected);
                        layout();
                        assert(paintedSelections() > 0);
                    }
                }
                const std::string coloredWord = "\x03"
                                                "04,02hello\x0f";
                rich.setValue(coloredWord);
                layout();
                const auto wordDocument =
                    blast::parseIrcFormatting(coloredWord, rgbHex(0xffffff), rgbHex(0x111111));
                const auto &wordMap = rich.displayProjection()->rawToDisplay;
                assert(rich.displayProjection()->text == "hello" &&
                       wordMap == wordDocument.rawToDisplay);
                // Start after the color token, end before the reset: both retain the word's style.
                const auto wordStart = static_cast<size_t>(
                    std::upper_bound(wordMap.begin(), wordMap.end(), 0) - wordMap.begin() - 1);
                const auto wordEnd = static_cast<size_t>(
                    std::lower_bound(wordMap.begin(), wordMap.end(), wordDocument.text.size()) -
                    wordMap.begin());
                assert(coloredWord.substr(wordStart, wordEnd - wordStart) == "hello" &&
                       wordEnd < coloredWord.size() && coloredWord[wordEnd] == '\x0f' &&
                       wordDocument.styleAt(wordStart) == wordDocument.styleAt(wordEnd) &&
                       wordDocument.styleAt(wordEnd).foreground &&
                       wordDocument.styleAt(wordEnd).background &&
                       !wordDocument.styleAt(coloredWord.size()).foreground &&
                       !wordDocument.styleAt(coloredWord.size()).background);
                assert(rich.projectedWordBoundary(coloredWord.size(), false) == wordStart &&
                       rich.projectedWordBoundary(wordStart, true) == wordEnd);
                rich.inputArea()->dispatchKey(XKB_KEY_Left, 0, KeyMod::Ctrl, true);
                rich.inputArea()->dispatchKey(XKB_KEY_Left, 0, KeyMod::Ctrl, false);
                assert(rich.cursorPosition() == wordStart && rich.selectionAnchor() == wordStart);
                rich.inputArea()->dispatchKey(XKB_KEY_Right, 0, KeyMod::Ctrl, true);
                rich.inputArea()->dispatchKey(XKB_KEY_Right, 0, KeyMod::Ctrl, false);
                assert(rich.cursorPosition() == wordEnd && rich.selectionAnchor() == wordEnd);
                // Repeating Ctrl+Right must not introduce an invisible step over the reset.
                rich.inputArea()->dispatchKey(XKB_KEY_Right, 0, KeyMod::Ctrl, true);
                rich.inputArea()->dispatchKey(XKB_KEY_Right, 0, KeyMod::Ctrl, false);
                assert(rich.cursorPosition() == wordEnd && rich.selectionAnchor() == wordEnd);
                std::vector<TextCursorStop> wordCell;
                scaled.measureTextCaretStops(main->text(), main->fontSize(), {0},
                                             main->textWrapWidth(), wordCell, main->font(), true);
                assert(wordCell.size() == 1);
                const auto wordMetrics = scaled.measureText(
                    main->text(), main->fontSize(), main->font(), main->textWrapWidth(), 0,
                    TextAlign::Start, TextEllipsize::End, true);
                Node::absolutePosition(viewport, viewportX, viewportY);
                const float wordX = viewportX + main->x() + main->textOriginX() -
                                    wordMetrics.alignOffsetX + wordCell[0].cellX +
                                    wordCell[0].cellWidth / 2;
                const float wordY = viewportY + main->y() + main->baselineOffset() -
                                    std::floor(wordMetrics.layoutBaseline * scale) / scale +
                                    wordCell[0].y + wordCell[0].height / 2;
                assert(focus.inputAreaAt(wordX, wordY) == rich.inputArea());
                std::uint32_t serial = 10;
                for (int click = 0; click < 2; ++click)
                    for (const bool pressed : {true, false}) {
                        ++serial;
                        assert(focus.pointerButton(wordX, wordY, BTN_LEFT, pressed, serial,
                                                   1000 + serial * 10, false));
                    }
                assert(std::min(rich.cursorPosition(), rich.selectionAnchor()) == wordStart &&
                       std::max(rich.cursorPosition(), rich.selectionAnchor()) == wordEnd);
                // An empty insertion is a real selected deletion, and must pass validation.
                const auto rejected = rich.editingState();
                const auto rejectedUndo = rejected.history->undo,
                           rejectedRedo = rejected.history->redo;
                size_t validatorCalls = 0;
                rich.setEditValidator(
                    [&](std::string_view before, std::string_view after, bool preedit) {
                        ++validatorCalls;
                        assert(!preedit && before == coloredWord &&
                               after == "\x03"
                                        "04,02\x0f");
                        return false;
                    });
                assert(!rich.insertText("") && validatorCalls == 1 &&
                       rich.editingStateMatches(rejected) &&
                       rejected.history->undo == rejectedUndo &&
                       rejected.history->redo == rejectedRedo);
                rich.setEditValidator({});
                size_t committedChanges = 0;
                rich.setOnChange([&](const std::string &) { ++committedChanges; });
                rich.setValue("\x02"
                              "a\x02");
                auto base = rich.editingState();
                base.current.cursorPos = base.current.selectionAnchor = 1;
                rich.restoreEditingState(base);
                std::string tallPreedit;
                for (int line = 0; line < 16; ++line)
                    tallPreedit += "日本😀\n";
                const auto tail = static_cast<int>(tallPreedit.size());
                rich.textInputApplyEdit({.commitText = {},
                                         .preeditText = tallPreedit,
                                         .preeditCursorBegin = tail,
                                         .preeditCursorEnd = tail,
                                         .hasPreedit = true});
                layout();
                const auto tall = rich.editingState();
                const auto tallUndo = tall.history->undo, tallRedo = tall.history->redo;
                const auto parses = projectionCalls;
                assert(tall.preeditBase && tall.scrollY > 0 && committedChanges == 0);
                rich.textInputApplyEdit({.commitText = {},
                                         .preeditText = tallPreedit,
                                         .preeditCursorBegin = 0,
                                         .preeditCursorEnd = 3,
                                         .hasPreedit = true});
                assert(rich.editingState().scrollY == 0);
                layout();
                auto movedPreedit = rich.editingState();
                assert(movedPreedit.current.value == tall.current.value &&
                       movedPreedit.current.cursorPos == 1 &&
                       movedPreedit.current.selectionAnchor == 4 &&
                       movedPreedit.preeditBase == tall.preeditBase &&
                       movedPreedit.history == tall.history &&
                       movedPreedit.history->undo == tallUndo &&
                       movedPreedit.history->redo == tallRedo && projectionCalls == parses &&
                       committedChanges == 0 && !caret->visible());
                bool selectedPreedit = false;
                for (const auto &layer : viewport->children())
                    selectedPreedit |= layer.get() != inverse && layer->clipChildren() &&
                                       layer->visible() && !layer->children().empty();
                assert(selectedPreedit);
                rich.textInputApplyEdit({.commitText = {},
                                         .preeditText = tallPreedit,
                                         .preeditCursorBegin = -1,
                                         .preeditCursorEnd = -1,
                                         .hasPreedit = true});
                layout();
                assert(rich.editingState().preeditCaretHidden && !caret->visible() &&
                       !inverse->visible() && projectionCalls == parses && committedChanges == 0);
                rich.textInputApplyEdit({.commitText = {},
                                         .preeditText = tallPreedit,
                                         .preeditCursorBegin = 0,
                                         .preeditCursorEnd = 0,
                                         .hasPreedit = true});
                layout();
                movedPreedit = rich.editingState();
                assert(!movedPreedit.preeditCaretHidden && movedPreedit.scrollY == 0 &&
                       caret->visible() && inverse->visible() && projectionCalls == parses &&
                       movedPreedit.preeditBase == tall.preeditBase &&
                       movedPreedit.history->undo == tallUndo &&
                       movedPreedit.history->redo == tallRedo &&
                       rich.textInputState().surroundingText == base.current.value &&
                       committedChanges == 0);
                rich.textInputResetPreedit();
                // Cancel the displayed preedit before computing a decoded word range.
                for (const bool deleting : {false, true}) {
                    rich.setValue("a b");
                    auto committed = rich.editingState();
                    committed.current.cursorPos = committed.current.selectionAnchor = 1;
                    rich.restoreEditingState(committed);
                    rich.textInputApplyEdit({.commitText = {},
                                             .preeditText = "z",
                                             .preeditCursorBegin = 1,
                                             .preeditCursorEnd = 1,
                                             .hasPreedit = true});
                    layout();
                    assert(rich.value() == "az b" && rich.composing());
                    const auto beforeCancelParses = projectionCalls;
                    const auto key = deleting ? XKB_KEY_Delete : XKB_KEY_Left;
                    const auto modifiers = deleting ? KeyMod::Ctrl : 0;
                    rich.inputArea()->dispatchKey(key, 0, modifiers, true);
                    rich.inputArea()->dispatchKey(key, 0, modifiers, false);
                    layout();
                    assert(!rich.composing() && rich.value() == (deleting ? "a" : "a b") &&
                           rich.cursorPosition() == (deleting ? 1 : 0) &&
                           projectionCalls == beforeCancelParses + (deleting ? 2 : 1));
                    if (deleting) {
                        rich.inputArea()->dispatchKey('z', 0, KeyMod::Ctrl, true);
                        rich.inputArea()->dispatchKey('z', 0, KeyMod::Ctrl, false);
                        assert(rich.value() == "a b" && !rich.composing());
                    }
                }
                rich.setMultiline(false);
                rich.setValue("אב");
                layout();
                rich.textInputApplyEdit({.commitText = {},
                                         .preeditText = "z",
                                         .preeditCursorBegin = 1,
                                         .preeditCursorEnd = 1,
                                         .hasPreedit = true});
                layout();
                rich.inputArea()->dispatchKey(XKB_KEY_Right, 0, 0, true);
                rich.inputArea()->dispatchKey(XKB_KEY_Right, 0, 0, false);
                assert(!rich.composing() && rich.value() == "אב" && rich.cursorPosition() == 2 &&
                       rich.selectionAnchor() == 2);
                layout(); // Right from the RTL end slot uses committed shaping, not raw +1.
                assert(rich.cursorPosition() == 2);
                for (const bool hidden : {false, true}) {
                    rich.setValue("a");
                    rich.textInputApplyEdit({.commitText = {},
                                             .preeditText = "猫猫",
                                             .preeditCursorBegin = hidden ? -1 : 0,
                                             .preeditCursorEnd = hidden ? -1 : 3,
                                             .hasPreedit = true});
                    layout();
                    const auto held = rich.editingState();
                    const auto heldUndo = held.history->undo, heldRedo = held.history->redo;
                    size_t rejectedKeys = 0;
                    rich.setEditValidator([&](std::string_view, std::string_view, bool) {
                        ++rejectedKeys;
                        return false;
                    });
                    rich.inputArea()->dispatchKey('q', 'q', KeyMod::Alt, true);
                    rich.inputArea()->dispatchKey('q', 'q', KeyMod::Alt, false);
                    assert(rejectedKeys == 1 && rich.editingStateMatches(held) &&
                           held.history->undo == heldUndo && held.history->redo == heldRedo &&
                           rich.composing() && rich.editingState().preeditCaretHidden == hidden);
                    rich.setEditValidator({});
                    rich.textInputResetPreedit();
                    assert(rich.value() == "a" && !rich.composing());
                }
                focus.setFocus(nullptr);
                focus.setSceneRoot(nullptr);
            }
        MotionService::instance().setEnabled(initialMotion);
        AnimationManager animations;
        Input input;
        input.setAnimationManager(&animations);
        input.setMultiline(true);
        input.setValue(
            "Variable width Wiii 猫 👩‍💻 words wrap across several lines without "
            "another text engine.");
        auto constraints = LayoutConstraints::unconstrained();
        constraints.setExactWidth(480);
        input.measure(renderer, constraints);
        input.layout(renderer);
        const float wide = input.measuredMultilineHeight();
        const auto document = input.editingState();
        const auto ime = input.textInputState();
        constraints.setExactWidth(140);
        input.measure(renderer, constraints);
        const float narrow = input.measuredMultilineHeight();
        assert(narrow > wide && input.editingStateMatches(document));
        const auto afterMeasure = input.textInputState();
        assert(afterMeasure.cursor == ime.cursor && afterMeasure.anchor == ime.anchor);
        assert(afterMeasure.cursorRectX == ime.cursorRectX &&
               afterMeasure.cursorRectY == ime.cursorRectY);
        assert(afterMeasure.cursorRectWidth == ime.cursorRectWidth &&
               afterMeasure.cursorRectHeight == ime.cursorRectHeight);
        input.setFontSize(24);
        input.measure(renderer, constraints);
        assert(input.measuredMultilineHeight() > narrow && input.editingStateMatches(document));
        // A body-font control height can be shorter than this font's padded native row.
        // Measure the real empty control so the first move tests motion, not scroll rebasing.
        Input emptyRow;
        emptyRow.setMultiline(true);
        emptyRow.setFontSize(24);
        emptyRow.measure(renderer, constraints);
        input.setSize(input.width(), emptyRow.measuredMultilineHeight());
        InputDispatcher dispatcher;
        dispatcher.setSceneRoot(&input);
        dispatcher.setFocus(input.inputArea());
        input.layout(renderer);
        const bool motion = MotionService::instance().enabled();
        MotionService::instance().setEnabled(true);
        input.setCaretStyle(Input::CaretShape::Box, Input::CaretEffect::Trail, 40, .20F);
        input.measure(renderer, constraints);
        input.layout(renderer);
        // Retain the non-interactive trail pool before selection adds unrelated rectangles.
        std::array<const RectNode *, input_caret::Trail::count> trailNodes{};
        Node *trailViewport = nullptr;
        std::size_t captured = 0;
        for (const auto &viewport : input.children())
            if (viewport->clipChildren())
                for (const auto &layer : viewport->children()) {
                    if (layer->type() != NodeType::Rect || layer->zIndex() != 0)
                        continue;
                    const auto *rect = static_cast<const RectNode *>(layer.get());
                    if (rect->hitTestVisible())
                        continue;
                    assert(captured < trailNodes.size());
                    trailNodes[captured++] = rect;
                    trailViewport = viewport.get();
                }
        assert(captured == trailNodes.size() && trailViewport);
        auto stamps = [&] {
            std::size_t count = 0;
            for (const auto *layer : trailNodes) {
                assert(!layer->hitTestVisible());
                if (!layer->visible())
                    continue;
                ++count;
                assert(layer->x() >= -.01F && layer->y() >= -.01F);
                assert(layer->x() + layer->width() <= trailViewport->width() + .01F);
                assert(layer->y() + layer->height() <= trailViewport->height() + .01F);
                float x = 0, y = 0;
                Node::mapToScene(trailViewport, layer->x() + layer->width() / 2,
                                 layer->y() + layer->height() / 2, x, y);
                assert(Node::hitTest(&input, x, y) == input.inputArea());
            }
            return count;
        };
        const auto beforeMove = input.textInputState();
        const auto beforeState = input.editingState();
        LayoutRect beforeCaret{};
        for (const auto &viewport : input.children())
            if (viewport->clipChildren())
                for (const auto &layer : viewport->children())
                    if (layer->type() == NodeType::Rect && layer->zIndex() == 1)
                        beforeCaret = {layer->x(), layer->y(), layer->width(), layer->height()};
        input.moveCaretLeft();
        const auto moved = input.textInputState();
        assert(moved.cursor < beforeMove.cursor &&
               moved.cursorRectWidth == beforeMove.cursorRectWidth);
        assert(moved.cursorRectX != beforeMove.cursorRectX ||
               moved.cursorRectY != beforeMove.cursorRectY);
        if (!animations.hasActive() || stamps() == 0) {
            const auto afterState = input.editingState();
            std::fprintf(stderr,
                         "native trail failure: active=%d stamps=%zu focus=%d motion=%d speed=%g "
                         "manager=%p expected=%p history=%p/%p cursor=%d/%d scroll=(%g,%g)/(%g,%g) "
                         "beforeCaret=(%g,%g,%g,%g) IME=(%d,%d,%d,%d)/(%d,%d,%d,%d)\n",
                         animations.hasActive(), stamps(), input.inputArea()->focused(),
                         MotionService::instance().enabled(), MotionService::instance().speed(),
                         static_cast<void *>(input.animationManager()),
                         static_cast<void *>(&animations),
                         static_cast<void *>(beforeState.history.get()),
                         static_cast<void *>(afterState.history.get()),
                         static_cast<int>(beforeMove.cursor), static_cast<int>(moved.cursor),
                         beforeState.scrollX, beforeState.scrollY, afterState.scrollX,
                         afterState.scrollY, beforeCaret.x, beforeCaret.y, beforeCaret.width,
                         beforeCaret.height, beforeMove.cursorRectX, beforeMove.cursorRectY,
                         beforeMove.cursorRectWidth, beforeMove.cursorRectHeight, moved.cursorRectX,
                         moved.cursorRectY, moved.cursorRectWidth, moved.cursorRectHeight);
            for (const auto &viewport : input.children()) {
                if (!viewport->clipChildren())
                    continue;
                float x = 0, y = 0;
                Node::absolutePosition(viewport.get(), x, y);
                std::fprintf(stderr, "viewport scene=(%g,%g,%g,%g) visible=%d\n", x, y,
                             viewport->width(), viewport->height(), viewport->visible());
                for (const auto &layer : viewport->children())
                    if (layer->type() == NodeType::Rect)
                        std::fprintf(stderr,
                                     "rect z=%d frame=(%g,%g,%g,%g) visible=%d opacity=%g\n",
                                     layer->zIndex(), layer->x(), layer->y(), layer->width(),
                                     layer->height(), layer->visible(), layer->opacity());
            }
        }
        assert(animations.hasActive() && stamps() > 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(70));
        animations.tick(0);
        assert(!animations.hasActive() && stamps() == 0);
        animations.tick(0);
        assert(!animations.hasActive()); // Stationary caret never schedules ambient trail work.
        auto command = input.editingState();
        --command.current.cursorPos; // Same-document model command, final word is ASCII.
        command.current.selectionAnchor = command.current.cursorPos;
        input.restoreEditingState(command, true);
        input.measure(renderer, constraints);
        input.layout(renderer);
        assert(animations.hasActive() && stamps() > 0);
        auto selected = command;
        selected.current.selectionAnchor = selected.current.cursorPos - 1;
        input.restoreEditingState(selected, true);
        assert(!animations.hasActive() && stamps() == 0);
        input.restoreEditingState(command, true);
        input.measure(renderer, constraints);
        input.layout(renderer);
        input.moveCaretLeft();
        assert(animations.hasActive());
        const TextInputEdit hiddenCaret{.commitText = {},
                                        .preeditText = "猫",
                                        .preeditCursorBegin = -1,
                                        .preeditCursorEnd = -1,
                                        .hasPreedit = true};
        input.textInputApplyEdit(hiddenCaret);
        const auto hiddenState = input.editingState();
        assert(hiddenState.history == command.history && hiddenState.preeditCaretHidden);
        assert(!animations.hasActive() && stamps() == 0);
        input.restoreEditingState(command, true);
        input.measure(renderer, constraints);
        input.layout(renderer);
        input.moveCaretLeft();
        assert(animations.hasActive());
        input.restoreEditingState(hiddenState, true);
        assert(!animations.hasActive() &&
               stamps() == 0);              // Same history never overrides IME hiding.
        input.restoreEditingState(command); // A full restore must clear even the same history.
        assert(!animations.hasActive() && stamps() == 0);
        input.measure(renderer, constraints);
        input.layout(renderer);
        input.moveCaretLeft();
        assert(animations.hasActive() && stamps() > 0);
        dispatcher.setFocus(nullptr);
        assert(!animations.hasActive() && stamps() == 0);
        dispatcher.setFocus(input.inputArea());
        input.clearSelection();
        input.moveCaretLeft();
        assert(animations.hasActive() && stamps() > 0);
        MotionService::instance().setEnabled(false);
        assert(stamps() == 0); // The actual native animation receives reduced-motion completion.
        input.setCaretStyle(Input::CaretShape::Box, Input::CaretEffect::Off);
        animations.tick(0);
        assert(!animations.hasActive());
        MotionService::instance().setEnabled(true);
        input.setCaretStyle(Input::CaretShape::Underline, Input::CaretEffect::Trail);
        input.measure(renderer, constraints);
        input.layout(renderer);
        input.moveCaretLeft();
        assert(animations.hasActive());
        input.setCaretStyle(Input::CaretShape::Underline, Input::CaretEffect::Off);
        assert(!animations.hasActive() && stamps() == 0);
        input.resetCaretEffects();
        assert(input.value() == document.current.value &&
               input.editingState().history == document.history);
        dispatcher.setFocus(nullptr);
        dispatcher.setSceneRoot(nullptr);
        MotionService::instance().setEnabled(motion);
    }
    wl_display_disconnect(display);
}
} // namespace

int main(int argc, char **argv) {
    setApplicationIdentity({.name = "blastirc", .envPrefix = "BLASTIRC"});
    using namespace input_caret;
    assert(duration(-1) == 40 && duration(300) == 240);
    assert(duration(std::numeric_limits<float>::quiet_NaN()) == 120);
    assert(strength(-1) == .05F && strength(1) == .35F);
    assert(strength(std::numeric_limits<float>::infinity()) == .20F);

    // These are the production snap/clip and finite trail state, not a mock renderer.
    for (float scale : {1.F, 1.25F, 1.5F, 2.F}) {
        const LayoutRect viewport{10.2F, 20.3F, 100.1F, 40.2F};
        const auto rect = clippedPixels({-20, 0, 200, 200}, viewport, scale);
        assert(rect.x >= viewport.x && rect.y >= viewport.y);
        assert(rect.x + rect.width <= viewport.x + viewport.width);
        assert(rect.y + rect.height <= viewport.y + viewport.height);
        for (float edge : {rect.x, rect.y, rect.x + rect.width, rect.y + rect.height})
            assert(near(edge * scale, std::round(edge * scale)));
        const auto outside = clippedPixels({500, 500, 2, 20}, viewport, scale);
        assert(outside.width == 0 && outside.height == 0);
    }
    const LayoutRect old{2, 4, 2, 20}, next{82, 28, 18, 20};
    Trail trail;
    assert(!trail.move(old, true) && !trail.active); // First focus/layout only anchors.
    assert(!trail.move(old, true));                  // Idle does not animate.
    assert(trail.move(next, true) && trail.active);
    assert(equal(trail.previous, next)); // Native destination is immediate, never interpolated.
    assert(equal(trail.sample(0, 0), old) && equal(trail.sample(1, 0), next));
    for (std::size_t i = 0; i < Trail::count; ++i) {
        assert(trail.opacity(0, i, .20F) > 0 && trail.opacity(0, i, .20F) <= .20F);
        assert(trail.opacity(1, i, .20F) == 0 && trail.opacity(2, i, .20F) == 0);
    }
    // Focus loss, selection, hidden composition, reduced motion and Off share the same gate.
    for (int reason = 0; reason < 5; ++reason) {
        assert(!trail.move(next, false) && !trail.active && !trail.anchored);
        assert(!trail.move(old, true)); // Never bridge a hidden interval or another document.
        assert(trail.move(next, true));
        trail.reset();
        assert(!trail.active && !trail.anchored && trail.opacity(0, 0, .20F) == 0);
    }

    // Exercise real Pango geometry without a Wayland surface or a GL/mock backend.
    CairoTextRenderer text;
    text.initialize(nullptr, nullptr);
    const TextFont font{.family = "sans-serif", .features = {}};
    std::vector<TextCursorStop> cells;
    for (float scale : {1.F, 2.F}) {
        text.measureCursorStopsWrapped(scale, "Wi", 16, {0, 1, 2}, 0, cells, font, true);
        assert(cells.size() == 3 && cells[0].cellWidth > cells[1].cellWidth);
        const float space = text.measure(scale, " ", 16, font).width;
        assert(space > 0 && near(cells.back().cellWidth, space));
        text.measureCursorStopsWrapped(scale,
                                       "a\xcc\x81"
                                       "b",
                                       16, {0, 1, 3, 4}, 0, cells, font, true);
        assert(near(cells[0].cellX, cells[1].cellX));
        assert(near(cells[0].cellWidth, cells[1].cellWidth) && cells[0].cellWidth > 0);
        text.measureCursorStopsWrapped(scale, "👩‍💻x", 16, {0, 4, 7, 11, 12}, 0, cells, font,
                                       true);
        assert(cells.size() == 5 && cells[0].cellWidth > 0);
        for (int i : {1, 2}) {
            assert(near(cells[0].cellX, cells[i].cellX));
            assert(near(cells[0].cellWidth, cells[i].cellWidth));
        }
        text.measureCursorStopsWrapped(scale, "猫\nאב", 16, {0, 3, 4, 6, 8}, 40, cells, font, true);
        assert(cells.size() == 5 && cells[0].cellWidth > 0 && cells[2].y > cells[0].y);
        assert(near(cells[1].cellWidth, space)); // Newline, not a cross-line caret subtraction.
        assert(cells[2].cellWidth > 0 && cells[3].cellWidth > 0 && cells[4].cellWidth > 0);
        for (const auto rtl : {"אב", "אב abc"}) {
            const std::string value(rtl);
            text.measureCursorStopsWrapped(scale, value, 16, {value.size()}, 100, cells, font,
                                           true);
            assert(cells.size() == 1 && near(cells[0].cellWidth, space));
            assert(near(cells[0].cellX + cells[0].cellWidth, cells[0].x));
        }
        text.measureCursorStopsWrapped(scale, "אב\n", 16, {2, 4}, 100, cells, font, true);
        assert(cells.size() == 2 && near(cells[1].cellX + cells[1].cellWidth, cells[1].x) &&
               cells[1].cellX + cells[1].cellWidth <= cells[0].cellX + .02F);
        const std::string ltrWithRtlEnd = "abc אב";
        text.measureCursorStopsWrapped(scale, ltrWithRtlEnd, 16, {ltrWithRtlEnd.size()}, 100, cells,
                                       font, true);
        assert(cells.size() == 1 && near(cells[0].cellX, cells[0].x));
        text.measureCursorStopsWrapped(scale, "", 16, {0}, 40, cells, font, true);
        assert(cells.size() == 1 && cells[0].height > 0 && near(cells[0].cellWidth, space));
        const auto rich = blast::parseIrcFormatting("\x02"
                                                    "Wi\x02"
                                                    "猫\n\x1d"
                                                    "אב\x1d"
                                                    "á👩‍💻\tZ",
                                                    rgbHex(0xffffff), rgbHex(0x111111));
        const auto markup = blast::ircMarkup(rich);
        const auto combining = rich.text.find("á"), zwj = rich.text.find("👩‍💻");
        const auto spaces = rich.text.find("    ");
        const std::vector<size_t> offsets{0,
                                          1,
                                          2,
                                          5,
                                          6,
                                          8,
                                          10,
                                          combining,
                                          combining + 1,
                                          combining + 3,
                                          zwj + 4,
                                          zwj + 7,
                                          zwj + 11,
                                          spaces + 1,
                                          spaces + 2,
                                          spaces + 3,
                                          spaces + 4,
                                          rich.text.size()};
        text.measureCursorStopsWrapped(scale, markup, 16, offsets, 140, cells, font, true, true);
        assert(cells.size() == offsets.size() && cells[0].cellWidth > cells[1].cellWidth &&
               cells[2].cellWidth > 0 && near(cells[3].cellWidth, space) &&
               cells[4].y > cells[0].y && cells[4].cellWidth > 0 && cells[5].cellWidth > 0);
        assert(near(cells[7].cellX, cells[8].cellX) &&
               near(cells[7].cellWidth, cells[8].cellWidth));
        for (size_t index : {10, 11}) {
            assert(near(cells[9].cellX, cells[index].cellX) &&
                   near(cells[9].cellWidth, cells[index].cellWidth));
        }
        for (size_t index : {12, 13, 14, 15})
            assert(near(cells[index].cellWidth, space));
        const auto metrics = text.measure(scale, markup, 16, font, 140, 0, TextAlign::Start,
                                          TextEllipsize::End, true);
        // Reference the native layout box, not the final cursor's run box:
        // fallback fonts can give different runs different vertical extents.
        // Like CairoTextRenderer, this map uses process-global Fontconfig fonts.
        auto *fontMap = pango_cairo_font_map_new();
        auto *context = pango_font_map_create_context(fontMap);
        pango_context_set_base_dir(context, PANGO_DIRECTION_LTR);
        auto *fontOptions = cairo_font_options_create();
        cairo_font_options_set_antialias(fontOptions, CAIRO_ANTIALIAS_GRAY);
        pango_cairo_context_set_font_options(context, fontOptions);
        cairo_font_options_destroy(fontOptions);
        auto *layout = pango_layout_new(context);
        auto *description = pango_font_description_new();
        pango_font_description_set_family(description, font.family.c_str());
        pango_font_description_set_weight(description, static_cast<PangoWeight>(font.weight));
        pango_font_description_set_absolute_size(description, 16 * scale * PANGO_SCALE);
        pango_layout_set_font_description(layout, description);
        pango_font_description_free(description);
        pango_layout_set_markup(layout, markup.data(), static_cast<int>(markup.size()));
        auto *attributes = pango_attr_list_copy(pango_layout_get_attributes(layout));
        auto *hyphens = pango_attr_insert_hyphens_new(FALSE);
        hyphens->start_index = 0;
        hyphens->end_index = PANGO_ATTR_INDEX_TO_TEXT_END;
        pango_attr_list_insert(attributes, hyphens);
        pango_layout_set_attributes(layout, attributes);
        pango_attr_list_unref(attributes);
        pango_layout_set_width(layout, static_cast<int>(140 * scale * PANGO_SCALE));
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_height(layout, -500); // Native maxLines=0 safety budget.
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
        PangoRectangle logical{};
        pango_layout_get_extents(layout, nullptr, &logical);
        const float unit = 1.F / PANGO_SCALE / scale;
        assert(metrics.layoutBaseline > 0 && metrics.layoutHeight > metrics.layoutBaseline &&
               near(metrics.layoutBaseline, pango_layout_get_baseline(layout) * unit) &&
               near(metrics.layoutHeight, logical.height * unit) &&
               metrics.lineCount == pango_layout_get_line_count(layout));
        for (size_t index = 0; index < cells.size(); ++index) {
            const auto &cell = cells[index];
            assert(cell.y >= logical.y * unit && cell.height > 0 &&
                   cell.y + cell.height <= (logical.y + logical.height) * unit);
            PangoRectangle cursor{};
            pango_layout_get_cursor_pos(layout, static_cast<int>(offsets[index]), &cursor, nullptr);
            assert(near(cell.x, cursor.x * unit) && near(cell.y, cursor.y * unit) &&
                   near(cell.height, cursor.height * unit));
        }
        g_object_unref(layout);
        g_object_unref(context);
        g_object_unref(fontMap);
    }

    // Shape/effect controls cannot alter real native editing/IME state or hide the selected shape.
    Input input;
    input.setMultiline(true);
    input.setValue("a猫z");
    input.clearSelection();
    const auto saved = input.editingState();
    const auto ime = input.textInputState();
    RectNode *caret = nullptr;
    for (const auto &child : input.children())
        for (const auto &layer : child->children())
            if (layer->type() == NodeType::Rect && layer->zIndex() == 1)
                caret = static_cast<RectNode *>(layer.get());
    assert(caret);
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&input);
    dispatcher.setFocus(input.inputArea());
    input.setCaretStyle(Input::CaretShape::Box, Input::CaretEffect::Trail);
    assert(input.editingStateMatches(saved));
    auto after = input.textInputState();
    assert(after.cursorRectX == ime.cursorRectX && after.cursorRectY == ime.cursorRectY);
    assert(after.cursorRectWidth == ime.cursorRectWidth &&
           after.cursorRectHeight == ime.cursorRectHeight);
    const bool motion = MotionService::instance().enabled();
    MotionService::instance().setEnabled(false);
    input.setCaretStyle(Input::CaretShape::Box, Input::CaretEffect::Off);
    assert(caret->visible());
    input.resetCaretEffects();
    assert(input.editingStateMatches(saved) && caret->visible());
    input.selectAll();
    assert(!caret->visible());
    input.restoreEditingState(saved);
    assert(caret->visible());
    dispatcher.setFocus(nullptr);
    assert(!caret->visible());
    dispatcher.setSceneRoot(nullptr);
    // Shared text/preedit/history are authoritative, but same-document peers retain their scroll.
    auto local = input.editingState();
    local.scrollX = 7;
    local.scrollY = 11;
    input.restoreEditingState(local);
    const TextInputEdit hidden{.commitText = {},
                               .preeditText = "猫",
                               .preeditCursorBegin = -1,
                               .preeditCursorEnd = -1,
                               .hasPreedit = true};
    input.textInputApplyEdit(hidden);
    auto remote = input.editingState();
    remote.scrollX = 20;
    remote.scrollY = 30;
    remote.history = std::make_shared<Input::EditHistory>();
    const auto composingIme = input.textInputState();
    input.restoreEditingState(remote, true);
    const auto retained = input.editingState();
    assert(retained.scrollX == local.scrollX && retained.scrollY == local.scrollY);
    assert(retained.current == remote.current && retained.history == remote.history);
    assert(retained.preeditBase == remote.preeditBase && retained.preeditCaretHidden &&
           input.composing());
    assert(input.textInputState().cursor == composingIme.cursor &&
           input.textInputState().anchor == composingIme.anchor);
    input.restoreEditingState(remote);
    assert(input.editingStateMatches(
        remote)); // Explicit identity changes restore the complete viewport.
    MotionService::instance().setEnabled(motion);
    if (argc > 1) {
        assert(argc == 2 && std::string_view(argv[1]) == "--native");
        nativeCheck();
    }
}
