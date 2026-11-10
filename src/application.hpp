#pragma once
#include "avatar.hpp"
#include "irc/network.hpp"
#include "model.hpp"
#include "storage.hpp"
#include <deque>
#include <memory>
#include <optional>

namespace blast {
// Sensitive, source-bound upload authorization; never persist or log this value.
// Revalidate with Current immediately before worker start and UI completion.
struct SojuFileHostContext {
    string source, parent, networkId, physicalHost, account, endpoint, trustedHost, password;
    uint64_t sourceGeneration = 0, parentGeneration = 0;
    bool operator==(const SojuFileHostContext &) const = default;
};
std::optional<SojuFileHostContext> sojuFileHostContext(const Model &, const string &source);
bool sojuFileHostContextCurrent(const Model &, const SojuFileHostContext &);
// Opaque runtime-only digest of the effective configuration supplied to a client.
string sojuSessionKey(const ServerConfig &, const Identity &);
class Application {
  public:
    Model model;
    Application();
    ~Application() noexcept;
    // Frame order: tick, Theme::update, Ui::update, Effects::draw.
    void tick(double now);
    bool save();
    Storage &storage() { return storage_; }

  private:
    friend struct ApplicationRegression;
    struct Pending {
        string buffer, id;
        double sent = 0;
        string label;
        std::optional<string> originalText{}; // Raw reply bytes retained for late multiline retry.
    };
    struct CommandOrigin {
        string server, buffer;
        double started = 0;
        string whoisTarget{};
        uint64_t sessionGeneration = 0;
    };
    struct QueuedSend {
        string buffer, text;
        uint64_t order = 0;
        size_t parts = 0;
        bool recovered = false, stored = true, complete = false;
        set<string> confirmed{};  // Actual own echoes, never the visual pending timeout.
        bool preserveRaw = false; // Reply prefixes belong on the wire, never in recovered drafts.
        set<string> written{}; // Completed no-echo writes, distinct from server acknowledgements.
    };
    struct HistoryGap {
        string cursor, next, label;
        size_t received = 0;
        bool active = false;
    };
    struct PresenceGroup {
        double started = 0;
        bool joins = false, explicitBatch = false, emitted = false;
        vector<Event> events;
    };
    struct ChannelAvatar {
        string url;
        uint64_t request = 0;
    };
    Storage storage_;
    map<string, std::unique_ptr<NetworkClient>> clients_;
    AvatarLoader avatarLoader_;
    map<string, ChannelAvatar> channelAvatars_;
    uint64_t avatarSequence_ = 0;
    map<string, string> caseMapping_, channelTypes_, prefixModes_, channelModes_,
        historyReferences_;
    map<string, std::deque<int64_t>> activity_;
    map<string, vector<Nick>> namesCache_;
    map<string, PresenceGroup> presenceGroups_;
    map<string, Pending> pending_;
    map<string, QueuedSend> queuedSends_;
    map<string, HistoryGap> gaps_;
    map<string, double> failedLabels_, failedSubmissions_;
    map<string, string> activeHistory_;
    map<string, CommandOrigin> commandOrigins_;
    set<string> namesInProgress_, requestedHistory_;
    map<string, set<string>> sojuCatalogSeen_;
    map<string, string> sojuCatalogBatch_;
    map<string, std::pair<string, string>> sojuMutations_;
    set<string> sojuSuspended_, sojuRetired_;
    bool shuttingDown_ = false;
    void drainClients();
    void adoptSojuProfiles();
    void synchronizeSoju(const string &parent);
    void retireSoju(const string &parent, const string &id);
    void disconnect(string server);
    vector<Action> activeActions_;
    size_t nextAction_ = 0;
    bool savingAllowed_ = true, typingEnabled_ = true;
    set<string> fools_;
    bool foolUnread_ = false;
    size_t messageCount_ = 0;
    uint64_t sequence_ = 0, messageRevision_ = 0;
    string idPrefix_;
    uint64_t mentionsStorageRevision_ = UINT64_MAX, mentionsLiveRevision_ = 0,
             mentionsBuiltRevision_ = UINT64_MAX;
    string mentionsNetworkKey_;
    map<string, Buffer> unsavedSources_;
    void refreshMentions();
    void jumpSource(const string &, const string &);
    double lastSave_ = 0, lastPrune_ = -30;
    string fold(const string &server, const string &value) const;
    bool channel(const string &server, const string &target) const;
    bool visible(const Buffer &) const;
    Buffer &buffer(const string &server, const string &target, const string &kind = "");
    void renameBuffer(string id, string target);
    Buffer *findBuffer(const string &id);
    NetworkClient *client(const string &server);
    string localId();
    void reviseMessages(Buffer &);
    void clearAvatar(Buffer &, bool metadata = true);
    void updateAvatars();
    void finishAvatar(AvatarLoader::Result);
    // Draining client events may erase the Buffer that supplied the server name.
    void connect(string server, string origin = "");
    void stopClient(string server);
    void reduce(const Event &);
    void action(const Action &);
    void dispatchActions();
    void command(Buffer &, const string &);
    void enqueueCommand(const Buffer &, const string &);
    bool commandResponse(const Event &);
    void updateWhois(const Event &, const CommandOrigin &);
    void finishWhois(const string &error);
    void send(Buffer &, const string &, const string &reply = "", bool action = false,
              const string &origin = "", const string &fallbackNick = "");
    bool recoverQueuedSends(const Event &, const string &reason);
    string confirmQueuedSend(const Buffer &, const string &label);
    bool failedSubmission(const string &label) const;
    void receive(Event);
    bool insert(Buffer &, Message, bool persist = true, const string &completedSend = "",
                bool confirmed = false);
    bool store(const Buffer &, Message &, bool update = false, const string &completedSend = "",
               bool confirmed = false);
    void repairChatActivity(Buffer &);
    void report(const string &, const string &server = "", const string &origin = "");
    void restoreDraft(Buffer &, const string &, bool distinctSubmission = false);
    bool failSends(const Event &, const string &);
    void effect(Event, const Buffer &);
    void activity(const Buffer &, const Message &);
    PresenceMode presenceMode(const Buffer &) const;
    bool presenceVisible(const Buffer &, const Event &) const;
    void presence(Buffer &, Event, const string &text, bool emit = true);
    void groupPresence(Event, bool joins, bool explicitBatch);
    void flushPresence(PresenceGroup &, bool aggregate);
    void recount(Buffer &);
    bool markRead(Buffer &);
    string historyCursor(const Buffer &, const string &) const;
    bool zncPlayback(const string &server) const;
    void requestHistory(Buffer &, const string &before = "");
    void trim();
    void clear(Buffer *);
};
} // namespace blast
