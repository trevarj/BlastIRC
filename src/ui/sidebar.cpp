#include "sidebar.hpp"
#include "../irc/protocol.hpp"
#include "../model.hpp"
#include "../render/graphics.hpp"
#include "../theme/theme.hpp"
#include "channel_avatar.hpp"
#include "logic.hpp"
#include "ui.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <core/input/key_symbols.h>
#include <cstdint>
#include <map>
#include <render/scene/input_area.h>
#include <render/scene/rect_node.h>
#include <ui/controls/button.h>
#include <ui/controls/flex.h>
#include <ui/controls/image.h>
#include <ui/controls/input.h>
#include <ui/controls/label.h>
#include <ui/controls/scroll_view.h>
#include <ui/controls/select.h>
#include <ui/controls/select_popup_context.h>
#include <ui/controls/virtual_list_view.h>
#include <ui/style.h>
#include <utility>
#include <vector>

namespace blast {
namespace {
constexpr float railPadding = 6;
constexpr float avatarRowExtra = 12;
} // namespace
LayoutRect serverAvatarBox(float railWidth, float scale) {
    const float itemWidth = std::max(0.f, railWidth - 2 * railPadding * scale);
    const float itemHeight = itemWidth + avatarRowExtra * scale;
    const float extent = std::max(0.f, std::min(itemWidth - 8 * scale, itemHeight - 32 * scale));
    return {(railWidth - extent) / 2, 2 * railPadding * scale, extent, extent};
}
namespace {

template <class T, class... Args> T *child(Node &parent, Args &&...args) {
    auto node = std::make_unique<T>(std::forward<Args>(args)...);
    auto *result = node.get();
    parent.addChild(std::move(node));
    return result;
}

void labelFont(Label &label, float size, bool monospace = false, bool emphasized = false) {
    const auto &font = themeUiFont();
    const std::string_view family = monospace ? std::string_view{"monospace"} : font.family;
    if (label.font().family != family)
        label.setFontFamily(std::string(family));
    const auto weight = emphasized ? FontWeight::SemiBold : font.weight;
    if (label.font().weight != weight)
        label.setFontWeight(weight);
    if (label.font().features != font.features)
        label.setFontFeatures(font.features);
    label.setFontSize(size);
}

bool menuKey(const InputArea::KeyData &key) {
    return key.sym == XKB_KEY_Menu || (key.sym == XKB_KEY_F10 && (key.modifiers & KeyMod::Shift));
}

// Retained text rows keep their keys, callbacks and text storage across model updates.
class Row final : public InputArea {
  public:
    explicit Row(std::string_view iconName = {}, bool withAvatar = false) {
        setFocusable(true);
        setRetainsFocusOnPointerRelease(true);
        setAcceptedButtons(buttonMask({BTN_LEFT, BTN_RIGHT}));
        setClipChildren(true);
        setPaintContained(true);
        background = child<RectNode>(*this);
        background->setHitTestVisible(false);
        if (withAvatar) {
            avatarBackground = child<RectNode>(*this);
            avatarBackground->setHitTestVisible(false);
            avatarImage = child<Image>(*this);
            avatarImage->setFit(ImageFit::Cover);
            avatarImage->setHitTestVisible(false);
            avatarInitials = child<Label>(*this);
            avatarInitials->setUseMarkup(false);
            avatarInitials->setMaxLines(1);
            avatarInitials->setHitTestVisible(false);
            draftBackground = child<RectNode>(*this);
            draftBackground->setHitTestVisible(false);
        }
        if (!iconName.empty()) {
            icon = child<LucideIcon>(*this, iconName);
            icon->setHitTestVisible(false);
        }
        title = child<Label>(*this);
        subtitle = child<Label>(*this);
        if (withAvatar) {
            badgeBackground = child<RectNode>(*this);
            badgeBackground->setHitTestVisible(false);
        }
        badge = child<Label>(*this);
        for (auto *label : {title, subtitle, badge}) {
            label->setUseMarkup(false);
            label->setMaxLines(1);
            label->setHitTestVisible(false);
        }
        setOnClick([this](const PointerData &data) {
            if (data.button == BTN_RIGHT) {
                if (menu)
                    menu(*this, data);
            } else if (activate)
                activate(*this);
        });
        setOnKeyDown([this](const KeyData &key) {
            if (!key.pressed)
                return;
            if (menuKey(key)) {
                if (menu)
                    menu(*this, std::nullopt);
            } else if (KeySymbol::isEnterOrSpace(key.sym) && activate)
                activate(*this);
        });
        setOnEnter([this](const PointerData &) { paint(); });
        setOnPress([this](const PointerData &) { paint(); });
        setOnLeave([this] { paint(); });
        setOnFocusGain([this] {
            paint();
            if (reveal)
                reveal(*this);
        });
        setOnFocusLoss([this] { paint(); });
    }

    void style(float scale, bool selected = false) {
        zoom = scale;
        active = selected;
        labelFont(*title, 14 * scale, monospace, active || emphasized);
        labelFont(*subtitle, 12 * scale);
        labelFont(*badge, (avatarOnly ? 11.f : 12.f) * scale);
        if (avatarInitials) {
            labelFont(*avatarInitials, 13 * scale);
            avatarInitials->setFontWeight(FontWeight::SemiBold);
        }
        paint();
    }

    float minimumHeight(Renderer &renderer) {
        // Reserve the badge lane even when there is no unread count.
        float minimum = unreadCountBadge
                            ? std::max(16 * zoom, badgeFontHeight.get(*badge, renderer)) +
                                  (avatarOnly ? 4 * zoom : 0)
                            : 0;
        if (avatarOnly)
            return minimum;
        title->measure(renderer);
        float textHeight = std::max(title->height(), titleFontHeight.get(*title, renderer, true));
        if (!subtitle->text().empty()) {
            subtitle->measure(renderer);
            const float subtitleHeight =
                std::max(subtitle->height(), subtitleFontHeight.get(*subtitle, renderer));
            textHeight = inlineSubtitle ? std::max(textHeight, subtitleHeight)
                                        : textHeight + subtitleHeight + 3 * zoom;
        }
        if (!unreadCountBadge && !badge->text().empty()) {
            badge->measure(renderer);
            minimum = std::max(16 * zoom, badge->height()) + 4 * zoom;
        }
        return std::max(minimum, textHeight + 4 * zoom);
    }

    void tooltip(std::string_view value) {
        if (tooltipText != value) {
            tooltipText.assign(value);
            setTooltip(tooltipText);
        }
    }

    void syncAvatar(std::string_view path, std::string_view caption) {
        if (avatarSource != path) {
            avatarSource.assign(path);
            markLayoutDirty();
        }
        if (avatarCaption != caption) {
            avatarCaption.assign(caption);
            avatarInitials->setText(nativeAvatarInitials(caption, true));
        }
    }

    void enableSprite() {
        for (auto &pixel : avatarPixels) {
            pixel = child<RectNode>(*this);
            pixel->setHitTestVisible(false);
        }
    }

    void paint() {
        const Color clear{0, 0, 0, 0};
        RoundedRectStyle style{};
        const bool primary = active || pressed();
        const bool hover = hovered() && !primary;
        const bool showAvatar = avatarImage && avatarLaneVisible;
        style.fill = primary ? themeSurfaceColor("primary_container")
                     : hover ? themeSurfaceColor("hover")
                             : clear;
        style.border = focused() ? themeForegroundColor("secondary")
                       : primary ? themeForegroundColor("primary")
                                 : clear;
        style.radius = themeCornerRadius();
        style.borderWidth = primary || focused() ? zoom : 0.f;
        background->setStyle(style);
        if (avatarBackground && showAvatar) {
            RoundedRectStyle avatarStyle{};
            avatarStyle.fill =
                themeSurfaceColor(primary ? "surface_container_highest" : "surface_container_high");
            avatarStyle.radius = themeCornerRadius();
            avatarBackground->setStyle(avatarStyle);
            avatarInitials->setColor(primary       ? themeForegroundColor("on_primary_container")
                                     : customTitle ? titleColor
                                                   : themeForegroundColor("on_surface_variant"));
            RoundedRectStyle draftStyle{};
            draftStyle.fill = themeForegroundColor("tertiary_container");
            draftStyle.radius = std::min(themeCornerRadius(), 8 * zoom);
            draftBackground->setStyle(draftStyle);
            draftBackground->setVisible(draft);
        }
        title->setColor(primary       ? themeForegroundColor("on_primary_container")
                        : hover       ? themeForegroundColor("on_hover")
                        : customTitle ? titleColor
                                      : themeForegroundColor("on_surface"));
        subtitle->setColor(hover ? themeForegroundColor("on_hover")
                                 : themeForegroundColor(primary ? "on_primary_container"
                                                        : error ? "error"
                                                        : draft ? "tertiary"
                                                                : "on_surface_variant"));
        badge->setColor(badgeBackground ? (mentioned ? ::palette.onError : ::palette.onPrimary)
                                        : themeForegroundColor(hover       ? "on_hover"
                                                               : mentioned ? "error"
                                                               : primary   ? "on_primary_container"
                                                                           : "primary"));
        if (badgeBackground) {
            RoundedRectStyle badgeStyle{};
            badgeStyle.fill = unreadCountBadge ? (mentioned ? ::palette.error : ::palette.primary)
                                               : themeSurfaceColor(mentioned ? "error" : "primary");
            badgeStyle.radius = 4 * zoom;
            badgeBackground->setStyle(badgeStyle);
        }
        if (icon) {
            icon->setVisible(!showAvatar || draft);
            icon->setColor(showAvatar && draft ? themeForegroundColor("on_tertiary_container")
                           : hover             ? themeForegroundColor("on_hover")
                                   : themeForegroundColor(primary ? "on_primary_container"
                                                          : draft ? "tertiary"
                                                                  : "on_surface_variant"));
        }
    }

    std::string key, context, preview, scratch;
    bool matched = true, draft = false, error = false, customTitle = false;
    bool inlineSubtitle = false, emphasized = false, monospace = false;
    bool avatarOnly = false, avatarLaneVisible = true, mentioned = false;
    bool unreadCountBadge = false;
    bool pinnedCurrent = false;
    bool spriteAvatar = false;
    float allocatedMinimum = 0, nativeMinimum = 0;
    std::uint32_t avatarHash = 0;
    Color titleColor{};
    Label *title = nullptr, *subtitle = nullptr, *badge = nullptr;
    std::function<void(Row &)> activate, reveal;
    std::function<void(Row &, std::optional<PointerData>)> menu;

