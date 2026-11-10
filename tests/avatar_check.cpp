#include "avatar.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <curl/curl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <webp/encode.h>

namespace {
using blast::AvatarLoader;
using Bytes = std::vector<std::uint8_t>;

void big32(Bytes &bytes, std::size_t offset, std::uint32_t value) {
    assert(offset <= bytes.size() && bytes.size() - offset >= 4);
    for (unsigned i = 0; i < 4; ++i)
        bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (24 - 8 * i));
}

void little32(Bytes &bytes, std::size_t offset, std::uint32_t value) {
    assert(offset <= bytes.size() && bytes.size() - offset >= 4);
    for (unsigned i = 0; i < 4; ++i)
        bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (8 * i));
}

void chunk(Bytes &bytes, std::string_view type, std::size_t length) {
    assert(type.size() == 4);
    const auto start = bytes.size();
    bytes.resize(start + 12 + length);
    big32(bytes, start, static_cast<std::uint32_t>(length));
    std::copy(type.begin(), type.end(), bytes.begin() + start + 4);
}

// Header/container fixtures only. CRC and pixel-stream validity belongs to the native decoder.
Bytes png(std::uint32_t width, std::uint32_t height, std::size_t payload = 1) {
    Bytes bytes{137, 80, 78, 71, 13, 10, 26, 10};
    chunk(bytes, "IHDR", 13);
    big32(bytes, 16, width);
    big32(bytes, 20, height);
    bytes[24] = 8;
    bytes[25] = 6;
    chunk(bytes, "IDAT", payload);
    chunk(bytes, "IEND", 0);
    return bytes;
}

Bytes jpeg(std::uint16_t width, std::uint16_t height, std::uint8_t marker = 0xc0) {
    return {0xff,
            0xd8,
            0xff,
            marker,
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
            0, // One frame component.
            0xff,
            0xda,
            0,
            8,
            1,
            1,
            0,
            0,
            63,
            0, // One scan component.
            0,
            0xff,
            0xd9};
}

Bytes webp() {
    constexpr std::array<std::uint8_t, 4> pixel{20, 40, 60, 255};
    std::uint8_t *encoded = nullptr;
    const auto size = WebPEncodeLosslessRGBA(pixel.data(), 1, 1, 4, &encoded);
    assert(size && encoded);
    Bytes bytes(encoded, encoded + size);
    WebPFree(encoded);
    return bytes;
}

void expect(const Bytes &bytes, std::string_view extension) {
    const auto actual = AvatarLoader::rasterExtension(bytes);
    assert(actual && *actual == extension);
}

void urls() {
    assert(AvatarLoader::url("https://example.com/avatar.png") == "https://example.com/avatar.png");
    const auto sized = AvatarLoader::url("HTTPS://example.com/avatar/{size}?width={size}");
    assert(sized && sized->find("/avatar/64?width=64") != sized->npos);
    assert(AvatarLoader::url("https://8.8.8.8/a"));
    assert(AvatarLoader::url("https://[2606:4700:4700::1111]/a"));
    assert(AvatarLoader::url("https://example.com/a?mail=@example.com"));
    for (std::string_view value : {"",
                                   "http://example.com/a",
                                   "file:///tmp/a.png",
                                   "ftp://example.com/a",
                                   "ftps://example.com/a",
                                   "data:image/png;base64,AAAA",
                                   "gopher://example.com/a",
                                   "//example.com/a",
                                   "https:example.com/a",
                                   "https://",
                                   "https:///",
                                   "https://@example.com/a",
                                   "https://:@example.com/a",
                                   "https://user@example.com/a",
                                   "https://user:@example.com/a",
                                   "https://user:password@example.com/a",
                                   "https://user:password;options@example.com/a",
                                   "https://%40@example.com/a",
                                   "https://example.com:65536/a",
                                   "https://example.com\\a",
                                   "https://example.com/a\\b",
                                   " https://example.com/a",
                                   "https://example.com/a b",
                                   "https://example.com/\r\nx:1",
                                   "https://[2606:4700:4700::1111%25eth0]/a",
                                   "https://[::ffff:8.8.8.8]/a",
                                   "https://127.0.0.1/a",
                                   "https://127.1/a",
                                   "https://0177.0.0.1/a",
                                   "https://0x7f000001/a",
                                   "https://2130706433/a",
                                   "https://0x7f.0.0.1/a",
                                   "https://167772161/a",
                                   "https://0300.0250.1.1/a",
                                   "https://%31%32%37.0.0.1/a",
                                   "https://10.0.0.1/a",
                                   "https://169.254.169.254/a",
                                   "https://192.0.2.1/a",
                                   "https://[::1]/a",
                                   "https://[fc00::1]/a",
                                   "https://[2001:db8::1]/a"})
        assert(!AvatarLoader::url(value));
    for (unsigned char control : {0, 1, 9, 10, 13, 31, 127}) {
        std::string value = "https://example.com/a";
        value.push_back(static_cast<char>(control));
        value += 'b';
        assert(!AvatarLoader::url(value));
    }
    std::string boundary = "https://example.com/";
    boundary.resize(2048, 'a');
    assert(AvatarLoader::url(boundary));
    boundary += 'a';
    assert(!AvatarLoader::url(boundary));
}

