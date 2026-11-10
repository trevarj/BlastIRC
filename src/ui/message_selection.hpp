#pragma once

#include <cstddef>
#include <pango/pango.h>
#include <render/core/renderer.h>
#include <string>
#include <string_view>
#include <vector>

namespace blast {

enum class MessageSelectionSnap { Before, Nearest, After };

// The one decoded message body, including whitespace omitted by visual wrapping.
// Offsets are UTF-8 bytes. Cursor boundaries belong to the whole body, not style runs.
class MessageSelectionText {
  public:
    bool update(std::string_view text);
    std::string_view text() const { return text_; }
    const std::vector<std::size_t> &boundaries() const { return boundaries_; }
    std::size_t snap(std::size_t byte,
                     MessageSelectionSnap bias = MessageSelectionSnap::Nearest) const;

  private:
    std::string text_;
    std::vector<std::size_t> boundaries_{0};
    std::vector<PangoLogAttr> attrs_;
};

struct MessageSelectionRect {
    float x = 0, y = 0, width = 0, height = 0;
};

// Geometry of one ACTUALLY shaped fragment, not a second paragraph layout.
// Rebuild on layout invalidation: body/range/markup/font/size/wrap changes, and
// renderer textMetricsGeneration()/renderScale() changes. Hit/rectangles do not shape.
class MessageSelectionFragment {
  public:
    // [begin,end) must use UTF-8 character boundaries in body; markup must decode
    // to exactly that substring. Match the drawn fragment's font/size/wrap flags.
    void rebuild(Renderer &renderer, const MessageSelectionText &body, std::size_t begin,
                 std::size_t end, std::string_view markup, float fontSize, const TextFont &font,
                 float wrapWidth = 0, bool useMarkup = true);
    std::size_t hit(float localX, float localY) const;
    // Replaces out, retaining its storage. Endpoints are global legal boundaries.
    void rectangles(std::size_t begin, std::size_t end,
                    std::vector<MessageSelectionRect> &out) const;
    const TextMetrics &metrics() const { return metrics_; }

    // All geometry is native Pango layout-local, in logical pixels. For a native
    // TextNode, translate X by sceneX - metrics().alignOffsetX, and Y by its scene
    // baseline - floor(metrics().layoutBaseline * renderScale) / renderScale.
    // Label uses its actual textOriginX()/baselineOffset() for that same origin.
    // Apply scene transforms once; never multiply these cells/font size by DPR.
  private:
    struct Cell {
        MessageSelectionRect rect;
        std::size_t begin, end;
        bool rtl;
    };
    TextMetrics metrics_;
    std::size_t emptyHit_ = 0;
    std::vector<std::size_t> offsets_;
    std::vector<PangoLogAttr> attrs_;
    std::vector<TextCursorStop> stops_;
    std::vector<Cell> cells_;
};

} // namespace blast
