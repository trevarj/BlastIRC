#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <render/scene/input_area.h>
#include <render/scene/node.h>
#include <span>
#include <string>
#include <string_view>

namespace blast {
enum class SidebarMode;
struct Model;
struct SidebarMenuInvocation {
    std::string command;
    std::optional<InputArea::PointerData> pointer;
};
// Visible server-avatar footprint in rail-local coordinates, shared by the title logo.
LayoutRect serverAvatarBox(float railWidth, float scale);
class ServerRail final : public Node {
  public:
    ServerRail(Model &, std::function<void(std::string)> execute,
               std::function<void(Node &, InputArea *)> retire);
    ~ServerRail() override;
    void sync(std::string_view selected, float width, float height, float scale);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
struct SidebarHintRect {
    std::string_view buffer;
    float x = 0, y = 0, width = 0, height = 0;
};
class Sidebar final : public Node {
  public:
    Sidebar(Model &, std::function<void(std::string)> execute);
    ~Sidebar() override;
    void sync(std::string_view selectedServer, std::string_view focusedBuffer, float width,
              float height, float scale, SidebarMode effectiveMode);
    void remapBuffer(std::string_view oldId, std::string_view newId);
    // Passive selected-server hints override the view only, never stored search/filter state.
    void setHintCandidates(std::span<const std::string_view> buffers);
    void clearHintCandidates(bool committed);
    void scrollHintCandidates(float logicalDelta);
    // Returns true only when finishing native focus restoration queues a tracked offset.
    bool finishHintFocusRestore();
    // A later explicit sidebar action supersedes a deferred cancellation restore.
    void cancelHintScrollRestore();
    // Views remain valid until the next sidebar update/remap/query.
    std::span<const SidebarHintRect> hintRects() const;

  protected:
    void doLayout(Renderer &) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class People final : public Node {
  public:
    People(Model &, std::function<void(std::string)> execute,
           std::function<void(SidebarMenuInvocation)> menu = {});
    ~People() override;
    void sync(std::string_view focusedBuffer, float width, float height, float scale,
              SidebarMode effectiveMode);
    void remapBuffer(std::string_view oldId, std::string_view newId);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace blast
