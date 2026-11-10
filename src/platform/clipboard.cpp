#include "platform/clipboard.hpp"
#include <core/deferred_call.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <glib.h>
#include <map>
#include <unistd.h>
#include <utility>
#include <wayland-client.h>
#include <wayland/wayland_connection.h>

namespace blast {
namespace {
constexpr std::array<const char *, 3> textMimes = {"text/plain;charset=utf-8", "UTF8_STRING",
                                                   "text/plain"};
// The linked native codec pack supports all four explicit raster types.
constexpr std::array<const char *, 4> imageMimes = {"image/png", "image/jpeg", "image/webp",
                                                    "image/gif"};
bool utf8(const std::string &text) {
    return g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr);
}
bool nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
           fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}
} // namespace

struct Clipboard::State {
    struct Offer {
        State *owner;
        wl_data_offer *object;
        std::string mime;
        int priority = 100;
        std::string imageMime;
        int imagePriority = 100;
        void offered(const char *type) {
            if (!type)
                return;
            for (std::size_t i = 0; i < textMimes.size(); ++i)
                if (std::string_view(type) == textMimes[i] && static_cast<int>(i) < priority) {
                    priority = static_cast<int>(i);
                    mime = type;
                }
            for (std::size_t i = 0; i < imageMimes.size(); ++i)
                if (std::string_view(type) == imageMimes[i] &&
                    static_cast<int>(i) < imagePriority) {
                    imagePriority = static_cast<int>(i);
                    imageMime = type;
                }
        }
        Offer(State *owner, wl_data_offer *object) : owner(owner), object(object) {}
        ~Offer() {
            if (object)
                wl_data_offer_destroy(object);
        }
    };
    struct Source {
        State *owner;
        wl_data_source *object;
        std::shared_ptr<const std::string> text;
        Source(State *owner, wl_data_source *object, std::shared_ptr<const std::string> text)
            : owner(owner), object(object), text(std::move(text)) {}
        ~Source() { wl_data_source_destroy(object); }
    };
    struct Write {
        int fd;
        std::shared_ptr<const std::string> text;
        std::size_t offset = 0;
    };
    WaylandConnection &connection;
    wl_data_device_manager *manager = nullptr;
    wl_data_device *device = nullptr;
    wl_seat *seat = nullptr;
    std::uint32_t global = 0, serial = 0;
    std::map<wl_data_offer *, std::unique_ptr<Offer>> offers;
    std::map<wl_data_source *, std::unique_ptr<Source>> sources;
    Offer *selection = nullptr;
    wl_data_source *owned = nullptr;
    std::shared_ptr<const std::string> pendingCopy;
    std::shared_ptr<const std::string> cache;
    int readFd = -1;
    std::string incoming;
    std::vector<TextCallback> requests;
    std::vector<Write> writes;
    struct PayloadRead {
        std::uint64_t id;
        bool cancelled = false;
        bool delivered = false;
        PayloadCallback callback;
    };
    std::shared_ptr<PayloadRead> payloadRequest;
    std::uint64_t nextPayload = 0;
    int payloadFd = -1;
    std::size_t payloadLimit = 0;
    std::string payloadMime, payloadBytes;

    void finishPayload(PayloadStatus status) {
        if (payloadFd >= 0)
            ::close(payloadFd);
        payloadFd = -1;
        if (!payloadRequest || payloadRequest->delivered || !payloadRequest->callback) {
            payloadBytes.clear();
            return;
        }
        Payload result{status, std::move(payloadMime), {}, payloadRequest->id};
        if (result.mime == "UTF8_STRING")
            result.mime = textMimes.front();
        if (status == PayloadStatus::Success)
            result.bytes = std::move(payloadBytes);
        payloadBytes.clear();
        auto request = payloadRequest;
        auto callback = std::move(request->callback);
        DeferredCall::callLater(
            [request, callback = std::move(callback), result = std::move(result)]() mutable {
                request->delivered = true;
                if (request->cancelled) {
                    result.status = PayloadStatus::Cancelled;
                    result.bytes.clear();
                    result.mime.clear();
                }
                callback(std::move(result));
            });
    }
    void cancelPayload() {
        if (payloadRequest) {
            payloadRequest->cancelled = true;
            finishPayload(PayloadStatus::Cancelled);
            payloadRequest.reset();
        }
    }
    void consumePayload(const char *bytes, std::size_t count) {
        if (count > payloadLimit - payloadBytes.size())
            finishPayload(PayloadStatus::TooLarge);
        else
            payloadBytes.append(bytes, count);
    }
    void endPayload() {
        finishPayload(payloadMime.starts_with("image/") || utf8(payloadBytes)
                          ? PayloadStatus::Success
                          : PayloadStatus::InvalidText);
    }

