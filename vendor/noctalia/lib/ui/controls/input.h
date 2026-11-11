#pragma once

#include "core/timer_manager.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "render/scene/input_area.h"
#include "ui/controls/input_caret.h"
#include "ui/palette.h"
#include "ui/signal.h"
#include "ui/style.h"
#include "ui/text_input_client.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class TextClipboard;
class GlyphNode;
class Label;
class RectNode;
class TextNode;

class Input : public Node, public TextInputClient {
public:
  enum class PasswordMaskStyle : std::uint8_t {
    CircleFilled = 0,
    RandomIcons = 1,
  };

  Input();
  ~Input() override;

  enum class CaretShape : std::uint8_t { Line = 0, Box = 1, Underline = 2 };
  enum class CaretEffect : std::uint8_t { Off = 0, Trail = 1, Comet = 2, Ribbon = 3, Pulse = 4 };
  // Box fills the native grapheme cell and repaints its shaped glyph in a contrasting color.
  void setCaretStyle(CaretShape shape, CaretEffect effect, float durationMs = 120.0F, float strength = 0.20F);
  // Paint state only. Call on buffer identity changes, pane hiding and external geometry changes.
  void resetCaretEffects();
  void setAnimationManager(AnimationManager* manager) override;
  // Native wrapped logical height plus padding, floored by the same font strut for empty/nonempty text.
  float measuredMultilineHeight() const noexcept;

