#include "network.hpp"
#include "protocol.hpp"
#include "sts.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <pthread.h>
#include <set>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace blast {
using irc::WireMessage;
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
struct Failure : std::runtime_error {
    bool fatal;
    Failure(string s, bool f = false) : std::runtime_error(std::move(s)), fatal(f) {}
};
string opensslError(const string &prefix) {
    unsigned long e = ERR_get_error();
    return prefix + (e ? ": " + string(ERR_error_string(e, nullptr)) : "");
}
vector<string> words(const string &s) {
    std::istringstream in(s);
    vector<string> out;
    for (string w; in >> w;)
        out.push_back(std::move(w));
    return out;
}
string upper(string s) {
    for (char &c : s)
        if (c >= 'a' && c <= 'z')
            c -= 32;
    return s;
}
bool token(const string &s) {
    return !s.empty() && s.front() != ':' &&
           std::none_of(s.begin(), s.end(), [](unsigned char c) { return c <= 32 || c == 127; });
}
bool clean(const string &s) { return s.find_first_of("\r\n\0", 0, 3) == string::npos; }
string afterWord(const string &s) {
    auto p = s.find(' ');
    if (p == string::npos)
        return {};
    p = s.find_first_not_of(' ', p);
    return p == string::npos ? string{} : s.substr(p);
}
string base64(const string &input) {
    string out(4 * ((input.size() + 2) / 3), '\0');
    if (!input.empty())
        EVP_EncodeBlock(reinterpret_cast<unsigned char *>(out.data()),
                        reinterpret_cast<const unsigned char *>(input.data()),
                        static_cast<int>(input.size()));
    return out;
}
size_t utf8End(const string &s, size_t begin, size_t count) {
    size_t end = std::min(s.size(), begin + count);
    if (end < s.size())
        while (end > begin && (static_cast<unsigned char>(s[end]) & 0xc0) == 0x80)
            --end;
    if (end == begin)
        throw std::invalid_argument("A UTF-8 character exceeds the IRC payload limit");
    return end;
}
string attr(const WireMessage &m, const string &name) {
    auto it = m.tags.find(name);
    return it == m.tags.end() ? string{} : it->second;
}
int integer(const string &s, int fallback) {
    try {
        size_t p = 0;
        int n = std::stoi(s, &p);
        return p == s.size() ? n : fallback;
    } catch (...) {
        return fallback;
    }
}
string millisecondTimestamp(string s) {
    if (!irc::parseTimestamp(s))
        throw std::invalid_argument("Invalid timestamp");
    if (s.size() > 19 && s[19] != '.')
        s.insert(19, ".000");
    return s;
}
string historyTimestamp(const string &encoded) {
    const auto parsed = irc::parseTimestampParts(encoded);
    if (!parsed)
        throw std::invalid_argument("Invalid history timestamp");
    using namespace std::chrono;
    const sys_seconds instant{seconds{parsed->seconds}};
    const auto day = floor<days>(instant);
    const year_month_day date{day};
    if (static_cast<int>(date.year()) < 1 || static_cast<int>(date.year()) > 9999)
        throw std::invalid_argument("History timestamp is outside the supported UTC range");
    const hh_mm_ss time{instant - day};
    char text[128];
    std::snprintf(text, sizeof(text), "%04d-%02u-%02uT%02lld:%02lld:%02lld.%03dZ",
                  static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
                  static_cast<unsigned>(date.day()), static_cast<long long>(time.hours().count()),
                  static_cast<long long>(time.minutes().count()),
                  static_cast<long long>(time.seconds().count()), parsed->nanoseconds / 1000000);
    return text;
}
std::map<string, string> attributes(const string &text, char separator = ',') {
    std::map<string, string> result;
    size_t p = 0;
    while (p <= text.size()) {
        auto e = text.find(separator, p);
        string v = text.substr(p, e == string::npos ? string::npos : e - p);
        auto q = v.find('=');
        if (!v.empty())
            result[v.substr(0, q)] = q == string::npos ? "" : irc::unescapeTag(v.substr(q + 1));
        if (e == string::npos)
            break;
        p = e + 1;
    }
    return result;
}
const std::set<string> wantedCaps = {"sasl",
                                     "cap-notify",
                                     "message-tags",
                                     "server-time",
                                     "znc.in/server-time-iso",
                                     "batch",
                                     "labeled-response",
                                     "echo-message",
                                     "multi-prefix",
                                     "away-notify",
                                     "account-notify",
                                     "account-tag",
                                     "extended-join",
                                     "chghost",
                                     "setname",
                                     "userhost-in-names",
                                     "invite-notify",
                                     "no-implicit-names",
                                     "draft/no-implicit-names",
                                     "soju.im/no-implicit-names",
                                     "extended-monitor",
                                     "draft/extended-monitor",
                                     "draft/chathistory",
                                     "draft/event-playback",
                                     "draft/read-marker",
                                     "soju.im/read",
                                     "draft/metadata-2",
                                     "standard-replies",
                                     "draft/relaymsg",
                                     "draft/pre-away",
                                     "draft/channel-rename",
                                     "draft/message-redaction",
                                     "draft/account-registration",
                                     "soju.im/bouncer-networks",
                                     "soju.im/bouncer-networks-notify",
                                     "soju.im/search",
                                     "draft/multiline",
                                     "znc.in/playback",
                                     "znc.in/self-message"};
const vector<vector<string>> aliases = {
    {"no-implicit-names", "draft/no-implicit-names", "soju.im/no-implicit-names"},
    {"extended-monitor", "draft/extended-monitor"},
    {"draft/read-marker", "soju.im/read"},
    {"server-time", "znc.in/server-time-iso"}};
} // namespace