void addresses() {
    for (std::string_view value :
         {"1.1.1.1", "8.8.8.8", "93.184.216.34", "100.63.255.255", "100.128.0.0", "172.15.255.255",
          "172.32.0.0", "192.0.1.1", "198.17.255.255", "198.20.0.0", "223.255.255.255",
          "2001:4860:4860::8888", "2606:4700:4700::1111", "2001:200::1", "3fff:1000::1"})
        assert(AvatarLoader::publicAddress(value));
    for (std::string_view value : {"",
                                   "localhost",
                                   "1.2.3",
                                   "1.2.3.999",
                                   "127.1",
                                   "0177.0.0.1",
                                   "0x7f000001",
                                   "2130706433",
                                   "0.0.0.0",
                                   "0.255.255.255",
                                   "10.0.0.1",
                                   "10.255.255.255",
                                   "100.64.0.0",
                                   "100.127.255.255",
                                   "127.0.0.1",
                                   "169.254.0.0",
                                   "169.254.255.255",
                                   "172.16.0.0",
                                   "172.31.255.255",
                                   "192.0.0.9",
                                   "192.0.2.1",
                                   "192.88.99.1",
                                   "192.168.0.1",
                                   "198.18.0.0",
                                   "198.19.255.255",
                                   "198.51.100.1",
                                   "203.0.113.1",
                                   "224.0.0.0",
                                   "239.255.255.255",
                                   "240.0.0.0",
                                   "255.255.255.255",
                                   "::",
                                   "::1",
                                   "::8.8.8.8",
                                   "::ffff:8.8.8.8",
                                   "64:ff9b::808:808",
                                   "100::1",
                                   "fc00::1",
                                   "fdff::1",
                                   "fe80::1",
                                   "fec0::1",
                                   "ff02::1",
                                   "2001::1",
                                   "2001:2::1",
                                   "2001:10::1",
                                   "2001:20::1",
                                   "2001:1ff::1",
                                   "2001:db8::1",
                                   "2002:808:808::1",
                                   "3ffe::1",
                                   "3fff::1",
                                   "3fff:fff::1",
                                   "4000::1",
                                   "[2606:4700:4700::1111]",
                                   "fe80::1%lo"})
        assert(!AvatarLoader::publicAddress(value));
    const std::string embedded("8.8.8.8\0evil", 12);
    assert(!AvatarLoader::publicAddress(embedded));
}

