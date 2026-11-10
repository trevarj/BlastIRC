#include "avatar.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <curl/curl.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <webp/decode.h>

namespace blast {
namespace {

bool curlReady() {
    // Process lifetime: another loader or curl user may still own handles at shutdown.
    static const bool ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    return ready;
}

using CurlUrl = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>;
using CurlString = std::unique_ptr<char, decltype(&curl_free)>;
using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;

CurlString urlPart(CURLU *url, CURLUPart part) {
    char *value = nullptr;
    if (curl_url_get(url, part, &value, 0) != CURLUE_OK)
        return {nullptr, curl_free};
    return {value, curl_free};
}

bool publicV4(const std::uint8_t *p) {
    const auto a = p[0], b = p[1], c = p[2];
    return !(a == 0 || a == 10 || a == 127 || a >= 224 || (a == 100 && b >= 64 && b <= 127) ||
             (a == 169 && b == 254) || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
             (a == 192 && b == 0 && (c == 0 || c == 2)) || (a == 192 && b == 88 && c == 99) ||
             (a == 198 && (b == 18 || b == 19)) || (a == 198 && b == 51 && c == 100) ||
             (a == 203 && b == 0 && c == 113));
}

bool publicV6(const std::uint8_t *p) {
    // Only ordinary global unicast; conservatively exclude protocol/transition space.
    return (p[0] & 0xe0) == 0x20 && !(p[0] == 0x20 && p[1] == 0x01 && p[2] < 2) // 2001::/23
           && !(p[0] == 0x20 && p[1] == 0x01 && p[2] == 0x0d && p[3] == 0xb8) &&
           !(p[0] == 0x20 && p[1] == 0x02)                           // 6to4
           && !(p[0] == 0x3f && p[1] == 0xfe)                        // Retired 6bone.
           && !(p[0] == 0x3f && p[1] == 0xff && (p[2] & 0xf0) == 0); // 3fff::/20
}

bool dimensions(std::uint32_t width, std::uint32_t height) {
    return width && height && width <= AvatarLoader::maxDimension &&
           height <= AvatarLoader::maxDimension &&
           std::uint64_t(width) * height <= AvatarLoader::maxPixels;
}

std::uint32_t big32(const std::uint8_t *p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) |
           p[3];
}

std::uint32_t little32(const std::uint8_t *p) {
    return (std::uint32_t(p[3]) << 24) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[1]) << 8) |
           p[0];
}

std::uint16_t big16(const std::uint8_t *p) { return (std::uint16_t(p[0]) << 8) | p[1]; }

bool png(std::span<const std::uint8_t> bytes) {
    constexpr std::array<std::uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
    if (bytes.size() < 33 || !std::equal(signature.begin(), signature.end(), bytes.begin()) ||
        big32(bytes.data() + 8) != 13 || std::memcmp(bytes.data() + 12, "IHDR", 4) ||
        !dimensions(big32(bytes.data() + 16), big32(bytes.data() + 20)))
        return false;
    const auto depth = bytes[24], color = bytes[25];
    if (!((color == 0 && (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)) ||
          (color == 3 && (depth == 1 || depth == 2 || depth == 4 || depth == 8)) ||
          ((color == 2 || color == 4 || color == 6) && (depth == 8 || depth == 16))) ||
        bytes[26] || bytes[27] || bytes[28] > 1)
        return false;
    bool imageData = false;
    for (std::size_t offset = 33; offset < bytes.size();) {
        if (bytes.size() - offset < 12)
            return false;
        const auto length = big32(bytes.data() + offset);
        if (length > bytes.size() - offset - 12)
            return false;
        const auto *type = bytes.data() + offset + 4;
        if (!std::memcmp(type, "IHDR", 4) || !std::memcmp(type, "acTL", 4) ||
            !std::memcmp(type, "fcTL", 4) || !std::memcmp(type, "fdAT", 4))
            return false;
        if (!std::memcmp(type, "IDAT", 4))
            imageData = true;
        if (!std::memcmp(type, "IEND", 4))
            return imageData && length == 0 && offset + 12 == bytes.size();
        offset += std::size_t(length) + 12;
    }
    return false;
}

