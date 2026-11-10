#include "preview.hpp"
#include "avatar.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <curl/curl.h>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <libxml/HTMLparser.h>
#include <libxml/tree.h>
#include <limits.h>
#include <limits>
#include <mutex>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <webp/decode.h>

namespace blast {
namespace {
constexpr std::uint64_t MiB = 1024 * 1024;
constexpr std::size_t htmlLimit = 256 * 1024, headerLimit = 32 * 1024;
constexpr std::uint64_t cacheLimit = PreviewService::maxAssetBytes;
constexpr std::size_t slots = PreviewService::maxAssets;
bool ready() {
    static const bool ok = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    return ok;
}
using Url = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>;
using Text = std::unique_ptr<char, decltype(&curl_free)>;
Text part(CURLU *u, CURLUPart p) {
    char *v = nullptr;
    if (curl_url_get(u, p, &v, 0) != CURLUE_OK)
        return {nullptr, curl_free};
    return {v, curl_free};
}
std::string lower(std::string_view s) {
    std::string out(s);
    for (char &c : out)
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    return out;
}
std::string_view trim(std::string_view s) {
    while (!s.empty() &&
           (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
        s.remove_prefix(1);
    while (!s.empty() &&
           (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.remove_suffix(1);
    return s;
}
std::string plain(const xmlChar *s, std::size_t limit) {
    std::string result;
    if (!s)
        return result;
    bool space = false;
    // libxml already emits UTF-8. Never cut inside a multi-byte character.
    for (std::size_t i = 0; s[i];) {
        const unsigned char c = s[i];
        if (c <= 0x20 || c == 0x7f) {
            space = !result.empty();
            ++i;
            continue;
        }
        std::size_t n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        if (result.size() + n + space > limit)
            break;
        if (space)
            result += ' ';
        space = false;
        for (std::size_t j = 0; j < n && s[i]; ++j)
            result += char(s[i++]);
    }
    return result;
}
std::optional<std::string> relative(std::string_view base, std::string_view value, bool local) {
    Url u(curl_url(), curl_url_cleanup);
    std::string b(base), v(value);
    if (!u || curl_url_set(u.get(), CURLUPART_URL, b.c_str(), CURLU_DISALLOW_USER) != CURLUE_OK ||
        curl_url_set(u.get(), CURLUPART_URL, v.c_str(), CURLU_DISALLOW_USER) != CURLUE_OK)
        return {};
    auto full = part(u.get(), CURLUPART_URL);
    return full ? PreviewService::url(full.get(), local) : std::nullopt;
}
std::uint32_t be32(const std::uint8_t *p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) |
           p[3];
}
std::uint32_t le32(const std::uint8_t *p) {
    return (std::uint32_t(p[3]) << 24) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[1]) << 8) |
           p[0];
}
std::uint16_t be16(const std::uint8_t *p) { return (std::uint16_t(p[0]) << 8) | p[1]; }
bool dimensions(std::uint32_t w, std::uint32_t h) {
    return w && h && w <= 8192 && h <= 8192 && std::uint64_t(w) * h <= 32 * MiB;
}
} // namespace

PreviewAsset::PreviewAsset(std::string p, std::uint64_t n) : path(std::move(p)), bytes(n) {}
PreviewAsset::~PreviewAsset() {
    if (!path.empty()) {
        ::unlink(path.c_str());
        // Each file owns its own directory, so assets safely outlive the service.
        const auto slash = path.rfind('/');
        if (slash != path.npos && slash < PATH_MAX) {
            char directory[PATH_MAX];
            std::memcpy(directory, path.data(), slash);
            directory[slash] = '\0';
            ::rmdir(directory);
        }
    }
}

std::optional<std::string> PreviewService::url(std::string_view value, bool local) {
    if (!ready() || value.empty() || value.size() > 4096)
        return {};
    for (unsigned char c : value)
        if (c <= 0x20 || c == 0x7f || c == '\\')
            return {};
    const auto start = value.find("://");
    if (start == value.npos)
        return {};
    const auto end = value.find_first_of("/?#", start + 3);
    if (value.substr(start + 3, end == value.npos ? value.npos : end - start - 3).find('@') !=
        value.npos)
        return {};
    Url u(curl_url(), curl_url_cleanup);
    std::string text(value);
    if (!u || curl_url_set(u.get(), CURLUPART_URL, text.c_str(), CURLU_DISALLOW_USER) != CURLUE_OK)
        return {};
    auto scheme = part(u.get(), CURLUPART_SCHEME), host = part(u.get(), CURLUPART_HOST);
    if (!scheme || std::strcmp(scheme.get(), "https") || !host || !*host ||
        part(u.get(), CURLUPART_USER) || part(u.get(), CURLUPART_PASSWORD) ||
        part(u.get(), CURLUPART_OPTIONS) || part(u.get(), CURLUPART_ZONEID))
        return {};
    std::string_view ip(host.get());
    if (ip.find('%') != ip.npos)
        return {};
    if (ip.front() == '[' && ip.back() == ']') {
        ip.remove_prefix(1);
        ip.remove_suffix(1);
    }
    in_addr v4{};
    in6_addr v6{};
    const std::string numeric(ip);
    if (!local && ((::inet_pton(AF_INET, numeric.c_str(), &v4) == 1 ||
                    ::inet_pton(AF_INET6, numeric.c_str(), &v6) == 1) &&
                   !AvatarLoader::publicAddress(ip)))
        return {};
    curl_url_set(u.get(), CURLUPART_FRAGMENT, nullptr, 0);
    auto normalized = part(u.get(), CURLUPART_URL);
    if (!normalized || std::strlen(normalized.get()) > 4096)
        return {};
    return std::string(normalized.get());
}

PreviewKind PreviewService::classify(std::string_view contentType) {
    const auto t = lower(trim(contentType.substr(0, contentType.find(';'))));
    if (t == "text/html" || t == "application/xhtml+xml")
        return PreviewKind::Link;
    if (t == "image/png" || t == "image/jpeg" || t == "image/webp" || t == "image/gif")
        return PreviewKind::Image;
    if (t.starts_with("audio/"))
        return PreviewKind::Audio;
    if (t.starts_with("video/"))
        return PreviewKind::Video;
    return PreviewKind::Unsupported;
}

std::optional<PreviewRaster> PreviewService::raster(std::span<const std::uint8_t> b) {
    if (b.empty() || b.size() > 32 * MiB)
        return {};
    std::uint32_t w = 0, h = 0;
    std::string ext;
    constexpr std::array<std::uint8_t, 8> sig{137, 80, 78, 71, 13, 10, 26, 10};
    if (b.size() >= 33 && std::equal(sig.begin(), sig.end(), b.begin()) &&
        be32(b.data() + 8) == 13 && !std::memcmp(b.data() + 12, "IHDR", 4)) {
        w = be32(b.data() + 16);
        h = be32(b.data() + 20);
        ext = ".png";
        const auto depth = b[24], color = b[25];
        if (!((color == 0 &&
               (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)) ||
              (color == 3 && (depth == 1 || depth == 2 || depth == 4 || depth == 8)) ||
              ((color == 2 || color == 4 || color == 6) && (depth == 8 || depth == 16))) ||
            b[26] || b[27] || b[28] > 1)
            return {};
        bool data = false, end = false;
        for (std::size_t o = 33; o < b.size();) {
            if (b.size() - o < 12)
                return {};
            const auto n = be32(b.data() + o);
            if (n > b.size() - o - 12)
                return {};
            const auto *t = b.data() + o + 4;
            if (!std::memcmp(t, "IHDR", 4) || !std::memcmp(t, "acTL", 4) ||
                !std::memcmp(t, "fcTL", 4) || !std::memcmp(t, "fdAT", 4))
                return {};
            if (!std::memcmp(t, "IDAT", 4))
                data = true;
            if (!std::memcmp(t, "IEND", 4)) {
                end = data && n == 0 && o + 12 == b.size();
                break;
            }
            o += n + 12;
        }
        if (!end)
            return {};
    } else if (b.size() >= 4 && b[0] == 0xff && b[1] == 0xd8) {
        bool frame = false, scan = false;
        for (std::size_t o = 2; o < b.size();) {
            if (b[o++] != 0xff)
                return {};
            while (o < b.size() && b[o] == 0xff)
                ++o;
            if (o == b.size())
                return {};
            const auto m = b[o++];
            if (m == 0 || m == 0xd8 || m == 0xd9 || m == 1 || (m >= 0xd0 && m <= 0xd7) ||
                b.size() - o < 2)
                return {};
            const auto n = be16(b.data() + o);
            if (n < 2 || n > b.size() - o)
                return {};
            if (m >= 0xc0 && m <= 0xcf && m != 0xc4 && m != 0xc8 && m != 0xcc) {
                if (frame || (m != 0xc0 && m != 0xc1 && m != 0xc2) || n < 11 || b[o + 2] != 8)
                    return {};
                const auto c = b[o + 7];
                if ((c != 1 && c != 3 && c != 4) || n != 8 + 3 * c)
                    return {};
                h = be16(b.data() + o + 3);
                w = be16(b.data() + o + 5);
                frame = true;
            }
            if (m == 0xda) {
                if (!frame || n < 6 || !b[o + 2] || b[o + 2] > 4 || n != 6 + 2 * b[o + 2])
                    return {};
                scan = o + n < b.size() - 1 && b[b.size() - 2] == 0xff && b.back() == 0xd9;
                break;
            }
            o += n;
        }
        if (!scan)
            return {};
        ext = ".jpg";
    } else if (b.size() >= 20 && !std::memcmp(b.data(), "RIFF", 4) &&
               !std::memcmp(b.data() + 8, "WEBP", 4) && le32(b.data() + 4) == b.size() - 8) {
        for (std::size_t o = 12; o < b.size();) {
            if (b.size() - o < 8)
                return {};
            const auto n = le32(b.data() + o + 4);
            const auto padded = std::uint64_t(n) + (n & 1);
            if (padded > b.size() - o - 8 || !std::memcmp(b.data() + o, "ANIM", 4) ||
                !std::memcmp(b.data() + o, "ANMF", 4))
                return {};
            o += 8 + padded;
        }
        WebPBitstreamFeatures f{};
        if (WebPGetFeatures(b.data(), b.size(), &f) != VP8_STATUS_OK || f.has_animation ||
            f.width <= 0 || f.height <= 0)
            return {};
        w = f.width;
        h = f.height;
        ext = ".webp";
    } else if (b.size() >= 14 &&
               (!std::memcmp(b.data(), "GIF87a", 6) || !std::memcmp(b.data(), "GIF89a", 6))) {
        const auto little16 = [&](std::size_t o) {
            return std::uint32_t(b[o]) | (std::uint32_t(b[o + 1]) << 8);
        };
        w = little16(6);
        h = little16(8);
        if (!dimensions(w, h))
            return {};
        std::size_t o = 13;
        const auto table = [&](std::uint8_t flags) {
            const auto n = (flags & 0x80) ? std::size_t(3) << ((flags & 7) + 1) : 0;
            if (n > b.size() - o)
                return false;
            o += n;
            return true;
        };
        const auto blocks = [&] {
            while (o < b.size()) {
                const auto n = b[o++];
                if (!n)
                    return true;
                if (n > b.size() - o)
                    return false;
                o += n;
            }
            return false;
        };
        if (!table(b[10]))
            return {};
        bool frame = false, end = false;
        while (o < b.size()) {
            const auto marker = b[o++];
            if (marker == 0x3b) {
                end = frame && o == b.size();
                break;
            }
            if (marker == 0x21) {
                if (o == b.size())
                    return {};
                ++o; // Extension label; sub-blocks are bounded but never interpreted here.
                if (!blocks())
                    return {};
            } else if (marker == 0x2c) {
                if (b.size() - o < 9)
                    return {};
                const auto x = little16(o), y = little16(o + 2);
                const auto fw = little16(o + 4), fh = little16(o + 6);
                const auto flags = b[o + 8];
                if (!fw || !fh || x + fw > w || y + fh > h)
                    return {};
                o += 9;
                if (!table(flags) || o == b.size() || b[o] < 2 || b[o] > 8)
                    return {};
                ++o; // LZW minimum code size. The bounded decoder validates pixel data.
                if (!blocks())
                    return {};
                frame = true;
            } else
                return {};
        }
        if (!end)
            return {};
        ext = ".gif";
    } else
        return {};
    if (!dimensions(w, h))
        return {};
    return PreviewRaster{w, h, std::move(ext)};
}

PreviewMetadata PreviewService::metadata(std::string_view html, std::string_view base, bool local) {
    PreviewMetadata result;
    if (html.empty() || html.size() > htmlLimit || html.find('\0') != html.npos)
        return result;
    // No entity substitution, DTD loading, XInclude, or resource fetching. Reject declarations
    // before libxml sees them: HTML recovery must not turn entity declarations into content.
    const auto folded = lower(html);
    if (folded.find("<!entity") != folded.npos)
        return result;
    for (std::size_t at = 0; (at = folded.find("<!doctype", at)) != folded.npos;) {
        const auto end = folded.find('>', at);
        if (end == folded.npos)
            return result;
        const auto declaration = std::string_view(folded).substr(at, end - at);
        if (declaration.find("system") != declaration.npos ||
            declaration.find("public") != declaration.npos ||
            declaration.find('[') != declaration.npos)
            return result;
        at = end + 1;
    }
    using Context = std::unique_ptr<htmlParserCtxt, decltype(&htmlFreeParserCtxt)>;
    Context context(htmlNewParserCtxt(), htmlFreeParserCtxt);
    if (!context)
        return result;
    // Per-context denial includes file:// and custom loaders, not only network URLs.
    xmlCtxtSetResourceLoader(
        context.get(),
        [](void *, const char *, const char *, xmlResourceType, xmlParserInputFlags,
           xmlParserInput **out) -> xmlParserErrors {
            *out = nullptr;
            return XML_IO_LOAD_ERROR;
        },
        nullptr);
    using Doc = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;
    Doc doc(htmlCtxtReadMemory(context.get(), html.data(), static_cast<int>(html.size()), nullptr,
                               nullptr,
                               HTML_PARSE_NODEFDTD | HTML_PARSE_NONET | HTML_PARSE_NOERROR |
                                   HTML_PARSE_NOWARNING | HTML_PARSE_COMPACT),
            xmlFreeDoc);
    if (!doc)
        return result;
    std::vector<xmlNode *> nodes;
    if (auto *root = xmlDocGetRootElement(doc.get()))
        nodes.push_back(root);
    std::size_t count = 0;
    while (!nodes.empty() && ++count <= 16384) {
        auto *node = nodes.back();
        nodes.pop_back();
        if (node->next)
            nodes.push_back(node->next);
        if (node->children)
            nodes.push_back(node->children);
        if (node->type != XML_ELEMENT_NODE)
            continue;
        if (!xmlStrcasecmp(node->name, BAD_CAST "title") && result.title.empty()) {
            auto *text = xmlNodeGetContent(node);
            result.title = plain(text, 256);
            xmlFree(text);
        } else if (!xmlStrcasecmp(node->name, BAD_CAST "meta")) {
            auto *key = xmlGetProp(node, BAD_CAST "property");
            if (!key)
                key = xmlGetProp(node, BAD_CAST "name");
            auto *content = xmlGetProp(node, BAD_CAST "content");
            const auto k = key ? lower(reinterpret_cast<const char *>(key)) : std::string{};
            if (k == "og:title")
                result.title = plain(content, 256);
            else if ((k == "description" || k == "og:description") && result.description.empty())
                result.description = plain(content, 512);
            else if (k == "og:image" && content && result.imageUrl.empty()) {
                const std::string_view v(reinterpret_cast<const char *>(content));
                if (v.size() <= 4096)
                    if (auto address = relative(base, v, local))
                        result.imageUrl = std::move(*address);
            }
            xmlFree(key);
            xmlFree(content);
        }
    }
    return result;
}

struct PreviewService::Impl {
    struct Budget {
        std::mutex mutex;
        std::size_t files = 0;
        std::uint64_t bytes = 0;
        std::size_t pending = 0;
    };
    struct Transfer {
        Impl &owner;
        const Request &request;
        std::vector<std::uint8_t> bytes{};
        std::string location{}, contentType{};
        std::size_t headers = 0;
        long status = 0;
        PreviewKind kind = PreviewKind::Unsupported;
        bool classified = false, limited = false, blocked = false;
    };
    std::shared_ptr<Budget> budget = std::make_shared<Budget>();
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Request> queue;
    std::vector<Result> results;
    std::thread worker;
    std::atomic<bool> abort{false};
    bool stopping = false, available = false;
    std::uint64_t active = 0, last = 0;
    bool local = false, blocked = false;
    const std::string caBundle;

