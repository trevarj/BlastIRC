#include "ui/controls/input.h"

#include "core/deferred_call.h"
#include "core/input/key_chord.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/text_clipboard.h"
#include "cursor-shape-v1-client-protocol.h"
#include "render/animation/animation.h"
#include "render/animation/animation_manager.h"
#include "render/animation/motion_service.h"
#include "render/core/color.h"
#include "render/core/render_styles.h"
#include "render/core/renderer.h"
#include "render/scene/glyph_node.h"
#include "render/scene/input_area.h"
#include "render/scene/rect_node.h"
#include "render/scene/text_node.h"
#include "render/text/glyph_registry.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/text_input_service.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <glib.h>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <wayland-client-protocol.h>

namespace {

  TextClipboard* g_clipboard = nullptr;
  Input::PasswordMaskStyle g_passwordMaskStyle = Input::PasswordMaskStyle::CircleFilled;
  bool g_readlineShortcuts = false;
  std::function<bool(std::uint32_t, std::uint32_t)> g_validateKeyMatcher;

  constexpr float kMinWidth = 48.0F;
  constexpr float kCursorWidth = 2.0F;
  constexpr float kCursorPadV = 4.0F;
  constexpr float kCursorMinHeight = 14.0F;
  constexpr float kCursorHeightRatio = 0.50F;
  constexpr float kCursorRevealPadding = 2.0F;
  constexpr float kClearGlyphScale = 0.85F;
  constexpr auto kCursorBlinkInterval = std::chrono::milliseconds(530);
  constexpr auto kCursorBlinkResumeDelay = std::chrono::milliseconds(650);
  constexpr float kTextInnerInset = 3.0F;
  constexpr float kPlaceholderAlpha = 0.68F;
  constexpr float kPrimaryPlaceholderAlpha = 0.50F;
  constexpr float kPasswordGlyphScale = 0.82F;
  constexpr auto kDoubleClickThreshold = std::chrono::milliseconds(400);
  constexpr float kDoubleClickDistance = 6.0F;
  constexpr std::size_t kUndoStackLimit = 100;
  // Keep in sync with cairo_text_renderer single-texture width clip (GL_MAX_TEXTURE_SIZE).
  constexpr float kMaxLabelRasterWidth = 8192.0F - 64.0F;
  constexpr auto kTypingUndoCoalesceWindow = std::chrono::milliseconds(1000);
  // Multiline mode: vertical text inset, wheel step in lines, and the stub
  // highlight width (in em) marking a selected line break.
  constexpr float kMultilinePadV = Style::spaceSm;
  constexpr float kWheelScrollLines = 3.0F;
  constexpr float kSelectionLineBreakStubEm = 0.4F;

  float chromeScaleForControlHeight(float controlHeight) noexcept {
    return std::max(0.1F, controlHeight / Style::controlHeight);
  }

  bool isWordCodepoint(const std::string& text, std::size_t bytePos) {
    if (bytePos >= text.size()) {
      return false;
    }

    const auto lead = static_cast<unsigned char>(text[bytePos]);
    if ((lead & 0x80U) != 0) {
      return true;
    }
    return std::isalnum(lead) != 0 || lead == '_';
  }

  char32_t passwordMaskCodepointForIndex(std::size_t index) {
    if (g_passwordMaskStyle == Input::PasswordMaskStyle::CircleFilled) {
      return GlyphRegistry::lookup("circle-filled");
    }
    static const std::array<char32_t, 7> randomCodepoints = {
        GlyphRegistry::lookup("circle-filled"),        GlyphRegistry::lookup("pentagon-filled"),
        GlyphRegistry::lookup("michelin-star-filled"), GlyphRegistry::lookup("square-rounded-filled"),
        GlyphRegistry::lookup("guitar-pick-filled"),   GlyphRegistry::lookup("blob-filled"),
        GlyphRegistry::lookup("triangle-filled"),
    };
    return randomCodepoints[index % randomCodepoints.size()];
  }

  void layoutPasswordMaskGlyph(
      Renderer& renderer, GlyphNode& glyph, char32_t codepoint, float glyphSize, float cellX, float cellSize,
      float inputHeight
  ) {
    glyph.setCodepoint(codepoint);
    glyph.setFontSize(glyphSize);
    glyph.setHitTestVisible(false);
    const auto metrics = renderer.measureGlyph(codepoint, glyphSize);
    // Shared horizontal em grid; per-glyph ink center on a common row line.
    const float cellCenterX = cellX + cellSize * 0.5F;
    const float emCenterX = glyphSize * 0.5F;
    const float inkCenterY = (metrics.top + metrics.bottom) * 0.5F;
    const float rowCenterY = inputHeight * 0.5F;
    glyph.setPosition(cellCenterX - emCenterX, rowCenterY - inkCenterY);
    // The node origin is the glyph baseline, so the ink center is the pivot animated scale/rotation must use.
    glyph.setTransformOrigin((metrics.left + metrics.right) * 0.5F, inkCenterY);
  }

  Color resolved(ColorRole role, float alpha = 1.0F) { return colorForRole(role, alpha); }

  bool isUtf8ContinuationByte(unsigned char value) noexcept { return (value & 0xC0U) == 0x80U; }

  std::size_t clampToUtf8Start(const std::string& text, std::size_t pos) {
    pos = std::min(pos, text.size());
    while (pos > 0 && pos < text.size() && isUtf8ContinuationByte(static_cast<unsigned char>(text[pos]))) {
      --pos;
    }
    return pos;
  }

  std::size_t clampToUtf8End(const std::string& text, std::size_t pos) {
    pos = std::min(pos, text.size());
    while (pos < text.size() && isUtf8ContinuationByte(static_cast<unsigned char>(text[pos]))) {
      ++pos;
    }
    return pos;
  }

  std::int32_t byteOffsetToProtocolInt(std::size_t value) {
    return static_cast<std::int32_t>(
        std::min<std::size_t>(value, static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
    );
  }

} // namespace

Input::Input() {
  // 0: background
  auto bg = std::make_unique<RectNode>();
  m_background = static_cast<RectNode*>(addChild(std::move(bg)));

  // 1: text viewport clip layer
  auto textViewport = std::make_unique<Node>();
  textViewport->setClipChildren(true);
  textViewport->setHitTestVisible(false);
  m_textViewport = addChild(std::move(textViewport));

  // Text-layer children:
  // 0: selection highlight (rendered behind text)
  auto sel = std::make_unique<RectNode>();
  sel->setStyle(
      RoundedRectStyle{
          .fill = resolved(ColorRole::Primary),
          .fillMode = FillMode::Solid,
          .radius = 2.0F,
      }
  );
  sel->setOpacity(0.3F);
  sel->setVisible(false);
  m_selectionRect = static_cast<RectNode*>(m_textViewport->addChild(std::move(sel)));

  // 1: text
  auto label = std::make_unique<Label>();
  label->setFontSize(m_fontSize);
  label->setMaxLines(1);
  label->setColor(colorSpecFromRole(ColorRole::OnSurface));
  m_label = static_cast<Label*>(m_textViewport->addChild(std::move(label)));

  // 2: cursor
  auto cursor = std::make_unique<RectNode>();
  cursor->setStyle(
      RoundedRectStyle{
          .fill = resolved(ColorRole::Primary),
          .fillMode = FillMode::Solid,
          .radius = 0.0F,
          .noAa = true,
      }
  );
  cursor->setVisible(false);
  cursor->setZIndex(1);
  cursor->setHitTestVisible(false);
  m_cursor = static_cast<RectNode*>(m_textViewport->addChild(std::move(cursor)));

  auto ghost = std::make_unique<TextNode>();
  ghost->setParticipatesInLayout(false);
  ghost->setHitTestVisible(false);
  ghost->setMaxLines(1);
  ghost->setVisible(false);
  m_inlineGhost = static_cast<TextNode*>(m_textViewport->addChild(std::move(ghost)));

  // Full-field input area.
  auto area = std::make_unique<InputArea>();
  area->setFocusable(true);
  area->setTextInputClient(this);
  area->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
  area->setOnEnter([this](const InputArea::PointerData& /*data*/) { applyVisualState(); });
  area->setOnLeave([this] { applyVisualState(); });
  area->setOnFocusGain([this] {
    revealCursor();
    startCursorBlink();
    applyVisualState();
    m_revealCursorYPending = true;
    markLayoutDirty();
    // Focus gain is delivered before pointer press in the same dispatch pass.
    // Defer scroll-to-caret so click placement sees the blurred scroll offset (0).
    const std::weak_ptr<int> alive = m_aliveToken;
    DeferredCall::callLater([this, alive] {
      if (alive.expired() || m_inputArea == nullptr || !m_inputArea->focused()) {
        return;
      }
      requestCaretUpdate();
    });
    if (m_onFocusGain) {
      m_onFocusGain();
    }
  });
  area->setOnFocusLoss([this] {
    const bool removedPreedit = removePreeditText();
    stopCursorBlink();
    updateCursorVisibility();
    if (!m_multiline) {
      m_scrollOffset = 0.0F;
    }
    applyVisualState();
    markLayoutDirty();
    if (removedPreedit) {
      updateDisplayText();
      markTextContentChanged();
    }
    if (m_submitOnFocusLoss && m_onSubmit) {
      m_onSubmit(m_value);
    }
    if (m_onFocusLoss) {
      m_onFocusLoss();
    }
  });
  area->setOnPress([this](const InputArea::PointerData& data) {
    if (data.button == BTN_RIGHT) {
      m_primaryPointerSelecting = false;
      if (data.pressed && m_onContextMenu) {
        auto callback = m_onContextMenu;
        callback(&data);
      }
      return;
    }
    if (data.pressed) {
      m_primaryPointerSelecting = data.button == BTN_LEFT;
      const float textStartX = m_horizontalPadding + kTextInnerInset;
      std::size_t offset = m_multiline
          ? pointToByteOffset(data.localX - textStartX, data.localY - kMultilinePadV + m_scrollOffsetY)
          : xToByteOffset(data.localX - textStartX + m_scrollOffset - m_contentLeadSlack);
      // Hit the painted composition first, then map its replaced range back to the base.
      if (m_preeditBase || m_preeditLen > 0) {
        const auto end = m_preeditStart + m_preeditLen;
        const auto baseEnd = m_preeditBase
            ? m_preeditBase->value.size() - (m_value.size() - end) : m_preeditStart;
        if (offset >= end)
          offset = baseEnd + (offset - end);
        else if (offset > m_preeditStart)
          offset = m_preeditStart;
      }
      const bool removedPreedit = removePreeditText();
      offset = clampToUtf8End(m_value, offset);
      if (removedPreedit) {
        updateDisplayText();
        markTextContentChanged();
        m_cursorPos = m_selectionAnchor = offset;
        if (m_renderer != nullptr) doLayout(*m_renderer);
      }
      resetUndoCoalescing();
      m_goalCaretX = -1.0F;
      const auto now = std::chrono::steady_clock::now();
      const bool inMultiClickWindow = data.button == BTN_LEFT
          && m_hasLastPrimaryPress
          && now - m_lastPrimaryPressTime <= kDoubleClickThreshold
          && std::abs(data.localX - m_lastPrimaryPressX) <= kDoubleClickDistance
          && std::abs(data.localY - m_lastPrimaryPressY) <= kDoubleClickDistance;

      if (data.button == BTN_LEFT) {
        m_primaryClickCount = inMultiClickWindow ? (m_primaryClickCount >= 3 ? 1 : m_primaryClickCount + 1) : 1;
        m_lastPrimaryPressTime = now;
        m_lastPrimaryPressX = data.localX;
        m_lastPrimaryPressY = data.localY;
        m_hasLastPrimaryPress = true;
      } else {
        m_hasLastPrimaryPress = false;
        m_primaryClickCount = 0;
      }

      if (data.button == BTN_LEFT && m_primaryClickCount == 2) {
        m_pointerSelectGranularity = PointerSelectGranularity::Word;
        selectWordAtByteOffset(offset);
        m_pointerSelectPivotStart = selectionStart();
        m_pointerSelectPivotEnd = selectionEnd();
      } else if (data.button == BTN_LEFT && m_primaryClickCount == 3 &&
                 (!m_multiline || multilineStopsValid())) {
        m_pointerSelectGranularity = PointerSelectGranularity::Line;
        selectLineAtByteOffset(offset);
        m_pointerSelectPivotStart = selectionStart();
        m_pointerSelectPivotEnd = selectionEnd();
      } else {
        m_pointerSelectGranularity = PointerSelectGranularity::Character;
        m_pointerSelectPivotStart = offset;
        m_pointerSelectPivotEnd = offset;
        m_cursorPos = offset;
        m_selectionAnchor = offset;
      }
      requestCaretUpdate();
      revealCursor();
      notifyTextInputStateChanged(TextInputChangeCause::Other);
    }
  });
  area->setOnMotion([this](const InputArea::PointerData& data) {
    if (m_inputArea != nullptr && m_inputArea->pressed() && m_primaryPointerSelecting) {
      if (m_textMetricsDirty) return;
      resetUndoCoalescing();
      m_goalCaretX = -1.0F;
      const float textStartX = m_horizontalPadding + kTextInnerInset;
      if (m_multiline) {
        // Drag near the top/bottom edge pans the viewport a line per motion event.
        const float heightPx = height() > 0.0F ? height() : m_controlHeight;
        const float edgePx = 12.0F;
        if (data.localY <= edgePx) {
          m_scrollOffsetY -= currentLineHeight();
        } else if (data.localY >= heightPx - edgePx) {
          m_scrollOffsetY += currentLineHeight();
        }
        clampScrollOffsetY();
        const std::size_t offset =
            pointToByteOffset(data.localX - textStartX, data.localY - kMultilinePadV + m_scrollOffsetY);
        extendPointerSelectionToByteOffset(offset);
        updateInteractiveGeometry();
        updateCursorVisibility();
        markPaintDirty();
        revealCursor();
        notifyTextInputStateChanged(TextInputChangeCause::Other);
        return;
      }
      const float widthPx = width() > 0.0F ? width() : kMinWidth;
      const float edgePx = std::max(12.0F, m_horizontalPadding);
      const float scrollNudge = std::max(4.0F, textViewportWidth() * 0.02F);
      bool handledByEdgeScroll = false;
      std::size_t offset = m_cursorPos;
      if (data.localX <= edgePx) {
        m_scrollOffset -= scrollNudge;
        offset = prevCharPos(m_value, m_cursorPos);
        handledByEdgeScroll = true;
      } else if (data.localX >= widthPx - edgePx) {
        m_scrollOffset += scrollNudge;
        offset = nextCharPos(m_value, m_cursorPos);
        handledByEdgeScroll = true;
      }
      clampScrollOffset();
      if (!handledByEdgeScroll) {
        offset = xToByteOffset(data.localX - textStartX + m_scrollOffset - m_contentLeadSlack);
      }
      extendPointerSelectionToByteOffset(offset);
      requestCaretUpdate();
      revealCursor();
      notifyTextInputStateChanged(TextInputChangeCause::Other);
    }
  });
  area->setOnAxisHandler([this](const InputArea::PointerData& data) {
    if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL || m_inputArea == nullptr || !m_inputArea->focused()) {
      return false;
    }
    if (m_multiline) {
      // Pan the viewport without moving the caret.
      const float delta = data.scrollDelta(currentLineHeight() * kWheelScrollLines);
      if (delta == 0.0F) {
        return false;
      }
      const float previous = m_scrollOffsetY;
      m_scrollOffsetY += delta;
      clampScrollOffsetY();
      if (std::abs(m_scrollOffsetY - previous) < 0.001F) {
        return false;
      }
      updateInteractiveGeometry();
      markPaintDirty();
      return true;
    }
    const float delta = data.scrollSteps();
    if (delta == 0.0F) {
      return false;
    }
    resetUndoCoalescing();
    // Wheel should move caret through text, not pan viewport directly.
    constexpr int kWheelCaretStep = 1;
    if (delta > 0.0F) {
      for (int i = 0; i < kWheelCaretStep; ++i) {
        m_cursorPos = prevCharPos(m_value, m_cursorPos);
      }
    } else {
      for (int i = 0; i < kWheelCaretStep; ++i) {
        m_cursorPos = nextCharPos(m_value, m_cursorPos);
      }
    }
    m_selectionAnchor = m_cursorPos;
    requestCaretUpdate();
    revealCursor();
    notifyTextInputStateChanged(TextInputChangeCause::Other);
    return true;
  });
  area->setOnKeyDown([this](const InputArea::KeyData& k) { handleKey(k.sym, k.utf32, k.modifiers, k.preedit); });
  m_inputArea = static_cast<InputArea*>(addChild(std::move(area)));

  // Optional clear button, kept above the full-field input area for hit-testing.
  auto clearButtonArea = std::make_unique<InputArea>();
  clearButtonArea->setOnEnter([this](const InputArea::PointerData& /*data*/) { applyVisualState(); });
  clearButtonArea->setOnLeave([this] { applyVisualState(); });
  clearButtonArea->setOnClick([this](const InputArea::PointerData& data) {
    if (data.button == BTN_LEFT) {
      clearFromButton();
    }
  });
  clearButtonArea->setVisible(false);
  m_clearButtonArea = static_cast<InputArea*>(addChild(std::move(clearButtonArea)));

  auto clearGlyph = std::make_unique<GlyphNode>();
  clearGlyph->setCodepoint(GlyphRegistry::lookup("close"));
  clearGlyph->setHitTestVisible(false);
  m_clearButtonGlyph = static_cast<GlyphNode*>(m_clearButtonArea->addChild(std::move(clearGlyph)));

  applyVisualState();
  m_paletteConn = paletteChanged().connect([this] {
    refreshDisplayProjection();
    applyVisualState();
  });
  m_inputBordersConn = Style::inputBordersChanged().connect([this] { applyVisualState(); });
}

