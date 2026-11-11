#pragma once

#include "ui/controls/color_swatch_preview.h"
#include "ui/controls/context_menu.h"
#include "ui/palette.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class SelectPopupContext {
public:
  virtual ~SelectPopupContext() = default;

  struct DropdownRequest {
    std::int32_t anchorX = 0;
    std::int32_t anchorY = 0;
    std::int32_t anchorWidth = 1;
    std::int32_t anchorHeight = 1;
    float menuWidth = 0.0F;
    // When > menuWidth, the dropdown grows past menuWidth to fit its widest option, up to this cap.
    float maxMenuWidth = 0.0F;
    // Drives the dropdown's row metrics (font, height, insets); pre-scaled by the caller.
    float fontSize = 0.0F;
    // Popup surfaces cannot inherit the trigger's font selection.
    TextFont font;
    std::vector<std::string> options;
    // Nonempty menu selects by leaf value, not row position. Branches have children and no action.
    std::vector<ContextMenuEntry> menu;
    std::vector<std::string> optionIcons;
    std::vector<ColorSpec> indicatorColors;
    std::vector<ColorSwatchPreview> optionSwatchPreviews;
    // Trusted caller-supplied Pango markup; empty entries keep the plain option text.
    std::vector<std::string> optionMarkupPreviews;
    std::size_t selectedIndex = static_cast<std::size_t>(-1);
    std::size_t maxVisibleOptions = 6;
    // The initiating press serial, rather than the later release that opens the menu.
    std::optional<std::uint32_t> inputSerial = std::nullopt;
    // Pointer menus attach at anchorX/Y, including when the compositor flips their direction.
    bool pointerContextMenu = false;
  };

  struct DropdownCallbacks {
    std::function<void(std::size_t index)> onSelect;
    std::function<void()> onDismiss;
  };

  virtual void openSelectDropdown(const DropdownRequest& request, DropdownCallbacks callbacks) = 0;
  virtual void closeSelectDropdown() = 0;
  virtual bool isSelectDropdownOpen() const = 0;
};
