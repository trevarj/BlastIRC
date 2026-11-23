#pragma once
#include <functional>
#include <memory>
#include <render/scene/node.h>
#include <string>
#include <string_view>

namespace blast {
struct Model;
enum class SidebarMode;
class ModeLine final : public Node {
  public:
    ModeLine(Model &, std::function<void(std::string)> execute, std::function<void(Node &)> retire);
    ~ModeLine() override;
    void sync(float userScale, SidebarMode effectiveNavigation, std::string_view keyHint = {},
              bool allowActivityMenu = true);
    void setContentBottomInset(float inset);

  protected:
    LayoutSize doMeasure(Renderer &, const LayoutConstraints &) override;
    void doLayout(Renderer &) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace blast