struct NetworkClient::Impl {
    struct Request {
        string type, target, text, extra, submission;
        string commandId{};
        string fallbackNick{};
        bool initial = false;
        vector<string> targets{};
        HistoryRequest history{};
        Clock::time_point admitted = Clock::now();
        bool timestampRetry = false;
    };
    struct SentMultiline {
        Request request;
        Clock::time_point deadline;
    };
    struct PendingSend {
        string target, text, folded;
        bool action = false;
        Clock::time_point deadline;
        size_t fallbackPrefixBytes = 0;
        bool writtenWithoutEcho = false;
    };
    struct PendingReaction {
        Request request;
        Clock::time_point deadline;
    };
    struct Resolution {
        std::mutex mutex;
        bool done = false, consumed = false;
        int code = 0;
        vector<std::pair<sockaddr_storage, socklen_t>> addresses;
        int notify = -1;
        ~Resolution() {
            if (notify >= 0)
                ::close(notify);
        }
    };
    struct Delivery {
        vector<Event> events;
        bool closed = false;
        size_t bytes = 0;
    };
    struct Batch {
        string type, target, label, parent;
        string historyOwner{};
        bool history = false;
        std::map<string, string> tags;
        vector<WireMessage> lines;
        size_t bytes = 0;
        std::shared_ptr<Delivery> delivery;
        std::uint64_t order = 0;
    };
    struct History {
        string target, label, batch;
        Clock::time_point sent;
        bool begun = false;
        string direction = "latest";
        int limit = 50;
        size_t count = 0;
        string cursor{};
        std::uint64_t cursorOrder = 0;
        bool initial = false;
        bool unsolicited = false;
        HistoryCursor oldest{}, newest{};
        std::uint64_t oldestOrder = UINT64_MAX, newestOrder = 0;
        bool root = false, rootClosed = false, ended = false;
        string protocolError{}, refType{}, failureCode{};
        Request request{};
    };
    struct Command {
        string id, label, name, target;
        Clock::time_point deadline;
        bool blocked = false;
        bool fallback = true;
        string modeList{};
        bool retired = false;
        bool uncertainTarget = false;
    };
    std::deque<Command> commands;
    std::deque<Command> supplementalCommands;
    bool supplementalSafe = true;
    bool fallbackSafe = true;
    ServerConfig config;
    Identity identity;
    std::atomic<bool> stopping{false};
    std::thread worker;
    std::mutex lifecycle, queueMutex, eventMutex;
    std::deque<Request> requests;
    bool accepting = true; // Preserve enqueue-before-start usage.
    vector<Event> events;
    int wake[2]{-1, -1};
    std::condition_variable eventReady;
    size_t eventBytes = 0, requestBytes = 0, outputBytes = 0, batchBytes = 0;
    short readWant = POLLIN, writeWant = POLLOUT, lastReady = 0;
    int socket = -1;
    SSL_CTX *context = nullptr;
    SSL *tls = nullptr;
    bool ready = false, capEnded = false, saslRunning = false, saslDone = false, fatal = false,
         stsUpgrade = false;
    std::optional<int64_t> stsDuration;
    bool bound = false, deferred = false, reconfigure = false, registrationSent = false,
         registered = false;
    bool metadataSubscribed = false;
    string bindNetwork;
    string sojuCommand, sojuId, sojuLabel;
    Clock::time_point sojuDeadline;
    unsigned nickRetries = 0;
    string nick, mechanism, receive, pingToken;
    Clock::time_point lastReceive, lastPing, registrationDeadline, pingSent;
    std::map<string, string> advertised, isupport;
    std::set<string> enabled, pendingCaps;
    std::map<string, SentMultiline> sentMultiline;
    std::map<string, PendingSend> pendingSends;
    size_t pendingSendBytes = 0;
    std::map<string, PendingReaction> pendingReactions;
    size_t pendingReactionBytes = 0;
    std::uint64_t frameSerial = 0;
    size_t attemptBytes = 0;
    std::map<string, Batch> batches;
    std::map<string, History> histories;
    std::shared_ptr<Resolution> resolution;
    std::deque<Request> historyQueue;
    vector<string> historyPriority;
    string unlabeledHistory;
    std::map<string, std::pair<string, Clock::time_point>> typed;
    // ponytail: at most 4096 channels; scan using current CASEMAPPING rather than
    // maintaining a second index that must be rebuilt when ISUPPORT changes.
    std::map<string, Clock::time_point> joinedChannels;
    std::map<string, string> typingTargets;
    string sessionId =
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()) + "-" +
        std::to_string(reinterpret_cast<std::uintptr_t>(this));
    struct OutputFrame {
        string bytes, label;
        bool echoExpected = false;
    };
    std::deque<OutputFrame> output;
    size_t outputOffset = 0;
    unsigned long serial = 0;
    std::deque<std::shared_ptr<Delivery>> ordered;
    std::shared_ptr<Delivery> capture;
    size_t orderedBytes = 0, orderedCount = 0;
    explicit Impl(const ServerConfig &c, const Identity &i) : config(c), identity(i), nick(i.nick) {
        if (upper(config.sasl) == "NONE")
            config.sasl.clear();
        if (config.bouncer.starts_with("soju:"))
            bindNetwork = config.bouncer.substr(5);
        if (pipe2(wake, O_NONBLOCK | O_CLOEXEC) != 0)
            throw std::runtime_error("Cannot create network wake pipe");
    }
    ~Impl() {
        stop();
        ::close(wake[0]);
        ::close(wake[1]);
    }
    void signal() {
        char c = 0;
        auto ignored = ::write(wake[1], &c, 1);
        (void)ignored;
    }
    void drain() {
        char data[128];
        while (::read(wake[0], data, sizeof(data)) > 0) {
        }
    }
    static size_t eventSize(const Event &e) {
        size_t bytes = sizeof(Event) + e.server.capacity() + e.target.capacity() +
                       e.nick.capacity() + e.text.capacity() + e.id.capacity() + e.time.capacity() +
                       e.reply.capacity() + e.label.capacity();
        for (const auto &[k, v] : e.tags)
            bytes += k.capacity() + v.capacity() + sizeof(k) + sizeof(v) + 4 * sizeof(void *);
        bytes += e.params.capacity() * sizeof(string);
        for (const auto &p : e.params)
            bytes += p.capacity();
        return bytes;
    }
    void deliver(Event e) {
        e.server = config.name;
        size_t bytes = eventSize(e);
        if (bytes > 16 * 1024 * 1024)
            throw Failure("IRC event exceeds receive memory budget");
        std::unique_lock lock(eventMutex);
        eventReady.wait(lock, [&] {
            return stopping || (events.size() < 32768 && eventBytes + bytes <= 16 * 1024 * 1024);
        });
        // Shutdown must retain completed writes and admitted submission handoffs.
        if (stopping && e.type != "sent" && e.type != "state" && e.type != "history_end" &&
            e.type != "error" && !e.tags.contains("command-complete") &&
            !e.tags.contains("rollback") &&
            !(e.tags.contains("pending") && e.tags.contains("submission")))
            return;
        eventBytes += bytes;
        events.push_back(std::move(e));
    }
    static std::uint64_t orderFor(const Event &e) {
        std::uint64_t order = 0;
        auto sequence = e.tags.find("source-order");
        if (sequence != e.tags.end())
            std::from_chars(sequence->second.data(),
                            sequence->second.data() + sequence->second.size(), order);
        return order;
    }
    void emit(Event e) {
        if (e.history && !e.tags.contains("draft/chathistory-context") && e.type != "typing" &&
            e.type != "history_begin" && e.type != "history_end") {
            auto owner = e.tags.find("history-owner");
            auto h = histories.find(owner == e.tags.end() ? e.label : owner->second);
            auto command = e.tags.find("command");
            if (h != histories.end() && (command == e.tags.end() || command->second != "BATCH")) {
                ++h->second.count;
                auto id = e.tags.find("msgid");
                const string noId;
                const string &boundaryId = id != e.tags.end() ? id->second
                                           : (e.type == "reaction" || e.type == "redact") ? noId
                                                                                          : e.id;
                if (orderFor(e) <= h->second.oldestOrder) {
                    h->second.oldestOrder = orderFor(e);
                    h->second.oldest = {boundaryId, e.time};
                }
                if (orderFor(e) >= h->second.newestOrder) {
                    h->second.newestOrder = orderFor(e);
                    h->second.newest = {boundaryId, e.time};
                }
                std::uint64_t order = 0;
                auto sequence = e.tags.find("source-order");
                if (sequence != e.tags.end())
                    std::from_chars(sequence->second.data(),
                                    sequence->second.data() + sequence->second.size(), order);
                auto refs = isupport.find("MSGREFTYPES");
                bool identified =
                    id != e.tags.end() && !id->second.empty() &&
                    !(config.bouncer == "znc" && !has("draft/chathistory")) &&
                    (refs == isupport.end() || refs->second.find("msgid") != string::npos);
                if (order >= h->second.cursorOrder && (identified || !e.time.empty())) {
                    h->second.cursorOrder = order;
                    h->second.cursor = identified ? "msgid=" + id->second : "timestamp=" + e.time;
                }
            }
        }
        bool control = e.type == "sent" || e.type == "state" || e.type == "caps" ||
                       e.type == "error" || e.type == "history_begin" ||
                       e.tags.contains("pending") || e.tags.contains("rollback") ||
                       (e.type == "history_end" && e.tags.contains("error")) ||
                       (e.tags.contains("command-complete") && e.tags.contains("error"));
        if (control || (!capture && ordered.empty())) {
            deliver(std::move(e));
            return;
        }
        e.server = config.name;
        size_t bytes = eventSize(e) + sizeof(Delivery) + 4 * sizeof(void *);
        if (orderedBytes + bytes > 8 * 1024 * 1024 || orderedCount >= 32768)
            throw Failure("Interleaved batch events exceed receive memory budget");
        auto destination = capture;
        if (!destination) {
            destination = std::make_shared<Delivery>();
            destination->closed = true;
            ordered.push_back(destination);
        }
        destination->bytes += bytes;
        destination->events.push_back(std::move(e));
        orderedBytes += bytes;
        ++orderedCount;
    }
    void flushOrdered() {
        while (!ordered.empty() && ordered.front()->closed) {
            auto delivery = std::move(ordered.front());
            ordered.pop_front();
            orderedBytes -= delivery->bytes;
            orderedCount -= delivery->events.size();
            for (auto &e : delivery->events)
                deliver(std::move(e));
        }
    }
    string diagnostic(string s) const {
        for (const auto *secret : {&identity.password, &config.password})
            if (!secret->empty()) {
                size_t p = 0;
                while ((p = s.find(*secret, p)) != string::npos) {
                    s.replace(p, secret->size(), "[redacted]");
                    p += 10;
                }
            }
        return s;
    }
    void error(const string &s, const string &target = {}, const string &l = {}) {
        Event e;
        e.type = "error";
        e.text = diagnostic(s);
        e.target = target;
        e.label = l;
        emit(std::move(e));
    }
    void state(const string &s, const string &err = {}) {
        Event e;
        e.type = "state";
        e.text = s;
        e.nick = nick;
        if (!err.empty())
            e.tags["error"] = diagnostic(err);
        emit(std::move(e));
    }
    string label() { return "blastirc-" + sessionId + "-" + std::to_string(++serial); }
    bool has(const string &c) const { return enabled.contains(c); }
    string fold(const string &s) const {
        auto it = isupport.find("CASEMAPPING");
        return irc::casefold(s, it == isupport.end() ? "rfc1459" : it->second);
    }
    bool own(const string &n) const { return fold(n) == fold(nick); }
    void forgetSend(const string &l) {
        auto it = pendingSends.find(l);
        if (it != pendingSends.end()) {
            pendingSendBytes -= it->second.text.size();
            pendingSends.erase(it);
        }
        auto multi = sentMultiline.find(l);
        if (multi != sentMultiline.end()) {
            attemptBytes -= multi->second.request.text.size();
            sentMultiline.erase(multi);
        }
    }
    void failedSend(const string &target, const string &text, const string &l, const string &reason,
                    const string &commandId = {}) {
        Event e;
        e.type = "error";
        e.target = target;
        e.label = l;
        e.text = diagnostic(reason);
        e.tags["send-failed"] = "1";
        e.tags["original-text"] = text;
        if (!commandId.empty()) {
            e.tags["command-id"] = commandId;
            e.tags["command-complete"] = "1";
        }
        forgetSend(l);
        emit(std::move(e));
    }
    void failAttempt(string l, const string &reason, bool connectionLost = false) {
        auto it = pendingSends.find(l);
        if (it == pendingSends.end())
            return;
        if (connectionLost && it->second.writtenWithoutEcho) {
            forgetSend(l);
            return;
        }
        string target = it->second.target,
               text = it->second.text.substr(it->second.fallbackPrefixBytes);
        if (it->second.action)
            text = "/me " + text;
        failedSend(target, text, l, reason);
    }
    void rejectRequest(const Request &r, const string &reason) {
        if (r.type == "history" || r.type == "search") {
            deliver(historyFailure(r, reason));
            return;
        }
        if (r.type == "history-priority" || r.type == "history-cancel")
            return;
        if (r.type == "soju") {
            sojuFailure(r.text, r.target, reason);
            return;
        }
        if (r.type == "send") {
            failedSend(r.target, r.text, r.submission, reason);
            return;
        }
        if (r.type == "command") {
            string line = r.text;
            if (line.starts_with('/'))
                line.erase(0, 1);
            auto end = line.find(' ');
            string cmd = upper(line.substr(0, end)), args = afterWord(line);
            if (cmd == "ME" || cmd == "MSG" || cmd == "QUERY" || cmd == "PRIVMSG") {
                string target = args.substr(0, args.find(' ')), text = afterWord(args);
                if (cmd == "PRIVMSG" && text.starts_with(':'))
                    text.erase(0, 1);
                if (cmd == "ME")
                    text = "/me " + text;
                else if (text.size() >= 9 && text.starts_with("\1ACTION ") && text.back() == '\1')
                    text = "/me " + text.substr(8, text.size() - 9);
                failedSend(target, text, r.submission, reason, r.commandId);
                return;
            }
        }
        if (!r.commandId.empty()) {
            Event e;
            e.type = "error";
            e.text = diagnostic(reason);
            e.tags = {{"command-id", r.commandId}, {"command-complete", "1"}};
            emit(std::move(e));
            return;
        }
        error(reason, r.target, r.submission);
    }
    void rejectQueued(const string &reason) {
        std::deque<Request> rejected;
        {
            std::lock_guard lock(queueMutex);
            accepting = false;
            rejected.swap(requests);
            requestBytes = 0;
        }
        for (const auto &r : rejected)
            rejectRequest(r, reason);
    }
    // ponytail: at most 4096 pending sends; index by target if large bursts ever
    // make this bounded scan material. Unlabeled errors cannot identify more.
    string rejectedSend(const WireMessage &m, const Event &e) const {
        if (!e.label.empty())
            return pendingSends.contains(e.label) ? e.label : "";
        string found;
        Clock::time_point oldest = Clock::time_point::max();
        for (const auto &[l, p] : pendingSends)
            for (size_t i = 0; i + 1 < m.params.size(); ++i)
                if (fold(m.params[i]) == p.folded && p.deadline < oldest) {
                    found = l;
                    oldest = p.deadline;
                }
        return found;
    }
    void confirmSend(Event &e) {
        if (!e.own || e.history)
            return;
        if (e.label.empty()) {
            Clock::time_point oldest = Clock::time_point::max();
            string target = fold(e.target);
            for (const auto &[l, p] : pendingSends)
                if (p.folded == target && p.text == e.text && p.action == (e.type == "action") &&
                    p.deadline < oldest) {
                    e.label = l;
                    oldest = p.deadline;
                }
        }
        if (!e.label.empty())
            forgetSend(e.label);
    }
    void forgetReaction(const string &l) {
        auto it = pendingReactions.find(l);
        if (it == pendingReactions.end())
            return;
        const auto &r = it->second.request;
        pendingReactionBytes -= r.target.size() + r.text.size() + r.extra.size();
        pendingReactions.erase(it);
    }
    void undoReaction(string l, const string &reason) {
        auto it = pendingReactions.find(l);
        if (it == pendingReactions.end())
            return;
        Request r = it->second.request;
        forgetReaction(l);
        bool remove = r.type != "unreact";
        Event e;
        e.type = "reaction";
        e.target = r.target;
        e.nick = nick;
        e.id = r.extra;
        e.text = r.text;
        e.own = true;
        e.tags["rollback"] = "1";
        if (remove)
            e.tags["remove"] = "1";
        emit(std::move(e));
        error(reason, r.target);
    }
    string rejectedReaction(const WireMessage &m, const Event &e) const {
        if (!e.label.empty())
            return pendingReactions.contains(e.label) ? e.label : "";
        if (m.command == "FAIL" && (m.params.empty() || m.params[0] != "TAGMSG"))
            return {};
        string found;
        Clock::time_point oldest = Clock::time_point::max();
        for (const auto &[l, p] : pendingReactions)
            for (size_t i = 0; i + 1 < m.params.size(); ++i)
                if (fold(m.params[i]) == fold(p.request.target) && p.deadline < oldest) {
                    found = l;
                    oldest = p.deadline;
                }
        return found;
    }
    void confirmReaction(const Event &e) {
        if (!e.own || e.history)
            return;
        if (!e.label.empty()) {
            forgetReaction(e.label);
            return;
        }
        for (const auto &[l, p] : pendingReactions)
            if (fold(p.request.target) == fold(e.target) && p.request.extra == e.id &&
                p.request.text == e.text &&
                (p.request.type == "unreact") == e.tags.contains("remove")) {
                string key = l;
                forgetReaction(key);
                return;
            }
    }
    std::string_view conversationTarget(std::string_view t) const {
        auto statuses = isupport.find("STATUSMSG");
        auto channels = isupport.find("CHANTYPES");
        std::string_view types = channels == isupport.end() ? std::string_view{"#&"}
                                                            : std::string_view{channels->second};
        if (statuses != isupport.end()) {
            auto original = t;
            while (!t.empty() && statuses->second.find(t.front()) != string::npos)
                t.remove_prefix(1);
            if (t.empty() || types.find(t.front()) == std::string_view::npos)
                t = original;
        }
        return t;
    }
    bool channel(const string &target) const {
        auto t = conversationTarget(target);
        auto it = isupport.find("CHANTYPES");
        std::string_view types =
            it == isupport.end() ? std::string_view{"#&"} : std::string_view{it->second};
        return !t.empty() && types.find(t.front()) != std::string_view::npos;
    }
    bool validChannel(const string &target) const {
        return target.size() > 1 && target.size() <= 512 && target.size() + 14 <= coreLimit() &&
               token(target) && channel(target) && conversationTarget(target) == target &&
               target.find_first_of(",:") == string::npos;
    }
    auto joinedChannel(const string &target) {
        const string key = fold(target);
        return std::find_if(joinedChannels.begin(), joinedChannels.end(),
                            [&](const auto &entry) { return fold(entry.first) == key; });
    }
    void start() {
        std::lock_guard lock(lifecycle);
        if (worker.joinable())
            return;
        {
            std::lock_guard queueLock(queueMutex);
            stopping = false;
            accepting = true;
        }
        worker = std::thread([this] { run(); });
    }
    void stop() {
        std::lock_guard lock(lifecycle);
        {
            std::lock_guard queueLock(queueMutex);
            accepting = false;
            stopping = true;
        }
        signal();
        eventReady.notify_all();
        if (worker.joinable())
            worker.join();
        rejectQueued("IRC client stopped before send");
    }
    void enqueue(Request r) {
        size_t bytes = r.type.size() + r.target.size() + r.text.size() + r.extra.size() +
                       r.submission.size() + r.commandId.size() + r.fallbackNick.size() +
                       r.history.first.id.size() + r.history.first.time.size() +
                       r.history.second.id.size() + r.history.second.time.size();
        for (const auto &target : r.targets)
            bytes += target.size();
        std::lock_guard lock(queueMutex);
        if (!accepting)
            throw std::runtime_error("IRC client is stopped or reconnecting");
        if (requests.size() >= 4096 || bytes > 8 * 1024 * 1024 ||
            requestBytes + bytes > 8 * 1024 * 1024)
            throw std::runtime_error("IRC request queue limit reached");
        requestBytes += bytes;
        requests.push_back(std::move(r));
        signal();
    }
    vector<Event> pollEvents() {
        std::lock_guard lock(eventMutex);
        vector<Event> out;
        out.swap(events);
        eventBytes = 0;
        eventReady.notify_all();
        return out;
    }
    void closeTransport() {
        joinedChannels.clear();
        metadataSubscribed = false;
        if (tls) {
            SSL_free(tls);
            tls = nullptr;
        }
        if (context) {
            SSL_CTX_free(context);
            context = nullptr;
        }
        if (socket >= 0) {
            ::close(socket);
            socket = -1;
        }
        if (auto duration = std::exchange(stsDuration, std::nullopt))
            irc::saveSts(config.host, config.port, *duration);
    }
    bool waitFd(short flags, int ms) {
        pollfd fds[2] = {{wake[0], POLLIN, 0}, {socket, flags, 0}};
        int r;
        do {
            r = ::poll(fds, socket >= 0 ? 2 : 1, ms);
        } while (r < 0 && errno == EINTR && !stopping);
        lastReady = socket >= 0 ? fds[1].revents : 0;
        if (fds[0].revents & POLLIN)
            drain();
        if (stopping)
            return false;
        if (r < 0)
            throw Failure(std::strerror(errno));
        if (lastReady & POLLNVAL)
            throw Failure("Invalid socket");
        return r > 0 && socket >= 0 && (lastReady & (flags | POLLHUP | POLLERR));
    }
    bool delay(int ms) {
        auto deadline = Clock::now() + std::chrono::milliseconds(ms);
        while (!stopping && Clock::now() < deadline)
            waitFd(0, std::max(
                          1, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  deadline - Clock::now())
                                                  .count())));
        return !stopping;
    }
    // The resolver owns no client state: even a stuck system DNS lookup cannot
    // make stop() block. Its shared result and notification fd outlive this client.
    vector<std::pair<sockaddr_storage, socklen_t>> resolve() {
        bool launch = !resolution;
        if (resolution) {
            std::lock_guard lock(resolution->mutex);
            launch = resolution->done && resolution->consumed;
        }
        if (launch) {
            resolution = std::make_shared<Resolution>();
            auto result = resolution;
            result->notify = fcntl(wake[1], F_DUPFD_CLOEXEC, 0);
            string host = config.host, port = std::to_string(config.port);
            std::thread([result, host, port] {
                addrinfo hints{};
                hints.ai_family = AF_UNSPEC;
                hints.ai_socktype = SOCK_STREAM;
                addrinfo *list = nullptr;
                int code = getaddrinfo(host.c_str(), port.c_str(), &hints, &list);
                {
                    std::lock_guard lock(result->mutex);
                    result->code = code;
                    for (auto p = list; p && result->addresses.size() < 128; p = p->ai_next)
                        if (p->ai_addrlen <= sizeof(sockaddr_storage)) {
                            sockaddr_storage address{};
                            std::memcpy(&address, p->ai_addr, p->ai_addrlen);
                            result->addresses.emplace_back(address, p->ai_addrlen);
                        }
                    result->done = true;
                }
                if (list)
                    freeaddrinfo(list);
                char c = 0;
                if (result->notify >= 0) {
                    auto ignored = ::write(result->notify, &c, 1);
                    (void)ignored;
                }
            }).detach();
        }
        auto result = resolution;
        auto deadline = Clock::now() + 30s;
        while (!stopping) {
            {
                std::lock_guard lock(result->mutex);
                if (result->done) {
                    result->consumed = true;
                    if (result->code)
                        throw Failure("DNS: " + string(gai_strerror(result->code)));
                    return std::move(result->addresses);
                }
            }
            if (Clock::now() > deadline)
                throw Failure("DNS lookup timed out");
            waitFd(0, 500);
        }
        return {};
    }
    void connectTransport() {
        for (const auto &[address, length] : resolve()) {
            if (stopping)
                return;
            socket = ::socket(address.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (socket < 0)
                continue;
            int r = ::connect(socket, reinterpret_cast<const sockaddr *>(&address), length);
            if (r < 0 && errno != EINPROGRESS) {
                ::close(socket);
                socket = -1;
                continue;
            }
            auto deadline = Clock::now() + 12s;
            while (r < 0 && !stopping && Clock::now() < deadline) {
                if (!waitFd(POLLOUT, 200))
                    continue;
                int err = 0;
                socklen_t n = sizeof(err);
                getsockopt(socket, SOL_SOCKET, SO_ERROR, &err, &n);
                if (err) {
                    errno = err;
                    break;
                }
                r = 0;
            }
            if (!r)
                break;
            ::close(socket);
            socket = -1;
        }
        if (stopping)
            return;
        if (socket < 0)
            throw Failure("Could not connect to " + config.host + ":" +
                          std::to_string(config.port));
        if (!config.tls)
            return;
        context = SSL_CTX_new(TLS_client_method());
        if (!context)
            throw Failure(opensslError("TLS initialization failed"), true);
        SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION);
        SSL_CTX_set_verify(context, SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_default_passwd_cb(context, [](char *, int, int, void *) { return 0; });
        if (SSL_CTX_set_default_verify_paths(context) != 1)
            throw Failure(opensslError("Cannot load trusted certificate authorities"), true);
        if (!identity.certificate.empty()) {
            if (SSL_CTX_use_certificate_chain_file(context, identity.certificate.c_str()) != 1)
                throw Failure(opensslError("Cannot load client certificate"), true);
            string key = identity.key.empty() ? identity.certificate : identity.key;
            if (SSL_CTX_use_PrivateKey_file(context, key.c_str(), SSL_FILETYPE_PEM) != 1 ||
                SSL_CTX_check_private_key(context) != 1)
                throw Failure(opensslError("Cannot load matching client key"), true);
        }
        tls = SSL_new(context);
        if (!tls)
            throw Failure(opensslError("TLS session failed"), true);
        SSL_set_fd(tls, socket);
        X509_VERIFY_PARAM_set_hostflags(SSL_get0_param(tls), X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        std::array<unsigned char, 16> address{};
        bool ip = inet_pton(AF_INET, config.host.c_str(), address.data()) == 1 ||
                  inet_pton(AF_INET6, config.host.c_str(), address.data()) == 1;
        if (ip) {
            if (X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(tls), config.host.c_str()) != 1)
                throw Failure("Invalid TLS IP address", true);
        } else {
            if (SSL_set_tlsext_host_name(tls, config.host.c_str()) != 1 ||
                SSL_set1_host(tls, config.host.c_str()) != 1)
                throw Failure("Invalid TLS hostname", true);
        }
        auto deadline = Clock::now() + 20s;
        while (!stopping) {
            ERR_clear_error();
            int r = SSL_connect(tls);
            if (r == 1) {
                if (SSL_get_verify_result(tls) != X509_V_OK)
                    throw Failure("TLS certificate verification failed", true);
                return;
            }
            int e = SSL_get_error(tls, r);
            if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE)
                throw Failure(opensslError("TLS handshake/certificate verification failed"),
                              e == SSL_ERROR_SSL || SSL_get_verify_result(tls) != X509_V_OK);
            if (Clock::now() > deadline)
                throw Failure("TLS handshake timed out");
            waitFd(e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, 200);
        }
    }
    size_t coreLimit() const {
        auto it = isupport.find("LINELEN");
        return static_cast<size_t>(
            std::clamp(it == isupport.end() ? 512 : integer(it->second, 512), 512, 65536) - 2);
    }
    void wire(WireMessage m, const string &commandId = {}) {
        const bool query = m.command == "WHOIS" || m.command == "WHO" || m.command == "NAMES" ||
                           m.command == "LIST" || m.command == "MODE" || m.command == "TOPIC";
        string wireLabel;
        if (!commandId.empty() && has("labeled-response")) {
            wireLabel = label();
            m.tags["label"] = wireLabel;
        }
        string frame = irc::serialize(m, coreLimit()) + "\r\n";
        if (outputBytes + frame.size() > 8 * 1024 * 1024)
            throw std::invalid_argument("IRC outgoing queue limit reached");
        if (query || !commandId.empty()) {
            expireCommands();
            if (commands.size() >= 128) {
                completeCommand(commands.front(), "Command correlation limit reached");
                commands.pop_front();
                fallbackSafe = false;
            }
            string target = m.params.empty() || m.command == "LIST" ? ""
                            : m.command == "WHOIS"                  ? m.params.back()
                                                                    : m.params.front();
            Command pending{commandId, wireLabel, m.command, target, Clock::now() + 30s};
            pending.fallback = query;
            if (m.command == "TOPIC" && m.params.size() != 1)
                pending.fallback = false;
            if (m.command == "MODE" && m.params.size() != 1) {
                pending.modeList = m.params.size() == 2 ? m.params[1] : "";
                if (pending.modeList.starts_with('+'))
                    pending.modeList.erase(0, 1);
                pending.fallback =
                    pending.modeList == "b" || pending.modeList == "e" || pending.modeList == "I";
            }
            // Missing, wildcard and multi-target queries cannot safely match numeric targets.
            pending.uncertainTarget =
                query && m.command != "LIST" &&
                (target.empty() || target.find_first_of(",*?") != string::npos);
            pending.blocked = pending.uncertainTarget;
            if (pending.label.empty() && pending.fallback &&
                (pending.name == "MODE" || pending.name == "TOPIC"))
                for (auto &other : supplementalCommands)
                    if (sameCommandTarget(other, pending)) {
                        completeCommand(other);
                        other.id.clear();
                        other.blocked = true;
                    }
            for (auto &other : commands)
                if (other.fallback && pending.fallback && other.label.empty() &&
                    pending.label.empty() && other.name == pending.name &&
                    (fold(other.target) == fold(target) || other.uncertainTarget ||
                     pending.uncertainTarget))
                    other.blocked = pending.blocked = true;
            commands.push_back(std::move(pending));
        }
        outputBytes += frame.size();
        output.push_back({std::move(frame), {}});
    }
    void raw(const string &cmd, vector<string> params = {}, std::map<string, string> tags = {}) {
        WireMessage m;
        m.command = cmd;
        m.params = std::move(params);
        m.tags = std::move(tags);
        wire(std::move(m));
    }
    void completeCommand(const Command &command, const string &reason = {}) {
        if (command.id.empty())
            return;
        Event e;
        e.type = "network";
        e.tags = {
            {"kind", "command-complete"}, {"command-id", command.id}, {"command-complete", "1"}};
        e.tags["command-name"] = command.name;
        if (!reason.empty())
            e.tags["error"] = reason;
        emit(std::move(e));
    }
    void retainCommandTombstone(Command command) {
        if (!fallbackSafe)
            return;
        command.id.clear();
        command.blocked = command.retired = true;
        command.deadline = Clock::time_point::max();
        commands.push_back(std::move(command));
    }
    bool sameCommandTarget(const Command &a, const Command &b) const {
        return a.name == b.name &&
               (a.uncertainTarget || b.uncertainTarget || fold(a.target) == fold(b.target));
    }
    void retainSupplemental(Command command, Event *primary = nullptr) {
        command.deadline = Clock::now() + 5s;
        command.blocked = command.blocked || command.uncertainTarget || !supplementalSafe;
        for (auto &other : supplementalCommands)
            if (sameCommandTarget(other, command)) {
                completeCommand(other);
                other.id.clear();
                other.blocked = command.blocked = true;
            }
        for (const auto &other : commands)
            if (other.label.empty() && other.fallback && sameCommandTarget(other, command))
                command.blocked = true;
        if (command.blocked) {
            if (primary && !command.id.empty())
                primary->tags["command-complete"] = "1";
            else
                completeCommand(command);
            command.id.clear();
        }
        if (!supplementalSafe)
            return;
        if (supplementalCommands.size() == 128) {
            // Lost ambiguity evidence cannot safely be replaced during this session.
            for (const auto &other : supplementalCommands)
                completeCommand(other);
            supplementalCommands.clear();
            supplementalSafe = false;
            if (primary && !command.id.empty())
                primary->tags["command-complete"] = "1";
            else
                completeCommand(command);
            return;
        }
        supplementalCommands.push_back(std::move(command));
    }
    void expireCommands() {
        const auto now = Clock::now();
        for (auto &context : supplementalCommands)
            if (context.deadline <= now) {
                completeCommand(context);
                context.id.clear();
                context.blocked = true;
                // Retire the origin, not unresolved late-reply evidence. This
                // bounded marker never participates in primary correlation.
                context.deadline = Clock::time_point::max();
            }
        for (auto it = commands.begin(); it != commands.end();) {
            if (it->retired || it->deadline > now) {
                ++it;
                continue;
            }
            completeCommand(*it, "Command response timed out");
            if (fallbackSafe && it->fallback && it->label.empty()) {
                // ponytail: idless cohort protection lasts until its real terminator
                // or reconnect. Exhausting the bounded table disables fallback this session.
                it->id.clear();
                it->blocked = it->retired = true;
                it->deadline = Clock::time_point::max();
                ++it;
            } else
                it = commands.erase(it);
        }
    }
    // ponytail: bounded scan uses the current IRC casemap, including later ISUPPORT changes.
    void correlateCommand(const WireMessage &m, Event &e) {
        expireCommands();
        if (e.history || m.command == "PRIVMSG" || m.command == "NOTICE" || m.command == "TAGMSG")
            return;
        auto selected = commands.end();
        bool terminal = false;
        if (!e.label.empty()) {
            selected = std::find_if(commands.begin(), commands.end(), [&](const Command &c) {
                return !c.label.empty() && c.label == e.label;
            });
            terminal = attr(m, "batch").empty();
        } else if (fallbackSafe && m.command.size() == 3 && m.command.front() >= '0' &&
                   m.command.front() <= '9') {
            if (m.command == "401" &&
                (!rejectedSend(m, e).empty() || !rejectedReaction(m, e).empty()))
                return;
            const string &n = m.command;
            if (n == "329" || n == "333") {
                if (m.params.size() < 2)
                    return;
                Command query;
                query.name = n == "329" ? "MODE" : "TOPIC";
                query.target = m.params[1];
                auto context = supplementalCommands.end();
                size_t count = 0;
                for (auto it = supplementalCommands.begin(); it != supplementalCommands.end(); ++it)
                    if (sameCommandTarget(*it, query)) {
                        context = it;
                        ++count;
                    }
                const bool active =
                    std::any_of(commands.begin(), commands.end(), [&](const Command &c) {
                        return c.label.empty() && c.fallback && sameCommandTarget(c, query);
                    });
                if (count == 1 && !active && !context->blocked && !context->id.empty()) {
                    e.tags["command-id"] = context->id;
                    string response = n;
                    for (size_t i = 1; i < m.params.size(); ++i)
                        response += " " + m.params[i];
                    e.tags["command-response"] = diagnostic(response);
                    e.tags["command-complete"] = "1";
                    supplementalCommands.erase(context);
                }
                return;
            }
            size_t matches = 0;
            for (auto it = commands.begin(); it != commands.end(); ++it) {
                if (!it->label.empty() || !it->fallback)
                    continue;
                bool end = false, family = false;
                size_t targetIndex = 1;
                if (it->name == "WHOIS") {
                    family = n == "276" || n == "301" || n == "307" || n == "310" || n == "311" ||
                             n == "312" || n == "313" || n == "317" || n == "318" || n == "319" ||
                             n == "320" || n == "330" || n == "335" || n == "338" || n == "378" ||
                             n == "379" || n == "671";
                    end = n == "318";
                } else if (it->name == "WHO") {
                    family = n == "352" || n == "315";
                    end = n == "315";
                } else if (it->name == "NAMES") {
                    family = n == "353" || n == "366";
                    targetIndex = n == "353" ? 2 : 1;
                    end = n == "366";
                } else if (it->name == "LIST") {
                    family = n == "321" || n == "322" || n == "323";
                    end = n == "323";
                } else if (it->name == "MODE") {
                    // Primary responses release the active route; optional metadata
                    // uses a short-lived, nonblocking completed context.
                    family = it->modeList.empty()  ? n == "221" || n == "324"
                             : it->modeList == "b" ? n == "367" || n == "368"
                             : it->modeList == "e" ? n == "348" || n == "349"
                                                   : n == "346" || n == "347";
                    end = n == "221" || n == "324" || n == "347" || n == "349" || n == "368";
                    if (n == "221")
                        targetIndex = 0;
                } else if (it->name == "TOPIC") {
                    family = n == "331" || n == "332";
                    end = family;
                }
                const bool targetError = ((n == "401" || n == "402") && it->name == "WHOIS") ||
                                         ((n == "403" || n == "442" || n == "482") &&
                                          (it->name == "WHO" || it->name == "NAMES" ||
                                           it->name == "MODE" || it->name == "TOPIC"));
                const bool commandError =
                    n == "421" || n == "461" || n == "263" || (n == "416" && it->name == "LIST");
                if (targetError || commandError) {
                    family = true;
                    end = true;
                    targetIndex = 1;
                }
                if (!family || m.params.size() <= targetIndex)
                    continue;
                if (commandError
                        ? upper(m.params[1]) != it->name
                        : it->name != "LIST" && fold(m.params[targetIndex]) != fold(it->target))
                    continue;
                ++matches;
                if (selected == commands.end())
                    selected = it;
                terminal = end;
            }
            // Preserve family identity without guessing a request for ambiguous WHOIS replies.
            if (selected != commands.end() && selected->name == "WHOIS")
                e.tags["command-name"] = "WHOIS";
            if (matches != 1 || selected == commands.end() || selected->blocked) {
                // Each genuine terminator drains one indistinguishable request,
                // without claiming its output. Errors may precede another terminator.
                if (selected != commands.end() && terminal &&
                    (n == "318" || n == "315" || n == "366" || n == "323" || n == "221" ||
                     n == "324" || n == "347" || n == "349" || n == "368" || n == "331" ||
                     n == "332")) {
                    Command retired = std::move(*selected);
                    commands.erase(selected);
                    completeCommand(retired);
                    if (n == "324" || n == "332") {
                        retired.id.clear();
                        retired.blocked = true;
                        retainSupplemental(std::move(retired));
                    }
                }
                return;
            }
        }
        if (selected == commands.end())
            return;
        if (!selected->id.empty()) {
            e.tags["command-id"] = selected->id;
            e.tags["command-name"] = selected->name;
            if (m.command != "ACK") {
                string response = m.command;
                const bool numeric =
                    m.command.size() == 3 && m.command.front() >= '0' && m.command.front() <= '9';
                for (size_t i = numeric ? 1 : 0; i < m.params.size(); ++i)
                    response += " " + m.params[i];
                e.tags["command-response"] = diagnostic(response);
            }
            if (terminal &&
                !(selected->label.empty() && (m.command == "324" || m.command == "332")))
                e.tags["command-complete"] = "1";
        }
        if (terminal) {
            if (m.command == "ACK")
                completeCommand(*selected);
            const bool lateTerminator =
                selected->label.empty() && selected->fallback &&
                (m.command.front() == '4' || m.command.front() == '5' || m.command == "263");
            Command retired = std::move(*selected);
            commands.erase(selected);
            if (retired.label.empty() && (m.command == "324" || m.command == "332"))
                retainSupplemental(std::move(retired), &e);
            if (lateTerminator)
                retainCommandTombstone(std::move(retired));
        }
    }
    void sojuFailure(const string &command, const string &id, const string &reason) {
        Event e;
        e.type = "network";
        e.tags = {{"kind", "bouncer-fail"}, {"command", command}};
        e.id = id;
        e.text = reason;
        emit(std::move(e));
    }
    void flush() {
        while (!output.empty() && !stopping) {
            auto &s = output.front().bytes;
            int n = 0;
            if (tls) {
                ERR_clear_error();
                n = SSL_write(tls, s.data() + outputOffset,
                              static_cast<int>(s.size() - outputOffset));
                if (n <= 0) {
                    int e = SSL_get_error(tls, n);
                    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
                        writeWant = e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
                        return;
                    }
                    throw Failure(opensslError("TLS send failed"));
                }
            } else {
                n = static_cast<int>(
                    ::send(socket, s.data() + outputOffset, s.size() - outputOffset, MSG_NOSIGNAL));
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    writeWant = POLLOUT;
                    return;
                }
                if (n < 0 && errno == EINTR)
                    continue;
                if (n <= 0)
                    throw Failure("Socket send failed");
            }
            writeWant = POLLOUT;
            outputOffset += static_cast<size_t>(n);
            if (outputOffset == s.size()) {
                outputBytes -= s.size();
                string completed = std::move(output.front().label);
                const bool echoExpected = output.front().echoExpected;
                output.pop_front();
                outputOffset = 0;
                if (!completed.empty()) {
                    if (!echoExpected) {
                        auto pending = pendingSends.find(completed);
                        if (pending != pendingSends.end())
                            pending->second.writtenWithoutEcho = true;
                    }
                    Event e;
                    e.type = "sent";
                    e.label = std::move(completed);
                    e.own = true;
                    e.tags["echo-expected"] = echoExpected ? "1" : "0";
                    emit(std::move(e));
                }
            }
        }
    }
    void boundState() const {
        auto within = [](const std::map<string, string> &values) {
            if (values.size() > 512)
                return false;
            size_t bytes = 0;
            for (const auto &[key, value] : values)
                bytes += key.capacity() + value.capacity() + sizeof(key) + sizeof(value) +
                         4 * sizeof(void *);
            return bytes <= 128 * 1024;
        };
        if (!within(advertised) || !within(isupport) || enabled.size() > 512 ||
            pendingCaps.size() > 512)
            throw Failure("IRC capability/ISUPPORT state exceeds receive limits");
    }
    void publishCaps() {
        boundState();
        Event e;
        e.type = "caps";
        e.params.assign(enabled.begin(), enabled.end());
        for (const auto &c : enabled) {
            auto value = advertised.find(c);
            e.tags[c] = value == advertised.end() ? "" : value->second;
        }
        emit(std::move(e));
    }
    bool clientTag(const string &tag) const {
        if (!has("message-tags"))
            return false;
        const auto it = isupport.find("CLIENTTAGDENY");
        return irc::clientTagAllowed(it == isupport.end() ? std::string_view{} : it->second, tag);
    }
    bool metadataEnabled() const { return has("batch") && has("draft/metadata-2"); }
    bool metadataSubscriptions() const {
        auto cap = advertised.find("draft/metadata-2");
        if (!metadataEnabled() || cap == advertised.end())
            return false;
        auto limits = attributes(cap->second);
        auto n = limits.find("max-subs");
        return n == limits.end() || integer(n->second, 0) > 0;
    }
    void enableMetadata() {
        if (!metadataEnabled())
            return;
        metadataSubscribed = metadataSubscriptions();
        if (metadataSubscribed)
            raw("METADATA", {"*", "SUB", "avatar"});
        const auto now = Clock::now();
        for (auto &[target, deadline] : joinedChannels)
            deadline = std::max(deadline, now);
    }
    void clearMetadataSync() {
        metadataSubscribed = false;
        for (auto &[target, deadline] : joinedChannels)
            deadline = {};
        // Capability events bypass ordered delivery. Do not let metadata held
        // behind an unrelated multiline batch survive into the next CAP session.
        for (auto &delivery : ordered) {
            const auto removed = std::erase_if(delivery->events, [](const Event &event) {
                const auto kind = event.tags.find("kind");
                return kind != event.tags.end() && kind->second == "metadata";
            });
            if (!removed)
                continue;
            orderedCount -= removed;
            orderedBytes -= delivery->bytes;
            delivery->bytes = 0;
            for (const auto &event : delivery->events)
                delivery->bytes += eventSize(event) + sizeof(Delivery) + 4 * sizeof(void *);
            orderedBytes += delivery->bytes;
        }
    }
    void postponeMetadata(const WireMessage &m) {
        if (!metadataSubscriptions() || m.params.size() < 2 || m.params.size() > 3 ||
            !own(m.params[0]) || !validChannel(m.params[1]))
            return;
        auto joined = joinedChannel(m.params[1]);
        if (joined == joinedChannels.end())
            return;
        const auto now = Clock::now();
        auto deadline = now + 5s;
        if (m.params.size() == 3) {
            const auto &text = m.params[2];
            if (text.empty() ||
                std::any_of(text.begin(), text.end(), [](char c) { return c < '0' || c > '9'; }))
                return;
            std::uint64_t seconds = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), seconds);
            if (parsed.ec == std::errc{} && seconds == 0)
                return;
            if (parsed.ec != std::errc{} && parsed.ec != std::errc::result_out_of_range)
                return;
            const auto available =
                std::chrono::duration_cast<std::chrono::seconds>(Clock::time_point::max() - now)
                    .count();
            deadline = parsed.ec == std::errc{} && seconds <= static_cast<std::uint64_t>(available)
                           ? now + std::chrono::seconds(seconds)
                           : Clock::time_point::max();
        }
        // Repeated postponements may extend, but never shorten, a server's wait.
        joined->second = std::max(joined->second, deadline);
    }
    void negotiate() {
        std::set<string> selected;
        for (const auto &[c, v] : advertised)
            if (wantedCaps.contains(c) && !enabled.contains(c) && !pendingCaps.contains(c))
                selected.insert(c);
        if (!advertised.contains("draft/chathistory"))
            selected.erase("draft/event-playback");
        if (!advertised.contains("batch"))
            selected.erase("draft/metadata-2");
        if (!advertised.contains("batch") || !advertised.contains("message-tags") ||
            !advertised.contains("standard-replies"))
            selected.erase("draft/multiline");
        if (!advertised.contains("message-tags"))
            selected.erase("draft/message-redaction");
        for (const auto &group : aliases) {
            bool held = false;
            for (const auto &c : group)
                if (enabled.contains(c) || pendingCaps.contains(c))
                    held = true;
            string first;
            for (const auto &c : group) {
                selected.erase(c);
                if (first.empty() && advertised.contains(c))
                    first = c;
            }
            if (!held && !first.empty())
                selected.insert(first);
        }
        bool child = !bindNetwork.empty() || identity.username.find('/') != string::npos;
        if (child && !deferred) {
            const std::set<string> early = {
                "sasl",        "draft/chathistory",     "batch", "message-tags", "labeled-response",
                "server-time", "znc.in/server-time-iso"};
            for (auto it = selected.begin(); it != selected.end();) {
                if (!early.contains(*it) &&
                    !(*it == "soju.im/bouncer-networks" && !bindNetwork.empty()))
                    it = selected.erase(it);
                else
                    ++it;
            }
        }
        if (child && deferred) {
            if (bindNetwork.empty()) {
                selected.erase("soju.im/bouncer-networks");
                selected.erase("soju.im/bouncer-networks-notify");
            }
        }
        string request;
        for (const auto &c : selected) {
            if (request.size() + c.size() + 1 > 400) {
                raw("CAP", {"REQ", request});
                request.clear();
            }
            if (!request.empty())
                request += ' ';
            request += c;
            pendingCaps.insert(c);
        }
        if (!request.empty())
            raw("CAP", {"REQ", request});
        finishCaps();
    }
    void beginRegistration() {
        if (registrationSent)
            return;
        if (!config.password.empty())
            raw("PASS", {config.password});
        raw("NICK", {nick});
        raw("USER", {identity.username, "0", "*", identity.realname});
        registrationSent = true;
    }
    void finishCaps() {
        if (!pendingCaps.empty() || saslRunning)
            return;
        if (capEnded) {
            if (registered && deferred && !ready)
                becameReady();
            return;
        }
        if (!config.sasl.empty() && !saslDone) {
            if (!has("sasl"))
                throw Failure("Server does not support required SASL authentication", true);
            beginSasl();
            return;
        }
        if (!bindNetwork.empty() && !bound) {
            if (!has("soju.im/bouncer-networks"))
                throw Failure("Server does not advertise native bouncer network selection", true);
            raw("BOUNCER", {"BIND", bindNetwork});
            bound = true;
        }
        raw("CAP", {"END"});
        capEnded = true;
    }
    void beginSasl() {
        mechanism = upper(config.sasl);
        if (mechanism == "AUTO")
            mechanism = identity.certificate.empty() ? "PLAIN" : "EXTERNAL";
        if (mechanism != "PLAIN" && mechanism != "EXTERNAL")
            throw Failure("SASL must be PLAIN or EXTERNAL", true);
        if (mechanism == "EXTERNAL" && identity.certificate.empty())
            throw Failure("SASL EXTERNAL requires a client certificate", true);
        auto offered = advertised["sasl"];
        if (!offered.empty() && !attributes(offered).contains(mechanism))
            throw Failure("SASL mechanism not offered by server: " + mechanism, true);
        saslRunning = true;
        raw("AUTHENTICATE", {mechanism});
    }
    void authenticate(const WireMessage &m) {
        if (!saslRunning || m.params.empty() || m.params[0] != "+")
            return;
        string payload;
        if (mechanism == "PLAIN") {
            string authcid = identity.username;
            if (authcid.find('/') == string::npos)
                payload = authcid;
            payload.push_back('\0');
            payload += authcid;
            payload.push_back('\0');
            payload += identity.password;
            payload = base64(payload);
        }
        if (payload.empty()) {
            raw("AUTHENTICATE", {"+"});
            return;
        }
        for (size_t p = 0; p < payload.size(); p += 400)
            raw("AUTHENTICATE", {payload.substr(p, 400)});
        if (payload.size() % 400 == 0)
            raw("AUTHENTICATE", {"+"});
    }
    void sts(const string &value) {
        auto attrs = attributes(value);
        if (!config.tls) {
            auto p = attrs.find("port");
            if (p == attrs.end())
                return;
            const int port = integer(p->second, 0);
            if (port < 1 || port > 65535)
                throw Failure("Invalid STS port", true);
            config.port = port;
            config.tls = true;
            stsUpgrade = true;
            throw Failure("Server requires an STS TLS upgrade");
        }
        auto duration = attrs.find("duration");
        if (duration == attrs.end())
            return;
        int64_t seconds = 0;
        const auto &text = duration->second;
        auto parsed = std::from_chars(text.data(), text.data() + text.size(), seconds);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || seconds < 0)
            throw Failure("Invalid STS duration", true);
        // The port key is only an upgrade policy; on TLS retain the actual port.
        irc::saveSts(config.host, config.port, seconds);
        stsDuration = seconds > 0 ? std::optional<int64_t>{seconds} : std::nullopt;
    }
    void capability(const WireMessage &m) {
        if (m.params.size() < 2)
            return;
        string sub = m.params[1];
        string list = m.params.size() > 2 ? m.params.back() : "";
        if (sub == "LS" || sub == "NEW") {
            for (const auto &item : words(list)) {
                auto p = item.find('=');
                string name = item.substr(0, p);
                string value = p == string::npos ? "" : item.substr(p + 1);
                advertised[name] = value;
                boundState();
                if (name == "sts")
                    sts(value);
            }
            if (sub == "LS" && m.params.size() > 3 && m.params[2] == "*")
                return;
            if (sub == "LS")
                beginRegistration();
            negotiate();
        } else if (sub == "ACK" || sub == "NAK") {
            bool hadMetadata = metadataEnabled(), hadNetworks = has("soju.im/bouncer-networks"),
                 wasReady = ready;
            for (string item : words(list)) {
                bool remove = item.starts_with('-');
                if (remove)
                    item.erase(0, 1);
                auto p = item.find('=');
                string c = item.substr(0, p);
                if (p != string::npos)
                    advertised[c] = item.substr(p + 1);
                pendingCaps.erase(c);
                if (sub == "ACK" && !remove)
                    enabled.insert(c);
                else
                    enabled.erase(c);
                boundState();
                if (c == "sasl" && (sub == "NAK" || remove) && !config.sasl.empty())
                    throw Failure("SASL capability rejected", true);
            }
            publishCaps();
            finishCaps();
            if (!metadataEnabled())
                clearMetadataSync();
            else if (wasReady && (!hadMetadata || metadataSubscribed != metadataSubscriptions()))
                enableMetadata();
            if (config.sojuAccount && wasReady && has("soju.im/bouncer-networks") && !hadNetworks)
                raw("BOUNCER", {"LISTNETWORKS"});
        } else if (sub == "DEL") {
            for (const auto &c : words(list)) {
                if (c == "sts")
                    continue; // Only a verified duration=0 may revoke persistence.
                enabled.erase(c);
                pendingCaps.erase(c);
                advertised.erase(c);
            }
            if (!metadataEnabled())
                clearMetadataSync();
            publishCaps();
            negotiate();
        }
    }
    Event event(const WireMessage &m) {
        Event e;
        e.nick = irc::prefixNick(m.prefix);
        e.own = own(e.nick);
        e.tags = m.tags;
        e.tags["command"] = m.command;
        e.params = m.params;
        e.id = attr(m, "msgid");
        for (const auto &name : {"+reply", "+draft/reply", "reply", "draft/reply"})
            if (e.reply.empty())
                e.reply = attr(m, name);
        e.label = attr(m, "label");
        e.time = attr(m, "time");
        string historyOwner;
        std::uint64_t order = frameSerial;
        string b = attr(m, "batch");
        int depth = 0;
        while (!b.empty() && depth++ < 16) {
            auto it = batches.find(b);
            if (it == batches.end())
                break;
            const auto &context = it->second;
            e.history |= context.history;
            if (historyOwner.empty())
                historyOwner = context.historyOwner;
            if (context.tags.contains("draft/chathistory-context"))
                e.tags["draft/chathistory-context"] = context.tags.at("draft/chathistory-context");
            if (context.history && !context.target.empty() && !e.tags.contains("history-target"))
                e.tags["history-target"] = context.target;
            if (e.label.empty())
                e.label = context.label;
            if (context.type == "soju.im/search")
                e.tags["search"] = "1";
            if (context.type == "draft/multiline") {
                order = context.order;
                for (const auto &[key, value] : context.tags)
                    if (key != "batch" && !e.tags.contains(key))
                        e.tags[key] = value;
                if (e.reply.empty())
                    for (const auto &name : {"+reply", "+draft/reply", "reply", "draft/reply"})
                        if (e.reply.empty()) {
                            auto tag = context.tags.find(name);
                            if (tag != context.tags.end())
                                e.reply = tag->second;
                        }
                if (e.time.empty()) {
                    auto t = context.tags.find("time");
                    if (t != context.tags.end())
                        e.time = t->second;
                }
            }
            if (!e.tags.contains("batch-type") || context.type == "netsplit" ||
                context.type == "netjoin")
                e.tags["batch-type"] = context.type;
            b = context.parent;
        }
        if (e.id.empty()) {
            auto id = e.tags.find("msgid");
            if (id != e.tags.end())
                e.id = id->second;
        }
        if (auto t = irc::parseTimestamp(e.time))
            e.timestamp = *t;
        else
            e.time.clear();
        const bool contextualHistory = e.history;
        const bool playback = m.tags.contains("znc.in/playback") ||
                              ((m.command == "PRIVMSG" || m.command == "NOTICE") &&
                               m.prefix.starts_with("*playback!"));
        e.tags.erase("history-owner");
        e.tags.erase("history-standalone");
        if (!historyOwner.empty())
            e.tags["history-owner"] = historyOwner;
        else if (!contextualHistory && config.bouncer == "znc" && playback)
            e.tags["history-standalone"] = "1";
        if (playback)
            e.history = true;
        if (e.history)
            e.tags["source-order"] = std::to_string(order);
        if (!e.label.empty()) {
            auto h = histories.find(historyOwner.empty() ? e.label : historyOwner);
            if (h != histories.end() && h->second.direction == "search")
                e.tags["search"] = "1";
        }
        e.tags.erase("command-id");
        e.tags.erase("command-complete");
        e.tags.erase("command-response");
        e.tags.erase("command-name");
        return e;
    }
    static string historyDirection(const Request &r) {
        if (r.type == "search")
            return "search";
        switch (r.history.direction) {
        case HistoryDirection::Before:
            return "before";
        case HistoryDirection::After:
            return "after";
        case HistoryDirection::Around:
            return "around";
        case HistoryDirection::Targets:
            return "targets";
        default:
            return "latest";
        }
    }
    static Event historyFailure(const Request &r, const string &reason) {
        Event e;
        e.type = "history_end";
        e.target = r.target;
        e.label = r.submission;
        e.tags = {{"direction", historyDirection(r)},
                  {"limit", std::to_string(r.type == "search" ? 100 : r.history.limit)},
                  {"count", "0"},
                  {"cursor", ""},
                  {"batch", ""},
                  {"error", reason}};
        e.tags["history-owner"] = e.label;
        e.tags["rejected"] = "1";
        if (r.initial)
            e.tags["initial"] = "1";
        return e;
    }
    void beginHistory(History &h, const string &batch = {}) {
        if (h.begun)
            return;
        h.begun = true;
        h.batch = batch;
        Event e;
        e.type = "history_begin";
        e.target = h.target;
        e.label = h.label;
        e.tags["history-owner"] = h.label;
        e.tags["batch"] = batch;
        e.tags["direction"] = h.direction;
        e.tags["limit"] = std::to_string(h.limit);
        if (h.unsolicited)
            e.tags["unsolicited"] = "1";
        if (h.initial)
            e.tags["initial"] = "1";
        emit(std::move(e));
    }
    void endHistory(const string &key, const string &err = {}, bool rejected = false) {
        auto it = histories.find(key);
        if (it == histories.end())
            return;
        Event e;
        e.type = "history_end";
        e.target = it->second.target;
        e.label = it->second.label;
        e.tags["history-owner"] = it->second.label;
        e.tags["batch"] = it->second.batch;
        e.tags["direction"] = it->second.direction;
        e.tags["limit"] = std::to_string(it->second.limit);
        e.tags["count"] = std::to_string(it->second.count);
        e.tags["cursor"] = it->second.cursor;
        e.tags["oldest-id"] = it->second.oldest.id;
        e.tags["oldest-time"] = it->second.oldest.time;
        e.tags["newest-id"] = it->second.newest.id;
        e.tags["newest-time"] = it->second.newest.time;
        e.tags["ref-type"] = it->second.refType;
        if (it->second.ended)
            e.tags["end"] = "1";
        if (!it->second.failureCode.empty())
            e.tags["failure-code"] = it->second.failureCode;
        if (err.empty() && !it->second.protocolError.empty())
            e.tags["error"] = it->second.protocolError;
        else if (err.empty() && (!it->second.root || !it->second.rootClosed))
            e.tags["error"] = "History response did not contain a complete root batch";
        if (err == "History request timed out")
            e.tags["timeout"] = "1";
        if (it->second.initial)
            e.tags["initial"] = "1";
        if (rejected)
            e.tags["rejected"] = "1";
        if (!err.empty())
            e.tags["error"] = err;
        emit(std::move(e));
        histories.erase(it);
        if (unlabeledHistory == key)
            unlabeledHistory.clear();
    }
    void startHistory() {
        if (historyQueue.empty() || !unlabeledHistory.empty() ||
            histories.size() >= (has("labeled-response") ? 6 : 1))
            return;
        // ponytail: at most 128 pending requests; scan rather than maintain a second queue index.
        auto rank = [&](const Request &r) {
            const auto target =
                std::find_if(historyPriority.begin(), historyPriority.end(),
                             [&](const string &t) { return fold(t) == fold(r.target); });
            if (target == historyPriority.begin() && target != historyPriority.end())
                return size_t{0};
            return !r.initial ? size_t{1}
                              : size_t{2} + static_cast<size_t>(target - historyPriority.begin());
        };
        auto next =
            std::min_element(historyQueue.begin(), historyQueue.end(),
                             [&](const Request &a, const Request &b) { return rank(a) < rank(b); });
        if (next->timestampRetry && std::any_of(batches.begin(), batches.end(), [&](const auto &p) {
                return p.second.label == next->submission;
            }))
            return;
        Request r = std::move(*next);
        historyQueue.erase(next);
        string l = r.submission.empty() ? label() : r.submission;
        History h{r.target, l, {}, r.admitted, false, historyDirection(r)};
        h.request = r;
        h.initial = r.initial;
        h.begun = r.timestampRetry;
        int maximum = 100;
        auto page = isupport.find("CHATHISTORY");
        if (page != isupport.end())
            maximum = integer(page->second, 100);
        if (maximum < 0)
            maximum = 100;
        h.limit = r.type == "search"
                      ? 100
                      : std::min(r.history.limit,
                                 r.history.direction == HistoryDirection::Targets ? 100 : 50);
        if (maximum > 0)
            h.limit = std::min(h.limit, maximum);
        histories.emplace(l, std::move(h));
        beginHistory(histories.at(l));
        std::map<string, string> tags;
        if (has("labeled-response"))
            tags["label"] = l;
        else
            unlabeledHistory = l;
        try {
            if (r.type == "search") {
                raw("SEARCH",
                    {"in=" + irc::escapeTag(r.target) + ";text=" + irc::escapeTag(r.text) +
                     ";limit=100"},
                    tags);
            } else if (has("draft/chathistory") || has("chathistory")) {
                const int limit = histories.at(l).limit;
                const auto direction = r.history.direction;
                if (direction == HistoryDirection::Targets) {
                    raw("CHATHISTORY",
                        {"TARGETS", "timestamp=" + historyTimestamp(r.history.first.time),
                         "timestamp=" + historyTimestamp(r.history.second.time),
                         std::to_string(limit)},
                        tags);
                    return;
                }
                string sub = upper(historyDirection(r));
                auto refs = isupport.find("MSGREFTYPES");
                auto supports = [&](const string &kind) {
                    return refs == isupport.end() ||
                           ("," + refs->second + ",").find("," + kind + ",") != string::npos;
                };
                string selector = "*";
                if (!r.timestampRetry && !r.history.first.id.empty() && supports("msgid")) {
                    selector = "msgid=" + r.history.first.id;
                    histories.at(l).refType = "msgid";
                } else if (!r.history.first.time.empty() &&
                           (r.timestampRetry || supports("timestamp"))) {
                    selector = "timestamp=" + historyTimestamp(r.history.first.time);
                    histories.at(l).refType = "timestamp";
                } else if (direction != HistoryDirection::Latest || !r.history.first.id.empty() ||
                           !r.history.first.time.empty())
                    throw std::invalid_argument("No supported history selector");
                raw("CHATHISTORY", {sub, r.target, selector, std::to_string(limit)}, tags);
            } else if (r.history.direction != HistoryDirection::Around &&
                       r.history.direction != HistoryDirection::Targets &&
                       config.bouncer == "znc" && has("znc.in/playback")) {
                unlabeledHistory = l;
                string cursor = "0";
                string bound = r.history.first.time;
                if (bound.empty() && (!r.history.first.id.empty() ||
                                      r.history.direction == HistoryDirection::Before ||
                                      r.history.direction == HistoryDirection::After))
                    throw std::invalid_argument("ZNC playback requires a timestamp cursor");
                bool after = r.history.direction == HistoryDirection::After;
                if (!bound.empty()) {
                    const auto t = irc::parseTimestampParts(bound);
                    if (!t)
                        throw std::invalid_argument("ZNC playback requires a timestamp cursor");
                    int64_t seconds = t->seconds;
                    int fraction = t->nanoseconds;
                    const bool negative = seconds < 0;
                    if (negative && fraction) {
                        ++seconds;
                        fraction = 1000000000 - fraction;
                    }
                    cursor = negative ? "-" + std::to_string(-seconds) : std::to_string(seconds);
                    if (fraction) {
                        const string digits = std::to_string(fraction);
                        cursor += "." + string(9 - digits.size(), '0') + digits;
                        while (cursor.back() == '0')
                            cursor.pop_back();
                    }
                }
                // ZNC routes this privately; PRIVMSG can leak a module query to
                // other attached clients.
                raw("ZNC", {"*playback", "PLAY " + r.target + " " + (after ? cursor : "0") +
                                             (!after && !bound.empty() ? " " + cursor : "")});
            } else
                endHistory(l, "Server does not advertise history playback", true);
        } catch (const std::invalid_argument &e) {
            endHistory(l, e.what(), true);
        }
    }
    string historyRootType(const History &history) const {
        return history.direction == "targets"  ? "draft/chathistory-targets"
               : history.direction == "search" ? "soju.im/search"
               : config.bouncer == "znc" && !has("draft/chathistory") && !has("chathistory")
                   ? "znc.in/playback"
                   : "chathistory";
    }
    void batch(const WireMessage &m) {
        if (m.params.empty() || m.params[0].size() < 2)
            return;
        string id = m.params[0].substr(1);
        if (m.params[0][0] == '+') {
            if (m.params.size() < 2) {
                auto malformed = event(m);
                const string key = malformed.label.empty() ? unlabeledHistory : malformed.label;
                if (!key.empty())
                    endHistory(key, "Malformed history root batch");
                return;
            }
            if (batches.size() >= 128 || batches.contains(id))
                throw Failure("Invalid or excessive IRC batches");
            Batch b;
            b.type = m.params[1];
            for (char &c : b.type)
                if (c >= 'A' && c <= 'Z')
                    c += 'a' - 'A';
            b.target = m.params.size() > 2 ? m.params[2] : "";
            b.parent = attr(m, "batch");
            b.tags = m.tags;
            auto context = event(m);
            b.label = context.label;
            if (auto owner = context.tags.find("history-owner"); owner != context.tags.end())
                b.historyOwner = owner->second;
            if (b.type == "soju.im/bouncer-networks") {
                Event e;
                e.type = "network";
                e.tags = {{"kind", "bouncer-list"}, {"phase", "begin"}};
                e.id = id;
                emit(std::move(e));
            }
            b.history = context.history || b.type == "chathistory" ||
                        b.type == "draft/chathistory" || b.type == "draft/chathistory-targets" ||
                        b.type == "znc.in/playback" || b.type == "soju.im/search";
            b.order = frameSerial;
            string parent = b.parent;
            unsigned depth = 0;
            while (!parent.empty()) {
                if (++depth > 16 || parent == id)
                    throw Failure("IRC batch nesting exceeds safe bounds");
                auto p = batches.find(parent);
                if (p == batches.end())
                    break;
                parent = p->second.parent;
            }
            if (b.label.empty() && !unlabeledHistory.empty() && b.parent.empty()) {
                const auto pending = histories.find(unlabeledHistory);
                if (pending != histories.end() && b.type == historyRootType(pending->second) &&
                    (pending->second.direction == "targets" ||
                     pending->second.direction == "search" ||
                     (!b.target.empty() && fold(b.target) == fold(pending->second.target))))
                    b.label = unlabeledHistory;
            }
            if (auto pending = histories.find(b.label); pending != histories.end()) {
                auto &h = pending->second;
                const auto parentBatch = batches.find(b.parent);
                const bool responseRoot =
                    b.parent.empty() || (parentBatch != batches.end() &&
                                         parentBatch->second.type == "labeled-response");
                if (responseRoot && b.type != "labeled-response") {
                    const string expected = historyRootType(h);
                    if (h.root || b.type != expected ||
                        (h.direction != "targets" && h.direction != "search" &&
                         (b.target.empty() || fold(b.target) != fold(h.target))))
                        h.protocolError = "Unexpected history root batch";
                    h.root = true;
                    h.batch = id;
                    h.ended = b.tags.contains("draft/chathistory-end");
                    b.history = true;
                    b.historyOwner = h.label;
                }
            }
            if (b.history) {
                const bool unsolicited = b.label.empty() && b.parent.empty() &&
                                         (b.type == "chathistory" || b.type == "znc.in/playback");
                if (b.label.empty())
                    b.label = label();
                if (!histories.contains(b.label))
                    histories.emplace(
                        b.label, History{b.target, b.label, id, Clock::now(), false, "latest"});
                auto pending = histories.find(b.label);
                if (b.historyOwner.empty())
                    b.historyOwner = pending->second.label;
                if (unsolicited)
                    pending->second.unsolicited = true;
                if (b.target.empty())
                    b.target = pending->second.target;
                if (pending->second.batch.empty())
                    pending->second.batch = id;
                beginHistory(pending->second, id);
                if (pending->second.request.type.empty() && pending->second.batch == id) {
                    pending->second.root = true;
                    pending->second.ended = b.tags.contains("draft/chathistory-end");
                }
            }
            if (b.type == "draft/multiline") {
                b.delivery = std::make_shared<Delivery>();
                ordered.push_back(b.delivery);
            }
            if (b.type == "netsplit" || b.type == "netjoin") {
                Event e = context;
                e.type = "network";
                e.tags["kind"] = b.type;
                e.id = id;
                e.text = m.params.size() > 2 ? m.params[2] : b.type;
                emit(std::move(e));
            }
            batches[id] = std::move(b);
        } else if (m.params[0][0] == '-') {
            auto it = batches.find(id);
            if (it == batches.end())
                return;
            Batch &b = it->second;
            if (b.type == "soju.im/bouncer-networks") {
                Event e;
                e.type = "network";
                e.tags = {{"kind", "bouncer-list"}, {"phase", "end"}};
                e.id = id;
                emit(std::move(e));
            }
            capture = b.delivery;
            if (b.type == "draft/multiline" && !b.lines.empty()) {
                bool valid = !b.lines.front().tags.contains("draft/multiline-concat");
                const auto &first = b.lines.front();
                bool content = false;
                for (const auto &line : b.lines) {
                    if (line.params.size() != 2 || line.command != first.command ||
                        line.params[0] != first.params[0] ||
                        (line.params[1].empty() && line.tags.contains("draft/multiline-concat")))
                        valid = false;
                    if (line.params.size() == 2 && !line.params[1].empty())
                        content = true;
                }
                if (valid && content) {
                    WireMessage joined = first;
                    string text = first.params[1];
                    for (size_t i = 1; i < b.lines.size(); ++i) {
                        if (!b.lines[i].tags.contains("draft/multiline-concat"))
                            text += '\n';
                        text += b.lines[i].params[1];
                    }
                    joined.params[1] = std::move(text);
                    joined.tags.erase("draft/multiline-concat");
                    dispatch(joined, true);
                } else
                    for (const auto &line : b.lines)
                        if (line.params.size() == 2 && !line.params[1].empty())
                            dispatch(line, true);
            }
            capture.reset();
            if (b.delivery)
                b.delivery->closed = true;
            batchBytes -= b.bytes;
            string batchLabel = b.label, batchType = b.type;
            bool historical = b.history;
            if (auto h = histories.find(batchLabel); h != histories.end() && h->second.batch == id)
                h->second.rootClosed = true;
            if (historical)
                for (const auto &[key, child] : batches)
                    if (child.parent == id)
                        if (auto h = histories.find(batchLabel); h != histories.end())
                            h->second.protocolError = "History batch closed before nested batch";
            batches.erase(it);
            if (historical && !batchLabel.empty()) {
                bool sibling = false;
                for (const auto &[key, other] : batches)
                    if (other.history && other.label == batchLabel)
                        sibling = true;
                const bool wrapped =
                    std::any_of(batches.begin(), batches.end(), [&](const auto &p) {
                        return p.second.type == "labeled-response" && p.second.label == batchLabel;
                    });
                if (!sibling && !wrapped)
                    endHistory(batchLabel);
            }
            if (batchType == "labeled-response" && !batchLabel.empty() &&
                histories.contains(batchLabel))
                endHistory(batchLabel);
            if (!batchLabel.empty()) {
                const bool outer = std::none_of(batches.begin(), batches.end(), [&](const auto &p) {
                    return p.second.label == batchLabel;
                });
                if (outer)
                    for (auto pending = commands.begin(); pending != commands.end(); ++pending)
                        if (pending->label == batchLabel) {
                            completeCommand(*pending);
                            commands.erase(pending);
                            break;
                        }
            }
            flushOrdered();
        }
    }
    void becameReady() {
        if (ready) {
            state("Ready");
            return;
        }
        if (!config.sasl.empty() && !saslDone)
            throw Failure("Registration completed without required SASL", true);
        ready = true;
        state("Ready");
        if (config.sojuAccount && has("soju.im/bouncer-networks"))
            raw("BOUNCER", {"LISTNETWORKS"});
        enableMetadata();
        // Application owns wanted channels, including runtime JOIN/PART intent.
    }
    void support(const WireMessage &m) {
        for (size_t i = 1; i + 1 < m.params.size(); ++i) {
            string v = m.params[i];
            if (v.starts_with('-'))
                isupport.erase(v.substr(1));
            else {
                auto p = v.find('=');
                isupport[v.substr(0, p)] = p == string::npos ? "" : v.substr(p + 1);
            }
            boundState();
        }
        Event e = event(m);
        e.type = "network";
        e.tags.insert(isupport.begin(), isupport.end());
        e.tags["kind"] = "isupport";
        emit(std::move(e));
    }
    void renameTarget(const string &oldTarget, const string &newTarget) {
        const string oldKey = fold(oldTarget), newKey = fold(newTarget);
        auto retarget = [&](string &target) {
            if (fold(target) == oldKey)
                target = newTarget;
        };
        auto joined = joinedChannel(oldTarget);
        if (joined != joinedChannels.end()) {
            auto entry = joinedChannels.extract(joined);
            entry.key() = newTarget;
            auto renamed = joinedChannels.insert(std::move(entry));
            if (!renamed.inserted)
                renamed.position->second =
                    std::max(renamed.position->second, renamed.node.mapped());
        }
        {
            std::lock_guard lock(queueMutex);
            for (auto &request : requests) {
                const size_t before = request.target.size();
                retarget(request.target);
                requestBytes = requestBytes - before + request.target.size();
                for (auto &target : request.targets) {
                    const size_t oldSize = target.size();
                    retarget(target);
                    requestBytes = requestBytes - oldSize + target.size();
                }
            }
            if (requestBytes > 8 * 1024 * 1024)
                throw Failure("Renamed IRC request queue exceeds safe bounds");
        }
        for (auto &[label, pending] : pendingSends)
            if (pending.folded == oldKey) {
                pending.target = newTarget;
                pending.folded = newKey;
            }
        for (auto &[label, pending] : sentMultiline)
            retarget(pending.request.target);
        for (auto &[label, pending] : pendingReactions) {
            const size_t before = pending.request.target.size();
            retarget(pending.request.target);
            pendingReactionBytes = pendingReactionBytes - before + pending.request.target.size();
        }
        if (pendingReactionBytes > 4 * 1024 * 1024)
            throw Failure("Renamed IRC reactions exceed safe bounds");
        for (auto &request : historyQueue)
            retarget(request.target);
        for (auto &target : historyPriority)
            retarget(target);
        for (auto &[label, history] : histories) {
            retarget(history.target);
            retarget(history.request.target);
        }
        for (auto &[id, batch] : batches) {
            retarget(batch.target);
            for (auto &line : batch.lines)
                if (!line.params.empty())
                    retarget(line.params[0]);
        }
        if (oldKey != newKey) {
            auto timer = typed.extract(oldKey);
            if (!timer.empty()) {
                timer.key() = newKey;
                typed.erase(newKey);
                typed.insert(std::move(timer));
            }
            typingTargets.erase(oldKey);
        }
        if (typed.contains(newKey))
            typingTargets[newKey] = newTarget;
    }
    void dispatch(const WireMessage &m, bool assembled = false) {
        if (!assembled)
            ++frameSerial;
        bool earlyCommandReply = false;
        if (!commands.empty() &&
            (m.command == "PONG" || m.command == "CAP" || m.command == "AUTHENTICATE" ||
             m.command == "900" || m.command == "903" || m.command == "902" || m.command == "904" ||
             m.command == "905" || m.command == "906" || m.command == "907" ||
             m.command == "ERROR" || m.command == "464" || m.command == "465" ||
             m.command == "432" || m.command == "433" || m.command == "436" || m.command == "001" ||
             m.command == "005")) {
            auto reply = event(m);
            correlateCommand(m, reply);
            earlyCommandReply = reply.tags.contains("command-id");
            if (earlyCommandReply) {
                reply.type = "network";
                reply.tags["kind"] = "raw";
                if (!m.params.empty())
                    reply.text = m.params.back();
                emit(std::move(reply));
            }
        }
        if (m.command == "PING") {
            if (!m.params.empty())
                raw("PONG", m.params);
            return;
        }
        if (m.command == "PONG") {
            if (!pingToken.empty() &&
                std::find(m.params.begin(), m.params.end(), pingToken) != m.params.end()) {
                Event e;
                e.type = "network";
                e.tags["kind"] = "latency";
                e.text = std::to_string(
                    std::chrono::duration<double, std::milli>(Clock::now() - pingSent).count());
                emit(std::move(e));
                pingToken.clear();
            }
            return;
        }
        if (m.command == "CAP") {
            capability(m);
            return;
        }
        if (m.command == "AUTHENTICATE") {
            authenticate(m);
            return;
        }
        if (!registrationSent && (m.command == "421" || m.command == "451")) {
            if (!config.sasl.empty())
                throw Failure("Server does not support required SASL/CAP", true);
            beginRegistration();
            capEnded = true;
            deferred = true;
            return;
        }
        if (m.command == "900" || m.command == "903") {
            if (m.command == "903") {
                saslDone = true;
                saslRunning = false;
                Event e;
                e.type = "network";
                e.tags = {
                    {"kind", "authentication"}, {"authenticated", "1"}, {"mechanism", mechanism}};
                emit(std::move(e));
                finishCaps();
            }
            return;
        }
        if (m.command == "902" || m.command == "904" || m.command == "905" || m.command == "906" ||
            m.command == "907")
            throw Failure("SASL authentication failed (" + m.command + ")", true);
        if (m.command == "ERROR")
            throw Failure(m.params.empty() ? "Server closed the connection" : m.params.back());
        if (m.command == "464" || m.command == "465")
            throw Failure("Server rejected registration (" + m.command + ")", true);
        if (config.bouncer == "znc" && m.command == "401" && m.params.size() > 1 &&
            m.params[1] == "*playback" && !unlabeledHistory.empty()) {
            endHistory(unlabeledHistory, "ZNC playback module is not available");
            return;
        }
        if (m.command == "432" || m.command == "433" || m.command == "436") {
            if (!ready) {
                if (++nickRetries > 3)
                    throw Failure("Nickname unavailable", true);
                nick += "_";
                raw("NICK", {nick});
            } else if (!earlyCommandReply)
                error(m.params.empty() ? "Nickname unavailable" : m.params.back());
            return;
        }
        if (m.command == "001") {
            if (!m.params.empty())
                nick = m.params[0];
            registered = true;
            if (deferred)
                finishCaps();
            return;
        }
        if (m.command == "005") {
            support(m);
            return;
        }
        if (registered && (m.command == "376" || m.command == "422")) {
            Event settled = event(m);
            settled.type = "network";
            settled.tags["kind"] = "isupport-settled";
            emit(std::move(settled));
        }
        if (m.command == "BATCH") {
            batch(m);
            return;
        }
        auto bit = batches.find(attr(m, "batch"));
        if (!assembled && bit != batches.end() && bit->second.type == "draft/multiline" &&
            (m.command == "PRIVMSG" || m.command == "NOTICE")) {
            size_t bytes = irc::retainedBytes(m);
            if (bit->second.lines.size() >= 4096 || bit->second.bytes + bytes > 4 * 1024 * 1024 ||
                batchBytes + bytes > 8 * 1024 * 1024)
                throw Failure("Multiline batch exceeds safe receive bounds");
            bit->second.bytes += bytes;
            batchBytes += bytes;
            bit->second.lines.push_back(m);
            return;
        }
        Event e = event(m);
        if (auto h = histories.find(e.tags.contains("history-owner") ? e.tags.at("history-owner")
                                                                     : e.label);
            h != histories.end() && h->second.direction == "targets" && m.command != "BATCH" &&
            e.history) {
            if (m.command != "CHATHISTORY" || m.params.size() != 3 ||
                upper(m.params[0]) != "TARGETS" || !token(m.params[1]) ||
                !irc::parseTimestampParts(m.params[2])) {
                h->second.protocolError = "Malformed CHATHISTORY TARGETS record";
                return;
            }
            e.type = "history_target";
            e.label = h->second.label;
            e.target = m.params[1];
            try {
                e.time = historyTimestamp(m.params[2]);
            } catch (const std::invalid_argument &) {
                h->second.protocolError = "Malformed CHATHISTORY TARGETS timestamp";
                return;
            }
            e.timestamp = irc::parseTimestampParts(m.params[2])->seconds;
            emit(std::move(e));
            return;
        }
        correlateCommand(m, e);
        if (!m.params.empty())
            e.target = conversationTarget(m.params[0]);
        if (!e.tags.contains("command-id") &&
            (m.command == "401" || m.command == "404" || m.command == "407" || m.command == "411" ||
             m.command == "412" || m.command == "413" || m.command == "414" ||
             m.command == "439")) {
            string rejected = rejectedSend(m, e);
            string reaction = rejectedReaction(m, e);
            if (!reaction.empty() && (rejected.empty() || pendingReactions.at(reaction).deadline <
                                                              pendingSends.at(rejected).deadline)) {
                undoReaction(reaction,
                             m.params.empty() ? "Server rejected reaction" : m.params.back());
                return;
            }
            if (!rejected.empty()) {
                failAttempt(rejected,
                            m.params.empty() ? "Server rejected message" : m.params.back());
                return;
            }
        }
        if (m.command == "PRIVMSG" || m.command == "NOTICE") {
            if (m.params.size() < 2)
                return;
            e.type = m.command == "NOTICE" ? "notice" : "message";
            e.text = m.params[1];
            if (own(e.target) && !channel(e.target))
                e.target = e.nick;
            // Confirmation happens after CTCP ACTION decoding, including when
            // a server echoes messages but does not implement labeled-response.
            if (e.text.size() >= 2 && e.text.front() == '\1' && e.text.back() == '\1') {
                string ctcp = e.text.substr(1, e.text.size() - 2);
                if (ctcp.starts_with("ACTION ")) {
                    e.type = "action";
                    e.text = ctcp.substr(7);
                } else if (m.command == "PRIVMSG" && !e.history && !e.own) {
                    if (ctcp == "VERSION")
                        raw("NOTICE", {e.nick, "\1VERSION BlastIRC\1"});
                    else if (ctcp.starts_with("PING "))
                        raw("NOTICE", {e.nick, "\1" + ctcp + "\1"});
                }
            }
            confirmSend(e);
            emit(std::move(e));
            return;
        }
        if (m.command == "TAGMSG") {
            if (m.params.empty())
                return;
            if (own(e.target) && !channel(e.target))
                e.target = e.nick;
            string typing = attr(m, "+typing");
            if (typing.empty())
                typing = attr(m, "typing");
            if (!typing.empty() && !e.own && !e.history &&
                (typing == "active" || typing == "paused" || typing == "done")) {
                e.type = "typing";
                e.text = typing;
                emit(e);
            }
            string reaction, removed;
            for (const auto &name : {"+draft/react", "+react", "draft/react", "react"})
                if (reaction.empty())
                    reaction = attr(m, name);
            for (const auto &name : {"+draft/unreact", "+unreact", "draft/unreact", "unreact"})
                if (removed.empty())
                    removed = attr(m, name);
            if ((!reaction.empty() || !removed.empty()) && !e.reply.empty()) {
                e.type = "reaction";
                e.text = removed.empty() ? reaction : removed;
                e.id = e.reply;
                if (!removed.empty())
                    e.tags["remove"] = "1";
                confirmReaction(e);
                emit(std::move(e));
            }
            return;
        }
        if (m.command == "JOIN") {
            if (m.params.empty())
                return;
            e.type = "join";
            if (m.params.size() > 1)
                e.tags["account"] = m.params[1];
            if (m.params.size() > 2)
                e.tags["realname"] = m.params[2];
            if (e.own && !e.history && validChannel(m.params[0])) {
                if (joinedChannel(m.params[0]) == joinedChannels.end()) {
                    if (joinedChannels.size() >= 4096)
                        throw Failure("Joined IRC channel state exceeds receive limits");
                    joinedChannels.emplace(m.params[0],
                                           metadataEnabled() ? Clock::now() : Clock::time_point{});
                }
                if (!has("no-implicit-names") && !has("draft/no-implicit-names") &&
                    !has("soju.im/no-implicit-names")) {
                    // JOIN's implicit NAMES has the same ambiguity as an explicit background query.
                    for (auto &pending : commands)
                        if (pending.name == "NAMES" && fold(pending.target) == fold(e.target))
                            pending.blocked = true;
                    if (commands.size() < 128)
                        commands.push_back({{}, {}, "NAMES", e.target, Clock::now() + 30s});
                    else
                        fallbackSafe = false;
                }
                if (has("no-implicit-names") || has("draft/no-implicit-names") ||
                    has("soju.im/no-implicit-names"))
                    raw("NAMES", {e.target});
                raw("WHO", {e.target});
                if (has("draft/read-marker") || has("soju.im/read"))
                    raw(has("draft/read-marker") ? "MARKREAD" : "READ", {e.target});
            }
            emit(std::move(e));
            return;
        }
        if (m.command == "PART" || m.command == "QUIT" || m.command == "KICK") {
            e.type = m.command == "PART" ? "part" : m.command == "QUIT" ? "quit" : "kick";
            if (m.command == "QUIT") {
                e.target =
                    e.history && e.tags.contains("history-target") ? e.tags["history-target"] : "";
                if (!m.params.empty())
                    e.text = m.params[0];
            } else if (m.params.size() > (m.command == "KICK" ? 2u : 1u))
                e.text = m.params.back();
            if (!e.history) {
                if (m.command == "QUIT" && e.own)
                    joinedChannels.clear();
                else if ((m.command == "PART" && e.own) ||
                         (m.command == "KICK" && m.params.size() > 1 && own(m.params[1]))) {
                    auto joined = joinedChannel(e.target);
                    if (joined != joinedChannels.end())
                        joinedChannels.erase(joined);
                }
            }
            emit(std::move(e));
            return;
        }
        if (m.command == "NICK") {
            e.type = "nick";
            if (!m.params.empty())
                e.text = m.params[0];
            e.target =
                e.history && e.tags.contains("history-target") ? e.tags["history-target"] : "";
            if (e.own && !e.history)
                nick = e.text;
            emit(std::move(e));
            return;
        }
        if (m.command == "TOPIC" || m.command == "332" || m.command == "331") {
            e.type = m.command == "TOPIC" ? "topic" : "topic_current";
            if (m.command != "TOPIC") {
                if (m.params.size() < (m.command == "332" ? 3u : 2u))
                    return;
                e.target = m.params[1];
                if (m.command == "332")
                    e.text = m.params[2];
            } else if (m.params.size() > 1)
                e.text = m.params[1];
            emit(std::move(e));
            return;
        }
        if (m.command == "353" || m.command == "366") {
            e.type = "names";
            if (m.command == "353" && m.params.size() > 3) {
                e.target = m.params[2];
                e.params = words(m.params[3]);
            } else if (m.params.size() > 1) {
                e.target = m.params[1];
                e.params.clear();
                e.tags["complete"] = "1";
            }
            e.tags["PREFIX"] = isupport.contains("PREFIX") ? isupport["PREFIX"] : "(ov)@+";
            emit(std::move(e));
            return;
        }
        if (m.command == "AWAY" || m.command == "301") {
            e.type = "away";
            if (m.command == "301" && m.params.size() > 2) {
                e.nick = m.params[1];
                e.text = m.params[2];
            } else if (!m.params.empty())
                e.text = m.params[0];
            e.own = own(e.nick);
            e.target =
                e.history && e.tags.contains("history-target") ? e.tags["history-target"] : "";
            e.tags["away"] = e.text.empty() ? "0" : "1";
            emit(std::move(e));
            return;
        }
        if (m.command == "352" && m.params.size() > 6) {
            e.type = "away";
            e.target = m.params[1];
            e.nick = m.params[5];
            e.own = own(e.nick);
            e.tags["away"] = m.params[6].find('G') != string::npos ? "1" : "0";
            e.tags["prefix"] = m.params[6];
            emit(std::move(e));
            return;
        }
        if (m.command == "MARKREAD" || m.command == "READ") {
            if (m.params.size() < 2)
                return;
            e.type = "read";
            e.text = m.params[1];
            if (e.text.starts_with("timestamp="))
                e.text.erase(0, 10);
            if (!irc::parseTimestamp(e.text))
                return;
            emit(std::move(e));
            return;
        }
        if (m.command == "REDACT") {
            if (m.params.size() < 2)
                return;
            e.type = "redact";
            e.id = m.params[1];
            if (m.params.size() > 2)
                e.text = m.params[2];
            emit(std::move(e));
            return;
        }
        if (m.command == "BOUNCER" && !m.params.empty() &&
            (m.params[0] == "ADDNETWORK" || m.params[0] == "CHANGENETWORK" ||
             m.params[0] == "DELNETWORK") &&
            m.params.size() == 2) {
            Event ack;
            ack.type = "network";
            ack.tags = {{"kind", "bouncer-ack"}, {"command", m.params[0]}};
            ack.id = m.params[1];
            if (e.tags.contains("command-id")) {
                ack.label = e.label;
                ack.tags["kind"] = "raw";
                ack.tags["command-id"] = e.tags.at("command-id");
                ack.text = "Bouncer accepted " + m.params[0];
                ack.tags["command-response"] = ack.text;
                if (e.tags.contains("command-complete"))
                    ack.tags["command-complete"] = "1";
                emit(std::move(ack));
                return;
            }
            if (sojuCommand == m.params[0] && (sojuId.empty() || sojuId == ack.id) &&
                e.label == sojuLabel) {
                sojuCommand.clear();
                sojuId.clear();
                sojuLabel.clear();
                emit(std::move(ack));
            }
            return;
        }
        if ((m.command == "FAIL" || m.command == "WARN" || m.command == "NOTE") &&
            !m.params.empty() && m.params[0] == "BOUNCER") {
            string command = m.params.size() > 2 ? upper(m.params[2]) : "BOUNCER";
            if (command != "ADDNETWORK" && command != "CHANGENETWORK" && command != "DELNETWORK" &&
                command != "LISTNETWORKS" && command != "BIND")
                command = "BOUNCER";
            if (m.command == "FAIL" && command == "BIND" && bound && !bindNetwork.empty()) {
                if (m.params[1] == "ACCOUNT_REQUIRED")
                    throw Failure(
                        "Bouncer rejected network binding (ACCOUNT_REQUIRED): authenticate with "
                        "SASL PLAIN or EXTERNAL before BIND. Configure the parent account's "
                        "authentication method and selected Identity credentials: PLAIN uses the "
                        "Identity's SASL password, not the Server password; EXTERNAL uses the "
                        "Identity's client certificate and private key.",
                        true);
                throw Failure("Bouncer rejected network binding", true);
            }
            if (e.tags.contains("command-id")) {
                Event safe;
                safe.type = "network";
                safe.label = e.label;
                safe.text = m.command == "FAIL" ? "Bouncer rejected " + command
                                                : "Bouncer notification (attributes redacted)";
                safe.tags = {{"kind", "standard-reply"},
                             {"command", m.command},
                             {"command-id", e.tags.at("command-id")},
                             {"command-response", safe.text}};
                if (e.tags.contains("command-complete"))
                    safe.tags["command-complete"] = "1";
                emit(std::move(safe));
                return;
            }
            const bool hasId = m.params.size() > 4 && m.params[1] == "INVALID_NETID";
            const bool pending = m.command == "FAIL" && !sojuCommand.empty() &&
                                 command == sojuCommand && e.label == sojuLabel &&
                                 (!hasId || sojuId.empty() || m.params[3] == sojuId);
            const bool mutation =
                command == "ADDNETWORK" || command == "CHANGENETWORK" || command == "DELNETWORK";
            if (m.command == "FAIL" && mutation &&
                ((!e.label.empty() && !pending) || (!sojuLabel.empty() && e.label != sojuLabel)))
                return;
            string id = pending ? sojuId : hasId ? m.params[3] : string{};
            if (m.command == "FAIL")
                sojuFailure(command, id, "Bouncer rejected " + command);
            else {
                Event safe;
                safe.type = "network";
                safe.tags = {{"kind", "standard-reply"}, {"command", m.command}};
                safe.text = "Bouncer notification (attributes redacted)";
                emit(std::move(safe));
            }
            if (pending) {
                sojuCommand.clear();
                sojuId.clear();
                sojuLabel.clear();
            }
            return;
        }
        if (m.command == "BOUNCER" && m.params.size() > 1 && m.params[0] == "NETWORK") {
            e.type = "network";
            e.tags["kind"] = "bouncer";
            e.params.clear(); // Never retain a raw credential-bearing attribute frame.
            if (e.tags.contains("command-id"))
                e.tags["command-response"] = "Bouncer network information";
            e.id = m.params[1];
            if (m.params.size() > 2) {
                if (m.params[2] == "*")
                    e.tags["deleted"] = "1";
                else {
                    auto attrs = attributes(m.params[2], ';');
                    e.tags.insert(attrs.begin(), attrs.end());
                    if (attrs.contains("name"))
                        e.text = attrs["name"];
                }
            }
            emit(std::move(e));
            return;
        }
        // Unknown BOUNCER replies may echo mutation attributes, including passwords.
        if (m.command == "BOUNCER") {
            Event safe;
            safe.type = "network";
            safe.tags = {{"kind", "raw"}, {"command", "BOUNCER"}};
            safe.text = "BOUNCER reply (attributes redacted)";
            if (e.tags.contains("command-id")) {
                safe.tags["command-id"] = e.tags.at("command-id");
                safe.tags["command-response"] = safe.text;
                if (e.tags.contains("command-complete"))
                    safe.tags["command-complete"] = "1";
            }
            emit(std::move(safe));
            return;
        }
        if (m.command == "FAIL" || m.command == "WARN" || m.command == "NOTE") {
            e.type = "network";
            e.tags["kind"] = "standard-reply";
            if (!m.params.empty())
                e.text = m.params.back();
            emit(e);
            if (m.command == "FAIL") {
                string rejectedMutation = rejectedReaction(m, e);
                if (!rejectedMutation.empty()) {
                    undoReaction(rejectedMutation, e.text);
                    return;
                }
                auto sent = sentMultiline.find(e.label);
                if (sent != sentMultiline.end() && m.params.size() > 1 && m.params[0] == "BATCH" &&
                    m.params[1].starts_with("MULTILINE_")) {
                    Request retry = std::move(sent->second.request);
                    attemptBytes -= retry.text.size();
                    sentMultiline.erase(sent);
                    // The replacement reuses this label; the rejected closing
                    // frame must not complete its newly admitted first ghost.
                    for (auto &frame : output)
                        if (frame.label == e.label)
                            frame.label.clear();
                    try {
                        sendText(retry.target, retry.text, retry.extra, false, e.label, true,
                                 !retry.submission.empty(), retry.fallbackNick);
                    } catch (const std::invalid_argument &rejection) {
                        failAttempt(e.label, rejection.what());
                    }
                } else {
                    const string historyLabel =
                        !e.label.empty() && histories.contains(e.label) ? e.label
                        : !unlabeledHistory.empty() && !m.params.empty() &&
                                (m.params[0] == "CHATHISTORY" || m.params[0] == "SEARCH")
                            ? unlabeledHistory
                            : "";
                    if (!historyLabel.empty()) {
                        auto &h = histories.at(historyLabel);
                        const string code = m.params.size() > 1 ? m.params[1] : "";
                        if (code == "INVALID_MSGREFTYPE" && h.refType == "msgid" &&
                            !h.request.timestampRetry && !h.request.history.first.time.empty() &&
                            !h.root && h.count == 0) {
                            Request retry = h.request;
                            retry.timestampRetry = true;
                            histories.erase(historyLabel);
                            if (unlabeledHistory == historyLabel)
                                unlabeledHistory.clear();
                            historyQueue.push_front(std::move(retry));
                        } else {
                            h.failureCode = code;
                            endHistory(historyLabel,
                                       e.text.empty() ? "History request rejected" : e.text);
                        }
                        return;
                    }
                    bool chatFailure =
                        !m.params.empty() && (m.params[0] == "PRIVMSG" || m.params[0] == "BATCH");
                    string rejected = chatFailure ? rejectedSend(m, e) : "";
                    if (!rejected.empty())
                        failAttempt(rejected, e.text);
                    else if (!e.tags.contains("command-id"))
                        error(e.text, e.target);
                }
            }
            return;
        }
        if (m.command == "ACK") {
            if (!e.label.empty() && histories.contains(e.label))
                endHistory(e.label);
            if (!e.label.empty())
                forgetReaction(e.label);
            return;
        }
        if (m.command == "774") {
            if (!e.history)
                postponeMetadata(m);
            if (e.tags.contains("command-id")) {
                e.type = "network";
                e.tags["kind"] = "raw";
                if (!m.params.empty())
                    e.text = m.params.back();
                emit(std::move(e));
            }
            return;
        }
        if (m.command == "METADATA" || m.command == "761" || m.command == "766") {
            if (!metadataEnabled()) {
                if (e.tags.contains("command-id")) {
                    e.type = "network";
                    e.tags["kind"] = "raw";
                    if (!m.params.empty())
                        e.text = m.params.back();
                    emit(std::move(e));
                }
                return;
            }
            const bool notification = m.command == "METADATA";
            if ((notification && m.params.size() != 3 && m.params.size() != 4) ||
                (m.command == "761" && m.params.size() != 5) ||
                (m.command == "766" && m.params.size() != 4))
                return;
            if (!notification && m.params[0] != "*" && !own(m.params[0]))
                return;
            const auto &target = m.params[notification ? 0 : 1];
            if (!validChannel(target) && !irc::validNickname(target))
                return;
            e.target = target;
            e.type = "network";
            e.tags["kind"] = "metadata";
            if (m.command == "761" || (notification && m.params.size() == 4))
                e.text = m.params.back();
            emit(std::move(e));
            return;
        }
        if (m.command == "RENAME") {
            if (m.params.size() < 2 || !validChannel(m.params[0]) || !validChannel(m.params[1]))
                return;
            if (!e.history)
                renameTarget(m.params[0], m.params[1]);
            e.type = "network";
            e.tags["kind"] = "rename";
            // Ergo omits the third parameter for an empty reason. Keep the
            // original params, but never mistake the new name for a reason.
            e.text = m.params.size() > 2 ? m.params[2] : "";
            emit(std::move(e));
            return;
        }
        if (m.command == "MODE" || m.command == "324") {
            e.type = "network";
            e.tags["kind"] = "mode";
            emit(std::move(e));
            return;
        }
        e.type = "network";
        e.tags["kind"] = m.command == "MONONLINE" || m.command == "MONOFFLINE" ||
                                 m.command == "730" || m.command == "731"
                             ? "monitor"
                         : m.command == "ACCOUNT"  ? "account"
                         : m.command == "CHGHOST"  ? "chghost"
                         : m.command == "SETNAME"  ? "setname"
                         : m.command == "INVITE"   ? "invite"
                         : m.command == "RELAYMSG" ? "relaymsg"
                         : m.command == "SEARCH"   ? "search"
                                                   : "raw";
        if (!m.params.empty())
            e.text = m.params.back();
        emit(std::move(e));
    }
    void receiveFrames() {
        char data[16384];
        for (int reads = 0; reads < 32 && !stopping; ++reads) {
            int n;
            if (tls) {
                ERR_clear_error();
                n = SSL_read(tls, data, sizeof(data));
                if (n <= 0) {
                    int e = SSL_get_error(tls, n);
                    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
                        readWant = e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
                        return;
                    }
                    if (e == SSL_ERROR_ZERO_RETURN)
                        throw Failure("Server closed TLS connection");
                    throw Failure(opensslError("TLS read failed"));
                }
            } else {
                n = static_cast<int>(::recv(socket, data, sizeof(data), 0));
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    return;
                if (n < 0 && errno == EINTR)
                    continue;
                if (n <= 0)
                    throw Failure("Server closed connection");
            }
            readWant = POLLIN;
            lastReceive = Clock::now();
            receive.append(data, static_cast<size_t>(n));
            size_t consumed = 0, p;
            while ((p = receive.find('\n', consumed)) != string::npos) {
                std::string_view line(receive.data() + consumed, p - consumed);
                if (!line.empty() && line.back() == '\r')
                    line.remove_suffix(1);
                auto message = irc::parse(line, irc::incomingCoreLimit);
                if (!message)
                    throw Failure("Malformed or oversized IRC frame");
                dispatch(*message);
                consumed = p + 1;
            }
            if (consumed)
                receive.erase(0, consumed);
            if (receive.size() > 8191 + irc::incomingCoreLimit + 2)
                throw Failure("Unterminated oversized IRC frame");
            if (tls && SSL_pending(tls) == 0)
                return;
        }
    }
    void sendText(const string &target, const string &input, const string &reply,
                  bool action = false, const string &attemptLabel = {}, bool forceLegacy = false,
                  bool submitted = false, const string &fallbackNick = {}) {
        if (!token(target) || target.find(',') != string::npos)
            throw std::invalid_argument("Invalid message target");
        if (input.find('\0') != string::npos || input.find('\r') != string::npos)
            throw std::invalid_argument("Messages cannot contain NUL or CR");
        if (!fallbackNick.empty() && !irc::validNickname(fallbackNick))
            throw std::invalid_argument("Invalid reply nickname");
        if (input.empty())
            return;
        auto length = isupport.find(channel(target) ? "CHANNELLEN" : "NICKLEN");
        size_t targetLimit = length == isupport.end()
                                 ? coreLimit()
                                 : static_cast<size_t>(std::max(
                                       1, integer(length->second, static_cast<int>(coreLimit()))));
        if (conversationTarget(target).size() > targetLimit)
            throw std::invalid_argument("Message target exceeds server length limit");
        if (input.size() > 4 * 1024 * 1024)
            throw std::invalid_argument("Message exceeds outgoing text budget");
        std::map<string, string> tags;
        string attempt = attemptLabel.empty() ? label() : attemptLabel;
        if (has("labeled-response"))
            tags["label"] = attempt;
        if (!reply.empty()) {
            if (!token(reply))
                throw std::invalid_argument("Invalid reply message id");
            if (clientTag("+reply"))
                tags["+reply"] = reply;
        }
        string effectiveReply = tags.contains("+reply") ? reply : "";
        string prefixed;
        size_t prefixBytes = 0;
        if (effectiveReply.empty() && !fallbackNick.empty()) {
            prefixed = fallbackNick + ": " + input;
            prefixBytes = fallbackNick.size() + 2;
        }
        const string &text = prefixBytes ? prefixed : input;
        if (text.size() > 4 * 1024 * 1024)
            throw std::invalid_argument("Message exceeds outgoing text budget");
        auto limits = attributes(advertised["draft/multiline"]);
        int maxLines =
            limits.contains("max-lines") ? std::max(0, integer(limits["max-lines"], 0)) : 4096;
        int maxBytes = std::max(0, integer(limits["max-bytes"], 0));
        size_t overhead = 10 + target.size();
        if (overhead >= coreLimit())
            throw std::invalid_argument("IRC target leaves no message payload");
        size_t payload = std::min<size_t>(400, coreLimit() - overhead);
        if (action) {
            if (payload <= 9)
                throw std::invalid_argument("IRC target leaves no action payload");
            payload -= 9;
        }
        struct Part {
            string text;
            bool concat;
        };
        vector<Part> parts;
        size_t pos = 0, textBytes = 0;
        while (pos <= text.size()) {
            size_t end = text.find('\n', pos);
            if (end == string::npos)
                end = text.size();
            if (end == pos)
                parts.push_back({"", false});
            else {
                bool concat = false;
                while (pos < end) {
                    size_t cut = utf8End(text, pos, std::min(payload, end - pos));
                    textBytes += cut - pos;
                    parts.push_back({text.substr(pos, cut - pos), concat});
                    concat = true;
                    pos = cut;
                    if (parts.size() > 4096)
                        throw std::invalid_argument("Message has too many IRC components");
                }
            }
            if (end == text.size())
                break;
            pos = end + 1;
        }
        if (parts.size() > 4096)
            throw std::invalid_argument("Message has too many IRC components");
        bool multiline = !forceLegacy && !action && parts.size() > 1 && has("draft/multiline") &&
                         has("batch") && has("message-tags") && has("standard-replies") &&
                         has("labeled-response") && has("echo-message") &&
                         parts.size() <= static_cast<size_t>(maxLines) &&
                         textBytes <= static_cast<size_t>(maxBytes);
        vector<OutputFrame> frames;
        vector<string> labels;
        size_t plannedBytes = 0;
        auto plan = [&](const string &cmd, vector<string> params, std::map<string, string> t = {}) {
            WireMessage m;
            m.command = cmd;
            m.params = std::move(params);
            m.tags = std::move(t);
            string frame = irc::serialize(m, coreLimit()) + "\r\n";
            if (plannedBytes + frame.size() + outputBytes > 8 * 1024 * 1024)
                throw std::invalid_argument("IRC outgoing queue limit reached");
            plannedBytes += frame.size();
            frames.push_back({std::move(frame), {}, has("echo-message")});
        };
        if (multiline) {
            if (sentMultiline.size() >= 128 || attemptBytes + input.size() > 8 * 1024 * 1024)
                throw std::invalid_argument("Too many unconfirmed multiline messages");
            string batchId = "m" + std::to_string(++serial);
            plan("BATCH", {"+" + batchId, "draft/multiline", target}, tags);
            for (const auto &part : parts) {
                std::map<string, string> t{{"batch", batchId}};
                if (part.concat)
                    t["draft/multiline-concat"] = "";
                plan("PRIVMSG", {target, part.text}, std::move(t));
            }
            plan("BATCH", {"-" + batchId});
            frames.back().label = attempt;
        } else {
            for (size_t i = 0; i < parts.size(); ++i) {
                labels.push_back(i == 0      ? attempt
                                 : submitted ? attempt + ".part" + std::to_string(i)
                                             : label());
                auto t = tags;
                if (has("labeled-response"))
                    t["label"] = labels.back();
                plan("PRIVMSG",
                     {target, action ? "\1ACTION " + parts[i].text + "\1" : parts[i].text},
                     std::move(t));
                frames.back().label = labels.back();
            }
        }
        size_t oldBytes = 0;
        auto previous = pendingSends.find(attempt);
        if (previous != pendingSends.end())
            oldBytes = previous->second.text.size();
        size_t count = multiline ? 1 : parts.size();
        if (pendingSends.size() - (previous != pendingSends.end() ? 1 : 0) + count > 4096 ||
            pendingSendBytes - oldBytes + text.size() > 8 * 1024 * 1024)
            throw std::invalid_argument("Too many unconfirmed sends");
        // Validate every frame and the complete byte budget before queueing any
        // of a message, so a long reply tag cannot cause a partially sent paste.
        for (auto &frame : frames)
            output.push_back(std::move(frame));
        outputBytes += plannedBytes;
        forgetSend(attempt);
        auto ghost = [&](const string &text, const string &l, bool complete) {
            Event e;
            e.type = action ? "action" : "message";
            e.target = target;
            e.nick = nick;
            e.text = text;
            e.id = l;
            e.label = l;
            e.reply = effectiveReply;
            e.own = true;
            e.tags["pending"] = "1";
            if (submitted) {
                e.tags["submission"] = attempt;
                if (complete) {
                    e.tags["submission-complete"] = "1";
                    e.tags["submission-parts"] = std::to_string(count);
                }
            }
            const size_t stripped = std::min(prefixBytes, text.size());
            if (stripped)
                e.tags["original-text"] = text.substr(stripped);
            emit(std::move(e));
            // Recovery drops only synthetic prefix bytes; correlation retains wire text.
            prefixBytes -= stripped;
            pendingSends.emplace(
                l, PendingSend{target, text, fold(target), action, Clock::now() + 30s, stripped});
            pendingSendBytes += text.size();
        };
        if (multiline) {
            sentMultiline.emplace(attempt, SentMultiline{Request{"send",
                                                                 target,
                                                                 input,
                                                                 reply,
                                                                 submitted ? attempt : string{},
                                                                 {},
                                                                 fallbackNick},
                                                         Clock::now() + 30s});
            attemptBytes += input.size();
            ghost(text, attempt, true);
        } else
            for (size_t i = 0; i < parts.size(); ++i)
                ghost(parts[i].text, labels[i], i + 1 == parts.size());
        typed.erase(fold(target));
        typingTargets.erase(fold(target));
    }
    void execute(const Request &r) {
        if (r.type == "history-priority") {
            historyPriority = r.targets;
            return;
        }
        if (r.type == "history-cancel") {
            for (auto it = historyQueue.begin(); it != historyQueue.end();)
                if (it->initial && fold(it->target) == fold(r.target)) {
                    auto end = historyFailure(*it, "Initial history request canceled");
                    end.tags["cancelled"] = "1";
                    deliver(std::move(end));
                    it = historyQueue.erase(it);
                } else
                    ++it;
            return;
        }
        if (!ready)
            throw std::invalid_argument("Not connected");
        if (r.type == "soju") {
            if (!config.sojuAccount || !has("soju.im/bouncer-networks"))
                throw std::invalid_argument("Bouncer management unavailable on this session");
            if (!sojuCommand.empty())
                throw std::invalid_argument("A bouncer mutation is already pending");
            vector<string> params{r.text};
            if (!r.target.empty())
                params.push_back(r.target);
            if (r.text != "DELNETWORK")
                params.push_back(r.extra);
            const string l = has("labeled-response") ? label() : string{};
            raw("BOUNCER", std::move(params),
                l.empty() ? std::map<string, string>{} : std::map<string, string>{{"label", l}});
            sojuCommand = r.text;
            sojuId = r.target;
            sojuLabel = l;
            sojuDeadline = Clock::now() + 30s;
            return;
        }
        if (r.type == "send") {
            sendText(r.target, r.text, r.extra, false, r.submission, false, !r.submission.empty(),
                     r.fallbackNick);
            return;
        }
        if (r.type == "history") {
            const auto &h = r.history;
            auto cursorValid = [&](const HistoryCursor &cursor) {
                return cursor.id.size() <= 2048 && (cursor.id.empty() || token(cursor.id)) &&
                       cursor.time.size() <= 128 &&
                       (cursor.time.empty() ||
                        (token(cursor.time) && irc::parseTimestampParts(cursor.time)));
            };
            if ((h.direction != HistoryDirection::Targets &&
                 (!token(r.target) || r.target.size() > 512)) ||
                !cursorValid(h.first) || !cursorValid(h.second) ||
                (h.direction == HistoryDirection::Targets &&
                 (!r.target.empty() || h.first.time.empty() || h.second.time.empty())) ||
                h.limit <= 0 || !token(r.submission) || r.submission.size() > 512)
                throw std::invalid_argument("Invalid history request");
            if (historyQueue.size() >= 128)
                throw std::invalid_argument("Too many queued history requests");
            historyQueue.push_back(r);
            return;
        }
        if (r.type == "search") {
            if (!token(r.target) || r.target.size() > 512 || r.text.empty() ||
                r.text.size() > 4096 || !clean(r.text) || !token(r.submission) ||
                r.submission.size() > 512)
                throw std::invalid_argument("SEARCH needs a bounded target and query");
            if (!has("soju.im/search"))
                throw std::invalid_argument("Server-side search is not advertised");
            if (historyQueue.size() >= 128)
                throw std::invalid_argument("Too many queued history requests");
            historyQueue.push_back(r);
            return;
        }
        if (r.type == "read") {
            if (!token(r.target))
                throw std::invalid_argument("Invalid read marker target");
            if (!has("draft/read-marker") && !has("soju.im/read"))
                return;
            string stamp = r.text.starts_with("timestamp=") ? r.text.substr(10) : r.text;
            raw(has("draft/read-marker") ? "MARKREAD" : "READ",
                {r.target, "timestamp=" + millisecondTimestamp(stamp)});
            return;
        }
        if (r.type == "typing") {
            if (!token(r.target))
                throw std::invalid_argument("Invalid typing target");
            string key = fold(r.target);
            if (r.text == "disabled") {
                typed.erase(key);
                typingTargets.erase(key);
                return;
            }
            if (!clientTag("+typing"))
                return;
            if (r.text != "active" && r.text != "paused" && r.text != "done")
                throw std::invalid_argument("Invalid typing state");
            auto now = Clock::now();
            auto it = typed.find(key);
            if (r.text != "done" && it != typed.end() && it->second.first == r.text &&
                now - it->second.second < 3s)
                return;
            if (typed.size() >= 256 && !typed.contains(key))
                throw std::invalid_argument("Too many active typing targets");
            raw("TAGMSG", {r.target}, {{"+typing", r.text}});
            typed[key] = {r.text, now};
            typingTargets[key] = r.target;
            if (r.text == "done") {
                typed.erase(key);
                typingTargets.erase(key);
            }
            return;
        }
        if (r.type == "react" || r.type == "unreact") {
            if (!token(r.target) || !token(r.extra) || r.text.empty() || !clean(r.text))
                throw std::invalid_argument("Invalid reaction");
            const bool remove = r.type == "unreact";
            string tag = remove ? "+draft/unreact" : "+draft/react";
            if (!clientTag(tag) || !clientTag("+reply"))
                return;
            size_t bytes = r.target.size() + r.text.size() + r.extra.size();
            if (pendingReactions.size() >= 256 || pendingReactionBytes + bytes > 4 * 1024 * 1024)
                throw std::invalid_argument("Too many unconfirmed reactions");
            string l = label();
            std::map<string, string> tags{{tag, r.text}, {"+reply", r.extra}};
            if (has("labeled-response"))
                tags["label"] = l;
            raw("TAGMSG", {r.target}, std::move(tags));
            pendingReactions.emplace(
                l, PendingReaction{
                       Request{remove ? "unreact" : "react", r.target, r.text, r.extra, {}},
                       Clock::now() + 30s});
            pendingReactionBytes += bytes;
            Event e;
            e.type = "reaction";
            e.target = r.target;
            e.nick = nick;
            e.id = r.extra;
            e.text = r.text;
            e.own = true;
            if (remove)
                e.tags["remove"] = "1";
            emit(std::move(e));
            return;
        }
        if (r.type == "redact") {
            if (!token(r.target) || !token(r.extra))
                throw std::invalid_argument("Invalid redaction");
            if (has("draft/message-redaction") && has("message-tags"))
                raw("REDACT", {r.target, r.extra});
            return;
        }
        if (r.type != "command")
            return;
        string line = r.text;
        if (!line.empty() && line.front() == '/')
            line.erase(0, 1);
        auto space = line.find(' ');
        string cmd = upper(line.substr(0, space));
        string rest = space == string::npos ? "" : afterWord(line);
        if (cmd == "ME" || cmd == "MSG" || cmd == "QUERY" || cmd == "PRIVMSG") {
            string target = rest.substr(0, rest.find(' ')), text = afterWord(rest);
            if (!token(target))
                throw std::invalid_argument("Command needs a target");
            if (cmd == "PRIVMSG" && text.starts_with(':'))
                text.erase(0, 1);
            bool action = cmd == "ME" || (text.size() >= 9 && text.starts_with("\1ACTION ") &&
                                          text.back() == '\1');
            if (cmd != "ME" && action)
                text = text.substr(8, text.size() - 9);
            if (!text.empty())
                sendText(target, text, {}, action, r.submission, false, !r.submission.empty());
            if (!r.commandId.empty())
                completeCommand({r.commandId, {}, {}, {}, {}});
            return;
        }
        if (!clean(line))
            throw std::invalid_argument("IRC command contains a line break or NUL");
        if (cmd == "SEARCH") {
            auto list = words(rest);
            if (list.size() < 2 || !token(list[0]) || list[0].size() > 512 || rest.size() > 4096)
                throw std::invalid_argument("SEARCH needs a bounded target and query");
            if (!has("soju.im/search"))
                throw std::invalid_argument("Server-side search is not advertised");
            if (historyQueue.size() >= 128)
                throw std::invalid_argument("Too many queued history requests");
            Request search{"search", list[0], afterWord(rest), {}, {}};
            historyQueue.push_back(std::move(search));
            if (!r.commandId.empty())
                completeCommand({r.commandId, {}, {}, {}, {}});
            return;
        }
        if (cmd == "BOUNCER" &&
            (rest.starts_with("BIND ") || (token(rest) && rest != "LISTNETWORKS"))) {
            string id = rest.starts_with("BIND ") ? afterWord(rest) : rest;
            if (!token(id) || id.size() > 497)
                throw std::invalid_argument("BOUNCER needs a bounded network id");
            bindNetwork = id;
            if (!r.commandId.empty())
                completeCommand({r.commandId, {}, {}, {}, {}});
            reconfigure = true;
            throw Failure("Selecting bouncer network");
        }
        auto message = irc::parse(line, coreLimit());
        if (!message || !message->prefix.empty() || !message->tags.empty())
            throw std::invalid_argument("Invalid raw IRC command");
        if (message->command == "BOUNCER" && !message->params.empty() &&
            upper(message->params[0]) == "LISTNETWORKS" && !config.sojuAccount)
            throw std::invalid_argument("Bouncer catalogs require an account control session");
        if (message->command == "QUIT") {
            wire(*message, r.commandId);
            flush();
            stopping = true;
            return;
        }
        wire(*message, r.commandId);
    }
    void pumpRequests() {
        std::deque<Request> work;
        {
            std::lock_guard lock(queueMutex);
            work.swap(requests);
            requestBytes = 0;
        }
        while (!work.empty()) {
            Request r = std::move(work.front());
            work.pop_front();
            if (stopping) {
                rejectRequest(r, "IRC client stopped before send");
                continue;
            }
            try {
                execute(r);
            } catch (const std::invalid_argument &e) {
                rejectRequest(r, e.what());
            } catch (const std::exception &e) {
                if (!reconfigure &&
                    (r.type == "send" || !r.submission.empty() || !r.commandId.empty()))
                    rejectRequest(r, e.what());
                for (const auto &queued : work)
                    rejectRequest(queued, "Connection changed before send");
                throw;
            }
        }
    }
    void timers() {
        expireCommands();
        if (!sojuCommand.empty() && Clock::now() >= sojuDeadline) {
            sojuFailure(sojuCommand, sojuId, "Bouncer acknowledgement timed out; outcome unknown");
            sojuCommand.clear();
            sojuId.clear();
            sojuLabel.clear();
            // An unlabeled late acknowledgement cannot identify its original request.
            throw Failure("Bouncer acknowledgement timed out");
        }
        auto now = Clock::now();
        if (!ready && now > registrationDeadline)
            throw Failure("IRC registration timed out");
        if (now - lastReceive > 180s)
            throw Failure("IRC connection timed out");
        if (!pingToken.empty() && now - pingSent > 45s)
            throw Failure("IRC PING timed out");
        if (ready && pingToken.empty() && now - lastPing > 60s) {
            pingToken = label();
            pingSent = lastPing = now;
            raw("PING", {pingToken});
        }
        // Drain network-selection CAP NEW/DEL before requesting deferred capabilities.
        if (registered && !deferred) {
            deferred = true;
            negotiate();
        }
        if (ready && metadataEnabled())
            for (auto &[target, deadline] : joinedChannels)
                if (deadline != Clock::time_point{} && now >= deadline) {
                    raw("METADATA", metadataSubscribed ? vector<string>{target, "SYNC"}
                                                       : vector<string>{target, "GET", "avatar"});
                    deadline = {};
                }
        vector<string> expired;
        for (const auto &[key, h] : histories)
            if (now - h.sent >= 35s)
                expired.push_back(key);
        for (const auto &key : expired)
            endHistory(key, "History request timed out");
        for (auto it = historyQueue.begin(); it != historyQueue.end();)
            if (now - it->admitted >= 35s) {
                auto end = historyFailure(*it, "History request timed out");
                end.tags["timeout"] = "1";
                end.tags.erase("rejected");
                deliver(std::move(end));
                it = historyQueue.erase(it);
            } else
                ++it;
        for (auto it = sentMultiline.begin(); it != sentMultiline.end();)
            if (now >= it->second.deadline) {
                attemptBytes -= it->second.request.text.size();
                it = sentMultiline.erase(it);
            } else
                ++it;
        for (auto it = pendingSends.begin(); it != pendingSends.end();)
            if (now >= it->second.deadline) {
                pendingSendBytes -= it->second.text.size();
                it = pendingSends.erase(it);
            } else
                ++it;
        for (auto it = pendingReactions.begin(); it != pendingReactions.end();)
            if (now >= it->second.deadline) {
                const auto &r = it->second.request;
                pendingReactionBytes -= r.target.size() + r.text.size() + r.extra.size();
                it = pendingReactions.erase(it);
            } else
                ++it;
        for (auto it = typed.begin(); it != typed.end();)
            if (now - it->second.second > 30s) {
                typingTargets.erase(it->first);
                it = typed.erase(it);
            } else
                ++it;
        for (auto &[target, t] : typed)
            if (t.first == "active" && now - t.second >= 6s) {
                if (clientTag("+typing"))
                    raw("TAGMSG", {typingTargets[target]}, {{"+typing", "paused"}});
                t = {"paused", now};
            }
        startHistory();
    }
    void resetSession() {
        sojuCommand.clear();
        sojuId.clear();
        sojuLabel.clear();
        ready = false;
        capEnded = false;
        saslRunning = false;
        saslDone = false;
        stsUpgrade = false;
        bound = false;
        deferred = false;
        reconfigure = false;
        registrationSent = false;
        registered = false;
        nickRetries = 0;
        nick = identity.nick;
        ordered.clear();
        capture.reset();
        orderedBytes = orderedCount = 0;
        fallbackSafe = true;
        supplementalSafe = true;
        advertised.clear();
        enabled.clear();
        pendingCaps.clear();
        isupport.clear();
        batches.clear();
        historyQueue.clear();
        unlabeledHistory.clear();
        typed.clear();
        typingTargets.clear();
        joinedChannels.clear();
        output.clear();
        outputOffset = 0;
        receive.clear();
        pingToken.clear();
        batchBytes = outputBytes = 0;
        readWant = POLLIN;
        writeWant = POLLOUT;
        lastReady = 0;
        sentMultiline.clear();
        attemptBytes = 0;
        pendingSends.clear();
        pendingSendBytes = 0;
        pendingReactions.clear();
        pendingReactionBytes = 0;
        lastReceive = lastPing = Clock::now();
        registrationDeadline = Clock::now() + 60s;
    }
    void run() {
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &signals, nullptr);
        int backoff = 1;
        try {
            if (config.host.empty() || config.host.size() > 253 ||
                std::any_of(config.host.begin(), config.host.end(),
                            [](unsigned char c) { return c <= 32 || c == 127; }) ||
                config.port < 1 || config.port > 65535 || !irc::validNickname(identity.nick) ||
                !token(identity.username) || identity.username.size() > 512 ||
                !clean(identity.realname) || identity.realname.size() > 4096 ||
                !clean(config.password) || config.password.size() > 16384 ||
                identity.password.size() > 16384 || identity.password.find('\0') != string::npos)
                throw Failure("Invalid IRC connection settings", true);
            if (auto policy = irc::loadSts(config.host)) {
                config.tls = true;
                config.port = policy->port;
            }
            while (!stopping) {
                resetSession();
                {
                    std::lock_guard lock(queueMutex);
                    if (!stopping)
                        accepting = true;
                }
                try {
                    state("Connecting");
                    connectTransport();
                    if (stopping)
                        break;
                    state("Registering");
                    registrationDeadline = Clock::now() + 60s;
                    raw("CAP", {"LS", "302"});
                    while (!stopping) {
                        pumpRequests();
                        timers();
                        flush();
                        short interested = readWant | (output.empty() ? 0 : writeWant);
                        if (waitFd(interested, 100)) {
                            if (lastReady & (readWant | POLLHUP | POLLERR))
                                receiveFrames();
                            if (!output.empty() && (lastReady & writeWant))
                                flush();
                        }
                        if (ready)
                            backoff = 1;
                    }
                } catch (const Failure &e) {
                    fatal = e.fatal;
                    rejectQueued(e.what());
                    while (!pendingSends.empty())
                        failAttempt(pendingSends.begin()->first, e.what(), true);
                    while (!pendingReactions.empty())
                        undoReaction(pendingReactions.begin()->first, e.what());
                    state(fatal ? "Failed" : "Disconnected", e.what());
                } catch (const std::exception &e) {
                    rejectQueued(e.what());
                    while (!pendingSends.empty())
                        failAttempt(pendingSends.begin()->first, e.what(), true);
                    while (!pendingReactions.empty())
                        undoReaction(pendingReactions.begin()->first, e.what());
                    state("Failed", e.what());
                    fatal = true;
                }
                while (!pendingSends.empty())
                    failAttempt(pendingSends.begin()->first,
                                "Connection closed before message confirmation", true);
                while (!pendingReactions.empty())
                    undoReaction(pendingReactions.begin()->first,
                                 "Connection closed before reaction confirmation");
                if (!sojuCommand.empty()) {
                    sojuFailure(sojuCommand, sojuId,
                                "Connection closed before bouncer confirmation; outcome unknown");
                    sojuCommand.clear();
                    sojuId.clear();
                    sojuLabel.clear();
                }
                for (const auto &pending : commands)
                    completeCommand(pending, "Connection closed");
                commands.clear();
                for (const auto &pending : supplementalCommands)
                    completeCommand(pending, "Connection closed");
                supplementalCommands.clear();
                vector<string> keys;
                for (const auto &[key, h] : histories)
                    keys.push_back(key);
                for (const auto &key : keys)
                    endHistory(key, "Connection closed", true);
                for (const auto &queued : historyQueue)
                    rejectRequest(queued, "Connection closed");
                historyQueue.clear();
                closeTransport();
                enabled.clear();
                publishCaps();
                if (stopping || fatal)
                    break;
                if (stsUpgrade || reconfigure)
                    continue;
                if (!delay(backoff * 1000))
                    break;
                backoff = std::min(backoff * 2, 60);
            }
        } catch (const std::exception &e) {
            rejectQueued(e.what());
            state("Failed", e.what());
        }
        for (const auto &pending : commands)
            completeCommand(pending, "Connection closed");
        commands.clear();
        for (const auto &pending : supplementalCommands)
            completeCommand(pending, "Connection closed");
        supplementalCommands.clear();
        rejectQueued("Connection closed before send");
        try {
            closeTransport();
        } catch (const std::exception &e) {
            state("Failed", e.what());
        }
        if (stopping)
            state("Disconnected");
    }
};
NetworkClient::NetworkClient(const ServerConfig &c, const Identity &i)
    : impl_(std::make_unique<Impl>(c, i)) {}
