#pragma once

#include <cstdint>
#include <string>

namespace blast {

// Conversation identity is shared; reply, focus, and scroll intent belong to the pane.
struct PaneState {
    std::uint64_t id = 1;
    std::string buffer;
    std::string reply;
    bool focusComposer = true;
    bool following = true;
    int unseen = 0;
};

} // namespace blast
