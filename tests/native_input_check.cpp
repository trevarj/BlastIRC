#include "ui/irc_format.hpp"
#include <cassert>
#include <cmath>
#include <core/application_identity.h>
#include <core/input/key_modifiers.h>
#include <core/input/keybind_matcher.h>
#include <core/text_clipboard.h>
#include <cstdio>
#include <glib.h>
#include <memory>
#include <render/animation/animation_manager.h>
#include <render/animation/motion_service.h>
#include <render/render_context.h>
#include <render/render_target.h>
#include <render/scene/input_area.h>
#include <render/scene/input_dispatcher.h>
#include <render/scene/rect_node.h>
#include <render/scene/text_node.h>
#include <stdexcept>
#include <string>
#include <ui/controls/box.h>
#include <ui/controls/button.h>
#include <ui/controls/context_menu_popup.h>
#include <ui/controls/input.h>
#include <ui/controls/label.h>
#include <ui/controls/scroll_view.h>
#include <vector>
#include <wayland-client.h>
#include <wayland/popup_surface.h>
#include <wayland/wayland_connection.h>
#include <wayland/wayland_seat.h>
#include <xkbcommon/xkbcommon-keysyms.h>

class ContextMenuPopupTestAccess {
  public:
    static void attachUninitializedSurface(ContextMenuPopup &popup, WaylandConnection &wayland,
                                           ContextMenuControl &menu) {
        // Exercise controller state only: no protocol objects or native mapping.
        popup.m_surface = std::make_unique<PopupSurface>(wayland);
        popup.m_menu = &menu;
    }
};

static void checkWaylandSeatTouchMouseIsolation() {
    WaylandSeat seat;
    std::vector<PointerEvent> events;
    seat.setPointerEventCallback([&](const PointerEvent &event) { events.push_back(event); });
    // Stable opaque identities only: check event construction, not compositor transport.
    int mouseIdentity = 0, touchIdentity = 0;
    auto *mouseSurface = reinterpret_cast<wl_surface *>(&mouseIdentity);
    auto *touchSurface = reinterpret_cast<wl_surface *>(&touchIdentity);
    double mouseX = 31.25, mouseY = 42.5;
    auto checkMouse = [&] {
        assert(seat.lastPointerSurface() == mouseSurface && seat.hasPointerPosition());
        assert(seat.lastPointerX() == mouseX && seat.lastPointerY() == mouseY);
    };
    auto checkEvent = [&](std::size_t index, PointerEvent::Type type, wl_surface *surface, double x,
                          double y, bool touch, bool pressed = false) {
        assert(index < events.size());
        const auto &event = events[index];
        assert(event.type == type && event.surface == surface);
        assert(event.sx == x && event.sy == y && event.touch == touch);
        assert(event.pressed == pressed);
        assert(event.button == (type == PointerEvent::Type::Button ? BTN_LEFT : 0));
    };

    WaylandSeat::handlePointerEnter(&seat, nullptr, 1, mouseSurface, wl_fixed_from_double(mouseX),
                                    wl_fixed_from_double(mouseY));
    assert(events.empty());
    WaylandSeat::handlePointerFrame(&seat, nullptr);
    checkMouse();
    assert(events.size() == 1);
    checkEvent(0, PointerEvent::Type::Enter, mouseSurface, mouseX, mouseY, false);
    events.clear();

    WaylandSeat::handleTouchDown(&seat, nullptr, 2, 20, touchSurface, 7,
                                 wl_fixed_from_double(101.25), wl_fixed_from_double(202.5));
    checkMouse();
    assert(events.empty());
    WaylandSeat::handleTouchFrame(&seat, nullptr);
    checkMouse();
    assert(events.size() == 2);
    checkEvent(0, PointerEvent::Type::Enter, touchSurface, 101.25, 202.5, true);
    checkEvent(1, PointerEvent::Type::Button, touchSurface, 101.25, 202.5, true, true);
    assert(events[0].serial == 2 && events[0].time == 20);
    assert(events[1].serial == 2 && events[1].time == 20);
    events.clear();

    WaylandSeat::handleTouchMotion(&seat, nullptr, 30, 7, wl_fixed_from_double(111.5),
                                   wl_fixed_from_double(222.75));
    checkMouse();
    assert(events.empty());
    WaylandSeat::handleTouchFrame(&seat, nullptr);
    checkMouse();
    assert(events.size() == 1);
    checkEvent(0, PointerEvent::Type::Motion, touchSurface, 111.5, 222.75, true);
    assert(events[0].time == 30);
    events.clear();

    // Mouse motion during the contact must not overwrite the later touch-up position.
    mouseX = 51.5;
    mouseY = 62.75;
    WaylandSeat::handlePointerMotion(&seat, nullptr, 40, wl_fixed_from_double(mouseX),
                                     wl_fixed_from_double(mouseY));
    assert(events.empty());
    WaylandSeat::handlePointerFrame(&seat, nullptr);
    checkMouse();
    assert(events.size() == 1);
    checkEvent(0, PointerEvent::Type::Motion, mouseSurface, mouseX, mouseY, false);
    events.clear();

    WaylandSeat::handleTouchUp(&seat, nullptr, 3, 50, 7);
    checkMouse();
    assert(events.empty());
    WaylandSeat::handleTouchFrame(&seat, nullptr);
    checkMouse();
    assert(events.size() == 2);
    checkEvent(0, PointerEvent::Type::Button, touchSurface, 111.5, 222.75, true);
    // Leave carries identity and serial, but no positional payload.
    checkEvent(1, PointerEvent::Type::Leave, touchSurface, 0, 0, true);
    assert(events[0].serial == 3 && events[0].time == 50 && events[1].serial == 3);
    events.clear();

    WaylandSeat::handlePointerButton(&seat, nullptr, 4, 60, BTN_LEFT,
                                     WL_POINTER_BUTTON_STATE_RELEASED);
    assert(events.empty());
    WaylandSeat::handlePointerFrame(&seat, nullptr);
    checkMouse();
    assert(events.size() == 1);
    checkEvent(0, PointerEvent::Type::Button, mouseSurface, mouseX, mouseY, false);
    assert(events[0].serial == 4 && events[0].time == 60);
    events.clear();

    // Cancel before the first frame must discard the queued Enter and Down.
    WaylandSeat::handleTouchDown(&seat, nullptr, 5, 70, touchSurface, 8,
                                 wl_fixed_from_double(301.25), wl_fixed_from_double(402.5));
    checkMouse();
    assert(events.empty());
    WaylandSeat::handleTouchCancel(&seat, nullptr);
    checkMouse();
    assert(events.size() == 1);
    checkEvent(0, PointerEvent::Type::Leave, touchSurface, 0, 0, true);
    WaylandSeat::handleTouchFrame(&seat, nullptr);
    checkMouse();
    assert(events.size() == 1);
    events.clear();

    // A fresh contact after cancel must work, including cancel after delivery.
    WaylandSeat::handleTouchDown(&seat, nullptr, 6, 80, touchSurface, 9,
                                 wl_fixed_from_double(501.5), wl_fixed_from_double(602.75));
    checkMouse();
    assert(events.empty());
    WaylandSeat::handleTouchFrame(&seat, nullptr);
    checkMouse();
    assert(events.size() == 2);
    checkEvent(0, PointerEvent::Type::Enter, touchSurface, 501.5, 602.75, true);
    checkEvent(1, PointerEvent::Type::Button, touchSurface, 501.5, 602.75, true, true);
    WaylandSeat::handleTouchCancel(&seat, nullptr);
    checkMouse();
    assert(events.size() == 3);
    checkEvent(2, PointerEvent::Type::Leave, touchSurface, 0, 0, true);
    WaylandSeat::handleTouchFrame(&seat, nullptr);
    checkMouse();
    assert(events.size() == 3);
}

