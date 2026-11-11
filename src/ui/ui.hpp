#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <render/scene/input_area.h>
#include <string>
#include <string_view>

class Node;
class InputDispatcher;
class TextClipboard;

namespace blast {
struct Model;
enum class SidebarMode;
struct WindowActions {
    std::function<void()> minimize, toggleMaximize, close;
    std::function<void(std::uint32_t)> beginMove;
    std::function<void(std::uint32_t, std::uint32_t)> beginResize;
    std::function<bool()> maximized;
    // Disabling refuses an existing native composition; enabling resets its state.
    std::function<bool(bool)> keyboardComposeEnabled;
    // Uses the host's normal renderer/context setup; false means layout was refused.
    std::function<bool()> synchronizeLayout;
    // Coalesces work on the host's native surface frame tick.
    std::function<void()> requestFrame;
};
class Ui {
  public:
    Ui(Model &, TextClipboard &);
    void setInputDispatcher(InputDispatcher &);
    void setWindowActive(bool);
    ~Ui();
    Node &root();
    void update(float logicalWidth, float logicalHeight, float nativeRenderScale);
    // Apply the latest captured drag at the host's native surface frame tick.
    void applyPendingResize();
    // Record host configure/scale changes without laying out inside the event pump.
    void invalidateInputGeometry();
    bool synchronizeInput(std::uint32_t keySym = 0);
    bool handleKey(const InputArea::KeyData &, std::uint32_t physicalKey = 0);
    // Return true for a release belonging to a retired scene gesture.
    bool handlePointerButton(std::uint32_t button, bool pressed, float x, float y,
                             bool touch = false);
    // Retire a known older parent-scene touch before a new popup or scene mouse capture.
    void prepareMousePress();
    // Return true to isolate a new touch from an existing parent-scene mouse capture.
    bool prepareTouchPress();
    // A retired touch stays suppressed through Up followed by Leave, or cancel-only Leave.
    bool retiredTouch() const;
    bool consumeTouchLeave();
    // Metadata only, for releases/cancels already consumed by a native popup or other surface.
    void discardPointerRelease(std::uint32_t button, bool touch);
    void handlePointerMotion(float x, float y, bool touch);
    // Fresh presses/wheel supersede old reveals before synchronizing focus and dispatch.
    void beginPointerInteraction(float x, float y, bool preparePressGeometry = true);
    void setWindowActions(WindowActions);
    void execute(std::string command);
    // Admit queued intents and core selection before saving drafts and workspace.
    // Safe before and after a final core tick; does not create or lay out views.
    void flush();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
void configureNativeControlBindings();
const std::map<std::string, std::string> &defaultBindings();
std::string_view commandDisplayName(std::string_view command);
std::string commandLabel(const Model &, std::string_view label, std::string_view command);
std::string sidebarModeToggleLabel(std::string_view panel, SidebarMode preferred,
                                   SidebarMode regularEffective);
std::string bindingStroke(const InputArea::KeyData &);
} // namespace blast
