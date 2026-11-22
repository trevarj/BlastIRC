#pragma once
#include "irc/protocol.hpp"
#include "model.hpp"
#include <limits>

namespace blast::application_detail {
inline string tag(const Event &e, const string &name) {
    auto i = e.tags.find(name);
    return i == e.tags.end() ? string{} : i->second;
}
inline bool flag(const Event &e, const string &name) {
    auto value = tag(e, name);
    return value == "1" || value == "true";
}
inline const string &historyOwner(const Event &e) {
    const auto owner = e.tags.find("history-owner");
    return owner == e.tags.end() ? e.label : owner->second;
}
inline bool historyMutation(const Event &e) { return e.type == "reaction" || e.type == "redact"; }
inline string upper(string value) {
    for (char &c : value)
        if (c >= 'a' && c <= 'z')
            c -= 32;
    return value;
}
inline int64_t markerTime(const string &value) {
    const auto parsed = irc::parseTimestampParts(value);
    return parsed ? parsed->seconds : 0;
}
inline int markerFraction(const string &value) {
    const auto parsed = irc::parseTimestampParts(value);
    return parsed ? parsed->nanoseconds : 0;
}
inline int64_t historyRank(const Message &message) {
    return message.historyOrder < 0 ? std::numeric_limits<int64_t>::max() : message.historyOrder;
}
inline bool historyTieLess(const Message &a, const Message &b) {
    if (historyRank(a) != historyRank(b))
        return historyRank(a) < historyRank(b);
    const auto aSequence =
        a.storageSequence > 0 ? a.storageSequence : std::numeric_limits<int64_t>::max();
    const auto bSequence =
        b.storageSequence > 0 ? b.storageSequence : std::numeric_limits<int64_t>::max();
    return aSequence < bSequence;
}
inline int64_t orderingTime(const Message &message) {
    return message.orderingTimestamp > 0 ? message.orderingTimestamp : message.timestamp;
}
} // namespace blast::application_detail