  protected:
    void doLayout(Renderer &renderer) override {
        background->setSize(width(), height());
        const float padding = avatarOnly ? 4 * zoom : 9 * zoom;
        const bool showAvatar = avatarImage && avatarLaneVisible;
        const float avatarSize =
            showAvatar ? std::max(0.f, std::min({width() - 2 * padding, height() - 8 * zoom,
                                                 avatarOnly ? width() - 16 * zoom
                                                            : 32 * zoom *
                                                                  std::clamp(width() / (240 * zoom),
                                                                             .85f, 1.25f)}))
                       : 0;
        const float avatarLeft = avatarOnly ? (width() - avatarSize) / 2 : padding;
        const float avatarTop = (height() - avatarSize) / 2;
        const bool sprite = showAvatar && spriteAvatar && avatarPixels.front();
        if (avatarPixels.front()) {
            const float pixelSize = avatarSize / 7;
            for (int y = 0; y < 5; ++y)
                for (int x = 0; x < 5; ++x) {
                    auto *pixel = avatarPixels[y * 5 + x];
                    pixel->setVisible(sprite && ((avatarHash >> (y * 3 + std::min(x, 4 - x))) & 1));
                    if (pixel->visible()) {
                        pixel->setPosition(avatarLeft + (x + 1) * pixelSize,
                                           avatarTop + (y + 1) * pixelSize);
                        pixel->setSize(pixelSize, pixelSize);
                        RoundedRectStyle style{};
                        style.fill = titleColor;
                        pixel->setStyle(style);
                    }
                }
        }
        float left = padding;
        if (avatarImage && !showAvatar) {
            avatarBackground->setVisible(false);
            avatarImage->setVisible(false);
            avatarInitials->setVisible(false);
            draftBackground->setVisible(false);
        }
        if (showAvatar) {
            avatarBackground->setVisible(true);
            avatarBackground->setPosition(avatarLeft, avatarTop);
            avatarBackground->setSize(avatarSize, avatarSize);
            avatarImage->setPosition(avatarLeft, avatarTop);
            avatarImage->setSize(avatarSize, avatarSize);
            avatarImage->setRadius(themeCornerRadius());
            const int target =
                std::max(1, int(std::ceil(avatarSize * std::max(1.f, renderer.renderScale()))));
            if (loadedAvatarSource != avatarSource || loadedAvatarSize != target) {
                loadedAvatarSource = avatarSource;
                loadedAvatarSize = target;
                loadChannelAvatarRaster(*avatarImage, renderer, avatarSource, target);
            }
            avatarImage->setVisible(avatarImage->hasImage());
            if (avatarImage->visible())
                avatarImage->layout(renderer);
            labelFont(*avatarInitials, avatarSize * (13.f / 32.f));
            avatarInitials->setVisible(!avatarImage->hasImage() && !sprite);
            if (avatarInitials->visible())
                avatarInitialsPlacement.place(*avatarInitials, renderer, avatarLeft, avatarTop,
                                              avatarSize, avatarSize);
            left += avatarSize + 10 * zoom;
        }
        if (icon) {
            const bool overlay = showAvatar && draft;
            const float iconSize = (overlay ? 12.f : 18.f) * zoom;
            const bool iconOnly = title->text().empty() && subtitle->text().empty();
            const float iconLeft = showAvatar ? avatarLeft + avatarSize - iconSize
                                   : iconOnly ? (width() - iconSize) / 2
                                              : padding;
            icon->setVisible(!showAvatar || draft);
            const float iconTop =
                overlay ? avatarTop + avatarSize - iconSize : (height() - iconSize) / 2;
            icon->setPosition(iconLeft, iconTop);
            if (overlay) {
                draftBackground->setPosition(iconLeft - 2 * zoom, iconTop - 2 * zoom);
                draftBackground->setSize(iconSize + 4 * zoom, iconSize + 4 * zoom);
            }
            icon->setSize(iconSize, iconSize);
            if (icon->visible())
                icon->layout(renderer);
            if (!showAvatar)
                left += iconSize + 7 * zoom;
        }
        badge->setVisible(!badge->text().empty());
        badge->setMaxWidth(avatarOnly ? 30 * zoom : std::max(0.f, width() * .35f));
        badge->measure(renderer);
        const float badgeHeight = std::max(16 * zoom, badge->height());
        const float badgeExtent =
            badgeBackground ? std::max(badgeHeight, badge->width() + 8 * zoom) : badge->width();
        const float badgeWidth =
            badge->visible() ? badgeExtent + (badgeBackground ? 6 : 8) * zoom : 0;
        title->setVisible(!avatarOnly);
        if (badgeBackground)
            badgeBackground->setVisible(badge->visible());
        if (avatarOnly) {
            subtitle->setVisible(false);
            const float badgeX = std::max(0.f, width() - badgeExtent - 2 * zoom);
            if (badgeBackground) {
                badgeBackground->setPosition(badgeX, 2 * zoom);
                badgeBackground->setSize(badgeExtent, badgeHeight);
            }
            badge->setPosition(badgeX + (badgeExtent - badge->width()) / 2,
                               2 * zoom + (badgeHeight - badge->height()) / 2);
            return;
        }
        const float available = std::max(0.f, width() - left - padding);
        const bool hasSubtitle = !subtitle->text().empty();
        subtitle->setVisible(hasSubtitle);
        subtitle->setMaxWidth(inlineSubtitle ? available * .45f : available);
        if (hasSubtitle)
            subtitle->measure(renderer);
        const float stateWidth = hasSubtitle && inlineSubtitle ? subtitle->width() + 8 * zoom : 0;
        title->setMaxWidth(std::max(0.f, available - badgeWidth - stateWidth));
        title->measure(renderer);
        const bool twoLines = hasSubtitle && !inlineSubtitle;
        const float textHeight = title->height() + (twoLines ? subtitle->height() + 3 * zoom : 0);
        const float top = std::max(2 * zoom, (height() - textHeight) / 2);
        title->setPosition(left, top);
        subtitle->setPosition(inlineSubtitle ? width() - padding - badgeWidth - subtitle->width()
                                             : left,
                              inlineSubtitle ? (height() - subtitle->height()) / 2
                                             : top + title->height() + 3 * zoom);
        const float badgeX = width() - padding - badgeExtent;
        if (badgeBackground) {
            badgeBackground->setPosition(badgeX, (height() - badgeHeight) / 2);
            badgeBackground->setSize(badgeExtent, badgeHeight);
        }
        badge->setPosition(badgeX + (badgeExtent - badge->width()) / 2,
                           (height() - badge->height()) / 2);
    }

  private:
    struct FontHeight {
        TextFont font;
        float size = 0, renderScale = 0, height = 0;
        std::uint64_t generation = 0;
        FontWeight normalWeight{};
        bool valid = false;

        float get(const Label &label, Renderer &renderer, bool reserveEmphasis = false) {
            const auto weight = themeUiFont().weight;
            if (!valid || font != label.font() || size != label.fontSize() ||
                renderScale != renderer.renderScale() ||
                generation != renderer.textMetricsGeneration() ||
                (reserveEmphasis && normalWeight != weight)) {
                font = label.font();
                size = label.fontSize();
                renderScale = renderer.renderScale();
                generation = renderer.textMetricsGeneration();
                normalWeight = weight;
                height = renderer.fontRowExtent(size, font);
                if (reserveEmphasis) {
                    auto emphasizedFont = font;
                    emphasizedFont.weight = weight;
                    height = std::max(height, renderer.fontRowExtent(size, emphasizedFont));
                    emphasizedFont.weight = FontWeight::SemiBold;
                    height = std::max(height, renderer.fontRowExtent(size, emphasizedFont));
                }
                valid = true;
            }
            return height;
        }
    };
    FontHeight titleFontHeight, subtitleFontHeight, badgeFontHeight;
    RectNode *background = nullptr, *avatarBackground = nullptr, *draftBackground = nullptr;
    RectNode *badgeBackground = nullptr;
    Image *avatarImage = nullptr;
    Label *avatarInitials = nullptr;
    std::array<RectNode *, 25> avatarPixels{};
    LucideIcon *icon = nullptr;
    std::string tooltipText, avatarSource, loadedAvatarSource, avatarCaption;
    int loadedAvatarSize = 0;
    NativeAvatarLabelPlacement avatarInitialsPlacement;
    float zoom = 1;
    bool active = false;
};

ContextMenuEntry menuEntry(std::string_view label, std::string_view icon,
                           std::size_t value = static_cast<std::size_t>(-1)) {
    ContextMenuEntry entry;
    entry.label.assign(label);
    entry.icon.assign(icon);
    entry.value = value;
    return entry;
}

struct Popup {
    Node &owner;
    std::shared_ptr<int> alive = std::make_shared<int>(0);
    bool open = false;
    std::uint64_t generation = 0;

    explicit Popup(Node &node) : owner(node) {}
    ~Popup() {
        alive.reset();
        if (open && owner.popupContext())
            owner.popupContext()->closeSelectDropdown();
    }
    void show(Node &anchor, float scale, std::vector<ContextMenuEntry> menu,
              std::function<void(std::size_t)> select,
              std::optional<InputArea::PointerData> pointer = {}) {
        auto *context = owner.popupContext();
        if (!context)
            return;
        float left, top, right, bottom;
        Node::transformedBounds(&anchor, left, top, right, bottom);
        SelectPopupContext::DropdownRequest request;
        request.pointerContextMenu = pointer.has_value();
        request.anchorX = int(std::round(left));
        request.anchorY = int(std::round(top));
        request.anchorWidth = std::max(1, int(std::round(right - left)));
        request.anchorHeight = std::max(1, int(std::round(bottom - top)));
        if (pointer) {
            request.anchorX = int(std::round(pointer->sceneX));
            request.anchorY = int(std::round(pointer->sceneY));
            request.anchorWidth = request.anchorHeight = 1;
            request.inputSerial = pointer->serial;
        }
        request.menuWidth = 230 * scale;
        request.maxMenuWidth = 330 * scale;
        request.fontSize = 14 * scale;
        request.maxVisibleOptions = menu.size();
        request.menu = std::move(menu);
        const std::weak_ptr<int> guard = alive;
        const auto current = ++generation;
        open = true;
        context->openSelectDropdown(
            request, {[this, guard, current, select = std::move(select)](std::size_t index) {
                          if (guard.expired() || current != generation)
                              return;
                          open = false;
                          select(index);
                      },
                      [this, guard, current] {
                          if (!guard.expired() && current == generation)
                              open = false;
                      }});
    }
};

void serverMenu(Model &model, Popup &popup, Node &anchor, float scale, const std::string &name,
                const std::function<void(std::string)> &execute,
                std::optional<InputArea::PointerData> pointer = {}) {
    const auto network = model.networks.find(name);
    const bool connected = network != model.networks.end() &&
                           network->second.state != "Disconnected" &&
                           network->second.state != "Failed";
    const auto profile = std::find_if(model.servers.begin(), model.servers.end(),
                                      [&](const auto &config) { return config.name == name; });
    const bool account = profile != model.servers.end() && profile->sojuAccount;
    const bool childNetwork = profile != model.servers.end() && !profile->sojuParent.empty();
    auto connection = menuEntry("Connection", "plug");
    connection.children.reserve(connected ? 2 : 1);
    connection.children.push_back(
        menuEntry(connected ? "Reconnect" : "Connect", connected ? "refresh" : "plug", 0));
    if (connected)
        connection.children.push_back(menuEntry("Disconnect", "plug-off", 1));
    std::vector<ContextMenuEntry> menu;
    menu.reserve(4);
    menu.push_back(menuEntry("Open server log", "terminal", 3));
    menu.push_back(std::move(connection));
    ContextMenuEntry separator;
    separator.separator = true;
    menu.push_back(std::move(separator));
    menu.push_back(menuEntry("Edit profile…", "settings", connected ? 2 : 1));
    if (account) {
        menu.push_back(menuEntry("Manage account and networks…", "settings", 4));
        menu.push_back(menuEntry("Add network…", "plus", 5));
    } else if (childNetwork) {
        menu.push_back(menuEntry("Edit remote network…", "settings", 6));
        menu.push_back(menuEntry("Delete remote network…", "trash", 7));
    }
    popup.show(
        anchor, scale, std::move(menu),
        [&model, execute, name, connected](std::size_t index) {
            if (index == 3)
                execute("open-server-log:" + name);
            else if (index == (connected ? 2u : 1u))
                execute("server:" + name);
            else if (index == 4)
                execute("soju-manage:" + name);
            else if (index == 5)
                execute("soju-add:" + name);
            else if (index == 6)
                execute("soju-edit:" + name);
            else if (index == 7)
                execute("soju-delete:" + name);
            else if (index <= (connected ? 1u : 0u))
                model.actions.push_back({index == 1 ? "disconnect" : "connect", {}, name, {}});
        },
        pointer);
}

class Avatar final : public InputArea {
  public:
    explicit Avatar(std::string name) : key(std::move(name)) {
        setFocusable(true);
        setRetainsFocusOnPointerRelease(true);
        setTabFocusKey("server:" + key);
        setAcceptedButtons(buttonMask({BTN_LEFT, BTN_RIGHT}));
        setClipChildren(true);
        setPaintContained(true);
        background = child<RectNode>(*this);
        selectionMarker = child<RectNode>(*this);
        image = child<Image>(*this);
        image->setFit(ImageFit::Cover);
        initials = child<Label>(*this);
        alias = child<Label>(*this);
        dot = child<RectNode>(*this);
        unreadBackground = child<RectNode>(*this);
        unreadBadge = child<Label>(*this);
        for (auto *node : {static_cast<Node *>(background), static_cast<Node *>(selectionMarker),
                           static_cast<Node *>(image), static_cast<Node *>(initials),
                           static_cast<Node *>(alias), static_cast<Node *>(dot),
                           static_cast<Node *>(unreadBackground), static_cast<Node *>(unreadBadge)})
            node->setHitTestVisible(false);
        for (auto *label : {initials, alias, unreadBadge}) {
            label->setUseMarkup(false);
            label->setMaxLines(1);
        }
        setOnClick([this](const PointerData &data) {
            if (data.button == BTN_RIGHT) {
                if (menu)
                    menu(*this, data);
            } else if (activate)
                activate(*this);
        });
        setOnKeyDown([this](const KeyData &event) {
            if (!event.pressed)
                return;
            if (menuKey(event)) {
                if (menu)
                    menu(*this, std::nullopt);
            } else if (KeySymbol::isEnterOrSpace(event.sym) && activate)
                activate(*this);
        });
        setOnEnter([this](const PointerData &) { paint(); });
        setOnLeave([this] { paint(); });
        setOnPress([this](const PointerData &) { paint(); });
        setOnFocusGain([this] {
            paint();
            if (reveal)
                reveal(*this);
        });
        setOnFocusLoss([this] { paint(); });
    }