  struct EditSnapshot {
    std::string value;
    std::size_t cursorPos = 0;
    std::size_t selectionAnchor = 0;
    bool operator==(const EditSnapshot&) const = default;
  };
  struct EditHistory {
    std::vector<EditSnapshot> undo, redo;
  };
  // History is shared, not copied, when a document is displayed in multiple panes.
  struct EditingState {
    EditSnapshot current;
    std::optional<EditSnapshot> preeditBase;
    std::size_t preeditStart = 0, preeditLength = 0;
    std::shared_ptr<EditHistory> history = std::make_shared<EditHistory>();
    float scrollX = 0.0F, scrollY = 0.0F;
    bool preeditCaretHidden = false;
  };
  EditingState editingState() const;
  // Committed composition state can change while displayed text/caret remain identical.
  bool editingStateMatches(const EditingState& state) const noexcept;
  // Same-document peers can restore shared text/history while retaining their own viewport.
  void restoreEditingState(const EditingState& state, bool preserveScroll = false);
  bool insertText(std::string_view text);
  // Raw committed UTF-8 byte under an actual visible shaped cell, never a nearest caret.
  std::optional<std::size_t> textByteAtPoint(float localX, float localY) const;
  // Discrete native edit; invalid UTF-8 boundaries and active composition are rejected.
  bool replaceRange(std::size_t rawBegin, std::size_t rawEnd, std::string_view text);
  std::size_t cursorPosition() const noexcept { return m_cursorPos; }
  std::size_t selectionAnchor() const noexcept { return m_selectionAnchor; }
  bool composing() const noexcept { return m_preeditBase.has_value(); }
  bool textGeometryDirty() const noexcept { return m_textMetricsDirty; }
  static bool usesTextGeometry(std::uint32_t sym) noexcept;
  void setOnEditingStateChanged(std::function<void()> callback);
  // Runs before keyboard, clipboard, or IME insertion. Rejection changes no text/history.
  void setEditValidator(std::function<bool(std::string_view, std::string_view, bool)> validator);
  struct DisplayProjection {
    std::string text, markup, contrastMarkup;
    // One entry per raw byte plus the end. Legal raw UTF-8 boundaries map
    // monotonically to legal decoded-display UTF-8 boundaries, including duplicates.
    std::vector<std::size_t> rawToDisplay;
  };
  // Trusted escaped Pango markup: both markup strings decode to text with identical shaping.
  // contrastMarkup omits foreground/background colors for native contrast overlays.
  // Password fields ignore projection; the raw document, history and IME stay native.
  void setDisplayProjector(std::function<DisplayProjection(std::string_view)> projector);
  // Decoration-only refresh preserves native geometry/effects when text and mapping are unchanged.
  void refreshDisplayProjection(bool decorationOnly = false);
  // The cached result is replaced only by a display-text update or explicit refresh.
  const DisplayProjection* displayProjection() const noexcept {
    return displayProjected() ? &m_displayProjection : nullptr;
  }
  // Word commands use decoded display text but return legal raw boundaries.
  // Backward starts trail leading metadata; forward ends precede trailing metadata.
  // Ctrl+Right seeks the next start with trailing affinity, except at the terminal word end.
  std::size_t projectedWordBoundary(std::size_t rawByte, bool forward) const;
  // Rewrite real edit candidates before validation/commit, never history/restores.
  // Cursor/anchor are raw UTF-8 bytes; bool is preedit. Rewritten candidates still pass the validator.
  void setEditRewriter(std::function<void(const EditSnapshot&, EditSnapshot&, bool)> rewriter);
  void setFontFamily(std::string family);
  void setFontFeatures(std::string features);
  void setTextColors(std::optional<Color> foreground, std::optional<Color> selectionBackground,
                     std::optional<Color> selectionForeground, std::optional<Color> caret);
  void setValue(std::string_view value);
  void setPlaceholder(std::string_view placeholder);
  // Paint-only suffix at the shaped end caret; never part of the document or projection.
  // Hidden while unfocused, selected, composing, password-masked, or away from the end.
  void setInlineGhost(std::string text);
  void setFontSize(float size);
  void setControlHeight(float height);
  void setHorizontalPadding(float padding);
  void setClearButtonEnabled(bool enabled);
  void setPasswordMode(bool enabled);
  /// Shows a password-mode value as plain text. Display only: the field keeps password semantics (no
  /// clipboard export, no undo history, sensitive text-input hints). Ignored outside password mode;
  /// entering or leaving password mode conceals the value again.
  void setPasswordRevealed(bool revealed);
  /// Multi-line editing: Enter inserts '\n' (Ctrl+Enter submits), the text wraps
  /// at the viewport width and scrolls vertically. The control keeps whatever
  /// height layout assigns (explicit height or flex-grown) instead of forcing
  /// the single-line control height. Mutually exclusive with password mode.
  void setMultiline(bool enabled);
  void setInvalid(bool invalid);
  void setFrameVisible(bool visible);
  /// When the frame is hidden, treat the field as sitting on a solid Primary fill (e.g. segmented control center).
  void setEmbeddedOnSolidPrimary(bool embedded);
  void setFontWeight(FontWeight fontWeight);
  void setMinLayoutWidth(float width);
  void setTextAlign(TextAlign align);
  void setOnChange(std::function<void(const std::string&)> callback);
  void setOnSubmit(std::function<void(const std::string&)> callback);
  void setOnKeyEvent(std::function<bool(std::uint32_t sym, std::uint32_t modifiers)> callback);
  // Pointer invocation preserves selection/composition; nullptr denotes Menu or Shift+F10.
  void setOnContextMenu(std::function<void(const InputArea::PointerData*)> callback);
  void setOnFocusLoss(std::function<void()> callback);
  void setOnFocusGain(std::function<void()> callback);
  void setSubmitOnFocusLoss(bool enabled);
  /// Multiline chat-style submit: Enter submits and Shift+Enter inserts a
  /// newline. Off (default), Enter inserts a newline and Ctrl+Enter submits.
  void setSubmitOnEnter(bool enabled);
  void setEnabled(bool enabled);
  void setSurfaceOpacity(float opacity);
  /// Unfocused frame fill. Fields on a SurfaceContainer card use Surface so they stand out; focus always fills with
  /// Surface.
  void setSurfaceRole(ColorRole role);
  /// Explicit native logical radius, independent of the control height.
  void setFrameRadius(float radius);
  bool enabled() const noexcept { return m_enabled; }
  bool passwordRevealed() const noexcept { return m_passwordRevealed; }
  void selectAll();
  void moveCaretLeft(bool shift = false);
  void moveCaretRight(bool shift = false);

  // Set once at application startup; all Input instances use this for Ctrl+C/X/V.
  static void setTextClipboard(TextClipboard* clipboard) noexcept;
  static TextClipboard* textClipboard() noexcept;
  /// Submit invokes onSubmit only when this matcher returns true (Application wires ConfigService validate keybinds).
  static void setValidateKeyMatcher(std::function<bool(std::uint32_t sym, std::uint32_t modifiers)> matcher) noexcept;
  static void setPasswordMaskStyle(PasswordMaskStyle style) noexcept;
  /// Readline-style editing shortcuts for every Input (Ctrl+A moves to the start instead of selecting all).
  static void setReadlineShortcutsEnabled(bool enabled) noexcept;
  void clearSelection();

