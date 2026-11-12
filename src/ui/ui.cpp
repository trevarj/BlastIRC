#include "ui.hpp"
#include "../application.hpp"
#include "../irc/protocol.hpp"
#include "../model.hpp"
#include "../platform/clipboard.hpp"
#include "../platform/file_picker.hpp"
#include "../preview.hpp"
#include "../render/graphics.hpp"
#include "../theme/theme.hpp"
#include "../upload.hpp"
#include "buffer_chrome.hpp"
#include "dialogs.hpp"
#include "editor.hpp"
#include "emoji_picker.hpp"
#include "logic.hpp"
#include "media_preview.hpp"
#include "mode_line.hpp"
#include "presentation.hpp"
#include "sidebar.hpp"
#include "transcript.hpp"
#include "workspace.hpp"
#include <algorithm>
#include <array>
#include <bitset>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <core/input/key_chord.h>
#include <core/input/key_modifiers.h>
#include <core/input/key_symbols.h>
#include <core/input/keybind_matcher.h>
#include <core/text_clipboard.h>
#include <core/ui_phase.h>
#include <cstdlib>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <iterator>
#include <optional>
#include <render/codecs/codecs.h>
#include <render/core/image_decoder.h>
#include <render/core/image_resize.h>
#include <render/core/texture_manager.h>
#include <render/scene/input_dispatcher.h>
#include <set>
#include <span>
#include <stdexcept>
#include <sys/stat.h>
#include <ui/builders.h>
#include <ui/controls/button.h>
#include <ui/controls/context_menu.h>
#include <ui/controls/image.h>
#include <ui/controls/input.h>
#include <ui/controls/select_popup_context.h>
#include <unistd.h>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

namespace blast {
void configureNativeControlBindings() {
    // Native controls have no default matchers; install the SDK host's standard bindings.
    KeybindMatcher::setMatcher(KeybindAction::Validate,
                               [](std::uint32_t sym, std::uint32_t modifiers) {
                                   return modifiers == 0 && KeySymbol::isEnterOrSpace(sym);
                               });
    constexpr std::pair<KeybindAction, KeyChord> nativeBindings[] = {
        {KeybindAction::Cancel, {XKB_KEY_Escape, 0}},
        {KeybindAction::Left, {XKB_KEY_Left, 0}},
        {KeybindAction::Right, {XKB_KEY_Right, 0}},
        {KeybindAction::Up, {XKB_KEY_Up, 0}},
        {KeybindAction::Down, {XKB_KEY_Down, 0}},
        {KeybindAction::TabNext, {XKB_KEY_Tab, 0}},
        {KeybindAction::TabPrevious, {XKB_KEY_ISO_Left_Tab, KeyMod::Shift}},
        {KeybindAction::Delete, {XKB_KEY_Delete, 0}},
        {KeybindAction::Copy, {XKB_KEY_c, KeyMod::Ctrl}},
        {KeybindAction::Save, {XKB_KEY_s, KeyMod::Ctrl}},
    };
    for (const auto &[action, chord] : nativeBindings) {
        KeybindMatcher::setMatcher(action, [chord](std::uint32_t sym, std::uint32_t modifiers) {
            return keyChordMatches(chord, sym, modifiers);
        });
    }
    Input::setValidateKeyMatcher([](std::uint32_t sym, std::uint32_t modifiers) {
        return KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers);
    });
}

const std::map<std::string, std::string> &defaultBindings() {
    static const std::map<std::string, std::string> bindings = {
        {"switch-buffer", "C-x b"},
        {"execute-command", "M-x"},
        {"jump-channel", "C-x j"},
        {"conversation-back", "C-o"},
        {"conversation-forward", "C-i"},
        {"global-mentions", ""},
        {"toggle-fools", "C-x f"},
        {"show-fools", ""},
        {"hide-fools", ""},
        {"message-prev", "C-p"},
        {"message-next", "C-n"},
        {"scroll-up", "PRIOR"},
        {"scroll-down", "NEXT"},
        {"message-reply", "C-r"},
        {"message-react", "C-e"},
        {"cancel", "C-g"},
        {"search", "C-s"},
        {"settings", "C-,"},
        {"toggle-people", ""},
        {"toggle-navigation", ""},
        {"send", "RET"},
        {"newline", "S-RET"},
        {"beginning-of-line", "C-a"},
        {"end-of-line", ""},
        {"backward-char", "C-b"},
        {"forward-char", "C-f"},
        {"previous-line", ""},
        {"next-line", ""},
        {"backward-word", "M-b"},
        {"forward-word", "M-f"},
        {"beginning-of-buffer", "M-<"},
        {"end-of-buffer", "M->"},
        {"kill-word", "M-d"},
        {"backward-kill-word", "M-BACKSPACE"},
        {"delete-char", "C-d"},
        {"backward-delete", "BACKSPACE"},
        {"kill-line", "C-k"},
        {"kill-region", "C-w"},
        {"copy-region", "M-w"},
        {"yank", "C-y"},
        {"cycle-spelling", "C-;"},
        {"undo", "C-/"},
        {"redo", ""},
        {"copy", ""},
        {"cut", ""},
        {"paste", ""},
        {"select-all", ""},
        {"transpose-chars", "C-t"},
        {"backward-kill-line", ""},
        {"set-mark", "C-SPC"},
        {"history-previous", "M-p"},
        {"history-next", "M-n"},
        {"complete", "TAB"}};
    return bindings;
}
std::string_view commandDisplayName(std::string_view command) {
    static constexpr std::pair<std::string_view, std::string_view> names[] = {
        {"switch-buffer", "Switch conversation"},
        {"execute-command", "Execute command"},
        {"jump-channel", "Jump to channel or conversation"},
        {"conversation-back", "Previous viewed conversation"},
        {"conversation-forward", "Next viewed conversation"},
        {"global-mentions", "Global mentions"},
        {"toggle-fools", "Toggle fools in this conversation"},
        {"show-fools", "Show fools"},
        {"hide-fools", "Hide fools"},
        {"message-prev", "Select previous message"},
        {"message-next", "Select next message"},
        {"scroll-up", "Scroll chat up half a page"},
        {"scroll-down", "Scroll chat down half a page"},
        {"message-reply", "Reply to selected message"},
        {"message-react", "React to selected message"},
        {"cancel", "Cancel"},
        {"search", "Search conversation"},
        {"settings", "Settings"},
        {"toggle-people", "Toggle People"},
        {"toggle-navigation", "Toggle navigation"},
        {"mute-buffer", "Mute conversation"},
        {"unmute-buffer", "Unmute conversation"},
        {"archive-buffer", "Archive conversation"},
        {"unarchive-buffer", "Unarchive conversation"},
        {"pin-buffer", "Pin chat"},
        {"unpin-buffer", "Unpin chat"},
        {"send", "Send message"},
        {"newline", "Insert newline"},
        {"beginning-of-line", "Beginning of line"},
        {"end-of-line", "End of line"},
        {"backward-char", "Previous character"},
        {"forward-char", "Next character"},
        {"previous-line", "Previous line"},
        {"next-line", "Next line"},
        {"backward-word", "Previous word"},
        {"forward-word", "Next word"},
        {"beginning-of-buffer", "Beginning of messages"},
        {"end-of-buffer", "End of messages"},
        {"kill-word", "Cut next word"},
        {"backward-kill-word", "Cut previous word"},
        {"delete-char", "Delete next character"},
        {"backward-delete", "Delete previous character"},
        {"kill-line", "Cut to end of line"},
        {"kill-region", "Cut selection"},
        {"copy-region", "Copy selection"},
        {"yank", "Paste cut text"},
        {"cycle-spelling", "Cycle spelling suggestions"},
        {"undo", "Undo edit"},
        {"redo", "Redo"},
        {"copy", "Copy selection"},
        {"cut", "Cut selection"},
        {"paste", "Paste clipboard"},
        {"select-all", "Select all"},
        {"transpose-chars", "Transpose characters"},
        {"backward-kill-line", "Cut to beginning of line"},
        {"set-mark", "Set selection mark"},
        {"history-previous", "Previous sent message"},
        {"history-next", "Next sent message"},
        {"complete", "Complete nickname or command"},
        {"connect", "Connect or reconnect network"},
        {"disconnect", "Disconnect network"},
        {"add-server", "Add network"},
        {"manage-networks", "Manage networks"},
        {"load-history", "Load older messages"},
        {"clear-history", "Clear local history"},
        {"mark-read", "Mark read"},
        {"mark-all-read", "Mark all buffers read"},
        {"close-buffer", "Close buffer"},
        {"topic", "Conversation details / topic"},
        {"quit", "Quit BlastIRC"},
        {"message-style:irc", "Use traditional IRC rows"},
        {"message-style:bubbles", "Use message bubbles"},
        {"message-style:stacked", "Use stacked IRC rows"}};
    const auto found =
        std::find_if(std::begin(names), std::end(names),
                     [command](const auto &entry) { return entry.first == command; });
    return found == std::end(names) ? command : found->second;
}
std::string commandLabel(const Model &model, std::string_view label, std::string_view command) {
    std::string result(label);
    if (const auto binding = model.settings.bindings.find(std::string(command));
        binding != model.settings.bindings.end() && !binding->second.empty())
        result.append(" · ").append(binding->second);
    return result;
}
namespace {
constexpr float panePaddingX = 12.f;
constexpr float navigationExpandedExtent = 250.f, peopleExpandedExtent = 220.f;
constexpr float compactPanelExtent = 56.f;
constexpr SidebarMode nextSidebarChoice(SidebarMode preferred, SidebarMode regularEffective) {
    const auto current = regularEffective == SidebarMode::Hidden && preferred != SidebarMode::Hidden
                             ? preferred
                             : regularEffective;
    return nextSidebarMode(current);
}
float shellOuterPadding(float width, float height, float scale) {
    const bool compact = height < 600 * scale || width < 1000 * scale;
    return (height < 360 * scale ? 4.f : compact ? 8.f : 12.f) * scale;
}
float panelWidth(SidebarMode mode, float expanded, float compact) {
    return mode == SidebarMode::Expanded ? expanded : mode == SidebarMode::Compact ? compact : 0.f;
}
float fittedRailWidth(const Workspace &workspace, float width, float outer, float boundary,
                      float scale) {
    float preferred = workspace.serverRailWidth().value_or(76.f) * scale;
    if (workspace.serverRailWidth()) {
        const float minimumChat = std::max(360 * scale, EditorStore::minimumComposerWidth(scale) +
                                                            2 * panePaddingX * scale);
        preferred = std::min(
            preferred,
            std::max(Workspace::minServerRailWidth * scale,
                     width - 2 * outer - 2 * boundary -
                         workspace.compactChannelSidebarWidth().value_or(compactPanelExtent) *
                             scale -
                         minimumChat));
    }
    return std::min(preferred, std::max(0.f, width - 2 * outer));
}
std::pair<SidebarMode, SidebarMode> fitSidebarModes(SidebarMode navigation, SidebarMode people,
                                                    float remaining, float boundary, float scale,
                                                    float minimumChat, float navigationExtent,
                                                    float navigationCompactExtent) {
    const float compact = compactPanelExtent * scale;
    const auto chatFor = [&](SidebarMode left, SidebarMode right) {
        return remaining -
               (left == SidebarMode::Hidden
                    ? 0.f
                    : panelWidth(left, navigationExtent, navigationCompactExtent) + boundary) -
               (right == SidebarMode::Hidden
                    ? 0.f
                    : panelWidth(right, peopleExpandedExtent * scale, compact) + boundary);
    };
    if (people == SidebarMode::Expanded && chatFor(navigation, people) < minimumChat)
        people = SidebarMode::Compact;
    if (navigation == SidebarMode::Expanded && chatFor(navigation, people) < minimumChat)
        navigation = SidebarMode::Compact;
    if (people == SidebarMode::Compact && chatFor(navigation, people) < minimumChat)
        people = SidebarMode::Hidden;
    return {navigation, people};
}
bool withinNode(const Node *node, const Node *root) {
    for (; node; node = node->parent())
        if (node == root)
            return true;
    return false;
}
bool visibleWithinNode(const Node *node, const Node *root) {
    for (; node; node = node->parent()) {
        if (!node->visible())
            return false;
        if (node == root)
            return true;
    }
    return false;
}
Input *nativeInputFor(InputArea *area) {
    if (!area)
        return nullptr;
    if (auto *input = dynamic_cast<Input *>(area->textInputClient()))
        return input;
    for (Node *node = area; node; node = node->parent())
        if (auto *input = dynamic_cast<Input *>(node))
            return input;
    return nullptr;
}
bool pressedWithin(const Node &node) {
    if (const auto *area = dynamic_cast<const InputArea *>(&node); area && area->pressed())
        return true;
    for (const auto &child : node.children())
        if (pressedWithin(*child))
            return true;
    return false;
}
void place(Node &node, float x, float y, float width, float height) {
    node.setPosition(x, y);
    node.setSize(std::max(0.f, width), std::max(0.f, height));
}
void place(Node &node, const WorkspaceRect &rect) {
    place(node, rect.x, rect.y, rect.width, rect.height);
}
bool inside(const WorkspaceRect &r, float x, float y) {
    return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}
void queue(Model &model, std::string type, std::string buffer = {}, std::string text = {},
           std::string extra = {}) {
    model.actions.push_back(
        {std::move(type), std::move(buffer), std::move(text), std::move(extra)});
}
ContextMenuEntry menuLeaf(std::vector<std::string> &commands, std::string label,
                          std::string_view icon, std::string command) {
    ContextMenuEntry entry{};
    entry.value = commands.size();
    entry.label = std::move(label);
    entry.icon = icon;
    commands.push_back(std::move(command));
    return entry;
}
ContextMenuEntry menuRadio(std::vector<std::string> &commands, std::string label,
                           std::string_view icon, std::string command, bool selected) {
    auto entry = menuLeaf(commands, std::move(label), icon, std::move(command));
    entry.radio = true;
    entry.toggleState = selected ? 1 : 0;
    return entry;
}
std::string_view paletteCommandIcon(std::string_view command) {
    if (command.starts_with("message-style:"))
        return "palette";
    if (command == "switch-buffer")
        return "messages";
    if (command == "jump-channel")
        return "scan";
    if (command == "toggle-navigation")
        return "compass";
    if (command == "toggle-people" || command.starts_with("presence:"))
        return "users";
    if (command == "search")
        return "search";
    if (command == "settings")
        return "settings";
    if (command == "mute-buffer")
        return "bell-off";
    if (command == "unmute-buffer")
        return "bell";
    if (command == "archive-buffer")
        return "archive";
    if (command == "unarchive-buffer")
        return "archive-off";
    if (command == "pin-buffer")
        return "pin";
    if (command == "unpin-buffer")
        return "unpin";
    if (command == "connect" || command == "disconnect" || command == "add-server" ||
        command == "manage-networks")
        return "server";
    if (command == "load-history" || command == "clear-history")
        return "history";
    if (command == "mark-read")
        return "checks";
    if (command == "close-buffer" || command == "quit")
        return "trash";
    if (command == "topic")
        return "info-circle";
    if (command == "message-prev" || command == "message-next")
        return "messages";
    if (command == "message-reply")
        return "message";
    if (command == "message-react")
        return "mood-smile";
    if (command == "send" || command == "reply")
        return "message";
    if (command == "copy")
        return "copy";
    if (command == "paste")
        return "clipboard";
    if (command == "complete")
        return "sparkles";
    if (command == "cancel")
        return "x";
    if (command == "undo")
        return "arrow-back-up";
    if (command == "redo")
        return "arrow-forward-up";
    return "terminal";
}
} // namespace
std::string sidebarModeToggleLabel(std::string_view panel, SidebarMode preferred,
                                   SidebarMode regularEffective) {
    std::string label(panel);
    label.append(": ").append(sidebarModeToken(regularEffective)).append("; next ");
    label.append(sidebarModeToken(nextSidebarChoice(preferred, regularEffective)));
    if (preferred != regularEffective) {
        label.append(" (preferred ").append(sidebarModeToken(preferred));
        if (regularEffective == SidebarMode::Hidden)
            label.append("; limited space");
        label.push_back(')');
    }
    return label;
}
std::string bindingStroke(const InputArea::KeyData &key) {
    std::string name;
    switch (key.sym) {
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        name = "ret";
        break;
    case XKB_KEY_Tab:
    case XKB_KEY_ISO_Left_Tab:
        name = "tab";
        break;
    case XKB_KEY_space:
        name = "spc";
        break;
    case XKB_KEY_BackSpace:
        name = "backspace";
        break;
    case XKB_KEY_Escape:
        name = "escape";
        break;
    case XKB_KEY_Left:
        name = "left";
        break;
    case XKB_KEY_Right:
        name = "right";
        break;
    case XKB_KEY_Up:
        name = "up";
        break;
    case XKB_KEY_Down:
        name = "down";
        break;
    case XKB_KEY_Page_Up:
    case XKB_KEY_KP_Page_Up:
        name = "prior";
        break;
    case XKB_KEY_Page_Down:
    case XKB_KEY_KP_Page_Down:
        name = "next";
        break;
    case XKB_KEY_Home:
        name = "home";
        break;
    case XKB_KEY_End:
        name = "end";
        break;
    case XKB_KEY_Delete:
        name = "delete";
        break;
    case XKB_KEY_Shift_L:
    case XKB_KEY_Shift_R:
    case XKB_KEY_Control_L:
    case XKB_KEY_Control_R:
    case XKB_KEY_Alt_L:
    case XKB_KEY_Alt_R:
    case XKB_KEY_Super_L:
    case XKB_KEY_Super_R:
        return {};
    default: {
        char text[64]{};
        const auto sym = xkb_keysym_to_lower(key.sym);
        if (xkb_keysym_to_utf8(sym, text, sizeof(text)) > 1)
            name = text;
        else if (xkb_keysym_get_name(sym, text, sizeof(text)) > 0)
            name = text;
        break;
    }
    }
    if (name.empty())
        return {};
    bool shift = key.modifiers & KeyMod::Shift;
    // These are the actual shifted symbols in the Emacs keymap, not S-, / S-.
    if (key.sym == XKB_KEY_less || key.sym == XKB_KEY_greater) {
        name = key.sym == XKB_KEY_less ? "<" : ">";
        shift = false;
    }
    std::string result;
    if (key.modifiers & KeyMod::Ctrl)
        result += "c-";
    if (key.modifiers & KeyMod::Alt)
        result += "m-";
    if (key.modifiers & KeyMod::Super)
        result += "s-";
    if (shift)
        result += "s-";
    result += name;
    return ui_logic::canonical(result);
}
namespace {
bool workspaceCommand(std::string_view name) {
    return name == "switch-buffer" || name == "execute-command" || name == "jump-channel" ||
           name == "conversation-back" || name == "conversation-forward" || name == "settings" ||
           name == "search" || name == "cancel" || name == "toggle-people" ||
           name == "toggle-navigation" || name == "quit" || name == "connect" ||
           name == "disconnect" || name == "load-history" || name == "clear-history" ||
           name == "manage-networks" || name == "mark-all-read" ||
           name.starts_with("message-style:") || name == "message-prev" || name == "message-next" ||
           name == "message-reply" || name == "message-react" || name == "mute-buffer" ||
           name == "unmute-buffer" || name == "archive-buffer" || name == "unarchive-buffer" ||
           name == "pin-buffer" || name == "unpin-buffer" || name == "global-mentions" ||
           name == "toggle-fools" || name == "show-fools" || name == "hide-fools";
}
// Local review only: never pass untrusted files to the SVG-capable file loader.
class UploadPreview final : public Node {
  public:
    void reset(std::string path = {}, std::optional<UploadFileIdentity> identity = {}) {
        if (image_)
            removeChild(image_);
        image_ = nullptr;
        path_ = std::move(path);
        identity_ = identity;
        pending_ = !path_.empty();
        setVisible(pending_);
        markLayoutDirty();
    }

  protected:
    void doLayout(Renderer &renderer) override {
        if (pending_) {
            pending_ = false;
            load(renderer);
            setVisible(image_ != nullptr);
        }
        if (image_)
            place(*image_, 0, 0, width(), height());
        Node::doLayout(renderer);
    }

  private:
    void load(Renderer &renderer) {
        if (!identity_ || !identity_->size || identity_->size > 32ULL * 1024 * 1024)
            return;
        struct File {
            int fd;
            ~File() {
                if (fd >= 0)
                    ::close(fd);
            }
        } file{::open(path_.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW)};
        const auto current = [&]() {
            struct stat s{};
            if (file.fd < 0 || ::fstat(file.fd, &s) != 0 || !S_ISREG(s.st_mode) || s.st_size < 0)
                return false;
            const UploadFileIdentity actual{static_cast<std::uint64_t>(s.st_dev),
                                            static_cast<std::uint64_t>(s.st_ino),
                                            static_cast<std::uint64_t>(s.st_size),
                                            s.st_mtim.tv_sec,
                                            s.st_mtim.tv_nsec,
                                            s.st_ctim.tv_sec,
                                            s.st_ctim.tv_nsec};
            return actual == *identity_ && inspectUploadFile(path_) == identity_;
        };
        if (!current())
            return;
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(identity_->size));
        std::size_t done = 0;
        while (done < bytes.size()) {
            const auto count = ::read(file.fd, bytes.data() + done, bytes.size() - done);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                return;
            done += static_cast<std::size_t>(count);
        }
        auto raster = PreviewService::raster(bytes);
        std::vector<std::uint8_t> pixels;
        int w = 0, h = 0;
        const bool gif =
            bytes.size() >= 10 && (std::equal(bytes.begin(), bytes.begin() + 6, "GIF87a") ||
                                   std::equal(bytes.begin(), bytes.begin() + 6, "GIF89a"));
        if (gif) {
            w = bytes[6] | (bytes[7] << 8);
            h = bytes[8] | (bytes[9] << 8);
        } else if (raster) {
            w = raster->width;
            h = raster->height;
        }
        if (w <= 0 || h <= 0 || w > 8192 || h > 8192 ||
            static_cast<std::uint64_t>(w) * h > 16ULL * 1024 * 1024)
            return;
        if (gif) {
            auto decoded =
                codecs::decodeAnimatedGif(bytes.data(), bytes.size(), 1, 64ULL * 1024 * 1024);
            if (!decoded || decoded->width != w || decoded->height != h || decoded->frames.empty())
                return;
            pixels = std::move(decoded->frames.front().rgba);
        } else {
            auto decoded = decodeRasterImage(bytes.data(), bytes.size());
            if (!decoded ||
                !((decoded->width == w && decoded->height == h) ||
                  (raster->extension == ".jpg" && decoded->width == h && decoded->height == w)))
                return;
            w = decoded->width;
            h = decoded->height;
            pixels = std::move(decoded->pixels);
        }
        if (!current())
            return;
        // ponytail: one static first frame, bounded to 480px; no animation or large-file copy.
        if (std::max(w, h) > 480) {
            const double factor = 480.0 / std::max(w, h);
            const int resizedW = std::max(1, static_cast<int>(w * factor));
            const int resizedH = std::max(1, static_cast<int>(h * factor));
            pixels =
                resizeToRgba(pixels.data(), w, h, ResizeSourceLayout::Rgba, resizedW, resizedH);
            w = resizedW;
            h = resizedH;
        }
        if (pixels.empty() || !current())
            return;
        auto image = std::make_unique<Image>();
        image->setFit(ImageFit::Contain);
        image->setHitTestVisible(false);
        if (image->setSourceRaw(renderer, pixels.data(), pixels.size(), w, h, 0,
                                PixmapFormat::RGBA))
            image_ = static_cast<Image *>(addChild(std::move(image)));
    }
    Image *image_ = nullptr;
    std::string path_;
    std::optional<UploadFileIdentity> identity_;
    bool pending_ = false;
};
class AppLogo final : public Node {
  public:
    AppLogo() {
        image_ = static_cast<Image *>(addChild(std::make_unique<Image>()));
        image_->setFit(ImageFit::Contain);
        image_->setHitTestVisible(false);
        setHitTestVisible(false);
    }

  protected:
    void doLayout(Renderer &renderer) override {
        if (path_.empty()) {
            std::array<std::filesystem::path, 3> roots;
#ifdef BLASTIRC_ASSET_DIR
            roots[0] = BLASTIRC_ASSET_DIR;
#endif
            roots[1] = "assets";
            std::error_code error;
            const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
            if (!error)
                roots[2] = executable.parent_path() / "../share/blastirc/assets";
            for (const auto &root : roots) {
                if (root.empty())
                    continue;
                const auto path = root / "icon.png";
                if (std::filesystem::is_regular_file(path, error)) {
                    path_ = path.string();
                    break;
                }
            }
            if (path_.empty())
                throw std::runtime_error("missing BlastIRC app icon");
        }
        // Decode once; rail resizing only scales the cached GPU image.
        if (!image_->hasImage() && !image_->setSourceFile(renderer, path_))
            throw std::runtime_error("cannot decode BlastIRC app icon: " + path_);
        place(*image_, 0, 0, width(), height());
        Node::doLayout(renderer);
    }

  private:
    Image *image_ = nullptr;
    std::string path_;
};