    void sync(std::string_view caption, std::string_view path, const Network *network,
              bool selected, float scale, bool account = false, bool collapsed = false,
              bool hiddenSelected = false, int unread = 0, bool mentioned = false,
              std::string_view childStatus = {}, bool childError = false) {
        zoom = scale;
        setFocusOnPointerPress(!account);
        active = selected;
        activeChild = hiddenSelected;
        unreadMention = mentioned;
        hiddenError = childError;
        unreadBadge->setText(unread > 99 ? "99+" : unread > 0 ? std::to_string(unread) : "");
        unreadBadge->setVisible(unread > 0);
        if (alias->text() != caption) {
            alias->setText(caption);
            initials->setText(nativeAvatarInitials(caption));
        }
        if (source != path) {
            source.assign(path);
            markLayoutDirty();
        }
        const auto state = network ? std::string_view(network->state) : "Disconnected";
        dotColor = themeForegroundColor(network && !network->error.empty() ? "error"
                                        : state == "Ready"                 ? "primary"
                                        : state == "Failed"                ? "error"
                                        : state == "Disconnected"          ? "outline"
                                                                           : "secondary");
        scratch.assign(caption);
        if (caption != key)
            scratch.append(" (").append(key).append(")");
        scratch.append("\n").append(state);
        if (network && !network->error.empty())
            scratch.append("\n").append(network->error);
        scratch.append(
            account ? (collapsed ? "\nClick to expand account" : "\nClick to collapse account")
                    : "\nClick to select network");
        scratch.append("; Menu for profile and connection actions");
        if (hiddenSelected)
            scratch.append("\nCurrent conversation is in this collapsed account");
        if (unread > 0)
            scratch.append("\nUnread: ").append(std::to_string(unread));
        if (mentioned)
            scratch.append("\nUnread mention");
        scratch.append(childStatus);
        if (tooltipText != scratch) {
            tooltipText = scratch;
            setTooltip(tooltipText);
        }
        labelFont(*initials, 17 * scale);
        initials->setFontWeight(FontWeight::SemiBold);
        labelFont(*alias, 11 * scale);
        labelFont(*unreadBadge, 10 * scale);
        unreadBadge->setFontWeight(FontWeight::SemiBold);
        paint();
    }

    std::string key;
    std::function<void(Avatar &)> activate, reveal;
    std::function<void(Avatar &, std::optional<PointerData>)> menu;

  protected:
    void doLayout(Renderer &renderer) override {
        const auto box = serverAvatarBox(width() + 2 * railPadding * zoom, zoom);
        const float extent = box.width;
        const float left = box.x - railPadding * zoom;
        const float top = box.y - railPadding * zoom;
        background->setPosition(left, top);
        background->setSize(extent, extent);
        const float markerHeight = std::min(20 * zoom, extent);
        selectionMarker->setPosition(0, top + (extent - markerHeight) / 2);
        selectionMarker->setSize(3 * zoom, markerHeight);
        image->setPosition(left + 2 * zoom, top + 2 * zoom);
        image->setSize(std::max(0.f, extent - 4 * zoom), std::max(0.f, extent - 4 * zoom));
        image->setRadius(themeCornerRadius());
        const int target = std::max(1, int(std::ceil(image->width())));
        const float renderScale = renderer.renderScale();
        if (loadedSource != source || loadedSize != target || loadedScale != renderScale) {
            loadedSource = source;
            loadedSize = target;
            loadedScale = renderScale;
            if (source.empty())
                image->clear(renderer);
            else
                image->setSourceFile(renderer, source, target, false, true);
        }
        image->setVisible(image->hasImage());
        if (image->visible())
            image->layout(renderer);
        initials->setVisible(!image->hasImage());
        labelFont(*initials, extent * (17.f / 44.f));
        if (initials->visible())
            initialsPlacement.place(*initials, renderer, left, top, extent, extent);
        alias->setMaxWidth(std::max(0.f, width() - 4 * zoom));
        alias->measure(renderer);
        alias->setPosition((width() - alias->width()) / 2, top + extent + 6 * zoom);
        const float dotSize = 8 * zoom;
        dot->setPosition(left + extent - dotSize, top + extent - dotSize);
        dot->setSize(dotSize, dotSize);
        unreadBadge->setMaxWidth(std::max(0.f, width() - 8 * zoom));
        unreadBadge->measure(renderer);
        const float badgeHeight = std::max(16 * zoom, unreadBadge->height());
        const float badgeWidth = std::max(badgeHeight, unreadBadge->width() + 8 * zoom);
        const float badgeLeft =
            std::max(0.f, std::min(left + extent - badgeWidth, width() - badgeWidth));
        unreadBackground->setVisible(unreadBadge->visible());
        unreadBackground->setPosition(badgeLeft, top);
        unreadBackground->setSize(badgeWidth, badgeHeight);
        unreadBadge->setPosition(badgeLeft + (badgeWidth - unreadBadge->width()) / 2,
                                 top + (badgeHeight - unreadBadge->height()) / 2);
    }

  private:
    void paint() {
        const bool highlight = active || pressed();
        RoundedRectStyle style{};
        style.fill = themeSurfaceColor(highlight   ? "primary_container"
                                       : hovered() ? "hover"
                                                   : "surface_container_high");
        style.border = themeForegroundColor(focused() ? "secondary"
                                            : active  ? "primary"
                                                      : "outline_variant");
        style.borderWidth = zoom;
        style.radius = themeCornerRadius();
        background->setStyle(style);
        selectionMarker->setVisible(active || activeChild);
        RoundedRectStyle marker{};
        marker.fill = themeForegroundColor("primary");
        marker.radius = 1.5f * zoom;
        selectionMarker->setStyle(marker);
        initials->setColor(highlight   ? themeForegroundColor("on_primary_container")
                           : hovered() ? themeForegroundColor("on_hover")
                                       : themeForegroundColor("on_surface"));
        alias->setColor(themeForegroundColor(hiddenError ? "error"
                                             : active    ? "primary"
                                                         : "on_surface_variant"));
        RoundedRectStyle status{};
        status.fill = dotColor;
        status.border = themeForegroundColor("surface");
        status.borderWidth = zoom;
        status.radius = 4 * zoom;
        dot->setStyle(status);
        RoundedRectStyle unreadStyle{};
        unreadStyle.fill = unreadMention ? ::palette.error : ::palette.primary;
        unreadStyle.radius = 4 * zoom;
        unreadBackground->setStyle(unreadStyle);
        unreadBadge->setColor(unreadMention ? ::palette.onError : ::palette.onPrimary);
    }

    RectNode *background = nullptr, *selectionMarker = nullptr, *dot = nullptr;
    RectNode *unreadBackground = nullptr;
    Image *image = nullptr;
    Label *initials = nullptr, *alias = nullptr;
    Label *unreadBadge = nullptr;
    std::string source, loadedSource, scratch, tooltipText;
    int loadedSize = 0;
    NativeAvatarLabelPlacement initialsPlacement;
    float zoom = 1, loadedScale = 0;
    bool active = false, activeChild = false, unreadMention = false, hiddenError = false;
    Color dotColor{};
};

std::string folded(std::string_view text) {
    std::string result(text);
    for (char &c : result)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

int prefixRank(std::string_view prefix) {
    for (std::size_t i = 0; i < 5; ++i)
        if (prefix.find("~&@%+"[i]) != std::string_view::npos)
            return int(i);
    return 5;
}

Color memberColor(const Model &model, const std::string &name, std::uint32_t hash) {
    if (auto color = model.settings.nickColors.find(name);
        color != model.settings.nickColors.end()) {
        Color parsed;
        if (tryParseHexColor(color->second, parsed))
            return parsed;
    }
    if (model.settings.nickPalette == "THEME")
        return withAlpha(themeNickColor(hash), 1.f);
    const bool classic = model.settings.nickPalette == "CLASSIC";
    return hsv(classic ? float(hash % 12) / 12 : float(hash % 360) / 360,
               model.settings.nickPalette == "VIVID" ? .85f
               : classic                             ? .6f
                                                     : .55f,
               classic ? .85f : .92f);
}
} // namespace

struct ServerRail::Impl {
    struct Entry {
        Avatar *avatar = nullptr;
        RectNode *capsule = nullptr;
        int unread = 0;
        bool mentioned = false, account = false;
        std::uint64_t seen = 0, placed = 0;
    };
    ServerRail &owner;
    Model &model;
    std::function<void(std::string)> execute;
    std::function<void(Node &, InputArea *)> retire;
    Popup popup;
    ScrollView *scroll = nullptr;
    Node *canvas = nullptr;
    Row *mentions = nullptr, *readAll = nullptr, *add = nullptr, *settings = nullptr;
    std::map<std::string, Entry> servers;
    std::vector<std::string> order;
    std::uint64_t epoch = 0;
    float zoom = 1;
    std::string settingsBinding;

    Impl(ServerRail &node, Model &value, std::function<void(std::string)> command,
         std::function<void(Node &, InputArea *)> retireInput)
        : owner(node), model(value), execute(std::move(command)), retire(std::move(retireInput)),
          popup(node) {
        scroll = child<ScrollView>(owner);
        scroll->setViewportPaddingH(0);
        scroll->setViewportPaddingV(0);
        canvas = child<Node>(*scroll->content());
        mentions = child<Row>(owner, "at-sign");
        mentions->setTabFocusKey("rail-global-mentions");
        mentions->tooltip(commandLabel(model, "Mentions — all networks", "global-mentions"));
        mentions->activate = [this](Row &) { execute("global-mentions"); };
        readAll = child<Row>(owner, "check-check");
        readAll->setTabFocusKey("rail-mark-all-read");
        readAll->tooltip("Mark all read — all buffers on every network");
        readAll->activate = [this](Row &row) {
            if (row.visible() && hasUnread())
                execute("mark-all-read");
        };
        add = child<Row>(owner, "plus");
        add->setTabFocusKey("rail-add-server");
        add->tooltip("Add network");
        add->activate = [this](Row &) { execute("add-server"); };
        settings = child<Row>(owner, "settings");
        settings->setTabFocusKey("rail-settings");
        settings->tooltip(commandLabel(model, "Settings", "settings"));
        settings->activate = [this](Row &) { execute("settings"); };
    }

    bool hasUnread() const {
        return std::any_of(model.buffers.begin(), model.buffers.end(), [](const auto &entry) {
            return entry.second.kind != "search" && entry.second.unread > 0;
        });
    }

    Entry &entry(const std::string &name) {
        auto [it, inserted] = servers.try_emplace(name);
        auto &result = it->second;
        result.seen = epoch;
        if (inserted) {
            order.push_back(name);
            result.avatar = child<Avatar>(*canvas, name);
            result.avatar->activate = [this](Avatar &avatar) {
                const auto item = servers.find(avatar.key);
                if (item != servers.end())
                    execute(std::string(item->second.account ? "soju-group-toggle:"
                                                             : "select-server:") +
                            avatar.key);
            };
            result.avatar->menu = [this](Avatar &avatar,
                                         std::optional<InputArea::PointerData> pointer) {
                serverMenu(model, popup, avatar, zoom, avatar.key, execute, pointer);
            };
            result.avatar->reveal = [this](Avatar &avatar) {
                scroll->requestScrollIntoView(&avatar);
            };
        }
        return result;
    }

