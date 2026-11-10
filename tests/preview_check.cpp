#include "preview.hpp"
#include "preview_fixture.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cassert>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <mutex>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>
#include <webp/encode.h>

using namespace blast;
namespace {
using Bytes = std::vector<std::uint8_t>;
using namespace std::chrono_literals;
using namespace preview_fixture;

PreviewService::Result finish(PreviewService &service, std::uint64_t id,
                              std::uint64_t generation = 73) {
    PreviewService::Result result;
    until([&] {
        auto results = service.poll();
        if (results.empty())
            return false;
        assert(results.size() == 1);
        result = std::move(results.front());
        assert(result.id == id && result.generation == generation);
        return true;
    });
    return result;
}
PreviewService::Request request(std::uint64_t id, std::string url,
                                PreviewKind kind = PreviewKind::Link) {
    PreviewService::Request r;
    r.id = id;
    r.generation = 73;
    r.url = std::move(url);
    r.kind = kind;
    r.allowLocal = true;
    r.maxBytes = 1024 * 1024;
    return r;
}
PreviewService::Result fetch(PreviewService &service, PreviewService::Request r) {
    const auto id = r.id, generation = r.generation;
    assert(service.request(std::move(r)) == PreviewService::Admission::Queued);
    return finish(service, id, generation);
}
void rejected(PreviewService &service, PreviewService::Request r) {
    const auto id = r.id, generation = r.generation;
    const auto admission = service.request(std::move(r));
    assert(admission != PreviewService::Admission::Full);
    if (admission == PreviewService::Admission::Queued) {
        auto result = finish(service, id, generation);
        assert(!result.error.empty() && !result.asset);
    }
}
void big32(Bytes &bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (24 - i * 8));
}
void chunk(Bytes &bytes, std::string_view type, std::size_t size) {
    const auto begin = bytes.size();
    bytes.resize(begin + 12 + size);
    big32(bytes, begin, size);
    std::copy(type.begin(), type.end(), bytes.begin() + begin + 4);
}
// Match avatar_check's container preflight fixtures; native decoders validate pixel streams.
Bytes png(std::uint32_t width, std::uint32_t height) {
    Bytes bytes{137, 80, 78, 71, 13, 10, 26, 10};
    chunk(bytes, "IHDR", 13);
    big32(bytes, 16, width);
    big32(bytes, 20, height);
    bytes[24] = 8;
    bytes[25] = 6;
    chunk(bytes, "IDAT", 1);
    chunk(bytes, "IEND", 0);
    return bytes;
}
Bytes jpeg(std::uint16_t width, std::uint16_t height) {
    return {0xff,
            0xd8,
            0xff,
            0xc0,
            0,
            11,
            8,
            static_cast<std::uint8_t>(height >> 8),
            static_cast<std::uint8_t>(height),
            static_cast<std::uint8_t>(width >> 8),
            static_cast<std::uint8_t>(width),
            1,
            1,
            0x11,
            0,
            0xff,
            0xda,
            0,
            8,
            1,
            1,
            0,
            0,
            63,
            0,
            0,
            0xff,
            0xd9};
}
Bytes webp() {
    constexpr std::array<std::uint8_t, 4> pixel{20, 40, 60, 255};
    std::uint8_t *data = nullptr;
    const auto size = WebPEncodeLosslessRGBA(pixel.data(), 1, 1, 4, &data);
    assert(size && data);
    Bytes bytes(data, data + size);
    WebPFree(data);
    return bytes;
}
// Real encoded 2x1 red images, not the dimension-only preflight fixtures above.
Bytes validPng() {
    return {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
        0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00, 0x00, 0x7b,
        0x40, 0xe8, 0xdd, 0x00, 0x00, 0x00, 0x0f, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xfc,
        0xcf, 0xc0, 0xc0, 0xc0, 0xc0, 0x00, 0x00, 0x06, 0x08, 0x01, 0x01, 0x72, 0x3c, 0xca, 0x32,
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };
}
Bytes validJpeg() {
    return {
        0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00,
        0x01, 0x00, 0x01, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x08, 0x06, 0x06, 0x07, 0x06,
        0x05, 0x08, 0x07, 0x07, 0x07, 0x09, 0x09, 0x08, 0x0a, 0x0c, 0x14, 0x0d, 0x0c, 0x0b, 0x0b,
        0x0c, 0x19, 0x12, 0x13, 0x0f, 0x14, 0x1d, 0x1a, 0x1f, 0x1e, 0x1d, 0x1a, 0x1c, 0x1c, 0x20,
        0x24, 0x2e, 0x27, 0x20, 0x22, 0x2c, 0x23, 0x1c, 0x1c, 0x28, 0x37, 0x29, 0x2c, 0x30, 0x31,
        0x34, 0x34, 0x34, 0x1f, 0x27, 0x39, 0x3d, 0x38, 0x32, 0x3c, 0x2e, 0x33, 0x34, 0x32, 0xff,
        0xdb, 0x00, 0x43, 0x01, 0x09, 0x09, 0x09, 0x0c, 0x0b, 0x0c, 0x18, 0x0d, 0x0d, 0x18, 0x32,
        0x21, 0x1c, 0x21, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32,
        0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32,
        0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32,
        0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0xff, 0xc0, 0x00, 0x11, 0x08, 0x00, 0x01,
        0x00, 0x02, 0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xff, 0xc4, 0x00,
        0x15, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x06, 0xff, 0xc4, 0x00, 0x14, 0x10, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xc4, 0x00,
        0x15, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x06, 0x07, 0xff, 0xc4, 0x00, 0x14, 0x11, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xda, 0x00,
        0x0c, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3f, 0x00, 0x8b, 0x00, 0x65, 0x71,
        0x7f, 0xff, 0xd9,
    };
}
Bytes animatedGif() {
    // Two distinct red/blue frames; consumers decode only the first, cache retains both.
    return {
        0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x02, 0x00, 0x01, 0x00, 0x81, 0x00, 0x00, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0xff, 0x0b,
        0x4e, 0x45, 0x54, 0x53, 0x43, 0x41, 0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x00,
        0x00, 0x00, 0x21, 0xf9, 0x04, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00,
        0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x08, 0x05, 0x00, 0x01, 0x00, 0x08, 0x08, 0x00,
        0x21, 0xf9, 0x04, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x02,
        0x00, 0x01, 0x00, 0x81, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x08, 0x05, 0x00, 0x01, 0x00, 0x08, 0x08, 0x00, 0x3b,
    };
}
Bytes animatedWebp() {
    // SDK webp_decoder_test's valid 16x16 red/blue animation.
    return {
        0x52, 0x49, 0x46, 0x46, 0x84, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56, 0x50,
        0x38, 0x58, 0x0a, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x0f,
        0x00, 0x00, 0x41, 0x4e, 0x49, 0x4d, 0x06, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
        0x00, 0x00, 0x41, 0x4e, 0x4d, 0x46, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x0f, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x64, 0x00, 0x00, 0x02, 0x56, 0x50,
        0x38, 0x4c, 0x0f, 0x00, 0x00, 0x00, 0x2f, 0x0f, 0xc0, 0x03, 0x00, 0x07, 0x10, 0xfd,
        0x8f, 0xfe, 0x07, 0x22, 0xa2, 0xff, 0x01, 0x00, 0x41, 0x4e, 0x4d, 0x46, 0x28, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x0f, 0x00, 0x00,
        0x64, 0x00, 0x00, 0x00, 0x56, 0x50, 0x38, 0x4c, 0x0f, 0x00, 0x00, 0x00, 0x2f, 0x0f,
        0xc0, 0x03, 0x00, 0x07, 0x10, 0xd1, 0xff, 0xfe, 0x07, 0x22, 0xa2, 0xff, 0x01, 0x00,
    };
}
Bytes animatedPng() {
    // Valid 2x1 red/blue APNG, including CRCs and both compressed frame streams.
    return {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
        0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00, 0x00, 0x7b,
        0x40, 0xe8, 0xdd, 0x00, 0x00, 0x00, 0x08, 0x61, 0x63, 0x54, 0x4c, 0x00, 0x00, 0x00, 0x02,
        0x00, 0x00, 0x00, 0x00, 0xf3, 0x8d, 0x93, 0x70, 0x00, 0x00, 0x00, 0x1a, 0x66, 0x63, 0x54,
        0x4c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x0a, 0x00, 0x00, 0xf9, 0x29, 0xb6,
        0x79, 0x00, 0x00, 0x00, 0x0f, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xfc, 0xcf, 0xc0,
        0xc0, 0xc0, 0xc0, 0x00, 0x00, 0x06, 0x08, 0x01, 0x01, 0xcb, 0x47, 0x76, 0x75, 0x00, 0x00,
        0x00, 0x1a, 0x66, 0x63, 0x54, 0x4c, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00,
        0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x0a,
        0x00, 0x00, 0x62, 0x5a, 0x5c, 0xad, 0x00, 0x00, 0x00, 0x13, 0x66, 0x64, 0x41, 0x54, 0x00,
        0x00, 0x00, 0x02, 0x78, 0x9c, 0x63, 0x64, 0x60, 0xf8, 0xcf, 0xc0, 0xc0, 0x00, 0x00, 0x04,
        0x0a, 0x01, 0x01, 0x01, 0x92, 0x82, 0xbd, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44,
        0xae, 0x42, 0x60, 0x82,
    };
}
std::string binary(const Bytes &bytes) {
    return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}
