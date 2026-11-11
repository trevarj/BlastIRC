#include "ui/controls/context_menu_popup.h"

#include "core/deferred_call.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "render/render_context.h"
#include "render/render_target.h"
#include "render/scene/node.h"
#include "ui/controls/scroll_view.h"
#include "ui/popup_chrome.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <wayland-client-protocol.h>

namespace {

  constexpr Logger kLog("context-menu-popup");

} // namespace

ContextMenuPopup* ContextMenuPopup::s_openMenu = nullptr;

ContextMenuPopup::ContextMenuPopup(WaylandConnection& wayland, RenderContext& renderContext)
    : m_wayland(wayland), m_renderContext(renderContext) {}

ContextMenuPopup::~ContextMenuPopup() {
  if (m_alive != nullptr) {
    *m_alive = false;
  }
  close();
}

void ContextMenuPopup::open(ContextMenuPopupRequest request) {
  close();
  m_alive = std::make_shared<bool>(true);
  if (!request.menu.empty()) {
    m_ownedMenu = std::make_shared<const std::vector<ContextMenuEntry>>(std::move(request.menu));
    m_level = m_ownedMenu.get();
  } else if (m_parentMenu == nullptr) {
    m_ownedMenu.reset();
    m_level = nullptr;
  }
  if (m_level != nullptr) {
    request.entries.clear();
    request.entries.reserve(m_level->size());
    for (std::size_t i = 0; i < m_level->size(); ++i) {
      auto row = static_cast<const ContextMenuControlEntry&>((*m_level)[i]);
      row.id = static_cast<std::int32_t>(i);
      row.hasSubmenu = !(*m_level)[i].children.empty();
      request.entries.push_back(std::move(row));
    }
  }
  auto rows = std::move(request.entries);
  m_childRequest = request;
  request.entries = std::move(rows);
  m_childRequest.inputSerial = request.inputSerial.value_or(m_wayland.lastInputSerial());
  m_childRequest.pointerContextMenu = false;

  // maxVisible caps the popup viewport; all entries remain reachable via scroll.
  const std::size_t maxVisible =
      request.maxVisible > 0 ? request.maxVisible : std::max<std::size_t>(1, request.entries.size());
  const float contentScale = std::max(0.1F, request.contentScale);
  const float menuHeight = ContextMenuControl::preferredHeight(request.entries, maxVisible, contentScale);
  float menuWidth = request.menuWidth;
  if (menuWidth <= 0.0F) {
    float measureScale = 1.0F;
    if (request.parent.output != nullptr) {
      if (const WaylandOutput* out = m_wayland.findOutputByWl(request.parent.output); out != nullptr) {
        measureScale = out->configuredScale();
      }
    }
    ScaledRenderer measureRenderer(m_renderContext, measureScale);
    menuWidth = ContextMenuControl::preferredWidth(measureRenderer, request.entries, contentScale, false, request.font);
    if (request.maxMenuWidth > 0.0F) {
      menuWidth = std::min(menuWidth, request.maxMenuWidth);
    }
    if (request.minMenuWidth > 0.0F) {
      menuWidth = std::max(menuWidth, request.minMenuWidth);
    }
  }
  const auto chrome =
      popup_chrome::computeGeometry(menuWidth, menuHeight, m_shadowConfig, Style::popupShadowsEnabled());
  m_scrollState = {};
  m_scrollView = nullptr;
  m_menu = nullptr;
  m_highlightedIndex = request.initialHighlight < request.entries.size() ? request.initialHighlight : 0;

  const ContextMenuPopupPlacement defaultPlacement{
      .anchor = XDG_POSITIONER_ANCHOR_BOTTOM,
      .gravity = XDG_POSITIONER_GRAVITY_BOTTOM,
      .offsetX = 0,
      .offsetY = static_cast<std::int32_t>(Style::spaceXs),
      .chromeAttachment = popup_chrome::Attachment{
          .horizontal = popup_chrome::HorizontalAttachment::Center, .vertical = popup_chrome::VerticalAttachment::Top
      },
  };
  const ContextMenuPopupPlacement resolvedPlacement = request.placement.value_or(defaultPlacement);

  PopupSurfaceConfig popupCfg{
      .anchorX = request.anchor.x,
      .anchorY = request.anchor.y,
      .anchorWidth = std::max(1, request.anchor.width),
      .anchorHeight = std::max(1, request.anchor.height),
      .width = chrome.surfaceWidth,
      .height = chrome.surfaceHeight,
      .anchor = resolvedPlacement.anchor,
      .gravity = resolvedPlacement.gravity,
      .constraintAdjustment = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X
          | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y
          | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X
          | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y
          | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X
          | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y,
      .offsetX = resolvedPlacement.offsetX,
      .offsetY = resolvedPlacement.offsetY,
      .serial = request.inputSerial.value_or(m_wayland.lastInputSerial()),
      .grab = true,
  };
  if (request.pointerContextMenu) {
    // Encode each shadow edge in the anchor rectangle, not an offset: XDG flips anchors/gravity,
    // but does not mirror offsets. Either opening direction then touches the invocation point.
    popupCfg.anchorX = request.anchor.x - chrome.bleed.left;
    popupCfg.anchorY = request.anchor.y - chrome.bleed.up;
    popupCfg.anchorWidth = std::max(1, chrome.bleed.left + chrome.bleed.right);
    popupCfg.anchorHeight = std::max(1, chrome.bleed.up + chrome.bleed.down);
    popupCfg.anchor = XDG_POSITIONER_ANCHOR_TOP_LEFT;
    popupCfg.gravity = XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
    popupCfg.offsetX = 0;
    popupCfg.offsetY = 0;
  } else {
    popup_chrome::applyToConfig(popupCfg, chrome, resolvedPlacement.chromeAttachment);
  }

  m_surface = std::make_unique<PopupSurface>(m_wayland);
  m_surface->setRenderContext(&m_renderContext);

  auto* self = this;

  m_surface->setConfigureCallback([self](std::uint32_t /*w*/, std::uint32_t /*h*/) {
    self->m_surface->requestLayout();
  });

  m_surface->setPrepareFrameCallback([self, entries = std::move(request.entries), chrome, contentScale,
                                      font = std::move(request.font)](bool /*needsUpdate*/, bool needsLayout) {
    if (self->m_surface == nullptr) {
      return;
    }

    const auto width = self->m_surface->width();
    const auto height = self->m_surface->height();
    if (width == 0 || height == 0) {
      return;
    }

    self->m_renderContext.makeCurrent(self->m_surface->renderTarget());

    const bool needsSceneBuild = self->m_sceneRoot == nullptr
        || static_cast<std::uint32_t>(std::round(self->m_sceneRoot->width())) != width
        || static_cast<std::uint32_t>(std::round(self->m_sceneRoot->height())) != height;
    if (!needsSceneBuild && !needsLayout) {
      return;
    }

    UiPhaseScope layoutPhase(UiPhase::Layout);

    if (self->m_menu != nullptr) {
      self->m_highlightedIndex = self->m_menu->highlightedIndex();
    }

    const auto fw = static_cast<float>(width);
    const auto fh = static_cast<float>(height);
    // The compositor may resize the preferred menu to its available constraint box.
    auto fittedChrome = chrome;
    fittedChrome.surfaceWidth = width;
    fittedChrome.surfaceHeight = height;
    fittedChrome.bleed.left = std::min(chrome.bleed.left, static_cast<std::int32_t>(width - 1));
    fittedChrome.bleed.right =
        std::min(chrome.bleed.right, static_cast<std::int32_t>(width - 1) - fittedChrome.bleed.left);
    fittedChrome.bleed.up = std::min(chrome.bleed.up, static_cast<std::int32_t>(height - 1));
    fittedChrome.bleed.down =
        std::min(chrome.bleed.down, static_cast<std::int32_t>(height - 1) - fittedChrome.bleed.up);
    fittedChrome.contentWidth = fw - static_cast<float>(fittedChrome.bleed.left + fittedChrome.bleed.right);
    fittedChrome.contentHeight = fh - static_cast<float>(fittedChrome.bleed.up + fittedChrome.bleed.down);

    self->m_inputDispatcher.setSceneRoot(nullptr);
    self->m_sceneRoot = std::make_unique<Node>();
    self->m_sceneRoot->setSize(fw, fh);
    self->m_sceneRoot->setClipChildren(true);
    if (Style::popupShadowsEnabled()) {
      popup_chrome::addShadow(
          *self->m_sceneRoot, fittedChrome, self->m_shadowConfig, Style::scaledRadiusLg(contentScale)
      );
    }
    popup_chrome::addCardBackground(*self->m_sceneRoot, fittedChrome, contentScale);

    auto scrollView = std::make_unique<ScrollView>();
    scrollView->setPosition(fittedChrome.contentX(), fittedChrome.contentY());
    scrollView->setSize(fittedChrome.contentWidth, fittedChrome.contentHeight);
    scrollView->setContentScale(contentScale);
    scrollView->setViewportPaddingH(0.0F);
    scrollView->setViewportPaddingV(0.0F);
    scrollView->clearFill();
    scrollView->clearBorder();
    scrollView->setRadius(0.0F);
    scrollView->bindState(&self->m_scrollState);
    scrollView->setScrollbarVisible(true);
    scrollView->setScrollbarInsetV(Style::scaledRadiusLg(contentScale));

    auto ctrl = std::make_unique<ContextMenuControl>();
    ContextMenuControl* menuPtr = ctrl.get();
    ctrl->setContentScale(contentScale);
    ctrl->setFont(font);
    ctrl->setMenuWidth(fittedChrome.contentWidth);
    // Keep every entry in scroll content, even when the compositor shrinks the viewport.
    ctrl->setMaxVisible(entries.size());
    ctrl->setEntries(entries);
    ctrl->setHighlightedIndex(self->m_highlightedIndex);
    self->m_highlightedIndex = ctrl->highlightedIndex();
    ctrl->setRedrawCallback([self] {
      if (self->m_surface)
        self->m_surface->requestRedraw();
    });
    ctrl->setOnActivate([self](const ContextMenuControlEntry& e) { self->deferActivation(e); });
    ctrl->setOnSubmenuOpen([self](const ContextMenuControlEntry& entry, float y) { self->openSubmenu(entry, y); });
    ctrl->setOnHighlightChanged([self](const ContextMenuControlEntry& entry) {
      if (self->m_child && self->m_childIndex != static_cast<std::size_t>(entry.id)) {
        self->m_child.reset();
        self->m_childIndex = static_cast<std::size_t>(-1);
      }
    });
    scrollView->content()->addChild(std::move(ctrl));
    ScrollView* scrollPtr = scrollView.get();
    scrollView->layout(self->m_surface->renderTarget().renderer());

    self->m_scrollView = scrollPtr;
    self->m_menu = menuPtr;
    self->ensureHighlightedVisible();

    self->m_sceneRoot->addChild(std::move(scrollView));
    self->m_inputDispatcher.setSceneRoot(self->m_sceneRoot.get());
    self->m_inputDispatcher.setCursorShapeCallback([self](std::uint32_t serial, std::uint32_t shape) {
      self->m_wayland.setCursorShape(serial, shape);
    });
    self->m_surface->setSceneRoot(self->m_sceneRoot.get());
    popup_chrome::setContentInputRegion(*self->m_surface, fittedChrome);
  });

  m_surface->setDismissedCallback([self] { self->deferClose(); });

  // A parent that refuses keyboard focus (a bar) must accept it before the grabbing popup maps,
  // or the popup gets no keyboard focus and ESC cannot reach it.
  if (request.parent.keyboardGrab) {
    m_releaseParentKeyboard = request.parent.keyboardGrab();
  }

  const auto lifetime = m_alive;
  const bool initialized = request.parent.xdgSurface != nullptr
      ? m_surface->initializeAsChild(request.parent.xdgSurface, request.parent.output, popupCfg)
      : m_surface->initialize(request.parent.attach, request.parent.output, popupCfg);
  // Native initialization roundtrips may dispatch input that replaces/destroys this level.
  if (!*lifetime) {
    return;
  }
  if (!initialized) {
    kLog.warn("failed to create context menu popup");
    close();
    return;
  }

  m_wlSurface = m_surface->wlSurface();
  m_pointerParentSurface = request.pointerParentSurface;
  s_openMenu = rootMenu();
}

