#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <render/scene/node.h>
#include <span>
#include <string_view>

struct TextFont;

namespace blast::emoji {
inline constexpr std::size_t maxResults = 8;

struct Match {
    std::string_view glyph, shortcode, name;
};

class Catalog {
  public:
    Catalog();
    ~Catalog();
    // The bounded span changes on the next query; its strings live with the catalog.
    std::span<const Match> matches(std::string_view query);
    std::size_t size() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Attach to an unclipped overlay host, not the clipped composer. The Input retains focus.
class CompletionView final : public Node {
  public:
    CompletionView();
    ~CompletionView() override;
    void setMatches(std::span<const Match>, std::size_t selected);
    // Both rectangles use the overlay parent's logical coordinate system.
    void setAnchor(LayoutRect anchor, LayoutRect viewport, float scale, const TextFont &font);
    void setOnChoose(std::function<void(std::size_t)>);
    // Cancel held taps before hiding; call this typed method rather than Node::setVisible.
    void setVisible(bool visible);

  protected:
    void doLayout(Renderer &) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace blast::emoji