    explicit Impl(std::string trust) : caBundle(std::move(trust)) {
        if (!ready())
            return;
        const auto *v = curl_version_info(CURLVERSION_NOW);
        if (!v || !(v->features & CURL_VERSION_ASYNCHDNS))
            return;
        try {
            worker = std::thread([this] { run(); });
            available = true;
        } catch (...) {
        }
    }
    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            abort = true;
            discardQueued();
        }
        wake.notify_one();
        if (worker.joinable())
            worker.join();
    }
    void releasePending(std::size_t count) {
        if (!count)
            return;
        std::lock_guard lock(budget->mutex);
        budget->pending -= count;
    }
    void discardQueued() {
        const auto count = std::count_if(queue.begin(), queue.end(), [](const Request &r) {
            return r.kind != PreviewKind::Link;
        });
        releasePending(static_cast<std::size_t>(count));
        queue.clear();
    }
    static curl_socket_t openSocket(void *p, curlsocktype purpose, curl_sockaddr *a) noexcept {
        auto &s = *static_cast<Impl *>(p);
        if (s.abort || purpose != CURLSOCKTYPE_IPCXN)
            return CURL_SOCKET_BAD;
        char text[INET6_ADDRSTRLEN]{};
        const void *addr = nullptr;
        if (a->family == AF_INET && a->addrlen >= sizeof(sockaddr_in))
            addr = &reinterpret_cast<sockaddr_in *>(&a->addr)->sin_addr;
        else if (a->family == AF_INET6 && a->addrlen >= sizeof(sockaddr_in6)) {
            auto *v6 = reinterpret_cast<sockaddr_in6 *>(&a->addr);
            if (v6->sin6_scope_id)
                return CURL_SOCKET_BAD;
            addr = &v6->sin6_addr;
        }
        if (!addr || !::inet_ntop(a->family, addr, text, sizeof(text)) ||
            (!s.local && !AvatarLoader::publicAddress(text))) {
            s.blocked = true;
            return CURL_SOCKET_BAD;
        }
        return ::socket(a->family, a->socktype | SOCK_CLOEXEC, a->protocol);
    }
    static int prerequisite(void *p, char *peer, char *, int, int) noexcept {
        auto &s = *static_cast<Impl *>(p);
        if (s.abort)
            return CURL_PREREQFUNC_ABORT;
        if (!peer || (!s.local && !AvatarLoader::publicAddress(peer))) {
            s.blocked = true;
            return CURL_PREREQFUNC_ABORT;
        }
        return CURL_PREREQFUNC_OK;
    }
    static int progress(void *p, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
        return static_cast<Impl *>(p)->abort ? 1 : 0;
    }
    static std::size_t header(char *p, std::size_t size, std::size_t count, void *data) noexcept {
        auto &t = *static_cast<Transfer *>(data);
        if (t.owner.abort || (size && count > headerLimit / size))
            return 0;
        const auto n = size * count;
        if (n > headerLimit - t.headers) {
            t.limited = true;
            return 0;
        }
        t.headers += n;
        try {
            std::string_view line(p, n);
            if (line.starts_with("HTTP/")) {
                t.contentType.clear();
                t.location.clear();
                const auto space = line.find(' ');
                if (space != line.npos && line.size() >= space + 4) {
                    t.status = 0;
                    for (std::size_t i = space + 1; i < space + 4; ++i) {
                        if (line[i] < '0' || line[i] > '9')
                            return 0;
                        t.status = t.status * 10 + line[i] - '0';
                    }
                }
            } else if (line == "\r\n" || line == "\n") {
                if (t.status >= 200 && t.status != 200)
                    return 0; // Only complete HTTP 200 bodies may become previews.
                if (t.status == 200) {
                    t.kind = classify(t.contentType);
                    if (t.kind != t.request.kind) {
                        t.classified = true;
                        return 0;
                    }
                }
            } else {
                const auto colon = line.find(':');
                if (colon != line.npos) {
                    const auto key = lower(line.substr(0, colon));
                    if (key == "content-type")
                        t.contentType = trim(line.substr(colon + 1));
                    else if (key == "location")
                        t.location = trim(line.substr(colon + 1));
                    else if (key == "content-encoding" &&
                             lower(trim(line.substr(colon + 1))) != "identity")
                        return 0;
                }
            }
        } catch (...) {
            return 0;
        }
        return n;
    }
    static std::size_t receive(char *p, std::size_t size, std::size_t count, void *data) noexcept {
        auto &t = *static_cast<Transfer *>(data);
        if (t.owner.abort || (size && count > t.request.maxBytes / size)) {
            t.limited = true;
            return 0;
        }
        const auto n = size * count;
        if (t.status >= 300 && t.status < 400)
            return 0; // Redirect bodies are never consumed.
        const auto limit = t.request.kind == PreviewKind::Link
                               ? std::min<std::uint64_t>(htmlLimit, t.request.maxBytes)
                           : t.request.kind == PreviewKind::Image
                               ? std::min<std::uint64_t>(32 * MiB, t.request.maxBytes)
                               : t.request.maxBytes;
        if (n > limit - t.bytes.size()) {
            t.limited = true;
            return 0;
        }
        try {
            t.bytes.insert(t.bytes.end(), reinterpret_cast<std::uint8_t *>(p),
                           reinterpret_cast<std::uint8_t *>(p) + n);
        } catch (...) {
            return 0;
        }
        return n;
    }
    Result download(const Request &r, bool &pending) {
        Result result;
        result.id = r.id;
        result.generation = r.generation;
        result.url = r.url;
        local = r.allowLocal;
        for (unsigned hop = 0; hop <= 4 && !abort; ++hop) {
            auto admitted = url(result.url, r.allowLocal);
            if (!admitted) {
                result.error = "address policy";
                return result;
            }
            result.url = std::move(*admitted);
            Url parsed(curl_url(), curl_url_cleanup);
            curl_url_set(parsed.get(), CURLUPART_URL, result.url.c_str(), 0);
            auto h = part(parsed.get(), CURLUPART_HOST);
            result.host = h ? h.get() : "";
            using Handle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
            Handle handle(curl_easy_init(), curl_easy_cleanup);
            if (!handle) {
                result.error = "transport initialization";
                return result;
            }
            Transfer t{*this, r};
            blocked = false;
            auto set = [&](CURLoption o, auto v) {
                return curl_easy_setopt(handle.get(), o, v) == CURLE_OK;
            };
            if (!set(CURLOPT_URL, result.url.c_str()) || !set(CURLOPT_PROTOCOLS_STR, "https") ||
                !set(CURLOPT_PROXY, "") || !set(CURLOPT_PRE_PROXY, "") ||
                !set(CURLOPT_FOLLOWLOCATION, 0L) || !set(CURLOPT_FRESH_CONNECT, 1L) ||
                !set(CURLOPT_FORBID_REUSE, 1L) || !set(CURLOPT_DNS_CACHE_TIMEOUT, 0L) ||
                !set(CURLOPT_SSL_VERIFYPEER, 1L) || !set(CURLOPT_SSL_VERIFYHOST, 2L) ||
                (!caBundle.empty() && !set(CURLOPT_CAINFO, caBundle.c_str())) ||
                !set(CURLOPT_NETRC, long(CURL_NETRC_IGNORED)) ||
                !set(CURLOPT_DISALLOW_USERNAME_IN_URL, 1L) ||
                !set(CURLOPT_HTTPAUTH, long(CURLAUTH_NONE)) ||
                !set(CURLOPT_HTTP_VERSION, long(CURL_HTTP_VERSION_1_1)) ||
                !set(CURLOPT_NOSIGNAL, 1L) || !set(CURLOPT_CONNECTTIMEOUT_MS, 3000L) ||
                !set(CURLOPT_TIMEOUT_MS, 30000L) || !set(CURLOPT_LOW_SPEED_LIMIT, 128L) ||
                !set(CURLOPT_LOW_SPEED_TIME, 5L) ||
                !set(CURLOPT_MAXFILESIZE_LARGE, curl_off_t(r.maxBytes)) ||
                !set(CURLOPT_HTTP_CONTENT_DECODING, 0L) ||
                !set(CURLOPT_OPENSOCKETFUNCTION, &openSocket) ||
                !set(CURLOPT_OPENSOCKETDATA, this) || !set(CURLOPT_PREREQFUNCTION, &prerequisite) ||
                !set(CURLOPT_PREREQDATA, this) || !set(CURLOPT_NOPROGRESS, 0L) ||
                !set(CURLOPT_XFERINFOFUNCTION, &progress) || !set(CURLOPT_XFERINFODATA, this) ||
                !set(CURLOPT_WRITEFUNCTION, &receive) || !set(CURLOPT_WRITEDATA, &t) ||
                !set(CURLOPT_HEADERFUNCTION, &header) || !set(CURLOPT_HEADERDATA, &t)) {
                result.error = "transport setup";
                return result;
            }
            const auto code = curl_easy_perform(handle.get());
            if (abort) {
                result.error = "canceled";
                return result;
            }
            if (t.status == 301 || t.status == 302 || t.status == 303 || t.status == 307 ||
                t.status == 308) {
                if (hop == 4 || t.location.empty()) {
                    result.error = "redirect limit";
                    return result;
                }
                auto next = relative(result.url, t.location, r.allowLocal);
                if (!next) {
                    result.error = "redirect address policy";
                    return result;
                }
                result.url = std::move(*next);
                continue;
            }
            if (t.status >= 200 && t.status != 200) {
                const char *reason = t.status == 206
                                         ? "partial response; complete HTTP 200 required"
                                     : t.status == 403 ? "origin refused anonymous preview"
                                     : t.status == 404 ? "asset not found"
                                     : t.status == 429 ? "origin rate limit"
                                     : t.status >= 500 ? "origin server error"
                                                       : "response not accepted; HTTP 200 required";
                result.error = "HTTP " + std::to_string(t.status) + ": " + reason;
                return result;
            }
            if (t.classified) {
                result.kind = t.kind;
                if (t.kind == PreviewKind::Unsupported)
                    result.error = "unsupported response type";
                return result;
            }
            if (code != CURLE_OK) {
                result.error =
                    blocked                                        ? "public-address policy"
                    : t.limited || code == CURLE_FILESIZE_EXCEEDED ? "response size limit"
                    : code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CACERT_BADFILE
                        ? "TLS verification"
                        : "HTTPS transfer failed";
                return result;
            }
            if (t.status != 200) {
                result.error = "missing HTTP response status";
                return result;
            }
            result.kind = t.kind;
            if (r.kind == PreviewKind::Link) {
                result.metadata =
                    metadata(std::string_view(reinterpret_cast<const char *>(t.bytes.data()),
                                              t.bytes.size()),
                             result.url, r.allowLocal);
                return result;
            }
            if (r.kind == PreviewKind::Image) {
                auto info = raster(t.bytes);
                if (!info) {
                    result.error = "unsupported, incomplete, or oversized image "
                                   "(animated PNG/WebP are unsupported)";
                    return result;
                }
                const auto mime = lower(trim(t.contentType.substr(0, t.contentType.find(';'))));
                const auto expected = info->extension == ".png"   ? "image/png"
                                      : info->extension == ".jpg" ? "image/jpeg"
                                      : info->extension == ".gif" ? "image/gif"
                                                                  : "image/webp";
                if (mime != expected) {
                    result.error = "image MIME/header mismatch";
                    return result;
                }
                if (std::uint64_t(info->width) * info->height > 16 * MiB) {
                    result.error = "image pixel limit";
                    return result;
                }
                result.raster = std::move(*info);
            }
            if (t.bytes.empty()) {
                result.error = "empty asset";
                return result;
            }
            result.asset = save(t.bytes, pending);
            if (!result.asset)
                result.error = "cache capacity or write failure";
            return result;
        }
        result.error = "canceled";
        return result;
    }
    std::shared_ptr<const PreviewAsset> save(std::span<const std::uint8_t> bytes, bool &pending) {
        {
            std::lock_guard lock(budget->mutex);
            if (budget->files >= slots || bytes.size() > cacheLimit - budget->bytes)
                return {};
            ++budget->files;
            budget->bytes += bytes.size();
            if (pending) {
                --budget->pending;
                pending = false;
            }
        }
        struct Reservation {
            std::shared_ptr<Budget> budget;
            std::uint64_t bytes;
            bool owned = true;
            ~Reservation() {
                if (owned) {
                    std::lock_guard lock(budget->mutex);
                    --budget->files;
                    budget->bytes -= bytes;
                }
            }
        } reservation{budget, bytes.size()};
        struct Temporary {
            std::string dir, path;
            int fd = -1;
            ~Temporary() {
                if (fd >= 0)
                    ::close(fd);
                if (!path.empty())
                    ::unlink(path.c_str());
                if (!dir.empty())
                    ::rmdir(dir.c_str());
            }
        } temp;
        std::error_code error;
        const auto root = std::filesystem::temp_directory_path(error);
        if (error)
            return {};
        temp.dir = (root / "blastirc-preview-XXXXXX").string();
        if (!::mkdtemp(temp.dir.data())) {
            temp.dir.clear();
            return {};
        }
        temp.path = temp.dir + "/asset";
        temp.fd =
            ::open(temp.path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (temp.fd < 0 || ::fchmod(temp.fd, 0600))
            return {};
        std::size_t offset = 0;
        while (offset < bytes.size() && !abort) {
            const auto n = ::write(temp.fd, bytes.data() + offset, bytes.size() - offset);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                return {};
            offset += static_cast<std::size_t>(n);
        }
        const auto closed = ::close(temp.fd);
        temp.fd = -1;
        if (closed || abort)
            return {};
        auto *asset = new PreviewAsset(temp.path, bytes.size());
        temp.path.clear();
        temp.dir.clear();
        reservation.owned = false;
        // shared_ptr invokes this deleter even if allocating its control block fails.
        return std::shared_ptr<const PreviewAsset>(asset, [state = budget](const PreviewAsset *a) {
            const auto size = a->bytes;
            delete a;
            std::lock_guard lock(state->mutex);
            --state->files;
            state->bytes -= size;
        });
    }
    void run() noexcept {
        for (;;) {
            Request r;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return stopping || !queue.empty(); });
                if (stopping)
                    return;
                r = std::move(queue.front());
                queue.pop_front();
                active = r.id;
                abort = false;
            }
            Result result;
            bool pending = r.kind != PreviewKind::Link;
            try {
                result = download(r, pending);
            } catch (...) {
                result.id = r.id;
                result.generation = r.generation;
                result.error = "preview processing failed";
            }
            if (pending)
                releasePending(1);
            {
                std::lock_guard lock(mutex);
                if (!stopping && !abort) {
                    try {
                        results.push_back(std::move(result));
                    } catch (...) {
                    }
                }
                active = 0;
            }
        }
    }
};