void successfulResponses(Certificate &cert) {
    const std::string html =
        "<html><head><title>Fallback</title>"
        "<meta property='og:title' content='TLS &amp; metadata'/>"
        "<meta name='description' content='Exact description'/>"
        "<meta property='og:image' content='/cover.png'/></head><body></body></html>";
    struct Case {
        std::string path, type, body;
        PreviewKind kind;
        std::uint32_t width = 0, height = 0;
        std::string extension{};
    };
    const std::vector<Case> cases{
        {"/html", "text/html; charset=UTF-8", html, PreviewKind::Link},
        {"/xhtml", "application/xhtml+xml", html, PreviewKind::Link},
        {"/png", "image/png", binary(validPng()), PreviewKind::Image, 2, 1, ".png"},
        {"/jpeg", "image/jpeg", binary(validJpeg()), PreviewKind::Image, 2, 1, ".jpg"},
        {"/webp", "image/webp", binary(webp()), PreviewKind::Image, 1, 1, ".webp"},
        {"/gif", "image/gif", binary(animatedGif()), PreviewKind::Image, 2, 1, ".gif"},
        {"/audio", "audio/ogg", std::string("OggS\0audio\xff", 11), PreviewKind::Audio},
        {"/video", "video/mp4",
         std::string("\0\0\0\x18"
                     "ftypmp42\0video\xff",
                     19),
         PreviewKind::Video},
    };
    Server server(cert, [&](std::string_view path) {
        for (const auto &test : cases)
            if (path == test.path)
                return Reply{test.body, "Content-Type: " + test.type + "\r\n"};
        assert(false && "unexpected success fixture route");
        return Reply{"", "", 404};
    });
    PreviewService service(cert.caPath);
    std::uint64_t id = 0;
    for (const auto &test : cases) {
        auto result = fetch(service, request(++id, server.endpoint + test.path, test.kind));
        if (!result.error.empty() || result.kind != test.kind)
            std::cerr << "TLS preview case " << test.path << ": error=\"" << result.error
                      << "\", kind=" << static_cast<int>(result.kind)
                      << ", expected=" << static_cast<int>(test.kind) << '\n';
        assert(result.error.empty() && result.kind == test.kind);
        if (test.kind == PreviewKind::Link) {
            assert(!result.asset);
            assert(result.metadata.title == "TLS & metadata");
            assert(result.metadata.description == "Exact description");
            assert(result.metadata.imageUrl == server.endpoint + "/cover.png");
        } else {
            assert(result.asset && result.asset->bytes == test.body.size());
            std::ifstream file(result.asset->path, std::ios::binary);
            assert(file.is_open());
            assert(std::string(std::istreambuf_iterator<char>(file), {}) == test.body);
            if (test.kind == PreviewKind::Image) {
                assert(result.raster.width == test.width && result.raster.height == test.height);
                assert(result.raster.extension == test.extension);
            }
        }
    }
    assert(server.requests == cases.size());
}