bool jpeg(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 4 || bytes[0] != 0xff || bytes[1] != 0xd8)
        return false;
    bool frame = false;
    for (std::size_t offset = 2; offset < bytes.size();) {
        if (bytes[offset++] != 0xff)
            return false;
        while (offset < bytes.size() && bytes[offset] == 0xff)
            ++offset;
        if (offset == bytes.size())
            return false;
        const auto marker = bytes[offset++];
        if (marker == 0 || marker == 0xd8 || marker == 0xd9 || marker == 0x01 ||
            (marker >= 0xd0 && marker <= 0xd7) || bytes.size() - offset < 2)
            return false;
        const auto length = big16(bytes.data() + offset);
        if (length < 2 || length > bytes.size() - offset)
            return false;
        if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 &&
            marker != 0xcc) {
            // Wuffs uses exactly these SOF layouts. Never skip a short frame as an unknown segment.
            if (frame || (marker != 0xc0 && marker != 0xc1 && marker != 0xc2) || length < 11 ||
                bytes[offset + 2] != 8)
                return false;
            const auto components = bytes[offset + 7];
            if ((components != 1 && components != 3 && components != 4) ||
                length != 8 + 3 * components ||
                !dimensions(big16(bytes.data() + offset + 5), big16(bytes.data() + offset + 3)))
                return false;
            for (unsigned i = 0; i < components; ++i) {
                const auto sampling = bytes[offset + 9 + 3 * i];
                if (!(sampling >> 4) || (sampling >> 4) > 4 || !(sampling & 15) ||
                    (sampling & 15) > 4 || bytes[offset + 10 + 3 * i] > 3)
                    return false;
                for (unsigned j = 0; j < i; ++j)
                    if (bytes[offset + 8 + 3 * i] == bytes[offset + 8 + 3 * j])
                        return false;
            }
            frame = true;
        }
        if (marker == 0xda) {
            if (!frame || length < 6)
                return false;
            const auto components = bytes[offset + 2];
            if (!components || components > 4 || length != 6 + 2 * components)
                return false;
            // Entropy data is left to the native decoder; require a complete container terminator.
            return offset + length < bytes.size() - 1 && bytes[bytes.size() - 2] == 0xff &&
                   bytes.back() == 0xd9;
        }
        offset += length;
    }
    return false;
}

struct TempFile {
    std::string path;
    int fd = -1;
    ~TempFile() {
        if (fd >= 0)
            ::close(fd);
        if (!path.empty())
            ::unlink(path.c_str());
    }

    bool save(const std::string &directory, std::uint64_t request,
              std::span<const std::uint8_t> bytes) {
        path = directory + "/avatar-" + std::to_string(request) + "-XXXXXX";
        fd = ::mkstemp(path.data());
        if (fd < 0) {
            path.clear();
            return false;
        }
        if (::fchmod(fd, 0600) || ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
            return false;
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
                return false;
            offset += static_cast<std::size_t>(written);
        }
        const int result = ::close(fd);
        fd = -1;
        return result == 0;
    }
};

} // namespace

bool AvatarLoader::publicAddress(std::string_view ip) {
    if (ip.empty() || ip.size() >= INET6_ADDRSTRLEN || ip.find('\0') != ip.npos)
        return false;
    std::array<char, INET6_ADDRSTRLEN> text{};
    std::copy(ip.begin(), ip.end(), text.begin());
    in_addr v4{};
    if (::inet_pton(AF_INET, text.data(), &v4) == 1)
        return publicV4(reinterpret_cast<const std::uint8_t *>(&v4));
    in6_addr v6{};
    return ::inet_pton(AF_INET6, text.data(), &v6) == 1 &&
           publicV6(reinterpret_cast<const std::uint8_t *>(&v6));
}