static void checkContextMenuHierarchyAndOutsideRelease() {
    RenderContext context;
    ScaledRenderer renderer(context, 1.f);
    ContextMenuControl menu;
    ContextMenuEntry branch;
    branch.id = 1;
    branch.value = 701;
    branch.label = "Actions";
    branch.hasSubmenu = true;
    branch.children.resize(4);
    branch.children[0].separator = true;
    branch.children[1].label = "Unavailable";
    branch.children[1].enabled = false;
    branch.children[2].label = "Group";
    branch.children[2].header = true;
    branch.children[3].label = "Nested action";
    branch.children[3].value = 941;

    std::size_t activatedValue = 0;
    int activations = 0, submenuOpens = 0;
    menu.setOnActivate([&](const ContextMenuControlEntry &entry) {
        activatedValue = entry.value;
        ++activations;
    });
    menu.setOnSubmenuOpen([&](const ContextMenuControlEntry &entry, float centerY) {
        assert(entry.id == branch.id && entry.value == branch.value && entry.hasSubmenu);
        assert(centerY > menu.rowTop(1) && centerY < menu.rowBottom(1));
        ++submenuOpens;
    });
    ContextMenuControlEntry heading{};
    heading.label = "Group";
    heading.header = true;
    ContextMenuControlEntry separator{};
    separator.separator = true;
    ContextMenuControlEntry unavailable{};
    unavailable.label = "Unavailable";
    unavailable.enabled = false;
    ContextMenuControlEntry moreHeading{};
    moreHeading.label = "More";
    moreHeading.header = true;
    ContextMenuControlEntry rootAction{};
    rootAction.id = 5;
    rootAction.value = 803;
    rootAction.label = "Root action";
    menu.setEntries({heading, static_cast<const ContextMenuControlEntry &>(branch), separator,
                     unavailable, moreHeading, rootAction});
    // Labels require a context-backed renderer; this exercises layout, not GPU/text rendering.
    menu.layout(renderer);
    WaylandConnection wayland;
    ContextMenuPopup popup(wayland, context);
    ContextMenuPopupTestAccess::attachUninitializedSurface(popup, wayland, menu);
    KeybindMatcher::setMatcher(KeybindAction::Down, [](std::uint32_t sym, std::uint32_t modifiers) {
        return sym == XKB_KEY_Down && modifiers == 0;
    });
    KeybindMatcher::setMatcher(KeybindAction::Up, [](std::uint32_t sym, std::uint32_t modifiers) {
        return sym == XKB_KEY_Up && modifiers == 0;
    });
    menu.setHighlightedIndex(1);
    assert(menu.activateHighlighted() && submenuOpens == 1 && activations == 0);
    popup.onKeyboardEvent({.sym = XKB_KEY_Down, .pressed = true});
    assert(menu.highlightedIndex() == 5);
    assert(menu.activateHighlighted() && activations == 1 && activatedValue == 803);
    popup.onKeyboardEvent({.sym = XKB_KEY_Up, .pressed = true});
    assert(menu.highlightedIndex() == 1);
    popup.onKeyboardEvent({.sym = XKB_KEY_Up, .pressed = true});
    assert(menu.highlightedIndex() == 5);          // Wrap skips heading.
    assert(submenuOpens == 1 && activations == 1); // Navigation never activates.

    std::vector<ContextMenuControlEntry> childRows;
    for (std::size_t i = 0; i < branch.children.size(); ++i) {
        childRows.push_back(static_cast<const ContextMenuControlEntry &>(branch.children[i]));
        childRows.back().id = static_cast<std::int32_t>(i);
    }
    menu.setEntries(std::move(childRows));
    menu.layout(renderer);
    menu.setHighlightedIndex(0);
    assert(menu.highlightedIndex() == 3);
    assert(menu.moveHighlight(1) && menu.highlightedIndex() == 3);
    assert(menu.moveHighlight(-1) && menu.highlightedIndex() == 3);
    assert(menu.activateHighlighted() && activations == 2 && activatedValue == 941);
    assert(submenuOpens == 1); // Child-local id 3 must not replace stable leaf value 941.

    menu.setEntries({separator, heading, unavailable});
    menu.layout(renderer);
    assert(!menu.moveHighlight(1) && !menu.moveHighlight(-1));
    assert(!menu.activateHighlighted() && activations == 2 && submenuOpens == 1);

    int dismissals = 0;
    popup.setOnDismissed([&] { ++dismissals; });
    assert(popup.isOpen());
    PointerEvent outside{.type = PointerEvent::Type::Button, .button = BTN_LEFT, .pressed = true};
    assert(popup.onPointerEvent(outside));
    assert(!popup.isOpen() && dismissals == 1);
    PointerEvent unrelated{.type = PointerEvent::Type::Button, .button = BTN_RIGHT};
    assert(!popup.onPointerEvent(unrelated)); // Only the matching release belongs to dismissal.
    outside.pressed = false;
    assert(popup.onPointerEvent(outside));  // Consumed even after the popup closed.
    assert(!popup.onPointerEvent(outside)); // Exactly one release is swallowed.
    popup.close();
    assert(dismissals == 1);
}

static void checkClippedCapture(bool transformed) {
    InputDispatcher dispatcher;
    Node root;
    root.setSize(400, 200);
    auto clipOwner = std::make_unique<Node>();
    clipOwner->setSize(110, 40);
    clipOwner->setClipChildren(true);
    // An ancestor's enlarged hit target must not enlarge its painted clip.
    clipOwner->setHitTestOutset({.right = 32});
    auto *clip = root.addChild(std::move(clipOwner));
    auto containerOwner = std::make_unique<Node>();
    containerOwner->setSize(140, 40);
    containerOwner->setClipChildren(true);
    auto *container = clip->addChild(std::move(containerOwner));
    auto areaOwner = std::make_unique<InputArea>();
    areaOwner->setPosition(88, 4);
    areaOwner->setSize(32, 32);
    areaOwner->setHitTestOutset({.left = 8, .right = 8});
    auto *area = static_cast<InputArea *>(container->addChild(std::move(areaOwner)));
    if (transformed) {
        root.setPosition(37, 29);
        root.setScale(1.1f, .9f);
        root.setRotation(-.2f);
        clip->setPosition(41, 23);
        clip->setScale(1.3f, .8f);
        clip->setRotation(.6f);
    }

    int clicks = 0, releases = 0, motions = 0, cancels = 0;
    bool dragging = false;
    float dragX = 0;
    std::string events;
    InputArea::PointerData lastPress, lastRelease, lastClick;
    area->setOnPress([&](const InputArea::PointerData &data) {
        assert(dispatcher.pointerCaptured() && area->pressed() == data.pressed);
        dragging = data.pressed;
        events += data.pressed ? 'P' : 'R';
        if (data.pressed)
            lastPress = data;
        else {
            lastRelease = data;
            ++releases;
        }
    });
    area->setOnMotion([&](const InputArea::PointerData &data) {
        if (!dragging)
            return;
        assert(dispatcher.pointerCaptured() && area->pressed());
        dragX = data.localX;
        ++motions;
        events += 'M';
    });
    area->setOnClick([&](const InputArea::PointerData &data) {
        assert(dispatcher.pointerCaptured() && !area->pressed() && !dragging);
        assert(events.back() == 'R'); // Release callback still precedes click.
        assert(data.serial == lastPress.serial && data.time == lastPress.time);
        assert(data.sceneX == lastPress.sceneX && data.sceneY == lastPress.sceneY);
        lastClick = data;
        ++clicks;
        events += 'C';
    });
    area->setOnCancel([&] { ++cancels; });
    dispatcher.setSceneRoot(&root);
    std::uint32_t serial = 0;
    auto buttonAt = [&](float x, bool pressed) {
        float sceneX = 0, sceneY = 0;
        Node::mapToScene(clip, x, 20, sceneX, sceneY);
        ++serial;
        assert(dispatcher.pointerButton(sceneX, sceneY, BTN_LEFT, pressed, serial, serial * 10,
                                        false));
        assert(dispatcher.pointerCaptured() == pressed && area->pressed() == pressed &&
               dragging == pressed);
    };
    auto motionAt = [&](float x) {
        float sceneX = 0, sceneY = 0;
        Node::mapToScene(clip, x, 20, sceneX, sceneY);
        dispatcher.pointerMotion(sceneX, sceneY, ++serial);
        assert(dispatcher.pointerCaptured() && area->pressed() && dragging);
    };

    buttonAt(100, true);
    buttonAt(115, false); // Still inside the child, but outside the 110-wide ancestor clip.
    assert(events == "PR" && clicks == 0 && releases == 1);
    assert(std::abs(lastRelease.localX - 27) < .001f);
    events.clear();
    buttonAt(100, true);
    buttonAt(100, false);
    assert(events == "PRC" && clicks == 1 && releases == 2);

    events.clear();
    buttonAt(100, true);
    motionAt(115);
    assert(motions == 1 && std::abs(dragX - 27) < .001f);
    buttonAt(115, false);
    assert(events == "PMR" && clicks == 1 && releases == 3);
    events.clear();
    buttonAt(100, true);
    motionAt(145); // Capture also survives leaving the target itself.
    assert(motions == 2 && std::abs(dragX - 57) < .001f);
    buttonAt(145, false);
    assert(events == "PMR" && clicks == 1 && releases == 4);
    events.clear();
    buttonAt(100, true);
    motionAt(115);
    motionAt(100);
    buttonAt(100, false);
    assert(events == "PMMRC" && clicks == 2 && releases == 5);

    buttonAt(84, true); // Target minimum-hit outsets still qualify inside the clip.
    buttonAt(86, false);
    assert(clicks == 3 && releases == 6 && std::abs(lastClick.localX + 2) < .001f);
    clip->setClipChildren(false);
    container->setClipChildren(false);
    container->setSize(1, 1);
    buttonAt(115, true); // Nonclipping ancestors must continue to allow overflow/popups.
    buttonAt(115, false);
    assert(clicks == 4 && releases == 7 && cancels == 0);
    dispatcher.setSceneRoot(nullptr);
}