Input::~Input() {
  resetCaretEffects();
  if (m_textInputService != nullptr) {
    m_textInputService->clearFocusedClient(this);
  }
}

void Input::setValue(std::string_view value) {
  resetCaretEffects();
  m_value = std::string(value);
  m_cursorPos = m_value.size();
  m_selectionAnchor = m_cursorPos;
  m_preeditStart = 0;
  m_preeditLen = 0;
  m_preeditBase.reset();
  m_preeditCaretHidden = false;
  m_goalCaretX = -1.0F;
  clearEditHistory();
  updateDisplayText();
  notifyTextInputStateChanged(TextInputChangeCause::Other);
  markTextContentChanged();
}

Input::EditingState Input::editingState() const {
  return {{m_value, m_cursorPos, m_selectionAnchor}, m_preeditBase,
          m_preeditStart, m_preeditLen, m_editHistory, m_scrollOffset, m_scrollOffsetY,
          m_preeditCaretHidden};
}

bool Input::editingStateMatches(const EditingState& state) const noexcept {
  return m_value == state.current.value &&
      m_cursorPos == state.current.cursorPos &&
      m_selectionAnchor == state.current.selectionAnchor &&
      m_preeditBase == state.preeditBase &&
      m_preeditStart == state.preeditStart &&
      m_preeditLen == state.preeditLength &&
      m_preeditCaretHidden == state.preeditCaretHidden &&
      m_editHistory == state.history &&
      m_scrollOffset == state.scrollX &&
      m_scrollOffsetY == state.scrollY;
}

void Input::restoreEditingState(const EditingState& state, bool preserveScroll) {
  // Same-document model commands are real caret moves, not buffer switches.
  if (!preserveScroll || state.history != m_editHistory) resetCaretEffects();
  const bool textChanged = m_value != state.current.value;
  const auto previousCursor = m_cursorPos;
  const auto previousAnchor = m_selectionAnchor;
  m_value = state.current.value;
  m_cursorPos = state.current.cursorPos;
  m_selectionAnchor = state.current.selectionAnchor;
  m_preeditBase = state.preeditBase;
  m_preeditCaretHidden = state.preeditBase && state.preeditCaretHidden;
  m_preeditStart = state.preeditStart;
  m_preeditLen = state.preeditLength;
  m_editHistory = state.history ? state.history : std::make_shared<EditHistory>();
  if (!preserveScroll) {
    m_scrollOffset = state.scrollX;
    m_scrollOffsetY = state.scrollY;
  }
  clampEditState();
  resetUndoCoalescing();
  if (textChanged) {
    updateDisplayText();
    markTextContentChanged();
  } else {
    if (m_cursorPos != previousCursor || m_selectionAnchor != previousAnchor)
      m_revealCursorYPending = true;
    markLayoutDirty();
  }
  updateInteractiveGeometry();
  revealCursor();
  notifyTextInputStateChanged(TextInputChangeCause::Other);
}


void Input::setOnEditingStateChanged(std::function<void()> callback) {
  m_onEditingStateChanged = std::move(callback);
}

void Input::setEditValidator(std::function<bool(std::string_view, std::string_view, bool)> validator) {
  m_editValidator = std::move(validator);
}

void Input::setDisplayProjector(std::function<DisplayProjection(std::string_view)> projector) {
  m_displayProjector = std::move(projector);
  refreshDisplayProjection();
}

void Input::refreshDisplayProjection(bool decorationOnly) {
  if (decorationOnly && displayProjected()) {
    auto projection = m_displayProjector(m_value);
    if (projection.text == m_displayProjection.text &&
        projection.rawToDisplay == m_displayProjection.rawToDisplay) {
      m_displayProjection = std::move(projection);
      if (!m_value.empty() || m_placeholder.empty()) m_label->setText(m_displayProjection.markup);
      for (auto& item : m_selectionText)
        if (item.label) item.label->setText(m_displayProjection.contrastMarkup);
      if (m_caretText.label) m_caretText.label->setText(m_displayProjection.contrastMarkup);
      markPaintDirty();
      return;
    }
  }
  resetCaretEffects();
  updateDisplayText();
  markTextContentChanged();
}

std::size_t Input::projectedWordBoundary(std::size_t rawByte, bool forward) const {
  return forward ? nextWordEndForByteOffset(rawByte) : previousWordStartForByteOffset(rawByte);
}

void Input::setEditRewriter(std::function<void(const EditSnapshot&, EditSnapshot&, bool)> rewriter) {
  m_editRewriter = std::move(rewriter);
}

void Input::setCaretStyle(CaretShape shape, CaretEffect effect, float durationMs, float strength) {
  if (shape != CaretShape::Box && shape != CaretShape::Underline) shape = CaretShape::Line;
  if (effect != CaretEffect::Trail && effect != CaretEffect::Comet &&
      effect != CaretEffect::Ribbon && effect != CaretEffect::Pulse) effect = CaretEffect::Off;
  const float duration = input_caret::duration(durationMs), amount = input_caret::strength(strength);
  if (m_caretShape == shape && m_caretEffect == effect &&
      m_caretDuration == duration && m_caretStrength == amount) {
    if (!MotionService::instance().enabled()) {
      m_cursorBlinkTimer.stop();
      m_cursorBlinkVisible = true;
      updateCursorVisibility();
    }
    return;
  }
  resetCaretEffects();
  const bool shapeChanged = m_caretShape != shape;
  m_caretShape = shape;
  m_caretEffect = effect;
  m_caretDuration = duration;
  m_caretStrength = amount;
  if (effect != CaretEffect::Off && m_caretStamps.front() == nullptr) {
    for (auto& stamp : m_caretStamps) {
      auto node = std::make_unique<RectNode>();
      node->setVisible(false);
      node->setHitTestVisible(false);
      stamp = static_cast<RectNode*>(m_textViewport->addChild(std::move(node)));
    }
  }
  if (shapeChanged) m_textMetricsDirty = true;
  applyVisualState();
  updateInteractiveGeometry();
  markLayoutDirty();
}

void Input::resetCaretEffects() {
  if (m_caretAnimation != 0 && animationManager() != nullptr) animationManager()->cancel(m_caretAnimation);
  m_caretAnimation = 0;
  m_caretTrail.reset();
  for (auto* stamp : m_caretStamps) if (stamp != nullptr) stamp->setVisible(false);
}

void Input::setAnimationManager(AnimationManager* manager) {
  if (manager != animationManager()) resetCaretEffects();
  Node::setAnimationManager(manager);
}

void Input::setFontFamily(std::string family) {
  if (m_label->font().family != family) resetCaretEffects();
  m_label->setFontFamily(std::move(family));
  m_logicalStrutDirty = true;
  markTextContentChanged();
}
void Input::setFontFeatures(std::string features) {
  if (m_label->font().features != features) resetCaretEffects();
  m_label->setFontFeatures(std::move(features));
  m_logicalStrutDirty = true;
  markTextContentChanged();
}

void Input::setTextColors(std::optional<Color> foreground, std::optional<Color> selectionBackground,
                          std::optional<Color> selectionForeground, std::optional<Color> caret) {
  if (m_foreground == foreground && m_selectionBackground == selectionBackground &&
      m_selectionForeground == selectionForeground && m_caretColor == caret) return;
  m_foreground = foreground;
  m_selectionBackground = selectionBackground;
  m_selectionForeground = selectionForeground;
  m_caretColor = caret;
  applyVisualState();
  markLayoutDirty();
}

void Input::updateClippedText(Renderer& renderer, RectNode* rect, SelectionText& item, Color color) {
  if (item.clip == nullptr) {
    auto clip = std::make_unique<Node>();
    clip->setClipChildren(true);
    clip->setHitTestVisible(false);
    auto label = std::make_unique<Label>();
    item.label = static_cast<Label*>(clip->addChild(std::move(label)));
    item.clip = m_textViewport->addChild(std::move(clip));
  }
  item.clip->setVisible(rect->visible());
  item.clip->setPosition(rect->x(), rect->y());
  item.clip->setFrameSize(rect->width(), rect->height());
  item.label->setUseMarkup(displayProjected());
  item.label->setText(displayProjected() ? m_displayProjection.contrastMarkup : m_label->text());
  item.label->setFontSize(m_fontSize);
  if (item.label->font().family != m_label->font().family)
    item.label->setFontFamily(m_label->font().family);
  if (item.label->font().features != m_label->font().features)
    item.label->setFontFeatures(m_label->font().features);
  item.label->setFontWeight(m_label->font().weight);
  item.label->setTextAlign(m_label->textAlign());
  item.label->setMaxLines(m_multiline || displayProjected() ? 0 : 1);
  item.label->setMaxWidth(m_label->maxWidth());
  item.label->setColor(color);
  item.label->measure(renderer);
  item.label->setPosition(m_label->x() + m_label->textOriginX() - item.label->textOriginX() - rect->x(),
                          m_label->y() + m_label->baselineOffset() - item.label->baselineOffset() - rect->y());
}

void Input::updateTextOverlays(Renderer& renderer) {
  std::size_t used = 0;
  auto overlay = [&](RectNode* rect) {
    rect->setZIndex(displayProjected() ? 1 : 0);
    if ((!m_selectionForeground && !displayProjected()) || !hasSelection() || passwordMasked() || !rect->visible() ||
        rect->y() + rect->height() <= 0 || rect->y() >= m_textViewport->height()) return;
    if (used == m_selectionText.size()) m_selectionText.push_back({});
    updateClippedText(renderer, rect, m_selectionText[used++],
                      m_selectionForeground.value_or(readableTextColorForBackground(rect->style().fill)));
    m_selectionText[used - 1].clip->setZIndex(displayProjected() ? 2 : 0);
  };
  overlay(m_selectionRect);
  for (auto* rect : m_selectionLineRects) overlay(rect);
  for (; used < m_selectionText.size(); ++used) m_selectionText[used].clip->setVisible(false);
  if (m_caretShape == CaretShape::Box && caretPaintEligible() && !m_value.empty() && !passwordMasked()) {
    updateClippedText(renderer, m_cursor, m_caretText,
                      readableTextColorForBackground(m_cursor->style().fill));
    m_caretText.clip->setZIndex(2);
  } else if (m_caretText.clip != nullptr) {
    m_caretText.clip->setVisible(false);
  }
}

bool Input::insertionAllowed(std::string_view text, bool preedit) const {
  if (!m_editValidator) return true;
  const auto& before = m_preeditBase ? m_preeditBase->value : m_value;
  const auto cursor = m_preeditBase ? m_preeditBase->cursorPos : m_cursorPos;
  const auto anchor = m_preeditBase ? m_preeditBase->selectionAnchor : m_selectionAnchor;
  std::string after = before;
  const auto start = std::min(cursor, anchor), end = std::max(cursor, anchor);
  after.replace(start, end - start, text);
  return m_editValidator(before, after, preedit);
}

bool Input::rewriteEdit(const EditSnapshot& before, EditSnapshot& after, bool preedit) const {
  if (m_editRewriter) m_editRewriter(before, after, preedit);
  if (m_editValidator && !m_editValidator(before.value, after.value, preedit)) return false;
  after.cursorPos = clampToUtf8End(after.value, after.cursorPos);
  after.selectionAnchor = clampToUtf8End(after.value, after.selectionAnchor);
  return true;
}

void Input::trackPreeditRange(const EditSnapshot& before, std::size_t replacedStart, std::size_t replacedEnd) {
  // Rewriting may extend the edit, but equal text must not shrink or relocate its native range.
  std::size_t start = 0;
  while (start < replacedStart && start < m_value.size() &&
         before.value[start] == m_value[start]) ++start;
  start = clampToUtf8Start(m_value, start);
  std::size_t oldEnd = before.value.size(), end = m_value.size();
  while (oldEnd > replacedEnd && end > start && before.value[oldEnd - 1] == m_value[end - 1]) {
    --oldEnd;
    --end;
  }
  m_preeditStart = start;
  m_preeditLen = clampToUtf8End(m_value, end) - start;
}

bool Input::replaceTextRange(std::size_t start, std::size_t end, std::string_view text,
                             EditCoalesceKind kind, bool preedit) {
  if (m_editRewriter || m_preeditBase) {
    auto before = currentEditSnapshot();
    auto after = before;
    after.value.replace(start, end - start, text);
    after.cursorPos = after.selectionAnchor = start + text.size();
    if (!rewriteEdit(before, after, preedit)) return false;
    removePreeditText();
    if (!preedit) pushUndoSnapshot(kind);
    m_value = std::move(after.value);
    m_cursorPos = after.cursorPos;
    m_selectionAnchor = after.selectionAnchor;
    if (preedit) {
      m_preeditBase = std::move(before);
      trackPreeditRange(*m_preeditBase, start, end);
    }
    return true;
  }
  if (m_editValidator) {
    auto after = m_value;
    after.replace(start, end - start, text);
    if (!m_editValidator(m_value, after, preedit)) return false;
  }
  if (preedit) m_preeditBase = currentEditSnapshot();
  else pushUndoSnapshot(kind);
  m_value.replace(start, end - start, text);
  m_cursorPos = m_selectionAnchor = start + text.size();
  if (preedit) {
    m_preeditStart = start;
    m_preeditLen = text.size();
  }
  return true;
}

bool Input::insertText(std::string_view text) {
  if (text.empty() && !m_editRewriter && !m_preeditBase && !insertionAllowed(text)) return false;
  const auto cursor = m_preeditBase ? m_preeditBase->cursorPos : m_cursorPos;
  const auto anchor = m_preeditBase ? m_preeditBase->selectionAnchor : m_selectionAnchor;
  if (!replaceTextRange(std::min(cursor, anchor), std::max(cursor, anchor), text,
                        EditCoalesceKind::Discrete)) return false;
  updateDisplayText();
  markTextContentChanged();
  revealCursor();
  notifyTextInputStateChanged(TextInputChangeCause::Other);
  if (m_onChange) m_onChange(m_value);
  return true;
}

bool Input::replaceRange(std::size_t rawBegin, std::size_t rawEnd, std::string_view text) {
  if (composing() || rawBegin > rawEnd || rawEnd > m_value.size() ||
      clampToUtf8Start(m_value, rawBegin) != rawBegin ||
      clampToUtf8Start(m_value, rawEnd) != rawEnd ||
      (!text.empty() && !g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr))) return false;
  if (!replaceTextRange(rawBegin, rawEnd, text, EditCoalesceKind::Discrete)) return false;
  updateDisplayText();
  markTextContentChanged();
  revealCursor();
  notifyTextInputStateChanged(TextInputChangeCause::Other);
  if (m_onChange) m_onChange(m_value);
  return true;
}

std::optional<std::size_t> Input::textByteAtPoint(float localX, float localY) const {
  if (composing() || m_textMetricsDirty || m_passwordMode || m_value.empty() ||
      !std::isfinite(localX) || !std::isfinite(localY)) return std::nullopt;
  const float textStartX = m_horizontalPadding + kTextInnerInset;
  const float viewportHeight = m_multiline ? std::max(0.0F, height() - 2.0F * kMultilinePadV) : height();
  const float viewportY = m_multiline ? kMultilinePadV : 0.0F;
  if (localX < textStartX || localX >= textStartX + textViewportWidth() ||
      localY < viewportY || localY >= viewportY + viewportHeight) return std::nullopt;
  const float x = localX - textStartX + (m_multiline ? 0.0F : m_scrollOffset - m_contentLeadSlack);
  const float y = localY - m_projectedOriginY + (m_multiline ? m_scrollOffsetY : 0.0F);
  const auto& cells = displayProjected() ? m_displayCells : (m_multiline ? m_stopRect : m_caretCells);
  const auto& bytes = displayProjected() ? m_displayStopByte : m_stopByte;
  if (cells.size() != bytes.size()) return std::nullopt;
  for (std::size_t i = 0; i + 1 < cells.size(); ++i) {
    const auto& cell = cells[i];
    if (cell.cellIsVirtual || cell.cellWidth <= 0.0F ||
        x < cell.cellX || x >= cell.cellX + cell.cellWidth ||
        y < cell.lineY || y >= cell.lineY + cell.lineHeight) continue;
    const auto raw = rawByteForDisplay(bytes[i], true);
    if (raw < m_value.size()) return raw;
  }
  return std::nullopt;
}

void Input::setInlineGhost(std::string text) {
  if (m_inlineGhost->text() == text) return;
  m_inlineGhost->setText(std::move(text));
  updateInlineGhost();
  markPaintDirty();
}

void Input::setPlaceholder(std::string_view placeholder) {
  if (m_placeholder == placeholder) {
    return;
  }
  m_placeholder = std::string(placeholder);
  if (m_value.empty()) {
    updateDisplayText();
    markLayoutDirty();
  }
}

void Input::setFontSize(float size) {
  const float next = std::max(1.0F, size);
  if (m_fontSize == next) {
    return;
  }
  resetCaretEffects();
  m_fontSize = next;
  m_logicalStrutDirty = true;
  if (m_label != nullptr) {
    m_label->setFontSize(m_fontSize);
  }
  markTextContentChanged();
}

void Input::setControlHeight(float height) {
  const float next = std::max(1.0F, height);
  if (m_controlHeight == next) {
    return;
  }
  resetCaretEffects();
  m_controlHeight = next;
  markLayoutDirty();
}

void Input::setHorizontalPadding(float padding) {
  const float next = std::max(0.0F, padding);
  if (next != m_horizontalPadding) resetCaretEffects();
  m_horizontalPadding = next;
  markLayoutDirty();
}

void Input::setClearButtonEnabled(bool enabled) {
  if (m_clearButtonEnabled == enabled) {
    return;
  }
  m_clearButtonEnabled = enabled;
  markLayoutDirty();
}

