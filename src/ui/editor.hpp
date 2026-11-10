#pragma once

#include "pane.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <render/scene/input_area.h>
#include <string>
#include <string_view>

class InputArea;
class InputDispatcher;
class Node;
class Renderer;
class TextClipboard;

namespace blast {
namespace emoji {
class Catalog;
}
struct Model;
using UiCommand = std::function<void(std::string)>;
using FocusPane = std::function<void(std::uint64_t)>;

// A buffer owns its editing state; each retained composer owns only pane-local controls.
class EditorStore {
  public:
    struct UploadOrigin {
        struct State;
        std::shared_ptr<const State> state;
    };
    enum class UploadInsertResult { Inserted, Changed, Composing, Missing, Invalid };
    using UploadRequested = std::function<void(std::uint64_t, const std::string &, Node &,
                                               std::optional<InputArea::PointerData>)>;
    void setOnUploadRequested(UploadRequested);
    using ImageUploadRequested =
        std::function<void(std::uint64_t, const std::string &, Node &, std::string, std::string)>;
    void setOnImageUploadRequested(ImageUploadRequested);
    std::optional<UploadOrigin> captureUploadOrigin(std::uint64_t paneId);
    bool uploadSourceCurrent(std::uint64_t paneId, const UploadOrigin &, const Node *anchor) const;
    // Input anchors allow any draft/selection; paperclip anchors additionally require emptiness.
    std::string uploadOriginBuffer(const UploadOrigin &) const;
    UploadInsertResult insertUpload(const UploadOrigin &, std::string_view, bool recovery = false);
    // The host synchronously retires only captured/pending presses owned by the affected subtree.
    EditorStore(Model &, TextClipboard &, UiCommand, FocusPane,
                std::function<void(Node &)> retirePointer);
    ~EditorStore();
    std::unique_ptr<Node> composer(PaneState &);
    void setInputDispatcher(InputDispatcher &);
    // Actual edit/caret/preedit publication for a pane; clear before destroying its views.
    void setOnEditingStateChanged(FocusPane);
    // Pane-local transcript readability; absent/stale reveal state keeps fool previews masked.
    void setReplyReadable(std::function<bool(std::uint64_t, const std::string &)>);
    // The pane hosts the reply outside normal flow; detach before destroying that host.
    void setReplyHost(std::uint64_t paneId, Node *);
    float replyContextHeight(std::uint64_t paneId, Renderer &, float innerWidth) const;
    // Already-user-scaled native logical total budget, never output pixels.
    void setComposerHeightLimit(std::uint64_t paneId, float maxHeight);
    // Prefer normal/reply native input height; pending paste may use one native row.
    // maxComposerHeight is the composer-root budget after host history/card reserves.
    float minimumComposerHeight(std::uint64_t paneId, Renderer &, float innerWidth,
                                float maxComposerHeight) const;
    // The host owns the popup; detach it with nullptr before destroying the host.
    void setOverlayHost(Node *);
    // Completion and the reaction picker share one lazily loaded catalog.
    emoji::Catalog &emojiCatalog();
    bool handleCompletionKey(std::uint32_t sym, std::uint32_t modifiers);
    // Run before native traversal; IME text is distinct from disabled editing shortcuts.
    bool handleCompositionKey(std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers,
                              bool preedit, bool handledRepeat);
    // Inner native-logical width for the declared Input / Plus / Send minimums.
    static float minimumComposerWidth(float userScale);
    bool ownsFocus(const InputArea *) const;
    // Native visual traversal across the hosted reply and Input; configured bindings win first.
    bool focusControls(bool backwards);
    bool composing() const;
    bool execute(std::string_view command, PaneState &);
    // Workspace ownership survives temporary modal/Avy keyboard focus.
    // Before Core migrates buffers: an unopened destination still makes this a merge.
    void prepareBufferRename(std::string_view oldId, std::string_view newId);
    void remapBuffer(std::string_view oldId, std::string_view newId, std::uint64_t activePane);
    void sync();
    void flush();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace blast
