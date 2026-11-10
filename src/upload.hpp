#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace blast {
enum class UploadProvider { CrafterBin, X0At, Uguu, Litterbox, Catbox, CNet, Custom, Soju };
struct UploadConfig {
    UploadProvider provider = UploadProvider::CrafterBin;
    std::string customEndpoint = "https://crafterbin.glennstack.dev";
    std::string expiryHours = "168";
    std::string litterboxExpiry = "24h";
    bool secret = true;
    std::uint64_t maxBytes = 25ULL * 1024 * 1024;
    std::string username, password;
};
std::string_view uploadProviderToken(UploadProvider provider);
std::optional<UploadProvider> parseUploadProvider(std::string_view token);
std::string_view uploadProviderLabel(UploadProvider provider);
std::string uploadEndpoint(const UploadConfig &config);
std::uint64_t uploadProviderLimit(UploadProvider provider);
// Returns a secret-free diagnostic, or empty when valid.
std::string validateUploadConfig(const UploadConfig &config);
bool safeUploadUrl(std::string_view url, bool httpsOnly = true);
bool sameUploadAuthority(std::string_view first, std::string_view second);
bool trustedSojuUploadEndpoint(std::string_view endpoint, std::string_view accountHost,
                               std::string_view explicitlyTrustedHost = {});
struct UploadFileIdentity {
    std::uint64_t device = 0, inode = 0, size = 0;
    std::int64_t modifiedSeconds = 0, modifiedNanos = 0, changedSeconds = 0, changedNanos = 0;
    bool operator==(const UploadFileIdentity &) const = default;
};
std::optional<UploadFileIdentity> inspectUploadFile(const std::string &path);
struct UploadRequest {
    UploadConfig config;
    std::string endpoint; // Effective endpoint snapshot; empty resolves config defaults.
    std::string path;
    std::optional<UploadFileIdentity>
        reviewedFile; // Required: capture when displaying review metadata.
    std::string mime = "application/octet-stream";
    std::string tlsCaFile; // Explicit private trust store; TLS verification always remains enabled.
    // Required for Soju; caller must additionally validate source/session binding before start.
    std::string credentialHost, trustedFileHost;
};
enum class UploadError {
    None,
    Cancelled,
    InvalidRequest,
    File,
    Size,
    Network,
    Tls,
    Http,
    Response
};
struct UploadResult {
    UploadError error = UploadError::None;
    std::string url, message;
    explicit operator bool() const { return error == UploadError::None; }
};
struct UploadProgress {
    std::uint64_t sent = 0, total = 0;
    bool complete = false; // True only after the successful response URL has been validated.
};
class Uploader {
  public:
    Uploader();
    ~Uploader();
    Uploader(const Uploader &) = delete;
    Uploader &operator=(const Uploader &) = delete;
    bool start(UploadRequest request); // One job; false until its result has been consumed.
    void cancel();
    std::optional<UploadProgress> takeProgress(); // One coalesced slot, at most 10Hz plus final.
    std::optional<UploadResult> takeResult();

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace blast