void Input::setPasswordMode(bool enabled) {
  // Mutually exclusive with multiline; multiline wins.
  if (enabled && m_multiline) {
    return;
  }
  if (m_passwordMode == enabled) {
    return;
  }
  m_passwordMode = enabled;
  m_passwordRevealed = false;
  if (m_passwordMode) {
    clearEditHistory();
  } else {
    syncPasswordGlyphNodes(0);
  }
  updateDisplayText();
  notifyTextInputStateChanged(TextInputChangeCause::Other);
  markTextContentChanged();
}

void Input::setPasswordRevealed(bool revealed) {
  if (!m_passwordMode || m_passwordRevealed == revealed) {
    return;
  }
  m_passwordRevealed = revealed;
  if (m_passwordRevealed) {
    syncPasswordGlyphNodes(0);
  }
  updateDisplayText();
  notifyTextInputStateChanged(TextInputChangeCause::Other);
  markTextContentChanged();
}

void Input::setMultiline(bool enabled) {
  if (m_multiline == enabled) {
    return;
  }
  resetCaretEffects();
  m_multiline = enabled;
  if (enabled && m_passwordMode) {
    m_passwordMode = false;
    m_passwordRevealed = false;
    syncPasswordGlyphNodes(0);
  }
  if (m_label != nullptr) {
    m_label->setMaxLines(enabled ? 0 : 1);
    if (!enabled) {
      m_label->setMaxWidth(0.0F);
    }
  }
  if (!enabled) {
    syncSelectionLineRects(0);
    if (m_selectionRect != nullptr) {
      m_selectionRect->setVisible(false);
    }
  }
  m_scrollOffset = 0.0F;
  m_scrollOffsetY = 0.0F;
  m_goalCaretX = -1.0F;
  m_stopsBuiltForWidth = -1.0F;
  updateDisplayText();
  notifyTextInputStateChanged(TextInputChangeCause::Other);
  markTextContentChanged();
}

void Input::setInvalid(bool invalid) {
  if (m_invalid == invalid) {
    return;
  }
  m_invalid = invalid;
  applyVisualState();
  markPaintDirty();
}

void Input::setFrameVisible(bool visible) {
  if (m_frameVisible == visible) {
    return;
  }
  m_frameVisible = visible;
  applyVisualState();
  markPaintDirty();
}

void Input::setEmbeddedOnSolidPrimary(bool embedded) {
  if (m_embeddedOnSolidPrimary == embedded) {
    return;
  }
  m_embeddedOnSolidPrimary = embedded;
  applyVisualState();
  markPaintDirty();
}

void Input::setFontWeight(FontWeight fontWeight) {
  if (m_label != nullptr) {
    if (m_label->font().weight != fontWeight) resetCaretEffects();
    m_label->setFontWeight(fontWeight);
    m_logicalStrutDirty = true;
  }
  markTextContentChanged();
}

void Input::setMinLayoutWidth(float width) {
  const float next = std::max(0.0F, width);
  if (m_minLayoutWidth == next) {
    return;
  }
  m_minLayoutWidth = next;
  markLayoutDirty();
}

void Input::setTextAlign(TextAlign align) {
  if (m_textAlign == align) {
    return;
  }
  m_textAlign = align;
  markLayoutDirty();
}

void Input::setOnChange(std::function<void(const std::string&)> callback) { m_onChange = std::move(callback); }

void Input::setOnSubmit(std::function<void(const std::string&)> callback) { m_onSubmit = std::move(callback); }

void Input::setOnKeyEvent(std::function<bool(std::uint32_t, std::uint32_t)> callback) {
  m_onKeyEvent = std::move(callback);
}

void Input::setOnContextMenu(std::function<void(const InputArea::PointerData*)> callback) {
  m_onContextMenu = std::move(callback);
  m_inputArea->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT) |
      (m_onContextMenu ? InputArea::buttonMask(BTN_RIGHT) : 0));
}

void Input::setOnFocusLoss(std::function<void()> callback) { m_onFocusLoss = std::move(callback); }

void Input::setOnFocusGain(std::function<void()> callback) { m_onFocusGain = std::move(callback); }

void Input::setSubmitOnFocusLoss(bool enabled) { m_submitOnFocusLoss = enabled; }

void Input::setSubmitOnEnter(bool enabled) { m_submitOnEnter = enabled; }

void Input::setSurfaceOpacity(float opacity) {
  const float clamped = std::clamp(opacity, 0.0F, 1.0F);
  if (m_surfaceOpacity == clamped) {
    return;
  }
  m_surfaceOpacity = clamped;
  applyVisualState();
}

void Input::setSurfaceRole(ColorRole role) {
  if (m_surfaceRole == role) {
    return;
  }
  m_surfaceRole = role;
  applyVisualState();
}

void Input::setFrameRadius(float radius) {
  const float clamped = std::max(0.0F, radius);
  if (m_frameRadius == clamped && m_frameRadiusExplicit) {
    return;
  }
  m_frameRadius = clamped;
  m_frameRadiusExplicit = true;
  applyVisualState();
}

void Input::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  if (m_inputArea != nullptr) {
    m_inputArea->setEnabled(enabled);
  }
  if (m_clearButtonArea != nullptr) {
    m_clearButtonArea->setEnabled(enabled);
  }
  setOpacity(enabled ? 1.0F : 0.55F);
  applyVisualState();
}

void Input::setTextClipboard(TextClipboard* clipboard) noexcept { g_clipboard = clipboard; }

TextClipboard* Input::textClipboard() noexcept { return g_clipboard; }

void Input::setValidateKeyMatcher(std::function<bool(std::uint32_t, std::uint32_t)> matcher) noexcept {
  g_validateKeyMatcher = std::move(matcher);
}

void Input::setPasswordMaskStyle(PasswordMaskStyle style) noexcept { g_passwordMaskStyle = style; }

void Input::setReadlineShortcutsEnabled(bool enabled) noexcept { g_readlineShortcuts = enabled; }

void Input::selectAll() {
  resetUndoCoalescing();
  m_selectionAnchor = 0;
  m_cursorPos = m_value.size();
  updateInteractiveGeometry();
  notifyTextInputStateChanged(TextInputChangeCause::Other);
  markPaintDirty();
}

void Input::moveCaretLeft(bool shift) {
  resetUndoCoalescing();
  if (!shift && hasSelection()) {
    m_cursorPos = selectionStart();
    m_selectionAnchor = m_cursorPos;
  } else {
    m_cursorPos = adjacentVisibleByte(m_cursorPos, true);
    if (!shift) {
      m_selectionAnchor = m_cursorPos;
    }
  }
  requestCaretUpdate();
  revealCursor();
  notifyTextInputStateChanged(TextInputChangeCause::Other);
}

void Input::moveCaretRight(bool shift) {
  resetUndoCoalescing();
  if (!shift && hasSelection()) {
    m_cursorPos = selectionEnd();
    m_selectionAnchor = m_cursorPos;
  } else {
    m_cursorPos = adjacentVisibleByte(m_cursorPos, false);
    if (!shift) {
      m_selectionAnchor = m_cursorPos;
    }
  }
  requestCaretUpdate();
  revealCursor();
  notifyTextInputStateChanged(TextInputChangeCause::Other);
}

void Input::clearSelection() {
  resetUndoCoalescing();
  m_selectionAnchor = m_cursorPos;
  updateInteractiveGeometry();
  notifyTextInputStateChanged(TextInputChangeCause::Other);
  markPaintDirty();
}

TextInputState Input::textInputState() const {
  auto committed = currentEditSnapshot();
  std::string surrounding = std::move(committed.value);
  std::size_t cursor = committed.cursorPos;
  std::size_t anchor = committed.selectionAnchor;

  float cursorSceneX = 0.0F;
  float cursorSceneY = 0.0F;
  if (m_textViewport != nullptr) {
    Node::absolutePosition(m_textViewport, cursorSceneX, cursorSceneY);
  }
  cursorSceneX += m_caretInsertionRect.x;
  cursorSceneY += m_caretInsertionRect.y;

  const bool password = m_passwordMode;
  const float cursorWidth = m_caretInsertionRect.width;
  const float cursorHeight = m_caretInsertionRect.height;
  return TextInputState{
      .surroundingText = password ? std::string{} : std::move(surrounding),
      .cursor = byteOffsetToProtocolInt(cursor),
      .anchor = byteOffsetToProtocolInt(anchor),
      .cursorRectX = static_cast<std::int32_t>(std::round(cursorSceneX)),
      .cursorRectY = static_cast<std::int32_t>(std::round(cursorSceneY)),
      .cursorRectWidth = static_cast<std::int32_t>(std::max(1.0F, std::round(cursorWidth))),
      .cursorRectHeight = static_cast<std::int32_t>(std::max(1.0F, std::round(cursorHeight))),
      .purpose = password ? TextInputPurpose::Password : TextInputPurpose::Normal,
      .sendSurroundingText = !password,
      .sensitiveData = password,
      .hiddenText = passwordMasked(),
      .preeditVisible = !password,
  };
}

void Input::textInputApplyEdit(const TextInputEdit& edit) {
  const auto before = currentEditSnapshot();
  auto permanent = before;
  auto start = std::min(permanent.cursorPos, permanent.selectionAnchor);
  auto end = std::max(permanent.cursorPos, permanent.selectionAnchor);
  if (edit.hasDelete) {
    const auto lo = clampToUtf8Start(permanent.value,
        edit.deleteBeforeLength > start ? 0 : start - edit.deleteBeforeLength);
    const auto hi = clampToUtf8End(permanent.value,
        std::min(permanent.value.size(), end + static_cast<std::size_t>(edit.deleteAfterLength)));
    permanent.value.erase(end, hi - end);
    permanent.value.erase(lo, start - lo);
    permanent.cursorPos = lo;
    permanent.selectionAnchor = lo + end - start;
  }
  if (edit.hasCommitText) {
    start = std::min(permanent.cursorPos, permanent.selectionAnchor);
    end = std::max(permanent.cursorPos, permanent.selectionAnchor);
    permanent.value.replace(start, end - start, edit.commitText);
    permanent.cursorPos = permanent.selectionAnchor = start + edit.commitText.size();
  }
  if (permanent.value != before.value && !rewriteEdit(before, permanent, false)) return;
  const bool changed = permanent.value != before.value;
  auto display = permanent;
  bool showPreedit = edit.hasPreedit && !edit.preeditText.empty();
  if (showPreedit) {
    start = std::min(display.cursorPos, display.selectionAnchor);
    end = std::max(display.cursorPos, display.selectionAnchor);
    display.value.replace(start, end - start, edit.preeditText);
    const auto cursor = edit.preeditCursorBegin < 0 ? edit.preeditText.size() :
        std::min(edit.preeditText.size(), static_cast<std::size_t>(edit.preeditCursorBegin));
    const auto anchor = edit.preeditCursorEnd < 0 ? cursor :
        std::min(edit.preeditText.size(), static_cast<std::size_t>(edit.preeditCursorEnd));
    display.cursorPos = start + clampToUtf8End(edit.preeditText, cursor);
    display.selectionAnchor = start + clampToUtf8End(edit.preeditText, anchor);
    if (!rewriteEdit(permanent, display, true)) {
      showPreedit = false;
      display = permanent;
    }
  }
  const bool caretChanged = m_cursorPos != display.cursorPos ||
      m_selectionAnchor != display.selectionAnchor ||
      m_preeditCaretHidden != (showPreedit && edit.preeditCursorBegin < 0);
  if (changed) pushUndoSnapshot(EditCoalesceKind::Discrete);
  m_preeditBase = showPreedit ? std::optional<EditSnapshot>(permanent) : std::nullopt;
  m_preeditCaretHidden = showPreedit && edit.preeditCursorBegin < 0;
  m_preeditStart = showPreedit ? start : 0;
  m_preeditLen = showPreedit ? edit.preeditText.size() : 0;
  const bool displayChanged = m_value != display.value;
  m_value = std::move(display.value);
  m_cursorPos = display.cursorPos;
  m_selectionAnchor = display.selectionAnchor;
  if (showPreedit && m_editRewriter) trackPreeditRange(permanent, start, end);
  if (displayChanged) {
    updateDisplayText();
    markTextContentChanged();
  } else if (caretChanged) {
    m_revealCursorYPending = true;
    if (m_multiline) ensureCursorVisibleY();
    markLayoutDirty();
  }
  updateInteractiveGeometry();
  revealCursor();
  markPaintDirty();
  const std::weak_ptr<int> alive = m_aliveToken;
  if (m_onEditingStateChanged) m_onEditingStateChanged();
  if (alive.expired()) return;
  const auto onChange = m_onChange;
  const auto onSubmit = m_onSubmit;
  if (changed && onChange) onChange(permanent.value);
  if (edit.submit && onSubmit) onSubmit(permanent.value);
}

void Input::textInputResetPreedit() {
  if (!removePreeditText()) {
    return;
  }
  updateDisplayText();
  markTextContentChanged();
  updateInteractiveGeometry();
  revealCursor();
  markPaintDirty();
  notifyTextInputStateChanged(TextInputChangeCause::Other);
}

void Input::textInputActivated(TextInputService& service) { m_textInputService = &service; }

void Input::textInputDeactivated(TextInputService& service) {
  if (m_textInputService == &service) {
    m_textInputService = nullptr;
  }
}

void Input::markTextContentChanged() {
  m_textMetricsDirty = true;
  m_revealCursorYPending = true;
  markLayoutDirty();
}

void Input::rebuildCursorStopsFull(Renderer& renderer) {
  const bool showPasswordGlyphs = passwordMasked() && !m_value.empty();
  // A font/DPR change must remeasure the painted slice's baseline even if its text is unchanged.
  if (!m_multiline && !displayProjected()) m_labelVisibleSlice.clear();
  m_stopByte.clear();
  m_stopX.clear();
  m_stopRect.clear();
  m_charSpans.clear();
  m_caretCells.clear();
  m_stopByte.push_back(0);
  m_stopX.push_back(0.0F);
  m_stopsInLogicalOrder = true;
  m_stopsBuiltForWidth = textViewportWidth();
  m_multilineLabelX = 0.0F;
  m_contentTextHeight = 0.0F;

  const float passwordCellSize = showPasswordGlyphs ? std::round(m_fontSize * kPasswordGlyphScale) : 0.0F;
  std::size_t pos = 0;
  float maskX = 0.0F;
  while (pos < m_value.size()) {
    pos = nextCharPos(m_value, pos);
    m_stopByte.push_back(pos);
    if (showPasswordGlyphs) {
      maskX += passwordCellSize;
      m_stopX.push_back(maskX);
    }
  }

  if (displayProjected()) {
    m_displayStopByte.clear();
    m_displayStopByte.push_back(0);
    for (std::size_t byte = 0; byte < m_displayProjection.text.size();) {
      byte = nextCharPos(m_displayProjection.text, byte);
      m_displayStopByte.push_back(byte);
    }
    const float wrapWidth = m_multiline ? m_stopsBuiltForWidth : 0.0F;
    renderer.measureTextCaretStops(m_displayProjection.markup, m_fontSize, m_displayStopByte,
                                   wrapWidth, m_displayCells, m_label->font(), true);
    if (m_displayCells.size() != m_displayStopByte.size())
      m_displayCells.assign(m_displayStopByte.size(),
                           TextCursorStop{.height = m_logicalStrut, .lineHeight = m_logicalStrut});
    if (m_displayProjection.text.empty()) m_displayCells.front().height = m_logicalStrut;
    auto& stops = m_multiline ? m_stopRect : m_caretCells;
    stops.reserve(m_stopByte.size());
    m_stopX.resize(m_stopByte.size());
    std::size_t displayIndex = 0;
    for (std::size_t i = 0; i < m_stopByte.size(); ++i) {
      while (displayIndex + 1 < m_displayStopByte.size() &&
             m_displayStopByte[displayIndex] < displayByte(m_stopByte[i])) ++displayIndex;
      stops.push_back(m_displayCells[displayIndex]);
      m_stopX[i] = stops.back().x;
    }
    const auto metrics = renderer.measureText(m_displayProjection.markup, m_fontSize, m_label->font(),
        wrapWidth, 0, TextAlign::Start, TextEllipsize::End, true);
    m_projectedLayoutBaseline = metrics.layoutBaseline;
    m_contentTextHeight = metrics.layoutHeight;
    m_multilineLabelX = metrics.alignOffsetX;
    if (!m_multiline) m_stopsInLogicalOrder = std::ranges::is_sorted(m_stopX);
    return;
  }

  if (m_value.empty()) {
    if (m_caretShape != CaretShape::Line && !showPasswordGlyphs) {
      renderer.measureTextCaretStops(m_value, m_fontSize, m_stopByte, 0.0F, m_caretCells, m_label->font());
      for (auto& stop : m_caretCells) stop.height = m_logicalStrut;
    }
    if (m_multiline)
      m_stopRect.push_back(TextCursorStop{.height = m_logicalStrut, .lineHeight = m_logicalStrut});
    m_contentTextHeight = m_logicalStrut;
    return;
  }

  if (m_multiline) {
    renderer.measureTextCaretStops(
        m_value, m_fontSize, m_stopByte, m_stopsBuiltForWidth, m_stopRect, m_label->font()
    );
    if (m_stopRect.size() != m_stopByte.size()) {
      m_stopRect.assign(m_stopByte.size(),
                        TextCursorStop{.height = m_logicalStrut, .lineHeight = m_logicalStrut});
    }
    m_stopX.resize(m_stopRect.size());
    for (std::size_t i = 0; i < m_stopRect.size(); ++i) m_stopX[i] = m_stopRect[i].x;
    const auto metrics = renderer.measureText(m_value, m_fontSize, m_label->font(), m_stopsBuiltForWidth);
    m_projectedLayoutBaseline = metrics.layoutBaseline;
    m_multilineLabelX = metrics.alignOffsetX;
    m_contentTextHeight = metrics.layoutHeight;
  } else if (!showPasswordGlyphs) {
    renderer.measureTextCaretStops(m_value, m_fontSize, m_stopByte, 0.0F, m_caretCells, m_label->font());
    m_stopX.clear();
    m_stopX.reserve(m_caretCells.size());
    for (const auto& stop : m_caretCells) m_stopX.push_back(stop.x);
    if (m_stopX.size() != m_stopByte.size()) m_stopX.assign(m_stopByte.size(), 0.0F);
    m_stopsInLogicalOrder = std::ranges::is_sorted(m_stopX);
    if (!m_stopsInLogicalOrder)
      renderer.measureTextCharSpans(m_value, m_fontSize, m_stopByte, m_charSpans, m_label->font());
  }
}