static void checkClippedSelection() {
    RenderContext context;
    ScaledRenderer renderer(context, 1.f);
    InputDispatcher dispatcher;
    Node clip;
    clip.setSize(110, 40);
    clip.setClipChildren(true);
    auto field = std::make_unique<Input>();
    field->setValue("abc");
    field->setPosition(88, 0);
    field->setSize(32, 32);
    field->setControlHeight(32);
    field->inputArea()->setSize(32, 32);
    auto *input = static_cast<Input *>(clip.addChild(std::move(field)));
    dispatcher.setSceneRoot(&clip);
    const auto preparePointerGeometry = [&](float x, float y) {
        auto *area = dispatcher.capturedArea();
        if (!area)
            area = dispatcher.inputAreaAt(x, y);
        if (area && area->textInputClient() == input && input->textGeometryDirty())
            clip.layout(renderer);
    };
    preparePointerGeometry(100, 16);
    assert(dispatcher.pointerButton(100, 16, BTN_LEFT, true, 1, 10, false));
    assert(input->cursorPosition() == 0 && input->selectionAnchor() == 0);
    // The host admits current geometry before native captured edge selection; no paint is needed.
    preparePointerGeometry(115, 16);
    dispatcher.pointerMotion(115, 16, 2);
    assert(dispatcher.pointerCaptured() && input->inputArea()->pressed());
    assert(input->cursorPosition() == 1 && input->selectionAnchor() == 0);
    preparePointerGeometry(115, 16);
    assert(dispatcher.pointerButton(115, 16, BTN_LEFT, false, 3, 30, false));
    assert(!dispatcher.pointerCaptured() && !input->inputArea()->pressed());
    preparePointerGeometry(116, 16);
    dispatcher.pointerMotion(116, 16, 4);
    assert(input->cursorPosition() == 1 && input->selectionAnchor() == 0);
    dispatcher.setSceneRoot(nullptr);
}

static void checkDeferredTouchOriginalIdentity() {
    ScaledRenderer renderer; // Fixed-size native content needs no text backend.
    InputDispatcher dispatcher;
    Node root;
    root.setSize(240, 140);
    auto scrollOwner = std::make_unique<ScrollView>();
    auto *scroll = scrollOwner.get();
    scroll->setViewportPaddingH(0);
    scroll->setViewportPaddingV(0);
    scroll->setScrollbarVisible(false);
    root.addChild(std::move(scrollOwner));
    auto contentOwner = std::make_unique<Box>();
    contentOwner->setSize(240, 600);
    auto *content = scroll->content()->addChild(std::move(contentOwner));
    auto *viewport = static_cast<InputArea *>(scroll->content()->parent());
    int aDowns = 0, aReleases = 0, bDowns = 0, bReleaseActions = 0;
    auto addOriginal = [&] {
        auto owner = std::make_unique<InputArea>();
        owner->setPosition(0, 30);
        owner->setSize(220, 80);
        owner->setOnPress(
            [&](const InputArea::PointerData &data) { data.pressed ? ++aDowns : ++aReleases; });
        return static_cast<InputArea *>(content->addChild(std::move(owner)));
    };
    auto *a = addOriginal();
    auto replacementOwner = std::make_unique<InputArea>();
    replacementOwner->setPosition(0, 180);
    replacementOwner->setSize(220, 80);
    replacementOwner->setOnPress([&](const InputArea::PointerData &data) {
        // Like a VirtualList slot, release itself acts: no pressed/onClick guard.
        data.pressed ? ++bDowns : ++bReleaseActions;
    });
    auto *b = static_cast<InputArea *>(content->addChild(std::move(replacementOwner)));
    scroll->arrange(renderer, {.width = 240, .height = 140});
    assert(scroll->scrollable());
    dispatcher.setSceneRoot(&root);
    std::uint32_t serial = 500;
    float x = 0, y = 0;
    auto down = [&] {
        Node::mapToScene(a, 20, 20, x, y);
        assert(dispatcher.inputAreaAt(x, y) == a && !a->passiveTouchPress());
        const int beforeDowns = aDowns, beforeReleases = aReleases;
        ++serial;
        assert(dispatcher.pointerButton(x, y, BTN_LEFT, true, serial, serial * 10, true));
        assert(dispatcher.pendingTouchPressArea() == a);
        assert(!dispatcher.pointerCaptured() && !a->pressed() && !viewport->pressed());
        assert(aDowns == beforeDowns && aReleases == beforeReleases);
    };
    auto up = [&] {
        ++serial;
        assert(dispatcher.pointerButton(x, y, BTN_LEFT, false, serial, serial * 10, true));
        assert(!dispatcher.pendingTouchPressArea() && !dispatcher.pointerCaptured());
        assert(!b->pressed() && bDowns == 0 && bReleaseActions == 0);
    };
    auto tap = [&] {
        const int beforeDowns = aDowns, beforeReleases = aReleases;
        down();
        up();
        assert(aDowns == beforeDowns + 1 && aReleases == beforeReleases + 1);
        assert(!a->pressed());
    };
    tap(); // Unchanged original still receives the deferred Down and matching release.

    // Reflow, horizontal fallback, destruction and live detach all keep terminal ownership.
    for (int scenario = 0; scenario != 4; ++scenario) {
        down();
        const int beforeDowns = aDowns, beforeReleases = aReleases;
        std::unique_ptr<Node> detached;
        if (scenario < 2) {
            a->setPosition(0, 180);
        } else {
            detached = content->removeChild(a);
            if (scenario == 2) {
                detached.reset(); // Tracked destruction must clear the pending raw identity.
                assert(!dispatcher.pendingTouchPressArea());
            }
        }
        b->setPosition(0, 30);
        assert(dispatcher.inputAreaAt(x, y) == b);
        // Force the release-only fallback target to B, not a stale hover on A.
        dispatcher.syncPointerHover();
        assert(dispatcher.hoveredArea() == b);
        if (scenario >= 2)
            assert(!dispatcher.pendingTouchPressArea());
        if (scenario == 1) {
            // Vertical scroll ancestor exists, but there is no horizontal pan owner.
            x += 24;
            dispatcher.pointerMotion(x, y, ++serial);
            assert(!dispatcher.pendingTouchPressArea() && !dispatcher.pointerCaptured());
            assert(bDowns == 0 && bReleaseActions == 0);
            // The invalidated stream must also swallow later motion and its matching Up.
            dispatcher.pointerMotion(x + 1, y, ++serial);
            assert(!dispatcher.pendingTouchPressArea() && !dispatcher.pointerCaptured());
        }
        up();
        assert(aDowns == beforeDowns && aReleases == beforeReleases);
        if (scenario == 3) {
            assert(detached.get() == a && !a->pressed()); // Detached, not destroyed.
            content->addChild(std::move(detached));
        } else if (scenario == 2) {
            a = addOriginal();
        }
        a->setPosition(0, 30);
        b->setPosition(0, 180);
        tap(); // A consumed terminal event must not poison the next contact.
    }

    // Real vertical handoff still captures the ScrollView viewport, never replaying A.
    down();
    const int beforePanDowns = aDowns, beforePanReleases = aReleases;
    const float beforeScroll = scroll->scrollOffset();
    y -= 24;
    dispatcher.pointerMotion(x, y, ++serial);
    assert(dispatcher.capturedArea() == viewport && viewport->pressed() && !a->pressed());
    assert(!dispatcher.pendingTouchPressArea() && scroll->scrollOffset() > beforeScroll);
    up();
    assert(!viewport->pressed() && aDowns == beforePanDowns && aReleases == beforePanReleases);
    scroll->setScrollOffset(0);

    // Explicit stream resets clear identity without activating the deferred leaf.
    Node otherRoot;
    otherRoot.setSize(240, 140);
    for (int reset = 0; reset != 3; ++reset) {
        down();
        const int beforeDowns = aDowns, beforeReleases = aReleases;
        if (reset == 0)
            dispatcher.cancelPointerCapture();
        else if (reset == 1)
            dispatcher.setSceneRoot(&otherRoot);
        else
            dispatcher.pointerLeave();
        assert(!dispatcher.pendingTouchPressArea() && !dispatcher.pointerCaptured());
        assert(!a->pressed() && aDowns == beforeDowns && aReleases == beforeReleases);
        assert(bDowns == 0 && bReleaseActions == 0);
        if (reset == 1)
            dispatcher.setSceneRoot(&root);
        tap();
    }
    // Replay must revalidate identity after real focus-loss callbacks mutate the scene.
    auto focusOwner = std::make_unique<InputArea>();
    focusOwner->setPosition(225, 0);
    focusOwner->setSize(10, 10);
    focusOwner->setFocusable(true);
    auto *otherFocus = static_cast<InputArea *>(root.addChild(std::move(focusOwner)));
    b->setFocusable(true);
    for (bool destroyOriginal : {false, true}) {
        for (bool horizontalFallback : {false, true}) {
            a->setFocusable(true);
            dispatcher.setFocus(otherFocus);
            int focusLosses = 0;
            otherFocus->setOnFocusLoss([&] {
                ++focusLosses;
                if (destroyOriginal) {
                    content->removeChild(a).reset();
                    a = nullptr;
                } else {
                    a->setPosition(0, 180);
                }
                b->setPosition(0, 30);
            });
            down();
            assert(dispatcher.focusedArea() == otherFocus && focusLosses == 0);
            const int beforeDowns = aDowns, beforeReleases = aReleases;
            if (horizontalFallback) {
                x += 24;
                dispatcher.pointerMotion(x, y, ++serial);
                assert(focusLosses == 1 && !dispatcher.pendingTouchPressArea());
                assert(!dispatcher.pointerCaptured() && bDowns == 0 && bReleaseActions == 0);
                dispatcher.syncPointerHover();
                assert(dispatcher.hoveredArea() == b);
            }
            up();
            assert(focusLosses == 1 && aDowns == beforeDowns && aReleases == beforeReleases);
            assert(dispatcher.inputAreaAt(x, y) == b && !b->pressed());
            otherFocus->setOnFocusLoss({});

            // Consuming A's stream must not suppress a subsequent genuine contact on B.
            ++serial;
            assert(dispatcher.pointerButton(x, y, BTN_LEFT, true, serial, serial * 10, true));
            assert(dispatcher.pendingTouchPressArea() == b && !dispatcher.pointerCaptured());
            assert(bDowns == 0 && bReleaseActions == 0);
            ++serial;
            assert(dispatcher.pointerButton(x, y, BTN_LEFT, false, serial, serial * 10, true));
            assert(bDowns == 1 && bReleaseActions == 1 && !b->pressed());
            assert(!dispatcher.pendingTouchPressArea() && !dispatcher.pointerCaptured());
            bDowns = bReleaseActions = 0;
            if (destroyOriginal)
                a = addOriginal();
            a->setPosition(0, 30);
            b->setPosition(0, 180);
        }
    }
    // Same-node rebinding can cancel by original identity during replay focus loss.
    for (bool horizontalFallback : {false, true}) {
        a->setFocusable(true);
        a->setTabFocusKey("original-binding");
        dispatcher.setFocus(otherFocus);
        bool rebound = false;
        otherFocus->setOnFocusLoss([&] {
            assert(!rebound && dispatcher.pendingTouchPressArea() == a);
            dispatcher.cancelPointerCapture(); // Cancel before changing the bound data.
            a->setTabFocusKey("replacement-binding");
            rebound = true;
        });
        const int beforeDowns = aDowns, beforeReleases = aReleases;
        down();
        assert(!rebound && dispatcher.focusedArea() == otherFocus);
        if (horizontalFallback) {
            x += 24;
            dispatcher.pointerMotion(x, y, ++serial);
            assert(rebound && !dispatcher.pendingTouchPressArea());
            assert(!dispatcher.pointerCaptured() && !a->pressed());
            assert(aDowns == beforeDowns && aReleases == beforeReleases);
            // The host masks the retired contact's Up after cancellation.
            // Ordinary hover must resume, rather than a resurrected pending stream swallowing it.
            b->setPosition(0, 110);
            float bX = 0, bY = 0;
            Node::mapToScene(b, 20, 10, bX, bY);
            assert(dispatcher.inputAreaAt(bX, bY) == b);
            dispatcher.pointerMotion(bX, bY, ++serial);
            assert(dispatcher.hoveredArea() == b && !dispatcher.pointerCaptured());
            assert(bDowns == 0 && bReleaseActions == 0);
            b->setPosition(0, 180);
        } else {
            up(); // Focus cancellation during Up consumes this already-dispatched terminal event.
        }
        assert(rebound && dispatcher.inputAreaAt(x, y) == a && !a->pressed());
        assert(aDowns == beforeDowns && aReleases == beforeReleases);
        otherFocus->setOnFocusLoss({});
        tap(); // The rebound leaf accepts a new, independently owned gesture.
    }
    dispatcher.setSceneRoot(nullptr);
}

