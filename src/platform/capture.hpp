#pragma once

#include <cstdint>
#include <filesystem>
#include <render/scene/custom_draw_node.h>
#include <vector>

namespace blast {
class Window;
// A final overlay reads the actual framebuffer before the SDK swaps it.
class Capture final : public CustomDrawNode {
  public:
    Capture(Window &, bool enabled);
    void draw(RenderBackend &, const CustomDrawContext &) const override;
    void save(const std::filesystem::path &) const;

  private:
    Window &window_;
    bool enabled_;
    mutable std::uint32_t width_ = 0, height_ = 0;
    mutable std::vector<std::uint8_t> pixels_;
};
} // namespace blast
