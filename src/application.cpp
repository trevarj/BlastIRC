#include "application.hpp"
#include "commands.hpp"
#include "irc/protocol.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <charconv>
#include <chrono>
#include <curl/curl.h>
#include <iostream>
#include <limits>
#include <openssl/sha.h>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

namespace blast {
namespace {
int64_t epoch() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
string tag(const Event &e, const string &name) {
    auto i = e.tags.find(name);
    return i == e.tags.end() ? string{} : i->second;
}
bool flag(const Event &e, const string &name) {
    auto value = tag(e, name);
    return value == "1" || value == "true";
}
string upper(string value) {
    for (char &c : value)
        if (c >= 'a' && c <= 'z')
            c -= 32;
    return value;
}
bool whoisNumeric(const string &command) {
    return command == "276" || command == "301" || command == "307" || command == "310" ||
           command == "311" || command == "312" || command == "313" || command == "317" ||
           command == "318" || command == "319" || command == "320" || command == "330" ||
           command == "335" || command == "338" || command == "378" || command == "379" ||
           command == "671";
}
string lower(string value) {
    for (char &c : value)
        if (c >= 'A' && c <= 'Z')
            c += 32;
    return value;
}
std::pair<string, string> word(const string &text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == string::npos)
        return {};
    auto end = text.find_first_of(" \t", first);
    if (end == string::npos)
        return {text.substr(first), {}};
    auto rest = text.find_first_not_of(" \t", end);
    return {text.substr(first, end - first), rest == string::npos ? string{} : text.substr(rest)};
}
vector<string> commaWords(const string &value) {
    vector<string> result;
    size_t start = 0;
    while (start < value.size()) {
        auto end = value.find(',', start);
        if (end == string::npos)
            end = value.size();
        if (end > start)
            result.push_back(value.substr(start, end - start));
        start = end + 1;
    }
    return result;
}
int64_t markerTime(const string &value) {
    const auto parsed = irc::parseTimestampParts(value);
    return parsed ? parsed->seconds : 0;
}
int markerFraction(const string &value) {
    const auto parsed = irc::parseTimestampParts(value);
    return parsed ? parsed->nanoseconds : 0;
}
string eventBody(const Event &e) {
    if (!e.text.empty())
        return e.text;
    string out = tag(e, "command");
    for (const auto &p : e.params) {
        if (!out.empty())
            out += ' ';
        out += p;
    }
    return out;
}
bool unsafeLine(const string &text) {
    return text.find_first_of("\r\n") != string::npos || text.find('\0') != string::npos;
}
bool submissionPart(std::string_view label, std::string_view submission) {
    if (label == submission)
        return true;
    if (!label.starts_with(submission))
        return false;
    label.remove_prefix(submission.size());
    return label.starts_with(".part") && label.size() > 5 &&
           std::all_of(label.begin() + 5, label.end(),
                       [](unsigned char c) { return std::isdigit(c); });
}
std::string_view submissionRoot(std::string_view label) {
    const auto part = label.rfind(".part");
    return part != std::string_view::npos && submissionPart(label, label.substr(0, part))
               ? label.substr(0, part)
               : label;
}
} // namespace

string sojuSessionKey(const ServerConfig &config, const Identity &identity) {
    string serialized;
    auto field = [&](const string &value) {
        serialized += std::to_string(value.size()) + ":" + value;
    };
    for (const auto *value :
         {&config.name, &config.host, &config.identity, &config.sasl, &config.password,
          &config.bouncer, &config.sojuParent, &config.sojuNetworkId, &config.sojuTrustedFileHost,
          &identity.username, &identity.password, &identity.nick, &identity.realname,
          &identity.certificate, &identity.key})
        field(*value);
    field(std::to_string(config.port));
    field(config.tls ? "tls" : "plain");
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(serialized.data()), serialized.size(), digest);
    return string(reinterpret_cast<const char *>(digest), sizeof(digest));
}
std::optional<SojuFileHostContext> sojuFileHostContext(const Model &model, const string &source) {
    auto child = std::find_if(model.servers.begin(), model.servers.end(),
                              [&](const auto &s) { return s.name == source; });
    if (child == model.servers.end() || child->sojuParent.empty() || !child->sojuEnabled)
        return {};
    auto parent = std::find_if(model.servers.begin(), model.servers.end(), [&](const auto &s) {
        return s.name == child->sojuParent && s.sojuAccount;
    });
    auto live = model.networks.find(source);
    auto control = model.networks.find(child->sojuParent);
    if (parent == model.servers.end() || live == model.networks.end() ||
        control == model.networks.end() || live->second.state != "Ready" ||
        control->second.state != "Ready" || !live->second.saslAuthenticated ||
        !control->second.saslAuthenticated || live->second.fileHost.empty())
        return {};
    auto identity = std::find_if(model.identities.begin(), model.identities.end(),
                                 [&](const auto &i) { return i.name == parent->identity; });
    if (identity == model.identities.end() || identity->username.empty() ||
        identity->password.empty() || identity->username.find_first_of("/:\r\n") != string::npos ||
        unsafeLine(identity->username))
        return {};
    ServerConfig resolved = *child;
    resolved.host = parent->host;
    resolved.port = parent->port;
    resolved.tls = parent->tls;
    resolved.identity = parent->identity;
    resolved.sasl = parent->sasl;
    resolved.password = parent->password;
    resolved.bouncer = "soju:" + child->sojuNetworkId;
    resolved.sojuTrustedFileHost = parent->sojuTrustedFileHost;
    if (control->second.sojuSessionKey != sojuSessionKey(*parent, *identity) ||
        live->second.sojuSessionKey != sojuSessionKey(resolved, *identity))
        return {};
    CURLU *url = curl_url();
    if (!url)
        return {};
    const auto cleanup = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>(url, curl_url_cleanup);
    if (curl_url_set(url, CURLUPART_URL, live->second.fileHost.c_str(), 0) != CURLUE_OK)
        return {};
    auto part = [&](CURLUPart key) {
        char *value = nullptr;
        string result;
        if (curl_url_get(url, key, &value, 0) == CURLUE_OK) {
            result = value;
            curl_free(value);
        }
        return result;
    };
    auto hasPart = [&](CURLUPart key) {
        char *value = nullptr;
        const bool present = curl_url_get(url, key, &value, 0) == CURLUE_OK;
        curl_free(value);
        return present;
    };
    auto hostname = [](string value) {
        value = lower(std::move(value));
        if (value.size() > 2 && value.front() == '[' && value.back() == ']')
            value = value.substr(1, value.size() - 2);
        return value;
    };
    const string host = hostname(part(CURLUPART_HOST)), physical = hostname(parent->host);
    if (part(CURLUPART_SCHEME) != "https" || host.empty() || physical.empty() ||
        hasPart(CURLUPART_USER) || hasPart(CURLUPART_PASSWORD) || hasPart(CURLUPART_FRAGMENT) ||
        std::any_of(live->second.fileHost.begin(), live->second.fileHost.end(),
                    [](unsigned char c) { return c < 32 || c == 127; }))
        return {};
    unsigned char address[16];
    const bool ip = inet_pton(AF_INET, physical.c_str(), address) == 1 ||
                    inet_pton(AF_INET6, physical.c_str(), address) == 1 ||
                    (!physical.empty() && physical.front() == '[');
    const bool trusted =
        host == physical ||
        (!ip && host.size() > physical.size() && host.ends_with("." + physical)) ||
        (!parent->sojuTrustedFileHost.empty() && host == hostname(parent->sojuTrustedFileHost));
    if (!trusted)
        return {};
    return SojuFileHostContext{source,
                               parent->name,
                               child->sojuNetworkId,
                               parent->host,
                               identity->username,
                               live->second.fileHost,
                               parent->sojuTrustedFileHost,
                               identity->password,
                               live->second.sessionGeneration,
                               control->second.sessionGeneration};
}
bool sojuFileHostContextCurrent(const Model &model, const SojuFileHostContext &context) {
    const auto current = sojuFileHostContext(model, context.source);
    return current && *current == context;
}

