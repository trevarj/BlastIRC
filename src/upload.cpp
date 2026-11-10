#include "upload.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <curl/curl.h>
#include <fcntl.h>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace blast {
namespace {
constexpr std::array tokens{"crafterbin", "x0at", "uguu",   "litterbox",
                            "catbox",     "cnet", "custom", "soju"};
constexpr std::array labels{"CrafterBin", "x0.at",           "Uguu",   "Litterbox",
                            "Catbox",     "paste.c-net.org", "Custom", "Soju"};
constexpr std::array endpoints{"https://crafterbin.glennstack.dev",
                               "https://x0.at",
                               "https://uguu.se/upload",
                               "https://litterbox.catbox.moe/resources/internals/api.php",
                               "https://catbox.moe/user/api.php",
                               "https://paste.c-net.org",
                               "",
                               ""};
bool clean(std::string_view s) {
    return std::none_of(s.begin(), s.end(), [](unsigned char c) { return c <= 32 || c == 127; });
}
struct Url {
    CURLU *handle = curl_url();
    explicit Url(std::string_view value) {
        if (!handle || !clean(value) ||
            curl_url_set(handle, CURLUPART_URL, std::string(value).c_str(), 0)) {
            if (handle)
                curl_url_cleanup(handle);
            handle = nullptr;
        }
    }
    ~Url() {
        if (handle)
            curl_url_cleanup(handle);
    }
    std::string part(CURLUPart part, unsigned flags = 0) const {
        char *p = nullptr;
        if (!handle || curl_url_get(handle, part, &p, flags))
            return {};
        std::string value(p);
        curl_free(p);
        return value;
    }
};
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}
std::string trim(std::string_view s) {
    while (!s.empty() &&
           (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
        s.remove_prefix(1);
    while (!s.empty() &&
           (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.remove_suffix(1);
    return std::string(s);
}
bool sameFile(const struct stat &a, const struct stat &b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
           a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
           a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
UploadFileIdentity identity(const struct stat &s) {
    return {static_cast<std::uint64_t>(s.st_dev),
            static_cast<std::uint64_t>(s.st_ino),
            static_cast<std::uint64_t>(s.st_size),
            s.st_mtim.tv_sec,
            s.st_mtim.tv_nsec,
            s.st_ctim.tv_sec,
            s.st_ctim.tv_nsec};
}
UploadResult fail(UploadError error, std::string message) {
    return {error, {}, std::move(message)};
}
} // namespace
std::optional<UploadFileIdentity> inspectUploadFile(const std::string &path) {
    if (path.find('\0') != path.npos)
        return {};
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0)
        return {};
    struct stat s{};
    bool valid = !fstat(fd, &s) && S_ISREG(s.st_mode) && s.st_size >= 0;
    close(fd);
    return valid ? std::optional<UploadFileIdentity>(identity(s)) : std::nullopt;
}
std::string_view uploadProviderToken(UploadProvider p) {
    auto i = static_cast<unsigned>(p);
    return i < tokens.size() ? tokens[i] : "";
}
std::string_view uploadProviderLabel(UploadProvider p) {
    auto i = static_cast<unsigned>(p);
    return i < labels.size() ? labels[i] : "Unknown";
}
std::optional<UploadProvider> parseUploadProvider(std::string_view token) {
    for (unsigned i = 0; i < tokens.size(); ++i)
        if (tokens[i] == token)
            return static_cast<UploadProvider>(i);
    return {};
}
std::string uploadEndpoint(const UploadConfig &c) {
    if (c.provider == UploadProvider::Custom)
        return c.customEndpoint;
    auto i = static_cast<unsigned>(c.provider);
    return i < endpoints.size() ? endpoints[i] : "";
}
std::uint64_t uploadProviderLimit(UploadProvider p) {
    return (p == UploadProvider::X0At     ? 1024ULL
            : p == UploadProvider::Custom ? 512ULL
                                          : 25ULL) *
           1024 * 1024;
}
bool safeUploadUrl(std::string_view value, bool httpsOnly) {
    if (value.empty() || !clean(value) || value.find('\\') != value.npos)
        return false;
    // Encoded controls are unsafe when another client decodes an inserted link.
    for (std::size_t i = 0; i + 2 < value.size(); ++i)
        if (value[i] == '%') {
            auto hex = [](unsigned char c) {
                return c >= '0' && c <= '9'   ? c - '0'
                       : c >= 'a' && c <= 'f' ? c - 'a' + 10
                       : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                              : -1;
            };
            int a = hex(value[i + 1]), b = hex(value[i + 2]);
            if (a >= 0 && b >= 0 && (a * 16 + b < 32 || a * 16 + b == 127))
                return false;
        }
    Url u(value);
    return u.handle && !u.part(CURLUPART_HOST).empty() && u.part(CURLUPART_USER).empty() &&
           u.part(CURLUPART_PASSWORD).empty() && value.find('@') == value.npos &&
           (u.part(CURLUPART_SCHEME) == "https" ||
            (!httpsOnly && u.part(CURLUPART_SCHEME) == "http"));
}
bool sameUploadAuthority(std::string_view a, std::string_view b) {
    if (!safeUploadUrl(a) || !safeUploadUrl(b))
        return false;
    Url x(a), y(b);
    return lower(x.part(CURLUPART_HOST)) == lower(y.part(CURLUPART_HOST)) &&
           x.part(CURLUPART_PORT, CURLU_DEFAULT_PORT) == y.part(CURLUPART_PORT, CURLU_DEFAULT_PORT);
}
bool trustedSojuUploadEndpoint(std::string_view endpoint, std::string_view accountHost,
                               std::string_view trusted) {
    if (!safeUploadUrl(endpoint) || accountHost.empty() || !clean(accountHost))
        return false;
    Url u(endpoint);
    auto host = lower(u.part(CURLUPART_HOST)), base = lower(std::string(accountHost));
    if (host == base || (!trusted.empty() && host == lower(std::string(trusted))))
        return true;
    in_addr ipv4{};
    in6_addr ipv6{};
    auto address = base;
    if (address.starts_with('[') && address.ends_with(']'))
        address = address.substr(1, address.size() - 2);
    if (inet_pton(AF_INET, address.c_str(), &ipv4) == 1 ||
        inet_pton(AF_INET6, address.c_str(), &ipv6) == 1)
        return false;
    return host.size() > base.size() + 1 && host.ends_with("." + base);
}
std::string validateUploadConfig(const UploadConfig &c) {
    if (uploadProviderToken(c.provider).empty())
        return "Unknown upload provider.";
    if (!c.maxBytes || c.maxBytes > uploadProviderLimit(c.provider))
        return "Upload size limit exceeds this provider's limit.";
    if (!c.expiryHours.empty() &&
        (c.expiryHours.size() > 8 || !std::all_of(c.expiryHours.begin(), c.expiryHours.end(),
                                                  [](unsigned char x) { return std::isdigit(x); })))
        return "Expiry must be hours or empty.";
    if (c.litterboxExpiry != "1h" && c.litterboxExpiry != "12h" && c.litterboxExpiry != "24h" &&
        c.litterboxExpiry != "72h")
        return "Unsupported Litterbox expiry.";
    if (c.provider == UploadProvider::Custom &&
        (!safeUploadUrl(c.customEndpoint) || c.customEndpoint.find('#') != c.customEndpoint.npos))
        return "Custom endpoint must be HTTPS without URL credentials, fragments or controls.";
    if (c.username.find(':') != std::string::npos || (!c.username.empty() && !clean(c.username)))
        return "Invalid upload username.";
    if (c.username.find('\0') != c.username.npos || c.password.find('\0') != c.password.npos)
        return "Invalid upload credentials.";
    return {};
}
struct Uploader::State {
    std::mutex mutex;
    std::atomic<bool> cancelled{false};
    bool busy = false;
    std::optional<UploadProgress> progress;
    std::optional<UploadResult> result;
    std::thread worker;
    void publish(std::uint64_t sent, std::uint64_t total, bool complete = false) {
        std::lock_guard lock(mutex);
        progress = UploadProgress{sent, total, complete};
    }
    struct Transfer {
        State &state;
        int fd;
        struct stat initial;
        std::uint64_t limit, readBytes = 0;
        bool readError = false, changed = false, oversized = false, overflow = false;
        bool locationSeen = false, ambiguousLocation = false;
        std::size_t received = 0;
        std::string body{}, location{};
        std::chrono::steady_clock::time_point last{};
        static size_t read(char *data, size_t size, size_t count, void *opaque) {
            auto &t = *static_cast<Transfer *>(opaque);
            if (t.state.cancelled)
                return CURL_READFUNC_ABORT;
            struct stat current{};
            if (fstat(t.fd, &current) || !sameFile(t.initial, current)) {
                t.changed = true;
                return CURL_READFUNC_ABORT;
            }
            auto remaining = static_cast<std::uint64_t>(t.initial.st_size) - t.readBytes;
            auto amount = std::min<std::uint64_t>(size * count, remaining);
            if (t.readBytes + amount > t.limit) {
                t.oversized = true;
                return CURL_READFUNC_ABORT;
            }
            if (!amount)
                return 0;
            ssize_t n;
            do {
                n = ::read(t.fd, data, amount);
            } while (n < 0 && errno == EINTR);
            if (n <= 0) {
                t.readError = true;
                return CURL_READFUNC_ABORT;
            }
            t.readBytes += n;
            return static_cast<size_t>(n);
        }
        static int seek(void *, curl_off_t, int) { return CURL_SEEKFUNC_CANTSEEK; }
        static size_t write(char *data, size_t size, size_t count, void *opaque) {
            auto &t = *static_cast<Transfer *>(opaque);
            auto n = size * count;
            if (n > 65536 - t.received) {
                t.overflow = true;
                return 0;
            }
            t.received += n;
            t.body.append(data, n);
            return n;
        }
        static size_t header(char *data, size_t size, size_t count, void *opaque) {
            auto &t = *static_cast<Transfer *>(opaque);
            auto n = size * count;
            if (n > 65536 - t.received) {
                t.overflow = true;
                return 0;
            }
            t.received += n;
            std::string_view line(data, n);
            if (line.starts_with("HTTP/")) {
                t.location.clear();
                t.locationSeen = false;
            }
            auto colon = line.find(':');
            if (colon != line.npos && lower(std::string(line.substr(0, colon))) == "location") {
                if (t.locationSeen) {
                    t.ambiguousLocation = true;
                    return 0;
                }
                t.locationSeen = true;
                t.location = trim(line.substr(colon + 1));
            }
            return n;
        }
        static int progress(void *opaque, curl_off_t, curl_off_t, curl_off_t total,
                            curl_off_t sent) {
            auto &t = *static_cast<Transfer *>(opaque);
            if (t.state.cancelled)
                return 1;
            auto now = std::chrono::steady_clock::now();
            if (now - t.last >= std::chrono::milliseconds(100)) {
                // MIME framing isn't file progress: report actual bytes consumed from the fd.
                t.state.publish(t.readBytes, t.initial.st_size);
                t.last = now;
            }
            (void)total;
            (void)sent;
            return 0;
        }
    };
    UploadResult run(const UploadRequest &request) {
        const auto &c = request.config;
        if (auto error = validateUploadConfig(c); !error.empty())
            return fail(UploadError::InvalidRequest, error);
        auto endpoint = request.endpoint.empty() ? uploadEndpoint(c) : request.endpoint;
        if (!safeUploadUrl(endpoint) || endpoint.find('#') != endpoint.npos)
            return fail(
                UploadError::InvalidRequest,
                "Upload endpoint must be HTTPS without URL credentials, fragments or controls.");
        if (c.provider == UploadProvider::Soju &&
            (c.username.empty() ||
             !trustedSojuUploadEndpoint(endpoint, request.credentialHost, request.trustedFileHost)))
            return fail(UploadError::InvalidRequest,
                        "Soju file host is not authorized for this account.");
        if (c.provider == UploadProvider::Custom && (!c.username.empty() || !c.password.empty()) &&
            !sameUploadAuthority(c.customEndpoint, endpoint))
            return fail(UploadError::InvalidRequest,
                        "Custom upload credentials belong to a different endpoint authority.");
        if (request.mime.empty() || !clean(request.mime))
            return fail(UploadError::InvalidRequest, "Invalid file media type.");
        if (request.path.find('\0') != request.path.npos)
            return fail(UploadError::File, "Invalid selected file path.");
        int fd = open(request.path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
        if (fd < 0)
            return fail(UploadError::File, "Cannot open the selected regular file.");
        struct Fd {
            int value;
            ~Fd() { close(value); }
        } file{fd};
        struct stat initial{};
        if (fstat(fd, &initial) || !S_ISREG(initial.st_mode) || initial.st_size < 0)
            return fail(UploadError::File, "Selected path is not a readable regular file.");
        if (!request.reviewedFile || *request.reviewedFile != identity(initial))
            return fail(UploadError::File, "Selected file changed since review; select it again.");
        if (static_cast<std::uint64_t>(initial.st_size) > c.maxBytes)
            return fail(UploadError::Size, "File exceeds the configured upload size limit.");
        auto name = request.path.substr(request.path.find_last_of('/') + 1);
        if (name.empty() || std::any_of(name.begin(), name.end(),
                                        [](unsigned char x) { return x < 32 || x == 127; }))
            return fail(UploadError::File, "File name contains unsafe control characters.");
        static const auto global = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (global != CURLE_OK)
            return fail(UploadError::Network, "HTTP transport initialization failed.");
        CURL *curl = curl_easy_init();
        if (!curl)
            return fail(UploadError::Network, "HTTP transport initialization failed.");
        struct Handles {
            CURL *curl;
            curl_mime *mime = nullptr;
            curl_slist *headers = nullptr;
            ~Handles() {
                curl_mime_free(mime);
                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
            }
        } h{curl};
        Transfer t{*this, fd, initial, c.maxBytes};
        curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 0L);
        curl_easy_setopt(curl, CURLOPT_NETRC, CURL_NETRC_IGNORED);
        curl_easy_setopt(curl, CURLOPT_PROXY,
                         ""); // No ambient proxy may receive upload credentials or file bytes.
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        const char *ca =
            request.tlsCaFile.empty() ? std::getenv("CURL_CA_BUNDLE") : request.tlsCaFile.c_str();
        if (!ca || !*ca)
            ca = std::getenv("SSL_CERT_FILE");
        if (ca && *ca)
            curl_easy_setopt(curl, CURLOPT_CAINFO, ca);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3600L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &Transfer::write);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &Transfer::header);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &t);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &Transfer::progress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        if (!c.username.empty() &&
            (c.provider == UploadProvider::Custom || c.provider == UploadProvider::Soju)) {
            curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
            curl_easy_setopt(curl, CURLOPT_USERNAME, c.username.c_str());
            curl_easy_setopt(curl, CURLOPT_PASSWORD, c.password.c_str());
        }
        auto header = [&](const std::string &s) {
            h.headers = curl_slist_append(h.headers, s.c_str());
        };
        header("Expect:");
        if (c.provider == UploadProvider::CNet || c.provider == UploadProvider::Soju) {
            curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST,
                             c.provider == UploadProvider::CNet ? "PUT" : "POST");
            curl_easy_setopt(curl, CURLOPT_READFUNCTION, &Transfer::read);
            curl_easy_setopt(curl, CURLOPT_READDATA, &t);
            curl_easy_setopt(curl, CURLOPT_SEEKFUNCTION, &Transfer::seek);
            curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE,
                             static_cast<curl_off_t>(initial.st_size));
            header("Content-Type: " + request.mime);
            if (c.provider == UploadProvider::CNet) {
                header("X-FileName: " + name);
                header("X-UUID;");
                header("Accept: application/json");
            } else {
                std::string quoted;
                for (char ch : name) {
                    if (ch == '"' || ch == '\\')
                        quoted += '\\';
                    quoted += ch;
                }
                header("Content-Disposition: attachment; filename=\"" + quoted + "\"");
            }
        } else {
            h.mime = curl_mime_init(curl);
            auto field = [&](const char *key, const std::string &value) {
                auto *p = curl_mime_addpart(h.mime);
                curl_mime_name(p, key);
                curl_mime_data(p, value.data(), value.size());
            };
            auto *part = curl_mime_addpart(h.mime);
            curl_mime_name(part, c.provider == UploadProvider::Uguu ? "files[]"
                                 : (c.provider == UploadProvider::Catbox ||
                                    c.provider == UploadProvider::Litterbox)
                                     ? "fileToUpload"
                                     : "file");
            curl_mime_filename(part, name.c_str());
            curl_mime_type(part, request.mime.c_str());
            curl_mime_data_cb(part, initial.st_size, &Transfer::read, &Transfer::seek, nullptr, &t);
            if (c.provider == UploadProvider::CrafterBin || c.provider == UploadProvider::Custom) {
                if (c.secret)
                    field("secret", "");
                if (!c.expiryHours.empty())
                    field("expires", c.expiryHours);
            }
            if (c.provider == UploadProvider::Custom) {
                field("expiration", "never");
                field("burn_after", "0");
                field("privacy", "unlisted");
                field("syntax_highlight", "none");
                if (!c.password.empty())
                    field("uploader_password", c.password);
            }
            if (c.provider == UploadProvider::Catbox || c.provider == UploadProvider::Litterbox)
                field("reqtype", "fileupload");
            if (c.provider == UploadProvider::Litterbox)
                field("time", c.litterboxExpiry);
            curl_easy_setopt(curl, CURLOPT_MIMEPOST, h.mime);
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h.headers);
        auto code = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        struct stat finalStat{};
        // Cancellation cannot retract a completed HTTP exchange: preserve a valid returned URL
        // for the caller's recovery UI rather than pretending no public upload occurred.
        if (cancelled && code != CURLE_OK)
            return fail(UploadError::Cancelled, "Upload cancelled.");
        if (fstat(fd, &finalStat) || !sameFile(initial, finalStat) || t.changed)
            return fail(UploadError::File, "File changed during upload; select it again.");
        if (t.readError)
            return fail(UploadError::File, "File could not be read completely.");
        if (t.oversized)
            return fail(UploadError::Size, "File exceeds the configured upload size limit.");
        if (t.ambiguousLocation || code == CURLE_WEIRD_SERVER_REPLY)
            return fail(UploadError::Response,
                        "Upload server returned malformed or ambiguous response headers.");
        if (t.overflow)
            return fail(UploadError::Response, "Upload response exceeds the safe response limit.");
        if (code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CONNECT_ERROR ||
            code == CURLE_SSL_CACERT_BADFILE)
            return fail(UploadError::Tls,
                        "Upload TLS certificate verification or handshake failed.");
        if (code != CURLE_OK)
            return fail(UploadError::Network,
                        "Upload connection failed or timed out; retry explicitly.");
        if (t.readBytes != static_cast<std::uint64_t>(initial.st_size))
            return fail(UploadError::File, "Server did not consume the complete file.");
        bool customRedirect = c.provider == UploadProvider::Custom && status >= 300 && status < 400;
        if ((!customRedirect && (status < 200 || status >= 300)) ||
            (c.provider == UploadProvider::Soju && status != 201))
            return fail(UploadError::Http,
                        "Upload server returned HTTP " + std::to_string(status) + ".");
        std::string result;
        if ((c.provider == UploadProvider::Custom || c.provider == UploadProvider::Soju) &&
            !t.location.empty()) {
            Url resolved(endpoint);
            if (curl_url_set(resolved.handle, CURLUPART_URL, t.location.c_str(), 0))
                return fail(UploadError::Response, "Upload server returned an invalid Location.");
            result = resolved.part(CURLUPART_URL);
            if (!safeUploadUrl(result))
                return fail(UploadError::Response, "Upload server returned an unsafe Location.");
            if (c.provider == UploadProvider::Custom) {
                if (!sameUploadAuthority(endpoint, result))
                    return fail(UploadError::Response,
                                "Custom upload redirected to a different authority.");
                auto path = resolved.part(CURLUPART_PATH);
                if (path == "/incorrect" || path.starts_with("/incorrect/"))
                    return fail(UploadError::Http, "Upload password was rejected.");
                if (path.starts_with("/upload/") && path.size() > 8) {
                    path.replace(0, 8, "/file/");
                    curl_url_set(resolved.handle, CURLUPART_PATH, path.c_str(), 0);
                    result = resolved.part(CURLUPART_URL);
                } else if (customRedirect)
                    return fail(UploadError::Response,
                                "Custom upload returned an unsupported redirect.");
            }
        } else if (c.provider == UploadProvider::Soju || customRedirect)
            return fail(UploadError::Response, "Upload response is missing Location.");
        else if (c.provider == UploadProvider::Uguu || c.provider == UploadProvider::CNet) {
            auto json = nlohmann::json::parse(
                t.body,
                [](int depth, nlohmann::json::parse_event_t, nlohmann::json &) {
                    if (depth > 16)
                        throw std::length_error("JSON nesting limit");
                    return true;
                },
                false);
            if (!json.is_discarded()) {
                if (c.provider == UploadProvider::CNet && json.is_object() &&
                    json.contains("url") && json["url"].is_string())
                    result = json["url"].get<std::string>();
                if (c.provider == UploadProvider::Uguu && json.is_object() &&
                    json.contains("files") && json["files"].is_array() && !json["files"].empty()) {
                    auto &f = json["files"][0];
                    if (f.is_object() && f.contains("url") && f["url"].is_string())
                        result = f["url"].get<std::string>();
                }
            }
        } else {
            std::string_view lines(t.body);
            while (!lines.empty()) {
                auto end = lines.find('\n');
                auto line = trim(lines.substr(0, end));
                if (line.starts_with("https://")) {
                    result = line;
                    break;
                }
                if (end == lines.npos)
                    break;
                lines.remove_prefix(end + 1);
            }
        }
        if (!safeUploadUrl(result))
            return fail(UploadError::Response,
                        "Upload server did not return a safe HTTPS file URL.");
        publish(initial.st_size, initial.st_size, true);
        return {UploadError::None, std::move(result), {}};
    }
};
Uploader::Uploader() : state_(std::make_unique<State>()) {}
Uploader::~Uploader() {
    cancel();
    if (state_->worker.joinable())
        state_->worker.join();
}
bool Uploader::start(UploadRequest request) {
    std::lock_guard lock(state_->mutex);
    if (state_->busy)
        return false;
    if (state_->worker.joinable())
        state_->worker.join();
    state_->busy = true;
    state_->cancelled = false;
    state_->progress.reset();
    state_->worker = std::thread([s = state_.get(), request = std::move(request)] {
        UploadResult result;
        try {
            result = s->run(request);
        } catch (...) {
            result = fail(UploadError::Response,
                          "Upload failed while processing the request or response.");
        }
        std::lock_guard guard(s->mutex);
        s->result = std::move(result);
    });
    return true;
}
void Uploader::cancel() { state_->cancelled = true; }
std::optional<UploadProgress> Uploader::takeProgress() {
    std::lock_guard lock(state_->mutex);
    auto p = state_->progress;
    state_->progress.reset();
    return p;
}
std::optional<UploadResult> Uploader::takeResult() {
    std::lock_guard lock(state_->mutex);
    auto r = std::move(state_->result);
    state_->result.reset();
    if (r)
        state_->busy = false;
    return r;
}
} // namespace blast
