#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace blast {
enum class PreviewKind { Link, Image, Audio, Video, Unsupported };
struct PreviewMetadata {
    std::string title, description, imageUrl;
};
struct PreviewRaster {
    std::uint32_t width = 0, height = 0;
    std::string extension;
};
// Private cache lifetime follows the last asset owner, including an open player.
struct PreviewAsset {
    const std::string path;
    const std::uint64_t bytes;
    ~PreviewAsset();
    PreviewAsset(std::string path, std::uint64_t bytes);
    PreviewAsset(const PreviewAsset &) = delete;
    PreviewAsset &operator=(const PreviewAsset &) = delete;
};
class PreviewService {
  public:
    struct Request {
        // IDs must increase across the service lifetime; generation is echoed unchanged.
        std::uint64_t id = 0, generation = 0;
        std::string url;
        PreviewKind kind = PreviewKind::Link;
        std::uint64_t maxBytes = 64ULL * 1024 * 1024;
        // Only the explicit advanced Settings.previewAllowLocal opt-in may enable this.
        bool allowLocal = false;
    };
    struct Result {
        std::uint64_t id = 0, generation = 0;
        // A mismatched response type is classified from headers without consuming its body.
        // Callers must independently admit a new request under the matching privacy mode.
        PreviewKind kind = PreviewKind::Unsupported;
        std::string url, host, error;
        PreviewMetadata metadata;
        PreviewRaster raster;
        std::shared_ptr<const PreviewAsset> asset;
    };
    enum class Admission { Queued, Invalid, Full };
    static constexpr std::size_t maxAssets = 16;
    static constexpr std::uint64_t maxAssetBytes = 512ULL * 1024 * 1024;
    struct Usage {
        std::size_t assets;
        std::uint64_t bytes;
        std::size_t pendingAssets;
    };
    // Empty uses platform trust. A custom PEM CA bundle never disables peer/host checks.
    explicit PreviewService(std::string caBundle = {});
    ~PreviewService();
    PreviewService(const PreviewService &) = delete;
    PreviewService &operator=(const PreviewService &) = delete;
    // Admission is bounded to 16 outstanding operations; assets retain up to 16 files/
    // 512 MiB until their last owner releases them. No caller-visible file is evicted.
    Admission request(Request request);
    void cancel(std::uint64_t id);
    void cancelAll();
    std::vector<Result> poll();
    // Asset and pending counts are disjoint in one atomic budget snapshot. Assets include
    // writes/unpolled results/player pins; pendingAssets covers queued/downloading non-Link
    // requests until completion/cancellation or atomic conversion into an asset.
    Usage usage() const;
    static std::optional<std::string> url(std::string_view value, bool allowLocal = false);
    static PreviewKind classify(std::string_view contentType);
    // Container/dimension preflight only, not pixel decoding. PNG/JPEG/static WebP and GIF;
    // GIF consumers must use bounded first-frame decoding, never autoplay. Animated PNG/
    // WebP remain unsupported. Downloads additionally require matching MIME and <=16 Mi pixels.
    static std::optional<PreviewRaster> raster(std::span<const std::uint8_t> bytes);
    static PreviewMetadata metadata(std::string_view html, std::string_view baseUrl,
                                    bool allowLocal = false);
    // Canonical YouTube watch links use bounded official metadata, never embed HTML.
    // The endpoint remains a Link request; its thumbnail requires separate Image consent.
    static std::optional<std::string> youtubeMetadataUrl(std::string_view value);
    static std::optional<PreviewMetadata> youtubeMetadata(std::string_view json,
                                                          bool allowLocal = false);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace blast