std::optional<std::string> AvatarLoader::url(std::string_view metadata) {
    if (metadata.empty() || metadata.size() > 2048 || !curlReady())
        return std::nullopt;
    for (unsigned char c : metadata)
        if (c <= 0x20 || c == 0x7f || c == '\\')
            return std::nullopt;
    std::string value(metadata);
    for (std::size_t position = 0; (position = value.find("{size}", position)) != value.npos;
         position += 2)
        value.replace(position, 6, "64");
    const auto schemeEnd = value.find("://");
    if (schemeEnd == value.npos)
        return std::nullopt;
    const auto authorityEnd = value.find_first_of("/?#", schemeEnd + 3);
    if (std::string_view(value)
            .substr(schemeEnd + 3,
                    authorityEnd == value.npos ? value.npos : authorityEnd - schemeEnd - 3)
            .find('@') != value.npos)
        return std::nullopt;
    CurlUrl parsed(curl_url(), curl_url_cleanup);
    if (!parsed ||
        curl_url_set(parsed.get(), CURLUPART_URL, value.c_str(), CURLU_DISALLOW_USER) != CURLUE_OK)
        return std::nullopt;
    auto scheme = urlPart(parsed.get(), CURLUPART_SCHEME);
    auto host = urlPart(parsed.get(), CURLUPART_HOST);
    if (!scheme || std::strcmp(scheme.get(), "https") || !host || !*host ||
        urlPart(parsed.get(), CURLUPART_USER) || urlPart(parsed.get(), CURLUPART_PASSWORD) ||
        urlPart(parsed.get(), CURLUPART_OPTIONS) || urlPart(parsed.get(), CURLUPART_ZONEID))
        return std::nullopt;
    std::string_view address(host.get());
    if (address.find('%') != address.npos)
        return std::nullopt;
    if (address.front() == '[' && address.back() == ']') {
        address.remove_prefix(1);
        address.remove_suffix(1);
        if (!publicAddress(address))
            return std::nullopt;
    } else {
        in_addr v4{};
        if (::inet_pton(AF_INET, host.get(), &v4) == 1 &&
            !publicV4(reinterpret_cast<const std::uint8_t *>(&v4)))
            return std::nullopt;
    }
    auto normalized = urlPart(parsed.get(), CURLUPART_URL);
    if (!normalized || std::strlen(normalized.get()) > 2048)
        return std::nullopt;
    return std::string(normalized.get());
}

std::optional<std::string_view> AvatarLoader::rasterExtension(std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > maxBytes)
        return std::nullopt;
    if (png(bytes))
        return ".png";
    if (jpeg(bytes))
        return ".jpg";
    if (bytes.size() >= 20 && !std::memcmp(bytes.data(), "RIFF", 4) &&
        !std::memcmp(bytes.data() + 8, "WEBP", 4) &&
        little32(bytes.data() + 4) == bytes.size() - 8) {
        for (std::size_t offset = 12; offset < bytes.size();) {
            if (bytes.size() - offset < 8)
                return std::nullopt;
            const auto length = little32(bytes.data() + offset + 4);
            const auto padded = std::size_t(length) + (length & 1);
            if (padded > bytes.size() - offset - 8 ||
                !std::memcmp(bytes.data() + offset, "ANIM", 4) ||
                !std::memcmp(bytes.data() + offset, "ANMF", 4))
                return std::nullopt;
            offset += 8 + padded;
        }
        WebPBitstreamFeatures features{};
        if (WebPGetFeatures(bytes.data(), bytes.size(), &features) == VP8_STATUS_OK &&
            !features.has_animation && features.width > 0 && features.height > 0 &&
            dimensions(static_cast<std::uint32_t>(features.width),
                       static_cast<std::uint32_t>(features.height)))
            return ".webp";
    }
    return std::nullopt;
}

bool AvatarLoader::capacity(std::size_t outstanding, std::size_t occupiedFiles) {
    return outstanding < maxQueue && occupiedFiles < maxFiles;
}

struct AvatarLoader::Impl {
    struct Request {
        std::uint64_t id;
        std::string url;
    };
    struct Transfer {
        Impl &owner;
        std::vector<std::uint8_t> bytes;
        std::size_t received = 0;
        bool tooLarge = false;
    };

    std::mutex mutex;
    std::condition_variable wake;
    std::vector<Request> queue;
    std::vector<Result> results;
    std::vector<std::string> files;
    std::string directory;
    std::thread worker;
    std::atomic<bool> abort{false};
    std::atomic<bool> available{false};
    bool stopping = false;
    bool blockedAddress = false; // Accessed only by the worker and its curl callbacks.
    std::uint64_t active = 0;
    std::uint64_t lastRequest = 0;
    std::size_t reservations = 0; // Each queued/active fetch already owns one cache slot.

