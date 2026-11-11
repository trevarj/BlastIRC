#pragma once

#include "../model.hpp"
#include "pane.hpp"
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace blast {

struct WorkspaceRect {
    float x = 0, y = 0, width = 0, height = 0;
};

class Workspace {
  public:
    explicit Workspace(Model &);
    std::map<std::uint64_t, PaneState> panes;
    std::uint64_t focused = 0;
    SidebarMode navigationMode = SidebarMode::Expanded;

    static constexpr float minServerRailWidth = 56, maxServerRailWidth = 160;
    static constexpr float minChannelSidebarWidth = 160, maxChannelSidebarWidth = 420;
    static constexpr float minCompactChannelSidebarWidth = 40, maxCompactChannelSidebarWidth = 112;
    // Logical widths stay unset until dragged; layout retains its original defaults.
    std::optional<float> serverRailWidth() const { return serverRailWidth_; }
    std::optional<float> channelSidebarWidth() const { return channelSidebarWidth_; }
    std::optional<float> compactChannelSidebarWidth() const { return compactChannelSidebarWidth_; }
    void setServerRailWidth(float width);
    void setChannelSidebarWidth(float width);
    void setCompactChannelSidebarWidth(float width);

    std::uint64_t open(std::string_view buffer);
    void focus(std::uint64_t id);
    std::optional<std::uint64_t> navigateHistory(bool forward);
    void reconcile();
    void remapBuffer(std::string_view oldId, std::string_view newId);
    void persist();

  private:
    struct SavedPane {
        std::string key, published;
    };
    Model &model_;
    std::uint64_t nextId_ = 1;
    std::map<std::uint64_t, SavedPane> saved_;
    std::string publishedActive_, serialized_;
    std::optional<float> serverRailWidth_, channelSidebarWidth_, compactChannelSidebarWidth_;
    std::vector<std::string> history_;
    std::size_t historyCursor_ = 0;
    bool suppressHistory_ = true;

    std::uint64_t freshId();
    const std::string &fallbackBuffer() const;
    void remove(std::uint64_t id);
    void deduplicate();
    void publish();
    bool historyEligible(const std::string &buffer) const;
    void recordHistory(const std::string &buffer);
    void remapHistory(const std::string &oldId, const std::string &newId);
};
} // namespace blast