  const std::string& value() const noexcept { return m_value; }
  InputArea* inputArea() const noexcept { return m_inputArea; }
  bool invalid() const noexcept { return m_invalid; }

  TextInputState textInputState() const override;
  void textInputApplyEdit(const TextInputEdit& edit) override;
  void textInputResetPreedit() override;
  void textInputActivated(TextInputService& service) override;
  void textInputDeactivated(TextInputService& service) override;

private:
  enum class EditCoalesceKind : std::uint8_t {
    None = 0,
    Typing = 1,
    Discrete = 2,
  };

  bool insertionAllowed(std::string_view text, bool preedit = false) const;
  bool replaceTextRange(std::size_t start, std::size_t end, std::string_view text,
                        EditCoalesceKind kind, bool preedit = false);
  bool rewriteEdit(const EditSnapshot& before, EditSnapshot& after, bool preedit) const;
  void trackPreeditRange(const EditSnapshot& before, std::size_t replacedStart, std::size_t replacedEnd);

  void doLayout(Renderer& renderer) override;
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doRebindRenderer(Renderer& renderer) override { m_renderer = &renderer; }
  void handleKey(std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers, bool preedit = false);
  void notifyTextInputStateChanged(TextInputChangeCause cause);
  bool removePreeditText();
  void applyVisualState();
  void updateDisplayText();
  void requestCaretUpdate();
  void updateInteractiveGeometry();
  bool caretPaintEligible() const;
  LayoutRect caretViewport() const;
  void updateCaretPaint(float cellX, float cellWidth);
  void paintCaretTrail(float progress);
  void applyCaretStyle(Color color);
  void clearFromButton();
  void updateCursorVisibility();
  void updateInlineGhost();
  void revealCursor();
  void startCursorBlink();
  void stopCursorBlink();
  void ensureCursorVisible();
  void clampScrollOffset();
  void clampEditState();
  void selectWordAtByteOffset(std::size_t offset);
  void selectLineAtByteOffset(std::size_t offset);
  void extendPointerSelectionToByteOffset(std::size_t offset);
  std::size_t wordStartForByteOffset(std::size_t offset) const;
  std::size_t wordEndForByteOffset(std::size_t offset) const;
  std::size_t previousWordStartForByteOffset(std::size_t offset) const;
  std::size_t nextWordStartForByteOffset(std::size_t offset) const;
  std::size_t nextWordEndForByteOffset(std::size_t offset) const;
  float textViewportWidth() const noexcept;
  bool clearButtonVisible() const noexcept;
  float clearButtonHitWidth() const noexcept;
  float clearButtonTextReserveWidth() const noexcept;
  bool hasSelection() const noexcept;
  std::size_t selectionStart() const noexcept;
  std::size_t selectionEnd() const noexcept;
  bool isReadOnlyVisual() const noexcept;
  bool passwordMasked() const noexcept { return m_passwordMode && !m_passwordRevealed; }
  bool displayProjected() const noexcept { return bool(m_displayProjector) && !m_passwordMode; }
  std::size_t displayByte(std::size_t rawByte) const noexcept;
  std::size_t rawByteForDisplay(std::size_t displayOffset, bool trailing) const;
  std::size_t adjacentVisibleByte(std::size_t rawByte, bool left) const;
  EditSnapshot currentEditSnapshot() const;
  void clearEditHistory();
  void resetUndoCoalescing();
  void pushUndoSnapshot(EditCoalesceKind kind);
  void noteTypingEditEnd();
  bool undoEdit();
  bool redoEdit();
  bool restoreFromHistory(std::vector<EditSnapshot>& source, std::vector<EditSnapshot>& target);
  void restoreEditSnapshot(const EditSnapshot& snapshot);
  std::size_t xToByteOffset(float localX) const;
  float stopXForByte(std::size_t bytePos) const;
  std::size_t visuallyAdjacentByte(std::size_t bytePos, bool left, bool wordStart) const;
  bool valueLaidOutRtl() const noexcept;
  bool labelSlicingEnabled() const noexcept;
  float restingScrollOffset() const;
  void syncPasswordGlyphNodes(std::size_t count);
  void animatePasswordGlyphIn(GlyphNode& glyph);