    explicit State(WaylandConnection &value) : connection(value) {}
    ~State() {
        cancelPayload();
        if (readFd >= 0)
            ::close(readFd);
        for (const auto &write : writes)
            ::close(write.fd);
    }
    void finish(std::shared_ptr<const std::string> text) {
        if (readFd >= 0)
            ::close(readFd);
        readFd = -1;
        incoming.clear();
        cache = std::move(text);
        auto callbacks = std::move(requests);
        requests.clear();
        for (auto &callback : callbacks) {
            DeferredCall::callLater([callback = std::move(callback), result = cache]() mutable {
                callback(result ? std::optional(*result) : std::nullopt);
            });
        }
    }
    void receive() {
        if (!selection || selection->mime.empty() || readFd >= 0)
            return;
        int fds[2];
        if (pipe2(fds, O_CLOEXEC) < 0) {
            finish({});
            return;
        }
        if (!nonblocking(fds[0])) {
            ::close(fds[0]);
            ::close(fds[1]);
            finish({});
            return;
        }
        readFd = fds[0];
        incoming.clear();
        wl_data_offer_receive(selection->object, selection->mime.c_str(), fds[1]);
        ::close(fds[1]);
    }
    void publish() {
        if (!pendingCopy || !manager || !device || !serial)
            return;
        wl_data_source *object = wl_data_device_manager_create_data_source(manager);
        if (!object)
            return;
        auto source = std::make_unique<Source>(this, object, pendingCopy);
        static const wl_data_source_listener listener = {
            .target = [](void *, wl_data_source *, const char *) {},
            .send =
                [](void *data, wl_data_source *, const char *mime, int fd) {
                    auto &source = *static_cast<Source *>(data);
                    if (!mime ||
                        std::none_of(textMimes.begin(), textMimes.end(), [mime](const char *type) {
                            return std::string_view(mime) == type;
                        })) {
                        ::close(fd);
                        return;
                    }
                    if (!nonblocking(fd)) {
                        ::close(fd);
                        return;
                    }
                    source.owner->writes.push_back({fd, source.text});
                },
            .cancelled =
                [](void *data, wl_data_source *object) {
                    const auto &source = *static_cast<Source *>(data);
                    auto &state = *source.owner;
                    if (state.owned == object) {
                        state.owned = nullptr;
                        if (state.cache == source.text)
                            state.cache.reset();
                    }
                    state.sources.erase(object);
                },
            .dnd_drop_performed = [](void *, wl_data_source *) {},
            .dnd_finished = [](void *, wl_data_source *) {},
            .action = [](void *, wl_data_source *, std::uint32_t) {},
        };
        wl_data_source_add_listener(object, &listener, source.get());
        for (const auto *mime : textMimes)
            wl_data_source_offer(object, mime);
        sources.emplace(object, std::move(source));
        wl_data_device_set_selection(device, object, serial);
        owned = object;
        cancelPayload();
        finish({});
        cache = std::move(pendingCopy);
    }
    void ensureDevice() {
        if (!manager || !connection.seat() || device)
            return;
        seat = connection.seat();
        device = wl_data_device_manager_get_data_device(manager, seat);
        static const wl_data_device_listener listener = {
            .data_offer =
                [](void *data, wl_data_device *, wl_data_offer *object) {
                    auto &state = *static_cast<State *>(data);
                    auto offer = std::make_unique<Offer>(&state, object);
                    static const wl_data_offer_listener offerListener = {
                        .offer =
                            [](void *data, wl_data_offer *, const char *mime) {
                                auto &offer = *static_cast<Offer *>(data);
                                offer.offered(mime);
                            },
                        .source_actions = [](void *, wl_data_offer *, std::uint32_t) {},
                        .action = [](void *, wl_data_offer *, std::uint32_t) {},
                    };
                    wl_data_offer_add_listener(object, &offerListener, offer.get());
                    state.offers.emplace(object, std::move(offer));
                },
            .enter =
                [](void *data, wl_data_device *, std::uint32_t, wl_surface *, wl_fixed_t,
                   wl_fixed_t, wl_data_offer *object) {
                    auto &state = *static_cast<State *>(data);
                    if (object && (!state.selection || state.selection->object != object))
                        state.offers.erase(object);
                },
            .leave = [](void *, wl_data_device *) {},
            .motion = [](void *, wl_data_device *, std::uint32_t, wl_fixed_t, wl_fixed_t) {},
            .drop = [](void *, wl_data_device *) {},
            .selection =
                [](void *data, wl_data_device *, wl_data_offer *object) {
                    auto &state = *static_cast<State *>(data);
                    state.cancelPayload();
                    state.finish({});
                    auto found = state.offers.find(object);
                    state.selection = found == state.offers.end() ? nullptr : found->second.get();
                    std::erase_if(state.offers,
                                  [&](const auto &entry) { return entry.first != object; });
                    if (state.selection)
                        state.receive();
                },
        };
        wl_data_device_add_listener(device, &listener, this);
        publish();
    }
};