void Input::rebuildCursorStops(Renderer& renderer) {
  m_renderer = &renderer;
  const auto generation = renderer.textMetricsGeneration();
  const float scale = renderer.renderScale();
  if (m_metricsGeneration != generation || m_metricsRenderScale != scale) {
    m_metricsGeneration = generation;
    m_metricsRenderScale = scale;
    m_textMetricsDirty = m_logicalStrutDirty = true;
  }
  if (m_logicalStrutDirty) {
    m_logicalStrut = std::max(1.0F, renderer.measureText(" ", m_fontSize, m_label->font()).layoutHeight);
    m_logicalStrutDirty = false;
  }
  if (!m_textMetricsDirty) return;
  rebuildCursorStopsFull(renderer);
  m_textMetricsDirty = false;
}

void Input::recomputeContentLeadSlack(Renderer& renderer, float width, bool showClearButton) {
  m_contentLeadSlack = 0.0F;
  if (m_multiline || (m_textAlign != TextAlign::Center && !Style::rtl() && !valueLaidOutRtl())) {
    return;
  }

  const float textInset = m_horizontalPadding + kTextInnerInset;
  const float rightInset = showClearButton ? clearButtonTextReserveWidth() : textInset;
  const float viewportWidth = std::max(0.0F, width - textInset - rightInset);
  float textExtent = 0.0F;
  const bool showPasswordGlyphs = passwordMasked() && !m_value.empty();
  if (showPasswordGlyphs) {
    const std::size_t charCount = !m_stopByte.empty() ? m_stopByte.size() - 1 : 0;
    const float passwordCellSize = std::round(m_fontSize * kPasswordGlyphScale);
    textExtent = static_cast<float>(charCount) * passwordCellSize;
  } else if (!m_value.empty() && !m_stopX.empty()) {
    textExtent = *std::ranges::max_element(m_stopX);
  } else if (m_value.empty() && !m_placeholder.empty()) {
    textExtent = renderer.measureText(m_placeholder, m_fontSize, m_label->font()).width;
  }
  if (viewportWidth > 0.0F && textExtent > 0.0F && textExtent + 0.5F < viewportWidth) {
    m_contentLeadSlack = m_textAlign == TextAlign::Center ? std::round((viewportWidth - textExtent) * 0.5F)
                                                          : std::max(0.0F, viewportWidth - textExtent - kCursorWidth);
  }
}

std::size_t Input::visibleLabelStartByte() const {
  if (m_stopX.empty()) {
    return 0;
  }
  const float viewStart = m_scrollOffset;
  std::size_t startByte = 0;
  for (std::size_t i = 0; i < m_stopByte.size(); ++i) {
    if (m_stopX[i] <= viewStart + 0.5F) {
      startByte = m_stopByte[i];
    } else {
      break;
    }
  }
  return startByte;
}

std::size_t Input::visibleLabelEndByte(float contentWidth, std::size_t startByte) const {
  if (m_stopX.empty()) {
    return m_value.size();
  }
  const float originX = stopXForByte(startByte);
  const float endX = originX + contentWidth;
  for (std::size_t i = 1; i < m_stopByte.size(); ++i) {
    if (m_stopX[i] >= endX - 0.5F) {
      return m_stopByte[i];
    }
  }
  return m_value.size();
}

void Input::updateLabelVisibleSlice(Renderer& renderer) {
  if (m_label == nullptr || m_value.empty() || passwordMasked() || displayProjected()) {
    return;
  }

  const bool sliced = labelSlicingEnabled();
  std::size_t sliceStart = 0;
  std::size_t sliceEnd = m_value.size();
  if (sliced) {
    const float viewportW = textViewportWidth();
    const float pad = std::max(viewportW, m_fontSize * 4.0F);
    const float sliceContentW = std::min(kMaxLabelRasterWidth, viewportW + pad * 2.0F);
    sliceStart = visibleLabelStartByte();
    if (sliceStart > 0) {
      sliceStart = prevCharPos(m_value, sliceStart + 1);
    }
    sliceEnd = visibleLabelEndByte(sliceContentW, sliceStart);
  }
  if (sliceEnd <= sliceStart) {
    return;
  }

  const std::string slice = m_value.substr(sliceStart, sliceEnd - sliceStart);
  m_labelSliceOriginX = sliced ? stopXForByte(sliceStart) : 0.0F;
  m_labelVisibleStartByte = sliceStart;
  if (slice != m_labelVisibleSlice) {
    m_labelVisibleSlice = slice;
    m_label->setText(m_labelVisibleSlice);
    m_label->measure(renderer);
    m_projectedLayoutBaseline = renderer.measureText(m_labelVisibleSlice, m_fontSize, m_label->font()).layoutBaseline;
    const float h = m_controlHeight;
    m_cachedLabelY = std::round((h - m_label->height()) * 0.5F);
  }
}

void Input::syncLabelScrollPosition() {
  if (m_label == nullptr || (passwordMasked() && !m_value.empty())) {
    return;
  }
  if (displayProjected() && !m_value.empty()) {
    m_label->setPosition(m_multilineLabelX - m_label->textOriginX() -
                            (m_multiline ? 0.0F : m_scrollOffset - m_contentLeadSlack),
                        m_cachedLabelY - (m_multiline ? m_scrollOffsetY : 0.0F));
    return;
  }
  if (m_multiline) {
    m_label->setPosition(m_multilineLabelX, m_cachedLabelY - m_scrollOffsetY);
    return;
  }
  if (m_value.empty()) {
    m_label->setPosition(-m_scrollOffset + m_contentLeadSlack, m_cachedLabelY);
    return;
  }
  m_label->setPosition(m_labelSliceOriginX - m_scrollOffset + m_contentLeadSlack, m_cachedLabelY);
}

void Input::doLayout(Renderer& renderer) {
  m_cursorRenderScale = std::max(0.1F, renderer.renderScale());
  const float minFromHint = m_minLayoutWidth > 0.0F ? m_minLayoutWidth : 0.0F;
  const float wBase = width() > 0.0F ? width() : (minFromHint > 0.0F ? minFromHint : kMinWidth);
  const float w = std::max(wBase, minFromHint);
  // Multiline keeps whatever height layout assigned (explicit or flex-grown)
  // instead of forcing the single-line control height.
  const float h = m_multiline && height() > 0.0F ? height() : m_controlHeight;
  setSize(w, h);
  const bool showClearButton = clearButtonVisible();

  const bool showPasswordGlyphs = passwordMasked() && !m_value.empty();
  m_label->setVisible(!showPasswordGlyphs);

  // Wrapped stops depend on the viewport width, not just the text.
  if (m_multiline && m_stopsBuiltForWidth != textViewportWidth()) {
    m_textMetricsDirty = true;
    m_revealCursorYPending = true;
  }
  rebuildCursorStops(renderer);
  recomputeContentLeadSlack(renderer, w, showClearButton);

  const bool focused = m_inputArea != nullptr && m_inputArea->focused();
  if (m_multiline) {
    // Only follow the caret when it moved or the text reflowed; any other
    // relayout (e.g. a plugin re-render) keeps the user's wheel position.
    if (focused && m_revealCursorYPending) {
      ensureCursorVisibleY();
    } else {
      clampScrollOffsetY();
    }
    m_revealCursorYPending = false;
  } else if (focused) {
    ensureCursorVisible();
  } else {
    m_scrollOffset = restingScrollOffset();
  }

  if (m_multiline) {
    m_label->setMaxWidth(textViewportWidth());
    m_label->measure(renderer);
    m_cachedLabelY = kMultilinePadV;
  } else if (!showPasswordGlyphs) {
    if (m_value.empty() && !m_placeholder.empty()) {
      m_label->measure(renderer);
      m_cachedLabelY = std::round((h - m_label->height()) * 0.5F);
    } else if (displayProjected()) {
      m_label->measure(renderer);
      m_cachedLabelY = std::round((h - m_label->height()) * 0.5F);
    } else if (!m_value.empty()) {
      updateLabelVisibleSlice(renderer);
    }
  }
  if (!showPasswordGlyphs) {
    m_projectedOriginY = m_value.empty()
        ? (m_multiline ? kMultilinePadV : std::round((h - m_logicalStrut) * 0.5F))
        : m_cachedLabelY + m_label->baselineOffset() -
            std::floor(m_projectedLayoutBaseline * m_cursorRenderScale) / m_cursorRenderScale;
  }

  std::size_t charCount = 0;
  if (showPasswordGlyphs) {
    charCount = !m_stopByte.empty() ? m_stopByte.size() - 1 : 0;
  }
  float passwordGlyphSize = 0.0F;
  const float passwordCellSize = showPasswordGlyphs ? std::round(m_fontSize * kPasswordGlyphScale) : 0.0F;
  if (showPasswordGlyphs) {
    passwordGlyphSize = m_fontSize * kPasswordGlyphScale;
  }

  if (showPasswordGlyphs) {
    syncPasswordGlyphNodes(charCount);
    float gx = m_contentLeadSlack - m_scrollOffset;
    for (std::size_t i = 0; i < m_passwordGlyphs.size(); ++i) {
      auto* glyph = m_passwordGlyphs[i];
      layoutPasswordMaskGlyph(
          renderer, *glyph, passwordMaskCodepointForIndex(i), passwordGlyphSize, gx, passwordCellSize, h
      );
      glyph->setVisible(true);
      gx += passwordCellSize;
    }
  } else {
    syncPasswordGlyphNodes(0);
    syncLabelScrollPosition();
  }

  m_background->setPosition(0.0F, 0.0F);
  m_background->setFrameSize(w, h);

  if (m_textViewport != nullptr) {
    const float textInset = m_horizontalPadding + kTextInnerInset;
    const float rightInset = showClearButton ? clearButtonTextReserveWidth() : textInset;
    const float viewportW = std::max(0.0F, w - textInset - rightInset);
    m_textViewport->setPosition(textInset, 0.0F);
    m_textViewport->setSize(viewportW, h);
  }

  m_inputArea->setPosition(0.0F, 0.0F);
  m_inputArea->setFrameSize(w, h);

  if (m_clearButtonArea != nullptr && m_clearButtonGlyph != nullptr) {
    const float buttonSize = clearButtonHitWidth();
    m_clearButtonArea->setVisible(showClearButton);
    m_clearButtonArea->setPosition(std::max(0.0F, w - buttonSize), 0.0F);
    m_clearButtonArea->setFrameSize(buttonSize, h);

    const float clearGlyphSize = std::round(m_fontSize * kClearGlyphScale);
    const auto metrics = renderer.measureGlyph(m_clearButtonGlyph->codepoint(), clearGlyphSize);
    const float glyphCenterX = (metrics.left + metrics.right) * 0.5F;
    const float glyphInkCenter = (metrics.top + metrics.bottom) * 0.5F;
    m_clearButtonGlyph->setFontSize(clearGlyphSize);
    m_clearButtonGlyph->setPosition(buttonSize * 0.5F - glyphCenterX, h * 0.5F - glyphInkCenter);
  }

  updateInteractiveGeometry();
  applyVisualState();
  updateCursorVisibility();
  updateTextOverlays(renderer);
}

bool Input::usesTextGeometry(std::uint32_t sym) noexcept {
  return KeySymbol::isLeft(sym) || KeySymbol::isRight(sym) ||
         KeySymbol::isHome(sym) || KeySymbol::isEnd(sym) ||
         KeySymbol::isUp(sym) || KeySymbol::isDown(sym) ||
         KeySymbol::isPageUp(sym) || KeySymbol::isPageDown(sym);
}

