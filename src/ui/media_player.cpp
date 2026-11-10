#include "media_player.hpp"
#include "../preview.hpp"

#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/stream_cb.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace blast {
namespace {
constexpr char assetUri[] = "blastasset://owned";
constexpr std::int64_t maxPixels = 3840LL * 2160;
constexpr std::size_t frameBytes = 960 * 540 * 4;
} // namespace

struct MediaPlayer::Impl {
    explicit Impl(AudioOutput output) : audioOutput(output) {}
    const AudioOutput audioOutput;
    mpv_handle *mpv = nullptr;
    std::shared_ptr<const PreviewAsset> asset;
    int fd = -1;
    std::int64_t bytes = 0, offset = 0;
    std::atomic<bool> opened{false};
    bool ready = false, paused = true, video = false;
    bool hasFrame = false;
    std::uint64_t revision = 0;
    double duration = 0, position = 0, limit = 0, volume = 100;
    std::string error;
    alignas(64) std::array<std::uint8_t, frameBytes> buffers[2]{};
    std::uint8_t *front = buffers[0].data(), *back = buffers[1].data();
    bool fresh = false;
    std::atomic<bool> update{false};
    std::thread renderer;
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false, shutdown = false;
    int renderResult = 0;

    static int open(void *opaque, char *uri, mpv_stream_cb_info *info) {
        auto &self = *static_cast<Impl *>(opaque);
        // One fixed capability, one open: never resolve a filename or URL from media.
        if (std::strcmp(uri, assetUri) || self.opened.exchange(true))
            return MPV_ERROR_LOADING_FAILED;
        *info = {};
        info->cookie = &self;
        info->read_fn = [](void *cookie, char *buffer, std::uint64_t size) -> std::int64_t {
            auto &s = *static_cast<Impl *>(cookie);
            auto count = std::min<std::uint64_t>(size, s.bytes - s.offset);
            count = std::min<std::uint64_t>(count, std::numeric_limits<ssize_t>::max());
            ssize_t n;
            do {
                n = ::pread(s.fd, buffer, count, s.offset);
            } while (n < 0 && errno == EINTR);
            if (n > 0)
                s.offset += n;
            return n;
        };
        info->seek_fn = [](void *cookie, std::int64_t offset) -> std::int64_t {
            auto &s = *static_cast<Impl *>(cookie);
            if (offset < 0 || offset > s.bytes)
                return MPV_ERROR_GENERIC;
            return s.offset = offset;
        };
        info->size_fn = [](void *cookie) -> std::int64_t {
            return static_cast<Impl *>(cookie)->bytes;
        };
        info->close_fn = [](void *) {};
        return 0;
    }

    bool option(const char *name, const char *value) {
        const int result = mpv_set_option_string(mpv, name, value);
        if (result >= 0)
            return true;
        error = std::string("Unavailable required player option: ") + name + ": " +
                mpv_error_string(result);
        return false;
    }

    void renderLoop() {
        mpv_render_context *context = nullptr;
        mpv_render_param create[] = {
            {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_SW)}, {}};
        {
            std::lock_guard lock(mutex);
            renderResult = mpv_render_context_create(&context, mpv, create);
            done = true;
        }
        changed.notify_one();
        if (!context)
            return;
        mpv_render_context_set_update_callback(
            context,
            [](void *opaque) {
                auto &s = *static_cast<Impl *>(opaque);
                s.update.store(true);
                s.changed.notify_one();
            },
            this);
        for (;;) {
            std::unique_lock lock(mutex);
            // Timeout prevents a lost notification: callback must never take our mutex.
            changed.wait_for(lock, std::chrono::milliseconds(10),
                             [&] { return update.load() || shutdown; });
            if (shutdown)
                break;
            if (!update.exchange(false))
                continue;
            if (mpv_render_context_update(context) & MPV_RENDER_UPDATE_FRAME) {
                int size[] = {960, 540};
                int block = 0;
                std::size_t stride = 960 * 4;
                mpv_render_param params[] = {
                    {MPV_RENDER_PARAM_SW_SIZE, size},
                    {MPV_RENDER_PARAM_SW_FORMAT, const_cast<char *>("rgb0")},
                    {MPV_RENDER_PARAM_SW_STRIDE, &stride},
                    {MPV_RENDER_PARAM_SW_POINTER, back},
                    {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
                    {}};
                renderResult = mpv_render_context_render(context, params);
                if (renderResult >= 0) {
                    for (std::size_t i = 3; i < frameBytes; i += 4)
                        back[i] = 255;
                    fresh = true;
                }
            }
        }
        mpv_render_context_set_update_callback(context, nullptr, nullptr);
        mpv_render_context_free(context);
    }

