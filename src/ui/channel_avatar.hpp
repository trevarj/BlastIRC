#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <render/core/image_decoder.h>
#include <render/core/image_resize.h>
#include <render/core/renderer.h>
#include <render/core/texture_manager.h>
#include <string>
#include <string_view>
#include <ui/controls/image.h>
#include <ui/controls/label.h>

namespace blast {

inline std::string nativeAvatarInitials(std::string_view name, bool channel = false) {
    if (channel) {
        const auto first = name.find_first_not_of("#&+!");
        if (first != std::string_view::npos)
            name.remove_prefix(first);
    }
    std::string result;
    const auto append = [&](std::size_t at) {
        if (at >= name.size())
            return;
        std::size_t end = at + 1;
        while (end < name.size() && (static_cast<unsigned char>(name[end]) & 0xc0) == 0x80)
            ++end;
        if (end == at + 1)
            result += char(std::toupper(static_cast<unsigned char>(name[at])));
        else
            result.append(name.substr(at, end - at));
    };
    const auto first = name.find_first_not_of(" \t-_");
    if (first == std::string_view::npos)
        return "?";
    append(first);
    const auto last = name.find_last_of(" \t-_");
    if (last != std::string_view::npos && last + 1 < name.size())
        append(last + 1);
    else
        append(first + result.size());
    return result;
}

class NativeAvatarLabelPlacement {
  public:
    void place(Label &label, Renderer &renderer, float x, float y, float width, float height) {
        label.setMaxWidth(width);
        label.measure(renderer);
        const float wrap = label.textWrapWidth();
        if (!valid || text != label.text() || font != label.font() || size != label.fontSize() ||
            wrapWidth != wrap || align != label.textAlign() || ellipsize != label.ellipsize() ||
            renderScale != renderer.renderScale() ||
            generation != renderer.textMetricsGeneration()) {
            text = label.text();
            font = label.font();
            size = label.fontSize();
            wrapWidth = wrap;
            align = label.textAlign();
            ellipsize = label.ellipsize();
            renderScale = renderer.renderScale();
            generation = renderer.textMetricsGeneration();
            metrics = renderer.measureText(text, size, font, wrapWidth, 1, align, ellipsize, false);
            valid = metrics.lineCount > 0;
        }
        // Rasterization removes Pango's layout alignment offset from the draw origin.
        const float inkCenterX = (metrics.inkLeft + metrics.inkRight) / 2 - metrics.alignOffsetX;
        label.setPosition(x + width / 2 - label.textOriginX() - inkCenterX,
                          y + height / 2 - label.baselineOffset() -
                              (metrics.inkTop + metrics.inkBottom) / 2);
    }

  private:
    std::string text;
    TextFont font;
    TextMetrics metrics;
    float size = 0, wrapWidth = 0, renderScale = 0;
    std::uint64_t generation = 0;
    TextAlign align = TextAlign::Start;
    TextEllipsize ellipsize = TextEllipsize::End;
    bool valid = false;
};

// Only pass Buffer::avatarPath, the internally fetched and validated private cache path.
// Raster byte decoding deliberately bypasses Image's file/SVG sniffing.
inline void loadChannelAvatarRaster(Image &image, Renderer &renderer,
                                    const std::string &trustedPath, int target) {
    image.clear(renderer);
    if (trustedPath.empty())
        return;
    std::ifstream input(trustedPath, std::ios::binary | std::ios::ate);
    const std::streamoff size = input.tellg();
    if (!input || size <= 0 || size > 1024 * 1024)
        return;
    const auto length = static_cast<std::size_t>(size);
    auto bytes = std::make_unique_for_overwrite<std::uint8_t[]>(length);
    input.seekg(0);
    if (!input.read(reinterpret_cast<char *>(bytes.get()), static_cast<std::streamsize>(length)))
        return;
    auto decoded = decodeRasterImage(bytes.get(), length);
    if (!decoded || decoded->width <= 0 || decoded->height <= 0)
        return;
    const int side = std::min(decoded->width, decoded->height);
    const auto rowBytes = static_cast<std::size_t>(side) * 4;
    if (decoded->width != decoded->height) {
        // Center-crop in place before downsampling; no second full-size pixel buffer.
        const int x = (decoded->width - side) / 2;
        const int y = (decoded->height - side) / 2;
        for (int row = 0; row < side; ++row) {
            const auto source = (static_cast<std::size_t>(y + row) * decoded->width + x) * 4;
            std::memmove(decoded->pixels.data() + static_cast<std::size_t>(row) * rowBytes,
                         decoded->pixels.data() + source, rowBytes);
        }
    }
    const int outputSide = std::min(side, target);
    if (outputSide == side) {
        image.setSourceRaw(renderer, decoded->pixels.data(), rowBytes * side, side, side, 0,
                           PixmapFormat::RGBA);
        return;
    }
    auto resized = resizeToRgba(decoded->pixels.data(), side, side, ResizeSourceLayout::Rgba,
                                outputSide, outputSide);
    if (!resized.empty())
        image.setSourceRaw(renderer, resized.data(), resized.size(), outputSide, outputSide, 0,
                           PixmapFormat::RGBA);
}

} // namespace blast