class IconButton final : public InputArea {
  public:
    IconButton(std::string_view icon, std::string tooltip, std::function<void()> click)
        : click_(std::move(click)) {
        setFocusable(true);
        hint(tooltip);
        background_ = static_cast<Box *>(addChild(std::make_unique<Box>()));
        background_->setHitTestVisible(false);
        icon_ = static_cast<LucideIcon *>(addChild(std::make_unique<LucideIcon>(icon, 18)));
        icon_->setHitTestVisible(false);
        setOnClick([this](const PointerData &) { click_(); });
        setOnKeyDown([this](const KeyData &key) {
            if (key.sym == XKB_KEY_Return || key.sym == XKB_KEY_KP_Enter ||
                key.sym == XKB_KEY_space)
                click_();
        });
    }
    void selected(bool value) { selected_ = value; }
    void hint(std::string_view value) {
        if (tooltip_ != value) {
            tooltip_ = value;
            setTooltip(tooltip_);
        }
    }
    void sync(float scale) {
        scale_ = scale;
        const bool primary = selected_ || pressed();
        const bool hover = hovered() && !primary;
        background_->setFill(primary ? themeSurfaceColor("primary_container")
                             : hover ? themeSurfaceColor("hover")
                                     : Color{0, 0, 0, 0});
        if (focused())
            background_->setBorder(themeForegroundColor("secondary"), std::max(1.f, scale));
        else
            background_->clearBorder();
        background_->setRadius(themeCornerRadius());
        icon_->setColor(themeForegroundColor(primary ? "on_primary_container"
                                             : hover ? "on_hover"
                                                     : "on_surface"));
    }

  protected:
    void doLayout(Renderer &renderer) override {
        place(*background_, 0, 0, width(), height());
        const float extent = std::min({18 * scale_, width(), height()});
        place(*icon_, (width() - extent) / 2, (height() - extent) / 2, extent, extent);
        InputArea::doLayout(renderer);
    }

  private:
    Box *background_;
    LucideIcon *icon_;
    std::function<void()> click_;
    std::string tooltip_;
    float scale_ = 1;
    bool selected_ = false;
};
class JumpOverlay final : public InputArea {
  public:
    explicit JumpOverlay(Sidebar &sidebar) : sidebar_(sidebar) {
        setFocusable(true);
        setTabFocusKey("channel-jump");
        setAcceptedButtons(buttonMask({BTN_LEFT, BTN_RIGHT, BTN_MIDDLE}));
        setTooltip("Type hint keys; Page Up/Down or wheel scrolls; Escape cancels");
        setClipChildren(true);
        scrim_ = static_cast<Box *>(addChild(std::make_unique<Box>()));
        scrim_->setHitTestVisible(false);
    }
    void begin(std::vector<ui_logic::JumpHint> hints) {
        hints_ = std::move(hints);
        badges_.reserve(hints_.size());
        while (badges_.size() < hints_.size()) {
            auto box = std::make_unique<Box>();
            box->setHitTestVisible(false);
            auto label = std::make_unique<Label>();
            label->setUseMarkup(false);
            label->setMaxLines(1);
            label->setHitTestVisible(false);
            auto *text = static_cast<Label *>(box->addChild(std::move(label)));
            auto *background = static_cast<Box *>(addChild(std::move(box)));
            badges_.push_back({background, text});
        }
        for (std::size_t i = hints_.size(); i < badges_.size(); ++i)
            badges_[i].box->setVisible(false);
        prefix_.clear();
        markLayoutDirty();
    }
    std::span<const ui_logic::JumpHint> hints() const { return hints_; }
    void prefix(std::string_view value) {
        prefix_.assign(value);
        markLayoutDirty();
    }
    void remap(std::string_view oldId, std::string_view newId) {
        if (oldId == newId)
            return;
        auto old = std::find_if(hints_.begin(), hints_.end(),
                                [&](const auto &hint) { return hint.buffer == oldId; });
        if (old == hints_.end())
            return;
        const auto existing = std::find_if(hints_.begin(), hints_.end(),
                                           [&](const auto &hint) { return hint.buffer == newId; });
        const auto oldIndex = static_cast<std::size_t>(old - hints_.begin());
        if (existing != hints_.end()) {
            if (old->keys.starts_with(prefix_) && !existing->keys.starts_with(prefix_)) {
                old->buffer.assign(newId);
                erase(static_cast<std::size_t>(existing - hints_.begin()));
            } else
                erase(oldIndex);
        } else
            old->buffer.assign(newId);
        markLayoutDirty();
    }
    bool prune(const Model &model, std::string_view server) {
        bool changed = false;
        for (std::size_t i = 0; i < hints_.size();) {
            const auto found = model.buffers.find(hints_[i].buffer);
            if (found == model.buffers.end() || found->second.server != server ||
                (found->second.kind != "channel" && found->second.kind != "query")) {
                erase(i);
                changed = true;
            } else
                ++i;
        }
        if (changed)
            markLayoutDirty();
        return changed;
    }

  protected:
    void doLayout(Renderer &renderer) override {
        const float scale = themeScale();
        place(*scrim_, 0, 0, width(), height());
        scrim_->setFill(withAlpha(themeColor("scrim"), .68f));
        const auto anchors = sidebar_.hintRects();
        for (std::size_t i = 0; i < hints_.size(); ++i) {
            const auto &hint = hints_[i];
            auto &badge = badges_[i];
            const auto anchor = std::find_if(anchors.begin(), anchors.end(), [&](const auto &rect) {
                return rect.buffer == hint.buffer;
            });
            const bool visible = hint.keys.starts_with(prefix_) && anchor != anchors.end();
            badge.box->setVisible(visible);
            if (!visible)
                continue;
            badge.label->setText(std::string_view(hint.keys).substr(prefix_.size()));
            badge.label->setFontFamily(themeUiFont().family);
            badge.label->setFontFeatures(themeUiFont().features);
            badge.label->setFontSize(14 * scale);
            badge.label->setFontWeight(FontWeight::Bold);
            badge.label->setColor(themeForegroundColor("on_primary_container"));
            badge.label->measure(renderer);
            const float badgeWidth = badge.label->width() + 12 * scale;
            const float badgeHeight = badge.label->height() + 8 * scale;
            const float x =
                std::clamp(anchor->x + 6 * scale, 0.f, std::max(0.f, width() - badgeWidth));
            const float y = std::clamp(anchor->y + (anchor->height - badgeHeight) / 2, 0.f,
                                       std::max(0.f, height() - badgeHeight));
            place(*badge.box, x, y, badgeWidth, badgeHeight);
            place(*badge.label, 6 * scale, 4 * scale, badge.label->width(), badge.label->height());
            badge.box->setFill(themeForegroundColor("primary_container"));
            badge.box->setBorder(themeForegroundColor("secondary"), 1.f);
            badge.box->setRadius(themeCornerRadius());
        }
        InputArea::doLayout(renderer);
    }

  private:
    struct Badge {
        Box *box;
        Label *label;
    };
    void erase(std::size_t index) {
        hints_.erase(hints_.begin() + index);
        badges_[hints_.size()].box->setVisible(false);
    }
    Sidebar &sidebar_;
    Box *scrim_;
    std::vector<ui_logic::JumpHint> hints_;
    std::vector<Badge> badges_;
    std::string prefix_;
};
class UiRoot final : public Node {
  public:
    std::function<bool()> afterChildrenLayout;
    bool inLayout = false;

  protected:
    void doLayout(Renderer &renderer) override {
        if (inLayout)
            return;
        inLayout = true;
        struct ResetLayout {
            bool &value;
            ~ResetLayout() { value = false; }
        } reset{inLayout};
        Node::doLayout(renderer);
        if (afterChildrenLayout && afterChildrenLayout())
            Node::doLayout(renderer);
    }
};
} // namespace

struct Ui::Impl {
    struct PaneView;
    Model &model;
    TextClipboard &clipboard;
    Workspace workspace;
    EditorStore editors;
    // Editors outlive all composer nodes registered with them.
    std::unique_ptr<UiRoot> scene = std::make_unique<UiRoot>();
    std::unique_ptr<MediaPreviewController> media;
    std::set<std::string> mediaFools;
    std::optional<FoolDisplay> mediaFoolDisplay;
    std::map<std::string, bool> mediaBufferFools;
    std::uint64_t sourceJumpRevision = 0;
    std::string mediaBuffer;
    MaterialNode *backdrop = nullptr, *peopleMaterial = nullptr;
    AppLogo *logo = nullptr;
    InputArea *titleMove = nullptr;
    Label *title = nullptr;
    Flex *titleRow = nullptr;
    ModeLine *modeLine = nullptr;
    Sidebar *sidebar = nullptr;
    People *people = nullptr;
    Dialogs *dialogs = nullptr;
    std::map<std::uint64_t, PaneView *> views;
    std::array<InputArea *, 8> resizeEdges{};
    std::array<IconButton *, 3> windowButtons{};
    std::array<std::string, 2> windowBindings;
    std::optional<SidebarMode> navigationHintPreferred, navigationHintEffective;
    ServerRail *rail = nullptr;
    std::array<Box *, 2> navigationDividers{};
    std::array<InputArea *, 2> widthHandles{};
    std::optional<std::size_t> widthDrag;
    float widthDragX = 0, widthDragStart = 0;
    std::optional<float> widthDragSceneX;
    bool widthDragChanged = false;
    SidebarMode widthDragMode = SidebarMode::Expanded;
    JumpOverlay *jump = nullptr;
    emoji::Picker *reactionPicker = nullptr;
    TopicPopover *topicPopover = nullptr;
    std::optional<std::pair<std::uint64_t, std::string>> topicSource;
    InputDispatcher::TabFocusSnapshot topicFocus;
    std::optional<TranscriptMessageTarget> reactionTarget, paletteMessageTarget;
    std::uint32_t reactionActivationKey = 0;
    std::vector<std::string_view> jumpCandidates;
    std::string jumpPrefix, jumpServer, jumpCancelBinding, jumpCancelChord;
    WorkspaceRect conversationRect;
    int savedUnread = -1;
    Node *palette = nullptr;
    Box *palettePanel = nullptr;
    Input *paletteQuery = nullptr;
    ScrollView *paletteList = nullptr;
    Label *paletteHelp = nullptr, *paletteTitle = nullptr;
    std::string paletteFont, paletteFeatures;
    FontWeight paletteWeight = FontWeight::Normal;
    float paletteScale = 0;
    std::vector<std::pair<std::string, std::string>> matches;
    struct PaletteSource {
        std::string id, target, caption;
    };
    std::vector<PaletteSource> paletteSources;
    std::vector<Button *> paletteRows;
    std::size_t paletteStart = 0;
    std::deque<std::string> pending;
    InputDispatcher *dispatcher = nullptr;
    WindowActions window;
    InputDispatcher::TabFocusSnapshot paletteFocus, jumpFocus;
    float width = 1000, height = 700, scale = 1;
    std::uint64_t conversationFocus = 0;
    bool windowActive = false, paletteOpen = false, commandMode = false;
    SidebarMode navigationRegularMode = SidebarMode::Expanded;
    SidebarMode peopleRegularMode = SidebarMode::Hidden;
    SidebarMode navigationMode = SidebarMode::Expanded, peopleMode = SidebarMode::Hidden;
    bool paletteHadFocus = false, paletteWasComposerInput = false;
    bool jumpOpen = false, jumpRestorePending = false, jumpHadFocus = false;
    bool jumpRestoreAfterLayout = false, restoringJumpFocus = false;
    bool jumpWasComposerInput = false, jumpRevealQueued = false, inputSynchronizing = false;
    bool inputGeometryPending = true;
    bool pointerOwnsTranscriptViewport = false;
    bool jumpTouchActive = false, jumpTouchDragged = false;
    float jumpTouchX = 0, jumpTouchY = 0, jumpTouchLastY = 0;
    std::uint32_t jumpActivationKey = 0;
    enum class TouchPanel { None, Navigation, People, Other, Captured };
    TouchPanel touchPanel = TouchPanel::None;
    bool shellDirty = true;
    std::string lastBuffer, chord, query, publishedActive, selectedServer;
    std::map<std::string, std::string> lastServerBuffer;
    std::size_t selected = 0;
    std::set<std::uint32_t> consumed;
    std::bitset<KEY_MAX + 1> consumedPhysical;
    std::array<std::bitset<KEY_MAX + 1>, 2> pressedPointer, suppressedPointer;
    std::map<std::uint32_t, std::string> repeatCommands;
    std::uint32_t titleClickTime = 0;
    platform::FilePicker filePicker;
    // Declared before the uploader so its worker stops before private source cleanup.
    struct PrivateUpload {
        std::string path;
        ~PrivateUpload() { clear(); }
        void clear() {
            if (!path.empty())
                ::unlink(path.c_str());
            path.clear();
        }
        bool create(std::string_view bytes, std::uint64_t cap, std::string_view mime) {
            clear();
            if (bytes.empty() || bytes.size() > cap)
                return false;
            const std::string_view suffix = mime == "image/png"    ? ".png"
                                            : mime == "image/jpeg" ? ".jpg"
                                            : mime == "image/webp" ? ".webp"
                                            : mime == "image/gif"  ? ".gif"
                                                                   : ".txt";
            std::string name = "/tmp/blast-upload-XXXXXX" + std::string(suffix);
            const int fd = ::mkstemps(name.data(), static_cast<int>(suffix.size()));
            if (fd < 0)
                return false;
            path = std::move(name);
            bool ok = ::fchmod(fd, S_IRUSR | S_IWUSR) == 0;
            std::size_t done = 0;
            while (ok && done < bytes.size()) {
                const auto count = ::write(fd, bytes.data() + done, bytes.size() - done);
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0)
                    ok = false;
                else
                    done += static_cast<std::size_t>(count);
            }
            if (::close(fd) != 0)
                ok = false;
            if (!ok)
                clear();
            return ok;
        }
    } privateUpload;
    Clipboard *payloadClipboard = dynamic_cast<Clipboard *>(&clipboard);
    std::uint64_t uploadPayloadRequest = 0;
    enum class UploadSource { File, Text, Clipboard, Image };
    UploadSource uploadSourceKind = UploadSource::File;
    std::string uploadBytes, uploadMime;
    std::shared_ptr<int> uploadAlive = std::make_shared<int>(0);
    Input *uploadText = nullptr;
    UploadPreview *uploadPreview = nullptr;
    bool uploadFocusRequested = false;
    Uploader uploader;
    enum class UploadStage {
        Closed,
        Choosing,
        Reading,
        Editing,
        Review,
        Uploading,
        Error,
        Complete
    };
    UploadStage uploadStage = UploadStage::Closed;
    EditorStore::UploadOrigin uploadOrigin;
    UploadConfig uploadConfig;
    std::optional<SojuFileHostContext> uploadSoju;
    platform::PickedFile uploadFile;
    std::string uploadSource, uploadSourceTarget, uploadMessage, uploadUrl;
    std::optional<UploadFileIdentity> uploadFileIdentity;
    UploadProgress uploadProgress;
    bool uploadCancelRequested = false;
    bool uploadAuthorizationRevoked = false;
    Box *uploadPanel = nullptr;
    ScrollView *uploadScroll = nullptr;
    Flex *uploadContent = nullptr;
    Label *uploadCaption = nullptr;
    std::array<Button *, 7> uploadButtons{};
    std::uint64_t uploadMenuGeneration = 0;
    Node *uploadMenuAnchor = nullptr;
    bool uploadMenuDismissed = false;
    std::uint64_t uploadMenuPane = 0;

    explicit Impl(Model &m, TextClipboard &c)
        : model(m), clipboard(c), workspace(m),
          editors(
              m, c, [this](std::string command) { pending.push_back(std::move(command)); },
              [this](std::uint64_t id) { focus(id); },
              [this](Node &affected) { retireSubtreePointerCapture(affected); }) {
        media = std::make_unique<MediaPreviewController>(model, *scene);
        media->setChanged([this] { mediaChanged(); });
        editors.setReplyReadable([this](std::uint64_t pane, const std::string &message) {
            return replyReadable(pane, message);
        });
        std::erase_if(model.settings.bindings, [](const auto &entry) {
            return entry.first == "close-tab" || entry.first == "next-tab" ||
                   entry.first.starts_with("select-tab:") ||
                   entry.first.starts_with("select-tab-keyboard:") ||
                   entry.first.starts_with("close-tab:") || entry.first.starts_with("reorder-tab:");
        });
        ui_logic::migrateMessageBindings(model.settings.bindings, [this](std::string_view name) {
            return model.settings.bindings.contains(std::string(name));
        });
        for (const auto &[name, binding] : defaultBindings()) {
            if ((name == "jump-channel" || name == "toggle-fools" || name == "conversation-back" ||
                 name == "conversation-forward" || name == "cycle-spelling") &&
                !model.settings.bindings.contains(name) &&
                std::any_of(model.settings.bindings.begin(), model.settings.bindings.end(),
                            [&](const auto &entry) {
                                return ui_logic::bindingConflicts(binding, entry.second);
                            }))
                model.settings.bindings.emplace(name, "");
            else
                model.settings.bindings.try_emplace(name, binding);
        }
        scene->afterChildrenLayout = [this] {
            inputGeometryPending = false;
            const bool restored = restoreJumpFocus(true);
            return focusUploadEditor() || restored;
        };
        editors.setOverlayHost(scene.get());
        editors.setOnEditingStateChanged([this](std::uint64_t paneId) { composerEdited(paneId); });
        editors.setOnUploadRequested([this](std::uint64_t pane, const std::string &buffer,
                                            Node &anchor,
                                            std::optional<InputArea::PointerData> pointer) {
            uploadSourceKind = UploadSource::File;
            uploadBytes.clear();
            uploadMime.clear();
            showUploadMenu(pane, buffer, anchor, std::move(pointer));
        });
        editors.setOnImageUploadRequested([this](std::uint64_t pane, const std::string &buffer,
                                                 Node &anchor, std::string mime,
                                                 std::string bytes) {
            if (uploadStage != UploadStage::Closed)
                return;
            uploadSourceKind = UploadSource::Image;
            uploadMime = std::move(mime);
            uploadBytes = std::move(bytes);
            showUploadMenu(pane, buffer, anchor, {});
        });
        auto callback = [this](std::string command) { pending.push_back(std::move(command)); };
        backdrop = static_cast<MaterialNode *>(
            scene->addChild(std::make_unique<MaterialNode>(MaterialKind::Backdrop)));
        backdrop->setHitTestVisible(false);
        titleMove = static_cast<InputArea *>(scene->addChild(std::make_unique<InputArea>()));
        titleMove->setOnPress([this](const InputArea::PointerData &event) {
            if (!event.pressed || event.button != BTN_LEFT)
                return;
            if (titleClickTime && event.time - titleClickTime < 300) {
                titleClickTime = 0;
                if (window.toggleMaximize)
                    window.toggleMaximize();
            } else {
                titleClickTime = event.time;
                if (window.beginMove)
                    window.beginMove(event.serial);
            }
        });
        logo = static_cast<AppLogo *>(scene->addChild(std::make_unique<AppLogo>()));
        titleRow = static_cast<Flex *>(scene->addChild(ui::row(
            {.align = FlexAlign::Center},
            ui::label({.out = &title, .text = "BlastIRC", .fontWeight = FontWeight::Bold}))));
        titleRow->setHitTestVisible(false);
        title->setHitTestVisible(false);
        title->setMaxLines(1);
        modeLine = static_cast<ModeLine *>(scene->addChild(std::make_unique<ModeLine>(model)));
        const std::array<std::pair<const char *, const char *>, 3> tools = {
            {{"panel-left", "Toggle navigation"},
             {"terminal", "Execute command"},
             {"x", "Close window"}}};
        constexpr std::array<const char *, 3> controlKeys = {"topbar-navigation", "topbar-command",
                                                             "topbar-close"};
        for (std::size_t i = 0; i < tools.size(); ++i) {
            auto button = std::make_unique<IconButton>(tools[i].first, tools[i].second, [this, i] {
                if (i == 0)
                    pending.emplace_back("toggle-navigation");
                else if (i == 1)
                    pending.emplace_back("execute-command");
                else if (i == 2) {
                    if (window.close)
                        window.close();
                    else
                        queue(model, "quit");
                }
            });
            button->setTabFocusKey(controlKeys[i]);
            if (i == 0)
                button->setFocusOnPointerPress(false);
            windowButtons[i] = static_cast<IconButton *>(scene->addChild(std::move(button)));
        }
        peopleMaterial =
            static_cast<MaterialNode *>(scene->addChild(std::make_unique<MaterialNode>()));
        peopleMaterial->setHitTestVisible(false);
        rail = static_cast<ServerRail *>(scene->addChild(std::make_unique<ServerRail>(
            model, callback, [this](Node &row, InputArea *replacement) {
                retireSubtreePointerCapture(row);
                if (dispatcher && withinNode(dispatcher->focusedArea(), &row))
                    dispatcher->setFocus(replacement);
            })));
        sidebar =
            static_cast<Sidebar *>(scene->addChild(std::make_unique<Sidebar>(model, callback)));
        people = static_cast<People *>(scene->addChild(
            std::make_unique<People>(model, callback, [this](SidebarMenuInvocation invocation) {
                run(std::move(invocation.command), std::move(invocation.pointer));
            })));
        for (auto &divider : navigationDividers) {
            divider = static_cast<Box *>(scene->addChild(std::make_unique<Box>()));
            divider->setHitTestVisible(false);
        }
        for (std::size_t i = 0; i < widthHandles.size(); ++i) {
            auto handle = std::make_unique<InputArea>();
            handle->setZIndex(100);
            handle->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
            handle->setCursorShape(26); // Native Wayland ew-resize.
            handle->setFocusable(true);
            handle->setFocusOnPointerPress(false);
            handle->setTabFocusKey(i == 0 ? "server-rail-resize" : "channel-sidebar-resize");
            handle->setTooltip(i == 0 ? "Resize server rail · Left/Right adjusts width"
                                      : "Resize channel sidebar · Left/Right adjusts width");
            handle->setOnPress([this, i](const InputArea::PointerData &event) {
                if (event.pressed) {
                    widthDrag = i;
                    widthDragX = event.sceneX;
                    widthDragStart = (i == 0 ? rail->width() : sidebar->width()) / scale;
                    widthDragMode = navigationMode;
                    widthDragSceneX.reset();
                    widthDragChanged = false;
                } else if (widthDrag == i) {
                    applyWidthDrag(event.sceneX);
                    finishWidthDrag();
                }
            });
            handle->setOnMotion([this, i](const InputArea::PointerData &event) {
                if (widthDrag == i) {
                    float sceneX = 0, sceneY = 0;
                    Node::mapToScene(widthHandles[i], event.localX, event.localY, sceneX, sceneY);
                    widthDragSceneX = sceneX;
                    if (window.requestFrame)
                        window.requestFrame();
                }
            });
            handle->setOnCancel([this] { finishWidthDrag(); });
            handle->setOnFocusGain(
                [this, i] { navigationDividers[i]->setFill(themeForegroundColor("secondary")); });
            handle->setOnFocusLoss([this, i] {
                navigationDividers[i]->setFill(
                    withAlpha(themeColor("outline_variant"), .18f * themeSurfaceAlpha()));
            });
            handle->setOnKeyDown([this, i](const InputArea::KeyData &event) {
                if (event.modifiers == 0 &&
                    (event.sym == XKB_KEY_Left || event.sym == XKB_KEY_Right)) {
                    if (widthDrag)
                        retirePointerCapture();
                    const float current = (i == 0 ? rail->width() : sidebar->width()) / scale;
                    if (resizePanel(i, current + (event.sym == XKB_KEY_Right ? 8.f : -8.f)))
                        queue(model, "save");
                }
            });
            widthHandles[i] = static_cast<InputArea *>(scene->addChild(std::move(handle)));
        }
        jump = static_cast<JumpOverlay *>(scene->addChild(std::make_unique<JumpOverlay>(*sidebar)));
        jump->setZIndex(1400);
        jump->setVisible(false);
        jump->setRetainsFocusOnPointerRelease(true);
        jump->setOnClick([this](const InputArea::PointerData &) {
            finishJump(false);
            refresh();
        });
        jump->setOnAxisHandler([this](const InputArea::PointerData &event) {
            if (!jumpOpen)
                return false;
            if (event.axis == 0) {
                sidebar->scrollHintCandidates(event.scrollDelta(52 * scale));
                jump->markLayoutDirty();
                refresh();
            }
            // A refresh may close the mode; never retain a gesture on its hidden overlay.
            return jumpOpen;
        });
        createPalette();
        dialogs =
            static_cast<Dialogs *>(scene->addChild(std::make_unique<Dialogs>(model, callback)));
        dialogs->setZIndex(1100);
        createUploadPanel();
        static constexpr std::array<std::uint32_t, 8> edges = {5, 1, 9, 8, 10, 2, 6, 4};
        for (std::size_t i = 0; i < edges.size(); ++i) {
            auto edge = std::make_unique<InputArea>();
            edge->setZIndex(1200);
            edge->setOnPress([this, value = edges[i]](const InputArea::PointerData &e) {
                if (e.pressed && e.button == BTN_LEFT && window.beginResize)
                    window.beginResize(e.serial, value);
            });
            resizeEdges[i] = static_cast<InputArea *>(scene->addChild(std::move(edge)));
        }
        if (const auto it = model.settings.workspace.find("selected_server");
            it != model.settings.workspace.end())
            selectedServer = it->second;
        for (const auto &[key, value] : model.settings.workspace)
            if (key.starts_with("last_buffer:"))
                lastServerBuffer.emplace(key.substr(12), value);
        if (model.firstRun)
            dialogs->open("first-run");
        model.bufferWillRename = [this](const std::string &oldId, const std::string &newId) {
            beforeBufferRename(oldId, newId);
        };
        model.bufferRenamed = [this](const std::string &oldId, const std::string &newId) {
            remapBuffer(oldId, newId);
        };
    }
    ~Impl() {
        uploadAlive.reset();
        if (payloadClipboard && uploadPayloadRequest)
            payloadClipboard->cancelPayload(uploadPayloadRequest);
        editors.setOnImageUploadRequested({});
        editors.setReplyReadable({});
        editors.setOnEditingStateChanged({});
        editors.setOnUploadRequested({});
        closeUploadMenu();
        model.bufferWillRename = {};
        model.bufferRenamed = {};
        if (scene->popupContext())
            scene->popupContext()->closeSelectDropdown();
        if (reactionPicker)
            reactionPicker->hide();
        editors.setOverlayHost(nullptr);
        shutdownMedia();
        scene.reset();
    }
    void createPalette();
    void showPalette(bool commands);
    void rebuildMatches(std::string_view preferred = {});
    void rebuildPaletteRows();
    void choose();
    void dismissPalette();
    void startJump();
    void updateJumpCandidates();
    void finishJump(bool committed, std::string buffer = {});
    bool restoreJumpFocus(bool afterLayout);
    bool completeJumpRestore();
    bool synchronizeInput(bool geometryPending = false);
    bool synchronizeKeyInput(std::uint32_t sym);
    bool synchronizePointerInput(float x, float y, bool targetInput, bool pendingReplay = true);
    void updateRegularPanelModes();
    bool resizePanel(std::size_t panel, float logicalWidth);
    void applyWidthDrag(float sceneX);
    void finishWidthDrag();
    void cancelPointerCapture();
    void retirePointerCapture();
    void retireSubtreePointerCapture(Node &subtree);
    void retireHiddenPointerCapture();
    void retirePanel(Node &panel, InputArea *replacement, bool hidden);
    void composerEdited(std::uint64_t paneId);
    bool transcriptOwnsFocus() const;
    bool messageCommand(std::string_view command);
    void showReactionPicker(TranscriptMessageTarget target);
    void finishReactionPicker(std::string glyph = {});
    void run(std::string command, std::optional<InputArea::PointerData> pointer = {});
    void drainPending();
    void focus(std::uint64_t id, bool focusComposer = true);
    void changeBuffer(std::string_view id);
    void beforeBufferRename(std::string_view oldId, std::string_view newId);
    void remapBuffer(std::string_view oldId, std::string_view newId);
    void menu(Node &, std::vector<ContextMenuEntry>, std::vector<std::string>,
              std::optional<InputArea::PointerData> pointer = {});
    void paneMenu(std::uint64_t id, Node &anchor, bool includeHiddenTools,
                  std::optional<InputArea::PointerData> pointer = {});
    BufferChromeCallbacks chromeCallbacks();
    void showTopic(std::uint64_t, Node &);
    void dismissTopic(bool restoreFocus = true);
    void nickMenu(std::string_view buffer, std::string_view nick,
                  std::optional<InputArea::PointerData> pointer = {});
    void selectServer(std::string_view name);
    void persistShell();
    bool uploadTrustCurrent() const {
        if (uploadAuthorizationRevoked)
            return false;
        if (uploadConfig.provider == UploadProvider::Soju)
            return uploadSoju && sojuFileHostContextCurrent(model, *uploadSoju);
        if (uploadConfig.provider == UploadProvider::Custom) {
            const auto &current = model.settings.uploads;
            return current.customEndpoint == uploadConfig.customEndpoint &&
                   current.username == uploadConfig.username &&
                   current.password == uploadConfig.password;
        }
        return true;
    }
    bool uploadMenuCurrent() const {
        return uploadMenuAnchor &&
               ((uploadMenuAnchor == uploadButtons[6] &&
                 (uploadStage == UploadStage::Editing || uploadStage == UploadStage::Review ||
                  uploadStage == UploadStage::Error)) ||
                editors.uploadSourceCurrent(uploadMenuPane, uploadOrigin, uploadMenuAnchor));
    }
    void closeUploadMenu() {
        if (!uploadMenuAnchor)
            return;
        ++uploadMenuGeneration;
        uploadMenuAnchor = nullptr;
        uploadMenuDismissed = false;
        if (uploadStage == UploadStage::Closed) {
            uploadBytes.clear();
            uploadMime.clear();
            uploadSourceKind = UploadSource::File;
        }
        if (scene->popupContext())
            scene->popupContext()->closeSelectDropdown();
    }
    void createUploadPanel();
    void showUploadMenu(std::uint64_t, const std::string &, Node &,
                        std::optional<InputArea::PointerData>);
    void showUploadHosts();
    void selectUploadHost(std::size_t value);
    void beginUploadSource();
    void prepareUploadBytes();
    bool chatControl(std::string_view command, std::string_view buffer) {
        const bool fools =
            command == "toggle-fools" || command == "show-fools" || command == "hide-fools";
        if (fools) {
            const auto source = model.buffers.find(std::string(buffer));
            if (source == model.buffers.end() ||
                (source->second.kind != "channel" && source->second.kind != "query"))
                return true;
            const bool shown = effectiveFoolDisplay(model.settings, source->first,
                                                    source->second.target) == FoolDisplay::Shown;
            model.settings.bufferFools[source->first] =
                command == "show-fools" || (command == "toggle-fools" && !shown);
            if (scene->popupContext())
                scene->popupContext()->closeSelectDropdown();
            finishReactionPicker();
            media->invalidate();
            queue(model, "save");
            return true;
        }
        const bool mute = command == "mute-buffer" || command == "unmute-buffer";
        const bool archive = command == "archive-buffer" || command == "unarchive-buffer";
        const bool pin = command == "pin-buffer" || command == "unpin-buffer";
        if (!mute && !archive && !pin)
            return false;
        const auto source = model.buffers.find(std::string(buffer));
        if (source != model.buffers.end() &&
            (source->second.kind == "channel" || source->second.kind == "query"))
            queue(model,
                  pin    ? "pin_buffer"
                  : mute ? "mute_buffer"
                         : "archive_buffer",
                  source->first,
                  command == "pin-buffer" || command == "mute-buffer" || command == "archive-buffer"
                      ? "on"
                      : "off");
        return true;
    }
    void uploadAction(std::string_view);
    void pollUpload();
    void syncUploadPanel();
    bool focusUploadEditor() {
        if (!uploadFocusRequested)
            return false;
        if (uploadStage != UploadStage::Editing) {
            uploadFocusRequested = false;
            return false;
        }
        if (!dispatcher || !windowActive || !visibleWithinNode(uploadText, scene.get()) ||
            paletteOpen || jumpOpen || reactionTarget || topicSource ||
            (dialogs && dialogs->active()) || (media && media->active()))
            return false;
        auto *previous = dispatcher->focusedArea();
        auto *native = nativeInputFor(previous);
        if (previous != uploadText->inputArea() &&
            (editors.composing() || (native && native->composing())))
            return false;
        uploadFocusRequested = false;
        for (auto &[id, pane] : workspace.panes)
            pane.focusComposer = false;
        dispatcher->setFocus(uploadText->inputArea());
        return dispatcher->focusedArea() != previous;
    }
    void pruneViews();
    void syncViews();
    void refresh(bool panelResize = false);
    bool repairFocus();
    bool key(const InputArea::KeyData &, std::uint32_t physicalKey);
    bool replyReadable(std::uint64_t, const std::string &) const;
    void mediaChanged();
    void shutdownMedia();
};