    bool metadata() {
        double seconds = 0;
        const int durationResult = mpv_get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &seconds);
        if (durationResult < 0) {
            error = std::string("Media duration unavailable: ") + mpv_error_string(durationResult);
            return false;
        }
        if (!std::isfinite(seconds) || seconds <= 0 || seconds > limit) {
            error = "Media duration " + std::to_string(seconds) +
                    " seconds is outside allowed range (0, " + std::to_string(limit) + "]";
            return false;
        }
        duration = seconds;
        std::int64_t vid = 0;
        video = mpv_get_property(mpv, "vid", MPV_FORMAT_INT64, &vid) >= 0 && vid > 0;
        if (video) {
            std::int64_t w = 0, h = 0;
            const int widthResult = mpv_get_property(mpv, "width", MPV_FORMAT_INT64, &w);
            const int heightResult = mpv_get_property(mpv, "height", MPV_FORMAT_INT64, &h);
            if (widthResult < 0 || heightResult < 0) {
                error = "Selected video dimensions unavailable (width: " +
                        std::string(mpv_error_string(widthResult)) +
                        ", height: " + mpv_error_string(heightResult) + ")";
                return false;
            }
            if (w <= 0 || h <= 0 || w > 3840 || h > 2160 || w * h > maxPixels) {
                error = "Selected video dimensions " + std::to_string(w) + "x" + std::to_string(h) +
                        " exceed the 3840x2160 limit";
                return false;
            }
        }
        return true;
    }
};

MediaPlayer::MediaPlayer(AudioOutput audioOutput) : impl_(std::make_unique<Impl>(audioOutput)) {}
MediaPlayer::~MediaPlayer() { close(); }

void MediaPlayer::close() {
    auto &s = *impl_;
    if (s.renderer.joinable()) {
        {
            std::lock_guard lock(s.mutex);
            s.shutdown = true;
        }
        s.changed.notify_one();
        s.renderer.join();
    }
    // The custom stream and its owner remain alive through core destruction.
    if (s.mpv)
        mpv_terminate_destroy(s.mpv);
    s.mpv = nullptr;
    if (s.fd >= 0)
        ::close(s.fd);
    s.fd = -1;
    s.asset.reset();
    s.ready = false;
    s.paused = true;
    s.video = s.fresh = false;
    s.hasFrame = false;
    s.position = s.duration = 0;
}

