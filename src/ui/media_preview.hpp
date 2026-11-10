#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <render/scene/input_area.h>
#include <span>
#include <string>
#include <string_view>

class InputDispatcher;
namespace blast {
struct Model;
class MediaPreviewCard;
// UI-thread controller. Destroy before the overlay host; invalidate before buffer/policy
// retirement.
class MediaPreviewController {
  public:
    MediaPreviewController(Model &, Node &overlayHost, std::string caBundle = {});
    ~MediaPreviewController();
    MediaPreviewController(const MediaPreviewController &) = delete;
    MediaPreviewController &operator=(const MediaPreviewController &) = delete;
    void setInputDispatcher(InputDispatcher &);
    void setChanged(std::function<void()>);
    void update(float width, float height, float scale);
    bool openUrl(std::string_view, std::uint64_t messageId = 0);
    bool handleKey(const InputArea::KeyData &);
    bool active() const;
    bool ready(std::uint64_t messageId = 0) const;
    std::string_view error(std::uint64_t messageId = 0) const;
    bool replacesUrl(std::uint64_t messageId) const;
    void close();
    void invalidate();
    void retainMessages(std::span<const std::uint64_t>);
    // No network for unreadable rows. One first URL per stable message identity.
    void bind(std::uint64_t messageId, std::string_view firstUrl, bool readable,
              bool automaticAllowed = true);
    float measure(Renderer &, std::uint64_t messageId, float actualWidth, float scale) const;

  private:
    friend class MediaPreviewCard;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class MediaPreviewCard final : public Node {
  public:
    explicit MediaPreviewCard(MediaPreviewController &);
    ~MediaPreviewCard() override;
    void setOpenLink(std::function<void(std::string, std::uint64_t)>);
    void setReadable(std::function<bool(std::uint64_t)>);
    void bind(std::uint64_t messageId, std::string_view firstUrl, bool readable,
              bool automaticAllowed = true);
    float measure(Renderer &, float actualWidth, float scale) const;
    void sync(Renderer &, float width, float scale);

  protected:
    void doLayout(Renderer &) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace blast