PreviewService::PreviewService(std::string caBundle)
    : impl_(std::make_unique<Impl>(std::move(caBundle))) {}
PreviewService::~PreviewService() = default;
PreviewService::Admission PreviewService::request(Request r) {
    if (!r.id || r.kind == PreviewKind::Unsupported || !r.maxBytes || r.maxBytes > 512 * MiB)
        return Admission::Invalid;
    auto address = url(r.url, r.allowLocal);
    if (!address)
        return Admission::Invalid;
    r.url = std::move(*address);
    try {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->available || impl_->stopping || r.id <= impl_->last)
            return Admission::Invalid;
        if (impl_->queue.size() + impl_->results.size() + (impl_->active != 0) >= slots)
            return Admission::Full;
        impl_->queue.push_back(std::move(r));
        impl_->last = impl_->queue.back().id;
        if (impl_->queue.back().kind != PreviewKind::Link) {
            std::lock_guard budgetLock(impl_->budget->mutex);
            ++impl_->budget->pending;
        }
    } catch (...) {
        return Admission::Invalid;
    }
    impl_->wake.notify_one();
    return Admission::Queued;
}
void PreviewService::cancel(std::uint64_t id) {
    std::lock_guard lock(impl_->mutex);
    std::size_t pending = 0;
    std::erase_if(impl_->queue, [&](const auto &r) {
        if (r.id != id)
            return false;
        pending += r.kind != PreviewKind::Link;
        return true;
    });
    impl_->releasePending(pending);
    std::erase_if(impl_->results, [&](const auto &r) { return r.id == id; });
    if (impl_->active == id)
        impl_->abort = true;
}
void PreviewService::cancelAll() {
    std::lock_guard lock(impl_->mutex);
    impl_->discardQueued();
    impl_->results.clear();
    impl_->abort = true;
}
std::vector<PreviewService::Result> PreviewService::poll() {
    std::lock_guard lock(impl_->mutex);
    std::vector<Result> result;
    result.swap(impl_->results);
    return result;
}
PreviewService::Usage PreviewService::usage() const {
    const auto &budget = *impl_->budget;
    std::lock_guard lock(impl_->budget->mutex);
    return {budget.files, budget.bytes, budget.pending};
}
} // namespace blast