bool MediaPlayer::load(std::shared_ptr<const PreviewAsset> asset, double maxSeconds) {
    close();
    auto &s = *impl_;
    s.error.clear();
    auto fail = [&](const char *message) {
        if (s.error.empty())
            s.error = message;
        close();
        return false;
    };
    if (!asset || !std::isfinite(maxSeconds) || maxSeconds <= 0 || maxSeconds > 7200 ||
        asset->bytes == 0 || asset->bytes > 512ULL * 1024 * 1024)
        return fail("Invalid media asset or duration limit");
    s.fd = ::open(asset->path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat st{};
    if (s.fd < 0 || ::fstat(s.fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid() ||
        (st.st_mode & 0077) || st.st_nlink != 1 || st.st_size <= 0 ||
        static_cast<std::uint64_t>(st.st_size) != asset->bytes)
        return fail("Media asset is not a private owned regular file");
    s.asset = std::move(asset);
    s.bytes = st.st_size;
    s.offset = 0;
    s.limit = maxSeconds;
    s.opened = false;
    s.shutdown = s.done = false;
    s.update = false;
    s.mpv = mpv_create();
    if (!s.mpv)
        return fail("Cannot create media player");
    // Force libavformat, forbid all nested I/O, and admit only self-contained containers.
    // access-references=no installs mpv's block_io_open callback in libavformat.
    const std::pair<const char *, const char *> options[] = {
        {"config", "no"},
        {"load-scripts", "no"},
        {"ytdl", "no"},
        {"input-default-bindings", "no"},
        {"input-vo-keyboard", "no"},
        {"input-terminal", "no"},
        {"terminal", "no"},
        {"osc", "no"},
        {"load-stats-overlay", "no"},
        {"load-console", "no"},
        {"load-commands", "no"},
        {"load-auto-profiles", "no"},
        {"load-select", "no"},
        {"load-context-menu", "no"},
        {"load-positioning", "no"},
        {"access-references", "no"},
        {"autoload-files", "no"},
        {"audio-file-auto", "no"},
        {"sub-auto", "no"},
        {"sid", "no"},
        {"ordered-chapters", "no"},
        {"load-unsafe-playlists", "no"},
        {"demuxer", "lavf"},
        {"demuxer-lavf-o",
         "protocol_whitelist=none,format_whitelist=[mov,mp4,m4a,3gp,3g2,mj2,matroska,webm,ogg,wav,"
         "mp3,flac,aac,avi],enable_drefs=0,use_absolute_path=0"},
        {"demuxer-lavf-probesize", "1048576"},
        {"demuxer-lavf-analyzeduration", "2"},
        {"demuxer-max-bytes", "16MiB"},
        {"demuxer-max-back-bytes", "1MiB"},
        {"cache", "no"},
        {"hwdec", "no"},
        {"vd-lavc-threads", "2"},
        {"vd-lavc-o", "max_pixels=8294400"},
        {"vo", "libmpv"},
        {"sws-fast", "yes"},
        {"osd-level", "0"},
        {"video-timing-offset", "0"},
        {"pause", "yes"},
        {"keep-open", "yes"},
        {"idle", "yes"},
        {"audio-display", "no"},
        {"volume-max", "100"}};
    for (auto [name, value] : options)
        if (!s.option(name, value))
            return fail("Unsafe player configuration");
    if (s.audioOutput == AudioOutput::Null && !s.option("ao", "null"))
        return fail("Cannot configure null audio output");
    const std::string end = std::to_string(maxSeconds);
    if (!s.option("end", end.c_str()) || mpv_initialize(s.mpv) < 0 ||
        mpv_stream_cb_add_ro(s.mpv, "blastasset", &s, Impl::open) < 0)
        return fail("Cannot initialize private media player");
    s.renderer = std::thread([&s] { s.renderLoop(); });
    int initialized;
    {
        std::unique_lock lock(s.mutex);
        s.changed.wait(lock, [&] { return s.done; });
        initialized = s.renderResult;
    }
    if (initialized < 0)
        return fail("Software video output is unavailable");
    volume(s.volume);
    const char *command[] = {"loadfile", assetUri, "replace", nullptr};
    if (mpv_command_async(s.mpv, 1, command) < 0)
        return fail("Cannot load media asset");
    return true;
}

void MediaPlayer::poll() {
    auto &s = *impl_;
    if (!s.mpv)
        return;
    bool failed = false;
    for (;;) {
        const auto *event = mpv_wait_event(s.mpv, 0);
        if (event->event_id == MPV_EVENT_NONE)
            break;
        if (event->event_id == MPV_EVENT_FILE_LOADED) {
            s.ready = s.metadata();
            if (!s.ready) {
                if (s.error.empty())
                    s.error = "Media duration or dimensions are unknown or exceed limits";
                failed = true;
            }
        } else if (event->event_id == MPV_EVENT_END_FILE) {
            const auto *end = static_cast<mpv_event_end_file *>(event->data);
            if (end->reason == MPV_END_FILE_REASON_REDIRECT) {
                s.error = "Media references another resource";
                failed = true;
            } else if (end->reason == MPV_END_FILE_REASON_ERROR) {
                s.error = end->error < 0 ? std::string("Media playback failed: ") +
                                               mpv_error_string(end->error)
                                         : "Media playback failed";
                failed = true;
            }
        } else if (event->event_id == MPV_EVENT_COMMAND_REPLY && event->error < 0) {
            s.error = "Media command failed";
            failed = true;
        } else if (event->event_id == MPV_EVENT_QUEUE_OVERFLOW ||
                   event->event_id == MPV_EVENT_SHUTDOWN) {
            s.error = "Media player stopped unexpectedly";
            failed = true;
        }
    }
    if (!failed && s.ready) {
        if (!s.metadata()) {
            if (s.error.empty())
                s.error = "Media exceeded playback limits";
            failed = true;
        }
        double pos = 0;
        if (mpv_get_property(s.mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos) >= 0) {
            if (!std::isfinite(pos) || pos < 0 || pos > s.limit)
                failed = true;
            else
                s.position = pos;
        }
        int paused = 1;
        if (mpv_get_property(s.mpv, "pause", MPV_FORMAT_FLAG, &paused) >= 0)
            s.paused = paused;
    }
    if (failed) {
        if (s.error.empty())
            s.error = "Media exceeded playback limits";
        close();
        return;
    }
    int result;
    {
        // Never hold this mutex across a client/core API call. The renderer is
        // independently driven by mpv, not by UI progress or synchronous calls.
        std::lock_guard lock(s.mutex);
        if (s.fresh) {
            std::swap(s.front, s.back);
            s.hasFrame = true;
            ++s.revision;
            s.fresh = false;
        }
        result = s.renderResult;
    }
    if (result < 0) {
        s.error = "Cannot render media frame";
        close();
    }
}

void MediaPlayer::playPause() {
    auto &s = *impl_;
    if (!s.mpv || !s.ready)
        return;
    // Revalidate immediately before the only operation that can unpause playback.
    if (!s.metadata()) {
        if (s.error.empty())
            s.error = "Unsafe media metadata";
        close();
        return;
    }
    int pause = s.paused ? 0 : 1;
    if (mpv_set_property(s.mpv, "pause", MPV_FORMAT_FLAG, &pause) >= 0)
        s.paused = pause;
}
void MediaPlayer::seek(double seconds) {
    auto &s = *impl_;
    if (!s.mpv || !s.ready || !std::isfinite(seconds))
        return;
    seconds = std::clamp(seconds, 0.0, std::min(s.duration, s.limit));
    mpv_set_property(s.mpv, "time-pos", MPV_FORMAT_DOUBLE, &seconds);
}
void MediaPlayer::volume(double percent) {
    auto &s = *impl_;
    if (!std::isfinite(percent))
        return;
    s.volume = std::clamp(percent, 0.0, 100.0);
    if (s.mpv)
        mpv_set_property(s.mpv, "volume", MPV_FORMAT_DOUBLE, &s.volume);
}
bool MediaPlayer::loaded() const { return impl_->mpv != nullptr; }
bool MediaPlayer::ready() const { return impl_->ready; }
bool MediaPlayer::paused() const { return impl_->paused; }
bool MediaPlayer::hasVideo() const { return impl_->video; }
double MediaPlayer::position() const { return impl_->position; }
double MediaPlayer::duration() const { return impl_->duration; }
double MediaPlayer::volume() const { return impl_->volume; }
const std::string &MediaPlayer::error() const { return impl_->error; }
std::span<const std::uint8_t> MediaPlayer::frame() const {
    return impl_->hasFrame ? std::span<const std::uint8_t>(impl_->front, frameBytes)
                           : std::span<const std::uint8_t>{};
}
std::uint64_t MediaPlayer::frameRevision() const { return impl_->revision; }
} // namespace blast
