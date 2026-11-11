#include "irc/network.hpp"
#include "irc/protocol.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string_view>
#include <thread>

using namespace blast;
using Clock = std::chrono::steady_clock;

namespace {
string setting(const char *name, const string &fallback = "") {
    const char *value = std::getenv(name);
    return value ? value : fallback;
}
void require(bool condition, const string &why) {
    if (!condition)
        throw std::runtime_error(why);
}
string tag(const Event &event, const string &name) {
    auto it = event.tags.find(name);
    return it == event.tags.end() ? "" : it->second;
}
struct Session {
    NetworkClient client;
    vector<Event> events;
    set<string> caps;
    string deniedTags, messageReferences, nick;
    bool allowFailure = false;
    bool ready = false;

    Session(const ServerConfig &server, const Identity &identity) : client(server, identity) {
        client.start();
    }
    void poll() {
        for (auto &event : client.poll()) {
            if (event.type == "caps")
                caps = set<string>(event.params.begin(), event.params.end());
            if (event.type == "network" && tag(event, "kind") == "isupport") {
                deniedTags = tag(event, "CLIENTTAGDENY");
                messageReferences = tag(event, "MSGREFTYPES");
            }
            if (event.type == "state" && event.text == "Ready") {
                ready = true;
                nick = event.nick;
            }
            if (!allowFailure && event.type == "state" &&
                (event.text == "Failed" || (ready && event.text == "Disconnected")))
                throw std::runtime_error("unexpected connection state " + event.text + ": " +
                                         tag(event, "error"));
            events.push_back(std::move(event));
        }
    }
    bool has(const string &cap) const { return caps.count(cap) != 0; }
    const Event *find(size_t from, const std::function<bool(const Event &)> &predicate) const {
        for (size_t i = from; i < events.size(); ++i)
            if (predicate(events[i]))
                return &events[i];
        return nullptr;
    }
};
void waitUntil(Session &a, Session *b, const std::function<bool()> &predicate,
               const string &description, int seconds = 20) {
    const auto deadline = Clock::now() + std::chrono::seconds(seconds);
    do {
        a.poll();
        if (b)
            b->poll();
        if (predicate())
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (Clock::now() < deadline);
    std::cerr << "Timeout: " << description << '\n';
    for (Session *session : {&a, b})
        if (session) {
            const size_t first = session->events.size() > 12 ? session->events.size() - 12 : 0;
            for (size_t i = first; i < session->events.size(); ++i) {
                const auto &e = session->events[i];
                std::cerr << "  " << e.type << " target=" << e.target << " nick=" << e.nick
                          << " text=" << e.text << " id=" << e.id << " history=" << e.history
                          << " error=" << tag(e, "error") << '\n';
            }
        }
    throw std::runtime_error(description);
}
Event waitEvent(Session &a, Session *b, size_t from,
                const std::function<bool(const Event &)> &predicate, const string &description) {
    waitUntil(a, b, [&] { return a.find(from, predicate) != nullptr; }, description);
    return *a.find(from, predicate);
}
void skip(const string &feature, const string &reason) {
    std::cout << "SKIP " << feature << ": " << reason << std::endl;
}
void pass(const string &feature) { std::cout << "PASS " << feature << std::endl; }
bool liveMessage(const Event &e, const string &text) {
    return e.type == "message" && e.text == text && !e.history && tag(e, "pending") != "1";
}
void join(Session &a, Session *b, const string &channel) {
    // A newly attached device may already have received its live membership snapshot.
    a.poll();
    const auto joined = [&](const Event &e) {
        return e.type == "join" && e.target == channel && e.own && !e.history;
    };
    if (a.find(0, joined))
        return;
    const size_t from = a.events.size();
    a.client.command("JOIN " + channel);
    waitEvent(a, b, from, joined, "own JOIN acknowledgement for " + channel);
}
ServerConfig server(const string &prefix, const string &kind) {
    ServerConfig result;
    result.name = kind;
    result.host = setting((prefix + "HOST").c_str(), "localhost");
    result.port = std::stoi(setting((prefix + "PORT").c_str()));
    result.tls = true;
    result.sasl = setting((prefix + "SASL").c_str(), "PLAIN");
    result.password = setting((prefix + "SERVER_PASSWORD").c_str());
    result.bouncer = kind == "direct" ? "none" : kind;
    return result;
}
Identity identity(const string &prefix) {
    Identity result;
    result.username = setting((prefix + "USERNAME").c_str());
    result.nick = setting((prefix + "NICK").c_str(), result.username);
    result.password = setting((prefix + "PASSWORD").c_str());
    result.realname = "BlastIRC isolated integration check";
    return result;
}
void invalidCertificate(const ServerConfig &good, const Identity &id) {
    ServerConfig bad = good;
    bad.port = std::stoi(setting("BLAST_TEST_INVALID_PORT"));
    Session rejected(bad, id);
    rejected.allowFailure = true;
    const Event error = waitEvent(
        rejected, nullptr, 0,
        [](const Event &e) { return e.type == "state" && e.text == "Failed"; },
        "untrusted TLS certificate must fail");
    require(!rejected.ready, "untrusted TLS endpoint reached Ready");
    string reason = tag(error, "error");
    std::transform(reason.begin(), reason.end(), reason.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    require(reason.find("cert") != string::npos || reason.find("verify") != string::npos,
            "TLS rejection did not identify certificate validation: " + reason);
    bool lateRejected = false;
    try {
        rejected.client.send("#unused", "must not survive terminal TLS failure", {}, "tls-late");
    } catch (const std::runtime_error &) {
        lateRejected = true;
    }
    require(lateRejected, "terminal client silently retained a late send");
    rejected.client.stop();
    pass("untrusted TLS certificate rejected before IRC registration");
}
void stopDuringHandoff(Session &session, const string &channel, const string &token) {
    constexpr size_t parts = 4096;
    if (session.has("labeled-response")) {
        session.client.command("CAP REQ :-labeled-response");
        waitUntil(
            session, nullptr, [&] { return !session.has("labeled-response"); },
            "server acknowledges disabling labeled-response for stop boundary");
    }
    // With no wire labels, output stays below 8 MiB, but these three local
    // label copies exceed two 16 MiB event batches. Draining the first pending
    // batch therefore cannot let this request finish before stop.
    const string submission = token + "-stop-handoff-" + string(3500, 's');
    const string queuedSubmission = token + "-stop-unstarted";
    string text;
    for (size_t i = 0; i < parts; ++i) {
        if (i)
            text += '\n';
        text += "stop-part-" + std::to_string(i);
    }
    const size_t from = session.events.size();
    session.client.command("/me " + channel + " " + text, submission);
    session.client.send(channel, "unstarted after blocked handoff", {}, queuedSubmission);
    waitEvent(
        session, nullptr, from,
        [&](const Event &event) {
            return event.label == submission && tag(event, "pending") == "1";
        },
        "real producer begins the backpressured handoff");
    session.allowFailure = true;
    session.client.stop();
    session.poll();
    set<string> handedOff, failed;
    size_t completed = 0, queuedRejected = 0;
    for (size_t i = from; i < session.events.size(); ++i) {
        const auto &event = session.events[i];
        if (event.label == queuedSubmission) {
            require(event.type == "error" && tag(event, "send-failed") == "1" &&
                        tag(event, "original-text") == "unstarted after blocked handoff",
                    "stop executed the request queued behind a blocked handoff");
            ++queuedRejected;
            continue;
        }
        if (event.label != submission && !event.label.starts_with(submission + ".part"))
            continue;
        if (tag(event, "pending") == "1") {
            const size_t part = handedOff.size();
            require(event.type == "action" && tag(event, "submission") == submission &&
                        event.label ==
                            (part ? submission + ".part" + std::to_string(part) : submission) &&
                        event.text == "stop-part-" + std::to_string(part) &&
                        handedOff.insert(event.label).second,
                    "stop lost, duplicated or reordered a producer handoff part");
            if (tag(event, "submission-complete") == "1") {
                require(handedOff.size() == parts &&
                            tag(event, "submission-parts") == std::to_string(parts),
                        "stopping producer completed an incomplete recovery representation");
                ++completed;
            }
        } else if (tag(event, "send-failed") == "1") {
            const string part =
                event.label == submission ? "0" : event.label.substr(submission.size() + 5);
            require(handedOff.contains(event.label) && failed.insert(event.label).second &&
                        tag(event, "original-text") == "/me stop-part-" + part,
                    "shutdown failed a part without its matching recovery handoff");
        }
    }
    require(handedOff.size() == parts && failed == handedOff && completed == 1 &&
                queuedRejected == 1,
            "producer/stop boundary did not retain exactly one complete handoff");
    pass("real producer stop preserves every handoff before failure and rejects unstarted work");
}

void channelRename(Session &primary, Session &peer, const string &base, const string &token) {
    if (!primary.has("draft/channel-rename") || !peer.has("draft/channel-rename")) {
        skip("channel rename", "draft/channel-rename absent on one consumer");
        return;
    }
    const string oldTarget = base + "-rename", newTarget = oldTarget + "-new";
    join(primary, &peer, oldTarget);
    join(peer, &primary, oldTarget);
    auto rename = [&](const string &from, const string &to, const string &reason) {
        const size_t primaryStart = primary.events.size(), peerStart = peer.events.size();
        primary.client.command("RENAME " + from + " " + to + " :" + reason);
        const auto received = [&](const Event &e) {
            return e.type == "network" && tag(e, "kind") == "rename" && e.target == from &&
                   e.params.size() >= 2 && e.params[1] == to && e.text == reason;
        };
        waitEvent(primary, &peer, primaryStart, received, "rename reaches originating consumer");
        waitEvent(peer, &primary, peerStart, received, "rename reaches other joined consumer");
    };
    rename(oldTarget, newTarget, "");
    string caseTarget = newTarget;
    for (char &c : caseTarget)
        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';
    rename(newTarget, caseTarget, "case-only rename");
    const size_t start = peer.events.size();
    primary.client.send(caseTarget, token + " renamed channel");
    waitEvent(
        peer, &primary, start,
        [&](const Event &e) {
            return liveMessage(e, token + " renamed channel") && e.target == caseTarget;
        },
        "live traffic uses the renamed channel, including case-only identity");
    primary.client.command("PART " + caseTarget);
    peer.client.command("PART " + caseTarget);
    pass("advertised channel rename, empty reason, case-only change and live destination");
}

void sojuCatalogAndBindings(const ServerConfig &saved, const Identity &selected) {
    ServerConfig root = saved;
    root.name = "soju-control";
    root.sojuAccount = true;
    root.bouncer = "soju";
    Identity account = selected;
    account.username.erase(account.username.find_first_of("/@") == string::npos
                               ? account.username.size()
                               : account.username.find_first_of("/@"));
    Session control(root, account);
    waitUntil(control, nullptr, [&] { return control.ready; }, "soju control registers");
    require(control.has("soju.im/bouncer-networks"), "soju fixture lacks native management");
    waitEvent(
        control, nullptr, 0,
        [](const Event &e) { return tag(e, "kind") == "bouncer-list" && tag(e, "phase") == "end"; },
        "soju authoritative catalog completes");
    const Event row = waitEvent(
        control, nullptr, 0,
        [](const Event &e) {
            return tag(e, "kind") == "bouncer" && tag(e, "name") == "local" && !e.id.empty();
        },
        "fixture local network has a stable discovered netid");
    ServerConfig first = saved;
    first.name = "soju-child-one";
    first.sojuAccount = false;
    first.bouncer = "soju:" + row.id;
    ServerConfig second = first;
    second.name = "soju-child-two";
    Session childOne(first, account), childTwo(second, account);
    waitUntil(
        childOne, &childTwo, [&] { return childOne.ready && childTwo.ready; },
        "two independently bound soju children register beside control");
    require(control.ready, "child registration replaced its control connection");
    childOne.client.stop();
    const size_t from = childTwo.events.size();
    childTwo.client.command("WHO " + account.nick);
    waitEvent(
        childTwo, &control, from, [](const Event &e) { return tag(e, "command") == "315"; },
        "stopping one bound child leaves the other physically responsive");
    childTwo.client.stop();
    control.client.stop();
    pass("root catalog and concurrent BIND-based independent child sessions");
}
void run() {
    const string kind = setting("BLAST_TEST_KIND", "direct");
    const string channel = setting("BLAST_TEST_CHANNEL", "#blastirc-" + kind);
    const string token = setting("BLAST_TEST_TOKEN", "blastirc-integration");
    const int idleSeconds = std::stoi(setting("BLAST_TEST_IDLE_SECONDS", "610"));
    require(idleSeconds >= 1, "idle duration must be positive");
    const ServerConfig config = server("BLAST_TEST_", kind);
    const Identity id = identity("BLAST_TEST_");
    const ServerConfig peerConfig = server("BLAST_TEST_PEER_", "direct");
    const Identity peerId = identity("BLAST_TEST_PEER_");
    {
        NetworkClient queued(config, id);
        const string submission = token + "-stopped";
        queued.send(channel, "queued before start", {}, submission);
        queued.command("/me " + channel + " queued action", submission + "-action");
        queued.stop();
        const auto rejected = queued.poll();
        require(rejected.size() == 2 &&
                    std::all_of(rejected.begin(), rejected.end(),
                                [&](const Event &e) {
                                    return e.type == "error" && tag(e, "send-failed") == "1" &&
                                           e.target == channel &&
                                           ((e.label == submission &&
                                             tag(e, "original-text") == "queued before start") ||
                                            (e.label == submission + "-action" &&
                                             tag(e, "original-text") == "/me queued action"));
                                }),
                "stop did not reject unpumped requests with their submission identity");
        bool lateRejected = false;
        try {
            queued.send(channel, "must not survive stop", {}, submission + "-late");
        } catch (const std::runtime_error &) {
            lateRejected = true;
        }
        require(lateRejected, "stopped client silently retained a late send");
    }
    if (kind == "direct")
        invalidCertificate(config, id);
    if (kind == "soju")
        sojuCatalogAndBindings(config, id);

    auto primary = std::make_unique<Session>(config, id);
    auto peer = std::make_unique<Session>(peerConfig, peerId);
    waitUntil(
        *primary, peer.get(), [&] { return primary->ready && peer->ready; },
        "both clients register");
    pass("TLS registration" +
         (config.sasl.empty() ? " using configured PASS" : " with SASL " + config.sasl));
    std::cout << "CAPS " << kind;
    for (const auto &cap : primary->caps)
        std::cout << ' ' << cap;
    std::cout << std::endl;
    join(*primary, peer.get(), channel);
    join(*peer, primary.get(), channel);
    pass("both consumers joined " + channel);

    const string firstText = token + " live";
    const size_t firstStart = primary->events.size();
    const size_t peerStart = peer->events.size();
    const string firstSubmission = token + "-first";
    primary->client.send(channel, firstText, {}, firstSubmission);
    const Event received = waitEvent(
        *peer, primary.get(), peerStart, [&](const Event &e) { return liveMessage(e, firstText); },
        "peer consumes actual live PRIVMSG");
    const Event firstPending = waitEvent(
        *primary, peer.get(), firstStart,
        [&](const Event &e) {
            return e.type == "message" && tag(e, "pending") == "1" && e.label == firstSubmission;
        },
        "caller-owned pending send");
    require(tag(firstPending, "submission") == firstSubmission &&
                tag(firstPending, "submission-complete") == "1" &&
                tag(firstPending, "submission-parts") == "1",
            "single pending ghost did not complete its caller-owned handoff");
    Event echoed;
    if (primary->has("echo-message")) {
        echoed = waitEvent(
            *primary, peer.get(), firstStart,
            [&](const Event &e) { return liveMessage(e, firstText) && e.own; },
            "real own echo (not the pending local event)");
        if (primary->has("labeled-response")) {
            const Event *pending = primary->find(firstStart, [&](const Event &e) {
                return e.type == "message" && e.text == firstText && tag(e, "pending") == "1";
            });
            require(pending && !pending->label.empty() && pending->label == echoed.label,
                    "labeled real echo did not correlate with pending consumer event");
        }
        pass("live server echo and pending correlation");
    } else
        skip("own echo", "echo-message not advertised by " + kind);
    if (peer->has("server-time"))
        require(!received.time.empty() && received.timestamp > 0,
                "live message lost advertised server-time");
    const string messageId = received.id.empty() ? echoed.id : received.id;

    const string actionSubmission = token + "-action";
    const string actionFirst = token + " action-first", actionLast = token + " action-last";
    const size_t actionStart = primary->events.size(), peerActionStart = peer->events.size();
    primary->client.command("/me " + channel + " " + actionFirst + "\n" + actionLast,
                            actionSubmission);
    const Event actionComplete = waitEvent(
        *primary, peer.get(), actionStart,
        [&](const Event &e) {
            return tag(e, "submission") == actionSubmission && tag(e, "submission-complete") == "1";
        },
        "legacy action handoff completes after every pending part");
    const Event *actionPending = primary->find(actionStart, [&](const Event &e) {
        return tag(e, "pending") == "1" && e.label == actionSubmission;
    });
    require(actionPending && actionPending->type == "action" &&
                actionPending->text == actionFirst &&
                tag(*actionPending, "submission") == actionSubmission &&
                tag(*actionPending, "submission-complete").empty() &&
                tag(*actionPending, "submission-parts").empty() &&
                actionComplete.type == "action" && tag(actionComplete, "pending") == "1" &&
                actionComplete.label == actionSubmission + ".part1" &&
                actionComplete.text == actionLast && tag(actionComplete, "submission-parts") == "2",
            "legacy parts lost caller ownership or completed before their last ghost");
    for (const auto &text : {actionFirst, actionLast})
        waitEvent(
            *peer, primary.get(), peerActionStart,
            [&](const Event &e) {
                return e.type == "action" && e.text == text && !e.history &&
                       tag(e, "pending") != "1";
            },
            "actual peer receives submitted action part");
    const string invalidSubmission = token + "-invalid";
    const size_t invalidStart = primary->events.size();
    primary->client.send("invalid target", "recoverable original", {}, invalidSubmission);
    const Event invalid = waitEvent(
        *primary, peer.get(), invalidStart,
        [&](const Event &e) { return e.type == "error" && e.label == invalidSubmission; },
        "pre-ghost validation rejection retains submission identity");
    require(tag(invalid, "send-failed") == "1" &&
                tag(invalid, "original-text") == "recoverable original",
            "pre-ghost rejection lost recoverable send text");
    pass("submission ownership, last-part handoff and pre-ghost rejection");

    if (primary->has("message-tags") && irc::clientTagAllowed(primary->deniedTags, "+typing") &&
        peer->has("message-tags")) {
        for (const string state : {"active", "paused", "done"}) {
            const size_t start = peer->events.size();
            primary->client.typing(channel, state);
            waitEvent(
                *peer, primary.get(), start,
                [&](const Event &e) {
                    return e.type == "typing" && e.target == channel && e.text == state;
                },
                "typing " + state + " delivered to other consumer");
        }
        pass("typing active/paused/done");
    } else
        skip("typing", "message-tags absent or CLIENTTAGDENY disallows typing");

    if (primary->has("message-tags") && irc::clientTagAllowed(primary->deniedTags, "+reply") &&
        peer->has("message-tags")) {
        require(!messageId.empty(), "reply supported but original message has no server msgid");
        const size_t start = peer->events.size();
        primary->client.send(channel, token + " reply", messageId);
        waitEvent(
            *peer, primary.get(), start,
            [&](const Event &e) {
                return liveMessage(e, token + " reply") && e.reply == messageId;
            },
            "reply linkage survives actual server/bouncer");
        pass("reply references original msgid");
    } else
        skip("reply", "message-tags absent or CLIENTTAGDENY disallows reply");

    if (primary->has("message-tags") &&
        irc::clientTagAllowed(primary->deniedTags, "+draft/react") &&
        irc::clientTagAllowed(primary->deniedTags, "+reply") && peer->has("message-tags")) {
        require(!messageId.empty(), "reaction supported but original message has no server msgid");
        const size_t start = peer->events.size();
        primary->client.react(channel, messageId, "👍", false);
        waitEvent(
            *peer, primary.get(), start,
            [&](const Event &e) {
                return e.type == "reaction" && e.target == channel && e.id == messageId &&
                       e.text == "👍";
            },
            "reaction references original consumer-visible msgid");
        pass("reaction event and emoji");
    } else
        skip("reaction", "message-tags absent or CLIENTTAGDENY disallows draft/react or reply");

    if (primary->has("draft/chathistory")) {
        const size_t boundaryStart = peer->events.size();
        primary->client.send(channel, token + " history-boundary");
        const Event boundary = waitEvent(
            *peer, primary.get(), boundaryStart,
            [&](const Event &e) { return liveMessage(e, token + " history-boundary"); },
            "history pagination boundary is acknowledged by server");
        const size_t start = primary->events.size();
        primary->client.history(channel, "", token + "-latest", false);
        const Event latestEnd = waitEvent(
            *primary, peer.get(), start,
            [&](const Event &e) { return e.type == "history_end" && e.target == channel; },
            "CHATHISTORY batch completes");
        require(tag(latestEnd, "error").empty(),
                "CHATHISTORY consumer completion reported an error: " + tag(latestEnd, "error"));
        require(primary->find(start,
                              [&](const Event &e) {
                                  return e.type == "history_begin" && e.target == channel;
                              }),
                "history ended without consumer history_begin");
        require(primary->find(start,
                              [&](const Event &e) {
                                  return e.type == "message" && e.text == firstText && e.history &&
                                         e.timestamp > 0 && !e.time.empty();
                              }),
                "CHATHISTORY did not return original message with historical server-time");
        const string continuation = tag(latestEnd, "cursor");
        require(!continuation.empty(), "CHATHISTORY omitted its consumer continuation cursor");
        if (!primary->messageReferences.empty())
            require(primary->messageReferences.find(
                        continuation.substr(0, continuation.find('='))) != string::npos,
                    "CHATHISTORY continuation cursor uses an unsupported reference type");
        pass("CHATHISTORY brackets, actual message and authoritative historical time");
        const Event *historicalBoundary = primary->find(start, [&](const Event &e) {
            return e.type == "message" && e.text == boundary.text && e.history;
        });
        require(historicalBoundary, "LATEST omitted the acknowledged pagination boundary");
        string cursor;
        if (!historicalBoundary->id.empty() &&
            (primary->messageReferences.empty() ||
             primary->messageReferences.find("msgid") != string::npos))
            cursor = "msgid=" + historicalBoundary->id;
        else {
            require(primary->messageReferences.empty() ||
                        primary->messageReferences.find("timestamp") != string::npos,
                    "CHATHISTORY advertised no usable pagination reference type");
            require(!historicalBoundary->time.empty() && historicalBoundary->timestamp > 0,
                    "CHATHISTORY timestamp cursor lacks authoritative historical server-time");
            cursor = "timestamp=" + historicalBoundary->time;
        }
        const size_t olderStart = primary->events.size();
        primary->client.history(channel, cursor, token + "-older", false);
        const Event olderEnd = waitEvent(
            *primary, peer.get(), olderStart,
            [&](const Event &e) { return e.type == "history_end" && e.target == channel; },
            "CHATHISTORY BEFORE batch completes");
        require(tag(olderEnd, "error").empty(),
                "BEFORE pagination reported a consumer error: " + tag(olderEnd, "error"));
        require(primary->find(olderStart,
                              [&](const Event &e) {
                                  return e.type == "message" && e.text == firstText && e.history;
                              }),
                "BEFORE pagination omitted earlier consumer-visible message");
        require(!primary->find(olderStart,
                               [&](const Event &e) {
                                   return e.type == "message" && e.text == boundary.text &&
                                          e.history;
                               }),
                "BEFORE pagination included its exclusive boundary");
        pass("load-older pagination excludes its advertised-selector boundary");
    } else
        skip("CHATHISTORY", "draft/chathistory not advertised by " + kind);

    if (primary->has("draft/read-marker") || primary->has("soju.im/read")) {
        peer->client.stop();
        // Ergo's multiclient identity is the SASL account plus nickname, not account alone.
        peer = std::make_unique<Session>(config, id);
        waitUntil(
            *primary, peer.get(), [&] { return peer->ready; },
            "second same-account read-marker consumer registers");
        require(peer->nick == primary->nick,
                "read-marker devices did not attach to the same IRC identity");
        join(*peer, primary.get(), channel);
        const size_t markerMessageStart = peer->events.size();
        primary->client.send(channel, token + " read-marker");
        const Event markerMessage = waitEvent(
            *peer, primary.get(), markerMessageStart,
            [&](const Event &e) { return liveMessage(e, token + " read-marker"); },
            "same-account reader receives new message before marking it read");
        require(!markerMessage.time.empty(),
                "read-marker support without authoritative message timestamp");
        const size_t start = peer->events.size();
        const size_t senderStart = primary->events.size();
        primary->client.markRead(channel, markerMessage.time);
        const auto marked = [&](const Event &e) {
            return e.type == "read" && e.target == channel && e.text == markerMessage.time;
        };
        waitEvent(*primary, peer.get(), senderStart, marked,
                  "read marker update acknowledged by sender's server");
        waitEvent(*peer, primary.get(), start, marked,
                  "read marker synchronizes to second same-identity device");
        pass("server-side read marker acknowledged and synchronized across devices");
        peer->client.stop();
        peer = std::make_unique<Session>(peerConfig, peerId);
        waitUntil(*primary, peer.get(), [&] { return peer->ready; }, "independent peer reconnects");
        join(*peer, primary.get(), channel);
    } else
        skip("read-marker sync",
             "neither draft/read-marker nor soju.im/read advertised by " + kind);

    if (primary->has("draft/message-redaction")) {
        require(!messageId.empty(), "redaction advertised but original message has no msgid");
        const size_t start = peer->events.size();
        primary->client.redact(channel, messageId);
        waitEvent(
            *peer, primary.get(), start,
            [&](const Event &e) {
                return e.type == "redact" && e.target == channel && e.id == messageId;
            },
            "redaction reaches the other consumer with original msgid");
        pass("redaction identifies original message");
    } else
        skip("redaction", "draft/message-redaction not advertised by " + kind);
    channelRename(*primary, *peer, channel, token);
    if (kind == "znc") {
        if (!primary->has("znc.in/playback")) {
            skip("native ZNC history requests",
                 "optional external playback module is not advertised");
        } else {
            const size_t start = primary->events.size();
            primary->client.history(channel, "", token + "-znc", false);
            const auto completed = waitEvent(
                *primary, peer.get(), start,
                [&](const Event &e) { return e.type == "history_end" && e.target == channel; },
                "advertised native ZNC playback completes");
            require(tag(completed, "error").empty(), "advertised ZNC playback failed");
            require(primary->find(start,
                                  [&](const Event &e) {
                                      return e.type == "message" && e.history &&
                                             e.text == firstText;
                                  }),
                    "native ZNC request did not replay actual previously sent channel text");
            pass("advertised native ZNC history requests");
        }
    }

    if (kind == "soju" || kind == "znc") {
        primary->client.stop();
        const string offlineText = token + " offline-buffered";
        peer->client.send(channel, offlineText);
        // An echoed peer message is the server's acknowledgement, not a timed sleep.
        require(peer->has("echo-message"),
                "direct Ergo peer must advertise echo-message for offline acknowledgement");
        waitEvent(
            *peer, nullptr, 0, [&](const Event &e) { return liveMessage(e, offlineText); },
            "server acknowledged message while downstream consumer disconnected");
        primary = std::make_unique<Session>(config, id);
        waitUntil(
            *primary, peer.get(), [&] { return primary->ready; }, "bouncer downstream reconnects");
        primary->client.command("JOIN " + channel);
        if (primary->has("draft/chathistory"))
            primary->client.history(channel, "", token + "-reconnect", true);
        waitEvent(
            *primary, peer.get(), 0,
            [&](const Event &e) {
                return e.type == "message" && e.text == offlineText && e.history &&
                       !e.time.empty() && e.timestamp > 0;
            },
            "genuine bouncer offline playback is classified as history with server-time");
        pass(kind + " offline playback delivered as consumer history");
    }

    const size_t idleStart = primary->events.size();
    const auto idleUntil = Clock::now() + std::chrono::seconds(idleSeconds);
    while (Clock::now() < idleUntil) {
        primary->poll();
        peer->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (idleSeconds >= 90) {
        require(primary->find(idleStart,
                              [](const Event &e) {
                                  return e.type == "network" && tag(e, "kind") == "latency";
                              }),
                "idle window did not produce an acknowledged keepalive PING/PONG");
    }
    const size_t afterIdle = peer->events.size();
    primary->client.send(channel, token + " after-idle");
    waitEvent(
        *peer, primary.get(), afterIdle,
        [&](const Event &e) { return liveMessage(e, token + " after-idle"); },
        "live traffic remains deliverable after idle PING cycles");
    pass("idle PING survival and live delivery after " + std::to_string(idleSeconds) + " seconds");
    stopDuringHandoff(*primary, channel, token);
    peer->client.stop();
}
} // namespace

int main() {
    try {
        run();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL " << setting("BLAST_TEST_KIND") << ": " << e.what() << '\n';
        return 1;
    }
}