void Input::handleKey(std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers, bool preedit) {
  // onSubmit/onKeyEvent below can synchronously destroy this Input (e.g. closing a
  // dialog that owns it). Bail before touching members if that happens.
  const std::weak_ptr<int> alive = m_aliveToken;
  if (sym == XKB_KEY_Menu || (sym == XKB_KEY_F10 && modifiers == KeyMod::Shift)) {
    if (m_onContextMenu) {
      auto callback = m_onContextMenu;
      callback(nullptr);
    }
    return;
  }
  clampEditState();

  const bool validateMatch = g_validateKeyMatcher && g_validateKeyMatcher(sym, modifiers);
  const bool shift = (modifiers & KeyMod::Shift) != 0;
  const bool ctrl = (modifiers & KeyMod::Ctrl) != 0;
  const bool plainPrintableText = isPlainPrintableKey(utf32, modifiers, preedit);

  // A printable key that doubles as a keybind chord (Space is bound to Validate)
  // is text while the field is focused, not an activation to hand to the panel.
  if (m_onKeyEvent && !(plainPrintableText && validateMatch)) {
    if (m_onKeyEvent(sym, modifiers)) {
      return;
    }
    if (alive.expired()) {
      return;
    }
  }
  const bool undoShortcut = ctrl && !shift && (sym == 'z' || sym == 'Z');
  const bool redoShortcut = (ctrl && (sym == 'y' || sym == 'Y')) || (ctrl && shift && (sym == 'z' || sym == 'Z'));
  const bool clearShortcut = ctrl && !shift && (sym == 'u' || sym == 'U');

  const bool alt = (modifiers & KeyMod::Alt) != 0;
  const bool lineEditCaretToEnd = g_readlineShortcuts && ctrl && !shift && (sym == 'e' || sym == 'E');
  const bool lineEditCaretBack = g_readlineShortcuts && ctrl && !shift && (sym == 'b' || sym == 'B');
  const bool lineEditCaretForward = g_readlineShortcuts && ctrl && !shift && (sym == 'f' || sym == 'F');
  const bool lineEditWordBack = g_readlineShortcuts && alt && (sym == 'b' || sym == 'B');
  const bool lineEditWordForward = g_readlineShortcuts && alt && (sym == 'f' || sym == 'F');
  const bool lineEditDeleteWordBack = g_readlineShortcuts && ctrl && (sym == 'w' || sym == 'W');
  const bool lineEditDeleteToEnd = g_readlineShortcuts && ctrl && !shift && (sym == 'k' || sym == 'K');
  const bool lineEditDeleteWordForward = g_readlineShortcuts && alt && (sym == 'd' || sym == 'D');
  const bool lineEditShortcut = lineEditCaretToEnd
      || lineEditCaretBack
      || lineEditCaretForward
      || lineEditWordBack
      || lineEditWordForward
      || lineEditDeleteWordBack
      || lineEditDeleteToEnd
      || lineEditDeleteWordForward;

  const bool verticalNav = m_multiline
      && (KeySymbol::isUp(sym) || KeySymbol::isDown(sym) || KeySymbol::isPageUp(sym) || KeySymbol::isPageDown(sym));

  // Any non-vertical key breaks an Up/Down run's sticky column.
  if (!verticalNav) {
    m_goalCaretX = -1.0F;
  }

  // Ignore non-text keys that aren't handled below. Ctrl chords may still have utf32 == 0.
  if (utf32 == 0 && !preedit && !ctrl) {
    const bool navigationOrEdit = KeySymbol::isBackspace(sym)
        || KeySymbol::isDelete(sym)
        || KeySymbol::isLeft(sym)
        || KeySymbol::isRight(sym)
        || KeySymbol::isHome(sym)
        || KeySymbol::isEnd(sym)
        || KeySymbol::isInsert(sym)
        || verticalNav
        || undoShortcut
        || redoShortcut
        || clearShortcut
        || lineEditShortcut;
    if (!navigationOrEdit && !validateMatch) {
      return;
    }
  }
  // Validate insertion before removing an existing composition or recording undo.
  if (!m_editRewriter && m_preeditBase) {
    if ((plainPrintableText || preedit) && utf32 >= 0x20U && utf32 != 0x7FU &&
        !insertionAllowed(StringUtils::utf32ToUtf8(utf32), preedit)) return;
    if (m_multiline && KeySymbol::isEnter(sym) && !ctrl &&
        (!m_submitOnEnter || shift) && !insertionAllowed("\n")) return;
  }

  bool changed = false;
  bool displayTextCurrent = false;
  auto previousPreedit = m_editValidator && m_preeditBase
      ? std::optional<EditingState>(editingState()) : std::nullopt;
  bool rejected = false;
  auto replace = [&](std::size_t start, std::size_t end, std::string_view text,
                     EditCoalesceKind kind, bool isPreedit = false) {
    const bool accepted = replaceTextRange(start, end, text, kind, isPreedit);
    rejected = !accepted;
    if (accepted) displayTextCurrent = false;
    return accepted;
  };
  auto eraseVisible = [&](bool backward, bool word) {
    std::size_t start = m_cursorPos, end = m_cursorPos;
    if (!displayProjected()) {
      if (backward) start = word ? previousWordStartForByteOffset(start) : prevCharPos(m_value, start);
      else end = word ? nextWordEndForByteOffset(end) : nextCharPos(m_value, end);
    } else if (m_displayProjection.text.empty()) {
      if (backward) start = 0;
      else end = m_value.size();
    } else if (backward) {
      end = rawByteForDisplay(displayByte(m_cursorPos), false);
      start = word ? previousWordStartForByteOffset(m_cursorPos) : adjacentVisibleByte(end, true);
    } else {
      start = rawByteForDisplay(displayByte(m_cursorPos), true);
      end = word ? nextWordEndForByteOffset(m_cursorPos) : nextCharPos(m_value, start);
    }
    if (start >= end) return false;
    return replace(start, end, {}, EditCoalesceKind::Discrete);
  };

  // Remove previous preedit text before processing
  changed = removePreeditText();
  if (changed) {
    updateDisplayText();
    markTextContentChanged();
    if (usesTextGeometry(sym) && m_renderer != nullptr) doLayout(*m_renderer);
    displayTextCurrent = true;
    if (usesTextGeometry(sym) && m_renderer == nullptr) {
      // No native layout has been bound: publish the restored base, not a guessed visual move.
      revealCursor();
      notifyTextInputStateChanged(TextInputChangeCause::Other);
      if (alive.expired()) return;
      if (!preedit && m_onChange) m_onChange(m_value);
      return;
    }
  }

  const bool copyShortcut = ctrl && (KeySymbol::isInsert(sym) || sym == 'c' || sym == 'C');
  const bool cutShortcut =
      (ctrl && (sym == 'x' || sym == 'X')) || (!ctrl && shift && KeySymbol::isDelete(sym) && hasSelection());
  const bool pasteShortcut = (ctrl && (sym == 'v' || sym == 'V')) || (!ctrl && shift && KeySymbol::isInsert(sym));

  if (undoShortcut) {
    if (undoEdit()) {
      return;
    }
    if (changed) {
      if (!displayTextCurrent) {
        updateDisplayText();
        markTextContentChanged();
      }
      revealCursor();
      notifyTextInputStateChanged(TextInputChangeCause::Other);
      if (!preedit && m_onChange) {
        m_onChange(m_value);
      }
    }
    return;
  }
  if (redoShortcut) {
    if (redoEdit()) {
      return;
    }
    if (changed) {
      if (!displayTextCurrent) {
        updateDisplayText();
        markTextContentChanged();
      }
      revealCursor();
      notifyTextInputStateChanged(TextInputChangeCause::Other);
      if (!preedit && m_onChange) {
        m_onChange(m_value);
      }
    }
    return;
  }
  if (clearShortcut) {
    resetUndoCoalescing();
    if (g_readlineShortcuts) {
      if (m_cursorPos > 0) {
        changed = replace(0, m_cursorPos, {}, EditCoalesceKind::Discrete) || changed;
      }
    } else {
      if (!m_value.empty() || hasSelection()) {
        changed = replace(0, m_value.size(), {}, EditCoalesceKind::Discrete) || changed;
      }
    }
  } else if (ctrl && (sym == 'a' || sym == 'A')) {
    resetUndoCoalescing();
    if (g_readlineShortcuts) {
      m_cursorPos = 0;
      m_selectionAnchor = 0;
    } else {
      m_selectionAnchor = 0;
      m_cursorPos = m_value.size();
    }
  } else if (lineEditCaretToEnd) {
    resetUndoCoalescing();
    m_cursorPos = m_value.size();
    m_selectionAnchor = m_cursorPos;
  } else if (lineEditCaretBack) {
    resetUndoCoalescing();
    if (hasSelection()) {
      m_cursorPos = selectionStart();
    } else {
      m_cursorPos = adjacentVisibleByte(m_cursorPos, true);
    }
    m_selectionAnchor = m_cursorPos;
  } else if (lineEditCaretForward) {
    resetUndoCoalescing();
    if (hasSelection()) {
      m_cursorPos = selectionEnd();
    } else {
      m_cursorPos = adjacentVisibleByte(m_cursorPos, false);
    }
    m_selectionAnchor = m_cursorPos;
  } else if (lineEditWordBack) {
    resetUndoCoalescing();
    m_cursorPos = previousWordStartForByteOffset(m_cursorPos);
    m_selectionAnchor = m_cursorPos;
  } else if (lineEditWordForward) {
    resetUndoCoalescing();
    m_cursorPos = nextWordEndForByteOffset(m_cursorPos);
    m_selectionAnchor = m_cursorPos;
  } else if (lineEditDeleteWordBack) {
    if (hasSelection()) {
      changed = replace(selectionStart(), selectionEnd(), {}, EditCoalesceKind::Discrete) || changed;
    } else {
      changed = eraseVisible(true, true) || changed;
    }
  } else if (lineEditDeleteToEnd) {
    if (m_cursorPos < m_value.size()) {
      changed = replace(m_cursorPos, m_value.size(), {}, EditCoalesceKind::Discrete) || changed;
    }
  } else if (lineEditDeleteWordForward) {
    changed = eraseVisible(false, true) || changed;
  } else if (copyShortcut) {
    if (g_clipboard != nullptr && hasSelection() && !m_passwordMode) {
      g_clipboard->setClipboardText(m_value.substr(selectionStart(), selectionEnd() - selectionStart()));
    }
  } else if (cutShortcut) {
    if (g_clipboard != nullptr && hasSelection()) {
      if (!m_passwordMode) {
        g_clipboard->setClipboardText(m_value.substr(selectionStart(), selectionEnd() - selectionStart()));
      }
      changed = replace(selectionStart(), selectionEnd(), {}, EditCoalesceKind::Discrete) || changed;
    }
  } else if (pasteShortcut) {
    if (g_clipboard != nullptr) {
      const auto expected = currentEditSnapshot();
      g_clipboard->requestClipboardText([this, alive, expected](std::optional<std::string> text) {
        if (alive.expired() || !text || currentEditSnapshot() != expected) return;
        insertText(*text);
      });
    }
  } else if (KeySymbol::isBackspace(sym)) {
    if (hasSelection()) {
      changed = replace(selectionStart(), selectionEnd(), {}, EditCoalesceKind::Discrete) || changed;
    } else if (m_cursorPos > 0) {
      changed = eraseVisible(true, ctrl) || changed;
    }
  } else if (KeySymbol::isDelete(sym)) {
    if (hasSelection()) {
      changed = replace(selectionStart(), selectionEnd(), {}, EditCoalesceKind::Discrete) || changed;
    } else if (m_cursorPos < m_value.size()) {
      changed = eraseVisible(false, ctrl) || changed;
    }
  } else if (KeySymbol::isLeft(sym) || KeySymbol::isRight(sym)) {
    resetUndoCoalescing();
    const bool left = KeySymbol::isLeft(sym);
    // Right-to-left runs put byte order and screen order apart; arrows follow the screen.
    const bool visual = !m_stopsInLogicalOrder;
    if (!shift && hasSelection()) {
      // Collapse to the selection edge on the arrow's side
      const bool startOnLeft = !visual || stopXForByte(selectionStart()) <= stopXForByte(selectionEnd());
      m_cursorPos = left == startOnLeft ? selectionStart() : selectionEnd();
      m_selectionAnchor = m_cursorPos;
    } else {
      if (visual) {
        m_cursorPos = visuallyAdjacentByte(m_cursorPos, left, ctrl);
      } else if (ctrl) {
        m_cursorPos = left ? previousWordStartForByteOffset(m_cursorPos) : nextWordStartForByteOffset(m_cursorPos);
      } else {
        m_cursorPos = adjacentVisibleByte(m_cursorPos, left);
      }
      if (!shift) {
        m_selectionAnchor = m_cursorPos;
      }
    }
  } else if (verticalNav) {
    resetUndoCoalescing();
    const bool up = KeySymbol::isUp(sym) || KeySymbol::isPageUp(sym);
    int lineDelta = up ? -1 : 1;
    if (KeySymbol::isPageUp(sym) || KeySymbol::isPageDown(sym)) {
      const int pageLines = std::max(1, static_cast<int>(std::floor(textViewportHeight() / currentLineHeight())));
      lineDelta *= pageLines;
    }
    if (!shift && hasSelection()) {
      m_cursorPos = up ? selectionStart() : selectionEnd();
      m_selectionAnchor = m_cursorPos;
      m_goalCaretX = -1.0F;
    } else {
      m_cursorPos = byteForVerticalMove(m_cursorPos, lineDelta);
      if (!shift) {
        m_selectionAnchor = m_cursorPos;
      }
    }
  } else if (m_multiline && KeySymbol::isEnter(sym) && !preedit) {
    if (ctrl || (m_submitOnEnter && !shift)) {
      if (m_onSubmit) {
        m_onSubmit(m_value);
        if (alive.expired()) {
          return;
        }
      }
    } else {
      changed = replace(selectionStart(), selectionEnd(), "\n",
                        hasSelection() ? EditCoalesceKind::Discrete : EditCoalesceKind::Typing) || changed;
      if (!rejected) noteTypingEditEnd();
    }
  } else if (KeySymbol::isHome(sym)) {
    resetUndoCoalescing();
    m_cursorPos = m_multiline && !ctrl ? lineStartForByte(m_cursorPos) : 0;
    if (!shift) {
      m_selectionAnchor = m_cursorPos;
    }
  } else if (KeySymbol::isEnd(sym)) {
    resetUndoCoalescing();
    m_cursorPos = m_multiline && !ctrl ? lineEndForByte(m_cursorPos) : m_value.size();
    if (!shift) {
      m_selectionAnchor = m_cursorPos;
    }
  } else if (validateMatch && !plainPrintableText) {
    if (m_onSubmit) {
      m_onSubmit(m_value);
      if (alive.expired()) {
        return;
      }
    }
  } else if (utf32 >= 0x20U && utf32 != 0x7FU) {
    // Printable character (skip DEL = 0x7F)
    const auto bytes = StringUtils::utf32ToUtf8(utf32);
    changed = replace(selectionStart(), selectionEnd(), bytes,
                      hasSelection() ? EditCoalesceKind::Discrete : EditCoalesceKind::Typing, preedit) || changed;
    if (!preedit && !rejected) {
      noteTypingEditEnd();
    }
  }
  if (rejected) {
    if (previousPreedit) {
      m_value = std::move(previousPreedit->current.value);
      m_cursorPos = previousPreedit->current.cursorPos;
      m_selectionAnchor = previousPreedit->current.selectionAnchor;
      m_preeditBase = std::move(previousPreedit->preeditBase);
      m_preeditStart = previousPreedit->preeditStart;
      m_preeditLen = previousPreedit->preeditLength;
      m_preeditCaretHidden = previousPreedit->preeditCaretHidden;
      if (displayTextCurrent) {
        updateDisplayText();
        markTextContentChanged();
        if (m_renderer != nullptr) doLayout(*m_renderer);
      }
    }
    return;
  }

  if (changed) {
    if (!displayTextCurrent) {
      updateDisplayText();
      markTextContentChanged();
    }
    revealCursor();
    notifyTextInputStateChanged(TextInputChangeCause::Other);
    if (!preedit && m_onChange) {
      m_onChange(m_value);
    }
  } else {
    requestCaretUpdate();
    revealCursor();
    notifyTextInputStateChanged(TextInputChangeCause::Other);
  }
}

void Input::notifyTextInputStateChanged(TextInputChangeCause cause) {
  clampEditState();
  if (m_selectionForeground || displayProjected()) markLayoutDirty();
  const std::weak_ptr<int> alive = m_aliveToken;
  if (m_onEditingStateChanged) m_onEditingStateChanged();
  if (alive.expired()) return;
  if (m_textInputService == nullptr) {
    return;
  }
  m_textInputService->notifyClientStateChanged(this, cause);
}

bool Input::removePreeditText() {
  m_preeditCaretHidden = false;
  if (m_preeditBase) {
    m_value = std::move(m_preeditBase->value);
    m_cursorPos = m_preeditBase->cursorPos;
    m_selectionAnchor = m_preeditBase->selectionAnchor;
    m_preeditBase.reset();
    m_preeditStart = m_preeditLen = 0;
    return true;
  }
  if (m_preeditLen == 0) {
    m_preeditStart = 0;
    m_preeditLen = 0;
    return false;
  }

  if (m_preeditStart > m_value.size()) {
    m_preeditStart = 0;
    m_preeditLen = 0;
    clampEditState();
    return false;
  }

  m_preeditStart = clampToUtf8End(m_value, m_preeditStart);
  const std::size_t len = std::min(m_preeditLen, m_value.size() - m_preeditStart);
  if (len == 0) {
    m_preeditStart = 0;
    m_preeditLen = 0;
    clampEditState();
    return false;
  }

  m_value.erase(m_preeditStart, len);
  m_cursorPos = m_preeditStart;
  m_selectionAnchor = m_cursorPos;
  m_preeditStart = 0;
  m_preeditLen = 0;
  return true;
}


void Input::applyVisualState() {
  const bool focused = m_inputArea != nullptr && m_inputArea->focused();
  const bool clearButtonHovered = m_clearButtonArea != nullptr && m_clearButtonArea->hovered();
  const bool inputHovered = (m_inputArea != nullptr && m_inputArea->hovered()) || clearButtonHovered;
  const bool readOnly = isReadOnlyVisual();
  const float chromeScale = chromeScaleForControlHeight(m_controlHeight);

  if (m_frameVisible) {
    m_background->setVisible(true);
    const Color fill =
        focused ? resolved(ColorRole::Surface, m_surfaceOpacity) : resolved(m_surfaceRole, m_surfaceOpacity);
    const Color border = m_invalid
        ? resolved(ColorRole::Error)
        : (focused ? resolveColorSpec(focusRingColorSpec())
                   : (inputHovered ? resolved(ColorRole::Hover) : resolved(ColorRole::OutlineVariant)));

    float resolvedBorderWidth = 0.0F;
    if (focused) {
      resolvedBorderWidth = Style::focusRingWidth;
    } else if (Style::inputBordersEnabled()) {
      resolvedBorderWidth = Style::borderWidth;
    }

    m_background->setStyle(
        RoundedRectStyle{
            .fill = fill,
            .border = border,
            .fillMode = FillMode::Solid,
            .radius = m_frameRadiusExplicit ? m_frameRadius : Style::scaledRadius(m_frameRadius, chromeScale),
            .softness = 1.0F,
            .borderWidth = resolvedBorderWidth,
        }
    );
  } else if (m_background != nullptr) {
    m_background->setVisible(false);
  }

  if (m_embeddedOnSolidPrimary && !m_frameVisible) {
    auto selectionStyleEmb = m_selectionRect->style();
    selectionStyleEmb.fill = resolved(ColorRole::Surface, 0.4F);
    selectionStyleEmb.fillMode = FillMode::Solid;
    selectionStyleEmb.radius = 2.0F;
    m_selectionRect->setStyle(selectionStyleEmb);
    for (auto* rect : m_selectionLineRects) {
      rect->setStyle(selectionStyleEmb);
    }

    applyCaretStyle(resolved(ColorRole::Surface));

    const bool showingPlaceholder = m_value.empty() && !m_placeholder.empty();
    if (m_invalid) {
      m_label->setColor(colorSpecFromRole(ColorRole::Error));
    } else if (showingPlaceholder) {
      m_label->setColor(colorSpecFromRole(ColorRole::OnPrimary, kPrimaryPlaceholderAlpha));
    } else if (readOnly) {
      m_label->setColor(colorSpecFromRole(ColorRole::OnPrimary, 0.65F));
    } else {
      m_label->setColor(colorSpecFromRole(ColorRole::OnPrimary));
    }
    const Color passwordGlyphEmb = m_invalid
        ? resolved(ColorRole::Error)
        : (showingPlaceholder ? resolved(ColorRole::OnPrimary, kPrimaryPlaceholderAlpha)
                              : (readOnly ? resolved(ColorRole::OnPrimary, 0.65F) : resolved(ColorRole::OnPrimary)));
    for (auto* glyph : m_passwordGlyphs) {
      glyph->setColor(passwordGlyphEmb);
    }
    if (m_clearButtonGlyph != nullptr) {
      m_clearButtonGlyph->setColor(resolved(ColorRole::OnPrimary, clearButtonHovered ? 1.0F : 0.72F));
    }
    updateInlineGhost();
    return;
  }

  auto selectionStyle = m_selectionRect->style();
  selectionStyle.fill = m_selectionBackground.value_or(resolved(ColorRole::Primary));
  selectionStyle.fillMode = FillMode::Solid;
  selectionStyle.radius = 2.0F;
  m_selectionRect->setStyle(selectionStyle);
  m_selectionRect->setOpacity(m_selectionBackground ? 1.0F : 0.3F);
  for (auto* rect : m_selectionLineRects) {
    rect->setStyle(selectionStyle);
    rect->setOpacity(m_selectionBackground ? 1.0F : 0.3F);
  }

  applyCaretStyle(m_caretColor.value_or(resolved(ColorRole::Primary)));

  const bool showingPlaceholder = m_value.empty() && !m_placeholder.empty();
  const ColorSpec textColor = m_invalid
      ? colorSpecFromRole(ColorRole::Error)
      : (showingPlaceholder
             ? colorSpecFromRole(ColorRole::OnSurfaceVariant, kPlaceholderAlpha)
             : (readOnly ? colorSpecFromRole(ColorRole::OnSurfaceVariant) : colorSpecFromRole(ColorRole::OnSurface)));
  m_label->setColor(textColor);
  if (m_foreground && !showingPlaceholder && !m_invalid) m_label->setColor(*m_foreground);
  const Color passwordGlyphColor = resolveColorSpec(textColor);
  for (auto* glyph : m_passwordGlyphs) {
    glyph->setColor(passwordGlyphColor);
  }
  if (m_clearButtonGlyph != nullptr) {
    m_clearButtonGlyph->setColor(resolved(clearButtonHovered ? ColorRole::OnSurface : ColorRole::OnSurfaceVariant));
  }
  updateInlineGhost();
}

void Input::updateDisplayText() {
  m_labelVisibleSlice.clear();
  m_labelVisibleStartByte = 0;
  m_labelSliceOriginX = 0.0F;
  if (displayProjected()) m_displayProjection = m_displayProjector(m_value);
  m_label->setUseMarkup(displayProjected());
  m_label->setMaxLines(m_multiline || displayProjected() ? 0 : 1);
  if (m_value.empty() && !m_placeholder.empty()) {
    m_label->setUseMarkup(false);
    m_label->setText(m_placeholder);
  } else if (passwordMasked()) {
    m_label->setText(std::string{});
  } else {
    m_label->setText(displayProjected() ? m_displayProjection.markup : m_value);
  }
}

