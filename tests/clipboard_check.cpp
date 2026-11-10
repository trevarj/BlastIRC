// Compile this translation unit instead of platform/clipboard.cpp for this check: the
// friend access keeps synthetic pipe/offer setup out of the production API.
#include "platform/clipboard.cpp"
#include <cassert>

namespace blast {
class ClipboardTestAccess {
    static void drain() {
        for (auto &callback : DeferredCall::takePending())
            callback();
    }
    static int begin(Clipboard &clipboard, std::size_t cap, std::string mime,
                     Clipboard::PayloadCallback callback) {
        auto &state = *clipboard.state_;
        state.cancelPayload();
        const auto id = ++state.nextPayload;
        state.payloadRequest = std::make_shared<Clipboard::State::PayloadRead>(
            Clipboard::State::PayloadRead{id, false, false, std::move(callback)});
        state.payloadLimit = cap;
        state.payloadMime = std::move(mime);
        state.payloadBytes.clear();
        int fds[2];
        assert(pipe2(fds, O_CLOEXEC) == 0);
        assert(nonblocking(fds[0]));
        state.payloadFd = fds[0];
        return fds[1];
    }
    static void deliver(Clipboard &clipboard, int writer, std::string_view bytes) {
        assert(::write(writer, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
        ::close(writer);
        std::vector<pollfd> fds;
        clipboard.appendPollFds(fds);
        assert(fds.size() == 1);
        fds.front().revents = POLLIN | POLLHUP;
        clipboard.dispatchPoll(fds);
        fds.clear();
        clipboard.appendPollFds(fds);
        assert(fds.empty());
    }

  public:
    static void run() {
        Clipboard::State::Offer offer(nullptr, nullptr);
        offer.offered("image/svg+xml");
        offer.offered("text/plain");
        offer.offered("image/gif");
        offer.offered("image/webp");
        offer.offered("image/jpeg");
        offer.offered("image/png");
        offer.offered("UTF8_STRING");
        offer.offered("text/plain;charset=utf-8");
        offer.offered(nullptr);
        assert(offer.imageMime == "image/png" && offer.mime == textMimes.front());
        Clipboard::State::Offer textOnly(nullptr, nullptr);
        textOnly.offered("image/svg+xml");
        textOnly.offered("text/plain");
        assert(textOnly.imageMime.empty() && textOnly.mime == "text/plain");

        WaylandConnection connection;
        Clipboard clipboard(connection);
        std::vector<Clipboard::Payload> results;
        auto receive = [&](Clipboard::Payload value) { results.push_back(std::move(value)); };
        auto writer = begin(clipboard, 4, "image/png", receive);
        deliver(clipboard, writer, std::string_view("\x89\0\xff\n", 4));
        assert(results.empty());
        drain();
        assert(results.size() == 1 && results.back().status == Clipboard::PayloadStatus::Success);
        assert(results.back().mime == "image/png" && results.back().bytes.size() == 4);
        const auto first = results.back().generation;

        writer = begin(clipboard, 3, "image/png", receive);
        deliver(clipboard, writer, "1234");
        drain();
        assert(results.back().status == Clipboard::PayloadStatus::TooLarge);
        assert(results.back().bytes.empty() && results.back().generation > first);
        writer = begin(clipboard, 4, "text/plain", receive);
        deliver(clipboard, writer, "\xff");
        drain();
        assert(results.back().status == Clipboard::PayloadStatus::InvalidText);
        assert(results.back().bytes.empty());
        writer = begin(clipboard, 4, "UTF8_STRING", receive);
        deliver(clipboard, writer, "\xc3\xa9");
        drain();
        assert(results.back().status == Clipboard::PayloadStatus::Success);
        assert(results.back().bytes == "\xc3\xa9");
        assert(results.back().mime == "text/plain;charset=utf-8");

        writer = begin(clipboard, 4, "text/plain", receive);
        const auto cancelledId = clipboard.state_->payloadRequest->id;
        clipboard.cancelPayload(cancelledId + 1);
        assert(clipboard.state_->payloadFd >= 0);
        clipboard.cancelPayload(cancelledId);
        ::close(writer);
        drain();
        assert(results.back().status == Clipboard::PayloadStatus::Cancelled);
        assert(results.back().generation == cancelledId && results.back().bytes.empty());

        writer = begin(clipboard, 4, "text/plain", receive);
        deliver(clipboard, writer, "safe");
        const auto stale = clipboard.state_->payloadRequest->id;
        const auto unavailable = clipboard.requestPayload(4, receive);
        assert(unavailable > stale);
        drain();
        assert(results[results.size() - 2].status == Clipboard::PayloadStatus::Cancelled);
        assert(results.back().status == Clipboard::PayloadStatus::Unavailable);

        writer = begin(clipboard, 4, "text/plain", receive);
        deliver(clipboard, writer, "safe");
        clipboard.releaseGlobals();
        drain();
        assert(results.back().status == Clipboard::PayloadStatus::Cancelled);
        {
            auto dying = std::make_unique<Clipboard>(connection);
            writer = begin(*dying, 4, "text/plain", receive);
            deliver(*dying, writer, "safe");
        }
        drain();
        assert(results.back().status == Clipboard::PayloadStatus::Cancelled);

        // Local text snapshots use the same bound and queued-cancellation contract.
        clipboard.state_->owned = reinterpret_cast<wl_data_source *>(1);
        clipboard.state_->cache = std::make_shared<const std::string>("local");
        auto local = clipboard.requestPayload(4, receive);
        drain();
        assert(results.back().status == Clipboard::PayloadStatus::TooLarge);
        assert(results.back().generation == local && results.back().bytes.empty());
        local = clipboard.requestPayload(5, receive);
        clipboard.cancelPayload(local);
        drain();
        assert(results.back().status == Clipboard::PayloadStatus::Cancelled);
        clipboard.state_->owned = nullptr;
    }
};
} // namespace blast

int main() { blast::ClipboardTestAccess::run(); }