void rasters() {
    expect(png(1, 1), ".png");
    expect(png(1024, 1024), ".png");
    for (const auto [width, height] : std::array<std::pair<std::uint32_t, std::uint32_t>, 6>{
             {{0, 1}, {1, 0}, {1025, 1}, {1, 1025}, {0xffffffff, 1}, {1024, 1025}}})
        assert(!AvatarLoader::rasterExtension(png(width, height)));
    auto image = png(1, 1);
    for (std::size_t length = 0; length < image.size(); ++length)
        assert(!AvatarLoader::rasterExtension(std::span<const std::uint8_t>(image).first(length)));
    auto invalid = image;
    big32(invalid, 8, 12);
    assert(!AvatarLoader::rasterExtension(invalid));
    invalid = image;
    big32(invalid, 33, 0xffffffff);
    assert(!AvatarLoader::rasterExtension(invalid));
    invalid = image;
    invalid[24] = 1; // RGBA cannot have one-bit channels.
    assert(!AvatarLoader::rasterExtension(invalid));
    invalid = image;
    invalid[26] = 1; // Unknown compression.
    assert(!AvatarLoader::rasterExtension(invalid));
    for (std::string_view type : {"acTL", "fcTL", "fdAT", "IHDR"}) {
        Bytes extra;
        chunk(extra, type, 8);
        invalid = image;
        invalid.insert(invalid.begin() + 33, extra.begin(), extra.end());
        assert(!AvatarLoader::rasterExtension(invalid));
    }
    image = png(1, 1, AvatarLoader::maxBytes - 57);
    assert(image.size() == AvatarLoader::maxBytes);
    expect(image, ".png");
    image.push_back(0);
    assert(!AvatarLoader::rasterExtension(image));

    for (std::uint8_t marker : {0xc0, 0xc1, 0xc2})
        expect(jpeg(1024, 1024, marker), ".jpg");
    assert(!AvatarLoader::rasterExtension(jpeg(0, 1)));
    assert(!AvatarLoader::rasterExtension(jpeg(1, 0)));
    assert(!AvatarLoader::rasterExtension(jpeg(1025, 1)));
    assert(!AvatarLoader::rasterExtension(jpeg(1, 65535)));
    assert(!AvatarLoader::rasterExtension(jpeg(1, 1, 0xc3)));
    image = jpeg(1, 1);
    for (std::size_t length = 0; length < image.size(); ++length)
        assert(!AvatarLoader::rasterExtension(std::span<const std::uint8_t>(image).first(length)));
    for (std::uint8_t length : {0, 1, 2, 7, 10, 12, 255}) {
        invalid = image;
        invalid[5] = length;
        assert(!AvatarLoader::rasterExtension(invalid));
    }
    invalid = image;
    invalid[6] = 12; // Unsupported precision, not a different SOF layout.
    assert(!AvatarLoader::rasterExtension(invalid));
    invalid = image;
    invalid[11] = 4; // A malformed SOF cannot hide larger dimensions in another marker.
    assert(!AvatarLoader::rasterExtension(invalid));
    invalid = image;
    invalid.insert(invalid.begin() + 15, image.begin() + 2, image.begin() + 15);
    assert(!AvatarLoader::rasterExtension(invalid));
    invalid = image;
    invalid[13] = 0; // Zero sampling factor.
    assert(!AvatarLoader::rasterExtension(invalid));
    invalid = image;
    invalid[18] = 7; // Truncated/inconsistent SOS length.
    assert(!AvatarLoader::rasterExtension(invalid));

    image = webp();
    expect(image, ".webp");
    for (std::size_t length = 0; length < image.size(); ++length)
        assert(!AvatarLoader::rasterExtension(std::span<const std::uint8_t>(image).first(length)));
    // The lossless encoder emits VP8L; mutate only its fixed five-byte feature header.
    assert(image.size() >= 25 &&
           std::string_view(reinterpret_cast<const char *>(image.data() + 12), 4) == "VP8L");
    invalid = image;
    little32(invalid, 21, 1023 | (1023 << 14));
    expect(invalid, ".webp");
    little32(invalid, 21, 1024 | (1023 << 14));
    assert(!AvatarLoader::rasterExtension(invalid));
    little32(invalid, 21, 1023 | (1024 << 14));
    assert(!AvatarLoader::rasterExtension(invalid));
    invalid = image;
    little32(invalid, 16, 0xffffffff);
    assert(!AvatarLoader::rasterExtension(invalid));
    invalid = image;
    invalid.insert(invalid.end(), {'A', 'N', 'I', 'M', 0, 0, 0, 0});
    little32(invalid, 4, static_cast<std::uint32_t>(invalid.size() - 8));
    assert(!AvatarLoader::rasterExtension(invalid));
    invalid = image;
    // Extended WebP animation flag and canvas, followed by the encoded bitstream.
    const Bytes animation{'V', 'P', '8', 'X', 10, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    invalid.insert(invalid.begin() + 12, animation.begin(), animation.end());
    little32(invalid, 4, static_cast<std::uint32_t>(invalid.size() - 8));
    assert(!AvatarLoader::rasterExtension(invalid));
    for (std::string_view unsupported :
         {"<svg xmlns='http://www.w3.org/2000/svg'/>", "GIF89a", "BM12345"})
        assert(!AvatarLoader::rasterExtension(std::span(
            reinterpret_cast<const std::uint8_t *>(unsupported.data()), unsupported.size())));
    assert(!AvatarLoader::rasterExtension(Bytes{0, 0, 1, 0, 1, 0})); // ICO.
}

std::set<std::filesystem::path> cacheDirectories() {
    std::set<std::filesystem::path> directories;
    for (const auto &entry :
         std::filesystem::directory_iterator(std::filesystem::temp_directory_path()))
        if (entry.path().filename().string().starts_with("blastirc-avatars-"))
            directories.insert(entry.path());
    return directories;
}

void capacity() {
    std::size_t outstanding = AvatarLoader::maxQueue;
    std::size_t owned = 0;
    std::size_t reserved = outstanding;
    assert(!AvatarLoader::capacity(outstanding, owned + reserved));
    --outstanding; // Cancelling queued work releases both admission and file reservation.
    --reserved;
    assert(AvatarLoader::capacity(outstanding, owned + reserved));

    outstanding = AvatarLoader::maxQueue;
    owned = outstanding;
    reserved = 0;
    assert(!AvatarLoader::capacity(outstanding, owned));
    --outstanding; // Polling a completion frees queue capacity, but retains its owned file.
    assert(AvatarLoader::capacity(outstanding, owned));

    outstanding = 1;
    owned = AvatarLoader::maxFiles - 1;
    reserved = 1;
    assert(!AvatarLoader::capacity(outstanding, owned + reserved));
    --reserved; // A cancelled or failed fetch releases its reserved cache slot.
    assert(AvatarLoader::capacity(outstanding, owned + reserved));

    reserved = 1;
    assert(!AvatarLoader::capacity(outstanding, owned + reserved));
    --reserved;
    ++owned; // A successful fetch converts, rather than frees, its reservation.
    assert(!AvatarLoader::capacity(outstanding, owned + reserved));
    outstanding = 0; // Polling cannot free a successful avatar's file capacity.
    assert(!AvatarLoader::capacity(outstanding, owned + reserved));
    --owned; // Removing an accepted avatar permits deferred work.
    assert(AvatarLoader::capacity(outstanding, owned + reserved));
    assert(!AvatarLoader::capacity(AvatarLoader::maxQueue + 1, 0));
    assert(!AvatarLoader::capacity(0, AvatarLoader::maxFiles + 1));
}

void lifecycle() {
    const char *previousTmp = std::getenv("TMPDIR");
    const bool hadTmp = previousTmp != nullptr;
    const std::string savedTmp = previousTmp ? previousTmp : "";
    std::string root =
        (std::filesystem::temp_directory_path() / "blastirc-avatar-check-XXXXXX").string();
    assert(::mkdtemp(root.data()));
    assert(::setenv("TMPDIR", root.c_str(), 1) == 0);
    const auto before = cacheDirectories();
    std::set<std::filesystem::path> created;
    std::string foreign = (std::filesystem::path(root) / "foreign-XXXXXX").string();
    const int fd = ::mkstemp(foreign.data());
    assert(fd >= 0);
    assert(::close(fd) == 0);
    {
        AvatarLoader loader;
        const auto during = cacheDirectories();
        std::set_difference(during.begin(), during.end(), before.begin(), before.end(),
                            std::inserter(created, created.end()));
        const auto *version = curl_version_info(CURLVERSION_NOW);
        if (version && (version->features & CURL_VERSION_ASYNCHDNS))
            assert(created.size() == 1);
        else
            assert(created.empty()); // Fail closed on blocking DNS builds.
        for (const auto &directory : created) {
            struct stat info{};
            assert(::stat(directory.c_str(), &info) == 0);
            assert((info.st_mode & 0777) == 0700 && info.st_uid == ::getuid());
            assert(std::filesystem::is_empty(directory));
        }
        const bool available = loader.hasCapacity();
        assert(available == !created.empty());
        assert(loader.poll().empty());
        assert(loader.request(0, "https://example.com/a") == AvatarLoader::Admission::Invalid);
        for (std::uint64_t id = 1; id <= AvatarLoader::maxQueue * 3; ++id) {
            assert(loader.request(id, "http://example.com/a") == AvatarLoader::Admission::Invalid);
            assert(loader.request(id, "https://127.1/a") == AvatarLoader::Admission::Invalid);
            loader.cancel(id);
        }
        loader.cancel(0);
        loader.remove(foreign.data());
        loader.remove("/etc/passwd");
        assert(std::filesystem::exists(foreign.data()));
        assert(loader.poll().empty());
        assert(loader.hasCapacity() == available);
    }
    for (const auto &directory : created)
        assert(!std::filesystem::exists(directory));
    assert(std::filesystem::exists(foreign.data()));
    assert(::unlink(foreign.data()) == 0);
    assert(::setenv("TMPDIR", (root + "/missing").c_str(), 1) == 0);
    {
        AvatarLoader unavailable;
        assert(!unavailable.hasCapacity() && unavailable.poll().empty());
    }
    assert(std::filesystem::is_empty(root)); // An invalid TMPDIR must not fall back elsewhere.
    if (hadTmp)
        assert(::setenv("TMPDIR", savedTmp.c_str(), 1) == 0);
    else
        assert(::unsetenv("TMPDIR") == 0);
    assert(std::filesystem::remove(root));
}

} // namespace

int main() {
    static_assert(AvatarLoader::maxQueue == 32 && AvatarLoader::maxFiles == 64);
    static_assert(AvatarLoader::maxBytes == 1048576 && AvatarLoader::maxDimension == 1024);
    static_assert(AvatarLoader::maxPixels == 1048576);
    urls();
    addresses();
    rasters();
    capacity();
    lifecycle();
}
