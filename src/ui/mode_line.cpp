#include "mode_line.hpp"
#include "../model.hpp"
#include "../theme/theme.hpp"
#include "logic.hpp"
#include <algorithm>
#include <array>
#include <string>
#include <ui/controls/glyph.h>
#include <ui/controls/label.h>
#include <vector>

namespace blast {
namespace {
template <class T> T *child(Node &parent) {
    auto node = std::make_unique<T>();
    auto *result = node.get();
    parent.addChild(std::move(node));
    return result;
}
void text(Label &label, std::string_view value) {
    if (label.setText(value))
        label.setTooltip(std::string(value));
}
} // namespace

struct ModeLine::Impl {
    struct Group {
        Glyph *icon;
        Label *label, *separator;
        float width = 0;
    };
    struct Unread {
        Label *label;
        std::string id, marker, name, target, caseMapping;
        size_t size = 0;
        uint64_t revision = 0;
        int count = 0;
        bool mentioned = false;
        float width = 0;
    };
    Model &model;
    std::array<Group, 5> groups;
    std::vector<Unread> unread;
    size_t unreadCount = 0;
    decltype(Settings::fools) fools;
    bool foolUnread = false;
    float scale = 1;
    bool error = false;

    Impl(Node &owner, Model &model) : model(model) {
        constexpr std::array icons{"user", "server", "plug-connected-x", "info-circle", "keyboard"};
        for (size_t i = 0; i < groups.size(); ++i) {
            auto &g = groups[i];
            g = {child<Glyph>(owner), child<Label>(owner), child<Label>(owner)};
            g.icon->setGlyph(icons[i]);
            g.icon->setHitTestVisible(false);
            g.separator->setText("│");
            g.separator->setHitTestVisible(false);
            g.label->setUseMarkup(false);
            g.label->setMaxLines(1);
            g.label->setEllipsize(TextEllipsize::End);
            g.label->setBaselineMode(LabelBaselineMode::TextFixedHeight);
        }
    }
};

ModeLine::ModeLine(Model &model) : impl_(std::make_unique<Impl>(*this, model)) {
    setClipChildren(true);
    setPaintContained(true);
    sync(1, SidebarMode::Expanded);
}
ModeLine::~ModeLine() = default;

void ModeLine::sync(float userScale, SidebarMode effectiveNavigation, std::string_view keyHint) {
    auto &p = *impl_;
    if (p.scale != userScale) {
        p.scale = userScale;
        markLayoutDirty();
    }
    const auto &model = p.model;
    const auto buffer = model.buffers.find(model.activeBuffer);
    const bool mentions = buffer != model.buffers.end() && buffer->second.kind == "mentions";
    auto network = model.networks.end();
    if (buffer != model.buffers.end() && !mentions)
        network = model.networks.find(buffer->second.server);
    else if (buffer == model.buffers.end() && !model.networks.empty())
        network = model.networks.begin();
    const Network *net = network == model.networks.end() ? nullptr : &network->second;
    const std::string_view serverName = buffer != model.buffers.end()
                                            ? std::string_view(buffer->second.server)
                                        : net ? std::string_view(network->first)
                                              : std::string_view{};
    const auto config = std::find_if(model.servers.begin(), model.servers.end(),
                                     [&](const auto &server) { return server.name == serverName; });
    const std::string_view name = mentions ? std::string_view{"All networks"}
                                  : config != model.servers.end() && !config->alias.empty()
                                      ? std::string_view(config->alias)
                                      : serverName;
    const std::string_view state = mentions ? "Read only"
                                   : net    ? std::string_view(net->state)
                                            : "Disconnected";
    std::string_view nick = mentions                       ? std::string_view{"Mentions"}
                            : net && net->state == "Ready" ? std::string_view(net->nick)
                                                           : std::string_view{};
    if (!mentions && nick.empty() && config != model.servers.end())
        if (const auto identity =
                std::find_if(model.identities.begin(), model.identities.end(),
                             [&](const auto &entry) { return entry.name == config->identity; });
            identity != model.identities.end())
            nick = identity->nick;
    p.error = net && !net->error.empty();
    const bool healthy = state == "Ready" && !p.error;
    text(*p.groups[0].label, nick);
    text(*p.groups[1].label, name);
    text(*p.groups[2].label, state == "Ready" ? "Connected" : state);
    text(*p.groups[3].label,
         p.error ? std::string_view(net->error) : std::string_view(model.status));
    text(*p.groups[4].label, keyHint);
    p.groups[2].icon->setGlyph(mentions ? "at" : healthy ? "plug" : "plug-connected-x");
    p.groups[3].icon->setGlyph(p.error ? "alert-circle" : "info-circle");
    const auto &font = themeUiFont();
    for (size_t i = 0; i < p.groups.size(); ++i) {
        auto &g = p.groups[i];
        for (auto *label : {g.label, g.separator}) {
            if (label->font().family != font.family)
                label->setFontFamily(font.family);
            if (label->font().features != font.features)
                label->setFontFeatures(font.features);
            label->setFontSize(12 * p.scale);
        }
        const auto role = i == 2              ? (state == "Failed" || p.error ? "error"
                                                 : healthy                    ? "primary"
                                                                              : "secondary")
                          : i == 3 && p.error ? "error"
                          : i == 4            ? "on_surface"
                                              : "on_surface_variant";
        g.label->setColor(themeForegroundColor(role));
        g.icon->setColor(themeForegroundColor(role));
        g.icon->setGlyphSize(14 * p.scale);
        g.separator->setColor(themeForegroundColor("outline_variant"));
    }
    const bool foolRulesChanged =
        p.foolUnread != model.settings.foolUnread || p.fools != model.settings.fools;
    const auto previousUnreadCount = p.unreadCount;
    p.unreadCount = 0;
    // Do not compose conversation labels while the channel sidebar is visible,
    // including its temporary compact presentation during a jump.
    if (effectiveNavigation == SidebarMode::Hidden) {
        if (foolRulesChanged) {
            p.foolUnread = model.settings.foolUnread;
            p.fools = model.settings.fools;
        }
        for (const auto &[id, channel] : model.buffers) {
            if (channel.kind != "channel" || channel.archived || channel.unread <= 0)
                continue;
            if (p.unreadCount == p.unread.size()) {
                auto *label = child<Label>(*this);
                label->setHitTestVisible(false);
                label->setUseMarkup(false);
                label->setMaxLines(1);
                label->setEllipsize(TextEllipsize::End);
                label->setBaselineMode(LabelBaselineMode::TextFixedHeight);
                auto &entry = p.unread.emplace_back();
                entry.label = label;
            }
            auto &entry = p.unread[p.unreadCount++];
            const auto config =
                std::find_if(model.servers.begin(), model.servers.end(),
                             [&](const auto &server) { return server.name == channel.server; });
            const std::string_view name = config != model.servers.end() && !config->alias.empty()
                                              ? std::string_view(config->alias)
                                              : std::string_view(channel.server);
            const auto network = model.networks.find(channel.server);
            const std::string_view mapping = network == model.networks.end()
                                                 ? std::string_view{"rfc1459"}
                                                 : std::string_view(network->second.caseMapping);
            const bool changed = entry.id != id || entry.count != channel.unread ||
                                 entry.marker != channel.readMarker ||
                                 entry.size != channel.messages.size() ||
                                 entry.revision != channel.messagesRevision ||
                                 entry.caseMapping != mapping || foolRulesChanged;
            const bool mentioned =
                changed ? ui_logic::hasUnreadMention(model, channel) : entry.mentioned;
            if (entry.id != id || entry.count != channel.unread || entry.name != name ||
                entry.target != channel.target || entry.mentioned != mentioned) {
                std::string caption;
                if (mentioned)
                    caption += '@';
                caption.append(name).append("/").append(channel.target).append(": ");
                caption += std::to_string(channel.unread);
                text(*entry.label, caption);
                entry.name = name;
                entry.target = channel.target;
            }
            if (changed) {
                entry.id = id;
                entry.count = channel.unread;
                entry.marker = channel.readMarker;
                entry.size = channel.messages.size();
                entry.revision = channel.messagesRevision;
                entry.caseMapping = mapping;
                entry.mentioned = mentioned;
            }
            auto &label = *entry.label;
            if (label.font().family != font.family)
                label.setFontFamily(font.family);
            if (label.font().features != font.features)
                label.setFontFeatures(font.features);
            label.setFontSize(12 * p.scale);
            label.setFontWeight(mentioned ? FontWeight::Bold : font.weight);
            label.setColor(themeForegroundColor(mentioned ? "error" : "primary"));
        }
    }
    for (size_t i = p.unreadCount; i < p.unread.size(); ++i) {
        p.unread[i].label->setVisible(false);
        p.unread[i].width = 0;
    }
    if (previousUnreadCount != p.unreadCount)
        markLayoutDirty();
}

LayoutSize ModeLine::doMeasure(Renderer &, const LayoutConstraints &constraints) {
    return constraints.constrain({420 * impl_->scale, 26 * impl_->scale});
}

void ModeLine::doLayout(Renderer &renderer) {
    auto &p = *impl_;
    const float padding = std::min(8 * p.scale, width() / 2);
    const float available = std::max(0.f, width() - 2 * padding);
    const float adornment = 30 * p.scale;
    auto measure = [&](size_t index, float budget) {
        auto &g = p.groups[index];
        const bool visible = !g.label->text().empty() && budget >= adornment + 12 * p.scale;
        g.label->setVisible(visible);
        g.icon->setVisible(visible);
        g.separator->setVisible(visible);
        g.width = 0;
        if (visible) {
            g.label->setMaxWidth(budget - adornment);
            g.label->measure(renderer);
            g.icon->measure(renderer);
            g.separator->measure(renderer);
            g.width = std::min(g.label->width(), budget - adornment) + adornment;
        }
        return g.width;
    };
    // Reserve connection and feedback before optional identity/chord groups.
    float remaining = available - measure(2, std::min(available, 132 * p.scale));
    remaining -= measure(3, remaining);
    remaining -= measure(4, std::min(remaining, available * .25f));
    remaining -= measure(0, remaining * .5f);
    remaining -= measure(1, remaining);
    for (size_t i = 0; i < p.unreadCount; ++i) {
        auto &entry = p.unread[i];
        entry.width = 0;
        const float gap = 8 * p.scale;
        const bool visible = remaining >= gap + 24 * p.scale;
        entry.label->setVisible(visible);
        if (!visible)
            continue;
        entry.label->setMaxWidth(remaining - gap);
        entry.label->measure(renderer);
        entry.width = std::min(entry.label->width(), remaining - gap) + gap;
        remaining -= entry.width;
    }
    float x = padding;
    for (auto &g : p.groups) {
        if (!g.width)
            continue;
        g.icon->setPosition(x, (height() - g.icon->height()) / 2);
        g.label->setPosition(x + 18 * p.scale, (height() - g.label->height()) / 2);
        g.separator->setPosition(x + g.width - 8 * p.scale, (height() - g.separator->height()) / 2);
        x += g.width;
    }
    for (size_t i = 0; i < p.unreadCount; ++i) {
        auto &entry = p.unread[i];
        if (!entry.width)
            continue;
        entry.label->setPosition(x + 4 * p.scale, (height() - entry.label->height()) / 2);
        x += entry.width;
    }
}
} // namespace blast