void ContextMenuPopup::close() {
  if (m_alive) {
    *m_alive = false;
  }
  m_child.reset();
  m_childIndex = static_cast<std::size_t>(-1);
  const bool wasOpen = m_surface != nullptr;
  if (s_openMenu == this) {
    s_openMenu = nullptr;
  }
  restoreParentKeyboardInteractivity();
  m_inputDispatcher.setSceneRoot(nullptr);
  m_menu = nullptr;
  m_scrollView = nullptr;
  m_sceneRoot.reset();
  m_surface.reset();
  m_wlSurface = nullptr;
  m_pointerParentSurface = nullptr;
  m_pointerInside = false;
  if (wasOpen && m_onDismissed) {
    m_onDismissed();
  }
}

bool ContextMenuPopup::isOpen() const noexcept { return m_surface != nullptr; }

void ContextMenuPopup::deferActivation(ContextMenuControlEntry entry) {
  auto* self = rootMenu();
  const std::weak_ptr<bool> alive = self->m_alive;
  const std::weak_ptr<bool> sourceAlive = m_alive;
  auto onActivate = self->m_onActivate;
  DeferredCall::callLater([self, alive, sourceAlive, onActivate = std::move(onActivate), entry = std::move(entry)] {
    const auto token = alive.lock();
    const auto sourceToken = sourceAlive.lock();
    if (token == nullptr || !*token || sourceToken == nullptr || !*sourceToken) {
      return;
    }
    // Close before running the action. The action may open another popup (e.g. the
    // color picker for a "Custom" entry); that popup must be created against the
    // now-topmost parent, and this menu cannot be destroyed while it still has a
    // child popup on top. Activating first violates the xdg_popup topmost rule.
    self->close();
    if (onActivate) {
      onActivate(entry);
    }
  });
}