    Impl() {
        queue.reserve(maxQueue);
        results.reserve(maxQueue);
        files.reserve(maxFiles);
        if (!curlReady())
            return;
        const auto *version = curl_version_info(CURLVERSION_NOW);
        // NOSIGNAL cannot bound a blocking system resolver; require curl's asynchronous DNS.
        if (!version || !(version->features & CURL_VERSION_ASYNCHDNS))
            return;
        std::error_code error;
        const auto temporary = std::filesystem::temp_directory_path(error);
        if (error)
            return;
        directory = (temporary / "blastirc-avatars-XXXXXX").string();
        if (!::mkdtemp(directory.data())) {
            directory.clear();
            return;
        }
        if (::chmod(directory.c_str(), 0700)) {
            ::rmdir(directory.c_str());
            directory.clear();
            return;
        }
        try {
            available = true;
            worker = std::thread([this] {
                try {
                    run();
                } catch (...) {
                    available = false;
                    abort = true;
                }
            });
        } catch (...) {
            available = false;
            ::rmdir(directory.c_str());
            directory.clear();
        }
    }

    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            abort = true;
            reservations -= queue.size();
            queue.clear();
        }
        wake.notify_one();
        if (worker.joinable())
            worker.join();
        for (const auto &path : files)
            ::unlink(path.c_str());
        if (!directory.empty())
            ::rmdir(directory.c_str());
    }

    void removeOwned(const std::string &path) {
        const auto found = std::find(files.begin(), files.end(), path);
        if (found == files.end())
            return;
        ::unlink(found->c_str());
        files.erase(found);
    }

    static curl_socket_t openSocket(void *data, curlsocktype purpose,
                                    curl_sockaddr *address) noexcept {
        auto &self = *static_cast<Impl *>(data);
        if (self.abort || purpose != CURLSOCKTYPE_IPCXN)
            return CURL_SOCKET_BAD;
        bool allowed = false;
        if (address->family == AF_INET && address->addrlen >= sizeof(sockaddr_in)) {
            const auto *ip = reinterpret_cast<const sockaddr_in *>(&address->addr);
            allowed = publicV4(reinterpret_cast<const std::uint8_t *>(&ip->sin_addr));
        } else if (address->family == AF_INET6 && address->addrlen >= sizeof(sockaddr_in6)) {
            const auto *ip = reinterpret_cast<const sockaddr_in6 *>(&address->addr);
            allowed = ip->sin6_scope_id == 0 &&
                      publicV6(reinterpret_cast<const std::uint8_t *>(&ip->sin6_addr));
        }
        if (!allowed) {
            self.blockedAddress = true;
            return CURL_SOCKET_BAD;
        }
        return ::socket(address->family, address->socktype | SOCK_CLOEXEC, address->protocol);
    }

    static int prerequisite(void *data, char *primary, char *, int, int) noexcept {
        auto &self = *static_cast<Impl *>(data);
        if (self.abort)
            return CURL_PREREQFUNC_ABORT;
        if (!primary || !publicAddress(primary)) {
            self.blockedAddress = true;
            return CURL_PREREQFUNC_ABORT;
        }
        return CURL_PREREQFUNC_OK;
    }

    static int progress(void *data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
        return static_cast<Impl *>(data)->abort ? 1 : 0;
    }

    static std::size_t receive(char *data, std::size_t size, std::size_t count,
                               void *context) noexcept {
        auto &transfer = *static_cast<Transfer *>(context);
        if (transfer.owner.abort)
            return 0;
        if (size && count > maxBytes / size) {
            transfer.tooLarge = true;
            return 0;
        }
        const auto length = size * count;
        if (length > maxBytes - transfer.received) {
            transfer.tooLarge = true;
            return 0;
        }
        transfer.received += length;
        try {
            transfer.bytes.insert(transfer.bytes.end(), reinterpret_cast<std::uint8_t *>(data),
                                  reinterpret_cast<std::uint8_t *>(data) + length);
        } catch (...) {
            return 0;
        }
        return length;
    }

    static std::size_t header(char *, std::size_t size, std::size_t count, void *context) noexcept {
        auto &transfer = *static_cast<Transfer *>(context);
        if (transfer.owner.abort)
            return 0;
        if (size && count > maxBytes / size) {
            transfer.tooLarge = true;
            return 0;
        }
        const auto length = size * count;
        if (length > maxBytes - transfer.received) {
            transfer.tooLarge = true;
            return 0;
        }
        transfer.received += length;
        return length;
    }

    std::vector<std::uint8_t> download(const std::string &url, std::string &error) {
        auto failure = [&](const char *classification, CURLcode code) {
            error = classification;
            error += ": ";
            error += curl_easy_strerror(code);
            error += " (" + std::to_string(static_cast<int>(code)) + ")";
        };
        Transfer transfer{*this, {}, 0};
        CurlHandle handle(curl_easy_init(), curl_easy_cleanup);
        if (!handle) {
            failure("curl initialization", CURLE_FAILED_INIT);
            return {};
        }
        transfer.bytes.reserve(maxBytes);
        blockedAddress = false;
        CURLcode code = CURLE_OK;
        auto set = [&](CURLoption option, auto value) {
            code = curl_easy_setopt(handle.get(), option, value);
            return code == CURLE_OK;
        };
        if (!set(CURLOPT_URL, url.c_str()) || !set(CURLOPT_PROTOCOLS_STR, "https") ||
            !set(CURLOPT_PROXY, "") || !set(CURLOPT_PRE_PROXY, "") ||
            !set(CURLOPT_FOLLOWLOCATION, 0L) || !set(CURLOPT_MAXREDIRS, 0L) ||
            !set(CURLOPT_FRESH_CONNECT, 1L) || !set(CURLOPT_FORBID_REUSE, 1L) ||
            !set(CURLOPT_DNS_CACHE_TIMEOUT, 0L) || !set(CURLOPT_SSL_VERIFYPEER, 1L) ||
            !set(CURLOPT_SSL_VERIFYHOST, 2L) || !set(CURLOPT_NETRC, long(CURL_NETRC_IGNORED)) ||
            !set(CURLOPT_DISALLOW_USERNAME_IN_URL, 1L) ||
            !set(CURLOPT_HTTPAUTH, long(CURLAUTH_NONE)) ||
            !set(CURLOPT_HTTP_VERSION, long(CURL_HTTP_VERSION_1_1)) || !set(CURLOPT_NOSIGNAL, 1L) ||
            !set(CURLOPT_CONNECTTIMEOUT_MS, 3000L) || !set(CURLOPT_TIMEOUT_MS, 10000L) ||
            !set(CURLOPT_LOW_SPEED_LIMIT, 128L) || !set(CURLOPT_LOW_SPEED_TIME, 3L) ||
            !set(CURLOPT_MAXFILESIZE_LARGE, curl_off_t(maxBytes)) ||
            !set(CURLOPT_HTTP_CONTENT_DECODING, 0L) ||
            !set(CURLOPT_OPENSOCKETFUNCTION, &openSocket) || !set(CURLOPT_OPENSOCKETDATA, this) ||
            !set(CURLOPT_PREREQFUNCTION, &prerequisite) || !set(CURLOPT_PREREQDATA, this) ||
            !set(CURLOPT_NOPROGRESS, 0L) || !set(CURLOPT_XFERINFOFUNCTION, &progress) ||
            !set(CURLOPT_XFERINFODATA, this) || !set(CURLOPT_WRITEFUNCTION, &receive) ||
            !set(CURLOPT_WRITEDATA, &transfer) || !set(CURLOPT_HEADERFUNCTION, &header) ||
            !set(CURLOPT_HEADERDATA, &transfer)) {
            failure("curl setup", code);
            return {};
        }
        code = curl_easy_perform(handle.get());
        if (code != CURLE_OK) {
            const char *classification = "HTTPS transfer";
            if (abort)
                classification = "canceled";
            else if (transfer.tooLarge || code == CURLE_FILESIZE_EXCEEDED)
                classification = "response size limit";
            else if (code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CACERT_BADFILE)
                classification = "TLS verification";
            else if (code == CURLE_SSL_CONNECT_ERROR)
                classification = "TLS connection";
            else if (code == CURLE_COULDNT_RESOLVE_HOST)
                classification = "DNS resolution";
            else if (blockedAddress &&
                     (code == CURLE_COULDNT_CONNECT || code == CURLE_ABORTED_BY_CALLBACK))
                classification = "public-address policy";
            else if (code == CURLE_COULDNT_CONNECT)
                classification = "connect";
            else if (code == CURLE_OPERATION_TIMEDOUT)
                classification = "timeout";
            failure(classification, code);
            if (code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CACERT_BADFILE ||
                code == CURLE_SSL_CONNECT_ERROR) {
                long verification = 0;
                if (curl_easy_getinfo(handle.get(), CURLINFO_SSL_VERIFYRESULT, &verification) ==
                    CURLE_OK)
                    error += "; TLS verify result " + std::to_string(verification);
            }
            return {};
        }
        if (abort) {
            error = "canceled";
            return {};
        }
        long status = 0;
        code = curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status);
        if (code != CURLE_OK) {
            failure("HTTP response", code);
            return {};
        }
        if (status != 200) {
            error = "HTTP status " + std::to_string(status);
            return {};
        }
        return std::move(transfer.bytes);
    }

    void run() {
        for (;;) {
            Request item;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return stopping || !queue.empty(); });
                if (stopping)
                    return;
                item = std::move(queue.front());
                queue.erase(queue.begin());
                active = item.id;
                abort = false;
            }
            TempFile file;
            bool saved = false;
            std::string error;
            try {
                if (!abort) {
                    const auto bytes = download(item.url, error);
                    if (!abort && error.empty()) {
                        if (!rasterExtension(bytes))
                            error = "raster preflight rejected";
                        else {
                            saved = file.save(directory, item.id, bytes);
                            if (!saved)
                                error = "cache write failed";
                        }
                    }
                }
            } catch (...) {
                // An untrusted response, allocation, or filesystem failure must not kill the
                // client.
                error = "worker failure";
            }
            {
                std::lock_guard lock(mutex);
                if (!stopping && !abort) {
                    std::string path;
                    if (saved) {
                        try {
                            files.push_back(file.path);
                            path = std::move(file.path);
                            file.path.clear();
                        } catch (...) {
                            error = "cache ownership failed";
                        }
                    }
                    // Admission counts queued, active, and unpolled results together, reserving
                    // this slot.
                    results.push_back({item.id, std::move(path), std::move(error)});
                }
                --reservations;
                active = 0;
            }
        }
    }
};