    void sync(std::string_view selected, float width, float height, float scale) {
        zoom = scale;
        ++epoch;
        const auto activeBuffer = model.buffers.find(model.activeBuffer);
        const std::string_view activeServer = activeBuffer == model.buffers.end()
                                                  ? std::string_view{}
                                                  : std::string_view(activeBuffer->second.server);
        const auto binding = model.settings.bindings.find("settings");
        for (auto &[name, item] : servers) {
            item.unread = 0;
            item.mentioned = false;
            item.account = false;
        }
        for (const auto &config : model.servers)
            entry(config.name).account = config.sojuAccount;
        for (const auto &[name, network] : model.networks)
            entry(name);
        for (const auto &[id, buffer] : model.buffers) {
            const auto item = servers.find(buffer.server);
            if (item != servers.end() && buffer.kind != "server" && buffer.unread > 0) {
                item->second.unread += buffer.unread;
                item->second.mentioned |= ui_logic::hasUnreadMention(model, buffer);
            }
        }
        const std::string_view current =
            binding == model.settings.bindings.end() ? std::string_view{} : binding->second;
        if (settingsBinding != current) {
            settingsBinding = current;
            settings->tooltip(commandLabel(model, "Settings", "settings"));
        }
        const float padding = railPadding * scale, gap = 6 * scale;
        const float footerExtent = std::max(32.f, 36 * scale);
        const bool showReadAll = hasUnread();
        if (!showReadAll && readAll->visible())
            retire(*readAll, add);
        readAll->setVisible(showReadAll);
        readAll->setEnabled(showReadAll);
        readAll->setExcludeSubtreeFromTabOrder(!showReadAll);
        const float footerRows = showReadAll ? 4.f : 3.f;
        const float footerY = std::max(padding, height - padding - footerRows * footerExtent -
                                                    (footerRows - 1) * gap);
        const float inner = std::max(0.f, width - 2 * padding);
        scroll->setPosition(padding, padding);
        scroll->setSize(inner, std::max(0.f, footerY - 2 * padding));
        scroll->setContentScale(scale);
        const float itemHeight = inner + avatarRowExtra * scale;
        float y = 0;
        std::size_t childIndex = 0;
        const auto place = [&](Entry &item, std::string_view caption, std::string_view avatar,
                               bool collapsed = false, bool hiddenSelected = false,
                               int aggregateUnread = -1, bool aggregateMention = false,
                               std::string_view childStatus = {}, bool childError = false) {
            if (item.placed == epoch)
                return;
            item.placed = epoch;
            auto &node = *item.avatar;
            node.setVisible(true);
            node.setExcludeSubtreeFromTabOrder(false);
            const auto network = model.networks.find(node.key);
            node.sync(caption, avatar, network == model.networks.end() ? nullptr : &network->second,
                      node.key == selected, scale, item.account, collapsed, hiddenSelected,
                      aggregateUnread < 0 ? item.unread : aggregateUnread,
                      aggregateUnread < 0 ? item.mentioned : aggregateMention, childStatus,
                      childError);
            if (canvas->children()[childIndex].get() != &node) {
                auto retained = canvas->removeChild(&node);
                canvas->insertChildAt(childIndex, std::move(retained));
            }
            ++childIndex;
            node.setPosition(0, y);
            node.setSize(inner, itemHeight);
            y += itemHeight + gap;
        };
        // ponytail: profiles are few; saved-order scans keep group order without another index.
        const auto grouped = [&](const ServerConfig &config) {
            return !config.sojuAccount && !config.sojuParent.empty() &&
                   std::any_of(model.servers.begin(), model.servers.end(), [&](const auto &parent) {
                       return parent.sojuAccount && parent.name == config.sojuParent;
                   });
        };
        const auto placeProfile = [&](const ServerConfig &config) {
            place(entry(config.name),
                  config.alias.empty() ? std::string_view(config.name)
                                       : std::string_view(config.alias),
                  config.avatar, false, false, -1, false,
                  !config.sojuParent.empty() && !config.sojuEnabled
                      ? "\nLocally disabled; remote upstream network is unchanged"
                      : "");
        };
        for (const auto &config : model.servers) {
            if (grouped(config))
                continue;
            auto &item = entry(config.name);
            if (!config.sojuAccount) {
                placeProfile(config);
                continue;
            }
            const auto fold = model.settings.workspace.find("soju-group:" + config.name);
            const bool collapsed =
                fold != model.settings.workspace.end() && fold->second == "collapsed";
            int aggregateUnread = item.unread;
            bool aggregateMention = item.mentioned, hiddenSelected = false, childError = false;
            std::string childStatus;
            for (const auto &configChild : model.servers) {
                if (configChild.sojuAccount || configChild.sojuParent != config.name)
                    continue;
                auto &childItem = entry(configChild.name);
                aggregateUnread += childItem.unread;
                aggregateMention |= childItem.mentioned;
                hiddenSelected |= configChild.name == activeServer;
                if (collapsed) {
                    const auto network = model.networks.find(configChild.name);
                    childError |=
                        network != model.networks.end() &&
                        (network->second.state == "Failed" || !network->second.error.empty());
                    childStatus.append("\n").append(configChild.alias.empty() ? configChild.name
                                                                              : configChild.alias);
                    childStatus.append(": ").append(
                        network == model.networks.end() ? "Disconnected" : network->second.state);
                    if (!configChild.sojuEnabled)
                        childStatus.append(" (locally disabled)");
                    if (network != model.networks.end() && !network->second.error.empty())
                        childStatus.append(" · ").append(network->second.error);
                }
            }
            if (!item.capsule) {
                item.capsule = child<RectNode>(*canvas);
                item.capsule->setHitTestVisible(false);
                item.capsule->setZIndex(-1);
            }
            RoundedRectStyle capsuleStyle{};
            capsuleStyle.fill = withAlpha(themeSurfaceColor("surface_container_high"), .45f);
            capsuleStyle.radius = std::min(themeCornerRadius() + 4 * scale, inner / 2);
            item.capsule->setStyle(capsuleStyle);
            item.capsule->setVisible(true);
            const float groupTop = y;
            y += 3 * scale;
            place(item,
                  config.alias.empty() ? std::string_view(config.name)
                                       : std::string_view(config.alias),
                  config.avatar, collapsed, collapsed && hiddenSelected,
                  collapsed ? aggregateUnread : item.unread,
                  collapsed ? aggregateMention : item.mentioned, childStatus, childError);
            for (const auto &configChild : model.servers) {
                if (configChild.sojuAccount || configChild.sojuParent != config.name)
                    continue;
                auto &childItem = entry(configChild.name);
                if (collapsed) {
                    childItem.placed = epoch;
                    childItem.avatar->setVisible(false);
                    childItem.avatar->setExcludeSubtreeFromTabOrder(true);
                } else {
                    placeProfile(configChild);
                }
            }
            item.capsule->setPosition(0, groupTop);
            item.capsule->setSize(inner, std::max(0.f, y - gap + 3 * scale - groupTop));
            y += 3 * scale;
        }
        // ponytail: retain first-seen live-network order; configured profiles keep saved order.
        for (const auto &name : order) {
            auto &item = servers.at(name);
            if (item.seen == epoch)
                place(item, name, {});
        }
        for (auto it = servers.begin(); it != servers.end();) {
            if (it->second.seen != epoch) {
                if (it->second.capsule)
                    canvas->removeChild(it->second.capsule);
                canvas->removeChild(it->second.avatar);
                it = servers.erase(it);
            } else
                ++it;
        }
        for (auto &[name, item] : servers)
            if (item.capsule && !item.account)
                item.capsule->setVisible(false);
        std::erase_if(order, [this](const std::string &name) { return !servers.contains(name); });
        canvas->setSize(inner, y);
        float footerTop = footerY;
        for (auto *row : {mentions, readAll, add, settings}) {
            if (!row->visible())
                continue;
            row->style(scale, row == mentions && model.activeBuffer == GlobalMentionsBufferId);
            row->setPosition(padding, footerTop);
            row->setSize(inner, footerExtent);
            footerTop += footerExtent + gap;
        }
    }
};

ServerRail::ServerRail(Model &model, std::function<void(std::string)> execute,
                       std::function<void(Node &, InputArea *)> retire)
    : impl_(std::make_unique<Impl>(*this, model, std::move(execute), std::move(retire))) {
    setClipChildren(true);
}
ServerRail::~ServerRail() = default;
void ServerRail::sync(std::string_view selected, float width, float height, float scale) {
    impl_->sync(selected, width, height, scale);
}

struct Sidebar::Impl {
    struct Group {
        Row *header = nullptr, *error = nullptr;
        std::map<std::string, Row *> buffers;
        std::vector<Row *> order;
        bool orderDirty = true;
        std::uint64_t seen = 0, configured = 0;
        std::string state;
    };
    struct Conversation {
        Row *row;
        std::string server;
        std::string readMarker;
        std::string caseMapping;
        std::uint64_t revision = 0;
        std::size_t size = 0;
        int unread = -1;
        bool mentioned = false;
        int64_t lastChatTimestamp = 0, lastChatSequence = 0;
        std::string lastChatTime;
        std::size_t pinRank = SIZE_MAX;
    };

    Sidebar &owner;
    Model &model;
    std::function<void(std::string)> execute;
    Popup popup;
    Input *search = nullptr;
    Select *compactFilter = nullptr;
    LucideIcon *searchIcon = nullptr;
    Button *all = nullptr, *unread = nullptr, *dms = nullptr, *archived = nullptr, *clear = nullptr;
    ScrollView *scroll = nullptr;
    Node *canvas = nullptr, *filterHeader = nullptr;
    Label *empty = nullptr;
    std::map<std::string, Group> groups;
    std::map<std::string, Conversation> conversations;
    std::vector<std::string> cachedPins;
    decltype(Settings::fools) cachedFools;
    bool cachedFoolUnread = false;
    std::map<std::string, std::size_t> pinRanks;
    ui_logic::SidebarFilter filter = ui_logic::SidebarFilter::All;
    std::uint64_t epoch = 0;
    int archivedUnread = -1;
    std::array<float, 4> filterWidths{};
    TextFont measuredFilterFont;
    float measuredFilterScale = 0, measuredFilterRenderScale = 0;
    std::uint64_t measuredFilterGeneration = 0;
    int measuredArchiveUnread = -1;
    std::size_t canvasOrder = 0;
    float zoom = 1, rowWidth = 0, viewportHeight = 0;
    float nativeConversationHeight = 0;
    SidebarMode mode = SidebarMode::Expanded;
    bool hintsActive = false, revealFocused = false;
    bool restoringHintFocus = false;
    float hintScroll = 0, hintScrollPosition = 0, hintMaxScroll = 0;
    bool hintScrollRestorePending = false, hintNormalGeometryReady = false;
    // Keep inactive strings alive so prefix edits reuse their capacity.
    std::vector<std::string> hintCandidates;
    std::size_t hintCount = 0;
    mutable std::vector<SidebarHintRect> hintRectCache;
    std::string selectedServer, lastPinnedBuffer;

    void discardHintScrollRestore() {
        if (!hintScrollRestorePending)
            return;
        hintScrollRestorePending = false;
        hintNormalGeometryReady = false;
        restoringHintFocus = false;
        scroll->cancelPendingScrollRequests();
    }

    void resetViewScroll() {
        hintScrollRestorePending = false;
        hintNormalGeometryReady = false;
        restoringHintFocus = false;
        if (scroll) {
            scroll->requestScrollIntoView(nullptr);
            scroll->setScrollOffset(0);
            scroll->requestScrollToOffset(0);
        }
    }