void statusFailures(Certificate &cert) {
    const std::array<std::pair<PreviewKind, std::string>, 4> kinds{{
        {PreviewKind::Link, "text/html"},
        {PreviewKind::Image, "image/png"},
        {PreviewKind::Audio, "audio/ogg"},
        {PreviewKind::Video, "video/mp4"},
    }};
    for (int status : {403, 404, 429, 500, 206}) {
        Server server(cert, [&](std::string_view path) {
            const auto index = static_cast<std::size_t>(path.at(1) - '0');
            assert(index < kinds.size());
            const auto type = path.ends_with("/mismatch") ? (index + 1) % kinds.size() : index;
            return Reply{"<title>Must not become metadata or an asset</title>",
                         "Content-Type: " + kinds[type].second + "\r\n", status};
        });
        PreviewService service(cert.caPath);
        std::uint64_t id = 0;
        for (std::size_t i = 0; i < kinds.size(); ++i) {
            for (std::string_view suffix : {"", "/mismatch"}) {
                auto result = fetch(
                    service,
                    request(++id, server.endpoint + "/" + std::to_string(i) + std::string(suffix),
                            kinds[i].first));
                // Status wins over MIME reclassification, including partial-content 206.
                const auto prefix = "HTTP " + std::to_string(status) + ": ";
                assert(result.error.starts_with(prefix) && result.error.size() > prefix.size());
                assert(!result.asset && result.metadata.title.empty());
                assert(service.usage().assets == 0 && service.usage().bytes == 0);
            }
        }
        assert(server.requests == kinds.size() * 2);
    }
}
void parsers() {
    assert(PreviewService::url("https://example.com/a?q=1"));
    assert(PreviewService::url("https://8.8.8.8/a"));
    assert(PreviewService::url("https://[2606:4700:4700::1111]/a"));
    for (std::string_view value :
         {"", "http://example.com/a", "file:///tmp/a", "data:text/html,x",
          "https://user:secret@example.com/", "https://@example.com/", "https://example.com/a b",
          "https://example.com/\r\nCookie:x", "https://example.com\\a", "https://127.0.0.1/",
          "https://127.1/", "https://2130706433/", "https://10.0.0.1/", "https://169.254.169.254/",
          "https://192.168.1.1/", "https://[::1]/", "https://[fc00::1]/"})
        assert(!PreviewService::url(value));
    assert(PreviewService::url("https://127.0.0.1/a", true));
    assert(PreviewService::url("https://localhost/a", true));
    assert(!PreviewService::url("http://localhost/a", true));
    assert(!PreviewService::url("https://user:secret@localhost/a", true));
    for (unsigned char c : {0, 9, 10, 13, 31, 127}) {
        std::string value = "https://example.com/a";
        value.push_back(static_cast<char>(c));
        assert(!PreviewService::url(value));
    }
    assert(PreviewService::classify("text/html; charset=UTF-8") == PreviewKind::Link);
    assert(PreviewService::classify("image/png") == PreviewKind::Image);
    assert(PreviewService::classify("image/jpeg") == PreviewKind::Image);
    assert(PreviewService::classify("image/webp") == PreviewKind::Image);
    assert(PreviewService::classify("image/gif") == PreviewKind::Image);
    assert(PreviewService::classify("application/xhtml+xml") == PreviewKind::Link);
    assert(PreviewService::classify("audio/ogg") == PreviewKind::Audio);
    assert(PreviewService::classify("video/mp4") == PreviewKind::Video);
    assert(PreviewService::classify("application/octet-stream") == PreviewKind::Unsupported);
    assert(PreviewService::classify("image/svg+xml") == PreviewKind::Unsupported);
    assert(PreviewService::classify(" IMAGE/PNG ; charset=binary ") == PreviewKind::Image);
    auto metadata = PreviewService::metadata(
        "<title>Fallback</title><meta property='og:title' content='A &amp; B'>"
        "<meta name='description' content='A &quot;description&quot;'>"
        "<meta property='og:image' content='/cover.webp'>",
        "https://example.com/dir/page");
    assert(metadata.title == "A & B");
    assert(metadata.description == "A \"description\"");
    assert(metadata.imageUrl == "https://example.com/cover.webp");
    assert(PreviewService::metadata("<title>Plain title</title>", "https://example.com/").title ==
           "Plain title");
    assert(PreviewService::metadata("<meta property='og:image' content='http://example.com/x'>",
                                    "https://example.com/")
               .imageUrl.empty());
    assert(PreviewService::metadata("<meta property='og:image' content='https://127.0.0.1/x'>",
                                    "https://example.com/")
               .imageUrl.empty());
    assert(PreviewService::metadata("<meta property='og:image' content='/x'>",
                                    "https://localhost/a", true)
               .imageUrl == "https://localhost/x");
    assert(PreviewService::metadata(std::string(256 * 1024 + 1, 'x'), "https://example.com/")
               .title.empty());
    assert(PreviewService::metadata(
               "<!DOCTYPE html [<!ENTITY leak SYSTEM 'file:///nonexistent-blast-preview-check'>]>"
               "<title>&leak;</title>",
               "https://example.com/")
               .title.empty());
    assert(PreviewService::metadata(
               "<!doctype html><title>Public documentation</title><script>let x=[];</script>",
               "https://example.com/")
               .title == "Public documentation");

    for (const auto &[bytes, extension] : std::vector<std::pair<Bytes, std::string>>{
             {png(640, 480), ".png"}, {jpeg(320, 240), ".jpg"}, {webp(), ".webp"}}) {
        auto result = PreviewService::raster(bytes);
        assert(result && result->extension == extension);
        if (extension == ".webp")
            assert(result->width == 1 && result->height == 1);
        else if (extension == ".png")
            assert(result->width == 640 && result->height == 480);
        else
            assert(result->width == 320 && result->height == 240);
        assert(!PreviewService::raster(std::span(bytes).first(8)));
    }
    assert(PreviewService::raster(png(8192, 1)));
    assert(PreviewService::raster(png(8192, 4096)));
    assert(!PreviewService::raster(png(8192, 4097)));
    assert(!PreviewService::raster(png(8193, 1)));
    assert(!PreviewService::raster(png(1, 8193)));
    assert(!PreviewService::raster(png(8192, 8192)));
    assert(!PreviewService::raster(png(0, 1)));
    assert(!PreviewService::raster(jpeg(8193, 1)));
    assert(!PreviewService::raster(Bytes{}));
    assert(!PreviewService::raster(Bytes{'G', 'I', 'F', '8', '9', 'a'}));
    assert(!PreviewService::raster(Bytes{'<', 's', 'v', 'g', '>'}));
    auto malformed = png(1, 1);
    malformed.push_back(0);
    assert(!PreviewService::raster(malformed));
    malformed = png(1, 1);
    big32(malformed, 33, 0xffffffff);
    assert(!PreviewService::raster(malformed));
    malformed = png(1, 1);
    std::copy_n("acTL", 4, malformed.begin() + 37);
    assert(!PreviewService::raster(malformed));
    malformed = jpeg(1, 1);
    malformed.pop_back();
    assert(!PreviewService::raster(malformed));
    malformed = webp();
    malformed[4] ^= 1;
    assert(!PreviewService::raster(malformed));
    const auto gif = animatedGif();
    auto raster = PreviewService::raster(gif);
    assert(raster && raster->width == 2 && raster->height == 1 && raster->extension == ".gif");
    auto gif87 = gif;
    gif87.resize(70); // First frame through its data terminator.
    gif87.push_back(0x3b);
    gif87.erase(gif87.begin() + 25, gif87.begin() + 52); // Remove GIF89a extensions.
    gif87[4] = '7';
    raster = PreviewService::raster(gif87);
    assert(raster && raster->width == 2 && raster->height == 1 && raster->extension == ".gif");
    malformed = gif;
    malformed.pop_back(); // Missing trailer.
    assert(!PreviewService::raster(malformed));
    malformed = gif;
    malformed.push_back(0); // Trailing data.
    assert(!PreviewService::raster(malformed));
    malformed = gif;
    malformed[10] = 0x87; // Global color table extends beyond the container.
    assert(!PreviewService::raster(malformed));
    malformed = gif;
    malformed[63] = 0xff; // Image data sub-block extends beyond the container.
    assert(!PreviewService::raster(malformed));
    malformed = gif;
    malformed[57] = 3; // First frame extends outside the 2x1 logical canvas.
    assert(!PreviewService::raster(malformed));
    malformed = gif;
    malformed[79] = 2; // Second frame's left edge is outside the canvas.
    assert(!PreviewService::raster(malformed));
    malformed = gif;
    malformed[6] = 1;
    malformed[7] = 0x20; // Canvas width 8193.
    assert(!PreviewService::raster(malformed));
    malformed = gif;
    malformed[6] = 0;
    assert(!PreviewService::raster(malformed));
    malformed = gif;
    malformed[6] = 0;
    malformed[7] = 0x20;
    malformed[8] = 0;
    malformed[9] = 0x20; // 8192x8192 exceeds the bounded pixel count.
    assert(!PreviewService::raster(malformed));
}

