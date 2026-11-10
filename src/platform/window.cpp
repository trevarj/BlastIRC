#include "platform/window.hpp"

#include <algorithm>
#include <limits>
#include <wayland-client.h>
#include <wayland/wayland_connection.h>
#include <xdg-shell-client-protocol.h>

namespace blast {
Window::Window(WaylandConnection &connection, std::uint32_t width, std::uint32_t height)
    : Surface(connection), width_(width), height_(std::max(height, 480u)), normalWidth_(width),
      normalHeight_(height_) {
    setDebugName("BlastIRC");
}

Window::~Window() {
    m_connection.unregisterSurface(m_surface);
    destroyRole();
}

bool Window::initialize() {
    if (!m_connection.hasXdgShell() || !createWlSurface())
        return false;
    setBufferScale(1);
    xdgSurface_ = xdg_wm_base_get_xdg_surface(m_connection.xdgWmBase(), m_surface);
    if (!xdgSurface_)
        return false;
    static const xdg_surface_listener surfaceListener = {.configure = configure};
    xdg_surface_add_listener(xdgSurface_, &surfaceListener, this);
    toplevel_ = xdg_surface_get_toplevel(xdgSurface_);
    if (!toplevel_)
        return false;
    static const xdg_toplevel_listener listener = {
        .configure = configureToplevel,
        .close = closeToplevel,
        .configure_bounds = configureBounds,
        .wm_capabilities = capabilities,
    };
    xdg_toplevel_add_listener(toplevel_, &listener, this);
    xdg_toplevel_set_title(toplevel_, "BlastIRC");
    xdg_toplevel_set_app_id(toplevel_, "io.github.trevarj.blastirc");
    xdg_toplevel_set_min_size(toplevel_, 720, static_cast<std::int32_t>(minimumHeight_));
    wl_surface_commit(m_surface);
    wl_display_flush(m_connection.display());
    if (wl_display_roundtrip(m_connection.display()) < 0)
        return false;
    setRunning(true);
    return true;
}

void Window::setMinimumHeight(std::uint32_t height) {
    height = std::clamp(height, 480u,
                        static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()));
    if (!toplevel_) {
        height_ = std::max(height_, height);
        normalHeight_ = std::max(normalHeight_, height);
    }
    if (minimumHeight_ == height)
        return;
    minimumHeight_ = height;
    if (toplevel_) {
        xdg_toplevel_set_min_size(toplevel_, 720, static_cast<std::int32_t>(minimumHeight_));
        wl_surface_commit(m_surface);
        wl_display_flush(m_connection.display());
    }
}

void Window::minimize() {
    if (toplevel_)
        xdg_toplevel_set_minimized(toplevel_);
}

void Window::toggleMaximize() {
    if (!toplevel_)
        return;
    if (maximized_)
        xdg_toplevel_unset_maximized(toplevel_);
    else
        xdg_toplevel_set_maximized(toplevel_);
}

void Window::beginMove(std::uint32_t serial) {
    if (toplevel_ && m_connection.seat() && serial)
        xdg_toplevel_move(toplevel_, m_connection.seat(), serial);
}

void Window::beginResize(std::uint32_t serial, std::uint32_t edge) {
    switch (edge) {
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP:
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM:
    case XDG_TOPLEVEL_RESIZE_EDGE_LEFT:
    case XDG_TOPLEVEL_RESIZE_EDGE_RIGHT:
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT:
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT:
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT:
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT:
        if (toplevel_ && m_connection.seat() && serial)
            xdg_toplevel_resize(toplevel_, m_connection.seat(), serial, edge);
        break;
    default:
        break;
    }
}

void Window::configure(void *data, xdg_surface *surface, std::uint32_t serial) {
    auto &self = *static_cast<Window *>(data);
    xdg_surface_ack_configure(surface, serial);
    self.maximized_ = self.pendingMaximized_;
    self.fullscreen_ = self.pendingFullscreen_;
    self.activated_ = self.pendingActivated_;
    self.resizing_ = self.pendingResizing_;
    self.suspended_ = self.pendingSuspended_;
    xdg_surface_set_window_geometry(surface, 0, 0, self.width_, self.height_);
    self.Surface::onConfigure(self.width_, self.height_);
}

void Window::configureToplevel(void *data, xdg_toplevel *, std::int32_t width, std::int32_t height,
                               wl_array *states) {
    auto &self = *static_cast<Window *>(data);
    self.pendingMaximized_ = self.pendingFullscreen_ = self.pendingActivated_ =
        self.pendingResizing_ = false;
    self.pendingSuspended_ = false;
    const auto *values = static_cast<const std::uint32_t *>(states->data);
    for (std::size_t i = 0; i < states->size / sizeof(*values); ++i) {
        self.pendingMaximized_ |= values[i] == XDG_TOPLEVEL_STATE_MAXIMIZED;
        self.pendingFullscreen_ |= values[i] == XDG_TOPLEVEL_STATE_FULLSCREEN;
        self.pendingActivated_ |= values[i] == XDG_TOPLEVEL_STATE_ACTIVATED;
        self.pendingResizing_ |= values[i] == XDG_TOPLEVEL_STATE_RESIZING;
        self.pendingSuspended_ |= values[i] == XDG_TOPLEVEL_STATE_SUSPENDED;
    }
    const bool normal = !self.pendingMaximized_ && !self.pendingFullscreen_;
    if (width > 0)
        self.width_ = static_cast<std::uint32_t>(width);
    else if (normal)
        self.width_ = self.normalWidth_;
    if (height > 0)
        self.height_ = static_cast<std::uint32_t>(height);
    else if (normal)
        self.height_ = std::max(self.normalHeight_, self.minimumHeight_);
    if (normal) {
        self.normalWidth_ = self.width_;
        self.normalHeight_ = self.height_;
    }
}

void Window::closeToplevel(void *data, xdg_toplevel *) {
    auto &self = *static_cast<Window *>(data);
    self.setRunning(false);
    if (self.closed_)
        self.closed_();
}

void Window::configureBounds(void *, xdg_toplevel *, std::int32_t, std::int32_t) {}
void Window::capabilities(void *, xdg_toplevel *, wl_array *) {}

void Window::render() {
    if (!renderContext() || !renderTarget().isReady())
        return;
    Surface::render();
    if (rendered_)
        rendered_();
}

void Window::destroyRole() {
    if (toplevel_)
        xdg_toplevel_destroy(toplevel_);
    if (xdgSurface_)
        xdg_surface_destroy(xdgSurface_);
    toplevel_ = nullptr;
    xdgSurface_ = nullptr;
}
} // namespace blast