  // Multiline mode: geometry comes from the wrapped cursor-stop rects
  // (m_stopRect, parallel to m_stopByte) instead of the 1-D x array.
  bool multilineStopsValid() const noexcept;
  std::size_t stopIndexForByte(std::size_t bytePos) const;
  std::size_t pointToByteOffset(float localX, float localY) const;
  std::size_t lineStartForByte(std::size_t bytePos) const;
  std::size_t lineEndForByte(std::size_t bytePos) const;
  std::size_t byteForVerticalMove(std::size_t from, int lineDelta);
  float contentTextHeight() const noexcept;
  float textViewportHeight() const noexcept;
  float currentLineHeight() const noexcept;
  void ensureCursorVisibleY();
  void clampScrollOffsetY();
  void updateMultilineSelection();
  void syncSelectionLineRects(std::size_t count);
  void updateProjectedSelection();

  void markTextContentChanged();
  void rebuildCursorStops(Renderer& renderer);
  void rebuildCursorStopsFull(Renderer& renderer);
  void recomputeContentLeadSlack(Renderer& renderer, float width, bool showClearButton);
  void updateLabelVisibleSlice(Renderer& renderer);
  void syncLabelScrollPosition();
  std::size_t visibleLabelStartByte() const;
  std::size_t visibleLabelEndByte(float contentWidth, std::size_t startByte) const;

  static std::size_t nextCharPos(const std::string& s, std::size_t pos);
  static std::size_t prevCharPos(const std::string& s, std::size_t pos);

  RectNode* m_background = nullptr;
  Node* m_textViewport = nullptr;
  RectNode* m_selectionRect = nullptr;
  Label* m_label = nullptr;
  RectNode* m_cursor = nullptr;
  TextNode* m_inlineGhost = nullptr;
  InputArea* m_inputArea = nullptr;
  InputArea* m_clearButtonArea = nullptr;
  GlyphNode* m_clearButtonGlyph = nullptr;

  std::string m_value;
  std::string m_placeholder;
  DisplayProjection m_displayProjection;
  std::vector<std::size_t> m_displayStopByte;
  std::vector<TextCursorStop> m_displayCells;
  std::function<DisplayProjection(std::string_view)> m_displayProjector;
  std::function<void(const EditSnapshot&, EditSnapshot&, bool)> m_editRewriter;
  std::size_t m_cursorPos = 0;
  std::size_t m_selectionAnchor = 0;
  std::size_t m_preeditStart = 0;
  std::size_t m_preeditLen = 0;
  std::optional<EditSnapshot> m_preeditBase;
  bool m_preeditCaretHidden = false;
  std::shared_ptr<EditHistory> m_editHistory = std::make_shared<EditHistory>();
  EditCoalesceKind m_lastEditCoalesceKind = EditCoalesceKind::None;
  std::chrono::steady_clock::time_point m_lastUndoRecordTime;
  std::size_t m_typingCoalesceCursorPos = 0;