void transport(Certificate &cert, Certificate &wrongHost, const std::string &bundle) {
    Server server(cert, [](std::string_view path) {
        if (path == "/start")
            return Reply{"", "Location: /page\r\n", 302};
        if (path == "/loop")
            return Reply{"", "Location: /loop\r\n", 302};
        if (path == "/http")
            return Reply{"", "Location: http://localhost/forbidden\r\n", 302};
        if (path == "/credentials")
            return Reply{"", "Location: https://name:secret@localhost/forbidden\r\n", 302};
        return Reply{"<title>Private TLS fixture</title>"};
    });
    PreviewService service(bundle);
    auto r = request(1, server.endpoint + "/page");
    r.allowLocal = false;
    rejected(service, r);
    assert(server.connections == 0 && server.requests == 0);
    r = request(2, server.endpoint + "/page");
    r.url.replace(r.url.find("localhost"), 9, "127.0.0.1");
    r.allowLocal = false;
    rejected(service, r);
    assert(server.connections == 0);
    auto result = fetch(service, request(3, server.endpoint + "/page"));
    assert(result.error.empty() && result.kind == PreviewKind::Link);
    assert(result.metadata.title == "Private TLS fixture" && !result.asset);
    assert(server.requests == 1);
    result = fetch(service, request(4, server.endpoint + "/start"));
    assert(result.error.empty() && result.metadata.title == "Private TLS fixture");
    assert(server.requests == 3); // Redirect was a separate, manually admitted request.
    for (const auto &[id, path] :
         std::vector<std::pair<unsigned, std::string>>{{5, "/http"}, {6, "/credentials"}}) {
        result = fetch(service, request(id, server.endpoint + path));
        assert(!result.error.empty() && !result.asset);
    }
    const auto beforeLoop = server.requests.load();
    result = fetch(service, request(7, server.endpoint + "/loop"));
    assert(!result.error.empty());
    assert(server.requests - beforeLoop == 5); // Initial request plus at most four redirects.
    {
        std::lock_guard lock(server.mutex);
        for (const auto &wire : server.wires) {
            assert(wire.starts_with("GET /"));
            std::string lower = wire;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            for (std::string_view secret : {"authorization:", "cookie:", "referer:"})
                assert(lower.find(secret) == lower.npos);
        }
    }
    // The CA is trusted but the leaf name does not match localhost.
    Server badName(wrongHost,
                   [](std::string_view) { return Reply{"<title>must not load</title>"}; });
    result = fetch(service, request(8, badName.endpoint + "/page"));
    assert(!result.error.empty() && result.metadata.title.empty());
    assert(badName.requests == 0);
}