Clipboard::Clipboard(WaylandConnection &connection) : state_(std::make_unique<State>(connection)) {}
Clipboard::~Clipboard() = default;

std::uint64_t Clipboard::requestPayload(std::size_t maxBytes, PayloadCallback callback) {
    auto &state = *state_;
    state.cancelPayload();
    state.ensureDevice();
    const auto id = ++state.nextPayload;
    state.payloadRequest = std::make_shared<State::PayloadRead>(
        State::PayloadRead{id, false, false, std::move(callback)});
    state.payloadLimit = maxBytes;
    state.payloadBytes.clear();
    if (state.owned && state.cache) {
        state.payloadMime = textMimes.front();
        state.consumePayload(state.cache->data(), state.cache->size());
        if (state.payloadRequest->callback)
            state.endPayload();
        return id;
    }
    if (!state.selection || (state.selection->imageMime.empty() && state.selection->mime.empty())) {
        state.finishPayload(PayloadStatus::Unavailable);
        return id;
    }
    state.payloadMime =
        state.selection->imageMime.empty() ? state.selection->mime : state.selection->imageMime;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0) {
        state.finishPayload(PayloadStatus::IoError);
        return id;
    }
    if (!nonblocking(fds[0])) {
        ::close(fds[0]);
        ::close(fds[1]);
        state.finishPayload(PayloadStatus::IoError);
        return id;
    }
    state.payloadFd = fds[0];
    wl_data_offer_receive(state.selection->object, state.payloadMime.c_str(), fds[1]);
    ::close(fds[1]);
    return id;
}

void Clipboard::cancelPayload(std::uint64_t request) {
    if (state_->payloadRequest && state_->payloadRequest->id == request)
        state_->cancelPayload();
}

std::optional<std::string> Clipboard::clipboardText() {
    return state_->cache ? std::optional(*state_->cache) : std::nullopt;
}

void Clipboard::requestClipboardText(TextCallback callback) {
    state_->ensureDevice();
    if (state_->cache) {
        callback(*state_->cache);
    } else if (state_->selection && !state_->selection->mime.empty()) {
        state_->requests.push_back(std::move(callback));
        state_->receive();
    } else {
        callback(std::nullopt);
    }
}

void Clipboard::setClipboardText(std::string text) {
    if (!utf8(text))
        return;
    state_->pendingCopy = std::make_shared<const std::string>(std::move(text));
    state_->ensureDevice();
    state_->publish();
}