struct Ui::Impl::PaneView final : Node {
    Impl &ui;
    PaneState &pane;
    MaterialNode *conversationMaterial = nullptr;
    BufferTitleLine *titleLine = nullptr;
    Box *titleDivider = nullptr;
    TranscriptView *transcript = nullptr;
    Node *composer = nullptr;
    Node *replyHost = nullptr;
    Node *motionHost = nullptr;
    ScrollView *welcomeScroll = nullptr;
    Flex *welcome = nullptr;
    Label *welcomeTitle = nullptr, *welcomeBody = nullptr;
    float userScale = 1;
    PaneView(Impl &owner, PaneState &state) : ui(owner), pane(state) {
        setClipChildren(true);
        conversationMaterial = static_cast<MaterialNode *>(
            addChild(std::make_unique<MaterialNode>(MaterialKind::Conversation)));
        conversationMaterial->setHitTestVisible(false);
        titleLine = static_cast<BufferTitleLine *>(
            addChild(std::make_unique<BufferTitleLine>(ui.model, ui.chromeCallbacks())));
        titleDivider = static_cast<Box *>(titleLine->addChild(std::make_unique<Box>()));
        titleDivider->setHitTestVisible(false);
        transcript = static_cast<TranscriptView *>(addChild(std::make_unique<TranscriptView>(
            ui.model, pane, ui.clipboard,
            [this](std::string command) { ui.pending.push_back(std::move(command)); },
            [this](std::uint64_t id) { ui.focus(id); })));
        transcript->setMediaPreviewController(ui.media.get());
        transcript->setOnNickMenu(
            [this](std::string command, std::optional<InputArea::PointerData> pointer) {
                ui.run(std::move(command), std::move(pointer));
            });
        if (ui.dispatcher) {
            titleLine->setInputDispatcher(*ui.dispatcher);
            transcript->setInputDispatcher(*ui.dispatcher);
        }
        composer = addChild(ui.editors.composer(pane));
        replyHost = addChild(std::make_unique<Node>());
        ui.editors.setReplyHost(pane.id, replyHost);
        welcomeScroll = static_cast<ScrollView *>(addChild(std::make_unique<ScrollView>()));
        welcomeScroll->setViewportPaddingH(0);
        welcomeScroll->setViewportPaddingV(0);
        welcomeScroll->setReserveScrollbarGutter(true);
        welcome = static_cast<Flex *>(welcomeScroll->content()->addChild(ui::column(
            {.gap = 12},
            ui::label({.out = &welcomeTitle,
                       .text = "Welcome to BlastIRC",
                       .fontSize = 22,
                       .maxLines = 0}),
            ui::label({.out = &welcomeBody,
                       .text = "Add a network from the left rail, then choose a conversation.",
                       .maxLines = 0}))));
        welcomeScroll->content()->setAlign(FlexAlign::Stretch);
        welcome->setFillWidth(true);
        motionHost = addChild(std::make_unique<Node>());
        motionHost->setHitTestVisible(false);
        motionHost->setExcludeSubtreeFromTabOrder(true);
        motionHost->setClipChildren(true);
        transcript->setMotionHost(motionHost);
    }
    ~PaneView() override {
        ui.editors.setReplyHost(pane.id, nullptr);
        transcript->setMotionHost(nullptr);
    }
    void sync(const WorkspaceRect &rect, bool selected) {
        place(*this, rect);
        setVisible(selected);
        setExcludeSubtreeFromTabOrder(!selected);
        transcript->setWindowActive(ui.windowActive);
        titleLine->sync(pane, ui.workspace.focused == pane.id, ui.scale, ui.peopleRegularMode,
                        ui.navigationMode);
        if (!selected) {
            transcript->setReadEligible(false);
            ui.editors.setComposerHeightLimit(pane.id, 0);
            return;
        }
        userScale = ui.scale;
        const auto found = ui.model.buffers.find(pane.buffer);
        const bool exists = found != ui.model.buffers.end();
        const bool search =
            exists && (found->second.kind == "search" || found->second.kind == "mentions");
        conversationMaterial->sync();
        titleDivider->setFill(withAlpha(themeColor("outline_variant"), .18f * themeSurfaceAlpha()));
        transcript->setVisible(exists);
        transcript->setReadEligible(ui.windowActive && ui.conversationFocus == pane.id &&
                                    ui.workspace.focused == pane.id && selected &&
                                    !ui.paletteOpen && !ui.jumpOpen && !ui.reactionTarget &&
                                    !ui.dialogs->active());
        transcript->sync();
        composer->setVisible(exists && !search);
        welcomeScroll->setVisible(!exists);
        welcome->setGap(12 * userScale);
        for (auto *label : {welcomeTitle, welcomeBody}) {
            label->setFontFamily(themeUiFont().family);
            label->setFontFeatures(themeUiFont().features);
            label->setFontWeight(themeUiFont().weight);
            label->setFontSize((label == welcomeTitle ? 22.f : 14.f) * userScale);
            label->setColor(themeForegroundColor("on_surface"));
        }
    }

  protected:
    void doLayout(Renderer &renderer) override {
        const float s = userScale;
        const bool shortPane = height() < 360 * s;
        const float paddingX = (shortPane ? 8.f : panePaddingX) * s;
        const float paddingY = shortPane ? 0.f : 8 * s;
        const float panelGap = (shortPane ? 6.f : 12.f) * s;
        const float left = paddingX;
        const float innerWidth = std::max(0.f, width() - 2 * left);
        place(*conversationMaterial, 0, 0, width(), height());
        titleLine->setCompact(shortPane);
        LayoutConstraints titleConstraints = LayoutConstraints::unconstrained();
        titleConstraints.setExactWidth(width());
        const float headerHeight = std::min(titleLine->measure(renderer, titleConstraints).height,
                                            std::max(0.f, height()));
        place(*titleLine, 0, 0, width(), headerHeight);
        place(*titleDivider, 0, std::max(0.f, headerHeight - 1.f), width(),
              std::min(1.f, headerHeight));
        const float top = headerHeight + panelGap;
        const float available = std::max(0.f, height() - top);
        const bool composing = composer->visible();
        const auto activeBuffer = ui.model.buffers.find(pane.buffer);
        const float messageSize = activeBuffer == ui.model.buffers.end()
                                      ? messageTextSize(ui.model.settings, Buffer{})
                                      : messageTextSize(ui.model.settings, activeBuffer->second);
        const float historyRow = renderer.fontRowExtent(messageSize, themeMessageFont());
        const float typingRow = composing ? typingIndicatorHeight(renderer, s, messageSize) : 0;
        const float replyHeight =
            composing ? ui.editors.replyContextHeight(pane.id, renderer, innerWidth) : 0;
        const float replyOverlayHeight = composing ? replyHeight + typingRow + 4 * s : 0;
        const float historyMinimum = 2 * historyRow + 12 * s;
        // Only typing reserves history space; the reply card floats above it.
        // Let paste previews/accessories compact before spending the native history floor.
        const float preferredMaximum =
            std::max(0.f, available - 2 * paddingY - historyMinimum - typingRow - panelGap);
        const float preferredBudget =
            std::max(std::max(32.f, 32 * s), std::min(available * .5f, preferredMaximum));
        const float minimumComposer =
            composing
                ? ui.editors.minimumComposerHeight(pane.id, renderer, innerWidth, preferredMaximum)
                : 0;
        const float composerBudget =
            std::min(std::max(preferredBudget, minimumComposer),
                     std::max(0.f, available - 2 * paddingY - historyMinimum - panelGap));
        ui.editors.setComposerHeightLimit(pane.id, composing ? composerBudget : 0);
        LayoutConstraints editorConstraints = LayoutConstraints::unconstrained();
        editorConstraints.setExactWidth(std::max(1.f, innerWidth));
        const float desiredComposer =
            composing ? composer->measure(renderer, editorConstraints).height : 0;
        const float composerCardHeight =
            composing ? std::min(desiredComposer + 2 * paddingY, available) : 0;
        const float composerPadding = std::min(paddingY, composerCardHeight / 2);
        const float composerHeight = std::max(0.f, composerCardHeight - 2 * composerPadding);
        const float composerGap =
            composing ? std::min(panelGap, std::max(0.f, available - composerCardHeight)) : 0;
        const float historyHeight = std::max(0.f, available - composerCardHeight - composerGap);
        const float composerY = top + historyHeight + composerGap;
        place(*transcript, left, top, innerWidth, historyHeight);
        // In an impossible pane the typing inset yields before native history text.
        // Reply visibility never changes the content extent or composer budget.
        const float inset = std::min(typingRow, std::max(0.f, historyHeight - 2 * historyRow));
        transcript->setContextInset(inset, typingRow,
                                    composing && !pane.reply.empty() ? replyOverlayHeight : 0);
        // Zero host bounds pass empty-zone hits through while traversing the real card.
        place(*replyHost, left, top + historyHeight - replyOverlayHeight, 0, 0);
        replyHost->setVisible(composing);
        replyHost->setExcludeSubtreeFromTabOrder(!composing);
        for (const auto &reply : replyHost->children())
            reply->arrange(renderer, {0, 0, innerWidth, replyHeight});
        // setSize would pin this content-sized Flex to its previous measured height.
        composer->arrange(renderer,
                          {left, composerY + composerPadding, innerWidth, composerHeight});
        float composerLeft = 0, composerTop = 0, composerRight = 0, composerBottom = 0;
        Node::transformedBounds(composer, composerLeft, composerTop, composerRight, composerBottom);
        transcript->setComposerOrigin((composerLeft + composerRight) * .5f, composerTop);
        welcomeScroll->setContentScale(s);
        place(*welcomeScroll, left, top, innerWidth, available);
        place(*motionHost, 0, 0, width(), height());
        Node::doLayout(renderer);
    }
};

bool Ui::Impl::replyReadable(std::uint64_t pane, const std::string &message) const {
    const auto found = views.find(pane);
    return found != views.end() && found->second->transcript->messageReadable(message);
}
void Ui::Impl::mediaChanged() {
    for (auto &[id, view] : views)
        view->transcript->invalidateMediaPreviews();
}
void Ui::Impl::shutdownMedia() {
    media->setChanged({});
    for (auto &[id, view] : views)
        view->transcript->setMediaPreviewController(nullptr);
    media->invalidate();
    media.reset();
}
void Ui::Impl::updateRegularPanelModes() {
    const float currentScale = ui_logic::userScale(model.settings.scale);
    const float outer = shellOuterPadding(width, height, currentScale);
    const float rail = fittedRailWidth(workspace, width, outer, outer, currentScale);
    const float remaining = std::max(1.f, width - 2 * outer - rail - outer);
    const float minimum =
        std::max(360 * currentScale,
                 EditorStore::minimumComposerWidth(currentScale) + 2 * panePaddingX * currentScale);
    const auto pane = workspace.panes.find(workspace.focused);
    const auto buffer = pane == workspace.panes.end() ? model.buffers.end()
                                                      : model.buffers.find(pane->second.buffer);
    const bool channel = buffer != model.buffers.end() && buffer->second.kind == "channel";
    const auto modes = fitSidebarModes(
        workspace.navigationMode, channel ? model.settings.peopleMode : SidebarMode::Hidden,
        remaining, outer, currentScale, minimum,
        workspace.channelSidebarWidth().value_or(navigationExpandedExtent) * currentScale,
        workspace.compactChannelSidebarWidth().value_or(compactPanelExtent) * currentScale);
    navigationRegularMode = modes.first;
    peopleRegularMode = modes.second;
}
bool Ui::Impl::resizePanel(std::size_t panel, float logicalWidth) {
    if (!std::isfinite(logicalWidth) || jumpOpen || paletteOpen || dialogs->active())
        return false;
    const float minimumChat =
        std::max(360 * scale, EditorStore::minimumComposerWidth(scale) + 2 * panePaddingX * scale);
    const float current = (panel == 0 ? rail->width() : sidebar->width()) / scale;
    const float maximum = current + std::max(0.f, conversationRect.width - minimumChat) / scale;
    logicalWidth = std::min(logicalWidth, maximum);
    const bool compact = (widthDrag ? widthDragMode : navigationMode) == SidebarMode::Compact;
    const float previous = panel == 0 ? workspace.serverRailWidth().value_or(current)
                           : compact  ? workspace.compactChannelSidebarWidth().value_or(current)
                                      : workspace.channelSidebarWidth().value_or(current);
    if (panel == 0)
        workspace.setServerRailWidth(logicalWidth);
    else if (compact)
        workspace.setCompactChannelSidebarWidth(logicalWidth);
    else
        workspace.setChannelSidebarWidth(logicalWidth);
    const float next = panel == 0 ? *workspace.serverRailWidth()
                       : compact  ? *workspace.compactChannelSidebarWidth()
                                  : *workspace.channelSidebarWidth();
    if (next == previous)
        return false;
    if (widthDrag)
        widthDragChanged = true;
    refresh(true);
    return true;
}
void Ui::Impl::applyWidthDrag(float sceneX) {
    widthDragSceneX.reset();
    if (!widthDrag)
        return;
    if (scale != ui_logic::userScale(model.settings.scale) || jumpOpen || paletteOpen ||
        dialogs->active()) {
        retirePointerCapture();
        return;
    }
    resizePanel(*widthDrag, widthDragStart + (sceneX - widthDragX) / scale);
}
void Ui::Impl::finishWidthDrag() {
    widthDragSceneX.reset();
    widthDrag.reset();
    if (std::exchange(widthDragChanged, false))
        queue(model, "save");
}
void Ui::Impl::cancelPointerCapture() {
    finishWidthDrag();
    if (pressedPointer[1].test(BTN_LEFT))
        suppressedPointer[1].set(BTN_LEFT);
    touchPanel = TouchPanel::None;
    if (dispatcher)
        dispatcher->cancelPointerCapture();
}
void Ui::Impl::retirePointerCapture() {
    for (std::size_t source = 0; source < pressedPointer.size(); ++source)
        suppressedPointer[source] |= pressedPointer[source];
    cancelPointerCapture();
}
void Ui::Impl::retireSubtreePointerCapture(Node &subtree) {
    if (uploadMenuAnchor) {
        const auto containsAnchor = [&](auto &&self, const Node &node) -> bool {
            if (&node == uploadMenuAnchor)
                return true;
            for (const auto &child : node.children())
                if (self(self, *child))
                    return true;
            return false;
        };
        if (containsAnchor(containsAnchor, subtree))
            closeUploadMenu();
    }
    if (!dispatcher)
        return;
    auto *area = dispatcher->capturedArea();
    if (!area)
        area = dispatcher->pendingTouchPressArea();
    if (withinNode(area, &subtree))
        retirePointerCapture();
}
void Ui::Impl::retireHiddenPointerCapture() {
    if (!dispatcher)
        return;
    auto *captured = dispatcher->capturedArea();
    if (!captured || visibleWithinNode(captured, scene.get()))
        return;
    retirePointerCapture();
}
void Ui::Impl::retirePanel(Node &panel, InputArea *replacement, bool hidden) {
    if (!dispatcher)
        return;
    const bool refocus = hidden && withinNode(dispatcher->focusedArea(), &panel);
    const bool touchOwned = (touchPanel == TouchPanel::Navigation && &panel == sidebar) ||
                            (touchPanel == TouchPanel::People && &panel == people);
    if (touchOwned || pressedWithin(panel) ||
        (touchPanel == TouchPanel::None && !dispatcher->pointerCaptured() &&
         withinNode(dispatcher->hoveredArea(), &panel))) {
        retirePointerCapture();
    }
    if (refocus)
        dispatcher->setFocus(replacement);
}
void Ui::Impl::composerEdited(std::uint64_t paneId) {
    if (const auto view = views.find(paneId); view != views.end())
        view->second->transcript->clearTextSelection();
}
bool Ui::Impl::transcriptOwnsFocus() const {
    const auto view = views.find(workspace.focused);
    if (!dispatcher || view == views.end())
        return false;
    for (Node *node = dispatcher->focusedArea(); node; node = node->parent())
        if (node == view->second->transcript)
            return true;
    return false;
}
bool Ui::Impl::messageCommand(std::string_view command) {
    const bool previous = command == "message-prev", next = command == "message-next";
    const bool reply = command == "message-reply" || command.starts_with("message-reply:");
    const bool react = command == "message-react" || command.starts_with("message-react:");
    if (!previous && !next && !reply && !react)
        return false;
    if (jumpOpen || paletteOpen || dialogs->active() || reactionTarget) {
        model.status = "Close the current dialog before selecting a message";
        return true;
    }
    if (editors.composing()) {
        model.status = "Finish composition before using message actions";
        return true;
    }
    std::optional<TranscriptMessageTarget> target;
    if (command.starts_with("message-reply:") || command.starts_with("message-react:")) {
        const auto split = command.find('\n', 14);
        if (split == std::string_view::npos) {
            model.status = "Message is no longer available";
            return true;
        }
        TranscriptMessageTarget bound{};
        bound.buffer = command.substr(14, split - 14);
        const auto identity = command.substr(split + 1);
        const auto [end, error] =
            std::from_chars(identity.data(), identity.data() + identity.size(), bound.identity);
        if (error != std::errc{} || end != identity.data() + identity.size() || !bound.identity ||
            !model.buffers.contains(bound.buffer)) {
            model.status = "Message is no longer available";
            return true;
        }
        const auto pane = workspace.open(bound.buffer);
        focus(pane, false);
        syncViews();
        const auto source = views.find(pane);
        if (source != views.end())
            target = source->second->transcript->resolveMessage(bound);
        if (!target) {
            model.status = "Message is no longer available";
            return true;
        }
    }
    const auto source = views.find(workspace.focused);
    if (source == views.end() || !source->second->visible() ||
        !source->second->transcript->visible()) {
        model.status = "No conversation is available";
        return true;
    }
    auto &view = *source->second;
    if (previous || next) {
        if (!view.transcript->navigateMessage(previous ? -1 : 1)) {
            focus(view.pane.id);
            editors.sync();
        }
        return true;
    }
    if (!target) {
        target = view.transcript->selectedMessage();
    }
    if (!target) {
        model.status = transcriptOwnsFocus() ? "Selected message is no longer available"
                                             : "Select a message with C-p first";
        return true;
    }
    if (reply) {
        if (!view.transcript->replyToMessage(*target))
            model.status = "This message is no longer available for reply";
    } else if (!target->canReact)
        model.status = "The server does not support reactions to this message";
    else
        showReactionPicker(std::move(*target));
    return true;
}
void Ui::Impl::showReactionPicker(TranscriptMessageTarget target) {
    if (jumpRestorePending)
        completeJumpRestore();
    else {
        jumpHadFocus = dispatcher && dispatcher->focusedArea();
        jumpWasComposerInput = dispatcher && editors.ownsFocus(dispatcher->focusedArea());
        jumpFocus =
            dispatcher ? dispatcher->captureTabFocus() : InputDispatcher::TabFocusSnapshot{};
    }
    if (!reactionPicker) {
        reactionPicker = static_cast<emoji::Picker *>(
            scene->addChild(std::make_unique<emoji::Picker>(editors.emojiCatalog())));
        reactionPicker->setZIndex(1080);
        if (dispatcher)
            reactionPicker->setInputDispatcher(*dispatcher);
    }
    reactionTarget = std::move(target);
    reactionActivationKey = 0;
    cancelPointerCapture();
    for (auto &[id, pane] : workspace.panes)
        pane.focusComposer = false;
    reactionPicker->sync(width, height, scale);
    reactionPicker->show(
        "React to " + reactionTarget->nick, {},
        [this](std::string glyph) { finishReactionPicker(std::move(glyph)); },
        [this] { finishReactionPicker(); });
}
void Ui::Impl::finishReactionPicker(std::string glyph) {
    if (!reactionTarget)
        return;
    auto target = std::move(*reactionTarget);
    reactionTarget.reset();
    reactionActivationKey = 0;
    reactionPicker->hide();
    if (!glyph.empty()) {
        const auto source = views.find(workspace.focused);
        if (source == views.end() || source->second->pane.buffer != target.buffer ||
            !source->second->transcript->reactToMessage(target, std::move(glyph)))
            model.status = "Message or reaction capability is no longer available";
    }
    // Reuse the existing single bounded native-return slot; no modal stack or event replay.
    jumpRestorePending = true;
    jumpRestoreAfterLayout = false;
    restoreJumpFocus(false);
}
void Ui::Impl::beforeBufferRename(std::string_view oldId, std::string_view newId) {
    dismissTopic();
    editors.prepareBufferRename(oldId, newId);
    for (const auto &[id, view] : views)
        if (view->pane.buffer == oldId || view->pane.buffer == newId)
            view->transcript->beforeBufferRename(oldId, newId);
}

