#include "preview.hpp"
#include "ui/media_player.hpp"

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
std::shared_ptr<const blast::PreviewAsset> asset(const std::string &data) {
    char path[] = "/tmp/blast-media-check-XXXXXX";
    const int fd = ::mkstemp(path);
    assert(fd >= 0);
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto count = ::write(fd, data.data() + offset, data.size() - offset);
        assert(count > 0);
        offset += count;
    }
    assert(::close(fd) == 0);
    return std::make_shared<blast::PreviewAsset>(path, data.size());
}
std::string wav() {
    // Two seconds of mono 8kHz signed PCM: no external fixture or codec tool required.
    std::string data;
    auto u16 = [&](unsigned n) {
        data += char(n);
        data += char(n >> 8);
    };
    auto u32 = [&](unsigned n) {
        u16(n);
        u16(n >> 16);
    };
    data += "RIFF";
    u32(32036);
    data += "WAVEfmt ";
    u32(16);
    u16(1);
    u16(1);
    u32(8000);
    u32(16000);
    u16(2);
    u16(16);
    data += "data";
    u32(32000);
    data.resize(data.size() + 32000, '\0');
    return data;
}
void settle(blast::MediaPlayer &player) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (player.loaded() && !player.ready() && std::chrono::steady_clock::now() < deadline) {
        player.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(!player.loaded() || player.ready());
}
int wavFixture(const char *path) {
    // Copy only a bounded regular file; PreviewAsset must never own the caller's path.
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat st{};
    if (fd < 0 || ::fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        st.st_size > 64LL * 1024 * 1024) {
        if (fd >= 0)
            ::close(fd);
        std::fprintf(stderr, "Fixture must be a nonempty regular file of at most 64 MiB\n");
        return 2;
    }
    std::string data(static_cast<std::size_t>(st.st_size), '\0');
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto count = ::read(fd, data.data() + offset, data.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            ::close(fd);
            std::fprintf(stderr, "Cannot read complete fixture\n");
            return 2;
        }
        offset += count;
    }
    ::close(fd);
    auto owned = asset(data);
    blast::MediaPlayer player(blast::MediaPlayer::AudioOutput::Null);
    const bool admitted = player.load(owned, 60);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (admitted && player.loaded() && !player.ready() &&
           std::chrono::steady_clock::now() < deadline) {
        player.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    for (int i = 0; i < 20 && player.ready(); ++i) {
        player.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const bool valid = admitted && player.loaded() && player.ready() && player.paused() &&
                       !player.hasVideo() && std::abs(player.duration() - 12.0) < .1 &&
                       player.position() < .1 && player.error().empty();
    if (!valid) {
        std::fprintf(stderr,
                     "WAV fixture failed: error=%s loaded=%d ready=%d paused=%d "
                     "video=%d duration=%.9g position=%.9g\n",
                     player.error().c_str(), player.loaded(), player.ready(), player.paused(),
                     player.hasVideo(), player.duration(), player.position());
        return 1;
    }
    return 0;
}
} // namespace
int main(int argc, char **argv) {
    if (argc == 3 && std::string(argv[1]) == "--wav-fixture")
        return wavFixture(argv[2]);
    if (argc != 1) {
        std::fprintf(stderr, "Usage: media-player-check [--wav-fixture PATH]\n");
        return 2;
    }
    blast::MediaPlayer player(blast::MediaPlayer::AudioOutput::Null);
    assert(!player.loaded() && !player.ready() && player.paused() && player.frame().empty());
    player.playPause();
    player.seek(1);
    player.close();
    player.close();
    assert(!player.load({}, 10));
    auto audio = asset(wav());
    assert(!player.load(audio, std::numeric_limits<double>::quiet_NaN()));
    assert(!player.load(audio, 0));
    auto wrongSize = asset(wav());
    assert(::truncate(wrongSize->path.c_str(), wrongSize->bytes - 1) == 0);
    assert(!player.load(wrongSize, 10));
    wrongSize.reset();
    auto special = asset("x");
    assert(::unlink(special->path.c_str()) == 0);
    assert(::mkfifo(special->path.c_str(), 0600) == 0);
    assert(!player.load(special, 10)); // must not block opening a FIFO
    special.reset();
    special = asset("x");
    assert(::unlink(special->path.c_str()) == 0);
    assert(::symlink(audio->path.c_str(), special->path.c_str()) == 0);
    assert(!player.load(special, 10));
    special.reset();
    assert(::chmod(audio->path.c_str(), 0644) == 0);
    assert(!player.load(audio, 10));
    assert(::chmod(audio->path.c_str(), 0600) == 0);
    assert(player.load(audio, 10));
    settle(player);
    assert(player.ready() && player.paused() && !player.hasVideo());
    assert(std::abs(player.duration() - 2.0) < .1);
    for (int i = 0; i < 10; ++i)
        player.poll();
    assert(player.paused() && player.position() < .1); // load never autoplays
    player.volume(-20);
    assert(player.volume() == 0);
    player.volume(200);
    assert(player.volume() == 100);
    player.seek(1);
    player.poll();
    assert(player.paused());
    const std::string ownedPath = audio->path;
    audio.reset();
    assert(::access(ownedPath.c_str(), F_OK) == 0); // player holds the asset lease
    player.close();
    assert(::access(ownedPath.c_str(), F_OK) != 0);
    assert(player.frame().empty() && !player.ready());

    audio = asset(wav());
    assert(player.load(audio, 1));
    settle(player);
    assert(!player.loaded() && !player.error().empty()); // known over-limit duration

    const int listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    assert(listener >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    assert(::listen(listener, 8) == 0);
    socklen_t length = sizeof(address);
    assert(::getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length) == 0);
    const std::string url =
        "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/private.wav";
    const std::vector<std::string> attacks = {"#EXTM3U\n" + url + "\n",
                                              "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXTINF:2,\n" +
                                                  url + "\n#EXT-X-ENDLIST\n",
                                              "ffconcat version 1.0\nfile '" + audio->path + "'\n",
                                              "[playlist]\nFile1=" + url + "\nNumberOfEntries=1\n"};
    for (const auto &payload : attacks) {
        auto hostile = asset(payload);
        assert(player.load(hostile, 10));
        settle(player);
        assert(!player.loaded() && !player.ready() && !player.error().empty());
        errno = 0;
        const int connection = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        assert(connection < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        player.close();
    }
    ::close(listener);
    for (int i = 0; i < 3; ++i) {
        assert(player.load(audio, 10));
        player.close(); // in-flight demux/render teardown and immediate reuse
        assert(!player.loaded() && player.frame().empty());
    }
}