    Impl(Sidebar &node, Model &value, std::function<void(std::string)> command)
        : owner(node), model(value), execute(std::move(command)), popup(node) {
        filterHeader = child<Node>(owner);
        filterHeader->setClipChildren(true);
        searchIcon = child<LucideIcon>(*filterHeader, "search");
        searchIcon->setZIndex(1);
        searchIcon->setHitTestVisible(false);
        search = child<Input>(*filterHeader);
        search->setPlaceholder("Filter loaded names…");
        search->setClearButtonEnabled(true);
        search->inputArea()->setTooltip("Filter loaded channel, person and network names");
        search->inputArea()->setTabFocusKey("sidebar-search");
        search->setOnChange([this](const std::string &) { resetViewScroll(); });
        for (auto entry : {std::pair{"All", ui_logic::SidebarFilter::All},
                           std::pair{"Unread", ui_logic::SidebarFilter::Unread},
                           std::pair{"DMs", ui_logic::SidebarFilter::Dms},
                           std::pair{"Archived", ui_logic::SidebarFilter::Archived}}) {
            auto *button = child<Button>(*filterHeader);
            button->setText(entry.first);
            button->inputArea()->setTabFocusKey(
                entry.second == ui_logic::SidebarFilter::All      ? "sidebar-filter:all"
                : entry.second == ui_logic::SidebarFilter::Unread ? "sidebar-filter:unread"
                : entry.second == ui_logic::SidebarFilter::Dms    ? "sidebar-filter:dms"
                                                                  : "sidebar-filter:archived");
            button->setVariant(ButtonVariant::Tab);
            button->setOnClick([this, selected = entry.second] {
                filter = selected;
                resetViewScroll();
            });
            button->setTooltip(entry.second == ui_logic::SidebarFilter::All
                                   ? "All loaded conversations"
                               : entry.second == ui_logic::SidebarFilter::Unread
                                   ? "Conversations with unread messages"
                               : entry.second == ui_logic::SidebarFilter::Dms
                                   ? "Direct messages"
                                   : "Archived conversations (unread counts are preserved)");
            if (entry.second == ui_logic::SidebarFilter::All)
                all = button;
            else if (entry.second == ui_logic::SidebarFilter::Unread)
                unread = button;
            else if (entry.second == ui_logic::SidebarFilter::Dms)
                dms = button;
            else
                archived = button;
        }
        compactFilter = child<Select>(*filterHeader);
        // Select owns its native trigger but does not expose an inputArea() accessor.
        for (const auto &node : compactFilter->children())
            if (auto *area = dynamic_cast<InputArea *>(node.get()); area && area->focusable()) {
                area->setTabFocusKey("sidebar-filter-menu");
                break;
            }
        compactFilter->setOptions({"All", "Unread", "DMs", "Archived"});
        compactFilter->setOptionIcons({"list", "inbox", "message", "archive"});
        compactFilter->setOnSelectionChanged([this](std::size_t index, std::string_view) {
            filter = index == 1   ? ui_logic::SidebarFilter::Unread
                     : index == 2 ? ui_logic::SidebarFilter::Dms
                     : index == 3 ? ui_logic::SidebarFilter::Archived
                                  : ui_logic::SidebarFilter::All;
            resetViewScroll();
        });
        scroll = child<ScrollView>(owner);
        scroll->setViewportPaddingH(0);
        scroll->setViewportPaddingV(0);
        scroll->setReserveScrollbarGutter(true);
        scroll->setOnScrollChanged([this](float) { refreshPreviews(); });
        canvas = child<Node>(*scroll->content());
        empty = child<Label>(*canvas);
        empty->setUseMarkup(false);
        empty->setMaxLines(0);
        clear = child<Button>(*canvas);
        clear->setText("Clear filters");
        clear->inputArea()->setTabFocusKey("sidebar-clear-filters");
        clear->setOnClick([this] {
            search->setValue("");
            filter = ui_logic::SidebarFilter::All;
            resetViewScroll();
        });
    }

    Group &group(const std::string &name) {
        auto [it, inserted] = groups.try_emplace(name);
        auto &result = it->second;
        result.seen = epoch;
        if (inserted) {
            result.header = child<Row>(*canvas, "server");
            result.header->key = name;
            result.header->setTabFocusKey("sidebar-server:" + name);
            result.header->title->setText(name);
            result.header->inlineSubtitle = true;
            result.header->emphasized = true;
            result.header->activate = [this](Row &row) {
                const auto buffer =
                    std::find_if(model.buffers.begin(), model.buffers.end(), [&](const auto &item) {
                        return item.second.server == row.key && item.second.kind == "server";
                    });
                execute(buffer == model.buffers.end() ? "server:" + row.key
                                                      : "buffer:" + buffer->first);
            };
            result.header->menu = [this](Row &row, std::optional<InputArea::PointerData> pointer) {
                networkMenu(row, pointer);
            };
            result.header->reveal = [this](Row &row) {
                if (!restoringHintFocus) {
                    discardHintScrollRestore();
                    scroll->requestScrollIntoView(&row);
                }
            };
            result.error = child<Row>(*canvas, "terminal");
            result.error->key = name;
            result.error->setTabFocusKey("sidebar-connect:" + name);
            result.error->error = true;
            result.error->title->setText("Reconnect");
            result.error->activate = [this](Row &row) {
                model.actions.push_back({"connect", {}, row.key, {}});
            };
            result.error->reveal = result.header->reveal;
        }
        return result;
    }

    void networkMenu(Row &row, std::optional<InputArea::PointerData> pointer) {
        serverMenu(model, popup, row, zoom, row.key, execute, pointer);
    }

    void bufferMenu(Row &row, std::optional<InputArea::PointerData> pointer) {
        const std::string id = row.key;
        const auto buffer = model.buffers.find(id);
        if (buffer == model.buffers.end())
            return;
        const bool chat = buffer->second.kind == "channel" || buffer->second.kind == "query";
        const bool mute = !buffer->second.muted, archive = !buffer->second.archived;
        const bool pinned =
            std::find(model.settings.pinnedBuffers.begin(), model.settings.pinnedBuffers.end(),
                      id) != model.settings.pinnedBuffers.end();
        std::vector<ContextMenuEntry> menu;
        menu.reserve(buffer->second.kind == "server" ? 3 : 5);
        auto conversation = menuEntry("Conversation", "messages");
        conversation.children.push_back(menuEntry("Open conversation", "message-circle", 0));
        if (chat)
            conversation.children.push_back(menuEntry(archive ? "Archive" : "Unarchive",
                                                      archive ? "archive" : "archive-off", 5));
        if (chat) {
            conversation.children.push_back(
                menuEntry(pinned ? "Unpin chat" : "Pin chat", pinned ? "unpin" : "pin", 6));
        }
        menu.push_back(std::move(conversation));
        auto history = menuEntry("History", "history");
        history.children.push_back(menuEntry("Load older messages", "history", 1));
        menu.push_back(std::move(history));
        auto notifications = menuEntry("Notifications", "bell");
        notifications.children.push_back(menuEntry("Mark read", "circle-check", 2));
        if (chat)
            notifications.children.push_back(
                menuEntry(mute ? "Mute" : "Unmute", mute ? "bell-off" : "bell", 4));
        menu.push_back(std::move(notifications));
        if (buffer->second.kind != "server") {
            ContextMenuEntry separator;
            separator.separator = true;
            menu.push_back(std::move(separator));
            menu.push_back(menuEntry("Close buffer", "x", 3));
        }
        popup.show(
            row, zoom, std::move(menu),
            [this, id, mute, archive, pinned](std::size_t index) {
                const auto current = model.buffers.find(id);
                if (current == model.buffers.end())
                    return;
                if (index == 6) {
                    if (current->second.kind == "channel" || current->second.kind == "query") {
                        model.actions.push_back({"pin_buffer", id, pinned ? "off" : "on", {}});
                    }
                    return;
                }
                if (index == 4 || index == 5) {
                    if (current->second.kind == "channel" || current->second.kind == "query")
                        model.actions.push_back({index == 4 ? "mute_buffer" : "archive_buffer",
                                                 id,
                                                 (index == 4 ? mute : archive) ? "on" : "off",
                                                 {}});
                    return;
                }
                if (index == 0)
                    execute("buffer:" + id);
                else if (index < 3 || (index == 3 && current->second.kind != "server"))
                    model.actions.push_back({index == 1   ? "history"
                                             : index == 2 ? "read"
                                                          : "close_buffer",
                                             id,
                                             {},
                                             {}});
            },
            pointer);
    }

    void refreshPreviews() {
        const float top = scroll->scrollOffset() - 64 * zoom;
        const float bottom = top + viewportHeight + 128 * zoom;
        for (auto &[id, conversation] : conversations) {
            auto &row = *conversation.row;
            if (!row.matched || row.y() + row.height() < top || row.y() > bottom)
                continue;
            auto found = model.buffers.find(id);
            if (found == model.buffers.end())
                continue;
            const auto &buffer = found->second;
            ui_logic::sidebarPreview(model, buffer, buffer.draft, row.preview);
            row.scratch.clear();
            row.scratch.append(buffer.target)
                .append("\n")
                .append(buffer.server)
                .append("\n")
                .append(row.preview);
            if (buffer.unread > 0)
                row.scratch.append("\nUnread: ").append(std::to_string(buffer.unread));
            if (row.mentioned)
                row.scratch.append("\nUnread mention");
            if (row.pinnedCurrent)
                row.scratch.append("\nCurrent conversation (outside regular filters)");
            row.tooltip(row.scratch);
            row.paint();
        }
    }

    void orderChild(Node &node) {
        if (canvasOrder < canvas->children().size() &&
            canvas->children()[canvasOrder].get() != &node) {
            auto retained = canvas->removeChild(&node);
            canvas->insertChildAt(canvasOrder, std::move(retained));
        }
        ++canvasOrder;
    }

    void place(Row &row, float &y, float height, float indent = 0) {
        orderChild(row);
        row.allocatedMinimum = height;
        row.setPosition(indent, y);
        row.setSize(std::max(0.f, rowWidth - indent), height);
        y += height + 4 * zoom;
    }

    float conversationHeight() const {
        return std::max(
            {32.f, mode == SidebarMode::Compact ? rowWidth : 48 * zoom, nativeConversationHeight});
    }

    void layoutRows(Renderer &renderer) {
        nativeConversationHeight = 0;
        for (const auto &node : canvas->children()) {
            auto *row = dynamic_cast<Row *>(node.get());
            if (!row || !row->visible())
                continue;
            row->nativeMinimum = row->minimumHeight(renderer);
            if (row->unreadCountBadge)
                nativeConversationHeight = std::max(nativeConversationHeight, row->nativeMinimum);
        }
        float delta = 0;
        bool resized = false;
        for (const auto &node : canvas->children()) {
            if (!node->visible())
                continue;
            if (delta != 0)
                node->setPosition(node->x(), node->y() + delta);
            if (auto *row = dynamic_cast<Row *>(node.get())) {
                const float height = row->unreadCountBadge
                                         ? conversationHeight()
                                         : std::max(row->allocatedMinimum, row->nativeMinimum);
                const float change = height - row->height();
                if (change != 0) {
                    row->setSize(row->width(), height);
                    delta += change;
                    resized = true;
                }
            }
        }
        if (!resized)
            return;
        // Adjust only heights and positions: retained order, gaps and focus stay intact.
        canvas->setSize(canvas->width(), canvas->height() + delta);
        if (hintsActive) {
            hintMaxScroll = std::max(0.f, canvas->height() - viewportHeight);
            hintScrollPosition = std::clamp(hintScrollPosition, 0.f, hintMaxScroll);
            scroll->requestScrollToOffset(hintScrollPosition);
        }
        refreshPreviews();
    }

    void placeGroup(const std::string &name, Group &group, float &y, std::size_t &matched) {
        const auto network = model.networks.find(name);
        const Network *net = network == model.networks.end() ? nullptr : &network->second;
        const bool expanded = mode == SidebarMode::Expanded;
        const bool details = expanded && !hintsActive;
        group.header->setVisible(details);
        group.header->setExcludeSubtreeFromTabOrder(!details);
        const std::string_view state = net ? std::string_view(net->state) : "Disconnected";
        if (group.state != state) {
            group.state = state;
            group.header->subtitle->setText(state);
        }
        group.header->scratch.assign(name);
        group.header->scratch.append(" · Open server or connection settings\n");
        group.header->scratch.append(net ? net->state : "Disconnected");
        group.header->tooltip(group.header->scratch);
        group.header->error = net && !net->error.empty();
        group.header->style(zoom);
        if (details)
            place(*group.header, y, std::max(32.f, 36 * zoom));
        const bool hasError = details && net && !net->error.empty();
        group.error->setVisible(hasError);
        group.error->setExcludeSubtreeFromTabOrder(!hasError);
        if (hasError) {
            group.error->title->setText(
                net->state == "Disconnected" || net->state == "Failed" ? "Connect" : "Reconnect");
            group.error->subtitle->setText(net->error);
            group.error->tooltip(net->error);
            group.error->style(zoom);
            place(*group.error, y, std::max(32.f, 54 * zoom), 16 * zoom);
        }
        if (group.orderDirty) {
            group.order.clear();
            group.order.reserve(group.buffers.size());
            for (auto &[id, row] : group.buffers)
                group.order.push_back(row);
            std::sort(group.order.begin(), group.order.end(), [this](const Row *a, const Row *b) {
                const auto left = conversations.at(a->key).pinRank;
                const auto right = conversations.at(b->key).pinRank;
                return left != right
                           ? left < right
                           : chatActivityBefore(model.buffers.at(a->key), model.buffers.at(b->key));
            });
            group.orderDirty = false;
        }
        for (auto *row : group.order) {
            if (!row->matched || row->pinnedCurrent)
                continue;
            ++matched;
            place(*row, y, conversationHeight(), expanded ? 4 * zoom : 0);
        }
        y += 6 * zoom;
    }