void ContextMenuPopup::deferClose() {
  auto* self = rootMenu();
  const std::weak_ptr<bool> alive = self->m_alive;
  const std::weak_ptr<bool> sourceAlive = m_alive;
  DeferredCall::callLater([self, alive, sourceAlive] {
    const auto token = alive.lock();
    const auto sourceToken = sourceAlive.lock();
    if (token != nullptr && *token && sourceToken != nullptr && *sourceToken) {
      self->close();
    }
  });
}

void ContextMenuPopup::setOnActivate(std::function<void(const ContextMenuControlEntry&)> callback) {
  m_onActivate = std::move(callback);
}

void ContextMenuPopup::setOnDismissed(std::function<void()> callback) { m_onDismissed = std::move(callback); }

void ContextMenuPopup::setShadowConfig(const ShadowStyle& shadow) {
  if (m_shadowConfig == shadow) {
    return;
  }
  m_shadowConfig = shadow;
  if (isOpen()) {
    close();
  }
}

bool ContextMenuPopup::onPointerEvent(const PointerEvent& event) {
  if (m_parentMenu == nullptr
      && event.type == PointerEvent::Type::Button
      && m_swallowButton == event.button
      && !event.pressed) {
    m_swallowButton = 0;
    return true;
  }
  if (!isOpen()) {
    return false;
  }
  const bool captured = m_inputDispatcher.pointerCaptured();
  if (m_child && (m_child->hasPointerCapture() || (!captured && m_child->containsSurface(event.surface)))) {
    return m_child->onPointerEvent(event);
  }
  if (m_parentMenu == nullptr
      && event.type == PointerEvent::Type::Button
      && event.pressed
      && !containsSurface(event.surface)) {
    m_swallowButton = event.button;
    close();
    return true;
  }

  const bool onPopup = (event.surface != nullptr && event.surface == m_wlSurface);
  auto localX = static_cast<float>(event.sx);
  auto localY = static_cast<float>(event.sy);
  // While a press holds the pointer capture (e.g. a scrollbar thumb drag), events sliding onto the
  // parent surface are translated into popup-local coordinates so the drag keeps tracking instead
  // of being cut off at the popup edge.
  bool mapped = onPopup;
  if (!onPopup && captured && event.surface != nullptr) {
    float offsetX = 0.0F;
    float offsetY = 0.0F;
    for (auto* level = this; level != nullptr && level->m_surface != nullptr; level = level->m_parentMenu) {
      offsetX += static_cast<float>(level->m_surface->configuredX());
      offsetY += static_cast<float>(level->m_surface->configuredY());
      if (event.surface == level->m_pointerParentSurface) {
        localX -= offsetX;
        localY -= offsetY;
        mapped = true;
        break;
      }
    }
    offsetX = 0.0F;
    offsetY = 0.0F;
    for (auto* level = m_child.get(); !mapped && level != nullptr && level->m_surface != nullptr;
         level = level->m_child.get()) {
      offsetX += static_cast<float>(level->m_surface->configuredX());
      offsetY += static_cast<float>(level->m_surface->configuredY());
      if (event.surface == level->m_wlSurface) {
        localX += offsetX;
        localY += offsetY;
        mapped = true;
      }
    }
  }

  switch (event.type) {
  case PointerEvent::Type::Enter:
    if (onPopup) {
      m_pointerInside = true;
      m_inputDispatcher.pointerEnter(localX, localY, event.serial);
    } else if (mapped) {
      // Parent-surface enter during a captured drag: swallow it so the parent's hover
      // states don't react while the drag owns the pointer.
      return true;
    }
    break;
  case PointerEvent::Type::Leave:
    if (onPopup) {
      m_pointerInside = false;
      // A captured drag survives leaving the surface; the deferred leave is delivered on release.
      if (!captured) {
        m_inputDispatcher.pointerLeave();
      }
    }
    break;
  case PointerEvent::Type::Motion:
    if (mapped || (event.surface == nullptr && m_pointerInside)) {
      if (onPopup) {
        m_pointerInside = true;
      }
      m_inputDispatcher.pointerMotion(localX, localY, 0);
      return true;
    }
    break;
  case PointerEvent::Type::Button:
    if (mapped || (event.surface == nullptr && m_pointerInside)) {
      if (onPopup) {
        m_pointerInside = true;
      }
      const bool pressed = event.pressed;
      m_inputDispatcher.pointerButton(localX, localY, event.button, pressed, event.serial, event.time, event.touch);
      if (!pressed && captured && !onPopup) {
        m_pointerInside = false;
        m_inputDispatcher.pointerLeave();
      }
      return true;
    }
    break;
  case PointerEvent::Type::Axis:
    if (onPopup || (event.surface == nullptr && m_pointerInside)) {
      if (onPopup) {
        m_pointerInside = true;
      }
      const bool consumed = m_inputDispatcher.pointerAxis(
          localX, localY, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120,
          event.axisLines
      );
      if (m_surface != nullptr && m_sceneRoot != nullptr) {
        if (m_sceneRoot->layoutDirty()) {
          m_surface->requestLayout();
        } else if (m_sceneRoot->paintDirty() || consumed) {
          m_surface->requestRedraw();
        }
      }
      return consumed || onPopup;
    }
    break;
  }

  if (m_surface != nullptr && m_sceneRoot != nullptr && m_surface->isRunning()) {
    m_surface->requestRedraw();
  }

  return onPopup;
}

