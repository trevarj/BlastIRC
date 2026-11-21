#pragma once
#include <cstdint>
#include <memory>
#include <render/core/color.h>
#include <render/scene/custom_draw_node.h>
#include <string>
#include <string_view>

namespace blast {
struct MessageSurface;
enum class MaterialKind { Backdrop, Panel, Conversation, Message };

// A retained modern surface. Children remain ordinary native controls and sharp text.
class MaterialNode : public CustomDrawNode {
  public:
    explicit MaterialNode(MaterialKind kind = MaterialKind::Panel);
    ~MaterialNode() override;
    void setMessage(const MessageSurface &);
    void sync();
    void draw(RenderBackend &, const CustomDrawContext &) const override;

  protected:
    void doLayout(Renderer &) override;
    LayoutSize doMeasure(Renderer &, const LayoutConstraints &) override;
    void doArrange(Renderer &, const LayoutRect &) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Native SVG decoding; the Image child owns its renderer-bound texture.
class LucideIcon : public Node {
  public:
    explicit LucideIcon(std::string_view name, float size = 18.f);
    ~LucideIcon() override;
    void setName(std::string_view);
    void setColor(Color);

  protected:
    void doLayout(Renderer &) override;
    LayoutSize doMeasure(Renderer &, const LayoutConstraints &) override;
    void doArrange(Renderer &, const LayoutRect &) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

void setVisualImpulse(float netsplitDistance, float disconnectAmount, float splitAge);
std::uint64_t visualRevision();
// Called by the retained effects overlay before drawing particles.
void drawVisualPostprocess(RenderBackend &, const CustomDrawContext &);
} // namespace blast
