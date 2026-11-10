#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace blast {

class AvatarLoader {
  public:
    struct Result {
        std::uint64_t request;
        std::string path;    // Empty on failure; otherwise owned until remove() or destruction.
        std::string error{}; // Fixed diagnostic classification; never the URL or response text.
    };
    enum class Admission { Invalid, Full, Queued };

    static constexpr std::size_t maxBytes = 1024 * 1024;
    static constexpr std::uint32_t maxDimension = 1024;
    static constexpr std::uint64_t maxPixels = 1024 * 1024;
    static constexpr std::size_t maxQueue = 32;
    static constexpr std::size_t maxFiles = 64;

    AvatarLoader();
    ~AvatarLoader();
    AvatarLoader(const AvatarLoader &) = delete;
    AvatarLoader &operator=(const AvatarLoader &) = delete;

    // IDs must be globally increasing and nonzero. Only Queued produces a completion.
    Admission request(std::uint64_t id, const std::string &metadata);
    bool hasCapacity() const;
    std::vector<Result> poll();
    void cancel(std::uint64_t id);
    void remove(const std::string &path);

    static std::optional<std::string> url(std::string_view metadata);
    static bool publicAddress(std::string_view ip);
    // Header/structure preflight, not a replacement for the GUI's raster decoder.
    static std::optional<std::string_view> rasterExtension(std::span<const std::uint8_t> bytes);
    // Pure admission boundary: occupiedFiles includes both owned files and fetch reservations.
    static bool capacity(std::size_t outstanding, std::size_t occupiedFiles);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace blast