void Input::requestCaretUpdate() {
  if (m_multiline) {
    ensureCursorVisibleY();
  }
  updateInteractiveGeometry();
  updateCursorVisibility();
  markPaintDirty();
}

bool Input::isReadOnlyVisual() const noexcept {
  return m_inputArea != nullptr && (!m_inputArea->focusable() || !m_inputArea->enabled());
}

void Input::clearFromButton() {
  if (m_value.empty()) {
    return;
  }
  const auto size = m_preeditBase ? m_preeditBase->value.size() : m_value.size();
  if (!replaceTextRange(0, size, {}, EditCoalesceKind::Discrete)) return;
  m_preeditStart = 0;
  m_preeditLen = 0;
  m_scrollOffset = 0.0F;
  m_scrollOffsetY = 0.0F;
  m_goalCaretX = -1.0F;
  updateDisplayText();
  if (m_clearButtonArea != nullptr) {
    m_clearButtonArea->setVisible(false);
  }
  updateInteractiveGeometry();
  revealCursor();
  applyVisualState();
  markTextContentChanged();
  markPaintDirty();
  notifyTextInputStateChanged(TextInputChangeCause::Other);
  if (m_onChange) {
    m_onChange(m_value);
  }
}

bool Input::caretPaintEligible() const {
  if (!m_enabled || m_inputArea == nullptr || !m_inputArea->focused() || hasSelection() || m_preeditCaretHidden)
    return false;
  for (const Node* node = this; node != nullptr; node = node->parent()) if (!node->visible()) return false;
  return true;
}

LayoutRect Input::caretViewport() const {
  float x = 0.0F, y = 0.0F;
  Node::absolutePosition(m_textViewport, x, y);
  return {x, y, textViewportWidth(), m_textViewport->height()};
}

void Input::applyCaretStyle(Color color) {
  RoundedRectStyle style{
      .fill = color,
      .fillMode = FillMode::Solid,
      .radius = 0.0F,
      .noAa = true,
  };
  m_cursor->setStyle(style);
  for (auto* stamp : m_caretStamps) if (stamp != nullptr) stamp->setStyle(style);
  if (m_caretText.label != nullptr)
    m_caretText.label->setColor(readableTextColorForBackground(color));
}

void Input::updateCaretPaint(float cellX, float cellWidth) {
  const auto viewport = caretViewport();
  if (!input_caret::equal(viewport, m_caretTrailViewport) || m_caretTrailScale != m_cursorRenderScale ||
      m_caretTrailScrollX != m_scrollOffset || m_caretTrailScrollY != m_scrollOffsetY) {
    resetCaretEffects();
    m_caretTrailViewport = viewport;
    m_caretTrailScale = m_cursorRenderScale;
    m_caretTrailScrollX = m_scrollOffset;
    m_caretTrailScrollY = m_scrollOffsetY;
  }
  auto rect = m_caretInsertionRect;
  if (m_caretShape != CaretShape::Line) {
    // Keep a thin insertion edge when an end slot lies entirely beyond either
    // viewport edge; moving the full cell inward would cover the preceding glyph.
    const bool outside = cellX >= textViewportWidth() || cellX + cellWidth <= 0.0F;
    rect.x = outside ? m_caretInsertionRect.x : cellX;
    if (!outside) rect.width = std::max(rect.width, cellWidth);
    if (m_caretShape == CaretShape::Underline) {
      const float stroke = std::min(rect.height, m_caretInsertionRect.width);
      rect.y += rect.height - stroke;
      rect.height = stroke;
    }
  }
  rect.x += viewport.x;
  rect.y += viewport.y;
  rect = input_caret::clippedPixels(rect, viewport, m_cursorRenderScale);
  rect.x -= viewport.x;
  rect.y -= viewport.y;
  m_cursor->setPosition(rect.x, rect.y);
  m_cursor->setFrameSize(rect.width, rect.height);
  if (m_caretText.clip != nullptr) {
    m_caretText.clip->setPosition(rect.x, rect.y);
    m_caretText.clip->setFrameSize(rect.width, rect.height);
    m_caretText.label->setPosition(
        m_label->x() + m_label->textOriginX() - m_caretText.label->textOriginX() - rect.x,
        m_label->y() + m_label->baselineOffset() - m_caretText.label->baselineOffset() - rect.y);
  }
  auto* animations = animationManager();
  const bool allowed = m_caretEffect != CaretEffect::Off && animations != nullptr &&
                       MotionService::instance().enabled() && caretPaintEligible() && !composing();
  if (!allowed) {
    resetCaretEffects();
    return;
  }
  // A restored/edited value is placed immediately, but trails only use freshly shaped cells.
  if (m_textMetricsDirty) return;
  if (rect.width <= 0.0F || rect.height <= 0.0F) {
    resetCaretEffects();
    return;
  }
  if (!m_caretTrail.move(rect, true)) return;
  if (m_caretAnimation != 0) animations->cancel(m_caretAnimation);
  paintCaretTrail(0.0F);
  m_caretAnimation = animations->animate(
      // Display-calibrated milliseconds, retaining native reduced-motion completion.
      0.0F, 1.0F, m_caretDuration * MotionService::instance().speed(), Easing::Linear,
      [this](float progress) { paintCaretTrail(progress); }, {}, this
  );
}

void Input::paintCaretTrail(float progress) {
  const auto viewport = caretViewport();
  const bool motion = MotionService::instance().enabled();
  if (!motion || !caretPaintEligible() || composing() || m_caretEffect == CaretEffect::Off ||
      !input_caret::equal(viewport, m_caretTrailViewport) ||
      m_caretTrailScale != m_cursorRenderScale || m_caretTrailScrollX != m_scrollOffset ||
      m_caretTrailScrollY != m_scrollOffsetY) m_caretTrail.reset();
  if (!motion) {
    m_cursorBlinkVisible = true;
    m_cursor->setVisible(caretPaintEligible());
    if (m_caretText.clip != nullptr)
      m_caretText.clip->setVisible(m_cursor->visible() && m_caretShape == CaretShape::Box &&
                                  !m_value.empty() && !passwordMasked() && !m_textMetricsDirty);
  }
  if (progress >= 1.0F) {
    m_caretTrail.active = false;
    m_caretAnimation = 0;
  }
  for (std::size_t i = 0; i < m_caretStamps.size(); ++i) {
    auto* stamp = m_caretStamps[i];
    if (stamp == nullptr) continue;
    auto rect = m_caretTrail.sample(progress, i);
    float rotation = 0.0F;
    if (m_caretEffect == CaretEffect::Trail) {
      rect.x += viewport.x;
      rect.y += viewport.y;
      rect = input_caret::clippedPixels(rect, viewport, m_cursorRenderScale);
      rect.x -= viewport.x;
      rect.y -= viewport.y;
    } else {
      // Six retained connected segments, clipped by the native text viewport.
      const float p = std::clamp(progress, 0.0F, 1.0F);
      const float a = static_cast<float>(i) / input_caret::Trail::count;
      const float b = static_cast<float>(i + 1) / input_caret::Trail::count;
      const auto start = m_caretTrail.sample(p, i);
      const auto end = i + 1 == input_caret::Trail::count ? m_caretTrail.to
                                                       : m_caretTrail.sample(p, i + 1);
      float x1 = start.x + start.width * .5F, y1 = start.y + start.height * .5F;
      float x2 = end.x + end.width * .5F, y2 = end.y + end.height * .5F;
      float thickness = std::max(1.0F / m_cursorRenderScale, m_caretTrail.to.height * .10F);
      if (m_caretEffect == CaretEffect::Pulse) {
        constexpr float tau = 6.28318530718F;
        const float radius = m_caretTrail.to.height * (.20F + .65F * p);
        const float cx = m_caretTrail.to.x + m_caretTrail.to.width * .5F;
        const float cy = m_caretTrail.to.y + m_caretTrail.to.height * .5F;
        x1 = cx + std::cos(a * tau) * radius; y1 = cy + std::sin(a * tau) * radius;
        x2 = cx + std::cos(b * tau) * radius; y2 = cy + std::sin(b * tau) * radius;
      } else if (m_caretEffect == CaretEffect::Ribbon) {
        const float wave = m_caretTrail.to.height * .16F * (1.0F - p);
        y1 += std::sin(a * 6.28318530718F + p * 6.28318530718F) * wave;
        y2 += std::sin(b * 6.28318530718F + p * 6.28318530718F) * wave;
      } else {
        thickness = std::max(thickness, m_caretTrail.to.height * (.08F + .55F * b));
      }
      rotation = std::atan2(y2 - y1, x2 - x1);
      const float length = std::hypot(x2 - x1, y2 - y1);
      // Position the unrotated segment around its starting center, not its top-left.
      rect = {x1 + std::sin(rotation) * thickness * .5F,
              y1 - std::cos(rotation) * thickness * .5F, length + .5F / m_cursorRenderScale, thickness};
    }
    stamp->setTransformOrigin(0.0F, 0.0F);
    stamp->setRotation(rotation);
    stamp->setPosition(rect.x, rect.y);
    stamp->setFrameSize(rect.width, rect.height);
    stamp->setOpacity(m_caretEffect == CaretEffect::Trail
        ? m_caretTrail.opacity(progress, i, m_caretStrength)
        : m_caretStrength * (1.0F - std::clamp(progress, 0.0F, 1.0F)));
    stamp->setVisible(m_caretTrail.active && rect.width > 0.0F && rect.height > 0.0F);
  }
}

void Input::updateInteractiveGeometry() {
  if (m_cursor == nullptr || m_selectionRect == nullptr) {
    return;
  }

  const float pixelScale = m_cursorRenderScale;
  const float cursorWidth = std::max(1.0F, std::round(
      kCursorWidth * m_fontSize / Style::fontSizeBody * pixelScale)) / pixelScale;
  float viewportX = 0.0F, viewportY = 0.0F;
  Node::absolutePosition(m_textViewport, viewportX, viewportY);
  auto placeCursor = [&](float x, float y, float height) {
    const float minX = std::ceil(viewportX * pixelScale) / pixelScale;
    const float maxX = std::max(minX,
        std::floor((viewportX + textViewportWidth()) * pixelScale) / pixelScale - cursorWidth);
    const float left = std::clamp(std::round((viewportX + x) * pixelScale) / pixelScale, minX, maxX);
    const float top = std::round((viewportY + y) * pixelScale) / pixelScale;
    const float bottom = std::round((viewportY + y + height) * pixelScale) / pixelScale;
    m_caretInsertionRect = {left - viewportX, top - viewportY, cursorWidth,
                           std::max(1.0F / pixelScale, bottom - top)};
    float cellX = x, cellWidth = cursorWidth;
    const auto& cells = m_caretCells.empty() ? m_stopRect : m_caretCells;
    if (m_caretShape != CaretShape::Line && cells.size() == m_stopByte.size() && !cells.empty()) {
      const auto& cell = cells[stopIndexForByte(m_cursorPos)];
      if (cell.cellWidth > 0.0F) {
        cellX += cell.cellX - cell.x;
        cellWidth = cell.cellWidth;
      }
    }
    updateCaretPaint(cellX, cellWidth);
    updateInlineGhost();
  };
  updateCursorVisibility();

  if (m_multiline) {
    // Vertical scroll is maintained by the callers (ensureCursorVisibleY /
    // wheel / drag); this only places the nodes for the current offset.
    syncLabelScrollPosition();

    TextCursorStop stop{0.0F, 0.0F, currentLineHeight()};
    if (multilineStopsValid()) {
      stop = m_stopRect[stopIndexForByte(m_cursorPos)];
    }
    placeCursor(stop.x, stop.y + (displayProjected() ? m_projectedOriginY : kMultilinePadV) -
                    m_scrollOffsetY, std::max(1.0F, stop.height));

    m_selectionRect->setVisible(false);
    if (displayProjected()) updateProjectedSelection();
    else updateMultilineSelection();
    return;
  }

  const float previousScrollOffset = m_scrollOffset;
  const bool shouldRevealCursor =
      m_inputArea != nullptr && (m_inputArea->focused() || m_inputArea->pressed() || hasSelection());
  if (shouldRevealCursor) {
    ensureCursorVisible();
  } else {
    m_scrollOffset = restingScrollOffset();
  }
  if (std::abs(m_scrollOffset - previousScrollOffset) > 0.001F) {
    if (!labelSlicingEnabled()) {
      syncLabelScrollPosition();
      markPaintDirty();
    } else {
      std::size_t sliceStart = visibleLabelStartByte();
      if (sliceStart > 0) {
        sliceStart = prevCharPos(m_value, sliceStart + 1);
      }
      if (!m_value.empty() && sliceStart != m_labelVisibleStartByte) {
        markLayoutDirty();
      } else {
        syncLabelScrollPosition();
        markPaintDirty();
      }
    }
  }
  if (displayProjected() && m_caretCells.size() == m_stopByte.size() && !m_caretCells.empty()) {
    const auto& stop = m_caretCells[stopIndexForByte(m_cursorPos)];
    placeCursor(stop.x - m_scrollOffset + m_contentLeadSlack,
                stop.y + m_projectedOriginY, std::max(1.0F, stop.height));
    m_selectionRect->setVisible(false);
    updateProjectedSelection();
    return;
  }

  const float controlHeight = height() > 0.0F ? height() : m_controlHeight;
  const float maxCursorHeight = std::max(0.0F, controlHeight - kCursorPadV * 2.0F);
  const float cursorHeight =
      std::clamp(controlHeight * kCursorHeightRatio, std::min(kCursorMinHeight, maxCursorHeight), maxCursorHeight);
  const float cursorY = std::round((controlHeight - cursorHeight) * 0.5F);
  const float cursorX = stopXForByte(m_cursorPos) - m_scrollOffset + m_contentLeadSlack;
  placeCursor(cursorX, cursorY, cursorHeight);

  if (hasSelection() && !m_charSpans.empty() && m_charSpans.size() + 1 == m_stopByte.size()) {
    // In bidi text a logical selection can cover several separate pieces on screen.
    std::vector<TextCharSpan> pieces;
    for (std::size_t i = 0; i < m_charSpans.size(); ++i) {
      if (m_stopByte[i] >= selectionStart() && m_stopByte[i] < selectionEnd()) {
        pieces.push_back(m_charSpans[i]);
      }
    }
    std::ranges::sort(pieces, {}, &TextCharSpan::left);
    std::vector<TextCharSpan> merged;
    for (const TextCharSpan& piece : pieces) {
      if (!merged.empty() && piece.left <= merged.back().right + 0.5F) {
        merged.back().right = std::max(merged.back().right, piece.right);
      } else {
        merged.push_back(piece);
      }
    }
    m_selectionRect->setVisible(false);
    syncSelectionLineRects(merged.size());
    for (std::size_t i = 0; i < merged.size(); ++i) {
      RectNode* rect = m_selectionLineRects[i];
      rect->setPosition(merged[i].left - m_scrollOffset + m_contentLeadSlack, cursorY);
      rect->setFrameSize(std::max(1.0F, merged[i].right - merged[i].left), cursorHeight);
      rect->setVisible(true);
    }
  } else if (hasSelection()) {
    syncSelectionLineRects(0);
    const float selX0 = stopXForByte(selectionStart()) - m_scrollOffset + m_contentLeadSlack;
    const float selX1 = stopXForByte(selectionEnd()) - m_scrollOffset + m_contentLeadSlack;
    m_selectionRect->setPosition(std::min(selX0, selX1), cursorY);
    m_selectionRect->setFrameSize(std::abs(selX1 - selX0), cursorHeight);
    m_selectionRect->setVisible(true);
  } else {
    syncSelectionLineRects(0);
    m_selectionRect->setVisible(false);
  }
}

void Input::ensureCursorVisible() {
  if (m_value.empty() || m_stopX.empty()) {
    m_scrollOffset = 0.0F;
    return;
  }

  const float viewportWidth = textViewportWidth();
  if (viewportWidth <= 0.0F) {
    m_scrollOffset = 0.0F;
    return;
  }

  const float cursorContentX = stopXForByte(m_cursorPos);
  const float revealPad = std::max(kCursorRevealPadding, kTextInnerInset);
  const float slack = m_contentLeadSlack;
  const float cursorVx = cursorContentX - m_scrollOffset + slack;
  const float leftEdge = revealPad;
  const float rightEdge = viewportWidth - revealPad - kCursorWidth;

  if (cursorVx < leftEdge) {
    m_scrollOffset = cursorContentX + slack - leftEdge;
  } else if (cursorVx > rightEdge) {
    m_scrollOffset = cursorContentX + slack - rightEdge;
  }

  clampScrollOffset();
}

void Input::clampScrollOffset() {
  if (m_value.empty() || m_stopX.empty()) {
    m_scrollOffset = 0.0F;
    return;
  }
  const float textExtent = *std::ranges::max_element(m_stopX);
  const float maxOffset = std::max(0.0F, textExtent - textViewportWidth() + kCursorWidth + kCursorRevealPadding);
  m_scrollOffset = std::clamp(m_scrollOffset, 0.0F, maxOffset);
}

bool Input::multilineStopsValid() const noexcept {
  return !m_textMetricsDirty && !m_stopRect.empty() && m_stopRect.size() == m_stopByte.size();
}

std::size_t Input::stopIndexForByte(std::size_t bytePos) const {
  if (m_stopByte.empty()) {
    return 0;
  }
  const auto it = std::ranges::lower_bound(m_stopByte, bytePos);
  if (it == m_stopByte.end()) {
    return m_stopByte.size() - 1;
  }
  return static_cast<std::size_t>(it - m_stopByte.begin());
}

float Input::contentTextHeight() const noexcept {
  return m_contentTextHeight;
}

