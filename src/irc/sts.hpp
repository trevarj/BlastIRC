#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace blast::irc {

struct StsPolicy {
    int port;
    int64_t expires;
};

// Hosts are ASCII-case-insensitive. Invalid input throws invalid_argument;
// storage/clock failures throw runtime_error, never an absent-policy fallback.
// A missing file or an expired policy returns nullopt without modifying storage.
std::optional<StsPolicy> loadSts(std::string_view host);

// The caller must have verified TLS before accepting the advertised policy.
// A zero duration removes the host; other durations are seconds from now.
void saveSts(std::string_view host, int port, int64_t durationSeconds);

} // namespace blast::irc
