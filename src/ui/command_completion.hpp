#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <render/scene/node.h>
#include <span>
#include <string_view>

struct TextFont;

namespace blast::command_completion {
inline constexpr std::size_t maxResults = 8;

struct Match {
    std::string_view name, description;
    bool literal = false;
};

// Attach to an unclipped overlay host; the composer Input retains native focus.
class View final : public Node {
  public:
    View();
    ~View() override;
    void setMatches(std::span<const Match>, std::size_t selected);
    // Both rectangles use the overlay parent's logical coordinate system.
    void setAnchor(LayoutRect anchor, LayoutRect viewport, float scale, const TextFont &font);
    void setOnChoose(std::function<void(std::size_t)>);
    // Use this typed method so hiding also cancels held pointer/touch presses.
    void setVisible(bool visible);

  protected:
    void doLayout(Renderer &) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace blast::command_completion