float Input::textViewportHeight() const noexcept {
  const float h = height() > 0.0F ? height() : m_controlHeight;
  return std::max(0.0F, h - kMultilinePadV * 2.0F);
}

float Input::currentLineHeight() const noexcept {
  if (multilineStopsValid()) {
    return std::max(1.0F, m_stopRect[stopIndexForByte(m_cursorPos)].lineHeight);
  }
  return std::max(1.0F, m_logicalStrut);
}

float Input::measuredMultilineHeight() const noexcept {
  return std::max(m_logicalStrut, contentTextHeight()) + kMultilinePadV * 2.0F;
}

void Input::ensureCursorVisibleY() {
  if (!multilineStopsValid()) {
    m_scrollOffsetY = 0.0F;
    return;
  }
  const float viewH = textViewportHeight();
  if (viewH <= 0.0F) {
    m_scrollOffsetY = 0.0F;
    return;
  }
  const TextCursorStop& stop = m_stopRect[stopIndexForByte(m_cursorPos)];
  if (stop.lineY < m_scrollOffsetY) {
    m_scrollOffsetY = stop.lineY;
  } else if (stop.lineY + stop.lineHeight > m_scrollOffsetY + viewH) {
    m_scrollOffsetY = stop.lineY + stop.lineHeight - viewH;
  }
  clampScrollOffsetY();
}

void Input::clampScrollOffsetY() {
  const float maxOffset = std::max(0.0F, contentTextHeight() - textViewportHeight());
  m_scrollOffsetY = std::clamp(m_scrollOffsetY, 0.0F, maxOffset);
}

void Input::clampEditState() {
  m_cursorPos = clampToUtf8End(m_value, m_cursorPos);
  m_selectionAnchor = clampToUtf8End(m_value, m_selectionAnchor);

  if (m_preeditLen == 0) {
    m_preeditStart = 0;
    return;
  }

  if (m_preeditStart > m_value.size()) {
    m_preeditStart = 0;
    m_preeditLen = 0;
    return;
  }

  m_preeditStart = clampToUtf8End(m_value, m_preeditStart);
  m_preeditLen = std::min(m_preeditLen, m_value.size() - m_preeditStart);
  if (m_preeditLen == 0) {
    m_preeditStart = 0;
  }
}

LayoutSize Input::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  const float minFromHint = m_minLayoutWidth > 0.0F ? m_minLayoutWidth : 0.0F;
  float assignW = width() > 0.0F ? width() : (minFromHint > 0.0F ? minFromHint : kMinWidth);
  if (constraints.hasExactWidth()) {
    assignW = std::max(constraints.maxWidth, minFromHint);
  }
  float assignH = m_multiline && height() > 0.0F ? height() : m_controlHeight;
  if (m_multiline && constraints.hasExactHeight()) {
    assignH = constraints.maxHeight;
  }
  setSize(assignW, assignH);
  if (m_multiline && m_stopsBuiltForWidth != textViewportWidth()) {
    m_textMetricsDirty = true;
    m_revealCursorYPending = true;
  }
  rebuildCursorStops(renderer);
  const float w = std::max(width(), minFromHint);
  return constraints.constrain(LayoutSize{.width = w, .height = assignH});
}

void Input::updateInlineGhost() {
  if (m_inlineGhost == nullptr) return;
  const bool show = !m_inlineGhost->text().empty() && caretPaintEligible() &&
                    !composing() && !m_passwordMode && m_cursorPos == m_value.size() &&
                    !m_textMetricsDirty;
  m_inlineGhost->setVisible(show);
  if (!show) return;
  m_inlineGhost->setFontSize(m_fontSize);
  if (m_inlineGhost->fontFamily() != m_label->font().family)
    m_inlineGhost->setFontFamily(m_label->font().family);
  if (m_inlineGhost->fontFeatures() != m_label->font().features)
    m_inlineGhost->setFontFeatures(m_label->font().features);
  m_inlineGhost->setFontWeight(m_label->font().weight);
  Color color = m_foreground.value_or(resolved(
      m_embeddedOnSolidPrimary && !m_frameVisible ? ColorRole::OnPrimary : ColorRole::OnSurface));
  color.a *= 0.4F;
  m_inlineGhost->setColor(color);
  const auto& stops = m_multiline ? m_stopRect : m_caretCells;
  const auto index = stopIndexForByte(m_cursorPos);
  const float lineY = index < stops.size() ? stops[index].lineY : 0.0F;
  const float x = m_multiline && index < stops.size() ? stops[index].x
      : stopXForByte(m_cursorPos) - m_scrollOffset + m_contentLeadSlack;
  m_inlineGhost->setPosition(x, m_label->y() + m_label->baselineOffset() + lineY);
}

void Input::updateCursorVisibility() {
  const bool eligible = caretPaintEligible();
  const bool blinkVisible = !MotionService::instance().enabled() || m_cursorBlinkVisible;
  m_cursor->setVisible(eligible && blinkVisible);
  if (m_caretText.clip != nullptr)
    m_caretText.clip->setVisible(m_cursor->visible() && m_caretShape == CaretShape::Box &&
                                !m_value.empty() && !passwordMasked() && !m_textMetricsDirty);
  if (!eligible || !MotionService::instance().enabled()) resetCaretEffects();
  updateInlineGhost();
}

void Input::revealCursor() {
  m_cursorBlinkVisible = true;
  updateCursorVisibility();
  markPaintDirty();
  if (m_inputArea != nullptr && m_inputArea->focused() && !hasSelection() &&
      !m_preeditCaretHidden && MotionService::instance().enabled()) {
    m_cursorBlinkTimer.start(kCursorBlinkResumeDelay, [this] { startCursorBlink(); });
  } else {
    m_cursorBlinkTimer.stop();
  }
}

void Input::startCursorBlink() {
  if (!MotionService::instance().enabled() || hasSelection() || m_preeditCaretHidden) {
    m_cursorBlinkTimer.stop();
    m_cursorBlinkVisible = true;
    updateCursorVisibility();
    return;
  }
  m_cursorBlinkTimer.startRepeating(kCursorBlinkInterval, [this] {
    if (m_inputArea == nullptr || !m_inputArea->focused()) {
      stopCursorBlink();
      return;
    }
    if (!MotionService::instance().enabled()) {
      startCursorBlink();
      return;
    }
    m_cursorBlinkVisible = !m_cursorBlinkVisible;
    updateCursorVisibility();
    markPaintDirty();
  });
}

void Input::stopCursorBlink() {
  m_cursorBlinkTimer.stop();
  m_cursorBlinkVisible = false;
}

void Input::selectWordAtByteOffset(std::size_t offset) {
  const std::size_t start = wordStartForByteOffset(offset);
  const std::size_t end = wordEndForByteOffset(offset);
  m_selectionAnchor = start;
  m_cursorPos = end;
}

void Input::selectLineAtByteOffset(std::size_t offset) {
  if (m_passwordMode || !m_multiline) {
    m_selectionAnchor = 0;
    m_cursorPos = m_value.size();
    return;
  }

  std::size_t start = 0;
  std::size_t end = m_value.size();
  if (multilineStopsValid()) {
    start = lineStartForByte(offset);
    end = lineEndForByte(offset);
  } else {
    // Layout stops not ready yet — fall back to hard newline bounds.
    const std::size_t pos = std::min(offset, m_value.size());
    const auto left = m_value.rfind('\n', pos == 0 ? 0 : pos - 1);
    start = left == std::string::npos ? 0 : left + 1;
    const auto right = m_value.find('\n', pos);
    end = right == std::string::npos ? m_value.size() : right;
  }
  m_selectionAnchor = start;
  m_cursorPos = end;
}

void Input::extendPointerSelectionToByteOffset(std::size_t offset) {
  if (m_pointerSelectGranularity == PointerSelectGranularity::Character) {
    m_cursorPos = offset;
    return;
  }

  std::size_t unitStart = offset;
  std::size_t unitEnd = offset;
  if (m_pointerSelectGranularity == PointerSelectGranularity::Word) {
    unitStart = wordStartForByteOffset(offset);
    unitEnd = wordEndForByteOffset(offset);
  } else {
    if (m_passwordMode || !m_multiline) {
      unitStart = 0;
      unitEnd = m_value.size();
    } else if (multilineStopsValid()) {
      unitStart = lineStartForByte(offset);
      unitEnd = lineEndForByte(offset);
    } else {
      const std::size_t pos = std::min(offset, m_value.size());
      const auto left = m_value.rfind('\n', pos == 0 ? 0 : pos - 1);
      unitStart = left == std::string::npos ? 0 : left + 1;
      const auto right = m_value.find('\n', pos);
      unitEnd = right == std::string::npos ? m_value.size() : right;
    }
  }

  m_selectionAnchor = std::min(m_pointerSelectPivotStart, unitStart);
  m_cursorPos = std::max(m_pointerSelectPivotEnd, unitEnd);
}

std::size_t Input::wordStartForByteOffset(std::size_t offset) const {
  if (m_passwordMode) return 0;
  const auto& text = displayProjected() ? m_displayProjection.text : m_value;
  if (text.empty()) return displayProjected() ? std::min(offset, m_value.size()) : 0;
  std::size_t pos = std::min(displayByte(offset), text.size());
  if (pos == text.size() && pos > 0) pos = prevCharPos(text, pos);
  if (isWordCodepoint(text, pos)) {
    while (pos > 0) {
      const std::size_t prev = prevCharPos(text, pos);
      if (prev == pos || !isWordCodepoint(text, prev)) break;
      pos = prev;
    }
  }
  return rawByteForDisplay(pos, true);
}

std::size_t Input::wordEndForByteOffset(std::size_t offset) const {
  if (m_passwordMode) return m_value.size();
  const auto& text = displayProjected() ? m_displayProjection.text : m_value;
  if (text.empty()) return displayProjected() ? std::min(offset, m_value.size()) : 0;
  std::size_t pos = std::min(displayByte(offset), text.size());
  if (pos == text.size() && pos > 0) pos = prevCharPos(text, pos);
  std::size_t end = nextCharPos(text, pos);
  if (isWordCodepoint(text, pos)) {
    while (end < text.size() && isWordCodepoint(text, end)) end = nextCharPos(text, end);
  }
  return rawByteForDisplay(end, false);
}

std::size_t Input::previousWordStartForByteOffset(std::size_t offset) const {
  if (m_passwordMode) return 0;
  const auto& text = displayProjected() ? m_displayProjection.text : m_value;
  if (text.empty()) return displayProjected() ? std::min(offset, m_value.size()) : 0;
  std::size_t pos = std::min(displayByte(offset), text.size());
  while (pos > 0) {
    const std::size_t prev = prevCharPos(text, pos);
    if (prev == pos || isWordCodepoint(text, prev)) break;
    pos = prev;
  }
  while (pos > 0) {
    const std::size_t prev = prevCharPos(text, pos);
    if (prev == pos || !isWordCodepoint(text, prev)) break;
    pos = prev;
  }
  return std::min(offset, rawByteForDisplay(pos, true));
}

std::size_t Input::nextWordStartForByteOffset(std::size_t offset) const {
  if (m_passwordMode) return m_value.size();
  const auto& text = displayProjected() ? m_displayProjection.text : m_value;
  if (text.empty()) return displayProjected() ? std::min(offset, m_value.size()) : 0;
  std::size_t pos = std::min(displayByte(offset), text.size());
  if (pos < text.size() && isWordCodepoint(text, pos)) {
    while (pos < text.size() && isWordCodepoint(text, pos)) pos = nextCharPos(text, pos);
  }
  while (pos < text.size() && !isWordCodepoint(text, pos)) pos = nextCharPos(text, pos);
  // Next starts trail their leading metadata; the terminal edge stays inside the last word's style.
  return std::max(std::min(offset, m_value.size()), rawByteForDisplay(pos, pos < text.size()));
}

std::size_t Input::nextWordEndForByteOffset(std::size_t offset) const {
  if (m_passwordMode) return m_value.size();
  const auto& text = displayProjected() ? m_displayProjection.text : m_value;
  if (text.empty()) return displayProjected() ? std::min(offset, m_value.size()) : 0;
  std::size_t pos = std::min(displayByte(offset), text.size());
  while (pos < text.size() && !isWordCodepoint(text, pos)) pos = nextCharPos(text, pos);
  while (pos < text.size() && isWordCodepoint(text, pos)) pos = nextCharPos(text, pos);
  return std::max(std::min(offset, m_value.size()), rawByteForDisplay(pos, false));
}

void Input::syncPasswordGlyphNodes(std::size_t count) {
  if (m_textViewport == nullptr) {
    return;
  }
  while (m_passwordGlyphs.size() > count) {
    auto* node = m_passwordGlyphs.back();
    m_textViewport->removeChild(node);
    m_passwordGlyphs.pop_back();
  }
  while (m_passwordGlyphs.size() < count) {
    auto glyph = std::make_unique<GlyphNode>();
    auto* glyphPtr = static_cast<GlyphNode*>(m_textViewport->insertChildAt(2, std::move(glyph)));
    m_passwordGlyphs.push_back(glyphPtr);
    // Animate the glyph entrance
    animatePasswordGlyphIn(*glyphPtr);
  }
}

void Input::animatePasswordGlyphIn(GlyphNode& glyph) {
  auto* animations = animationManager();
  if (animations == nullptr) {
    return;
  }

  constexpr float kStartScale = 0.15F;
  // Fraction of the animation over which the glyph fades in.
  constexpr float kFadeInFraction = 0.35F;
  // Three alternating swings (right, left, right), starting and ending at zero rotation.
  constexpr float kWobbleSwings = 3.0F;
  constexpr float kWobbleAmplitude = 0.45F; // radians, ~26 degrees

  glyph.setOpacity(0.0F);
  glyph.setScale(kStartScale);
  glyph.setRotation(0.0F);

  auto* glyphPtr = &glyph;
  // Driven linearly so each animated property carries its own curve. The owner is the glyph node, so a
  // backspace that destroys it cancels this animation.
  animations->animate(
      0.0F, 1.0F, static_cast<float>(Style::animNormal), Easing::Linear,
      [glyphPtr](float t) {
        const float scaleEase = applyEasing(Easing::EaseOutCubic, t);
        glyphPtr->setScale(kStartScale + (1.0F - kStartScale) * scaleEase);
        glyphPtr->setOpacity(applyEasing(Easing::EaseOutCubic, std::min(1.0F, t / kFadeInFraction)));

        // Rotation stays on even for the rotationally symmetric circle mask: a non-zero rotation is what makes
        // the glyph renderer skip pixel snapping, which would otherwise jump the glyph a whole pixel per frame.
        const float wobbleDecay = applyEasing(Easing::EaseOutCubic, 1.0F - t);
        const float wobble = kWobbleAmplitude * std::sin(t * kWobbleSwings * std::numbers::pi_v<float>) * wobbleDecay;
        glyphPtr->setRotation(wobble);
      },
      [glyphPtr] {
        glyphPtr->setOpacity(1.0F);
        glyphPtr->setScale(1.0F);
        glyphPtr->setRotation(0.0F);
      },
      glyphPtr
  );
}

float Input::textViewportWidth() const noexcept {
  const float w = width() > 0.0F ? width() : kMinWidth;
  const float textInset = m_horizontalPadding + kTextInnerInset;
  const float rightInset = clearButtonVisible() ? clearButtonTextReserveWidth() : textInset;
  return std::max(0.0F, w - textInset - rightInset);
}

bool Input::clearButtonVisible() const noexcept { return m_clearButtonEnabled && !m_value.empty() && !m_multiline; }

float Input::clearButtonHitWidth() const noexcept {
  if (!clearButtonVisible()) {
    return 0.0F;
  }
  const float h = height() > 0.0F ? height() : m_controlHeight;
  return std::max(0.0F, h);
}

float Input::clearButtonTextReserveWidth() const noexcept {
  if (!clearButtonVisible()) {
    return 0.0F;
  }
  const float clearGlyphSize = std::round(m_fontSize * kClearGlyphScale);
  return clearButtonHitWidth() * 0.5F + clearGlyphSize * 0.5F + kTextInnerInset;
}

bool Input::hasSelection() const noexcept { return selectionStart() != selectionEnd(); }

std::size_t Input::selectionStart() const noexcept {
  return std::min({m_selectionAnchor, m_cursorPos, m_value.size()});
}

std::size_t Input::selectionEnd() const noexcept {
  return std::min(std::max(m_selectionAnchor, m_cursorPos), m_value.size());
}

Input::EditSnapshot Input::currentEditSnapshot() const {
  if (m_preeditBase) return *m_preeditBase;
  return EditSnapshot{
      .value = m_value,
      .cursorPos = clampToUtf8End(m_value, m_cursorPos),
      .selectionAnchor = clampToUtf8End(m_value, m_selectionAnchor),
  };
}

void Input::clearEditHistory() {
  m_editHistory->undo.clear();
  m_editHistory->redo.clear();
  resetUndoCoalescing();
}

void Input::resetUndoCoalescing() {
  m_lastEditCoalesceKind = EditCoalesceKind::None;
  m_lastUndoRecordTime = {};
  m_typingCoalesceCursorPos = m_cursorPos;
}