void mediaAndCaps(Certificate &cert, const Bytes &image) {
    Server server(cert, [&](std::string_view path) {
        if (path == "/html-big")
            return Reply{std::string(256 * 1024 + 1, 'x')};
        if (path == "/stream")
            return Reply{std::string(4096, 'x'), "Content-Type: audio/ogg\r\n", 200, false, true};
        if (path == "/bad-raster")
            return Reply{"<svg>not raster</svg>", "Content-Type: image/png\r\n"};
        if (path == "/mime-mismatch")
            return Reply{binary(image), "Content-Type: image/png\r\n"};
        if (path == "/animated-webp")
            return Reply{binary(animatedWebp()), "Content-Type: image/webp\r\n"};
        if (path == "/animated-png")
            return Reply{binary(animatedPng()), "Content-Type: image/png\r\n"};
        return Reply{binary(image), "Content-Type: image/webp\r\n"};
    });
    PreviewService service(cert.caPath);
    auto result = fetch(service, request(1, server.endpoint + "/image", PreviewKind::Image));
    assert(result.error.empty() && result.asset && result.kind == PreviewKind::Image);
    assert(result.raster.width == 1 && result.raster.height == 1);
    assert(result.asset->bytes == image.size());
    std::ifstream file(result.asset->path, std::ios::binary);
    assert(std::string(std::istreambuf_iterator<char>(file), {}) == binary(image));
    struct stat info{};
    assert(stat(result.asset->path.c_str(), &info) == 0 && (info.st_mode & 077) == 0);
    assert(stat(std::filesystem::path(result.asset->path).parent_path().c_str(), &info) == 0 &&
           (info.st_mode & 077) == 0);
    auto small = request(2, server.endpoint + "/image", PreviewKind::Image);
    small.maxBytes = image.size() - 1;
    auto failed = fetch(service, small);
    assert(!failed.error.empty() && !failed.asset);
    small = request(3, server.endpoint + "/stream", PreviewKind::Audio);
    small.maxBytes = 128;
    failed = fetch(service, small);
    assert(!failed.error.empty() && !failed.asset);
    failed = fetch(service, request(4, server.endpoint + "/html-big"));
    assert(!failed.error.empty() && !failed.asset);
    failed = fetch(service, request(5, server.endpoint + "/bad-raster", PreviewKind::Image));
    assert(!failed.error.empty() && !failed.asset);
    small = request(6, server.endpoint + "/image", PreviewKind::Image);
    small.maxBytes = 512ULL * 1024 * 1024 + 1;
    const auto before = server.requests.load();
    rejected(service, small);
    assert(server.requests == before);
    failed = fetch(service, request(7, server.endpoint + "/mime-mismatch", PreviewKind::Image));
    assert(!failed.error.empty() && !failed.asset);
    std::uint64_t id = 7;
    for (std::string_view path : {"/animated-webp", "/animated-png"}) {
        failed =
            fetch(service, request(++id, server.endpoint + std::string(path), PreviewKind::Image));
        assert(!failed.asset && failed.error.find("unsupported") != std::string::npos);
        assert(failed.error.find("animated PNG/WebP are unsupported") != std::string::npos);
    }
}