static void checkPassiveTouchRow() {
    ScaledRenderer renderer; // Fixed-size content needs no text/texture backend.
    InputDispatcher dispatcher;
    Node root;
    root.setSize(240, 140);
    auto scrollOwner = std::make_unique<ScrollView>();
    auto *scroll = scrollOwner.get();
    scroll->setViewportPaddingH(0);
    scroll->setViewportPaddingV(0);
    scroll->setScrollbarVisible(false);
    root.addChild(std::move(scrollOwner));
    auto contentOwner = std::make_unique<Box>();
    contentOwner->setSize(240, 600);
    auto *content = scroll->content()->addChild(std::move(contentOwner));
    auto *viewport = static_cast<InputArea *>(scroll->content()->parent());
    auto rowOwner = std::make_unique<InputArea>();
    rowOwner->setPosition(0, 30);
    rowOwner->setSize(220, 80);
    auto *row = static_cast<InputArea *>(content->addChild(std::move(rowOwner)));

    std::string binding = "buffer:message-A", pressedBinding;
    bool armed = false;
    int presses = 0, releases = 0, cancels = 0, menus = 0, motions = 0;
    float pressX = 0, pressY = 0, motionX = 0;
    std::uint32_t pressSerial = 0, clickSerial = 0;
    row->setTabFocusKey(binding);
    row->setOnPress([&](const InputArea::PointerData &data) {
        if (data.pressed) {
            ++presses;
            pressedBinding = binding;
            pressSerial = data.serial;
            pressX = data.localX;
            pressY = data.localY;
            armed = true;
        } else {
            ++releases;
        }
    });
    row->setOnMotion([&](const InputArea::PointerData &data) {
        if (!row->pressed())
            return;
        assert(dispatcher.pointerCaptured() && !viewport->pressed());
        ++motions;
        motionX = data.localX;
        const float dx = data.localX - pressX, dy = data.localY - pressY;
        if (dx * dx + dy * dy > 4 * 4)
            armed = false; // The app's tap guard is smaller than native pan slop (8px).
    });
    row->setOnCancel([&] {
        ++cancels;
        armed = false;
    });
    row->setOnClick([&](const InputArea::PointerData &data) {
        if (armed && pressedBinding == binding) {
            ++menus;
            clickSerial = data.serial;
        }
        armed = false;
    });
    auto bind = [&](const std::string &key) {
        if (binding != key) {
            row->dispatchCancel(); // The app, not a native identity generation, owns rebinding.
            binding = key;
        }
        row->setTabFocusKey(key);
    };
    scroll->arrange(renderer, {.width = 240, .height = 140});
    assert(scroll->scrollable());
    dispatcher.setSceneRoot(&root);
    std::uint32_t serial = 100;
    float x = 0, y = 0;
    auto down = [&](bool touch) {
        Node::mapToScene(row, 20, 20, x, y);
        ++serial;
        assert(dispatcher.pointerButton(x, y, BTN_LEFT, true, serial, serial * 10, touch));
        return serial;
    };
    auto up = [&](bool touch) {
        ++serial;
        assert(dispatcher.pointerButton(x, y, BTN_LEFT, false, serial, serial * 10, touch));
        assert(!dispatcher.pointerCaptured() && !row->pressed());
    };

    // No opt-in: retain legacy deferred DOWN and stationary replay on UP.
    const auto deferredSerial = down(true);
    assert(presses == 0 && releases == 0 && menus == 0 && !armed);
    assert(!dispatcher.pointerCaptured() && !row->pressed());
    up(true);
    assert(presses == 1 && releases == 1 && menus == 1 && !armed);
    assert(pressSerial == deferredSerial && clickSerial == deferredSerial);

    row->setPassiveTouchPress(true);
    const auto immediateSerial = down(true);
    assert(dispatcher.pointerCaptured() && row->pressed() && !viewport->pressed());
    assert(presses == 2 && releases == 1 && menus == 1 && armed);
    assert(pressSerial == immediateSerial);
    up(true);
    assert(presses == 2 && releases == 2 && menus == 2 && !armed);
    assert(clickSerial == immediateSerial && clickSerial != serial);

    // Subthreshold native motion still reaches the original passive row's app tap guard.
    {
        const int beforeMenus = menus, beforeMotions = motions, beforeReleases = releases;
        const float beforeScroll = scroll->scrollOffset();
        down(true);
        assert(armed && pressX == 20 && pressY == 20);
        x += 6;
        dispatcher.pointerMotion(x, y, ++serial);
        assert(motions == beforeMotions + 1 && motionX == pressX + 6);
        assert(dispatcher.pointerCaptured() && row->pressed() && !viewport->pressed());
        assert(!armed && scroll->scrollOffset() == beforeScroll);
        x -= 6;
        dispatcher.pointerMotion(x, y, ++serial);
        assert(motions == beforeMotions + 2 && motionX == pressX);
        assert(dispatcher.pointerCaptured() && row->pressed() && !viewport->pressed());
        assert(!armed && scroll->scrollOffset() == beforeScroll);
        up(true);
        assert(releases == beforeReleases + 1 && !armed && menus == beforeMenus);
    }

    // Stable layout keys retain the press; actual replacement/ABA, hide and exclusion cancel it.
    for (bool touch : {false, true}) {
        const int beforeMenus = menus, beforePresses = presses;
        const auto stableSerial = down(touch);
        assert(dispatcher.pointerCaptured() && row->pressed() && armed);
        bind(binding);
        assert(row->pressed() && armed);
        up(touch);
        assert(menus == beforeMenus + 1 && presses == beforePresses + 1);
        assert(clickSerial == stableSerial);

        for (bool restoreBinding : {false, true}) {
            down(touch);
            const std::string original = binding;
            bind(original + ":replacement");
            if (restoreBinding)
                bind(original);
            assert(!row->pressed() && !armed);
            up(touch);
            assert(menus == beforeMenus + 1);
            bind(original);
        }

        down(touch);
        row->dispatchCancel(); // App pane/row hiding cancels before visibility changes.
        row->setVisible(false);
        row->setVisible(true);
        assert(!row->pressed() && !armed);
        up(touch);
        assert(menus == beforeMenus + 1);

        down(touch);
        row->dispatchCancel();
        row->setParticipatesInLayout(false);
        row->setParticipatesInLayout(true);
        assert(!row->pressed() && !armed);
        up(touch);
        assert(menus == beforeMenus + 1);
    }

    // Child cancellation must not discard the viewport's pending physical-touch pan.
    for (bool rebindBeforePan : {false, true}) {
        scroll->setScrollOffset(0);
        const int beforeMenus = menus, beforePresses = presses, beforeReleases = releases;
        const int beforeCancels = cancels;
        down(true);
        assert(dispatcher.pointerCaptured() && row->pressed() && armed);
        const int beforeMotions = motions;
        x += 6;
        dispatcher.pointerMotion(x, y, ++serial);
        assert(motions == beforeMotions + 1 && !armed);
        assert(dispatcher.pointerCaptured() && row->pressed() && !viewport->pressed());
        if (rebindBeforePan) {
            bind(binding + ":pan-replacement");
            assert(!row->pressed() && !armed);
        }
        const float beforeScroll = scroll->scrollOffset();
        y -= 24;
        dispatcher.pointerMotion(x, y, ++serial);
        assert(dispatcher.pointerCaptured() && viewport->pressed() && !row->pressed());
        assert(!armed && cancels > beforeCancels);
        assert(scroll->scrollOffset() > beforeScroll && menus == beforeMenus);
        up(true);
        assert(!viewport->pressed() && menus == beforeMenus);
        assert(presses == beforePresses + 1 && releases == beforeReleases);
    }

    // Destruction clears native capture; the replacement must not receive replay or stray UP.
    scroll->setScrollOffset(0);
    const int beforePresses = presses, beforeReleases = releases, beforeMenus = menus;
    down(true);
    assert(row->pressed() && dispatcher.pointerCaptured());
    content->removeChild(row).reset();
    armed = false; // The app retires its binding state along with the row.
    auto replacementOwner = std::make_unique<InputArea>();
    replacementOwner->setPosition(0, 30);
    replacementOwner->setSize(220, 80);
    replacementOwner->setPassiveTouchPress(true);
    int replacementPresses = 0, replacementReleases = 0, replacementMenus = 0;
    replacementOwner->setOnPress([&](const InputArea::PointerData &data) {
        data.pressed ? ++replacementPresses : ++replacementReleases;
    });
    replacementOwner->setOnClick([&](const InputArea::PointerData &) { ++replacementMenus; });
    row = static_cast<InputArea *>(content->addChild(std::move(replacementOwner)));
    assert(!dispatcher.pointerCaptured());
    // Make the new row the hovered target as well, to catch unmatched-release fallback.
    dispatcher.syncPointerHover();
    assert(dispatcher.hoveredArea() == row);
    ++serial;
    dispatcher.pointerButton(x, y, BTN_LEFT, false, serial, serial * 10, true);
    assert(!dispatcher.pointerCaptured() && !row->pressed());
    assert(replacementPresses == 0 && replacementReleases == 0 && replacementMenus == 0);
    assert(presses == beforePresses + 1 && releases == beforeReleases && menus == beforeMenus);

    // A passive native Button must clear its pressed colors on pan, without a release or animator.
    scroll->setScrollOffset(0);
    auto buttonOwner = std::make_unique<Button>();
    buttonOwner->setPosition(0, 30);
    buttonOwner->setSize(220, 80);
    buttonOwner->updateInputArea();
    const auto normalColor = rgba(0.0F, 1.0F, 0.0F);
    const auto pressedColor = rgba(1.0F, 0.0F, 0.0F);
    Button::ButtonPalette palette;
    palette.normal.label.fixed = normalColor;
    palette.hover.label.fixed = normalColor;
    palette.pressed.label.fixed = pressedColor;
    buttonOwner->setCustomPalette(palette);
    int buttonClicks = 0;
    buttonOwner->setOnClick([&] { ++buttonClicks; });
    assert(!buttonOwner->inputArea()->passiveTouchPress());
    buttonOwner->inputArea()->setPassiveTouchPress(true);
    auto *button = static_cast<Button *>(content->addChild(std::move(buttonOwner)));
    assert(button->animationManager() == nullptr && button->foregroundColor() == normalColor);
    Node::mapToScene(button->inputArea(), 20, 20, x, y);
    ++serial;
    assert(dispatcher.pointerButton(x, y, BTN_LEFT, true, serial, serial * 10, true));
    assert(dispatcher.pointerCaptured() && button->inputArea()->pressed() && !viewport->pressed());
    assert(button->foregroundColor() == pressedColor && buttonClicks == 0);
    const float beforeButtonPan = scroll->scrollOffset();
    y -= 24;
    dispatcher.pointerMotion(x, y, ++serial);
    assert(dispatcher.pointerCaptured() && viewport->pressed() && !button->inputArea()->pressed());
    assert(scroll->scrollOffset() > beforeButtonPan);
    assert(button->foregroundColor() == normalColor && buttonClicks == 0);
    dispatcher.setSceneRoot(nullptr);
}