void ContextMenuPopup::onKeyboardEvent(const KeyboardEvent& event) {
  if (!isOpen() || !event.pressed || event.preedit) {
    return;
  }
  if (m_child && m_child->isOpen()) {
    m_child->onKeyboardEvent(event);
    return;
  }

  const std::uint32_t sym = event.sym;
  const std::uint32_t modifiers = event.modifiers;

  if (KeybindMatcher::matches(KeybindAction::Cancel, sym, modifiers)) {
    deferClose();
    return;
  }

  if (KeySymbol::isLeft(sym) && m_parentMenu != nullptr) {
    close();
    return;
  }

  if (m_menu == nullptr) {
    return;
  }

  if (KeySymbol::isRight(sym)) {
    const auto index = m_menu->highlightedIndex();
    if (m_level != nullptr && index < m_level->size() && !(*m_level)[index].children.empty()) {
      auto row = static_cast<const ContextMenuControlEntry&>((*m_level)[index]);
      row.id = static_cast<std::int32_t>(index);
      openSubmenu(row, (m_menu->rowTop(index) + m_menu->rowBottom(index)) * 0.5F);
    }
    return;
  }
  if (KeybindMatcher::matches(KeybindAction::Down, sym, modifiers)) {
    if (m_menu->moveHighlight(1)) {
      m_highlightedIndex = m_menu->highlightedIndex();
      ensureHighlightedVisible();
      requestVisualUpdate();
    }
  } else if (KeybindMatcher::matches(KeybindAction::Up, sym, modifiers)) {
    if (m_menu->moveHighlight(-1)) {
      m_highlightedIndex = m_menu->highlightedIndex();
      ensureHighlightedVisible();
      requestVisualUpdate();
    }
  } else if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
    m_menu->activateHighlighted();
  } else if (KeySymbol::isHome(sym)) {
    m_menu->setHighlightedIndex(0);
    m_highlightedIndex = m_menu->highlightedIndex();
    ensureHighlightedVisible();
    requestVisualUpdate();
  } else if (KeySymbol::isEnd(sym) && m_menu->entryCount() > 0) {
    m_menu->setHighlightedIndex(m_menu->entryCount() - 1);
    m_highlightedIndex = m_menu->highlightedIndex();
    ensureHighlightedVisible();
    requestVisualUpdate();
  }
}

