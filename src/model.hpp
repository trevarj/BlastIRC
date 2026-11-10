#pragma once
#include "irc/protocol.hpp"
#include "upload.hpp"
#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace blast {
using std::map;
using std::set;
using std::string;
using std::vector;
enum class FxLevel { Off, Subtle, Full };
enum class SendMotion { None, Flight, Rise, Pop, Slide, Drift };
enum class SendReveal { None, Fade, Pixels };
enum class FoolDisplay { Minimized, Blurred, Dimmed, Hidden, Shown };
enum class PreviewMode { Off, Click, Automatic };
enum class PresenceMode { All, Smart, Hidden, Global };
enum class SidebarMode { Expanded, Compact, Hidden };
constexpr SidebarMode nextSidebarMode(SidebarMode mode) {
    switch (mode) {
    case SidebarMode::Expanded:
        return SidebarMode::Compact;
    case SidebarMode::Compact:
        return SidebarMode::Hidden;
    case SidebarMode::Hidden:
        return SidebarMode::Expanded;
    }
    return SidebarMode::Hidden;
}
constexpr std::string_view sidebarModeToken(SidebarMode mode) {
    switch (mode) {
    case SidebarMode::Expanded:
        return "expanded";
    case SidebarMode::Compact:
        return "compact";
    case SidebarMode::Hidden:
        return "hidden";
    }
    return {};
}
struct Identity {
    string name = "default", nick = "blastirc", username = "blastirc", realname = "BlastIRC";
    string password, certificate, key;
};
struct ServerConfig {
    string name = "Libera", host = "irc.libera.chat";
    string alias, avatar;
    int port = 6697;
    bool tls = true;
    string identity = "default", sasl, password, bouncer = "none";
    vector<string> channels;
    bool sojuAccount = false, sojuEnabled = true;
    string sojuParent, sojuNetworkId, sojuTrustedFileHost;
};
struct Cue {
    bool enabled = true;
    float volume = .5f, pitch = 1.f;
};
struct Settings {
    UploadConfig uploads;
    FxLevel fx = FxLevel::Subtle;
    SendMotion sendMotion = SendMotion::Flight;
    SendReveal sendReveal = SendReveal::Pixels;
    SendMotion incomingMotion = SendMotion::None;
    SendReveal incomingReveal = SendReveal::Fade;
    float incomingDurationMs = 550.f;
    FoolDisplay foolDisplay = FoolDisplay::Minimized;
    bool foolUnread = false;
    int messageWrapColumns = 80;
    PreviewMode linkPreviews = PreviewMode::Click, imagePreviews = PreviewMode::Click,
                mediaPreviews = PreviewMode::Click;
    int previewMaxHeight = 120, previewMediaMaxMiB = 64, previewMediaMaxSeconds = 600;
    bool previewAllowLocal = false;
    PresenceMode presence = PresenceMode::Smart;
    float opacity = 1.f, scale = 1.f, messageScale = 1.f, masterVolume = .5f, gravity = 80.f,
          drag = 1.f, shake = 1.f;
    bool reducedMotion = false, sound = true, typingSend = true, typingReceive = true;
    bool timestamps = true, avatars = true, transparency = false;
    int retentionDays = 30, maxMessages = 100000;
    string uiFont, messageFont, nickPalette = "THEME", avatarStyle = "sprite";
    string themeSource = "auto", pitchVariation = "natural";
    bool spellcheck = true;
    string spellcheckLanguage = "auto";
    string cursorShape = "line", cursorEffect = "off";
    float cursorTrailDurationMs = 120.f, cursorTrailStrength = .20f;
    SidebarMode peopleMode = SidebarMode::Hidden;
    map<string, Cue> cues;
    map<string, string> bindings, nickColors, workspace;
    set<string> friends, fools, ircChannels;
    vector<string> pinnedBuffers; // Full buffer IDs, newest pin first.
    map<string, PresenceMode> channelPresence;
    map<string, bool> bufferFools;
};
inline PresenceMode effectivePresenceMode(const Settings &settings, const string &bufferId,
                                          const string &target) {
    auto mode = settings.channelPresence.find(bufferId);
    if (mode == settings.channelPresence.end())
        mode = settings.channelPresence.find(target);
    return mode == settings.channelPresence.end() || mode->second == PresenceMode::Global
               ? settings.presence
               : mode->second;
}
inline FoolDisplay effectiveFoolDisplay(const Settings &settings, const string &bufferId,
                                        const string &target) {
    auto found = settings.bufferFools.find(bufferId);
    if (found == settings.bufferFools.end())
        found = settings.bufferFools.find(target);
    return found == settings.bufferFools.end() ? settings.foolDisplay
           : found->second                     ? FoolDisplay::Shown
                                               : FoolDisplay::Hidden;
}
// Shared ingestion/backfill classifier; case mapping follows the source network.
bool actualNickMention(std::string_view text, std::string_view nick, std::string_view caseMapping);
inline constexpr const char *GlobalMentionsBufferId = "!global-mentions";
// Sent means the full IRC frame was written, not delivered or read by a recipient.
enum class SendStatus { None, Queued, Sent, Confirmed, Unknown };
enum class ActivityKind { None, Join, Part, Quit, Nick };
struct Message {
    string id, nick, text, time, reply, label, activityTarget;
    string sourceBuffer, sourceId, sourceServer, sourceTarget;
    int64_t storageSequence = 0;
    int64_t orderingTimestamp = 0;
    int64_t timestamp = 0;
    double arrival = 0;
    SendStatus sendStatus = SendStatus::None;
    ActivityKind activity = ActivityKind::None;
    bool own = false, history = false, pending = false, deleted = false, mention = false,
         action = false, presence = false;
    map<string, vector<string>> reactions;
};
struct Nick {
    string name, account, prefix;
    bool away = false;
    double arrived = 0;
};
struct ChatActivityKey {
    int64_t timestamp = 0, sequence = 0;
    string time;
};
struct Buffer {
    string id, server, target, title, kind = "channel", draft, topic, readMarker;
    // Live channel metadata and its validated private cache file; never persisted.
    string avatar, avatarPath;
    // Runtime origin for repeating a search from this network's result buffer.
    string searchSource;
    int64_t lastChatTimestamp = 0, lastChatSequence = 0;
    string lastChatTime;
    // Newly received replay rows are excluded only during live-session repairs.
    set<int64_t> chatReplaySequences;
    string lastChatId;
    // Historical own confirmations may replace a row without changing its live activity key.
    map<string, ChatActivityKey> replayChatKeys;
    std::deque<Message> messages;
    // Runtime lookup cache rebuilt with the retained mention snapshot.
    map<string, size_t> sourceMessageIndex;
    // Non-append identity/order token; genuine tail appends leave it unchanged.
    uint64_t messagesRevision = 0;
    // Transient Smart-presence invalidation when raw participation evidence changes.
    uint64_t participationRevision = 0;
    vector<Nick> nicks;
    map<string, double> typing;
    int unread = 0;
    bool loading = false, joined = false;
    bool muted = false, archived = false;
};
// Durable chat ordering, independent of presence, unread state, and pin rank.
inline bool chatActivityBefore(const Buffer &a, const Buffer &b) {
    if (a.lastChatTimestamp != b.lastChatTimestamp)
        return a.lastChatTimestamp > b.lastChatTimestamp;
    const auto at = irc::parseTimestampParts(a.lastChatTime);
    const auto bt = irc::parseTimestampParts(b.lastChatTime);
    const auto an = at ? at->nanoseconds : 0, bn = bt ? bt->nanoseconds : 0;
    if (an != bn)
        return an > bn;
    if (a.lastChatSequence != b.lastChatSequence)
        return a.lastChatSequence > b.lastChatSequence;
    return a.id < b.id;
}
inline void recordChatActivity(Buffer &buffer, int64_t timestamp, int64_t sequence,
                               const string &serverTime, const string &id) {
    if (buffer.kind != "channel" && buffer.kind != "query")
        return;
    const auto time = irc::parseTimestampParts(serverTime);
    const auto previous = irc::parseTimestampParts(buffer.lastChatTime);
    const auto fraction = time ? time->nanoseconds : 0;
    const auto oldFraction = previous ? previous->nanoseconds : 0;
    if (timestamp < buffer.lastChatTimestamp ||
        (timestamp == buffer.lastChatTimestamp &&
         (fraction < oldFraction ||
          (fraction == oldFraction && sequence <= buffer.lastChatSequence))))
        return;
    buffer.lastChatTimestamp = timestamp;
    buffer.lastChatSequence = sequence;
    buffer.lastChatTime = serverTime;
    buffer.lastChatId = id;
}
inline void recordChatActivity(Buffer &buffer, const Message &message) {
    if ((buffer.kind != "channel" && buffer.kind != "query") || message.presence || message.deleted)
        return;
    recordChatActivity(
        buffer, message.orderingTimestamp > 0 ? message.orderingTimestamp : message.timestamp,
        message.storageSequence, message.time, message.id);
}
struct SojuNetworkInfo {
    string id;
    map<string, string> attributes;
};
struct Network {
    string name, state = "Disconnected", nick, error, clientTagDeny;
    double latency = 0;
    set<string> caps;
    map<string, SojuNetworkInfo> sojuNetworks;
    bool sojuDiscoveryComplete = false, sojuMutationPending = false;
    string fileHost, sojuMutationError, sojuMutationCommand, sojuMutationId;
    uint64_t sessionGeneration = 0;
    bool saslAuthenticated = false;
    string sojuSessionKey;
    bool sojuMutationSucceeded = false;
    string caseMapping = "rfc1459";
    string channelTypes = "#&+!";
};
struct Event {
    string type, server, target, nick, text, id, time, reply, label;
    int64_t timestamp = 0;
    bool history = false, own = false;
    map<string, string> tags;
    vector<string> params;
    bool silent = false;
};
struct Action {
    string type, buffer, text, extra;
    uint64_t requestId = 0;
    map<string, string> attributes{};
};
struct SettingsSaveResult {
    uint64_t requestId = 0;
    bool success = false;
    string error;
};
struct WhoisRecord {
    string nick, username, host, realName, account, server, serverInfo, away, idle, signon,
        channels;
    vector<string> details;
};
struct WhoisProfile {
    string requestId, server, target, error;
    uint64_t sessionGeneration = 0, revision = 0;
    bool open = false, loading = false;
    vector<WhoisRecord> records;
};
struct SourceJumpRequest {
    uint64_t revision = 0;
    string buffer, message;
};
struct Model {
    vector<Identity> identities;
    vector<ServerConfig> servers;
    Settings settings;
    map<string, Buffer> buffers;
    map<string, Network> networks;
    string activeBuffer, status;
    bool firstRun = true, quit = false;
    double now = 0;
    vector<Event> effects;
    vector<Action> actions;
    uint64_t settingsSaveRequest = 0;
    SettingsSaveResult settingsSaveResult;
    WhoisProfile whoisProfile;
    map<string, Buffer> mentionSources;
    SourceJumpRequest pendingSourceJump;
    // Synchronous native identity callbacks; absent in headless core clients.
    // Called before a rename, with the source intact and destination optional.
    // Observation only: do not mutate buffers or dispatch core actions here.
    std::function<void(const string &, const string &)> bufferWillRename;
    // Called after core migration, before same-tick persistence. Equal IDs mean
    // a case-only presentation change, never a document/pane replacement.
    std::function<void(const string &, const string &)> bufferRenamed;
};
// Prefer live state, including tombstones, over the retained-source snapshot.
inline const Message *sourceMessage(const Model &model, const string &buffer, const string &id) {
    auto live = model.buffers.find(buffer);
    if (live != model.buffers.end()) {
        const auto indexed = live->second.sourceMessageIndex.find(id);
        if (indexed != live->second.sourceMessageIndex.end() &&
            indexed->second < live->second.messages.size()) {
            const auto &message = live->second.messages[indexed->second];
            if (message.id == id)
                return message.deleted ? nullptr : &message;
        }
        for (const auto &message : live->second.messages)
            if (message.id == id)
                return message.deleted ? nullptr : &message;
    }
    auto source = model.mentionSources.find(buffer);
    if (source != model.mentionSources.end()) {
        const auto indexed = source->second.sourceMessageIndex.find(id);
        if (indexed != source->second.sourceMessageIndex.end() &&
            indexed->second < source->second.messages.size()) {
            const auto &message = source->second.messages[indexed->second];
            if (message.id == id)
                return message.deleted ? nullptr : &message;
        }
        if (source->second.sourceMessageIndex.empty())
            for (const auto &message : source->second.messages)
                if (message.id == id)
                    return message.deleted ? nullptr : &message;
    }
    return nullptr;
}
inline const Buffer *resolveMessageSource(const Model &model, const Buffer &rowBuffer,
                                          const Message &rowMessage) {
    const bool aggregate = rowBuffer.kind == "mentions";
    if (aggregate &&
        (rowMessage.sourceBuffer.empty() || rowMessage.sourceId.empty() ||
         rowMessage.sourceServer.empty() || rowMessage.sourceTarget.empty() || rowMessage.own ||
         rowMessage.presence || rowMessage.deleted || !rowMessage.mention))
        return nullptr;
    if (!aggregate && (!rowMessage.sourceBuffer.empty() || !rowMessage.sourceId.empty() ||
                       !rowMessage.sourceServer.empty() || !rowMessage.sourceTarget.empty()))
        return nullptr;
    const auto &id = aggregate ? rowMessage.sourceBuffer : rowBuffer.id;
    const auto &messageId = aggregate ? rowMessage.sourceId : rowMessage.id;
    const auto *original = sourceMessage(model, id, messageId);
    if (!original || original->deleted || original->nick != rowMessage.nick ||
        original->text != rowMessage.text || original->reply != rowMessage.reply)
        return nullptr;
    const Buffer *context = nullptr;
    auto live = model.buffers.find(id);
    if (live != model.buffers.end() && live->second.kind != "mentions")
        context = &live->second;
    else if (auto source = model.mentionSources.find(id); source != model.mentionSources.end())
        context = &source->second;
    if (!context || (aggregate && (context->server != rowMessage.sourceServer ||
                                   context->target != rowMessage.sourceTarget ||
                                   (context->kind != "channel" && context->kind != "query") ||
                                   original->own || original->presence || !original->mention)))
        return nullptr;
    return context;
}
inline bool isFool(const Model &model, std::string_view server, std::string_view nick) {
    if (nick.empty() || model.settings.fools.empty())
        return false;
    const auto network = model.networks.find(string(server));
    const std::string_view mapping = network == model.networks.end()
                                         ? std::string_view{"rfc1459"}
                                         : std::string_view{network->second.caseMapping};
    const auto folded = irc::casefold(nick, mapping);
    return std::any_of(model.settings.fools.begin(), model.settings.fools.end(),
                       [&](const auto &entry) { return irc::casefold(entry, mapping) == folded; });
}
} // namespace blast