void Ui::Impl::remapBuffer(std::string_view oldId, std::string_view newId) {
    if (scene->popupContext())
        scene->popupContext()->closeSelectDropdown();
    editors.remapBuffer(oldId, newId, workspace.focused);
    for (const auto &[id, view] : views)
        view->transcript->remapBuffer(oldId, newId);
    std::uint64_t oldPane = 0, newPane = 0;
    if ((jumpOpen || jumpRestorePending || paletteOpen || reactionTarget || dialogs->active()) &&
        oldId != newId)
        for (const auto &[id, pane] : workspace.panes) {
            if (pane.buffer == oldId)
                oldPane = id;
            if (pane.buffer == newId)
                newPane = id;
        }
    workspace.remapBuffer(oldId, newId);
    if (oldPane && newPane && oldPane != newPane) {
        const auto survivor = workspace.panes.contains(oldPane) ? oldPane : newPane;
        const auto removed = survivor == oldPane ? newPane : oldPane;
        for (auto *snapshot : {&jumpFocus, &paletteFocus})
            for (auto *key : {&snapshot->key, &snapshot->fallbackKey})
                if (key->has_value())
                    ui_logic::remapPaneFocusKey(key->value(), removed, survivor);
        dialogs->remapPane(removed, survivor);
    }
    pruneViews();
    dialogs->remapBuffer(oldId, newId);
    people->remapBuffer(oldId, newId);
    sidebar->remapBuffer(oldId, newId);
    for (auto *snapshot : {&jumpFocus, &paletteFocus})
        for (auto *key : {&snapshot->key, &snapshot->fallbackKey})
            if (key->has_value() && !key->value().empty())
                ui_logic::remapBufferFocusKey(key->value(), oldId, newId);
    for (auto *target : {&reactionTarget, &paletteMessageTarget})
        if (*target && (*target)->buffer == oldId)
            (*target)->buffer = newId;
    if (jumpOpen) {
        jump->remap(oldId, newId);
        updateJumpCandidates();
    }
    if (oldId != newId) {
        if (lastBuffer == oldId)
            lastBuffer = newId;
        if (publishedActive == oldId)
            publishedActive = newId;
        for (auto &command : pending) {
            const size_t start = command.starts_with("buffer:")           ? 7
                                 : command.starts_with("nick:")           ? 5
                                 : command.starts_with("nick-action:")    ? 12
                                 : command.starts_with("insert-nick:")    ? 12
                                 : command.starts_with("buffer-command:") ? 15
                                 : command.starts_with("message-react:")  ? 14
                                 : command.starts_with("message-reply:")  ? 14
                                                                          : 0;
            if (!start)
                continue;
            const auto end = start == 7 ? command.size() : command.find('\n', start);
            if (end != std::string::npos &&
                std::string_view(command).substr(start, end - start) == oldId)
                command.replace(start, end - start, newId);
        }
    }
    for (auto &[server, buffer] : lastServerBuffer)
        if (buffer == oldId) {
            buffer = newId;
            shellDirty = true;
        }
    if (paletteOpen) {
        std::string preferred = selected < matches.size() ? matches[selected].first : std::string{};
        if (!commandMode && preferred == oldId)
            preferred = newId;
        rebuildMatches(preferred);
    }
    for (const auto &[id, view] : views)
        if (view->pane.buffer == newId)
            view->titleLine->sync(view->pane, workspace.focused == id, scale, peopleRegularMode,
                                  navigationMode);
    // This callback runs inside core reduction, including the final shutdown
    // tick. Publish live identities before core can perform any same-tick save.
    workspace.persist();
    persistShell();
}

void Ui::Impl::createUploadPanel() {
    uploadPanel = static_cast<Box *>(scene->addChild(std::make_unique<Box>()));
    uploadPanel->setZIndex(1050);
    uploadPanel->setDialogStyle();
    uploadPanel->setVisible(false);
    uploadPanel->setClipChildren(true);
    uploadScroll = static_cast<ScrollView *>(uploadPanel->addChild(std::make_unique<ScrollView>()));
    uploadScroll->setViewportPaddingH(0);
    uploadScroll->setViewportPaddingV(0);
    uploadScroll->setReserveScrollbarGutter(true);
    uploadScroll->content()->setDirection(FlexDirection::Vertical);
    uploadScroll->content()->setAlign(FlexAlign::Stretch);
    auto content = ui::column({.align = FlexAlign::Stretch, .gap = 6, .padding = 10},
                              ui::label({.out = &uploadCaption, .maxLines = 0}));
    uploadContent = content.get();
    uploadText = static_cast<Input *>(content->addChild(std::make_unique<Input>()));
    uploadText->inputArea()->setTabFocusKey("upload-text");
    uploadText->setMultiline(true);
    uploadText->setVisible(false);
    uploadText->setOnChange([this](const std::string &) {
        if ((uploadStage == UploadStage::Review || uploadStage == UploadStage::Error) &&
            uploadMime == "text/plain" && !uploadFile.path.empty()) {
            uploadStage = UploadStage::Editing;
            uploadFile = {};
            uploadFileIdentity.reset();
            privateUpload.clear();
            uploadPreview->reset();
            uploadMessage = "Text changed; review again before uploading";
        }
    });
    uploadPreview =
        static_cast<UploadPreview *>(content->addChild(std::make_unique<UploadPreview>()));
    uploadPreview->setVisible(false);
    uploadContent->setFillWidth(true);
    auto actions = ui::row({.gap = 6});
    actions->setWrap(true);
    static constexpr std::array<std::string_view, 7> commands = {
        "upload-start",       "upload-insert",  "upload-copy", "upload-close",
        "upload-review-text", "upload-default", "upload-host"};
    for (std::size_t i = 0; i < commands.size(); ++i) {
        auto button = std::make_unique<Button>();
        button->setTabStop(true);
        button->inputArea()->setTabFocusKey(std::string(commands[i]));
        button->setOnClick(
            [this, command = std::string(commands[i])] { pending.push_back(command); });
        uploadButtons[i] = static_cast<Button *>(actions->addChild(std::move(button)));
    }
    content->addChild(std::move(actions));
    uploadScroll->content()->addChild(std::move(content));
}

void Ui::Impl::showUploadMenu(std::uint64_t pane, const std::string &buffer, Node &anchor,
                              std::optional<InputArea::PointerData> pointer) {
    if (uploadStage != UploadStage::Closed) {
        model.status = "Finish or close the current upload first";
        return;
    }
    auto origin = editors.captureUploadOrigin(pane);
    const auto source = model.buffers.find(buffer);
    if (!origin || source == model.buffers.end() || !anchor.popupContext())
        return;
    uploadOrigin = *origin;
    uploadSource = source->second.server;
    uploadSourceTarget = source->second.kind == "server" ? "Server log" : source->second.target;
    uploadConfig = model.settings.uploads;
    uploadAuthorizationRevoked = false;
    uploadSoju = sojuFileHostContext(model, uploadSource);
    const auto generation = ++uploadMenuGeneration;
    uploadMenuAnchor = &anchor;
    uploadMenuPane = pane;
    uploadMenuDismissed = false;
    if (uploadSourceKind == UploadSource::Image) {
        beginUploadSource();
        return;
    }
    std::vector<ContextMenuEntry> sources;
    for (const auto &[value, label] : std::array<std::pair<std::size_t, const char *>, 3>{
             {{0, "File…"}, {1, "Text / paste…"}, {2, "Clipboard…"}}}) {
        ContextMenuEntry entry{};
        entry.value = value;
        entry.label = label;
        entry.enabled = value != 2 || payloadClipboard;
        sources.push_back(std::move(entry));
    }
    float x = 0, y = 0;
    Node::absolutePosition(&anchor, x, y);
    SelectPopupContext::DropdownRequest request{};
    request.anchorX = static_cast<std::int32_t>(x);
    request.anchorY = static_cast<std::int32_t>(y);
    request.anchorWidth = static_cast<std::int32_t>(std::max(1.f, anchor.width()));
    request.anchorHeight = static_cast<std::int32_t>(std::max(1.f, anchor.height()));
    request.inputSerial = pointer ? pointer->serial : 0;
    request.menuWidth = 250 * scale;
    request.fontSize = 14 * scale;
    request.font = themeUiFont();
    request.menu = std::move(sources);
    SelectPopupContext::DropdownCallbacks callbacks{};
    callbacks.onSelect = [this, generation](std::size_t value) {
        if (generation != uploadMenuGeneration || !uploadMenuCurrent() || value > 2)
            return;
        uploadSourceKind = static_cast<UploadSource>(value);
        beginUploadSource();
    };
    callbacks.onDismiss = [this, generation] {
        if (generation == uploadMenuGeneration)
            uploadMenuDismissed = true;
    };
    anchor.popupContext()->openSelectDropdown(request, std::move(callbacks));
}

void Ui::Impl::showUploadHosts() {
    if (!uploadMenuCurrent())
        return;
    auto &anchor = *uploadMenuAnchor;
    const auto generation = ++uploadMenuGeneration;
    uploadMenuDismissed = false;
    std::vector<ContextMenuEntry> entries;
    static constexpr std::array<std::string_view, 8> providerIcons = {
        "clipboard", "upload", "cloud", "clock", "cat", "network", "link", "server"};
    for (auto provider : {UploadProvider::CrafterBin, UploadProvider::X0At, UploadProvider::Uguu,
                          UploadProvider::Litterbox, UploadProvider::Catbox, UploadProvider::CNet,
                          UploadProvider::Custom, UploadProvider::Soju}) {
        ContextMenuEntry entry{};
        entry.value = static_cast<std::size_t>(provider);
        entry.label = std::string(uploadProviderLabel(provider));
        entry.icon = providerIcons[static_cast<std::size_t>(provider)];
        entry.radio = true;
        entry.toggleState = provider == uploadConfig.provider ? 1 : 0;
        entry.enabled = provider != UploadProvider::Soju || uploadSoju.has_value();
        if (!entry.enabled)
            entry.label += " — unavailable for this source";
        entries.push_back(std::move(entry));
    }
    ContextMenuEntry settings{};
    settings.value = 8;
    settings.label = "Web host settings…";
    settings.icon = "settings";
    entries.push_back(std::move(settings));
    float x = 0, y = 0;
    Node::absolutePosition(&anchor, x, y);
    SelectPopupContext::DropdownRequest request{};
    request.anchorX = static_cast<std::int32_t>(x);
    request.anchorY = static_cast<std::int32_t>(y);
    request.anchorWidth = static_cast<std::int32_t>(std::max(1.f, anchor.width()));
    request.anchorHeight = static_cast<std::int32_t>(std::max(1.f, anchor.height()));
    request.inputSerial = 0;
    request.menuWidth = 300 * scale;
    request.maxMenuWidth = 440 * scale;
    request.fontSize = 14 * scale;
    request.font = themeUiFont();
    request.maxVisibleOptions = 10;
    request.selectedIndex = static_cast<std::size_t>(uploadConfig.provider);
    request.menu = std::move(entries);
    SelectPopupContext::DropdownCallbacks callbacks{};
    callbacks.onSelect = [this, generation](std::size_t value) {
        if (generation == uploadMenuGeneration)
            selectUploadHost(value);
    };
    callbacks.onDismiss = [this, generation] {
        if (generation == uploadMenuGeneration)
            uploadMenuDismissed = true;
    };
    anchor.popupContext()->openSelectDropdown(request, std::move(callbacks));
}
void Ui::Impl::selectUploadHost(std::size_t value) {
    const bool existing = uploadStage != UploadStage::Closed;
    if ((existing && uploadStage != UploadStage::Editing && uploadStage != UploadStage::Review &&
         uploadStage != UploadStage::Error) ||
        (!existing && !uploadMenuCurrent()))
        return;
    ++uploadMenuGeneration;
    uploadMenuAnchor = nullptr;
    if (value == 8) {
        pending.push_back("settings-uploads");
        return;
    }
    if (value > static_cast<std::size_t>(UploadProvider::Soju))
        return;
    uploadConfig = model.settings.uploads;
    uploadConfig.provider = static_cast<UploadProvider>(value);
    uploadConfig.maxBytes =
        std::min(uploadConfig.maxBytes, uploadProviderLimit(uploadConfig.provider));
    if (uploadConfig.provider != UploadProvider::Custom) {
        uploadConfig.username.clear();
        uploadConfig.password.clear();
    }
    if (!uploadTrustCurrent()) {
        uploadAuthorizationRevoked = true;
        uploadStage = UploadStage::Error;
        uploadMessage = "Host authorization or source session changed; choose the host again";
        return;
    }
    if (existing) {
        uploadMessage.clear();
        if (!uploadFile.path.empty()) {
            uploadStage = UploadStage::Review;
            if (uploadFile.size > uploadConfig.maxBytes) {
                uploadStage = UploadStage::Error;
                uploadMessage = "Source exceeds this host's size limit; choose another host";
            }
        }
        return;
    }
    uploadFile = {};
    uploadFileIdentity.reset();
    uploadPreview->reset();
    uploadUrl.clear();
    uploadMessage.clear();
    uploadProgress = {};
    if (uploadSourceKind == UploadSource::File) {
        if (filePicker.open())
            uploadStage = UploadStage::Choosing;
        else {
            uploadStage = UploadStage::Error;
            uploadMessage = "The native file chooser could not be opened";
        }
    } else if (uploadSourceKind == UploadSource::Text) {
        uploadText->setValue("");
        uploadStage = UploadStage::Editing;
        uploadFocusRequested = true;
    } else if (uploadSourceKind == UploadSource::Image) {
        prepareUploadBytes();
    } else if (payloadClipboard) {
        uploadStage = UploadStage::Reading;
        const auto requestGeneration = uploadMenuGeneration;
        const std::weak_ptr<int> token = uploadAlive;
        uploadPayloadRequest = payloadClipboard->requestPayload(
            uploadConfig.maxBytes, [this, token, requestGeneration](Clipboard::Payload payload) {
                if (token.expired())
                    return;
                if (requestGeneration != uploadMenuGeneration ||
                    uploadStage != UploadStage::Reading)
                    return;
                uploadPayloadRequest = 0;
                if (payload.status != Clipboard::PayloadStatus::Success) {
                    uploadStage = UploadStage::Error;
                    uploadMessage = "Clipboard unavailable, changed, or larger than the host limit";
                    return;
                }
                uploadMime = std::move(payload.mime);
                uploadBytes = std::move(payload.bytes);
                if (!uploadMime.starts_with("image/")) {
                    uploadText->setValue(uploadBytes);
                    uploadBytes.clear();
                    uploadStage = UploadStage::Editing;
                    uploadFocusRequested = true;
                } else
                    prepareUploadBytes();
            });
    }
}

void Ui::Impl::beginUploadSource() {
    if (!uploadMenuCurrent())
        return;
    const bool available = uploadConfig.provider == UploadProvider::Soju
                               ? uploadSoju.has_value()
                               : safeUploadUrl(uploadEndpoint(uploadConfig));
    if (!available || !uploadTrustCurrent()) {
        showUploadHosts();
        return;
    }
    selectUploadHost(static_cast<std::size_t>(uploadConfig.provider));
}

void Ui::Impl::prepareUploadBytes() {
    const bool editing = uploadStage == UploadStage::Editing;
    const auto cap = std::min(uploadConfig.maxBytes, uploadProviderLimit(uploadConfig.provider));
    uploadFile = {};
    uploadFileIdentity.reset();
    if (!privateUpload.create(uploadBytes, cap, uploadMime)) {
        uploadBytes.clear();
        uploadStage = editing ? UploadStage::Editing : UploadStage::Error;
        uploadMessage = "Source is empty, exceeds the host limit, or could not be staged privately";
        return;
    }
    uploadBytes.clear();
    const auto identity = inspectUploadFile(privateUpload.path);
    if (!identity || identity->size > cap) {
        privateUpload.clear();
        uploadStage = editing ? UploadStage::Editing : UploadStage::Error;
        uploadMessage = "Private source could not be reviewed";
        return;
    }
    uploadFile.path = privateUpload.path;
    uploadFile.mime = uploadMime;
    uploadFile.name = uploadMime.starts_with("image/") ? "Clipboard image" : "Text paste";
    uploadFile.identity = *identity;
    uploadFile.size = identity->size;
    uploadFileIdentity = *identity;
    uploadPreview->reset(uploadFile.path, uploadFileIdentity);
    uploadStage = UploadStage::Review;
    uploadMessage.clear();
}

void Ui::Impl::uploadAction(std::string_view action) {
    if (action == "upload-host" &&
        (uploadStage == UploadStage::Editing || uploadStage == UploadStage::Review ||
         uploadStage == UploadStage::Error)) {
        if (uploadText->composing()) {
            uploadMessage = "Finish input-method composition before changing host";
            return;
        }
        uploadMenuAnchor = uploadButtons[6];
        showUploadHosts();
        return;
    }
    if (action == "upload-default" && uploadTrustCurrent() && uploadStage != UploadStage::Closed &&
        uploadStage != UploadStage::Uploading) {
        model.settings.uploads.provider = uploadConfig.provider;
        model.settings.uploads.maxBytes =
            std::min(model.settings.uploads.maxBytes, uploadProviderLimit(uploadConfig.provider));
        queue(model, "save");
        uploadMessage = "Host made default";
        return;
    }
    if (action == "upload-review-text" && uploadStage == UploadStage::Editing) {
        if (uploadText->composing()) {
            uploadMessage = "Finish input-method composition before reviewing";
            return;
        }
        uploadBytes = uploadText->value();
        uploadMime = "text/plain";
        prepareUploadBytes();
        return;
    }
    if (action == "upload-close") {
        if (uploadStage == UploadStage::Uploading) {
            uploader.cancel();
            uploadCancelRequested = true;
            uploadMessage = "Cancelling upload…";
        } else if (uploadStage == UploadStage::Choosing) {
            filePicker.cancel();
            uploadMessage = "Closing file chooser…";
        } else {
            retireSubtreePointerCapture(*uploadPanel);
            ++uploadMenuGeneration;
            if (payloadClipboard && uploadPayloadRequest)
                payloadClipboard->cancelPayload(uploadPayloadRequest);
            uploadPayloadRequest = 0;
            privateUpload.clear();
            uploadFile = {};
            uploadFileIdentity.reset();
            uploadPreview->reset();
            uploadBytes.clear();
            uploadMime.clear();
            uploadText->setValue("");
            uploadSourceKind = UploadSource::File;
            uploadStage = UploadStage::Closed;
            uploadFocusRequested = false;
            uploadUrl.clear();
            uploadSoju.reset();
            uploadConfig.password.clear();
        }
        return;
    }
    if (action == "upload-copy" && uploadStage == UploadStage::Complete) {
        clipboard.setClipboardText(uploadUrl);
        uploadMessage = "Link copied";
    } else if (action == "upload-insert" && uploadStage == UploadStage::Complete) {
        if (editors.insertUpload(uploadOrigin, uploadUrl, true) ==
            EditorStore::UploadInsertResult::Inserted) {
            uploadAction("upload-close");
        } else {
            uploadMessage = "Finish composition and collapse selection in the original draft, or "
                            "Copy the retained link";
        }
    } else if (action == "upload-start" &&
               (uploadStage == UploadStage::Review || uploadStage == UploadStage::Error) &&
               !uploadFile.path.empty() && uploadFile.size <= uploadConfig.maxBytes) {
        if (uploadMime == "text/plain" && uploadText->composing()) {
            uploadMessage = "Finish input-method composition before uploading reviewed text";
            return;
        }
        if (!uploadTrustCurrent()) {
            uploadAuthorizationRevoked = true;
            uploadStage = UploadStage::Error;
            uploadMessage = "Host authorization or source session changed; choose the host again";
            return;
        }
        if (!uploadFileIdentity || inspectUploadFile(uploadFile.path) != uploadFileIdentity) {
            uploadStage = UploadStage::Error;
            uploadPreview->reset();
            uploadMessage = "The reviewed file changed; close and choose it again";
            return;
        }
        UploadRequest request;
        request.config = uploadConfig;
        request.path = uploadFile.path;
        request.mime = uploadFile.mime;
        request.reviewedFile = uploadFileIdentity;
        if (uploadConfig.provider == UploadProvider::Soju) {
            request.endpoint = uploadSoju->endpoint;
            request.config.username = uploadSoju->account;
            request.config.password = uploadSoju->password;
            request.credentialHost = uploadSoju->physicalHost;
            request.trustedFileHost = uploadSoju->trustedHost;
        }
        if (uploader.start(std::move(request))) {
            uploadStage = UploadStage::Uploading;
            uploadCancelRequested = false;
            uploadMessage.clear();
            uploadProgress = {};
        }
    }
}

void Ui::Impl::pollUpload() {
    if (auto result = filePicker.poll()) {
        if (uploadStage == UploadStage::Choosing) {
            if (result->status == platform::FilePickerStatus::Cancelled) {
                uploadStage = UploadStage::Error;
                uploadAction("upload-close");
            } else if (result->status == platform::FilePickerStatus::Error) {
                uploadStage = UploadStage::Error;
                uploadMessage = result->error;
            } else {
                uploadFile = std::move(result->file);
                uploadFileIdentity = uploadFile.identity;
                uploadFile.size = uploadFile.identity.size;
                const auto currentFile = inspectUploadFile(uploadFile.path);
                if (!currentFile || *currentFile != *uploadFileIdentity) {
                    uploadFile = {};
                    uploadStage = UploadStage::Error;
                    uploadMessage = "The selected file changed before review; choose it again";
                    return;
                }
                uploadStage = UploadStage::Review;
                uploadMessage.clear();
                uploadPreview->reset(uploadFile.path, uploadFileIdentity);
            }
        }
    }
    if (auto progress = uploader.takeProgress())
        uploadProgress = *progress;
    if (uploadStage == UploadStage::Uploading && !uploadTrustCurrent()) {
        uploader.cancel();
        uploadCancelRequested = true;
        uploadAuthorizationRevoked = true;
        uploadMessage = "Host authorization or source session changed; cancelling upload";
    }
    if ((uploadStage == UploadStage::Review || uploadStage == UploadStage::Error ||
         uploadStage == UploadStage::Choosing || uploadStage == UploadStage::Editing ||
         uploadStage == UploadStage::Reading) &&
        !uploadTrustCurrent()) {
        uploadAuthorizationRevoked = true;
        uploadMessage =
            "Host authorization or source session changed; close and choose the host again";
        if (uploadStage != UploadStage::Choosing)
            uploadStage = UploadStage::Error;
    }
    if (auto result = uploader.takeResult()) {
        if (*result) {
            uploadUrl = std::move(result->url);
            uploadStage = UploadStage::Complete;
            if (!uploadCancelRequested && uploadTrustCurrent() &&
                editors.insertUpload(uploadOrigin, uploadUrl) ==
                    EditorStore::UploadInsertResult::Inserted) {
                uploadAction("upload-close");
            } else {
                uploadMessage =
                    uploadCancelRequested
                        ? "Upload finished before cancellation; link retained for Insert or Copy"
                        : "Link ready; original draft changed or source session unavailable";
            }
        } else {
            uploadStage = UploadStage::Error;
            uploadMessage = result->error == UploadError::Cancelled
                                ? "Upload cancelled. Retry only if you choose."
                                : result->message;
        }
    }
}

