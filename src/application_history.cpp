#include "application.hpp"
#include "application_detail.hpp"
#include "irc/protocol.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace blast {
using namespace application_detail;
namespace {
bool historyPrimary(const Event &e) {
    return !e.tags.contains("draft/chathistory-context") &&
           (e.type == "message" || e.type == "notice" || e.type == "action");
}
const string &historyProtocolId(const Event &e) {
    const auto wire = e.tags.find("msgid");
    if (wire != e.tags.end())
        return wire->second;
    static const string missing;
    return historyMutation(e) ? missing : e.id;
}
int64_t historyMillis(const string &time) {
    const auto parsed = irc::parseTimestampParts(time);
    return parsed ? parsed->seconds * 1000 + parsed->nanoseconds / 1000000 : 0;
}
string historyTime(int64_t millis) {
    using namespace std::chrono;
    millis = std::max<int64_t>(0, millis);
    const sys_time<milliseconds> point{milliseconds{millis}};
    const auto day = floor<days>(point);
    const year_month_day date{day};
    const hh_mm_ss clock{point - day};
    auto pad = [](int value, size_t width) {
        auto text = std::to_string(value);
        return string(width > text.size() ? width - text.size() : 0, '0') + text;
    };
    return pad(int(date.year()), 4) + "-" + pad(unsigned(date.month()), 2) + "-" +
           pad(unsigned(date.day()), 2) + "T" + pad(clock.hours().count(), 2) + ":" +
           pad(clock.minutes().count(), 2) + ":" + pad(clock.seconds().count(), 2) + "." +
           pad(clock.subseconds().count(), 3) + "Z";
}
bool reorderHistoryTies(Buffer &buffer, const vector<size_t> &positions) {
    bool changed = false;
    vector<size_t> order;
    for (size_t first = 0; first < positions.size();) {
        const auto timestamp = orderingTime(buffer.messages[positions[first]]);
        const auto fraction = markerFraction(buffer.messages[positions[first]].time);
        size_t last = first + 1;
        while (last < positions.size() &&
               orderingTime(buffer.messages[positions[last]]) == timestamp &&
               markerFraction(buffer.messages[positions[last]].time) == fraction)
            ++last;
        auto begin = positions.begin() + first;
        auto end = positions.begin() + last;
        if (!std::is_sorted(begin, end, [&](size_t a, size_t b) {
                return historyTieLess(buffer.messages[a], buffer.messages[b]);
            })) {
            order.resize(last - first);
            for (size_t i = 0; i < order.size(); ++i)
                order[i] = i;
            std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                const auto &left = buffer.messages[positions[first + a]];
                const auto &right = buffer.messages[positions[first + b]];
                if (historyTieLess(left, right))
                    return true;
                if (historyTieLess(right, left))
                    return false;
                return a < b;
            });
            // Apply the index permutation with one moved row, not a transcript-sized row copy.
            for (size_t i = 0; i < order.size(); ++i) {
                if (order[i] == i)
                    continue;
                Message held = std::move(buffer.messages[positions[first + i]]);
                size_t destination = i;
                while (order[destination] != i) {
                    const auto source = order[destination];
                    buffer.messages[positions[first + destination]] =
                        std::move(buffer.messages[positions[first + source]]);
                    order[destination] = destination;
                    destination = source;
                }
                buffer.messages[positions[first + destination]] = std::move(held);
                order[destination] = destination;
            }
            changed = true;
        }
        first = last;
    }
    return changed;
}
} // namespace

bool Application::historyEvent(const Event &event) {
    if (flag(event, "search") || tag(event, "direction") == "search")
        return false;
    const bool history = event.history || event.type == "history_begin" ||
                         event.type == "history_end" || event.type == "history_target";
    if (!history)
        return false;
    if (flag(event, "history-standalone") && !event.tags.contains("history-owner"))
        return false;
    const auto &owner = historyOwner(event);
    auto found = historyPages_.find(owner);
    if (found == historyPages_.end() && event.type == "history_begin" &&
        flag(event, "unsolicited") && !owner.empty()) {
        const auto network = model.networks.find(event.server);
        const auto room =
            std::find_if(model.buffers.begin(), model.buffers.end(), [&](const auto &entry) {
                return entry.second.server == event.server &&
                       fold(event.server, entry.second.target) == fold(event.server, event.target);
            });
        if (network == model.networks.end() || network->second.state != "Ready" ||
            !network->second.caps.contains("batch") || room == model.buffers.end() ||
            (room->second.kind != "channel" && room->second.kind != "query") ||
            storage_.historyDiscarded(room->first))
            return true;
        HistoryPage page;
        page.server = event.server;
        page.buffer = room->first;
        page.session = network->second.sessionGeneration;
        page.unsolicited = true;
        page.request.target = room->second.target;
        page.request.requestId = owner;
        page.started = model.now;
        const auto limit = tag(event, "limit");
        if (!limit.empty()) {
            int value = 0;
            const auto parsed = std::from_chars(limit.data(), limit.data() + limit.size(), value);
            if (parsed.ec == std::errc{} && value > 0)
                page.request.limit = value;
        }
        found = historyPages_.emplace(owner, std::move(page)).first;
        activeHistory_[owner] = room->first;
    }
    if (found == historyPages_.end())
        return !owner.empty();
    auto &page = found->second;
    auto network = model.networks.find(page.server);
    if (network == model.networks.end() || network->second.sessionGeneration != page.session) {
        historyPages_.erase(found);
        return true;
    }
    if (event.server != page.server)
        return true;
    if (event.type == "history_begin") {
        if (page.seed)
            return true;
        if (auto *b = findBuffer(page.buffer)) {
            if (page.request.initial)
                b->initialHistorySync = HistorySyncState::Loading;
            else
                b->loading = true;
        }
        return true;
    }
    if (event.type != "history_end") {
        size_t bytes = sizeof(Event) + event.text.size() + event.id.size() + event.time.size() +
                       event.nick.size() + event.target.size();
        for (const auto &[key, value] : event.tags)
            bytes += key.size() + value.size();
        for (const auto &value : event.params)
            bytes += value.size();
        size_t total = bytes;
        for (const auto &[label, pending] : historyPages_)
            total += pending.bytes;
        if (page.events.size() >= 65536 || total > 32 * 1024 * 1024) {
            Event failed = event;
            failed.type = "history_end";
            failed.label = owner;
            failed.tags["error"] = "History page exceeded receive limits";
            auto retired = std::move(page);
            historyPages_.erase(found);
            const auto server = retired.server;
            try {
                finishHistoryPage(std::move(retired), failed, false, false);
            } catch (const std::exception &error) {
                cancelHistorySession(server);
                report("History: " + string(error.what()), server);
            }
            return true;
        }
        page.bytes += bytes;
        page.events.push_back(event);
        return true;
    }
    auto completed = std::move(page);
    historyPages_.erase(found);
    Event end = event;
    end.label = owner;
    if (tag(end, "count") != "0" &&
        ((tag(end, "oldest-id").empty() && tag(end, "oldest-time").empty()) ||
         (tag(end, "newest-id").empty() && tag(end, "newest-time").empty())))
        end.tags["incomplete"] = "1";
    bool applied = tag(end, "error").empty() && !flag(end, "cancelled");
    bool progressed = false;
    if (applied && completed.request.direction != HistoryDirection::Targets) {
        try {
            progressed = applyHistoryPage(completed, end);
        } catch (const std::exception &error) {
            applied = false;
            end.tags["error"] = error.what();
        }
    }
    if (completed.request.direction == HistoryDirection::Around) {
        applyingHistory_ = true;
        reduce(end);
        applyingHistory_ = false;
    }
    const auto server = completed.server;
    try {
        finishHistoryPage(std::move(completed), end, applied, progressed);
    } catch (const std::exception &error) {
        cancelHistorySession(server);
        report("History: " + string(error.what()), server);
    }
    return true;
}