void Input::pushUndoSnapshot(EditCoalesceKind kind) {
  // Password mode keeps no edit history so the plaintext is never retained in a
  // snapshot and can't be restored (e.g. Ctrl+Z after clearing the field).
  if (m_passwordMode) {
    resetUndoCoalescing();
    return;
  }
  if (kind == EditCoalesceKind::None) {
    resetUndoCoalescing();
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  if (kind == EditCoalesceKind::Typing
      && m_lastEditCoalesceKind == EditCoalesceKind::Typing
      && m_cursorPos == m_typingCoalesceCursorPos
      && !hasSelection()
      && !m_editHistory->undo.empty()
      && now - m_lastUndoRecordTime <= kTypingUndoCoalesceWindow) {
    m_editHistory->redo.clear();
    m_lastUndoRecordTime = now;
    return;
  }

  const EditSnapshot snapshot = currentEditSnapshot();
  if (m_editHistory->undo.empty() || !(m_editHistory->undo.back() == snapshot)) {
    m_editHistory->undo.push_back(snapshot);
    if (m_editHistory->undo.size() > kUndoStackLimit) {
      m_editHistory->undo.erase(m_editHistory->undo.begin());
    }
  }
  m_editHistory->redo.clear();
  m_lastEditCoalesceKind = kind;
  m_lastUndoRecordTime = now;
  m_typingCoalesceCursorPos = m_cursorPos;
}

void Input::noteTypingEditEnd() { m_typingCoalesceCursorPos = m_cursorPos; }

bool Input::undoEdit() { return !m_passwordMode && restoreFromHistory(m_editHistory->undo, m_editHistory->redo); }

bool Input::redoEdit() { return !m_passwordMode && restoreFromHistory(m_editHistory->redo, m_editHistory->undo); }

bool Input::restoreFromHistory(std::vector<EditSnapshot>& source, std::vector<EditSnapshot>& target) {
  if (source.empty()) {
    resetUndoCoalescing();
    return false;
  }

  const EditSnapshot current = currentEditSnapshot();
  const EditSnapshot snapshot = source.back();
  source.pop_back();
  if (target.empty() || !(target.back() == current)) {
    target.push_back(current);
    if (target.size() > kUndoStackLimit) {
      target.erase(target.begin());
    }
  }
  restoreEditSnapshot(snapshot);
  resetUndoCoalescing();
  return true;
}

void Input::restoreEditSnapshot(const EditSnapshot& snapshot) {
  const std::string previousValue = m_value;
  m_value = snapshot.value;
  m_cursorPos = std::min(snapshot.cursorPos, m_value.size());
  m_selectionAnchor = std::min(snapshot.selectionAnchor, m_value.size());
  m_preeditStart = 0;
  m_preeditLen = 0;
  m_preeditBase.reset();
  m_preeditCaretHidden = false;
  updateDisplayText();
  updateInteractiveGeometry();
  revealCursor();
  applyVisualState();
  markTextContentChanged();
  markPaintDirty();
  notifyTextInputStateChanged(TextInputChangeCause::Other);
  if (m_value != previousValue && m_onChange) {
    m_onChange(m_value);
  }
}

std::size_t Input::xToByteOffset(float localX) const {
  if (m_stopX.empty()) {
    return 0;
  }
  std::size_t best = 0;
  float bestDistance = std::abs(m_stopX.front() - localX);
  for (std::size_t i = 1; i < m_stopX.size(); ++i) {
    const float distance = std::abs(m_stopX[i] - localX);
    if (distance < bestDistance || (displayProjected() && distance == bestDistance &&
        displayByte(m_stopByte[i]) == displayByte(m_stopByte[best]))) {
      best = i;
      bestDistance = distance;
    }
  }
  return m_stopByte[best];
}

std::size_t Input::pointToByteOffset(float localX, float localY) const {
  if (!multilineStopsValid()) {
    return 0;
  }

  if (displayProjected()) localY += kMultilinePadV - m_projectedOriginY;
  // Find the visual line containing localY (clamped to first/last line).
  std::size_t lineFirst = 0;
  std::size_t lineLast = 0;
  for (std::size_t i = 0; i < m_stopRect.size();) {
    std::size_t j = i;
    while (j + 1 < m_stopRect.size() && m_stopRect[j + 1].lineIndex == m_stopRect[i].lineIndex) {
      ++j;
    }
    lineFirst = i;
    lineLast = j;
    const float bottom = m_stopRect[i].lineY + m_stopRect[i].lineHeight;
    if (localY < bottom || j + 1 >= m_stopRect.size()) {
      break;
    }
    i = j + 1;
  }

  // Nearest caret boundary by x within the line.
  std::size_t best = lineFirst;
  float bestDist = std::numeric_limits<float>::max();
  for (std::size_t i = lineFirst; i <= lineLast; ++i) {
    const float dist = std::abs(m_stopRect[i].x - localX);
    if (dist < bestDist || (displayProjected() && dist == bestDist &&
        displayByte(m_stopByte[i]) == displayByte(m_stopByte[best]))) {
      bestDist = dist;
      best = i;
    }
  }

  // Clicking past the end of a soft-wrapped line targets the wrap boundary
  // (one past the line's last char start) rather than before the last char.
  if (best == lineLast && lineLast + 1 < m_stopRect.size() && localX > m_stopRect[lineLast].x) {
    const auto& text = displayProjected() ? m_displayProjection.text : m_value;
    const std::size_t byte = displayByte(m_stopByte[lineLast]);
    if (byte < text.size() && text[byte] != '\n') {
      std::size_t next = lineLast + 1;
      while (next + 1 < m_stopByte.size() && displayProjected() &&
             displayByte(m_stopByte[next + 1]) == displayByte(m_stopByte[next])) ++next;
      return m_stopByte[next];
    }
  }
  return m_stopByte[best];
}

std::size_t Input::lineStartForByte(std::size_t bytePos) const {
  if (!multilineStopsValid()) {
    return 0;
  }
  std::size_t idx = stopIndexForByte(bytePos);
  while (idx > 0 && m_stopRect[idx - 1].lineIndex == m_stopRect[idx].lineIndex) {
    --idx;
  }
  return m_stopByte[idx];
}

std::size_t Input::lineEndForByte(std::size_t bytePos) const {
  if (!multilineStopsValid()) {
    return m_value.size();
  }
  std::size_t idx = stopIndexForByte(bytePos);
  while (idx + 1 < m_stopRect.size() && m_stopRect[idx + 1].lineIndex == m_stopRect[idx].lineIndex) {
    ++idx;
  }
  const std::size_t byte = m_stopByte[idx];
  const auto& text = displayProjected() ? m_displayProjection.text : m_value;
  const auto offset = displayByte(byte);
  if (offset >= text.size() || text[offset] == '\n') {
    // End-of-text stop, or the caret slot just before the newline: already the
    // visual end of this line.
    return byte;
  }
  // Soft-wrapped line: the end position is the wrap boundary one char further
  // (the caret renders at the next line's start — no cursor affinity).
  return idx + 1 < m_stopByte.size() ? m_stopByte[idx + 1] : byte;
}

std::size_t Input::byteForVerticalMove(std::size_t from, int lineDelta) {
  if (!multilineStopsValid() || lineDelta == 0) {
    return from;
  }
  const std::size_t fromIdx = stopIndexForByte(from);
  if (m_goalCaretX < 0.0F) {
    m_goalCaretX = m_stopRect[fromIdx].x;
  }

  const int target = m_stopRect[fromIdx].lineIndex + lineDelta;
  if (target < 0) {
    return 0;
  }
  if (target > m_stopRect.back().lineIndex) {
    return m_value.size();
  }

  std::size_t best = fromIdx;
  float bestDist = std::numeric_limits<float>::max();
  for (std::size_t i = 0; i < m_stopRect.size(); ++i) {
    if (m_stopRect[i].lineIndex != target) {
      continue;
    }
    const float dist = std::abs(m_stopRect[i].x - m_goalCaretX);
    if (dist < bestDist || (displayProjected() && dist == bestDist &&
        displayByte(m_stopByte[i]) == displayByte(m_stopByte[best]))) {
      bestDist = dist;
      best = i;
    }
  }
  return m_stopByte[best];
}

void Input::updateMultilineSelection() {
  if (!hasSelection() || !multilineStopsValid()) {
    syncSelectionLineRects(0);
    return;
  }

  struct LineSpan {
    float x0 = 0.0F;
    float x1 = 0.0F;
    float y = 0.0F;
    float h = 0.0F;
    int lineIndex = 0;
  };
  std::vector<LineSpan> lines;
  const std::size_t i0 = stopIndexForByte(selectionStart());
  const std::size_t i1 = std::min(stopIndexForByte(selectionEnd()), m_stopRect.size() - 1);
  const float stub = m_fontSize * kSelectionLineBreakStubEm;
  for (std::size_t i = i0; i < i1; ++i) {
    const TextCursorStop& a = m_stopRect[i];
    const TextCursorStop& b = m_stopRect[i + 1];
    // A char whose next boundary sits on another line ends its line; highlight
    // it with a short stub (its true advance is not in the stop data).
    const float boundaryX = a.lineIndex == b.lineIndex ? b.x : a.x + stub;
    const float spanStart = std::min(a.x, boundaryX);
    const float spanEnd = std::max(a.x, boundaryX);
    if (!lines.empty() && lines.back().lineIndex == a.lineIndex) {
      lines.back().x0 = std::min(lines.back().x0, spanStart);
      lines.back().x1 = std::max(lines.back().x1, spanEnd);
      lines.back().h = std::max(lines.back().h, a.lineHeight);
    } else {
      lines.push_back(LineSpan{spanStart, spanEnd, a.lineY, a.lineHeight, a.lineIndex});
    }
  }

  syncSelectionLineRects(lines.size());
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const LineSpan& span = lines[i];
    RectNode* rect = m_selectionLineRects[i];
    rect->setPosition(span.x0, span.y + kMultilinePadV - m_scrollOffsetY);
    rect->setFrameSize(std::max(1.0F, span.x1 - span.x0), std::max(1.0F, span.h));
    rect->setVisible(true);
  }
}

void Input::updateProjectedSelection() {
  if (!hasSelection() || m_textMetricsDirty || m_displayCells.size() != m_displayStopByte.size()) {
    syncSelectionLineRects(0);
    return;
  }
  const auto first = displayByte(selectionStart()), last = displayByte(selectionEnd());
  std::vector<TextCursorStop> pieces;
  for (std::size_t i = 0; i + 1 < m_displayStopByte.size(); ++i) {
    if (m_displayStopByte[i] < first || m_displayStopByte[i] >= last ||
        m_displayCells[i].cellWidth <= 0.0F) continue;
    const auto& stop = m_displayCells[i];
    const auto& next = m_displayCells[i + 1];
    // Hard line breaks keep their selection marker; only inline whitespace can wrap away.
    if (stop.cellIsVirtual && m_multiline && m_stopsBuiltForWidth > 0.0F) {
      const auto character = g_utf8_get_char(m_displayProjection.text.data() + m_displayStopByte[i]);
      if ((character == '\t' || g_unichar_type(character) == G_UNICODE_SPACE_SEPARATOR) &&
          (next.lineIndex != stop.lineIndex || next.x == stop.x)) continue;
    }
    pieces.push_back(stop);
  }
  std::ranges::sort(pieces, [](const auto& a, const auto& b) {
    return a.lineIndex == b.lineIndex ? a.cellX < b.cellX : a.lineIndex < b.lineIndex;
  });
  std::size_t count = 0;
  for (const auto& piece : pieces) {
    if (count > 0 && pieces[count - 1].lineIndex == piece.lineIndex &&
        piece.cellX <= pieces[count - 1].cellX + pieces[count - 1].cellWidth + 0.5F) {
      auto& previous = pieces[count - 1];
      previous.cellWidth = std::max(previous.cellX + previous.cellWidth,
                                    piece.cellX + piece.cellWidth) - previous.cellX;
      previous.lineHeight = std::max(previous.lineHeight, piece.lineHeight);
    } else {
      pieces[count++] = piece;
    }
  }
  syncSelectionLineRects(count);
  for (std::size_t i = 0; i < count; ++i) {
    const auto& piece = pieces[i];
    auto* rect = m_selectionLineRects[i];
    rect->setPosition(piece.cellX - (m_multiline ? 0.0F : m_scrollOffset - m_contentLeadSlack),
                      piece.lineY + m_projectedOriginY - (m_multiline ? m_scrollOffsetY : 0.0F));
    rect->setFrameSize(piece.cellWidth, std::max(1.0F, piece.lineHeight));
    rect->setVisible(true);
  }
}

void Input::syncSelectionLineRects(std::size_t count) {
  if (m_textViewport == nullptr) {
    return;
  }
  while (m_selectionLineRects.size() > count) {
    auto* node = m_selectionLineRects.back();
    m_textViewport->removeChild(node);
    m_selectionLineRects.pop_back();
  }
  while (m_selectionLineRects.size() < count) {
    auto rect = std::make_unique<RectNode>();
    rect->setStyle(m_selectionRect != nullptr ? m_selectionRect->style() : RoundedRectStyle{});
    rect->setOpacity(m_selectionBackground ? 1.0F : 0.3F);
    rect->setZIndex(displayProjected() ? 1 : 0);
    rect->setVisible(false);
    auto* rectPtr = static_cast<RectNode*>(m_textViewport->insertChildAt(0, std::move(rect)));
    m_selectionLineRects.push_back(rectPtr);
  }
}

float Input::stopXForByte(std::size_t bytePos) const {
  for (std::size_t i = 0; i < m_stopByte.size(); ++i) {
    if (m_stopByte[i] == bytePos) {
      return m_stopX[i];
    }
  }
  return m_stopX.empty() ? 0.0F : m_stopX.back();
}

std::size_t Input::displayByte(std::size_t rawByte) const noexcept {
  if (!displayProjected() || m_displayProjection.rawToDisplay.empty()) return rawByte;
  return m_displayProjection.rawToDisplay[std::min(rawByte, m_displayProjection.rawToDisplay.size() - 1)];
}

std::size_t Input::rawByteForDisplay(std::size_t displayOffset, bool trailing) const {
  if (!displayProjected() || m_displayProjection.rawToDisplay.empty())
    return clampToUtf8End(m_value, displayOffset);
  const auto& map = m_displayProjection.rawToDisplay;
  if (trailing) {
    const auto it = std::ranges::upper_bound(map, displayOffset);
    const auto raw = it == map.begin() ? 0 : static_cast<std::size_t>(it - map.begin() - 1);
    return clampToUtf8Start(m_value, raw);
  }
  const auto it = std::ranges::lower_bound(map, displayOffset);
  const auto raw = static_cast<std::size_t>(it - map.begin());
  return clampToUtf8End(m_value, raw);
}

std::size_t Input::adjacentVisibleByte(std::size_t rawByte, bool left) const {
  if (!displayProjected()) return left ? prevCharPos(m_value, rawByte) : nextCharPos(m_value, rawByte);
  const auto offset = displayByte(rawByte);
  auto next = rawByte;
  do {
    const auto previous = next;
    next = left ? prevCharPos(m_value, next) : nextCharPos(m_value, next);
    if (next == previous) return rawByte;
  } while (displayByte(next) == offset);
  // Metadata belongs to the boundary before the next displayed character.
  while (next < m_value.size()) {
    const auto following = nextCharPos(m_value, next);
    if (displayByte(following) != displayByte(next)) break;
    next = following;
  }
  return next;
}

std::size_t Input::visuallyAdjacentByte(std::size_t bytePos, bool left, bool wordStart) const {
  // A word move lands on the nearest word start on screen; the logical next or previous one can sit
  // on the other side of the caret when it lies on a bidi run boundary.
  std::vector<std::size_t> wordStarts;
  if (wordStart) {
    std::size_t pos = rawByteForDisplay(0, true);
    wordStarts.push_back(pos);
    while (pos < m_value.size()) {
      const std::size_t next = nextWordStartForByteOffset(pos);
      if (next == pos) {
        break;
      }
      pos = next;
      wordStarts.push_back(pos);
    }
  }
  const float fromX = stopXForByte(bytePos);
  if (displayProjected() && !wordStart) {
    const auto adjacent = adjacentVisibleByte(bytePos, left);
    if (adjacent != bytePos && stopXForByte(adjacent) == fromX) return adjacent;
  }
  std::size_t best = bytePos;
  float bestX = fromX;
  bool found = false;
  for (std::size_t i = 0; i < m_stopX.size() && i < m_stopByte.size(); ++i) {
    if (wordStart && !std::ranges::binary_search(wordStarts, m_stopByte[i])) {
      continue;
    }
    const float x = m_stopX[i];
    const bool ahead = left ? x < fromX - 0.5F : x > fromX + 0.5F;
    const bool closer = left ? x > bestX : x < bestX;
    if (ahead && (!found || closer || (displayProjected() && x == bestX &&
        displayByte(m_stopByte[i]) == displayByte(best)))) {
      best = m_stopByte[i];
      bestX = x;
      found = true;
    }
  }
  return best;
}

bool Input::valueLaidOutRtl() const noexcept {
  // Pango resolves the paragraph direction from the text itself, independent of the UI direction. An RTL
  // paragraph places the logical start on its right edge and the logical end on its left.
  return !m_multiline && !passwordMasked() && m_stopX.size() > 1 && m_stopX.front() > m_stopX.back();
}

bool Input::labelSlicingEnabled() const noexcept {
  // A slice is laid out on its own, which only lines up with the full-text stops when every stop
  // advances left to right with the byte offset.
  return !displayProjected() && !Style::rtl() && m_stopsInLogicalOrder;
}

float Input::restingScrollOffset() const {
  // An unfocused field shows the logical start of its value, which is the right end of RTL text.
  if (!valueLaidOutRtl()) {
    return 0.0F;
  }
  return std::max(0.0F, *std::ranges::max_element(m_stopX) - textViewportWidth());
}

std::size_t Input::nextCharPos(const std::string& s, std::size_t pos) {
  if (pos >= s.size()) {
    return s.size();
  }
  ++pos;
  while (pos < s.size() && (static_cast<unsigned char>(s[pos]) & 0xC0U) == 0x80U) {
    ++pos;
  }
  return pos;
}

std::size_t Input::prevCharPos(const std::string& s, std::size_t pos) {
  pos = std::min(pos, s.size());
  if (pos == 0) {
    return 0;
  }
  --pos;
  while (pos > 0 && (static_cast<unsigned char>(s[pos]) & 0xC0U) == 0x80U) {
    --pos;
  }
  return pos;
}