void ContextMenuPopup::ensureHighlightedVisible() {
  if (m_menu == nullptr || m_scrollView == nullptr || !m_scrollView->scrollable()) {
    return;
  }
  const std::size_t index = m_menu->highlightedIndex();
  const float rowTop = m_menu->rowTop(index);
  const float rowBottom = m_menu->rowBottom(index);
  const float viewportH = m_scrollView->contentViewportHeight();
  const float offset = m_scrollView->scrollOffset();
  if (rowTop < offset) {
    m_scrollView->setScrollOffset(rowTop);
  } else if (rowBottom > offset + viewportH) {
    m_scrollView->setScrollOffset(rowBottom - viewportH);
  }
}

void ContextMenuPopup::requestVisualUpdate() {
  if (m_surface == nullptr || m_sceneRoot == nullptr) {
    return;
  }
  if (m_sceneRoot->layoutDirty()) {
    m_surface->requestLayout();
  } else {
    m_surface->requestRedraw();
  }
}

void ContextMenuPopup::restoreParentKeyboardInteractivity() {
  if (auto release = std::exchange(m_releaseParentKeyboard, {}); release) {
    release();
  }
}

bool ContextMenuPopup::dispatchKeyboardEvent(const KeyboardEvent& event) {
  if (s_openMenu == nullptr || !s_openMenu->isOpen()) {
    return false;
  }
  auto* target = s_openMenu;
  while (target->m_child && target->m_child->isOpen()) {
    target = target->m_child.get();
  }
  target->onKeyboardEvent(event);
  return true;
}