bool Application::applyHistoryPage(HistoryPage &page, const Event &end) {
    auto *b = findBuffer(page.buffer);
    if (!b)
        throw std::runtime_error("History conversation closed");
    if (!savingAllowed_)
        throw std::runtime_error("History cannot be committed while storage is unavailable");
    auto write = storage_.historyWrite();
    storage_.loadHistoryState(*b);
    auto before = b->historyState;
    const HistoryCursor wireOldest{tag(end, "oldest-id"), tag(end, "oldest-time")};
    HistoryCursor oldest, newest;
    for (const auto &event : page.events)
        if (historyPrimary(event)) {
            HistoryCursor cursor{historyProtocolId(event), event.time};
            if (oldest.id.empty() && oldest.time.empty())
                oldest = cursor;
            newest = std::move(cursor);
        }
    auto usable = [](const HistoryCursor &cursor) {
        return !cursor.id.empty() || irc::parseTimestampParts(cursor.time).has_value();
    };
    auto timeLess = [](const HistoryCursor &a, const HistoryCursor &z) {
        const auto x = irc::parseTimestampParts(a.time), y = irc::parseTimestampParts(z.time);
        return x && y &&
               (x->seconds < y->seconds ||
                (x->seconds == y->seconds && x->nanoseconds < y->nanoseconds));
    };
    auto identical = [](const HistoryCursor &a, const HistoryCursor &z) {
        return !a.id.empty() && a.id == z.id;
    };
    if (tag(end, "ref-type") == "timestamp" && !page.request.first.id.empty())
        historyReferences_[b->server] = "timestamp";
    const bool timestampRequest =
        tag(end, "ref-type") == "timestamp" || zncPlayback(b->server) ||
        (tag(end, "ref-type").empty() &&
         !historyUsesMsgid(b->server, page.request.direction == HistoryDirection::Latest
                                          ? wireOldest.id
                                          : page.request.first.id));
    auto ownsNewer = [&](const HistoryGap &gap) {
        const bool unchanged = usable(page.gapBoundary) ? page.gapBoundary == gap.newer
                                                        : identical(page.request.first, gap.newer);
        return page.request.direction == HistoryDirection::Before && page.gapId == gap.id &&
               unchanged;
    };
    // Prefer persisted content continuity, not the newest live cache island.
    auto previousNewest = before.newest;
    std::optional<HistoryCursor> durableNewest;
    if (!usable(previousNewest) || previousNewest.id.empty())
        durableNewest = storage_.latestHistoryBoundary(b->id);
    if (!usable(previousNewest) && durableNewest)
        previousNewest = *durableNewest;
    string previousAnchor = previousNewest.id;
    if (previousAnchor.empty() && durableNewest && irc::parseTimestampParts(previousNewest.time) &&
        irc::parseTimestampParts(durableNewest->time) &&
        !timeLess(previousNewest, *durableNewest) && !timeLess(*durableNewest, previousNewest))
        previousAnchor = durableNewest->id;
    size_t count = 0;
    const auto countText = tag(end, "count");
    const auto parsed =
        std::from_chars(countText.data(), countText.data() + countText.size(), count);
    if (countText.empty() || parsed.ec != std::errc{})
        count = page.events.size();
    const auto limitText = tag(end, "limit");
    size_t limit = page.request.limit;
    if (!limitText.empty())
        std::from_chars(limitText.data(), limitText.data() + limitText.size(), limit);
    const bool timestampFull = timestampRequest && count >= limit && count != 0;
    const auto overlapTime = historyTime(historyMillis(wireOldest.time) + 1);
    const bool bounded = usable(oldest) && usable(newest);
    set<string> ids;
    for (const auto &event : page.events) {
        const auto &id = historyProtocolId(event);
        if (historyPrimary(event) && !id.empty() &&
            (!timestampFull || historyMillis(event.time) > historyMillis(wireOldest.time)))
            ids.insert(id);
    }
    const auto savedState = b->historyState;
    const auto savedSize = b->messages.size();
    const auto savedRead = b->readMarker;
    const auto savedTyping = b->typing;
    const auto savedJump = model.pendingSourceJump;
    const ChatActivityKey savedActivity{b->lastChatTimestamp, b->lastChatSequence, b->lastChatTime};
    const auto savedChatId = b->lastChatId;
    const auto replayMaximum =
        b->chatReplaySequences.empty() ? 0 : *b->chatReplaySequences.rbegin();
    map<int64_t, bool> replayBefore;
    map<string, std::optional<ChatActivityKey>> keysBefore;
    set<string> touched;
    for (const auto &event : page.events) {
        touched.insert(event.id);
        touched.insert(event.reply);
    }
    for (const auto &row : b->messages)
        if (std::any_of(page.events.begin(), page.events.end(), [&](const auto &event) {
                return (!row.label.empty() && row.label == event.label) ||
                       ((row.id.starts_with("local:") || row.id.starts_with("local-")) &&
                        row.time == event.time &&
                        fold(b->server, row.nick) == fold(b->server, event.nick) &&
                        row.text == event.text && row.action == (event.type == "action"));
            }))
            touched.insert(row.id);
    for (const auto &row : b->messages)
        if (touched.contains(row.id)) {
            replayBefore[row.storageSequence] =
                b->chatReplaySequences.contains(row.storageSequence);
            const auto key = b->replayChatKeys.find(row.id);
            keysBefore[row.id] =
                key == b->replayChatKeys.end() ? std::nullopt : std::optional{key->second};
        }
    // Keep membership/order as compact stable identities; only touched rows need an undo copy.
    vector<std::pair<int64_t, int64_t>> originalOrder;
    originalOrder.reserve(b->messages.size());
    map<string, int64_t> localIdentities;
    map<int64_t, Message> originalRows;
    int64_t localIdentity = 0;
    for (const auto &row : b->messages) {
        const auto identity = row.storageSequence > 0 ? row.storageSequence : --localIdentity;
        originalOrder.emplace_back(identity, row.historyOrder);
        if (row.storageSequence == 0)
            localIdentities.emplace(row.id, identity);
        if (touched.contains(row.id))
            originalRows.emplace(identity, row);
    }
    map<string, Pending> savedPending;
    for (const auto &[key, pending] : pending_)
        if (pending.buffer == b->id)
            savedPending.emplace(key, pending);
    map<string, QueuedSend> savedQueued;
    for (const auto &[key, queued] : queuedSends_)
        if (queued.buffer == b->id)
            savedQueued.emplace(key, queued);
    const auto savedCount = messageCount_;
    const auto savedRevision = messageRevision_;
    const auto savedMentions = mentionsLiveRevision_;
    const auto savedSequence = sequence_;
    bool committed = false;
    string oldestAnchor, newestAnchor;
    bool firstPrimary = true;
    vector<string> playbackOrder;
    set<string> existingCanonicalIds;
    playbackOrder.reserve(page.events.size());
    try {
        applyingHistory_ = true;
        for (auto &event : page.events) {
            if (!historyPrimary(event) && !historyMutation(event) && event.type != "message" &&
                event.type != "notice" && event.type != "action")
                continue;
            event.target = b->target;
            historyCanonicalId_.clear();
            historyCanonicalExisting_ = false;
            reduce(event);
            if (historyMutation(event))
                historyCanonicalId_.clear();
            if (!historyCanonicalId_.empty()) {
                playbackOrder.push_back(historyCanonicalId_);
                if (historyCanonicalExisting_)
                    existingCanonicalIds.insert(historyCanonicalId_);
            }
            if (historyPrimary(event)) {
                const auto &protocolId = historyProtocolId(event);
                const auto &canonicalId = historyCanonicalId_;
                if (!canonicalId.empty())
                    ids.insert(canonicalId);
                if (firstPrimary) {
                    oldestAnchor = protocolId.empty() ? canonicalId : protocolId;
                    firstPrimary = false;
                }
                newestAnchor = protocolId.empty() ? canonicalId : protocolId;
            }
        }
        applyingHistory_ = false;
        historyCanonicalId_.clear();
        historyCanonicalExisting_ = false;
        vector<string> existingCanonicalOrder;
        if (!existingCanonicalIds.empty()) {
            set<std::pair<int64_t, int>> affectedTimes;
            for (const auto &row : b->messages)
                if (existingCanonicalIds.contains(row.id))
                    if (const auto time = irc::parseTimestampParts(row.time))
                        affectedTimes.emplace(orderingTime(row), time->nanoseconds);
            vector<int64_t> originalSequences;
            originalSequences.reserve(originalOrder.size());
            for (const auto &[identity, rank] : originalOrder)
                if (identity > 0)
                    originalSequences.push_back(identity);
            std::sort(originalSequences.begin(), originalSequences.end());
            for (const auto &row : b->messages) {
                const auto time = irc::parseTimestampParts(row.time);
                if (!time || !affectedTimes.contains({orderingTime(row), time->nanoseconds}))
                    continue;
                if (existingCanonicalIds.contains(row.id) ||
                    std::binary_search(originalSequences.begin(), originalSequences.end(),
                                       row.storageSequence))
                    existingCanonicalOrder.push_back(row.id);
            }
        }
        const auto ranks =
            write.reconcileOrder(b->id, playbackOrder,
                                 page.request.direction == HistoryDirection::Before ||
                                     page.request.direction == HistoryDirection::Around,
                                 existingCanonicalOrder);
        vector<size_t> playbackPositions;
        playbackPositions.reserve(std::min(b->messages.size(), ranks.size()));
        for (size_t position = 0; position < b->messages.size(); ++position) {
            auto &row = b->messages[position];
            const auto rank = std::lower_bound(
                ranks.begin(), ranks.end(), row.storageSequence,
                [](const auto &entry, int64_t sequence) { return entry.first < sequence; });
            if (rank != ranks.end() && rank->first == row.storageSequence) {
                row.historyOrder = rank->second;
                playbackPositions.push_back(position);
            }
        }
        if (reorderHistoryTies(*b, playbackPositions))
            reviseMessages(*b);
        auto &state = b->historyState;
        state.oldest = before.oldest;
        state.newest = before.newest;
        if (count && bounded) {
            HistoryCursor gapOlder = previousNewest;
            if (gapOlder.id.empty())
                gapOlder.id = previousAnchor;
            HistoryCursor gapNewer = oldest;
            if (gapNewer.id.empty())
                gapNewer.id = oldestAnchor;
            if (timestampFull && irc::parseTimestampParts(wireOldest.time))
                gapNewer = {{}, overlapTime};
            HistoryCursor newestBoundary = newest;
            if (newestBoundary.id.empty())
                newestBoundary.id = newestAnchor;
            if (page.gapId && std::any_of(before.gaps.begin(), before.gaps.end(), ownsNewer) &&
                timeLess(newestBoundary, page.gapBoundary))
                newestBoundary = page.gapBoundary;
            const bool includesPrevious =
                (!previousNewest.id.empty() && ids.contains(previousNewest.id)) ||
                (!previousAnchor.empty() && ids.contains(previousAnchor));
            const bool orderedTimes = irc::parseTimestampParts(previousNewest.time) &&
                                      irc::parseTimestampParts(oldest.time) &&
                                      irc::parseTimestampParts(newest.time) &&
                                      !timeLess(newest, oldest);
            if ((page.request.direction == HistoryDirection::Latest || page.latestSeam) &&
                !flag(end, "end") && orderedTimes && !includesPrevious &&
                !timeLess(oldest, previousNewest) && !identical(gapOlder, gapNewer)) {
                const bool already =
                    std::any_of(state.gaps.begin(), state.gaps.end(), [&](const auto &gap) {
                        const bool coversOlder =
                            timeLess(gap.older, gapOlder) || identical(gap.older, gapOlder);
                        const bool coversNewer =
                            timeLess(gapNewer, gap.newer) || identical(gapNewer, gap.newer);
                        return coversOlder && coversNewer;
                    });
                if (!already)
                    state.gaps.push_back({0, gapOlder, gapNewer, false});
            }
            vector<blast::HistoryGap> remaining;
            for (auto gap : state.gaps) {
                const bool reachesOlder =
                    ids.contains(gap.older.id) || timeLess(gapNewer, gap.older);
                const bool reachesNewer = ids.contains(gap.newer.id) ||
                                          timeLess(gap.newer, newestBoundary) || ownsNewer(gap);
                const bool insideOlder = timeLess(gap.older, gapNewer);
                const bool insideNewer = timeLess(newestBoundary, gap.newer);
                const bool joinsNewer = ids.contains(gap.newer.id) || ownsNewer(gap);
                const bool tiedNewer = irc::parseTimestampParts(gapNewer.time) &&
                                       irc::parseTimestampParts(gap.newer.time) &&
                                       !timeLess(gapNewer, gap.newer) &&
                                       !timeLess(gap.newer, gapNewer);
                if (reachesOlder && reachesNewer)
                    continue;
                if (reachesNewer && !reachesOlder && !identical(gapNewer, gap.newer) &&
                    !timeLess(gapNewer, gap.older) &&
                    (timeLess(gapNewer, gap.newer) || (joinsNewer && tiedNewer))) {
                    gap.newer = gapNewer;
                    gap.unavailable = false;
                } else if (reachesOlder && insideNewer && timeLess(gap.older, newestBoundary)) {
                    gap.older = newestBoundary;
                    gap.unavailable = false;
                } else if (insideOlder && insideNewer && timeLess(gapNewer, newestBoundary)) {
                    auto upper = gap;
                    upper.id = 0;
                    upper.older = newestBoundary;
                    gap.newer = gapNewer;
                    remaining.push_back(std::move(upper));
                }
                remaining.push_back(std::move(gap));
            }
            state.gaps = std::move(remaining);
            if (!usable(state.oldest) || timeLess(oldest, state.oldest) ||
                (page.request.direction == HistoryDirection::Before &&
                 !timeLess(state.oldest, oldest)))
                state.oldest = oldest;
            if (!usable(state.newest) || timeLess(state.newest, newest) ||
                (page.request.direction == HistoryDirection::Latest &&
                 !timeLess(newest, state.newest)))
                state.newest = newest;
            if (!timestampFull && flag(end, "end") &&
                page.request.direction == HistoryDirection::Latest &&
                page.request.first.id.empty() && page.request.first.time.empty())
                state.beginning = true;
        }
        if (!timestampFull && page.request.direction == HistoryDirection::Before &&
            (flag(end, "end") || !count)) {
            if (page.gapId) {
                for (auto &gap : state.gaps)
                    if (gap.id == page.gapId)
                        gap.unavailable = true;
            } else
                state.beginning = true;
        }
        storage_.saveHistoryState(*b);
        write.commit();
        committed = true;
        storage_.loadHistoryState(*b);
        for (const auto &id : storage_.takeHistoryInvalidations())
            if (id != b->id)
                if (auto *affected = findBuffer(id))
                    storage_.loadHistoryState(*affected);
        if (auto source = unsavedSources_.find(b->id); source != unsavedSources_.end())
            std::erase_if(source->second.messages,
                          [&](const auto &row) { return touched.contains(row.id); });
        if (b->readMarker != savedRead && markerTime(b->readMarker) > 0)
            try {
                if (auto c = client(b->server))
                    c->markRead(b->target, b->readMarker);
            } catch (const std::exception &error) {
                report(error.what(), b->server);
            }
        if (!usable(wireOldest))
            return false;
        if (page.gapId) {
            const auto prior = std::find_if(before.gaps.begin(), before.gaps.end(),
                                            [&](const auto &gap) { return gap.id == page.gapId; });
            const auto current =
                std::find_if(state.gaps.begin(), state.gaps.end(),
                             [&](const auto &gap) { return gap.id == page.gapId; });
            if (prior == before.gaps.end() || current == state.gaps.end() || !ownsNewer(*prior) ||
                current->unavailable)
                return false;
        } else if (!page.latestSeam && page.request.direction != HistoryDirection::Before)
            return false;
        const bool nextMsgid = historyUsesMsgid(b->server, wireOldest.id);
        if (nextMsgid && !timestampRequest)
            return wireOldest.id != page.request.first.id &&
                   !timeLess(page.request.first, wireOldest);
        const auto previousTime = irc::parseTimestampParts(page.request.first.time);
        const auto nextTime = irc::parseTimestampParts(wireOldest.time);
        return previousTime && nextTime &&
               (nextTime->seconds < previousTime->seconds ||
                (nextTime->seconds == previousTime->seconds &&
                 nextTime->nanoseconds / 1000000 < previousTime->nanoseconds / 1000000));
    } catch (...) {
        applyingHistory_ = false;
        historyCanonicalId_.clear();
        historyCanonicalExisting_ = false;
        if (committed) {
            for (auto &[id, value] : model.buffers)
                value.historyState = {};
            throw;
        }
        model.pendingSourceJump = savedJump;
        // Restore exact loaded islands, not the database's newest window.
        b->historyState = savedState;
        b->readMarker = savedRead;
        b->typing = savedTyping;
        map<int64_t, Message> retained;
        for (auto &row : b->messages) {
            auto identity = row.storageSequence;
            if (identity == 0) {
                const auto local = localIdentities.find(row.id);
                if (local == localIdentities.end())
                    continue;
                identity = local->second;
            }
            retained.emplace(identity, std::move(row));
        }
        for (auto &[identity, row] : originalRows)
            retained.insert_or_assign(identity, std::move(row));
        b->messages.clear();
        for (const auto &[identity, rank] : originalOrder)
            if (auto row = retained.find(identity); row != retained.end()) {
                row->second.historyOrder = rank;
                b->messages.push_back(std::move(row->second));
            }
        std::erase_if(b->chatReplaySequences,
                      [&](int64_t sequence) { return sequence > replayMaximum; });
        for (const auto &[sequence, present] : replayBefore) {
            if (present)
                b->chatReplaySequences.insert(sequence);
            else
                b->chatReplaySequences.erase(sequence);
        }
        for (const auto &id : touched)
            b->replayChatKeys.erase(id);
        for (const auto &[id, key] : keysBefore)
            if (key)
                b->replayChatKeys[id] = *key;
        b->lastChatTimestamp = savedActivity.timestamp;
        b->lastChatSequence = savedActivity.sequence;
        b->lastChatTime = savedActivity.time;
        b->lastChatId = savedChatId;
        b->sourceMessageIndex.clear();
        std::erase_if(pending_, [&](const auto &entry) { return entry.second.buffer == b->id; });
        pending_.merge(savedPending);
        std::erase_if(queuedSends_,
                      [&](const auto &entry) { return entry.second.buffer == b->id; });
        queuedSends_.merge(savedQueued);
        messageCount_ = savedCount - savedSize + b->messages.size();
        messageRevision_ = savedRevision;
        mentionsLiveRevision_ = savedMentions;
        sequence_ = savedSequence;
        std::erase_if(activity_,
                      [&](const auto &entry) { return entry.first.starts_with(b->id + "\n"); });
        for (const auto &row : b->messages)
            activity(*b, row);
        ++b->messagesRevision;
        ++b->participationRevision;
        recount(*b);
        std::erase_if(presenceGroups_, [&](const auto &group) {
            return std::any_of(
                group.second.events.begin(), group.second.events.end(),
                [&](const auto &event) { return historyOwner(event) == page.request.requestId; });
        });
        throw;
    }
}

