#pragma once

#include "pane.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <render/scene/input_area.h>
#include <string_view>

class InputDispatcher;
class Input;

namespace blast {
struct Model;
enum class SidebarMode;

// The workspace owns all mutations; chrome forwards stable pane IDs and native events.
// Every callback is required. Enqueue structural intents; never destroy a dispatching control.
struct BufferChromeCallbacks {
    std::function<void(std::uint64_t, std::string_view)> command;
    // Hidden direct tools are exposed by the existing menu, bound to this pane.
    std::function<void(std::uint64_t, Node &, bool includeHiddenTools,
                       std::optional<InputArea::PointerData>)>
        paneMenu;
    std::function<void(std::uint64_t, Node &, std::optional<InputArea::PointerData>)> topic;
};

class BufferTitleLine final : public Node {
    // Read-only conversation identity; navigation stays in the sidebar.
  public:
    BufferTitleLine(Model &, BufferChromeCallbacks);
    ~BufferTitleLine() override;
    void sync(const PaneState &, bool focused, float userScale, SidebarMode peopleEffective,
              SidebarMode navigationEffective);
    void setCompact(bool);
    // Responsive tool hiding hands keyboard focus to the surviving More control.
    void setInputDispatcher(InputDispatcher &);

  protected:
    void doLayout(Renderer &) override;
    LayoutSize doMeasure(Renderer &, const LayoutConstraints &) override;
    void doArrange(Renderer &, const LayoutRect &) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Native anchored scene popover; the host owns source validity and return focus.
class TopicPopover final : public Node {
  public:
    TopicPopover();
    ~TopicPopover() override;
    void setInputDispatcher(InputDispatcher &);
    // Anchor uses the overlay parent's coordinates. Text remains raw and selectable.
    void show(std::string_view topic, LayoutRect anchor, std::function<void()> dismiss);
    void hide();
    bool active() const;
    void sync(float width, float height, float scale);
    bool handleKey(const InputArea::KeyData &);
    Input &topicInput();

  protected:
    void doLayout(Renderer &) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace blast