static void checkToolbarPointerFocus() {
    InputDispatcher dispatcher;
    Node root;
    root.setSize(240, 120);
    auto inputOwner = std::make_unique<Input>();
    inputOwner->setSize(220, 40);
    inputOwner->inputArea()->setSize(220, 40);
    inputOwner->setValue("draft");
    auto *input = static_cast<Input *>(root.addChild(std::move(inputOwner)));
    auto buttonOwner = std::make_unique<Button>();
    buttonOwner->setPosition(0, 60);
    buttonOwner->setSize(100, 40);
    buttonOwner->inputArea()->setSize(100, 40);
    int clicks = 0;
    buttonOwner->setOnClick([&] { ++clicks; });
    buttonOwner->inputArea()->setFocusOnPointerPress(false);
    auto *button = static_cast<Button *>(root.addChild(std::move(buttonOwner)));
    assert(button->inputArea()->focusable() && button->inputArea()->tabStop());
    dispatcher.setSceneRoot(&root);
    dispatcher.setFocus(input->inputArea());
    TextInputClient &client = *input;
    const TextInputEdit preedit{.commitText = {},
                                .preeditText = "猫",
                                .preeditCursorBegin = 3,
                                .preeditCursorEnd = 3,
                                .hasPreedit = true};
    client.textInputApplyEdit(preedit);
    const auto heldState = input->editingState();
    const auto heldText = input->value();
    const auto heldSurroundingText = client.textInputState().surroundingText;
    assert(input->composing() && heldState.preeditBase && heldState.preeditLength == 3);
    assert(heldText.substr(heldState.preeditStart, heldState.preeditLength) == "猫");
    std::uint32_t serial = 0;
    auto pointer = [&](bool touch, bool pressed) {
        ++serial;
        assert(dispatcher.pointerButton(20, 80, BTN_LEFT, pressed, serial, serial * 10, touch));
        assert(dispatcher.pointerCaptured() == pressed);
        assert(button->inputArea()->pressed() == pressed);
    };
    for (bool touch : {false, true}) {
        const int beforeClicks = clicks;
        for (bool pressed : {true, false}) {
            pointer(touch, pressed);
            assert(dispatcher.focusedArea() == input->inputArea());
            assert(input->inputArea()->focused() && !button->inputArea()->focused());
            assert(input->composing() && input->editingStateMatches(heldState));
            assert(input->value() == heldText);
            assert(client.textInputState().surroundingText == heldSurroundingText);
            assert(clicks == beforeClicks + (pressed ? 0 : 1));
        }
    }
    assert(dispatcher.cycleTabFocus(false));
    assert(dispatcher.focusedArea() == button->inputArea());
    assert(button->inputArea()->focusable() && button->inputArea()->tabStop());
    for (bool touch : {false, true}) {
        const int beforeClicks = clicks;
        for (bool pressed : {true, false}) {
            pointer(touch, pressed);
            assert(dispatcher.focusedArea() == button->inputArea());
            assert(button->inputArea()->focused() && !input->inputArea()->focused());
            assert(clicks == beforeClicks + (pressed ? 0 : 1));
        }
    }
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

static void checkCancelPendingScrollRequests() {
    ScaledRenderer renderer;
    AnimationManager animations;
    ScrollView scroll;
    scroll.setViewportPaddingH(0);
    scroll.setViewportPaddingV(0);
    scroll.setScrollbarVisible(false);
    auto contentOwner = std::make_unique<Box>();
    contentOwner->setSize(240, 600);
    auto *content = scroll.content()->addChild(std::move(contentOwner));
    auto targetOwner = std::make_unique<Node>();
    targetOwner->setPosition(0, 520);
    targetOwner->setSize(220, 40);
    auto *target = content->addChild(std::move(targetOwner));
    const LayoutRect viewport{.width = 240, .height = 140};
    scroll.arrange(renderer, viewport);
    assert(scroll.maxScrollOffset() > 200);
    scroll.setScrollOffset(60);
    const float heldOffset = scroll.scrollOffset();
    assert(heldOffset == 60);

    scroll.requestScrollToOffset(200);
    scroll.requestScrollIntoView(target);
    scroll.cancelPendingScrollRequests();
    scroll.arrange(renderer, viewport);
    assert(scroll.scrollOffset() == heldOffset);

    scroll.requestScrollToBottom();
    scroll.requestScrollIntoView(target);
    scroll.cancelPendingScrollRequests();
    scroll.arrange(renderer, viewport);
    assert(scroll.scrollOffset() == heldOffset);
    assert(scroll.scrollOffset() < scroll.maxScrollOffset());

    const bool motionEnabled = MotionService::instance().enabled();
    MotionService::instance().setEnabled(true);
    scroll.setAnimationManager(&animations);
    scroll.requestScrollToOffset(200); // Obsolete restore precedes the new user scroll.
    scroll.requestScrollIntoView(target);
    scroll.scrollBy(80);
    assert(animations.hasActive() && scroll.scrollOffset() == heldOffset);
    scroll.cancelPendingScrollRequests();
    assert(scroll.scrollOffset() == heldOffset && animations.hasActive());
    scroll.arrange(renderer, viewport);
    assert(scroll.scrollOffset() == heldOffset && animations.hasActive());
    scroll.setScrollOffset(heldOffset);
    assert(!animations.hasActive());
    MotionService::instance().setEnabled(motionEnabled);
}

static void checkKeyedParentFocusFallback() {
    InputDispatcher dispatcher;
    Node root;
    auto addArea = [](Node &parent, const std::string &key) {
        auto owner = std::make_unique<InputArea>();
        owner->setFocusable(true);
        owner->setTabStop(true);
        owner->setTabFocusKey(key);
        return static_cast<InputArea *>(parent.addChild(std::move(owner)));
    };
    auto *unrelated = addArea(root, "");
    auto *outer = addArea(root, "transcript");
    auto *message = addArea(*outer, "message:A");
    auto *intermediate = addArea(*message, "message:A:non-tab-stop");
    intermediate->setTabStop(false);
    auto *action = addArea(*intermediate, "message:A:action");
    dispatcher.setSceneRoot(&root);
    dispatcher.setFocus(action);
    auto snapshot = dispatcher.captureTabFocus();

    dispatcher.setFocus(unrelated);
    dispatcher.restoreTabFocus(snapshot);
    assert(dispatcher.focusedArea() == action && action->focused());
    assert(!message->focused() && !intermediate->focused() && !unrelated->focused());

    dispatcher.stashTabFocus();
    intermediate->removeChild(action).reset();
    dispatcher.setFocus(unrelated);
    dispatcher.restoreTabFocus(snapshot);
    assert(dispatcher.focusedArea() == message && message->focused());
    assert(!outer->focused() && !intermediate->focused() && !unrelated->focused());

    dispatcher.setFocus(unrelated);
    dispatcher.restoreStashedTabFocus();
    assert(dispatcher.focusedArea() == message && message->focused());
    assert(!outer->focused() && !intermediate->focused() && !unrelated->focused());

    dispatcher.setFocus(nullptr);
    message->setVisible(false);
    snapshot.index = 0; // A missing keyed target must not fall through to position.
    dispatcher.restoreTabFocus(snapshot);
    assert(dispatcher.focusedArea() == nullptr);
    assert(!unrelated->focused() && !outer->focused() && !message->focused());

    // An unkeyed control without a keyed ancestor still restores by position.
    assert(dispatcher.cycleTabFocus(false));
    assert(dispatcher.focusedArea() == unrelated && unrelated->focused());
    auto unkeyedSnapshot = dispatcher.captureTabFocus();
    assert(unkeyedSnapshot.index == 0 && !unkeyedSnapshot.key);
    dispatcher.setFocus(outer);
    dispatcher.restoreTabFocus(unkeyedSnapshot);
    assert(dispatcher.focusedArea() == unrelated && unrelated->focused());
    assert(!outer->focused());

    // A keyed ancestor must not displace an unchanged unkeyed child.
    auto *unkeyedChild = addArea(*outer, "");
    dispatcher.setFocus(unkeyedChild);
    auto childSnapshot = dispatcher.captureTabFocus();
    dispatcher.setFocus(unrelated);
    dispatcher.restoreTabFocus(childSnapshot);
    assert(dispatcher.focusedArea() == unkeyedChild && unkeyedChild->focused());
    assert(!outer->focused() && !unrelated->focused());
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

static void checkInlineGhost() {
    RenderContext context;
    ScaledRenderer renderer(context, 1.25F);
    Input input;
    input.setMultiline(true);
    input.setSize(180, 90);
    int projections = 0, edits = 0;
    input.setDisplayProjector([&](std::string_view raw) {
        ++projections;
        Input::DisplayProjection result;
        result.text = std::string(raw);
        result.markup = "<b>" + result.text + "</b>";
        result.contrastMarkup = result.markup;
        for (std::size_t i = 0; i <= raw.size(); ++i)
            result.rawToDisplay.push_back(i);
        return result;
    });
    input.setValue("/msg 猫\nlast");
    input.setOnEditingStateChanged([&] { ++edits; });
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&input);
    dispatcher.setFocus(input.inputArea());
    input.layout(renderer);
    TextNode *ghost = nullptr;
    Label *label = nullptr;
    for (const auto &child : input.children())
        for (const auto &layer : child->children()) {
            if (layer->type() == NodeType::Text)
                ghost = static_cast<TextNode *>(layer.get());
            if (auto *candidate = dynamic_cast<Label *>(layer.get()))
                label = candidate;
        }
    assert(ghost && label && ghost->parent()->clipChildren());
    assert(!ghost->participatesInLayout() && !ghost->hitTestVisible());
    const auto before = input.editingState();
    const auto undo = before.history->undo, redo = before.history->redo;
    const auto surrounding = input.textInputState().surroundingText;
    const auto projection = *input.displayProjection();
    const auto height = input.measuredMultilineHeight();
    const auto labelX = label->x(), labelY = label->y();
    const int projectionCount = projections;
    input.setInlineGhost(" <target> <message> a very long suffix that must clip");
    input.layout(renderer);
    assert(ghost->visible() && !ghost->useMarkup() && ghost->maxWidth() == 0);
    assert(ghost->font() == label->font());
    std::vector<TextCursorStop> stops;
    renderer.measureTextCaretStops(projection.markup, label->fontSize(), {projection.text.size()},
                                   ghost->parent()->width(), stops, label->font(), true);
    assert(stops.size() == 1);
    assert(std::abs(ghost->x() - stops[0].x) < 0.01F);
    assert(std::abs(ghost->y() - (label->y() + label->baselineOffset() + stops[0].lineY)) < 0.01F);
    assert(ghost->color().a < label->color().a);
    assert(input.editingStateMatches(before) && input.value() == before.current.value);
    assert(before.history->undo == undo && before.history->redo == redo && edits == 0);
    assert(projections == projectionCount && input.displayProjection()->text == projection.text);
    assert(input.displayProjection()->markup == projection.markup);
    assert(input.displayProjection()->rawToDisplay == projection.rawToDisplay);
    assert(input.textInputState().surroundingText == surrounding);
    assert(input.measuredMultilineHeight() == height && input.height() == 90);
    assert(label->x() == labelX && label->y() == labelY);

    input.moveCaretLeft();
    assert(!ghost->visible());
    input.moveCaretRight();
    assert(ghost->visible());
    input.moveCaretLeft(true);
    assert(!ghost->visible() && input.cursorPosition() != input.selectionAnchor());
    const auto selection = input.editingState();
    input.setInlineGhost(" <selected>");
    assert(input.editingStateMatches(selection) && !ghost->visible());
    input.restoreEditingState(before);
    input.layout(renderer);
    assert(ghost->visible());
    input.textInputApplyEdit({.commitText = {},
                              .preeditText = "猫",
                              .preeditCursorBegin = 3,
                              .preeditCursorEnd = 3,
                              .hasPreedit = true});
    input.layout(renderer);
    assert(input.composing() && !ghost->visible());
    const auto composition = input.editingState();
    input.setInlineGhost(" <other>");
    assert(input.editingStateMatches(composition) && !ghost->visible());
    input.textInputResetPreedit();
    input.layout(renderer);
    assert(!input.composing() && ghost->visible() && input.value() == before.current.value);

    class Clipboard final : public TextClipboard {
      public:
        std::string text;
        std::optional<std::string> clipboardText() override { return text; }
        void setClipboardText(std::string value) override { text = std::move(value); }
    } clipboard;
    auto *previousClipboard = Input::textClipboard();
    Input::setTextClipboard(&clipboard);
    input.selectAll();
    dispatcher.keyEvent('c', 0, KeyMod::Ctrl, true);
    assert(clipboard.text == before.current.value && !ghost->visible());
    Input::setTextClipboard(previousClipboard);
    input.restoreEditingState(before);
    input.layout(renderer);
    input.insertText("x");
    input.setInlineGhost(" <replacement>");
    dispatcher.keyEvent('z', 0, KeyMod::Ctrl, true);
    assert(input.value() == before.current.value);
    dispatcher.keyEvent('y', 0, KeyMod::Ctrl, true);
    assert(input.value() == before.current.value + "x");
    input.layout(renderer);
    assert(ghost->visible());
    dispatcher.setFocus(nullptr);
    assert(!ghost->visible());
    dispatcher.setFocus(input.inputArea());
    input.layout(renderer);
    assert(ghost->visible());
    input.setInlineGhost("");
    assert(!ghost->visible());
    dispatcher.setSceneRoot(nullptr);
}

// Real headless Pango shaping, unlike an uninitialized RenderContext's zero-width fallback.
class ShapedInputRenderer final : public Renderer {
    CairoTextRenderer text;
    static TextMetrics metrics(const CairoTextRenderer::TextMetrics &m) {
        return {m.width,     m.left,         m.right,          m.top,         m.bottom,
                m.inkTop,    m.inkBottom,    m.inkLeft,        m.inkRight,    m.capHeight,
                m.lineCount, m.alignOffsetX, m.layoutBaseline, m.layoutHeight};
    }

  public:
    ShapedInputRenderer() { text.initialize(nullptr, nullptr); }
    float renderScale() const noexcept override { return 1.25F; }
    TextMetrics measureText(std::string_view value, float size, const TextFont &font = {},
                            float width = 0, int lines = 0, TextAlign align = TextAlign::Start,
                            TextEllipsize ellipsize = TextEllipsize::End,
                            bool markup = false) override {
        return metrics(
            text.measure(renderScale(), value, size, font, width, lines, align, ellipsize, markup));
    }
    TextMetrics measureFont(float size, const TextFont &font = {}) override {
        return metrics(text.measureFont(renderScale(), size, font));
    }
    void measureTextCursorStops(std::string_view value, float size,
                                const std::vector<std::size_t> &bytes, std::vector<float> &out,
                                const TextFont &font = {}) override {
        text.measureCursorStops(renderScale(), value, size, bytes, out, font);
    }
    void measureTextCursorStopsWrapped(std::string_view value, float size,
                                       const std::vector<std::size_t> &bytes, float width,
                                       std::vector<TextCursorStop> &out, const TextFont &font = {},
                                       bool markup = false) override {
        text.measureCursorStopsWrapped(renderScale(), value, size, bytes, width, out, font, false,
                                       markup);
    }
    void measureTextCaretStops(std::string_view value, float size,
                               const std::vector<std::size_t> &bytes, float width,
                               std::vector<TextCursorStop> &out, const TextFont &font = {},
                               bool markup = false) override {
        text.measureCursorStopsWrapped(renderScale(), value, size, bytes, width, out, font, true,
                                       markup);
    }
    void measureTextCharSpans(std::string_view value, float size,
                              const std::vector<std::size_t> &bytes, std::vector<TextCharSpan> &out,
                              const TextFont &font = {}) override {
        text.measureCharSpans(renderScale(), value, size, bytes, out, font);
    }
    TextMetrics measureGlyph(char32_t codepoint, float size) override {
        char buffer[6];
        const auto length = g_unichar_to_utf8(codepoint, buffer);
        return measureText(std::string_view(buffer, length), size);
    }
    TextureManager &textureManager() override {
        throw std::logic_error("Headless input shaping does not allocate GPU textures");
    }
};

static void checkInputRangeAndCells() {
    ShapedInputRenderer renderer;
    Input input;
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&input);
    dispatcher.setFocus(input.inputArea());
    bool decorated = false;
    bool multiline = true;
    input.setDisplayProjector([&](std::string_view raw) {
        auto doc = blast::parseIrcFormatting(raw, Color{1, 1, 1, 1}, Color{0, 0, 0, 1});
        Input::DisplayProjection projection;
        for (const auto &run : doc.runs) {
            const auto text = std::string_view(doc.text).substr(run.start, run.end - run.start);
            blast::irc_detail::appendMarkup(projection.markup, text, run.style, true);
            blast::irc_detail::appendMarkup(projection.contrastMarkup, text, run.style, false);
        }
        if (decorated) {
            projection.markup = "<span underline=\"error\">" + projection.markup + "</span>";
            projection.contrastMarkup =
                "<span underline=\"error\">" + projection.contrastMarkup + "</span>";
        }
        projection.text = std::move(doc.text);
        projection.rawToDisplay = std::move(doc.rawToDisplay);
        return projection;
    });
    auto verifyCells = [&] {
        input.layout(renderer);
        Label *label = nullptr;
        for (const auto &child : input.children())
            for (const auto &layer : child->children())
                if (auto *candidate = dynamic_cast<Label *>(layer.get()))
                    label = candidate;
        assert(label);
        Input::DisplayProjection plain;
        const bool projected = input.displayProjection() != nullptr;
        if (!projected) {
            plain.text = plain.markup = input.value();
            for (std::size_t byte = 0; byte <= plain.text.size(); ++byte)
                plain.rawToDisplay.push_back(byte);
        }
        const auto &projection = projected ? *input.displayProjection() : plain;
        std::vector<std::size_t> bytes;
        for (std::size_t i = 0; i < projection.text.size(); ++i)
            if ((static_cast<unsigned char>(projection.text[i]) & 0xc0) != 0x80)
                bytes.push_back(i);
        bytes.push_back(projection.text.size());
        std::vector<TextCursorStop> cells;
        const float wrap = multiline ? label->parent()->width() : 0;
        renderer.measureTextCaretStops(projection.markup, label->fontSize(), bytes, wrap, cells,
                                       label->font(), projected);
        const auto metrics =
            renderer.measureText(projection.markup, label->fontSize(), label->font(), wrap, 0,
                                 TextAlign::Start, TextEllipsize::End, projected);
        const float originY =
            label->parent()->y() + label->y() + label->baselineOffset() -
            std::floor(metrics.layoutBaseline * renderer.renderScale()) / renderer.renderScale();
        const auto state = input.editingState();
        const float insetY = multiline ? label->y() + state.scrollY : 0.0F;
        const float viewportTop = label->parent()->y() + insetY;
        const float viewportBottom = label->parent()->y() + label->parent()->height() - insetY;
        int hits = 0;
        for (std::size_t i = 0; i + 1 < cells.size(); ++i) {
            const auto &cell = cells[i];
            if (cell.cellIsVirtual || cell.cellWidth <= 0)
                continue;
            const float cellLeft =
                label->parent()->x() + cell.cellX - (multiline ? 0 : state.scrollX);
            const float left = std::max(cellLeft, label->parent()->x());
            const float right = std::min(cellLeft + cell.cellWidth,
                                         label->parent()->x() + label->parent()->width());
            const float top = std::max(originY + cell.lineY, viewportTop);
            const float bottom = std::min(originY + cell.lineY + cell.lineHeight, viewportBottom);
            if (left >= right || top >= bottom)
                continue;
            const float x = (left + right) * 0.5F, y = (top + bottom) * 0.5F;
            const auto found = input.textByteAtPoint(x, y);
            auto expected =
                static_cast<std::size_t>(std::upper_bound(projection.rawToDisplay.begin(),
                                                          projection.rawToDisplay.end(), bytes[i]) -
                                         projection.rawToDisplay.begin() - 1);
            while (expected > 0 &&
                   (static_cast<unsigned char>(input.value()[expected]) & 0xc0) == 0x80)
                --expected;
            if (!found || *found != expected)
                std::fprintf(
                    stderr,
                    "input cell mismatch: multiline=%d projected=%d size=%.1fx%.1f cell=%zu "
                    "display=%zu expectedRaw=%zu foundRaw=%zu found=%d point=%.3f,%.3f "
                    "originY=%.3f scroll=%.3f,%.3f width=%.3f text=%s\n",
                    multiline, projected, input.width(), input.height(), i, bytes[i], expected,
                    found.value_or(0), found.has_value(), x, y, originY, state.scrollX,
                    state.scrollY, wrap, projection.text.c_str());
            assert(found && *found == static_cast<std::size_t>(expected));
            for (const float fraction : {0.01F, 0.99F}) {
                const float edgeY = top + (bottom - top) * fraction;
                assert(input.textByteAtPoint(x, edgeY) == found);
            }
            if (multiline) {
                assert(!input.textByteAtPoint(x, viewportTop * 0.5F));
                assert(!input.textByteAtPoint(x, (viewportBottom + input.height()) * 0.5F));
            }
            ++hits;
        }
        assert(hits > 0);
        assert(!input.textByteAtPoint(-1, input.height() / 2));
        assert(!input.textByteAtPoint(input.width() + 1, input.height() / 2));
        assert(!input.textByteAtPoint(input.width() / 2, -1));
        assert(!input.textByteAtPoint(input.width() / 2, input.height() + 1));
    };
    input.setMultiline(true);
    input.setSize(160, 150);
    input.setValue("\x02"
                   "café"
                   "\x02"
                   " אבג 猫 wrap words across lines");
    verifyCells();
    input.setSize(100, 45);
    verifyCells();
    assert(input.editingState().scrollY > 0);
    input.setMultiline(false);
    multiline = false;
    input.setSize(100, 40);
    input.setValue("café אבג 猫 long words scrolling");
    verifyCells();
    assert(input.editingState().scrollX > 0);

    input.setMultiline(true);
    multiline = true;
    input.setSize(250, 90);
    input.setValue("\x02"
                   "café"
                   "\x02"
                   " tail");
    input.setInlineGhost(" ghost");
    input.layout(renderer);
    bool sawGhost = false;
    for (const auto &child : input.children())
        for (const auto &layer : child->children())
            if (auto *ghost = dynamic_cast<TextNode *>(layer.get()); ghost && ghost->visible()) {
                sawGhost = true;
                assert(!input.textByteAtPoint(child->x() + ghost->x() + 3,
                                              child->y() + ghost->y() - 2));
            }
    assert(sawGhost);
    auto selected = input.editingState();
    selected.current.cursorPos = selected.current.value.size();
    selected.current.selectionAnchor = 7;
    input.restoreEditingState(selected);
    input.layout(renderer);
    const auto before = input.editingState();
    const auto undoCount = before.history->undo.size();
    int changes = 0, rewrites = 0;
    input.setOnChange([&](const std::string &) { ++changes; });
    input.setEditRewriter(
        [&](const Input::EditSnapshot &old, Input::EditSnapshot &next, bool preedit) {
            assert(!preedit && old == before.current);
            assert(next.value == "\x02"
                                 "coffee"
                                 "\x02"
                                 " tail");
            ++rewrites;
        });
    assert(!input.replaceRange(4, 5, "x")); // Interior of é.
    assert(!input.replaceRange(5, 1, "x"));
    assert(!input.replaceRange(0, input.value().size() + 1, "x"));
    assert(!input.replaceRange(1, 6, std::string_view("\xff", 1)));
    assert(input.editingStateMatches(before));
    input.setEditValidator([](std::string_view, std::string_view, bool) { return false; });
    assert(!input.replaceRange(1, 6, "coffee"));
    assert(input.editingStateMatches(before) && before.history->undo.size() == undoCount);
    input.setEditValidator({});
    assert(input.replaceRange(1, 6, "coffee"));
    assert(input.value() == "\x02"
                            "coffee"
                            "\x02"
                            " tail");
    assert(input.cursorPosition() == 7 && input.selectionAnchor() == 7 && changes == 1);
    assert(before.history->undo.size() == undoCount + 1 && rewrites == 2);
    dispatcher.keyEvent('z', 0, KeyMod::Ctrl, true);
    assert(input.editingState().current == before.current);
    input.setEditRewriter({});
    input.setEditValidator([](std::string_view, std::string_view, bool) { return false; });
    assert(!input.replaceRange(1, 6, "")); // Deletions must also reach the validator.
    assert(input.editingState().current == before.current);
    input.setEditValidator({});
    input.layout(renderer);
    const auto paintState = input.editingState();
    decorated = true;
    input.refreshDisplayProjection(true);
    assert(!input.textGeometryDirty() && input.editingStateMatches(paintState));
    assert(input.displayProjection()->markup.find("underline=\"error\"") != std::string::npos);
    TextInputClient &client = input;
    client.textInputApplyEdit({.commitText = {},
                               .preeditText = "猫",
                               .preeditCursorBegin = 3,
                               .preeditCursorEnd = 3,
                               .hasPreedit = true});
    input.layout(renderer);
    const auto composing = input.editingState();
    assert(input.composing() && !input.replaceRange(1, 6, "coffee"));
    assert(!input.textByteAtPoint(20, 20) && input.editingStateMatches(composing));
    client.textInputResetPreedit();
    input.setDisplayProjector({});
    input.setInlineGhost("");
    input.setSize(400, 120);
    input.setValue("A猫 café אבג");
    verifyCells();
    input.setMultiline(false);
    multiline = false;
    verifyCells();
    input.setFontSize(24);
    verifyCells();
    input.setFontFeatures("kern=0");
    verifyCells();
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
}