    void layoutFilters(Renderer &renderer) {
        if (mode != SidebarMode::Expanded)
            return;
        const std::array buttons{all, unread, dms, archived};
        if (measuredFilterScale != zoom || measuredFilterRenderScale != renderer.renderScale() ||
            measuredFilterGeneration != renderer.textMetricsGeneration() ||
            measuredFilterFont != all->label()->font() || measuredArchiveUnread != archivedUnread) {
            measuredFilterScale = zoom;
            measuredFilterRenderScale = renderer.renderScale();
            measuredFilterGeneration = renderer.textMetricsGeneration();
            measuredFilterFont = all->label()->font();
            measuredArchiveUnread = archivedUnread;
            for (std::size_t i = 0; i < buttons.size(); ++i) {
                const auto *label = buttons[i]->label();
                filterWidths[i] =
                    std::ceil(renderer.measureText(label->text(), label->fontSize(), label->font())
                                  .width) +
                    12 * zoom;
            }
        }
        const float gap = 6 * zoom;
        const float available = filterHeader->width();
        const float required =
            filterWidths[0] + filterWidths[1] + filterWidths[2] + filterWidths[3] + 3 * gap;
        const bool shortHeader = owner.height() < 360 * zoom;
        if (shortHeader) {
            const float selectorWidth =
                std::min(available, std::max(96 * zoom, filterWidths[3] + 20 * zoom));
            const float searchWidth = std::max(0.f, available - selectorWidth - gap);
            search->setSize(searchWidth, search->height());
            compactFilter->setPosition(available - selectorWidth, 0);
            compactFilter->setSize(selectorWidth, compactFilter->height());
            return;
        }
        const bool tabsFit = required <= available;
        compactFilter->setVisible(!tabsFit);
        compactFilter->setExcludeSubtreeFromTabOrder(tabsFit);
        if (!tabsFit) {
            compactFilter->setPosition(0, all->y());
            compactFilter->setSize(available, all->height());
            compactFilter->setControlHeight(all->height());
        }
        float x = 0;
        for (std::size_t i = 0; i < buttons.size(); ++i) {
            auto *button = buttons[i];
            button->setVisible(tabsFit);
            button->setExcludeSubtreeFromTabOrder(!tabsFit);
            button->setPosition(x, button->y());
            button->setSize(filterWidths[i], button->height());
            x += filterWidths[i] + gap;
        }
    }

