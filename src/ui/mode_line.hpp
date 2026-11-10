#pragma once
#include <memory>
#include <render/scene/node.h>
#include <string_view>

namespace blast {
struct Model;
enum class SidebarMode;
class ModeLine final : public Node {
  public:
    explicit ModeLine(Model &);
    ~ModeLine() override;
    void sync(float userScale, SidebarMode effectiveNavigation, std::string_view keyHint = {});

  protected:
    LayoutSize doMeasure(Renderer &, const LayoutConstraints &) override;
    void doLayout(Renderer &) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace blast