void headersOnly(Certificate &cert, PreviewKind wanted, const std::string &type,
                 PreviewKind expected) {
    Server server(cert, [&](std::string_view) {
        return Reply{std::string(1024, 'x'), "Content-Type: " + type + "\r\n", 200, true};
    });
    PreviewService service(cert.caPath);
    auto result = fetch(service, request(1, server.endpoint + "/headers", wanted));
    // The body remains blocked until fixture destruction, so completion proves headers suffice.
    assert(server.requests == 1 && !server.releaseBody);
    assert(!result.asset && result.kind == expected);
    assert(result.error.empty() == (expected != PreviewKind::Unsupported));
}

void cancellation(Certificate &cert) {
    Server server(cert, [](std::string_view path) {
        return Reply{"<title>fresh</title>", "Content-Type: text/html\r\n", 200, path == "/stall"};
    });
    PreviewService service(cert.caPath);
    assert(service.request(request(10, server.endpoint + "/stall")) ==
           PreviewService::Admission::Queued);
    until([&] { return server.requests == 1; });
    service.cancel(10);
    server.releaseBody = true;
    auto next = request(11, server.endpoint + "/fresh");
    next.generation = 999;
    auto result = fetch(service, next);
    assert(result.error.empty() && result.metadata.title == "fresh");
    assert(service.poll().empty());
    rejected(service, request(10, server.endpoint + "/old"));
    rejected(service, request(11, server.endpoint + "/duplicate"));
    const auto before = server.requests.load();
    // Cancel an already completed but unpolled result as well as an active request.
    server.releaseBody = false;
    assert(service.request(request(12, server.endpoint + "/fresh")) ==
           PreviewService::Admission::Queued);
    assert(service.request(request(13, server.endpoint + "/stall")) ==
           PreviewService::Admission::Queued);
    until([&] { return server.requests >= before + 2; });
    // Starting 13 proves 12 is completed but unpolled.
    service.cancel(12);
    assert(service.request(request(14, server.endpoint + "/queued")) ==
           PreviewService::Admission::Queued);
    service.cancel(14);
    service.cancelAll();
    server.releaseBody = true;
    result = fetch(service, request(15, server.endpoint + "/fresh"));
    assert(result.error.empty() && service.poll().empty());
    assert(server.requests == before + 3); // The queued request never reached the listener.
}

