#pragma once

#include "render/core/image_decoder.h"
#include "render/core/image_resize.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

// The opt-in codec pack (noctalia_ui_codecs_dep): Wuffs (PNG, JPEG, GIF, BMP, ...), ICO, WebP, JPEG XL,
// SVG through librsvg, and HEIF/AVIF through a runtime-loaded libheif.
namespace codecs {

  // Makes this pack the process image decoder (setImageDecoder()).
  void installImageDecoder();

  std::expected<DecodedRasterImage, std::string> decodeRasterImage(const std::uint8_t* data, std::size_t size);

  std::expected<LoadedImageFile, std::string>
  loadImageFile(const std::string& path, int targetSize = 0, bool centerSquareCrop = false);

  struct DecodedRasterFrame {
    std::vector<std::uint8_t> rgba; // canvas-size, RGBA8 non-premul
    std::uint32_t durationMs = 0;
  };

  struct DecodedRasterAnimation {
    int width = 0;
    int height = 0;
    std::uint32_t loops = 0; // Zero repeats indefinitely.
    std::vector<DecodedRasterFrame> frames;
    bool truncated = false; // hit cap; later frames omitted
  };

  // Decode an animated GIF into RGBA frames with composited disposal/blend.
  // The frame/byte caps apply to returned frames, optionally resized after compositing.
  // If a cap is exceeded, `truncated` is set and the partial frames are returned.
  std::expected<DecodedRasterAnimation, std::string> decodeAnimatedGif(
      const std::uint8_t* data, std::size_t size, int maxFrames, std::size_t maxRgbaBytes, ImageExtent target = {}
  );

} // namespace codecs
