#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace blast {
struct PreviewAsset;

// All calls, including frame consumption, belong to the UI thread.
class MediaPlayer {
  public:
    enum class AudioOutput { System, Null };
    explicit MediaPlayer(AudioOutput audioOutput = AudioOutput::System);
    ~MediaPlayer();
    MediaPlayer(const MediaPlayer &) = delete;
    MediaPlayer &operator=(const MediaPlayer &) = delete;
    bool load(std::shared_ptr<const PreviewAsset> asset, double maxSeconds);
    void poll();
    void playPause();
    void seek(double seconds);
    void volume(double percent);
    void close();
    bool loaded() const;
    bool ready() const;
    bool paused() const;
    bool hasVideo() const;
    double position() const;
    double duration() const;
    double volume() const;
    const std::string &error() const;
    // Retained latest frame; empty before first render or after close/load.
    std::span<const std::uint8_t> frame() const;
    std::uint64_t frameRevision() const;
    int width() const { return 960; }
    int height() const { return 540; }

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace blast