  std::vector<float> m_stopX;
  std::vector<std::size_t> m_stopByte;
  std::vector<TextCursorStop> m_stopRect;
  // Single-line text only: false when bidi reordering puts some cursor stops out of byte order (any
  // right-to-left run). Multiline and masked text keep it true.
  bool m_stopsInLogicalOrder = true;
  // Per-character screen extents (parallel to m_stopByte minus the last stop), measured only for
  // single-line text whose stops are out of byte order.
  std::vector<TextCharSpan> m_charSpans;
  float m_stopsBuiltForWidth = -1.0F;
  bool m_textMetricsDirty = true;
  bool m_logicalStrutDirty = true;
  float m_logicalStrut = 0.0F;
  float m_contentTextHeight = 0.0F;
  float m_metricsRenderScale = 0.0F;
  std::uint64_t m_metricsGeneration = 0;
  // Hosts must rebind transient measurement views to their surface-owned renderer before input.
  // That stable binding is reused to shape committed geometry after native preedit cancellation.
  Renderer* m_renderer = nullptr;
  float m_projectedLayoutBaseline = 0.0F;
  float m_projectedOriginY = 0.0F;
  float m_cachedLabelY = 0.0F;
  std::string m_labelVisibleSlice;
  std::size_t m_labelVisibleStartByte = 0;
  float m_labelSliceOriginX = 0.0F;
  // Multiline: x of the wrapped label, so its lines land on the cursor stops.
  float m_multilineLabelX = 0.0F;
  std::vector<GlyphNode*> m_passwordGlyphs;
  std::vector<RectNode*> m_selectionLineRects;
  struct SelectionText { Node* clip = nullptr; Label* label = nullptr; };
  std::vector<SelectionText> m_selectionText;
  SelectionText m_caretText;
  std::optional<Color> m_foreground, m_selectionBackground, m_selectionForeground, m_caretColor;
  float m_cursorRenderScale = 1.0F;
  LayoutRect m_caretInsertionRect;
  CaretShape m_caretShape = CaretShape::Line;
  CaretEffect m_caretEffect = CaretEffect::Off;
  float m_caretDuration = 120.0F, m_caretStrength = 0.20F;
  std::vector<TextCursorStop> m_caretCells;
  std::array<RectNode*, input_caret::Trail::count> m_caretStamps{};
  input_caret::Trail m_caretTrail;
  std::uint32_t m_caretAnimation = 0;
  LayoutRect m_caretTrailViewport;
  float m_caretTrailScale = 1.0F, m_caretTrailScrollX = 0.0F, m_caretTrailScrollY = 0.0F;
  void updateClippedText(Renderer& renderer, RectNode* rect, SelectionText& item, Color color);
  void updateTextOverlays(Renderer& renderer);
  float m_scrollOffset = 0.0F;
  float m_scrollOffsetY = 0.0F;
  // Multiline: the caret moved or the text changed since the last layout, so
  // the next layout scrolls it into view. Otherwise layout keeps the offset.
  bool m_revealCursorYPending = false;
  // Sticky caret column for Up/Down runs; negative = unset.
  float m_goalCaretX = -1.0F;
  bool m_cursorBlinkVisible = true;
  Timer m_cursorBlinkTimer;

  std::function<void(const std::string&)> m_onChange;
  std::function<void()> m_onEditingStateChanged;
  std::function<bool(std::string_view, std::string_view, bool)> m_editValidator;
  std::function<void(const std::string&)> m_onSubmit;
  std::function<bool(std::uint32_t, std::uint32_t)> m_onKeyEvent;
  std::function<void(const InputArea::PointerData*)> m_onContextMenu;
  std::function<void()> m_onFocusLoss;
  std::function<void()> m_onFocusGain;
  bool m_submitOnFocusLoss = false;
  bool m_submitOnEnter = false;
  float m_fontSize = Style::fontSizeBody;
  float m_controlHeight = Style::controlHeight;
  float m_horizontalPadding = Style::spaceMd;
  bool m_clearButtonEnabled = false;
  bool m_passwordMode = false;
  bool m_passwordRevealed = false;
  bool m_multiline = false;
  bool m_invalid = false;
  bool m_frameVisible = true;
  bool m_embeddedOnSolidPrimary = false;
  ColorRole m_surfaceRole = ColorRole::SurfaceContainer;
  float m_surfaceOpacity = 1.0F;
  float m_frameRadius = Style::radiusMd;
  bool m_frameRadiusExplicit = false;
  bool m_enabled = true;
  TextInputService* m_textInputService = nullptr;
  float m_minLayoutWidth = 0.0F;
  float m_contentLeadSlack = 0.0F;
  TextAlign m_textAlign = TextAlign::Start;
  std::chrono::steady_clock::time_point m_lastPrimaryPressTime;
  float m_lastPrimaryPressX = 0.0F;
  float m_lastPrimaryPressY = 0.0F;
  bool m_hasLastPrimaryPress = false;
  bool m_primaryPointerSelecting = false;
  // 1 = caret/char drag, 2 = word, 3 = line (resets outside the multi-click window).
  int m_primaryClickCount = 0;
  enum class PointerSelectGranularity : std::uint8_t { Character, Word, Line };
  PointerSelectGranularity m_pointerSelectGranularity = PointerSelectGranularity::Character;
  // Bounds of the unit selected on the multi-click that started the drag; motion
  // expands the selection to include the unit under the pointer.
  std::size_t m_pointerSelectPivotStart = 0;
  std::size_t m_pointerSelectPivotEnd = 0;
  Signal<>::ScopedConnection m_paletteConn;
  Signal<>::ScopedConnection m_inputBordersConn;

  // Detects synchronous self-destruction from user callbacks (onSubmit/onKeyEvent
  // can close a dialog that owns this Input). handleKey takes a weak_ptr to it and
  // bails before touching members if the callback tore us down.
  std::shared_ptr<int> m_aliveToken = std::make_shared<int>(0);
};