Application::Application() {
    const auto serial = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    idPrefix_ = "local:" + std::to_string(serial) + ":" + std::to_string(getpid()) + ":";
    try {
        storage_.load(model);
    } catch (const std::exception &e) {
        savingAllowed_ = false;
        report(string("Configuration was not loaded; original files will not be overwritten: ") +
               e.what());
    }
    adoptSojuProfiles();
    for (const auto &s : model.servers) {
        auto &n = model.networks[s.name];
        n.name = s.name;
        for (const auto &identity : model.identities)
            if (identity.name == s.identity) {
                n.nick = identity.nick;
                break;
            }
        buffer(s.name, "*server*", "server");
        for (const auto &target : s.channels)
            buffer(s.name, target);
    }
    for (auto &[id, b] : model.buffers) {
        b.id = id;
        reviseMessages(b);
        b.joined = false;
        b.loading = false;
        b.typing.clear();
        b.nicks.clear();
        for (auto &m : b.messages)
            m.history = true;
        // Only the five most recent timestamps per nick are needed for SMART.
        try {
            for (const auto &m :
                 storage_.history(id, std::clamp(model.settings.maxMessages, 1, 100000)))
                activity(b, m);
        } catch (const std::exception &e) {
            report(e.what(), b.server);
        }
        recount(b);
    }
    messageCount_ = 0;
    typingEnabled_ = model.settings.typingSend;
    for (const auto &[id, b] : model.buffers)
        if (b.kind != "search")
            messageCount_ += b.messages.size();
    set<string> interrupted;
    for (const auto &[id, b] : model.buffers)
        if (std::any_of(b.messages.begin(), b.messages.end(),
                        [](const auto &m) { return m.own && m.pending; }))
            interrupted.insert(b.server);
    for (const auto &server : interrupted) {
        Event failed;
        failed.server = server;
        failed.tags["unconfirmed"] = "1";
        failSends(failed, "Previous session ended before confirmation");
    }
    trim();
    if (model.activeBuffer.empty() && !model.buffers.empty())
        model.activeBuffer = model.buffers.begin()->first;
    if (savingAllowed_ && !model.firstRun) {
        vector<string> initial;
        for (const auto &s : model.servers)
            if (s.sojuParent.empty() && s.sojuEnabled)
                initial.push_back(s.name);
        for (const auto &name : initial)
            connect(name);
    }
}
Application::~Application() noexcept {
    shuttingDown_ = true;
    for (auto &[name, c] : clients_) {
        try {
            c->stop();
        } catch (...) {
        }
    }
    drainClients();
    for (const auto &[server, c] : clients_) {
        Event failed;
        failed.server = server;
        failed.tags["unconfirmed"] = "1";
        failed.tags["queued-unconfirmed"] = "1";
        try {
            failSends(failed, "Application closed before the send reached pending history");
        } catch (const std::exception &error) {
            std::cerr << "BlastIRC: " << error.what() << '\n';
        }
    }
    try {
        save();
    } catch (...) {
    } // Destruction must not mask an earlier shutdown failure.
}
string Application::fold(const string &server, const string &value) const {
    auto mapping = caseMapping_.find(server);
    return irc::casefold(value, mapping == caseMapping_.end() ? "rfc1459" : mapping->second);
}
bool Application::channel(const string &server, const string &target) const {
    auto types = channelTypes_.find(server);
    return !target.empty() &&
           (types == channelTypes_.end() ? string("#&+!") : types->second).find(target[0]) !=
               string::npos;
}
bool Application::visible(const Buffer &b) const {
    const auto i = model.settings.workspace.find("visible_buffers");
    return (i == model.settings.workspace.end() ? model.activeBuffer : i->second) == b.id;
}
Buffer &Application::buffer(const string &server, const string &target, const string &kind) {
    string id = server + "/" + fold(server, target);
    auto it = model.buffers.find(id);
    if (it != model.buffers.end() && fold(server, it->second.target) != fold(server, target))
        it = model.buffers.end();
    if (it == model.buffers.end()) {
        it = std::find_if(model.buffers.begin(), model.buffers.end(), [&](const auto &entry) {
            return entry.second.server == server &&
                   fold(server, entry.second.target) == fold(server, target);
        });
    }
    if (it == model.buffers.end()) {
        if (model.buffers.contains(id))
            throw std::runtime_error("Conflicting IRC buffer identifier: " + id);
        Buffer value;
        value.id = id;
        value.server = server;
        value.target = target;
        value.title = target;
        value.kind = kind.empty() ? (channel(server, target) ? "channel" : "query") : kind;
        if (savingAllowed_)
            storage_.seedChatActivity(value);
        reviseMessages(value);
        it = model.buffers.emplace(id, std::move(value)).first;
    }
    return it->second;
}
void Application::repairChatActivity(Buffer &b) {
    for (auto &[id, key] : b.replayChatKeys)
        if (savingAllowed_)
            if (auto stored = storage_.message(b.id, id)) {
                key.sequence = stored->storageSequence;
                b.chatReplaySequences.insert(key.sequence);
            }
    if (savingAllowed_)
        storage_.seedChatActivity(b, false);
    else {
        b.lastChatTimestamp = b.lastChatSequence = 0;
        b.lastChatTime.clear();
        b.lastChatId.clear();
    }
    for (auto &row : b.messages) {
        if (row.history || b.replayChatKeys.contains(row.id))
            continue;
        if (savingAllowed_ && row.storageSequence > 0)
            if (auto stored = storage_.message(b.id, row.id)) {
                row.storageSequence = stored->storageSequence;
                row.orderingTimestamp = stored->orderingTimestamp;
                recordChatActivity(b, *stored);
                continue;
            }
        recordChatActivity(b, row);
    }
    for (const auto &[id, key] : b.replayChatKeys)
        recordChatActivity(b, key.timestamp, key.sequence, key.time, id);
}
void Application::reviseMessages(Buffer &b) {
    b.messagesRevision = ++messageRevision_;
    if (b.kind != "mentions")
        ++mentionsLiveRevision_;
}
void Application::clearAvatar(Buffer &b, bool metadata) {
    auto state = channelAvatars_.find(b.id);
    if (state != channelAvatars_.end()) {
        if (state->second.request)
            avatarLoader_.cancel(state->second.request);
        channelAvatars_.erase(state);
    }
    if (!b.avatarPath.empty())
        avatarLoader_.remove(b.avatarPath);
    b.avatarPath.clear();
    if (metadata)
        b.avatar.clear();
}
void Application::updateAvatars() {
    bool capacity = avatarLoader_.hasCapacity();
    for (auto &[id, b] : model.buffers) {
        const auto network = model.networks.find(b.server);
        const bool eligible = model.settings.avatars && b.kind == "channel" && b.joined &&
                              network != model.networks.end() &&
                              network->second.caps.contains("batch") &&
                              network->second.caps.contains("draft/metadata-2");
        if (!eligible) {
            clearAvatar(b, false);
            continue;
        }
        if (b.avatar.empty() || channelAvatars_.contains(id))
            continue;
        if (!capacity)
            continue; // Admission pressure defers work; failed HTTP requests are not retried.
        auto &state = channelAvatars_[id];
        state.url = b.avatar;
        state.request = ++avatarSequence_;
        switch (avatarLoader_.request(state.request, state.url)) {
        case AvatarLoader::Admission::Full:
            channelAvatars_.erase(id);
            capacity = false;
            break;
        case AvatarLoader::Admission::Invalid:
            state.request = 0;
            break;
        case AvatarLoader::Admission::Queued:
            capacity = avatarLoader_.hasCapacity();
            break;
        }
    }
    for (auto &result : avatarLoader_.poll())
        finishAvatar(std::move(result));
}
void Application::finishAvatar(AvatarLoader::Result result) {
    auto state =
        std::find_if(channelAvatars_.begin(), channelAvatars_.end(), [&](const auto &entry) {
            return result.request && entry.second.request == result.request;
        });
    auto b = state == channelAvatars_.end() ? nullptr : findBuffer(state->first);
    const auto network = b ? model.networks.find(b->server) : model.networks.end();
    if (b && b->avatar == state->second.url && b->joined && b->kind == "channel" &&
        model.settings.avatars && network != model.networks.end() &&
        network->second.caps.contains("batch") &&
        network->second.caps.contains("draft/metadata-2")) {
        b->avatarPath = std::move(result.path);
        state->second.request = 0;
    } else if (!result.path.empty())
        avatarLoader_.remove(result.path);
}
void Application::renameBuffer(string oldId, string target) {
    auto old = model.buffers.find(oldId);
    if (old == model.buffers.end())
        return;
    const string server = old->second.server, previous = old->second.target;
    const string id = server + "/" + fold(server, target);
    const bool room = old->second.kind == "channel";
    auto joinIntent = [&] {
        if (!room)
            return;
        for (auto &config : model.servers) {
            if (config.name != server)
                continue;
            bool held =
                fold(server, previous) != fold(server, target) &&
                std::any_of(config.channels.begin(), config.channels.end(), [&](const auto &saved) {
                    return fold(server, saved) == fold(server, target);
                });
            for (auto channel = config.channels.begin(); channel != config.channels.end();) {
                if (fold(server, *channel) != fold(server, previous)) {
                    ++channel;
                } else if (held) {
                    channel = config.channels.erase(channel);
                } else {
                    *channel++ = target;
                    held = true;
                }
            }
        }
    };
    if (model.bufferWillRename)
        model.bufferWillRename(oldId, id);
    if (oldId == id) {
        old->second.target = target;
        old->second.title = target;
        ++mentionsLiveRevision_;
        joinIntent();
        if (model.bufferRenamed)
            model.bufferRenamed(oldId, id);
        return;
    }
    if (savingAllowed_)
        storage_.renameBuffer(oldId, id);
    auto source = std::move(old->second);
    model.buffers.erase(old);
    for (auto &[bufferId, retained] : model.buffers)
        if (retained.searchSource == oldId)
            retained.searchSource = id;
    auto dest = model.buffers.find(id);
    if (dest == model.buffers.end()) {
        source.id = id;
        source.target = target;
        source.title = target;
        dest = model.buffers.emplace(id, std::move(source)).first;
    } else {
        auto &b = dest->second;
        b.chatReplaySequences.merge(source.chatReplaySequences);
        for (auto &[messageId, key] : source.replayChatKeys) {
            const bool liveDuplicate =
                !b.replayChatKeys.contains(messageId) &&
                std::any_of(b.messages.begin(), b.messages.end(),
                            [&](const auto &row) { return row.id == messageId && !row.history; });
            if (!liveDuplicate) {
                auto [entry, inserted] = b.replayChatKeys.try_emplace(messageId, std::move(key));
                if (!inserted &&
                    (key.timestamp > entry->second.timestamp ||
                     (key.timestamp == entry->second.timestamp &&
                      (markerFraction(key.time) > markerFraction(entry->second.time) ||
                       (markerFraction(key.time) == markerFraction(entry->second.time) &&
                        key.sequence > entry->second.sequence)))))
                    entry->second = std::move(key);
            }
        }
        if (source.joined || !b.joined) {
            clearAvatar(b);
            b.avatar = std::move(source.avatar);
            b.avatarPath = std::move(source.avatarPath);
        } else
            clearAvatar(source);
        {
            std::map<std::string_view, Message *> seen;
            for (auto &m : b.messages)
                seen.emplace(m.id, &m);
            reviseMessages(b);
            for (auto &m : source.messages) {
                auto duplicate = seen.find(m.id);
                if (duplicate != seen.end()) {
                    auto &keep = *duplicate->second;
                    keep.own = keep.own || m.own;
                    keep.pending = keep.pending && m.pending;
                    if (m.sendStatus == SendStatus::Confirmed ||
                        (keep.sendStatus != SendStatus::Confirmed &&
                         (m.sendStatus == SendStatus::Sent ||
                          (keep.sendStatus != SendStatus::Sent &&
                           m.sendStatus == SendStatus::Queued) ||
                          (keep.sendStatus == SendStatus::None &&
                           m.sendStatus == SendStatus::Unknown))))
                        keep.sendStatus = m.sendStatus;
                    if (!m.history || !keep.history)
                        b.chatReplaySequences.erase(keep.storageSequence);
                    if (!m.history && !source.replayChatKeys.contains(m.id))
                        b.replayChatKeys.erase(m.id);
                    if (keep.activity == ActivityKind::None)
                        keep.activity = m.activity;
                    if (keep.timestamp == 0) {
                        keep.timestamp = m.timestamp;
                        keep.time = m.time;
                    }
                    if (keep.reply.empty())
                        keep.reply = m.reply;
                    if (keep.label.empty())
                        keep.label = m.label;
                    if (keep.text.empty())
                        keep.text = std::move(m.text);
                    keep.deleted = keep.deleted || m.deleted;
                    if (keep.deleted) {
                        keep.text.clear();
                        keep.reactions.clear();
                    } else
                        for (const auto &[emoji, nicks] : m.reactions)
                            for (const auto &nick : nicks) {
                                auto &users = keep.reactions[emoji];
                                if (std::none_of(users.begin(), users.end(), [&](const auto &user) {
                                        return fold(server, user) == fold(server, nick);
                                    }))
                                    users.push_back(nick);
                            }
                    if (b.kind != "search")
                        --messageCount_;
                    continue;
                }
                b.messages.push_back(std::move(m));
                seen.emplace(b.messages.back().id, &b.messages.back());
            }
        }
        std::stable_sort(b.messages.begin(), b.messages.end(), [](const auto &a, const auto &b) {
            if (a.timestamp != b.timestamp)
                return a.timestamp < b.timestamp;
            return markerFraction(a.time) < markerFraction(b.time);
        });
        restoreDraft(b, source.draft);
        if (b.readMarker.empty() || markerTime(source.readMarker) > markerTime(b.readMarker) ||
            (markerTime(source.readMarker) == markerTime(b.readMarker) &&
             markerFraction(source.readMarker) > markerFraction(b.readMarker)))
            b.readMarker = source.readMarker;
        if (source.joined) {
            b.topic = std::move(source.topic);
            b.nicks = std::move(source.nicks);
            b.typing = std::move(source.typing);
        } else {
            for (auto &nick : source.nicks)
                if (std::none_of(b.nicks.begin(), b.nicks.end(), [&](const auto &n) {
                        return fold(server, n.name) == fold(server, nick.name);
                    }))
                    b.nicks.push_back(std::move(nick));
            for (const auto &[nick, time] : source.typing)
                b.typing[nick] = std::max(b.typing[nick], time);
        }
        b.joined = b.joined || source.joined;
        b.loading = b.loading || source.loading;
        b.muted = b.muted || source.muted;
        b.archived = b.archived && source.archived;
        b.target = target;
        b.title = target;
    }
    auto moveKey = [&](auto &values) {
        auto node = values.extract(oldId);
        if (!node.empty()) {
            node.key() = id;
            values.erase(id);
            values.insert(std::move(node));
        }
    };
    moveKey(channelAvatars_);
    moveKey(namesCache_);
    moveKey(gaps_);
    moveKey(model.settings.channelPresence);
    moveKey(model.settings.bufferFools);
    const bool pinsChanged =
        std::find(model.settings.pinnedBuffers.begin(), model.settings.pinnedBuffers.end(),
                  oldId) != model.settings.pinnedBuffers.end();
    auto &pins = model.settings.pinnedBuffers;
    bool pinned = false;
    std::erase_if(pins, [&](string &pin) {
        if (pin != oldId && pin != id)
            return false;
        if (pinned)
            return true;
        pin = id;
        pinned = true;
        return false;
    });
    if (auto unsaved = unsavedSources_.find(oldId); unsaved != unsavedSources_.end()) {
        auto source = std::move(unsaved->second);
        unsavedSources_.erase(unsaved);
        auto &destination = unsavedSources_[id];
        destination.id = id;
        destination.server = server;
        destination.target = target;
        destination.title = target;
        destination.kind = source.kind;
        for (auto &row : source.messages)
            if (std::none_of(destination.messages.begin(), destination.messages.end(),
                             [&](const auto &other) { return row.id == other.id; }))
                destination.messages.push_back(std::move(row));
    }
    ++mentionsLiveRevision_;
    if (model.settings.ircChannels.erase(oldId))
        model.settings.ircChannels.insert(id);
    if (requestedHistory_.erase(oldId))
        requestedHistory_.insert(id);
    if (namesInProgress_.erase(oldId))
        namesInProgress_.insert(id);
    for (auto i = pending_.begin(); i != pending_.end();) {
        if (i->second.buffer != oldId) {
            ++i;
            continue;
        }
        auto node = pending_.extract(i++);
        node.mapped().buffer = id;
        node.key() = id + "\n" + node.mapped().id;
        pending_.insert(std::move(node));
    }
    for (auto &[label, queued] : queuedSends_)
        if (queued.buffer == oldId)
            queued.buffer = id;
    for (auto i = activity_.begin(); i != activity_.end();) {
        if (!i->first.starts_with(oldId + "\n")) {
            ++i;
            continue;
        }
        auto node = activity_.extract(i++);
        auto &times = activity_[id + node.key().substr(oldId.size())];
        times.insert(times.end(), node.mapped().begin(), node.mapped().end());
        std::sort(times.begin(), times.end());
        while (times.size() > 5)
            times.pop_front();
    }
    for (auto &[label, route] : activeHistory_)
        if (route == oldId)
            route = id;
    for (auto &[command, origin] : commandOrigins_)
        if (origin.buffer == oldId)
            origin.buffer = id;
    for (auto &[key, value] : model.settings.workspace) {
        if (value == oldId &&
            (key == "visible_buffers" || key.starts_with("last_buffer:") ||
             (!key.empty() && std::all_of(key.begin(), key.end(),
                                          [](unsigned char c) { return c >= '0' && c <= '9'; }))))
            value = id;
    }
    for (auto &a : model.actions)
        if (a.buffer == oldId)
            a.buffer = id;
    for (size_t i = nextAction_; i < activeActions_.size(); ++i)
        if (activeActions_[i].buffer == oldId)
            activeActions_[i].buffer = id;
    if (model.activeBuffer == oldId)
        model.activeBuffer = id;
    if (model.pendingSourceJump.buffer == oldId) {
        model.pendingSourceJump.buffer.clear();
        model.pendingSourceJump.message.clear();
        ++model.pendingSourceJump.revision;
    }
    for (auto &[key, group] : presenceGroups_) {
        for (auto &e : group.events)
            if (e.server == server && fold(server, e.target) == fold(server, previous))
                e.target = target;
    }
    for (auto &e : model.effects)
        if (e.server == server && fold(server, e.target) == fold(server, previous))
            e.target = target;
    recount(dest->second);
    repairChatActivity(dest->second);
    joinIntent();
    if (model.bufferRenamed)
        model.bufferRenamed(oldId, id);
    if (pinsChanged && savingAllowed_)
        storage_.saveSettings(model.settings);
}
Buffer *Application::findBuffer(const string &id) {
    auto i = model.buffers.find(id.empty() ? model.activeBuffer : id);
    return i == model.buffers.end() ? nullptr : &i->second;
}
NetworkClient *Application::client(const string &server) {
    auto i = clients_.find(server);
    return i == clients_.end() ? nullptr : i->second.get();
}
string Application::localId() { return idPrefix_ + std::to_string(++sequence_); }
void Application::adoptSojuProfiles() {
    for (auto &s : model.servers) {
        if (s.bouncer != "soju" || !s.sojuParent.empty())
            continue;
        const auto identity = std::find_if(model.identities.begin(), model.identities.end(),
                                           [&](const auto &i) { return i.name == s.identity; });
        if (identity != model.identities.end() && identity->username.find('/') == string::npos)
            s.sojuAccount = true;
    }
    for (auto &s : model.servers) {
        if (!s.sojuParent.empty() || !s.bouncer.starts_with("soju:"))
            continue;
        vector<string> matches;
        for (const auto &parent : model.servers)
            if (parent.sojuAccount && parent.name != s.name && parent.host == s.host &&
                parent.port == s.port && parent.tls == s.tls && parent.identity == s.identity &&
                parent.sasl == s.sasl && parent.password == s.password)
                matches.push_back(parent.name);
        if (matches.size() != 1)
            continue;
        const auto duplicates =
            std::count_if(model.servers.begin(), model.servers.end(), [&](const auto &other) {
                return other.bouncer == s.bouncer && other.host == s.host && other.port == s.port &&
                       other.tls == s.tls && other.identity == s.identity && other.sasl == s.sasl &&
                       other.password == s.password;
            });
        if (duplicates != 1)
            continue;
        const string id = s.bouncer.substr(5);
        const bool occupied =
            std::any_of(model.servers.begin(), model.servers.end(), [&](const auto &other) {
                return other.name != s.name && other.sojuParent == matches.front() &&
                       other.sojuNetworkId == id;
            });
        if (!occupied) {
            s.sojuParent = matches.front();
            s.sojuNetworkId = id;
            s.password.clear();
        }
    }
}
void Application::disconnect(string server) {
    sojuSuspended_.insert(server);
    vector<string> children;
    for (const auto &s : model.servers)
        if (s.sojuParent == server)
            children.push_back(s.name);
    children.push_back(server);
    for (const auto &name : children) {
        stopClient(name);
        clients_.erase(name);
        auto runtime = model.networks.find(name);
        if (runtime != model.networks.end()) {
            auto &n = runtime->second;
            n.state = "Disconnected";
            n.error.clear();
            n.fileHost.clear();
            n.saslAuthenticated = false;
            n.sojuSessionKey.clear();
            n.sojuDiscoveryComplete = false;
            n.sojuMutationSucceeded = false;
            if (n.sojuMutationPending)
                n.sojuMutationError = "Disconnected before management acknowledgement";
            n.sojuMutationPending = false;
            ++n.sessionGeneration;
            if (model.whoisProfile.server == name)
                finishWhois("Connection closed before WHOIS completed");
        }
        sojuMutations_.erase(name);
        sojuCatalogSeen_.erase(name);
        sojuCatalogBatch_.erase(name);
    }
}
void Application::retireSoju(const string &parent, const string &id) {
    vector<string> retired;
    for (const auto &s : model.servers)
        if (s.sojuParent == parent && s.sojuNetworkId == id)
            retired.push_back(s.name);
    for (const auto &name : retired) {
        sojuRetired_.insert(name);
        disconnect(name);
        model.networks.erase(name);
        std::erase_if(model.servers, [&](const auto &s) { return s.name == name; });
    }
}
void Application::synchronizeSoju(const string &parent) {
    if (shuttingDown_ || sojuSuspended_.contains(parent))
        return;
    auto config = std::find_if(model.servers.begin(), model.servers.end(),
                               [&](const auto &s) { return s.name == parent && s.sojuAccount; });
    auto live = model.networks.find(parent);
    if (config == model.servers.end() || live == model.networks.end() ||
        live->second.state != "Ready" || !live->second.sojuDiscoveryComplete)
        return;
    auto identity = std::find_if(model.identities.begin(), model.identities.end(),
                                 [&](const auto &i) { return i.name == config->identity; });
    if (identity == model.identities.end() ||
        live->second.sojuSessionKey != sojuSessionKey(*config, *identity))
        return;
    vector<string> attach;
    for (const auto &[id, info] : live->second.sojuNetworks) {
        auto child = std::find_if(model.servers.begin(), model.servers.end(), [&](const auto &s) {
            return s.sojuParent == parent && s.sojuNetworkId == id;
        });
        if (child == model.servers.end()) {
            ServerConfig created;
            do {
                created.name = "soju-child-" + localId();
            } while (std::any_of(model.servers.begin(), model.servers.end(),
                                 [&](const auto &s) { return s.name == created.name; }) ||
                     model.networks.contains(created.name));
            created.host.clear();
            created.identity.clear();
            created.sojuParent = parent;
            created.sojuNetworkId = id;
            created.bouncer = "soju:" + id;
            model.servers.push_back(std::move(created));
            child = std::prev(model.servers.end());
        }
        const auto name = info.attributes.find("name");
        child->alias = name == info.attributes.end() ? string{} : name->second;
        if (child->sojuEnabled && !client(child->name))
            attach.push_back(child->name);
    }
    for (const auto &name : attach)
        connect(name);
}
void Application::connect(string name, string origin) {
    sojuSuspended_.erase(name);
    auto config = std::find_if(model.servers.begin(), model.servers.end(),
                               [&](const auto &s) { return s.name == name; });
    if (config == model.servers.end()) {
        report("Unknown server: " + name, name, origin);
        return;
    }
    ServerConfig resolved = *config;
    if (resolved.sojuAccount) {
        vector<string> children;
        for (const auto &s : model.servers)
            if (s.sojuParent == name)
                children.push_back(s.name);
        for (const auto &child : children)
            disconnect(child);
    }
    if (!resolved.sojuParent.empty()) {
        if (!resolved.sojuEnabled)
            return;
        auto parent = std::find_if(model.servers.begin(), model.servers.end(), [&](const auto &s) {
            return s.name == resolved.sojuParent && s.sojuAccount;
        });
        if (parent == model.servers.end())
            return;
        const auto control = model.networks.find(parent->name);
        if (control == model.networks.end() || control->second.state != "Ready" ||
            !control->second.sojuDiscoveryComplete ||
            !control->second.sojuNetworks.contains(resolved.sojuNetworkId))
            return;
        auto parentIdentity =
            std::find_if(model.identities.begin(), model.identities.end(),
                         [&](const auto &i) { return i.name == parent->identity; });
        if (parentIdentity == model.identities.end() ||
            control->second.sojuSessionKey != sojuSessionKey(*parent, *parentIdentity))
            return;
        resolved.host = parent->host;
        resolved.port = parent->port;
        resolved.tls = parent->tls;
        resolved.identity = parent->identity;
        resolved.sasl = parent->sasl;
        resolved.password = parent->password;
        resolved.bouncer = "soju:" + resolved.sojuNetworkId;
        resolved.sojuTrustedFileHost = parent->sojuTrustedFileHost;
    }
    auto identity = std::find_if(model.identities.begin(), model.identities.end(),
                                 [&](const auto &i) { return i.name == resolved.identity; });
    if (identity == model.identities.end()) {
        report("Unknown identity: " + resolved.identity, name, origin);
        return;
    }
    if (client(name))
        disconnect(name);
    sojuSuspended_.erase(name);
    auto &n = model.networks[name];
    n.name = name;
    n.nick = identity->nick;
    n.error.clear();
    n.caps.clear();
    n.clientTagDeny.clear();
    ++n.sessionGeneration;
    n.fileHost.clear();
    n.saslAuthenticated = false;
    n.sojuDiscoveryComplete = false;
    n.sojuNetworks.clear();
    sojuCatalogSeen_.erase(name);
    sojuCatalogBatch_.erase(name);
    n.sojuMutationSucceeded = false;
    n.sojuSessionKey = sojuSessionKey(resolved, *identity);
    buffer(name, "*server*", "server");
    try {
        auto c = std::make_unique<NetworkClient>(resolved, *identity);
        c->start();
        clients_[name] = std::move(c);
    } catch (const std::exception &e) {
        n.state = "Failed";
        n.error = e.what();
        report(n.error, name, origin);
    }
}
void Application::stopClient(string server) {
    if (auto *c = client(server)) {
        c->stop();
        for (const auto &e : c->poll())
            try {
                reduce(e);
            } catch (const std::exception &error) {
                report(error.what(), server);
            }
        Event failed;
        failed.server = server;
        failed.tags["unconfirmed"] = "1";
        failed.tags["queued-unconfirmed"] = "1";
        failSends(failed, "Connection closed before the send reached pending history");
    }
}
void Application::report(const string &text, const string &server, const string &origin) {
    model.status = text;
    string name = server;
    if (name.empty() && !model.servers.empty())
        name = model.servers.front().name;
    auto destination = origin.empty() ? nullptr : findBuffer(origin);
    if (destination && destination->kind == "mentions")
        destination = nullptr;
    auto &b = destination ? *destination : buffer(name, "*server*", "server");
    Message m;
    m.id = localId();
    m.nick = "BlastIRC";
    m.text = text;
    m.timestamp = epoch();
    m.arrival = model.now;
    m.presence = true;
    b.messages.push_back(std::move(m));
    if (b.kind != "search")
        ++messageCount_;
}
bool Application::store(const Buffer &b, Message &m, bool update, const string &completedSend,
                        bool confirmed) {
    if (b.kind == "search" || b.kind == "mentions")
        return false;
    ++mentionsLiveRevision_;
    auto remember = [&] {
        // Own-send recovery is backed by queued_sends, not the incoming-source retry cache.
        if (m.own)
            return;
        auto &source = unsavedSources_[b.id];
        source.id = b.id;
        source.server = b.server;
        source.target = b.target;
        source.title = b.title;
        source.kind = b.kind;
        std::erase_if(source.messages, [&](const auto &row) { return row.id == m.id; });
        source.messages.push_back(m);
    };
    if (!savingAllowed_) {
        remember();
        return false;
    }
    try {
        const size_t parts = completedSend.empty() ? 0 : queuedSends_.at(completedSend).parts;
        const bool retired =
            update
                ? storage_.update(b.id, m, completedSend, parts, confirmed, &b, &m.storageSequence)
                : storage_.append(b.id, m, completedSend, parts, confirmed, &b, &m.storageSequence);
        if (auto source = unsavedSources_.find(b.id); source != unsavedSources_.end())
            std::erase_if(source->second.messages, [&](const auto &row) { return row.id == m.id; });
        return completedSend.empty() || retired;
    } catch (const std::exception &e) {
        remember();
        report(e.what(), b.server);
        return false;
    }
}
void Application::restoreDraft(Buffer &b, const string &text, bool distinctSubmission) {
    // Separate submissions may equal the new draft; only buffer-merging callers deduplicate text.
    if (text.empty() || (!distinctSubmission && b.draft == text))
        return;
    b.draft = b.draft.empty() ? text : text + "\n" + b.draft;
}
bool Application::recoverQueuedSends(const Event &e, const string &reason) {
    vector<string> labels;
    for (const auto &[label, queued] : queuedSends_) {
        const auto *b = findBuffer(queued.buffer);
        if (b && b->server == e.server &&
            (e.label.empty()
                 ? (flag(e, "queued-unconfirmed") || (queued.preserveRaw && flag(e, "unconfirmed")))
                 : submissionPart(e.label, label)))
            labels.push_back(label);
    }
    // restoreDraft prepends: recover newest first to retain original submission order.
    std::sort(labels.begin(), labels.end(), [&](const auto &a, const auto &b) {
        return queuedSends_.at(a).order > queuedSends_.at(b).order;
    });
    for (const auto &label : labels) {
        auto &queued = queuedSends_.at(label);
        auto &b = *findBuffer(queued.buffer);
        if (!queued.recovered) {
            restoreDraft(b, queued.text, true);
            queued.recovered = true;
            bool revised = false;
            for (auto m = b.messages.begin(); m != b.messages.end();) {
                if (!m->own || !m->id.starts_with("local:") || !submissionPart(m->label, label)) {
                    ++m;
                    continue;
                }
                failedLabels_[m->label] = model.now;
                pending_.erase(b.id + "\n" + m->id);
                std::erase_if(model.effects, [&](const auto &effect) {
                    return effect.id == m->id && effect.server == b.server;
                });
                if (!revised) {
                    reviseMessages(b);
                    revised = true;
                }
                m = b.messages.erase(m);
                --messageCount_;
            }
            failedLabels_[label] = model.now;
            failedSubmissions_[label] = model.now;
        }
        if (savingAllowed_)
            try {
                storage_.recoverSend(b, label);
                queuedSends_.erase(label);
            } catch (const std::exception &error) {
                report(error.what(), b.server);
            }
        else
            queuedSends_.erase(label);
        model.status = "Send failed: " + reason;
    }
    return !labels.empty();
}
string Application::confirmQueuedSend(const Buffer &b, const string &label) {
    if (label.empty())
        return {};
    for (auto &[submission, queued] : queuedSends_)
        if (!queued.recovered && queued.buffer == b.id && submissionPart(label, submission)) {
            queued.confirmed.insert(label);
            return queued.complete && queued.parts && queued.confirmed.size() == queued.parts
                       ? submission
                       : string{};
        }
    return {};
}
bool Application::failedSubmission(const string &label) const {
    return !label.empty() &&
           std::any_of(failedSubmissions_.begin(), failedSubmissions_.end(),
                       [&](const auto &failed) { return submissionPart(label, failed.first); });
}
bool Application::failSends(const Event &e, const string &reason) {
    if (!e.label.empty() && (failedLabels_.contains(e.label) || failedSubmission(e.label)))
        return true;
    bool restored = recoverQueuedSends(e, reason);
    for (auto &[id, b] : model.buffers)
        if (b.server == e.server && b.kind != "search") {
            vector<Message> failed;
            for (auto m = b.messages.begin(); m != b.messages.end();) {
                const bool match =
                    m->own && m->id.starts_with("local:") &&
                    (!e.label.empty() ? m->label == e.label
                                      : flag(e, "unconfirmed") &&
                                            (m->pending || m->sendStatus == SendStatus::Queued));
                if (!match) {
                    ++m;
                    continue;
                }
                if (failed.empty())
                    reviseMessages(b);
                failed.push_back(std::move(*m));
                m = b.messages.erase(m);
                --messageCount_;
            }
            if (failed.empty())
                continue;
            restored = true;
            string recovered;
            for (const auto &m : failed) {
                if (!recovered.empty())
                    recovered += '\n';
                const auto original = e.tags.find("original-text");
                recovered += m.label == e.label && original != e.tags.end()
                                 ? original->second
                                 : (m.action ? "/me " : "") + m.text;
            }
            restoreDraft(b, recovered, true);
            string cleanupError;
            for (const auto &m : failed) {
                pending_.erase(id + "\n" + m.id);
                if (!m.label.empty())
                    failedLabels_[m.label] = model.now;
                std::erase_if(model.effects, [&](const auto &effect) {
                    return effect.id == m.id && effect.server == b.server;
                });
                if (savingAllowed_)
                    try {
                        storage_.eraseMessage(id, m.id);
                    } catch (const std::exception &error) {
                        cleanupError = error.what();
                    }
            }
            Message notice;
            notice.id = localId();
            notice.nick = "BlastIRC";
            notice.presence = true;
            notice.timestamp = epoch();
            notice.arrival = model.now;
            notice.text = "Send not confirmed; text restored to draft — " + reason;
            insert(b, std::move(notice));
            model.status = "Send failed: " + reason;
            if (!cleanupError.empty())
                report("Failed-send cleanup: " + cleanupError, b.server);
        }
    return restored;
}
void Application::effect(Event e, const Buffer &b) {
    if (e.history || !visible(b) || (!e.own && isFool(model, b.server, e.nick)))
        return;
    if (b.kind == "server" && (e.type == "message" || e.type == "notice" || e.type == "action"))
        return;
    e.target = b.target;
    e.server = b.server;
    e.silent = e.silent || (b.muted && e.type != "state");
    model.effects.push_back(std::move(e));
}
void Application::activity(const Buffer &b, const Message &m) {
    if (m.presence || m.deleted || m.timestamp <= 0 || m.nick.empty())
        return;
    auto &times = activity_[b.id + "\n" + fold(b.server, m.nick)];
    auto p = std::upper_bound(times.begin(), times.end(), m.timestamp);
    times.insert(p, m.timestamp);
    while (times.size() > 5)
        times.pop_front();
}
PresenceMode Application::presenceMode(const Buffer &b) const {
    return effectivePresenceMode(model.settings, b.id, b.target);
}
bool Application::presenceVisible(const Buffer &b, const Event &e) const {
    if (flag(e, "force-presence"))
        return true;
    auto net = model.networks.find(b.server);
    if (e.own ||
        (net != model.networks.end() && fold(b.server, e.nick) == fold(b.server, net->second.nick)))
        return true;
    const auto mode = presenceMode(b);
    if (mode == PresenceMode::All)
        return true;
    if (mode == PresenceMode::Hidden)
        return false;
    auto i = activity_.find(b.id + "\n" + fold(b.server, e.nick));
    if (i == activity_.end() || i->second.empty())
        return false;
    const auto at = e.timestamp > 0 ? e.timestamp : epoch();
    int count = 0;
    for (auto time : i->second) {
        if (time <= at && at - time <= 300)
            return true;
        if (time <= at && at - time <= 7 * 86400)
            ++count;
    }
    return count >= 5;
}
void Application::presence(Buffer &b, Event e, const string &text, bool emit) {
    const bool eligible = presenceVisible(b, e);
    if (!eligible && presenceMode(b) != PresenceMode::Smart)
        return;
    Message m;
    m.id = e.id.empty() ? localId() : e.id;
    m.nick = e.nick;
    m.text = text;
    m.time = e.time;
    m.timestamp = e.timestamp > 0 ? e.timestamp : (e.history ? 0 : epoch());
    m.arrival = model.now;
    m.history = e.history;
    m.own = e.own;
    m.presence = true;
    if (e.type == "join")
        m.activity = ActivityKind::Join;
    else if (e.type == "part")
        m.activity = ActivityKind::Part;
    else if (e.type == "quit")
        m.activity = ActivityKind::Quit;
    else if (e.type == "nick") {
        m.activity = ActivityKind::Nick;
        m.activityTarget = e.text;
    }
    e.id = m.id;
    insert(b, std::move(m));
    if (emit && eligible)
        effect(std::move(e), b);
}
bool Application::insert(Buffer &b, Message m, bool persist, const string &completedSend,
                         bool confirmed) {
    ++mentionsLiveRevision_;
    if (!m.id.empty() && std::any_of(b.messages.begin(), b.messages.end(),
                                     [&](const auto &old) { return old.id == m.id; }))
        return false;
    if (m.history && m.timestamp > 0 &&
        std::any_of(b.messages.begin(), b.messages.end(), [&](const auto &old) {
            return !old.pending && old.timestamp == m.timestamp && old.time == m.time &&
                   fold(b.server, old.nick) == fold(b.server, m.nick) && old.text == m.text &&
                   old.action == m.action && old.presence == m.presence &&
                   old.activity == m.activity &&
                   (old.id.starts_with("local:") || m.id.starts_with("local:"));
        }))
        return false;
    if (m.id.empty())
        m.id = localId();
    bool durableReplay = false;
    if (savingAllowed_ && persist && !m.id.starts_with("local:")) {
        try {
            if (auto old = storage_.message(b.id, m.id)) {
                durableReplay = true;
                m.mention = m.mention || old->mention;
                m.storageSequence = old->storageSequence;
                m.orderingTimestamp = old->orderingTimestamp;
                m.reactions = old->reactions;
                if (old->deleted) {
                    m.deleted = true;
                    m.text.clear();
                    m.reactions.clear();
                }
            }
        } catch (const std::exception &e) {
            report(e.what(), b.server);
        }
    }
    if (auto source = unsavedSources_.find(b.id); source != unsavedSources_.end())
        for (const auto &row : source->second.messages)
            if (row.id == m.id && row.deleted) {
                m.deleted = true;
                m.text.clear();
                m.reactions.clear();
                break;
            }
    if (m.orderingTimestamp <= 0)
        m.orderingTimestamp = m.timestamp > 0 ? m.timestamp : epoch();
    const bool stored = !persist || store(b, m, false, completedSend, confirmed);
    if (persist && m.history && !m.presence && !m.deleted && !durableReplay && stored &&
        m.storageSequence > 0 && (b.kind == "channel" || b.kind == "query"))
        b.chatReplaySequences.insert(m.storageSequence);
    if (!m.history && !durableReplay)
        recordChatActivity(b, m);
    activity(b, m);
    bool appendOnly = true;
    if (m.history) {
        auto pos = b.messages.begin();
        if (m.timestamp > 0) {
            while (
                pos != b.messages.end() &&
                ((pos->history && pos->timestamp == 0) ||
                 (pos->timestamp > 0 && (pos->timestamp < m.timestamp ||
                                         (pos->timestamp == m.timestamp &&
                                          markerFraction(pos->time) <= markerFraction(m.time))))))
                ++pos;
        } else {
            while (pos != b.messages.end() && pos->history && pos->timestamp == 0)
                ++pos;
        }
        appendOnly = pos == b.messages.end();
        b.messages.insert(pos, std::move(m));
    } else
        b.messages.push_back(std::move(m));
    if (b.kind == "search") {
        const auto limit = static_cast<size_t>(std::clamp(model.settings.maxMessages, 1, 2000));
        if (b.messages.size() > limit)
            appendOnly = false;
        while (b.messages.size() > limit)
            b.messages.pop_front();
    } else
        ++messageCount_;
    if (!appendOnly)
        reviseMessages(b);
    recount(b);
    return stored;
}
void Application::recount(Buffer &b) {
    const auto time = markerTime(b.readMarker);
    bool after = b.readMarker.empty();
    if (!time && !after) {
        const string id =
            b.readMarker.starts_with("local:") ? b.readMarker.substr(6) : b.readMarker;
        after = std::none_of(b.messages.begin(), b.messages.end(),
                             [&](const auto &m) { return m.id == id || m.id == b.readMarker; });
    }
    b.unread = 0;
    for (const auto &m : b.messages) {
        if (!time && (m.id == b.readMarker || "local:" + m.id == b.readMarker)) {
            after = true;
            continue;
        }
        const bool newer =
            time ? (m.timestamp > time ||
                    (m.timestamp == time && markerFraction(m.time) > markerFraction(b.readMarker)))
                 : after;
        if (newer && !m.own && !m.presence &&
            (model.settings.foolUnread || !isFool(model, b.server, m.nick)))
            ++b.unread;
    }
}
void Application::receive(Event e) {
    auto &network = model.networks[e.server];
    if (!e.history && (e.own || flag(e, "pending")) && failedSubmission(e.label))
        return;
    if (flag(e, "search")) {
        auto &results = buffer(e.server, "*search*", "search");
        Message m;
        m.id = e.id.empty() ? localId() : "search:" + fold(e.server, e.target) + ":" + e.id;
        m.nick = e.nick;
        m.text = "[" + e.target + "] " + e.nick + ": " + e.text;
        m.time = e.time;
        m.timestamp = e.timestamp;
        m.arrival = model.now;
        m.history = true;
        m.presence = true;
        insert(results, std::move(m), false);
        return;
    }
    auto routedQueue =
        flag(e, "pending") ? queuedSends_.find(tag(e, "submission")) : queuedSends_.end();
    if (routedQueue == queuedSends_.end() && e.own && !e.history && !e.label.empty())
        routedQueue = std::find_if(queuedSends_.begin(), queuedSends_.end(), [&](const auto &item) {
            return submissionPart(e.label, item.first);
        });
    if (routedQueue != queuedSends_.end())
        if (const auto *routed = findBuffer(routedQueue->second.buffer);
            routed && routed->server == e.server)
            e.target = routed->target;
    if (!e.label.empty())
        for (const auto &[key, p] : pending_)
            if (p.label == e.label) {
                if (auto routed = findBuffer(p.buffer))
                    e.target = routed->target;
                break;
            }
    if (e.target.empty() || e.target == "*")
        e.target = "*server*";
    if (e.type == "notice" && e.nick.find('.') != string::npos && !channel(e.server, e.target))
        e.target = "*server*";
    if (fold(e.server, e.target) == fold(e.server, network.nick) && !e.own && !e.nick.empty())
        e.target = e.nick;
    auto &b = buffer(e.server, e.target, e.target == "*server*" ? "server" : "");
    e.own =
        e.own || (!network.nick.empty() && fold(e.server, e.nick) == fold(e.server, network.nick));
    if (e.history) {
        auto gap = gaps_.find(b.id);
        if (gap != gaps_.end() && gap->second.active &&
            (gap->second.label.empty() || gap->second.label == e.label)) {
            ++gap->second.received;
            if (!e.id.empty())
                gap->second.next = "msgid=" + e.id;
            else if (!e.time.empty())
                gap->second.next = "timestamp=" + e.time;
        }
    }
    const bool pending = flag(e, "pending");
    const string submission = pending ? tag(e, "submission") : string{};
    auto queued = pending ? queuedSends_.find(submission) : queuedSends_.end();
    auto retry =
        pending && !e.label.empty()
            ? std::find_if(b.messages.begin(), b.messages.end(),
                           [&](const auto &m) {
                               return m.own && m.id.starts_with("local:") && m.label == e.label;
                           })
            : b.messages.end();
    if (retry != b.messages.end() && queued == queuedSends_.end() && !submission.empty() &&
        e.label == submission && !flag(e, "submission-complete")) {
        // The server's existing multiline fallback is about to replace the full original.
        const string actionText = retry->action ? "/me " + retry->text : string{};
        const auto pendingOriginal = pending_.find(b.id + "\n" + retry->id);
        const auto *raw = pendingOriginal != pending_.end() && pendingOriginal->second.originalText
                              ? &*pendingOriginal->second.originalText
                              : nullptr;
        const string &original = raw ? *raw : retry->action ? actionText : retry->text;
        queued = queuedSends_.emplace(submission, QueuedSend{b.id, original, ++sequence_}).first;
        queued->second.preserveRaw = raw != nullptr;
        try {
            if (savingAllowed_)
                storage_.queueSend(b, submission, original);
        } catch (const std::exception &error) {
            Event failed;
            failed.server = b.server;
            failed.label = submission;
            recoverQueuedSends(failed, error.what());
            report(error.what(), b.server);
            return;
        }
    }
    const bool complete = queued != queuedSends_.end() && flag(e, "submission-complete");
    if (queued != queuedSends_.end()) {
        queued->second.complete = complete;
        if (complete) {
            const auto count = tag(e, "submission-parts");
            const auto parsed =
                std::from_chars(count.data(), count.data() + count.size(), queued->second.parts);
            if (parsed.ec != std::errc{} || parsed.ptr != count.data() + count.size())
                queued->second.parts = 0;
        }
    }
    string completedSend =
        complete && queued->second.stored && queued->second.parts && !queued->second.preserveRaw
            ? queued->first
            : string{};
    if (retry != b.messages.end()) {
        retry->text = e.text;
        retry->pending = true;
        if (retry->sendStatus != SendStatus::Confirmed)
            retry->sendStatus = SendStatus::Queued;
        retry->action = e.type == "action";
        retry->reply = e.reply;
        pending_[b.id + "\n" + retry->id] = {b.id, retry->id, model.now, e.label};
        if (const auto raw = e.tags.find("original-text"); raw != e.tags.end())
            pending_.at(b.id + "\n" + retry->id).originalText = raw->second;
        const bool stored = store(b, *retry, true, completedSend);
        if (queued != queuedSends_.end()) {
            queued->second.stored &= stored;
            if (stored && !completedSend.empty())
                queuedSends_.erase(queued);
        }
        return;
    }
    if (pending)
        e.id = localId();
    if (e.own && !pending) {
        auto old = std::find_if(b.messages.begin(), b.messages.end(), [&](const auto &m) {
            const bool waiting = pending_.contains(b.id + "\n" + m.id);
            if (!m.own)
                return false;
            const bool sameId = !e.id.empty() && e.id == m.id;
            if (!waiting && !(m.id.starts_with("local:") && !m.label.empty()) && !sameId)
                return false;
            return (!e.label.empty() && e.label == m.label) || sameId ||
                   (!e.history && waiting && e.label.empty() && e.text == m.text &&
                    (e.type == "action") == m.action && model.now - m.arrival <= 30);
        });
        if (old != b.messages.end()) {
            const bool wasRead = b.readMarker == old->id || b.readMarker == "local:" + old->id;
            const bool correctedActivity =
                old->id == b.lastChatId || old->pending || b.replayChatKeys.contains(old->id);
            const string previousId = old->id;
            auto replayKey = b.replayChatKeys.extract(previousId);
            const bool previouslyEligible =
                (b.kind == "channel" || b.kind == "query") &&
                (!old->history || (old->storageSequence > 0 &&
                                   !b.chatReplaySequences.contains(old->storageSequence)));
            if (e.history && (!replayKey.empty() || previouslyEligible)) {
                if (replayKey.empty()) {
                    const auto timestamp =
                        old->orderingTimestamp > 0 ? old->orderingTimestamp : old->timestamp;
                    b.replayChatKeys.emplace(
                        previousId, ChatActivityKey{timestamp, old->storageSequence, old->time});
                    replayKey = b.replayChatKeys.extract(previousId);
                }
                replayKey.key() = e.id.empty() ? previousId : e.id;
                b.replayChatKeys.insert(std::move(replayKey));
            }
            pending_.erase(b.id + "\n" + old->id);
            const bool idChanged = !e.id.empty() && old->id != e.id;
            const bool labelChanged = !e.history && !e.label.empty() && old->label != e.label;
            if (idChanged || labelChanged)
                reviseMessages(b);
            if (idChanged)
                old->id = e.id;
            if (labelChanged)
                old->label = e.label;
            old->pending = false;
            old->sendStatus = SendStatus::Confirmed;
            if (e.timestamp > 0)
                old->timestamp = e.timestamp;
            if (e.timestamp > 0)
                old->orderingTimestamp = e.timestamp;
            if (!e.time.empty())
                old->time = e.time;
            old->text = e.text;
            old->reply = e.reply.empty() ? old->reply : e.reply;
            e.id = old->id;
            e.type = "echo";
            if (wasRead)
                b.readMarker = old->time.empty() ? "local:" + old->id : old->time;
            const string completed = confirmQueuedSend(b, old->label);
            const bool stored = store(b, *old, true, completed, true);
            if (e.history && old->storageSequence > 0)
                b.chatReplaySequences.insert(old->storageSequence);
            if (e.history && b.lastChatId == previousId)
                b.lastChatId = old->id;
            if (!e.history)
                b.chatReplaySequences.erase(old->storageSequence);
            if (!e.history && (idChanged || correctedActivity))
                repairChatActivity(b);
            if (stored && !completed.empty())
                queuedSends_.erase(completed);
            if (wasRead) {
                if (savingAllowed_)
                    try {
                        storage_.read(b.id, b.readMarker);
                    } catch (const std::exception &error) {
                        report(error.what(), b.server);
                    }
                if (markerTime(b.readMarker) > 0)
                    if (auto c = client(b.server))
                        c->markRead(b.target, b.readMarker);
            }
            effect(e, b);
            recount(b);
            return;
        }
        completedSend = confirmQueuedSend(b, e.label);
    }
    if (!e.id.empty() && std::any_of(b.messages.begin(), b.messages.end(),
                                     [&](const auto &m) { return m.id == e.id; }))
        return;
    bool restoreArchive = b.archived && !b.muted && !e.history && !e.own &&
                          !isFool(model, b.server, e.nick) &&
                          (b.kind == "channel" || b.kind == "query");
    if (restoreArchive && savingAllowed_ && !e.id.empty())
        try {
            restoreArchive = !storage_.message(b.id, e.id).has_value();
        } catch (const std::exception &error) {
            report(error.what(), b.server);
            restoreArchive = false;
        }
    if (restoreArchive)
        b.archived = false;
    Message m;
    m.id = e.id.empty() ? localId() : e.id;
    m.nick = e.nick;
    m.text = e.text;
    m.time = e.time;
    m.reply = e.reply;
    m.label = e.label;
    m.timestamp = e.timestamp > 0 ? e.timestamp : (e.history ? 0 : epoch());
    m.arrival = model.now;
    m.own = e.own;
    m.history = e.history;
    m.pending = pending;
    m.sendStatus = !m.own ? SendStatus::None : pending ? SendStatus::Queued : SendStatus::Confirmed;
    m.action = e.type == "action";
    const bool foolish = !m.own && isFool(model, e.server, m.nick);
    if (!m.own && b.kind != "server")
        m.mention = actualNickMention(m.text, network.nick, network.caseMapping);
    const string id = m.id;
    const bool mention = m.mention;
    const auto previousUnread = b.unread;
    if (pending)
        pending_[b.id + "\n" + id] = {b.id, id, model.now, e.label};
    if (pending)
        if (const auto raw = e.tags.find("original-text"); raw != e.tags.end())
            pending_.at(b.id + "\n" + id).originalText = raw->second;
    const bool stored = insert(b, std::move(m), true, completedSend, e.own && !pending);
    if (queued != queuedSends_.end()) {
        queued->second.stored &= stored;
        if (stored && !completedSend.empty())
            queuedSends_.erase(queued);
    } else if (stored && !completedSend.empty())
        queuedSends_.erase(completedSend);
    if (restoreArchive)
        save();
    std::erase_if(b.typing, [&](const auto &item) {
        return fold(e.server, item.first) == fold(e.server, e.nick);
    });
    if (!e.history && !b.messages.empty() && b.messages.back().id == id &&
        b.messages.back().deleted)
        return;
    e.id = id;
    if (pending)
        e.type = "send";
    else if (mention && !foolish)
        e.type = "mention";
    effect(e, b);
    if (mention && !foolish && !b.muted && !e.history && !e.own) {
        auto notification = e;
        notification.type = "notify";
        model.effects.push_back(std::move(notification));
    }
    if (!foolish && !e.history && b.unread > previousUnread && !visible(b)) {
        // Badges are drawn by the UI; hidden conversations never create overlay FX/sounds.
        model.status = "New message in " + b.target;
    }
}
void Application::groupPresence(Event e, bool joins, bool explicitBatch) {
    const string key = e.server + "\n" + (joins ? "join\n" : "quit\n") +
                       (tag(e, "batch").empty() ? e.text : tag(e, "batch"));
    auto expired = presenceGroups_.find(key);
    if (expired != presenceGroups_.end() && model.now - expired->second.started >= 10) {
        if (!expired->second.emitted)
            flushPresence(expired->second, false);
        presenceGroups_.erase(expired);
    }
    auto [it, created] = presenceGroups_.try_emplace(key);
    auto &group = it->second;
    if (created) {
        group.started = model.now;
        group.joins = joins;
    }
    group.explicitBatch = group.explicitBatch || explicitBatch;
    auto &b = buffer(e.server, e.target);
    if (presenceMode(b) != PresenceMode::Hidden) {
        if (e.id.empty())
            e.id = localId();
        presence(b, e, e.nick + (joins ? " joined " : " quit: ") + (joins ? b.target : e.text),
                 false);
    }
    group.events.push_back(std::move(e));
    set<string> nicks;
    for (const auto &event : group.events)
        nicks.insert(fold(event.server, event.nick));
    if (group.explicitBatch || nicks.size() >= 3)
        flushPresence(group, true);
}
void Application::flushPresence(PresenceGroup &group, bool aggregate) {
    if (group.emitted)
        return;
    for (const auto &e : group.events) {
        auto &b = buffer(e.server, e.target);
        if (!e.own && isFool(model, b.server, e.nick))
            continue;
        if (aggregate) {
            if (presenceMode(b) == PresenceMode::Hidden || e.history || !visible(b))
                continue;
            auto event = e;
            event.type = group.joins ? "join" : "netsplit";
            effect(event, b);
            break;
        }
        if (presenceMode(b) == PresenceMode::Hidden)
            presence(b, e,
                     e.nick + (group.joins ? " joined " : " quit: ") +
                         (group.joins ? b.target : e.text));
        else if (presenceVisible(b, e))
            effect(e, b);
    }
    group.emitted = true;
}
void Application::reduce(const Event &input) {
    if (sojuRetired_.contains(input.server))
        return;
    Event e = input;
    const bool routedCommand = commandResponse(e);
    if (e.type == "network" && tag(e, "kind") == "command-complete")
        return;
    auto &n = model.networks[e.server];
    n.name = e.server;
    if (e.type == "sent") {
        if (e.label.empty() || failedLabels_.contains(e.label) || failedSubmission(e.label))
            return;
        // Labels include the originating client/submission session. Never match text
        // or visual pending state: either can also describe a different send.
        for (auto &[id, b] : model.buffers)
            if (b.server == e.server)
                for (auto &m : b.messages)
                    if (m.own && m.label == e.label && m.sendStatus == SendStatus::Queued)
                        m.sendStatus = SendStatus::Sent;
        if (tag(e, "echo-expected") == "0")
            for (auto queued = queuedSends_.begin(); queued != queuedSends_.end(); ++queued) {
                auto &send = queued->second;
                if (!send.preserveRaw || !submissionPart(e.label, queued->first))
                    continue;
                send.written.insert(e.label);
                if (!send.complete || !send.parts || send.written.size() != send.parts)
                    break;
                auto *chat = findBuffer(send.buffer);
                if (!chat)
                    break;
                bool stored = true;
                for (auto &message : chat->messages)
                    if (message.own && submissionPart(message.label, queued->first)) {
                        message.pending = false;
                        stored &= store(*chat, message, true);
                    }
                if (stored) {
                    if (savingAllowed_)
                        storage_.recoverSend(*chat, queued->first);
                    queuedSends_.erase(queued);
                }
                break;
            }
        return;
    }
    if ((e.history || e.type == "history_end") && !flag(e, "search") && !e.label.empty()) {
        auto route = activeHistory_.find(e.label);
        if (route != activeHistory_.end())
            if (auto b = findBuffer(route->second))
                e.target = b->target;
    }
    if (e.type == "message" || e.type == "action" || e.type == "notice") {
        receive(std::move(e));
        return;
    }
    if (e.type == "state") {
        const string old = n.state;
        n.state = e.text;
        n.error = tag(e, "error");
        if (!e.nick.empty())
            n.nick = e.nick;
        if (!tag(e, "nick").empty())
            n.nick = tag(e, "nick");
        if (e.text == "Connecting" || e.text == "Disconnected" || e.text == "Failed") {
            ++n.sessionGeneration;
            if (model.whoisProfile.server == e.server)
                finishWhois("Connection changed before WHOIS completed");
            std::erase_if(commandOrigins_,
                          [&](const auto &item) { return item.second.server == e.server; });
            n.fileHost.clear();
            n.saslAuthenticated = false;
            n.sojuDiscoveryComplete = false;
            n.sojuMutationSucceeded = false;
            sojuCatalogSeen_.erase(e.server);
            sojuCatalogBatch_.erase(e.server);
            if (n.sojuMutationPending) {
                n.sojuMutationPending = false;
                n.sojuMutationError = "Connection changed before management acknowledgement";
            }
            sojuMutations_.erase(e.server);
        }
        if (e.text == "Connecting") {
            n.sojuNetworks.clear();
            n.caps.clear();
            n.clientTagDeny.clear();
            caseMapping_.erase(e.server);
            n.caseMapping = "rfc1459";
            channelTypes_.erase(e.server);
            n.channelTypes = "#&+!";
            prefixModes_.erase(e.server);
            channelModes_.erase(e.server);
            historyReferences_.erase(e.server);
            for (auto &[id, b] : model.buffers)
                if (b.server == e.server)
                    clearAvatar(b);
        }
        if (!n.error.empty())
            report(n.error, e.server);
        if (e.text == "Disconnected" || e.text == "Failed") {
            auto failed = e;
            failed.tags["unconfirmed"] = "1";
            failSends(failed, e.text + (n.error.empty() ? "" : " — " + n.error));
            std::erase_if(requestedHistory_,
                          [&](const auto &id) { return id.starts_with(e.server + "/"); });
            std::erase_if(gaps_,
                          [&](const auto &item) { return item.first.starts_with(e.server + "/"); });
            std::erase_if(activeHistory_, [&](const auto &item) {
                return item.second.starts_with(e.server + "/");
            });
            for (auto &[id, b] : model.buffers)
                if (b.server == e.server) {
                    b.joined = false;
                    clearAvatar(b);
                    b.loading = false;
                    b.typing.clear();
                    b.nicks.clear();
                }
            if (old != e.text && (old == "Ready" || old == "Registering")) {
                for (const auto &[id, b] : model.buffers)
                    if (b.server == e.server && visible(b)) {
                        effect(e, b);
                        break;
                    }
            }
        }
        if (e.text == "Ready" && old != "Ready") {
            const bool root =
                std::any_of(model.servers.begin(), model.servers.end(),
                            [&](const auto &s) { return s.name == e.server && s.sojuAccount; });
            if (root && !n.caps.contains("soju.im/bouncer-networks"))
                n.sojuMutationError = "Soju server does not support network discovery";
            synchronizeSoju(e.server);
            for (const auto &s : model.servers)
                if (s.name == e.server)
                    for (const auto &target : s.channels)
                        if (auto c = client(e.server))
                            c->command("JOIN " + target);
            for (auto &[id, b] : model.buffers)
                if (b.server == e.server && b.kind == "query")
                    requestHistory(b);
        }
        auto &b = buffer(e.server, "*server*", "server");
        Message m;
        m.id = localId();
        m.nick = "BlastIRC";
        m.text = e.text;
        m.timestamp = epoch();
        m.arrival = model.now;
        m.presence = true;
        insert(b, std::move(m));
        return;
    }
    if (e.type == "caps") {
        const bool hadHistory = n.caps.contains("draft/chathistory") ||
                                n.caps.contains("chathistory") || zncPlayback(e.server);
        const bool hadRead =
            n.caps.contains("draft/read-marker") || n.caps.contains("soju.im/read");
        n.caps = set<string>(e.params.begin(), e.params.end());
        if (!n.caps.contains("batch") || !n.caps.contains("draft/metadata-2"))
            for (auto &[id, b] : model.buffers)
                if (b.server == e.server)
                    clearAvatar(b);
        const bool hasHistory = n.caps.contains("draft/chathistory") ||
                                n.caps.contains("chathistory") || zncPlayback(e.server);
        const bool hasRead =
            n.caps.contains("draft/read-marker") || n.caps.contains("soju.im/read");
        if (n.state == "Ready")
            for (auto &[id, b] : model.buffers)
                if (b.server == e.server && (b.joined || b.kind == "query")) {
                    if (!hadHistory && hasHistory)
                        requestHistory(b);
                    if (!hadRead && hasRead && markerTime(b.readMarker) > 0)
                        if (auto c = client(e.server))
                            c->markRead(b.target, b.readMarker);
                }
        return;
    }
    if (e.type == "error") {
        const string reason = e.text.empty() ? eventBody(e) : e.text;
        if (flag(e, "send-failed")) {
            const bool restored = failSends(e, reason);
            if (!restored && !e.target.empty())
                restoreDraft(buffer(e.server, e.target), tag(e, "original-text"), true);
        }
        if (!routedCommand)
            report(reason, e.server);
        return;
    }
    if (e.type == "network") {
        auto kind = tag(e, "kind");
        if (kind == "authentication") {
            n.saslAuthenticated = flag(e, "authenticated") && tag(e, "mechanism") == "PLAIN";
            return;
        }
        if (kind == "metadata") {
            if (e.history || !n.caps.contains("batch") || !n.caps.contains("draft/metadata-2"))
                return;
            const auto command = upper(tag(e, "command"));
            const size_t offset = command == "761" || command == "766" ? 1 : 0;
            if ((command != "METADATA" && command != "761" && command != "766") ||
                e.params.size() < offset + (command == "766" ? 2 : 3))
                return;
            const auto &target = e.params[offset];
            if (!channel(e.server, target) || e.params[offset + 1] != "avatar" ||
                (command == "761" && e.params.size() < offset + 4))
                return;
            auto room =
                std::find_if(model.buffers.begin(), model.buffers.end(), [&](const auto &entry) {
                    const auto &b = entry.second;
                    return b.server == e.server && b.kind == "channel" && b.joined &&
                           fold(e.server, b.target) == fold(e.server, target);
                });
            if (room == model.buffers.end())
                return;
            auto &b = room->second;
            const std::string_view value = command == "766" || e.params.size() == offset + 3
                                               ? std::string_view{}
                                               : e.params[offset + 3];
            if (b.avatar != value) {
                clearAvatar(b);
                if (value.size() <= 2048)
                    b.avatar = value;
            }
            return;
        }
        if (kind == "isupport") {
            const string advertised = tag(e, "soju.im/FILEHOST");
            if (n.fileHost != advertised) {
                n.fileHost = advertised;
                ++n.sessionGeneration;
                // FILEHOST revokes upload grants, not an IRC command's connection identity.
                for (auto &[id, origin] : commandOrigins_)
                    if (origin.server == e.server)
                        origin.sessionGeneration = n.sessionGeneration;
                if (model.whoisProfile.server == e.server)
                    model.whoisProfile.sessionGeneration = n.sessionGeneration;
            }
            auto snapshot = [&](auto &values, const string &key, bool emptyIsValue = false) {
                auto found = e.tags.find(key);
                if (found == e.tags.end() || (!emptyIsValue && found->second.empty()))
                    values.erase(e.server);
                else
                    values[e.server] = found->second;
            };
            snapshot(caseMapping_, "CASEMAPPING");
            n.caseMapping = caseMapping_.contains(e.server) ? caseMapping_.at(e.server) : "rfc1459";
            snapshot(channelTypes_, "CHANTYPES", true);
            n.channelTypes = channelTypes_.contains(e.server) ? channelTypes_.at(e.server) : "#&+!";
            snapshot(prefixModes_, "PREFIX");
            snapshot(channelModes_, "CHANMODES");
            n.clientTagDeny = tag(e, "CLIENTTAGDENY");
            auto refs = e.tags.find("MSGREFTYPES");
            if (refs != e.tags.end())
                historyReferences_[e.server] = refs->second;
            else
                historyReferences_.erase(e.server);
            vector<std::pair<string, string>> renamed;
            for (const auto &[id, b] : model.buffers)
                if (b.server == e.server && id != e.server + "/" + fold(e.server, b.target))
                    renamed.emplace_back(id, b.target);
            for (const auto &[id, target] : renamed)
                renameBuffer(id, target);
            for (auto &[id, b] : model.buffers)
                if (b.server == e.server)
                    recount(b);
            return;
        }
        if (!e.history && upper(tag(e, "command")) == "MODE" && e.params.size() >= 2 &&
            channel(e.server, e.params[0])) {
            auto &room = buffer(e.server, e.params[0]);
            const string spec = prefixModes_.contains(e.server) ? prefixModes_[e.server] : "(ov)@+";
            const auto close = spec.find(')');
            const string modes = close == string::npos ? "ov" : spec.substr(1, close - 1);
            const string prefixes = close == string::npos ? "@+" : spec.substr(close + 1);
            vector<string> groups;
            std::istringstream stream(channelModes_.contains(e.server) ? channelModes_[e.server]
                                                                       : "b,k,l,imnpst");
            for (string part; std::getline(stream, part, ',');)
                groups.push_back(part);
            groups.resize(4);
            bool adding = true;
            size_t argument = 2;
            for (const char mode : e.params[1]) {
                if (mode == '+') {
                    adding = true;
                    continue;
                }
                if (mode == '-') {
                    adding = false;
                    continue;
                }
                const auto status = modes.find(mode);
                const bool parameter = status != string::npos ||
                                       groups[0].find(mode) != string::npos ||
                                       groups[1].find(mode) != string::npos ||
                                       (adding && groups[2].find(mode) != string::npos);
                if (!parameter || argument >= e.params.size())
                    continue;
                const auto &value = e.params[argument++];
                if (status != string::npos && status < prefixes.size()) {
                    for (auto &nick : room.nicks)
                        if (fold(e.server, nick.name) == fold(e.server, value)) {
                            std::erase(nick.prefix, prefixes[status]);
                            if (adding)
                                nick.prefix += prefixes[status];
                            std::sort(nick.prefix.begin(), nick.prefix.end(), [&](char a, char b) {
                                return prefixes.find(a) < prefixes.find(b);
                            });
                        }
                }
                if (mode == 'b' && adding) {
                    auto ban = e;
                    ban.type = "ban";
                    ban.target = room.target;
                    ban.tags["force-presence"] = "1";
                    presence(room, ban, e.nick + " banned " + value);
                }
            }
        }
        if (kind == "account" || kind == "setname" || kind == "chghost") {
            if (e.history)
                return;
            for (auto &[id, b] : model.buffers)
                if (b.server == e.server)
                    for (auto &nick : b.nicks)
                        if (fold(e.server, nick.name) == fold(e.server, e.nick) &&
                            kind == "account") {
                            nick.account = tag(e, "account");
                            if (nick.account.empty() && !e.text.empty() && e.text != "*")
                                nick.account = e.text;
                            if (nick.account == "*")
                                nick.account.clear();
                        }
            return;
        }
        if (kind == "bouncer" || kind == "bouncer-list" || kind == "bouncer-ack" ||
            kind == "bouncer-fail") {
            const bool root =
                std::any_of(model.servers.begin(), model.servers.end(),
                            [&](const auto &s) { return s.name == e.server && s.sojuAccount; });
            if (!root)
                return;
            if (kind == "bouncer-list") {
                if (tag(e, "phase") == "begin") {
                    sojuCatalogSeen_[e.server].clear();
                    sojuCatalogBatch_[e.server] = e.id;
                    n.sojuDiscoveryComplete = false;
                } else if (tag(e, "phase") == "end" && sojuCatalogSeen_.contains(e.server) &&
                           sojuCatalogBatch_[e.server] == e.id) {
                    set<string> absent;
                    for (const auto &[id, info] : n.sojuNetworks)
                        if (!sojuCatalogSeen_.at(e.server).contains(id))
                            absent.insert(id);
                    for (const auto &s : model.servers)
                        if (s.sojuParent == e.server &&
                            !sojuCatalogSeen_.at(e.server).contains(s.sojuNetworkId))
                            absent.insert(s.sojuNetworkId);
                    for (const auto &id : absent) {
                        n.sojuNetworks.erase(id);
                        retireSoju(e.server, id);
                    }
                    sojuCatalogSeen_.erase(e.server);
                    sojuCatalogBatch_.erase(e.server);
                    n.sojuDiscoveryComplete = true;
                    synchronizeSoju(e.server);
                    save();
                }
            } else if (kind == "bouncer") {
                if (flag(e, "deleted")) {
                    n.sojuNetworks.erase(e.id);
                    retireSoju(e.server, e.id);
                } else {
                    if (sojuCatalogSeen_.contains(e.server))
                        sojuCatalogSeen_[e.server].insert(e.id);
                    auto &info = n.sojuNetworks[e.id];
                    info.id = e.id;
                    for (const auto &[key, value] : e.tags) {
                        if (key == "kind" || key == "command" || key == "batch" ||
                            key == "source-order" || key == "label")
                            continue;
                        if (value.empty())
                            info.attributes.erase(key);
                        else
                            info.attributes[key] = value;
                    }
                    synchronizeSoju(e.server);
                }
                save();
            } else {
                if (kind == "bouncer-fail" && tag(e, "command") == "LISTNETWORKS") {
                    n.sojuDiscoveryComplete = false;
                    n.sojuMutationError = "Soju rejected network discovery";
                    return;
                }
                auto pending = sojuMutations_.find(e.server);
                if (pending == sojuMutations_.end() || pending->second.first != tag(e, "command") ||
                    (!pending->second.second.empty() && pending->second.second != e.id))
                    return;
                n.sojuMutationPending = false;
                n.sojuMutationSucceeded = kind == "bouncer-ack";
                n.sojuMutationError =
                    kind == "bouncer-fail" ? "Soju rejected the network operation" : "";
                n.sojuMutationId = e.id;
                if (kind == "bouncer-ack" && tag(e, "command") == "DELNETWORK") {
                    n.sojuNetworks.erase(e.id);
                    retireSoju(e.server, e.id);
                    save();
                }
                sojuMutations_.erase(pending);
            }
            return;
        }
        if (kind == "netsplit" || kind == "netjoin")
            return; // Individual batch members carry the affected nicks.
        if (kind == "latency" || !tag(e, "latency").empty()) {
            try {
                n.latency = std::stod(tag(e, "latency").empty() ? e.text : tag(e, "latency"));
            } catch (const std::exception &) {
            }
            return;
        }
        if (kind == "rename") {
            if (e.params.size() < 2 || !channel(e.server, e.params[0]) ||
                !channel(e.server, e.params[1]))
                return;
            const string previous = e.params[0], target = e.params[1];
            if (!e.history) {
                const string id = buffer(e.server, previous).id;
                renameBuffer(id, target);
            }
            auto &renamed = buffer(e.server, e.history ? e.target : target);
            auto notice = e;
            notice.tags["force-presence"] = "1";
            presence(renamed, notice,
                     e.nick + " renamed " + previous + " to " + target +
                         (e.text.empty() ? "" : " — " + e.text),
                     false);
            return;
        }
        if (routedCommand)
            return;
        auto &b = buffer(e.server, kind == "search" ? "*search*" : "*server*",
                         kind == "search" ? "search" : "server");
        Message m;
        m.id = e.id.empty() ? localId() : e.id;
        m.nick = e.nick;
        m.text = eventBody(e);
        m.time = e.time;
        m.timestamp = e.timestamp > 0 ? e.timestamp : (e.history ? 0 : epoch());
        m.arrival = model.now;
        m.history = e.history;
        m.presence = true;
        if (!m.text.empty())
            insert(b, std::move(m), kind != "search");
        return;
    }
    if (e.type == "nick") {
        if (e.history) {
            if (!e.target.empty())
                presence(buffer(e.server, e.target), e, e.nick + " is now known as " + e.text);
            return;
        }
        const auto old = fold(e.server, e.nick), replacement = fold(e.server, e.text);
        if (old == fold(e.server, n.nick))
            n.nick = e.text;
        vector<string> queries;
        for (auto &[id, b] : model.buffers)
            if (b.server == e.server) {
                bool member = false;
                for (auto &nick : b.nicks)
                    if (fold(e.server, nick.name) == old) {
                        nick.name = e.text;
                        member = true;
                    }
                if (b.kind == "query" && fold(e.server, b.target) == old) {
                    queries.push_back(id);
                    member = true;
                }
                auto a = activity_.find(id + "\n" + old);
                if (a != activity_.end() && old != replacement) {
                    auto &dest = activity_[id + "\n" + replacement];
                    dest.insert(dest.end(), a->second.begin(), a->second.end());
                    std::sort(dest.begin(), dest.end());
                    while (dest.size() > 5)
                        dest.pop_front();
                    activity_.erase(a);
                }
                auto typing = std::find_if(b.typing.begin(), b.typing.end(), [&](const auto &item) {
                    return fold(e.server, item.first) == old;
                });
                if (typing != b.typing.end()) {
                    const auto at = typing->second;
                    b.typing.erase(typing);
                    b.typing[e.text] = at;
                }
                if (member)
                    presence(b, e, e.nick + " is now known as " + e.text);
            }
        for (const auto &id : queries)
            renameBuffer(id, e.text);
        return;
    }
    if (e.type == "quit" || e.type == "away") {
        if (e.history) {
            if (e.type == "quit" && !e.target.empty())
                groupPresence(e, false, tag(e, "batch-type") == "netsplit");
            return;
        }
        for (auto &[id, b] : model.buffers)
            if (b.server == e.server) {
                auto nick = std::find_if(b.nicks.begin(), b.nicks.end(), [&](const auto &value) {
                    return fold(e.server, value.name) == fold(e.server, e.nick);
                });
                if (nick == b.nicks.end())
                    continue;
                if (e.type == "away") {
                    nick->away = flag(e, "away") || (!e.text.empty() && tag(e, "away") != "0");
                    continue;
                }
                b.nicks.erase(nick);
                std::erase_if(b.typing, [&](const auto &item) {
                    return fold(e.server, item.first) == fold(e.server, e.nick);
                });
                auto event = e;
                event.target = b.target;
                groupPresence(event, false, tag(e, "batch-type") == "netsplit");
            }
        return;
    }
    if ((e.type == "typing" || e.type == "reaction" || e.type == "redact") && !e.nick.empty() &&
        !e.own && fold(e.server, e.target) == fold(e.server, n.nick))
        e.target = e.nick;
    const bool searchBatch =
        (e.type == "history_begin" || e.type == "history_end") && tag(e, "direction") == "search";
    if (searchBatch)
        e.target = "*search*";
    auto &b = buffer(e.server, e.target.empty() ? "*server*" : e.target,
                     searchBatch        ? "search"
                     : e.target.empty() ? "server"
                                        : "");
    if (e.type == "history_begin") {
        b.loading = true;
        if (!e.label.empty())
            activeHistory_[e.label] = b.id;
        auto gap = gaps_.find(b.id);
        if (tag(e, "direction") == "after" && gap != gaps_.end()) {
            gap->second.active = true;
            gap->second.received = 0;
            gap->second.next.clear();
            gap->second.label = e.label;
        }
        return;
    }
    if (e.type == "history_end") {
        b.loading = false;
        if (!e.label.empty())
            activeHistory_.erase(e.label);
        auto gap = gaps_.find(b.id);
        if (tag(e, "direction") == "after" && gap != gaps_.end()) {
            auto &g = gap->second;
            int limit = 200;
            const auto value = tag(e, "limit");
            if (!value.empty()) {
                int parsed = 0;
                auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
                if (result.ec == std::errc{} && parsed > 0)
                    limit = parsed;
            }
            const auto count = tag(e, "count");
            if (!count.empty()) {
                size_t parsed = 0;
                auto result = std::from_chars(count.data(), count.data() + count.size(), parsed);
                if (result.ec == std::errc{})
                    g.received = parsed;
            }
            if (!tag(e, "cursor").empty())
                g.next = tag(e, "cursor");
            if (tag(e, "error").empty() && g.received >= static_cast<size_t>(limit) &&
                !g.next.empty() && g.next != g.cursor) {
                g.cursor = g.next;
                g.next.clear();
                g.active = false;
                g.received = 0;
                if (auto c = client(b.server)) {
                    c->history(b.target, "after=" + g.cursor);
                    b.loading = true;
                }
            } else {
                gaps_.erase(gap);
                if (!tag(e, "error").empty())
                    if (auto c = client(b.server)) {
                        c->history(b.target);
                        b.loading = true;
                    }
            }
        }
        if (!tag(e, "error").empty())
            report("History: " + tag(e, "error"), e.server);
        return;
    }
    if (e.type == "names") {
        if (e.history)
            return;
        if (!namesInProgress_.contains(b.id)) {
            namesInProgress_.insert(b.id);
            namesCache_[b.id] = b.nicks;
            b.nicks.clear();
        }
        string prefixes = "~&@%+";
        auto prefix = tag(e, "PREFIX");
        auto close = prefix.find(')');
        if (close != string::npos)
            prefixes = prefix.substr(close + 1);
        for (auto value : e.params) {
            Nick nick;
            while (!value.empty() && prefixes.find(value.front()) != string::npos) {
                nick.prefix += value.front();
                value.erase(value.begin());
            }
            auto bang = value.find('!');
            nick.name = value.substr(0, bang);
            if (nick.name.empty())
                continue;
            auto previous = std::find_if(
                namesCache_[b.id].begin(), namesCache_[b.id].end(), [&](const auto &old) {
                    return fold(e.server, old.name) == fold(e.server, nick.name);
                });
            if (previous != namesCache_[b.id].end()) {
                nick.account = previous->account;
                nick.away = previous->away;
                nick.arrived = previous->arrived;
            }
            auto found = std::find_if(b.nicks.begin(), b.nicks.end(), [&](const auto &old) {
                return fold(e.server, old.name) == fold(e.server, nick.name);
            });
            if (found == b.nicks.end())
                b.nicks.push_back(std::move(nick));
            else
                found->prefix = nick.prefix;
        }
        if (flag(e, "complete")) {
            namesInProgress_.erase(b.id);
            namesCache_.erase(b.id);
        }
        return;
    }
    if (e.type == "join") {
        e.own = e.own || fold(e.server, e.nick) == fold(e.server, n.nick);
        if (!e.history) {
            auto nick = std::find_if(b.nicks.begin(), b.nicks.end(), [&](const auto &old) {
                return fold(e.server, old.name) == fold(e.server, e.nick);
            });
            if (nick == b.nicks.end()) {
                b.nicks.push_back({e.nick, tag(e, "account"), "", false, model.now});
                nick = std::prev(b.nicks.end());
            } else
                nick->account = tag(e, "account");
            if (nick->account == "*")
                nick->account.clear();
            if (e.own) {
                b.joined = true;
                requestHistory(b);
            }
        }
        if (tag(e, "batch-type") == "netjoin") {
            groupPresence(e, true, true);
            return;
        }
        presence(b, e, e.nick + " joined " + b.target);
        return;
    }
    if (e.type == "part" || e.type == "kick") {
        string victim =
            e.type == "kick" ? (e.params.size() > 1 ? e.params[1] : tag(e, "victim")) : e.nick;
        if (victim.empty())
            victim = e.nick;
        if (!e.history) {
            std::erase_if(b.nicks, [&](const auto &nick) {
                return fold(e.server, nick.name) == fold(e.server, victim);
            });
            std::erase_if(b.typing, [&](const auto &item) {
                return fold(e.server, item.first) == fold(e.server, victim);
            });
        }
        if (!e.history && fold(e.server, victim) == fold(e.server, n.nick)) {
            b.joined = false;
            clearAvatar(b);
            for (auto &s : model.servers)
                if (s.name == e.server)
                    std::erase_if(s.channels, [&](const auto &target) {
                        return fold(e.server, target) == fold(e.server, b.target);
                    });
        }
        if (e.type == "kick") {
            // Kicks are moderation events, not optional SMART presence noise.
            auto event = e;
            event.tags["force-presence"] = "1";
            event.tags["victim"] = victim;
            presence(b, event,
                     victim + " was kicked by " + e.nick + (e.text.empty() ? "" : " — " + e.text));
        } else
            presence(b, e, e.nick + " left " + b.target + (e.text.empty() ? "" : " — " + e.text));
        return;
    }
    if (e.type == "topic_current" || e.type == "topic") {
        if (!e.history)
            b.topic = e.text;
        return;
    }
    if (e.type == "typing") {
        std::erase_if(b.typing, [&](const auto &item) {
            return fold(e.server, item.first) == fold(e.server, e.nick);
        });
        if (model.settings.typingReceive && e.text == "active" && !e.own &&
            !isFool(model, e.server, e.nick) && fold(e.server, e.nick) != fold(e.server, n.nick))
            b.typing[e.nick] = model.now;
        return;
    }
    if (e.type == "read") {
        if (e.text.empty())
            return;
        const auto incoming = markerTime(e.text), current = markerTime(b.readMarker);
        if (!b.readMarker.empty() &&
            (incoming < current ||
             (incoming == current && markerFraction(e.text) < markerFraction(b.readMarker))))
            return;
        b.readMarker = e.text;
        recount(b);
        if (savingAllowed_)
            try {
                storage_.read(b.id, b.readMarker);
            } catch (const std::exception &error) {
                report(error.what(), b.server);
            }
        return;
    }
    if (e.type == "reaction" || e.type == "redact") {
        auto position = std::find_if(b.messages.begin(), b.messages.end(),
                                     [&](const auto &value) { return value.id == e.id; });
        std::optional<Message> durable;
        Message *m = position == b.messages.end() ? nullptr : &*position;
        if (auto source = unsavedSources_.find(b.id); source != unsavedSources_.end())
            for (const auto &row : source->second.messages)
                if (row.id == e.id) {
                    if (!m) {
                        durable = row;
                        m = &*durable;
                    } else if (row.deleted) {
                        m->deleted = true;
                        m->text.clear();
                        m->reactions.clear();
                    }
                    break;
                }
        if (!m && savingAllowed_) {
            durable = storage_.message(b.id, e.id);
            if (durable)
                m = &*durable;
        }
        if (!m && e.type == "redact" && !e.id.empty()) {
            durable.emplace();
            durable->id = e.id;
            durable->history = true;
            m = &*durable;
        }
        if (!m)
            return; // A reaction outside retained local history has no retained row.
        bool changed = false;
        if (e.type == "redact") {
            changed = !m->deleted;
            m->deleted = true;
            if (model.pendingSourceJump.buffer == b.id && model.pendingSourceJump.message == e.id) {
                model.pendingSourceJump.buffer.clear();
                model.pendingSourceJump.message.clear();
                ++model.pendingSourceJump.revision;
            }
            m->text.clear();
            m->reactions.clear();
        } else if (!m->deleted && !e.text.empty()) {
            auto &reactors = m->reactions[e.text];
            auto who = std::find_if(reactors.begin(), reactors.end(), [&](const auto &name) {
                return fold(e.server, name) == fold(e.server, e.nick);
            });
            if (flag(e, "remove")) {
                if (who != reactors.end()) {
                    reactors.erase(who);
                    changed = true;
                }
                if (reactors.empty())
                    m->reactions.erase(e.text);
            } else if (who == reactors.end()) {
                reactors.push_back(e.nick);
                changed = true;
            }
        }
        const bool inMemory = position != b.messages.end();
        if (changed) {
            store(b, *m, true);
            if (inMemory && e.type == "redact" && !m->presence) {
                ++b.participationRevision;
                const string prefix = b.id + "\n";
                std::erase_if(activity_,
                              [&](const auto &entry) { return entry.first.starts_with(prefix); });
                // Redaction is rare: replay retained evidence and structured nick
                // changes for this buffer, never rescan history on ordinary appends.
                for (const auto &row : b.messages) {
                    if (row.deleted)
                        continue;
                    if (row.activity == ActivityKind::Nick && !row.activityTarget.empty()) {
                        const auto old = prefix + fold(b.server, row.nick);
                        const auto replacement = prefix + fold(b.server, row.activityTarget);
                        auto previous = activity_.find(old);
                        if (previous != activity_.end() && old != replacement) {
                            auto &dest = activity_[replacement];
                            dest.insert(dest.end(), previous->second.begin(),
                                        previous->second.end());
                            std::sort(dest.begin(), dest.end());
                            while (dest.size() > 5)
                                dest.pop_front();
                            activity_.erase(previous);
                        }
                    } else
                        activity(b, row);
                }
            }
            if (inMemory && !flag(e, "rollback"))
                effect(e, b);
        }
        return;
    }
    if (!e.text.empty()) {
        Message m;
        m.id = localId();
        m.text = e.text;
        m.presence = true;
        m.history = e.history;
        m.timestamp = e.timestamp > 0 ? e.timestamp : (e.history ? 0 : epoch());
        m.arrival = model.now;
        insert(b, std::move(m));
    }
}
bool Application::markRead(Buffer &b) {
    if (b.kind == "mentions")
        return true;
    const auto last = std::find_if(b.messages.rbegin(), b.messages.rend(),
                                   [](const auto &m) { return !m.pending && !m.presence; });
    const string marker = last == b.messages.rend() ? b.readMarker
                          : last->time.empty()      ? "local:" + last->id
                                                    : last->time;
    if (marker == b.readMarker && !b.unread)
        return true;
    try {
        if (savingAllowed_)
            storage_.read(b.id, marker);
    } catch (const std::exception &e) {
        report(e.what(), b.server);
        return false;
    }
    b.readMarker = marker;
    b.unread = 0;
    const auto network = model.networks.find(b.server);
    if (last != b.messages.rend() && !last->time.empty() && network != model.networks.end() &&
        network->second.state == "Ready")
        try {
            if (auto c = client(b.server))
                c->markRead(b.target, last->time);
        } catch (const std::exception &e) {
            report(e.what(), b.server);
            return false;
        }
    return true;
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
string Application::historyCursor(const Buffer &b, const string &cursor) const {
    if (cursor.empty())
        return {};
    const bool after = cursor.starts_with("after=");
    string selector = after ? cursor.substr(6) : cursor;
    if (!selector.starts_with("timestamp=")) {
        const string id = selector.starts_with("msgid=") ? selector.substr(6) : selector;
        auto refs = historyReferences_.find(b.server);
        const bool timestampOnly =
            zncPlayback(b.server) ||
            (refs != historyReferences_.end() && refs->second.find("msgid") == string::npos &&
             refs->second.find("timestamp") != string::npos);
        auto message = timestampOnly ? std::find_if(b.messages.begin(), b.messages.end(),
                                                    [&](const Message &m) { return m.id == id; })
                                     : b.messages.end();
        if (timestampOnly && message != b.messages.end() && !message->time.empty())
            selector = "timestamp=" + message->time;
        else
            selector = "msgid=" + id;
    }
    return after ? "after=" + selector : selector;
}
void Application::requestHistory(Buffer &b, const string &before) {
    if (b.kind != "channel" && b.kind != "query")
        return;
    if (auto c = client(b.server)) {
        const auto &caps = model.networks[b.server].caps;
        const bool playback = zncPlayback(b.server);
        if (caps.contains("draft/chathistory") || caps.contains("chathistory") || playback) {
            if (before.empty()) {
                if (!requestedHistory_.insert(b.id).second)
                    return;
                auto tail =
                    std::find_if(b.messages.rbegin(), b.messages.rend(), [&](const auto &m) {
                        return !m.presence && !m.pending &&
                               (!m.time.empty() || (!playback && !m.id.starts_with("local:") &&
                                                    !m.id.starts_with("local-")));
                    });
                if (tail != b.messages.rend()) {
                    auto &gap = gaps_[b.id];
                    gap.cursor = !tail->id.starts_with("local:") && !tail->id.starts_with("local-")
                                     ? historyCursor(b, tail->id)
                                     : "timestamp=" + tail->time;
                    c->history(b.target, "after=" + gap.cursor);
                    b.loading = true;
                    return;
                }
            }
            c->history(b.target, historyCursor(b, before));
            b.loading = true;
        }
    }
}
void Application::clear(Buffer *b) {
    if (savingAllowed_)
        try {
            if (b)
                storage_.clearHistory(b->id);
            else
                storage_.clearHistory();
        } catch (const std::exception &e) {
            report(e.what(), b ? b->server : "");
            return;
        }
    ++mentionsLiveRevision_;
    if (!b || model.pendingSourceJump.buffer == b->id) {
        model.pendingSourceJump.buffer.clear();
        model.pendingSourceJump.message.clear();
        ++model.pendingSourceJump.revision;
    }
    if (b)
        unsavedSources_.erase(b->id);
    else
        unsavedSources_.clear();
    for (auto &[label, queued] : queuedSends_)
        if (!b || queued.buffer == b->id)
            queued.stored = false;
    for (auto &[id, value] : model.buffers)
        if (!b || id == b->id) {
            if (value.kind != "search" && value.kind != "mentions")
                messageCount_ -= value.messages.size();
            if (!value.messages.empty())
                reviseMessages(value);
            value.messages.clear();
            value.unread = 0;
            value.readMarker.clear();
        }
    if (!b) {
        activity_.clear();
        pending_.clear();
    } else {
        std::erase_if(activity_,
                      [&](const auto &entry) { return entry.first.starts_with(b->id + "\n"); });
        std::erase_if(pending_, [&](const auto &entry) { return entry.second.buffer == b->id; });
    }
}
void Application::send(Buffer &b, const string &text, const string &reply, bool isAction,
                       const string &origin, const string &fallbackNick) {
    if (text.empty())
        return;
    const string actionText = isAction ? "/me " + text : string{};
    const string &recovered = isAction ? actionText : text;
    auto *c = client(b.server);
    if (!c || model.networks[b.server].state != "Ready") {
        restoreDraft(b, recovered, true);
        report("Not connected to " + b.server, b.server, origin);
        return;
    }
    if (b.kind != "channel" && b.kind != "query") {
        restoreDraft(b, recovered, true);
        report("Choose a conversation before sending", b.server, origin);
        return;
    }
    if (reply.starts_with("local:")) {
        restoreDraft(b, recovered, true);
        report("Cannot reply until this message has a server ID", b.server, origin);
        return;
    }
    const string label = localId();
    queuedSends_.emplace(label, QueuedSend{b.id, recovered, sequence_});
    try {
        queuedSends_.at(label).preserveRaw = !fallbackNick.empty();
        if (savingAllowed_)
            storage_.queueSend(b, label, recovered);
        if (isAction)
            c->command("/me " + b.target + " " + text, label);
        else
            c->send(b.target, text, reply, label, fallbackNick);
    } catch (const std::exception &error) {
        Event failed;
        failed.server = b.server;
        failed.label = label;
        recoverQueuedSends(failed, error.what());
        report(error.what(), b.server, origin);
        return;
    }
    if (model.settings.typingSend)
        try {
            c->typing(b.target, "done");
        } catch (const std::exception &error) {
            // The send is already queued; a typing failure must not resurrect its text.
            report(error.what(), b.server, origin);
        }
}
void Application::enqueueCommand(const Buffer &b, const string &line) {
    auto c = client(b.server);
    if (!c)
        return;
    std::erase_if(commandOrigins_,
                  [&](const auto &item) { return model.now - item.second.started >= 120; });
    if (commandOrigins_.size() >= 256) {
        auto oldest = std::min_element(
            commandOrigins_.begin(), commandOrigins_.end(),
            [](const auto &a, const auto &b) { return a.second.started < b.second.started; });
        commandOrigins_.erase(oldest);
    }
    const auto id = "command-" + localId();
    const auto wire = irc::parse(line, irc::incomingCoreLimit);
    const string target =
        wire && wire->command == "WHOIS" && !wire->params.empty() ? wire->params.back() : string{};
    const auto generation = model.networks[b.server].sessionGeneration;
    commandOrigins_.emplace(id, CommandOrigin{b.server, b.id, model.now, target, generation});
    if (!target.empty()) {
        const auto revision = model.whoisProfile.revision + 1;
        model.whoisProfile = {};
        auto &profile = model.whoisProfile;
        profile.requestId = id;
        profile.server = b.server;
        profile.target = target;
        profile.sessionGeneration = generation;
        profile.revision = revision;
        profile.open = profile.loading = true;
    }
    try {
        c->command(line, {}, id);
    } catch (...) {
        commandOrigins_.erase(id);
        if (model.whoisProfile.requestId == id)
            finishWhois("Unable to send WHOIS request");
        throw;
    }
}
bool Application::commandResponse(const Event &e) {
    const bool whois = tag(e, "command-name") == "WHOIS" || whoisNumeric(tag(e, "command"));
    const auto id = tag(e, "command-id");
    if (id.empty())
        return whois;
    auto found = commandOrigins_.find(id);
    if (found == commandOrigins_.end())
        return whois;
    const auto origin = found->second;
    // A wrong-server event cannot consume the genuine route.
    if (origin.server != e.server)
        return whois || !origin.whoisTarget.empty();
    const bool valid = model.now - origin.started < 120 &&
                       origin.sessionGeneration == model.networks[e.server].sessionGeneration;
    auto b = valid ? findBuffer(origin.buffer) : nullptr;
    if (!origin.whoisTarget.empty()) {
        if (valid && b && b->server == e.server)
            updateWhois(e, origin);
        if (flag(e, "command-complete") || !valid)
            commandOrigins_.erase(found);
        return true;
    }
    if (flag(e, "command-complete") || !valid)
        commandOrigins_.erase(found);
    if (!valid || !b || b->server != e.server)
        return false;
    string text = tag(e, "command-response");
    if (text.empty() && e.type == "error")
        text = e.text;
    const bool hasOutput = !text.empty();
    if (!text.empty()) {
        Message m;
        m.id = localId();
        m.nick = e.nick.empty() ? "Server" : e.nick;
        m.text = std::move(text);
        m.time = e.time;
        m.timestamp = e.timestamp > 0 ? e.timestamp : epoch();
        m.arrival = model.now;
        m.presence = true;
        insert(*b, std::move(m));
    }
    return hasOutput || tag(e, "kind") == "command-complete";
}
void Application::finishWhois(const string &error) {
    auto &profile = model.whoisProfile;
    if (!profile.loading)
        return;
    profile.loading = false;
    profile.error = error;
    if (profile.error.empty() && profile.records.empty())
        profile.error = "No profile information returned";
    ++profile.revision;
}
void Application::updateWhois(const Event &e, const CommandOrigin &origin) {
    auto &profile = model.whoisProfile;
    if (profile.requestId != tag(e, "command-id") || profile.server != e.server ||
        profile.sessionGeneration != origin.sessionGeneration)
        return;
    if (!profile.open) {
        if (flag(e, "command-complete"))
            profile.loading = false;
        return;
    }
    const auto command = tag(e, "command");
    const bool failed = e.type == "error" || command == "FAIL" || !tag(e, "error").empty() ||
                        command == "401" || command == "402" || command == "421" ||
                        command == "461" || command == "263" ||
                        (command.size() == 3 && (command[0] == '4' || command[0] == '5'));
    if (failed) {
        string reason = tag(e, "error");
        if (reason.empty() && !e.params.empty())
            reason = e.params.back();
        if (reason.empty())
            reason = e.text;
        finishWhois(reason.empty() ? "WHOIS request failed" : reason.substr(0, 4096));
        return;
    }
    const size_t minimum = command == "311" ? 6 : command == "312" ? 4 : 3;
    if (whoisNumeric(command) && command != "318" && e.params.size() >= minimum &&
        irc::validNickname(e.params[1])) {
        const auto &nick = e.params[1];
        const bool many = origin.whoisTarget.find_first_of(",*?") != string::npos;
        const bool matching = fold(e.server, nick) == fold(e.server, origin.whoisTarget);
        if (matching || many) {
            auto record =
                std::find_if(profile.records.begin(), profile.records.end(), [&](const auto &item) {
                    return fold(e.server, item.nick) == fold(e.server, nick);
                });
            if (record == profile.records.end() && profile.records.size() < 32) {
                profile.records.push_back({});
                record = std::prev(profile.records.end());
                record->nick = nick;
            }
            if (record != profile.records.end()) {
                const auto field = [&](size_t index) {
                    return index < e.params.size() ? e.params[index].substr(0, 4096) : string{};
                };
                if (command == "311" && e.params.size() >= 6) {
                    record->nick = nick;
                    record->username = field(2);
                    record->host = field(3);
                    record->realName = field(5);
                } else if (command == "312" && e.params.size() >= 4) {
                    record->server = field(2);
                    record->serverInfo = field(3);
                } else if (command == "301" && e.params.size() >= 3) {
                    record->away = field(2);
                } else if (command == "330" && e.params.size() >= 3) {
                    record->account = field(2);
                } else if (command == "319" && e.params.size() >= 3) {
                    if (!record->channels.empty() && record->channels.size() < 16384)
                        record->channels += ' ';
                    record->channels += field(2).substr(0, 16384 - record->channels.size());
                } else if (command == "317" && e.params.size() >= 3) {
                    const auto number = [&](size_t index) {
                        if (index >= e.params.size())
                            return string{};
                        const auto &text = e.params[index];
                        uint64_t value = 0;
                        const auto parsed =
                            std::from_chars(text.data(), text.data() + text.size(), value);
                        return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()
                                   ? std::to_string(value)
                                   : string{};
                    };
                    record->idle = number(2);
                    record->signon = number(3);
                } else if (command != "311" && command != "312" && command != "317" &&
                           command != "319" && command != "330" && command != "301" &&
                           e.params.size() >= 3 && record->details.size() < 32) {
                    string detail = command + ":";
                    for (size_t i = 2; i < e.params.size() && detail.size() < 4096; ++i)
                        detail += " " + field(i).substr(0, 4096 - detail.size() - 1);
                    if (std::find(record->details.begin(), record->details.end(), detail) ==
                        record->details.end())
                        record->details.push_back(std::move(detail));
                }
                ++profile.revision;
            }
        }
    }
    if (flag(e, "command-complete"))
        finishWhois({});
}
void Application::command(Buffer &b, const string &input) {
    if (unsafeLine(input)) {
        restoreDraft(b, input, true);
        report("Commands cannot contain NUL or line breaks", b.server, b.id);
        return;
    }
    auto [name, args] = word(input.starts_with('/') ? input.substr(1) : input);
    name = upper(name);
    const auto required = missingCommandArguments(
        input, b.kind, channelTypes_.contains(b.server) ? channelTypes_.at(b.server) : "#&+!");
    const auto info =
        std::find_if(commandCatalog.begin(), commandCatalog.end(),
                     [&](const auto &entry) { return commandEqual(name, entry.name); });
    if (!required.empty() && info != commandCatalog.end() && info->client) {
        if (name == "ME")
            restoreDraft(b, input, true);
        report("Usage: /" + string(info->name) + " " + string(info->usage), b.server, b.id);
        return;
    }
    auto c = client(b.server);
    if (name == "CONNECT") {
        action({"connect", b.id, args.empty() ? b.server : args, {}});
        return;
    }
    if (name == "DISCONNECT") {
        action({"disconnect", {}, b.server, {}});
        return;
    }
    if (name == "CLEAR") {
        clear(&b);
        return;
    }
    if (name == "MESSAGESTYLE") {
        const auto style = lower(args);
        if ((b.kind != "channel" && b.kind != "query") ||
            (!style.empty() && style != "irc" && style != "bubbles")) {
            report("Usage: /messagestyle [bubbles|irc] (in a conversation)", b.server, b.id);
            return;
        }
        if (style == "irc" || (style.empty() && !model.settings.ircChannels.contains(b.id)))
            model.settings.ircChannels.insert(b.id);
        else
            model.settings.ircChannels.erase(b.id);
        save();
        return;
    }
    if (name == "QUIT") {
        model.quit = true;
        for (auto &[server, connection] : clients_)
            try {
                connection->command("QUIT :" + (args.empty() ? "Leaving BlastIRC" : args));
            } catch (const std::exception &error) {
                report(error.what(), server, b.id);
            }
        return;
    }
    if (name == "ME") {
        if (args.empty() || (b.kind != "channel" && b.kind != "query")) {
            restoreDraft(b, input, true);
            report("Usage: /me action (in a conversation)", b.server, b.id);
            return;
        }
        send(b, args, {}, true, b.id);
        return;
    }
    if (name == "MSG" || name == "QUERY") {
        auto [target, text] = word(args);
        if (target.empty()) {
            report("Usage: /" + lower(name) + " nick [message]", b.server, b.id);
            return;
        }
        auto &query = buffer(b.server, target);
        model.activeBuffer = query.id;
        requestHistory(query);
        if (!text.empty())
            send(query, text, {}, false, b.id);
        return;
    }
    if (!c) {
        report("Not connected to " + b.server, b.server, b.id);
        return;
    }
    if (name == "BOUNCER") {
        auto [id, rest] = word(args);
        if (upper(id) == "BIND") {
            auto parts = word(rest);
            id = parts.first;
            rest = parts.second;
        }
        if (id.empty() || !rest.empty()) {
            report("Usage: /bouncer network-id", b.server, b.id);
            return;
        }
        string parent = b.server;
        for (const auto &s : model.servers)
            if (s.name == b.server && !s.sojuParent.empty())
                parent = s.sojuParent;
        auto root = model.networks.find(parent);
        if (root == model.networks.end() || !root->second.sojuNetworks.contains(id)) {
            report("Select a discovered network from its Soju account", b.server, b.id);
            return;
        }
        synchronizeSoju(parent);
        for (const auto &s : model.servers)
            if (s.sojuParent == parent && s.sojuNetworkId == id) {
                model.activeBuffer = buffer(s.name, "*server*", "server").id;
                break;
            }
        return;
    }
    if (name == "JOIN") {
        auto [targets, keys] = word(args);
        if (targets.empty()) {
            report("Usage: /join #channel[,#channel] [keys]", b.server, b.id);
            return;
        }
        for (const auto &target : commaWords(targets)) {
            auto &joined = buffer(b.server, target);
            model.activeBuffer = joined.id;
            for (auto &s : model.servers)
                if (s.name == b.server &&
                    std::none_of(s.channels.begin(), s.channels.end(), [&](const auto &value) {
                        return fold(b.server, value) == fold(b.server, target);
                    }))
                    s.channels.push_back(target);
        }
        enqueueCommand(b, "JOIN " + args);
        return;
    }
    if (name == "PART") {
        auto [target, reason] = word(args);
        if (!channel(b.server, target)) {
            reason = args;
            target = b.kind == "channel" ? b.target : "";
        }
        if (target.empty()) {
            report("Usage: /part #channel [reason]", b.server, b.id);
            return;
        }
        const auto targets = commaWords(target);
        for (auto &s : model.servers)
            if (s.name == b.server)
                std::erase_if(s.channels, [&](const auto &value) {
                    return std::any_of(targets.begin(), targets.end(), [&](const auto &part) {
                        return fold(b.server, value) == fold(b.server, part);
                    });
                });
        enqueueCommand(b, "PART " + target + (reason.empty() ? "" : " :" + reason));
        return;
    }
    if (name == "KICK") {
        auto [first, rest] = word(args);
        string target = b.target, victim = first, reason = rest;
        if (channel(b.server, first)) {
            target = first;
            auto parts = word(rest);
            victim = parts.first;
            reason = parts.second;
        }
        if (!channel(b.server, target) || victim.empty()) {
            report("Usage: /kick [#channel] nick [reason]", b.server, b.id);
            return;
        }
        enqueueCommand(b, "KICK " + target + " " + victim + (reason.empty() ? "" : " :" + reason));
        return;
    }
    if (name == "NICK") {
        auto [nick, rest] = word(args);
        if (nick.empty() || !rest.empty()) {
            report("Usage: /nick newnick", b.server, b.id);
            return;
        }
        enqueueCommand(b, "NICK " + nick);
        return;
    }
    if (name == "AWAY") {
        enqueueCommand(b, args.empty() ? "AWAY" : "AWAY :" + args);
        return;
    }
    if (name == "WHOIS") {
        auto target = args.empty() ? (b.kind == "query" ? b.target : "") : args;
        if (target.empty()) {
            report("Usage: /whois nick", b.server, b.id);
            return;
        }
        enqueueCommand(b, "WHOIS " + target);
        return;
    }
    if (name == "RAW" || name == "QUOTE") {
        if (!args.empty())
            enqueueCommand(b, args);
        return;
    }
    if (!name.empty())
        enqueueCommand(b, name + (args.empty() ? "" : " " + args));
}
void Application::dispatchActions() {
    activeActions_.swap(model.actions);
    model.actions.clear();
    for (nextAction_ = 0; nextAction_ < activeActions_.size(); ++nextAction_) {
        const auto &a = activeActions_[nextAction_];
        try {
            action(a);
        } catch (const std::exception &error) {
            auto b = findBuffer(a.buffer);
            report(error.what(), b ? b->server : "",
                   b && (a.type == "command" || a.type == "whois") ? b->id : "");
            if (b && a.type == "send")
                restoreDraft(*b, a.text, true);
        }
    }
    activeActions_.clear();
    nextAction_ = 0;
}
void Application::action(const Action &a) {
    if (a.type == "global-mentions") {
        refreshMentions();
        model.activeBuffer = GlobalMentionsBufferId;
        return;
    }
    if (a.type == "source-jump") {
        jumpSource(a.buffer, a.text);
        return;
    }
    Buffer *b = findBuffer(a.buffer);
    string server = b ? b->server : (model.servers.empty() ? string{} : model.servers.front().name);
    if (a.type == "pin_buffer") {
        const auto origin = model.buffers.find(a.buffer);
        if (origin == model.buffers.end() ||
            (origin->second.kind != "channel" && origin->second.kind != "query") ||
            (a.text != "on" && a.text != "off"))
            return;
        auto &pins = model.settings.pinnedBuffers;
        const bool pinned = std::find(pins.begin(), pins.end(), a.buffer) != pins.end();
        if (pinned == (a.text == "on"))
            return;
        if (!savingAllowed_) {
            report("Configuration errors must be fixed before saving; original files are unchanged",
                   origin->second.server);
            return;
        }
        auto previous = pins;
        if (a.text == "on")
            pins.insert(pins.begin(), a.buffer);
        else
            std::erase(pins, a.buffer);
        try {
            storage_.save(model);
        } catch (const std::exception &error) {
            pins = std::move(previous);
            report(error.what(), origin->second.server);
        }
        return;
    }
    if (a.type == "mute_buffer" || a.type == "archive_buffer") {
        // These commands require their captured origin, never the current selection.
        const auto origin = model.buffers.find(a.buffer);
        if (origin == model.buffers.end() ||
            (origin->second.kind != "channel" && origin->second.kind != "query") ||
            (a.text != "on" && a.text != "off"))
            return;
        if (!savingAllowed_) {
            report("Configuration errors must be fixed before saving; original files are unchanged",
                   origin->second.server);
            return;
        }
        auto &chat = origin->second;
        bool &flag = a.type == "mute_buffer" ? chat.muted : chat.archived;
        const bool previous = flag;
        const string previousActive = model.activeBuffer;
        flag = a.text == "on";
        if (a.type == "archive_buffer" && chat.archived && model.activeBuffer == chat.id)
            model.activeBuffer = buffer(chat.server, "*server*", "server").id;
        try {
            // This transaction includes the flags and selection, without unrelated
            // queued-send recovery or a second post-commit maintenance pass.
            storage_.save(model);
        } catch (const std::exception &error) {
            flag = previous;
            model.activeBuffer = previousActive;
            report(error.what(), chat.server);
        }
        return;
    }
    if (a.type == "soju-remove-account") {
        const string parent = a.buffer;
        if (std::none_of(model.servers.begin(), model.servers.end(),
                         [&](const auto &s) { return s.name == parent && s.sojuAccount; }))
            return;
        disconnect(parent);
        vector<string> retired;
        for (const auto &s : model.servers)
            if (s.sojuParent == parent || s.name == parent)
                retired.push_back(s.name);
        for (const auto &name : retired) {
            sojuRetired_.insert(name);
            model.networks.erase(name);
        }
        std::erase_if(model.servers,
                      [&](const auto &s) { return s.sojuParent == parent || s.name == parent; });
        save();
        return;
    }
    if (a.type == "open-server-log") {
        const string name = a.buffer.empty() ? a.text : a.buffer;
        if (std::none_of(model.servers.begin(), model.servers.end(),
                         [&](const auto &s) { return s.name == name; })) {
            report("Unknown server log");
            return;
        }
        model.activeBuffer = buffer(name, "*server*", "server").id;
        return;
    }
    if (a.type == "soju-set-enabled") {
        auto child = std::find_if(model.servers.begin(), model.servers.end(), [&](const auto &s) {
            return s.name == a.buffer && !s.sojuParent.empty();
        });
        if (child == model.servers.end())
            return;
        child->sojuEnabled = a.text == "1";
        const string name = child->name;
        if (child->sojuEnabled)
            connect(name);
        else
            disconnect(name);
        save();
        return;
    }
    if (a.type == "soju-associate-network") {
        auto profile = std::find_if(model.servers.begin(), model.servers.end(),
                                    [&](const auto &s) { return s.name == a.buffer; });
        auto parent = std::find_if(model.servers.begin(), model.servers.end(), [&](const auto &s) {
            return s.name == a.text && s.sojuAccount;
        });
        auto live = model.networks.find(a.text);
        if (profile == model.servers.end() || parent == model.servers.end() || profile == parent ||
            profile->sojuAccount || live == model.networks.end() ||
            !live->second.sojuNetworks.contains(a.extra)) {
            report("Choose an unassociated profile and an available discovered Soju network");
            return;
        }
        if (profile->sojuParent == a.text && profile->sojuNetworkId == a.extra)
            return;
        // Explicit association replaces only the binding, never merges histories.
        retireSoju(a.text, a.extra);
        profile = std::find_if(model.servers.begin(), model.servers.end(),
                               [&](const auto &s) { return s.name == a.buffer; });
        if (profile == model.servers.end())
            return;
        disconnect(profile->name);
        profile->sojuParent = a.text;
        profile->sojuNetworkId = a.extra;
        profile->bouncer = "soju:" + a.extra;
        profile->password.clear();
        const string name = profile->name;
        save();
        connect(name);
        return;
    }
    if (a.type == "soju-add-network" || a.type == "soju-change-network" ||
        a.type == "soju-delete-network") {
        const auto parent =
            std::find_if(model.servers.begin(), model.servers.end(),
                         [&](const auto &s) { return s.name == a.buffer && s.sojuAccount; });
        auto live = model.networks.find(a.buffer);
        if (parent == model.servers.end() || live == model.networks.end())
            return;
        auto &n = live->second;
        if (n.sojuMutationPending)
            return;
        n.sojuMutationError.clear();
        n.sojuMutationSucceeded = false;
        auto identity = std::find_if(model.identities.begin(), model.identities.end(),
                                     [&](const auto &i) { return i.name == parent->identity; });
        if (identity == model.identities.end() ||
            n.sojuSessionKey != sojuSessionKey(*parent, *identity)) {
            n.sojuMutationError = "Reconnect the Soju account after changing its configuration";
            return;
        }
        if (n.state != "Ready" || !n.caps.contains("soju.im/bouncer-networks") ||
            !client(a.buffer)) {
            n.sojuMutationError =
                "Connect an authenticated Soju account with network management support";
            return;
        }
        const string command = a.type == "soju-add-network"      ? "ADDNETWORK"
                               : a.type == "soju-change-network" ? "CHANGENETWORK"
                                                                 : "DELNETWORK";
        try {
            client(a.buffer)->sojuMutation(command, a.text, a.attributes);
            n.sojuMutationPending = true;
            n.sojuMutationCommand = command;
            n.sojuMutationId = a.text;
            sojuMutations_[a.buffer] = {command, a.text};
        } catch (const std::exception &) {
            n.sojuMutationError = "Invalid or unavailable Soju network operation";
        }
        return;
    }
    if (a.type == "save" || a.type == "save-connect") {
        const bool success = save();
        if (a.requestId && a.requestId == model.settingsSaveRequest) {
            const auto &previous = model.settingsSaveResult;
            if (success && previous.requestId && !previous.success && !previous.error.empty() &&
                model.status == previous.error)
                model.status.clear();
            model.settingsSaveResult = {a.requestId, success, success ? string{} : model.status};
        }
        if (success && a.type == "save-connect")
            connect(a.text.empty() ? server : a.text);
        return;
    }
    if (a.type == "quit") {
        model.quit = true;
        save();
        return;
    }
    if (a.type == "connect" || a.type == "disconnect") {
        const string target = a.text.empty() ? server : a.text;
        const auto profile = std::find_if(model.servers.begin(), model.servers.end(),
                                          [&](const auto &s) { return s.name == target; });
        if (profile != model.servers.end() && !profile->sojuParent.empty()) {
            action({"soju-set-enabled", target, a.type == "connect" ? "1" : "0", {}});
        } else if (a.type == "connect") {
            connect(target, b ? b->id : "");
        } else {
            disconnect(target);
        }
        return;
    }
    if (a.type == "clear_history") {
        clear(nullptr);
        return;
    }
    if (a.type == "read-all") {
        bool success = true;
        for (auto &[id, buffer] : model.buffers)
            if (buffer.kind != "search" && buffer.kind != "mentions")
                success = markRead(buffer) && success;
        if (success)
            model.status = "All buffers marked as read";
        return;
    }
    if (!b && !a.buffer.empty()) {
        model.status = "Action refers to a closed buffer";
        return;
    }
    if (!b && (a.type == "join" || a.type == "query" || a.type == "command"))
        b = &buffer(server, "*server*", "server");
    if (!b) {
        report("Action refers to a closed buffer: " + a.buffer, server);
        return;
    }
    // Aggregate rows have no network, composer or mutable source identity.
    if (b->kind == "mentions")
        return;
    auto c = client(b->server);
    if (a.type == "command") {
        command(*b, a.text);
        return;
    }
    if (a.type == "read") {
        markRead(*b);
        return;
    }
    if (a.type == "send") {
        const auto fallback = a.attributes.find("reply-nick");
        send(*b, a.text, a.extra, false, "",
             fallback == a.attributes.end() ? string{} : fallback->second);
        return;
    }
    if (a.type == "typing") {
        if (c && model.settings.typingSend)
            c->typing(b->target, a.text);
        return;
    }
    if (a.type == "join") {
        command(*b, "/join " + a.text);
        return;
    }
    if (a.type == "query") {
        if (a.text.empty() || unsafeLine(a.text) || a.text.find_first_of(" \t,") != string::npos) {
            report("Invalid nickname", b->server);
            return;
        }
        auto &query = buffer(b->server, a.text, "query");
        model.activeBuffer = query.id;
        requestHistory(query);
        return;
    }
    if (a.type == "whois") {
        command(*b, "/whois " + a.text);
        return;
    }
    if (a.type == "react" || a.type == "redact") {
        if (!c || a.extra.empty())
            return;
        if (a.extra.starts_with("local:")) {
            report("This message has no server ID for remote interaction", b->server);
            return;
        }
        if (a.type == "react") {
            const auto network = model.networks.find(b->server);
            const auto message = std::find_if(b->messages.begin(), b->messages.end(),
                                              [&](const auto &m) { return m.id == a.extra; });
            if (network == model.networks.end() || b->kind == "search" ||
                message == b->messages.end() || message->deleted || message->presence ||
                message->pending || unsafeLine(a.extra) ||
                a.extra.find_first_of(" \t") != string::npos)
                return;
            const auto &net = network->second;
            const bool tags = std::any_of(net.caps.begin(), net.caps.end(), [](const auto &cap) {
                return cap == "message-tags" || cap.starts_with("message-tags=");
            });
            bool removing = false;
            if (auto reaction = message->reactions.find(a.text);
                reaction != message->reactions.end())
                removing = std::any_of(
                    reaction->second.begin(), reaction->second.end(), [&](const auto &nick) {
                        return fold(b->server, nick) == fold(b->server, net.nick);
                    });
            if (net.state != "Ready" || !tags ||
                !irc::clientTagAllowed(net.clientTagDeny, "+reply") ||
                !irc::clientTagAllowed(net.clientTagDeny,
                                       removing ? "+draft/unreact" : "+draft/react"))
                return;
            c->react(b->target, a.extra, a.text, removing);
        } else
            c->redact(b->target, a.extra);
        return;
    }
    if (a.type == "history") {
        if (b->loading)
            return;
        const string before =
            a.extra.empty() ? (b->messages.empty() ? string{} : b->messages.front().id) : a.extra;
        if (savingAllowed_)
            try {
                auto page = storage_.history(b->id, 200, before);
                for (auto &m : page) {
                    m.history = true;
                    m.pending = false;
                    insert(*b, std::move(m), false);
                }
            } catch (const std::exception &e) {
                report(e.what(), b->server);
            }
        string remote = b->messages.empty() ? before : b->messages.front().id;
        // Locally assigned ids are not valid CHATHISTORY selectors.
        if (remote.starts_with("local:") || remote.starts_with("local-")) {
            auto oldest = std::find_if(b->messages.begin(), b->messages.end(),
                                       [](const auto &m) { return !m.time.empty(); });
            remote = oldest == b->messages.end() ? string{} : "timestamp=" + oldest->time;
        }
        requestHistory(*b, remote);
        return;
    }
    if (a.type == "search") {
        if (b->kind == "search") {
            auto *source = b->searchSource.empty() ? nullptr : findBuffer(b->searchSource);
            if (!source || source->kind == "search" || source->server != b->server) {
                report("Choose a conversation to search", b->server);
                return;
            }
            b = source;
        }
        const string query = lower(a.text), source = b->id, name = b->server;
        auto &results = buffer(name, "*search*", "search");
        results.searchSource = source;
        reviseMessages(results);
        results.messages.clear();
        results.title = "Search: " + a.text;
        if (!query.empty()) {
            vector<Message> messages;
            try {
                messages =
                    storage_.history(source, std::clamp(model.settings.maxMessages, 1, 100000));
            } catch (const std::exception &e) {
                report(e.what(), name);
            }
            std::set<std::string_view> seen;
            for (const auto &m : b->messages)
                if (m.deleted)
                    seen.insert(m.id);
            const auto limit = static_cast<size_t>(std::clamp(model.settings.maxMessages, 1, 2000));
            // ponytail: a 2k search view; page results if larger searches need navigation.
            auto match = [&](const Message &m) {
                if (m.deleted || !seen.insert(m.id).second ||
                    lower(m.text).find(query) == string::npos || results.messages.size() >= limit)
                    return;
                Message row = m;
                row.id = "search:" + fold(name, b->target) + ":" + m.id;
                row.pending = false;
                row.sendStatus = SendStatus::None;
                row.presence = true;
                row.activity = ActivityKind::None;
                row.history = true;
                row.text = "[" + b->target + "] " + m.nick + ": " + m.text;
                results.messages.push_back(std::move(row));
            };
            for (const auto &m : messages)
                match(m);
            for (const auto &m : b->messages)
                match(m);
            const auto &caps = model.networks[name].caps;
            if (c && caps.contains("soju.im/search") && !unsafeLine(a.text))
                c->command("/search " + b->target + " " + a.text);
        }
        model.activeBuffer = results.id;
        return;
    }
    if (a.type == "close_buffer") {
        const string id = b->id;
        if (b->kind == "server") {
            report("Disconnect the server instead of closing its utility buffer", b->server);
            return;
        }
        vector<string> submissions;
        for (const auto &[label, queued] : queuedSends_)
            if (queued.buffer == id)
                submissions.emplace_back(label);
        for (const auto &[key, pending] : pending_)
            if (pending.buffer == id && !pending.label.empty())
                submissions.emplace_back(submissionRoot(pending.label));
        for (const auto &message : b->messages)
            if (message.own && message.pending && !message.label.empty())
                submissions.emplace_back(submissionRoot(message.label));
        std::sort(submissions.begin(), submissions.end());
        submissions.erase(std::unique(submissions.begin(), submissions.end()), submissions.end());
        if (savingAllowed_)
            storage_.closeBuffer(*b, submissions);
        if (std::erase(model.settings.pinnedBuffers, id) && savingAllowed_)
            storage_.saveSettings(model.settings);
        for (const auto &label : submissions)
            failedSubmissions_[label] = model.now;
        for (auto queued = queuedSends_.begin(); queued != queuedSends_.end();) {
            if (queued->second.buffer != id) {
                ++queued;
                continue;
            }
            queued = queuedSends_.erase(queued);
        }
        if (b->kind == "channel")
            for (auto &s : model.servers)
                if (s.name == b->server)
                    std::erase_if(s.channels, [&](const auto &target) {
                        return fold(b->server, target) == fold(b->server, b->target);
                    });
        if (b->kind == "channel" && b->joined)
            try {
                command(*b, "/part " + b->target);
            } catch (const std::exception &error) {
                report(error.what(), b->server);
            }
        for (auto group = presenceGroups_.begin(); group != presenceGroups_.end();) {
            std::erase_if(group->second.events, [&](const auto &e) {
                return e.server == b->server &&
                       fold(b->server, e.target) == fold(b->server, b->target);
            });
            if (group->second.events.empty())
                group = presenceGroups_.erase(group);
            else
                ++group;
        }
        gaps_.erase(id);
        namesCache_.erase(id);
        namesInProgress_.erase(id);
        std::erase_if(activeHistory_, [&](const auto &item) { return item.second == id; });
        if (const auto request = commandOrigins_.find(model.whoisProfile.requestId);
            request != commandOrigins_.end() && request->second.buffer == id) {
            model.whoisProfile.open = model.whoisProfile.loading = false;
            ++model.whoisProfile.revision;
        }
        std::erase_if(commandOrigins_, [&](const auto &item) { return item.second.buffer == id; });
        if (b->kind != "search")
            messageCount_ -= b->messages.size();
        clearAvatar(*b);
        model.buffers.erase(id);
        ++mentionsLiveRevision_;
        if (model.pendingSourceJump.buffer == id) {
            model.pendingSourceJump.buffer.clear();
            model.pendingSourceJump.message.clear();
            ++model.pendingSourceJump.revision;
        }
        requestedHistory_.erase(id);
        std::erase_if(pending_, [&](const auto &entry) { return entry.second.buffer == id; });
        std::erase_if(activity_,
                      [&](const auto &entry) { return entry.first.starts_with(id + "\n"); });
        for (auto &[key, value] : model.settings.workspace)
            if (value == id &&
                (key == "visible_buffers" || key.starts_with("last_buffer:") ||
                 (!key.empty() && std::all_of(key.begin(), key.end(), [](unsigned char c) {
                     return c >= '0' && c <= '9';
                 }))))
                value.clear();
        if (model.activeBuffer == id)
            model.activeBuffer = buffer(server, "*server*", "server").id;
        return;
    }
    report("Unknown UI action: " + a.type, server);
}
void Application::refreshMentions() {
    string networkKey;
    for (const auto &[name, network] : model.networks)
        networkKey += std::to_string(name.size()) + ":" + name +
                      std::to_string(network.nick.size()) + ":" + network.nick +
                      network.caseMapping;
    if (mentionsStorageRevision_ == storage_.revision() &&
        mentionsBuiltRevision_ == mentionsLiveRevision_ && networkKey == mentionsNetworkKey_)
        return;
    map<string, Buffer> sources;
    try {
        if (savingAllowed_)
            sources = storage_.mentionSources(model.networks);
    } catch (const std::exception &error) {
        model.mentionSources.clear();
        if (auto feed = model.buffers.find(GlobalMentionsBufferId); feed != model.buffers.end()) {
            feed->second.messages.clear();
            reviseMessages(feed->second);
        }
        report(error.what());
        return;
    }
    set<std::pair<string, string>> tombstones;
    for (const auto &[id, source] : unsavedSources_)
        for (const auto &row : source.messages)
            if (row.deleted)
                tombstones.emplace(id, row.id);
    auto merge = [&](const Buffer &live) {
        if ((live.kind != "channel" && live.kind != "query") || live.server.empty() ||
            live.target.empty())
            return;
        auto &source = sources[live.id];
        source.id = live.id;
        source.server = live.server;
        source.target = live.target;
        source.title = live.title;
        source.kind = live.kind;
        map<string, Message> rows;
        for (auto &message : source.messages)
            rows.emplace(message.id, std::move(message));
        source.messages.clear();
        for (const auto &row : live.messages) {
            auto old = rows.find(row.id);
            Message message = row;
            message.deleted |= tombstones.contains({live.id, message.id});
            if (old != rows.end()) {
                message.mention |= old->second.mention;
                message.storageSequence = old->second.storageSequence;
                message.orderingTimestamp = old->second.orderingTimestamp;
                rows.erase(old);
            }
            const auto network = model.networks.find(live.server);
            if (!message.mention && !message.own && !message.presence && !message.deleted &&
                network != model.networks.end())
                message.mention |= actualNickMention(message.text, network->second.nick,
                                                     network->second.caseMapping);
            if (!message.id.empty() && message.mention && !message.own && !message.presence &&
                !message.deleted)
                rows.emplace(message.id, std::move(message));
        }
        for (auto &[key, message] : rows)
            source.messages.push_back(std::move(message));
    };
    for (const auto &[id, source] : unsavedSources_)
        merge(source);
    for (const auto &[id, live] : model.buffers)
        merge(live);
    const auto cutoff = epoch() - int64_t(std::max(0, model.settings.retentionDays)) * 86400;
    auto older = [](const Message &a, const Message &b) {
        const auto at = a.orderingTimestamp > 0 ? a.orderingTimestamp : a.timestamp;
        const auto bt = b.orderingTimestamp > 0 ? b.orderingTimestamp : b.timestamp;
        if (at != bt)
            return at < bt;
        if (markerFraction(a.time) != markerFraction(b.time))
            return markerFraction(a.time) < markerFraction(b.time);
        if (a.storageSequence != b.storageSequence)
            return a.storageSequence < b.storageSequence;
        if (a.arrival != b.arrival)
            return a.arrival < b.arrival;
        return a.id < b.id;
    };
    for (auto &[id, source] : sources) {
        if (model.settings.retentionDays > 0)
            std::erase_if(source.messages, [&](const auto &message) {
                const auto at =
                    message.orderingTimestamp > 0 ? message.orderingTimestamp : message.timestamp;
                return at > 0 && at < cutoff;
            });
        std::stable_sort(source.messages.begin(), source.messages.end(), older);
        source.sourceMessageIndex.clear();
        for (size_t i = 0; i < source.messages.size(); ++i)
            source.sourceMessageIndex.emplace(source.messages[i].id, i);
    }
    model.mentionSources = std::move(sources);
    // Backfilled positives are recorded facts even before a failed live write is retried.
    auto preserveClassified = [&](Buffer &source) {
        const auto retained = model.mentionSources.find(source.id);
        if (retained == model.mentionSources.end())
            return;
        for (auto &row : source.messages)
            row.mention |= retained->second.sourceMessageIndex.contains(row.id);
    };
    for (auto &[id, source] : unsavedSources_)
        preserveClassified(source);
    for (auto &[id, live] : model.buffers) {
        if (live.kind == "mentions")
            continue;
        live.sourceMessageIndex.clear();
        preserveClassified(live);
        for (size_t i = 0; i < live.messages.size(); ++i)
            live.sourceMessageIndex.emplace(live.messages[i].id, i);
    }
    auto &feed = model.buffers[GlobalMentionsBufferId];
    feed.id = GlobalMentionsBufferId;
    feed.kind = "mentions";
    feed.title = feed.target = "Mentions";
    feed.server.clear();
    feed.messages.clear();
    for (const auto &[id, source] : model.mentionSources)
        for (const auto &message : source.messages) {
            auto row = message;
            row.sourceBuffer = id;
            row.sourceId = message.id;
            row.sourceServer = source.server;
            row.sourceTarget = source.target;
            row.id = std::to_string(id.size()) + ":" + id + message.id;
            row.history = true;
            feed.messages.push_back(std::move(row));
        }
    std::stable_sort(feed.messages.begin(), feed.messages.end(),
                     [&](const auto &a, const auto &b) { return older(b, a); });
    reviseMessages(feed);
    if (!model.pendingSourceJump.buffer.empty()) {
        const auto &request = model.pendingSourceJump;
        bool valid = sourceMessage(model, request.buffer, request.message) != nullptr;
        try {
            if (!valid && savingAllowed_) {
                auto message = storage_.message(request.buffer, request.message);
                valid = message && !message->deleted;
            }
        } catch (...) {
            valid = false;
        }
        if (!valid) {
            model.pendingSourceJump.buffer.clear();
            model.pendingSourceJump.message.clear();
            ++model.pendingSourceJump.revision;
        }
    }
    mentionsStorageRevision_ = storage_.revision();
    mentionsBuiltRevision_ = mentionsLiveRevision_;
    mentionsNetworkKey_ = std::move(networkKey);
}

void Application::jumpSource(const string &id, const string &messageId) {
    if (id.empty() || messageId.empty() || id == GlobalMentionsBufferId)
        return;
    refreshMentions();
    auto live = model.buffers.find(id);
    std::optional<Buffer> durableBuffer;
    std::optional<Message> durableMessage;
    const Message *message = nullptr;
    if (live != model.buffers.end())
        for (const auto &row : live->second.messages)
            if (row.id == messageId) {
                if (row.deleted)
                    return;
                message = &row;
                break;
            }
    try {
        if (const auto unsaved = unsavedSources_.find(id); unsaved != unsavedSources_.end())
            for (const auto &row : unsaved->second.messages)
                if (row.id == messageId) {
                    if (row.deleted)
                        return;
                    if (!message)
                        message = &row;
                    break;
                }
        if (savingAllowed_) {
            durableBuffer = storage_.sourceBuffer(id);
            if (!message) {
                durableMessage = storage_.message(id, messageId);
                if (durableMessage)
                    message = &*durableMessage;
            }
        }
        if (!message)
            message = sourceMessage(model, id, messageId);
        const Buffer *context = live != model.buffers.end() ? &live->second
                                : durableBuffer             ? &*durableBuffer
                                                            : nullptr;
        if (!context) {
            auto source = model.mentionSources.find(id);
            if (source != model.mentionSources.end())
                context = &source->second;
        }
        if (!context || !message || message->deleted || context->server.empty() ||
            context->target.empty() || (context->kind != "channel" && context->kind != "query"))
            return;
        if (isFool(model, context->server, message->nick) &&
            effectiveFoolDisplay(model.settings, id, context->target) == FoolDisplay::Hidden)
            return;
        const auto orderedAt =
            message->orderingTimestamp > 0 ? message->orderingTimestamp : message->timestamp;
        if (model.settings.retentionDays > 0 && orderedAt > 0 &&
            orderedAt < epoch() - int64_t(model.settings.retentionDays) * 86400)
            return;
        const Message selected = *message;
        auto history = savingAllowed_ ? storage_.history(id, -1) : vector<Message>{};
        if (live == model.buffers.end()) {
            Buffer reopened;
            reopened.id = id;
            reopened.server = context->server;
            reopened.target = context->target;
            reopened.title = context->title;
            reopened.kind = context->kind;
            reopened.topic = context->topic;
            reopened.muted = context->muted;
            reopened.archived = context->archived;
            if (savingAllowed_)
                storage_.seedChatActivity(reopened);
            live = model.buffers.emplace(id, std::move(reopened)).first;
        }
        auto &target = live->second;
        // Keep the live document (draft, nick state and pending rows) intact.
        map<string, Message> rows;
        for (auto &row : history)
            rows.emplace(row.id, std::move(row));
        auto overlay = [&](const Message &liveRow) {
            auto row = liveRow;
            if (auto old = rows.find(row.id); old != rows.end()) {
                row.storageSequence = old->second.storageSequence;
                row.orderingTimestamp = old->second.orderingTimestamp;
            }
            rows[row.id] = std::move(row);
        };
        if (const auto unsaved = unsavedSources_.find(id); unsaved != unsavedSources_.end())
            for (const auto &row : unsaved->second.messages)
                overlay(row);
        for (const auto &row : target.messages)
            overlay(row);
        overlay(selected);
        messageCount_ -= target.messages.size();
        target.messages.clear();
        for (auto &[key, row] : rows)
            target.messages.push_back(std::move(row));
        std::stable_sort(
            target.messages.begin(), target.messages.end(), [](const auto &a, const auto &b) {
                const auto at = a.orderingTimestamp > 0 ? a.orderingTimestamp : a.timestamp;
                const auto bt = b.orderingTimestamp > 0 ? b.orderingTimestamp : b.timestamp;
                if (at != bt)
                    return at < bt;
                if (markerFraction(a.time) != markerFraction(b.time))
                    return markerFraction(a.time) < markerFraction(b.time);
                if (a.storageSequence != b.storageSequence)
                    return a.storageSequence < b.storageSequence;
                return a.id < b.id;
            });
        messageCount_ += target.messages.size();
        target.sourceMessageIndex.clear();
        for (size_t i = 0; i < target.messages.size(); ++i)
            target.sourceMessageIndex.emplace(target.messages[i].id, i);
        reviseMessages(target);
        model.activeBuffer = id;
        model.pendingSourceJump.buffer = id;
        model.pendingSourceJump.message = messageId;
        ++model.pendingSourceJump.revision;
    } catch (const std::exception &error) {
        report(error.what());
    }
}

void Application::trim() {
    const auto limit = static_cast<size_t>(std::clamp(model.settings.maxMessages, 1, 100000));
    set<string> changed;
    if (model.now - lastPrune_ >= 30) {
        lastPrune_ = model.now;
        const auto wall = epoch();
        const auto cutoff =
            wall - static_cast<int64_t>(std::max(0, model.settings.retentionDays)) * 86400;
        if (model.settings.retentionDays > 0)
            for (auto &[id, b] : model.buffers) {
                if (b.kind == "search" || b.kind == "mentions")
                    continue;
                auto removed = std::erase_if(b.messages, [&](const auto &m) {
                    const auto at = m.orderingTimestamp > 0 ? m.orderingTimestamp : m.timestamp;
                    if (at <= 0 || at >= cutoff)
                        return false;
                    pending_.erase(id + "\n" + m.id);
                    if (auto source = unsavedSources_.find(id); source != unsavedSources_.end())
                        std::erase_if(source->second.messages,
                                      [&](const auto &row) { return row.id == m.id; });
                    return true;
                });
                messageCount_ -= removed;
                if (removed) {
                    reviseMessages(b);
                    changed.insert(id);
                }
            }
        std::erase_if(activity_, [&](const auto &item) {
            return item.second.empty() || item.second.back() < wall - 7 * 86400;
        });
    }
    while (messageCount_ > limit) {
        Buffer *oldest = nullptr;
        int64_t when = std::numeric_limits<int64_t>::max();
        for (auto &[id, b] : model.buffers)
            if (b.kind != "search" && b.kind != "mentions" && !b.messages.empty()) {
                const auto at = b.messages.front().timestamp;
                if (!oldest || at < when) {
                    oldest = &b;
                    when = at;
                }
            }
        if (!oldest) {
            messageCount_ = 0;
            break;
        }
        if (changed.insert(oldest->id).second)
            reviseMessages(*oldest);
        pending_.erase(oldest->id + "\n" + oldest->messages.front().id);
        if (auto source = unsavedSources_.find(oldest->id); source != unsavedSources_.end())
            std::erase_if(source->second.messages,
                          [&](const auto &row) { return row.id == oldest->messages.front().id; });
        oldest->messages.pop_front();
        --messageCount_;
    }
    for (const auto &id : changed)
        recount(model.buffers.at(id));
    while (activity_.size() > limit) {
        auto oldest =
            std::min_element(activity_.begin(), activity_.end(), [](const auto &a, const auto &b) {
                return a.second.back() < b.second.back();
            });
        activity_.erase(oldest);
    }
}
bool Application::save() {
    lastSave_ = model.now;
    if (!savingAllowed_) {
        model.status =
            "Configuration errors must be fixed before saving; original files are unchanged";
        return false;
    }
    try {
        for (auto queued = queuedSends_.begin(); queued != queuedSends_.end();) {
            auto *b = findBuffer(queued->second.buffer);
            if (!b) {
                queued = queuedSends_.erase(queued);
                continue;
            }
            if (queued->second.recovered) {
                storage_.recoverSend(*b, queued->first);
                queued = queuedSends_.erase(queued);
                continue;
            }
            if (queued->second.complete && queued->second.parts &&
                queued->second.confirmed.size() == queued->second.parts) {
                for (const auto &message : b->messages)
                    if (message.own && submissionPart(message.label, queued->first))
                        storage_.update(b->id, message, {}, 0, true);
                storage_.recoverSend(*b, queued->first);
                queued = queuedSends_.erase(queued);
                continue;
            }
            if (queued->second.complete && !queued->second.preserveRaw) {
                const Message *last = nullptr;
                size_t parts = 0;
                for (const auto &m : b->messages)
                    if (m.own && submissionPart(m.label, queued->first)) {
                        storage_.update(b->id, m, {}, 0,
                                        queued->second.confirmed.contains(m.label));
                        last = &m;
                        ++parts;
                    }
                if (last && parts == queued->second.parts &&
                    storage_.update(b->id, *last, queued->first, queued->second.parts)) {
                    queued = queuedSends_.erase(queued);
                    continue;
                }
            }
            ++queued;
        }
        for (auto source = unsavedSources_.begin(); source != unsavedSources_.end();) {
            for (const auto &row : source->second.messages) {
                const bool replay = row.history && !row.presence && !row.deleted &&
                                    !storage_.message(source->first, row.id).has_value();
                int64_t sequence = 0;
                storage_.update(source->first, row, {}, 0, row.sendStatus == SendStatus::Confirmed,
                                &source->second, &sequence);
                if (replay)
                    if (auto live = model.buffers.find(source->first); live != model.buffers.end())
                        live->second.chatReplaySequences.insert(sequence);
            }
            source = unsavedSources_.erase(source);
        }
        storage_.save(model);
        storage_.prune(model.settings);
        return true;
    } catch (const std::exception &e) {
        report(e.what());
        return false;
    }
}
void Application::drainClients() {
    vector<Event> events;
    for (auto &[server, connection] : clients_) {
        auto incoming = connection->poll();
        events.insert(events.end(), std::make_move_iterator(incoming.begin()),
                      std::make_move_iterator(incoming.end()));
    }
    for (const auto &event : events)
        try {
            reduce(event);
        } catch (const std::exception &error) {
            report(error.what(), event.server);
        }
}
void Application::tick(double now) {
    model.effects.clear();
    model.now = now;
    if (model.whoisProfile.loading) {
        const auto request = commandOrigins_.find(model.whoisProfile.requestId);
        if (request == commandOrigins_.end() || model.now - request->second.started >= 120)
            finishWhois("WHOIS response timed out");
        else if (model.whoisProfile.sessionGeneration !=
                 model.networks[model.whoisProfile.server].sessionGeneration)
            finishWhois("Connection changed before WHOIS completed");
    }
    std::erase_if(commandOrigins_,
                  [&](const auto &item) { return model.now - item.second.started >= 120; });
    if (fools_ != model.settings.fools || foolUnread_ != model.settings.foolUnread) {
        fools_ = model.settings.fools;
        foolUnread_ = model.settings.foolUnread;
        // Policy changes recompute from retained rows and existing read markers;
        // they never mark unrelated conversations read.
        for (auto &[id, b] : model.buffers)
            recount(b);
    }
    if (typingEnabled_ && !model.settings.typingSend) {
        for (const auto &[id, b] : model.buffers)
            if (b.kind == "channel" || b.kind == "query")
                if (auto c = client(b.server))
                    try {
                        c->typing(b.target, "disabled");
                    } catch (const std::exception &e) {
                        report(e.what(), b.server);
                    }
    }
    typingEnabled_ = model.settings.typingSend;
    drainClients();
    dispatchActions();
    // Drain local echoes already produced by the worker; later ticks collect the rest.
    drainClients();
    updateAvatars();
    for (auto i = pending_.begin(); i != pending_.end();) {
        auto b = findBuffer(i->second.buffer);
        if (!b) {
            i = pending_.erase(i);
            continue;
        }
        auto m = std::find_if(b->messages.begin(), b->messages.end(),
                              [&](const auto &value) { return value.id == i->second.id; });
        if (m == b->messages.end()) {
            i = pending_.erase(i);
            continue;
        }
        if (now - i->second.sent >= 1 && m->pending) {
            m->pending = false;
            store(*b, *m, true);
        }
        if (now - i->second.sent > 30)
            i = pending_.erase(i);
        else
            ++i;
    }
    for (auto &[id, b] : model.buffers) {
        std::erase_if(b.typing, [&](const auto &entry) {
            return !model.settings.typingReceive || now - entry.second > 10 ||
                   isFool(model, b.server, entry.first);
        });
    }
    std::erase_if(failedLabels_, [&](const auto &entry) { return now - entry.second > 60; });
    std::erase_if(failedSubmissions_, [&](const auto &entry) { return now - entry.second > 60; });
    for (auto i = presenceGroups_.begin(); i != presenceGroups_.end();) {
        if (now - i->second.started < 10) {
            ++i;
            continue;
        }
        if (!i->second.emitted)
            flushPresence(i->second, false);
        i = presenceGroups_.erase(i);
    }
    trim();
    if (now - lastSave_ >= 30)
        save();
    refreshMentions();
}
} // namespace blast