void Clipboard::setInputSerial(std::uint32_t serial) {
    if (serial)
        state_->serial = serial;
    state_->ensureDevice();
    state_->publish();
}

void Clipboard::appendPollFds(std::vector<pollfd> &fds) const {
    if (state_->readFd >= 0)
        fds.push_back({state_->readFd, POLLIN, 0});
    if (state_->payloadFd >= 0)
        fds.push_back({state_->payloadFd, POLLIN, 0});
    for (const auto &write : state_->writes)
        fds.push_back({write.fd, POLLOUT, 0});
}

void Clipboard::dispatchPoll(const std::vector<pollfd> &fds) {
    auto &state = *state_;
    const auto ready = [&](int fd) {
        const auto found = std::find_if(fds.begin(), fds.end(),
                                        [fd](const pollfd &entry) { return entry.fd == fd; });
        return found == fds.end() ? short{0} : found->revents;
    };
    if (state.payloadFd >= 0 && ready(state.payloadFd)) {
        std::array<char, 16384> buffer;
        for (;;) {
            const auto count = ::read(state.payloadFd, buffer.data(), buffer.size());
            if (count > 0) {
                state.consumePayload(buffer.data(), static_cast<std::size_t>(count));
                if (state.payloadFd < 0)
                    break;
            } else if (count == 0) {
                state.endPayload();
                break;
            } else if (errno == EINTR) {
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else {
                state.finishPayload(PayloadStatus::IoError);
                break;
            }
        }
    }
    if (state.readFd >= 0 && ready(state.readFd)) {
        std::array<char, 16384> buffer;
        for (;;) {
            const auto count = ::read(state.readFd, buffer.data(), buffer.size());
            if (count > 0) {
                // A clipboard producer is untrusted; reject oversized transfers instead of
                // truncating text.
                if (state.incoming.size() + static_cast<std::size_t>(count) > 64 * 1024 * 1024) {
                    state.finish({});
                    break;
                }
                state.incoming.append(buffer.data(), static_cast<std::size_t>(count));
            } else if (count == 0) {
                auto text = std::move(state.incoming);
                state.finish(utf8(text) ? std::make_shared<const std::string>(std::move(text))
                                        : nullptr);
                break;
            } else if (errno == EINTR) {
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else {
                state.finish({});
                break;
            }
        }
    }
    std::erase_if(state.writes, [&](State::Write &write) {
        if (!ready(write.fd))
            return false;
        while (write.offset < write.text->size()) {
            const auto count = ::write(write.fd, write.text->data() + write.offset,
                                       write.text->size() - write.offset);
            if (count > 0)
                write.offset += static_cast<std::size_t>(count);
            else if (count < 0 && errno == EINTR)
                continue;
            else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return false;
            else
                break;
        }
        ::close(write.fd);
        return true;
    });
}

void Clipboard::onGlobal(wl_registry *registry, std::uint32_t name, std::string_view interface,
                         std::uint32_t version) {
    if (interface == wl_data_device_manager_interface.name && !state_->manager) {
        state_->manager = static_cast<wl_data_device_manager *>(wl_registry_bind(
            registry, name, &wl_data_device_manager_interface, std::min(version, 3u)));
        state_->global = name;
    }
    state_->ensureDevice();
}

void Clipboard::onGlobalRemove(std::uint32_t name) {
    if (state_->global == name)
        releaseGlobals();
}

void Clipboard::releaseGlobals() {
    auto &state = *state_;
    state.cancelPayload();
    state.finish({});
    state.selection = nullptr;
    state.offers.clear();
    state.sources.clear();
    state.owned = nullptr;
    for (const auto &write : state.writes)
        ::close(write.fd);
    state.writes.clear();
    if (state.device) {
        if (wl_data_device_get_version(state.device) >= WL_DATA_DEVICE_RELEASE_SINCE_VERSION)
            wl_data_device_release(state.device);
        else
            wl_data_device_destroy(state.device);
        state.device = nullptr;
    }
    if (state.manager) {
        wl_data_device_manager_destroy(state.manager);
        state.manager = nullptr;
    }
    state.seat = nullptr;
    state.serial = state.global = 0;
}
} // namespace blast