void Ui::Impl::syncUploadPanel() {
    const bool visible = uploadStage != UploadStage::Closed;
    if (!visible && uploadPanel->visible())
        retireSubtreePointerCapture(*uploadPanel);
    uploadPanel->setVisible(visible);
    uploadPanel->setExcludeSubtreeFromTabOrder(!visible);
    if (!visible)
        return;
    const bool editing = uploadStage == UploadStage::Editing;
    const bool textReview =
        uploadMime == "text/plain" && !uploadFile.path.empty() &&
        (uploadStage == UploadStage::Review || uploadStage == UploadStage::Error);
    const bool showText = editing || textReview;
    if (!showText && uploadText->visible())
        retireSubtreePointerCapture(*uploadText);
    uploadText->setVisible(showText);
    uploadText->setExcludeSubtreeFromTabOrder(!showText);
    uploadText->setFontSize(13 * scale);
    uploadText->setFontWeight(themeUiFont().weight);
    std::string caption = uploadSourceTarget;
    caption.append(" · ").append(serverDisplayName(model, uploadSource));
    caption.append(" · ").append(uploadProviderLabel(uploadConfig.provider));
    if (uploadStage == UploadStage::Choosing)
        caption += "\nChoose one local file in the native file chooser";
    else if (uploadStage == UploadStage::Reading)
        caption += "\nReading a bounded clipboard snapshot…";
    else if (uploadStage == UploadStage::Editing)
        caption += "\nEdit or paste text, then review. Nothing is sent automatically.";
    else if (uploadStage == UploadStage::Review || uploadStage == UploadStage::Error) {
        if (!uploadFile.path.empty())
            caption += "\n" + uploadFile.name + " · " +
                       (uploadFile.mime.empty() ? "application/octet-stream" : uploadFile.mime) +
                       " · " + std::to_string(uploadFile.size) + " bytes";
        const auto resolvedEndpoint = uploadConfig.provider == UploadProvider::Soju
                                          ? std::string{}
                                          : uploadEndpoint(uploadConfig);
        const std::string_view endpoint =
            uploadConfig.provider == UploadProvider::Soju && uploadSoju
                ? std::string_view(uploadSoju->endpoint)
                : std::string_view(resolvedEndpoint);
        if (safeUploadUrl(endpoint)) {
            const auto schemeEnd = endpoint.find("://");
            if (schemeEnd != std::string_view::npos) {
                const auto authorityEnd = endpoint.find_first_of("/?#", schemeEnd + 3);
                caption.append("\nDestination: ").append(endpoint.substr(0, authorityEnd));
            }
        }
        if (uploadStage == UploadStage::Review)
            caption +=
                uploadConfig.secret && (uploadConfig.provider == UploadProvider::CrafterBin ||
                                        uploadConfig.provider == UploadProvider::Custom)
                    ? "\nVisibility: unlisted, accessible to anyone with the link."
                    : "\nVisibility: public/link-accessible; host retention rules apply.";
        if (uploadStage == UploadStage::Review && uploadConfig.provider == UploadProvider::Soju)
            caption += "\nThis host receives your soju account username and password via HTTPS.";
        if (textReview)
            caption += "\nReviewed text below; editing requires review again.";
        else if (!uploadFile.path.empty())
            caption += "\nLocal image preview when supported; otherwise file metadata only.";
    } else if (uploadStage == UploadStage::Uploading)
        caption += "\nUploading " + std::to_string(uploadProgress.sent) + " / " +
                   std::to_string(uploadProgress.total) + " bytes";
    else if (uploadStage == UploadStage::Complete)
        caption += "\n" + uploadUrl;
    if (!uploadMessage.empty())
        caption += "\n" + uploadMessage;
    uploadCaption->setText(caption);
    uploadCaption->setTooltip(caption);
    uploadCaption->setFontFamily(themeUiFont().family);
    uploadCaption->setFontWeight(themeUiFont().weight);
    uploadCaption->setFontSize(12 * scale);
    uploadCaption->setColor(themeForegroundColor("on_surface"));
    uploadButtons[0]->setText(uploadStage == UploadStage::Error ? "Retry upload" : "Upload");
    uploadButtons[1]->setText("Insert");
    uploadButtons[2]->setText("Copy");
    uploadButtons[3]->setText(
        uploadStage == UploadStage::Uploading || uploadStage == UploadStage::Choosing ? "Cancel"
                                                                                      : "Close");
    uploadButtons[4]->setText("Review text");
    uploadButtons[5]->setText("Make default");
    uploadButtons[6]->setText("Choose host…");
    const std::array<bool, 7> shown = {
        (uploadStage == UploadStage::Review || uploadStage == UploadStage::Error) &&
            !uploadFile.path.empty() && uploadFile.size <= uploadConfig.maxBytes &&
            uploadTrustCurrent() && (!textReview || !uploadText->composing()),
        uploadStage == UploadStage::Complete,
        uploadStage == UploadStage::Complete,
        true,
        editing,
        uploadStage != UploadStage::Uploading && uploadTrustCurrent() &&
            uploadConfig.provider != model.settings.uploads.provider,
        editing || uploadStage == UploadStage::Review || uploadStage == UploadStage::Error};
    for (std::size_t i = 0; i < uploadButtons.size(); ++i) {
        auto *button = uploadButtons[i];
        if (!shown[i] && button->visible())
            retireSubtreePointerCapture(*button);
        button->setVisible(shown[i]);
        button->setExcludeSubtreeFromTabOrder(!shown[i]);
        button->setFontSize(12 * scale);
        button->label()->setFontWeight(themeUiFont().weight);
    }
    uploadContent->setGap(6 * scale);
    uploadContent->setPadding(8 * scale);
    uploadPanel->setFill(themeSurfaceColor("surface_container_high"));
    uploadPanel->setRadius(themeCornerRadius());
    const float margin =
        std::min(8 * scale, std::min(conversationRect.width, conversationRect.height) * .05f);
    const float panelWidth =
        std::min(540 * scale, std::max(1.f, conversationRect.width - 2 * margin));
    const bool previewing = showText || uploadPreview->visible();
    const float panelHeight = std::min(previewing ? 420 * scale : 230 * scale,
                                       std::max(1.f, conversationRect.height - 2 * margin));
    uploadText->setSize(std::max(1.f, panelWidth - 16 * scale),
                        std::max(96 * scale, std::min(180 * scale, panelHeight * .55f)));
    uploadPreview->setSize(std::max(1.f, panelWidth - 16 * scale), 160 * scale);
    place(*uploadPanel, conversationRect.x + (conversationRect.width - panelWidth) / 2,
          conversationRect.y + conversationRect.height - panelHeight - margin, panelWidth,
          panelHeight);
    uploadScroll->setContentScale(scale);
    place(*uploadScroll, 0, 0, panelWidth, panelHeight);
}

void Ui::Impl::createPalette() {
    auto overlay = std::make_unique<InputArea>();
    overlay->setZIndex(1300);
    overlay->setVisible(false);
    overlay->setOnClick([this](const InputArea::PointerData &e) {
        if (!inside({palettePanel->x(), palettePanel->y(), palettePanel->width(),
                     palettePanel->height()},
                    e.localX, e.localY))
            dismissPalette();
    });
    palettePanel = static_cast<Box *>(overlay->addChild(std::make_unique<Box>()));
    palettePanel->setDialogStyle();
    auto content = ui::column({.align = FlexAlign::Stretch, .gap = 10, .padding = 16},
                              ui::label({.out = &paletteTitle, .text = "Commands", .fontSize = 18}),
                              ui::input({.out = &paletteQuery,
                                         .placeholder = "Type to filter…",
                                         .onChange =
                                             [this](const std::string &value) {
                                                 query = value;
                                                 selected = 0;
                                                 rebuildMatches();
                                             },
                                         .onSubmit = [this](const std::string &) { choose(); }}),
                              ui::scrollView({.out = &paletteList, .flexGrow = 1}),
                              ui::label({.out = &paletteHelp, .text = "↑ ↓ select · Enter open"}));
    paletteList->content()->setAlign(FlexAlign::Stretch);
    palettePanel->addChild(std::move(content));
    palette = scene->addChild(std::move(overlay));
}
void Ui::Impl::showPalette(bool commands) {
    if (reactionTarget) {
        model.status = "Close the reaction picker before opening the command palette";
        return;
    }
    commandMode = commands;
    paletteOpen = true;
    selected = 0;
    query.clear();
    paletteQuery->setValue("");
    for (auto &[id, pane] : workspace.panes)
        pane.focusComposer = false;
    paletteQuery->setPlaceholder(commands ? "Find a command…" : "Find a conversation…");
    paletteQuery->inputArea()->setTooltip(
        commandLabel(model, commands ? "Execute command" : "Switch conversation",
                     commands ? "execute-command" : "switch-buffer"));
    paletteHadFocus = dispatcher && dispatcher->focusedArea();
    paletteWasComposerInput = dispatcher && editors.ownsFocus(dispatcher->focusedArea());
    paletteFocus = dispatcher ? dispatcher->captureTabFocus() : InputDispatcher::TabFocusSnapshot{};
    paletteMessageTarget.reset();
    if (const auto view = views.find(workspace.focused); view != views.end())
        paletteMessageTarget = view->second->transcript->selectedMessage();
    palette->setVisible(true);
    rebuildMatches();
    if (dispatcher)
        dispatcher->setFocus(paletteQuery->inputArea());
}
void Ui::Impl::dismissPalette() {
    paletteOpen = false;
    palette->setVisible(false);
    if (dialogs->active()) {
        if (dispatcher)
            dispatcher->restoreTabFocus(std::move(paletteFocus));
    } else if (auto view = views.find(workspace.focused); view != views.end()) {
        auto &target = *view->second;
        if (target.visible() && target.composer->visible()) {
            target.pane.focusComposer = true;
            editors.sync();
        } else {
            target.pane.focusComposer = false;
            if (dispatcher && windowActive)
                dispatcher->restoreTabFocus(std::move(paletteFocus));
        }
    }
    paletteMessageTarget.reset();
}
void Ui::Impl::startJump() {
    if (jumpOpen || reactionTarget || dialogs->active() || editors.composing())
        return;
    if (dispatcher)
        if (auto *input = dispatcher->focusedArea(); input && input->textInputClient())
            if (const auto *native = dynamic_cast<Input *>(input->textInputClient());
                native && native->composing())
                return;
    if (window.keyboardComposeEnabled && !window.keyboardComposeEnabled(false))
        return;
    if (scene->popupContext())
        scene->popupContext()->closeSelectDropdown();
    jumpServer.assign(selectedServer);
    if (jumpServer.empty())
        if (const auto buffer = model.buffers.find(model.activeBuffer);
            buffer != model.buffers.end())
            jumpServer = buffer->second.server;
    const auto cancel = model.settings.bindings.find("cancel");
    jumpCancelBinding = cancel == model.settings.bindings.end()
                            ? std::string{}
                            : ui_logic::canonical(cancel->second);
    auto firstCancel = std::string_view(jumpCancelBinding).substr(0, jumpCancelBinding.find(' '));
    if (firstCancel.starts_with("s-"))
        firstCancel.remove_prefix(2);
    const char reserved =
        firstCancel.size() == 1
            ? ui_logic::jumpHintKey(static_cast<unsigned char>(firstCancel.front()))
            : '\0';
    auto hints = ui_logic::avyHints(model, jumpServer, reserved);
    if (hints.empty()) {
        if (window.keyboardComposeEnabled)
            window.keyboardComposeEnabled(true);
        model.status = "No channels or conversations on the selected network";
        return;
    }
    const bool paletteReturn = paletteOpen;
    const bool inheritReturnFocus = jumpRestorePending && !paletteReturn;
    if (jumpRestorePending)
        completeJumpRestore();
    if (paletteReturn) {
        jumpFocus = std::move(paletteFocus);
        jumpHadFocus = paletteHadFocus;
        jumpWasComposerInput = paletteWasComposerInput;
        paletteOpen = false;
        palette->setVisible(false);
    } else if (!inheritReturnFocus) {
        jumpHadFocus = dispatcher && dispatcher->focusedArea();
        jumpWasComposerInput = dispatcher && editors.ownsFocus(dispatcher->focusedArea());
    }
    for (std::size_t source = 0; source < pressedPointer.size(); ++source)
        suppressedPointer[source] |= pressedPointer[source];
    if (dispatcher) {
        if (!inheritReturnFocus && !paletteReturn)
            jumpFocus = dispatcher->captureTabFocus();
        cancelPointerCapture();
    }
    jumpTouchActive = false;
    jumpTouchDragged = false;
    jumpRestorePending = false;
    jumpRestoreAfterLayout = false;
    jumpActivationKey = 0;
    jumpPrefix.clear();
    jumpCancelChord.clear();
    jump->begin(std::move(hints));
    jumpOpen = true;
    jump->setVisible(true);
    place(*jump, 0, 0, width, height);
    if (dispatcher)
        dispatcher->syncPointerHover();
    for (auto &[id, pane] : workspace.panes)
        pane.focusComposer = false;
    updateJumpCandidates();
    if (dispatcher)
        dispatcher->setFocus(jump);
}
void Ui::Impl::updateJumpCandidates() {
    jumpCandidates.clear();
    for (const auto &hint : jump->hints()) {
        const auto source = model.buffers.find(hint.buffer);
        if (source != model.buffers.end() && !source->second.archived &&
            hint.keys.starts_with(jumpPrefix))
            jumpCandidates.push_back(hint.buffer);
    }
    if (jumpCandidates.empty()) {
        finishJump(false);
        return;
    }
    sidebar->setHintCandidates(jumpCandidates);
    jump->prefix(jumpPrefix);
}
void Ui::Impl::finishJump(bool committed, std::string buffer) {
    if (!jumpOpen)
        return;
    if (committed) {
        const auto source = model.buffers.find(buffer);
        committed = source != model.buffers.end() && !source->second.archived;
    }
    if (window.keyboardComposeEnabled)
        window.keyboardComposeEnabled(true);
    for (std::size_t source = 0; source < pressedPointer.size(); ++source)
        suppressedPointer[source] |= pressedPointer[source];
    cancelPointerCapture();
    jumpOpen = false;
    jumpRestoreAfterLayout = false;
    jumpActivationKey = 0;
    jumpTouchActive = false;
    jumpTouchDragged = false;
    jump->setVisible(false);
    if (dispatcher)
        dispatcher->syncPointerHover();
    sidebar->clearHintCandidates(committed);
    jumpCandidates.clear();
    jumpPrefix.clear();
    jumpCancelChord.clear();
    jump->begin({});
    if (committed) {
        jumpRestorePending = false;
        changeBuffer(buffer);
    } else
        jumpRestorePending = true;
}
bool Ui::Impl::completeJumpRestore() {
    if (!jumpRestorePending)
        return false;
    const bool queuedOffset = sidebar->finishHintFocusRestore();
    jumpRestorePending = false;
    jumpRestoreAfterLayout = false;
    return queuedOffset;
}
bool Ui::Impl::restoreJumpFocus(bool afterLayout) {
    if (!jumpRestorePending || jumpOpen || afterLayout != jumpRestoreAfterLayout)
        return false;
    restoringJumpFocus = true;
    jumpRevealQueued = false;
    if (dispatcher) {
        dispatcher->setFocus(nullptr);
        if (jumpHadFocus) {
            jumpFocus.index.reset();
            if (afterLayout) {
                dispatcher->restoreTabFocus(std::move(jumpFocus));
                if (!dispatcher->focusedArea())
                    if (const auto found = views.find(workspace.focused);
                        found != views.end() && found->second->visible()) {
                        const auto &view = *found->second;
                        if (jumpWasComposerInput && view.composer->visible())
                            dispatcher->restoreTabFocus(
                                {.index = std::nullopt,
                                 .key = "composer:" + std::to_string(workspace.focused) + ":input",
                                 .fallbackKey = std::nullopt});
                        if (!dispatcher->focusedArea() && view.transcript->visible() &&
                            view.transcript->width() > 0 && view.transcript->height() > 0)
                            dispatcher->restoreTabFocus(
                                {.index = std::nullopt,
                                 .key = "transcript:" + std::to_string(workspace.focused) +
                                        ":keyboard",
                                 .fallbackKey = std::nullopt});
                    }
            } else if (jumpFocus.key && !jumpFocus.key->empty())
                dispatcher->restoreTabFocus(
                    {.index = std::nullopt, .key = jumpFocus.key, .fallbackKey = std::nullopt});
        }
    }
    restoringJumpFocus = false;
    if (!afterLayout && dispatcher && jumpHadFocus && !dispatcher->focusedArea()) {
        // Keep the full snapshot until actual child layout has rebound native targets.
        jumpRestoreAfterLayout = true;
        scene->markLayoutDirty();
        return false;
    }
    const bool queuedOffset = completeJumpRestore();
    return afterLayout && (jumpRevealQueued || queuedOffset);
}
bool Ui::Impl::synchronizeInput(bool geometryPending) {
    if (!jumpRestorePending && !geometryPending)
        return true;
    if (restoringJumpFocus && dispatcher && dispatcher->focusedArea() && !geometryPending)
        return true;
    if (inputSynchronizing || scene->inLayout || currentUiPhase() == UiPhase::Layout ||
        currentUiPhase() == UiPhase::Render)
        return false;
    if (!window.synchronizeLayout)
        throw std::runtime_error(
            "A native layout synchronizer is required for input geometry/focus");
    inputSynchronizing = true;
    struct ResetSynchronization {
        bool &value;
        ~ResetSynchronization() { value = false; }
    } reset{inputSynchronizing};
    if (!window.synchronizeLayout())
        throw std::runtime_error("Cannot prepare native input: rendering context unavailable");
    if (jumpRestorePending)
        throw std::runtime_error("Native layout did not resolve the pending focus return");
    return true;
}
bool Ui::Impl::synchronizeKeyInput(std::uint32_t sym) {
    const bool geometryKey = Input::usesTextGeometry(sym);
    const auto dirtyInput = [this] {
        auto *area = dispatcher ? dispatcher->focusedArea() : nullptr;
        if (!visibleWithinNode(area, scene.get()))
            return false;
        auto *input = nativeInputFor(area);
        return input && (inputGeometryPending || input->textGeometryDirty());
    };
    if (!synchronizeInput(geometryKey && dirtyInput()))
        return false;
    if (geometryKey && dirtyInput())
        throw std::runtime_error("Native layout did not prepare focused input geometry");
    return true;
}
bool Ui::Impl::synchronizePointerInput(float x, float y, bool targetInput, bool pendingReplay) {
    retireHiddenPointerCapture();
    const auto currentInput = [this, x, y, targetInput, pendingReplay]() -> Input * {
        if (!dispatcher)
            return nullptr;
        if (auto *captured = dispatcher->capturedArea())
            return nativeInputFor(captured);
        if (targetInput)
            return nativeInputFor(dispatcher->inputAreaAt(x, y));
        auto *area = pendingReplay ? dispatcher->pendingTouchPressArea() : nullptr;
        return area && area->enabled() && visibleWithinNode(area, scene.get())
                   ? nativeInputFor(area)
                   : nullptr;
    };
    bool geometryPending = inputGeometryPending && targetInput;
    if (inputGeometryPending && !targetInput && dispatcher) {
        geometryPending = dispatcher->pointerCaptured();
        if (!geometryPending && pendingReplay) {
            auto *area = dispatcher->pendingTouchPressArea();
            geometryPending = area && area->enabled() && visibleWithinNode(area, scene.get());
        }
    }
    if (!geometryPending) {
        auto *input = currentInput();
        geometryPending = input && input->textGeometryDirty();
    }
    if (!geometryPending && (!targetInput || !jumpRestorePending))
        return true;
    if (!synchronizeInput(geometryPending))
        return false;
    retireHiddenPointerCapture();
    auto *input = currentInput();
    if (inputGeometryPending || (input && input->textGeometryDirty()))
        throw std::runtime_error("Native layout did not prepare pointer input geometry");
    return true;
}
void Ui::Impl::rebuildMatches(std::string_view preferred) {
    struct Match {
        int score;
        std::string id, label;
    };
    std::vector<Match> found;
    if (commandMode) {
        const auto active = model.buffers.find(model.activeBuffer);
        const bool networkLog = active != model.buffers.end() && active->second.kind == "server";
        std::set<std::string> names;
        for (const auto &[name, binding] : model.settings.bindings) {
            names.insert(name);
            if (networkLog && name == "close-buffer")
                continue;
            const std::string display(commandDisplayName(name));
            const int score =
                std::max(ui_logic::fuzzy(query, name), ui_logic::fuzzy(query, display));
            if (score > -100000)
                found.push_back(
                    {score, name, commandLabel(model, display, name) + "    [" + name + "]"});
        }
        for (const std::string name : {"connect",
                                       "disconnect",
                                       "load-history",
                                       "clear-history",
                                       "search",
                                       "quit",
                                       "add-server",
                                       "manage-networks",
                                       "mark-all-read",
                                       "mark-read",
                                       "close-buffer",
                                       "topic",
                                       "message-style:irc",
                                       "message-style:bubbles",
                                       "message-style:stacked",
                                       "mute-buffer",
                                       "unmute-buffer",
                                       "archive-buffer",
                                       "unarchive-buffer",
                                       "pin-buffer",
                                       "unpin-buffer"}) {
            if (names.contains(name) || (networkLog && name == "close-buffer"))
                continue;
            const std::string display(commandDisplayName(name));
            const int score =
                std::max(ui_logic::fuzzy(query, name), ui_logic::fuzzy(query, display));
            if (score > -100000)
                found.push_back(
                    {score, name, commandLabel(model, display, name) + "    [" + name + "]"});
        }
    } else {
        paletteSources.clear();
        for (const auto &[id, buffer] : model.buffers) {
            const auto caption = serverDisplayName(model, buffer.server);
            paletteSources.push_back({id, buffer.target, std::string(caption)});
            const auto label = buffer.kind == "server"
                                   ? std::string(caption)
                                   : buffer.target + "  ·  " + std::string(caption);
            int score = ui_logic::fuzzy(query, label);
            // Keep the existing target + routing-key search without exposing
            // technical keys in the displayed result.
            if (score <= -100000)
                score = ui_logic::fuzzy(query, buffer.target + "  ·  " + buffer.server);
            if (score > -100000)
                found.push_back({score, id, label});
        }
    }
    std::stable_sort(found.begin(), found.end(),
                     [](const Match &a, const Match &b) { return a.score > b.score; });
    matches.clear();
    for (auto &item : found)
        matches.emplace_back(std::move(item.id), std::move(item.label));
    if (!preferred.empty()) {
        const auto found = std::find_if(matches.begin(), matches.end(), [&](const auto &match) {
            return match.first == preferred;
        });
        if (found != matches.end())
            selected = std::size_t(found - matches.begin());
    }
    selected = std::min(selected, matches.empty() ? std::size_t(0) : matches.size() - 1);
    rebuildPaletteRows();
    paletteHelp->setText(commandLabel(
        model, matches.empty() ? "No matches · Cancel" : "↑ ↓ select · Enter open · Cancel",
        "cancel"));
}
void Ui::Impl::rebuildPaletteRows() {
    for (auto *row : paletteRows)
        paletteList->content()->removeChild(row);
    paletteRows.clear();
    // Only a bounded picker page is instantiated, without hiding any match from navigation.
    paletteStart = (selected / 80) * 80;
    const auto end = std::min(matches.size(), paletteStart + 80);
    for (std::size_t i = paletteStart; i < end; ++i) {
        auto row = ui::button({.text = matches[i].second,
                               .selected = i == selected,
                               .contentAlign = ButtonContentAlign::Start,
                               .variant = ButtonVariant::Ghost,
                               .onClick = [this, i] {
                                   selected = i;
                                   choose();
                               }});
        row->label()->setFontFamily(themeUiFont().family);
        row->label()->setFontFeatures(themeUiFont().features);
        row->label()->setFontWeight(themeUiFont().weight);
        row->setFontSize(14 * scale);
        std::string_view icon = "terminal";
        if (commandMode)
            icon = paletteCommandIcon(matches[i].first);
        else if (const auto buffer = model.buffers.find(matches[i].first);
                 buffer != model.buffers.end())
            icon = buffer->second.kind == "channel" ? "hash"
                   : buffer->second.kind == "query" ? "user"
                                                    : "server";
        row->setGlyph(icon);
        row->setGlyphSize(18 * scale);
        row->setControlHeight(std::max(32.f, 36 * scale));
        row->setRadius(themeCornerRadius());
        paletteRows.push_back(
            static_cast<Button *>(paletteList->content()->addChild(std::move(row))));
    }
}
void Ui::Impl::choose() {
    if (matches.empty())
        return;
    const auto value = matches[std::min(selected, matches.size() - 1)].first;
    const bool commands = commandMode;
    if (commands && value == "jump-channel") {
        startJump();
        if (!paletteOpen)
            paletteMessageTarget.reset();
    } else if (commands && (value == "message-prev" || value == "message-next" ||
                            value == "message-reply" || value == "message-react")) {
        auto target = std::move(paletteMessageTarget);
        paletteMessageTarget.reset();
        paletteOpen = false;
        palette->setVisible(false);
        if (dispatcher) {
            dispatcher->setFocus(nullptr);
            dispatcher->restoreTabFocus(std::move(paletteFocus));
        }
        if (target && (value == "message-reply" || value == "message-react"))
            run(value + ":" + target->buffer + "\n" + std::to_string(target->identity));
        else
            run(value);
    } else {
        dismissPalette();
        if (commands)
            run(value);
        else
            changeBuffer(value);
    }
    refresh();
}
void Ui::Impl::focus(std::uint64_t id, bool focusComposer) {
    if (!workspace.panes.contains(id))
        return;
    workspace.focus(id);
    model.activeBuffer = workspace.panes.at(id).buffer;
    if (const auto buffer = model.buffers.find(model.activeBuffer);
        buffer != model.buffers.end() && !buffer->second.server.empty()) {
        const auto last = lastServerBuffer.find(buffer->second.server);
        shellDirty |= selectedServer != buffer->second.server || last == lastServerBuffer.end() ||
                      last->second != buffer->first;
        selectedServer = buffer->second.server;
        lastServerBuffer[selectedServer] = buffer->first;
    }
    conversationFocus = focusComposer ? id : 0;
    for (auto &[paneId, pane] : workspace.panes)
        pane.focusComposer = focusComposer && paneId == id;
}
void Ui::Impl::changeBuffer(std::string_view id) {
    if (!model.buffers.contains(std::string(id)))
        return;
    focus(workspace.open(id));
    if (paletteOpen)
        dismissPalette();
}
void Ui::Impl::selectServer(std::string_view name) {
    const bool known = model.networks.contains(std::string(name)) ||
                       std::any_of(model.servers.begin(), model.servers.end(),
                                   [&](const auto &server) { return server.name == name; });
    if (!known)
        return;
    if (selectedServer == name)
        return;
    selectedServer = name;
    shellDirty = true;
    if (const auto last = lastServerBuffer.find(selectedServer); last != lastServerBuffer.end()) {
        const auto buffer = model.buffers.find(last->second);
        if (buffer != model.buffers.end() && buffer->second.server == selectedServer) {
            changeBuffer(buffer->first);
            return;
        }
    }
    const auto conversation =
        std::find_if(model.buffers.begin(), model.buffers.end(),
                     [&](const auto &item) { return item.second.server == selectedServer; });
    if (conversation != model.buffers.end())
        changeBuffer(conversation->first);
}
void Ui::Impl::persistShell() {
    if (!shellDirty)
        return;
    auto &state = model.settings.workspace;
    state["selected_server"] = selectedServer;
    std::erase_if(state, [&](const auto &entry) {
        return entry.first.starts_with("last_buffer:") &&
               !lastServerBuffer.contains(entry.first.substr(12));
    });
    for (const auto &[server, buffer] : lastServerBuffer)
        if (const auto found = state.find("last_buffer:" + server);
            found == state.end() || found->second != buffer)
            state["last_buffer:" + server] = buffer;
    shellDirty = false;
}
void Ui::Impl::menu(Node &anchor, std::vector<ContextMenuEntry> entries,
                    std::vector<std::string> commands,
                    std::optional<InputArea::PointerData> pointer) {
    if (!anchor.popupContext())
        return;
    float x = 0, y = 0;
    Node::absolutePosition(&anchor, x, y);
    SelectPopupContext::DropdownRequest request{};
    request.anchorX = static_cast<std::int32_t>(x);
    request.anchorY = static_cast<std::int32_t>(y);
    request.anchorWidth = static_cast<std::int32_t>(std::max(1.f, anchor.width()));
    request.anchorHeight = static_cast<std::int32_t>(std::max(1.f, anchor.height()));
    request.pointerContextMenu = pointer.has_value();
    if (pointer) {
        request.anchorX = static_cast<std::int32_t>(pointer->sceneX);
        request.anchorY = static_cast<std::int32_t>(pointer->sceneY);
        request.anchorWidth = request.anchorHeight = 1;
        request.inputSerial = pointer->serial;
    }
    request.menuWidth = 260 * scale;
    request.maxMenuWidth = 440 * scale;
    request.fontSize = 14 * scale;
    request.font = themeUiFont();
    request.maxVisibleOptions = 14;
    request.menu = std::move(entries);
    SelectPopupContext::DropdownCallbacks callbacks{};
    callbacks.onSelect = [this, commands = std::move(commands)](std::size_t value) mutable {
        if (value < commands.size())
            pending.push_back(std::move(commands[value]));
    };
    anchor.popupContext()->openSelectDropdown(request, std::move(callbacks));
}
void Ui::Impl::paneMenu(std::uint64_t id, Node &anchor, bool includeHiddenTools,
                        std::optional<InputArea::PointerData> pointer) {
    const auto pane = workspace.panes.find(id);
    if (pane == workspace.panes.end())
        return;
    std::string prefix;
    prefix.reserve(16 + pane->second.buffer.size());
    prefix.append("buffer-command:").append(pane->second.buffer).push_back('\n');
    std::vector<std::string> commands;
    commands.reserve(15);
    std::vector<ContextMenuEntry> entries;
    entries.reserve(12);
    const auto source = model.buffers.find(pane->second.buffer);
    if (source != model.buffers.end()) {
        ContextMenuEntry heading{};
        heading.label = source->second.kind == "server" ? "Server log" : source->second.target;
        const auto networkName = serverDisplayName(model, source->second.server);
        if (!networkName.empty() &&
            (source->second.kind == "server" || networkName != source->second.server))
            heading.label.append(" · ").append(networkName);
        heading.icon = source->second.kind == "channel" ? "hash"
                       : source->second.kind == "query" ? "user"
                                                        : "server";
        heading.header = true;
        entries.push_back(std::move(heading));
    }
    ContextMenuEntry navigation{};
    navigation.label = "Navigation";
    navigation.icon = "compass";
    navigation.children.reserve(3);
    navigation.children.push_back(
        menuLeaf(commands, commandLabel(model, "Switch conversation", "switch-buffer"), "messages",
                 "switch-buffer"));
    navigation.children.push_back(
        menuLeaf(commands, commandLabel(model, "Jump on selected network", "jump-channel"), "scan",
                 "jump-channel"));
    navigation.children.push_back(menuLeaf(commands, "Global mentions", "at", "global-mentions"));
    const auto nextNavigation = nextSidebarChoice(workspace.navigationMode, navigationRegularMode);
    navigation.children.push_back(
        menuLeaf(commands,
                 commandLabel(model,
                              sidebarModeToggleLabel("Channels", workspace.navigationMode,
                                                     navigationRegularMode),
                              "toggle-navigation"),
                 nextNavigation == SidebarMode::Expanded  ? "layout-sidebar-left-expand"
                 : nextNavigation == SidebarMode::Compact ? "layout-sidebar-left-collapse"
                                                          : "eye-off",
                 "toggle-navigation"));
    entries.push_back(std::move(navigation));
    if (source != model.buffers.end() && source->second.kind == "mentions") {
        menu(anchor, std::move(entries), std::move(commands), std::move(pointer));
        return;
    }
    if (source != model.buffers.end()) {
        const auto &buffer = source->second;
        if (includeHiddenTools) {
            entries.push_back(menuLeaf(commands,
                                       commandLabel(model, "Search conversation", "search"),
                                       "search", prefix + "search"));
            if (buffer.kind == "channel")
                entries.push_back(menuLeaf(
                    commands,
                    commandLabel(model,
                                 sidebarModeToggleLabel("People", model.settings.peopleMode,
                                                        peopleRegularMode),
                                 "toggle-people"),
                    "users", prefix + "toggle-people"));
        }
        if (buffer.kind == "channel" || buffer.kind == "query") {
            const auto &pins = model.settings.pinnedBuffers;
            const bool pinned = std::find(pins.begin(), pins.end(), source->first) != pins.end();
            entries.push_back(menuLeaf(commands, pinned ? "Unpin chat" : "Pin chat",
                                       pinned ? "unpin" : "pin",
                                       prefix + (pinned ? "unpin-buffer" : "pin-buffer")));
            const bool shown = effectiveFoolDisplay(model.settings, source->first, buffer.target) ==
                               FoolDisplay::Shown;
            entries.push_back(menuLeaf(
                commands, commandLabel(model, shown ? "Hide fools" : "Show fools", "toggle-fools"),
                shown ? "eye-off" : "eye", prefix + (shown ? "hide-fools" : "show-fools")));
            entries.push_back(menuLeaf(commands, buffer.muted ? "Unmute" : "Mute",
                                       buffer.muted ? "bell" : "bell-off",
                                       prefix + (buffer.muted ? "unmute-buffer" : "mute-buffer")));
            entries.push_back(
                menuLeaf(commands, buffer.archived ? "Unarchive" : "Archive",
                         buffer.archived ? "archive-off" : "archive",
                         prefix + (buffer.archived ? "unarchive-buffer" : "archive-buffer")));
            const auto layout = messageLayout(model.settings, buffer);
            ContextMenuEntry display{};
            display.label = "Message display";
            display.icon = "layout-list";
            display.children.reserve(3);
            display.children.push_back(menuRadio(commands, "Message bubbles", "messages",
                                                 prefix + "message-style:bubbles",
                                                 layout == MessageLayout::Bubbles));
            display.children.push_back(menuRadio(commands, "Traditional IRC", "list",
                                                 prefix + "message-style:irc",
                                                 layout == MessageLayout::Irc));
            display.children.push_back(menuRadio(commands, "Traditional · two-line", "layout-list",
                                                 prefix + "message-style:stacked",
                                                 layout == MessageLayout::Stacked));
            entries.push_back(std::move(display));
        }
        ContextMenuEntry history{};
        history.label = "History";
        history.icon = "history";
        history.children.reserve(2);
        history.children.push_back(
            menuLeaf(commands, "Load older messages", "history", prefix + "load-history"));
        history.children.push_back(menuLeaf(commands, "Mark read", "checks", prefix + "mark-read"));
        auto details =
            menuLeaf(commands, "Conversation details / topic", "info-circle", prefix + "topic");
        auto override = model.settings.channelPresence.find(pane->second.buffer);
        if (override == model.settings.channelPresence.end())
            override = model.settings.channelPresence.find(buffer.target);
        const bool global = override == model.settings.channelPresence.end() ||
                            override->second == PresenceMode::Global;
        ContextMenuEntry presence{};
        presence.label = "Presence";
        presence.icon = "users";
        presence.children.reserve(4);
        presence.children.push_back(
            menuRadio(commands, "Use global mode", "settings", prefix + "presence:global", global));
        presence.children.push_back(menuRadio(commands, "All", "eye", prefix + "presence:all",
                                              !global && override->second == PresenceMode::All));
        presence.children.push_back(menuRadio(commands, "Smart", "filter",
                                              prefix + "presence:smart",
                                              !global && override->second == PresenceMode::Smart));
        presence.children.push_back(menuRadio(commands, "Hidden", "eye-off",
                                              prefix + "presence:hidden",
                                              !global && override->second == PresenceMode::Hidden));
        entries.push_back(std::move(presence));
        entries.push_back(std::move(history));
        entries.push_back(std::move(details));
        if (buffer.kind != "server") {
            ContextMenuEntry separator{};
            separator.separator = true;
            entries.push_back(std::move(separator));
            entries.push_back(menuLeaf(commands, "Close buffer", "trash", prefix + "close-buffer"));
        }
    }
    menu(anchor, std::move(entries), std::move(commands), std::move(pointer));
}
void Ui::Impl::nickMenu(std::string_view buffer, std::string_view nick,
                        std::optional<InputArea::PointerData> pointer) {
    const auto source = model.buffers.find(std::string(buffer));
    if (source == model.buffers.end() || nick.empty())
        return;
    Node *anchor = views.contains(workspace.focused)
                       ? static_cast<Node *>(views.at(workspace.focused))
                       : scene.get();
    if (!pointer && dispatcher && dispatcher->focusedArea() &&
        visibleWithinNode(dispatcher->focusedArea(), scene.get()))
        anchor = dispatcher->focusedArea();
    const std::string identity(nick),
        prefix = "nick-action:" + source->first + "\n" + identity + "\n";
    std::vector<std::string> commands;
    commands.reserve(7);
    auto message = menuLeaf(commands, "Message " + identity, "message", prefix + "query");
    auto whois = menuLeaf(commands, "Whois " + identity, "id", prefix + "whois");
    ContextMenuEntry relationship{};
    relationship.label = "Relationship";
    relationship.icon = "users";
    relationship.children.reserve(2);
    auto friendEntry = menuLeaf(commands, "Friend", "star", prefix + "friend");
    friendEntry.checkmark = true;
    friendEntry.toggleState = isFriend(model, source->second.server, identity) ? 1 : 0;
    relationship.children.push_back(std::move(friendEntry));
    auto foolEntry = menuLeaf(commands, "Fool", "user-x", prefix + "fool");
    foolEntry.checkmark = true;
    foolEntry.toggleState = isFool(model, source->second.server, identity) ? 1 : 0;
    relationship.children.push_back(std::move(foolEntry));
    ContextMenuEntry appearance{};
    appearance.label = "Appearance";
    appearance.icon = "palette";
    appearance.children.reserve(2);
    appearance.children.push_back(
        menuLeaf(commands, "Nick color…", "color-picker", "nick-color:" + identity));
    appearance.children.push_back(
        menuLeaf(commands, "Use palette color", "restore", prefix + "palette"));
    auto insert =
        menuLeaf(commands, "Insert nick", "at", "insert-nick:" + source->first + "\n" + identity);
    std::vector<ContextMenuEntry> entries;
    entries.reserve(6);
    entries.push_back(std::move(message));
    entries.push_back(std::move(whois));
    entries.push_back(std::move(insert));
    ContextMenuEntry separator{};
    separator.separator = true;
    entries.push_back(std::move(separator));
    entries.push_back(std::move(relationship));
    entries.push_back(std::move(appearance));
    menu(*anchor, std::move(entries), std::move(commands), std::move(pointer));
}