void pendingUsage(Certificate &cert, const Bytes &image) {
    Server server(cert, [&](std::string_view path) {
        return Reply{binary(image),
                     path == "/unsupported" ? "Content-Type: text/plain\r\n"
                                            : "Content-Type: image/webp\r\n",
                     200, path == "/hold"};
    });
    PreviewService service(cert.caPath);
    assert(service.usage().pendingAssets == 0);
    assert(service.request(request(1, server.endpoint + "/hold", PreviewKind::Image)) ==
           PreviewService::Admission::Queued);
    until([&] { return server.requests == 1; });
    assert(service.request(request(2, server.endpoint + "/queued", PreviewKind::Image)) ==
           PreviewService::Admission::Queued);
    assert(service.request(request(3, server.endpoint + "/link", PreviewKind::Link)) ==
           PreviewService::Admission::Queued);
    auto usage = service.usage();
    assert(usage.assets == 0 && usage.pendingAssets == 2);
    service.cancel(2);
    service.cancel(3);
    assert(service.usage().pendingAssets == 1);
    server.releaseBody = true;
    // Observe the completed but unpolled asset: one request must never occupy both counts.
    until([&] {
        const auto current = service.usage();
        assert(current.assets + current.pendingAssets == 1);
        return current.assets == 1;
    });
    auto result = finish(service, 1);
    assert(result.asset && service.usage().pendingAssets == 0);
    result.asset.reset();
    assert(service.usage().assets == 0 && service.usage().bytes == 0);
    result = fetch(service, request(4, server.endpoint + "/unsupported", PreviewKind::Image));
    assert(!result.error.empty() && service.usage().pendingAssets == 0);
    server.releaseBody = false;
    assert(service.request(request(5, server.endpoint + "/hold", PreviewKind::Image)) ==
           PreviewService::Admission::Queued);
    until([&] { return server.requests == 3; });
    assert(service.request(request(6, server.endpoint + "/queued", PreviewKind::Image)) ==
           PreviewService::Admission::Queued);
    assert(service.usage().pendingAssets == 2);
    service.cancelAll();
    server.releaseBody = true;
    until([&] { return service.usage().pendingAssets == 0; });
    result = fetch(service, request(7, server.endpoint + "/fresh", PreviewKind::Image));
    assert(result.asset && service.usage().assets == 1 && service.usage().pendingAssets == 0);
    assert(server.requests == 4 && service.poll().empty());
}