wl_surface* ContextMenuPopup::wlSurface() const noexcept { return m_wlSurface; }

xdg_surface* ContextMenuPopup::xdgSurface() const noexcept {
  return m_surface != nullptr ? m_surface->xdgSurface() : nullptr;
}

std::uint32_t ContextMenuPopup::width() const noexcept { return m_surface != nullptr ? m_surface->width() : 0; }

std::uint32_t ContextMenuPopup::height() const noexcept { return m_surface != nullptr ? m_surface->height() : 0; }

ContextMenuPopup* ContextMenuPopup::rootMenu() noexcept {
  auto* root = this;
  while (root->m_parentMenu != nullptr) {
    root = root->m_parentMenu;
  }
  return root;
}

bool ContextMenuPopup::containsSurface(wl_surface* surface) const noexcept {
  return surface != nullptr && ((isOpen() && surface == m_wlSurface) || (m_child && m_child->containsSurface(surface)));
}

void ContextMenuPopup::openSubmenu(const ContextMenuControlEntry& entry, float rowCenterY) {
  const auto index = static_cast<std::size_t>(entry.id);
  if (m_level == nullptr
      || index >= m_level->size()
      || (*m_level)[index].children.empty()
      || !entry.enabled
      || m_surface == nullptr
      || m_scrollView == nullptr) {
    return;
  }
  if (m_child && m_child->isOpen() && m_childIndex == index) {
    return;
  }
  m_child.reset();
  m_childIndex = index;
  auto request = m_childRequest;
  request.initialHighlight = static_cast<std::size_t>(-1);
  request.parent.xdgSurface = xdgSurface();
  request.parent.attach = nullptr;
  request.parent.keyboardGrab = {};
  request.pointerParentSurface = m_wlSurface;
  request.anchor = {
      .x = static_cast<std::int32_t>(m_scrollView->x()),
      .y = static_cast<std::int32_t>(m_scrollView->y() + rowCenterY - m_scrollView->scrollOffset()),
      .width = std::max(1, static_cast<std::int32_t>(m_scrollView->width())),
      .height = 1,
  };
  request.placement = ContextMenuPopupPlacement{
      .anchor = XDG_POSITIONER_ANCHOR_RIGHT,
      .gravity = XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT,
      .chromeAttachment = {
          .horizontal = popup_chrome::HorizontalAttachment::Left,
          .vertical = popup_chrome::VerticalAttachment::Top,
      },
  };
  m_child = std::make_unique<ContextMenuPopup>(m_wayland, m_renderContext);
  m_child->m_parentMenu = this;
  m_child->m_ownedMenu = m_ownedMenu;
  m_child->m_level = &(*m_level)[index].children;
  m_child->setShadowConfig(m_shadowConfig);
  m_child->open(std::move(request));
}

bool ContextMenuPopup::hasPointerCapture() const noexcept {
  return m_inputDispatcher.pointerCaptured() || (m_child && m_child->hasPointerCapture());
}
