#pragma once

#include "editor.hpp"
#include "pane.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <render/core/color.h>
#include <render/scene/input_area.h>
#include <render/scene/node.h>
#include <string>
#include <string_view>
#include <vector>

class Renderer;
class InputDispatcher;
class TextClipboard;

namespace blast {
struct Model;
struct Buffer;
struct IrcDocument;
struct NickAnnotation {
    std::size_t begin, end;
    std::string nick;
};
std::vector<NickAnnotation> annotateNicks(const Model &, const Buffer &, const IrcDocument &,
                                          std::string_view author = {});
class MediaPreviewController;
Color nickColor(const Model &, const std::string &nick, const std::string &server);

struct TranscriptMessageTarget {
    std::string buffer;
    std::uint64_t identity = 0;
    std::string nick;
    bool canReply = false, canReact = false;
    // Scene-logical bounds of the current materialized message, if visible.
    std::optional<LayoutRect> anchor;
};

class TranscriptView final : public Node {
  public:
    TranscriptView(Model &, PaneState &, TextClipboard &, UiCommand, FocusPane);
    ~TranscriptView() override;
    void sync();
    // Retain keyboard action focus when native message controls rematerialize.
    void setInputDispatcher(InputDispatcher &);
    void setOnNickMenu(std::function<void(std::string, std::optional<InputArea::PointerData>)>);
    // The host queued native IntoView for a specific message action, not the canvas.
    void noteFocusReveal();
    // A newer explicit pointer/scroll/navigation intent supersedes only our queued reveal.
    void cancelFocusReveal();
    // Focused typing-list navigation precedes global cancel/history bindings.
    bool handleTypingKey(const InputArea::KeyData &);
    bool handleKey(const InputArea::KeyData &);
    // A false navigation result returns keyboard ownership to the composer.
    bool navigateMessage(int delta);
    // Scroll the actual history viewport without transferring composer focus.
    bool scrollHalfPage(int direction);
    bool scrollToBoundary(bool newest);
    // Core has validated and materialized the original history; never changes composer state.
    bool jumpToSourceMessage(std::string_view id);
    std::optional<TranscriptMessageTarget> selectedMessage() const;
    std::optional<TranscriptMessageTarget> resolveMessage(const TranscriptMessageTarget &) const;
    bool replyToMessage(const TranscriptMessageTarget &);
    bool reactToMessage(const TranscriptMessageTarget &, std::string emoji);
    bool copySelection();
    // Pane-local privacy gate for stable message keys; unknown/stale rows fail closed.
    bool messageReadable(const std::string &key) const;
    void setMediaPreviewController(MediaPreviewController *);
    void invalidateMediaPreviews();
    void clearTextSelection();
    // Observe active-buffer arrivals before Core renames or merges its message lists.
    // Data-only: the post-rename remap then treats any imported rows as baseline.
    void beforeBufferRename(std::string_view oldId, std::string_view newId);
    // Core-provided rename/merge, not user navigation: retain this pane's body anchor.
    void remapBuffer(std::string_view oldId, std::string_view newId);
    // The host supplies actual native-window focus AND focused-pane eligibility.
    // Also cancels gestures when the host has hidden this pane or an ancestor.
    void setReadEligible(bool eligible);
    // Focus suppresses the visit separator, independently of whether history is read.
    void setWindowActive(bool active);
    // Actual native composer top-center, in scene-logical coordinates.
    void setComposerOrigin(float sceneX, float sceneY);
    // Only typing reserves content; active reply occlusion affects controls/reveal, not scrolling.
    void setContextInset(float typingInset, float typingRowHeight, float replyOverlayHeight);
    // Pane-owned overlay, detached before its native subtree is retired.
    void setMotionHost(Node *host);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    void doLayout(Renderer &) override;
    LayoutSize doMeasure(Renderer &, const LayoutConstraints &) override;
    void doArrange(Renderer &, const LayoutRect &) override;
};
} // namespace blast