    void sync(std::string_view selected, std::string_view focused, float width, float height,
              float scale, SidebarMode effectiveMode) {
        zoom = scale;
        mode = effectiveMode;
        const bool expanded = mode == SidebarMode::Expanded;
        owner.setVisible(mode != SidebarMode::Hidden);
        owner.setExcludeSubtreeFromTabOrder(mode == SidebarMode::Hidden);
        if (selectedServer != selected) {
            hintScrollRestorePending = false;
            hintNormalGeometryReady = false;
            restoringHintFocus = false;
            hintScroll = hintScrollPosition = 0;
            selectedServer.assign(selected);
            scroll->setScrollOffset(0);
            scroll->requestScrollIntoView(nullptr);
            scroll->requestScrollToOffset(0);
        }
        if (mode == SidebarMode::Hidden)
            return;
        ++epoch;
        const bool shortHeader = height < 360 * scale;
        int unreadInArchive = 0;
        for (const auto &[id, buffer] : model.buffers)
            if (buffer.archived && buffer.server == selected &&
                (buffer.kind == "channel" || buffer.kind == "query"))
                unreadInArchive += buffer.unread;
        if (archivedUnread != unreadInArchive) {
            archivedUnread = unreadInArchive;
            const auto label = archivedUnread ? "Archived · " + std::to_string(archivedUnread)
                                              : std::string("Archived");
            archived->setText(label);
            compactFilter->setOptions({"All", "Unread", "DMs", label});
        }
        const float padding = (expanded ? 8.f : 4.f) * scale, gap = 6 * scale;
        const float verticalPadding = padding;
        const float inner = std::max(0.f, width - 2 * padding);
        const float searchHeight = std::max(32.f, (shortHeader ? 36.f : 40.f) * scale);
        const float filterHeight = std::max(32.f, 32 * scale);
        const float filterTop = searchHeight + 8 * scale;
        const float headerHeight =
            expanded ? (shortHeader ? searchHeight : filterTop + filterHeight) : searchHeight;
        const float listTop = verticalPadding + headerHeight + 8 * scale;
        viewportHeight = std::max(0.f, height - verticalPadding - listTop);
        scroll->setPosition(padding, listTop);
        scroll->setSize(inner, viewportHeight);
        scroll->setScrollbarVisible(expanded);
        scroll->setReserveScrollbarGutter(expanded);
        scroll->setContentScale(scale);
        rowWidth = expanded ? std::max(0.f, inner - scroll->scrollbarGutter()) : inner;
        const float headerWidth = inner;
        filterHeader->setVisible(true);
        filterHeader->setExcludeSubtreeFromTabOrder(false);
        filterHeader->setPosition(padding, verticalPadding);
        filterHeader->setSize(headerWidth, headerHeight);
        const float selectorWidth = std::min(96 * scale, std::max(64 * scale, inner * .25f));
        const float searchWidth = shortHeader ? std::max(0.f, inner - selectorWidth - gap) : inner;
        compactFilter->setVisible(!expanded || shortHeader);
        compactFilter->setExcludeSubtreeFromTabOrder(expanded && !shortHeader);
        compactFilter->setPosition(expanded ? searchWidth + gap : 0, 0);
        compactFilter->setSize(expanded ? selectorWidth : inner, searchHeight);
        compactFilter->setControlHeight(searchHeight);
        compactFilter->setFontSize(12 * scale);
        compactFilter->setHorizontalPadding(8 * scale);
        compactFilter->setGlyphSize(12 * scale);
        compactFilter->setRadius(themeCornerRadius());
        compactFilter->setSelectedIndexSilently(filter == ui_logic::SidebarFilter::Unread     ? 1
                                                : filter == ui_logic::SidebarFilter::Dms      ? 2
                                                : filter == ui_logic::SidebarFilter::Archived ? 3
                                                                                              : 0);
        search->setVisible(expanded);
        search->setExcludeSubtreeFromTabOrder(!expanded);
        searchIcon->setVisible(expanded);
        searchIcon->setPosition(10 * scale, (searchHeight - 18 * scale) / 2);
        searchIcon->setSize(18 * scale, 18 * scale);
        searchIcon->setColor(themeForegroundColor("on_surface_variant"));
        search->setPosition(0, 0);
        search->setSize(searchWidth, searchHeight);
        search->setControlHeight(searchHeight);
        search->setHorizontalPadding(34 * scale);
        search->setFrameRadius(themeCornerRadius());
        search->setFontSize(14 * scale);
        search->setFontFamily(themeUiFont().family);
        search->setFontFeatures(themeUiFont().features);
        const float filterPadding = 6 * scale;
        for (auto [button, value] : {std::pair{all, ui_logic::SidebarFilter::All},
                                     std::pair{unread, ui_logic::SidebarFilter::Unread},
                                     std::pair{dms, ui_logic::SidebarFilter::Dms},
                                     std::pair{archived, ui_logic::SidebarFilter::Archived}}) {
            button->setVisible(expanded && !shortHeader);
            button->setExcludeSubtreeFromTabOrder(!expanded || shortHeader);
            button->setPosition(0, filterTop);
            button->setSize(button->width(), filterHeight);
            button->setPadding(0, filterPadding);
            button->setControlHeight(filterHeight);
            button->setRadius(themeCornerRadius());
            button->setFontSize(12 * scale);
            button->label()->setFontFamily(themeUiFont().family);
            button->label()->setFontFeatures(themeUiFont().features);
            button->setSelected(filter == value);
        }
        for (auto it = conversations.begin(); it != conversations.end();) {
            const auto buffer = model.buffers.find(it->first);
            if (buffer == model.buffers.end() || buffer->second.kind == "server") {
                groups.at(it->second.server).buffers.erase(it->first);
                groups.at(it->second.server).orderDirty = true;
                canvas->removeChild(it->second.row);
                it = conversations.erase(it);
            } else
                ++it;
        }
        for (const auto &server : model.servers) {
            auto &target = group(server.name);
            target.configured = epoch;
            target.header->title->setText(server.alias.empty() ? server.name : server.alias);
        }
        for (const auto &[name, network] : model.networks) {
            auto &target = group(name);
            if (target.configured != epoch)
                target.header->title->setText(name);
        }
        if (cachedPins != model.settings.pinnedBuffers) {
            cachedPins = model.settings.pinnedBuffers;
            pinRanks.clear();
            for (std::size_t i = 0; i < cachedPins.size(); ++i)
                pinRanks.emplace(cachedPins[i], i);
        }
        const bool foolRulesChanged =
            cachedFoolUnread != model.settings.foolUnread || cachedFools != model.settings.fools;
        if (foolRulesChanged) {
            cachedFoolUnread = model.settings.foolUnread;
            cachedFools = model.settings.fools;
        }
        Row *pinnedCurrent = nullptr;
        for (const auto &[id, buffer] : model.buffers) {
            auto &target = group(buffer.server);
            if (buffer.kind == "server")
                continue;
            auto it = conversations.find(id);
            const bool inserted = it == conversations.end();
            if (inserted) {
                Conversation value{};
                value.server = buffer.server;
                it = conversations.emplace(id, std::move(value)).first;
            }
            auto &conversation = it->second;
            if (inserted) {
                conversation.row = child<Row>(*canvas, "type", true);
                conversation.row->unreadCountBadge = true;
                conversation.row->key = id;
                conversation.row->setTabFocusKey("buffer:" + id);
                conversation.row->activate = [this](Row &row) {
                    if (model.buffers.contains(row.key))
                        execute("buffer:" + row.key);
                };
                conversation.row->menu = [this](Row &row,
                                                std::optional<InputArea::PointerData> pointer) {
                    bufferMenu(row, pointer);
                };
                conversation.row->reveal = [this](Row &row) {
                    if (!restoringHintFocus) {
                        discardHintScrollRestore();
                        scroll->requestScrollIntoView(&row);
                    }
                };
                target.buffers.emplace(id, conversation.row);
                target.orderDirty = true;
            } else if (conversation.server != buffer.server) {
                groups.at(conversation.server).buffers.erase(id);
                groups.at(conversation.server).orderDirty = true;
                target.buffers.emplace(id, conversation.row);
                conversation.server = buffer.server;
                target.orderDirty = true;
            }
            auto &row = *conversation.row;
            const auto rank = pinRanks.find(id);
            const auto pinRank = rank == pinRanks.end() ? SIZE_MAX : rank->second;
            if (conversation.lastChatTimestamp != buffer.lastChatTimestamp ||
                conversation.lastChatSequence != buffer.lastChatSequence ||
                conversation.lastChatTime != buffer.lastChatTime ||
                conversation.pinRank != pinRank) {
                conversation.lastChatTimestamp = buffer.lastChatTimestamp;
                conversation.lastChatSequence = buffer.lastChatSequence;
                conversation.lastChatTime = buffer.lastChatTime;
                conversation.pinRank = pinRank;
                target.orderDirty = true;
            }
            const std::string_view caption = buffer.target;
            const bool isTarget = buffer.kind == "channel" || buffer.kind == "query";
            const bool filterMatch = !hintsActive && buffer.server == selected &&
                                     (expanded || isTarget) &&
                                     ui_logic::sidebarMatches(buffer, search->value(), filter);
            const bool pin =
                !buffer.archived && filter != ui_logic::SidebarFilter::Archived && id == focused;
            row.pinnedCurrent = !hintsActive && pin && !filterMatch;
            row.matched = hintsActive
                              ? !buffer.archived && buffer.server == selected && isTarget &&
                                    std::binary_search(hintCandidates.begin(),
                                                       hintCandidates.begin() + hintCount, id)
                              : filterMatch || pin;
            if (row.pinnedCurrent) {
                pinnedCurrent = &row;
                row.scratch.assign("Current · ");
                row.scratch.append(buffer.kind == "mentions" ? "All networks"
                                   : buffer.server.empty()   ? "No network"
                                                             : buffer.server);
                row.subtitle->setText(row.scratch);
            } else
                row.subtitle->setText(conversation.pinRank != SIZE_MAX ? "Pinned" : "");
            row.setVisible(row.matched);
            row.setExcludeSubtreeFromTabOrder(!row.matched);
            row.title->setText(caption);
            row.draft = !buffer.draft.empty();
            row.monospace = buffer.kind == "channel";
            row.avatarOnly = !expanded;
            const auto network = model.networks.find(buffer.server);
            const std::string_view mapping = network == model.networks.end()
                                                 ? std::string_view{"rfc1459"}
                                                 : std::string_view(network->second.caseMapping);
            if (conversation.unread != buffer.unread ||
                conversation.size != buffer.messages.size() ||
                conversation.revision != buffer.messagesRevision ||
                conversation.readMarker != buffer.readMarker ||
                conversation.caseMapping != mapping || foolRulesChanged) {
                conversation.unread = buffer.unread;
                conversation.size = buffer.messages.size();
                conversation.revision = buffer.messagesRevision;
                conversation.readMarker = buffer.readMarker;
                conversation.caseMapping = mapping;
                conversation.mentioned = ui_logic::hasUnreadMention(model, buffer);
            }
            row.mentioned = conversation.mentioned;
            row.syncAvatar(buffer.kind == "channel" && model.settings.avatars
                               ? std::string_view(buffer.avatarPath)
                               : std::string_view{},
                           caption);
            row.emphasized = buffer.unread > 0;
            if (buffer.unread > 0) {
                const std::string count = buffer.unread > (expanded ? 99 : 9)
                                              ? (expanded ? "99+" : "9+")
                                              : std::to_string(buffer.unread);
                row.badge->setText(row.mentioned ? "@" + count : count);
            } else
                row.badge->setText("");
            row.style(scale, id == focused);
        }
        if (!hintsActive) {
            if (pinnedCurrent) {
                if (lastPinnedBuffer != pinnedCurrent->key) {
                    if (!hintScrollRestorePending)
                        revealFocused = true;
                    lastPinnedBuffer = pinnedCurrent->key;
                }
            } else
                lastPinnedBuffer.clear();
        }
        for (auto it = groups.begin(); it != groups.end();) {
            if (it->second.seen != epoch) {
                canvas->removeChild(it->second.header);
                canvas->removeChild(it->second.error);
                it = groups.erase(it);
            } else
                ++it;
        }
        float y = 0;
        canvasOrder = 0;
        std::size_t matched = 0;
        if (pinnedCurrent)
            place(*pinnedCurrent, y, conversationHeight(), expanded ? 4 * zoom : 0);
        for (auto &[name, value] : groups) {
            if (name == selected) {
                placeGroup(name, value, y, matched);
                continue;
            }
            for (auto *row : {value.header, value.error}) {
                row->setVisible(false);
                row->setExcludeSubtreeFromTabOrder(true);
            }
        }
        const bool hasSelected = groups.contains(selectedServer);
        const bool noNetworks = groups.empty();
        const bool showEmpty = expanded && !hintsActive && !matched;
        empty->setVisible(showEmpty);
        const bool showClear =
            showEmpty && (!search->value().empty() || filter != ui_logic::SidebarFilter::All);
        clear->setVisible(showClear);
        clear->setExcludeSubtreeFromTabOrder(!showClear);
        if (showEmpty) {
            orderChild(*empty);
            const char *filterName = filter == ui_logic::SidebarFilter::Unread ? "Unread"
                                     : filter == ui_logic::SidebarFilter::Dms  ? "DMs"
                                                                               : "All";
            empty->setText(
                noNetworks ? "Your conversations live here. Add a network from the + button "
                             "in the server rail."
                : !hasSelected
                    ? (pinnedCurrent ? "No other conversations are shown for this network "
                                       "selection. Choose a network from the server rail."
                                     : "Select a network from the server rail.")
                    : std::string(pinnedCurrent ? "No other conversations match "
                                                : "No conversations match ") +
                          filterName + (search->value().empty() ? "." : " and this search."));
            labelFont(*empty, 13 * scale);
            empty->setColor(themeForegroundColor("on_surface_variant"));
            empty->setMaxWidth(rowWidth);
            empty->setPosition(0, y);
            y += 78 * scale;
            if (showClear) {
                orderChild(*clear);
                clear->setPosition(0, y);
                const float clearHeight = std::max(32.f, 34 * scale);
                clear->setSize(rowWidth, clearHeight);
                clear->setControlHeight(clearHeight);
                clear->setRadius(themeCornerRadius());
                clear->setFontSize(13 * scale);
                clear->label()->setFontFamily(themeUiFont().family);
                clear->label()->setFontFeatures(themeUiFont().features);
                y += clearHeight + 4 * scale;
            }
        }
        canvas->setSize(rowWidth, y);
        if (hintsActive) {
            hintMaxScroll = std::max(0.f, y - viewportHeight);
            const float bounded = std::clamp(hintScrollPosition, 0.f, hintMaxScroll);
            if (bounded != hintScrollPosition) {
                hintScrollPosition = bounded;
                scroll->requestScrollToOffset(bounded);
            }
        }
        if (hintScrollRestorePending && !hintsActive) {
            hintNormalGeometryReady = true;
            scroll->requestScrollIntoView(nullptr);
            scroll->requestScrollToOffset(hintScroll);
        }
        if (revealFocused && !hintsActive) {
            const auto row = conversations.find(std::string(focused));
            if (row != conversations.end() && row->second.row->matched)
                scroll->requestScrollIntoView(row->second.row);
            revealFocused = false;
        }
        refreshPreviews();
    }
};

Sidebar::Sidebar(Model &model, std::function<void(std::string)> execute)
    : impl_(std::make_unique<Impl>(*this, model, std::move(execute))) {
    setClipChildren(true);
}
Sidebar::~Sidebar() = default;
void Sidebar::doLayout(Renderer &renderer) {
    impl_->layoutFilters(renderer);
    impl_->layoutRows(renderer);
    Node::doLayout(renderer);
    if (impl_->hintsActive) {
        impl_->hintMaxScroll = impl_->scroll->maxScrollOffset();
        impl_->hintScrollPosition =
            std::clamp(impl_->hintScrollPosition, 0.f, impl_->hintMaxScroll);
    } else if (impl_->hintScrollRestorePending && impl_->hintNormalGeometryReady) {
        const float restored = std::clamp(impl_->hintScroll, 0.f, impl_->scroll->maxScrollOffset());
        if (std::abs(impl_->scroll->scrollOffset() - restored) < .01f)
            impl_->hintScrollRestorePending = false;
    }
}
void Sidebar::sync(std::string_view selectedServer, std::string_view focusedBuffer, float width,
                   float height, float scale, SidebarMode effectiveMode) {
    impl_->sync(selectedServer, focusedBuffer, width, height, scale, effectiveMode);
}
void Sidebar::setHintCandidates(std::span<const std::string_view> buffers) {
    auto &s = *impl_;
    if (!s.hintsActive) {
        if (!s.hintScrollRestorePending)
            s.hintScroll = s.scroll->scrollOffset();
        s.hintScrollRestorePending = false;
        s.hintNormalGeometryReady = false;
        s.hintScrollPosition = s.hintScroll;
        s.hintsActive = true;
    } else
        s.hintScrollPosition = 0;
    s.hintCount = buffers.size();
    if (s.hintCandidates.size() < s.hintCount)
        s.hintCandidates.resize(s.hintCount);
    std::size_t matched = 0;
    for (std::size_t i = 0; i < s.hintCount; ++i) {
        s.hintCandidates[i].assign(buffers[i]);
        const auto buffer = s.model.buffers.find(s.hintCandidates[i]);
        if (buffer != s.model.buffers.end() && !buffer->second.archived &&
            buffer->second.server == s.selectedServer &&
            (buffer->second.kind == "channel" || buffer->second.kind == "query"))
            ++matched;
    }
    std::sort(s.hintCandidates.begin(), s.hintCandidates.begin() + s.hintCount);
    const float contentHeight = matched * (s.conversationHeight() + 4 * s.zoom) + 6 * s.zoom;
    s.hintMaxScroll = std::max(0.f, contentHeight - s.viewportHeight);
    s.hintScrollPosition = std::clamp(s.hintScrollPosition, 0.f, s.hintMaxScroll);
    s.scroll->requestScrollIntoView(nullptr);
    s.scroll->requestScrollToOffset(s.hintScrollPosition);
    s.hintRectCache.clear();
    markLayoutDirty();
}
void Sidebar::clearHintCandidates(bool committed) {
    auto &s = *impl_;
    if (!s.hintsActive)
        return;
    s.hintsActive = false;
    s.restoringHintFocus = !committed;
    s.hintScrollRestorePending = !committed;
    s.hintNormalGeometryReady = false;
    s.hintCount = 0;
    s.hintRectCache.clear();
    if (committed)
        s.revealFocused = true;
    else {
        s.scroll->requestScrollIntoView(nullptr);
        s.scroll->requestScrollToOffset(s.hintScroll);
    }
    markLayoutDirty();
}
bool Sidebar::finishHintFocusRestore() {
    auto &s = *impl_;
    if (!s.restoringHintFocus)
        return false;
    s.restoringHintFocus = false;
    if (!s.hintScrollRestorePending)
        return false;
    s.scroll->requestScrollIntoView(nullptr);
    s.scroll->requestScrollToOffset(s.hintScroll);
    markLayoutDirty();
    return true;
}
void Sidebar::cancelHintScrollRestore() { impl_->discardHintScrollRestore(); }
void Sidebar::scrollHintCandidates(float logicalDelta) {
    auto &s = *impl_;
    if (!s.hintsActive)
        return;
    s.hintScrollPosition = std::clamp(s.hintScrollPosition + logicalDelta, 0.f, s.hintMaxScroll);
    s.scroll->requestScrollToOffset(s.hintScrollPosition);
    markLayoutDirty();
}
std::span<const SidebarHintRect> Sidebar::hintRects() const {
    auto &s = *impl_;
    s.hintRectCache.clear();
    if (!s.hintsActive)
        return s.hintRectCache;
    s.hintRectCache.reserve(s.hintCount);
    float viewLeft, viewTop, viewRight, viewBottom;
    Node::transformedBounds(s.scroll, viewLeft, viewTop, viewRight, viewBottom);
    for (const auto &[id, conversation] : s.conversations) {
        const auto &row = *conversation.row;
        if (!row.visible() || !row.matched)
            continue;
        float left, top, right, bottom;
        Node::transformedBounds(&row, left, top, right, bottom);
        left = std::max(left, viewLeft);
        top = std::max(top, viewTop);
        right = std::min(right, viewRight);
        bottom = std::min(bottom, viewBottom);
        if (right > left && bottom > top)
            s.hintRectCache.push_back({row.key, left, top, right - left, bottom - top});
    }
    return s.hintRectCache;
}
void Sidebar::remapBuffer(std::string_view oldId, std::string_view newId) {
    auto &s = *impl_;
    ++s.popup.generation;
    if (s.popup.open && popupContext())
        popupContext()->closeSelectDropdown();
    s.popup.open = false;
    if (oldId != newId) {
        s.hintRectCache.clear();
        if (s.lastPinnedBuffer == oldId)
            s.lastPinnedBuffer.assign(newId);
        for (std::size_t i = 0; i < s.hintCount; ++i)
            if (s.hintCandidates[i] == oldId)
                s.hintCandidates[i].assign(newId);
        auto begin = s.hintCandidates.begin();
        auto end = begin + s.hintCount;
        std::sort(begin, end);
        s.hintCount = std::unique(begin, end) - begin;
        auto source = s.conversations.find(std::string(oldId));
        auto target = s.conversations.find(std::string(newId));
        if (source != s.conversations.end()) {
            s.groups.at(source->second.server).orderDirty = true;
            if (target != s.conversations.end())
                s.groups.at(target->second.server).orderDirty = true;
            if (target != s.conversations.end() && target->second.row->focused()) {
                s.groups.at(source->second.server).buffers.erase(source->first);
                s.canvas->removeChild(source->second.row);
                s.conversations.erase(source);
            } else {
                if (target != s.conversations.end()) {
                    s.groups.at(target->second.server).buffers.erase(target->first);
                    s.canvas->removeChild(target->second.row);
                    s.conversations.erase(target);
                }
                s.groups.at(source->second.server).buffers.erase(source->first);
                auto retained = s.conversations.extract(source);
                retained.key() = newId;
                auto &conversation = s.conversations.insert(std::move(retained)).position->second;
                conversation.row->key = newId;
                conversation.row->setTabFocusKey("buffer:" + std::string(newId));
                s.groups.at(conversation.server).buffers.emplace(newId, conversation.row);
            }
        }
    }
    const auto row = s.conversations.find(std::string(newId));
    const auto buffer = s.model.buffers.find(std::string(newId));
    if (row != s.conversations.end() && buffer != s.model.buffers.end())
        row->second.row->title->setText(buffer->second.target);
}

struct People::Impl final : VirtualListAdapter {
    struct Member {
        std::string name, prefix, account, lower, title, tooltip;
        bool away = false;
        double arrived = 0;
        std::uint32_t hash = 2166136261u;
        std::uint64_t key = 0, revision = 1, seen = 0;
        Color color{};
    };
    People &owner;
    Model &model;
    std::function<void(std::string)> execute;
    std::function<void(SidebarMenuInvocation)> menu;
    Label *heading = nullptr, *empty = nullptr;
    Flex *header = nullptr;
    LucideIcon *peopleIcon = nullptr;
    InputArea *navigation = nullptr;
    VirtualListView *list = nullptr;
    std::map<std::string, Member> members;
    std::vector<Member *> order;
    std::string buffer, keyboardNick;
    std::uint64_t epoch = 0, nextKey = 0, visualRevision = 0;
    float zoom = 1, lastRadius = -1;
    std::uint64_t lastThemeRevision = 0;
    std::string lastFont, lastFeatures, lastAvatarStyle;
    bool lastAvatars = false;
    SidebarMode mode = SidebarMode::Hidden;

