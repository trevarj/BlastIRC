#pragma once

#include "emoji.hpp"
#include <functional>
#include <memory>
#include <render/scene/input_area.h>
#include <string>
#include <string_view>

class Input;
class InputDispatcher;

namespace blast::emoji {
// Full-viewport scene overlay. The controller owns message identity/capability checks
// and return-focus policy in callbacks; this component never touches drafts.
class Picker final : public Node {
  public:
    explicit Picker(Catalog &);
    ~Picker() override;
    void setInputDispatcher(InputDispatcher &);
    // Anchor uses the overlay parent's logical coordinates. An empty anchor centers the panel.
    void show(std::string_view title, LayoutRect anchor, std::function<void(std::string)> choose,
              std::function<void()> cancel);
    // Silent dismissal for controller teardown/source invalidation; no callback is invoked.
    void hide();
    bool active() const;
    void sync(float width, float height, float scale);
    bool handleKey(const InputArea::KeyData &);
    Input &searchInput();

  protected:
    void doLayout(Renderer &) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace blast::emoji