BufferChromeCallbacks Ui::Impl::chromeCallbacks() {
    return {.command =
                [this](std::uint64_t id, std::string_view command) {
                    const auto pane = workspace.panes.find(id);
                    if (pane == workspace.panes.end())
                        return;
                    std::string intent;
                    intent.reserve(16 + pane->second.buffer.size() + command.size());
                    intent.append("buffer-command:").append(pane->second.buffer).push_back('\n');
                    intent.append(command);
                    pending.push_back(std::move(intent));
                },
            .paneMenu =
                [this](std::uint64_t id, Node &anchor, bool includeHiddenTools,
                       std::optional<InputArea::PointerData> pointer) {
                    paneMenu(id, anchor, includeHiddenTools, std::move(pointer));
                },
            .topic = [this](std::uint64_t id, Node &anchor,
                            std::optional<InputArea::PointerData>) { showTopic(id, anchor); }};
}

void Ui::Impl::showTopic(std::uint64_t id, Node &anchor) {
    const auto pane = workspace.panes.find(id);
    if (pane == workspace.panes.end() || paletteOpen || jumpOpen || reactionTarget ||
        dialogs->active() || media->active())
        return;
    const auto buffer = model.buffers.find(pane->second.buffer);
    if (buffer == model.buffers.end() || buffer->second.kind != "channel")
        return;
    if (dispatcher)
        if (auto *area = dispatcher->focusedArea(); area)
            if (auto *input = dynamic_cast<Input *>(area->textInputClient());
                input && input->composing())
                return;
    if (topicSource) {
        dismissTopic();
        return;
    }
    if (scene->popupContext())
        scene->popupContext()->closeSelectDropdown();
    topicFocus = dispatcher ? dispatcher->captureTabFocus() : InputDispatcher::TabFocusSnapshot{};
    if (!topicPopover) {
        topicPopover =
            static_cast<TopicPopover *>(scene->addChild(std::make_unique<TopicPopover>()));
        if (dispatcher)
            topicPopover->setInputDispatcher(*dispatcher);
    }
    focus(id, false);
    pane->second.focusComposer = false;
    topicSource = std::pair{id, pane->second.buffer};
    float left = 0, top = 0, right = 0, bottom = 0;
    Node::transformedBounds(&anchor, left, top, right, bottom);
    topicPopover->sync(width, height, scale);
    topicPopover->show(buffer->second.topic, {left, top, right - left, bottom - top},
                       [this] { dismissTopic(); });
}

void Ui::Impl::dismissTopic(bool restoreFocus) {
    if (!topicSource)
        return;
    bool ownedFocus = false;
    for (Node *node = dispatcher ? dispatcher->focusedArea() : nullptr; node; node = node->parent())
        if (node == topicPopover) {
            ownedFocus = true;
            break;
        }
    retireSubtreePointerCapture(*topicPopover);
    topicPopover->hide();
    topicSource.reset();
    if (dispatcher && restoreFocus && (ownedFocus || !dispatcher->focusedArea()))
        dispatcher->restoreTabFocus(std::move(topicFocus));
}

void Ui::Impl::run(std::string command, std::optional<InputArea::PointerData> pointer) {
    if (command.empty())
        return;
    chord.clear();
    if (command.starts_with("upload-")) {
        uploadAction(command);
        return;
    }
    if (command == "settings-uploads") {
        dialogs->open(command);
        return;
    }
    if (command.starts_with("open-server-log:")) {
        queue(model, "open-server-log", command.substr(16));
        return;
    }
    if (command.starts_with("soju-group-toggle:")) {
        const auto parent = command.substr(18);
        const auto profile =
            std::find_if(model.servers.begin(), model.servers.end(), [&](const auto &server) {
                return server.name == parent && server.sojuAccount;
            });
        if (profile != model.servers.end()) {
            auto &intent = model.settings.workspace["soju-group:" + parent];
            intent = intent == "collapsed" ? "expanded" : "collapsed";
            queue(model, "save");
        }
        return;
    }
    if (command == "global-mentions") {
        if (model.buffers.contains(GlobalMentionsBufferId))
            changeBuffer(GlobalMentionsBufferId);
        else
            queue(model, "global-mentions");
        return;
    }
    for (const auto &[route, dialog] :
         {std::pair{"soju-manage:", "manage-soju:"}, std::pair{"soju-add:", "add-soju-network:"},
          std::pair{"soju-edit:", "edit-soju-network:"},
          std::pair{"soju-delete:", "delete-soju-network:"}}) {
        if (command.starts_with(route)) {
            dialogs->open(std::string(dialog) + command.substr(std::string_view(route).size()));
            return;
        }
    }
    if (messageCommand(command))
        return;
    if ((command == "backward-char" || command == "forward-char" ||
         command == "beginning-of-line" || command == "end-of-line") &&
        !jumpOpen && !paletteOpen && !reactionTarget && !dialogs->active() &&
        transcriptOwnsFocus()) {
        if (const auto view = views.find(workspace.focused);
            view != views.end() && view->second->transcript->keyboardTextSelection()) {
            view->second->transcript->textSelectionCommand(command);
            return;
        }
    }
    if (command == "scroll-up" || command == "scroll-down") {
        if (!jumpOpen && !paletteOpen && !reactionTarget && !dialogs->active())
            if (const auto view = views.find(workspace.focused); view != views.end())
                view->second->transcript->scrollHalfPage(command == "scroll-up" ? -1 : 1);
        return;
    }
    if (command == "beginning-of-buffer" || command == "end-of-buffer") {
        if (!jumpOpen && !paletteOpen && !reactionTarget && !dialogs->active())
            if (const auto view = views.find(workspace.focused); view != views.end())
                view->second->transcript->scrollToBoundary(command == "end-of-buffer");
        return;
    }
    if (command == "copy-region") {
        if (!jumpOpen && !paletteOpen && !reactionTarget && !dialogs->active() &&
            (transcriptOwnsFocus() ||
             (dispatcher && editors.ownsFocus(dispatcher->focusedArea())))) {
            if (const auto view = views.find(workspace.focused); view != views.end()) {
                if (view->second->transcript->copySelection())
                    return;
                if (transcriptOwnsFocus()) {
                    view->second->transcript->copySelectedMessage();
                    return;
                }
            }
        }
        if (!dispatcher || !editors.ownsFocus(dispatcher->focusedArea()))
            return;
    }
    if ((command == "cycle-spelling" || command == "complete") &&
        (jumpOpen || paletteOpen || reactionTarget || dialogs->active() ||
         (dispatcher && !editors.ownsFocus(dispatcher->focusedArea()))))
        return;
    const bool insertNick = command.starts_with("insert-nick:");
    if (insertNick || command.starts_with("buffer-command:")) {
        const std::size_t prefix = insertNick ? 12 : 15;
        const auto split = command.find('\n', prefix);
        if (split == std::string::npos || split + 1 == command.size())
            return;
        const auto buffer = command.substr(prefix, split - prefix);
        if (!model.buffers.contains(buffer))
            return;
        if (!insertNick && chatControl(std::string_view(command).substr(split + 1), buffer))
            return;
        const auto current = workspace.panes.find(workspace.focused);
        const bool preservePanelFocus =
            !insertNick && std::string_view(command).substr(split + 1) == "toggle-people" &&
            current != workspace.panes.end() && current->second.buffer == buffer &&
            model.activeBuffer == buffer;
        const auto pane = preservePanelFocus ? workspace.focused : workspace.open(buffer);
        if (!preservePanelFocus)
            focus(pane);
        command.erase(0, split + 1);
        if (insertNick) {
            editors.execute("insert-nick:" + command, workspace.panes.at(pane));
            return;
        }
    }
    if (chatControl(command, model.activeBuffer))
        return;
    if (command.starts_with("nick-color "))
        command = "nick-color:" + command.substr(11);
    if (command.starts_with("buffer:")) {
        changeBuffer(command.substr(7));
        return;
    }
    if (command == "conversation-back" || command == "conversation-forward") {
        if (const auto pane = workspace.navigateHistory(command == "conversation-forward"))
            focus(*pane);
        return;
    }
    if (command.starts_with("select-server:")) {
        selectServer(std::string_view(command).substr(14));
        return;
    }
    if (command.starts_with("nick:")) {
        const auto split = command.find('\n', 5);
        if (split != std::string::npos)
            nickMenu(std::string_view(command).substr(5, split - 5),
                     std::string_view(command).substr(split + 1), std::move(pointer));
        return;
    }
    if (command.starts_with("nick-action:")) {
        const auto a = command.find('\n', 12),
                   b = a == std::string::npos ? a : command.find('\n', a + 1);
        if (a == std::string::npos || b == std::string::npos)
            return;
        const std::string buffer = command.substr(12, a - 12),
                          nick = command.substr(a + 1, b - a - 1), action = command.substr(b + 1);
        if (!model.buffers.contains(buffer))
            return;
        if (action == "query" || action == "whois")
            queue(model, action, buffer, nick);
        else if (action == "friend" || action == "fool") {
            auto &set = action == "friend" ? model.settings.friends : model.settings.fools;
            const auto &server = model.buffers.at(buffer).server;
            if (action == "friend" ? isFriend(model, server, nick) : isFool(model, server, nick)) {
                const auto network = model.networks.find(server);
                const std::string_view mapping =
                    network == model.networks.end() ? std::string_view{"rfc1459"}
                                                    : std::string_view{network->second.caseMapping};
                const auto folded = irc::casefold(nick, mapping);
                std::erase_if(set, [&](const auto &entry) {
                    return irc::casefold(entry, mapping) == folded;
                });
            } else
                set.insert(nick);
            queue(model, "save");
        } else if (action == "palette") {
            model.settings.nickColors.erase(nick);
            queue(model, "save");
        }
        return;
    }
    if (command == "persist") {
        workspace.persist();
        persistShell();
        return;
    }
    if (command == "settings" && dialogs->settingsOpen()) {
        dialogs->dismiss();
        return;
    }
    if (command == "switch-buffer" || command == "execute-command") {
        showPalette(command == "execute-command");
        return;
    }
    if (command == "jump-channel") {
        startJump();
        return;
    }
    if (command == "toggle-navigation") {
        if (widthDrag)
            retirePointerCapture();
        if (jumpOpen)
            finishJump(false);
        updateRegularPanelModes();
        workspace.navigationMode =
            nextSidebarChoice(workspace.navigationMode, navigationRegularMode);
        updateRegularPanelModes();
        queue(model, "save");
    } else if (command == "toggle-people") {
        if (widthDrag)
            retirePointerCapture();
        if (jumpOpen)
            finishJump(false);
        updateRegularPanelModes();
        model.settings.peopleMode = nextSidebarChoice(model.settings.peopleMode, peopleRegularMode);
        updateRegularPanelModes();
        queue(model, "save");
    } else if (command == "cancel") {
        model.status.clear();
        if (topicSource) {
            dismissTopic();
            return;
        }
        if (widthDrag)
            retirePointerCapture();
        if (jumpOpen) {
            finishJump(false);
            return;
        }
        if (scene->popupContext())
            scene->popupContext()->closeSelectDropdown();
        if (reactionTarget) {
            finishReactionPicker();
            return;
        }
        if (!paletteOpen && !dialogs->active() && transcriptOwnsFocus()) {
            views.at(workspace.focused)->transcript->clearTextSelection();
            focus(workspace.focused);
            editors.sync();
            return;
        }
        if (paletteOpen)
            dismissPalette();
        dialogs->dismiss();
        composerEdited(workspace.focused);
        if (auto found = workspace.panes.find(workspace.focused); found != workspace.panes.end()) {
            editors.execute("cancel", found->second);
            found->second.reply.clear();
            found->second.focusComposer = true;
        }
    } else if (command == "settings" || command == "search" || command == "add-server" ||
               command == "first-run" || command == "clear-history" || command == "config-error" ||
               command == "manage-networks" || command.starts_with("server:") ||
               command.starts_with("nick-color:")) {
        if (paletteOpen)
            dismissPalette();
        dialogs->open(command == "manage-networks" ? "networks" : command);
        for (auto &[id, pane] : workspace.panes)
            pane.focusComposer = false;
    } else if (command == "connect" || command == "disconnect") {
        if (model.servers.empty())
            dialogs->open("add-server");
        else
            queue(model, command, model.activeBuffer);
    } else if (command == "load-history")
        queue(model, "history", model.activeBuffer);
    else if (command == "mark-read")
        queue(model, "read", model.activeBuffer);
    else if (command == "mark-all-read")
        queue(model, "read-all");
    else if (command == "close-buffer")
        queue(model, "close_buffer", model.activeBuffer);
    else if (command == "quit")
        queue(model, "quit");
    else if (command == "message-style:irc" || command == "message-style:bubbles" ||
             command == "message-style:stacked") {
        const auto buffer = model.buffers.find(model.activeBuffer);
        if (buffer != model.buffers.end() &&
            (buffer->second.kind == "channel" || buffer->second.kind == "query")) {
            if (command == "message-style:bubbles")
                model.settings.messageLayouts.erase(buffer->first);
            else
                model.settings.messageLayouts[buffer->first] =
                    command == "message-style:irc" ? MessageLayout::Irc : MessageLayout::Stacked;
            queue(model, "save");
        }
    } else if (command.starts_with("presence:")) {
        const auto value = command.substr(9);
        if (value == "global") {
            const auto buffer = model.buffers.find(model.activeBuffer);
            if (buffer != model.buffers.end() &&
                model.settings.channelPresence.contains(buffer->second.target))
                model.settings.channelPresence[model.activeBuffer] = PresenceMode::Global;
            else
                model.settings.channelPresence.erase(model.activeBuffer);
        } else if (value == "all" || value == "smart" || value == "hidden")
            model.settings.channelPresence[model.activeBuffer] = value == "all" ? PresenceMode::All
                                                                 : value == "smart"
                                                                     ? PresenceMode::Smart
                                                                     : PresenceMode::Hidden;
        queue(model, "save");
    } else if (command == "topic") {
        const auto buffer = model.buffers.find(model.activeBuffer);
        if (buffer != model.buffers.end()) {
            model.status = (buffer->second.kind == "server" ? std::string("Server log")
                                                            : buffer->second.target) +
                           " · " + std::string(serverDisplayName(model, buffer->second.server)) +
                           "\n" + buffer->second.topic;
            dialogs->open("topic");
        }
    } else if (auto found = workspace.panes.find(workspace.focused);
               found != workspace.panes.end()) {
        if (!editors.execute(command, found->second))
            model.status = "Unknown command: " + command;
    }
    if (workspace.panes.contains(workspace.focused))
        model.activeBuffer = workspace.panes.at(workspace.focused).buffer;
}

