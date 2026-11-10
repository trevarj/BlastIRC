#pragma once

#include <core/text_clipboard.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <poll.h>
#include <string>
#include <vector>
#include <wayland/wayland_extension.h>

class WaylandConnection;

namespace blast {
// Ordinary wl_data_device clipboard. WaylandConnection owns this extension.
class Clipboard final : public TextClipboard, public WaylandExtension {
  public:
    explicit Clipboard(WaylandConnection &);
    ~Clipboard() override;
    std::optional<std::string> clipboardText() override;
    void setClipboardText(std::string text) override;
    void requestClipboardText(TextCallback callback) override;
    enum class PayloadStatus { Success, Unavailable, Cancelled, TooLarge, InvalidText, IoError };
    struct Payload {
        PayloadStatus status = PayloadStatus::Unavailable;
        std::string mime;
        std::string bytes;
        std::uint64_t generation = 0;
    };
    using PayloadCallback = std::function<void(Payload)>;
    // One bounded snapshot at a time. Completion is deferred; cancellation also invalidates
    // already queued completion. Images take priority over UTF-8 text in mixed offers.
    std::uint64_t requestPayload(std::size_t maxBytes, PayloadCallback callback);
    void cancelPayload(std::uint64_t request);
    void setInputSerial(std::uint32_t serial);
    void appendPollFds(std::vector<pollfd> &) const;
    void dispatchPoll(const std::vector<pollfd> &);
    void onGlobal(wl_registry *, std::uint32_t, std::string_view, std::uint32_t) override;
    void onGlobalRemove(std::uint32_t) override;
    void releaseGlobals() override;

  private:
    friend class ClipboardTestAccess;
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace blast