AvatarLoader::AvatarLoader() : impl_(std::make_unique<Impl>()) {}
AvatarLoader::~AvatarLoader() = default;

AvatarLoader::Admission AvatarLoader::request(std::uint64_t id, const std::string &metadata) {
    if (!id || !impl_->available)
        return Admission::Invalid;
    try {
        auto address = url(metadata);
        if (!address)
            return Admission::Invalid;
        {
            std::lock_guard lock(impl_->mutex);
            if (impl_->stopping || !impl_->available || id <= impl_->lastRequest)
                return Admission::Invalid;
            if (!capacity(impl_->queue.size() + impl_->results.size() + (impl_->active != 0),
                          impl_->files.size() + impl_->reservations))
                return Admission::Full;
            impl_->queue.push_back({id, std::move(*address)});
            ++impl_->reservations;
            impl_->lastRequest = id;
        }
        impl_->wake.notify_one();
        return Admission::Queued;
    } catch (...) {
        return Admission::Invalid;
    }
}

bool AvatarLoader::hasCapacity() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->available && !impl_->stopping &&
           capacity(impl_->queue.size() + impl_->results.size() + (impl_->active != 0),
                    impl_->files.size() + impl_->reservations);
}

std::vector<AvatarLoader::Result> AvatarLoader::poll() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->results.empty())
        return {};
    std::vector<Result> ready;
    ready.reserve(maxQueue);
    ready.swap(impl_->results);
    return ready;
}

void AvatarLoader::cancel(std::uint64_t id) {
    std::lock_guard lock(impl_->mutex);
    const auto removed =
        std::erase_if(impl_->queue, [id](const auto &item) { return item.id == id; });
    impl_->reservations -= removed;
    if (impl_->active == id)
        impl_->abort = true;
    std::erase_if(impl_->results, [&](const auto &result) {
        if (result.request != id)
            return false;
        impl_->removeOwned(result.path);
        return true;
    });
}

void AvatarLoader::remove(const std::string &path) {
    std::lock_guard lock(impl_->mutex);
    impl_->removeOwned(path);
}

} // namespace blast