void Ui::Impl::pruneViews() {
    for (auto it = views.begin(); it != views.end();) {
        if (!workspace.panes.contains(it->first)) {
            media->invalidate();
            scene->removeChild(it->second);
            it = views.erase(it);
        } else
            ++it;
    }
}
void Ui::Impl::syncViews() {
    pruneViews();
    for (auto &[id, pane] : workspace.panes)
        if (!views.contains(id)) {
            const bool focusComposer = pane.focusComposer;
            // Native focus callbacks need the view attached and registered before they run.
            pane.focusComposer = false;
            auto view = std::make_unique<PaneView>(*this, pane);
            views[id] = static_cast<PaneView *>(scene->addChild(std::move(view)));
            pane.focusComposer = focusComposer;
        }
    for (const auto &[id, view] : views)
        view->sync(conversationRect, id == workspace.focused);
}
void Ui::Impl::drainPending() {
    while (!pending.empty()) {
        auto command = std::move(pending.front());
        pending.pop_front();
        run(std::move(command));
    }
}
void Ui::Impl::refresh(bool panelResize) {
    std::string externalActive;
    if (model.activeBuffer != publishedActive)
        externalActive = model.activeBuffer;
    drainPending();
    if (widthDrag && (jumpOpen || paletteOpen || dialogs->active()))
        retirePointerCapture();
    if (!externalActive.empty() && model.buffers.contains(externalActive))
        model.activeBuffer = externalActive;
    workspace.reconcile();
    if (!externalActive.empty() && model.buffers.contains(externalActive)) {
        // Core selection can land after a newer palette interaction; keep its query and focus.
        focus(workspace.open(externalActive), !paletteOpen);
    }
    const float nextScale = ui_logic::userScale(model.settings.scale);
    if (scale != nextScale) {
        if (widthDrag)
            retirePointerCapture();
        inputGeometryPending = true;
    }
    scale = nextScale;
    place(*scene, 0, 0, width, height);
    place(*backdrop, 0, 0, width, height);
    backdrop->sync();
    // The titlebar and navigation share one material, including when transparency is enabled.
    backdrop->setVisible(true);
    // ponytail: compact windows reclaim padding, never fonts or native hit targets.
    const bool compact = height < 600 * scale || width < 1000 * scale;
    const bool tight = height < 360 * scale;
    const float outer = shellOuterPadding(width, height, scale);
    const float boundary = outer;
    const float railWidth = fittedRailWidth(workspace, width, outer, boundary, scale);
    const auto avatarBox = serverAvatarBox(railWidth, scale);
    const float logoExtent = avatarBox.width;
    const float extent = std::max(32.f, (compact ? 32.f : 34.f) * scale);
    const float gap = 8 * scale;
    const float titlePadding = (tight ? 5.f : compact ? 8.f : 10.f) * scale;
    const float titleHeight = std::max(extent, logoExtent) + 2 * titlePadding;
    const float controls = windowButtons.size() * extent + (windowButtons.size() - 1) * gap;
    const float controlsLeft = width - outer - controls;
    const float logoLeft = outer + avatarBox.x;
    const float titleLeft = logoLeft + logoExtent + gap;
    const float titleWidth = std::max(0.f, controlsLeft - gap - titleLeft);
    place(*logo, logoLeft, (titleHeight - logoExtent) / 2, logoExtent, logoExtent);
    place(*titleMove, titleLeft, (titleHeight - extent) / 2, titleWidth, extent);
    // The rounded body spans approximately y=340..1074 in the 1254px master; exclude collar,
    // fuse, spark and tail.
    constexpr float logoBodyCenter = 707.f / 1254.f;
    place(*titleRow, titleLeft, logoExtent * (logoBodyCenter - .5f), titleWidth, titleHeight);
    title->setMaxWidth(titleWidth);
    if (title->font().family != themeUiFont().family)
        title->setFontFamily(themeUiFont().family);
    title->setFontSize(16 * scale);
    if (title->font().features != themeUiFont().features)
        title->setFontFeatures(themeUiFont().features);
    title->setColor(themeForegroundColor("primary"));
    int unread = 0;
    for (const auto &[id, b] : model.buffers)
        unread += b.unread;
    if (savedUnread != unread) {
        title->setText("BlastIRC" + (unread ? " · " + std::to_string(unread) + " unread" : ""));
        savedUnread = unread;
    }
    for (std::size_t i = 0; i < windowButtons.size(); ++i) {
        place(*windowButtons[i], controlsLeft + i * (extent + gap), (titleHeight - extent) / 2,
              extent, extent);
        if (i != 0)
            windowButtons[i]->sync(scale);
    }
    const auto commandBinding = model.settings.bindings.find("execute-command");
    const std::string_view commandValue = commandBinding == model.settings.bindings.end()
                                              ? std::string_view{}
                                              : commandBinding->second;
    if (windowBindings[1] != commandValue) {
        windowBindings[1] = commandValue;
        windowButtons[1]->hint(commandLabel(model, "Execute command", "execute-command"));
    }
    const auto focused = workspace.panes.find(workspace.focused);
    const auto buffer = focused == workspace.panes.end()
                            ? model.buffers.end()
                            : model.buffers.find(focused->second.buffer);
    const bool channel = buffer != model.buffers.end() && buffer->second.kind == "channel";
    lastBuffer = focused == workspace.panes.end() ? "" : focused->second.buffer;
    const bool knownServer =
        (buffer != model.buffers.end() && buffer->second.server == selectedServer) ||
        model.networks.contains(selectedServer) ||
        std::any_of(model.servers.begin(), model.servers.end(),
                    [&](const auto &server) { return server.name == selectedServer; });
    if (!knownServer && !selectedServer.empty()) {
        selectedServer.clear();
        shellDirty = true;
    }
    if (selectedServer.empty()) {
        if (buffer != model.buffers.end()) {
            selectedServer = buffer->second.server;
            shellDirty = true;
        } else if (!model.servers.empty()) {
            selectedServer = model.servers.front().name;
            shellDirty = true;
        }
    }
    if (buffer != model.buffers.end()) {
        const auto last = lastServerBuffer.find(buffer->second.server);
        if (last == lastServerBuffer.end() || last->second != buffer->first) {
            lastServerBuffer[buffer->second.server] = buffer->first;
            shellDirty = true;
        }
    }
    shellDirty |= std::erase_if(lastServerBuffer, [&](const auto &entry) {
                      return !model.buffers.contains(entry.second);
                  }) != 0;
    const auto previousNavigation = navigationMode, previousPeople = peopleMode;
    navigationRegularMode = workspace.navigationMode;
    peopleRegularMode = channel ? model.settings.peopleMode : SidebarMode::Hidden;
    const float statusHeight = (tight ? 22.f : compact ? 26.f : 30.f) * scale;
    const float contentTop = titleHeight + outer;
    const float contentHeight = std::max(1.f, height - contentTop - statusHeight - outer);
    const float expandedWidth =
        workspace.channelSidebarWidth().value_or(navigationExpandedExtent) * scale;
    const float compactWidth = compactPanelExtent * scale;
    const float compactNavigationWidth =
        workspace.compactChannelSidebarWidth().value_or(compactPanelExtent) * scale;
    const float expandedPeopleWidth = peopleExpandedExtent * scale;
    const float chatMinimum =
        std::max(360 * scale, EditorStore::minimumComposerWidth(scale) + 2 * panePaddingX * scale);
    const float remaining = std::max(1.f, width - 2 * outer - railWidth - boundary);
    const auto regularModes =
        fitSidebarModes(navigationRegularMode, peopleRegularMode, remaining, boundary, scale,
                        chatMinimum, expandedWidth, compactNavigationWidth);
    navigationRegularMode = regularModes.first;
    peopleRegularMode = regularModes.second;
    if (jumpOpen && (selectedServer != jumpServer || dialogs->active() || paletteOpen)) {
        finishJump(false);
        completeJumpRestore();
    } else if (jumpOpen && jump->prune(model, jumpServer))
        updateJumpCandidates();
    navigationMode = jumpOpen && navigationRegularMode == SidebarMode::Hidden
                         ? SidebarMode::Compact
                         : navigationRegularMode;
    peopleMode = peopleRegularMode;
    if (widthDrag && (previousNavigation != navigationMode || previousPeople != peopleMode))
        retirePointerCapture();
    if (navigationMode != navigationRegularMode)
        peopleMode = fitSidebarModes(navigationMode, peopleRegularMode, remaining, boundary, scale,
                                     chatMinimum, expandedWidth, compactNavigationWidth)
                         .second;
    const float navigationWidth = panelWidth(navigationMode, expandedWidth, compactNavigationWidth);
    const float peopleWidth = panelWidth(peopleMode, expandedPeopleWidth, compactWidth);
    if (panelResize && (rail->width() != railWidth || sidebar->width() != navigationWidth)) {
        // Only moving effective geometry retires content; the handle keeps its capture.
        for (auto *node :
             {static_cast<Node *>(rail), static_cast<Node *>(sidebar), static_cast<Node *>(people)})
            retireSubtreePointerCapture(*node);
        for (auto &[id, view] : views)
            retireSubtreePointerCapture(*view);
        inputGeometryPending = true;
    }
    if (previousNavigation != navigationMode)
        retirePanel(*sidebar, windowButtons[0], navigationMode == SidebarMode::Hidden);
    if (previousPeople != peopleMode) {
        InputArea *replacement = nullptr;
        if (const auto view = views.find(workspace.focused); dispatcher && view != views.end())
            replacement = dispatcher->firstTabFocusUnder(view->second->titleLine);
        retirePanel(*people, replacement, peopleMode == SidebarMode::Hidden);
    }
    const WorkspaceRect railRect{outer, contentTop, railWidth, contentHeight};
    place(*rail, railRect);
    rail->sync(selectedServer, railRect.width, railRect.height, scale);
    float left = outer + railWidth + boundary;
    const WorkspaceRect navigationRect{left, contentTop, navigationWidth, contentHeight};
    place(*sidebar, navigationRect);
    sidebar->sync(selectedServer, lastBuffer, navigationRect.width, navigationRect.height, scale,
                  navigationMode);
    const auto dividerColor = withAlpha(themeColor("outline_variant"), .18f * themeSurfaceAlpha());
    const float dividerX = outer + railWidth + (boundary - 1.f) / 2;
    place(*navigationDividers[0], dividerX, contentTop, 1.f, contentHeight);
    place(*navigationDividers[1], left + navigationWidth + (boundary - 1.f) / 2, contentTop, 1.f,
          contentHeight);
    navigationDividers[1]->setVisible(navigationMode != SidebarMode::Hidden);
    for (std::size_t i = 0; i < navigationDividers.size(); ++i)
        navigationDividers[i]->setFill(
            widthHandles[i]->focused() ? themeForegroundColor("secondary") : dividerColor);
    const float handleWidth = 8 * scale;
    place(*widthHandles[0], dividerX - handleWidth / 2, contentTop, handleWidth, contentHeight);
    place(*widthHandles[1], left + navigationWidth + boundary / 2 - handleWidth / 2, contentTop,
          handleWidth, contentHeight);
    widthHandles[0]->setVisible(!jumpOpen && !paletteOpen && !dialogs->active());
    widthHandles[1]->setVisible(navigationMode != SidebarMode::Hidden && !jumpOpen &&
                                !paletteOpen && !dialogs->active());
    if (navigationMode != SidebarMode::Hidden)
        left += navigationWidth + boundary;
    const float right =
        width - outer - (peopleMode != SidebarMode::Hidden ? peopleWidth + boundary : 0);
    const float chatWidth = std::max(1.f, right - left);
    if (conversationRect.x != left || conversationRect.y != contentTop ||
        conversationRect.width != chatWidth || conversationRect.height != contentHeight)
        inputGeometryPending = true;
    conversationRect = {left, contentTop, chatWidth, contentHeight};
    const WorkspaceRect peopleRect{right + boundary, contentTop, peopleWidth, contentHeight};
    place(*people, peopleRect);
    people->sync(lastBuffer, peopleRect.width, peopleRect.height, scale, peopleMode);
    peopleMaterial->setVisible(peopleMode != SidebarMode::Hidden);
    if (peopleMode != SidebarMode::Hidden) {
        place(*peopleMaterial, peopleRect);
        peopleMaterial->sync();
    }
    const auto navigationBinding = model.settings.bindings.find("toggle-navigation");
    const std::string_view navigationValue = navigationBinding == model.settings.bindings.end()
                                                 ? std::string_view{}
                                                 : navigationBinding->second;
    if (windowBindings[0] != navigationValue ||
        navigationHintPreferred != workspace.navigationMode ||
        navigationHintEffective != navigationRegularMode) {
        windowBindings[0] = navigationValue;
        navigationHintPreferred = workspace.navigationMode;
        navigationHintEffective = navigationRegularMode;
        windowButtons[0]->hint(commandLabel(
            model,
            sidebarModeToggleLabel("Channels", workspace.navigationMode, navigationRegularMode),
            "toggle-navigation"));
    }
    windowButtons[0]->selected(navigationRegularMode != SidebarMode::Hidden);
    windowButtons[0]->sync(scale);
    dialogs->sync(width, height, scale);
    if (reactionPicker)
        reactionPicker->sync(width, height, scale);
    if (topicSource) {
        const auto pane = workspace.panes.find(topicSource->first);
        const auto buffer = model.buffers.find(topicSource->second);
        if (pane == workspace.panes.end() || pane->second.buffer != topicSource->second ||
            buffer == model.buffers.end() || buffer->second.kind != "channel" ||
            workspace.focused != topicSource->first || paletteOpen || jumpOpen || reactionTarget ||
            dialogs->active() || media->active()) {
            dismissTopic(false);
        } else {
            pane->second.focusComposer = false;
            topicPopover->sync(width, height, scale);
            if (topicPopover->topicInput().value() != buffer->second.topic)
                topicPopover->topicInput().setValue(buffer->second.topic);
        }
    }
    if (paletteOpen || jumpOpen || reactionTarget || dialogs->active())
        for (auto &[id, pane] : workspace.panes)
            pane.focusComposer = false;
    if (mediaBuffer != model.activeBuffer || mediaFools != model.settings.fools ||
        mediaFoolDisplay != model.settings.foolDisplay ||
        mediaBufferFools != model.settings.bufferFools) {
        media->invalidate();
        mediaBuffer = model.activeBuffer;
        mediaFools = model.settings.fools;
        mediaFoolDisplay = model.settings.foolDisplay;
        mediaBufferFools = model.settings.bufferFools;
    }
    syncViews();
    const auto &jumpRequest = model.pendingSourceJump;
    if (jumpRequest.revision != sourceJumpRevision && !jumpRequest.buffer.empty()) {
        const auto pane = workspace.panes.find(workspace.focused);
        if (pane != workspace.panes.end() && pane->second.buffer == jumpRequest.buffer)
            if (const auto view = views.find(pane->first); view != views.end()) {
                view->second->transcript->jumpToSourceMessage(jumpRequest.message);
                sourceJumpRevision = jumpRequest.revision;
            }
    }
    media->update(width, height, scale);
    for (const auto &[id, view] : views)
        if (!view->visible() || !view->composer->visible())
            view->pane.focusComposer = false;
    if (reactionTarget) {
        const auto source = views.find(workspace.focused);
        if (source == views.end() || source->second->pane.buffer != reactionTarget->buffer ||
            !model.buffers.contains(reactionTarget->buffer)) {
            model.status = "The selected conversation is no longer available";
            finishReactionPicker();
        }
    }
    editors.sync();
    if (uploadMenuAnchor && (uploadMenuDismissed || !uploadMenuCurrent()))
        closeUploadMenu();
    restoreJumpFocus(false);
    syncUploadPanel();
    if (windowActive)
        repairFocus();
    modeLine->sync(scale, navigationMode, chord);
    place(*modeLine, outer, height - statusHeight, std::max(0.f, width - 2 * outer), statusHeight);
    if (paletteOpen && !commandMode) {
        bool changed = paletteSources.size() != model.buffers.size();
        auto source = paletteSources.begin();
        for (auto buffer = model.buffers.begin(); !changed && buffer != model.buffers.end();
             ++buffer, ++source)
            changed = source->id != buffer->first || source->target != buffer->second.target ||
                      source->caption != serverDisplayName(model, buffer->second.server);
        if (changed) {
            const std::string preferred = selected < matches.size() ? matches[selected].first : "";
            rebuildMatches(preferred);
        }
    }
    place(*palette, 0, 0, width, height);
    place(*jump, 0, 0, width, height);
    const float paletteWidth = std::min(620 * scale, width - 20 * scale),
                paletteHeight = std::min(480 * scale, height - 30 * scale);
    place(*palettePanel, (width - paletteWidth) / 2, (height - paletteHeight) / 2, paletteWidth,
          paletteHeight);
    for (const auto &content : palettePanel->children())
        place(*content, 0, 0, paletteWidth, paletteHeight);
    palettePanel->setFill(themeSurfaceColor("surface_container_high"));
    palettePanel->setRadius(themeCornerRadius());
    paletteQuery->setFrameRadius(themeCornerRadius());
    if (paletteScale != scale || paletteFont != themeUiFont().family ||
        paletteFeatures != themeUiFont().features || paletteWeight != themeUiFont().weight) {
        paletteScale = scale;
        paletteFont = themeUiFont().family;
        paletteFeatures = themeUiFont().features;
        paletteWeight = themeUiFont().weight;
        paletteQuery->setFontSize(16 * scale);
        paletteQuery->setFontFamily(paletteFont);
        paletteQuery->setFontFeatures(paletteFeatures);
        paletteQuery->setFontWeight(paletteWeight);
        paletteTitle->setFontSize(18 * scale);
        paletteHelp->setFontSize(12 * scale);
        paletteTitle->setFontFamily(paletteFont);
        paletteHelp->setFontFamily(paletteFont);
        paletteTitle->setFontWeight(paletteWeight);
        paletteHelp->setFontWeight(paletteWeight);
        for (const auto &node : palettePanel->children())
            if (auto *column = dynamic_cast<Flex *>(node.get())) {
                column->setGap(10 * scale);
                column->setPadding(16 * scale);
            }
        paletteList->setContentScale(scale);
        for (auto *row : paletteRows) {
            row->setFontSize(14 * scale);
            if (row->label()) {
                row->label()->setFontFamily(paletteFont);
                row->label()->setFontWeight(paletteWeight);
            }
            row->setRadius(themeCornerRadius());
        }
    }
    const float edge = 5 * scale;
    const std::array<WorkspaceRect, 8> edgeRects = {{{0, 0, edge, edge},
                                                     {edge, 0, width - 2 * edge, edge},
                                                     {width - edge, 0, edge, edge},
                                                     {width - edge, edge, edge, height - 2 * edge},
                                                     {width - edge, height - edge, edge, edge},
                                                     {edge, height - edge, width - 2 * edge, edge},
                                                     {0, height - edge, edge, edge},
                                                     {0, edge, edge, height - 2 * edge}}};
    for (std::size_t i = 0; i < resizeEdges.size(); ++i) {
        place(*resizeEdges[i], edgeRects[i]);
        resizeEdges[i]->setVisible(!(window.maximized && window.maximized()));
    }
    workspace.persist();
    persistShell();
    publishedActive = model.activeBuffer;
}
bool Ui::Impl::repairFocus() {
    auto *area = dispatcher ? dispatcher->focusedArea() : nullptr;
    if (!area || visibleWithinNode(area, scene.get()))
        return false;
    // A retained control can become hidden or detached after a filter or buffer change.
    Node *target = scene.get();
    if (jumpOpen)
        target = jump;
    else if (paletteOpen)
        target = palette;
    else if (reactionTarget && !dialogs->active())
        target = reactionPicker;
    else if (dialogs->active())
        target = dialogs;
    else if (topicSource)
        target = topicPopover;
    else if (const auto view = views.find(workspace.focused);
             view != views.end() && view->second->visible()) {
        if (view->second->composer->visible())
            target = view->second->composer;
        else
            target = view->second->titleLine;
    } else
        target = sidebar;
    dispatcher->setFocus(dispatcher->firstTabFocusUnder(target));
    return true;
}
bool Ui::Impl::key(const InputArea::KeyData &event, std::uint32_t physicalKey) {
    const bool physical = physicalKey > 0 && physicalKey < consumedPhysical.size();
    const auto keyId = physical ? physicalKey | 0x80000000u : xkb_keysym_to_lower(event.sym);
    const auto isConsumed = [&] {
        return physical ? consumedPhysical[physicalKey] : consumed.contains(keyId);
    };
    const auto consume = [&] {
        if (physical)
            consumedPhysical.set(physicalKey);
        else
            consumed.insert(keyId);
        return true;
    };
    if (!event.pressed) {
        const bool mediaHandled = media->handleKey(event);
        if (jumpActivationKey == keyId)
            jumpActivationKey = 0;
        if (reactionActivationKey == keyId)
            reactionActivationKey = 0;
        repeatCommands.erase(keyId);
        const bool handled = isConsumed();
        if (physical)
            consumedPhysical.reset(physicalKey);
        else
            consumed.erase(keyId);
        return handled || mediaHandled;
    }
    if (!event.preedit && event.sym == XKB_KEY_g && event.modifiers == KeyMod::Ctrl)
        model.status.clear();
    const bool nativeLineEnd = !event.preedit && chord.empty() && bindingStroke(event) == "c-e" &&
                               nativeInputFor(dispatcher ? dispatcher->focusedArea() : nullptr);
    if (!nativeLineEnd && media->handleKey(event))
        return consume();
    const bool modifierOnly = KeySymbol::isModifier(event.sym);
    if (!modifierOnly) {
        if (!synchronizeKeyInput(event.sym))
            throw std::logic_error("Keyboard input cannot be dispatched during native layout");
        // Resolve prior native-control actions and explicit composer focus before this key.
        if (!pending.empty())
            refresh();
        else if (!paletteOpen && !dialogs->active()) {
            const auto pane = workspace.panes.find(workspace.focused);
            if (pane != workspace.panes.end() && pane->second.focusComposer)
                editors.sync();
        }
        if (!synchronizeKeyInput(event.sym))
            throw std::logic_error("Keyboard input cannot be dispatched during native layout");
        if (repairFocus())
            return consume();
    }
    auto *focusedInput = nativeInputFor(dispatcher ? dispatcher->focusedArea() : nullptr);
    if (!event.preedit && chord.empty() && bindingStroke(event) == "c-e" && focusedInput &&
        !focusedInput->composing() && !editors.composing()) {
        const bool customBinding = std::any_of(
            model.settings.bindings.begin(), model.settings.bindings.end(), [](const auto &entry) {
                if (entry.first == "message-react")
                    return false;
                const auto binding = ui_logic::canonical(entry.second);
                return binding == "c-e" || binding.starts_with("c-e ");
            });
        if (!customBinding) {
            if (isConsumed())
                return true;
            if (dispatcher && editors.ownsFocus(dispatcher->focusedArea())) {
                if (auto pane = workspace.panes.find(workspace.focused);
                    pane != workspace.panes.end())
                    editors.execute("end-of-line", pane->second);
            } else {
                // Native non-composer Inputs still need hard-line rather than visual-row end.
                auto state = focusedInput->editingState();
                const auto end = focusedInput->value().find('\n', focusedInput->cursorPosition());
                state.current.cursorPos = state.current.selectionAnchor =
                    end == std::string::npos ? focusedInput->value().size() : end;
                focusedInput->restoreEditingState(state, true);
            }
            return consume();
        }
    }
    if (nativeLineEnd && media->handleKey(event))
        return consume();
    if (topicSource) {
        if (bindingStroke(event) == "c-g") {
            dismissTopic();
            return consume();
        }
        if (topicPopover->handleKey(event))
            return consume();
        return false; // Native topic selection/copy/navigation, never global chat commands.
    }
    const auto keyStroke = bindingStroke(event);
    if (jumpOpen) {
        if (jumpActivationKey == keyId && isConsumed())
            return true;
        const bool up = KeySymbol::isUp(event.sym) || event.sym == XKB_KEY_KP_Up;
        const bool down = KeySymbol::isDown(event.sym) || event.sym == XKB_KEY_KP_Down;
        const bool scrolling =
            up || down || KeySymbol::isPageUp(event.sym) || KeySymbol::isPageDown(event.sym);
        bool cancel = event.sym == XKB_KEY_Escape || keyStroke == "c-g" || event.preedit;
        if (!cancel && isConsumed() && event.sym != XKB_KEY_BackSpace && !scrolling)
            return true;
        bool cancelPrefix = false;
        if (!cancel && !isConsumed() && !KeySymbol::isModifier(event.sym) && !keyStroke.empty() &&
            !jumpCancelBinding.empty()) {
            const auto full =
                jumpCancelChord.empty() ? keyStroke : jumpCancelChord + " " + keyStroke;
            if (full == jumpCancelBinding)
                cancel = true;
            else if (jumpCancelBinding.starts_with(full + " ")) {
                jumpCancelChord = full;
                cancelPrefix = true;
            } else if (!jumpCancelChord.empty())
                cancel = true;
        }
        if (cancel)
            finishJump(false);
        else if (!cancelPrefix) {
            if (event.sym == XKB_KEY_BackSpace) {
                if (!jumpPrefix.empty()) {
                    jumpPrefix.pop_back();
                    updateJumpCandidates();
                }
            } else if (scrolling) {
                const bool previous = up || KeySymbol::isPageUp(event.sym);
                const float step =
                    up || down ? 52 * scale : std::max(52 * scale, conversationRect.height * .8f);
                sidebar->scrollHintCandidates(previous ? -step : step);
                jump->markLayoutDirty();
            } else if (!KeySymbol::isModifier(event.sym)) {
                const char letter = ui_logic::jumpHintKey(event.utf32 ? event.utf32 : event.sym);
                if (!(event.modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super)) && letter) {
                    jumpPrefix.push_back(letter);
                    const auto hints = jump->hints();
                    const auto exact =
                        std::find_if(hints.begin(), hints.end(),
                                     [&](const auto &hint) { return hint.keys == jumpPrefix; });
                    if (exact != hints.end())
                        finishJump(true, exact->buffer);
                    else
                        updateJumpCandidates();
                } else
                    finishJump(false);
            }
        }
        refresh();
        return consume();
    }
    if (reactionPicker && reactionPicker->active() && !paletteOpen && !dialogs->active()) {
        const bool imeText = event.preedit || event.utf32 >= 0x80;
        const bool cancel = event.sym == XKB_KEY_Escape || keyStroke == "c-g";
        if (!imeText && isConsumed() && (reactionActivationKey == keyId || cancel))
            return true;
        if (reactionPicker->handleKey(event)) {
            if (cancel && reactionTarget)
                reactionActivationKey = keyId;
            return consume();
        }
        return false;
    }
    if (!paletteOpen && !dialogs->active() &&
        editors.handleCompositionKey(event.sym, event.utf32, event.modifiers, event.preedit,
                                     isConsumed()))
        return consume();
    if (event.preedit)
        return false;
    if (keyStroke.empty())
        return false;
    // Completion consumes only its focused, non-composing Input's active popup.
    if (chord.empty() && !paletteOpen && !dialogs->active() &&
        editors.handleCompletionKey(event.sym, event.modifiers))
        return consume();
    if (isConsumed() && !(paletteOpen && (event.sym == XKB_KEY_Up || event.sym == XKB_KEY_Down))) {
        if (const auto repeat = repeatCommands.find(keyId);
            repeat != repeatCommands.end() && !paletteOpen && !dialogs->active()) {
            if (repeat->second == "scroll-up" || repeat->second == "scroll-down" ||
                repeat->second == "beginning-of-buffer" || repeat->second == "end-of-buffer") {
                auto *focused = dispatcher ? dispatcher->focusedArea() : nullptr;
                auto *input = focused ? dynamic_cast<Input *>(focused->textInputClient()) : nullptr;
                if ((!editors.ownsFocus(focused) && !transcriptOwnsFocus()) ||
                    editors.composing() || (input && input->composing()))
                    return true;
            }
            if (repeat->second == "message-prev" || repeat->second == "message-next" ||
                repeat->second == "scroll-up" || repeat->second == "scroll-down" ||
                repeat->second == "beginning-of-buffer" || repeat->second == "end-of-buffer") {
                run(repeat->second);
                refresh();
                return true;
            }
            if (repeat->second.starts_with("@selection:")) {
                if (transcriptOwnsFocus())
                    if (const auto view = views.find(workspace.focused); view != views.end())
                        view->second->transcript->textSelectionCommand(
                            std::string_view(repeat->second).substr(11));
                refresh();
                return true;
            }
            if (repeat->second == "@transcript") {
                if (const auto view = views.find(workspace.focused); view != views.end())
                    view->second->transcript->handleKey(event);
            } else if (!dispatcher || editors.ownsFocus(dispatcher->focusedArea())) {
                if (auto pane = workspace.panes.find(workspace.focused);
                    pane != workspace.panes.end())
                    editors.execute(repeat->second, pane->second);
            }
            editors.sync();
        }
        return true;
    }
    auto *area = dispatcher ? dispatcher->focusedArea() : nullptr;
    auto *nativeInput = area ? dynamic_cast<Input *>(area->textInputClient()) : nullptr;
    if ((nativeInput && nativeInput->composing()) || editors.composing()) {
        if (keyStroke == "c-g" || event.sym == XKB_KEY_Escape) {
            if (editors.ownsFocus(area)) {
                if (auto p = workspace.panes.find(workspace.focused); p != workspace.panes.end())
                    editors.execute("cancel", p->second);
            } else if (nativeInput)
                nativeInput->textInputResetPreedit();
            chord.clear();
            return consume();
        }
        return false;
    }
    if (dialogs->active() && !paletteOpen) {
        if (dialogs->handleKey(event))
            return consume();
        if (!dialogs->settingsOpen())
            return false;
    }
    if (paletteOpen) {
        if (event.sym == XKB_KEY_Escape || keyStroke == "c-g") {
            dismissPalette();
            return consume();
        }
        if (event.sym == XKB_KEY_Up || event.sym == XKB_KEY_Down) {
            if (!matches.empty()) {
                if (event.sym == XKB_KEY_Up)
                    selected = selected ? selected - 1 : matches.size() - 1;
                else
                    selected = (selected + 1) % matches.size();
                if (selected < paletteStart || selected >= paletteStart + paletteRows.size())
                    rebuildPaletteRows();
                for (std::size_t i = 0; i < paletteRows.size(); ++i)
                    paletteRows[i]->setSelected(paletteStart + i == selected);
                if (selected >= paletteStart && selected - paletteStart < paletteRows.size())
                    paletteList->requestScrollIntoView(paletteRows[selected - paletteStart]);
            }
            return consume();
        }
        if (event.sym == XKB_KEY_Return || event.sym == XKB_KEY_KP_Enter) {
            choose();
            if (jumpOpen)
                jumpActivationKey = keyId;
            if (reactionTarget)
                reactionActivationKey = keyId;
            return consume();
        }
        if (event.sym == XKB_KEY_Tab) {
            if (dispatcher)
                dispatcher->cycleTabFocusInSubtree(palette, event.modifiers & KeyMod::Shift);
            return consume();
        }
        return false;
    }
    if (!dialogs->active())
        if (const auto view = views.find(workspace.focused);
            view != views.end() && view->second->transcript->handleTypingKey(event)) {
            repeatCommands[keyId] = "@transcript";
            return consume();
        }
    bool composer = !dispatcher || editors.ownsFocus(dispatcher->focusedArea());
    const std::string full = chord.empty() ? keyStroke : chord + " " + keyStroke;
    auto view = views.find(workspace.focused);
    auto *transcript = !dialogs->active() && transcriptOwnsFocus() && view != views.end()
                           ? view->second->transcript
                           : nullptr;
    const auto movement = [](std::string_view command) {
        return command == "backward-char" || command == "forward-char" ||
               command == "beginning-of-line" || command == "end-of-line";
    };
    const auto selectionCandidate = [&](std::string_view command, const std::string &binding) {
        auto candidate = ui_logic::canonical(binding);
        if (command == "end-of-line" && candidate.empty()) {
            const auto stock = ui_logic::canonical(defaultBindings().at("message-react"));
            const bool claimed =
                std::any_of(model.settings.bindings.begin(), model.settings.bindings.end(),
                            [&](const auto &entry) {
                                if (entry.first == "message-react")
                                    return false;
                                const auto other = ui_logic::canonical(entry.second);
                                return other == stock || other.starts_with(stock + " ");
                            });
            if (!claimed)
                candidate = stock;
        }
        return candidate;
    };
    const bool selectionBinding =
        transcript && transcript->keyboardTextSelection() &&
        std::any_of(model.settings.bindings.begin(), model.settings.bindings.end(),
                    [&](const auto &entry) {
                        if (!movement(entry.first))
                            return false;
                        const auto candidate = selectionCandidate(entry.first, entry.second);
                        return candidate == full || candidate.starts_with(full + " ");
                    });
    bool prefix = false;
    for (const auto &[command, binding] : model.settings.bindings) {
        const bool scrolling = command == "scroll-up" || command == "scroll-down" ||
                               command == "beginning-of-buffer" || command == "end-of-buffer";
        const auto candidate =
            transcript && transcript->keyboardTextSelection() && movement(command)
                ? selectionCandidate(command, binding)
                : ui_logic::canonical(binding);
        if (candidate.empty() ||
            (!workspaceCommand(command) && !composer &&
             !((command == "copy-region" || scrolling || command == "set-mark" ||
                (movement(command) && transcript && transcript->keyboardTextSelection())) &&
               transcript)) ||
            (command == "message-react" && (!transcriptOwnsFocus() || selectionBinding)) ||
            (scrolling && dialogs->active()) ||
            (dialogs->active() &&
             (command == "message-prev" || command == "message-next" ||
              command == "message-reply" || command == "message-react" ||
              command == "conversation-back" || command == "conversation-forward")))
            continue;
        if (candidate == full) {
            if (!synchronizeInput())
                throw std::logic_error("Commands cannot be dispatched during native layout");
            chord.clear();
            if (transcript && (command == "set-mark" ||
                               (movement(command) && transcript->keyboardTextSelection()))) {
                transcript->textSelectionCommand(command);
                if (movement(command))
                    repeatCommands[keyId] = "@selection:" + command;
                refresh();
                return consume();
            }
            if (command.starts_with("backward-") || command.starts_with("forward-") ||
                command == "previous-line" || command == "next-line" || command == "delete-char" ||
                command == "kill-word" || command == "history-previous" ||
                command == "history-next" || command == "message-prev" ||
                command == "message-next" || scrolling)
                repeatCommands[keyId] = command;
            if (composer && !workspaceCommand(command) && command != "copy-region" &&
                command != "cycle-spelling" && !scrolling)
                composerEdited(workspace.focused);
            run(command);
            if (command == "jump-channel" && jumpOpen)
                jumpActivationKey = keyId;
            if (command == "message-react" && reactionTarget)
                reactionActivationKey = keyId;
            refresh();
            return consume();
        }
        if (candidate.starts_with(full + " "))
            prefix = true;
    }
    if (prefix) {
        chord = full;
        return consume();
    }
    if (!chord.empty()) {
        model.status = "Unbound chord: " + full;
        chord.clear();
        return consume();
    }
    if (transcript && transcript->keyboardTextSelection() && chord.empty() && !event.modifiers &&
        (event.sym == XKB_KEY_Left || event.sym == XKB_KEY_Right)) {
        const std::string command = event.sym == XKB_KEY_Left ? "backward-char" : "forward-char";
        transcript->textSelectionCommand(command);
        repeatCommands[keyId] = "@selection:" + command;
        refresh();
        return consume();
    }
    if (!dialogs->active() && keyStroke == "c-c" &&
        (transcriptOwnsFocus() || (dispatcher && editors.ownsFocus(dispatcher->focusedArea()))))
        if (const auto view = views.find(workspace.focused);
            view != views.end() && view->second->transcript->copySelection())
            return consume();
    if (event.sym == XKB_KEY_Escape) {
        run("cancel");
        refresh();
        return consume();
    }
    if (const auto view = views.find(workspace.focused);
        view != views.end() && view->second->transcript->handleKey(event)) {
        repeatCommands[keyId] = "@transcript";
        return consume();
    }
    if (KeySymbol::isTab(event.sym) && (event.modifiers == 0 || event.modifiers == KeyMod::Shift) &&
        editors.focusControls(event.modifiers == KeyMod::Shift))
        return consume();
    if (!composer && dispatcher && !dialogs->active() && !media->active() &&
        uploadStage == UploadStage::Closed &&
        (!area || (!area->textInputClient() && !area->capturesAllKeys())) &&
        isPlainPrintableKey(event.utf32, event.modifiers, event.preedit) &&
        // Space still activates a focused native control rather than becoming draft text.
        (!area || event.utf32 != ' ')) {
        if (const auto view = views.find(workspace.focused);
            view != views.end() && view->second->visible() && view->second->composer->visible()) {
            const auto focusKey = "composer:" + std::to_string(workspace.focused) + ":input";
            dispatcher->restoreTabFocus(
                {.index = std::nullopt, .key = focusKey, .fallbackKey = std::nullopt});
            area = dispatcher->focusedArea();
            composer = area && area->tabFocusKey() == focusKey && editors.ownsFocus(area);
        }
    }
    // Leave the original event unhandled: native Input inserts it exactly once after focus.
    if (composer && ((event.utf32 >= 0x20 &&
                      !(event.modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super))) ||
                     KeySymbol::isLeft(event.sym) || KeySymbol::isRight(event.sym) ||
                     KeySymbol::isUp(event.sym) || KeySymbol::isDown(event.sym) ||
                     KeySymbol::isHome(event.sym) || KeySymbol::isEnd(event.sym) ||
                     KeySymbol::isPageUp(event.sym) || KeySymbol::isPageDown(event.sym) ||
                     KeySymbol::isBackspaceOrDelete(event.sym) || KeySymbol::isEnter(event.sym) ||
                     keyStroke == "c-v" || keyStroke == "c-x" || keyStroke == "c-z" ||
                     keyStroke == "s-insert" || keyStroke == "s-delete"))
        composerEdited(workspace.focused);
    return false;
}

