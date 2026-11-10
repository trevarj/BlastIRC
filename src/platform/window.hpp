#pragma once

#include <cstdint>
#include <functional>
#include <utility>
#include <wayland/surface.h>

struct xdg_surface;
struct xdg_toplevel;
struct wl_array;

namespace blast {
// The SDK Surface owns scaling, EGL and scheduling; this class owns only its xdg-shell role.
class Window final : public Surface {
  public:
    explicit Window(WaylandConnection &, std::uint32_t width = 1440, std::uint32_t height = 940);
    ~Window() override;
    bool initialize() override;
    // Native logical height; never below 480. Compositor configure sizes remain authoritative.
    void setMinimumHeight(std::uint32_t height);
    void minimize();
    void toggleMaximize();
    void beginMove(std::uint32_t serial);
    void beginResize(std::uint32_t serial, std::uint32_t edge);
    bool maximized() const noexcept { return maximized_; }
    bool activated() const noexcept { return activated_; }
    bool resizing() const noexcept { return resizing_; }
    bool suspended() const noexcept { return suspended_; }
    xdg_surface *xdgSurface() const noexcept { return xdgSurface_; }
    void setClosedCallback(std::function<void()> callback) { closed_ = std::move(callback); }
    void setRenderedCallback(std::function<void()> callback) { rendered_ = std::move(callback); }

  private:
    void render() override;
    void destroyRole();
    static void configure(void *, xdg_surface *, std::uint32_t);
    static void configureToplevel(void *, xdg_toplevel *, std::int32_t, std::int32_t, wl_array *);
    static void closeToplevel(void *, xdg_toplevel *);
    static void configureBounds(void *, xdg_toplevel *, std::int32_t, std::int32_t);
    static void capabilities(void *, xdg_toplevel *, wl_array *);
    xdg_surface *xdgSurface_ = nullptr;
    xdg_toplevel *toplevel_ = nullptr;
    std::uint32_t width_, height_, normalWidth_, normalHeight_;
    std::uint32_t minimumHeight_ = 480;
    bool maximized_ = false, fullscreen_ = false, activated_ = false, resizing_ = false,
         suspended_ = false;
    bool pendingMaximized_ = false, pendingFullscreen_ = false, pendingActivated_ = false;
    bool pendingResizing_ = false, pendingSuspended_ = false;
    std::function<void()> closed_, rendered_;
};
} // namespace blast
