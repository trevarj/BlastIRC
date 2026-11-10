#pragma once
#include "../model.hpp"
class Node;
#include <memory>
#include <string_view>

namespace blast {
// Ui registers only visible rows. Entries expire each frame, so clipped/offscreen
// messages never spray particles onto an unrelated row.
void fxAnchor(std::string_view server, std::string_view target, std::string_view id, float x,
              float y, float width, float height);
// Register each visible transcript viewport before its rows, in scene logical pixels.
void fxViewport(float x, float y, float width, float height);
class Effects {
  public:
    // Owns MIX_Init/MIX_Quit and the SDL audio device; never initializes SDL video.
    Effects();
    ~Effects();
    void attach(Node &root);
    void draw(Model &);

  private:
    friend class EffectsTestAccess;
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace blast