    Impl(People &node, Model &value, std::function<void(std::string)> command,
         std::function<void(SidebarMenuInvocation)> contextMenu)
        : owner(node), model(value), execute(std::move(command)), menu(std::move(contextMenu)) {
        header = child<Flex>(owner);
        header->setDirection(FlexDirection::Horizontal);
        header->setAlign(FlexAlign::Center);
        peopleIcon = child<LucideIcon>(*header, "users");
        peopleIcon->setHitTestVisible(false);
        heading = child<Label>(*header);
        heading->setFlexGrow(1);
        heading->setMaxLines(1);
        heading->setUseMarkup(false);
        heading->setHitTestVisible(false);
        navigation = child<InputArea>(owner);
        navigation->setFocusable(true);
        navigation->setRetainsFocusOnPointerRelease(true);
        navigation->setTabFocusKey("people-members");
        navigation->setTooltip(
            "Channel members. Arrow keys select; Enter inserts a nick; Menu opens member actions.");
        navigation->setOnFocusGain([this] {
            if (keyboardNick.empty() && !order.empty())
                keyboardNick = order.front()->name;
            refreshKeyboard();
        });
        navigation->setOnFocusLoss([this] { refreshKeyboard(); });
        navigation->setOnKeyDown([this](const InputArea::KeyData &key) { handleKey(key); });
        list = child<VirtualListView>(*navigation);
        list->setAdapter(this);
        list->setOverscanItems(4);
        list->scrollView().setReserveScrollbarGutter(true);
        empty = child<Label>(owner);
        empty->setUseMarkup(false);
        empty->setMaxLines(0);
    }

    ~Impl() override { list->setAdapter(nullptr); }
    std::size_t itemCount() const override { return order.size(); }
    std::uint64_t itemKey(std::size_t index) const override { return order[index]->key; }
    std::uint64_t itemRevision(std::size_t index) const override {
        return order[index]->revision + visualRevision;
    }
    bool itemInteractive(std::size_t) const override { return true; }
    float rowHeight() const {
        return std::max(32.f, (mode == SidebarMode::Compact ? 48.f : 36.f) * zoom);
    }
    float measureItem(Renderer &, std::size_t, float) override { return rowHeight(); }
    std::unique_ptr<Node> createItem() override {
        auto row = std::make_unique<Row>(std::string_view{}, true);
        row->setFocusable(false);
        row->setTabStop(false);
        row->customTitle = true;
        row->enableSprite();
        row->activate = [this](Row &item) {
            keyboardNick = item.key;
            execute("insert-nick:" + item.context + "\n" + item.key);
            refreshKeyboard();
        };
        row->menu = [this](Row &item, std::optional<InputArea::PointerData> pointer) {
            keyboardNick = item.key;
            if (menu)
                menu({"nick:" + item.context + "\n" + item.key, pointer});
            else
                execute("nick:" + item.context + "\n" + item.key);
            refreshKeyboard();
        };
        return row;
    }
    void bindItem(Renderer &, Node &node, std::size_t index, float, bool) override {
        auto &row = static_cast<Row &>(node);
        const auto &member = *order[index];
        if (row.key != member.name || row.context != buffer)
            row.dispatchCancel();
        row.key = member.name;
        row.context = buffer;
        row.title->setText(member.title);
        row.titleColor = member.color;
        const bool compact = mode == SidebarMode::Compact;
        const bool avatars = model.settings.avatars && model.settings.avatarStyle != "none";
        const bool avatarOnly = compact && avatars;
        if (row.avatarOnly != avatarOnly || row.avatarLaneVisible != avatars) {
            row.avatarOnly = avatarOnly;
            row.avatarLaneVisible = avatars;
            row.markLayoutDirty();
        }
        row.spriteAvatar = model.settings.avatarStyle == "sprite";
        row.avatarHash = member.hash;
        row.syncAvatar({}, member.name);
        const auto privilege = std::string_view(member.prefix).substr(0, 1);
        row.badge->setText(compact ? privilege : std::string_view{});
        row.tooltip(member.tooltip);
        row.style(zoom, navigation->focused() && member.name == keyboardNick);
    }

    void refreshKeyboard() {
        ++visualRevision;
        list->markLayoutDirty();
    }
    void handleKey(const InputArea::KeyData &key) {
        if (!key.pressed || order.empty())
            return;
        auto current = std::find_if(order.begin(), order.end(), [&](const Member *member) {
            return member->name == keyboardNick;
        });
        std::size_t index = current == order.end() ? 0 : std::size_t(current - order.begin());
        if (KeySymbol::isEnterOrSpace(key.sym)) {
            execute("insert-nick:" + buffer + "\n" + order[index]->name);
            return;
        }
        if (menuKey(key)) {
            if (menu)
                menu({"nick:" + buffer + "\n" + order[index]->name, std::nullopt});
            else
                execute("nick:" + buffer + "\n" + order[index]->name);
            return;
        }
        if (KeySymbol::isUp(key.sym))
            index = index ? index - 1 : 0;
        else if (KeySymbol::isDown(key.sym))
            index = std::min(index + 1, order.size() - 1);
        else if (KeySymbol::isHome(key.sym))
            index = 0;
        else if (KeySymbol::isEnd(key.sym))
            index = order.size() - 1;
        else if (KeySymbol::isPageUp(key.sym) || KeySymbol::isPageDown(key.sym)) {
            const auto page = std::max(std::size_t(1), std::size_t(list->height() / rowHeight()));
            index = KeySymbol::isPageUp(key.sym) ? index - std::min(index, page)
                                                 : std::min(index + page, order.size() - 1);
        } else
            return;
        keyboardNick = order[index]->name;
        list->scrollToIndex(index);
        refreshKeyboard();
    }

    void sync(std::string_view focused, float width, float height, float scale,
              SidebarMode effectiveMode) {
        const bool presentationChanged = mode != effectiveMode;
        mode = effectiveMode;
        owner.setVisible(mode != SidebarMode::Hidden);
        owner.setExcludeSubtreeFromTabOrder(mode == SidebarMode::Hidden);
        if (mode == SidebarMode::Hidden)
            return;
        ++epoch;
        bool reordered = false, changed = false;
        if (buffer != focused) {
            buffer.assign(focused);
            members.clear();
            order.clear();
            keyboardNick.clear();
            reordered = true;
            list->scrollView().requestScrollToOffset(0);
        }
        const auto found = model.buffers.find(buffer);
        const bool channel = found != model.buffers.end() && found->second.kind == "channel";
        if (channel)
            for (const auto &nick : found->second.nicks) {
                auto [it, inserted] = members.try_emplace(nick.name);
                auto &member = it->second;
                member.seen = epoch;
                if (inserted) {
                    member.name = nick.name;
                    member.lower = folded(nick.name);
                    member.key = ++nextKey;
                    for (unsigned char c : member.lower) {
                        member.hash ^= c;
                        member.hash *= 16777619;
                    }
                }
                if (inserted || member.prefix != nick.prefix || member.account != nick.account ||
                    member.away != nick.away || member.arrived != nick.arrived) {
                    reordered |= inserted || member.prefix != nick.prefix;
                    member.prefix = nick.prefix;
                    member.account = nick.account;
                    member.away = nick.away;
                    member.arrived = nick.arrived;
                    member.title = member.prefix + member.name;
                    member.tooltip = member.name;
                    if (!member.account.empty())
                        member.tooltip.append("\nAccount: ").append(member.account);
                    if (!member.prefix.empty())
                        member.tooltip.append("\nPrefix: ").append(member.prefix);
                    if (member.away)
                        member.tooltip.append("\nAway");
                    ++member.revision;
                    changed = true;
                }
                auto color = memberColor(model, member.name, member.hash);
                float fade = member.away ? .4f : 1.f;
                if (!model.settings.reducedMotion && model.settings.fx != FxLevel::Off &&
                    member.arrived > 0)
                    fade *= std::clamp(float((model.now - member.arrived) / .18), 0.f, 1.f);
                color.a *= fade;
                if (member.color != color) {
                    member.color = color;
                    ++member.revision;
                    changed = true;
                }
            }
        for (auto it = members.begin(); it != members.end();) {
            if (it->second.seen != epoch) {
                it = members.erase(it);
                reordered = true;
            } else
                ++it;
        }
        if (reordered) {
            order.clear();
            order.reserve(members.size());
            for (auto &[name, member] : members)
                order.push_back(&member);
            std::sort(order.begin(), order.end(), [](const Member *a, const Member *b) {
                const int left = prefixRank(a->prefix), right = prefixRank(b->prefix);
                return left != right          ? left < right
                       : a->lower != b->lower ? a->lower < b->lower
                                              : a->name < b->name;
            });
            if (!members.contains(keyboardNick))
                keyboardNick = order.empty() ? std::string{} : order.front()->name;
            list->notifyDataChanged();
        }
        const auto &font = themeUiFont();
        const auto revision = themeRevision();
        const float radius = themeCornerRadius();
        if (presentationChanged || zoom != scale || lastRadius != radius ||
            lastThemeRevision != revision || lastFont != font.family ||
            lastFeatures != font.features || lastAvatars != model.settings.avatars ||
            lastAvatarStyle != model.settings.avatarStyle) {
            zoom = scale;
            lastRadius = radius;
            lastThemeRevision = revision;
            lastFont = font.family;
            lastFeatures = font.features;
            lastAvatars = model.settings.avatars;
            lastAvatarStyle = model.settings.avatarStyle;
            ++visualRevision;
            changed = true;
        }
        if (changed)
            list->markLayoutDirty();
        const bool expanded = mode == SidebarMode::Expanded;
        const float padding = (expanded ? 8.f : 4.f) * scale;
        const float verticalPadding = padding;
        header->setVisible(expanded);
        header->setExcludeSubtreeFromTabOrder(!expanded);
        list->scrollView().setScrollbarVisible(expanded);
        list->scrollView().setReserveScrollbarGutter(expanded);
        const float extent = std::max(32.f, 32 * scale);
        header->setPosition(padding, verticalPadding);
        header->setSize(std::max(0.f, width - 2 * padding), extent);
        header->setGap(6 * scale);
        peopleIcon->setSize(18 * scale, 18 * scale);
        peopleIcon->setColor(themeForegroundColor("on_surface_variant"));
        heading->setText(std::to_string(order.size()) + " members");
        labelFont(*heading, 12 * scale);
        heading->setColor(themeForegroundColor("on_surface_variant"));
        heading->setMaxWidth(std::max(0.f, width - 2 * padding - 24 * scale));
        const float top = verticalPadding + (expanded ? extent + 4 * scale : 0);
        navigation->setPosition(padding, top);
        navigation->setSize(std::max(0.f, width - 2 * padding),
                            std::max(0.f, height - top - verticalPadding));
        list->setSize(navigation->width(), navigation->height());
        list->setContentScale(scale);
        empty->setVisible(expanded && order.empty());
        empty->setText(channel ? "No members are loaded for this channel."
                               : "Select a channel to see its members.");
        labelFont(*empty, 13 * scale);
        empty->setColor(themeForegroundColor("on_surface_variant"));
        empty->setPosition(padding, top);
        empty->setMaxWidth(std::max(0.f, width - 2 * padding));
    }
};

People::People(Model &model, std::function<void(std::string)> execute,
               std::function<void(SidebarMenuInvocation)> menu)
    : impl_(std::make_unique<Impl>(*this, model, std::move(execute), std::move(menu))) {
    setClipChildren(true);
}
People::~People() = default;
void People::sync(std::string_view focusedBuffer, float width, float height, float scale,
                  SidebarMode effectiveMode) {
    impl_->sync(focusedBuffer, width, height, scale, effectiveMode);
}
void People::remapBuffer(std::string_view oldId, std::string_view newId) {
    if (oldId != newId && impl_->buffer == oldId) {
        impl_->buffer = newId;
        impl_->refreshKeyboard();
    }
}
} // namespace blast
