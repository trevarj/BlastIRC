#pragma once
#include "model.hpp"
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>

namespace blast {
// Pure profile validation shared by native settings and persistence. Empty means
// valid; errors describe fields/rules without including passwords or key data.
string validateIdentityProfile(const Model &, std::size_t index);
string validateServerProfile(const Model &, std::size_t index);

// All failures throw std::runtime_error. Callers must surface them to the user.
class Storage {
  public:
    Storage();
    ~Storage();
    Storage(const Storage &) = delete;
    Storage &operator=(const Storage &) = delete;
    // Completed pages only: never keep this transaction across a network wait.
    class HistoryWrite {
      public:
        ~HistoryWrite();
        HistoryWrite(const HistoryWrite &) = delete;
        HistoryWrite &operator=(const HistoryWrite &) = delete;
        // Outermost commit applies retention after page metadata, within the same write.
        // Reload affected buffers' complete HistoryState afterward: pruning invalidates proof.
        void commit();
        // Only valid original server times establish playback cohorts. Native neighbor order
        // restores existing optimistic own rows first materialized durably by this write.
        vector<std::pair<int64_t, int64_t>>
        reconcileOrder(const string &buffer, const vector<string> &canonicalIds, bool prepend,
                       const vector<string> &existingCanonicalOrder = {});

      private:
        friend class Storage;
        explicit HistoryWrite(Storage &);
        struct State;
        std::unique_ptr<State> state;
    };
    HistoryWrite historyWrite();
    // Drain after a committed write/prune; only these other buffers need metadata reloads.
    vector<string> takeHistoryInvalidations();
    HistoryDiscovery historyDiscovery(const string &server);
    void saveHistoryDiscovery(const string &server, const HistoryDiscovery &);
    void loadHistoryState(Buffer &);
    // Assigns IDs only after its savepoint succeeds. An enclosing HistoryWrite rollback
    // requires callers to restore their in-memory page/state, including newly assigned IDs.
    void saveHistoryState(Buffer &);
    // nullopt means open/unknown; empty means a legacy closed source with no boundary.
    std::optional<string> historyDiscarded(const string &buffer);
    // load is all-or-nothing; a configuration failure blocks save until reload.
    // Restores metadata/drafts/read state, latest 200 rows and all provisional own rows.
    // Restored own local echoes have Unknown sendStatus even after pending times out;
    // other own non-presence rows are Confirmed. Incoming rows retain None.
    // Activity kinds round-trip explicitly; legacy rows remain ActivityKind::None.
    void load(Model &);
    // While firstRun is true, only preferences/workspace are saved, not profiles.
    // Global presence inheritance is valid only for channel overrides, not the global mode.
    // Saves canonical people_mode; legacy people_visible is accepted only on load.
    void save(const Model &);
    // Preferences only: pin lifecycle edits leave retained Source state untouched.
    void saveSettings(const Settings &);
    // Seed on restore/reopen; initialize=false preserves this session's replay exclusions.
    void seedChatActivity(Buffer &, bool initialize = true);
    // Per-buffer ID upsert; matching own-echo labels replace earlier pending ghost IDs.
    // Full message state replaces reactions. Redaction is permanent for that row.
    // Returns requested backup retirement proof after retention, not ordinary write success.
    // confirmed is actual own-server-echo evidence, never the visual pending timeout.
    // source captures authoritative routing metadata in the same transaction;
    // it never overwrites the durable draft/read state.
    bool append(const string &buffer, const Message &, const string &completedSend = "",
                std::size_t completedParts = 0, bool confirmed = false,
                const Buffer *source = nullptr, int64_t *storedSequence = nullptr,
                int64_t *storedOrder = nullptr);
    bool update(const string &buffer, const Message &, const string &completedSend = "",
                std::size_t completedParts = 0, bool confirmed = false,
                const Buffer *source = nullptr, int64_t *storedSequence = nullptr,
                int64_t *storedOrder = nullptr);
    // Atomically backs up a submission and the current post-submission editable draft.
    void queueSend(const Buffer &, const string &label, const string &text);
    // Persist a restored draft and retire its backup and provisional local messages.
    void recoverSend(const Buffer &, const string &label);
    // Oldest-to-newest by seconds, server-time nanoseconds, then durable history order.
    // before is a stored message ID or RFC3339 server time (exclusive).
    // limit=-1 returns every retained row, for validated source jumps.
    vector<Message> history(const string &buffer, int limit = 200, const string &before = "");
    // Indexed exact lookup, including rows outside the in-memory history tail.
    std::optional<Message> message(const string &buffer, const string &id);
    // Immutable identity and durable tie rank only; never loads message bodies/reactions.
    std::optional<std::pair<int64_t, int64_t>> messagePosition(const string &buffer,
                                                               const string &id);
    // Latest retained authoritative selector, including hidden/redacted rows outside RAM.
    std::optional<HistoryCursor> latestHistoryBoundary(const string &buffer);
    // Changes whenever durable state changes; callers can cache retained mention views.
    uint64_t revision() const;
    // Authoritative source metadata, including intentionally closed conversations.
    // Missing/incomplete metadata and synthetic/server buffers fail closed.
    std::optional<Buffer> sourceBuffer(const string &id);
    // All retained incoming mentions, oldest-first per source; backfills positive
    // classifications using only a known current network nickname/casemapping.
    map<string, Buffer> mentionSources(const map<string, Network> &networks);
    // Delete only this row (and its reactions), for an explicitly rejected ghost.
    void eraseMessage(const string &buffer, const string &id);
    // Intentionally close a buffer and discard its queued sends/provisional ghosts.
    // Confirmed history remains available for reopening, even without server IDs/times.
    // Captures current authoritative source context atomically, even before first save.
    void closeBuffer(const Buffer &buffer, const vector<string> &submissions = {});
    // Atomically migrate/merge durable state; caller rekeys Model/workspace.
    void renameBuffer(const string &oldId, const string &newId);
    void read(const string &buffer, const string &marker);
    void clearHistory();
    void clearHistory(const string &buffer);
    // retentionDays=0 disables age expiry; maxMessages is a global positive cap.
    void prune(const Settings &);
    string configDir() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace blast