void cacheLifetime(Certificate &cert, const Bytes &image) {
    Server server(cert, [&](std::string_view) {
        return Reply{binary(image), "Content-Type: image/webp\r\n"};
    });
    std::shared_ptr<const PreviewAsset> survivor;
    std::string survivorPath;
    {
        PreviewService service(cert.caPath);
        assert(service.usage().assets == 0 && service.usage().bytes == 0);
        std::vector<std::shared_ptr<const PreviewAsset>> owners;
        for (unsigned id = 1; id <= PreviewService::maxAssets; ++id) {
            auto result = fetch(service, request(id, server.endpoint + "/" + std::to_string(id),
                                                 PreviewKind::Image));
            assert(result.error.empty() && result.asset);
            owners.push_back(result.asset);
            const auto usage = service.usage();
            assert(usage.assets == id && usage.bytes == image.size() * id);
        }
        auto over = request(17, server.endpoint + "/over", PreviewKind::Image);
        const auto admission = service.request(over);
        assert(admission != PreviewService::Admission::Invalid);
        if (admission == PreviewService::Admission::Queued) {
            auto result = finish(service, over.id);
            assert(!result.error.empty() && !result.asset);
        }
        const auto removed = owners.front()->path;
        owners.erase(owners.begin());
        assert(!std::filesystem::exists(removed));
        assert(service.usage().assets == PreviewService::maxAssets - 1);
        assert(service.usage().bytes == image.size() * (PreviewService::maxAssets - 1));
        auto result =
            fetch(service, request(18, server.endpoint + "/released", PreviewKind::Image));
        assert(result.error.empty() && result.asset);
        survivor = result.asset;
        survivorPath = survivor->path;
    }
    assert(std::filesystem::exists(survivorPath));
    auto secondOwner = survivor;
    survivor.reset();
    assert(std::filesystem::exists(survivorPath));
    secondOwner.reset();
    assert(!std::filesystem::exists(survivorPath));
}
} // namespace

int main() {
    signal(SIGPIPE, SIG_IGN);
    parsers();
    TempDirectory directory;
    Certificate cert(directory.path), wrongHost(directory.path, "wrong.invalid");
    // Normal TLS trust only: no insecure flags, custom resolver, or service test API.
    const std::string bundle = directory.path + "/bundle.pem";
    {
        std::ofstream out(bundle, std::ios::binary);
        out << std::ifstream(cert.caPath).rdbuf() << std::ifstream(wrongHost.caPath).rdbuf();
        assert(out.good());
    }
    transport(cert, wrongHost, bundle);
    {
        TempDirectory untrustedDirectory;
        Certificate untrusted(untrustedDirectory.path);
        Server server(untrusted, [](std::string_view) { return Reply{"never read"}; });
        PreviewService service(cert.caPath);
        auto result = fetch(service, request(1, server.endpoint + "/untrusted"));
        assert(!result.error.empty() && server.requests == 0);
    }
    const auto image = webp();
    mediaAndCaps(cert, image);
    successfulResponses(cert);
    statusFailures(cert);
    headersOnly(cert, PreviewKind::Link, "image/webp", PreviewKind::Image);
    headersOnly(cert, PreviewKind::Link, "audio/ogg", PreviewKind::Audio);
    headersOnly(cert, PreviewKind::Link, "video/mp4", PreviewKind::Video);
    headersOnly(cert, PreviewKind::Link, "application/octet-stream", PreviewKind::Unsupported);
    cancellation(cert);
    cacheLifetime(cert, image);
    pendingUsage(cert, image);
    std::cout << "preview checks passed\n";
}
