#include "platform/capture.hpp"
#include "platform/window.hpp"

#include <GLES2/gl2.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace blast {
Capture::Capture(Window &window, bool enabled) : window_(window), enabled_(enabled) {
    setHitTestVisible(false);
    setParticipatesInLayout(false);
}

void Capture::draw(RenderBackend &, const CustomDrawContext &) const {
    if (!enabled_)
        return;
    width_ = window_.renderTarget().bufferWidth();
    height_ = window_.renderTarget().bufferHeight();
    const std::uint64_t bytes = std::uint64_t(width_) * height_ * 4;
    if (bytes > std::numeric_limits<std::size_t>::max())
        throw std::runtime_error("Screenshot exceeds addressable memory");
    pixels_.resize(static_cast<std::size_t>(bytes));
    GLint alignment = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, pixels_.data());
    glPixelStorei(GL_PACK_ALIGNMENT, alignment);
}

void Capture::save(const std::filesystem::path &path) const {
    if (!width_ || !height_ || pixels_.empty())
        throw std::runtime_error("No rendered frame is available for screenshot");
    const std::uint64_t stride = (std::uint64_t(width_) * 3 + 3) & ~std::uint64_t(3);
    const std::uint64_t bytes = stride * height_;
    if (bytes > std::numeric_limits<std::uint32_t>::max() - 54)
        throw std::runtime_error("Screenshot exceeds BMP size limit");
    std::array<std::uint8_t, 54> header{};
    const auto u16 = [&](std::size_t offset, std::uint16_t value) {
        header[offset] = value & 0xff;
        header[offset + 1] = value >> 8;
    };
    const auto u32 = [&](std::size_t offset, std::uint32_t value) {
        for (int i = 0; i < 4; ++i)
            header[offset + i] = (value >> (8 * i)) & 0xff;
    };
    header[0] = 'B';
    header[1] = 'M';
    u32(2, static_cast<std::uint32_t>(54 + bytes));
    u32(10, 54);
    u32(14, 40);
    u32(18, width_);
    u32(22, height_);
    u16(26, 1);
    u16(28, 24);
    u32(34, static_cast<std::uint32_t>(bytes));
    std::ofstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot open screenshot: " + path.string());
    file.write(reinterpret_cast<const char *>(header.data()), header.size());
    std::vector<std::uint8_t> row(static_cast<std::size_t>(stride), 0);
    // GL and positive-height BMP both store the bottom row first. BMP RGB is straight alpha.
    for (std::uint32_t y = 0; y < height_; ++y) {
        for (std::uint32_t x = 0; x < width_; ++x) {
            const auto *pixel = pixels_.data() + (std::size_t(y) * width_ + x) * 4;
            const auto straight = [&](int component) -> std::uint8_t {
                return pixel[3] ? std::min(255u, (unsigned(pixel[component]) * 255 + pixel[3] / 2) /
                                                     pixel[3])
                                : 0;
            };
            row[x * 3] = straight(2);
            row[x * 3 + 1] = straight(1);
            row[x * 3 + 2] = straight(0);
        }
        file.write(reinterpret_cast<const char *>(row.data()), row.size());
    }
    file.close();
    if (!file)
        throw std::runtime_error("Cannot write screenshot: " + path.string());
}
} // namespace blast