NetworkClient::~NetworkClient() = default;
void NetworkClient::start() { impl_->start(); }
void NetworkClient::stop() { impl_->stop(); }
void NetworkClient::send(const string &target, const string &text, const string &reply,
                         const string &submission, const string &fallbackNick) {
    impl_->enqueue({"send", target, text, reply, submission, {}, fallbackNick});
}
void NetworkClient::command(const string &line, const string &submission, const string &commandId) {
    impl_->enqueue({"command", {}, line, {}, submission, commandId});
}
void NetworkClient::sojuMutation(const string &command, const string &id,
                                 const std::map<string, string> &attrs) {
    const string cmd = upper(command);
    if (cmd != "ADDNETWORK" && cmd != "CHANGENETWORK" && cmd != "DELNETWORK")
        throw std::invalid_argument("Invalid bouncer mutation command");
    if ((cmd == "ADDNETWORK" && !id.empty()) ||
        (cmd != "ADDNETWORK" && (!token(id) || id.size() > 497 || id == "*")) ||
        (cmd == "DELNETWORK" && !attrs.empty()) || (cmd == "CHANGENETWORK" && attrs.empty()) ||
        (cmd == "ADDNETWORK" && (!attrs.contains("host") || attrs.at("host").empty())))
        throw std::invalid_argument("Invalid bouncer mutation parameters");
    string encoded;
    for (const auto &[key, value] : attrs) {
        if (key.empty() || key.size() > 128 ||
            std::any_of(key.begin(), key.end(),
                        [](unsigned char c) {
                            return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                     (c >= '0' && c <= '9') || c == '-' || c == '/' || c == '.');
                        }) ||
            std::any_of(value.begin(), value.end(),
                        [](unsigned char c) { return c < 32 || c == 127; }))
            throw std::invalid_argument("Invalid bouncer attribute");
        if (!encoded.empty())
            encoded += ';';
        encoded += key + "=" + irc::escapeTag(value);
        if (encoded.size() > 65536)
            throw std::invalid_argument("Bouncer attributes exceed safe bounds");
    }
    impl_->enqueue({"soju", id, cmd, std::move(encoded), {}});
}
void NetworkClient::history(const HistoryRequest &history) {
    Impl::Request request{"history", history.target, {}, {}, history.requestId};
    request.history = history;
    request.initial = history.initial;
    impl_->enqueue(std::move(request));
}
void NetworkClient::search(const string &target, const string &query, const string &requestId) {
    impl_->enqueue({"search", target, query, {}, requestId});
}
void NetworkClient::prioritizeHistory(const vector<string> &targets) {
    Impl::Request request{"history-priority", {}, {}, {}, {}};
    request.targets = targets;
    impl_->enqueue(std::move(request));
}
void NetworkClient::cancelInitialHistory(const string &target) {
    impl_->enqueue({"history-cancel", target, {}, {}, {}});
}
void NetworkClient::markRead(const string &target, const string &timestamp) {
    impl_->enqueue({"read", target, timestamp, {}, {}});
}
void NetworkClient::typing(const string &target, const string &state) {
    impl_->enqueue({"typing", target, state, {}, {}});
}
void NetworkClient::react(const string &target, const string &id, const string &emoji,
                          bool removing) {
    impl_->enqueue({removing ? "unreact" : "react", target, emoji, id, {}});
}
void NetworkClient::redact(const string &target, const string &id) {
    impl_->enqueue({"redact", target, {}, id, {}});
}
vector<Event> NetworkClient::poll() { return impl_->pollEvents(); }
} // namespace blast