bool Application::zncPlayback(const string &server) const {
    auto network = model.networks.find(server);
    if (network == model.networks.end() || !network->second.caps.contains("znc.in/playback") ||
        network->second.caps.contains("draft/chathistory") ||
        network->second.caps.contains("chathistory"))
        return false;
    return std::any_of(model.servers.begin(), model.servers.end(), [&](const auto &config) {
        return config.name == server && config.bouncer == "znc";
    });
}
bool Application::historyUsesMsgid(const string &server, const string &id) const {
    if (id.empty() || id.starts_with("local:") || id.starts_with("local-") || zncPlayback(server))
        return false;
    const auto refs = historyReferences_.find(server);
    return refs == historyReferences_.end() || refs->second == "msgid" ||
           refs->second.starts_with("msgid,") || refs->second.ends_with(",msgid") ||
           refs->second.find(",msgid,") != string::npos;
}
string Application::historyCursor(const Buffer &b, const string &cursor) const {
    if (cursor.empty())
        return {};
    string selector = cursor;
    if (!selector.starts_with("timestamp=")) {
        const string id = selector.starts_with("msgid=") ? selector.substr(6) : selector;
        const bool timestampOnly = !historyUsesMsgid(b.server, id);
        auto message = timestampOnly ? std::find_if(b.messages.begin(), b.messages.end(),
                                                    [&](const Message &m) { return m.id == id; })
                                     : b.messages.end();
        if (timestampOnly && message != b.messages.end() && !message->time.empty())
            selector = "timestamp=" + message->time;
        else
            selector = "msgid=" + id;
    }
    return selector;
}
vector<string> Application::historyOrder(const string &server) const {
    vector<const Buffer *> pending;
    for (const auto &[id, b] : model.buffers)
        if (b.server == server && b.initialHistorySync != HistorySyncState::Idle)
            pending.push_back(&b);
    const auto session = historySessions_.find(server);
    const auto &pins = model.settings.pinnedBuffers;
    auto pin = [&](const Buffer &b) { return std::find(pins.begin(), pins.end(), b.id); };
    auto advertised = [&](const Buffer &b) {
        if (session == historySessions_.end())
            return int64_t{0};
        const auto it = session->second.advertised.find(fold(server, b.target));
        return it == session->second.advertised.end() ? int64_t{0} : historyMillis(it->second.time);
    };
    std::stable_sort(pending.begin(), pending.end(), [&](const Buffer *a, const Buffer *b) {
        if ((a->id == model.activeBuffer) != (b->id == model.activeBuffer))
            return a->id == model.activeBuffer;
        if (pin(*a) != pin(*b))
            return pin(*a) < pin(*b);
        const bool lowA = a->muted || a->archived, lowB = b->muted || b->archived;
        if (lowA != lowB)
            return !lowA;
        if (advertised(*a) != advertised(*b))
            return advertised(*a) > advertised(*b);
        return chatActivityBefore(*a, *b);
    });
    vector<string> order;
    for (const auto *b : pending)
        order.push_back(b->target);
    return order;
}
void Application::submitHistory(HistoryPage page) {
    auto c = client(page.server);
    if (!c)
        return;
    if (page.request.requestId.empty())
        page.request.requestId = localId();
    page.started = model.now;
    if (page.request.direction == HistoryDirection::Latest && page.request.first.id.empty() &&
        page.request.first.time.empty())
        if (const auto *b = findBuffer(page.buffer); b && b->kind == "query")
            if (const auto floor = storage_.historyDiscarded(b->id); floor && !floor->empty())
                page.request.first.time = *floor;
    const auto label = page.request.requestId;
    historyPages_.emplace(label, std::move(page));
    auto &stored = historyPages_.at(label);
    if (!stored.buffer.empty()) {
        activeHistory_[label] = stored.buffer;
        if (auto b = findBuffer(stored.buffer)) {
            if (stored.request.initial) {
                initialHistory_[label] = b->id;
                b->initialHistorySync = HistorySyncState::Queued;
            } else if (!stored.seed)
                b->loading = true;
        }
    }
    try {
        c->history(stored.request);
    } catch (const std::exception &error) {
        auto failed = std::move(stored);
        historyPages_.erase(label);
        Event end;
        end.server = failed.server;
        end.label = label;
        end.tags["error"] = error.what();
        finishHistoryPage(std::move(failed), end, false, false);
    }
}
void Application::enqueueHistory(Buffer &b, HistoryRequest request, int64_t gapId) {
    request.target = b.target;
    HistoryCursor gapBoundary;
    if (gapId)
        for (const auto &gap : b.historyState.gaps)
            if (gap.id == gapId)
                gapBoundary = gap.newer;
    if (gapId && !gapBoundary.id.empty() && request.first == gapBoundary &&
        irc::parseTimestampParts(request.first.time))
        request.first.time = historyTime(historyMillis(request.first.time) + 1);
    const auto &network = model.networks[b.server];
    for (auto *cursor : {&request.first, &request.second})
        if (!historyUsesMsgid(b.server, cursor->id))
            cursor->id.clear();
    if (request.direction == HistoryDirection::Latest && request.first.id.empty() &&
        request.first.time.empty() && b.kind == "query")
        if (const auto floor = storage_.historyDiscarded(b.id); floor && !floor->empty())
            request.first.time = *floor;
    if (network.state != "Ready" || !client(b.server) ||
        (!network.caps.contains("chathistory") && !network.caps.contains("draft/chathistory") &&
         !zncPlayback(b.server)))
        return;
    if (const auto session = historySessions_.find(b.server);
        session != historySessions_.end() && session->second.refused.contains(b.id))
        return;
    if (const auto session = historySessions_.find(b.server);
        session != historySessions_.end() && request.direction == HistoryDirection::Before &&
        std::any_of(session->second.retries.begin(), session->second.retries.end(),
                    [&](const auto &queued) {
                        return queued.buffer == b.id &&
                               queued.request.direction == HistoryDirection::Before;
                    }))
        return;
    for (auto &[label, page] : historyPages_)
        if (page.server == b.server && page.buffer == b.id &&
            ((request.direction == HistoryDirection::Before &&
              page.request.direction == HistoryDirection::Before) ||
             (page.request.direction == request.direction && page.request.first == request.first &&
              page.request.second == request.second && page.gapId == gapId))) {
            if (request.initial && !page.request.initial) {
                page.request.initial = true;
                page.seed = false;
                initialHistory_[label] = b.id;
                b.initialHistorySync = HistorySyncState::Loading;
            } else if (!request.initial)
                b.loading = true;
            return;
        }
    HistoryPage page;
    page.gapBoundary = std::move(gapBoundary);
    page.request = std::move(request);
    page.server = b.server;
    page.buffer = b.id;
    page.session = model.networks[b.server].sessionGeneration;
    page.gapId = gapId;
    submitHistory(std::move(page));
}
size_t Application::loadLocalHistory(Buffer &b, int limit, const string &before) {
    if (!savingAllowed_)
        return 0;
    try {
        auto page = storage_.history(b.id, limit, before);
        for (auto &message : page) {
            message.history = true;
            message.pending = false;
            insert(b, std::move(message), false);
        }
        return page.size();
    } catch (const std::exception &e) {
        report(e.what(), b.server);
        return 0;
    }
}