int main() {
    setApplicationIdentity({.name = "blastirc", .envPrefix = "BLASTIRC"});
    Input input;
    input.setMultiline(true);
    TextInputClient &client = input;
    auto captured = input.editingState();
    std::string published;
    input.setOnEditingStateChanged([&] {
        if (input.editingStateMatches(captured))
            return;
        captured = input.editingState();
        published = captured.preeditBase ? captured.preeditBase->value : captured.current.value;
    });

    const TextInputEdit preedit{.commitText = {},
                                .preeditText = "ab",
                                .preeditCursorBegin = 2,
                                .preeditCursorEnd = 2,
                                .hasPreedit = true};
    client.textInputApplyEdit(preedit);
    assert(input.value() == "ab" && published.empty());
    const auto beforePartial = captured;
    const TextInputEdit partial{.commitText = "a",
                                .preeditText = "b",
                                .preeditCursorBegin = 1,
                                .preeditCursorEnd = 1,
                                .hasCommitText = true,
                                .hasPreedit = true};
    client.textInputApplyEdit(partial);
    assert(input.value() == beforePartial.current.value);
    assert(input.cursorPosition() == beforePartial.current.cursorPos);
    assert(input.selectionAnchor() == beforePartial.current.selectionAnchor);
    assert(!input.editingStateMatches(beforePartial));
    assert(published == "a" && client.textInputState().surroundingText == "a");
    assert(captured.preeditStart == 1 && captured.preeditLength == 1);
    const auto conversation = captured;

    Input::EditingState other;
    other.current = {"other", 4, 1};
    input.restoreEditingState(other);
    assert(client.textInputState().cursor == 4 && client.textInputState().anchor == 1);
    input.restoreEditingState(conversation);
    assert(input.value() == "ab" && published == "a" && input.composing());
    client.textInputResetPreedit();
    assert(input.value() == "a" && published == "a" && !input.composing());
    assert(client.textInputState().cursor == 1 && client.textInputState().anchor == 1);

    // Cancelling composition restores the original UTF-8 selection, not just its text.
    auto selected = input.editingState();
    selected.current = {"a猫z", 4, 1};
    input.restoreEditingState(selected);
    client.textInputApplyEdit(preedit);
    assert(input.value() == "aabz");
    client.textInputResetPreedit();
    assert(input.value() == "a猫z" && published == "a猫z");
    assert(input.cursorPosition() == 4 && input.selectionAnchor() == 1);

    // Exercise the real retained caret and focus dispatch, without a mock renderer.
    RectNode *caret = nullptr;
    for (const auto &child : input.children())
        for (const auto &layer : child->children())
            if (layer->type() == NodeType::Rect && layer->zIndex() == 1) {
                assert(!caret);
                caret = static_cast<RectNode *>(layer.get());
            }
    assert(caret && !caret->visible());
    const bool motionEnabled = MotionService::instance().enabled();
    MotionService::instance().setEnabled(false);
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&input);
    dispatcher.setFocus(input.inputArea());
    assert(input.inputArea()->focused() && !caret->visible()); // Selection remains intact.
    input.clearSelection();
    assert(caret->visible() && input.value() == "a猫z");
    input.moveCaretLeft();
    assert(input.cursorPosition() == 1 && caret->visible());
    input.moveCaretRight();
    assert(input.cursorPosition() == 4 && caret->visible() && input.value() == "a猫z");

    const auto beforeHidden = input.editingState();
    const TextInputEdit hidden{.commitText = {},
                               .preeditText = "猫",
                               .preeditCursorBegin = -1,
                               .preeditCursorEnd = -1,
                               .hasPreedit = true};
    client.textInputApplyEdit(hidden);
    assert(input.value() == "a猫猫z" && !caret->visible());
    const auto hiddenState = input.editingState();
    assert(hiddenState.preeditCaretHidden && published == "a猫z");
    const TextInputEdit visible{.commitText = {},
                                .preeditText = "猫",
                                .preeditCursorBegin = 3,
                                .preeditCursorEnd = 3,
                                .hasPreedit = true};
    client.textInputApplyEdit(visible);
    assert(input.value() == hiddenState.current.value && caret->visible());
    assert(input.cursorPosition() == hiddenState.current.cursorPos);
    assert(!input.editingStateMatches(hiddenState));
    input.restoreEditingState(hiddenState);
    assert(!caret->visible());
    client.textInputResetPreedit();
    assert(caret->visible() && input.value() == beforeHidden.current.value);
    assert(input.cursorPosition() == beforeHidden.current.cursorPos);
    input.selectAll();
    assert(!caret->visible());
    input.restoreEditingState(beforeHidden);
    assert(caret->visible() && input.value() == "a猫z");

    Button button;
    int keyboardClicks = 0;
    button.setOnClick([&] { ++keyboardClicks; });
    KeybindMatcher::setMatcher(KeybindAction::Validate,
                               [](std::uint32_t sym, std::uint32_t modifiers) {
                                   return sym == XKB_KEY_Return && modifiers == 0;
                               });
    button.setTabStop(true);
    dispatcher.setSceneRoot(&button);
    dispatcher.setFocus(button.inputArea());
    assert(!caret->visible() && button.inputArea()->focused());
    dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
    dispatcher.keyEvent(XKB_KEY_Return, 0, 0, false);
    assert(keyboardClicks == 1);
    input.restoreEditingState(beforeHidden);
    assert(!caret->visible() && dispatcher.focusedArea() == button.inputArea());
    dispatcher.setSceneRoot(&input);
    dispatcher.setFocus(input.inputArea());
    dispatcher.keyEvent('x', 'x', 0, true);
    assert(caret->visible() &&
           input.value() == "a猫xz"); // First key after native focus is not lost.
    dispatcher.setFocus(nullptr);
    dispatcher.setSceneRoot(nullptr);
    assert(!caret->visible());
    checkClippedCapture(false);
    checkClippedCapture(true);
    checkClippedSelection();
    checkPassiveTouchRow();
    checkDeferredTouchOriginalIdentity();
    checkToolbarPointerFocus();
    checkCancelPendingScrollRequests();
    checkKeyedParentFocusFallback();
    checkContextMenuHierarchyAndOutsideRelease();
    checkWaylandSeatTouchMouseIsolation();
    checkInlineGhost();
    checkInputRangeAndCells();
    MotionService::instance().setEnabled(motionEnabled);
}