Ui::Ui(Model &model, TextClipboard &clipboard) : impl_(std::make_unique<Impl>(model, clipboard)) {}
Ui::~Ui() = default;
Node &Ui::root() { return *impl_->scene; }
void Ui::setInputDispatcher(InputDispatcher &dispatcher) {
    impl_->dispatcher = &dispatcher;
    impl_->editors.setInputDispatcher(dispatcher);
    impl_->media->setInputDispatcher(dispatcher);
    if (impl_->reactionPicker)
        impl_->reactionPicker->setInputDispatcher(dispatcher);
    if (impl_->topicPopover)
        impl_->topicPopover->setInputDispatcher(dispatcher);
    for (const auto &[id, view] : impl_->views) {
        view->titleLine->setInputDispatcher(dispatcher);
        view->transcript->setInputDispatcher(dispatcher);
    }
    dispatcher.setFocusChangeCallback([this](InputArea *, InputArea *area) {
        if (impl_->jumpRestorePending && !impl_->restoringJumpFocus && area != impl_->jump)
            impl_->completeJumpRestore();
        if (!area)
            return;
        impl_->conversationFocus = 0;
        bool sidebarFocus = false, transcriptFocus = false;
        for (Node *node = area; node; node = node->parent()) {
            if (node == impl_->sidebar) {
                sidebarFocus = true;
                break;
            }
            if (dynamic_cast<TranscriptView *>(node)) {
                transcriptFocus = true;
                break;
            }
        }
        const bool restoringSidebar = impl_->restoringJumpFocus && sidebarFocus;
        const std::string_view focusKey = area->tabFocusKey();
        const bool canvasKeyboard =
            focusKey.starts_with("transcript:") && focusKey.ends_with(":keyboard");
        const bool suppressPointerReveal = impl_->pointerOwnsTranscriptViewport && transcriptFocus;
        if (sidebarFocus && !restoringSidebar)
            impl_->sidebar->cancelHintScrollRestore();
        bool queuedReveal = false;
        for (Node *node = area; node; node = node->parent()) {
            // The keyboard target covers the canvas, not a particular history row.
            if (!restoringSidebar && !canvasKeyboard && !suppressPointerReveal)
                if (auto *scroll = dynamic_cast<ScrollView *>(node)) {
                    scroll->requestScrollIntoView(area);
                    queuedReveal = true;
                    if (impl_->restoringJumpFocus)
                        impl_->jumpRevealQueued = true;
                }
            for (const auto &[id, view] : impl_->views) {
                if (node == view->transcript && queuedReveal)
                    view->transcript->noteFocusReveal();
                if (node == view) {
                    impl_->conversationFocus = id;
                    return;
                }
            }
        }
    });
    impl_->dialogs->setInputDispatcher(dispatcher);
}
void Ui::setWindowActive(bool active) {
    if (!active && impl_->windowActive) {
        impl_->chord.clear();
        impl_->consumed.clear();
        impl_->consumedPhysical.reset();
        impl_->repeatCommands.clear();
    }
    impl_->windowActive = active;
    for (const auto &[id, view] : impl_->views) {
        view->transcript->setWindowActive(active);
        if (!active)
            view->transcript->setReadEligible(false);
    }
}
void Ui::update(float width, float height, float nativeRenderScale) {
    width = std::max(1.f, width);
    height = std::max(1.f, height);
    if (impl_->width != width || impl_->height != height) {
        if (impl_->widthDrag)
            impl_->retirePointerCapture();
        impl_->inputGeometryPending = true;
    }
    impl_->width = width;
    impl_->height = height;
    (void)nativeRenderScale; // Output density is exclusively renderer-owned.
    impl_->pollUpload();
    impl_->refresh();
}
void Ui::applyPendingResize() {
    if (impl_->widthDragSceneX)
        impl_->applyWidthDrag(*impl_->widthDragSceneX);
}
void Ui::invalidateInputGeometry() {
    if (impl_->widthDrag)
        impl_->retirePointerCapture();
    impl_->inputGeometryPending = true;
}
bool Ui::synchronizeInput(std::uint32_t keySym) { return impl_->synchronizeKeyInput(keySym); }
bool Ui::handleKey(const InputArea::KeyData &key, std::uint32_t physicalKey) {
    return impl_->key(key, physicalKey);
}
void Ui::prepareMousePress() {
    if (impl_->touchPanel == Impl::TouchPanel::None)
        return;
    if (impl_->touchPanel == Impl::TouchPanel::Captured) {
        if (impl_->pressedPointer[1].test(BTN_LEFT))
            impl_->suppressedPointer[1].set(BTN_LEFT);
        impl_->touchPanel = Impl::TouchPanel::None;
    } else
        impl_->cancelPointerCapture();
}
bool Ui::prepareTouchPress() {
    impl_->retireHiddenPointerCapture();
    const bool captured = impl_->dispatcher && impl_->dispatcher->pointerCaptured();
    impl_->touchPanel = captured ? Impl::TouchPanel::Captured : Impl::TouchPanel::None;
    impl_->jumpTouchActive = impl_->jumpTouchDragged = false;
    impl_->pressedPointer[1].reset();
    impl_->suppressedPointer[1].reset();
    if (captured) {
        impl_->pressedPointer[1].set(BTN_LEFT);
        impl_->suppressedPointer[1].set(BTN_LEFT);
    }
    return captured;
}
bool Ui::retiredTouch() const { return impl_->suppressedPointer[1].test(BTN_LEFT); }
bool Ui::consumeTouchLeave() {
    // A pre-frame cancellation may emit Leave without ever delivering Enter/Down.
    const bool retired =
        retiredTouch() || (impl_->touchPanel == Impl::TouchPanel::None && impl_->dispatcher &&
                           impl_->dispatcher->pointerCaptured());
    impl_->suppressedPointer[1].reset();
    discardPointerRelease(BTN_LEFT, true);
    return retired;
}
void Ui::discardPointerRelease(std::uint32_t button, bool touch) {
    const std::size_t source = touch ? 1 : 0;
    if (touch) {
        impl_->touchPanel = Impl::TouchPanel::None;
        impl_->jumpTouchActive = impl_->jumpTouchDragged = false;
        impl_->pressedPointer[source].reset();
        // Native touch-up is followed by Leave; keep its retired-stream marker until then.
    } else if (button < impl_->pressedPointer[source].size()) {
        impl_->pressedPointer[source].reset(button);
        impl_->suppressedPointer[source].reset(button);
    }
}
bool Ui::handlePointerButton(std::uint32_t button, bool pressed, float x, float y, bool touch) {
    if (pressed && !touch)
        prepareMousePress();
    const std::size_t source = touch ? 1 : 0;
    if (!pressed &&
        (button >= impl_->suppressedPointer[source].size() ||
         !impl_->suppressedPointer[source][button]) &&
        !impl_->synchronizePointerInput(x, y, false, button == BTN_LEFT))
        throw std::logic_error("Pointer release cannot be dispatched during native layout");
    if (touch && button == BTN_LEFT) {
        if (!pressed)
            impl_->touchPanel = Impl::TouchPanel::None;
        else {
            if (prepareTouchPress())
                return true;
            auto *origin = impl_->dispatcher ? impl_->dispatcher->inputAreaAt(x, y) : nullptr;
            impl_->touchPanel = withinNode(origin, impl_->sidebar)  ? Impl::TouchPanel::Navigation
                                : withinNode(origin, impl_->people) ? Impl::TouchPanel::People
                                                                    : Impl::TouchPanel::Other;
        }
    }
    if (button < impl_->pressedPointer[source].size()) {
        if (pressed) {
            impl_->suppressedPointer[source].reset(button);
            impl_->pressedPointer[source].set(button);
        } else {
            impl_->pressedPointer[source].reset(button);
            if (impl_->suppressedPointer[source][button]) {
                if (!touch)
                    impl_->suppressedPointer[source].reset(button);
                return true;
            }
        }
    }
    if (pressed && impl_->dispatcher && !impl_->jumpOpen && !impl_->paletteOpen &&
        !impl_->reactionTarget && !impl_->dialogs->active()) {
        const auto view = impl_->views.find(impl_->workspace.focused);
        auto *hit = impl_->dispatcher->inputAreaAt(x, y);
        const bool windowGesture = hit == impl_->titleMove ||
                                   std::find(impl_->resizeEdges.begin(), impl_->resizeEdges.end(),
                                             hit) != impl_->resizeEdges.end();
        if (view != impl_->views.end() && !windowGesture) {
            bool insideTranscript = false;
            for (Node *node = hit; node; node = node->parent())
                if (node == view->second->transcript) {
                    insideTranscript = true;
                    break;
                }
            if (!insideTranscript)
                view->second->transcript->clearTextSelection();
        }
    }
    if (touch && button == BTN_LEFT) {
        if (pressed && impl_->jumpOpen) {
            impl_->jumpTouchActive = true;
            impl_->jumpTouchDragged = false;
            impl_->jumpTouchX = x;
            impl_->jumpTouchY = impl_->jumpTouchLastY = y;
        } else if (!pressed && impl_->jumpTouchActive) {
            const bool dragged = impl_->jumpTouchDragged;
            impl_->jumpTouchActive = impl_->jumpTouchDragged = false;
            if (dragged) {
                impl_->cancelPointerCapture();
                return true;
            }
        }
    }
    return false;
}
void Ui::handlePointerMotion(float x, float y, bool touch) {
    if (touch && retiredTouch())
        return;
    if (!impl_->synchronizePointerInput(x, y, false))
        throw std::logic_error("Pointer motion cannot be dispatched during native layout");
    if (!touch || !impl_->jumpOpen || !impl_->jumpTouchActive)
        return;
    if (!impl_->jumpTouchDragged) {
        if (std::max(std::abs(x - impl_->jumpTouchX), std::abs(y - impl_->jumpTouchY)) <
            8 * impl_->scale)
            return;
        impl_->jumpTouchDragged = true;
    }
    impl_->sidebar->scrollHintCandidates(impl_->jumpTouchLastY - y);
    impl_->jumpTouchLastY = y;
    impl_->jump->markLayoutDirty();
    impl_->refresh();
}
void Ui::beginPointerInteraction(float x, float y, bool preparePressGeometry) {
    const auto cancelTranscriptReveals = [&] {
        for (const auto &[id, view] : impl_->views)
            view->transcript->cancelFocusReveal();
    };
    cancelTranscriptReveals();
    const bool pendingReturn = impl_->jumpRestorePending;
    const bool previousIntent = impl_->pointerOwnsTranscriptViewport;
    // A keyboard-focus return must not move history underneath a newer pointer intent.
    impl_->pointerOwnsTranscriptViewport = true;
    struct RestorePointerIntent {
        bool &value;
        bool previous;
        ~RestorePointerIntent() { value = previous; }
    } restoreIntent{impl_->pointerOwnsTranscriptViewport, previousIntent};
    if (preparePressGeometry) {
        if (!impl_->synchronizePointerInput(x, y, true))
            throw std::logic_error("Pointer press cannot be dispatched during native layout");
    } else {
        impl_->retireHiddenPointerCapture();
        if (!impl_->synchronizeInput())
            throw std::logic_error("Pointer input cannot be dispatched during native layout");
        if (pendingReturn)
            impl_->retireHiddenPointerCapture();
    }
    if (pendingReturn)
        cancelTranscriptReveals();
    if (!impl_->jumpOpen && !impl_->paletteOpen && !impl_->dialogs->active() &&
        inside({impl_->sidebar->x(), impl_->sidebar->y(), impl_->sidebar->width(),
                impl_->sidebar->height()},
               x, y))
        impl_->sidebar->cancelHintScrollRestore();
}
void Ui::setWindowActions(WindowActions actions) { impl_->window = std::move(actions); }
void Ui::execute(std::string command) {
    if (impl_->scene->inLayout || impl_->inputSynchronizing ||
        currentUiPhase() == UiPhase::Layout || currentUiPhase() == UiPhase::Render) {
        impl_->pending.push_back(std::move(command));
        return;
    }
    if (!impl_->synchronizeInput()) {
        impl_->pending.push_back(std::move(command));
        return;
    }
    impl_->run(std::move(command));
    impl_->refresh();
}
void Ui::flush() {
    const std::string externalActive =
        impl_->model.activeBuffer != impl_->publishedActive ? impl_->model.activeBuffer : "";
    impl_->drainPending();
    impl_->editors.flush();
    const std::string previousBuffer = impl_->workspace.panes.at(impl_->workspace.focused).buffer;
    if (!externalActive.empty() && impl_->model.buffers.contains(externalActive))
        impl_->model.activeBuffer = externalActive;
    impl_->workspace.reconcile();
    impl_->pruneViews();
    if (const auto buffer = impl_->model.buffers.find(impl_->model.activeBuffer);
        previousBuffer != impl_->model.activeBuffer && buffer != impl_->model.buffers.end()) {
        const auto last = impl_->lastServerBuffer.find(buffer->second.server);
        impl_->shellDirty |= impl_->selectedServer != buffer->second.server ||
                             last == impl_->lastServerBuffer.end() || last->second != buffer->first;
        impl_->selectedServer = buffer->second.server;
        impl_->lastServerBuffer[impl_->selectedServer] = buffer->first;
    }
    impl_->workspace.persist();
    impl_->persistShell();
    impl_->publishedActive = impl_->model.activeBuffer;
}
} // namespace blast