void Application::preloadScrollback() {
    if (scrollbackBuffer_ != model.activeBuffer) {
        scrollbackBuffer_ = model.activeBuffer;
        scrollbackPreloadAt_ = model.now + 0.25;
    }
    if (!scrollbackPreloadAt_ || model.now < *scrollbackPreloadAt_)
        return;
    auto *b = findBuffer(scrollbackBuffer_);
    if (!savingAllowed_ || !b || (b->kind != "channel" && b->kind != "query")) {
        scrollbackPreloadAt_.reset();
        return;
    }
    if (b->loading)
        return;

    const auto limit = static_cast<size_t>(std::clamp(model.settings.maxMessages, 1, 100000));
    const auto target = std::min<size_t>(600, limit);
    const Message *oldest = nullptr;
    size_t count = 0;
    for (const auto &message : b->messages) {
        if (!isHistoryWindowMessage(message))
            continue;
        if (!oldest)
            oldest = &message;
        if (++count == target)
            break;
    }
    if (!oldest || count == target || messageCount_ >= limit) {
        scrollbackPreloadAt_.reset();
        return;
    }
    const auto pageSize = std::min({size_t(50), target - count, limit - messageCount_});
    const string before = oldest->id;
    if (loadLocalHistory(*b, static_cast<int>(pageSize), before) < pageSize ||
        oldestHistoryMessage(*b)->id == before)
        scrollbackPreloadAt_.reset();
}

void Application::enqueueHistory(Buffer &b, const string &cursor, bool initial) {
    HistoryRequest request;
    request.initial = initial;
    request.direction = cursor.empty() ? HistoryDirection::Latest : HistoryDirection::Before;
    auto selector = historyCursor(b, cursor);
    if (selector.starts_with("timestamp="))
        request.first.time = selector.substr(10);
    else if (selector.starts_with("msgid=")) {
        request.first.id = selector.substr(6);
        const auto row = std::find_if(b.messages.begin(), b.messages.end(),
                                      [&](const Message &m) { return m.id == request.first.id; });
        if (row != b.messages.end())
            request.first.time = row->time;
    }
    enqueueHistory(b, std::move(request));
}
void Application::cancelHistory(Buffer &b) {
    if (auto c = client(b.server))
        try {
            c->cancelInitialHistory(b.target);
        } catch (const std::exception &e) {
            report("History: " + string(e.what()), b.server);
        }
    std::erase_if(historyPages_, [&](const auto &entry) {
        if (entry.second.buffer != b.id)
            return false;
        activeHistory_.erase(entry.first);
        initialHistory_.erase(entry.first);
        return true;
    });
    if (auto it = historySessions_.find(b.server); it != historySessions_.end()) {
        auto &s = it->second;
        std::erase(s.visible, b.id);
        std::erase(s.background, b.id);
        if (std::erase(s.seeds, b.id))
            s.seedFailed = true;
        std::erase_if(s.retries, [&](const HistoryPage &page) { return page.buffer == b.id; });
    }
    b.initialHistorySync = HistorySyncState::Idle;
    b.loading = false;
    requestedHistory_.erase(b.id);
}
void Application::cancelHistorySession(const string &server) {
    historySessions_.erase(server);
    std::erase_if(historyPages_, [&](const auto &entry) {
        if (entry.second.server != server)
            return false;
        activeHistory_.erase(entry.first);
        initialHistory_.erase(entry.first);
        return true;
    });
    for (auto &[id, b] : model.buffers)
        if (b.server == server) {
            b.initialHistorySync = HistorySyncState::Idle;
            b.loading = false;
            requestedHistory_.erase(id);
        }
}
void Application::planHistory(const string &server, bool seeds) {
    auto &s = historySessions_.at(server);
    for (const auto &[key, advertised] : s.advertised) {
        auto known = std::find_if(model.buffers.begin(), model.buffers.end(), [&](const auto &v) {
            return v.second.server == server && fold(server, v.second.target) == key;
        });
        if (known != model.buffers.end() || channel(server, advertised.name))
            continue;
        const auto discarded = storage_.historyDiscarded(server + "/" + key);
        if (discarded &&
            (discarded->empty() || historyMillis(advertised.time) <= historyMillis(*discarded)))
            continue;
        buffer(server, advertised.name, "query");
    }
    for (auto &[id, b] : model.buffers) {
        if (b.server != server || (b.kind != "query" && b.kind != "channel") ||
            (b.kind == "channel" && !b.joined))
            continue;
        const auto advertised = s.advertised.find(fold(server, b.target));
        const bool hasCursor =
            !b.historyState.newest.id.empty() || !b.historyState.newest.time.empty();
        const bool changed =
            !s.refused.contains(id) && (!hasCursor || (advertised != s.advertised.end() &&
                                                       markerTime(b.historyState.newest.time) <
                                                           markerTime(advertised->second.time)));
        if (seeds && advertised == s.advertised.end())
            continue;
        const bool gaps =
            !seeds && !s.refused.contains(id) &&
            std::any_of(b.historyState.gaps.begin(), b.historyState.gaps.end(),
                        [&](const auto &gap) {
                            return !gap.unavailable && !s.attemptedGaps.contains({id, gap.id});
                        });
        b.initialHistorySync = changed || gaps ? HistorySyncState::Queued : HistorySyncState::Idle;
    }
    size_t count = 0;
    for (const auto &target : historyOrder(server)) {
        auto found = std::find_if(model.buffers.begin(), model.buffers.end(), [&](const auto &v) {
            return v.second.server == server &&
                   fold(server, v.second.target) == fold(server, target);
        });
        if (found == model.buffers.end())
            continue;
        auto &b = found->second;
        if (seeds) {
            s.seeds.push_back(b.id);
            b.initialHistorySync = HistorySyncState::Idle;
        } else if (count++ < 12)
            s.visible.push_back(b.id);
        else {
            s.background.push_back(b.id);
            b.initialHistorySync = HistorySyncState::Idle;
        }
    }
    s.nextTarget = model.now + .5;
}
void Application::scheduleHistory() {
    for (const auto &[server, c] : clients_) {
        try {
            const auto &network = model.networks[server];
            if (network.state != "Ready" || network.automaticHistoryPaused)
                continue;
            const bool supported =
                network.caps.contains("draft/chathistory") || network.caps.contains("chathistory");
            if (!supported) {
                if (zncPlayback(server))
                    for (auto &[id, b] : model.buffers)
                        if (b.server == server && b.initialHistorySync != HistorySyncState::Idle &&
                            std::none_of(initialHistory_.begin(), initialHistory_.end(),
                                         [&](const auto &v) { return v.second == id; })) {
                            HistoryRequest request;
                            request.initial = true;
                            auto newest = irc::parseTimestampParts(b.historyState.newest.time);
                            if (newest)
                                request.first.time = b.historyState.newest.time;
                            for (const auto &row : b.messages) {
                                if (row.presence || row.pending || row.id.empty() ||
                                    row.id.starts_with("local:") || row.id.starts_with("local-"))
                                    continue;
                                const auto time = irc::parseTimestampParts(row.time);
                                if (time && (!newest || time->seconds > newest->seconds ||
                                             (time->seconds == newest->seconds &&
                                              time->nanoseconds > newest->nanoseconds))) {
                                    request.first.time = row.time;
                                    newest = time;
                                }
                            }
                            if (!request.first.time.empty())
                                request.direction = HistoryDirection::After;
                            enqueueHistory(b, std::move(request));
                        }
                continue;
            }
            if (!channelTypes_.contains(server) && !historyClassificationSettled_.contains(server))
                continue;
            auto priorities = historyOrder(server);
            if (const auto *viewed = findBuffer(model.activeBuffer);
                viewed && viewed->server == server) {
                std::erase(priorities, viewed->target);
                priorities.insert(priorities.begin(), viewed->target);
            }
            c->prioritizeHistory(priorities);
            auto existing = historySessions_.find(server);
            if (existing != historySessions_.end() &&
                existing->second.generation != network.sessionGeneration)
                cancelHistorySession(server);
            if (!historySessions_.contains(server)) {
                HistorySession session;
                session.generation = network.sessionGeneration;
                session.durable = storage_.historyDiscovery(server);
                const auto now = historyMillis(irc::timestampNow());
                const auto base = session.durable.watermark.empty()
                                      ? now - 30LL * 24 * 60 * 60 * 1000
                                      : historyMillis(session.durable.watermark);
                session.lower = historyTime(base - 10000);
                session.upper = historyTime(now + 10000);
                if (session.durable.backfillUpper.empty() && !session.durable.backfillComplete) {
                    session.durable.backfillUpper = historyTime(historyMillis(session.lower) + 1);
                    if (savingAllowed_)
                        storage_.saveHistoryDiscovery(server, session.durable);
                }
                historySessions_.emplace(server, std::move(session));
            }
            auto &s = historySessions_.at(server);
            if (auto *viewed = findBuffer(model.activeBuffer); viewed && viewed->server == server) {
                const auto background =
                    std::find(s.background.begin(), s.background.end(), viewed->id);
                if (background != s.background.end()) {
                    s.background.erase(background);
                    s.visible.push_front(viewed->id);
                    viewed->initialHistorySync = HistorySyncState::Queued;
                } else if (auto queued = std::find(s.visible.begin(), s.visible.end(), viewed->id);
                           queued != s.visible.end() && queued != s.visible.begin()) {
                    s.visible.erase(queued);
                    s.visible.push_front(viewed->id);
                }
            }
            size_t active = 0;
            bool targets = false, seed = false;
            for (const auto &[label, page] : historyPages_)
                if (page.server == server && page.session == s.generation) {
                    ++active;
                    targets |= page.request.direction == HistoryDirection::Targets;
                    seed |= page.seed;
                }
            const unsigned width = network.caps.contains("labeled-response") ? s.width : 1;
            while (!s.retries.empty() && active < width) {
                auto page = std::move(s.retries.front());
                s.retries.pop_front();
                targets |= page.request.direction == HistoryDirection::Targets;
                seed |= page.seed;
                submitHistory(std::move(page));
                ++active;
            }
            if (s.discovering) {
                if (!targets && active < width && model.now >= s.nextDiscovery &&
                    (!s.backfill || (s.seeds.empty() && !seed && s.checkpoint.empty()))) {
                    HistoryPage page;
                    page.server = server;
                    page.session = s.generation;
                    page.request.direction = HistoryDirection::Targets;
                    page.request.limit = 100;
                    page.request.first.time = s.upper;
                    page.request.second.time = s.lower;
                    submitHistory(std::move(page));
                    ++active;
                    targets = true;
                }
                if (!s.backfill)
                    continue;
            }
            while (!s.visible.empty() && active < width) {
                const auto id = s.visible.front();
                s.visible.pop_front();
                if (auto b = findBuffer(id)) {
                    const auto advertised = s.advertised.find(fold(server, b->target));
                    const bool current = (!b->historyState.newest.id.empty() ||
                                          !b->historyState.newest.time.empty()) &&
                                         (advertised == s.advertised.end() ||
                                          markerTime(b->historyState.newest.time) >=
                                              markerTime(advertised->second.time));
                    const auto gap = std::find_if(
                        b->historyState.gaps.begin(), b->historyState.gaps.end(),
                        [&](const auto &value) {
                            return !value.unavailable && !s.attemptedGaps.contains({id, value.id});
                        });
                    if (current && gap != b->historyState.gaps.end()) {
                        HistoryRequest request;
                        request.direction = HistoryDirection::Before;
                        request.first = gap->newer;
                        request.initial = true;
                        s.attemptedGaps.insert({id, gap->id});
                        enqueueHistory(*b, std::move(request), gap->id);
                    } else
                        enqueueHistory(*b, "", true);
                    ++active;
                }
            }
            const bool visibleActive =
                std::any_of(initialHistory_.begin(), initialHistory_.end(), [&](const auto &entry) {
                    const auto b = findBuffer(entry.second);
                    return b && b->server == server;
                });
            if (!s.visible.empty() || visibleActive) {
                s.visibleDone = false;
                continue;
            }
            if (!s.visibleDone) {
                s.visibleDone = true;
                s.nextTarget = model.now + .5;
            }
            if (!s.backfill && s.planned && !s.discoveryFailed &&
                historyMillis(s.highwater) > historyMillis(s.durable.watermark)) {
                s.durable.watermark = s.highwater;
                if (savingAllowed_)
                    storage_.saveHistoryDiscovery(server, s.durable);
            }
            if (!s.backfill && !s.discovering && active == 0 && !s.durable.backfillComplete &&
                !s.durable.backfillUpper.empty()) {
                s.backfill = true;
                s.discovering = true;
                s.upper = s.durable.backfillUpper;
                s.lower = historyTime(0);
                s.tieTime.clear();
                s.tieTargets.clear();
                s.advertised.clear();
                s.shortTie = false;
                s.nextDiscovery = model.now + 2;
            }
            auto &paced = !s.seeds.empty() ? s.seeds : s.background;
            if (!paced.empty() && !seed && active == 0 && model.now >= s.nextTarget) {
                const auto id = paced.front();
                paced.pop_front();
                if (auto b = findBuffer(id)) {
                    HistoryPage page;
                    page.server = server;
                    page.buffer = id;
                    page.session = s.generation;
                    page.seed = true;
                    page.backfillSeed = &paced == &s.seeds;
                    page.request.target = b->target;
                    if (!page.backfillSeed) {
                        const auto advertised = s.advertised.find(fold(server, b->target));
                        const bool current = (!b->historyState.newest.id.empty() ||
                                              !b->historyState.newest.time.empty()) &&
                                             (advertised == s.advertised.end() ||
                                              markerTime(b->historyState.newest.time) >=
                                                  markerTime(advertised->second.time));
                        const auto gap =
                            std::find_if(b->historyState.gaps.begin(), b->historyState.gaps.end(),
                                         [&](const auto &value) {
                                             return !value.unavailable &&
                                                    !s.attemptedGaps.contains({id, value.id});
                                         });
                        if (current && gap != b->historyState.gaps.end()) {
                            page.request.direction = HistoryDirection::Before;
                            page.request.first = gap->newer;
                            if (!gap->newer.id.empty() &&
                                irc::parseTimestampParts(page.request.first.time))
                                page.request.first.time =
                                    historyTime(historyMillis(page.request.first.time) + 1);
                            page.gapId = gap->id;
                            page.gapBoundary = gap->newer;
                            if (!historyUsesMsgid(server, page.request.first.id))
                                page.request.first.id.clear();
                            s.attemptedGaps.insert({id, gap->id});
                        }
                    }
                    submitHistory(std::move(page));
                }
                s.nextTarget = model.now + .5;
                continue;
            }
            if (s.backfill && s.seeds.empty() && !seed && !targets && active == 0) {
                if (s.seedFailed) {
                    s.discovering = false;
                    continue;
                }
                if (!s.checkpoint.empty()) {
                    s.durable.backfillUpper = s.checkpoint;
                    if (s.terminal && !s.shortTie)
                        s.durable.backfillComplete = true;
                    if (savingAllowed_)
                        storage_.saveHistoryDiscovery(server, s.durable);
                    s.checkpoint.clear();
                    s.nextDiscovery = model.now + 2;
                }
                if (s.terminal)
                    s.discovering = false;
            }
        } catch (const std::exception &error) {
            cancelHistorySession(server);
            report("History: " + string(error.what()), server);
        }
    }
}
void Application::finishHistoryPage(HistoryPage page, const Event &end, bool applied,
                                    bool progressed) {
    activeHistory_.erase(end.label);
    initialHistory_.erase(end.label);
    auto b = findBuffer(page.buffer);
    if (b) {
        b->loading =
            std::any_of(activeHistory_.begin(), activeHistory_.end(), [&](const auto &entry) {
                return entry.second == b->id && !initialHistory_.contains(entry.first);
            });
        if (page.request.initial)
            b->initialHistorySync = HistorySyncState::Idle;
    }
    auto found = page.unsolicited ? historySessions_.end() : historySessions_.find(page.server);
    if (page.session != model.networks[page.server].sessionGeneration)
        return;
    const bool error = !tag(end, "error").empty() || flag(end, "cancelled") || !applied;
    if (found != historySessions_.end()) {
        auto &s = found->second;
        if (s.generation != page.session)
            return;
        if (flag(end, "timeout")) {
            s.width = std::max(1u, s.width / 2);
            s.clean = 0;
        } else if (!error && ++s.clean >= 3) {
            s.width = std::min(6u, s.width + 1);
            s.clean = 0;
        }
        if (error) {
            const auto reason = upper(tag(end, "failure-code"));
            const bool retryable = flag(end, "timeout") || reason == "TEMPORARILY_UNAVAILABLE" ||
                                   reason == "TEMPORARY_ERROR" || reason == "TRY_AGAIN";
            if (!flag(end, "cancelled") && !flag(end, "rejected") && retryable &&
                page.retries < 1) {
                ++page.retries;
                page.events.clear();
                page.bytes = 0;
                page.request.requestId.clear();
                if (b) {
                    if (page.request.initial)
                        b->initialHistorySync = HistorySyncState::Queued;
                    else if (!page.seed)
                        b->loading = true;
                }
                s.retries.push_back(std::move(page));
                return;
            }
            if (page.request.direction == HistoryDirection::Targets) {
                s.discoveryFailed = true;
                s.discovering = false;
                if (!s.backfill) {
                    planHistory(page.server);
                    s.planned = true;
                }
            } else if (reason == "INVALID_TARGET") {
                s.refused.insert(page.buffer);
                std::erase(s.visible, page.buffer);
                std::erase(s.background, page.buffer);
                std::erase(s.seeds, page.buffer);
            } else if (page.backfillSeed)
                s.seedFailed = true;
            if (!flag(end, "cancelled"))
                report("History: " + tag(end, "error"), page.server);
            return;
        }
        if (page.request.direction == HistoryDirection::Targets) {
            int64_t oldest = std::numeric_limits<int64_t>::max();
            set<string> tied;
            size_t count = 0;
            int limit = page.request.limit;
            const auto negotiated = tag(end, "limit");
            if (!negotiated.empty()) {
                int value = 0;
                const auto parsed = std::from_chars(negotiated.data(),
                                                    negotiated.data() + negotiated.size(), value);
                if (parsed.ec == std::errc{} && value > 0)
                    limit = value;
            }
            for (const auto &event : page.events) {
                if (event.type != "history_target")
                    continue;
                ++count;
                const auto time = historyMillis(event.time);
                const auto key = fold(page.server, event.target);
                auto &advertised = s.advertised[key];
                if (advertised.name.empty() || time > historyMillis(advertised.time))
                    advertised = {event.target, event.time};
                if (time > historyMillis(s.highwater))
                    s.highwater = event.time;
                if (time < oldest) {
                    oldest = time;
                    tied.clear();
                }
                if (time == oldest)
                    tied.insert(key);
            }
            if (s.advertised.size() > 100000) {
                s.discoveryFailed = true;
                s.discovering = false;
                report("History: TARGETS exceeded the discovery limit", page.server);
                if (!s.backfill) {
                    planHistory(page.server);
                    s.planned = true;
                }
                return;
            }
            s.terminal = flag(end, "end") || count == 0;
            string next;
            if (!s.terminal) {
                const bool repeated = s.tieTime == historyTime(oldest) && s.tieTargets == tied;
                if (repeated && count < static_cast<size_t>(limit) &&
                    oldest > historyMillis(s.lower)) {
                    s.shortTie = true;
                    next = historyTime(oldest);
                    s.tieTime.clear();
                    s.tieTargets.clear();
                } else if (repeated) {
                    s.discoveryFailed = true;
                } else {
                    next = historyTime(oldest + 1);
                    s.tieTime = historyTime(oldest);
                    s.tieTargets = std::move(tied);
                    if (historyMillis(next) >= historyMillis(s.upper) ||
                        historyMillis(next) <= historyMillis(s.lower))
                        s.discoveryFailed = true;
                }
            }
            if (s.discoveryFailed) {
                s.discovering = false;
                report("History: TARGETS could not advance its timestamp boundary", page.server);
            } else if (s.terminal)
                s.discovering = false;
            if (s.backfill) {
                if (!s.discoveryFailed) {
                    s.checkpoint = s.terminal ? s.lower : next;
                    if (!s.terminal)
                        s.upper = next;
                    planHistory(page.server, true);
                    s.advertised.clear();
                    s.nextDiscovery = model.now + 2;
                }
            } else if (!s.discovering) {
                planHistory(page.server);
                s.planned = true;
            } else
                s.upper = next;
            return;
        }
        if (page.request.initial && !page.seed)
            for (const auto &name : {"oldest-time", "newest-time"})
                if (historyMillis(tag(end, name)) > historyMillis(s.highwater))
                    s.highwater = tag(end, name);
        if (page.seed)
            s.nextTarget = model.now + .5;
    } else if (error) {
        report("History: " + tag(end, "error"), page.server);
        return;
    }
    const auto actualLimit =
        tag(end, "limit").empty() ? std::to_string(page.request.limit) : tag(end, "limit");
    const bool timestampRequest =
        tag(end, "ref-type") == "timestamp" ||
        (tag(end, "ref-type").empty() &&
         !historyUsesMsgid(page.server, page.request.direction == HistoryDirection::Latest
                                            ? tag(end, "oldest-id")
                                            : page.request.first.id));
    const bool timestampFull = timestampRequest && tag(end, "count") == actualLimit;
    if (b && page.request.initial && (timestampFull || flag(end, "incomplete")))
        requestedHistory_.erase(b->id);
    if (!b || !applied || error || flag(end, "incomplete"))
        return;
    const auto firstContent = std::find_if(page.events.begin(), page.events.end(), historyPrimary);
    const bool startSeam = (page.request.direction == HistoryDirection::Latest &&
                            (page.request.initial || b->id == model.activeBuffer || page.seed)) ||
                           page.latestSeam;
    const bool olderPresence = page.request.direction == HistoryDirection::Before && !page.gapId &&
                               !page.latestSeam && progressed && firstContent == page.events.end();
    HistoryCursor cursor{tag(end, "oldest-id"), tag(end, "oldest-time")};
    bool advancing = progressed;
    if (timestampFull && irc::parseTimestampParts(cursor.time)) {
        cursor = {{}, historyTime(historyMillis(cursor.time) + 1)};
        advancing = page.request.direction == HistoryDirection::Latest ||
                    historyMillis(cursor.time) < historyMillis(page.request.first.time);
    }
    auto gap = std::find_if(b->historyState.gaps.begin(), b->historyState.gaps.end(),
                            [&](const auto &value) { return value.id == page.gapId; });
    const bool owned = gap != b->historyState.gaps.end();
    if (page.gapId && found != historySessions_.end())
        found->second.attemptedGaps.insert({b->id, page.gapId});
    const bool seekingContent = !page.gapId && (startSeam || olderPresence) && !flag(end, "end") &&
                                tag(end, "count") != "0" && firstContent == page.events.end() &&
                                irc::parseTimestampParts(cursor.time);
    if ((!owned || (found != historySessions_.end() && (!advancing || gap->unavailable))) &&
        (startSeam || page.gapId) && !seekingContent) {
        if (startSeam && firstContent != page.events.end()) {
            const HistoryCursor seam =
                timestampFull ? cursor
                              : HistoryCursor{historyProtocolId(*firstContent), firstContent->time};
            gap = std::find_if(
                b->historyState.gaps.begin(), b->historyState.gaps.end(), [&](const auto &value) {
                    return !value.unavailable && (seam.id.empty() ? value.newer.time == seam.time
                                                                  : value.newer.id == seam.id);
                });
        } else
            gap = b->historyState.gaps.end();
        if (gap == b->historyState.gaps.end())
            gap = std::find_if(
                b->historyState.gaps.begin(), b->historyState.gaps.end(), [&](const auto &value) {
                    return value.id != page.gapId && !value.unavailable &&
                           (found == historySessions_.end() ||
                            !found->second.attemptedGaps.contains({b->id, value.id}));
                });
        if (gap != b->historyState.gaps.end()) {
            cursor = gap->newer;
            if (!cursor.id.empty() && irc::parseTimestampParts(cursor.time))
                cursor.time = historyTime(historyMillis(cursor.time) + 1);
            advancing = true;
        }
    }
    if (seekingContent && page.request.direction == HistoryDirection::Latest)
        advancing = true;
    if (!advancing || (gap == b->historyState.gaps.end() && !seekingContent) ||
        (gap != b->historyState.gaps.end() && gap->unavailable))
        return;
    HistoryPage next;
    next.server = page.server;
    next.buffer = b->id;
    next.session = page.session;
    if (gap != b->historyState.gaps.end()) {
        next.gapId = gap->id;
        next.gapBoundary = gap->newer;
        if (found != historySessions_.end())
            found->second.attemptedGaps.insert({b->id, gap->id});
    } else
        next.latestSeam = startSeam;
    next.request.direction = HistoryDirection::Before;
    next.request.target = b->target;
    next.request.first = std::move(cursor);
    if (!historyUsesMsgid(b->server, next.request.first.id))
        next.request.first.id.clear();
    if (found != historySessions_.end()) {
        found->second.retries.push_back(std::move(next));
        b->loading = true;
    } else
        submitHistory(std::move(next));
}
void Application::requestHistory(Buffer &b, const string &before, bool initial) {
    const auto &n = model.networks[b.server];
    if (initial && n.automaticHistoryPaused)
        return;
    if (const auto session = historySessions_.find(b.server);
        session != historySessions_.end() && session->second.refused.contains(b.id))
        return;
    if ((b.kind != "channel" && b.kind != "query") || n.state != "Ready" ||
        (b.kind == "channel" && !b.joined) || !client(b.server))
        return;
    if (!n.caps.contains("draft/chathistory") && !n.caps.contains("chathistory") &&
        !zncPlayback(b.server))
        return;
    if (!initial) {
        HistoryRequest request;
        request.direction = before.empty() ? HistoryDirection::Latest : HistoryDirection::Before;
        const auto selector = historyCursor(b, before);
        if (selector.starts_with("timestamp="))
            request.first.time = selector.substr(10);
        else if (selector.starts_with("msgid=")) {
            request.first.id = selector.substr(6);
            const auto row =
                std::find_if(b.messages.begin(), b.messages.end(),
                             [&](const auto &value) { return value.id == request.first.id; });
            if (row != b.messages.end())
                request.first.time = row->time;
        }
        auto older = [&](const HistoryCursor &cursor) {
            if (cursor.id.empty() && cursor.time.empty())
                return;
            if (request.first.id.empty() && request.first.time.empty())
                request.first = cursor;
            else if (historyMillis(cursor.time) &&
                     historyMillis(cursor.time) < historyMillis(request.first.time))
                request.first = cursor;
        };
        older(b.historyState.oldest);
        for (const auto &gap : b.historyState.gaps)
            older(gap.older);
        if (!request.first.id.empty() || !request.first.time.empty())
            request.direction = HistoryDirection::Before;
        enqueueHistory(b, std::move(request));
        return;
    }
    if (requestedHistory_.insert(b.id).second) {
        b.initialHistorySync = HistorySyncState::Queued;
        const auto session = historySessions_.find(b.server);
        if (session != historySessions_.end() && session->second.planned)
            session->second.visible.push_front(b.id);
    }
}
} // namespace blast
